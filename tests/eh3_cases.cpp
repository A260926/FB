/*
The MIT License (MIT)

Copyright (c) 2013 Andrew Ruef

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

// Synthetic AMD64 PE32+ images are constructed byte by byte (standard library
// only) and fed through the production parser plus the EH3 recovery. The
// parsed_pe context is destroyed before every query so that graph ownership is
// exercised as well.

#include "task_support/check.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include <pe-parse/cxx_eh3.h>
#include <pe-parse/parse.h>

namespace {

using peparse::CxxEh3Error;
using peparse::CxxEh3Graph;
using peparse::CxxEh3Status;
using peparse::RVA;
using peparse::parsed_pe;

constexpr RVA TEXT_VA = 0x1000u;
constexpr RVA PDATA_VA = 0x2000u;
constexpr RVA XDATA_VA = 0x3000u;
constexpr std::uint32_t TEXT_SZ = 0x1000u;
constexpr std::uint32_t PDATA_SZ = 0x1000u;
constexpr std::uint32_t XDATA_SZ = 0x2000u;
constexpr RVA PERSONALITY = 0x1070u;
constexpr std::uint32_t EH3_MAGIC = 0x19930522u;

// Three sections with distinct file and virtual layouts so that RVA handling
// cannot accidentally rely on RVA == file offset.
struct PeBuilder {
  std::vector<std::uint8_t> b;

  PeBuilder() {
    b.assign(0x4200, 0);
    putU16(0, 0x5a4d);
    putU32(0x3c, 0x80);
    putU32(0x80, 0x4550);
    putU16(0x84, 0x8664); // AMD64
    putU16(0x86, 3);
    putU16(0x94, 240);
    putU16(0x96, 0x22);

    const std::size_t o = 0x98;
    putU16(o, 0x20b); // PE32+
    putU32(o + 16, 0x1000);
    putU32(o + 20, 0x1000);
    putU64(o + 24, 0x140000000ull);
    putU32(o + 32, 0x1000); // SectionAlignment
    putU32(o + 36, 0x200);  // FileAlignment
    putU32(o + 56, 0x5000); // SizeOfImage
    putU32(o + 60, 0x200);  // SizeOfHeaders
    putU16(o + 68, 3);
    putU32(o + 108, 16);
    // Exception directory (index 3).
    putU32(o + 112 + 3 * 8, PDATA_VA);
    putU32(o + 116 + 3 * 8, 0);

    writeSectionHeader(0x188,
                       ".text",
                       TEXT_SZ,
                       TEXT_VA,
                       TEXT_SZ,
                       0x200,
                       0x60000020);
    writeSectionHeader(0x1b0,
                       ".pdata",
                       PDATA_SZ,
                       PDATA_VA,
                       PDATA_SZ,
                       0x1200,
                       0x40000040);
    writeSectionHeader(0x1d8,
                       ".xdata",
                       XDATA_SZ,
                       XDATA_VA,
                       XDATA_SZ,
                       0x2200,
                       0x40000040);
  }

  std::uint32_t fileOffset(RVA rva) const {
    if (rva >= TEXT_VA && rva < TEXT_VA + TEXT_SZ) {
      return 0x200u + (rva - TEXT_VA);
    }
    if (rva >= PDATA_VA && rva < PDATA_VA + PDATA_SZ) {
      return 0x1200u + (rva - PDATA_VA);
    }
    if (rva >= XDATA_VA && rva < XDATA_VA + XDATA_SZ) {
      return 0x2200u + (rva - XDATA_VA);
    }
    return 0xFFFFFFFFu;
  }

  void putU8(std::uint32_t off, std::uint8_t v) {
    b.at(off) = v;
  }
  void putU16(std::uint32_t off, std::uint16_t v) {
    b.at(off) = static_cast<std::uint8_t>(v);
    b.at(off + 1) = static_cast<std::uint8_t>(v >> 8);
  }
  void putU32(std::uint32_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      b.at(off + i) = static_cast<std::uint8_t>(v >> (8 * i));
    }
  }
  void putS32(std::uint32_t off, std::int32_t v) {
    std::uint32_t raw = 0;
    std::memcpy(&raw, &v, sizeof(raw));
    putU32(off, raw);
  }
  void putU64(std::uint32_t off, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      b.at(off + i) = static_cast<std::uint8_t>(v >> (8 * i));
    }
  }

  void rvaU8(RVA rva, std::uint8_t v) {
    putU8(fileOffset(rva), v);
  }
  void rvaU16(RVA rva, std::uint16_t v) {
    putU16(fileOffset(rva), v);
  }
  void rvaU32(RVA rva, std::uint32_t v) {
    putU32(fileOffset(rva), v);
  }
  void rvaS32(RVA rva, std::int32_t v) {
    putS32(fileOffset(rva), v);
  }

  // Marks an executable RVA with a one-byte ret instruction.
  void code(RVA rva) {
    rvaU8(rva, 0xc3);
  }

  void setExceptionDirectory(std::uint32_t byteSize) {
    putU32(0x98 + 116 + 3 * 8, byteSize);
  }

private:
  void writeSectionHeader(std::uint32_t off,
                          const char *name,
                          std::uint32_t virtualSize,
                          RVA virtualAddress,
                          std::uint32_t rawSize,
                          std::uint32_t rawPtr,
                          std::uint32_t characteristics) {
    std::memcpy(b.data() + off, name, std::strlen(name));
    putU32(off + 8, virtualSize);
    putU32(off + 12, virtualAddress);
    putU32(off + 16, rawSize);
    putU32(off + 20, rawPtr);
    putU32(off + 36, characteristics);
  }
};

void runtimeFunction(PeBuilder &pe,
                     RVA entryRva,
                     RVA begin,
                     RVA end,
                     RVA unwindInfo) {
  pe.rvaU32(entryRva, begin);
  pe.rvaU32(entryRva + 4, end);
  pe.rvaU32(entryRva + 8, unwindInfo);
}

// flagBits uses the UNW_FLAG_* numbers (1 = EHANDLER, 2 = UHANDLER,
// 4 = CHAININFO); the version occupies the low three bits.
void unwindInfo(PeBuilder &pe,
                RVA rva,
                std::uint8_t version,
                std::uint8_t flagBits,
                std::uint8_t countOfCodes,
                RVA personality,
                RVA funcInfo) {
  pe.rvaU8(rva, static_cast<std::uint8_t>(version | (flagBits << 3)));
  pe.rvaU8(rva + 2, countOfCodes);
  for (std::uint8_t i = 0; i < countOfCodes; ++i) {
    pe.rvaU16(rva + 4 + 2u * i, 0);
  }
  std::uint32_t aligned = (static_cast<std::uint32_t>(countOfCodes) + 1u) &
                          ~1u;
  RVA handlerSlot = rva + 4 + aligned * 2u;
  pe.rvaU32(handlerSlot, personality);
  pe.rvaU32(handlerSlot + 4, funcInfo);
}

void funcInfo(PeBuilder &pe,
              RVA rva,
              std::int32_t maxState,
              RVA unwindMapRva,
              std::uint32_t numTryBlocks,
              RVA tryBlockMapRva,
              std::uint32_t ipMapEntries,
              RVA ipToStateMapRva,
              RVA esTypeListRva = 0,
              std::uint32_t ehFlags = 0,
              std::uint32_t magic = EH3_MAGIC) {
  pe.rvaU32(rva, magic);
  pe.rvaS32(rva + 4, maxState);
  pe.rvaU32(rva + 8, unwindMapRva);
  pe.rvaU32(rva + 12, numTryBlocks);
  pe.rvaU32(rva + 16, tryBlockMapRva);
  pe.rvaU32(rva + 20, ipMapEntries);
  pe.rvaU32(rva + 24, ipToStateMapRva);
  pe.rvaS32(rva + 28, -32); // UnwindHelp frame offset
  pe.rvaU32(rva + 32, esTypeListRva);
  pe.rvaU32(rva + 36, ehFlags);
}

void unwindEntry(PeBuilder &pe, RVA rva, std::int32_t toState, RVA action) {
  pe.rvaS32(rva, toState);
  pe.rvaU32(rva + 4, action);
}

void tryBlock(PeBuilder &pe,
              RVA rva,
              std::int32_t tryLow,
              std::int32_t tryHigh,
              std::int32_t catchHigh,
              std::uint32_t numCatches,
              RVA handlerArrayRva) {
  pe.rvaS32(rva, tryLow);
  pe.rvaS32(rva + 4, tryHigh);
  pe.rvaS32(rva + 8, catchHigh);
  pe.rvaU32(rva + 12, numCatches);
  pe.rvaU32(rva + 16, handlerArrayRva);
}

void handlerType(PeBuilder &pe,
                 RVA rva,
                 std::uint32_t adjectives,
                 RVA typeDescriptorRva,
                 std::int32_t catchObjOffset,
                 RVA handlerRva,
                 std::int32_t parentFrameOffset) {
  pe.rvaU32(rva, adjectives);
  pe.rvaU32(rva + 4, typeDescriptorRva);
  pe.rvaS32(rva + 8, catchObjOffset);
  pe.rvaU32(rva + 12, handlerRva);
  pe.rvaS32(rva + 16, parentFrameOffset);
}

void ipState(PeBuilder &pe, RVA rva, RVA ip, std::int32_t state) {
  pe.rvaU32(rva, ip);
  pe.rvaS32(rva + 4, state);
}

parsed_pe *parse(PeBuilder &pe) {
  return peparse::ParsePEFromPointer(pe.b.data(),
                                     static_cast<std::uint32_t>(pe.b.size()));
}

std::vector<RVA> knownHandlers() {
  std::vector<RVA> handlers;
  handlers.push_back(PERSONALITY);
  return handlers;
}

struct BuiltGraph {
  CxxEh3Graph graph;
  CxxEh3Error error;
  CxxEh3Status status = CxxEh3Status::Corrupt;
};

// Builds the graph and immediately drops the parser context: every later
// query reads only graph-owned bytes.
BuiltGraph build(PeBuilder &pe, const std::vector<RVA> &handlers) {
  BuiltGraph out;
  parsed_pe *p = parse(pe);
  if (p == nullptr) {
    out.status = CxxEh3Status::InvalidArgument;
    return out;
  }
  out.status = peparse::BuildCxxEh3Graph(p, handlers, out.graph, out.error);
  peparse::DestructParsedPE(p);
  return out;
}

// ---------------------------------------------------------------------------
// mapping group: ranges, state intervals and cross-section provenance.
// ---------------------------------------------------------------------------

int mapping_basic_intervals() {
  PeBuilder pe;
  pe.setExceptionDirectory(12);

  // Code markers live in .text; all metadata lives in .xdata.
  pe.code(0x1001);
  pe.code(0x1010);
  pe.code(0x1018);
  pe.code(0x1030);

  runtimeFunction(pe, 0x2000, 0x1000, 0x1080, 0x3700);
  unwindInfo(pe, 0x3700, 1, 1 /*EHANDLER*/, 0, PERSONALITY, 0x3100);

  funcInfo(pe, 0x3100, 3, 0x3200, 1, 0x3300, 3, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1010);
  unwindEntry(pe, 0x3208, 0, 0x1018);
  unwindEntry(pe, 0x3210, 1, 0);
  tryBlock(pe, 0x3300, 0, 1, 2, 1, 0x3400);
  handlerType(pe, 0x3400, 0, 0x3500, 0, 0x1030, 0);
  ipState(pe, 0x3600, 0x1000, 0);
  ipState(pe, 0x3608, 0x1040, 1);
  ipState(pe, 0x3610, 0x1060, -1);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Ok);
  TASK_CHECK(g.graph.funcInfos.size() == 1);
  TASK_CHECK(g.graph.runtimeFunctions.size() == 1);
  TASK_CHECK(g.graph.unrecognized.empty());

  const peparse::CxxEh3RuntimeFunction &rt = g.graph.runtimeFunctions[0];
  TASK_CHECK(rt.beginRva == 0x1000 && rt.endRva == 0x1080);
  TASK_CHECK(rt.funcInfoRva == 0x3100 && rt.personalityRva == PERSONALITY);
  TASK_CHECK(rt.intervals.size() == 3);
  TASK_CHECK(rt.intervals[0].beginRva == 0x1000 &&
             rt.intervals[0].endRva == 0x1040 &&
             rt.intervals[0].state == 0 &&
             rt.intervals[0].sourceEntryRva == 0x3600);
  TASK_CHECK(rt.intervals[1].beginRva == 0x1040 &&
             rt.intervals[1].endRva == 0x1060 &&
             rt.intervals[1].state == 1 &&
             rt.intervals[1].sourceEntryRva == 0x3608);
  TASK_CHECK(rt.intervals[2].beginRva == 0x1060 &&
             rt.intervals[2].endRva == 0x1080 &&
             rt.intervals[2].state == -1 &&
             rt.intervals[2].sourceEntryRva == 0x3610);

  // Cross-section provenance: code in .text, table entries in .xdata.
  TASK_CHECK(rt.unwindInfoRva >= XDATA_VA);
  TASK_CHECK(g.graph.funcInfos[0].unwindMap[1].actionRva == 0x1018);
  TASK_CHECK(g.graph.funcInfos[0].unwindMap[1].entryRva == 0x3208);

  peparse::CxxEh3QueryResult q;
  CxxEh3Error err;
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x103f, true, q, err) == CxxEh3Status::Ok);
  TASK_CHECK(q.state == 0);
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x1040, true, q, err) == CxxEh3Status::Ok);
  TASK_CHECK(q.state == 1);
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x107f, true, q, err) == CxxEh3Status::Ok);
  TASK_CHECK(q.state == -1);
  return 0;
}

int mapping_shared_funcinfo_hole() {
  PeBuilder pe;
  pe.setExceptionDirectory(24); // two RUNTIME_FUNCTION entries

  pe.code(0x1002);
  pe.code(0x1102);
  pe.code(0x1040);

  // Two disjoint code ranges, a hole [0x1040, 0x1100) between them, one
  // shared FuncInfo identity and two separate UNWIND_INFO records.
  runtimeFunction(pe, 0x2000, 0x1000, 0x1040, 0x3700);
  runtimeFunction(pe, 0x200c, 0x1100, 0x1140, 0x3720);
  unwindInfo(pe, 0x3700, 1, 1, 0, PERSONALITY, 0x3100);
  unwindInfo(pe, 0x3720, 1, 1, 0, PERSONALITY, 0x3100);

  funcInfo(pe, 0x3100, 3, 0x3200, 0, 0, 4, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1040);
  unwindEntry(pe, 0x3208, 0, 0);
  unwindEntry(pe, 0x3210, 0, 0);
  ipState(pe, 0x3600, 0x1000, 0);
  ipState(pe, 0x3608, 0x1020, 1);
  ipState(pe, 0x3610, 0x1100, 0);
  ipState(pe, 0x3618, 0x1120, -1);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Ok);
  TASK_CHECK(g.graph.funcInfos.size() == 1);
  TASK_CHECK(g.graph.runtimeFunctions.size() == 2);
  TASK_CHECK(g.graph.runtimeFunctions[0].funcInfoIndex ==
             g.graph.runtimeFunctions[1].funcInfoIndex);

  const peparse::CxxEh3RuntimeFunction &r1 = g.graph.runtimeFunctions[0];
  const peparse::CxxEh3RuntimeFunction &r2 = g.graph.runtimeFunctions[1];
  TASK_CHECK(r1.beginRva == 0x1000 && r1.endRva == 0x1040);
  TASK_CHECK(r2.beginRva == 0x1100 && r2.endRva == 0x1140);
  TASK_CHECK(r1.intervals.size() == 2 && r2.intervals.size() == 2);
  TASK_CHECK(r1.intervals[0].beginRva == 0x1000 &&
             r1.intervals[1].endRva == 0x1040);
  TASK_CHECK(r2.intervals[0].beginRva == 0x1100 &&
             r2.intervals[1].endRva == 0x1140);
  // No interval may span the hole.
  for (const auto &iv : r1.intervals) {
    TASK_CHECK(iv.endRva <= 0x1040);
  }
  for (const auto &iv : r2.intervals) {
    TASK_CHECK(iv.beginRva >= 0x1100);
  }

  peparse::CxxEh3QueryResult q;
  CxxEh3Error err;
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x1080, true, q, err) ==
             CxxEh3Status::Uncovered);
  TASK_CHECK(err.sourceRva == 0x1080);
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x1120, true, q, err) == CxxEh3Status::Ok);
  TASK_CHECK(q.state == -1 && q.paths.size() == 1 && q.paths[0].escape);
  return 0;
}

int mapping_distinct_funcinfos() {
  PeBuilder pe;
  pe.setExceptionDirectory(24);
  pe.code(0x1006);
  pe.code(0x1106);

  // Two runtime functions with two distinct FuncInfo records; only the
  // shared-identity case may collapse indices.
  runtimeFunction(pe, 0x2000, 0x1000, 0x1020, 0x3700);
  runtimeFunction(pe, 0x200c, 0x1100, 0x1120, 0x3720);
  unwindInfo(pe, 0x3700, 1, 1, 0, PERSONALITY, 0x3100);
  unwindInfo(pe, 0x3720, 1, 1, 0, PERSONALITY, 0x3180);

  funcInfo(pe, 0x3100, 1, 0x3200, 0, 0, 1, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1006);
  ipState(pe, 0x3600, 0x1000, 0);

  funcInfo(pe, 0x3180, 1, 0x3280, 0, 0, 1, 0x3680);
  unwindEntry(pe, 0x3280, -1, 0x1106);
  ipState(pe, 0x3680, 0x1100, 0);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Ok);
  TASK_CHECK(g.graph.funcInfos.size() == 2);
  TASK_CHECK(g.graph.runtimeFunctions.size() == 2);
  TASK_CHECK(g.graph.runtimeFunctions[0].funcInfoIndex !=
             g.graph.runtimeFunctions[1].funcInfoIndex);
  TASK_CHECK(g.graph.funcInfos[0].rva == 0x3100 &&
             g.graph.funcInfos[1].rva == 0x3180);

  // The pointer/array overload must behave identically to the vector one.
  parsed_pe *p = parse(pe);
  TASK_CHECK(p != nullptr);
  CxxEh3Graph viaArray;
  CxxEh3Error arrayErr;
  const RVA handlers[] = {PERSONALITY};
  TASK_CHECK(peparse::BuildCxxEh3Graph(
                 p, handlers, 1, viaArray, arrayErr) == CxxEh3Status::Ok);
  peparse::DestructParsedPE(p);
  TASK_CHECK(viaArray.funcInfos.size() == 2 &&
             viaArray.runtimeFunctions.size() == 2);

  // An empty handler set means every personality is unrecognized.
  BuiltGraph none = build(pe, std::vector<RVA>());
  TASK_CHECK(none.status == CxxEh3Status::Ok);
  TASK_CHECK(none.graph.runtimeFunctions.empty());
  TASK_CHECK(none.graph.unrecognized.size() == 2);
  return 0;
}

int mapping_leading_minus_one_and_empty() {
  PeBuilder pe;
  pe.setExceptionDirectory(12);
  pe.code(0x1020);

  runtimeFunction(pe, 0x2000, 0x1000, 0x1060, 0x3700);
  unwindInfo(pe, 0x3700, 1, 1, 0, PERSONALITY, 0x3100);
  funcInfo(pe, 0x3100, 2, 0x3200, 0, 0, 1, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1020);
  unwindEntry(pe, 0x3208, 0, 0);
  // The first mapped IP starts inside the range: everything before it is -1.
  ipState(pe, 0x3600, 0x1030, 0);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Ok);
  const peparse::CxxEh3RuntimeFunction &rt = g.graph.runtimeFunctions[0];
  TASK_CHECK(rt.intervals.size() == 2);
  TASK_CHECK(rt.intervals[0].beginRva == 0x1000 &&
             rt.intervals[0].state == -1 &&
             rt.intervals[0].sourceEntryRva == 0);
  TASK_CHECK(rt.intervals[1].beginRva == 0x1030 &&
             rt.intervals[1].state == 0);

  peparse::CxxEh3QueryResult q;
  CxxEh3Error err;
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x1010, true, q, err) == CxxEh3Status::Ok);
  TASK_CHECK(q.state == -1);
  TASK_CHECK(q.paths.size() == 1 && q.paths[0].escape &&
             q.paths[0].steps.empty());

  // No exception directory at all: empty graph, successful build.
  PeBuilder bare;
  bare.setExceptionDirectory(0);
  BuiltGraph emptyGraph = build(bare, knownHandlers());
  TASK_CHECK(emptyGraph.status == CxxEh3Status::Ok);
  TASK_CHECK(emptyGraph.graph.empty());
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 emptyGraph.graph, 0x1000, true, q, err) ==
             CxxEh3Status::Uncovered);
  return 0;
}

int mapping_unrecognized_personalities() {
  PeBuilder pe;
  pe.setExceptionDirectory(36); // three entries
  pe.code(0x1050);

  // Recognized EH3 function.
  runtimeFunction(pe, 0x2000, 0x1000, 0x1020, 0x3700);
  unwindInfo(pe, 0x3700, 1, 1, 0, PERSONALITY, 0x3100);
  funcInfo(pe, 0x3100, 1, 0x3200, 0, 0, 1, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1050);
  ipState(pe, 0x3600, 0x1000, 0);

  // Unknown personality: kept as an unrecognized record, never interpreted.
  runtimeFunction(pe, 0x200c, 0x1100, 0x1120, 0x3800);
  unwindInfo(pe, 0x3800, 1, 1, 0, 0x1099, 0x3999);

  // No language handler slot at all.
  runtimeFunction(pe, 0x2018, 0x1200, 0x1220, 0x3900);
  unwindInfo(pe, 0x3900, 1, 0, 0, 0, 0);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Ok);
  TASK_CHECK(g.graph.runtimeFunctions.size() == 1);
  TASK_CHECK(g.graph.unrecognized.size() == 2);
  TASK_CHECK(g.graph.unrecognized[0].reason ==
             peparse::CxxEh3UnrecognizedReason::UnknownPersonality);
  TASK_CHECK(g.graph.unrecognized[0].personalityRva == 0x1099);
  TASK_CHECK(g.graph.unrecognized[1].reason ==
             peparse::CxxEh3UnrecognizedReason::NoLanguageHandler);
  return 0;
}

// ---------------------------------------------------------------------------
// dispatch group: candidate ordering, cleanup chains and query limits.
// ---------------------------------------------------------------------------

// Shared layout: MaxState 5, nested try blocks and a chain containing a
// zero-action transition.
void buildNestedFixture(PeBuilder &pe) {
  pe.setExceptionDirectory(12);
  static const RVA codeRvas[] = {0x1001, 0x1002, 0x1003, 0x1004,
                                  0x1040, 0x1048, 0x1050};
  for (RVA r : codeRvas) {
    pe.code(r);
  }

  runtimeFunction(pe, 0x2000, 0x1000, 0x1080, 0x3700);
  unwindInfo(pe, 0x3700, 1, 1, 0, PERSONALITY, 0x3100);

  funcInfo(pe, 0x3100, 5, 0x3200, 2, 0x3300, 3, 0x3600);
  // 0 -> -1 (funclet), 1 -> 0 (zero action), 2 -> 1, 3 -> 2, 4 -> 3.
  unwindEntry(pe, 0x3200, -1, 0x1001);
  unwindEntry(pe, 0x3208, 0, 0);
  unwindEntry(pe, 0x3210, 1, 0x1002);
  unwindEntry(pe, 0x3218, 2, 0x1003);
  unwindEntry(pe, 0x3220, 3, 0x1004);

  // Inner try [2,3], CatchHigh 4, two catches in fixed order.
  tryBlock(pe, 0x3300, 2, 3, 4, 2, 0x3400);
  handlerType(pe, 0x3400, 1, 0x3510, 8, 0x1040, 0);
  handlerType(pe, 0x3414, 2, 0x3520, 16, 0x1048, 0);
  // Outer try [0,3], CatchHigh 4, one catch.
  tryBlock(pe, 0x3314, 0, 3, 4, 1, 0x3440);
  handlerType(pe, 0x3440, 0, 0x3530, 0, 0x1050, 0);

  ipState(pe, 0x3600, 0x1000, 0);
  ipState(pe, 0x3608, 0x1020, 3);
  ipState(pe, 0x3610, 0x1060, 4);
}

int dispatch_nested_try_cleanup_chain() {
  PeBuilder pe;
  buildNestedFixture(pe);
  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Ok);

  peparse::CxxEh3QueryResult q;
  CxxEh3Error err;
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x1030, true, q, err) == CxxEh3Status::Ok);
  TASK_CHECK(q.state == 3);
  // Two inner catches, one outer catch, then the escape path.
  TASK_CHECK(q.paths.size() == 4);
  TASK_CHECK(!q.paths[0].escape && q.paths[0].tryOrdinal == 0 &&
             q.paths[0].catchOrdinal == 0);
  TASK_CHECK(!q.paths[1].escape && q.paths[1].tryOrdinal == 0 &&
             q.paths[1].catchOrdinal == 1);
  TASK_CHECK(!q.paths[2].escape && q.paths[2].tryOrdinal == 1 &&
             q.paths[2].catchOrdinal == 0);
  TASK_CHECK(q.paths[3].escape);

  // Inner candidate: stop at TryLow 2 without running state 2's action.
  const peparse::CxxEh3Path &inner = q.paths[0];
  TASK_CHECK(inner.stopState == 2 && inner.steps.size() == 1);
  TASK_CHECK(inner.steps[0].fromState == 3 &&
             inner.steps[0].toState == 2 &&
             inner.steps[0].actionRva == 0x1003 &&
             inner.steps[0].unwindEntryRva == 0x3218);
  TASK_CHECK(inner.handlerRva == 0x1040 &&
             inner.handlerEntryRva == 0x3400 &&
             inner.tryBlockEntryRva == 0x3300);
  TASK_CHECK(inner.adjectives == 1 &&
             inner.typeDescriptorRva == 0x3510 &&
             inner.catchObjOffset == 8);

  // Outer candidate: three steps, the middle chain includes a zero action
  // that must still be recorded and followed.
  const peparse::CxxEh3Path &outer = q.paths[2];
  TASK_CHECK(outer.stopState == 0 && outer.steps.size() == 3);
  TASK_CHECK(outer.steps[0].fromState == 3 &&
             outer.steps[0].toState == 2);
  TASK_CHECK(outer.steps[1].fromState == 2 &&
             outer.steps[1].toState == 1 &&
             outer.steps[1].actionRva == 0x1002);
  TASK_CHECK(outer.steps[2].fromState == 1 &&
             outer.steps[2].toState == 0 &&
             outer.steps[2].actionRva == 0 &&
             outer.steps[2].unwindEntryRva == 0x3208);
  TASK_CHECK(outer.handlerRva == 0x1050);

  // Escape: unwind all the way to -1 and leave the function.
  const peparse::CxxEh3Path &escape = q.paths[3];
  TASK_CHECK(escape.steps.size() == 4);
  TASK_CHECK(escape.steps[3].fromState == 0 &&
             escape.steps[3].toState == -1 &&
             escape.steps[3].actionRva == 0x1001);
  return 0;
}

int dispatch_catch_order_and_boundaries() {
  PeBuilder pe;
  pe.setExceptionDirectory(12);
  pe.code(0x1008);
  pe.code(0x1040);
  pe.code(0x1048);

  runtimeFunction(pe, 0x2000, 0x1000, 0x1030, 0x3700);
  unwindInfo(pe, 0x3700, 1, 1, 0, PERSONALITY, 0x3100);
  funcInfo(pe, 0x3100, 3, 0x3200, 1, 0x3300, 3, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1008);
  unwindEntry(pe, 0x3208, 0, 0);
  unwindEntry(pe, 0x3210, 1, 0);
  tryBlock(pe, 0x3300, 0, 1, 2, 2, 0x3400);
  handlerType(pe, 0x3400, 0, 0, 0, 0x1040, 0);
  handlerType(pe, 0x3414, 0, 0, 0, 0x1048, 0);
  ipState(pe, 0x3600, 0x1000, 0);
  ipState(pe, 0x3608, 0x1010, 1);
  ipState(pe, 0x3610, 0x1020, -1);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Ok);

  peparse::CxxEh3QueryResult q;
  CxxEh3Error err;
  // TryLow boundary.
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x1000, true, q, err) == CxxEh3Status::Ok);
  TASK_CHECK(q.state == 0 && q.paths.size() == 3);
  TASK_CHECK(q.paths[0].catchOrdinal == 0 &&
             q.paths[1].catchOrdinal == 1 &&
             q.paths[2].escape);
  // Stopping at TryLow == current state means zero recorded steps.
  TASK_CHECK(q.paths[0].steps.empty());

  // TryHigh boundary still matches the same try.
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x101f, true, q, err) == CxxEh3Status::Ok);
  TASK_CHECK(q.state == 1 && q.paths.size() == 3);
  TASK_CHECK(q.paths[0].stopState == 0 &&
             q.paths[0].steps.size() == 1 &&
             q.paths[0].steps[0].fromState == 1 &&
             q.paths[0].steps[0].toState == 0);

  // State -1 after the guarded region: only escape remains.
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x1020, true, q, err) == CxxEh3Status::Ok);
  TASK_CHECK(q.state == -1 && q.paths.size() == 1 &&
             q.paths[0].escape);
  return 0;
}

int dispatch_nested_order_independent() {
  PeBuilder pe;
  pe.setExceptionDirectory(12);
  static const RVA codeRvas[] = {0x1001, 0x1002, 0x1003, 0x1040, 0x1050};
  for (RVA r : codeRvas) {
    pe.code(r);
  }
  runtimeFunction(pe, 0x2000, 0x1000, 0x1080, 0x3700);
  unwindInfo(pe, 0x3700, 1, 1, 0, PERSONALITY, 0x3100);
  funcInfo(pe, 0x3100, 5, 0x3200, 2, 0x3300, 2, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1001);
  unwindEntry(pe, 0x3208, 0, 0);
  unwindEntry(pe, 0x3210, 1, 0x1002);
  unwindEntry(pe, 0x3218, 2, 0x1003);
  unwindEntry(pe, 0x3220, 3, 0);

  // The OUTER block is written first (ordinal 0), the inner block second
  // (ordinal 1). Inner/outer ordering must still come from containment.
  tryBlock(pe, 0x3300, 0, 3, 4, 1, 0x3440);
  handlerType(pe, 0x3440, 0, 0x3530, 0, 0x1050, 0);
  tryBlock(pe, 0x3314, 2, 3, 4, 1, 0x3400);
  handlerType(pe, 0x3400, 0, 0x3510, 8, 0x1040, 0);

  ipState(pe, 0x3600, 0x1000, 0);
  ipState(pe, 0x3608, 0x1020, 3);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Ok);

  peparse::CxxEh3QueryResult q;
  CxxEh3Error err;
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x1030, true, q, err) == CxxEh3Status::Ok);
  TASK_CHECK(q.state == 3 && q.paths.size() == 3);
  // First candidate is the inner try even though it has table ordinal 1.
  TASK_CHECK(!q.paths[0].escape && q.paths[0].tryOrdinal == 1 &&
             q.paths[0].handlerRva == 0x1040);
  TASK_CHECK(!q.paths[1].escape && q.paths[1].tryOrdinal == 0 &&
             q.paths[1].handlerRva == 0x1050);
  TASK_CHECK(q.paths[2].escape);
  return 0;
}

int dispatch_state_outside_any_try() {
  PeBuilder pe;
  pe.setExceptionDirectory(12);
  pe.code(0x1001);
  pe.code(0x1002);
  runtimeFunction(pe, 0x2000, 0x1000, 0x1040, 0x3700);
  unwindInfo(pe, 0x3700, 1, 1, 0, PERSONALITY, 0x3100);
  // No TryBlockMap at all: a valid guarded-less state only has an escape.
  funcInfo(pe, 0x3100, 2, 0x3200, 0, 0, 2, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1001);
  unwindEntry(pe, 0x3208, 0, 0x1002);
  ipState(pe, 0x3600, 0x1000, 0);
  ipState(pe, 0x3608, 0x1020, 1);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Ok);

  peparse::CxxEh3QueryResult q;
  CxxEh3Error err;
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x1030, true, q, err) == CxxEh3Status::Ok);
  TASK_CHECK(q.state == 1 && q.paths.size() == 1 && q.paths[0].escape);
  TASK_CHECK(q.paths[0].steps.size() == 2);
  TASK_CHECK(q.paths[0].steps[0].fromState == 1 &&
             q.paths[0].steps[0].toState == 0 &&
             q.paths[0].steps[0].actionRva == 0x1002);
  TASK_CHECK(q.paths[0].steps[1].fromState == 0 &&
             q.paths[0].steps[1].toState == -1 &&
             q.paths[0].steps[1].actionRva == 0x1001);
  return 0;
}

int dispatch_zero_catch_try_block() {
  PeBuilder pe;
  pe.setExceptionDirectory(12);
  pe.code(0x1001);
  pe.code(0x1002);
  pe.code(0x1040);
  runtimeFunction(pe, 0x2000, 0x1000, 0x1040, 0x3700);
  unwindInfo(pe, 0x3700, 1, 1, 0, PERSONALITY, 0x3100);
  // MaxState 4; two disjoint try blocks, the first carrying zero handlers.
  funcInfo(pe, 0x3100, 4, 0x3200, 2, 0x3300, 1, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1001);
  unwindEntry(pe, 0x3208, 0, 0);
  unwindEntry(pe, 0x3210, 1, 0x1002);
  unwindEntry(pe, 0x3218, 2, 0);
  // Zero-catch block [0,0], CatchHigh 1 (its funclet region is only state 1).
  tryBlock(pe, 0x3300, 0, 0, 1, 0, 0);
  // Normal block [2,2], CatchHigh 3 (its funclet region is only state 3).
  tryBlock(pe, 0x3314, 2, 2, 3, 1, 0x3400);
  handlerType(pe, 0x3400, 0, 0, 0, 0x1040, 0);
  ipState(pe, 0x3600, 0x1000, 2);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Ok);
  TASK_CHECK(g.graph.funcInfos[0].tryBlocks.size() == 2);
  TASK_CHECK(g.graph.funcInfos[0].tryBlocks[0].handlers.empty());

  peparse::CxxEh3QueryResult q;
  CxxEh3Error err;
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x1010, true, q, err) == CxxEh3Status::Ok);
  TASK_CHECK(q.state == 2);
  // The zero-catch block yields nothing; one normal catch plus the escape.
  TASK_CHECK(q.paths.size() == 2);
  TASK_CHECK(!q.paths[0].escape && q.paths[0].handlerRva == 0x1040);
  TASK_CHECK(q.paths[1].escape);
  return 0;
}

int dispatch_funclet_and_rethrow_unsupported() {
  PeBuilder pe;
  buildNestedFixture(pe);
  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Ok);

  peparse::CxxEh3QueryResult q;
  CxxEh3Error err;
  // Caller says this is not a plain first throw in the function body.
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x1030, false, q, err) ==
             CxxEh3Status::Unsupported);
  TASK_CHECK(err.sourceRva == 0x1030);

  // State 4 sits in (TryHigh=3, CatchHigh=4]: a funclet region.
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x1060, true, q, err) ==
             CxxEh3Status::Unsupported);
  TASK_CHECK(err.sourceRva == 0x3300 || err.sourceRva == 0x3314);
  return 0;
}

// ---------------------------------------------------------------------------
// integrity group: malformed metadata must fail loudly with no partial graph.
// ---------------------------------------------------------------------------

// Minimal valid surrounding scaffolding for the corruption cases.
void scaffold(PeBuilder &pe, RVA funcInfoRva = 0x3100) {
  pe.setExceptionDirectory(12);
  pe.code(0x1005);
  runtimeFunction(pe, 0x2000, 0x1000, 0x1040, 0x3700);
  unwindInfo(pe, 0x3700, 1, 1, 0, PERSONALITY, funcInfoRva);
}

int integrity_duplicate_ip_rejected() {
  PeBuilder pe;
  scaffold(pe);
  funcInfo(pe, 0x3100, 2, 0x3200, 0, 0, 2, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1005);
  unwindEntry(pe, 0x3208, 0, 0);
  ipState(pe, 0x3600, 0x1000, 0);
  ipState(pe, 0x3608, 0x1000, 1); // duplicate IP

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Corrupt);
  TASK_CHECK(g.error.sourceRva == 0x3608);
  TASK_CHECK(g.graph.empty());
  return 0;
}

int integrity_unwind_cycle_rejected() {
  PeBuilder pe;
  scaffold(pe);
  funcInfo(pe, 0x3100, 2, 0x3200, 0, 0, 1, 0x3600);
  unwindEntry(pe, 0x3200, 1, 0x1005); // 0 -> 1
  unwindEntry(pe, 0x3208, 0, 0);      // 1 -> 0: cycle
  ipState(pe, 0x3600, 0x1000, 0);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Cycle);
  TASK_CHECK(g.error.sourceRva != 0);
  TASK_CHECK(g.graph.empty());
  return 0;
}

int integrity_unreachable_stop_rejected() {
  PeBuilder pe;
  scaffold(pe);
  funcInfo(pe, 0x3100, 5, 0x3200, 1, 0x3300, 1, 0x3600);
  // State 3 unwinds to 1, skipping TryLow 2 of the try block [2,3].
  unwindEntry(pe, 0x3200, -1, 0x1005);
  unwindEntry(pe, 0x3208, 0, 0);
  unwindEntry(pe, 0x3210, 1, 0);
  unwindEntry(pe, 0x3218, 1, 0); // state 3 -> 1
  unwindEntry(pe, 0x3220, 3, 0);
  tryBlock(pe, 0x3300, 2, 3, 4, 1, 0x3400);
  handlerType(pe, 0x3400, 0, 0, 0, 0x1005, 0);
  ipState(pe, 0x3600, 0x1000, 3);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::UnreachableStop);
  TASK_CHECK(g.error.sourceRva == 0x3300);
  TASK_CHECK(g.graph.empty());
  return 0;
}

int integrity_out_of_range_state() {
  PeBuilder pe;
  scaffold(pe);
  funcInfo(pe, 0x3100, 2, 0x3200, 0, 0, 1, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1005);
  unwindEntry(pe, 0x3208, 2, 0); // ToState == MaxState: illegal
  ipState(pe, 0x3600, 0x1000, 0);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::OutOfRangeState);
  TASK_CHECK(g.error.sourceRva == 0x3208);
  TASK_CHECK(g.graph.empty());
  return 0;
}

int integrity_overlapping_try_rejected() {
  PeBuilder pe;
  scaffold(pe);
  funcInfo(pe, 0x3100, 5, 0x3200, 2, 0x3300, 1, 0x3600);
  for (std::uint32_t i = 0; i < 5; ++i) {
    unwindEntry(pe, 0x3200 + i * 8, i == 0 ? -1 : static_cast<std::int32_t>(i) - 1, 0);
  }
  tryBlock(pe, 0x3300, 0, 2, 4, 1, 0x3400);
  tryBlock(pe, 0x3314, 1, 3, 4, 1, 0x3420); // partial overlap
  handlerType(pe, 0x3400, 0, 0, 0, 0x1005, 0);
  handlerType(pe, 0x3420, 0, 0, 0, 0x1005, 0);
  ipState(pe, 0x3600, 0x1000, 0);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::OverlappingTry);
  TASK_CHECK(g.error.sourceRva == 0x3314);
  TASK_CHECK(g.graph.empty());
  return 0;
}

int integrity_overlapping_functions() {
  PeBuilder pe;
  pe.setExceptionDirectory(24); // two entries
  pe.code(0x1005);

  // Two RUNTIME_FUNCTION entries whose half-open code ranges intersect.
  runtimeFunction(pe, 0x2000, 0x1000, 0x1040, 0x3700);
  runtimeFunction(pe, 0x200c, 0x1020, 0x1060, 0x3720);
  unwindInfo(pe, 0x3700, 1, 1, 0, PERSONALITY, 0x3100);
  unwindInfo(pe, 0x3720, 1, 1, 0, PERSONALITY, 0x3100);
  funcInfo(pe, 0x3100, 1, 0x3200, 0, 0, 1, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1005);
  ipState(pe, 0x3600, 0x1000, 0);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Corrupt);
  TASK_CHECK(g.error.sourceRva == 0x200c);
  TASK_CHECK(g.graph.empty());
  return 0;
}

int integrity_unmapped_and_bad_magic() {
  PeBuilder pe;
  // FuncInfo RVA points outside every mapped section.
  scaffold(pe, 0x9000);
  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::UnmappedRead);
  TASK_CHECK(g.error.sourceRva == 0x9000);
  TASK_CHECK(g.graph.empty());

  PeBuilder pe2;
  scaffold(pe2);
  funcInfo(pe2,
           0x3100,
           1,
           0x3200,
           0,
           0,
           1,
           0x3600,
           0,
           0,
           0xdeadbeef); // wrong magic
  unwindEntry(pe2, 0x3200, -1, 0x1005);
  ipState(pe2, 0x3600, 0x1000, 0);
  BuiltGraph g2 = build(pe2, knownHandlers());
  TASK_CHECK(g2.status == CxxEh3Status::Corrupt);
  TASK_CHECK(g2.error.sourceRva == 0x3100);
  TASK_CHECK(g2.graph.empty());
  return 0;
}

int integrity_chain_and_version_unsupported() {
  PeBuilder pe;
  pe.setExceptionDirectory(24);
  pe.code(0x1005);
  runtimeFunction(pe, 0x2000, 0x1000, 0x1020, 0x3700);
  runtimeFunction(pe, 0x200c, 0x1100, 0x1120, 0x3800);
  unwindInfo(pe, 0x3700, 1, 4 /*CHAININFO*/, 0, PERSONALITY, 0x3100);
  unwindInfo(pe, 0x3800, 2, 1, 0, PERSONALITY, 0x3100); // version 2

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Unsupported);
  TASK_CHECK(g.error.sourceRva == 0x3700);
  TASK_CHECK(g.graph.empty());

  // ESTypeList and reserved EHFlags bits are first-version limitations.
  PeBuilder pe2;
  scaffold(pe2);
  funcInfo(pe2, 0x3100, 1, 0x3200, 0, 0, 1, 0x3600, 0x3550);
  unwindEntry(pe2, 0x3200, -1, 0x1005);
  ipState(pe2, 0x3600, 0x1000, 0);
  BuiltGraph g2 = build(pe2, knownHandlers());
  TASK_CHECK(g2.status == CxxEh3Status::Unsupported);

  PeBuilder pe3;
  scaffold(pe3);
  funcInfo(pe3, 0x3100, 1, 0x3200, 0, 0, 1, 0x3600, 0, 2);
  unwindEntry(pe3, 0x3200, -1, 0x1005);
  ipState(pe3, 0x3600, 0x1000, 0);
  BuiltGraph g3 = build(pe3, knownHandlers());
  TASK_CHECK(g3.status == CxxEh3Status::Unsupported);
  return 0;
}

int integrity_reserved_unwind_flags() {
  PeBuilder pe;
  pe.setExceptionDirectory(12);
  pe.code(0x1005);
  runtimeFunction(pe, 0x2000, 0x1000, 0x1040, 0x3700);
  // flag bit 3 (0x08 after packing) is reserved; EHANDLER is set too so the
  // record would otherwise look like a language-handler association.
  unwindInfo(pe, 0x3700, 1, 0x09, 0, PERSONALITY, 0x3100);
  funcInfo(pe, 0x3100, 1, 0x3200, 0, 0, 1, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1005);
  ipState(pe, 0x3600, 0x1000, 0);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Corrupt);
  TASK_CHECK(g.error.sourceRva == 0x3700);
  TASK_CHECK(g.graph.empty());
  return 0;
}

int integrity_odd_countofcodes_alignment() {
  PeBuilder pe;
  pe.setExceptionDirectory(12);
  pe.code(0x1005);

  runtimeFunction(pe, 0x2000, 0x1000, 0x1040, 0x3700);
  // Three unwind codes: the handler slot sits after four even-aligned slots,
  // i.e. at 0x370C with the FuncInfo pointer at 0x3710.
  unwindInfo(pe, 0x3700, 1, 1, 3, PERSONALITY, 0x3100);
  funcInfo(pe, 0x3100, 1, 0x3200, 0, 0, 1, 0x3600);
  unwindEntry(pe, 0x3200, -1, 0x1005);
  ipState(pe, 0x3600, 0x1000, 0);

  BuiltGraph g = build(pe, knownHandlers());
  TASK_CHECK(g.status == CxxEh3Status::Ok);
  TASK_CHECK(g.graph.runtimeFunctions.size() == 1);
  TASK_CHECK(g.graph.runtimeFunctions[0].personalityRva == PERSONALITY);
  TASK_CHECK(g.graph.runtimeFunctions[0].funcInfoRva == 0x3100);

  // The graph keeps working after the parsed_pe object is gone (already
  // destroyed inside build()); query it once more explicitly.
  peparse::CxxEh3QueryResult q;
  CxxEh3Error err;
  TASK_CHECK(peparse::QueryCxxEh3Paths(
                 g.graph, 0x1010, true, q, err) == CxxEh3Status::Ok);
  TASK_CHECK(q.state == 0 && q.paths.size() == 1 && q.paths[0].escape);
  return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"mapping", "basic_intervals", &mapping_basic_intervals},
    {"mapping", "shared_funcinfo_hole", &mapping_shared_funcinfo_hole},
    {"mapping", "distinct_funcinfos", &mapping_distinct_funcinfos},
    {"mapping", "leading_minus_one_and_empty",
     &mapping_leading_minus_one_and_empty},
    {"mapping", "unrecognized_personalities",
     &mapping_unrecognized_personalities},

    {"dispatch", "nested_try_cleanup_chain",
     &dispatch_nested_try_cleanup_chain},
    {"dispatch", "catch_order_and_boundaries",
     &dispatch_catch_order_and_boundaries},
    {"dispatch", "nested_order_independent",
     &dispatch_nested_order_independent},
    {"dispatch", "state_outside_any_try",
     &dispatch_state_outside_any_try},
    {"dispatch", "zero_catch_try_block", &dispatch_zero_catch_try_block},
    {"dispatch", "funclet_and_rethrow_unsupported",
     &dispatch_funclet_and_rethrow_unsupported},

    {"integrity", "duplicate_ip_rejected", &integrity_duplicate_ip_rejected},
    {"integrity", "unwind_cycle_rejected", &integrity_unwind_cycle_rejected},
    {"integrity", "unreachable_stop_rejected",
     &integrity_unreachable_stop_rejected},
    {"integrity", "out_of_range_state", &integrity_out_of_range_state},
    {"integrity", "overlapping_try_rejected",
     &integrity_overlapping_try_rejected},
    {"integrity", "overlapping_functions",
     &integrity_overlapping_functions},
    {"integrity", "unmapped_and_bad_magic",
     &integrity_unmapped_and_bad_magic},
    {"integrity", "chain_and_version_unsupported",
     &integrity_chain_and_version_unsupported},
    {"integrity", "reserved_unwind_flags",
     &integrity_reserved_unwind_flags},
    {"integrity", "odd_countofcodes_alignment",
     &integrity_odd_countofcodes_alignment},
};

const std::size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
