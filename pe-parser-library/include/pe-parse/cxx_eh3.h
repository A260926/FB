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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <pe-parse/parse.h>

namespace peparse {

// Read-only EH3 (magic 0x19930522, __CxxFrameHandler3) control-flow recovery
// for AMD64 PE32+ images. Only version 1, non-chained UNWIND_INFO is handled.
//
// The personality routines that count as EH3 handlers are supplied by the
// caller as a set of RVAs; nothing is inferred from section names or byte
// patterns. A successfully built graph owns copies of all of its data, so it
// remains queryable after the source parsed_pe has been destroyed.

enum class CxxEh3Status {
  Ok = 0,
  // Chained UNWIND_INFO, unsupported UNWIND_INFO version, non-AMD64/PE32+
  // image, ESTypeList in use, reserved EHFlags bits, or a query that would
  // require runtime type matching / rethrow / funclet-throw semantics.
  Unsupported,
  // Structural damage: bad magic, malformed tables, overlapping functions,
  // non-executable code/action/handler RVAs, duplicate IP map entries, etc.
  Corrupt,
  // A required byte range is not mapped (or runs past the raw section data).
  UnmappedRead,
  // A state value outside [-1, MaxState).
  OutOfRangeState,
  // The UnwindMap does not terminate at -1 because of a loop.
  Cycle,
  // A guarded state's cleanup chain never reaches its try block's TryLow.
  UnreachableStop,
  // Two try guard ranges partially overlap instead of nesting or being
  // disjoint.
  OverlappingTry,
  // Query RVA lies outside every recovered runtime function range.
  Uncovered,
  InvalidArgument
};

const char *CxxEh3StatusString(CxxEh3Status status);

struct CxxEh3Error {
  CxxEh3Status status = CxxEh3Status::Ok;
  // Image RVA of the table entry / record that triggered the failure; zero
  // when the failure is not tied to a specific record.
  RVA sourceRva = 0;
  std::string message;
};

// One entry of a FuncInfo UnwindMap. The array index is the state whose
// frame this entry unwinds; actionRva == 0 means a state transition without
// a cleanup funclet, which still has to be followed during a walk.
struct CxxEh3UnwindAction {
  std::int32_t state = 0;
  std::int32_t toState = 0;
  RVA actionRva = 0;
  // RVA of the 8-byte UnwindMap entry in the image.
  RVA entryRva = 0;
};

// One HandlerType record, kept in its original TryBlockMap order. Type
// descriptors are never dereferenced; dynamic type matching is out of scope.
struct CxxEh3HandlerType {
  std::uint32_t adjectives = 0;
  RVA typeDescriptorRva = 0;
  std::int32_t catchObjOffset = 0;
  RVA handlerRva = 0;
  std::int32_t parentFrameOffset = 0;
  // RVA of the 20-byte HandlerType record in the image.
  RVA entryRva = 0;
  // Original order of this handler inside its try block.
  std::uint32_t ordinal = 0;
};

struct CxxEh3TryBlock {
  std::int32_t tryLow = 0;
  std::int32_t tryHigh = 0;
  std::int32_t catchHigh = 0;
  std::uint32_t numCatches = 0;
  RVA handlerArrayRva = 0;
  // RVA of the 20-byte TryBlockMap entry in the image.
  RVA entryRva = 0;
  // Original index in the TryBlockMap.
  std::uint32_t ordinal = 0;
  std::vector<CxxEh3HandlerType> handlers;
};

// One raw IPToStateMap entry. IPs are strictly increasing within a FuncInfo.
struct CxxEh3IpState {
  RVA ipRva = 0;
  std::int32_t state = 0;
  // RVA of the 8-byte IPToStateMap entry in the image.
  RVA entryRva = 0;
};

struct CxxEh3FuncInfo {
  RVA rva = 0;
  std::int32_t maxState = 0;
  RVA unwindMapRva = 0;
  RVA tryBlockMapRva = 0;
  RVA ipToStateMapRva = 0;
  std::int32_t unwindHelp = 0;
  RVA esTypeListRva = 0;
  std::uint32_t ehFlags = 0;
  // Indexed directly by state; size == MaxState.
  std::vector<CxxEh3UnwindAction> unwindMap;
  std::vector<CxxEh3TryBlock> tryBlocks;
  // The global IP-to-state map; shared by every runtime function associated
  // with this FuncInfo.
  std::vector<CxxEh3IpState> ipMap;
};

// A half-open [beginRva, endRva) interval of constant state inside one
// runtime function. A leading interval with state == -1 has sourceEntryRva
// == 0 because it precedes every IPToStateMap entry.
struct CxxEh3StateInterval {
  RVA beginRva = 0;
  RVA endRva = 0;
  std::int32_t state = 0;
  RVA sourceEntryRva = 0;
};

struct CxxEh3RuntimeFunction {
  // RVA of the 12-byte RUNTIME_FUNCTION entry in the exception directory.
  RVA directoryEntryRva = 0;
  RVA beginRva = 0;
  RVA endRva = 0;
  RVA unwindInfoRva = 0;
  RVA personalityRva = 0;
  RVA funcInfoRva = 0;
  // Index into CxxEh3Graph::funcInfos; several runtime functions may share
  // one FuncInfo identity.
  std::size_t funcInfoIndex = 0;
  // Intervals cover exactly [beginRva, endRva); holes between runtime
  // functions that share a FuncInfo are never represented as code.
  std::vector<CxxEh3StateInterval> intervals;
};

enum class CxxEh3UnrecognizedReason {
  // The UNWIND_INFO names a personality RVA the caller did not identify.
  UnknownPersonality,
  // The UNWIND_INFO carries no EHANDLER/UHANDLER language handler slot.
  NoLanguageHandler
};

struct CxxEh3UnrecognizedRecord {
  RVA directoryEntryRva = 0;
  RVA beginRva = 0;
  RVA endRva = 0;
  RVA unwindInfoRva = 0;
  RVA personalityRva = 0;
  CxxEh3UnrecognizedReason reason =
      CxxEh3UnrecognizedReason::UnknownPersonality;
};

struct CxxEh3Graph {
  std::vector<CxxEh3FuncInfo> funcInfos;
  // Sorted by beginRva.
  std::vector<CxxEh3RuntimeFunction> runtimeFunctions;
  std::vector<CxxEh3UnrecognizedRecord> unrecognized;

  bool empty() const {
    return funcInfos.empty() && runtimeFunctions.empty() &&
           unrecognized.empty();
  }
};

// One state transition on a cleanup walk. Zero-action transitions are
// recorded exactly like funclet transitions.
struct CxxEh3UnwindStep {
  std::int32_t fromState = 0;
  std::int32_t toState = 0;
  RVA actionRva = 0;
  // RVA of the UnwindMap entry that produced this step.
  RVA unwindEntryRva = 0;
};

// A statically possible route out of a throw site. Catch paths stop at the
// enclosing try block's TryLow without executing that state's action; the
// escape path unwinds to state -1 and leaves the function.
struct CxxEh3Path {
  bool escape = false;
  std::int32_t stopState = 0;
  // Catch-path provenance; zero on the escape path.
  RVA tryBlockEntryRva = 0;
  std::uint32_t tryOrdinal = 0;
  RVA handlerEntryRva = 0;
  RVA handlerRva = 0;
  std::uint32_t catchOrdinal = 0;
  std::uint32_t adjectives = 0;
  RVA typeDescriptorRva = 0;
  std::int32_t catchObjOffset = 0;
  std::int32_t parentFrameOffset = 0;
  std::vector<CxxEh3UnwindStep> steps;
};

struct CxxEh3QueryResult {
  RVA throwRva = 0;
  RVA runtimeBeginRva = 0;
  RVA runtimeEndRva = 0;
  std::int32_t state = 0;
  // Inner-to-outer try blocks, each with its catches in original table order,
  // followed by the single escape path.
  std::vector<CxxEh3Path> paths;
};

// Build an EH3 graph. handlerRvas is the caller-supplied set of recognized
// language-handler (personality) RVAs. On failure the output graph is cleared
// and error carries a source RVA. An image without an exception directory
// yields Ok and an empty graph.
CxxEh3Status BuildCxxEh3Graph(const parsed_pe *pe,
                              const RVA *handlerRvas,
                              std::size_t handlerCount,
                              CxxEh3Graph &graph,
                              CxxEh3Error &error);

CxxEh3Status BuildCxxEh3Graph(const parsed_pe *pe,
                              const std::vector<RVA> &handlerRvas,
                              CxxEh3Graph &graph,
                              CxxEh3Error &error);

// Query the static first-throw candidates at throwRva. firstThrowInBody must
// be true: rethrows and throws from catch/cleanup funclets are reported as
// Unsupported, as is any state in a (TryHigh, CatchHigh] funclet region.
// RVAs outside all recovered code ranges return Uncovered.
CxxEh3Status QueryCxxEh3Paths(const CxxEh3Graph &graph,
                              RVA throwRva,
                              bool firstThrowInBody,
                              CxxEh3QueryResult &result,
                              CxxEh3Error &error);
} // namespace peparse
