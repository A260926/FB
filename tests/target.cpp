#include "task_support/check.h"
#include "src/ContinuousRKRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;

using MoogLadders::ContinuousRKError;
using MoogLadders::ContinuousRKRenderer;
using MoogLadders::ControlPoint;
using MoogLadders::ProcessReport;
using MoogLadders::StepDepthExceeded;

using u128 = unsigned __int128;

double u128ToDouble(u128 v) { return static_cast<double>(v); }

// Integer ceil(a*b/c), matching the renderer's first-output arithmetic.
uint64_t ceilRatio(uint64_t frame, uint32_t timesRate, uint32_t overRate)
{
    u128 num = static_cast<u128>(frame) * static_cast<u128>(timesRate);
    u128 den = static_cast<u128>(overRate);
    return static_cast<uint64_t>((num + den - 1u) / den);
}

// Largest integer j with j/targetRate < endFrame/sourceRate.
uint64_t lastOutputBefore(uint64_t endFrame, uint32_t sourceRate, uint32_t targetRate)
{
    return ceilRatio(endFrame, targetRate, sourceRate) - 1u;
}

// Local time (relative to the origin) of target frame j, computed with integer
// clock math and only then converted to double.
double targetLocalTime(uint64_t j, uint64_t origin, uint32_t sourceRate,
                       uint32_t targetRate)
{
    u128 num = static_cast<u128>(j) * static_cast<u128>(sourceRate) -
               static_cast<u128>(origin) * static_cast<u128>(targetRate);
    u128 den = static_cast<u128>(targetRate) * static_cast<u128>(sourceRate);
    return u128ToDouble(num) / u128ToDouble(den);
}

// ---------------------------------------------------------------------------
// Independent fixed-step reference model.  It re-derives the four-stage ladder
// equations directly (no use of the production class or its saturation helper)
// and integrates with classic RK4 on a fine uniform grid in LOCAL time, so it
// stays accurate even when the absolute origin is beyond 2^53.
// ---------------------------------------------------------------------------
struct ReferenceLadder
{
    uint32_t sourceRate = 0;
    uint32_t targetRate = 0;
    uint64_t origin = 0;
    size_t channels = 0;
    const std::vector<std::vector<float>> *source = nullptr;
    const std::vector<ControlPoint> *controls = nullptr;
    double state[6][4] = {};

    static double saturate(double z)
    {
        double v = z / 3.0;
        v = std::max(-1.0, std::min(1.0, v));
        return 3.0 * (v - v * v * v / 3.0);
    }

    // Local source coordinate q = (t - origin/sourceRate) * sourceRate.
    double sourceInput(size_t ch, double q) const
    {
        const auto &x = (*source)[ch];
        const size_t n = x.size();
        if (q <= 0.0)
            return static_cast<double>(x.front());
        if (q >= static_cast<double>(n - 1))
            return static_cast<double>(x.back());
        size_t i = static_cast<size_t>(q);
        double frac = q - static_cast<double>(i);
        return static_cast<double>(x[i]) * (1.0 - frac) +
               static_cast<double>(x[i + 1]) * frac;
    }

    void controlAt(double q, double &cutoff, double &resonance) const
    {
        const auto &cps = *controls;
        size_t i = 0;
        while (i + 1 < cps.size() &&
               static_cast<double>(cps[i + 1].frame - origin) <= q)
            ++i;
        if (i + 1 == cps.size())
        {
            cutoff = cps[i].cutoff;
            resonance = cps[i].resonance;
            return;
        }
        double lo = static_cast<double>(cps[i].frame - origin);
        double hi = static_cast<double>(cps[i + 1].frame - origin);
        double alpha = (q - lo) / (hi - lo);
        cutoff = cps[i].cutoff + alpha * (cps[i + 1].cutoff - cps[i].cutoff);
        resonance = cps[i].resonance + alpha * (cps[i + 1].resonance - cps[i].resonance);
    }

    void derivatives(size_t ch, double tau, const double s[4], double d[4]) const
    {
        double q = tau * static_cast<double>(sourceRate);
        double cutoff, resonance;
        controlAt(q, cutoff, resonance);
        double k = 2.0 * kPi * cutoff;
        double x = sourceInput(ch, q);
        double s0 = saturate(s[0]);
        double s1 = saturate(s[1]);
        double s2 = saturate(s[2]);
        d[0] = k * (saturate(x - resonance * s[3]) - s0);
        d[1] = k * (s0 - s1);
        d[2] = k * (s1 - s2);
        d[3] = k * (s2 - saturate(s[3]));
    }

    void rk4(size_t ch, double tau, double h)
    {
        double d1[4], d2[4], d3[4], d4[4], tmp[4];
        derivatives(ch, tau, state[ch], d1);
        for (int i = 0; i < 4; ++i) tmp[i] = state[ch][i] + 0.5 * h * d1[i];
        derivatives(ch, tau + 0.5 * h, tmp, d2);
        for (int i = 0; i < 4; ++i) tmp[i] = state[ch][i] + 0.5 * h * d2[i];
        derivatives(ch, tau + 0.5 * h, tmp, d3);
        for (int i = 0; i < 4; ++i) tmp[i] = state[ch][i] + h * d3[i];
        derivatives(ch, tau + h, tmp, d4);
        for (int i = 0; i < 4; ++i)
            state[ch][i] += (h / 6.0) * (d1[i] + 2.0 * d2[i] + 2.0 * d3[i] + d4[i]);
    }

    // Produces the fourth-stage reference value at every target frame in js,
    // integrating on a grid of M substeps per source frame.
    std::vector<std::vector<double>> render(const std::vector<uint64_t> &js, int m)
    {
        std::vector<std::vector<double>> out(channels,
                                             std::vector<double>(js.size(), 0.0));
        double h = 1.0 / (static_cast<double>(sourceRate) * m);
        double tau = 0.0;
        for (size_t n = 0; n < js.size(); ++n)
        {
            double tauTarget = targetLocalTime(js[n], origin, sourceRate, targetRate);
            long long full = static_cast<long long>(std::floor((tauTarget - tau) / h + 1e-12));
            for (long long i = 0; i < full; ++i)
            {
                for (size_t ch = 0; ch < channels; ++ch) rk4(ch, tau, h);
                tau += h;
            }
            double rem = tauTarget - tau;
            if (rem > 1e-15)
            {
                for (size_t ch = 0; ch < channels; ++ch) rk4(ch, tau, rem);
                tau = tauTarget;
            }
            for (size_t ch = 0; ch < channels; ++ch) out[ch][n] = state[ch][3];
        }
        return out;
    }
};

// Drives a renderer to completion and collects every output frame.
struct Collected
{
    std::vector<std::vector<float>> samples;
    uint64_t first = 0;
    bool gotFirst = false;
    uint64_t accepted = 0, rejected = 0, derivatives = 0;
    uint64_t nextSource = 0, nextOutput = 0;
    bool done = false;
};

Collected runToCompletion(ContinuousRKRenderer &r,
                          const std::vector<std::vector<float>> &source,
                          size_t inChunk, size_t outChunk,
                          size_t startOffset = 0)
{
    const size_t ch = r.channels();
    Collected c;
    c.samples.assign(ch, {});
    const size_t n = source.empty() ? 0 : source[0].size();

    std::vector<const float *> inPtr(ch);
    std::vector<std::vector<float>> outBuf(ch);
    std::vector<float *> outPtr(ch);
    for (size_t k = 0; k < ch; ++k)
    {
        outBuf[k].assign(std::max<size_t>(outChunk, 1), 0.0f);
        outPtr[k] = outBuf[k].data();
    }

    size_t pos = startOffset;
    bool finished = false;
    while (!finished)
    {
        size_t take = std::min(inChunk == 0 ? 0 : inChunk, n - pos);
        for (size_t k = 0; k < ch; ++k)
            inPtr[k] = take ? source[k].data() + pos : nullptr;
        bool last = (pos + take == n);
        ProcessReport rep = r.process(inPtr.data(), take, last, outPtr.data(), outChunk);
        if (rep.produced && !c.gotFirst)
        {
            c.first = rep.first;
            c.gotFirst = true;
        }
        for (size_t k = 0; k < ch; ++k)
            for (size_t i = 0; i < rep.produced; ++i)
                c.samples[k].push_back(outBuf[k][i]);
        pos += rep.consumed;
        finished = rep.done;
        if (take == 0 && rep.produced == 0 && !rep.done)
            break; // defensive: avoid spinning on a misconfigured loop
    }
    c.accepted = r.acceptedSteps();
    c.rejected = r.rejectedSteps();
    c.derivatives = r.derivativeCalls();
    c.nextSource = r.nextSourceFrame();
    c.nextOutput = r.nextOutputFrame();
    c.done = r.finished();
    return c;
}

uint32_t floatBits(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

std::vector<ControlPoint> sweepControls(uint64_t origin)
{
    // Source rate in the strong-drive case is 8000 Hz, so the cutoff ceiling
    // is min(5000, 0.2*8000) = 1600.
    return {{origin, 900.0, 1.5},
            {origin + 100, 1500.0, 3.2},
            {origin + 250, 350.0, 0.4}};
}

} // namespace

// ===========================================================================
// Continuous: match an independently written, fixed-step reference solution.
// ===========================================================================

static int continuousStrongDrive(void)
{
    const uint32_t sr = 8000, tr = 48000;
    const size_t n = 300, ch = 2;
    std::vector<std::vector<float>> x(ch, std::vector<float>(n));
    for (size_t i = 0; i < n; ++i)
    {
        x[0][i] = float(0.9 * std::sin(0.11 * i) + (i == 20 ? 5.0 : 0.0));
        x[1][i] = float(0.8 * std::cos(0.073 * i) + (i == 60 ? -4.0 : 0.0));
    }
    auto cps = sweepControls(0);

    ContinuousRKRenderer r(sr, tr, ch, 0, cps, 1e-10, 20);
    Collected c = runToCompletion(r, x, 4096, 4096);

    TASK_CHECK(c.done);
    TASK_CHECK(c.rejected > 0);                 // adaptive rejection really happens
    TASK_CHECK(c.derivatives > 0);
    TASK_CHECK(c.samples[0].size() == c.samples[1].size());

    std::vector<uint64_t> js(c.samples[0].size());
    for (size_t i = 0; i < js.size(); ++i) js[i] = c.first + i;

    ReferenceLadder ref64{sr, tr, 0, ch, &x, &cps, {}};
    auto a = ref64.render(js, 64);
    ReferenceLadder ref128{sr, tr, 0, ch, &x, &cps, {}};
    auto b = ref128.render(js, 128);

    double convergence = 0.0, modelError = 0.0;
    for (size_t k = 0; k < ch; ++k)
        for (size_t i = 0; i < js.size(); ++i)
        {
            convergence = std::max(convergence, std::fabs(a[k][i] - b[k][i]));
            modelError = std::max(modelError, std::fabs(b[k][i] - c.samples[k][i]));
        }
    TASK_CHECK(convergence < 1e-7);            // reference converges as refined
    TASK_CHECK(modelError <= 2e-5);
    return 0;
}

static int continuousSixChannels(void)
{
    const uint32_t sr = 48000, tr = 48000;
    const size_t n = 140, ch = 6;
    std::vector<std::vector<float>> x(ch, std::vector<float>(n));
    for (size_t k = 0; k < ch; ++k)
        for (size_t i = 0; i < n; ++i)
            x[k][i] = float(0.6 * std::sin((0.05 + 0.013 * k) * i + k) +
                            (i == 10 + 7 * k ? 3.0 : 0.0));
    std::vector<ControlPoint> cps = {{0, 1200.0, 3.5}, {70, 400.0, 1.0}};

    ContinuousRKRenderer r(sr, tr, ch, 0, cps, 1e-9, 20);
    Collected c = runToCompletion(r, x, 37, 64);
    TASK_CHECK(c.done);
    TASK_CHECK(c.samples[0].size() == n);

    std::vector<uint64_t> js(n);
    for (size_t i = 0; i < n; ++i) js[i] = c.first + i;
    ReferenceLadder ref{sr, tr, 0, ch, &x, &cps, {}};
    auto refOut = ref.render(js, 64);

    for (size_t k = 0; k < ch; ++k)
    {
        double e = 0.0;
        for (size_t i = 0; i < n; ++i) e = std::max(e, std::fabs(refOut[k][i] - c.samples[k][i]));
        TASK_CHECK(e <= 2e-5);
    }
    return 0;
}

static int continuousCrossesControlNodes(void)
{
    const uint32_t sr = 16000, tr = 24000;
    const size_t n = 40, ch = 1;
    std::vector<std::vector<float>> x(ch, std::vector<float>(n));
    for (size_t i = 0; i < n; ++i)
        x[0][i] = float(1.5 * std::sin(0.4 * i) + (i == 23 ? 6.0 : 0.0));
    std::vector<ControlPoint> cps = {{0, 200.0, 0.2},
                                     {5, 1500.0, 3.4},
                                     {11, 800.0, 2.0},
                                     {17, 3000.0, 0.1},
                                     {30, 600.0, 2.8}};

    ContinuousRKRenderer r(sr, tr, ch, 0, cps, 1e-10, 20);
    Collected c = runToCompletion(r, x, 9, 13);
    TASK_CHECK(c.done);
    TASK_CHECK(c.rejected > 0);

    std::vector<uint64_t> js(c.samples[0].size());
    for (size_t i = 0; i < js.size(); ++i) js[i] = c.first + i;
    ReferenceLadder refA{sr, tr, 0, ch, &x, &cps, {}};
    auto a = refA.render(js, 48);
    ReferenceLadder refB{sr, tr, 0, ch, &x, &cps, {}};
    auto b = refB.render(js, 96);

    double conv = 0.0, err = 0.0;
    for (size_t i = 0; i < js.size(); ++i)
    {
        conv = std::max(conv, std::fabs(a[0][i] - b[0][i]));
        err = std::max(err, std::fabs(b[0][i] - c.samples[0][i]));
    }
    TASK_CHECK(conv < 1e-7);
    TASK_CHECK(err <= 2e-5);
    return 0;
}

// ===========================================================================
// Clock: rational frame arithmetic, first/last indices, big origins.
// ===========================================================================

static std::vector<std::vector<float>> clockSignal(size_t ch, size_t n, uint32_t sr)
{
    std::vector<std::vector<float>> x(ch, std::vector<float>(n));
    for (size_t k = 0; k < ch; ++k)
        for (size_t i = 0; i < n; ++i)
            x[k][i] = float(0.7 * std::sin((0.2 + 0.05 * k) * i + 0.3 * k) +
                            (i == 3 + 2 * k ? 2.5 : 0.0));
    (void)sr;
    return x;
}

static int clockCase(uint32_t sr, uint32_t tr, uint64_t origin, size_t n, size_t ch)
{
    auto x = clockSignal(ch, n, sr);
    std::vector<ControlPoint> cps;
    cps.push_back({origin, 700.0, 2.0});
    if (n > 20) cps.push_back({origin + n / 2, 1800.0, 0.8});

    ContinuousRKRenderer r(sr, tr, ch, origin, cps, 1e-9, 20);
    Collected c = runToCompletion(r, x, 11, 7);

    uint64_t expectFirst = ceilRatio(origin, tr, sr);
    TASK_CHECK(c.gotFirst ? c.first == expectFirst : true);
    TASK_CHECK(r.nextOutputFrame() == expectFirst + c.samples[0].size());
    TASK_CHECK(r.nextSourceFrame() == origin + n);

    if (n != 0)
    {
        uint64_t expectLast = lastOutputBefore(origin + n, sr, tr);
        TASK_CHECK(expectLast >= expectFirst);
        TASK_CHECK(c.samples[0].size() == size_t(expectLast - expectFirst + 1));
        TASK_CHECK(c.first == expectFirst);

        std::vector<uint64_t> js(c.samples[0].size());
        for (size_t i = 0; i < js.size(); ++i) js[i] = expectFirst + i;
        ReferenceLadder ref{sr, tr, origin, ch, &x, &cps, {}};
        auto refOut = ref.render(js, 64);
        for (size_t k = 0; k < ch; ++k)
        {
            double e = 0.0;
            for (size_t i = 0; i < js.size(); ++i)
                e = std::max(e, std::fabs(refOut[k][i] - c.samples[k][i]));
            TASK_CHECK(e <= 2e-5);
        }
    }
    else
    {
        TASK_CHECK(c.samples[0].empty());
        TASK_CHECK(c.done);
        TASK_CHECK(r.nextOutputFrame() == expectFirst);
    }
    return 0;
}

static int clock44To48(void) { return clockCase(44100, 48000, 0, 50, 2); }
static int clock48To32(void) { return clockCase(48000, 32000, 0, 60, 2); }
static int clockSameRate(void) { return clockCase(44100, 44100, 0, 40, 1); }
static int clockEmpty(void) { return clockCase(48000, 48000, 0, 0, 2); }
static int clockSingleFrame(void) { return clockCase(44100, 48000, 0, 1, 2); }

static int clockHugeOrigin(void)
{
    const uint32_t sr = 44100, tr = 48000;
    const uint64_t origin = (uint64_t(1) << 54) + 777;
    const size_t n = 24, ch = 2;
    auto x = clockSignal(ch, n, sr);
    std::vector<ControlPoint> cps = {{origin, 900.0, 2.5},
                                     {origin + 12, 2200.0, 1.0}};

    ContinuousRKRenderer r(sr, tr, ch, origin, cps, 1e-9, 20);
    Collected c = runToCompletion(r, x, 5, 3);
    TASK_CHECK(c.done);

    uint64_t expectFirst = ceilRatio(origin, tr, sr);
    uint64_t expectLast = lastOutputBefore(origin + n, sr, tr);
    TASK_CHECK(c.first == expectFirst);
    TASK_CHECK(c.samples[0].size() == size_t(expectLast - expectFirst + 1));
    TASK_CHECK(r.nextSourceFrame() == origin + n);

    // Channel permutation: swapping the two input channels must exactly swap
    // the two output channels, independent of the huge absolute clock.
    auto xSwap = x;
    std::swap(xSwap[0], xSwap[1]);
    ContinuousRKRenderer r2(sr, tr, ch, origin, cps, 1e-9, 20);
    Collected c2 = runToCompletion(r2, xSwap, 5, 3);
    TASK_CHECK(c2.samples[0].size() == c.samples[0].size());
    for (size_t i = 0; i < c.samples[0].size(); ++i)
    {
        TASK_CHECK(floatBits(c2.samples[0][i]) == floatBits(c.samples[1][i]));
        TASK_CHECK(floatBits(c2.samples[1][i]) == floatBits(c.samples[0][i]));
    }
    return 0;
}

// ===========================================================================
// Streaming: chunking must be bit-exact, plus capacity/EOF/rollback semantics.
// ===========================================================================

struct StreamFixture
{
    const uint32_t sr = 44100, tr = 48000;
    const size_t n = 400, ch = 2;
    std::vector<std::vector<float>> x;
    std::vector<ControlPoint> cps;

    StreamFixture()
    {
        x.assign(ch, std::vector<float>(n));
        for (size_t k = 0; k < ch; ++k)
            for (size_t i = 0; i < n; ++i)
                x[k][i] = float(0.8 * std::sin((0.09 + 0.04 * k) * i) +
                                (i == 50 + 30 * k ? 4.0 : 0.0));
        cps = {{0, 1000.0, 2.2}, {150, 2600.0, 3.1}, {320, 500.0, 0.6}};
    }
};

static int streamingBitExactChunks(void)
{
    StreamFixture f;
    const size_t inChunks[] = {4096, 7, 127, 1};
    const size_t outChunks[] = {4096, 1, 5, 113};

    std::vector<Collected> runs;
    for (size_t q = 0; q < 4; ++q)
    {
        ContinuousRKRenderer r(f.sr, f.tr, f.ch, 0, f.cps, 1e-9, 20);
        runs.push_back(runToCompletion(r, f.x, inChunks[q], outChunks[q]));
    }

    const Collected &base = runs[0];
    TASK_CHECK(base.done);
    for (size_t q = 1; q < 4; ++q)
    {
        const Collected &v = runs[q];
        TASK_CHECK(v.done);
        TASK_CHECK(v.samples[0].size() == base.samples[0].size());
        TASK_CHECK(v.first == base.first);
        TASK_CHECK(v.accepted == base.accepted);
        TASK_CHECK(v.rejected == base.rejected);
        TASK_CHECK(v.derivatives == base.derivatives);
        TASK_CHECK(v.nextSource == base.nextSource);
        TASK_CHECK(v.nextOutput == base.nextOutput);
        for (size_t k = 0; k < f.ch; ++k)
            for (size_t i = 0; i < base.samples[k].size(); ++i)
                TASK_CHECK(floatBits(v.samples[k][i]) == floatBits(base.samples[k][i]));
    }
    return 0;
}

static int streamingZeroCapacity(void)
{
    StreamFixture f;
    ContinuousRKRenderer r(f.sr, f.tr, f.ch, 0, f.cps, 1e-9, 20);
    std::vector<const float *> in(f.ch);
    std::vector<float *> out(f.ch);
    std::vector<std::vector<float>> outBuf(f.ch, std::vector<float>(8));
    for (size_t k = 0; k < f.ch; ++k)
    {
        in[k] = f.x[k].data();
        out[k] = outBuf[k].data();
    }

    uint64_t firstOut = r.nextOutputFrame();
    ProcessReport rep = r.process(in.data(), 10, true, out.data(), 0);
    TASK_CHECK(rep.consumed == 0 && rep.produced == 0);
    TASK_CHECK(rep.first == firstOut && !rep.done);
    TASK_CHECK(r.nextSourceFrame() == 0);          // nothing ingested
    TASK_CHECK(r.derivativeCalls() == 0);          // nothing computed
    TASK_CHECK(!r.finished());                      // end was not latched

    // A normal run afterwards must match the monolithic reference run.
    Collected c = runToCompletion(r, f.x, 13, 17);
    ContinuousRKRenderer ref(f.sr, f.tr, f.ch, 0, f.cps, 1e-9, 20);
    Collected cRef = runToCompletion(ref, f.x, 4096, 4096);
    TASK_CHECK(c.samples[0].size() == cRef.samples[0].size());
    for (size_t k = 0; k < f.ch; ++k)
        for (size_t i = 0; i < c.samples[k].size(); ++i)
            TASK_CHECK(floatBits(c.samples[k][i]) == floatBits(cRef.samples[k][i]));
    return 0;
}

static int streamingStarvation(void)
{
    StreamFixture f;
    ContinuousRKRenderer r(f.sr, f.tr, f.ch, 0, f.cps, 1e-9, 20);
    std::vector<const float *> in(f.ch);
    std::vector<std::vector<float>> outBuf(f.ch, std::vector<float>(16));
    std::vector<float *> out(f.ch);
    for (size_t k = 0; k < f.ch; ++k) out[k] = outBuf[k].data();
    for (size_t k = 0; k < f.ch; ++k) in[k] = f.x[k].data();

    // Feed only two frames, no end: the renderer must not invent the next edge.
    ProcessReport a = r.process(in.data(), 2, false, out.data(), 16);
    TASK_CHECK(a.consumed == 2);
    TASK_CHECK(r.nextSourceFrame() == 2);
    uint64_t sourceAfter = r.nextSourceFrame();

    // Outputs produced during the hand-fed prefix must be retained for comparison.
    std::vector<std::vector<float>> head(f.ch);
    for (size_t k = 0; k < f.ch; ++k)
        for (size_t i = 0; i < a.produced; ++i)
            head[k].push_back(outBuf[k][i]);

    // Calling again with no input changes nothing.
    ProcessReport b = r.process(nullptr, 0, false, out.data(), 16);
    TASK_CHECK(b.consumed == 0 && r.nextSourceFrame() == sourceAfter);
    TASK_CHECK(b.produced == 0);

    // Complete the stream and compare the concatenation against a monolithic run.
    Collected rest = runToCompletion(r, f.x, 29, 11, 2);
    ContinuousRKRenderer refR(f.sr, f.tr, f.ch, 0, f.cps, 1e-9, 20);
    Collected whole = runToCompletion(refR, f.x, 4096, 4096);
    TASK_CHECK(rest.done && whole.done);
    for (size_t k = 0; k < f.ch; ++k)
        for (size_t i = 0; i < rest.samples[k].size(); ++i)
            head[k].push_back(rest.samples[k][i]);
    TASK_CHECK(head[0].size() == whole.samples[0].size());
    for (size_t k = 0; k < f.ch; ++k)
        for (size_t i = 0; i < whole.samples[k].size(); ++i)
            TASK_CHECK(floatBits(head[k][i]) == floatBits(whole.samples[k][i]));
    return 0;
}

static int streamingEofDrain(void)
{
    StreamFixture f;
    ContinuousRKRenderer r(f.sr, f.tr, f.ch, 0, f.cps, 1e-9, 20);
    std::vector<const float *> in(f.ch, nullptr);
    std::vector<std::vector<float>> outBuf(f.ch, std::vector<float>(32));
    std::vector<float *> out(f.ch);
    for (size_t k = 0; k < f.ch; ++k) out[k] = outBuf[k].data();
    for (size_t k = 0; k < f.ch; ++k) in[k] = f.x[k].data();

    // Hand over the whole stream with end=true but a tiny output capacity,
    // then drain with zero-frame calls.
    size_t pos = 0;
    size_t total = 0;
    bool done = false;
    while (!done)
    {
        size_t take = std::min<size_t>(31, f.n - pos);
        ProcessReport rep = r.process(in.data(), take, take != 0 && pos + take == f.n,
                                      out.data(), 7);
        pos += rep.consumed;
        total += rep.produced;
        for (size_t k = 0; k < f.ch; ++k) in[k] = f.x[k].data() + pos;
        if (rep.done) done = true;
        if (take == 0 && rep.produced == 0 && rep.consumed == 0 && !rep.done)
        {
            TASK_CHECK(false);
            break;
        }
    }
    TASK_CHECK(pos == f.n);

    // Repeated calls after completion keep the completed state.
    ProcessReport tail1 = r.process(nullptr, 0, false, out.data(), 32);
    ProcessReport tail2 = r.process(nullptr, 0, false, out.data(), 32);
    TASK_CHECK(tail1.done && tail2.done);
    TASK_CHECK(tail1.produced == 0 && tail2.produced == 0);
    TASK_CHECK(tail1.first == r.nextOutputFrame());

    // Non-empty input after completion throws and leaves state untouched.
    uint64_t outCursor = r.nextOutputFrame();
    uint64_t srcCursor = r.nextSourceFrame();
    bool threw = false;
    try
    {
        r.process(in.data(), 1, false, out.data(), 32);
    }
    catch (const ContinuousRKError &)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(r.nextOutputFrame() == outCursor);
    TASK_CHECK(r.nextSourceFrame() == srcCursor);
    TASK_CHECK(r.finished());
    (void)total;
    return 0;
}

static int streamingDepthFailureRollback(void)
{
    const uint32_t sr = 48000, tr = 48000;
    const size_t n = 8, ch = 2;
    std::vector<std::vector<float>> mild(ch, std::vector<float>(n, 0.0f));
    for (size_t i = 0; i < n; ++i)
    {
        mild[0][i] = float(0.3 * std::sin(0.2 * i));
        mild[1][i] = float(0.25 * std::cos(0.17 * i));
    }
    std::vector<ControlPoint> cps = {{0, 800.0, 0.5}};

    // A small depth lets the mild opening segments accept after one split,
    // while the impulse frame forces rejections that exhaust the depth budget.
    ContinuousRKRenderer r(sr, tr, ch, 0, cps, 1e-9, 1);
    std::vector<const float *> in(ch);
    std::vector<std::vector<float>> outBuf(ch, std::vector<float>(16));
    std::vector<float *> out(ch);
    for (size_t k = 0; k < ch; ++k) out[k] = outBuf[k].data();
    for (size_t k = 0; k < ch; ++k) in[k] = mild[k].data();

    ProcessReport first = r.process(in.data(), n - 1, false, out.data(), 16);
    TASK_CHECK(first.consumed == n - 1);
    TASK_CHECK(r.acceptedSteps() > 0);

    double saved[2][4];
    for (size_t k = 0; k < ch; ++k)
        for (int i = 0; i < 4; ++i)
            saved[k][i] = r.stageState(k, i);

    std::vector<std::vector<float>> harsh = mild;
    harsh[0][n - 1] = 6.0f;
    harsh[1][n - 1] = -5.5f;
    for (size_t k = 0; k < ch; ++k) in[k] = harsh[k].data();

    uint64_t rejectedBefore = r.rejectedSteps();
    uint64_t derivBefore = r.derivativeCalls();
    uint64_t srcBefore = r.nextSourceFrame();
    bool threw = false;
    try
    {
        r.process(in.data(), 1, false, out.data(), 16);
    }
    catch (const StepDepthExceeded &)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(r.rejectedSteps() > rejectedBefore);   // diagnostics keep the work
    TASK_CHECK(r.derivativeCalls() > derivBefore);
    TASK_CHECK(r.nextSourceFrame() == srcBefore + 1);  // ingestion stands...
    for (size_t k = 0; k < ch; ++k)
        for (int i = 0; i < 4; ++i)
            TASK_CHECK(r.stageState(k, i) == saved[k][i]); // ...but states roll back
    return 0;
}

static int streamingIllegalConfiguration(void)
{
    std::vector<ControlPoint> ok = {{0, 500.0, 1.0}};
    auto throws_ = [](std::function<void()> f) {
        try { f(); } catch (const ContinuousRKError &) { return true; }
        return false;
    };

    TASK_CHECK(throws_([&] { ContinuousRKRenderer r(7999, 48000, 2, 0, ok); }));
    TASK_CHECK(throws_([&] { ContinuousRKRenderer r(48000, 192001, 2, 0, ok); }));
    TASK_CHECK(throws_([&] { ContinuousRKRenderer r(48000, 48000, 3, 0, ok); }));
    TASK_CHECK(throws_([&] { ContinuousRKRenderer r(48000, 48000, 2,
                                                    (uint64_t(1) << 55) + 1, ok); }));
    TASK_CHECK(throws_([&] { ContinuousRKRenderer r(48000, 48000, 2, 0, ok, 1e-11); }));
    TASK_CHECK(throws_([&] { ContinuousRKRenderer r(48000, 48000, 2, 0, ok, 1e-8, 21); }));
    TASK_CHECK(throws_([&] { ContinuousRKRenderer r(48000, 48000, 2, 0, {}); }));
    TASK_CHECK(throws_([&] { ContinuousRKRenderer r(48000, 48000, 2, 7, ok); }));

    std::vector<ControlPoint> badOrder = {{0, 500.0, 1.0}, {0, 800.0, 1.0}};
    TASK_CHECK(throws_([&] { ContinuousRKRenderer r(48000, 48000, 2, 0, badOrder); }));
    std::vector<ControlPoint> badCut = {{0, 5001.0, 1.0}};
    TASK_CHECK(throws_([&] { ContinuousRKRenderer r(48000, 48000, 2, 0, badCut); }));
    std::vector<ControlPoint> badRes = {{0, 500.0, 3.6}};
    TASK_CHECK(throws_([&] { ContinuousRKRenderer r(48000, 48000, 2, 0, badRes); }));

    // Illegal call counts throw without changing an existing renderer.
    ContinuousRKRenderer r(48000, 48000, 2, 0, ok);
    std::vector<const float *> in(2, nullptr);
    std::vector<std::vector<float>> ob(2, std::vector<float>(8));
    std::vector<float *> out = {ob[0].data(), ob[1].data()};
    uint64_t cursor = r.nextOutputFrame();
    TASK_CHECK(throws_([&] { r.process(in.data(), 4097, false, out.data(), 8); }));
    TASK_CHECK(r.nextOutputFrame() == cursor);
    return 0;
}

const task_case task_cases[] = {
    {"Continuous", "StrongDrive", continuousStrongDrive},
    {"Continuous", "SixChannels", continuousSixChannels},
    {"Continuous", "ControlNodeSweep", continuousCrossesControlNodes},

    {"Clock", "Rate44100To48000", clock44To48},
    {"Clock", "Rate48000To32000", clock48To32},
    {"Clock", "SameRate", clockSameRate},
    {"Clock", "EmptyInput", clockEmpty},
    {"Clock", "SingleFrame", clockSingleFrame},
    {"Clock", "HugeOriginAndPermutation", clockHugeOrigin},

    {"Streaming", "BitExactChunking", streamingBitExactChunks},
    {"Streaming", "ZeroCapacity", streamingZeroCapacity},
    {"Streaming", "StarvationThenFeed", streamingStarvation},
    {"Streaming", "EofDrainAndReuse", streamingEofDrain},
    {"Streaming", "DepthFailureRollback", streamingDepthFailureRollback},
    {"Streaming", "IllegalConfiguration", streamingIllegalConfiguration},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
