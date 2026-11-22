#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdio>
#include <stdexcept>
struct Element {uint64_t id;size_t start,end;};
std::vector<unsigned char> bytes;
Element next(size_t& pos,size_t limit){
 if(pos>=limit)throw std::runtime_error("missing element");
 auto vint=[&](bool id){
  if(pos>=limit || !bytes[pos])throw std::runtime_error("invalid vint");
  unsigned n=1, mask=0x80;while(!(bytes[pos]&mask)){mask>>=1;++n;}
  if(n>(id?4u:8u) || pos+n>limit)throw std::runtime_error("truncated vint");
  uint64_t v=id?bytes[pos]:(bytes[pos]&(mask-1));++pos;
  for(unsigned i=1;i<n;++i)v=(v<<8)|bytes[pos++];
  if(!id && v==((uint64_t(1)<<(7*n))-1))return UINT64_MAX;
  return v;
 };
 auto id=vint(true),len=vint(false);size_t start=pos;
 if(len==UINT64_MAX)len=limit-pos;
 if(len>limit-pos)throw std::runtime_error("truncated payload");
 pos+=len;return {id,start,pos};
}
bool blocks(size_t begin,size_t end){
 for(size_t p=begin;p<end;){auto e=next(p,end);
  if((e.id==0xA3 || e.id==0xA1) && e.end-e.start>4)return true;
  if(e.id==0xA0 && blocks(e.start,e.end))return true;
 }return false;
}
int main(int argc,char**argv){try{
 if(argc!=2)return 1;std::ifstream in(argv[1],std::ios::binary);
 bytes={std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
 size_t p=0;auto head=next(p,bytes.size());if(head.id!=0x1A45DFA3)return 2;
 auto segment=next(p,bytes.size());if(segment.id!=0x18538067)return 3;
 bool tracks=false,cluster=false;
 for(size_t k=segment.start;k<segment.end;){auto e=next(k,segment.end);
  if(e.id==0x1654AE6B)tracks=true;
  if(e.id==0x1F43B675 && blocks(e.start,e.end))cluster=true;
 }
 if(!tracks || !cluster)return 4;
 std::puts("EXAMPLE_MATROSKA_READABLE");return 0;
}catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 5;}}
