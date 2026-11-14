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

#include <pe-parse/cxx_eh3.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace peparse {

namespace {

constexpr std::uint32_t EH3_MAGIC = 0x19930522u;
constexpr std::uint8_t UNW_FLAG_EHANDLER = 0x01u;
constexpr std::uint8_t UNW_FLAG_UHANDLER = 0x02u;
constexpr std::uint8_t UNW_FLAG_CHAININFO = 0x04u;

constexpr std::uint32_t FUNCINFO_SIZE = 40u;
constexpr std::uint32_t UNWIND_ENTRY_SIZE = 8u;
constexpr std::uint32_t TRYBLOCK_ENTRY_SIZE = 20u;
constexpr std::uint32_t HANDLERTYPE_SIZE = 20u;
constexpr std::uint32_t IPSTATE_ENTRY_SIZE = 8u;
constexpr std::uint32_t RUNTIME_FUNCTION_SIZE = 12u;

struct SectionView {
  RVA rva = 0;
  std::uint32_t virtualSize = 0;
  std::uint32_t rawSize = 0;
  std::uint32_t characteristics = 0;
  bounded_buffer *data = nullptr;
};

// Resolves image RVAs through the parsed section table and performs every
// raw byte access through the library's production read helpers. Reads are
// little-endian; signed fields share their bit pattern with the unsigned
// readers.
class ImageReader {
public:
  explicit ImageReader(const parsed_pe *pe) {
    IterSec(const_cast<parsed_pe *>(pe), &ImageReader::collectSection, this);
    std::sort(views_.begin(), views_.end(), [](const SectionView &a,
                                               const SectionView &b) {
      return a.rva < b.rva;
    });
  }

  bool readU8(RVA rva, std::uint8_t &out) const {
    const SectionView *view = nullptr;
    std::uint32_t offset = 0;
    if (!locate(rva, view, offset) || offset >= view->rawSize) {
      return false;
    }
    return readByte(view->data, offset, out);
  }

  bool readU16(RVA rva, std::uint16_t &out) const {
    const SectionView *view = nullptr;
    std::uint32_t offset = 0;
    if (!locate(rva, view, offset)) {
      return false;
    }
    return readWord(view->data, offset, out);
  }

  bool readU32(RVA rva, std::uint32_t &out) const {
    const SectionView *view = nullptr;
    std::uint32_t offset = 0;
    if (!locate(rva, view, offset)) {
      return false;
    }
    return readDword(view->data, offset, out);
  }

  bool readS32(RVA rva, std::int32_t &out) const {
    std::uint32_t raw = 0;
    if (!readU32(rva, raw)) {
      return false;
    }
    std::memcpy(&out, &raw, sizeof(out));
    return true;
  }

  bool rangeMapped(RVA rva, std::uint32_t size) const {
    if (size == 0u) {
      return true;
    }
    std::uint64_t end = static_cast<std::uint64_t>(rva) + size;
    if (end > 0xFFFFFFFFull) {
      return false;
    }
    RVA cur = rva;
    while (cur < static_cast<RVA>(end)) {
      const SectionView *view = nullptr;
      std::uint32_t offset = 0;
      if (!locate(cur, view, offset) || offset >= view->rawSize) {
        return false;
      }
      std::uint32_t available = view->rawSize - offset;
      std::uint64_t sectionEnd =
          static_cast<std::uint64_t>(view->rva) + view->virtualSize;
      std::uint32_t span = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(sectionEnd, end) - cur);
      span = std::min(span, available);
      if (span == 0u) {
        return false;
      }
      cur += span;
    }
    return true;
  }

  bool rangeExecutable(RVA rva, std::uint32_t size) const {
    if (size == 0u) {
      return false;
    }
    std::uint64_t end = static_cast<std::uint64_t>(rva) + size;
    if (end > 0xFFFFFFFFull) {
      return false;
    }
    RVA cur = rva;
    while (cur < static_cast<RVA>(end)) {
      const SectionView *view = nullptr;
      std::uint32_t offset = 0;
      if (!locate(cur, view, offset) || offset >= view->rawSize ||
          (view->characteristics & IMAGE_SCN_MEM_EXECUTE) == 0u) {
        return false;
      }
      std::uint32_t available = view->rawSize - offset;
      std::uint64_t sectionEnd =
          static_cast<std::uint64_t>(view->rva) + view->virtualSize;
      std::uint32_t span = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(sectionEnd, end) - cur);
      span = std::min(span, available);
      cur += span;
    }
    return true;
  }

  bool executableRva(RVA rva) const {
    const SectionView *view = nullptr;
    std::uint32_t offset = 0;
    return locate(rva, view, offset) && offset < view->rawSize &&
           (view->characteristics & IMAGE_SCN_MEM_EXECUTE) != 0u;
  }

private:
  static int collectSection(void *cbd,
                            const VA &sectionBase,
                            const std::string &,
                            const image_section_header &sec,
                            const bounded_buffer *data) {
    (void)sectionBase;
    ImageReader *reader = static_cast<ImageReader *>(cbd);
    SectionView view;
    view.rva = sec.VirtualAddress;
    view.virtualSize = sec.Misc.VirtualSize;
    view.rawSize = data->bufLen;
    view.characteristics = sec.Characteristics;
    view.data = const_cast<bounded_buffer *>(data);
    reader->views_.push_back(view);
    return 0;
  }

  bool locate(RVA rva,
              const SectionView *&view,
              std::uint32_t &offset) const {
    view = nullptr;
    for (const SectionView &candidate : views_) {
      std::uint64_t end =
          static_cast<std::uint64_t>(candidate.rva) + candidate.virtualSize;
      if (rva >= candidate.rva &&
          static_cast<std::uint64_t>(rva) < end) {
        view = &candidate;
        offset = rva - candidate.rva;
        return true;
      }
    }
    return false;
  }

  std::vector<SectionView> views_;
};

CxxEh3Error makeError(CxxEh3Status status, RVA rva, std::string message) {
  CxxEh3Error error;
  error.status = status;
  error.sourceRva = rva;
  error.message = std::move(message);
  return error;
}

bool productAdd(RVA base,
                std::uint64_t count,
                std::uint32_t itemSize,
                RVA &first,
                RVA &pastEnd) {
  std::uint64_t bytes = count * itemSize;
  if (bytes > 0xFFFFFFFFull) {
    return false;
  }
  std::uint64_t end = static_cast<std::uint64_t>(base) + bytes;
  if (end > 0xFFFFFFFFull) {
    return false;
  }
  first = base;
  pastEnd = static_cast<RVA>(end);
  return true;
}

struct RuntimeEntry {
  RVA directoryEntryRva = 0;
  RVA beginRva = 0;
  RVA endRva = 0;
  RVA unwindInfoRva = 0;
};

// Walks UnwindMap states from fromState. When stopState is non-null the walk
// stops as soon as that state is reached (the stop state's action is not
// recorded); otherwise it stops at -1 (the function exit state).
CxxEh3Status walkChain(const CxxEh3FuncInfo &funcInfo,
                       std::int32_t fromState,
                       const std::int32_t *stopState,
                       std::vector<CxxEh3UnwindStep> &steps,
                       CxxEh3Error &error) {
  steps.clear();
  std::set<std::int32_t> visited;
  std::int32_t cur = fromState;
  while (true) {
    if (stopState != nullptr && cur == *stopState) {
      return CxxEh3Status::Ok;
    }
    if (cur == -1) {
      if (stopState != nullptr) {
        error = makeError(CxxEh3Status::UnreachableStop,
                          funcInfo.rva,
                          "cleanup chain reaches state -1 before the stop "
                          "state");
        return CxxEh3Status::UnreachableStop;
      }
      return CxxEh3Status::Ok;
    }
    if (cur < 0 || cur >= funcInfo.maxState) {
      error = makeError(CxxEh3Status::OutOfRangeState,
                        funcInfo.unwindMapRva,
                        "cleanup chain leaves the legal state range");
      return CxxEh3Status::OutOfRangeState;
    }
    if (!visited.insert(cur).second) {
      error = makeError(CxxEh3Status::Cycle,
                        funcInfo.unwindMapRva +
                            static_cast<RVA>(cur) * UNWIND_ENTRY_SIZE,
                        "UnwindMap chain contains a cycle");
      return CxxEh3Status::Cycle;
    }
    const CxxEh3UnwindAction &action =
        funcInfo.unwindMap[static_cast<std::size_t>(cur)];
    CxxEh3UnwindStep step;
    step.fromState = cur;
    step.toState = action.toState;
    step.actionRva = action.actionRva;
    step.unwindEntryRva = action.entryRva;
    steps.push_back(step);
    cur = action.toState;
  }
}

class GraphBuilder {
public:
  GraphBuilder(const parsed_pe *pe,
               const RVA *handlerRvas,
               std::size_t handlerCount,
               CxxEh3Graph &graph,
               CxxEh3Error &error)
      : pe_(pe), reader_(pe), graph_(graph), error_(error) {
    for (std::size_t i = 0; i < handlerCount; ++i) {
      handlerRvas_.insert(handlerRvas[i]);
    }
  }

  CxxEh3Status build() {
    graph_ = CxxEh3Graph();

    if (pe_->peHeader.nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        pe_->peHeader.nt.OptionalMagic != NT_OPTIONAL_64_MAGIC) {
      error_ = makeError(CxxEh3Status::Unsupported,
                         0,
                         "EH3 recovery requires an AMD64 PE32+ image");
      return CxxEh3Status::Unsupported;
    }

    const data_directory &dir =
        pe_->peHeader.nt.OptionalHeader64.DataDirectory[DIR_EXCEPTION];
    if (dir.VirtualAddress == 0u || dir.Size == 0u) {
      // No exception directory: publish an empty graph.
      return CxxEh3Status::Ok;
    }
    if (dir.Size % RUNTIME_FUNCTION_SIZE != 0u) {
      fail(CxxEh3Status::Corrupt,
           dir.VirtualAddress,
           "exception directory size is not a multiple of RUNTIME_FUNCTION");
      return CxxEh3Status::Corrupt;
    }
    if (!reader_.rangeMapped(dir.VirtualAddress, dir.Size)) {
      fail(CxxEh3Status::UnmappedRead,
           dir.VirtualAddress,
           "exception directory is not mapped");
      return CxxEh3Status::UnmappedRead;
    }

    std::uint32_t entryCount = dir.Size / RUNTIME_FUNCTION_SIZE;
    std::vector<RuntimeEntry> entries;
    entries.reserve(entryCount);
    for (std::uint32_t i = 0; i < entryCount; ++i) {
      RVA entryRva =
          dir.VirtualAddress + i * RUNTIME_FUNCTION_SIZE;
      RuntimeEntry entry;
      entry.directoryEntryRva = entryRva;
      if (!reader_.readU32(entryRva, entry.beginRva) ||
          !reader_.readU32(entryRva + 4u, entry.endRva) ||
          !reader_.readU32(entryRva + 8u, entry.unwindInfoRva)) {
        fail(CxxEh3Status::UnmappedRead,
             entryRva,
             "RUNTIME_FUNCTION entry is not fully readable");
        return CxxEh3Status::UnmappedRead;
      }
      if (entry.beginRva == 0u || entry.endRva <= entry.beginRva) {
        fail(CxxEh3Status::Corrupt,
             entryRva,
             "RUNTIME_FUNCTION has an empty or inverted code range");
        return CxxEh3Status::Corrupt;
      }
      entries.push_back(entry);
    }

    std::sort(entries.begin(),
              entries.end(),
              [](const RuntimeEntry &a, const RuntimeEntry &b) {
                if (a.beginRva != b.beginRva) {
                  return a.beginRva < b.beginRva;
                }
                return a.endRva < b.endRva;
              });
    for (std::size_t i = 1; i < entries.size(); ++i) {
      if (entries[i].beginRva < entries[i - 1].endRva) {
        fail(CxxEh3Status::Corrupt,
             entries[i].directoryEntryRva,
             "runtime function code ranges overlap");
        return CxxEh3Status::Corrupt;
      }
    }

    for (const RuntimeEntry &entry : entries) {
      CxxEh3Status status = processEntry(entry);
      if (status != CxxEh3Status::Ok) {
        graph_ = CxxEh3Graph();
        return status;
      }
    }

    std::sort(graph_.runtimeFunctions.begin(),
              graph_.runtimeFunctions.end(),
              [](const CxxEh3RuntimeFunction &a,
                 const CxxEh3RuntimeFunction &b) {
                if (a.beginRva != b.beginRva) {
                  return a.beginRva < b.beginRva;
                }
                return a.endRva < b.endRva;
              });
    return CxxEh3Status::Ok;
  }

private:
  void fail(CxxEh3Status status, RVA rva, const std::string &message) {
    graph_ = CxxEh3Graph();
    error_ = makeError(status, rva, message);
  }

  CxxEh3Status processEntry(const RuntimeEntry &entry) {
    std::uint32_t rangeSize = entry.endRva - entry.beginRva;
    if (!reader_.rangeExecutable(entry.beginRva, rangeSize)) {
      fail(CxxEh3Status::Corrupt,
           entry.directoryEntryRva,
           "runtime function code is not in an executable section");
      return CxxEh3Status::Corrupt;
    }
    if (!reader_.rangeMapped(entry.unwindInfoRva, 4u)) {
      fail(CxxEh3Status::UnmappedRead,
           entry.unwindInfoRva,
           "UNWIND_INFO header is not mapped");
      return CxxEh3Status::UnmappedRead;
    }

    std::uint8_t versionFlags = 0;
    std::uint8_t countOfCodes = 0;
    if (!reader_.readU8(entry.unwindInfoRva, versionFlags) ||
        !reader_.readU8(entry.unwindInfoRva + 2u, countOfCodes)) {
      fail(CxxEh3Status::UnmappedRead,
           entry.unwindInfoRva,
           "UNWIND_INFO header is not readable");
      return CxxEh3Status::UnmappedRead;
    }

    // The byte packs the three-bit version in its low bits and the five
    // UNW_FLAG_* bits above them; shift the flags back down before testing.
    std::uint8_t version = versionFlags & 0x07u;
    std::uint8_t flags = versionFlags >> 3;
    if (version != 1u) {
      fail(CxxEh3Status::Unsupported,
           entry.unwindInfoRva,
           "only version 1 UNWIND_INFO is supported");
      return CxxEh3Status::Unsupported;
    }
    if ((flags & UNW_FLAG_CHAININFO) != 0u) {
      fail(CxxEh3Status::Unsupported,
           entry.unwindInfoRva,
           "chained UNWIND_INFO is not supported");
      return CxxEh3Status::Unsupported;
    }
    if ((flags &
         ~(UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER | UNW_FLAG_CHAININFO)) !=
        0u) {
      fail(CxxEh3Status::Corrupt,
           entry.unwindInfoRva,
           "UNWIND_INFO sets reserved flag bits");
      return CxxEh3Status::Corrupt;
    }
    if ((flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) == 0u) {
      CxxEh3UnrecognizedRecord record;
      record.directoryEntryRva = entry.directoryEntryRva;
      record.beginRva = entry.beginRva;
      record.endRva = entry.endRva;
      record.unwindInfoRva = entry.unwindInfoRva;
      record.reason = CxxEh3UnrecognizedReason::NoLanguageHandler;
      graph_.unrecognized.push_back(record);
      return CxxEh3Status::Ok;
    }

    std::uint32_t alignedCodes =
        (static_cast<std::uint32_t>(countOfCodes) + 1u) & ~1u;
    std::uint64_t slot =
        static_cast<std::uint64_t>(entry.unwindInfoRva) + 4u +
        static_cast<std::uint64_t>(alignedCodes) * 2u;
    if (slot + 8u > 0xFFFFFFFFull) {
      fail(CxxEh3Status::Corrupt,
           entry.unwindInfoRva,
           "UNWIND_INFO handler slot runs past the image address space");
      return CxxEh3Status::Corrupt;
    }
    RVA handlerSlot = static_cast<RVA>(slot);
    RVA funcInfoSlot = handlerSlot + 4u;
    if (!reader_.rangeMapped(handlerSlot, 8u)) {
      fail(CxxEh3Status::UnmappedRead,
           handlerSlot,
           "UNWIND_INFO language handler slot is not mapped");
      return CxxEh3Status::UnmappedRead;
    }

    RVA personalityRva = 0;
    RVA funcInfoRva = 0;
    if (!reader_.readU32(handlerSlot, personalityRva) ||
        !reader_.readU32(funcInfoSlot, funcInfoRva)) {
      fail(CxxEh3Status::UnmappedRead,
           handlerSlot,
           "UNWIND_INFO language handler slot is not readable");
      return CxxEh3Status::UnmappedRead;
    }

    if (handlerRvas_.find(personalityRva) == handlerRvas_.end()) {
      CxxEh3UnrecognizedRecord record;
      record.directoryEntryRva = entry.directoryEntryRva;
      record.beginRva = entry.beginRva;
      record.endRva = entry.endRva;
      record.unwindInfoRva = entry.unwindInfoRva;
      record.personalityRva = personalityRva;
      record.reason = CxxEh3UnrecognizedReason::UnknownPersonality;
      graph_.unrecognized.push_back(record);
      return CxxEh3Status::Ok;
    }

    std::size_t funcInfoIndex = 0;
    CxxEh3Status status =
        getOrParseFuncInfo(funcInfoRva, funcInfoIndex);
    if (status != CxxEh3Status::Ok) {
      return status;
    }

    CxxEh3RuntimeFunction runtime;
    runtime.directoryEntryRva = entry.directoryEntryRva;
    runtime.beginRva = entry.beginRva;
    runtime.endRva = entry.endRva;
    runtime.unwindInfoRva = entry.unwindInfoRva;
    runtime.personalityRva = personalityRva;
    runtime.funcInfoRva = funcInfoRva;
    runtime.funcInfoIndex = funcInfoIndex;
    status = sliceIntervals(graph_.funcInfos[funcInfoIndex], runtime);
    if (status != CxxEh3Status::Ok) {
      return status;
    }
    graph_.runtimeFunctions.push_back(runtime);
    return CxxEh3Status::Ok;
  }

  CxxEh3Status getOrParseFuncInfo(RVA funcInfoRva, std::size_t &index) {
    auto found = funcInfoIndices_.find(funcInfoRva);
    if (found != funcInfoIndices_.end()) {
      index = found->second;
      return CxxEh3Status::Ok;
    }
    if (!reader_.rangeMapped(funcInfoRva, FUNCINFO_SIZE)) {
      fail(CxxEh3Status::UnmappedRead,
           funcInfoRva,
           "FuncInfo record is not mapped");
      return CxxEh3Status::UnmappedRead;
    }

    CxxEh3FuncInfo funcInfo;
    funcInfo.rva = funcInfoRva;
    std::uint32_t magic = 0;
    std::uint32_t maxState = 0;
    std::uint32_t numTryBlocks = 0;
    std::uint32_t ipMapEntries = 0;
    if (!reader_.readU32(funcInfoRva, magic) ||
        !reader_.readU32(funcInfoRva + 4u, maxState) ||
        !reader_.readU32(funcInfoRva + 8u, funcInfo.unwindMapRva) ||
        !reader_.readU32(funcInfoRva + 12u, numTryBlocks) ||
        !reader_.readU32(funcInfoRva + 16u, funcInfo.tryBlockMapRva) ||
        !reader_.readU32(funcInfoRva + 20u, ipMapEntries) ||
        !reader_.readU32(funcInfoRva + 24u, funcInfo.ipToStateMapRva) ||
        !reader_.readS32(funcInfoRva + 28u, funcInfo.unwindHelp) ||
        !reader_.readU32(funcInfoRva + 32u, funcInfo.esTypeListRva) ||
        !reader_.readU32(funcInfoRva + 36u, funcInfo.ehFlags)) {
      fail(CxxEh3Status::UnmappedRead,
           funcInfoRva,
           "FuncInfo record is not readable");
      return CxxEh3Status::UnmappedRead;
    }
    if (magic != EH3_MAGIC) {
      fail(CxxEh3Status::Corrupt,
           funcInfoRva,
           "FuncInfo magic is not 0x19930522");
      return CxxEh3Status::Corrupt;
    }
    if (funcInfo.esTypeListRva != 0u) {
      fail(CxxEh3Status::Unsupported,
           funcInfoRva + 32u,
           "FuncInfo ESTypeList is not supported");
      return CxxEh3Status::Unsupported;
    }
    if (funcInfo.ehFlags > 1u) {
      fail(CxxEh3Status::Unsupported,
           funcInfoRva + 36u,
           "reserved EHFlags bits are set");
      return CxxEh3Status::Unsupported;
    }
    if (maxState > static_cast<std::uint32_t>(
                       0x7FFFFFFFu - static_cast<std::int32_t>(1))) {
      fail(CxxEh3Status::Corrupt, funcInfoRva, "MaxState is implausible");
      return CxxEh3Status::Corrupt;
    }
    funcInfo.maxState = static_cast<std::int32_t>(maxState);

    CxxEh3Status status = parseUnwindMap(funcInfo);
    if (status != CxxEh3Status::Ok) {
      return status;
    }
    status = parseTryBlocks(funcInfo, numTryBlocks);
    if (status != CxxEh3Status::Ok) {
      return status;
    }
    status = parseIpMap(funcInfo, ipMapEntries);
    if (status != CxxEh3Status::Ok) {
      return status;
    }
    status = validateUnwindReachability(funcInfo);
    if (status != CxxEh3Status::Ok) {
      return status;
    }
    status = validateTryNesting(funcInfo);
    if (status != CxxEh3Status::Ok) {
      return status;
    }
    status = validateTryStopReachability(funcInfo);
    if (status != CxxEh3Status::Ok) {
      return status;
    }

    index = graph_.funcInfos.size();
    funcInfoIndices_[funcInfoRva] = index;
    graph_.funcInfos.push_back(std::move(funcInfo));
    return CxxEh3Status::Ok;
  }

  CxxEh3Status parseUnwindMap(CxxEh3FuncInfo &funcInfo) {
    std::uint32_t count = static_cast<std::uint32_t>(funcInfo.maxState);
    RVA first = 0;
    RVA pastEnd = 0;
    if (!productAdd(
            funcInfo.unwindMapRva, count, UNWIND_ENTRY_SIZE, first, pastEnd)) {
      fail(CxxEh3Status::Corrupt,
           funcInfo.unwindMapRva,
           "UnwindMap size overflows the image address space");
      return CxxEh3Status::Corrupt;
    }
    if (count != 0u && !reader_.rangeMapped(first, pastEnd - first)) {
      fail(CxxEh3Status::UnmappedRead,
           funcInfo.unwindMapRva,
           "UnwindMap is not fully mapped");
      return CxxEh3Status::UnmappedRead;
    }
    funcInfo.unwindMap.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      RVA entryRva =
          funcInfo.unwindMapRva + i * UNWIND_ENTRY_SIZE;
      CxxEh3UnwindAction &action = funcInfo.unwindMap[i];
      action.state = static_cast<std::int32_t>(i);
      action.entryRva = entryRva;
      if (!reader_.readS32(entryRva, action.toState) ||
          !reader_.readU32(entryRva + 4u, action.actionRva)) {
        fail(CxxEh3Status::UnmappedRead,
             entryRva,
             "UnwindMap entry is not readable");
        return CxxEh3Status::UnmappedRead;
      }
      if (action.toState < -1 ||
          action.toState >= funcInfo.maxState) {
        fail(CxxEh3Status::OutOfRangeState,
             entryRva,
             "UnwindMap ToState is outside [-1, MaxState)");
        return CxxEh3Status::OutOfRangeState;
      }
      if (action.actionRva != 0u &&
          !reader_.executableRva(action.actionRva)) {
        fail(CxxEh3Status::Corrupt,
             entryRva + 4u,
             "UnwindMap action RVA is not executable code");
        return CxxEh3Status::Corrupt;
      }
    }
    return CxxEh3Status::Ok;
  }

  CxxEh3Status parseTryBlocks(CxxEh3FuncInfo &funcInfo,
                              std::uint32_t numTryBlocks) {
    RVA first = 0;
    RVA pastEnd = 0;
    if (!productAdd(funcInfo.tryBlockMapRva,
                    numTryBlocks,
                    TRYBLOCK_ENTRY_SIZE,
                    first,
                    pastEnd)) {
      fail(CxxEh3Status::Corrupt,
           funcInfo.tryBlockMapRva,
           "TryBlockMap size overflows the image address space");
      return CxxEh3Status::Corrupt;
    }
    if (numTryBlocks != 0u && !reader_.rangeMapped(first, pastEnd - first)) {
      fail(CxxEh3Status::UnmappedRead,
           funcInfo.tryBlockMapRva,
           "TryBlockMap is not fully mapped");
      return CxxEh3Status::UnmappedRead;
    }

    for (std::uint32_t i = 0; i < numTryBlocks; ++i) {
      RVA entryRva =
          funcInfo.tryBlockMapRva + i * TRYBLOCK_ENTRY_SIZE;
      CxxEh3TryBlock block;
      block.entryRva = entryRva;
      block.ordinal = i;
      if (!reader_.readS32(entryRva, block.tryLow) ||
          !reader_.readS32(entryRva + 4u, block.tryHigh) ||
          !reader_.readS32(entryRva + 8u, block.catchHigh) ||
          !reader_.readU32(entryRva + 12u, block.numCatches) ||
          !reader_.readU32(entryRva + 16u, block.handlerArrayRva)) {
        fail(CxxEh3Status::UnmappedRead,
             entryRva,
             "TryBlockMap entry is not readable");
        return CxxEh3Status::UnmappedRead;
      }
      if (!(block.tryLow >= 0 && block.tryLow <= block.tryHigh &&
            block.tryHigh < block.catchHigh &&
            block.catchHigh < funcInfo.maxState)) {
        fail(CxxEh3Status::Corrupt,
             entryRva,
             "TryBlockMap state range violates 0<=TryLow<=TryHigh<"
             "CatchHigh<MaxState");
        return CxxEh3Status::Corrupt;
      }

      RVA handlersFirst = 0;
      RVA handlersPastEnd = 0;
      if (!productAdd(block.handlerArrayRva,
                      block.numCatches,
                      HANDLERTYPE_SIZE,
                      handlersFirst,
                      handlersPastEnd)) {
        fail(CxxEh3Status::Corrupt,
             block.handlerArrayRva,
             "HandlerType array size overflows the image address space");
        return CxxEh3Status::Corrupt;
      }
      // A zero NumCatches carries no HandlerType array (zero counts permit a
      // zero RVA); such a try block still validates its state ranges but
      // contributes no catch candidates at query time.
      if (block.numCatches != 0u) {
        if (!reader_.rangeMapped(handlersFirst,
                                 handlersPastEnd - handlersFirst)) {
          fail(CxxEh3Status::UnmappedRead,
               block.handlerArrayRva,
               "HandlerType array is not fully mapped");
          return CxxEh3Status::UnmappedRead;
        }
      }

      block.handlers.resize(block.numCatches);
      for (std::uint32_t j = 0; j < block.numCatches; ++j) {
        RVA handlerEntryRva =
            block.handlerArrayRva + j * HANDLERTYPE_SIZE;
        CxxEh3HandlerType &handler = block.handlers[j];
        handler.entryRva = handlerEntryRva;
        handler.ordinal = j;
        if (!reader_.readU32(handlerEntryRva, handler.adjectives) ||
            !reader_.readU32(handlerEntryRva + 4u,
                             handler.typeDescriptorRva) ||
            !reader_.readS32(handlerEntryRva + 8u,
                             handler.catchObjOffset) ||
            !reader_.readU32(handlerEntryRva + 12u, handler.handlerRva) ||
            !reader_.readS32(handlerEntryRva + 16u,
                             handler.parentFrameOffset)) {
          fail(CxxEh3Status::UnmappedRead,
               handlerEntryRva,
               "HandlerType record is not readable");
          return CxxEh3Status::UnmappedRead;
        }
        if (handler.handlerRva == 0u ||
            !reader_.executableRva(handler.handlerRva)) {
          fail(CxxEh3Status::Corrupt,
               handlerEntryRva + 12u,
               "catch handler RVA is not executable code");
          return CxxEh3Status::Corrupt;
        }
      }
      funcInfo.tryBlocks.push_back(std::move(block));
    }
    return CxxEh3Status::Ok;
  }

  CxxEh3Status parseIpMap(CxxEh3FuncInfo &funcInfo,
                          std::uint32_t ipMapEntries) {
    RVA first = 0;
    RVA pastEnd = 0;
    if (!productAdd(funcInfo.ipToStateMapRva,
                    ipMapEntries,
                    IPSTATE_ENTRY_SIZE,
                    first,
                    pastEnd)) {
      fail(CxxEh3Status::Corrupt,
           funcInfo.ipToStateMapRva,
           "IPToStateMap size overflows the image address space");
      return CxxEh3Status::Corrupt;
    }
    if (ipMapEntries != 0u && !reader_.rangeMapped(first, pastEnd - first)) {
      fail(CxxEh3Status::UnmappedRead,
           funcInfo.ipToStateMapRva,
           "IPToStateMap is not fully mapped");
      return CxxEh3Status::UnmappedRead;
    }

    funcInfo.ipMap.resize(ipMapEntries);
    RVA previousIp = 0;
    for (std::uint32_t i = 0; i < ipMapEntries; ++i) {
      RVA entryRva =
          funcInfo.ipToStateMapRva + i * IPSTATE_ENTRY_SIZE;
      CxxEh3IpState &entry = funcInfo.ipMap[i];
      entry.entryRva = entryRva;
      if (!reader_.readU32(entryRva, entry.ipRva) ||
          !reader_.readS32(entryRva + 4u, entry.state)) {
        fail(CxxEh3Status::UnmappedRead,
             entryRva,
             "IPToStateMap entry is not readable");
        return CxxEh3Status::UnmappedRead;
      }
      if (i != 0u && entry.ipRva <= previousIp) {
        fail(CxxEh3Status::Corrupt,
             entryRva,
             "IPToStateMap IPs must be strictly increasing");
        return CxxEh3Status::Corrupt;
      }
      if (entry.state < -1 || entry.state >= funcInfo.maxState) {
        fail(CxxEh3Status::OutOfRangeState,
             entryRva + 4u,
             "IPToStateMap state is outside [-1, MaxState)");
        return CxxEh3Status::OutOfRangeState;
      }
      if (entry.ipRva == 0u || !reader_.executableRva(entry.ipRva)) {
        fail(CxxEh3Status::Corrupt,
             entryRva,
             "IPToStateMap IP is not executable code");
        return CxxEh3Status::Corrupt;
      }
      previousIp = entry.ipRva;
    }
    return CxxEh3Status::Ok;
  }

  CxxEh3Status
  validateUnwindReachability(const CxxEh3FuncInfo &funcInfo) {
    for (std::int32_t state = 0; state < funcInfo.maxState; ++state) {
      std::vector<CxxEh3UnwindStep> steps;
      CxxEh3Error walkError;
      CxxEh3Status status =
          walkChain(funcInfo, state, nullptr, steps, walkError);
      if (status != CxxEh3Status::Ok) {
        error_ = walkError;
        graph_ = CxxEh3Graph();
        return status;
      }
    }
    return CxxEh3Status::Ok;
  }

  CxxEh3Status validateTryNesting(const CxxEh3FuncInfo &funcInfo) {
    const std::vector<CxxEh3TryBlock> &blocks = funcInfo.tryBlocks;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      for (std::size_t j = i + 1; j < blocks.size(); ++j) {
        const CxxEh3TryBlock &a = blocks[i];
        const CxxEh3TryBlock &b = blocks[j];
        bool disjoint = a.tryHigh < b.tryLow || b.tryHigh < a.tryLow;
        bool nested =
            (a.tryLow <= b.tryLow && b.tryHigh <= a.tryHigh) ||
            (b.tryLow <= a.tryLow && a.tryHigh <= b.tryHigh);
        if (!disjoint && !nested) {
          fail(CxxEh3Status::OverlappingTry,
               b.entryRva,
               "try guard ranges partially overlap instead of nesting");
          return CxxEh3Status::OverlappingTry;
        }
      }
    }
    return CxxEh3Status::Ok;
  }

  CxxEh3Status validateTryStopReachability(const CxxEh3FuncInfo &funcInfo) {
    for (const CxxEh3TryBlock &block : funcInfo.tryBlocks) {
      for (std::int32_t state = block.tryLow; state <= block.tryHigh;
           ++state) {
        std::vector<CxxEh3UnwindStep> steps;
        CxxEh3Error walkError;
        std::int32_t stop = block.tryLow;
        CxxEh3Status status =
            walkChain(funcInfo, state, &stop, steps, walkError);
        if (status != CxxEh3Status::Ok) {
          if (status == CxxEh3Status::UnreachableStop) {
            walkError.sourceRva = block.entryRva;
          }
          error_ = walkError;
          graph_ = CxxEh3Graph();
          return status;
        }
      }
    }
    return CxxEh3Status::Ok;
  }

  // Splits one runtime function's half-open range into constant-state
  // intervals using predecessor IPToStateMap entries. Holes between runtime
  // functions are never visited here.
  CxxEh3Status sliceIntervals(const CxxEh3FuncInfo &funcInfo,
                              CxxEh3RuntimeFunction &runtime) {
    std::int32_t currentState = -1;
    RVA currentBegin = runtime.beginRva;
    RVA currentSource = 0;

    for (const CxxEh3IpState &entry : funcInfo.ipMap) {
      if (entry.ipRva <= runtime.beginRva) {
        currentState = entry.state;
        currentSource = entry.entryRva;
        continue;
      }
      if (entry.ipRva >= runtime.endRva) {
        break;
      }
      if (currentBegin < entry.ipRva) {
        CxxEh3StateInterval interval;
        interval.beginRva = currentBegin;
        interval.endRva = entry.ipRva;
        interval.state = currentState;
        interval.sourceEntryRva = currentSource;
        runtime.intervals.push_back(interval);
      }
      currentBegin = entry.ipRva;
      currentState = entry.state;
      currentSource = entry.entryRva;
    }
    if (currentBegin < runtime.endRva) {
      CxxEh3StateInterval interval;
      interval.beginRva = currentBegin;
      interval.endRva = runtime.endRva;
      interval.state = currentState;
      interval.sourceEntryRva = currentSource;
      runtime.intervals.push_back(interval);
    }
    return CxxEh3Status::Ok;
  }

  const parsed_pe *pe_;
  ImageReader reader_;
  CxxEh3Graph &graph_;
  CxxEh3Error &error_;
  std::set<RVA> handlerRvas_;
  std::map<RVA, std::size_t> funcInfoIndices_;
};

} // namespace

const char *CxxEh3StatusString(CxxEh3Status status) {
  switch (status) {
    case CxxEh3Status::Ok:
      return "ok";
    case CxxEh3Status::Unsupported:
      return "unsupported";
    case CxxEh3Status::Corrupt:
      return "corrupt";
    case CxxEh3Status::UnmappedRead:
      return "unmapped read";
    case CxxEh3Status::OutOfRangeState:
      return "state out of range";
    case CxxEh3Status::Cycle:
      return "unwind cycle";
    case CxxEh3Status::UnreachableStop:
      return "stop state unreachable";
    case CxxEh3Status::OverlappingTry:
      return "overlapping try ranges";
    case CxxEh3Status::Uncovered:
      return "address not covered";
    case CxxEh3Status::InvalidArgument:
      return "invalid argument";
    default:
      break;
  }
  return "unknown";
}

CxxEh3Status BuildCxxEh3Graph(const parsed_pe *pe,
                              const RVA *handlerRvas,
                              std::size_t handlerCount,
                              CxxEh3Graph &graph,
                              CxxEh3Error &error) {
  if (pe == nullptr) {
    error = makeError(CxxEh3Status::InvalidArgument,
                      0,
                      "parsed_pe must not be null");
    graph = CxxEh3Graph();
    return CxxEh3Status::InvalidArgument;
  }
  if (handlerRvas == nullptr && handlerCount != 0u) {
    error = makeError(CxxEh3Status::InvalidArgument,
                      0,
                      "handler RVA array must not be null");
    graph = CxxEh3Graph();
    return CxxEh3Status::InvalidArgument;
  }
  GraphBuilder builder(pe, handlerRvas, handlerCount, graph, error);
  return builder.build();
}

CxxEh3Status BuildCxxEh3Graph(const parsed_pe *pe,
                              const std::vector<RVA> &handlerRvas,
                              CxxEh3Graph &graph,
                              CxxEh3Error &error) {
  return BuildCxxEh3Graph(pe,
                          handlerRvas.empty() ? nullptr : handlerRvas.data(),
                          handlerRvas.size(),
                          graph,
                          error);
}

CxxEh3Status QueryCxxEh3Paths(const CxxEh3Graph &graph,
                              RVA throwRva,
                              bool firstThrowInBody,
                              CxxEh3QueryResult &result,
                              CxxEh3Error &error) {
  result = CxxEh3QueryResult();
  result.throwRva = throwRva;

  const CxxEh3RuntimeFunction *runtime = nullptr;
  for (const CxxEh3RuntimeFunction &candidate : graph.runtimeFunctions) {
    if (throwRva >= candidate.beginRva && throwRva < candidate.endRva) {
      runtime = &candidate;
      break;
    }
  }
  if (runtime == nullptr) {
    error = makeError(CxxEh3Status::Uncovered,
                      throwRva,
                      "throw RVA is outside every recovered code range");
    return CxxEh3Status::Uncovered;
  }
  if (!firstThrowInBody) {
    error = makeError(CxxEh3Status::Unsupported,
                      throwRva,
                      "rethrows and funclet throws are not supported");
    return CxxEh3Status::Unsupported;
  }

  result.runtimeBeginRva = runtime->beginRva;
  result.runtimeEndRva = runtime->endRva;

  std::int32_t state = -1;
  bool found = false;
  for (const CxxEh3StateInterval &interval : runtime->intervals) {
    if (throwRva >= interval.beginRva && throwRva < interval.endRva) {
      state = interval.state;
      found = true;
      break;
    }
  }
  if (!found) {
    error = makeError(CxxEh3Status::Uncovered,
                      throwRva,
                      "throw RVA falls in an unmapped state interval");
    return CxxEh3Status::Uncovered;
  }
  result.state = state;

  const CxxEh3FuncInfo &funcInfo =
      graph.funcInfos[runtime->funcInfoIndex];

  if (state != -1) {
    for (const CxxEh3TryBlock &block : funcInfo.tryBlocks) {
      if (state > block.tryHigh && state <= block.catchHigh) {
        error = makeError(CxxEh3Status::Unsupported,
                          block.entryRva,
                          "throw state is inside a catch/cleanup funclet "
                          "region");
        return CxxEh3Status::Unsupported;
      }
    }
  }

  std::vector<const CxxEh3TryBlock *> matching;
  for (const CxxEh3TryBlock &block : funcInfo.tryBlocks) {
    if (state >= block.tryLow && state <= block.tryHigh) {
      matching.push_back(&block);
    }
  }
  // Inner-to-outer: the innermost guard ends first and starts last. Equal
  // ranges keep their original table order via ordinal.
  std::sort(matching.begin(),
            matching.end(),
            [](const CxxEh3TryBlock *a, const CxxEh3TryBlock *b) {
              if (a->tryHigh != b->tryHigh) {
                return a->tryHigh < b->tryHigh;
              }
              if (a->tryLow != b->tryLow) {
                return a->tryLow > b->tryLow;
              }
              return a->ordinal < b->ordinal;
            });

  for (const CxxEh3TryBlock *block : matching) {
    for (const CxxEh3HandlerType &handler : block->handlers) {
      CxxEh3Path path;
      path.escape = false;
      path.stopState = block->tryLow;
      path.tryBlockEntryRva = block->entryRva;
      path.tryOrdinal = block->ordinal;
      path.handlerEntryRva = handler.entryRva;
      path.handlerRva = handler.handlerRva;
      path.catchOrdinal = handler.ordinal;
      path.adjectives = handler.adjectives;
      path.typeDescriptorRva = handler.typeDescriptorRva;
      path.catchObjOffset = handler.catchObjOffset;
      path.parentFrameOffset = handler.parentFrameOffset;

      std::int32_t stop = block->tryLow;
      CxxEh3Status status = walkChain(
          funcInfo, state, &stop, path.steps, error);
      if (status != CxxEh3Status::Ok) {
        result = CxxEh3QueryResult();
        return status;
      }
      result.paths.push_back(std::move(path));
    }
  }

  CxxEh3Path escape;
  escape.escape = true;
  escape.stopState = -1;
  CxxEh3Status status =
      walkChain(funcInfo, state, nullptr, escape.steps, error);
  if (status != CxxEh3Status::Ok) {
    result = CxxEh3QueryResult();
    return status;
  }
  result.paths.push_back(std::move(escape));
  return CxxEh3Status::Ok;
}
} // namespace peparse
