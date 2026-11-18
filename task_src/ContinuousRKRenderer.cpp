/*
ContinuousRKRenderer.cpp

Implementation of the continuous-time double-precision ladder renderer.
All clock arithmetic (frame ratios, event ordering, bisection points) is
integer/rational; absolute frame numbers are never converted to double.
Only local frame differences become doubles for integration/interpolation.
*/

#include "src/ContinuousRKRenderer.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace MoogLadders {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

using u128 = unsigned __int128;
using i128 = signed __int128;

} // namespace

int ContinuousRKRenderer::compareTime(uint64_t origin, const TimePoint& a,
                                      const TimePoint& b)
{
    // Exact rational comparison by quotient/remainder; no cross product that
    // could overflow 128 bits for the largest supported frame positions.
    u128 na = u128(a.anchor - origin) * a.den + uint64_t(a.num);
    u128 nb = u128(b.anchor - origin) * b.den + uint64_t(b.num);
    u128 qa = na / a.den, ra = na % a.den;
    u128 qb = nb / b.den, rb = nb % b.den;
    if (qa != qb)
        return qa < qb ? -1 : 1;
    u128 lhs = ra * b.den;
    u128 rhs = rb * a.den;
    if (lhs < rhs) return -1;
    if (lhs > rhs) return 1;
    return 0;
}

ContinuousRKRenderer::TimePoint
ContinuousRKRenderer::midPoint(uint64_t origin, const TimePoint& a,
                               const TimePoint& b)
{
    // m = a + (b - a) / 2, built from the local segment difference so the
    // integer magnitudes only grow with the recursion depth, never with the
    // absolute frame position.  Consecutive segment endpoints are less than
    // one source frame apart.
    (void)origin;
    const i128 ad = int64_t(a.den);
    const i128 bd = int64_t(b.den);
    const i128 span = int64_t(b.anchor - a.anchor);

    // b - a as a reduced rational (span + b.num/bd - a.num/ad).
    i128 dnum = span * ad * bd + int64_t(b.num) * ad - int64_t(a.num) * bd;
    i128 dden = ad * bd;

    // Fractional offset of the midpoint from a.anchor:
    // a.num/ad + dnum / (2 * dden).
    i128 fnum = int64_t(a.num) * 2 * dden + dnum * ad;
    i128 fden = ad * 2 * dden;
    i128 g = std::gcd(fnum < 0 ? -fnum : fnum, fden);
    if (g > 1)
    {
        fnum /= g;
        fden /= g;
    }

    i128 carry = fnum / fden; // floor, also handles a negative fraction
    i128 rem = fnum - carry * fden;
    return TimePoint{a.anchor + uint64_t(carry), int64_t(rem),
                     uint64_t(fden)};
}

ContinuousRKRenderer::ContinuousRKRenderer(
    uint32_t sourceRate, uint32_t targetRate, std::size_t channels,
    uint64_t sourceOrigin, std::vector<ControlPoint> controls,
    double tolerance, uint32_t maxDepth)
    : sourceRate_(sourceRate)
    , targetRate_(targetRate)
    , channels_(channels)
    , origin_(sourceOrigin)
    , tolerance_(tolerance)
    , maxDepth_(maxDepth)
    , controls_(std::move(controls))
{
    if (sourceRate < kMinRate || sourceRate > kMaxRate ||
        targetRate < kMinRate || targetRate > kMaxRate)
        throw std::invalid_argument("ContinuousRKRenderer: sample rate out of range");
    if (channels != 1 && channels != 2 && channels != 6)
        throw std::invalid_argument("ContinuousRKRenderer: channels must be 1, 2 or 6");
    if (sourceOrigin > kMaxFramePosition)
        throw std::invalid_argument("ContinuousRKRenderer: source origin too large");
    if (!(tolerance >= 1e-10 && tolerance <= 1e-5))
        throw std::invalid_argument("ContinuousRKRenderer: tolerance out of range");
    if (maxDepth > 20)
        throw std::invalid_argument("ContinuousRKRenderer: maxDepth out of range");
    if (controls_.empty())
        throw std::invalid_argument("ContinuousRKRenderer: at least one control point required");
    if (controls_.front().frame != sourceOrigin)
        throw std::invalid_argument(
            "ContinuousRKRenderer: first control point must equal source origin");

    const double cutoffBound = std::min(5000.0, 0.2 * double(sourceRate));
    for (std::size_t i = 0; i < controls_.size(); ++i)
    {
        const ControlPoint& cp = controls_[i];
        if (i > 0 && cp.frame <= controls_[i - 1].frame)
            throw std::invalid_argument(
                "ContinuousRKRenderer: control frames must be strictly increasing");
        if (cp.frame > kMaxFramePosition)
            throw std::invalid_argument("ContinuousRKRenderer: control frame too large");
        if (!std::isfinite(cp.cutoff) || cp.cutoff < 0.0 ||
            cp.cutoff > cutoffBound)
            throw std::invalid_argument("ContinuousRKRenderer: cutoff out of range");
        if (!std::isfinite(cp.resonance) || cp.resonance < 0.0 ||
            cp.resonance > 3.5)
            throw std::invalid_argument("ContinuousRKRenderer: resonance out of range");
    }

    state_.assign(channels_ * 4, 0.0);
    snapshot_.assign(channels_ * 4, 0.0);
    coarse_.assign(channels_ * 4, 0.0);
    fine_.assign(channels_ * 4, 0.0);

    ringFrame_.assign(2, kInvalidFrame);
    ringValue_.assign(2 * channels_, 0.0f);

    received_ = origin_;
    nextIn_ = origin_;
    cursor_ = TimePoint{origin_, 0, sourceRate_};

    // ceil(origin * targetRate / sourceRate), in exact integer arithmetic.
    u128 prod = u128(origin_) * targetRate_;
    nextOut_ = uint64_t((prod + sourceRate_ - 1) / sourceRate_);
}

double ContinuousRKRenderer::stateAt(std::size_t channel,
                                     std::size_t stage) const
{
    if (channel >= channels_ || stage >= 4)
        throw std::invalid_argument("ContinuousRKRenderer: state index out of range");
    return state_[channel * 4 + stage];
}

void ContinuousRKRenderer::copyStates(std::size_t channel, double out[4]) const
{
    if (channel >= channels_)
        throw std::invalid_argument("ContinuousRKRenderer: channel out of range");
    for (std::size_t i = 0; i < 4; ++i)
        out[i] = state_[channel * 4 + i];
}

ContinuousRKRenderer::TimePoint
ContinuousRKRenderer::targetPoint(uint64_t targetFrame) const
{
    // Absolute source-frame coordinate of target time j / targetRate:
    // j * sourceRate / targetRate.  j is absolute (time zero based), so the
    // origin is not added; it only selects the first emitted j.
    u128 p = u128(targetFrame) * sourceRate_;
    uint64_t q = uint64_t(p / targetRate_);
    uint64_t r = uint64_t(p % targetRate_);
    return TimePoint{q, int64_t(r), targetRate_};
}

void ContinuousRKRenderer::controlAt(const TimePoint& p, double& k,
                                     double& resonance)
{
    // Stateless binary search for the bracketing control segment.  Control
    // points sit on integer source frames, so only the anchor is compared;
    // this stays correct during rejected RK trials that revisit a time.
    std::size_t lo = 0, hi = controls_.size();
    while (lo + 1 < hi)
    {
        std::size_t mid = lo + (hi - lo) / 2;
        if (controls_[mid].frame <= p.anchor)
            lo = mid;
        else
            hi = mid;
    }
    std::size_t i = lo;

    const ControlPoint& c0 = controls_[i];
    if (i + 1 == controls_.size())
    {
        k = kTwoPi * c0.cutoff;
        resonance = c0.resonance;
        return;
    }
    const ControlPoint& c1 = controls_[i + 1];
    // Only the local difference from the bracketing point becomes a double.
    double local = double(p.anchor - c0.frame) +
                   double(p.num) / double(p.den);
    double alpha = local / double(c1.frame - c0.frame);
    double cutoff = c0.cutoff + alpha * (c1.cutoff - c0.cutoff);
    resonance = c0.resonance + alpha * (c1.resonance - c0.resonance);
    k = kTwoPi * cutoff;
}

float ContinuousRKRenderer::sampleAt(std::size_t ch,
                                     uint64_t absoluteFrame) const
{
    if (absoluteFrame == terminalFrame_ || absoluteFrame == batchTerminal_)
        return 0.0f;
    for (std::size_t s = 0; s < ringFrame_.size(); ++s)
    {
        if (ringFrame_[s] == absoluteFrame)
            return ringValue_[s * channels_ + ch];
    }
    if (batchInput_ && absoluteFrame >= batchStart_ &&
        absoluteFrame < batchStart_ + batchFrames_)
    {
        return batchInput_[ch][absoluteFrame - batchStart_];
    }
    throw std::runtime_error(
        "ContinuousRKRenderer: internal error, source sample not available");
}

double ContinuousRKRenderer::inputAt(std::size_t ch, const TimePoint& p) const
{
    float v0 = sampleAt(ch, p.anchor);
    if (p.num == 0)
        return double(v0);
    float v1 = sampleAt(ch, p.anchor + 1);
    double w = double(p.num) / double(p.den);
    return double(v0) + w * (double(v1) - double(v0));
}
void ContinuousRKRenderer::evaluateStage(std::size_t ch, const TimePoint& p,
                                         const double state[4],
                                         double dstate[4])
{
    double k, resonance;
    controlAt(p, k, resonance);
    double x = inputAt(ch, p);
    continuousRKDerivatives(x, k, resonance, state, dstate);
    ++derivativeCalls_;
}

void ContinuousRKRenderer::rk4Channel(std::size_t ch, const TimePoint& a,
                                      const TimePoint& b,
                                      const double* inState, double* outState)
{
    // Local time difference in seconds; absolute frames stay integer.
    double hFrames = double(b.anchor - a.anchor) +
                     double(b.num) / double(b.den) -
                     double(a.num) / double(a.den);
    double h = hFrames / double(sourceRate_);
    TimePoint m = midPoint(origin_, a, b);

    double d1[4], d2[4], d3[4], d4[4], tmp[4];
    const double* s = inState + ch * 4;

    evaluateStage(ch, a, s, d1);
    for (int i = 0; i < 4; ++i)
        tmp[i] = s[i] + 0.5 * h * d1[i];
    evaluateStage(ch, m, tmp, d2);
    for (int i = 0; i < 4; ++i)
        tmp[i] = s[i] + 0.5 * h * d2[i];
    evaluateStage(ch, m, tmp, d3);
    for (int i = 0; i < 4; ++i)
        tmp[i] = s[i] + h * d3[i];
    evaluateStage(ch, b, tmp, d4);
    for (int i = 0; i < 4; ++i)
        outState[ch * 4 + i] =
            s[i] + (h / 6.0) * (d1[i] + 2.0 * d2[i] + 2.0 * d3[i] + d4[i]);
}

void ContinuousRKRenderer::rk4All(const TimePoint& a, const TimePoint& b,
                                  const double* inState, double* outState)
{
    for (std::size_t ch = 0; ch < channels_; ++ch)
        rk4Channel(ch, a, b, inState, outState);
}

void ContinuousRKRenderer::adaptSegment(const TimePoint& a,
                                        const TimePoint& b, uint32_t depth)
{
    TimePoint m = midPoint(origin_, a, b);

    // One full step versus two half steps, evaluated from the same untouched
    // state.  No audio state is mutated on a rejected trial.
    rk4All(a, b, state_.data(), coarse_.data());
    rk4All(a, m, state_.data(), fine_.data());
    rk4All(m, b, fine_.data(), fine_.data());

    double err = 0.0;
    for (std::size_t i = 0; i < state_.size(); ++i)
    {
        double e = std::fabs(fine_[i] - coarse_[i]) /
                   (15.0 * (1.0 + std::fabs(fine_[i])));
        if (e > err)
            err = e;
    }

    if (err <= tolerance_)
    {
        std::copy(fine_.begin(), fine_.end(), state_.begin());
        ++acceptedSteps_;
        return;
    }

    ++rejectedSteps_;
    if (depth >= maxDepth_)
        throw std::runtime_error(
            "ContinuousRKRenderer: adaptive integration depth exhausted");

    // One shared step decision for every channel: recurse left then right.
    adaptSegment(a, m, depth + 1);
    adaptSegment(m, b, depth + 1);
}

void ContinuousRKRenderer::integrateSegment(const TimePoint& a,
                                            const TimePoint& b)
{
    if (compareTime(origin_, a, b) == 0)
        return;

    // Whole-segment rollback snapshot for every channel.
    std::copy(state_.begin(), state_.end(), snapshot_.begin());
    try
    {
        adaptSegment(a, b, 0);
    }
    catch (...)
    {
        std::copy(snapshot_.begin(), snapshot_.end(), state_.begin());
        throw;
    }
}

void ContinuousRKRenderer::retainLastTwo(uint64_t reached)
{
    // Keep the two most recent source endpoints at or below the integration
    // horizon so the next call can interpolate across the boundary.  Read the
    // values *before* touching the ring: one of these frames may only exist in
    // the ring being replaced.
    float picked[2][6];
    uint64_t frames[2] = {reached - 2, reached - 1};
    bool have[2] = {false, false};
    for (int t = 0; t < 2; ++t)
    {
        if (frames[t] >= origin_)
        {
            for (std::size_t ch = 0; ch < channels_; ++ch)
                picked[t][ch] = sampleAt(ch, frames[t]);
            have[t] = true;
        }
    }

    ringFrame_[0] = kInvalidFrame;
    ringFrame_[1] = kInvalidFrame;
    std::size_t slot = 0;
    for (int t = 0; t < 2; ++t)
    {
        if (!have[t])
            continue;
        ringFrame_[slot] = frames[t];
        for (std::size_t ch = 0; ch < channels_; ++ch)
            ringValue_[slot * channels_ + ch] = picked[t][ch];
        ++slot;
    }
}

ProcessResult ContinuousRKRenderer::process(const float* const* input,
                                            std::size_t frames, bool end,
                                            float* const* output,
                                            std::size_t capacity)
{
    if (frames > kMaxBlock || capacity > kMaxBlock)
        throw std::invalid_argument("ContinuousRKRenderer: block size out of range");

    // Zero capacity: read no pointers, accept no input, do not latch end.
    if (capacity == 0)
        return ProcessResult{0, 0, nextOut_, done_};

    if (done_)
    {
        if (frames != 0)
            throw std::invalid_argument(
                "ContinuousRKRenderer: input supplied after completion");
        // Repeated calls after completion keep returning the completed state.
        return ProcessResult{0, 0, nextOut_, true};
    }
    if (endLatched_ && frames != 0)
        throw std::invalid_argument(
            "ContinuousRKRenderer: input supplied after end of stream");

    for (std::size_t f = 0; f < frames; ++f)
        for (std::size_t ch = 0; ch < channels_; ++ch)
        {
            float v = input[ch][f];
            if (!std::isfinite(v) || std::fabs(v) > 6.0f)
                throw std::invalid_argument(
                    "ContinuousRKRenderer: input sample out of range");
        }

    const uint64_t terminalIfLatched = received_ + frames;
    if (end && terminalIfLatched > kMaxFramePosition)
        throw std::invalid_argument(
            "ContinuousRKRenderer: source end position too large");

    const uint64_t firstOutput = nextOut_;
    const uint64_t availEnd = received_ + frames; // one past last sample
    // The terminal zero-valued endpoint is reachable inside this call only if
    // end is set; it becomes permanent once the whole prefix is consumed.
    const bool latchPending = end;
    batchTerminal_ = latchPending ? availEnd : kInvalidFrame;

    batchInput_ = input;
    batchStart_ = received_;
    batchFrames_ = frames;

    std::size_t produced = 0;
    try
    {
        while (true)
        {
            const TimePoint sp{nextIn_, 0, sourceRate_};
            const TimePoint tp = targetPoint(nextOut_);

            // Ordinary source endpoints already received, plus the synthetic
            // zero-valued terminal endpoint once end is in effect.
            const bool terminalActive = latchPending || endLatched_;
            const uint64_t terminal = terminalActive
                                          ? (latchPending ? availEnd
                                                          : terminalFrame_)
                                          : kInvalidFrame;
            bool srcReachable = nextIn_ < availEnd ||
                                (terminalActive && nextIn_ == terminal);
            // Target events must stay strictly before the terminal time and
            // must not run past the last known source endpoint.
            bool tgtReachable = false;
            if (terminalActive)
            {
                // j / targetRate < terminal / sourceRate, exact compare.
                u128 lhs = u128(nextOut_) * sourceRate_;
                u128 rhs = u128(terminal) * targetRate_;
                tgtReachable = lhs < rhs;
            }
            else if (nextIn_ < availEnd)
            {
                tgtReachable = compareTime(origin_, tp, sp) <= 0;
            }

            const bool capacityFull = produced >= capacity;
            if (capacityFull)
            {
                // No room for another target sample.  A source endpoint that
                // lies strictly before the next target instant can still be
                // integrated (it produces no output), keeping the source
                // cursor and step bookkeeping independent of output capacity.
                // A coincident or earlier target event forces a stop so its
                // sample is never skipped.
                bool sourceOnly = srcReachable &&
                                  (!tgtReachable ||
                                   compareTime(origin_, sp, tp) < 0);
                if (!sourceOnly)
                    break;
                integrateSegment(cursor_, sp);
                cursor_ = sp;
                ++nextIn_;
                continue;
            }

            if (!srcReachable && !tgtReachable)
                break; // starved: never guess the next endpoint

            const TimePoint* b;
            bool hitSource = false;
            bool hitTarget = false;
            if (srcReachable &&
                (!tgtReachable || compareTime(origin_, sp, tp) <= 0))
            {
                b = &sp;
                hitSource = true;
                hitTarget = tgtReachable &&
                            compareTime(origin_, sp, tp) == 0;
            }
            else
            {
                b = &tp;
                hitTarget = true;
            }

            integrateSegment(cursor_, *b);
            cursor_ = *b;

            if (hitSource)
                ++nextIn_;
            if (hitTarget)
            {
                for (std::size_t ch = 0; ch < channels_; ++ch)
                    output[ch][produced] = float(state_[ch * 4 + 3]);
                ++nextOut_;
                ++produced;
            }
        }
    }
    catch (...)
    {
        batchInput_ = nullptr;
        batchFrames_ = 0;
        batchTerminal_ = kInvalidFrame;
        throw;
    }

    // Consume only the source prefix actually integrated through.  When the
    // terminal endpoint was reached, nextIn_ is one frame past the samples.
    uint64_t reached = nextIn_;
    if (reached > availEnd)
        reached = availEnd;
    std::size_t consumed = static_cast<std::size_t>(reached - batchStart_);

    if (consumed > 0)
        retainLastTwo(reached);
    received_ = reached;

    batchInput_ = nullptr;
    batchFrames_ = 0;
    batchTerminal_ = kInvalidFrame;

    // End is latched only when every frame supplied this call was consumed.
    if (end && consumed == frames)
    {
        endLatched_ = true;
        terminalFrame_ = received_;
    }

    if (endLatched_)
    {
        u128 lhs = u128(nextOut_) * sourceRate_;
        u128 rhs = u128(terminalFrame_) * targetRate_;
        if (!(lhs < rhs))
            done_ = true;
    }

    return ProcessResult{consumed, produced, firstOutput, done_};
}

} // namespace MoogLadders
