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

#include "ozz/animation/offline/skeleton_reducer.h"

#include <cmath>
#include <utility>

#include "ozz/animation/offline/raw_animation_utils.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/simd_math.h"

namespace ozz {
namespace animation {
namespace offline {

namespace {

// Flattens a raw skeleton hierarchy in depth-first order, matching the joint
// numbering used by SkeletonBuilder.
struct FlatSkeleton {
  // Joints in depth-first traversal order.
  ozz::vector<const RawSkeleton::Joint*> joints;
  // Parent of each joint, in original (depth-first) indices. -1 for roots.
  ozz::vector<int> parents;
};

void Flatten(const RawSkeleton::Joint::Children& _children, int _parent,
             FlatSkeleton* _flat) {
  for (size_t i = 0; i < _children.size(); ++i) {
    const RawSkeleton::Joint& joint = _children[i];
    const int index = static_cast<int>(_flat->joints.size());
    _flat->joints.push_back(&joint);
    _flat->parents.push_back(_parent);
    Flatten(joint.children, index, _flat);
  }
}

bool IsFinite(const math::Float3& _v) {
  return std::isfinite(_v.x) && std::isfinite(_v.y) && std::isfinite(_v.z);
}

bool IsFinite(const math::Quaternion& _q) {
  return std::isfinite(_q.x) && std::isfinite(_q.y) && std::isfinite(_q.z) &&
         std::isfinite(_q.w);
}

// Attempts to decompose _m to a finite, non-zero scale TRS, and validates that
// the recomposed matrix matches _m within _tolerance (element-wise absolute
// difference). Compositions that shear (non-uniform scale combined with
// rotations) can't be represented as a TRS and are rejected, they are not
// approximated. Quaternion sign is irrelevant: _q and -_q recompose to the
// same matrix.
bool DecomposeChecked(const math::Float4x4& _m, float _tolerance,
                      math::Transform* _trs) {
  math::Transform trs;
  if (!math::ToAffine(_m, &trs)) {
    return false;
  }
  // The decomposed TRS must be finite, with a non-zero scale.
  if (!IsFinite(trs.translation) || !IsFinite(trs.rotation) ||
      !IsFinite(trs.scale)) {
    return false;
  }
  if (trs.scale.x == 0.f || trs.scale.y == 0.f || trs.scale.z == 0.f) {
    return false;
  }
  // Compares the recomposed matrix with the composed one. Uses !(a <= b) so
  // that non-finite differences are rejected as well.
  const math::Float4x4 recomposed = math::Float4x4::FromAffine(trs);
  for (int c = 0; c < 4; ++c) {
    float expected[4], actual[4];
    math::StorePtrU(_m.cols[c], expected);
    math::StorePtrU(recomposed.cols[c], actual);
    for (int r = 0; r < 4; ++r) {
      if (!(std::fabs(expected[r] - actual[r]) <= _tolerance)) {
        return false;
      }
    }
  }
  *_trs = trs;
  return true;
}

// Validates key frame times of a single channel: finite, strictly increasing
// and within [0, _duration]. Reports the first offending time.
template <typename _Key>
bool ValidateKeys(const ozz::vector<_Key>& _keys, float _duration,
                  float* _offending_time) {
  float previous = 0.f;
  for (size_t k = 0; k < _keys.size(); ++k) {
    const float time = _keys[k].time;
    if (!std::isfinite(time) || time < 0.f || time > _duration ||
        (k != 0 && time <= previous)) {
      *_offending_time = time;
      return false;
    }
    previous = time;
  }
  return true;
}

// Builds the reduced joint (recursively) in depth-first order.
void BuildJoint(const FlatSkeleton& _flat,
                const ozz::vector<math::Transform>& _rest,
                const ozz::vector<ozz::vector<int>>& _children, int _joint,
                RawSkeleton::Joint* _out) {
  _out->name = _flat.joints[_joint]->name;
  _out->transform = _rest[_joint];
  _out->children.resize(_children[_joint].size());
  for (size_t c = 0; c < _children[_joint].size(); ++c) {
    BuildJoint(_flat, _rest, _children, _children[_joint][c],
               &_out->children[c]);
  }
}

}  // namespace

bool SkeletonReducer::operator()(const RawSkeleton& _skeleton,
                                 span<const Clip> _clips,
                                 span<const int> _must_keep, Result* _result,
                                 Error* _error) const {
  if (_error) {
    *_error = Error();
  }
  if (!_result) {
    return false;
  }
  // Never leaves a partial result behind.
  *_result = Result();

  const auto fail = [_error](Error::Kind _kind, int _animation, int _joint,
                             float _time) {
    if (_error) {
      _error->kind = _kind;
      _error->animation = _animation;
      _error->joint = _joint;
      _error->time = _time;
    }
    return false;
  };

  // The recomposition tolerance must be finite and strictly positive.
  if (!std::isfinite(tolerance) || tolerance <= 0.f) {
    return fail(Error::kInvalidTolerance, -1, -1, 0.f);
  }

  // The skeleton must be valid (joint count within Skeleton::kMaxJoints).
  if (!_skeleton.Validate()) {
    return fail(Error::kInvalidSkeleton, -1, -1, 0.f);
  }

  // Flattens the hierarchy in depth-first order (SkeletonBuilder's joint
  // numbering convention).
  FlatSkeleton flat;
  Flatten(_skeleton.roots, -1, &flat);
  const int num_joints = static_cast<int>(flat.joints.size());

  // Rest pose transforms must be finite, with non-zero scales and unit
  // rotations.
  for (int j = 0; j < num_joints; ++j) {
    const math::Transform& rest = flat.joints[j]->transform;
    if (!IsFinite(rest.translation) || !IsFinite(rest.rotation) ||
        !IsFinite(rest.scale)) {
      return fail(Error::kInvalidRestPose, -1, j, 0.f);
    }
    if (rest.scale.x == 0.f || rest.scale.y == 0.f || rest.scale.z == 0.f) {
      return fail(Error::kInvalidRestPose, -1, j, 0.f);
    }
    if (!math::IsNormalized(rest.rotation)) {
      return fail(Error::kInvalidRestPose, -1, j, 0.f);
    }
  }

  // Must-keep set, in original joint indices.
  ozz::vector<char> must_keep(num_joints, 0);
  for (size_t k = 0; k < _must_keep.size(); ++k) {
    const int joint = _must_keep[k];
    if (joint < 0 || joint >= num_joints) {
      return fail(Error::kInvalidKeepIndex, -1, joint, 0.f);
    }
    must_keep[joint] = 1;
  }

  // Validates clips and counts the total number of scheduled times.
  ozz::vector<size_t> clip_offsets(_clips.size() + 1, 0);
  for (size_t i = 0; i < _clips.size(); ++i) {
    const Clip& clip = _clips[i];
    if (clip.animation == nullptr) {
      return fail(Error::kInvalidAnimation, static_cast<int>(i), -1, 0.f);
    }
    const RawAnimation& animation = *clip.animation;
    if (!std::isfinite(animation.duration) || animation.duration <= 0.f) {
      return fail(Error::kInvalidAnimation, static_cast<int>(i), -1,
                  animation.duration);
    }
    // Every joint of the original skeleton must have a track (possibly with
    // empty channels, which SampleAnimation samples as identity).
    if (animation.num_tracks() != num_joints) {
      return fail(Error::kTrackCountMismatch, static_cast<int>(i), -1, 0.f);
    }
    for (int j = 0; j < num_joints; ++j) {
      const RawAnimation::JointTrack& track = animation.tracks[j];
      float offending = 0.f;
      if (!ValidateKeys(track.translations, animation.duration, &offending) ||
          !ValidateKeys(track.rotations, animation.duration, &offending) ||
          !ValidateKeys(track.scales, animation.duration, &offending)) {
        return fail(Error::kInvalidAnimation, static_cast<int>(i), j,
                    offending);
      }
    }
    // The schedule must be finite, strictly increasing, and include both 0
    // and the animation duration.
    const span<const float> times = clip.times;
    if (times.size() < 2) {
      return fail(Error::kInvalidSchedule, static_cast<int>(i), -1,
                  times.size() != 0 ? times[0] : 0.f);
    }
    if (times.front() != 0.f) {
      return fail(Error::kInvalidSchedule, static_cast<int>(i), -1,
                  times.front());
    }
    if (times.back() != animation.duration) {
      return fail(Error::kInvalidSchedule, static_cast<int>(i), -1,
                  times.back());
    }
    for (size_t k = 1; k < times.size(); ++k) {
      if (!std::isfinite(times[k]) || times[k] <= times[k - 1]) {
        return fail(Error::kInvalidSchedule, static_cast<int>(i), -1,
                    times[k]);
      }
    }
    clip_offsets[i + 1] = clip_offsets[i] + times.size();
  }
  const size_t total_times = clip_offsets.back();

  // Samples every clip at every scheduled time. locals[j * total_times +
  // clip_offsets[i] + k] is the current local transform of joint j for clip i
  // at scheduled time k. Empty channels are sampled as identity, like
  // SampleAnimation does.
  ozz::vector<math::Transform> locals(static_cast<size_t>(num_joints) *
                                      total_times);
  {
    ozz::vector<math::Transform> posture(num_joints);
    for (size_t i = 0; i < _clips.size(); ++i) {
      const Clip& clip = _clips[i];
      for (size_t k = 0; k < clip.times.size(); ++k) {
        if (!SampleAnimation(*clip.animation, clip.times[k],
                             make_span(posture))) {
          return fail(Error::kInvalidAnimation, static_cast<int>(i), -1,
                      clip.times[k]);
        }
        for (int j = 0; j < num_joints; ++j) {
          locals[static_cast<size_t>(j) * total_times + clip_offsets[i] + k] =
              posture[j];
        }
      }
    }
  }

  // Current reduction state. parent[j] is the current parent of joint j (in
  // original indices), rest[j] its current rest pose local transform. Both
  // absorb the transforms of removed ancestors as the reduction progresses.
  ozz::vector<int> parent(flat.parents);
  ozz::vector<char> present(num_joints, 1);
  ozz::vector<math::Transform> rest(num_joints);
  ozz::vector<ozz::vector<int>> children(num_joints);
  for (int j = 0; j < num_joints; ++j) {
    rest[j] = flat.joints[j]->transform;
    if (flat.parents[j] >= 0) {
      children[flat.parents[j]].push_back(j);
    }
  }

  // Attempts to remove every removable joint once, from the highest original
  // index to the lowest. Descendants of a joint have higher indices than the
  // joint itself (depth-first order), so all descendants have already been
  // processed when a joint is attempted.
  for (int j = num_joints - 1; j >= 0; --j) {
    if (flat.parents[j] < 0 || must_keep[j]) {
      continue;  // Original roots and must-keep joints are never removed.
    }

    // Computes the candidate local transforms of every child that would be
    // re-attached: the product of the skipped local transforms, multiplied in
    // original parent-to-child order. The rest pose and every clip at every
    // scheduled time must all decompose to a finite, non-zero scale TRS that
    // recomposes within tolerance, otherwise the joint is kept.
    struct Candidate {
      int child;
      ozz::vector<math::Transform> trs;  // [0] is rest pose, then per time.
    };
    ozz::vector<Candidate> candidates;
    bool removable = true;
    for (size_t c = 0; removable && c < children[j].size(); ++c) {
      const int child = children[j][c];
      if (!present[child]) {
        continue;
      }
      Candidate candidate;
      candidate.child = child;
      candidate.trs.resize(1 + total_times);
      if (!DecomposeChecked(math::Float4x4::FromAffine(rest[j]) *
                                math::Float4x4::FromAffine(rest[child]),
                            tolerance, &candidate.trs[0])) {
        removable = false;
        break;
      }
      for (size_t t = 0; t < total_times; ++t) {
        const math::Float4x4 composed =
            math::Float4x4::FromAffine(
                locals[static_cast<size_t>(j) * total_times + t]) *
            math::Float4x4::FromAffine(
                locals[static_cast<size_t>(child) * total_times + t]);
        if (!DecomposeChecked(composed, tolerance, &candidate.trs[1 + t])) {
          removable = false;
          break;
        }
      }
      if (removable) {
        candidates.push_back(std::move(candidate));
      }
    }
    if (!removable) {
      continue;  // Keeps the joint, attempts the next one.
    }

    // Applies the removal: re-attaches children to the closest kept ancestor
    // and stores their new local transforms.
    const int p = parent[j];
    for (size_t c = 0; c < candidates.size(); ++c) {
      const int child = candidates[c].child;
      rest[child] = candidates[c].trs[0];
      for (size_t t = 0; t < total_times; ++t) {
        locals[static_cast<size_t>(child) * total_times + t] =
            candidates[c].trs[1 + t];
      }
      parent[child] = p;
      children[p].push_back(child);
    }
    present[j] = 0;
    children[j].clear();
  }

  Result result;

  // Joint mappings. The reduced skeleton depth-first order is the original
  // depth-first order restricted to kept joints.
  result.old_to_new.assign(num_joints, -1);
  for (int j = 0; j < num_joints; ++j) {
    if (present[j]) {
      result.old_to_new[j] = static_cast<int>(result.new_to_old.size());
      result.new_to_old.push_back(j);
    }
  }

  // Children lists of kept joints, in original traversal order.
  ozz::vector<ozz::vector<int>> out_children(num_joints);
  ozz::vector<int> out_roots;
  for (int j = 0; j < num_joints; ++j) {
    if (!present[j]) {
      continue;
    }
    if (parent[j] < 0) {
      out_roots.push_back(j);
    } else {
      out_children[parent[j]].push_back(j);
    }
  }

  // Builds the reduced skeleton.
  result.skeleton.roots.resize(out_roots.size());
  for (size_t r = 0; r < out_roots.size(); ++r) {
    BuildJoint(flat, rest, out_children, out_roots[r],
               &result.skeleton.roots[r]);
  }

  // Builds the reduced animations. Every kept joint gets a full TRS key at
  // every scheduled time of its clip.
  result.animations.resize(_clips.size());
  for (size_t i = 0; i < _clips.size(); ++i) {
    const Clip& clip = _clips[i];
    RawAnimation& output = result.animations[i];
    output.name = clip.animation->name;
    output.duration = clip.animation->duration;
    const size_t num_times = clip.times.size();
    output.tracks.resize(result.new_to_old.size());
    for (size_t nj = 0; nj < result.new_to_old.size(); ++nj) {
      const int oj = result.new_to_old[nj];
      const math::Transform* joint_locals =
          &locals[static_cast<size_t>(oj) * total_times + clip_offsets[i]];
      RawAnimation::JointTrack& track = output.tracks[nj];
      track.translations.reserve(num_times);
      track.rotations.reserve(num_times);
      track.scales.reserve(num_times);
      math::Quaternion previous = math::Quaternion::identity();
      for (size_t k = 0; k < num_times; ++k) {
        const math::Transform& local = joint_locals[k];
        const float time = clip.times[k];
        const RawAnimation::TranslationKey tkey = {time, local.translation};
        track.translations.push_back(tkey);
        // Keeps successive rotation keys in the same hemisphere, so that
        // interpolation takes the shortest path.
        math::Quaternion rotation = local.rotation;
        if (k != 0 && math::Dot(previous, rotation) < 0.f) {
          rotation = -rotation;  // q and -q are the same rotation.
        }
        previous = rotation;
        const RawAnimation::RotationKey rkey = {time, rotation};
        track.rotations.push_back(rkey);
        const RawAnimation::ScaleKey skey = {time, local.scale};
        track.scales.push_back(skey);
      }
    }
  }

  *_result = std::move(result);
  return true;
}

bool SkeletonReducer::RemapSlots(span<const int> _slot_joints,
                                 span<const int> _old_to_new,
                                 ozz::vector<int>* _remapped, Error* _error) {
  if (_error) {
    *_error = Error();
  }
  if (_remapped == nullptr) {
    return false;
  }
  _remapped->clear();

  ozz::vector<int> remapped;
  remapped.reserve(_slot_joints.size());
  for (size_t s = 0; s < _slot_joints.size(); ++s) {
    const int joint = _slot_joints[s];
    const int count = static_cast<int>(_old_to_new.size());
    if (joint < 0 || joint >= count) {
      if (_error) {
        _error->kind = Error::kSlotJointOutOfRange;
        _error->joint = joint;
        _error->slot = static_cast<int>(s);
      }
      return false;
    }
    const int mapped = _old_to_new[joint];
    if (mapped < 0) {
      if (_error) {
        _error->kind = Error::kSlotJointRemoved;
        _error->joint = joint;
        _error->slot = static_cast<int>(s);
      }
      return false;
    }
    remapped.push_back(mapped);
  }
  *_remapped = std::move(remapped);
  return true;
}
}  // namespace offline
}  // namespace animation
}  // namespace ozz
