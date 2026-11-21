#pragma once

#include "cal3d/BoostToStd.h"
#include "cal3d/global.h"

class CalCoreAnimation;
class CalStreamClip;

class CAL3D_API CalAnimation {
public:
    CalAnimation(const ISTD::shared_ptr<CalCoreAnimation>& pCoreAnimation, float weight, unsigned priority);

    // Streamed source.  When set, coreAnimation may be null and the mixer
    // samples the animation from the block file instead of from memory.
    CalAnimation(const ISTD::shared_ptr<CalStreamClip>& pStreamClip, float weight, unsigned priority);

    // Attach or replace the optional streamed source.  Passing a null clip
    // makes this a regular in-memory animation again.
    void setStreamClip(const ISTD::shared_ptr<CalStreamClip>& pStreamClip);

    bool isStreamed() const {
        return streamClip.get() != 0;
    }

    // Block that must be decoded to sample the current time.  Returns 0 when
    // this is not a streamed animation.
    unsigned streamBlockIndex() const;

    const ISTD::shared_ptr<CalCoreAnimation> coreAnimation;
    ISTD::shared_ptr<CalStreamClip> streamClip;

    float time; // current time
    float rampValue; // 0->1 fade in, 1->0 fade out
    const float weight;
    const unsigned priority; // 0 is lowest
};

CAL3D_PTR(CalAnimation);
