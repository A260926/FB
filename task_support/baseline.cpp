#include <elfio/elfio.hpp>
#include <iostream>
using namespace ELFIO;
int main(int argc,char** argv){
 if(argc!=2)return 1;elfio e;if(!e.load(argv[1]))return 2;
 auto map=e.sections[".llvm_bb_addr_map"];if(!map||!map->get_data())return 3;
 unsigned n=0;for(auto& p:e.sections){if(p->get_type()!=SHT_RELA)continue;const_relocation_section_accessor r(e,p.get());for(Elf_Xword i=0;i<r.get_entries_num();++i){Elf64_Addr off;Elf_Word sym;unsigned type;Elf_Sxword add;
 if(!r.get_entry(i,off,sym,type,add))return 4;
 std::string name;Elf64_Addr value;Elf_Xword size;unsigned char bind,st,other;Elf_Half sec;
 const_symbol_section_accessor sy(e,e.sections[p->get_link()]);if(!sy.get_symbol(sym,name,value,size,bind,st,sec,other))return 5;
 std::cout<<"RAW_RELA offset="<<off<<" target_section="<<sec<<" value="<<value<<" addend="<<add<<" type="<<type<<"\n";++n;}}
 if(e.get_type()==ET_REL&&n!=3)return 6;
 std::cout<<"ELFIO_RAW_OK type="<<e.get_type()<<" map_bytes="<<map->get_size()<<" relocations="<<n<<"\n";
}
