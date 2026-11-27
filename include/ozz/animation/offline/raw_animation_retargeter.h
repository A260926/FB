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

// Forward declaration of the runtime skeleton type.
class Skeleton;

namespace offline {

// Forward declaration of the offline animation type.
struct RawAnimation;

// Defines the class responsible for retargeting an offline raw animation from
// a source skeleton to a target skeleton.
//
// Both skeletons are expected to describe a character in the same model space
// (same coordinate system, same global orientation), but they may expose
// different rest poses, different bone lengths and different numbers of
// intermediate/helper joints. Mapping is performed by joint indices (the
// caller provides, for every target joint, the index of the source joint it
// must be driven by); names are never compared.
//
// Rotation retargeting is based on model-space rotations. Let Rs and Rt be the
// model-space rotations of a mapped source joint and of its target joint in
// their respective rest poses, and Qs be the sampled source joint model-space
// rotation. The desired target model-space rotation is:
//
//   Qd = Qs * inverse(Rs) * Rt
//
// The target local rotation is then recomputed at every sampled time from the
// actual (retargeted) target parent model rotation at that same time, not from
// the target rest parent rotation. This is what makes chains whose rest poses
// or intermediate joints differ follow the source motion correctly.
//
// Joints that map to -1 are not driven: they keep their target rest local
// rotation and translation, and they still take part in the propagation to
// their descendants. Target translations always keep their target rest local
// value, except for the target root joint when it is mapped to the source root
// joint: the root rest position is then offset by
//
//   root_scale * (source_root_translation - source_rest_translation)
//
// where root_scale is a caller provided strictly positive ratio. Mapping the
// target root joint to a non-root source joint is rejected. No foot locking,
// no inverse kinematics and no scale retargeting is performed.
//
// The produced RawAnimation does not contain one keyframe per input sample.
// The caller supplies "probe times" and position/angle tolerances. The
// retargeter first computes the complete target reference pose at every probe
// time, then selects one set of translation and rotation keyframe times shared
// by all joints. Keyframe times are always a subset of the probe times, always
// include the first (0) and last (duration) probe times, and scale tracks are
// left empty (identity scale). The selection takes the whole target skeleton
// into account: errors are measured on model-space joint origins and model-
// space joint rotations, so the error at the end of a long bone chain is part
// of the selection process, not only the compression of individual local
// curves. Once resampled with the regular RawAnimation interpolation, the
// output stays within the requested tolerances at every probe time and for
// every joint, and removing any of the retained internal key times makes at
// least one of these constraints fail. No minimality is claimed between probe
// times, and no continuous error bound is claimed between probe times.
//
// Current limitations (unsupported inputs are rejected):
//  - Source and target skeletons must each have exactly one root joint.
//  - Local scale must be unit for the rest poses of both skeletons and for
//    every scale key of the source animation.
//  - Source and target translations and source rotations must be finite, and
//    source rotation keys must be (close to) unit quaternions.
//  - The source animation must have exactly as many tracks as the source
//    skeleton has joints. Empty channels follow SampleAnimation()'s identity
//    value convention.
class OZZ_ANIMOFFLINE_DLL RawAnimationRetargeter {
 public:
  // Retargeting parameters.
  struct Params {
    // Source joint index for each target joint, in target skeleton joint order.
    // The range size must be equal to the target skeleton's number of joints.
    // A value of -1 means that the target joint is not driven by the source
    // animation. Every other value must be a valid source joint index.
    // Mapping the target root joint (index of the joint without parent) to a
    // non-root source joint is invalid.
    ozz::span<const int> source_joint_indices;

    // Probe times at which the reference target pose is evaluated and at which
    // the output error constraints are enforced. The range must contain at
    // least two finite, strictly increasing values, the first one must be 0
    // and the last one must be equal to the input animation duration. Any
    // other value must be within [0, duration].
    ozz::span<const float> probe_times;

    // Maximum allowed model-space joint origin distance at probe times.
    // Must be finite and strictly positive.
    float position_tolerance = 1e-3f;

    // Maximum allowed model-space joint rotation angle error, in radians.
    // Must be finite and strictly positive.
    float angle_tolerance = 1e-3f;

    // Scale applied to the source root translation delta when the target root
    // joint is mapped to the source root joint. Must be finite and strictly
    // positive.
    float root_scale = 1.f;
  };

  // Retargets _input from _source_skeleton to _target_skeleton, using *this
  // parameters, and fills _output with the retargeted animation.
  //
  // Returns true on success. Returns false if _output is null, if _output
  // aliases _input, or if any input is invalid: invalid animation (see
  // RawAnimation::Validate()), source track count different from source joint
  // count, invalid mapping, invalid probe times, unsupported scale, non-finite
  // or degenerate transforms, invalid tolerances, invalid root scale, or
  // skeletons that don't have exactly one root.
  //
  // On failure *_output (when valid and not aliasing _input) is reset to a
  // default, valid and readable RawAnimation. _input, _source_skeleton and
  // _target_skeleton are never modified. On success, conversion is
  // deterministic: the same inputs produce the same key time sequence and the
  // same values. Quaternion signs have no effect on the produced motion.
  bool operator()(const RawAnimation& _input,
                  const Skeleton& _source_skeleton,
                  const Skeleton& _target_skeleton, const Params& _params,
                  RawAnimation* _output) const;
};
}  // namespace offline
}  // namespace animation
}  // namespace ozz
#endif  // OZZ_OZZ_ANIMATION_OFFLINE_RAW_ANIMATION_RETARGETER_H_
