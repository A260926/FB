#include <coffi/coffi.hpp>
#include "fixture.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <cstdlib>
static void require(bool ok, const char* why) {
    if(!ok) { std::cerr << "BASELINE_FAIL " << why << '\n'; std::exit(1); }
}
int main() {
    unsigned definitions=0, references=0, primaries=0, raw_slots=0;
    for(unsigned m=0; m<3; ++m) {
        auto bytes=sample::module(m);
        std::string original(bytes.begin(),bytes.end());
        std::string path=".task-build/module"+std::to_string(m)+".obj";
        std::ofstream file(path,std::ios::binary); file.write(original.data(),original.size()); file.close();
        COFFI::coffi obj; std::istringstream in(original,std::ios::binary);
        require(obj.load(in),"load actual COFF");
        require(obj.get_sections().get_count()==4,"sections");
        COFFI::coffi from_file; require(from_file.load(path),"legacy file path");
        require(from_file.get_sections().get_count()==4,"file path sections");
        require(obj.get_optional_header()==nullptr,"ordinary OBJ");
        require(obj.get_header()->get_machine()==IMAGE_FILE_MACHINE_AMD64,"AMD64");
        const auto& symbols=*obj.get_symbols();
        require(symbols.size()==8,"primary symbols");
        require(obj.get_header()->get_symbols_count()==12,"raw slots including aux");
        primaries+=symbols.size(); raw_slots+=obj.get_header()->get_symbols_count();
        std::cout << "MODULE " << m << " SECTIONS=4 PRIMARY_SYMBOLS=8 RAW_SLOTS=12\n";
        for(const auto& s:symbols) {
            require(obj.get_symbol(s.get_index())==&s,"raw-slot lookup");
            if(s.get_name()=="entry" && s.get_section_number()!=0) ++definitions;
            if(!s.get_auxiliary_symbols().empty()) {
                COFFI::auxiliary_symbol_record_5 aux{};
                std::memcpy(&aux,s.get_auxiliary_symbols()[0].value,sizeof(aux));
                std::cout << "SECTION " << m << ':' << s.get_section_number()
                          << " SLOT=" << s.get_index() << " NAME=" << s.get_name()
                          << " SELECTION=" << unsigned(aux.selection) << " PARENT=" << aux.number << '\n';
            }
        }
        for(const auto& sec:obj.get_sections()) {
            for(const auto& r:sec.get_relocations()) {
                auto* s=obj.get_symbol(r.get_symbol_table_index());
                require(s!=nullptr,"relocation names primary slot");
                std::cout << "RELOC " << m << ':' << sec.get_index()+1
                          << '+' << r.get_virtual_address() << " SLOT=" << r.get_symbol_table_index()
                          << " NAME=" << s->get_name() << " TYPE=" << r.get_type() << '\n';
                ++references;
            }
        }
        require(std::string(bytes.begin(),bytes.end())==original,"input bytes unchanged");
    }
    require(definitions==3 && references==12 && primaries==24 && raw_slots==36,"physical totals");
    std::cout << "RAW_ENTRY_DEFINITIONS=3 RAW_REFERENCES=12 PRIMARY_SYMBOLS=24 RAW_SLOTS=36\nBASELINE_OK\n";
}
