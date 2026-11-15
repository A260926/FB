#pragma once

#include "cal3d/BoostToStd.h"
#include "cal3d/global.h"

class CalCoreAnimation;

class CAL3D_API CalAnimation {
public:
    CalAnimation(const ISTD::shared_ptr<CalCoreAnimation>& pCoreAnimation, float weight, unsigned priority);

    const ISTD::shared_ptr<CalCoreAnimation> coreAnimation;

    float time; // current time
    float rampValue; // 0->1 fade in, 1->0 fade out
    const float weight;
    const unsigned priority; // 0 is lowest
};

CAL3D_PTR(CalAnimation);
