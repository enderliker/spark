/* bytes.c - the `bytes` constructors exposed to the language.
 *
 * The object itself (allocation, content equality, key hashing and printing)
 * lives in object.c; this module registers the two construction natives with
 * the VM the way net.c registers the network ones. */
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

void bytes_init(void)
{
    vm_define_native("bytes", native_bytes, 1);
    vm_define_native("bytes_from", native_bytes_from, 1);
}
