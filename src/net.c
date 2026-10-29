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
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

/* HTTP limits (phase 17). Every bound has an explicit error: the status line
 * plus the header block may not exceed 64 KiB, a body defaults to 16 MiB and
 * redirects stop after five hops. */
#define NET_HTTP_HEADER_MAX (64 * 1024)
#define NET_HTTP_BODY_DEFAULT_MAX NET_READ_MAX
#define NET_HTTP_REDIRECT_MAX 5
#define NET_HTTP_MAX_HEADERS 1000
#define NET_HTTP_TIMEOUT_DEFAULT 30000
#define NET_HTTP_USER_AGENT "spark/0.1"

/* Result of a non-blocking socket operation. */
#define NET_DONE 0
#define NET_AGAIN 1
#define NET_FAILED (-1)

/* TLS verification depth and the minimum protocol version accepted. */
#define NET_TLS_VERIFY_DEPTH 10

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

int64_t net_now_ms(void)
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
        net_set_nonblocking(fd, true);
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
        net_set_nonblocking(fd, true);
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

/* ------------------------------------------------------------------ */
/* TLS (phase 18)                                                      */
/* ------------------------------------------------------------------ */

/* Formats the most recent OpenSSL error into a categorized network failure.
 * Certificate verification failures are reported separately by
 * net_tls_verified(), because OpenSSL keeps the verification result on the SSL
 * object even when the handshake aborts. */
static void net_tls_error(NetError *err, const char *action, int ssl_error)
{
    unsigned long code = ERR_get_error();
    char detail[256];
    if (code != 0) {
        ERR_error_string_n(code, detail, sizeof(detail));
    } else if (ssl_error == SSL_ERROR_SYSCALL && errno != 0) {
        snprintf(detail, sizeof(detail), "%s", strerror(errno));
    } else if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        snprintf(detail, sizeof(detail), "the peer closed the connection");
    } else if (ssl_error == SSL_ERROR_SYSCALL) {
        snprintf(detail, sizeof(detail), "the connection ended unexpectedly");
    } else {
        snprintf(detail, sizeof(detail), "unknown TLS error");
    }
    net_fail(err, ERROR_NETWORK, "TLS %s failed: %s", action, detail);
}

/* Rejects a chain that OpenSSL accepted without a valid verification result.
 * With SSL_VERIFY_PEER this is a defense-in-depth check: a successful handshake
 * already implies X509_V_OK. */
static bool net_tls_verified(SSL *ssl, NetError *err)
{
    long result = SSL_get_verify_result(ssl);

    if (result != X509_V_OK) {
        net_fail(err, ERROR_NETWORK, "TLS certificate verification failed: %s",
                 X509_verify_cert_error_string(result));
        return false;
    }
    return true;
}

/* Creates a client context that always verifies the peer: the chain is checked
 * against the system trust store, or against `ca_file` when the caller supplies
 * one, and TLS 1.2 is the minimum accepted version. There is deliberately no
 * option to disable verification. */
static SSL_CTX *net_tls_context(const char *ca_file, NetError *err)
{
    /* Drop errors left by an earlier operation so a failure here reports its
     * own cause instead of a stale one. */
    ERR_clear_error();

    SSL_CTX *context = SSL_CTX_new(TLS_client_method());

    if (!context) {
        net_fail(err, ERROR_MEMORY, "cannot create the TLS context");
        return NULL;
    }
    SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_verify_depth(context, NET_TLS_VERIFY_DEPTH);
    SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION);
#ifdef SSL_OP_NO_RENEGOTIATION
    /* A server cannot force a renegotiation in the middle of a stream. */
    SSL_CTX_set_options(context, SSL_OP_NO_RENEGOTIATION);
#endif
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    /* A peer that closes the TCP stream without close_notify is treated as a
     * normal end of stream. HTTP framing still detects a truncated body. */
    SSL_CTX_set_options(context, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif

    int loaded = ca_file ? SSL_CTX_load_verify_locations(context, ca_file, NULL)
                         : SSL_CTX_set_default_verify_paths(context);

    if (loaded != 1) {
        unsigned long code = ERR_get_error();
        char detail[256];

        if (code != 0) {
            ERR_error_string_n(code, detail, sizeof(detail));
        } else if (ca_file) {
            snprintf(detail, sizeof(detail), "the file is missing or is not a PEM certificate");
        } else {
            snprintf(detail, sizeof(detail), "no system trust store is available");
        }
        if (ca_file) {
            net_fail(err, ERROR_NETWORK, "cannot load the CA file '%s': %s", ca_file, detail);
        } else {
            net_fail(err, ERROR_NETWORK, "cannot load the system trust store: %s", detail);
        }
        SSL_CTX_free(context);
        return NULL;
    }
    return context;
}

/* Creates the SSL object and pins the expected peer identity: SNI plus a DNS
 * host-name check for names, an IP-address check for IPv4/IPv6 literals. */
static SSL *net_tls_client(SSL_CTX *context, const char *host, int fd, NetError *err)
{
    ERR_clear_error();

    SSL *ssl = SSL_new(context);
    char host_buffer[NI_MAXHOST];
    const char *name;
    unsigned char address[16];
    bool is_ip;

    if (!ssl) {
        net_fail(err, ERROR_MEMORY, "cannot create the TLS connection");
        return NULL;
    }
    if (SSL_set_fd(ssl, fd) != 1) {
        net_tls_error(err, "setup", SSL_ERROR_SYSCALL);
        SSL_free(ssl);
        return NULL;
    }

    name = net_strip_brackets(host, host_buffer, sizeof(host_buffer));
    is_ip = inet_pton(AF_INET, name, address) == 1 || inet_pton(AF_INET6, name, address) == 1;

    if (is_ip) {
        if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), name) != 1) {
            net_fail(err, ERROR_NETWORK, "cannot verify the TLS peer address '%s'", host);
            SSL_free(ssl);
            return NULL;
        }
    } else {
        if (SSL_set_tlsext_host_name(ssl, name) != 1) {
            net_fail(err, ERROR_NETWORK, "cannot set the TLS server name '%s'", host);
            SSL_free(ssl);
            return NULL;
        }
        /* Reject partial wildcards such as "w*.example.com". */
        SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (SSL_set1_host(ssl, name) != 1) {
            net_fail(err, ERROR_NETWORK, "cannot verify the TLS host name '%s'", host);
            SSL_free(ssl);
            return NULL;
        }
    }
    return ssl;
}

/* One non-blocking handshake step. Returns 1 when the handshake is complete,
 * 0 when OpenSSL wants readiness on `*events`, and -1 with `err` filled. */
static int net_tls_handshake_step(SSL *ssl, short *events, NetError *err)
{
    ERR_clear_error();
    errno = 0;

    int status = SSL_connect(ssl);

    if (status == 1) {
        return net_tls_verified(ssl, err) ? 1 : -1;
    }

    int error = SSL_get_error(ssl, status);

    if (error == SSL_ERROR_WANT_READ) {
        *events = POLLIN;
        return 0;
    }
    if (error == SSL_ERROR_WANT_WRITE) {
        *events = POLLOUT;
        return 0;
    }
    if (SSL_get_verify_result(ssl) != X509_V_OK) {
        net_tls_verified(ssl, err);
        return -1;
    }
    net_tls_error(err, "handshake", error);
    return -1;
}

/* Blocking handshake used by the HTTP client, which is documented as
 * synchronous. The socket methods use the coroutine scheduler instead. */
static bool net_tls_handshake_blocking(SSL *ssl, int fd, int64_t deadline, NetError *err)
{
    for (;;) {
        short events = 0;
        int status = net_tls_handshake_step(ssl, &events, err);

        if (status != 0) {
            return status > 0;
        }

        int ready = net_wait(fd, events, deadline);

        if (ready == 0) {
            net_fail(err, ERROR_TIMEOUT, "the TLS handshake timed out");
            return false;
        }
        if (ready < 0) {
            net_fail_errno(err, errno, "poll failed during the TLS handshake");
            return false;
        }
    }
}

/* Blocking read used by the HTTP client. Returns the byte count, 0 at end of
 * stream and -1 with `err` filled. */
static ssize_t net_tls_read_blocking(SSL *ssl, int fd, char *buffer, size_t capacity,
                                     int64_t deadline, NetError *err)
{
    if (capacity == 0) {
        return 0;
    }
    for (;;) {
        size_t received = 0;

        ERR_clear_error();
        errno = 0;
        int status = SSL_read_ex(ssl, buffer, capacity, &received);

        if (status == 1) {
            return (ssize_t)received;
        }

        int error = SSL_get_error(ssl, status);

        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            int ready = net_wait(fd, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline);

            if (ready == 0) {
                net_fail(err, ERROR_TIMEOUT, "the TLS read timed out");
                return -1;
            }
            if (ready < 0) {
                net_fail_errno(err, errno, "poll failed during a TLS read");
                return -1;
            }
            continue;
        }
        if (error == SSL_ERROR_ZERO_RETURN || (error == SSL_ERROR_SYSCALL && errno == 0)) {
            return 0;
        }
        net_tls_error(err, "read", error);
        return -1;
    }
}

/* Releases the SSL/SSL_CTX owned by a socket without touching its descriptor. */
void net_socket_release_tls(ObjSocket *socket)
{
    if (socket->ssl) {
        SSL_shutdown((SSL *)socket->ssl);
        ERR_clear_error();
        SSL_free((SSL *)socket->ssl);
        socket->ssl = NULL;
    }
    if (socket->ssl_ctx) {
        SSL_CTX_free((SSL_CTX *)socket->ssl_ctx);
        socket->ssl_ctx = NULL;
    }
}

/* Frees the TLS state and closes the descriptor without raising. Used by close()
 * and by the error paths of tls_connect. */
static void net_socket_dispose(ObjSocket *socket)
{
    net_socket_release_tls(socket);
    if (socket->fd >= 0) {
        close(socket->fd);
        socket->fd = -1;
    }
    socket->is_closed = true;
    socket->rbuf_len = 0;
    socket->rbuf_pos = 0;
}

/* One non-blocking TLS read, for the coroutine scheduler. */
static int net_do_tls_read(ObjSocket *socket, int max, short *events, Value *out, NetError *err)
{
    SSL *ssl = (SSL *)socket->ssl;
    char *buffer;
    size_t received = 0;

    if (socket->is_closed || !ssl) {
        net_fail(err, ERROR_RUNTIME, "cannot use a closed socket");
        return NET_FAILED;
    }
    if (max == 0) {
        *out = OBJ_VAL(object_string("", 0));
        return NET_DONE;
    }

    buffer = malloc((size_t)max);
    if (!buffer) {
        net_fail(err, ERROR_MEMORY, "out of memory in 'read'");
        return NET_FAILED;
    }

    ERR_clear_error();
    errno = 0;
    int status = SSL_read_ex(ssl, buffer, (size_t)max, &received);

    if (status == 1) {
        if (received == 0) {
            free(buffer);
            *out = NIL_VAL;
            return NET_DONE;
        }
        ObjString *result = object_string(buffer, (int)received);

        free(buffer);
        *out = OBJ_VAL(result);
        return NET_DONE;
    }

    int error = SSL_get_error(ssl, status);

    free(buffer);
    if (error == SSL_ERROR_WANT_READ) {
        *events = POLLIN;
        return NET_AGAIN;
    }
    if (error == SSL_ERROR_WANT_WRITE) {
        *events = POLLOUT;
        return NET_AGAIN;
    }
    if (error == SSL_ERROR_ZERO_RETURN || (error == SSL_ERROR_SYSCALL && errno == 0)) {
        *out = NIL_VAL;
        return NET_DONE;
    }
    net_tls_error(err, "read", error);
    return NET_FAILED;
}

/* One non-blocking TLS write, continuing at `offset`. */
static int net_do_tls_write(ObjSocket *socket, ObjString *payload, size_t offset, size_t *progress,
                            short *events, Value *out, NetError *err)
{
    SSL *ssl = (SSL *)socket->ssl;
    size_t written = 0;

    if (socket->is_closed || !ssl) {
        net_fail(err, ERROR_RUNTIME, "cannot use a closed socket");
        return NET_FAILED;
    }
    if (offset >= (size_t)payload->length) {
        *progress = offset;
        *out = INT_VAL((int64_t)offset);
        return NET_DONE;
    }

    ERR_clear_error();
    errno = 0;
    int status = SSL_write_ex(ssl, payload->chars + offset, (size_t)payload->length - offset,
                              &written);

    if (status == 1) {
        if (written == 0) {
            net_fail(err, ERROR_NETWORK, "TLS write failed: no progress");
            return NET_FAILED;
        }
        *progress = offset + written;
        if (*progress >= (size_t)payload->length) {
            *out = INT_VAL((int64_t)*progress);
            return NET_DONE;
        }
        *events = POLLOUT;
        return NET_AGAIN;
    }

    int error = SSL_get_error(ssl, status);

    if (error == SSL_ERROR_WANT_READ) {
        *events = POLLIN;
        *progress = offset;
        return NET_AGAIN;
    }
    if (error == SSL_ERROR_WANT_WRITE) {
        *events = POLLOUT;
        *progress = offset;
        return NET_AGAIN;
    }
    net_tls_error(err, "write", error);
    return NET_FAILED;
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
/* URL parsing and encoding (phase 17)                                 */
/* ------------------------------------------------------------------ */

/* A parsed absolute URL. Every string field is owned by the caller and
 * released with net_url_free(). `host` keeps the brackets of an IPv6 literal
 * ("[::1]") so it can feed both the Host header and getaddrinfo. */
typedef struct {
    char *scheme; /* lowercased */
    char *host;
    bool port_known;
    int64_t port;
    char *path; /* never empty: "/" when the URL omits it */
    char *query;
    char *fragment;
} NetUrl;

static char *net_strndup_owned(const char *text, size_t length)
{
    char *copy = mem_realloc(NULL, 0, length + 1);

    if (!copy) {
        return NULL;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static char *net_strndup_lower(const char *text, size_t length)
{
    char *copy = net_strndup_owned(text, length);

    if (!copy) {
        return NULL;
    }
    for (size_t i = 0; i < length; i++) {
        copy[i] = (char)tolower((unsigned char)copy[i]);
    }
    return copy;
}

static void net_url_free(NetUrl *url)
{
    mem_realloc(url->scheme, url->scheme ? strlen(url->scheme) + 1 : 0, 0);
    mem_realloc(url->host, url->host ? strlen(url->host) + 1 : 0, 0);
    mem_realloc(url->path, url->path ? strlen(url->path) + 1 : 0, 0);
    mem_realloc(url->query, url->query ? strlen(url->query) + 1 : 0, 0);
    mem_realloc(url->fragment, url->fragment ? strlen(url->fragment) + 1 : 0, 0);
    memset(url, 0, sizeof(*url));
}

static bool net_scheme_char(int c)
{
    return isalnum(c) || c == '+' || c == '-' || c == '.';
}

/* Parses "scheme://[userinfo@]host[:port][/path][?query][#fragment]". A missing
 * path becomes "/"; a missing query or fragment becomes "". Malformed input
 * fails with a value error instead of a half-built URL. */
static bool net_url_parse(const char *text, NetUrl *out, NetError *err)
{
    const char *scheme_sep = strstr(text, "://");

    memset(out, 0, sizeof(*out));

    if (!scheme_sep || scheme_sep == text) {
        net_fail(err, ERROR_VALUE, "invalid URL '%s': expected scheme://host", text);
        return false;
    }
    size_t scheme_length = (size_t)(scheme_sep - text);

    if (!isalpha((unsigned char)text[0])) {
        net_fail(err, ERROR_VALUE, "invalid URL '%s': the scheme must start with a letter", text);
        return false;
    }
    for (size_t i = 0; i < scheme_length; i++) {
        if (!net_scheme_char((unsigned char)text[i])) {
            net_fail(err, ERROR_VALUE, "invalid URL '%s': bad character in the scheme", text);
            return false;
        }
    }
    out->scheme = net_strndup_lower(text, scheme_length);

    const char *authority = scheme_sep + 3;
    const char *authority_end = authority + strcspn(authority, "/?#");

    if (authority_end == authority) {
        net_fail(err, ERROR_VALUE, "invalid URL '%s': missing host", text);
        goto fail;
    }

    /* Userinfo ("user:pass@host") is accepted and ignored. */
    const char *host_start = authority;

    for (const char *p = authority; p < authority_end; p++) {
        if (*p == '@') {
            host_start = p + 1;
        }
    }
    if (host_start == authority_end) {
        net_fail(err, ERROR_VALUE, "invalid URL '%s': missing host", text);
        goto fail;
    }

    const char *host_end;
    const char *port_start = NULL;

    if (*host_start == '[') {
        const char *close = memchr(host_start, ']', (size_t)(authority_end - host_start));

        if (!close) {
            net_fail(err, ERROR_VALUE, "invalid URL '%s': unterminated IPv6 literal", text);
            goto fail;
        }
        host_end = close + 1; /* keep the brackets */
        if (host_end < authority_end) {
            if (*host_end != ':') {
                net_fail(err, ERROR_VALUE, "invalid URL '%s': unexpected text after the host", text);
                goto fail;
            }
            port_start = host_end + 1;
        }
    } else {
        const char *colon = memchr(host_start, ':', (size_t)(authority_end - host_start));

        host_end = colon ? colon : authority_end;
        if (colon) {
            port_start = colon + 1;
        }
    }
    if (host_end == host_start) {
        net_fail(err, ERROR_VALUE, "invalid URL '%s': missing host", text);
        goto fail;
    }
    out->host = net_strndup_owned(host_start, (size_t)(host_end - host_start));

    if (port_start) {
        int64_t port = 0;

        if (port_start >= authority_end) {
            net_fail(err, ERROR_VALUE, "invalid URL '%s': missing port", text);
            goto fail;
        }
        for (const char *p = port_start; p < authority_end; p++) {
            if (!isdigit((unsigned char)*p)) {
                net_fail(err, ERROR_VALUE, "invalid URL '%s': the port must be numeric", text);
                goto fail;
            }
            port = port * 10 + (*p - '0');
            if (port > 65535) {
                net_fail(err, ERROR_VALUE, "invalid URL '%s': port out of range", text);
                goto fail;
            }
        }
        out->port = port;
        out->port_known = true;
    } else if (strcmp(out->scheme, "http") == 0) {
        out->port = 80;
        out->port_known = true;
    } else if (strcmp(out->scheme, "https") == 0) {
        out->port = 443;
        out->port_known = true;
    }

    const char *rest = authority_end;
    const char *path_end = rest + strcspn(rest, "?#");

    out->path = path_end == rest ? net_strndup_owned("/", 1)
                                 : net_strndup_owned(rest, (size_t)(path_end - rest));
    rest = path_end;
    if (*rest == '?') {
        rest++;
        const char *query_end = rest + strcspn(rest, "#");

        out->query = net_strndup_owned(rest, (size_t)(query_end - rest));
        rest = query_end;
    } else {
        out->query = net_strndup_owned("", 0);
    }
    out->fragment = *rest == '#' ? net_strndup_owned(rest + 1, strlen(rest + 1))
                                 : net_strndup_owned("", 0);

    if (!out->scheme || !out->host || !out->path || !out->query || !out->fragment) {
        net_fail(err, ERROR_MEMORY, "out of memory parsing the URL");
        goto fail;
    }
    return true;

fail:
    net_url_free(out);
    return false;
}

/* A percent-encoding escape or a literal: `A-Z a-z 0-9 - _ . ~` pass through,
 * a space becomes `+` and everything else becomes `%XX` (uppercase hex). */
static bool net_url_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

static int net_hex_value(char c)
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

/* Decodes percent-escapes and `+` into `buffer` (at least `length + 1` bytes,
 * NUL-terminated on success). Fails on a truncated or invalid escape and on a
 * `%00` that would embed a null byte. */
static bool net_url_decode_into(const char *text, int length, char *buffer, int *out_length,
                                NetError *err)
{
    int written = 0;

    for (int i = 0; i < length; i++) {
        char c = text[i];

        if (c == '+') {
            buffer[written++] = ' ';
            continue;
        }
        if (c == '%') {
            if (i + 2 >= length) {
                net_fail(err, ERROR_VALUE, "invalid percent-encoding: truncated escape");
                return false;
            }
            int high = net_hex_value(text[i + 1]);
            int low = net_hex_value(text[i + 2]);

            if (high < 0 || low < 0) {
                net_fail(err, ERROR_VALUE, "invalid percent-encoding in '%.*s'",
                         length > 32 ? 32 : length, text);
                return false;
            }
            unsigned char byte = (unsigned char)((high << 4) | low);

            if (byte == 0) {
                net_fail(err, ERROR_VALUE, "percent-encoding produced a null byte");
                return false;
            }
            buffer[written++] = (char)byte;
            i += 2;
            continue;
        }
        buffer[written++] = c;
    }
    buffer[written] = '\0';
    *out_length = written;
    return true;
}

static Value native_url_parse(int arg_count, Value *args)
{
    if (arg_count != 1) {
        vm_runtime_error(ERROR_ARITY, "'url_parse' expects 1 argument, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'url_parse' expects a string, received %s",
                         net_value_type(args[0]));
        return NIL_VAL;
    }
    ObjString *text = AS_STRING(args[0]);

    if (net_string_has_null(text, "url_parse")) {
        return NIL_VAL;
    }

    NetUrl url;
    NetError err;

    if (!net_url_parse(text->chars, &url, &err)) {
        net_raise(&err);
        return NIL_VAL;
    }

    mem_pause_gc();
    ObjMap *map = object_map();

    map_set(map, OBJ_VAL(object_string("scheme", 6)),
            OBJ_VAL(object_string(url.scheme, (int)strlen(url.scheme))));
    map_set(map, OBJ_VAL(object_string("host", 4)),
            OBJ_VAL(object_string(url.host, (int)strlen(url.host))));
    map_set(map, OBJ_VAL(object_string("port", 4)),
            url.port_known ? INT_VAL(url.port) : NIL_VAL);
    map_set(map, OBJ_VAL(object_string("path", 4)),
            OBJ_VAL(object_string(url.path, (int)strlen(url.path))));
    map_set(map, OBJ_VAL(object_string("query", 5)),
            OBJ_VAL(object_string(url.query, (int)strlen(url.query))));
    map_set(map, OBJ_VAL(object_string("fragment", 8)),
            OBJ_VAL(object_string(url.fragment, (int)strlen(url.fragment))));
    mem_resume_gc();

    net_url_free(&url);
    return OBJ_VAL(map);
}

static Value native_url_encode(int arg_count, Value *args)
{
    static const char hex[] = "0123456789ABCDEF";

    if (arg_count != 1) {
        vm_runtime_error(ERROR_ARITY, "'url_encode' expects 1 argument, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'url_encode' expects a string, received %s",
                         net_value_type(args[0]));
        return NIL_VAL;
    }
    ObjString *text = AS_STRING(args[0]);
    size_t capacity = (size_t)text->length * 3 + 1;
    char *buffer = mem_realloc(NULL, 0, capacity);

    if (!buffer) {
        vm_runtime_error(ERROR_MEMORY, "out of memory in 'url_encode'");
        return NIL_VAL;
    }

    size_t written = 0;

    for (int i = 0; i < text->length; i++) {
        unsigned char c = (unsigned char)text->chars[i];

        if (net_url_unreserved(c)) {
            buffer[written++] = (char)c;
        } else if (c == ' ') {
            buffer[written++] = '+';
        } else {
            buffer[written++] = '%';
            buffer[written++] = hex[c >> 4];
            buffer[written++] = hex[c & 0x0F];
        }
    }
    ObjString *result = object_string(buffer, (int)written);

    mem_realloc(buffer, capacity, 0);
    return OBJ_VAL(result);
}

static Value native_url_decode(int arg_count, Value *args)
{
    if (arg_count != 1) {
        vm_runtime_error(ERROR_ARITY, "'url_decode' expects 1 argument, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'url_decode' expects a string, received %s",
                         net_value_type(args[0]));
        return NIL_VAL;
    }
    ObjString *text = AS_STRING(args[0]);
    char *buffer = mem_realloc(NULL, 0, (size_t)text->length + 1);

    if (!buffer) {
        vm_runtime_error(ERROR_MEMORY, "out of memory in 'url_decode'");
        return NIL_VAL;
    }

    NetError err;
    int length = 0;

    if (!net_url_decode_into(text->chars, text->length, buffer, &length, &err)) {
        mem_realloc(buffer, (size_t)text->length + 1, 0);
        net_raise(&err);
        return NIL_VAL;
    }
    ObjString *result = object_string(buffer, length);

    mem_realloc(buffer, (size_t)text->length + 1, 0);
    return OBJ_VAL(result);
}

/* Splits an application/x-www-form-urlencoded query into a map from key to an
 * array of values, so repeated keys keep every occurrence. A leading `?` is
 * accepted; empty pairs ("a=1&&b=2") are ignored. */
static Value native_parse_query(int arg_count, Value *args)
{
    if (arg_count != 1) {
        vm_runtime_error(ERROR_ARITY, "'parse_query' expects 1 argument, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'parse_query' expects a string, received %s",
                         net_value_type(args[0]));
        return NIL_VAL;
    }
    ObjString *text = AS_STRING(args[0]);
    const char *cursor = text->chars;
    int remaining = text->length;

    if (remaining > 0 && cursor[0] == '?') {
        cursor++;
        remaining--;
    }

    NetError err;
    bool ok = true;

    mem_pause_gc();
    ObjMap *map = object_map();

    const char *end = cursor + remaining;

    while (cursor <= end && ok) {
        const char *ampersand = memchr(cursor, '&', (size_t)(end - cursor));
        const char *pair_end = ampersand ? ampersand : end;

        if (pair_end > cursor) {
            const char *equals = memchr(cursor, '=', (size_t)(pair_end - cursor));
            const char *key_end = equals ? equals : pair_end;
            const char *value_start = equals ? equals + 1 : pair_end;
            int key_raw = (int)(key_end - cursor);
            int value_raw = (int)(pair_end - value_start);
            char *key = mem_realloc(NULL, 0, (size_t)key_raw + 1);
            char *value = mem_realloc(NULL, 0, (size_t)value_raw + 1);
            int key_length = 0;
            int value_length = 0;

            if (!key || !value ||
                !net_url_decode_into(cursor, key_raw, key, &key_length, &err) ||
                !net_url_decode_into(value_start, value_raw, value, &value_length, &err)) {
                mem_realloc(key, (size_t)key_raw + 1, 0);
                mem_realloc(value, (size_t)value_raw + 1, 0);
                ok = false;
                break;
            }

            ObjString *key_string = object_string(key, key_length);
            Value values;

            if (!map_get(map, OBJ_VAL(key_string), &values)) {
                ObjArray *array = object_array();

                array_push(array, OBJ_VAL(object_string(value, value_length)));
                map_set(map, OBJ_VAL(key_string), OBJ_VAL(array));
            } else {
                array_push(AS_ARRAY(values), OBJ_VAL(object_string(value, value_length)));
            }
            mem_realloc(key, (size_t)key_raw + 1, 0);
            mem_realloc(value, (size_t)value_raw + 1, 0);
        }
        if (!ampersand) {
            break;
        }
        cursor = ampersand + 1;
    }
    mem_resume_gc();

    if (!ok) {
        net_raise(&err);
        return NIL_VAL;
    }
    return OBJ_VAL(map);
}

/* ------------------------------------------------------------------ */
/* HTTP/1.1 client (phase 17)                                          */
/* ------------------------------------------------------------------ */

/* A growable byte buffer used to build requests and bodies. */
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} NetBuffer;

static void net_buffer_init(NetBuffer *buffer)
{
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

static void net_buffer_free(NetBuffer *buffer)
{
    mem_realloc(buffer->data, buffer->capacity, 0);
    net_buffer_init(buffer);
}

static void net_buffer_reserve(NetBuffer *buffer, size_t extra)
{
    if (buffer->length + extra <= buffer->capacity) {
        return;
    }
    size_t capacity = buffer->capacity ? buffer->capacity : 256;

    while (capacity < buffer->length + extra) {
        capacity *= 2;
    }
    buffer->data = mem_realloc(buffer->data, buffer->capacity, capacity);
    buffer->capacity = capacity;
}

static void net_buffer_append(NetBuffer *buffer, const char *data, size_t length)
{
    net_buffer_reserve(buffer, length);
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
}

static void net_buffer_puts(NetBuffer *buffer, const char *text)
{
    net_buffer_append(buffer, text, strlen(text));
}

/* Appends a terminator without counting it in `length`, for buffers used as C
 * strings. */
static void net_buffer_terminate(NetBuffer *buffer)
{
    net_buffer_reserve(buffer, 1);
    buffer->data[buffer->length] = '\0';
}

static void net_buffer_printf(NetBuffer *buffer, const char *format, ...)
{
    va_list ap;
    va_list copy;

    va_start(ap, format);
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, format, copy);

    va_end(copy);
    if (needed < 0) {
        va_end(ap);
        return;
    }
    net_buffer_reserve(buffer, (size_t)needed + 1);
    vsnprintf(buffer->data + buffer->length, (size_t)needed + 1, format, ap);
    buffer->length += (size_t)needed;
    va_end(ap);
}

/* A buffered reader over one response. Bytes are read from the descriptor on
 * demand and kept until consumed, so header parsing never over-reads past the
 * body it is about to return. */
typedef struct {
    int fd;
    void *ssl; /* SSL * when the response arrives over TLS, NULL otherwise */
    int64_t deadline;
    char *data;
    size_t length;
    size_t position;
    size_t capacity;
} NetHttpReader;

static void net_http_reader_init(NetHttpReader *reader, int fd, void *ssl, int64_t deadline)
{
    reader->fd = fd;
    reader->ssl = ssl;
    reader->deadline = deadline;
    reader->data = NULL;
    reader->length = 0;
    reader->position = 0;
    reader->capacity = 0;
}

static void net_http_reader_free(NetHttpReader *reader)
{
    mem_realloc(reader->data, reader->capacity, 0);
    net_http_reader_init(reader, -1, NULL, 0);
}

/* Refills the buffer, growing it up to `limit`. Returns 1 on data, 0 at end of
 * stream and -1 with `err` filled (including a timeout). Fresh bytes are
 * appended after anything already buffered, so a long line can span reads. */
static int net_http_fill(NetHttpReader *reader, size_t limit, NetError *err)
{
    if (reader->position == reader->length) {
        reader->position = reader->length = 0; /* everything consumed */
    }
    if (reader->length == reader->capacity) {
        if (reader->capacity >= limit) {
            net_fail(err, ERROR_NETWORK, "an HTTP response line exceeds the maximum of %zu bytes",
                     limit);
            return -1;
        }
        size_t capacity = reader->capacity ? reader->capacity * 2 : 8192;

        if (capacity > limit) {
            capacity = limit;
        }
        reader->data = mem_realloc(reader->data, reader->capacity, capacity);
        reader->capacity = capacity;
    }

    if (reader->ssl) {
        ssize_t received = net_tls_read_blocking((SSL *)reader->ssl, reader->fd,
                                                 reader->data + reader->length,
                                                 reader->capacity - reader->length,
                                                 reader->deadline, err);

        if (received > 0) {
            reader->length += (size_t)received;
            return 1;
        }
        return received == 0 ? 0 : -1;
    }

    for (;;) {
        int ready = net_wait(reader->fd, POLLIN, reader->deadline);

        if (ready == 0) {
            net_fail(err, ERROR_TIMEOUT, "the HTTP request timed out");
            return -1;
        }
        if (ready < 0) {
            net_fail_errno(err, errno, "recv failed");
            return -1;
        }

        ssize_t received = recv(reader->fd, reader->data + reader->length,
                                reader->capacity - reader->length, 0);

        if (received > 0) {
            reader->length += (size_t)received;
            return 1;
        }
        if (received == 0) {
            return 0; /* end of stream */
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        net_fail_errno(err, errno, "recv failed");
        return -1;
    }
}

/* Reads one line up to `\n`, dropping the newline and a preceding `\r`. The
 * line is malloc'd into `*out` and NUL-terminated. Returns 0 on success, 1 at
 * an immediate end of stream and -1 with `err` filled. */
static int net_http_read_line(NetHttpReader *reader, size_t limit, char **out, size_t *out_length,
                              NetError *err)
{
    for (;;) {
        if (reader->position < reader->length) {
            char *newline = memchr(reader->data + reader->position, '\n',
                                   reader->length - reader->position);

            if (newline) {
                size_t length = (size_t)(newline - (reader->data + reader->position));

                if (length > 0 && reader->data[reader->position + length - 1] == '\r') {
                    length--;
                }
                char *line = mem_realloc(NULL, 0, length + 1);

                memcpy(line, reader->data + reader->position, length);
                line[length] = '\0';
                reader->position = (size_t)(newline - reader->data) + 1;
                *out = line;
                *out_length = length;
                return 0;
            }
            if (reader->length - reader->position >= limit) {
                net_fail(err, ERROR_NETWORK, "an HTTP response line exceeds the maximum of %zu bytes",
                         limit);
                return -1;
            }
        }

        int status = net_http_fill(reader, limit, err);

        if (status <= 0) {
            return status;
        }
    }
}

/* Copies exactly `count` bytes out of the reader. Returns 0 on success, 1 on a
 * premature end of stream and -1 with `err` filled. */
static int net_http_read_exact(NetHttpReader *reader, size_t count, char *destination, NetError *err)
{
    size_t copied = 0;

    while (copied < count) {
        if (reader->position < reader->length) {
            size_t take = reader->length - reader->position;

            if (take > count - copied) {
                take = count - copied;
            }
            memcpy(destination + copied, reader->data + reader->position, take);
            reader->position += take;
            copied += take;
            continue;
        }

        int status = net_http_fill(reader, NET_HTTP_HEADER_MAX, err);

        if (status == 0) {
            return 1;
        }
        if (status < 0) {
            return -1;
        }
    }
    return 0;
}

typedef struct {
    char *name;
    char *value;
} NetHttpHeader;

typedef struct {
    int status;
    NetHttpHeader *headers;
    int header_count;
    int header_capacity;
    char *body;
    size_t body_length;
    size_t body_capacity;
} NetHttpResponse;

static void net_http_response_free(NetHttpResponse *response)
{
    for (int i = 0; i < response->header_count; i++) {
        mem_realloc(response->headers[i].name, strlen(response->headers[i].name) + 1, 0);
        mem_realloc(response->headers[i].value, strlen(response->headers[i].value) + 1, 0);
    }
    mem_realloc(response->headers, (size_t)response->header_capacity * sizeof(NetHttpHeader), 0);
    mem_realloc(response->body, response->body_capacity, 0);
    memset(response, 0, sizeof(*response));
}

static void net_http_response_add_header(NetHttpResponse *response, char *name, char *value)
{
    if (response->header_count == response->header_capacity) {
        int capacity = response->header_capacity ? response->header_capacity * 2 : 8;

        response->headers = mem_realloc(response->headers,
                                        (size_t)response->header_capacity * sizeof(NetHttpHeader),
                                        (size_t)capacity * sizeof(NetHttpHeader));
        response->header_capacity = capacity;
    }
    response->headers[response->header_count].name = name;
    response->headers[response->header_count].value = value;
    response->header_count++;
}

/* First header with `name` (lowercase), or NULL. */
static const char *net_http_response_header(const NetHttpResponse *response, const char *name)
{
    for (int i = 0; i < response->header_count; i++) {
        if (strcmp(response->headers[i].name, name) == 0) {
            return response->headers[i].value;
        }
    }
    return NULL;
}

static bool net_http_user_has_header(ObjMap *headers, const char *name)
{
    if (!headers) {
        return false;
    }
    for (int i = 0; i < headers->capacity; i++) {
        MapEntry *entry = &headers->entries[i];

        if (entry->state == 1 && is_obj_type(entry->key, OBJ_STRING) &&
            strcasecmp(AS_STRING(entry->key)->chars, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool net_http_default_port(const NetUrl *url)
{
    return (strcmp(url->scheme, "http") == 0 && url->port == 80) ||
           (strcmp(url->scheme, "https") == 0 && url->port == 443);
}

static void net_http_build_request(NetBuffer *out, const char *method, const char *target,
                                   const NetUrl *url, ObjMap *headers, const char *body,
                                   size_t body_length, bool has_body)
{
    net_buffer_printf(out, "%s %s HTTP/1.1\r\n", method, target);

    if (!net_http_user_has_header(headers, "host")) {
        if (url->port_known && !net_http_default_port(url)) {
            net_buffer_printf(out, "Host: %s:%lld\r\n", url->host, (long long)url->port);
        } else {
            net_buffer_printf(out, "Host: %s\r\n", url->host);
        }
    }
    if (!net_http_user_has_header(headers, "user-agent")) {
        net_buffer_puts(out, "User-Agent: " NET_HTTP_USER_AGENT "\r\n");
    }
    if (!net_http_user_has_header(headers, "accept")) {
        net_buffer_puts(out, "Accept: */*\r\n");
    }
    if (!net_http_user_has_header(headers, "connection")) {
        net_buffer_puts(out, "Connection: close\r\n");
    }

    if (headers) {
        for (int i = 0; i < headers->capacity; i++) {
            MapEntry *entry = &headers->entries[i];

            if (entry->state != 1 || !is_obj_type(entry->key, OBJ_STRING) ||
                !is_obj_type(entry->value, OBJ_STRING)) {
                continue;
            }
            /* The client controls the framing headers. */
            if (strcasecmp(AS_STRING(entry->key)->chars, "content-length") == 0) {
                continue;
            }
            net_buffer_printf(out, "%s: %s\r\n", AS_STRING(entry->key)->chars,
                              AS_STRING(entry->value)->chars);
        }
    }

    if (has_body) {
        net_buffer_printf(out, "Content-Length: %zu\r\n", body_length);
        if (body_length > 0 && !net_http_user_has_header(headers, "content-type")) {
            net_buffer_puts(out, "Content-Type: application/octet-stream\r\n");
        }
    }
    net_buffer_puts(out, "\r\n");

    if (has_body && body_length > 0) {
        net_buffer_append(out, body, body_length);
    }
}

static bool net_http_send_all(int fd, void *ssl, const char *data, size_t length,
                              int64_t deadline, NetError *err)
{
    size_t sent = 0;

    if (ssl) {
        while (sent < length) {
            size_t written = 0;

            ERR_clear_error();
            errno = 0;
            int status = SSL_write_ex((SSL *)ssl, data + sent, length - sent, &written);

            if (status == 1) {
                if (written == 0) {
                    net_fail(err, ERROR_NETWORK, "TLS write failed: no progress");
                    return false;
                }
                sent += written;
                continue;
            }

            int error = SSL_get_error((SSL *)ssl, status);

            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                int ready = net_wait(fd, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline);

                if (ready == 0) {
                    net_fail(err, ERROR_TIMEOUT, "the HTTP request timed out");
                    return false;
                }
                if (ready < 0) {
                    net_fail_errno(err, errno, "send failed");
                    return false;
                }
                continue;
            }
            net_tls_error(err, "write", error);
            return false;
        }
        return true;
    }

    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, MSG_NOSIGNAL);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int ready = net_wait(fd, POLLOUT, deadline);

            if (ready == 0) {
                net_fail(err, ERROR_TIMEOUT, "the HTTP request timed out");
                return false;
            }
            if (ready < 0) {
                net_fail_errno(err, errno, "send failed");
                return false;
            }
            continue;
        }
        net_fail_errno(err, errno, "send failed");
        return false;
    }
    return true;
}

static bool net_http_read_status(NetHttpReader *reader, NetHttpResponse *response, NetError *err)
{
    char *line = NULL;
    size_t length = 0;
    int status = net_http_read_line(reader, NET_HTTP_HEADER_MAX, &line, &length, err);

    if (status != 0) {
        if (status == 0) {
            net_fail(err, ERROR_NETWORK, "the HTTP response ended before the status line");
        }
        return false;
    }
    if (length < 12 || strncmp(line, "HTTP/1.", 7) != 0 || !isdigit((unsigned char)line[7]) ||
        line[8] != ' ' || !isdigit((unsigned char)line[9]) || !isdigit((unsigned char)line[10]) ||
        !isdigit((unsigned char)line[11])) {
        net_fail(err, ERROR_NETWORK, "malformed HTTP status line '%s'", line);
        mem_realloc(line, length + 1, 0);
        return false;
    }
    response->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    mem_realloc(line, length + 1, 0);

    if (response->status < 100 || response->status > 599) {
        net_fail(err, ERROR_NETWORK, "invalid HTTP status code %d", response->status);
        return false;
    }
    return true;
}

static bool net_http_read_headers(NetHttpReader *reader, NetHttpResponse *response, NetError *err)
{
    size_t total = 0;

    for (;;) {
        char *line = NULL;
        size_t length = 0;
        int status = net_http_read_line(reader, NET_HTTP_HEADER_MAX, &line, &length, err);

        if (status != 0) {
            if (status == 0) {
                net_fail(err, ERROR_NETWORK, "the HTTP response ended inside the headers");
            }
            return false;
        }
        total += length + 2;
        if (total > NET_HTTP_HEADER_MAX) {
            mem_realloc(line, length + 1, 0);
            net_fail(err, ERROR_NETWORK, "the HTTP response headers exceed the maximum of %d bytes",
                     NET_HTTP_HEADER_MAX);
            return false;
        }
        if (length == 0) {
            mem_realloc(line, length + 1, 0);
            return true;
        }

        char *colon = memchr(line, ':', length);

        if (!colon || colon == line) {
            net_fail(err, ERROR_NETWORK, "malformed HTTP header '%s'", line);
            mem_realloc(line, length + 1, 0);
            return false;
        }
        /* Trim optional whitespace around the name and the value (obs-fold is
         * not supported). */
        char *name_end = colon;

        while (name_end > line && (name_end[-1] == ' ' || name_end[-1] == '\t')) {
            name_end--;
        }
        char *value = colon + 1;
        char *value_end = line + length;

        while (value < value_end && (*value == ' ' || *value == '\t')) {
            value++;
        }
        while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) {
            value_end--;
        }

        size_t name_length = (size_t)(name_end - line);
        size_t value_length = (size_t)(value_end - value);
        char *name = mem_realloc(NULL, 0, name_length + 1);
        char *value_copy = mem_realloc(NULL, 0, value_length + 1);

        memcpy(name, line, name_length);
        name[name_length] = '\0';
        for (size_t i = 0; i < name_length; i++) {
            name[i] = (char)tolower((unsigned char)name[i]);
        }
        memcpy(value_copy, value, value_length);
        value_copy[value_length] = '\0';
        mem_realloc(line, length + 1, 0);

        if (response->header_count >= NET_HTTP_MAX_HEADERS) {
            mem_realloc(name, name_length + 1, 0);
            mem_realloc(value_copy, value_length + 1, 0);
            net_fail(err, ERROR_NETWORK, "the HTTP response has too many headers (maximum %d)",
                     NET_HTTP_MAX_HEADERS);
            return false;
        }
        net_http_response_add_header(response, name, value_copy);
    }
}

static bool net_http_read_chunked(NetHttpReader *reader, int64_t max_body, NetHttpResponse *response,
                                  NetError *err)
{
    NetBuffer body;

    net_buffer_init(&body);
    for (;;) {
        char *line = NULL;
        size_t length = 0;
        int line_status = net_http_read_line(reader, NET_HTTP_HEADER_MAX, &line, &length, err);

        if (line_status != 0) {
            if (line_status == 0) {
                net_fail(err, ERROR_NETWORK, "the chunked body ended before its final chunk");
            }
            goto fail;
        }

        int64_t size = 0;
        const char *cursor = line;
        bool digits = false;

        while (*cursor && *cursor != ';') {
            int digit = net_hex_value(*cursor);

            if (digit < 0) {
                net_fail(err, ERROR_NETWORK, "malformed chunk size '%s'", line);
                mem_realloc(line, length + 1, 0);
                goto fail;
            }
            size = size * 16 + digit;
            digits = true;
            if (size > max_body || size > INT32_MAX) {
                net_fail(err, ERROR_NETWORK, "the HTTP response body exceeds the maximum of %lld bytes",
                         (long long)max_body);
                mem_realloc(line, length + 1, 0);
                goto fail;
            }
            cursor++;
        }
        mem_realloc(line, length + 1, 0);
        if (!digits) {
            net_fail(err, ERROR_NETWORK, "malformed chunk size");
            goto fail;
        }
        if (size == 0) {
            break; /* last chunk; the trailer follows */
        }
        if ((int64_t)body.length + size > max_body) {
            net_fail(err, ERROR_NETWORK, "the HTTP response body exceeds the maximum of %lld bytes",
                     (long long)max_body);
            goto fail;
        }

        net_buffer_reserve(&body, (size_t)size);
        int chunk_status = net_http_read_exact(reader, (size_t)size, body.data + body.length, err);

        if (chunk_status == 1) {
            net_fail(err, ERROR_NETWORK, "the chunked body ended in the middle of a chunk");
        }
        if (chunk_status != 0) {
            goto fail;
        }
        body.length += (size_t)size;

        char *trailer = NULL;
        size_t trailer_length = 0;
        int trailer_status = net_http_read_line(reader, 8, &trailer, &trailer_length, err);

        if (trailer_status != 0 || trailer_length != 0) {
            if (trailer_status == 0) {
                net_fail(err, ERROR_NETWORK, "missing the end of a chunk");
            }
            mem_realloc(trailer, trailer_length + 1, 0);
            goto fail;
        }
        mem_realloc(trailer, trailer_length + 1, 0);
    }

    /* Trailer headers, terminated by an empty line. */
    for (;;) {
        char *line = NULL;
        size_t length = 0;
        int line_status = net_http_read_line(reader, NET_HTTP_HEADER_MAX, &line, &length, err);

        if (line_status != 0) {
            if (line_status == 0) {
                net_fail(err, ERROR_NETWORK, "the chunked trailer ended unexpectedly");
            }
            goto fail;
        }
        mem_realloc(line, length + 1, 0);
        if (length == 0) {
            break;
        }
    }

    response->body = body.data;
    response->body_length = body.length;
    response->body_capacity = body.capacity;
    return true;

fail:
    net_buffer_free(&body);
    return false;
}

static bool net_http_read_to_eof(NetHttpReader *reader, int64_t max_body, NetHttpResponse *response,
                                 NetError *err)
{
    NetBuffer body;

    net_buffer_init(&body);
    for (;;) {
        if (reader->position < reader->length) {
            size_t take = reader->length - reader->position;

            if ((int64_t)body.length + (int64_t)take > max_body) {
                net_fail(err, ERROR_NETWORK, "the HTTP response body exceeds the maximum of %lld bytes",
                         (long long)max_body);
                goto fail;
            }
            net_buffer_append(&body, reader->data + reader->position, take);
            reader->position = reader->length;
            continue;
        }

        int status = net_http_fill(reader, NET_HTTP_HEADER_MAX, err);

        if (status == 0) {
            break;
        }
        if (status < 0) {
            goto fail;
        }
    }

    response->body = body.data;
    response->body_length = body.length;
    response->body_capacity = body.capacity;
    return true;

fail:
    net_buffer_free(&body);
    return false;
}

/* Resolves a Location header against `base`. Absolute, scheme-relative and
 * root-relative targets are supported, plus simple relative paths. The caller
 * releases the result. */
static char *net_http_resolve_location(const NetUrl *base, const char *location)
{
    NetBuffer out;

    net_buffer_init(&out);
    if (strncmp(location, "http://", 7) == 0 || strncmp(location, "https://", 8) == 0) {
        net_buffer_puts(&out, location);
        return net_strndup_owned(out.data, out.length);
    }
    net_buffer_puts(&out, base->scheme);
    net_buffer_puts(&out, "://");
    net_buffer_puts(&out, base->host);
    if (base->port_known) {
        net_buffer_printf(&out, ":%lld", (long long)base->port);
    }
    if (location[0] == '/') {
        net_buffer_puts(&out, location);
        char *result = net_strndup_owned(out.data, out.length);

        net_buffer_free(&out);
        return result;
    }
    /* Relative to the directory of the base path. */
    const char *slash = strrchr(base->path, '/');

    if (slash) {
        net_buffer_append(&out, base->path, (size_t)(slash - base->path) + 1);
    } else {
        net_buffer_puts(&out, "/");
    }
    net_buffer_puts(&out, location);
    char *result = net_strndup_owned(out.data, out.length);

    net_buffer_free(&out);
    return result;
}

static bool net_http_read_body(NetHttpReader *reader, NetHttpResponse *response, int64_t max_body,
                               NetError *err)
{
    const char *transfer = net_http_response_header(response, "transfer-encoding");
    const char *length_header = net_http_response_header(response, "content-length");

    if (transfer && strstr(transfer, "chunked")) {
        return net_http_read_chunked(reader, max_body, response, err);
    }
    if (length_header) {
        char *end = NULL;
        long long declared = strtoll(length_header, &end, 10);

        if (end == length_header || *end != '\0' || declared < 0) {
            net_fail(err, ERROR_NETWORK, "invalid Content-Length '%s'", length_header);
            return false;
        }
        if (declared > max_body) {
            net_fail(err, ERROR_NETWORK, "the HTTP response body exceeds the maximum of %lld bytes",
                     (long long)max_body);
            return false;
        }
        if (declared == 0) {
            return true;
        }
        response->body = mem_realloc(NULL, 0, (size_t)declared);
        response->body_capacity = (size_t)declared;
        int status = net_http_read_exact(reader, (size_t)declared, response->body, err);

        if (status == 1) {
            net_fail(err, ERROR_NETWORK, "the HTTP response ended before the body was complete");
        }
        if (status != 0) {
            return false;
        }
        response->body_length = (size_t)declared;
        return true;
    }
    return net_http_read_to_eof(reader, max_body, response, err);
}

/* Performs one request (plus redirects), filling `response`. `deadline` is an
 * absolute monotonic millisecond bound shared by the whole chain (0 = none). */
static bool net_http_perform(const char *url_text, const char *method, ObjMap *headers,
                             const char *body, size_t body_length, bool has_body, int64_t deadline,
                             bool follow, int64_t max_body, const char *ca_file, int redirects_left,
                             NetHttpResponse *response, NetError *err)
{
    NetUrl url;

    if (!net_url_parse(url_text, &url, err)) {
        return false;
    }

    bool secure = strcmp(url.scheme, "https") == 0;

    if (!secure && strcmp(url.scheme, "http") != 0) {
        net_fail(err, ERROR_NETWORK, "unsupported URL scheme '%s'", url.scheme);
        net_url_free(&url);
        return false;
    }
    if (!url.port_known) {
        net_fail(err, ERROR_NETWORK, "invalid URL '%s': missing port", url_text);
        net_url_free(&url);
        return false;
    }

    NetBuffer target;
    NetBuffer request;
    NetHttpReader reader;
    bool ok = false;
    int fd = -1;
    SSL_CTX *tls_context = NULL;
    SSL *tls = NULL;

    net_buffer_init(&target);
    net_buffer_init(&request);
    net_buffer_puts(&target, url.path);
    if (url.query[0] != '\0') {
        net_buffer_puts(&target, "?");
        net_buffer_puts(&target, url.query);
    }
    net_buffer_terminate(&target);
    net_http_build_request(&request, method, target.data, &url, headers, body, body_length,
                           has_body);

    fd = net_connect(url.host, url.port, deadline, err);
    if (fd < 0) {
        goto cleanup;
    }
    if (secure) {
        tls_context = net_tls_context(ca_file, err);
        if (!tls_context) {
            goto cleanup;
        }
        tls = net_tls_client(tls_context, url.host, fd, err);
        if (!tls) {
            goto cleanup;
        }
        if (!net_tls_handshake_blocking(tls, fd, deadline, err)) {
            goto cleanup;
        }
    }
    if (!net_http_send_all(fd, tls, request.data, request.length, deadline, err)) {
        goto cleanup;
    }

    net_http_reader_init(&reader, fd, tls, deadline);
    if (!net_http_read_status(&reader, response, err)) {
        net_http_reader_free(&reader);
        goto cleanup;
    }
    if (!net_http_read_headers(&reader, response, err)) {
        net_http_reader_free(&reader);
        goto cleanup;
    }
    if (!net_http_read_body(&reader, response, max_body, err)) {
        net_http_reader_free(&reader);
        goto cleanup;
    }
    net_http_reader_free(&reader);
    ok = true;

cleanup:
    if (tls) {
        SSL_shutdown(tls);
        ERR_clear_error();
        SSL_free(tls);
    }
    if (tls_context) {
        SSL_CTX_free(tls_context);
    }
    if (fd >= 0) {
        close(fd);
    }
    net_buffer_free(&target);
    net_buffer_free(&request);

    if (ok && follow) {
        const char *location = net_http_response_header(response, "location");
        bool redirect = response->status == 301 || response->status == 302 ||
                        response->status == 303 || response->status == 307 ||
                        response->status == 308;

        if (redirect && location && *location != '\0') {
            if (redirects_left <= 0) {
                net_http_response_free(response);
                net_url_free(&url);
                net_fail(err, ERROR_NETWORK, "too many redirects (maximum %d)", NET_HTTP_REDIRECT_MAX);
                return false;
            }
            char *next = net_http_resolve_location(&url, location);
            const char *next_method = method;
            const char *next_body = body;
            size_t next_length = body_length;
            bool next_has_body = has_body;

            if (response->status == 303 ||
                ((response->status == 301 || response->status == 302) &&
                 strcmp(method, "POST") == 0)) {
                next_method = "GET";
                next_body = NULL;
                next_length = 0;
                next_has_body = false;
            }
            net_http_response_free(response);
            net_url_free(&url);
            ok = net_http_perform(next, next_method, headers, next_body, next_length,
                                  next_has_body, deadline, follow, max_body, ca_file,
                                  redirects_left - 1, response, err);
            mem_realloc(next, strlen(next) + 1, 0);
            return ok;
        }
    }
    net_url_free(&url);
    return ok;
}

/* Rejects CR/LF so a value cannot inject an extra header or request line. */
static bool net_http_safe_text(ObjString *text, const char *function, const char *what)
{
    if (memchr(text->chars, '\r', (size_t)text->length) ||
        memchr(text->chars, '\n', (size_t)text->length)) {
        vm_runtime_error(ERROR_VALUE, "'%s' %s cannot contain a newline", function, what);
        return false;
    }
    return true;
}

static bool net_http_validate_headers(ObjMap *headers, const char *function)
{
    for (int i = 0; i < headers->capacity; i++) {
        MapEntry *entry = &headers->entries[i];

        if (entry->state != 1) {
            continue;
        }
        if (!is_obj_type(entry->key, OBJ_STRING)) {
            vm_runtime_error(ERROR_TYPE, "'%s' header names must be strings, received %s", function,
                             net_value_type(entry->key));
            return false;
        }
        if (!is_obj_type(entry->value, OBJ_STRING)) {
            vm_runtime_error(ERROR_TYPE, "'%s' header '%s' must be a string, received %s", function,
                             AS_STRING(entry->key)->chars, net_value_type(entry->value));
            return false;
        }
        if (!net_http_safe_text(AS_STRING(entry->key), function, "header name") ||
            !net_http_safe_text(AS_STRING(entry->value), function, "header value")) {
            return false;
        }
    }
    return true;
}

static Value net_http_execute(const char *function, const char *method, ObjString *url,
                              Value body_value, ObjMap *headers, int64_t timeout, bool follow,
                              int64_t max_body, const char *ca_file)
{
    if (net_string_has_null(url, function) || !net_http_safe_text(url, function, "URL")) {
        return NIL_VAL;
    }
    const char *body = NULL;
    size_t body_length = 0;
    bool has_body = false;

    if (!is_nil(body_value)) {
        if (!is_obj_type(body_value, OBJ_STRING)) {
            vm_runtime_error(ERROR_TYPE, "'%s' expects a string body, received %s", function,
                             net_value_type(body_value));
            return NIL_VAL;
        }
        body = AS_STRING(body_value)->chars;
        body_length = (size_t)AS_STRING(body_value)->length;
        has_body = true;
    }
    if (headers && !net_http_validate_headers(headers, function)) {
        return NIL_VAL;
    }

    NetHttpResponse response;
    NetError err;

    memset(&response, 0, sizeof(response));
    int64_t deadline = net_now_ms() + timeout;

    if (!net_http_perform(url->chars, method, headers, body, body_length, has_body, deadline, follow,
                          max_body, ca_file, NET_HTTP_REDIRECT_MAX, &response, &err)) {
        net_http_response_free(&response);
        net_raise(&err);
        return NIL_VAL;
    }

    mem_pause_gc();
    ObjMap *result = object_map();
    ObjMap *header_map = object_map();

    for (int i = 0; i < response.header_count; i++) {
        map_set(header_map,
                OBJ_VAL(object_string(response.headers[i].name,
                                      (int)strlen(response.headers[i].name))),
                OBJ_VAL(object_string(response.headers[i].value,
                                      (int)strlen(response.headers[i].value))));
    }
    map_set(result, OBJ_VAL(object_string("status", 6)), INT_VAL(response.status));
    map_set(result, OBJ_VAL(object_string("headers", 7)), OBJ_VAL(header_map));
    map_set(result, OBJ_VAL(object_string("body", 4)),
            OBJ_VAL(object_string(response.body ? response.body : "", (int)response.body_length)));
    mem_resume_gc();

    net_http_response_free(&response);
    return OBJ_VAL(result);
}

/* http_get/post/put/delete(url, body?, headers?, timeout_ms?, follow_redirects?,
 * max_body?) */
static Value net_http_positional(const char *function, const char *method, int arg_count,
                                 Value *args)
{
    if (arg_count < 1 || arg_count > 6) {
        vm_runtime_error(ERROR_ARITY, "'%s' expects between 1 and 6 arguments, received %d", function,
                         arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'%s' expects a string URL, received %s", function,
                         net_value_type(args[0]));
        return NIL_VAL;
    }
    if (net_string_has_null(AS_STRING(args[0]), function)) {
        return NIL_VAL;
    }

    Value body = arg_count > 1 ? args[1] : NIL_VAL;
    ObjMap *headers = NULL;

    if (arg_count > 2 && !is_nil(args[2])) {
        if (!is_obj_type(args[2], OBJ_MAP)) {
            vm_runtime_error(ERROR_TYPE, "'%s' expects a map of headers, received %s", function,
                             net_value_type(args[2]));
            return NIL_VAL;
        }
        headers = AS_MAP(args[2]);
    }

    int64_t timeout = NET_HTTP_TIMEOUT_DEFAULT;

    if (arg_count > 3 && !is_nil(args[3])) {
        if (!is_int(args[3])) {
            vm_runtime_error(ERROR_TYPE, "'%s' expects an integer timeout_ms, received %s", function,
                             net_value_type(args[3]));
            return NIL_VAL;
        }
        if (AS_INT(args[3]) < 0) {
            vm_runtime_error(ERROR_VALUE, "'%s' timeout cannot be negative", function);
            return NIL_VAL;
        }
        if (AS_INT(args[3]) > 0) {
            timeout = AS_INT(args[3]);
        }
    }

    bool follow = true;

    if (arg_count > 4 && !is_nil(args[4])) {
        if (!is_bool(args[4])) {
            vm_runtime_error(ERROR_TYPE, "'%s' expects a bool follow_redirects, received %s",
                             function, net_value_type(args[4]));
            return NIL_VAL;
        }
        follow = AS_BOOL(args[4]);
    }

    int64_t max_body = NET_HTTP_BODY_DEFAULT_MAX;

    if (arg_count > 5 && !is_nil(args[5])) {
        if (!is_int(args[5])) {
            vm_runtime_error(ERROR_TYPE, "'%s' expects an integer max_body, received %s", function,
                             net_value_type(args[5]));
            return NIL_VAL;
        }
        if (AS_INT(args[5]) <= 0 || AS_INT(args[5]) > NET_READ_MAX) {
            vm_runtime_error(ERROR_VALUE, "'%s' max_body must be between 1 and %d", function,
                             NET_READ_MAX);
            return NIL_VAL;
        }
        max_body = AS_INT(args[5]);
    }

    return net_http_execute(function, method, AS_STRING(args[0]), body, headers, timeout, follow,
                            max_body, NULL);
}

static Value native_http_get(int arg_count, Value *args)
{
    return net_http_positional("http_get", "GET", arg_count, args);
}

static Value native_http_post(int arg_count, Value *args)
{
    return net_http_positional("http_post", "POST", arg_count, args);
}

static Value native_http_put(int arg_count, Value *args)
{
    return net_http_positional("http_put", "PUT", arg_count, args);
}

static Value native_http_delete(int arg_count, Value *args)
{
    return net_http_positional("http_delete", "DELETE", arg_count, args);
}

/* http_request({url, method?, body?, headers?, timeout_ms?, follow_redirects?,
 * max_body?, ca_file?}) */
static Value native_http_request(int arg_count, Value *args)
{
    if (arg_count != 1) {
        vm_runtime_error(ERROR_ARITY, "'http_request' expects 1 argument, received %d", arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_MAP)) {
        vm_runtime_error(ERROR_TYPE, "'http_request' expects a map of options, received %s",
                         net_value_type(args[0]));
        return NIL_VAL;
    }

    ObjMap *options = AS_MAP(args[0]);
    ObjString *url = NULL;
    const char *method = "GET";
    ObjString *method_string = NULL;
    Value body = NIL_VAL;
    ObjMap *headers = NULL;
    int64_t timeout = NET_HTTP_TIMEOUT_DEFAULT;
    bool follow = true;
    int64_t max_body = NET_HTTP_BODY_DEFAULT_MAX;
    const char *ca_file = NULL;

    for (int i = 0; i < options->capacity; i++) {
        MapEntry *entry = &options->entries[i];

        if (entry->state != 1) {
            continue;
        }
        if (!is_obj_type(entry->key, OBJ_STRING)) {
            vm_runtime_error(ERROR_TYPE, "'http_request' option names must be strings, received %s",
                             net_value_type(entry->key));
            return NIL_VAL;
        }
        const char *name = AS_STRING(entry->key)->chars;
        Value value = entry->value;

        if (strcmp(name, "url") == 0) {
            if (!is_obj_type(value, OBJ_STRING)) {
                vm_runtime_error(ERROR_TYPE, "'http_request' url must be a string, received %s",
                                 net_value_type(value));
                return NIL_VAL;
            }
            url = AS_STRING(value);
        } else if (strcmp(name, "method") == 0) {
            if (!is_obj_type(value, OBJ_STRING)) {
                vm_runtime_error(ERROR_TYPE, "'http_request' method must be a string, received %s",
                                 net_value_type(value));
                return NIL_VAL;
            }
            method_string = AS_STRING(value);
            method = method_string->chars;
        } else if (strcmp(name, "body") == 0) {
            body = value;
        } else if (strcmp(name, "headers") == 0) {
            if (!is_nil(value)) {
                if (!is_obj_type(value, OBJ_MAP)) {
                    vm_runtime_error(ERROR_TYPE,
                                     "'http_request' headers must be a map, received %s",
                                     net_value_type(value));
                    return NIL_VAL;
                }
                headers = AS_MAP(value);
            }
        } else if (strcmp(name, "timeout_ms") == 0) {
            if (!is_nil(value)) {
                if (!is_int(value) || AS_INT(value) < 0) {
                    vm_runtime_error(ERROR_VALUE, "'http_request' timeout_ms must be a non-negative "
                                                  "integer");
                    return NIL_VAL;
                }
                if (AS_INT(value) > 0) {
                    timeout = AS_INT(value);
                }
            }
        } else if (strcmp(name, "follow_redirects") == 0) {
            if (!is_nil(value)) {
                if (!is_bool(value)) {
                    vm_runtime_error(ERROR_TYPE, "'http_request' follow_redirects must be a bool");
                    return NIL_VAL;
                }
                follow = AS_BOOL(value);
            }
        } else if (strcmp(name, "max_body") == 0) {
            if (!is_nil(value)) {
                if (!is_int(value) || AS_INT(value) <= 0 || AS_INT(value) > NET_READ_MAX) {
                    vm_runtime_error(ERROR_VALUE,
                                     "'http_request' max_body must be between 1 and %d", NET_READ_MAX);
                    return NIL_VAL;
                }
                max_body = AS_INT(value);
            }
        } else if (strcmp(name, "ca_file") == 0) {
            if (!is_nil(value)) {
                if (!is_obj_type(value, OBJ_STRING)) {
                    vm_runtime_error(ERROR_TYPE,
                                     "'http_request' ca_file must be a string, received %s",
                                     net_value_type(value));
                    return NIL_VAL;
                }
                if (net_string_has_null(AS_STRING(value), "http_request")) {
                    return NIL_VAL;
                }
                ca_file = AS_STRING(value)->chars;
            }
        } else {
            vm_runtime_error(ERROR_VALUE, "'http_request' does not accept the option '%s'", name);
            return NIL_VAL;
        }
    }

    if (!url) {
        vm_runtime_error(ERROR_VALUE, "'http_request' requires a 'url' option");
        return NIL_VAL;
    }
    if (method_string && !net_http_safe_text(method_string, "http_request", "method")) {
        return NIL_VAL;
    }
    for (const char *p = method; *p; p++) {
        if (*p == ' ' || *p == '\t') {
            vm_runtime_error(ERROR_VALUE, "'http_request' method cannot contain whitespace");
            return NIL_VAL;
        }
    }
    return net_http_execute("http_request", method, url, body, headers, timeout, follow, max_body,
                            ca_file);
}

/* ------------------------------------------------------------------ */
/* Native functions                                                    */
/* ------------------------------------------------------------------ */

static void net_waiter_push(ObjCoroutine *coroutine);

static Value native_tls_connect(int arg_count, Value *args)
{
    if (arg_count < 2 || arg_count > 4) {
        vm_runtime_error(ERROR_ARITY, "'tls_connect' expects between 2 and 4 arguments, received %d",
                         arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_STRING)) {
        vm_runtime_error(ERROR_TYPE, "'tls_connect' expects a string host, received %s",
                         net_value_type(args[0]));
        return NIL_VAL;
    }
    ObjString *host = AS_STRING(args[0]);

    if (net_string_has_null(host, "tls_connect")) {
        return NIL_VAL;
    }

    int64_t port = 0;
    int64_t timeout = 0;

    if (!net_int_argument(args[1], "tls_connect", "port", &port)) {
        return NIL_VAL;
    }
    if (!net_optional_timeout(arg_count > 2 ? args[2] : NIL_VAL, arg_count, 2, "tls_connect",
                              &timeout)) {
        return NIL_VAL;
    }

    const char *ca_file = NULL;

    if (arg_count > 3 && !is_nil(args[3])) {
        if (!is_obj_type(args[3], OBJ_STRING)) {
            vm_runtime_error(ERROR_TYPE, "'tls_connect' expects a string ca_file, received %s",
                             net_value_type(args[3]));
            return NIL_VAL;
        }
        if (net_string_has_null(AS_STRING(args[3]), "tls_connect")) {
            return NIL_VAL;
        }
        ca_file = AS_STRING(args[3])->chars;
    }

    NetError err;
    int64_t deadline = net_deadline(timeout);
    int fd = net_connect(host->chars, port, deadline, &err);

    if (fd < 0) {
        net_raise(&err);
        return NIL_VAL;
    }

    SSL_CTX *context = net_tls_context(ca_file, &err);

    if (!context) {
        close(fd);
        net_raise(&err);
        return NIL_VAL;
    }

    SSL *ssl = net_tls_client(context, host->chars, fd, &err);

    if (!ssl) {
        SSL_CTX_free(context);
        close(fd);
        net_raise(&err);
        return NIL_VAL;
    }

    ObjSocket *socket = object_socket(fd, NET_TLS, timeout);

    socket->ssl = ssl;
    socket->ssl_ctx = context;

    short events = 0;
    int status = net_tls_handshake_step(ssl, &events, &err);

    if (status < 0) {
        net_socket_dispose(socket);
        net_raise(&err);
        return NIL_VAL;
    }
    if (status == 1) {
        return OBJ_VAL(socket);
    }

    if (vm_run_depth() > 1) {
        net_socket_dispose(socket);
        vm_runtime_error(ERROR_RUNTIME,
                         "a tls_connect that would block cannot run inside a callback or a module");
        return NIL_VAL;
    }

    ObjCoroutine *self = vm_current_coroutine();

    self->wait_kind = (events & POLLOUT) ? CORO_WAIT_WRITE : CORO_WAIT_READ;
    self->wait_op = NET_OP_TLS_HANDSHAKE;
    self->wait_socket = socket;
    self->wait_server = NULL;
    self->wait_sockets = NULL;
    self->wait_events = (events & POLLOUT) ? NET_EVENT_WRITE : NET_EVENT_READ;
    self->wait_timeout = timeout;
    self->wait_deadline = deadline;
    self->wait_arg = 0;
    self->wait_value = NIL_VAL;
    self->status = CORO_BLOCKED;
    net_waiter_push(self);
    vm_block_native();
    return NIL_VAL;
}

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

static Value native_net_select(int arg_count, Value *args);

void net_init(void)
{
    vm_define_native("tcp_connect", native_tcp_connect, -1);
    vm_define_native("tls_connect", native_tls_connect, -1);
    vm_define_native("tcp_listen", native_tcp_listen, -1);
    vm_define_native("udp_bind", native_udp_bind, -1);
    vm_define_native("net_hostname", native_net_hostname, 0);
    vm_define_native("net_select", native_net_select, -1);
    vm_define_native("url_parse", native_url_parse, 1);
    vm_define_native("url_encode", native_url_encode, 1);
    vm_define_native("url_decode", native_url_decode, 1);
    vm_define_native("parse_query", native_parse_query, 1);
    vm_define_native("http_get", native_http_get, -1);
    vm_define_native("http_post", native_http_post, -1);
    vm_define_native("http_put", native_http_put, -1);
    vm_define_native("http_delete", native_http_delete, -1);
    vm_define_native("http_request", native_http_request, 1);
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
    net_socket_dispose(socket);
    return NIL_VAL;
}

/* Writes the payload from `offset`, continuing where a previous attempt stopped.
 * On NET_AGAIN, `*progress` gets the bytes written so far. */
static int net_do_send(ObjSocket *socket, ObjString *payload, size_t offset, size_t *progress,
                       Value *out, NetError *err)
{
    size_t sent = offset;

    while (sent < (size_t)payload->length) {
        ssize_t n = send(socket->fd, payload->chars + sent, (size_t)payload->length - sent,
                         MSG_NOSIGNAL);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            *progress = sent;
            return NET_AGAIN;
        }
        net_fail_errno(err, errno, "send failed");
        return NET_FAILED;
    }
    *out = INT_VAL((int64_t)sent);
    return NET_DONE;
}

static int net_do_recv(ObjSocket *socket, int max, Value *out, NetError *err)
{
    char *buffer;

    if (socket->rbuf_pos < socket->rbuf_len) {
        int available = socket->rbuf_len - socket->rbuf_pos;
        int take = available < max ? available : max;

        buffer = malloc((size_t)take);
        if (!buffer) {
            net_fail(err, ERROR_MEMORY, "out of memory in 'recv'");
            return NET_FAILED;
        }
        memcpy(buffer, socket->rbuf + socket->rbuf_pos, (size_t)take);
        socket->rbuf_pos += take;
        if (socket->rbuf_pos >= socket->rbuf_len) {
            net_socket_reset_buffer(socket);
        }
        ObjString *buffered = object_string(buffer, take);

        free(buffer);
        *out = OBJ_VAL(buffered);
        return NET_DONE;
    }

    if (max == 0) {
        *out = OBJ_VAL(object_string("", 0));
        return NET_DONE;
    }

    buffer = malloc((size_t)max);
    if (!buffer) {
        net_fail(err, ERROR_MEMORY, "out of memory in 'recv'");
        return NET_FAILED;
    }

    for (;;) {
        ssize_t received = recv(socket->fd, buffer, (size_t)max, 0);

        if (received > 0) {
            ObjString *result = object_string(buffer, (int)received);

            free(buffer);
            *out = OBJ_VAL(result);
            return NET_DONE;
        }
        if (received == 0) {
            free(buffer);
            *out = NIL_VAL; /* EOF */
            return NET_DONE;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            free(buffer);
            return NET_AGAIN;
        }
        net_fail_errno(err, errno, "recv failed");
        free(buffer);
        return NET_FAILED;
    }
}

/* Reads up to a newline, accumulating into the socket's read buffer so a
 * would-block leaves the partial line intact for the next attempt. */
static int net_do_recv_line(ObjSocket *socket, int max, Value *out, NetError *err)
{
    for (;;) {
        char *start = socket->rbuf + socket->rbuf_pos;
        int available = socket->rbuf_len - socket->rbuf_pos;
        char *newline = available > 0 ? memchr(start, '\n', (size_t)available) : NULL;

        if (newline) {
            int length = (int)(newline - start);

            if (length > 0 && start[length - 1] == '\r') {
                length--;
            }
            ObjString *result = object_string(start, length);

            socket->rbuf_pos = (int)(newline - socket->rbuf) + 1;
            if (socket->rbuf_pos >= socket->rbuf_len) {
                net_socket_reset_buffer(socket);
            }
            *out = OBJ_VAL(result);
            return NET_DONE;
        }

        if (available > max) {
            net_fail(err, ERROR_VALUE, "'recv_line' exceeded the maximum of %d bytes", max);
            return NET_FAILED;
        }

        if (socket->rbuf_len == socket->rbuf_cap) {
            if (socket->rbuf_pos > 0) {
                memmove(socket->rbuf, socket->rbuf + socket->rbuf_pos,
                        (size_t)(socket->rbuf_len - socket->rbuf_pos));
                socket->rbuf_len -= socket->rbuf_pos;
                socket->rbuf_pos = 0;
            }
            if (socket->rbuf_len == socket->rbuf_cap) {
                if (socket->rbuf_cap > max) {
                    net_fail(err, ERROR_VALUE, "'recv_line' exceeded the maximum of %d bytes", max);
                    return NET_FAILED;
                }
                int capacity = socket->rbuf_cap * 2;
                if (capacity > max + 1) {
                    capacity = max + 1;
                }
                socket->rbuf = mem_realloc(socket->rbuf, (size_t)socket->rbuf_cap,
                                           (size_t)capacity);
                socket->rbuf_cap = capacity;
            }
        }

        ssize_t received = recv(socket->fd, socket->rbuf + socket->rbuf_len,
                                (size_t)(socket->rbuf_cap - socket->rbuf_len), 0);

        if (received > 0) {
            socket->rbuf_len += (int)received;
            continue;
        }
        if (received == 0) {
            if (available == 0) {
                *out = NIL_VAL; /* EOF with nothing buffered */
                return NET_DONE;
            }
            int length = available;

            if (length > 0 && start[length - 1] == '\r') {
                length--;
            }
            ObjString *result = object_string(start, length);

            net_socket_reset_buffer(socket);
            *out = OBJ_VAL(result);
            return NET_DONE;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return NET_AGAIN;
        }
        net_fail_errno(err, errno, "recv failed");
        return NET_FAILED;
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

static int net_do_accept(ObjServer *server, int64_t timeout, Value *out, NetError *err)
{
    for (;;) {
        int fd = accept(server->fd, NULL, NULL);

        if (fd >= 0) {
            net_set_nonblocking(fd, true);
            *out = OBJ_VAL(object_socket(fd, NET_TCP, timeout));
            return NET_DONE;
        }
        if (errno == EINTR || errno == ECONNABORTED) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return NET_AGAIN;
        }
        net_fail_errno(err, errno, "accept failed");
        return NET_FAILED;
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
/* Coroutine waits                                                     */
/* ------------------------------------------------------------------ */

static const char *net_op_name(NetWaitOp op)
{
    switch (op) {
    case NET_OP_WAIT: return "wait";
    case NET_OP_RECV: return "recv";
    case NET_OP_RECV_LINE: return "recv_line";
    case NET_OP_SEND: return "send";
    case NET_OP_ACCEPT: return "accept";
    case NET_OP_SELECT: return "net_select";
    case NET_OP_TLS_HANDSHAKE: return "tls_connect";
    case NET_OP_TLS_READ: return "read";
    case NET_OP_TLS_WRITE: return "write";
    }
    return "operation";
}

static ObjCoroutine *net_waiters_head = NULL;
static ObjCoroutine *net_waiters_tail = NULL;

static void net_waiter_push(ObjCoroutine *coroutine)
{
    coroutine->wait_next = NULL;
    if (net_waiters_tail) {
        net_waiters_tail->wait_next = coroutine;
    } else {
        net_waiters_head = coroutine;
    }
    net_waiters_tail = coroutine;
}

bool net_has_waiters(void)
{
    return net_waiters_head != NULL;
}

void net_mark_waiters(void)
{
    for (ObjCoroutine *waiter = net_waiters_head; waiter; waiter = waiter->wait_next) {
        vm_mark_object((Obj *)waiter);
        if (waiter->wait_socket) {
            vm_mark_object((Obj *)waiter->wait_socket);
        }
        if (waiter->wait_server) {
            vm_mark_object((Obj *)waiter->wait_server);
        }
        if (waiter->wait_sockets) {
            vm_mark_object((Obj *)waiter->wait_sockets);
        }
    }
}

/* Suspends the current coroutine until a descriptor is ready or `timeout_ms`
 * (0 = forever) passes. */
static int net_wait_begin(ObjCoroutine *self, NetWaitOp op, ObjSocket *socket, ObjServer *server,
                          int events, int64_t timeout_ms, int arg, Value value)
{
    self->wait_kind = (events & NET_EVENT_WRITE) ? CORO_WAIT_WRITE : CORO_WAIT_READ;
    self->wait_op = op;
    self->wait_socket = socket;
    self->wait_server = server;
    self->wait_sockets = NULL;
    self->wait_events = events;
    self->wait_timeout = timeout_ms;
    self->wait_deadline = timeout_ms > 0 ? net_now_ms() + timeout_ms : 0;
    self->wait_arg = arg;
    self->wait_value = value;
    self->status = CORO_BLOCKED;
    net_waiter_push(self);
    return 1;
}

/* Registers a wait, or raises the documented error when the operation runs
 * inside a native callback or an imported module and must not block. */
static int net_block(ObjCoroutine *self, const char *operation, NetWaitOp op, ObjSocket *socket,
                     ObjServer *server, int events, int64_t timeout_ms, int arg, Value value,
                     Value *out)
{
    if (vm_run_depth() > 1) {
        vm_runtime_error(ERROR_RUNTIME,
                         "a %s that would block cannot run inside a callback or a module",
                         operation);
        return -1;
    }
    *out = NIL_VAL;
    return net_wait_begin(self, op, socket, server, events, timeout_ms, arg, value);
}

static void net_deferred_error(ObjCoroutine *waiter, ErrorType type, const char *message)
{
    snprintf(waiter->resume_error_buffer, sizeof(waiter->resume_error_buffer), "%s", message);
    vm_wake_coroutine(waiter, NIL_VAL, waiter->resume_error_buffer);
    waiter->resume_error_type = type;
}

static void net_timeout_wake(ObjCoroutine *waiter)
{
    if (waiter->wait_op == NET_OP_ACCEPT) {
        vm_wake_coroutine(waiter, NIL_VAL, NULL); /* accept returns nil on timeout */
        return;
    }
    if (waiter->wait_op == NET_OP_SELECT) {
        mem_pause_gc();
        ObjArray *empty = object_array();
        mem_resume_gc();
        vm_wake_coroutine(waiter, OBJ_VAL(empty), NULL);
        return;
    }
    if (waiter->wait_op == NET_OP_TLS_HANDSHAKE && waiter->wait_socket) {
        net_socket_dispose(waiter->wait_socket);
    }
    snprintf(waiter->resume_error_buffer, sizeof(waiter->resume_error_buffer),
             "'%s' timed out after %lld ms", net_op_name(waiter->wait_op),
             (long long)waiter->wait_timeout);
    vm_wake_coroutine(waiter, NIL_VAL, waiter->resume_error_buffer);
    waiter->resume_error_type = ERROR_TIMEOUT;
}

/* Performs (or reports) a waiter whose descriptor is ready or whose deadline
 * passed. Returns true when the coroutine was woken. */
static bool net_complete_waiter(ObjCoroutine *waiter, struct pollfd *fds, int base, bool candidate,
                                bool expired, bool poll_failed)
{
    Value result = NIL_VAL;
    NetError err;

    if (poll_failed) {
        net_deferred_error(waiter, ERROR_NETWORK, "network poll failed");
        return true;
    }

    switch (waiter->wait_op) {
    case NET_OP_WAIT:
        if (candidate) {
            vm_wake_coroutine(waiter, BOOL_VAL(true), NULL);
            return true;
        }
        break;
    case NET_OP_RECV:
        if (candidate) {
            int status = net_do_recv(waiter->wait_socket, waiter->wait_arg, &result, &err);

            if (status == NET_DONE) {
                vm_wake_coroutine(waiter, result, NULL);
                return true;
            }
            if (status == NET_FAILED) {
                net_deferred_error(waiter, err.type, err.message);
                return true;
            }
            return false;
        }
        break;
    case NET_OP_RECV_LINE:
        if (candidate) {
            int status = net_do_recv_line(waiter->wait_socket, waiter->wait_arg, &result, &err);

            if (status == NET_DONE) {
                vm_wake_coroutine(waiter, result, NULL);
                return true;
            }
            if (status == NET_FAILED) {
                net_deferred_error(waiter, err.type, err.message);
                return true;
            }
            return false;
        }
        break;
    case NET_OP_SEND:
        if (candidate) {
            ObjString *payload = AS_STRING(waiter->wait_value);
            size_t progress = (size_t)waiter->wait_arg;
            int status = net_do_send(waiter->wait_socket, payload, progress, &progress, &result,
                                     &err);

            if (status == NET_DONE) {
                vm_wake_coroutine(waiter, result, NULL);
                return true;
            }
            if (status == NET_FAILED) {
                net_deferred_error(waiter, err.type, err.message);
                return true;
            }
            waiter->wait_arg = (int)progress;
            return false;
        }
        break;
    case NET_OP_ACCEPT:
        if (candidate) {
            int status = net_do_accept(waiter->wait_server, waiter->wait_arg, &result, &err);

            if (status == NET_DONE) {
                vm_wake_coroutine(waiter, result, NULL);
                return true;
            }
            if (status == NET_FAILED) {
                net_deferred_error(waiter, err.type, err.message);
                return true;
            }
            return false;
        }
        break;
    case NET_OP_SELECT:
        if (candidate) {
            ObjArray *sockets = waiter->wait_sockets;

            for (int i = 0; i < sockets->count; i++) {
                if (AS_SOCKET(sockets->items[i])->is_closed) {
                    net_deferred_error(waiter, ERROR_RUNTIME, "cannot select a closed socket");
                    return true;
                }
            }

            mem_pause_gc();
            ObjArray *ready = object_array();
            for (int i = 0; i < sockets->count; i++) {
                if (fds[base + i].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
                    array_push(ready, INT_VAL(i));
                }
            }
            mem_resume_gc();
            vm_wake_coroutine(waiter, OBJ_VAL(ready), NULL);
            return true;
        }
        break;
    case NET_OP_TLS_HANDSHAKE:
        if (candidate) {
            short events = 0;

            if (waiter->wait_socket->is_closed || !waiter->wait_socket->ssl) {
                net_deferred_error(waiter, ERROR_RUNTIME, "cannot use a closed socket");
                return true;
            }
            int status = net_tls_handshake_step((SSL *)waiter->wait_socket->ssl, &events, &err);

            if (status == 1) {
                vm_wake_coroutine(waiter, OBJ_VAL(waiter->wait_socket), NULL);
                return true;
            }
            if (status < 0) {
                net_socket_dispose(waiter->wait_socket);
                net_deferred_error(waiter, err.type, err.message);
                return true;
            }
            waiter->wait_events = (events & POLLOUT) ? NET_EVENT_WRITE : NET_EVENT_READ;
            return false;
        }
        break;
    case NET_OP_TLS_READ:
        if (candidate) {
            short events = POLLIN;
            int status = net_do_tls_read(waiter->wait_socket, waiter->wait_arg, &events, &result,
                                         &err);

            if (status == NET_DONE) {
                vm_wake_coroutine(waiter, result, NULL);
                return true;
            }
            if (status == NET_FAILED) {
                net_deferred_error(waiter, err.type, err.message);
                return true;
            }
            waiter->wait_events = (events & POLLOUT) ? NET_EVENT_WRITE : NET_EVENT_READ;
            return false;
        }
        break;
    case NET_OP_TLS_WRITE:
        if (candidate) {
            ObjString *payload = AS_STRING(waiter->wait_value);
            size_t progress = (size_t)waiter->wait_arg;
            short events = POLLOUT;
            int status = net_do_tls_write(waiter->wait_socket, payload, progress, &progress,
                                          &events, &result, &err);

            if (status == NET_DONE) {
                vm_wake_coroutine(waiter, result, NULL);
                return true;
            }
            if (status == NET_FAILED) {
                net_deferred_error(waiter, err.type, err.message);
                return true;
            }
            waiter->wait_arg = (int)progress;
            waiter->wait_events = (events & POLLIN) ? NET_EVENT_READ : NET_EVENT_WRITE;
            return false;
        }
        break;
    }

    if (expired) {
        net_timeout_wake(waiter);
        return true;
    }
    return false;
}

void net_scheduler_poll(bool may_block)
{
    if (!net_waiters_head) {
        return;
    }

    int count = 0;
    for (ObjCoroutine *waiter = net_waiters_head; waiter; waiter = waiter->wait_next) {
        count += waiter->wait_op == NET_OP_SELECT ? waiter->wait_sockets->count : 1;
    }

    struct pollfd *fds = count > 0 ? malloc(sizeof(struct pollfd) * (size_t)count) : NULL;

    if (count > 0 && !fds) {
        return; /* allocation failed: leave the waiters for the next round */
    }

    int index = 0;
    for (ObjCoroutine *waiter = net_waiters_head; waiter; waiter = waiter->wait_next) {
        if (waiter->wait_op == NET_OP_SELECT) {
            ObjArray *sockets = waiter->wait_sockets;

            for (int i = 0; i < sockets->count; i++) {
                ObjSocket *socket = AS_SOCKET(sockets->items[i]);

                fds[index].fd = socket->is_closed ? -1 : socket->fd;
                fds[index].events = POLLIN;
                fds[index].revents = 0;
                index++;
            }
        } else {
            int fd = waiter->wait_socket ? waiter->wait_socket->fd
                                         : (waiter->wait_server ? waiter->wait_server->fd : -1);
            short events = 0;

            if (waiter->wait_events & NET_EVENT_READ) {
                events |= POLLIN;
            }
            if (waiter->wait_events & NET_EVENT_WRITE) {
                events |= POLLOUT;
            }
            fds[index].fd = fd;
            fds[index].events = events;
            fds[index].revents = 0;
            index++;
        }
    }

    int timeout = 0;
    if (may_block) {
        int64_t now = net_now_ms();

        timeout = -1;
        for (ObjCoroutine *waiter = net_waiters_head; waiter; waiter = waiter->wait_next) {
            if (waiter->wait_deadline == 0) {
                continue;
            }
            int64_t remaining = waiter->wait_deadline - now;
            int ms = remaining <= 0 ? 0 : (remaining > INT32_MAX ? INT32_MAX : (int)remaining);

            if (timeout < 0 || ms < timeout) {
                timeout = ms;
            }
        }
    }

    int ready = -1;
    bool poll_failed = false;

    if (count > 0) {
        do {
            ready = poll(fds, (nfds_t)count, timeout);
        } while (ready < 0 && errno == EINTR);
        poll_failed = ready < 0;
    }

    int64_t now = net_now_ms();
    ObjCoroutine *keep_head = NULL;
    ObjCoroutine *keep_tail = NULL;

    index = 0;
    for (ObjCoroutine *waiter = net_waiters_head; waiter;) {
        ObjCoroutine *next = waiter->wait_next;
        int base = index;
        bool candidate = false;
        bool expired = waiter->wait_deadline != 0 && now >= waiter->wait_deadline;

        if (waiter->wait_op == NET_OP_SELECT) {
            ObjArray *sockets = waiter->wait_sockets;

            for (int i = 0; i < sockets->count; i++) {
                short revents = fds ? fds[index + i].revents : 0;

                if (revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL) ||
                    AS_SOCKET(sockets->items[i])->is_closed) {
                    candidate = true;
                }
            }
            index += sockets->count;
        } else {
            short revents = fds ? fds[index].revents : 0;

            if (revents & (POLLIN | POLLOUT | POLLERR | POLLHUP | POLLNVAL)) {
                candidate = true;
            }
            if ((waiter->wait_socket && waiter->wait_socket->is_closed) ||
                (waiter->wait_server && waiter->wait_server->is_closed)) {
                candidate = true;
            }
            index++;
        }

        bool completed = false;

        if (poll_failed || candidate || expired) {
            completed = net_complete_waiter(waiter, fds, base, candidate, expired, poll_failed);
        }
        if (!completed) {
            waiter->wait_next = NULL;
            if (keep_tail) {
                keep_tail->wait_next = waiter;
            } else {
                keep_head = waiter;
            }
            keep_tail = waiter;
        }
        waiter = next;
    }

    free(fds);
    net_waiters_head = keep_head;
    net_waiters_tail = keep_tail;
}

static Value net_ready_indexes(ObjArray *sockets, struct pollfd *fds, int base)
{
    mem_pause_gc();
    ObjArray *ready = object_array();

    for (int i = 0; i < sockets->count; i++) {
        if (fds[base + i].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
            array_push(ready, INT_VAL(i));
        }
    }
    mem_resume_gc();
    return OBJ_VAL(ready);
}

static Value native_net_select(int arg_count, Value *args)
{
    if (arg_count < 1 || arg_count > 2) {
        vm_runtime_error(ERROR_ARITY, "'net_select' expects 1 or 2 arguments, received %d",
                         arg_count);
        return NIL_VAL;
    }
    if (!is_obj_type(args[0], OBJ_ARRAY)) {
        vm_runtime_error(ERROR_TYPE, "'net_select' expects an array of sockets, received %s",
                         net_value_type(args[0]));
        return NIL_VAL;
    }
    ObjArray *sockets = AS_ARRAY(args[0]);
    int64_t timeout = 0;

    if (!net_optional_timeout(arg_count > 1 ? args[1] : NIL_VAL, arg_count, 1, "net_select",
                              &timeout)) {
        return NIL_VAL;
    }
    for (int i = 0; i < sockets->count; i++) {
        if (!is_obj_type(sockets->items[i], OBJ_SOCKET)) {
            vm_runtime_error(ERROR_TYPE, "'net_select' expects sockets, received %s",
                             net_value_type(sockets->items[i]));
            return NIL_VAL;
        }
        if (AS_SOCKET(sockets->items[i])->is_closed) {
            vm_runtime_error(ERROR_RUNTIME, "cannot select a closed socket");
            return NIL_VAL;
        }
        if (AS_SOCKET(sockets->items[i])->kind == NET_TLS) {
            vm_runtime_error(ERROR_NETWORK,
                             "'net_select' does not support TLS sockets; use 'read' with a timeout");
            return NIL_VAL;
        }
    }

    if (sockets->count == 0) {
        return OBJ_VAL(object_array());
    }

    int count = sockets->count;
    struct pollfd *fds;

    if (count <= 0) {
        return OBJ_VAL(object_array());
    }
    fds = malloc(sizeof(struct pollfd) * (size_t)count);

    if (!fds) {
        vm_runtime_error(ERROR_MEMORY, "out of memory in 'net_select'");
        return NIL_VAL;
    }
    for (int i = 0; i < count; i++) {
        fds[i].fd = AS_SOCKET(sockets->items[i])->fd;
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }
    int ready = poll(fds, (nfds_t)count, 0);

    if (ready > 0) {
        Value indexes = net_ready_indexes(sockets, fds, 0);

        free(fds);
        return indexes;
    }
    free(fds);

    if (vm_run_depth() > 1) {
        vm_runtime_error(ERROR_RUNTIME,
                         "a net_select that would block cannot run inside a callback or a module");
        return NIL_VAL;
    }

    ObjCoroutine *self = vm_current_coroutine();

    self->wait_kind = CORO_WAIT_READ;
    self->wait_op = NET_OP_SELECT;
    self->wait_socket = NULL;
    self->wait_server = NULL;
    self->wait_sockets = sockets;
    self->wait_events = NET_EVENT_READ;
    self->wait_timeout = timeout;
    self->wait_deadline = timeout > 0 ? net_now_ms() + timeout : 0;
    self->wait_arg = 0;
    self->wait_value = NIL_VAL;
    self->status = CORO_BLOCKED;
    net_waiter_push(self);
    vm_block_native();
    return NIL_VAL;
}

/* ------------------------------------------------------------------ */
/* Method dispatch                                                     */
/* ------------------------------------------------------------------ */

int net_dispatch_method(Value receiver, ObjString *method, int arg_count, Value *args, Value *out)
{
    const char *name = method->chars;
    ObjCoroutine *self = vm_current_coroutine();
    NetError err;

    if (is_obj_type(receiver, OBJ_SOCKET)) {
        ObjSocket *socket = AS_SOCKET(receiver);

        if (strcmp(name, "close") == 0) {
            if (arg_count != 0) {
                vm_runtime_error(ERROR_ARITY, "'close' expects no arguments, received %d", arg_count);
                return -1;
            }
            *out = net_socket_close(socket);
            return 0;
        }
        if (strcmp(name, "read") == 0) {
            int64_t max = 0;

            if (arg_count != 1) {
                vm_runtime_error(ERROR_ARITY, "'read' expects 1 argument, received %d", arg_count);
                return -1;
            }
            if (socket->kind != NET_TLS) {
                vm_runtime_error(ERROR_NETWORK,
                                 "'read' requires a TLS socket; use 'recv' or 'recv_from' instead");
                return -1;
            }
            if (!net_int_argument(args[0], "read", "size", &max)) {
                return -1;
            }
            if (max < 0 || max > NET_READ_MAX) {
                vm_runtime_error(ERROR_VALUE, "'read' size out of valid range (0..%d)", NET_READ_MAX);
                return -1;
            }
            if (!net_socket_open(socket)) {
                return -1;
            }

            short events = POLLIN;
            int status = net_do_tls_read(socket, (int)max, &events, out, &err);

            if (status == NET_DONE) {
                return 0;
            }
            if (status == NET_FAILED) {
                net_raise(&err);
                return -1;
            }
            return net_block(self, "read", NET_OP_TLS_READ, socket, NULL,
                             (events & POLLOUT) ? NET_EVENT_WRITE : NET_EVENT_READ,
                             socket->timeout_ms, (int)max, NIL_VAL, out);
        }
        if (strcmp(name, "write") == 0) {
            if (arg_count != 1) {
                vm_runtime_error(ERROR_ARITY, "'write' expects 1 argument, received %d", arg_count);
                return -1;
            }
            if (socket->kind != NET_TLS) {
                vm_runtime_error(ERROR_NETWORK,
                                 "'write' requires a TLS socket; use 'send' or 'send_to' instead");
                return -1;
            }
            if (!is_obj_type(args[0], OBJ_STRING)) {
                vm_runtime_error(ERROR_TYPE, "'write' expects a string, received %s",
                                 net_value_type(args[0]));
                return -1;
            }
            if (!net_socket_open(socket)) {
                return -1;
            }
            ObjString *payload = AS_STRING(args[0]);

            if (payload->length > INT32_MAX) {
                vm_runtime_error(ERROR_VALUE, "'write' payload is too large");
                return -1;
            }

            size_t progress = 0;
            short events = POLLOUT;
            int status = net_do_tls_write(socket, payload, 0, &progress, &events, out, &err);

            if (status == NET_DONE) {
                return 0;
            }
            if (status == NET_FAILED) {
                net_raise(&err);
                return -1;
            }
            return net_block(self, "write", NET_OP_TLS_WRITE, socket, NULL,
                             (events & POLLIN) ? NET_EVENT_READ : NET_EVENT_WRITE,
                             socket->timeout_ms, (int)progress, args[0], out);
        }
        if (strcmp(name, "wait") == 0 && arg_count <= 3) {
            bool read = false;
            bool write = false;
            int64_t timeout = 0;

            if (!net_socket_open(socket)) {
                return -1;
            }
            if (socket->kind == NET_TLS) {
                vm_runtime_error(ERROR_NETWORK,
                                 "'wait' is not supported on TLS sockets; use 'read' with a timeout");
                return -1;
            }
            if (arg_count >= 1 && !is_nil(args[0])) {
                if (!is_bool(args[0])) {
                    vm_runtime_error(ERROR_TYPE, "'wait' read must be a bool, received %s",
                                     net_value_type(args[0]));
                    return -1;
                }
                read = AS_BOOL(args[0]);
            }
            if (arg_count >= 2 && !is_nil(args[1])) {
                if (!is_bool(args[1])) {
                    vm_runtime_error(ERROR_TYPE, "'wait' write must be a bool, received %s",
                                     net_value_type(args[1]));
                    return -1;
                }
                write = AS_BOOL(args[1]);
            }
            if (!net_optional_timeout(arg_count > 2 ? args[2] : NIL_VAL, arg_count, 2, "wait",
                                      &timeout)) {
                return -1;
            }
            if (!read && !write) {
                vm_runtime_error(ERROR_VALUE, "'wait' needs read and/or write to be true");
                return -1;
            }

            short events = (short)((read ? POLLIN : 0) | (write ? POLLOUT : 0));
            struct pollfd descriptor = { socket->fd, events, 0 };
            int ready = poll(&descriptor, 1, 0);

            if (ready > 0 && (descriptor.revents & (events | POLLERR | POLLHUP | POLLNVAL))) {
                *out = BOOL_VAL(true);
                return 0;
            }
            return net_block(self, "wait", NET_OP_WAIT, socket, NULL,
                             (read ? NET_EVENT_READ : 0) | (write ? NET_EVENT_WRITE : 0),
                             timeout, 0, NIL_VAL, out);
        }
        if (strcmp(name, "send") == 0 && arg_count == 1) {
            if (!net_socket_open(socket)) {
                return -1;
            }
            if (!is_obj_type(args[0], OBJ_STRING)) {
                vm_runtime_error(ERROR_TYPE, "'send' expects a string, received %s",
                                 net_value_type(args[0]));
                return -1;
            }
            if (socket->kind != NET_TCP) {
                if (socket->kind == NET_TLS) {
                    vm_runtime_error(ERROR_NETWORK, "'send' requires a TCP socket; use 'write' for TLS");
                } else {
                    vm_runtime_error(ERROR_NETWORK,
                                     "'send' requires a TCP socket; use 'send_to' for UDP");
                }
                return -1;
            }
            ObjString *payload = AS_STRING(args[0]);

            if (payload->length > INT32_MAX) {
                vm_runtime_error(ERROR_VALUE, "'send' payload is too large");
                return -1;
            }

            size_t progress = 0;
            int status = net_do_send(socket, payload, 0, &progress, out, &err);

            if (status == NET_DONE) {
                return 0;
            }
            if (status == NET_FAILED) {
                net_raise(&err);
                return -1;
            }
            return net_block(self, "send", NET_OP_SEND, socket, NULL, NET_EVENT_WRITE,
                             socket->timeout_ms, (int)progress, args[0], out);
        }
        if (strcmp(name, "recv") == 0 && arg_count == 1) {
            int64_t max = 0;

            if (socket->kind != NET_TCP) {
                if (socket->kind == NET_TLS) {
                    vm_runtime_error(ERROR_NETWORK, "'recv' requires a TCP socket; use 'read' for TLS");
                } else {
                    vm_runtime_error(ERROR_NETWORK, "'recv' requires a TCP socket");
                }
                return -1;
            }
            if (!net_int_argument(args[0], "recv", "size", &max)) {
                return -1;
            }
            if (max < 0 || max > NET_READ_MAX) {
                vm_runtime_error(ERROR_VALUE, "'recv' size out of valid range (0..%d)", NET_READ_MAX);
                return -1;
            }
            if (!net_socket_open(socket)) {
                return -1;
            }

            int status = net_do_recv(socket, (int)max, out, &err);

            if (status == NET_DONE) {
                return 0;
            }
            if (status == NET_FAILED) {
                net_raise(&err);
                return -1;
            }
            return net_block(self, "recv", NET_OP_RECV, socket, NULL, NET_EVENT_READ,
                             socket->timeout_ms, (int)max, NIL_VAL, out);
        }
        if (strcmp(name, "recv_line") == 0 && (arg_count == 0 || arg_count == 1)) {
            int64_t max = NET_LINE_DEFAULT_MAX;

            if (socket->kind != NET_TCP) {
                if (socket->kind == NET_TLS) {
                    vm_runtime_error(ERROR_NETWORK,
                                     "'recv_line' requires a TCP socket; use 'read' for TLS");
                } else {
                    vm_runtime_error(ERROR_NETWORK, "'recv_line' requires a TCP socket");
                }
                return -1;
            }
            if (arg_count == 1 && !is_nil(args[0]) &&
                !net_int_argument(args[0], "recv_line", "size", &max)) {
                return -1;
            }
            if (max <= 0 || max > NET_READ_MAX) {
                vm_runtime_error(ERROR_VALUE, "'recv_line' size out of valid range (1..%d)",
                                 NET_READ_MAX);
                return -1;
            }
            if (!net_socket_open(socket)) {
                return -1;
            }

            int status = net_do_recv_line(socket, (int)max, out, &err);

            if (status == NET_DONE) {
                return 0;
            }
            if (status == NET_FAILED) {
                net_raise(&err);
                return -1;
            }
            return net_block(self, "recv_line", NET_OP_RECV_LINE, socket, NULL, NET_EVENT_READ,
                             socket->timeout_ms, (int)max, NIL_VAL, out);
        }
        if (strcmp(name, "send_to") == 0 && arg_count == 3) {
            if (!net_socket_open(socket)) {
                return -1;
            }
            *out = net_socket_send_to(socket, args[0], args[1], args[2]);
            return vm_had_error() ? -1 : 0;
        }
        if (strcmp(name, "recv_from") == 0 && arg_count == 1) {
            int64_t max = 0;

            if (!net_int_argument(args[0], "recv_from", "size", &max)) {
                return -1;
            }
            if (!net_socket_open(socket)) {
                return -1;
            }
            *out = net_socket_recv_from(socket, max);
            return vm_had_error() ? -1 : 0;
        }
        if ((strcmp(name, "peer") == 0 || strcmp(name, "local") == 0) && arg_count == 0) {
            if (!net_socket_open(socket)) {
                return -1;
            }
            *out = net_socket_address(socket, strcmp(name, "peer") == 0);
            return 0;
        }
        return SPARK_NET_UNHANDLED;
    }

    if (is_obj_type(receiver, OBJ_SERVER)) {
        ObjServer *server = AS_SERVER(receiver);

        if (strcmp(name, "close") == 0) {
            if (arg_count != 0) {
                vm_runtime_error(ERROR_ARITY, "'close' expects no arguments, received %d", arg_count);
                return -1;
            }
            *out = net_server_close(server);
            return 0;
        }
        if (strcmp(name, "accept") == 0 && (arg_count == 0 || arg_count == 1)) {
            int64_t timeout = 0;

            if (!net_optional_timeout(arg_count > 0 ? args[0] : NIL_VAL, arg_count, 0, "accept",
                                      &timeout)) {
                return -1;
            }
            if (!net_server_open(server)) {
                return -1;
            }

            int status = net_do_accept(server, timeout, out, &err);

            if (status == NET_DONE) {
                return 0;
            }
            if (status == NET_FAILED) {
                net_raise(&err);
                return -1;
            }
            return net_block(self, "accept", NET_OP_ACCEPT, NULL, server, NET_EVENT_READ, timeout,
                             (int)timeout, NIL_VAL, out);
        }
        if (strcmp(name, "port") == 0 && arg_count == 0) {
            if (!net_server_open(server)) {
                return -1;
            }
            *out = net_server_port(server);
            return 0;
        }
        return SPARK_NET_UNHANDLED;
    }

    return SPARK_NET_UNHANDLED;
}
