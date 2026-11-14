#include <elfio/elfio.hpp>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
using namespace ELFIO;
static void u(std::string& s,uint64_t v){do{unsigned char b=v&127;v>>=7;s.push_back(char(b|(v?128:0)));}while(v);}
static void addr(std::string& s,uint64_t v){for(int i=0;i<8;++i)s.push_back(char(v>>(8*i)));}
static bool make(const std::string& path,bool linked){
 elfio w;w.create(ELFCLASS64,ELFDATA2LSB);w.set_type(linked?ET_EXEC:ET_REL);w.set_machine(EM_X86_64);
 auto hot=w.sections.add(".text.hot");auto cold=w.sections.add(".text.cold");
 for(auto p:{hot,cold}){p->set_type(SHT_PROGBITS);p->set_flags(SHF_ALLOC|SHF_EXECINSTR);p->set_addr_align(16);p->set_data(std::string(64,char(0x90)));}
 hot->set_address(linked?0x4000:0);cold->set_address(linked?0x8000:0);
 auto str=w.sections.add(".strtab");str->set_type(SHT_STRTAB);string_section_accessor strings(str);
 auto sym=w.sections.add(".symtab");sym->set_type(SHT_SYMTAB);sym->set_link(str->get_index());sym->set_info(3);sym->set_addr_align(8);sym->set_entry_size(w.get_default_entry_size(SHT_SYMTAB));
 symbol_section_accessor syms(w,sym);
 auto hs=syms.add_symbol(strings,"",0,0,STB_LOCAL,STT_SECTION,0,hot->get_index());
 auto cs=syms.add_symbol(strings,"",0,0,STB_LOCAL,STT_SECTION,0,cold->get_index());
 auto fn=syms.add_symbol(strings,"worker",linked?0x4008:8,9,STB_GLOBAL,STT_FUNC,0,hot->get_index());
 syms.add_symbol(strings,"worker.cold",linked?0x8008:8,5,STB_GLOBAL,STT_FUNC,0,cold->get_index());
 std::string bytes;std::vector<size_t> bases;
 bytes.push_back(2);bytes.push_back(8);u(bytes,2);
 bases.push_back(bytes.size());addr(bytes,linked?0x4008:0);u(bytes,2);
 u(bytes,9);u(bytes,0);u(bytes,4);u(bytes,0);
 u(bytes,1);u(bytes,3);u(bytes,2);u(bytes,1);
 bases.push_back(bytes.size());addr(bytes,linked?0x8008:0);u(bytes,1);
 u(bytes,4);u(bytes,0);u(bytes,5);u(bytes,1);
 bytes.push_back(3);bytes.push_back(32);bases.push_back(bytes.size());addr(bytes,linked?0x4020:0);u(bytes,2);
 u(bytes,7);u(bytes,0);u(bytes,2);u(bytes,2);u(bytes,3);u(bytes,4);u(bytes,0);
 u(bytes,3);u(bytes,2);u(bytes,0);u(bytes,1);u(bytes,1);
 auto map=w.sections.add(".llvm_bb_addr_map");map->set_type(0x6fff4c0a);map->set_link(hot->get_index());map->set_flags(SHF_LINK_ORDER);map->set_addr_align(1);map->set_data(bytes);
 if(!linked){auto r=w.sections.add(".positions");r->set_type(SHT_RELA);r->set_link(sym->get_index());r->set_info(map->get_index());r->set_addr_align(8);r->set_entry_size(w.get_default_entry_size(SHT_RELA));
 relocation_section_accessor rel(w,r);rel.add_entry(bases[2],hs,R_X86_64_64,32);rel.add_entry(bases[0],fn,R_X86_64_64,0);rel.add_entry(bases[1],cs,R_X86_64_64,8);}
 return w.save(path);
}
int main(){if(!make(".task-build/split.o",false)||!make(".task-build/linked.elf",true))return 1;std::cout<<"SYNTHETIC_ELF_SAMPLES=2\n";}
