#include "iso_alloc.h"
#include <string.h>
#include <stdio.h>
#define OK(x) do { if(!(x)) return __LINE__; } while(0)
int main(void) {
    iso_alloc_initialize();
    unsigned char *p=iso_alloc(48);
    OK(p); size_t sz=iso_chunksz(p); OK(sz>=48);
    memset(p,0x5a,48);
    unsigned char *q=iso_realloc(p,192);
    OK(q); for(size_t i=0;i<48;i++) OK(q[i]==0x5a);
    iso_free_size(q,192);
    iso_alloc_zone_handle *a=iso_alloc_new_zone(64), *b=iso_alloc_new_zone(128);
    OK(a && b);
    void *x=iso_alloc_from_zone(a),*y=iso_alloc_from_zone(b); OK(x && y);
    OK(iso_alloc_detect_zone_leaks(a)==1 && iso_alloc_detect_zone_leaks(b)==1);
    iso_free_from_zone(x,a); iso_free_from_zone(y,b); iso_flush_caches();
    OK(iso_alloc_detect_zone_leaks(a)==0 && iso_alloc_detect_zone_leaks(b)==0);
    iso_alloc_destroy_zone(a); iso_alloc_destroy_zone(b);
    puts("ISOALLOC_HEALTHY_REALLOC_ZONES_OK");
    return 0;
}
