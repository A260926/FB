#ifndef TEXT_PATH_FONT_FIXTURE_HPP
#define TEXT_PATH_FONT_FIXTURE_HPP
#include "lunasvg.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
// Original test-only outlines, dedicated to CC0. No system fonts or font files.
// Units/em=1000, ascent=800, descent=-200. A: advance=600, [0,0,400,700].
// B: advance=800, [50,0,650,700]. Space: advance=250, no outline.
namespace text_path_fixture {
using Bytes = std::vector<unsigned char>;
inline void u16(Bytes& b, unsigned x) { b.push_back(x>>8);b.push_back(x); }
inline void u32(Bytes& b, std::uint32_t x) { u16(b,x>>16);u16(b,x); }
inline void put16(Bytes& b, size_t i,unsigned x) { b.at(i)=x>>8;b.at(i+1)=x; }
inline void put32(Bytes& b,size_t i,std::uint32_t x) { put16(b,i,x>>16);put16(b,i+2,x); }
inline Bytes rectangle(int left,int right) {
 Bytes b;u16(b,1);u16(b,left);u16(b,0);u16(b,right);u16(b,700);
 u16(b,3);u16(b,0); // endpoint and instruction length
 for(int i=0;i<4;i++)b.push_back(1); // on-curve, signed 16-bit deltas
 for(int x : {left,right-left,0,left-right})u16(b,x);
 for(int y : {0,0,700,0})u16(b,y);
 while(b.size()%4)b.push_back(0);return b;
}
inline Bytes make_font() {
 struct Table { const char* tag; Bytes data; };
 Bytes head(54);put32(head,0,0x10000);put32(head,12,0x5f0f3cf5);put16(head,18,1000);
 put16(head,36,0);put16(head,38,0);put16(head,40,650);put16(head,42,700);put16(head,50,1);
 Bytes hhea(36);put32(hhea,0,0x10000);put16(hhea,4,800);put16(hhea,6,-200);put16(hhea,10,800);put16(hhea,34,3);
 Bytes maxp(32);put32(maxp,0,0x10000);put16(maxp,4,3);put16(maxp,6,4);put16(maxp,8,1);
 Bytes hmtx;for(int a : {250,600,800}){u16(hmtx,a);u16(hmtx,a==800?50:0);}
 auto a=rectangle(0,400),b=rectangle(50,650);Bytes glyf=a;glyf.insert(glyf.end(),b.begin(),b.end());
 Bytes loca;for(unsigned x : {0u,0u,unsigned(a.size()),unsigned(glyf.size())})u32(loca,x);
 Bytes cmap;u16(cmap,0);u16(cmap,1);u16(cmap,3);u16(cmap,1);u32(cmap,12);
 for(unsigned x : {4u,32u,0u,4u,4u,1u,0u,66u,65535u,0u,65u,65535u,65472u,1u,0u,0u})u16(cmap,x);
 std::vector<Table> tables={{"cmap",cmap},{"glyf",glyf},{"head",head},{"hhea",hhea},{"hmtx",hmtx},{"loca",loca},{"maxp",maxp}};
 Bytes out;u32(out,0x10000);u16(out,tables.size());u16(out,64);u16(out,2);u16(out,48);out.resize(12+16*tables.size());
 size_t adjustment=0;
 for(size_t i=0;i<tables.size();i++) {
  auto& t=tables[i];size_t offset=out.size();Bytes padded=t.data;while(padded.size()%4)padded.push_back(0);
  std::uint32_t sum=0;for(size_t j=0;j<padded.size();j+=4)sum+=(std::uint32_t(padded[j])<<24)|(std::uint32_t(padded[j+1])<<16)|(std::uint32_t(padded[j+2])<<8)|padded[j+3];
  size_t d=12+16*i;for(int k=0;k<4;k++)out[d+k]=t.tag[k];put32(out,d+4,sum);put32(out,d+8,offset);put32(out,d+12,t.data.size());
  if(std::string(t.tag)=="head")adjustment=offset+8;
  out.insert(out.end(),padded.begin(),padded.end());
 }
 std::uint32_t sum=0;for(size_t j=0;j<out.size();j+=4)sum+=(std::uint32_t(out[j])<<24)|(std::uint32_t(out[j+1])<<16)|(std::uint32_t(out[j+2])<<8)|out[j+3];
 put32(out,adjustment,0xb1b0afba-sum);return out;
}
inline bool install() { static const auto font=make_font();return lunasvg_add_font_face_from_data("Fixture",false,false,font.data(),font.size(),nullptr,nullptr); }
inline unsigned alpha(const lunasvg::Bitmap& b,int x,int y) {
 std::uint32_t px;std::copy_n(b.data()+y*b.stride()+x*4,4,reinterpret_cast<unsigned char*>(&px));return px>>24;
}
inline unsigned covered(const lunasvg::Bitmap& b) {
 unsigned n=0;for(int y=0;y<b.height();++y)for(int x=0;x<b.width();++x)n+=alpha(b,x,y)!=0;return n;
}
}
#endif
