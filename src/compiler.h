/* compiler.h - compilation of the AST to bytecode. */
#ifndef SPARK_COMPILER_H
#define SPARK_COMPILER_H

#include <stdbool.h>
#include <stddef.h>

#include "ast.h"
#include "object.h"

/* Compiles the whole program and returns the main function.
 * On error it returns NULL and fills in `error`, `error_line` and
 * `error_column` (0 when the column is unknown). */
ObjFunction *compile_ast(Stmt *program, const char *name, const char *source, bool repl_mode,
                         char *error, size_t error_size, int *error_line, int *error_column);

#endif
