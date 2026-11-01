/* codec.c - base64, hex and DEFLATE/gzip codecs over `bytes`.
 *
 * Every base64/hex native accepts a `bytes` value or a `string` (whose UTF-8
 * bytes are used as they are, which is the only place a text/binary conversion
 * happens without naming `bytes_from`: it is a convenience for literals) and
 * returns `bytes`. `base64_encode` is exported for the JSON encoder, which emits
 * a base64 string for a `bytes` value.
 *
 * `inflate` (raw DEFLATE, RFC 1951) and `gunzip` (RFC 1952, CRC and size
 * verified) decode a bounded number of output bytes: the cap defaults to 16 MiB
 * and a stream that would exceed it raises instead of exhausting memory. The
 * decoder is adapted from the public-domain puff.c described below; zlib is not
 * a dependency (decision Q17). */
#include "codec.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
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

/* ---- DEFLATE (RFC 1951) and gzip (RFC 1952) -------------------------------
 *
 * The decoder follows the structure of Mark Adler's puff.c, the reference
 * public-domain ("zlib licence") minimal inflate shipped in zlib's contrib
 * directory: a bit-at-a-time canonical-Huffman decoder, the same fixed and
 * dynamic block handling, and the same length/distance tables. Spark does not
 * add zlib as a dependency (decision Q17); this is a bounded adaptation, and it
 * is bounded on purpose: every write checks a caller-chosen output cap, so a
 * decompression bomb fails with an error instead of exhausting memory. */

#define INFLATE_MAX_BITS 15
#define INFLATE_MAX_SYMBOLS 288
#define INFLATE_MAX_LENGTHS 316 /* 286 literal/length plus 30 distance symbols */
#define INFLATE_MAX_OUTPUT (16 * 1024 * 1024)

typedef struct {
    short count[INFLATE_MAX_BITS + 1];
    short symbol[INFLATE_MAX_SYMBOLS];
} InflateHuffman;

typedef struct {
    const unsigned char *data;
    size_t length;
    size_t position;
    uint32_t bits;
    int count;
} InflateBits;

typedef enum {
    INFLATE_OK,
    INFLATE_TRUNCATED,
    INFLATE_INVALID,
    INFLATE_LIMIT,
    INFLATE_NOT_GZIP,
    INFLATE_CHECKSUM
} InflateStatus;

static const short INFLATE_LENGTH_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const short INFLATE_LENGTH_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 0
};
static const short INFLATE_DISTANCE_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const short INFLATE_DISTANCE_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
    10, 11, 11, 12, 12, 13, 13
};
static const short INFLATE_CODE_ORDER[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int inflate_need(InflateBits *bits, int need)
{
    while (bits->count < need) {
        if (bits->position >= bits->length) {
            return -1;
        }
        bits->bits |= (uint32_t)bits->data[bits->position++] << bits->count;
        bits->count += 8;
    }
    return 0;
}

/* Reads `need` bits, least-significant bit first, or -1 at end of input. */
static int inflate_get(InflateBits *bits, int need)
{
    int value;

    if (need == 0) {
        return 0;
    }
    if (inflate_need(bits, need) != 0) {
        return -1;
    }
    value = (int)(bits->bits & ((1u << need) - 1));
    bits->bits >>= need;
    bits->count -= need;
    return value;
}

/* Drops the remaining bits of the current byte; `count` is always below 8, so
 * this leaves `position` at the next byte boundary. */
static void inflate_align(InflateBits *bits)
{
    bits->bits = 0;
    bits->count = 0;
}

/* Builds the canonical code from a length table. Returns the number of unused
 * codes: 0 is a complete code, a negative value an over-subscribed one and a
 * positive value an incomplete one (the caller decides whether that is legal).
 * A table with no codes at all returns 0 with every count zero. */
static int inflate_construct(InflateHuffman *huffman, const short *lengths, int symbols)
{
    short offsets[INFLATE_MAX_BITS + 1];
    int left;
    int length;

    for (length = 0; length <= INFLATE_MAX_BITS; length++) {
        huffman->count[length] = 0;
    }
    for (int symbol = 0; symbol < symbols; symbol++) {
        huffman->count[lengths[symbol]]++;
    }
    if (huffman->count[0] == symbols) {
        return 0;
    }

    left = 1;
    for (length = 1; length <= INFLATE_MAX_BITS; length++) {
        left <<= 1;
        left -= huffman->count[length];
        if (left < 0) {
            return -1;
        }
    }
    offsets[1] = 0;
    for (length = 1; length < INFLATE_MAX_BITS; length++) {
        offsets[length + 1] = (short)(offsets[length] + huffman->count[length]);
    }
    for (int symbol = 0; symbol < symbols; symbol++) {
        if (lengths[symbol] != 0) {
            huffman->symbol[offsets[lengths[symbol]]++] = (short)symbol;
        }
    }
    return left;
}

/* Decodes one symbol; -1 is a truncated stream and -2 a code that cannot
 * exist in this table. */
static int inflate_decode(InflateBits *bits, const InflateHuffman *huffman)
{
    int code = 0;
    int first = 0;
    int index = 0;

    for (int length = 1; length <= INFLATE_MAX_BITS; length++) {
        int bit = inflate_get(bits, 1);
        int count;

        if (bit < 0) {
            return -1;
        }
        code |= bit;
        count = huffman->count[length];
        if (code - count < first) {
            return huffman->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

/* Appends `length` bytes taken from `distance` bytes back. The caller has
 * already checked the cap and that `distance` is within the output. */
static bool inflate_copy_match(buf_t *out, size_t distance, size_t length)
{
    if (buf_reserve(out, length) != 0) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        out->data[out->len] = out->data[out->len - distance];
        out->len++;
    }
    out->data[out->len] = '\0';
    return true;
}

static InflateStatus inflate_codes(InflateBits *bits, const InflateHuffman *literal,
                                   const InflateHuffman *distances, size_t max, buf_t *out)
{
    for (;;) {
        int symbol = inflate_decode(bits, literal);

        if (symbol < 0) {
            return symbol == -1 ? INFLATE_TRUNCATED : INFLATE_INVALID;
        }
        if (symbol < 256) {
            unsigned char byte = (unsigned char)symbol;

            if (out->len + 1 > max) {
                return INFLATE_LIMIT;
            }
            if (buf_append(out, &byte, 1) != 0) {
                return INFLATE_LIMIT;
            }
        } else if (symbol == 256) {
            return INFLATE_OK;
        } else {
            int index = symbol - 257;
            int length;
            int extra;
            int distance_symbol;
            size_t match_distance;

            if (index >= 29) {
                return INFLATE_INVALID;
            }
            extra = inflate_get(bits, INFLATE_LENGTH_EXTRA[index]);
            if (extra < 0) {
                return INFLATE_TRUNCATED;
            }
            length = INFLATE_LENGTH_BASE[index] + extra;
            distance_symbol = inflate_decode(bits, distances);
            if (distance_symbol < 0) {
                return distance_symbol == -1 ? INFLATE_TRUNCATED : INFLATE_INVALID;
            }
            if (distance_symbol >= 30) {
                return INFLATE_INVALID;
            }
            extra = inflate_get(bits, INFLATE_DISTANCE_EXTRA[distance_symbol]);
            if (extra < 0) {
                return INFLATE_TRUNCATED;
            }
            match_distance = (size_t)INFLATE_DISTANCE_BASE[distance_symbol] + (size_t)extra;
            if (match_distance > out->len) {
                return INFLATE_INVALID;
            }
            if (out->len + (size_t)length > max) {
                return INFLATE_LIMIT;
            }
            /* A reserve failure is reported like the cap: it stays a catchable
             * error rather than a crash, and the cap makes it unreachable in
             * practice because `max` is at most 16 MiB. */
            if (!inflate_copy_match(out, match_distance, (size_t)length)) {
                return INFLATE_LIMIT;
            }
        }
    }
}

static InflateStatus inflate_dynamic(InflateBits *bits, size_t max, buf_t *out)
{
    short lengths[INFLATE_MAX_LENGTHS] = {0};
    InflateHuffman literal;
    InflateHuffman distance;
    InflateHuffman code;
    int literal_count;
    int distance_count;
    int code_count;
    int have;
    int left;
    int symbol;

    literal_count = inflate_get(bits, 5);
    distance_count = inflate_get(bits, 5);
    code_count = inflate_get(bits, 4);
    if (literal_count < 0 || distance_count < 0 || code_count < 0) {
        return INFLATE_TRUNCATED;
    }
    literal_count += 257;
    distance_count += 1;
    code_count += 4;
    if (literal_count > 286 || distance_count > 30) {
        return INFLATE_INVALID;
    }
    for (int i = 0; i < code_count; i++) {
        int value = inflate_get(bits, 3);

        if (value < 0) {
            return INFLATE_TRUNCATED;
        }
        lengths[INFLATE_CODE_ORDER[i]] = (short)value;
    }
    /* The code-length code must be complete. */
    if (inflate_construct(&code, lengths, 19) != 0) {
        return INFLATE_INVALID;
    }

    have = 0;
    while (have < literal_count + distance_count) {
        symbol = inflate_decode(bits, &code);
        if (symbol < 0) {
            return symbol == -1 ? INFLATE_TRUNCATED : INFLATE_INVALID;
        }
        if (symbol < 16) {
            lengths[have++] = (short)symbol;
        } else {
            int repeat;
            int value = 0;

            if (symbol == 16) {
                if (have == 0) {
                    return INFLATE_INVALID;
                }
                value = lengths[have - 1];
                repeat = inflate_get(bits, 2);
                if (repeat < 0) {
                    return INFLATE_TRUNCATED;
                }
                repeat += 3;
            } else if (symbol == 17) {
                repeat = inflate_get(bits, 3);
                if (repeat < 0) {
                    return INFLATE_TRUNCATED;
                }
                repeat += 3;
            } else {
                repeat = inflate_get(bits, 7);
                if (repeat < 0) {
                    return INFLATE_TRUNCATED;
                }
                repeat += 11;
            }
            if (have + repeat > literal_count + distance_count) {
                return INFLATE_INVALID;
            }
            while (repeat-- > 0) {
                lengths[have++] = (short)value;
            }
        }
    }
    if (lengths[256] == 0) {
        return INFLATE_INVALID;
    }

    left = inflate_construct(&literal, lengths, literal_count);
    /* An incomplete code is only legal when it has a single symbol (a stream
     * that never uses the others), exactly as puff allows. */
    if (left != 0 && !(left > 0 && literal_count == literal.count[0] + literal.count[1])) {
        return INFLATE_INVALID;
    }
    left = inflate_construct(&distance, lengths + literal_count, distance_count);
    if (left != 0 && !(left > 0 && distance_count == distance.count[0] + distance.count[1])) {
        return INFLATE_INVALID;
    }
    return inflate_codes(bits, &literal, &distance, max, out);
}

static InflateStatus inflate_fixed(InflateBits *bits, size_t max, buf_t *out)
{
    short lengths[INFLATE_MAX_SYMBOLS];
    InflateHuffman literal;
    InflateHuffman distance;

    for (int symbol = 0; symbol < 144; symbol++) {
        lengths[symbol] = 8;
    }
    for (int symbol = 144; symbol < 256; symbol++) {
        lengths[symbol] = 9;
    }
    for (int symbol = 256; symbol < 280; symbol++) {
        lengths[symbol] = 7;
    }
    for (int symbol = 280; symbol < 288; symbol++) {
        lengths[symbol] = 8;
    }
    /* The fixed literal/length code is complete; the fixed distance code has
     * 30 of the 32 length-5 codes defined, so it is intentionally incomplete
     * (symbols 30 and 31 are invalid and decode fails). */
    (void)inflate_construct(&literal, lengths, 288);
    for (int symbol = 0; symbol < 30; symbol++) {
        lengths[symbol] = 5;
    }
    (void)inflate_construct(&distance, lengths, 30);
    return inflate_codes(bits, &literal, &distance, max, out);
}

static InflateStatus inflate_raw(InflateBits *bits, size_t max, buf_t *out)
{
    int final = 0;

    while (!final) {
        int type;
        InflateStatus status;

        final = inflate_get(bits, 1);
        type = inflate_get(bits, 2);
        if (final < 0 || type < 0) {
            return INFLATE_TRUNCATED;
        }
        if (type == 0) {
            int length;
            int complement;

            inflate_align(bits);
            length = inflate_get(bits, 16);
            complement = inflate_get(bits, 16);
            if (length < 0 || complement < 0) {
                return INFLATE_TRUNCATED;
            }
            if ((length ^ 0xffff) != complement) {
                return INFLATE_INVALID;
            }
            if (out->len + (size_t)length > max) {
                return INFLATE_LIMIT;
            }
            if (bits->position + (size_t)length > bits->length) {
                return INFLATE_TRUNCATED;
            }
            if (buf_append(out, bits->data + bits->position, (size_t)length) != 0) {
                return INFLATE_LIMIT;
            }
            bits->position += (size_t)length;
            status = INFLATE_OK;
        } else if (type == 1) {
            status = inflate_fixed(bits, max, out);
        } else if (type == 2) {
            status = inflate_dynamic(bits, max, out);
        } else {
            return INFLATE_INVALID;
        }
        if (status != INFLATE_OK) {
            return status;
        }
    }
    return INFLATE_OK;
}

static uint32_t inflate_crc32(const unsigned char *data, size_t length)
{
    static uint32_t table[256];
    static bool ready = false;
    uint32_t crc = 0xffffffffu;

    if (!ready) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;

            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        ready = true;
    }
    for (size_t i = 0; i < length; i++) {
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

static uint32_t inflate_read32(const unsigned char *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

/* Decodes a complete gzip member (RFC 1952), verifying its CRC-32 and ISIZE. */
static InflateStatus inflate_gzip(const unsigned char *data, size_t length, size_t max,
                                  buf_t *out)
{
    unsigned char flags;
    size_t position = 10;
    size_t deflate_start;
    InflateBits bits;
    InflateStatus status;

    if (length < 2) {
        return INFLATE_TRUNCATED;
    }
    if (data[0] != 0x1f || data[1] != 0x8b) {
        return INFLATE_NOT_GZIP;
    }
    /* The fixed header is ten bytes and the trailer eight; a member shorter
     * than that cannot be complete. */
    if (length < 18) {
        return INFLATE_TRUNCATED;
    }
    if (data[2] != 8) {
        return INFLATE_INVALID;
    }
    flags = data[3];
    if ((flags & 0xe0) != 0) {
        return INFLATE_INVALID;
    }
    if ((flags & 0x04) != 0) { /* FEXTRA */
        size_t extra;

        if (position + 2 > length) {
            return INFLATE_TRUNCATED;
        }
        extra = (size_t)data[position] | ((size_t)data[position + 1] << 8);
        position += 2;
        if (extra > length - position) {
            return INFLATE_TRUNCATED;
        }
        position += extra;
    }
    if ((flags & 0x08) != 0) { /* FNAME */
        while (position < length && data[position] != 0) {
            position++;
        }
        if (position >= length) {
            return INFLATE_TRUNCATED;
        }
        position++;
    }
    if ((flags & 0x10) != 0) { /* FCOMMENT */
        while (position < length && data[position] != 0) {
            position++;
        }
        if (position >= length) {
            return INFLATE_TRUNCATED;
        }
        position++;
    }
    if ((flags & 0x02) != 0) { /* FHCRC, skipped: the payload CRC below is stronger */
        if (position + 2 > length) {
            return INFLATE_TRUNCATED;
        }
        position += 2;
    }
    deflate_start = position;
    bits.data = data + deflate_start;
    bits.length = length - deflate_start;
    bits.position = 0;
    bits.bits = 0;
    bits.count = 0;
    status = inflate_raw(&bits, max, out);
    if (status != INFLATE_OK) {
        return status;
    }
    inflate_align(&bits);
    if (bits.position + 8 > bits.length) {
        return INFLATE_TRUNCATED;
    }
    if (inflate_crc32((const unsigned char *)out->data, out->len) !=
            inflate_read32(data + deflate_start + bits.position) ||
        (out->len & 0xffffffffu) !=
            inflate_read32(data + deflate_start + bits.position + 4)) {
        return INFLATE_CHECKSUM;
    }
    return INFLATE_OK;
}

bool codec_zlib_header(const unsigned char *data, size_t length)
{
    unsigned int cmf;
    unsigned int flg;

    if (length < 2) {
        return false;
    }
    cmf = data[0];
    flg = data[1];
    if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7) {
        return false; /* CM must be DEFLATE with a 32 KiB window or less */
    }
    if (((cmf << 8) | flg) % 31 != 0) {
        return false; /* the header checksum must be a multiple of 31 */
    }
    return (flg & 0x20) == 0; /* FDICT: no preset dictionary is available */
}

static uint32_t inflate_adler32(const unsigned char *data, size_t length)
{
    uint32_t low = 1;
    uint32_t high = 0;

    for (size_t i = 0; i < length; i++) {
        low = (low + data[i]) % 65521u;
        high = (high + low) % 65521u;
    }
    return (high << 16) | low;
}

/* Decodes a complete RFC 1950 (zlib) stream, verifying its Adler-32. */
static InflateStatus inflate_zlib(const unsigned char *data, size_t length, size_t max,
                                 buf_t *out)
{
    InflateBits bits;
    InflateStatus status;
    const unsigned char *trailer;
    uint32_t stored;

    /* The two-byte header, at least one block and the four-byte trailer. */
    if (length < 6) {
        return INFLATE_TRUNCATED;
    }
    if (!codec_zlib_header(data, length)) {
        return INFLATE_INVALID;
    }
    bits.data = data + 2;
    bits.length = length - 2;
    bits.position = 0;
    bits.bits = 0;
    bits.count = 0;
    status = inflate_raw(&bits, max, out);
    if (status != INFLATE_OK) {
        return status;
    }
    inflate_align(&bits);
    if (bits.position + 4 > bits.length) {
        return INFLATE_TRUNCATED;
    }
    /* The Adler-32 trailer is stored most-significant byte first (RFC 1950),
     * unlike the little-endian gzip trailer above. */
    trailer = data + 2 + bits.position;
    stored = ((uint32_t)trailer[0] << 24) | ((uint32_t)trailer[1] << 16) |
             ((uint32_t)trailer[2] << 8) | (uint32_t)trailer[3];

    if (inflate_adler32((const unsigned char *)out->data, out->len) != stored) {
        return INFLATE_CHECKSUM;
    }
    return INFLATE_OK;
}

CodecDeflateStatus codec_deflate_decode(const unsigned char *data, size_t length, size_t max,
                                        CodecDeflateFormat format, buf_t *out)
{
    InflateStatus status;

    if (format == CODEC_DEFLATE_GZIP) {
        status = inflate_gzip(data, length, max, out);
    } else if (format == CODEC_DEFLATE_ZLIB) {
        status = inflate_zlib(data, length, max, out);
    } else {
        InflateBits bits;

        bits.data = data;
        bits.length = length;
        bits.position = 0;
        bits.bits = 0;
        bits.count = 0;
        status = inflate_raw(&bits, max, out);
    }
    switch (status) {
    case INFLATE_OK:
        return CODEC_DEFLATE_OK;
    case INFLATE_TRUNCATED:
        return CODEC_DEFLATE_TRUNCATED;
    case INFLATE_LIMIT:
        return CODEC_DEFLATE_LIMIT;
    case INFLATE_NOT_GZIP:
        return CODEC_DEFLATE_NOT_GZIP;
    case INFLATE_CHECKSUM:
        return CODEC_DEFLATE_CHECKSUM;
    case INFLATE_INVALID:
        break;
    }
    return CODEC_DEFLATE_INVALID;
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

/* The optional maximum-output argument of the decompressors, with the same
 * integer rule as the rest of the standard library. */
static bool codec_size_argument(Value value, const char *function, int *out)
{
    if (is_int(value)) {
        int64_t integer = AS_INT(value);

        if (integer < INT_MIN || integer > INT_MAX) {
            vm_runtime_error(ERROR_VALUE, "'%s' expects an integer as size, received %lld",
                             function, (long long)integer);
            return false;
        }
        *out = (int)integer;
        return true;
    }
    if (is_number(value)) {
        double raw = AS_NUMBER(value);

        if (raw != floor(raw) || raw < (double)INT_MIN || raw > (double)INT_MAX) {
            vm_runtime_error(ERROR_VALUE, "'%s' expects an integer as size, received %g", function,
                             raw);
            return false;
        }
        *out = (int)raw;
        return true;
    }
    vm_runtime_error(ERROR_VALUE, "'%s' expects an integer as size, received %s", function,
                     codec_value_type(value));
    return false;
}

static Value codec_decompress(int arg_count, Value *args, const char *function, bool gzip)
{
    const unsigned char *data;
    int length;
    int max = INFLATE_MAX_OUTPUT;
    buf_t out;
    CodecDeflateStatus status;

    if (arg_count < 1 || arg_count > 2) {
        vm_runtime_error(ERROR_ARITY, "'%s' expects 1 or 2 arguments, received %d", function,
                         arg_count);
        return NIL_VAL;
    }
    if (!codec_input(args[0], function, &data, &length)) {
        return NIL_VAL;
    }
    if (arg_count == 2 && !codec_size_argument(args[1], function, &max)) {
        return NIL_VAL;
    }
    if (max < 0 || max > INFLATE_MAX_OUTPUT) {
        vm_runtime_error(ERROR_VALUE, "'%s' maximum output out of valid range (0..%d)", function,
                         INFLATE_MAX_OUTPUT);
        return NIL_VAL;
    }

    buf_init(&out);
    status = codec_deflate_decode(data, (size_t)length, (size_t)max,
                                  gzip ? CODEC_DEFLATE_GZIP : CODEC_DEFLATE_RAW, &out);
    if (status != CODEC_DEFLATE_OK) {
        const char *kind = gzip ? "gzip" : "deflate";

        buf_free(&out);
        switch (status) {
        case CODEC_DEFLATE_TRUNCATED:
            vm_runtime_error(ERROR_VALUE, "'%s' received a truncated %s stream", function, kind);
            break;
        case CODEC_DEFLATE_INVALID:
            vm_runtime_error(ERROR_VALUE, "'%s' received an invalid %s stream", function, kind);
            break;
        case CODEC_DEFLATE_LIMIT:
            vm_runtime_error(ERROR_VALUE, "'%s' output exceeds the %d-byte limit", function, max);
            break;
        case CODEC_DEFLATE_NOT_GZIP:
            vm_runtime_error(ERROR_VALUE, "'%s' received data that is not a gzip stream",
                             function);
            break;
        case CODEC_DEFLATE_CHECKSUM:
            vm_runtime_error(ERROR_VALUE, "'%s' checksum mismatch", function);
            break;
        case CODEC_DEFLATE_OK:
            break;
        }
        return NIL_VAL;
    }
    return codec_result(&out);
}

static Value native_gunzip(int arg_count, Value *args)
{
    return codec_decompress(arg_count, args, "gunzip", true);
}

static Value native_inflate(int arg_count, Value *args)
{
    return codec_decompress(arg_count, args, "inflate", false);
}

void codec_init(void)
{
    vm_define_native("base64_encode", native_base64_encode, 1);
    vm_define_native("base64_decode", native_base64_decode, 1);
    vm_define_native("hex_encode", native_hex_encode, 1);
    vm_define_native("hex_decode", native_hex_decode, 1);
    vm_define_native("inflate", native_inflate, -1);
    vm_define_native("gunzip", native_gunzip, -1);
}