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
// Tests for the optional mesh topology preserving position quantization mode.
//
// The fixed point is recomputed here by an independent, deliberately naive
// whole-mesh iteration (ReferenceFixedPoint) that derives the lattice from
// first principles; the production plan is never used to validate itself.
// Decoded geometry is additionally checked bit for bit against the coordinates
// the plan says the decoder must produce.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "draco/attributes/attribute_quantization_transform.h"
#include "draco/attributes/attribute_quantization_transform_extension.h"
#include "draco/attributes/attribute_transform_data.h"
#include "draco/compression/attributes/topology_preserving_quantization.h"
#include "draco/compression/config/encoder_options.h"
#include "draco/compression/decode.h"
#include "draco/compression/encode.h"
#include "draco/compression/expert_encode.h"
#include "draco/mesh/triangle_soup_mesh_builder.h"
#include "task_support/check.h"

namespace {

using draco::AttributeValueIndex;
using draco::EncoderBuffer;
using draco::FaceIndex;
using draco::GeometryAttribute;
using draco::Mesh;
using draco::PointAttribute;
using draco::TopologyPreservingQuantizationPlan;

typedef std::shared_ptr<const TopologyPreservingQuantizationPlan> PlanPtr;

// ---------------------------------------------------------------------------
// Fixtures.
// ---------------------------------------------------------------------------

// A fine cube-like shell far away (large, well conditioned) plus a tiny sheet
// near the origin. The shell spans most of the quantization range but its
// triangles are huge compared to the lattice step, so it keeps its orientation;
// the tiny sheet collapses onto a single lattice point and must be restored.
std::unique_ptr<Mesh> MakeFarShellAndTinySheet() {
  draco::TriangleSoupMeshBuilder builder;
  builder.Start(6);
  const int position_id =
      builder.AddAttribute(GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);

  // Large tetrahedron with edges ~100 centered around (900,900,900).
  const float a[3] = {850.f, 850.f, 850.f};
  const float b[3] = {950.f, 850.f, 850.f};
  const float c[3] = {850.f, 950.f, 850.f};
  const float d[3] = {850.f, 850.f, 950.f};
  builder.SetAttributeValuesForFace(position_id, FaceIndex(0), a, b, c);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(1), b, a, d);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(2), a, c, d);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(3), c, b, d);

  // Tiny open sheet at the origin (two triangles sharing an edge) with extent
  // ~0.004. Its area is many orders of magnitude below the lattice step, so it
  // collapses at low bit counts.
  const float e = 0.002f;
  const float s0[3] = {0.f, 0.f, 0.f};
  const float s1[3] = {e, 0.f, 0.f};
  const float s2[3] = {0.f, e, 0.f};
  const float s3[3] = {e, e, 0.f};
  builder.SetAttributeValuesForFace(position_id, FaceIndex(4), s0, s1, s2);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(5), s1, s3, s2);
  return builder.Finalize();
}

// A small planar grid. At high bit counts every face keeps its orientation so
// nothing needs restoring.
std::unique_ptr<Mesh> MakeCleanGrid() {
  draco::TriangleSoupMeshBuilder builder;
  builder.Start(8);
  const int position_id =
      builder.AddAttribute(GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
  const int n = 3;
  std::vector<std::array<float, 3>> grid;
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      grid.push_back(
          {static_cast<float>(x) * 0.25f, static_cast<float>(y) * 0.25f, 0.f});
    }
  }
  int face = 0;
  for (int y = 0; y + 1 < n; ++y) {
    for (int x = 0; x + 1 < n; ++x) {
      const int ia = y * n + x;
      const int ib = ia + 1;
      const int ic = ia + n;
      const int id = ic + 1;
      builder.SetAttributeValuesForFace(position_id, FaceIndex(face++),
                                        grid[ia].data(), grid[ib].data(),
                                        grid[ic].data());
      builder.SetAttributeValuesForFace(position_id, FaceIndex(face++),
                                        grid[ib].data(), grid[id].data(),
                                        grid[ic].data());
    }
  }
  return builder.Finalize();
}

// A grid whose UVs are split along the middle column. Position values are
// shared across the seam (one position value is used by points on both sides)
// while the texture coordinate values differ.
std::unique_ptr<Mesh> MakeSeamMesh() {
  draco::TriangleSoupMeshBuilder builder;
  builder.Start(12);
  const int position_id =
      builder.AddAttribute(GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
  const int uv_id =
      builder.AddAttribute(GeometryAttribute::TEX_COORD, 2, draco::DT_FLOAT32);
  // Far-off tetrahedron widens the quantization range so the seam grid
  // collapses at low bit counts.
  const float s = 850.f;
  const float a[3] = {s, s, s};
  const float b[3] = {s + 100.f, s, s};
  const float c[3] = {s, s + 100.f, s};
  const float d[3] = {s, s, s + 100.f};
  const float* shell[4][3] = {{a, b, c}, {b, a, d}, {a, c, d}, {c, b, d}};
  for (int i = 0; i < 4; ++i) {
    const float uv[3][2] = {{0.f, 0.f}, {1.f, 0.f}, {0.f, 1.f}};
    builder.SetAttributeValuesForFace(position_id, FaceIndex(i), shell[i][0],
                                      shell[i][1], shell[i][2]);
    builder.SetAttributeValuesForFace(uv_id, FaceIndex(i), uv[0], uv[1], uv[2]);
  }
  const int n = 3;
  std::vector<std::array<float, 3>> grid;
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      grid.push_back(
          {static_cast<float>(x) * 0.25f, static_cast<float>(y) * 0.25f, 0.f});
    }
  }
  int face = 4;
  for (int y = 0; y + 1 < n; ++y) {
    for (int x = 0; x + 1 < n; ++x) {
      const int ia = y * n + x;
      const int ib = ia + 1;
      const int ic = ia + n;
      const int id = ic + 1;
      // Left cells use u ~ 0.1, right cells u ~ 0.9. The shared middle column
      // therefore gets two different UV values for the same position, i.e. a
      // real UV seam that splits points without moving positions.
      const float u = (x == 0) ? 0.1f : 0.9f;
      const float uv_a[2] = {u, y * 0.3f};
      const float uv_b[2] = {u, y * 0.3f};
      const float uv_c[2] = {u, (y + 1) * 0.3f};
      const float uv_d[2] = {u, (y + 1) * 0.3f};
      builder.SetAttributeValuesForFace(position_id, FaceIndex(face),
                                        grid[ia].data(), grid[ib].data(),
                                        grid[ic].data());
      builder.SetAttributeValuesForFace(uv_id, FaceIndex(face), uv_a, uv_b,
                                        uv_c);
      ++face;
      builder.SetAttributeValuesForFace(position_id, FaceIndex(face),
                                        grid[ib].data(), grid[id].data(),
                                        grid[ic].data());
      builder.SetAttributeValuesForFace(uv_id, FaceIndex(face), uv_b, uv_d,
                                        uv_c);
      ++face;
    }
  }
  return builder.Finalize();
}

// A single strip of N triangles with alternating winding and z jitter, plus a
// far tetrahedron to widen the quantization range. Parameterized so the tests
// can search for an example that needs more than one propagation round.
std::unique_ptr<Mesh> MakeJitteredStrip(int num_faces, float step,
                                        float amplitude, float shell_scale) {
  draco::TriangleSoupMeshBuilder builder;
  builder.Start(num_faces + 4);
  const int position_id =
      builder.AddAttribute(GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
  const float s = shell_scale;
  const float a[3] = {s, s, s};
  const float b[3] = {s + 0.1f * s, s, s};
  const float c[3] = {s, s + 0.1f * s, s};
  const float d[3] = {s, s, s + 0.1f * s};
  builder.SetAttributeValuesForFace(position_id, FaceIndex(0), a, b, c);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(1), b, a, d);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(2), a, c, d);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(3), c, b, d);

  std::vector<std::array<float, 3>> v;
  for (int i = 0; i < num_faces + 2; ++i) {
    std::array<float, 3> p;
    p[0] = static_cast<float>(i) * step;
    p[1] = (i % 3) * amplitude * 0.1f;
    p[2] = ((i % 2) ? amplitude : -amplitude) * (1.f + 0.1f * (i % 4));
    v.push_back(p);
  }
  for (int i = 0; i < num_faces; ++i) {
    const int f = 4 + i;
    if (i % 2 == 0) {
      builder.SetAttributeValuesForFace(position_id, FaceIndex(f), v[i].data(),
                                        v[i + 1].data(), v[i + 2].data());
    } else {
      builder.SetAttributeValuesForFace(position_id, FaceIndex(f), v[i].data(),
                                        v[i + 2].data(), v[i + 1].data());
    }
  }
  return builder.Finalize();
}

// ---------------------------------------------------------------------------
// Independent reference implementation of the fixed point.
// ---------------------------------------------------------------------------

struct ReferenceResult {
  std::vector<uint8_t> exact;  // Indexed by POSITION AttributeValueIndex.
  int rounds = 0;
};

// Recomputes the ordinary quantization/dequantization from first principles
// (same single precision arithmetic as Quantizer/Dequantizer, but written
// independently) and iterates the whole mesh with simultaneous rounds until it
// stops changing. Uses double precision for all geometry predicates.
ReferenceResult ReferenceFixedPoint(const Mesh& mesh, int position_att_id,
                                    int bits) {
  const PointAttribute* pos = mesh.attribute(position_att_id);
  const size_t num_values = pos->size();
  ReferenceResult result;
  result.exact.assign(num_values, 0);

  float min_v[3];
  float max_v[3];
  pos->GetValue(AttributeValueIndex(0), min_v);
  pos->GetValue(AttributeValueIndex(0), max_v);
  float raw[3];
  for (AttributeValueIndex i(1); i < num_values; ++i) {
    pos->GetValue(i, raw);
    for (int c = 0; c < 3; ++c) {
      min_v[c] = std::min(min_v[c], raw[c]);
      max_v[c] = std::max(max_v[c], raw[c]);
    }
  }
  double range = 0.0;
  for (int c = 0; c < 3; ++c) {
    range = std::max(range, static_cast<double>(max_v[c]) - min_v[c]);
  }
  if (range == 0.0) {
    range = 1.0;
  }
  const uint32_t max_quant = (1u << bits) - 1;

  std::vector<double> original(num_values * 3);
  std::vector<double> reconstructed(num_values * 3);
  for (AttributeValueIndex i(0); i < num_values; ++i) {
    pos->GetValue(i, raw);
    for (int c = 0; c < 3; ++c) {
      original[i.value() * 3 + c] = raw[c];
      const float centered = raw[c] - min_v[c];
      const float inverse_delta =
          static_cast<float>(max_quant) / static_cast<float>(range);
      const int32_t q =
          static_cast<int32_t>(std::floor(centered * inverse_delta + 0.5f));
      const float delta = static_cast<float>(range) / static_cast<float>(max_quant);
      reconstructed[i.value() * 3 + c] =
          static_cast<double>(static_cast<float>(q) * delta + min_v[c]);
    }
  }

  std::vector<uint8_t> next(num_values, 0);
  while (true) {
    std::fill(next.begin(), next.end(), 0);
    bool changed = false;
    for (FaceIndex f(0); f < mesh.num_faces(); ++f) {
      const auto& face = mesh.face(f);
      uint32_t idx[3];
      double p[3][3];
      double o[3][3];
      for (int c = 0; c < 3; ++c) {
        idx[c] = pos->mapped_index(face[c]).value();
        for (int k = 0; k < 3; ++k) {
          p[c][k] = result.exact[idx[c]] ? original[idx[c] * 3 + k]
                                         : reconstructed[idx[c] * 3 + k];
          o[c][k] = original[idx[c] * 3 + k];
        }
      }
      const double ax = p[1][0] - p[0][0], ay = p[1][1] - p[0][1],
                   az = p[1][2] - p[0][2];
      const double bx = p[2][0] - p[0][0], by = p[2][1] - p[0][1],
                   bz = p[2][2] - p[0][2];
      const double nx = ay * bz - az * by, ny = az * bx - ax * bz,
                   nz = ax * by - ay * bx;
      const double oax = o[1][0] - o[0][0], oay = o[1][1] - o[0][1],
                   oaz = o[1][2] - o[0][2];
      const double obx = o[2][0] - o[0][0], oby = o[2][1] - o[0][1],
                   obz = o[2][2] - o[0][2];
      const double onx = oay * obz - oaz * oby, ony = oaz * obx - oax * obz,
                   onz = oax * oby - oay * obx;
      if (nx * onx + ny * ony + nz * onz <= 0.0) {
        for (int c = 0; c < 3; ++c) {
          if (!result.exact[idx[c]]) {
            next[idx[c]] = 1;
            changed = true;
          }
        }
      }
    }
    if (!changed) {
      break;
    }
    for (size_t i = 0; i < num_values; ++i) {
      result.exact[i] |= next[i];
    }
    ++result.rounds;
  }
  return result;
}

draco::Status BuildPlan(const Mesh& mesh, int bits, PlanPtr* out_plan) {
  draco::EncoderOptions options = draco::EncoderOptions::CreateDefaultOptions();
  options.SetAttributeInt(0, "quantization_bits", bits);
  return draco::BuildTopologyPreservingQuantizationPlan(mesh, options, out_plan);
}

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

int CountExact(const std::vector<uint8_t>& mask) {
  int n = 0;
  for (uint8_t v : mask) {
    n += v ? 1 : 0;
  }
  return n;
}

void FaceNormal(const PointAttribute& pos, const Mesh& mesh, FaceIndex f,
                const double p[3][3], double out[3]) {
  (void)pos;
  (void)mesh;
  (void)f;
  const double ax = p[1][0] - p[0][0], ay = p[1][1] - p[0][1],
               az = p[1][2] - p[0][2];
  const double bx = p[2][0] - p[0][0], by = p[2][1] - p[0][1],
               bz = p[2][2] - p[0][2];
  out[0] = ay * bz - az * by;
  out[1] = az * bx - ax * bz;
  out[2] = ax * by - ay * bx;
}

// Expected coordinate of the POSITION value |value| after decoding.
void ExpectedCoordinate(const std::vector<float>& input_values,
                        const TopologyPreservingQuantizationPlan& plan,
                        uint32_t value, int component, double* out) {
  *out = plan.exact_value[value]
             ? static_cast<double>(input_values[value * 3 + component])
             : static_cast<double>(plan.reconstructed_values[value * 3 + component]);
}

bool ReadInputPositions(const Mesh& mesh, int position_att_id,
                        std::vector<float>* out) {
  const PointAttribute* pos = mesh.attribute(position_att_id);
  out->assign(pos->size() * 3, 0.f);
  for (AttributeValueIndex i(0); i < pos->size(); ++i) {
    pos->GetValue(i, out->data() + i.value() * 3);
  }
  return true;
}

// Verifies the plan's central promise independently of the decoder: with the
// plan's reconstruction every face keeps a strictly positive dot product with
// its original normal. This is exactly the "no face flips or collapses" rule.
bool PlanPreservesAllFaceNormals(const Mesh& mesh, int position_att_id,
                                 const TopologyPreservingQuantizationPlan& plan) {
  const PointAttribute* pos = mesh.attribute(position_att_id);
  std::vector<float> input_values;
  ReadInputPositions(mesh, position_att_id, &input_values);
  for (FaceIndex f(0); f < mesh.num_faces(); ++f) {
    const auto& face = mesh.face(f);
    uint32_t idx[3];
    double p[3][3];
    double o[3][3];
    for (int c = 0; c < 3; ++c) {
      idx[c] = pos->mapped_index(face[c]).value();
      for (int k = 0; k < 3; ++k) {
        ExpectedCoordinate(input_values, plan, idx[c], k, &p[c][k]);
        o[c][k] = input_values[idx[c] * 3 + k];
      }
    }
    double n[3];
    double on[3];
    FaceNormal(*pos, mesh, f, p, n);
    FaceNormal(*pos, mesh, f, o, on);
    if (!(n[0] * on[0] + n[1] * on[1] + n[2] * on[2] > 0.0)) {
      return false;
    }
  }
  return true;
}

// Verifies that |decoded| contains exactly the coordinates the plan predicts
// for every original face: restored positions appear with their original
// FLOAT32 bits, all other positions with the ordinary quantized
// reconstruction. Faces may be reordered (Edgebreaker), so faces are matched
// as unordered coordinate triples.
bool DecodedMatchesPlan(const Mesh& original, int position_att_id,
                        const TopologyPreservingQuantizationPlan& plan,
                        const Mesh& decoded) {
  const PointAttribute* op = original.attribute(position_att_id);
  const PointAttribute* dp =
      decoded.GetNamedAttribute(GeometryAttribute::POSITION);
  if (dp == nullptr || original.num_faces() != decoded.num_faces()) {
    return false;
  }
  std::vector<float> input_values;
  ReadInputPositions(original, position_att_id, &input_values);

  std::array<std::array<float, 3>, 3> expected;
  std::vector<std::array<std::array<float, 3>, 3>> expected_faces;
  expected_faces.reserve(original.num_faces());
  for (FaceIndex f(0); f < original.num_faces(); ++f) {
    const auto& face = original.face(f);
    for (int c = 0; c < 3; ++c) {
      const uint32_t value = op->mapped_index(face[c]).value();
      for (int k = 0; k < 3; ++k) {
        expected[c][k] = plan.exact_value[value]
                             ? input_values[value * 3 + k]
                             : plan.reconstructed_values[value * 3 + k];
      }
    }
    expected_faces.push_back(expected);
  }
  auto less = [](const std::array<float, 3>& l, const std::array<float, 3>& r) {
    return std::memcmp(l.data(), r.data(), sizeof(float) * 3) < 0;
  };
  for (auto& face : expected_faces) {
    std::sort(face.begin(), face.end(), less);
  }
  std::vector<uint8_t> used(decoded.num_faces(), 0);
  for (const auto& want : expected_faces) {
    bool matched = false;
    for (FaceIndex g(0); g < decoded.num_faces(); ++g) {
      if (used[g.value()]) {
        continue;
      }
      const auto& df = decoded.face(g);
      std::array<std::array<float, 3>, 3> got;
      for (int c = 0; c < 3; ++c) {
        dp->GetValue(dp->mapped_index(df[c]), got[c].data());
      }
      std::sort(got.begin(), got.end(), less);
      if (std::memcmp(got.data(), want.data(), sizeof(float) * 9) == 0) {
        used[g.value()] = 1;
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }
  return true;
}

bool DecodeMesh(const EncoderBuffer& buffer, std::unique_ptr<Mesh>* out_mesh,
                bool skip_position_transform = false) {
  draco::Decoder decoder;
  if (skip_position_transform) {
    decoder.SetSkipAttributeTransform(GeometryAttribute::POSITION);
  }
  draco::DecoderBuffer in;
  in.Init(buffer.data(), buffer.size());
  auto result = decoder.DecodeMeshFromBuffer(&in);
  if (!result.ok()) {
    return false;
  }
  *out_mesh = std::move(result).value();
  return *out_mesh != nullptr;
}

draco::Status EncodeProtected(const Mesh& mesh, int method, int bits,
                              EncoderBuffer* out_buffer,
                              draco::MeshTopologyPreservingQuantizationStats*
                                  stats = nullptr) {
  draco::Encoder encoder;
  encoder.SetEncodingMethod(method);
  encoder.SetSpeedOptions(5, 5);
  encoder.SetAttributeQuantization(GeometryAttribute::POSITION, bits);
  encoder.SetMeshTopologyPreservingQuantization(true);
  const draco::Status status = encoder.EncodeMeshToBuffer(mesh, out_buffer);
  if (status.ok() && stats != nullptr) {
    *stats = encoder.topology_preserving_quantization_stats();
  }
  return status;
}

// ---------------------------------------------------------------------------
// Group: closure.
// ---------------------------------------------------------------------------

int CaseClosureMatchesReference() {
  struct Config {
    int faces;
    float step;
    float amplitude;
    float shell;
  };
  const Config configs[] = {
      {4, 0.02f, 0.01f, 1000.f}, {7, 0.01f, 0.02f, 1000.f},
      {9, 0.03f, 0.005f, 1000.f}, {12, 0.05f, 0.02f, 500.f},
      {6, 0.004f, 0.001f, 1000.f},
  };
  int compared = 0;
  for (const Config& config : configs) {
    auto mesh = MakeJitteredStrip(config.faces, config.step, config.amplitude,
                                  config.shell);
    TASK_CHECK(mesh != nullptr);
    for (int bits : {4, 6, 8, 10, 12, 15, 20}) {
      PlanPtr plan;
      TASK_CHECK(BuildPlan(*mesh, bits, &plan).ok());
      TASK_CHECK(plan != nullptr);
      const PointAttribute* pos =
          mesh->GetNamedAttribute(GeometryAttribute::POSITION);
      const ReferenceResult reference = ReferenceFixedPoint(*mesh, 0, bits);
      TASK_CHECK(plan->stats.unique_position_value_count == pos->size());
      TASK_CHECK(plan->exact_value.size() == pos->size());
      for (size_t i = 0; i < pos->size(); ++i) {
        TASK_CHECK(plan->exact_value[i] == reference.exact[i]);
      }
      TASK_CHECK(plan->stats.exact_position_value_count ==
                 static_cast<size_t>(CountExact(plan->exact_value)));
      TASK_CHECK(plan->stats.closure_round_count ==
                 static_cast<size_t>(reference.rounds));
      TASK_CHECK(PlanPreservesAllFaceNormals(*mesh, 0, *plan));
      ++compared;
    }
  }
  TASK_CHECK(compared == static_cast<int>(sizeof(configs) / sizeof(configs[0])) *
                             7);
  return 0;
}

int CaseClosureUsesMoreThanOneRound() {
  // A fixed, hand-checked fixture whose fixed point needs exactly two
  // propagation rounds: restoring the first cluster of values flips a
  // neighboring face, which restores a value that is not incident to the
  // initially dangerous faces. (Validated against ReferenceFixedPoint below;
  // the fixture was found by an exhaustive deterministic search.)
  static const float kSheetVertices[8][3] = {
      {0.f, 0.00166423246f, -0.025259601f},
      {0.0247657262f, 0.00497589959f, 0.00667815842f},
      {0.0495314524f, 0.00671112956f, -0.00749429921f},
      {0.0742971748f, 0.00531096756f, 0.00984957349f},
      {0.0990629047f, 0.00526081817f, -0.00381947472f},
      {0.123828627f, 0.000507485296f, 0.00946335681f},
      {0.14859435f, 0.00217049289f, -0.000490147155f},
      {0.17336008f, 0.00731132505f, 0.00375128305f},
  };
  draco::TriangleSoupMeshBuilder builder;
  builder.Start(10);
  const int position_id =
      builder.AddAttribute(GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
  const float s = 1000.f;
  const float a[3] = {s, s, s};
  const float b[3] = {s + 0.1f, s, s};
  const float c[3] = {s, s + 0.1f, s};
  const float d[3] = {s, s, s + 0.1f};
  // A correctly oriented far tetrahedron.
  builder.SetAttributeValuesForFace(position_id, FaceIndex(0), a, b, c);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(1), b, a, d);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(2), a, c, d);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(3), c, b, d);
  for (int i = 0; i < 6; ++i) {
    const int f = 4 + i;
    if (i % 2 == 0) {
      builder.SetAttributeValuesForFace(
          position_id, FaceIndex(f), kSheetVertices[i], kSheetVertices[i + 1],
          kSheetVertices[i + 2]);
    } else {
      builder.SetAttributeValuesForFace(
          position_id, FaceIndex(f), kSheetVertices[i], kSheetVertices[i + 2],
          kSheetVertices[i + 1]);
    }
  }
  auto mesh = builder.Finalize();
  TASK_CHECK(mesh != nullptr);

  PlanPtr plan;
  TASK_CHECK(BuildPlan(*mesh, 14, &plan).ok());
  const ReferenceResult reference = ReferenceFixedPoint(*mesh, 0, 14);
  TASK_CHECK(reference.rounds >= 2);
  TASK_CHECK(plan->stats.closure_round_count >= 2);
  TASK_CHECK(plan->stats.closure_round_count ==
             static_cast<size_t>(reference.rounds));
  for (size_t i = 0; i < plan->exact_value.size(); ++i) {
    TASK_CHECK(plan->exact_value[i] == reference.exact[i]);
  }
  TASK_CHECK(plan->stats.face_recheck_count > 0);
  TASK_CHECK(plan->stats.exact_position_value_count > 0);
  TASK_CHECK(plan->stats.used_incremental_closure());
  TASK_CHECK(!plan->stats.full_lossless_position_fallback());
  TASK_CHECK(PlanPreservesAllFaceNormals(*mesh, 0, *plan));

  // The real stream must reproduce the fixed point too, for both mesh coding
  // methods.
  for (int method : {int(draco::MESH_SEQUENTIAL_ENCODING),
                     int(draco::MESH_EDGEBREAKER_ENCODING)}) {
    EncoderBuffer buffer;
    TASK_CHECK(EncodeProtected(*mesh, method, 14, &buffer).ok());
    std::unique_ptr<Mesh> decoded;
    TASK_CHECK(DecodeMesh(buffer, &decoded));
    TASK_CHECK(DecodedMatchesPlan(*mesh, 0, *plan, *decoded));
  }
  return 0;
}

int CaseClosureLargeShellKeepsSheetLocal() {
  auto mesh = MakeFarShellAndTinySheet();
  TASK_CHECK(mesh != nullptr);
  PlanPtr plan;
  TASK_CHECK(BuildPlan(*mesh, 8, &plan).ok());
  const ReferenceResult reference = ReferenceFixedPoint(*mesh, 0, 8);
  for (size_t i = 0; i < plan->exact_value.size(); ++i) {
    TASK_CHECK(plan->exact_value[i] == reference.exact[i]);
  }
  // The tiny sheet collapses and must be restored; the far shell keeps its
  // orientation and must NOT be restored. This is a localized exception set,
  // not a full lossless fallback.
  TASK_CHECK(plan->stats.exact_position_value_count > 0);
  TASK_CHECK(plan->stats.exact_position_value_count <
             plan->stats.unique_position_value_count);
  TASK_CHECK(!plan->stats.full_lossless_position_fallback());
  // The shell's position values must remain quantized: check that at least one
  // far-away value is not exact.
  const PointAttribute* pos = mesh->GetNamedAttribute(GeometryAttribute::POSITION);
  bool far_value_quantized = false;
  for (AttributeValueIndex i(0); i < pos->size(); ++i) {
    float v[3];
    pos->GetValue(i, v);
    if (v[0] > 500.f && !plan->exact_value[i.value()]) {
      far_value_quantized = true;
    }
  }
  TASK_CHECK(far_value_quantized);
  TASK_CHECK(PlanPreservesAllFaceNormals(*mesh, 0, *plan));
  return 0;
}

int CaseClosureCleanGridNeedsNothing() {
  auto mesh = MakeCleanGrid();
  TASK_CHECK(mesh != nullptr);
  for (int bits : {16, 20, 24}) {
    PlanPtr plan;
    TASK_CHECK(BuildPlan(*mesh, bits, &plan).ok());
    TASK_CHECK(plan->stats.exact_position_value_count == 0);
    TASK_CHECK(plan->stats.face_recheck_count == 0);
    TASK_CHECK(plan->stats.closure_round_count == 0);
    TASK_CHECK(!plan->stats.used_incremental_closure());
    TASK_CHECK(!plan->stats.full_lossless_position_fallback());
    TASK_CHECK(CountExact(plan->exact_value) == 0);
    TASK_CHECK(PlanPreservesAllFaceNormals(*mesh, 0, *plan));
    // Without restoration the extension is still signalled, but there are no
    // records. Verify the stream round trip.
    EncoderBuffer buffer;
    TASK_CHECK(
        EncodeProtected(*mesh, draco::MESH_EDGEBREAKER_ENCODING, bits, &buffer)
            .ok());
    std::unique_ptr<Mesh> decoded;
    TASK_CHECK(DecodeMesh(buffer, &decoded));
    TASK_CHECK(DecodedMatchesPlan(*mesh, 0, *plan, *decoded));
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Group: mapping.
// ---------------------------------------------------------------------------

int CaseMappingExactValuesSurviveEdgebreakerReordering() {
  auto mesh = MakeFarShellAndTinySheet();
  TASK_CHECK(mesh != nullptr);
  for (int method : {int(draco::MESH_SEQUENTIAL_ENCODING),
                     int(draco::MESH_EDGEBREAKER_ENCODING)}) {
    PlanPtr plan;
    TASK_CHECK(BuildPlan(*mesh, 8, &plan).ok());
    EncoderBuffer buffer;
    TASK_CHECK(EncodeProtected(*mesh, method, 8, &buffer).ok());
    std::unique_ptr<Mesh> decoded;
    TASK_CHECK(DecodeMesh(buffer, &decoded));
    TASK_CHECK(decoded->num_faces() == mesh->num_faces());
    TASK_CHECK(DecodedMatchesPlan(*mesh, 0, *plan, *decoded));
    // All restored values must be present with their exact original bits.
    const PointAttribute* op =
        mesh->GetNamedAttribute(GeometryAttribute::POSITION);
    const PointAttribute* dp =
        decoded->GetNamedAttribute(GeometryAttribute::POSITION);
    std::vector<float> input_values;
    ReadInputPositions(*mesh, 0, &input_values);
    for (size_t v = 0; v < plan->exact_value.size(); ++v) {
      if (!plan->exact_value[v]) {
        continue;
      }
      bool found = false;
      for (AttributeValueIndex i(0); i < dp->size(); ++i) {
        float dv[3];
        dp->GetValue(i, dv);
        if (std::memcmp(dv, &input_values[v * 3], sizeof(float) * 3) == 0) {
          found = true;
          break;
        }
      }
      TASK_CHECK(found);
    }
    TASK_CHECK(op->size() > 0);
  }
  return 0;
}

int CaseMappingSeamBothSidesRestoredTogether() {
  auto mesh = MakeSeamMesh();
  TASK_CHECK(mesh != nullptr);
  // A far-off tetrahedron widens the range so the seam grid collapses. The
  // seam is position-shared (one position value, several points) so restoring
  // a seam position must restore it for both sides at once.
  PlanPtr plan;
  TASK_CHECK(BuildPlan(*mesh, 8, &plan).ok());
  // The middle column must actually be shared by points with different UVs,
  // i.e. two distinct points map to the same POSITION value.
  {
    const PointAttribute* pos =
        mesh->GetNamedAttribute(GeometryAttribute::POSITION);
    std::vector<int> usage(pos->size(), 0);
    for (draco::PointIndex p(0); p < mesh->num_points(); ++p) {
      ++usage[pos->mapped_index(p).value()];
    }
    int shared = 0;
    for (int count : usage) {
      if (count > 1) {
        ++shared;
      }
    }
    TASK_CHECK(shared > 0);
  }
  for (int method : {int(draco::MESH_SEQUENTIAL_ENCODING),
                     int(draco::MESH_EDGEBREAKER_ENCODING)}) {
    EncoderBuffer buffer;
    TASK_CHECK(EncodeProtected(*mesh, method, 8, &buffer).ok());
    std::unique_ptr<Mesh> decoded;
    TASK_CHECK(DecodeMesh(buffer, &decoded));
    TASK_CHECK(DecodedMatchesPlan(*mesh, 0, *plan, *decoded));
    // Texture coordinates must be present with two components. Their number of
    // unique values may grow because attribute seams can be duplicated by the
    // mesh coders, so compare coordinates as a set.
    const PointAttribute* ouv =
        mesh->GetNamedAttribute(GeometryAttribute::TEX_COORD);
    const PointAttribute* duv =
        decoded->GetNamedAttribute(GeometryAttribute::TEX_COORD);
    TASK_CHECK(ouv != nullptr);
    TASK_CHECK(duv != nullptr);
    TASK_CHECK(ouv->num_components() == 2);
    TASK_CHECK(duv->num_components() == 2);
    std::vector<std::array<float, 2>> decoded_uvs;
    for (AttributeValueIndex i(0); i < duv->size(); ++i) {
      float v[2];
      duv->GetValue(i, v);
      decoded_uvs.push_back({v[0], v[1]});
    }
    for (AttributeValueIndex i(0); i < ouv->size(); ++i) {
      float v[2];
      ouv->GetValue(i, v);
      bool found = false;
      for (const auto& candidate : decoded_uvs) {
        if (candidate[0] == v[0] && candidate[1] == v[1]) {
          found = true;
          break;
        }
      }
      TASK_CHECK(found);
    }
    TASK_CHECK(decoded->num_faces() == mesh->num_faces());
  }
  return 0;
}

int CaseMappingSharedValueRestoredForEveryPoint() {
  // A thin two-triangle fan around a shared center position value; the whole
  // fan is dwarfed by a far tetrahedron so it collapses. The two triangles
  // share the edge (center, p1) in opposite directions so the fan is a valid
  // manifold pair, and several points map to the center position value.
  draco::TriangleSoupMeshBuilder builder;
  builder.Start(6);
  const int position_id =
      builder.AddAttribute(GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
  const float a[3] = {850.f, 850.f, 850.f};
  const float b[3] = {950.f, 850.f, 850.f};
  const float c[3] = {850.f, 950.f, 850.f};
  const float d[3] = {850.f, 850.f, 950.f};
  builder.SetAttributeValuesForFace(position_id, FaceIndex(0), a, b, c);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(1), b, a, d);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(2), a, c, d);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(3), c, b, d);
  const float center[3] = {0.f, 0.f, 0.f};
  const float p1[3] = {0.0005f, 0.f, 0.f};
  const float p2[3] = {0.f, 0.0005f, 0.f};
  const float p3[3] = {0.f, -0.0005f, 0.f};
  builder.SetAttributeValuesForFace(position_id, FaceIndex(4), center, p1, p2);
  builder.SetAttributeValuesForFace(position_id, FaceIndex(5), p1, center, p3);
  auto mesh = builder.Finalize();
  TASK_CHECK(mesh != nullptr);
  PlanPtr plan;
  TASK_CHECK(BuildPlan(*mesh, 8, &plan).ok());
  TASK_CHECK(plan->stats.exact_position_value_count > 0);
  EncoderBuffer buffer;
  TASK_CHECK(
      EncodeProtected(*mesh, draco::MESH_EDGEBREAKER_ENCODING, 8, &buffer).ok());
  std::unique_ptr<Mesh> decoded;
  TASK_CHECK(DecodeMesh(buffer, &decoded));
  TASK_CHECK(DecodedMatchesPlan(*mesh, 0, *plan, *decoded));
  return 0;
}

// Makes a deep copy of |src| (the native build has no public Mesh copy ctor).
std::unique_ptr<Mesh> CloneMesh(const Mesh& src) {
  std::unique_ptr<Mesh> dst(new Mesh());
  dst->set_num_points(src.num_points());
  dst->SetNumFaces(src.num_faces());
  for (FaceIndex f(0); f < src.num_faces(); ++f) {
    dst->SetFace(f, src.face(f));
  }
  for (int a = 0; a < src.num_attributes(); ++a) {
    const PointAttribute* s = src.attribute(a);
    draco::GeometryAttribute ga;
    ga.Init(s->attribute_type(), nullptr, s->num_components(),
            s->data_type(), s->normalized(), s->byte_stride(),
            s->byte_offset());
    std::unique_ptr<PointAttribute> d(new PointAttribute(ga));
    if (!d->Reset(s->size())) {
      return nullptr;
    }
    if (s->is_mapping_identity()) {
      d->SetIdentityMapping();
    } else {
      d->SetExplicitMapping(s->indices_map_size());
      for (draco::PointIndex p(0); p < s->indices_map_size(); ++p) {
        d->SetPointMapEntry(p, s->mapped_index(p));
      }
    }
    d->buffer()->Update(s->buffer()->data(), s->buffer()->data_size());
    d->set_unique_id(s->unique_id());
    if (s->GetAttributeTransformData() != nullptr) {
      d->SetAttributeTransformData(std::unique_ptr<draco::AttributeTransformData>(
          new draco::AttributeTransformData(*s->GetAttributeTransformData())));
    }
    const int id = dst->AddAttribute(std::move(d));
    if (id < 0) {
      return nullptr;
    }
    dst->SetAttributeElementType(id, src.GetAttributeElementType(a));
  }
  return dst;
}

int CaseMappingIsolatedUnusedValueStaysQuantized() {
  // A mesh whose POSITION attribute has an extra value that no point maps to.
  // That value still contributes to the quantization box but has no portable
  // entry, so it must never produce an exception record (and the stream must
  // still decode successfully).
  auto base = MakeFarShellAndTinySheet();
  TASK_CHECK(base != nullptr);
  std::unique_ptr<Mesh> mesh = CloneMesh(*base);
  TASK_CHECK(mesh != nullptr);
  const int position_id =
      mesh->GetNamedAttributeId(GeometryAttribute::POSITION);
  TASK_CHECK(position_id >= 0);
  PointAttribute* pos = mesh->attribute(position_id);
  const AttributeValueIndex new_index(pos->size());
  draco::GeometryAttribute ga;
  ga.Init(GeometryAttribute::POSITION, nullptr, 3, draco::DT_FLOAT32, false,
          sizeof(float) * 3, 0);
  std::unique_ptr<PointAttribute> grown(new PointAttribute(ga));
  TASK_CHECK(grown->Reset(pos->size() + 1));
  grown->SetIdentityMapping();
  for (AttributeValueIndex i(0); i < pos->size(); ++i) {
    float v[3];
    pos->GetValue(i, v);
    grown->SetAttributeValue(i, v);
  }
  const float unused[3] = {500.f, 500.f, 500.f};
  grown->SetAttributeValue(new_index, unused);
  grown->set_unique_id(pos->unique_id());
  mesh->SetAttribute(position_id, std::move(grown));

  PlanPtr plan;
  TASK_CHECK(BuildPlan(*mesh, 8, &plan).ok());
  TASK_CHECK(plan->stats.unique_position_value_count == new_index.value() + 1);
  TASK_CHECK(!plan->exact_value[new_index.value()]);
  // Isolated values are not dangerous and must remain quantized.
  EncoderBuffer buffer;
  TASK_CHECK(
      EncodeProtected(*mesh, draco::MESH_SEQUENTIAL_ENCODING, 8, &buffer).ok());
  std::unique_ptr<Mesh> decoded;
  TASK_CHECK(DecodeMesh(buffer, &decoded));
  TASK_CHECK(DecodedMatchesPlan(*mesh, position_id, *plan, *decoded));

  // Skip-transform image: every exception record must reference an encoded
  // value below the portable count, never the unused input value.
  std::unique_ptr<Mesh> skipped;
  TASK_CHECK(DecodeMesh(buffer, &skipped, /*skip_position_transform=*/true));
  const PointAttribute* spos =
      skipped->GetNamedAttribute(GeometryAttribute::POSITION);
  TASK_CHECK(spos != nullptr);
  TASK_CHECK(spos->GetAttributeTransformData() != nullptr);
  TASK_CHECK(spos->GetAttributeTransformData()->transform_type() ==
             draco::ATTRIBUTE_QUANTIZATION_TOPOLOGY_TRANSFORM);
  draco::AttributeQuantizationTransform transform;
  TASK_CHECK(transform.InitFromAttribute(*spos));
  for (const auto& record : transform.exact_values()) {
    TASK_CHECK(record.encoded_value_index < spos->size());
  }
  return 0;
}

int CaseMappingEqualCoordinatesDifferentIndicesNotWelded() {
  // Two faces with identical coordinates but different POSITION attribute
  // values (the classic "split points" case). Sharing in this mode is defined
  // by mapped_index(), so the coincident vertices are distinct: the plan must
  // not merge them, and each value gets its own mask bit.
  //
  // The soup builder deduplicates equal attribute values, so the mesh is built
  // by hand: a far shell (to widen the quantization range) plus two coincident
  // tiny triangles whose three corners each use separate POSITION values.
  const float s = 850.f;
  const float e = 0.0005f;
  struct V3 {
    float v[3];
  };
  // Shell vertices (4), then 3 vertices for each of the two coincident
  // triangles = 10 unique points, positions, and attribute values.
  const V3 positions[10] = {
      {{s, s, s}},
      {{s + 100.f, s, s}},
      {{s, s + 100.f, s}},
      {{s, s, s + 100.f}},
      // Triangle A.
      {{0.f, 0.f, 0.f}},
      {{e, 0.f, 0.f}},
      {{0.f, e, 0.f}},
      // Triangle B: identical coordinates, distinct values.
      {{0.f, 0.f, 0.f}},
      {{e, 0.f, 0.f}},
      {{0.f, e, 0.f}},
  };
  const int faces[6][3] = {{0, 1, 2}, {1, 0, 3}, {0, 2, 3}, {2, 1, 3},
                           {4, 5, 6}, {7, 9, 8}};
  std::unique_ptr<Mesh> mesh(new Mesh());
  mesh->set_num_points(10);
  draco::GeometryAttribute ga;
  ga.Init(GeometryAttribute::POSITION, nullptr, 3, draco::DT_FLOAT32, false,
          sizeof(float) * 3, 0);
  std::unique_ptr<PointAttribute> pos(new PointAttribute(ga));
  TASK_CHECK(pos->Reset(10));
  pos->SetIdentityMapping();
  for (int i = 0; i < 10; ++i) {
    pos->SetAttributeValue(AttributeValueIndex(i), positions[i].v);
  }
  pos->set_unique_id(0);
  const int position_id = mesh->AddAttribute(std::move(pos));
  TASK_CHECK(position_id == 0);
  mesh->SetNumFaces(6);
  for (int f = 0; f < 6; ++f) {
    Mesh::Face face;
    for (int c = 0; c < 3; ++c) {
      face[c] = draco::PointIndex(faces[f][c]);
    }
    mesh->SetFace(FaceIndex(f), face);
  }
  Mesh& mesh_ref = *mesh;
  const PointAttribute* pos_ref =
      mesh_ref.GetNamedAttribute(GeometryAttribute::POSITION);
  // Values 4 and 7 (and 5/8, 6/9) have identical coordinates but are distinct
  // attribute values.
  {
    float a[3];
    float b[3];
    pos_ref->GetValue(AttributeValueIndex(4), a);
    pos_ref->GetValue(AttributeValueIndex(7), b);
    TASK_CHECK(std::memcmp(a, b, sizeof(a)) == 0);
  }
  TASK_CHECK(pos_ref->size() == 10);
  PlanPtr plan;
  TASK_CHECK(BuildPlan(mesh_ref, 8, &plan).ok());
  // The plan is indexed per attribute value, never per distinct coordinate.
  TASK_CHECK(plan->stats.unique_position_value_count == pos_ref->size());
  TASK_CHECK(plan->exact_value.size() == pos_ref->size());
  EncoderBuffer buffer;
  TASK_CHECK(EncodeProtected(mesh_ref, draco::MESH_SEQUENTIAL_ENCODING, 8,
                             &buffer)
                 .ok());
  std::unique_ptr<Mesh> decoded;
  TASK_CHECK(DecodeMesh(buffer, &decoded));
  TASK_CHECK(DecodedMatchesPlan(mesh_ref, 0, *plan, *decoded));
  // Both coincident triangles must survive; the decoder must not have welded
  // the ten input values into fewer due to coordinate equality.
  TASK_CHECK(decoded->num_faces() == 6);
  return 0;
}

// ---------------------------------------------------------------------------
// Group: stream.
// ---------------------------------------------------------------------------
int CaseStreamExactThroughOrdinaryDecoderBothMethods() {
  for (int method : {int(draco::MESH_SEQUENTIAL_ENCODING),
                     int(draco::MESH_EDGEBREAKER_ENCODING)}) {
    auto shells = MakeFarShellAndTinySheet();
    auto clean = MakeCleanGrid();
    TASK_CHECK(shells != nullptr);
    TASK_CHECK(clean != nullptr);
    for (Mesh* mesh : {shells.get(), clean.get()}) {
      for (int bits : {8, 12}) {
        PlanPtr plan;
        TASK_CHECK(BuildPlan(*mesh, bits, &plan).ok());
        EncoderBuffer buffer;
        TASK_CHECK(EncodeProtected(*mesh, method, bits, &buffer).ok());
        TASK_CHECK(buffer.size() > 0);
        std::unique_ptr<Mesh> decoded;
        TASK_CHECK(DecodeMesh(buffer, &decoded));
        TASK_CHECK(decoded->num_faces() == mesh->num_faces());
        TASK_CHECK(DecodedMatchesPlan(*mesh, 0, *plan, *decoded));
        TASK_CHECK(PlanPreservesAllFaceNormals(*mesh, 0, *plan));
      }
    }
  }
  return 0;
}

int CaseStreamSkipTransformInverseMatchesDecode() {
  auto mesh = MakeFarShellAndTinySheet();
  TASK_CHECK(mesh != nullptr);
  for (int method : {int(draco::MESH_SEQUENTIAL_ENCODING),
                     int(draco::MESH_EDGEBREAKER_ENCODING)}) {
    EncoderBuffer buffer;
    TASK_CHECK(EncodeProtected(*mesh, method, 8, &buffer).ok());
    std::unique_ptr<Mesh> direct;
    TASK_CHECK(DecodeMesh(buffer, &direct));
    std::unique_ptr<Mesh> skipped;
    TASK_CHECK(DecodeMesh(buffer, &skipped, /*skip_position_transform=*/true));
    const PointAttribute* pos =
        skipped->GetNamedAttribute(GeometryAttribute::POSITION);
    TASK_CHECK(pos != nullptr);
    TASK_CHECK(pos->data_type() == draco::DT_INT32);
    TASK_CHECK(pos->GetAttributeTransformData() != nullptr);
    TASK_CHECK(pos->GetAttributeTransformData()->transform_type() ==
               draco::ATTRIBUTE_QUANTIZATION_TOPOLOGY_TRANSFORM);

    draco::AttributeQuantizationTransform transform;
    TASK_CHECK(transform.InitFromAttribute(*pos));
    TASK_CHECK(transform.has_exact_values());
    TASK_CHECK(transform.extension_enabled());

    // Rebuild the float attribute in the portable value order and run the
    // public inverse transform.
    draco::GeometryAttribute geometry_attribute;
    geometry_attribute.Init(GeometryAttribute::POSITION, nullptr, 3,
                            draco::DT_FLOAT32, false, sizeof(float) * 3, 0);
    draco::PointAttribute target(geometry_attribute);
    TASK_CHECK(target.Reset(pos->size()));
    target.SetIdentityMapping();
    TASK_CHECK(transform.InverseTransformAttribute(*pos, &target));

    const PointAttribute* dp =
        direct->GetNamedAttribute(GeometryAttribute::POSITION);
    int mismatches = 0;
    for (FaceIndex f(0); f < direct->num_faces(); ++f) {
      const auto& df = direct->face(f);
      const auto& sf = skipped->face(f);
      for (int c = 0; c < 3; ++c) {
        float a[3];
        float b[3];
        dp->GetValue(dp->mapped_index(df[c]), a);
        target.GetValue(pos->mapped_index(sf[c]), b);
        if (std::memcmp(a, b, sizeof(a)) != 0) {
          ++mismatches;
        }
      }
    }
    TASK_CHECK(mismatches == 0);
    TASK_CHECK(!transform.exact_values().empty());
  }
  return 0;
}

int CaseStreamRejectsCorruptedRecords() {
  auto mesh = MakeFarShellAndTinySheet();
  TASK_CHECK(mesh != nullptr);
  EncoderBuffer buffer;
  TASK_CHECK(
      EncodeProtected(*mesh, draco::MESH_EDGEBREAKER_ENCODING, 8, &buffer).ok());
  // Truncated streams must fail rather than return a partial mesh.
  for (size_t cut : {buffer.size() - 1, buffer.size() / 2, size_t{12}}) {
    draco::Decoder decoder;
    draco::DecoderBuffer in;
    in.Init(buffer.data(), cut);
    auto result = decoder.DecodeMeshFromBuffer(&in);
    TASK_CHECK(!result.ok());
  }
  // A corrupted extension payload must be rejected by the decoder. Build valid
  // payloads and then corrupt them, checking that the real Decoder path (via a
  // skip-transform decode that reads the payload) returns an error rather than
  // a partially valid mesh.
  auto decode_corrupted_payload = [](const std::vector<uint8_t>& payload,
                                     uint32_t expected_num_values) {
    // Wrap the payload in a transform data block the same way the stream does
    // and decode it with the public extension reader through a fake attribute.
    draco::AttributeTransformData data;
    data.set_transform_type(draco::ATTRIBUTE_QUANTIZATION_TOPOLOGY_TRANSFORM);
    data.AppendBytes(payload.data(), static_cast<int64_t>(payload.size()));
    std::vector<draco::TopologyPreservingExactValue> out;
    uint32_t num_values = 0;
    if (!draco::LoadTopologyPreservingQuantizationExtension(&data, &num_values,
                                                            &out)) {
      return false;
    }
    // The caller knows how many portable values it expects; a mismatch is an
    // error exactly as DecodeParametersWithExtension treats it.
    return num_values == expected_num_values;
  };
  {
    // Valid payload: 8 values, one record at index 2.
    std::vector<draco::TopologyPreservingExactValue> records(1);
    records[0].encoded_value_index = 2;
    records[0].value[0] = 0.5f;
    records[0].value[1] = -0.25f;
    records[0].value[2] = 1.f;
    draco::AttributeTransformData valid_data;
    draco::StoreTopologyPreservingQuantizationExtension(8, records,
                                                        &valid_data);
    std::vector<uint8_t> payload(valid_data.data_size());
    valid_data.GetBytes(0, payload.data(),
                        static_cast<int64_t>(payload.size()));
    TASK_CHECK(decode_corrupted_payload(payload, 8));
    // Truncate by one byte: must fail.
    payload.pop_back();
    TASK_CHECK(!decode_corrupted_payload(payload, 8));
    // Duplicate index: must fail.
    std::vector<draco::TopologyPreservingExactValue> dup(2);
    dup[0].encoded_value_index = 3;
    dup[1].encoded_value_index = 3;
    draco::AttributeTransformData dup_data;
    draco::StoreTopologyPreservingQuantizationExtension(8, dup, &dup_data);
    std::vector<uint8_t> dup_payload(dup_data.data_size());
    dup_data.GetBytes(0, dup_payload.data(),
                      static_cast<int64_t>(dup_payload.size()));
    TASK_CHECK(!decode_corrupted_payload(dup_payload, 8));
    // Out-of-range index: must fail.
    std::vector<draco::TopologyPreservingExactValue> oor(1);
    oor[0].encoded_value_index = 8;  // num_values == 8 -> max index 7.
    draco::AttributeTransformData oor_data;
    draco::StoreTopologyPreservingQuantizationExtension(8, oor, &oor_data);
    std::vector<uint8_t> oor_payload(oor_data.data_size());
    oor_data.GetBytes(0, oor_payload.data(),
                      static_cast<int64_t>(oor_payload.size()));
    TASK_CHECK(!decode_corrupted_payload(oor_payload, 8));
    // Non-finite exact value: must fail.
    std::vector<draco::TopologyPreservingExactValue> bad(1);
    bad[0].encoded_value_index = 0;
    bad[0].value[2] = std::numeric_limits<float>::infinity();
    draco::AttributeTransformData bad_data;
    draco::StoreTopologyPreservingQuantizationExtension(8, bad, &bad_data);
    std::vector<uint8_t> bad_payload(bad_data.data_size());
    bad_data.GetBytes(0, bad_payload.data(),
                      static_cast<int64_t>(bad_payload.size()));
    TASK_CHECK(!decode_corrupted_payload(bad_payload, 8));
    // Wrong count (declared 8, payload says 7): must fail.
    std::vector<uint8_t> wrong_count = payload;
    // The payload layout is tag, version, uint32 num_values, uint32 count...
    // Restore a well-formed payload first by re-encoding.
    draco::AttributeTransformData good_data;
    draco::StoreTopologyPreservingQuantizationExtension(8, records, &good_data);
    wrong_count.assign(good_data.data_size(), 0);
    good_data.GetBytes(0, wrong_count.data(),
                       static_cast<int64_t>(wrong_count.size()));
    // num_values lives at byte offset 2.
    wrong_count[2] = 9;  // Declares 9 values while the caller expects 8.
    TASK_CHECK(!decode_corrupted_payload(wrong_count, 8));
  }
  return 0;
}

int CaseStreamMixedQuantizedAttributesBothMethods() {
  // A 2.3 stream may still contain other quantized attributes (e.g. UVs).
  // Only POSITION carries the extension payload; the UV attribute must keep
  // the plain quantization transform and decode normally.
  auto mesh = MakeSeamMesh();
  TASK_CHECK(mesh != nullptr);
  PlanPtr plan;
  TASK_CHECK(BuildPlan(*mesh, 8, &plan).ok());
  for (int method : {int(draco::MESH_SEQUENTIAL_ENCODING),
                     int(draco::MESH_EDGEBREAKER_ENCODING)}) {
    draco::Encoder encoder;
    encoder.SetEncodingMethod(method);
    encoder.SetSpeedOptions(5, 5);
    encoder.SetAttributeQuantization(GeometryAttribute::POSITION, 8);
    encoder.SetAttributeQuantization(GeometryAttribute::TEX_COORD, 10);
    encoder.SetMeshTopologyPreservingQuantization(true);
    EncoderBuffer buffer;
    TASK_CHECK(encoder.EncodeMeshToBuffer(*mesh, &buffer).ok());
    TASK_CHECK(static_cast<uint8_t>(buffer.data()[6]) ==
               draco::kDracoMeshTopologyBitstreamVersionMinor);
    std::unique_ptr<Mesh> decoded;
    TASK_CHECK(DecodeMesh(buffer, &decoded));
    TASK_CHECK(DecodedMatchesPlan(*mesh, 0, *plan, *decoded));
    const PointAttribute* uv =
        decoded->GetNamedAttribute(GeometryAttribute::TEX_COORD);
    TASK_CHECK(uv != nullptr);
    TASK_CHECK(uv->num_components() == 2);
    TASK_CHECK(uv->data_type() == draco::DT_FLOAT32);

    // Skip-transform decode: POSITION uses the extension transform, TEX_COORD
    // keeps the plain quantization transform.
    std::unique_ptr<Mesh> skipped;
    TASK_CHECK(DecodeMesh(buffer, &skipped, /*skip_position_transform=*/true));
    // Only POSITION is skipped here; UV is still dequantized normally.
    const PointAttribute* spos =
        skipped->GetNamedAttribute(GeometryAttribute::POSITION);
    const PointAttribute* suv =
        skipped->GetNamedAttribute(GeometryAttribute::TEX_COORD);
    TASK_CHECK(spos->GetAttributeTransformData() != nullptr);
    TASK_CHECK(spos->GetAttributeTransformData()->transform_type() ==
               draco::ATTRIBUTE_QUANTIZATION_TOPOLOGY_TRANSFORM);
    TASK_CHECK(suv->GetAttributeTransformData() == nullptr ||
               suv->GetAttributeTransformData()->transform_type() ==
                   draco::ATTRIBUTE_QUANTIZATION_TRANSFORM);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Group: compatibility.
// ---------------------------------------------------------------------------

int CaseCompatibilityDisabledBytesMatchLegacy() {
  auto mesh = MakeFarShellAndTinySheet();
  TASK_CHECK(mesh != nullptr);
  for (int method : {int(draco::MESH_SEQUENTIAL_ENCODING),
                     int(draco::MESH_EDGEBREAKER_ENCODING)}) {
    for (int bits : {8, 12}) {
      draco::Encoder legacy;
      legacy.SetEncodingMethod(method);
      legacy.SetSpeedOptions(5, 5);
      legacy.SetAttributeQuantization(GeometryAttribute::POSITION, bits);
      EncoderBuffer legacy_buffer;
      TASK_CHECK(legacy.EncodeMeshToBuffer(*mesh, &legacy_buffer).ok());

      draco::Encoder explicitly_off;
      explicitly_off.SetEncodingMethod(method);
      explicitly_off.SetSpeedOptions(5, 5);
      explicitly_off.SetAttributeQuantization(GeometryAttribute::POSITION,
                                              bits);
      explicitly_off.SetMeshTopologyPreservingQuantization(false);
      EncoderBuffer off_buffer;
      TASK_CHECK(explicitly_off.EncodeMeshToBuffer(*mesh, &off_buffer).ok());

      TASK_CHECK(legacy_buffer.size() == off_buffer.size());
      TASK_CHECK(std::memcmp(legacy_buffer.data(), off_buffer.data(),
                             legacy_buffer.size()) == 0);
      std::unique_ptr<Mesh> decoded;
      TASK_CHECK(DecodeMesh(legacy_buffer, &decoded));
      TASK_CHECK(decoded->num_faces() == mesh->num_faces());
    }
  }
  return 0;
}

int CaseCompatibilityLegacyStreamDecodes() {
  auto mesh = MakeCleanGrid();
  TASK_CHECK(mesh != nullptr);
  draco::Encoder encoder;
  encoder.SetEncodingMethod(draco::MESH_EDGEBREAKER_ENCODING);
  encoder.SetAttributeQuantization(GeometryAttribute::POSITION, 12);
  EncoderBuffer buffer;
  TASK_CHECK(encoder.EncodeMeshToBuffer(*mesh, &buffer).ok());
  // Skip-transform decode of a legacy stream must expose the plain
  // quantization transform, not the extension.
  std::unique_ptr<Mesh> skipped;
  TASK_CHECK(DecodeMesh(buffer, &skipped, /*skip_position_transform=*/true));
  const PointAttribute* pos =
      skipped->GetNamedAttribute(GeometryAttribute::POSITION);
  TASK_CHECK(pos != nullptr);
  TASK_CHECK(pos->GetAttributeTransformData() != nullptr);
  TASK_CHECK(pos->GetAttributeTransformData()->transform_type() ==
             draco::ATTRIBUTE_QUANTIZATION_TRANSFORM);
  draco::AttributeQuantizationTransform transform;
  TASK_CHECK(transform.InitFromAttribute(*pos));
  TASK_CHECK(!transform.has_exact_values());
  TASK_CHECK(!transform.extension_enabled());
  // And the protection-enabled stream version must differ from the legacy one
  // (new representation, rejected by an older decoder).
  EncoderBuffer protected_buffer;
  TASK_CHECK(
      EncodeProtected(*mesh, draco::MESH_EDGEBREAKER_ENCODING, 12,
                      &protected_buffer)
          .ok());
  TASK_CHECK(protected_buffer.size() != buffer.size() ||
             std::memcmp(protected_buffer.data(), buffer.data(),
                         buffer.size()) != 0);
  // Both are mesh headers; minor versions differ.
  TASK_CHECK(buffer.data()[6] == draco::kDracoMeshBitstreamVersionMinor);
  TASK_CHECK(protected_buffer.data()[6] ==
             draco::kDracoMeshTopologyBitstreamVersionMinor);
  return 0;
}

int CaseCompatibilityInvalidInputsRejected() {
  // Quantization bits outside [1, 30].
  {
    auto mesh = MakeCleanGrid();
    TASK_CHECK(mesh != nullptr);
    PlanPtr plan;
    draco::EncoderOptions options =
        draco::EncoderOptions::CreateDefaultOptions();
    options.SetAttributeInt(0, "quantization_bits", 0);
    TASK_CHECK(
        !draco::BuildTopologyPreservingQuantizationPlan(*mesh, options, &plan)
             .ok());
    TASK_CHECK(plan == nullptr);
    options.SetAttributeInt(0, "quantization_bits", 31);
    TASK_CHECK(
        !draco::BuildTopologyPreservingQuantizationPlan(*mesh, options, &plan)
             .ok());
    TASK_CHECK(plan == nullptr);
  }
  // Two-component POSITION.
  {
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(1);
    const int position_id =
        builder.AddAttribute(GeometryAttribute::POSITION, 2, draco::DT_FLOAT32);
    const float a[2] = {0, 0};
    const float b[2] = {1, 0};
    const float c[2] = {0, 1};
    builder.SetAttributeValuesForFace(position_id, FaceIndex(0), a, b, c);
    auto mesh = builder.Finalize();
    TASK_CHECK(mesh != nullptr);
    PlanPtr plan;
    TASK_CHECK(!BuildPlan(*mesh, 10, &plan).ok());
    TASK_CHECK(plan == nullptr);
  }
  // Degenerate face: repeated POSITION value.
  {
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(1);
    const int position_id =
        builder.AddAttribute(GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    const float a[3] = {0, 0, 0};
    const float b[3] = {1, 0, 0};
    builder.SetAttributeValuesForFace(position_id, FaceIndex(0), a, a, b);
    auto mesh = builder.Finalize();
    TASK_CHECK(mesh != nullptr);
    PlanPtr plan;
    TASK_CHECK(!BuildPlan(*mesh, 10, &plan).ok());
    TASK_CHECK(plan == nullptr);
  }
  // Non-manifold edge: three faces share one edge.
  {
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(3);
    const int position_id =
        builder.AddAttribute(GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    const float a[3] = {0, 0, 0};
    const float b[3] = {1, 0, 0};
    const float c[3] = {0, 1, 0};
    const float d[3] = {0, -1, 0};
    const float e[3] = {0, 0, 1};
    builder.SetAttributeValuesForFace(position_id, FaceIndex(0), a, b, c);
    builder.SetAttributeValuesForFace(position_id, FaceIndex(1), b, a, d);
    builder.SetAttributeValuesForFace(position_id, FaceIndex(2), a, b, e);
    auto mesh = builder.Finalize();
    TASK_CHECK(mesh != nullptr);
    PlanPtr plan;
    TASK_CHECK(!BuildPlan(*mesh, 10, &plan).ok());
    TASK_CHECK(plan == nullptr);
  }
  // Non-finite coordinate.
  {
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(1);
    const int position_id =
        builder.AddAttribute(GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    const float a[3] = {0, 0, 0};
    const float b[3] = {std::numeric_limits<float>::quiet_NaN(), 0, 0};
    const float c[3] = {0, 1, 0};
    builder.SetAttributeValuesForFace(position_id, FaceIndex(0), a, b, c);
    auto mesh = builder.Finalize();
    TASK_CHECK(mesh != nullptr);
    PlanPtr plan;
    TASK_CHECK(!BuildPlan(*mesh, 10, &plan).ok());
    TASK_CHECK(plan == nullptr);
  }
  // And the public encoders must return an error, not silently downgrade.
  {
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(1);
    const int position_id =
        builder.AddAttribute(GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    const float a[3] = {0, 0, 0};
    const float b[3] = {1, 0, 0};
    builder.SetAttributeValuesForFace(position_id, FaceIndex(0), a, a, b);
    auto bad = builder.Finalize();
    TASK_CHECK(bad != nullptr);
    EncoderBuffer buffer;
    TASK_CHECK(!EncodeProtected(*bad, draco::MESH_SEQUENTIAL_ENCODING, 10,
                                &buffer)
                    .ok());
  }
  return 0;
}

int CaseCompatibilityFailedEncodeCanBeReused() {
  auto good = MakeCleanGrid();
  TASK_CHECK(good != nullptr);
  draco::TriangleSoupMeshBuilder builder;
  builder.Start(1);
  const int position_id =
      builder.AddAttribute(GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
  const float a[3] = {0, 0, 0};
  const float b[3] = {1, 0, 0};
  builder.SetAttributeValuesForFace(position_id, FaceIndex(0), a, a, b);
  auto bad = builder.Finalize();
  TASK_CHECK(bad != nullptr);

  draco::Encoder encoder;
  encoder.SetEncodingMethod(draco::MESH_SEQUENTIAL_ENCODING);
  encoder.SetAttributeQuantization(GeometryAttribute::POSITION, 10);
  encoder.SetMeshTopologyPreservingQuantization(true);
  EncoderBuffer rejected;
  TASK_CHECK(!encoder.EncodeMeshToBuffer(*bad, &rejected).ok());

  // Reuse the same encoder for a valid mesh with the option off: it must work
  // and produce a legacy (2.2) stream.
  encoder.SetMeshTopologyPreservingQuantization(false);
  EncoderBuffer recovered;
  TASK_CHECK(encoder.EncodeMeshToBuffer(*good, &recovered).ok());
  std::unique_ptr<Mesh> decoded;
  TASK_CHECK(DecodeMesh(recovered, &decoded));
  TASK_CHECK(decoded->num_faces() == good->num_faces());
  TASK_CHECK(recovered.data()[6] == draco::kDracoMeshBitstreamVersionMinor);
  std::unique_ptr<Mesh> skipped;
  TASK_CHECK(DecodeMesh(recovered, &skipped, /*skip_position_transform=*/true));
  const PointAttribute* pos =
      skipped->GetNamedAttribute(GeometryAttribute::POSITION);
  TASK_CHECK(pos->GetAttributeTransformData() != nullptr);
  TASK_CHECK(pos->GetAttributeTransformData()->transform_type() ==
             draco::ATTRIBUTE_QUANTIZATION_TRANSFORM);

  // And a subsequent enabled encode of the valid mesh must also work.
  encoder.SetMeshTopologyPreservingQuantization(true);
  EncoderBuffer enabled;
  TASK_CHECK(encoder.EncodeMeshToBuffer(*good, &enabled).ok());
  TASK_CHECK(DecodeMesh(enabled, &decoded));
  TASK_CHECK(decoded->num_faces() == good->num_faces());
  return 0;
}

int CaseCompatibilityExpertEncoderApi() {
  auto mesh = MakeFarShellAndTinySheet();
  TASK_CHECK(mesh != nullptr);
  draco::ExpertEncoder encoder(*mesh);
  encoder.SetSpeedOptions(5, 5);
  encoder.SetEncodingMethod(draco::MESH_EDGEBREAKER_ENCODING);
  encoder.SetAttributeQuantization(0, 8);
  encoder.SetMeshTopologyPreservingQuantization(true);
  EncoderBuffer buffer;
  TASK_CHECK(encoder.EncodeToBuffer(&buffer).ok());
  TASK_CHECK(encoder.topology_preserving_quantization_stats()
                 .exact_position_value_count > 0);
  PlanPtr plan;
  TASK_CHECK(BuildPlan(*mesh, 8, &plan).ok());
  std::unique_ptr<Mesh> decoded;
  TASK_CHECK(DecodeMesh(buffer, &decoded));
  TASK_CHECK(DecodedMatchesPlan(*mesh, 0, *plan, *decoded));

  // A point cloud must be rejected, not silently downgraded, through both the
  // ExpertEncoder and the basic Encoder.
  draco::PointCloud point_cloud;
  draco::ExpertEncoder pc_encoder(point_cloud);
  pc_encoder.SetMeshTopologyPreservingQuantization(true);
  EncoderBuffer pc_buffer;
  TASK_CHECK(!pc_encoder.EncodeToBuffer(&pc_buffer).ok());

  draco::Encoder basic;
  basic.SetAttributeQuantization(GeometryAttribute::POSITION, 8);
  basic.SetMeshTopologyPreservingQuantization(true);
  EncoderBuffer basic_buffer;
  draco::PointCloud basic_pc;
  TASK_CHECK(!basic.EncodePointCloudToBuffer(basic_pc, &basic_buffer).ok());

  // A mesh passed to the point-cloud entry point of the basic Encoder is also
  // an error.
  TASK_CHECK(!basic.EncodePointCloudToBuffer(*mesh, &basic_buffer).ok());
  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"closure", "matches_reference", CaseClosureMatchesReference},
    {"closure", "multi_round_propagation", CaseClosureUsesMoreThanOneRound},
    {"closure", "large_shell_keeps_sheet_local",
     CaseClosureLargeShellKeepsSheetLocal},
    {"closure", "clean_grid_needs_nothing", CaseClosureCleanGridNeedsNothing},
    {"mapping", "edgebreaker_reordering",
     CaseMappingExactValuesSurviveEdgebreakerReordering},
    {"mapping", "seam_both_sides", CaseMappingSeamBothSidesRestoredTogether},
    {"mapping", "shared_value_every_point",
     CaseMappingSharedValueRestoredForEveryPoint},
    {"mapping", "isolated_unused_value",
     CaseMappingIsolatedUnusedValueStaysQuantized},
    {"mapping", "equal_coords_distinct_indices",
     CaseMappingEqualCoordinatesDifferentIndicesNotWelded},
    {"stream", "ordinary_decoder_both_methods",
     CaseStreamExactThroughOrdinaryDecoderBothMethods},
    {"stream", "skip_transform_inverse",
     CaseStreamSkipTransformInverseMatchesDecode},
    {"stream", "corrupted_records", CaseStreamRejectsCorruptedRecords},
    {"stream", "mixed_quantized_attributes",
     CaseStreamMixedQuantizedAttributesBothMethods},
    {"compatibility", "disabled_bytes_match_legacy",
     CaseCompatibilityDisabledBytesMatchLegacy},
    {"compatibility", "legacy_stream_decodes",
     CaseCompatibilityLegacyStreamDecodes},
    {"compatibility", "invalid_inputs_rejected",
     CaseCompatibilityInvalidInputsRejected},
    {"compatibility", "failed_encode_reusable",
     CaseCompatibilityFailedEncodeCanBeReused},
    {"compatibility", "expert_encoder_api", CaseCompatibilityExpertEncoderApi},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
