#include "rpmalloc.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
static void *remote(void*p){rpmalloc_thread_initialize();rpfree(p);rpmalloc_thread_finalize();return 0;}
int main(void){if(rpmalloc_initialize(0))return 1;unsigned char*a=rpmalloc(73);void*b=rpaligned_alloc(64,128);if(!a||!b||((uintptr_t)b%64))return 2;memset(a,0x52,73);a=rprealloc(a,900);if(!a)return 3;for(int i=0;i<73;i++)if(a[i]!=0x52)return 4;pthread_t t;if(pthread_create(&t,0,remote,b))return 5;pthread_join(t,0);rpmalloc_thread_collect();rpfree(a);rpmalloc_thread_finalize();rpmalloc_finalize();puts("RPMALLOC preserved=73 aligned=64 remote=joined");return 0;}
