#include "src/ContinuousRKRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace MoogLadders
{

namespace
{

// All absolute clock products (frame up to 2^55 times rate up to 192000)
// exceed 64 bits, so ordering of rational times uses 128-bit integer math.
using u128 = unsigned __int128;

int compareRationalTimes(uint64_t frameA, uint32_t rateA,
                         uint64_t frameB, uint32_t rateB)
{
    const u128 lhs = static_cast<u128>(frameA) * static_cast<u128>(rateA);
    const u128 rhs = static_cast<u128>(frameB) * static_cast<u128>(rateB);
    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

uint64_t ceilDivide128(uint64_t numeratorFrame, uint32_t numeratorRate,
                       uint32_t denominatorRate)
{
    const u128 num = static_cast<u128>(numeratorFrame) * static_cast<u128>(numeratorRate);
    const u128 den = static_cast<u128>(denominatorRate);
    return static_cast<uint64_t>((num + den - 1u) / den);
}

// Local clock differences can reach ~2^73, beyond uint64; convert the 128-bit
// value straight to double (only local differences ever leave integer math).
double u128ToDouble(u128 value)
{
    return static_cast<double>(value);
}

} // namespace

ContinuousRKRenderer::ContinuousRKRenderer(uint32_t sourceRate,
                                           uint32_t targetRate,
                                           size_t channels,
                                           uint64_t sourceOrigin,
                                           std::vector<ControlPoint> controls,
                                           double tolerance,
                                           uint32_t maxDepth)
    : sourceRate_(sourceRate)
    , targetRate_(targetRate)
    , channels_(channels)
    , nextSource_(sourceOrigin)
    , tolerance_(tolerance)
    , maxDepth_(maxDepth)
    , controls_(std::move(controls))
{
    if (sourceRate < kMinRate || sourceRate > kMaxRate ||
        targetRate < kMinRate || targetRate > kMaxRate)
        throw ContinuousRKError("ContinuousRKRenderer: sample rate out of range");

    if (channels != 1 && channels != 2 && channels != 6)
        throw ContinuousRKError("ContinuousRKRenderer: channel count must be 1, 2 or 6");

    if (sourceOrigin > kMaxFramePosition)
        throw ContinuousRKError("ContinuousRKRenderer: source origin beyond 2^55");

    if (!(tolerance >= 1e-10 && tolerance <= 1e-5))
        throw ContinuousRKError("ContinuousRKRenderer: tolerance out of range");

    if (maxDepth > 20)
        throw ContinuousRKError("ContinuousRKRenderer: maxDepth out of range");

    if (controls_.empty())
        throw ContinuousRKError("ContinuousRKRenderer: at least one control point required");

    const double cutoffLimit = std::min(5000.0, 0.2 * static_cast<double>(sourceRate));
    for (size_t i = 0; i < controls_.size(); ++i)
    {
        const ControlPoint &cp = controls_[i];
        if (i == 0)
        {
            if (cp.frame != sourceOrigin)
                throw ContinuousRKError("ContinuousRKRenderer: first control point must equal sourceOrigin");
        }
        else if (cp.frame <= controls_[i - 1].frame)
        {
            throw ContinuousRKError("ContinuousRKRenderer: control points must be strictly increasing");
        }

        if (cp.frame > kMaxFramePosition)
            throw ContinuousRKError("ContinuousRKRenderer: control point beyond 2^55");

        if (!std::isfinite(cp.cutoff) || cp.cutoff < 0.0 || cp.cutoff > cutoffLimit)
            throw ContinuousRKError("ContinuousRKRenderer: cutoff out of range");

        if (!std::isfinite(cp.resonance) || cp.resonance < 0.0 || cp.resonance > 3.5)
            throw ContinuousRKError("ContinuousRKRenderer: resonance out of range");
    }

    // No endpoint is preloaded: the first received source frame lives at
    // sourceOrigin and becomes the start of the first renderable segment.
    nextOutput_ = ceilDivide128(sourceOrigin, targetRate_, sourceRate_);
}

double ContinuousRKRenderer::stageState(size_t channel, size_t stage) const
{
    if (channel >= channels_ || stage >= 4)
        throw ContinuousRKError("ContinuousRKRenderer: state index out of range");
    return state_[channel][stage];
}

void ContinuousRKRenderer::validateCall(const float *const *input, size_t frames,
                                        const float *const *output,
                                        size_t capacity) const
{
    if (frames > kMaxBlock || capacity > kMaxBlock)
        throw ContinuousRKError("ContinuousRKRenderer: block size out of range");

    if (done_ && frames != 0)
        throw ContinuousRKError("ContinuousRKRenderer: input received after completion");

    if (output == nullptr)
        throw ContinuousRKError("ContinuousRKRenderer: output array required");

    for (size_t ch = 0; ch < channels_; ++ch)
    {
        if (output[ch] == nullptr)
            throw ContinuousRKError("ContinuousRKRenderer: output channel required");
    }

    if (frames != 0 && input == nullptr)
        throw ContinuousRKError("ContinuousRKRenderer: input array required");

    for (size_t ch = 0; frames != 0 && ch < channels_; ++ch)
    {
        if (input[ch] == nullptr)
            throw ContinuousRKError("ContinuousRKRenderer: input channel required");
    }
}

void ContinuousRKRenderer::ingestInput(const float *const *input, size_t index)
{
    // The absolute source end position must stay at or below 2^55.  A frame
    // whose own coordinate is 2^55 would push the end past the limit, so it is
    // refused before any state is touched.
    if (nextSource_ >= kMaxFramePosition)
        throw ContinuousRKError("ContinuousRKRenderer: source position beyond 2^55");

    Endpoint &slot = buffer_[buffered_];
    slot.frame = nextSource_;
    for (size_t ch = 0; ch < channels_; ++ch)
        slot.value[ch] = static_cast<double>(input[ch][index]);
    ++buffered_;
    ++nextSource_;
}

void ContinuousRKRenderer::ingestZeroEndpoint()
{
    Endpoint &slot = buffer_[buffered_];
    slot.frame = nextSource_;
    for (size_t ch = 0; ch < channels_; ++ch)
        slot.value[ch] = 0.0;
    ++buffered_;
    zeroEndpointServed_ = true;
}

void ContinuousRKRenderer::advanceControlHint(uint64_t frame)
{
    while (controlHint_ + 1 < controls_.size() &&
           controls_[controlHint_ + 1].frame <= frame)
    {
        ++controlHint_;
    }
}

void ContinuousRKRenderer::controlsAt(uint64_t segmentFrame, double u,
                                      double &cutoff, double &resonance) const
{
    const ControlPoint &lo = controls_[controlHint_];
    if (controlHint_ + 1 == controls_.size())
    {
        cutoff = lo.cutoff;
        resonance = lo.resonance;
        return;
    }

    const ControlPoint &hi = controls_[controlHint_ + 1];
    const double span = static_cast<double>(hi.frame - lo.frame);
    const double local = static_cast<double>(segmentFrame - lo.frame) + u;
    const double alpha = local / span;
    cutoff = lo.cutoff + alpha * (hi.cutoff - lo.cutoff);
    resonance = lo.resonance + alpha * (hi.resonance - lo.resonance);
}

double ContinuousRKRenderer::inputAt(size_t channel, double u,
                                     const Endpoint &a, const Endpoint &b) const
{
    return a.value[channel] + u * (b.value[channel] - a.value[channel]);
}

void ContinuousRKRenderer::evaluateDerivatives(size_t channel, double u,
                                               const double state[4],
                                               double dstate[4],
                                               const Endpoint &a,
                                               const Endpoint &b,
                                               uint64_t segmentFrame)
{
    double cutoff = 0.0;
    double resonance = 0.0;
    controlsAt(segmentFrame, u, cutoff, resonance);

    const double input = inputAt(channel, u, a, b);
    const double k = 2.0 * MOOG_PI * cutoff;

    rkLadderDerivatives(input, k, resonance, state, dstate,
                        continuousRKSaturation);
    ++derivativeCalls_;
}

void ContinuousRKRenderer::rk4Step(size_t channel, const double state[4],
                                   double u0, double du, double out[4],
                                   const Endpoint &a, const Endpoint &b,
                                   uint64_t segmentFrame)
{
    // u spans one source frame (u in [0,1]) while the derivatives are taken
    // with respect to time, so the RK step in time is du / sourceRate.
    const double h = du / static_cast<double>(sourceRate_);
    double d1[4], d2[4], d3[4], d4[4], temp[4];

    evaluateDerivatives(channel, u0, state, d1, a, b, segmentFrame);
    for (int i = 0; i < 4; ++i)
        temp[i] = state[i] + 0.5 * h * d1[i];

    evaluateDerivatives(channel, u0 + 0.5 * du, temp, d2, a, b, segmentFrame);
    for (int i = 0; i < 4; ++i)
        temp[i] = state[i] + 0.5 * h * d2[i];

    evaluateDerivatives(channel, u0 + 0.5 * du, temp, d3, a, b, segmentFrame);
    for (int i = 0; i < 4; ++i)
        temp[i] = state[i] + h * d3[i];

    evaluateDerivatives(channel, u0 + du, temp, d4, a, b, segmentFrame);
    for (int i = 0; i < 4; ++i)
        out[i] = state[i] + (h / 6.0) * (d1[i] + 2.0 * d2[i] + 2.0 * d3[i] + d4[i]);
}

void ContinuousRKRenderer::adaptiveInterval(double u0, double u1, int depth,
                                            const Endpoint &a,
                                            const Endpoint &b,
                                            uint64_t segmentFrame)
{
    if (!(u1 > u0))
        return;

    const double du = u1 - u0;
    const double half = 0.5 * du;
    double coarse[6][4];
    double mid[6][4];
    double fine[6][4];

    // One full RK4 step versus two half RK4 steps, for every channel at once.
    // The step decision is collective: channels never pick separate steps.
    for (size_t ch = 0; ch < channels_; ++ch)
    {
        rk4Step(ch, state_[ch], u0, du, coarse[ch], a, b, segmentFrame);
        rk4Step(ch, state_[ch], u0, half, mid[ch], a, b, segmentFrame);
        rk4Step(ch, mid[ch], u0 + half, half, fine[ch], a, b, segmentFrame);
    }

    double metric = 0.0;
    for (size_t ch = 0; ch < channels_; ++ch)
    {
        for (int i = 0; i < 4; ++i)
        {
            const double e = std::fabs(fine[ch][i] - coarse[ch][i]) /
                             (15.0 * (1.0 + std::fabs(fine[ch][i])));
            if (e > metric)
                metric = e;
        }
    }

    if (metric <= tolerance_)
    {
        for (size_t ch = 0; ch < channels_; ++ch)
            for (int i = 0; i < 4; ++i)
                state_[ch][i] = fine[ch][i];
        ++acceptedSteps_;
        return;
    }

    ++rejectedSteps_;
    if (depth <= 0)
    {
        // Nothing has been committed: every channel stays at its pre-trial state.
        throw StepDepthExceeded();
    }

    const double umid = u0 + half;
    adaptiveInterval(u0, umid, depth - 1, a, b, segmentFrame);
    adaptiveInterval(umid, u1, depth - 1, a, b, segmentFrame);
}

void ContinuousRKRenderer::renderSegment(float *const *output, size_t capacity,
                                         size_t &produced)
{
    // Lazily open the segment stored in the two endpoint slots and snapshot
    // every channel's state so a depth failure rolls the whole segment back.
    if (!segmentActive_)
    {
        segmentActive_ = true;
        segmentU_ = 0.0;
        segmentFrame_ = buffer_[0].frame;
        std::memcpy(segmentSnapshot_, state_, sizeof(state_));
        advanceControlHint(segmentFrame_);
    }

    const Endpoint &a = buffer_[0];
    const Endpoint &b = buffer_[1];

    try
    {
        while (true)
        {
            const uint64_t j = nextOutput_;

            // Outputs are emitted only at times strictly before the source end.
            // j/targetRate >= endFrame/sourceRate  <=>  j*sourceRate >= endFrame*targetRate.
            const bool pastEnd = endLatched_ &&
                compareRationalTimes(j, sourceRate_, endFrame_, targetRate_) >= 0;

            double uStop;
            bool emit;
            if (pastEnd)
            {
                uStop = 1.0;
                emit = false;
            }
            else
            {
                // Compare j/targetRate with b.frame/sourceRate via cross multiply.
                const int cmp = compareRationalTimes(j, sourceRate_, b.frame, targetRate_);
                if (cmp > 0)
                {
                    // Next target sample lies beyond this source segment.
                    uStop = 1.0;
                    emit = false;
                }
                else
                {
                    // All quantities are integer clock math; only the local
                    // segment offset becomes a double.
                    const u128 num = static_cast<u128>(j) * static_cast<u128>(sourceRate_) -
                                     static_cast<u128>(a.frame) * static_cast<u128>(targetRate_);
                    const u128 den = static_cast<u128>(b.frame - a.frame) *
                                     static_cast<u128>(targetRate_);
                    uStop = u128ToDouble(num) / u128ToDouble(den);
                    if (cmp == 0)
                        uStop = 1.0;
                    emit = true;
                }
            }

            if (uStop > segmentU_)
                adaptiveInterval(segmentU_, uStop, static_cast<int>(maxDepth_),
                                 a, b, segmentFrame_);
            segmentU_ = uStop;

            if (emit)
            {
                if (produced >= capacity)
                    return; // Pause: state and segmentU_ are kept, resume later.
                for (size_t ch = 0; ch < channels_; ++ch)
                    output[ch][produced] = static_cast<float>(state_[ch][3]);
                ++produced;
                ++nextOutput_;
            }

            if (!(uStop < 1.0))
            {
                // Segment complete: drop the older endpoint and close it.
                buffer_[0] = buffer_[1];
                buffered_ = 1;
                segmentActive_ = false;
                if (pastEnd)
                    done_ = true;
                return;
            }
        }
    }
    catch (const StepDepthExceeded &)
    {
        // Roll the whole segment (all channels) back to its opening state;
        // diagnostic counters already record the work that happened.
        std::memcpy(state_, segmentSnapshot_, sizeof(state_));
        segmentU_ = 0.0;
        throw;
    }
}

ProcessReport ContinuousRKRenderer::process(const float *const *input, size_t frames,
                                            bool end,
                                            float *const *output, size_t capacity)
{
    // Zero capacity touches nothing: no pointers, no ingestion, no end latch.
    if (capacity == 0)
        return ProcessReport{0, 0, nextOutput_, done_};

    validateCall(input, frames, output, capacity);

    size_t consumed = 0;
    size_t produced = 0;

    // Interleave ingestion and rendering: keep at most two source endpoints,
    // render each completed segment, then refill from the same offered prefix.
    while (true)
    {
        while (consumed < frames && buffered_ < 2)
        {
            ingestInput(input, consumed);
            ++consumed;
        }

        // end latches only when the whole offered prefix was actually consumed.
        if (end && !endLatched_ && consumed == frames)
        {
            endLatched_ = true;
            endFrame_ = nextSource_;
        }

        // Once EOF is latched, finish the last linear segment with a
        // zero-valued endpoint at the source end (added whenever the renderer
        // has drained down to a single endpoint).  An empty stream ends at once.
        if (endLatched_ && !zeroEndpointServed_)
        {
            if (buffered_ == 0)
            {
                done_ = true;
                break;
            }
            if (buffered_ == 1)
                ingestZeroEndpoint();
        }

        if (done_ || buffered_ < 2)
            break; // completed, or starved: the next edge cannot be guessed

        if (produced >= capacity)
            break;

        // Renders one segment; returns early (segmentActive_ stays set) if the
        // output capacity fills, and resumes on the next process() call.
        renderSegment(output, capacity, produced);
    }

    const uint64_t first = produced == 0 ? nextOutput_ : nextOutput_ - produced;
    return ProcessReport{consumed, produced, first, done_};
}

} // namespace MoogLadders
