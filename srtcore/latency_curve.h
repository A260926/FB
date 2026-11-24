/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2024 Haivision Systems Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

#ifndef INC_SRT_LATENCY_CURVE_H
#define INC_SRT_LATENCY_CURVE_H

#include "platform_sys.h"
#include "sync.h"

namespace srt
{

/// @brief Locally scheduled, piecewise-linear evolution of the TSBPD receive
/// latency.
///
/// The curve is defined entirely in local steady-clock microseconds and never
/// touches the wire. A schedule started at local time t0 takes the latency L0
/// that is in effect at t0 and ramps it towards the target L1 over a ramp
/// duration R (microseconds):
///
///   t <= t0        : L(t) = L0
///   t0 < t < t0+R  : L(t) = L0 + floor((L1 - L0) * (t - t0) / R)
///   t >= t0+R      : L(t) = L1
///
/// The floor is taken towards negative infinity, so decreasing ramps keep the
/// same rounding rule as increasing ones. Rescheduling while a ramp is still
/// in progress continues from the latency in effect at the new t0: the curve
/// stays continuous and neither jumps to the previous target nor restarts from
/// the originally negotiated latency.
class LatencyCurve
{
    typedef sync::steady_clock::time_point time_point;

public:
    /// Ramp parameter limits, expressed in the public API units (milliseconds).
    static const int TARGET_MIN_MS = 0;
    static const int TARGET_MAX_MS = 60000;
    static const int RAMP_MIN_MS   = 1;
    static const int RAMP_MAX_MS   = 60000;

    LatencyCurve()
        : m_bActive(false)
        , m_llStartUs(0)
        , m_llRampUs(0)
        , m_llFromUs(0)
        , m_llTargetUs(0)
    {
    }

    /// @brief Start a new ramp at an explicit local monotonic time.
    ///
    /// @param t0       local steady-clock start of the ramp.
    /// @param fromUs   latency currently in effect (microseconds).
    /// @param targetUs target latency (microseconds), 0..60000000.
    /// @param rampUs   ramp duration (microseconds), strictly positive. For an
    ///                 increasing schedule it must be strictly greater than
    ///                 (targetUs - fromUs).
    /// @return true if the schedule was installed, false if the parameters do
    /// not satisfy the ramp constraints (state is left untouched on failure).
    bool schedule(const time_point& t0, int64_t fromUs, int64_t targetUs, int64_t rampUs);

    /// Drop any installed schedule. Afterwards the curve reports itself
    /// inactive and contributes no extra buffering latency.
    void reset();

    bool isActive() const { return m_bActive; }

    /// @brief Latency L(t) in effect at local time @a now, in microseconds.
    /// Returns 0 when no schedule is installed.
    int64_t latencyAt(const time_point& now) const;

    /// @brief First integer local time t >= @a notBeforeUs (microseconds since
    /// the steady-clock epoch) at which a packet whose buffer-free source time
    /// is @a baseUs (microseconds since the same epoch) becomes due.
    ///
    /// The packet is due at the first t for which (t - baseUs) >= L(t). The
    /// search is restricted to t >= notBeforeUs: a packet already due at the
    /// reference time gets @a notBeforeUs as its due time and is immediately
    /// readable. Returns an integer microsecond epoch value.
    int64_t firstDueUs(int64_t baseUs, int64_t notBeforeUs) const;

    /// Same as firstDueUs(), but expressed with steady-clock time points.
    time_point firstDue(const time_point& base, const time_point& notBefore) const;

private:
    bool    m_bActive;
    int64_t m_llStartUs;  // t0, microseconds since the steady-clock epoch
    int64_t m_llRampUs;   // R, strictly positive while active
    int64_t m_llFromUs;   // L0
    int64_t m_llTargetUs; // L1
};

} // namespace srt

#endif // INC_SRT_LATENCY_CURVE_H
