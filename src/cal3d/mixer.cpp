//****************************************************************************//
// mixer.cpp                                                                  //
// Copyright (C) 2001, 2002 Bruno 'Beosil' Heidelberger                       //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <map>

#include "cal3d/error.h"
#include "cal3d/mixer.h"
#include "cal3d/corebone.h"
#include "cal3d/coreanimation.h"
#include "cal3d/coretrack.h"
#include "cal3d/corekeyframe.h"
#include "cal3d/skeleton.h"
#include "cal3d/bone.h"
#include "cal3d/animation.h"
#include "cal3d/streamclip.h"

void CalMixer::addAnimation(const ISTD::shared_ptr<CalAnimation>& animation) {
    AnimationList::iterator i = activeAnimations.begin();
    while (i != activeAnimations.end() && animation->priority < (*i)->priority) {
        ++i;
    }
    activeAnimations.insert(i, animation);
}

void CalMixer::removeAnimation(const ISTD::shared_ptr<CalAnimation>& animation) {
    activeAnimations.remove(animation);
}

bool CalMixer::hasStreamedAnimation() const {
    for (auto i = activeAnimations.begin(); i != activeAnimations.end(); ++i) {
        if ((*i)->isStreamed()) {
            return true;
        }
    }
    return false;
}

void CalMixer::appendMemorySamples(
    const ISTD::shared_ptr<CalAnimation>& animation,
    size_t boneCount,
    std::vector<PoseSample>& samples
) const {
    if (!animation->coreAnimation) {
        return;
    }

    // Same traversal, weights and attenuation as the historical implementation.
    const float perAnimationWeight = animation->weight * animation->rampValue;
    const float subsequentAttenuation =
        animation->priority != 0 ? animation->rampValue : 0.0f;

    const auto& tracks = animation->coreAnimation->tracks;
    for (auto track = tracks.begin(); track != tracks.end(); ++track) {
        if (track->coreBoneId >= boneCount) {
            continue;
        }

        const cal3d::RotateTranslate transform = track->getCurrentTransform(animation->time);

        PoseSample sample;
        sample.boneId = track->coreBoneId;
        sample.rotation = transform.rotation;
        sample.translation = transform.translation;
        sample.weight = perAnimationWeight;
        sample.subsequentAttenuation = subsequentAttenuation;
        samples.push_back(sample);
    }
}

void CalMixer::blendSamples(CalSkeleton* skeleton, const std::vector<PoseSample>& samples) {
    auto& bones = skeleton->bones;
    for (size_t i = 0; i < samples.size(); ++i) {
        const PoseSample& sample = samples[i];
        bones[sample.boneId].blendPose(
            sample.weight,
            cal3d::RotateTranslate(sample.rotation, sample.translation),
            sample.subsequentAttenuation);
    }
}

void CalMixer::updateSkeleton(
    CalSkeleton* skeleton,
    const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
    const std::vector<BoneScaleAdjustment>& boneScaleAdjustments
) {
    // Explicitly reject streamed sources rather than silently skipping them.
    // Nothing has been touched yet, so the skeleton is unchanged on error.
    if (hasStreamedAnimation()) {
        CalError::setLastError(
            CalError::INVALID_MIXER_TYPE,
            __FILE__,
            __LINE__,
            "CalMixer::updateSkeleton cannot sample a streamed animation; use updateSkeletonStreamed");
        cal3d::verify(
            false,
            "CalMixer::updateSkeleton cannot sample a streamed animation; use updateSkeletonStreamed");
        return;
    }

    std::vector<PoseSample> samples;
    for (auto itaa = activeAnimations.begin(); itaa != activeAnimations.end(); ++itaa) {
        appendMemorySamples(*itaa, skeleton->bones.size(), samples);
    }

    finishSkeleton(skeleton, boneTransformAdjustments, boneScaleAdjustments, samples);
}

bool CalMixer::updateSkeletonStreamed(
    CalSkeleton* skeleton,
    const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
    const std::vector<BoneScaleAdjustment>& boneScaleAdjustments,
    CalStreamClipCache* streamCache
) {
    // Classify the active animations into the two kinds of source and build
    // the per-frame working set in blend order.  Only a repeated *block* is
    // collapsed; a repeated animation is still blended at its proper place.
    struct StreamedEntry {
        ISTD::shared_ptr<CalAnimation> animation;
        uint32_t blockIndex;
    };
    std::vector<StreamedEntry> streamedEntries;
    std::map<CalStreamClipBlockKey, size_t> requestIndex;

    std::vector<CalStreamClipBlockRef> requests;
    std::vector<CalStreamClipBlockKey> requestOrder;

    bool needsStreamData = false;
    for (auto itaa = activeAnimations.begin(); itaa != activeAnimations.end(); ++itaa) {
        const ISTD::shared_ptr<CalAnimation>& animation = *itaa;
        if (!animation->isStreamed()) {
            continue;
        }

        needsStreamData = true;
        StreamedEntry entry;
        entry.animation = animation;
        entry.blockIndex = animation->streamBlockIndex();
        streamedEntries.push_back(entry);

        CalStreamClipBlockKey key(animation->streamClip->identity(), entry.blockIndex);
        if (requestIndex.find(key) == requestIndex.end()) {
            requestIndex[key] = requests.size();
            requestOrder.push_back(key);
            requests.push_back(CalStreamClipBlockRef(animation->streamClip, entry.blockIndex));
        }
    }

    if (needsStreamData && !streamCache) {
        return false;
    }

    // Acquire all missing blocks atomically.  On failure nothing downstream
    // runs: no skeleton reset, no time changes, no partial publish.
    std::vector<CalStreamClip::DecodedBlockPtr> blocks;
    if (!requests.empty()) {
        if (!streamCache->acquire(requests, blocks)) {
            return false;
        }
    }

    // Decode the requested block for each streamed animation.
    std::vector<const CalStreamClip::DecodedBlock*> animationBlocks;
    animationBlocks.reserve(streamedEntries.size());
    for (size_t i = 0; i < streamedEntries.size(); ++i) {
        CalStreamClipBlockKey key(streamedEntries[i].animation->streamClip->identity(), streamedEntries[i].blockIndex);
        size_t index = requestIndex.find(key)->second;
        animationBlocks.push_back(blocks[index].get());
    }

    // Now, and only now, start mutating observable state.
    std::vector<PoseSample> samples;
    samples.reserve(64);

    size_t streamedCursor = 0;
    const size_t boneCount = skeleton->bones.size();
    for (auto itaa = activeAnimations.begin(); itaa != activeAnimations.end(); ++itaa) {
        const ISTD::shared_ptr<CalAnimation>& animation = *itaa;
        if (!animation->isStreamed()) {
            appendMemorySamples(animation, boneCount, samples);
            continue;
        }

        const CalStreamClip::DecodedBlock* block = animationBlocks[streamedCursor++];

        const float perAnimationWeight = animation->weight * animation->rampValue;
        const float subsequentAttenuation =
            animation->priority != 0 ? animation->rampValue : 0.0f;

        for (size_t i = 0; i < block->tracks.size(); ++i) {
            const CalStreamClip::DecodedTrack& track = block->tracks[i];
            if (track.boneId >= boneCount) {
                continue;
            }

            const cal3d::RotateTranslate transform =
                CalStreamClip::sampleTrack(track, animation->time);

            PoseSample sample;
            sample.boneId = track.boneId;
            sample.rotation = transform.rotation;
            sample.translation = transform.translation;
            sample.weight = perAnimationWeight;
            sample.subsequentAttenuation = subsequentAttenuation;
            samples.push_back(sample);
        }
    }

    finishSkeleton(skeleton, boneTransformAdjustments, boneScaleAdjustments, samples);
    return true;
}

void CalMixer::finishSkeleton(
    CalSkeleton* skeleton,
    const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
    const std::vector<BoneScaleAdjustment>& boneScaleAdjustments,
    const std::vector<PoseSample>& samples
) {
    skeleton->resetPose();

    // The bone adjustments are "replace" so they have to go first, giving them
    // highest priority and full influence.  Subsequent animations affecting the same bones,
    // including subsequent replace animations, will have their influence attenuated appropriately.
    applyBoneAdjustments(skeleton, boneTransformAdjustments, boneScaleAdjustments);

    blendSamples(skeleton, samples);

    skeleton->calculateAbsolutePose();
}

void CalMixer::applyBoneAdjustments(
    CalSkeleton* skeleton,
    const std::vector<BoneTransformAdjustment>& boneTransformAdjustments,
    const std::vector<BoneScaleAdjustment>& boneScaleAdjustments
) {
    CalSkeleton::BoneArray& bones = skeleton->bones;

    for (size_t i = 0; i < boneTransformAdjustments.size(); ++i) {
        const BoneTransformAdjustment& ba = boneTransformAdjustments[i];
        CalBone& bo = bones[ba.boneId];
        bo.blendPose(
            ba.rampValue, /* weight */
            cal3d::RotateTranslate(
                ba.localOri,
                bo.getOriginalTranslation() /* adjustedLocalPos */),
            ba.rampValue /* subsequentAttenuation */);
    }

    for (size_t i = 0; i < boneScaleAdjustments.size(); i++) {
        const BoneScaleAdjustment& ba = boneScaleAdjustments[i];
        bones[ba.boneId].scale = ba.scale;
    }
}
