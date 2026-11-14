#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "PDB_Types.h"
namespace fixture {
using Bytes=std::vector<unsigned char>;
inline void u16(Bytes& b,uint16_t x){b.push_back(x&255);b.push_back(x>>8);}
inline void u32(Bytes& b,uint32_t x){for(int i=0;i<4;++i)b.push_back((x>>(8*i))&255);}
inline void put32(Bytes& b,size_t p,uint32_t x){for(int i=0;i<4;++i)b.at(p+i)=(x>>(8*i))&255;}
inline void align4(Bytes& b){while(b.size()%4)b.push_back(0);}
inline size_t record(Bytes& b,uint16_t kind,const Bytes& data){
 size_t off=b.size();u16(b,uint16_t(data.size()+2));u16(b,kind);
 b.insert(b.end(),data.begin(),data.end());align4(b);return off;
}
inline void subsection(Bytes& b,uint32_t kind,const Bytes& data){
 u32(b,kind);u32(b,uint32_t(data.size()));b.insert(b.end(),data.begin(),data.end());align4(b);
}
// A small MSF7 container. Stream pages are separated by unused pages.
inline Bytes msf(const std::vector<Bytes>& streams){
 constexpr uint32_t bs=512;Bytes dir;u32(dir,uint32_t(streams.size()));
 for(auto& s:streams)u32(dir,uint32_t(s.size()));
 uint32_t next=5;std::vector<std::vector<uint32_t>> pages;
 for(auto& s:streams){pages.emplace_back();for(size_t p=0;p<s.size();p+=bs){pages.back().push_back(next);u32(dir,next);next+=2;}}
 if(dir.size()>bs)throw std::runtime_error("fixture directory too large");
 Bytes image(next*bs,0);std::memcpy(image.data(),PDB::SuperBlock::MAGIC,30);
 put32(image,32,bs);put32(image,36,1);put32(image,40,next);put32(image,44,uint32_t(dir.size()));put32(image,52,4);put32(image,4*bs,3);
 std::copy(dir.begin(),dir.end(),image.begin()+3*bs);
 for(size_t i=0;i<streams.size();++i)for(size_t j=0;j<pages[i].size();++j){
  size_t n=std::min<size_t>(bs,streams[i].size()-j*bs);
  std::copy_n(streams[i].begin()+j*bs,n,image.begin()+pages[i][j]*bs);
 }
 return image;
}
struct Normal { Bytes image;uint32_t symbols,c13; };
inline Normal normal(){
 Bytes module;u32(module,4);Bytes p;for(int i=0;i<7;++i)u32(p,i==3?64:0);
 u32(p,0x100);u16(p,1);p.push_back(0);p.insert(p.end(),{'m','a','i','n',0});size_t proc=record(module,0x1110,p);
 Bytes in;u32(in,uint32_t(proc));u32(in,0);u32(in,0x1000);in.insert(in.end(),{3,16,4,8,0});
 size_t site=record(module,0x114d,in);size_t siteEnd=record(module,0x114e,{});size_t procEnd=record(module,6,{});
 put32(module,proc+8,uint32_t(procEnd));put32(module,site+8,uint32_t(siteEnd));
 uint32_t symbols=uint32_t(module.size());Bytes il;u32(il,0);u32(il,0x1000);u32(il,0);u32(il,41);subsection(module,0xf6,il);
 Bytes ck;u32(ck,1);ck.push_back(0);ck.push_back(0);align4(ck);subsection(module,0xf4,ck);
 uint32_t c13=uint32_t(module.size())-symbols;Bytes names;u32(names,0xeffeeffe);u32(names,1);u32(names,11);
 const unsigned char strings[]={0,'s','o','u','r','c','e','.','c','c',0};names.insert(names.end(),std::begin(strings),std::end(strings));
 return {msf({module,names}),symbols,c13};
}
}
