/* codec.h - base64 and hex codecs over `bytes`.
 *
 * The cores are exported because other modules reuse them (the JSON encoder
 * emits base64 for a `bytes` value); the language natives live in codec.c and
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

#endif