// SkeletonReducer target tests.
// Registered through task_support/check.h, see task_support/README.md.

#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_animation_utils.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/offline/skeleton_reducer.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/transform.h"
#include "ozz/base/memory/unique_ptr.h"
#include "ozz/geometry/runtime/skinning_job.h"

namespace {

using ozz::math::Float3;
using ozz::math::Float4x4;
using ozz::math::Quaternion;
using ozz::math::Transform;
using ozz::animation::offline::RawAnimation;
using ozz::animation::offline::RawSkeleton;
using ozz::animation::offline::SkeletonReducer;

const float kPi = 3.14159265358979323846f;

Transform Trs(const Float3& _t, const Quaternion& _r, const Float3& _s) {
  Transform tr;
  tr.translation = _t;
  tr.rotation = _r;
  tr.scale = _s;
  return tr;
}

Transform T(float _x, float _y, float _z) {
  return Trs(Float3(_x, _y, _z), Quaternion::identity(), Float3::one());
}

Quaternion Rz(float _a) {
  return Quaternion::FromAxisAngle(Float3::z_axis(), _a);
}

Quaternion Ry(float _a) {
  return Quaternion::FromAxisAngle(Float3::y_axis(), _a);
}

// Maximum element-wise absolute difference between two matrices.
float MatDiff(const Float4x4& _a, const Float4x4& _b) {
  float diff = 0.f;
  for (int c = 0; c < 4; ++c) {
    float fa[4], fb[4];
    ozz::math::StorePtrU(_a.cols[c], fa);
    ozz::math::StorePtrU(_b.cols[c], fb);
    for (int r = 0; r < 4; ++r) {
      diff = std::max(diff, std::fabs(fa[r] - fb[r]));
    }
  }
  return diff;
}

// Determinant of the upper 3x3 part of _m.
float Det3(const Float4x4& _m) {
  float c0[4], c1[4], c2[4];
  ozz::math::StorePtrU(_m.cols[0], c0);
  ozz::math::StorePtrU(_m.cols[1], c1);
  ozz::math::StorePtrU(_m.cols[2], c2);
  return c0[0] * (c1[1] * c2[2] - c1[2] * c2[1]) -
         c1[0] * (c0[1] * c2[2] - c0[2] * c2[1]) +
         c2[0] * (c0[1] * c1[2] - c0[2] * c1[1]);
}

// Collects rest pose transforms in depth-first order (SkeletonBuilder's joint
// numbering).
void CollectRest(const RawSkeleton::Joint::Children& _children,
                 ozz::vector<Transform>* _out) {
  for (size_t i = 0; i < _children.size(); ++i) {
    _out->push_back(_children[i].transform);
    CollectRest(_children[i].children, _out);
  }
}

// Negates every rest pose rotation of the hierarchy. q and -q are the same
// rotation.
void NegateRestRotations(RawSkeleton::Joint::Children& _children) {
  for (size_t i = 0; i < _children.size(); ++i) {
    _children[i].transform.rotation = -_children[i].transform.rotation;
    NegateRestRotations(_children[i].children);
  }
}

// Computes model-space matrices by walking the hierarchy depth-first.
// Independent reference implementation, not shared with the library.
int ComputeModels(const RawSkeleton::Joint::Children& _children,
                  const Float4x4& _parent_model, const Transform* _locals,
                  int _next, Float4x4* _models) {
  for (size_t i = 0; i < _children.size(); ++i) {
    const int j = _next++;
    _models[j] = _parent_model * Float4x4::FromAffine(_locals[j]);
    _next = ComputeModels(_children[i].children, _models[j], _locals, _next,
                          _models);
  }
  return _next;
}

// Max model-matrix difference of kept joints between an original and a
// reduced setup, for the given local transforms (depth-first order).
float KeptModelDiff(const RawSkeleton& _raw_o, const Transform* _locals_o,
                    const RawSkeleton& _raw_r, const Transform* _locals_r,
                    const ozz::vector<int>& _old_to_new) {
  ozz::vector<Float4x4> mo(_raw_o.num_joints()), mr(_raw_r.num_joints());
  ComputeModels(_raw_o.roots, Float4x4::identity(), _locals_o, 0, mo.data());
  ComputeModels(_raw_r.roots, Float4x4::identity(), _locals_r, 0, mr.data());
  float diff = 0.f;
  for (size_t j = 0; j < _old_to_new.size(); ++j) {
    if (_old_to_new[j] >= 0) {
      diff = std::max(diff, MatDiff(mo[j], mr[_old_to_new[j]]));
    }
  }
  return diff;
}

// Same as KeptModelDiff, sampling both animations at every given time.
float KeptModelDiff(const RawSkeleton& _raw_o, const RawAnimation& _anim_o,
                    const RawSkeleton& _raw_r, const RawAnimation& _anim_r,
                    const ozz::span<const float>& _times,
                    const ozz::vector<int>& _old_to_new) {
  float diff = 0.f;
  ozz::vector<Transform> lo(_raw_o.num_joints()), lr(_raw_r.num_joints());
  for (size_t k = 0; k < _times.size(); ++k) {
    if (!ozz::animation::offline::SampleAnimation(_anim_o, _times[k],
                                                  ozz::make_span(lo)) ||
        !ozz::animation::offline::SampleAnimation(_anim_r, _times[k],
                                                  ozz::make_span(lr))) {
      return 1e30f;
    }
    diff = std::max(diff, KeptModelDiff(_raw_o, lo.data(), _raw_r, lr.data(),
                                        _old_to_new));
  }
  return diff;
}

float KeptRestDiff(const RawSkeleton& _raw_o, const RawSkeleton& _raw_r,
                   const ozz::vector<int>& _old_to_new) {
  ozz::vector<Transform> lo, lr;
  CollectRest(_raw_o.roots, &lo);
  CollectRest(_raw_r.roots, &lr);
  return KeptModelDiff(_raw_o, lo.data(), _raw_r, lr.data(), _old_to_new);
}

// Samples a runtime animation and converts it to model-space matrices.
bool RuntimeModels(const ozz::animation::Skeleton& _skeleton,
                   const ozz::animation::Animation& _animation, float _time,
                   ozz::animation::SamplingJob::Context* _context,
                   ozz::vector<ozz::math::SoaTransform>* _locals,
                   Float4x4* _models) {
  _locals->resize(_skeleton.num_soa_joints());
  ozz::animation::SamplingJob sampling;
  sampling.animation = &_animation;
  sampling.context = _context;
  sampling.ratio = _time / _animation.duration();
  sampling.output = ozz::make_span(*_locals);
  if (!sampling.Run()) {
    return false;
  }
  ozz::animation::LocalToModelJob ltm;
  ltm.skeleton = &_skeleton;
  ltm.input = ozz::make_span(*_locals);
  ltm.output = ozz::span<Float4x4>(_models, _skeleton.num_joints());
  return ltm.Run();
}

// Converts rest poses to model-space matrices through LocalToModelJob.
bool RuntimeRestModels(const ozz::animation::Skeleton& _skeleton,
                       Float4x4* _models) {
  ozz::animation::LocalToModelJob ltm;
  ltm.skeleton = &_skeleton;
  ltm.input = _skeleton.joint_rest_poses();
  ltm.output = ozz::span<Float4x4>(_models, _skeleton.num_joints());
  return ltm.Run();
}

// Runs the SkinningJob on 2-influence vertices.
bool Skin2(const Float4x4* _palette, size_t _palette_size,
           const uint16_t* _indices, const float* _weights,
           const float* _positions, int _vertex_count, float* _out) {
  ozz::geometry::SkinningJob job;
  job.vertex_count = _vertex_count;
  job.influences_count = 2;
  job.joint_matrices = ozz::span<const Float4x4>(_palette, _palette_size);
  job.joint_indices =
      ozz::span<const uint16_t>(_indices, _vertex_count * 2);
  job.joint_indices_stride = sizeof(uint16_t) * 2;
  job.joint_weights = ozz::span<const float>(_weights, _vertex_count);
  job.joint_weights_stride = sizeof(float);
  job.in_positions = ozz::span<const float>(_positions, _vertex_count * 3);
  job.in_positions_stride = sizeof(float) * 3;
  job.out_positions = ozz::span<float>(_out, _vertex_count * 3);
  job.out_positions_stride = sizeof(float) * 3;
  return job.Run();
}

// root(0) -> helper_a(1) -> helper_b(2) -> tip(3, kept).
RawSkeleton ChainSkeleton() {
  RawSkeleton raw;
  raw.roots.resize(1);
  RawSkeleton::Joint& root = raw.roots[0];
  root.name = "root";
  root.transform = T(0.f, 0.f, 0.f);
  root.children.resize(1);
  RawSkeleton::Joint& a = root.children[0];
  a.name = "helper_a";
  a.transform = T(0.f, 1.f, 0.f);
  a.children.resize(1);
  RawSkeleton::Joint& b = a.children[0];
  b.name = "helper_b";
  b.transform = Trs(Float3(0.f, .5f, 0.f), Rz(.3f), Float3::one());
  b.children.resize(1);
  RawSkeleton::Joint& tip = b.children[0];
  tip.name = "tip";
  tip.transform = T(2.f, 0.f, 0.f);
  return raw;
}

const float kChainTimes[] = {0.f, .5f, 1.f};

RawAnimation ChainClip() {
  RawAnimation anim;
  anim.name = "chain";
  anim.duration = 1.f;
  anim.tracks.resize(4);
  anim.tracks[0].translations.push_back({0.f, Float3(0.f, 0.f, 0.f)});
  anim.tracks[0].translations.push_back({1.f, Float3(1.f, 0.f, 0.f)});
  anim.tracks[1].rotations.push_back({0.f, Quaternion::identity()});
  anim.tracks[1].rotations.push_back({1.f, Rz(.5f)});
  anim.tracks[2].translations.push_back({0.f, Float3(0.f, .5f, 0.f)});
  anim.tracks[2].translations.push_back({1.f, Float3(0.f, 1.f, 0.f)});
  anim.tracks[2].scales.push_back({0.f, Float3(1.f, 1.f, 1.f)});
  anim.tracks[2].scales.push_back({1.f, Float3(2.f, 2.f, 2.f)});
  anim.tracks[3].translations.push_back({0.f, Float3(2.f, 0.f, 0.f)});
  anim.tracks[3].translations.push_back({1.f, Float3(3.f, 0.f, 0.f)});
  return anim;
}

// root(0) -> mid(1) -> leaf(2), identity transforms except a leaf offset.
RawSkeleton TinySkeleton() {
  RawSkeleton raw;
  raw.roots.resize(1);
  raw.roots[0].name = "root";
  raw.roots[0].children.resize(1);
  raw.roots[0].children[0].name = "mid";
  raw.roots[0].children[0].children.resize(1);
  raw.roots[0].children[0].children[0].name = "leaf";
  raw.roots[0].children[0].children[0].transform = T(1.f, 0.f, 0.f);
  return raw;
}

const float kTinyTimes[] = {0.f, 1.f};

RawAnimation TinyClip() {
  RawAnimation anim;
  anim.name = "tiny";
  anim.duration = 1.f;
  anim.tracks.resize(3);
  anim.tracks[2].translations.push_back({0.f, Float3(1.f, 0.f, 0.f)});
  anim.tracks[2].translations.push_back({1.f, Float3(2.f, 0.f, 0.f)});
  return anim;
}

double SkeletonChecksum(const RawSkeleton& _raw) {
  ozz::vector<Transform> rests;
  CollectRest(_raw.roots, &rests);
  double sum = _raw.num_joints();
  for (size_t i = 0; i < rests.size(); ++i) {
    const Transform& t = rests[i];
    sum += t.translation.x + t.translation.y + t.translation.z + t.rotation.x +
           t.rotation.y + t.rotation.z + t.rotation.w + t.scale.x + t.scale.y +
           t.scale.z;
  }
  return sum;
}

double NameChecksum(const RawSkeleton::Joint::Children& _children) {
  double sum = 0.;
  for (size_t i = 0; i < _children.size(); ++i) {
    sum += static_cast<double>(_children[i].name.size());
    sum += NameChecksum(_children[i].children);
  }
  return sum;
}

double AnimationChecksum(const RawAnimation& _anim) {
  double sum = _anim.duration + _anim.num_tracks();
  for (size_t t = 0; t < _anim.tracks.size(); ++t) {
    const RawAnimation::JointTrack& track = _anim.tracks[t];
    for (size_t k = 0; k < track.translations.size(); ++k) {
      sum += track.translations[k].time + track.translations[k].value.x +
             track.translations[k].value.y + track.translations[k].value.z;
    }
    for (size_t k = 0; k < track.rotations.size(); ++k) {
      sum += track.rotations[k].time + track.rotations[k].value.x +
             track.rotations[k].value.y + track.rotations[k].value.z +
             track.rotations[k].value.w;
    }
    for (size_t k = 0; k < track.scales.size(); ++k) {
      sum += track.scales[k].time + track.scales[k].value.x +
             track.scales[k].value.y + track.scales[k].value.z;
    }
  }
  return sum;
}

//----------------------------------------------------------------------------//
// reduction group
//----------------------------------------------------------------------------//

// Helper joints that compose to plain TRS are removed; descendants are
// re-attached to the closest kept ancestor.
int CaseChainRemovesHelpers() {
  RawSkeleton raw = ChainSkeleton();
  RawAnimation anim = ChainClip();
  const int keep[] = {3};
  const SkeletonReducer::Clip clip = {&anim, ozz::make_span(kChainTimes)};
  SkeletonReducer reducer;
  SkeletonReducer::Result result;
  SkeletonReducer::Error error;
  TASK_CHECK(reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                     ozz::make_span(keep), &result, &error));

  // Both helpers were removed, root and tip kept.
  TASK_CHECK(result.old_to_new.size() == 4u);
  TASK_CHECK(result.old_to_new[0] == 0 && result.old_to_new[1] == -1 &&
             result.old_to_new[2] == -1 && result.old_to_new[3] == 1);
  TASK_CHECK(result.new_to_old.size() == 2u && result.new_to_old[0] == 0 &&
             result.new_to_old[1] == 3);

  // Reduced hierarchy: tip re-attached to root.
  TASK_CHECK(result.skeleton.num_joints() == 2);
  TASK_CHECK(result.skeleton.roots.size() == 1u);
  TASK_CHECK(result.skeleton.roots[0].name == "root");
  TASK_CHECK(result.skeleton.roots[0].children.size() == 1u);
  TASK_CHECK(result.skeleton.roots[0].children[0].name == "tip");
  TASK_CHECK(result.skeleton.roots[0].children[0].children.empty());
  // The tip rest transform absorbed the skipped transforms: helper_b's
  // rotation is the only rest rotation of the chain.
  const Quaternion& rest_q =
      result.skeleton.roots[0].children[0].transform.rotation;
  TASK_CHECK(std::fabs(std::fabs(ozz::math::Dot(rest_q, Rz(.3f))) - 1.f) <
             1e-3f);

  // Reduced animation: full TRS keys at every scheduled time, per kept joint.
  TASK_CHECK(result.animations.size() == 1u);
  const RawAnimation& out = result.animations[0];
  TASK_CHECK(out.name == "chain");
  TASK_CHECK(out.duration == 1.f);
  TASK_CHECK(out.num_tracks() == 2);
  TASK_CHECK(out.Validate());
  for (int t = 0; t < 2; ++t) {
    TASK_CHECK(out.tracks[t].translations.size() == 3u);
    TASK_CHECK(out.tracks[t].rotations.size() == 3u);
    TASK_CHECK(out.tracks[t].scales.size() == 3u);
    for (size_t k = 0; k < 3; ++k) {
      TASK_CHECK(out.tracks[t].translations[k].time == kChainTimes[k]);
      TASK_CHECK(out.tracks[t].rotations[k].time == kChainTimes[k]);
      TASK_CHECK(out.tracks[t].scales[k].time == kChainTimes[k]);
    }
    // Successive rotation keys are in the same hemisphere.
    for (size_t k = 1; k < 3; ++k) {
      TASK_CHECK(ozz::math::Dot(out.tracks[t].rotations[k - 1].value,
                                out.tracks[t].rotations[k].value) >= 0.f);
    }
  }

  // Rest pose and animated model matrices of kept joints are preserved.
  TASK_CHECK(KeptRestDiff(raw, result.skeleton, result.old_to_new) < 1e-3f);
  TASK_CHECK(KeptModelDiff(raw, anim, result.skeleton, out,
                           ozz::make_span(kChainTimes),
                           result.old_to_new) < 1e-3f);
  return 0;
}

// Negative scales are supported: a reflection composing to an exact TRS still
// allows the removal.
int CaseNegativeScaleComposes() {
  RawSkeleton raw;
  raw.roots.resize(1);
  raw.roots[0].name = "root";
  raw.roots[0].children.resize(1);
  RawSkeleton::Joint& helper = raw.roots[0].children[0];
  helper.name = "helper";
  helper.transform = Trs(Float3(0.f, 1.f, 0.f), Quaternion::identity(),
                         Float3(-2.f, 1.f, 1.f));
  helper.children.resize(1);
  RawSkeleton::Joint& tip = helper.children[0];
  tip.name = "tip";
  tip.transform = T(1.f, 0.f, 0.f);

  RawAnimation anim;
  anim.duration = 1.f;
  anim.tracks.resize(3);
  anim.tracks[1].translations.push_back({0.f, Float3(0.f, 1.f, 0.f)});
  anim.tracks[1].translations.push_back({1.f, Float3(0.f, 2.f, 0.f)});
  anim.tracks[1].scales.push_back({0.f, Float3(-2.f, 1.f, 1.f)});
  anim.tracks[1].scales.push_back({1.f, Float3(-3.f, 2.f, 1.f)});
  anim.tracks[2].translations.push_back({0.f, Float3(1.f, 0.f, 0.f)});
  anim.tracks[2].translations.push_back({1.f, Float3(2.f, 0.f, 0.f)});

  const int keep[] = {2};
  const SkeletonReducer::Clip clip = {&anim, ozz::make_span(kChainTimes)};
  SkeletonReducer reducer;
  SkeletonReducer::Result result;
  SkeletonReducer::Error error;
  TASK_CHECK(reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                     ozz::make_span(keep), &result, &error));

  // The helper was removed, the reflection was absorbed by the tip.
  TASK_CHECK(result.old_to_new[0] == 0 && result.old_to_new[1] == -1 &&
             result.old_to_new[2] == 1);
  TASK_CHECK(KeptRestDiff(raw, result.skeleton, result.old_to_new) < 1e-3f);
  TASK_CHECK(KeptModelDiff(raw, anim, result.skeleton, result.animations[0],
                           ozz::make_span(kChainTimes),
                           result.old_to_new) < 1e-3f);

  // The reflection is preserved by the reduced rest pose (negative
  // determinant), it wasn't dropped.
  ozz::vector<Transform> rests;
  CollectRest(result.skeleton.roots, &rests);
  TASK_CHECK(rests.size() == 2u);
  ozz::vector<Float4x4> models(2);
  ComputeModels(result.skeleton.roots, Float4x4::identity(), rests.data(), 0,
                models.data());
  TASK_CHECK(Det3(models[1]) < 0.f);
  return 0;
}

// Multiple roots are always kept; duplicate names don't disturb the
// index-based mapping.
int CaseMultiRootDuplicateNames() {
  RawSkeleton raw;
  raw.roots.resize(2);
  // First chain: "joint"(0) -> "joint"(1, removable) -> "joint"(2, kept).
  RawSkeleton::Joint& root_a = raw.roots[0];
  root_a.name = "joint";
  root_a.children.resize(1);
  RawSkeleton::Joint& child_a = root_a.children[0];
  child_a.name = "joint";
  child_a.transform = T(1.f, 0.f, 0.f);
  child_a.children.resize(1);
  RawSkeleton::Joint& tip_a = child_a.children[0];
  tip_a.name = "joint";
  tip_a.transform = T(1.f, 0.f, 0.f);
  // Second chain: "joint"(3) -> "joint"(4, kept).
  RawSkeleton::Joint& root_b = raw.roots[1];
  root_b.name = "joint";
  root_b.transform = T(5.f, 0.f, 0.f);
  root_b.children.resize(1);
  RawSkeleton::Joint& child_b = root_b.children[0];
  child_b.name = "joint";
  child_b.transform = T(0.f, 1.f, 0.f);

  const int keep[] = {2, 4};
  SkeletonReducer reducer;
  SkeletonReducer::Result result;
  SkeletonReducer::Error error;
  // No animation at all: reduction is driven by the rest pose only.
  TASK_CHECK(reducer(raw, ozz::span<const SkeletonReducer::Clip>(),
                     ozz::make_span(keep), &result, &error));

  // Only the middle joint of the first chain was removed. Roots are never
  // removed.
  TASK_CHECK(result.old_to_new.size() == 5u);
  TASK_CHECK(result.old_to_new[0] == 0 && result.old_to_new[1] == -1 &&
             result.old_to_new[2] == 1 && result.old_to_new[3] == 2 &&
             result.old_to_new[4] == 3);
  TASK_CHECK(result.new_to_old.size() == 4u && result.new_to_old[0] == 0 &&
             result.new_to_old[1] == 2 && result.new_to_old[2] == 3 &&
             result.new_to_old[3] == 4);

  // Both roots survive, in order, with their (identical) names.
  TASK_CHECK(result.skeleton.roots.size() == 2u);
  TASK_CHECK(result.skeleton.num_joints() == 4);
  TASK_CHECK(result.skeleton.roots[0].name == "joint");
  TASK_CHECK(result.skeleton.roots[1].name == "joint");
  TASK_CHECK(result.skeleton.roots[0].children.size() == 1u);
  TASK_CHECK(result.skeleton.roots[1].children.size() == 1u);
  TASK_CHECK(result.skeleton.roots[1].transform.translation.x == 5.f);

  // Model matrices of kept joints match, associated by index (names are all
  // identical and couldn't be used).
  TASK_CHECK(KeptRestDiff(raw, result.skeleton, result.old_to_new) < 1e-3f);
  TASK_CHECK(result.animations.empty());
  return 0;
}

// Negating quaternions (rest pose or animation keys) must not change the set
// of kept joints.
int CaseQuaternionSignInvariant() {
  RawSkeleton raw;
  raw.roots.resize(1);
  raw.roots[0].name = "root";
  raw.roots[0].children.resize(1);
  RawSkeleton::Joint& helper = raw.roots[0].children[0];
  helper.name = "helper";
  helper.transform = Trs(Float3(0.f, 1.f, 0.f), Rz(.3f), Float3::one());
  helper.children.resize(1);
  RawSkeleton::Joint& tip = helper.children[0];
  tip.name = "tip";
  tip.transform = Trs(Float3(1.f, 0.f, 0.f), Rz(.2f), Float3::one());

  RawAnimation anim;
  anim.duration = 1.f;
  anim.tracks.resize(3);
  anim.tracks[1].rotations.push_back({0.f, Rz(.1f)});
  anim.tracks[1].rotations.push_back({1.f, Rz(.9f)});
  anim.tracks[2].rotations.push_back({0.f, Rz(.2f)});
  anim.tracks[2].rotations.push_back({1.f, Rz(-.4f)});
  anim.tracks[2].translations.push_back({0.f, Float3(1.f, 0.f, 0.f)});
  anim.tracks[2].translations.push_back({1.f, Float3(1.5f, 0.f, 0.f)});

  const int keep[] = {2};
  const SkeletonReducer::Clip clip = {&anim, ozz::make_span(kChainTimes)};
  SkeletonReducer reducer;
  SkeletonReducer::Result first;
  SkeletonReducer::Error error;
  TASK_CHECK(reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                     ozz::make_span(keep), &first, &error));
  TASK_CHECK(first.old_to_new[1] == -1);  // Helper was removed.

  // Negates every quaternion of the inputs (rest pose and animation keys).
  // q and -q are the same rotation.
  RawSkeleton neg_raw = raw;
  NegateRestRotations(neg_raw.roots);
  RawAnimation neg_anim = anim;
  for (size_t t = 0; t < neg_anim.tracks.size(); ++t) {
    for (size_t k = 0; k < neg_anim.tracks[t].rotations.size(); ++k) {
      neg_anim.tracks[t].rotations[k].value =
          -neg_anim.tracks[t].rotations[k].value;
    }
  }

  const SkeletonReducer::Clip neg_clip = {&neg_anim,
                                          ozz::make_span(kChainTimes)};
  SkeletonReducer::Result second;
  TASK_CHECK(reducer(neg_raw, ozz::span<const SkeletonReducer::Clip>(
                                  &neg_clip, 1),
                     ozz::make_span(keep), &second, &error));

  // Same kept set, same mappings.
  TASK_CHECK(first.old_to_new == second.old_to_new);
  TASK_CHECK(first.new_to_old == second.new_to_old);

  // Both reduced outputs describe the same motion.
  const ozz::vector<int> identity_map = {0, 1};
  TASK_CHECK(KeptRestDiff(first.skeleton, second.skeleton, identity_map) <
             1e-3f);
  TASK_CHECK(KeptModelDiff(first.skeleton, first.animations[0],
                           second.skeleton, second.animations[0],
                           ozz::make_span(kChainTimes), identity_map) < 1e-3f);
  return 0;
}

//----------------------------------------------------------------------------//
// shear group
//----------------------------------------------------------------------------//

// A composition that shears in only one of the animations keeps the helper,
// even if the rest pose and the other animation are plain TRS.
int CaseShearInOneAnimationKeepsHelper() {
  RawSkeleton raw;
  raw.roots.resize(1);
  raw.roots[0].name = "root";
  raw.roots[0].children.resize(1);
  RawSkeleton::Joint& helper = raw.roots[0].children[0];
  helper.name = "helper";
  helper.children.resize(1);
  RawSkeleton::Joint& tip = helper.children[0];
  tip.name = "tip";
  tip.transform = T(1.f, 0.f, 0.f);

  // Clip A: non-uniform scale on the helper meets a rotation on the tip.
  // The composed local matrix shears and can't be represented as a TRS.
  RawAnimation shear_clip;
  shear_clip.duration = 1.f;
  shear_clip.tracks.resize(3);
  shear_clip.tracks[1].scales.push_back({0.f, Float3(2.f, 1.f, 1.f)});
  shear_clip.tracks[1].scales.push_back({1.f, Float3(2.f, 1.f, 1.f)});
  shear_clip.tracks[2].rotations.push_back({0.f, Quaternion::identity()});
  shear_clip.tracks[2].rotations.push_back({1.f, Rz(kPi / 2.f)});
  shear_clip.tracks[2].translations.push_back({0.f, Float3(1.f, 0.f, 0.f)});
  shear_clip.tracks[2].translations.push_back({1.f, Float3(1.f, 0.f, 0.f)});

  // Clip B: same non-uniform scale, but the tip only translates: the
  // composition stays a TRS.
  RawAnimation plain_clip;
  plain_clip.duration = 1.f;
  plain_clip.tracks.resize(3);
  plain_clip.tracks[1].scales.push_back({0.f, Float3(2.f, 1.f, 1.f)});
  plain_clip.tracks[1].scales.push_back({1.f, Float3(2.f, 1.f, 1.f)});
  plain_clip.tracks[2].translations.push_back({0.f, Float3(1.f, 0.f, 0.f)});
  plain_clip.tracks[2].translations.push_back({1.f, Float3(2.f, 0.f, 0.f)});
  const float plain_times[] = {0.f, 1.f};

  const int keep[] = {2};
  SkeletonReducer reducer;
  SkeletonReducer::Error error;

  // With both clips, the helper must be kept: the shear in clip A can't be
  // approximated away.
  {
    const SkeletonReducer::Clip clips[] = {
        {&shear_clip, ozz::make_span(kChainTimes)},
        {&plain_clip, ozz::make_span(plain_times)}};
    SkeletonReducer::Result result;
    TASK_CHECK(reducer(raw, ozz::make_span(clips), ozz::make_span(keep),
                       &result, &error));
    TASK_CHECK(result.old_to_new.size() == 3u);
    TASK_CHECK(result.old_to_new[0] == 0 && result.old_to_new[1] == 1 &&
               result.old_to_new[2] == 2);
    TASK_CHECK(result.skeleton.num_joints() == 3);
    TASK_CHECK(result.animations.size() == 2u);
    // The kept motion is untouched.
    TASK_CHECK(KeptModelDiff(raw, shear_clip, result.skeleton,
                             result.animations[0], ozz::make_span(kChainTimes),
                             result.old_to_new) < 1e-3f);
    TASK_CHECK(KeptModelDiff(raw, plain_clip, result.skeleton,
                             result.animations[1], ozz::make_span(plain_times),
                             result.old_to_new) < 1e-3f);
  }

  // With only the plain clip, the same helper is removed: the decision
  // depends on every animation, not on the rest pose alone.
  {
    const SkeletonReducer::Clip clips[] = {
        {&plain_clip, ozz::make_span(plain_times)}};
    SkeletonReducer::Result result;
    TASK_CHECK(reducer(raw, ozz::make_span(clips), ozz::make_span(keep),
                       &result, &error));
    TASK_CHECK(result.old_to_new[0] == 0 && result.old_to_new[1] == -1 &&
               result.old_to_new[2] == 1);
    TASK_CHECK(KeptModelDiff(raw, plain_clip, result.skeleton,
                             result.animations[0], ozz::make_span(plain_times),
                             result.old_to_new) < 1e-3f);
  }
  return 0;
}

// A rest pose composition that shears keeps the helper, even without any
// animation.
int CaseShearAtRestKeepsHelper() {
  RawSkeleton raw;
  raw.roots.resize(1);
  raw.roots[0].name = "root";
  raw.roots[0].children.resize(1);
  RawSkeleton::Joint& helper = raw.roots[0].children[0];
  helper.name = "helper";
  helper.transform = Trs(Float3(0.f, 1.f, 0.f), Quaternion::identity(),
                         Float3(2.f, 1.f, 1.f));
  helper.children.resize(1);
  RawSkeleton::Joint& tip = helper.children[0];
  tip.name = "tip";
  tip.transform = Trs(Float3(1.f, 0.f, 0.f), Rz(.5f), Float3::one());

  const int keep[] = {2};
  SkeletonReducer reducer;
  SkeletonReducer::Error error;
  SkeletonReducer::Result result;
  TASK_CHECK(reducer(raw, ozz::span<const SkeletonReducer::Clip>(),
                     ozz::make_span(keep), &result, &error));
  // Non-uniform scale meets a rotation: the helper stays.
  TASK_CHECK(result.old_to_new[0] == 0 && result.old_to_new[1] == 1 &&
             result.old_to_new[2] == 2);
  TASK_CHECK(result.skeleton.num_joints() == 3);

  // Control case: without the tip rotation, the composition is a TRS and the
  // helper is removed.
  RawSkeleton plain = raw;
  plain.roots[0].children[0].children[0].transform.rotation =
      Quaternion::identity();
  SkeletonReducer::Result plain_result;
  TASK_CHECK(reducer(plain, ozz::span<const SkeletonReducer::Clip>(),
                     ozz::make_span(keep), &plain_result, &error));
  TASK_CHECK(plain_result.old_to_new[1] == -1);
  TASK_CHECK(KeptRestDiff(plain, plain_result.skeleton,
                          plain_result.old_to_new) < 1e-3f);
  return 0;
}

//----------------------------------------------------------------------------//
// mapping group
//----------------------------------------------------------------------------//

// Bidirectional joint mappings between original and reduced skeletons.
int CaseMappingTables() {
  RawSkeleton raw = ChainSkeleton();
  RawAnimation anim = ChainClip();
  const int keep[] = {3};
  const SkeletonReducer::Clip clip = {&anim, ozz::make_span(kChainTimes)};
  SkeletonReducer reducer;
  SkeletonReducer::Result result;
  SkeletonReducer::Error error;
  TASK_CHECK(reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                     ozz::make_span(keep), &result, &error));

  // Forward table: -1 for removed joints.
  const int expected_forward[] = {0, -1, -1, 1};
  TASK_CHECK(result.old_to_new.size() == 4u);
  for (size_t j = 0; j < 4; ++j) {
    TASK_CHECK(result.old_to_new[j] == expected_forward[j]);
  }
  // Reverse table: source joint of every reduced joint.
  const int expected_reverse[] = {0, 3};
  TASK_CHECK(result.new_to_old.size() == 2u);
  for (size_t j = 0; j < 2; ++j) {
    TASK_CHECK(result.new_to_old[j] == expected_reverse[j]);
  }
  // Round trip: new_to_old[old_to_new[j]] == j for kept joints.
  for (size_t j = 0; j < result.old_to_new.size(); ++j) {
    if (result.old_to_new[j] >= 0) {
      TASK_CHECK(result.new_to_old[result.old_to_new[j]] ==
                 static_cast<int>(j));
    }
  }

  // The reduced skeleton numbering follows SkeletonBuilder's depth-first
  // order: joint 0 is "root", joint 1 is "tip".
  ozz::animation::offline::SkeletonBuilder builder;
  ozz::unique_ptr<ozz::animation::Skeleton> skeleton =
      builder(result.skeleton);
  TASK_CHECK(skeleton.get() != nullptr);
  TASK_CHECK(skeleton->num_joints() == 2);
  TASK_CHECK(std::strcmp(skeleton->joint_names()[0], "root") == 0);
  TASK_CHECK(std::strcmp(skeleton->joint_names()[1], "tip") == 0);
  TASK_CHECK(skeleton->joint_parents()[0] == ozz::animation::Skeleton::kNoParent);
  TASK_CHECK(skeleton->joint_parents()[1] == 0);
  return 0;
}

// Slot remapping: slots are palette indices, not skeleton joint indices.
int CaseSlotRemapScrambled() {
  RawSkeleton raw = ChainSkeleton();
  RawAnimation anim = ChainClip();
  const int keep[] = {3};
  const SkeletonReducer::Clip clip = {&anim, ozz::make_span(kChainTimes)};
  SkeletonReducer reducer;
  SkeletonReducer::Result result;
  SkeletonReducer::Error error;
  TASK_CHECK(reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                     ozz::make_span(keep), &result, &error));

  // Palette slots reference original joints in an order that differs from the
  // skeleton order: slot 0 is the tip (joint 3), slot 1 is the root (0).
  const int slot_joints[] = {3, 0};
  ozz::vector<int> remapped;
  TASK_CHECK(SkeletonReducer::RemapSlots(ozz::make_span(slot_joints),
                                         ozz::make_span(result.old_to_new),
                                         &remapped, &error));
  TASK_CHECK(remapped.size() == 2u);
  // Slot 0 keeps addressing the tip, now at reduced index 1.
  TASK_CHECK(remapped[0] == 1);
  // Slot 1 keeps addressing the root, still at reduced index 0.
  TASK_CHECK(remapped[1] == 0);
  // Each slot maps through old_to_new of its own joint, not of its slot
  // index.
  for (size_t s = 0; s < 2; ++s) {
    TASK_CHECK(remapped[s] == result.old_to_new[slot_joints[s]]);
  }
  return 0;
}

//----------------------------------------------------------------------------//
// runtime group
//----------------------------------------------------------------------------//

// Full runtime round trip: SkeletonBuilder, AnimationBuilder, SamplingJob,
// LocalToModelJob and SkinningJob on both original and reduced data.
int CaseRuntimePipeline() {
  // root(0) -> spine_helper(1) -> chest(2, kept) -> arm_helper(3)
  //   -> hand(4, kept)
  RawSkeleton raw;
  raw.roots.resize(1);
  RawSkeleton::Joint& root = raw.roots[0];
  root.name = "root";
  root.children.resize(1);
  RawSkeleton::Joint& spine = root.children[0];
  spine.name = "spine_helper";
  spine.transform = Trs(Float3(0.f, .5f, 0.f), Rz(.2f), Float3::one());
  spine.children.resize(1);
  RawSkeleton::Joint& chest = spine.children[0];
  chest.name = "chest";
  chest.transform = T(0.f, .5f, 0.f);
  chest.children.resize(1);
  RawSkeleton::Joint& arm = chest.children[0];
  arm.name = "arm_helper";
  arm.transform = Trs(Float3(.3f, .2f, 0.f), Ry(.4f), Float3::one());
  arm.children.resize(1);
  RawSkeleton::Joint& hand = arm.children[0];
  hand.name = "hand";
  hand.transform = T(.4f, 0.f, 0.f);

  RawAnimation anim;
  anim.name = "move";
  anim.duration = 2.f;
  anim.tracks.resize(5);
  anim.tracks[0].translations.push_back({0.f, Float3(0.f, 0.f, 0.f)});
  anim.tracks[0].translations.push_back({2.f, Float3(1.f, 0.f, 0.f)});
  anim.tracks[1].rotations.push_back({0.f, Quaternion::identity()});
  anim.tracks[1].rotations.push_back({2.f, Rz(.6f)});
  anim.tracks[1].translations.push_back({0.f, Float3(0.f, .5f, 0.f)});
  anim.tracks[1].translations.push_back({2.f, Float3(0.f, .5f, 0.f)});
  anim.tracks[2].rotations.push_back({0.f, Quaternion::identity()});
  anim.tracks[2].rotations.push_back({2.f, Rz(-.4f)});
  anim.tracks[2].translations.push_back({0.f, Float3(0.f, .5f, 0.f)});
  anim.tracks[2].translations.push_back({2.f, Float3(0.f, .7f, 0.f)});
  anim.tracks[3].rotations.push_back({0.f, Quaternion::identity()});
  anim.tracks[3].rotations.push_back({2.f, Ry(.8f)});
  anim.tracks[3].translations.push_back({0.f, Float3(.3f, .2f, 0.f)});
  anim.tracks[3].translations.push_back({2.f, Float3(.3f, .2f, 0.f)});
  anim.tracks[4].rotations.push_back({0.f, Quaternion::identity()});
  anim.tracks[4].rotations.push_back({2.f, Rz(.5f)});
  anim.tracks[4].translations.push_back({0.f, Float3(.4f, 0.f, 0.f)});
  anim.tracks[4].translations.push_back({2.f, Float3(.6f, 0.f, 0.f)});
  anim.tracks[4].scales.push_back({0.f, Float3(1.f, 1.f, 1.f)});
  anim.tracks[4].scales.push_back({2.f, Float3(1.5f, 1.5f, 1.5f)});

  const float times[] = {0.f, .5f, 1.f, 1.5f, 2.f};
  const int keep[] = {2, 4};
  const SkeletonReducer::Clip clip = {&anim, ozz::make_span(times)};
  SkeletonReducer reducer;
  SkeletonReducer::Result result;
  SkeletonReducer::Error error;
  TASK_CHECK(reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                     ozz::make_span(keep), &result, &error));
  TASK_CHECK(result.old_to_new.size() == 5u);
  TASK_CHECK(result.old_to_new[0] == 0 && result.old_to_new[1] == -1 &&
             result.old_to_new[2] == 1 && result.old_to_new[3] == -1 &&
             result.old_to_new[4] == 2);
  TASK_CHECK(result.animations.size() == 1u);
  TASK_CHECK(result.animations[0].name == "move");
  TASK_CHECK(result.animations[0].duration == 2.f);
  TASK_CHECK(result.animations[0].Validate());

  // Builds runtime skeletons and animations.
  ozz::animation::offline::SkeletonBuilder skeleton_builder;
  ozz::unique_ptr<ozz::animation::Skeleton> sk_original =
      skeleton_builder(raw);
  ozz::unique_ptr<ozz::animation::Skeleton> sk_reduced =
      skeleton_builder(result.skeleton);
  TASK_CHECK(sk_original.get() != nullptr && sk_reduced.get() != nullptr);
  TASK_CHECK(sk_original->num_joints() == 5 && sk_reduced->num_joints() == 3);
  ozz::animation::offline::AnimationBuilder animation_builder;
  ozz::unique_ptr<ozz::animation::Animation> an_original =
      animation_builder(anim);
  ozz::unique_ptr<ozz::animation::Animation> an_reduced =
      animation_builder(result.animations[0]);
  TASK_CHECK(an_original.get() != nullptr && an_reduced.get() != nullptr);
  TASK_CHECK(an_reduced->num_tracks() == 3);

  // Rest pose model matrices through LocalToModelJob.
  ozz::vector<Float4x4> rest_original(sk_original->num_joints());
  ozz::vector<Float4x4> rest_reduced(sk_reduced->num_joints());
  TASK_CHECK(RuntimeRestModels(*sk_original, rest_original.data()));
  TASK_CHECK(RuntimeRestModels(*sk_reduced, rest_reduced.data()));
  for (size_t j = 0; j < result.old_to_new.size(); ++j) {
    if (result.old_to_new[j] >= 0) {
      TASK_CHECK(MatDiff(rest_original[j],
                         rest_reduced[result.old_to_new[j]]) < 1e-3f);
    }
  }

  // Animated model matrices through SamplingJob + LocalToModelJob, compared
  // with the independent offline reference.
  ozz::animation::SamplingJob::Context context_original(
      an_original->num_tracks());
  ozz::animation::SamplingJob::Context context_reduced(an_reduced->num_tracks());
  ozz::vector<ozz::math::SoaTransform> locals_original, locals_reduced;
  ozz::vector<Float4x4> models_original(sk_original->num_joints());
  ozz::vector<Float4x4> models_reduced(sk_reduced->num_joints());
  ozz::vector<Transform> sampled(raw.num_joints());
  ozz::vector<Float4x4> models_reference(raw.num_joints());
  for (size_t k = 0; k < 5; ++k) {
    TASK_CHECK(RuntimeModels(*sk_original, *an_original, times[k],
                             &context_original, &locals_original,
                             models_original.data()));
    TASK_CHECK(RuntimeModels(*sk_reduced, *an_reduced, times[k],
                             &context_reduced, &locals_reduced,
                             models_reduced.data()));
    // Independent reference: offline sampling + manual hierarchy walk.
    TASK_CHECK(ozz::animation::offline::SampleAnimation(
        anim, times[k], ozz::make_span(sampled)));
    ComputeModels(raw.roots, Float4x4::identity(), sampled.data(), 0,
                  models_reference.data());
    for (size_t j = 0; j < result.old_to_new.size(); ++j) {
      // Runtime pipeline of the original skeleton matches the reference.
      TASK_CHECK(MatDiff(models_original[j], models_reference[j]) < 1e-2f);
      if (result.old_to_new[j] >= 0) {
        // Reduced runtime pipeline matches the original one.
        TASK_CHECK(MatDiff(models_original[j],
                           models_reduced[result.old_to_new[j]]) < 1e-2f);
      }
    }
  }

  // Skinning: palette slots reference kept joints in an order that differs
  // from the skeleton order. Inverse-bind matrices, vertex slot indices and
  // weights are left untouched; only the per-slot joint indices are remapped.
  const int slot_joints[] = {4, 0, 2};  // hand, root, chest.
  ozz::vector<int> slot_remap;
  TASK_CHECK(SkeletonReducer::RemapSlots(ozz::make_span(slot_joints),
                                         ozz::make_span(result.old_to_new),
                                         &slot_remap, &error));
  TASK_CHECK(slot_remap.size() == 3u && slot_remap[0] == 2 &&
             slot_remap[1] == 0 && slot_remap[2] == 1);

  Float4x4 inverse_bind[3];
  for (size_t s = 0; s < 3; ++s) {
    inverse_bind[s] = ozz::math::Invert(rest_original[slot_joints[s]]);
  }
  const uint16_t vertex_indices[] = {0, 1, 1, 2, 2, 0};
  const float vertex_weights[] = {.25f, .5f, .75f};
  const float vertex_positions[] = {.1f, .2f, .3f,  -.2f, .5f, .1f,
                                    .3f, -.1f, .2f};
  for (size_t k = 0; k < 5; ++k) {
    TASK_CHECK(RuntimeModels(*sk_original, *an_original, times[k],
                             &context_original, &locals_original,
                             models_original.data()));
    TASK_CHECK(RuntimeModels(*sk_reduced, *an_reduced, times[k],
                             &context_reduced, &locals_reduced,
                             models_reduced.data()));
    Float4x4 palette_original[3], palette_reduced[3];
    for (size_t s = 0; s < 3; ++s) {
      palette_original[s] =
          models_original[slot_joints[s]] * inverse_bind[s];
      palette_reduced[s] = models_reduced[slot_remap[s]] * inverse_bind[s];
    }
    float skinned_original[9], skinned_reduced[9];
    TASK_CHECK(Skin2(palette_original, 3, vertex_indices, vertex_weights,
                     vertex_positions, 3, skinned_original));
    TASK_CHECK(Skin2(palette_reduced, 3, vertex_indices, vertex_weights,
                     vertex_positions, 3, skinned_reduced));
    for (int v = 0; v < 9; ++v) {
      TASK_CHECK(std::fabs(skinned_original[v] - skinned_reduced[v]) < 2e-2f);
    }
  }
  return 0;
}

// Two clips with different durations and sampling schedules are reduced
// together and stay consistent through the runtime pipeline.
int CaseRuntimeTwoClips() {
  // root(0) -> helper(1) -> tip(2, kept).
  RawSkeleton raw;
  raw.roots.resize(1);
  raw.roots[0].name = "root";
  raw.roots[0].children.resize(1);
  RawSkeleton::Joint& helper = raw.roots[0].children[0];
  helper.name = "helper";
  helper.transform = T(0.f, 1.f, 0.f);
  helper.children.resize(1);
  RawSkeleton::Joint& tip = helper.children[0];
  tip.name = "tip";
  tip.transform = T(1.f, 0.f, 0.f);

  RawAnimation clip_a;
  clip_a.name = "a";
  clip_a.duration = 1.f;
  clip_a.tracks.resize(3);
  clip_a.tracks[1].rotations.push_back({0.f, Quaternion::identity()});
  clip_a.tracks[1].rotations.push_back({1.f, Rz(.6f)});
  clip_a.tracks[1].translations.push_back({0.f, Float3(0.f, 1.f, 0.f)});
  clip_a.tracks[1].translations.push_back({1.f, Float3(0.f, 1.5f, 0.f)});
  clip_a.tracks[2].translations.push_back({0.f, Float3(1.f, 0.f, 0.f)});
  clip_a.tracks[2].translations.push_back({1.f, Float3(2.f, 0.f, 0.f)});
  const float times_a[] = {0.f, .5f, 1.f};

  RawAnimation clip_b;
  clip_b.name = "b";
  clip_b.duration = 2.f;
  clip_b.tracks.resize(3);
  clip_b.tracks[1].scales.push_back({0.f, Float3(1.f, 1.f, 1.f)});
  clip_b.tracks[1].scales.push_back({2.f, Float3(3.f, 3.f, 3.f)});
  clip_b.tracks[1].rotations.push_back({0.f, Quaternion::identity()});
  clip_b.tracks[1].rotations.push_back({2.f, Rz(-.5f)});
  clip_b.tracks[2].rotations.push_back({0.f, Quaternion::identity()});
  clip_b.tracks[2].rotations.push_back({2.f, Ry(.9f)});
  clip_b.tracks[2].translations.push_back({0.f, Float3(1.f, 0.f, 0.f)});
  clip_b.tracks[2].translations.push_back({2.f, Float3(1.f, 0.f, 1.f)});
  const float times_b[] = {0.f, 1.f, 2.f};

  const int keep[] = {2};
  const SkeletonReducer::Clip clips[] = {{&clip_a, ozz::make_span(times_a)},
                                         {&clip_b, ozz::make_span(times_b)}};
  SkeletonReducer reducer;
  SkeletonReducer::Result result;
  SkeletonReducer::Error error;
  TASK_CHECK(reducer(raw, ozz::make_span(clips), ozz::make_span(keep), &result,
                     &error));
  TASK_CHECK(result.old_to_new[0] == 0 && result.old_to_new[1] == -1 &&
             result.old_to_new[2] == 1);

  // Each clip keeps its own duration and schedule.
  TASK_CHECK(result.animations.size() == 2u);
  TASK_CHECK(result.animations[0].duration == 1.f);
  TASK_CHECK(result.animations[1].duration == 2.f);
  TASK_CHECK(result.animations[0].num_tracks() == 2);
  TASK_CHECK(result.animations[1].num_tracks() == 2);
  TASK_CHECK(result.animations[0].Validate() &&
             result.animations[1].Validate());
  for (int t = 0; t < 2; ++t) {
    TASK_CHECK(result.animations[1].tracks[t].rotations.size() == 3u);
    TASK_CHECK(result.animations[1].tracks[t].rotations[2].time == 2.f);
  }

  // Runtime pipelines of both clips match the original ones.
  ozz::animation::offline::SkeletonBuilder skeleton_builder;
  ozz::unique_ptr<ozz::animation::Skeleton> sk_original =
      skeleton_builder(raw);
  ozz::unique_ptr<ozz::animation::Skeleton> sk_reduced =
      skeleton_builder(result.skeleton);
  TASK_CHECK(sk_original.get() != nullptr && sk_reduced.get() != nullptr);
  ozz::animation::offline::AnimationBuilder animation_builder;
  const RawAnimation* raws[] = {&clip_a, &clip_b};
  const float* schedules[] = {times_a, times_b};
  const size_t schedule_sizes[] = {3u, 3u};
  for (int i = 0; i < 2; ++i) {
    ozz::unique_ptr<ozz::animation::Animation> an_original =
        animation_builder(*raws[i]);
    ozz::unique_ptr<ozz::animation::Animation> an_reduced =
        animation_builder(result.animations[i]);
    TASK_CHECK(an_original.get() != nullptr && an_reduced.get() != nullptr);
    ozz::animation::SamplingJob::Context context_original(
        an_original->num_tracks());
    ozz::animation::SamplingJob::Context context_reduced(
        an_reduced->num_tracks());
    ozz::vector<ozz::math::SoaTransform> locals_original, locals_reduced;
    ozz::vector<Float4x4> models_original(sk_original->num_joints());
    ozz::vector<Float4x4> models_reduced(sk_reduced->num_joints());
    for (size_t k = 0; k < schedule_sizes[i]; ++k) {
      const float time = schedules[i][k];
      TASK_CHECK(RuntimeModels(*sk_original, *an_original, time,
                               &context_original, &locals_original,
                               models_original.data()));
      TASK_CHECK(RuntimeModels(*sk_reduced, *an_reduced, time,
                               &context_reduced, &locals_reduced,
                               models_reduced.data()));
      for (size_t j = 0; j < result.old_to_new.size(); ++j) {
        if (result.old_to_new[j] >= 0) {
          TASK_CHECK(MatDiff(models_original[j],
                             models_reduced[result.old_to_new[j]]) < 1e-2f);
        }
      }
    }
  }
  return 0;
}

//----------------------------------------------------------------------------//
// invalid group
//----------------------------------------------------------------------------//

// Rest pose transforms with zero scale, non-finite values or non-unit
// rotations are rejected with a located error.
int CaseInvalidRestPose() {
  RawAnimation anim = TinyClip();
  const SkeletonReducer::Clip clip = {&anim, ozz::make_span(kTinyTimes)};
  SkeletonReducer reducer;

  // Zero scale.
  {
    RawSkeleton raw = TinySkeleton();
    raw.roots[0].children[0].transform.scale = Float3(0.f, 1.f, 1.f);
    SkeletonReducer::Result result;
    SkeletonReducer::Error error;
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidRestPose);
    TASK_CHECK(error.joint == 1);
    TASK_CHECK(error.animation == -1);
    // No partial result is left behind.
    TASK_CHECK(result.skeleton.num_joints() == 0);
    TASK_CHECK(result.animations.empty());
    TASK_CHECK(result.old_to_new.empty() && result.new_to_old.empty());
  }

  // Non-finite translation.
  {
    RawSkeleton raw = TinySkeleton();
    raw.roots[0].children[0].transform.translation =
        Float3(std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f);
    SkeletonReducer::Result result;
    SkeletonReducer::Error error;
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidRestPose);
    TASK_CHECK(error.joint == 1);
  }

  // Non-unit rotation.
  {
    RawSkeleton raw = TinySkeleton();
    raw.roots[0].children[0].transform.rotation = Quaternion(0.f, 0.f, 0.f, 2.f);
    SkeletonReducer::Result result;
    SkeletonReducer::Error error;
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidRestPose);
    TASK_CHECK(error.joint == 1);
  }
  return 0;
}

// Invalid tolerances, keep indices, animations and schedules are rejected
// with errors locating the offending clip, joint and time.
int CaseInvalidInputs() {
  RawSkeleton raw = TinySkeleton();
  RawAnimation anim = TinyClip();
  SkeletonReducer::Result result;
  SkeletonReducer::Error error;

  // Tolerance must be finite and strictly positive.
  {
    SkeletonReducer reducer;
    reducer.tolerance = 0.f;
    const SkeletonReducer::Clip clip = {&anim, ozz::make_span(kTinyTimes)};
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidTolerance);
    reducer.tolerance = -1e-3f;
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidTolerance);
    reducer.tolerance = std::numeric_limits<float>::quiet_NaN();
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidTolerance);
  }

  // Must-keep indices out of range.
  {
    SkeletonReducer reducer;
    const SkeletonReducer::Clip clip = {&anim, ozz::make_span(kTinyTimes)};
    const int keep_high[] = {5};
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                        ozz::make_span(keep_high), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidKeepIndex);
    TASK_CHECK(error.joint == 5);
    const int keep_negative[] = {-2};
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                        ozz::make_span(keep_negative), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidKeepIndex);
    TASK_CHECK(error.joint == -2);
  }

  // Animation track count must match the skeleton joint count.
  {
    SkeletonReducer reducer;
    RawAnimation few;
    few.duration = 1.f;
    few.tracks.resize(2);
    const SkeletonReducer::Clip clip = {&few, ozz::make_span(kTinyTimes)};
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kTrackCountMismatch);
    TASK_CHECK(error.animation == 0);
  }

  // Schedules must include 0 and duration, and be strictly increasing.
  {
    SkeletonReducer reducer;
    const float missing_zero[] = {.25f, 1.f};
    const SkeletonReducer::Clip clip = {&anim, ozz::make_span(missing_zero)};
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidSchedule);
    TASK_CHECK(error.animation == 0 && error.time == .25f);

    const float missing_duration[] = {0.f, .5f};
    const SkeletonReducer::Clip clip2 = {&anim,
                                         ozz::make_span(missing_duration)};
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip2, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidSchedule);
    TASK_CHECK(error.time == .5f);

    const float not_increasing[] = {0.f, .5f, .5f, 1.f};
    const SkeletonReducer::Clip clip3 = {&anim,
                                         ozz::make_span(not_increasing)};
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip3, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidSchedule);
    TASK_CHECK(error.time == .5f);

    const float single[] = {0.f};
    const SkeletonReducer::Clip clip4 = {&anim, ozz::make_span(single)};
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip4, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidSchedule);
  }

  // Animation keys out of the duration range locate the joint and time.
  {
    SkeletonReducer reducer;
    RawAnimation beyond = TinyClip();
    beyond.tracks[2].translations.push_back(
        {1.5f, Float3(0.f, 0.f, 0.f)});
    const SkeletonReducer::Clip clip = {&beyond, ozz::make_span(kTinyTimes)};
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidAnimation);
    TASK_CHECK(error.animation == 0 && error.joint == 2 &&
               error.time == 1.5f);
  }

  // Null animation.
  {
    SkeletonReducer reducer;
    const SkeletonReducer::Clip clip = {nullptr, ozz::make_span(kTinyTimes)};
    TASK_CHECK(!reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidAnimation);
  }

  // The error locates the offending clip when several are provided.
  {
    SkeletonReducer reducer;
    RawAnimation good = TinyClip();
    RawAnimation bad = TinyClip();
    bad.tracks[1].translations.push_back({2.f, Float3(0.f, 0.f, 0.f)});
    const SkeletonReducer::Clip clips[] = {
        {&good, ozz::make_span(kTinyTimes)},
        {&bad, ozz::make_span(kTinyTimes)}};
    TASK_CHECK(!reducer(raw, ozz::make_span(clips), ozz::span<const int>(),
                        &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidAnimation);
    TASK_CHECK(error.animation == 1 && error.joint == 1 && error.time == 2.f);
  }

  // Skeletons with too many joints are rejected.
  {
    RawSkeleton huge;
    huge.roots.resize(1);
    RawSkeleton::Joint* joint = &huge.roots[0];
    for (int j = 0; j < 1024; ++j) {  // Skeleton::kMaxJoints + 1 joints.
      joint->children.resize(1);
      joint = &joint->children[0];
    }
    SkeletonReducer reducer;
    const SkeletonReducer::Clip clip = {&anim, ozz::make_span(kTinyTimes)};
    TASK_CHECK(!reducer(huge, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                        ozz::span<const int>(), &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidSkeleton);
  }
  return 0;
}

// Slot remapping failures: removed or out-of-range joints, no partial output.
int CaseSlotRemapErrors() {
  RawSkeleton raw = ChainSkeleton();
  RawAnimation anim = ChainClip();
  const int keep[] = {3};
  const SkeletonReducer::Clip clip = {&anim, ozz::make_span(kChainTimes)};
  SkeletonReducer reducer;
  SkeletonReducer::Result result;
  SkeletonReducer::Error error;
  TASK_CHECK(reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                     ozz::make_span(keep), &result, &error));

  // A slot referencing a removed joint fails and locates the slot.
  {
    const int slot_joints[] = {0, 2, 3};  // Joint 2 was removed.
    ozz::vector<int> remapped;
    TASK_CHECK(!SkeletonReducer::RemapSlots(
        ozz::make_span(slot_joints), ozz::make_span(result.old_to_new),
        &remapped, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kSlotJointRemoved);
    TASK_CHECK(error.slot == 1 && error.joint == 2);
    TASK_CHECK(remapped.empty());
  }

  // A slot referencing an out-of-range joint fails as well.
  {
    const int slot_joints[] = {0, 7};
    ozz::vector<int> remapped;
    TASK_CHECK(!SkeletonReducer::RemapSlots(
        ozz::make_span(slot_joints), ozz::make_span(result.old_to_new),
        &remapped, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kSlotJointOutOfRange);
    TASK_CHECK(error.slot == 1 && error.joint == 7);
    TASK_CHECK(remapped.empty());
  }

  // Valid slots remap fine.
  {
    const int slot_joints[] = {3, 0};
    ozz::vector<int> remapped;
    TASK_CHECK(SkeletonReducer::RemapSlots(ozz::make_span(slot_joints),
                                           ozz::make_span(result.old_to_new),
                                           &remapped, &error));
    TASK_CHECK(remapped.size() == 2u && remapped[0] == 1 &&
               remapped[1] == 0);
  }
  return 0;
}

// Failed and successful reductions never modify their inputs and never leave
// partial results behind.
int CaseInputsNotModified() {
  RawSkeleton raw = ChainSkeleton();
  RawAnimation good = ChainClip();
  RawAnimation other = ChainClip();
  const double skeleton_sum = SkeletonChecksum(raw);
  const double names_sum = NameChecksum(raw.roots);
  const double animation_sum = AnimationChecksum(good);

  // A failing reduction (the second clip's schedule misses the duration).
  {
    const float bad_times[] = {0.f, .5f};
    const SkeletonReducer::Clip clips[] = {
        {&good, ozz::make_span(kChainTimes)}, {&other, ozz::make_span(bad_times)}};
    SkeletonReducer reducer;
    SkeletonReducer::Result result;
    SkeletonReducer::Error error;
    const int keep[] = {3};
    TASK_CHECK(!reducer(raw, ozz::make_span(clips), ozz::make_span(keep),
                        &result, &error));
    TASK_CHECK(error.kind == SkeletonReducer::Error::kInvalidSchedule);
    TASK_CHECK(error.animation == 1);
    TASK_CHECK(result.skeleton.num_joints() == 0);
    TASK_CHECK(result.animations.empty());
    TASK_CHECK(result.old_to_new.empty() && result.new_to_old.empty());
  }
  TASK_CHECK(SkeletonChecksum(raw) == skeleton_sum);
  TASK_CHECK(NameChecksum(raw.roots) == names_sum);
  TASK_CHECK(AnimationChecksum(good) == animation_sum);
  TASK_CHECK(AnimationChecksum(other) == animation_sum);

  // A successful reduction doesn't modify its inputs either.
  {
    const SkeletonReducer::Clip clip = {&good, ozz::make_span(kChainTimes)};
    SkeletonReducer reducer;
    SkeletonReducer::Result result;
    SkeletonReducer::Error error;
    const int keep[] = {3};
    TASK_CHECK(reducer(raw, ozz::span<const SkeletonReducer::Clip>(&clip, 1),
                       ozz::make_span(keep), &result, &error));
    TASK_CHECK(result.skeleton.num_joints() == 2);
  }
  TASK_CHECK(SkeletonChecksum(raw) == skeleton_sum);
  TASK_CHECK(NameChecksum(raw.roots) == names_sum);
  TASK_CHECK(AnimationChecksum(good) == animation_sum);
  return 0;
}

}  // namespace

const task_case task_cases[] = {
    {"reduction", "chain_removes_helpers", &CaseChainRemovesHelpers},
    {"reduction", "negative_scale_composes", &CaseNegativeScaleComposes},
    {"reduction", "multi_root_duplicate_names", &CaseMultiRootDuplicateNames},
    {"reduction", "quaternion_sign_invariant", &CaseQuaternionSignInvariant},
    {"shear", "shear_in_one_animation_keeps_helper",
     &CaseShearInOneAnimationKeepsHelper},
    {"shear", "shear_at_rest_keeps_helper", &CaseShearAtRestKeepsHelper},
    {"mapping", "mapping_tables", &CaseMappingTables},
    {"mapping", "slot_remap_scrambled", &CaseSlotRemapScrambled},
    {"runtime", "runtime_pipeline", &CaseRuntimePipeline},
    {"runtime", "runtime_two_clips", &CaseRuntimeTwoClips},
    {"invalid", "invalid_rest_pose", &CaseInvalidRestPose},
    {"invalid", "invalid_inputs", &CaseInvalidInputs},
    {"invalid", "slot_remap_errors", &CaseSlotRemapErrors},
    {"invalid", "inputs_not_modified", &CaseInputsNotModified},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
