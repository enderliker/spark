/* bytecode.h - serialization and deserialization of compiled Spark bytecode (.sparkc). */
#ifndef SPARK_BYTECODE_H
#define SPARK_BYTECODE_H

#include <stdbool.h>
#include <stddef.h>

#include "object.h"

#define SPARK_BYTECODE_MAGIC "SPRK"
#define SPARK_BYTECODE_VERSION 2

/* Serializes `function` to disk at `path`.
 * Returns true on success, or false on error (with message in `error`). */
bool bytecode_save(const ObjFunction *function, const char *path, char *error, size_t error_size);

/* Loads an ObjFunction from a precompiled bytecode file at `path`.
 * Returns the function on success, or NULL on error (with message in `error`). */
ObjFunction *bytecode_load(const char *path, char *error, size_t error_size);

/* Returns true if the file starts with the Spark bytecode magic "SPRK". */
bool bytecode_is_bytecode_file(const char *path);

#endif
