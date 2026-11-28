#pragma once

#include "cal3d/global.h"
#include "cal3d/vector.h"
#include "cal3d/vector4.h"


typedef unsigned int CalColor32; // 32-bit integer, compatible with NSPR

inline CalColor32 CalMakeColor(const CalVector4& cv) {
    return
        ((unsigned int)(cv.z * 0xff) << 0) +
        (((unsigned int)(cv.y * 0xff)) << 8) +
        (((unsigned int)(cv.x * 0xff)) << 16) +
        (((unsigned int)(cv.w * 0xff)) << 24);
}

inline CalVector4 CalVector4FromColor(CalColor32 color) {
    return CalVector4(
        ((color >> 16) & 0xff) / float(0xff),  // r
        ((color >> 8) & 0xff) / float(0xff),   // g
        ((color >> 0) & 0xff) / float(0xff),
        ((color >> 24) & 0xff) / float(0xff));  // a
}
