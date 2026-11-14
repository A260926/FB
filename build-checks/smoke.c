#include "rpmalloc.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
static void *remote(void*p){rpmalloc_thread_initialize(); rpfree(p); rpmalloc_thread_finalize(); return 0;}
static int errs=0;
static void on_error(const char* m){ printf("CALLBACK: %s\n", m); }
int main(void){
  rpmalloc_interface_t itf = {0}; itf.error_callback = on_error;
  if(rpmalloc_initialize(&itf)) return 1;
  for(int round=0;round<50;round++){
    unsigned char* a = rpmalloc(73);
    void* b = rpaligned_alloc(64,128);
    if(!a||!b||((uintptr_t)b%64)) { printf("alloc fail\n"); return 2; }
    memset(a,0x52,73);
    a = rprealloc(a,900);
    if(!a){printf("realloc fail\n");return 3;}
    for(int i=0;i<73;i++) if(a[i]!=0x52){printf("content corrupt\n");return 4;}
    pthread_t t;
    if(pthread_create(&t,0,remote,b)) return 5;
    pthread_join(t,0);
    rpmalloc_thread_collect();
    rpfree(a);
  }
  /* churn reuse */
  void* keep[200];
  for(int i=0;i<200;i++) keep[i]=rpmalloc(48);
  for(int i=0;i<200;i+=2) rpfree(keep[i]);
  for(int i=0;i<200;i+=2){ void* x=rpmalloc(48); if(!x){printf("reuse fail\n");return 6;} keep[i]=x; }
  for(int i=0;i<200;i++) rpfree(keep[i]);
  rpmalloc_thread_finalize();
  rpmalloc_finalize();
  printf(errs?"SMOKE FAIL\n":"SMOKE OK\n");
  return errs;
}
