#include "error.h"

#include <stdio.h>

const char *error_type_name(ErrorType type)
{
    switch (type) {
    case ERROR_SYNTAX: return "SyntaxError";
    case ERROR_COMPILE: return "CompileError";
    case ERROR_TYPE: return "TypeError";
    case ERROR_NAME: return "NameError";
    case ERROR_CONST: return "ConstError";
    case ERROR_ZERO_DIVISION: return "ZeroDivisionError";
    case ERROR_INDEX: return "IndexError";
    case ERROR_VALUE: return "ValueError";
    case ERROR_ARITY: return "ArityError";
    case ERROR_ASSERT: return "AssertionError";
    case ERROR_RECURSION: return "RecursionError";
    case ERROR_MEMORY: return "MemoryError";
    case ERROR_RUNTIME: return "RuntimeError";
    }
    return "Error";
}

/* Points `*out` at the first byte of `line` and returns its length in bytes,
 * or returns -1 when the source has fewer lines. */
static int find_line(const char *source, int line, const char **out)
{
    const char *p = source;
    int current = 1;

    while (current < line) {
        while (*p && *p != '\n') {
            p++;
        }
        if (*p == '\0') {
            return -1;
        }
        p++;
        current++;
    }

    const char *end = p;

    while (*end && *end != '\n' && *end != '\r') {
        end++;
    }
    *out = p;
    return (int)(end - p);
}

void error_print(ErrorType type, int line, int column, const char *source, const char *message)
{
    const char *text = NULL;
    int length;

    fprintf(stderr, "%s at line %d: %s\n", error_type_name(type), line, message);
    if (!source || line < 1) {
        fflush(stderr);
        return;
    }

    length = find_line(source, line, &text);
    if (length < 0) {
        fflush(stderr);
        return;
    }

    if (column < 1) {
        /* Without a column, point at the first non-blank character. */
        column = 1;
        while (column <= length && (text[column - 1] == ' ' || text[column - 1] == '\t')) {
            column++;
        }
    }

    fprintf(stderr, "%4d | %.*s\n", line, length, text);
    fputs("     | ", stderr);

    /* One output cell per source character; tabs are kept so the caret lines
     * up with the line above in a terminal. */
    for (const char *p = text; p < text + length && p < text + column - 1; p++) {
        if (((unsigned char)*p & 0xc0) == 0x80) {
            continue; /* UTF-8 continuation byte */
        }
        fputc(*p == '\t' ? '\t' : ' ', stderr);
    }
    fputs("^\n", stderr);
    fflush(stderr);
}