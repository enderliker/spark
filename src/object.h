/* object.h - language objects: strings, arrays, functions, closures and natives. */
#ifndef SPARK_OBJECT_H
#define SPARK_OBJECT_H

#include <stdio.h>

#include "chunk.h"
#include "error.h"
#include "table.h"
#include "value.h"

struct ObjClass;
struct ObjInstance;
struct ObjClosure;
struct ObjCoroutine;
struct ObjChannel;
struct ObjSocket;
struct ObjServer;

struct ObjString {
    Obj obj;
    uint32_t hash;
    int length;
    struct ObjString *intern_next;
    char chars[]; /* NUL-terminated string */
};

typedef struct {
    Obj obj;
    Value *items;
    int count;
    int capacity;
} ObjArray;

typedef struct {
    Value key;
    Value value;
    uint32_t hash;
    uint8_t state; /* 0 = free, 1 = occupied, 2 = tombstone */
} MapEntry;

typedef struct {
    Obj obj;
    MapEntry *entries;
    int count;
    int tombstones;
    int capacity;
} ObjMap;

typedef struct {
    int slot;
    ObjString *name;
    int start_pc;
    int end_pc;
} DebugLocal;

typedef struct ObjFunction {
    Obj obj;
    int arity;
    int min_arity;
    bool is_varargs;
    int upvalue_count;
    Chunk chunk;
    ObjString *name;
    ObjString *script_path;
    const char *source; /* borrowed from the VM; used to show error snippets */
    DebugLocal *debug_locals;
    int debug_local_count;
    int debug_local_capacity;
} ObjFunction;

typedef struct ObjUpvalue {
    Obj obj;
    Value *location;
    Value closed;
    struct ObjUpvalue *next;
    /* Coroutine whose stack `location` points into while the upvalue is open.
     * Marking it keeps an escaped closure from outliving its stack. */
    struct ObjCoroutine *owner;
} ObjUpvalue;

typedef struct ObjClosure {
    Obj obj;
    ObjFunction *function;
    ObjUpvalue **upvalues;
    int upvalue_count;
    /* Class that declared the method, or NULL for plain functions. `super`
     * starts its lookup at klass->superclass. */
    struct ObjClass *klass;
    /* True when this closure is stored in a class's static table. */
    bool is_static;
} ObjClosure;

/* A class: instance methods, static members and the closure that initializes
 * the fields this class declares (base classes are initialized first). */
typedef struct ObjClass {
    Obj obj;
    ObjString *name;
    struct ObjClass *superclass;
    Table methods;
    Table statics;
    struct ObjClosure *field_init;
} ObjClass;

/* A class instance. Fields live in a map so field access reuses the same hash
 * table as maps; methods are looked up through the class chain. */
typedef struct ObjInstance {
    Obj obj;
    ObjClass *klass;
    ObjMap *fields;
} ObjInstance;

typedef enum {
    CORO_NEW,
    CORO_RUNNABLE,
    CORO_RUNNING,
    CORO_BLOCKED,
    CORO_DONE
} CoroutineStatus;

typedef enum {
    CORO_WAIT_NONE,
    CORO_WAIT_SEND,
    CORO_WAIT_RECEIVE,
    CORO_WAIT_JOIN,
    CORO_WAIT_READ,
    CORO_WAIT_WRITE
} CoroutineWaitKind;

/* Readiness a socket wait is interested in. */
#define NET_EVENT_READ 1
#define NET_EVENT_WRITE 2

/* Pending network operation a blocked coroutine performs (or reports) when its
 * descriptor becomes ready or its deadline expires. */
typedef enum {
    NET_OP_WAIT = 0, /* explicit sock.wait: result is `true` on readiness */
    NET_OP_RECV,
    NET_OP_RECV_LINE,
    NET_OP_SEND,
    NET_OP_ACCEPT,
    NET_OP_SELECT,
    NET_OP_TLS_HANDSHAKE,
    NET_OP_TLS_READ,
    NET_OP_TLS_WRITE
} NetWaitOp;

/* A cooperative green thread. While suspended it owns a private value stack
 * and a snapshot of its call frames; the VM swaps those in when it runs. The
 * CallFrame and TryHandler layouts are private to vm.c, so they are stored as
 * void pointers here. */
typedef struct ObjCoroutine {
    Obj obj;
    CoroutineStatus status;
    bool started;
    bool owns_stack;

    Value *stack;
    int stack_count;
    int stack_capacity;
    void *frames;
    int frame_count;
    int frame_capacity;
    size_t frame_bytes;
    ObjUpvalue *open_upvalues;
    void *try_handlers;
    int try_handler_count;
    int try_handler_capacity;
    size_t try_handler_bytes;
    bool has_exception;
    Value exception_value;
    bool had_error;

    /* Scheduler links. `wait_next` is used by channel wait queues and by the
     * joiners list of the coroutine being joined. */
    struct ObjCoroutine *queue_next;
    struct ObjCoroutine *wait_next;
    struct ObjCoroutine *joiners;
    struct ObjCoroutine *wait_target;
    struct ObjChannel *wait_channel;
    CoroutineWaitKind wait_kind;
    Value wait_value;
    /* Network wait state (phase 16): the socket or server being awaited, the
     * operation to retry on wake-up, the requested readiness bits and the
     * absolute monotonic deadline in milliseconds (0 = wait forever). */
    struct ObjSocket *wait_socket;
    struct ObjServer *wait_server;
    ObjArray *wait_sockets;
    NetWaitOp wait_op;
    int wait_events;
    int wait_arg;
    int64_t wait_timeout; /* configured timeout in ms, for the error message */
    int64_t wait_deadline;
    bool has_resume_result;
    Value resume_result;
    const char *resume_error;
    ErrorType resume_error_type;
    /* Backing storage for a formatted wake-up error (for example a timeout). */
    char resume_error_buffer[256];
    /* Bytecode position of the method call that blocked, used to report
     * errors (such as sending on a channel closed while waiting). */
    uint8_t *resume_ip;

    Value result;
} ObjCoroutine;

/* FIFO channel. Capacity zero means an unbuffered rendezvous channel. */
typedef struct ObjChannel {
    Obj obj;
    Value *buffer;
    int capacity;
    int count;
    int head;
    bool closed;
    struct ObjCoroutine *waiting_senders;
    struct ObjCoroutine *waiting_senders_tail;
    struct ObjCoroutine *waiting_receivers;
    struct ObjCoroutine *waiting_receivers_tail;
} ObjChannel;

typedef struct ObjFile {
    Obj obj;
    FILE *stream;
    bool is_closed;
    ObjString *path;
} ObjFile;

/* Transport of a socket object. TCP sockets are byte streams; UDP sockets are
 * datagram endpoints that must use send_to/recv_from. */
typedef enum {
    NET_TCP,
    NET_UDP,
    NET_TLS
} NetKind;

/* A GC-managed socket, modeled on ObjFile: it owns a file descriptor that is
 * released by close(), by the collector's finalizer and on VM shutdown. TCP
 * sockets also keep a small read buffer so recv_line never consumes bytes past
 * the end of the line it returns. */
typedef struct ObjSocket {
    Obj obj;
    int fd;
    bool is_closed;
    NetKind kind;
    int64_t timeout_ms; /* default deadline for this socket; 0 = wait forever */
    void *ssl; /* SSL * for NET_TLS sockets, NULL otherwise */
    void *ssl_ctx; /* SSL_CTX * owned by this socket, NULL otherwise */
    char *rbuf; /* buffered but unconsumed bytes for recv_line (may be NULL) */
    int rbuf_cap;
    int rbuf_len;
    int rbuf_pos;
} ObjSocket;

/* A listening TCP server: owns the listening descriptor. */
typedef struct ObjServer {
    Obj obj;
    int fd;
    bool is_closed;
} ObjServer;

typedef struct {
    Obj obj;
    char *chars;
    int length;
    int capacity;
} ObjStringBuilder;

#define AS_FUNCTION(v) ((ObjFunction *)(v).as.obj)
#define AS_CLOSURE(v) ((ObjClosure *)(v).as.obj)
#define AS_ARRAY(v) ((ObjArray *)(v).as.obj)
#define AS_MAP(v) ((ObjMap *)(v).as.obj)
#define AS_FILE(v) ((ObjFile *)(v).as.obj)
#define AS_SOCKET(v) ((ObjSocket *)(v).as.obj)
#define AS_SERVER(v) ((ObjServer *)(v).as.obj)
#define AS_CLASS(v) ((ObjClass *)(v).as.obj)
#define AS_INSTANCE(v) ((ObjInstance *)(v).as.obj)
#define AS_COROUTINE(v) ((ObjCoroutine *)(v).as.obj)
#define AS_CHANNEL(v) ((ObjChannel *)(v).as.obj)

typedef Value (*NativeFn)(int arg_count, Value *args);

typedef struct {
    Obj obj;
    NativeFn function;
    int arity; /* -1 = variadic */
    ObjString *name;
} ObjNative;

/* Linked list of all live objects (for the GC). */
Obj *objects_head(void);
void objects_set_head(Obj *head);

ObjString *object_string(const char *chars, int length);
ObjString *object_string_concat(ObjString *a, ObjString *b);
ObjString *value_to_string(Value v);

ObjArray *object_array(void);
bool array_push(ObjArray *array, Value value);
bool array_pop(ObjArray *array, Value *out);

ObjMap *object_map(void);
bool map_get(ObjMap *map, Value key, Value *out);
/* Same as map_get, but also reports the entry index so callers can cache the
 * probe location (the method inline cache in the VM does this). */
bool map_get_index(ObjMap *map, Value key, Value *out, int *out_index);
bool map_set(ObjMap *map, Value key, Value value);
bool map_delete(ObjMap *map, Value key);

ObjClass *object_class(ObjString *name, ObjClass *superclass);
ObjInstance *object_instance(ObjClass *klass);
/* Finds `name` in `klass` or any superclass. */
ObjClosure *class_find_method(ObjClass *klass, ObjString *name);
Value *class_find_static(ObjClass *klass, ObjString *name);
bool class_is_subclass(ObjClass *klass, ObjClass *candidate);

ObjFunction *object_function(void);
ObjClosure *object_closure(ObjFunction *function);
ObjNative *object_native(NativeFn fn, int arity, const char *name);
ObjUpvalue *object_upvalue(Value *slot);
ObjCoroutine *object_coroutine(void);
ObjChannel *object_channel(int capacity);
ObjFile *object_file(FILE *stream, ObjString *path);
ObjSocket *object_socket(int fd, NetKind kind, int64_t timeout_ms);
ObjServer *object_server(int fd);
ObjStringBuilder *object_string_builder(int initial_capacity);
bool string_builder_append(ObjStringBuilder *sb, const char *chars, int len);
void string_builder_clear(ObjStringBuilder *sb);
ObjString *string_builder_to_string(ObjStringBuilder *sb);

void object_free(Obj *obj);
void intern_remove_unmarked(void);
void intern_free_table(void);

/* Value printing: `print` uses the readable form; `repr` quotes strings. */
void value_print(FILE *out, Value v);
void value_print_repr(FILE *out, Value v);

#endif
