/* regex.h - lightweight regular expression engine for Spark.
 *
 * The engine is a Pike VM over the NFA of the pattern (see regex.c). Matching
 * follows Perl/JavaScript leftmost-first semantics (decision Q27), and every
 * position the engine reports is a UTF-8 character offset, consistent with
 * `len` and `index_of`.
 *
 * All functions report syntax errors and the step-limit failure through the
 * caller's `error` buffer (prefixed "regex syntax error:" or "regex:"); when a
 * function returns false or NULL with a non-empty buffer, the native must raise
 * a ValueError with that text. */
#ifndef SPARK_REGEX_H
#define SPARK_REGEX_H

#include <stdbool.h>
#include <stddef.h>

#include "object.h"

typedef struct {
    bool case_insensitive;
    bool dotall;
    bool multiline;
} RegexFlags;

/* Parses the optional flags string of the natives ("i", "s", "m", possibly
 * combined or empty). Returns false on an unknown flag. */
bool regex_parse_flags(const char *flags, RegexFlags *out, char *error, size_t error_size);

/* Whether pattern matches text. */
bool regex_match(const char *pattern, const char *text, RegexFlags flags, char *error,
                 size_t error_size);

/* All non-overlapping matches, as strings. */
ObjArray *regex_find_all(const char *pattern, const char *text, RegexFlags flags, char *error,
                         size_t error_size);

/* All non-overlapping matches, as maps {start, end} with character offsets. */
ObjArray *regex_find_index(const char *pattern, const char *text, RegexFlags flags, char *error,
                           size_t error_size);

/* Tries to match starting exactly at `char_start` (a character offset). On a
 * match returns true and fills the character offsets; on no match returns false
 * with an empty error buffer. */
bool regex_match_at(const char *pattern, const char *text, int char_start, RegexFlags flags,
                    int *start_char, int *end_char, char *error, size_t error_size);

/* Replaces every non-overlapping match. The replacement expands `$1`, `$0`,
 * `${name}` and `$$`; an unknown group is an error. */
ObjString *regex_replace(const char *pattern, const char *replacement, const char *text,
                         RegexFlags flags, char *error, size_t error_size);

/* Splits text on non-empty matches; zero-width matches are ignored. `limit` is
 * the maximum number of parts (negative means no limit). */
ObjArray *regex_split(const char *pattern, const char *text, int limit, RegexFlags flags,
                      char *error, size_t error_size);

/* The first match with capture groups:
 * {start, end, groups: [{start, end, text} | nil, ...], named: {name: group}}.
 * `*result` is set only on a match; it returns false with an empty error buffer
 * when there is no match. */
bool regex_match_groups(const char *pattern, const char *text, RegexFlags flags, ObjMap **result,
                        char *error, size_t error_size);

#endif