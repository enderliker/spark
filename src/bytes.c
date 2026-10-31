/* bytes.c - the `bytes` constructors and builder exposed to the language.
 *
 * The objects themselves (allocation, content equality, key hashing and
 * printing) live in object.c; this module registers the natives with the VM the
 * way net.c registers the network ones, and dispatches the `bytes_builder`
 * methods back to them. */
#include "bytes.h"

#include <limits.h>
#include <string.h>

#include "vm.h"

static const char *bytes_value_type(Value value)
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

/* Required integer argument. As in net.c, only the VAL_INT tag counts: a whole
 * float is not an integer, matching `is_int`. */
static bool bytes_int_argument(Value value, const char *function, const char *name, int64_t *out)
{
    if (!is_int(value)) {
        vm_runtime_error(ERROR_TYPE, "'%s' expects an integer %s, received %s", function, name,
                         bytes_value_type(value));
        return false;
    }
    *out = AS_INT(value);
    return true;
}

/* bytes(n): n zero bytes. */
static Value native_bytes(int arg_count, Value *args)
{
    int64_t size;

    (void)arg_count;
    /* A string is the mistake this native invites, so answer it with the
     * bridge that does what the caller meant. There is no implicit conversion
     * between text and raw bytes (decision A16). */
    if (is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'bytes' expects an integer size, received a string; "
                         "use 'bytes_from(text)' for the UTF-8 bytes of a string");
        return NIL_VAL;
    }
    if (!bytes_int_argument(args[0], "bytes", "size", &size)) {
        return NIL_VAL;
    }
    if (size < 0) {
        vm_runtime_error(ERROR_VALUE, "'bytes' does not accept a negative size (%lld)",
                         (long long)size);
        return NIL_VAL;
    }
    /* The length is stored in an int, so anything above it is refused before the
     * cast rather than truncated into a short allocation. */
    if (size > (int64_t)INT_MAX - (int64_t)sizeof(ObjBytes)) {
        vm_runtime_error(ERROR_MEMORY, "'bytes' cannot allocate %lld bytes", (long long)size);
        return NIL_VAL;
    }
    return OBJ_VAL(object_bytes((int)size));
}

/* bytes_from(array_of_ints) and bytes_from(string). The string form copies the
 * UTF-8 bytes as they are: no decoding, no encoding, embedded NULs preserved. */
static Value native_bytes_from(int arg_count, Value *args)
{
    Value source = args[0];

    (void)arg_count;
    if (is_obj_type(source, OBJ_STRING)) {
        ObjString *text = AS_STRING(source);
        ObjBytes *bytes = object_bytes(text->length);

        if (text->length > 0) {
            memcpy(bytes->data, text->chars, (size_t)text->length);
        }
        return OBJ_VAL(bytes);
    }
    if (is_obj_type(source, OBJ_ARRAY)) {
        ObjArray *array = AS_ARRAY(source);
        int count = array->count;

        /* Validate every element before allocating: a failure allocates
         * nothing, and the message can name the offending index. The array is
         * reachable from the value stack (it is a native argument), so it
         * survives the collection the allocation may trigger. */
        for (int i = 0; i < count; i++) {
            int64_t value;

            if (!is_int(array->items[i])) {
                vm_runtime_error(ERROR_TYPE, "'bytes_from' expects an array of integers, "
                                 "received %s at index %d", bytes_value_type(array->items[i]), i);
                return NIL_VAL;
            }
            value = AS_INT(array->items[i]);
            if (value < 0 || value > 255) {
                vm_runtime_error(ERROR_VALUE, "'bytes_from' expects byte values between 0 and 255, "
                                 "received %lld at index %d", (long long)value, i);
                return NIL_VAL;
            }
        }

        ObjBytes *bytes = object_bytes(count);

        for (int i = 0; i < count; i++) {
            bytes->data[i] = (unsigned char)AS_INT(array->items[i]);
        }
        return OBJ_VAL(bytes);
    }
    vm_runtime_error(ERROR_TYPE, "'bytes_from' expects an array of integers or a string, "
                     "received %s", bytes_value_type(source));
    return NIL_VAL;
}

/* bytes_builder(): the mutable companion of `bytes` (Q18), mirroring
 * string_builder(). It takes no argument, an initial capacity (an integer) or
 * initial contents (a bytes value). */
static Value native_bytes_builder(int arg_count, Value *args)
{
    int64_t capacity = 16;
    Value initial = NIL_VAL;

    if (arg_count >= 1) {
        if (is_obj_type(args[0], OBJ_BYTES)) {
            initial = args[0];
            capacity = (int64_t)AS_BYTES(initial)->length + 16;
        } else if (is_int(args[0])) {
            capacity = AS_INT(args[0]);
            if (capacity < 0) {
                vm_runtime_error(ERROR_VALUE,
                                 "'bytes_builder' does not accept a negative capacity (%lld)",
                                 (long long)capacity);
                return NIL_VAL;
            }
        } else {
            /* A string is the other mistake this native invites (A16). */
            vm_runtime_error(ERROR_TYPE,
                             "'bytes_builder' expects a capacity or initial bytes, received %s; "
                             "use bytes_from(x) to convert",
                             bytes_value_type(args[0]));
            return NIL_VAL;
        }
    }
    if (capacity > (int64_t)INT_MAX) {
        capacity = INT_MAX;
    }
    ObjBytesBuilder *builder = object_bytes_builder((int)capacity);

    if (!is_nil(initial)) {
        /* Copied straight into the buffer: the builder has no root of its own
         * yet, so this must not allocate, and the capacity already fits. */
        memcpy(builder->data, AS_BYTES(initial)->data, (size_t)AS_BYTES(initial)->length);
        builder->length = AS_BYTES(initial)->length;
    }
    return OBJ_VAL(builder);
}

static Value native_bb_append(int arg_count, Value *args)
{
    ObjBytesBuilder *builder;
    Value value;

    if (arg_count < 2) {
        vm_runtime_error(ERROR_ARITY, "'bb_append' expects 2 arguments (builder, value)");
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_BYTES_BUILDER)) {
        vm_runtime_error(ERROR_TYPE, "'bb_append' first argument must be a bytes_builder, "
                         "received %s", bytes_value_type(args[0]));
        return NIL_VAL;
    }
    builder = AS_BYTES_BUILDER(args[0]);
    value = args[1];
    if (is_int(value)) {
        int64_t byte = AS_INT(value);

        if (byte < 0 || byte > 255) {
            vm_runtime_error(ERROR_VALUE,
                             "'bb_append' expects a byte value between 0 and 255, received %lld",
                             (long long)byte);
            return NIL_VAL;
        }
        unsigned char single = (unsigned char)byte;

        if (!bytes_builder_append(builder, &single, 1)) {
            vm_runtime_error(ERROR_MEMORY, "could not grow the bytes_builder");
            return NIL_VAL;
        }
    } else if (is_obj_type(value, OBJ_BYTES)) {
        ObjBytes *bytes = AS_BYTES(value);

        if (!bytes_builder_append(builder, bytes->data, bytes->length)) {
            vm_runtime_error(ERROR_MEMORY, "could not grow the bytes_builder");
            return NIL_VAL;
        }
    } else {
        vm_runtime_error(ERROR_TYPE,
                         "'bb_append' expects an integer byte value or bytes, received %s; "
                         "use bytes_from(x) to convert",
                         bytes_value_type(value));
        return NIL_VAL;
    }
    return args[0];
}

static Value native_bb_to_bytes(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_BYTES_BUILDER)) {
        vm_runtime_error(ERROR_TYPE, "'bb_to_bytes' expects a bytes_builder, received %s",
                         bytes_value_type(args[0]));
        return NIL_VAL;
    }
    return OBJ_VAL(bytes_builder_to_bytes(AS_BYTES_BUILDER(args[0])));
}

static Value native_bb_len(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_BYTES_BUILDER)) {
        vm_runtime_error(ERROR_TYPE, "'bb_len' expects a bytes_builder, received %s",
                         bytes_value_type(args[0]));
        return NIL_VAL;
    }
    /* Counted in bytes, exactly like the immutable value it produces. */
    return INT_VAL(AS_BYTES_BUILDER(args[0])->length);
}

static Value native_bb_clear(int arg_count, Value *args)
{
    (void)arg_count;
    if (!is_obj_type(args[0], OBJ_BYTES_BUILDER)) {
        vm_runtime_error(ERROR_TYPE, "'bb_clear' expects a bytes_builder, received %s",
                         bytes_value_type(args[0]));
        return NIL_VAL;
    }
    bytes_builder_clear(AS_BYTES_BUILDER(args[0]));
    return args[0];
}

bool bytes_builder_dispatch(Value receiver, ObjString *method, int arg_count, Value *args,
                            Value *out)
{
    const char *name = method->chars;

    if (!is_obj_type(receiver, OBJ_BYTES_BUILDER)) {
        return false;
    }
    if (strcmp(name, "append") == 0 && arg_count == 1) {
        Value a[2] = { receiver, args[0] };

        *out = native_bb_append(2, a);
        return true;
    }
    if (strcmp(name, "to_bytes") == 0 && arg_count == 0) {
        Value a[1] = { receiver };

        *out = native_bb_to_bytes(1, a);
        return true;
    }
    if (strcmp(name, "len") == 0 && arg_count == 0) {
        *out = INT_VAL(AS_BYTES_BUILDER(receiver)->length);
        return true;
    }
    if (strcmp(name, "clear") == 0 && arg_count == 0) {
        bytes_builder_clear(AS_BYTES_BUILDER(receiver));
        *out = receiver;
        return true;
    }
    return false;
}

void bytes_init(void)
{
    vm_define_native("bytes", native_bytes, 1);
    vm_define_native("bytes_from", native_bytes_from, 1);
    vm_define_native("bytes_builder", native_bytes_builder, -1);
    vm_define_native("bb_append", native_bb_append, 2);
    vm_define_native("bb_to_bytes", native_bb_to_bytes, 1);
    vm_define_native("bb_len", native_bb_len, 1);
    vm_define_native("bb_clear", native_bb_clear, 1);
}
