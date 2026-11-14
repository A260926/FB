#pragma once
#include "PDB_PCH.h"
#include "PDB_Types.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace fixture {
using Bytes = std::vector<unsigned char>;
template<class T> void append(Bytes& out, const T& value) {
    const auto* p = reinterpret_cast<const unsigned char*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}
inline void u16(Bytes& out, unsigned v) { out.push_back(v & 255); out.push_back((v >> 8) & 255); }
inline void u32(Bytes& out, unsigned v) { for (unsigned i=0; i<4; ++i) out.push_back((v >> (8*i)) & 255); }
// Only serializes streams; no OMAP interpretation or reference answer.
inline Bytes msf(const std::vector<Bytes>& streams) {
    constexpr unsigned page = 512;
    Bytes directory; u32(directory, static_cast<unsigned>(streams.size()));
    for (const auto& s : streams) u32(directory, static_cast<unsigned>(s.size()));
    unsigned next = 5;
    std::vector<std::vector<unsigned>> pages;
    for (const auto& s : streams) {
        pages.emplace_back();
        for (size_t offset=0; offset<s.size(); offset+=page) {
            pages.back().push_back(next); u32(directory,next); next+=2;
        }
    }
    if (directory.size()>page) throw std::runtime_error("fixture directory too large");
    Bytes result(next*page,0xcc);
    std::fill(result.begin(), result.begin()+5*page, 0);
    PDB::SuperBlock super{};
    std::memcpy(super.fileMagic,PDB::SuperBlock::MAGIC,30);
    super.blockSize=page; super.freeBlockMapIndex=1; super.blockCount=next;
    super.directorySize=static_cast<unsigned>(directory.size());
    std::memcpy(result.data(),&super,sizeof(super));
    unsigned blockmap=4;std::memcpy(result.data()+sizeof(super),&blockmap,4);
    unsigned dirpage=3;std::memcpy(result.data()+4*page,&dirpage,4);
    std::memcpy(result.data()+3*page,directory.data(),directory.size());
    for (size_t i=0;i<streams.size();++i) {
        size_t offset=0;
        for (unsigned physical : pages[i]) {
            size_t size=std::min<size_t>(page,streams[i].size()-offset);
            std::memcpy(result.data()+physical*page,streams[i].data()+offset,size);offset+=size;
        }
    }
    return result;
}
}
