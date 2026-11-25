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

// Optional model-session feedback channel for local protocol regressions.
//
// This header defines the public session data types, the stable on-the-wire
// event format (fixed 40-byte little-endian payload under blob tag 9) and
// small shared validation/codec helpers used by both the production writer
// (`BatchResult::WriteSessionEvent`) and the production reader
// (`BatchResult::ReadSessionBatch`).
//
// See task_support/session_contract.md for the full behavioral contract.

#ifndef THIRD_PARTY_CENTIPEDE_SESSION_FEEDBACK_H_
#define THIRD_PARTY_CENTIPEDE_SESSION_FEEDBACK_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fuzztest::internal {

class BlobSequence;

// Blob tag reserved for model session events. Tags 0..8 are already taken by
// the execution/mutation result protocol, so this must stay 9.
inline constexpr uint32_t kSessionEventTag = 9;

// Every session event payload is exactly 40 bytes.
inline constexpr size_t kSessionEventPayloadSize = 40;

// Contract limits.
inline constexpr size_t kSessionModelMaxEdges = 4096;
inline constexpr size_t kSessionMaxSessionsPerInput = 64;
inline constexpr size_t kSessionMaxStepsPerSession = 4096;
inline constexpr size_t kSessionMaxEventsPerInput = 65536;
inline constexpr uint32_t kSessionMaxStateValue = 255;

enum class SessionEventKind : uint8_t {
  Begin = 1,
  Step = 2,
  Finish = 3,
  Cancel = 4,
  Timeout = 5,
};

// One declared model edge: from-state + action -> to-state.
struct SessionEdge {
  uint32_t from = 0;
  uint32_t action = 0;
  uint32_t to = 0;
};

// Declarative session model. The initial state and every from/to state must
// fit 0..255. Empty edge sets are legal.
struct SessionModel {
  uint32_t initial = 0;
  std::vector<SessionEdge> edges;
};

// One raw session event. Aggregate initialization field order is part of the
// public contract: {kind, session, tick, deadline, sequence, action}.
struct SessionEvent {
  SessionEventKind kind = SessionEventKind::Begin;
  uint64_t session = 0;
  uint64_t tick = 0;
  uint64_t deadline = 0;
  uint32_t sequence = 0;
  uint32_t action = 0;

  // Field-by-field equality (including the event kind).
  bool operator==(const SessionEvent& other) const {
    return kind == other.kind && session == other.session &&
           tick == other.tick && deadline == other.deadline &&
           sequence == other.sequence && action == other.action;
  }

  bool operator!=(const SessionEvent& other) const { return !(*this == other); }
};

// Validates the static model contract: initial/from/to/action in 0..255, at
// most kSessionModelMaxEdges edges and no repeated (from, action) pair (even
// an identical repeat is rejected). An empty edge vector is valid.
bool ValidateSessionModel(const SessionModel& model);

// Validates only the context-independent shape of one event, as checked by
// WriteSessionEvent: known kind, non-zero session, the per-kind zero-field
// requirements, Begin's deadline > tick and the 0..255 action bound.
// Does not maintain model/session state and cannot check sequence continuity,
// tick monotonicity or termination deadlines.
bool SessionEventHasValidShape(const SessionEvent& event);

// Encodes `event` into the fixed 40-byte little-endian wire payload.
// `out` must point at at least kSessionEventPayloadSize bytes.
// Always emits zero reserved bytes; never relies on struct padding/layout.
void EncodeSessionEventPayload(const SessionEvent& event, uint8_t* out);

// Decodes a 40-byte payload produced by EncodeSessionEventPayload (or any
// conforming little-endian producer). Checks the exact size, the zero
// reserved bytes and the single-event shape. Returns false on any violation
// and leaves `*out` in an unspecified but valid state.
bool DecodeSessionEventPayload(const uint8_t* data, size_t size,
                              SessionEvent* out);

}  // namespace fuzztest::internal

#endif  // THIRD_PARTY_CENTIPEDE_SESSION_FEEDBACK_H_
