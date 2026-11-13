#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
inline void put(std::vector<std::uint8_t>& b,size_t o,std::uint64_t v,size_t n) {
 for(size_t i=0;i<n;i++)b.at(o+i)=std::uint8_t(v>>(8*i));
}
inline std::vector<std::uint8_t> small_pe(bool x64) {
 std::vector<std::uint8_t> b(0x800);const size_t nt=0x80,opt=nt+24,os=x64?240:224,sec=opt+os;
 put(b,0,0x5a4d,2);put(b,0x3c,nt,4);put(b,nt,0x4550,4);
 put(b,nt+4,x64?0x8664:0x14c,2);put(b,nt+6,1,2);put(b,nt+20,os,2);put(b,nt+22,0x102,2);
 put(b,opt,x64?0x20b:0x10b,2);put(b,opt+16,0x1000,4);
 put(b,opt+(x64?24:28),x64?0x140000000ULL:0x400000,x64?8:4);
 put(b,opt+32,0x1000,4);put(b,opt+36,0x200,4);put(b,opt+56,0x2000,4);put(b,opt+60,0x200,4);
 put(b,opt+68,3,2);put(b,opt+70,0x4000,2);put(b,opt+(x64?108:92),16,4);
 size_t dir=opt+(x64?112:96)+10*8;put(b,dir,0x1100,4);put(b,dir+4,x64?148:92,4);
 std::memcpy(b.data()+sec,".text",5);put(b,sec+8,0x600,4);put(b,sec+12,0x1000,4);put(b,sec+16,0x600,4);put(b,sec+20,0x200,4);put(b,sec+36,0x60000020,4);b[0x200]=0xc3;
 size_t cfg=0x300;put(b,cfg,x64?148:92,4);const auto base=x64?0x140000000ULL:0x400000;
 put(b,cfg+(x64?128:80),base+0x1200,x64?8:4);put(b,cfg+(x64?136:84),2,x64?8:4);put(b,cfg+(x64?144:88),0x10000500,4);
 put(b,0x400,0x1000,4);b[0x404]=0;put(b,0x405,0x1017,4);b[0x409]=1;
 return b;
}
