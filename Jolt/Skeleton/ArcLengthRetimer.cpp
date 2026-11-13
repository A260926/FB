// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-FileCopyrightText: 2025 Jorrit Rouwe
// SPDX-License-Identifier: MIT

#include <Jolt/Jolt.h>

#include <Jolt/Skeleton/ArcLengthRetimer.h>

#include <algorithm>
#include <cmath>

JPH_NAMESPACE_BEGIN

namespace
{

/// Shared counter for every full pose evaluation performed by the tool.
class EvaluationBudget
{
public:
	explicit				EvaluationBudget(int inBudget) : mRemaining(inBudget) { }

	bool					Acquire()
	{
		if (mRemaining <= 0)
			return false;
		--mRemaining;
		return true;
	}

private:
	int						mRemaining;
};

struct TrackRef
{
	int											mJointIndex = -1;
	const SkeletalAnimation::AnimatedJoint *	mTrack = nullptr;
};

static bool sIsFinite(float inV)
{
	return std::isfinite(inV);
}

static bool sIsFinite(Vec3Arg inV)
{
	return sIsFinite(inV.GetX()) && sIsFinite(inV.GetY()) && sIsFinite(inV.GetZ());
}

static bool sIsFinite(QuatArg inQ)
{
	return sIsFinite(inQ.mValue.GetX()) && sIsFinite(inQ.mValue.GetY())
		&& sIsFinite(inQ.mValue.GetZ()) && sIsFinite(inQ.mValue.GetW());
}

/// Evaluates the measured curve and full poses. Every evaluation starts from a
/// fresh copy of the rest pose local states, so joints without a track keep
/// their rest pose state instead of inheriting a previously sampled value.
class CurveEvaluator
{
public:
	using JointState = SkeletalAnimation::JointState;
	using JointStateVector = SkeletonPose::JointStateVector;

							CurveEvaluator(const Skeleton *inSkeleton, const JointStateVector &inRestStates,
								const Array<TrackRef> &inTracks, int inMeasuredJoint, Vec3Arg inLocalPoint,
								EvaluationBudget &inBudget) :
		mSkeleton(inSkeleton),
		mRestStates(inRestStates),
		mTracks(inTracks),
		mLocalPoint(inLocalPoint),
		mBudget(inBudget)
	{
		// Collect the ancestor chain of the measured joint, root first.
		for (int j = inMeasuredJoint; j >= 0; j = inSkeleton->GetJoint(j).mParentJointIndex)
			mChain.push_back(j);
		std::reverse(mChain.begin(), mChain.end());

		mPose.resize(inRestStates.size());
	}

	/// Full pose at inTime using linear translation and shortest arc SLERP,
	/// always treated as non looping and clamped to the closed interval [0, D].
	bool					EvaluatePose(float inTime, JointStateVector &outPose)
	{
		if (!mBudget.Acquire())
			return false;

		mPose = mRestStates;
		if (!ApplyTracks(inTime))
			return false;

		outPose = mPose;
		return true;
	}

	/// Model space position of the local point at inTime. The root offset is
	/// never part of the model matrices here.
	bool					EvaluatePoint(float inTime, Vec3 &outPoint)
	{
		if (!mBudget.Acquire())
			return false;

		mPose = mRestStates;
		if (!ApplyTracks(inTime))
			return false;

		Mat44 model = Mat44::sIdentity();
		for (int j : mChain)
		{
			Mat44 local = mPose[j].ToMatrix();
			int parent = mSkeleton->GetJoint(j).mParentJointIndex;
			model = parent >= 0 ? model * local : local;
		}

		outPoint = model * mLocalPoint;
		return sIsFinite(outPoint);
	}

private:
	/// Mirrors SkeletalAnimation::Sample on a single track, without the looping
	/// time correction and writing into the supplied pose.
	bool					SampleTrack(const TrackRef &inTrack, float inTime)
	{
		const SkeletalAnimation::KeyframeVector &keys = inTrack.mTrack->mKeyframes;

		int high = (int)keys.size(), low = -1;
		while (high - low > 1)
		{
			int probe = (high + low) / 2;
			if (keys[probe].mTime < inTime)
				low = probe;
			else
				high = probe;
		}

		JointState &state = mPose[inTrack.mJointIndex];
		if (low == -1)
		{
			// First key (also used at t == 0)
			state = static_cast<const JointState &>(keys.front());
		}
		else if (high == (int)keys.size())
		{
			// Last key, returned verbatim at t == D (no wrap around)
			state = static_cast<const JointState &>(keys.back());
		}
		else
		{
			const SkeletalAnimation::Keyframe &s1 = keys[low];
			const SkeletalAnimation::Keyframe &s2 = keys[low + 1];
			float fraction = (inTime - s1.mTime) / (s2.mTime - s1.mTime);
			state.mTranslation = (1.0f - fraction) * s1.mTranslation + fraction * s2.mTranslation;
			state.mRotation = s1.mRotation.SLERP(s2.mRotation, fraction);
		}
		return sIsFinite(state.mTranslation) && sIsFinite(state.mRotation);
	}

	bool					ApplyTracks(float inTime)
	{
		for (const TrackRef &track : mTracks)
			if (!SampleTrack(track, inTime))
				return false;
		return true;
	}

	const Skeleton *			mSkeleton;
	const JointStateVector &	mRestStates;
	const Array<TrackRef> &	mTracks;
	Vec3						mLocalPoint;
	EvaluationBudget &			mBudget;
	Array<int>					mChain;
	JointStateVector			mPose;
};

/// One accepted micro interval of the adaptive global curve integration.
struct ArcLeaf
{
	float					mT0 = 0.0f;
	float					mT1 = 0.0f;
	double					mLength = 0.0f;
};

/// Adaptive arc length integration of a single cell. Uses the Richardson
/// extrapolation of nested chord sums (equivalent to adaptive Simpson on the
/// curve): a cell is accepted once |fine chord sum - coarse chord| is within
/// the cell tolerance, and the returned length is (4 * fine - coarse) / 3.
/// Recursion refines far beyond the output key vertices, so chord length
/// between output keys is never used as the curve length.
static bool sIntegrateCell(CurveEvaluator &inEvaluator, float inA, float inB,
	Vec3Arg inPA, Vec3Arg inPB, double inTolerance, int inDepth, Array<ArcLeaf> &outLeaves)
{
	float mid = (inA + inB) * 0.5f;
	if (mid <= inA || mid >= inB)
	{
		// Floating point resolution exhausted; the chord is the best estimate.
		outLeaves.push_back({ inA, inB, (inPB - inPA).Length() });
		return true;
	}

	Vec3 pm;
	if (!inEvaluator.EvaluatePoint(mid, pm))
		return false;

	double coarse = (inPB - inPA).Length();
	double fine = (pm - inPA).Length() + (inPB - pm).Length();

	if (inDepth == 0 || std::abs(fine - coarse) <= inTolerance)
	{
		outLeaves.push_back({ inA, inB, (4.0 * fine - coarse) / 3.0 });
		return true;
	}

	// Split the tolerance with the two child cells.
	if (!sIntegrateCell(inEvaluator, inA, mid, inPA, pm, inTolerance * 0.5, inDepth - 1, outLeaves))
		return false;
	if (!sIntegrateCell(inEvaluator, mid, inB, pm, inPB, inTolerance * 0.5, inDepth - 1, outLeaves))
		return false;
	return true;
}

/// Integrate an arbitrary interval with a fixed uniform starting grid. The
/// grid makes a full revolution inside one interval visible (its sampled
/// points cannot all coincide), so motion is never declared absent from just
/// the interval start, midpoint and end.
static bool sIntegrateInterval(CurveEvaluator &inEvaluator, float inA, float inB,
	double inTolerance, Array<ArcLeaf> &outLeaves)
{
	const int cells = 8;

	Array<Vec3> points;
	points.resize(cells + 1);
	for (int i = 0; i <= cells; ++i)
	{
		float t = inA + (inB - inA) * (float(i) / float(cells));
		if (!inEvaluator.EvaluatePoint(t, points[i]))
			return false;
	}

	double cell_tolerance = inTolerance / cells;
	for (int i = 0; i < cells; ++i)
	{
		float t0 = inA + (inB - inA) * (float(i) / float(cells));
		float t1 = inA + (inB - inA) * (float(i + 1) / float(cells));
		if (!sIntegrateCell(inEvaluator, t0, t1, points[i], points[i + 1], cell_tolerance, 32, outLeaves))
			return false;
	}
	return true;
}

/// True arc length of [inA, inB], used for the small correction checks while
/// solving inside an accepted leaf. The interval is at most one leaf wide, so
/// its curve is nearly straight and the integration stays cheap.
static bool sIntegrateLength(CurveEvaluator &inEvaluator, float inA, float inB,
	double inTolerance, double &outLength)
{
	Array<ArcLeaf> leaves;
	if (!sIntegrateInterval(inEvaluator, inA, inB, inTolerance, leaves))
		return false;

	double total = 0.0;
	for (const ArcLeaf &leaf : leaves)
		total += leaf.mLength;
	outLength = total;
	return true;
}

/// Validate every contract precondition. Returns false on the first problem.
static bool sValidate(const SkeletalAnimation &inAnimation, const SkeletonPose &inRestPose,
	const ArcLengthSettings &inSettings, Array<TrackRef> &outTracks, Array<float> &outBreakpoints)
{
	const Skeleton *skeleton = inRestPose.GetSkeleton();
	if (skeleton == nullptr)
		return false;

	int joint_count = skeleton->GetJointCount();
	if (joint_count <= 0)
		return false;

	// Parents must precede their children.
	if (!skeleton->AreJointsCorrectlyOrdered())
		return false;

	// Skeleton joints must have unique names, otherwise track names are ambiguous.
	for (int j = 0; j < joint_count; ++j)
		for (int k = j + 1; k < joint_count; ++k)
			if (skeleton->GetJoint(j).mName == skeleton->GetJoint(k).mName)
				return false;

	if ((int)inRestPose.GetJointCount() != joint_count)
		return false;

	// Settings.
	if (inSettings.mJointIndex < 0 || inSettings.mJointIndex >= joint_count)
		return false;
	if (inSettings.mSampleCount < 2)
		return false;
	if (!sIsFinite(inSettings.mOutputDuration) || inSettings.mOutputDuration <= 0.0f)
		return false;
	if (!std::isfinite(inSettings.mLengthTolerance) || inSettings.mLengthTolerance <= 0.0)
		return false;
	if (inSettings.mMaxEvaluations <= 0)
		return false;
	if (!sIsFinite(inSettings.mLocalPoint))
		return false;

	// Rest pose local states.
	for (int j = 0; j < joint_count; ++j)
	{
		const SkeletalAnimation::JointState &state = inRestPose.GetJoint(j);
		if (!sIsFinite(state.mTranslation) || !sIsFinite(state.mRotation) || !state.mRotation.IsNormalized())
			return false;
	}

	// Tracks.
	const SkeletalAnimation::AnimatedJointVector &tracks = inAnimation.GetAnimatedJoints();
	if (tracks.empty())
		return false;

	float duration = inAnimation.GetDuration();
	if (!sIsFinite(duration) || duration <= 0.0f)
		return false;

	Array<bool> used;
	used.resize(joint_count, false);

	outTracks.reserve(tracks.size());
	Array<float> key_times;

	for (const SkeletalAnimation::AnimatedJoint &aj : tracks)
	{
		if (aj.mKeyframes.empty())
			return false;

		int joint = skeleton->GetJointIndex(aj.mJointName);
		if (joint < 0)
			return false;
		if (used[joint])
			return false; // Duplicate track for a skeleton joint.
		used[joint] = true;

		if (aj.mKeyframes.front().mTime != 0.0f || aj.mKeyframes.back().mTime != duration)
			return false;

		float previous = -1.0f;
		for (const SkeletalAnimation::Keyframe &key : aj.mKeyframes)
		{
			if (!sIsFinite(key.mTime) || key.mTime < 0.0f || key.mTime > duration)
				return false;
			if (key.mTime <= previous)
				return false; // Strictly increasing key times.
			previous = key.mTime;

			if (!sIsFinite(key.mTranslation) || !sIsFinite(key.mRotation) || !key.mRotation.IsNormalized())
				return false;
		}

		outTracks.push_back({ joint, &aj });
	}

	// Breakpoints: every key time of every track on the ancestor chain of the
	// measured joint, plus 0 and D, sorted and without duplicates.
	key_times.push_back(0.0f);
	key_times.push_back(duration);
	for (const TrackRef &track : outTracks)
	{
		bool is_ancestor = false;
		for (int j = inSettings.mJointIndex; j >= 0; j = skeleton->GetJoint(j).mParentJointIndex)
			if (j == track.mJointIndex)
			{
				is_ancestor = true;
				break;
			}
		if (!is_ancestor)
			continue;

		for (const SkeletalAnimation::Keyframe &key : track.mTrack->mKeyframes)
			key_times.push_back(key.mTime);
	}

	std::sort(key_times.begin(), key_times.end());
	key_times.erase(std::unique(key_times.begin(), key_times.end()), key_times.end());
	outBreakpoints = std::move(key_times);

	return outBreakpoints.size() >= 2;
}

} // namespace

bool RetimingByJointArcLength(const SkeletalAnimation &inAnimation, const SkeletonPose &inRestPose,
	const ArcLengthSettings &inSettings, ArcLengthResult &out)
{
	Array<TrackRef> tracks;
	Array<float> breakpoints;
	if (!sValidate(inAnimation, inRestPose, inSettings, tracks, breakpoints))
		return false;

	const Skeleton *skeleton = inRestPose.GetSkeleton();
	const int joint_count = skeleton->GetJointCount();
	const float duration = inAnimation.GetDuration();
	const int sample_count = inSettings.mSampleCount;
	const int segment_count = (int)breakpoints.size() - 1;

	EvaluationBudget budget(inSettings.mMaxEvaluations);
	CurveEvaluator evaluator(skeleton, inRestPose.GetJoints(), tracks,
		inSettings.mJointIndex, inSettings.mLocalPoint, budget);

	// A timeline segment between two consecutive relevant key times. Within a
	// segment every relevant track interpolates between a fixed pair of keys,
	// so the measured curve is a single continuous piece.
	struct Segment
	{
		float				mT0 = 0.0f;
		float				mT1 = 0.0f;
		double				mLength = 0.0f;
		int					mLeafBegin = 0;
		int					mLeafEnd = 0;
	};

	// Integrate every segment once into a shared table of tightly accepted
	// micro leaves. The total tolerance is split over the segments, so the
	// summed integration error stays inside the requested tolerance.
	Array<ArcLeaf> leaves;
	Array<Segment> segments;
	segments.resize(segment_count);
	double total_length = 0.0;
	const double segment_tolerance = inSettings.mLengthTolerance / (4.0 * segment_count);

	for (int s = 0; s < segment_count; ++s)
	{
		Segment &segment = segments[s];
		segment.mT0 = breakpoints[s];
		segment.mT1 = breakpoints[s + 1];
		segment.mLeafBegin = (int)leaves.size();

		if (!sIntegrateInterval(evaluator, segment.mT0, segment.mT1, segment_tolerance, leaves))
			return false;

		segment.mLeafEnd = (int)leaves.size();
		double segment_length = 0.0;
		for (int k = segment.mLeafBegin; k < segment.mLeafEnd; ++k)
			segment_length += leaves[k].mLength;
		if (!std::isfinite(segment_length) || segment_length < 0.0)
			return false;

		segment.mLength = segment_length;
		total_length += segment_length;
	}

	if (!std::isfinite(total_length) || total_length < 0.0)
		return false;

	// Cumulative length at every breakpoint.
	Array<double> cumulative;
	cumulative.resize(segment_count + 1);
	cumulative[0] = 0.0;
	for (int s = 0; s < segment_count; ++s)
		cumulative[s + 1] = cumulative[s] + segments[s].mLength;

	// Solve L(t) = q * L(D) for every output key.
	Array<float> source_times;
	source_times.resize(sample_count);
	source_times[0] = 0.0f;
	source_times[sample_count - 1] = duration;

	if (total_length <= 0.0)
	{
		// Entirely stationary curve: uniform time mapping.
		for (int i = 1; i < sample_count - 1; ++i)
		{
			float q = float(i) / float(sample_count - 1);
			source_times[i] = q * duration;
		}
	}
	else
	{
		const double stop_tolerance = inSettings.mLengthTolerance * 0.25;

		for (int i = 1; i < sample_count - 1; ++i)
		{
			double target = total_length * (double(i) / double(sample_count - 1));

			// Rightmost breakpoint whose cumulative length is <= target. The
			// upper_bound based index also lands at the end of stationary
			// plateaus, implementing the rightmost solution rule.
			int r = int(std::upper_bound(cumulative.begin(), cumulative.end(), target) - cumulative.begin()) - 1;
			if (r < 0)
				r = 0;
			if (r >= segment_count)
			{
				source_times[i] = duration;
				continue;
			}

			const Segment &segment = segments[r];
			if (cumulative[r + 1] <= target)
			{
				// Target sits at the right end of a stationary piece.
				source_times[i] = segment.mT1;
				continue;
			}

			// Locate the accepted micro leaf that contains the target.
			double leaf_start_length = cumulative[r];
			int leaf_index = segment.mLeafBegin;
			for (; leaf_index < segment.mLeafEnd; ++leaf_index)
			{
				if (leaf_start_length + leaves[leaf_index].mLength >= target)
					break;
				leaf_start_length += leaves[leaf_index].mLength;
			}
			if (leaf_index >= segment.mLeafEnd)
			{
				source_times[i] = segment.mT1;
				continue;
			}

			const ArcLeaf &leaf = leaves[leaf_index];
			if (leaf.mLength <= 0.0 || leaf.mT1 <= leaf.mT0)
			{
				// Stationary leaf: jump to its right edge (rightmost solution).
				source_times[i] = leaf.mT1;
				continue;
			}

			// Initial guess from the chord proportion inside the leaf. The leaf
			// was accepted at segment tolerance scale, so its curve is straight
			// to within that scale and the guess starts very close.
			double local_target = target - leaf_start_length;
			double fraction = std::min(std::max(local_target / leaf.mLength, 0.0), 1.0);
			float solved = leaf.mT0 + float(fraction) * (leaf.mT1 - leaf.mT0);

			// Correct the guess using the true integrated length over [t0, t].
			// Each correction spans a single accepted leaf, so each integration
			// is cheap; at most a handful are needed to reach the stop tolerance.
			bool converged = false;
			for (int correction = 0; correction < 12; ++correction)
			{
				double prefix = 0.0;
				if (!sIntegrateLength(evaluator, leaf.mT0, solved, stop_tolerance * 0.25, prefix))
					return false;

				double residual = leaf_start_length + prefix - target;
				if (std::abs(residual) <= stop_tolerance)
				{
					converged = true;
					break;
				}

				// Convert length residual to a time shift using the leaf's mean
				// speed, clamped to the leaf bounds.
				double mean_speed = leaf.mLength / double(leaf.mT1 - leaf.mT0);
				if (mean_speed <= 0.0)
					break;
				float shift = float(residual / mean_speed);
				float next = solved - shift;
				next = std::min(std::max(next, leaf.mT0), leaf.mT1);
				if (next == solved)
					break;
				solved = next;
			}

			// The accumulated length at the solved time must really meet the
			// tolerance. If the budget could not provide enough accuracy the
			// call fails instead of returning an approximate result.
			if (!converged)
				return false;

			source_times[i] = solved;
		}
	}

	// Build the output animation: one track per skeleton joint, skeleton order,
	// every joint gets rotation and translation keys, all sharing source times.
	Ref<SkeletalAnimation> output = new SkeletalAnimation;
	output->SetIsLooping(false);
	SkeletalAnimation::AnimatedJointVector &output_tracks = output->GetAnimatedJoints();
	output_tracks.resize(joint_count);
	for (int j = 0; j < joint_count; ++j)
	{
		output_tracks[j].mJointName = skeleton->GetJoint(j).mName;
		output_tracks[j].mKeyframes.resize(sample_count);
	}

	SkeletonPose::JointStateVector sampled_pose;
	for (int i = 0; i < sample_count; ++i)
	{
		if (!evaluator.EvaluatePose(source_times[i], sampled_pose))
			return false;

		float output_time = inSettings.mOutputDuration * float(i) / float(sample_count - 1);
		for (int j = 0; j < joint_count; ++j)
		{
			SkeletalAnimation::Keyframe &key = output_tracks[j].mKeyframes[i];
			key.mTime = output_time;
			key.mRotation = sampled_pose[j].mRotation;
			key.mTranslation = sampled_pose[j].mTranslation;
		}
	}

	// Everything succeeded: only now touch the caller supplied result.
	ArcLengthResult result;
	result.mAnimation = output;
	result.mSourceTimes = std::move(source_times);
	result.mTotalLength = total_length;
	out = std::move(result);
	return true;
}

JPH_NAMESPACE_END
