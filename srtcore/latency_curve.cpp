/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2024 Haivision Systems Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

#include "latency_curve.h"

#include "platform_sys.h"

#include <algorithm>

namespace srt
{

namespace
{
    /// Floor division: the remainder follows the sign of the denominator, so
    /// the quotient is rounded towards negative infinity.
    int64_t floor_div(int64_t n, int64_t d)
    {
        // d is always strictly positive in this module.
        int64_t q = n / d;
        int64_t r = n % d;
        if (r != 0 && r < 0)
            --q;
        return q;
    }

    /// Smallest integer x in [lo, hi] for which pred(x) holds. pred must be
    /// false for all x below the result and true from the result onwards.
    /// Returns hi + 1 if pred never holds on the range.
    template <class Pred>
    int64_t lower_bound_int(int64_t lo, int64_t hi, Pred pred)
    {
        int64_t bad = lo - 1;
        int64_t good = hi + 1;
        while (good - bad > 1)
        {
            int64_t mid = bad + (good - bad) / 2;
            if (pred(mid))
                good = mid;
            else
                bad = mid;
        }
        return good;
    }
} // namespace

bool LatencyCurve::schedule(const time_point& t0, int64_t fromUs, int64_t targetUs, int64_t rampUs)
{
    // The ramp duration is expressed in microseconds; the millisecond-level
    // minimum (RAMP_MIN_MS) is enforced by the public API entry, while the
    // curve itself only requires a strictly positive duration.
    if (rampUs <= 0)
        return false;
    if (targetUs < TARGET_MIN_MS * int64_t(1000) || targetUs > TARGET_MAX_MS * int64_t(1000))
        return false;

    // An increase must be spread over a ramp strictly longer than the
    // latency difference, otherwise the piecewise curve could step backwards
    // relative to wall-clock delivery times.
    if (targetUs > fromUs && rampUs <= (targetUs - fromUs))
        return false;

    m_bActive   = true;
    m_llStartUs = sync::count_microseconds(t0.time_since_epoch());
    m_llRampUs  = rampUs;
    m_llFromUs  = fromUs;
    m_llTargetUs = targetUs;
    return true;
}

void LatencyCurve::reset()
{
    m_bActive    = false;
    m_llStartUs  = 0;
    m_llRampUs   = 0;
    m_llFromUs   = 0;
    m_llTargetUs = 0;
}

int64_t LatencyCurve::latencyAt(const time_point& now) const
{
    if (!m_bActive)
        return 0;

    const int64_t t = sync::count_microseconds(now.time_since_epoch());
    const int64_t x = t - m_llStartUs;

    if (x <= 0)
        return m_llFromUs;
    if (x >= m_llRampUs)
        return m_llTargetUs;

    return m_llFromUs + floor_div((m_llTargetUs - m_llFromUs) * x, m_llRampUs);
}

int64_t LatencyCurve::firstDueUs(int64_t baseUs, int64_t notBeforeUs) const
{
    if (!m_bActive)
    {
        // No schedule: a packet is due exactly one (zero) buffering latency
        // after its source time, clamped to the queried time region.
        return std::max(baseUs, notBeforeUs);
    }

    const int64_t s = m_llStartUs;
    const int64_t R = m_llRampUs;
    const int64_t D = m_llTargetUs - m_llFromUs;

    // Constant region preceding the ramp start.
    if (notBeforeUs < s)
    {
        const int64_t candidate = std::max(notBeforeUs, baseUs + m_llFromUs);
        if (candidate <= s)
            return candidate;
    }

    // Ramp region: x = t - s, condition x - C >= floor(D * x / R)
    // with C = baseUs + L0 - s.
    const int64_t C      = baseUs + m_llFromUs - s;
    const int64_t xLo    = std::max<int64_t>(0, notBeforeUs - s);
    const int64_t xHi    = R - 1; // t = s + R belongs to the constant target region

    if (xLo <= xHi)
    {
        int64_t xDue = xHi + 1;
        if (D >= 0)
        {
            // g(x) = x - floor(D*x/R) is non-decreasing across the ramp.
            xDue = lower_bound_int(xLo, xHi, [&](int64_t x) {
                return x - floor_div(D * x, R) >= C;
            });
        }
        else
        {
            // Decreasing ramp: floor(D*x/R) = -ceil(|D|*x/R), so the
            // condition becomes x + ceil(|D|*x/R) >= C.
            const int64_t E = -D;
            xDue = lower_bound_int(xLo, xHi, [&](int64_t x) {
                const int64_t ceilTerm = floor_div(E * x + R - 1, R);
                return x + ceilTerm >= C;
            });
        }

        if (xDue <= xHi)
            return s + xDue;
    }

    // Constant target region. Always has a solution.
    return std::max(notBeforeUs, std::max(s + R, baseUs + m_llTargetUs));
}

LatencyCurve::time_point LatencyCurve::firstDue(const time_point& base, const time_point& notBefore) const
{
    const int64_t baseUs      = sync::count_microseconds(base.time_since_epoch());
    const int64_t notBeforeUs = sync::count_microseconds(notBefore.time_since_epoch());
    return time_point(sync::microseconds_from(firstDueUs(baseUs, notBeforeUs)));
}

} // namespace srt
