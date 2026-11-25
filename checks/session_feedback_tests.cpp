// Content tests for the optional model-session feedback channel.
// Registers cases into the three groups required by task_support/driver.c:
// lifecycle, framing, features.

#include "task_support/check.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "centipede/runner_result.h"
#include "centipede/session_feedback.h"

using namespace fuzztest::internal;

namespace {

constexpr size_t kDefaultBufSize = 1u << 20;  // 1 MiB.

feature_t EdgeFeature(size_t number) {
  return feature_domains::kUserDomains[14].ConvertToMe(number);
}
feature_t TermFeature(size_t number) {
  return feature_domains::kUserDomains[15].ConvertToMe(number);
}

SessionEvent Ev(SessionEventKind kind, uint64_t session, uint64_t tick,
                uint64_t deadline, uint32_t sequence, uint32_t action) {
  return SessionEvent{kind, session, tick, deadline, sequence, action};
}
SessionEvent Begin(uint64_t sid, uint64_t tick = 0,
                   uint64_t deadline = 100) {
  return Ev(SessionEventKind::Begin, sid, tick, deadline, 0, 0);
}
SessionEvent Step(uint64_t sid, uint64_t tick, uint32_t sequence,
                  uint32_t action) {
  return Ev(SessionEventKind::Step, sid, tick, 0, sequence, action);
}
SessionEvent Term(SessionEventKind kind, uint64_t sid, uint64_t tick,
                  uint32_t sequence) {
  return Ev(kind, sid, tick, 0, sequence, 0);
}

// Owns the backing bytes together with the BlobSequence. The vector is never
// resized after construction so the sequence pointer stays valid.
struct Writer {
  explicit Writer(size_t size = kDefaultBufSize)
      : bytes(size, 0), seq(bytes.data(), bytes.size()) {}

  bool Event(const SessionEvent& e) {
    return BatchResult::WriteSessionEvent(e, seq);
  }
  bool BeginInput() { return BatchResult::WriteInputBegin(seq); }
  bool EndInput() { return BatchResult::WriteInputEnd(seq); }
  bool Features(std::initializer_list<feature_t> fs) {
    std::vector<feature_t> v(fs);
    return BatchResult::WriteOneFeatureVec(v.data(), v.size(), seq);
  }
  bool Raw(Blob::SizeAndTagT tag, Blob::SizeAndTagT size,
           const uint8_t* data) {
    return seq.Write(Blob(tag, size, data));
  }

  std::vector<uint8_t> bytes;
  BlobSequence seq;
};

// Resets the read cursor and runs the session reader on an already sized
// BatchResult.
bool RunRead(Writer& w, BatchResult& batch, const SessionModel& model) {
  w.seq.Reset();
  return batch.ReadSessionBatch(w.seq, model);
}

bool ReadNew(Writer& w, size_t n, const SessionModel& model,
             BatchResult& batch) {
  batch.ClearAndResize(n);
  return RunRead(w, batch, model);
}

// Model: 0 --10--> 1 --11--> 2 --12--> 3.
SessionModel LinearModel() {
  return SessionModel{0, {{0, 10, 1}, {1, 11, 2}, {2, 12, 3}}};
}

void PutLe64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// Writes a raw native TLV header into `buf` (same encoding BlobSequence uses).
void WriteRawHeader(std::vector<uint8_t>& buf, size_t at, size_t tag,
                    size_t size) {
  std::memcpy(buf.data() + at, &tag, sizeof(tag));
  std::memcpy(buf.data() + at + sizeof(tag), &size, sizeof(size));
}

// ---------------------------------------------------------------------------
// lifecycle group
// ---------------------------------------------------------------------------

int lc_single_finish_loopback() {
  SessionModel m = LinearModel();
  Writer w;
  TASK_CHECK(w.BeginInput());
  const SessionEvent a = Begin(1);
  const SessionEvent b = Step(1, 1, 1, 10);
  const SessionEvent c = Step(1, 2, 2, 11);
  const SessionEvent d = Term(SessionEventKind::Finish, 1, 3, 2);
  TASK_CHECK(w.Event(a) && w.Event(b) && w.Event(c) && w.Event(d));
  TASK_CHECK(w.EndInput());

  BatchResult batch;
  TASK_CHECK(ReadNew(w, 1, m, batch));
  TASK_CHECK(!w.seq.read_error());
  TASK_CHECK(batch.num_outputs_read() == 1);
  const auto& r = batch.results()[0];
  TASK_CHECK(r.session_events().size() == 4);
  TASK_CHECK(r.session_events()[0] == a);
  TASK_CHECK(r.session_events()[1] == b);
  TASK_CHECK(r.session_events()[2] == c);
  TASK_CHECK(r.session_events()[3] == d);
  FeatureVec expected{EdgeFeature(10), EdgeFeature(267), TermFeature(6)};
  std::sort(expected.begin(), expected.end());
  TASK_CHECK(r.features() == expected);
  return 0;
}

int lc_interleaved_sessions() {
  SessionModel m = LinearModel();
  Writer w;
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Event(Begin(1, 0, 100)));
  TASK_CHECK(w.Event(Begin(2, 0, 50)));
  TASK_CHECK(w.Event(Step(1, 1, 1, 10)));   // s1 0->1
  TASK_CHECK(w.Event(Step(2, 2, 1, 10)));   // s2 0->1
  TASK_CHECK(w.Event(Step(1, 3, 2, 11)));   // s1 1->2
  TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 1, 4, 2)));  // state 2 K0
  TASK_CHECK(w.Event(Term(SessionEventKind::Cancel, 2, 5, 1)));  // state 1 K1
  TASK_CHECK(w.EndInput());

  BatchResult batch;
  TASK_CHECK(ReadNew(w, 1, m, batch));
  const auto& r = batch.results()[0];
  TASK_CHECK(r.session_events().size() == 7);
  // Raw stream order preserved, interleaving intact.
  TASK_CHECK(r.session_events()[2].session == 1);
  TASK_CHECK(r.session_events()[3].session == 2);
  FeatureVec expected{EdgeFeature(10), EdgeFeature(267), TermFeature(6),
                      TermFeature(4)};
  std::sort(expected.begin(), expected.end());
  TASK_CHECK(r.features() == expected);
  return 0;
}

int lc_cancel_reachable_state() {
  SessionModel m = LinearModel();
  Writer w;
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Event(Begin(9)));
  TASK_CHECK(w.Event(Step(9, 1, 1, 10)));  // 0->1
  TASK_CHECK(w.Event(Step(9, 2, 2, 11)));  // 1->2
  TASK_CHECK(w.Event(Term(SessionEventKind::Cancel, 9, 3, 2)));
  TASK_CHECK(w.EndInput());
  BatchResult batch;
  TASK_CHECK(ReadNew(w, 1, m, batch));
  const FeatureVec& f = batch.results()[0].features();
  TASK_CHECK(std::find(f.begin(), f.end(), TermFeature(2 * 3 + 1)) != f.end());
  TASK_CHECK(std::find(f.begin(), f.end(), TermFeature(6)) == f.end());
  TASK_CHECK(std::find(f.begin(), f.end(), TermFeature(8)) == f.end());
  return 0;
}

int lc_timeout_boundary() {
  SessionModel m = LinearModel();
  // Timeout exactly at deadline is legal.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1, 0, 10)));
    TASK_CHECK(w.Event(Step(1, 9, 1, 10)));  // 0->1, tick < deadline
    TASK_CHECK(w.Event(Term(SessionEventKind::Timeout, 1, 10, 1)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(ReadNew(w, 1, m, batch));
    const FeatureVec& f = batch.results()[0].features();
    TASK_CHECK(std::find(f.begin(), f.end(), TermFeature(1 * 3 + 2)) !=
               f.end());
  }
  // Timeout well after deadline also legal.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1, 0, 10)));
    TASK_CHECK(w.Event(Term(SessionEventKind::Timeout, 1, 42, 0)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(ReadNew(w, 1, m, batch));
    TASK_CHECK(batch.results()[0].features() ==
               FeatureVec{TermFeature(2)});  // state 0, K=2 -> 2
  }
  // A Step at/after deadline is illegal.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1, 0, 10)));
    TASK_CHECK(w.Event(Step(1, 10, 1, 10)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  // Timeout before deadline is illegal.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1, 0, 10)));
    TASK_CHECK(w.Event(Term(SessionEventKind::Timeout, 1, 9, 0)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  // Finish/Cancel exactly at the deadline are illegal (must be earlier).
  for (SessionEventKind kind :
       {SessionEventKind::Finish, SessionEventKind::Cancel}) {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1, 0, 10)));
    TASK_CHECK(w.Event(Term(kind, 1, 10, 0)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  // A terminal tick that regresses past the last Step tick is illegal.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1, 0, 100)));
    TASK_CHECK(w.Event(Step(1, 50, 1, 10)));
    TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 1, 49, 1)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  return 0;
}

int lc_terminal_without_steps() {
  SessionModel m = LinearModel();
  Writer w;
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Event(Begin(3, 5, 10)));
  TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 3, 5, 0)));
  TASK_CHECK(w.EndInput());
  BatchResult batch;
  TASK_CHECK(ReadNew(w, 1, m, batch));
  TASK_CHECK(batch.results()[0].features() == FeatureVec{TermFeature(0)});
  return 0;
}

int lc_same_tick_steps_and_terminal() {
  SessionModel m{0, {{0, 1, 0}}};  // self loop
  Writer w;
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Event(Begin(1, 0, 5)));
  TASK_CHECK(w.Event(Step(1, 4, 1, 1)));
  TASK_CHECK(w.Event(Step(1, 4, 2, 1)));
  TASK_CHECK(w.Event(Step(1, 4, 3, 1)));
  TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 1, 4, 3)));
  TASK_CHECK(w.EndInput());
  BatchResult batch;
  TASK_CHECK(ReadNew(w, 1, m, batch));
  FeatureVec expected{EdgeFeature(1), TermFeature(0)};
  std::sort(expected.begin(), expected.end());
  TASK_CHECK(batch.results()[0].features() == expected);
  return 0;
}

int lc_identity_reuse_across_inputs() {
  SessionModel m = LinearModel();
  Writer w;
  // Input 0: session id 7 takes two edges.
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Event(Begin(7)));
  TASK_CHECK(w.Event(Step(7, 1, 1, 10)));
  TASK_CHECK(w.Event(Step(7, 2, 2, 11)));
  TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 7, 3, 2)));
  TASK_CHECK(w.EndInput());
  // Input 1: same id 7 reopens from the reset initial state, one edge.
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Event(Begin(7)));
  TASK_CHECK(w.Event(Step(7, 1, 1, 10)));
  TASK_CHECK(w.Event(Term(SessionEventKind::Cancel, 7, 2, 1)));
  TASK_CHECK(w.EndInput());

  BatchResult batch;
  TASK_CHECK(ReadNew(w, 2, m, batch));
  TASK_CHECK(batch.num_outputs_read() == 2);
  const FeatureVec& f0 = batch.results()[0].features();
  const FeatureVec& f1 = batch.results()[1].features();
  TASK_CHECK(std::find(f0.begin(), f0.end(), EdgeFeature(267)) != f0.end());
  TASK_CHECK(std::find(f1.begin(), f1.end(), EdgeFeature(267)) == f1.end());
  TASK_CHECK(std::find(f1.begin(), f1.end(), TermFeature(4)) != f1.end());
  TASK_CHECK(batch.results()[0].session_events().size() == 4);
  TASK_CHECK(batch.results()[1].session_events().size() == 3);
  return 0;
}

int lc_three_complete_inputs_and_edge_cap() {
  // Three complete inputs into four slots; the unused slot is cleared.
  Writer w;
  for (uint64_t sid = 1; sid <= 3; ++sid) {
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(sid)));
    TASK_CHECK(w.Event(Term(SessionEventKind::Finish, sid, 1, 0)));
    TASK_CHECK(w.EndInput());
  }
  BatchResult batch;
  batch.ClearAndResize(4);
  batch.results()[3].mutable_features().push_back(999);
  TASK_CHECK(RunRead(w, batch, SessionModel{0, {}}));
  TASK_CHECK(batch.num_outputs_read() == 3);
  for (size_t i = 0; i < 3; ++i) {
    TASK_CHECK(batch.results()[i].session_events().size() == 2);
  }
  TASK_CHECK(batch.results()[3].features().empty());
  TASK_CHECK(batch.results()[3].session_events().empty());

  // Exactly 4096 distinct edges is a valid model.
  SessionModel cap_model{0, {}};
  for (uint32_t i = 0; i < 4096; ++i) {
    cap_model.edges.push_back({i / 256, i % 256, 0});
  }
  TASK_CHECK(ValidateSessionModel(cap_model));
  return 0;
}

int lc_empty_model() {
  SessionModel m{0, {}};
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1)));
    TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 1, 1, 0)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(ReadNew(w, 1, m, batch));
    TASK_CHECK(batch.results()[0].features() == FeatureVec{TermFeature(0)});
  }
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1)));
    TASK_CHECK(w.Event(Step(1, 1, 1, 99)));  // no declared edge
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  return 0;
}

int lc_session_count_limit() {
  SessionModel m{0, {}};
  {
    Writer w(1u << 16);
    TASK_CHECK(w.BeginInput());
    for (uint64_t sid = 1; sid <= 64; ++sid) {
      TASK_CHECK(w.Event(Begin(sid)));
      TASK_CHECK(w.Event(Term(SessionEventKind::Finish, sid, 1, 0)));
    }
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(ReadNew(w, 1, m, batch));
    TASK_CHECK(batch.results()[0].session_events().size() == 128);
  }
  {
    Writer w(1u << 16);
    TASK_CHECK(w.BeginInput());
    for (uint64_t sid = 1; sid <= 65; ++sid) {
      TASK_CHECK(w.Event(Begin(sid)));
      TASK_CHECK(w.Event(Term(SessionEventKind::Finish, sid, 1, 0)));
    }
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  return 0;
}

int lc_step_count_limit() {
  SessionModel m{0, {{0, 1, 0}}};  // self loop keeps edge lookup legal.
  // Exactly 4096 steps is legal.
  {
    Writer w(1u << 19);
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1, 0, 2)));
    for (uint32_t i = 1; i <= 4096; ++i) {
      TASK_CHECK(w.Event(Step(1, 1, i, 1)));
    }
    TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 1, 1, 4096)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(ReadNew(w, 1, m, batch));
    // Repeated edge contributes once.
    const FeatureVec& f = batch.results()[0].features();
    TASK_CHECK(std::count(f.begin(), f.end(), EdgeFeature(1)) == 1);
  }
  // 4097th step rejected.
  {
    Writer w(1u << 19);
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1, 0, 2)));
    for (uint32_t i = 1; i <= 4097; ++i) {
      TASK_CHECK(w.Event(Step(1, 1, i, 1)));
    }
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  return 0;
}

int lc_total_event_limit() {
  SessionModel m{0, {{0, 1, 0}}};
  auto build = [&](Writer& w, int extra) -> bool {
    if (!w.BeginInput()) return false;
    // 16 sessions x (Begin + 4094 Steps + Finish) = 65536 events.
    for (uint64_t sid = 1; sid <= 16; ++sid) {
      if (!w.Event(Begin(sid, 0, 2))) return false;
      for (uint32_t i = 1; i <= 4094; ++i) {
        if (!w.Event(Step(sid, 1, i, 1))) return false;
      }
      if (!w.Event(Term(SessionEventKind::Finish, sid, 1, 4094))) return false;
    }
    for (int i = 0; i < extra; ++i) {
      if (!w.Event(Begin(1000 + i))) return false;  // 65537th+ event.
    }
    return true;
  };
  {
    Writer w(8u << 20);
    TASK_CHECK(build(w, 0));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(ReadNew(w, 1, m, batch));
    TASK_CHECK(batch.results()[0].session_events().size() == 65536);
  }
  {
    Writer w(8u << 20);
    TASK_CHECK(build(w, 1));
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  return 0;
}

int lc_closed_session_rejects_later_events() {
  SessionModel m = LinearModel();
  // Step after Finish.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1)));
    TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 1, 1, 0)));
    TASK_CHECK(w.Event(Step(1, 2, 1, 10)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  // Double termination.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1)));
    TASK_CHECK(w.Event(Term(SessionEventKind::Cancel, 1, 1, 0)));
    TASK_CHECK(w.Event(Term(SessionEventKind::Cancel, 1, 2, 0)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  // Identity cannot reopen within the same input.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1)));
    TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 1, 1, 0)));
    TASK_CHECK(w.Event(Begin(1)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  return 0;
}

int lc_unclosed_session_at_end_rejected() {
  SessionModel m = LinearModel();
  Writer w;
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Event(Begin(1)));
  TASK_CHECK(w.Event(Step(1, 1, 1, 10)));
  // No termination before InputEnd.
  TASK_CHECK(w.EndInput());
  BatchResult batch;
  TASK_CHECK(!ReadNew(w, 1, m, batch));
  return 0;
}

int lc_tick_regression_and_sequence_gap() {
  SessionModel m = LinearModel();
  // Tick goes backwards.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1, 10, 100)));
    TASK_CHECK(w.Event(Step(1, 11, 1, 10)));
    TASK_CHECK(w.Event(Step(1, 10, 2, 11)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  // Sequence gap.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1)));
    TASK_CHECK(w.Event(Step(1, 1, 2, 10)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  // Terminal sequence must match the last Step.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1)));
    TASK_CHECK(w.Event(Step(1, 1, 1, 10)));
    TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 1, 2, 2)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  // Step on unknown session.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Step(42, 1, 1, 10)));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, m, batch));
  }
  return 0;
}

// ---------------------------------------------------------------------------
// framing group
// ---------------------------------------------------------------------------

int fr_empty_stream_clears_all_slots() {
  Writer w(128);  // all zero -> normal terminator.
  BatchResult batch;
  batch.ClearAndResize(2);
  batch.results()[0].mutable_features().push_back(12345);
  batch.num_outputs_read() = 9;
  TASK_CHECK(RunRead(w, batch, LinearModel()));
  TASK_CHECK(batch.num_outputs_read() == 0);
  TASK_CHECK(batch.results().size() == 2);
  TASK_CHECK(batch.results()[0].features().empty());
  TASK_CHECK(batch.results()[1].features().empty());
  TASK_CHECK(batch.results()[0].session_events().empty());
  return 0;
}

int fr_exact_buffer_boundary() {
  // InputBegin(16) + Begin event(56) + Finish event(56) + InputEnd(16) = 144.
  Writer w(144);
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Event(Begin(1)));
  TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 1, 1, 0)));
  TASK_CHECK(w.EndInput());
  TASK_CHECK(w.seq.offset() == 144);
  BatchResult batch;
  TASK_CHECK(ReadNew(w, 1, SessionModel{0, {}}, batch));
  TASK_CHECK(!w.seq.read_error());
  TASK_CHECK(batch.num_outputs_read() == 1);
  return 0;
}

int fr_nonzero_truncated_header() {
  std::vector<uint8_t> buf(24, 0);
  WriteRawHeader(buf, 0, /*tag=*/2, /*size=*/0);  // InputBegin, ends at 16.
  buf[16] = 9;  // Non-zero first byte of a truncated next header.
  BlobSequence seq(buf.data(), buf.size());
  BatchResult batch;
  batch.ClearAndResize(1);
  TASK_CHECK(!batch.ReadSessionBatch(seq, LinearModel()));
  TASK_CHECK(seq.read_error());

  // An all-zero short tail is not corruption.
  std::vector<uint8_t> zero_tail(24, 0);
  BlobSequence seq2(zero_tail.data(), zero_tail.size());
  Blob b1 = seq2.Read();
  TASK_CHECK(!b1.IsValid());
  TASK_CHECK(!seq2.read_error());
  return 0;
}

int fr_tag_zero_nonzero_size() {
  std::vector<uint8_t> buf(64, 0);
  WriteRawHeader(buf, 0, /*tag=*/0, /*size=*/5);
  BlobSequence seq(buf.data(), buf.size());
  Blob bad = seq.Read();
  TASK_CHECK(!bad.IsValid());
  TASK_CHECK(seq.read_error());
  seq.Reset();
  TASK_CHECK(!seq.read_error());
  BatchResult batch;
  batch.ClearAndResize(1);
  TASK_CHECK(!batch.ReadSessionBatch(seq, LinearModel()));
  return 0;
}

int fr_payload_out_of_bounds() {
  std::vector<uint8_t> buf(24, 0);
  WriteRawHeader(buf, 0, /*tag=*/9, /*size=*/40);  // claims 40 payload bytes.
  BlobSequence seq(buf.data(), buf.size());
  Blob bad = seq.Read();
  TASK_CHECK(!bad.IsValid());
  TASK_CHECK(seq.read_error());
  return 0;
}

int fr_unknown_tags_rejected() {
  for (Blob::SizeAndTagT bad_tag : {6, 7, 8, 10, 99}) {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Raw(bad_tag, 0, nullptr));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, LinearModel(), batch));
  }
  // Unknown tag before any Begin is also rejected (data without Begin).
  {
    Writer w;
    TASK_CHECK(w.Raw(99, 0, nullptr));
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, LinearModel(), batch));
  }
  return 0;
}

int fr_nested_begin_and_extra_end() {
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.BeginInput());  // nested
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 2, LinearModel(), batch));
  }
  {
    Writer w;
    TASK_CHECK(w.EndInput());  // extra End
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, LinearModel(), batch));
  }
  return 0;
}

int fr_input_count_limit_and_lone_tail_slot() {
  // N == 1: one complete input plus another Begin exceeds the limit.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.EndInput());
    TASK_CHECK(w.BeginInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, LinearModel(), batch));
  }
  // N == 1: a single Begin-only tail is legal and publishes zero outputs.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1)));
    BatchResult batch;
    TASK_CHECK(ReadNew(w, 1, LinearModel(), batch));
    TASK_CHECK(batch.num_outputs_read() == 0);
    TASK_CHECK(batch.results()[0].session_events().empty());
  }
  return 0;
}

int fr_tail_prefix_published_tail_discarded() {
  SessionModel m = LinearModel();
  Writer w;
  // Complete input 0 with ordinary + session coverage.
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Features({7, 9}));
  TASK_CHECK(w.Event(Begin(1)));
  TASK_CHECK(w.Event(Step(1, 1, 1, 10)));
  TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 1, 2, 1)));
  TASK_CHECK(w.EndInput());
  // Legal but unfinished tail input 1: coverage must be discarded.
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Features({777}));
  TASK_CHECK(w.Event(Begin(2)));
  TASK_CHECK(w.Event(Step(2, 1, 1, 10)));

  BatchResult batch;
  TASK_CHECK(ReadNew(w, 2, m, batch));
  TASK_CHECK(batch.num_outputs_read() == 1);
  const auto& r0 = batch.results()[0];
  TASK_CHECK(r0.features().size() == 4);  // 7, 9, edge, terminal sorted unique.
  TASK_CHECK(r0.session_events().size() == 3);
  const auto& r1 = batch.results()[1];
  TASK_CHECK(r1.features().empty());
  TASK_CHECK(r1.session_events().empty());
  TASK_CHECK(r1.metadata().cmp_data.empty());
  TASK_CHECK(r1.stats() == ExecutionResult::Stats{});
  return 0;
}

int fr_tail_illegal_event_fails_whole_read() {
  SessionModel m = LinearModel();
  Writer w;
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Features({7}));
  TASK_CHECK(w.EndInput());
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Event(Begin(2)));
  TASK_CHECK(w.Event(Step(2, 1, 5, 10)));  // sequence gap in tail.
  BatchResult batch;
  TASK_CHECK(!ReadNew(w, 2, m, batch));
  return 0;
}

int fr_corrupt_framing_not_half_input() {
  // Complete input, then a Begin, then a payload-overrun event header.
  std::vector<uint8_t> buf(512, 0);
  BlobSequence writer(buf.data(), buf.size());
  TASK_CHECK(BatchResult::WriteInputBegin(writer));
  TASK_CHECK(BatchResult::WriteInputEnd(writer));
  TASK_CHECK(BatchResult::WriteInputBegin(writer));
  const size_t off = writer.offset();
  // Native header claiming a huge payload.
  WriteRawHeader(buf, off, 9, 4096);

  BlobSequence seq(buf.data(), buf.size());
  BatchResult batch;
  batch.ClearAndResize(2);
  TASK_CHECK(!batch.ReadSessionBatch(seq, LinearModel()));
  TASK_CHECK(seq.read_error());
  TASK_CHECK(batch.num_outputs_read() == 0);  // untouched default.
  return 0;
}

int fr_bad_event_payloads_rejected() {
  // Wrong payload size.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    uint8_t small[39] = {};
    TASK_CHECK(w.Raw(9, sizeof(small), small));
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, LinearModel(), batch));
  }
  // Non-zero reserved byte 1..7.
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1)));
    // Locate the event payload: InputBegin header+empty = 16, event header
    // another 16, reserved byte starts at 16 + 16 + 1.
    w.bytes[16 + 16 + 1] = 7;
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, LinearModel(), batch));
  }
  // Unknown kind byte.
  {
    uint8_t payload[40] = {};
    payload[0] = 9;  // kind out of range.
    PutLe64(payload + 8, 1);
    PutLe64(payload + 24, 10);
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Raw(9, 40, payload));
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, LinearModel(), batch));
  }
  return 0;
}

int fr_writer_shape_validation_and_offset() {
  const SessionEvent bad_events[] = {
      Ev(static_cast<SessionEventKind>(0), 1, 0, 10, 0, 0),
      Ev(static_cast<SessionEventKind>(99), 1, 0, 10, 0, 0),
      Begin(0),                                  // zero session
      Ev(SessionEventKind::Begin, 1, 0, 0, 0, 0),  // deadline == tick
      Ev(SessionEventKind::Begin, 1, 5, 4, 0, 0),  // deadline < tick
      Ev(SessionEventKind::Begin, 1, 0, 10, 1, 0),  // Begin sequence != 0
      Ev(SessionEventKind::Begin, 1, 0, 10, 0, 1),  // Begin action != 0
      Ev(SessionEventKind::Step, 1, 1, 9, 1, 10),    // Step deadline non-zero
      Ev(SessionEventKind::Step, 1, 1, 0, 1, 256),   // action out of range
      Ev(SessionEventKind::Finish, 1, 1, 1, 0, 0),  // terminal deadline != 0
      Ev(SessionEventKind::Cancel, 1, 1, 0, 0, 3),   // terminal action != 0
  };
  for (const SessionEvent& bad : bad_events) {
    Writer w;
    TASK_CHECK(!BatchResult::WriteSessionEvent(bad, w.seq));
    TASK_CHECK(w.seq.offset() == 0);
  }
  // A failed write after a successful one does not move offset/bytes.
  Writer w;
  TASK_CHECK(w.Event(Begin(1)));
  const size_t offset_before = w.seq.offset();
  const uint8_t byte_before = w.bytes[offset_before];
  TASK_CHECK(!w.Event(Begin(0)));
  TASK_CHECK(w.seq.offset() == offset_before);
  TASK_CHECK(w.bytes[offset_before] == byte_before);
  return 0;
}

int fr_writer_capacity_failure() {
  std::vector<uint8_t> bytes(48, 0);  // event blob needs 16 + 40 = 56 bytes.
  BlobSequence seq(bytes.data(), bytes.size());
  TASK_CHECK(!BatchResult::WriteSessionEvent(Begin(1), seq));
  TASK_CHECK(seq.offset() == 0);
  return 0;
}

int fr_duplicate_ordinary_blobs_rejected() {
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Features({1}));
    TASK_CHECK(w.Features({2}));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, LinearModel(), batch));
  }
  {
    Writer w;
    ExecutionResult::Stats s;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(BatchResult::WriteStats(s, w.seq));
    TASK_CHECK(BatchResult::WriteStats(s, w.seq));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, LinearModel(), batch));
  }
  {
    Writer w;
    uint8_t meta[] = {2, 1, 2, 3, 4};
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(BatchResult::WriteMetadata(ByteSpan(meta, sizeof(meta)), w.seq));
    TASK_CHECK(BatchResult::WriteMetadata(ByteSpan(meta, sizeof(meta)), w.seq));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, LinearModel(), batch));
  }
  return 0;
}

int fr_bad_feature_and_stats_sizes() {
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    uint8_t seven[7] = {};  // Not a multiple of feature_t (8).
    TASK_CHECK(w.Raw(1, 7, seven));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, LinearModel(), batch));
  }
  {
    Writer w;
    TASK_CHECK(w.BeginInput());
    uint8_t bad_stats[24] = {};  // Stats is 32 bytes.
    TASK_CHECK(w.Raw(4, sizeof(bad_stats), bad_stats));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, LinearModel(), batch));
  }
  return 0;
}

int fr_error_does_not_partially_rewrite() {
  SessionModel m = LinearModel();
  Writer good;
  TASK_CHECK(good.BeginInput());
  TASK_CHECK(good.Features({7, 9}));
  ExecutionResult::Stats stats;
  stats.peak_rss_mb = 31;
  stats.exec_time_usec = 1234;
  TASK_CHECK(BatchResult::WriteStats(stats, good.seq));
  uint8_t meta_bytes[] = {2, 1, 2, 3, 4};
  TASK_CHECK(BatchResult::WriteMetadata(
      ByteSpan(meta_bytes, sizeof(meta_bytes)), good.seq));
  TASK_CHECK(good.Event(Begin(1)));
  TASK_CHECK(good.Event(Step(1, 1, 1, 10)));
  TASK_CHECK(good.Event(Term(SessionEventKind::Finish, 1, 2, 1)));
  TASK_CHECK(good.EndInput());

  BatchResult batch;
  TASK_CHECK(ReadNew(good, 1, m, batch));
  const FeatureVec saved_features = batch.results()[0].features();
  const auto saved_events = batch.results()[0].session_events();
  const auto saved_stats = batch.results()[0].stats();
  const ByteArray saved_meta = batch.results()[0].metadata().cmp_data;
  batch.log() = "sentinel-log";
  batch.exit_code() = 77;
  batch.failure_description() = "sentinel-desc";
  batch.failure_signature() = "sentinel-sig";
  const size_t saved_num = batch.num_outputs_read();

  // Illegal second read must leave everything byte-for-byte as before.
  Writer bad;
  TASK_CHECK(bad.BeginInput());
  TASK_CHECK(bad.Event(Begin(1)));
  TASK_CHECK(bad.Event(Step(1, 1, 9, 10)));  // sequence gap.
  TASK_CHECK(bad.EndInput());
  TASK_CHECK(!RunRead(bad, batch, m));

  TASK_CHECK(batch.num_outputs_read() == saved_num);
  TASK_CHECK(batch.results()[0].features() == saved_features);
  TASK_CHECK(batch.results()[0].session_events() == saved_events);
  TASK_CHECK(batch.results()[0].stats() == saved_stats);
  TASK_CHECK(batch.results()[0].metadata().cmp_data == saved_meta);
  TASK_CHECK(batch.log() == "sentinel-log");
  TASK_CHECK(batch.exit_code() == 77);
  TASK_CHECK(batch.failure_description() == "sentinel-desc");
  TASK_CHECK(batch.failure_signature() == "sentinel-sig");

  // Invalid model also preserves.
  SessionModel bad_model{256, {}};
  Writer empty;
  TASK_CHECK(!batch.ReadSessionBatch(empty.seq, bad_model));
  TASK_CHECK(batch.num_outputs_read() == saved_num);
  TASK_CHECK(batch.results()[0].features() == saved_features);
  return 0;
}

// ---------------------------------------------------------------------------
// features group
// ---------------------------------------------------------------------------

int ft_merge_sort_unique() {
  SessionModel m{0, {{0, 10, 0}}};  // repeated self-loop edge.
  Writer w;
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Features({42, 7, 42, 1000, 7}));  // unsorted + duplicates.
  TASK_CHECK(w.Event(Begin(1)));
  TASK_CHECK(w.Event(Step(1, 1, 1, 10)));
  TASK_CHECK(w.Event(Step(1, 1, 2, 10)));  // same edge again.
  TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 1, 2, 2)));
  TASK_CHECK(w.EndInput());
  BatchResult batch;
  TASK_CHECK(ReadNew(w, 1, m, batch));
  FeatureVec expected{7, 42, 1000, EdgeFeature(10), TermFeature(0)};
  std::sort(expected.begin(), expected.end());
  expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
  const FeatureVec& actual = batch.results()[0].features();
  TASK_CHECK(actual == expected);
  TASK_CHECK(std::is_sorted(actual.begin(), actual.end()));
  TASK_CHECK(std::adjacent_find(actual.begin(), actual.end()) == actual.end());
  return 0;
}

int ft_reserved_domains_reject_ordinary() {
  for (int domain_id : {14, 15}) {
    feature_t reserved =
        feature_domains::kUserDomains[domain_id].ConvertToMe(123);
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Features({7, reserved}));
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(!ReadNew(w, 1, LinearModel(), batch));
  }
  // Neighboring domains remain acceptable.
  feature_t neighbor = feature_domains::kUserDomains[13].ConvertToMe(123);
  Writer w;
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Features({neighbor}));
  TASK_CHECK(w.EndInput());
  BatchResult batch;
  TASK_CHECK(ReadNew(w, 1, LinearModel(), batch));
  TASK_CHECK(batch.results()[0].features() == FeatureVec{neighbor});
  return 0;
}

int ft_edge_feature_exact_value() {
  SessionModel m{5, {{5, 200, 9}}};
  Writer w;
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Event(Begin(1)));
  TASK_CHECK(w.Event(Step(1, 1, 1, 200)));
  TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 1, 2, 1)));
  TASK_CHECK(w.EndInput());
  BatchResult batch;
  TASK_CHECK(ReadNew(w, 1, m, batch));
  FeatureVec expected{EdgeFeature(5 * 256 + 200), TermFeature(9 * 3 + 0)};
  std::sort(expected.begin(), expected.end());
  TASK_CHECK(batch.results()[0].features() == expected);
  return 0;
}

int ft_terminal_k_mapping() {
  const SessionEventKind kinds[3] = {SessionEventKind::Finish,
                                     SessionEventKind::Cancel,
                                     SessionEventKind::Timeout};
  const uint32_t ks[3] = {0, 1, 2};
  for (int i = 0; i < 3; ++i) {
    Writer w;
    TASK_CHECK(w.BeginInput());
    TASK_CHECK(w.Event(Begin(1, 0, 10)));
    if (kinds[i] == SessionEventKind::Timeout) {
      TASK_CHECK(w.Event(Term(kinds[i], 1, 10, 0)));
    } else {
      TASK_CHECK(w.Event(Term(kinds[i], 1, 1, 0)));
    }
    TASK_CHECK(w.EndInput());
    BatchResult batch;
    TASK_CHECK(ReadNew(w, 1, SessionModel{3, {}}, batch));
    TASK_CHECK(batch.results()[0].features() ==
               FeatureVec{TermFeature(3 * 3 + ks[i])});
  }
  return 0;
}

int ft_declaration_order_independent() {
  auto run = [&](const SessionModel& m) -> FeatureVec {
    Writer w;
    if (!w.BeginInput() || !w.Event(Begin(1)) ||
        !w.Event(Step(1, 1, 1, 10)) || !w.Event(Step(1, 2, 2, 11)) ||
        !w.Event(Term(SessionEventKind::Finish, 1, 3, 2)) || !w.EndInput()) {
      return {};
    }
    BatchResult batch;
    if (!ReadNew(w, 1, m, batch)) return {};
    return batch.results()[0].features();
  };
  FeatureVec a = run(SessionModel{0, {{0, 10, 1}, {1, 11, 2}}});
  FeatureVec b = run(SessionModel{0, {{1, 11, 2}, {0, 10, 1}}});
  TASK_CHECK(!a.empty());
  TASK_CHECK(a == b);
  return 0;
}

int ft_unreachable_edges_produce_nothing() {
  SessionModel m{0, {{0, 1, 1}, {9, 2, 8}}};  // second edge unreachable.
  Writer w;
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(w.Event(Begin(1)));
  TASK_CHECK(w.Event(Step(1, 1, 1, 1)));
  TASK_CHECK(w.Event(Term(SessionEventKind::Finish, 1, 2, 1)));
  TASK_CHECK(w.EndInput());
  BatchResult batch;
  TASK_CHECK(ReadNew(w, 1, m, batch));
  const FeatureVec& f = batch.results()[0].features();
  TASK_CHECK(std::find(f.begin(), f.end(), EdgeFeature(1)) != f.end());
  TASK_CHECK(std::find(f.begin(), f.end(), EdgeFeature(9 * 256 + 2)) ==
             f.end());
  return 0;
}

int ft_model_validation() {
  TASK_CHECK(ValidateSessionModel(SessionModel{0, {}}));
  TASK_CHECK(ValidateSessionModel(SessionModel{255, {{255, 255, 0}}}));
  TASK_CHECK(!ValidateSessionModel(SessionModel{256, {}}));
  TASK_CHECK(!ValidateSessionModel(SessionModel{0, {{256, 0, 1}}}));
  TASK_CHECK(!ValidateSessionModel(SessionModel{0, {{0, 256, 1}}}));
  TASK_CHECK(!ValidateSessionModel(SessionModel{0, {{0, 0, 256}}}));
  // Identical repeated (from, action) is still forbidden.
  TASK_CHECK(!ValidateSessionModel(
      SessionModel{0, {{0, 1, 2}, {0, 1, 2}}}));
  // Conflicting repeat too.
  TASK_CHECK(!ValidateSessionModel(
      SessionModel{0, {{0, 1, 2}, {0, 1, 3}}}));
  // Different actions from the same state are fine.
  TASK_CHECK(ValidateSessionModel(
      SessionModel{0, {{0, 1, 2}, {0, 2, 3}}}));
  // Edge cap: construct 4097 distinct (from, action) pairs.
  SessionModel huge{0, {}};
  for (uint32_t i = 0; i < 4097; ++i) {
    huge.edges.push_back({i / 256, i % 256, 0});
  }
  TASK_CHECK(huge.edges.size() == 4097);
  TASK_CHECK(!ValidateSessionModel(huge));
  return 0;
}

int ft_wire_payload_is_stable_little_endian() {
  // Begin exercises session/tick/deadline bytes.
  SessionEvent begin{SessionEventKind::Begin, 0x1122334455667788ULL,
                     0x0102030405060708ULL, 0x8877665544332211ULL, 0, 0};
  uint8_t p[40];
  EncodeSessionEventPayload(begin, p);
  TASK_CHECK(p[0] == 1);
  for (int i = 1; i <= 7; ++i) TASK_CHECK(p[i] == 0);
  const uint8_t expect_session[8] = {0x88, 0x77, 0x66, 0x55,
                                     0x44, 0x33, 0x22, 0x11};
  TASK_CHECK(std::memcmp(p + 8, expect_session, 8) == 0);
  TASK_CHECK(p[16] == 0x08 && p[23] == 0x01);  // tick LE
  TASK_CHECK(p[24] == 0x11 && p[31] == 0x88);  // deadline LE
  TASK_CHECK(p[32] == 0 && p[39] == 0);
  SessionEvent decoded;
  TASK_CHECK(DecodeSessionEventPayload(p, 40, &decoded));
  TASK_CHECK(decoded == begin);

  // Step exercises sequence/action bytes and the zero deadline field.
  SessionEvent step{SessionEventKind::Step, 1, 5, 0, 0xAABBCCDDu, 200};
  EncodeSessionEventPayload(step, p);
  TASK_CHECK(p[0] == 2);
  TASK_CHECK(p[24] == 0 && p[31] == 0);
  TASK_CHECK(p[32] == 0xDD && p[33] == 0xCC && p[34] == 0xBB &&
             p[35] == 0xAA);
  TASK_CHECK(p[36] == 200 && p[37] == 0 && p[38] == 0 && p[39] == 0);
  TASK_CHECK(DecodeSessionEventPayload(p, 40, &decoded));
  TASK_CHECK(decoded == step);

  // Size must be exactly 40.
  TASK_CHECK(!DecodeSessionEventPayload(p, 39, &decoded));
  TASK_CHECK(!DecodeSessionEventPayload(p, 41, &decoded));
  return 0;
}

int ft_old_read_interface_unchanged() {
  Writer w;
  feature_t f1[] = {7, 9};
  TASK_CHECK(w.BeginInput());
  TASK_CHECK(BatchResult::WriteOneFeatureVec(f1, 2, w.seq));
  // The old reader simply ignores unknown tag-9 blobs.
  TASK_CHECK(w.Event(Begin(1)));
  TASK_CHECK(w.EndInput());
  w.seq.Reset();
  BatchResult batch;
  batch.ClearAndResize(1);
  TASK_CHECK(batch.Read(w.seq));
  TASK_CHECK(batch.num_outputs_read() == 1);
  TASK_CHECK(batch.results()[0].features() == FeatureVec({7, 9}));
  TASK_CHECK(batch.results()[0].session_events().empty());
  return 0;
}

}  // namespace

// check.h declares these with ordinary C++ linkage and task_support/driver.c
// is built as C++ too (run.sh uses -x c++), so no extern "C" here.
const task_case task_cases[] = {
    {"lifecycle", "single_finish_loopback", lc_single_finish_loopback},
    {"lifecycle", "interleaved_sessions", lc_interleaved_sessions},
    {"lifecycle", "cancel_reachable_state", lc_cancel_reachable_state},
    {"lifecycle", "timeout_boundary", lc_timeout_boundary},
    {"lifecycle", "terminal_without_steps", lc_terminal_without_steps},
    {"lifecycle", "same_tick_steps_and_terminal",
     lc_same_tick_steps_and_terminal},
    {"lifecycle", "identity_reuse_across_inputs",
     lc_identity_reuse_across_inputs},
    {"lifecycle", "three_complete_inputs_and_edge_cap",
     lc_three_complete_inputs_and_edge_cap},
    {"lifecycle", "empty_model", lc_empty_model},
    {"lifecycle", "session_count_limit", lc_session_count_limit},
    {"lifecycle", "step_count_limit", lc_step_count_limit},
    {"lifecycle", "total_event_limit", lc_total_event_limit},
    {"lifecycle", "closed_session_rejects_later_events",
     lc_closed_session_rejects_later_events},
    {"lifecycle", "unclosed_session_at_end_rejected",
     lc_unclosed_session_at_end_rejected},
    {"lifecycle", "tick_regression_and_sequence_gap",
     lc_tick_regression_and_sequence_gap},

    {"framing", "empty_stream_clears_all_slots",
     fr_empty_stream_clears_all_slots},
    {"framing", "exact_buffer_boundary", fr_exact_buffer_boundary},
    {"framing", "nonzero_truncated_header", fr_nonzero_truncated_header},
    {"framing", "tag_zero_nonzero_size", fr_tag_zero_nonzero_size},
    {"framing", "payload_out_of_bounds", fr_payload_out_of_bounds},
    {"framing", "unknown_tags_rejected", fr_unknown_tags_rejected},
    {"framing", "nested_begin_and_extra_end",
     fr_nested_begin_and_extra_end},
    {"framing", "input_count_limit_and_lone_tail_slot",
     fr_input_count_limit_and_lone_tail_slot},
    {"framing", "tail_prefix_published_tail_discarded",
     fr_tail_prefix_published_tail_discarded},
    {"framing", "tail_illegal_event_fails_whole_read",
     fr_tail_illegal_event_fails_whole_read},
    {"framing", "corrupt_framing_not_half_input",
     fr_corrupt_framing_not_half_input},
    {"framing", "bad_event_payloads_rejected",
     fr_bad_event_payloads_rejected},
    {"framing", "writer_shape_validation_and_offset",
     fr_writer_shape_validation_and_offset},
    {"framing", "writer_capacity_failure", fr_writer_capacity_failure},
    {"framing", "duplicate_ordinary_blobs_rejected",
     fr_duplicate_ordinary_blobs_rejected},
    {"framing", "bad_feature_and_stats_sizes",
     fr_bad_feature_and_stats_sizes},
    {"framing", "error_does_not_partially_rewrite",
     fr_error_does_not_partially_rewrite},

    {"features", "merge_sort_unique", ft_merge_sort_unique},
    {"features", "reserved_domains_reject_ordinary",
     ft_reserved_domains_reject_ordinary},
    {"features", "edge_feature_exact_value", ft_edge_feature_exact_value},
    {"features", "terminal_k_mapping", ft_terminal_k_mapping},
    {"features", "declaration_order_independent",
     ft_declaration_order_independent},
    {"features", "unreachable_edges_produce_nothing",
     ft_unreachable_edges_produce_nothing},
    {"features", "model_validation", ft_model_validation},
    {"features", "wire_payload_is_stable_little_endian",
     ft_wire_payload_is_stable_little_endian},
    {"features", "old_read_interface_unchanged",
     ft_old_read_interface_unchanged},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
