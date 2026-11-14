#pragma once
#include <cstdint>
#include <vector>
#include <cstring>
namespace sample {
inline void u16(std::vector<std::uint8_t>&b,size_t o,std::uint16_t v){b.at(o)=v;b.at(o+1)=v>>8;}
inline void u32(std::vector<std::uint8_t>&b,size_t o,std::uint32_t v){for(int i=0;i<4;++i)b.at(o+i)=v>>(8*i);}
inline void u64(std::vector<std::uint8_t>&b,size_t o,std::uint64_t v){for(int i=0;i<8;++i)b.at(o+i)=v>>(8*i);}
inline std::vector<std::uint8_t> pe(){
 std::vector<std::uint8_t>b(0xc00,0);u16(b,0,0x5a4d);u32(b,0x3c,0x80);u32(b,0x80,0x4550);
 u16(b,0x84,0x8664);u16(b,0x86,3);u16(b,0x94,240);u16(b,0x96,0x22);
 size_t o=0x98;u16(b,o,0x20b);u32(b,o+16,0x1000);u32(b,o+20,0x1000);u64(b,o+24,0x140000000ULL);
 u32(b,o+32,0x1000);u32(b,o+36,0x200);u32(b,o+56,0x4000);u32(b,o+60,0x200);u16(b,o+68,3);u32(b,o+108,16);
 u32(b,o+112+3*8,0x2000);u32(b,o+116+3*8,12);
 const char*names[]={".text",".pdata",".xdata"};
 for(size_t i=0;i<3;++i){size_t s=0x188+i*40;std::memcpy(b.data()+s,names[i],std::strlen(names[i]));u32(b,s+8,i==2?0x600:0x200);u32(b,s+12,0x1000*(i+1));u32(b,s+16,i==2?0x600:0x200);u32(b,s+20,0x200*(i+1));u32(b,s+36,i?0x40000040:0x60000020);}
 b[0x200]=0xc3;u32(b,0x400,0x1000);u32(b,0x404,0x1080);u32(b,0x408,0x3000);
 // UNWIND_INFO v1, EHANDLER, no unwind codes; handler and EH3 payload follow.
 b[0x600]=9;u32(b,0x604,0x1070);u32(b,0x608,0x3040);
 // FuncInfo: two states, one try block and two IP transitions.
 u32(b,0x640,0x19930522);u32(b,0x644,2);u32(b,0x648,0x3080);u32(b,0x64c,1);u32(b,0x650,0x30a0);u32(b,0x654,2);u32(b,0x658,0x30e0);u32(b,0x664,1);
 u32(b,0x680,0xffffffff);u32(b,0x684,0);u32(b,0x688,0);u32(b,0x68c,0);
 u32(b,0x6a0,0);u32(b,0x6a4,0);u32(b,0x6a8,1);u32(b,0x6ac,1);u32(b,0x6b0,0x30c0);
 u32(b,0x6cc,0x1060);u32(b,0x6d0,0x20);
 u32(b,0x6e0,0x1000);u32(b,0x6e4,0);u32(b,0x6e8,0x1060);u32(b,0x6ec,1);
 return b;
}
}
