#include "object.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "memory.h"
#include "net.h"

#define INTERN_INITIAL_BUCKETS 4096

static Obj *object_list = NULL;
/* Chained hash table of interned strings. It starts small and doubles (rehashing
 * every entry) once the load factor passes 3/4, so lookups stay O(1) on average
 * no matter how many strings a program creates. */
static ObjString **intern_buckets = NULL;
static uint32_t intern_capacity = 0; /* always 0 or a power of two */
static size_t intern_count = 0;

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

/* `allocate_obj` has already linked the new object into the GC list and set its
 * header, so bulk zeroing must start after the header. Zeroing from the start
 * would clear `next` (truncating the list, hiding live objects from the sweep)
 * and `marked` (leaving stale marks that skip blackening later). */
static void zero_obj_body(void *object, size_t size)
{
    memset((char *)object + sizeof(Obj), 0, size - sizeof(Obj));
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

static void intern_grow(void)
{
    uint32_t new_capacity = intern_capacity == 0 ? 16384 : intern_capacity * 2;
    ObjString **new_buckets = calloc(new_capacity, sizeof(ObjString *));

    if (!new_buckets) {
        return; /* keep the current table: correct, just slower */
    }
    for (uint32_t i = 0; i < intern_capacity; i++) {
        ObjString *entry = intern_buckets[i];

        while (entry) {
            ObjString *next = entry->intern_next;
            uint32_t bucket = entry->hash & (new_capacity - 1);

            entry->intern_next = new_buckets[bucket];
            new_buckets[bucket] = entry;
            entry = next;
        }
    }
    free(intern_buckets);
    intern_buckets = new_buckets;
    intern_capacity = new_capacity;
}

static inline bool string_chars_equal(const char *a, const char *b, int len)
{
    if (len <= 16) {
        for (int i = 0; i < len; i++) {
            if (a[i] != b[i]) {
                return false;
            }
        }
        return true;
    }
    return memcmp(a, b, (size_t)len) == 0;
}

static ObjString *intern_find(const char *chars, int length, uint32_t hash)
{
    if (intern_capacity == 0) {
        return NULL;
    }
    ObjString *entry = intern_buckets[hash & (intern_capacity - 1)];

    while (entry) {
        if (entry->hash == hash && entry->length == length &&
            string_chars_equal(entry->chars, chars, length)) {
            return entry;
        }
        entry = entry->intern_next;
    }
    return NULL;
}

static ObjString *object_string_with_hash(const char *chars, int length, uint32_t hash)
{
    ObjString *existing = intern_find(chars, length, hash);

    if (existing) {
        return existing;
    }

    ObjString *string = allocate_obj(sizeof(ObjString) + (size_t)length + 1, OBJ_STRING);
    string->hash = hash;
    string->length = length;
    memcpy(string->chars, chars, (size_t)length);
    string->chars[length] = '\0';

    if ((intern_count + 1) * 4 > (size_t)intern_capacity * 3) {
        intern_grow();
    }
    if (intern_capacity != 0) {
        uint32_t bucket = hash & (intern_capacity - 1);
        string->intern_next = intern_buckets[bucket];
        intern_buckets[bucket] = string;
        intern_count++;
    }
    return string;
}

ObjString *object_string(const char *chars, int length)
{
    uint32_t hash = hash_string(chars, length);
    return object_string_with_hash(chars, length, hash);
}

ObjString *object_string_concat(ObjString *a, ObjString *b)
{
    if (a->length > INT_MAX - b->length) {
        return NULL;
    }
    int length = a->length + b->length;
    char stack_buf[512];
    char *buffer = length < (int)sizeof(stack_buf) ? stack_buf : malloc((size_t)length + 1);

    if (!buffer) {
        return NULL;
    }
    memcpy(buffer, a->chars, (size_t)a->length);
    memcpy(buffer + a->length, b->chars, (size_t)b->length);
    buffer[length] = '\0';

    uint32_t hash = a->hash;
    for (int i = 0; i < b->length; i++) {
        hash ^= (uint8_t)b->chars[i];
        hash *= 16777619u;
    }

    ObjString *result = intern_find(buffer, length, hash);
    if (!result) {
        result = allocate_obj(sizeof(ObjString) + (size_t)length + 1, OBJ_STRING);
        result->hash = hash;
        result->length = length;
        memcpy(result->chars, buffer, (size_t)length);
        result->chars[length] = '\0';

        if ((intern_count + 1) * 4 > (size_t)intern_capacity * 3) {
            intern_grow();
        }
        if (intern_capacity != 0) {
            uint32_t bucket = hash & (intern_capacity - 1);
            result->intern_next = intern_buckets[bucket];
            intern_buckets[bucket] = result;
            intern_count++;
        }
    }

    if (buffer != stack_buf) {
        free(buffer);
    }
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
    case VAL_INT: {
        uint64_t bits = (uint64_t)value.as.integer;

        bits ^= bits >> 33;
        bits *= 0xff51afd7ed558ccdULL;
        bits ^= bits >> 33;
        return (uint32_t)bits;
    }
    case VAL_NUMBER: {
        double number = value.as.number;
        uint64_t bits;

        /* A whole number in the int64 range hashes exactly like the integer,
         * so map[1] and map[1.0] share the same entry. */
        if (number >= -TWO_POW_63 && number < TWO_POW_63) {
            int64_t integer = (int64_t)number;

            if ((double)integer == number) {
                return hash_value(INT_VAL(integer));
            }
        }
        memcpy(&bits, &number, sizeof(bits));
        if (number == 0) {
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

static inline bool keys_equal(Value a, Value b)
{
    if (a.type == b.type) {
        if (a.type == VAL_OBJ) {
            return a.as.obj == b.as.obj;
        }
        if (a.type == VAL_INT) {
            return a.as.integer == b.as.integer;
        }
        if (a.type == VAL_NUMBER) {
            return a.as.number == b.as.number;
        }
        if (a.type == VAL_BOOL) {
            return a.as.boolean == b.as.boolean;
        }
        return true;
    }
    if (a.type == VAL_INT && b.type == VAL_NUMBER) {
        return int_equals_number(a.as.integer, b.as.number);
    }
    if (a.type == VAL_NUMBER && b.type == VAL_INT) {
        return int_equals_number(b.as.integer, a.as.number);
    }
    return false;
}

static MapEntry *map_find_entry(MapEntry *entries, int capacity, Value key, uint32_t hash)
{
    uint32_t index = hash & (uint32_t)(capacity - 1);
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
        } else if (entry->hash == hash && keys_equal(entry->key, key)) {
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
        uint32_t index = old->hash & (uint32_t)(capacity - 1);
        while (entries[index].state != 0) {
            index = (index + 1) & (uint32_t)(capacity - 1);
        }
        entries[index] = *old;
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

ObjClass *object_class(ObjString *name, ObjClass *superclass)
{
    ObjClass *klass;

    /* The class is not reachable from the VM until OP_CLASS finishes. */
    mem_pause_gc();
    klass = allocate_obj(sizeof(ObjClass), OBJ_CLASS);
    klass->name = name;
    klass->superclass = superclass;
    table_init(&klass->methods);
    table_init(&klass->statics);
    klass->field_init = NULL;
    mem_resume_gc();
    return klass;
}

ObjInstance *object_instance(ObjClass *klass)
{
    ObjInstance *instance;

    mem_pause_gc();
    instance = allocate_obj(sizeof(ObjInstance), OBJ_INSTANCE);
    instance->klass = klass;
    instance->fields = object_map();
    mem_resume_gc();
    return instance;
}

ObjClosure *class_find_method(ObjClass *klass, ObjString *name)
{
    for (ObjClass *current = klass; current; current = current->superclass) {
        Value value;

        if (table_get(&current->methods, name, &value) && is_obj_type(value, OBJ_CLOSURE)) {
            return AS_CLOSURE(value);
        }
    }
    return NULL;
}

Value *class_find_static(ObjClass *klass, ObjString *name)
{
    for (ObjClass *current = klass; current; current = current->superclass) {
        for (int i = 0; i < current->statics.capacity; i++) {
            TableEntry *entry = &current->statics.entries[i];

            if (entry->key == name) {
                return &entry->value;
            }
        }
    }
    return NULL;
}

bool class_is_subclass(ObjClass *klass, ObjClass *candidate)
{
    for (ObjClass *current = klass; current; current = current->superclass) {
        if (current == candidate) {
            return true;
        }
    }
    return false;
}

bool map_get_index(ObjMap *map, Value key, Value *out, int *out_index)
{
    if (map->capacity == 0 || map->count == 0) {
        return false;
    }
    uint32_t mask = (uint32_t)(map->capacity - 1);
    MapEntry *entries = map->entries;

    if (key.type == VAL_OBJ) {
        void *key_obj = key.as.obj;
        uint32_t hash = key.as.obj->type == OBJ_STRING
                            ? ((ObjString *)key_obj)->hash
                            : (uint32_t)(((uintptr_t)key_obj >> 4) * 2654435761u);
        uint32_t index = hash & mask;

        for (;;) {
            MapEntry *entry = &entries[index];
            if (entry->state == 0) {
                return false;
            }
            if (entry->state == 1 && entry->hash == hash && entry->key.type == VAL_OBJ &&
                entry->key.as.obj == key_obj) {
                *out = entry->value;
                if (out_index) {
                    *out_index = (int)index;
                }
                return true;
            }
            index = (index + 1) & mask;
        }
    }

    uint32_t hash = hash_value(key);
    uint32_t index = hash & mask;

    for (;;) {
        MapEntry *entry = &entries[index];
        if (entry->state == 0) {
            return false;
        }
        if (entry->state == 1 && entry->hash == hash && keys_equal(entry->key, key)) {
            *out = entry->value;
            if (out_index) {
                *out_index = (int)index;
            }
            return true;
        }
        index = (index + 1) & mask;
    }
}

bool map_get(ObjMap *map, Value key, Value *out)
{
    return map_get_index(map, key, out, NULL);
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

    uint32_t mask = (uint32_t)(map->capacity - 1);
    MapEntry *entries = map->entries;
    MapEntry *entry;

    if (key.type == VAL_OBJ) {
        void *key_obj = key.as.obj;
        uint32_t hash = key.as.obj->type == OBJ_STRING
                            ? ((ObjString *)key_obj)->hash
                            : (uint32_t)(((uintptr_t)key_obj >> 4) * 2654435761u);
        if (map->tombstones == 0) {
            uint32_t index = hash & mask;
            for (;;) {
                entry = &entries[index];
                if (entry->state == 0 || (entry->hash == hash && entry->key.type == VAL_OBJ &&
                                          entry->key.as.obj == key_obj)) {
                    break;
                }
                index = (index + 1) & mask;
            }
        } else {
            entry = map_find_entry(entries, map->capacity, key, hash);
        }

        if (entry->state == 1) {
            entry->value = value;
            return false;
        }
        if (entry->state == 2) {
            map->tombstones--;
        }
        entry->key = key;
        entry->value = value;
        entry->hash = hash;
        entry->state = 1;
        map->count++;
        return true;
    }

    uint32_t hash = hash_value(key);
    if (map->tombstones == 0) {
        uint32_t index = hash & mask;
        for (;;) {
            entry = &entries[index];
            if (entry->state == 0 || (entry->hash == hash && keys_equal(entry->key, key))) {
                break;
            }
            index = (index + 1) & mask;
        }
    } else {
        entry = map_find_entry(map->entries, map->capacity, key, hash);
    }

    if (entry->state == 1) {
        entry->value = value;
        return false;
    }
    if (entry->state == 2) {
        map->tombstones--;
    }
    entry->key = key;
    entry->value = value;
    entry->hash = hash;
    entry->state = 1;
    map->count++;
    return true;
}

bool map_delete(ObjMap *map, Value key)
{
    if (map->capacity == 0 || map->count == 0) {
        return false;
    }
    uint32_t hash = hash_value(key);
    MapEntry *entry = map_find_entry(map->entries, map->capacity, key, hash);

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
    function->min_arity = 0;
    function->is_varargs = false;
    function->upvalue_count = 0;
    function->name = NULL;
    function->script_path = NULL;
    function->source = NULL;
    function->debug_locals = NULL;
    function->debug_local_count = 0;
    function->debug_local_capacity = 0;
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
    closure->klass = NULL;
    closure->is_static = false;
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
    upvalue->owner = NULL;
    return upvalue;
}

ObjCoroutine *object_coroutine(void)
{
    ObjCoroutine *coroutine;

    mem_pause_gc();
    coroutine = allocate_obj(sizeof(ObjCoroutine), OBJ_COROUTINE);
    zero_obj_body(coroutine, sizeof(ObjCoroutine));
    coroutine->status = CORO_NEW;
    coroutine->result = NIL_VAL;
    coroutine->wait_value = NIL_VAL;
    coroutine->resume_result = NIL_VAL;
    coroutine->exception_value = NIL_VAL;
    mem_resume_gc();
    return coroutine;
}

ObjChannel *object_channel(int capacity)
{
    ObjChannel *channel;

    mem_pause_gc();
    channel = allocate_obj(sizeof(ObjChannel), OBJ_CHANNEL);
    zero_obj_body(channel, sizeof(ObjChannel));
    channel->capacity = capacity;
    if (capacity > 0) {
        channel->buffer = mem_realloc(NULL, 0, sizeof(Value) * (size_t)capacity);
    }
    mem_resume_gc();
    return channel;
}

ObjFile *object_file(FILE *stream, ObjString *path)
{
    ObjFile *file = allocate_obj(sizeof(ObjFile), OBJ_FILE);

    file->stream = stream;
    file->is_closed = false;
    file->path = path;
    return file;
}

ObjSocket *object_socket(int fd, NetKind kind, int64_t timeout_ms)
{
    ObjSocket *socket;

    /* The socket is not reachable from a root until the caller returns it, so
     * no collection may run while its read buffer is allocated. */
    mem_pause_gc();
    socket = allocate_obj(sizeof(ObjSocket), OBJ_SOCKET);
    socket->fd = fd;
    socket->is_closed = false;
    socket->kind = kind;
    socket->timeout_ms = timeout_ms;
    socket->ssl = NULL;
    socket->ssl_ctx = NULL;
    socket->rbuf_cap = 8192;
    socket->rbuf = mem_realloc(NULL, 0, (size_t)socket->rbuf_cap);
    socket->rbuf_len = 0;
    socket->rbuf_pos = 0;
    mem_resume_gc();
    return socket;
}

ObjServer *object_server(int fd)
{
    ObjServer *server = allocate_obj(sizeof(ObjServer), OBJ_SERVER);

    server->fd = fd;
    server->is_closed = false;
    return server;
}

ObjStringBuilder *object_string_builder(int initial_capacity)
{
    if (initial_capacity < 16) {
        initial_capacity = 16;
    }
    ObjStringBuilder *sb = allocate_obj(sizeof(ObjStringBuilder), OBJ_STRING_BUILDER);
    sb->chars = mem_realloc(NULL, 0, (size_t)initial_capacity);
    if (sb->chars) {
        sb->chars[0] = '\0';
    }
    sb->length = 0;
    sb->capacity = initial_capacity;
    return sb;
}

bool string_builder_append(ObjStringBuilder *sb, const char *chars, int len)
{
    if (!chars || len <= 0) {
        return true;
    }
    if (sb->length + len + 1 > sb->capacity) {
        int old_cap = sb->capacity;
        int new_cap = old_cap < 16 ? 16 : old_cap;
        while (new_cap < sb->length + len + 1) {
            new_cap *= 2;
        }
        char *new_chars = mem_realloc(sb->chars, (size_t)old_cap, (size_t)new_cap);
        if (!new_chars) {
            return false;
        }
        sb->chars = new_chars;
        sb->capacity = new_cap;
    }
    memcpy(sb->chars + sb->length, chars, (size_t)len);
    sb->length += len;
    sb->chars[sb->length] = '\0';
    return true;
}

void string_builder_clear(ObjStringBuilder *sb)
{
    sb->length = 0;
    if (sb->chars && sb->capacity > 0) {
        sb->chars[0] = '\0';
    }
}

ObjString *string_builder_to_string(ObjStringBuilder *sb)
{
    return object_string(sb->chars ? sb->chars : "", sb->length);
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
        mem_realloc(function->debug_locals, sizeof(DebugLocal) * (size_t)function->debug_local_capacity, 0);
        mem_realloc(function, sizeof(ObjFunction), 0);
        break;
    }
    case OBJ_CLOSURE: {
        ObjClosure *closure = (ObjClosure *)obj;
        mem_realloc(closure->upvalues, sizeof(ObjUpvalue *) * (size_t)closure->upvalue_count, 0);
        mem_realloc(closure, sizeof(ObjClosure), 0);
        break;
    }
    case OBJ_CLASS: {
        ObjClass *klass = (ObjClass *)obj;
        table_free(&klass->methods);
        table_free(&klass->statics);
        mem_realloc(klass, sizeof(ObjClass), 0);
        break;
    }
    case OBJ_INSTANCE:
        /* The fields map is a separate GC object and is swept on its own. */
        mem_realloc(obj, sizeof(ObjInstance), 0);
        break;
    case OBJ_NATIVE:
        mem_realloc(obj, sizeof(ObjNative), 0);
        break;
    case OBJ_UPVALUE:
        mem_realloc(obj, sizeof(ObjUpvalue), 0);
        break;
    case OBJ_COROUTINE: {
        ObjCoroutine *coroutine = (ObjCoroutine *)obj;

        if (coroutine->owns_stack) {
            mem_realloc(coroutine->stack,
                        sizeof(Value) * (size_t)coroutine->stack_capacity, 0);
        }
        mem_realloc(coroutine->frames, coroutine->frame_bytes, 0);
        mem_realloc(coroutine->try_handlers, coroutine->try_handler_bytes, 0);
        mem_realloc(coroutine, sizeof(ObjCoroutine), 0);
        break;
    }
    case OBJ_CHANNEL: {
        ObjChannel *channel = (ObjChannel *)obj;

        mem_realloc(channel->buffer, sizeof(Value) * (size_t)channel->capacity, 0);
        mem_realloc(channel, sizeof(ObjChannel), 0);
        break;
    }
    case OBJ_FILE: {
        ObjFile *file = (ObjFile *)obj;
        if (file->stream && !file->is_closed) {
            fclose(file->stream);
            file->stream = NULL;
            file->is_closed = true;
        }
        mem_realloc(file, sizeof(ObjFile), 0);
        break;
    }
    case OBJ_SOCKET: {
        ObjSocket *socket = (ObjSocket *)obj;
        net_socket_release_tls(socket);
        if (!socket->is_closed && socket->fd >= 0) {
            close(socket->fd);
        }
        mem_realloc(socket->rbuf, (size_t)socket->rbuf_cap, 0);
        mem_realloc(socket, sizeof(ObjSocket), 0);
        break;
    }
    case OBJ_SERVER: {
        ObjServer *server = (ObjServer *)obj;
        if (!server->is_closed && server->fd >= 0) {
            close(server->fd);
        }
        mem_realloc(server, sizeof(ObjServer), 0);
        break;
    }
    case OBJ_STRING_BUILDER: {
        ObjStringBuilder *sb = (ObjStringBuilder *)obj;
        mem_realloc(sb->chars, (size_t)sb->capacity, 0);
        mem_realloc(sb, sizeof(ObjStringBuilder), 0);
        break;
    }
    }
}

void intern_remove_unmarked(void)
{
    for (uint32_t i = 0; i < intern_capacity; i++) {
        ObjString **link = &intern_buckets[i];

        while (*link) {
            ObjString *string = *link;

            if (!string->obj.marked) {
                *link = string->intern_next;
                string->intern_next = NULL;
                intern_count--;
            } else {
                link = &string->intern_next;
            }
        }
    }
}

void intern_free_table(void)
{
    free(intern_buckets);
    intern_buckets = NULL;
    intern_capacity = 0;
    intern_count = 0;
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
    case OBJ_FILE: return "file";
    case OBJ_STRING_BUILDER: return "string_builder";
    case OBJ_CLASS: return "class";
    case OBJ_INSTANCE: return "instance";
    case OBJ_COROUTINE: return "coroutine";
    case OBJ_CHANNEL: return "channel";
    case OBJ_SOCKET: return "socket";
    case OBJ_SERVER: return "server";
    }
    return "object";
}

bool values_equal(Value a, Value b)
{
    if (a.type != b.type) {
        if (a.type == VAL_INT && b.type == VAL_NUMBER) {
            return int_equals_number(a.as.integer, b.as.number);
        }
        if (a.type == VAL_NUMBER && b.type == VAL_INT) {
            return int_equals_number(b.as.integer, a.as.number);
        }
        return false;
    }
    switch (a.type) {
    case VAL_NIL:
        return true;
    case VAL_BOOL:
        return a.as.boolean == b.as.boolean;
    case VAL_INT:
        return a.as.integer == b.as.integer;
    case VAL_NUMBER:
        return a.as.number == b.as.number;
    case VAL_OBJ:
        return a.as.obj == b.as.obj;
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
    case VAL_INT: {
        char buffer[32];

        snprintf(buffer, sizeof(buffer), "%lld", (long long)AS_INT(v));
        buf_append_str(b, buffer);
        break;
    }
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
        case OBJ_FILE: {
            ObjFile *file = (ObjFile *)AS_OBJ(v);
            buf_appendf(b, "<file %s%s>", file->path ? file->path->chars : "unknown",
                        file->is_closed ? " (closed)" : "");
            break;
        }
        case OBJ_STRING_BUILDER: {
            ObjStringBuilder *sb = (ObjStringBuilder *)AS_OBJ(v);
            buf_append_str(b, sb->chars ? sb->chars : "");
            break;
        }
        case OBJ_CLASS: {
            ObjClass *klass = (ObjClass *)AS_OBJ(v);
            buf_appendf(b, "<class %s>", klass->name ? klass->name->chars : "?");
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance *instance = (ObjInstance *)AS_OBJ(v);
            buf_appendf(b, "<%s instance>",
                        instance->klass && instance->klass->name
                            ? instance->klass->name->chars
                            : "?");
            break;
        }
        case OBJ_COROUTINE: {
            ObjCoroutine *coroutine = (ObjCoroutine *)AS_OBJ(v);
            const char *status = coroutine->status == CORO_DONE       ? "done"
                                 : coroutine->status == CORO_BLOCKED  ? "blocked"
                                 : coroutine->status == CORO_RUNNING ? "running"
                                                                     : "suspended";
            buf_appendf(b, "<coroutine %s>", status);
            break;
        }
        case OBJ_CHANNEL: {
            ObjChannel *channel = (ObjChannel *)AS_OBJ(v);
            if (channel->closed) {
                buf_append_str(b, "<channel closed>");
            } else if (channel->capacity == 0) {
                buf_append_str(b, "<channel unbuffered>");
            } else {
                buf_appendf(b, "<channel capacity=%d>", channel->capacity);
            }
            break;
        }
        case OBJ_SOCKET: {
            ObjSocket *socket = (ObjSocket *)AS_OBJ(v);
            if (socket->is_closed) {
                buf_append_str(b, "<socket closed>");
            } else {
                const char *kind = socket->kind == NET_UDP ? "udp"
                                   : socket->kind == NET_TLS ? "tls"
                                                             : "tcp";

                buf_appendf(b, "<socket %s fd=%d>", kind, socket->fd);
            }
            break;
        }
        case OBJ_SERVER: {
            ObjServer *server = (ObjServer *)AS_OBJ(v);
            if (server->is_closed) {
                buf_append_str(b, "<server closed>");
            } else {
                buf_appendf(b, "<server fd=%d>", server->fd);
            }
            break;
        }
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

static ObjString *int_to_string(int64_t val)
{
    char temp[24];
    int tlen = 0;
    bool neg = false;
    uint64_t u;

    if (val < 0) {
        neg = true;
        u = (uint64_t)(-(val + 1)) + 1;
    } else {
        u = (uint64_t)val;
    }

    do {
        temp[tlen++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u > 0);

    char buf[32];
    int pos = 0;
    uint32_t hash = 2166136261u;

    if (neg) {
        buf[pos++] = '-';
        hash ^= (uint8_t)'-';
        hash *= 16777619u;
    }
    while (tlen > 0) {
        char c = temp[--tlen];
        buf[pos++] = c;
        hash ^= (uint8_t)c;
        hash *= 16777619u;
    }
    buf[pos] = '\0';
    return object_string_with_hash(buf, pos, hash);
}

ObjString *value_to_string(Value v)
{
    if (is_obj_type(v, OBJ_STRING)) {
        return AS_STRING(v);
    }
    if (is_obj_type(v, OBJ_STRING_BUILDER)) {
        return string_builder_to_string(AS_STRING_BUILDER(v));
    }
    if (v.type == VAL_INT) {
        return int_to_string(v.as.integer);
    }
    if (v.type == VAL_BOOL) {
        return AS_BOOL(v) ? object_string("true", 4) : object_string("false", 5);
    }
    if (v.type == VAL_NIL) {
        return object_string("nil", 3);
    }
    if (v.type == VAL_NUMBER) {
        char buffer[64];
        double number = AS_NUMBER(v);
        if (isnan(number)) {
            return object_string("nan", 3);
        }
        if (isinf(number)) {
            return object_string(number > 0 ? "inf" : "-inf", number > 0 ? 3 : 4);
        }
        int len = snprintf(buffer, sizeof(buffer), "%.15g", number);
        return object_string(buffer, len);
    }

    buf_t buffer;
    ObjString *result;

    buf_init(&buffer);
    value_format(&buffer, v, false);
    result = object_string(buffer.data ? buffer.data : "", (int)buffer.len);
    buf_free(&buffer);
    return result;
}
