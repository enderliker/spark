/* regex.h - lightweight regular expression engine for Spark. */
#ifndef SPARK_REGEX_H
#define SPARK_REGEX_H

#include <stdbool.h>
#include <stddef.h>

#include "object.h"

/* Returns true if pattern matches text. */
bool regex_match(const char *pattern, const char *text, char *error, size_t error_size);

/* Finds all non-overlapping matches in text and returns an ObjArray of matched strings. */
ObjArray *regex_find_all(const char *pattern, const char *text, char *error, size_t error_size);

/* Replaces all non-overlapping matches in text with replacement and returns a new string. */
ObjString *regex_replace(const char *pattern, const char *replacement, const char *text,
                         char *error, size_t error_size);

#endif
