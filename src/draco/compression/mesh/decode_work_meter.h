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
#ifndef DRACO_COMPRESSION_MESH_DECODE_WORK_METER_H_
#define DRACO_COMPRESSION_MESH_DECODE_WORK_METER_H_

#include <cstdint>

#include "draco/compression/mesh/mesh_decode_session.h"

namespace draco {

// Fine-grained work categories.  Production decoder code reports one call per
// processed element; MeshDecodeSession turns those reports into bounded steps.
enum class WorkUnit {
  kTopologySymbol,
  kStartFace,
  kSplitEvent,
  kHoleEvent,
  kSeamBit,
  kCorner,
  kVertex,
  kMappingItem,
  kResidualComponent,
  kTag,
  kDirectionBit,
  kPredictedValue,
  kInverseValue,
  kRebuiltVector,
  kInverseVector,
  kFloatWrite,
  kTableEntry,
  kElementOperation
};

class SessionCanceledException {};

class DecodeWorkMeter {
 public:
  // Active meter on the current cooperative fiber.  There is at most one
  // running decoder on a thread; paused sessions keep their own meter.
  static thread_local DecodeWorkMeter* active;

  std::uint64_t step_budget = 0;
  std::uint64_t step_work = 0;
  bool cancel_requested = false;
  bool unsupported_requested = false;
  MeshDecodeSession::Progress progress;

  // Set by MeshDecodeSession::Impl to context-switch back to step().
  void (*yield_callback)(void* user_data) = nullptr;
  void* yield_user_data = nullptr;

  void ResetWork() {
    step_budget = 0;
    step_work = 0;
    cancel_requested = false;
    unsupported_requested = false;
    progress = MeshDecodeSession::Progress();
  }

  void SetPhase(MeshDecodeSession::Phase phase) { progress.phase = phase; }

  void Charge(WorkUnit unit, std::uint64_t count = 1) {
    if (cancel_requested) {
      throw SessionCanceledException();
    }
    if (unit == WorkUnit::kInverseVector) {
      progress.inverse_transformed_values += count;
      progress.inverse_transformed_vectors += count;
    } else if (unit == WorkUnit::kRebuiltVector) {
      progress.rebuilt_value_vectors += count;
    }
    std::uint64_t* specific = CounterFor(unit);
    if (specific) *specific += count;
    progress.total_work += count;
    step_work += count;

    if (step_work >= step_budget && yield_callback != nullptr) {
      yield_callback(yield_user_data);
      // A resume after cancel request is turned into stack unwinding here,
      // before another decoder operation can run.
      if (cancel_requested) {
        throw SessionCanceledException();
      }
    }
  }

  void RequestUnsupported() { unsupported_requested = true; }

  void SyncAliases() {
    progress.total_work_units = progress.total_work;
    progress.work_total = progress.total_work;
    progress.work = progress.total_work;
    progress.num_topology_symbols = progress.topology_symbols;
    progress.symbols = progress.topology_symbols;
    progress.num_start_faces = progress.start_faces;
    progress.start_face_flags = progress.start_faces;
    progress.num_split_events = progress.split_events;
    progress.topology_split_events = progress.split_events;
    progress.num_seam_bits = progress.seam_bits;
    progress.attribute_seam_bits = progress.seam_bits;
    progress.traversed_corners = progress.corners_processed;
    progress.corners_visited = progress.corners_processed;
    progress.num_corners_visited = progress.corners_processed;
    progress.corners_traversed = progress.corners_processed;
    progress.num_corners_traversed = progress.corners_processed;
    progress.corners = progress.corners_processed;
    progress.vertices = progress.vertices_processed;
    progress.point_mapping_items = progress.mapping_items;
    progress.num_residual_components = progress.residual_components;
    progress.entropy_residual_components = progress.residual_components;
    progress.tagged_tags = progress.tags;
    progress.prediction_direction_bits = progress.direction_bits;
    progress.num_predicted_values = progress.predicted_values;
    progress.predictions = progress.predicted_values;
    progress.num_inverse_transformed_values =
        progress.inverse_transformed_values;
    progress.inverse_values = progress.inverse_transformed_values;
    progress.dequantized_values = progress.inverse_transformed_values;
    progress.value_vectors_rebuilt = progress.rebuilt_value_vectors;
    progress.inverse_transformed_vectors =
        progress.inverse_transformed_vectors;  // alias same canonical field
    progress.value_vectors_inverse_transformed =
        progress.inverse_transformed_vectors;
    progress.float_writes = progress.float_attribute_writes;
    progress.table_entries = progress.probability_table_entries;
    progress.other_elements = progress.element_operations;
  }

 private:
  std::uint64_t* CounterFor(WorkUnit unit) {
    switch (unit) {
      case WorkUnit::kTopologySymbol:
        return &progress.topology_symbols;
      case WorkUnit::kStartFace:
        return &progress.start_faces;
      case WorkUnit::kSplitEvent:
        return &progress.split_events;
      case WorkUnit::kHoleEvent:
        return &progress.hole_events;
      case WorkUnit::kSeamBit:
        return &progress.seam_bits;
      case WorkUnit::kCorner:
        return &progress.corners_processed;
      case WorkUnit::kVertex:
        return &progress.vertices_processed;
      case WorkUnit::kMappingItem:
        return &progress.mapping_items;
      case WorkUnit::kResidualComponent:
        return &progress.residual_components;
      case WorkUnit::kTag:
        return &progress.tags;
      case WorkUnit::kDirectionBit:
        return &progress.direction_bits;
      case WorkUnit::kPredictedValue:
        return &progress.predicted_values;
      case WorkUnit::kInverseValue:
        return &progress.inverse_transformed_values;
      case WorkUnit::kRebuiltVector:
      case WorkUnit::kInverseVector:
        // Handled in Charge so both public value and vector counters move.
        return nullptr;
      case WorkUnit::kFloatWrite:
        return &progress.float_attribute_writes;
      case WorkUnit::kTableEntry:
        return &progress.probability_table_entries;
      case WorkUnit::kElementOperation:
        return &progress.element_operations;
    }
    return nullptr;
  }
};

inline thread_local DecodeWorkMeter* DecodeWorkMeter::active = nullptr;

inline void SessionCharge(WorkUnit unit, std::uint64_t count = 1) {
  if (DecodeWorkMeter::active != nullptr) {
    DecodeWorkMeter::active->Charge(unit, count);
  }
}

inline void SessionSetPhase(MeshDecodeSession::Phase phase) {
  if (DecodeWorkMeter::active != nullptr) {
    DecodeWorkMeter::active->SetPhase(phase);
  }
}

inline bool SessionUnsupportedRequested() {
  return DecodeWorkMeter::active != nullptr &&
         DecodeWorkMeter::active->unsupported_requested;
}

}  // namespace draco

#define DRACO_SESSION_CHARGE(unit) ::draco::SessionCharge(unit)
#define DRACO_SESSION_CHARGE_N(unit, count) \
  ::draco::SessionCharge(unit, static_cast<std::uint64_t>(count))
#define DRACO_SESSION_PHASE(phase) ::draco::SessionSetPhase(phase)

#endif  // DRACO_COMPRESSION_MESH_DECODE_WORK_METER_H_
