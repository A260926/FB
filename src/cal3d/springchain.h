//****************************************************************************//
// springchain.h                                                              //
// Verlet-style spring chain with its own motion state, acting directly on a  //
// CalSkeleton.  The chain owns world-space segment endpoints, velocities and //
// a fixed-timestep time balance; animation, core skeleton and inverse bind   //
// poses are never modified.                                                  //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#pragma once

#include <vector>
#include "cal3d/global.h"
#include "cal3d/transform.h"
#include "cal3d/vector.h"

class CalSkeleton;

class CAL3D_API CalSpringChain {
public:
    // World-space collision sphere.  Radius must be strictly positive and
    // every component must be finite.
    struct Contact {
        Contact()
            : center()
            , radius(0.0f)
        {}

        Contact(const CalVector& center_, float radius_)
            : center(center_)
            , radius(radius_)
        {}

        CalVector center;
        float radius;
    };

    struct Config {
        Config()
            : stiffness(0.0f)
            , damping(0.0f)
            , gravity()
            , contacts()
        {}

        // Non-negative spring pull towards the animated goal.
        float stiffness;
        // Non-negative exponential velocity damping coefficient.
        float damping;
        // World-space gravity acceleration applied every substep.
        CalVector gravity;
        // World-space collision spheres; expected to be mutually disjoint.
        std::vector<Contact> contacts;
    };

    // All entry points return a ResultCode instead of throwing.  On any
    // error code the chain state and the CalSkeleton are left byte-for-byte
    // untouched.  Unsatisfiable contacts are not an error: advance() keeps
    // finite results and reports the residual through
    // remainingPenetration().
    enum ResultCode {
        SUCCESS = 0,
        // Invalid chain specification or parameters: bad/duplicate indices,
        // discontinuous bones, non-positive segment length, non-finite data,
        // negative stiffness/damping, invalid contact sphere, non-rigid
        // object transform, or dt outside [0, 1].
        INVALID_ARGUMENT = 1,
        // The skeleton is scaled, or its bone count does not match the one
        // the chain was created with.
        INVALID_SKELETON = 2,
        // advance()/reset()/configure() called before a successful create().
        NOT_INITIALIZED = 3
    };

    CalSpringChain();

    // Validates the chain specification against the skeleton and builds the
    // chain.  boneIds are ordered parent-to-child and must be consecutive in
    // the skeleton parent hierarchy (each bone's parentId is the preceding
    // entry).  Segment lengths are the core local translations between
    // consecutive bones; the final segment runs from the last bone to
    // finalTipLocal, expressed in that bone's local frame.  All segment
    // lengths must be strictly positive.  The skeleton must carry no scale.
    // On failure returns an error code and leaves out untouched.
    static ResultCode create(
        CalSpringChain& out,
        const CalSkeleton& skeleton,
        const std::vector<int>& boneIds,
        const CalVector& finalTipLocal,
        const Config& config);

    // Replaces the dynamics/contact configuration.  On failure the chain and
    // its state are left untouched.
    ResultCode configure(const Config& config);

    // Initializes every world endpoint from the skeleton's current animated
    // pose (run CalMixer first), and zeroes velocities and the time balance.
    // objectToWorld must be a finite rigid transform (unit quaternion).
    ResultCode reset(
        CalSkeleton& skeleton,
        const cal3d::RotateTranslate& objectToWorld);

    // Advances the chain.  Call once per frame after CalMixer.  dt is in
    // seconds and must be in [0, 1]; legal time is never discarded, it is
    // accumulated against fixed h = 1/120 s substeps.  A remainder smaller
    // than one substep only updates the balance and never touches the
    // skeleton.  Within one call the animated local pose is fixed; each
    // substep recomputes anchors and goals from the already updated parent
    // bone poses.  After solving, chain bones receive a shortest-swing
    // rotation correction (animated twist preserved) written back in parent
    // local space; all absoluteTransform/boneTransform entries, including
    // non-chain branches, are resynchronized.  Core skeleton, animations and
    // inverse bind poses are never modified.
    ResultCode advance(
        CalSkeleton& skeleton,
        const cal3d::RotateTranslate& objectToWorld,
        float dt);

    bool initialized() const {
        return m_initialized;
    }

    size_t segmentCount() const {
        return m_segments.size();
    }

    // Read-only world endpoint of segment i (one entry per segment, root to
    // tip).  Returns null when i is out of range or the chain is not set up.
    const CalVector* endpoint(size_t i) const;
    // Read-only world velocity of segment i.
    const CalVector* velocity(size_t i) const;

    // Accumulated time below one full substep, in seconds, always in [0, h).
    float timeBalance() const {
        return static_cast<float>(m_balance);
    }

    // Total number of fixed substeps executed since create().
    unsigned long long accumulatedSubsteps() const {
        return m_substepTotal;
    }

    // Largest ||endpoint - anchor| - segmentLength| after the latest
    // reset()/advance(), over every segment.
    float maxLengthError() const {
        return m_maxLengthError;
    }

    // Largest residual penetration of any endpoint into any contact sphere
    // after the latest reset()/advance(); zero when everything is separated.
    float remainingPenetration() const {
        return m_remainingPenetration;
    }

    // Fixed integration substep, in seconds.
    static constexpr float SUBSTEP = 1.0f / 120.0f;

private:
    struct Segment {
        int boneId;          // bone owning the segment's local frame
        int childBoneId;     // next chain bone, or -1 for the final segment
        float length;        // strictly positive
        CalVector endpoint;  // world-space segment tip
        CalVector velocity;  // world-space endpoint velocity
    };

    static ResultCode validateConfig(const Config& config);
    ResultCode validateSkeleton(const CalSkeleton& skeleton) const;
    // Read-only finiteness check of the chain bones' current local poses and
    // of every fixed local offset, run before any state is mutated.
    ResultCode validatePosesFinite(const CalSkeleton& skeleton) const;

    // Snapshot handling.  The chain writes a swing-corrected local rotation
    // back into the chain bones, so on the next advance() the accumulator no
    // longer holds the pure animated pose unless CalMixer ran in between.
    // m_animatedLocal keeps the genuine animated reference; m_lastWrittenLocal
    // records exactly what the chain wrote, letting advance() tell a fresh
    // mixer pose from its own untouched write-back.  This is what makes the
    // same total time produce identical substep state regardless of how dt
    // is split across calls.
    void captureAnimatedPose(const CalSkeleton& skeleton);

    bool m_initialized;
    bool m_hasAnimatedSnapshot;
    size_t m_skeletonBoneCount;
    int m_rootParentId;
    CalVector m_finalTipLocal;
    Config m_config;
    std::vector<Segment> m_segments;
    std::vector<cal3d::RotateTranslate> m_animatedLocal;
    std::vector<cal3d::RotateTranslate> m_lastWrittenLocal;
    double m_balance;
    unsigned long long m_substepTotal;
    float m_maxLengthError;
    float m_remainingPenetration;
};
