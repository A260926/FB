#pragma once
#include "dwarf/internal.hh"
#include <vector>
#include <memory>
#include <cstdint>
namespace fixture {
using bytes=std::vector<unsigned char>;
inline void u32(bytes& b, uint32_t x) {for(int i=0;i<4;++i)b.push_back((x>>(8*i))&255);}
inline bytes ordinary() {
 bytes h={1,1,1,251,14,13,0,1,1,1,1,0,0,0,1,0,0,1,0};
 const unsigned char names[]={'u','.','c',0,0,0,0,0};h.insert(h.end(),names,names+sizeof(names));
 bytes program={0,9,2,0,1,0,0,0,0,0,0,1,2,4,3,1,1,2,4,0,1,1};
 bytes body={4,0};u32(body,h.size());body.insert(body.end(),h.begin(),h.end());body.insert(body.end(),program.begin(),program.end());
 bytes result;u32(result,body.size());result.insert(result.end(),body.begin(),body.end());return result;
}
inline dwarf::line_table table(const bytes& b) {
 auto s=std::make_shared<dwarf::section>(dwarf::section_type::line,b.data(),b.size(),dwarf::byte_order::lsb);
 return dwarf::line_table(s,0,8,"/src","u.c");
}
}
