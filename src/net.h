/* net.h - TCP/UDP sockets exposed to the language.
 *
 * The native socket objects live in net.c: the VM registers them through
 * net_init() and routes method calls on them through net_dispatch_method().
 * A would-block operation registers its coroutine as a network waiter; the
 * scheduler polls those descriptors (net_scheduler_poll) and net.c performs the
 * pending operation when one is ready or its deadline expires. */
#ifndef SPARK_NET_H
#define SPARK_NET_H

#include "object.h"
#include "value.h"

/* Registers the network natives with the VM. Called once from vm_init. */
void net_init(void);

/* Handles a method call on a socket or server receiver. Returns 0 when the
 * method completed (the result is written to `out`), 1 when the current
 * coroutine was suspended waiting for a descriptor, and -1 after raising an
 * error. Unknown methods return 3 (SPARK_NET_UNHANDLED) so the caller can
 * report a normal "method not found". */
#define SPARK_NET_UNHANDLED 3
int net_dispatch_method(Value receiver, ObjString *method, int arg_count, Value *args, Value *out);

/* Scheduler integration. `may_block` lets the poll sleep until the nearest
 * deadline instead of doing a non-blocking check. */
bool net_has_waiters(void);
void net_scheduler_poll(bool may_block);
void net_mark_waiters(void);

/* Monotonic milliseconds, shared by the socket deadlines and the scheduler. */
int64_t net_now_ms(void);

#endif
