/* codec.h - base64, hex and DEFLATE/gzip codecs over `bytes`.
 *
 * The base64/hex cores are exported because other modules reuse them (the JSON
 * encoder emits base64 for a `bytes` value); the language natives (base64 and
 * hex, plus the bounded `inflate`/`gunzip` decompressors) live in codec.c and
 * are registered with the VM through codec_init() from vm_init, the way
 * bytes_init() registers the `bytes` constructors. */
#ifndef SPARK_CODEC_H
#define SPARK_CODEC_H

#include <stdbool.h>

#include "buf.h"

/* Registers the base64 and hex natives with the VM. Called once from vm_init. */
void codec_init(void);

/* Appends the RFC 4648 base64 encoding of `data` to `out`, without line
 * breaks. */
void codec_base64_encode(buf_t *out, const unsigned char *data, int length);

/* The compression formats the bounded decompressor understands. `deflate` is
 * ambiguous on the wire (RFC 9110 allows either the zlib or the raw form), so
 * the two are distinct here and the caller picks with codec_zlib_header(). */
typedef enum {
    CODEC_DEFLATE_RAW = 0, /* RFC 1951 */
    CODEC_DEFLATE_ZLIB,    /* RFC 1950 */
    CODEC_DEFLATE_GZIP     /* RFC 1952 */
} CodecDeflateFormat;

/* Why a decode failed. The HTTP client maps these onto its own error
 * categories, so they carry no message text. */
typedef enum {
    CODEC_DEFLATE_OK = 0,
    CODEC_DEFLATE_TRUNCATED,
    CODEC_DEFLATE_INVALID,
    CODEC_DEFLATE_LIMIT,
    CODEC_DEFLATE_NOT_GZIP,
    CODEC_DEFLATE_CHECKSUM
} CodecDeflateStatus;

/* Decodes `data` into `out`, refusing to produce more than `max` bytes. Raises
 * no VM error and frees nothing on failure: the caller owns `out` either way
 * (call buf_free once). */
CodecDeflateStatus codec_deflate_decode(const unsigned char *data, size_t length, size_t max,
                                        CodecDeflateFormat format, buf_t *out);

/* True when `data` starts with a well-formed RFC 1950 zlib header (no preset
 * dictionary), which is how the ambiguous `deflate` content coding is told
 * apart from the raw form. */
bool codec_zlib_header(const unsigned char *data, size_t length);

#endif