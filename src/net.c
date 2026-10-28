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

/* HTTP limits (phase 17). Every bound has an explicit error: the status line
 * plus the header block may not exceed 64 KiB, a body defaults to 16 MiB and
 * redirects stop after five hops. */
#define NET_HTTP_HEADER_MAX (64 * 1024)
#define NET_HTTP_BODY_DEFAULT_MAX NET_READ_MAX
#define NET_HTTP_REDIRECT_MAX 5
#define NET_HTTP_MAX_HEADERS 1000
#define NET_HTTP_TIMEOUT_DEFAULT 30000
#define NET_HTTP_USER_AGENT "spark/0.1"

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

static Value native_net_select(int arg_count, Value *args);

void net_init(void)
{
    vm_define_native("tcp_connect", native_tcp_connect, -1);
    vm_define_native("tcp_listen", native_tcp_listen, -1);
    vm_define_native("udp_bind", native_udp_bind, -1);
    vm_define_native("net_hostname", native_net_hostname, 0);
    vm_define_native("net_select", native_net_select, -1);
    vm_define_native("url_parse", native_url_parse, 1);
    vm_define_native("url_encode", native_url_encode, 1);
    vm_define_native("url_decode", native_url_decode, 1);
    vm_define_native("parse_query", native_parse_query, 1);
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

/* Result of a non-blocking socket operation. */
#define NET_DONE 0
#define NET_AGAIN 1
#define NET_FAILED (-1)

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
        if (strcmp(name, "wait") == 0 && arg_count <= 3) {
            bool read = false;
            bool write = false;
            int64_t timeout = 0;

            if (!net_socket_open(socket)) {
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
                vm_runtime_error(ERROR_NETWORK, "'send' requires a TCP socket; use 'send_to' for UDP");
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
