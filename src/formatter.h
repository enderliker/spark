/* formatter.h - code formatter for the Spark language. */
#ifndef SPARK_FORMATTER_H
#define SPARK_FORMATTER_H

#include <stdbool.h>
#include <stddef.h>

/* Formats Spark source code into a canonical format.
 * Returns a dynamically allocated string (caller must free),
 * or NULL on error (with message in error_buf). */
char *formatter_format(const char *source, const char *path, char *error_buf, size_t error_buf_size);

#endif
