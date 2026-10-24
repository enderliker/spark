#include "vm.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ast.h"
#include "buf.h"
#include "chunk.h"
#include "compiler.h"
#include "memory.h"
#include "object.h"
#include "parser.h"
#include "table.h"

#define STACK_MAX 131072
#define FRAMES_MAX 256

typedef struct {
    ObjClosure *closure;
    uint8_t *ip;
    Value *slots;
} CallFrame;

typedef struct {
    ObjString *name;
    Value value;
    bool defined;
    bool is_const;
} GlobalSlot;

typedef struct {
    CallFrame frames[FRAMES_MAX];
    int frame_count;
    Value stack[STACK_MAX];
    Value *stack_top;
    Table global_names;
    GlobalSlot *global_slots;
    int global_count;
    int global_capacity;
    ObjUpvalue *open_upvalues;
    bool had_error;
} VM;

static VM vm;
static bool vm_ready = false;

typedef struct {
    Obj **items;
    int count;
    int capacity;
} GrayStack;

static GrayStack gray;

static void mark_object(Obj *object);
static void mark_value(Value value);
static void collect_garbage(void);
static void define_native(const char *name, NativeFn function, int arity);
static void close_upvalues(Value *last);
static InterpretResult run(int base_frame);
static bool call_callback(Value callee, int arg_count, Value *result);
static inline bool compare_values(bool greater, Value a, Value b, bool *out);

static void push(Value value)
{
    *vm.stack_top++ = value;
}

static Value pop(void)
{
    return *--vm.stack_top;
}

static Value peek(int distance)
{
    return vm.stack_top[-1 - distance];
}

static const char *value_type_name(Value value)
{
    switch (value.type) {
    case VAL_NIL: return "nil";
    case VAL_BOOL: return "bool";
    case VAL_NUMBER: return "number";
    case VAL_OBJ: return obj_type_name(AS_OBJ(value)->type);
    }
    return "unknown";
}

/* ------------------------------------------------------------------ */
/* Mark & sweep garbage collector                                      */
/* ------------------------------------------------------------------ */

static void gray_push(Obj *object)
{
    if (!object) {
        return;
    }
    if (gray.count == gray.capacity) {
        gray.capacity = gray.capacity < 64 ? 64 : gray.capacity * 2;
        gray.items = realloc(gray.items, sizeof(Obj *) * (size_t)gray.capacity);
        if (!gray.items) {
            fprintf(stderr, "spark: out of memory in the GC\n");
            exit(70);
        }
    }
    gray.items[gray.count++] = object;
}

static void mark_object(Obj *object)
{
    if (!object || object->marked) {
        return;
    }
    object->marked = true;
    gray_push(object);
}

static void mark_value(Value value)
{
    if (is_obj(value)) {
        mark_object(AS_OBJ(value));
    }
}

static void blacken(Obj *object)
{
    switch (object->type) {
    case OBJ_STRING:
        break;
    case OBJ_ARRAY: {
        ObjArray *array = (ObjArray *)object;

        for (int i = 0; i < array->count; i++) {
            mark_value(array->items[i]);
        }
        break;
    }
    case OBJ_MAP: {
        ObjMap *map = (ObjMap *)object;

        for (int i = 0; i < map->capacity; i++) {
            MapEntry *entry = &map->entries[i];

            if (entry->state == 1) {
                mark_value(entry->key);
                mark_value(entry->value);
            }
        }
        break;
    }
    case OBJ_FUNCTION: {
        ObjFunction *function = (ObjFunction *)object;

        mark_object((Obj *)function->name);
        for (int i = 0; i < function->chunk.constants_count; i++) {
            mark_value(function->chunk.constants[i]);
        }
        break;
    }
    case OBJ_CLOSURE: {
        ObjClosure *closure = (ObjClosure *)object;

        mark_object((Obj *)closure->function);
        for (int i = 0; i < closure->upvalue_count; i++) {
            mark_object((Obj *)closure->upvalues[i]);
        }
        break;
    }
    case OBJ_NATIVE:
        mark_object((Obj *)((ObjNative *)object)->name);
        break;
    case OBJ_UPVALUE:
        mark_value(((ObjUpvalue *)object)->closed);
        break;
    }
}

static void trace_references(void)
{
    while (gray.count > 0) {
        Obj *object = gray.items[--gray.count];

        blacken(object);
    }
}

static void mark_roots(void)
{
    for (Value *slot = vm.stack; slot < vm.stack_top; slot++) {
        mark_value(*slot);
    }
    for (int i = 0; i < vm.frame_count; i++) {
        mark_object((Obj *)vm.frames[i].closure);
    }
    for (int i = 0; i < vm.global_count; i++) {
        mark_object((Obj *)vm.global_slots[i].name);
        if (vm.global_slots[i].defined) {
            mark_value(vm.global_slots[i].value);
        }
    }
    for (int i = 0; i < vm.global_names.capacity; i++) {
        TableEntry *entry = &vm.global_names.entries[i];

        mark_object((Obj *)entry->key);
    }
    for (ObjUpvalue *upvalue = vm.open_upvalues; upvalue; upvalue = upvalue->next) {
        mark_object((Obj *)upvalue);
    }
}

static void sweep(void)
{
    Obj *previous = NULL;
    Obj *object = objects_head();

    while (object) {
        if (object->marked) {
            object->marked = false;
            previous = object;
            object = object->next;
        } else {
            Obj *unreached = object;

            object = object->next;
            if (previous) {
                previous->next = object;
            } else {
                objects_set_head(object);
            }
            object_free(unreached);
        }
    }
}

static void collect_garbage(void)
{
    if (mem_gc_paused()) {
        return;
    }
    mark_roots();
    trace_references();
    intern_remove_unmarked();
    sweep();

    size_t threshold = mem_allocated() * 2;

    if (threshold < 1024u * 1024u) {
        threshold = 1024u * 1024u;
    }
    mem_set_threshold(threshold);
}

/* ------------------------------------------------------------------ */
/* Natives                                                             */
/* ------------------------------------------------------------------ */

static int utf8_length(const char *chars, int length)
{
    int count = 0;

    for (int i = 0; i < length; i++) {
        if (((unsigned char)chars[i] & 0xc0) != 0x80) {
            count++;
        }
    }
    return count;
}

/* Byte offset of UTF-8 character number `char_index`, or -1. */
static int utf8_byte_offset(const ObjString *string, int char_index)
{
    int count = 0;

    if (char_index < 0) {
        return -1;
    }
    for (int i = 0; i < string->length; i++) {
        if (((unsigned char)string->chars[i] & 0xc0) != 0x80) {
            if (count == char_index) {
                return i;
            }
            count++;
        }
    }
    return -1;
}

static int utf8_char_length(const ObjString *string, int byte_index)
{
    if (byte_index < 0 || byte_index >= string->length) {
        return 0;
    }
    int end = byte_index + 1;

    while (end < string->length && ((unsigned char)string->chars[end] & 0xc0) == 0x80) {
        end++;
    }
    return end - byte_index;
}

static Value native_print(int arg_count, Value *args)
{
    for (int i = 0; i < arg_count; i++) {
        if (i > 0) {
            fputc(' ', stdout);
        }
        value_print(stdout, args[i]);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return NIL_VAL;
}

static Value native_len(int arg_count, Value *args)
{
    (void)arg_count;
    Value value = args[0];

    if (is_obj_type(value, OBJ_STRING)) {
        ObjString *string = AS_STRING(value);

        return NUMBER_VAL((double)utf8_length(string->chars, string->length));
    }
    if (is_obj_type(value, OBJ_ARRAY)) {
        return NUMBER_VAL((double)AS_ARRAY(value)->count);
    }
    if (is_obj_type(value, OBJ_MAP)) {
        return NUMBER_VAL((double)AS_MAP(value)->count);
    }
    vm_runtime_error("'len' expects a string, an array or a map, received %s",
                     value_type_name(value));
    return NIL_VAL;
}

static Value native_push(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'push' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = (ObjArray *)AS_OBJ(args[0]);

    array_push(array, args[1]);
    return args[0];
}

static Value native_pop(int arg_count, Value *args)
{
    Value popped;

    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'pop' expects an array, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = (ObjArray *)AS_OBJ(args[0]);

    if (!array_pop(array, &popped)) {
        vm_runtime_error("'pop' cannot be applied to an empty array");
        return NIL_VAL;
    }
    return popped;
}

static Value native_str(int arg_count, Value *args)
{
    (void)arg_count;
    return OBJ_VAL(value_to_string(args[0]));
}

static Value native_num(int arg_count, Value *args)
{
    (void)arg_count;
    if (is_number(args[0])) {
        return args[0];
    }
    if (is_obj_type(args[0], OBJ_STRING)) {
        ObjString *string = AS_STRING(args[0]);
        char *end = NULL;
        double result = strtod(string->chars, &end);

        if (end == string->chars) {
            vm_runtime_error("'num' could not convert \"%s\" to a number", string->chars);
            return NIL_VAL;
        }
        while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
            end++;
        }
        if (*end != '\0') {
            vm_runtime_error("'num' could not convert \"%s\" to a number", string->chars);
            return NIL_VAL;
        }
        return NUMBER_VAL(result);
    }
    vm_runtime_error("'num' expects a number or a string, received %s", value_type_name(args[0]));
    return NIL_VAL;
}

static Value native_type(int arg_count, Value *args)
{
    const char *name;

    (void)arg_count;
    if (is_obj(args[0]) &&
        (AS_OBJ(args[0])->type == OBJ_CLOSURE || AS_OBJ(args[0])->type == OBJ_FUNCTION ||
         AS_OBJ(args[0])->type == OBJ_NATIVE)) {
        name = "function";
    } else {
        name = value_type_name(args[0]);
    }
    return OBJ_VAL(object_string(name, (int)strlen(name)));
}

static Value native_clock(int arg_count, Value *args)
{
    struct timespec ts;

    (void)arg_count;
    (void)args;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return NUMBER_VAL((double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0);
}

static Value native_assert(int arg_count, Value *args)
{
    if (arg_count < 1 || arg_count > 2) {
        vm_runtime_error("'assert' expects 1 or 2 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (value_is_falsey(args[0])) {
        if (arg_count == 2) {
            ObjString *message = is_obj_type(args[1], OBJ_STRING) ? AS_STRING(args[1])
                                                                 : value_to_string(args[1]);

            vm_runtime_error("%s", message->chars);
        } else {
            vm_runtime_error("assert failed");
        }
    }
    return NIL_VAL;
}

static Value native_has(int arg_count, Value *args)
{
    Value ignored;

    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_MAP)) {
        vm_runtime_error("'has' expects a map as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    return BOOL_VAL(map_get(AS_MAP(args[0]), args[1], &ignored));
}

static Value native_del(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_MAP)) {
        vm_runtime_error("'del' expects a map as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    return BOOL_VAL(map_delete(AS_MAP(args[0]), args[1]));
}

static Value native_keys(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_MAP)) {
        vm_runtime_error("'keys' expects a map, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(args[0]);
    ObjArray *keys = object_array();

    push(OBJ_VAL(keys));
    for (int i = 0; i < map->capacity; i++) {
        if (map->entries[i].state == 1) {
            array_push(keys, map->entries[i].key);
        }
    }
    pop();
    return OBJ_VAL(keys);
}

static Value native_values(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_MAP)) {
        vm_runtime_error("'values' expects a map, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(args[0]);
    ObjArray *values = object_array();

    push(OBJ_VAL(values));
    for (int i = 0; i < map->capacity; i++) {
        if (map->entries[i].state == 1) {
            array_push(values, map->entries[i].value);
        }
    }
    pop();
    return OBJ_VAL(values);
}

static bool integer_argument(Value value, const char *name, const char *function, int *out)
{
    if (!is_number(value)) {
        vm_runtime_error("'%s' expects an integer as %s, received %s", function, name,
                         value_type_name(value));
        return false;
    }
    double raw = AS_NUMBER(value);

    if (raw != floor(raw) || raw < -2147483648.0 || raw > 2147483647.0) {
        vm_runtime_error("'%s' expects an integer as %s, received %g", function, name, raw);
        return false;
    }
    *out = (int)raw;
    return true;
}

static Value native_range(int arg_count, Value *args)
{
    int start = 0;
    int end;

    if (arg_count == 1) {
        if (!integer_argument(args[0], "end", "range", &end)) {
            return NIL_VAL;
        }
    } else if (arg_count == 2) {
        if (!integer_argument(args[0], "start", "range", &start) ||
            !integer_argument(args[1], "end", "range", &end)) {
            return NIL_VAL;
        }
    } else {
        vm_runtime_error("'range' expects 1 or 2 arguments, received %d", arg_count);
        return NIL_VAL;
    }

    if (end < start) {
        end = start;
    }
    if ((long long)end - start > 10000000LL) {
        vm_runtime_error("'range' cannot generate more than 10 million elements");
        return NIL_VAL;
    }

    ObjArray *array = object_array();

    push(OBJ_VAL(array));
    for (int i = start; i < end; i++) {
        array_push(array, NUMBER_VAL((double)i));
    }
    pop();
    return OBJ_VAL(array);
}

static Value native_substr(int arg_count, Value *args)
{
    if (arg_count < 2 || arg_count > 3) {
        vm_runtime_error("'substr' expects 2 or 3 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error("'substr' expects a string as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    int start;
    int count;
    int total = utf8_length(AS_STRING(args[0])->chars, AS_STRING(args[0])->length);

    if (!integer_argument(args[1], "start", "substr", &start)) {
        return NIL_VAL;
    }
    if (start < 0) {
        vm_runtime_error("'substr' does not accept a negative start (%d)", start);
        return NIL_VAL;
    }
    if (arg_count == 3) {
        if (!integer_argument(args[2], "count", "substr", &count)) {
            return NIL_VAL;
        }
        if (count < 0) {
            vm_runtime_error("'substr' does not accept a negative count (%d)", count);
            return NIL_VAL;
        }
    } else {
        count = total - start;
    }

    ObjString *string = AS_STRING(args[0]);

    if (start >= total) {
        return OBJ_VAL(object_string("", 0));
    }
    if (count > total - start) {
        count = total - start;
    }

    int byte_start = utf8_byte_offset(string, start);
    int byte_end = byte_start;

    for (int i = 0; i < count; i++) {
        byte_end += utf8_char_length(string, byte_end);
    }
    return OBJ_VAL(object_string(string->chars + byte_start, byte_end - byte_start));
}

static Value numeric_unary(int arg_count, Value *args, const char *name, double (*fn)(double))
{
    (void)arg_count;
    if (!is_number(args[0])) {
        vm_runtime_error("'%s' expects a number, received %s", name, value_type_name(args[0]));
        return NIL_VAL;
    }
    return NUMBER_VAL(fn(AS_NUMBER(args[0])));
}

static Value native_abs(int arg_count, Value *args)
{
    return numeric_unary(arg_count, args, "abs", fabs);
}

static Value native_floor(int arg_count, Value *args)
{
    return numeric_unary(arg_count, args, "floor", floor);
}

static Value native_ceil(int arg_count, Value *args)
{
    return numeric_unary(arg_count, args, "ceil", ceil);
}

static Value native_round(int arg_count, Value *args)
{
    return numeric_unary(arg_count, args, "round", round);
}

static Value native_sqrt(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_number(args[0])) {
        vm_runtime_error("'sqrt' expects a number, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    if (AS_NUMBER(args[0]) < 0) {
        vm_runtime_error("'sqrt' does not accept negative numbers");
        return NIL_VAL;
    }
    return NUMBER_VAL(sqrt(AS_NUMBER(args[0])));
}

static Value native_pow(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_number(args[0]) || !is_number(args[1])) {
        vm_runtime_error("'pow' expects two numbers");
        return NIL_VAL;
    }
    return NUMBER_VAL(pow(AS_NUMBER(args[0]), AS_NUMBER(args[1])));
}

static Value native_min(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_number(args[0]) || !is_number(args[1])) {
        vm_runtime_error("'min' expects two numbers");
        return NIL_VAL;
    }
    return AS_NUMBER(args[0]) <= AS_NUMBER(args[1]) ? args[0] : args[1];
}

static Value native_max(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_number(args[0]) || !is_number(args[1])) {
        vm_runtime_error("'max' expects two numbers");
        return NIL_VAL;
    }
    return AS_NUMBER(args[0]) >= AS_NUMBER(args[1]) ? args[0] : args[1];
}

/* ------------------------------------------------------------------ */
/* Collections and text natives                                        */
/* ------------------------------------------------------------------ */

/* First byte offset of `needle` inside `haystack`, or -1. */
static int string_byte_find(ObjString *haystack, ObjString *needle)
{
    if (needle->length == 0) {
        return 0;
    }
    for (int i = 0; i + needle->length <= haystack->length; i++) {
        if (memcmp(haystack->chars + i, needle->chars, (size_t)needle->length) == 0) {
            return i;
        }
    }
    return -1;
}

/* Creates a string and appends it to `array`. The new string is rooted while
 * the array grows, since that growth can trigger a collection. */
static void array_push_new_string(ObjArray *array, const char *chars, int length)
{
    ObjString *string = object_string(chars, length);

    push(OBJ_VAL(string));
    array_push(array, OBJ_VAL(string));
    pop();
}

static Value native_contains(int arg_count, Value *args)
{
    (void)arg_count;
    Value container = args[0];
    Value needle = args[1];

    if (is_obj_type(container, OBJ_ARRAY)) {
        ObjArray *array = AS_ARRAY(container);

        for (int i = 0; i < array->count; i++) {
            if (values_equal(array->items[i], needle)) {
                return BOOL_VAL(true);
            }
        }
        return BOOL_VAL(false);
    }
    if (is_obj_type(container, OBJ_STRING)) {
        if (!is_obj_type(needle, OBJ_STRING)) {
            vm_runtime_error("'contains' expects a string as the value to look up when the "
                             "container is a string, received %s", value_type_name(needle));
            return NIL_VAL;
        }
        return BOOL_VAL(string_byte_find(AS_STRING(container), AS_STRING(needle)) >= 0);
    }
    if (is_obj_type(container, OBJ_MAP)) {
        Value ignored;

        return BOOL_VAL(map_get(AS_MAP(container), needle, &ignored));
    }
    vm_runtime_error("'contains' expects an array, a string or a map as first argument, "
                     "received %s", value_type_name(container));
    return NIL_VAL;
}

static Value native_index_of(int arg_count, Value *args)
{
    (void)arg_count;
    Value container = args[0];
    Value needle = args[1];

    if (is_obj_type(container, OBJ_ARRAY)) {
        ObjArray *array = AS_ARRAY(container);

        for (int i = 0; i < array->count; i++) {
            if (values_equal(array->items[i], needle)) {
                return NUMBER_VAL((double)i);
            }
        }
        return NUMBER_VAL(-1);
    }
    if (is_obj_type(container, OBJ_STRING)) {
        if (!is_obj_type(needle, OBJ_STRING)) {
            vm_runtime_error("'index_of' expects a string as the value to look up when the "
                             "container is a string, received %s", value_type_name(needle));
            return NIL_VAL;
        }
        ObjString *haystack = AS_STRING(container);
        int byte_index = string_byte_find(haystack, AS_STRING(needle));

        if (byte_index < 0) {
            return NUMBER_VAL(-1);
        }
        return NUMBER_VAL((double)utf8_length(haystack->chars, byte_index));
    }
    vm_runtime_error("'index_of' expects an array or a string as first argument, "
                     "received %s", value_type_name(container));
    return NIL_VAL;
}

static Value native_insert(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'insert' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = AS_ARRAY(args[0]);
    int index;

    if (!integer_argument(args[1], "index", "insert", &index)) {
        return NIL_VAL;
    }
    if (index < 0 || index > array->count) {
        vm_runtime_error("'insert' index out of range: %d (size %d)", index, array->count);
        return NIL_VAL;
    }
    array_push(array, NIL_VAL);
    memmove(array->items + index + 1, array->items + index,
            sizeof(Value) * (size_t)(array->count - index - 1));
    array->items[index] = args[2];
    return args[0];
}

static Value native_remove(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'remove' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = AS_ARRAY(args[0]);
    int index;

    if (!integer_argument(args[1], "index", "remove", &index)) {
        return NIL_VAL;
    }
    if (index < 0 || index >= array->count) {
        vm_runtime_error("'remove' index out of range: %d (size %d)", index, array->count);
        return NIL_VAL;
    }
    Value removed = array->items[index];

    memmove(array->items + index, array->items + index + 1,
            sizeof(Value) * (size_t)(array->count - index - 1));
    array->count--;
    return removed;
}

static Value native_reverse(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'reverse' expects an array, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = AS_ARRAY(args[0]);

    for (int i = 0, j = array->count - 1; i < j; i++, j--) {
        Value swap = array->items[i];

        array->items[i] = array->items[j];
        array->items[j] = swap;
    }
    return args[0];
}

static Value native_fill(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'fill' expects an array, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = AS_ARRAY(args[0]);

    for (int i = 0; i < array->count; i++) {
        array->items[i] = args[1];
    }
    return args[0];
}

static Value native_slice(int arg_count, Value *args)
{
    if (arg_count < 2 || arg_count > 3) {
        vm_runtime_error("'slice' expects 2 or 3 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'slice' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = AS_ARRAY(args[0]);
    int start;
    int end;

    if (!integer_argument(args[1], "start", "slice", &start)) {
        return NIL_VAL;
    }
    if (arg_count == 3) {
        if (!integer_argument(args[2], "end", "slice", &end)) {
            return NIL_VAL;
        }
    } else {
        end = array->count;
    }
    if (start < 0 || end < 0) {
        vm_runtime_error("'slice' does not accept negative indexes (start %d, end %d)",
                         start, end);
        return NIL_VAL;
    }
    if (start > array->count) {
        start = array->count;
    }
    if (end > array->count) {
        end = array->count;
    }
    if (end < start) {
        end = start;
    }

    ObjArray *result = object_array();

    push(OBJ_VAL(result));
    for (int i = start; i < end; i++) {
        array_push(result, array->items[i]);
    }
    pop();
    return OBJ_VAL(result);
}

static Value native_starts_with(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING) || !is_obj_type(args[1], OBJ_STRING)) {
        vm_runtime_error("'starts_with' expects two strings");
        return NIL_VAL;
    }
    ObjString *text = AS_STRING(args[0]);
    ObjString *prefix = AS_STRING(args[1]);

    if (prefix->length > text->length) {
        return BOOL_VAL(false);
    }
    return BOOL_VAL(memcmp(text->chars, prefix->chars, (size_t)prefix->length) == 0);
}

static Value native_ends_with(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING) || !is_obj_type(args[1], OBJ_STRING)) {
        vm_runtime_error("'ends_with' expects two strings");
        return NIL_VAL;
    }
    ObjString *text = AS_STRING(args[0]);
    ObjString *suffix = AS_STRING(args[1]);

    if (suffix->length > text->length) {
        return BOOL_VAL(false);
    }
    return BOOL_VAL(memcmp(text->chars + text->length - suffix->length, suffix->chars,
                           (size_t)suffix->length) == 0);
}

static Value native_split(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING) || !is_obj_type(args[1], OBJ_STRING)) {
        vm_runtime_error("'split' expects two strings");
        return NIL_VAL;
    }
    ObjString *text = AS_STRING(args[0]);
    ObjString *separator = AS_STRING(args[1]);
    ObjArray *parts = object_array();

    push(OBJ_VAL(parts));
    if (separator->length == 0) {
        int byte_index = 0;

        while (byte_index < text->length) {
            int char_length = utf8_char_length(text, byte_index);

            array_push_new_string(parts, text->chars + byte_index, char_length);
            byte_index += char_length;
        }
    } else {
        int start = 0;
        int i = 0;

        while (i + separator->length <= text->length) {
            if (memcmp(text->chars + i, separator->chars, (size_t)separator->length) == 0) {
                array_push_new_string(parts, text->chars + start, i - start);
                i += separator->length;
                start = i;
            } else {
                i++;
            }
        }
        array_push_new_string(parts, text->chars + start, text->length - start);
    }
    pop();
    return OBJ_VAL(parts);
}

static bool buf_append_checked(buf_t *buffer, const void *data, size_t length,
                               const char *function)
{
    if (buf_append(buffer, data, length) != 0) {
        vm_runtime_error("could not allocate memory for '%s'", function);
        return false;
    }
    return true;
}

static Value native_join(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'join' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    if (!is_obj_type(args[1], OBJ_STRING)) {
        vm_runtime_error("'join' expects a string as separator, received %s",
                         value_type_name(args[1]));
        return NIL_VAL;
    }
    ObjArray *array = AS_ARRAY(args[0]);
    ObjString *separator = AS_STRING(args[1]);
    buf_t buffer;

    buf_init(&buffer);
    for (int i = 0; i < array->count; i++) {
        if (i > 0 && !buf_append_checked(&buffer, separator->chars,
                                         (size_t)separator->length, "join")) {
            buf_free(&buffer);
            return NIL_VAL;
        }
        ObjString *piece = value_to_string(array->items[i]);

        if (!buf_append_checked(&buffer, piece->chars, (size_t)piece->length, "join")) {
            buf_free(&buffer);
            return NIL_VAL;
        }
        if (buffer.len > (size_t)INT_MAX) {
            buf_free(&buffer);
            vm_runtime_error("'join' would create a string that is too long");
            return NIL_VAL;
        }
    }
    ObjString *result = object_string(buffer.data ? buffer.data : "", (int)buffer.len);

    buf_free(&buffer);
    return OBJ_VAL(result);
}

static bool is_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static Value native_trim(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error("'trim' expects a string, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjString *string = AS_STRING(args[0]);
    int start = 0;
    int end = string->length;

    while (start < end && is_whitespace(string->chars[start])) {
        start++;
    }
    while (end > start && is_whitespace(string->chars[end - 1])) {
        end--;
    }
    return OBJ_VAL(object_string(string->chars + start, end - start));
}

static Value string_case_convert(Value value, bool to_upper)
{
    const char *name = to_upper ? "upper" : "lower";

    if (!is_obj_type(value, OBJ_STRING)) {
        vm_runtime_error("'%s' expects a string, received %s", name, value_type_name(value));
        return NIL_VAL;
    }
    ObjString *string = AS_STRING(value);
    buf_t buffer;

    buf_init(&buffer);
    for (int i = 0; i < string->length; i++) {
        char c = string->chars[i];

        if (to_upper && c >= 'a' && c <= 'z') {
            c = (char)(c - ('a' - 'A'));
        } else if (!to_upper && c >= 'A' && c <= 'Z') {
            c = (char)(c + ('a' - 'A'));
        }
        if (!buf_append_checked(&buffer, &c, 1, name)) {
            buf_free(&buffer);
            return NIL_VAL;
        }
    }
    ObjString *result = object_string(buffer.data ? buffer.data : "", (int)buffer.len);

    buf_free(&buffer);
    return OBJ_VAL(result);
}

static Value native_upper(int arg_count, Value *args)
{
    (void)arg_count;
    return string_case_convert(args[0], true);
}

static Value native_lower(int arg_count, Value *args)
{
    (void)arg_count;
    return string_case_convert(args[0], false);
}

static Value native_repeat(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error("'repeat' expects a string, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjString *string = AS_STRING(args[0]);
    int times;

    if (!integer_argument(args[1], "count", "repeat", &times)) {
        return NIL_VAL;
    }
    if (times < 0) {
        vm_runtime_error("'repeat' does not accept a negative count (%d)", times);
        return NIL_VAL;
    }
    if (string->length > 0 && times > INT_MAX / string->length) {
        vm_runtime_error("'repeat' would create a string that is too long");
        return NIL_VAL;
    }
    int length = string->length * times;

    if (length == 0) {
        return OBJ_VAL(object_string("", 0));
    }
    char *buffer = malloc((size_t)length + 1);

    if (!buffer) {
        vm_runtime_error("could not allocate memory for 'repeat'");
        return NIL_VAL;
    }
    for (int i = 0; i < times; i++) {
        memcpy(buffer + (size_t)i * (size_t)string->length, string->chars,
               (size_t)string->length);
    }
    buffer[length] = '\0';
    ObjString *result = object_string(buffer, length);

    free(buffer);
    return OBJ_VAL(result);
}

static Value native_replace(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING) || !is_obj_type(args[1], OBJ_STRING) ||
        !is_obj_type(args[2], OBJ_STRING)) {
        vm_runtime_error("'replace' expects three strings");
        return NIL_VAL;
    }
    ObjString *text = AS_STRING(args[0]);
    ObjString *old = AS_STRING(args[1]);
    ObjString *replacement = AS_STRING(args[2]);

    if (old->length == 0) {
        return args[0];
    }

    buf_t buffer;
    int i = 0;

    buf_init(&buffer);
    while (i < text->length) {
        if (i + old->length <= text->length &&
            memcmp(text->chars + i, old->chars, (size_t)old->length) == 0) {
            if (!buf_append_checked(&buffer, replacement->chars,
                                    (size_t)replacement->length, "replace")) {
                buf_free(&buffer);
                return NIL_VAL;
            }
            i += old->length;
        } else {
            if (!buf_append_checked(&buffer, text->chars + i, 1, "replace")) {
                buf_free(&buffer);
                return NIL_VAL;
            }
            i++;
        }
        if (buffer.len > (size_t)INT_MAX) {
            buf_free(&buffer);
            vm_runtime_error("'replace' would create a string that is too long");
            return NIL_VAL;
        }
    }
    ObjString *result = object_string(buffer.data ? buffer.data : "", (int)buffer.len);

    buf_free(&buffer);
    return OBJ_VAL(result);
}

static Value native_clear(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_MAP)) {
        vm_runtime_error("'clear' expects a map, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjMap *map = AS_MAP(args[0]);

    mem_realloc(map->entries, sizeof(MapEntry) * (size_t)map->capacity, 0);
    map->entries = NULL;
    map->count = 0;
    map->tombstones = 0;
    map->capacity = 0;
    return args[0];
}

static Value native_merge(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_MAP) || !is_obj_type(args[1], OBJ_MAP)) {
        vm_runtime_error("'merge' expects two maps");
        return NIL_VAL;
    }
    ObjMap *destination = AS_MAP(args[0]);
    ObjMap *source = AS_MAP(args[1]);

    if (destination == source) {
        return args[0];
    }
    for (int i = 0; i < source->capacity; i++) {
        MapEntry *entry = &source->entries[i];

        if (entry->state == 1) {
            map_set(destination, entry->key, entry->value);
        }
    }
    return args[0];
}

static Value native_get(int arg_count, Value *args)
{
    if (arg_count < 2 || arg_count > 3) {
        vm_runtime_error("'get' expects 2 or 3 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_MAP)) {
        vm_runtime_error("'get' expects a map as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    Value stored;

    if (map_get(AS_MAP(args[0]), args[1], &stored)) {
        return stored;
    }
    return arg_count == 3 ? args[2] : NIL_VAL;
}

/* ------------------------------------------------------------------ */
/* Higher-order natives                                                */
/* ------------------------------------------------------------------ */

static Value native_map(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'map' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *input = AS_ARRAY(args[0]);
    Value callback = args[1];
    ObjArray *output = object_array();

    push(OBJ_VAL(output));
    for (int i = 0; i < input->count; i++) {
        Value mapped;

        push(input->items[i]);
        if (!call_callback(callback, 1, &mapped) || vm.had_error) {
            return NIL_VAL;
        }
        /* Root the result while the output may grow and collect. */
        push(mapped);
        array_push(output, mapped);
        pop();
    }
    pop();
    return OBJ_VAL(output);
}

static Value native_filter(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'filter' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *input = AS_ARRAY(args[0]);
    Value callback = args[1];
    ObjArray *output = object_array();

    push(OBJ_VAL(output));
    for (int i = 0; i < input->count; i++) {
        Value keep;

        push(input->items[i]);
        if (!call_callback(callback, 1, &keep) || vm.had_error) {
            return NIL_VAL;
        }
        if (!value_is_falsey(keep)) {
            array_push(output, input->items[i]);
        }
    }
    pop();
    return OBJ_VAL(output);
}

static Value native_each(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'each' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *input = AS_ARRAY(args[0]);
    Value callback = args[1];

    for (int i = 0; i < input->count; i++) {
        Value ignored;

        push(input->items[i]);
        if (!call_callback(callback, 1, &ignored) || vm.had_error) {
            return NIL_VAL;
        }
    }
    return NIL_VAL;
}

static Value native_reduce(int arg_count, Value *args)
{
    if (arg_count < 2 || arg_count > 3) {
        vm_runtime_error("'reduce' expects 2 or 3 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'reduce' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *input = AS_ARRAY(args[0]);
    Value callback = args[1];
    Value accumulator;
    int start;

    if (arg_count == 3) {
        accumulator = args[2];
        start = 0;
    } else {
        if (input->count == 0) {
            vm_runtime_error("'reduce' cannot be applied to an empty array without an "
                             "initial value");
            return NIL_VAL;
        }
        accumulator = input->items[0];
        start = 1;
    }

    /* Keep the accumulator rooted on the stack while callbacks run. */
    push(accumulator);
    for (int i = start; i < input->count; i++) {
        Value reduced;

        push(vm.stack_top[-1]);
        push(input->items[i]);
        if (!call_callback(callback, 2, &reduced) || vm.had_error) {
            return NIL_VAL;
        }
        vm.stack_top[-1] = reduced;
    }
    return pop();
}

static Value native_sort(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'sort' expects an array, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = AS_ARRAY(args[0]);

    for (int i = 1; i < array->count; i++) {
        Value key = array->items[i];
        int j = i - 1;

        while (j >= 0) {
            bool before;

            if (!compare_values(false, key, array->items[j], &before)) {
                return NIL_VAL;
            }
            if (!before) {
                break;
            }
            array->items[j + 1] = array->items[j];
            j--;
        }
        array->items[j + 1] = key;
    }
    return args[0];
}

static Value native_sort_by(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error("'sort_by' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = AS_ARRAY(args[0]);
    Value callback = args[1];

    for (int i = 1; i < array->count; i++) {
        Value key = array->items[i];
        int j = i - 1;

        while (j >= 0) {
            Value before;

            push(key);
            push(array->items[j]);
            if (!call_callback(callback, 2, &before) || vm.had_error) {
                return NIL_VAL;
            }
            if (value_is_falsey(before)) {
                break;
            }
            array->items[j + 1] = array->items[j];
            j--;
        }
        array->items[j + 1] = key;
    }
    return args[0];
}

int vm_global_slot(ObjString *name)
{
    Value existing;

    if (!vm_ready) {
        vm_init();
    }
    if (vm.global_names.count > 0 && table_get(&vm.global_names, name, &existing)) {
        return (int)AS_NUMBER(existing);
    }
    if (vm.global_count >= 65536) {
        return -1;
    }
    if (vm.global_count == vm.global_capacity) {
        int capacity = vm.global_capacity < 16 ? 16 : vm.global_capacity * 2;

        vm.global_slots = mem_realloc(vm.global_slots,
                                      sizeof(GlobalSlot) * (size_t)vm.global_capacity,
                                      sizeof(GlobalSlot) * (size_t)capacity);
        vm.global_capacity = capacity;
    }
    int index = vm.global_count++;

    vm.global_slots[index].name = name;
    vm.global_slots[index].value = NIL_VAL;
    vm.global_slots[index].defined = false;
    vm.global_slots[index].is_const = false;
    table_set(&vm.global_names, name, NUMBER_VAL((double)index));
    return index;
}

bool vm_global_is_const(ObjString *name)
{
    Value existing;

    if (!vm_ready) {
        return false;
    }
    if (vm.global_names.count > 0 && table_get(&vm.global_names, name, &existing)) {
        int slot = (int)AS_NUMBER(existing);

        return slot >= 0 && slot < vm.global_count && vm.global_slots[slot].is_const;
    }
    return false;
}

const char *vm_global_slot_name(int slot)
{
    if (slot < 0 || slot >= vm.global_count) {
        return "?";
    }
    return vm.global_slots[slot].name->chars;
}

static void define_native(const char *name, NativeFn function, int arity)
{
    ObjNative *native = object_native(function, arity, name);

    push(OBJ_VAL(native));
    int slot = vm_global_slot(native->name);

    if (slot < 0) {
        fprintf(stderr, "spark: too many global variables\n");
        exit(70);
    }
    vm.global_slots[slot].value = OBJ_VAL(native);
    vm.global_slots[slot].defined = true;
    pop();
}

void vm_runtime_error(const char *fmt, ...)
{
    va_list ap;
    char message[512];
    int line = 0;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    if (vm.frame_count > 0) {
        CallFrame *frame = &vm.frames[vm.frame_count - 1];
        ObjFunction *function = frame->closure->function;
        int offset = (int)(frame->ip - function->chunk.code) - 1;

        if (offset < 0) {
            offset = 0;
        }
        if (function->chunk.count > 0 && offset < function->chunk.count) {
            line = function->chunk.lines[offset];
        }
    }

    fprintf(stderr, "runtime error at line %d: %s\n", line, message);
    for (int i = vm.frame_count - 1; i >= 0; i--) {
        CallFrame *frame = &vm.frames[i];
        ObjFunction *function = frame->closure->function;
        int offset = (int)(frame->ip - function->chunk.code) - 1;
        int frame_line = 0;

        if (offset < 0) {
            offset = 0;
        }
        if (function->chunk.count > 0 && offset < function->chunk.count) {
            frame_line = function->chunk.lines[offset];
        }
        fprintf(stderr, "  in %s (line %d)\n",
                function->name ? function->name->chars : "anonymous", frame_line);
    }
    fflush(stderr);
    vm.had_error = true;
}

/* ------------------------------------------------------------------ */
/* Closures and calls                                                  */
/* ------------------------------------------------------------------ */

static ObjUpvalue *capture_upvalue(Value *local)
{
    ObjUpvalue *previous = NULL;
    ObjUpvalue *upvalue = vm.open_upvalues;

    while (upvalue && upvalue->location > local) {
        previous = upvalue;
        upvalue = upvalue->next;
    }
    if (upvalue && upvalue->location == local) {
        return upvalue;
    }

    ObjUpvalue *created = object_upvalue(local);

    created->next = upvalue;
    if (previous) {
        previous->next = created;
    } else {
        vm.open_upvalues = created;
    }
    return created;
}

static void close_upvalues(Value *last)
{
    while (vm.open_upvalues && vm.open_upvalues->location >= last) {
        ObjUpvalue *upvalue = vm.open_upvalues;

        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm.open_upvalues = upvalue->next;
    }
}

static bool call_closure(ObjClosure *closure, int arg_count)
{
    ObjFunction *function = closure->function;

    if (arg_count != function->arity) {
        vm_runtime_error("the function '%s' expects %d argument(s) but received %d",
                         function->name ? function->name->chars : "anonymous", function->arity,
                         arg_count);
        return false;
    }
    if (vm.frame_count == FRAMES_MAX) {
        vm_runtime_error("too much call depth (possible infinite recursion)");
        return false;
    }

    CallFrame *frame = &vm.frames[vm.frame_count++];

    frame->closure = closure;
    frame->ip = function->chunk.code;
    frame->slots = vm.stack_top - arg_count - 1;
    return true;
}

static bool call_value(Value callee, int arg_count)
{
    if (is_obj(callee)) {
        switch (AS_OBJ(callee)->type) {
        case OBJ_CLOSURE:
            return call_closure((ObjClosure *)AS_OBJ(callee), arg_count);
        case OBJ_NATIVE: {
            ObjNative *native = (ObjNative *)AS_OBJ(callee);

            if (native->arity >= 0 && arg_count != native->arity) {
                vm_runtime_error("the function '%s' expects %d argument(s) but received %d",
                                 native->name->chars, native->arity, arg_count);
                return false;
            }
            /* Capture the base before the call so the native can use the stack
             * freely (including nested calls); we restore it afterwards. */
            Value *native_base = vm.stack_top - arg_count - 1;
            Value result = native->function(arg_count, native_base + 1);

            vm.stack_top = native_base;
            push(result);
            return true;
        }
        default:
            break;
        }
    }
    vm_runtime_error("can only call functions, tried to call a %s",
                     value_type_name(callee));
    return false;
}

/* ------------------------------------------------------------------ */
/* Binary operations shared by the superinstructions                   */
/* ------------------------------------------------------------------ */

static inline bool compare_values(bool greater, Value a, Value b, bool *out)
{
    if (is_number(a) && is_number(b)) {
        *out = greater ? AS_NUMBER(a) > AS_NUMBER(b) : AS_NUMBER(a) < AS_NUMBER(b);
        return true;
    }
    if (is_obj_type(a, OBJ_STRING) && is_obj_type(b, OBJ_STRING)) {
        ObjString *left = AS_STRING(a);
        ObjString *right = AS_STRING(b);
        int min_length = left->length < right->length ? left->length : right->length;
        int comparison = memcmp(left->chars, right->chars, (size_t)min_length);

        if (comparison == 0) {
            comparison = left->length - right->length;
        }
        *out = greater ? comparison > 0 : comparison < 0;
        return true;
    }
    vm_runtime_error("comparison operators expect two numbers or two strings "
                     "(received %s and %s)",
                     value_type_name(a), value_type_name(b));
    return false;
}

static inline bool arithmetic_values(uint8_t op, Value a, Value b, double *out)
{
    if (!is_number(a) || !is_number(b)) {
        vm_runtime_error("arithmetic operators expect numbers (received %s and %s)",
                         value_type_name(a), value_type_name(b));
        return false;
    }
    double left = AS_NUMBER(a);
    double right = AS_NUMBER(b);

    if ((op == OP_DIVIDE || op == OP_MODULO) && right == 0) {
        vm_runtime_error("division by zero");
        return false;
    }
    switch (op) {
    case OP_SUBTRACT: *out = left - right; break;
    case OP_MULTIPLY: *out = left * right; break;
    case OP_DIVIDE: *out = left / right; break;
    default: *out = fmod(left, right); break;
    }
    return true;
}

static inline bool add_values(Value a, Value b, Value *out)
{
    if (is_number(a) && is_number(b)) {
        *out = NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b));
        return true;
    }
    if (is_obj_type(a, OBJ_STRING) && is_obj_type(b, OBJ_STRING)) {
        ObjString *result = object_string_concat(AS_STRING(a), AS_STRING(b));

        if (!result) {
            vm_runtime_error("could not concatenate the strings");
            return false;
        }
        *out = OBJ_VAL(result);
        return true;
    }
    vm_runtime_error("the operands of '+' must be two numbers or two strings "
                     "(received %s and %s)",
                     value_type_name(a), value_type_name(b));
    return false;
}

/* ------------------------------------------------------------------ */
/* Execution loop                                                      */
/* ------------------------------------------------------------------ */

#define READ_BYTE() (*frame->ip++)
#define READ_U16() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->closure->function->chunk.constants[READ_U16()])
#define READ_STRING() AS_STRING(READ_CONSTANT())

/* Reads a big-endian u16 operand at `p`. */
#define U16_AT(p) ((uint16_t)(((p)[0] << 8) | (p)[1]))

/* Reloads the state cached in locals from the VM after anything that may have
 * changed it (the cold path, calls and returns). */
#define LOAD_FRAME()                                                      \
    do {                                                                  \
        ip = frame->ip;                                                   \
        slots = frame->slots;                                             \
        constants = frame->closure->function->chunk.constants;           \
    } while (0)

/* The loop has two paths. The hot path below keeps `ip` and `sp` in local
 * variables (so the compiler can keep them in registers) and only handles the
 * common, error-free, allocation-free cases. Anything else jumps to `cold`
 * with `ip` still pointing at the opcode: the cold path stores `ip` and `sp`
 * back into the VM and runs the general implementation, so errors, the GC and
 * natives always see a consistent state. */
static InterpretResult run(int base_frame)
{
    CallFrame *frame = &vm.frames[vm.frame_count - 1];
    uint8_t *ip;
    Value *slots;
    Value *constants;
    Value *sp = vm.stack_top;

    LOAD_FRAME();

    for (;;) {
        uint8_t instruction;

        switch (*ip) {
        case OP_CONSTANT:
            *sp++ = constants[U16_AT(ip + 1)];
            ip += 3;
            continue;

        case OP_NIL:
            *sp++ = NIL_VAL;
            ip += 1;
            continue;

        case OP_TRUE:
            *sp++ = BOOL_VAL(true);
            ip += 1;
            continue;

        case OP_FALSE:
            *sp++ = BOOL_VAL(false);
            ip += 1;
            continue;

        case OP_POP:
            sp--;
            ip += 1;
            continue;

        case OP_GET_LOCAL:
            *sp++ = slots[ip[1]];
            ip += 2;
            continue;

        case OP_SET_LOCAL:
            slots[ip[1]] = *--sp;
            ip += 2;
            continue;

        case OP_GET_GLOBAL: {
            GlobalSlot *global = &vm.global_slots[U16_AT(ip + 1)];

            if (!global->defined) {
                goto cold;
            }
            *sp++ = global->value;
            ip += 3;
            continue;
        }

        case OP_ADD: {
            Value a = sp[-2];
            Value b = sp[-1];

            if (a.type != VAL_NUMBER || b.type != VAL_NUMBER) {
                goto cold;
            }
            sp--;
            sp[-1] = NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b));
            ip += 1;
            continue;
        }

        case OP_SUBTRACT: {
            Value a = sp[-2];
            Value b = sp[-1];

            if (a.type != VAL_NUMBER || b.type != VAL_NUMBER) {
                goto cold;
            }
            sp--;
            sp[-1] = NUMBER_VAL(AS_NUMBER(a) - AS_NUMBER(b));
            ip += 1;
            continue;
        }

        case OP_MULTIPLY: {
            Value a = sp[-2];
            Value b = sp[-1];

            if (a.type != VAL_NUMBER || b.type != VAL_NUMBER) {
                goto cold;
            }
            sp--;
            sp[-1] = NUMBER_VAL(AS_NUMBER(a) * AS_NUMBER(b));
            ip += 1;
            continue;
        }

        case OP_LESS: {
            Value a = sp[-2];
            Value b = sp[-1];

            if (a.type != VAL_NUMBER || b.type != VAL_NUMBER) {
                goto cold;
            }
            sp--;
            sp[-1] = BOOL_VAL(AS_NUMBER(a) < AS_NUMBER(b));
            ip += 1;
            continue;
        }

        case OP_GREATER: {
            Value a = sp[-2];
            Value b = sp[-1];

            if (a.type != VAL_NUMBER || b.type != VAL_NUMBER) {
                goto cold;
            }
            sp--;
            sp[-1] = BOOL_VAL(AS_NUMBER(a) > AS_NUMBER(b));
            ip += 1;
            continue;
        }

        case OP_ADD_LOCAL_CONST: {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];

            if (a.type != VAL_NUMBER || b.type != VAL_NUMBER) {
                goto cold;
            }
            *sp++ = NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b));
            ip += 4;
            continue;
        }

        case OP_SUBTRACT_LOCAL_CONST: {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];

            if (a.type != VAL_NUMBER || b.type != VAL_NUMBER) {
                goto cold;
            }
            *sp++ = NUMBER_VAL(AS_NUMBER(a) - AS_NUMBER(b));
            ip += 4;
            continue;
        }

        case OP_MULTIPLY_LOCAL_CONST: {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];

            if (a.type != VAL_NUMBER || b.type != VAL_NUMBER) {
                goto cold;
            }
            *sp++ = NUMBER_VAL(AS_NUMBER(a) * AS_NUMBER(b));
            ip += 4;
            continue;
        }

        case OP_LESS_LOCAL_CONST: {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];

            if (a.type != VAL_NUMBER || b.type != VAL_NUMBER) {
                goto cold;
            }
            *sp++ = BOOL_VAL(AS_NUMBER(a) < AS_NUMBER(b));
            ip += 4;
            continue;
        }

        case OP_GREATER_LOCAL_CONST: {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];

            if (a.type != VAL_NUMBER || b.type != VAL_NUMBER) {
                goto cold;
            }
            *sp++ = BOOL_VAL(AS_NUMBER(a) > AS_NUMBER(b));
            ip += 4;
            continue;
        }

        case OP_JUMP:
            ip += 3 + U16_AT(ip + 1);
            continue;

        case OP_JUMP_IF_FALSE:
            if (value_is_falsey(sp[-1])) {
                ip += 3 + U16_AT(ip + 1);
            } else {
                ip += 3;
            }
            continue;

        case OP_JUMP_IF_FALSE_POP:
            if (value_is_falsey(*--sp)) {
                ip += 3 + U16_AT(ip + 1);
            } else {
                ip += 3;
            }
            continue;

        case OP_JUMP_IF_NOT_LESS_LOCAL_CONST: {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];

            if (a.type != VAL_NUMBER || b.type != VAL_NUMBER) {
                goto cold;
            }
            if (AS_NUMBER(a) < AS_NUMBER(b)) {
                ip += 6;
            } else {
                ip += 6 + U16_AT(ip + 4);
            }
            continue;
        }

        case OP_JUMP_IF_NOT_GREATER_LOCAL_CONST: {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];

            if (a.type != VAL_NUMBER || b.type != VAL_NUMBER) {
                goto cold;
            }
            if (AS_NUMBER(a) > AS_NUMBER(b)) {
                ip += 6;
            } else {
                ip += 6 + U16_AT(ip + 4);
            }
            continue;
        }

        case OP_LOOP:
            ip += 3 - U16_AT(ip + 1);
            continue;

        case OP_CALL: {
            int arg_count = ip[1];
            Value callee = sp[-1 - arg_count];

            /* Fast path: a closure called with the right number of arguments
             * and room for another frame. Everything else (natives, errors)
             * goes through the general path. */
            if (callee.type == VAL_OBJ && AS_OBJ(callee)->type == OBJ_CLOSURE) {
                ObjClosure *closure = (ObjClosure *)AS_OBJ(callee);
                ObjFunction *function = closure->function;

                if (arg_count == function->arity && vm.frame_count < FRAMES_MAX) {
                    CallFrame *callee_frame = &vm.frames[vm.frame_count++];

                    frame->ip = ip + 2;
                    callee_frame->closure = closure;
                    callee_frame->ip = function->chunk.code;
                    callee_frame->slots = sp - arg_count - 1;
                    frame = callee_frame;
                    LOAD_FRAME();
                    continue;
                }
            }
            goto cold;
        }

        case OP_RETURN_LOCAL:
        case OP_RETURN: {
            int remaining = vm.frame_count - 1;
            Value result;

            /* Returning from the outermost frame of this run() (or of the
             * program) is handled by the general path. */
            if (remaining == 0 || remaining == base_frame) {
                goto cold;
            }
            result = *ip == OP_RETURN ? sp[-1] : slots[ip[1]];
            if (vm.open_upvalues) {
                close_upvalues(slots);
            }
            vm.frame_count = remaining;
            sp = slots;
            *sp++ = result;
            frame = &vm.frames[remaining - 1];
            LOAD_FRAME();
            continue;
        }

        default:
            break;
        }

    cold:
        frame->ip = ip;
        vm.stack_top = sp;
        instruction = READ_BYTE();

        switch (instruction) {
        case OP_CONSTANT:
            push(READ_CONSTANT());
            break;

        case OP_NIL:
            push(NIL_VAL);
            break;
        case OP_TRUE:
            push(BOOL_VAL(true));
            break;
        case OP_FALSE:
            push(BOOL_VAL(false));
            break;

        case OP_POP:
            pop();
            break;

        case OP_DUP:
            push(peek(0));
            break;

        case OP_DUP2: {
            Value a = peek(1);
            Value b = peek(0);

            push(a);
            push(b);
            break;
        }

        case OP_GET_LOCAL: {
            uint8_t slot = READ_BYTE();

            push(frame->slots[slot]);
            break;
        }

        case OP_SET_LOCAL: {
            uint8_t slot = READ_BYTE();

            frame->slots[slot] = pop();
            break;
        }

        case OP_GET_GLOBAL: {
            uint16_t slot = READ_U16();
            GlobalSlot *global = &vm.global_slots[slot];

            if (!global->defined) {
                vm_runtime_error("global variable '%s' not defined", global->name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            push(global->value);
            break;
        }

        case OP_DEFINE_GLOBAL: {
            uint16_t slot = READ_U16();
            GlobalSlot *global = &vm.global_slots[slot];

            if (global->is_const && global->defined) {
                vm_runtime_error("cannot redefine constant '%s'", global->name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            global->value = pop();
            global->defined = true;
            break;
        }

        case OP_DEFINE_GLOBAL_CONST: {
            uint16_t slot = READ_U16();
            GlobalSlot *global = &vm.global_slots[slot];

            if (global->defined) {
                vm_runtime_error("cannot redefine constant '%s'", global->name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            global->value = pop();
            global->defined = true;
            global->is_const = true;
            break;
        }

        case OP_SET_GLOBAL: {
            uint16_t slot = READ_U16();
            GlobalSlot *global = &vm.global_slots[slot];

            if (!global->defined) {
                vm_runtime_error("global variable '%s' not defined", global->name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            if (global->is_const) {
                vm_runtime_error("cannot assign to constant '%s'", global->name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            global->value = pop();
            break;
        }

        case OP_GET_UPVALUE: {
            uint8_t slot = READ_BYTE();

            push(*frame->closure->upvalues[slot]->location);
            break;
        }

        case OP_SET_UPVALUE: {
            uint8_t slot = READ_BYTE();

            *frame->closure->upvalues[slot]->location = pop();
            break;
        }

        case OP_INDEX: {
            Value index = peek(0);
            Value object = peek(1);
            Value result = NIL_VAL;

            if (is_obj_type(object, OBJ_ARRAY)) {
                if (!is_number(index)) {
                    vm_runtime_error("the index must be a number, received a %s",
                                     value_type_name(index));
                    return INTERPRET_RUNTIME_ERROR;
                }
                double raw = AS_NUMBER(index);

                if (raw < 0 || raw != floor(raw) || raw > INT_MAX) {
                    vm_runtime_error("the index must be a non-negative integer, received %g", raw);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjArray *array = AS_ARRAY(object);
                int position = (int)raw;

                if (position >= array->count) {
                    vm_runtime_error("index out of range: %d (size %d)", position,
                                     array->count);
                    return INTERPRET_RUNTIME_ERROR;
                }
                result = array->items[position];
            } else if (is_obj_type(object, OBJ_MAP)) {
                Value stored;

                if (map_get(AS_MAP(object), index, &stored)) {
                    result = stored;
                }
            } else if (is_obj_type(object, OBJ_STRING)) {
                if (!is_number(index)) {
                    vm_runtime_error("the index must be a number, received a %s",
                                     value_type_name(index));
                    return INTERPRET_RUNTIME_ERROR;
                }
                double raw = AS_NUMBER(index);

                if (raw < 0 || raw != floor(raw) || raw > INT_MAX) {
                    vm_runtime_error("the index must be a non-negative integer, received %g", raw);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjString *string = AS_STRING(object);
                int position = (int)raw;
                int byte_index = utf8_byte_offset(string, position);

                if (byte_index < 0) {
                    vm_runtime_error("index out of range: %d (string of %d characters)",
                                     position, utf8_length(string->chars, string->length));
                    return INTERPRET_RUNTIME_ERROR;
                }
                int char_length = utf8_char_length(string, byte_index);

                result = OBJ_VAL(object_string(string->chars + byte_index, char_length));
            } else {
                vm_runtime_error("only an array, a map or a string can be indexed, "
                                 "indexed a %s",
                                 value_type_name(object));
                return INTERPRET_RUNTIME_ERROR;
            }
            pop();
            pop();
            push(result);
            break;
        }

        case OP_INDEX_SET: {
            Value value = peek(0);
            Value index = peek(1);
            Value object = peek(2);

            if (is_obj_type(object, OBJ_ARRAY)) {
                if (!is_number(index)) {
                    vm_runtime_error("the index must be a number, received a %s",
                                     value_type_name(index));
                    return INTERPRET_RUNTIME_ERROR;
                }
                double raw = AS_NUMBER(index);

                if (raw < 0 || raw != floor(raw) || raw > INT_MAX) {
                    vm_runtime_error("the index must be a non-negative integer, received %g", raw);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjArray *array = AS_ARRAY(object);
                int position = (int)raw;

                if (position >= array->count) {
                    vm_runtime_error("index out of range: %d (size %d)", position,
                                     array->count);
                    return INTERPRET_RUNTIME_ERROR;
                }
                array->items[position] = value;
            } else if (is_obj_type(object, OBJ_MAP)) {
                map_set(AS_MAP(object), index, value);
            } else {
                vm_runtime_error("can only assign to an array or a map, tried to assign to a %s",
                                 value_type_name(object));
                return INTERPRET_RUNTIME_ERROR;
            }
            vm.stack_top -= 3;
            break;
        }

        case OP_ARRAY: {
            int count = READ_U16();
            Value *items = vm.stack_top - count;
            ObjArray *array = object_array();

            push(OBJ_VAL(array));
            for (int i = 0; i < count; i++) {
                array_push(array, items[i]);
            }
            memmove(items, items + count, sizeof(Value));
            vm.stack_top = items + 1;
            break;
        }

        case OP_MAP: {
            int count = READ_U16();
            Value *items = vm.stack_top - 2 * count;
            ObjMap *map = object_map();

            push(OBJ_VAL(map));
            for (int i = 0; i < count; i++) {
                map_set(map, items[2 * i], items[2 * i + 1]);
            }
            memmove(items, items + 2 * count, sizeof(Value));
            vm.stack_top = items + 1;
            break;
        }

        case OP_ITER_KEYS: {
            Value iterable = peek(0);

            if (is_obj_type(iterable, OBJ_ARRAY)) {
                break;
            }
            if (is_obj_type(iterable, OBJ_MAP)) {
                ObjMap *map = AS_MAP(iterable);
                ObjArray *keys = object_array();

                push(OBJ_VAL(keys));
                for (int i = 0; i < map->capacity; i++) {
                    if (map->entries[i].state == 1) {
                        array_push(keys, map->entries[i].key);
                    }
                }
                vm.stack_top[-2] = OBJ_VAL(keys);
                vm.stack_top--;
                break;
            }
            if (is_obj_type(iterable, OBJ_STRING)) {
                ObjString *string = AS_STRING(iterable);
                ObjArray *chars = object_array();

                push(OBJ_VAL(chars));
                for (int i = 0; i < string->length;) {
                    int length = utf8_char_length(string, i);

                    array_push_new_string(chars, string->chars + i, length);
                    i += length;
                }
                vm.stack_top[-2] = OBJ_VAL(chars);
                vm.stack_top--;
                break;
            }
            vm_runtime_error("cannot iterate over a %s", value_type_name(iterable));
            return INTERPRET_RUNTIME_ERROR;
        }

        case OP_LEN: {
            Value value = peek(0);
            double length;

            if (is_obj_type(value, OBJ_ARRAY)) {
                length = (double)AS_ARRAY(value)->count;
            } else if (is_obj_type(value, OBJ_MAP)) {
                length = (double)AS_MAP(value)->count;
            } else if (is_obj_type(value, OBJ_STRING)) {
                length = (double)utf8_length(AS_STRING(value)->chars, AS_STRING(value)->length);
            } else {
                vm_runtime_error("'len' expects an array, a map or a string, received %s",
                                 value_type_name(value));
                return INTERPRET_RUNTIME_ERROR;
            }
            /* Leaves the original value and pushes its length: the compiler uses it
             * to initialize the counter of a for-in. */
            push(NUMBER_VAL(length));
            break;
        }

        case OP_TO_STRING:
            vm.stack_top[-1] = OBJ_VAL(value_to_string(peek(0)));
            break;

        case OP_EQUAL: {
            Value b = pop();
            Value a = pop();

            push(BOOL_VAL(values_equal(a, b)));
            break;
        }

        case OP_LESS_LOCAL_CONST:
        case OP_GREATER_LOCAL_CONST: {
            uint8_t slot = READ_BYTE();
            Value b = frame->closure->function->chunk.constants[READ_U16()];
            bool result;

            if (!compare_values(instruction == OP_GREATER_LOCAL_CONST, frame->slots[slot], b,
                                &result)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            push(BOOL_VAL(result));
            break;
        }

        case OP_GREATER:
        case OP_LESS: {
            Value b = pop();
            Value a = pop();
            bool result;

            if (!compare_values(instruction == OP_GREATER, a, b, &result)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            push(BOOL_VAL(result));
            break;
        }

        case OP_ADD_LOCAL_CONST: {
            uint8_t slot = READ_BYTE();
            Value b = frame->closure->function->chunk.constants[READ_U16()];
            Value result;

            if (!add_values(frame->slots[slot], b, &result)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            push(result);
            break;
        }

        case OP_ADD: {
            Value b = pop();
            Value a = pop();
            Value result;

            if (!add_values(a, b, &result)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            push(result);
            break;
        }

        case OP_SUBTRACT_LOCAL_CONST:
        case OP_MULTIPLY_LOCAL_CONST:
        case OP_DIVIDE_LOCAL_CONST: {
            uint8_t slot = READ_BYTE();
            Value b = frame->closure->function->chunk.constants[READ_U16()];
            uint8_t op = instruction == OP_SUBTRACT_LOCAL_CONST ? OP_SUBTRACT
                         : instruction == OP_MULTIPLY_LOCAL_CONST ? OP_MULTIPLY
                                                                  : OP_DIVIDE;
            double result;

            if (!arithmetic_values(op, frame->slots[slot], b, &result)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            push(NUMBER_VAL(result));
            break;
        }

        case OP_SUBTRACT:
        case OP_MULTIPLY:
        case OP_DIVIDE:
        case OP_MODULO: {
            Value b = pop();
            Value a = pop();
            double result;

            if (!arithmetic_values(instruction, a, b, &result)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            push(NUMBER_VAL(result));
            break;
        }

        case OP_NOT:
            push(BOOL_VAL(value_is_falsey(pop())));
            break;

        case OP_NEGATE: {
            Value value = pop();

            if (!is_number(value)) {
                vm_runtime_error("negation expects a number, received %s",
                                 value_type_name(value));
                return INTERPRET_RUNTIME_ERROR;
            }
            push(NUMBER_VAL(-AS_NUMBER(value)));
            break;
        }

        case OP_PRINT_REPL:
            value_print(stdout, peek(0));
            fputc('\n', stdout);
            fflush(stdout);
            pop();
            break;

        case OP_JUMP: {
            uint16_t offset = READ_U16();

            frame->ip += offset;
            break;
        }

        case OP_JUMP_IF_FALSE: {
            uint16_t offset = READ_U16();

            if (value_is_falsey(peek(0))) {
                frame->ip += offset;
            }
            break;
        }

        case OP_JUMP_IF_FALSE_POP: {
            uint16_t offset = READ_U16();

            if (value_is_falsey(pop())) {
                frame->ip += offset;
            }
            break;
        }

        case OP_JUMP_IF_NOT_LESS_LOCAL_CONST:
        case OP_JUMP_IF_NOT_GREATER_LOCAL_CONST: {
            uint8_t slot = READ_BYTE();
            Value b = frame->closure->function->chunk.constants[READ_U16()];
            uint16_t offset = READ_U16();
            bool result;

            if (!compare_values(instruction == OP_JUMP_IF_NOT_GREATER_LOCAL_CONST,
                                frame->slots[slot], b, &result)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            if (!result) {
                frame->ip += offset;
            }
            break;
        }

        case OP_LOOP: {
            uint16_t offset = READ_U16();

            frame->ip -= offset;
            break;
        }

        case OP_CALL: {
            int arg_count = READ_BYTE();

            if (!call_value(peek(arg_count), arg_count)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            if (vm.had_error) {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }

        case OP_CLOSURE: {
            ObjFunction *function = AS_FUNCTION(READ_CONSTANT());
            ObjClosure *closure = object_closure(function);

            push(OBJ_VAL(closure));
            for (int i = 0; i < closure->upvalue_count; i++) {
                uint8_t is_local = READ_BYTE();
                uint8_t index = READ_BYTE();

                if (is_local) {
                    closure->upvalues[i] = capture_upvalue(frame->slots + index);
                } else {
                    closure->upvalues[i] = frame->closure->upvalues[index];
                }
            }
            break;
        }

        case OP_CLOSE_UPVALUE:
            close_upvalues(vm.stack_top - 1);
            pop();
            break;

        case OP_RETURN_LOCAL: {
            uint8_t slot = READ_BYTE();
            Value result = frame->slots[slot];

            close_upvalues(frame->slots);
            vm.frame_count--;
            if (vm.frame_count == 0) {
                vm.stack_top = vm.stack;
                return INTERPRET_OK;
            }
            if (vm.frame_count == base_frame) {
                vm.stack_top = vm.frames[vm.frame_count].slots;
                push(result);
                return INTERPRET_OK;
            }
            vm.stack_top = vm.frames[vm.frame_count].slots;
            push(result);
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }

        case OP_RETURN: {
            Value result = pop();

            close_upvalues(frame->slots);
            vm.frame_count--;
            if (vm.frame_count == 0) {
                pop();
                return INTERPRET_OK;
            }
            if (vm.frame_count == base_frame) {
                vm.stack_top = vm.frames[vm.frame_count].slots;
                push(result);
                return INTERPRET_OK;
            }
            vm.stack_top = vm.frames[vm.frame_count].slots;
            push(result);
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }

        default:
            vm_runtime_error("unknown instruction (%d)", instruction);
            return INTERPRET_RUNTIME_ERROR;
        }

        /* The general path may have changed the frame, the stack or the ip. */
        sp = vm.stack_top;
        LOAD_FRAME();
    }
}

/* Calls `callee` with `arg_count` arguments already pushed on the stack and
 * stores its return value in *result. Used by higher-order natives. */
static bool call_callback(Value callee, int arg_count, Value *result)
{
    Value *args = vm.stack_top - arg_count;

    if (is_obj_type(callee, OBJ_NATIVE)) {
        ObjNative *native = (ObjNative *)AS_OBJ(callee);

        if (native->arity >= 0 && arg_count != native->arity) {
            vm_runtime_error("the function '%s' expects %d argument(s) but received %d",
                             native->name->chars, native->arity, arg_count);
            return false;
        }
        *result = native->function(arg_count, args);
        vm.stack_top = args;
        return true;
    }
    if (!is_obj_type(callee, OBJ_CLOSURE)) {
        vm_runtime_error("a callback must be a function, received %s", value_type_name(callee));
        return false;
    }
    if (vm.frame_count == FRAMES_MAX) {
        vm_runtime_error("too much call depth (possible infinite recursion)");
        return false;
    }

    memmove(args + 1, args, sizeof(Value) * (size_t)arg_count);
    args[0] = callee;
    vm.stack_top++;

    int base_frame = vm.frame_count;

    if (!call_closure((ObjClosure *)AS_OBJ(callee), arg_count)) {
        vm.stack_top = args;
        return false;
    }

    InterpretResult status = run(base_frame);

    if (status != INTERPRET_OK) {
        return false;
    }
    *result = pop();
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void vm_init(void)
{
    if (vm_ready) {
        return;
    }
    memset(&vm, 0, sizeof(vm));
    vm.stack_top = vm.stack;
    table_init(&vm.global_names);
    mem_set_collector(collect_garbage);
    mem_set_threshold(1024u * 1024u);
    vm_ready = true;

    define_native("print", native_print, -1);
    define_native("len", native_len, 1);
    define_native("push", native_push, 2);
    define_native("pop", native_pop, 1);
    define_native("str", native_str, 1);
    define_native("num", native_num, 1);
    define_native("type", native_type, 1);
    define_native("clock", native_clock, 0);
    define_native("assert", native_assert, -1);
    define_native("has", native_has, 2);
    define_native("del", native_del, 2);
    define_native("keys", native_keys, 1);
    define_native("values", native_values, 1);
    define_native("range", native_range, -1);
    define_native("substr", native_substr, -1);
    define_native("abs", native_abs, 1);
    define_native("floor", native_floor, 1);
    define_native("ceil", native_ceil, 1);
    define_native("round", native_round, 1);
    define_native("sqrt", native_sqrt, 1);
    define_native("pow", native_pow, 2);
    define_native("min", native_min, 2);
    define_native("max", native_max, 2);
    define_native("contains", native_contains, 2);
    define_native("index_of", native_index_of, 2);
    define_native("insert", native_insert, 3);
    define_native("remove", native_remove, 2);
    define_native("reverse", native_reverse, 1);
    define_native("fill", native_fill, 2);
    define_native("slice", native_slice, -1);
    define_native("starts_with", native_starts_with, 2);
    define_native("ends_with", native_ends_with, 2);
    define_native("split", native_split, 2);
    define_native("join", native_join, 2);
    define_native("trim", native_trim, 1);
    define_native("upper", native_upper, 1);
    define_native("lower", native_lower, 1);
    define_native("repeat", native_repeat, 2);
    define_native("replace", native_replace, 3);
    define_native("clear", native_clear, 1);
    define_native("merge", native_merge, 2);
    define_native("get", native_get, -1);
    define_native("map", native_map, 2);
    define_native("filter", native_filter, 2);
    define_native("each", native_each, 2);
    define_native("reduce", native_reduce, -1);
    define_native("sort", native_sort, 1);
    define_native("sort_by", native_sort_by, 2);
}

void vm_free(void)
{
    if (!vm_ready) {
        return;
    }
    table_free(&vm.global_names);
    mem_realloc(vm.global_slots, sizeof(GlobalSlot) * (size_t)vm.global_capacity, 0);
    vm.global_slots = NULL;
    vm.global_count = 0;
    vm.global_capacity = 0;
    intern_free_table();

    Obj *object = objects_head();

    while (object) {
        Obj *next = object->next;

        object_free(object);
        object = next;
    }
    objects_set_head(NULL);
    free(gray.items);
    gray.items = NULL;
    gray.count = 0;
    gray.capacity = 0;
    memset(&vm, 0, sizeof(vm));
    vm.stack_top = vm.stack;
    vm_ready = false;
}

InterpretResult vm_interpret(const char *source, const char *script_name, bool repl_mode)
{
    Arena arena;
    Stmt *program = NULL;
    char error[256];
    int error_line = 0;

    arena_init(&arena);
    if (!parse_source(source, &arena, &program, error, sizeof(error), &error_line)) {
        fprintf(stderr, "syntax error at line %d: %s\n", error_line, error);
        arena_free(&arena);
        return INTERPRET_COMPILE_ERROR;
    }

    ObjFunction *function = compile_ast(program, script_name, repl_mode, error, sizeof(error),
                                        &error_line);

    arena_free(&arena);
    if (!function) {
        fprintf(stderr, "compilation error at line %d: %s\n", error_line, error);
        return INTERPRET_COMPILE_ERROR;
    }

    push(OBJ_VAL(function));
    ObjClosure *closure = object_closure(function);

    pop();
    push(OBJ_VAL(closure));
    if (!call_closure(closure, 0)) {
        close_upvalues(vm.stack);
        vm.stack_top = vm.stack;
        vm.frame_count = 0;
        vm.open_upvalues = NULL;
        vm.had_error = false;
        return INTERPRET_RUNTIME_ERROR;
    }

    InterpretResult result = run(0);

    close_upvalues(vm.stack);
    vm.stack_top = vm.stack;
    vm.frame_count = 0;
    vm.open_upvalues = NULL;
    vm.had_error = false;
    return result;
}
