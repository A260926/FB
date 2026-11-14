// Spring chain task cases.  Registered through task_support/check.h; this
// file provides no main().  All assets are constructed in code.

#include "task_support/check.h"

#include <cal3d/animation.h>
#include <cal3d/bone.h>
#include <cal3d/bonetransform.h>
#include <cal3d/coreanimation.h>
#include <cal3d/corebone.h>
#include <cal3d/corekeyframe.h>
#include <cal3d/coreskeleton.h>
#include <cal3d/coresubmesh.h>
#include <cal3d/coretrack.h>
#include <cal3d/matrix.h>
#include <cal3d/mixer.h>
#include <cal3d/physique.h>
#include <cal3d/quaternion.h>
#include <cal3d/skeleton.h>
#include <cal3d/springchain.h>
#include <cal3d/transform.h>

#include <cmath>
#include <limits>
#include <vector>

namespace {
    const float H = CalSpringChain::SUBSTEP;

    bool near(float a, float b, float tolerance) {
        return std::fabs(a - b) <= tolerance;
    }

    bool nearVec(const CalVector& a, const CalVector& b, float tolerance) {
        return near(a.x, b.x, tolerance)
               && near(a.y, b.y, tolerance)
               && near(a.z, b.z, tolerance);
    }

    bool finiteVec(const CalVector& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    bool finiteBoneTransform(const BoneTransform& t) {
        return std::isfinite(t.rowx.x) && std::isfinite(t.rowx.y)
               && std::isfinite(t.rowx.z) && std::isfinite(t.rowx.w)
               && std::isfinite(t.rowy.x) && std::isfinite(t.rowy.y)
               && std::isfinite(t.rowy.z) && std::isfinite(t.rowy.w)
               && std::isfinite(t.rowz.x) && std::isfinite(t.rowz.y)
               && std::isfinite(t.rowz.z) && std::isfinite(t.rowz.w);
    }

    CalQuaternion axisAngle(const CalVector& axis, float angle) {
        CalQuaternion q;
        q.setAxisAngle(axis, angle);
        return q;
    }

    CalVector4 applyBoneTransform(const BoneTransform& m, const CalPoint4& v) {
        return CalVector4(
            m.rowx.x * v.x + m.rowx.y * v.y + m.rowx.z * v.z + m.rowx.w,
            m.rowy.x * v.x + m.rowy.y * v.y + m.rowy.z * v.z + m.rowy.w,
            m.rowz.x * v.x + m.rowz.y * v.y + m.rowz.z * v.z + m.rowz.w,
            1.0f);
    }

    // One root bone at the origin.  Each chain supplies its own segment
    // length through finalTipLocal or the child bone's local translation.
    ISTD::shared_ptr<CalCoreSkeleton> makeSingleBoneCore() {
        ISTD::shared_ptr<CalCoreSkeleton> core = ISTD::make_shared<CalCoreSkeleton>();
        core->addCoreBone(ISTD::make_shared<CalCoreBone>("root"));
        return core;
    }

    CalSpringChain::Config makeConfig(
        float stiffness,
        float damping,
        const CalVector& gravity,
        const std::vector<CalSpringChain::Contact>& contacts =
            std::vector<CalSpringChain::Contact>())
    {
        CalSpringChain::Config config;
        config.stiffness = stiffness;
        config.damping = damping;
        config.gravity = gravity;
        config.contacts = contacts;
        return config;
    }
}

//***********************************************************************//
// dynamics                                                              //
//***********************************************************************//

static int dynamics_single_step_integration() {
    ISTD::shared_ptr<CalCoreSkeleton> core = makeSingleBoneCore();
    CalSkeleton skeleton(core);
    CalMixer mixer;
    mixer.updateSkeleton(
        &skeleton, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());

    const float stiffness = 12.0f;
    const float damping = 3.5f;
    const CalVector gravity(0.0f, -10.0f, 0.0f);

    CalSpringChain chain;
    TASK_CHECK(CalSpringChain::create(
                   chain, skeleton, std::vector<int>{0}, CalVector(1, 0, 0),
                   makeConfig(stiffness, damping, gravity))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.reset(skeleton, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);

    TASK_CHECK(chain.accumulatedSubsteps() == 0);
    TASK_CHECK(near(chain.timeBalance(), 0.0f, 1e-9f));

    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), H)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.accumulatedSubsteps() == 1);
    TASK_CHECK(near(chain.timeBalance(), 0.0f, 1e-9f));

    // Independent reference computation of the same substep.
    const CalVector p0(1.0f, 0.0f, 0.0f);
    const CalVector v0(0.0f, 0.0f, 0.0f);
    const CalVector goal(1.0f, 0.0f, 0.0f);
    const CalVector vTrial =
        (v0 + H * (stiffness * (goal - p0) + gravity))
        * expf(-damping * H);
    CalVector pTrial = p0 + H * vTrial;
    const float radialLength = pTrial.length();
    const CalVector pExpected = pTrial * (1.0f / radialLength);
    const CalVector vExpected = (pExpected - p0) / H;

    const CalVector* p = chain.endpoint(0);
    const CalVector* v = chain.velocity(0);
    TASK_CHECK(p != 0 && v != 0);
    TASK_CHECK(nearVec(*p, pExpected, 1e-6f));
    TASK_CHECK(nearVec(*v, vExpected, 1e-5f));
    TASK_CHECK(near(p->length(), 1.0f, 1e-6f));
    TASK_CHECK(chain.maxLengthError() < 1e-5f);
    return 0;
}

static int dynamics_static_equilibrium() {
    ISTD::shared_ptr<CalCoreSkeleton> core = makeSingleBoneCore();
    CalSkeleton skeleton(core);
    CalMixer mixer;
    mixer.updateSkeleton(
        &skeleton, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());

    CalSpringChain chain;
    TASK_CHECK(CalSpringChain::create(
                   chain, skeleton, std::vector<int>{0}, CalVector(1, 0, 0),
                   makeConfig(50.0f, 4.0f, CalVector()))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.reset(skeleton, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);

    // No gravity, endpoint already on the animated goal: nothing should move.
    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), 0.25f)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.accumulatedSubsteps() == 30);
    const CalVector* p = chain.endpoint(0);
    const CalVector* v = chain.velocity(0);
    TASK_CHECK(nearVec(*p, CalVector(1, 0, 0), 1e-6f));
    TASK_CHECK(nearVec(*v, CalVector(), 1e-6f));
    TASK_CHECK(chain.maxLengthError() < 1e-6f);
    TASK_CHECK(chain.remainingPenetration() == 0.0f);
    return 0;
}

static int dynamics_damping_decay() {
    ISTD::shared_ptr<CalCoreSkeleton> core = makeSingleBoneCore();
    CalSkeleton skeletonDamped(core);
    CalSkeleton skeletonFree(core);
    CalMixer mixerDamped;
    CalMixer mixerFree;
    mixerDamped.updateSkeleton(
        &skeletonDamped, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());
    mixerFree.updateSkeleton(
        &skeletonFree, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());

    const CalVector gravity(0.0f, -10.0f, 0.0f);
    const float damping = 6.0f;

    CalSpringChain damped;
    CalSpringChain free;
    TASK_CHECK(CalSpringChain::create(
                   damped, skeletonDamped, std::vector<int>{0},
                   CalVector(1, 0, 0), makeConfig(0.0f, damping, gravity))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(CalSpringChain::create(
                   free, skeletonFree, std::vector<int>{0},
                   CalVector(1, 0, 0), makeConfig(0.0f, 0.0f, gravity))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(damped.reset(skeletonDamped, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);
    TASK_CHECK(free.reset(skeletonFree, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);

    TASK_CHECK(damped.advance(skeletonDamped, cal3d::RotateTranslate(), H)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(free.advance(skeletonFree, cal3d::RotateTranslate(), H)
               == CalSpringChain::SUCCESS);

    // Before projection, damped trial velocity is exactly h*g*exp(-d*h).
    const CalVector dampedTrial = gravity * (H * expf(-damping * H));
    const CalVector freeTrial = gravity * H;
    TASK_CHECK(std::fabs(damped.velocity(0)->y) < std::fabs(free.velocity(0)->y));
    TASK_CHECK(near(damped.velocity(0)->y, dampedTrial.y, 2e-4f));
    TASK_CHECK(near(free.velocity(0)->y, freeTrial.y, 2e-4f));
    return 0;
}

static int dynamics_fixed_substep_chunking() {
    const CalVector gravity(0.0f, -9.8f, 0.0f);

    CalSkeleton skeletons[3] = {
        CalSkeleton(makeSingleBoneCore()),
        CalSkeleton(makeSingleBoneCore()),
        CalSkeleton(makeSingleBoneCore()),
    };
    CalSpringChain chains[3];
    for (int i = 0; i < 3; ++i) {
        CalMixer mixer;
        mixer.updateSkeleton(
            &skeletons[i], std::vector<BoneTransformAdjustment>(),
            std::vector<BoneScaleAdjustment>());
        TASK_CHECK(CalSpringChain::create(
                       chains[i], skeletons[i], std::vector<int>{0},
                       CalVector(1, 0, 0), makeConfig(25.0f, 3.0f, gravity))
                   == CalSpringChain::SUCCESS);
        TASK_CHECK(chains[i].reset(skeletons[i], cal3d::RotateTranslate())
                   == CalSpringChain::SUCCESS);
    }

    TASK_CHECK(chains[0].advance(skeletons[0], cal3d::RotateTranslate(), 0.1f)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chains[1].advance(skeletons[1], cal3d::RotateTranslate(), 0.05f)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chains[1].advance(skeletons[1], cal3d::RotateTranslate(), 0.05f)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chains[2].advance(skeletons[2], cal3d::RotateTranslate(), 0.033f)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chains[2].advance(skeletons[2], cal3d::RotateTranslate(), 0.033f)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chains[2].advance(skeletons[2], cal3d::RotateTranslate(), 0.034f)
               == CalSpringChain::SUCCESS);

    for (int i = 0; i < 3; ++i) {
        TASK_CHECK(chains[i].accumulatedSubsteps() == 12);
        TASK_CHECK(near(chains[i].timeBalance(), chains[0].timeBalance(), 1e-8f));
    }
    for (int i = 1; i < 3; ++i) {
        TASK_CHECK(nearVec(*chains[i].endpoint(0), *chains[0].endpoint(0), 1e-5f));
        TASK_CHECK(nearVec(*chains[i].velocity(0), *chains[0].velocity(0), 1e-5f));
    }
    return 0;
}

static int dynamics_two_independent_instances() {
    ISTD::shared_ptr<CalCoreSkeleton> core = makeSingleBoneCore();
    CalSkeleton skeletonA(core);
    CalSkeleton skeletonB(core);
    CalMixer mixerA;
    CalMixer mixerB;
    mixerA.updateSkeleton(
        &skeletonA, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());
    mixerB.updateSkeleton(
        &skeletonB, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());

    CalSpringChain chainA;
    CalSpringChain chainB;
    TASK_CHECK(CalSpringChain::create(
                   chainA, skeletonA, std::vector<int>{0}, CalVector(1, 0, 0),
                   makeConfig(10.0f, 2.0f, CalVector(0, -9.8f, 0)))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(CalSpringChain::create(
                   chainB, skeletonB, std::vector<int>{0}, CalVector(1, 0, 0),
                   makeConfig(10.0f, 2.0f, CalVector(0, -3.0f, 0)))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chainA.reset(skeletonA, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chainB.reset(skeletonB, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);

    TASK_CHECK(chainA.advance(skeletonA, cal3d::RotateTranslate(), 0.1f)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chainB.advance(skeletonB, cal3d::RotateTranslate(), 0.05f)
               == CalSpringChain::SUCCESS);

    const CalVector snapshotA = *chainA.endpoint(0);
    const CalVector snapshotVelA = *chainA.velocity(0);
    const unsigned long long snapshotStepsA = chainA.accumulatedSubsteps();

    // Mutating instance B must not touch instance A.
    TASK_CHECK(chainB.reset(skeletonB, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chainB.advance(skeletonB, cal3d::RotateTranslate(), 0.2f)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(nearVec(*chainA.endpoint(0), snapshotA, 0.0f));
    TASK_CHECK(nearVec(*chainA.velocity(0), snapshotVelA, 0.0f));
    TASK_CHECK(chainA.accumulatedSubsteps() == snapshotStepsA);
    TASK_CHECK(chainB.accumulatedSubsteps() != snapshotStepsA);
    TASK_CHECK(!nearVec(*chainA.endpoint(0), *chainB.endpoint(0), 1e-3f));
    return 0;
}

static int dynamics_partial_substep_only_accumulates() {
    ISTD::shared_ptr<CalCoreSkeleton> core = makeSingleBoneCore();
    CalSkeleton skeleton(core);
    CalMixer mixer;
    mixer.updateSkeleton(
        &skeleton, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());

    CalSpringChain chain;
    TASK_CHECK(CalSpringChain::create(
                   chain, skeleton, std::vector<int>{0}, CalVector(1, 0, 0),
                   makeConfig(20.0f, 3.0f, CalVector(0, -9.8f, 0)))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.reset(skeleton, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);

    const CalVector initialEndpoint = *chain.endpoint(0);
    const CalVector initialBoneTranslation =
        skeleton.bones[0].absoluteTransform.translation;

    // Less than one substep: time accumulates, nothing integrates or moves.
    const float partial = H * 0.5f;
    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), partial)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.accumulatedSubsteps() == 0);
    TASK_CHECK(near(chain.timeBalance(), partial, 1e-9f));
    TASK_CHECK(nearVec(*chain.endpoint(0), initialEndpoint, 0.0f));
    TASK_CHECK(nearVec(*chain.velocity(0), CalVector(), 0.0f));
    TASK_CHECK(nearVec(skeleton.bones[0].absoluteTransform.translation,
                       initialBoneTranslation, 0.0f));

    // Zero dt changes nothing.
    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), 0.0f)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.accumulatedSubsteps() == 0);
    TASK_CHECK(near(chain.timeBalance(), partial, 1e-9f));

    // A second partial call crosses one full substep: the accumulated time
    // is consumed rather than discarded.
    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), partial)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.accumulatedSubsteps() == 1);
    TASK_CHECK(chain.timeBalance() < H);
    return 0;
}

static int dynamics_twist_preserved_under_swing() {
    ISTD::shared_ptr<CalCoreSkeleton> core = makeSingleBoneCore();
    CalSkeleton skeleton(core);
    CalMixer mixer;
    mixer.updateSkeleton(
        &skeleton, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());

    // Animated pose is a pure twist about the segment axis (+x); it leaves
    // the segment direction at +x but rotates the perpendicular frame.
    const float twistAngle = 0.5f;
    cal3d::RotateTranslate twisted(
        axisAngle(CalVector(1, 0, 0), twistAngle), CalVector());
    skeleton.bones[0].setRelativeTransform(twisted);
    skeleton.calculateAbsolutePose();

    CalSpringChain chain;
    TASK_CHECK(CalSpringChain::create(
                   chain, skeleton, std::vector<int>{0}, CalVector(1, 0, 0),
                   makeConfig(2.0f, 1.0f, CalVector(0, -20.0f, 0)))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.reset(skeleton, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), 0.2f)
               == CalSpringChain::SUCCESS);

    // The endpoint must have swung away from +x.
    const CalVector segmentAxis = *chain.endpoint(0);
    TASK_CHECK(segmentAxis.length() > 0.999f);
    TASK_CHECK(std::fabs(segmentAxis.y) > 0.05f);

    // Twist angle of a unit quaternion Q around unit axis n is
    // 2 * atan2(dot(Q.xyz, n), Q.w).  It must equal the animated twist.
    const CalQuaternion resultQuat(skeleton.bones[0].absoluteTransform.basis);
    const float dot = resultQuat.x * segmentAxis.x
                      + resultQuat.y * segmentAxis.y
                      + resultQuat.z * segmentAxis.z;
    const float resultTwist = 2.0f * atan2f(dot, resultQuat.w);
    TASK_CHECK(near(resultTwist, twistAngle, 2e-4f));
    TASK_CHECK(std::isfinite(resultQuat.x) && std::isfinite(resultQuat.y)
               && std::isfinite(resultQuat.z) && std::isfinite(resultQuat.w));
    return 0;
}

static int dynamics_invalid_inputs_leave_state_untouched() {
    ISTD::shared_ptr<CalCoreSkeleton> core = ISTD::make_shared<CalCoreSkeleton>();
    core->addCoreBone(ISTD::make_shared<CalCoreBone>("root"));
    ISTD::shared_ptr<CalCoreBone> child = ISTD::make_shared<CalCoreBone>("a", 0);
    child->relativeTransform.translation = CalVector(1, 0, 0);
    core->addCoreBone(child);
    ISTD::shared_ptr<CalCoreBone> orphan = ISTD::make_shared<CalCoreBone>("b", 0);
    orphan->relativeTransform.translation = CalVector(0, 1, 0);
    core->addCoreBone(orphan);

    CalSkeleton skeleton(core);
    CalMixer mixer;
    mixer.updateSkeleton(
        &skeleton, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());

    // Bad index.
    CalSpringChain bad;
    TASK_CHECK(CalSpringChain::create(
                   bad, skeleton, std::vector<int>{7}, CalVector(1, 0, 0),
                   makeConfig(1, 1, CalVector()))
               == CalSpringChain::INVALID_ARGUMENT);
    TASK_CHECK(!bad.initialized());

    // Discontinuous chain: bone 2's parent is 0, not bone 1.
    TASK_CHECK(CalSpringChain::create(
                   bad, skeleton, std::vector<int>{1, 2}, CalVector(1, 0, 0),
                   makeConfig(1, 1, CalVector()))
               == CalSpringChain::INVALID_ARGUMENT);

    // Zero segment length.
    TASK_CHECK(CalSpringChain::create(
                   bad, skeleton, std::vector<int>{0}, CalVector(0, 0, 0),
                   makeConfig(1, 1, CalVector()))
               == CalSpringChain::INVALID_ARGUMENT);

    // Negative parameters.
    TASK_CHECK(CalSpringChain::create(
                   bad, skeleton, std::vector<int>{0, 1}, CalVector(1, 0, 0),
                   makeConfig(-1.0f, 1.0f, CalVector()))
               == CalSpringChain::INVALID_ARGUMENT);
    TASK_CHECK(CalSpringChain::create(
                   bad, skeleton, std::vector<int>{0, 1}, CalVector(1, 0, 0),
                   makeConfig(1.0f, -1.0f, CalVector()))
               == CalSpringChain::INVALID_ARGUMENT);

    // Invalid contact spheres.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    TASK_CHECK(CalSpringChain::create(
                   bad, skeleton, std::vector<int>{0, 1}, CalVector(1, 0, 0),
                   makeConfig(1, 1, CalVector(),
                              std::vector<CalSpringChain::Contact>{
                                  CalSpringChain::Contact(CalVector(), 0.0f)}))
               == CalSpringChain::INVALID_ARGUMENT);
    TASK_CHECK(CalSpringChain::create(
                   bad, skeleton, std::vector<int>{0, 1}, CalVector(1, 0, 0),
                   makeConfig(1, 1, CalVector(),
                              std::vector<CalSpringChain::Contact>{
                                  CalSpringChain::Contact(CalVector(nan, 0, 0), 1.0f)}))
               == CalSpringChain::INVALID_ARGUMENT);

    // Scaled skeleton.
    skeleton.bones[0].scale = cal3d::Scale(CalVector(2, 2, 2));
    TASK_CHECK(CalSpringChain::create(
                   bad, skeleton, std::vector<int>{0, 1}, CalVector(1, 0, 0),
                   makeConfig(1, 1, CalVector()))
               == CalSpringChain::INVALID_SKELETON);
    skeleton.bones[0].scale.setIdentity();

    // A valid chain, then runtime validation failures.
    CalSpringChain chain;
    TASK_CHECK(CalSpringChain::create(
                   chain, skeleton, std::vector<int>{0, 1}, CalVector(1, 0, 0),
                   makeConfig(10.0f, 2.0f, CalVector(0, -9.8f, 0)))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.reset(skeleton, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), 0.05f)
               == CalSpringChain::SUCCESS);

    const CalVector savedEndpoint = *chain.endpoint(0);
    const float savedBalance = chain.timeBalance();
    const unsigned long long savedSteps = chain.accumulatedSubsteps();
    const CalVector savedBoneTranslation =
        skeleton.bones[1].absoluteTransform.translation;

    // Non-rigid object transform (non-unit quaternion).
    cal3d::RotateTranslate nonRigid(
        CalQuaternion(0, 0, 0, 2), CalVector(1, 0, 0));
    TASK_CHECK(chain.advance(skeleton, nonRigid, 0.05f)
               == CalSpringChain::INVALID_ARGUMENT);
    TASK_CHECK(chain.reset(skeleton, nonRigid)
               == CalSpringChain::INVALID_ARGUMENT);

    // dt out of range and non-finite.
    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), 1.5f)
               == CalSpringChain::INVALID_ARGUMENT);
    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), -0.1f)
               == CalSpringChain::INVALID_ARGUMENT);
    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), nan)
               == CalSpringChain::INVALID_ARGUMENT);

    // Failed configure() keeps the old configuration.
    CalSpringChain::Config badConfig = makeConfig(-1.0f, 0.0f, CalVector());
    TASK_CHECK(chain.configure(badConfig)
               == CalSpringChain::INVALID_ARGUMENT);

    // State is byte-for-byte unchanged after every failure.
    TASK_CHECK(nearVec(*chain.endpoint(0), savedEndpoint, 0.0f));
    TASK_CHECK(near(chain.timeBalance(), savedBalance, 0.0f));
    TASK_CHECK(chain.accumulatedSubsteps() == savedSteps);
    TASK_CHECK(nearVec(skeleton.bones[1].absoluteTransform.translation,
                       savedBoneTranslation, 0.0f));

    // Non-finite animated pose: validation must fail before any mutation.
    const CalVector savedBone0Translation =
        skeleton.bones[0].absoluteTransform.translation;
    cal3d::RotateTranslate corrupted =
        skeleton.bones[0].getRelativeTransform();
    corrupted.rotation.x = nan;
    skeleton.bones[0].setRelativeTransform(corrupted);
    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), H)
               == CalSpringChain::INVALID_ARGUMENT);
    TASK_CHECK(chain.reset(skeleton, cal3d::RotateTranslate())
               == CalSpringChain::INVALID_ARGUMENT);
    // The failed calls changed neither the chain state nor the skeleton.
    TASK_CHECK(nearVec(*chain.endpoint(0), savedEndpoint, 0.0f));
    TASK_CHECK(chain.accumulatedSubsteps() == savedSteps);
    TASK_CHECK(nearVec(skeleton.bones[0].absoluteTransform.translation,
                       savedBone0Translation, 0.0f));
    // Restore a valid animated pose through the mixer path.
    mixer.updateSkeleton(
        &skeleton, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());

    // The old config still works.
    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), H)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.accumulatedSubsteps() == savedSteps + 1);

    // Entry points on an uninitialized chain.
    TASK_CHECK(bad.advance(skeleton, cal3d::RotateTranslate(), H)
               == CalSpringChain::NOT_INITIALIZED);
    TASK_CHECK(bad.reset(skeleton, cal3d::RotateTranslate())
               == CalSpringChain::NOT_INITIALIZED);
    TASK_CHECK(bad.configure(makeConfig(1, 1, CalVector()))
               == CalSpringChain::NOT_INITIALIZED);
    return 0;
}

//***********************************************************************//
// contacts                                                              //
//***********************************************************************//

static int contacts_solvable_sphere_contact() {
    ISTD::shared_ptr<CalCoreSkeleton> core = makeSingleBoneCore();
    CalSkeleton skeleton(core);
    CalMixer mixer;
    mixer.updateSkeleton(
        &skeleton, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());

    // Sphere disjoint from the anchor; the falling chain lands on it.
    const CalSpringChain::Contact contact(CalVector(0.6f, -0.6f, 0.0f), 0.5f);
    CalSpringChain chain;
    TASK_CHECK(CalSpringChain::create(
                   chain, skeleton, std::vector<int>{0}, CalVector(1, 0, 0),
                   makeConfig(2.0f, 1.0f, CalVector(0, -25.0f, 0),
                              std::vector<CalSpringChain::Contact>{contact}))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.reset(skeleton, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);

    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), 0.5f)
               == CalSpringChain::SUCCESS);

    TASK_CHECK(chain.maxLengthError() < 1e-4f);
    TASK_CHECK(chain.remainingPenetration() < 1e-4f);

    const CalVector* p = chain.endpoint(0);
    TASK_CHECK(finiteVec(*p));
    const float clearance = (*p - contact.center).length() - contact.radius;
    TASK_CHECK(clearance >= -1e-4f);
    // The chain actually reached the sphere instead of swinging past it.
    TASK_CHECK(clearance <= 1e-3f);
    return 0;
}

static int contacts_two_disjoint_spheres() {
    ISTD::shared_ptr<CalCoreSkeleton> core = makeSingleBoneCore();
    CalSkeleton skeleton(core);
    CalMixer mixer;
    mixer.updateSkeleton(
        &skeleton, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());

    // Two mutually disjoint spheres.  A catches the falling endpoint; B sits
    // beside the swing path and must be avoided by the joint projection.
    const CalSpringChain::Contact sphereA(CalVector(0.6f, -0.6f, 0.0f), 0.5f);
    const CalSpringChain::Contact sphereB(CalVector(-0.9f, -0.3f, 0.0f), 0.3f);
    TASK_CHECK((sphereA.center - sphereB.center).length()
               > sphereA.radius + sphereB.radius);

    CalSpringChain chain;
    TASK_CHECK(CalSpringChain::create(
                   chain, skeleton, std::vector<int>{0}, CalVector(1, 0, 0),
                   makeConfig(2.0f, 1.0f, CalVector(0, -25.0f, 0),
                              std::vector<CalSpringChain::Contact>{
                                  sphereA, sphereB}))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.reset(skeleton, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), 0.5f)
               == CalSpringChain::SUCCESS);

    TASK_CHECK(chain.maxLengthError() < 1e-4f);
    TASK_CHECK(chain.remainingPenetration() < 1e-4f);
    const CalVector* p = chain.endpoint(0);
    TASK_CHECK(finiteVec(*p));
    TASK_CHECK((*p - sphereA.center).length()
               >= sphereA.radius - 1e-4f);
    TASK_CHECK((*p - sphereB.center).length()
               >= sphereB.radius - 1e-4f);
    // Sphere A is the active resting contact.
    TASK_CHECK((*p - sphereA.center).length()
               <= sphereA.radius + 1e-3f);
    return 0;
}

static int contacts_antiparallel_direction() {
    ISTD::shared_ptr<CalCoreSkeleton> core = makeSingleBoneCore();
    CalSkeleton skeleton(core);
    CalMixer mixer;
    mixer.updateSkeleton(
        &skeleton, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());

    // h^2 * g = -2 along x drives the endpoint exactly to (-1, 0, 0): a
    // 180-degree shortest swing, exercising the deterministic fallback axis.
    const CalVector gravity(-2.0f / (H * H), 0.0f, 0.0f);
    CalSpringChain chain;
    TASK_CHECK(CalSpringChain::create(
                   chain, skeleton, std::vector<int>{0}, CalVector(1, 0, 0),
                   makeConfig(0.0f, 0.0f, gravity))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.reset(skeleton, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), H)
               == CalSpringChain::SUCCESS);

    const CalVector* p = chain.endpoint(0);
    TASK_CHECK(finiteVec(*p));
    TASK_CHECK(nearVec(*p, CalVector(-1, 0, 0), 1e-5f));
    TASK_CHECK(chain.maxLengthError() < 1e-5f);

    // The bone basis must rotate +x onto -x without any NaN.
    const CalVector mapped =
        skeleton.bones[0].absoluteTransform.basis * CalVector(1, 0, 0);
    TASK_CHECK(nearVec(mapped, CalVector(-1, 0, 0), 1e-5f));
    for (size_t i = 0; i < skeleton.bones.size(); ++i) {
        TASK_CHECK(finiteBoneTransform(skeleton.boneTransforms[i]));
    }
    return 0;
}

static int contacts_unsatisfiable_reports_penetration() {
    ISTD::shared_ptr<CalCoreSkeleton> core = makeSingleBoneCore();
    CalSkeleton skeleton(core);
    CalMixer mixer;
    mixer.updateSkeleton(
        &skeleton, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());

    // Sphere centered on the anchor with radius 2: every length-sphere point
    // is inside it, so no feasible configuration exists.
    const CalSpringChain::Contact contact(CalVector(0, 0, 0), 2.0f);
    CalSpringChain chain;
    TASK_CHECK(CalSpringChain::create(
                   chain, skeleton, std::vector<int>{0}, CalVector(1, 0, 0),
                   makeConfig(0.0f, 1.0f, CalVector(0, -10, 0),
                              std::vector<CalSpringChain::Contact>{contact}))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.reset(skeleton, cal3d::RotateTranslate())
               == CalSpringChain::SUCCESS);

    TASK_CHECK(chain.advance(skeleton, cal3d::RotateTranslate(), 0.1f)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(finiteVec(*chain.endpoint(0)));
    TASK_CHECK(finiteVec(*chain.velocity(0)));
    // The hard length constraint survives; the residual penetration is kept.
    TASK_CHECK(chain.maxLengthError() < 1e-5f);
    TASK_CHECK(chain.remainingPenetration() > 0.5f);
    TASK_CHECK(near(chain.remainingPenetration(), 1.0f, 1e-4f));
    return 0;
}

//***********************************************************************//
// pipeline                                                              //
//***********************************************************************//

namespace {
    struct PipelineRig {
        ISTD::shared_ptr<CalCoreSkeleton> core;
        ISTD::shared_ptr<CalCoreAnimation> clip;
    };

    PipelineRig makePipelineCore() {
        PipelineRig rig;
        rig.core = ISTD::make_shared<CalCoreSkeleton>();
        ISTD::shared_ptr<CalCoreBone> rootBone =
            ISTD::make_shared<CalCoreBone>("root");
        // Non-identity, non-unit inverse bind on the root as well.
        rootBone->inverseBindPoseTransform = cal3d::RotateTranslate(
            axisAngle(CalVector(0, 1, 0), 0.2f), CalVector(0.5f, 0.0f, 0.0f));
        rig.core->addCoreBone(rootBone);

        const char* names[4] = {"a", "b", "c", "branch"};
        const int parents[4] = {0, 1, 2, 2};
        const CalVector translations[4] = {
            CalVector(1, 0, 0), CalVector(1, 0, 0), CalVector(1, 0, 0),
            CalVector(0, 1, 0)};
        for (int i = 0; i < 4; ++i) {
            ISTD::shared_ptr<CalCoreBone> bone =
                ISTD::make_shared<CalCoreBone>(names[i], parents[i]);
            bone->relativeTransform.translation = translations[i];
            // Deliberately non-identity, non-unit inverse bind poses.
            bone->inverseBindPoseTransform = cal3d::RotateTranslate(
                axisAngle(CalVector(1, 0, 0), 0.3f * (i + 1)),
                CalVector(0.5f * (i + 1), 0.0f, 0.0f));
            rig.core->addCoreBone(bone);
        }

        rig.clip = ISTD::make_shared<CalCoreAnimation>();
        rig.clip->duration = 1.0f;
        for (int boneId = 0; boneId < 4; ++boneId) {
            CalCoreTrack::KeyframeList keyframes;
            CalVector translation = (boneId == 0)
                ? CalVector()
                : translations[boneId - 1];
            keyframes.push_back(CalCoreKeyframe(
                0.0f, translation,
                axisAngle(CalVector(0, 0, 1), 0.1f * (boneId + 1))));
            rig.clip->tracks.push_back(CalCoreTrack(boneId, keyframes));
        }
        return rig;
    }

    void poseSkeleton(
        const PipelineRig& rig,
        CalSkeleton& skeleton,
        CalMixer& mixer)
    {
        ISTD::shared_ptr<CalAnimation> animation =
            ISTD::make_shared<CalAnimation>(rig.clip, 1.0f, 0);
        mixer.addAnimation(animation);
        mixer.updateSkeleton(
            &skeleton, std::vector<BoneTransformAdjustment>(),
            std::vector<BoneScaleAdjustment>());
    }
}

static int pipeline_chain_branches_and_skinning() {
    PipelineRig rig = makePipelineCore();

    CalSkeleton skeleton(rig.core);
    CalSkeleton reference(rig.core);
    CalMixer mixer;
    CalMixer referenceMixer;
    poseSkeleton(rig, skeleton, mixer);
    poseSkeleton(rig, reference, referenceMixer);

    cal3d::RotateTranslate objectToWorld(
        axisAngle(CalVector(0, 1, 0), 0.7f), CalVector(5, -3, 2));

    CalSpringChain chain;
    TASK_CHECK(CalSpringChain::create(
                   chain, skeleton, std::vector<int>{0, 1, 2, 3},
                   CalVector(1, 0, 0),
                   makeConfig(20.0f, 4.0f, CalVector(0, -9.8f, 0),
                              std::vector<CalSpringChain::Contact>{
                                  CalSpringChain::Contact(
                                      CalVector(5, -8, 2), 0.7f)}))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.reset(skeleton, objectToWorld) == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.segmentCount() == 4);
    TASK_CHECK(chain.advance(skeleton, objectToWorld, 0.1f)
               == CalSpringChain::SUCCESS);

    // The branch bone keeps its animated local pose; only its inherited
    // parent transform changes.
    const cal3d::RotateTranslate& branchLocal =
        skeleton.bones[4].getRelativeTransform();
    const cal3d::RotateTranslate& referenceBranchLocal =
        reference.bones[4].getRelativeTransform();
    TASK_CHECK(nearVec(branchLocal.translation,
                       referenceBranchLocal.translation, 1e-6f));
    TASK_CHECK(near(branchLocal.rotation.x, referenceBranchLocal.rotation.x, 1e-6f));
    TASK_CHECK(near(branchLocal.rotation.y, referenceBranchLocal.rotation.y, 1e-6f));
    TASK_CHECK(near(branchLocal.rotation.z, referenceBranchLocal.rotation.z, 1e-6f));
    TASK_CHECK(near(branchLocal.rotation.w, referenceBranchLocal.rotation.w, 1e-6f));

    const CalVector expectedBranchAbsolute =
        skeleton.bones[2].absoluteTransform * CalVector(0, 1, 0);
    TASK_CHECK(nearVec(skeleton.bones[4].absoluteTransform.translation,
                       expectedBranchAbsolute, 1e-5f));

    // Local segment lengths are preserved between consecutive joints.
    for (int i = 0; i < 3; ++i) {
        const float jointLength =
            (skeleton.bones[i + 1].absoluteTransform.translation
             - skeleton.bones[i].absoluteTransform.translation)
                .length();
        TASK_CHECK(near(jointLength, 1.0f, 1e-5f));
    }
    const CalVector finalTip =
        skeleton.bones[3].absoluteTransform * CalVector(1, 0, 0);
    TASK_CHECK(near(
        (finalTip - skeleton.bones[3].absoluteTransform.translation).length(),
        1.0f, 1e-5f));

    // World endpoints match the corrected skeleton poses exactly.
    const CalVector offsets[4] = {
        CalVector(1, 0, 0), CalVector(1, 0, 0), CalVector(1, 0, 0),
        CalVector(1, 0, 0)};
    for (int i = 0; i < 4; ++i) {
        const CalVector skeletonTip =
            skeleton.bones[i].absoluteTransform * offsets[i];
        const CalVector expectedWorld =
            objectToWorld.rotation * skeletonTip + objectToWorld.translation;
        TASK_CHECK(nearVec(*chain.endpoint(i), expectedWorld, 1e-4f));
        TASK_CHECK(finiteVec(*chain.velocity(i)));
    }
    TASK_CHECK(chain.maxLengthError() < 1e-4f);
    TASK_CHECK(chain.remainingPenetration() < 1e-4f);

    // boneTransforms stay in sync with absolute * inverseBind; the inverse
    // bind poses are non-identity.
    for (size_t i = 0; i < skeleton.bones.size(); ++i) {
        TASK_CHECK(std::fabs(
                       rig.core->coreBones[i]->inverseBindPoseTransform
                           .translation.x)
                   > 0.0f);
        const BoneTransform expected(skeleton.bones[i].absoluteTransform
                                     * skeleton.inverseBindPoseTransforms[i]);
        TASK_CHECK(near(skeleton.boneTransforms[i].rowx.x, expected.rowx.x, 1e-5f));
        TASK_CHECK(near(skeleton.boneTransforms[i].rowx.w, expected.rowx.w, 1e-5f));
        TASK_CHECK(near(skeleton.boneTransforms[i].rowy.y, expected.rowy.y, 1e-5f));
        TASK_CHECK(near(skeleton.boneTransforms[i].rowy.w, expected.rowy.w, 1e-5f));
        TASK_CHECK(near(skeleton.boneTransforms[i].rowz.z, expected.rowz.z, 1e-5f));
        TASK_CHECK(near(skeleton.boneTransforms[i].rowz.w, expected.rowz.w, 1e-5f));
        TASK_CHECK(finiteBoneTransform(skeleton.boneTransforms[i]));
    }

    // Real CalPhysique scalar skinning of a two-bone weighted vertex.
    CalCoreSubmesh::Vertex vertices[1];
    vertices[0].position = CalPoint4(0.3f, 0.2f, 0.1f);
    vertices[0].normal = CalVector4(0, 1, 0);
    CalCoreSubmesh::Influence influences[2] = {
        CalCoreSubmesh::Influence(2, 0.25f, false),
        CalCoreSubmesh::Influence(3, 0.75f, true)};
    CalVector4 skinned[2];
    CalPhysique::calculateVerticesAndNormals_x87(
        &skeleton.boneTransforms[0], 1, vertices, influences, skinned);

    const CalVector4 expected2 =
        applyBoneTransform(skeleton.boneTransforms[2], vertices[0].position);
    const CalVector4 expected3 =
        applyBoneTransform(skeleton.boneTransforms[3], vertices[0].position);
    TASK_CHECK(near(skinned[0].x,
                    0.25f * expected2.x + 0.75f * expected3.x, 1e-5f));
    TASK_CHECK(near(skinned[0].y,
                    0.25f * expected2.y + 0.75f * expected3.y, 1e-5f));
    TASK_CHECK(near(skinned[0].z,
                    0.25f * expected2.z + 0.75f * expected3.z, 1e-5f));

    // Core skeleton and inverse bind poses are untouched.
    for (size_t i = 0; i < rig.core->coreBones.size(); ++i) {
        TASK_CHECK(rig.core->coreBones[i]->inverseBindPoseTransform
                       .translation.x
                   == 0.5f * static_cast<float>(i == 0 ? 1 : i));
    }
    TASK_CHECK(nearVec(rig.core->coreBones[1]->relativeTransform.translation,
                       CalVector(1, 0, 0), 0.0f));
    return 0;
}

static int pipeline_reset_and_next_frame_animation() {
    PipelineRig rig = makePipelineCore();
    CalSkeleton skeleton(rig.core);
    CalSkeleton reference(rig.core);
    CalMixer mixer;
    CalMixer referenceMixer;
    poseSkeleton(rig, skeleton, mixer);
    poseSkeleton(rig, reference, referenceMixer);

    cal3d::RotateTranslate objectToWorld(
        axisAngle(CalVector(0, 1, 0), 0.7f), CalVector(5, -3, 2));

    CalSpringChain chain;
    TASK_CHECK(CalSpringChain::create(
                   chain, skeleton, std::vector<int>{0, 1, 2, 3},
                   CalVector(1, 0, 0),
                   makeConfig(20.0f, 4.0f, CalVector(0, -9.8f, 0)))
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.reset(skeleton, objectToWorld) == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.advance(skeleton, objectToWorld, 0.1f)
               == CalSpringChain::SUCCESS);
    TASK_CHECK(chain.accumulatedSubsteps() == 12);

    // The next mixer update starts from the animations again: the chain's
    // write-back is completely overwritten.
    mixer.updateSkeleton(
        &skeleton, std::vector<BoneTransformAdjustment>(),
        std::vector<BoneScaleAdjustment>());
    for (size_t i = 0; i < skeleton.bones.size(); ++i) {
        TASK_CHECK(nearVec(skeleton.bones[i].absoluteTransform.translation,
                           reference.bones[i].absoluteTransform.translation,
                           1e-5f));
        TASK_CHECK(near(skeleton.bones[i].absoluteTransform.basis.cx.x,
                        reference.bones[i].absoluteTransform.basis.cx.x, 1e-5f));
        TASK_CHECK(near(skeleton.bones[i].absoluteTransform.basis.cz.z,
                        reference.bones[i].absoluteTransform.basis.cz.z, 1e-5f));
    }

    // Explicit reset removes the accumulated inertia: velocities and the
    // time balance are zeroed and endpoints reinitialize from the animated
    // pose.  The lifetime substep counter is preserved.
    const unsigned long long stepsBeforeSecondAdvance =
        chain.accumulatedSubsteps();
    TASK_CHECK(chain.advance(skeleton, objectToWorld, 0.1f)
               == CalSpringChain::SUCCESS);
    const unsigned long long stepsBeforeReset = chain.accumulatedSubsteps();
    TASK_CHECK(stepsBeforeReset - stepsBeforeSecondAdvance == 12);
    TASK_CHECK(stepsBeforeReset >= 12);
    TASK_CHECK(chain.reset(skeleton, objectToWorld) == CalSpringChain::SUCCESS);
    for (size_t i = 0; i < chain.segmentCount(); ++i) {
        TASK_CHECK(nearVec(*chain.velocity(i), CalVector(), 0.0f));
    }
    TASK_CHECK(near(chain.timeBalance(), 0.0f, 1e-9f));
    TASK_CHECK(chain.accumulatedSubsteps() == stepsBeforeReset);

    // Endpoints reinitialize from the fresh animated pose.
    for (int i = 0; i < 4; ++i) {
        const CalVector skeletonTip =
            skeleton.bones[i].absoluteTransform * CalVector(1, 0, 0);
        const CalVector expectedWorld =
            objectToWorld.rotation * skeletonTip + objectToWorld.translation;
        TASK_CHECK(nearVec(*chain.endpoint(i), expectedWorld, 1e-5f));
    }
    return 0;
}

//***********************************************************************//
// registration                                                          //
//***********************************************************************//

const task_case task_cases[] = {
    {"dynamics", "single_step_integration", &dynamics_single_step_integration},
    {"dynamics", "static_equilibrium", &dynamics_static_equilibrium},
    {"dynamics", "damping_decay", &dynamics_damping_decay},
    {"dynamics", "fixed_substep_chunking", &dynamics_fixed_substep_chunking},
    {"dynamics", "two_independent_instances", &dynamics_two_independent_instances},
    {"dynamics", "partial_substep_accumulation", &dynamics_partial_substep_only_accumulates},
    {"dynamics", "twist_preserved", &dynamics_twist_preserved_under_swing},
    {"dynamics", "invalid_inputs", &dynamics_invalid_inputs_leave_state_untouched},
    {"contacts", "solvable_sphere_contact", &contacts_solvable_sphere_contact},
    {"contacts", "two_disjoint_spheres", &contacts_two_disjoint_spheres},
    {"contacts", "antiparallel_direction", &contacts_antiparallel_direction},
    {"contacts", "unsatisfiable_report", &contacts_unsatisfiable_reports_penetration},
    {"pipeline", "branches_endpoints_skinning", &pipeline_chain_branches_and_skinning},
    {"pipeline", "reset_and_next_frame", &pipeline_reset_and_next_frame_animation},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
