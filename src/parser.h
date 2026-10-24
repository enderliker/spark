/* parser.h - Chispa syntax analysis. */
#ifndef CHISPA_PARSER_H
#define CHISPA_PARSER_H

#include <stdbool.h>
#include <stddef.h>

#include "ast.h"

/* Parses the code and leaves the program (a block) in *out.
 * Returns false and fills in `error`/`error_line` if there is a syntax error. */
bool parse_source(const char *source, Arena *arena, Stmt **out,
                  char *error, size_t error_size, int *error_line);

#endif
