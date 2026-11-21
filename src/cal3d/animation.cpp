//****************************************************************************//
// animation.cpp                                                              //
// Copyright (C) 2001, 2002 Bruno 'Beosil' Heidelberger                       //
//****************************************************************************//
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#include "cal3d/animation.h"
#include "cal3d/error.h"
#include "cal3d/coreanimation.h"
#include "cal3d/streamclip.h"

CalAnimation::CalAnimation(const ISTD::shared_ptr<CalCoreAnimation>& pCoreAnimation, float weight, unsigned priority)
    : coreAnimation(pCoreAnimation)
    , time(0.0f)
    , rampValue(1.0f)
    , weight(weight)
    , priority(priority)
{}

CalAnimation::CalAnimation(const ISTD::shared_ptr<CalStreamClip>& pStreamClip, float weight, unsigned priority)
    : streamClip(pStreamClip)
    , time(0.0f)
    , rampValue(1.0f)
    , weight(weight)
    , priority(priority)
{}

void CalAnimation::setStreamClip(const ISTD::shared_ptr<CalStreamClip>& pStreamClip) {
    streamClip = pStreamClip;
}

unsigned CalAnimation::streamBlockIndex() const {
    if (!streamClip) {
        return 0;
    }
    return streamClip->blockIndexForTime(time);
}
