#include "table.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "object.h"

#define TABLE_MAX_LOAD 0.75

void table_init(Table *table)
{
    table->count = 0;
    table->capacity = 0;
    table->entries = NULL;
}

void table_free(Table *table)
{
    mem_realloc(table->entries, sizeof(TableEntry) * (size_t)table->capacity, 0);
    table_init(table);
}

static TableEntry *table_find_entry(TableEntry *entries, int capacity, ObjString *key)
{
    uint32_t index = key->hash & (uint32_t)(capacity - 1);
    TableEntry *tombstone = NULL;

    for (;;) {
        TableEntry *entry = &entries[index];

        if (entry->key == NULL) {
            if (is_nil(entry->value)) {
                return tombstone ? tombstone : entry;
            }
            if (!tombstone) {
                tombstone = entry;
            }
        } else if (entry->key == key) {
            return entry;
        }
        index = (index + 1) & (uint32_t)(capacity - 1);
    }
}

static void table_adjust_capacity(Table *table, int capacity)
{
    TableEntry *entries = mem_realloc(NULL, 0, sizeof(TableEntry) * (size_t)capacity);
    for (int i = 0; i < capacity; i++) {
        entries[i].key = NULL;
        entries[i].value = NIL_VAL;
    }

    table->count = 0;
    for (int i = 0; i < table->capacity; i++) {
        TableEntry *entry = &table->entries[i];
        if (!entry->key) {
            continue;
        }
        TableEntry *dest = table_find_entry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        table->count++;
    }

    mem_realloc(table->entries, sizeof(TableEntry) * (size_t)table->capacity, 0);
    table->entries = entries;
    table->capacity = capacity;
}

bool table_get(Table *table, ObjString *key, Value *value)
{
    if (table->count == 0) {
        return false;
    }
    TableEntry *entry = table_find_entry(table->entries, table->capacity, key);
    if (!entry->key) {
        return false;
    }
    *value = entry->value;
    return true;
}

bool table_set(Table *table, ObjString *key, Value value)
{
    if (table->count + 1 > (int)(table->capacity * TABLE_MAX_LOAD)) {
        int capacity = table->capacity < 8 ? 8 : table->capacity * 2;
        table_adjust_capacity(table, capacity);
    }

    TableEntry *entry = table_find_entry(table->entries, table->capacity, key);
    bool is_new = entry->key == NULL;
    if (is_new && is_nil(entry->value)) {
        table->count++;
    }
    entry->key = key;
    entry->value = value;
    return is_new;
}

bool table_delete(Table *table, ObjString *key)
{
    if (table->count == 0) {
        return false;
    }
    TableEntry *entry = table_find_entry(table->entries, table->capacity, key);
    if (!entry->key) {
        return false;
    }
    entry->key = NULL;
    entry->value = BOOL_VAL(true); /* tombstone */
    return true;
}
