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
// End-to-end replay of the explicit round mode. Given an output directory:
//
//   receipt_replay OUTPUT_DIR
//
// the program forks two independent, normally-terminating processes. Each
// child opens two coverage rounds through the C ABI, exercises the real
// SanitizerCoverage comparison callbacks (no ASAN_OPTIONS is set), binds the
// loaded executable to a build identifier and commits one receipt per round.
// The parent then runs the standalone receipt_merge tool over every receipt,
// verifies the merged identity against the per-round receipts and records the
// whole audit trail in OUTPUT_DIR/transcript.txt. The fixtures only perform
// ordinary integer comparisons; no mutated or crashing inputs are used.

#if defined(_WIN32)
#error receipt_replay is only supported on local Linux/macOS builds.
#endif

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "source/common.h"
#include "source/receipt.h"

namespace {

constexpr const char* kSource =
    "0123456789abcdef0123456789abcdef01234567";
constexpr const char* kBuild =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kInputRound1 =
    "1111111111111111111111111111111111111111111111111111111111111111";
constexpr const char* kInputRound2 =
    "2222222222222222222222222222222222222222222222222222222222222222";

// Real SanitizerCoverage entry points implemented by cmpcov.cc. Declared here
// so the replay drives the same callbacks an instrumented build would emit.
extern "C" {
void __sanitizer_cov_trace_const_cmp8(uint64_t Arg1, uint64_t Arg2);
void __sanitizer_cov_trace_cmp8(uint64_t Arg1, uint64_t Arg2);
void __sanitizer_cov_trace_const_cmp4(uint32_t Arg1, uint32_t Arg2);
}

__attribute__((noinline)) static uint64_t GateConst64(uint64_t value) {
  __sanitizer_cov_trace_const_cmp8(0x1122334455667788ULL, value);
  return value ^ 0x1122334455667788ULL;
}

__attribute__((noinline)) static uint64_t GateNonConst64(uint64_t a,
                                                         uint64_t b) {
  __sanitizer_cov_trace_cmp8(a, b);
  return a + b;
}

__attribute__((noinline)) static uint32_t GateConst32(uint32_t value) {
  __sanitizer_cov_trace_const_cmp4(0xdeadbeefU, value);
  return value ^ 0xdeadbeefU;
}

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (dir.empty() || dir.back() == '/') {
    return dir + name;
  }
  return dir + "/" + name;
}

std::string SelfDirectory() {
  Dl_info info;
  if (!dladdr(reinterpret_cast<void*>(&SelfDirectory), &info) ||
      info.dli_fname == nullptr) {
    return std::string();
  }
  std::string path(info.dli_fname);
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".")
                                    : path.substr(0, slash);
}

int RunChild(int child_id, const std::string& output_dir,
             std::string* error) {
  const int pid = GetPid();
  const size_t anchor = reinterpret_cast<size_t>(&GateConst64);

  // Round 1: a constant 64-bit comparison with one matching low byte and a
  // 32-bit one.
  if (!CmpCovBeginRound(kSource, 1, kInputRound1)) {
    *error = "child: BeginRound(1) failed";
    return 1;
  }
  if (!CmpCovBindModule(reinterpret_cast<const void*>(anchor), kBuild)) {
    *error = "child: BindModule(1) failed";
    return 1;
  }
  GateConst64(0x3322334455667788ULL);
  GateConst32(0xdeadbeefU);
  const std::string receipt1 =
      JoinPath(output_dir, "receipt.p" + std::to_string(pid) + ".r1");
  if (!CmpCovCommitRound(receipt1.c_str())) {
    *error = "child: CommitRound(1) failed";
    return 1;
  }

  // Round 2: repeat the round-1 comparison (it must re-appear in the new
  // receipt even though it is cumulatively duplicate) and add a non-constant
  // comparison, which is collected without ASAN_OPTIONS while a round is open.
  if (!CmpCovBeginRound(kSource, 2, kInputRound2)) {
    *error = "child: BeginRound(2) failed";
    return 1;
  }
  if (!CmpCovBindModule(reinterpret_cast<const void*>(anchor), kBuild)) {
    *error = "child: BindModule(2) failed";
    return 1;
  }
  GateConst64(0x3322334455667788ULL);
  // Two low bytes match, so the non-constant comparison contributes traces
  // even though non-constant instrumentation is disabled in ASAN_OPTIONS.
  GateNonConst64(0x0102030405060708ULL, 0x1122334455660708ULL);
  const std::string receipt2 =
      JoinPath(output_dir, "receipt.p" + std::to_string(pid) + ".r2");
  if (!CmpCovCommitRound(receipt2.c_str())) {
    *error = "child: CommitRound(2) failed";
    return 1;
  }

  std::printf("child %d pid=%d committed rounds 1 and 2\n", child_id, pid);
  return 0;
}

int RunMergeTool(const std::vector<std::string>& receipts,
                 const std::string& merged_path, int* exit_status) {
  const std::string tool = JoinPath(SelfDirectory(), "receipt_merge");

  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(tool.c_str()));
  argv.push_back(const_cast<char*>(merged_path.c_str()));
  for (const std::string& receipt : receipts) {
    argv.push_back(const_cast<char*>(receipt.c_str()));
  }
  argv.push_back(nullptr);

  const pid_t child = fork();
  if (child < 0) {
    return -1;
  }
  if (child == 0) {
    execv(tool.c_str(), argv.data());
    _exit(127);
  }

  int status = 0;
  if (waitpid(child, &status, 0) != child) {
    return -1;
  }
  *exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return 0;
}

bool ReadTextFile(const std::string& path, std::string* contents) {
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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s OUTPUT_DIR\n",
                 argc > 0 ? argv[0] : "receipt_replay");
    return 2;
  }
  const std::string output_dir = argv[1];
  if (mkdir(output_dir.c_str(), 0755) != 0 && errno != EEXIST) {
    std::perror("mkdir");
    return 1;
  }

  // Remove receipts generated by earlier runs so the no-overwrite publication
  // rule cannot reject a fresh replay with changed contents.
  if (DIR* dir_handle = opendir(output_dir.c_str())) {
    while (dirent* entry = readdir(dir_handle)) {
      const std::string name(entry->d_name);
      if (name.rfind("receipt.", 0) == 0 || name == "merged.receipt") {
        unlink(JoinPath(output_dir, name).c_str());
      }
    }
    closedir(dir_handle);
  }

  const std::string transcript_path = JoinPath(output_dir, "transcript.txt");
  std::ofstream transcript(transcript_path.c_str(),
                           std::ios::out | std::ios::trunc);
  if (!transcript.good()) {
    std::fprintf(stderr, "unable to open %s\n", transcript_path.c_str());
    return 1;
  }

  transcript << "CMPCOV REPLAY TRANSCRIPT\n";
  transcript << "source " << kSource << "\n";
  transcript << "build " << kBuild << "\n";

  std::vector<pid_t> children;
  for (int child_id = 0; child_id < 2; ++child_id) {
    pid_t pid = fork();
    if (pid < 0) {
      transcript << "RESULT FAIL fork\n";
      return 1;
    }
    if (pid == 0) {
      std::string error;
      int status = RunChild(child_id, output_dir, &error);
      if (status != 0) {
        std::fprintf(stderr, "%s\n", error.c_str());
      }
      _exit(status);
    }
    children.push_back(pid);
    transcript << "spawn child " << child_id << " pid " << pid << "\n";
  }

  bool ok = true;
  for (size_t i = 0; i < children.size(); ++i) {
    int status = 0;
    if (waitpid(children[i], &status, 0) != children[i] ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      transcript << "child pid " << children[i] << " abnormal exit\n";
      ok = false;
    } else {
      transcript << "child pid " << children[i] << " exit 0\n";
    }
  }

  std::vector<std::string> receipts;
  for (pid_t pid : children) {
    for (int round = 1; round <= 2; ++round) {
      receipts.push_back(
          JoinPath(output_dir, "receipt.p" + std::to_string(pid) +
                                   ".r" + std::to_string(round)));
    }
  }

  // Identity check on every per-round receipt before merging.
  std::set<std::string> round_identities;
  for (const std::string& path : receipts) {
    std::string contents;
    if (!ReadTextFile(path, &contents)) {
      transcript << "missing receipt " << path << "\n";
      ok = false;
      continue;
    }
    Receipt receipt;
    if (!ParseReceipt(contents, &receipt)) {
      transcript << "malformed receipt " << path << "\n";
      ok = false;
      continue;
    }
    if (receipt.source != kSource ||
        receipt.modules != std::set<std::string>{kBuild}) {
      transcript << "identity mismatch in " << path << "\n";
      ok = false;
    }
    round_identities.insert(receipt.source);
    transcript << "receipt " << path << " round " << DecUint(receipt.round)
               << " modules " << receipt.modules.size()
               << " traces " << receipt.traces.size() << "\n";
  }

  // Run the standalone merge tool over real files.
  const std::string merged_path = JoinPath(output_dir, "merged.receipt");
  int merge_status = -1;
  if (RunMergeTool(receipts, merged_path, &merge_status) != 0 ||
      merge_status != 0) {
    transcript << "receipt_merge failed status " << merge_status << "\n";
    ok = false;
  } else {
    transcript << "receipt_merge exit 0 output " << merged_path << "\n";
  }

  std::string merged_contents;
  Receipt merged;
  if (!ReadTextFile(merged_path, &merged_contents) ||
      !ParseReceipt(merged_contents, &merged) ||
      merged.magic != "CMPCOV-MERGED") {
    transcript << "merged receipt unreadable or malformed\n";
    ok = false;
  } else {
    transcript << "merged magic " << merged.magic << " version "
               << merged.version << "\n";
    transcript << "merged source " << merged.source << "\n";
    transcript << "merged modules " << merged.modules.size()
               << " traces " << merged.traces.size() << "\n";
    if (merged.source != kSource ||
        merged.modules != std::set<std::string>{kBuild} ||
        round_identities != std::set<std::string>{kSource} ||
        merged.traces.empty()) {
      transcript << "merged identity check failed\n";
      ok = false;
    } else {
      transcript << "identity check: source and module set consistent\n";
    }
  }

  transcript << (ok ? "RESULT OK\n" : "RESULT FAIL\n");
  transcript.close();
  std::printf("transcript written to %s\n", transcript_path.c_str());
  return ok ? 0 : 1;
}
