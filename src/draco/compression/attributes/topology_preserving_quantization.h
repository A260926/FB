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
// Optional mesh position quantization protection.
//
// The regular quantization path is left completely untouched when the feature
// is disabled. When enabled on a mesh encoder, the encoder first computes the
// positions that the regular dequantizer would reconstruct and detects faces
// whose reconstructed normal no longer points in the same half space as the
// original normal (dot product <= 0). Every position value used by such a face
// is then restored exactly (the original FLOAT32 bits are stored in the
// stream). Restoring a value can flip a neighboring face, so the set of exact
// values is grown to the least fixed point of this rule using position -> face
// reverse adjacency and a work queue. Restored values are the *only* values
// that are stored exactly; all other values keep the ordinary quantized
// reconstruction.
//
// Input requirements (violations are reported as errors, never silently
// downgraded to the old mode):
//   - triangular mesh with exactly one POSITION attribute,
//   - POSITION is DT_FLOAT32 with three components,
//   - position quantization is in [1, 30],
//   - all position coordinates are finite,
//   - every face has positive area (no duplicate position indices),
//   - the triangle mesh is oriented 2-manifold. Open boundaries, multiple
//     connected components and points split by UV/normal seams are allowed.
//     Shared position identity is defined by POSITION::mapped_index(); two
//     equal coordinates with different attribute value indices are distinct.
#ifndef DRACO_COMPRESSION_ATTRIBUTES_TOPOLOGY_PRESERVING_QUANTIZATION_H_
#define DRACO_COMPRESSION_ATTRIBUTES_TOPOLOGY_PRESERVING_QUANTIZATION_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "draco/compression/config/encoder_options.h"
#include "draco/core/status.h"

namespace draco {

class Mesh;

// Public diagnostics for the optional topology preserving quantization mode.
// All counters refer to the position attribute of the encoded mesh.
struct MeshTopologyPreservingQuantizationStats {
  // Number of unique POSITION attribute values in the input (this includes
  // isolated, not-face-referenced values and values shared by several points).
  size_t unique_position_value_count = 0;
  // Number of unique POSITION attribute values that were restored exactly.
  size_t exact_position_value_count = 0;
  // Number of face danger evaluations performed by the incremental closure
  // after the initial full scan (each face is evaluated at most once per
  // propagation round).
  size_t face_recheck_count = 0;
  // Number of fixed-point rounds that produced at least one new exact mark.
  // The initial full-mesh detection counts as a round when it marks anything;
  // every subsequent round is an incremental recheck of faces affected by the
  // previous round's marks. Zero means no position value had to be restored.
  size_t closure_round_count = 0;

  // True when every unique position value had to be restored. This is the full
  // lossless position fallback, as opposed to a localized exception set.
  bool full_lossless_position_fallback() const {
    return unique_position_value_count > 0 &&
           exact_position_value_count == unique_position_value_count;
  }
  // True when restoration propagated past the initial detection, i.e. at least
  // two closure rounds were needed. False when the initial detection alone
  // determined the whole exception set, and false when nothing was restored.
  bool used_incremental_closure() const { return closure_round_count >= 2; }

  // Convenience accessors mirroring the stored counters.
  size_t num_unique_position_values() const {
    return unique_position_value_count;
  }
  size_t num_exact_position_values() const {
    return exact_position_value_count;
  }
  size_t num_face_rechecks() const { return face_recheck_count; }
  size_t num_closure_rounds() const { return closure_round_count; }
};

// The result of the validation + fixed point computation. |exact_value| is
// indexed by the input POSITION AttributeValueIndex and is 1 for values whose
// original FLOAT32 bits must be stored in the stream. The remaining fields are
// the exact parameters of the ordinary quantization transform (so the encoder
// does not recompute them and cannot drift) and the lattice/reconstruction
// produced by it.
struct TopologyPreservingQuantizationPlan {
  std::vector<uint8_t> exact_value;
  std::vector<int32_t> quantized_values;    // 3 components per unique value.
  std::vector<float> reconstructed_values;  // 3 components per unique value.
  int32_t position_attribute_id = -1;
  int quantization_bits = 0;
  float min_value[3] = {0.f, 0.f, 0.f};
  float quantization_range = 0.f;
  MeshTopologyPreservingQuantizationStats stats;
};

// Validates |mesh| for the topology preserving quantization mode, computes the
// least fixed point of "a dangerous face restores all of its position values",
// and returns an immutable plan. On invalid input an error status describing
// the problem is returned and |out_plan| is reset.
Status BuildTopologyPreservingQuantizationPlan(
    const Mesh& mesh, const EncoderOptions& options,
    std::shared_ptr<const TopologyPreservingQuantizationPlan>* out_plan);

}  // namespace draco

#endif  // DRACO_COMPRESSION_ATTRIBUTES_TOPOLOGY_PRESERVING_QUANTIZATION_H_
