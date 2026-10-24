/* buf.h - dynamic byte buffer, always NUL-terminated. */
#ifndef SPARK_BUF_H
#define SPARK_BUF_H

#include <stddef.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
void buf_clear(buf_t *b);

int buf_reserve(buf_t *b, size_t extra);
int buf_append(buf_t *b, const void *data, size_t len);
int buf_append_str(buf_t *b, const char *s);
int buf_appendf(buf_t *b, const char *fmt, ...);

/* Return the contents and reset the buffer. Free with free(). */
char *buf_steal(buf_t *b);

#endif
