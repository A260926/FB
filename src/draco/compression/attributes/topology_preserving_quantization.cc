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
// Implementation notes for the optional topology preserving position
// quantization mode.
//
// The plan is computed from the *input* mesh and from the quantization options
// that the ordinary sequential quantization encoder is going to use. The
// integer lattice and the reconstructed float coordinates are produced by
// exactly the same arithmetic as Quantizer/Dequantizer (see
// core/quantization_utils.h). Nothing is written to the stream here: the
// resulting "exact value" mask is consumed by the position attribute encoder,
// which stores the original FLOAT32 bits of the masked values in a dedicated,
// self-describing extension of the quantization transform data.
//
// Fixed point rule:
//   A face is unsafe when dot(normal(original), normal(reconstructed)) <= 0.
//   Every unsafe face forces all three of its position attribute values to be
//   restored exactly. Restoring a value can make a neighboring face unsafe, so
//   the mask is grown to the least fixed point of this monotone rule. Marking
//   is only ever added, never removed, and values that are never marked keep
//   the ordinary quantized reconstruction.
//
// The fixed point is computed incrementally: position value -> incident face
// reverse adjacency plus a work queue of affected faces. A full face scan is
// performed exactly once (the initial detection); all later work only visits
// faces incident to values that were newly marked, and each affected face is
// rechecked at most once per propagation round.

#include "draco/compression/attributes/topology_preserving_quantization.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "draco/attributes/point_attribute.h"
#include "draco/compression/config/compression_shared.h"
#include "draco/core/quantization_utils.h"
#include "draco/mesh/mesh.h"

namespace draco {
namespace {

// Returns a key for an ordered (a, b) vertex index pair.
uint64_t DirectedEdgeKey(uint32_t a, uint32_t b) {
  return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
}

class DisjointSet {
 public:
  explicit DisjointSet(size_t n) : parent_(n), rank_(n, 0) {
    for (size_t i = 0; i < n; ++i) {
      parent_[i] = static_cast<uint32_t>(i);
    }
  }
  uint32_t Find(uint32_t x) {
    while (parent_[x] != x) {
      parent_[x] = parent_[parent_[x]];
      x = parent_[x];
    }
    return x;
  }
  void Union(uint32_t a, uint32_t b) {
    a = Find(a);
    b = Find(b);
    if (a == b) {
      return;
    }
    if (rank_[a] < rank_[b]) {
      std::swap(a, b);
    }
    parent_[b] = a;
    if (rank_[a] == rank_[b]) {
      ++rank_[a];
    }
  }

 private:
  std::vector<uint32_t> parent_;
  std::vector<uint8_t> rank_;
};

bool IsFiniteFloat(float v) { return std::isfinite(v); }

Status BuildQuantizationParameters(const PointAttribute& position_attribute,
                                   int32_t attribute_id,
                                   const EncoderOptions& options,
                                   int* out_bits, float out_min[3],
                                   float* out_range) {
  const int bits = options.GetAttributeInt(attribute_id, "quantization_bits",
                                           -1);
  // The protection is defined only for the regular position quantization, so
  // the requested number of bits must be the one the regular quantizer accepts.
  if (bits < 1 || bits > 30) {
    return Status(Status::INVALID_PARAMETER,
                  "Topology preserving quantization requires POSITION "
                  "quantization bits in [1, 30].");
  }
  const bool has_origin =
      options.IsAttributeOptionSet(attribute_id, "quantization_origin");
  const bool has_range =
      options.IsAttributeOptionSet(attribute_id, "quantization_range");
  if (has_origin && has_range) {
    std::vector<float> origin(position_attribute.num_components(), 0.f);
    options.GetAttributeVector(attribute_id, "quantization_origin",
                               position_attribute.num_components(),
                               origin.data());
    const float range = options.GetAttributeFloat(attribute_id,
                                                  "quantization_range", 1.f);
    for (int c = 0; c < position_attribute.num_components(); ++c) {
      if (!IsFiniteFloat(origin[c])) {
        return Status(Status::INVALID_PARAMETER,
                      "Non-finite POSITION quantization origin.");
      }
      out_min[c] = origin[c];
    }
    if (!IsFiniteFloat(range) || range <= 0.f) {
      return Status(Status::INVALID_PARAMETER,
                    "Invalid POSITION quantization range.");
    }
    *out_range = range;
    *out_bits = bits;
    return OkStatus();
  }
  // If only one of origin/range is set, the ordinary encoder ignores both and
  // derives the box from the values (see
  // SequentialQuantizationAttributeEncoder::Init). Mirror that so the plan and
  // the stream can never disagree.

  // Mirror AttributeQuantizationTransform::ComputeParameters() exactly.
  const int num_components = position_attribute.num_components();
  float min_values[3] = {0.f, 0.f, 0.f};
  float max_values[3] = {0.f, 0.f, 0.f};
  // Use ConvertValue() rather than GetValue(..., void*) so an interleaved
  // attribute (byte_stride > num_components*sizeof(float)) cannot overflow the
  // three-component buffer. The src attribute is FLOAT32, so no precision is
  // lost and this matches ComputeParameters() component by component.
  position_attribute.ConvertValue<float>(AttributeValueIndex(0), 3,
                                         min_values);
  position_attribute.ConvertValue<float>(AttributeValueIndex(0), 3,
                                         max_values);
  float values[3] = {0.f, 0.f, 0.f};
  float range = 0.f;
  for (AttributeValueIndex i(1); i < position_attribute.size(); ++i) {
    position_attribute.ConvertValue<float>(i, 3, values);
    for (int c = 0; c < num_components; ++c) {
      if (std::isnan(values[c])) {
        return Status(Status::INVALID_PARAMETER,
                      "Non-finite POSITION coordinate.");
      }
      min_values[c] = std::min(min_values[c], values[c]);
      max_values[c] = std::max(max_values[c], values[c]);
    }
  }
  for (int c = 0; c < num_components; ++c) {
    if (!IsFiniteFloat(min_values[c]) || !IsFiniteFloat(max_values[c])) {
      return Status(Status::INVALID_PARAMETER,
                    "Non-finite POSITION coordinate.");
    }
    range = std::max(range, max_values[c] - min_values[c]);
  }
  if (!IsFiniteFloat(range)) {
    return Status(Status::INVALID_PARAMETER,
                  "POSITION quantization range is not finite.");
  }
  if (range == 0.f) {
    range = 1.f;
  }
  for (int c = 0; c < num_components; ++c) {
    out_min[c] = min_values[c];
  }
  *out_range = range;
  *out_bits = bits;
  return OkStatus();
}

}  // namespace

Status BuildTopologyPreservingQuantizationPlan(
    const Mesh& mesh, const EncoderOptions& options,
    std::shared_ptr<const TopologyPreservingQuantizationPlan>* out_plan) {
  if (out_plan == nullptr) {
    return Status(Status::INVALID_PARAMETER, "Invalid output plan.");
  }
  out_plan->reset();

  // Exactly one POSITION attribute with the layout required by this mode.
  if (mesh.num_attributes() <= 0) {
    return Status(Status::INVALID_PARAMETER, "Mesh has no attributes.");
  }
  int32_t position_attribute_id = -1;
  int32_t position_attribute_count = 0;
  for (int32_t i = 0; i < mesh.num_attributes(); ++i) {
    if (mesh.attribute(i)->attribute_type() == GeometryAttribute::POSITION) {
      position_attribute_id = i;
      ++position_attribute_count;
    }
  }
  if (position_attribute_count != 1) {
    return Status(Status::INVALID_PARAMETER,
                  "Topology preserving quantization requires exactly one "
                  "POSITION attribute.");
  }
  const PointAttribute& position_attribute =
      *mesh.attribute(position_attribute_id);
  if (position_attribute.data_type() != DT_FLOAT32) {
    return Status(Status::INVALID_PARAMETER,
                  "Topology preserving quantization requires a FLOAT32 "
                  "POSITION attribute.");
  }
  if (position_attribute.num_components() != 3) {
    return Status(Status::INVALID_PARAMETER,
                  "Topology preserving quantization requires a three "
                  "component POSITION attribute.");
  }
  if (mesh.num_faces() == 0) {
    return Status(Status::INVALID_PARAMETER,
                  "Topology preserving quantization requires a non-empty "
                  "triangle mesh.");
  }

  int quantization_bits = 0;
  float min_values[3] = {0.f, 0.f, 0.f};
  float range = 0.f;
  DRACO_RETURN_IF_ERROR(BuildQuantizationParameters(
      position_attribute, position_attribute_id, options, &quantization_bits,
      min_values, &range));

  const size_t num_values = position_attribute.size();
  if (num_values == 0) {
    return Status(Status::INVALID_PARAMETER, "Empty POSITION attribute.");
  }

  // Read and validate all position values. All values must be finite, not just
  // the values referenced by faces: unused values are carried in the stream.
  std::vector<float> input_values(num_values * 3);
  for (AttributeValueIndex i(0); i < num_values; ++i) {
    position_attribute.ConvertValue<float>(i, 3,
                                           &input_values[i.value() * 3]);
  }
  for (size_t i = 0; i < input_values.size(); ++i) {
    if (!IsFiniteFloat(input_values[i])) {
      return Status(Status::INVALID_PARAMETER,
                    "Non-finite POSITION coordinate.");
    }
  }

  // Quantize on the same lattice as the production quantizer and immediately
  // dequantize with the production inverse transform.
  const uint32_t max_quantized_value =
      (1u << static_cast<uint32_t>(quantization_bits)) - 1;
  Quantizer quantizer;
  quantizer.Init(range, static_cast<int32_t>(max_quantized_value));
  Dequantizer dequantizer;
  if (!dequantizer.Init(range, static_cast<int32_t>(max_quantized_value))) {
    return Status(Status::INVALID_PARAMETER, "Invalid quantization setup.");
  }
  std::vector<int32_t> quantized(num_values * 3);
  std::vector<float> reconstructed(num_values * 3);
  for (size_t i = 0; i < num_values; ++i) {
    for (int c = 0; c < 3; ++c) {
      const float centered = input_values[i * 3 + c] - min_values[c];
      const int32_t q = quantizer.QuantizeFloat(centered);
      quantized[i * 3 + c] = q;
      float value = dequantizer.DequantizeFloat(q);
      value = value + min_values[c];
      reconstructed[i * 3 + c] = value;
    }
  }

  // Map faces to POSITION attribute value indices. The shared position
  // identity of this mode is exactly POSITION::mapped_index(); points that map
  // to different values are never welded, even if their coordinates match.
  const uint32_t num_faces = mesh.num_faces();
  const size_t num_corners = static_cast<size_t>(num_faces) * 3;
  std::vector<std::array<uint32_t, 3>> face_values(num_faces);
  std::vector<double> original_normals(num_faces * 3);
  std::unordered_map<uint64_t, uint32_t> directed_edges;
  directed_edges.reserve(num_corners);
  std::vector<uint32_t> vertex_corners_offset(num_values + 1, 0);
  std::vector<uint32_t> vertex_corners;
  vertex_corners.reserve(num_corners);

  for (uint32_t f = 0; f < num_faces; ++f) {
    const Mesh::Face& face = mesh.face(FaceIndex(f));
    for (int c = 0; c < 3; ++c) {
      const PointIndex point = face[c];
      if (point.value() >= mesh.num_points()) {
        return Status(Status::INVALID_PARAMETER,
                      "Face references an invalid point.");
      }
      const AttributeValueIndex value_index =
          position_attribute.mapped_index(point);
      if (value_index.value() >= num_values) {
        return Status(Status::INVALID_PARAMETER,
                      "Point maps to an invalid POSITION value.");
      }
      face_values[f][c] = value_index.value();
    }
    const uint32_t v0 = face_values[f][0];
    const uint32_t v1 = face_values[f][1];
    const uint32_t v2 = face_values[f][2];
    if (v0 == v1 || v0 == v2 || v1 == v2) {
      return Status(Status::INVALID_PARAMETER,
                    "Degenerate face: a face must have three distinct "
                    "POSITION values.");
    }
    // Positive area (and a valid original normal) in double precision.
    const double ax = static_cast<double>(input_values[v1 * 3 + 0]) -
                      static_cast<double>(input_values[v0 * 3 + 0]);
    const double ay = static_cast<double>(input_values[v1 * 3 + 1]) -
                      static_cast<double>(input_values[v0 * 3 + 1]);
    const double az = static_cast<double>(input_values[v1 * 3 + 2]) -
                      static_cast<double>(input_values[v0 * 3 + 2]);
    const double bx = static_cast<double>(input_values[v2 * 3 + 0]) -
                      static_cast<double>(input_values[v0 * 3 + 0]);
    const double by = static_cast<double>(input_values[v2 * 3 + 1]) -
                      static_cast<double>(input_values[v0 * 3 + 1]);
    const double bz = static_cast<double>(input_values[v2 * 3 + 2]) -
                      static_cast<double>(input_values[v0 * 3 + 2]);
    const double nx = ay * bz - az * by;
    const double ny = az * bx - ax * bz;
    const double nz = ax * by - ay * bx;
    if (!(nx != 0.0 || ny != 0.0 || nz != 0.0) || !std::isfinite(nx) ||
        !std::isfinite(ny) || !std::isfinite(nz)) {
      return Status(Status::INVALID_PARAMETER,
                    "Degenerate face: zero or non-finite area.");
    }
    original_normals[f * 3 + 0] = nx;
    original_normals[f * 3 + 1] = ny;
    original_normals[f * 3 + 2] = nz;

    // Directed edge uniqueness enforces both orientability and manifoldness of
    // the edge neighborhood; open boundary edges simply occur once.
    for (int c = 0; c < 3; ++c) {
      const uint32_t a = face_values[f][c];
      const uint32_t b = face_values[f][(c + 1) % 3];
      const auto result = directed_edges.insert(
          std::make_pair(DirectedEdgeKey(a, b), 3 * f + c));
      if (!result.second) {
        return Status(Status::INVALID_PARAMETER,
                      "Mesh is not an oriented 2-manifold triangle mesh.");
      }
    }
    for (int c = 0; c < 3; ++c) {
      vertex_corners_offset[face_values[f][c] + 1]++;
    }
  }

  // Prefix sums for the per-value corner lists.
  for (size_t i = 1; i < vertex_corners_offset.size(); ++i) {
    vertex_corners_offset[i] += vertex_corners_offset[i - 1];
  }
  vertex_corners.resize(num_corners);
  {
    std::vector<uint32_t> fill = vertex_corners_offset;
    for (uint32_t f = 0; f < num_faces; ++f) {
      for (int c = 0; c < 3; ++c) {
        vertex_corners[fill[face_values[f][c]]++] = 3 * f + c;
      }
    }
  }

  // Vertex manifoldness: all corners around a POSITION value must belong to a
  // single fan. Corners of two faces that share an edge are merged for each
  // endpoint through a disjoint set.
  {
    DisjointSet fans(num_corners);
    for (uint32_t f = 0; f < num_faces; ++f) {
      for (int c = 0; c < 3; ++c) {
        const uint32_t a = face_values[f][c];
        const uint32_t b = face_values[f][(c + 1) % 3];
        const auto opposite = directed_edges.find(DirectedEdgeKey(b, a));
        if (opposite == directed_edges.end()) {
          continue;  // Boundary edge.
        }
        const uint32_t other_corner = opposite->second;
        const uint32_t other_face = other_corner / 3;
        const uint32_t other_local = other_corner % 3;
        // |other_corner| has tip b and next a. The corner of the other face at
        // vertex a is the next one; the corner at vertex b in this face is the
        // next one from |c|.
        fans.Union(3 * f + c, 3 * other_face + (other_local + 1) % 3);
        fans.Union(3 * f + (c + 1) % 3, 3 * other_face + other_local);
      }
    }
    for (size_t v = 0; v < num_values; ++v) {
      const uint32_t begin = vertex_corners_offset[v];
      const uint32_t end = vertex_corners_offset[v + 1];
      if (begin == end) {
        continue;
      }
      const uint32_t root = fans.Find(vertex_corners[begin]);
      for (uint32_t i = begin + 1; i < end; ++i) {
        if (fans.Find(vertex_corners[i]) != root) {
          return Status(Status::INVALID_PARAMETER,
                        "Mesh has a non-manifold vertex.");
        }
      }
    }
  }

  // Build position value -> incident face reverse adjacency.
  std::vector<uint32_t> face_incidence_offset(num_values + 1, 0);
  for (uint32_t f = 0; f < num_faces; ++f) {
    for (int c = 0; c < 3; ++c) {
      face_incidence_offset[face_values[f][c] + 1]++;
    }
  }
  for (size_t i = 1; i < face_incidence_offset.size(); ++i) {
    face_incidence_offset[i] += face_incidence_offset[i - 1];
  }
  std::vector<uint32_t> incident_faces(num_corners);
  {
    std::vector<uint32_t> fill = face_incidence_offset;
    for (uint32_t f = 0; f < num_faces; ++f) {
      for (int c = 0; c < 3; ++c) {
        incident_faces[fill[face_values[f][c]]++] = f;
      }
    }
  }

  // The plan starts from the plain quantized reconstruction; the fixed point
  // loop only ever adds exact marks to it.
  auto plan = std::make_shared<TopologyPreservingQuantizationPlan>();
  plan->exact_value.assign(num_values, 0);
  plan->quantized_values = quantized;
  plan->reconstructed_values = reconstructed;
  plan->position_attribute_id = position_attribute_id;
  plan->quantization_bits = quantization_bits;
  plan->min_value[0] = min_values[0];
  plan->min_value[1] = min_values[1];
  plan->min_value[2] = min_values[2];
  plan->quantization_range = range;
  plan->stats.unique_position_value_count = num_values;

  // Evaluate whether a face is unsafe under the *current* reconstruction. A
  // value that is marked for exact restoration contributes its original
  // FLOAT32 coordinates; every other value contributes the ordinary quantized
  // reconstruction. All geometry is evaluated in double precision.
  auto current_component = [&](uint32_t value, int component) -> double {
    if (plan->exact_value[value]) {
      return static_cast<double>(input_values[value * 3 + component]);
    }
    return static_cast<double>(reconstructed[value * 3 + component]);
  };
  auto is_face_unsafe = [&](uint32_t f) {
    const uint32_t v0 = face_values[f][0];
    const uint32_t v1 = face_values[f][1];
    const uint32_t v2 = face_values[f][2];
    const double ax = current_component(v1, 0) - current_component(v0, 0);
    const double ay = current_component(v1, 1) - current_component(v0, 1);
    const double az = current_component(v1, 2) - current_component(v0, 2);
    const double bx = current_component(v2, 0) - current_component(v0, 0);
    const double by = current_component(v2, 1) - current_component(v0, 1);
    const double bz = current_component(v2, 2) - current_component(v0, 2);
    const double nx = ay * bz - az * by;
    const double ny = az * bx - ax * bz;
    const double nz = ax * by - ay * bx;
    const double dot = nx * original_normals[f * 3 + 0] +
                       ny * original_normals[f * 3 + 1] +
                       nz * original_normals[f * 3 + 2];
    return dot <= 0.0;
  };

  // Incremental closure: a position -> face reverse adjacency plus a work
  // queue advanced in generations. A full scan of the mesh is done exactly
  // once; all later work is limited to faces incident to values that were
  // newly marked. Each generation uses the mask of the previous generation and
  // applies all new marks at its end, so the result is the canonical
  // inflationary fixed point (F applied round by round) and does not depend on
  // face visitation order.
  std::vector<uint32_t> current_round;
  std::vector<uint32_t> next_round;
  std::vector<uint8_t> queued(num_faces, 0);
  std::vector<uint8_t> queued_next(num_faces, 0);
  std::vector<uint32_t> pending_marks;

  auto enqueue_face = [&](uint32_t f) {
    if (!queued_next[f]) {
      queued_next[f] = 1;
      next_round.push_back(f);
    }
  };
  // Marks |value| exactly and schedules all its incident faces for the next
  // generation. Returns true only when the mark is new.
  auto mark_value = [&](uint32_t value) {
    if (plan->exact_value[value]) {
      return false;
    }
    plan->exact_value[value] = 1;
    ++plan->stats.exact_position_value_count;
    pending_marks.push_back(value);
    for (uint32_t i = face_incidence_offset[value];
         i < face_incidence_offset[value + 1]; ++i) {
      enqueue_face(incident_faces[i]);
    }
    return true;
  };

  // Initial detection. This is the only place where the whole mesh is swept.
  // All faces are evaluated under the plain quantized reconstruction first and
  // every currently dangerous face raises its three marks "simultaneously";
  // the marks are applied only after the sweep so the evaluation cannot depend
  // on face order. This detection counts as closure round 1 when it marks any
  // value (matching the reference iteration, which also counts its first
  // marking pass as a round).
  std::vector<uint32_t> initial_dangerous_faces;
  for (uint32_t f = 0; f < num_faces; ++f) {
    if (is_face_unsafe(f)) {
      initial_dangerous_faces.push_back(f);
    }
  }
  for (uint32_t f : initial_dangerous_faces) {
    for (int c = 0; c < 3; ++c) {
      mark_value(face_values[f][c]);
    }
  }
  if (!initial_dangerous_faces.empty()) {
    if (plan->stats.exact_position_value_count > 0) {
      ++plan->stats.closure_round_count;
    }
    // The marks above already scheduled every affected face (including the
    // seed faces themselves) into |next_round|.
    current_round.swap(next_round);
    std::fill(queued_next.begin(), queued_next.end(), 0);
  }

  while (!current_round.empty()) {
    next_round.clear();
    pending_marks.clear();
    // Evaluate this generation's faces against the mask from the start of the
    // round; marks are collected and only applied afterwards.
    std::vector<uint32_t> dangerous_faces;
    for (uint32_t f : current_round) {
      if (queued[f]) {
        continue;
      }
      queued[f] = 1;
      ++plan->stats.face_recheck_count;
      if (is_face_unsafe(f)) {
        dangerous_faces.push_back(f);
      }
    }
    for (uint32_t f : dangerous_faces) {
      for (int c = 0; c < 3; ++c) {
        const uint32_t value = face_values[f][c];
        if (plan->exact_value[value]) {
          continue;
        }
        plan->exact_value[value] = 1;
        ++plan->stats.exact_position_value_count;
        pending_marks.push_back(value);
      }
    }
    if (!pending_marks.empty()) {
      // This recheck round discovered new marks: it counts as a closure round.
      ++plan->stats.closure_round_count;
    }
    // Schedule every face incident to a value marked in this round.
    for (uint32_t value : pending_marks) {
      for (uint32_t i = face_incidence_offset[value];
           i < face_incidence_offset[value + 1]; ++i) {
        enqueue_face(incident_faces[i]);
      }
    }
    current_round.swap(next_round);
    std::fill(queued_next.begin(), queued_next.end(), 0);
    std::fill(queued.begin(), queued.end(), 0);
  }

  // Exact restoration of a value also affects all points that map to that same
  // POSITION value; the encoder resolves those through mapped_index(), so no
  // extra point-level bookkeeping is needed here.

  *out_plan = std::move(plan);
  return OkStatus();
}

}  // namespace draco
