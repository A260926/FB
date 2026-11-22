#include "alloc_probe.h"
#include <stdlib.h>
static long remaining=-1;
static size_t allocations,frees;
void task_alloc_fail_after(long n){remaining=n;}
void task_alloc_counters_reset(void){allocations=frees=0;}
size_t task_alloc_calls(void){return allocations;}
size_t task_free_calls(void){return frees;}
static int reject(void){++allocations;if(remaining<0)return 0;if(remaining==0)return 1;--remaining;return 0;}
void *task_malloc(size_t n){return reject()?NULL:malloc(n);}
void *task_calloc(size_t n,size_t size){return reject()?NULL:calloc(n,size);}
void *task_realloc(void *p,size_t n){return reject()?NULL:realloc(p,n);}
void task_free(void *p){++frees;free(p);}
