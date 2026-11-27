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
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>

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

// Quaternions are accepted if their squared norm is within this tolerance of
// one. They are normalized before being used.
constexpr float kQuaternionNormTolerance = 1e-3f;

// Local scale is only supported if it is equal to one within this tolerance.
constexpr float kScaleTolerance = 1e-5f;

bool IsFinite(float _value) { return std::isfinite(_value); }

bool IsFinite(const math::Float3& _value) {
  return IsFinite(_value.x) && IsFinite(_value.y) && IsFinite(_value.z);
}

bool IsFinite(const math::Quaternion& _value) {
  return IsFinite(_value.x) && IsFinite(_value.y) && IsFinite(_value.z) &&
         IsFinite(_value.w);
}

bool IsUnitScale(const math::Float3& _scale) {
  return std::abs(_scale.x - 1.f) <= kScaleTolerance &&
         std::abs(_scale.y - 1.f) <= kScaleTolerance &&
         std::abs(_scale.z - 1.f) <= kScaleTolerance;
}

bool IsNormalizedQuaternion(const math::Quaternion& _quaternion) {
  if (!IsFinite(_quaternion)) {
    return false;
  }
  const float squared_norm =
      _quaternion.x * _quaternion.x + _quaternion.y * _quaternion.y +
      _quaternion.z * _quaternion.z + _quaternion.w * _quaternion.w;
  return std::abs(squared_norm - 1.f) <= kQuaternionNormTolerance;
}

math::Quaternion NormalizeQuaternion(const math::Quaternion& _quaternion) {
  return math::NormalizeSafe(_quaternion, math::Quaternion::identity());
}

// Returns the unsigned angle, in radians, between rotations _a and _b.
// Both quaternions are expected to be normalized.
float RotationAngle(const math::Quaternion& _a,
                    const math::Quaternion& _b) {
  // Computes _a^-1 * _b, which is the relative rotation. Using atan2 on the
  // vector/scalar parts is numerically stable for small angles, unlike acos.
  const math::Quaternion relative =
      NormalizeQuaternion(math::Conjugate(_a) * _b);
  const float vector_sq = relative.x * relative.x + relative.y * relative.y +
                          relative.z * relative.z;
  const float vector_length = std::sqrt(vector_sq);
  const float scalar = std::abs(relative.w);
  return 2.f * std::atan2(vector_length, scalar);
}

// Returns a deterministic representative of the rotation _quaternion. _q and
// -_q represent the same rotation, this function always returns the same
// value for both.
math::Quaternion CanonicalQuaternion(const math::Quaternion& _quaternion) {
  math::Quaternion normalized = NormalizeQuaternion(_quaternion);
  const float components[4] = {normalized.x, normalized.y, normalized.z,
                               normalized.w};
  int largest = 0;
  for (int i = 1; i < 4; ++i) {
    if (std::abs(components[i]) > std::abs(components[largest])) {
      largest = i;
    }
  }
  if (components[largest] < 0.f) {
    normalized = -normalized;
  }
  return normalized;
}

// Finds the unique root joint of _skeleton.
// Returns Skeleton::kNoParent if there's no root, or a value lower than
// kNoParent if there are several roots.
int FindUniqueRoot(const Skeleton& _skeleton) {
  const span<const int16_t> parents = _skeleton.joint_parents();
  int root = Skeleton::kNoParent;
  for (int i = 0; i < _skeleton.num_joints(); ++i) {
    if (parents[i] == Skeleton::kNoParent) {
      if (root != Skeleton::kNoParent) {
        return Skeleton::kNoParent - 1;  // Multiple roots.
      }
      root = i;
    }
  }
  return root;
}

// Type used to store a complete target pose, in local and model spaces, at a
// given time.
struct TargetPose {
  // Local transforms. Scale is always unit.
  ozz::vector<math::Transform> locals;

  // Model-space joint origins.
  ozz::vector<math::Float3> origins;

  // Model-space joint rotations.
  ozz::vector<math::Quaternion> rotations;
};

// All the data that is constant during a retargeting operation.
struct RetargetSetup {
  const Skeleton* source = nullptr;
  const Skeleton* target = nullptr;

  int source_root = Skeleton::kNoParent;
  int target_root = Skeleton::kNoParent;

  // Source joint index for each target joint. -1 means unmapped.
  const int* mapping = nullptr;

  float root_scale = 1.f;

  // Rest poses, in local space.
  ozz::vector<math::Transform> source_rest_locals;
  ozz::vector<math::Transform> target_rest_locals;

  // Rest poses rotations, in model space.
  ozz::vector<math::Quaternion> source_rest_model_rotations;
  ozz::vector<math::Quaternion> target_rest_model_rotations;
};

// Extracts a skeleton rest pose in local space, and validates the data that
// retargeting relies on.
bool ExtractRestPose(const Skeleton& _skeleton,
                     ozz::vector<math::Transform>* _locals) {
  const int num_joints = _skeleton.num_joints();
  _locals->resize(num_joints);
  for (int i = 0; i < num_joints; ++i) {
    const math::Transform rest = GetJointRestPoseLocalSpace(_skeleton, i);
    if (!IsFinite(rest.translation) || !IsFinite(rest.rotation) ||
        !IsUnitScale(rest.scale) || !IsNormalizedQuaternion(rest.rotation)) {
      return false;
    }
    (*_locals)[i] = rest;
    (*_locals)[i].rotation = NormalizeQuaternion(rest.rotation);
    (*_locals)[i].scale = math::Float3::one();
  }
  return true;
}

// Accumulates local rotations to model space. Joint order is depth-first, so
// a joint parent always comes first. Scale is expected to be unit.
void ComputeRestModelRotations(
    const Skeleton& _skeleton, const ozz::vector<math::Transform>& _locals,
    ozz::vector<math::Quaternion>* _model_rotations) {
  const span<const int16_t> parents = _skeleton.joint_parents();
  const int num_joints = _skeleton.num_joints();
  _model_rotations->resize(num_joints);
  for (int i = 0; i < num_joints; ++i) {
    const math::Quaternion local = _locals[i].rotation;
    const int parent = parents[i];
    (*_model_rotations)[i] =
        parent == Skeleton::kNoParent
            ? local
            : NormalizeQuaternion((*_model_rotations)[parent] * local);
  }
}

// Validates and computes the source model-space rotations at time _time.
bool ComputeSourceModelRotations(const RetargetSetup& _setup,
                                 const RawAnimation& _input, float _time,
                                 ozz::vector<math::Quaternion>* _model_rotations,
                                 ozz::vector<math::Transform>* _locals) {
  const Skeleton& source = *_setup.source;
  const int num_joints = source.num_joints();
  _locals->resize(num_joints);

  // SampleAnimation fills unmapped/empty channels with identity values, which
  // is the convention expected by this retargeter.
  if (!SampleAnimation(_input, _time, make_span(*_locals))) {
    return false;
  }

  const span<const int16_t> parents = source.joint_parents();
  _model_rotations->resize(num_joints);
  for (int i = 0; i < num_joints; ++i) {
    const math::Quaternion local = NormalizeQuaternion((*_locals)[i].rotation);
    const int parent = parents[i];
    (*_model_rotations)[i] =
        parent == Skeleton::kNoParent
            ? local
            : NormalizeQuaternion((*_model_rotations)[parent] * local);
  }
  return true;
}

// Computes the full target reference pose at time _time.
bool ComputeTargetReference(const RetargetSetup& _setup,
                            const RawAnimation& _input, float _time,
                            TargetPose* _pose) {
  const Skeleton& target = *_setup.target;
  const int num_target_joints = target.num_joints();
  const span<const int16_t> target_parents = target.joint_parents();

  // Samples the source animation and computes its model-space rotations.
  ozz::vector<math::Quaternion> source_model_rotations;
  ozz::vector<math::Transform> source_locals;
  if (!ComputeSourceModelRotations(_setup, _input, _time,
                                   &source_model_rotations, &source_locals)) {
    return false;
  }

  _pose->locals.resize(num_target_joints);
  _pose->rotations.resize(num_target_joints);
  _pose->origins.resize(num_target_joints);

  // Computes target local rotations. Local rotation of a mapped joint is
  // recomputed from the *actual* target parent model rotation at this time, so
  // that intermediate joints and different rest poses are taken into account.
  for (int i = 0; i < num_target_joints; ++i) {
    const int parent = target_parents[i];
    const math::Quaternion parent_model_rotation =
        parent == Skeleton::kNoParent
            ? math::Quaternion::identity()
            : _pose->rotations[parent];

    const int source_joint = _setup.mapping[i];
    math::Quaternion local_rotation;
    if (source_joint >= 0) {
      // Desired model rotation: Qs * inverse(Rs) * Rt.
      const math::Quaternion desired_model_rotation =
          source_model_rotations[source_joint] *
          math::Conjugate(_setup.source_rest_model_rotations[source_joint]) *
          _setup.target_rest_model_rotations[i];
      local_rotation = NormalizeQuaternion(
          math::Conjugate(parent_model_rotation) * desired_model_rotation);
    } else {
      // Unmapped joints keep their rest local pose.
      local_rotation = _setup.target_rest_locals[i].rotation;
    }
    _pose->locals[i].rotation = local_rotation;
    _pose->locals[i].scale = math::Float3::one();
    _pose->rotations[i] =
        NormalizeQuaternion(parent_model_rotation * local_rotation);
  }

  // Computes target local translations. Only the target root joint, when it is
  // mapped to the source root joint, receives translated motion.
  for (int i = 0; i < num_target_joints; ++i) {
    math::Float3 local_translation = _setup.target_rest_locals[i].translation;
    if (i == _setup.target_root && _setup.mapping[i] == _setup.source_root) {
      const math::Float3 source_root_translation =
          source_locals[_setup.source_root].translation;
      const math::Float3 source_rest_translation =
          _setup.source_rest_locals[_setup.source_root].translation;
      local_translation =
          local_translation + (source_root_translation - source_rest_translation) *
                                  _setup.root_scale;
    }
    _pose->locals[i].translation = local_translation;
  }

  // Computes model-space joint origins. Scale is unit.
  for (int i = 0; i < num_target_joints; ++i) {
    const int parent = target_parents[i];
    if (parent == Skeleton::kNoParent) {
      _pose->origins[i] = _pose->locals[i].translation;
    } else {
      _pose->origins[i] =
          _pose->origins[parent] +
          math::TransformVector(_pose->rotations[parent],
                                _pose->locals[i].translation);
    }
  }
  return true;
}

// Evaluates the model-space error generated by a set of retained keyframe
// times. Key times are indices in the probe times array.
class ErrorEvaluator {
 public:
  ErrorEvaluator(const RetargetSetup& _setup,
                 const ozz::vector<TargetPose>& _references,
                 const span<const float>& _times, float _position_tolerance,
                 float _angle_tolerance)
      : setup_(_setup),
        references_(_references),
        times_(_times),
        position_tolerance_(_position_tolerance),
        angle_tolerance_(_angle_tolerance) {}

  // Returns the worst normalized error at probe time _probe, given the sorted
  // list of retained key times. A result greater than one means that at least
  // one constraint is violated. Exact key times have no error.
  float ProbeError(int _probe, const ozz::vector<int>& _keys) const {
    const ozz::vector<int>::const_iterator found =
        std::lower_bound(_keys.begin(), _keys.end(), _probe);
    if (found != _keys.end() && *found == _probe) {
      return 0.f;  // Exact key, no reconstruction error.
    }
    assert(found != _keys.begin() && found != _keys.end());
    const int left = *(found - 1);
    const int right = *found;
    const float left_time = times_[left];
    const float right_time = times_[right];
    const float alpha =
        (times_[_probe] - left_time) / (right_time - left_time);

    const TargetPose& pose_left = references_[left];
    const TargetPose& pose_right = references_[right];
    const TargetPose& reference = references_[_probe];

    const int num_joints = setup_.target->num_joints();
    ozz::vector<math::Transform> locals(num_joints);
    ozz::vector<math::Quaternion> model_rotations(num_joints);
    ozz::vector<math::Float3> origins(num_joints);

    for (int i = 0; i < num_joints; ++i) {
      locals[i].translation = LerpTranslation(pose_left.locals[i].translation,
                                              pose_right.locals[i].translation,
                                              alpha);
      locals[i].rotation = LerpRotation(pose_left.locals[i].rotation,
                                        pose_right.locals[i].rotation, alpha);
      locals[i].scale = math::Float3::one();
    }

    const span<const int16_t> parents = setup_.target->joint_parents();
    float worst = 0.f;
    for (int i = 0; i < num_joints; ++i) {
      const int parent = parents[i];
      if (parent == Skeleton::kNoParent) {
        origins[i] = locals[i].translation;
        model_rotations[i] = locals[i].rotation;
      } else {
        model_rotations[i] = NormalizeQuaternion(model_rotations[parent] *
                                                 locals[i].rotation);
        origins[i] = origins[parent] + math::TransformVector(
                                           model_rotations[parent],
                                           locals[i].translation);
      }

      const float position_error = math::Length(origins[i] - reference.origins[i]);
      const float angle_error =
          RotationAngle(model_rotations[i], reference.rotations[i]);
      worst = math::Max(worst, position_error / position_tolerance_);
      worst = math::Max(worst, angle_error / angle_tolerance_);
    }
    return worst;
  }

  // Returns the worst normalized error over all probe times.
  float MaxError(const ozz::vector<int>& _keys) const {
    float worst = 0.f;
    const int num_probes = static_cast<int>(references_.size());
    for (int p = 0; p < num_probes; ++p) {
      worst = math::Max(worst, ProbeError(p, _keys));
    }
    return worst;
  }

 private:
  const RetargetSetup& setup_;
  const ozz::vector<TargetPose>& references_;
  span<const float> times_;
  float position_tolerance_;
  float angle_tolerance_;
};

// Validates the source animation channels that are used by retargeting.
bool ValidateSourceAnimation(const RawAnimation& _input) {
  if (_input.num_tracks() > Skeleton::kMaxJoints) {
    return false;
  }
  for (int i = 0; i < _input.num_tracks(); ++i) {
    const RawAnimation::JointTrack& track = _input.tracks[i];
    for (const RawAnimation::TranslationKey& key : track.translations) {
      if (!IsFinite(key.value)) {
        return false;
      }
    }
    for (const RawAnimation::RotationKey& key : track.rotations) {
      if (!IsNormalizedQuaternion(key.value)) {
        return false;
      }
    }
    for (const RawAnimation::ScaleKey& key : track.scales) {
      if (!IsFinite(key.value) || !IsUnitScale(key.value)) {
        return false;
      }
    }
  }
  return true;
}

// Validates retargeting parameters that don't depend on the skeletons.
bool ValidateParams(const RawAnimationRetargeter::Params& _params,
                    float _duration) {
  if (!IsFinite(_params.position_tolerance) ||
      _params.position_tolerance <= 0.f) {
    return false;
  }
  if (!IsFinite(_params.angle_tolerance) || _params.angle_tolerance <= 0.f) {
    return false;
  }
  if (!IsFinite(_params.root_scale) || _params.root_scale <= 0.f) {
    return false;
  }

  const span<const float> times = _params.probe_times;
  if (times.size() < 2) {
    return false;
  }
  if (times[0] != 0.f || times[times.size() - 1] != _duration) {
    return false;
  }
  for (size_t i = 0; i < times.size(); ++i) {
    if (!IsFinite(times[i]) || times[i] < 0.f || times[i] > _duration) {
      return false;
    }
    if (i > 0 && !(times[i] > times[i - 1])) {
      return false;
    }
  }
  return true;
}
}  // namespace

bool RawAnimationRetargeter::operator()(const RawAnimation& _input,
                                        const Skeleton& _source_skeleton,
                                        const Skeleton& _target_skeleton,
                                        const Params& _params,
                                        RawAnimation* _output) const {
  if (!_output || &_input == _output) {
    return false;
  }

  // Output is always reset to a valid, readable and empty animation on
  // failure. Inputs are never modified.
  *_output = RawAnimation();

  // Validates the source animation.
  if (!_input.Validate() || !IsFinite(_input.duration)) {
    return false;
  }
  if (!ValidateSourceAnimation(_input)) {
    return false;
  }

  // The source animation must drive each source joint exactly once.
  const int num_source_joints = _source_skeleton.num_joints();
  if (_input.num_tracks() != num_source_joints) {
    return false;
  }

  // Validates the mapping.
  const int num_target_joints = _target_skeleton.num_joints();
  if (_params.source_joint_indices.size() !=
      static_cast<size_t>(num_target_joints)) {
    return false;
  }
  const span<const int> mapping = _params.source_joint_indices;
  for (int i = 0; i < num_target_joints; ++i) {
    if (mapping[i] < -1 || mapping[i] >= num_source_joints) {
      return false;
    }
  }

  // Both skeletons must have exactly one root.
  const int source_root = FindUniqueRoot(_source_skeleton);
  const int target_root = FindUniqueRoot(_target_skeleton);
  if (source_root < 0 || target_root < 0) {
    return false;
  }

  // Mapping the target root to a non-root source joint is not supported.
  if (mapping[target_root] >= 0 && mapping[target_root] != source_root) {
    return false;
  }

  // Validates probe times and tolerances.
  if (!ValidateParams(_params, _input.duration)) {
    return false;
  }

  // Extracts and validates both rest poses.
  RetargetSetup setup;
  setup.source = &_source_skeleton;
  setup.target = &_target_skeleton;
  setup.source_root = source_root;
  setup.target_root = target_root;
  setup.mapping = mapping.data();
  setup.root_scale = _params.root_scale;

  if (!ExtractRestPose(_source_skeleton, &setup.source_rest_locals) ||
      !ExtractRestPose(_target_skeleton, &setup.target_rest_locals)) {
    return false;
  }
  ComputeRestModelRotations(_source_skeleton, setup.source_rest_locals,
                            &setup.source_rest_model_rotations);
  ComputeRestModelRotations(_target_skeleton, setup.target_rest_locals,
                            &setup.target_rest_model_rotations);

  // Computes the complete target reference pose at every probe time.
  const span<const float> probe_times = _params.probe_times;
  const int num_probes = static_cast<int>(probe_times.size());
  ozz::vector<TargetPose> references(num_probes);
  for (int p = 0; p < num_probes; ++p) {
    if (!ComputeTargetReference(setup, _input, probe_times[p],
                                &references[p])) {
      *_output = RawAnimation();
      return false;
    }
  }

  const ErrorEvaluator evaluator(setup, references, probe_times,
                                 _params.position_tolerance,
                                 _params.angle_tolerance);

  // Selects a set of key times shared by all joints. Starts with the mandatory
  // first and last probe times, then inserts the worst violating probe until
  // all constraints are met.
  ozz::vector<int> keys;
  keys.push_back(0);
  if (num_probes > 1) {
    keys.push_back(num_probes - 1);
  }
  for (;;) {
    int worst_probe = -1;
    float worst_error = 1.f;
    for (int p = 0; p < num_probes; ++p) {
      if (std::binary_search(keys.begin(), keys.end(), p)) {
        continue;
      }
      const float error = evaluator.ProbeError(p, keys);
      if (error > worst_error) {
        worst_error = error;
        worst_probe = p;
      }
    }
    if (worst_probe < 0) {
      break;  // All probe times are within tolerances.
    }
    keys.insert(std::lower_bound(keys.begin(), keys.end(), worst_probe),
                worst_probe);
  }

  // Removes keys that are not required. Once this loop ends, removing any
  // remaining internal key makes at least one constraint fail.
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t k = 1; k + 1 < keys.size(); ++k) {
      ozz::vector<int> candidate = keys;
      candidate.erase(candidate.begin() + k);
      if (evaluator.MaxError(candidate) <= 1.f) {
        keys.swap(candidate);
        changed = true;
        break;
      }
    }
  }

  // Builds the output animation. One shared set of key times is used for both
  // translations and rotations of all joints. Scales are left empty, meaning
  // unit scale.
  _output->name = _input.name;
  _output->duration = _input.duration;
  _output->tracks.resize(num_target_joints);
  for (int i = 0; i < num_target_joints; ++i) {
    RawAnimation::JointTrack& track = _output->tracks[i];
    track.translations.reserve(keys.size());
    track.rotations.reserve(keys.size());
    for (const int key : keys) {
      const float time = probe_times[key];
      track.translations.push_back(
          {time, references[key].locals[i].translation});
      track.rotations.push_back(
          {time, CanonicalQuaternion(references[key].locals[i].rotation)});
    }
  }

  return _output->Validate();
}
}  // namespace offline
}  // namespace animation
}  // namespace ozz
