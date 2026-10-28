/* net.c - TCP/UDP sockets exposed to the language.
 *
 * Sockets are GC-managed resources modeled on ObjFile: each owns a file
 * descriptor and is released by close(), the collector's finalizer or VM
 * shutdown. Every blocking operation takes an optional millisecond timeout; a
 * timeout of zero (or an omitted argument) waits indefinitely.
 *
 * The socket layer adds no dependencies: it uses the POSIX socket API only.
 * Phase 16 routes these waits through the coroutine scheduler; until then a
 * blocking call stalls every coroutine, which is documented in the limits
 * section of the language reference. */

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "memory.h"
#include "vm.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Not exposed under _POSIX_C_SOURCE alone. */
#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

/* Largest single read (and largest accepted recv/recv_line bound). */
#define NET_READ_MAX (16 * 1024 * 1024)
#define NET_LINE_DEFAULT_MAX (64 * 1024)
#define NET_LISTEN_BACKLOG 128

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    ErrorType type;
    char message[256];
} NetError;

static void net_fail(NetError *err, ErrorType type, const char *fmt, ...)
{
    va_list ap;

    err->type = type;
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, ap);
    va_end(ap);
}

static void net_raise(const NetError *err)
{
    vm_runtime_error(err->type, "%s", err->message);
}

/* Translates an errno value into a category and a human message. */
static void net_fail_errno(NetError *err, int error, const char *action)
{
    switch (error) {
    case ETIMEDOUT:
        net_fail(err, ERROR_TIMEOUT, "operation timed out");
        return;
    case ECONNREFUSED:
        net_fail(err, ERROR_NETWORK, "connection refused");
        return;
    case ECONNRESET:
    case EPIPE:
        net_fail(err, ERROR_NETWORK, "connection reset by peer");
        return;
    case EMFILE:
    case ENFILE:
        net_fail(err, ERROR_NETWORK, "too many open descriptors");
        return;
    case EADDRINUSE:
        net_fail(err, ERROR_NETWORK, "address already in use");
        return;
    case EADDRNOTAVAIL:
        net_fail(err, ERROR_NETWORK, "address not available");
        return;
    case EACCES:
    case EPERM:
        net_fail(err, ERROR_NETWORK, "permission denied");
        return;
    case ENETUNREACH:
        net_fail(err, ERROR_NETWORK, "network unreachable");
        return;
    case EHOSTUNREACH:
        net_fail(err, ERROR_NETWORK, "host unreachable");
        return;
    case EAFNOSUPPORT:
        net_fail(err, ERROR_NETWORK, "address family not supported");
        return;
    default:
        net_fail(err, ERROR_NETWORK, "%s: %s", action, strerror(error));
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *net_value_type(Value value)
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

static bool net_string_has_null(ObjString *string, const char *function)
{
    if (strlen(string->chars) != (size_t)string->length) {
        vm_runtime_error(ERROR_VALUE, "'%s' argument cannot contain null bytes", function);
        return true;
    }
    return false;
}

/* Parses an optional trailing timeout in milliseconds. A missing value or nil
 * means "wait forever" (0); a negative value is rejected. */
static bool net_optional_timeout(Value value, int arg_count, int index, const char *function,
                                 int64_t *out)
{
    *out = 0;
    if (arg_count <= index || is_nil(value)) {
        return true;
    }
    if (!is_int(value)) {
        vm_runtime_error(ERROR_TYPE, "'%s' expects an integer timeout in milliseconds, received %s",
                         function, net_value_type(value));
        return false;
    }
    if (AS_INT(value) < 0) {
        vm_runtime_error(ERROR_VALUE, "'%s' timeout cannot be negative", function);
        return false;
    }
    *out = AS_INT(value);
    return true;
}

/* Parses a required integer, raising a type error when it is not one. */
static bool net_int_argument(Value value, const char *function, const char *name, int64_t *out)
{
    if (!is_int(value)) {
        vm_runtime_error(ERROR_TYPE, "'%s' expects an integer %s, received %s", function, name,
                         net_value_type(value));
        return false;
    }
    *out = AS_INT(value);
    return true;
}

/* Validates a port number and formats it for getaddrinfo. */
static bool net_port_service(int64_t port, char *service, size_t size, NetError *err)
{
    if (port < 0 || port > 65535) {
        net_fail(err, ERROR_NETWORK, "invalid port %lld", (long long)port);
        return false;
    }
    snprintf(service, size, "%lld", (long long)port);
    return true;
}

/* Strips the brackets of an IPv6 literal ("[::1]" -> "::1"). Returns a pointer
 * either to `buffer` or to the original string. */
static const char *net_strip_brackets(const char *host, char *buffer, size_t size)
{
    size_t length = strlen(host);

    if (length >= 2 && host[0] == '[' && host[length - 1] == ']' && length - 2 < size) {
        memcpy(buffer, host + 1, length - 2);
        buffer[length - 2] = '\0';
        return buffer;
    }
    return host;
}

static int64_t net_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static int64_t net_deadline(int64_t timeout_ms)
{
    return timeout_ms > 0 ? net_now_ms() + timeout_ms : 0;
}

/* poll timeout for a deadline: -1 blocks forever, 0 has already expired. */
static int net_poll_timeout(int64_t deadline)
{
    int64_t remaining;

    if (deadline == 0) {
        return -1;
    }
    remaining = deadline - net_now_ms();
    if (remaining <= 0) {
        return 0;
    }
    if (remaining > INT32_MAX) {
        return INT32_MAX;
    }
    return (int)remaining;
}

/* Waits for one of `events` on `fd`. Returns 1 when ready, 0 on timeout and -1
 * on error (errno set). */
static int net_wait(int fd, short events, int64_t deadline)
{
    for (;;) {
        int timeout = net_poll_timeout(deadline);
        struct pollfd descriptor;

        if (timeout == 0) {
            return 0;
        }
        descriptor.fd = fd;
        descriptor.events = events;
        descriptor.revents = 0;

        int ready = poll(&descriptor, 1, timeout);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (ready == 0) {
            return 0;
        }
        return 1;
    }
}

static void net_set_nonblocking(int fd, bool enabled)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return;
    }
    if (enabled) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    fcntl(fd, F_SETFL, flags);
}

/* ------------------------------------------------------------------ */
/* Connection setup                                                    */
/* ------------------------------------------------------------------ */

/* Connects a stream socket to host:port, trying each resolved address and
 * honouring the deadline. Returns the descriptor or -1 with `err` filled. */
static int net_connect(const char *host, int64_t port, int64_t deadline, NetError *err)
{
    char host_buffer[NI_MAXHOST];
    char service[16];
    const char *node = net_strip_brackets(host, host_buffer, sizeof(host_buffer));
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    int fd = -1;
    int saved = 0;

    if (!net_port_service(port, service, sizeof(service), err)) {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(node, service, &hints, &result);

    if (gai != 0) {
        if (gai == EAI_NONAME || gai == EAI_FAIL || gai == EAI_AGAIN) {
            net_fail(err, ERROR_NETWORK, "unknown host '%s'", host);
        } else if (gai == EAI_SERVICE || gai == EAI_SOCKTYPE) {
            net_fail(err, ERROR_NETWORK, "invalid port %lld", (long long)port);
        } else {
            net_fail(err, ERROR_NETWORK, "cannot resolve '%s': %s", host, gai_strerror(gai));
        }
        return -1;
    }

    for (struct addrinfo *ai = result; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            saved = errno;
            continue;
        }

        net_set_nonblocking(fd, true);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            net_set_nonblocking(fd, false);
            break;
        }
        if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EINTR) {
            saved = errno;
            close(fd);
            fd = -1;
            continue;
        }

        int ready = net_wait(fd, POLLOUT, deadline);

        if (ready == 0) {
            close(fd);
            freeaddrinfo(result);
            net_fail(err, ERROR_TIMEOUT, "timed out connecting to %s:%lld", host, (long long)port);
            return -1;
        }
        if (ready < 0) {
            saved = errno;
            close(fd);
            fd = -1;
            continue;
        }

        int socket_error = 0;
        socklen_t length = sizeof(socket_error);

        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0 || socket_error != 0) {
            saved = (socket_error != 0) ? socket_error : errno;
            close(fd);
            fd = -1;
            continue;
        }
        net_set_nonblocking(fd, false);
        break;
    }

    freeaddrinfo(result);

    if (fd < 0) {
        net_fail_errno(err, saved != 0 ? saved : ECONNREFUSED, "connect");
        return -1;
    }
    return fd;
}

/* Binds and listens on host:port. Returns the descriptor or -1 with `err`. */
static int net_listen(const char *host, int64_t port, int backlog, NetError *err)
{
    char host_buffer[NI_MAXHOST];
    const char *node = net_strip_brackets(host, host_buffer, sizeof(host_buffer));
    char service[16];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    int fd = -1;
    int saved = 0;
    int reuse = 1;

    if (!net_port_service(port, service, sizeof(service), err)) {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int gai = getaddrinfo(node[0] == '\0' ? NULL : node, service, &hints, &result);

    if (gai != 0) {
        if (gai == EAI_NONAME || gai == EAI_FAIL) {
            net_fail(err, ERROR_NETWORK, "unknown host '%s'", host);
        } else {
            net_fail(err, ERROR_NETWORK, "cannot listen on '%s': %s", host, gai_strerror(gai));
        }
        return -1;
    }

    for (struct addrinfo *ai = result; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            saved = errno;
            continue;
        }
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            saved = errno;
            close(fd);
            fd = -1;
            continue;
        }
        if (listen(fd, backlog) != 0) {
            saved = errno;
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }

    freeaddrinfo(result);

    if (fd < 0) {
        net_fail_errno(err, saved != 0 ? saved : EADDRNOTAVAIL, "listen");
        return -1;
    }
    return fd;
}

/* Binds a datagram socket. Returns the descriptor or -1 with `err`. */
static int net_udp_bind(const char *host, int64_t port, NetError *err)
{
    char host_buffer[NI_MAXHOST];
    const char *node = net_strip_brackets(host, host_buffer, sizeof(host_buffer));
    char service[16];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    int fd = -1;
    int saved = 0;
    int reuse = 1;

    if (!net_port_service(port, service, sizeof(service), err)) {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    int gai = getaddrinfo(node[0] == '\0' ? NULL : node, service, &hints, &result);

    if (gai != 0) {
        if (gai == EAI_NONAME || gai == EAI_FAIL) {
            net_fail(err, ERROR_NETWORK, "unknown host '%s'", host);
        } else {
            net_fail(err, ERROR_NETWORK, "cannot bind '%s': %s", host, gai_strerror(gai));
        }
        return -1;
    }

    for (struct addrinfo *ai = result; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            saved = errno;
            continue;
        }
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            saved = errno;
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }

    freeaddrinfo(result);

    if (fd < 0) {
        net_fail_errno(err, saved != 0 ? saved : EADDRNOTAVAIL, "bind");
        return -1;
    }
    return fd;
}

/* Writes the whole buffer, retrying short writes until the deadline. Returns 0
 * on success, -2 on timeout and -1 on error. `written` gets the bytes sent. */
static int net_send_all(int fd, const char *data, int length, int64_t deadline, size_t *written)
{
    size_t total = 0;

    while (total < (size_t)length) {
        int ready = net_wait(fd, POLLOUT, deadline);

        if (ready == 0) {
            *written = total;
            return -2;
        }
        if (ready < 0) {
            *written = total;
            return -1;
        }

        ssize_t sent = send(fd, data + total, (size_t)length - total, MSG_NOSIGNAL);

        if (sent < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            *written = total;
            return -1;
        }
        total += (size_t)sent;
    }
    *written = total;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Address formatting                                                  */
/* ------------------------------------------------------------------ */

static ObjMap *net_address_map(const struct sockaddr *address, socklen_t length)
{
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];
    ObjMap *map;

    if (getnameinfo(address, length, host, sizeof(host), service, sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        strcpy(host, "?");
        strcpy(service, "0");
    }

    map = object_map();
    map_set(map, OBJ_VAL(object_string("host", 4)), OBJ_VAL(object_string(host, (int)strlen(host))));
    map_set(map, OBJ_VAL(object_string("port", 4)), INT_VAL(strtoll(service, NULL, 10)));
    return map;
}

/* ------------------------------------------------------------------ */
/* Native functions                                                    */
/* ------------------------------------------------------------------ */

static Value native_tcp_connect(int arg_count, Value *args)
{
    if (arg_count < 2 || arg_count > 3) {
        vm_runtime_error(ERROR_ARITY, "'tcp_connect' expects 2 or 3 arguments, received %d",
                         arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'tcp_connect' expects a string host, received %s",
                         net_value_type(args[0]));
        return NIL_VAL;
    }
    ObjString *host = AS_STRING(args[0]);

    if (net_string_has_null(host, "tcp_connect")) {
        return NIL_VAL;
    }

    int64_t port = 0;
    int64_t timeout = 0;

    if (!net_int_argument(args[1], "tcp_connect", "port", &port)) {
        return NIL_VAL;
    }
    if (!net_optional_timeout(arg_count > 2 ? args[2] : NIL_VAL, arg_count, 2, "tcp_connect",
                              &timeout)) {
        return NIL_VAL;
    }

    NetError err;
    int fd = net_connect(host->chars, port, net_deadline(timeout), &err);

    if (fd < 0) {
        net_raise(&err);
        return NIL_VAL;
    }
    return OBJ_VAL(object_socket(fd, NET_TCP, timeout));
}

static Value native_tcp_listen(int arg_count, Value *args)
{
    if (arg_count < 2 || arg_count > 3) {
        vm_runtime_error(ERROR_ARITY, "'tcp_listen' expects 2 or 3 arguments, received %d",
                         arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'tcp_listen' expects a string host, received %s",
                         net_value_type(args[0]));
        return NIL_VAL;
    }
    ObjString *host = AS_STRING(args[0]);

    if (net_string_has_null(host, "tcp_listen")) {
        return NIL_VAL;
    }

    int64_t port = 0;
    int64_t backlog = NET_LISTEN_BACKLOG;

    if (!net_int_argument(args[1], "tcp_listen", "port", &port)) {
        return NIL_VAL;
    }
    if (arg_count == 3 && !is_nil(args[2]) &&
        !net_int_argument(args[2], "tcp_listen", "backlog", &backlog)) {
        return NIL_VAL;
    }
    if (backlog < 1) {
        backlog = 1;
    }

    NetError err;
    int fd = net_listen(host->chars, port, (int)backlog, &err);

    if (fd < 0) {
        net_raise(&err);
        return NIL_VAL;
    }
    return OBJ_VAL(object_server(fd));
}

static Value native_udp_bind(int arg_count, Value *args)
{
    if (arg_count < 2 || arg_count > 3) {
        vm_runtime_error(ERROR_ARITY, "'udp_bind' expects 2 or 3 arguments, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'udp_bind' expects a string host, received %s",
                         net_value_type(args[0]));
        return NIL_VAL;
    }
    ObjString *host = AS_STRING(args[0]);

    if (net_string_has_null(host, "udp_bind")) {
        return NIL_VAL;
    }

    int64_t port = 0;
    int64_t timeout = 0;

    if (!net_int_argument(args[1], "udp_bind", "port", &port)) {
        return NIL_VAL;
    }
    if (!net_optional_timeout(arg_count > 2 ? args[2] : NIL_VAL, arg_count, 2, "udp_bind",
                              &timeout)) {
        return NIL_VAL;
    }

    NetError err;
    int fd = net_udp_bind(host->chars, port, &err);

    if (fd < 0) {
        net_raise(&err);
        return NIL_VAL;
    }
    return OBJ_VAL(object_socket(fd, NET_UDP, timeout));
}

static Value native_net_hostname(int arg_count, Value *args)
{
    char name[256];

    (void)arg_count;
    (void)args;
    if (gethostname(name, sizeof(name)) != 0) {
        vm_runtime_error(ERROR_NETWORK, "cannot read the host name: %s", strerror(errno));
        return NIL_VAL;
    }
    name[sizeof(name) - 1] = '\0';
    return OBJ_VAL(object_string(name, (int)strlen(name)));
}

void net_init(void)
{
    vm_define_native("tcp_connect", native_tcp_connect, -1);
    vm_define_native("tcp_listen", native_tcp_listen, -1);
    vm_define_native("udp_bind", native_udp_bind, -1);
    vm_define_native("net_hostname", native_net_hostname, 0);
}

/* ------------------------------------------------------------------ */
/* Socket methods                                                      */
/* ------------------------------------------------------------------ */

static bool net_socket_open(ObjSocket *socket)
{
    if (socket->is_closed) {
        vm_runtime_error(ERROR_RUNTIME, "cannot use a closed socket");
        return false;
    }
    return true;
}

static bool net_server_open(ObjServer *server)
{
    if (server->is_closed) {
        vm_runtime_error(ERROR_RUNTIME, "cannot use a closed server");
        return false;
    }
    return true;
}

static void net_socket_reset_buffer(ObjSocket *socket)
{
    socket->rbuf_len = 0;
    socket->rbuf_pos = 0;
}

static Value net_socket_close(ObjSocket *socket)
{
    if (socket->is_closed) {
        vm_runtime_error(ERROR_RUNTIME, "socket is already closed");
        return NIL_VAL;
    }
    close(socket->fd);
    socket->fd = -1;
    socket->is_closed = true;
    net_socket_reset_buffer(socket);
    return NIL_VAL;
}

static Value net_socket_send(ObjSocket *socket, Value data)
{
    if (!is_obj_type(data, OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'send' expects a string, received %s", net_value_type(data));
        return NIL_VAL;
    }
    if (socket->kind != NET_TCP) {
        vm_runtime_error(ERROR_NETWORK, "'send' requires a TCP socket; use 'send_to' for UDP");
        return NIL_VAL;
    }

    ObjString *payload = AS_STRING(data);
    size_t written = 0;
    int status = net_send_all(socket->fd, payload->chars, payload->length,
                              net_deadline(socket->timeout_ms), &written);

    if (status == -2) {
        vm_runtime_error(ERROR_TIMEOUT, "'send' timed out after %lld ms",
                         (long long)socket->timeout_ms);
        return NIL_VAL;
    }
    if (status < 0) {
        NetError err;

        net_fail_errno(&err, errno, "send failed");
        net_raise(&err);
        return NIL_VAL;
    }
    return INT_VAL((int64_t)written);
}

static Value net_socket_recv(ObjSocket *socket, int64_t max_value)
{
    int max;
    char *buffer;

    if (max_value < 0 || max_value > NET_READ_MAX) {
        vm_runtime_error(ERROR_VALUE, "'recv' size out of valid range (0..%d)", NET_READ_MAX);
        return NIL_VAL;
    }
    max = (int)max_value;
    if (max == 0) {
        return OBJ_VAL(object_string("", 0));
    }

    buffer = malloc((size_t)max);

    if (!buffer) {
        vm_runtime_error(ERROR_MEMORY, "out of memory in 'recv'");
        return NIL_VAL;
    }

    int total = 0;

    if (socket->rbuf_pos < socket->rbuf_len) {
        int available = socket->rbuf_len - socket->rbuf_pos;
        int take = available < max ? available : max;

        memcpy(buffer, socket->rbuf + socket->rbuf_pos, (size_t)take);
        socket->rbuf_pos += take;
        if (socket->rbuf_pos >= socket->rbuf_len) {
            net_socket_reset_buffer(socket);
        }
        total = take;
    }

    if (total == 0) {
        int64_t deadline = net_deadline(socket->timeout_ms);
        int ready = net_wait(socket->fd, POLLIN, deadline);

        if (ready == 0) {
            free(buffer);
            vm_runtime_error(ERROR_TIMEOUT, "'recv' timed out after %lld ms",
                             (long long)socket->timeout_ms);
            return NIL_VAL;
        }
        if (ready < 0) {
            free(buffer);
            NetError err;

            net_fail_errno(&err, errno, "recv failed");
            net_raise(&err);
            return NIL_VAL;
        }

        ssize_t received = recv(socket->fd, buffer, (size_t)max, 0);

        if (received < 0) {
            NetError err;

            free(buffer);
            net_fail_errno(&err, errno, "recv failed");
            net_raise(&err);
            return NIL_VAL;
        }
        if (received == 0) {
            free(buffer);
            return NIL_VAL; /* EOF */
        }
        total = (int)received;
    }

    ObjString *result = object_string(buffer, total);

    free(buffer);
    return OBJ_VAL(result);
}

static Value net_socket_recv_line(ObjSocket *socket, int max)
{
    buf_t line;
    int64_t deadline = net_deadline(socket->timeout_ms);

    buf_init(&line);

    for (;;) {
        if (socket->rbuf_pos < socket->rbuf_len) {
            char *start = socket->rbuf + socket->rbuf_pos;
            int available = socket->rbuf_len - socket->rbuf_pos;
            char *newline = memchr(start, '\n', (size_t)available);
            int take = newline ? (int)(newline - start) : available;

            if (buf_append(&line, start, (size_t)take) != 0) {
                buf_free(&line);
                vm_runtime_error(ERROR_MEMORY, "out of memory in 'recv_line'");
                return NIL_VAL;
            }
            socket->rbuf_pos += take;

            if (newline) {
                socket->rbuf_pos++;
                if (socket->rbuf_pos >= socket->rbuf_len) {
                    net_socket_reset_buffer(socket);
                }
                int length = (int)line.len;

                if (length > 0 && line.data[length - 1] == '\r') {
                    length--;
                }
                ObjString *result = object_string(line.data ? line.data : "", length);

                buf_free(&line);
                return OBJ_VAL(result);
            }
            if (socket->rbuf_pos >= socket->rbuf_len) {
                net_socket_reset_buffer(socket);
            }
            if (line.len > (size_t)max) {
                buf_free(&line);
                vm_runtime_error(ERROR_VALUE, "'recv_line' exceeded the maximum of %d bytes", max);
                return NIL_VAL;
            }
            continue;
        }

        int ready = net_wait(socket->fd, POLLIN, deadline);

        if (ready == 0) {
            buf_free(&line);
            vm_runtime_error(ERROR_TIMEOUT, "'recv_line' timed out after %lld ms",
                             (long long)socket->timeout_ms);
            return NIL_VAL;
        }
        if (ready < 0) {
            buf_free(&line);
            NetError err;

            net_fail_errno(&err, errno, "recv failed");
            net_raise(&err);
            return NIL_VAL;
        }

        ssize_t received = recv(socket->fd, socket->rbuf, (size_t)socket->rbuf_cap, 0);

        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            NetError err;

            buf_free(&line);
            net_fail_errno(&err, errno, "recv failed");
            net_raise(&err);
            return NIL_VAL;
        }
        if (received == 0) {
            if (line.len == 0) {
                buf_free(&line);
                return NIL_VAL; /* EOF with nothing read */
            }
            ObjString *result = object_string(line.data ? line.data : "", (int)line.len);

            buf_free(&line);
            return OBJ_VAL(result);
        }
        socket->rbuf_len = (int)received;
        socket->rbuf_pos = 0;
    }
}

static Value net_socket_send_to(ObjSocket *socket, Value data, Value host_value, Value port_value)
{
    if (socket->kind != NET_UDP) {
        vm_runtime_error(ERROR_NETWORK, "'send_to' requires a UDP socket");
        return NIL_VAL;
    }
    if (!is_obj_type(data, OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'send_to' expects a string, received %s", net_value_type(data));
        return NIL_VAL;
    }
    if (!is_obj_type(host_value, OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'send_to' expects a string host, received %s",
                         net_value_type(host_value));
        return NIL_VAL;
    }
    ObjString *host = AS_STRING(host_value);

    if (net_string_has_null(host, "send_to")) {
        return NIL_VAL;
    }

    int64_t port = 0;

    if (!net_int_argument(port_value, "send_to", "port", &port)) {
        return NIL_VAL;
    }

    char host_buffer[NI_MAXHOST];
    char service[16];
    const char *node = net_strip_brackets(host->chars, host_buffer, sizeof(host_buffer));
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    NetError err;

    if (!net_port_service(port, service, sizeof(service), &err)) {
        net_raise(&err);
        return NIL_VAL;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    int gai = getaddrinfo(node, service, &hints, &result);

    if (gai != 0) {
        if (gai == EAI_NONAME || gai == EAI_FAIL) {
            vm_runtime_error(ERROR_NETWORK, "unknown host '%s'", host->chars);
        } else {
            vm_runtime_error(ERROR_NETWORK, "cannot resolve '%s': %s", host->chars, gai_strerror(gai));
        }
        return NIL_VAL;
    }

    ObjString *payload = AS_STRING(data);
    ssize_t sent = -1;

    for (struct addrinfo *ai = result; ai; ai = ai->ai_next) {
        int ready = net_wait(socket->fd, POLLOUT, net_deadline(socket->timeout_ms));

        if (ready == 0) {
            freeaddrinfo(result);
            vm_runtime_error(ERROR_TIMEOUT, "'send_to' timed out after %lld ms",
                             (long long)socket->timeout_ms);
            return NIL_VAL;
        }
        if (ready < 0) {
            break;
        }
        sent = sendto(socket->fd, payload->chars, (size_t)payload->length, MSG_NOSIGNAL,
                      ai->ai_addr, ai->ai_addrlen);
        if (sent >= 0 || (errno != EAFNOSUPPORT && errno != EINTR)) {
            break;
        }
    }

    freeaddrinfo(result);

    if (sent < 0) {
        NetError err;

        net_fail_errno(&err, errno, "send_to failed");
        net_raise(&err);
        return NIL_VAL;
    }
    return INT_VAL((int64_t)sent);
}

static Value net_socket_recv_from(ObjSocket *socket, int64_t max_value)
{
    int max;
    int ready;
    char *buffer;
    struct sockaddr_storage source;
    socklen_t source_length;
    ssize_t received;
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];

    if (socket->kind != NET_UDP) {
        vm_runtime_error(ERROR_NETWORK, "'recv_from' requires a UDP socket");
        return NIL_VAL;
    }
    if (max_value < 0 || max_value > NET_READ_MAX) {
        vm_runtime_error(ERROR_VALUE, "'recv_from' size out of valid range (0..%d)", NET_READ_MAX);
        return NIL_VAL;
    }
    max = (int)max_value;
    if (max == 0) {
        max = 1;
    }

    ready = net_wait(socket->fd, POLLIN, net_deadline(socket->timeout_ms));

    if (ready == 0) {
        vm_runtime_error(ERROR_TIMEOUT, "'recv_from' timed out after %lld ms",
                         (long long)socket->timeout_ms);
        return NIL_VAL;
    }
    if (ready < 0) {
        NetError err;

        net_fail_errno(&err, errno, "recv_from failed");
        net_raise(&err);
        return NIL_VAL;
    }

    buffer = malloc((size_t)max + 1);

    if (!buffer) {
        vm_runtime_error(ERROR_MEMORY, "out of memory in 'recv_from'");
        return NIL_VAL;
    }

    source_length = sizeof(source);
    received = recvfrom(socket->fd, buffer, (size_t)max, 0,
                        (struct sockaddr *)&source, &source_length);

    if (received < 0) {
        NetError err;

        free(buffer);
        net_fail_errno(&err, errno, "recv_from failed");
        net_raise(&err);
        return NIL_VAL;
    }

    if (getnameinfo((struct sockaddr *)&source, source_length, host, sizeof(host), service,
                    sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        strcpy(host, "?");
        strcpy(service, "0");
    }

    mem_pause_gc();
    ObjMap *map = object_map();

    map_set(map, OBJ_VAL(object_string("data", 4)), OBJ_VAL(object_string(buffer, (int)received)));
    map_set(map, OBJ_VAL(object_string("host", 4)), OBJ_VAL(object_string(host, (int)strlen(host))));
    map_set(map, OBJ_VAL(object_string("port", 4)), INT_VAL(strtoll(service, NULL, 10)));
    mem_resume_gc();
    free(buffer);
    return OBJ_VAL(map);
}

static Value net_socket_address(ObjSocket *socket, bool peer)
{
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    int status = peer ? getpeername(socket->fd, (struct sockaddr *)&address, &length)
                      : getsockname(socket->fd, (struct sockaddr *)&address, &length);

    if (status != 0) {
        NetError err;

        net_fail_errno(&err, errno, peer ? "getpeername failed" : "getsockname failed");
        net_raise(&err);
        return NIL_VAL;
    }

    mem_pause_gc();
    ObjMap *map = net_address_map((struct sockaddr *)&address, length);

    mem_resume_gc();
    return OBJ_VAL(map);
}

/* ------------------------------------------------------------------ */
/* Server methods                                                      */
/* ------------------------------------------------------------------ */

static Value net_server_accept(ObjServer *server, int64_t timeout)
{
    int64_t deadline = net_deadline(timeout);

    for (;;) {
        int ready = net_wait(server->fd, POLLIN, deadline);

        if (ready == 0) {
            return NIL_VAL; /* timed out: not an error for accept */
        }
        if (ready < 0) {
            NetError err;

            net_fail_errno(&err, errno, "accept failed");
            net_raise(&err);
            return NIL_VAL;
        }

        int fd = accept(server->fd, NULL, NULL);

        if (fd >= 0) {
            return OBJ_VAL(object_socket(fd, NET_TCP, timeout));
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) {
            continue;
        }
        NetError err;

        net_fail_errno(&err, errno, "accept failed");
        net_raise(&err);
        return NIL_VAL;
    }
}

static Value net_server_port(ObjServer *server)
{
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);

    if (getsockname(server->fd, (struct sockaddr *)&address, &length) != 0) {
        NetError err;

        net_fail_errno(&err, errno, "getsockname failed");
        net_raise(&err);
        return NIL_VAL;
    }
    if (address.ss_family == AF_INET) {
        return INT_VAL(ntohs(((struct sockaddr_in *)&address)->sin_port));
    }
    if (address.ss_family == AF_INET6) {
        return INT_VAL(ntohs(((struct sockaddr_in6 *)&address)->sin6_port));
    }
    return INT_VAL(0);
}

static Value net_server_close(ObjServer *server)
{
    if (server->is_closed) {
        vm_runtime_error(ERROR_RUNTIME, "server is already closed");
        return NIL_VAL;
    }
    close(server->fd);
    server->fd = -1;
    server->is_closed = true;
    return NIL_VAL;
}

/* ------------------------------------------------------------------ */
/* Method dispatch                                                     */
/* ------------------------------------------------------------------ */

bool net_dispatch_method(Value receiver, ObjString *method, int arg_count, Value *args, Value *out)
{
    const char *name = method->chars;

    if (is_obj_type(receiver, OBJ_SOCKET)) {
        ObjSocket *socket = AS_SOCKET(receiver);

        if (strcmp(name, "close") == 0) {
            if (arg_count != 0) {
                vm_runtime_error(ERROR_ARITY, "'close' expects no arguments, received %d", arg_count);
                *out = NIL_VAL;
                return true;
            }
            *out = net_socket_close(socket);
            return true;
        }
        if (strcmp(name, "send") == 0 && arg_count == 1) {
            if (net_socket_open(socket)) {
                *out = net_socket_send(socket, args[0]);
            } else {
                *out = NIL_VAL;
            }
            return true;
        }
        if (strcmp(name, "recv") == 0 && arg_count == 1) {
            int64_t max = 0;

            if (!net_int_argument(args[0], "recv", "size", &max)) {
                *out = NIL_VAL;
                return true;
            }
            if (net_socket_open(socket)) {
                *out = net_socket_recv(socket, max);
            } else {
                *out = NIL_VAL;
            }
            return true;
        }
        if (strcmp(name, "recv_line") == 0 && (arg_count == 0 || arg_count == 1)) {
            int64_t max = NET_LINE_DEFAULT_MAX;

            if (arg_count == 1 && !is_nil(args[0]) &&
                !net_int_argument(args[0], "recv_line", "size", &max)) {
                *out = NIL_VAL;
                return true;
            }
            if (max <= 0 || max > NET_READ_MAX) {
                vm_runtime_error(ERROR_VALUE, "'recv_line' size out of valid range (1..%d)",
                                 NET_READ_MAX);
                *out = NIL_VAL;
                return true;
            }
            if (net_socket_open(socket)) {
                *out = net_socket_recv_line(socket, (int)max);
            } else {
                *out = NIL_VAL;
            }
            return true;
        }
        if (strcmp(name, "send_to") == 0 && arg_count == 3) {
            if (net_socket_open(socket)) {
                *out = net_socket_send_to(socket, args[0], args[1], args[2]);
            } else {
                *out = NIL_VAL;
            }
            return true;
        }
        if (strcmp(name, "recv_from") == 0 && arg_count == 1) {
            int64_t max = 0;

            if (!net_int_argument(args[0], "recv_from", "size", &max)) {
                *out = NIL_VAL;
                return true;
            }
            if (net_socket_open(socket)) {
                *out = net_socket_recv_from(socket, max);
            } else {
                *out = NIL_VAL;
            }
            return true;
        }
        if ((strcmp(name, "peer") == 0 || strcmp(name, "local") == 0) && arg_count == 0) {
            if (net_socket_open(socket)) {
                *out = net_socket_address(socket, strcmp(name, "peer") == 0);
            } else {
                *out = NIL_VAL;
            }
            return true;
        }
        return false;
    }

    if (is_obj_type(receiver, OBJ_SERVER)) {
        ObjServer *server = AS_SERVER(receiver);

        if (strcmp(name, "close") == 0) {
            if (arg_count != 0) {
                vm_runtime_error(ERROR_ARITY, "'close' expects no arguments, received %d", arg_count);
                *out = NIL_VAL;
                return true;
            }
            *out = net_server_close(server);
            return true;
        }
        if (strcmp(name, "accept") == 0 && (arg_count == 0 || arg_count == 1)) {
            int64_t timeout = 0;

            if (!net_optional_timeout(arg_count > 0 ? args[0] : NIL_VAL, arg_count, 0, "accept",
                                      &timeout)) {
                *out = NIL_VAL;
                return true;
            }
            if (net_server_open(server)) {
                *out = net_server_accept(server, timeout);
            } else {
                *out = NIL_VAL;
            }
            return true;
        }
        if (strcmp(name, "port") == 0 && arg_count == 0) {
            if (net_server_open(server)) {
                *out = net_server_port(server);
            } else {
                *out = NIL_VAL;
            }
            return true;
        }
        return false;
    }

    return false;
}
