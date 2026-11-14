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

// Minimal read-only EH3 control-flow dump example. It takes a PE file, one
// or more hexadecimal personality (language handler) RVAs, and a throw RVA:
//
//   eh3_dump image.exe 0x1070 0x1060
//
// The graph is built before the parsed_pe context is destroyed and is then
// queried from its own data, demonstrating that the result is self-contained.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <pe-parse/cxx_eh3.h>
#include <pe-parse/parse.h>

namespace {

bool parseRva(const char *text, peparse::RVA &rva) {
  char *end = nullptr;
  unsigned long long value = std::strtoull(text, &end, 16);
  if (end == text || *end != '\0' || value > 0xFFFFFFFFull) {
    return false;
  }
  rva = static_cast<peparse::RVA>(value);
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <pe-file> <personality-rva> [throw-rva]\n",
                 argv[0]);
    return 2;
  }

  peparse::RVA personalityRva = 0;
  if (!parseRva(argv[2], personalityRva)) {
    std::fprintf(stderr, "bad personality RVA: %s\n", argv[2]);
    return 2;
  }

  peparse::parsed_pe *pe = peparse::ParsePEFromFile(argv[1]);
  if (pe == nullptr) {
    std::fprintf(stderr,
                 "parse failed: %s (%s)\n",
                 peparse::GetPEErrString().c_str(),
                 peparse::GetPEErrLoc().c_str());
    return 1;
  }

  std::vector<peparse::RVA> handlers;
  handlers.push_back(personalityRva);

  peparse::CxxEh3Graph graph;
  peparse::CxxEh3Error error;
  peparse::CxxEh3Status status =
      peparse::BuildCxxEh3Graph(pe, handlers, graph, error);
  peparse::DestructParsedPE(pe);

  if (status != peparse::CxxEh3Status::Ok) {
    std::fprintf(stderr,
                 "EH3 build failed: %s at RVA 0x%x: %s\n",
                 peparse::CxxEh3StatusString(status),
                 error.sourceRva,
                 error.message.c_str());
    return 1;
  }

  std::printf("funcInfos=%zu runtimeFunctions=%zu unrecognized=%zu\n",
              graph.funcInfos.size(),
              graph.runtimeFunctions.size(),
              graph.unrecognized.size());
  for (const peparse::CxxEh3RuntimeFunction &runtime :
       graph.runtimeFunctions) {
    std::printf("function [0x%x,0x%x) funcInfo=0x%x intervals=%zu\n",
                runtime.beginRva,
                runtime.endRva,
                runtime.funcInfoRva,
                runtime.intervals.size());
  }

  if (argc < 4) {
    return 0;
  }

  peparse::RVA throwRva = 0;
  if (!parseRva(argv[3], throwRva)) {
    std::fprintf(stderr, "bad throw RVA: %s\n", argv[3]);
    return 2;
  }

  peparse::CxxEh3QueryResult result;
  status = peparse::QueryCxxEh3Paths(
      graph, throwRva, /*firstThrowInBody=*/true, result, error);
  if (status != peparse::CxxEh3Status::Ok) {
    std::fprintf(stderr,
                 "EH3 query failed: %s at RVA 0x%x: %s\n",
                 peparse::CxxEh3StatusString(status),
                 error.sourceRva,
                 error.message.c_str());
    return 1;
  }

  std::printf("throw 0x%x state=%d paths=%zu\n",
              result.throwRva,
              result.state,
              result.paths.size());
  for (const peparse::CxxEh3Path &path : result.paths) {
    if (path.escape) {
      std::printf(" escape to -1, steps=%zu\n", path.steps.size());
    } else {
      std::printf(" catch try#%u catch#%u handler=0x%x steps=%zu\n",
                  path.tryOrdinal,
                  path.catchOrdinal,
                  path.handlerRva,
                  path.steps.size());
    }
    for (const peparse::CxxEh3UnwindStep &step : path.steps) {
      std::printf("  %d -> %d action=0x%x entry=0x%x\n",
                  step.fromState,
                  step.toState,
                  step.actionRva,
                  step.unwindEntryRva);
    }
  }
  return 0;
}
