//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) Guillaume Blanc                                              //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#include "ozz/animation/offline/raw_animation_retargeter.h"

#include <algorithm>
#include <cmath>

#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_animation_utils.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/runtime/skeleton_utils.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/transform.h"
#include "ozz/base/maths/vec_float.h"

namespace ozz {
namespace animation {
namespace offline {

namespace {

bool IsFinite(const math::Float3& _v) {
  return std::isfinite(_v.x) && std::isfinite(_v.y) && std::isfinite(_v.z);
}

bool IsFinite(const math::Quaternion& _q) {
  return std::isfinite(_q.x) && std::isfinite(_q.y) && std::isfinite(_q.z) &&
         std::isfinite(_q.w);
}

bool IsUnitScale(const math::Float3& _scale) {
  static const float kScaleTolerance = 1e-6f;
  return std::abs(_scale.x - 1.f) <= kScaleTolerance &&
         std::abs(_scale.y - 1.f) <= kScaleTolerance &&
         std::abs(_scale.z - 1.f) <= kScaleTolerance;
}

// Local rest poses together with the packed depth-first parent table.
struct LocalSkeleton {
  ozz::vector<math::Transform> rest_locals;
  ozz::vector<int16_t> parents;
};

// Reads a runtime skeleton in its packed depth-first order. Fails if the
// skeleton does not define exactly one root.
bool ReadSkeleton(const Skeleton& _skeleton, LocalSkeleton* _local) {
  const int num_joints = _skeleton.num_joints();
  if (num_joints <= 0) {
    return false;
  }
  const span<const int16_t> parents = _skeleton.joint_parents();

  // Joint 0 must be the single root, every other joint must have a parent.
  if (parents[0] != Skeleton::kNoParent) {
    return false;
  }
  for (int i = 1; i < num_joints; ++i) {
    if (parents[i] < 0 || parents[i] >= i) {
      return false;
    }
  }

  _local->parents.assign(parents.begin(), parents.end());
  _local->rest_locals.resize(num_joints);
  for (int i = 0; i < num_joints; ++i) {
    _local->rest_locals[i] = GetJointRestPoseLocalSpace(_skeleton, i);
  }
  return true;
}

// Model-space posture evaluated at a probe time.
struct ModelPose {
  ozz::vector<math::Float3> translations;
  ozz::vector<math::Quaternion> rotations;
};

// Re-samples a raw animation output (in target local space) at _time and
// returns the corresponding model-space posture. Joints are stored in
// depth-first order, so a parent is always evaluated before its children.
// Empty channels follow the SampleAnimation identity conventions.
bool SampleModelPose(const RawAnimation& _animation,
                     const LocalSkeleton& _skeleton, float _time,
                     ModelPose* _pose) {
  const int num_joints = static_cast<int>(_skeleton.rest_locals.size());
  _pose->translations.resize(num_joints);
  _pose->rotations.resize(num_joints);

  for (int i = 0; i < num_joints; ++i) {
    math::Transform local;
    if (!SampleTrack(_animation.tracks[i], _time, &local)) {
      return false;
    }

    const int16_t parent = _skeleton.parents[i];
    if (parent == Skeleton::kNoParent) {
      _pose->translations[i] = local.translation;
      _pose->rotations[i] = local.rotation;
    } else {
      const math::Quaternion& parent_rotation = _pose->rotations[parent];
      _pose->translations[i] =
          _pose->translations[parent] +
          math::TransformVector(parent_rotation, local.translation);
      _pose->rotations[i] = parent_rotation * local.rotation;
    }
  }
  return true;
}

// Unsigned model-space rotation angle between two unit quaternions. Both are
// normalized before measuring, and the angle of the relative quaternion uses
// atan2 so it stays well conditioned near the identity (an acos of a value
// barely below 1 would otherwise inflate rounding noise into a large angle).
float RotationAngle(const math::Quaternion& _a, const math::Quaternion& _b) {
  const math::Quaternion a = math::Normalize(_a);
  const math::Quaternion b = math::Normalize(_b);
  const math::Quaternion relative = math::Conjugate(a) * b;
  const float sine_half =
      std::sqrt(relative.x * relative.x + relative.y * relative.y +
                relative.z * relative.z);
  return 2.f * std::atan2(sine_half, std::abs(relative.w));
}

// Tests that re-sampling _candidate at every probe time reproduces the
// reference postures within the position and angle tolerances.
bool FitsReference(const RawAnimation& _candidate,
                   const LocalSkeleton& _target,
                   const span<const float>& _probe_times,
                   const ozz::vector<ModelPose>& _reference,
                   float _position_tolerance, float _angle_tolerance) {
  ModelPose sampled;
  const int num_joints = static_cast<int>(_target.rest_locals.size());
  for (size_t p = 0; p < _probe_times.size(); ++p) {
    if (!SampleModelPose(_candidate, _target, _probe_times[p], &sampled)) {
      return false;
    }
    for (int j = 0; j < num_joints; ++j) {
      const math::Float3 position_delta =
          sampled.translations[j] - _reference[p].translations[j];
      if (math::Length(position_delta) > _position_tolerance) {
        return false;
      }
      if (RotationAngle(sampled.rotations[j], _reference[p].rotations[j]) >
          _angle_tolerance) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool RawAnimationRetargeter::operator()(
    const RawAnimation& _input, const Skeleton& _source_skeleton,
    const Skeleton& _target_skeleton, const span<const int16_t>& _joint_mapping,
    float _root_scale, const span<const float>& _probe_times,
    float _position_tolerance, float _angle_tolerance,
    RawAnimation* _output) const {
  // Validates pointers.
  if (_output == nullptr || _output == &_input) {
    return false;
  }

  // Validates the input animation.
  if (!_input.Validate()) {
    return false;
  }

  // Tolerances and root scale must be positive and finite.
  if (!std::isfinite(_root_scale) || _root_scale <= 0.f ||
      !std::isfinite(_position_tolerance) || _position_tolerance <= 0.f ||
      !std::isfinite(_angle_tolerance) || _angle_tolerance <= 0.f) {
    return false;
  }

  // Probe times must be strict, finite, cover 0 and the duration.
  if (_probe_times.size() < 2 || _probe_times[0] != 0.f ||
      _probe_times.back() != _input.duration) {
    return false;
  }
  for (size_t i = 0; i < _probe_times.size(); ++i) {
    if (!std::isfinite(_probe_times[i]) ||
        (i > 0 && _probe_times[i] <= _probe_times[i - 1])) {
      return false;
    }
  }

  // Reads both single-root skeletons.
  LocalSkeleton source;
  LocalSkeleton target;
  if (!ReadSkeleton(_source_skeleton, &source) ||
      !ReadSkeleton(_target_skeleton, &target)) {
    return false;
  }
  const int num_source_joints = static_cast<int>(source.rest_locals.size());
  const int num_target_joints = static_cast<int>(target.rest_locals.size());

  // The source animation must provide one track per source joint.
  if (_input.num_tracks() != num_source_joints) {
    return false;
  }

  // Validates the mapping: one entry per target joint, indices in range,
  // root only mapped to the source root.
  if (static_cast<int>(_joint_mapping.size()) != num_target_joints) {
    return false;
  }
  for (int j = 0; j < num_target_joints; ++j) {
    const int mapped = _joint_mapping[j];
    if (mapped < -1 || mapped >= num_source_joints) {
      return false;
    }
    if (j == 0) {
      // The target root can only stay unmapped or follow the source root.
      if (mapped != -1 && mapped != 0) {
        return false;
      }
    }
  }

  // Validates rest poses: finite, unit scales and unit quaternions only.
  for (const math::Transform& t : source.rest_locals) {
    if (!IsFinite(t.translation) || !IsFinite(t.rotation) ||
        !IsUnitScale(t.scale) || !math::IsNormalized(t.rotation)) {
      return false;
    }
  }
  for (const math::Transform& t : target.rest_locals) {
    if (!IsFinite(t.translation) || !IsFinite(t.rotation) ||
        !IsUnitScale(t.scale) || !math::IsNormalized(t.rotation)) {
      return false;
    }
  }

  // Computes both rest model-space poses. Unit scales make model rotation and
  // translation independent.
  ozz::vector<math::Float3> source_rest_t(num_source_joints);
  ozz::vector<math::Quaternion> source_rest_r(num_source_joints);
  ozz::vector<math::Float3> target_rest_t(num_target_joints);
  ozz::vector<math::Quaternion> target_rest_r(num_target_joints);
  for (int i = 0; i < num_source_joints; ++i) {
    const math::Transform& local = source.rest_locals[i];
    const int16_t parent = source.parents[i];
    if (parent == Skeleton::kNoParent) {
      source_rest_t[i] = local.translation;
      source_rest_r[i] = local.rotation;
    } else {
      source_rest_t[i] =
          source_rest_t[parent] +
          math::TransformVector(source_rest_r[parent], local.translation);
      source_rest_r[i] = source_rest_r[parent] * local.rotation;
    }
  }
  for (int i = 0; i < num_target_joints; ++i) {
    const math::Transform& local = target.rest_locals[i];
    const int16_t parent = target.parents[i];
    if (parent == Skeleton::kNoParent) {
      target_rest_t[i] = local.translation;
      target_rest_r[i] = local.rotation;
    } else {
      target_rest_t[i] =
          target_rest_t[parent] +
          math::TransformVector(target_rest_r[parent], local.translation);
      target_rest_r[i] = target_rest_r[parent] * local.rotation;
    }
  }

  const size_t num_probes = _probe_times.size();

  // Samples the source animation in local space at every probe time, and
  // validates the sampled channels (finite, unit scale, unit rotation).
  ozz::vector<ozz::vector<math::Transform>> source_local(
      num_probes, ozz::vector<math::Transform>(num_source_joints));
  for (size_t p = 0; p < num_probes; ++p) {
    if (!SampleAnimation(_input, _probe_times[p],
                         make_span(source_local[p]))) {
      return false;
    }
    for (int j = 0; j < num_source_joints; ++j) {
      const math::Transform& t = source_local[p][j];
      if (!IsFinite(t.translation) || !IsFinite(t.rotation) ||
          !IsUnitScale(t.scale) || !math::IsNormalized(t.rotation)) {
        return false;
      }
    }
  }

  // Builds the full source model-space posture at every probe time.
  ozz::vector<ModelPose> source_model(num_probes);
  for (size_t p = 0; p < num_probes; ++p) {
    source_model[p].translations.resize(num_source_joints);
    source_model[p].rotations.resize(num_source_joints);
    for (int j = 0; j < num_source_joints; ++j) {
      const math::Transform& local = source_local[p][j];
      const int16_t parent = source.parents[j];
      if (parent == Skeleton::kNoParent) {
        source_model[p].translations[j] = local.translation;
        source_model[p].rotations[j] = local.rotation;
      } else {
        source_model[p].translations[j] =
            source_model[p].translations[parent] +
            math::TransformVector(source_model[p].rotations[parent],
                                  local.translation);
        source_model[p].rotations[j] =
            source_model[p].rotations[parent] * local.rotation;
      }
    }
  }

  // Builds the complete target reference posture at every probe time. The
  // target local rotation of a mapped joint is computed against the actual
  // target parent model rotation of that frame, so unmapped branches (kept at
  // rest) still propagate correctly.
  ozz::vector<ModelPose> reference(num_probes);
  ozz::vector<ozz::vector<math::Transform>> target_local(
      num_probes, ozz::vector<math::Transform>(num_target_joints));
  const bool root_mapped = _joint_mapping[0] == 0;
  for (size_t p = 0; p < num_probes; ++p) {
    reference[p].translations.resize(num_target_joints);
    reference[p].rotations.resize(num_target_joints);
    for (int j = 0; j < num_target_joints; ++j) {
      const int mapped = _joint_mapping[j];
      const math::Transform& target_rest_local = target.rest_locals[j];
      const int16_t parent = target.parents[j];

      math::Float3 model_translation;
      math::Quaternion model_rotation;

      if (j == 0) {
        // Target root.
        if (root_mapped) {
          model_translation =
              target_rest_t[0] +
              (source_model[p].translations[0] - source_rest_t[0]) *
                  _root_scale;
          model_rotation = source_model[p].rotations[0] *
                           math::Conjugate(source_rest_r[0]) *
                           target_rest_r[0];
        } else {
          model_translation = target_rest_t[0];
          model_rotation = target_rest_r[0];
        }
      } else if (mapped == -1) {
        // Unmapped joints keep their rest local pose and propagate.
        const math::Quaternion& parent_rotation =
            reference[p].rotations[parent];
        model_translation =
            reference[p].translations[parent] +
            math::TransformVector(parent_rotation,
                                  target_rest_local.translation);
        model_rotation = parent_rotation * target_rest_local.rotation;
      } else {
        // Expected target model rotation Qs * inverse(Rs) * Rt.
        model_rotation = source_model[p].rotations[mapped] *
                         math::Conjugate(source_rest_r[mapped]) *
                         target_rest_r[j];
        // Target bone length and rest translation are preserved.
        model_translation =
            reference[p].translations[parent] +
            math::TransformVector(reference[p].rotations[parent],
                                  target_rest_local.translation);
      }

      reference[p].translations[j] = model_translation;
      reference[p].rotations[j] = model_rotation;

      // Derives the local pose from the actual frame parent model pose.
      math::Transform& local = target_local[p][j];
      local.scale = math::Float3::one();
      if (parent == Skeleton::kNoParent) {
        local.translation = model_translation;
        local.rotation = model_rotation;
      } else {
        const math::Quaternion& parent_rotation =
            reference[p].rotations[parent];
        local.translation = math::TransformVector(
            math::Conjugate(parent_rotation),
            model_translation - reference[p].translations[parent]);
        local.rotation = math::Conjugate(parent_rotation) * model_rotation;
      }
    }
  }

  // Canonicalizes quaternion signs per joint track: the first key is forced
  // into the w >= 0 hemisphere, and every following key is flipped when
  // needed to keep a positive dot with the previous one. This mirrors what
  // AnimationBuilder does, makes the key sequence independent from the sign
  // convention used in the source animation, and keeps key selection
  // deterministic. A quaternion and its negation represent the same rotation,
  // so model-space tolerances are unaffected.
  for (int j = 0; j < num_target_joints; ++j) {
    for (size_t p = 0; p < num_probes; ++p) {
      math::Quaternion& rotation = target_local[p][j].rotation;
      if (p == 0) {
        if (rotation.w < 0.f) {
          rotation = -rotation;
        }
      } else if (math::Dot(target_local[p - 1][j].rotation, rotation) < 0.f) {
        rotation = -rotation;
      }
    }
  }

  // Starts with a key at every probe time, then greedily removes the internal
  // ones that are not needed. The common key-time set is shared by every
  // joint, so chain end points and unmapped branches take part in the choice.
  ozz::vector<size_t> kept(num_probes);
  for (size_t i = 0; i < num_probes; ++i) {
    kept[i] = i;
  }

  // Helper that fills an output-like raw animation from the selected probes.
  auto build_candidate = [&](const ozz::vector<size_t>& _keys) {
    RawAnimation candidate;
    candidate.duration = _input.duration;
    candidate.tracks.resize(num_target_joints);
    for (const size_t key_index : _keys) {
      const float time = _probe_times[key_index];
      for (int j = 0; j < num_target_joints; ++j) {
        const math::Transform& local = target_local[key_index][j];
        candidate.tracks[j].translations.push_back({time, local.translation});
        candidate.tracks[j].rotations.push_back({time, local.rotation});
        candidate.tracks[j].scales.push_back({time, math::Float3::one()});
      }
    }
    return candidate;
  };

  // Internal probes are tested in ascending order, and the passes repeat
  // until a whole pass removes nothing. Every internal probe time kept by
  // the final set is therefore necessary: dropping it alone would violate
  // at least one tolerance. Iteration order is fixed, so the selected key
  // sequence is deterministic for identical inputs.
  bool removed_any = true;
  while (removed_any) {
    removed_any = false;
    for (size_t probe_index = 1; probe_index + 1 < num_probes; ++probe_index) {
      if (std::find(kept.begin(), kept.end(), probe_index) == kept.end()) {
        continue;
      }
      ozz::vector<size_t> reduced;
      reduced.reserve(kept.size() - 1);
      for (size_t k : kept) {
        if (k != probe_index) {
          reduced.push_back(k);
        }
      }

      RawAnimation reduced_animation = build_candidate(reduced);
      if (FitsReference(reduced_animation, target, _probe_times, reference,
                        _position_tolerance, _angle_tolerance)) {
        kept = std::move(reduced);
        removed_any = true;
      }
    }
  }

  // Builds the final output only once all inputs proved valid.
  RawAnimation result;
  result.duration = _input.duration;
  result.name = _input.name;
  result.tracks.resize(num_target_joints);
  for (const size_t key_index : kept) {
    const float time = _probe_times[key_index];
    for (int j = 0; j < num_target_joints; ++j) {
      const math::Transform& local = target_local[key_index][j];
      result.tracks[j].translations.push_back({time, local.translation});
      result.tracks[j].rotations.push_back({time, local.rotation});
      result.tracks[j].scales.push_back({time, math::Float3::one()});
    }
  }

  if (!result.Validate()) {
    return false;
  }
  // The full key set must always meet the tolerances; a failure here is an
  // internal error rather than a user input error.
  if (!FitsReference(result, target, _probe_times, reference,
                     _position_tolerance, _angle_tolerance)) {
    return false;
  }

  *_output = std::move(result);
  return true;
}

}  // namespace offline
}  // namespace animation
}  // namespace ozz
