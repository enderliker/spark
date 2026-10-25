/* json.h - JSON encoder and decoder for Spark. */
#ifndef SPARK_JSON_H
#define SPARK_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "object.h"
#include "value.h"

/* Serializes a Spark value to a JSON string. Returns NULL and writes to error on failure. */
ObjString *json_encode(Value value, char *error, size_t error_size);

/* Deserializes a JSON string into Spark values. Returns NIL_VAL and writes to error on failure. */
Value json_decode(const char *text, int length, char *error, size_t error_size);

#endif
