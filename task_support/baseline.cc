#include "elf_parser.hpp"
#include "task_support/fixture.hh"
#include <iostream>
int main(int argc,char** argv) {
    std::string path=".task-build/basic64.elf";auto bytes=sample::basic64();
    if(argc>1 && std::string(argv[1])=="class32-probe") {
        // A valid minimal ELF32 header with no sections/program headers.
        bytes.assign(52,0);bytes[0]=0x7f;bytes[1]='E';bytes[2]='L';bytes[3]='F';bytes[4]=1;bytes[5]=1;bytes[6]=1;
        sample::put(bytes,16,1,2);sample::put(bytes,18,3,2);sample::put(bytes,20,1,4);sample::put(bytes,40,52,2);sample::put(bytes,42,32,2);sample::put(bytes,46,40,2);
        path=".task-build/class32.elf";sample::save(path,bytes);elf_parser::Elf_parser parser(path);
        std::cout<<"CLASS32_CONSTRUCTOR_RETURNED\n";return 0;
    }
    sample::save(path,bytes);elf_parser::Elf_parser parser(path);
    auto sections=parser.get_sections();auto segments=parser.get_segments();auto syms=parser.get_symbols();auto rels=parser.get_relocations();
    if(sections.size()!=6 || segments.size()!=1 || syms.size()!=2 || rels.size()!=1)return 2;
    if(syms[1].symbol_name!="anchor" || syms[1].symbol_value!=0x401002 || rels[0].relocation_symbol_name!="anchor")return 3;
    std::cout<<"SECTIONS=6 SEGMENTS=1 SYMBOLS=2 RELOCATIONS=1\n";
    std::cout<<"SYMBOL=anchor VALUE="<<syms[1].symbol_value<<" RELOCATION="<<rels[0].relocation_type<<"\nBASELINE_OK\n";
}
