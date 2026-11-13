// Standalone verification program for the arc length animation retimer.
// Build and run through: bash support/run.sh target

#include <Jolt/Jolt.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Jolt/Skeleton/SkeletalAnimation.h>
#include <Jolt/Skeleton/ArcLengthRetimer.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace JPH;

static void require(bool inOK, const char *inMessage)
{
	if (!inOK)
	{
		std::fprintf(stderr, "FAIL: %s\n", inMessage);
		std::exit(1);
	}
}

static bool close(float inA, float inB, float inEps)
{
	return std::abs(inA - inB) <= inEps;
}

static bool close(double inA, double inB, double inEps)
{
	return std::abs(inA - inB) <= inEps;
}

static bool close(Vec3Arg inA, Vec3Arg inB, float inEps)
{
	return (inA - inB).Length() <= inEps;
}

static bool close(QuatArg inA, QuatArg inB, float inEps)
{
	// Quaternion sign is irrelevant for the represented rotation.
	return std::abs(std::abs(inA.Dot(inB)) - 1.0f) <= inEps;
}

struct KeyDesc
{
	float					mTime;
	Quat					mRotation;
	Vec3					mTranslation;
};

static void addTrack(SkeletalAnimation &inClip, const char *inJointName, const std::vector<KeyDesc> &inKeys)
{
	SkeletalAnimation::AnimatedJoint track;
	track.mJointName = inJointName;
	for (const KeyDesc &desc : inKeys)
	{
		SkeletalAnimation::Keyframe key;
		key.mTime = desc.mTime;
		key.mRotation = desc.mRotation;
		key.mTranslation = desc.mTranslation;
		track.mKeyframes.push_back(key);
	}
	inClip.GetAnimatedJoints().push_back(track);
}

static Quat rotZ(float inAngle)
{
	return Quat::sRotation(Vec3::sAxisZ(), inAngle);
}

static Quat rotX(float inAngle)
{
	return Quat::sRotation(Vec3::sAxisX(), inAngle);
}

/// Independent reference: evaluate the measured point with the library's own
/// Sample on a pose that is reset to the rest local state every time. Only
/// valid for non looping clips (the retimer always evaluates non looping).
static Vec3 referencePoint(const SkeletalAnimation &inClip, const Skeleton *inSkeleton,
	const SkeletonPose::JointStateVector &inRest, int inJoint, Vec3Arg inLocalPoint, float inTime)
{
	SkeletonPose pose;
	pose.SetSkeleton(inSkeleton);
	pose.GetJoints() = inRest;
	inClip.Sample(inTime, pose);
	pose.CalculateJointMatrices();
	return pose.GetJointMatrix(inJoint) * inLocalPoint;
}

/// Independent high density chord integration of the same curve.
static double referenceLength(const SkeletalAnimation &inClip, const Skeleton *inSkeleton,
	const SkeletonPose::JointStateVector &inRest, int inJoint, Vec3Arg inLocalPoint,
	float inDuration, int inSteps, std::vector<double> *outCumulative = nullptr)
{
	Vec3 previous = referencePoint(inClip, inSkeleton, inRest, inJoint, inLocalPoint, 0.0f);
	double total = 0.0;
	if (outCumulative != nullptr)
	{
		outCumulative->clear();
		outCumulative->reserve(inSteps + 1);
		outCumulative->push_back(0.0);
	}
	for (int i = 1; i <= inSteps; ++i)
	{
		float t = inDuration * (float(i) / float(inSteps));
		Vec3 current = referencePoint(inClip, inSkeleton, inRest, inJoint, inLocalPoint, t);
		total += (current - previous).Length();
		previous = current;
		if (outCumulative != nullptr)
			outCumulative->push_back(total);
	}
	return total;
}

static ArcLengthSettings makeSettings(int inJoint, Vec3Arg inLocalPoint)
{
	ArcLengthSettings settings;
	settings.mJointIndex = inJoint;
	settings.mLocalPoint = inLocalPoint;
	settings.mSampleCount = 17;
	settings.mOutputDuration = 2.0f;
	settings.mLengthTolerance = 1e-4;
	settings.mMaxEvaluations = 200000;
	return settings;
}

/// Checks shared by every successful retiming: output structure, shared source
/// times across all joints (including a non measured branch) and endpoint
/// poses reproduced through real Sample + CalculateJointMatrices calls.
static void verifyOutput(const SkeletalAnimation &inSource, const Skeleton *inSkeleton,
	const SkeletonPose::JointStateVector &inRest, const ArcLengthSettings &inSettings,
	const ArcLengthResult &inResult, int inBranchJoint)
{
	require(inResult.mAnimation != nullptr, "output animation exists");
	require(!inResult.mAnimation->IsLooping(), "output animation is non looping");
	require((int)inResult.mSourceTimes.size() == inSettings.mSampleCount, "source time count");
	require(close(inResult.mSourceTimes.front(), 0.0f, 1e-6f), "first source time is 0");
	require(close(inResult.mSourceTimes.back(), inSource.GetDuration(), 1e-6f), "last source time is D");
	for (int i = 1; i < (int)inResult.mSourceTimes.size(); ++i)
		require(inResult.mSourceTimes[i] >= inResult.mSourceTimes[i - 1], "source times monotonic");

	const SkeletalAnimation &out_clip = *inResult.mAnimation;
	const auto &out_tracks = out_clip.GetAnimatedJoints();
	require((int)out_tracks.size() == inSkeleton->GetJointCount(), "one output track per joint");
	for (int j = 0; j < inSkeleton->GetJointCount(); ++j)
	{
		require(out_tracks[j].mJointName == inSkeleton->GetJoint(j).mName, "track names follow skeleton order");
		require((int)out_tracks[j].mKeyframes.size() == inSettings.mSampleCount, "key count per track");
		for (int i = 0; i < inSettings.mSampleCount; ++i)
		{
			float expected_time = inSettings.mOutputDuration * float(i) / float(inSettings.mSampleCount - 1);
			require(close(out_tracks[j].mKeyframes[i].mTime, expected_time, 1e-6f), "output key time grid");
			if (i > 0)
				require(out_tracks[j].mKeyframes[i].mTime > out_tracks[j].mKeyframes[i - 1].mTime, "output times increasing");
		}
	}

	const int n = inSettings.mSampleCount;
	for (int i = 0; i < n; ++i)
	{
		float output_time = inSettings.mOutputDuration * float(i) / float(n - 1);

		// Drive the produced clip through the real sampling pipeline.
		SkeletonPose out_pose;
		out_pose.SetSkeleton(inSkeleton);
		out_pose.GetJoints() = inRest;
		out_clip.Sample(output_time, out_pose);
		out_pose.CalculateJointMatrices();

		SkeletonPose ref_pose;
		ref_pose.SetSkeleton(inSkeleton);
		ref_pose.GetJoints() = inRest;
		inSource.Sample(inResult.mSourceTimes[i], ref_pose);
		ref_pose.CalculateJointMatrices();

		Vec3 produced = out_pose.GetJointMatrix(inSettings.mJointIndex) * inSettings.mLocalPoint;
		Vec3 expected = ref_pose.GetJointMatrix(inSettings.mJointIndex) * inSettings.mLocalPoint;
		require(close(produced, expected, 2e-4f), "measured point reproduced through output clip");

		// Every joint, including a branch that does not influence the measured
		// point, must have been sampled at the very same source time: compare
		// the full local rotation and translation produced by the real pipeline.
		for (int j = 0; j < inSkeleton->GetJointCount(); ++j)
		{
			require(close(out_pose.GetJoint(j).mTranslation, ref_pose.GetJoint(j).mTranslation, 2e-4f), "joint translation shares the source time");
			require(close(out_pose.GetJoint(j).mRotation, ref_pose.GetJoint(j).mRotation, 2e-4f), "joint rotation shares the source time");
		}

		// A branch that does not influence the measured point must still use
		// the very same source time (checked through its model matrix too).
		const Mat44 &branch_out = out_pose.GetJointMatrix(inBranchJoint);
		const Mat44 &branch_ref = ref_pose.GetJointMatrix(inBranchJoint);
		require(close(branch_out.GetTranslation(), branch_ref.GetTranslation(), 2e-4f), "branch shares the source time");
	}

	// Explicit endpoint checks.
	SkeletonPose end_pose;
	end_pose.SetSkeleton(inSkeleton);
	end_pose.GetJoints() = inRest;
	out_clip.Sample(inSettings.mOutputDuration, end_pose);
	end_pose.CalculateJointMatrices();
	Vec3 end_point = end_pose.GetJointMatrix(inSettings.mJointIndex) * inSettings.mLocalPoint;
	Vec3 expected_end = referencePoint(inSource, inSkeleton, inRest, inSettings.mJointIndex,
		inSettings.mLocalPoint, inSource.GetDuration());
	require(close(end_point, expected_end, 2e-4f), "output endpoint matches source endpoint");
}

int main()
{
	RegisterDefaultAllocator();

	constexpr float kDuration = 1.0f;
	constexpr int kSamples = 17;

	// ------------------------------------------------------------------
	// Case A: root does not move, a descendant arm swings. A sibling branch
	// with its own track verifies the shared time mapping, and a root offset
	// on the rest pose must be ignored by the measured curve.
	// ------------------------------------------------------------------
	{
		Ref<Skeleton> skeleton = new Skeleton;
		skeleton->AddJoint("root", -1);
		skeleton->AddJoint("arm", 0);
		skeleton->AddJoint("branch", 0);

		SkeletonPose rest;
		rest.SetSkeleton(skeleton);
		rest.SetRootOffset(RVec3(10.0f, 20.0f, 30.0f));

		Ref<SkeletalAnimation> clip = new SkeletalAnimation;
		clip->SetIsLooping(false);
		addTrack(*clip, "root", {
			{ 0.0f, Quat::sIdentity(), Vec3::sZero() },
			{ kDuration, Quat::sIdentity(), Vec3::sZero() },
		});
		// Two different angular speeds, so the retiming must be non uniform.
		addTrack(*clip, "arm", {
			{ 0.0f, rotZ(0.0f), Vec3::sZero() },
			{ 0.5f, rotZ(JPH_PI / 8.0f), Vec3::sZero() },
			{ kDuration, rotZ(JPH_PI / 2.0f), Vec3::sZero() },
		});
		addTrack(*clip, "branch", {
			{ 0.0f, rotX(0.0f), Vec3(0.0f, 0.0f, 0.5f) },
			{ 0.35f, rotX(JPH_PI / 3.0f), Vec3(0.0f, 0.0f, 0.5f) },
			{ kDuration, rotX(JPH_PI), Vec3(0.0f, 0.0f, 0.5f) },
		});

		Vec3 local_point(2.0f, 0.0f, 0.0f);
		ArcLengthSettings settings = makeSettings(1, local_point);
		ArcLengthResult result;
		require(RetimingByJointArcLength(*clip, rest, settings, result), "case A retiming succeeds");

		const double analytical = 2.0 * (JPH_PI / 2.0);
		require(close(result.mTotalLength, analytical, 1e-4), "case A analytical quarter circle length");

		std::vector<double> cumulative;
		double dense = referenceLength(*clip, skeleton, rest.GetJoints(), 1, local_point, kDuration, 40000, &cumulative);
		require(close(result.mTotalLength, dense, 2e-4), "case A matches independent integration");

		// Non uniform timing: the second half rotates three times as far.
		require(result.mSourceTimes[8] > 0.5f, "case A source times non uniform");
		// Residual of every internal key against the dense cumulative table,
		// linearly interpolated between its 40000 grid entries.
		for (int i = 1; i < kSamples - 1; ++i)
		{
			double grid_pos = double(result.mSourceTimes[i] / kDuration) * 40000.0;
			size_t k = (size_t)grid_pos;
			double a = cumulative[k];
			double b = cumulative[std::min(k + 1, cumulative.size() - 1)];
			double length_at = a + (b - a) * (grid_pos - double(k));
			double target = result.mTotalLength * (double(i) / double(kSamples - 1));
			require(std::abs(length_at - target) <= 3e-4, "case A accumulated length residual");
		}

		verifyOutput(*clip, skeleton, rest.GetJoints(), settings, result, 2);
		require(rest.GetRootOffset() == RVec3(10.0f, 20.0f, 30.0f), "rest pose root offset untouched");
		// Rest pose local states must not be mutated by the retimer.
		for (int j = 0; j < skeleton->GetJointCount(); ++j)
		{
			require(close(rest.GetJoint(j).mTranslation, Vec3::sZero(), 0.0f), "rest translation untouched");
			require(close(rest.GetJoint(j).mRotation, Quat::sIdentity(), 0.0f), "rest rotation untouched");
		}
		require(!clip->IsLooping() && clip->GetDuration() == kDuration, "source clip untouched");
		require(clip->GetAnimatedJoints().size() == 3, "source track count untouched");
		std::puts("case A ok: fixed root, swinging arm, branch and ignored root offset");
	}

	// ------------------------------------------------------------------
	// Case B: eight coaxial helper bones each rotating 90 degrees. The off
	// axis tip point completes two full turns: analytical length 4 * pi.
	// Start, midpoint and end points coincide but the length is non zero.
	// ------------------------------------------------------------------
	{
		constexpr int kBoneCount = 8;
		Ref<Skeleton> skeleton = new Skeleton;
		skeleton->AddJoint("root", -1);
		for (int i = 0; i < kBoneCount; ++i)
			skeleton->AddJoint("bone" + std::to_string(i + 1), i);

		SkeletonPose rest;
		rest.SetSkeleton(skeleton);
		for (int i = 1; i <= kBoneCount; ++i)
			rest.GetJoint(i).mTranslation = Vec3(0.0f, 0.0f, 0.5f); // non zero axial spacing

		Ref<SkeletalAnimation> clip = new SkeletalAnimation;
		clip->SetIsLooping(false);
		for (int i = 1; i <= kBoneCount; ++i)
			addTrack(*clip, ("bone" + std::to_string(i)).c_str(), {
				{ 0.0f, rotZ(0.0f), Vec3(0.0f, 0.0f, 0.5f) },
				{ kDuration, rotZ(JPH_PI / 2.0f), Vec3(0.0f, 0.0f, 0.5f) },
			});

		Vec3 local_point(1.0f, 0.0f, 0.0f);
		ArcLengthSettings settings = makeSettings(kBoneCount, local_point);
		ArcLengthResult result;
		require(RetimingByJointArcLength(*clip, rest, settings, result), "case B retiming succeeds");
		require(close(result.mTotalLength, 4.0 * JPH_PI, 1e-4), "case B two turn analytical length 4pi");

		Vec3 p0 = referencePoint(*clip, skeleton, rest.GetJoints(), kBoneCount, local_point, 0.0f);
		Vec3 pm = referencePoint(*clip, skeleton, rest.GetJoints(), kBoneCount, local_point, 0.5f * kDuration);
		Vec3 p1 = referencePoint(*clip, skeleton, rest.GetJoints(), kBoneCount, local_point, kDuration);
		require(close(p0, pm, 1e-5f) && close(pm, p1, 1e-5f), "case B start mid end coincide");
		require(result.mTotalLength > 12.0, "case B coincident endpoints still give non zero length");

		std::vector<double> cumulative;
		double dense = referenceLength(*clip, skeleton, rest.GetJoints(), kBoneCount, local_point, kDuration, 80000, &cumulative);
		require(close(result.mTotalLength, dense, 2e-4), "case B matches independent integration");
		for (int i = 1; i < kSamples - 1; ++i)
		{
			double grid_pos = double(result.mSourceTimes[i] / kDuration) * 80000.0;
			size_t k = (size_t)grid_pos;
			double a = cumulative[k];
			double b = cumulative[std::min(k + 1, cumulative.size() - 1)];
			double length_at = a + (b - a) * (grid_pos - double(k));
			double target = result.mTotalLength * (double(i) / double(kSamples - 1));
			require(std::abs(length_at - target) <= 3e-4, "case B accumulated length residual");
		}

		// Constant angular speed gives an almost uniform source time mapping.
		for (int i = 0; i < kSamples; ++i)
		{
			float expected = kDuration * float(i) / float(kSamples - 1);
			require(close(result.mSourceTimes[i], expected, 2e-3f), "case B uniform in time");
		}

		verifyOutput(*clip, skeleton, rest.GetJoints(), settings, result, 0);
		std::puts("case B ok: eight bones, two turns, 4pi with coincident endpoints");
	}

	// ------------------------------------------------------------------
	// Case C: tracks with staggered key times and a genuine full stop.
	// ------------------------------------------------------------------
	{
		Ref<Skeleton> skeleton = new Skeleton;
		skeleton->AddJoint("root", -1);
		skeleton->AddJoint("arm", 0);

		SkeletonPose rest;
		rest.SetSkeleton(skeleton);

		Ref<SkeletalAnimation> clip = new SkeletalAnimation;
		clip->SetIsLooping(false);
		// Root pauses on [0.3, 0.6].
		addTrack(*clip, "root", {
			{ 0.0f, Quat::sIdentity(), Vec3(0.0f, 0.0f, 0.0f) },
			{ 0.3f, Quat::sIdentity(), Vec3(1.0f, 0.0f, 0.0f) },
			{ 0.6f, Quat::sIdentity(), Vec3(1.0f, 0.0f, 0.0f) },
			{ kDuration, Quat::sIdentity(), Vec3(2.0f, 0.0f, 0.0f) },
		});
		// Arm has its own, staggered key times and holds still during the root pause.
		addTrack(*clip, "arm", {
			{ 0.0f, rotZ(0.0f), Vec3::sZero() },
			{ 0.2f, rotZ(JPH_PI / 4.0f), Vec3::sZero() },
			{ 0.3f, rotZ(JPH_PI / 2.0f), Vec3::sZero() },
			{ 0.6f, rotZ(JPH_PI / 2.0f), Vec3::sZero() },
			{ kDuration, rotZ(JPH_PI), Vec3::sZero() },
		});

		Vec3 local_point(0.5f, 0.0f, 0.0f);
		ArcLengthSettings settings = makeSettings(1, local_point);
		ArcLengthResult result;
		require(RetimingByJointArcLength(*clip, rest, settings, result), "case C retiming succeeds");
		require(result.mTotalLength > 0.0, "case C has motion");

		std::vector<double> cumulative;
		double dense = referenceLength(*clip, skeleton, rest.GetJoints(), 1, local_point, kDuration, 60000, &cumulative);
		require(close(result.mTotalLength, dense, 3e-4), "case C matches independent integration");
		for (int i = 1; i < kSamples - 1; ++i)
		{
			double grid_pos = double(result.mSourceTimes[i] / kDuration) * 60000.0;
			size_t k = (size_t)grid_pos;
			double a = cumulative[k];
			double b = cumulative[std::min(k + 1, cumulative.size() - 1)];
			double length_at = a + (b - a) * (grid_pos - double(k));
			double target = result.mTotalLength * (double(i) / double(kSamples - 1));
			require(std::abs(length_at - target) <= 4e-4, "case C accumulated length residual");
		}
		verifyOutput(*clip, skeleton, rest.GetJoints(), settings, result, 0);

		// Dedicated plateau clip: pure root translation with a stop, where the
		// half length target lands exactly inside the stationary range. The
		// rightmost solution must be the end of the pause (t = 0.6).
		Ref<SkeletalAnimation> plateau = new SkeletalAnimation;
		plateau->SetIsLooping(false);
		addTrack(*plateau, "root", {
			{ 0.0f, Quat::sIdentity(), Vec3(0.0f, 0.0f, 0.0f) },
			{ 0.3f, Quat::sIdentity(), Vec3(1.0f, 0.0f, 0.0f) },
			{ 0.6f, Quat::sIdentity(), Vec3(1.0f, 0.0f, 0.0f) },
			{ kDuration, Quat::sIdentity(), Vec3(2.0f, 0.0f, 0.0f) },
		});
		ArcLengthSettings plateau_settings = makeSettings(0, Vec3::sZero());
		ArcLengthResult plateau_result;
		require(RetimingByJointArcLength(*plateau, rest, plateau_settings, plateau_result), "plateau retiming succeeds");
		require(close(plateau_result.mTotalLength, 2.0, 1e-4), "plateau total length");
		require(close(plateau_result.mSourceTimes[kSamples / 2], 0.6f, 1e-5f), "rightmost solution jumps to pause end");
		std::puts("case C ok: staggered keys, real pause and rightmost plateau solution");
	}

	// ------------------------------------------------------------------
	// Case D: a joint without a track keeps its rest local offset.
	// ------------------------------------------------------------------
	{
		Ref<Skeleton> skeleton = new Skeleton;
		skeleton->AddJoint("root", -1);
		skeleton->AddJoint("arm", 0);

		SkeletonPose rest;
		rest.SetSkeleton(skeleton);
		rest.GetJoint(1).mTranslation = Vec3(0.0f, 1.0f, 0.0f); // static local offset, no arm track

		Ref<SkeletalAnimation> clip = new SkeletalAnimation;
		clip->SetIsLooping(false);
		addTrack(*clip, "root", {
			{ 0.0f, rotZ(0.0f), Vec3::sZero() },
			{ kDuration, rotZ(JPH_PI / 2.0f), Vec3::sZero() },
		});

		ArcLengthSettings settings = makeSettings(1, Vec3::sZero());
		ArcLengthResult result;
		require(RetimingByJointArcLength(*clip, rest, settings, result), "case D retiming succeeds");
		require(close(result.mTotalLength, JPH_PI / 2.0, 1e-4), "case D rest offset traces quarter circle");

		const auto &out_tracks = result.mAnimation->GetAnimatedJoints();
		require(out_tracks.size() == 2, "case D outputs all joints");
		for (const auto &key : out_tracks[1].mKeyframes)
		{
			require(close(key.mTranslation, Vec3(0.0f, 1.0f, 0.0f), 1e-6f), "case D static rest offset preserved");
			require(close(key.mRotation, Quat::sIdentity(), 1e-6f), "case D static rest rotation preserved");
		}
		verifyOutput(*clip, skeleton, rest.GetJoints(), settings, result, 1);
		std::puts("case D ok: missing track keeps the rest local state");
	}

	// ------------------------------------------------------------------
	// Case E: looping input is still evaluated on the closed interval [0, D].
	// ------------------------------------------------------------------
	{
		Ref<Skeleton> skeleton = new Skeleton;
		skeleton->AddJoint("root", -1);
		skeleton->AddJoint("arm", 0);

		SkeletonPose rest;
		rest.SetSkeleton(skeleton);

		Ref<SkeletalAnimation> clip = new SkeletalAnimation;
		clip->SetIsLooping(true); // deliberately looping
		addTrack(*clip, "root", {
			{ 0.0f, Quat::sIdentity(), Vec3(0.0f, 0.0f, 0.0f) },
			{ kDuration, Quat::sIdentity(), Vec3(5.0f, 0.0f, 0.0f) },
		});
		addTrack(*clip, "arm", {
			{ 0.0f, rotZ(0.0f), Vec3::sZero() },
			{ kDuration, rotZ(JPH_PI / 2.0f), Vec3::sZero() },
		});

		Vec3 local_point(1.0f, 0.0f, 0.0f);
		ArcLengthSettings settings = makeSettings(1, local_point);
		ArcLengthResult result;
		require(RetimingByJointArcLength(*clip, rest, settings, result), "case E retiming succeeds");
		require(clip->IsLooping(), "case E source looping flag untouched");
		require(!result.mAnimation->IsLooping(), "case E output is non looping");

		// Non wrapped endpoint: root at x=5, arm rotated 90 degrees.
		SkeletonPose end_pose;
		end_pose.SetSkeleton(skeleton);
		end_pose.GetJoints() = rest.GetJoints();
		result.mAnimation->Sample(settings.mOutputDuration, end_pose);
		end_pose.CalculateJointMatrices();
		Vec3 end_point = end_pose.GetJointMatrix(1) * local_point;
		require(close(end_point, Vec3(5.0f, 1.0f, 0.0f), 2e-4f), "case E closed interval endpoint, no wrap");

		// The library's looping sample at D wraps to the first key instead.
		SkeletonPose wrapped_pose;
		wrapped_pose.SetSkeleton(skeleton);
		wrapped_pose.GetJoints() = rest.GetJoints();
		clip->Sample(kDuration, wrapped_pose);
		wrapped_pose.CalculateJointMatrices();
		Vec3 wrapped_point = wrapped_pose.GetJointMatrix(1) * local_point;
		require(close(wrapped_point, Vec3(1.0f, 0.0f, 0.0f), 2e-4f), "case E confirms looping source would wrap");
		require(!close(end_point, wrapped_point, 1e-3f), "case E endpoint differs from wrapped pose");
		std::puts("case E ok: looping input retimed on the closed interval");
	}

	// ------------------------------------------------------------------
	// Case F: quaternion sign inversion, fully stationary clip and an
	// exhausted evaluation budget.
	// ------------------------------------------------------------------
	{
		Ref<Skeleton> skeleton = new Skeleton;
		skeleton->AddJoint("root", -1);
		skeleton->AddJoint("arm", 0);

		SkeletonPose rest;
		rest.SetSkeleton(skeleton);

		// Sign inversion: -identity is the same rotation as identity.
		Ref<SkeletalAnimation> sign_clip = new SkeletalAnimation;
		sign_clip->SetIsLooping(false);
		addTrack(*sign_clip, "arm", {
			{ 0.0f, Quat::sIdentity(), Vec3::sZero() },
			{ kDuration, Quat(0.0f, 0.0f, 0.0f, -1.0f), Vec3::sZero() },
		});
		ArcLengthSettings settings = makeSettings(1, Vec3(1.0f, 0.0f, 0.0f));
		ArcLengthResult result;
		require(RetimingByJointArcLength(*sign_clip, rest, settings, result), "sign inversion accepted");
		require(close(result.mTotalLength, 0.0, 1e-9), "sign inversion produces no motion");
		for (int i = 0; i < kSamples; ++i)
		{
			float expected = kDuration * float(i) / float(kSamples - 1);
			require(close(result.mSourceTimes[i], expected, 1e-6f), "zero length uses q * D");
		}

		// Fully stationary clip with ordinary keys. Identity rotations keep the
		// point exactly fixed (SLERP of an identical quaternion normalizes back
		// to itself); the constant translations exercise both channels.
		Ref<SkeletalAnimation> still_clip = new SkeletalAnimation;
		still_clip->SetIsLooping(false);
		addTrack(*still_clip, "root", {
			{ 0.0f, Quat::sIdentity(), Vec3(1.0f, 2.0f, 3.0f) },
			{ kDuration, Quat::sIdentity(), Vec3(1.0f, 2.0f, 3.0f) },
		});
		addTrack(*still_clip, "arm", {
			{ 0.0f, Quat::sIdentity(), Vec3(0.0f, 0.0f, 0.25f) },
			{ kDuration, Quat::sIdentity(), Vec3(0.0f, 0.0f, 0.25f) },
		});
		ArcLengthResult still_result;
		require(RetimingByJointArcLength(*still_clip, rest, settings, still_result), "stationary clip accepted");
		require(close(still_result.mTotalLength, 0.0, 1e-9), "stationary clip zero length");
		for (int i = 0; i < kSamples; ++i)
		{
			float expected = kDuration * float(i) / float(kSamples - 1);
			require(close(still_result.mSourceTimes[i], expected, 1e-6f), "stationary source times uniform");
		}

		// Budget exhaustion on the two turn style clip: failure, not an
		// approximate result, and out is left untouched.
		Ref<Skeleton> chain = new Skeleton;
		chain->AddJoint("root", -1);
		for (int i = 0; i < 8; ++i)
			chain->AddJoint("bone" + std::to_string(i + 1), i);
		SkeletonPose chain_rest;
		chain_rest.SetSkeleton(chain);
		for (int i = 1; i <= 8; ++i)
			chain_rest.GetJoint(i).mTranslation = Vec3(0.0f, 0.0f, 0.5f);
		Ref<SkeletalAnimation> chain_clip = new SkeletalAnimation;
		chain_clip->SetIsLooping(false);
		for (int i = 1; i <= 8; ++i)
			addTrack(*chain_clip, ("bone" + std::to_string(i)).c_str(), {
				{ 0.0f, rotZ(0.0f), Vec3(0.0f, 0.0f, 0.5f) },
				{ kDuration, rotZ(JPH_PI / 2.0f), Vec3(0.0f, 0.0f, 0.5f) },
			});
		ArcLengthSettings tight = makeSettings(8, Vec3(1.0f, 0.0f, 0.0f));
		tight.mMaxEvaluations = 3;
		ArcLengthResult guarded;
		guarded.mTotalLength = -1234.5;
		guarded.mSourceTimes.push_back(42.0f);
		require(!RetimingByJointArcLength(*chain_clip, chain_rest, tight, guarded), "tiny budget rejected");
		require(close(guarded.mTotalLength, -1234.5, 1e-9), "failed call leaves result length untouched");
		require(guarded.mSourceTimes.size() == 1 && close(guarded.mSourceTimes[0], 42.0f, 1e-6f), "failed call leaves result times untouched");
		require(guarded.mAnimation == nullptr, "failed call leaves result animation untouched");
		std::puts("case F ok: sign inversion, stationary clip and budget exhaustion");
	}

	// ------------------------------------------------------------------
	// Rejection cases: every documented invalid input returns false and the
	// caller supplied result is not modified.
	// ------------------------------------------------------------------
	{
		Ref<Skeleton> skeleton = new Skeleton;
		skeleton->AddJoint("root", -1);
		skeleton->AddJoint("arm", 0);
		SkeletonPose rest;
		rest.SetSkeleton(skeleton);

		auto valid_clip = []() {
			Ref<SkeletalAnimation> clip = new SkeletalAnimation;
			clip->SetIsLooping(false);
			addTrack(*clip, "root", {
				{ 0.0f, Quat::sIdentity(), Vec3::sZero() },
				{ 1.0f, Quat::sIdentity(), Vec3::sZero() },
			});
			addTrack(*clip, "arm", {
				{ 0.0f, Quat::sIdentity(), Vec3::sZero() },
				{ 1.0f, Quat::sIdentity(), Vec3::sZero() },
			});
			return clip;
		};

		auto check_reject = [&](const SkeletalAnimation *inClip, const SkeletonPose &inRest,
			const ArcLengthSettings &inSettings, const char *inName) {
			ArcLengthResult sentinel;
			sentinel.mTotalLength = -999.0;
			sentinel.mSourceTimes.push_back(7.0f);
			bool ok = RetimingByJointArcLength(*inClip, inRest, inSettings, sentinel);
			require(!ok, inName);
			require(close(sentinel.mTotalLength, -999.0, 1e-9), "rejected call leaves out untouched");
			require(sentinel.mSourceTimes.size() == 1 && close(sentinel.mSourceTimes[0], 7.0f, 1e-6f), "rejected out times untouched");
		};

		Ref<SkeletalAnimation> clip = valid_clip();
		ArcLengthSettings good = makeSettings(1, Vec3(1.0f, 0.0f, 0.0f));

		// Empty track.
		Ref<SkeletalAnimation> empty_track = new SkeletalAnimation;
		empty_track->SetIsLooping(false);
		empty_track->GetAnimatedJoints().resize(1);
		empty_track->GetAnimatedJoints()[0].mJointName = "root";
		check_reject(empty_track, rest, good, "empty track rejected");

		// Unknown joint name.
		Ref<SkeletalAnimation> unknown = valid_clip();
		unknown->GetAnimatedJoints()[1].mJointName = "ghost";
		check_reject(unknown, rest, good, "unknown joint name rejected");

		// Duplicate track for one joint.
		Ref<SkeletalAnimation> duplicate = new SkeletalAnimation;
		duplicate->SetIsLooping(false);
		addTrack(*duplicate, "root", { { 0.0f, Quat::sIdentity(), Vec3::sZero() }, { 1.0f, Quat::sIdentity(), Vec3::sZero() } });
		addTrack(*duplicate, "root", { { 0.0f, Quat::sIdentity(), Vec3::sZero() }, { 1.0f, Quat::sIdentity(), Vec3::sZero() } });
		check_reject(duplicate, rest, good, "duplicate track rejected");

		// Duplicate skeleton joint names.
		Ref<Skeleton> dup_skeleton = new Skeleton;
		dup_skeleton->AddJoint("root", -1);
		dup_skeleton->AddJoint("root", 0);
		SkeletonPose dup_rest;
		dup_rest.SetSkeleton(dup_skeleton);
		check_reject(clip, dup_rest, good, "duplicate skeleton name rejected");

		// Pose without a skeleton.
		SkeletonPose no_skeleton_pose;
		check_reject(clip, no_skeleton_pose, good, "missing skeleton rejected");

		// Invalid parent ordering (parent index points to a later joint).
		Ref<Skeleton> bad_order = new Skeleton;
		bad_order->AddJoint("a", -1);
		bad_order->AddJoint("b", -1);
		bad_order->GetJoint(0).mParentJointIndex = 1;
		SkeletonPose bad_order_rest;
		bad_order_rest.SetSkeleton(bad_order);
		check_reject(clip, bad_order_rest, good, "invalid parent ordering rejected");

		// Zero duration clip (single key at t = 0).
		Ref<SkeletalAnimation> zero_d = new SkeletalAnimation;
		zero_d->SetIsLooping(false);
		addTrack(*zero_d, "root", { { 0.0f, Quat::sIdentity(), Vec3::sZero() } });
		check_reject(zero_d, rest, good, "zero duration rejected");

		// Out of range joint index.
		ArcLengthSettings bad_joint = good;
		bad_joint.mJointIndex = 2;
		check_reject(clip, rest, bad_joint, "joint index out of range rejected");
		bad_joint = good;
		bad_joint.mJointIndex = -1;
		check_reject(clip, rest, bad_joint, "negative joint index rejected");

		// Non finite local point.
		ArcLengthSettings nonfinite = good;
		nonfinite.mLocalPoint = Vec3(std::numeric_limits<float>::infinity(), 0.0f, 0.0f);
		check_reject(clip, rest, nonfinite, "non finite local point rejected");

		// Non unit rotation key.
		Ref<SkeletalAnimation> non_unit = valid_clip();
		non_unit->GetAnimatedJoints()[1].mKeyframes[1].mRotation = Quat(1.0f, 0.0f, 0.0f, 1.0f);
		check_reject(non_unit, rest, good, "non unit rotation rejected");

		// Non finite translation key.
		Ref<SkeletalAnimation> nan_clip = valid_clip();
		nan_clip->GetAnimatedJoints()[0].mKeyframes[0].mTranslation = Vec3(std::nanf(""), 0.0f, 0.0f);
		check_reject(nan_clip, rest, good, "non finite translation rejected");

		// Settings errors.
		ArcLengthSettings s = good;
		s.mSampleCount = 1;
		check_reject(clip, rest, s, "sample count < 2 rejected");
		s = good;
		s.mOutputDuration = 0.0f;
		check_reject(clip, rest, s, "zero output duration rejected");
		s = good;
		s.mOutputDuration = -1.0f;
		check_reject(clip, rest, s, "negative output duration rejected");
		s = good;
		s.mLengthTolerance = 0.0;
		check_reject(clip, rest, s, "zero tolerance rejected");
		s = good;
		s.mLengthTolerance = -1e-4;
		check_reject(clip, rest, s, "negative tolerance rejected");
		s = good;
		s.mMaxEvaluations = 0;
		check_reject(clip, rest, s, "zero budget rejected");

		// A valid call on the same data still works.
		ArcLengthResult valid_result;
		require(RetimingByJointArcLength(*clip, rest, good, valid_result), "valid call after rejections works");
		require(valid_result.mAnimation != nullptr && valid_result.mTotalLength < 1e-6, "valid call data intact");
		std::puts("rejection cases ok: all invalid inputs refused, out untouched");
	}

	// ------------------------------------------------------------------
	// Case G: track order deliberately differs from skeleton joint order.
	// Track names must still resolve through the skeleton.
	// ------------------------------------------------------------------
	{
		Ref<Skeleton> skeleton = new Skeleton;
		skeleton->AddJoint("root", -1);
		skeleton->AddJoint("arm", 0);
		skeleton->AddJoint("twist", 1);

		SkeletonPose rest;
		rest.SetSkeleton(skeleton);

		Ref<SkeletalAnimation> clip = new SkeletalAnimation;
		clip->SetIsLooping(false);
		// Inserted in reverse skeleton order: twist, arm, root.
		addTrack(*clip, "twist", {
			{ 0.0f, rotZ(0.0f), Vec3::sZero() },
			{ kDuration, rotZ(JPH_PI / 4.0f), Vec3::sZero() },
		});
		addTrack(*clip, "arm", {
			{ 0.0f, rotZ(0.0f), Vec3::sZero() },
			{ kDuration, rotZ(JPH_PI / 2.0f), Vec3::sZero() },
		});
		addTrack(*clip, "root", {
			{ 0.0f, Quat::sIdentity(), Vec3::sZero() },
			{ kDuration, Quat::sIdentity(), Vec3::sZero() },
		});

		Vec3 local_point(1.5f, 0.0f, 0.0f);
		ArcLengthSettings settings = makeSettings(2, local_point);
		ArcLengthResult result;
		require(RetimingByJointArcLength(*clip, rest, settings, result), "case G shuffled track order accepted");
		require((int)result.mAnimation->GetAnimatedJoints().size() == 3, "case G three output tracks");
		// Output tracks must follow skeleton order regardless of input order.
		require(result.mAnimation->GetAnimatedJoints()[0].mJointName == "root", "case G output order root");
		require(result.mAnimation->GetAnimatedJoints()[1].mJointName == "arm", "case G output order arm");
		require(result.mAnimation->GetAnimatedJoints()[2].mJointName == "twist", "case G output order twist");
		verifyOutput(*clip, skeleton, rest.GetJoints(), settings, result, 2);
		std::puts("case G ok: shuffled input track order resolved by skeleton names");
	}

	std::puts("JOLT_ARCLENGTH_OK");
	return 0;
}
