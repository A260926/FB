#include "page_observer.h"
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"PAGE_BASELINE_FAIL line=%d\n",__LINE__);return 1;}}while(0)
int main(){
 task_input::fixture f;REQUIRE(f.init());REQUIRE(f.retire_remote());REQUIRE(f.live_contents());
 int empty_perm=task_input::permissions(f.page),live_perm=task_input::permissions(f.survivor);
 // Healthy invariant: the live sibling remains RW. Empty-page permissions are an observation, not a required old defect.
 REQUIRE(live_perm>=0&&(live_perm&3)==3);REQUIRE(empty_perm>=0);
 std::printf("PAGE_OBSERVATION page_bytes=%zu slab_bytes=%zu returned_objects=%zu empty_page_perm=%d live_sibling_perm=%d\n",f.page_bytes,f.slab_bytes,f.retired,empty_perm,live_perm);
 REQUIRE(f.finish());std::puts("SNMALLOC_PARTIAL_SLAB_HEALTHY_OK");
}
