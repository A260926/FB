/////////////////////////////////////////////////////////////////////////
//
// Author: Mateusz Jurczyk (mjurczyk@google.com)
//
// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Description
// ===========
//
// Content tests for the explicit round-mode receipt journal, registered with
// task_support/check.h in three groups:
//
//   identity    - identifier validation and module/build identity rules,
//                 including same-name modules and reverse-order loading,
//   rounds      - round lifecycle, cross-round repetition, binding conflicts
//                 and commit-failure retries,
//   publication - receipt formatting, atomic publication and real-file
//                 merging through the standalone receipt_merge tool.
//
// The fixtures only perform ordinary integer comparisons; no mutated or
// crashing inputs are constructed.

#include "task_support/check.h"

#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "source/common.h"
#include "source/modules.h"
#include "source/receipt.h"
#include "source/traces.h"

namespace {

constexpr const char* kSource =
    "0123456789abcdef0123456789abcdef01234567";
constexpr const char* kSourceOther =
    "fedcba9876543210fedcba9876543210fedcba98";
constexpr const char* kBuildA =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kBuildB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr const char* kInput1 =
    "1111111111111111111111111111111111111111111111111111111111111111";
constexpr const char* kInput2 =
    "2222222222222222222222222222222222222222222222222222222222222222";

// Real SanitizerCoverage callbacks provided by cmpcov.cc. No ASAN_OPTIONS is
// set in the test process, so collection while a round is active proves the
// round-mode gate independently of the legacy configuration.
extern "C" {
void __sanitizer_cov_trace_const_cmp8(uint64_t Arg1, uint64_t Arg2);
void __sanitizer_cov_trace_cmp8(uint64_t Arg1, uint64_t Arg2);
}

__attribute__((noinline)) static void GateOne(uint64_t value) {
  __sanitizer_cov_trace_const_cmp8(0x1122334455667788ULL, value);
}

__attribute__((noinline)) static void GateTwo(uint64_t a, uint64_t b) {
  __sanitizer_cov_trace_cmp8(a, b);
}

std::string TestDir() {
  const char* tmp = getenv("TMPDIR");
  std::string base = tmp != nullptr && tmp[0] != '\0' ? tmp : "/tmp";
  std::ostringstream path;
  path << base << "/receipt-tests-" << GetPid();
  return path.str();
}

bool EnsureDir(const std::string& path) {
  return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool ReadFile(const std::string& path, std::string* contents) {
  std::ifstream file(path.c_str(), std::ios::binary);
  if (!file.good()) {
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) {
    return false;
  }
  *contents = buffer.str();
  return true;
}

bool WriteFile(const std::string& path, const std::string& contents) {
  std::ofstream file(path.c_str(),
                     std::ios::binary | std::ios::out | std::ios::trunc);
  if (!file.good()) {
    return false;
  }
  file.write(contents.data(),
             static_cast<std::streamsize>(contents.size()));
  file.close();
  return static_cast<bool>(file);
}

// Builds two shared libraries sharing the same base name but living in
// different directories, so two loaded modules have identical names yet
// distinct identities and load addresses.
struct FixtureLibs {
  std::string dir;
  void* handle_a = nullptr;
  void* handle_b = nullptr;
  size_t pc_a = 0;
  size_t pc_b = 0;
};

bool BuildFixtureLibs(FixtureLibs* libs) {
  if (!EnsureDir(TestDir())) {
    return false;
  }
  libs->dir = TestDir() + "/libs";
  const std::string dir_a = libs->dir + "/a";
  const std::string dir_b = libs->dir + "/b";
  if (!EnsureDir(libs->dir) || !EnsureDir(dir_a) || !EnsureDir(dir_b)) {
    return false;
  }

  const std::string source_path = libs->dir + "/fixture.cc";
  if (!WriteFile(source_path,
                 "extern \"C\" int fixture_fn(int x) {\n"
                 "  if (x == 0x41424344) return 1;\n"
                 "  return x + 1;\n"
                 "}\n")) {
    return false;
  }

#if defined(__APPLE__)
  const char* kFormat =
      "c++ -O0 -dynamiclib -install_name @rpath/libfixture.dylib '%s' -o '%s'";
#else
  const char* kFormat =
      "c++ -O0 -shared -fPIC '%s' -o '%s'";
#endif

  const std::string lib_a = dir_a + "/libfixture"
#if defined(__APPLE__)
      ".dylib";
#else
      ".so";
#endif
  const std::string lib_b = dir_b + "/libfixture"
#if defined(__APPLE__)
      ".dylib";
#else
      ".so";
#endif

  char command[1024];
  snprintf(command, sizeof(command), kFormat, source_path.c_str(),
           lib_a.c_str());
  if (system(command) != 0) {
    return false;
  }
  snprintf(command, sizeof(command), kFormat, source_path.c_str(),
           lib_b.c_str());
  if (system(command) != 0) {
    return false;
  }

  libs->handle_a = dlopen(lib_a.c_str(), RTLD_NOW);
  libs->handle_b = dlopen(lib_b.c_str(), RTLD_NOW);
  if (libs->handle_a == nullptr || libs->handle_b == nullptr) {
    return false;
  }

  libs->pc_a = reinterpret_cast<size_t>(dlsym(libs->handle_a, "fixture_fn"));
  libs->pc_b = reinterpret_cast<size_t>(dlsym(libs->handle_b, "fixture_fn"));
  return libs->pc_a != 0 && libs->pc_b != 0 &&
         libs->pc_a != libs->pc_b;
}

std::string SelfDirectory() {
  Dl_info info;
  if (!dladdr(reinterpret_cast<void*>(&SelfDirectory), &info) ||
      info.dli_fname == nullptr) {
    return ".";
  }
  std::string path(info.dli_fname);
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

int RunTool(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  const pid_t child = fork();
  if (child < 0) {
    return -1;
  }
  if (child == 0) {
    execv(argv[0], argv.data());
    _exit(127);
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child) {
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Every line must end with '\n', contain no trailing whitespace and use only
// single spaces between fields.
bool CheckLineShape(const std::string& contents) {
  size_t pos = 0;
  while (pos < contents.size()) {
    size_t end = contents.find('\n', pos);
    if (end == std::string::npos) {
      return false;
    }
    const std::string line = contents.substr(pos, end - pos);
    if (line.empty() || line.back() == ' ' ||
        line.find("  ") != std::string::npos) {
      return false;
    }
    pos = end + 1;
  }
  return true;
}

// Creates (or reuses) a scratch subdirectory under the per-process root.
std::string ScratchDir(const std::string& name) {
  EnsureDir(TestDir());
  const std::string path = TestDir() + "/" + name;
  EnsureDir(path);
  return path;
}

////////////////////////////////////////////////////////////////////////////////
//
// identity group.
//
////////////////////////////////////////////////////////////////////////////////

static int identity_validation() {
  TASK_CHECK(IsValidSourceId(std::string(40, 'a')));
  TASK_CHECK(IsValidBuildId(std::string(64, 'f')));
  TASK_CHECK(!IsValidSourceId(std::string(39, 'a')));
  TASK_CHECK(!IsValidSourceId(std::string(40, 'A')));
  TASK_CHECK(!IsValidBuildId(std::string(63, '0')));
  TASK_CHECK(!IsValidBuildId(std::string(64, 'g')));

  uint64_t value = 0;
  TASK_CHECK(ParseHexUint("0", &value) && value == 0);
  TASK_CHECK(ParseHexUint("deadbeef", &value) && value == 0xdeadbeefULL);
  TASK_CHECK(!ParseHexUint("", &value));
  TASK_CHECK(!ParseHexUint("0x10", &value));
  TASK_CHECK(!ParseHexUint("ABC", &value));
  TASK_CHECK(ParseDecUint("0", &value) && value == 0);
  TASK_CHECK(ParseDecUint("12345", &value) && value == 12345);
  TASK_CHECK(!ParseDecUint("00", &value));
  TASK_CHECK(!ParseDecUint("0123", &value));
  TASK_CHECK(!ParseDecUint("12a", &value));
  TASK_CHECK(HexUint(0xabc) == "abc");
  TASK_CHECK(DecUint(7) == "7");

  // Null pointers and bad identifiers fail without touching state.
  TASK_CHECK(!CmpCovBeginRound(nullptr, 1, kInput1));
  TASK_CHECK(!CmpCovBeginRound(kSource, 1, nullptr));
  TASK_CHECK(!CmpCovBeginRound(std::string(40, 'z').c_str(), 1, kInput1));
  TASK_CHECK(!CmpCovBeginRound(kSource, 0, kInput1));
  TASK_CHECK(!CmpCovCommitRound(nullptr));
  TASK_CHECK(!CmpCovAbortRound());

  // A successful round can then be aborted, leaving the collector idle again.
  TASK_CHECK(CmpCovBeginRound(kSource, 99, kInput1));
  TASK_CHECK(!CmpCovBeginRound(kSource, 100, kInput2));
  TASK_CHECK(CmpCovAbortRound());
  TASK_CHECK(!CmpCovAbortRound());
  return 0;
}

static int identity_same_name_modules() {
  FixtureLibs libs;
  TASK_CHECK(BuildFixtureLibs(&libs));

  Traces traces;

  // Two distinct modules share a base name; neither basename nor load address
  // may merge their identities.
  Modules modules;
  const int idx_a = modules.GetModuleIndex(libs.pc_a);
  const int idx_b = modules.GetModuleIndex(libs.pc_b);
  TASK_CHECK(idx_a != -1 && idx_b != -1 && idx_a != idx_b);
  TASK_CHECK(modules.GetModuleName(idx_a) == modules.GetModuleName(idx_b));
  TASK_CHECK(modules.GetModuleBaseAddress(idx_a) !=
             modules.GetModuleBaseAddress(idx_b));

  TASK_CHECK(traces.BeginRound(kSource, 1, kInput1));

  // Same-named modules may carry different build identifiers.
  TASK_CHECK(traces.BindModule(libs.pc_a, kBuildA));
  TASK_CHECK(traces.BindModule(libs.pc_b, kBuildB));

  // The same build may also be loaded at several addresses: binding both
  // images to one identifier is legal and deduplicates on commit.
  Traces mirrored;
  TASK_CHECK(mirrored.BeginRound(kSource, 1, kInput1));
  TASK_CHECK(mirrored.BindModule(libs.pc_a, kBuildA));
  TASK_CHECK(mirrored.BindModule(libs.pc_b, kBuildA));

  traces.TrySaveTrace(libs.pc_a, 1, 0);
  traces.TrySaveTrace(libs.pc_b, 1, 0);
  // The mirrored images are one build at two addresses; give the comparisons
  // distinct labels so both survive (build, offset, arg1, arg2) deduplication.
  mirrored.TrySaveTrace(libs.pc_a, 2, 0);
  mirrored.TrySaveTrace(libs.pc_b, 3, 0);

  const std::string dir = ScratchDir("identity-same-name");
  TASK_CHECK(EnsureDir(dir));
  const std::string path = dir + "/receipt";
  const std::string mirror_path = dir + "/mirror";
  TASK_CHECK(traces.CommitRound(path));
  TASK_CHECK(mirrored.CommitRound(mirror_path));

  Receipt receipt, mirror;
  std::string contents, mirror_contents;
  TASK_CHECK(ReadFile(path, &contents) && ParseReceipt(contents, &receipt));
  TASK_CHECK(ReadFile(mirror_path, &mirror_contents) &&
             ParseReceipt(mirror_contents, &mirror));
  TASK_CHECK(receipt.modules.size() == 2);
  TASK_CHECK(receipt.modules.count(kBuildA) && receipt.modules.count(kBuildB));
  TASK_CHECK(mirror.modules.size() == 1);
  TASK_CHECK(*mirror.modules.begin() == kBuildA);
  TASK_CHECK(mirror.traces.size() == 2);
  for (const ReceiptTrace& trace : mirror.traces) {
    TASK_CHECK(trace.build == kBuildA);
  }
  return 0;
}

static int identity_reverse_loading() {
  FixtureLibs libs;
  TASK_CHECK(BuildFixtureLibs(&libs));

  Traces traces;
  TASK_CHECK(traces.BeginRound(kSource, 1, kInput1));

  // Coverage arrives in reverse order relative to the bindings below.
  traces.TrySaveTrace(libs.pc_b, 3, 4);
  traces.TrySaveTrace(libs.pc_a, 1, 2);

  // Bindings are then made in the opposite order; attribution must follow the
  // loaded image, not the order in which things were first seen.
  TASK_CHECK(traces.BindModule(libs.pc_b, kBuildB));
  TASK_CHECK(traces.BindModule(libs.pc_a, kBuildA));

  EnsureDir(TestDir());
  const std::string path = TestDir() + "/identity-reverse.receipt";
  TASK_CHECK(traces.CommitRound(path));

  Receipt receipt;
  std::string contents;
  TASK_CHECK(ReadFile(path, &contents) && ParseReceipt(contents, &receipt));
  TASK_CHECK(receipt.traces.size() == 2);

  Modules modules;
  const int idx_a = modules.GetModuleIndex(libs.pc_a);
  const int idx_b = modules.GetModuleIndex(libs.pc_b);
  const uint64_t off_a = libs.pc_a - modules.GetModuleBaseAddress(idx_a);
  const uint64_t off_b = libs.pc_b - modules.GetModuleBaseAddress(idx_b);

  bool found_a = false, found_b = false;
  for (const ReceiptTrace& trace : receipt.traces) {
    if (trace.build == kBuildA) {
      TASK_CHECK(trace.offset == off_a && trace.arg1 == 1 &&
                 trace.arg2 == 2);
      found_a = true;
    } else if (trace.build == kBuildB) {
      TASK_CHECK(trace.offset == off_b && trace.arg1 == 3 &&
                 trace.arg2 == 4);
      found_b = true;
    }
  }
  TASK_CHECK(found_a && found_b);
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
//
// rounds group.
//
////////////////////////////////////////////////////////////////////////////////

static int rounds_lifecycle_and_tags() {
  const std::string dir = ScratchDir("rounds-lifecycle");
  TASK_CHECK(EnsureDir(dir));

  // Drives the actual comparison callbacks with no ASAN_OPTIONS set; the
  // round must still collect.
  TASK_CHECK(CmpCovBeginRound(kSource, 1, kInput1));
  const size_t pc = reinterpret_cast<size_t>(&GateOne);
  TASK_CHECK(CmpCovBindModule(reinterpret_cast<const void*>(pc), kBuildA));
  GateOne(0x3322334455667788ULL);  // One low byte matches the constant.
  GateTwo(0x0102030405060708ULL, 0x0102030405060700ULL);
  const std::string path = dir + "/round1";
  TASK_CHECK(CmpCovCommitRound(path.c_str()));

  std::string contents;
  TASK_CHECK(ReadFile(path, &contents));
  TASK_CHECK(CheckLineShape(contents));

  Receipt receipt;
  TASK_CHECK(ParseReceipt(contents, &receipt));
  TASK_CHECK(receipt.magic == "CMPCOV-ROUND");
  TASK_CHECK(receipt.source == kSource && receipt.round == 1);
  TASK_CHECK(receipt.input == kInput1);
  TASK_CHECK(receipt.modules.size() == 1);
  TASK_CHECK(*receipt.modules.begin() == kBuildA);
  TASK_CHECK(!receipt.traces.empty());

  // Offset and the two labels stay separate: reconstruct the tagged wide
  // trace and confirm the fields round-trip without aliasing an address.
  Modules modules;
  const int idx = modules.GetModuleIndex(pc);
  const uint64_t base = modules.GetModuleBaseAddress(idx);
  for (const ReceiptTrace& trace : receipt.traces) {
    TASK_CHECK(trace.build == kBuildA);
    TASK_CHECK(trace.offset < (1ULL << 48));
    TASK_CHECK(trace.offset == (base + trace.offset) - base);
    const uint64_t wide = trace.offset | (trace.arg2 << 48) |
                          (trace.arg1 << 60);
    TASK_CHECK((wide >> 60) == trace.arg1);
    TASK_CHECK(((wide >> 48) & 0xfff) == trace.arg2);
    TASK_CHECK((wide & ((1ULL << 48) - 1)) == trace.offset);
  }

  // Idle once more: commit and abort both fail outside a round.
  TASK_CHECK(!CmpCovCommitRound(path.c_str()));
  TASK_CHECK(!CmpCovAbortRound());
  return 0;
}

static int rounds_cross_round_repeat() {
  const std::string dir = ScratchDir("rounds-repeat");
  TASK_CHECK(EnsureDir(dir));
  const size_t pc = reinterpret_cast<size_t>(&GateOne);

  TASK_CHECK(CmpCovBeginRound(kSource, 1, kInput1));
  TASK_CHECK(CmpCovBindModule(reinterpret_cast<const void*>(pc), kBuildA));
  GateOne(0x3322334455667788ULL);
  const std::string path1 = dir + "/r1";
  TASK_CHECK(CmpCovCommitRound(path1.c_str()));

  // The same comparison must be recorded afresh in the next round even though
  // the cumulative log already contains it.
  TASK_CHECK(CmpCovBeginRound(kSource, 2, kInput2));
  TASK_CHECK(CmpCovBindModule(reinterpret_cast<const void*>(pc), kBuildA));
  GateOne(0x3322334455667788ULL);
  const std::string path2 = dir + "/r2";
  TASK_CHECK(CmpCovCommitRound(path2.c_str()));

  Receipt r1, r2;
  std::string c1, c2;
  TASK_CHECK(ReadFile(path1, &c1) && ParseReceipt(c1, &r1));
  TASK_CHECK(ReadFile(path2, &c2) && ParseReceipt(c2, &r2));
  TASK_CHECK(r1.round == 1 && r2.round == 2);
  TASK_CHECK(r1.traces == r2.traces);
  TASK_CHECK(!r1.traces.empty());

  // A round with no comparisons at all still commits, with an empty trace set.
  TASK_CHECK(CmpCovBeginRound(kSource, 3, kInput1));
  TASK_CHECK(CmpCovBindModule(reinterpret_cast<const void*>(pc), kBuildA));
  const std::string path3 = dir + "/r3";
  TASK_CHECK(CmpCovCommitRound(path3.c_str()));
  Receipt r3;
  std::string c3;
  TASK_CHECK(ReadFile(path3, &c3) && ParseReceipt(c3, &r3));
  TASK_CHECK(r3.traces.empty());
  TASK_CHECK(r3.modules.size() == 1);

  // A round with neither bindings nor coverage commits an empty receipt, which
  // must still round-trip through the strict parser.
  TASK_CHECK(CmpCovBeginRound(kSource, 4, kInput2));
  const std::string path4 = dir + "/r4";
  TASK_CHECK(CmpCovCommitRound(path4.c_str()));
  Receipt r4;
  std::string c4;
  TASK_CHECK(ReadFile(path4, &c4) && ParseReceipt(c4, &r4));
  TASK_CHECK(r4.modules.empty() && r4.traces.empty());
  TASK_CHECK(!CmpCovCommitRound(path4.c_str()));  // Idle again.
  return 0;
}

static int rounds_binding_conflicts() {
  FixtureLibs libs;
  TASK_CHECK(BuildFixtureLibs(&libs));

  Traces traces;
  TASK_CHECK(!traces.BindModule(libs.pc_a, kBuildA));  // No active round.
  TASK_CHECK(traces.BeginRound(kSource, 1, kInput1));

  // Re-binding the same module to the same build is idempotent.
  TASK_CHECK(traces.BindModule(libs.pc_a, kBuildA));
  TASK_CHECK(traces.BindModule(libs.pc_a, kBuildA));

  // Switching a module to a different build is rejected and leaves the
  // original binding in place.
  TASK_CHECK(!traces.BindModule(libs.pc_a, kBuildB));
  TASK_CHECK(traces.BindModule(libs.pc_a, kBuildA));

  // Malformed identifiers fail without creating a binding.
  TASK_CHECK(!traces.BindModule(libs.pc_b, std::string(64, 'Z')));
  TASK_CHECK(traces.BindModule(libs.pc_b, kBuildB));

  traces.TrySaveTrace(libs.pc_a, 1, 0);
  traces.TrySaveTrace(libs.pc_b, 1, 0);

  EnsureDir(TestDir());
  const std::string path = TestDir() + "/binding-conflict.receipt";
  TASK_CHECK(traces.CommitRound(path));

  Receipt receipt;
  std::string contents;
  TASK_CHECK(ReadFile(path, &contents) && ParseReceipt(contents, &receipt));
  TASK_CHECK(receipt.modules.size() == 2);
  bool a_ok = false, b_ok = false;
  for (const ReceiptTrace& trace : receipt.traces) {
    if (trace.build == kBuildA) a_ok = true;
    if (trace.build == kBuildB) b_ok = true;
  }
  TASK_CHECK(a_ok && b_ok);

  // Bindings only last for the round: a fresh round starts with none, so the
  // same module may take another identifier.
  TASK_CHECK(traces.BeginRound(kSource, 2, kInput2));
  TASK_CHECK(traces.BindModule(libs.pc_a, kBuildB));
  TASK_CHECK(traces.AbortRound());
  TASK_CHECK(!traces.AbortRound());
  return 0;
}

static int rounds_commit_failure_retry() {
  FixtureLibs libs;
  TASK_CHECK(BuildFixtureLibs(&libs));

  Traces traces;
  const std::string dir = ScratchDir("commit-retry");
  TASK_CHECK(EnsureDir(dir));
  const std::string path = dir + "/receipt";

  TASK_CHECK(traces.BeginRound(kSource, 1, kInput1));
  traces.TrySaveTrace(libs.pc_a, 5, 6);

  // Coverage in an unbound module rejects the commit without clearing state.
  TASK_CHECK(!traces.CommitRound(path));

  // The round is still active: new coverage and the missing binding are
  // accepted, and the retried commit now succeeds with both comparisons.
  traces.TrySaveTrace(libs.pc_a, 7, 8);
  TASK_CHECK(traces.BindModule(libs.pc_a, kBuildA));
  TASK_CHECK(traces.CommitRound(path));

  Receipt receipt;
  std::string contents;
  TASK_CHECK(ReadFile(path, &contents) && ParseReceipt(contents, &receipt));
  TASK_CHECK(receipt.round == 1);
  TASK_CHECK(receipt.traces.size() == 2);
  TASK_CHECK(!traces.IsRoundActive());

  // Publication failure (different existing contents) likewise preserves the
  // active round for a retry.
  TASK_CHECK(traces.BeginRound(kSource, 2, kInput2));
  TASK_CHECK(traces.BindModule(libs.pc_a, kBuildA));
  traces.TrySaveTrace(libs.pc_a, 9, 10);
  TASK_CHECK(!traces.CommitRound(path));
  TASK_CHECK(traces.IsRoundActive());
  TASK_CHECK(traces.AbortRound());

  std::string untouched;
  TASK_CHECK(ReadFile(path, &untouched));
  TASK_CHECK(untouched == contents);
  return 0;
}

static int rounds_abort_discards() {
  Traces traces;
  FixtureLibs libs;
  TASK_CHECK(BuildFixtureLibs(&libs));

  TASK_CHECK(traces.BeginRound(kSource, 1, kInput1));
  TASK_CHECK(traces.BindModule(libs.pc_a, kBuildA));
  traces.TrySaveTrace(libs.pc_a, 1, 1);
  TASK_CHECK(traces.AbortRound());

  // Everything round-local is gone: the next round starts empty and the old
  // binding no longer applies.
  TASK_CHECK(traces.BeginRound(kSource, 2, kInput2));
  traces.TrySaveTrace(libs.pc_a, 2, 2);
  EnsureDir(TestDir());
  TASK_CHECK(!traces.CommitRound(TestDir() + "/abort-should-fail"));
  TASK_CHECK(traces.IsRoundActive());
  TASK_CHECK(traces.AbortRound());
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
//
// publication group.
//
////////////////////////////////////////////////////////////////////////////////

static int publication_format_and_idempotence() {
  const std::string dir = ScratchDir("publication");
  TASK_CHECK(EnsureDir(dir));
  const std::string path = dir + "/receipt";

  Traces traces;
  TASK_CHECK(traces.BeginRound(kSource, 7, kInput1));
  const size_t pc = reinterpret_cast<size_t>(&GateOne);
  TASK_CHECK(traces.BindModule(pc, kBuildA));
  // Local collector: feed it directly. The global-callback path is exercised
  // by the rounds group and by the replay example.
  traces.TrySaveTrace(pc, 1, 0);
  TASK_CHECK(traces.CommitRound(path));

  std::string first;
  TASK_CHECK(ReadFile(path, &first));
  TASK_CHECK(CheckLineShape(first));
  // Absolute paths, base addresses and PIDs are never written.
  TASK_CHECK(first.find('/') == std::string::npos);
  TASK_CHECK(first.find("0x") == std::string::npos);

  // Exact header order and spelling.
  std::istringstream stream(first);
  std::string line;
  TASK_CHECK(std::getline(stream, line) && line == "CMPCOV-ROUND 1");
  TASK_CHECK(std::getline(stream, line) &&
             line == std::string("source ") + kSource);
  TASK_CHECK(std::getline(stream, line) && line == "round 7");
  TASK_CHECK(std::getline(stream, line) &&
             line == std::string("input ") + kInput1);
  TASK_CHECK(std::getline(stream, line) &&
             line == std::string("module ") + kBuildA);

  // Trace fields: build, lowercase unprefixed hex offset, decimal labels.
  bool saw_trace = false;
  std::string previous;
  while (std::getline(stream, line)) {
    TASK_CHECK(line.rfind("trace ", 0) == 0);
    std::istringstream trace_stream(line);
    std::string tag, build, offset, arg1, arg2;
    TASK_CHECK(trace_stream >> tag >> build >> offset >> arg1 >> arg2);
    TASK_CHECK(build == kBuildA);
    TASK_CHECK(offset.find_first_not_of("0123456789abcdef") ==
               std::string::npos);
    TASK_CHECK(offset[0] != '0' || offset == "0");
    TASK_CHECK((arg1[0] != '0' || arg1 == "0") &&
               (arg2[0] != '0' || arg2 == "0"));
    if (saw_trace) {
      TASK_CHECK(previous <= line);  // Records are sorted.
    }
    previous = line;
    saw_trace = true;
  }
  TASK_CHECK(saw_trace);

  // Re-publishing identical contents is idempotent; different contents fail.
  Traces again;
  TASK_CHECK(again.BeginRound(kSource, 7, kInput1));
  TASK_CHECK(again.BindModule(pc, kBuildA));
  again.TrySaveTrace(pc, 1, 0);
  TASK_CHECK(again.CommitRound(path));

  Traces different;
  TASK_CHECK(different.BeginRound(kSource, 8, kInput2));
  TASK_CHECK(different.BindModule(pc, kBuildA));
  different.TrySaveTrace(pc, 2, 0);
  TASK_CHECK(!different.CommitRound(path));
  TASK_CHECK(different.IsRoundActive());
  TASK_CHECK(different.AbortRound());

  std::string still;
  TASK_CHECK(ReadFile(path, &still));
  TASK_CHECK(still == first);
  return 0;
}

static int publication_real_file_merge() {
  const std::string dir = ScratchDir("merge");
  TASK_CHECK(EnsureDir(dir));

  // Two independent collectors, two rounds each, committed to real files.
  struct RoundFile {
    std::string path;
    uint64_t round;
  };
  std::vector<RoundFile> round_files;
  std::set<ReceiptTrace> expected_union;
  for (int collector = 0; collector < 2; ++collector) {
    Traces traces;
    size_t pc;
    if (collector == 0) {
      pc = reinterpret_cast<size_t>(&GateOne);
    } else {
      pc = reinterpret_cast<size_t>(&GateTwo);
    }
    for (int slot = 1; slot <= 2; ++slot) {
      // Distinct collectors must not reuse a (source, round, input) triple:
      // collector 0 owns rounds 1-2, collector 1 owns rounds 3-4.
      const uint64_t round = collector * 2 + slot;
      const char* input = slot == 1 ? kInput1 : kInput2;
      TASK_CHECK(traces.BeginRound(kSource, round, input));
      TASK_CHECK(traces.BindModule(pc, kBuildA));
      // Distinct callsites and labels make the union non-trivial.
      traces.TrySaveTrace(pc, slot, collector);
      const std::string path =
          dir + "/c" + std::to_string(collector) +
          "r" + std::to_string(round);
      TASK_CHECK(traces.CommitRound(path));
      round_files.push_back(RoundFile{path, round});

      Receipt parsed;
      std::string contents;
      TASK_CHECK(ReadFile(path, &contents) &&
                 ParseReceipt(contents, &parsed));
      TASK_CHECK(!parsed.traces.empty());
      expected_union.insert(parsed.traces.begin(), parsed.traces.end());
    }
  }

  const std::string merge_tool = SelfDirectory() + "/receipt_merge";
  const std::string output = dir + "/merged";
  std::vector<std::string> argv = {merge_tool, output};
  for (const RoundFile& rf : round_files) {
    argv.push_back(rf.path);
  }
  TASK_CHECK(RunTool(argv) == 0);

  std::string merged_contents;
  TASK_CHECK(ReadFile(output, &merged_contents));
  TASK_CHECK(CheckLineShape(merged_contents));
  Receipt merged;
  TASK_CHECK(ParseReceipt(merged_contents, &merged));
  TASK_CHECK(merged.magic == "CMPCOV-MERGED");
  TASK_CHECK(!merged.has_round && !merged.has_input);
  TASK_CHECK(merged.source == kSource);
  TASK_CHECK(merged.modules == std::set<std::string>{kBuildA});
  TASK_CHECK(merged.traces == expected_union);

  // Input order and duplicate files must not change the result.
  const std::string output2 = dir + "/merged2";
  std::vector<std::string> reversed = {merge_tool, output2};
  for (auto it = round_files.rbegin(); it != round_files.rend(); ++it) {
    reversed.push_back(it->path);
  }
  reversed.push_back(round_files.front().path);  // Duplicate input.
  TASK_CHECK(RunTool(reversed) == 0);
  std::string merged2_contents;
  TASK_CHECK(ReadFile(output2, &merged2_contents));
  TASK_CHECK(merged2_contents == merged_contents);

  // Re-running into the same output with identical contents is idempotent.
  TASK_CHECK(RunTool(argv) == 0);

  // Duplicate round key with conflicting contents must be rejected and leave
  // any pre-existing output byte-identical.
  Receipt tampered;
  std::string base_contents;
  TASK_CHECK(ReadFile(round_files[0].path, &base_contents));
  TASK_CHECK(ParseReceipt(base_contents, &tampered));
  const std::string conflict_path = dir + "/conflict";
  std::string conflict = base_contents;
  conflict += std::string("trace ") + kBuildA + " ffffffff 1 0\n";
  TASK_CHECK(WriteFile(conflict_path, conflict));
  const std::string guard = dir + "/guard";
  TASK_CHECK(WriteFile(guard, merged_contents));
  std::vector<std::string> bad_argv = {merge_tool, guard,
                                       round_files[0].path, conflict_path};
  TASK_CHECK(RunTool(bad_argv) != 0);
  std::string guard_contents;
  TASK_CHECK(ReadFile(guard, &guard_contents));
  TASK_CHECK(guard_contents == merged_contents);
  return 0;
}

static int publication_malformed_inputs() {
  const std::string dir = ScratchDir("malformed");
  TASK_CHECK(EnsureDir(dir));

  // One well-formed receipt acts as the reference identity.
  Traces traces;
  const size_t pc = reinterpret_cast<size_t>(&GateOne);
  TASK_CHECK(traces.BeginRound(kSource, 1, kInput1));
  TASK_CHECK(traces.BindModule(pc, kBuildA));
  traces.TrySaveTrace(pc, 1, 0);
  const std::string good = dir + "/good";
  TASK_CHECK(traces.CommitRound(good));
  std::string good_contents;
  TASK_CHECK(ReadFile(good, &good_contents));

  Receipt parsed;
  TASK_CHECK(ParseReceipt(good_contents, &parsed));
  const std::string other_source =
      std::string("source ") + kSourceOther + "\n";
  const std::string other_module =
      std::string("module ") + kBuildB + "\n";

  struct BadCase {
    const char* name;
    std::string contents;
    bool parser_rejects;
  };
  std::vector<BadCase> cases;

  // Truncated: drop the final newline and last byte.
  cases.push_back({"truncated",
                   good_contents.substr(0, good_contents.size() - 1), true});
  cases.push_back({"truncated-byte",
                   good_contents.substr(0, good_contents.size() - 2), true});
  // Unknown version and bad magic.
  {
    std::string text = good_contents;
    text.replace(0, 13, "CMPCOV-ROUND 2");
    cases.push_back({"unknown-version", text, true});
  }
  {
    std::string text = good_contents;
    text.replace(0, 12, "CMPCOV-OTHER");
    cases.push_back({"bad-magic", text, true});
  }
  // Bad fields.
  cases.push_back({"bad-source",
                   std::string("CMPCOV-ROUND 1\nsource ") +
                       std::string(40, 'z') +
                       "\nround 1\ninput " + kInput1 + "\nmodule " +
                       kBuildA + "\n",
                   true});
  cases.push_back({"round-zero",
                   std::string("CMPCOV-ROUND 1\nsource ") + kSource +
                       "\nround 0\ninput " + kInput1 + "\nmodule " +
                       kBuildA + "\n",
                   true});
  cases.push_back({"bad-decimal",
                   std::string("CMPCOV-ROUND 1\nsource ") + kSource +
                       "\nround 01\ninput " + kInput1 + "\nmodule " +
                       kBuildA + "\n",
                   true});
  // Duplicate and out-of-order module lines.
  {
    std::string text = good_contents;
    const std::string mod_line = std::string("module ") + kBuildA + "\n";
    const size_t at = text.find(mod_line);
    text.insert(at + mod_line.size(), mod_line);
    cases.push_back({"duplicate-module", text, true});
  }
  {
    std::string text = std::string("CMPCOV-ROUND 1\nsource ") + kSource +
                       "\nround 1\ninput " + kInput1 + "\n";
    for (const ReceiptTrace& trace : parsed.traces) {
      text += std::string("trace ") + trace.build + " " +
              HexUint(trace.offset) + " " +
              DecUint(trace.arg1) + " " + DecUint(trace.arg2) + "\n";
    }
    text += std::string("module ") + kBuildA + "\n";  // Modules after traces.
    cases.push_back({"out-of-order", text, true});
  }
  // Trace referencing an undeclared module.
  cases.push_back({"undeclared-module",
                   std::string("CMPCOV-ROUND 1\nsource ") + kSource +
                       "\nround 1\ninput " + kInput1 + "\nmodule " +
                       kBuildA + "\ntrace " + kBuildB +
                       " 10 1 0\n",
                   true});
  // Duplicate trace and out-of-order trace lines.
  cases.push_back({"duplicate-trace",
                   std::string("CMPCOV-ROUND 1\nsource ") + kSource +
                       "\nround 1\ninput " + kInput1 + "\nmodule " +
                       kBuildA + "\ntrace " + kBuildA +
                       " 10 1 0\ntrace " + kBuildA + " 10 1 0\n",
                   true});
  cases.push_back({"out-of-order-trace",
                   std::string("CMPCOV-ROUND 1\nsource ") + kSource +
                       "\nround 1\ninput " + kInput1 + "\nmodule " +
                       kBuildA + "\ntrace " + kBuildA +
                       " 20 1 0\ntrace " + kBuildA + " 10 1 0\n",
                   true});
  // Two modules listed in reverse identifier order.
  cases.push_back({"out-of-order-module",
                   std::string("CMPCOV-ROUND 1\nsource ") + kSource +
                       "\nround 1\ninput " + kInput1 + "\nmodule " +
                       kBuildB + "\nmodule " + kBuildA + "\n",
                   true});
  // Trailing garbage line and a stray input line in a merged file.
  cases.push_back({"trailing-garbage", good_contents + "garbage\n", true});
  {
    std::string merged = std::string("CMPCOV-MERGED 1\nsource ") + kSource +
                         "\ninput " + kInput1 + "\nmodule " + kBuildA +
                         "\n";
    cases.push_back({"merged-with-input", merged, true});
  }
  // Identity mismatches relative to the good receipt: syntactically valid,
  // but rejected by the merge tool against the reference receipt.
  {
    std::string text = good_contents;
    text.replace(text.find("source "), other_source.size(), other_source);
    cases.push_back({"source-mismatch", text, false});
  }
  {
    std::string text = good_contents;
    const std::string mod_line = std::string("module ") + kBuildA + "\n";
    text.replace(text.find(mod_line), mod_line.size(), other_module);
    for (const ReceiptTrace& trace : parsed.traces) {
      const std::string old_trace =
          std::string("trace ") + trace.build + " " + HexUint(trace.offset) +
          " " + DecUint(trace.arg1) + " " + DecUint(trace.arg2) + "\n";
      const std::string new_trace =
          std::string("trace ") + kBuildB + " " + HexUint(trace.offset) +
          " " + DecUint(trace.arg1) + " " + DecUint(trace.arg2) + "\n";
      text.replace(text.find(old_trace), old_trace.size(), new_trace);
    }
    cases.push_back({"module-mismatch", text, false});
  }

  const std::string merge_tool = SelfDirectory() + "/receipt_merge";
  for (const BadCase& bad : cases) {
    const std::string bad_path =
        dir + std::string("/bad-") + bad.name;
    TASK_CHECK(WriteFile(bad_path, bad.contents));

    // Direct parser rejection for syntactically broken receipts; the
    // identity variants must still parse on their own.
    Receipt rejected;
    if (bad.parser_rejects) {
      TASK_CHECK(!ParseReceipt(bad.contents, &rejected));
    } else {
      TASK_CHECK(ParseReceipt(bad.contents, &rejected));
    }

    // Every variant is rejected when merged with the reference receipt, and
    // the guarded output stays byte-identical.
    const std::string guard = dir + std::string("/guard-") + bad.name;
    TASK_CHECK(WriteFile(guard, good_contents));
    const std::vector<std::string> argv = {merge_tool, guard, good,
                                           bad_path};
    TASK_CHECK(RunTool(argv) != 0);
    std::string guard_contents;
    TASK_CHECK(ReadFile(guard, &guard_contents));
    TASK_CHECK(guard_contents == good_contents);
  }
  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"identity", "validation", identity_validation},
    {"identity", "same_name_modules", identity_same_name_modules},
    {"identity", "reverse_loading", identity_reverse_loading},
    {"rounds", "lifecycle_and_tags", rounds_lifecycle_and_tags},
    {"rounds", "cross_round_repeat", rounds_cross_round_repeat},
    {"rounds", "binding_conflicts", rounds_binding_conflicts},
    {"rounds", "commit_failure_retry", rounds_commit_failure_retry},
    {"rounds", "abort_discards", rounds_abort_discards},
    {"publication", "format_and_idempotence",
     publication_format_and_idempotence},
    {"publication", "real_file_merge", publication_real_file_merge},
    {"publication", "malformed_inputs", publication_malformed_inputs},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
