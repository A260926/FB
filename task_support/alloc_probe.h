#ifndef TASK_ALLOC_PROBE_H
#define TASK_ALLOC_PROBE_H
#include <stddef.h>
void task_alloc_fail_after(long successful_allocations);
void task_alloc_counters_reset(void);
size_t task_alloc_calls(void);
size_t task_free_calls(void);
void *task_malloc(size_t n);
void *task_calloc(size_t n,size_t size);
void *task_realloc(void *p,size_t n);
void task_free(void *p);
#endif
