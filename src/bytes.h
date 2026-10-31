/* bytes.h - the immutable `bytes` type and its construction natives.
 *
 * `ObjBytes` itself lives in object.c beside the other objects (allocation,
 * equality, hashing and printing); this module owns the language-level
 * constructors `bytes()` and `bytes_from()`, registered with the VM through
 * bytes_init() from vm_init, the way net_init() registers the network natives.
 *
 * There is no implicit conversion between `string` and `bytes` (decision A16):
 * `bytes_from` and `to_string` are the explicit bridges. */
#ifndef SPARK_BYTES_H
#define SPARK_BYTES_H

#include "object.h"

/* Registers the `bytes` natives with the VM. Called once from vm_init. */
void bytes_init(void);

#endif
