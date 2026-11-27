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

// Tests for ozz::animation::offline::RawAnimationRetargeter.
//
// Sketelons are generated programmatically, with different rest rotations,
// extra intermediate bones, unmapped joints and a number of joints that isn't
// a multiple of four. The expected (reference) model rotations and model
// endpoints are computed independently from the retargeter implementation,
// following the documented equations. The output animation is also pushed
// through AnimationBuilder, SamplingJob and LocalToModelJob, as well as through
// the offline SampleAnimation() resampling path.

#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <limits>
#include <string>

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_animation_retargeter.h"
#include "ozz/animation/offline/raw_animation_utils.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/runtime/skeleton_utils.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/transform.h"
#include "ozz/base/maths/vec_float.h"
#include "ozz/base/platform.h"
#include "ozz/base/span.h"

using ozz::animation::Animation;
using ozz::animation::LocalToModelJob;
using ozz::animation::SamplingJob;
using ozz::animation::Skeleton;
using ozz::animation::offline::AnimationBuilder;
using ozz::animation::offline::RawAnimation;
using ozz::animation::offline::RawAnimationRetargeter;
using ozz::animation::offline::RawSkeleton;
using ozz::animation::offline::SampleAnimation;
using ozz::animation::offline::SkeletonBuilder;
using ozz::math::Float3;
using ozz::math::Float4x4;
using ozz::math::Quaternion;
using ozz::math::Transform;

namespace {

constexpr float kPi = 3.14159265358979323846f;

Quaternion AxisAngle(const Float3& _axis, float _degrees) {
  return Quaternion::FromAxisAngle(ozz::math::Normalize(_axis),
                                   _degrees * kPi / 180.f);
}

float AngleBetween(const Quaternion& _a, const Quaternion& _b) {
  const Quaternion relative = ozz::math::NormalizeSafe(
      ozz::math::Conjugate(_a) * _b, Quaternion::identity());
  const float vector_length =
      std::sqrt(relative.x * relative.x + relative.y * relative.y +
                relative.z * relative.z);
  return 2.f * std::atan2(vector_length, std::abs(relative.w));
}

// Builds a source skeleton with 7 joints. The rest pose exposes non-identity
// rotations, and 7 isn't a multiple of 4.
ozz::unique_ptr<Skeleton> BuildSourceSkeleton() {
  RawSkeleton raw;
  raw.roots.resize(1);
  RawSkeleton::Joint& root = raw.roots[0];
  root.name = "source_root";
  root.transform.translation = Float3(0.f, 1.f, 0.f);
  root.transform.rotation = AxisAngle(Float3(0.f, 1.f, 0.f), 10.f);

  root.children.resize(2);
  RawSkeleton::Joint& spine = root.children[0];
  spine.name = "source_spine";
  spine.transform.translation = Float3(0.f, 0.5f, 0.f);
  spine.transform.rotation = AxisAngle(Float3(1.f, 0.f, 0.f), 20.f);
  spine.children.resize(2);

  RawSkeleton::Joint& neck = spine.children[0];
  neck.name = "source_neck";
  neck.transform.translation = Float3(0.f, 0.3f, 0.f);
  neck.transform.rotation = AxisAngle(Float3(0.f, 0.f, 1.f), -15.f);
  neck.children.resize(1);
  RawSkeleton::Joint& head = neck.children[0];
  head.name = "source_head";
  head.transform.translation = Float3(0.f, 0.2f, 0.f);
  head.transform.rotation = AxisAngle(Float3(1.f, 1.f, 0.f), 5.f);

  RawSkeleton::Joint& arm = spine.children[1];
  arm.name = "source_arm";
  arm.transform.translation = Float3(0.4f, 0.f, 0.f);
  arm.transform.rotation = AxisAngle(Float3(0.f, 1.f, 0.f), 35.f);
  arm.children.resize(1);
  RawSkeleton::Joint& hand = arm.children[0];
  hand.name = "source_hand";
  hand.transform.translation = Float3(0.6f, 0.f, 0.f);
  hand.transform.rotation = AxisAngle(Float3(1.f, 0.f, 0.f), -25.f);

  RawSkeleton::Joint& leg = root.children[1];
  leg.name = "source_leg";
  leg.transform.translation = Float3(0.2f, -0.5f, 0.f);
  leg.transform.rotation = AxisAngle(Float3(0.f, 0.f, 1.f), 40.f);

  return SkeletonBuilder()(raw);
}

// Builds a target skeleton with 10 joints: different rest rotations, two extra
// intermediate bones (spine_a, spine_b), an unmapped intermediate bone (hand)
// with a mapped descendant (finger), and an unmapped leaf (foot).
ozz::unique_ptr<Skeleton> BuildTargetSkeleton() {
  RawSkeleton raw;
  raw.roots.resize(1);
  RawSkeleton::Joint& root = raw.roots[0];
  root.name = "target_root";
  root.transform.translation = Float3(0.f, 1.2f, 0.f);
  root.transform.rotation = AxisAngle(Float3(0.f, 1.f, 0.f), 5.f);

  root.children.resize(2);
  RawSkeleton::Joint& spine_a = root.children[0];
  spine_a.name = "target_spine_a";
  spine_a.transform.translation = Float3(0.f, 0.2f, 0.f);
  spine_a.transform.rotation = AxisAngle(Float3(0.f, 1.f, 0.f), 10.f);
  spine_a.children.resize(1);

  RawSkeleton::Joint& spine_b = spine_a.children[0];
  spine_b.name = "target_spine_b";
  spine_b.transform.translation = Float3(0.f, 0.3f, 0.f);
  spine_b.transform.rotation = AxisAngle(Float3(1.f, 0.f, 0.f), 5.f);
  spine_b.children.resize(2);

  RawSkeleton::Joint& neck = spine_b.children[0];
  neck.name = "target_neck";
  neck.transform.translation = Float3(0.f, 0.35f, 0.f);
  neck.transform.rotation = AxisAngle(Float3(0.f, 0.f, 1.f), -20.f);
  neck.children.resize(1);
  RawSkeleton::Joint& head = neck.children[0];
  head.name = "target_head";
  head.transform.translation = Float3(0.f, 0.25f, 0.f);
  head.transform.rotation = AxisAngle(Float3(1.f, 0.f, 0.f), 15.f);

  RawSkeleton::Joint& arm = spine_b.children[1];
  arm.name = "target_arm";
  arm.transform.translation = Float3(0.5f, 0.f, 0.f);
  arm.transform.rotation = AxisAngle(Float3(0.f, 1.f, 0.f), -30.f);
  arm.children.resize(1);
  RawSkeleton::Joint& hand = arm.children[0];
  hand.name = "target_hand";
  hand.transform.translation = Float3(0.7f, 0.f, 0.f);
  hand.transform.rotation = AxisAngle(Float3(1.f, 1.f, 1.f), 25.f);
  hand.children.resize(1);
  RawSkeleton::Joint& finger = hand.children[0];
  finger.name = "target_finger";
  finger.transform.translation = Float3(0.2f, 0.05f, 0.f);
  finger.transform.rotation = AxisAngle(Float3(0.f, 0.f, 1.f), -35.f);

  RawSkeleton::Joint& leg = root.children[1];
  leg.name = "target_leg";
  leg.transform.translation = Float3(0.25f, -0.4f, 0.f);
  leg.transform.rotation = AxisAngle(Float3(0.f, 0.f, 1.f), -5.f);
  leg.children.resize(1);
  RawSkeleton::Joint& foot = leg.children[0];
  foot.name = "target_foot";
  foot.transform.translation = Float3(0.f, -0.3f, 0.1f);
  foot.transform.rotation = AxisAngle(Float3(1.f, 0.f, 0.f), 12.f);

  return SkeletonBuilder()(raw);
}

// Source animation used by most cases. Rotations are smooth sinusoidal
// motions; the root translation is linear so that root retargeting can be
// checked exactly. Local scales are unit (tracks are left empty).
RawAnimation BuildSourceAnimation(const Skeleton& _skeleton,
                                  bool _empty_head_rotation = false) {
  RawAnimation animation;
  animation.name = "source_motion";
  animation.duration = 1.f;
  animation.tracks.resize(_skeleton.num_joints());

  // Root translation, linear in time, relative to rest.
  RawAnimation::JointTrack& root_track = animation.tracks[0];
  root_track.translations.push_back({0.f, Float3(0.f, 1.f, 0.f)});
  root_track.translations.push_back({1.f, Float3(0.4f, 0.8f, 0.3f)});

  const float times[] = {0.f, 0.25f, 0.5f, 0.75f, 1.f};
  struct Motion {
    Float3 axis;
    float base;
    float amplitude;
    float phase;
  };
  // One motion per source joint, in depth-first order.
  const Motion motions[] = {
      {Float3(0.f, 1.f, 0.f), 0.f, 18.f, 0.f},     // root
      {Float3(1.f, 0.2f, 0.f), 4.f, 22.f, 0.7f},   // spine
      {Float3(0.f, 0.3f, 1.f), -6.f, 15.f, 1.4f},  // neck
      {Float3(1.f, 0.f, 0.2f), 8.f, 26.f, 2.1f},   // head
      {Float3(0.2f, 1.f, 0.f), -12.f, 32.f, 2.8f}, // arm
      {Float3(1.f, 1.f, 0.f), 14.f, 40.f, 3.5f},   // hand
      {Float3(0.f, 0.f, 1.f), -9.f, 20.f, 4.2f},   // leg
  };
  RETARGET_CHECK(OZZ_ARRAY_SIZE(motions) ==
                 static_cast<size_t>(_skeleton.num_joints()));

  for (int i = 0; i < _skeleton.num_joints(); ++i) {
    if (_empty_head_rotation && i == 3) {
      continue;  // Head rotation channel stays empty (identity convention).
    }
    for (const float time : times) {
      const float angle =
          motions[i].base +
          motions[i].amplitude * std::sin(2.f * kPi * time + motions[i].phase);
      animation.tracks[i].rotations.push_back(
          {time, AxisAngle(motions[i].axis, angle)});
    }
  }
  return animation;
}

// Mapping for the target skeleton above:
//  0 root       -> 0 source root
//  1 spine_a    -> 1 source spine
//  2 spine_b    -> 2 source neck
//  3 neck       -> 2 source neck
//  4 head       -> 3 source head
//  5 arm        -> 4 source arm
//  6 hand       -> -1 (unmapped, intermediate)
//  7 finger     -> 5 source hand
//  8 leg        -> 6 source leg
//  9 foot       -> -1 (unmapped leaf)
const int kMapping[] = {0, 1, 2, 2, 3, 4, -1, 5, 6, -1};

constexpr int kSourceRoot = 0;
constexpr int kTargetRoot = 0;

// Probe times covering [0, duration], strictly increasing.
const float kProbeTimes[] = {0.f,  0.1f, 0.2f, 0.3f, 0.4f, 0.5f,
                             0.6f, 0.7f, 0.8f, 0.9f, 1.f};

// A target pose, in model space.
struct Pose {
  ozz::vector<Float3> origins;
  ozz::vector<Quaternion> rotations;
};

// Local-to-model accumulation from a set of local transforms.
void ForwardKinematics(const Skeleton& _skeleton,
                       const ozz::vector<Transform>& _locals, Pose* _pose) {
  const int num_joints = _skeleton.num_joints();
  const ozz::span<const int16_t> parents = _skeleton.joint_parents();
  _pose->origins.resize(num_joints);
  _pose->rotations.resize(num_joints);
  for (int i = 0; i < num_joints; ++i) {
    const int parent = parents[i];
    const Quaternion local_rotation =
        ozz::math::NormalizeSafe(_locals[i].rotation, Quaternion::identity());
    if (parent == Skeleton::kNoParent) {
      _pose->rotations[i] = local_rotation;
      _pose->origins[i] = _locals[i].translation;
    } else {
      _pose->rotations[i] = ozz::math::NormalizeSafe(
          _pose->rotations[parent] * local_rotation, Quaternion::identity());
      _pose->origins[i] =
          _pose->origins[parent] + ozz::math::TransformVector(
                                       _pose->rotations[parent],
                                       _locals[i].translation);
    }
  }
}

// Computes model-space rest rotations of a skeleton, independently from the
// retargeter.
ozz::vector<Quaternion> RestModelRotations(const Skeleton& _skeleton) {
  const int num_joints = _skeleton.num_joints();
  const ozz::span<const int16_t> parents = _skeleton.joint_parents();
  ozz::vector<Quaternion> rotations(num_joints);
  for (int i = 0; i < num_joints; ++i) {
    const Transform rest =
        ozz::animation::GetJointRestPoseLocalSpace(_skeleton, i);
    const Quaternion rest_rotation =
        ozz::math::NormalizeSafe(rest.rotation, Quaternion::identity());
    const int parent = parents[i];
    rotations[i] =
        parent == Skeleton::kNoParent
            ? rest_rotation
            : ozz::math::NormalizeSafe(rotations[parent] * rest_rotation,
                                       Quaternion::identity());
  }
  return rotations;
}

// Context shared by all independent reference computations.
struct ReferenceContext {
  const Skeleton* source = nullptr;
  const Skeleton* target = nullptr;
  ozz::vector<Transform> source_rest_locals;
  ozz::vector<Transform> target_rest_locals;
  ozz::vector<Quaternion> source_rest_model_rotations;
  ozz::vector<Quaternion> target_rest_model_rotations;
};

ReferenceContext MakeReferenceContext(const Skeleton& _source,
                                      const Skeleton& _target) {
  ReferenceContext context;
  context.source = &_source;
  context.target = &_target;
  context.source_rest_locals.resize(_source.num_joints());
  context.target_rest_locals.resize(_target.num_joints());
  for (int i = 0; i < _source.num_joints(); ++i) {
    context.source_rest_locals[i] =
        ozz::animation::GetJointRestPoseLocalSpace(_source, i);
  }
  for (int i = 0; i < _target.num_joints(); ++i) {
    context.target_rest_locals[i] =
        ozz::animation::GetJointRestPoseLocalSpace(_target, i);
  }
  context.source_rest_model_rotations = RestModelRotations(_source);
  context.target_rest_model_rotations = RestModelRotations(_target);
  return context;
}

// Describes a retargeting mapping, shared by the reference computation and the
// tests. By default this is the kMapping table above.
struct Mapping {
  const int* joints = kMapping;
  size_t size = OZZ_ARRAY_SIZE(kMapping);
  int target_root = kTargetRoot;
  int source_root = kSourceRoot;
};

// Computes the expected target reference pose at _time, independently from the
// retargeter implementation.
// Model rotation of a mapped target joint is Qs * inverse(Rs) * Rt; its local
// rotation is computed from the actual target parent model rotation at the same
// time. Unmapped joints keep their rest local transform.
Pose ReferencePose(const ReferenceContext& _context,
                   const RawAnimation& _animation, float _time,
                   float _root_scale, const Mapping& _mapping) {
  const Skeleton& source = *_context.source;
  const Skeleton& target = *_context.target;
  const int num_source = source.num_joints();
  const int num_target = target.num_joints();

  // Samples source animation.
  ozz::vector<Transform> source_locals(num_source);
  const bool sampled =
      SampleAnimation(_animation, _time, ozz::make_span(source_locals));
  RETARGET_CHECK(sampled);

  // Source model-space rotations.
  const ozz::span<const int16_t> source_parents = source.joint_parents();
  ozz::vector<Quaternion> source_model_rotations(num_source);
  for (int i = 0; i < num_source; ++i) {
    const Quaternion local = ozz::math::NormalizeSafe(
        source_locals[i].rotation, Quaternion::identity());
    const int parent = source_parents[i];
    source_model_rotations[i] =
        parent == Skeleton::kNoParent
            ? local
            : ozz::math::NormalizeSafe(source_model_rotations[parent] * local,
                                       Quaternion::identity());
  }

  // Target local transforms.
  const ozz::span<const int16_t> target_parents = target.joint_parents();
  ozz::vector<Transform> target_locals(num_target);
  Pose pose;
  pose.origins.resize(num_target);
  pose.rotations.resize(num_target);
  for (int i = 0; i < num_target; ++i) {
    const int parent = target_parents[i];
    const Quaternion parent_model_rotation =
        parent == Skeleton::kNoParent ? Quaternion::identity()
                                      : pose.rotations[parent];
    const int source_joint = _mapping.joints[i];
    Quaternion local_rotation;
    if (source_joint >= 0) {
      const Quaternion desired_model_rotation =
          source_model_rotations[source_joint] *
          ozz::math::Conjugate(
              _context.source_rest_model_rotations[source_joint]) *
          _context.target_rest_model_rotations[i];
      local_rotation = ozz::math::NormalizeSafe(
          ozz::math::Conjugate(parent_model_rotation) * desired_model_rotation,
          Quaternion::identity());
    } else {
      local_rotation = ozz::math::NormalizeSafe(
          _context.target_rest_locals[i].rotation, Quaternion::identity());
    }
    target_locals[i].rotation = local_rotation;
    target_locals[i].scale = Float3::one();

    Float3 translation = _context.target_rest_locals[i].translation;
    if (i == _mapping.target_root && source_joint == _mapping.source_root) {
      translation = translation +
                    (source_locals[_mapping.source_root].translation -
                     _context.source_rest_locals[_mapping.source_root]
                         .translation) *
                        _root_scale;
    }
    target_locals[i].translation = translation;

    pose.rotations[i] = ozz::math::NormalizeSafe(
        parent_model_rotation * local_rotation, Quaternion::identity());
    pose.origins[i] =
        parent == Skeleton::kNoParent
            ? translation
            : pose.origins[parent] +
                  ozz::math::TransformVector(pose.rotations[parent],
                                             translation);
  }
  return pose;
}

// Errors between two poses.
struct PoseError {
  float position = 0.f;
  float angle = 0.f;
};

PoseError ComputeError(const Pose& _pose, const Pose& _reference) {
  PoseError error;
  const size_t num_joints = _reference.origins.size();
  RETARGET_CHECK(_pose.origins.size() == num_joints);
  for (size_t i = 0; i < num_joints; ++i) {
    error.position = ozz::math::Max(
        error.position,
        ozz::math::Length(_pose.origins[i] - _reference.origins[i]));
    error.angle =
        ozz::math::Max(error.angle, AngleBetween(_pose.rotations[i],
                                                 _reference.rotations[i]));
  }
  return error;
}

// Samples a raw animation and computes its target pose.
Pose SamplePose(const RawAnimation& _animation, const Skeleton& _target,
                float _time) {
  ozz::vector<Transform> locals(_target.num_joints());
  const bool sampled =
      SampleAnimation(_animation, _time, ozz::make_span(locals));
  RETARGET_CHECK(sampled);
  Pose pose;
  ForwardKinematics(_target, locals, &pose);
  return pose;
}

// Returns the union of key times of a raw animation, checking that every joint
// shares the exact same key time sequence.
ozz::vector<float> SharedTimeSequence(const RawAnimation& _animation) {
  ozz::vector<float> times;
  if (_animation.tracks.empty()) {
    return times;
  }
  const RawAnimation::JointTrack& first = _animation.tracks[0];
  for (const auto& key : first.translations) {
    times.push_back(key.time);
  }
  for (const auto& key : first.rotations) {
    times.push_back(key.time);
  }
  std::sort(times.begin(), times.end());
  times.erase(std::unique(times.begin(), times.end()), times.end());

  for (const RawAnimation::JointTrack& track : _animation.tracks) {
    ozz::vector<float> other;
    for (const auto& key : track.translations) {
      other.push_back(key.time);
    }
    for (const auto& key : track.rotations) {
      other.push_back(key.time);
    }
    std::sort(other.begin(), other.end());
    other.erase(std::unique(other.begin(), other.end()), other.end());
    RETARGET_CHECK(other.size() == times.size());
    for (size_t i = 0; i < times.size(); ++i) {
      RETARGET_CHECK(other[i] == times[i]);
    }
    // Scales must remain unit, ie tracks are empty.
    RETARGET_CHECK(track.scales.empty());
    RETARGET_CHECK(track.translations.size() == times.size());
    RETARGET_CHECK(track.rotations.size() == times.size());
  }
  return times;
}

// Removes, from every joint, the key at time _time.
void RemoveTime(RawAnimation* _animation, float _time) {
  for (RawAnimation::JointTrack& track : _animation->tracks) {
    auto erase_time = [=](auto& _keys) {
      _keys.erase(std::remove_if(_keys.begin(), _keys.end(),
                                 [=](const auto& _key) {
                                   return _key.time == _time;
                                 }),
                  _keys.end());
    };
    erase_time(track.translations);
    erase_time(track.rotations);
    erase_time(track.scales);
  }
}

// Computes the worst pose error of _animation at all probe times, compared to
// the independently computed reference poses.
PoseError EvaluateAtProbes(const RawAnimation& _animation,
                           const ReferenceContext& _context,
                           const ozz::vector<Pose>& _references) {
  PoseError worst;
  for (size_t p = 0; p < _references.size(); ++p) {
    const Pose pose = SamplePose(_animation, *_context.target, kProbeTimes[p]);
    const PoseError error = ComputeError(pose, _references[p]);
    worst.position = ozz::math::Max(worst.position, error.position);
    worst.angle = ozz::math::Max(worst.angle, error.angle);
  }
  return worst;
}

// Computes reference poses at all probe times.
ozz::vector<Pose> ReferencePoses(const ReferenceContext& _context,
                                 const RawAnimation& _animation,
                                 float _root_scale,
                                 const Mapping& _mapping = Mapping()) {
  ozz::vector<Pose> references;
  for (const float time : kProbeTimes) {
    references.push_back(
        ReferencePose(_context, _animation, time, _root_scale, _mapping));
  }
  return references;
}

RawAnimationRetargeter::Params MakeParams(float _position_tolerance,
                                          float _angle_tolerance,
                                          float _root_scale,
                                          const Mapping& _mapping = Mapping()) {
  RawAnimationRetargeter::Params params;
  params.source_joint_indices =
      ozz::span<const int>(_mapping.joints, _mapping.size);
  params.probe_times = ozz::make_span(kProbeTimes);
  params.position_tolerance = _position_tolerance;
  params.angle_tolerance = _angle_tolerance;
  params.root_scale = _root_scale;
  return params;
}

}  // namespace

// Verifies the basic retargeting equations: output sampled at every probe time
// matches the independently computed reference pose, key times are shared by
// all joints, are a subset of probe times, include both ends, and the output
// is not built from one key per probe.
RETARGET_CASE(RetargetProbeAccuracy) {
  ozz::unique_ptr<Skeleton> source = BuildSourceSkeleton();
  ozz::unique_ptr<Skeleton> target = BuildTargetSkeleton();
  RETARGET_CHECK(source && source->num_joints() == 7);
  RETARGET_CHECK(target && target->num_joints() == 10);
  // Joint counts aren't multiples of 4.
  RETARGET_CHECK(source->num_joints() % 4 != 0);
  RETARGET_CHECK(target->num_joints() % 4 != 0);

  const RawAnimation input = BuildSourceAnimation(*source);
  const ReferenceContext context = MakeReferenceContext(*source, *target);
  const ozz::vector<Pose> references = ReferencePoses(context, input, 1.f);

  const float position_tolerance = 0.02f;
  const float angle_tolerance = 0.03f;
  const RawAnimationRetargeter::Params params =
      MakeParams(position_tolerance, angle_tolerance, 1.f);

  RawAnimation output;
  RETARGET_CHECK(RawAnimationRetargeter()(input, *source, *target, params,
                                          &output));
  RETARGET_CHECK(output.Validate());
  RETARGET_CHECK(output.num_tracks() == target->num_joints());
  RETARGET_CHECK(output.duration == input.duration);

  const ozz::vector<float> times = SharedTimeSequence(output);
  RETARGET_CHECK(times.size() >= 2);
  RETARGET_CHECK(times.front() == kProbeTimes[0]);
  RETARGET_CHECK(times.back() == kProbeTimes[10]);
  // Not one key per input sample.
  RETARGET_CHECK(times.size() < OZZ_ARRAY_SIZE(kProbeTimes));
  // Every key time comes from the probe times.
  for (const float time : times) {
    bool found = false;
    for (const float probe : kProbeTimes) {
      found = found || (time == probe);
    }
    RETARGET_CHECK(found);
  }

  const PoseError error = EvaluateAtProbes(output, context, references);
  RETARGET_CHECK(error.position <= position_tolerance);
  RETARGET_CHECK(error.angle <= angle_tolerance);
  RETARGET_CHECK(error.position > 0.f);  // Interpolation is exercised.
}

// Verifies root displacement, including root_scale and the target rest offset.
RETARGET_CASE(RetargetRootDisplacement) {
  ozz::unique_ptr<Skeleton> source = BuildSourceSkeleton();
  ozz::unique_ptr<Skeleton> target = BuildTargetSkeleton();
  const RawAnimation input = BuildSourceAnimation(*source);
  const ReferenceContext context = MakeReferenceContext(*source, *target);

  const float root_scale = 2.5f;
  const RawAnimationRetargeter::Params params =
      MakeParams(0.02f, 0.03f, root_scale);

  RawAnimation output;
  RETARGET_CHECK(RawAnimationRetargeter()(input, *source, *target, params,
                                          &output));
  RETARGET_CHECK(output.Validate());

  const Float3 target_rest = context.target_rest_locals[kTargetRoot].translation;
  const Float3 source_rest = context.source_rest_locals[kSourceRoot].translation;

  for (const float time : kProbeTimes) {
    // Source root translation is linear, so reading it from the input animation
    // is the independent expected value.
    ozz::vector<Transform> source_locals(source->num_joints());
    RETARGET_CHECK(
        SampleAnimation(input, time, ozz::make_span(source_locals)));
    const Float3 expected =
        target_rest + (source_locals[kSourceRoot].translation - source_rest) *
                          root_scale;

    ozz::vector<Transform> output_locals(target->num_joints());
    RETARGET_CHECK(
        SampleAnimation(output, time, ozz::make_span(output_locals)));
    const Float3 actual = output_locals[kTargetRoot].translation;
    RETARGET_CHECK(ozz::math::Length(actual - expected) <= 1e-5f);
  }

  // With root_scale == 1, the same check applies without scaling.
  const RawAnimationRetargeter::Params unit_params =
      MakeParams(0.02f, 0.03f, 1.f);
  RawAnimation unit_output;
  RETARGET_CHECK(RawAnimationRetargeter()(input, *source, *target, unit_params,
                                          &unit_output));
  ozz::vector<Transform> unit_locals(target->num_joints());
  RETARGET_CHECK(
      SampleAnimation(unit_output, 0.5f, ozz::make_span(unit_locals)));
  RETARGET_CHECK(ozz::math::Length(
                     unit_locals[kTargetRoot].translation -
                     (target_rest + (Float3(0.2f, 0.9f, 0.15f) - source_rest))) <=
                 1e-5f);

  // An unmapped target root stays at its rest position, even though root_scale
  // is not one.
  {
    int unmapped_root_mapping[10];
    std::copy(std::begin(kMapping), std::end(kMapping), unmapped_root_mapping);
    unmapped_root_mapping[kTargetRoot] = -1;
    const Mapping unmapped_root{unmapped_root_mapping,
                                OZZ_ARRAY_SIZE(unmapped_root_mapping),
                                kTargetRoot, kSourceRoot};

    const RawAnimationRetargeter::Params root_params =
        MakeParams(0.02f, 0.03f, 3.f, unmapped_root);
    RawAnimation root_output;
    RETARGET_CHECK(RawAnimationRetargeter()(input, *source, *target,
                                            root_params, &root_output));
    RETARGET_CHECK(root_output.Validate());
    for (const float time : kProbeTimes) {
      ozz::vector<Transform> root_locals(target->num_joints());
      RETARGET_CHECK(SampleAnimation(root_output, time,
                                     ozz::make_span(root_locals)));
      RETARGET_CHECK(ozz::math::Length(root_locals[kTargetRoot].translation -
                                       target_rest) <= 1e-6f);
    }

    // The full reference pose (unmapped root, mapped descendants) is followed.
    const ozz::vector<Pose> root_references =
        ReferencePoses(context, input, 3.f, unmapped_root);
    const PoseError root_error =
        EvaluateAtProbes(root_output, context, root_references);
    RETARGET_CHECK(root_error.position <= 0.02f);
    RETARGET_CHECK(root_error.angle <= 0.03f);
  }
}

// Verifies that unmapped joints keep their rest local pose, still propagate to
// their descendants, and that pure target-only intermediate bones are driven
// correctly.
RETARGET_CASE(RetargetUnmappedBranch) {
  ozz::unique_ptr<Skeleton> source = BuildSourceSkeleton();
  ozz::unique_ptr<Skeleton> target = BuildTargetSkeleton();
  const RawAnimation input = BuildSourceAnimation(*source);
  const ReferenceContext context = MakeReferenceContext(*source, *target);
  const ozz::vector<Pose> references = ReferencePoses(context, input, 1.f);

  const RawAnimationRetargeter::Params params =
      MakeParams(0.02f, 0.03f, 1.f);
  RawAnimation output;
  RETARGET_CHECK(RawAnimationRetargeter()(input, *source, *target, params,
                                          &output));

  // Unmapped intermediate (hand, joint 6) and unmapped leaf (foot, joint 9)
  // keep their target rest local transform at every key.
  const int unmapped_joints[] = {6, 9};
  for (const int joint : unmapped_joints) {
    const Transform rest = context.target_rest_locals[joint];
    for (const auto& key : output.tracks[joint].translations) {
      RETARGET_CHECK(ozz::math::Length(key.value - rest.translation) <= 1e-6f);
    }
    for (const auto& key : output.tracks[joint].rotations) {
      RETARGET_CHECK(AngleBetween(key.value, rest.rotation) <= 1e-6f);
    }
  }

  // The mapped descendant of the unmapped hand (finger, joint 7) is still
  // driven: its reference motion is followed and is not the rest pose.
  const int finger = 7;
  RETARGET_CHECK(references[0].rotations.size() == 10);
  float finger_motion = 0.f;
  for (size_t p = 0; p < references.size(); ++p) {
    const Pose pose = SamplePose(output, *target, kProbeTimes[p]);
    finger_motion = ozz::math::Max(
        finger_motion,
        AngleBetween(pose.rotations[finger], references[p].rotations[finger]));
    RETARGET_CHECK(ozz::math::Length(pose.origins[finger] -
                                     references[p].origins[finger]) <= 0.02f);
    RETARGET_CHECK(AngleBetween(pose.rotations[finger],
                                references[p].rotations[finger]) <= 0.03f);
  }
  RETARGET_CHECK(finger_motion <= 0.03f);

  // The finger reference itself moves away from its rest model rotation, so
  // propagation through the unmapped hand is genuinely exercised.
  const Quaternion rest_finger_model = context.target_rest_model_rotations[finger];
  float reference_deviation = 0.f;
  for (const Pose& reference : references) {
    reference_deviation =
        ozz::math::Max(reference_deviation,
                       AngleBetween(reference.rotations[finger],
                                    rest_finger_model));
  }
  RETARGET_CHECK(reference_deviation > 0.05f);
}

// Verifies that every retained internal key time is necessary: removing it
// from every joint track violates at least one probe constraint.
RETARGET_CASE(RetargetInternalKeysNecessary) {
  ozz::unique_ptr<Skeleton> source = BuildSourceSkeleton();
  ozz::unique_ptr<Skeleton> target = BuildTargetSkeleton();
  const RawAnimation input = BuildSourceAnimation(*source);
  const ReferenceContext context = MakeReferenceContext(*source, *target);
  const ozz::vector<Pose> references = ReferencePoses(context, input, 1.f);

  const float position_tolerance = 0.02f;
  const float angle_tolerance = 0.03f;
  const RawAnimationRetargeter::Params params =
      MakeParams(position_tolerance, angle_tolerance, 1.f);
  RawAnimation output;
  RETARGET_CHECK(RawAnimationRetargeter()(input, *source, *target, params,
                                          &output));

  const ozz::vector<float> times = SharedTimeSequence(output);
  RETARGET_CHECK(times.size() >= 3);  // At least one internal key.

  // The complete output is within tolerances.
  const PoseError error = EvaluateAtProbes(output, context, references);
  RETARGET_CHECK(error.position <= position_tolerance);
  RETARGET_CHECK(error.angle <= angle_tolerance);

  // Removing any single internal time must violate at least one constraint.
  for (size_t k = 1; k + 1 < times.size(); ++k) {
    RawAnimation without = output;
    RemoveTime(&without, times[k]);
    RETARGET_CHECK(without.Validate());
    const PoseError removal_error =
        EvaluateAtProbes(without, context, references);
    RETARGET_CHECK(removal_error.position > position_tolerance ||
                   removal_error.angle > angle_tolerance);
  }
}

// Verifies empty channel convention: a joint with no rotation key follows the
// SampleAnimation() identity convention, and the retargeted descendant matches
// the independent reference.
RETARGET_CASE(RetargetEmptyChannels) {
  ozz::unique_ptr<Skeleton> source = BuildSourceSkeleton();
  ozz::unique_ptr<Skeleton> target = BuildTargetSkeleton();
  const RawAnimation input = BuildSourceAnimation(*source, true);
  RETARGET_CHECK(input.tracks[3].rotations.empty());
  const ReferenceContext context = MakeReferenceContext(*source, *target);
  const ozz::vector<Pose> references = ReferencePoses(context, input, 1.f);

  const RawAnimationRetargeter::Params params =
      MakeParams(0.02f, 0.03f, 1.f);
  RawAnimation output;
  RETARGET_CHECK(RawAnimationRetargeter()(input, *source, *target, params,
                                          &output));
  RETARGET_CHECK(output.Validate());

  // Head (target joint 4) is driven by the empty channel source joint 3, so its
  // source model rotation is the identity-composed rest chain.
  const PoseError error = EvaluateAtProbes(output, context, references);
  RETARGET_CHECK(error.position <= 0.02f);
  RETARGET_CHECK(error.angle <= 0.03f);

  // The reference pose itself was computed from an empty channel: sampling the
  // source at t = 0.3 gives identity local rotation for joint 3.
  ozz::vector<Transform> source_locals(source->num_joints());
  RETARGET_CHECK(
      SampleAnimation(input, 0.3f, ozz::make_span(source_locals)));
  RETARGET_CHECK(AngleBetween(source_locals[3].rotation,
                              Quaternion::identity()) <= 1e-6f);
}

// Verifies that invalid inputs are rejected, don't modify the sources, and
// leave the output in a readable (valid) state.
RETARGET_CASE(RetargetInvalidInputs) {
  ozz::unique_ptr<Skeleton> source = BuildSourceSkeleton();
  ozz::unique_ptr<Skeleton> target = BuildTargetSkeleton();
  const RawAnimation input = BuildSourceAnimation(*source);
  const ReferenceContext context = MakeReferenceContext(*source, *target);

  const int source_tracks = input.num_tracks();
  const size_t input_size = input.size();
  const float first_angle = input.tracks[0].rotations[0].value.w;
  const char* source_root_name = source->joint_names()[0];

  const RawAnimationRetargeter::Params valid_params =
      MakeParams(0.02f, 0.03f, 1.f);

  auto expect_failure = [&](const RawAnimation& _animation,
                            const Skeleton& _source_skeleton,
                            const Skeleton& _target_skeleton,
                            const RawAnimationRetargeter::Params& _params) {
    RawAnimation output;
    output.duration = 0.5f;
    RETARGET_CHECK(!RawAnimationRetargeter()(_animation, _source_skeleton,
                                             _target_skeleton, _params,
                                             &output));
    // Output remains readable (valid), even after a failure.
    RETARGET_CHECK(output.Validate());
    RETARGET_CHECK(output.num_tracks() == 0);
  };

  {  // Mapping size doesn't match the target joint count.
    RawAnimationRetargeter::Params params = valid_params;
    const int short_mapping[] = {0, 1, 2};
    params.source_joint_indices = ozz::make_span(short_mapping);
    expect_failure(input, *source, *target, params);
  }
  {  // Mapping references an out-of-range source joint.
    RawAnimationRetargeter::Params params = valid_params;
    int bad_mapping[10];
    std::copy(std::begin(kMapping), std::end(kMapping), bad_mapping);
    bad_mapping[1] = source->num_joints();
    params.source_joint_indices = ozz::make_span(bad_mapping);
    expect_failure(input, *source, *target, params);
  }
  {  // Target root mapped to a non-root source joint.
    RawAnimationRetargeter::Params params = valid_params;
    int bad_mapping[10];
    std::copy(std::begin(kMapping), std::end(kMapping), bad_mapping);
    bad_mapping[kTargetRoot] = 1;
    params.source_joint_indices = ozz::make_span(bad_mapping);
    expect_failure(input, *source, *target, params);
  }
  {  // Source track count differs from source joint count.
    RawAnimation bad_animation = input;
    bad_animation.tracks.resize(source->num_joints() - 1);
    RETARGET_CHECK(bad_animation.Validate());
    expect_failure(bad_animation, *source, *target, valid_params);
  }
  {  // Probe times don't cover 0 and/or duration.
    RawAnimationRetargeter::Params params = valid_params;
    const float bad_probes[] = {0.1f, 0.5f, 1.f};
    params.probe_times = ozz::make_span(bad_probes);
    expect_failure(input, *source, *target, params);
  }
  {
    RawAnimationRetargeter::Params params = valid_params;
    const float bad_probes[] = {0.f, 0.5f, 0.9f};
    params.probe_times = ozz::make_span(bad_probes);
    expect_failure(input, *source, *target, params);
  }
  {  // Probe times aren't strictly increasing.
    RawAnimationRetargeter::Params params = valid_params;
    const float bad_probes[] = {0.f, 0.5f, 0.5f, 1.f};
    params.probe_times = ozz::make_span(bad_probes);
    expect_failure(input, *source, *target, params);
  }
  {  // Probe times aren't finite.
    RawAnimationRetargeter::Params params = valid_params;
    const float bad_probes[] = {0.f, 0.5f,
                                std::numeric_limits<float>::quiet_NaN(), 1.f};
    params.probe_times = ozz::make_span(bad_probes);
    expect_failure(input, *source, *target, params);
  }
  {  // Non-unit source scale keys aren't supported.
    RawAnimation bad_animation = input;
    bad_animation.tracks[2].scales.push_back({0.5f, Float3(2.f, 1.f, 1.f)});
    RETARGET_CHECK(bad_animation.Validate());
    expect_failure(bad_animation, *source, *target, valid_params);
  }
  {  // Non-unit rest scale isn't supported.
    RawSkeleton raw;
    raw.roots.resize(1);
    raw.roots[0].name = "scaled";
    raw.roots[0].transform.scale = Float3(2.f, 1.f, 1.f);
    ozz::unique_ptr<Skeleton> scaled = SkeletonBuilder()(raw);
    RETARGET_CHECK(scaled && scaled->num_joints() == 1);
    RawAnimation single;
    single.duration = 1.f;
    single.tracks.resize(1);
    RawAnimationRetargeter::Params params = valid_params;
    const int single_mapping[] = {0};
    const float single_probes[] = {0.f, 1.f};
    params.source_joint_indices = ozz::make_span(single_mapping);
    params.probe_times = ozz::make_span(single_probes);
    expect_failure(single, *scaled, *scaled, params);
  }
  {  // Invalid tolerances and root scale.
    RawAnimationRetargeter::Params params = valid_params;
    params.position_tolerance = 0.f;
    expect_failure(input, *source, *target, params);
    params = valid_params;
    params.angle_tolerance = -1.f;
    expect_failure(input, *source, *target, params);
    params = valid_params;
    params.root_scale = 0.f;
    expect_failure(input, *source, *target, params);
  }

  // Sources weren't modified.
  RETARGET_CHECK(input.num_tracks() == source_tracks);
  RETARGET_CHECK(input.size() == input_size);
  RETARGET_CHECK(input.tracks[0].rotations[0].value.w == first_angle);
  RETARGET_CHECK(std::string(source->joint_names()[0]) == source_root_name);
}

// Verifies the runtime pipeline: AnimationBuilder -> SamplingJob ->
// LocalToModelJob follows the reference poses, with a 1e-3 margin reserved for
// runtime compression.
RETARGET_CASE(RetargetRuntimePipeline) {
  ozz::unique_ptr<Skeleton> source = BuildSourceSkeleton();
  ozz::unique_ptr<Skeleton> target = BuildTargetSkeleton();
  const RawAnimation input = BuildSourceAnimation(*source);
  const ReferenceContext context = MakeReferenceContext(*source, *target);
  const float root_scale = 1.5f;
  const ozz::vector<Pose> references =
      ReferencePoses(context, input, root_scale);

  const float position_tolerance = 0.005f;
  const float angle_tolerance = 0.005f;
  const RawAnimationRetargeter::Params params =
      MakeParams(position_tolerance, angle_tolerance, root_scale);
  RawAnimation raw_output;
  RETARGET_CHECK(RawAnimationRetargeter()(input, *source, *target, params,
                                          &raw_output));

  // The raw output itself is within the offline tolerances.
  const PoseError offline_error =
      EvaluateAtProbes(raw_output, context, references);
  RETARGET_CHECK(offline_error.position <= position_tolerance);
  RETARGET_CHECK(offline_error.angle <= angle_tolerance);

  ozz::unique_ptr<Animation> animation = AnimationBuilder()(raw_output);
  RETARGET_CHECK(animation != nullptr);
  RETARGET_CHECK(animation->num_tracks() == target->num_joints());

  SamplingJob::Context sampling_context(target->num_joints());
  ozz::vector<ozz::math::SoaTransform> locals(target->num_soa_joints());
  ozz::vector<Float4x4> models(target->num_joints());

  const float runtime_margin = 1e-3f;
  for (size_t p = 0; p < references.size(); ++p) {
    SamplingJob sampling_job;
    sampling_job.animation = animation.get();
    sampling_job.context = &sampling_context;
    sampling_job.ratio = kProbeTimes[p] / animation->duration();
    sampling_job.output = ozz::make_span(locals);
    RETARGET_CHECK(sampling_job.Run());

    LocalToModelJob local_to_model_job;
    local_to_model_job.skeleton = target.get();
    local_to_model_job.input = ozz::make_span(locals);
    local_to_model_job.output = ozz::make_span(models);
    RETARGET_CHECK(local_to_model_job.Run());

    for (int i = 0; i < target->num_joints(); ++i) {
      Float3 translation;
      Quaternion rotation;
      Float3 scale;
      RETARGET_CHECK(ozz::math::ToAffine(models[i], &translation, &rotation,
                                         &scale));
      // Unit scale is preserved.
      RETARGET_CHECK(ozz::math::Length(scale - Float3::one()) <= 1e-4f);
      const float position_error =
          ozz::math::Length(translation - references[p].origins[i]);
      const float angle_error = AngleBetween(rotation, references[p].rotations[i]);
      RETARGET_CHECK(position_error <= position_tolerance + runtime_margin);
      RETARGET_CHECK(angle_error <= angle_tolerance + runtime_margin);
    }
  }
}

// Verifies determinism, and that negating source quaternions doesn't change
// the resulting motion or the key time sequence.
RETARGET_CASE(RetargetDeterministicAndSignInvariant) {
  ozz::unique_ptr<Skeleton> source = BuildSourceSkeleton();
  ozz::unique_ptr<Skeleton> target = BuildTargetSkeleton();
  const RawAnimation input = BuildSourceAnimation(*source);
  const RawAnimationRetargeter::Params params =
      MakeParams(0.02f, 0.03f, 1.f);

  RawAnimation first;
  RawAnimation second;
  RETARGET_CHECK(RawAnimationRetargeter()(input, *source, *target, params,
                                          &first));
  RETARGET_CHECK(RawAnimationRetargeter()(input, *source, *target, params,
                                          &second));

  // Same inputs produce exactly the same key sequence and values.
  RETARGET_CHECK(first.num_tracks() == second.num_tracks());
  for (int i = 0; i < first.num_tracks(); ++i) {
    RETARGET_CHECK(first.tracks[i].translations.size() ==
                   second.tracks[i].translations.size());
    RETARGET_CHECK(first.tracks[i].rotations.size() ==
                   second.tracks[i].rotations.size());
    for (size_t k = 0; k < first.tracks[i].translations.size(); ++k) {
      RETARGET_CHECK(first.tracks[i].translations[k].time ==
                     second.tracks[i].translations[k].time);
      RETARGET_CHECK(first.tracks[i].translations[k].value ==
                     second.tracks[i].translations[k].value);
    }
    for (size_t k = 0; k < first.tracks[i].rotations.size(); ++k) {
      RETARGET_CHECK(first.tracks[i].rotations[k].time ==
                     second.tracks[i].rotations[k].time);
      RETARGET_CHECK(first.tracks[i].rotations[k].value ==
                     second.tracks[i].rotations[k].value);
    }
  }

  // Negates every source rotation key. Same rotations, opposite quaternions.
  RawAnimation negated = input;
  for (RawAnimation::JointTrack& track : negated.tracks) {
    for (RawAnimation::RotationKey& key : track.rotations) {
      key.value = -key.value;
    }
  }
  RawAnimation negated_output;
  RETARGET_CHECK(RawAnimationRetargeter()(negated, *source, *target, params,
                                          &negated_output));

  const ozz::vector<float> base_times = SharedTimeSequence(first);
  const ozz::vector<float> negated_times = SharedTimeSequence(negated_output);
  RETARGET_CHECK(base_times.size() == negated_times.size());
  for (size_t i = 0; i < base_times.size(); ++i) {
    RETARGET_CHECK(base_times[i] == negated_times[i]);
  }
  for (int i = 0; i < first.num_tracks(); ++i) {
    for (size_t k = 0; k < first.tracks[i].translations.size(); ++k) {
      RETARGET_CHECK(ozz::math::Length(first.tracks[i].translations[k].value -
                                       negated_output.tracks[i]
                                           .translations[k]
                                           .value) <= 1e-6f);
    }
    for (size_t k = 0; k < first.tracks[i].rotations.size(); ++k) {
      RETARGET_CHECK(AngleBetween(first.tracks[i].rotations[k].value,
                                  negated_output.tracks[i].rotations[k].value) <=
                     1e-5f);
    }
  }
}
