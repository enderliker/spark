/* codec.c - base64 and hex codecs over `bytes`.
 *
 * Every native accepts a `bytes` value or a `string` (whose UTF-8 bytes are
 * used as they are, which is the only place a text/binary conversion happens
 * without naming `bytes_from`: it is a convenience for literals) and returns
 * `bytes`. `base64_encode` is exported for the JSON encoder, which emits a
 * base64 string for a `bytes` value. */
#include "codec.h"

#include <limits.h>
#include <string.h>

#include "object.h"
#include "vm.h"

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char hex_alphabet[] = "0123456789abcdef";

static const char *codec_value_type(Value value)
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

void codec_base64_encode(buf_t *out, const unsigned char *data, int length)
{
    int i = 0;

    for (; i + 2 < length; i += 3) {
        uint32_t group = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        char encoded[4];

        encoded[0] = base64_alphabet[(group >> 18) & 0x3f];
        encoded[1] = base64_alphabet[(group >> 12) & 0x3f];
        encoded[2] = base64_alphabet[(group >> 6) & 0x3f];
        encoded[3] = base64_alphabet[group & 0x3f];
        buf_append(out, encoded, 4);
    }
    if (i < length) {
        uint32_t group = (uint32_t)data[i] << 16;
        char encoded[4];

        if (i + 1 < length) {
            group |= (uint32_t)data[i + 1] << 8;
        }
        encoded[0] = base64_alphabet[(group >> 18) & 0x3f];
        encoded[1] = base64_alphabet[(group >> 12) & 0x3f];
        encoded[2] = i + 1 < length ? base64_alphabet[(group >> 6) & 0x3f] : '=';
        encoded[3] = '=';
        buf_append(out, encoded, 4);
    }
}

static void hex_encode_into(buf_t *out, const unsigned char *data, int length)
{
    for (int i = 0; i < length; i++) {
        char encoded[2];

        encoded[0] = hex_alphabet[data[i] >> 4];
        encoded[1] = hex_alphabet[data[i] & 0x0f];
        buf_append(out, encoded, 2);
    }
}

static int base64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

/* Standard alphabet, padding optional: a final group of two or three characters
 * decodes as if it were padded. Anything else is a ValueError. */
static bool base64_decode_into(const unsigned char *data, int length, buf_t *out)
{
    int pad = 0;
    int data_length;
    uint32_t accumulator = 0;
    int bits = 0;

    if (length > 0 && data[length - 1] == '=') {
        pad++;
    }
    if (length > 1 && data[length - 2] == '=') {
        pad++;
    }
    data_length = length - pad;
    /* The characters are validated before the group length, so a bad character
     * at the end is not misreported as a truncated group. */
    for (int i = 0; i < data_length; i++) {
        if (base64_value(data[i]) < 0) {
            if (data[i] == '=') {
                vm_runtime_error(ERROR_VALUE,
                                 "'base64_decode' accepts padding only at the end (index %d)", i);
            } else {
                vm_runtime_error(ERROR_VALUE,
                                 "'base64_decode' received an invalid base64 character at index %d",
                                 i);
            }
            return false;
        }
    }
    if (pad > 0 && length % 4 != 0) {
        vm_runtime_error(ERROR_VALUE, "'base64_decode' received incomplete padding");
        return false;
    }
    if (data_length % 4 == 1) {
        vm_runtime_error(ERROR_VALUE, "'base64_decode' received a truncated base64 group");
        return false;
    }
    for (int i = 0; i < data_length; i++) {
        int value = base64_value(data[i]);

        accumulator = (accumulator << 6) | (uint32_t)value;
        bits += 6;
        if (bits >= 8) {
            unsigned char byte;

            bits -= 8;
            byte = (unsigned char)((accumulator >> bits) & 0xff);
            buf_append(out, &byte, 1);
            accumulator &= bits > 0 ? (1u << bits) - 1 : 0;
        }
    }
    return true;
}

static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool hex_decode_into(const unsigned char *data, int length, buf_t *out)
{
    if (length % 2 != 0) {
        vm_runtime_error(ERROR_VALUE,
                         "'hex_decode' expects an even number of hexadecimal digits, received %d",
                         length);
        return false;
    }
    for (int i = 0; i < length; i += 2) {
        int high = hex_value(data[i]);
        int low = hex_value(data[i + 1]);
        unsigned char byte;

        if (high < 0) {
            vm_runtime_error(ERROR_VALUE,
                             "'hex_decode' received an invalid hexadecimal digit at index %d", i);
            return false;
        }
        if (low < 0) {
            vm_runtime_error(ERROR_VALUE,
                             "'hex_decode' received an invalid hexadecimal digit at index %d",
                             i + 1);
            return false;
        }
        byte = (unsigned char)((high << 4) | low);
        buf_append(out, &byte, 1);
    }
    return true;
}

/* The first argument of every codec native. */
static bool codec_input(Value value, const char *function, const unsigned char **data, int *length)
{
    if (is_obj_type(value, OBJ_BYTES)) {
        *data = AS_BYTES(value)->data;
        *length = AS_BYTES(value)->length;
        return true;
    }
    if (is_obj_type(value, OBJ_STRING)) {
        *data = (const unsigned char *)AS_STRING(value)->chars;
        *length = AS_STRING(value)->length;
        return true;
    }
    vm_runtime_error(ERROR_TYPE, "'%s' expects bytes or a string, received %s", function,
                     codec_value_type(value));
    return false;
}

/* Copies the buffer into an immutable `bytes` and releases it. */
static Value codec_result(buf_t *out)
{
    ObjBytes *bytes;

    if (out->len > (size_t)INT_MAX) {
        buf_free(out);
        vm_runtime_error(ERROR_MEMORY, "the encoded result is too large");
        return NIL_VAL;
    }
    bytes = object_bytes((int)out->len);
    if (out->len > 0) {
        memcpy(bytes->data, out->data, out->len);
    }
    buf_free(out);
    return OBJ_VAL(bytes);
}

static Value native_base64_encode(int arg_count, Value *args)
{
    const unsigned char *data;
    int length;
    buf_t out;

    (void)arg_count;
    if (!codec_input(args[0], "base64_encode", &data, &length)) {
        return NIL_VAL;
    }
    buf_init(&out);
    codec_base64_encode(&out, data, length);
    return codec_result(&out);
}

static Value native_base64_decode(int arg_count, Value *args)
{
    const unsigned char *data;
    int length;
    buf_t out;

    (void)arg_count;
    if (!codec_input(args[0], "base64_decode", &data, &length)) {
        return NIL_VAL;
    }
    buf_init(&out);
    if (!base64_decode_into(data, length, &out)) {
        buf_free(&out);
        return NIL_VAL;
    }
    return codec_result(&out);
}

static Value native_hex_encode(int arg_count, Value *args)
{
    const unsigned char *data;
    int length;
    buf_t out;

    (void)arg_count;
    if (!codec_input(args[0], "hex_encode", &data, &length)) {
        return NIL_VAL;
    }
    buf_init(&out);
    hex_encode_into(&out, data, length);
    return codec_result(&out);
}

static Value native_hex_decode(int arg_count, Value *args)
{
    const unsigned char *data;
    int length;
    buf_t out;

    (void)arg_count;
    if (!codec_input(args[0], "hex_decode", &data, &length)) {
        return NIL_VAL;
    }
    buf_init(&out);
    if (!hex_decode_into(data, length, &out)) {
        buf_free(&out);
        return NIL_VAL;
    }
    return codec_result(&out);
}

void codec_init(void)
{
    vm_define_native("base64_encode", native_base64_encode, 1);
    vm_define_native("base64_decode", native_base64_decode, 1);
    vm_define_native("hex_encode", native_hex_encode, 1);
    vm_define_native("hex_decode", native_hex_decode, 1);
}