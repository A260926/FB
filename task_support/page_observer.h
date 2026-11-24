#pragma once
#include <snmalloc/snmalloc.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
# include <mach/mach.h>
# include <mach/mach_vm.h>
#endif
namespace task_input {
// Only asks the OS about mappings; never probes accessibility by dereference.
inline int permissions(uintptr_t at) {
#ifdef __APPLE__
 mach_vm_address_t a=at;mach_vm_size_t n=0;vm_region_basic_info_data_64_t i{};
 mach_msg_type_number_t c=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t o=MACH_PORT_NULL;
 auto k=mach_vm_region(mach_task_self(),&a,&n,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&i,&c,&o);
 if(o!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),o);
 if(k!=KERN_SUCCESS||a>at||at-a>=n)return -1;
 return ((i.protection&VM_PROT_READ)?1:0)|((i.protection&VM_PROT_WRITE)?2:0)|((i.protection&VM_PROT_EXECUTE)?4:0);
#elif defined(__linux__)
 FILE* f=std::fopen("/proc/self/maps","r");if(!f)return -1;
 char line[1024],mode[5];unsigned long long lo,hi;int result=-1;
 while(std::fgets(line,sizeof line,f))if(std::sscanf(line,"%llx-%llx %4s",&lo,&hi,mode)==3&&at>=lo&&at<hi){
  result=(mode[0]=='r'?1:0)|(mode[1]=='w'?2:0)|(mode[2]=='x'?4:0);break;
 }
 std::fclose(f);return result;
#else
# error OS permission observer requires macOS or Linux
#endif
}
struct fixture {
 snmalloc::ScopedAllocator<> owner;
 std::vector<void*> objects;
 uintptr_t slab=0,page=0,survivor=0;
 size_t page_bytes=size_t(sysconf(_SC_PAGESIZE)),slab_bytes=0,usable=0,retired=0;
 bool init() {
  auto sc=snmalloc::size_to_sizeclass(3072);slab_bytes=snmalloc::sizeclass_to_slab_size(sc);
  usable=snmalloc::sizeclass_to_size(sc);
  if(page_bytes==0||page_bytes>=slab_bytes)return false;
  std::map<uintptr_t,std::vector<size_t>> groups;
  for(size_t j=0;j<2048;j++){
   void* p=owner->alloc(3072);if(!p)return false;
   std::memset(p,0x39,3072);objects.push_back(p);
   groups[uintptr_t(p)&~uintptr_t(slab_bytes-1)].push_back(j);
  }
  for(auto& [base,ids]:groups){
   if(ids.size()!=snmalloc::sizeclass_to_slab_object_count(sc))continue;
   uintptr_t candidate=base+page_bytes;
   for(auto id:ids){auto a=uintptr_t(objects[id]);if(a+usable<=candidate||a>=candidate+page_bytes){
    slab=base;page=candidate;survivor=a;return true;
   }}
  }
  return false;
 }
 bool touches_page(void* p)const {auto a=uintptr_t(p);return a<page+page_bytes&&page<a+usable;}
 bool retire_remote() {
  if(!page)return false;
  std::thread worker([&]{auto peer=snmalloc::get_scoped_allocator<>();
   for(auto& p:objects)if(p&&touches_page(p)){peer->dealloc(p);p=nullptr;retired++;}
   peer->flush();
  });worker.join();owner->flush();return retired>0;
 }
 bool live_contents()const {
  for(auto p:objects)if(p){auto b=static_cast<const unsigned char*>(p);for(size_t i=0;i<3072;i++)if(b[i]!=0x39)return false;}
  int live_perm=permissions(survivor);return survivor&&live_perm>=0&&((live_perm&3)==3);
 }
 bool finish(){for(auto& p:objects)if(p){owner->dealloc(p);p=nullptr;}owner->flush();bool empty=true;owner->debug_is_empty(&empty);return empty;}
 ~fixture(){for(auto p:objects)if(p)owner->dealloc(p);owner->flush();}
};
}
