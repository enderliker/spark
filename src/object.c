#include "object.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "memory.h"

#define INTERN_BUCKET_COUNT 1024

static Obj *object_list = NULL;
static ObjString *intern_table[INTERN_BUCKET_COUNT];

Obj *objects_head(void)
{
    return object_list;
}

void objects_set_head(Obj *head)
{
    object_list = head;
}

static void *allocate_obj(size_t size, ObjType type)
{
    Obj *obj = mem_realloc(NULL, 0, size);

    obj->type = type;
    obj->marked = false;
    obj->next = object_list;
    object_list = obj;
    return obj;
}

static uint32_t hash_string(const char *chars, int length)
{
    uint32_t hash = 2166136261u;

    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)chars[i];
        hash *= 16777619u;
    }
    return hash;
}

static ObjString *intern_find(const char *chars, int length, uint32_t hash)
{
    ObjString *entry = intern_table[hash % INTERN_BUCKET_COUNT];

    while (entry) {
        if (entry->hash == hash && entry->length == length &&
            memcmp(entry->chars, chars, (size_t)length) == 0) {
            return entry;
        }
        entry = entry->intern_next;
    }
    return NULL;
}

ObjString *object_string(const char *chars, int length)
{
    uint32_t hash = hash_string(chars, length);
    ObjString *existing = intern_find(chars, length, hash);

    if (existing) {
        return existing;
    }

    ObjString *string = allocate_obj(sizeof(ObjString) + (size_t)length + 1, OBJ_STRING);
    string->hash = hash;
    string->length = length;
    memcpy(string->chars, chars, (size_t)length);
    string->chars[length] = '\0';

    uint32_t bucket = hash % INTERN_BUCKET_COUNT;
    string->intern_next = intern_table[bucket];
    intern_table[bucket] = string;
    return string;
}

ObjString *object_string_concat(ObjString *a, ObjString *b)
{
    if (a->length > INT_MAX - b->length) {
        return NULL;
    }
    int length = a->length + b->length;
    char *buffer = malloc((size_t)length + 1);

    if (!buffer) {
        return NULL;
    }
    memcpy(buffer, a->chars, (size_t)a->length);
    memcpy(buffer + a->length, b->chars, (size_t)b->length);
    buffer[length] = '\0';

    ObjString *result = object_string(buffer, length);
    free(buffer);
    return result;
}

ObjArray *object_array(void)
{
    ObjArray *array = allocate_obj(sizeof(ObjArray), OBJ_ARRAY);

    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
    return array;
}

static uint32_t hash_value(Value value)
{
    switch (value.type) {
    case VAL_NIL:
        return 0x811c9dc5u;
    case VAL_BOOL:
        return 0x9e3779b9u ^ (AS_BOOL(value) ? 0x85ebca6bu : 0u);
    case VAL_NUMBER: {
        uint64_t bits;

        memcpy(&bits, &value.as.number, sizeof(bits));
        if (value.as.number == 0) {
            bits = 0;
        }
        bits ^= bits >> 33;
        bits *= 0xff51afd7ed558ccdULL;
        bits ^= bits >> 33;
        return (uint32_t)bits;
    }
    case VAL_OBJ:
        if (value.as.obj->type == OBJ_STRING) {
            return ((ObjString *)value.as.obj)->hash;
        }
        return (uint32_t)(((uintptr_t)value.as.obj >> 4) * 2654435761u);
    }
    return 0;
}

static MapEntry *map_find_entry(MapEntry *entries, int capacity, Value key)
{
    uint32_t index = hash_value(key) & (uint32_t)(capacity - 1);
    MapEntry *tombstone = NULL;

    for (;;) {
        MapEntry *entry = &entries[index];

        if (entry->state == 0) {
            return tombstone ? tombstone : entry;
        }
        if (entry->state == 2) {
            if (!tombstone) {
                tombstone = entry;
            }
        } else if (values_equal(entry->key, key)) {
            return entry;
        }
        index = (index + 1) & (uint32_t)(capacity - 1);
    }
}

static void map_adjust_capacity(ObjMap *map, int capacity)
{
    MapEntry *entries = mem_realloc(NULL, 0, sizeof(MapEntry) * (size_t)capacity);
    MapEntry *old_entries = map->entries;
    int old_capacity = map->capacity;

    memset(entries, 0, sizeof(MapEntry) * (size_t)capacity);
    map->entries = entries;
    map->capacity = capacity;
    map->count = 0;
    map->tombstones = 0;

    for (int i = 0; i < old_capacity; i++) {
        MapEntry *old = &old_entries[i];

        if (old->state != 1) {
            continue;
        }
        MapEntry *entry = map_find_entry(entries, capacity, old->key);

        entry->key = old->key;
        entry->value = old->value;
        entry->state = 1;
        map->count++;
    }

    mem_realloc(old_entries, sizeof(MapEntry) * (size_t)old_capacity, 0);
}

ObjMap *object_map(void)
{
    ObjMap *map = allocate_obj(sizeof(ObjMap), OBJ_MAP);

    map->entries = NULL;
    map->count = 0;
    map->tombstones = 0;
    map->capacity = 0;
    return map;
}

bool map_get(ObjMap *map, Value key, Value *out)
{
    if (map->capacity == 0 || map->count == 0) {
        return false;
    }
    MapEntry *entry = map_find_entry(map->entries, map->capacity, key);

    if (entry->state != 1) {
        return false;
    }
    *out = entry->value;
    return true;
}

bool map_set(ObjMap *map, Value key, Value value)
{
    int capacity = map->capacity ? map->capacity : 8;

    if (map->capacity == 0 || (map->count + map->tombstones + 1) * 4 > map->capacity * 3) {
        if (map->capacity > 0 && (map->count + 1) * 2 > map->capacity) {
            capacity = map->capacity * 2;
        }
        map_adjust_capacity(map, capacity);
    }

    MapEntry *entry = map_find_entry(map->entries, map->capacity, key);
    if (entry->state == 1) {
        entry->value = value;
        return false;
    }
    if (entry->state == 2) {
        map->tombstones--;
    }
    entry->key = key;
    entry->value = value;
    entry->state = 1;
    map->count++;
    return true;
}

bool map_delete(ObjMap *map, Value key)
{
    if (map->capacity == 0 || map->count == 0) {
        return false;
    }
    MapEntry *entry = map_find_entry(map->entries, map->capacity, key);

    if (entry->state != 1) {
        return false;
    }
    entry->state = 2;
    map->count--;
    map->tombstones++;

    if (map->count == 0) {
        mem_realloc(map->entries, sizeof(MapEntry) * (size_t)map->capacity, 0);
        map->entries = NULL;
        map->capacity = 0;
        map->tombstones = 0;
    }
    return true;
}

bool array_push(ObjArray *array, Value value)
{
    if (array->count + 1 > array->capacity) {
        int capacity = array->capacity < 4 ? 4 : array->capacity * 2;
        array->items = mem_realloc(array->items, sizeof(Value) * (size_t)array->capacity,
                                   sizeof(Value) * (size_t)capacity);
        array->capacity = capacity;
    }
    array->items[array->count++] = value;
    return true;
}

bool array_pop(ObjArray *array, Value *out)
{
    if (array->count == 0) {
        return false;
    }
    *out = array->items[--array->count];
    return true;
}

ObjFunction *object_function(void)
{
    ObjFunction *function = allocate_obj(sizeof(ObjFunction), OBJ_FUNCTION);

    function->arity = 0;
    function->upvalue_count = 0;
    function->name = NULL;
    chunk_init(&function->chunk);
    return function;
}

ObjClosure *object_closure(ObjFunction *function)
{
    ObjClosure *closure;

    /* The closure is not on the VM stack yet: keep the GC from
     * collecting it while the upvalue array is allocated. */
    mem_pause_gc();
    closure = allocate_obj(sizeof(ObjClosure), OBJ_CLOSURE);
    closure->function = function;
    closure->upvalue_count = function->upvalue_count;
    if (closure->upvalue_count > 0) {
        closure->upvalues = mem_realloc(NULL, 0,
                                        sizeof(ObjUpvalue *) * (size_t)closure->upvalue_count);
        for (int i = 0; i < closure->upvalue_count; i++) {
            closure->upvalues[i] = NULL;
        }
    } else {
        closure->upvalues = NULL;
    }
    mem_resume_gc();
    return closure;
}

ObjNative *object_native(NativeFn fn, int arity, const char *name)
{
    ObjNative *native;

    mem_pause_gc();
    native = allocate_obj(sizeof(ObjNative), OBJ_NATIVE);
    native->function = fn;
    native->arity = arity;
    native->name = object_string(name, (int)strlen(name));
    mem_resume_gc();
    return native;
}

ObjUpvalue *object_upvalue(Value *slot)
{
    ObjUpvalue *upvalue = allocate_obj(sizeof(ObjUpvalue), OBJ_UPVALUE);

    upvalue->location = slot;
    upvalue->closed = NIL_VAL;
    upvalue->next = NULL;
    return upvalue;
}

void object_free(Obj *obj)
{
    switch (obj->type) {
    case OBJ_STRING:
        mem_realloc(obj, sizeof(ObjString) + (size_t)((ObjString *)obj)->length + 1, 0);
        break;
    case OBJ_ARRAY: {
        ObjArray *array = (ObjArray *)obj;
        mem_realloc(array->items, sizeof(Value) * (size_t)array->capacity, 0);
        mem_realloc(array, sizeof(ObjArray), 0);
        break;
    }
    case OBJ_MAP: {
        ObjMap *map = (ObjMap *)obj;
        mem_realloc(map->entries, sizeof(MapEntry) * (size_t)map->capacity, 0);
        mem_realloc(map, sizeof(ObjMap), 0);
        break;
    }
    case OBJ_FUNCTION: {
        ObjFunction *function = (ObjFunction *)obj;
        chunk_free(&function->chunk);
        mem_realloc(function, sizeof(ObjFunction), 0);
        break;
    }
    case OBJ_CLOSURE: {
        ObjClosure *closure = (ObjClosure *)obj;
        mem_realloc(closure->upvalues, sizeof(ObjUpvalue *) * (size_t)closure->upvalue_count, 0);
        mem_realloc(closure, sizeof(ObjClosure), 0);
        break;
    }
    case OBJ_NATIVE:
        mem_realloc(obj, sizeof(ObjNative), 0);
        break;
    case OBJ_UPVALUE:
        mem_realloc(obj, sizeof(ObjUpvalue), 0);
        break;
    }
}

void intern_remove_unmarked(void)
{
    for (int i = 0; i < INTERN_BUCKET_COUNT; i++) {
        ObjString **link = &intern_table[i];

        while (*link) {
            ObjString *string = *link;

            if (!string->obj.marked) {
                *link = string->intern_next;
                string->intern_next = NULL;
            } else {
                link = &string->intern_next;
            }
        }
    }
}

void intern_free_table(void)
{
    memset(intern_table, 0, sizeof(intern_table));
}

const char *obj_type_name(ObjType type)
{
    switch (type) {
    case OBJ_STRING: return "string";
    case OBJ_ARRAY: return "array";
    case OBJ_MAP: return "map";
    case OBJ_FUNCTION: return "function";
    case OBJ_CLOSURE: return "function";
    case OBJ_NATIVE: return "native function";
    case OBJ_UPVALUE: return "upvalue";
    }
    return "object";
}

bool values_equal(Value a, Value b)
{
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
    case VAL_NIL:
        return true;
    case VAL_BOOL:
        return a.as.boolean == b.as.boolean;
    case VAL_NUMBER:
        return a.as.number == b.as.number;
    case VAL_OBJ: {
        if (a.as.obj == b.as.obj) {
            return true;
        }
        if (a.as.obj->type == OBJ_STRING && b.as.obj->type == OBJ_STRING) {
            ObjString *sa = (ObjString *)a.as.obj;
            ObjString *sb = (ObjString *)b.as.obj;

            return sa->length == sb->length &&
                   memcmp(sa->chars, sb->chars, (size_t)sa->length) == 0;
        }
        return false;
    }
    }
    return false;
}

static void format_number(buf_t *b, double number)
{
    char buffer[64];

    if (isnan(number)) {
        buf_append_str(b, "nan");
        return;
    }
    if (isinf(number)) {
        buf_append_str(b, number > 0 ? "inf" : "-inf");
        return;
    }
    snprintf(buffer, sizeof(buffer), "%.15g", number);
    buf_append_str(b, buffer);
}

static void value_format(buf_t *b, Value v, bool repr)
{
    switch (v.type) {
    case VAL_NIL:
        buf_append_str(b, "nil");
        break;
    case VAL_BOOL:
        buf_append_str(b, AS_BOOL(v) ? "true" : "false");
        break;
    case VAL_NUMBER:
        format_number(b, AS_NUMBER(v));
        break;
    case VAL_OBJ:
        switch (AS_OBJ(v)->type) {
        case OBJ_STRING: {
            ObjString *s = AS_STRING(v);
            if (repr) {
                buf_append_str(b, "\"");
                for (int i = 0; i < s->length; i++) {
                    char c = s->chars[i];
                    switch (c) {
                    case '\n': buf_append_str(b, "\\n"); break;
                    case '\t': buf_append_str(b, "\\t"); break;
                    case '\r': buf_append_str(b, "\\r"); break;
                    case '"': buf_append_str(b, "\\\""); break;
                    case '\\': buf_append_str(b, "\\\\"); break;
                    default: buf_append(b, &c, 1); break;
                    }
                }
                buf_append_str(b, "\"");
            } else {
                buf_append(b, s->chars, (size_t)s->length);
            }
            break;
        }
        case OBJ_ARRAY: {
            ObjArray *array = (ObjArray *)AS_OBJ(v);
            buf_append_str(b, "[");
            for (int i = 0; i < array->count; i++) {
                if (i > 0) {
                    buf_append_str(b, ", ");
                }
                value_format(b, array->items[i], true);
            }
            buf_append_str(b, "]");
            break;
        }
        case OBJ_MAP: {
            ObjMap *map = (ObjMap *)AS_OBJ(v);
            int printed = 0;

            buf_append_str(b, "{");
            for (int i = 0; i < map->capacity; i++) {
                MapEntry *entry = &map->entries[i];

                if (entry->state != 1) {
                    continue;
                }
                if (printed++ > 0) {
                    buf_append_str(b, ", ");
                }
                value_format(b, entry->key, true);
                buf_append_str(b, ": ");
                value_format(b, entry->value, true);
            }
            buf_append_str(b, "}");
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction *fn = (ObjFunction *)AS_OBJ(v);
            buf_appendf(b, "<fn %s>", fn->name ? fn->name->chars : "anonymous");
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure *closure = (ObjClosure *)AS_OBJ(v);
            buf_appendf(b, "<fn %s>", closure->function->name ? closure->function->name->chars
                                                              : "anonymous");
            break;
        }
        case OBJ_NATIVE:
            buf_appendf(b, "<native %s>", ((ObjNative *)AS_OBJ(v))->name->chars);
            break;
        case OBJ_UPVALUE:
            buf_append_str(b, "<upvalue>");
            break;
        }
        break;
    }
}

void value_print(FILE *out, Value v)
{
    buf_t buffer;

    buf_init(&buffer);
    value_format(&buffer, v, false);
    fwrite(buffer.data ? buffer.data : "", 1, buffer.len, out);
    buf_free(&buffer);
}

void value_print_repr(FILE *out, Value v)
{
    buf_t buffer;

    buf_init(&buffer);
    value_format(&buffer, v, true);
    fwrite(buffer.data ? buffer.data : "", 1, buffer.len, out);
    buf_free(&buffer);
}

ObjString *value_to_string(Value v)
{
    buf_t buffer;
    ObjString *result;

    buf_init(&buffer);
    value_format(&buffer, v, false);
    result = object_string(buffer.data ? buffer.data : "", (int)buffer.len);
    buf_free(&buffer);
    return result;
}
