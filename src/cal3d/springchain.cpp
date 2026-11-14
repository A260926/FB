//****************************************************************************//
// springchain.cpp                                                            //
// Verlet-style spring chain with fixed substeps, length constraints, sphere  //
// contacts and swing-only skeleton write-back.                               //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cmath>
#include <cstddef>
#include "cal3d/bonetransform.h"
#include "cal3d/bone.h"
#include "cal3d/matrix.h"
#include "cal3d/quaternion.h"
#include "cal3d/skeleton.h"
#include "cal3d/springchain.h"
#include "cal3d/transform.h"

namespace {
    const float RIGID_TOLERANCE = 1e-5f;
    const float DIRECTION_EPSILON = 1e-12f;
    const unsigned PROJECTION_ITERATIONS = 8;
    // Absolute time tolerance (seconds) for substep boundary rounding.  It is
    // orders of magnitude smaller than h, so genuine partial substeps are
    // kept while float rounding of exact h-multiples (e.g. 0.25 s = 30h)
    // cannot make the same total time integrate a different number of
    // substeps depending on how the caller chunks dt.
    const double SUBSTEP_ROUND_EPSILON = 1e-9;

    bool finite(float v) {
        return std::isfinite(v);
    }

    bool finite(const CalVector& v) {
        return finite(v.x) && finite(v.y) && finite(v.z);
    }

    bool finite(const CalQuaternion& q) {
        return finite(q.x) && finite(q.y) && finite(q.z) && finite(q.w);
    }

    bool finite(const cal3d::RotateTranslate& t) {
        return finite(t.rotation) && finite(t.translation);
    }

    bool exactEqualQuaternion(const CalQuaternion& a, const CalQuaternion& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }

    // Bit-exact comparison used to recognize the chain's own write-back:
    // the stored local transform is read back unchanged when CalMixer has
    // not run, so no tolerance is needed and a coincidentally similar fresh
    // animation cannot be mistaken for the write-back.
    bool exactEqualRT(
        const cal3d::RotateTranslate& a,
        const cal3d::RotateTranslate& b)
    {
        return exactEqualQuaternion(a.rotation, b.rotation)
               && exactlyEqual(a.translation, b.translation);
    }

    bool isUnitQuaternion(const CalQuaternion& q) {
        if (!finite(q)) {
            return false;
        }
        const float normSquared = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        return std::fabs(normSquared - 1.0f) <= RIGID_TOLERANCE;
    }

    CalVector safeNormalize(const CalVector& v, const CalVector& fallback) {
        const float length = v.length();
        if (length >= DIRECTION_EPSILON) {
            return v / length;
        }
        return fallback;
    }

    // Deterministic unit vector perpendicular to the (unit) vector v.
    CalVector perpendicularAxis(const CalVector& v) {
        CalVector reference(0.0f, 1.0f, 0.0f);
        if (std::fabs(v.y) >= std::fabs(v.x) && std::fabs(v.y) >= std::fabs(v.z)) {
            reference = CalVector(1.0f, 0.0f, 0.0f);
        }
        CalVector axis = cross(v, reference);
        if (axis.length() < DIRECTION_EPSILON) {
            axis = cross(v, CalVector(0.0f, 0.0f, 1.0f));
        }
        axis.normalize();
        return axis;
    }

    // Shortest-arc unit quaternion rotating unit vector a onto unit vector b.
    // Antiparallel vectors use a deterministic perpendicular axis; the result
    // is always finite and never NaN.
    CalQuaternion shortestArcQuaternion(const CalVector& a, const CalVector& b) {
        const float d = cal3d::clamp(dot(a, b), -1.0f, 1.0f);
        if (d > 1.0f - 1e-7f) {
            return CalQuaternion();
        }
        CalVector axis;
        float angle;
        if (d < -1.0f + 1e-7f) {
            axis = perpendicularAxis(a);
            angle = 3.14159265358979323846f;
        } else {
            axis = cross(a, b);
            axis.normalize();
            angle = acosf(d);
        }
        CalQuaternion q;
        q.setAxisAngle(axis, angle);
        return q;
    }

    // Inverse rotation of an orthonormal basis.
    CalMatrix transposeMatrix(const CalMatrix& m) {
        return CalMatrix(
            CalVector(m.cx.x, m.cy.x, m.cz.x),
            CalVector(m.cx.y, m.cy.y, m.cz.y),
            CalVector(m.cx.z, m.cy.z, m.cz.z));
    }

    CalVector transformPoint(
        const cal3d::RotateTranslate& objectToWorld,
        const CalVector& p)
    {
        return objectToWorld.rotation * p + objectToWorld.translation;
    }
}

CalSpringChain::CalSpringChain()
    : m_initialized(false)
    , m_hasAnimatedSnapshot(false)
    , m_skeletonBoneCount(0)
    , m_rootParentId(-1)
    , m_finalTipLocal()
    , m_config()
    , m_segments()
    , m_animatedLocal()
    , m_lastWrittenLocal()
    , m_balance(0.0)
    , m_substepTotal(0)
    , m_maxLengthError(0.0f)
    , m_remainingPenetration(0.0f)
{}

CalSpringChain::ResultCode CalSpringChain::validateConfig(const Config& config) {
    if (!finite(config.stiffness) || config.stiffness < 0.0f) {
        return INVALID_ARGUMENT;
    }
    if (!finite(config.damping) || config.damping < 0.0f) {
        return INVALID_ARGUMENT;
    }
    if (!finite(config.gravity)) {
        return INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < config.contacts.size(); ++i) {
        const Contact& contact = config.contacts[i];
        if (!finite(contact.center) || !finite(contact.radius)) {
            return INVALID_ARGUMENT;
        }
        if (!(contact.radius > 0.0f)) {
            return INVALID_ARGUMENT;
        }
    }
    return SUCCESS;
}

CalSpringChain::ResultCode CalSpringChain::validateSkeleton(
    const CalSkeleton& skeleton) const
{
    if (!m_initialized || skeleton.bones.size() != m_skeletonBoneCount) {
        return INVALID_SKELETON;
    }
    for (size_t i = 0; i < skeleton.bones.size(); ++i) {
        if (!skeleton.bones[i].scale.isIdentity()) {
            return INVALID_SKELETON;
        }
    }
    return SUCCESS;
}

CalSpringChain::ResultCode CalSpringChain::validatePosesFinite(
    const CalSkeleton& skeleton) const
{
    for (size_t i = 0; i < m_segments.size(); ++i) {
        const Segment& segment = m_segments[i];
        if (!finite(skeleton.bones[segment.boneId].getRelativeTransform())) {
            return INVALID_ARGUMENT;
        }
        const CalVector localOffset = (segment.childBoneId >= 0)
            ? skeleton.bones[segment.childBoneId].getOriginalTranslation()
            : m_finalTipLocal;
        if (!finite(localOffset)) {
            return INVALID_ARGUMENT;
        }
    }
    return SUCCESS;
}

void CalSpringChain::captureAnimatedPose(const CalSkeleton& skeleton) {
    const size_t segmentCount = m_segments.size();
    m_animatedLocal.resize(segmentCount);
    m_lastWrittenLocal.resize(segmentCount);
    for (size_t i = 0; i < segmentCount; ++i) {
        const int boneId = m_segments[i].boneId;
        const cal3d::RotateTranslate current =
            skeleton.bones[boneId].getRelativeTransform();
        if (m_hasAnimatedSnapshot
            && exactEqualRT(current, m_lastWrittenLocal[i]))
        {
            // CalMixer did not run since the previous advance(): the bone
            // still carries the chain's own write-back, so keep the genuine
            // animated reference pose.
        } else {
            // A fresh mixer pose (or the first capture): this is the real
            // animated local transform to target for this frame.
            m_animatedLocal[i] = current;
        }
    }
    m_hasAnimatedSnapshot = true;
}

CalSpringChain::ResultCode CalSpringChain::create(
    CalSpringChain& out,
    const CalSkeleton& skeleton,
    const std::vector<int>& boneIds,
    const CalVector& finalTipLocal,
    const Config& config)
{
    const ResultCode configResult = validateConfig(config);
    if (configResult != SUCCESS) {
        return configResult;
    }
    if (!finite(finalTipLocal)) {
        return INVALID_ARGUMENT;
    }
    if (boneIds.empty()) {
        return INVALID_ARGUMENT;
    }

    const size_t boneCount = skeleton.bones.size();
    for (size_t i = 0; i < boneCount; ++i) {
        if (!skeleton.bones[i].scale.isIdentity()) {
            return INVALID_SKELETON;
        }
    }

    std::vector<Segment> segments;
    segments.reserve(boneIds.size());

    for (size_t i = 0; i < boneIds.size(); ++i) {
        const int boneId = boneIds[i];
        if (boneId < 0 || size_t(boneId) >= boneCount) {
            return INVALID_ARGUMENT;
        }
        for (size_t j = 0; j < i; ++j) {
            if (boneIds[j] == boneId) {
                return INVALID_ARGUMENT;
            }
        }

        CalVector localOffset;
        int childBoneId = -1;
        if (i + 1 < boneIds.size()) {
            childBoneId = boneIds[i + 1];
            if (childBoneId < 0 || size_t(childBoneId) >= boneCount) {
                return INVALID_ARGUMENT;
            }
            // Consecutive parent-to-child chain.
            if (skeleton.bones[childBoneId].parentId != boneId) {
                return INVALID_ARGUMENT;
            }
            localOffset = skeleton.bones[childBoneId].getOriginalTranslation();
        } else {
            localOffset = finalTipLocal;
        }

        if (!finite(localOffset)) {
            return INVALID_ARGUMENT;
        }
        const float length = localOffset.length();
        if (!(length > 0.0f)) {
            return INVALID_ARGUMENT;
        }

        Segment segment;
        segment.boneId = boneId;
        segment.childBoneId = childBoneId;
        segment.length = length;
        segment.endpoint = CalVector();
        segment.velocity = CalVector();
        segments.push_back(segment);
    }

    out.m_initialized = true;
    out.m_hasAnimatedSnapshot = false;
    out.m_skeletonBoneCount = boneCount;
    out.m_rootParentId = skeleton.bones[boneIds.front()].parentId;
    out.m_finalTipLocal = finalTipLocal;
    out.m_config = config;
    out.m_segments.swap(segments);
    out.m_animatedLocal.clear();
    out.m_lastWrittenLocal.clear();
    out.m_balance = 0.0;
    out.m_substepTotal = 0;
    out.m_maxLengthError = 0.0f;
    out.m_remainingPenetration = 0.0f;
    return SUCCESS;
}

CalSpringChain::ResultCode CalSpringChain::configure(const Config& config) {
    if (!m_initialized) {
        return NOT_INITIALIZED;
    }
    const ResultCode configResult = validateConfig(config);
    if (configResult != SUCCESS) {
        return configResult;
    }
    m_config = config;
    return SUCCESS;
}

const CalVector* CalSpringChain::endpoint(size_t i) const {
    if (!m_initialized || i >= m_segments.size()) {
        return 0;
    }
    return &m_segments[i].endpoint;
}

const CalVector* CalSpringChain::velocity(size_t i) const {
    if (!m_initialized || i >= m_segments.size()) {
        return 0;
    }
    return &m_segments[i].velocity;
}

CalSpringChain::ResultCode CalSpringChain::reset(
    CalSkeleton& skeleton,
    const cal3d::RotateTranslate& objectToWorld)
{
    if (!m_initialized) {
        return NOT_INITIALIZED;
    }
    const ResultCode skeletonResult = validateSkeleton(skeleton);
    if (skeletonResult != SUCCESS) {
        return skeletonResult;
    }
    if (!isUnitQuaternion(objectToWorld.rotation) ||
        !finite(objectToWorld.translation))
    {
        return INVALID_ARGUMENT;
    }
    {
        const ResultCode finiteResult = validatePosesFinite(skeleton);
        if (finiteResult != SUCCESS) {
            return finiteResult;
        }
    }

    // The current skeleton pose is the genuine animated reference for the
    // freshly reset chain.  Invalidate write-back tracking first so a mixer
    // pose that happens to equal an earlier write-back is still captured as
    // the new reference rather than mistaken for an untouched write-back.
    m_hasAnimatedSnapshot = false;
    captureAnimatedPose(skeleton);

    for (size_t i = 0; i < m_segments.size(); ++i) {
        const Segment& segment = m_segments[i];
        const CalBone& bone = skeleton.bones[segment.boneId];

        const CalVector& localOffset = (segment.childBoneId >= 0)
            ? skeleton.bones[segment.childBoneId].getOriginalTranslation()
            : m_finalTipLocal;

        const CalVector tipInSkeleton = bone.absoluteTransform * localOffset;
        m_segments[i].endpoint = transformPoint(objectToWorld, tipInSkeleton);
        m_segments[i].velocity = CalVector();
    }

    // reset() initializes endpoints and zeroes velocity and the time balance.
    // The cumulative substep counter is a lifetime statistic and is kept.
    m_balance = 0.0;

    // Report residual errors against the freshly initialized pose.  The
    // animated pose satisfies every length constraint up to float precision.
    float maxLengthError = 0.0f;
    float remainingPenetration = 0.0f;
    CalVector rootAnchor;
    {
        const cal3d::Transform rootParent = (m_rootParentId < 0)
            ? cal3d::Transform()
            : skeleton.bones[m_rootParentId].absoluteTransform;
        const CalVector rootOriginSkeleton =
            rootParent * skeleton.bones[m_segments.front().boneId]
                           .getRelativeTransform().translation;
        rootAnchor = transformPoint(objectToWorld, rootOriginSkeleton);
    }
    for (size_t i = 0; i < m_segments.size(); ++i) {
        const CalVector& anchor = (i == 0)
            ? rootAnchor
            : m_segments[i - 1].endpoint;
        const float distance = (m_segments[i].endpoint - anchor).length();
        const float error = std::fabs(distance - m_segments[i].length);
        if (error > maxLengthError) {
            maxLengthError = error;
        }
        for (size_t c = 0; c < m_config.contacts.size(); ++c) {
            const Contact& contact = m_config.contacts[c];
            const float clearance =
                (m_segments[i].endpoint - contact.center).length() - contact.radius;
            if (clearance < 0.0f && -clearance > remainingPenetration) {
                remainingPenetration = -clearance;
            }
        }
    }
    m_maxLengthError = maxLengthError;
    m_remainingPenetration = remainingPenetration;
    return SUCCESS;
}

CalSpringChain::ResultCode CalSpringChain::advance(
    CalSkeleton& skeleton,
    const cal3d::RotateTranslate& objectToWorld,
    float dt)
{
    if (!m_initialized) {
        return NOT_INITIALIZED;
    }
    const ResultCode skeletonResult = validateSkeleton(skeleton);
    if (skeletonResult != SUCCESS) {
        return skeletonResult;
    }
    if (!isUnitQuaternion(objectToWorld.rotation) ||
        !finite(objectToWorld.translation))
    {
        return INVALID_ARGUMENT;
    }
    if (!finite(dt) || dt < 0.0f || dt > 1.0f) {
        return INVALID_ARGUMENT;
    }

    const size_t segmentCount = m_segments.size();

    // All read-only validation completes before any state mutation so a
    // failure leaves the chain and skeleton byte-for-byte untouched.
    {
        const ResultCode finiteResult = validatePosesFinite(skeleton);
        if (finiteResult != SUCCESS) {
            return finiteResult;
        }
    }

    // Resolve the genuine animated local pose for this frame.  If CalMixer
    // did not run since the last advance(), the bones still hold the chain's
    // own write-back and the cached animated reference is reused, which keeps
    // the integration independent of how the caller chunks dt.
    captureAnimatedPose(skeleton);

    std::vector<CalVector> localOffsets(segmentCount);
    for (size_t i = 0; i < segmentCount; ++i) {
        const Segment& segment = m_segments[i];
        localOffsets[i] = (segment.childBoneId >= 0)
            ? skeleton.bones[segment.childBoneId].getOriginalTranslation()
            : m_finalTipLocal;
    }

    // Fixed-substep accounting.  Legal time is never discarded: anything
    // short of a full substep stays in the balance.  Counting uses the exact
    // mathematical 1/120 in double precision: the float SUBSTEP used for the
    // actual integration rounds slightly high, so dividing by it would make
    // exact multiples (e.g. 0.25 s = 30h) count one substep short.  The
    // rounding epsilon absorbs float representation error at substep
    // boundaries while keeping a genuine partial substep in the balance.
    m_balance += static_cast<double>(dt);
    const double h = 1.0 / 120.0;
    long substepsToRun = static_cast<long>(
        std::floor((m_balance + SUBSTEP_ROUND_EPSILON) / h));
    if (substepsToRun < 0) {
        substepsToRun = 0;
    }
    m_balance -= static_cast<double>(substepsToRun) * h;
    if (m_balance < SUBSTEP_ROUND_EPSILON) {
        m_balance = 0.0;
    }

    const float hf = SUBSTEP;
    const float dampingDecay = expf(-m_config.damping * hf);
    const CalQuaternion inverseObjectRotation = -objectToWorld.rotation;

    // The root anchor does not move during the call: the root bone's parent
    // is outside the chain, so its animated world pose (and the root bone's
    // fixed animated local translation) stay fixed.  The anchor is the root
    // chain bone's own origin.
    CalVector rootAnchor;
    {
        const cal3d::Transform rootParent = (m_rootParentId < 0)
            ? cal3d::Transform()
            : skeleton.bones[m_rootParentId].absoluteTransform;
        const CalVector rootOriginSkeleton =
            rootParent * m_animatedLocal.front().translation;
        rootAnchor = transformPoint(objectToWorld, rootOriginSkeleton);
    }

    for (long step = 0; step < substepsToRun; ++step) {
        for (size_t i = 0; i < segmentCount; ++i) {
            Segment& segment = m_segments[i];
            CalBone& bone = skeleton.bones[segment.boneId];

            // Anchor from the already updated parent pose.
            CalVector anchorWorld;
            cal3d::Transform parentAbsolute;
            if (i == 0) {
                anchorWorld = rootAnchor;
                if (m_rootParentId < 0) {
                    parentAbsolute = cal3d::Transform();
                } else {
                    parentAbsolute = skeleton.bones[m_rootParentId].absoluteTransform;
                }
            } else {
                anchorWorld = m_segments[i - 1].endpoint;
                parentAbsolute =
                    skeleton.bones[m_segments[i - 1].boneId].absoluteTransform;
            }

            // Goal: updated parent pose times this frame's fixed animated
            // local transform, applied to the fixed-length local offset.
            const cal3d::RotateTranslate& goalLocal = m_animatedLocal[i];
            const CalVector goalTipSkeleton =
                parentAbsolute * (goalLocal * localOffsets[i]);
            const CalVector goalWorld =
                transformPoint(objectToWorld, goalTipSkeleton);

            const CalVector oldEndpoint = segment.endpoint;
            const CalVector trialVelocity =
                (segment.velocity
                 + hf * (m_config.stiffness * (goalWorld - oldEndpoint)
                         + m_config.gravity))
                * dampingDecay;
            CalVector trialPosition = oldEndpoint + hf * trialVelocity;

            // Joint projection.  Contact spheres are resolved first, then
            // the segment length (the structural hard constraint) is
            // re-imposed, so an unsatisfiable configuration leaves the
            // endpoint on the length sphere and reports the unavoidable
            // residual penetration instead of violating the segment length.
            // Iteration reconciles competing, disjoint spheres.
            CalVector fallbackDirection =
                safeNormalize(goalWorld - anchorWorld, oldEndpoint - anchorWorld);
            fallbackDirection =
                safeNormalize(fallbackDirection, CalVector(1.0f, 0.0f, 0.0f));

            for (unsigned iteration = 0; iteration < PROJECTION_ITERATIONS; ++iteration) {
                for (size_t c = 0; c < m_config.contacts.size(); ++c) {
                    const Contact& contact = m_config.contacts[c];
                    CalVector away = trialPosition - contact.center;
                    const float awayLength = away.length();
                    if (awayLength < contact.radius) {
                        const CalVector awayDirection =
                            safeNormalize(away, fallbackDirection);
                        trialPosition =
                            contact.center + awayDirection * contact.radius;
                    }
                }

                const CalVector radial = trialPosition - anchorWorld;
                const CalVector radialDirection =
                    safeNormalize(radial, fallbackDirection);
                trialPosition = anchorWorld + radialDirection * segment.length;

                bool stillPenetrating = false;
                for (size_t c = 0; c < m_config.contacts.size(); ++c) {
                    const Contact& contact = m_config.contacts[c];
                    if ((trialPosition - contact.center).length()
                        < contact.radius - DIRECTION_EPSILON)
                    {
                        stillPenetrating = true;
                        break;
                    }
                }
                if (!stillPenetrating) {
                    break;
                }
            }

            segment.endpoint = trialPosition;
            // Velocity is recomputed from the corrected displacement so the
            // constraint response is never accumulated as uncorrected speed.
            segment.velocity = (trialPosition - oldEndpoint) / hf;

            // Shortest-swing correction of the animated rotation, preserving
            // the animated twist around the segment axis.  The swing is built
            // against the absolute rotation the animated local pose would
            // have under the already updated parent, never against a basis
            // corrected during an earlier substep.
            const CalMatrix animatedAbsoluteBasis =
                parentAbsolute.basis * CalMatrix(goalLocal.rotation);
            const CalVector animatedSkeletonDirection = safeNormalize(
                animatedAbsoluteBasis * localOffsets[i],
                fallbackDirection);
            const CalVector worldDirection =
                safeNormalize(trialPosition - anchorWorld, fallbackDirection);
            const CalVector newSkeletonDirection = safeNormalize(
                inverseObjectRotation * worldDirection,
                animatedSkeletonDirection);

            const CalQuaternion swing = shortestArcQuaternion(
                animatedSkeletonDirection, newSkeletonDirection);
            const CalMatrix newAbsoluteBasis =
                CalMatrix(swing) * animatedAbsoluteBasis;
            const CalMatrix localBasis =
                transposeMatrix(parentAbsolute.basis) * newAbsoluteBasis;

            cal3d::RotateTranslate newLocal = m_animatedLocal[i];
            newLocal.rotation = CalQuaternion(localBasis);
            bone.setRelativeTransform(newLocal);
            bone.calculateAbsolutePose(skeleton.bones.data());
            skeleton.boneTransforms[segment.boneId] = BoneTransform(
                bone.absoluteTransform * skeleton.inverseBindPoseTransforms[segment.boneId]);
            // Record the exact write-back so the next advance() can tell a
            // fresh mixer pose from the chain's own untouched local pose.
            m_lastWrittenLocal[i] = newLocal;
        }

        // Non-chain branches only inherit updated parent transforms; their
        // local poses are not modified.
        for (size_t boneId = 0; boneId < skeleton.bones.size(); ++boneId) {
            bool inChain = false;
            for (size_t i = 0; i < segmentCount; ++i) {
                if (m_segments[i].boneId == static_cast<int>(boneId)) {
                    inChain = true;
                    break;
                }
            }
            if (inChain) {
                continue;
            }
            CalBone& other = skeleton.bones[boneId];
            other.calculateAbsolutePose(skeleton.bones.data());
            skeleton.boneTransforms[boneId] = BoneTransform(
                other.absoluteTransform * skeleton.inverseBindPoseTransforms[boneId]);
        }

        ++m_substepTotal;
    }

    // Error reporting against the actual anchors after the latest state.
    float maxLengthError = 0.0f;
    float remainingPenetration = 0.0f;
    for (size_t i = 0; i < segmentCount; ++i) {
        const CalVector& anchor = (i == 0)
            ? rootAnchor
            : m_segments[i - 1].endpoint;
        const float distance = (m_segments[i].endpoint - anchor).length();
        const float error = std::fabs(distance - m_segments[i].length);
        if (error > maxLengthError) {
            maxLengthError = error;
        }
        for (size_t c = 0; c < m_config.contacts.size(); ++c) {
            const Contact& contact = m_config.contacts[c];
            const float clearance =
                (m_segments[i].endpoint - contact.center).length() - contact.radius;
            if (clearance < 0.0f && -clearance > remainingPenetration) {
                remainingPenetration = -clearance;
            }
        }
    }
    m_maxLengthError = maxLengthError;
    m_remainingPenetration = remainingPenetration;

    return SUCCESS;
}
