#pragma once
#include "PDBFormat.hpp"
#include <vector>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <algorithm>
#include <span>
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

// -----------------------------------------------------------------------------
// Composable builder for richer synthetic images (tests/parameter_locations.cpp)
//
// The original image(bool) above is left untouched. A Builder declares several
// modules, sections, procedures, S_BLOCK32 scopes and S_LOCAL / S_DEFRANGE
// records, then emits an MSF container. `fragmented` scatters every multi-page
// stream across non-contiguous blocks so the reader must reassemble it.
//
// Type index conventions used by the tests:
//   LF_ARGLIST      : 0x1000 + i                    (i = index into `argLists`)
//   LF_PROCEDURE    : 0x1000 + argLists.size() + i  (same i)
//   extraTypes      : 0x1000 + 2*argLists.size() + i (i = extraTypes index)
// -----------------------------------------------------------------------------
struct SymSpec {
  uint16_t kind;
  Bytes payload;
};

// Range operand of a S_DEFRANGE_* record, in section-relative terms.
struct RangeSpec {
  uint16_t section;
  uint32_t sectionOffset;
  uint16_t length;
  uint16_t gapOffset = 0;
  uint16_t gapLength = 0;
};

struct LocalSpec {
  uint32_t typeIdx;
  uint16_t flags;
  std::string name;              // may be empty
  std::vector<SymSpec> ranges;   // S_DEFRANGE* records, in order
};

struct ProcSpec {
  std::string name;
  uint32_t typeIdx;              // index into argLists (0x1000 + i)
  uint32_t procLen;
  uint16_t segment;
  uint32_t offset;               // entry offset within `segment`
  std::vector<SymSpec> prefix;   // records before the first S_LOCAL
  std::vector<LocalSpec> locals; // emitted in declaration order
  std::vector<SymSpec> suffix;   // records after the locals
};

struct ModuleSpec {
  std::string name;
  uint16_t section;
  uint32_t offset;
  uint32_t size;
  std::vector<ProcSpec> procs;
  Bytes rawSymbols;              // optional hand-built body, used verbatim
};

struct SectionSpec {
  std::string name;
  uint32_t rva;                  // section base RVA
  uint32_t size;
  uint32_t chars = 0x60000020;
};

struct PublicSpec {
  std::string name;
  uint16_t segment;
  uint32_t offset;
  uint32_t flags = 2;            // fFunction by default
};

// ---- payload helpers -------------------------------------------------------
inline Bytes localPayload(uint32_t typeIdx, uint16_t flags, std::string_view name) {
  Bytes b; put(b, typeIdx); put(b, flags); text(b, std::string(name).c_str()); return b;
}
inline Bytes defRangeRegister(uint16_t reg, const RangeSpec& r) {
  Bytes b; put(b, reg); put(b, uint16_t(0));
  put(b, r.sectionOffset); put(b, r.section); put(b, r.length);
  put(b, r.gapOffset); put(b, r.gapLength); return b;
}
inline Bytes defRangeFramePointerRel(int32_t offset, const RangeSpec& r) {
  Bytes b; put(b, offset);
  put(b, r.sectionOffset); put(b, r.section); put(b, r.length);
  put(b, r.gapOffset); put(b, r.gapLength); return b;
}
inline Bytes defRangeFramePointerRelFullScope(int32_t offset) {
  Bytes b; put(b, offset); return b;
}
inline Bytes defRangeRegisterRel(uint16_t reg, uint16_t flags, int32_t offset,
                                 const RangeSpec& r) {
  Bytes b; put(b, reg); put(b, flags); put(b, offset);
  put(b, r.sectionOffset); put(b, r.section); put(b, r.length);
  put(b, r.gapOffset); put(b, r.gapLength); return b;
}
inline Bytes blockPayload(uint32_t offset, uint16_t segment, uint32_t length,
                          std::string_view name) {
  Bytes b;
  put(b, uint32_t(0)); put(b, uint32_t(0));  // Parent, End (End patched by builder)
  put(b, uint32_t(0)); put(b, uint32_t(0));  // unused
  put(b, offset); put(b, segment); put(b, uint16_t(0)); put(b, length);
  text(b, std::string(name).c_str()); return b;
}
inline Bytes argListPayload(int count, uint32_t type = 0x74) {
  Bytes b; put(b, uint32_t(count));
  for (int i = 0; i < count; ++i) put(b, type);
  return b;
}
inline Bytes argListPayload(std::initializer_list<uint32_t> types) {
  Bytes b; put(b, uint32_t(types.size()));
  for (uint32_t t : types) put(b, t);
  return b;
}
inline Bytes argListPayload(std::span<const uint32_t> types) {
  Bytes b; put(b, uint32_t(types.size()));
  for (uint32_t t : types) put(b, t);
  return b;
}
inline Bytes procPayload(uint32_t argListIndex, int paramCount, uint32_t retType = 0x74) {
  Bytes b;
  put(b, retType); put(b, uint8_t(0)); put(b, uint8_t(0));
  put(b, uint16_t(paramCount));
  put(b, uint32_t(0x1000 + argListIndex));
  return b;
}

// ---- stream helper ---------------------------------------------------------
// Emits a raw S_* module body (starting with the u32 signature) with one
// procedure record per ProcSpec, or a module's rawSymbols verbatim.
inline Bytes buildModuleStream(const ModuleSpec& m, uint32_t procTypeBase = 0x2000) {
  using namespace MemPDB::detail;
  if (!m.rawSymbols.empty()) return m.rawSymbols;
  Bytes out; put(out, uint32_t(4));
  for (const auto& p : m.procs) {
    ProcSym32Payload ps{};
    ps.ProcLen = p.procLen; ps.TypeIndex = procTypeBase + p.typeIdx;
    ps.Offset = p.offset; ps.Segment = p.segment;
    ps.DbgStart = 0; ps.DbgEnd = p.procLen;
    Bytes pp; put(pp, ps); text(pp, p.name.c_str());
    const size_t procAt = record(out, kS_GPROC32, pp);
    for (const auto& s : p.prefix) record(out, s.kind, s.payload);
    for (const auto& l : p.locals) {
      record(out, kS_LOCAL, localPayload(l.typeIdx, l.flags, l.name));
      for (const auto& r : l.ranges) record(out, r.kind, r.payload);
    }
    for (const auto& s : p.suffix) record(out, s.kind, s.payload);
    const size_t endAt = record(out, kS_END, {});
    uint32_t end32 = uint32_t(endAt);
    std::memcpy(out.data() + procAt + 4 + 4, &end32, 4); // ProcSym.Parent = End
  }
  return out;
}

struct Builder {
  std::vector<SectionSpec> sections;
  std::vector<ModuleSpec>  modules;
  std::vector<PublicSpec>  publics;
  std::vector<Bytes>       argLists;   // LF_ARGLIST payloads
  std::vector<SymSpec>     extraTypes; // emitted at 0x3000 + i

  Bytes build(bool fragmented = false, uint32_t blockSize = 4096) const {
    using namespace MemPDB::detail;
    std::vector<Bytes> streams(8); // 0 unused, 1 info, 2 TPI, 3 DBI, 4 IPI, 5 syms, 6 sections

    // --- TPI ---
    Bytes types;
    auto typerec = [&](uint16_t kind, Bytes payload) {
      while ((payload.size() + 4) % 4) payload.push_back(std::byte{0xf1});
      put(types, uint16_t(payload.size() + 2)); put(types, kind); append(types, payload);
    };
    for (const auto& al : argLists) typerec(kLF_ARGLIST, al);
    for (size_t i = 0; i < argLists.size(); ++i) {
      int count = 0;
      if (argLists[i].size() >= 4) std::memcpy(&count, argLists[i].data(), 2);
      typerec(kLF_PROCEDURE, procPayload(uint32_t(i), count));
    }
    for (const auto& t : extraTypes) typerec(t.kind, t.payload);

    // --- module streams ---
    std::vector<Bytes> modStreams;
    const uint32_t procTypeBase = 0x1000 + uint32_t(argLists.size());
    for (const auto& m : modules)
      modStreams.push_back(buildModuleStream(m, procTypeBase));

    // --- DBI module-info entries (module symbol streams are 8 + i: the
    //     eight fixed streams occupy 0..7) ---
    Bytes modulesInfo;
    for (size_t i = 0; i < modules.size(); ++i) {
      ModuleInfoEntry mi{};
      mi.ScSection = int16_t(modules[i].section);
      mi.ScOffset = int32_t(modules[i].offset);
      mi.ScSize = int32_t(modules[i].size);
      mi.ModuleSymStream = uint16_t(8 + i);
      mi.SymByteSize = uint32_t(modStreams[i].size());
      put(modulesInfo, mi);
      text(modulesInfo, modules[i].name.c_str());
      text(modulesInfo, modules[i].name.c_str());
      while (modulesInfo.size() % 4) modulesInfo.push_back(std::byte{0});
    }

    // --- section headers ---
    Bytes shs;
    for (const auto& s : sections) {
      SectionHeader h{};
      std::memcpy(h.Name, s.name.data(), std::min<size_t>(8, s.name.size()));
      h.VirtualAddress = s.rva; h.VirtualSize = s.size; h.SizeOfRawData = s.size;
      h.Characteristics = s.chars;
      put(shs, h);
    }

    // --- public symbols ---
    Bytes syms;
    for (const auto& p : publics) {
      PublicSym32Payload pub{}; pub.PublicSymFlags = p.flags;
      pub.Offset = p.offset; pub.Segment = p.segment;
      Bytes b; put(b, pub); text(b, p.name.c_str()); record(syms, kS_PUB32, b);
    }

    // --- fixed streams ---
    put(streams[1], uint32_t(20000404)); put(streams[1], uint32_t(0)); put(streams[1], uint32_t(1));
    streams[1].resize(28); for (uint32_t n : {0u,0u,1u,0u,0u}) put(streams[1], n);

    TPIStreamHeader th{};
    th.Version = 20040203; th.HeaderSize = sizeof(th);
    th.TypeIndexBegin = 0x1000;
    th.TypeIndexEnd = 0x1000 + uint32_t(types.size() / 4);
    th.TypeRecordBytes = uint32_t(types.size());
    th.HashStreamIndex = th.HashAuxStreamIndex = -1;
    put(streams[2], th); append(streams[2], types);

    DBIStreamHeader dh{};
    dh.VersionSignature = -1; dh.VersionHeader = 19990903; dh.Age = 1;
    dh.GlobalStreamIndex = dh.PublicStreamIndex = 0xffff;
    dh.SymRecordStream = 5;
    dh.ModInfoSize = int32_t(modulesInfo.size());
    dh.OptionalDbgHeaderSize = 22;
    dh.Machine = 0x14c;
    put(streams[3], dh); append(streams[3], modulesInfo);
    for (int i = 0; i < 11; ++i) put(streams[3], uint16_t(i == 5 ? 6 : 0xffff));

    streams[5] = syms;
    streams[6] = shs;
    for (auto& ms : modStreams) streams.push_back(std::move(ms));

    // --- MSF serialization ---
    const uint32_t block = blockSize;
    const size_t numStreams = streams.size();

    Bytes directory; put(directory, uint32_t(numStreams));
    for (const auto& st : streams) put(directory, uint32_t(st.size()));

    const size_t dirPageCount = (directory.size() + block - 1) / block;
    uint32_t nextPage = 5 + uint32_t(dirPageCount);

    std::vector<std::vector<uint32_t>> pages(numStreams);
    for (size_t i = 0; i < numStreams; ++i) {
      if (streams[i].empty()) continue;
      const uint32_t count = uint32_t((streams[i].size() + block - 1) / block);
      for (uint32_t k = 0; k < count; ++k) pages[i].push_back(nextPage + k);
      if (fragmented && count > 1) std::reverse(pages[i].begin(), pages[i].end());
      nextPage += count;
    }
    for (size_t i = 0; i < numStreams; ++i)
      for (uint32_t pg : pages[i]) put(directory, pg);

    Bytes out(size_t(nextPage) * block, std::byte{0});
    MSFSuperBlock sb{};
    std::memcpy(sb.Magic, kMSFMagic, 32);
    sb.BlockSize = block; sb.FreeBlockMapBlock = 1;
    sb.NumBlocks = nextPage; sb.NumDirectoryBytes = uint32_t(directory.size());
    sb.BlockMapAddr = 3;
    std::memcpy(out.data(), &sb, sizeof(sb));

    // Block map lists the directory pages 5..5+dirPageCount-1.
    for (uint32_t k = 0; k < dirPageCount; ++k) {
      uint32_t pg = 5 + k;
      std::memcpy(out.data() + 3 * block + 4 * k, &pg, 4);
    }
    {
      size_t written = 0;
      for (size_t k = 0; k < dirPageCount; ++k) {
        const size_t chunk = std::min<size_t>(block, directory.size() - written);
        std::memcpy(out.data() + size_t(5 + k) * block,
                    directory.data() + written, chunk);
        written += chunk;
      }
    }

    for (size_t i = 0; i < numStreams; ++i) {
      const auto& st = streams[i];
      if (st.empty()) continue;
      size_t offset = 0;
      for (uint32_t pg : pages[i]) {
        const size_t chunk = std::min<size_t>(block, st.size() - offset);
        std::memcpy(out.data() + size_t(pg) * block, st.data() + offset, chunk);
        offset += chunk;
      }
    }
    return out;
  }
};
}
