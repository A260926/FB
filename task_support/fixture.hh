#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
namespace sample {
using bytes=std::vector<unsigned char>;
inline void put(bytes& b,size_t p,uint64_t v,unsigned n) {
    if(p+n>b.size()) b.resize(p+n,0);
    for(unsigned i=0;i<n;i++) b[p+i]=(v>>(8*i))&255;
}
inline bytes basic64() {
    bytes b(64+56,0);b[0]=0x7f;b[1]='E';b[2]='L';b[3]='F';b[4]=2;b[5]=1;b[6]=1;
    put(b,16,3,2);put(b,18,62,2);put(b,20,1,4);put(b,32,64,8);put(b,52,64,2);
    put(b,54,56,2);put(b,56,1,2);put(b,58,64,2);put(b,60,6,2);put(b,62,1,2);
    b.resize(128,0);size_t text=b.size();b.insert(b.end(),{0x11,0x22,0x33,0x44});
    const char strings[]="\0anchor\0";size_t str=b.size();b.insert(b.end(),strings,strings+sizeof(strings));
    while(b.size()%8)b.push_back(0);size_t sym=b.size();b.resize(sym+48,0);
    put(b,sym+24,1,4);put(b,sym+28,0x11,1);put(b,sym+30,2,2);put(b,sym+32,0x401002,8);put(b,sym+40,1,8);
    size_t rel=b.size();b.resize(rel+24,0);put(b,rel,0x401000,8);put(b,rel+8,(uint64_t(1)<<32)|1,8);put(b,rel+16,uint64_t(-4),8);
    const char names[]="\0.shstrtab\0.text\0.strtab\0.symtab\0.rela.text\0";
    std::string ns(names,sizeof(names));size_t strings_pos=b.size();b.insert(b.end(),names,names+sizeof(names));
    while(b.size()%8)b.push_back(0);size_t sh=b.size();b.resize(sh+6*64,0);put(b,40,sh,8);
    auto sec=[&](unsigned i,const char* name,unsigned type,size_t off,size_t size,unsigned link,unsigned info,unsigned entry,uint64_t addr) {
        size_t p=sh+i*64;put(b,p,ns.find(name),4);put(b,p+4,type,4);put(b,p+16,addr,8);put(b,p+24,off,8);put(b,p+32,size,8);put(b,p+40,link,4);put(b,p+44,info,4);put(b,p+48,1,8);put(b,p+56,entry,8);
    };
    sec(1,".shstrtab",3,strings_pos,sizeof(names),0,0,0,0);
    sec(2,".text",1,text,4,0,0,0,0x401000);put(b,sh+2*64+8,6,8);
    sec(3,".strtab",3,str,sizeof(strings),0,0,0,0);
    sec(4,".symtab",2,sym,48,3,1,24,0);
    sec(5,".rela.text",4,rel,24,4,2,24,0);
    put(b,64,1,4);put(b,68,5,4);put(b,72,text,8);put(b,80,0x401000,8);put(b,88,0x401000,8);put(b,96,4,8);put(b,104,32,8);put(b,112,1,8);
    return b;
}
inline void save(const std::string& path,const bytes& b) {
    std::ofstream out(path,std::ios::binary);out.write((const char*)b.data(),b.size());
    if(!out)throw std::runtime_error("write fixture");
}
}
