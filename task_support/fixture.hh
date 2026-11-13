#ifndef SEED_FIXTURE_HH
#define SEED_FIXTURE_HH
#include <vector>
#include <cstring>
#include <memory>
#include <stdexcept>
#include "elf++.hh"
struct seed_memory : elf::loader {
 std::vector<unsigned char> bytes;
 const void *load(off_t off,size_t n) override {
  if(off<0||size_t(off)>bytes.size()||n>bytes.size()-size_t(off))throw std::range_error("seed bounds");
  return bytes.data()+off;
 }
};
static void seed_put(std::vector<unsigned char>&b,size_t o,uint64_t v,unsigned n){for(unsigned i=0;i<n;i++)b[o+i]=static_cast<unsigned char>(v>>(8*i));}
static std::shared_ptr<seed_memory> seed_elf(){
 auto m=std::make_shared<seed_memory>();auto&b=m->bytes;b.resize(512,0);
 const char names[]="\0.shstrtab\0.debug_info\0.debug_abbrev\0";
 std::memcpy(b.data()+64,names,sizeof names);
 unsigned char info[]={21,0,0,0,4,0,0,0,0,0,8,1,0,0,0,0,0,0,0,0,'s','e','e','d',0};
 unsigned char abbrev[]={1,0x11,0,0x11,1,3,8,0,0,0};
 std::memcpy(b.data()+128,info,sizeof info);std::memcpy(b.data()+160,abbrev,sizeof abbrev);
 b[0]=0x7f;b[1]='E';b[2]='L';b[3]='F';b[4]=2;b[5]=1;b[6]=1;
 seed_put(b,16,1,2);seed_put(b,18,62,2);seed_put(b,20,1,4);seed_put(b,40,256,8);
 seed_put(b,52,64,2);seed_put(b,58,64,2);seed_put(b,60,4,2);seed_put(b,62,1,2);
 const unsigned ns[]={0,1,11,23},ofs[]={0,64,128,160},sz[]={0,sizeof names,sizeof info,sizeof abbrev};
 for(unsigned i=1;i<4;i++){size_t o=256+64*i;seed_put(b,o,ns[i],4);seed_put(b,o+4,i==1?3:1,4);seed_put(b,o+24,ofs[i],8);seed_put(b,o+32,sz[i],8);seed_put(b,o+48,1,8);}
 return m;
}
#endif
