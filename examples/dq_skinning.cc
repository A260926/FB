// No-window dual quaternion skinning example.
//
// Builds a five-joint character and an animation entirely in memory, samples
// and evaluates the animation, computes non-identity inverse-bind matrices,
// remaps a compact skinning palette, and skins final vertices with both the
// linear and dual quaternion modes.  Output is position CSV on stdout.

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/maths/quaternion.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/maths/vec_float.h>
#include <ozz/base/span.h>
#include <ozz/geometry/runtime/skinning_job.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr int kJointCount = 5;
constexpr int kVertexCount = 6;
constexpr int kInfluenceCount = 4;
constexpr int kPaletteSize = 4;

bool RunPositions(
    ozz::geometry::SkinningJob::SkinningMode mode,
    const ozz::span<const ozz::math::Float4x4> matrices,
    const uint16_t* indices, size_t indices_stride, const float* weights,
    size_t weights_stride, const float* positions, float* output) {
  ozz::geometry::SkinningJob job;
  job.vertex_count = kVertexCount;
  job.influences_count = kInfluenceCount;
  job.skinning_mode = mode;
  job.joint_matrices = matrices;
  job.joint_indices = ozz::span<const uint16_t>(
      indices, kVertexCount * kInfluenceCount);
  job.joint_indices_stride = indices_stride;
  job.joint_weights =
      ozz::span<const float>(weights, kVertexCount * (kInfluenceCount - 1));
  job.joint_weights_stride = weights_stride;
  job.in_positions = ozz::span<const float>(positions, kVertexCount * 3);
  job.in_positions_stride = 3 * sizeof(float);
  job.out_positions = ozz::span<float>(output, kVertexCount * 3);
  job.out_positions_stride = 3 * sizeof(float);
  return job.Run();
}

}  // namespace

int main() {
  // Program-generated five-joint chain.
  ozz::animation::offline::RawSkeleton raw_skeleton;
  raw_skeleton.roots.resize(1);
  ozz::math::Float3 rest_translations[kJointCount];
  rest_translations[0] = {0.f, 0.1f, 0.f};

  ozz::animation::offline::RawSkeleton::Joint* joint =
      &raw_skeleton.roots[0];
  joint->name = "joint_0";
  joint->transform.translation = rest_translations[0];

  for (int j = 1; j < kJointCount; ++j) {
    rest_translations[j] = {0.f, 0.28f, 0.f};
    joint->children.resize(1);
    joint = &joint->children[0];
    joint->name = "joint_" + std::to_string(j);
    joint->transform.translation = rest_translations[j];
  }

  if (!raw_skeleton.Validate()) {
    std::fprintf(stderr, "invalid generated skeleton\n");
    return 1;
  }
  ozz::animation::offline::SkeletonBuilder skeleton_builder;
  ozz::unique_ptr<ozz::animation::Skeleton> skeleton =
      skeleton_builder(raw_skeleton);
  if (!skeleton || skeleton->num_joints() != kJointCount) {
    std::fprintf(stderr, "skeleton build failed\n");
    return 1;
  }

  // A short twisting animation.  All tracks have explicit translation,
  // rotation and scale keys so sampling is deterministic.
  ozz::animation::offline::RawAnimation raw_animation;
  raw_animation.duration = 1.f;
  raw_animation.tracks.resize(kJointCount);
  const float twist[kJointCount] = {0.f, 0.35f, 1.25f, -1.15f, -0.3f};

  for (int j = 0; j < kJointCount; ++j) {
    auto& track = raw_animation.tracks[j];

    ozz::animation::offline::RawAnimation::TranslationKey t0;
    t0.time = 0.f;
    t0.value = rest_translations[j];
    ozz::animation::offline::RawAnimation::TranslationKey t1;
    t1.time = 1.f;
    t1.value = rest_translations[j];
    track.translations.push_back(t0);
    track.translations.push_back(t1);

    ozz::animation::offline::RawAnimation::RotationKey r0;
    r0.time = 0.f;
    r0.value = ozz::math::Quaternion::identity();
    ozz::animation::offline::RawAnimation::RotationKey r1;
    r1.time = 1.f;
    r1.value = ozz::math::Quaternion::FromAxisAngle({0.f, 1.f, 0.f}, twist[j]);
    track.rotations.push_back(r0);
    track.rotations.push_back(r1);

    ozz::animation::offline::RawAnimation::ScaleKey s0;
    s0.time = 0.f;
    s0.value = {1.f, 1.f, 1.f};
    ozz::animation::offline::RawAnimation::ScaleKey s1;
    s1.time = 1.f;
    s1.value = {1.f, 1.f, 1.f};
    track.scales.push_back(s0);
    track.scales.push_back(s1);
  }

  if (!raw_animation.Validate()) {
    std::fprintf(stderr, "invalid generated animation\n");
    return 1;
  }
  ozz::animation::offline::AnimationBuilder animation_builder;
  ozz::unique_ptr<ozz::animation::Animation> animation =
      animation_builder(raw_animation);
  if (!animation) {
    std::fprintf(stderr, "animation build failed\n");
    return 1;
  }

  // Sample at a large part of the twist and evaluate model-space matrices.
  ozz::vector<ozz::math::SoaTransform> local_transforms(
      skeleton->num_soa_joints());
  ozz::animation::SamplingJob::Context context(kJointCount);
  ozz::animation::SamplingJob sampling;
  sampling.ratio = 0.72f;
  sampling.animation = animation.get();
  sampling.context = &context;
  sampling.output = ozz::make_span(local_transforms);
  if (!sampling.Run()) {
    std::fprintf(stderr, "sampling failed\n");
    return 1;
  }

  ozz::vector<ozz::math::Float4x4> model_matrices(kJointCount);
  ozz::animation::LocalToModelJob local_to_model;
  local_to_model.skeleton = skeleton.get();
  local_to_model.input = ozz::make_span(local_transforms);
  local_to_model.output = ozz::make_span(model_matrices);
  if (!local_to_model.Run()) {
    std::fprintf(stderr, "local-to-model failed\n");
    return 1;
  }

  // Compute actual inverse-bind matrices from the generated rest hierarchy.
  // These translations make them deliberately non-identity.
  ozz::vector<ozz::math::Float4x4> rest_model_matrices(kJointCount);
  ozz::animation::LocalToModelJob rest_to_model;
  rest_to_model.skeleton = skeleton.get();
  rest_to_model.input = skeleton->joint_rest_poses();
  rest_to_model.output = ozz::make_span(rest_model_matrices);
  if (!rest_to_model.Run()) {
    std::fprintf(stderr, "rest local-to-model failed\n");
    return 1;
  }

  ozz::vector<ozz::math::Float4x4> inverse_bind_matrices(kJointCount);
  ozz::vector<ozz::math::Float4x4> skin_matrices(kJointCount);
  for (int j = 0; j < kJointCount; ++j) {
    inverse_bind_matrices[j] = ozz::math::Invert(rest_model_matrices[j]);
    skin_matrices[j] = model_matrices[j] * inverse_bind_matrices[j];
  }

  // Compact, shuffled palette remapping. Palette slot -> skeleton joint.
  const uint16_t palette_joint[kPaletteSize] = {4, 1, 3, 0};
  ozz::math::Float4x4 palette[kPaletteSize];
  for (int p = 0; p < kPaletteSize; ++p) {
    palette[p] = skin_matrices[palette_joint[p]];
  }

  // A small ring of final mesh vertices around the twisting chain.
  float positions[kVertexCount * 3];
  uint16_t indices[kVertexCount * kInfluenceCount];
  float explicit_weights[kVertexCount * (kInfluenceCount - 1)];
  for (int v = 0; v < kVertexCount; ++v) {
    const float angle = 2.f * 3.14159265358979323846f * v / kVertexCount;
    positions[v * 3 + 0] = 0.18f * std::cos(angle);
    positions[v * 3 + 1] = 0.62f;
    positions[v * 3 + 2] = 0.18f * std::sin(angle);

    for (int i = 0; i < kInfluenceCount; ++i) {
      indices[v * kInfluenceCount + i] = static_cast<uint16_t>(i);
    }
    explicit_weights[v * 3 + 0] = 0.30f;
    explicit_weights[v * 3 + 1] = 0.28f;
    explicit_weights[v * 3 + 2] = 0.22f;  // Restored final weight is .20.
  }

  float linear_positions[kVertexCount * 3] = {};
  float dq_positions[kVertexCount * 3] = {};
  const auto palette_span =
      ozz::span<const ozz::math::Float4x4>(palette, kPaletteSize);

  if (!RunPositions(ozz::geometry::SkinningJob::kLinear, palette_span, indices,
                    kInfluenceCount * sizeof(uint16_t), explicit_weights,
                    (kInfluenceCount - 1) * sizeof(float), positions,
                    linear_positions)) {
    std::fprintf(stderr, "linear skinning failed\n");
    return 1;
  }
  if (!RunPositions(ozz::geometry::SkinningJob::kDualQuaternion, palette_span,
                    indices, kInfluenceCount * sizeof(uint16_t),
                    explicit_weights,
                    (kInfluenceCount - 1) * sizeof(float), positions,
                    dq_positions)) {
    std::fprintf(stderr, "dual quaternion skinning failed\n");
    return 1;
  }

  double maximum_mode_difference = 0.0;
  for (int v = 0; v < kVertexCount; ++v) {
    for (int k = 0; k < 3; ++k) {
      const float value = dq_positions[v * 3 + k];
      if (!std::isfinite(value)) {
        std::fprintf(stderr, "non-finite DQ output at vertex %d\n", v);
        return 1;
      }
      maximum_mode_difference = std::max(
          maximum_mode_difference,
          std::fabs(static_cast<double>(value) -
                    linear_positions[v * 3 + k]));
    }

    // The generated chain only translates on Y and rotates around Y, so the
    // final DQ vertices retain their generated cross-section radius.
    const double radius =
        std::hypot(dq_positions[v * 3 + 0], dq_positions[v * 3 + 2]);
    if (std::fabs(radius - 0.18) > 1e-4) {
      std::fprintf(stderr, "DQ cross-section radius changed at vertex %d\n",
                   v);
      return 2;
    }
  }
  if (maximum_mode_difference < 1e-5) {
    std::fprintf(stderr, "linear and DQ outputs are unexpectedly identical\n");
    return 3;
  }

  std::printf("mode,vertex,x,y,z\n");
  for (int v = 0; v < kVertexCount; ++v) {
    std::printf("linear,%d,%.6f,%.6f,%.6f\n", v, linear_positions[v * 3],
                linear_positions[v * 3 + 1], linear_positions[v * 3 + 2]);
  }
  for (int v = 0; v < kVertexCount; ++v) {
    for (int k = 0; k < 3; ++k) {
      if (!std::isfinite(dq_positions[v * 3 + k])) {
        std::fprintf(stderr, "non-finite DQ output at vertex %d\n", v);
        return 1;
      }
    }
    std::printf("dual_quaternion,%d,%.6f,%.6f,%.6f\n", v,
                dq_positions[v * 3], dq_positions[v * 3 + 1],
                dq_positions[v * 3 + 2]);
  }
  return 0;
}
