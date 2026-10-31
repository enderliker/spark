/* crypto.c - SHA-256, HMAC-SHA256 and a CSPRNG over `bytes` (T118).
 *
 * The primitives come from OpenSSL, which the interpreter already links for
 * TLS; reimplementing a hash or a random generator would risk a subtle and
 * security-relevant bug. Inputs are a `bytes` value or a `string` (its UTF-8
 * bytes), the same rule the codecs follow. Error messages name the received
 * type but never echo the key or the data, so a secret cannot leak through an
 * exception. `rand()` stays the language's non-cryptographic PRNG; only
 * `rand_bytes` is backed by the CSPRNG.
 *
 * The largest value `rand_bytes` produces is capped at 16 MiB: it is a
 * convenience bound that keeps a typo from asking the process for gigabytes,
 * and the cap is documented with the other I/O limits. */
#include "crypto.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "object.h"
#include "vm.h"

#define CRYPTO_RAND_MAX (16 * 1024 * 1024)
#define CRYPTO_DIGEST_LENGTH ((int)SHA256_DIGEST_LENGTH)
#define CRYPTO_HEX_LENGTH ((int)(SHA256_DIGEST_LENGTH * 2))

static const char *crypto_type_name(Value value)
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

/* A digest input: bytes or a string, used as they are. */
static bool crypto_input(Value value, const char *function, const unsigned char **data,
                         size_t *length)
{
    if (is_obj_type(value, OBJ_BYTES)) {
        *data = AS_BYTES(value)->data;
        *length = (size_t)AS_BYTES(value)->length;
        return true;
    }
    if (is_obj_type(value, OBJ_STRING)) {
        *data = (const unsigned char *)AS_STRING(value)->chars;
        *length = (size_t)AS_STRING(value)->length;
        return true;
    }
    vm_runtime_error(ERROR_TYPE, "'%s' expects bytes or a string, received %s", function,
                     crypto_type_name(value));
    return false;
}

/* The size argument of rand_bytes, with the same acceptance rule as the other
 * integer arguments of the standard library: an int, or an integral number. */
static bool crypto_size_argument(Value value, int *out)
{
    if (is_int(value)) {
        int64_t integer = AS_INT(value);

        if (integer < INT_MIN || integer > INT_MAX) {
            vm_runtime_error(ERROR_VALUE, "'rand_bytes' expects an integer as size, received %lld",
                             (long long)integer);
            return false;
        }
        *out = (int)integer;
        return true;
    }
    if (is_number(value)) {
        double raw = AS_NUMBER(value);

        if (raw != floor(raw) || raw < (double)INT_MIN || raw > (double)INT_MAX) {
            vm_runtime_error(ERROR_VALUE, "'rand_bytes' expects an integer as size, received %g",
                             raw);
            return false;
        }
        *out = (int)raw;
        return true;
    }
    vm_runtime_error(ERROR_VALUE, "'rand_bytes' expects an integer as size, received %s",
                     crypto_type_name(value));
    return false;
}

static Value crypto_digest_result(const unsigned char *digest)
{
    ObjBytes *bytes = object_bytes(CRYPTO_DIGEST_LENGTH);

    memcpy(bytes->data, digest, CRYPTO_DIGEST_LENGTH);
    return OBJ_VAL(bytes);
}

static Value native_sha256(int arg_count, Value *args)
{
    const unsigned char *data;
    size_t length;
    unsigned char digest[SHA256_DIGEST_LENGTH];

    (void)arg_count;
    if (!crypto_input(args[0], "sha256", &data, &length)) {
        return NIL_VAL;
    }
    SHA256(data, length, digest);
    return crypto_digest_result(digest);
}

static Value native_sha256_hex(int arg_count, Value *args)
{
    static const char digits[] = "0123456789abcdef";
    const unsigned char *data;
    size_t length;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    char hex[CRYPTO_HEX_LENGTH];

    (void)arg_count;
    if (!crypto_input(args[0], "sha256_hex", &data, &length)) {
        return NIL_VAL;
    }
    SHA256(data, length, digest);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    return OBJ_VAL(object_string(hex, CRYPTO_HEX_LENGTH));
}

static Value native_hmac_sha256(int arg_count, Value *args)
{
    const unsigned char *key;
    const unsigned char *data;
    size_t key_length;
    size_t data_length;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;

    (void)arg_count;
    if (!crypto_input(args[0], "hmac_sha256", &key, &key_length)) {
        return NIL_VAL;
    }
    if (!crypto_input(args[1], "hmac_sha256", &data, &data_length)) {
        return NIL_VAL;
    }
    /* Key length is an int in the OpenSSL API; a key already in memory cannot
     * sensibly exceed that, and the digest input is size_t. */
    if (key_length > (size_t)INT_MAX || data_length > (size_t)INT_MAX) {
        vm_runtime_error(ERROR_MEMORY, "'hmac_sha256' input is too large");
        return NIL_VAL;
    }
    if (HMAC(EVP_sha256(), key, (int)key_length, data, data_length, digest,
             &digest_length) == NULL ||
        digest_length != SHA256_DIGEST_LENGTH) {
        vm_runtime_error(ERROR_RUNTIME, "'hmac_sha256' failed in the crypto library");
        return NIL_VAL;
    }
    return crypto_digest_result(digest);
}

static Value native_rand_bytes(int arg_count, Value *args)
{
    ObjBytes *bytes;
    int length;

    (void)arg_count;
    if (!crypto_size_argument(args[0], &length)) {
        return NIL_VAL;
    }
    if (length < 0 || length > CRYPTO_RAND_MAX) {
        vm_runtime_error(ERROR_VALUE, "'rand_bytes' expects a size between 0 and %d, received %d",
                         CRYPTO_RAND_MAX, length);
        return NIL_VAL;
    }
    bytes = object_bytes(length);
    /* RAND_bytes takes an int and returns 0 on failure; there is deliberately
     * no fallback to rand(), whose sequence is predictable. */
    if (length > 0 && RAND_bytes(bytes->data, length) != 1) {
        vm_runtime_error(ERROR_RUNTIME, "'rand_bytes' could not obtain randomness from the CSPRNG");
        return NIL_VAL;
    }
    return OBJ_VAL(bytes);
}

void crypto_init(void)
{
    vm_define_native("sha256", native_sha256, 1);
    vm_define_native("sha256_hex", native_sha256_hex, 1);
    vm_define_native("hmac_sha256", native_hmac_sha256, 2);
    vm_define_native("rand_bytes", native_rand_bytes, 1);
}