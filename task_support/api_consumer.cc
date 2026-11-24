#include <snmalloc/page_shield.h>
#include "page_observer.h"
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"PAGE_SHIELD_API_FAIL line=%d\n",__LINE__);return 1;}}while(0)
int main(){
 task_input::fixture f;REQUIRE(f.init());REQUIRE(f.retire_remote());
 snmalloc::page_shield_info i{};
 REQUIRE(snmalloc::page_shield_query(*f.owner.alloc,f.page,i));
 REQUIRE(i.page==f.page&&i.slab==f.slab&&i.page_bytes==f.page_bytes&&i.outstanding==0);
 size_t held=snmalloc::page_shield_collect(*f.owner.alloc,128*1024*1024);
 REQUIRE(held>=f.page_bytes&&held<=128*1024*1024);
 REQUIRE(snmalloc::page_shield_query(*f.owner.alloc,f.page,i));
 REQUIRE(i.state==snmalloc::page_shield_state::shielded&&i.outstanding==0);
 REQUIRE(task_input::permissions(f.page)==0);
 REQUIRE(f.live_contents());
 f.owner->flush();REQUIRE(task_input::permissions(f.page)==0);REQUIRE(f.live_contents());
 REQUIRE(snmalloc::page_shield_release(*f.owner.alloc)==held);
 int restored=task_input::permissions(f.page);REQUIRE(restored>=0&&(restored&3)==3);
 REQUIRE(snmalloc::page_shield_collect(*f.owner.alloc,0)==0);
 REQUIRE(f.finish());std::puts("SNMALLOC_PAGE_SHIELD_API_OK");
}
