#include <vector>
#include <string>
#include <fstream>
using Bytes=std::vector<unsigned char>;
inline void put(Bytes& b,size_t at,unsigned long long v,int n) {for(int i=0;i<n;i++)b.at(at+i)=v>>(8*i);}
inline void add(Bytes& b,unsigned long long v,int n){size_t p=b.size();b.resize(p+n);put(b,p,v,n);}
inline void fixture(const char* path,bool neighbor) {
 Bytes abbrev={1,0x11,0,0x79,0x17,0x72,0x17,0x25,0x25,0,0,0};
 Bytes info;add(info,18,4);add(info,5,2);add(info,1,1);add(info,8,1);add(info,0,4);
 add(info,1,1);add(info,0,4);add(info,8,4);add(info,neighbor?3:0,1);
 Bytes offsets;add(offsets,neighbor?8:12,4);add(offsets,5,2);add(offsets,0,2);add(offsets,0,4);
 if(neighbor){add(offsets,12,4);add(offsets,5,2);add(offsets,0,2);add(offsets,8,4);add(offsets,0,4);}else add(offsets,8,4);
 Bytes strings={'F','I','R','S','T',' ','1',0,'S','E','C','O','N','D',' ','2',0};
 Bytes macro={5,0,0,0x0b,1,static_cast<unsigned char>(neighbor?0:1),0};
 std::vector<std::string> names={"",".shstrtab",".debug_info",".debug_abbrev",".debug_str_offsets",".debug_str",".debug_macro"};
 Bytes shstr={0};std::vector<size_t> noff={0};
 for(size_t i=1;i<names.size();i++){noff.push_back(shstr.size());shstr.insert(shstr.end(),names[i].begin(),names[i].end());shstr.push_back(0);}
 std::vector<Bytes> sections={{},shstr,info,abbrev,offsets,strings,macro};
 Bytes elf(64);elf[0]=0x7f;elf[1]='E';elf[2]='L';elf[3]='F';elf[4]=2;elf[5]=1;elf[6]=1;
 put(elf,16,1,2);put(elf,18,62,2);put(elf,20,1,4);put(elf,52,64,2);put(elf,58,64,2);put(elf,60,sections.size(),2);put(elf,62,1,2);
 std::vector<size_t> loc(sections.size());
 for(size_t i=1;i<sections.size();i++){loc[i]=elf.size();elf.insert(elf.end(),sections[i].begin(),sections[i].end());}
 while(elf.size()%8)elf.push_back(0);size_t shoff=elf.size();put(elf,40,shoff,8);elf.resize(shoff+sections.size()*64);
 for(size_t i=1;i<sections.size();i++){size_t p=shoff+i*64;put(elf,p,noff[i],4);put(elf,p+4,i==1?3:1,4);put(elf,p+24,loc[i],8);put(elf,p+32,sections[i].size(),8);put(elf,p+48,1,8);}
 std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(elf.data()),elf.size());
}
