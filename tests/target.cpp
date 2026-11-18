/*
target.cpp

Regression tests for MoogLadders::ContinuousRKRenderer.

Three registered groups (driven by task_support/driver.c):

  Continuous - numerical agreement with an independent fixed-step RK4
               reference built from the test's own copy of the differential
               equation; reference step halving convergence; strong drive,
               high resonance, multiple channels and a control sweep that
               crosses node boundaries; the adaptive solver must reject steps.
  Clock      - exact integer/rational output timing for several rate pairs,
               empty/single-frame streams, origins beyond 2^53 and channel
               permutation.
  Streaming  - bit-exact determinism across input/output chunkings, zero
               capacity, starvation/refill, EOF draining, illegal-use
               exceptions and whole-segment rollback on depth exhaustion.

Only the C++17 standard library is used.
*/

#include "src/ContinuousRKRenderer.h"
#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using MoogLadders::ContinuousRKRenderer;
using MoogLadders::ControlPoint;
using MoogLadders::ProcessResult;

namespace {

constexpr double kPi = 3.1415926535897932384626433832795;

// ---------------------------------------------------------------------------
// Independent reference: the test's own formulation of the same ladder ODE.
// Time is measured in source frames (local coordinate starting at zero) and
// integration uses classical RK4 on a fixed micro-grid with shortened steps
// that stop exactly on every target sampling instant.
// ---------------------------------------------------------------------------
struct ReferenceLadder
{
    static double saturation(double z)
    {
        double v = z / 3.0;
        if (v > 1.0) v = 1.0;
        if (v < -1.0) v = -1.0;
        return 3.0 * (v - v * v * v / 3.0);
    }

    static void derivatives(double x, double k, double r,
                            const double y[4], double d[4])
    {
        d[0] = k * (saturation(x - r * y[3]) - saturation(y[0]));
        d[1] = k * (saturation(y[0]) - saturation(y[1]));
        d[2] = k * (saturation(y[1]) - saturation(y[2]));
        d[3] = k * (saturation(y[2]) - saturation(y[3]));
    }

    // Piecewise-linear source reconstruction, zero beyond the last sample.
    const std::vector<std::vector<float>>& input;
    std::size_t frames;
    double sourceRate;

    double inputAt(std::size_t ch, double t) const
    {
        if (t >= double(frames))
            return 0.0;
        long i = (long)std::floor(t);
        double w = t - double(i);
        double v0 = input[ch][i];
        double v1 = (i + 1 < (long)frames) ? input[ch][i + 1] : 0.0;
        return v0 + w * (v1 - v0);
    }

    void controlAt(const std::vector<ControlPoint>& cp, double t,
                   double& k, double& r) const
    {
        std::size_t i = 0;
        while (i + 1 < cp.size() && cp[i + 1].frame <= (uint64_t)t)
            ++i;
        if (i + 1 == cp.size())
        {
            k = 2.0 * kPi * cp[i].cutoff;
            r = cp[i].resonance;
            return;
        }
        double a = (t - double(cp[i].frame)) /
                   double(cp[i + 1].frame - cp[i].frame);
        double cutoff = cp[i].cutoff +
                        a * (cp[i + 1].cutoff - cp[i].cutoff);
        r = cp[i].resonance + a * (cp[i + 1].resonance - cp[i].resonance);
        k = 2.0 * kPi * cutoff;
    }

    // Returns the fourth-stage value at every target instant j/T with
    // j*sourceRate/targetRate < frames, one vector per channel.
    std::vector<std::vector<double>>
    run(uint32_t targetRate, std::size_t channels,
        const std::vector<ControlPoint>& cp, std::size_t sub) const
    {
        std::vector<std::vector<double>> y(channels,
                                           std::vector<double>(4, 0.0));
        std::vector<std::vector<double>> out(channels);

        const double hFrame = 1.0 / double(sub);
        double t = 0.0;
        long nextMicro = 1;
        uint64_t j = 0;
        const double end = double(frames);

        auto targetFrameTime = [&](uint64_t jj) {
            return double(jj) * sourceRate / double(targetRate);
        };

        while (t < end - 1e-15)
        {
            double microT = double(nextMicro) * hFrame;
            double tgtT = targetFrameTime(j);
            double t1 = std::min(microT, tgtT);
            if (t1 > end)
                t1 = end;
            double h = (t1 - t) / sourceRate;
            double mid = 0.5 * (t + t1);

            for (std::size_t c = 0; c < channels; ++c)
            {
                double d1[4], d2[4], d3[4], d4[4], tmp[4];
                auto eval = [&](double tt, const double state[4],
                                double dd[4]) {
                    double kk, rr;
                    controlAt(cp, tt, kk, rr);
                    derivatives(inputAt(c, tt), kk, rr, state, dd);
                };
                eval(t, y[c].data(), d1);
                for (int q = 0; q < 4; ++q)
                    tmp[q] = y[c][q] + 0.5 * h * d1[q];
                eval(mid, tmp, d2);
                for (int q = 0; q < 4; ++q)
                    tmp[q] = y[c][q] + 0.5 * h * d2[q];
                eval(mid, tmp, d3);
                for (int q = 0; q < 4; ++q)
                    tmp[q] = y[c][q] + h * d3[q];
                eval(t1, tmp, d4);
                for (int q = 0; q < 4; ++q)
                    y[c][q] += (h / 6.0) *
                               (d1[q] + 2.0 * d2[q] + 2.0 * d3[q] + d4[q]);
            }
            t = t1;

            if (std::fabs(t - tgtT) < 1e-12 && tgtT < end - 1e-12)
            {
                for (std::size_t c = 0; c < channels; ++c)
                    out[c].push_back(y[c][3]);
                ++j;
            }
            if (std::fabs(t - microT) < 1e-12)
                ++nextMicro;
        }
        return out;
    }
};

// Strong, wide-band planar test signal exercising the saturation region.
std::vector<std::vector<float>>
makeSignal(std::size_t channels, std::size_t n, uint64_t origin, int salt)
{
    std::vector<std::vector<float>> x(channels, std::vector<float>(n));
    for (std::size_t c = 0; c < channels; ++c)
        for (std::size_t i = 0; i < n; ++i)
        {
            double v = 2.2 * std::sin(0.073 * i + c * 1.9 + salt) +
                       1.1 * std::sin(0.311 * i + c * 0.7) +
                       0.8 * std::sin(0.903 * i);
            if (i % 29 == 3 + c)
                v += (c % 2 ? -6.0 : 6.0); // hard impulse at legal limit
            if (i % 17 == 0)
                v = (i % 34 == 0) ? 5.5 : -5.5; // square-wave corners
            // The streaming contract requires finite |sample| <= 6.
            v = std::max(-6.0, std::min(6.0, v));
            x[c][i] = float(v);
        }
    (void)origin;
    return x;
}

uint32_t floatBits(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

// One-shot offline render through the class under test.
struct ClassRender
{
    std::vector<std::vector<float>> audio;
    uint64_t first = 0;
    uint64_t accepted = 0, rejected = 0, derivatives = 0;
    uint64_t nextIn = 0, nextOut = 0;
    bool done = false;
};

ClassRender renderOnce(uint32_t sr, uint32_t tr, std::size_t ch,
                       uint64_t origin, const std::vector<ControlPoint>& cp,
                       const std::vector<std::vector<float>>& x,
                       double tolerance, uint32_t maxDepth)
{
    ContinuousRKRenderer r(sr, tr, ch, origin, cp, tolerance, maxDepth);
    std::size_t n = x[0].size();
    std::vector<const float*> in(ch);
    for (std::size_t c = 0; c < ch; ++c)
        in[c] = x[c].data();
    std::vector<std::vector<float>> buf(ch, std::vector<float>(n * 2 + 8));
    std::vector<float*> out(ch);
    for (std::size_t c = 0; c < ch; ++c)
        out[c] = buf[c].data();

    ClassRender cr;
    cr.audio.resize(ch);
    ProcessResult pr = r.process(in.data(), n, true, out.data(),
                                 buf[0].size());
    cr.first = pr.first;
    cr.done = pr.done;
    for (std::size_t c = 0; c < ch; ++c)
        cr.audio[c].assign(buf[c].begin(), buf[c].begin() + pr.produced);
    cr.accepted = r.acceptedSteps();
    cr.rejected = r.rejectedSteps();
    cr.derivatives = r.derivativeCalls();
    cr.nextIn = r.nextSourceFrame();
    cr.nextOut = r.nextOutputFrame();
    return cr;
}

// Generic chunked streaming render; mirrors the required streaming contract.
ClassRender renderChunked(uint32_t sr, uint32_t tr, std::size_t ch,
                          uint64_t origin,
                          const std::vector<ControlPoint>& cp,
                          const std::vector<std::vector<float>>& x,
                          std::size_t inChunk, std::size_t outCap,
                          double tolerance, uint32_t maxDepth,
                          std::size_t zeroCapEvery = 0)
{
    ContinuousRKRenderer r(sr, tr, ch, origin, cp, tolerance, maxDepth);
    std::size_t n = x[0].size();
    ClassRender cr;
    std::vector<std::vector<float>> collected(ch);
    std::size_t pos = 0;
    uint64_t firstReported = 0;
    bool haveFirst = false;
    uint64_t calls = 0;

    while (true)
    {
        // Optional zero-capacity probes: must never consume or latch.
        if (zeroCapEvery && (calls % zeroCapEvery) == 0 && pos < n)
        {
            std::vector<const float*> probe(ch);
            for (std::size_t c = 0; c < ch; ++c)
                probe[c] = x[c].data() + pos;
            std::vector<float*> sink(ch, nullptr);
            ProcessResult z = r.process(probe.data(),
                                        std::min(inChunk, n - pos),
                                        false, sink.data(), 0);
            if (z.consumed != 0 || z.produced != 0)
            {
                cr.done = false;
                return cr; // signal failure through done flag
            }
        }
        ++calls;

        std::size_t take = std::min(inChunk, n - pos);
        std::vector<const float*> in(ch);
        for (std::size_t c = 0; c < ch; ++c)
            in[c] = x[c].data() + pos;
        std::vector<std::vector<float>> obuf(ch,
                                             std::vector<float>(outCap));
        std::vector<float*> out(ch);
        for (std::size_t c = 0; c < ch; ++c)
            out[c] = obuf[c].data();

        bool last = (pos + take == n);
        ProcessResult pr = r.process(in.data(), take, last, out.data(),
                                     outCap);
        if (!haveFirst && pr.produced)
        {
            firstReported = pr.first;
            haveFirst = true;
        }
        for (std::size_t c = 0; c < ch; ++c)
            collected[c].insert(collected[c].end(), obuf[c].begin(),
                                obuf[c].begin() + pr.produced);
        pos += pr.consumed;

        if (pr.done)
            break;
        if (pos == n)
        {
            // End may not be latched yet (partial final consumption); drain
            // with zero frames, never guessing new source endpoints.
            std::vector<std::vector<float>> dbuf(ch,
                                                 std::vector<float>(outCap));
            std::vector<float*> dout(ch);
            for (std::size_t c = 0; c < ch; ++c)
                dout[c] = dbuf[c].data();
            ProcessResult q = r.process(nullptr, 0, false, dout.data(),
                                        outCap);
            for (std::size_t c = 0; c < ch; ++c)
                collected[c].insert(collected[c].end(), dbuf[c].begin(),
                                    dbuf[c].begin() + q.produced);
            if (q.done)
                break;
        }
    }

    cr.audio = std::move(collected);
    cr.first = firstReported;
    cr.done = true;
    cr.accepted = r.acceptedSteps();
    cr.rejected = r.rejectedSteps();
    cr.derivatives = r.derivativeCalls();
    cr.nextIn = r.nextSourceFrame();
    cr.nextOut = r.nextOutputFrame();
    return cr;
}

bool sameBits(const ClassRender& a, const ClassRender& b, std::size_t ch)
{
    if (a.audio.size() != ch || b.audio.size() != ch)
        return false;
    for (std::size_t c = 0; c < ch; ++c)
    {
        if (a.audio[c].size() != b.audio[c].size())
            return false;
        for (std::size_t i = 0; i < a.audio[c].size(); ++i)
            if (floatBits(a.audio[c][i]) != floatBits(b.audio[c][i]))
                return false;
    }
    return a.accepted == b.accepted && a.rejected == b.rejected &&
           a.derivatives == b.derivatives && a.nextIn == b.nextIn &&
           a.nextOut == b.nextOut && a.first == b.first;
}

// Expected first absolute target index: ceil(origin * tr / sr).
uint64_t ceilDiv(uint64_t origin, uint32_t tr, uint32_t sr)
{
    unsigned __int128 p = (unsigned __int128)origin * tr;
    return uint64_t((p + sr - 1) / sr);
}

// Count of target frames strictly before the terminal source time.
uint64_t expectedCount(uint64_t origin, std::size_t n, uint32_t tr,
                       uint32_t sr)
{
    uint64_t first = ceilDiv(origin, tr, sr);
    uint64_t terminal = origin + n;
    // largest j with j * sr < terminal * tr.
    unsigned __int128 rhs = (unsigned __int128)terminal * tr;
    uint64_t j = first;
    while ((unsigned __int128)j * sr < rhs)
        ++j;
    return j - first; // j is now the first excluded index
}

} // namespace

// ===========================================================================
// Continuous group
// ===========================================================================
static int continuous_reference()
{
    const uint32_t sr = 48000, tr = 44100;
    const std::size_t ch = 2, n = 400;
    const uint64_t origin = 0;
    std::vector<ControlPoint> cp = {
        {origin + 0, 200.0, 0.5},
        {origin + 150, 4000.0, 3.4},      // high resonance node
        {origin + 300, 80.0, 1.5},
        {origin + n, 1200.0, 2.0}};
    auto x = makeSignal(ch, n, origin, 1);

    ClassRender cr = renderOnce(sr, tr, ch, origin, cp, x, 1e-9, 20);
    TASK_CHECK(cr.done);
    TASK_CHECK(cr.rejected > 0);                 // adaptive rejection happens
    TASK_CHECK(cr.derivatives > cr.accepted * 8);

    ReferenceLadder ref{x, n, double(sr)};
    auto fine = ref.run(tr, ch, cp, 256);
    auto coarse = ref.run(tr, ch, cp, 128);

    std::size_t count = cr.audio[0].size();
    TASK_CHECK(count > 0);
    TASK_CHECK(fine[0].size() == count);
    TASK_CHECK(coarse[0].size() == count);

    double maxErrClass = 0.0, maxSelfConv = 0.0, maxCoarse = 0.0;
    for (std::size_t c = 0; c < ch; ++c)
        for (std::size_t i = 0; i < count; ++i)
        {
            maxErrClass = std::max(
                maxErrClass, std::fabs(cr.audio[c][i] - fine[c][i]));
            maxSelfConv = std::max(
                maxSelfConv, std::fabs(fine[c][i] - coarse[c][i]));
            maxCoarse = std::max(
                maxCoarse, std::fabs(cr.audio[c][i] - coarse[c][i]));
        }

    // Reference converges under step halving (RK4 shrinks the error sharply).
    TASK_CHECK(maxSelfConv > 0.0);
    TASK_CHECK(maxSelfConv < 1e-7);
    // Class output agrees with the independent fine reference.
    TASK_CHECK(maxErrClass <= 2e-5);
    TASK_CHECK(maxCoarse <= 2e-5);
    return 0;
}

static int continuous_six_channels()
{
    const uint32_t sr = 96000, tr = 8000;
    const std::size_t ch = 6, n = 257;
    const uint64_t origin = 0;
    std::vector<ControlPoint> cp = {
        {origin, 3000.0, 2.7},
        {origin + 128, 600.0, 0.2},
        {origin + n, 3000.0, 2.7}};
    auto x = makeSignal(ch, n, origin, 7);
    ClassRender cr = renderOnce(sr, tr, ch, origin, cp, x, 1e-9, 20);
    TASK_CHECK(cr.done);
    TASK_CHECK(cr.rejected > 0);

    ReferenceLadder ref{x, n, double(sr)};
    auto fine = ref.run(tr, ch, cp, 192);
    TASK_CHECK(fine[0].size() == cr.audio[0].size());
    double maxErr = 0.0;
    for (std::size_t c = 0; c < ch; ++c)
        for (std::size_t i = 0; i < fine[c].size(); ++i)
            maxErr = std::max(maxErr,
                              std::fabs(cr.audio[c][i] - fine[c][i]));
    TASK_CHECK(maxErr <= 2e-5);

    // Distinct channels must produce distinct, finite fourth-stage outputs.
    // The sample at the time origin is the shared zero initial state, so the
    // permutation check starts once the ladder has responded.
    for (std::size_t i = 0; i < cr.audio[0].size(); ++i)
        for (std::size_t c = 0; c < ch; ++c)
            TASK_CHECK(std::isfinite(cr.audio[c][i]));
    TASK_CHECK(cr.audio[0].size() > 4);
    TASK_CHECK(cr.audio[0][0] == 0.0f);
    bool channelsDiffer = false;
    for (std::size_t i = 4; i < cr.audio[0].size() && !channelsDiffer; ++i)
        for (std::size_t a = 0; a < ch && !channelsDiffer; ++a)
            for (std::size_t b = a + 1; b < ch; ++b)
                if (floatBits(cr.audio[a][i]) !=
                    floatBits(cr.audio[b][i]))
                {
                    channelsDiffer = true;
                    break;
                }
    TASK_CHECK(channelsDiffer);
    return 0;
}

static int continuous_zero_cutoff_hold()
{
    // Cutoff reaching zero freezes the ladder states: every output at or after
    // the freeze instant must be the identical float (derivatives exactly 0,
    // so RK cannot drift), and the held tail keeps that constant.
    const uint32_t sr = 48000, tr = 48000;
    const std::size_t ch = 1, n = 64;
    std::vector<ControlPoint> cp = {{0, 1500.0, 2.0}, {32, 0.0, 2.0}};
    auto x = makeSignal(ch, n, 0, 3);
    ContinuousRKRenderer probe(sr, tr, ch, 0, cp, 1e-10, 20);
    const float* in = x[0].data();
    float out[128];
    float* op = out;
    ProcessResult pr = probe.process(&in, n, true, &op, 128);
    TASK_CHECK(pr.done);
    TASK_CHECK(pr.produced > 40);
    // Target frame j coincides with source frame j at equal rates.
    float frozen = out[32];
    TASK_CHECK(std::isfinite(frozen));
    for (std::size_t i = 33; i < pr.produced; ++i)
        TASK_CHECK(floatBits(out[i]) == floatBits(frozen));
    // The frozen fourth state is still readable and matches the emitted value.
    double s[4];
    probe.copyStates(0, s);
    TASK_CHECK(float(s[3]) == frozen);
    return 0;
}

// ===========================================================================
// Clock group
// ===========================================================================
static int clock_rate_pairs()
{
    struct Pair { uint32_t sr, tr; };
    Pair pairs[] = {{44100, 48000}, {48000, 32000}, {48000, 48000}};
    const std::size_t ch = 2, n = 137;
    for (const Pair& p : pairs)
    {
        std::vector<ControlPoint> cp = {
            {0, 1000.0, 1.0}, {70, 2500.0, 2.5}, {n, 500.0, 1.0}};
        auto x = makeSignal(ch, n, 0, 11);
        ClassRender cr = renderOnce(p.sr, p.tr, ch, 0, cp, x, 1e-9, 20);
        TASK_CHECK(cr.done);

        uint64_t first = ceilDiv(0, p.tr, p.sr);
        uint64_t count = expectedCount(0, n, p.tr, p.sr);
        TASK_CHECK(cr.first == first);
        TASK_CHECK(cr.audio[0].size() == count);
        TASK_CHECK(cr.nextOut == first + count);

        // Every emitted index j satisfies j*sr/tr < n (strict terminal rule)
        // and j is contiguous from the first index.
        for (uint64_t j = cr.first; j < cr.nextOut; ++j)
        {
            unsigned __int128 lhs = (unsigned __int128)j * p.sr;
            unsigned __int128 rhs = (unsigned __int128)n * p.tr;
            TASK_CHECK(lhs < rhs);
        }
        for (std::size_t c = 0; c < ch; ++c)
            for (float v : cr.audio[c])
                TASK_CHECK(std::isfinite(v));
    }
    return 0;
}

static int clock_empty_and_single()
{
    const uint32_t sr = 48000, tr = 48000;
    const std::size_t ch = 1;
    std::vector<ControlPoint> cp = {{0, 1000.0, 1.0}};

    // Empty input produces nothing and never completes on its own.
    {
        ContinuousRKRenderer r(sr, tr, ch, 0, cp, 1e-9, 20);
        const float* in = nullptr;
        float* out = nullptr;
        ProcessResult pr = r.process(&in, 0, false, &out, 64);
        TASK_CHECK(pr.consumed == 0 && pr.produced == 0);
        TASK_CHECK(pr.first == 0);
        TASK_CHECK(!pr.done);
    }

    // A single source frame spans [0, 1); only target time zero qualifies,
    // where the ladder states are still all zero.
    {
        ContinuousRKRenderer r(sr, tr, ch, 0, cp, 1e-9, 20);
        float one = 4.0f;
        const float* in = &one;
        float out[8];
        float* op = out;
        ProcessResult pr = r.process(&in, 1, true, &op, 8);
        TASK_CHECK(pr.consumed == 1);
        TASK_CHECK(pr.produced == 1);
        TASK_CHECK(pr.first == 0);
        TASK_CHECK(out[0] == 0.0f);
        TASK_CHECK(pr.done);
        // Repeated calls after completion stay completed.
        ProcessResult again = r.process(&in, 0, false, &op, 8);
        TASK_CHECK(again.done && again.produced == 0);
    }
    return 0;
}

static int clock_large_origin()
{
    // Origin beyond 2^53: absolute frames must stay exact integers.
    const std::size_t ch = 2, n = 96;
    const uint64_t big = (uint64_t(1) << 54) + 1234567;
    auto x = makeSignal(ch, n, big, 5);

    // At equal rates the target grid lands exactly on source frames, so the
    // local relative alignment is identical to an origin-zero render: outputs
    // must match bit for bit, proving only local differences enter the math.
    {
        const uint32_t rate = 48000;
        std::vector<ControlPoint> cpBig = {
            {big, 1200.0, 2.0}, {big + 40, 3000.0, 3.0},
            {big + n, 800.0, 1.0}};
        std::vector<ControlPoint> cpZero = {
            {0, 1200.0, 2.0}, {40, 3000.0, 3.0}, {n, 800.0, 1.0}};
        ClassRender bigRender =
            renderOnce(rate, rate, ch, big, cpBig, x, 1e-9, 20);
        ClassRender zeroRender =
            renderOnce(rate, rate, ch, 0, cpZero, x, 1e-9, 20);
        TASK_CHECK(bigRender.done && zeroRender.done);
        // Same local problem: identical samples, step counts and cursors.
        TASK_CHECK(bigRender.audio.size() == ch);
        for (std::size_t c = 0; c < ch; ++c)
        {
            TASK_CHECK(bigRender.audio[c].size() ==
                       zeroRender.audio[c].size());
            for (std::size_t i = 0; i < bigRender.audio[c].size(); ++i)
                TASK_CHECK(floatBits(bigRender.audio[c][i]) ==
                           floatBits(zeroRender.audio[c][i]));
        }
        TASK_CHECK(bigRender.accepted == zeroRender.accepted &&
                   bigRender.rejected == zeroRender.rejected &&
                   bigRender.derivatives == zeroRender.derivatives);
        TASK_CHECK(bigRender.first == big);
        TASK_CHECK(bigRender.nextIn == big + n + 1);
        TASK_CHECK(zeroRender.nextIn == n + 1);
    }

    // Rational rates: the fractional phase relative to the origin differs, so
    // only the exact integer clock (first index, count, terminal exclusion) is
    // asserted, never via a double conversion of the absolute frame.
    {
        const uint32_t sr = 44100, tr = 48000;
        std::vector<ControlPoint> cpBig = {
            {big, 1200.0, 2.0}, {big + 40, 3000.0, 3.0},
            {big + n, 800.0, 1.0}};
        ClassRender r = renderOnce(sr, tr, ch, big, cpBig, x, 1e-9, 20);
        TASK_CHECK(r.done);
        TASK_CHECK(r.first == ceilDiv(big, tr, sr));
        TASK_CHECK(r.audio[0].size() ==
                   expectedCount(big, n, tr, sr));
        for (std::size_t c = 0; c < ch; ++c)
            for (float v : r.audio[c])
                TASK_CHECK(std::isfinite(v));
    }
    return 0;
}

static int clock_channel_permutation()
{
    const uint32_t sr = 48000, tr = 41000;
    const std::size_t ch = 6, n = 120;
    auto x = makeSignal(ch, n, 0, 13);
    std::vector<ControlPoint> cp = {{0, 2000.0, 2.2}, {n, 900.0, 1.1}};

    ClassRender ref = renderOnce(sr, tr, ch, 0, cp, x, 1e-9, 20);

    // Permute channels 0..5 and confirm outputs follow the same permutation.
    std::size_t perm[6] = {5, 2, 0, 4, 1, 3};
    std::vector<std::vector<float>> xp(ch, std::vector<float>(n));
    for (std::size_t c = 0; c < ch; ++c)
        xp[c] = x[perm[c]];
    ClassRender got = renderOnce(sr, tr, ch, 0, cp, xp, 1e-9, 20);

    TASK_CHECK(ref.audio[0].size() == got.audio[0].size());
    for (std::size_t c = 0; c < ch; ++c)
        for (std::size_t i = 0; i < ref.audio[c].size(); ++i)
            TASK_CHECK(floatBits(got.audio[c][i]) ==
                       floatBits(ref.audio[perm[c]][i]));
    return 0;
}

// ===========================================================================
// Streaming group
// ===========================================================================
static int streaming_chunk_determinism()
{
    const uint32_t sr = 44100, tr = 48000;
    const std::size_t ch = 2, n = 600;
    auto x = makeSignal(ch, n, 0, 17);
    std::vector<ControlPoint> cp = {
        {0, 400.0, 0.8}, {210, 3200.0, 3.1}, {430, 700.0, 1.4},
        {n, 1500.0, 2.2}};

    ClassRender whole =
        renderChunked(sr, tr, ch, 0, cp, x, n, 4096, 1e-9, 20);
    TASK_CHECK(whole.done);
    TASK_CHECK(whole.rejected > 0);

    const std::size_t inChunks[] = {1, 7, 127};
    const std::size_t outCaps[] = {1, 5, 113};
    for (std::size_t ic : inChunks)
        for (std::size_t oc : outCaps)
        {
            ClassRender r =
                renderChunked(sr, tr, ch, 0, cp, x, ic, oc, 1e-9, 20);
            TASK_CHECK(r.done);
            TASK_CHECK(sameBits(whole, r, ch));
        }

    // Zero-capacity probes interleaved into the feed change nothing.
    ClassRender probed = renderChunked(sr, tr, ch, 0, cp, x, 37, 19,
                                       1e-9, 20, 3);
    TASK_CHECK(probed.done);
    TASK_CHECK(sameBits(whole, probed, ch));
    return 0;
}

static int streaming_starve_refill_and_eof()
{
    const uint32_t sr = 48000, tr = 32000;
    const std::size_t ch = 1, n = 300;
    auto x = makeSignal(ch, n, 0, 23);
    std::vector<ControlPoint> cp = {{0, 1500.0, 2.0}, {n, 600.0, 1.0}};

    ClassRender reference =
        renderChunked(sr, tr, ch, 0, cp, x, n, 4096, 1e-9, 20);

    // Hand feed where the renderer is repeatedly starved (one source frame
    // at a time, output capacity one) and end arrives on a non-final step.
    ContinuousRKRenderer r(sr, tr, ch, 0, cp, 1e-9, 20);
    std::vector<float> got;
    uint64_t pos = 0;
    bool latched = false;
    while (true)
    {
        float out[1];
        float* op = out;
        ProcessResult pr;
        if (pos < n)
        {
            const float* in = x[0].data() + pos;
            bool last = (pos + 1 == n);
            pr = r.process(&in, 1, last, &op, 1);
            pos += pr.consumed;
            if (last && pr.consumed == 1)
                latched = true;
        }
        else
        {
            pr = r.process(nullptr, 0, false, &op, 1);
        }
        if (pr.produced)
            got.push_back(out[0]);
        if (pr.done)
            break;
    }
    TASK_CHECK(latched);
    TASK_CHECK(got.size() == reference.audio[0].size());
    for (std::size_t i = 0; i < got.size(); ++i)
        TASK_CHECK(floatBits(got[i]) ==
                   floatBits(reference.audio[0][i]));
    TASK_CHECK(r.nextSourceFrame() == reference.nextIn);
    TASK_CHECK(r.nextOutputFrame() == reference.nextOut);
    TASK_CHECK(r.acceptedSteps() == reference.accepted);
    TASK_CHECK(r.rejectedSteps() == reference.rejected);
    TASK_CHECK(r.derivativeCalls() == reference.derivatives);
    return 0;
}

static int streaming_illegal_use()
{
    const uint32_t sr = 48000, tr = 48000;
    const std::size_t ch = 1, n = 32;
    auto x = makeSignal(ch, n, 0, 29);
    std::vector<ControlPoint> good = {{0, 1000.0, 1.0}, {n, 500.0, 1.0}};

    auto throwsArg = [&](auto&& build) {
        bool threw = false;
        try { build(); } catch (const std::exception&) { threw = true; }
        return threw;
    };

    TASK_CHECK(throwsArg([&] {
        ContinuousRKRenderer bad(7999, tr, ch, 0, good);
    }));
    TASK_CHECK(throwsArg([&] {
        ContinuousRKRenderer bad(sr, 192001, ch, 0, good);
    }));
    TASK_CHECK(throwsArg([&] {
        ContinuousRKRenderer bad(sr, tr, 3, 0, good);
    }));
    std::vector<ControlPoint> wrongStart = {{1, 1000.0, 1.0}};
    TASK_CHECK(throwsArg([&] {
        ContinuousRKRenderer bad(sr, tr, ch, 0, wrongStart);
    }));
    std::vector<ControlPoint> badCut = {{0, 5001.0, 1.0}};
    TASK_CHECK(throwsArg([&] {
        ContinuousRKRenderer bad(sr, tr, ch, 0, badCut);
    }));
    std::vector<ControlPoint> badRes = {{0, 1000.0, 3.6}};
    TASK_CHECK(throwsArg([&] {
        ContinuousRKRenderer bad(sr, tr, ch, 0, badRes);
    }));
    TASK_CHECK(throwsArg([&] {
        ContinuousRKRenderer bad(sr, tr, ch, 0, good, 1e-11, 20);
    }));
    TASK_CHECK(throwsArg([&] {
        ContinuousRKRenderer bad(sr, tr, ch, 0, good, 1e-9, 21);
    }));

    // Oversized block counts throw without mutating the renderer.
    ContinuousRKRenderer r(sr, tr, ch, 0, good, 1e-9, 20);
    std::vector<float> big(4097, 0.0f);
    const float* bp = big.data();
    float ob[4];
    float* op = ob;
    uint64_t inBefore = r.nextSourceFrame();
    uint64_t outBefore = r.nextOutputFrame();
    bool threw = false;
    try {
        r.process(&bp, 4097, false, &op, 4);
    } catch (const std::exception&) {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(r.nextSourceFrame() == inBefore);
    TASK_CHECK(r.nextOutputFrame() == outBefore);

    // Non-empty input after completion throws; completed state is retained.
    // Capacity one consumes only a prefix, so re-feed the unconsumed suffix
    // (end latches only once the whole presented input is consumed); after
    // that, zero-frame calls drain the remaining target samples.
    std::size_t fed = 0;
    const float* gp = x[0].data();
    ProcessResult done = r.process(&gp, n, true, &op, 1);
    fed += done.consumed;
    while (!done.done)
    {
        if (fed < n)
        {
            const float* rest = x[0].data() + fed;
            done = r.process(&rest, n - fed, true, &op, 1);
            fed += done.consumed;
        }
        else
        {
            done = r.process(nullptr, 0, false, &op, 1);
        }
    }
    TASK_CHECK(done.done);
    TASK_CHECK(fed == n);
    threw = false;
    try {
        r.process(&gp, 1, false, &op, 1);
    } catch (const std::exception&) {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(r.nextSourceFrame() == n + 1);
    return 0;
}

static int streaming_depth_exhaustion_rollback()
{
    // Low source rate, hard-driven discontinuous input and zero depth: the
    // first coarse segment is rejected and the renderer must throw with the
    // whole segment's states restored.
    const uint32_t sr = 8000, tr = 8000;
    const std::size_t ch = 2, n = 64;
    std::vector<ControlPoint> cp = {{0, 1500.0, 3.5}, {n, 1500.0, 3.5}};
    std::vector<std::vector<float>> x(ch, std::vector<float>(n));
    for (std::size_t c = 0; c < ch; ++c)
        for (std::size_t i = 0; i < n; ++i)
            x[c][i] = (i % 2) ? 6.0f : -6.0f;

    ContinuousRKRenderer r(sr, tr, ch, 0, cp, 1e-10, 0);
    std::vector<const float*> in(ch);
    for (std::size_t c = 0; c < ch; ++c)
        in[c] = x[c].data();
    float obuf[2][8];
    float* out[2] = {obuf[0], obuf[1]};

    double before[2][4];
    for (std::size_t c = 0; c < ch; ++c)
        r.copyStates(c, before[c]);
    uint64_t inBefore = r.nextSourceFrame();
    uint64_t outBefore = r.nextOutputFrame();

    bool threw = false;
    try {
        r.process(in.data(), n, true, out, 8);
    } catch (const std::exception&) {
        threw = true;
    }
    TASK_CHECK(threw);

    // All four states of every channel rolled back to the segment start, and
    // no non-trivial integration step was accepted.  The degenerate event at
    // the time origin may advance a cursor by at most one without integrating.
    for (std::size_t c = 0; c < ch; ++c)
    {
        double s[4];
        r.copyStates(c, s);
        for (int q = 0; q < 4; ++q)
            TASK_CHECK(s[q] == before[c][q]);
    }
    TASK_CHECK(r.nextSourceFrame() <= inBefore + 1);
    TASK_CHECK(r.nextOutputFrame() <= outBefore + 1);

    // Diagnostics retain the work the failed attempt actually performed.
    TASK_CHECK(r.rejectedSteps() >= 1);
    TASK_CHECK(r.derivativeCalls() > 0);
    TASK_CHECK(r.acceptedSteps() == 0);
    return 0;
}

// ===========================================================================
// Registration (external linkage so the C driver can resolve the symbols)
// ===========================================================================
extern const task_case task_cases[] = {
    {"Continuous", "FixedStepReference", continuous_reference},
    {"Continuous", "SixChannels", continuous_six_channels},
    {"Continuous", "ZeroCutoffHold", continuous_zero_cutoff_hold},
    {"Clock", "RatePairs", clock_rate_pairs},
    {"Clock", "EmptyAndSingle", clock_empty_and_single},
    {"Clock", "LargeOrigin", clock_large_origin},
    {"Clock", "ChannelPermutation", clock_channel_permutation},
    {"Streaming", "ChunkDeterminism", streaming_chunk_determinism},
    {"Streaming", "StarveRefillAndEOF", streaming_starve_refill_and_eof},
    {"Streaming", "IllegalUse", streaming_illegal_use},
    {"Streaming", "DepthExhaustionRollback",
     streaming_depth_exhaustion_rollback},
};

extern const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
