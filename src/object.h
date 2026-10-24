/* object.h - language objects: strings, arrays, functions, closures and natives. */
#ifndef SPARK_OBJECT_H
#define SPARK_OBJECT_H

#include <stdio.h>

#include "chunk.h"
#include "value.h"

struct ObjString {
    Obj obj;
    uint32_t hash;
    int length;
    struct ObjString *intern_next;
    char chars[]; /* NUL-terminated string */
};

typedef struct {
    Obj obj;
    Value *items;
    int count;
    int capacity;
} ObjArray;

typedef struct {
    Value key;
    Value value;
    uint32_t hash;
    uint8_t state; /* 0 = free, 1 = occupied, 2 = tombstone */
} MapEntry;

typedef struct {
    Obj obj;
    MapEntry *entries;
    int count;
    int tombstones;
    int capacity;
} ObjMap;

typedef struct ObjFunction {
    Obj obj;
    int arity;
    int upvalue_count;
    Chunk chunk;
    ObjString *name;
    const char *source; /* borrowed from the VM; used to show error snippets */
} ObjFunction;

typedef struct ObjUpvalue {
    Obj obj;
    Value *location;
    Value closed;
    struct ObjUpvalue *next;
} ObjUpvalue;

typedef struct {
    Obj obj;
    ObjFunction *function;
    ObjUpvalue **upvalues;
    int upvalue_count;
} ObjClosure;

#define AS_FUNCTION(v) ((ObjFunction *)(v).as.obj)
#define AS_CLOSURE(v) ((ObjClosure *)(v).as.obj)
#define AS_ARRAY(v) ((ObjArray *)(v).as.obj)
#define AS_MAP(v) ((ObjMap *)(v).as.obj)

typedef Value (*NativeFn)(int arg_count, Value *args);

typedef struct {
    Obj obj;
    NativeFn function;
    int arity; /* -1 = variadic */
    ObjString *name;
} ObjNative;

/* Linked list of all live objects (for the GC). */
Obj *objects_head(void);
void objects_set_head(Obj *head);

ObjString *object_string(const char *chars, int length);
ObjString *object_string_concat(ObjString *a, ObjString *b);
ObjString *value_to_string(Value v);

ObjArray *object_array(void);
bool array_push(ObjArray *array, Value value);
bool array_pop(ObjArray *array, Value *out);

ObjMap *object_map(void);
bool map_get(ObjMap *map, Value key, Value *out);
bool map_set(ObjMap *map, Value key, Value value);
bool map_delete(ObjMap *map, Value key);

ObjFunction *object_function(void);
ObjClosure *object_closure(ObjFunction *function);
ObjNative *object_native(NativeFn fn, int arity, const char *name);
ObjUpvalue *object_upvalue(Value *slot);

void object_free(Obj *obj);
void intern_remove_unmarked(void);
void intern_free_table(void);

/* Value printing: `print` uses the readable form; `repr` quotes strings. */
void value_print(FILE *out, Value v);
void value_print_repr(FILE *out, Value v);

#endif
