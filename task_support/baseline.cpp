#include <pe-parse/parse.h>
#include "fixture.h"
#include <cstdio>
int main(){
 auto bytes=sample::pe();auto *p=peparse::ParsePEFromPointer(bytes.data(),bytes.size());
 if(!p){std::fprintf(stderr,"parse: %s\n",peparse::GetPEErrString().c_str());return 1;}
 peparse::VA entry=0;std::vector<std::uint8_t>d;std::uint8_t x=0;
 bool ok=peparse::GetEntryPoint(p,entry)&&entry==0x140001000ULL&&peparse::GetDataDirectoryEntry(p,peparse::DIR_EXCEPTION,d)&&d.size()==12&&peparse::ReadByteAtVA(p,0x140003040ULL,x)&&x==0x22;
 if(ok)std::printf("PE_BASELINE entry=140001000 exception_bytes=%zu funcinfo_magic_low=%02x\n",d.size(),unsigned(x));
 peparse::DestructParsedPE(p);return ok?0:2;
}
