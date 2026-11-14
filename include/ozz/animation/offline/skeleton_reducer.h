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

#ifndef OZZ_OZZ_ANIMATION_OFFLINE_SKELETON_REDUCER_H_
#define OZZ_OZZ_ANIMATION_OFFLINE_SKELETON_REDUCER_H_

#include "ozz/animation/offline/export.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/span.h"

namespace ozz {
namespace animation {
namespace offline {

// Reduces a skeleton and a set of animations by removing helper joints that
// are not required at runtime (typically joints that carry no skinning
// weights).
//
// Joint indices used as inputs and outputs of the reduction follow the
// SkeletonBuilder convention: joints are numbered in the depth-first traversal
// order of the raw skeleton hierarchy. Joint names are not used by the
// reduction (names don't need to be unique), all associations are index based.
//
// The reduction attempts to remove every joint once, from the highest
// original index to the lowest. Original skeleton roots and joints listed in
// the must-keep set are never removed. When a joint is removed, its children
// are re-attached to the closest kept ancestor, keeping the original traversal
// order between siblings. The local transform of a re-attached child becomes
// the product of the skipped local transforms, multiplied in original
// parent-to-child order.
//
// A removal is accepted only if, for the rest pose and for every animation at
// every scheduled time, each new local matrix can be decomposed back to a
// finite, non-zero scale TRS, and the recomposed matrix matches the composed
// one within tolerance (element-wise absolute difference). Compositions that
// generate shearing (non-uniform scale combined with rotations) cannot be
// represented as a TRS and therefore keep the joint. Negative scales are
// supported, and negating a quaternion does not change the outcome of the
// reduction.
//
// The reduced skeleton and animations only guarantee model-space matrices
// matching the original ones (within the recomposition tolerance) at the rest
// pose and at the scheduled times. Interpolation between those times is not
// guaranteed to match the original animation.
class OZZ_ANIMOFFLINE_DLL SkeletonReducer {
 public:
  // Couples an animation with the sampling schedule used to validate the
  // reduction. The schedule drives both the validation of joint removals and
  // the keyframes of the output animation: every kept joint gets a full TRS
  // key at every scheduled time.
  struct Clip {
    // The animation to reduce. Must be valid (see RawAnimation::Validate) and
    // expose exactly one track per joint of the original skeleton. Empty
    // translation, rotation or scale channels are sampled with their identity
    // value, like SampleAnimation does. Not owned, must outlive the reduction.
    const RawAnimation* animation = nullptr;

    // Sampling times for *animation. Times must be finite, strictly
    // increasing, and include both 0 and the animation duration.
    span<const float> times;
  };

  // Output of a successful reduction.
  struct Result {
    // The reduced skeleton. Rest pose transforms of re-attached joints are
    // the decomposed composition of the skipped transforms.
    RawSkeleton skeleton;

    // The reduced animations, in the same order as the input clips. Every
    // track stores a full TRS key at every scheduled time of its clip.
    // Successive rotation keys of a track are kept in the same hemisphere.
    ozz::vector<RawAnimation> animations;

    // Maps an original skeleton joint index to its index in the reduced
    // skeleton, or -1 if the joint was removed.
    ozz::vector<int> old_to_new;

    // Maps a reduced skeleton joint index to its original skeleton index.
    ozz::vector<int> new_to_old;
  };

  // Detailed report of a reduction or slot remapping failure.
  struct Error {
    // Nature of the failure.
    enum Kind {
      kNone = 0,
      // The skeleton is invalid (exceeds Skeleton::kMaxJoints joints).
      kInvalidSkeleton,
      // A rest pose transform has a zero scale, a non-finite component or a
      // non-unit rotation. joint is the original joint index.
      kInvalidRestPose,
      // An animation is null or invalid (see RawAnimation::Validate). joint is
      // the offending track index when known.
      kInvalidAnimation,
      // An animation track count differs from the skeleton joint count.
      kTrackCountMismatch,
      // A sampling schedule isn't finite, strictly increasing, or doesn't
      // include 0 and the animation duration. time is the offending entry.
      kInvalidSchedule,
      // A must-keep joint index is out of the skeleton joint range. joint is
      // the offending index.
      kInvalidKeepIndex,
      // The recomposition tolerance isn't finite and strictly positive.
      kInvalidTolerance,
      // A skinning slot references a joint that was removed by the reduction.
      kSlotJointRemoved,
      // A skinning slot references an out-of-range joint index.
      kSlotJointOutOfRange,
    };

    Kind kind = kNone;

    // Index of the offending input clip, or -1 when not animation related.
    int animation = -1;

    // Original skeleton joint index related to the failure, or -1.
    int joint = -1;

    // Skinning slot index related to the failure, or -1.
    int slot = -1;

    // Time related to the failure (offending schedule entry or key time).
    float time = 0.f;
  };

  // Element-wise absolute tolerance used when comparing a composed local
  // matrix with its TRS recomposition. Must be finite and strictly positive.
  float tolerance = 1e-3f;

  // Attempts to reduce _skeleton and the animations of _clips.
  // _must_keep lists original skeleton joint indices that must be preserved.
  // On success, fills _result and returns true. On failure, resets _result to
  // an empty result, fills _error (when provided) with the offending clip,
  // joint and time, and returns false. Inputs are never modified.
  // _result must not be null.
  bool operator()(const RawSkeleton& _skeleton, span<const Clip> _clips,
                  span<const int> _must_keep, Result* _result,
                  Error* _error = nullptr) const;

  // Remaps skinning palette slots after a reduction.
  // _slot_joints[i] is the original skeleton joint index bound to palette
  // slot i. Note that slot indices are palette indices, not skeleton joint
  // indices. Every joint referenced by a slot must have been preserved by the
  // reduction (usually by listing it in the must-keep set). Inverse-bind
  // matrices, per-vertex slot indices and weights are unaffected by the
  // reduction and don't need any change: only the per-slot skeleton joint
  // indices are remapped.
  // On success, fills _remapped with the reduced skeleton joint index of each
  // slot and returns true. On failure, resets _remapped, fills _error (when
  // provided) and returns false.
  static bool RemapSlots(span<const int> _slot_joints,
                         span<const int> _old_to_new,
                         ozz::vector<int>* _remapped,
                         Error* _error = nullptr);
};
}  // namespace offline
}  // namespace animation
}  // namespace ozz
#endif  // OZZ_OZZ_ANIMATION_OFFLINE_SKELETON_REDUCER_H_
