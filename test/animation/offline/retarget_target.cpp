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

// Functional tests for ozz::animation::offline::RawAnimationRetargeter.
// Skeletons are generated procedurally: the target uses different rest
// rotations, different bone lengths and extra intermediate helper joints.
// Neither joint count is a multiple of 4. Reference model rotations and
// chain end points are computed independently from the retargeter.

#include <algorithm>
#include <cmath>
#include <vector>

#include "task_support/check.h"

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
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/transform.h"
#include "ozz/base/maths/vec_float.h"
#include "ozz/base/memory/unique_ptr.h"

namespace {

using ozz::animation::Skeleton;
using ozz::animation::offline::AnimationBuilder;
using ozz::animation::offline::RawAnimation;
using ozz::animation::offline::RawAnimationRetargeter;
using ozz::animation::offline::RawSkeleton;
using ozz::animation::offline::SkeletonBuilder;
namespace math = ozz::math;

const float kPi = ozz::math::kPi;

// Numerical tolerances used by the retargeter. They are deliberately loose
// enough to leave the required 1e-3 head room for runtime compression, while
// still being tight against the independent references.
const float kPositionTolerance = 5e-3f;
const float kAngleTolerance = 5e-3f;
const float kRuntimeEpsilon = 1e-3f;

math::Quaternion AxisAngle(const math::Float3& _axis, float _angle) {
  return math::Quaternion::FromAxisAngle(math::Normalize(_axis), _angle);
}

// Appends a child joint with an identity rest rotation.
RawSkeleton::Joint* AddChild(RawSkeleton::Joint* _parent, const char* _name,
                             const math::Float3& _translation) {
  _parent->children.resize(_parent->children.size() + 1);
  RawSkeleton::Joint* child = &_parent->children.back();
  child->name = _name;
  child->transform = math::Transform::identity();
  child->transform.translation = _translation;
  return child;
}

// Appends a child joint with an explicit rest rotation.
RawSkeleton::Joint* AddChild(RawSkeleton::Joint* _parent, const char* _name,
                             const math::Float3& _translation,
                             const math::Quaternion& _rotation) {
  RawSkeleton::Joint* child = AddChild(_parent, _name, _translation);
  child->transform.rotation = _rotation;
  return child;
}

// Independent depth-first flattening of a raw skeleton.
struct FlatSkeleton {
  struct Joint {
    math::Transform local;
    int parent;
  };
  std::vector<Joint> joints;

  void Build(const RawSkeleton& _skeleton) {
    joints.clear();
    Walk(_skeleton.roots, -1);
  }
  void Walk(const RawSkeleton::Joint::Children& _children, int _parent) {
    for (const RawSkeleton::Joint& child : _children) {
      const int index = static_cast<int>(joints.size());
      joints.push_back({child.transform, _parent});
      Walk(child.children, index);
    }
  }
};

struct ModelPose {
  std::vector<math::Float3> translations;
  std::vector<math::Quaternion> rotations;
};

// Independent forward kinematics on local transforms (unit scale).
ModelPose ComputeModel(const std::vector<math::Transform>& _locals,
                       const FlatSkeleton& _skeleton) {
  ModelPose pose;
  const int n = static_cast<int>(_skeleton.joints.size());
  pose.translations.resize(n);
  pose.rotations.resize(n);
  for (int i = 0; i < n; ++i) {
    const int parent = _skeleton.joints[i].parent;
    if (parent < 0) {
      pose.translations[i] = _locals[i].translation;
      pose.rotations[i] = _locals[i].rotation;
    } else {
      pose.translations[i] =
          pose.translations[parent] +
          math::TransformVector(pose.rotations[parent],
                                _locals[i].translation);
      pose.rotations[i] = pose.rotations[parent] * _locals[i].rotation;
    }
  }
  return pose;
}

ModelPose RestModel(const FlatSkeleton& _skeleton) {
  std::vector<math::Transform> locals;
  locals.reserve(_skeleton.joints.size());
  for (const auto& joint : _skeleton.joints) {
    locals.push_back(joint.local);
  }
  return ComputeModel(locals, _skeleton);
}

// Unsigned shortest-path angle between unit quaternions. The relative
// quaternion is measured with atan2, which stays well conditioned for tiny
// angles (acos of a value barely below 1 would otherwise inflate floating
// point noise into a 1e-4 "angle").
float AngleBetween(const math::Quaternion& _a,
                   const math::Quaternion& _b) {
  const math::Quaternion a = math::Normalize(_a);
  const math::Quaternion b = math::Normalize(_b);
  const math::Quaternion relative = math::Conjugate(a) * b;
  const float sine_half =
      std::sqrt(relative.x * relative.x + relative.y * relative.y +
                relative.z * relative.z);
  return 2.f * std::atan2(sine_half, std::abs(relative.w));
}

float Distance(const math::Float3& _a, const math::Float3& _b) {
  return math::Length(_a - _b);
}

std::vector<float> MakeProbes(float _duration, int _count) {
  std::vector<float> probes(_count);
  for (int i = 0; i < _count; ++i) {
    probes[i] = _duration * static_cast<float>(i) /
                static_cast<float>(_count - 1);
  }
  return probes;
}

// Samples a raw animation in local space at _time.
std::vector<math::Transform> SampleLocals(const RawAnimation& _animation,
                                          float _time, int _num_joints) {
  std::vector<math::Transform> locals(_num_joints);
  RETARGET_CHECK(ozz::animation::offline::SampleAnimation(
      _animation, _time, ozz::make_span(locals)));
  return locals;
}

// Runs the retargeter and returns the output raw animation.
RawAnimation RunRetarget(
    const RawAnimation& _input, const Skeleton& _source,
    const Skeleton& _target, const std::vector<int16_t>& _mapping,
    float _root_scale, const std::vector<float>& _probes,
    float _position_tolerance = kPositionTolerance,
    float _angle_tolerance = kAngleTolerance) {
  RawAnimation output;
  const bool ok = RawAnimationRetargeter()(
      _input, _source, _target, ozz::make_span(_mapping), _root_scale,
      ozz::make_span(_probes), _position_tolerance, _angle_tolerance, &output);
  RETARGET_CHECK(ok);
  RETARGET_CHECK(output.Validate());
  RETARGET_CHECK(output.num_tracks() == _target.num_joints());
  return output;
}

// Expected target model posture at a probe time, computed independently from
// the retargeter formulas.
ModelPose ExpectedTarget(
    const std::vector<math::Transform>& _source_locals,
    const FlatSkeleton& _source_flat, const FlatSkeleton& _target_flat,
    const ModelPose& _source_rest, const ModelPose& _target_rest,
    const std::vector<int16_t>& _mapping, float _root_scale) {
  const ModelPose source_pose = ComputeModel(_source_locals, _source_flat);
  const int n = static_cast<int>(_target_flat.joints.size());
  ModelPose expected;
  expected.translations.resize(n);
  expected.rotations.resize(n);
  for (int j = 0; j < n; ++j) {
    const int mapped = _mapping[j];
    const int parent = _target_flat.joints[j].parent;
    if (j == 0) {
      if (mapped == 0) {
        expected.translations[0] =
            _target_rest.translations[0] +
            (source_pose.translations[0] -
             _source_rest.translations[0]) *
                _root_scale;
        expected.rotations[0] = source_pose.rotations[0] *
                                math::Conjugate(_source_rest.rotations[0]) *
                                _target_rest.rotations[0];
      } else {
        expected.translations[0] = _target_rest.translations[0];
        expected.rotations[0] = _target_rest.rotations[0];
      }
    } else if (mapped < 0) {
      expected.translations[j] =
          expected.translations[parent] +
          math::TransformVector(expected.rotations[parent],
                                _target_flat.joints[j].local.translation);
      expected.rotations[j] =
          expected.rotations[parent] *
          _target_flat.joints[j].local.rotation;
    } else {
      expected.rotations[j] =
          source_pose.rotations[mapped] *
          math::Conjugate(_source_rest.rotations[mapped]) *
          _target_rest.rotations[j];
      expected.translations[j] =
          expected.translations[parent] +
          math::TransformVector(expected.rotations[parent],
                                _target_flat.joints[j].local.translation);
    }
  }
  return expected;
}

// Checks the raw output against the independent reference at every probe.
void CheckRawAtProbes(const RawAnimation& _output,
                      const FlatSkeleton& _target_flat,
                      const std::vector<float>& _probes,
                      const std::vector<ModelPose>& _expected,
                      float _position_epsilon, float _angle_epsilon) {
  const int n = static_cast<int>(_target_flat.joints.size());
  for (size_t p = 0; p < _probes.size(); ++p) {
    const std::vector<math::Transform> locals =
        SampleLocals(_output, _probes[p], n);
    const ModelPose sampled = ComputeModel(locals, _target_flat);
    for (int j = 0; j < n; ++j) {
      RETARGET_CHECK(Distance(sampled.translations[j],
                              _expected[p].translations[j]) <=
                     _position_epsilon);
      RETARGET_CHECK(AngleBetween(sampled.rotations[j],
                                  _expected[p].rotations[j]) <=
                     _angle_epsilon);
    }
  }
}

// Collects the union of key times of the output (all tracks share them).
std::vector<float> OutputKeyTimes(const RawAnimation& _output) {
  std::vector<float> times;
  for (const RawAnimation::RotationKey& key : _output.tracks[0].rotations) {
    times.push_back(key.time);
  }
  return times;
}

// Pushes the output through AnimationBuilder, SamplingJob and
// LocalToModelJob, and returns the runtime model matrices at _ratio.
std::vector<math::Float4x4> RuntimeModelMatrices(
    const RawAnimation& _output, const Skeleton& _skeleton, float _ratio) {
  const ozz::unique_ptr<ozz::animation::Animation> animation =
      AnimationBuilder()(_output);
  RETARGET_CHECK(animation);

  ozz::vector<math::SoaTransform> locals(_skeleton.num_soa_joints());
  ozz::animation::SamplingJob::Context context(_skeleton.num_joints());
  ozz::animation::SamplingJob sampling;
  sampling.animation = animation.get();
  sampling.context = &context;
  sampling.ratio = _ratio;
  sampling.output = ozz::make_span(locals);
  RETARGET_CHECK(sampling.Run());

  std::vector<math::Float4x4> models(_skeleton.num_joints());
  ozz::animation::LocalToModelJob ltm;
  ltm.skeleton = &_skeleton;
  ltm.input = ozz::make_span(locals);
  ltm.output = ozz::make_span(models);
  RETARGET_CHECK(ltm.Run());
  return models;
}

math::Float3 MatrixTranslation(const math::Float4x4& _matrix) {
  math::Float3 translation;
  math::Store3PtrU(_matrix.cols[3], &translation.x);
  return translation;
}

math::Quaternion MatrixRotation(const math::Float4x4& _matrix) {
  math::Quaternion rotation;
  math::StorePtrU(math::ToQuaternion(_matrix), &rotation.x);
  return rotation;
}

// Builds the source skeleton: a 5-joint main chain plus a side branch
// (7 joints, not a multiple of 4), axis-aligned rest pose.
// Depth-first order: 0 s_root, 1 s_spine, 2 s_arm, 3 s_hand, 4 s_leg,
// 5 s_foot, 6 s_branch.
RawSkeleton BuildSourceSkeleton() {
  RawSkeleton skeleton;
  skeleton.roots.resize(1);
  RawSkeleton::Joint* root = &skeleton.roots[0];
  root->name = "s_root";
  root->transform = math::Transform::identity();
  root->transform.translation = math::Float3(0.f, 1.f, 0.f);

  RawSkeleton::Joint* spine = AddChild(root, "s_spine", math::Float3(0.f, 1.f, 0.f));
  RawSkeleton::Joint* arm =
      AddChild(spine, "s_arm", math::Float3(1.f, 0.f, 0.f));
  AddChild(arm, "s_hand", math::Float3(1.f, 0.f, 0.f));
  RawSkeleton::Joint* leg =
      AddChild(spine, "s_leg", math::Float3(0.f, -1.f, 0.f));
  AddChild(leg, "s_foot", math::Float3(0.f, -1.f, 0.f));
  AddChild(spine, "s_branch", math::Float3(0.f, 0.f, 1.f));
  RETARGET_CHECK(skeleton.num_joints() == 7);
  RETARGET_CHECK(skeleton.num_joints() % 4 != 0);
  return skeleton;
}

// Builds the target skeleton: 9 joints (not a multiple of 4) with tilted rest
// rotations, longer bones and an extra intermediate bone in both the arm and
// the leg chains. Depth-first order:
// 0 t_root, 1 t_spine, 2 t_arm_up, 3 t_arm_mid(extra), 4 t_hand,
// 5 t_leg_up, 6 t_knee(extra), 7 t_foot, 8 t_branch (unmapped branch).
RawSkeleton BuildTargetSkeleton() {
  RawSkeleton skeleton;
  skeleton.roots.resize(1);
  RawSkeleton::Joint* root = &skeleton.roots[0];
  root->name = "t_root";
  root->transform = math::Transform::identity();
  root->transform.translation = math::Float3(0.f, 2.f, 0.f);
  root->transform.rotation = AxisAngle(math::Float3(0.f, 0.f, 1.f), kPi / 8.f);

  RawSkeleton::Joint* spine = AddChild(
      root, "t_spine", math::Float3(0.f, 1.5f, 0.f),
      AxisAngle(math::Float3(1.f, 0.f, 0.f), kPi / 10.f));
  RawSkeleton::Joint* arm_up = AddChild(
      spine, "t_arm_up", math::Float3(1.2f, 0.f, 0.f),
      AxisAngle(math::Float3(0.f, 1.f, 0.f), kPi / 12.f));
  RawSkeleton::Joint* arm_mid = AddChild(
      arm_up, "t_arm_mid", math::Float3(0.8f, 0.f, 0.f),
      AxisAngle(math::Float3(0.f, 0.f, 1.f), kPi / 14.f));
  AddChild(arm_mid, "t_hand", math::Float3(0.7f, 0.f, 0.f),
           AxisAngle(math::Float3(1.f, 0.f, 0.f), kPi / 16.f));
  RawSkeleton::Joint* leg_up = AddChild(
      spine, "t_leg_up", math::Float3(0.f, -1.4f, 0.f),
      AxisAngle(math::Float3(0.f, 0.f, 1.f), -kPi / 12.f));
  RawSkeleton::Joint* knee = AddChild(
      leg_up, "t_knee", math::Float3(0.f, -0.9f, 0.f),
      AxisAngle(math::Float3(1.f, 0.f, 0.f), kPi / 14.f));
  AddChild(knee, "t_foot", math::Float3(0.f, -0.8f, 0.f),
           AxisAngle(math::Float3(0.f, 1.f, 0.f), -kPi / 16.f));
  AddChild(spine, "t_branch", math::Float3(0.f, 0.f, 0.6f),
           AxisAngle(math::Float3(0.f, 1.f, 0.f), kPi / 6.f));
  RETARGET_CHECK(skeleton.num_joints() == 9);
  RETARGET_CHECK(skeleton.num_joints() % 4 != 0);
  return skeleton;
}

// Target joint order (depth first) -> source joint index, -1 when unmapped.
// The extra intermediate bones t_arm_mid and t_knee, and the t_branch side
// bone, are not driven.
const std::vector<int16_t>& StandardMapping() {
  static const std::vector<int16_t> mapping = {
      0,   // t_root    <- s_root
      1,   // t_spine   <- s_spine
      2,   // t_arm_up  <- s_arm
      -1,  // t_arm_mid (extra helper bone)
      3,   // t_hand    <- s_hand
      4,   // t_leg_up  <- s_leg
      -1,  // t_knee    (extra helper bone)
      5,   // t_foot    <- s_foot
      -1,  // t_branch  (unmapped branch)
  };
  return mapping;
}

// Builds an animated source clip: root translates and rotates, every chain
// joint swings around a different axis with a different phase.
RawAnimation BuildSourceAnimation(float _duration) {
  RawAnimation animation;
  animation.duration = _duration;
  animation.name = "retarget_source";
  animation.tracks.resize(7);

  const int frames = 9;
  const struct Key {
    math::Float3 axis;
    float amplitude;
    float phase;
  } keys[7] = {
      {{0.f, 1.f, 0.f}, 0.6f, 0.0f},   // root
      {{1.f, 0.f, 0.f}, 0.5f, 0.7f},   // spine
      {{0.f, 0.f, 1.f}, 0.8f, 1.3f},   // arm
      {{0.f, 1.f, 0.f}, 0.7f, 2.1f},   // hand
      {{1.f, 0.f, 0.f}, 0.6f, 2.9f},   // leg
      {{0.f, 0.f, 1.f}, 0.9f, 3.7f},   // foot
      {{0.f, 1.f, 1.f}, 0.4f, 4.5f},   // unused branch slot, still animated
  };

  for (int f = 0; f < frames; ++f) {
    const float time =
        _duration * static_cast<float>(f) / static_cast<float>(frames - 1);
    const float wave = std::sin(2.f * kPi * f / (frames - 1));
    for (int j = 0; j < 7; ++j) {
      const float angle = keys[j].amplitude *
                          std::sin(2.f * kPi * f / (frames - 1) +
                                   keys[j].phase);
      animation.tracks[j].rotations.push_back(
          {time, AxisAngle(keys[j].axis, angle)});
      animation.tracks[j].scales.push_back({time, math::Float3::one()});
      if (j == 0) {
        // Root motion: starts at its rest translation (1, 0, 0 offset over
        // the clip), no vertical drift.
        const math::Float3 rest(0.f, 1.f, 0.f);
        const math::Float3 translation(rest.x + 2.f * wave, rest.y,
                                       rest.z + 1.5f * wave);
        animation.tracks[j].translations.push_back({time, translation});
      } else {
        // Non root tracks keep a single, rest translation key. Empty
        // translation channels would use the SampleAnimation identity
        // convention, so explicit rest keys are required.
      }
    }
  }

  // Non-root joints need their rest translation even though they do not
  // animate: provide one key at t = 0 (builder fixes up the end key).
  const FlatSkeleton source_flat = [] {
    RawSkeleton source = BuildSourceSkeleton();
    FlatSkeleton flat;
    flat.Build(source);
    return flat;
  }();
  for (int j = 1; j < 7; ++j) {
    animation.tracks[j].translations.push_back(
        {0.f, source_flat.joints[j].local.translation});
  }
  RETARGET_CHECK(animation.Validate());
  return animation;
}

// Shared fixtures.
struct Fixtures {
  RawSkeleton source_skeleton;
  RawSkeleton target_skeleton;
  ozz::unique_ptr<Skeleton> source_runtime;
  ozz::unique_ptr<Skeleton> target_runtime;
  FlatSkeleton source_flat;
  FlatSkeleton target_flat;
  ModelPose source_rest;
  ModelPose target_rest;
  RawAnimation input;
  std::vector<float> probes;

  Fixtures()
      : source_skeleton(BuildSourceSkeleton()),
        target_skeleton(BuildTargetSkeleton()),
        input(BuildSourceAnimation(2.f)),
        probes(MakeProbes(2.f, 13)) {
    source_runtime = SkeletonBuilder()(source_skeleton);
    target_runtime = SkeletonBuilder()(target_skeleton);
    RETARGET_CHECK(source_runtime);
    RETARGET_CHECK(target_runtime);
    source_flat.Build(source_skeleton);
    target_flat.Build(target_skeleton);
    source_rest = RestModel(source_flat);
    target_rest = RestModel(target_flat);
  }
};

}  // namespace

// Case 1: full retarget with intermediate bones and an unmapped branch.
// Probe errors stay inside tolerances, and the output drives AnimationBuilder,
// SamplingJob and LocalToModelJob within the 1e-3 runtime compression budget.
RETARGET_CASE(retarget_full_chain_and_runtime_pipeline) {
  Fixtures fx;
  const RawAnimation output =
      RunRetarget(fx.input, *fx.source_runtime, *fx.target_runtime,
                  StandardMapping(), 1.f, fx.probes);

  // All output key times come from the probe set, both ends are present and
  // every track shares the same times.
  const std::vector<float> key_times = OutputKeyTimes(output);
  RETARGET_CHECK(key_times.front() == 0.f);
  RETARGET_CHECK(key_times.back() == fx.input.duration);
  for (float time : key_times) {
    RETARGET_CHECK(std::find(fx.probes.begin(), fx.probes.end(), time) !=
                   fx.probes.end());
  }
  for (const RawAnimation::JointTrack& track : output.tracks) {
    RETARGET_CHECK(track.rotations.size() == key_times.size());
    RETARGET_CHECK(track.translations.size() == key_times.size());
    RETARGET_CHECK(track.scales.size() == key_times.size());
    for (const auto& scale_key : track.scales) {
      RETARGET_CHECK(scale_key.value.x == 1.f &&
                     scale_key.value.y == 1.f &&
                     scale_key.value.z == 1.f);
    }
  }

  // Independent references at every probe.
  std::vector<ModelPose> expected;
  for (float time : fx.probes) {
    const std::vector<math::Transform> source_locals =
        SampleLocals(fx.input, time, 7);
    expected.push_back(ExpectedTarget(
        source_locals, fx.source_flat, fx.target_flat, fx.source_rest,
        fx.target_rest, StandardMapping(), 1.f));
  }
  CheckRawAtProbes(output, fx.target_flat, fx.probes, expected,
                   kPositionTolerance, kAngleTolerance);

  // Long chain end point (t_hand, index 4) must actually move and match the
  // independent end point reference.
  const math::Float3 hand_at_mid =
      ComputeModel(SampleLocals(output, 1.f, 9), fx.target_flat)
          .translations[4];
  const math::Float3 hand_rest = fx.target_rest.translations[4];
  RETARGET_CHECK(Distance(hand_at_mid, hand_rest) > 0.05f);
  RETARGET_CHECK(Distance(hand_at_mid, expected[fx.probes.size() / 2]
                                            .translations[4]) <=
                 kPositionTolerance);

  // Runtime pipeline check at probe times only: the retargeter only claims
  // tolerance at probe times, not between them. Runtime half-float and
  // quaternion compression must stay within the extra 1e-3 budget.
  RETARGET_CHECK(fx.target_runtime->num_joints() == 9);
  float max_position_error = 0.f;
  float max_angle_error = 0.f;
  for (float probe_time : fx.probes) {
    const float ratio = probe_time / fx.input.duration;
    const std::vector<math::Float4x4> models =
        RuntimeModelMatrices(output, *fx.target_runtime, ratio);
    const std::vector<math::Transform> source_locals =
        SampleLocals(fx.input, probe_time, 7);
    const ModelPose reference = ExpectedTarget(
        source_locals, fx.source_flat, fx.target_flat, fx.source_rest,
        fx.target_rest, StandardMapping(), 1.f);
    for (int j = 0; j < 9; ++j) {
      max_position_error = math::Max(
          max_position_error,
          Distance(MatrixTranslation(models[j]), reference.translations[j]));
      max_angle_error = math::Max(
          max_angle_error,
          AngleBetween(MatrixRotation(models[j]), reference.rotations[j]));
    }
  }
  RETARGET_CHECK(max_position_error <=
                 kPositionTolerance + kRuntimeEpsilon);
  RETARGET_CHECK(max_angle_error <= kAngleTolerance + kRuntimeEpsilon);
}

// Case 2: root translation is retargeted with root_scale, including the
// explicit t = 0 and t = duration values.
RETARGET_CASE(root_motion_uses_root_scale) {
  Fixtures fx;
  const float root_scale = 2.5f;
  const RawAnimation output =
      RunRetarget(fx.input, *fx.source_runtime, *fx.target_runtime,
                  StandardMapping(), root_scale, fx.probes);

  for (size_t p = 0; p < fx.probes.size(); ++p) {
    const std::vector<math::Transform> source_locals =
        SampleLocals(fx.input, fx.probes[p], 7);
    const ModelPose source_pose =
        ComputeModel(source_locals, fx.source_flat);
    const math::Float3 expected_root =
        fx.target_rest.translations[0] +
        (source_pose.translations[0] - fx.source_rest.translations[0]) *
            root_scale;
    const std::vector<math::Transform> target_locals =
        SampleLocals(output, fx.probes[p], 9);
    const ModelPose target_pose =
        ComputeModel(target_locals, fx.target_flat);
    RETARGET_CHECK(Distance(target_pose.translations[0], expected_root) <=
                   kPositionTolerance);
  }

  // At t = 0 the source root is at rest, so the target root must be exactly
  // at its rest position (up to floating point noise).
  const std::vector<math::Transform> first_locals =
      SampleLocals(output, 0.f, 9);
  RETARGET_CHECK(Distance(first_locals[0].translation,
                          fx.target_flat.joints[0].local.translation) < 1e-5f);

  // At t = duration, the source wave returns to 0: target root back at rest.
  const std::vector<math::Transform> last_locals =
      SampleLocals(output, fx.input.duration, 9);
  RETARGET_CHECK(Distance(last_locals[0].translation,
                          fx.target_flat.joints[0].local.translation) < 1e-5f);
}

// Case 3: an unmapped target root keeps the whole skeleton at rest, whatever
// the source animation does.
RETARGET_CASE(unmapped_root_keeps_target_rest) {
  Fixtures fx;
  std::vector<int16_t> mapping = StandardMapping();
  for (int16_t& mapped : mapping) {
    mapped = -1;  // Nothing is driven.
  }
  const RawAnimation output =
      RunRetarget(fx.input, *fx.source_runtime, *fx.target_runtime, mapping,
                  1.f, fx.probes);

  float rest_position_error = 0.f;
  float rest_angle_error = 0.f;
  for (float time : fx.probes) {
    const std::vector<math::Transform> locals =
        SampleLocals(output, time, 9);
    const ModelPose pose = ComputeModel(locals, fx.target_flat);
    for (int j = 0; j < 9; ++j) {
      // Round-trips through model space leave only floating point noise, far
      // below the retargeting tolerances.
      rest_position_error = math::Max(
          rest_position_error,
          Distance(pose.translations[j], fx.target_rest.translations[j]));
      rest_angle_error = math::Max(
          rest_angle_error,
          AngleBetween(pose.rotations[j], fx.target_rest.rotations[j]));
    }
  }
  RETARGET_CHECK(rest_position_error < 1e-4f);
  RETARGET_CHECK(rest_angle_error < 1e-4f);
  // A constant posture only needs the two end keys.
  RETARGET_CHECK(OutputKeyTimes(output).size() == 2);
}

// Case 4: unmapped branches and extra intermediate bones keep propagating
// their rest local pose while driven ancestors move.
RETARGET_CASE(unmapped_branch_and_helper_bones_propagate) {
  Fixtures fx;
  const RawAnimation output =
      RunRetarget(fx.input, *fx.source_runtime, *fx.target_runtime,
                  StandardMapping(), 1.f, fx.probes);

  // t_arm_mid (3), t_knee (6) and t_branch (8) are unmapped: their local
  // transform must remain the rest local transform at every probe.
  const int unmapped[3] = {3, 6, 8};
  for (float time : fx.probes) {
    const std::vector<math::Transform> locals =
        SampleLocals(output, time, 9);
    for (int joint : unmapped) {
      RETARGET_CHECK(Distance(locals[joint].translation,
                              fx.target_flat.joints[joint].local
                                  .translation) < 1e-4f);
      RETARGET_CHECK(AngleBetween(locals[joint].rotation,
                                  fx.target_flat.joints[joint].local
                                      .rotation) < 1e-4f);
    }
  }

  // The unmapped branch still moves in model space because its parent moves.
  const std::vector<math::Transform> mid_locals =
      SampleLocals(output, 1.f, 9);
  const ModelPose mid_pose = ComputeModel(mid_locals, fx.target_flat);
  RETARGET_CHECK(Distance(mid_pose.translations[8],
                          fx.target_rest.translations[8]) > 0.02f);
}

// Case 5: every retained internal key is necessary: removing any one of them
// violates at least one probe tolerance for at least one joint.
RETARGET_CASE(internal_keys_are_irreducible) {
  Fixtures fx;
  // A denser probe grid than the other cases: the reference motion is
  // non-trivial, so only some of these probes survive the selection.
  const std::vector<float> dense_probes = MakeProbes(fx.input.duration, 25);
  const RawAnimation output =
      RunRetarget(fx.input, *fx.source_runtime, *fx.target_runtime,
                  StandardMapping(), 1.f, dense_probes);

  const std::vector<float> key_times = OutputKeyTimes(output);
  RETARGET_CHECK(key_times.size() >= 4);
  RETARGET_CHECK(key_times.size() < dense_probes.size());

  std::vector<ModelPose> expected;
  for (float time : dense_probes) {
    const std::vector<math::Transform> source_locals =
        SampleLocals(fx.input, time, 7);
    expected.push_back(ExpectedTarget(
        source_locals, fx.source_flat, fx.target_flat, fx.source_rest,
        fx.target_rest, StandardMapping(), 1.f));
  }

  // The selected set must itself meet the tolerances at every probe.
  CheckRawAtProbes(output, fx.target_flat, dense_probes, expected,
                   kPositionTolerance, kAngleTolerance);

  for (size_t removed = 1; removed + 1 < key_times.size(); ++removed) {
    RawAnimation reduced = output;
    const float removed_time = key_times[removed];
    for (RawAnimation::JointTrack& track : reduced.tracks) {
      track.translations.erase(
          std::remove_if(track.translations.begin(),
                         track.translations.end(),
                         [removed_time](const auto& _key) {
                           return _key.time == removed_time;
                         }),
          track.translations.end());
      track.rotations.erase(
          std::remove_if(track.rotations.begin(), track.rotations.end(),
                         [removed_time](const auto& _key) {
                           return _key.time == removed_time;
                         }),
          track.rotations.end());
      track.scales.erase(
          std::remove_if(track.scales.begin(), track.scales.end(),
                         [removed_time](const auto& _key) {
                           return _key.time == removed_time;
                         }),
          track.scales.end());
    }
    RETARGET_CHECK(reduced.Validate());

    bool violated = false;
    for (size_t p = 0; p < dense_probes.size() && !violated; ++p) {
      const std::vector<math::Transform> locals =
          SampleLocals(reduced, dense_probes[p], 9);
      const ModelPose sampled = ComputeModel(locals, fx.target_flat);
      for (int j = 0; j < 9 && !violated; ++j) {
        if (Distance(sampled.translations[j],
                     expected[p].translations[j]) > kPositionTolerance ||
            AngleBetween(sampled.rotations[j],
                         expected[p].rotations[j]) > kAngleTolerance) {
          violated = true;
        }
      }
    }
    RETARGET_CHECK(violated);
  }
}

// Case 6: deterministic output and quaternion sign invariance.
RETARGET_CASE(deterministic_and_quaternion_sign_invariant) {
  Fixtures fx;
  const RawAnimation first =
      RunRetarget(fx.input, *fx.source_runtime, *fx.target_runtime,
                  StandardMapping(), 1.f, fx.probes);
  const RawAnimation second =
      RunRetarget(fx.input, *fx.source_runtime, *fx.target_runtime,
                  StandardMapping(), 1.f, fx.probes);

  RETARGET_CHECK(first.duration == second.duration);
  RETARGET_CHECK(first.num_tracks() == second.num_tracks());
  RETARGET_CHECK(OutputKeyTimes(first) == OutputKeyTimes(second));
  for (int j = 0; j < first.num_tracks(); ++j) {
    RETARGET_CHECK(first.tracks[j].rotations.size() ==
                   second.tracks[j].rotations.size());
    for (size_t k = 0; k < first.tracks[j].rotations.size(); ++k) {
      const math::Quaternion& a = first.tracks[j].rotations[k].value;
      const math::Quaternion& b = second.tracks[j].rotations[k].value;
      // Same rotation, and actually the same key sequence.
      RETARGET_CHECK(std::abs(math::Dot(a, b)) > 1.f - 1e-6f);
      RETARGET_CHECK(a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w);
    }
  }

  // Negating source quaternions represents the same motion: with the
  // retargeter's canonical quaternion sign convention, the retargeted key
  // times and model postures must be identical up to floating point noise.
  RawAnimation negated = fx.input;
  for (RawAnimation::JointTrack& track : negated.tracks) {
    for (auto& key : track.rotations) {
      key.value = -key.value;
    }
  }
  const RawAnimation output_negated =
      RunRetarget(negated, *fx.source_runtime, *fx.target_runtime,
                  StandardMapping(), 1.f, fx.probes);
  RETARGET_CHECK(OutputKeyTimes(output_negated) == OutputKeyTimes(first));
  float sign_position_error = 0.f;
  float sign_angle_error = 0.f;
  for (float time : fx.probes) {
    const ModelPose pose_a =
        ComputeModel(SampleLocals(first, time, 9), fx.target_flat);
    const ModelPose pose_b =
        ComputeModel(SampleLocals(output_negated, time, 9), fx.target_flat);
    for (int j = 0; j < 9; ++j) {
      sign_position_error = math::Max(
          sign_position_error,
          Distance(pose_a.translations[j], pose_b.translations[j]));
      sign_angle_error = math::Max(
          sign_angle_error,
          AngleBetween(pose_a.rotations[j], pose_b.rotations[j]));
    }
  }
  RETARGET_CHECK(sign_position_error < 1e-5f);
  RETARGET_CHECK(sign_angle_error < 1e-5f);
}

// Case 7: invalid arguments fail and leave the output untouched. The source
// skeleton and animation are not modified either.
RETARGET_CASE(invalid_arguments_fail_without_side_effects) {
  Fixtures fx;
  RawAnimation output;
  output.duration = 1234.f;  // Sentinel proving the output is not touched.
  const RawAnimation input_snapshot = fx.input;
  const RawSkeleton source_snapshot = fx.source_skeleton;

  auto expect_failure = [&](const std::vector<int16_t>& mapping,
                            float root_scale,
                            const std::vector<float>& probes,
                            float position_tolerance,
                            float angle_tolerance) {
    const bool ok = RawAnimationRetargeter()(
        fx.input, *fx.source_runtime, *fx.target_runtime,
        ozz::make_span(mapping), root_scale, ozz::make_span(probes),
        position_tolerance, angle_tolerance, &output);
    RETARGET_CHECK(!ok);
    RETARGET_CHECK(output.duration == 1234.f);
  };

  // Wrong mapping size.
  {
    std::vector<int16_t> mapping = StandardMapping();
    mapping.pop_back();
    expect_failure(mapping, 1.f, fx.probes, kPositionTolerance,
                   kAngleTolerance);
  }
  // Out of range source index.
  {
    std::vector<int16_t> mapping = StandardMapping();
    mapping[4] = 99;
    expect_failure(mapping, 1.f, fx.probes, kPositionTolerance,
                   kAngleTolerance);
  }
  // Index below -1.
  {
    std::vector<int16_t> mapping = StandardMapping();
    mapping[4] = -2;
    expect_failure(mapping, 1.f, fx.probes, kPositionTolerance,
                   kAngleTolerance);
  }
  // Target root mapped to a non-root source joint.
  {
    std::vector<int16_t> mapping = StandardMapping();
    mapping[0] = 2;
    expect_failure(mapping, 1.f, fx.probes, kPositionTolerance,
                   kAngleTolerance);
  }
  // Non positive root scale.
  expect_failure(StandardMapping(), 0.f, fx.probes, kPositionTolerance,
                 kAngleTolerance);
  // Non positive tolerances.
  expect_failure(StandardMapping(), 1.f, fx.probes, 0.f, kAngleTolerance);
  expect_failure(StandardMapping(), 1.f, fx.probes, kPositionTolerance, -1.f);
  // Probe times not covering 0.
  {
    std::vector<float> probes = fx.probes;
    probes.front() = 0.1f;
    expect_failure(StandardMapping(), 1.f, probes, kPositionTolerance,
                   kAngleTolerance);
  }
  // Probe times not strictly increasing.
  {
    std::vector<float> probes = fx.probes;
    probes[3] = probes[2];
    expect_failure(StandardMapping(), 1.f, probes, kPositionTolerance,
                   kAngleTolerance);
  }
  // Probe times not ending at the duration.
  {
    std::vector<float> probes = fx.probes;
    probes.back() = fx.input.duration - 0.2f;
    expect_failure(StandardMapping(), 1.f, probes, kPositionTolerance,
                   kAngleTolerance);
  }
  // Single probe time.
  {
    std::vector<float> probes = {0.f};
    expect_failure(StandardMapping(), 1.f, probes, kPositionTolerance,
                   kAngleTolerance);
  }
  // Null output and aliased output.
  RETARGET_CHECK(!RawAnimationRetargeter()(
      fx.input, *fx.source_runtime, *fx.target_runtime,
      ozz::make_span(StandardMapping()), 1.f, ozz::make_span(fx.probes),
      kPositionTolerance, kAngleTolerance, nullptr));
  RETARGET_CHECK(!RawAnimationRetargeter()(
      fx.input, *fx.source_runtime, *fx.target_runtime,
      ozz::make_span(StandardMapping()), 1.f, ozz::make_span(fx.probes),
      kPositionTolerance, kAngleTolerance, &fx.input));

  // Source track count mismatch.
  {
    RawAnimation wrong_tracks = fx.input;
    wrong_tracks.tracks.resize(6);
    RETARGET_CHECK(!RawAnimationRetargeter()(
        wrong_tracks, *fx.source_runtime, *fx.target_runtime,
        ozz::make_span(StandardMapping()), 1.f, ozz::make_span(fx.probes),
        kPositionTolerance, kAngleTolerance, &output));
    RETARGET_CHECK(output.duration == 1234.f);
  }

  // Non-unit scale in a skeleton rest pose is rejected.
  {
    RawSkeleton scaled_source = fx.source_skeleton;
    scaled_source.roots[0].transform.scale = math::Float3(2.f, 1.f, 1.f);
    const ozz::unique_ptr<Skeleton> scaled_runtime =
        SkeletonBuilder()(scaled_source);
    RETARGET_CHECK(scaled_runtime);
    RETARGET_CHECK(!RawAnimationRetargeter()(
        fx.input, *scaled_runtime, *fx.target_runtime,
        ozz::make_span(StandardMapping()), 1.f, ozz::make_span(fx.probes),
        kPositionTolerance, kAngleTolerance, &output));
    RETARGET_CHECK(output.duration == 1234.f);
  }
  // Non-unit scale in an animation channel is rejected.
  {
    RawAnimation scaled_anim = fx.input;
    scaled_anim.tracks[0].scales[0].value = math::Float3(1.f, 2.f, 1.f);
    RETARGET_CHECK(!RawAnimationRetargeter()(
        scaled_anim, *fx.source_runtime, *fx.target_runtime,
        ozz::make_span(StandardMapping()), 1.f, ozz::make_span(fx.probes),
        kPositionTolerance, kAngleTolerance, &output));
    RETARGET_CHECK(output.duration == 1234.f);
  }
  // Multi-root skeleton is rejected.
  {
    RawSkeleton multi_root = fx.target_skeleton;
    multi_root.roots.resize(2);
    multi_root.roots[1].name = "extra_root";
    const ozz::unique_ptr<Skeleton> multi_runtime =
        SkeletonBuilder()(multi_root);
    RETARGET_CHECK(multi_runtime);
    RETARGET_CHECK(!RawAnimationRetargeter()(
        fx.input, *fx.source_runtime, *multi_runtime,
        ozz::make_span(StandardMapping()), 1.f, ozz::make_span(fx.probes),
        kPositionTolerance, kAngleTolerance, &output));
    RETARGET_CHECK(output.duration == 1234.f);
  }

  // Inputs were not modified.
  RETARGET_CHECK(fx.input.duration == input_snapshot.duration);
  RETARGET_CHECK(fx.input.num_tracks() == input_snapshot.num_tracks());
  for (int j = 0; j < fx.input.num_tracks(); ++j) {
    RETARGET_CHECK(fx.input.tracks[j].rotations.size() ==
                   input_snapshot.tracks[j].rotations.size());
  }
  RETARGET_CHECK(fx.source_skeleton.num_joints() ==
                 source_snapshot.num_joints());
}
