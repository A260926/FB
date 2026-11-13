//----------------------------------------------------------------------------//
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//----------------------------------------------------------------------------//
//
// Dual quaternion skinning example.
//
// Generates a small (5 joints) character procedurally, with non identity
// inverse bind matrices and a compact joint palette remap. The final skinning
// matrices are obtained through the complete animation pipeline:
//   SkeletonBuilder -> AnimationBuilder -> SamplingJob -> LocalToModelJob
// then the mesh vertices are skinned with the linear blend mode (historical
// SkinningJob behavior) and with the new dual quaternion mode.
//
// Vertex positions of both modes are printed as CSV on stdout. The example also
// validates the *final vertices*: dual quaternion skinning must produce a rigid
// deformation (pairwise vertex distances are preserved), which is not the case
// for linear blend skinning when a large twist is applied. It returns a
// non-zero status if a check fails.
//
// This example doesn't open any window.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/transform.h"
#include "ozz/base/maths/vec_float.h"
#include "ozz/base/memory/unique_ptr.h"
#include "ozz/base/span.h"
#include "ozz/geometry/runtime/skinning_job.h"

namespace {

const int kNumJoints = 5;

// A single vertex influenced by the compact palette.
struct Vertex {
  float position[3];
  float normal[3];
  float tangent[3];
  uint16_t indices[2];
  float weights[1];  // The second weight is restored as 1 - weights[0].
};

// Returns the local translation of joint _j in the bind pose.
ozz::math::Float3 BindTranslation(int _j) {
  // Joints are a vertical chain, each one 1 unit above its parent.
  switch (_j) {
    case 0:
      return ozz::math::Float3(0.f, 0.f, 0.f);
    default:
      return ozz::math::Float3(0.f, 1.f, 0.f);
  }
}

// Root bind rotation, non identity so that inverse bind matrices carry both a
// rotation and a translation.
ozz::math::Quaternion BindRootRotation() {
  return ozz::math::Quaternion::FromAxisAngle(ozz::math::Float3::z_axis(),
                                              0.35f);
}

// Returns the bind pose scale of joint _j. Joints 1 and 2 have a non unit,
// but uniform, scale so that their inverse bind matrices are not simple rigid
// transforms either (inverse bind = rotation * inverse scale * inverse
// translation). The skinning matrices built from them stay rigid because the
// animation reproduces the same scale, but the inverse bind matrices are
// themselves non unit.
ozz::math::Float3 BindScale(int _j) {
  if (_j == 1) {
    return ozz::math::Float3(1.5f, 1.5f, 1.5f);
  }
  if (_j == 2) {
    return ozz::math::Float3(0.75f, 0.75f, 0.75f);
  }
  return ozz::math::Float3::one();
}

// Builds the model space bind matrix of joint _j.
ozz::math::Float4x4 BindModel(int _j) {
  using namespace ozz::math;
  Float4x4 model = Float4x4::FromQuaternion(BindRootRotation());
  for (int i = 1; i <= _j; ++i) {
    model = model * Float4x4::Translation(BindTranslation(i)) *
            Float4x4::Scaling(BindScale(i));
  }
  return model;
}

// Builds the RawSkeleton matching the bind pose.
ozz::animation::offline::RawSkeleton BuildRawSkeleton() {
  using ozz::animation::offline::RawSkeleton;
  using ozz::math::Quaternion;
  using ozz::math::Float3;
  RawSkeleton raw;
  raw.roots.resize(1);
  RawSkeleton::Joint* current = &raw.roots[0];
  for (int j = 0; j < kNumJoints; ++j) {
    current->name = j == 0 ? "root" : "joint";
    current->transform.translation = BindTranslation(j);
    current->transform.rotation =
        j == 0 ? BindRootRotation() : Quaternion::identity();
    current->transform.scale = BindScale(j);
    if (j + 1 < kNumJoints) {
      current->children.resize(1);
      current = &current->children[0];
    }
  }
  return raw;
}

// Builds a 1s animation applying a large twist to the last two joints of the
// chain, so that linear blend skinning squashes the mesh cross-section while
// dual quaternion skinning preserves it.
ozz::animation::offline::RawAnimation BuildRawAnimation() {
  using namespace ozz;
  using namespace ozz::animation::offline;
  const float kPi = 3.14159265358979323846f;
  const float kTwist = 100.f * kPi / 180.f;

  RawAnimation raw;
  raw.duration = 1.f;
  raw.tracks.resize(kNumJoints);

  for (int j = 0; j < kNumJoints; ++j) {
    RawAnimation::JointTrack& track = raw.tracks[j];

    // Local translations are constant and match the bind pose.
    RawAnimation::TranslationKey translation;
    translation.time = 0.f;
    translation.value = BindTranslation(j);
    track.translations.push_back(translation);

    // Rotations: the root keeps its bind rotation, joints 2 and 3 twist in
    // opposite directions around the limb axis (y).
    if (j == 0) {
      RawAnimation::RotationKey rotation;
      rotation.time = 0.f;
      rotation.value = BindRootRotation();
      track.rotations.push_back(rotation);
    } else if (j == 2 || j == 3) {
      const float sign = j == 2 ? 1.f : -1.f;
      RawAnimation::RotationKey start;
      start.time = 0.f;
      start.value = math::Quaternion::identity();
      track.rotations.push_back(start);

      RawAnimation::RotationKey end;
      end.time = 1.f;
      end.value = math::Quaternion::FromAxisAngle(math::Float3::y_axis(),
                                                  sign * kTwist);
      track.rotations.push_back(end);
    } else {
      RawAnimation::RotationKey rotation;
      rotation.time = 0.f;
      rotation.value = math::Quaternion::identity();
      track.rotations.push_back(rotation);
    }

    // Scales are constant and reproduce the bind pose scale. Sampling identity
    // instead would make skinning matrices non rigid (the inverse bind pose
    // carries the bind scale) and force the linear fallback, which isn't what
    // this example demonstrates.
    RawAnimation::ScaleKey scale;
    scale.time = 0.f;
    scale.value = BindScale(j);
    track.scales.push_back(scale);
  }
  return raw;
}

// Builds the procedural mesh: three rings of vertices around the twisted part of
// the limb. Only joints 2 and 3 of the skeleton are used, through a compact
// palette of 2 matrices.
std::vector<Vertex> BuildMesh() {
  const int kRingVertices = 8;
  const float kRadius = 0.5f;
  const float kPi = 3.14159265358979323846f;
  const float kRingHeights[3] = {2.2f, 2.5f, 2.8f};
  const float kSecondWeights[3] = {0.2f, 0.5f, 0.8f};
  // Palette ordering: entry 0 is skeleton joint 2, entry 1 is joint 3.
  const uint16_t kPaletteJoint[kNumJoints] = {0, 0, 0, 1, 1};

  std::vector<Vertex> vertices;
  for (int ring = 0; ring < 3; ++ring) {
    for (int i = 0; i < kRingVertices; ++i) {
      const float angle =
          static_cast<float>(i) * 2.f * kPi / static_cast<float>(kRingVertices);
      const float c = std::cos(angle);
      const float s = std::sin(angle);
      Vertex vertex;
      vertex.position[0] = kRadius * c;
      vertex.position[1] = kRingHeights[ring];
      vertex.position[2] = kRadius * s;
      vertex.normal[0] = c;
      vertex.normal[1] = 0.f;
      vertex.normal[2] = s;
      vertex.tangent[0] = -s;
      vertex.tangent[1] = 0.f;
      vertex.tangent[2] = c;
      vertex.indices[0] = kPaletteJoint[2];
      vertex.indices[1] = kPaletteJoint[3];
      vertex.weights[0] = 1.f - kSecondWeights[ring];
      vertices.push_back(vertex);
    }
  }
  return vertices;
}

bool CheckClose(float _actual, float _expected, float _tolerance,
                const char* _what) {
  const float error = std::fabs(_actual - _expected);
  if (!(error <= _tolerance)) {
    std::fprintf(stderr, "check failed: %s actual=%g expected=%g error=%g\n",
                 _what, static_cast<double>(_actual),
                 static_cast<double>(_expected), static_cast<double>(error));
    return false;
  }
  return true;
}

}  // namespace

int main() {
  using namespace ozz;

  // -------------------------------------------------------------------------
  // Builds the runtime skeleton.
  // -------------------------------------------------------------------------
  const animation::offline::RawSkeleton raw_skeleton = BuildRawSkeleton();
  if (!raw_skeleton.Validate()) {
    std::fprintf(stderr, "Invalid raw skeleton.\n");
    return EXIT_FAILURE;
  }
  animation::offline::SkeletonBuilder skeleton_builder;
  unique_ptr<animation::Skeleton> skeleton = skeleton_builder(raw_skeleton);
  if (!skeleton) {
    std::fprintf(stderr, "Failed to build skeleton.\n");
    return EXIT_FAILURE;
  }
  const int num_joints = skeleton->num_joints();
  if (num_joints != kNumJoints) {
    std::fprintf(stderr, "Unexpected joint count.\n");
    return EXIT_FAILURE;
  }

  // -------------------------------------------------------------------------
  // Builds and samples the animation.
  // -------------------------------------------------------------------------
  const animation::offline::RawAnimation raw_animation = BuildRawAnimation();
  if (!raw_animation.Validate()) {
    std::fprintf(stderr, "Invalid raw animation.\n");
    return EXIT_FAILURE;
  }
  animation::offline::AnimationBuilder animation_builder;
  unique_ptr<animation::Animation> animation =
      animation_builder(raw_animation);
  if (!animation) {
    std::fprintf(stderr, "Failed to build animation.\n");
    return EXIT_FAILURE;
  }
  if (animation->num_tracks() != num_joints) {
    std::fprintf(stderr, "Animation/skeleton joint count mismatch.\n");
    return EXIT_FAILURE;
  }

  const int num_soa_joints = skeleton->num_soa_joints();
  ozz::vector<math::SoaTransform> locals(num_soa_joints);
  animation::SamplingJob::Context context(num_joints);
  animation::SamplingJob sampling_job;
  sampling_job.animation = animation.get();
  sampling_job.context = &context;
  sampling_job.ratio = 1.f;  // End of the twist.
  sampling_job.output = make_span(locals);
  if (!sampling_job.Run()) {
    std::fprintf(stderr, "Sampling failed.\n");
    return EXIT_FAILURE;
  }

  ozz::vector<math::Float4x4> models(num_joints);
  animation::LocalToModelJob ltm_job;
  ltm_job.skeleton = skeleton.get();
  ltm_job.input = make_span(locals);
  ltm_job.output = make_span(models);
  if (!ltm_job.Run()) {
    std::fprintf(stderr, "Local to model failed.\n");
    return EXIT_FAILURE;
  }

  // -------------------------------------------------------------------------
  // Builds the compact skinning palette.
  // The palette remapping table maps compact palette entries to skeleton joint
  // indices. The mesh only references 2 of the 5 joints (a compact palette),
  // so it never needs a 5 matrix palette. For each palette entry, the skinning
  // matrix is the model space transform multiplied by the inverse bind
  // transformation of the corresponding skeleton joint:
  //   skinning[palette] = models[joint_remaps[palette]] * inverse_bind[joint]
  // -------------------------------------------------------------------------
  const int kPaletteSize = 2;
  uint16_t joint_remaps[kPaletteSize] = {2, 3};
  ozz::vector<math::Float4x4> skinning_matrices(kPaletteSize);
  for (int p = 0; p < kPaletteSize; ++p) {
    const int joint = joint_remaps[p];
    const math::Float4x4 inverse_bind = math::Invert(BindModel(joint));
    skinning_matrices[p] = models[joint] * inverse_bind;
  }

  // -------------------------------------------------------------------------
  // Skins the mesh with both modes.
  // -------------------------------------------------------------------------
  std::vector<Vertex> linear_vertices = BuildMesh();
  std::vector<Vertex> dual_vertices = linear_vertices;
  const int vertex_count = static_cast<int>(dual_vertices.size());
  std::vector<float> linear_positions(static_cast<size_t>(vertex_count) * 3);
  std::vector<float> dual_positions(static_cast<size_t>(vertex_count) * 3);

  auto make_job = [&](std::vector<Vertex>& _vertices, float* _out_positions,
                      geometry::SkinningJob::SkinningMode _mode) {
    // Interleaved input buffers: the span has to cover the last vertex's
    // attribute, which ends before the end of the vertex structure. Span sizes
    // are expressed in elements, not bytes.
    const size_t float_span =
        (sizeof(Vertex) * (vertex_count - 1) + sizeof(float) * 3) /
        sizeof(float);
    const size_t indices_span =
        (sizeof(Vertex) * (vertex_count - 1) + sizeof(uint16_t) * 2) /
        sizeof(uint16_t);
    const size_t weights_span =
        (sizeof(Vertex) * (vertex_count - 1) + sizeof(float)) / sizeof(float);

    geometry::SkinningJob job;
    job.vertex_count = vertex_count;
    job.mode = _mode;
    job.influences_count = 2;
    job.joint_matrices = make_span(skinning_matrices);
    job.joint_indices = {_vertices.data()->indices, indices_span};
    job.joint_indices_stride = sizeof(Vertex);
    job.joint_weights = {_vertices.data()->weights, weights_span};
    job.joint_weights_stride = sizeof(Vertex);
    job.in_positions = {_vertices.data()->position, float_span};
    job.in_positions_stride = sizeof(Vertex);
    job.out_positions = {_out_positions, static_cast<size_t>(vertex_count) * 3};
    job.out_positions_stride = sizeof(float) * 3;
    job.in_normals = {_vertices.data()->normal, float_span};
    job.in_normals_stride = sizeof(Vertex);
    job.out_normals = {_vertices.data()->normal, float_span};
    job.out_normals_stride = sizeof(Vertex);
    job.in_tangents = {_vertices.data()->tangent, float_span};
    job.in_tangents_stride = sizeof(Vertex);
    job.out_tangents = {_vertices.data()->tangent, float_span};
    job.out_tangents_stride = sizeof(Vertex);
    return job;
  };

  geometry::SkinningJob linear_job =
      make_job(linear_vertices, linear_positions.data(),
               geometry::SkinningJob::kLinear);
  if (!linear_job.Run()) {
    std::fprintf(stderr, "Linear skinning failed.\n");
    return EXIT_FAILURE;
  }
  // In place output: positions are written back into the dual_vertices buffer
  // through a separate tightly packed destination to keep the CSV readable.
  geometry::SkinningJob dual_job =
      make_job(dual_vertices, dual_positions.data(),
               geometry::SkinningJob::kDualQuaternion);
  if (!dual_job.Run()) {
    std::fprintf(stderr, "Dual quaternion skinning failed.\n");
    return EXIT_FAILURE;
  }

  // -------------------------------------------------------------------------
  // Checks the final vertices.
  // -------------------------------------------------------------------------
  bool success = true;

  const std::vector<Vertex> rest_vertices = BuildMesh();

  // 1. All outputs must be finite.
  for (int v = 0; v < vertex_count; ++v) {
    for (int i = 0; i < 3; ++i) {
      if (!std::isfinite(dual_positions[v * 3 + i]) ||
          !std::isfinite(linear_positions[v * 3 + i])) {
        std::fprintf(stderr, "Non finite vertex %d.\n", v);
        success = false;
      }
    }
  }

  // 2. Dual quaternion skinning is a rigid deformation of every ring: all the
  // vertices of a ring share the same weights, hence the same dual quaternion,
  // so pairwise distances within a ring must match the rest pose distances.
  // This is a direct check on the final skinned positions, not on joint
  // matrices.
  const int kRingVertices = 8;
  const int kRingCount = vertex_count / kRingVertices;
  for (int ring = 0; ring < kRingCount && success; ++ring) {
    const int ring_begin = ring * kRingVertices;
    const int ring_end = ring_begin + kRingVertices;
    for (int a = ring_begin; a < ring_end && success; ++a) {
      for (int b = a + 1; b < ring_end; ++b) {
        float rest_delta[3];
        float dual_delta[3];
        for (int i = 0; i < 3; ++i) {
          rest_delta[i] = rest_vertices[a].position[i] -
                          rest_vertices[b].position[i];
          dual_delta[i] =
              dual_positions[a * 3 + i] - dual_positions[b * 3 + i];
        }
        const float rest = std::sqrt(rest_delta[0] * rest_delta[0] +
                                     rest_delta[1] * rest_delta[1] +
                                     rest_delta[2] * rest_delta[2]);
        const float dual = std::sqrt(dual_delta[0] * dual_delta[0] +
                                     dual_delta[1] * dual_delta[1] +
                                     dual_delta[2] * dual_delta[2]);
        if (!CheckClose(dual, rest, 1e-3f, "dual quaternion rigidity")) {
          success = false;
          break;
        }
      }
    }
  }

  // 3. The twist must actually squash the linear blend cross-section, otherwise
  // the example wouldn't demonstrate anything. Compare the distance between two
  // opposite vertices of the middle ring (indices 8 and 12).
  {
    const int a = 8;
    const int b = 12;
    float linear_delta[3];
    float rest_delta[3];
    for (int i = 0; i < 3; ++i) {
      linear_delta[i] =
          linear_positions[a * 3 + i] - linear_positions[b * 3 + i];
      rest_delta[i] =
          rest_vertices[a].position[i] - rest_vertices[b].position[i];
    }
    const float linear = std::sqrt(linear_delta[0] * linear_delta[0] +
                                   linear_delta[1] * linear_delta[1] +
                                   linear_delta[2] * linear_delta[2]);
    const float rest = std::sqrt(rest_delta[0] * rest_delta[0] +
                                 rest_delta[1] * rest_delta[1] +
                                 rest_delta[2] * rest_delta[2]);
    if (!(linear < 0.75f * rest)) {
      std::fprintf(stderr,
                   "Expected linear blend skinning to squash the cross-section "
                   "(linear=%g rest=%g).\n",
                   static_cast<double>(linear), static_cast<double>(rest));
      success = false;
    }
  }

  // 4. Normals and tangents keep their length in dual quaternion mode.
  for (int v = 0; v < vertex_count; ++v) {
    const float* n = dual_vertices[v].normal;
    const float* t = dual_vertices[v].tangent;
    const float n_len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    const float t_len = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    if (!CheckClose(n_len, 1.f, 1e-3f, "dual normal length") ||
        !CheckClose(t_len, 1.f, 1e-3f, "dual tangent length")) {
      success = false;
    }
  }

  // -------------------------------------------------------------------------
  // Prints both positions as CSV.
  // -------------------------------------------------------------------------
  std::printf("# dual quaternion skinning example\n");
  std::printf("# %d joints, %d palette entries, %d vertices\n", num_joints,
              kPaletteSize, vertex_count);
  std::printf("# linear_vertex,linear_x,linear_y,linear_z\n");
  for (int v = 0; v < vertex_count; ++v) {
    std::printf("%d,%.6f,%.6f,%.6f\n", v,
                static_cast<double>(linear_positions[v * 3 + 0]),
                static_cast<double>(linear_positions[v * 3 + 1]),
                static_cast<double>(linear_positions[v * 3 + 2]));
  }
  std::printf("# dual_quaternion_vertex,dual_x,dual_y,dual_z\n");
  for (int v = 0; v < vertex_count; ++v) {
    std::printf("%d,%.6f,%.6f,%.6f\n", v,
                static_cast<double>(dual_positions[v * 3 + 0]),
                static_cast<double>(dual_positions[v * 3 + 1]),
                static_cast<double>(dual_positions[v * 3 + 2]));
  }

  if (!success) {
    std::fprintf(stderr, "OZZ_DQ_EXAMPLE_FAILED\n");
    return EXIT_FAILURE;
  }
  std::printf("# OZZ_DQ_EXAMPLE_OK\n");
  return EXIT_SUCCESS;
}
