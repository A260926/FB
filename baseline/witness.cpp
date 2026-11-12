#include <elfio/elfio.hpp>
#include <cassert>
#include <sstream>
#include <iostream>
int main() {
  for (auto cls : {ELFIO::ELFCLASS32, ELFIO::ELFCLASS64})
  for (auto enc : {ELFIO::ELFDATA2LSB, ELFIO::ELFDATA2MSB}) {
    ELFIO::elfio w; w.create(cls, enc); w.set_type(ELFIO::ET_DYN);
    auto str=w.sections.add(".dynstr"); str->set_type(ELFIO::SHT_STRTAB);
    ELFIO::string_section_accessor strings(str);
    auto sym=w.sections.add(".dynsym"); sym->set_type(ELFIO::SHT_DYNSYM);
    sym->set_entry_size(w.get_default_entry_size(ELFIO::SHT_SYMTAB));
    sym->set_link(str->get_index());
    ELFIO::symbol_section_accessor symbols(w,sym);
    symbols.add_symbol(strings,"safe_function",0x2340,8,ELFIO::STB_GLOBAL,ELFIO::STT_FUNC,0,ELFIO::SHN_ABS);
    std::stringstream wire; assert(w.save(wire));
    ELFIO::elfio r; wire.seekg(0); assert(r.load(wire));
    ELFIO::symbol_section_accessor got(r,r.sections[".dynsym"]);
    assert(got.get_symbols_num()==2);
    std::string name; ELFIO::Elf64_Addr value; ELFIO::Elf_Xword size;
    unsigned char bind,type,other; ELFIO::Elf_Half section;
    assert(got.get_symbol(1,name,value,size,bind,type,section,other));
    assert(name=="safe_function" && value==0x2340 && size==8 && section==ELFIO::SHN_ABS);
    std::cout<<"SYMBOL_ROUNDTRIP class="<<int(cls)<<" endian="<<int(enc)<<" PASS\n";
  }
  std::cout<<"ELF_SYMBOL_BASELINE_OK\n";
}
