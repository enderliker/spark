/* error.h - error categories shared by the compiler and the VM. */
#ifndef SPARK_ERROR_H
#define SPARK_ERROR_H

/* Every diagnostic carries a category so callers can tell errors apart
 * (useful for a future try/catch) and print a recognizable name. */
typedef enum {
    ERROR_SYNTAX,        /* malformed source (lexer/parser) */
    ERROR_COMPILE,       /* semantic error detected before running */
    ERROR_TYPE,          /* an operation received a value of the wrong type */
    ERROR_NAME,          /* a global name is not defined */
    ERROR_CONST,         /* assigning to or redefining a constant */
    ERROR_ZERO_DIVISION, /* division or modulo by zero */
    ERROR_INDEX,         /* array or string index out of range */
    ERROR_VALUE,         /* a value is outside the accepted domain */
    ERROR_ARITY,         /* wrong number of arguments */
    ERROR_ASSERT,        /* a failed assert */
    ERROR_RECURSION,     /* call depth exhausted */
    ERROR_MEMORY,        /* allocation failure or an oversized result */
    ERROR_NETWORK,       /* a socket or name-resolution failure */
    ERROR_TIMEOUT,       /* a network operation exceeded its deadline */
    ERROR_RUNTIME        /* any other runtime failure */
} ErrorType;

const char *error_type_name(ErrorType type);

/* Prints "<Type> at line N: message" followed by the offending source line and
 * a caret under `column` (1-based). `source` and the column are optional: when
 * the column is 0 the caret points at the first non-blank character. */
void error_print(ErrorType type, int line, int column, const char *source,
                 const char *message);

#endif