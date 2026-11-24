#include <stdint.h>
#include <stddef.h>
#include <sys/mman.h>
#include <stdlib.h>
#define LIMIT 262144
static uint8_t *hits;
static uint32_t next_guard;
void __sanitizer_cov_trace_pc_guard_init(uint32_t *a,uint32_t *b) {
 if(a==b || *a)return;
 while(a<b) {if(next_guard+1>=LIMIT)abort();*a++=++next_guard;}
}
void __sanitizer_cov_trace_pc_guard(uint32_t *g) {
 if(hits && *g<LIMIT)__atomic_store_n(&hits[*g],1,__ATOMIC_RELAXED);
}
void task_cov_start(void) {
 if(hits)return;
 hits=mmap(NULL,LIMIT,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANON,-1,0);
 if(hits==MAP_FAILED)abort();
}
unsigned task_cov_hits(void) {
 unsigned n=0;if(hits)for(unsigned i=1;i<=next_guard;i++)n+=hits[i]!=0;return n;
}
