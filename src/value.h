/* value.h - value representation and object header. */
#ifndef SPARK_VALUE_H
#define SPARK_VALUE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Obj Obj;
typedef struct ObjString ObjString;

typedef enum {
    VAL_NIL,
    VAL_BOOL,
    VAL_NUMBER,
    VAL_INT,
    VAL_OBJ
} ValueType;

typedef struct {
    ValueType type;
    union {
        bool boolean;
        double number;
        int64_t integer;
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
    OBJ_UPVALUE,
    OBJ_FILE,
    OBJ_STRING_BUILDER,
    OBJ_CLASS,
    OBJ_INSTANCE
} ObjType;

struct Obj {
    ObjType type;
    bool marked;
    Obj *next;
};

#define NIL_VAL ((Value){.type = VAL_NIL})
#define BOOL_VAL(b) ((Value){.type = VAL_BOOL, .as.boolean = (b)})
#define NUMBER_VAL(n) ((Value){.type = VAL_NUMBER, .as.number = (n)})
#define INT_VAL(i) ((Value){.type = VAL_INT, .as.integer = (i)})
#define OBJ_VAL(o) ((Value){.type = VAL_OBJ, .as.obj = (Obj *)(o)})

#define AS_BOOL(v) ((v).as.boolean)
#define AS_NUMBER(v) ((v).as.number)
#define AS_INT(v) ((v).as.integer)
#define AS_OBJ(v) ((v).as.obj)
#define AS_STRING(v) ((ObjString *)(v).as.obj)
#define AS_STRING_BUILDER(v) ((ObjStringBuilder *)(v).as.obj)

static inline bool is_nil(Value v) { return v.type == VAL_NIL; }
static inline bool is_bool(Value v) { return v.type == VAL_BOOL; }
static inline bool is_number(Value v) { return v.type == VAL_NUMBER; }
static inline bool is_int(Value v) { return v.type == VAL_INT; }
static inline bool is_obj(Value v) { return v.type == VAL_OBJ; }
static inline bool is_obj_type(Value v, ObjType type)
{
    return v.type == VAL_OBJ && v.as.obj->type == type;
}

/* Numbers and integers are both "numeric": they mix in arithmetic and
 * comparisons by promoting the integer to a double. */
static inline bool value_is_numeric(Value v)
{
    return v.type == VAL_NUMBER || v.type == VAL_INT;
}

static inline double value_as_double(Value v)
{
    return v.type == VAL_INT ? (double)v.as.integer : v.as.number;
}

/* 2^63: the exact (double) exclusive upper bound of int64, and the negation
 * of its lower bound. Used when a value must be checked against the int64
 * range before converting. */
#define TWO_POW_63 9223372036854775808.0

/* Exact equality between an integer and a floating-point value: the double
 * must be a whole number in the int64 range and represent the same integer. */
static inline bool int_equals_number(int64_t integer, double number)
{
    if (!(number >= -TWO_POW_63 && number < TWO_POW_63)) {
        return false;
    }
    int64_t truncated = (int64_t)number;

    return (double)truncated == number && integer == truncated;
}

/* Overflow-checked int64 arithmetic. Each returns true when the operation
 * does not fit and the caller must promote the result to a double. */
#if defined(__GNUC__) && (__GNUC__ >= 5 || defined(__clang__))
#define SPARK_OVERFLOW_BUILTINS 1
#endif

#ifdef SPARK_OVERFLOW_BUILTINS
static inline bool int_add_overflow(int64_t a, int64_t b, int64_t *out) { return __builtin_add_overflow(a, b, out); }
static inline bool int_sub_overflow(int64_t a, int64_t b, int64_t *out) { return __builtin_sub_overflow(a, b, out); }
static inline bool int_mul_overflow(int64_t a, int64_t b, int64_t *out) { return __builtin_mul_overflow(a, b, out); }
#else
static inline bool int_add_overflow(int64_t a, int64_t b, int64_t *out)
{
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return true;
    }
    *out = a + b;
    return false;
}

static inline bool int_sub_overflow(int64_t a, int64_t b, int64_t *out)
{
    if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b)) {
        return true;
    }
    *out = a - b;
    return false;
}

static inline bool int_mul_overflow(int64_t a, int64_t b, int64_t *out)
{
    if (a == 0 || b == 0) {
        *out = 0;
        return false;
    }
    /* Work with magnitudes so the product itself cannot overflow a uint64. */
    uint64_t magnitude_a = a < 0 ? (uint64_t)(-(a + 1)) + 1u : (uint64_t)a;
    uint64_t magnitude_b = b < 0 ? (uint64_t)(-(b + 1)) + 1u : (uint64_t)b;

    if (magnitude_b > UINT64_MAX / magnitude_a) {
        return true;
    }
    uint64_t product = magnitude_a * magnitude_b;
    bool negative = (a < 0) != (b < 0);

    if (negative) {
        if (product > (uint64_t)INT64_MAX + 1u) {
            return true;
        }
        *out = product == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)product;
    } else {
        if (product > (uint64_t)INT64_MAX) {
            return true;
        }
        *out = (int64_t)product;
    }
    return false;
}
#endif /* SPARK_OVERFLOW_BUILTINS */

bool values_equal(Value a, Value b);

static inline bool value_is_falsey(Value v)
{
    return v.type == VAL_NIL || (v.type == VAL_BOOL && !v.as.boolean);
}

const char *obj_type_name(ObjType type);

#endif
