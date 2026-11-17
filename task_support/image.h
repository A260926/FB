#pragma once
#include "PDBFormat.hpp"
#include <vector>
#include <cstring>
#include <cstdint>
#include <stdexcept>
namespace fixture {
using Bytes=std::vector<std::byte>;
template<class T> void put(Bytes& b,T v) { auto p=reinterpret_cast<const std::byte*>(&v);b.insert(b.end(),p,p+sizeof(v)); }
inline void text(Bytes& b,const char* p) { while(*p)b.push_back(std::byte(*p++));b.push_back(std::byte{0}); }
inline void append(Bytes& a,const Bytes& b){a.insert(a.end(),b.begin(),b.end());}
inline size_t record(Bytes& b,uint16_t kind,Bytes payload) {
  // CodeView symbol records are padded to a four-byte boundary, length includes padding.
  while((payload.size()+4)%4)payload.push_back(std::byte{0});
  size_t start=b.size();put(b,uint16_t(payload.size()+2));put(b,kind);append(b,payload);return start;
}
inline Bytes image(bool modern) {
  using namespace MemPDB::detail;
  std::vector<Bytes> streams(8);
  Bytes args;put(args,uint32_t(1));put(args,uint32_t(0x74));
  Bytes proc;put(proc,uint32_t(0x74));put(proc,uint8_t(0));put(proc,uint8_t(0));put(proc,uint16_t(1));put(proc,uint32_t(0x1000));
  Bytes types;
  auto typerec=[&](uint16_t kind,Bytes payload){while((payload.size()+4)%4)payload.push_back(std::byte{0xf1});put(types,uint16_t(payload.size()+2));put(types,kind);append(types,payload);};
  typerec(kLF_ARGLIST,args);typerec(kLF_PROCEDURE,proc);
  TPIStreamHeader th{};th.Version=20040203;th.HeaderSize=sizeof(th);th.TypeIndexBegin=0x1000;th.TypeIndexEnd=0x1002;th.TypeRecordBytes=types.size();th.HashStreamIndex=th.HashAuxStreamIndex=-1;
  put(streams[2],th);append(streams[2],types);
  // Minimal deterministic Info stream and empty named-stream map.
  put(streams[1],uint32_t(20000404));put(streams[1],uint32_t(0));put(streams[1],uint32_t(1));
  streams[1].resize(28);for(uint32_t n:{0u,0u,1u,0u,0u})put(streams[1],n);
  Bytes& mod=streams[7];put(mod,uint32_t(4));
  ProcSym32Payload ps{};ps.ProcLen=16;ps.TypeIndex=0x1001;ps.Offset=0x20;ps.Segment=1;ps.DbgEnd=16;
  Bytes pp;put(pp,ps);text(pp,"inspect");size_t procAt=record(mod,kS_GPROC32,pp);
  Bytes local;
  if(modern) {
    put(local,uint32_t(0x74));put(local,uint16_t(1));text(local,"count");record(mod,0x113e,local);
    Bytes dr;put(dr,uint16_t(17));put(dr,uint16_t(0));put(dr,uint32_t(0x20));put(dr,uint16_t(1));put(dr,uint16_t(16));put(dr,uint16_t(4));put(dr,uint16_t(4));record(mod,0x1141,dr);
  } else {
    RegRelSym32Payload rr{};rr.Offset=8;rr.TypeIndex=0x74;rr.Register=22;put(local,rr);text(local,"count");record(mod,kS_REGREL32,local);
  }
  size_t end=record(mod,kS_END,{});uint32_t end32=end;std::memcpy(mod.data()+procAt+4+4,&end32,4);
  ModuleInfoEntry mi{};mi.ScSection=1;mi.ScOffset=0x20;mi.ScSize=16;mi.ModuleSymStream=7;mi.SymByteSize=mod.size();
  Bytes modules;put(modules,mi);text(modules,"fixture.obj");text(modules,"fixture.obj");while(modules.size()%4)modules.push_back(std::byte{0});
  DBIStreamHeader dh{};dh.VersionSignature=-1;dh.VersionHeader=19990903;dh.Age=1;dh.GlobalStreamIndex=dh.PublicStreamIndex=0xffff;dh.SymRecordStream=5;dh.ModInfoSize=modules.size();dh.OptionalDbgHeaderSize=22;dh.Machine=0x14c;
  put(streams[3],dh);append(streams[3],modules);for(int i=0;i<11;++i)put(streams[3],uint16_t(i==5?6:0xffff));
  PublicSym32Payload pub{};pub.PublicSymFlags=2;pub.Offset=0x20;pub.Segment=1;Bytes pb;put(pb,pub);text(pb,"inspect");record(streams[5],kS_PUB32,pb);
  SectionHeader sh{};std::memcpy(sh.Name,".text",5);sh.VirtualAddress=0x1000;sh.VirtualSize=0x100;sh.Characteristics=0x60000020;put(streams[6],sh);
  // MSF7: FPM pages 1/2, block map 3, directory 4; payload streams begin at page 5.
  constexpr uint32_t block=4096;Bytes directory;put(directory,uint32_t(streams.size()));
  for(const auto& st:streams)put(directory,uint32_t(st.size()));
  uint32_t page=5;std::vector<uint32_t> pages(streams.size());
  for(size_t i=0;i<streams.size();++i){if(!streams[i].empty()){if(streams[i].size()>block)throw std::runtime_error("basic fixture too large");pages[i]=page;put(directory,page++);}}
  Bytes out(size_t(page)*block);MSFSuperBlock sb{};std::memcpy(sb.Magic,kMSFMagic,32);sb.BlockSize=block;sb.FreeBlockMapBlock=1;sb.NumBlocks=page;sb.NumDirectoryBytes=directory.size();sb.BlockMapAddr=3;std::memcpy(out.data(),&sb,sizeof(sb));
  uint32_t dirPage=4;std::memcpy(out.data()+3*block,&dirPage,4);std::memcpy(out.data()+4*block,directory.data(),directory.size());
  for(size_t i=0;i<streams.size();++i)if(pages[i])std::memcpy(out.data()+size_t(pages[i])*block,streams[i].data(),streams[i].size());
  return out;
}
}
