#include "iso_alloc.h"
#include <stdio.h>
#define REQUIRE(x) do { if(!(x)) {fprintf(stderr,"API_CONSUMER_FAIL line=%d\n",__LINE__);return 1;} } while(0)
int main(void) {
    iso_alloc_initialize();
    REQUIRE(iso_quarantine_configure(2,4096)==0);
    void *p=iso_alloc(32); REQUIRE(p); size_t bytes=iso_chunksz(p); iso_free(p);
    iso_quarantine_stats s;
    REQUIRE(iso_quarantine_snapshot(0,&s)==0 && s.pending_chunks==1 && s.pending_bytes==bytes);
    uint64_t t=iso_quarantine_detach(); REQUIRE(t);
    REQUIRE(iso_quarantine_snapshot(t,&s)==0 && s.pending_chunks==1);
    REQUIRE(iso_quarantine_configure(2,4096)==0 && iso_quarantine_adopt(t)==0);
    REQUIRE(iso_quarantine_snapshot(0,&s)==0 && s.pending_chunks==1);
    iso_flush_caches();
    REQUIRE(iso_quarantine_snapshot(0,&s)==0 && s.pending_chunks==0 && s.released_flush==1);
    REQUIRE(iso_quarantine_discard(t)==-1);
    iso_alloc_zone_handle *a=iso_alloc_new_zone(64), *b=iso_alloc_new_zone(128);
    REQUIRE(a && b);
    void *x=iso_alloc_from_zone(a); REQUIRE(x);
    REQUIRE(iso_alloc_detect_zone_leaks(a)==1);
    iso_free_from_zone(x,a);
    REQUIRE(iso_alloc_detect_zone_leaks(a)==1);
    uint64_t zone_ticket=iso_quarantine_detach(); REQUIRE(zone_ticket);
    iso_alloc_destroy_zone(b);
    REQUIRE(iso_alloc_detect_zone_leaks(a)==1);
    REQUIRE(iso_quarantine_discard(zone_ticket)==0);
    REQUIRE(iso_alloc_detect_zone_leaks(a)==0);
    iso_alloc_destroy_zone(a);
    puts("QUARANTINE_TRANSFER_API_OK");
    return 0;
}
