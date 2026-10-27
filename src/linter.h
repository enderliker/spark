/* linter.h - static analysis linter for the Spark language. */
#ifndef SPARK_LINTER_H
#define SPARK_LINTER_H

#include <stdbool.h>

/* Runs the linter on the given source code and prints findings to stdout.
 * Returns the number of warnings (0 if clean, >0 if findings),
 * or -1 if there was a syntax error. */
int linter_lint(const char *source, const char *path);

#endif
