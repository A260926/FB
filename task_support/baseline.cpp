#include <pe-parse/parse.h>
#include "pe_fixture.h"
#include <iostream>
int main(){
 for(bool is64:{false,true}){
  auto b=small_pe(is64);auto*p=peparse::ParsePEFromPointer(b.data(),b.size());
  if(!p){std::cerr<<peparse::GetPEErrString()<<" "<<peparse::GetPEErrLoc()<<"\n";return 2;}
  peparse::VA entry=0;std::uint8_t op=0;std::vector<std::uint8_t> raw;
  if(!peparse::GetEntryPoint(p,entry)||!peparse::ReadByteAtVA(p,entry,op)||op!=0xc3)return 3;
  if(!peparse::GetDataDirectoryEntry(p,peparse::DIR_LOAD_CONFIG,raw)||raw.size()!=(is64?148:92))return 4;
  std::cout<<"LOAD_CONFIG_RAW pe="<<(is64?64:32)<<" bytes="<<raw.size()<<" entry_opcode="<<int(op)<<"\n";
  peparse::DestructParsedPE(p);
 }
 auto*p=peparse::ParsePEFromFile("tests/assets/example.exe");if(!p)return 5;peparse::VA v=0;if(!peparse::GetEntryPoint(p,v))return 6;peparse::DestructParsedPE(p);
 std::cout<<"PE_GUARD_BASELINE_OK\n";
}
