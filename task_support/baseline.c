#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ngtcp2_strm.h"
#include "ngtcp2_rob.h"
static void check(int x) { if(!x){fputs("BASELINE_CHECK_FAILED\n",stderr);abort();} }
typedef struct item { void *p; struct item *next; } item;
typedef struct ledger { item *head; size_t live; } ledger;
static void *alloc(size_t n,void *u) {ledger *l=u;void *p=malloc(n);if(p){item *i=malloc(sizeof(*i));check(i!=NULL);*i=(item){p,l->head};l->head=i;++l->live;}return p;}
static void release(void *p,void *u) {if(!p)return;ledger *l=u;item **q=&l->head;while(*q && (*q)->p!=p)q=&(*q)->next;check(*q!=NULL);item *i=*q;*q=i->next;free(i);--l->live;free(p);}
static void *zero(size_t n,size_t s,void *u) {void *p=alloc(n*s,u);if(p)memset(p,0,n*s);return p;}
static void *resize(void *p,size_t n,void *u) {if(!p)return alloc(n,u);if(!n){release(p,u);return NULL;}ledger *l=u;item *i=l->head;while(i && i->p!=p)i=i->next;check(i!=NULL);void *q=realloc(p,n);if(q)i->p=q;return q;}
static void scenario(size_t quantum) {
  ledger l={0};ngtcp2_mem mem={&l,alloc,release,zero,resize};ngtcp2_objalloc frames;ngtcp2_objalloc_init(&frames,1024,&mem);
  ngtcp2_ksl tree;ngtcp2_ksl_init(&tree,ngtcp2_ksl_uint64_less,ngtcp2_ksl_uint64_less_search,sizeof(uint64_t),&mem);
  for(uint64_t i=0;i<100;++i)check(ngtcp2_ksl_insert(&tree,NULL,&i,NULL)==0);
  check(ngtcp2_ksl_len(&tree)==100);ngtcp2_ksl_it it=ngtcp2_ksl_begin(&tree);for(uint64_t i=0;i<100;++i){check(!ngtcp2_ksl_it_end(&it));check(*(const uint64_t*)ngtcp2_ksl_it_key(&it)==i);ngtcp2_ksl_it_next(&it);}check(ngtcp2_ksl_it_end(&it));ngtcp2_ksl_free(&tree);check(l.live==0);
  ngtcp2_strm s;ngtcp2_strm_init(&s,0,NGTCP2_STRM_FLAG_NONE,32768,32768,NULL,&frames,&mem);
  uint8_t body[16417];for(size_t i=0;i<sizeof(body);++i)body[i]=(uint8_t)('a'+i%26);s.rx.last_offset=sizeof(body);
  check(ngtcp2_strm_recv_reordering(&s,body+8192,sizeof(body)-8192,8192)==(ngtcp2_ssize)(sizeof(body)-8192));check(ngtcp2_strm_rx_offset(&s)==0);
  check(ngtcp2_strm_recv_reordering(&s,body+8192,32,8192)==0);check(ngtcp2_strm_recv_reordering(&s,body,8192,0)==8192);check(ngtcp2_strm_rx_offset(&s)==sizeof(body));
  size_t pos=0;while(pos<sizeof(body)){const uint8_t *p=NULL;size_t n=ngtcp2_rob_data_at(s.rx.rob,&p,pos);check(n>0);if(n>quantum)n=quantum;check(memcmp(p,body+pos,n)==0);pos+=n;ngtcp2_strm_update_rx_offset(&s,pos);}
  ngtcp2_strm_free(&s);ngtcp2_objalloc_free(&frames);check(l.live==0 && l.head==NULL);
  ngtcp2_objalloc_init(&frames,1024,&mem);ngtcp2_strm_init(&s,4,NGTCP2_STRM_FLAG_NONE,32768,32768,NULL,&frames,&mem);s.rx.last_offset=sizeof(body);
  check(ngtcp2_strm_recv_reordering(&s,body+8192,32,8192)==32);ngtcp2_strm_shutdown(&s,NGTCP2_STRM_FLAG_SHUT_RDWR);ngtcp2_strm_free(&s);ngtcp2_objalloc_free(&frames);check(l.live==0 && l.head==NULL);
#ifndef BASELINE_FUZZ
  printf("BASELINE quantum=%zu body=16417 duplicate=0 delivered=%zu canceled=1 live=0\n",quantum,pos);
  puts("BASELINE tree_keys=100 ordered=100 live=0");
#endif
}
#ifdef BASELINE_FUZZ
int LLVMFuzzerTestOneInput(const uint8_t *p,size_t n) {scenario(n && (p[0]&1)?1:64);return 0;}
#else
int main(void) {scenario(1);scenario(64);}
#endif
