#include "fixture.hh"
#include "dwarf++.hh"
#include <iostream>
int main(){
 auto mem=seed_elf();elf::elf file(mem);
 if(file.sections().size()!=4)return 1;
 dwarf::dwarf debug(dwarf::elf::create_loader(file));
 if(debug.compilation_units().size()!=1)return 2;
 auto root=debug.compilation_units()[0].root();
 if(root[dwarf::DW_AT::name].as_string()!="seed")return 3;
 if(root[dwarf::DW_AT::low_pc].as_address()!=0)return 4;
 if(mem->bytes[140]!=0)return 5;
 std::cout<<"LIBELFIN_SEED_CU seed low_pc=0 sections=4\n";
}
