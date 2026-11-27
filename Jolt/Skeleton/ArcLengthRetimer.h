// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-FileCopyrightText: 2026 Jorrit Rouwe
// SPDX-License-Identifier: MIT

#pragma once

#include <Jolt/Skeleton/SkeletalAnimation.h>

JPH_NAMESPACE_BEGIN

class Skeleton;
class SkeletonPose;

/// Settings for an offline arc length based re-timing of a skeletal animation.
/// The animation is re-timed so that a point attached to one joint sweeps equal
/// model space arc length in equal (output) time. The same time mapping is
/// applied to every joint track, which preserves the relative timing of the
/// whole character while making the measured point move at constant speed at
/// the produced key times.
class ArcLengthSettings
{
public:
	int					mJointIndex = -1;						///< Joint that carries the measured point.
	Vec3				mLocalPoint = Vec3::sZero();			///< Measured point, expressed in the local space of mJointIndex.
	uint				mSampleCount = 0;						///< Number of key times in the output animation (at least 2).
	float				mOutputDuration = 0.0f;					///< Duration of the output animation in seconds (strictly positive).
	float				mLengthTolerance = 0.0f;				///< Absolute arc length tolerance in model space length units (strictly positive).
	uint				mMaxEvaluations = 0;					///< Maximum number of full pose evaluations allowed (strictly positive).
};

/// Output of RetimingByJointArcLength. On failure the object is left untouched.
class ArcLengthResult
{
public:
	Ref<SkeletalAnimation>	mAnimation;							///< Re-timed, non looping animation with one track per skeleton joint in skeleton order.
	Array<float>			mSourceTimes;						///< For every output key, the source time it was sampled from (size mSampleCount).
	double					mTotalLength = 0.0;					///< Total model space arc length of the measured point, in model space length units.
};

/// Re-time an animation by the model space arc length of a point attached to one joint.
///
/// The measured curve is P(t) = M_joint(t) * mLocalPoint, where M_joint(t) is the
/// model space transform of ArcLengthSettings::mJointIndex. Its arc length
/// L(t) = integral_0^t |dP/dt| dt is evaluated adaptively and inverted so that
/// output key i (at output time i * outputDuration / (sampleCount - 1)) is taken
/// from the source time t for which L(t) = (i / (sampleCount - 1)) * L(D), D
/// being the source duration. The rightmost solution is used, so completely
/// stationary intervals are skipped to their end. If L(D) == 0 the uniform
/// mapping t = q * D is used.
///
/// Pose evaluation always starts from the rest pose local joint states, then
/// applies the source clip using the library rules (linear translation,
/// shortest path SLERP), and finally walks the full parent chain to build model
/// space transforms. SkeletonPose::GetRootOffset is not part of the curve.
/// Joints without a track keep their rest pose local state; a looping source
/// clip is still evaluated on the closed interval [0, D].
///
/// Error control and stopping conditions:
/// - Arc length is computed with adaptive dyadic chord sums. On every interval
///   the coarse chord |P(b) - P(a)| is compared with the two-chord sum through
///   the midpoint; the latter is a better approximation of the true curve length
///   and their difference is the local error estimate. The interval is split
///   recursively until the estimate is within its share of
///   ArcLengthSettings::mLengthTolerance (the tolerance is halved for every
///   split, so the summed leaf errors stay within the requested value).
/// - The internal cumulative length table is built to one eighth of
///   ArcLengthSettings::mLengthTolerance, leaving headroom so that the total
///   length and the accumulated length at every internal output key match their
///   target cumulative length within the requested mLengthTolerance.
/// - When inverting, cumulative lengths that differ by no more than a small
///   scale relative float32 noise floor (a few dozen ULPs of the measured
///   point's position) are treated as equal. That makes a truly stationary
///   interval an exact plateau so the rightmost solution lands at its end; the
///   floor is far below any practical length tolerance and never scales with the
///   requested tolerance, so smaller genuine motion is still resolved.
/// - A minimum refinement depth is enforced on every interval, so a curve whose
///   endpoints coincide (a closed loop) or whose motion only appears inside the
///   interval is never declared stationary from its end points and midpoint
///   alone. Refinement also stops when float time can no longer be split; the
///   remaining leaf error is accumulated and checked against the tolerance.
/// - Every full pose evaluation performed while building the arc length table
///   and inverting it counts against ArcLengthSettings::mMaxEvaluations.
///   Samples at the same source time are evaluated once and reused. Extracting
///   the output key poses afterwards is a separate resampling step that does not
///   consume the integration budget. If the budget is exhausted before the
///   tolerance is met the function fails and returns false; an under-tolerance
///   result is never returned.
///
/// @param inAnimation Source animation. It is not modified.
/// @param inRestPose Pose whose skeleton defines the hierarchy and whose local joint states are used for joints without a track. It is not modified.
/// @param inSettings Re-timing parameters.
/// @param outResult Receives the re-timed animation, source times and total length. Left untouched on failure.
/// @return True on success, false on invalid input or an exhausted evaluation budget.
JPH_EXPORT bool			RetimingByJointArcLength(const SkeletalAnimation &inAnimation, const SkeletonPose &inRestPose,
												 const ArcLengthSettings &inSettings, ArcLengthResult &outResult);

JPH_NAMESPACE_END
