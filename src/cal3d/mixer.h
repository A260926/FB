//****************************************************************************//
// mixer.h                                                                    //
// Copyright (C) 2001, 2002 Bruno 'Beosil' Heidelberger                       //
// Copyright (C) 2004 Mekensleep <licensing@mekensleep.com>                   //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#pragma once

#include <list>
#include "cal3d/animation.h"
#include "cal3d/quaternion.h"

class CalAnimation;
class CalSkeleton;
class CalStreamClipCache;

struct BoneTransformAdjustment {
    unsigned boneId;

    // Relative to the parent frame of reference.
    CalQuaternion localOri;

    // The adjustment is a highest priority "replace" animation for the bone.  Lower priority
    // animations for the bone, including other replace animations, will be attenuated by 1 - rampValue.
    float rampValue;
};

struct BoneScaleAdjustment {
    BoneScaleAdjustment(unsigned i, const CalVector& scale)
        : boneId(i)
        , scale(scale)
    {}

    unsigned boneId;
    CalVector scale;
};

class CAL3D_API CalMixer {
public:
    void addAnimation(const ISTD::shared_ptr<CalAnimation>& animation);
    void removeAnimation(const ISTD::shared_ptr<CalAnimation>& animation);

    // Blend active in-memory animations.  Throws (and leaves the skeleton
    // untouched) if any active animation has a streamed source; use
    // updateSkeletonStreamed for those.
    void updateSkeleton(
        CalSkeleton* skeleton,
        const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
        const std::vector<BoneScaleAdjustment>& boneScaleAdjustments);

    // Blend active in-memory *and* streamed animations, fetching every block
    // required by this frame from the shared cache before touching the
    // skeleton.  Returns false without modifying the skeleton or animation
    // times if the cache cannot supply all required blocks.  streamCache must
    // be non-null when a streamed animation is active.
    bool updateSkeletonStreamed(
        CalSkeleton* skeleton,
        const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
        const std::vector<BoneScaleAdjustment>& boneScaleAdjustments,
        CalStreamClipCache* streamCache);

    bool hasStreamedAnimation() const;

private:
    // One track sample in blend order.  Both entry points reduce to this
    // representation; a single routine does the actual blending.
    struct PoseSample {
        unsigned boneId;
        CalQuaternion rotation;
        CalVector translation;
        float weight;
        float subsequentAttenuation;
    };

    void appendMemorySamples(
        const ISTD::shared_ptr<CalAnimation>& animation,
        size_t boneCount,
        std::vector<PoseSample>& samples) const;

    static void blendSamples(CalSkeleton* skeleton, const std::vector<PoseSample>& samples);

    void finishSkeleton(
        CalSkeleton* skeleton,
        const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
        const std::vector<BoneScaleAdjustment>& boneScaleAdjustments,
        const std::vector<PoseSample>& samples);

    void applyBoneAdjustments(
        CalSkeleton* skeleton,
        const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
        const std::vector<BoneScaleAdjustment>& boneScaleAdjustments);

    typedef std::list<ISTD::shared_ptr<CalAnimation>> AnimationList;
    AnimationList activeAnimations;
};
