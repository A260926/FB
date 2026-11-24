#include <stdint.h>
#include <stdbool.h>
struct fake_domain;
struct mi_page_qsbr_s { uint64_t goal; };
struct mi_heap_qsbr_s { struct fake_domain* domain; };
#define MI_QSBR 1
#include "src/static.c"
struct fake_domain { uint64_t requested, completed; };
static struct fake_domain fake;
void mi_heap_qsbr_init(mi_heap_qsbr_t* h) { h->domain=&fake; }
void mi_page_qsbr_clear(mi_page_qsbr_t* p) { p->goal=0; }
bool mi_page_qsbr_is_clear(const mi_page_qsbr_t* p) { return p->goal==0; }
bool mi_page_qsbr_should_delay(mi_page_qsbr_t* p, mi_heap_qsbr_t* h, size_t bytes) {
  (void)bytes;
  if (!p->goal) p->goal=++h->domain->requested;
  return p->goal>h->domain->completed;
}
static bool area(const mi_heap_t* h,const mi_heap_area_t* a,void* b,size_t n,void* arg) {
  (void)h;(void)a;(void)b;(void)n; ++*(size_t*)arg; return true;
}
static size_t pages(mi_heap_t* h) { size_t n=0; mi_heap_visit_blocks(h,false,area,&n);return n; }
int main(void) {
  mi_heap_t* h=mi_heap_new_qsbr(); if(!h) return 1;
  unsigned char* p=mi_heap_malloc(h,4*1024*1024); if(!p)return 2;
  p[0]=17; p[4*1024*1024-1]=29; unsigned sum=p[0]+p[4*1024*1024-1];
  mi_free(p); mi_heap_collect(h,true);
  size_t retained=pages(h); uint64_t goal=fake.requested;
  fake.completed=fake.requested; mi_heap_collect(h,true); size_t released=pages(h);
  mi_heap_destroy(h);
  printf("QSBR_BASELINE checksum=%u retained=%zu released=%zu goal=%llu\n",sum,retained,released,(unsigned long long)goal);
  return sum==46 && retained==1 && released==0 && goal>0 ? 0:3;
}
