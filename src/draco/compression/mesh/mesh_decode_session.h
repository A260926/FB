// Copyright 2026 The Draco Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#ifndef DRACO_COMPRESSION_MESH_MESH_DECODE_SESSION_H_
#define DRACO_COMPRESSION_MESH_MESH_DECODE_SESSION_H_

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// Required by the small POSIX ucontext fiber backend used in the .cc file.
#define _XOPEN_SOURCE 700
#elif defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include "draco/compression/config/decoder_options.h"
#include "draco/core/decoder_buffer.h"
#include "draco/core/status.h"

namespace draco {

class DecoderBuffer;
class EncoderBuffer;
class Mesh;

// Cooperative, budget-driven decoder for the restricted MeshDecodeSession
// profile.  A session owns a copy of the compressed input and all partially
// decoded production state.  It never starts a thread or background task:
// decoding advances only inside step(), cancel(), or reset()-driven cleanup.
//
// The supported profile is bitstream 2.2, triangular-mesh Edgebreaker with
// STANDARD traversal, one three-component FLOAT32 POSITION attribute, and an
// optional two-component FLOAT32 TEX_COORD attribute.  Quantized and direct
// floating-point attributes are supported, as are position
// difference/parallelogram prediction, UV difference/portable prediction,
// open boundaries, multiple components, and UV seams.  Streams with metadata,
// additional attributes, other versions, or other Edgebreaker traversals are
// rejected with an unsupported/error result.
class MeshDecodeSession {
 public:
  enum class State : int {
    kInitial = 0,
    initial = 0,
    idle = 0,
    kIdle = 0,
    ready = 0,
    kReady = 0,
    kWorking = 1,
    working = 1,
    busy = 1,
    kBusy = 1,
    paused = 1,
    kPaused = 1,
    kRunning = 1,
    running = 1,
    kDone = 2,
    done = 2,
    complete = 2,
    kComplete = 2,
    completed = 2,
    kCompleted = 2,
    kError = 3,
    error = 3,
    failed = 3,
    kFailed = 3,
    kCancelled = 4,
    cancelled = 4,
    canceled = 4,
    kCanceled = 4
  };

  enum class Phase : int {
    kInitial = 0,
    initial = 0,
    idle = 0,
    kIdle = 0,
    header = 1,
    kHeader = 1,
    topology_events = 2,
    kTopologyEvents = 2,
    topology_symbols = 3,
    kTopologySymbols = 3,
    topology_finalization = 4,
    kTopologyFinalization = 4,
    attribute_seams = 5,
    kAttributeSeams = 5,
    seam_reconstruction = 6,
    kSeamReconstruction = 6,
    point_mapping = 7,
    kPointMapping = 7,
    attribute_sequencing = 8,
    kAttributeSequencing = 8,
    attribute_descriptors = 9,
    kAttributeDescriptors = 9,
    entropy_tables = 10,
    kEntropyTables = 10,
    residual_decoding = 11,
    kResidualDecoding = 11,
    prediction_data = 12,
    kPredictionData = 12,
    prediction_reconstruction = 13,
    kPredictionReconstruction = 13,
    inverse_transform = 14,
    kInverseTransform = 14,
    store_values = 15,
    kStoreValues = 15,
    done = 16,
    kDone = 16,
    error = 17,
    kError = 17,
    cancelled = 18,
    kCancelled = 18
  };

  // Exact cumulative counters.  All values are reset by a successful begin().
  // total_work is also the sum charged against step budgets.  Additional
  // counters make the "other element-sized traversal/initialization/copy"
  // charges auditable.
  struct Progress {
    State state = State::kInitial;
    Phase phase = Phase::kInitial;

    std::uint64_t total_work = 0;
    std::uint64_t total_work_units = 0;
    std::uint64_t work_total = 0;
    std::uint64_t work = 0;

    std::uint64_t topology_symbols = 0;
    std::uint64_t num_topology_symbols = 0;
    std::uint64_t symbols = 0;
    std::uint64_t start_faces = 0;
    std::uint64_t num_start_faces = 0;
    std::uint64_t start_face_flags = 0;
    std::uint64_t split_events = 0;
    std::uint64_t num_split_events = 0;
    std::uint64_t topology_split_events = 0;
    std::uint64_t hole_events = 0;
    std::uint64_t seam_bits = 0;
    std::uint64_t num_seam_bits = 0;
    std::uint64_t attribute_seam_bits = 0;

    std::uint64_t corners_processed = 0;
    std::uint64_t traversed_corners = 0;
    std::uint64_t corners_visited = 0;
    std::uint64_t num_corners_visited = 0;
    std::uint64_t corners_traversed = 0;
    std::uint64_t num_corners_traversed = 0;
    std::uint64_t corners = 0;
    std::uint64_t vertices_processed = 0;
    std::uint64_t vertices = 0;
    std::uint64_t mapping_items = 0;
    std::uint64_t point_mapping_items = 0;

    std::uint64_t residual_components = 0;
    std::uint64_t num_residual_components = 0;
    std::uint64_t entropy_residual_components = 0;
    std::uint64_t tags = 0;
    std::uint64_t tagged_tags = 0;
    std::uint64_t direction_bits = 0;
    std::uint64_t prediction_direction_bits = 0;

    std::uint64_t predicted_values = 0;
    std::uint64_t num_predicted_values = 0;
    std::uint64_t predictions = 0;
    std::uint64_t inverse_transformed_values = 0;
    std::uint64_t num_inverse_transformed_values = 0;
    std::uint64_t inverse_values = 0;
    std::uint64_t dequantized_values = 0;

    std::uint64_t rebuilt_value_vectors = 0;
    std::uint64_t value_vectors_rebuilt = 0;
    std::uint64_t inverse_transformed_vectors = 0;
    std::uint64_t value_vectors_inverse_transformed = 0;
    std::uint64_t float_attribute_writes = 0;
    std::uint64_t float_writes = 0;
    std::uint64_t probability_table_entries = 0;
    std::uint64_t table_entries = 0;

    // Element-sized setup/copy/traversal work that does not fit a more
    // specific counter.  Each item is still charged once per element.
    std::uint64_t element_operations = 0;
    std::uint64_t other_elements = 0;

    std::string error_message;
  };

  struct Options {
    // Match Decoder::SetSkipAttributeTransform(POSITION / TEX_COORD).
    bool skip_position_transform = false;
    bool skip_tex_coord_transform = false;
    // Cooperative fiber stack. Exposed mainly for unusually deep valid meshes.
    std::size_t fiber_stack_size = 8u * 1024u * 1024u;
  };

  class BeginResult : public Status {
   public:
    enum Code : int {
      kOk = 0,
      kBusy = 1,
      busy = 1,
      kInvalidHeader = 2,
      invalid_header = 2,
      kUnsupported = 3,
      unsupported = 3,
      kInvalidArgument = 4,
      invalid_argument = 4,
      kSessionError = 5
    };

    // Session-specific outcome.  The base draco::Status ok()/code() interface
    // remains available for callers and macros that expect a Status.
    Code outcome = kSessionError;
    State state = State::kInitial;

    BeginResult() : Status(Status::DRACO_ERROR) {}
    BeginResult(Code outcome_code, State result_state, std::string message)
        : Status(outcome_code == kOk ? Status::OK : Status::DRACO_ERROR,
                 message),
          outcome(outcome_code),
          state(result_state) {}

    bool ok() const { return outcome == kOk && Status::ok(); }
    explicit operator bool() const { return ok(); }
    bool is_busy() const { return outcome == kBusy; }
    bool is_unsupported() const { return outcome == kUnsupported; }
  };

  struct StepResult {
    State state = State::kInitial;
    State status = State::kInitial;
    Phase phase = Phase::kInitial;
    std::uint64_t budget = 0;

    // All names refer to the same returned quantum.  Exactly one unit is
    // returned per charged discrete operation.
    std::uint64_t work_done = 0;
    std::uint64_t work_units_done = 0;
    std::uint64_t actual_work = 0;
    std::uint64_t work = 0;
    std::uint64_t work_units = 0;
    std::uint64_t units = 0;
    std::uint64_t consumed = 0;
    std::uint64_t budget_used = 0;

    bool ok = true;
    std::string error_message;

    bool is_working() const { return state == State::kWorking; }
    bool working() const { return is_working(); }
    bool done() const { return state == State::kDone; }
    bool is_done() const { return done(); }
    bool has_error() const { return state == State::kError; }
    bool error() const { return has_error(); }
    bool is_cancelled() const { return state == State::kCancelled; }

    // Allows `EXPECT_EQ(session.step(1), 1)` style use when only the amount
    // is relevant.  Named fields and methods above remain the preferred API.
    operator std::uint64_t() const { return work_done; }
  };

  MeshDecodeSession();
  explicit MeshDecodeSession(const Options& options);
  ~MeshDecodeSession();

  MeshDecodeSession(const MeshDecodeSession&) = delete;
  MeshDecodeSession& operator=(const MeshDecodeSession&) = delete;

  // Validates only the fixed Draco header and retains an owned copy of
  // [data, data+size).  The caller's buffer may be released immediately after
  // begin returns success.  A busy session is untouched.  An invalid header
  // likewise leaves any previous terminal state/result untouched.
  BeginResult begin(const void* data, std::size_t size);
  BeginResult begin(const void* data, std::size_t size,
                    const Options& options);
  BeginResult begin(const DecoderBuffer& buffer);
  BeginResult begin(const DecoderBuffer* buffer);
  BeginResult begin(const EncoderBuffer& buffer);
  BeginResult begin(const EncoderBuffer* buffer);

  // Convenience overloads for owned byte strings/containers.  The session
  // copies the bytes during begin(), exactly as for the pointer/size form.
  BeginResult begin(const std::string& bytes) {
    return begin(bytes.data(), bytes.size());
  }
  template <class Container,
            typename std::enable_if<std::is_class<Container>::value,
                                    int>::type = 0>
  BeginResult begin(const Container& bytes) {
    return begin(bytes.data(), bytes.size());
  }

  // Advances at most |budget| charged operations.  budget == 0 never resumes
  // decoding.  Stepping a done/error/cancelled session returns zero work.
  StepResult step(std::uint64_t budget);

  // Integral convenience overload.  Negative values are rejected without
  // changing session state; the task contract uses non-negative budgets.
  template <typename Integer,
            typename std::enable_if<std::is_integral<Integer>::value,
                                    int>::type = 0>
  StepResult step(Integer budget) {
    if (std::is_signed<Integer>::value && budget < 0) {
      StepResult result;
      result.state = state();
      result.status = result.state;
      result.phase = progress().phase;
      result.budget = 0;
      result.ok = false;
      result.error_message = "budget must be non-negative";
      return result;
    }
    return step(static_cast<std::uint64_t>(budget));
  }

  const Progress& progress() const;
  const Progress& GetProgress() const { return progress(); }
  State state() const { return progress().state; }
  State status() const { return state(); }
  bool is_working() const { return state() == State::kWorking; }
  bool is_done() const { return state() == State::kDone; }
  bool has_error() const { return state() == State::kError; }
  bool is_cancelled() const { return state() == State::kCancelled; }
  std::string error() const { return progress().error_message; }
  std::string error_message() const { return error(); }

  // Non-null only in State::kDone and valid until the next successful begin,
  // cancel, reset, or release_result().
  const Mesh* result() const;
  const Mesh* get() const { return result(); }
  const Mesh* mesh() const { return result(); }
  const Mesh* GetMesh() const { return result(); }

  // Transfers ownership of the completed mesh.  This is used by Draco's
  // synchronous Decoder adapter and is not needed by ordinary session callers.
  std::unique_ptr<Mesh> release_result();

  void cancel();
  void Cancel() { cancel(); }
  void reset();
  void Reset() { reset(); }

  // GeometryAttribute::Type is accepted as int to keep this lightweight
  // header independent of Draco's attribute enum definition.
  void SetSkipAttributeTransform(int attribute_type);
  void set_skip_attribute_transform(int attribute_type) {
    SetSkipAttributeTransform(attribute_type);
  }

  // Pascal-case aliases for callers/tests following Draco style.
  BeginResult Begin(const void* data, std::size_t size) {
    return begin(data, size);
  }
  template <class BufferOrContainer>
  BeginResult Begin(const BufferOrContainer& value) {
    return begin(value);
  }
  StepResult Step(std::uint64_t budget) { return step(budget); }
  const Mesh* GetResult() const { return result(); }

  // Internal bridge used by Decoder to propagate all DecoderOptions.
  void CopyDecoderOptions(const DecoderOptions& decoder_options);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  Options default_options_;
  Progress progress_;
};

}  // namespace draco

#endif  // DRACO_COMPRESSION_MESH_MESH_DECODE_SESSION_H_
