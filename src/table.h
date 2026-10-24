/* table.h - hash table for global variables. */
#ifndef SPARK_TABLE_H
#define SPARK_TABLE_H

#include "value.h"

typedef struct {
    ObjString *key;
    Value value;
} TableEntry;

typedef struct {
    int count;
    int capacity;
    TableEntry *entries;
} Table;

void table_init(Table *table);
void table_free(Table *table);
bool table_get(Table *table, ObjString *key, Value *value);
bool table_set(Table *table, ObjString *key, Value value);
bool table_delete(Table *table, ObjString *key);

#endif
