// Standalone verification for RetimingByJointArcLength.
// Build and run with: bash support/run.sh target
//
// The program generates its own skeletons and clips, runs the offline retimer,
// and checks the results against analytic arc lengths and an independent
// high density integrator built on the real SkeletalAnimation::Sample /
// SkeletonPose::CalculateJointMatrices code path.

#include <Jolt/Jolt.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Jolt/Skeleton/SkeletalAnimation.h>
#include <Jolt/Skeleton/ArcLengthRetimer.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <string>

using namespace JPH;

static void Fail(const char *inMessage)
{
	std::fprintf(stderr, "FAIL: %s\n", inMessage);
	std::exit(1);
}

static void Require(bool inCondition, const char *inMessage)
{
	if (!inCondition)
		Fail(inMessage);
}

static bool Near(double inA, double inB, double inTolerance)
{
	return std::fabs(inA - inB) <= inTolerance;
}

static bool VecNear(Vec3Arg inA, Vec3Arg inB, float inTolerance)
{
	return (inA - inB).Length() <= inTolerance;
}

// ---------------------------------------------------------------------------
// Clip construction helpers
// ---------------------------------------------------------------------------

using Keyframe = SkeletalAnimation::Keyframe;
using AnimatedJoint = SkeletalAnimation::AnimatedJoint;

static Keyframe MakeKey(float inTime, QuatArg inRotation, Vec3Arg inTranslation)
{
	Keyframe key;
	key.mTime = inTime;
	key.mRotation = inRotation;
	key.mTranslation = inTranslation;
	return key;
}

static AnimatedJoint MakeTrack(const std::string &inName, std::initializer_list<Keyframe> inKeys)
{
	AnimatedJoint joint;
	joint.mJointName = inName;
	for (const Keyframe &key : inKeys)
		joint.mKeyframes.push_back(key);
	return joint;
}

// A non looping copy of a clip used as an independent reference for the closed
// interval [0, D] (a looping source clip wraps at D when sampled directly).
static Ref<SkeletalAnimation> MakeNonLoopingCopy(const SkeletalAnimation &inClip)
{
	Ref<SkeletalAnimation> copy = new SkeletalAnimation(inClip);
	copy->SetIsLooping(false);
	return copy;
}

// ---------------------------------------------------------------------------
// Independent high density integrator on the real library sampling path.
// ---------------------------------------------------------------------------

class IndependentArcLength
{
public:
	IndependentArcLength(const SkeletalAnimation &inNonLoopingClip, const Skeleton *inSkeleton,
						 int inJointIndex, Vec3Arg inLocalPoint, float inDuration, int inSegments) :
		mClip(inNonLoopingClip), mJointIndex(inJointIndex), mLocalPoint(inLocalPoint),
		mDuration(inDuration), mSegments(inSegments)
	{
		mPose.SetSkeleton(inSkeleton);
		mCumulative.resize(inSegments + 1);
		mCumulative[0] = 0.0;

		Vec3 previous = PointAt(0.0f);
		for (int i = 1; i <= inSegments; ++i)
		{
			Vec3 current = PointAt(float(i) * inDuration / float(inSegments));
			mCumulative[i] = mCumulative[i - 1] + double((current - previous).Length());
			previous = current;
		}
	}

	double						Total() const							{ return mCumulative.back(); }

	// Cumulative arc length at an arbitrary time, linearly interpolated on the
	// dense cumulative length table.
	double						LengthAt(float inTime) const
	{
		double u = double(inTime) / double(mDuration) * double(mSegments);
		int i = int(u);
		if (i >= mSegments)
			return mCumulative[mSegments];
		double fraction = u - double(i);
		return mCumulative[i] + fraction * (mCumulative[i + 1] - mCumulative[i]);
	}

	Vec3						PointAt(float inTime)
	{
		mClip.Sample(inTime, mPose);
		mPose.CalculateJointMatrices();
		return mPose.GetJointMatrix(mJointIndex) * mLocalPoint;
	}

private:
	const SkeletalAnimation &	mClip;
	SkeletonPose				mPose;
	int							mJointIndex;
	Vec3						mLocalPoint;
	float						mDuration;
	int							mSegments;
	Array<double>				mCumulative;
};

// ---------------------------------------------------------------------------
// Shared checks for a successful retiming run
// ---------------------------------------------------------------------------

static constexpr float cTolerance = 1.0e-4f;
static constexpr uint cMaxEvaluations = 200000;
static constexpr uint cSampleCount = 17;
static constexpr float cOutputDuration = 2.0f;

static ArcLengthSettings MakeSettings(int inJointIndex, Vec3Arg inLocalPoint)
{
	ArcLengthSettings settings;
	settings.mJointIndex = inJointIndex;
	settings.mLocalPoint = inLocalPoint;
	settings.mSampleCount = cSampleCount;
	settings.mOutputDuration = cOutputDuration;
	settings.mLengthTolerance = cTolerance;
	settings.mMaxEvaluations = cMaxEvaluations;
	return settings;
}

static void CheckOutputStructure(const ArcLengthResult &inResult, const Skeleton *inSkeleton, float inDuration)
{
	Require(inResult.mAnimation != nullptr, "output animation missing");
	Require(!inResult.mAnimation->IsLooping(), "output animation must be non looping");
	Require(inResult.mSourceTimes.size() == cSampleCount, "source time count");
	Require(inResult.mAnimation->GetAnimatedJoints().size() == (size_t)inSkeleton->GetJointCount(), "one track per joint");

	for (int j = 0; j < inSkeleton->GetJointCount(); ++j)
	{
		const AnimatedJoint &track = inResult.mAnimation->GetAnimatedJoints()[j];
		Require(track.mJointName == inSkeleton->GetJoint(j).mName, "track names follow skeleton order");
		Require(track.mKeyframes.size() == cSampleCount, "key count per track");
		for (uint i = 0; i < cSampleCount; ++i)
		{
			float expected_time = float(i) * cOutputDuration / float(cSampleCount - 1);
			if (i == cSampleCount - 1)
				expected_time = cOutputDuration;
			Require(Near(track.mKeyframes[i].mTime, expected_time, 1.0e-7), "output key time grid");
			if (i > 0)
				Require(track.mKeyframes[i].mTime > track.mKeyframes[i - 1].mTime, "output key times strictly increasing");
		}
	}

	Require(inResult.mSourceTimes[0] == 0.0f, "first source time is 0");
	Require(inResult.mSourceTimes[cSampleCount - 1] == inDuration, "last source time is D");
	for (uint i = 1; i < cSampleCount; ++i)
		Require(inResult.mSourceTimes[i] >= inResult.mSourceTimes[i - 1], "source times non decreasing");
}

// Verify cumulative lengths at the solved source times against equal arc targets,
// and verify the produced clip through the real Sample / CalculateJointMatrices path.
static void CheckArcLengthAndSampling(const ArcLengthResult &inResult, const SkeletalAnimation &inClip,
									  const Skeleton *inSkeleton, int inJointIndex, Vec3Arg inLocalPoint,
									  float inDuration, double inExpectedTotal, double inExpectedTolerance,
									  int inIndependentSegments = 20000)
{
	Ref<SkeletalAnimation> reference = MakeNonLoopingCopy(inClip);

	IndependentArcLength independent(*reference, inSkeleton, inJointIndex, inLocalPoint, inDuration, inIndependentSegments);
	Require(Near(inResult.mTotalLength, inExpectedTotal, inExpectedTolerance), "total length vs expected");
	Require(Near(inResult.mTotalLength, independent.Total(), cTolerance), "total length vs independent integrator");

	SkeletonPose out_pose;
	out_pose.SetSkeleton(inSkeleton);
	SkeletonPose ref_pose;
	ref_pose.SetSkeleton(inSkeleton);

	for (uint i = 0; i < cSampleCount; ++i)
	{
		float q = float(i) / float(cSampleCount - 1);

		// Cumulative length at the solved source time must match q * L(D).
		double cumulative = independent.LengthAt(inResult.mSourceTimes[i]);
		Require(Near(cumulative, double(q) * inResult.mTotalLength, cTolerance), "equal cumulative arc length at output key");

		// The output clip, sampled for real, must reproduce the source pose at sourceTimes[i].
		float output_time = float(i) * cOutputDuration / float(cSampleCount - 1);
		inResult.mAnimation->Sample(output_time, out_pose);
		out_pose.CalculateJointMatrices();
		reference->Sample(inResult.mSourceTimes[i], ref_pose);
		ref_pose.CalculateJointMatrices();

		Vec3 out_point = out_pose.GetJointMatrix(inJointIndex) * inLocalPoint;
		Vec3 ref_point = ref_pose.GetJointMatrix(inJointIndex) * inLocalPoint;
		Require(VecNear(out_point, ref_point, 2.0e-5f), "output clip reproduces the measured point");
	}

	// Endpoints of the output clip against the source curve ends.
	Vec3 out_start = [&] { inResult.mAnimation->Sample(0.0f, out_pose); out_pose.CalculateJointMatrices(); return out_pose.GetJointMatrix(inJointIndex) * inLocalPoint; }();
	Vec3 ref_start = [&] { reference->Sample(0.0f, ref_pose); ref_pose.CalculateJointMatrices(); return ref_pose.GetJointMatrix(inJointIndex) * inLocalPoint; }();
	Vec3 out_end = [&] { inResult.mAnimation->Sample(cOutputDuration, out_pose); out_pose.CalculateJointMatrices(); return out_pose.GetJointMatrix(inJointIndex) * inLocalPoint; }();
	Vec3 ref_end = [&] { reference->Sample(inDuration, ref_pose); ref_pose.CalculateJointMatrices(); return ref_pose.GetJointMatrix(inJointIndex) * inLocalPoint; }();
	Require(VecNear(out_start, ref_start, 2.0e-5f), "output start endpoint");
	Require(VecNear(out_end, ref_end, 2.0e-5f), "output end endpoint");
}

// Every output track must carry the same source time mapping, including branches
// that do not influence the measured point.
static void CheckBranchUsesSameSourceTimes(const ArcLengthResult &inResult, const SkeletalAnimation &inClip,
										   const Skeleton *inSkeleton, int inBranchJoint)
{
	Ref<SkeletalAnimation> reference = MakeNonLoopingCopy(inClip);
	SkeletonPose out_pose, ref_pose;
	out_pose.SetSkeleton(inSkeleton);
	ref_pose.SetSkeleton(inSkeleton);

	for (uint i = 0; i < cSampleCount; ++i)
	{
		float output_time = float(i) * cOutputDuration / float(cSampleCount - 1);
		inResult.mAnimation->Sample(output_time, out_pose);
		out_pose.CalculateJointMatrices();
		reference->Sample(inResult.mSourceTimes[i], ref_pose);
		ref_pose.CalculateJointMatrices();

		// Same rotation, allowing the quaternion sign to differ (same orientation).
		Quat out_rot = out_pose.GetJoint(inBranchJoint).mRotation;
		Quat ref_rot = ref_pose.GetJoint(inBranchJoint).mRotation;
		Require(out_rot.IsNormalized() && ref_rot.IsNormalized(), "branch rotation valid");
		Require(std::fabs(out_rot.Dot(ref_rot)) > 1.0f - 1.0e-5f, "branch rotation shares the source time mapping");

		// Same translation and same full model space transform of a local point.
		Require((out_pose.GetJoint(inBranchJoint).mTranslation - ref_pose.GetJoint(inBranchJoint).mTranslation).Length() <= 2.0e-5f,
				"branch translation shares the source time mapping");
		Vec3 probe(0.3f, -0.2f, 0.15f);
		Require(VecNear(out_pose.GetJointMatrix(inBranchJoint) * probe,
						ref_pose.GetJointMatrix(inBranchJoint) * probe, 2.0e-5f),
				"branch model transform shares the source time mapping");
	}
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

static SkeletonPose MakeRestPose(const Skeleton *inSkeleton)
{
	SkeletonPose pose;
	pose.SetSkeleton(inSkeleton);
	for (int j = 0; j < inSkeleton->GetJointCount(); ++j)
	{
		pose.GetJoint(j).mRotation = Quat::sIdentity();
		pose.GetJoint(j).mTranslation = Vec3::sZero();
	}
	return pose;
}

// 1. Fixed root, descendant swings an arm: analytic quarter circle.
static void TestArmSwing()
{
	Ref<Skeleton> skeleton = new Skeleton;
	skeleton->AddJoint("root", -1);
	skeleton->AddJoint("hand", 0);

	const float arm_length = 1.0f;
	const float local_radius = 0.5f;
	SkeletonPose rest = MakeRestPose(skeleton);
	rest.GetJoint(1).mTranslation = Vec3(arm_length, 0, 0);

	const float duration = 1.0f;
	Ref<SkeletalAnimation> clip = new SkeletalAnimation;
	clip->SetIsLooping(false);
	clip->GetAnimatedJoints().push_back(MakeTrack("hand", {
		MakeKey(0.0f, Quat::sIdentity(), Vec3(arm_length, 0, 0)),
		MakeKey(duration, Quat::sRotation(Vec3::sAxisZ(), JPH_PI / 2.0f), Vec3(arm_length, 0, 0)) }));

	ArcLengthSettings settings = MakeSettings(1, Vec3(local_radius, 0, 0));
	ArcLengthResult result;
	Require(RetimingByJointArcLength(*clip, rest, settings, result), "arm swing retiming succeeds");

	// The hand rotates about its own pivot (the arm translation); the local
	// point therefore sweeps a quarter circle of radius |local point|.
	double expected = double(local_radius) * JPH_PI / 2.0;
	CheckOutputStructure(result, skeleton, duration);
	CheckArcLengthAndSampling(result, *clip, skeleton, 1, settings.mLocalPoint, duration, expected, 1.0e-5);

	// Equal arc spacing: consecutive source times must span equal analytic angle.
	for (uint i = 1; i + 1 < cSampleCount; ++i)
	{
		double q = double(i) / double(cSampleCount - 1);
		double expected_time = q * double(duration);
		Require(Near(result.mSourceTimes[i], expected_time, 1.0e-3), "constant angular speed gives uniform source times");
	}
	std::puts("[ok] arm swing with fixed root");
}

// 2. Eight coaxial helper bones turning 90 degrees each: the off axis end point
//    makes two full turns, analytic length 4 pi, even though start, midpoint and
//    end positions coincide.
static void TestTwoFullTurns()
{
	constexpr int cBoneCount = 8;
	const float axis_spacing = 0.3f;
	const float radius = 1.0f;
	const float duration = 1.0f;

	Ref<Skeleton> skeleton = new Skeleton;
	skeleton->AddJoint("bone0", -1);
	for (int i = 1; i < cBoneCount; ++i)
		skeleton->AddJoint("bone" + std::to_string(i), i - 1);

	SkeletonPose rest = MakeRestPose(skeleton);
	for (int i = 0; i < cBoneCount; ++i)
		rest.GetJoint(i).mTranslation = Vec3(0, axis_spacing, 0);		// Non zero spacing along the rotation axis

	Ref<SkeletalAnimation> clip = new SkeletalAnimation;
	clip->SetIsLooping(false);
	for (int i = 0; i < cBoneCount; ++i)
		clip->GetAnimatedJoints().push_back(MakeTrack("bone" + std::to_string(i), {
			MakeKey(0.0f, Quat::sIdentity(), Vec3(0, axis_spacing, 0)),
			MakeKey(duration, Quat::sRotation(Vec3::sAxisY(), JPH_PI / 2.0f), Vec3(0, axis_spacing, 0)) }));

	Vec3 local_point(radius, 0, 0);
	ArcLengthSettings settings = MakeSettings(cBoneCount - 1, local_point);
	ArcLengthResult result;
	Require(RetimingByJointArcLength(*clip, rest, settings, result), "two turns retiming succeeds");

	CheckOutputStructure(result, skeleton, duration);
	CheckArcLengthAndSampling(result, *clip, skeleton, cBoneCount - 1, local_point, duration, 4.0 * JPH_PI, 2.0e-4);

	// Start, middle and end of the measured curve are the same point, yet the length is non zero.
	Ref<SkeletalAnimation> reference = MakeNonLoopingCopy(*clip);
	IndependentArcLength independent(*reference, skeleton, cBoneCount - 1, local_point, duration, 20000);
	Vec3 start = independent.PointAt(0.0f);
	Vec3 middle = independent.PointAt(result.mSourceTimes[cSampleCount / 2]);
	Vec3 end = independent.PointAt(duration);
	Require(VecNear(start, end, 2.0e-5f), "two turns return to the start point");
	Require(VecNear(start, middle, 2.0e-5f), "the midpoint is the same point after one turn (within float precision)");
	Require(result.mTotalLength > 12.0, "closed loop still has a non zero arc length");
	Require(Near(independent.LengthAt(result.mSourceTimes[cSampleCount / 2]), 2.0 * JPH_PI, 2.0e-4),
			"one turn of cumulative length at the coincident midpoint");

	// Uniform angular speed means uniform source times despite the coincident positions.
	for (uint i = 1; i + 1 < cSampleCount; ++i)
	{
		double q = double(i) / double(cSampleCount - 1);
		Require(Near(result.mSourceTimes[i], q * double(duration), 1.0e-3), "uniform source times over the two turns");
	}
	std::puts("[ok] eight coaxial bones, two full turns (4 pi)");
}

// 3. Staggered key times on different tracks and a genuine pause. The rightmost
//    solution must jump to the end of the stationary interval.
static void TestStaggeredKeysAndPause()
{
	Ref<Skeleton> skeleton = new Skeleton;
	skeleton->AddJoint("root", -1);
	skeleton->AddJoint("hand", 0);
	skeleton->AddJoint("flag", 0);									// Branch that does not influence the measured point

	const float radius = 2.0f;
	SkeletonPose rest = MakeRestPose(skeleton);
	rest.GetJoint(1).mTranslation = Vec3(radius, 0, 0);

	const float duration = 1.0f;
	Ref<SkeletalAnimation> clip = new SkeletalAnimation;
	clip->SetIsLooping(false);

	// Hand: quarter turn, pause between t = 0.3 and t = 0.6, then another quarter turn.
	clip->GetAnimatedJoints().push_back(MakeTrack("hand", {
		MakeKey(0.0f, Quat::sIdentity(), Vec3(radius, 0, 0)),
		MakeKey(0.3f, Quat::sRotation(Vec3::sAxisZ(), JPH_PI / 2.0f), Vec3(radius, 0, 0)),
		MakeKey(0.6f, Quat::sRotation(Vec3::sAxisZ(), JPH_PI / 2.0f), Vec3(radius, 0, 0)),
		MakeKey(duration, Quat::sRotation(Vec3::sAxisZ(), JPH_PI), Vec3(radius, 0, 0)) }));

	// Flag track with deliberately different key times.
	clip->GetAnimatedJoints().push_back(MakeTrack("flag", {
		MakeKey(0.0f, Quat::sIdentity(), Vec3(0, 0, 0)),
		MakeKey(0.25f, Quat::sRotation(Vec3::sAxisX(), 0.7f), Vec3(0, 0, 0)),
		MakeKey(0.7f, Quat::sRotation(Vec3::sAxisX(), 1.3f), Vec3(0, 0, 0)),
		MakeKey(duration, Quat::sRotation(Vec3::sAxisX(), 1.9f), Vec3(0, 0, 0)) }));

	Vec3 local_point(0.25f, 0, 0);
	ArcLengthSettings settings = MakeSettings(1, local_point);
	ArcLengthResult result;
	Require(RetimingByJointArcLength(*clip, rest, settings, result), "pause retiming succeeds");

	CheckOutputStructure(result, skeleton, duration);

	// Half the total length is reached at the start of the pause; the rightmost
	// solution is the end of the pause, t = 0.6.
	const uint half = cSampleCount / 2;
	Require(Near(result.mSourceTimes[half], 0.6, 1.0e-3), "rightmost root jumps to the end of the pause");
	Require(result.mSourceTimes[half - 1] < 0.6f, "key before the pause maps inside the moving part");
	Require(result.mSourceTimes[half + 1] > 0.6f, "key after the pause maps after it");

	Ref<SkeletalAnimation> reference = MakeNonLoopingCopy(*clip);
	IndependentArcLength independent(*reference, skeleton, 1, local_point, duration, 20000);
	for (uint i = 0; i < cSampleCount; ++i)
	{
		float q = float(i) / float(cSampleCount - 1);
		Require(Near(independent.LengthAt(result.mSourceTimes[i]), double(q) * result.mTotalLength, cTolerance),
				"equal arc length across the pause");
	}

	CheckBranchUsesSameSourceTimes(result, *clip, skeleton, 2);
	std::puts("[ok] staggered key times with a real pause");
}

// 4. A joint without a track keeps its rest pose local offset at every output key.
static void TestMissingTrackKeepsRestOffset()
{
	Ref<Skeleton> skeleton = new Skeleton;
	skeleton->AddJoint("root", -1);
	skeleton->AddJoint("hand", 0);
	skeleton->AddJoint("sensor", 1);

	SkeletonPose rest = MakeRestPose(skeleton);
	rest.GetJoint(1).mTranslation = Vec3(1, 0, 0);
	rest.GetJoint(2).mTranslation = Vec3(0, 0.5f, 0);							// Static local offset, no track will exist
	rest.GetJoint(2).mRotation = Quat::sRotation(Vec3::sAxisZ(), 0.4f);

	const float duration = 1.0f;
	Ref<SkeletalAnimation> clip = new SkeletalAnimation;
	clip->SetIsLooping(false);
	clip->GetAnimatedJoints().push_back(MakeTrack("hand", {
		MakeKey(0.0f, Quat::sIdentity(), Vec3(1, 0, 0)),
		MakeKey(duration, Quat::sRotation(Vec3::sAxisZ(), JPH_PI / 2.0f), Vec3(1, 0, 0)) }));

	ArcLengthSettings settings = MakeSettings(1, Vec3(0.2f, 0, 0));
	ArcLengthResult result;
	Require(RetimingByJointArcLength(*clip, rest, settings, result), "missing track retiming succeeds");

	// Every output key of the untracked joint equals the rest local state exactly.
	const AnimatedJoint &sensor_track = result.mAnimation->GetAnimatedJoints()[2];
	Require(sensor_track.mJointName == "sensor", "untracked joint still gets a track");
	for (const Keyframe &key : sensor_track.mKeyframes)
	{
		Require((key.mTranslation - rest.GetJoint(2).mTranslation).Length() == 0.0f, "rest translation preserved, no dirty reuse");
		Require(key.mRotation == rest.GetJoint(2).mRotation || key.mRotation == -rest.GetJoint(2).mRotation, "rest rotation preserved");
	}

	// Through the real sampling path the rest local offset is still applied,
	// composed on top of the (animated) parent transform.
	SkeletonPose pose;
	pose.SetSkeleton(skeleton);
	result.mAnimation->Sample(cOutputDuration, pose);
	pose.CalculateJointMatrices();
	Mat44 expected_sensor_model = pose.GetJointMatrix(1) * rest.GetJoint(2).ToMatrix();
	Require(VecNear(pose.GetJointMatrix(2).GetTranslation(), expected_sensor_model.GetTranslation(), 2.0e-5f),
			"static rest offset composed on the animated parent");
	std::puts("[ok] missing track keeps the rest local offset");
}

// 5. A looping source clip is still evaluated on the closed interval: the last
//    output key holds the source end pose, not the wrapped start pose.
static void TestLoopingInputClosedInterval()
{
	Ref<Skeleton> skeleton = new Skeleton;
	skeleton->AddJoint("root", -1);
	skeleton->AddJoint("hand", 0);

	SkeletonPose rest = MakeRestPose(skeleton);
	rest.GetJoint(1).mTranslation = Vec3(1, 0, 0);

	const float duration = 1.0f;
	Ref<SkeletalAnimation> clip = new SkeletalAnimation;
	clip->SetIsLooping(true);													// Looping input
	clip->GetAnimatedJoints().push_back(MakeTrack("hand", {
		MakeKey(0.0f, Quat::sIdentity(), Vec3(1, 0, 0)),
		MakeKey(duration, Quat::sRotation(Vec3::sAxisZ(), JPH_PI / 2.0f), Vec3(1, 0, 0)) }));

	ArcLengthSettings settings = MakeSettings(1, Vec3(0.5f, 0, 0));
	ArcLengthResult result;
	Require(RetimingByJointArcLength(*clip, rest, settings, result), "looping input retiming succeeds");
	Require(clip->IsLooping(), "source looping flag is not modified");
	Require(!result.mAnimation->IsLooping(), "output clip is non looping");

	// The looping clip itself wraps at D; a non looping copy exposes the end pose.
	Ref<SkeletalAnimation> reference = MakeNonLoopingCopy(*clip);
	SkeletonPose wrap_pose, end_pose, out_pose;
	wrap_pose.SetSkeleton(skeleton);
	end_pose.SetSkeleton(skeleton);
	out_pose.SetSkeleton(skeleton);

	clip->Sample(duration, wrap_pose);											// Wraps to the start pose
	reference->Sample(duration, end_pose);										// Genuine end pose
	result.mAnimation->Sample(cOutputDuration, out_pose);

	Require((out_pose.GetJoint(1).mTranslation - end_pose.GetJoint(1).mTranslation).Length() <= 2.0e-5f,
			"last output key uses the source end pose");
	Require((out_pose.GetJoint(1).mRotation - end_pose.GetJoint(1).mRotation).Length() <= 2.0e-5f,
			"last output key rotation is the end rotation");
	Require((out_pose.GetJoint(1).mRotation - wrap_pose.GetJoint(1).mRotation).Length() > 0.5f,
			"last output key is not the wrapped start pose");

	CheckOutputStructure(result, skeleton, duration);
	std::puts("[ok] looping input evaluated on the closed [0, D] interval");
}

// 6. Negating a quaternion denotes the same rotation: two clips differing only
//    by quaternion signs retime identically.
static void TestQuaternionSign()
{
	Ref<Skeleton> skeleton = new Skeleton;
	skeleton->AddJoint("root", -1);
	skeleton->AddJoint("hand", 0);

	SkeletonPose rest = MakeRestPose(skeleton);
	rest.GetJoint(1).mTranslation = Vec3(1, 0, 0);

	const float duration = 1.0f;
	auto build_clip = [&](bool inNegate) {
		Ref<SkeletalAnimation> clip = new SkeletalAnimation;
		clip->SetIsLooping(false);
		Quat end_rotation = Quat::sRotation(Vec3::sAxisZ(), JPH_PI / 2.0f);
		if (inNegate)
			end_rotation = -end_rotation;
		clip->GetAnimatedJoints().push_back(MakeTrack("hand", {
			MakeKey(0.0f, Quat::sIdentity(), Vec3(1, 0, 0)),
			MakeKey(duration, end_rotation, Vec3(1, 0, 0)) }));
		return clip;
	};

	ArcLengthSettings settings = MakeSettings(1, Vec3(0.5f, 0, 0));
	ArcLengthResult result_a, result_b;
	Require(RetimingByJointArcLength(*build_clip(false), rest, settings, result_a), "sign case A");
	Require(RetimingByJointArcLength(*build_clip(true), rest, settings, result_b), "sign case B");

	Require(Near(result_a.mTotalLength, result_b.mTotalLength, 1.0e-9), "lengths identical under quaternion negation");
	for (uint i = 0; i < cSampleCount; ++i)
		Require(Near(result_a.mSourceTimes[i], result_b.mSourceTimes[i], 1.0e-6), "source times identical under quaternion negation");

	SkeletonPose pose_a, pose_b;
	pose_a.SetSkeleton(skeleton);
	pose_b.SetSkeleton(skeleton);
	for (uint i = 0; i < cSampleCount; ++i)
	{
		float output_time = float(i) * cOutputDuration / float(cSampleCount - 1);
		result_a.mAnimation->Sample(output_time, pose_a);
		result_b.mAnimation->Sample(output_time, pose_b);
		pose_a.CalculateJointMatrices();
		pose_b.CalculateJointMatrices();
		Require(VecNear(pose_a.GetJointMatrix(1) * settings.mLocalPoint,
						pose_b.GetJointMatrix(1) * settings.mLocalPoint, 2.0e-5f),
				"negated quaternions produce the same measured curve");
	}
	std::puts("[ok] quaternion sign negation");
}

// 7. A completely stationary clip: zero length gives the uniform t = q * D map.
static void TestFullyStationary()
{
	Ref<Skeleton> skeleton = new Skeleton;
	skeleton->AddJoint("root", -1);
	skeleton->AddJoint("hand", 0);

	SkeletonPose rest = MakeRestPose(skeleton);
	rest.GetJoint(1).mTranslation = Vec3(1, 0, 0);

	const float duration = 1.5f;
	Ref<SkeletalAnimation> clip = new SkeletalAnimation;
	clip->SetIsLooping(false);
	clip->GetAnimatedJoints().push_back(MakeTrack("hand", {
		MakeKey(0.0f, Quat::sIdentity(), Vec3(1, 0, 0)),
		MakeKey(0.5f, Quat::sIdentity(), Vec3(1, 0, 0)),
		MakeKey(duration, Quat::sIdentity(), Vec3(1, 0, 0)) }));

	ArcLengthSettings settings = MakeSettings(1, Vec3(0.5f, 0, 0));
	ArcLengthResult result;
	Require(RetimingByJointArcLength(*clip, rest, settings, result), "stationary retiming succeeds");
	Require(result.mTotalLength == 0.0, "stationary curve has zero length");
	for (uint i = 0; i < cSampleCount; ++i)
	{
		float q = float(i) / float(cSampleCount - 1);
		Require(result.mSourceTimes[i] == q * duration, "uniform source times on a zero length curve");
	}
	CheckOutputStructure(result, skeleton, duration);
	std::puts("[ok] fully stationary clip");
}

// 8. An exhausted evaluation budget fails and never returns an approximate result.
static void TestBudgetExhaustedAndUntouchedOutput()
{
	Ref<Skeleton> skeleton = new Skeleton;
	skeleton->AddJoint("root", -1);
	skeleton->AddJoint("hand", 0);

	SkeletonPose rest = MakeRestPose(skeleton);
	rest.GetJoint(1).mTranslation = Vec3(1, 0, 0);

	const float duration = 1.0f;
	Ref<SkeletalAnimation> clip = new SkeletalAnimation;
	clip->SetIsLooping(false);
	clip->GetAnimatedJoints().push_back(MakeTrack("hand", {
		MakeKey(0.0f, Quat::sIdentity(), Vec3(1, 0, 0)),
		MakeKey(duration, Quat::sRotation(Vec3::sAxisZ(), JPH_PI / 2.0f), Vec3(1, 0, 0)) }));

	ArcLengthSettings settings = MakeSettings(1, Vec3(0.5f, 0, 0));
	settings.mMaxEvaluations = 3;										// Far below what adaptive integration needs

	ArcLengthResult result;
	result.mTotalLength = -1234.0;
	result.mSourceTimes.push_back(999.0f);
	Ref<SkeletalAnimation> sentinel = new SkeletalAnimation;
	result.mAnimation = sentinel;

	Require(!RetimingByJointArcLength(*clip, rest, settings, result), "tiny budget must fail");
	Require(result.mTotalLength == -1234.0, "result length untouched on failure");
	Require(result.mSourceTimes.size() == 1 && result.mSourceTimes[0] == 999.0f, "result source times untouched on failure");
	Require(result.mAnimation == sentinel, "result animation untouched on failure");
	std::puts("[ok] exhausted budget fails and leaves the result untouched");
}

// All input validation rejections.
static void TestValidationRejections()
{
	Ref<Skeleton> skeleton = new Skeleton;
	skeleton->AddJoint("root", -1);
	skeleton->AddJoint("hand", 0);

	SkeletonPose rest = MakeRestPose(skeleton);
	rest.GetJoint(1).mTranslation = Vec3(1, 0, 0);

	const float duration = 1.0f;
	auto make_good_clip = [&] {
		Ref<SkeletalAnimation> clip = new SkeletalAnimation;
		clip->SetIsLooping(false);
		clip->GetAnimatedJoints().push_back(MakeTrack("hand", {
			MakeKey(0.0f, Quat::sIdentity(), Vec3(1, 0, 0)),
			MakeKey(duration, Quat::sRotation(Vec3::sAxisZ(), 0.5f), Vec3(1, 0, 0)) }));
		return clip;
	};

	ArcLengthSettings good = MakeSettings(1, Vec3(0.5f, 0, 0));
	ArcLengthResult out;

	// sampleCount < 2
	{ ArcLengthSettings s = good; s.mSampleCount = 1; Require(!RetimingByJointArcLength(*make_good_clip(), rest, s, out), "sampleCount < 2 rejected"); }
	// outputDuration <= 0
	{ ArcLengthSettings s = good; s.mOutputDuration = 0.0f; Require(!RetimingByJointArcLength(*make_good_clip(), rest, s, out), "zero output duration rejected"); }
	{ ArcLengthSettings s = good; s.mOutputDuration = -1.0f; Require(!RetimingByJointArcLength(*make_good_clip(), rest, s, out), "negative output duration rejected"); }
	// lengthTolerance <= 0
	{ ArcLengthSettings s = good; s.mLengthTolerance = 0.0f; Require(!RetimingByJointArcLength(*make_good_clip(), rest, s, out), "zero tolerance rejected"); }
	// zero budget
	{ ArcLengthSettings s = good; s.mMaxEvaluations = 0; Require(!RetimingByJointArcLength(*make_good_clip(), rest, s, out), "zero budget rejected"); }
	// joint index out of range
	{ ArcLengthSettings s = good; s.mJointIndex = -1; Require(!RetimingByJointArcLength(*make_good_clip(), rest, s, out), "negative joint index rejected"); }
	{ ArcLengthSettings s = good; s.mJointIndex = 2; Require(!RetimingByJointArcLength(*make_good_clip(), rest, s, out), "joint index past the end rejected"); }
	// non finite local point
	{ ArcLengthSettings s = good; s.mLocalPoint = Vec3(std::nanf(""), 0, 0); Require(!RetimingByJointArcLength(*make_good_clip(), rest, s, out), "NaN local point rejected"); }
	{ ArcLengthSettings s = good; s.mLocalPoint = Vec3(std::numeric_limits<float>::infinity(), 0, 0); Require(!RetimingByJointArcLength(*make_good_clip(), rest, s, out), "infinite local point rejected"); }

	// No skeleton
	{ SkeletonPose empty; ArcLengthResult o; Require(!RetimingByJointArcLength(*make_good_clip(), empty, good, o), "pose without skeleton rejected"); }

	// Illegal parent ordering
	{
		Ref<Skeleton> bad = new Skeleton;
		bad->AddJoint("a", -1);
		bad->AddJoint("b", 0);
		bad->GetJoint(0).mParentJointIndex = 1;
		SkeletonPose bad_pose = MakeRestPose(bad);
		ArcLengthResult o;
		Require(!RetimingByJointArcLength(*make_good_clip(), bad_pose, good, o), "bad parent ordering rejected");
	}

	// D <= 0
	{
		Ref<SkeletalAnimation> zero_clip = new SkeletalAnimation;
		zero_clip->SetIsLooping(false);
		zero_clip->GetAnimatedJoints().push_back(MakeTrack("hand", { MakeKey(0.0f, Quat::sIdentity(), Vec3(1, 0, 0)) }));
		ArcLengthResult o;
		Require(!RetimingByJointArcLength(*zero_clip, rest, good, o), "zero duration rejected");
	}

	// Empty track
	{
		Ref<SkeletalAnimation> clip = make_good_clip();
		clip->GetAnimatedJoints().push_back(MakeTrack("root", {}));
		ArcLengthResult o;
		Require(!RetimingByJointArcLength(*clip, rest, good, o), "empty track rejected");
	}

	// Duplicate and unknown bone names
	{
		Ref<SkeletalAnimation> clip = make_good_clip();
		clip->GetAnimatedJoints().push_back(MakeTrack("hand", {
			MakeKey(0.0f, Quat::sIdentity(), Vec3::sZero()),
			MakeKey(duration, Quat::sIdentity(), Vec3::sZero()) }));
		ArcLengthResult o;
		Require(!RetimingByJointArcLength(*clip, rest, good, o), "duplicate track rejected");
	}
	{
		Ref<SkeletalAnimation> clip = make_good_clip();
		clip->GetAnimatedJoints().push_back(MakeTrack("ghost", {
			MakeKey(0.0f, Quat::sIdentity(), Vec3::sZero()),
			MakeKey(duration, Quat::sIdentity(), Vec3::sZero()) }));
		ArcLengthResult o;
		Require(!RetimingByJointArcLength(*clip, rest, good, o), "unknown bone rejected");
	}

	// Non unit rotation
	{
		Ref<SkeletalAnimation> clip = new SkeletalAnimation;
		clip->SetIsLooping(false);
		Quat bad_rotation = Quat::sRotation(Vec3::sAxisZ(), 0.5f) * 2.0f;
		clip->GetAnimatedJoints().push_back(MakeTrack("hand", {
			MakeKey(0.0f, Quat::sIdentity(), Vec3(1, 0, 0)),
			MakeKey(duration, bad_rotation, Vec3(1, 0, 0)) }));
		ArcLengthResult o;
		Require(!RetimingByJointArcLength(*clip, rest, good, o), "non unit rotation rejected");
	}

	// Non finite translation and non strictly increasing key times
	{
		Ref<SkeletalAnimation> clip = new SkeletalAnimation;
		clip->SetIsLooping(false);
		clip->GetAnimatedJoints().push_back(MakeTrack("hand", {
			MakeKey(0.0f, Quat::sIdentity(), Vec3(1, 0, 0)),
			MakeKey(0.5f, Quat::sIdentity(), Vec3(std::nanf(""), 0, 0)),
			MakeKey(duration, Quat::sIdentity(), Vec3(1, 0, 0)) }));
		ArcLengthResult o;
		Require(!RetimingByJointArcLength(*clip, rest, good, o), "NaN translation rejected");
	}
	{
		Ref<SkeletalAnimation> clip = new SkeletalAnimation;
		clip->SetIsLooping(false);
		clip->GetAnimatedJoints().push_back(MakeTrack("hand", {
			MakeKey(0.0f, Quat::sIdentity(), Vec3(1, 0, 0)),
			MakeKey(0.5f, Quat::sIdentity(), Vec3(1, 0, 0)),
			MakeKey(0.5f, Quat::sIdentity(), Vec3(1, 0, 0)),
			MakeKey(duration, Quat::sIdentity(), Vec3(1, 0, 0)) }));
		ArcLengthResult o;
		Require(!RetimingByJointArcLength(*clip, rest, good, o), "non increasing key times rejected");
	}

	// Non unit rest pose rotation
	{
		SkeletonPose bad_rest = rest;
		bad_rest.GetJoint(0).mRotation = Quat::sIdentity() * 2.0f;
		ArcLengthResult o;
		Require(!RetimingByJointArcLength(*make_good_clip(), bad_rest, good, o), "non unit rest rotation rejected");
	}
	std::puts("[ok] input validation rejections");
}

// Root offset must not enter the measured curve, and inputs must stay untouched.
static void TestRootOffsetExcludedAndInputsUntouched()
{
	Ref<Skeleton> skeleton = new Skeleton;
	skeleton->AddJoint("root", -1);
	skeleton->AddJoint("hand", 0);

	SkeletonPose rest = MakeRestPose(skeleton);
	rest.GetJoint(1).mTranslation = Vec3(1, 0, 0);
	rest.SetRootOffset(RVec3(100, -50, 7));

	const float duration = 1.0f;
	Ref<SkeletalAnimation> clip = new SkeletalAnimation;
	clip->SetIsLooping(true);
	clip->GetAnimatedJoints().push_back(MakeTrack("hand", {
		MakeKey(0.0f, Quat::sIdentity(), Vec3(1, 0, 0)),
		MakeKey(duration, Quat::sRotation(Vec3::sAxisZ(), JPH_PI / 2.0f), Vec3(1, 0, 0)) }));

	ArcLengthSettings settings = MakeSettings(1, Vec3(0.5f, 0, 0));
	ArcLengthResult result;
	Require(RetimingByJointArcLength(*clip, rest, settings, result), "root offset case succeeds");

	double expected = 0.5 * JPH_PI / 2.0;
	Require(Near(result.mTotalLength, expected, 1.0e-5), "root offset does not contribute to arc length");
	Require(rest.GetRootOffset() == RVec3(100, -50, 7), "rest pose root offset untouched");
	Require(clip->IsLooping(), "looping flag untouched");
	Require(clip->GetDuration() == duration && clip->GetAnimatedJoints().size() == 1, "source clip untouched");
	std::puts("[ok] root offset excluded and inputs untouched");
}

int main()
{
	RegisterDefaultAllocator();

	TestArmSwing();
	TestTwoFullTurns();
	TestStaggeredKeysAndPause();
	TestMissingTrackKeepsRestOffset();
	TestLoopingInputClosedInterval();
	TestQuaternionSign();
	TestFullyStationary();
	TestBudgetExhaustedAndUntouchedOutput();
	TestValidationRejections();
	TestRootOffsetExcludedAndInputsUntouched();

	std::puts("JOLT_ARCLENGTH_OK");
	return 0;
}
