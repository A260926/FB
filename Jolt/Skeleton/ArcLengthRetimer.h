// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-FileCopyrightText: 2025 Jorrit Rouwe
// SPDX-License-Identifier: MIT

#pragma once

#include <Jolt/Skeleton/SkeletalAnimation.h>
#include <Jolt/Skeleton/SkeletonPose.h>

JPH_NAMESPACE_BEGIN

/// Settings for an arc length based retiming of a skeletal animation.
/// See RetimingByJointArcLength().
class ArcLengthSettings
{
public:
	/// Index of the joint whose model space curve is measured.
	int					mJointIndex = -1;

	/// Point in the local space of mJointIndex that traces the measured curve.
	Vec3				mLocalPoint = Vec3::sZero();

	/// Number of keys written to the output animation. Must be >= 2.
	int					mSampleCount = 0;

	/// Duration in seconds of the output animation. Must be > 0. The output keys
	/// are placed at i * mOutputDuration / (mSampleCount - 1).
	float				mOutputDuration = 0.0f;

	/// Absolute length tolerance, in the same model space units as mLocalPoint.
	/// Both the total arc length and the accumulated lengths at the internal
	/// output keys are guaranteed within this tolerance. Must be > 0.
	double				mLengthTolerance = 0.0;

	/// Maximum number of full pose evaluations that may be spent on integration
	/// and inverse solving together. A budget of 0 is rejected. When the budget
	/// is exhausted before the tolerance is met the function fails instead of
	/// returning an approximate result.
	int					mMaxEvaluations = 0;
};

/// Result of RetimingByJointArcLength().
class ArcLengthResult
{
public:
	/// Non looping animation with one track per skeleton joint in skeleton order,
	/// each containing mSampleCount keys sampled at the retimed source times.
	Ref<SkeletalAnimation>	mAnimation;

	/// Source time (in the original animation) used for each output key,
	/// mSampleCount entries. The first entry is always 0 and the last one the
	/// original duration D.
	Array<float>		mSourceTimes;

	/// Total arc length L(D) of the measured curve, in model space units.
	double				mTotalLength = 0.0;
};

/// Retime a skeletal animation so that a point attached to a single joint
/// travels equal arc length between consecutive output keys.
///
/// The measured curve is (model matrix of inSettings.mJointIndex at time t) *
/// inSettings.mLocalPoint. Its arc length L(t) is the integral of the curve
/// speed over [0, t]. The SkeletonPose root offset is not part of the curve.
///
/// Evaluation rules (identical to SkeletalAnimation::Sample, but always
/// non looping even when the input clip is marked as looping):
/// - Every source time is evaluated starting from the rest pose local states,
///   so joints without a track keep their rest pose local state instead of
///   inheriting a previously sampled value.
/// - Translation is interpolated linearly, rotation with shortest arc SLERP.
/// - The clip is evaluated on the closed interval [0, D], where D is the
///   original duration. At t = D the last key is returned, never the wrapped
///   first key.
/// - Track names are resolved through the rest pose Skeleton; track order does
///   not have to match joint order.
///
/// Segmentation: the timeline is split at every key time of every track on the
/// ancestor chain of the measured joint (including the joint itself), because
/// each of those tracks can change the curve. Every segment therefore has a
/// constant set of bracketing keys and a continuous curve.
///
/// Error control and stopping conditions:
/// - Each segment is integrated on an initial uniform 8 cell grid (so a full
///   revolution inside one cell can never look like no motion) followed by
///   adaptive Richardson refinement of nested chord sums, equivalent to
///   adaptive Simpson quadrature on the curve. A cell is accepted once
///   |fine chord sum - coarse chord| is within its share of the segment
///   tolerance; the reported cell length is the extrapolated
///   (4 * fine - coarse) / 3. Refinement depth is capped at 32 as a safety
///   limit, after which the current best estimate is kept. The per segment
///   tolerances sum to at most mLengthTolerance / 4, leaving headroom for the
///   inverse solving below. Accepted cells, called leaves, are kept in a table
///   with their time range and arc length. Chord length between output
///   vertices is never used as a substitute for the integrated curve length.
/// - Each internal output key asks for the rightmost time t at which
///   L(t) = q * L(D), q = i / (mSampleCount - 1). The rightmost solution means
///   that fully stationary ranges are skipped to their end: target lengths are
///   located with an upper bound search over the cumulative breakpoint lengths
///   and then over the leaf table, which lands at the end of every zero length
///   plateau. Inside the selected leaf a chord proportion gives an initial
///   guess, corrected by a small fixed number of true sub interval arc length
///   integrations using the leaf's mean speed as a Newton step, stopping as
///   soon as the absolute accumulated length residual is within
///   mLengthTolerance / 4. A stationary leaf is resolved to its right edge.
/// - Every pose evaluation performed by the integration, the correction steps
///   and the final output sampling is counted against mMaxEvaluations. If the
///   budget runs out, or a correction cannot reach the stopping tolerance, the
///   function returns false and out is left untouched instead of returning an
///   approximate result.
///
/// Degenerate cases:
/// - If the whole curve has zero length the source times are q * D.
/// - Quaternion key sign inversions are supported; SLERP takes the shortest arc.
///
/// On failure out is not modified and false is returned. Rejected inputs
/// include (among others): empty tracks, duplicate or unknown joint names on
/// tracks, no skeleton or invalid parent ordering, D <= 0, out of range joint
/// index, non finite input values, non unit rotations, mSampleCount < 2,
/// mOutputDuration <= 0, mLengthTolerance <= 0 and a zero evaluation budget.
JPH_EXPORT bool RetimingByJointArcLength(const SkeletalAnimation &inAnimation,
	const SkeletonPose &inRestPose, const ArcLengthSettings &inSettings,
	ArcLengthResult &out);

JPH_NAMESPACE_END
