#include "vm.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ast.h"
#include "buf.h"
#include "chunk.h"
#include "compiler.h"
#include "json.h"
#include "memory.h"
#include "object.h"
#include "parser.h"
#include "regex.h"
#include "table.h"

#define STACK_MAX 131072
#define FRAMES_MAX 256

typedef struct {
    ObjClosure *closure;
    uint8_t *ip;
    Value *slots;
    Value *constants;
    int argc;
} CallFrame;

typedef struct {
    ObjString *name;
    Value value;
    bool defined;
    bool is_const;
} GlobalSlot;

typedef struct {
    char *text;
    size_t length;
} SourceText;

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
    SourceText *sources;
    int source_count;
    int source_capacity;
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
/* Numbers the hot path can combine directly: integers and floats. A mixed
 * pair is converted to double, which is the same promotion the general path
 * applies, so mixed arithmetic does not have to leave the fast path. */
static inline bool numeric_fast(Value v)
{
    return v.type == VAL_INT || v.type == VAL_NUMBER;
}

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
    case VAL_INT: return "number";
    case VAL_OBJ: return obj_type_name(AS_OBJ(value)->type);
    }
    return "unknown";
}

/* Numeric comparison: exact for two integers, otherwise by promoting. */
static inline bool numeric_less(Value a, Value b)
{
    if (a.type == VAL_INT && b.type == VAL_INT) {
        return AS_INT(a) < AS_INT(b);
    }
    return value_as_double(a) < value_as_double(b);
}

static inline bool numeric_greater(Value a, Value b)
{
    if (a.type == VAL_INT && b.type == VAL_INT) {
        return AS_INT(a) > AS_INT(b);
    }
    return value_as_double(a) > value_as_double(b);
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
    case OBJ_FILE: {
        ObjFile *file = (ObjFile *)object;
        if (file->path) {
            mark_object((Obj *)file->path);
        }
        break;
    }
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

        return INT_VAL(utf8_length(string->chars, string->length));
    }
    if (is_obj_type(value, OBJ_ARRAY)) {
        return INT_VAL(AS_ARRAY(value)->count);
    }
    if (is_obj_type(value, OBJ_MAP)) {
        return INT_VAL(AS_MAP(value)->count);
    }
    vm_runtime_error(ERROR_TYPE, "'len' expects a string, an array or a map, received %s",
                     value_type_name(value));
    return NIL_VAL;
}

static Value native_push(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'push' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = AS_ARRAY(args[0]);

    if (array->count < array->capacity) {
        array->items[array->count++] = args[1];
        return args[0];
    }
    array_push(array, args[1]);
    return args[0];
}

static Value native_pop(int arg_count, Value *args)
{
    Value popped;

    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'pop' expects an array, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = (ObjArray *)AS_OBJ(args[0]);

    if (!array_pop(array, &popped)) {
        vm_runtime_error(ERROR_VALUE, "'pop' cannot be applied to an empty array");
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
    if (is_int(args[0]) || is_number(args[0])) {
        return args[0];
    }
    if (is_obj_type(args[0], OBJ_STRING)) {
        ObjString *string = AS_STRING(args[0]);
        char *end = NULL;
        double result = strtod(string->chars, &end);

        if (end == string->chars) {
            vm_runtime_error(ERROR_VALUE, "'num' could not convert \"%s\" to a number", string->chars);
            return NIL_VAL;
        }
        while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
            end++;
        }
        if (*end != '\0') {
            vm_runtime_error(ERROR_VALUE, "'num' could not convert \"%s\" to a number", string->chars);
            return NIL_VAL;
        }
        return NUMBER_VAL(result);
    }
    vm_runtime_error(ERROR_TYPE, "'num' expects a number or a string, received %s", value_type_name(args[0]));
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
        vm_runtime_error(ERROR_ARITY, "'assert' expects 1 or 2 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (value_is_falsey(args[0])) {
        if (arg_count == 2) {
            ObjString *message = is_obj_type(args[1], OBJ_STRING) ? AS_STRING(args[1])
                                                                 : value_to_string(args[1]);

            vm_runtime_error(ERROR_ASSERT, "%s", message->chars);
        } else {
            vm_runtime_error(ERROR_ASSERT, "assert failed");
        }
    }
    return NIL_VAL;
}

static Value native_has(int arg_count, Value *args)
{
    Value ignored;

    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_MAP)) {
        vm_runtime_error(ERROR_TYPE, "'has' expects a map as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    return BOOL_VAL(map_get(AS_MAP(args[0]), args[1], &ignored));
}

static Value native_del(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_MAP)) {
        vm_runtime_error(ERROR_TYPE, "'del' expects a map as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    return BOOL_VAL(map_delete(AS_MAP(args[0]), args[1]));
}

static Value native_keys(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_MAP)) {
        vm_runtime_error(ERROR_TYPE, "'keys' expects a map, received %s", value_type_name(args[0]));
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
        vm_runtime_error(ERROR_TYPE, "'values' expects a map, received %s", value_type_name(args[0]));
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
    if (is_int(value)) {
        int64_t integer = AS_INT(value);

        if (integer < -2147483648LL || integer > 2147483647LL) {
            vm_runtime_error(ERROR_VALUE, "'%s' expects an integer as %s, received %lld", function,
                             name, (long long)integer);
            return false;
        }
        *out = (int)integer;
        return true;
    }
    if (!is_number(value)) {
        vm_runtime_error(ERROR_VALUE, "'%s' expects an integer as %s, received %s", function, name,
                         value_type_name(value));
        return false;
    }
    double raw = AS_NUMBER(value);

    if (raw != floor(raw) || raw < -2147483648.0 || raw > 2147483647.0) {
        vm_runtime_error(ERROR_VALUE, "'%s' expects an integer as %s, received %g", function, name, raw);
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
        vm_runtime_error(ERROR_ARITY, "'range' expects 1 or 2 arguments, received %d", arg_count);
        return NIL_VAL;
    }

    if (end < start) {
        end = start;
    }
    if ((long long)end - start > 10000000LL) {
        vm_runtime_error(ERROR_VALUE, "'range' cannot generate more than 10 million elements");
        return NIL_VAL;
    }

    ObjArray *array = object_array();

    push(OBJ_VAL(array));
    for (int i = start; i < end; i++) {
        array_push(array, INT_VAL(i));
    }
    pop();
    return OBJ_VAL(array);
}

static Value native_substr(int arg_count, Value *args)
{
    if (arg_count < 2 || arg_count > 3) {
        vm_runtime_error(ERROR_ARITY, "'substr' expects 2 or 3 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'substr' expects a string as first argument, received %s",
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
        vm_runtime_error(ERROR_VALUE, "'substr' does not accept a negative start (%d)", start);
        return NIL_VAL;
    }
    if (arg_count == 3) {
        if (!integer_argument(args[2], "count", "substr", &count)) {
            return NIL_VAL;
        }
        if (count < 0) {
            vm_runtime_error(ERROR_VALUE, "'substr' does not accept a negative count (%d)", count);
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

static Value native_abs(int arg_count, Value *args)
{
    (void)arg_count;
    if (is_int(args[0])) {
        int64_t value = AS_INT(args[0]);

        if (value == INT64_MIN) {
            return NUMBER_VAL(TWO_POW_63);
        }
        return INT_VAL(value < 0 ? -value : value);
    }
    if (is_number(args[0])) {
        return NUMBER_VAL(fabs(AS_NUMBER(args[0])));
    }
    vm_runtime_error(ERROR_TYPE, "'abs' expects a number, received %s", value_type_name(args[0]));
    return NIL_VAL;
}

static Value rounding_native(int arg_count, Value *args, const char *name, double (*fn)(double))
{
    (void)arg_count;
    if (is_int(args[0])) {
        return args[0];
    }
    if (is_number(args[0])) {
        return NUMBER_VAL(fn(AS_NUMBER(args[0])));
    }
    vm_runtime_error(ERROR_TYPE, "'%s' expects a number, received %s", name, value_type_name(args[0]));
    return NIL_VAL;
}

static Value native_floor(int arg_count, Value *args)
{
    return rounding_native(arg_count, args, "floor", floor);
}

static Value native_ceil(int arg_count, Value *args)
{
    return rounding_native(arg_count, args, "ceil", ceil);
}

static Value native_round(int arg_count, Value *args)
{
    return rounding_native(arg_count, args, "round", round);
}

static Value native_sqrt(int arg_count, Value *args)
{
    (void)arg_count;
    if (!value_is_numeric(args[0])) {
        vm_runtime_error(ERROR_TYPE, "'sqrt' expects a number, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    if (value_as_double(args[0]) < 0) {
        vm_runtime_error(ERROR_VALUE, "'sqrt' does not accept negative numbers");
        return NIL_VAL;
    }
    return NUMBER_VAL(sqrt(value_as_double(args[0])));
}

static Value native_pow(int arg_count, Value *args)
{
    (void)arg_count;
    if (!value_is_numeric(args[0]) || !value_is_numeric(args[1])) {
        vm_runtime_error(ERROR_TYPE, "'pow' expects two numbers");
        return NIL_VAL;
    }
    return NUMBER_VAL(pow(value_as_double(args[0]), value_as_double(args[1])));
}

static Value native_min(int arg_count, Value *args)
{
    (void)arg_count;
    if (!value_is_numeric(args[0]) || !value_is_numeric(args[1])) {
        vm_runtime_error(ERROR_TYPE, "'min' expects two numbers");
        return NIL_VAL;
    }
    return numeric_less(args[1], args[0]) ? args[1] : args[0];
}

static Value native_max(int arg_count, Value *args)
{
    (void)arg_count;
    if (!value_is_numeric(args[0]) || !value_is_numeric(args[1])) {
        vm_runtime_error(ERROR_TYPE, "'max' expects two numbers");
        return NIL_VAL;
    }
    return numeric_greater(args[1], args[0]) ? args[1] : args[0];
}

/* ------------------------------------------------------------------ */
/* Integer conversion natives                                          */
/* ------------------------------------------------------------------ */

static bool string_has_only_trailing_space(const char *end)
{
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
        end++;
    }
    return *end == '\0';
}

static Value native_int(int arg_count, Value *args)
{
    (void)arg_count;
    if (is_int(args[0])) {
        return args[0];
    }
    if (is_number(args[0])) {
        double value = AS_NUMBER(args[0]);

        if (!(value >= -TWO_POW_63 && value < TWO_POW_63)) {
            vm_runtime_error(ERROR_VALUE, "'int' cannot convert %g to an integer", value);
            return NIL_VAL;
        }
        return INT_VAL((int64_t)value);
    }
    if (is_obj_type(args[0], OBJ_STRING)) {
        ObjString *string = AS_STRING(args[0]);
        char *end = NULL;
        long long result;

        errno = 0;
        result = strtoll(string->chars, &end, 10);
        if (end == string->chars || errno != 0 || !string_has_only_trailing_space(end)) {
            vm_runtime_error(ERROR_VALUE, "'int' could not convert \"%s\" to an integer", string->chars);
            return NIL_VAL;
        }
        return INT_VAL((int64_t)result);
    }
    vm_runtime_error(ERROR_TYPE, "'int' expects a number or a string, received %s",
                     value_type_name(args[0]));
    return NIL_VAL;
}

static Value native_float(int arg_count, Value *args)
{
    (void)arg_count;
    if (is_int(args[0]) || is_number(args[0])) {
        return NUMBER_VAL(value_as_double(args[0]));
    }
    if (is_obj_type(args[0], OBJ_STRING)) {
        ObjString *string = AS_STRING(args[0]);
        char *end = NULL;
        double result = strtod(string->chars, &end);

        if (end == string->chars || !string_has_only_trailing_space(end)) {
            vm_runtime_error(ERROR_VALUE, "'float' could not convert \"%s\" to a number", string->chars);
            return NIL_VAL;
        }
        return NUMBER_VAL(result);
    }
    vm_runtime_error(ERROR_TYPE, "'float' expects a number or a string, received %s",
                     value_type_name(args[0]));
    return NIL_VAL;
}

static Value native_is_int(int arg_count, Value *args)
{
    (void)arg_count;
    return BOOL_VAL(is_int(args[0]));
}

static Value native_is_nan(int arg_count, Value *args)
{
    (void)arg_count;
    if (args[0].type == VAL_NUMBER && isnan(args[0].as.number)) {
        return BOOL_VAL(true);
    }
    return BOOL_VAL(false);
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
            vm_runtime_error(ERROR_TYPE, "'contains' expects a string as the value to look up when the "
                             "container is a string, received %s", value_type_name(needle));
            return NIL_VAL;
        }
        return BOOL_VAL(string_byte_find(AS_STRING(container), AS_STRING(needle)) >= 0);
    }
    if (is_obj_type(container, OBJ_MAP)) {
        Value ignored;

        return BOOL_VAL(map_get(AS_MAP(container), needle, &ignored));
    }
    vm_runtime_error(ERROR_TYPE, "'contains' expects an array, a string or a map as first argument, "
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
                return INT_VAL(i);
            }
        }
        return INT_VAL(-1);
    }
    if (is_obj_type(container, OBJ_STRING)) {
        if (!is_obj_type(needle, OBJ_STRING)) {
            vm_runtime_error(ERROR_TYPE, "'index_of' expects a string as the value to look up when the "
                             "container is a string, received %s", value_type_name(needle));
            return NIL_VAL;
        }
        ObjString *haystack = AS_STRING(container);
        int byte_index = string_byte_find(haystack, AS_STRING(needle));

        if (byte_index < 0) {
            return INT_VAL(-1);
        }
        return INT_VAL(utf8_length(haystack->chars, byte_index));
    }
    vm_runtime_error(ERROR_TYPE, "'index_of' expects an array or a string as first argument, "
                     "received %s", value_type_name(container));
    return NIL_VAL;
}

static Value native_insert(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'insert' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = AS_ARRAY(args[0]);
    int index;

    if (!integer_argument(args[1], "index", "insert", &index)) {
        return NIL_VAL;
    }
    if (index < 0 || index > array->count) {
        vm_runtime_error(ERROR_INDEX, "'insert' index out of range: %d (size %d)", index, array->count);
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
        vm_runtime_error(ERROR_TYPE, "'remove' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = AS_ARRAY(args[0]);
    int index;

    if (!integer_argument(args[1], "index", "remove", &index)) {
        return NIL_VAL;
    }
    if (index < 0 || index >= array->count) {
        vm_runtime_error(ERROR_INDEX, "'remove' index out of range: %d (size %d)", index, array->count);
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
        vm_runtime_error(ERROR_TYPE, "'reverse' expects an array, received %s", value_type_name(args[0]));
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
        vm_runtime_error(ERROR_TYPE, "'fill' expects an array, received %s", value_type_name(args[0]));
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
        vm_runtime_error(ERROR_ARITY, "'slice' expects 2 or 3 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'slice' expects an array as first argument, received %s",
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
        vm_runtime_error(ERROR_VALUE, "'slice' does not accept negative indexes (start %d, end %d)",
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
        vm_runtime_error(ERROR_TYPE, "'starts_with' expects two strings");
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
        vm_runtime_error(ERROR_TYPE, "'ends_with' expects two strings");
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
        vm_runtime_error(ERROR_TYPE, "'split' expects two strings");
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
        vm_runtime_error(ERROR_MEMORY, "could not allocate memory for '%s'", function);
        return false;
    }
    return true;
}

static Value native_join(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'join' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    if (!is_obj_type(args[1], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'join' expects a string as separator, received %s",
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
            vm_runtime_error(ERROR_MEMORY, "'join' would create a string that is too long");
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
        vm_runtime_error(ERROR_TYPE, "'trim' expects a string, received %s", value_type_name(args[0]));
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
        vm_runtime_error(ERROR_TYPE, "'%s' expects a string, received %s", name, value_type_name(value));
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
        vm_runtime_error(ERROR_TYPE, "'repeat' expects a string, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjString *string = AS_STRING(args[0]);
    int times;

    if (!integer_argument(args[1], "count", "repeat", &times)) {
        return NIL_VAL;
    }
    if (times < 0) {
        vm_runtime_error(ERROR_VALUE, "'repeat' does not accept a negative count (%d)", times);
        return NIL_VAL;
    }
    if (string->length > 0 && times > INT_MAX / string->length) {
        vm_runtime_error(ERROR_MEMORY, "'repeat' would create a string that is too long");
        return NIL_VAL;
    }
    int length = string->length * times;

    if (length == 0) {
        return OBJ_VAL(object_string("", 0));
    }
    char *buffer = malloc((size_t)length + 1);

    if (!buffer) {
        vm_runtime_error(ERROR_MEMORY, "could not allocate memory for 'repeat'");
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
        vm_runtime_error(ERROR_TYPE, "'replace' expects three strings");
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
            vm_runtime_error(ERROR_MEMORY, "'replace' would create a string that is too long");
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
        vm_runtime_error(ERROR_TYPE, "'clear' expects a map, received %s", value_type_name(args[0]));
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
        vm_runtime_error(ERROR_TYPE, "'merge' expects two maps");
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
        vm_runtime_error(ERROR_ARITY, "'get' expects 2 or 3 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_MAP)) {
        vm_runtime_error(ERROR_TYPE, "'get' expects a map as first argument, received %s",
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
        vm_runtime_error(ERROR_TYPE, "'map' expects an array as first argument, received %s",
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
        vm_runtime_error(ERROR_TYPE, "'filter' expects an array as first argument, received %s",
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
        vm_runtime_error(ERROR_TYPE, "'each' expects an array as first argument, received %s",
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
        vm_runtime_error(ERROR_ARITY, "'reduce' expects 2 or 3 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'reduce' expects an array as first argument, received %s",
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
            vm_runtime_error(ERROR_VALUE, "'reduce' cannot be applied to an empty array without an "
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

/* Strict "right comes before left" test used by the stable merge sort. */
typedef bool (*ValueLess)(Value callback, Value right, Value left, bool *out);

static bool less_natural(Value callback, Value right, Value left, bool *out)
{
    (void)callback;
    return compare_values(false, right, left, out);
}

static bool less_callback(Value callback, Value right, Value left, bool *out)
{
    Value before;

    push(right);
    push(left);
    if (!call_callback(callback, 2, &before) || vm.had_error) {
        return false;
    }
    *out = !value_is_falsey(before);
    return true;
}

/* Bottom-up stable merge sort, O(n log n). Both work buffers are arrays rooted on
 * the VM stack and the source is never modified while a pass reads it, so the
 * collector always sees every value, even while a user callback runs. The array
 * is written back only at the end. */
static bool sort_array_with(ObjArray *array, ValueLess less, Value callback)
{
    int n = array->count;
    bool ok = true;

    /* Root each buffer as soon as it exists: creating the second one can collect. */
    ObjArray *first = object_array();
    push(OBJ_VAL(first));
    ObjArray *second = object_array();
    push(OBJ_VAL(second));
    for (int i = 0; i < n; i++) {
        array_push(first, array->items[i]);
        array_push(second, NIL_VAL);
    }

    Value *src = first->items;
    Value *dst = second->items;

    for (int width = 1; ok && width < n; width *= 2) {
        for (int lo = 0; ok && lo < n; lo += 2 * width) {
            int mid = lo + width < n ? lo + width : n;
            int hi = lo + 2 * width < n ? lo + 2 * width : n;
            int i = lo;
            int j = mid;
            int k = lo;

            while (i < mid && j < hi) {
                bool right_first;

                if (!less(callback, src[j], src[i], &right_first)) {
                    ok = false;
                    break;
                }
                dst[k++] = right_first ? src[j++] : src[i++];
            }
            while (ok && i < mid) {
                dst[k++] = src[i++];
            }
            while (ok && j < hi) {
                dst[k++] = src[j++];
            }
        }
        Value *swap = src;
        src = dst;
        dst = swap;
    }

    if (ok) {
        for (int i = 0; i < n && i < array->count; i++) {
            array->items[i] = src[i];
        }
    }
    pop();
    pop();
    return ok;
}

static Value native_sort(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'sort' expects an array, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = AS_ARRAY(args[0]);

    if (array->count < 2) {
        return args[0];
    }
    return sort_array_with(array, less_natural, NIL_VAL) ? args[0] : NIL_VAL;
}

static Value native_sort_by(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'sort_by' expects an array as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *array = AS_ARRAY(args[0]);

    if (array->count < 2) {
        return args[0];
    }
    return sort_array_with(array, less_callback, args[1]) ? args[0] : NIL_VAL;
}

extern char **environ;

static Value native_exit(int arg_count, Value *args)
{
    int code = 0;
    if (arg_count == 1) {
        if (!integer_argument(args[0], "code", "exit", &code)) {
            return NIL_VAL;
        }
    } else if (arg_count > 1) {
        vm_runtime_error(ERROR_ARITY, "'exit' expects 0 or 1 argument, received %d", arg_count);
        return NIL_VAL;
    }
    fflush(stdout);
    fflush(stderr);
    exit(code);
    return NIL_VAL;
}

static Value native_env(int arg_count, Value *args)
{
    if (arg_count == 0) {
        ObjMap *map = object_map();
        push(OBJ_VAL(map));
        if (environ) {
            for (char **e = environ; *e; e++) {
                char *eq = strchr(*e, '=');
                if (eq) {
                    ObjString *key = object_string(*e, (int)(eq - *e));
                    push(OBJ_VAL(key));
                    ObjString *val = object_string(eq + 1, (int)strlen(eq + 1));
                    push(OBJ_VAL(val));
                    map_set(map, OBJ_VAL(key), OBJ_VAL(val));
                    pop();
                    pop();
                }
            }
        }
        return pop();
    }
    if (arg_count == 1) {
        if (!is_obj_type(args[0], OBJ_STRING)) {
            vm_runtime_error(ERROR_TYPE, "'env' expects a string, received %s",
                             value_type_name(args[0]));
            return NIL_VAL;
        }
        ObjString *name_str = AS_STRING(args[0]);
        if (strlen(name_str->chars) != (size_t)name_str->length) {
            return NIL_VAL;
        }
        const char *val = getenv(name_str->chars);
        if (!val) {
            return NIL_VAL;
        }
        return OBJ_VAL(object_string(val, (int)strlen(val)));
    }
    vm_runtime_error(ERROR_ARITY, "'env' expects 0 or 1 argument, received %d", arg_count);
    return NIL_VAL;
}

static Value native_input(int arg_count, Value *args)
{
    if (arg_count == 1) {
        if (is_obj_type(args[0], OBJ_STRING)) {
            fputs(AS_STRING(args[0])->chars, stdout);
        } else {
            value_print(stdout, args[0]);
        }
        fflush(stdout);
    } else if (arg_count > 1) {
        vm_runtime_error(ERROR_ARITY, "'input' expects 0 or 1 argument, received %d", arg_count);
        return NIL_VAL;
    }

    char *line = NULL;
    size_t capacity = 0;
    ssize_t read_bytes = getline(&line, &capacity, stdin);

    if (read_bytes < 0) {
        free(line);
        return NIL_VAL;
    }

    while (read_bytes > 0 && (line[read_bytes - 1] == '\n' || line[read_bytes - 1] == '\r')) {
        read_bytes--;
    }
    line[read_bytes] = '\0';

    ObjString *result = object_string(line, (int)read_bytes);
    free(line);
    return OBJ_VAL(result);
}

void vm_set_args(int argc, char **argv)
{
    ObjArray *array = object_array();
    push(OBJ_VAL(array));
    for (int i = 0; i < argc; i++) {
        ObjString *arg = object_string(argv[i], (int)strlen(argv[i]));
        push(OBJ_VAL(arg));
        array_push(array, OBJ_VAL(arg));
        pop();
    }
    int slot = vm_global_slot(object_string("argv", 4));
    if (slot >= 0) {
        vm.global_slots[slot].value = OBJ_VAL(array);
        vm.global_slots[slot].defined = true;
    }
    pop();
}

static inline bool string_has_null(ObjString *s, const char *fn_name)
{
    if (strlen(s->chars) != (size_t)s->length) {
        vm_runtime_error(ERROR_VALUE, "'%s' argument cannot contain null bytes", fn_name);
        return true;
    }
    return false;
}

static Value native_file_exists(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'file_exists' expects a string, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjString *path_str = AS_STRING(args[0]);
    if (string_has_null(path_str, "file_exists")) {
        return NIL_VAL;
    }
    return BOOL_VAL(access(path_str->chars, F_OK) == 0);
}

static Value native_read_file(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'read_file' expects a string, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjString *path_str = AS_STRING(args[0]);
    if (string_has_null(path_str, "read_file")) {
        return NIL_VAL;
    }
    const char *path = path_str->chars;
    FILE *file = fopen(path, "rb");
    if (!file) {
        vm_runtime_error(ERROR_RUNTIME, "could not open '%s' for reading", path);
        return NIL_VAL;
    }

    buf_t contents;
    buf_init(&contents);
    char chunk[8192];
    size_t read_bytes;

    while ((read_bytes = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (buf_append(&contents, chunk, read_bytes) != 0) {
            buf_free(&contents);
            fclose(file);
            vm_runtime_error(ERROR_RUNTIME, "file too large '%s'", path);
            return NIL_VAL;
        }
    }
    if (ferror(file)) {
        buf_free(&contents);
        fclose(file);
        vm_runtime_error(ERROR_RUNTIME, "error reading '%s'", path);
        return NIL_VAL;
    }
    fclose(file);

    ObjString *result = object_string(contents.data ? contents.data : "", (int)contents.len);
    buf_free(&contents);
    return OBJ_VAL(result);
}

static Value native_write_file(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'write_file' expects a string as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    if (!is_obj_type(args[1], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'write_file' expects a string as second argument, received %s",
                         value_type_name(args[1]));
        return NIL_VAL;
    }
    ObjString *path_str = AS_STRING(args[0]);
    if (string_has_null(path_str, "write_file")) {
        return NIL_VAL;
    }
    const char *path = path_str->chars;
    ObjString *data = AS_STRING(args[1]);
    FILE *file = fopen(path, "wb");
    if (!file) {
        vm_runtime_error(ERROR_RUNTIME, "could not open '%s' for writing", path);
        return NIL_VAL;
    }

    if (data->length > 0) {
        size_t written = fwrite(data->chars, 1, (size_t)data->length, file);
        if (written < (size_t)data->length) {
            fclose(file);
            vm_runtime_error(ERROR_RUNTIME, "could not write all data to '%s'", path);
            return NIL_VAL;
        }
    }
    fclose(file);
    return NIL_VAL;
}

static Value native_append_file(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'append_file' expects a string as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    if (!is_obj_type(args[1], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'append_file' expects a string as second argument, received %s",
                         value_type_name(args[1]));
        return NIL_VAL;
    }
    ObjString *path_str = AS_STRING(args[0]);
    if (string_has_null(path_str, "append_file")) {
        return NIL_VAL;
    }
    const char *path = path_str->chars;
    ObjString *data = AS_STRING(args[1]);
    FILE *file = fopen(path, "ab");
    if (!file) {
        vm_runtime_error(ERROR_RUNTIME, "could not open '%s' for appending", path);
        return NIL_VAL;
    }

    if (data->length > 0) {
        size_t written = fwrite(data->chars, 1, (size_t)data->length, file);
        if (written < (size_t)data->length) {
            fclose(file);
            vm_runtime_error(ERROR_RUNTIME, "could not append all data to '%s'", path);
            return NIL_VAL;
        }
    }
    fclose(file);
    return NIL_VAL;
}

static Value native_open(int arg_count, Value *args)
{
    if (arg_count < 1 || arg_count > 2) {
        vm_runtime_error(ERROR_ARITY, "'open' expects 1 or 2 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'open' expects a string path, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjString *path_str = AS_STRING(args[0]);
    if (string_has_null(path_str, "open")) {
        return NIL_VAL;
    }
    const char *mode = "r";
    if (arg_count == 2) {
        if (!is_obj_type(args[1], OBJ_STRING)) {
            vm_runtime_error(ERROR_TYPE, "'open' expects a string mode, received %s",
                             value_type_name(args[1]));
            return NIL_VAL;
        }
        ObjString *mode_str = AS_STRING(args[1]);
        if (string_has_null(mode_str, "open")) {
            return NIL_VAL;
        }
        mode = mode_str->chars;
    }

    FILE *file = fopen(path_str->chars, mode);
    if (!file) {
        vm_runtime_error(ERROR_RUNTIME, "could not open '%s' with mode '%s'",
                         path_str->chars, mode);
        return NIL_VAL;
    }

    ObjFile *obj_file = object_file(file, path_str);
    return OBJ_VAL(obj_file);
}

static Value native_read_line(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_FILE)) {
        vm_runtime_error(ERROR_TYPE, "'read_line' expects a file handle, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjFile *file = AS_FILE(args[0]);
    if (file->is_closed || !file->stream) {
        vm_runtime_error(ERROR_RUNTIME, "cannot read from a closed file");
        return NIL_VAL;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, file->stream);
    if (n < 0) {
        free(line);
        return NIL_VAL;
    }

    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        n--;
    }
    line[n] = '\0';

    ObjString *res = object_string(line, (int)n);
    free(line);
    return OBJ_VAL(res);
}

static Value native_read(int arg_count, Value *args)
{
    if (arg_count < 1 || arg_count > 2) {
        vm_runtime_error(ERROR_ARITY, "'read' expects 1 or 2 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_FILE)) {
        vm_runtime_error(ERROR_TYPE, "'read' expects a file handle, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjFile *file = AS_FILE(args[0]);
    if (file->is_closed || !file->stream) {
        vm_runtime_error(ERROR_RUNTIME, "cannot read from a closed file");
        return NIL_VAL;
    }

    if (arg_count == 2) {
        int bytes = 0;
        if (!integer_argument(args[1], "bytes", "read", &bytes)) {
            return NIL_VAL;
        }
        if (bytes < 0 || bytes > 0x3fffffff) {
            vm_runtime_error(ERROR_VALUE, "'read' byte count out of valid range");
            return NIL_VAL;
        }
        if (bytes == 0) {
            return OBJ_VAL(object_string("", 0));
        }

        char *buf = malloc((size_t)bytes + 1);
        if (!buf) {
            vm_runtime_error(ERROR_RUNTIME, "out of memory in 'read'");
            return NIL_VAL;
        }
        size_t read_bytes = fread(buf, 1, (size_t)bytes, file->stream);
        if (ferror(file->stream)) {
            free(buf);
            vm_runtime_error(ERROR_RUNTIME, "error reading from file");
            return NIL_VAL;
        }
        buf[read_bytes] = '\0';
        ObjString *res = object_string(buf, (int)read_bytes);
        free(buf);
        return OBJ_VAL(res);
    }

    buf_t contents;
    buf_init(&contents);
    char chunk[8192];
    size_t read_bytes;

    while ((read_bytes = fread(chunk, 1, sizeof(chunk), file->stream)) > 0) {
        if (buf_append(&contents, chunk, read_bytes) != 0) {
            buf_free(&contents);
            vm_runtime_error(ERROR_RUNTIME, "file too large in 'read'");
            return NIL_VAL;
        }
    }
    if (ferror(file->stream)) {
        buf_free(&contents);
        vm_runtime_error(ERROR_RUNTIME, "error reading from file");
        return NIL_VAL;
    }
    ObjString *res = object_string(contents.data ? contents.data : "", (int)contents.len);
    buf_free(&contents);
    return OBJ_VAL(res);
}

static Value native_write(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_FILE)) {
        vm_runtime_error(ERROR_TYPE, "'write' expects a file handle as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    if (!is_obj_type(args[1], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'write' expects a string as second argument, received %s",
                         value_type_name(args[1]));
        return NIL_VAL;
    }
    ObjFile *file = AS_FILE(args[0]);
    if (file->is_closed || !file->stream) {
        vm_runtime_error(ERROR_RUNTIME, "cannot write to a closed file");
        return NIL_VAL;
    }
    ObjString *data = AS_STRING(args[1]);
    size_t written = 0;
    if (data->length > 0) {
        written = fwrite(data->chars, 1, (size_t)data->length, file->stream);
    }
    return INT_VAL((int64_t)written);
}

static Value native_close(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_FILE)) {
        vm_runtime_error(ERROR_TYPE, "'close' expects a file handle, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjFile *file = AS_FILE(args[0]);
    if (file->is_closed || !file->stream) {
        vm_runtime_error(ERROR_RUNTIME, "cannot close an already closed file");
        return NIL_VAL;
    }
    fclose(file->stream);
    file->stream = NULL;
    file->is_closed = true;
    return NIL_VAL;
}

static Value native_eof(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_FILE)) {
        vm_runtime_error(ERROR_TYPE, "'eof' expects a file handle, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjFile *file = AS_FILE(args[0]);
    if (file->is_closed || !file->stream) {
        vm_runtime_error(ERROR_RUNTIME, "cannot check eof on a closed file");
        return NIL_VAL;
    }
    return BOOL_VAL(feof(file->stream) != 0);
}

static uint64_t prng_state = 0x853c49e6748fea9bULL;

static inline uint64_t prng_next(void)
{
    uint64_t z = (prng_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static Value native_srand(int arg_count, Value *args)
{
    (void)arg_count;
    uint64_t seed = 0;
    if (args[0].type == VAL_INT) {
        seed = (uint64_t)AS_INT(args[0]);
    } else if (args[0].type == VAL_NUMBER) {
        seed = (uint64_t)AS_NUMBER(args[0]);
    } else {
        vm_runtime_error(ERROR_TYPE, "'srand' expects a number, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    prng_state = seed != 0 ? seed : 0x853c49e6748fea9bULL;
    return NIL_VAL;
}

static Value native_rand(int arg_count, Value *args)
{
    (void)arg_count;
    (void)args;
    double r = (double)(prng_next() >> 11) * (1.0 / 9007199254740992.0);
    return NUMBER_VAL(r);
}

static Value native_randint(int arg_count, Value *args)
{
    int64_t min_val = 0;
    int64_t max_val = 0;

    if (arg_count == 1) {
        if (!is_int(args[0])) {
            vm_runtime_error(ERROR_TYPE, "'randint' expects integer arguments, received %s",
                             value_type_name(args[0]));
            return NIL_VAL;
        }
        max_val = AS_INT(args[0]);
        min_val = 0;
    } else if (arg_count == 2) {
        if (!is_int(args[0]) || !is_int(args[1])) {
            vm_runtime_error(ERROR_TYPE, "'randint' expects integer arguments");
            return NIL_VAL;
        }
        min_val = AS_INT(args[0]);
        max_val = AS_INT(args[1]);
    } else {
        vm_runtime_error(ERROR_ARITY, "'randint' expects 1 or 2 arguments, received %d", arg_count);
        return NIL_VAL;
    }

    if (min_val > max_val) {
        vm_runtime_error(ERROR_VALUE, "'randint' min (%lld) cannot be greater than max (%lld)",
                         (long long)min_val, (long long)max_val);
        return NIL_VAL;
    }

    uint64_t range = (uint64_t)(max_val - min_val) + 1;
    uint64_t offset;
    if (range == 0) {
        offset = prng_next();
    } else {
        offset = prng_next() % range;
    }
    return INT_VAL(min_val + (int64_t)offset);
}

static Value native_to_json(int arg_count, Value *args)
{
    (void)arg_count;
    char error[256] = {0};
    ObjString *json = json_encode(args[0], error, sizeof(error));
    if (!json) {
        vm_runtime_error(ERROR_VALUE, "%s", error[0] ? error : "could not serialize to JSON");
        return NIL_VAL;
    }
    return OBJ_VAL(json);
}

static Value native_from_json(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'from_json' expects a string, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjString *str = AS_STRING(args[0]);
    char error[256] = {0};
    Value val = json_decode(str->chars, str->length, error, sizeof(error));
    if (error[0] != '\0') {
        vm_runtime_error(ERROR_VALUE, "%s", error);
        return NIL_VAL;
    }
    return val;
}

static Value native_regex_match(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING) || !is_obj_type(args[1], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'regex_match' expects two strings");
        return NIL_VAL;
    }
    ObjString *pat = AS_STRING(args[0]);
    ObjString *text = AS_STRING(args[1]);
    char error[256] = {0};
    bool matched = regex_match(pat->chars, text->chars, error, sizeof(error));
    if (error[0] != '\0') {
        vm_runtime_error(ERROR_VALUE, "%s", error);
        return NIL_VAL;
    }
    return BOOL_VAL(matched);
}

static Value native_regex_find(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING) || !is_obj_type(args[1], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'regex_find' expects two strings");
        return NIL_VAL;
    }
    ObjString *pat = AS_STRING(args[0]);
    ObjString *text = AS_STRING(args[1]);
    char error[256] = {0};
    ObjArray *matches = regex_find_all(pat->chars, text->chars, error, sizeof(error));
    if (error[0] != '\0') {
        vm_runtime_error(ERROR_VALUE, "%s", error);
        return NIL_VAL;
    }
    return OBJ_VAL(matches);
}

static Value native_regex_replace(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING) || !is_obj_type(args[1], OBJ_STRING) ||
        !is_obj_type(args[2], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'regex_replace' expects three strings");
        return NIL_VAL;
    }
    ObjString *pat = AS_STRING(args[0]);
    ObjString *repl = AS_STRING(args[1]);
    ObjString *text = AS_STRING(args[2]);
    char error[256] = {0};
    ObjString *res = regex_replace(pat->chars, repl->chars, text->chars, error, sizeof(error));
    if (error[0] != '\0') {
        vm_runtime_error(ERROR_VALUE, "%s", error);
        return NIL_VAL;
    }
    return OBJ_VAL(res);
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

void vm_runtime_error(ErrorType type, const char *fmt, ...)
{
    va_list ap;
    char message[512];
    int line = 0;
    int column = 0;
    const char *source = NULL;

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
            column = function->chunk.columns[offset];
        }
        source = function->source;
    }

    error_print(type, line, column, source, message);
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

    if (arg_count < function->min_arity || (!function->is_varargs && arg_count > function->arity)) {
        if (function->is_varargs) {
            vm_runtime_error(ERROR_ARITY, "the function '%s' expects at least %d argument(s) but received %d",
                             function->name ? function->name->chars : "anonymous",
                             function->min_arity, arg_count);
        } else if (function->min_arity == function->arity) {
            vm_runtime_error(ERROR_ARITY, "the function '%s' expects %d argument(s) but received %d",
                             function->name ? function->name->chars : "anonymous", function->arity,
                             arg_count);
        } else {
            vm_runtime_error(ERROR_ARITY, "the function '%s' expects between %d and %d arguments but received %d",
                             function->name ? function->name->chars : "anonymous",
                             function->min_arity, function->arity, arg_count);
        }
        return false;
    }
    if (vm.frame_count == FRAMES_MAX) {
        vm_runtime_error(ERROR_RECURSION, "too much call depth (possible infinite recursion)");
        return false;
    }

    if (function->is_varargs) {
        int fixed_passed = arg_count < function->arity ? arg_count : function->arity;
        int extra_count = arg_count > function->arity ? arg_count - function->arity : 0;
        ObjArray *rest = object_array();
        push(OBJ_VAL(rest));
        for (int i = 0; i < extra_count; i++) {
            array_push(rest, *(vm.stack_top - 1 - extra_count + i));
        }
        vm.stack_top -= (extra_count + 1);
        while (fixed_passed < function->arity) {
            push(NIL_VAL);
            fixed_passed++;
        }
        push(OBJ_VAL(rest));

        CallFrame *frame = &vm.frames[vm.frame_count++];
        frame->closure = closure;
        frame->ip = function->chunk.code;
        frame->slots = vm.stack_top - (function->arity + 1) - 1;
        frame->constants = function->chunk.constants;
        frame->argc = arg_count < function->arity ? arg_count : function->arity;
        return true;
    }

    int original_argc = arg_count;
    while (arg_count < function->arity) {
        push(NIL_VAL);
        arg_count++;
    }

    CallFrame *frame = &vm.frames[vm.frame_count++];

    frame->closure = closure;
    frame->ip = function->chunk.code;
    frame->slots = vm.stack_top - function->arity - 1;
    frame->constants = function->chunk.constants;
    frame->argc = original_argc;
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
                vm_runtime_error(ERROR_ARITY, "the function '%s' expects %d argument(s) but received %d",
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
    vm_runtime_error(ERROR_TYPE, "can only call functions, tried to call a %s",
                     value_type_name(callee));
    return false;
}

/* ------------------------------------------------------------------ */
/* Binary operations shared by the superinstructions                   */
/* ------------------------------------------------------------------ */

static inline bool compare_values(bool greater, Value a, Value b, bool *out)
{
    if (value_is_numeric(a) && value_is_numeric(b)) {
        *out = greater ? numeric_greater(a, b) : numeric_less(a, b);
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
    vm_runtime_error(ERROR_TYPE, "comparison operators expect two numbers or two strings "
                     "(received %s and %s)",
                     value_type_name(a), value_type_name(b));
    return false;
}

static inline bool arithmetic_values(uint8_t op, Value a, Value b, Value *out)
{
    if (!value_is_numeric(a) || !value_is_numeric(b)) {
        vm_runtime_error(ERROR_TYPE, "arithmetic operators expect numbers (received %s and %s)",
                         value_type_name(a), value_type_name(b));
        return false;
    }
    if (op == OP_DIVIDE || op == OP_MODULO) {
        double right = value_as_double(b);

        if (right == 0) {
            vm_runtime_error(ERROR_ZERO_DIVISION, "division by zero");
            return false;
        }
        if (op == OP_MODULO && a.type == VAL_INT && b.type == VAL_INT) {
            int64_t divisor = AS_INT(b);

            /* INT64_MIN % -1 overflows in C; the mathematical remainder is 0. */
            *out = INT_VAL(divisor == -1 ? 0 : AS_INT(a) % divisor);
            return true;
        }
        if (op == OP_DIVIDE) {
            *out = NUMBER_VAL(value_as_double(a) / right);
        } else {
            *out = NUMBER_VAL(fmod(value_as_double(a), right));
        }
        return true;
    }
    if (a.type == VAL_INT && b.type == VAL_INT) {
        int64_t result;
        bool fits;

        if (op == OP_SUBTRACT) {
            fits = !int_sub_overflow(AS_INT(a), AS_INT(b), &result);
        } else {
            fits = !int_mul_overflow(AS_INT(a), AS_INT(b), &result);
        }
        if (fits) {
            *out = INT_VAL(result);
            return true;
        }
    }
    double left = value_as_double(a);
    double right = value_as_double(b);

    switch (op) {
    case OP_ADD: *out = NUMBER_VAL(left + right); break;
    case OP_SUBTRACT: *out = NUMBER_VAL(left - right); break;
    case OP_MULTIPLY: *out = NUMBER_VAL(left * right); break;
    default: *out = NUMBER_VAL(fmod(left, right)); break;
    }
    return true;
}

/* `div`: truncating integer division. Both operands must be integers. */
static inline bool int_divide_values(Value a, Value b, Value *out)
{
    if (!is_int(a) || !is_int(b)) {
        vm_runtime_error(ERROR_TYPE, "'div' expects two integers, received %s and %s",
                         value_type_name(a), value_type_name(b));
        return false;
    }
    if (AS_INT(b) == 0) {
        vm_runtime_error(ERROR_ZERO_DIVISION, "division by zero");
        return false;
    }
    if (AS_INT(a) == INT64_MIN && AS_INT(b) == -1) {
        *out = NUMBER_VAL(TWO_POW_63);
        return true;
    }
    *out = INT_VAL(AS_INT(a) / AS_INT(b));
    return true;
}

/* Bitwise operators: integer-only, and shift counts are validated to avoid UB. */
static inline bool bitwise_values(uint8_t op, Value a, Value b, Value *out)
{
    if (!is_int(a) || !is_int(b)) {
        vm_runtime_error(ERROR_TYPE, "bitwise operators expect integers, received %s and %s",
                         value_type_name(a), value_type_name(b));
        return false;
    }
    int64_t left = AS_INT(a);
    int64_t right = AS_INT(b);

    switch (op) {
    case OP_BIT_AND: *out = INT_VAL(left & right); return true;
    case OP_BIT_OR: *out = INT_VAL(left | right); return true;
    case OP_BIT_XOR: *out = INT_VAL(left ^ right); return true;
    case OP_SHL:
    case OP_SHR: {
        if (right < 0 || right > 63) {
            vm_runtime_error(ERROR_VALUE, "the shift count must be between 0 and 63, received %lld",
                             (long long)right);
            return false;
        }
        uint64_t bits = (uint64_t)left;

        if (op == OP_SHL) {
            *out = INT_VAL((int64_t)(bits << (unsigned)right));
        } else {
            /* Arithmetic shift: shift the magnitude and restore the sign for
             * negative values, so the result is well defined for any input. */
            uint64_t shifted = left < 0 ? ~(~bits >> (unsigned)right) : bits >> (unsigned)right;

            *out = INT_VAL((int64_t)shifted);
        }
        return true;
    }
    }
    return false;
}

static inline bool add_values(Value a, Value b, Value *out)
{
    if (value_is_numeric(a) && value_is_numeric(b)) {
        if (a.type == VAL_INT && b.type == VAL_INT) {
            int64_t result;

            if (!int_add_overflow(AS_INT(a), AS_INT(b), &result)) {
                *out = INT_VAL(result);
                return true;
            }
        }
        *out = NUMBER_VAL(value_as_double(a) + value_as_double(b));
        return true;
    }
    if (is_obj_type(a, OBJ_STRING) && is_obj_type(b, OBJ_STRING)) {
        ObjString *result = object_string_concat(AS_STRING(a), AS_STRING(b));

        if (!result) {
            vm_runtime_error(ERROR_MEMORY, "could not concatenate the strings");
            return false;
        }
        *out = OBJ_VAL(result);
        return true;
    }
    vm_runtime_error(ERROR_TYPE, "the operands of '+' must be two numbers or two strings "
                     "(received %s and %s)",
                     value_type_name(a), value_type_name(b));
    return false;
}

/* ------------------------------------------------------------------ */
/* Execution loop                                                      */
/* ------------------------------------------------------------------ */

/* Validates an array/string index (an integer or an integral float) and
 * writes the position; reports the error and returns false otherwise. */
static bool index_to_position(Value index, int *out)
{
    if (is_int(index)) {
        int64_t integer = AS_INT(index);

        if (integer < 0 || integer > INT_MAX) {
            vm_runtime_error(ERROR_VALUE, "the index must be a non-negative integer, received %lld",
                             (long long)integer);
            return false;
        }
        *out = (int)integer;
        return true;
    }
    if (is_number(index)) {
        double raw = AS_NUMBER(index);

        if (raw < 0 || raw != floor(raw) || raw > INT_MAX) {
            vm_runtime_error(ERROR_VALUE, "the index must be a non-negative integer, received %g", raw);
            return false;
        }
        *out = (int)raw;
        return true;
    }
    vm_runtime_error(ERROR_TYPE, "the index must be a number, received a %s",
                     value_type_name(index));
    return false;
}

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
        constants = frame->constants;                                     \
    } while (0)

#if defined(__GNUC__) || defined(__clang__)
#define SPARK_COMPUTED_GOTO 1
#endif

#ifdef SPARK_COMPUTED_GOTO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Woverride-init"
#define DISPATCH() goto *dispatch_table[*ip]
#define TARGET(op) op_##op
#else
#define DISPATCH() continue
#define TARGET(op) case op
#endif

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
    uint8_t instruction;

    LOAD_FRAME();

#ifdef SPARK_COMPUTED_GOTO
    static const void *const dispatch_table[256] = {
        [0 ... 255] = &&cold_dispatch,
        [OP_CONSTANT] = &&op_OP_CONSTANT,
        [OP_NIL] = &&op_OP_NIL,
        [OP_TRUE] = &&op_OP_TRUE,
        [OP_FALSE] = &&op_OP_FALSE,
        [OP_POP] = &&op_OP_POP,
        [OP_GET_LOCAL] = &&op_OP_GET_LOCAL,
        [OP_SET_LOCAL] = &&op_OP_SET_LOCAL,
        [OP_GET_GLOBAL] = &&op_OP_GET_GLOBAL,
        [OP_ADD] = &&op_OP_ADD,
        [OP_SUBTRACT] = &&op_OP_SUBTRACT,
        [OP_MULTIPLY] = &&op_OP_MULTIPLY,
        [OP_LESS] = &&op_OP_LESS,
        [OP_GREATER] = &&op_OP_GREATER,
        [OP_ADD_LOCAL_CONST] = &&op_OP_ADD_LOCAL_CONST,
        [OP_SUBTRACT_LOCAL_CONST] = &&op_OP_SUBTRACT_LOCAL_CONST,
        [OP_MULTIPLY_LOCAL_CONST] = &&op_OP_MULTIPLY_LOCAL_CONST,
        [OP_LESS_LOCAL_CONST] = &&op_OP_LESS_LOCAL_CONST,
        [OP_GREATER_LOCAL_CONST] = &&op_OP_GREATER_LOCAL_CONST,
        [OP_JUMP] = &&op_OP_JUMP,
        [OP_JUMP_IF_FALSE] = &&op_OP_JUMP_IF_FALSE,
        [OP_JUMP_IF_FALSE_POP] = &&op_OP_JUMP_IF_FALSE_POP,
        [OP_JUMP_IF_NOT_LESS_LOCAL_CONST] = &&op_OP_JUMP_IF_NOT_LESS_LOCAL_CONST,
        [OP_JUMP_IF_NOT_GREATER_LOCAL_CONST] = &&op_OP_JUMP_IF_NOT_GREATER_LOCAL_CONST,
        [OP_LOOP] = &&op_OP_LOOP,
        [OP_DEFAULT_ARG] = &&op_OP_DEFAULT_ARG,
        [OP_CALL] = &&op_OP_CALL,
        [OP_TAIL_CALL] = &&op_OP_TAIL_CALL,
        [OP_RETURN_LOCAL] = &&op_OP_RETURN_LOCAL,
        [OP_RETURN] = &&op_OP_RETURN,
        [OP_RETURN_ADD] = &&op_OP_RETURN_ADD,
        [OP_CALL_SELF_SUB_LOCAL_CONST] = &&op_OP_CALL_SELF_SUB_LOCAL_CONST,
        [OP_SET_GLOBAL] = &&op_OP_SET_GLOBAL,
        [OP_GET_UPVALUE] = &&op_OP_GET_UPVALUE,
        [OP_SET_UPVALUE] = &&op_OP_SET_UPVALUE,
        [OP_INDEX] = &&op_OP_INDEX,
        [OP_INDEX_SET] = &&op_OP_INDEX_SET,
        [OP_DUP] = &&op_OP_DUP,
    };
    DISPATCH();
#else
    for (;;) {
        switch (*ip) {
#endif
        TARGET(OP_CONSTANT):
            *sp++ = constants[U16_AT(ip + 1)];
            ip += 3;
            DISPATCH();

        TARGET(OP_NIL):
            *sp++ = NIL_VAL;
            ip += 1;
            DISPATCH();

        TARGET(OP_TRUE):
            *sp++ = BOOL_VAL(true);
            ip += 1;
            DISPATCH();

        TARGET(OP_FALSE):
            *sp++ = BOOL_VAL(false);
            ip += 1;
            DISPATCH();

        TARGET(OP_POP):
            sp--;
            ip += 1;
            DISPATCH();

        TARGET(OP_GET_LOCAL):
            *sp++ = slots[ip[1]];
            ip += 2;
            DISPATCH();

        TARGET(OP_SET_LOCAL):
            slots[ip[1]] = *--sp;
            ip += 2;
            DISPATCH();

        TARGET(OP_GET_GLOBAL): {
            GlobalSlot *global = &vm.global_slots[U16_AT(ip + 1)];

            if (!global->defined) {
                goto cold;
            }
            *sp++ = global->value;
            ip += 3;
            DISPATCH();
        }

        TARGET(OP_ADD): {
            Value a = sp[-2];
            Value b = sp[-1];

            if (a.type == VAL_INT && b.type == VAL_INT) {
                int64_t result;

                if (!int_add_overflow(AS_INT(a), AS_INT(b), &result)) {
                    sp--;
                    sp[-1] = INT_VAL(result);
                    ip += 1;
                    DISPATCH();
                }
                goto cold; /* overflow promotes to a double */
            }
            if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                sp--;
                sp[-1] = NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b));
                ip += 1;
                DISPATCH();
            }
            if (!numeric_fast(a) || !numeric_fast(b)) {
                goto cold;
            }
            sp--;
            sp[-1] = NUMBER_VAL(value_as_double(a) + value_as_double(b));
            ip += 1;
            DISPATCH();
        }

        TARGET(OP_SUBTRACT): {
            Value a = sp[-2];
            Value b = sp[-1];

            if (a.type == VAL_INT && b.type == VAL_INT) {
                int64_t result;

                if (!int_sub_overflow(AS_INT(a), AS_INT(b), &result)) {
                    sp--;
                    sp[-1] = INT_VAL(result);
                    ip += 1;
                    DISPATCH();
                }
                goto cold;
            }
            if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                sp--;
                sp[-1] = NUMBER_VAL(AS_NUMBER(a) - AS_NUMBER(b));
                ip += 1;
                DISPATCH();
            }
            if (!numeric_fast(a) || !numeric_fast(b)) {
                goto cold;
            }
            sp--;
            sp[-1] = NUMBER_VAL(value_as_double(a) - value_as_double(b));
            ip += 1;
            DISPATCH();
        }

        TARGET(OP_MULTIPLY): {
            Value a = sp[-2];
            Value b = sp[-1];

            if (a.type == VAL_INT && b.type == VAL_INT) {
                int64_t result;

                if (!int_mul_overflow(AS_INT(a), AS_INT(b), &result)) {
                    sp--;
                    sp[-1] = INT_VAL(result);
                    ip += 1;
                    DISPATCH();
                }
                goto cold;
            }
            if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                sp--;
                sp[-1] = NUMBER_VAL(AS_NUMBER(a) * AS_NUMBER(b));
                ip += 1;
                DISPATCH();
            }
            if (!numeric_fast(a) || !numeric_fast(b)) {
                goto cold;
            }
            sp--;
            sp[-1] = NUMBER_VAL(value_as_double(a) * value_as_double(b));
            ip += 1;
            DISPATCH();
        }

        TARGET(OP_LESS): {
            Value a = sp[-2];
            Value b = sp[-1];

            if (a.type == VAL_INT && b.type == VAL_INT) {
                sp--;
                sp[-1] = BOOL_VAL(AS_INT(a) < AS_INT(b));
                ip += 1;
                DISPATCH();
            }
            if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                sp--;
                sp[-1] = BOOL_VAL(AS_NUMBER(a) < AS_NUMBER(b));
                ip += 1;
                DISPATCH();
            }
            if (!numeric_fast(a) || !numeric_fast(b)) {
                goto cold;
            }
            sp--;
            sp[-1] = BOOL_VAL(value_as_double(a) < value_as_double(b));
            ip += 1;
            DISPATCH();
        }

        TARGET(OP_GREATER): {
            Value a = sp[-2];
            Value b = sp[-1];

            if (a.type == VAL_INT && b.type == VAL_INT) {
                sp--;
                sp[-1] = BOOL_VAL(AS_INT(a) > AS_INT(b));
                ip += 1;
                DISPATCH();
            }
            if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                sp--;
                sp[-1] = BOOL_VAL(AS_NUMBER(a) > AS_NUMBER(b));
                ip += 1;
                DISPATCH();
            }
            if (!numeric_fast(a) || !numeric_fast(b)) {
                goto cold;
            }
            sp--;
            sp[-1] = BOOL_VAL(value_as_double(a) > value_as_double(b));
            ip += 1;
            DISPATCH();
        }

        TARGET(OP_ADD_LOCAL_CONST): {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];

            if (a.type == VAL_INT && b.type == VAL_INT) {
                int64_t result;

                if (!int_add_overflow(AS_INT(a), AS_INT(b), &result)) {
                    *sp++ = INT_VAL(result);
                    ip += 4;
                    DISPATCH();
                }
                goto cold;
            }
            if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                *sp++ = NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b));
                ip += 4;
                DISPATCH();
            }
            if (!numeric_fast(a) || !numeric_fast(b)) {
                goto cold;
            }
            *sp++ = NUMBER_VAL(value_as_double(a) + value_as_double(b));
            ip += 4;
            DISPATCH();
        }

        TARGET(OP_SUBTRACT_LOCAL_CONST): {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];

            if (a.type == VAL_INT && b.type == VAL_INT) {
                int64_t result;

                if (!int_sub_overflow(AS_INT(a), AS_INT(b), &result)) {
                    *sp++ = INT_VAL(result);
                    ip += 4;
                    DISPATCH();
                }
                goto cold;
            }
            if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                *sp++ = NUMBER_VAL(AS_NUMBER(a) - AS_NUMBER(b));
                ip += 4;
                DISPATCH();
            }
            if (!numeric_fast(a) || !numeric_fast(b)) {
                goto cold;
            }
            *sp++ = NUMBER_VAL(value_as_double(a) - value_as_double(b));
            ip += 4;
            DISPATCH();
        }

        TARGET(OP_MULTIPLY_LOCAL_CONST): {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];

            if (a.type == VAL_INT && b.type == VAL_INT) {
                int64_t result;

                if (!int_mul_overflow(AS_INT(a), AS_INT(b), &result)) {
                    *sp++ = INT_VAL(result);
                    ip += 4;
                    DISPATCH();
                }
                goto cold;
            }
            if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                *sp++ = NUMBER_VAL(AS_NUMBER(a) * AS_NUMBER(b));
                ip += 4;
                DISPATCH();
            }
            if (!numeric_fast(a) || !numeric_fast(b)) {
                goto cold;
            }
            *sp++ = NUMBER_VAL(value_as_double(a) * value_as_double(b));
            ip += 4;
            DISPATCH();
        }

        TARGET(OP_LESS_LOCAL_CONST): {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];

            if (a.type == VAL_INT && b.type == VAL_INT) {
                *sp++ = BOOL_VAL(AS_INT(a) < AS_INT(b));
                ip += 4;
                DISPATCH();
            }
            if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                *sp++ = BOOL_VAL(AS_NUMBER(a) < AS_NUMBER(b));
                ip += 4;
                DISPATCH();
            }
            if (!numeric_fast(a) || !numeric_fast(b)) {
                goto cold;
            }
            *sp++ = BOOL_VAL(value_as_double(a) < value_as_double(b));
            ip += 4;
            DISPATCH();
        }

        TARGET(OP_GREATER_LOCAL_CONST): {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];

            if (a.type == VAL_INT && b.type == VAL_INT) {
                *sp++ = BOOL_VAL(AS_INT(a) > AS_INT(b));
                ip += 4;
                DISPATCH();
            }
            if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                *sp++ = BOOL_VAL(AS_NUMBER(a) > AS_NUMBER(b));
                ip += 4;
                DISPATCH();
            }
            if (!numeric_fast(a) || !numeric_fast(b)) {
                goto cold;
            }
            *sp++ = BOOL_VAL(value_as_double(a) > value_as_double(b));
            ip += 4;
            DISPATCH();
        }

        TARGET(OP_JUMP):
            ip += 3 + U16_AT(ip + 1);
            DISPATCH();

        TARGET(OP_JUMP_IF_FALSE):
            if (value_is_falsey(sp[-1])) {
                ip += 3 + U16_AT(ip + 1);
            } else {
                ip += 3;
            }
            DISPATCH();

        TARGET(OP_JUMP_IF_FALSE_POP):
            if (value_is_falsey(*--sp)) {
                ip += 3 + U16_AT(ip + 1);
            } else {
                ip += 3;
            }
            DISPATCH();

        TARGET(OP_JUMP_IF_NOT_LESS_LOCAL_CONST): {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];
            bool less;

            if (a.type == VAL_INT && b.type == VAL_INT) {
                less = AS_INT(a) < AS_INT(b);
            } else if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                less = AS_NUMBER(a) < AS_NUMBER(b);
            } else if (numeric_fast(a) && numeric_fast(b)) {
                less = value_as_double(a) < value_as_double(b);
            } else {
                goto cold;
            }
            if (less) {
                ip += 6;
            } else {
                ip += 6 + U16_AT(ip + 4);
            }
            DISPATCH();
        }

        TARGET(OP_JUMP_IF_NOT_GREATER_LOCAL_CONST): {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];
            bool greater;

            if (a.type == VAL_INT && b.type == VAL_INT) {
                greater = AS_INT(a) > AS_INT(b);
            } else if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                greater = AS_NUMBER(a) > AS_NUMBER(b);
            } else if (numeric_fast(a) && numeric_fast(b)) {
                greater = value_as_double(a) > value_as_double(b);
            } else {
                goto cold;
            }
            if (greater) {
                ip += 6;
            } else {
                ip += 6 + U16_AT(ip + 4);
            }
            DISPATCH();
        }

        TARGET(OP_LOOP):
            ip += 3 - U16_AT(ip + 1);
            DISPATCH();

        TARGET(OP_DEFAULT_ARG): {
            uint8_t slot = ip[1];
            uint16_t offset = U16_AT(ip + 2);
            if (frame->argc >= slot) {
                ip += 4 + offset;
            } else {
                ip += 4;
            }
            DISPATCH();
        }

        TARGET(OP_CALL): {
            int arg_count = ip[1];
            Value callee = sp[-1 - arg_count];

            /* Fast path: a closure or native called with the right number of arguments.
             * Everything else (arity errors, non-callables) goes through the general path. */
            if (callee.type == VAL_OBJ) {
                Obj *obj = AS_OBJ(callee);
                if (obj->type == OBJ_CLOSURE) {
                    ObjClosure *closure = (ObjClosure *)obj;
                    ObjFunction *function = closure->function;

                    if (!function->is_varargs && arg_count >= function->min_arity &&
                        arg_count <= function->arity && frame < &vm.frames[FRAMES_MAX - 1]) {
                        int original_argc = arg_count;
                        while (arg_count < function->arity) {
                            *sp++ = NIL_VAL;
                            arg_count++;
                        }
                        frame->ip = ip + 2;
                        frame++;
                        frame->closure = closure;
                        frame->ip = function->chunk.code;
                        frame->slots = sp - function->arity - 1;
                        frame->constants = function->chunk.constants;
                        frame->argc = original_argc;
                        ip = function->chunk.code;
                        slots = frame->slots;
                        constants = frame->constants;
                        DISPATCH();
                    }
                } else if (obj->type == OBJ_NATIVE) {
                    ObjNative *native = (ObjNative *)obj;
                    if (native->arity >= 0 && arg_count == native->arity) {
                        frame->ip = ip + 2;
                        vm.frame_count = (int)(frame - vm.frames) + 1;
                        Value *native_base = sp - arg_count - 1;
                        vm.stack_top = sp;
                        Value result = native->function(arg_count, native_base + 1);
                        if (__builtin_expect(!vm.had_error, 1)) {
                            *native_base = result;
                            sp = native_base + 1;
                            ip += 2;
                            DISPATCH();
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }
            }
            goto cold;
        }

        TARGET(OP_TAIL_CALL): {
            int arg_count = ip[1];
            Value callee = sp[-1 - arg_count];

            if (callee.type == VAL_OBJ) {
                Obj *obj = AS_OBJ(callee);
                if (obj->type == OBJ_CLOSURE) {
                    ObjClosure *closure = (ObjClosure *)obj;
                    ObjFunction *function = closure->function;

                    if (!function->is_varargs && arg_count >= function->min_arity &&
                        arg_count <= function->arity) {
                        if (__builtin_expect(vm.open_upvalues != NULL, 0)) {
                            close_upvalues(slots);
                        }
                        int original_argc = arg_count;
                        memmove(slots, sp - arg_count - 1, (size_t)(arg_count + 1) * sizeof(Value));
                        while (arg_count < function->arity) {
                            slots[arg_count + 1] = NIL_VAL;
                            arg_count++;
                        }
                        sp = slots + function->arity + 1;
                        frame->closure = closure;
                        frame->ip = function->chunk.code;
                        frame->constants = function->chunk.constants;
                        frame->argc = original_argc;
                        ip = function->chunk.code;
                        constants = frame->constants;
                        DISPATCH();
                    }
                } else if (obj->type == OBJ_NATIVE) {
                    ObjNative *native = (ObjNative *)obj;
                    if (native->arity >= 0 && arg_count == native->arity &&
                        frame != &vm.frames[base_frame]) {
                        Value *native_base = sp - arg_count - 1;
                        vm.frame_count = (int)(frame - vm.frames) + 1;
                        vm.stack_top = sp;
                        Value result = native->function(arg_count, native_base + 1);
                        if (__builtin_expect(!vm.had_error, 1)) {
                            if (__builtin_expect(vm.open_upvalues != NULL, 0)) {
                                close_upvalues(slots);
                            }
                            sp = slots;
                            *sp++ = result;
                            frame--;
                            ip = frame->ip;
                            slots = frame->slots;
                            constants = frame->constants;
                            DISPATCH();
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }
            }
            goto cold;
        }

        TARGET(OP_RETURN_LOCAL):
        TARGET(OP_RETURN): {
            Value result;

            /* Returning from the outermost frame of this run() (or of the
             * program) is handled by the general path. */
            if (__builtin_expect(frame == &vm.frames[base_frame], 0)) {
                goto cold;
            }
            result = *ip == OP_RETURN ? sp[-1] : slots[ip[1]];
            if (__builtin_expect(vm.open_upvalues != NULL, 0)) {
                close_upvalues(slots);
            }
            sp = slots;
            *sp++ = result;
            frame--;
            ip = frame->ip;
            slots = frame->slots;
            constants = frame->constants;
            DISPATCH();
        }

        TARGET(OP_CALL_SELF_SUB_LOCAL_CONST): {
            Value a = slots[ip[1]];
            Value b = constants[U16_AT(ip + 2)];
            Value arg;

            if (__builtin_expect(a.type == VAL_INT && b.type == VAL_INT, 1)) {
                int64_t result;
                if (__builtin_expect(!int_sub_overflow(AS_INT(a), AS_INT(b), &result), 1)) {
                    arg = INT_VAL(result);
                    goto do_call_self;
                }
            } else if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                arg = NUMBER_VAL(AS_NUMBER(a) - AS_NUMBER(b));
                goto do_call_self;
            } else if (numeric_fast(a) && numeric_fast(b)) {
                arg = NUMBER_VAL(value_as_double(a) - value_as_double(b));
                goto do_call_self;
            }
            goto cold;

        do_call_self:
            if (__builtin_expect(frame >= &vm.frames[FRAMES_MAX - 1], 0)) {
                goto cold;
            }
            frame->ip = ip + 4;
            CallFrame *prev = frame;
            frame++;
            frame->closure = prev->closure;
            frame->ip = frame->closure->function->chunk.code;
            frame->slots = sp;
            frame->constants = constants;
            sp[0] = prev->slots[0];
            sp[1] = arg;
            sp += 2;
            ip = frame->ip;
            slots = sp - 2;
            DISPATCH();
        }

        TARGET(OP_RETURN_ADD): {
            Value a = sp[-2];
            Value b = sp[-1];
            Value result;

            if (__builtin_expect(a.type == VAL_INT && b.type == VAL_INT, 1)) {
                int64_t sum;
                if (__builtin_expect(!int_add_overflow(AS_INT(a), AS_INT(b), &sum), 1)) {
                    result = INT_VAL(sum);
                    goto do_ret_add;
                }
            } else if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                result = NUMBER_VAL(AS_NUMBER(a) + AS_NUMBER(b));
                goto do_ret_add;
            } else if (numeric_fast(a) && numeric_fast(b)) {
                result = NUMBER_VAL(value_as_double(a) + value_as_double(b));
                goto do_ret_add;
            }
            goto cold;

        do_ret_add:
            if (__builtin_expect(frame == &vm.frames[base_frame], 0)) {
                goto cold;
            }
            if (__builtin_expect(vm.open_upvalues != NULL, 0)) {
                close_upvalues(slots);
            }
            sp = slots;
            *sp++ = result;
            frame--;
            ip = frame->ip;
            slots = frame->slots;
            constants = frame->constants;
            DISPATCH();
        }

        TARGET(OP_SET_GLOBAL): {
            GlobalSlot *global = &vm.global_slots[U16_AT(ip + 1)];

            if (!global->defined || global->is_const) {
                goto cold;
            }
            global->value = *--sp;
            ip += 3;
            DISPATCH();
        }

        TARGET(OP_GET_UPVALUE):
            *sp++ = *frame->closure->upvalues[ip[1]]->location;
            ip += 2;
            DISPATCH();

        TARGET(OP_SET_UPVALUE):
            *frame->closure->upvalues[ip[1]]->location = *--sp;
            ip += 2;
            DISPATCH();

        TARGET(OP_INDEX): {
            Value index = sp[-1];
            Value object = sp[-2];

            if (is_obj_type(object, OBJ_ARRAY) && index.type == VAL_INT) {
                ObjArray *array = AS_ARRAY(object);
                int64_t pos = index.as.integer;
                if (pos >= 0 && pos < array->count) {
                    sp[-2] = array->items[pos];
                    sp--;
                    ip += 1;
                    DISPATCH();
                }
            } else if (is_obj_type(object, OBJ_MAP)) {
                Value stored;
                if (map_get(AS_MAP(object), index, &stored)) {
                    sp[-2] = stored;
                } else {
                    sp[-2] = NIL_VAL;
                }
                sp--;
                ip += 1;
                DISPATCH();
            }
            goto cold;
        }

        TARGET(OP_INDEX_SET): {
            Value value = sp[-1];
            Value index = sp[-2];
            Value object = sp[-3];

            if (is_obj_type(object, OBJ_ARRAY) && index.type == VAL_INT) {
                ObjArray *array = AS_ARRAY(object);
                int64_t pos = index.as.integer;
                if (pos >= 0 && pos < array->count) {
                    array->items[pos] = value;
                    sp -= 3;
                    ip += 1;
                    DISPATCH();
                }
            } else if (is_obj_type(object, OBJ_MAP)) {
                map_set(AS_MAP(object), index, value);
                sp -= 3;
                ip += 1;
                DISPATCH();
            }
            goto cold;
        }

        TARGET(OP_DUP):
            *sp = sp[-1];
            sp++;
            ip += 1;
            DISPATCH();

#ifdef SPARK_COMPUTED_GOTO
cold_dispatch:
        goto cold;
#else
        default:
            break;
        }
#endif

    cold:
        vm.frame_count = (int)(frame - vm.frames) + 1;
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
                vm_runtime_error(ERROR_NAME, "global variable '%s' not defined", global->name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            push(global->value);
            break;
        }

        case OP_DEFINE_GLOBAL: {
            uint16_t slot = READ_U16();
            GlobalSlot *global = &vm.global_slots[slot];

            if (global->is_const && global->defined) {
                vm_runtime_error(ERROR_CONST, "cannot redefine constant '%s'", global->name->chars);
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
                vm_runtime_error(ERROR_CONST, "cannot redefine constant '%s'", global->name->chars);
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
                vm_runtime_error(ERROR_NAME, "global variable '%s' not defined", global->name->chars);
                return INTERPRET_RUNTIME_ERROR;
            }
            if (global->is_const) {
                vm_runtime_error(ERROR_CONST, "cannot assign to constant '%s'", global->name->chars);
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
                ObjArray *array = AS_ARRAY(object);
                int position;

                if (!index_to_position(index, &position)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (position >= array->count) {
                    vm_runtime_error(ERROR_INDEX, "index out of range: %d (size %d)", position,
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
                ObjString *string = AS_STRING(object);
                int position;
                int byte_index;
                int char_length;

                if (!index_to_position(index, &position)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                byte_index = utf8_byte_offset(string, position);

                if (byte_index < 0) {
                    vm_runtime_error(ERROR_INDEX, "index out of range: %d (string of %d characters)",
                                     position, utf8_length(string->chars, string->length));
                    return INTERPRET_RUNTIME_ERROR;
                }
                char_length = utf8_char_length(string, byte_index);

                result = OBJ_VAL(object_string(string->chars + byte_index, char_length));
            } else {
                vm_runtime_error(ERROR_TYPE, "only an array, a map or a string can be indexed, "
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
                ObjArray *array = AS_ARRAY(object);
                int position;

                if (!index_to_position(index, &position)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (position >= array->count) {
                    vm_runtime_error(ERROR_INDEX, "index out of range: %d (size %d)", position,
                                     array->count);
                    return INTERPRET_RUNTIME_ERROR;
                }
                array->items[position] = value;
            } else if (is_obj_type(object, OBJ_MAP)) {
                map_set(AS_MAP(object), index, value);
            } else {
                vm_runtime_error(ERROR_TYPE, "can only assign to an array or a map, tried to assign to a %s",
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
            if (count > 0) {
                array->items = mem_realloc(NULL, 0, sizeof(Value) * (size_t)count);
                array->capacity = count;
                array->count = count;
                memcpy(array->items, items, sizeof(Value) * (size_t)count);
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
            vm_runtime_error(ERROR_TYPE, "cannot iterate over a %s", value_type_name(iterable));
            return INTERPRET_RUNTIME_ERROR;
        }

        case OP_LEN: {
            Value value = peek(0);
            int64_t length;

            if (is_obj_type(value, OBJ_ARRAY)) {
                length = AS_ARRAY(value)->count;
            } else if (is_obj_type(value, OBJ_MAP)) {
                length = AS_MAP(value)->count;
            } else if (is_obj_type(value, OBJ_STRING)) {
                length = utf8_length(AS_STRING(value)->chars, AS_STRING(value)->length);
            } else {
                vm_runtime_error(ERROR_TYPE, "'len' expects an array, a map or a string, received %s",
                                 value_type_name(value));
                return INTERPRET_RUNTIME_ERROR;
            }
            /* Leaves the original value and pushes its length: the compiler uses it
             * to initialize the counter of a for-in. */
            push(INT_VAL(length));
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
            Value result;

            if (!arithmetic_values(op, frame->slots[slot], b, &result)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            push(result);
            break;
        }

        case OP_SUBTRACT:
        case OP_MULTIPLY:
        case OP_DIVIDE:
        case OP_MODULO: {
            Value b = pop();
            Value a = pop();
            Value result;

            if (!arithmetic_values(instruction, a, b, &result)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            push(result);
            break;
        }

        case OP_INT_DIVIDE: {
            Value b = pop();
            Value a = pop();
            Value result;

            if (!int_divide_values(a, b, &result)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            push(result);
            break;
        }

        case OP_BIT_AND:
        case OP_BIT_OR:
        case OP_BIT_XOR:
        case OP_SHL:
        case OP_SHR: {
            Value b = pop();
            Value a = pop();
            Value result;

            if (!bitwise_values(instruction, a, b, &result)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            push(result);
            break;
        }

        case OP_NOT:
            push(BOOL_VAL(value_is_falsey(pop())));
            break;

        case OP_NEGATE: {
            Value value = pop();

            if (value.type == VAL_INT) {
                int64_t integer = AS_INT(value);

                push(integer == INT64_MIN ? NUMBER_VAL(TWO_POW_63) : INT_VAL(-integer));
            } else if (value.type == VAL_NUMBER) {
                push(NUMBER_VAL(-AS_NUMBER(value)));
            } else {
                vm_runtime_error(ERROR_TYPE, "negation expects a number, received %s",
                                 value_type_name(value));
                return INTERPRET_RUNTIME_ERROR;
            }
            break;
        }

        case OP_BIT_NOT: {
            Value value = pop();

            if (!is_int(value)) {
                vm_runtime_error(ERROR_TYPE, "the bitwise complement expects an integer, received %s",
                                 value_type_name(value));
                return INTERPRET_RUNTIME_ERROR;
            }
            push(INT_VAL(~AS_INT(value)));
            break;
        }

        case OP_PRINT_REPL: {
            Value val = peek(0);
            value_print(stdout, val);
            fputc('\n', stdout);
            fflush(stdout);
            int slot = vm_global_slot(object_string("_", 1));
            if (slot >= 0) {
                vm.global_slots[slot].value = val;
                vm.global_slots[slot].defined = true;
            }
            pop();
            break;
        }

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

        case OP_DEFAULT_ARG: {
            uint8_t slot = READ_BYTE();
            uint16_t offset = READ_U16();
            if (frame->argc >= slot) {
                frame->ip += offset;
            }
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

        case OP_TAIL_CALL: {
            int arg_count = READ_BYTE();
            Value callee = peek(arg_count);
            if (is_obj_type(callee, OBJ_CLOSURE)) {
                ObjClosure *closure = (ObjClosure *)AS_OBJ(callee);
                ObjFunction *function = closure->function;
                if (arg_count < function->min_arity || (!function->is_varargs && arg_count > function->arity)) {
                    if (function->is_varargs) {
                        vm_runtime_error(ERROR_ARITY, "the function '%s' expects at least %d argument(s) but received %d",
                                         function->name ? function->name->chars : "anonymous", function->min_arity,
                                         arg_count);
                    } else if (function->min_arity == function->arity) {
                        vm_runtime_error(ERROR_ARITY, "the function '%s' expects %d argument(s) but received %d",
                                         function->name ? function->name->chars : "anonymous", function->arity,
                                         arg_count);
                    } else {
                        vm_runtime_error(ERROR_ARITY, "the function '%s' expects between %d and %d arguments but received %d",
                                         function->name ? function->name->chars : "anonymous",
                                         function->min_arity, function->arity, arg_count);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                close_upvalues(frame->slots);
                if (function->is_varargs) {
                    int fixed_passed = arg_count < function->arity ? arg_count : function->arity;
                    int extra_count = arg_count > function->arity ? arg_count - function->arity : 0;
                    ObjArray *rest = object_array();
                    push(OBJ_VAL(rest));
                    for (int i = 0; i < extra_count; i++) {
                        array_push(rest, *(vm.stack_top - 1 - extra_count + i));
                    }
                    vm.stack_top -= (extra_count + 1);
                    memmove(frame->slots, vm.stack_top - fixed_passed - 1, (size_t)(fixed_passed + 1) * sizeof(Value));
                    for (int i = fixed_passed; i < function->arity; i++) {
                        frame->slots[i + 1] = NIL_VAL;
                    }
                    frame->slots[function->arity + 1] = OBJ_VAL(rest);
                    vm.stack_top = frame->slots + function->arity + 2;
                    frame->closure = closure;
                    frame->ip = function->chunk.code;
                    frame->constants = function->chunk.constants;
                    frame->argc = fixed_passed;
                    break;
                }
                int original_argc = arg_count;
                memmove(frame->slots, vm.stack_top - arg_count - 1, (size_t)(arg_count + 1) * sizeof(Value));
                while (arg_count < function->arity) {
                    frame->slots[arg_count + 1] = NIL_VAL;
                    arg_count++;
                }
                vm.stack_top = frame->slots + function->arity + 1;
                frame->closure = closure;
                frame->ip = function->chunk.code;
                frame->constants = function->chunk.constants;
                frame->argc = original_argc;
                break;
            } else if (is_obj_type(callee, OBJ_NATIVE)) {
                ObjNative *native = (ObjNative *)AS_OBJ(callee);
                if (native->arity >= 0 && arg_count != native->arity) {
                    vm_runtime_error(ERROR_ARITY, "the function '%s' expects %d argument(s) but received %d",
                                     native->name->chars, native->arity, arg_count);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value *native_base = vm.stack_top - arg_count - 1;
                Value result = native->function(arg_count, native_base + 1);
                if (vm.had_error) {
                    return INTERPRET_RUNTIME_ERROR;
                }
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
            vm_runtime_error(ERROR_TYPE, "can only call functions, tried to call a %s",
                             value_type_name(callee));
            return INTERPRET_RUNTIME_ERROR;
        }

        case OP_ARRAY_PUSH: {
            Value item = pop();
            Value array = peek(0);
            array_push(AS_ARRAY(array), item);
            break;
        }

        case OP_ARRAY_EXTEND: {
            Value src = pop();
            Value dest = peek(0);
            if (!is_obj_type(src, OBJ_ARRAY)) {
                vm_runtime_error(ERROR_TYPE, "spread operator '...' requires an array, received %s",
                                 value_type_name(src));
                return INTERPRET_RUNTIME_ERROR;
            }
            ObjArray *src_arr = AS_ARRAY(src);
            for (int i = 0; i < src_arr->count; i++) {
                array_push(AS_ARRAY(dest), src_arr->items[i]);
            }
            break;
        }

        case OP_CALL_SPREAD: {
            Value args = pop();
            if (!is_obj_type(args, OBJ_ARRAY)) {
                vm_runtime_error(ERROR_TYPE, "spread operator '...' requires an array, received %s",
                                 value_type_name(args));
                return INTERPRET_RUNTIME_ERROR;
            }
            ObjArray *arr = AS_ARRAY(args);
            int count = arr->count;
            if (count >= STACK_MAX - (int)(vm.stack_top - vm.stack)) {
                vm_runtime_error(ERROR_MEMORY, "stack overflow");
                return INTERPRET_RUNTIME_ERROR;
            }
            for (int i = 0; i < count; i++) {
                push(arr->items[i]);
            }
            if (!call_value(peek(count), count)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            if (vm.had_error) {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }

        case OP_TAIL_CALL_SPREAD: {
            Value args = pop();
            if (!is_obj_type(args, OBJ_ARRAY)) {
                vm_runtime_error(ERROR_TYPE, "spread operator '...' requires an array, received %s",
                                 value_type_name(args));
                return INTERPRET_RUNTIME_ERROR;
            }
            ObjArray *arr = AS_ARRAY(args);
            int arg_count = arr->count;
            if (arg_count >= STACK_MAX - (int)(vm.stack_top - vm.stack)) {
                vm_runtime_error(ERROR_MEMORY, "stack overflow");
                return INTERPRET_RUNTIME_ERROR;
            }
            for (int i = 0; i < arg_count; i++) {
                push(arr->items[i]);
            }
            Value callee = peek(arg_count);
            if (is_obj_type(callee, OBJ_CLOSURE)) {
                ObjClosure *closure = (ObjClosure *)AS_OBJ(callee);
                ObjFunction *function = closure->function;
                if (arg_count < function->min_arity || (!function->is_varargs && arg_count > function->arity)) {
                    if (function->is_varargs) {
                        vm_runtime_error(ERROR_ARITY, "the function '%s' expects at least %d argument(s) but received %d",
                                         function->name ? function->name->chars : "anonymous", function->min_arity,
                                         arg_count);
                    } else if (function->min_arity == function->arity) {
                        vm_runtime_error(ERROR_ARITY, "the function '%s' expects %d argument(s) but received %d",
                                         function->name ? function->name->chars : "anonymous", function->arity,
                                         arg_count);
                    } else {
                        vm_runtime_error(ERROR_ARITY, "the function '%s' expects between %d and %d arguments but received %d",
                                         function->name ? function->name->chars : "anonymous",
                                         function->min_arity, function->arity, arg_count);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                close_upvalues(frame->slots);
                if (function->is_varargs) {
                    int fixed_passed = arg_count < function->arity ? arg_count : function->arity;
                    int extra_count = arg_count > function->arity ? arg_count - function->arity : 0;
                    ObjArray *rest = object_array();
                    push(OBJ_VAL(rest));
                    for (int i = 0; i < extra_count; i++) {
                        array_push(rest, *(vm.stack_top - 1 - extra_count + i));
                    }
                    vm.stack_top -= (extra_count + 1);
                    memmove(frame->slots, vm.stack_top - fixed_passed - 1, (size_t)(fixed_passed + 1) * sizeof(Value));
                    for (int i = fixed_passed; i < function->arity; i++) {
                        frame->slots[i + 1] = NIL_VAL;
                    }
                    frame->slots[function->arity + 1] = OBJ_VAL(rest);
                    vm.stack_top = frame->slots + function->arity + 2;
                    frame->closure = closure;
                    frame->ip = function->chunk.code;
                    frame->constants = function->chunk.constants;
                    frame->argc = fixed_passed;
                    break;
                }
                int original_argc = arg_count;
                memmove(frame->slots, vm.stack_top - arg_count - 1, (size_t)(arg_count + 1) * sizeof(Value));
                while (arg_count < function->arity) {
                    frame->slots[arg_count + 1] = NIL_VAL;
                    arg_count++;
                }
                vm.stack_top = frame->slots + function->arity + 1;
                frame->closure = closure;
                frame->ip = function->chunk.code;
                frame->constants = function->chunk.constants;
                frame->argc = original_argc;
                break;
            } else if (is_obj_type(callee, OBJ_NATIVE)) {
                ObjNative *native = (ObjNative *)AS_OBJ(callee);
                if (native->arity >= 0 && arg_count != native->arity) {
                    vm_runtime_error(ERROR_ARITY, "the function '%s' expects %d argument(s) but received %d",
                                     native->name->chars, native->arity, arg_count);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value *native_base = vm.stack_top - arg_count - 1;
                Value result = native->function(arg_count, native_base + 1);
                if (vm.had_error) {
                    return INTERPRET_RUNTIME_ERROR;
                }
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
            vm_runtime_error(ERROR_TYPE, "can only call functions, tried to call a %s",
                             value_type_name(callee));
            return INTERPRET_RUNTIME_ERROR;
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

        case OP_CALL_SELF_SUB_LOCAL_CONST: {
            uint8_t slot = READ_BYTE();
            Value b = READ_CONSTANT();
            Value a = frame->slots[slot];
            Value arg;
            if (!arithmetic_values(TOKEN_MINUS, a, b, &arg)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            push(frame->slots[0]);
            push(arg);
            if (!call_closure(frame->closure, 1)) {
                return INTERPRET_RUNTIME_ERROR;
            }
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }

        case OP_RETURN_ADD: {
            Value b = pop();
            Value a = pop();
            Value result;
            if (!add_values(a, b, &result)) {
                return INTERPRET_RUNTIME_ERROR;
            }
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


        default:
            vm_runtime_error(ERROR_RUNTIME, "unknown instruction (%d)", instruction);
            return INTERPRET_RUNTIME_ERROR;
        }

        /* The general path may have changed the frame, the stack or the ip. */
        frame = &vm.frames[vm.frame_count - 1];
        sp = vm.stack_top;
        LOAD_FRAME();
#ifdef SPARK_COMPUTED_GOTO
        DISPATCH();
#else
    }
#endif
}
#ifdef SPARK_COMPUTED_GOTO
#pragma GCC diagnostic pop
#endif

/* Calls `callee` with `arg_count` arguments already pushed on the stack and
 * stores its return value in *result. Used by higher-order natives. */
static bool call_callback(Value callee, int arg_count, Value *result)
{
    Value *args = vm.stack_top - arg_count;

    if (is_obj_type(callee, OBJ_NATIVE)) {
        ObjNative *native = (ObjNative *)AS_OBJ(callee);

        if (native->arity >= 0 && arg_count != native->arity) {
            vm_runtime_error(ERROR_ARITY, "the function '%s' expects %d argument(s) but received %d",
                             native->name->chars, native->arity, arg_count);
            return false;
        }
        *result = native->function(arg_count, args);
        vm.stack_top = args;
        return true;
    }
    if (!is_obj_type(callee, OBJ_CLOSURE)) {
        vm_runtime_error(ERROR_TYPE, "a callback must be a function, received %s", value_type_name(callee));
        return false;
    }
    if (vm.frame_count == FRAMES_MAX) {
        vm_runtime_error(ERROR_RECURSION, "too much call depth (possible infinite recursion)");
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
    define_native("int", native_int, 1);
    define_native("float", native_float, 1);
    define_native("is_int", native_is_int, 1);
    define_native("is_nan", native_is_nan, 1);
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
    define_native("exit", native_exit, -1);
    define_native("env", native_env, -1);
    define_native("input", native_input, -1);
    define_native("file_exists", native_file_exists, 1);
    define_native("read_file", native_read_file, 1);
    define_native("write_file", native_write_file, 2);
    define_native("append_file", native_append_file, 2);
    define_native("open", native_open, -1);
    define_native("read_line", native_read_line, 1);
    define_native("read", native_read, -1);
    define_native("write", native_write, 2);
    define_native("close", native_close, 1);
    define_native("eof", native_eof, 1);
    define_native("srand", native_srand, 1);
    define_native("rand", native_rand, 0);
    define_native("randint", native_randint, -1);
    define_native("to_json", native_to_json, 1);
    define_native("from_json", native_from_json, 1);
    define_native("regex_match", native_regex_match, 2);
    define_native("regex_find", native_regex_find, 2);
    define_native("regex_replace", native_regex_replace, 3);
    vm_set_args(0, NULL);

    int underscore_slot = vm_global_slot(object_string("_", 1));
    if (underscore_slot >= 0) {
        vm.global_slots[underscore_slot].value = NIL_VAL;
        vm.global_slots[underscore_slot].defined = true;
    }
    int nan_slot = vm_global_slot(object_string("nan", 3));
    if (nan_slot >= 0) {
        vm.global_slots[nan_slot].value = NUMBER_VAL(NAN);
        vm.global_slots[nan_slot].defined = true;
        vm.global_slots[nan_slot].is_const = true;
    }
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
    for (int i = 0; i < vm.source_count; i++) {
        mem_realloc(vm.sources[i].text, vm.sources[i].length + 1, 0);
    }
    mem_realloc(vm.sources, sizeof(SourceText) * (size_t)vm.source_capacity, 0);
    vm.sources = NULL;
    vm.source_count = 0;
    vm.source_capacity = 0;
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

static const char *vm_own_source(const char *source)
{
    size_t length = strlen(source);
    char *copy = mem_realloc(NULL, 0, length + 1);

    memcpy(copy, source, length + 1);
    if (vm.source_count == vm.source_capacity) {
        int capacity = vm.source_capacity < 4 ? 4 : vm.source_capacity * 2;

        vm.sources = mem_realloc(vm.sources, sizeof(SourceText) * (size_t)vm.source_capacity,
                                 sizeof(SourceText) * (size_t)capacity);
        vm.source_capacity = capacity;
    }
    vm.sources[vm.source_count].text = copy;
    vm.sources[vm.source_count].length = length;
    vm.source_count++;
    return copy;
}

InterpretResult vm_interpret(const char *source, const char *script_name, bool repl_mode)
{
    Arena arena;
    Stmt *program = NULL;
    char error[256];
    int error_line = 0;
    int error_column = 0;
    const char *owned = vm_own_source(source);

    arena_init(&arena);
    if (!parse_source(owned, &arena, &program, error, sizeof(error), &error_line,
                      &error_column)) {
        error_print(ERROR_SYNTAX, error_line, error_column, owned, error);
        arena_free(&arena);
        return INTERPRET_COMPILE_ERROR;
    }

    ObjFunction *function = compile_ast(program, script_name, owned, repl_mode, error,
                                        sizeof(error), &error_line, &error_column);

    arena_free(&arena);
    if (!function) {
        error_print(ERROR_COMPILE, error_line, error_column, owned, error);
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
