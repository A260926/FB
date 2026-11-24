#include "mimalloc/qsbr-runtime.h"
#include <stdint.h>
#include <stdio.h>
static bool area(const mi_heap_t* h,const mi_heap_area_t* a,void* b,size_t z,void* arg) {
  (void)h;(void)a;(void)b;(void)z; ++*(size_t*)arg; return true;
}
static size_t pages(mi_heap_t* h) { size_t n=0; if(!mi_heap_visit_blocks(h,false,area,&n)) return SIZE_MAX; return n; }
#define REQUIRE(x) do { if(!(x)) { fprintf(stderr,"API_CHECK:%d:%s\n",__LINE__,#x);return 1; } }while(0)
int main(void) {
  REQUIRE(mi_qsbr_domain_new(0)==NULL);
  mi_qsbr_domain_t* d=mi_qsbr_domain_new(1); REQUIRE(d!=NULL);
  mi_qsbr_reader_t* r=mi_qsbr_register(d); REQUIRE(r!=NULL);
  REQUIRE(mi_qsbr_register(d)==NULL);
  mi_heap_t* h=mi_qsbr_heap_new(d); REQUIRE(h!=NULL);
  unsigned char* p=mi_heap_malloc(h,4*1024*1024); REQUIRE(p!=NULL);
  p[0]=7; p[4*1024*1024-1]=9; REQUIRE(p[0]+p[4*1024*1024-1]==16);
  REQUIRE(mi_qsbr_enter(r)); REQUIRE(mi_qsbr_enter(r));
  mi_free(p); mi_heap_collect(h,true);
  uint64_t before=mi_qsbr_poll(d); REQUIRE(pages(h)==1);
  REQUIRE(!mi_qsbr_unregister(r)); REQUIRE(!mi_qsbr_heap_destroy(d,h));
  REQUIRE(mi_qsbr_leave(r)); mi_qsbr_poll(d); mi_heap_collect(h,true); REQUIRE(pages(h)==1);
  REQUIRE(mi_qsbr_leave(r)); REQUIRE(!mi_qsbr_leave(r));
  REQUIRE(mi_qsbr_poll(d)>before); mi_heap_collect(h,true); REQUIRE(pages(h)==0);
  REQUIRE(!mi_qsbr_domain_delete(d));
  REQUIRE(mi_qsbr_heap_destroy(d,h)); REQUIRE(mi_qsbr_unregister(r)); REQUIRE(mi_qsbr_domain_delete(d));
  puts("QSBR_API_CONSUMER_OK"); return 0;
}
