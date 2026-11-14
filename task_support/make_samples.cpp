// Deterministic, inert raw BTF material. No kernel loading or target analysis.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>
using Bytes=std::vector<std::uint8_t>;
static void u32(Bytes &b,std::uint32_t n) {for(int i=0;i<4;++i)b.push_back(static_cast<std::uint8_t>(n>>(8*i)));}
static std::uint32_t str(Bytes &b,const std::string &s){auto n=static_cast<std::uint32_t>(b.size());b.insert(b.end(),s.begin(),s.end());b.push_back(0);return n;}
static void type(Bytes&b,std::uint32_t name,std::uint32_t kind,std::uint32_t n,std::uint32_t sz){u32(b,name);u32(b,(kind<<24)|n);u32(b,sz);}
static void save(const std::filesystem::path&p,const Bytes&t,const Bytes&s){
 Bytes b{0x9f,0xeb,1,0};u32(b,24);u32(b,0);u32(b,t.size());u32(b,t.size());u32(b,s.size());
 b.insert(b.end(),t.begin(),t.end());b.insert(b.end(),s.begin(),s.end());
 std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());if(!f)throw std::runtime_error("write fixture");
}
int main(int argc,char**argv){
 if(argc!=2)return 2;std::filesystem::path out=argv[1];std::filesystem::create_directories(out);
 Bytes bs{0},bt;auto intn=str(bs,"int");auto shared=str(bs,"shared");auto value=str(bs,"value");auto ghost=str(bs,"ghost_name");
 type(bt,intn,1,0,4);u32(bt,0x01000020);type(bt,shared,4,1,4);u32(bt,value);u32(bt,1);u32(bt,0);save(out/"base.btf",bt,bs);
 for(int side=0;side<2;++side){
  Bytes s,t;auto node=static_cast<std::uint32_t>(bs.size())+str(s,"node");auto next=static_cast<std::uint32_t>(bs.size())+str(s,"next");
  type(t,0,2,0,4); // local ID3 -> local ID4, a recursive pointer.
  type(t,node,4,2,side?24:16);u32(t,next);u32(t,3);u32(t,0);
  u32(t,ghost);u32(t,side?1:2);u32(t,64); // unused base string, but different field types.
  save(out/(side?"right.btf":"left.btf"),t,s);
 }
 std::cout<<"MATERIAL_WRITTEN base_types=2 sibling_local_ids=3,4 base_strings="<<bs.size()<<"\n";
}
