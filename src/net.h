/* net.h - TCP/UDP sockets exposed to the language.
 *
 * The native socket objects live in net.c; the VM registers them through
 * net_init() and routes method calls on them through net_dispatch_method().
 * Blocking operations honour a caller-supplied timeout and are candidates for
 * the coroutine-aware waits added in phase 16. */
#ifndef SPARK_NET_H
#define SPARK_NET_H

#include "object.h"
#include "value.h"

/* Registers the network natives with the VM. Called once from vm_init. */
void net_init(void);

/* Handles a method call on a socket or server receiver. Returns true when the
 * method was recognized (including when it raised a runtime error), in which
 * case *out holds the result value. */
bool net_dispatch_method(Value receiver, ObjString *method, int arg_count,
                         Value *args, Value *out);

#endif
