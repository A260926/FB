// Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
// SPDX-FileCopyrightText: 2026 Jorrit Rouwe
// SPDX-License-Identifier: MIT

#include <Jolt/Jolt.h>

#include <Jolt/Skeleton/ArcLengthRetimer.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <algorithm>
#include <cmath>

JPH_NAMESPACE_BEGIN

namespace ArcLengthRetimerDetail
{
	/// Per joint binding of an animation track. Tracks are looked up by the
	/// joint names stored in the skeleton, never by their order in the clip.
	struct TrackBinding
	{
		const SkeletalAnimation::KeyframeVector *mKeyframes = nullptr;		///< Keyframes of the track, or nullptr when the joint has no track.
	};

	/// Evaluates the measured point P(t) = M_joint(t) * local point.
	/// Every evaluation starts from the rest pose local states, applies the clip
	/// rules itself (linear translation, shortest path SLERP) and rebuilds the
	/// full parent chain, so no state is reused between samples.
	class CurveEvaluator
	{
	public:
		using KeyframeVector = SkeletalAnimation::KeyframeVector;

		/// Construct and bind tracks. Returns false when a track name is unknown or duplicated.
		bool					Initialize(const SkeletalAnimation &inAnimation, const SkeletonPose &inRestPose, int inJointIndex, Vec3Arg inLocalPoint)
		{
			mSkeleton = inRestPose.GetSkeleton();
			mJointCount = mSkeleton->GetJointCount();
			mMeasuredJoint = inJointIndex;
			mLocalPoint = inLocalPoint;

			mRestRot.resize(mJointCount);
			mRestTrans.resize(mJointCount);
			for (int j = 0; j < mJointCount; ++j)
			{
				mRestRot[j] = inRestPose.GetJoint(j).mRotation;
				mRestTrans[j] = inRestPose.GetJoint(j).mTranslation;
			}

			mBindings.resize(mJointCount);
			for (const SkeletalAnimation::AnimatedJoint &aj : inAnimation.GetAnimatedJoints())
			{
				int index = mSkeleton->GetJointIndex(aj.mJointName);
				if (index < 0 || mBindings[index].mKeyframes != nullptr)
					return false;												// Unknown bone name, or duplicate track for a bone
				mBindings[index].mKeyframes = &aj.mKeyframes;
			}

			mScratchRot.resize(mJointCount);
			mScratchTrans.resize(mJointCount);
			mScratchModel.resize(mJointCount);
			return true;
		}

		/// Evaluate the full local pose at inTime, always non looping and always
		/// starting from the rest pose local states. Missing tracks keep the
		/// rest pose state instead of inheriting a previously sampled value.
		void					EvaluatePose(float inTime, Array<Quat> &outRot, Array<Vec3> &outTrans)
		{
			outRot = mRestRot;
			outTrans = mRestTrans;

			for (int j = 0; j < mJointCount; ++j)
			{
				const KeyframeVector *keyframes = mBindings[j].mKeyframes;
				if (keyframes == nullptr)
					continue;

				// Same binary search as SkeletalAnimation::Sample, but without loop wrapping.
				int high = (int)keyframes->size(), low = -1;
				while (high - low > 1)
				{
					int probe = (high + low) / 2;
					if ((*keyframes)[probe].mTime < inTime)
						low = probe;
					else
						high = probe;
				}

				if (low == -1)
				{
					outRot[j] = (*keyframes).front().mRotation;
					outTrans[j] = (*keyframes).front().mTranslation;
				}
				else if (high == (int)keyframes->size())
				{
					outRot[j] = (*keyframes).back().mRotation;
					outTrans[j] = (*keyframes).back().mTranslation;
				}
				else
				{
					const SkeletalAnimation::Keyframe &s1 = (*keyframes)[low];
					const SkeletalAnimation::Keyframe &s2 = (*keyframes)[low + 1];
					float fraction = (inTime - s1.mTime) / (s2.mTime - s1.mTime);
					outTrans[j] = (1.0f - fraction) * s1.mTranslation + fraction * s2.mTranslation;
					outRot[j] = s1.mRotation.SLERP(s2.mRotation, fraction);
				}
			}
		}

		/// Evaluate the measured point at inTime. One full pose evaluation.
		Vec3					Evaluate(float inTime)
		{
			EvaluatePose(inTime, mScratchRot, mScratchTrans);

			// Rebuild the model transforms along the full ancestor chain. Root offset is excluded.
			for (int j = 0; j < mJointCount; ++j)
			{
				Mat44 local = Mat44::sRotationTranslation(mScratchRot[j], mScratchTrans[j]);
				int parent = mSkeleton->GetJoint(j).mParentJointIndex;
				mScratchModel[j] = parent >= 0? mScratchModel[parent] * local : local;
			}

			return mScratchModel[mMeasuredJoint] * mLocalPoint;
		}

		const Skeleton *		mSkeleton = nullptr;
		int						mJointCount = 0;
		int						mMeasuredJoint = -1;
		Vec3					mLocalPoint = Vec3::sZero();
		Array<Quat>				mRestRot;
		Array<Vec3>				mRestTrans;
		Array<TrackBinding>		mBindings;

	private:
		Array<Quat>				mScratchRot;
		Array<Vec3>				mScratchTrans;
		Array<Mat44>			mScratchModel;
	};

	/// Adaptive arc length integration by dyadic chord sums.
	///
	/// For an interval [a, b] the coarse estimate is the chord |P(b) - P(a)| and
	/// the fine estimate the two chords through the midpoint. By the triangle
	/// inequality the fine sum is never smaller than the coarse chord, and as the
	/// dyadic partition is refined the chord sum converges from below to the true
	/// arc length of the smooth curve. Their difference is therefore a
	/// conservative local error estimate. A minimum refinement depth guarantees
	/// that motion hidden inside an interval (e.g. a closed loop whose endpoints
	/// coincide) is always discovered instead of being judged from a few samples.
	///
	/// The table is built to an internal tolerance of 1/cAccuracyFactor of the
	/// caller requested length tolerance. Length inversion then solves against
	/// this more accurate table, leaving the remaining error budget as
	/// headroom so that cumulative lengths at the output keys stay within the
	/// requested tolerance of the true arc length.
	///
	/// After refinement, every evaluated point is sorted into one cumulative
	/// length table covering [0, D], which the length inversion walks. Point
	/// samples are evaluated once and shared everywhere.
	class ArcLengthIntegrator
	{
	public:
		/// The internal table is cAccuracyFactor times more accurate than asked.
		static constexpr int	cAccuracyFactor = 8;

		using PointSample = std::pair<float, Vec3>;

		explicit				ArcLengthIntegrator(CurveEvaluator &inEvaluator, uint inMaxEvaluations) :
			mEvaluator(inEvaluator), mMaxEvaluations(inMaxEvaluations) { }

		bool					BudgetExhausted() const					{ return mBudgetFailed; }
		uint					GetEvaluationCount() const				{ return mEvaluationCount; }
		double					GetTotalLength() const					{ return mTableLengths.empty()? 0.0 : mTableLengths.back(); }
		double					GetInternalTolerance() const			{ return mInternalTolerance; }

		/// Point on the curve at inTime, evaluated at most once per distinct time.
		Vec3					GetPoint(float inTime)
		{
			// Refinement revisits the most recent times, so scan from the back.
			for (size_t i = mCache.size(); i > 0; --i)
				if (mCache[i - 1].first == inTime)
					return mCache[i - 1].second;

			if (mEvaluationCount >= mMaxEvaluations)
			{
				mBudgetFailed = true;
				return Vec3::sZero();
			}
			++mEvaluationCount;

			Vec3 point = mEvaluator.Evaluate(inTime);
			mCache.emplace_back(inTime, point);
			return point;
		}

		/// Chord length between two curve samples.
		double					Chord(float inA, float inB)
		{
			Vec3 a = GetPoint(inA);
			Vec3 b = GetPoint(inB);
			return double((b - a).Length());
		}

		/// Recursively integrate one interval. Each child receives half of the
		/// local error budget, so summed leaf errors stay within inIntervalTolerance.
		/// Refinement stops when the coarse/fine discrepancy is within budget and
		/// the minimum inspection depth has been reached, or when float time can
		/// no longer be split.
		double					Refine(float inA, float inB, double inIntervalTolerance, int inDepth, double &ioError)
		{
			if (mBudgetFailed)
				return 0.0;

			float mid = float(0.5 * (double(inA) + double(inB)));
			bool can_split = mid > inA && mid < inB;

			double coarse = Chord(inA, inB);
			if (!can_split)
				return coarse;

			double fine = Chord(inA, mid) + Chord(mid, inB);
			double delta = fine - coarse;										// Always >= 0 by the triangle inequality
			if (inDepth >= cMinRefineDepth && delta <= inIntervalTolerance)
			{
				ioError += delta;
				return fine;
			}

			double child_tolerance = 0.5 * inIntervalTolerance;
			double left_error = 0.0, right_error = 0.0;
			double left = Refine(inA, mid, child_tolerance, inDepth + 1, left_error);
			double right = Refine(mid, inB, child_tolerance, inDepth + 1, right_error);
			if (mBudgetFailed)
				return 0.0;

			ioError += left_error + right_error;
			return left + right;
		}

		/// Build the sorted cumulative length table from all cached samples,
		/// refining to 1/cAccuracyFactor of inUserTolerance.
		/// Returns false when the evaluation budget was exhausted.
		bool					BuildTable(float inDuration, float inUserTolerance, double &outError)
		{
			mInternalTolerance = double(inUserTolerance) / double(cAccuracyFactor);

			// Float roundoff floor for a stationary point, relative to the curve's
			// size. Two cumulative lengths differing by no more than this are
			// treated as one stationary plateau during inversion; below a few ULPs
			// of position, float32 cannot separate real motion from matrix/SLERP
			// roundoff anyway.
			Vec3 start = GetPoint(0.0f);
			Vec3 end = GetPoint(inDuration);
			double scale = std::max(double(start.Length()), double(end.Length()));
			mNoiseFloor = cFloatNoise * std::max(1.0, scale);

			outError = 0.0;
			Refine(0.0f, inDuration, mInternalTolerance, 0, outError);
			if (mBudgetFailed)
				return false;

			Array<PointSample> samples = mCache;
			std::sort(samples.begin(), samples.end(),
					  [](const PointSample &inLHS, const PointSample &inRHS) { return inLHS.first < inRHS.first; });

			mTableTimes.reserve(samples.size());
			mTableLengths.reserve(samples.size());
			double cumulative = 0.0;
			for (size_t i = 0; i < samples.size(); ++i)
			{
				if (i > 0)
				{
					if (samples[i].first == samples[i - 1].first)
						continue;												// Same float time from overlapping dyadic intervals
					cumulative += double((samples[i].second - samples[i - 1].second).Length());
				}
				mTableTimes.push_back(samples[i].first);
				mTableLengths.push_back(cumulative);
			}
			return true;
		}

		/// Estimated cumulative length L(inTime), with inTime inside one table
		/// cell. The extra chord is bounded by the accepted error of that cell.
		double					LengthAt(float inTime)
		{
			size_t cell = FindCell(inTime);
			if (mTableTimes[cell] == inTime)
				return mTableLengths[cell];
			return mTableLengths[cell] + Chord(mTableTimes[cell], inTime);
		}

		/// Find the rightmost source time t for which L(t) == inTargetLength.
		/// Cumulative lengths within the relative float noise floor are treated as
		/// equal, so a stationary plateau (constant L up to roundoff) is crossed to
		/// its rightmost float time. outLengthAtTime receives L(t); returns false
		/// only on budget exhaustion.
		bool					SolveRightmost(double inTargetLength, float &outTime, double &outLengthAtTime)
		{
			// Rightmost table entry whose cumulative length is not genuinely above
			// the target; equal length stationary cells are all skipped.
			size_t k = 0;
			while (k + 1 < mTableTimes.size() && mTableLengths[k + 1] <= inTargetLength + mNoiseFloor)
				++k;

			float low = mTableTimes[k];
			float high = (k + 1 < mTableTimes.size())? mTableTimes[k + 1] : low;

			while (high > low)
			{
				float mid = float(0.5 * (double(low) + double(high)));
				if (!(mid > low && mid < high))
					break;														// Float precision exhausted

				// Incremental chord sum along the fixed left partition.
				double mid_length = LengthAt(low) + Chord(low, mid);
				if (mid_length <= inTargetLength + mNoiseFloor)
					low = mid;
				else
					high = mid;
			}

			// Choose the better end of the final bracket; ties prefer the later time.
			double length_at_low = LengthAt(low);
			double length_at_high = LengthAt(high);
			if (std::fabs(length_at_low - inTargetLength) < std::fabs(length_at_high - inTargetLength))
			{
				outTime = low;
				outLengthAtTime = length_at_low;
			}
			else
			{
				outTime = high;
				outLengthAtTime = length_at_high;
			}
			return !mBudgetFailed;
		}

	private:
		/// Index of the table cell containing inTime (mTableTimes[k] <= inTime <= mTableTimes[k+1]).
		size_t					FindCell(float inTime)
		{
			size_t low = 0, high = mTableTimes.size() - 1;
			while (low + 1 < high)
			{
				size_t mid = (low + high) / 2;
				if (mTableTimes[mid] <= inTime)
					low = mid;
				else
					high = mid;
			}
			return low;
		}

		static constexpr int	cMinRefineDepth = 8;							// At least 256 sub intervals before trusting a stationary verdict
		static constexpr double cFloatNoise = 32.0 * 1.1920928955078125e-7;		// ~32 float32 ULPs, relative to the curve's scale

		CurveEvaluator &		mEvaluator;
		uint					mMaxEvaluations;
		uint					mEvaluationCount = 0;
		bool					mBudgetFailed = false;
		double					mInternalTolerance = 0.0;
		double					mNoiseFloor = 0.0;
		Array<PointSample>		mCache;
		Array<float>			mTableTimes;
		Array<double>			mTableLengths;
	};

	/// Finite check for a Vec3 (rejects NaN and infinity).
	static bool				sIsFinite(Vec3Arg inV)
	{
		return isfinite(inV.GetX()) && isfinite(inV.GetY()) && isfinite(inV.GetZ());
	}

	/// Finite, unit length check for a quaternion.
	static bool				sIsFiniteUnit(const Quat &inQ)
	{
		Vec4 q = inQ.mValue;
		return isfinite(q.GetX()) && isfinite(q.GetY()) && isfinite(q.GetZ()) && isfinite(q.GetW()) && inQ.IsNormalized();
	}

	/// Validate all keyframes of one track: non empty, strictly increasing times
	/// in [0, inDuration], first key at 0, last key at inDuration, finite values,
	/// unit rotations.
	static bool				sValidateTrack(const SkeletalAnimation::KeyframeVector &inKeyframes, float inDuration)
	{
		if (inKeyframes.empty())
			return false;
		if (inKeyframes.front().mTime != 0.0f || inKeyframes.back().mTime != inDuration)
			return false;

		float previous = -1.0f;
		for (const SkeletalAnimation::Keyframe &key : inKeyframes)
		{
			if (!isfinite(key.mTime) || key.mTime < 0.0f || key.mTime > inDuration || key.mTime <= previous)
				return false;
			if (!sIsFiniteUnit(key.mRotation) || !sIsFinite(key.mTranslation))
				return false;
			previous = key.mTime;
		}
		return true;
	}
}

bool RetimingByJointArcLength(const SkeletalAnimation &inAnimation, const SkeletonPose &inRestPose,
							  const ArcLengthSettings &inSettings, ArcLengthResult &outResult)
{
	using namespace ArcLengthRetimerDetail;

	// --- Validate settings ---
	if (inSettings.mSampleCount < 2 || inSettings.mMaxEvaluations == 0
		|| !isfinite(inSettings.mOutputDuration) || inSettings.mOutputDuration <= 0.0f
		|| !isfinite(inSettings.mLengthTolerance) || inSettings.mLengthTolerance <= 0.0f
		|| !sIsFinite(inSettings.mLocalPoint))
		return false;

	const Skeleton *skeleton = inRestPose.GetSkeleton();
	if (skeleton == nullptr || skeleton->GetJointCount() == 0 || !skeleton->AreJointsCorrectlyOrdered())
		return false;

	int joint_count = skeleton->GetJointCount();
	if (inSettings.mJointIndex < 0 || inSettings.mJointIndex >= joint_count)
		return false;

	// The rest pose must carry one local state per skeleton joint.
	if (inRestPose.GetJointCount() != joint_count)
		return false;

	// --- Validate rest pose ---
	for (int j = 0; j < joint_count; ++j)
	{
		const SkeletalAnimation::JointState &state = inRestPose.GetJoint(j);
		if (!sIsFiniteUnit(state.mRotation) || !sIsFinite(state.mTranslation))
			return false;
	}

	// --- Validate clip ---
	float duration = inAnimation.GetDuration();
	if (!isfinite(duration) || duration <= 0.0f)
		return false;

	for (const SkeletalAnimation::AnimatedJoint &aj : inAnimation.GetAnimatedJoints())
		if (!sValidateTrack(aj.mKeyframes, duration))
			return false;

	// --- Bind tracks by skeleton name ---
	CurveEvaluator evaluator;
	if (!evaluator.Initialize(inAnimation, inRestPose, inSettings.mJointIndex, inSettings.mLocalPoint))
		return false;

	// All work happens on local objects; outResult is only touched after full success.
	ArcLengthIntegrator integrator(evaluator, inSettings.mMaxEvaluations);
	const double tolerance = double(inSettings.mLengthTolerance);
	const double tolerance_slack = 1.0e-9 * (1.0 + std::fabs(tolerance));			// Float roundoff slack on top of the requested tolerance

	// --- Build the adaptive cumulative length table over [0, D] ---
	double table_error = 0.0;
	if (!integrator.BuildTable(duration, inSettings.mLengthTolerance, table_error))
		return false;
	if (table_error > integrator.GetInternalTolerance() + tolerance_slack)
		return false;

	double total_length = integrator.GetTotalLength();

	const uint sample_count = inSettings.mSampleCount;
	Array<float> source_times;
	source_times.resize(sample_count);
	source_times[0] = 0.0f;
	source_times[sample_count - 1] = duration;

	if (total_length == 0.0)
	{
		// Completely stationary curve: uniform time mapping t = q * D.
		for (uint i = 1; i + 1 < sample_count; ++i)
			source_times[i] = float(i) / float(sample_count - 1) * duration;
	}
	else
	{
		// --- Invert L(t) = q * L(D), taking the rightmost solution ---
		for (uint i = 1; i + 1 < sample_count; ++i)
		{
			double target = double(i) / double(sample_count - 1) * total_length;

			float t;
			double length_at_t;
			if (!integrator.SolveRightmost(target, t, length_at_t))
				return false;

			// The cumulative length at this internal key must hit the target.
			if (std::fabs(length_at_t - target) > tolerance + tolerance_slack)
				return false;

			source_times[i] = t;
		}

		// The closure of the last interval must reproduce the total length.
		double closure = integrator.LengthAt(duration);
		if (std::fabs(closure - total_length) > tolerance_slack)
			return false;
	}

	if (integrator.BudgetExhausted())
		return false;

	// --- Build the output clip: one track per joint, in skeleton order ---
	Ref<SkeletalAnimation> output = new SkeletalAnimation;
	output->SetIsLooping(false);
	SkeletalAnimation::AnimatedJointVector &out_joints = output->GetAnimatedJoints();
	out_joints.resize(joint_count);
	for (int j = 0; j < joint_count; ++j)
		out_joints[j].mJointName = skeleton->GetJoint(j).mName;

	// Reusable scratch pose; EvaluatePose resets every joint to its rest state first.
	Array<Quat> pose_rot;
	Array<Vec3> pose_trans;
	for (uint i = 0; i < sample_count; ++i)
	{
		// Non looping evaluation on the closed interval [0, D], even when the
		// source clip is marked looping: at t == D this yields the end keys.
		evaluator.EvaluatePose(source_times[i], pose_rot, pose_trans);

		float output_time = float(i) * inSettings.mOutputDuration / float(sample_count - 1);
		if (i == sample_count - 1)
			output_time = inSettings.mOutputDuration;							// Guarantee the exact end time

		for (int j = 0; j < joint_count; ++j)
		{
			SkeletalAnimation::Keyframe key;
			key.mTime = output_time;
			key.mRotation = pose_rot[j];
			key.mTranslation = pose_trans[j];
			out_joints[j].mKeyframes.push_back(key);
		}
	}

	// --- Commit; outResult was deliberately left untouched until now ---
	outResult.mAnimation = output;
	outResult.mSourceTimes = std::move(source_times);
	outResult.mTotalLength = total_length;
	return true;
}

JPH_NAMESPACE_END
