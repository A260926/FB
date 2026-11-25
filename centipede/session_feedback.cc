// Copyright 2025 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/session_feedback.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "./centipede/feature.h"
#include "./centipede/runner_result.h"
#include "./centipede/shared_memory_blob_sequence.h"

namespace fuzztest::internal {

namespace {

// Execution-result blob tags, fixed by the existing protocol in
// runner_result.cc. Tags 6..8 belong to the mutation protocol and are
// intentionally rejected by ReadSessionBatch as unknown tags.
inline constexpr Blob::SizeAndTagT kTagFeatures = 1;
inline constexpr Blob::SizeAndTagT kTagInputBegin = 2;
inline constexpr Blob::SizeAndTagT kTagInputEnd = 3;
inline constexpr Blob::SizeAndTagT kTagStats = 4;
inline constexpr Blob::SizeAndTagT kTagMetadata = 5;

inline constexpr uint32_t kNoEdgeTarget = std::numeric_limits<uint32_t>::max();

void WriteU32Le(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xffu);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xffu);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xffu);
}

void WriteU64Le(uint8_t* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xffu);
  }
}

uint32_t ReadU32Le(const uint8_t* in) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<uint32_t>(in[i]) << (8 * i);
  }
  return value;
}

uint64_t ReadU64Le(const uint8_t* in) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(in[i]) << (8 * i);
  }
  return value;
}

// Mutable per-session state while processing one input.
struct SessionState {
  bool closed = false;
  uint32_t current_state = 0;
  uint64_t last_sequence = 0;
  uint64_t last_tick = 0;
  uint64_t deadline = 0;
  uint32_t step_count = 0;
};

// All parsed/processed data for one InputBegin..InputEnd tuple (or a
// trailing InputBegin without InputEnd). Lives entirely on the reader's
// stack/heap until the whole blob sequence validates, which gives the
// all-or-nothing commit boundary.
struct PendingInput {
  bool has_features = false;
  bool has_stats = false;
  bool has_metadata = false;
  FeatureVec ordinary_features;
  FeatureVec session_features;
  std::vector<SessionEvent> events;
  ExecutionResult::Stats stats;
  ByteArray metadata_bytes;
  std::vector<std::pair<uint64_t, SessionState>> sessions;
  size_t total_events = 0;

  SessionState* FindSession(uint64_t id) {
    for (auto& entry : sessions) {
      if (entry.first == id) return &entry.second;
    }
    return nullptr;
  }
};

// Processes one already shape-valid event against the per-input session
// table and the declared model. Appends raw events/features only through
// `input`. Returns false on any state-rule violation.
bool ProcessSessionEvent(const SessionEvent& event, uint32_t model_initial,
                         const std::vector<uint32_t>& edge_to,
                         PendingInput& input) {
  if (input.total_events >= kSessionMaxEventsPerInput) return false;
  ++input.total_events;

  switch (event.kind) {
    case SessionEventKind::Begin: {
      // Identity cannot reopen within the same input.
      if (input.FindSession(event.session) != nullptr) return false;
      if (input.sessions.size() >= kSessionMaxSessionsPerInput) return false;
      SessionState state;
      state.current_state = model_initial;
      state.last_tick = event.tick;
      state.deadline = event.deadline;
      input.sessions.emplace_back(event.session, state);
      // Begin intentionally contributes no feature.
      return true;
    }
    case SessionEventKind::Step: {
      SessionState* session = input.FindSession(event.session);
      if (session == nullptr || session->closed) return false;
      if (session->step_count >= kSessionMaxStepsPerSession) return false;
      // Sequence starts at 1 and is strictly consecutive.
      if (static_cast<uint64_t>(event.sequence) !=
          session->last_sequence + 1) {
        return false;
      }
      // Logical ticks never decrease.
      if (event.tick < session->last_tick) return false;
      // No step at/after the deadline.
      if (event.tick >= session->deadline) return false;
      const uint32_t target =
          edge_to[session->current_state * 256 + event.action];
      if (target == kNoEdgeTarget) return false;  // Must follow declared edge.
      const feature_t edge_feature =
          feature_domains::kUserDomains[14].ConvertToMe(
              session->current_state * 256 + event.action);
      input.session_features.push_back(edge_feature);
      session->current_state = target;
      session->last_sequence = event.sequence;
      session->last_tick = event.tick;
      ++session->step_count;
      return true;
    }
    case SessionEventKind::Finish:
    case SessionEventKind::Cancel:
    case SessionEventKind::Timeout: {
      SessionState* session = input.FindSession(event.session);
      if (session == nullptr || session->closed) return false;
      // Terminal sequence equals the last Step sequence (zero allowed).
      if (static_cast<uint64_t>(event.sequence) != session->last_sequence) {
        return false;
      }
      if (event.tick < session->last_tick) return false;
      if (event.kind == SessionEventKind::Finish ||
          event.kind == SessionEventKind::Cancel) {
        // Finish/Cancel must happen strictly before the deadline.
        if (event.tick >= session->deadline) return false;
      } else {
        // Timeout must happen at or after the deadline.
        if (event.tick < session->deadline) return false;
      }
      const uint32_t k =
          event.kind == SessionEventKind::Finish ? 0 :
          event.kind == SessionEventKind::Cancel ? 1 :
                                                   2;
      const feature_t terminal_feature =
          feature_domains::kUserDomains[15].ConvertToMe(
              session->current_state * 3 + k);
      input.session_features.push_back(terminal_feature);
      session->closed = true;
      session->last_tick = event.tick;
      return true;
    }
  }
  return false;
}

}  // namespace

bool ValidateSessionModel(const SessionModel& model) {
  if (model.initial > kSessionMaxStateValue) return false;
  if (model.edges.size() > kSessionModelMaxEdges) return false;
  bool seen[256][256] = {};
  for (const SessionEdge& edge : model.edges) {
    if (edge.from > kSessionMaxStateValue ||
        edge.action > kSessionMaxStateValue ||
        edge.to > kSessionMaxStateValue) {
      return false;
    }
    // Repeated (from, action) pairs are both duplicates and conflicts,
    // including identical repeats.
    if (seen[edge.from][edge.action]) return false;
    seen[edge.from][edge.action] = true;
  }
  return true;
}

bool SessionEventHasValidShape(const SessionEvent& event) {
  switch (event.kind) {
    case SessionEventKind::Begin:
      if (event.session == 0) return false;
      if (event.sequence != 0 || event.action != 0) return false;
      if (event.deadline <= event.tick) return false;
      return true;
    case SessionEventKind::Step:
      if (event.session == 0) return false;
      if (event.deadline != 0) return false;
      if (event.action > kSessionMaxStateValue) return false;
      return true;
    case SessionEventKind::Finish:
    case SessionEventKind::Cancel:
    case SessionEventKind::Timeout:
      if (event.session == 0) return false;
      if (event.deadline != 0 || event.action != 0) return false;
      return true;
  }
  return false;
}

void EncodeSessionEventPayload(const SessionEvent& event, uint8_t* out) {
  std::memset(out, 0, kSessionEventPayloadSize);
  out[0] = static_cast<uint8_t>(event.kind);
  // out[1..7] stay zero (reserved bytes).
  WriteU64Le(out + 8, event.session);
  WriteU64Le(out + 16, event.tick);
  WriteU64Le(out + 24, event.deadline);
  WriteU32Le(out + 32, event.sequence);
  WriteU32Le(out + 36, event.action);
}

bool DecodeSessionEventPayload(const uint8_t* data, size_t size,
                               SessionEvent* out) {
  if (size != kSessionEventPayloadSize || data == nullptr || out == nullptr) {
    return false;
  }
  const uint8_t kind_byte = data[0];
  if (kind_byte < static_cast<uint8_t>(SessionEventKind::Begin) ||
      kind_byte > static_cast<uint8_t>(SessionEventKind::Timeout)) {
    return false;
  }
  for (size_t i = 1; i < 8; ++i) {
    if (data[i] != 0) return false;  // Reserved bytes must be zero.
  }
  SessionEvent event;
  event.kind = static_cast<SessionEventKind>(kind_byte);
  event.session = ReadU64Le(data + 8);
  event.tick = ReadU64Le(data + 16);
  event.deadline = ReadU64Le(data + 24);
  event.sequence = ReadU32Le(data + 32);
  event.action = ReadU32Le(data + 36);
  if (!SessionEventHasValidShape(event)) return false;
  *out = event;
  return true;
}

bool BatchResult::WriteSessionEvent(const SessionEvent& event,
                                    BlobSequence& blobseq) {
  // Validate before touching the sequence: an illegal event must leave
  // offset and bytes unchanged.
  if (!SessionEventHasValidShape(event)) return false;
  uint8_t payload[kSessionEventPayloadSize];
  EncodeSessionEventPayload(event, payload);
  // BlobSequence::Write itself provides the capacity check with no partial
  // state mutation on failure.
  return blobseq.Write(
      Blob{kSessionEventTag, kSessionEventPayloadSize, payload});
}

bool BatchResult::ReadSessionBatch(BlobSequence& blobseq,
                                  const SessionModel& model) {
  // Corrupt framing left from the last Reset() can never be treated as a
  // legal half input.
  if (blobseq.read_error()) return false;
  if (!ValidateSessionModel(model)) return false;

  // Build the (state, action) -> target edge index. Validation guaranteed
  // uniqueness and bounds.
  std::vector<uint32_t> edge_to(256 * 256, kNoEdgeTarget);
  for (const SessionEdge& edge : model.edges) {
    edge_to[edge.from * 256 + edge.action] = edge.to;
  }

  const size_t num_slots = results_.size();
  std::vector<PendingInput> inputs(num_slots);
  size_t num_begins = 0;
  size_t num_ends = 0;
  constexpr size_t kNoCurrent = std::numeric_limits<size_t>::max();
  size_t current = kNoCurrent;

  while (true) {
    Blob blob = blobseq.Read();
    // A framing corruption dominates everything: whole read fails.
    if (blobseq.read_error()) return false;
    if (!blob.IsValid()) break;  // Normal terminator / exact boundary.

    if (blob.tag == kTagInputBegin) {
      // Input nesting is illegal.
      if (current != kNoCurrent) return false;
      // Begin occupies one of the ClearAndResize(N) slots; the legal
      // missing-End trailing input is included in this limit.
      if (num_begins >= num_slots) return false;
      current = num_begins;
      ++num_begins;
      continue;
    }

    if (blob.tag == kTagInputEnd) {
      if (current == kNoCurrent) return false;  // Extra End.
      PendingInput& input = inputs[current];
      // A complete input must have every begun session terminated.
      for (const auto& entry : input.sessions) {
        if (!entry.second.closed) return false;
      }
      ++num_ends;
      current = kNoCurrent;
      continue;
    }

    // Every other blob must belong to an open input ("no Begin data").
    if (current == kNoCurrent) return false;
    PendingInput& input = inputs[current];

    if (blob.tag == kTagFeatures) {
      if (input.has_features) return false;  // Duplicate Features.
      if (blob.size % sizeof(feature_t) != 0) return false;
      input.has_features = true;
      const size_t count = blob.size / sizeof(feature_t);
      input.ordinary_features.resize(count);
      if (count != 0) {
        std::memcpy(input.ordinary_features.data(), blob.data, blob.size);
      }
      // Domains 14 and 15 are reserved for this optional mode: ordinary
      // features landing there must be rejected, not mixed by origin.
      for (feature_t feature : input.ordinary_features) {
        if (feature_domains::kUserDomains[14].Contains(feature) ||
            feature_domains::kUserDomains[15].Contains(feature)) {
          return false;
        }
      }
      continue;
    }

    if (blob.tag == kTagStats) {
      if (input.has_stats) return false;  // Duplicate Stats.
      if (blob.size != sizeof(ExecutionResult::Stats)) return false;
      input.has_stats = true;
      std::memcpy(&input.stats, blob.data, blob.size);
      continue;
    }

    if (blob.tag == kTagMetadata) {
      if (input.has_metadata) return false;  // Duplicate Metadata.
      input.has_metadata = true;
      // Preserve bytes verbatim (same assignment ExecutionMetadata::Read
      // performs), without re-interpreting CMP contents.
      input.metadata_bytes.assign(blob.data, blob.data + blob.size);
      continue;
    }

    if (blob.tag == kSessionEventTag) {
      SessionEvent event;
      if (!DecodeSessionEventPayload(blob.data, blob.size, &event)) {
        return false;
      }
      if (!ProcessSessionEvent(event, model.initial, edge_to, input)) {
        return false;
      }
      input.events.push_back(event);
      continue;
    }

    // Unknown tag (including mutation-protocol tags 6..8): reject.
    return false;
  }

  // Stream ended normally. `current` may reference the single legal
  // Begin-without-End trailing input; it is never published.

  // ---- Commit point: only now mutate this BatchResult. ----
  for (size_t i = 0; i < num_slots; ++i) {
    ExecutionResult& result = results_[i];
    result.clear();  // Clears old features/stats/metadata/trajectory.
    if (i < num_ends) {
      PendingInput& input = inputs[i];
      FeatureVec merged;
      merged.reserve(input.ordinary_features.size() +
                     input.session_features.size());
      merged.insert(merged.end(), input.ordinary_features.begin(),
                    input.ordinary_features.end());
      merged.insert(merged.end(), input.session_features.begin(),
                    input.session_features.end());
      std::sort(merged.begin(), merged.end());
      merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
      result.mutable_features() = std::move(merged);
      result.mutable_session_events() = std::move(input.events);
      if (input.has_stats) result.stats() = input.stats;
      if (input.has_metadata) {
        result.metadata().cmp_data = std::move(input.metadata_bytes);
      }
    }
    // Trailing input slot (i == num_ends when it is Begin-open) and all
    // never-used slots stay fully cleared.
  }
  num_outputs_read_ = num_ends;
  return true;
}

}  // namespace fuzztest::internal
