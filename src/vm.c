#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "vm.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ast.h"
#include "buf.h"
#include "bytecode.h"
#include "chunk.h"
#include "compiler.h"
#include "debugger.h"
#include "json.h"
#include "memory.h"
#include "object.h"
#include "parser.h"
#include "regex.h"
#include "table.h"

#define FRAMES_MAX 4096
#define STACK_INITIAL_CAPACITY 2048
#define STACK_MAX_LIMIT (4 * 1024 * 1024)

struct CallFrame {
    ObjClosure *closure;
    uint8_t *ip;
    Value *slots;
    Value *constants;
    int argc;
    Value self;
};

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

typedef enum {
    MODULE_LOADING,
    MODULE_LOADED
} ModuleStatus;

typedef struct ModuleEntry {
    char *canonical_path;
    ModuleStatus status;
    struct ModuleEntry *next;
} ModuleEntry;

#define TRY_HANDLERS_MAX 512

typedef struct {
    int frame_count;
    int stack_count;
    uint8_t *target_ip;
    bool is_finally;
} TryHandler;

typedef struct {
    CallFrame frames[FRAMES_MAX];
    int frame_count;
    Value *stack;
    int stack_capacity;
    Value *stack_top;
    Table global_names;
    GlobalSlot *global_slots;
    int global_count;
    int global_capacity;
    ObjUpvalue *open_upvalues;
    SourceText *sources;
    int source_count;
    int source_capacity;
    ModuleEntry *modules;
    TryHandler try_handlers[TRY_HANDLERS_MAX];
    int try_handler_count;
    bool has_exception;
    Value exception_value;
    bool had_error;
    bool debug_mode;
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
static bool call_callback_bound(Value callee, int arg_count, Value self, Value *result);
static ObjString *vm_value_to_string(Value value);
static inline bool compare_values(bool greater, Value a, Value b, bool *out);
static bool import_module(ObjString *path_str, ObjString *importer_path);

/* Method inline cache. It remembers the probe location of a method on a map so
 * repeated calls skip hashing and linear probing. An entry is only a hint: the
 * hot path verifies the map still owns the same entry array with the same
 * capacity and that the slot still holds the requested key, so rehashing,
 * deletion, clearing and address reuse all degrade to a cache miss instead of
 * a stale read. The cached location is revalidated, never the method value, so
 * the cache does not need to participate in garbage collection. */
enum { METHOD_CACHE_SIZE = 512 };

typedef struct {
    ObjString *name;
    MapEntry *entries;
    int capacity;
    int index;
} MethodCacheEntry;

static MethodCacheEntry method_cache[METHOD_CACHE_SIZE];

static inline uint32_t method_cache_slot(ObjString *name, const ObjMap *map)
{
    return (name->hash ^ (uint32_t)(uintptr_t)map->entries) & (METHOD_CACHE_SIZE - 1);
}


static char *duplicate_string(const char *str)
{
    size_t len = strlen(str);
    char *copy = malloc(len + 1);

    if (copy) {
        memcpy(copy, str, len + 1);
    }
    return copy;
}

static bool grow_stack(int needed)
{
    int old_capacity = vm.stack_capacity;
    int new_capacity = old_capacity < STACK_INITIAL_CAPACITY ? STACK_INITIAL_CAPACITY : old_capacity * 2;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    if (new_capacity > STACK_MAX_LIMIT) {
        if (needed > STACK_MAX_LIMIT) {
            vm_runtime_error(ERROR_RECURSION, "stack overflow (exceeded maximum stack size)");
            return false;
        }
        new_capacity = STACK_MAX_LIMIT;
    }

    Value *old_stack = vm.stack;
    Value *new_stack = mem_realloc(old_stack,
                                   (size_t)old_capacity * sizeof(Value),
                                   (size_t)new_capacity * sizeof(Value));
    if (!new_stack) {
        vm_runtime_error(ERROR_RUNTIME, "out of memory growing stack");
        return false;
    }

    ptrdiff_t delta = new_stack - old_stack;
    vm.stack = new_stack;
    vm.stack_capacity = new_capacity;
    vm.stack_top += delta;

    for (int i = 0; i < vm.frame_count; i++) {
        vm.frames[i].slots += delta;
    }
    for (ObjUpvalue *upvalue = vm.open_upvalues; upvalue; upvalue = upvalue->next) {
        upvalue->location += delta;
    }
    return true;
}

static inline bool ensure_stack(int extra)
{
    int current = (int)(vm.stack_top - vm.stack);
    if (__builtin_expect(current + extra > vm.stack_capacity, 0)) {
        return grow_stack(current + extra);
    }
    return true;
}

static void push(Value value)
{
    ensure_stack(1);
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

/* Class of an instance, or the class itself when the value is a class. */
static ObjClass *value_class(Value value)
{
    if (is_obj_type(value, OBJ_INSTANCE)) {
        return AS_INSTANCE(value)->klass;
    }
    if (is_obj_type(value, OBJ_CLASS)) {
        return AS_CLASS(value);
    }
    return NULL;
}

static bool class_has_name(ObjClass *klass, ObjString *name)
{
    return klass->name && klass->name->length == name->length &&
           memcmp(klass->name->chars, name->chars, (size_t)name->length) == 0;
}

/* True when `value` is an instance of `target` or of any of its subclasses. */
static bool value_is_instance_of(Value value, ObjClass *target)
{
    ObjClass *klass = value_class(value);

    return klass != NULL && class_is_subclass(klass, target);
}

typedef enum {
    DUNDER_ABSENT,
    DUNDER_OK,
    DUNDER_ERROR
} DunderStatus;

/* Calls the special method `name` on `receiver` with `arg_count` arguments.
 * Returns DUNDER_ABSENT when the instance does not define it. The receiver and
 * arguments are pushed while the lookup and the call run so the collector can
 * see them even though they only live in C locals here. */
static DunderStatus call_dunder(Value receiver, const char *name, int arg_count,
                                const Value *args, Value *out)
{
    if (!is_obj_type(receiver, OBJ_INSTANCE)) {
        return DUNDER_ABSENT;
    }
    ObjInstance *instance = AS_INSTANCE(receiver);
    int depth = (int)(vm.stack_top - vm.stack);

    mem_pause_gc();
    if (!ensure_stack(arg_count + 2)) {
        mem_resume_gc();
        return DUNDER_ERROR;
    }
    push(receiver);
    for (int i = 0; i < arg_count; i++) {
        push(args[i]);
    }
    mem_resume_gc();

    ObjString *method_name = object_string(name, (int)strlen(name));
    ObjClosure *method = class_find_method(instance->klass, method_name);

    if (!method) {
        vm.stack_top = vm.stack + depth;
        return DUNDER_ABSENT;
    }
    if (!call_callback_bound(OBJ_VAL(method), arg_count, receiver, out)) {
        vm.stack_top = vm.stack + depth;
        return DUNDER_ERROR;
    }
    vm.stack_top = vm.stack + depth;
    return DUNDER_OK;
}

/* Dunder that implements an arithmetic opcode, or NULL when the opcode has
 * no overloadable counterpart. */
static const char *arithmetic_dunder(uint8_t op)
{
    switch (op) {
    case OP_ADD: return "__add__";
    case OP_SUBTRACT: return "__sub__";
    case OP_MULTIPLY: return "__mul__";
    case OP_DIVIDE: return "__div__";
    case OP_MODULO: return "__mod__";
    case OP_POW: return "__pow__";
    default: return NULL;
    }
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
        mark_object((Obj *)function->script_path);
        for (int i = 0; i < function->debug_local_count; i++) {
            mark_object((Obj *)function->debug_locals[i].name);
        }
        for (int i = 0; i < function->chunk.constants_count; i++) {
            mark_value(function->chunk.constants[i]);
        }
        break;
    }
    case OBJ_CLOSURE: {
        ObjClosure *closure = (ObjClosure *)object;

        mark_object((Obj *)closure->function);
        mark_object((Obj *)closure->klass);
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
    case OBJ_CLASS: {
        ObjClass *klass = (ObjClass *)object;

        mark_object((Obj *)klass->name);
        mark_object((Obj *)klass->superclass);
        mark_object((Obj *)klass->field_init);
        for (int i = 0; i < klass->methods.capacity; i++) {
            TableEntry *entry = &klass->methods.entries[i];

            if (entry->key) {
                mark_object((Obj *)entry->key);
                mark_value(entry->value);
            }
        }
        for (int i = 0; i < klass->statics.capacity; i++) {
            TableEntry *entry = &klass->statics.entries[i];

            if (entry->key) {
                mark_object((Obj *)entry->key);
                mark_value(entry->value);
            }
        }
        break;
    }
    case OBJ_INSTANCE: {
        ObjInstance *instance = (ObjInstance *)object;

        mark_object((Obj *)instance->klass);
        mark_object((Obj *)instance->fields);
        break;
    }
    case OBJ_FILE: {
        ObjFile *file = (ObjFile *)object;
        if (file->path) {
            mark_object((Obj *)file->path);
        }
        break;
    }
    case OBJ_STRING_BUILDER:
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
        mark_value(vm.frames[i].self);
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
    if (vm.has_exception) {
        mark_value(vm.exception_value);
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
    size_t before = mem_allocated();

    mark_roots();
    trace_references();
    intern_remove_unmarked();
    sweep();

    size_t after = mem_allocated();
    double live_ratio = before > 0 ? (double)after / (double)before : 0.5;
    double factor = 2.0;
    if (live_ratio > 0.7) {
        factor = 2.5;
    } else if (live_ratio < 0.3) {
        factor = 1.5;
    }
    size_t threshold = (size_t)((double)after * factor);

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
    (void)args;
    for (int i = 0; i < arg_count; i++) {
        if (i > 0) {
            fputc(' ', stdout);
        }
        /* A to_string() override can grow the VM stack, so re-read the native
         * arguments from the stack instead of keeping the original pointer. */
        Value *current = vm.stack_top - arg_count;
        ObjString *text = vm_value_to_string(current[i]);

        if (!text) {
            return NIL_VAL;
        }
        fwrite(text->chars, 1, (size_t)text->length, stdout);
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
    if (is_obj_type(value, OBJ_INSTANCE)) {
        Value result;
        DunderStatus status = call_dunder(value, "__len__", 0, NULL, &result);

        if (status == DUNDER_ERROR) {
            return NIL_VAL;
        }
        if (status == DUNDER_OK) {
            if (!is_int(result) || AS_INT(result) < 0) {
                vm_runtime_error(ERROR_TYPE, "'__len__' must return a non-negative integer, "
                                 "received %s", value_type_name(result));
                return NIL_VAL;
            }
            return result;
        }
    }
    vm_runtime_error(ERROR_TYPE, "'len' expects a string, an array, a map or an instance "
                     "with '__len__', received %s", value_type_name(value));
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
    ObjString *text = vm_value_to_string(args[0]);

    return text ? OBJ_VAL(text) : NIL_VAL;
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
    if (is_obj_type(args[0], OBJ_INSTANCE)) {
        ObjClass *klass = AS_INSTANCE(args[0])->klass;

        if (klass && klass->name) {
            return OBJ_VAL(klass->name);
        }
    }
    if (is_obj(args[0]) &&
        (AS_OBJ(args[0])->type == OBJ_CLOSURE || AS_OBJ(args[0])->type == OBJ_FUNCTION ||
         AS_OBJ(args[0])->type == OBJ_NATIVE)) {
        name = "function";
    } else {
        name = value_type_name(args[0]);
    }
    return OBJ_VAL(object_string(name, (int)strlen(name)));
}

static Value native_gc(int arg_count, Value *args)
{
    (void)arg_count;
    (void)args;
    collect_garbage();
    return NIL_VAL;
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
        start += total;
        if (start < 0) {
            start = 0;
        }
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

static Value native_sin(int arg_count, Value *args)
{
    (void)arg_count;
    if (!value_is_numeric(args[0])) {
        vm_runtime_error(ERROR_TYPE, "'sin' expects a number, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    return NUMBER_VAL(sin(value_as_double(args[0])));
}

static Value native_cos(int arg_count, Value *args)
{
    (void)arg_count;
    if (!value_is_numeric(args[0])) {
        vm_runtime_error(ERROR_TYPE, "'cos' expects a number, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    return NUMBER_VAL(cos(value_as_double(args[0])));
}

static Value native_log(int arg_count, Value *args)
{
    (void)arg_count;
    if (!value_is_numeric(args[0])) {
        vm_runtime_error(ERROR_TYPE, "'log' expects a number, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    double d = value_as_double(args[0]);
    if (d <= 0.0) {
        vm_runtime_error(ERROR_VALUE, "'log' expects a positive number, received %g", d);
        return NIL_VAL;
    }
    return NUMBER_VAL(log(d));
}

static Value native_exp(int arg_count, Value *args)
{
    (void)arg_count;
    if (!value_is_numeric(args[0])) {
        vm_runtime_error(ERROR_TYPE, "'exp' expects a number, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    return NUMBER_VAL(exp(value_as_double(args[0])));
}

static Value native_min(int arg_count, Value *args)
{
    if (arg_count == 0) {
        vm_runtime_error(ERROR_ARITY, "'min' expects at least 1 argument");
        return NIL_VAL;
    }
    if (arg_count == 1) {
        if (!is_obj_type(args[0], OBJ_ARRAY)) {
            vm_runtime_error(ERROR_TYPE, "single argument to 'min' must be an array");
            return NIL_VAL;
        }
        ObjArray *arr = AS_ARRAY(args[0]);
        if (arr->count == 0) {
            vm_runtime_error(ERROR_VALUE, "cannot find min of an empty array");
            return NIL_VAL;
        }
        Value m = arr->items[0];
        if (!value_is_numeric(m)) {
            vm_runtime_error(ERROR_TYPE, "'min' array elements must be numbers");
            return NIL_VAL;
        }
        for (int i = 1; i < arr->count; i++) {
            Value item = arr->items[i];
            if (!value_is_numeric(item)) {
                vm_runtime_error(ERROR_TYPE, "'min' array elements must be numbers");
                return NIL_VAL;
            }
            if (numeric_less(item, m)) m = item;
        }
        return m;
    }
    Value m = args[0];
    if (!value_is_numeric(m)) {
        vm_runtime_error(ERROR_TYPE, "'min' arguments must be numbers");
        return NIL_VAL;
    }
    for (int i = 1; i < arg_count; i++) {
        if (!value_is_numeric(args[i])) {
            vm_runtime_error(ERROR_TYPE, "'min' arguments must be numbers");
            return NIL_VAL;
        }
        if (numeric_less(args[i], m)) m = args[i];
    }
    return m;
}

static Value native_max(int arg_count, Value *args)
{
    if (arg_count == 0) {
        vm_runtime_error(ERROR_ARITY, "'max' expects at least 1 argument");
        return NIL_VAL;
    }
    if (arg_count == 1) {
        if (!is_obj_type(args[0], OBJ_ARRAY)) {
            vm_runtime_error(ERROR_TYPE, "single argument to 'max' must be an array");
            return NIL_VAL;
        }
        ObjArray *arr = AS_ARRAY(args[0]);
        if (arr->count == 0) {
            vm_runtime_error(ERROR_VALUE, "cannot find max of an empty array");
            return NIL_VAL;
        }
        Value m = arr->items[0];
        if (!value_is_numeric(m)) {
            vm_runtime_error(ERROR_TYPE, "'max' array elements must be numbers");
            return NIL_VAL;
        }
        for (int i = 1; i < arr->count; i++) {
            Value item = arr->items[i];
            if (!value_is_numeric(item)) {
                vm_runtime_error(ERROR_TYPE, "'max' array elements must be numbers");
                return NIL_VAL;
            }
            if (numeric_greater(item, m)) m = item;
        }
        return m;
    }
    Value m = args[0];
    if (!value_is_numeric(m)) {
        vm_runtime_error(ERROR_TYPE, "'max' arguments must be numbers");
        return NIL_VAL;
    }
    for (int i = 1; i < arg_count; i++) {
        if (!value_is_numeric(args[i])) {
            vm_runtime_error(ERROR_TYPE, "'max' arguments must be numbers");
            return NIL_VAL;
        }
        if (numeric_greater(args[i], m)) m = args[i];
    }
    return m;
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
    if (!is_obj_type(args[0], OBJ_ARRAY) && !is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'slice' expects an array or a string as first argument, received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    int start = 0;
    int end = 0;

    if (!integer_argument(args[1], "start", "slice", &start)) {
        return NIL_VAL;
    }

    if (is_obj_type(args[0], OBJ_ARRAY)) {
        ObjArray *array = AS_ARRAY(args[0]);
        if (arg_count == 3) {
            if (!integer_argument(args[2], "end", "slice", &end)) {
                return NIL_VAL;
            }
        } else {
            end = array->count;
        }

        if (start < 0) start += array->count;
        if (start < 0) start = 0;
        if (start > array->count) start = array->count;

        if (end < 0) end += array->count;
        if (end < 0) end = 0;
        if (end > array->count) end = array->count;

        if (end < start) end = start;

        ObjArray *result = object_array();
        push(OBJ_VAL(result));
        for (int i = start; i < end; i++) {
            array_push(result, array->items[i]);
        }
        pop();
        return OBJ_VAL(result);
    } else {
        ObjString *string = AS_STRING(args[0]);
        int total_chars = utf8_length(string->chars, string->length);
        if (arg_count == 3) {
            if (!integer_argument(args[2], "end", "slice", &end)) {
                return NIL_VAL;
            }
        } else {
            end = total_chars;
        }

        if (start < 0) start += total_chars;
        if (start < 0) start = 0;
        if (start > total_chars) start = total_chars;

        if (end < 0) end += total_chars;
        if (end < 0) end = 0;
        if (end > total_chars) end = total_chars;

        if (end < start) end = start;

        int byte_start = utf8_byte_offset(string, start);
        int byte_end = (end == total_chars) ? string->length : utf8_byte_offset(string, end);
        if (byte_start < 0) byte_start = string->length;
        if (byte_end < byte_start) byte_end = byte_start;

        return OBJ_VAL(object_string(string->chars + byte_start, byte_end - byte_start));
    }
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
        ObjString *piece = vm_value_to_string(array->items[i]);

        if (!piece) {
            buf_free(&buffer);
            return NIL_VAL;
        }
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

/* ------------------------------------------------------------------ */
/* Phase 10 / 12 Standard Library & StringBuilder natives             */
/* ------------------------------------------------------------------ */

static Value native_time(int arg_count, Value *args)
{
    (void)arg_count;
    (void)args;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return NUMBER_VAL((double)tv.tv_sec + (double)tv.tv_usec / 1000000.0);
}

static Value native_sleep(int arg_count, Value *args)
{
    (void)arg_count;
    if (!value_is_numeric(args[0])) {
        vm_runtime_error(ERROR_TYPE, "'sleep' expects a number (milliseconds), received %s",
                         value_type_name(args[0]));
        return NIL_VAL;
    }
    double ms = value_as_double(args[0]);
    if (ms < 0) {
        vm_runtime_error(ERROR_VALUE, "'sleep' duration cannot be negative");
        return NIL_VAL;
    }
    long seconds = (long)(ms / 1000.0);
    long nanoseconds = (long)((ms - (seconds * 1000.0)) * 1000000.0);
    struct timespec req;
    req.tv_sec = seconds;
    req.tv_nsec = nanoseconds;
    nanosleep(&req, NULL);
    return NIL_VAL;
}

static Value native_chr(int arg_count, Value *args)
{
    (void)arg_count;
    int code = 0;
    if (!integer_argument(args[0], "codepoint", "chr", &code)) {
        return NIL_VAL;
    }
    if (code < 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
        vm_runtime_error(ERROR_VALUE, "invalid Unicode codepoint %d", code);
        return NIL_VAL;
    }
    char buf[5];
    int len = 0;
    if (code <= 0x7f) {
        buf[len++] = (char)code;
    } else if (code <= 0x7ff) {
        buf[len++] = (char)(0xc0 | (code >> 6));
        buf[len++] = (char)(0x80 | (code & 0x3f));
    } else if (code <= 0xffff) {
        buf[len++] = (char)(0xe0 | (code >> 12));
        buf[len++] = (char)(0x80 | ((code >> 6) & 0x3f));
        buf[len++] = (char)(0x80 | (code & 0x3f));
    } else {
        buf[len++] = (char)(0xf0 | (code >> 18));
        buf[len++] = (char)(0x80 | ((code >> 12) & 0x3f));
        buf[len++] = (char)(0x80 | ((code >> 6) & 0x3f));
        buf[len++] = (char)(0x80 | (code & 0x3f));
    }
    buf[len] = '\0';
    return OBJ_VAL(object_string(buf, len));
}

static Value native_ord(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'ord' expects a string, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjString *s = AS_STRING(args[0]);
    if (s->length == 0) {
        vm_runtime_error(ERROR_VALUE, "'ord' expects a non-empty string");
        return NIL_VAL;
    }
    const uint8_t *p = (const uint8_t *)s->chars;
    uint32_t cp = 0;
    if (p[0] < 0x80) {
        cp = p[0];
    } else if ((p[0] & 0xe0) == 0xc0) {
        cp = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
    } else if ((p[0] & 0xf0) == 0xe0) {
        cp = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
    } else if ((p[0] & 0xf8) == 0xf0) {
        cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
    }
    return INT_VAL((int64_t)cp);
}

static Value pad_string_helper(int arg_count, Value *args, bool start, const char *name)
{
    if (arg_count < 2 || arg_count > 3) {
        vm_runtime_error(ERROR_ARITY, "'%s' expects 2 or 3 arguments, received %d", name, arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'%s' expects a string as first argument", name);
        return NIL_VAL;
    }
    int target_len = 0;
    if (!integer_argument(args[1], "target length", name, &target_len)) {
        return NIL_VAL;
    }
    const char *pad_chars = " ";
    int pad_len = 1;
    if (arg_count == 3) {
        if (!is_obj_type(args[2], OBJ_STRING)) {
            vm_runtime_error(ERROR_TYPE, "'%s' pad string must be a string", name);
            return NIL_VAL;
        }
        ObjString *p = AS_STRING(args[2]);
        if (p->length == 0) {
            return args[0];
        }
        pad_chars = p->chars;
        pad_len = p->length;
    }
    ObjString *s = AS_STRING(args[0]);
    int cur_len = utf8_length(s->chars, s->length);
    if (cur_len >= target_len) {
        return args[0];
    }
    int needed = target_len - cur_len;
    buf_t b;
    buf_init(&b);
    if (start) {
        for (int i = 0; i < needed; i++) {
            buf_append(&b, pad_chars + (i % pad_len), 1);
        }
        buf_append(&b, s->chars, (size_t)s->length);
    } else {
        buf_append(&b, s->chars, (size_t)s->length);
        for (int i = 0; i < needed; i++) {
            buf_append(&b, pad_chars + (i % pad_len), 1);
        }
    }
    ObjString *res = object_string(b.data ? b.data : "", (int)b.len);
    buf_free(&b);
    return OBJ_VAL(res);
}

static Value native_pad_start(int arg_count, Value *args)
{
    return pad_string_helper(arg_count, args, true, "pad_start");
}

static Value native_pad_end(int arg_count, Value *args)
{
    return pad_string_helper(arg_count, args, false, "pad_end");
}

static Value native_sum(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'sum' expects an array, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *arr = AS_ARRAY(args[0]);
    if (arr->count == 0) return INT_VAL(0);
    bool all_int = true;
    int64_t int_sum = 0;
    double double_sum = 0.0;
    for (int i = 0; i < arr->count; i++) {
        Value v = arr->items[i];
        if (!value_is_numeric(v)) {
            vm_runtime_error(ERROR_TYPE, "'sum' elements must be numbers");
            return NIL_VAL;
        }
        if (all_int && is_int(v)) {
            int_sum += AS_INT(v);
        } else {
            if (all_int) {
                double_sum = (double)int_sum;
                all_int = false;
            }
            double_sum += value_as_double(v);
        }
    }
    return all_int ? INT_VAL(int_sum) : NUMBER_VAL(double_sum);
}

static Value native_any(int arg_count, Value *args)
{
    if (arg_count < 1 || arg_count > 2) {
        vm_runtime_error(ERROR_ARITY, "'any' expects 1 or 2 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'any' expects an array as first argument");
        return NIL_VAL;
    }
    ObjArray *arr = AS_ARRAY(args[0]);
    Value fn = arg_count == 2 ? args[1] : NIL_VAL;
    for (int i = 0; i < arr->count; i++) {
        if (arg_count == 2) {
            Value res;
            push(arr->items[i]);
            if (!call_callback(fn, 1, &res) || vm.had_error) return NIL_VAL;
            if (!value_is_falsey(res)) return BOOL_VAL(true);
        } else {
            if (!value_is_falsey(arr->items[i])) return BOOL_VAL(true);
        }
    }
    return BOOL_VAL(false);
}

static Value native_all(int arg_count, Value *args)
{
    if (arg_count < 1 || arg_count > 2) {
        vm_runtime_error(ERROR_ARITY, "'all' expects 1 or 2 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'all' expects an array as first argument");
        return NIL_VAL;
    }
    ObjArray *arr = AS_ARRAY(args[0]);
    Value fn = arg_count == 2 ? args[1] : NIL_VAL;
    for (int i = 0; i < arr->count; i++) {
        if (arg_count == 2) {
            Value res;
            push(arr->items[i]);
            if (!call_callback(fn, 1, &res) || vm.had_error) return NIL_VAL;
            if (value_is_falsey(res)) return BOOL_VAL(false);
        } else {
            if (value_is_falsey(arr->items[i])) return BOOL_VAL(false);
        }
    }
    return BOOL_VAL(true);
}

static Value native_enumerate(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'enumerate' expects an array, received %s", value_type_name(args[0]));
        return NIL_VAL;
    }
    ObjArray *arr = AS_ARRAY(args[0]);
    ObjArray *res = object_array();
    push(OBJ_VAL(res));
    for (int i = 0; i < arr->count; i++) {
        ObjArray *pair = object_array();
        push(OBJ_VAL(pair));
        array_push(pair, INT_VAL(i));
        array_push(pair, arr->items[i]);
        array_push(res, OBJ_VAL(pair));
        pop();
    }
    pop();
    return OBJ_VAL(res);
}

static Value native_zip(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_ARRAY) || !is_obj_type(args[1], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'zip' expects two arrays");
        return NIL_VAL;
    }
    ObjArray *a1 = AS_ARRAY(args[0]);
    ObjArray *a2 = AS_ARRAY(args[1]);
    int n = a1->count < a2->count ? a1->count : a2->count;
    ObjArray *res = object_array();
    push(OBJ_VAL(res));
    for (int i = 0; i < n; i++) {
        ObjArray *pair = object_array();
        push(OBJ_VAL(pair));
        array_push(pair, a1->items[i]);
        array_push(pair, a2->items[i]);
        array_push(res, OBJ_VAL(pair));
        pop();
    }
    pop();
    return OBJ_VAL(res);
}

static Value native_isinstance(int arg_count, Value *args)
{
    (void)arg_count;
    if (is_obj_type(args[1], OBJ_CLASS)) {
        return BOOL_VAL(value_is_instance_of(args[0], AS_CLASS(args[1])));
    }
    if (is_obj_type(args[1], OBJ_STRING)) {
        ObjString *target_type = AS_STRING(args[1]);
        ObjClass *klass = value_class(args[0]);

        if (!klass) {
            return BOOL_VAL(strcmp(target_type->chars, value_type_name(args[0])) == 0);
        }
        for (ObjClass *current = klass; current; current = current->superclass) {
            if (class_has_name(current, target_type)) {
                return BOOL_VAL(true);
            }
        }
        return BOOL_VAL(false);
    }
    vm_runtime_error(ERROR_TYPE, "'isinstance' second argument must be a class or a type name");
    return NIL_VAL;
}

static Value native_is_a(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[1], OBJ_CLASS)) {
        vm_runtime_error(ERROR_TYPE, "'is_a' expects a class as its second argument, received %s",
                         value_type_name(args[1]));
        return NIL_VAL;
    }
    return BOOL_VAL(value_is_instance_of(args[0], AS_CLASS(args[1])));
}

static Value native_copy(int arg_count, Value *args)
{
    (void)arg_count;
    Value val = args[0];
    if (is_obj_type(val, OBJ_ARRAY)) {
        ObjArray *arr = AS_ARRAY(val);
        ObjArray *cp = object_array();
        push(OBJ_VAL(cp));
        for (int i = 0; i < arr->count; i++) {
            array_push(cp, arr->items[i]);
        }
        pop();
        return OBJ_VAL(cp);
    }
    if (is_obj_type(val, OBJ_MAP)) {
        ObjMap *map = AS_MAP(val);
        ObjMap *cp = object_map();
        push(OBJ_VAL(cp));
        for (int i = 0; i < map->capacity; i++) {
            if (map->entries[i].state == 1) {
                map_set(cp, map->entries[i].key, map->entries[i].value);
            }
        }
        pop();
        return OBJ_VAL(cp);
    }
    return val;
}

static Value native_list_dir(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'list_dir' expects a directory path string");
        return NIL_VAL;
    }
    ObjString *path = AS_STRING(args[0]);
    DIR *dir = opendir(path->chars);
    if (!dir) {
        vm_runtime_error(ERROR_RUNTIME, "cannot open directory '%s'", path->chars);
        return NIL_VAL;
    }
    ObjArray *res = object_array();
    push(OBJ_VAL(res));
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        ObjString *name = object_string(entry->d_name, (int)strlen(entry->d_name));
        array_push(res, OBJ_VAL(name));
    }
    closedir(dir);
    pop();
    return OBJ_VAL(res);
}

static Value native_mkdir(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'mkdir' expects a path string");
        return NIL_VAL;
    }
    ObjString *path = AS_STRING(args[0]);
    if (mkdir(path->chars, 0777) != 0 && errno != EEXIST) {
        vm_runtime_error(ERROR_RUNTIME, "cannot create directory '%s'", path->chars);
        return NIL_VAL;
    }
    return BOOL_VAL(true);
}

static Value native_exec(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'exec' expects a command string");
        return NIL_VAL;
    }
    ObjString *cmd = AS_STRING(args[0]);
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        vm_runtime_error(ERROR_RUNTIME, "failed to create pipes for 'exec'");
        return NIL_VAL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        vm_runtime_error(ERROR_RUNTIME, "failed to fork process for 'exec'");
        return NIL_VAL;
    }
    if (pid == 0) {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd->chars, (char *)NULL);
        _exit(127);
    }
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    buf_t out_buf, err_buf;
    buf_init(&out_buf);
    buf_init(&err_buf);
    char temp[512];
    ssize_t n;
    while ((n = read(stdout_pipe[0], temp, sizeof(temp))) > 0) {
        buf_append(&out_buf, temp, (size_t)n);
    }
    while ((n = read(stderr_pipe[0], temp, sizeof(temp))) > 0) {
        buf_append(&err_buf, temp, (size_t)n);
    }
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    ObjMap *map = object_map();
    push(OBJ_VAL(map));
    map_set(map, OBJ_VAL(object_string("exit_code", 9)), INT_VAL(exit_code));
    ObjString *out_str = object_string(out_buf.data ? out_buf.data : "", (int)out_buf.len);
    map_set(map, OBJ_VAL(object_string("stdout", 6)), OBJ_VAL(out_str));
    ObjString *err_str = object_string(err_buf.data ? err_buf.data : "", (int)err_buf.len);
    map_set(map, OBJ_VAL(object_string("stderr", 6)), OBJ_VAL(err_str));
    buf_free(&out_buf);
    buf_free(&err_buf);
    pop();
    return OBJ_VAL(map);
}

static Value native_string_builder(int arg_count, Value *args)
{
    int cap = 16;
    const char *init = NULL;
    int init_len = 0;
    if (arg_count == 1) {
        if (is_int(args[0])) {
            cap = (int)AS_INT(args[0]);
        } else if (is_obj_type(args[0], OBJ_STRING)) {
            ObjString *s = AS_STRING(args[0]);
            init = s->chars;
            init_len = s->length;
            cap = init_len + 16;
        }
    }
    ObjStringBuilder *sb = object_string_builder(cap);
    if (init && init_len > 0) {
        string_builder_append(sb, init, init_len);
    }
    return OBJ_VAL(sb);
}

static Value native_sb_append(int arg_count, Value *args)
{
    if (arg_count < 2) {
        vm_runtime_error(ERROR_ARITY, "'sb_append' expects 2 arguments (builder, value)");
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING_BUILDER)) {
        vm_runtime_error(ERROR_TYPE, "'sb_append' first argument must be a string_builder");
        return NIL_VAL;
    }
    ObjStringBuilder *sb = AS_STRING_BUILDER(args[0]);
    ObjString *s = is_obj_type(args[1], OBJ_STRING) ? AS_STRING(args[1])
                                                   : vm_value_to_string(args[1]);
    if (!s) {
        return NIL_VAL;
    }
    string_builder_append(sb, s->chars, s->length);
    return args[0];
}

static Value native_sb_to_string(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING_BUILDER)) {
        vm_runtime_error(ERROR_TYPE, "'sb_to_string' expects a string_builder");
        return NIL_VAL;
    }
    return OBJ_VAL(string_builder_to_string(AS_STRING_BUILDER(args[0])));
}

static Value native_sb_len(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING_BUILDER)) {
        vm_runtime_error(ERROR_TYPE, "'sb_len' expects a string_builder");
        return NIL_VAL;
    }
    ObjStringBuilder *sb = AS_STRING_BUILDER(args[0]);
    return INT_VAL(utf8_length(sb->chars, sb->length));
}

static Value native_sb_clear(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_STRING_BUILDER)) {
        vm_runtime_error(ERROR_TYPE, "'sb_clear' expects a string_builder");
        return NIL_VAL;
    }
    string_builder_clear(AS_STRING_BUILDER(args[0]));
    return args[0];
}

static bool dispatch_native_method(Value receiver, ObjString *method, int arg_count, Value *out)
{
    const char *name = method->chars;
    Value *args = vm.stack_top - arg_count;

    if (is_obj_type(receiver, OBJ_STRING)) {
        ObjString *str = AS_STRING(receiver);
        if (strcmp(name, "len") == 0 && arg_count == 0) {
            *out = INT_VAL((int64_t)utf8_length(str->chars, str->length));
            return true;
        }
        if (strcmp(name, "upper") == 0 && arg_count == 0) {
            Value a[1] = { receiver };
            *out = native_upper(1, a);
            return true;
        }
        if (strcmp(name, "lower") == 0 && arg_count == 0) {
            Value a[1] = { receiver };
            *out = native_lower(1, a);
            return true;
        }
        if (strcmp(name, "trim") == 0 && arg_count == 0) {
            Value a[1] = { receiver };
            *out = native_trim(1, a);
            return true;
        }
        if (strcmp(name, "split") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_split(2, a);
            return true;
        }
        if (strcmp(name, "replace") == 0 && arg_count == 2) {
            Value a[3] = { receiver, args[0], args[1] };
            *out = native_replace(3, a);
            return true;
        }
        if (strcmp(name, "starts_with") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_starts_with(2, a);
            return true;
        }
        if (strcmp(name, "ends_with") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_ends_with(2, a);
            return true;
        }
        if (strcmp(name, "contains") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_contains(2, a);
            return true;
        }
        if (strcmp(name, "index_of") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_index_of(2, a);
            return true;
        }
        if (strcmp(name, "repeat") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_repeat(2, a);
            return true;
        }
        if (strcmp(name, "slice") == 0 && (arg_count == 1 || arg_count == 2)) {
            Value a[3] = { receiver, args[0], arg_count == 2 ? args[1] : NIL_VAL };
            *out = native_slice(arg_count + 1, a);
            return true;
        }
        if (strcmp(name, "pad_start") == 0 && (arg_count == 1 || arg_count == 2)) {
            Value a[3] = { receiver, args[0], arg_count == 2 ? args[1] : NIL_VAL };
            *out = pad_string_helper(arg_count + 1, a, true, "pad_start");
            return true;
        }
        if (strcmp(name, "pad_end") == 0 && (arg_count == 1 || arg_count == 2)) {
            Value a[3] = { receiver, args[0], arg_count == 2 ? args[1] : NIL_VAL };
            *out = pad_string_helper(arg_count + 1, a, false, "pad_end");
            return true;
        }
    } else if (is_obj_type(receiver, OBJ_ARRAY)) {
        ObjArray *arr = AS_ARRAY(receiver);
        if (strcmp(name, "len") == 0 && arg_count == 0) {
            *out = INT_VAL((int64_t)arr->count);
            return true;
        }
        if (strcmp(name, "push") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_push(2, a);
            return true;
        }
        if (strcmp(name, "pop") == 0 && arg_count == 0) {
            Value a[1] = { receiver };
            *out = native_pop(1, a);
            return true;
        }
        if (strcmp(name, "contains") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_contains(2, a);
            return true;
        }
        if (strcmp(name, "index_of") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_index_of(2, a);
            return true;
        }
        if (strcmp(name, "insert") == 0 && arg_count == 2) {
            Value a[3] = { receiver, args[0], args[1] };
            *out = native_insert(3, a);
            return true;
        }
        if (strcmp(name, "remove") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_remove(2, a);
            return true;
        }
        if (strcmp(name, "reverse") == 0 && arg_count == 0) {
            Value a[1] = { receiver };
            *out = native_reverse(1, a);
            return true;
        }
        if (strcmp(name, "fill") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_fill(2, a);
            return true;
        }
        if (strcmp(name, "slice") == 0 && (arg_count == 1 || arg_count == 2)) {
            Value a[3] = { receiver, args[0], arg_count == 2 ? args[1] : NIL_VAL };
            *out = native_slice(arg_count + 1, a);
            return true;
        }
        if (strcmp(name, "join") == 0 && (arg_count == 0 || arg_count == 1)) {
            Value sep = arg_count == 1 ? args[0] : OBJ_VAL(object_string("", 0));
            Value a[2] = { receiver, sep };
            *out = native_join(2, a);
            return true;
        }
        if (strcmp(name, "sort") == 0 && arg_count == 0) {
            Value a[1] = { receiver };
            *out = native_sort(1, a);
            return true;
        }
        if (strcmp(name, "sort_by") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_sort_by(2, a);
            return true;
        }
        if (strcmp(name, "map") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_map(2, a);
            return true;
        }
        if (strcmp(name, "filter") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_filter(2, a);
            return true;
        }
        if (strcmp(name, "each") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_each(2, a);
            return true;
        }
        if (strcmp(name, "reduce") == 0 && (arg_count == 1 || arg_count == 2)) {
            Value a[3] = { receiver, args[0], arg_count == 2 ? args[1] : NIL_VAL };
            *out = native_reduce(arg_count + 1, a);
            return true;
        }
    } else if (is_obj_type(receiver, OBJ_STRING_BUILDER)) {
        ObjStringBuilder *sb = AS_STRING_BUILDER(receiver);
        if (strcmp(name, "append") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_sb_append(2, a);
            return true;
        }
        if (strcmp(name, "to_string") == 0 && arg_count == 0) {
            *out = OBJ_VAL(string_builder_to_string(sb));
            return true;
        }
        if (strcmp(name, "len") == 0 && arg_count == 0) {
            *out = INT_VAL((int64_t)utf8_length(sb->chars, sb->length));
            return true;
        }
        if (strcmp(name, "clear") == 0 && arg_count == 0) {
            string_builder_clear(sb);
            *out = receiver;
            return true;
        }
    } else if (is_obj_type(receiver, OBJ_FILE)) {
        if (strcmp(name, "read_line") == 0 && arg_count == 0) {
            Value a[1] = { receiver };
            *out = native_read_line(1, a);
            return true;
        }
        if (strcmp(name, "read") == 0 && (arg_count == 0 || arg_count == 1)) {
            Value a[2] = { receiver, arg_count == 1 ? args[0] : NIL_VAL };
            *out = native_read(arg_count + 1, a);
            return true;
        }
        if (strcmp(name, "write") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_write(2, a);
            return true;
        }
        if (strcmp(name, "close") == 0 && arg_count == 0) {
            Value a[1] = { receiver };
            *out = native_close(1, a);
            return true;
        }
        if (strcmp(name, "eof") == 0 && arg_count == 0) {
            Value a[1] = { receiver };
            *out = native_eof(1, a);
            return true;
        }
    } else if (is_obj_type(receiver, OBJ_MAP)) {
        if (strcmp(name, "keys") == 0 && arg_count == 0) {
            Value a[1] = { receiver };
            *out = native_keys(1, a);
            return true;
        }
        if (strcmp(name, "values") == 0 && arg_count == 0) {
            Value a[1] = { receiver };
            *out = native_values(1, a);
            return true;
        }
        if (strcmp(name, "len") == 0 && arg_count == 0) {
            Value a[1] = { receiver };
            *out = native_len(1, a);
            return true;
        }
        if (strcmp(name, "has") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_has(2, a);
            return true;
        }
        if (strcmp(name, "get") == 0 && (arg_count == 1 || arg_count == 2)) {
            Value a[3] = { receiver, args[0], arg_count == 2 ? args[1] : NIL_VAL };
            *out = native_get(arg_count + 1, a);
            return true;
        }
        if (strcmp(name, "delete") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_del(2, a);
            return true;
        }
        if (strcmp(name, "clear") == 0 && arg_count == 0) {
            Value a[1] = { receiver };
            *out = native_clear(1, a);
            return true;
        }
        if (strcmp(name, "merge") == 0 && arg_count == 1) {
            Value a[2] = { receiver, args[0] };
            *out = native_merge(2, a);
            return true;
        }
    }
    return false;
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
    if (slot < 0 || slot >= vm.global_count || !vm.global_slots[slot].name) {
        return "?";
    }
    return vm.global_slots[slot].name->chars;
}

int vm_global_count(void)
{
    return vm.global_count;
}

ObjString *vm_global_slot_obj_name(int slot)
{
    if (slot >= 0 && slot < vm.global_count) {
        return vm.global_slots[slot].name;
    }
    return NULL;
}

void vm_register_global_slot(int slot, ObjString *name)
{
    if (!vm_ready) {
        vm_init();
    }
    while (slot >= vm.global_capacity) {
        int capacity = vm.global_capacity < 16 ? 16 : vm.global_capacity * 2;
        vm.global_slots = mem_realloc(vm.global_slots,
                                      sizeof(GlobalSlot) * (size_t)vm.global_capacity,
                                      sizeof(GlobalSlot) * (size_t)capacity);
        for (int i = vm.global_capacity; i < capacity; i++) {
            vm.global_slots[i].name = NULL;
            vm.global_slots[i].value = NIL_VAL;
            vm.global_slots[i].defined = false;
            vm.global_slots[i].is_const = false;
        }
        vm.global_capacity = capacity;
    }
    if (slot >= vm.global_count) {
        for (int i = vm.global_count; i <= slot; i++) {
            vm.global_slots[i].name = NULL;
            vm.global_slots[i].value = NIL_VAL;
            vm.global_slots[i].defined = false;
            vm.global_slots[i].is_const = false;
        }
        vm.global_count = slot + 1;
    }
    vm.global_slots[slot].name = name;
    table_set(&vm.global_names, name, NUMBER_VAL((double)slot));
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

    if (vm.try_handler_count > 0) {
        ObjMap *err = object_map();
        push(OBJ_VAL(err));

        ObjString *k_type = object_string("type", 4);
        push(OBJ_VAL(k_type));
        const char *tname = error_type_name(type);
        ObjString *v_type = object_string(tname, (int)strlen(tname));
        push(OBJ_VAL(v_type));
        map_set(err, OBJ_VAL(k_type), OBJ_VAL(v_type));
        pop(); pop();

        ObjString *k_msg = object_string("message", 7);
        push(OBJ_VAL(k_msg));
        ObjString *v_msg = object_string(message, (int)strlen(message));
        push(OBJ_VAL(v_msg));
        map_set(err, OBJ_VAL(k_msg), OBJ_VAL(v_msg));
        pop(); pop();

        ObjString *k_line = object_string("line", 4);
        push(OBJ_VAL(k_line));
        map_set(err, OBJ_VAL(k_line), INT_VAL(line));
        pop();

        ObjString *k_col = object_string("column", 6);
        push(OBJ_VAL(k_col));
        map_set(err, OBJ_VAL(k_col), INT_VAL(column));
        pop();

        pop(); /* pop err */

        vm.has_exception = true;
        vm.exception_value = OBJ_VAL(err);
        return;
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
    if (!ensure_stack(function->arity + 256)) {
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
        frame->self = NIL_VAL;
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
    frame->self = NIL_VAL;
    return true;
}

static bool call_value(Value callee, int arg_count)
{
    if (is_obj(callee)) {
        switch (AS_OBJ(callee)->type) {
        case OBJ_CLOSURE:
            return call_closure((ObjClosure *)AS_OBJ(callee), arg_count);
        case OBJ_CLASS: {
            ObjClass *klass = (ObjClass *)AS_OBJ(callee);

            vm_runtime_error(ERROR_TYPE, "class '%s' must be constructed with 'new %s(...)'",
                             klass->name ? klass->name->chars : "?",
                             klass->name ? klass->name->chars : "?");
            return false;
        }
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
            if (vm.has_exception) {
                return false;
            }
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

/* Calls `closure` with the arguments already on the VM stack and binds
 * `receiver` as `self` on the new frame. The receiver is kept reachable while
 * call_closure makes room for default arguments, which may allocate. */
static bool call_method(ObjClosure *closure, int arg_count, Value receiver)
{
    Value *slot = vm.stack_top - arg_count - 1;
    bool ok;

    *slot = OBJ_VAL(closure);
    mem_pause_gc();
    ok = call_closure(closure, arg_count);
    if (ok) {
        vm.frames[vm.frame_count - 1].self = receiver;
    }
    mem_resume_gc();
    return ok;
}

/* Runs the field initializers of `klass` and its ancestors base-first. */
static bool initialize_instance_fields(ObjClass *klass, Value instance);

/* Implements `new`: the class and its arguments are on the VM stack at
 * vm.stack_top - arg_count - 1. On success the init frame (if any) is ready to
 * run, or the new instance replaces the class value. */
static bool invoke_constructor(int arg_count)
{
    Value *slot = vm.stack_top - arg_count - 1;
    Value class_value = *slot;

    if (!is_obj_type(class_value, OBJ_CLASS)) {
        vm_runtime_error(ERROR_TYPE, "'new' expects a class, received %s",
                         value_type_name(class_value));
        return false;
    }

    ObjClass *klass = AS_CLASS(class_value);
    ObjInstance *instance = object_instance(klass);
    Value instance_value = OBJ_VAL(instance);

    /* The class value is no longer needed; leaving the instance in its place
     * keeps it reachable while the field initializers run. */
    *slot = instance_value;

    if (!initialize_instance_fields(klass, instance_value)) {
        return false;
    }

    ObjClosure *init = class_find_method(klass, object_string("init", 4));
    if (!init) {
        if (arg_count != 0) {
            vm_runtime_error(ERROR_ARITY,
                             "class '%s' does not define 'init' but received %d argument(s)",
                             klass->name ? klass->name->chars : "?", arg_count);
            return false;
        }
        vm.stack_top = slot + 1;
        return true;
    }

    return call_method(init, arg_count, instance_value);
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

/* Evaluates one ordering operator, trying the instance dunder first. The
 * primitive fallback can be expressed as `!(a > b)` for `<=` and `>=`, so
 * `greater` and `invert` describe that fallback while the dunder is used
 * directly. `out` receives the truth value of the operator itself. */
static bool compare_operator(Value a, Value b, const char *dunder, bool greater, bool invert,
                             bool *out)
{
    if (is_obj_type(a, OBJ_INSTANCE)) {
        Value result;
        DunderStatus status = call_dunder(a, dunder, 1, &b, &result);

        if (status == DUNDER_OK) {
            *out = !value_is_falsey(result);
            return true;
        }
        if (status == DUNDER_ERROR) {
            return false;
        }
    }
    if (!compare_values(greater, a, b, out)) {
        return false;
    }
    if (invert) {
        *out = !*out;
    }
    return true;
}

/* Equality with an instance dunder. Nil keeps its identity semantics so
 * `value == nil` stays a reliable null check even for overloaded classes. */
static bool equality_values(Value a, Value b, bool *out)
{
    if (!is_nil(a) && !is_nil(b)) {
        if (is_obj_type(a, OBJ_INSTANCE)) {
            Value result;
            DunderStatus status = call_dunder(a, "__eq__", 1, &b, &result);

            if (status == DUNDER_OK) {
                *out = !value_is_falsey(result);
                return true;
            }
            if (status == DUNDER_ERROR) {
                return false;
            }
        }
        if (is_obj_type(b, OBJ_INSTANCE)) {
            Value result;
            DunderStatus status = call_dunder(b, "__eq__", 1, &a, &result);

            if (status == DUNDER_OK) {
                *out = !value_is_falsey(result);
                return true;
            }
            if (status == DUNDER_ERROR) {
                return false;
            }
        }
    }
    *out = values_equal(a, b);
    return true;
}

/* Fast path for the comparison-and-jump superinstructions. Returns false when
 * the operands need the general path (non-numeric ordering); equality is
 * always decided here because it cannot fail. `*jump` is the branch decision. */
static inline bool fused_comparison_jump(OpCode op, Value a, Value b, bool *jump)
{
    if (a.type == VAL_INT && b.type == VAL_INT) {
        int64_t x = AS_INT(a);
        int64_t y = AS_INT(b);

        switch (op) {
        case OP_JUMP_IF_NOT_LESS_LOCAL_CONST:
        case OP_JUMP_IF_NOT_LESS_LOCAL_LOCAL:
            *jump = x >= y;
            return true;
        case OP_JUMP_IF_NOT_GREATER_LOCAL_CONST:
        case OP_JUMP_IF_NOT_GREATER_LOCAL_LOCAL:
            *jump = x <= y;
            return true;
        case OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_CONST:
        case OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_LOCAL:
            *jump = x > y;
            return true;
        case OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_CONST:
        case OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_LOCAL:
            *jump = x < y;
            return true;
        case OP_JUMP_IF_EQUAL_LOCAL_CONST:
        case OP_JUMP_IF_EQUAL_LOCAL_LOCAL:
            *jump = x == y;
            return true;
        case OP_JUMP_IF_NOT_EQUAL_LOCAL_CONST:
        case OP_JUMP_IF_NOT_EQUAL_LOCAL_LOCAL:
            *jump = x != y;
            return true;
        default:
            return false;
        }
    }
    if (value_is_numeric(a) && value_is_numeric(b)) {
        double x = value_as_double(a);
        double y = value_as_double(b);

        switch (op) {
        case OP_JUMP_IF_NOT_LESS_LOCAL_CONST:
        case OP_JUMP_IF_NOT_LESS_LOCAL_LOCAL:
            *jump = !(x < y);
            return true;
        case OP_JUMP_IF_NOT_GREATER_LOCAL_CONST:
        case OP_JUMP_IF_NOT_GREATER_LOCAL_LOCAL:
            *jump = !(x > y);
            return true;
        case OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_CONST:
        case OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_LOCAL:
            /* `a <= b` is false for NaN, so the branch must be taken. */
            *jump = !(x <= y);
            return true;
        case OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_CONST:
        case OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_LOCAL:
            *jump = !(x >= y);
            return true;
        case OP_JUMP_IF_EQUAL_LOCAL_CONST:
        case OP_JUMP_IF_EQUAL_LOCAL_LOCAL:
            /* Mixed int/number equality has to stay exact for magnitudes that
             * a double cannot represent (2^53 and beyond). */
            *jump = values_equal(a, b);
            return true;
        case OP_JUMP_IF_NOT_EQUAL_LOCAL_CONST:
        case OP_JUMP_IF_NOT_EQUAL_LOCAL_LOCAL:
            *jump = !values_equal(a, b);
            return true;
        default:
            return false;
        }
    }
    switch (op) {
    case OP_JUMP_IF_EQUAL_LOCAL_CONST:
    case OP_JUMP_IF_EQUAL_LOCAL_LOCAL:
        if (is_obj_type(a, OBJ_INSTANCE) || is_obj_type(b, OBJ_INSTANCE)) {
            return false;
        }
        *jump = values_equal(a, b);
        return true;
    case OP_JUMP_IF_NOT_EQUAL_LOCAL_CONST:
    case OP_JUMP_IF_NOT_EQUAL_LOCAL_LOCAL:
        if (is_obj_type(a, OBJ_INSTANCE) || is_obj_type(b, OBJ_INSTANCE)) {
            return false;
        }
        *jump = !values_equal(a, b);
        return true;
    default:
        return false;
    }
}

/* General path for the ordering variants, plus equality when an instance
 * participates and the fused path cannot decide it: reports a runtime error
 * when the operands are not comparable. */
static bool slow_comparison_jump(OpCode op, Value a, Value b, bool *jump)
{
    const char *dunder = NULL;
    bool greater = false;
    bool invert = false;
    bool negate = false;

    switch (op) {
    case OP_JUMP_IF_NOT_LESS_LOCAL_CONST:
    case OP_JUMP_IF_NOT_LESS_LOCAL_LOCAL:
        dunder = "__lt__";
        negate = true;
        break;
    case OP_JUMP_IF_NOT_GREATER_LOCAL_CONST:
    case OP_JUMP_IF_NOT_GREATER_LOCAL_LOCAL:
        dunder = "__gt__";
        greater = true;
        negate = true;
        break;
    case OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_CONST:
    case OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_LOCAL:
        dunder = "__le__";
        greater = true;
        invert = true;
        negate = true;
        break;
    case OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_CONST:
    case OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_LOCAL:
        dunder = "__ge__";
        invert = true;
        negate = true;
        break;
    case OP_JUMP_IF_EQUAL_LOCAL_CONST:
    case OP_JUMP_IF_EQUAL_LOCAL_LOCAL:
    case OP_JUMP_IF_NOT_EQUAL_LOCAL_CONST:
    case OP_JUMP_IF_NOT_EQUAL_LOCAL_LOCAL: {
        bool equal;

        if (!equality_values(a, b, &equal)) {
            return false;
        }
        *jump = (op == OP_JUMP_IF_EQUAL_LOCAL_CONST ||
                 op == OP_JUMP_IF_EQUAL_LOCAL_LOCAL) ? equal : !equal;
        return true;
    }
    default:
        return false;
    }

    bool result;
    if (!compare_operator(a, b, dunder, greater, invert, &result)) {
        return false;
    }
    *jump = negate ? !result : result;
    return true;
}

static inline bool arithmetic_values(uint8_t op, Value a, Value b, Value *out)
{
    if (is_obj_type(a, OBJ_INSTANCE)) {
        const char *dunder = arithmetic_dunder(op);

        if (dunder) {
            Value result;
            DunderStatus status = call_dunder(a, dunder, 1, &b, &result);

            if (status == DUNDER_OK) {
                *out = result;
                return true;
            }
            if (status == DUNDER_ERROR) {
                return false;
            }
        }
    }
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
    if (is_obj_type(a, OBJ_INSTANCE)) {
        Value result;
        DunderStatus status = call_dunder(a, "__add__", 1, &b, &result);

        if (status == DUNDER_OK) {
            *out = result;
            return true;
        }
        if (status == DUNDER_ERROR) {
            return false;
        }
    }
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
    vm_runtime_error(ERROR_TYPE,
                     "the operands of '+' must be two numbers or two strings (received %s and %s); "
                     "use string interpolation (\"value: ${x}\") or str(x) to build text",
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

        if (integer < INT_MIN || integer > INT_MAX) {
            vm_runtime_error(ERROR_VALUE, "the index is out of integer range, received %lld",
                             (long long)integer);
            return false;
        }
        *out = (int)integer;
        return true;
    }
    if (is_number(index)) {
        double raw = AS_NUMBER(index);

        if (raw != floor(raw) || raw < (double)INT_MIN || raw > (double)INT_MAX) {
            vm_runtime_error(ERROR_VALUE, "the index must be an integral number, received %g", raw);
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

/* Comparison-and-jump superinstructions: both operands compare with a fused
 * fast path; anything it cannot decide goes through the cold implementation. */
#define FUSED_JUMP_LOCAL_CONST(opname)                                        \
    TARGET(opname) :                                                          \
    {                                                                         \
        Value cmp_a = slots[ip[1]];                                           \
        Value cmp_b = constants[U16_AT(ip + 2)];                              \
        bool cmp_jump;                                                        \
        if (__builtin_expect(fused_comparison_jump(opname, cmp_a, cmp_b,      \
                                                   &cmp_jump), 1)) {          \
            ip += cmp_jump ? 6 + U16_AT(ip + 4) : 6;                          \
            DISPATCH();                                                       \
        }                                                                     \
        goto cold;                                                            \
    }

#define FUSED_JUMP_LOCAL_LOCAL(opname)                                        \
    TARGET(opname) :                                                          \
    {                                                                         \
        Value cmp_a = slots[ip[1]];                                           \
        Value cmp_b = slots[ip[2]];                                           \
        bool cmp_jump;                                                        \
        if (__builtin_expect(fused_comparison_jump(opname, cmp_a, cmp_b,      \
                                                   &cmp_jump), 1)) {          \
            ip += cmp_jump ? 5 + U16_AT(ip + 3) : 5;                          \
            DISPATCH();                                                       \
        }                                                                     \
        goto cold;                                                            \
    }

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
        [OP_GET_LOCAL_16] = &&op_OP_GET_LOCAL_16,
        [OP_SET_LOCAL_16] = &&op_OP_SET_LOCAL_16,
        [OP_GET_GLOBAL] = &&op_OP_GET_GLOBAL,
        [OP_ADD] = &&op_OP_ADD,
        [OP_SUBTRACT] = &&op_OP_SUBTRACT,
        [OP_MULTIPLY] = &&op_OP_MULTIPLY,
        [OP_LESS] = &&op_OP_LESS,
        [OP_GREATER] = &&op_OP_GREATER,
        [OP_GREATER_OR_EQUAL] = &&op_OP_GREATER_OR_EQUAL,
        [OP_LESS_OR_EQUAL] = &&op_OP_LESS_OR_EQUAL,
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
        [OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_CONST] = &&op_OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_CONST,
        [OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_CONST] =
            &&op_OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_CONST,
        [OP_JUMP_IF_EQUAL_LOCAL_CONST] = &&op_OP_JUMP_IF_EQUAL_LOCAL_CONST,
        [OP_JUMP_IF_NOT_EQUAL_LOCAL_CONST] = &&op_OP_JUMP_IF_NOT_EQUAL_LOCAL_CONST,
        [OP_JUMP_IF_NOT_LESS_LOCAL_LOCAL] = &&op_OP_JUMP_IF_NOT_LESS_LOCAL_LOCAL,
        [OP_JUMP_IF_NOT_GREATER_LOCAL_LOCAL] = &&op_OP_JUMP_IF_NOT_GREATER_LOCAL_LOCAL,
        [OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_LOCAL] = &&op_OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_LOCAL,
        [OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_LOCAL] =
            &&op_OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_LOCAL,
        [OP_JUMP_IF_EQUAL_LOCAL_LOCAL] = &&op_OP_JUMP_IF_EQUAL_LOCAL_LOCAL,
        [OP_JUMP_IF_NOT_EQUAL_LOCAL_LOCAL] = &&op_OP_JUMP_IF_NOT_EQUAL_LOCAL_LOCAL,
        [OP_LOOP] = &&op_OP_LOOP,
        [OP_DEFAULT_ARG] = &&op_OP_DEFAULT_ARG,
        [OP_CALL] = &&op_OP_CALL,
        [OP_INVOKE] = &&op_OP_INVOKE,
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
    if (__builtin_expect(vm.debug_mode, 0)) {
        goto cold;
    }
    DISPATCH();
#else
    for (;;) {
        if (__builtin_expect(vm.debug_mode, 0)) {
            goto cold;
        }
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

        TARGET(OP_GET_LOCAL_16):
            *sp++ = slots[U16_AT(ip + 1)];
            ip += 3;
            DISPATCH();

        TARGET(OP_SET_LOCAL_16):
            slots[U16_AT(ip + 1)] = *--sp;
            ip += 3;
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

        TARGET(OP_LESS_OR_EQUAL): {
            Value a = sp[-2];
            Value b = sp[-1];

            if (a.type == VAL_INT && b.type == VAL_INT) {
                sp--;
                sp[-1] = BOOL_VAL(AS_INT(a) <= AS_INT(b));
                ip += 1;
                DISPATCH();
            }
            if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                sp--;
                sp[-1] = BOOL_VAL(AS_NUMBER(a) <= AS_NUMBER(b));
                ip += 1;
                DISPATCH();
            }
            if (!numeric_fast(a) || !numeric_fast(b)) {
                goto cold;
            }
            sp--;
            sp[-1] = BOOL_VAL(value_as_double(a) <= value_as_double(b));
            ip += 1;
            DISPATCH();
        }

        TARGET(OP_GREATER_OR_EQUAL): {
            Value a = sp[-2];
            Value b = sp[-1];

            if (a.type == VAL_INT && b.type == VAL_INT) {
                sp--;
                sp[-1] = BOOL_VAL(AS_INT(a) >= AS_INT(b));
                ip += 1;
                DISPATCH();
            }
            if (a.type == VAL_NUMBER && b.type == VAL_NUMBER) {
                sp--;
                sp[-1] = BOOL_VAL(AS_NUMBER(a) >= AS_NUMBER(b));
                ip += 1;
                DISPATCH();
            }
            if (!numeric_fast(a) || !numeric_fast(b)) {
                goto cold;
            }
            sp--;
            sp[-1] = BOOL_VAL(value_as_double(a) >= value_as_double(b));
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

        FUSED_JUMP_LOCAL_CONST(OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_CONST)
        FUSED_JUMP_LOCAL_CONST(OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_CONST)
        FUSED_JUMP_LOCAL_CONST(OP_JUMP_IF_EQUAL_LOCAL_CONST)
        FUSED_JUMP_LOCAL_CONST(OP_JUMP_IF_NOT_EQUAL_LOCAL_CONST)

        FUSED_JUMP_LOCAL_LOCAL(OP_JUMP_IF_NOT_LESS_LOCAL_LOCAL)
        FUSED_JUMP_LOCAL_LOCAL(OP_JUMP_IF_NOT_GREATER_LOCAL_LOCAL)
        FUSED_JUMP_LOCAL_LOCAL(OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_LOCAL)
        FUSED_JUMP_LOCAL_LOCAL(OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_LOCAL)
        FUSED_JUMP_LOCAL_LOCAL(OP_JUMP_IF_EQUAL_LOCAL_LOCAL)
        FUSED_JUMP_LOCAL_LOCAL(OP_JUMP_IF_NOT_EQUAL_LOCAL_LOCAL)

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

            if (__builtin_expect(sp + 256 >= vm.stack + vm.stack_capacity, 0)) {
                goto cold;
            }

            /* Fast path: a closure or native called with the right number of arguments.
             * Everything else (arity errors, non-callables) goes through the general path. */
            if (callee.type == VAL_OBJ) {
                Obj *obj = AS_OBJ(callee);
                if (obj->type == OBJ_CLOSURE) {
                    ObjClosure *closure = (ObjClosure *)obj;
                    ObjFunction *function = closure->function;

                    if (!function->is_varargs &&
                        arg_count >= function->min_arity &&
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
                        frame->self = NIL_VAL;
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
                        if (__builtin_expect(!vm.had_error && !vm.has_exception, 1)) {
                            *native_base = result;
                            sp = native_base + 1;
                            ip += 2;
                            DISPATCH();
                        }
                        goto handle_runtime_error;
                    }
                }
            }
            goto cold;
        }

        TARGET(OP_INVOKE): {
            ObjString *method_name = AS_STRING(constants[U16_AT(ip + 1)]);
            int arg_count = ip[3];
            Value receiver = sp[-(arg_count + 1)];

            if (receiver.type == VAL_OBJ && receiver.as.obj->type == OBJ_MAP) {
                ObjMap *map = (ObjMap *)receiver.as.obj;
                MethodCacheEntry *cache = &method_cache[method_cache_slot(method_name, map)];

                if (cache->name == method_name && cache->entries == map->entries &&
                    cache->capacity == map->capacity && cache->index < map->capacity) {
                    MapEntry *entry = &map->entries[cache->index];

                    if (entry->state == 1 && entry->key.type == VAL_OBJ &&
                        entry->key.as.obj == (Obj *)method_name && entry->value.type == VAL_OBJ) {
                        Obj *method_obj = entry->value.as.obj;

                        if (method_obj->type == OBJ_CLOSURE) {
                            ObjClosure *closure = (ObjClosure *)method_obj;
                            ObjFunction *function = closure->function;

                            if (!function->is_varargs &&
                                arg_count >= function->min_arity &&
                                arg_count <= function->arity &&
                                frame < &vm.frames[FRAMES_MAX - 1]) {
                                int original_argc = arg_count;

                                sp[-(arg_count + 1)] = entry->value;
                                while (arg_count < function->arity) {
                                    *sp++ = NIL_VAL;
                                    arg_count++;
                                }
                                frame->ip = ip + 4;
                                frame++;
                                frame->closure = closure;
                                frame->ip = function->chunk.code;
                                frame->slots = sp - function->arity - 1;
                                frame->constants = function->chunk.constants;
                                frame->argc = original_argc;
                                frame->self = receiver;
                                ip = function->chunk.code;
                                slots = frame->slots;
                                constants = frame->constants;
                                DISPATCH();
                            }
                        }
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

                    if (!function->is_varargs &&
                        arg_count >= function->min_arity &&
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
                        if (__builtin_expect(!vm.had_error && !vm.has_exception, 1)) {
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
                        goto handle_runtime_error;
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
            if (__builtin_expect(vm.try_handler_count > 0, 0)) {
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
            if (__builtin_expect(frame >= &vm.frames[FRAMES_MAX - 1], 0) ||
                __builtin_expect(sp + 256 >= vm.stack + vm.stack_capacity, 0)) {
                goto cold;
            }
            frame->ip = ip + 4;
            CallFrame *prev = frame;
            frame++;
            frame->closure = prev->closure;
            frame->ip = frame->closure->function->chunk.code;
            frame->slots = sp;
            frame->constants = constants;
            frame->self = prev->self;
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
            if (__builtin_expect(vm.try_handler_count > 0, 0)) {
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
        if (__builtin_expect(vm.debug_mode, 0)) {
            int offset = (int)(ip - frame->closure->function->chunk.code);
            int line = (offset >= 0 && offset < frame->closure->function->chunk.count) ?
                       frame->closure->function->chunk.lines[offset] : 1;
            const char *file = frame->closure->function->script_path ?
                               frame->closure->function->script_path->chars : "<script>";
            debugger_check_stop(file, line, frame);
            ip = frame->ip;
            sp = vm.stack_top;
        }
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

        case OP_GET_LOCAL_16: {
            uint16_t slot = READ_U16();

            push(frame->slots[slot]);
            break;
        }

        case OP_SET_LOCAL_16: {
            uint16_t slot = READ_U16();

            frame->slots[slot] = pop();
            break;
        }

        case OP_GET_GLOBAL: {
            uint16_t slot = READ_U16();
            GlobalSlot *global = &vm.global_slots[slot];

            if (!global->defined) {
                vm_runtime_error(ERROR_NAME, "global variable '%s' not defined", global->name->chars);
                goto handle_runtime_error;
            }
            push(global->value);
            break;
        }

        case OP_DEFINE_GLOBAL: {
            uint16_t slot = READ_U16();
            GlobalSlot *global = &vm.global_slots[slot];

            if (global->is_const && global->defined) {
                vm_runtime_error(ERROR_CONST, "cannot redefine constant '%s'", global->name->chars);
                goto handle_runtime_error;
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
                goto handle_runtime_error;
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
                goto handle_runtime_error;
            }
            if (global->is_const) {
                vm_runtime_error(ERROR_CONST, "cannot assign to constant '%s'", global->name->chars);
                goto handle_runtime_error;
            }
            global->value = pop();
            break;
        }

        case OP_IMPORT: {
            uint16_t slot = READ_U16();
            ObjString *path_str = AS_STRING(frame->closure->function->chunk.constants[slot]);

            if (!import_module(path_str, frame->closure->function->script_path)) {
                goto handle_runtime_error;
            }
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
                    goto handle_runtime_error;
                }
                if (position < 0) {
                    position += array->count;
                }
                if (position < 0 || position >= array->count) {
                    vm_runtime_error(ERROR_INDEX, "index out of range: %d (size %d)", position,
                                     array->count);
                    goto handle_runtime_error;
                }
                result = array->items[position];
            } else if (is_obj_type(object, OBJ_MAP)) {
                Value stored;

                if (map_get(AS_MAP(object), index, &stored)) {
                    result = stored;
                }
            } else if (is_obj_type(object, OBJ_INSTANCE)) {
                ObjInstance *instance = AS_INSTANCE(object);

                /* A string key always addresses a field first. Dot access is
                 * compiled as a string index, so this also keeps `self.x`
                 * usable inside `__index__`; the dunder handles other keys. */
                if (is_obj_type(index, OBJ_STRING) &&
                    map_get(instance->fields, index, &result)) {
                    /* Field found. */
                } else if (is_obj_type(index, OBJ_STRING) &&
                           !class_find_method(instance->klass,
                                              object_string("__index__", 9))) {
                    vm_runtime_error(ERROR_NAME, "field '%s' not found on instance of '%s'",
                                     AS_STRING(index)->chars,
                                     instance->klass && instance->klass->name
                                         ? instance->klass->name->chars
                                         : "?");
                    goto handle_runtime_error;
                } else {
                    DunderStatus dunder = call_dunder(object, "__index__", 1, &index, &result);

                    if (dunder == DUNDER_ERROR) {
                        goto handle_runtime_error;
                    }
                    if (dunder == DUNDER_ABSENT) {
                        if (!is_obj_type(index, OBJ_STRING)) {
                            vm_runtime_error(ERROR_TYPE,
                                             "cannot index an instance of '%s' with %s; "
                                             "define '__index__' or use a string field name",
                                             instance->klass && instance->klass->name
                                                 ? instance->klass->name->chars
                                                 : "?",
                                             value_type_name(index));
                        } else {
                            vm_runtime_error(ERROR_NAME,
                                             "field '%s' not found on instance of '%s'",
                                             AS_STRING(index)->chars,
                                             instance->klass && instance->klass->name
                                                 ? instance->klass->name->chars
                                                 : "?");
                        }
                        goto handle_runtime_error;
                    }
                }
            } else if (is_obj_type(object, OBJ_CLASS)) {
                ObjClass *klass = AS_CLASS(object);

                if (!is_obj_type(index, OBJ_STRING)) {
                    vm_runtime_error(ERROR_TYPE, "a class member name must be a string, received %s",
                                     value_type_name(index));
                    goto handle_runtime_error;
                }
                Value *static_value = class_find_static(klass, AS_STRING(index));

                if (static_value) {
                    result = *static_value;
                } else {
                    ObjClosure *method = class_find_method(klass, AS_STRING(index));

                    if (!method) {
                        vm_runtime_error(ERROR_NAME, "class '%s' has no member '%s'",
                                         klass->name ? klass->name->chars : "?",
                                         AS_STRING(index)->chars);
                        goto handle_runtime_error;
                    }
                    result = OBJ_VAL(method);
                }
            } else if (is_obj_type(object, OBJ_STRING)) {
                ObjString *string = AS_STRING(object);
                int position;
                int byte_index;
                int char_length;

                if (!index_to_position(index, &position)) {
                    goto handle_runtime_error;
                }
                int total_chars = utf8_length(string->chars, string->length);
                if (position < 0) {
                    position += total_chars;
                }
                if (position < 0 || position >= total_chars) {
                    vm_runtime_error(ERROR_INDEX, "index out of range: %d (string of %d characters)",
                                     position, total_chars);
                    goto handle_runtime_error;
                }
                byte_index = utf8_byte_offset(string, position);
                char_length = utf8_char_length(string, byte_index);

                result = OBJ_VAL(object_string(string->chars + byte_index, char_length));
            } else {
                vm_runtime_error(ERROR_TYPE,
                                 "cannot index a %s; expected an array, a map or a string",
                                 value_type_name(object));
                goto handle_runtime_error;
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
                    goto handle_runtime_error;
                }
                if (position < 0) {
                    position += array->count;
                }
                if (position < 0 || position >= array->count) {
                    vm_runtime_error(ERROR_INDEX, "index out of range: %d (size %d)", position,
                                     array->count);
                    goto handle_runtime_error;
                }
                array->items[position] = value;
            } else if (is_obj_type(object, OBJ_MAP)) {
                map_set(AS_MAP(object), index, value);
            } else if (is_obj_type(object, OBJ_INSTANCE)) {
                /* String keys are fields: dot assignment compiles as a string
                 * index, so this keeps `self.x = v` working inside
                 * `__index_set__` instead of recursing. */
                if (is_obj_type(index, OBJ_STRING)) {
                    map_set(AS_INSTANCE(object)->fields, index, value);
                } else {
                    Value args[2] = { index, value };
                    Value ignored;
                    DunderStatus dunder =
                        call_dunder(object, "__index_set__", 2, args, &ignored);

                    if (dunder == DUNDER_ERROR) {
                        goto handle_runtime_error;
                    }
                    if (dunder == DUNDER_ABSENT) {
                        vm_runtime_error(ERROR_TYPE,
                                         "cannot assign an index of type %s on an instance of "
                                         "'%s'; define '__index_set__' or use a string field name",
                                         value_type_name(index),
                                         AS_INSTANCE(object)->klass &&
                                                 AS_INSTANCE(object)->klass->name
                                             ? AS_INSTANCE(object)->klass->name->chars
                                             : "?");
                        goto handle_runtime_error;
                    }
                }
            } else if (is_obj_type(object, OBJ_CLASS)) {
                if (!is_obj_type(index, OBJ_STRING)) {
                    vm_runtime_error(ERROR_TYPE, "a class member name must be a string, received %s",
                                     value_type_name(index));
                    goto handle_runtime_error;
                }
                table_set(&AS_CLASS(object)->statics, AS_STRING(index), value);
            } else {
                vm_runtime_error(ERROR_TYPE,
                                 "can only assign to an array, a map, an instance or a class, "
                                 "tried to assign to a %s",
                                 value_type_name(object));
                goto handle_runtime_error;
            }
            vm.stack_top -= 3;
            break;
        }

        case OP_INDEX_INC: {
            uint8_t flags = READ_BYTE();
            bool decrement = (flags & 1) != 0;
            bool prefix = (flags & 2) != 0;
            Value index = peek(0);
            Value object = peek(1);
            Value old_value = NIL_VAL;
            Value new_value = NIL_VAL;

            if (is_obj_type(object, OBJ_ARRAY)) {
                ObjArray *array = AS_ARRAY(object);
                int position;

                if (!index_to_position(index, &position)) {
                    goto handle_runtime_error;
                }
                if (position < 0) {
                    position += array->count;
                }
                if (position < 0 || position >= array->count) {
                    vm_runtime_error(ERROR_INDEX, "index out of range: %d (size %d)", position,
                                     array->count);
                    goto handle_runtime_error;
                }
                old_value = array->items[position];
                if (decrement) {
                    if (!arithmetic_values(OP_SUBTRACT, old_value, INT_VAL(1), &new_value)) {
                        goto handle_runtime_error;
                    }
                } else if (!add_values(old_value, INT_VAL(1), &new_value)) {
                    goto handle_runtime_error;
                }
                array->items[position] = new_value;
            } else if (is_obj_type(object, OBJ_MAP)) {
                ObjMap *map = AS_MAP(object);

                if (map_get(map, index, &old_value)) {
                    if (!value_is_numeric(old_value)) {
                        vm_runtime_error(ERROR_TYPE,
                                         "'%s' expects a numeric value, received %s",
                                         decrement ? "--" : "++", value_type_name(old_value));
                        goto handle_runtime_error;
                    }
                } else {
                    vm_runtime_error(ERROR_NAME, "'%s' cannot update a missing map key",
                                     decrement ? "--" : "++");
                    goto handle_runtime_error;
                }
                if (decrement) {
                    if (!arithmetic_values(OP_SUBTRACT, old_value, INT_VAL(1), &new_value)) {
                        goto handle_runtime_error;
                    }
                } else if (!add_values(old_value, INT_VAL(1), &new_value)) {
                    goto handle_runtime_error;
                }
                map_set(map, index, new_value);
            } else {
                vm_runtime_error(ERROR_TYPE,
                                 "can only update an array or a map element, tried to update a %s",
                                 value_type_name(object));
                goto handle_runtime_error;
            }
            vm.stack_top[-2] = prefix ? new_value : old_value;
            vm.stack_top--;
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
            goto handle_runtime_error;
        }

        case OP_ITER_PAIR: {
            Value index_val = vm.stack_top[-1];
            Value keys_val = vm.stack_top[-2];
            Value original = vm.stack_top[-3];
            Value first;
            Value second;

            if (!is_obj_type(keys_val, OBJ_ARRAY) || !is_int(index_val)) {
                vm_runtime_error(ERROR_RUNTIME, "corrupt iteration state");
                goto handle_runtime_error;
            }
            int position = (int)AS_INT(index_val);
            ObjArray *keys = AS_ARRAY(keys_val);

            if (position < 0 || position >= keys->count) {
                vm_runtime_error(ERROR_INDEX, "index out of range: %d (size %d)", position,
                                 keys->count);
                goto handle_runtime_error;
            }
            if (is_obj_type(original, OBJ_MAP)) {
                Value stored = NIL_VAL;

                first = keys->items[position];
                map_get(AS_MAP(original), first, &stored);
                second = stored;
            } else {
                first = INT_VAL(position);
                second = keys->items[position];
            }
            vm.stack_top[-3] = first;
            vm.stack_top[-2] = second;
            vm.stack_top--;
            break;
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
            } else if (is_obj_type(value, OBJ_INSTANCE)) {
                Value result;
                DunderStatus status = call_dunder(value, "__len__", 0, NULL, &result);

                if (status == DUNDER_ERROR) {
                    goto handle_runtime_error;
                }
                if (status == DUNDER_ABSENT) {
                    vm_runtime_error(ERROR_TYPE, "'len' expects an array, a map, a string or an "
                                     "instance with '__len__', received %s",
                                     value_type_name(value));
                    goto handle_runtime_error;
                }
                if (!is_int(result) || AS_INT(result) < 0) {
                    vm_runtime_error(ERROR_TYPE, "'__len__' must return a non-negative integer, "
                                     "received %s", value_type_name(result));
                    goto handle_runtime_error;
                }
                length = AS_INT(result);
            } else {
                vm_runtime_error(ERROR_TYPE, "'len' expects an array, a map, a string or an "
                                 "instance with '__len__', received %s",
                                 value_type_name(value));
                goto handle_runtime_error;
            }
            /* Leaves the original value and pushes its length: the compiler uses it
             * to initialize the counter of a for-in. */
            push(INT_VAL(length));
            break;
        }

        case OP_TO_STRING: {
            ObjString *text = vm_value_to_string(peek(0));

            if (!text) {
                goto handle_runtime_error;
            }
            vm.stack_top[-1] = OBJ_VAL(text);
            break;
        }

        case OP_EQUAL: {
            Value b = pop();
            Value a = pop();
            bool equal;

            if (!equality_values(a, b, &equal)) {
                goto handle_runtime_error;
            }
            push(BOOL_VAL(equal));
            break;
        }

        case OP_LESS_LOCAL_CONST:
        case OP_GREATER_LOCAL_CONST: {
            uint8_t slot = READ_BYTE();
            Value b = frame->closure->function->chunk.constants[READ_U16()];
            bool result;

            if (!compare_operator(frame->slots[slot], b,
                                  instruction == OP_GREATER_LOCAL_CONST ? "__gt__" : "__lt__",
                                  instruction == OP_GREATER_LOCAL_CONST, false, &result)) {
                goto handle_runtime_error;
            }
            push(BOOL_VAL(result));
            break;
        }

        case OP_GREATER:
        case OP_LESS: {
            Value b = pop();
            Value a = pop();
            bool result;

            if (!compare_operator(a, b, instruction == OP_GREATER ? "__gt__" : "__lt__",
                                  instruction == OP_GREATER, false, &result)) {
                goto handle_runtime_error;
            }
            push(BOOL_VAL(result));
            break;
        }

        case OP_LESS_OR_EQUAL:
        case OP_GREATER_OR_EQUAL: {
            Value b = pop();
            Value a = pop();
            bool result;

            if (a.type == VAL_INT && b.type == VAL_INT) {
                result = instruction == OP_LESS_OR_EQUAL
                             ? AS_INT(a) <= AS_INT(b)
                             : AS_INT(a) >= AS_INT(b);
                push(BOOL_VAL(result));
                break;
            }
            if (value_is_numeric(a) && value_is_numeric(b)) {
                result = instruction == OP_LESS_OR_EQUAL
                             ? value_as_double(a) <= value_as_double(b)
                             : value_as_double(a) >= value_as_double(b);
                push(BOOL_VAL(result));
                break;
            }
            /* Strings have a total order, so `a <= b` is `!(a > b)` and
             * `a >= b` is `!(a < b)`; instances may define their own dunder and
             * anything else is a type error. */
            if (!compare_operator(a, b,
                                  instruction == OP_LESS_OR_EQUAL ? "__le__" : "__ge__",
                                  instruction == OP_LESS_OR_EQUAL, true, &result)) {
                goto handle_runtime_error;
            }
            push(BOOL_VAL(result));
            break;
        }

        case OP_ADD_LOCAL_CONST: {
            uint8_t slot = READ_BYTE();
            Value b = frame->closure->function->chunk.constants[READ_U16()];
            Value result;

            if (!add_values(frame->slots[slot], b, &result)) {
                goto handle_runtime_error;
            }
            push(result);
            break;
        }

        case OP_ADD: {
            Value b = pop();
            Value a = pop();
            Value result;

            if (!add_values(a, b, &result)) {
                goto handle_runtime_error;
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
                goto handle_runtime_error;
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
                goto handle_runtime_error;
            }
            push(result);
            break;
        }

        case OP_INT_DIVIDE: {
            Value b = pop();
            Value a = pop();
            Value result;

            if (!int_divide_values(a, b, &result)) {
                goto handle_runtime_error;
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
                goto handle_runtime_error;
            }
            push(result);
            break;
        }

        case OP_POW: {
            Value b = pop();
            Value a = pop();
            Value result;

            if (is_obj_type(a, OBJ_INSTANCE)) {
                DunderStatus status = call_dunder(a, "__pow__", 1, &b, &result);

                if (status == DUNDER_OK) {
                    push(result);
                    break;
                }
                if (status == DUNDER_ERROR) {
                    goto handle_runtime_error;
                }
            }
            if (!value_is_numeric(a) || !value_is_numeric(b)) {
                vm_runtime_error(ERROR_TYPE, "operands of '**' must be numbers");
                goto handle_runtime_error;
            }
            push(NUMBER_VAL(pow(value_as_double(a), value_as_double(b))));
            break;
        }

        case OP_IN: {
            Value coll = pop();
            Value item = pop();
            if (is_obj_type(coll, OBJ_ARRAY)) {
                ObjArray *arr = AS_ARRAY(coll);
                bool found = false;
                for (int i = 0; i < arr->count; i++) {
                    if (values_equal(arr->items[i], item)) {
                        found = true;
                        break;
                    }
                }
                push(BOOL_VAL(found));
            } else if (is_obj_type(coll, OBJ_MAP)) {
                Value val;
                push(BOOL_VAL(map_get(AS_MAP(coll), item, &val)));
            } else if (is_obj_type(coll, OBJ_STRING)) {
                if (!is_obj_type(item, OBJ_STRING)) {
                    vm_runtime_error(ERROR_TYPE, "'in' with a string requires a string needle");
                    goto handle_runtime_error;
                }
                ObjString *str = AS_STRING(coll);
                ObjString *sub = AS_STRING(item);
                if (sub->length == 0) {
                    push(BOOL_VAL(true));
                } else {
                    push(BOOL_VAL(strstr(str->chars, sub->chars) != NULL));
                }
            } else {
                vm_runtime_error(ERROR_TYPE, "cannot use 'in' on %s", value_type_name(coll));
                goto handle_runtime_error;
            }
            break;
        }

        case OP_INSTANCEOF: {
            Value target = pop();
            Value inst = pop();
            bool result = false;

            if (is_obj_type(target, OBJ_CLASS)) {
                result = value_is_instance_of(inst, AS_CLASS(target));
            } else {
                ObjString *target_name = NULL;

                /* A type name also works; the class chain is searched so
                 * `instance instanceof "Base"` respects inheritance. */
                if (is_obj_type(target, OBJ_STRING)) {
                    target_name = AS_STRING(target);
                } else if (is_obj_type(target, OBJ_CLOSURE)) {
                    target_name = AS_CLOSURE(target)->function->name;
                } else if (is_obj_type(target, OBJ_FUNCTION)) {
                    target_name = AS_FUNCTION(target)->name;
                }
                if (target_name) {
                    ObjClass *klass = value_class(inst);

                    if (klass) {
                        for (ObjClass *current = klass; current; current = current->superclass) {
                            if (class_has_name(current, target_name)) {
                                result = true;
                                break;
                            }
                        }
                    } else {
                        result = strcmp(value_type_name(inst), target_name->chars) == 0;
                    }
                }
            }
            push(BOOL_VAL(result));
            break;
        }

        case OP_SLICE: {
            Value step_val = pop();
            Value end_val = pop();
            Value start_val = pop();
            Value obj_val = pop();

            int step = 1;
            if (!is_nil(step_val)) {
                if (!integer_argument(step_val, "step", "slice", &step)) {
                    goto handle_runtime_error;
                }
                if (step == 0) {
                    vm_runtime_error(ERROR_VALUE, "slice step cannot be zero");
                    goto handle_runtime_error;
                }
            }

            if (is_obj_type(obj_val, OBJ_ARRAY)) {
                ObjArray *arr = AS_ARRAY(obj_val);
                int len = arr->count;
                int start = step > 0 ? 0 : len - 1;
                int end = step > 0 ? len : -1;

                if (!is_nil(start_val)) {
                    if (!integer_argument(start_val, "start", "slice", &start)) goto handle_runtime_error;
                    if (start < 0) start += len;
                    if (start < 0) start = step > 0 ? 0 : -1;
                    if (start >= len) start = step > 0 ? len : len - 1;
                }
                if (!is_nil(end_val)) {
                    if (!integer_argument(end_val, "end", "slice", &end)) goto handle_runtime_error;
                    if (end < 0) end += len;
                    if (end < 0) end = step > 0 ? 0 : -1;
                    if (end >= len) end = step > 0 ? len : len - 1;
                }

                ObjArray *res = object_array();
                push(OBJ_VAL(res));
                if (step > 0) {
                    for (int i = start; i < end; i += step) {
                        array_push(res, arr->items[i]);
                    }
                } else {
                    for (int i = start; i > end; i += step) {
                        array_push(res, arr->items[i]);
                    }
                }
                break;
            } else if (is_obj_type(obj_val, OBJ_STRING)) {
                ObjString *str = AS_STRING(obj_val);
                int len = utf8_length(str->chars, str->length);
                int start = step > 0 ? 0 : len - 1;
                int end = step > 0 ? len : -1;

                if (!is_nil(start_val)) {
                    if (!integer_argument(start_val, "start", "slice", &start)) goto handle_runtime_error;
                    if (start < 0) start += len;
                    if (start < 0) start = step > 0 ? 0 : -1;
                    if (start >= len) start = step > 0 ? len : len - 1;
                }
                if (!is_nil(end_val)) {
                    if (!integer_argument(end_val, "end", "slice", &end)) goto handle_runtime_error;
                    if (end < 0) end += len;
                    if (end < 0) end = step > 0 ? 0 : -1;
                    if (end >= len) end = step > 0 ? len : len - 1;
                }

                buf_t b;
                buf_init(&b);
                if (step > 0) {
                    for (int i = start; i < end; i += step) {
                        int byte_off = utf8_byte_offset(str, i);
                        int char_len = utf8_char_length(str, byte_off);
                        buf_append(&b, str->chars + byte_off, (size_t)char_len);
                    }
                } else {
                    for (int i = start; i > end; i += step) {
                        int byte_off = utf8_byte_offset(str, i);
                        int char_len = utf8_char_length(str, byte_off);
                        buf_append(&b, str->chars + byte_off, (size_t)char_len);
                    }
                }
                ObjString *res = object_string(b.data ? b.data : "", (int)b.len);
                buf_free(&b);
                push(OBJ_VAL(res));
                break;
            } else {
                vm_runtime_error(ERROR_TYPE, "cannot slice %s", value_type_name(obj_val));
                goto handle_runtime_error;
            }
        }

        case OP_NOT:
            push(BOOL_VAL(value_is_falsey(pop())));
            break;

        case OP_NEGATE: {
            Value value = pop();

            if (is_obj_type(value, OBJ_INSTANCE)) {
                Value result;
                DunderStatus status = call_dunder(value, "__neg__", 0, NULL, &result);

                if (status == DUNDER_OK) {
                    push(result);
                    break;
                }
                if (status == DUNDER_ERROR) {
                    goto handle_runtime_error;
                }
            }
            if (value.type == VAL_INT) {
                int64_t integer = AS_INT(value);

                push(integer == INT64_MIN ? NUMBER_VAL(TWO_POW_63) : INT_VAL(-integer));
            } else if (value.type == VAL_NUMBER) {
                push(NUMBER_VAL(-AS_NUMBER(value)));
            } else {
                vm_runtime_error(ERROR_TYPE, "negation expects a number, received %s",
                                 value_type_name(value));
                goto handle_runtime_error;
            }
            break;
        }

        case OP_BIT_NOT: {
            Value value = pop();

            if (!is_int(value)) {
                vm_runtime_error(ERROR_TYPE, "the bitwise complement expects an integer, received %s",
                                 value_type_name(value));
                goto handle_runtime_error;
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
        case OP_JUMP_IF_NOT_GREATER_LOCAL_CONST:
        case OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_CONST:
        case OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_CONST:
        case OP_JUMP_IF_EQUAL_LOCAL_CONST:
        case OP_JUMP_IF_NOT_EQUAL_LOCAL_CONST: {
            uint8_t slot = READ_BYTE();
            Value b = frame->closure->function->chunk.constants[READ_U16()];
            uint16_t offset = READ_U16();
            bool jump;

            if (!fused_comparison_jump((OpCode)instruction, frame->slots[slot], b, &jump) &&
                !slow_comparison_jump((OpCode)instruction, frame->slots[slot], b, &jump)) {
                goto handle_runtime_error;
            }
            if (jump) {
                frame->ip += offset;
            }
            break;
        }

        case OP_JUMP_IF_NOT_LESS_LOCAL_LOCAL:
        case OP_JUMP_IF_NOT_GREATER_LOCAL_LOCAL:
        case OP_JUMP_IF_NOT_LESS_OR_EQUAL_LOCAL_LOCAL:
        case OP_JUMP_IF_NOT_GREATER_OR_EQUAL_LOCAL_LOCAL:
        case OP_JUMP_IF_EQUAL_LOCAL_LOCAL:
        case OP_JUMP_IF_NOT_EQUAL_LOCAL_LOCAL: {
            uint8_t slot_a = READ_BYTE();
            uint8_t slot_b = READ_BYTE();
            uint16_t offset = READ_U16();
            Value a = frame->slots[slot_a];
            Value b = frame->slots[slot_b];
            bool jump;

            if (!fused_comparison_jump((OpCode)instruction, a, b, &jump) &&
                !slow_comparison_jump((OpCode)instruction, a, b, &jump)) {
                goto handle_runtime_error;
            }
            if (jump) {
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
                if (vm.has_exception) {
                    vm.had_error = false;
                    push(vm.exception_value);
                    vm.has_exception = false;
                    goto do_throw;
                }
                goto handle_runtime_error;
            }
            if (vm.has_exception) {
                vm.had_error = false;
                push(vm.exception_value);
                vm.has_exception = false;
                goto do_throw;
            }
            if (vm.had_error) {
                goto handle_runtime_error;
            }
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }

        case OP_CLASS: {
            uint16_t name_slot = READ_U16();
            ObjString *name = AS_STRING(frame->closure->function->chunk.constants[name_slot]);
            Value super_val = pop();
            ObjClass *superclass = NULL;

            if (!is_nil(super_val)) {
                if (!is_obj_type(super_val, OBJ_CLASS)) {
                    vm_runtime_error(ERROR_TYPE,
                                     "a class can only extend another class, received %s",
                                     value_type_name(super_val));
                    goto handle_runtime_error;
                }
                superclass = AS_CLASS(super_val);
            }
            push(OBJ_VAL(object_class(name, superclass)));
            break;
        }

        case OP_CLASS_FIELD_INIT: {
            Value closure_val = peek(0);
            ObjClass *klass = AS_CLASS(peek(1));

            if (!is_obj_type(closure_val, OBJ_CLOSURE)) {
                vm_runtime_error(ERROR_TYPE, "field initializers must be functions");
                goto handle_runtime_error;
            }
            AS_CLOSURE(closure_val)->klass = klass;
            klass->field_init = AS_CLOSURE(closure_val);
            pop();
            break;
        }

        case OP_METHOD: {
            uint16_t name_slot = READ_U16();
            bool is_static = READ_BYTE() != 0;
            ObjString *name = AS_STRING(frame->closure->function->chunk.constants[name_slot]);
            Value method_val = peek(0);
            ObjClass *klass = AS_CLASS(peek(1));

            if (!is_obj_type(method_val, OBJ_CLOSURE)) {
                vm_runtime_error(ERROR_TYPE, "methods must be functions");
                goto handle_runtime_error;
            }
            AS_CLOSURE(method_val)->klass = klass;
            AS_CLOSURE(method_val)->is_static = is_static;
            Table *target = is_static ? &klass->statics : &klass->methods;

            table_set(target, name, method_val);
            pop();
            break;
        }

        case OP_SUPER: {
            uint8_t arg_count = READ_BYTE();
            ObjClass *klass = frame->closure->klass;

            if (!klass || !klass->superclass) {
                vm_runtime_error(ERROR_NAME, "'super' requires a superclass");
                goto handle_runtime_error;
            }
            ObjClosure *method = class_find_method(klass->superclass, object_string("init", 4));
            if (!method) {
                vm_runtime_error(ERROR_NAME, "the superclass '%s' does not define 'init'",
                                 klass->superclass->name ? klass->superclass->name->chars : "?");
                goto handle_runtime_error;
            }
            if (!call_method(method, arg_count, peek(arg_count))) {
                if (vm.has_exception) {
                    vm.had_error = false;
                    push(vm.exception_value);
                    vm.has_exception = false;
                    goto do_throw;
                }
                goto handle_runtime_error;
            }
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }

        case OP_SUPER_INVOKE: {
            uint16_t name_slot = READ_U16();
            uint8_t arg_count = READ_BYTE();
            ObjString *method_name = AS_STRING(frame->closure->function->chunk.constants[name_slot]);
            ObjClass *klass = frame->closure->klass;
            bool is_static = frame->closure->is_static;

            if (!klass || !klass->superclass) {
                vm_runtime_error(ERROR_NAME, "'super' requires a superclass");
                goto handle_runtime_error;
            }

            if (is_static) {
                Value *static_value = class_find_static(klass->superclass, method_name);

                if (!static_value) {
                    vm_runtime_error(ERROR_NAME,
                                     "static method '%s' not found in the superclass of '%s'",
                                     method_name->chars,
                                     klass->name ? klass->name->chars : "?");
                    goto handle_runtime_error;
                }
                if (is_obj_type(*static_value, OBJ_CLOSURE)) {
                    if (!call_method(AS_CLOSURE(*static_value), arg_count, NIL_VAL)) {
                        if (vm.has_exception) {
                            vm.had_error = false;
                            push(vm.exception_value);
                            vm.has_exception = false;
                            goto do_throw;
                        }
                        goto handle_runtime_error;
                    }
                    frame = &vm.frames[vm.frame_count - 1];
                    break;
                }
                if (is_obj_type(*static_value, OBJ_NATIVE)) {
                    *(vm.stack_top - arg_count - 1) = *static_value;
                    if (!call_value(*static_value, arg_count)) {
                        if (vm.has_exception) {
                            vm.had_error = false;
                            push(vm.exception_value);
                            vm.has_exception = false;
                            goto do_throw;
                        }
                        goto handle_runtime_error;
                    }
                    break;
                }
                vm_runtime_error(ERROR_TYPE, "static '%s' is not a function", method_name->chars);
                goto handle_runtime_error;
            }

            ObjClosure *method = class_find_method(klass->superclass, method_name);
            if (!method) {
                vm_runtime_error(ERROR_NAME, "method '%s' not found in the superclass of '%s'",
                                 method_name->chars,
                                 klass->name ? klass->name->chars : "?");
                goto handle_runtime_error;
            }
            if (!call_method(method, arg_count, peek(arg_count))) {
                if (vm.has_exception) {
                    vm.had_error = false;
                    push(vm.exception_value);
                    vm.has_exception = false;
                    goto do_throw;
                }
                goto handle_runtime_error;
            }
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }

        case OP_NEW: {
            int arg_count = READ_BYTE();

            if (!invoke_constructor(arg_count)) {
                if (vm.has_exception) {
                    vm.had_error = false;
                    push(vm.exception_value);
                    vm.has_exception = false;
                    goto do_throw;
                }
                goto handle_runtime_error;
            }
            if (vm.had_error) {
                goto handle_runtime_error;
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
                    goto handle_runtime_error;
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
                    goto handle_runtime_error;
                }
                Value *native_base = vm.stack_top - arg_count - 1;
                Value result = native->function(arg_count, native_base + 1);
                if (vm.had_error || vm.has_exception) {
                    goto handle_runtime_error;
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
            goto handle_runtime_error;
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
                goto handle_runtime_error;
            }
            ObjArray *src_arr = AS_ARRAY(src);
            for (int i = 0; i < src_arr->count; i++) {
                array_push(AS_ARRAY(dest), src_arr->items[i]);
            }
            break;
        }

        case OP_NEW_SPREAD: {
            Value args = pop();
            if (!is_obj_type(args, OBJ_ARRAY)) {
                vm_runtime_error(ERROR_TYPE, "spread operator '...' requires an array, received %s",
                                 value_type_name(args));
                goto handle_runtime_error;
            }
            ObjArray *arr = AS_ARRAY(args);
            int count = arr->count;
            if (!ensure_stack(count + 256)) {
                goto handle_runtime_error;
            }
            for (int i = 0; i < count; i++) {
                push(arr->items[i]);
            }
            if (!invoke_constructor(count)) {
                if (vm.has_exception) {
                    vm.had_error = false;
                    push(vm.exception_value);
                    vm.has_exception = false;
                    goto do_throw;
                }
                goto handle_runtime_error;
            }
            if (vm.had_error) {
                goto handle_runtime_error;
            }
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }

        case OP_CALL_SPREAD: {
            Value args = pop();
            if (!is_obj_type(args, OBJ_ARRAY)) {
                vm_runtime_error(ERROR_TYPE, "spread operator '...' requires an array, received %s",
                                 value_type_name(args));
                goto handle_runtime_error;
            }
            ObjArray *arr = AS_ARRAY(args);
            int count = arr->count;
            if (!ensure_stack(count + 256)) {
                goto handle_runtime_error;
            }
            for (int i = 0; i < count; i++) {
                push(arr->items[i]);
            }
            if (!call_value(peek(count), count)) {
                if (vm.has_exception) {
                    vm.had_error = false;
                    push(vm.exception_value);
                    vm.has_exception = false;
                    goto do_throw;
                }
                goto handle_runtime_error;
            }
            if (vm.has_exception) {
                vm.had_error = false;
                push(vm.exception_value);
                vm.has_exception = false;
                goto do_throw;
            }
            if (vm.had_error) {
                goto handle_runtime_error;
            }
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }

        case OP_TAIL_CALL_SPREAD: {
            Value args = pop();
            if (!is_obj_type(args, OBJ_ARRAY)) {
                vm_runtime_error(ERROR_TYPE, "spread operator '...' requires an array, received %s",
                                 value_type_name(args));
                goto handle_runtime_error;
            }
            ObjArray *arr = AS_ARRAY(args);
            int arg_count = arr->count;
            if (!ensure_stack(arg_count + 256)) {
                goto handle_runtime_error;
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
                    goto handle_runtime_error;
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
                    goto handle_runtime_error;
                }
                Value *native_base = vm.stack_top - arg_count - 1;
                Value result = native->function(arg_count, native_base + 1);
                if (vm.had_error || vm.has_exception) {
                    goto handle_runtime_error;
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
            goto handle_runtime_error;
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
            push(result);
            goto op_return_shared;
        }

        case OP_RETURN_ADD: {
            Value b = pop();
            Value a = pop();
            Value result;
            if (!add_values(a, b, &result)) {
                goto handle_runtime_error;
            }
            push(result);
            goto op_return_shared;
        }

        case OP_RETURN: {
        op_return_shared: ;
            Value result = pop();

            while (vm.try_handler_count > 0 &&
                   vm.try_handlers[vm.try_handler_count - 1].frame_count == vm.frame_count) {
                TryHandler *h = &vm.try_handlers[--vm.try_handler_count];

                if (h->is_finally) {
                    close_upvalues(vm.stack + h->stack_count);
                    vm.stack_top = vm.stack + h->stack_count;
                    push(result);
                    push(INT_VAL(1));
                    frame->ip = h->target_ip;
                    goto end_instruction;
                }
            }

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

        case OP_PUSH_CATCH: {
            uint16_t offset = READ_U16();

            if (vm.try_handler_count == TRY_HANDLERS_MAX) {
                vm_runtime_error(ERROR_RUNTIME, "too many nested try handlers");
                goto handle_runtime_error;
            }
            TryHandler *h = &vm.try_handlers[vm.try_handler_count++];
            h->frame_count = vm.frame_count;
            h->stack_count = (int)(vm.stack_top - vm.stack);
            h->target_ip = frame->ip + offset;
            h->is_finally = false;
            break;
        }

        case OP_PUSH_FINALLY: {
            uint16_t offset = READ_U16();

            if (vm.try_handler_count == TRY_HANDLERS_MAX) {
                vm_runtime_error(ERROR_RUNTIME, "too many nested try handlers");
                goto handle_runtime_error;
            }
            TryHandler *h = &vm.try_handlers[vm.try_handler_count++];
            h->frame_count = vm.frame_count;
            h->stack_count = (int)(vm.stack_top - vm.stack);
            h->target_ip = frame->ip + offset;
            h->is_finally = true;
            break;
        }

        case OP_POP_TRY: {
            if (vm.try_handler_count > 0) {
                vm.try_handler_count--;
            }
            break;
        }

        case OP_END_FINALLY: {
            Value action_val = pop();
            Value saved_val = pop();
            int action = (int)AS_INT(action_val);

            if (action == 1) {
                push(saved_val);
                goto op_return_shared;
            } else if (action == 2) {
                push(saved_val);
                goto do_throw;
            }
            break;
        }

        case OP_THROW: {
        do_throw: ;
            Value exception_val = vm.has_exception ? vm.exception_value : pop();
            vm.has_exception = false;

            while (vm.try_handler_count > 0) {
                TryHandler *h = &vm.try_handlers[vm.try_handler_count - 1];

                if (base_frame > 0 && h->frame_count <= base_frame) {
                    vm.has_exception = true;
                    vm.exception_value = exception_val;
                    return INTERPRET_RUNTIME_ERROR;
                }
                vm.try_handler_count--;

                while (vm.frame_count > h->frame_count) {
                    close_upvalues(vm.frames[vm.frame_count - 1].slots);
                    vm.frame_count--;
                }

                frame = &vm.frames[vm.frame_count - 1];
                close_upvalues(vm.stack + h->stack_count);
                vm.stack_top = vm.stack + h->stack_count;

                if (!h->is_finally) {
                    push(exception_val);
                    frame->ip = h->target_ip;
                    goto end_instruction;
                } else {
                    push(exception_val);
                    push(INT_VAL(2));
                    frame->ip = h->target_ip;
                    goto end_instruction;
                }
            }

            ObjString *msg = value_to_string(exception_val);
            vm_runtime_error(ERROR_RUNTIME, "uncaught exception: %s", msg->chars);
            return INTERPRET_RUNTIME_ERROR;
        }

        case OP_INVOKE: {
            uint16_t name_slot = READ_U16();
            int arg_count = READ_BYTE();
            ObjString *method_name = AS_STRING(frame->closure->function->chunk.constants[name_slot]);

            Value receiver = peek(arg_count);

            if (is_obj_type(receiver, OBJ_MAP)) {
                ObjMap *receiver_map = AS_MAP(receiver);
                Value method_val;
                int method_index;

                if (map_get_index(receiver_map, OBJ_VAL(method_name), &method_val,
                                  &method_index)) {
                    MethodCacheEntry *cache =
                        &method_cache[method_cache_slot(method_name, receiver_map)];

                    cache->name = method_name;
                    cache->entries = receiver_map->entries;
                    cache->capacity = receiver_map->capacity;
                    cache->index = method_index;

                    /* Replace receiver on stack with the method */
                    *(vm.stack_top - arg_count - 1) = method_val;

                    if (is_obj_type(method_val, OBJ_CLOSURE)) {
                        if (!call_closure((ObjClosure *)AS_OBJ(method_val), arg_count)) {
                            if (vm.has_exception) {
                                vm.had_error = false;
                                push(vm.exception_value);
                                vm.has_exception = false;
                                goto do_throw;
                            }
                            goto handle_runtime_error;
                        }
                        /* Bind self on the newly created frame */
                        vm.frames[vm.frame_count - 1].self = receiver;
                        frame = &vm.frames[vm.frame_count - 1];
                    } else if (is_obj_type(method_val, OBJ_NATIVE)) {
                        if (!call_value(method_val, arg_count)) {
                            if (vm.has_exception) {
                                vm.had_error = false;
                                push(vm.exception_value);
                                vm.has_exception = false;
                                goto do_throw;
                            }
                            goto handle_runtime_error;
                        }
                    } else if (is_nil(method_val)) {
                        vm_runtime_error(ERROR_TYPE,
                                         "field '%s' is nil; check it before calling",
                                         method_name->chars);
                        goto handle_runtime_error;
                    } else {
                        vm_runtime_error(ERROR_TYPE, "field '%s' is not a function", method_name->chars);
                        goto handle_runtime_error;
                    }
                    break;
                }
            }

            if (is_obj_type(receiver, OBJ_INSTANCE)) {
                ObjInstance *instance = AS_INSTANCE(receiver);
                Value field_value;

                /* A field holding a function shadows a method, so callbacks
                 * stored on instances keep working. */
                if (map_get(instance->fields, OBJ_VAL(method_name), &field_value)) {
                    if (is_obj_type(field_value, OBJ_CLOSURE)) {
                        if (!call_method(AS_CLOSURE(field_value), arg_count, receiver)) {
                            if (vm.has_exception) {
                                vm.had_error = false;
                                push(vm.exception_value);
                                vm.has_exception = false;
                                goto do_throw;
                            }
                            goto handle_runtime_error;
                        }
                        frame = &vm.frames[vm.frame_count - 1];
                        break;
                    }
                    if (is_obj_type(field_value, OBJ_NATIVE)) {
                        if (!call_value(field_value, arg_count)) {
                            if (vm.has_exception) {
                                vm.had_error = false;
                                push(vm.exception_value);
                                vm.has_exception = false;
                                goto do_throw;
                            }
                            goto handle_runtime_error;
                        }
                        break;
                    }
                    if (is_nil(field_value)) {
                        vm_runtime_error(ERROR_TYPE, "field '%s' is nil; check it before calling",
                                         method_name->chars);
                        goto handle_runtime_error;
                    }
                    vm_runtime_error(ERROR_TYPE, "field '%s' is not a function", method_name->chars);
                    goto handle_runtime_error;
                }

                ObjClosure *method = class_find_method(instance->klass, method_name);
                if (!method) {
                    vm_runtime_error(ERROR_NAME, "method '%s' not found on instance of '%s'",
                                     method_name->chars,
                                     instance->klass && instance->klass->name
                                         ? instance->klass->name->chars
                                         : "?");
                    goto handle_runtime_error;
                }
                if (!call_method(method, arg_count, receiver)) {
                    if (vm.has_exception) {
                        vm.had_error = false;
                        push(vm.exception_value);
                        vm.has_exception = false;
                        goto do_throw;
                    }
                    goto handle_runtime_error;
                }
                frame = &vm.frames[vm.frame_count - 1];
                break;
            }

            if (is_obj_type(receiver, OBJ_CLASS)) {
                ObjClass *receiver_class = AS_CLASS(receiver);
                Value *static_value = class_find_static(receiver_class, method_name);

                if (!static_value) {
                    if (class_find_method(receiver_class, method_name)) {
                        vm_runtime_error(ERROR_TYPE,
                                         "method '%s' is an instance method; call it on an instance of '%s'",
                                         method_name->chars,
                                         receiver_class->name ? receiver_class->name->chars : "?");
                    } else {
                        vm_runtime_error(ERROR_NAME, "class '%s' has no static method '%s'",
                                         receiver_class->name ? receiver_class->name->chars : "?",
                                         method_name->chars);
                    }
                    goto handle_runtime_error;
                }
                if (is_obj_type(*static_value, OBJ_CLOSURE)) {
                    if (!call_method(AS_CLOSURE(*static_value), arg_count, NIL_VAL)) {
                        if (vm.has_exception) {
                            vm.had_error = false;
                            push(vm.exception_value);
                            vm.has_exception = false;
                            goto do_throw;
                        }
                        goto handle_runtime_error;
                    }
                    frame = &vm.frames[vm.frame_count - 1];
                    break;
                }
                if (is_obj_type(*static_value, OBJ_NATIVE)) {
                    if (!call_value(*static_value, arg_count)) {
                        if (vm.has_exception) {
                            vm.had_error = false;
                            push(vm.exception_value);
                            vm.has_exception = false;
                            goto do_throw;
                        }
                        goto handle_runtime_error;
                    }
                    break;
                }
                vm_runtime_error(ERROR_TYPE, "static '%s' is not a function", method_name->chars);
                goto handle_runtime_error;
            }

            Value native_res;
            if (dispatch_native_method(receiver, method_name, arg_count, &native_res)) {
                if (vm.has_exception) {
                    vm.had_error = false;
                    push(vm.exception_value);
                    vm.has_exception = false;
                    goto do_throw;
                }
                if (vm.had_error) {
                    goto handle_runtime_error;
                }
                vm.stack_top -= (arg_count + 1);
                push(native_res);
                break;
            }

            if (is_obj_type(receiver, OBJ_MAP)) {
                vm_runtime_error(ERROR_NAME, "method '%s' not found on map", method_name->chars);
            } else if (is_obj_type(receiver, OBJ_STRING) || is_obj_type(receiver, OBJ_ARRAY) ||
                       is_obj_type(receiver, OBJ_STRING_BUILDER) || is_obj_type(receiver, OBJ_FILE)) {
                vm_runtime_error(ERROR_NAME, "method '%s' not found on %s", method_name->chars,
                                 value_type_name(receiver));
            } else {
                vm_runtime_error(ERROR_TYPE,
                                 "method call receiver must be a map, an instance or a class, "
                                 "received %s",
                                 value_type_name(receiver));
            }
            goto handle_runtime_error;
        }

        case OP_INVOKE_SPREAD: {
            uint16_t name_slot = READ_U16();
            ObjString *method_name = AS_STRING(frame->closure->function->chunk.constants[name_slot]);

            Value args_val = pop();
            if (!is_obj_type(args_val, OBJ_ARRAY)) {
                vm_runtime_error(ERROR_TYPE, "spread operator '...' requires an array, received %s",
                                 value_type_name(args_val));
                goto handle_runtime_error;
            }

            Value receiver = peek(0);
            Value method_val = NIL_VAL;
            bool found = false;
            bool bind_self = true;

            if (is_obj_type(receiver, OBJ_MAP)) {
                found = map_get(AS_MAP(receiver), OBJ_VAL(method_name), &method_val);
            } else if (is_obj_type(receiver, OBJ_INSTANCE)) {
                ObjInstance *instance = AS_INSTANCE(receiver);

                if (map_get(instance->fields, OBJ_VAL(method_name), &method_val)) {
                    found = true;
                } else {
                    ObjClosure *method = class_find_method(instance->klass, method_name);

                    if (method) {
                        method_val = OBJ_VAL(method);
                        found = true;
                    }
                }
            } else if (is_obj_type(receiver, OBJ_CLASS)) {
                Value *static_value = class_find_static(AS_CLASS(receiver), method_name);

                if (static_value) {
                    method_val = *static_value;
                    found = true;
                    bind_self = false;
                }
            } else {
                vm_runtime_error(ERROR_TYPE,
                                 "method call receiver must be a map, an instance or a class, "
                                 "received %s",
                                 value_type_name(receiver));
                goto handle_runtime_error;
            }

            if (!found) {
                vm_runtime_error(ERROR_NAME, "method '%s' not found on %s", method_name->chars,
                                 value_type_name(receiver));
                goto handle_runtime_error;
            }

            ObjArray *arr = AS_ARRAY(args_val);
            int count = arr->count;
            if (!ensure_stack(count + 256)) {
                goto handle_runtime_error;
            }

            /* Replace receiver on stack with the method */
            vm.stack_top[-1] = method_val;

            for (int i = 0; i < count; i++) {
                push(arr->items[i]);
            }

            if (is_obj_type(method_val, OBJ_CLOSURE)) {
                if (!call_method(AS_CLOSURE(method_val), count,
                                 bind_self ? receiver : NIL_VAL)) {
                    if (vm.has_exception) {
                        vm.had_error = false;
                        push(vm.exception_value);
                        vm.has_exception = false;
                        goto do_throw;
                    }
                    goto handle_runtime_error;
                }
                frame = &vm.frames[vm.frame_count - 1];
            } else if (is_obj_type(method_val, OBJ_NATIVE)) {
                if (!call_value(method_val, count)) {
                    if (vm.has_exception) {
                        vm.had_error = false;
                        push(vm.exception_value);
                        vm.has_exception = false;
                        goto do_throw;
                    }
                    goto handle_runtime_error;
                }
            } else {
                vm_runtime_error(ERROR_TYPE, "field '%s' is not a function", method_name->chars);
                goto handle_runtime_error;
            }
            break;
        }

        case OP_SELF: {
            if (is_nil(frame->self)) {
                vm_runtime_error(ERROR_NAME, "'self' is not bound (not in a method call)");
                goto handle_runtime_error;
            }
            push(frame->self);
            break;
        }

        case OP_CALL_SELF_SUB_LOCAL_CONST: {
            uint8_t slot = READ_BYTE();
            Value b = READ_CONSTANT();
            Value a = frame->slots[slot];
            Value arg;
            if (!arithmetic_values(OP_SUBTRACT, a, b, &arg)) {
                goto handle_runtime_error;
            }
            push(frame->slots[0]);
            push(arg);
            if (!call_closure(frame->closure, 1)) {
                goto handle_runtime_error;
            }
            frame = &vm.frames[vm.frame_count - 1];
            break;
        }

        default:
            vm_runtime_error(ERROR_RUNTIME, "unknown instruction (%d)", instruction);
            goto handle_runtime_error;
        }

    end_instruction:
        /* The general path may have changed the frame, the stack or the ip. */
        frame = &vm.frames[vm.frame_count - 1];
        sp = vm.stack_top;
        LOAD_FRAME();
        if (__builtin_expect(vm.debug_mode, 0)) {
            goto cold;
        }
#ifdef SPARK_COMPUTED_GOTO
        DISPATCH();
#else
    }
#endif

handle_runtime_error:
    if (vm.has_exception && vm.try_handler_count > 0) {
        vm.had_error = false;
        goto do_throw;
    }
    return INTERPRET_RUNTIME_ERROR;
}
#ifdef SPARK_COMPUTED_GOTO
#pragma GCC diagnostic pop
#endif

/* Calls `callee` with `arg_count` arguments already pushed on the stack and
 * stores its return value in *result. Used by higher-order natives. */
static bool call_callback_impl(Value callee, int arg_count, Value self, bool bind_self,
                               Value *result)
{
    if (!ensure_stack(arg_count + 256)) {
        return false;
    }
    int args_offset = (int)(vm.stack_top - vm.stack) - arg_count;
    Value *args = vm.stack + args_offset;

    if (is_obj_type(callee, OBJ_NATIVE)) {
        ObjNative *native = (ObjNative *)AS_OBJ(callee);

        if (native->arity >= 0 && arg_count != native->arity) {
            vm_runtime_error(ERROR_ARITY, "the function '%s' expects %d argument(s) but received %d",
                             native->name->chars, native->arity, arg_count);
            return false;
        }
        *result = native->function(arg_count, args);
        vm.stack_top = vm.stack + args_offset;
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

    /* `self` is only a C local until the frame starts; keep the collector from
     * freeing it while call_closure makes room for default arguments. */
    if (bind_self) {
        mem_pause_gc();
    }
    if (!call_closure((ObjClosure *)AS_OBJ(callee), arg_count)) {
        if (bind_self) {
            mem_resume_gc();
        }
        vm.stack_top = vm.stack + args_offset;
        return false;
    }
    if (bind_self) {
        vm.frames[vm.frame_count - 1].self = self;
        mem_resume_gc();
    }

    InterpretResult status = run(base_frame);

    if (status != INTERPRET_OK) {
        return false;
    }
    *result = pop();
    return true;
}

static bool call_callback(Value callee, int arg_count, Value *result)
{
    return call_callback_impl(callee, arg_count, NIL_VAL, false, result);
}

static bool call_callback_bound(Value callee, int arg_count, Value self, Value *result)
{
    return call_callback_impl(callee, arg_count, self, true, result);
}

/* Runs the field initializers of `klass` and its ancestors base-first. */
static bool initialize_instance_fields(ObjClass *klass, Value instance)
{
    ObjClass *chain[64];
    int depth = 0;

    for (ObjClass *current = klass; current; current = current->superclass) {
        if (depth == (int)(sizeof(chain) / sizeof(chain[0]))) {
            vm_runtime_error(ERROR_RUNTIME, "class hierarchy is too deep");
            return false;
        }
        chain[depth++] = current;
    }

    for (int i = depth - 1; i >= 0; i--) {
        if (!chain[i]->field_init) {
            continue;
        }
        Value result;

        if (!call_callback_bound(OBJ_VAL(chain[i]->field_init), 0, instance, &result)) {
            return false;
        }
    }
    return true;
}

static ObjString *vm_value_to_string(Value value)
{
    if (is_obj_type(value, OBJ_INSTANCE)) {
        Value result;
        DunderStatus status = call_dunder(value, "to_string", 0, NULL, &result);

        if (status == DUNDER_ERROR) {
            return NULL;
        }
        if (status == DUNDER_OK) {
            if (!is_obj_type(result, OBJ_STRING)) {
                vm_runtime_error(ERROR_TYPE, "to_string() must return a string, received %s",
                                 value_type_name(result));
                return NULL;
            }
            return AS_STRING(result);
        }
    }
    return value_to_string(value);
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
    debugger_init();
    vm.stack_capacity = STACK_INITIAL_CAPACITY;
    vm.stack = mem_realloc(NULL, 0, (size_t)vm.stack_capacity * sizeof(Value));
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
    define_native("gc", native_gc, 0);
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
    define_native("sin", native_sin, 1);
    define_native("cos", native_cos, 1);
    define_native("log", native_log, 1);
    define_native("exp", native_exp, 1);
    define_native("min", native_min, -1);
    define_native("max", native_max, -1);
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
    define_native("time", native_time, 0);
    define_native("sleep", native_sleep, 1);
    define_native("chr", native_chr, 1);
    define_native("ord", native_ord, 1);
    define_native("pad_start", native_pad_start, -1);
    define_native("pad_end", native_pad_end, -1);
    define_native("sum", native_sum, 1);
    define_native("any", native_any, -1);
    define_native("all", native_all, -1);
    define_native("enumerate", native_enumerate, 1);
    define_native("zip", native_zip, 2);
    define_native("isinstance", native_isinstance, 2);
    define_native("is_a", native_is_a, 2);
    define_native("copy", native_copy, 1);
    define_native("list_dir", native_list_dir, 1);
    define_native("mkdir", native_mkdir, 1);
    define_native("exec", native_exec, 1);
    define_native("string_builder", native_string_builder, -1);
    define_native("sb_append", native_sb_append, 2);
    define_native("sb_to_string", native_sb_to_string, 1);
    define_native("sb_len", native_sb_len, 1);
    define_native("sb_clear", native_sb_clear, 1);
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
    int pi_slot = vm_global_slot(object_string("pi", 2));
    if (pi_slot >= 0) {
        vm.global_slots[pi_slot].value = NUMBER_VAL(3.14159265358979323846);
        vm.global_slots[pi_slot].defined = true;
        vm.global_slots[pi_slot].is_const = true;
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

    ModuleEntry *mod = vm.modules;
    while (mod) {
        ModuleEntry *next = mod->next;
        free(mod->canonical_path);
        free(mod);
        mod = next;
    }
    vm.modules = NULL;

    if (vm.stack) {
        mem_realloc(vm.stack, (size_t)vm.stack_capacity * sizeof(Value), 0);
        vm.stack = NULL;
        vm.stack_capacity = 0;
        vm.stack_top = NULL;
    }

    debugger_free();

    memset(&vm, 0, sizeof(vm));
    vm.stack_top = vm.stack;
    vm_ready = false;
}

void vm_set_debug_mode(bool debug)
{
    vm.debug_mode = debug;
}

bool vm_get_debug_mode(void)
{
    return vm.debug_mode;
}

int vm_frame_count(void)
{
    return vm.frame_count;
}

CallFrame *vm_current_frame(void)
{
    return vm.frame_count > 0 ? &vm.frames[vm.frame_count - 1] : NULL;
}

const char *vm_frame_source(CallFrame *frame)
{
    if (!frame || !frame->closure || !frame->closure->function) {
        return NULL;
    }
    return frame->closure->function->source;
}

void vm_print_frame_locals(CallFrame *frame)
{
    if (!frame) {
        printf("  (no active frame)\n");
        return;
    }
    ObjFunction *fn = frame->closure->function;
    int stack_slots = (int)(vm.stack_top - frame->slots);
    int current_pc = (frame->ip && fn->chunk.code) ? (int)(frame->ip - fn->chunk.code) : 0;
    bool printed = false;

    printf("Locals:\n");
    for (int i = 0; i < fn->debug_local_count; i++) {
        DebugLocal *dl = &fn->debug_locals[i];
        if (current_pc >= dl->start_pc && current_pc < dl->end_pc) {
            int slot = dl->slot;
            if (slot >= 0 && slot < stack_slots) {
                printf("  %s = ", dl->name->chars);
                value_print(stdout, frame->slots[slot]);
                printf("\n");
                printed = true;
            }
        }
    }
    if (vm.frame_count == 1 && vm.global_count > 0) {
        for (int i = 0; i < vm.global_count; i++) {
            if (vm.global_slots[i].defined &&
                !is_obj_type(vm.global_slots[i].value, OBJ_NATIVE) &&
                strcmp(vm.global_slots[i].name->chars, "_") != 0 &&
                strcmp(vm.global_slots[i].name->chars, "nan") != 0) {
                printf("  %s = ", vm.global_slots[i].name->chars);
                value_print(stdout, vm.global_slots[i].value);
                printf("\n");
                printed = true;
            }
        }
    }
    if (!printed) {
        printf("  (no local variables)\n");
    }
}

void vm_print_stack_backtrace(void)
{
    printf("Call stack:\n");
    for (int i = vm.frame_count - 1; i >= 0; i--) {
        CallFrame *frame = &vm.frames[i];
        ObjFunction *fn = frame->closure->function;
        const char *fn_name = fn->name ? fn->name->chars : "<script>";
        const char *path = fn->script_path ? fn->script_path->chars : "<unknown>";
        int offset = (int)(frame->ip - fn->chunk.code);
        if (offset > 0 && offset <= fn->chunk.count) {
            offset--;
        }
        int line = (offset >= 0 && offset < fn->chunk.count) ? fn->chunk.lines[offset] : 1;
        printf("  #%d %s() at %s:%d\n", i, fn_name, path, line);
    }
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

const char *vm_add_source(const char *source)
{
    if (!source) return NULL;
    for (int i = 0; i < vm.source_count; i++) {
        if (strcmp(vm.sources[i].text, source) == 0) {
            return vm.sources[i].text;
        }
    }
    return vm_own_source(source);
}

static bool import_module(ObjString *path_str, ObjString *importer_path)
{
    char resolved[PATH_MAX];
    char target_path[PATH_MAX * 2];
    const char *dir = ".";
    char dir_buf[PATH_MAX];

    if (importer_path && importer_path->chars[0] != '<') {
        const char *last_slash = strrchr(importer_path->chars, '/');

        if (last_slash) {
            size_t dirlen = (size_t)(last_slash - importer_path->chars);

            if (dirlen == 0) {
                dir = "/";
            } else {
                if (dirlen >= sizeof(dir_buf)) {
                    dirlen = sizeof(dir_buf) - 1;
                }
                memcpy(dir_buf, importer_path->chars, dirlen);
                dir_buf[dirlen] = '\0';
                dir = dir_buf;
            }
        }
    }

    if (path_str->chars[0] == '/') {
        snprintf(target_path, sizeof(target_path), "%s", path_str->chars);
    } else if (strcmp(dir, "/") == 0) {
        snprintf(target_path, sizeof(target_path), "/%s", path_str->chars);
    } else {
        snprintf(target_path, sizeof(target_path), "%s/%s", dir, path_str->chars);
    }

    bool found = false;

    if (realpath(target_path, resolved) != NULL) {
        FILE *f = fopen(resolved, "rb");

        if (f) {
            fclose(f);
            found = true;
        }
    }

    if (!found) {
        size_t len = strlen(target_path);

        if (len < 6 || strcmp(target_path + len - 6, ".spark") != 0) {
            char target_spark[PATH_MAX * 2 + 16];

            snprintf(target_spark, sizeof(target_spark), "%s.spark", target_path);
            if (realpath(target_spark, resolved) != NULL) {
                FILE *f = fopen(resolved, "rb");

                if (f) {
                    fclose(f);
                    found = true;
                }
            }
        }
    }

    if (!found) {
        size_t len = strlen(target_path);

        if (len < 7 || strcmp(target_path + len - 7, ".sparkc") != 0) {
            char target_sparkc[PATH_MAX * 2 + 16];

            snprintf(target_sparkc, sizeof(target_sparkc), "%s.sparkc", target_path);
            if (realpath(target_sparkc, resolved) != NULL) {
                FILE *f = fopen(resolved, "rb");

                if (f) {
                    fclose(f);
                    found = true;
                }
            }
        }
    }

    if (!found) {
        vm_runtime_error(ERROR_RUNTIME, "could not open or resolve module '%s'", path_str->chars);
        return false;
    }

    for (ModuleEntry *entry = vm.modules; entry; entry = entry->next) {
        if (strcmp(entry->canonical_path, resolved) == 0) {
            return true;
        }
    }

    if (bytecode_is_bytecode_file(resolved)) {
        ModuleEntry *entry = malloc(sizeof(ModuleEntry));
        if (!entry) {
            vm_runtime_error(ERROR_MEMORY, "out of memory allocating module entry");
            return false;
        }
        entry->canonical_path = duplicate_string(resolved);
        if (!entry->canonical_path) {
            free(entry);
            vm_runtime_error(ERROR_MEMORY, "out of memory allocating module path");
            return false;
        }
        entry->status = MODULE_LOADING;
        entry->next = vm.modules;
        vm.modules = entry;

        char err[256];
        ObjFunction *function = bytecode_load(resolved, err, sizeof(err));
        if (!function) {
            vm_runtime_error(ERROR_RUNTIME, "%s", err);
            vm.modules = entry->next;
            free(entry->canonical_path);
            free(entry);
            return false;
        }
        push(OBJ_VAL(function));
        ObjClosure *closure = object_closure(function);
        pop();
        push(OBJ_VAL(closure));
        if (!call_closure(closure, 0)) {
            vm.modules = entry->next;
            free(entry->canonical_path);
            free(entry);
            return false;
        }
        InterpretResult result = run(vm.frame_count - 1);
        if (result != INTERPRET_OK) {
            return false;
        }
        entry->status = MODULE_LOADED;
        return true;
    }

    FILE *file = fopen(resolved, "rb");

    if (!file) {
        vm_runtime_error(ERROR_RUNTIME, "cannot read module file '%s'", resolved);
        return false;
    }
    fseek(file, 0L, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char *buf = malloc((size_t)size + 1);

    if (!buf) {
        fclose(file);
        vm_runtime_error(ERROR_MEMORY, "out of memory reading module '%s'", resolved);
        return false;
    }
    size_t bytes_read = fread(buf, sizeof(char), (size_t)size, file);
    buf[bytes_read] = '\0';
    fclose(file);

    ModuleEntry *entry = malloc(sizeof(ModuleEntry));

    if (!entry) {
        free(buf);
        vm_runtime_error(ERROR_MEMORY, "out of memory allocating module entry");
        return false;
    }
    entry->canonical_path = duplicate_string(resolved);
    if (!entry->canonical_path) {
        free(buf);
        free(entry);
        vm_runtime_error(ERROR_MEMORY, "out of memory allocating module path");
        return false;
    }
    entry->status = MODULE_LOADING;
    entry->next = vm.modules;
    vm.modules = entry;

    Arena arena;
    Stmt *program = NULL;
    char error[256];
    int error_line = 0;
    int error_column = 0;
    const char *owned = vm_own_source(buf);
    free(buf);

    arena_init(&arena);
    if (!parse_source(owned, &arena, &program, error, sizeof(error), &error_line, &error_column)) {
        error_print(ERROR_SYNTAX, error_line, error_column, owned, error);
        arena_free(&arena);
        vm.modules = entry->next;
        free(entry->canonical_path);
        free(entry);
        vm.had_error = true;
        return false;
    }

    ObjFunction *function = compile_ast(program, resolved, owned, false, error,
                                        sizeof(error), &error_line, &error_column);
    arena_free(&arena);
    if (!function) {
        error_print(ERROR_COMPILE, error_line, error_column, owned, error);
        vm.modules = entry->next;
        free(entry->canonical_path);
        free(entry);
        vm.had_error = true;
        return false;
    }

    push(OBJ_VAL(function));
    ObjClosure *closure = object_closure(function);
    pop();

    Value res = NIL_VAL;
    if (!call_callback(OBJ_VAL(closure), 0, &res) || vm.had_error) {
        vm.modules = entry->next;
        free(entry->canonical_path);
        free(entry);
        return false;
    }

    entry->status = MODULE_LOADED;
    return true;
}

InterpretResult vm_run_function(ObjFunction *function, const char *script_name)
{
    char canonical_main[PATH_MAX];
    ModuleEntry *main_entry = NULL;

    if (script_name && script_name[0] != '<') {
        if (realpath(script_name, canonical_main) != NULL) {
            main_entry = malloc(sizeof(ModuleEntry));
            if (main_entry) {
                main_entry->canonical_path = duplicate_string(canonical_main);
                if (main_entry->canonical_path) {
                    main_entry->status = MODULE_LOADING;
                    main_entry->next = vm.modules;
                    vm.modules = main_entry;
                } else {
                    free(main_entry);
                    main_entry = NULL;
                }
            }
        }
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
        vm.try_handler_count = 0;
        vm.has_exception = false;
        vm.exception_value = NIL_VAL;
        vm.had_error = false;
        return INTERPRET_RUNTIME_ERROR;
    }

    if (__builtin_expect(vm.debug_mode, 0)) {
        debugger_on_start(function);
    }

    InterpretResult result = run(0);

    if (main_entry) {
        main_entry->status = MODULE_LOADED;
    }

    close_upvalues(vm.stack);
    vm.stack_top = vm.stack;
    vm.frame_count = 0;
    vm.open_upvalues = NULL;
    vm.try_handler_count = 0;
    vm.has_exception = false;
    vm.exception_value = NIL_VAL;
    vm.had_error = false;
    return result;
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

    return vm_run_function(function, script_name);
}
