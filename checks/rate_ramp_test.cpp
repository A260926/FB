// Verification for the RateTransposer rate ramp mode (floating point build).
//
// Seven case groups. Each group prints "RATE_RAMP_CASE <name> PASS" only
// after its assertions have actually executed successfully; any failure
// exits non-zero immediately. The Ref class below is an independent
// re-implementation of the documented ramp clock used to cross-check the
// library: output frame k uses source coordinate x_k (x_0 = 0 after
// construction/clear) and increment r_k, then x_{k+1} = x_k + r_k. With a
// ramp of length N from r0 to rt, r_k = r0 + (rt - r0) * k / N for k < N and
// r_k = rt afterwards. All channels share the same coordinate. Anti-alias
// filtering (active for r_k > 1) centers a 64-tap Hamming-windowed sinc of
// cutoff 0.5/r_k (input-frame units, DC-normalized per frame) on the exact
// source coordinate; history before coordinate zero is zero.

#include "RateTransposer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

using namespace soundtouch;

#define REF_PI 3.14159265358979323846

static void check(bool ok, const char *msg)
{
    if (!ok)
    {
        std::fprintf(stderr, "RATE_RAMP_FAIL %s\n", msg);
        std::exit(1);
    }
}

static void checkClose(double a, double b, double tol, const char *msg)
{
    if (!(std::fabs(a - b) <= tol))
    {
        std::fprintf(stderr, "RATE_RAMP_FAIL %s (%.17g vs %.17g)\n", msg, a, b);
        std::exit(1);
    }
}

#define EXPECT_INVALID(expr)                                                \
    do {                                                                    \
        bool threw_ = false;                                                \
        try { expr; } catch (const std::invalid_argument &) { threw_ = true; } \
        check(threw_, "expected std::invalid_argument: " #expr);            \
    } while (0)

// Deterministic pseudo-random source (xorshift32).
struct Rng
{
    unsigned s;
    Rng(unsigned seed) : s(seed ? seed : 1u) {}
    double uniform()   // [-1, 1)
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (double)(s >> 8) / 8388608.0 - 1.0;
    }
};

// Independent reference implementation of the documented ramp clock.
struct Ref
{
    int nch;
    bool aa;
    bool cubic;
    double x, cur, r0, rt;
    unsigned total, done;
    std::vector<float> in;   // every input frame fed so far (interleaved)

    Ref(int channels, bool useAA, bool useCubic)
        : nch(channels), aa(useAA), cubic(useCubic),
          x(0.0), cur(1.0), r0(1.0), rt(1.0), total(0), done(0) {}

    double current() const
    {
        return (done < total) ? r0 + (rt - r0) * (double)done / (double)total : cur;
    }

    unsigned remaining() const
    {
        return (done < total) ? total - done : 0;
    }

    void setRamp(double target, unsigned n)
    {
        if (n == 0) { cur = target; total = 0; done = 0; return; }
        r0 = current(); rt = target; total = n; done = 0;
    }

    void setRate(double r) { cur = r; total = 0; done = 0; }

    void feed(const float *p, unsigned frames)
    {
        in.insert(in.end(), p, p + (size_t)frames * nch);
    }

    // Generates every frame the fed input allows, appends to 'out'.
    void generate(std::vector<float> &out)
    {
        for (;;)
        {
            double r = current();
            double iFloor = std::floor(x);
            long long i0 = (long long)iFloor;
            bool filtered = aa && r > 1.0;
            long long needLast = filtered ? i0 + 32 : (cubic ? i0 + 2 : i0 + 1);
            if ((long long)(in.size() / nch) <= needLast) break;

            double f = x - iFloor;
            if (filtered)
            {
                double wc = REF_PI / r;
                double coeffs[64];
                double sum = 0.0;
                for (int t = 0; t < 64; t++)
                {
                    long long j = i0 - 31 + t;
                    double d = (double)j - x;
                    double h = (d == 0.0) ? 1.0 : std::sin(d * wc) / (d * wc);
                    double w = 0.54 + 0.46 * std::cos(REF_PI * d / 32.0);
                    coeffs[t] = h * w;
                    sum += coeffs[t];
                }
                for (int c = 0; c < nch; c++)
                {
                    double acc = 0.0;
                    for (int t = 0; t < 64; t++)
                    {
                        long long j = i0 - 31 + t;
                        if (j >= 0) acc += coeffs[t] * (double)in[(size_t)j * nch + c];
                    }
                    out.push_back((float)(acc / sum));
                }
            }
            else if (cubic)
            {
                double f2 = f * f, f3 = f2 * f;
                double y0 = -0.5 * f3 +       f2 - 0.5 * f;
                double y1 =  1.5 * f3 - 2.5 * f2           + 1.0;
                double y2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
                double y3 =  0.5 * f3 - 0.5 * f2;
                for (int c = 0; c < nch; c++)
                {
                    double s0 = (i0 - 1 >= 0) ? (double)in[(size_t)(i0 - 1) * nch + c] : 0.0;
                    double s1 = (double)in[(size_t)i0 * nch + c];
                    double s2 = (double)in[(size_t)(i0 + 1) * nch + c];
                    double s3 = (double)in[(size_t)(i0 + 2) * nch + c];
                    out.push_back((float)(y0 * s0 + y1 * s1 + y2 * s2 + y3 * s3));
                }
            }
            else
            {
                for (int c = 0; c < nch; c++)
                {
                    double s1 = (double)in[(size_t)i0 * nch + c];
                    double s2 = (double)in[(size_t)(i0 + 1) * nch + c];
                    out.push_back((float)((1.0 - f) * s1 + f * s2));
                }
            }

            x += r;
            if (done < total)
            {
                done++;
                if (done == total) cur = rt;
            }
        }
    }
};

// Library harness helpers ---------------------------------------------------

static void makeRampLib(RateTransposer &rt, int channels, bool aa, bool cubic)
{
    TransposerBase::setAlgorithm(cubic ? TransposerBase::CUBIC : TransposerBase::LINEAR);
    rt.setChannels(channels);
    rt.enableAAFilter(aa ? TRUE : FALSE);
    rt.enableRateRamp(TRUE);
}

static void drainInto(RateTransposer &rt, std::vector<float> &out, int nch, unsigned maxChunk)
{
    std::vector<float> buf;
    for (;;)
    {
        unsigned avail = rt.numSamples();
        if (avail == 0) break;
        unsigned want = (maxChunk && avail > maxChunk) ? maxChunk : avail;
        buf.resize((size_t)want * nch);
        unsigned got = rt.receiveSamples(buf.data(), want);
        out.insert(out.end(), buf.begin(), buf.begin() + (size_t)got * nch);
        if (got == 0) break;
    }
}

// Feeds one chunk to library and reference, drains the library, and verifies
// the trajectory bookkeeping matches the reference after the call.
static void feedAndSync(RateTransposer &rt, Ref &ref, const float *p, unsigned frames,
                        std::vector<float> &libOut, std::vector<float> &refOut,
                        unsigned drainChunk)
{
    rt.putSamples(p, frames);
    ref.feed(p, frames);
    ref.generate(refOut);
    drainInto(rt, libOut, ref.nch, drainChunk);
    if (rt.getRateRampRemaining() != ref.remaining())
    {
        std::fprintf(stderr, "RATE_RAMP_FAIL remaining %u vs %u\n",
                     rt.getRateRampRemaining(), ref.remaining());
        std::exit(1);
    }
    checkClose(rt.getCurrentRate(), ref.current(), 1e-12, "current rate diverged");
}

static double compareOut(const std::vector<float> &a, const std::vector<float> &b,
                         double tol, const char *what)
{
    if (a.size() != b.size())
    {
        std::fprintf(stderr, "RATE_RAMP_FAIL %s: length %zu vs %zu\n", what, a.size(), b.size());
        std::exit(1);
    }
    double m = 0.0;
    for (size_t i = 0; i < a.size(); i++) m = std::max(m, (double)std::fabs(a[i] - b[i]));
    if (!(m <= tol))
    {
        std::fprintf(stderr, "RATE_RAMP_FAIL %s: max error %.9g > %.9g\n", what, m, tol);
        std::exit(1);
    }
    return m;
}

static std::vector<float> makeNoise(int nch, unsigned frames, unsigned seed)
{
    Rng rng(seed);
    std::vector<float> v((size_t)frames * nch);
    for (size_t i = 0; i < v.size(); i++) v[i] = (float)(0.8 * rng.uniform());
    return v;
}

// Case 1: coordinate ---------------------------------------------------------
// Double precision coordinate recursion + linear interpolation, checked
// against the independent reference, including an N=1 ramp and ramp lengths
// that are not multiples of the input block size.
static void caseCoordinate()
{
    const unsigned frames = 30000;
    std::vector<float> input = makeNoise(2, frames, 7);

    RateTransposer rt;
    makeRampLib(rt, 2, false, false);
    Ref ref(2, false, false);

    rt.setRateRamp(1.5, 1);          // N = 1 ramp
    ref.setRamp(1.5, 1);
    check(rt.getRateRampRemaining() == 1, "N=1 ramp pending");
    checkClose(rt.getCurrentRate(), 1.0, 1e-12, "N=1 ramp starts at current rate");

    const unsigned chunks[] = {1, 97, 1024, 5, 4096, 37};
    std::vector<float> libOut, refOut;
    unsigned pos = 0, ci = 0;
    bool ramp2 = false, ramp3 = false;
    while (pos < frames)
    {
        unsigned n = std::min(chunks[ci % 6], frames - pos);
        feedAndSync(rt, ref, input.data() + (size_t)pos * 2, n, libOut, refOut, 0);
        pos += n; ci++;
        if (!ramp2 && pos >= frames * 2 / 5)
        {
            rt.setRateRamp(0.75, 1009);   // not a multiple of any block size
            ref.setRamp(0.75, 1009);
            ramp2 = true;
        }
        if (!ramp3 && pos >= frames * 3 / 4)
        {
            rt.setRateRamp(2.0, 333);
            ref.setRamp(2.0, 333);
            ramp3 = true;
        }
    }
    check(ramp2 && ramp3, "coordinate ramps scheduled");
    check(rt.getRateRampRemaining() == 0, "ramps completed");
    checkClose(rt.getCurrentRate(), 2.0, 1e-12, "final rate");
    check(libOut.size() > 20000 * 2, "real output produced");
    double err = compareOut(libOut, refOut, 1e-6, "coordinate audio");
    std::printf("coordinate frames=%zu max_error=%.9g\n", libOut.size() / 2, err);
    std::puts("RATE_RAMP_CASE coordinate PASS");
}

// Case 2: retarget -----------------------------------------------------------
// Ramps retargeted mid-flight continue from the current increment; zero
// length ramps and invalid calls do not disturb the following audio.
static void caseRetarget()
{
    const unsigned frames = 30000;
    std::vector<float> input = makeNoise(2, frames, 11);

    RateTransposer rt;
    makeRampLib(rt, 2, false, true);
    Ref ref(2, false, true);

    rt.setRateRamp(2.0, 4000);
    ref.setRamp(2.0, 4000);

    std::vector<float> libOut, refOut;
    unsigned pos = 0;
    while (refOut.size() / 2 < 1500)   // walk part of the ramp
    {
        unsigned n = std::min(113u, frames - pos);
        feedAndSync(rt, ref, input.data() + (size_t)pos * 2, n, libOut, refOut, 0);
        pos += n;
    }
    double curBefore = rt.getCurrentRate();
    check(curBefore > 1.1 && curBefore < 1.6, "ramp advanced before retarget");

    rt.setRateRamp(0.6, 2500);          // reverse direction mid-ramp
    ref.setRamp(0.6, 2500);
    checkClose(rt.getCurrentRate(), curBefore, 0.0, "retarget continues from current increment");
    check(rt.getRateRampRemaining() == 2500, "retarget replaced ramp");

    while (pos < 9000)
    {
        unsigned n = std::min(113u, frames - pos);
        feedAndSync(rt, ref, input.data() + (size_t)pos * 2, n, libOut, refOut, 0);
        pos += n;
    }

    rt.setRateRamp(1.8, 0);             // zero length: immediate increment
    ref.setRamp(1.8, 0);
    check(rt.getRateRampRemaining() == 0, "zero length ramp leaves nothing");
    checkClose(rt.getCurrentRate(), 1.8, 1e-12, "zero length ramp sets increment");

    // invalid calls must throw and leave the trajectory untouched
    double nan = std::numeric_limits<double>::quiet_NaN();
    double inf = std::numeric_limits<double>::infinity();
    EXPECT_INVALID(rt.setRateRamp(0.3, 100));
    EXPECT_INVALID(rt.setRateRamp(2.5, 100));
    EXPECT_INVALID(rt.setRateRamp(0.49, 100));
    EXPECT_INVALID(rt.setRateRamp(2.01, 100));
    EXPECT_INVALID(rt.setRateRamp(nan, 100));
    EXPECT_INVALID(rt.setRateRamp(inf, 100));
    EXPECT_INVALID(rt.setRate(-0.5f));
    EXPECT_INVALID(rt.setRate(0.1f));
    EXPECT_INVALID(rt.setRate(2.5f));
    EXPECT_INVALID(rt.setRate((float)nan));
    check(rt.getRateRampRemaining() == ref.remaining(), "invalid calls kept ramp");
    checkClose(rt.getCurrentRate(), ref.current(), 0.0, "invalid calls kept rate");

    // mode switch mid-stream must throw; same-mode call is a no-op
    EXPECT_INVALID(rt.enableRateRamp(FALSE));
    rt.enableRateRamp(TRUE);            // already enabled: not a switch
    check(rt.getRateRampRemaining() == ref.remaining(), "no-op enable kept ramp");

    // valid immediate setRate cancels the ramp
    rt.setRateRamp(0.9, 500);
    ref.setRamp(0.9, 500);
    rt.setRate(1.25f);
    ref.setRate(1.25);
    check(rt.getRateRampRemaining() == 0, "setRate cancels ramp");
    checkClose(rt.getCurrentRate(), 1.25, 1e-12, "setRate sets increment");

    // unsupported interpolation algorithm must throw, object preserved
    TransposerBase::setAlgorithm(TransposerBase::SHANNON);
    {
        RateTransposer rtS;
        rtS.setChannels(2);
        EXPECT_INVALID(rtS.enableRateRamp(TRUE));
        check(!rtS.isRateRampEnabled(), "shannon object preserved");
    }
    TransposerBase::setAlgorithm(TransposerBase::CUBIC);

    // legacy rate outside [0.5, 2.0] blocks enabling but keeps legacy usable
    {
        RateTransposer rtL;
        rtL.setChannels(2);
        rtL.setRate(3.0f);              // legal in legacy mode
        EXPECT_INVALID(rtL.enableRateRamp(TRUE));
        std::vector<float> mono = makeNoise(2, 5000, 3), out;
        rtL.putSamples(mono.data(), 5000);
        drainInto(rtL, out, 2, 0);
        check(out.size() > 1000, "legacy processing continues after rejected enable");
    }

    // feed the rest and compare the complete stream
    while (pos < frames)
    {
        unsigned n = std::min(113u, frames - pos);
        feedAndSync(rt, ref, input.data() + (size_t)pos * 2, n, libOut, refOut, 0);
        pos += n;
    }
    double err = compareOut(libOut, refOut, 1e-6, "retarget audio");
    std::printf("retarget frames=%zu max_error=%.9g\n", libOut.size() / 2, err);

    // clear(): drops sound and fractional phase, cancels the ramp, keeps the
    // current increment and the mode; the next segment restarts at source
    // coordinate zero and the mode may be toggled again.
    {
        RateTransposer rtC;
        makeRampLib(rtC, 2, false, true);
        rtC.setRateRamp(1.5, 10000);
        std::vector<float> noise = makeNoise(2, 4000, 17), tmp;
        rtC.putSamples(noise.data(), 4000);
        rtC.putSamples(noise.data(), 2000);
        double curAtClear = rtC.getCurrentRate();
        check(rtC.getRateRampRemaining() > 0, "ramp still active before clear");
        rtC.clear();
        check(rtC.numSamples() == 0, "clear empties output");
        check(rtC.getRateRampRemaining() == 0, "clear cancels ramp");
        checkClose(rtC.getCurrentRate(), curAtClear, 0.0, "clear keeps increment");
        check(rtC.isRateRampEnabled(), "clear keeps mode");
        rtC.setRateRamp(1.75, 0);       // float-exact rate survives a toggle
        rtC.enableRateRamp(FALSE);      // allowed again right after clear
        rtC.enableRateRamp(TRUE);
        checkClose(rtC.getCurrentRate(), 1.75, 0.0, "toggle keeps rate");
        Ref refC(2, false, true);
        refC.setRate(1.75);
        std::vector<float> outC, refOutC;
        feedAndSync(rtC, refC, noise.data(), 4000, outC, refOutC, 0);
        compareOut(outC, refOutC, 1e-6, "clear restarts at coordinate zero");
    }
    std::puts("RATE_RAMP_CASE retarget PASS");
}

// Case 3: starvation ---------------------------------------------------------
// Insufficient lookahead, consecutive empty calls and different receive
// schedules must never advance the trajectory or change the audio.
static void caseStarvation()
{
    const unsigned frames = 12000;
    std::vector<float> input = makeNoise(2, frames, 13);

    RateTransposer rtA, rtB;
    makeRampLib(rtA, 2, true, true);
    makeRampLib(rtB, 2, true, true);
    rtA.setRateRamp(1.5, 8000);
    rtB.setRateRamp(1.5, 8000);
    Ref ref(2, true, true);
    ref.setRamp(1.5, 8000);

    std::vector<float> outA, outB, refOut;
    unsigned pos = 0, step = 0;
    while (pos < frames)
    {
        unsigned n = std::min(3u, frames - pos);
        unsigned remBefore = rtA.getRateRampRemaining();
        double curBefore = rtA.getCurrentRate();

        rtA.putSamples(input.data() + (size_t)pos * 2, n);
        rtB.putSamples(input.data() + (size_t)pos * 2, n);
        ref.feed(input.data() + (size_t)pos * 2, n);
        ref.generate(refOut);

        if (step % 7 == 3)
        {
            // consecutive empty calls and queries must not advance anything
            unsigned remMid = rtA.getRateRampRemaining();
            double curMid = rtA.getCurrentRate();
            rtA.putSamples(input.data(), 0);
            rtA.putSamples(input.data(), 0);
            (void)rtA.numSamples();
            (void)rtA.isEmpty();
            check(rtA.getRateRampRemaining() == remMid, "empty call advanced ramp");
            checkClose(rtA.getCurrentRate(), curMid, 0.0, "empty call moved rate");
        }

        unsigned produced = rtA.numSamples();
        if (produced == 0)
        {
            check(rtA.getRateRampRemaining() == remBefore, "starved put advanced ramp");
            checkClose(rtA.getCurrentRate(), curBefore, 0.0, "starved put moved rate");
        }
        // schedule A: read one frame at a time
        drainInto(rtA, outA, 2, 1);
        check(rtA.getRateRampRemaining() == ref.remaining(), "starvation remaining");
        checkClose(rtA.getCurrentRate(), ref.current(), 1e-12, "starvation rate");
        pos += n; step++;
    }
    // schedule B: everything read only at the end
    drainInto(rtB, outB, 2, 0);

    compareOut(outA, outB, 0.0, "receive schedules differ");
    double err = compareOut(outA, refOut, 1e-6, "starvation audio");
    check(outA.size() > 8000 * 2, "starvation produced output");
    std::printf("starvation frames=%zu max_error=%.9g\n", outA.size() / 2, err);
    std::puts("RATE_RAMP_CASE starvation PASS");
}

// Case 4: filtered_crossing --------------------------------------------------
// Long ramps crossing rate 1 with the anti-alias filter active. Identical
// ramps fed as one block, alternating 1/7/113 chunks and 4096-frame chunks
// must give identical output, matching the independent reference (whose
// filter is centered on the source coordinate) within 2e-5.
static std::vector<float> makeProbe(unsigned frames)
{
    Rng rng(23);
    std::vector<float> probe(frames);
    for (unsigned i = 0; i < frames; i++)
    {
        double v = 0.25 * std::sin(2.0 * REF_PI * 0.013 * i)
                 + 0.10 * std::sin(2.0 * REF_PI * 0.113 * i + 1.0)
                 + 0.07 * std::sin(2.0 * REF_PI * 0.271 * i + 2.0)
                 + 0.05 * rng.uniform();
        probe[i] = (float)v;
    }
    // sparse impulses to expose any filter center jumps
    probe[1000]  += 0.7f;
    probe[17377] -= 0.6f;
    probe[39996] += 0.8f;
    probe[70001] -= 0.7f;
    // inverted stereo
    std::vector<float> stereo((size_t)frames * 2);
    for (unsigned i = 0; i < frames; i++)
    {
        stereo[2 * i]     = probe[i];
        stereo[2 * i + 1] = -0.5f * probe[i];
    }
    return stereo;
}

static std::vector<float> runCrossing(const std::vector<float> &input, unsigned inFrames,
                                      double startRate, double targetRate, unsigned rampN,
                                      const std::vector<unsigned> &chunks,
                                      std::vector<float> &refOut)
{
    RateTransposer rt;
    makeRampLib(rt, 2, true, true);
    rt.setRateRamp(startRate, 0);   // immediate, double precision
    rt.setRateRamp(targetRate, rampN);
    Ref ref(2, true, true);
    ref.setRate(startRate);
    ref.setRamp(targetRate, rampN);

    std::vector<float> libOut;
    unsigned pos = 0, ci = 0;
    while (pos < inFrames)
    {
        unsigned n = std::min(chunks[ci % chunks.size()], inFrames - pos);
        feedAndSync(rt, ref, input.data() + (size_t)pos * 2, n, libOut, refOut, 0);
        pos += n; ci++;
    }
    return libOut;
}

static void checkInverted(const std::vector<float> &out)
{
    for (size_t i = 0; i + 1 < out.size(); i += 2)
    {
        check(std::isfinite(out[i]) && std::isfinite(out[i + 1]), "non-finite output");
        check(out[i + 1] == -0.5f * out[i], "inverted channel relation broken");
    }
}

static void caseFilteredCrossing()
{
    const unsigned frames = 96000;
    std::vector<float> input = makeProbe(frames);

    std::vector<unsigned> whole(1, frames);
    std::vector<unsigned> alt;  alt.push_back(1); alt.push_back(7); alt.push_back(113);
    std::vector<unsigned> big(1, 4096);

    const struct { double r0, rt; unsigned n; const char *name; } runs[2] = {
        {0.75, 1.5, 40000, "up"},
        {1.5, 0.6, 40000, "down"},
    };

    for (int r = 0; r < 2; r++)
    {
        std::vector<float> refA, refB, refC;
        std::vector<float> a = runCrossing(input, frames, runs[r].r0, runs[r].rt, runs[r].n, whole, refA);
        std::vector<float> b = runCrossing(input, frames, runs[r].r0, runs[r].rt, runs[r].n, alt, refB);
        std::vector<float> c = runCrossing(input, frames, runs[r].r0, runs[r].rt, runs[r].n, big, refC);

        compareOut(a, b, 2e-5, "crossing whole vs 1/7/113");
        compareOut(a, c, 2e-5, "crossing whole vs 4096");
        double err = compareOut(a, refA, 2e-5, "crossing vs reference");
        checkInverted(a);
        check(a.size() > 60000 * 2, "crossing produced output");
        std::printf("filtered_crossing %s frames=%zu max_error=%.9g\n",
                    runs[r].name, a.size() / 2, err);
    }

    // Retarget mid-run so the trajectory crosses rate 1 twice. The reference
    // is updated at the actually generated frame position, not at an input
    // block boundary.
    {
        RateTransposer rt;
        makeRampLib(rt, 2, true, true);
        rt.setRate(0.75f);
        rt.setRateRamp(1.5, 30000);
        Ref ref(2, true, true);
        ref.setRate(0.75);
        ref.setRamp(1.5, 30000);

        std::vector<float> libOut, refOut;
        unsigned pos = 0;
        while (refOut.size() / 2 < 20000)
        {
            unsigned n = std::min(113u, frames - pos);
            feedAndSync(rt, ref, input.data() + (size_t)pos * 2, n, libOut, refOut, 0);
            pos += n;
        }
        unsigned long long generatedBefore = refOut.size() / 2;
        unsigned remBefore = rt.getRateRampRemaining();
        check(remBefore == 30000 - (unsigned)generatedBefore, "retarget position bookkeeping");
        double curBefore = rt.getCurrentRate();
        check(curBefore > 1.0, "first crossing happened before retarget");

        rt.setRateRamp(0.6, 25000);   // second crossing, back below 1
        ref.setRamp(0.6, 25000);
        checkClose(rt.getCurrentRate(), curBefore, 0.0, "second ramp starts from current increment");
        std::printf("filtered_crossing retarget_at_frame=%llu remaining_was=%u\n",
                    generatedBefore, remBefore);

        while (pos < frames)
        {
            unsigned n = std::min(113u, frames - pos);
            feedAndSync(rt, ref, input.data() + (size_t)pos * 2, n, libOut, refOut, 0);
            pos += n;
        }
        double err = compareOut(libOut, refOut, 2e-5, "double crossing vs reference");
        checkInverted(libOut);
        // phase continuity: no sample-to-sample jump anywhere near the crossings
        double maxStep = 0.0;
        for (size_t i = 2; i + 1 < libOut.size(); i += 2)
            maxStep = std::max(maxStep, (double)std::fabs(libOut[i] - libOut[i - 2]));
        check(maxStep < 1.0, "phase continuity across crossings");
        std::printf("filtered_crossing two_crossings frames=%zu max_error=%.9g max_step=%.6f\n",
                    libOut.size() / 2, err, maxStep);
    }
    std::puts("RATE_RAMP_CASE filtered_crossing PASS");
}

// Case 5: spectral -----------------------------------------------------------
// Frequency response on stable rate plateaus, statistics taken away from the
// plateau edges.
static std::vector<float> renderSine(double rate, double freq, bool aa, unsigned nIn, bool viaRampCall)
{
    RateTransposer rt;
    makeRampLib(rt, 1, aa, true);
    if (viaRampCall) rt.setRateRamp(rate, 0); else rt.setRate((float)rate);
    std::vector<float> in(nIn), out;
    for (unsigned i = 0; i < nIn; i++)
        in[i] = 0.4f * (float)std::sin(2.0 * REF_PI * freq * i);
    rt.putSamples(in.data(), nIn);
    drainInto(rt, out, 1, 0);
    return out;
}

static double rmsWindow(const std::vector<float> &v, size_t skip)
{
    check(v.size() > 2 * skip + 4096, "plateau keeps at least 4096 frames");
    double s = 0.0;
    size_t n = 0;
    for (size_t i = skip; i + skip < v.size(); i++) { s += (double)v[i] * v[i]; n++; }
    return std::sqrt(s / n);
}

static void caseSpectral()
{
    const unsigned nIn = 16384;
    const size_t skip = 512;    // plateau ends excluded from statistics
    const double inRms = 0.4 / std::sqrt(2.0);

    // rate 2, 0.4 cycles/input-frame: AA-on must be >= 35 dB below AA-off
    std::vector<float> on2  = renderSine(2.0, 0.4, true, nIn, false);
    std::vector<float> off2 = renderSine(2.0, 0.4, false, nIn, false);
    double supp = 20.0 * std::log10(rmsWindow(on2, skip) / rmsWindow(off2, skip));
    check(supp <= -35.0, "rate 2 anti-alias suppression");
    std::printf("spectral rate2 suppression_db=%.3f\n", supp);

    // rate 0.5, 0.30 cycles/input-frame must be preserved within +/-3 dB
    std::vector<float> p05 = renderSine(0.5, 0.30, true, nIn, true);
    double keep = 20.0 * std::log10(rmsWindow(p05, skip) / inRms);
    check(std::fabs(keep) <= 3.0, "rate 0.5 passband at 0.30");
    std::printf("spectral rate0.5 f0.30 level_db=%.3f\n", keep);

    // 0.05 cycles/input-frame within +/-1 dB on both plateaus
    std::vector<float> lo05 = renderSine(0.5, 0.05, true, nIn, false);
    double lo05db = 20.0 * std::log10(rmsWindow(lo05, skip) / inRms);
    check(std::fabs(lo05db) <= 1.0, "rate 0.5 passband at 0.05");
    std::vector<float> lo2 = renderSine(2.0, 0.05, true, nIn, true);
    double lo2db = 20.0 * std::log10(rmsWindow(lo2, skip) / inRms);
    check(std::fabs(lo2db) <= 1.0, "rate 2 passband at 0.05");
    std::printf("spectral f0.05 rate0.5_db=%.3f rate2_db=%.3f\n", lo05db, lo2db);
    std::puts("RATE_RAMP_CASE spectral PASS");
}

// Case 6: channels -----------------------------------------------------------
// Mono, inverted stereo and 6-channel interleaved PCM with impulses at
// different per-channel positions; one shared source coordinate.
static void runChannelCase(int nch, const std::vector<float> &input, unsigned frames,
                           double r0, double rt, unsigned rampN, std::vector<float> &libOut)
{
    RateTransposer rtObj;
    makeRampLib(rtObj, nch, true, true);
    rtObj.setRateRamp(r0, 0);   // immediate, double precision
    rtObj.setRateRamp(rt, rampN);
    Ref ref(nch, true, true);
    ref.setRate(r0);
    ref.setRamp(rt, rampN);

    std::vector<float> refOut;
    unsigned pos = 0;
    const unsigned chunks[] = {5, 211, 64};
    unsigned ci = 0;
    while (pos < frames)
    {
        unsigned n = std::min(chunks[ci % 3], frames - pos);
        feedAndSync(rtObj, ref, input.data() + (size_t)pos * nch, n, libOut, refOut, 0);
        pos += n; ci++;
    }
    compareOut(libOut, refOut, 1e-6, "channel case vs reference");
}

static void caseChannels()
{
    // mono: impulses over noise
    {
        const unsigned frames = 20000;
        std::vector<float> in = makeNoise(1, frames, 29);
        in[300] += 0.8f; in[9999] -= 0.8f; in[15000] += 0.7f;
        std::vector<float> out;
        runChannelCase(1, in, frames, 0.9, 1.4, 6000, out);
        check(out.size() > 15000, "mono output");
        std::printf("channels mono frames=%zu\n", out.size());
    }
    // inverted stereo
    {
        const unsigned frames = 20000;
        std::vector<float> mono = makeNoise(1, frames, 31), in(frames * 2);
        for (unsigned i = 0; i < frames; i++)
        {
            in[2 * i] = mono[i];
            in[2 * i + 1] = -0.5f * mono[i];
        }
        std::vector<float> out;
        runChannelCase(2, in, frames, 1.2, 0.7, 6000, out);
        checkInverted(out);
        std::printf("channels stereo frames=%zu\n", out.size() / 2);
    }
    // 6 channels: impulse at a different position per channel
    {
        const unsigned frames = 30000;
        std::vector<float> in((size_t)frames * 6, 0.0f);
        for (unsigned i = 0; i < frames; i++)
            for (int c = 0; c < 6; c++)
                in[(size_t)i * 6 + c] = (float)(0.05 * (c + 1) * std::sin(2.0 * REF_PI * 0.02 * i));
        for (int c = 0; c < 6; c++)
            in[(size_t)(500 + 137 * c) * 6 + c] += 0.5f;
        std::vector<float> out;
        runChannelCase(6, in, frames, 0.8, 1.6, 9000, out);
        check(out.size() > 20000 * 6, "6ch output");
        // shared coordinate: the per-channel impulses land at matching output
        // positions relative to each channel's own impulse delay
        std::printf("channels 6ch frames=%zu\n", out.size() / 6);
    }
    std::puts("RATE_RAMP_CASE channels PASS");
}

// Case 7: legacy -------------------------------------------------------------
// The pre-existing constant-rate behaviour must be unchanged while the new
// mode is off. (The SoundStretch file-processing part of the legacy baseline
// is executed by checks/rate_ramp.sh via checks/run_baseline.sh.)
static std::vector<float> renderLegacy(unsigned block, bool aa, float rate, double freq, unsigned n)
{
    TransposerBase::setAlgorithm(TransposerBase::LINEAR);
    RateTransposer r;
    r.setChannels(2);
    r.enableAAFilter(aa ? TRUE : FALSE);
    r.setRate(rate);
    std::vector<float> input((size_t)n * 2), out;
    for (unsigned i = 0; i < n; i++)
    {
        float y = 0.4f * (float)std::sin(2.0 * REF_PI * freq * i);
        input[2 * i] = y;
        input[2 * i + 1] = -0.5f * y;
    }
    std::vector<float> buf;
    for (unsigned p = 0; p < n;)
    {
        unsigned k = std::min(block, n - p);
        r.putSamples(input.data() + (size_t)p * 2, k);
        unsigned avail;
        while ((avail = r.numSamples()) > 0)
        {
            buf.resize((size_t)avail * 2);
            unsigned got = r.receiveSamples(buf.data(), avail);
            out.insert(out.end(), buf.begin(), buf.begin() + (size_t)got * 2);
        }
        p += k;
    }
    return out;
}

static double rmsMid(const std::vector<float> &v)
{
    double s = 0.0;
    size_t n = 0;
    for (size_t i = 2048; i + 2048 < v.size(); i += 2) { s += (double)v[i] * v[i]; n++; }
    return std::sqrt(s / n);
}

static void caseLegacy()
{
    const unsigned n = 48000;
    std::vector<float> a = renderLegacy(4096, true, 0.75f, 440.0 / 48000, n);
    std::vector<float> b = renderLegacy(37, true, 0.75f, 440.0 / 48000, n);
    double err = compareOut(a, b, 1e-6, "legacy constant-rate chunk equivalence");
    std::printf("legacy constant frames=%zu max_error=%.9g\n", a.size() / 2, err);

    std::vector<float> high = renderLegacy(4096, true, 1.5f, 0.4, n);
    std::vector<float> raw = renderLegacy(4096, false, 1.5f, 0.4, n);
    double supp = 20.0 * std::log10(rmsMid(high) / rmsMid(raw));
    check(supp < -35.0, "legacy AA suppression");
    std::printf("legacy aa suppression_db=%.3f\n", supp);

    // toggling the new mode on and off before any input keeps legacy behaviour
    {
        TransposerBase::setAlgorithm(TransposerBase::LINEAR);
        RateTransposer r;
        r.setChannels(2);
        r.enableRateRamp(TRUE);
        r.enableRateRamp(FALSE);
        check(!r.isRateRampEnabled(), "mode toggled back off");
        r.setRate(0.75f);
        r.setRate(2.5f);    // legacy range is not limited to [0.5, 2.0]
        r.setRate(0.75f);
        std::vector<float> input = makeNoise(2, 20000, 41), outA, outB;
        r.putSamples(input.data(), 20000);
        drainInto(r, outA, 2, 0);
        RateTransposer r2;
        r2.setChannels(2);
        r2.setRate(0.75f);
        r2.putSamples(input.data(), 20000);
        drainInto(r2, outB, 2, 0);
        compareOut(outA, outB, 0.0, "toggle-then-legacy equivalence");
    }
    TransposerBase::setAlgorithm(TransposerBase::CUBIC);
    std::puts("RATE_RAMP_CASE legacy PASS");
}

int main()
{
    caseCoordinate();
    caseRetarget();
    caseStarvation();
    caseFilteredCrossing();
    caseSpectral();
    caseChannels();
    caseLegacy();
    std::puts("RATE_RAMP_OK cases=7");
    return 0;
}
