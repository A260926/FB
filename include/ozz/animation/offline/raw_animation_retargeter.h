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

#ifndef OZZ_OZZ_ANIMATION_OFFLINE_RAW_ANIMATION_RETARGETER_H_
#define OZZ_OZZ_ANIMATION_OFFLINE_RAW_ANIMATION_RETARGETER_H_

#include "ozz/animation/offline/export.h"
#include "ozz/base/span.h"

namespace ozz {
namespace animation {

// Forward declares the runtime skeleton type.
class Skeleton;

namespace offline {

// Forward declares the offline animation type.
struct RawAnimation;

// Retargets a raw animation from a source skeleton to a target skeleton.
//
// The two skeletons share the same model axes, but can have different rest
// poses, bone lengths and numbers of intermediate helper joints. Joints are
// therefore matched by indices, never by names: _joint_mapping gives, for
// every target joint (the mapping is indexed by target joint), the index of
// the source joint that drives it. A mapping value of -1 means the target
// joint is not driven and keeps its rest local pose (it still propagates to
// its descendants).
//
// For a target joint mapped to source joint s, the expected model-space
// rotation is Qs * inverse(Rs) * Rt, where Rs and Rt are the source and
// target rest model-space rotations, and Qs is the sampled source model-space
// rotation. The target local rotation is then computed from the *actual*
// target parent model rotation at that same time, never from the rest parent
// rotation.
//
// This first implementation only supports single-root skeletons, unit local
// scales, finite translations and unit (normalized) quaternions. Target bone
// translations keep their rest values, except for the root: when the target
// root is mapped to the source root, its translation is the target rest root
// translation plus _root_scale times the source root translation delta from
// its rest position. No foot locking or inverse kinematics is performed.
//
// The output is not a per-frame dump of the input. The complete target
// reference posture is first evaluated at every probe time. A single set of
// translation and rotation key times, shared by all joints, is then selected
// from the probe times: the two end times are always kept, and every
// remaining internal probe time is kept only if removing it would make the
// re-sampled output exceed either tolerance at some probe time, for some
// joint's model-space origin (position) or model-space rotation. Long bone
// chain end points thus take part in the selection, not only the local
// curves. There is no claim of global key minimality, nor of any continuous
// error guarantee between probe times.
//
// The function fails, leaving *_output untouched, if any of the following
// holds:
//  - _output is nullptr, or _output is the same object as _input;
//  - _input is invalid, or either skeleton has no joint or more than one
//    root joint;
//  - the number of tracks of _input is not the number of source joints;
//  - _joint_mapping does not contain one entry per target joint, an entry is
//    smaller than -1 or references a non existing source joint, or the target
//    root is not mapped to either -1 or the source root;
//  - a non-unit scale is found in a skeleton rest pose or in a sampled
//    animation channel, or a sampled animation quaternion is not normalized;
//  - _root_scale is not a positive finite value;
//  - _probe_times is empty or has a single entry, is not strictly
//    increasing, does not start at 0 or does not end at _input.duration, or
//    contains a non-finite value;
//  - _position_tolerance or _angle_tolerance is not a positive finite value.
//
// On success the output RawAnimation is valid, has one track per target
// joint, unit scales and key times all taken from _probe_times. Retargeting
// the same inputs twice produces identical key sequences, and negating a
// quaternion of the input animation does not change the resulting motion.
class OZZ_ANIMOFFLINE_DLL RawAnimationRetargeter {
 public:
  // Runs the retargeting. See class documentation for parameters and failure
  // conditions. Returns true on success, false otherwise, in which case
  // *_output is not modified and the input animation and skeletons are left
  // untouched.
  bool operator()(const RawAnimation& _input,
                  const Skeleton& _source_skeleton,
                  const Skeleton& _target_skeleton,
                  const span<const int16_t>& _joint_mapping,
                  float _root_scale, const span<const float>& _probe_times,
                  float _position_tolerance, float _angle_tolerance,
                  RawAnimation* _output) const;
};
}  // namespace offline
}  // namespace animation
}  // namespace ozz
#endif  // OZZ_OZZ_ANIMATION_OFFLINE_RAW_ANIMATION_RETARGETER_H_
