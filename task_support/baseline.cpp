#include <elfio/elfio.hpp>
#include <sstream>
#include <iostream>
#include <cstring>
int main() {
 using namespace ELFIO;
 for (auto cls : {ELFCLASS32,ELFCLASS64}) for (auto enc : {ELFDATA2LSB,ELFDATA2MSB}) {
  elfio w;w.create(cls,enc);w.set_type(ET_EXEC);w.set_machine(EM_X86_64);
  // Public fixture contains a DWARF4 unit and abbrev bytes; ELFIO treats them as opaque sections.
  const unsigned char infoLE[]={15,0,0,0,4,0,0,0,0,0,8,1,2,'i','n','t',0,4,0};
  unsigned char info[sizeof(infoLE)];std::memcpy(info,infoLE,sizeof(info));
  if(enc==ELFDATA2MSB){info[0]=0;info[3]=15;info[4]=0;info[5]=4;}
  const unsigned char ab[]={1,0x11,1,0,0,2,0x24,0,3,8,0x0b,0x0b,0,0,0};
  auto* a=w.sections.add(".debug_abbrev");a->set_type(SHT_PROGBITS);a->set_data((const char*)ab,sizeof(ab));
  auto* i=w.sections.add(".debug_info");i->set_type(SHT_PROGBITS);i->set_data((const char*)info,sizeof(info));
  std::stringstream out(std::ios::in|std::ios::out|std::ios::binary);
  if(!w.save(out))return 2;elfio r;if(!r.load(out))return 3;
  auto* s=r.sections[".debug_info"];
  if(!s || s->get_size()!=sizeof(info) || std::memcmp(s->get_data(),info,sizeof(info)))return 4;
  std::cout<<"OPAQUE_DWARF class="<<int(cls)<<" endian="<<int(enc)<<" bytes="<<s->get_size()<<"\n";
 }
 std::cout<<"ELFIO_BASELINE_OK\n";
}
