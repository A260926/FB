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
#include "cal3d/transform.h"

class CalAnimation;
class CalSkeleton;

namespace cal3d {
class CalStreamBlockCache;

// Resolves streamed track poses during a mixer blend pass.  In-memory
// animations never consult a provider.
struct IStreamPoseProvider {
    virtual ~IStreamPoseProvider() {}
    virtual bool getTrackTransform(
        CalAnimation& animation,
        unsigned coreBoneId,
        float time,
        RotateTranslate& result) = 0;
};
}

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

    void updateSkeleton(
        CalSkeleton* skeleton,
        const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
        const std::vector<BoneScaleAdjustment>& boneScaleAdjustments);

    // Streaming counterpart of updateSkeleton().  Streamed animations sample
    // through the shared block cache; ordinary in-memory animations can be
    // mixed in the same call.  Both entry points run the same blending code.
    // Returns false (without touching the skeleton or the cache state) when a
    // required block cannot be made resident.
    bool updateSkeletonStreamed(
        CalSkeleton* skeleton,
        const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
        const std::vector<BoneScaleAdjustment>& boneScaleAdjustments,
        cal3d::CalStreamBlockCache& cache);

private:
    void applyBoneAdjustments(
        CalSkeleton* skeleton,
        const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
        const std::vector<BoneScaleAdjustment>& boneScaleAdjustments);

    // Single blending implementation shared by both public entry points.
    // A null streamProvider means every animation must be in-memory.
    bool blendSkeleton(
        CalSkeleton* skeleton,
        const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
        const std::vector<BoneScaleAdjustment>& boneScaleAdjustments,
        cal3d::IStreamPoseProvider* streamProvider);

    typedef std::list<ISTD::shared_ptr<CalAnimation>> AnimationList;
    AnimationList activeAnimations;
};
