#include "tlsf.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdalign.h>
static alignas(1024) unsigned char arena[65536];
int main(void) {
 tlsf_t t=tlsf_create_with_pool(arena,sizeof(arena));
 unsigned char *p=tlsf_malloc(t,71);
 assert(t && p); memset(p,0x5a,71);
 for(int i=0;i<71;i++) assert(p[i]==0x5a);
 tlsf_free(t,p); assert(tlsf_check(t)==0);
 assert(tlsf_check_pool(tlsf_get_pool(t))==0);
 tlsf_destroy(t); puts("TLSF_BASELINE_OK alloc-write-free=71");
 return 0;
}
