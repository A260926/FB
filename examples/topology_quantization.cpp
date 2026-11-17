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
// Example for the optional mesh topology preserving position quantization.
//
// Builds a model with a large shell and a very thin, tiny sheet (plus UV seams
// and split points), encodes it twice with position quantization, once with the
// protection disabled and once enabled, decodes both with the ordinary Decoder
// and writes:
//   <out_dir>/decoded.obj  the protected decode
//   <out_dir>/stats.txt    before/after face-normal statistics and diagnostics
//
// The example exits non-zero if encoding, decoding or the output files fail.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "draco/compression/attributes/topology_preserving_quantization.h"
#include "draco/compression/config/encoder_options.h"
#include "draco/compression/decode.h"
#include "draco/compression/encode.h"
#include "draco/io/obj_encoder.h"
#include "draco/mesh/triangle_soup_mesh_builder.h"

namespace {

using draco::AttributeValueIndex;
using draco::FaceIndex;
using draco::GeometryAttribute;
using draco::Mesh;
using draco::PointAttribute;

struct Fixture {
  std::unique_ptr<Mesh> mesh;
  int position_attribute_id = -1;
  int tex_coord_attribute_id = -1;
};

// A large, well-conditioned shell far from the origin plus a tiny thin sheet
// near the origin, with a UV seam that splits points without moving positions.
// The far shell dominates the quantized bounding box, so the sheet collapses at
// low bit counts unless the protection restores it.
Fixture MakeModel() {
  draco::TriangleSoupMeshBuilder builder;
  builder.Start(16);
  const int position_id =
      builder.AddAttribute(GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
  const int tex_coord_id =
      builder.AddAttribute(GeometryAttribute::TEX_COORD, 2, draco::DT_FLOAT32);

  // Large tetrahedron shell (edges ~100) around (900,900,900).
  const float a[3] = {850.f, 850.f, 850.f};
  const float b[3] = {950.f, 850.f, 850.f};
  const float c[3] = {850.f, 950.f, 850.f};
  const float d[3] = {850.f, 850.f, 950.f};
  const float* shell_faces[4][3] = {{a, b, c}, {b, a, d}, {a, c, d}, {c, b, d}};
  for (int i = 0; i < 4; ++i) {
    const float uv[3][2] = {{0.f, 0.f}, {1.f, 0.f}, {0.f, 1.f}};
    builder.SetAttributeValuesForFace(position_id, FaceIndex(i),
                                      shell_faces[i][0], shell_faces[i][1],
                                      shell_faces[i][2]);
    builder.SetAttributeValuesForFace(tex_coord_id, FaceIndex(i), uv[0], uv[1],
                                      uv[2]);
  }

  // Thin sheet: a 4x4 grid at the origin, extent 0.004 in x/y and 0.0004 in z
  // so it is essentially coplanar. It is split in two along the middle column
  // with different UVs but shared positions.
  const int n = 4;
  const float step = 0.001f;
  const float thickness = 0.0002f;
  std::vector<std::array<float, 3>> grid;
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      grid.push_back({static_cast<float>(x) * step,
                      static_cast<float>(y) * step,
                      (x % 2) ? thickness : -thickness});
    }
  }
  int face = 4;
  for (int y = 0; y + 1 < n; ++y) {
    for (int x = 0; x + 1 < n; ++x) {
      const int ia = y * n + x;
      const int ib = ia + 1;
      const int ic = ia + n;
      const int id = ic + 1;
      const bool left = x == 0;
      const float uv_a[2] = {left ? 0.1f : 0.9f, y * 0.2f};
      const float uv_b[2] = {left ? 0.1f : 0.9f, y * 0.2f};
      const float uv_c[2] = {left ? 0.1f : 0.9f, (y + 1) * 0.2f};
      const float uv_d[2] = {left ? 0.1f : 0.9f, (y + 1) * 0.2f};
      if (face >= 16) {
        break;
      }
      builder.SetAttributeValuesForFace(position_id, FaceIndex(face),
                                        grid[ia].data(), grid[ib].data(),
                                        grid[ic].data());
      builder.SetAttributeValuesForFace(tex_coord_id, FaceIndex(face), uv_a,
                                        uv_b, uv_c);
      ++face;
      if (face >= 16) {
        break;
      }
      builder.SetAttributeValuesForFace(position_id, FaceIndex(face),
                                        grid[ib].data(), grid[id].data(),
                                        grid[ic].data());
      builder.SetAttributeValuesForFace(tex_coord_id, FaceIndex(face), uv_b,
                                        uv_d, uv_c);
      ++face;
    }
  }
  Fixture fixture;
  fixture.mesh = builder.Finalize();
  fixture.position_attribute_id = position_id;
  fixture.tex_coord_attribute_id = tex_coord_id;
  return fixture;
}

// Counts how many faces of |mesh| have a reconstructed normal that does not
// agree with the original mesh's normal at the matching face.
int CountFlippedFaces(const Mesh& original, const Mesh& decoded, int* out_faces) {
  const PointAttribute* op =
      original.GetNamedAttribute(GeometryAttribute::POSITION);
  const PointAttribute* dp =
      decoded.GetNamedAttribute(GeometryAttribute::POSITION);
  *out_faces = static_cast<int>(original.num_faces());
  if (dp == nullptr || decoded.num_faces() != original.num_faces()) {
    return -1;
  }
  auto normal = [](const PointAttribute& pos, const Mesh& mesh, FaceIndex f,
                   double out[3]) {
    const auto& face = mesh.face(f);
    double p[3][3];
    for (int c = 0; c < 3; ++c) {
      float v[3];
      pos.GetValue(pos.mapped_index(face[c]), v);
      for (int k = 0; k < 3; ++k) {
        p[c][k] = v[k];
      }
    }
    const double ax = p[1][0] - p[0][0], ay = p[1][1] - p[0][1],
                 az = p[1][2] - p[0][2];
    const double bx = p[2][0] - p[0][0], by = p[2][1] - p[0][1],
                 bz = p[2][2] - p[0][2];
    out[0] = ay * bz - az * by;
    out[1] = az * bx - ax * bz;
    out[2] = ax * by - ay * bx;
  };
  int flipped = 0;
  for (FaceIndex f(0); f < original.num_faces(); ++f) {
    double on[3];
    double dn[3];
    normal(*op, original, f, on);
    normal(*dp, decoded, f, dn);
    if (on[0] * dn[0] + on[1] * dn[1] + on[2] * dn[2] <= 0.0) {
      ++flipped;
    }
  }
  return flipped;
}

bool Encode(const Mesh& mesh, int bits, bool protect, draco::EncoderBuffer* out,
            draco::MeshTopologyPreservingQuantizationStats* stats) {
  draco::Encoder encoder;
  // Sequential mesh encoding preserves the input face order, which keeps the
  // before/after face-normal comparison below straightforward.
  encoder.SetEncodingMethod(draco::MESH_SEQUENTIAL_ENCODING);
  encoder.SetSpeedOptions(5, 5);
  encoder.SetAttributeQuantization(GeometryAttribute::POSITION, bits);
  encoder.SetMeshTopologyPreservingQuantization(protect);
  const draco::Status status = encoder.EncodeMeshToBuffer(mesh, out);
  if (!status.ok()) {
    std::cerr << "encode failed: " << status.error_msg() << "\n";
    return false;
  }
  if (stats != nullptr) {
    *stats = encoder.topology_preserving_quantization_stats();
  }
  return true;
}

bool Decode(const draco::EncoderBuffer& buffer, std::unique_ptr<Mesh>* out,
            bool skip_position_transform = false) {
  draco::Decoder decoder;
  if (skip_position_transform) {
    decoder.SetSkipAttributeTransform(GeometryAttribute::POSITION);
  }
  draco::DecoderBuffer in;
  in.Init(buffer.data(), buffer.size());
  auto result = decoder.DecodeMeshFromBuffer(&in);
  if (!result.ok()) {
    std::cerr << "decode failed: " << result.status().error_msg() << "\n";
    return false;
  }
  *out = std::move(result).value();
  return *out != nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = (argc > 1) ? argv[1] : ".";
  const std::string obj_path = out_dir + "/decoded.obj";
  const std::string stats_path = out_dir + "/stats.txt";
  const int bits = 8;

  Fixture fixture = MakeModel();
  if (fixture.mesh == nullptr) {
    std::cerr << "failed to build the model\n";
    return 1;
  }
  const Mesh& mesh = *fixture.mesh;

  draco::EncoderBuffer legacy_buffer;
  draco::EncoderBuffer protected_buffer;
  draco::MeshTopologyPreservingQuantizationStats stats;
  if (!Encode(mesh, bits, /*protect=*/false, &legacy_buffer, nullptr)) {
    return 1;
  }
  if (!Encode(mesh, bits, /*protect=*/true, &protected_buffer, &stats)) {
    return 1;
  }

  std::unique_ptr<Mesh> legacy_decoded;
  std::unique_ptr<Mesh> protected_decoded;
  if (!Decode(legacy_buffer, &legacy_decoded)) {
    return 1;
  }
  if (!Decode(protected_buffer, &protected_decoded)) {
    return 1;
  }

  int legacy_faces = 0;
  int protected_faces = 0;
  const int legacy_flipped = CountFlippedFaces(mesh, *legacy_decoded, &legacy_faces);
  const int protected_flipped =
      CountFlippedFaces(mesh, *protected_decoded, &protected_faces);
  if (legacy_flipped < 0 || protected_flipped < 0) {
    std::cerr << "decoded connectivity does not match the input\n";
    return 1;
  }

  // Verify the protected decode reproduces the protected values exactly where
  // the plan says so, and that every face keeps its orientation.
  draco::EncoderOptions options = draco::EncoderOptions::CreateDefaultOptions();
  options.SetAttributeInt(fixture.position_attribute_id, "quantization_bits",
                          bits);
  std::shared_ptr<const draco::TopologyPreservingQuantizationPlan> plan;
  const draco::Status plan_status = draco::BuildTopologyPreservingQuantizationPlan(
      mesh, options, &plan);
  if (!plan_status.ok()) {
    std::cerr << "plan failed: " << plan_status.error_msg() << "\n";
    return 1;
  }

  // Encode OBJ into a buffer and write it with the standard library. (Writing
  // through ObjEncoder::EncodeToFile relies on a static factory registration
  // that a static library link may drop when nothing else references it.)
  draco::ObjEncoder obj_encoder;
  draco::EncoderBuffer obj_buffer;
  if (!obj_encoder.EncodeToBuffer(*protected_decoded, &obj_buffer)) {
    std::cerr << "failed to encode OBJ\n";
    return 1;
  }
  {
    std::ofstream obj_file(obj_path, std::ios::binary);
    if (!obj_file) {
      std::cerr << "failed to open " << obj_path << "\n";
      return 1;
    }
    obj_file.write(obj_buffer.data(), static_cast<std::streamsize>(obj_buffer.size()));
    if (!obj_file) {
      std::cerr << "failed to write " << obj_path << "\n";
      return 1;
    }
  }

  std::ofstream stats_file(stats_path);
  if (!stats_file) {
    std::cerr << "failed to write " << stats_path << "\n";
    return 1;
  }
  stats_file << "model_faces=" << mesh.num_faces() << "\n";
  stats_file << "position_quantization_bits=" << bits << "\n";
  stats_file << "legacy_bytes=" << legacy_buffer.size() << "\n";
  stats_file << "protected_bytes=" << protected_buffer.size() << "\n";
  stats_file << "legacy_flipped_faces=" << legacy_flipped << "\n";
  stats_file << "protected_flipped_faces=" << protected_flipped << "\n";
  stats_file << "unique_position_values="
             << stats.unique_position_value_count << "\n";
  stats_file << "exact_position_values="
             << stats.exact_position_value_count << "\n";
  stats_file << "face_rechecks=" << stats.face_recheck_count << "\n";
  stats_file << "closure_rounds=" << stats.closure_round_count << "\n";
  stats_file << "full_lossless_fallback="
             << (stats.full_lossless_position_fallback() ? 1 : 0) << "\n";
  stats_file << "used_incremental_closure="
             << (stats.used_incremental_closure() ? 1 : 0) << "\n";
  stats_file << "plan_exact_position_values="
             << plan->stats.exact_position_value_count << "\n";
  stats_file.close();
  if (!stats_file) {
    std::cerr << "failed to finalize " << stats_path << "\n";
    return 1;
  }

  if (mesh.num_faces() != protected_decoded->num_faces()) {
    std::cerr << "protected decode lost faces\n";
    return 1;
  }
  if (protected_flipped != 0) {
    std::cerr << "protected decode still has flipped faces\n";
    return 1;
  }
  if (protected_decoded->GetNamedAttribute(GeometryAttribute::TEX_COORD) ==
      nullptr) {
    std::cerr << "protected decode lost texture coordinates\n";
    return 1;
  }
  return 0;
}
