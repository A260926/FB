#include <stdlib.h>
void *sft_real_malloc(size_t n){ return malloc(n); }
void *sft_real_calloc(size_t n, size_t s){ return calloc(n, s); }
void *sft_real_realloc(void *p, size_t n){ return realloc(p, n); }
void sft_real_free(void *p){ free(p); }
