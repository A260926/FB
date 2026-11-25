// examples/session_feedback_replay.cpp
//
// End-to-end producer/consumer example for the model-session feedback
// channel. Two local child processes build real production BlobSequence
// byte streams with BatchResult::Write* / WriteSessionEvent and ship the
// bytes over pipes. Pipe handshakes order the children (no sleeps).
//
//   child A: two inputs, benign sessions ending normally (Finish),
//            including interleaved sessions in input 1.
//   child B: one Cancel input, one Timeout input and one legal but
//            InputEnd-less trailing input whose coverage is discarded.
//
// The parent reads both streams with BatchResult::ReadSessionBatch, merges
// the completed inputs under stable global input numbers (0..3), checks the
// decoded content and writes transcript.txt into the output directory
// passed as argv[1].

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "centipede/runner_result.h"
#include "centipede/session_feedback.h"

using namespace fuzztest::internal;

namespace {

#define EX_CHECK(cond)                                                     \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "example check failed: %s (%s:%d)\n", #cond,    \
                   __FILE__, __LINE__);                                    \
      return 1;                                                            \
    }                                                                      \
  } while (0)

constexpr size_t kChildBufferSize = 1u << 16;

// Benign protocol model: IDLE --hello(10)--> GREETED --join(11)-->
// JOINED --fetch(12)--> STREAMING.
SessionModel ExampleModel() {
  return SessionModel{
      0, {{0, 10, 1}, {1, 11, 2}, {2, 12, 3}},
  };
}

SessionEvent BeginEv(uint64_t sid, uint64_t deadline = 100) {
  return SessionEvent{SessionEventKind::Begin, sid, 0, deadline, 0, 0};
}
SessionEvent StepEv(uint64_t sid, uint64_t tick, uint32_t seq,
                    uint32_t action) {
  return SessionEvent{SessionEventKind::Step, sid, tick, 0, seq, action};
}
SessionEvent TermEv(SessionEventKind kind, uint64_t sid, uint64_t tick,
                    uint32_t seq) {
  return SessionEvent{kind, sid, tick, 0, seq, 0};
}

bool WriteAll(int fd, const uint8_t* data, size_t size) {
  size_t off = 0;
  while (off < size) {
    ssize_t n = write(fd, data + off, size - off);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

std::vector<uint8_t> ReadAll(int fd, bool* ok) {
  std::vector<uint8_t> buf(4096);
  size_t off = 0;
  while (true) {
    if (off == buf.size()) buf.resize(buf.size() * 2);
    ssize_t n = read(fd, buf.data() + off, buf.size() - off);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      *ok = false;
      return {};
    }
    off += static_cast<size_t>(n);
  }
  buf.resize(off);
  *ok = true;
  return buf;
}

[[noreturn]] void ChildMain(int go_fd, int data_fd, int child_id) {
  char go = 0;
  ssize_t r = read(go_fd, &go, 1);  // Handshake: wait for parent's GO.
  if (r != 1 || go != 'G') _exit(3);

  std::vector<uint8_t> bytes(kChildBufferSize, 0);
  BlobSequence seq(bytes.data(), bytes.size());
  bool ok = true;

  auto put_feature = [&](std::initializer_list<feature_t> fs) {
    std::vector<feature_t> v(fs);
    ok = ok && BatchResult::WriteOneFeatureVec(v.data(), v.size(), seq);
  };
  auto ev = [&](const SessionEvent& e) {
    ok = ok && BatchResult::WriteSessionEvent(e, seq);
  };

  if (child_id == 0) {
    // ---- Global input 0: one session, normal Finish, full path. ----
    ok = ok && BatchResult::WriteInputBegin(seq);
    put_feature({1001, 1002});
    ExecutionResult::Stats stats;
    stats.exec_time_usec = 42;
    stats.peak_rss_mb = 7;
    ok = ok && BatchResult::WriteStats(stats, seq);
    ev(BeginEv(1001));
    ev(StepEv(1001, 1, 1, 10));
    ev(StepEv(1001, 2, 2, 11));
    ev(StepEv(1001, 3, 3, 12));
    ev(TermEv(SessionEventKind::Finish, 1001, 4, 3));
    ok = ok && BatchResult::WriteInputEnd(seq);

    // ---- Global input 1: two interleaved sessions, both Finish. ----
    ok = ok && BatchResult::WriteInputBegin(seq);
    put_feature({2001});
    ev(BeginEv(2001, 50));
    ev(BeginEv(2002, 60));
    ev(StepEv(2001, 1, 1, 10));  // 0->1
    ev(StepEv(2002, 2, 1, 10));  // 0->1
    ev(TermEv(SessionEventKind::Finish, 2001, 3, 1));
    ev(TermEv(SessionEventKind::Cancel, 2002, 4, 1));
    ok = ok && BatchResult::WriteInputEnd(seq);
  } else {
    // ---- Global input 2: Cancel before deadline. ----
    ok = ok && BatchResult::WriteInputBegin(seq);
    put_feature({3001});
    ev(BeginEv(3001, 20));
    ev(StepEv(3001, 1, 1, 10));  // 0->1
    ev(TermEv(SessionEventKind::Cancel, 3001, 5, 1));
    ok = ok && BatchResult::WriteInputEnd(seq);

    // ---- Global input 3: Timeout exactly at deadline. ----
    ok = ok && BatchResult::WriteInputBegin(seq);
    put_feature({4001});
    ev(BeginEv(4001, 10));
    ev(StepEv(4001, 9, 1, 10));  // 0->1, still before deadline.
    ev(TermEv(SessionEventKind::Timeout, 4001, 10, 1));
    ok = ok && BatchResult::WriteInputEnd(seq);

    // ---- Legal unfinished tail: must parse but be discarded. ----
    ok = ok && BatchResult::WriteInputBegin(seq);
    put_feature({9999});
    ev(BeginEv(4999, 10));
    ev(StepEv(4999, 1, 1, 10));
    // Intentionally no termination and no WriteInputEnd.
  }

  if (!ok) _exit(4);
  if (!WriteAll(data_fd, bytes.data(), seq.offset())) _exit(5);
  close(data_fd);
  _exit(0);
}

pid_t StartChild(int child_id, int* go_write_fd, int* data_read_fd) {
  int go_pipe[2];    // parent writes GO, child reads.
  int data_pipe[2];  // child writes bytes, parent reads.
  if (pipe(go_pipe) != 0 || pipe(data_pipe) != 0) return -1;
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    close(go_pipe[1]);
    close(data_pipe[0]);
    ChildMain(go_pipe[0], data_pipe[1], child_id);
  }
  close(go_pipe[0]);
  close(data_pipe[1]);
  *go_write_fd = go_pipe[1];
  *data_read_fd = data_pipe[0];
  return pid;
}

bool SignalAndCollect(pid_t pid, int go_fd, int data_fd,
                      std::vector<uint8_t>* bytes) {
  if (!WriteAll(go_fd, reinterpret_cast<const uint8_t*>("G"), 1)) return false;
  close(go_fd);
  bool read_ok = false;
  *bytes = ReadAll(data_fd, &read_ok);
  close(data_fd);
  if (!read_ok) return false;
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) return false;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string FeatureList(const FeatureVec& features) {
  std::string s = "[";
  for (size_t i = 0; i < features.size(); ++i) {
    if (i) s += ",";
    s += std::to_string(features[i]);
  }
  s += "]";
  return s;
}

const char* KindName(SessionEventKind kind) {
  switch (kind) {
    case SessionEventKind::Begin:
      return "Begin";
    case SessionEventKind::Step:
      return "Step";
    case SessionEventKind::Finish:
      return "Finish";
    case SessionEventKind::Cancel:
      return "Cancel";
    case SessionEventKind::Timeout:
      return "Timeout";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
    return 2;
  }
  const std::string out_dir = argv[1];
  mkdir(out_dir.c_str(), 0755);  // usually already a fresh mktemp directory.

  const SessionModel model = ExampleModel();

  int go_a = -1, data_a = -1, go_b = -1, data_b = -1;
  pid_t pid_a = StartChild(0, &go_a, &data_a);
  EX_CHECK(pid_a > 0);
  pid_t pid_b = StartChild(1, &go_b, &data_b);
  EX_CHECK(pid_b > 0);

  // Handshake order: fully collect child A before releasing child B.
  std::vector<uint8_t> bytes_a;
  EX_CHECK(SignalAndCollect(pid_a, go_a, data_a, &bytes_a));
  std::vector<uint8_t> bytes_b;
  EX_CHECK(SignalAndCollect(pid_b, go_b, data_b, &bytes_b));

  BlobSequence seq_a(bytes_a.data(), bytes_a.size());
  BatchResult batch_a;
  batch_a.ClearAndResize(2);
  EX_CHECK(batch_a.ReadSessionBatch(seq_a, model));
  EX_CHECK(!seq_a.read_error());
  EX_CHECK(batch_a.num_outputs_read() == 2);

  BlobSequence seq_b(bytes_b.data(), bytes_b.size());
  BatchResult batch_b;
  batch_b.ClearAndResize(3);
  EX_CHECK(batch_b.ReadSessionBatch(seq_b, model));
  EX_CHECK(!seq_b.read_error());
  EX_CHECK(batch_b.num_outputs_read() == 2);
  // The unfinished tail slot must be fully cleared.
  EX_CHECK(batch_b.results()[2].features().empty());
  EX_CHECK(batch_b.results()[2].session_events().empty());

  // Content verification against the stable model feature formulas.
  const feature_t edge_10 =
      feature_domains::kUserDomains[14].ConvertToMe(0 * 256 + 10);
  const feature_t edge_267 =
      feature_domains::kUserDomains[14].ConvertToMe(1 * 256 + 11);
  const feature_t edge_524 =
      feature_domains::kUserDomains[14].ConvertToMe(2 * 256 + 12);
  const feature_t finish_state3 =
      feature_domains::kUserDomains[15].ConvertToMe(3 * 3 + 0);
  const feature_t finish_state1 =
      feature_domains::kUserDomains[15].ConvertToMe(1 * 3 + 0);
  const feature_t cancel_state1 =
      feature_domains::kUserDomains[15].ConvertToMe(1 * 3 + 1);
  const feature_t timeout_state1 =
      feature_domains::kUserDomains[15].ConvertToMe(1 * 3 + 2);

  const auto& a0 = batch_a.results()[0];
  const auto& a1 = batch_a.results()[1];
  const auto& b0 = batch_b.results()[0];
  const auto& b1 = batch_b.results()[1];

  EX_CHECK(a0.session_events().size() == 5);
  EX_CHECK(a0.stats().peak_rss_mb == 7);
  for (feature_t f : {feature_t{1001}, feature_t{1002}, edge_10, edge_267,
                      edge_524, finish_state3}) {
    EX_CHECK(std::find(a0.features().begin(), a0.features().end(), f) !=
             a0.features().end());
  }
  EX_CHECK(a1.session_events().size() == 6);
  for (feature_t f :
       {feature_t{2001}, edge_10, finish_state1, cancel_state1}) {
    EX_CHECK(std::find(a1.features().begin(), a1.features().end(), f) !=
             a1.features().end());
  }
  EX_CHECK(b0.session_events().size() == 3);
  EX_CHECK(std::find(b0.features().begin(), b0.features().end(),
                     feature_t{3001}) != b0.features().end());
  EX_CHECK(std::find(b0.features().begin(), b0.features().end(),
                     cancel_state1) != b0.features().end());
  EX_CHECK(b1.session_events().size() == 3);
  EX_CHECK(std::find(b1.features().begin(), b1.features().end(),
                     feature_t{4001}) != b1.features().end());
  EX_CHECK(std::find(b1.features().begin(), b1.features().end(),
                     timeout_state1) != b1.features().end());
  // Tail ordinary coverage 9999 must not leak into any published input.
  for (const auto* r : {&b0, &b1}) {
    EX_CHECK(std::find(r->features().begin(), r->features().end(),
                       feature_t{9999}) == r->features().end());
  }

  // Merge by stable global input number: A owns 0,1 and B owns 2,3.
  struct MergedInput {
    int global_number;
    const char* producer;
    const ExecutionResult* result;
    const char* outcome;
  };
  const MergedInput merged[4] = {
      {0, "child-a", &a0, "finish/full-path"},
      {1, "child-a", &a1, "finish+cancel/interleaved"},
      {2, "child-b", &b0, "cancel"},
      {3, "child-b", &b1, "timeout"},
  };

  std::ofstream transcript(out_dir + "/transcript.txt",
                           std::ios::out | std::ios::trunc);
  EX_CHECK(transcript.good());
  transcript << "Centipede session feedback replay transcript\n";
  transcript << "model: initial=0 edges=(0,10)->1 (1,11)->2 (2,12)->3\n";
  transcript << "producers=2 transport=pipe handshake=go-byte\n";
  transcript << "completed_inputs=4 discarded_tail_inputs=1\n";
  for (const MergedInput& in : merged) {
    transcript << "INPUT " << in.global_number << " producer=" << in.producer
               << " outcome=" << in.outcome << "\n";
    transcript << "  events=" << in.result->session_events().size()
               << " features=" << FeatureList(in.result->features()) << "\n";
    transcript << "  trajectory:";
    for (const SessionEvent& e : in.result->session_events()) {
      transcript << " " << KindName(e.kind) << "#" << e.session << "@"
                 << e.tick;
    }
    transcript << "\n";
  }
  transcript << "MERGED inputs=0..3 ordered_by=stable_input_number\n";
  transcript << "TRANSCRIPT_OK\n";
  transcript.close();
  EX_CHECK(transcript.good());

  std::printf("example: merged 4 completed inputs, tail discarded; "
              "wrote %s/transcript.txt\n",
              out_dir.c_str());
  return 0;
}
