#include "tlsf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void walk(void*p,size_t n,int used,void*u){(void)p;(void)n;if(used)++*(int*)u;}
int main(void){void*c=malloc(tlsf_size());void*m=malloc(32768);tlsf_t t=tlsf_create(c);pool_t pool=tlsf_add_pool(t,m,32768);void*a=tlsf_malloc(t,37);void*b=tlsf_memalign(t,64,90);memset(a,0x31,37);a=tlsf_realloc(t,a,300);if(!a||!b)return 2;for(int i=0;i<37;i++)if(((unsigned char*)a)[i]!=0x31)return 3;int n=0;tlsf_walk_pool(pool,walk,&n);if(n!=2||tlsf_check(t)||tlsf_check_pool(pool))return 4;printf("TLSF live=%d preserved=37 check=0\n",n);tlsf_free(t,a);tlsf_free(t,b);tlsf_remove_pool(t,pool);tlsf_destroy(t);free(m);free(c);return 0;}
