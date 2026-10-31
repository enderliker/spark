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

/* Handles a method call whose receiver is a `bytes_builder` (append, to_bytes,
 * len, clear). Returns false when the name is not one of them, so the VM
 * reports the same "method not found" error it gives for other types. */
bool bytes_builder_dispatch(Value receiver, ObjString *method, int arg_count, Value *args,
                            Value *out);

/* Handles the conversion methods: `bytes.to_string()` and `string.to_bytes()`.
 * Returns false when the receiver/name pair is not one of them. */
bool bytes_dispatch_method(Value receiver, ObjString *method, int arg_count, Value *args,
                           Value *out);

#endif
