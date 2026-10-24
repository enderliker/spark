#include "ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_BLOCK_SIZE 8192

void arena_init(Arena *arena)
{
    arena->head = NULL;
}

void *arena_alloc(Arena *arena, size_t size)
{
    size_t aligned = (size + 7u) & ~(size_t)7u;
    ArenaBlock *block = arena->head;

    if (!block || block->capacity - block->used < aligned) {
        size_t capacity = aligned > ARENA_BLOCK_SIZE ? aligned : ARENA_BLOCK_SIZE;
        ArenaBlock *fresh = malloc(sizeof(ArenaBlock) + capacity);

        if (!fresh) {
            fprintf(stderr, "spark: out of memory while building the AST\n");
            exit(70);
        }
        fresh->next = arena->head;
        fresh->used = 0;
        fresh->capacity = capacity;
        arena->head = fresh;
        block = fresh;
    }

    void *result = block->data + block->used;
    block->used += aligned;
    memset(result, 0, size);
    return result;
}

char *arena_strndup(Arena *arena, const char *chars, int length)
{
    char *copy = arena_alloc(arena, (size_t)length + 1);

    memcpy(copy, chars, (size_t)length);
    copy[length] = '\0';
    return copy;
}

void arena_free(Arena *arena)
{
    ArenaBlock *block = arena->head;

    while (block) {
        ArenaBlock *next = block->next;

        free(block);
        block = next;
    }
    arena->head = NULL;
}
