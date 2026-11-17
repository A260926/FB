#ifndef TASK_COFF_FIXTURE_HPP
#define TASK_COFF_FIXTURE_HPP
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
namespace sample {
using bytes = std::vector<unsigned char>;
inline void put(bytes& b, size_t off, uint32_t v, unsigned width) {
    if (off + width > b.size()) throw std::runtime_error("fixture extent");
    for (unsigned i=0; i<width; ++i) b[off+i]=static_cast<unsigned char>(v>>(8*i));
}
inline void name(bytes& b, size_t off, const std::string& s) {
    if (s.size()>8) throw std::runtime_error("short names only");
    for(size_t i=0; i<s.size(); ++i) b[off+i]=s[i];
}
struct reloc { uint32_t offset, slot; uint16_t type; };
struct section {
    std::string name;
    bytes data;
    unsigned selection=0, parent=0;
    std::vector<reloc> relocs;
};
struct symbol {
    std::string name;
    uint32_t value;
    uint16_t section;
    uint8_t storage;
    std::vector<bytes> aux;
};
// Plain little-endian AMD64 COFF OBJ, independently serialized, no image execution.
inline bytes object(const std::vector<section>& secs, const std::vector<symbol>& syms) {
    size_t cursor=20+40*secs.size();
    std::vector<size_t> data_off, rel_off;
    for(const auto& s:secs) {
        cursor=(cursor+3)&~size_t(3);
        data_off.push_back(cursor); cursor+=s.data.size();
        cursor=(cursor+3)&~size_t(3);
        rel_off.push_back(cursor); cursor+=10*s.relocs.size();
    }
    cursor=(cursor+3)&~size_t(3);
    size_t sym_off=cursor, slots=0;
    for(const auto& s:syms) slots+=1+s.aux.size();
    bytes out(cursor+18*slots+4,0);
    put(out,0,0x8664,2); put(out,2,secs.size(),2);
    put(out,8,sym_off,4); put(out,12,slots,4);
    for(size_t i=0; i<secs.size(); ++i) {
        const auto& s=secs[i]; size_t h=20+40*i;
        name(out,h,s.name); put(out,h+16,s.data.size(),4);
        put(out,h+20,data_off[i],4);
        if(!s.relocs.empty()) put(out,h+24,rel_off[i],4);
        put(out,h+32,s.relocs.size(),2);
        put(out,h+36,0x40300040u|(s.selection?0x1000u:0),4);
        for(size_t j=0; j<s.data.size(); ++j) out[data_off[i]+j]=s.data[j];
        for(size_t j=0; j<s.relocs.size(); ++j) {
            size_t r=rel_off[i]+10*j;
            put(out,r,s.relocs[j].offset,4); put(out,r+4,s.relocs[j].slot,4);
            put(out,r+8,s.relocs[j].type,2);
        }
    }
    cursor=sym_off;
    for(const auto& s:syms) {
        name(out,cursor,s.name); put(out,cursor+8,s.value,4);
        put(out,cursor+12,s.section,2); put(out,cursor+16,s.storage,1);
        put(out,cursor+17,s.aux.size(),1); cursor+=18;
        for(const auto& a:s.aux) {
            if(a.size()!=18) throw std::runtime_error("aux size");
            for(size_t j=0; j<18; ++j) out[cursor+j]=a[j];
            cursor+=18;
        }
    }
    put(out,cursor,4,4); return out;
}
inline bytes module(unsigned m) {
    // B deliberately puts the grandchild before its parent and root.
    std::vector<section> secs=m==1?
      std::vector<section>{{".pdata",bytes(4),5,3,{}},{".text",bytes(12),6,0,{}},
                           {".xdata",bytes(4),5,2,{}},{".refs",bytes(12),0,0,{}}}:
      std::vector<section>{{".text",bytes(m==0?4:12),6,0,{}},{".xdata",bytes(4),5,1,{}},
                           {".pdata",bytes(4),5,2,{}},{".refs",bytes(12),0,0,{}}};
    std::vector<symbol> syms;
    uint32_t slot=0, local_slot=0;
    for(size_t i=0; i<secs.size(); ++i) {
        bytes a(18,0); put(a,0,secs[i].data.size(),4);
        put(a,4,secs[i].name==".refs"?3:(secs[i].name==".text"?1:0),2);
        put(a,12,secs[i].parent,2); put(a,14,secs[i].selection,1);
        syms.push_back({secs[i].name,0,static_cast<uint16_t>(i+1),3,{a}}); slot+=2;
        if(secs[i].selection==6) {
            syms.push_back({"entry",0,static_cast<uint16_t>(i+1),2,{}}); ++slot;
        }
        if(secs[i].name==".xdata") {
            local_slot=slot; syms.push_back({"local",0,static_cast<uint16_t>(i+1),3,{}}); ++slot;
        }
    }
    uint32_t dup_slot=slot++;
    syms.push_back({"dup",0,4,2,{}});
    uint32_t undef_slot=slot++;
    syms.push_back({"entry",0,0,2,{}});
    for(auto& s:secs) {
        if(s.name==".refs") s.relocs={{0,undef_slot,2},{4,local_slot,2},{8,dup_slot,2}};
        if(s.name==".text") s.relocs={{0,undef_slot,2}};
    }
    return object(secs,syms);
}
} // namespace sample
#endif
