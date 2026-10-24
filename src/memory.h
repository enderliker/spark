/* memory.h - counted allocation and garbage collector triggering. */
#ifndef CHISPA_MEMORY_H
#define CHISPA_MEMORY_H

#include <stdbool.h>
#include <stddef.h>

/* Reallocate memory and keep track of live bytes.
 * If the threshold is crossed while growing, invoke the registered collector. */
void *mem_realloc(void *ptr, size_t old_size, size_t new_size);

size_t mem_allocated(void);
void mem_set_threshold(size_t bytes);
size_t mem_threshold(void);

void mem_set_collector(void (*collector)(void));
void mem_pause_gc(void);
void mem_resume_gc(void);
bool mem_gc_paused(void);

#endif
