#pragma once

#include "cal3d/BoostToStd.h"
#include "cal3d/global.h"

class CalCoreAnimation;

namespace cal3d {
class CalStreamClip;
using CalStreamClipPtr = ISTD::shared_ptr<CalStreamClip>;
}

class CAL3D_API CalAnimation {
public:
    CalAnimation(const ISTD::shared_ptr<CalCoreAnimation>& pCoreAnimation, float weight, unsigned priority);

    // Null exactly when this animation is driven by a streamed clip.  Legacy
    // code must reject such animations rather than dereferencing this pointer.
    ISTD::shared_ptr<CalCoreAnimation> coreAnimation;

    // Optional streamed source; empty for ordinary in-memory animations.
    cal3d::CalStreamClipPtr streamClip;

    float time; // current time
    float rampValue; // 0->1 fade in, 1->0 fade out
    const float weight;
    const unsigned priority; // 0 is lowest
};

CAL3D_PTR(CalAnimation);
