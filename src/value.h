/* value.h - value representation and object header. */
#ifndef CHISPA_VALUE_H
#define CHISPA_VALUE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Obj Obj;
typedef struct ObjString ObjString;

typedef enum {
    VAL_NIL,
    VAL_BOOL,
    VAL_NUMBER,
    VAL_OBJ
} ValueType;

typedef struct {
    ValueType type;
    union {
        bool boolean;
        double number;
        Obj *obj;
    } as;
} Value;

typedef enum {
    OBJ_STRING,
    OBJ_ARRAY,
    OBJ_MAP,
    OBJ_FUNCTION,
    OBJ_CLOSURE,
    OBJ_NATIVE,
    OBJ_UPVALUE
} ObjType;

struct Obj {
    ObjType type;
    bool marked;
    Obj *next;
};

#define NIL_VAL ((Value){.type = VAL_NIL})
#define BOOL_VAL(b) ((Value){.type = VAL_BOOL, .as.boolean = (b)})
#define NUMBER_VAL(n) ((Value){.type = VAL_NUMBER, .as.number = (n)})
#define OBJ_VAL(o) ((Value){.type = VAL_OBJ, .as.obj = (Obj *)(o)})

#define AS_BOOL(v) ((v).as.boolean)
#define AS_NUMBER(v) ((v).as.number)
#define AS_OBJ(v) ((v).as.obj)
#define AS_STRING(v) ((ObjString *)(v).as.obj)

static inline bool is_nil(Value v) { return v.type == VAL_NIL; }
static inline bool is_bool(Value v) { return v.type == VAL_BOOL; }
static inline bool is_number(Value v) { return v.type == VAL_NUMBER; }
static inline bool is_obj(Value v) { return v.type == VAL_OBJ; }
static inline bool is_obj_type(Value v, ObjType type)
{
    return v.type == VAL_OBJ && v.as.obj->type == type;
}

bool values_equal(Value a, Value b);

static inline bool value_is_falsey(Value v)
{
    return v.type == VAL_NIL || (v.type == VAL_BOOL && !v.as.boolean);
}

const char *obj_type_name(ObjType type);

#endif
