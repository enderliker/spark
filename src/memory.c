#include "memory.h"

#include <stdio.h>
#include <stdlib.h>

#define GC_MIN_THRESHOLD (1024u * 1024u)

static size_t bytes_allocated = 0;
static size_t gc_threshold = GC_MIN_THRESHOLD;
static int gc_pause_depth = 0;
static int collecting = 0;
static void (*collector)(void) = NULL;

void mem_set_collector(void (*fn)(void))
{
    collector = fn;
}

void *mem_realloc(void *ptr, size_t old_size, size_t new_size)
{
    if (new_size > old_size && collector && !collecting && gc_pause_depth == 0 &&
        bytes_allocated + (new_size - old_size) > gc_threshold) {
        collecting = 1;
        collector();
        collecting = 0;
    }

    if (new_size == 0) {
        free(ptr);
        bytes_allocated = (bytes_allocated >= old_size) ? bytes_allocated - old_size : 0;
        return NULL;
    }

    void *result = realloc(ptr, new_size);
    if (!result) {
        fprintf(stderr, "chispa: out of memory\n");
        exit(70);
    }

    if (new_size >= old_size) {
        bytes_allocated += new_size - old_size;
    } else {
        bytes_allocated -= old_size - new_size;
    }
    return result;
}

size_t mem_allocated(void)
{
    return bytes_allocated;
}

void mem_set_threshold(size_t bytes)
{
    gc_threshold = bytes < GC_MIN_THRESHOLD ? GC_MIN_THRESHOLD : bytes;
}

size_t mem_threshold(void)
{
    return gc_threshold;
}

void mem_pause_gc(void)
{
    gc_pause_depth++;
}

void mem_resume_gc(void)
{
    if (gc_pause_depth > 0) {
        gc_pause_depth--;
    }
}

bool mem_gc_paused(void)
{
    return gc_pause_depth > 0;
}
