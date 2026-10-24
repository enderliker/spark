#include "buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void buf_init(buf_t *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(buf_t *b)
{
    if (!b) {
        return;
    }
    free(b->data);
    buf_init(b);
}

void buf_clear(buf_t *b)
{
    b->len = 0;
    if (b->data) {
        b->data[0] = '\0';
    }
}

int buf_reserve(buf_t *b, size_t extra)
{
    size_t need;
    size_t cap;
    char *grown;

    if (extra > (size_t)-1 - b->len - 1) {
        return -1;
    }
    need = b->len + extra + 1;
    if (need <= b->cap) {
        return 0;
    }

    cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }

    grown = realloc(b->data, cap);
    if (!grown) {
        return -1;
    }
    b->data = grown;
    b->cap = cap;
    if (b->len == 0) {
        b->data[0] = '\0';
    }
    return 0;
}

int buf_append(buf_t *b, const void *data, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (!data || buf_reserve(b, len) != 0) {
        return -1;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
    return 0;
}

int buf_append_str(buf_t *b, const char *s)
{
    return buf_append(b, s, s ? strlen(s) : 0);
}

int buf_appendf(buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_list copy;
    int needed;

    va_start(ap, fmt);
    va_copy(copy, ap);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(ap);
        return -1;
    }
    if (buf_reserve(b, (size_t)needed) != 0) {
        va_end(ap);
        return -1;
    }
    vsnprintf(b->data + b->len, (size_t)needed + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)needed;
    return 0;
}

char *buf_steal(buf_t *b)
{
    char *out = b->data;

    if (!out) {
        out = calloc(1, 1);
    }
    buf_init(b);
    return out;
}
