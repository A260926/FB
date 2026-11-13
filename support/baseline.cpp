#include <pe-parse/parse.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
static void put(std::vector<std::uint8_t>&b,unsigned o,unsigned v,unsigned n=4){for(unsigned i=0;i<n;i++)b[o+i]=static_cast<std::uint8_t>(v>>(8*i));}
static int imports(void *p,const peparse::VA&va,const std::string&mod,const std::string&sym){
 unsigned &n=*static_cast<unsigned*>(p);assert(mod=="B.DLL");
 if(n==0){assert(sym=="Entry" && va==0x401080);}else{assert(sym=="ORDINAL_B.DLL_7" && va==0x401084);}++n;return 0;
}
int main(){
 std::vector<std::uint8_t>b(0x600,0);put(b,0,0x5a4d,2);put(b,0x3c,0x80);put(b,0x80,0x4550);
 put(b,0x84,0x14c,2);put(b,0x86,1,2);put(b,0x94,224,2);put(b,0x96,0x2102,2);
 unsigned o=0x98;put(b,o,0x10b,2);put(b,o+16,0x1000);put(b,o+28,0x400000);
 put(b,o+32,0x1000);put(b,o+36,0x200);put(b,o+56,0x2000);put(b,o+60,0x200);put(b,o+92,16);
 put(b,o+104,0x1000);put(b,o+108,40);unsigned s=o+224;memcpy(&b[s],".data",5);
 put(b,s+8,0x400);put(b,s+12,0x1000);put(b,s+16,0x400);put(b,s+20,0x200);put(b,s+36,0xc0000040);
 put(b,0x200,0x1060);put(b,0x20c,0x1050);put(b,0x210,0x1080);memcpy(&b[0x250],"B.dll",6);
 put(b,0x260,0x1090);put(b,0x264,0x80000007);memcpy(&b[0x292],"Entry",6);
 peparse::parsed_pe *p=peparse::ParsePEFromPointer(b.data(),static_cast<std::uint32_t>(b.size()));
 if(!p){fprintf(stderr,"parse failure %s\n",peparse::GetPEErrString().c_str());return 1;}
 unsigned count=0;peparse::IterImpVAString(p,imports,&count);assert(count==2);
 peparse::VA entry;assert(peparse::GetEntryPoint(p,entry) && entry==0x401000);
 peparse::DestructParsedPE(p);puts("BASELINE_PE_IMPORT_OK named=1 ordinal=1");
}
