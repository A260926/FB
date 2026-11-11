/*
    live_contract.cpp

    Regression contract for the Rubber Band Live pitch shifter,
    exercised through BOTH public interfaces:

      * the C++ class RubberBand::RubberBandLiveShifter::shift()
      * the C wrapper rubberband_live_shift() (a separate, independently
        constructed instance - never the same instance reused through
        two interfaces, and never getter-only checks)

    The test deliberately contains no audio device and no listening
    judgement: every conclusion is a numeric assertion on signals the
    test itself synthesises and analyses.  Frequency estimation uses a
    direct DFT correlation scan implemented below.  It does not call any
    Rubber Band internal FFT, and no second Rubber Band output is used
    as the reference signal.

    Configuration
    -------------
      * sample rate 48000 Hz, 2 channels
      * block size is whatever the library reports from getBlockSize()
        (512 samples in the current implementation); every shift() call
        passes and receives exactly one block
      * OptionChannelsTogether is always set; the primary window is
        OptionWindowShort, with OptionWindowMedium also covered
      * pitch scales 0.5, 1.0 and 2.0; a fresh instance is built for
        every combination and the scale is set exactly once

    Signals (all amplitude 0.3)
    ---------------------------
      A in-phase dual mono      L = R = 440 Hz sine
      B strict anti-phase       L = 440 Hz sine, R = -L
      C distinct pitches        L = 330 Hz, R = 550 Hz
      D gapped in-phase         A with a central silence gap
      E gapped anti-phase       B with a central silence gap
      F finite burst (unity)    bounded 440 Hz tone burst + zero tail

    Assertion tolerances and steady-state regions (all measured against
    the unmodified library, see the comments at each constant)
    -------------------------------------------------------------------
      ANALYSIS_LEN = 12000 frames (0.25 s at 48 kHz).  The lowest
        analysed frequency is 330 * 0.5 = 165 Hz, so every analysis
        window contains at least 165 * 0.25 ~= 41 complete source
        cycles; ordinary 440 Hz windows contain 55 cycles (110 after a
        2x shift).  No conclusion is drawn from the start-up transient
        region.

      Normal streams are 240 blocks (122880 frames, 2.56 s); the single
        analysis window starts at n/2 = frame 61440, far beyond the
        largest reported start delay (4672) plus window settling.

      Gapped streams are 300 blocks (153600 frames).  Silence occupies
        blocks 100..140 (frames 51200..71680).  Steady evidence is taken
        from two windows: frames 20480..32480 (well before the gap) and
        frames 81920..93920 (well after the gap, itself past
        gap_end + max_delay = 74304).  The silence region is never used
        as evidence.  Each steady segment is >= 1.0 s long.

      FREQ_TOL_HZ = 3 Hz: detected dominant frequency must lie within
        3 Hz of source_frequency * scale.  The DFT search band is
        [0.75, 1.30] * expected; an ignored scale (output left at the
        source frequency) lands well outside this band for 0.5/2.

      ENERGY_MIN = 50.0 per 12000-frame window (observed ~554): every
        output window must carry non-zero, fully finite energy.

      REL_RESIDUAL = 1e-6: normalised L/R residual.  In-phase and
        anti-phase relations are reconstructed exactly by the
        mid/side round trip, so the observed residual is 0.0.

      MONO_RESIDUAL_MIN = 0.5: for the distinct-pitch signal the
        normalised L-minus-R residual must stay high (observed 1.00);
        a collapse to mono drives it below the threshold, and an
        explicit channel swap is reported when each channel carries the
        other channel's expected frequency.

      UNITY_MAXERR = 0.01: unity-ratio sample-wise error after aligning
        by the reported start delay and skipping the first 4096 input
        frames.  Measured maximum on the unmodified library is
        3.88e-3 for BOTH window sizes, so no separate medium-window
        tolerance is needed.

      BURST_CORR_MIN = 0.999 (measured 0.99999982): a finite tone burst
        with a zero tail is cross-correlated against the output so the
        true delay is found from an aperiodic feature.  An integer
        number of periods of misalignment on the infinite sine could
        otherwise look correct; the burst cannot be fooled that way.
        The measured lag must equal the reported delay exactly.

    Buffer layout
    -------------
    Each channel input is held in its own guarded plane with a separate
    backup plane; each channel output lives in a separate, differently
    padded (hence non-adjacent) plane with sentinel values before and
    after the data region.  Sentinels are checked after every block and
    the input planes are compared with their backups after each run.

    Failure categories (printed as DETECTED markers)
    ------------------------------------------------
      LIVE_PITCH_DETECTED     wrong dominant frequency, dead/non-finite
                              output, or C/C++ sample divergence
      LIVE_RELATION_DETECTED  in-phase/anti-phase relation broken,
                              channel swap or mono collapse
      LIVE_LATENCY_DETECTED   start delay misreported or unity
                              sample alignment wrong
*/

#include "rubberband/RubberBandLiveShifter.h"
#include "rubberband/rubberband-c.h"

#include <vector>
#include <string>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>

using RubberBand::RubberBandLiveShifter;

namespace {

const double PI = 3.14159265358979323846;
const int SR = 48000;
const double AMP = 0.3;
const float SENTINEL = 777.0f;

const size_t ANALYSIS_LEN = 12000;            // >= 41 cycles at 165 Hz
const double FREQ_TOL_HZ = 3.0;
const double ENERGY_MIN = 50.0;               // observed ~554
const double REL_RESIDUAL = 1e-6;             // observed 0.0
const double MONO_RESIDUAL_MIN = 0.5;         // observed 1.00
const double UNITY_MAXERR = 0.01;             // observed 3.88e-3, both windows
const double BURST_CORR_MIN = 0.999;          // observed 0.99999982
const size_t UNITY_SKIP = 4096;

enum Cat { CAT_PITCH = 0, CAT_RELATION, CAT_LATENCY, CAT_COUNT };
const char *const CAT_NAME[CAT_COUNT] = { "PITCH", "RELATION", "LATENCY" };
bool g_failed[CAT_COUNT] = { false, false, false };

void fail(Cat cat, const char *fmt, ...)
{
    if (!g_failed[cat]) {
        g_failed[cat] = true;
        std::fprintf(stderr, "LIVE_%s_DETECTED\n", CAT_NAME[cat]);
    }
    std::fprintf(stderr, "    [%s] ", CAT_NAME[cat]);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
}

// A separately allocated guarded plane.  Left and right planes use
// different guard widths so the two channel planes can never be
// adjacent even by allocator coincidence.
struct Plane {
    size_t guard;
    size_t n;
    std::vector<float> mem;
    float *p;

    Plane(size_t n_, size_t g_, float dataFill) :
        guard(g_), n(n_), mem(n_ + 2 * g_, SENTINEL),
        p(mem.data() + g_)
    {
        for (size_t i = 0; i < n; ++i) p[i] = dataFill;
    }

    bool guardsIntact() const
    {
        for (size_t i = 0; i < guard; ++i) {
            if (mem[i] != SENTINEL || mem[guard + n + i] != SENTINEL)
                return false;
        }
        return true;
    }
};

enum SignalKind {
    SIG_INPHASE,
    SIG_ANTIPHASE,
    SIG_DISTINCT,
    SIG_GAP_INPHASE,
    SIG_GAP_ANTIPHASE,
    SIG_BURST
};

struct SignalSpec {
    const char *name;
    SignalKind kind;
    double fL, fR;
    size_t nblocks;
    size_t gapBlock0, gapBlock1;   // silence [block0, block1); 0 if none
};

struct Window { size_t a, b; };

struct Stream {
    // output planes (guarded); inputs are owned by the runner caller
    Plane outL;
    Plane outR;
    size_t blockSize;
    size_t startDelay;
    size_t channels;
    double pitchScale;
    Stream(size_t n, size_t gL, size_t gR) :
        outL(n, gL, 0.0f), outR(n, gR, 0.0f),
        blockSize(0), startDelay(0), channels(0), pitchScale(0.0) { }
};

void buildSignal(const SignalSpec &spec, size_t bs,
                 std::vector<float> &l, std::vector<float> &r)
{
    size_t n = spec.nblocks * bs;
    l.assign(n, 0.0f);
    r.assign(n, 0.0f);
    size_t gap0 = spec.gapBlock0 * bs;
    size_t gap1 = spec.gapBlock1 * bs;
    for (size_t i = 0; i < n; ++i) {
        if (spec.kind == SIG_BURST) {
            // Finite tone burst inside [gap0,gap1), zero elsewhere;
            // the zero tail makes the delay measurable by correlation.
            if (i >= gap0 && i < gap1) {
                l[i] = float(AMP * std::sin(2.0 * PI * spec.fL * i / SR));
                r[i] = l[i];
            } else {
                l[i] = 0.0f; r[i] = 0.0f;
            }
            continue;
        }
        bool inGap = (spec.gapBlock1 > 0 && i >= gap0 && i < gap1);
        if (inGap) { l[i] = 0.0f; r[i] = 0.0f; continue; }
        float sl = float(AMP * std::sin(2.0 * PI * spec.fL * i / SR));
        float sr = float(AMP * std::sin(2.0 * PI * spec.fR * i / SR));
        switch (spec.kind) {
        case SIG_ANTIPHASE:
        case SIG_GAP_ANTIPHASE:
            l[i] = sl; r[i] = -sl; break;
        default:
            l[i] = sl; r[i] = sr; break;
        }
    }
}

std::vector<Window> analysisWindows(const SignalSpec &spec, size_t bs)
{
    size_t n = spec.nblocks * bs;
    std::vector<Window> ws;
    if (spec.kind == SIG_GAP_INPHASE || spec.kind == SIG_GAP_ANTIPHASE) {
        // blocks 40.. and 160.., each 12000 frames and clear of the
        // gap (blocks 100..140) even after delay alignment
        ws.push_back({40 * bs, 40 * bs + ANALYSIS_LEN});
        ws.push_back({160 * bs, 160 * bs + ANALYSIS_LEN});
    } else if (spec.kind == SIG_BURST) {
        // no steady-state spectral evidence; latency path only
        return ws;
    } else {
        ws.push_back({n / 2, n / 2 + ANALYSIS_LEN});
    }
    return ws;
}

// Test-owned frequency estimator: direct DFT correlation scan, 1 Hz
// coarse grid followed by a 0.1 Hz local refinement.  No library FFT.
double estimatePeakHz(const float *x, size_t a, size_t b,
                      double fExpected, double &magnitudeOut)
{
    double lo = 0.75 * fExpected;
    double hi = 1.30 * fExpected;
    if (lo < 20.0) lo = 20.0;
    if (hi > SR / 2.0 - 100.0) hi = SR / 2.0 - 100.0;

    double bestM = -1.0;
    double bestF = lo;
    auto scan = [&](double f) {
        double w = 2.0 * PI * f / SR;
        double re = 0.0, im = 0.0;
        for (size_t i = a; i < b; ++i) {
            double ang = w * double(i - a);
            re += double(x[i]) * std::cos(ang);
            im -= double(x[i]) * std::sin(ang);
        }
        double m = re * re + im * im;
        if (m > bestM) { bestM = m; bestF = f; }
    };
    for (int f = int(std::floor(lo)); f <= int(std::ceil(hi)); ++f)
        scan(double(f));
    double centre = bestF;
    bestM = -1.0;
    for (int k = 0; k <= 30; ++k)
        scan(centre - 1.5 + k * 0.1);
    magnitudeOut = std::sqrt(bestM);
    return bestF;
}

bool windowFiniteAndLoud(const Plane &plane, size_t a, size_t b)
{
    double e = 0.0;
    for (size_t i = a; i < b; ++i) {
        float v = plane.p[i];
        if (!std::isfinite(v)) return false;
        e += double(v) * double(v);
    }
    return e >= ENERGY_MIN;
}

double normalisedResidual(const Plane &x, const Plane &y, size_t a,
                          size_t b, bool opposite)
{
    double num = 0.0, den = 0.0;
    for (size_t i = a; i < b; ++i) {
        double d = opposite ? (double(x.p[i]) + double(y.p[i]))
                            : (double(x.p[i]) - double(y.p[i]));
        num += d * d;
        den += double(x.p[i]) * double(x.p[i])
             + double(y.p[i]) * double(y.p[i]);
    }
    return std::sqrt(num / den);
}

void checkSentinel(const char *tag, const Plane &pl, Cat cat)
{
    if (!pl.guardsIntact())
        fail(cat, "%s: guard sentinel overwritten on plane", tag);
}

// Run one full block-by-block stream through the C++ API.
Stream runCpp(int opts, double scale, const std::vector<float> &inL,
              const std::vector<float> &inR, const char *&iface)
{
    iface = "C++";
    size_t n = inL.size();
    Plane workL(n, 64, 0.0f), workR(n, 96, 0.0f);
    Plane backL(n, 64, 0.0f), backR(n, 96, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        workL.p[i] = backL.p[i] = inL[i];
        workR.p[i] = backR.p[i] = inR[i];
    }
    Stream st(n, 64, 96);
    RubberBandLiveShifter sh(SR, 2, opts);
    sh.setPitchScale(scale);              // set once, never changed
    st.blockSize = sh.getBlockSize();
    st.startDelay = sh.getStartDelay();
    st.channels = sh.getChannelCount();
    st.pitchScale = sh.getPitchScale();
    size_t bs = st.blockSize;
    for (size_t blk = 0; blk < n / bs; ++blk) {
        const float *in[2] = { workL.p + blk * bs, workR.p + blk * bs };
        float *out[2] = { st.outL.p + blk * bs, st.outR.p + blk * bs };
        sh.shift(in, out);                // exactly one block per call
        checkSentinel("C++ output L", st.outL, CAT_PITCH);
        checkSentinel("C++ output R", st.outR, CAT_PITCH);
    }
    for (size_t i = 0; i < n; ++i) {
        if (workL.p[i] != backL.p[i] || workR.p[i] != backR.p[i]) {
            fail(CAT_PITCH, "C++: input buffer modified at sample %zu", i);
            break;
        }
    }
    checkSentinel("C++ input L", workL, CAT_PITCH);
    checkSentinel("C++ input R", workR, CAT_PITCH);
    return st;
}

// Run the identical stream through the C API on its own instance.
Stream runC(int opts, double scale, const std::vector<float> &inL,
            const std::vector<float> &inR, const char *&iface)
{
    iface = "C";
    size_t n = inL.size();
    Plane workL(n, 64, 0.0f), workR(n, 96, 0.0f);
    Plane backL(n, 64, 0.0f), backR(n, 96, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        workL.p[i] = backL.p[i] = inL[i];
        workR.p[i] = backR.p[i] = inR[i];
    }
    Stream st(n, 64, 96);
    RubberBandLiveState sh = rubberband_live_new(SR, 2, opts);
    rubberband_live_set_pitch_scale(sh, scale);
    st.blockSize = rubberband_live_get_block_size(sh);
    st.startDelay = rubberband_live_get_start_delay(sh);
    st.channels = rubberband_live_get_channel_count(sh);
    st.pitchScale = rubberband_live_get_pitch_scale(sh);
    size_t bs = st.blockSize;
    for (size_t blk = 0; blk < n / bs; ++blk) {
        const float *in[2] = { workL.p + blk * bs, workR.p + blk * bs };
        float *out[2] = { st.outL.p + blk * bs, st.outR.p + blk * bs };
        rubberband_live_shift(sh, in, out);
        checkSentinel("C output L", st.outL, CAT_PITCH);
        checkSentinel("C output R", st.outR, CAT_PITCH);
    }
    rubberband_live_delete(sh);
    for (size_t i = 0; i < n; ++i) {
        if (workL.p[i] != backL.p[i] || workR.p[i] != backR.p[i]) {
            fail(CAT_PITCH, "C: input buffer modified at sample %zu", i);
            break;
        }
    }
    checkSentinel("C input L", workL, CAT_PITCH);
    checkSentinel("C input R", workR, CAT_PITCH);
    return st;
}

void checkStream(const Stream &st, const SignalSpec &spec,
                 const std::vector<Window> &ws, const char *iface,
                 const char *winName, double scale)
{
    char tag[128];
    std::snprintf(tag, sizeof(tag), "%s/%s/scale=%.1f/%s",
                  winName, iface, scale, spec.name);

    if (st.channels != 2)
        fail(CAT_PITCH, "%s: channel count %zu != 2", tag, st.channels);
    if (st.blockSize == 0)
        fail(CAT_PITCH, "%s: reported zero block size", tag);

    // whole-stream finiteness
    size_t n = spec.nblocks * st.blockSize;
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(st.outL.p[i]) || !std::isfinite(st.outR.p[i])) {
            fail(CAT_PITCH, "%s: non-finite output at sample %zu", tag, i);
            break;
        }
    }

    if (spec.kind == SIG_BURST) return;

    for (const Window &w : ws) {
        // energy must be non-zero and finite on both channels
        if (!windowFiniteAndLoud(st.outL, w.a, w.b))
            fail(CAT_PITCH, "%s ch=L window[%zu..%zu]: dead/non-finite energy",
                 tag, w.a, w.b);
        if (!windowFiniteAndLoud(st.outR, w.a, w.b))
            fail(CAT_PITCH, "%s ch=R window[%zu..%zu]: dead/non-finite energy",
                 tag, w.a, w.b);

        // dominant frequency on each channel
        double expL = spec.fL * scale, expR = spec.fR * scale;
        double mag;
        double gotL = estimatePeakHz(st.outL.p, w.a, w.b, expL, mag);
        if (std::fabs(gotL - expL) > FREQ_TOL_HZ)
            fail(CAT_PITCH,
                 "%s ch=L: detected %.2f Hz expected %.2f Hz (tol %.0f Hz)",
                 tag, gotL, expL, FREQ_TOL_HZ);
        double gotR = estimatePeakHz(st.outR.p, w.a, w.b, expR, mag);
        if (std::fabs(gotR - expR) > FREQ_TOL_HZ)
            fail(CAT_PITCH,
                 "%s ch=R: detected %.2f Hz expected %.2f Hz (tol %.0f Hz)",
                 tag, gotR, expR, FREQ_TOL_HZ);

        // inter-channel relations; silence windows are excluded by design
        switch (spec.kind) {
        case SIG_INPHASE:
        case SIG_GAP_INPHASE: {
            double res = normalisedResidual(st.outL, st.outR, w.a, w.b, false);
            if (res > REL_RESIDUAL)
                fail(CAT_RELATION,
                     "%s: in-phase residual %.3e > %.0e (window %zu..%zu)",
                     tag, res, REL_RESIDUAL, w.a, w.b);
            break;
        }
        case SIG_ANTIPHASE:
        case SIG_GAP_ANTIPHASE: {
            double res = normalisedResidual(st.outL, st.outR, w.a, w.b, true);
            if (res > REL_RESIDUAL)
                fail(CAT_RELATION,
                     "%s: anti-phase residual %.3e > %.0e (window %zu..%zu)",
                     tag, res, REL_RESIDUAL, w.a, w.b);
            break;
        }
        case SIG_DISTINCT: {
            // An actual left/right swap: each channel carries the
            // other one's expected pitch.
            if (std::fabs(gotL - expR) <= FREQ_TOL_HZ &&
                std::fabs(gotR - expL) <= FREQ_TOL_HZ)
                fail(CAT_RELATION,
                     "%s: channels appear swapped (L=%.2f Hz R=%.2f Hz)",
                     tag, gotL, gotR);
            // must not have been folded down to one mono signal
            double res = normalisedResidual(st.outL, st.outR, w.a, w.b, false);
            if (res < MONO_RESIDUAL_MIN)
                fail(CAT_RELATION,
                     "%s: distinct-pitch residual %.3e < %.2f suggests mono "
                     "collapse (window %zu..%zu)",
                     tag, res, MONO_RESIDUAL_MIN, w.a, w.b);
            break;
        }
        default: break;
        }
    }
}

// The two interfaces wrap the same implementation on independent
// instances: reported geometry and every output sample must match.
void checkInterfaceParity(const Stream &cpp, const Stream &c,
                          const SignalSpec &spec, const char *winName,
                          double scale)
{
    char tag[128];
    std::snprintf(tag, sizeof(tag), "%s/scale=%.1f/%s",
                  winName, scale, spec.name);
    if (cpp.blockSize != c.blockSize)
        fail(CAT_PITCH, "%s: block size C++=%zu C=%zu",
             tag, cpp.blockSize, c.blockSize);
    if (cpp.channels != c.channels)
        fail(CAT_PITCH, "%s: channel count C++=%zu C=%zu",
             tag, cpp.channels, c.channels);
    if (cpp.startDelay != c.startDelay)
        fail(CAT_LATENCY, "%s: start delay C++=%zu C=%zu",
             tag, cpp.startDelay, c.startDelay);
    if (std::fabs(cpp.pitchScale - c.pitchScale) > 1e-12)
        fail(CAT_PITCH, "%s: pitch scale getter C++=%.6f C=%.6f",
             tag, cpp.pitchScale, c.pitchScale);
    size_t n = spec.nblocks * cpp.blockSize;
    for (size_t ch = 0; ch < 2; ++ch) {
        const Plane &a = (ch == 0) ? cpp.outL : cpp.outR;
        const Plane &b = (ch == 0) ? c.outL : c.outR;
        for (size_t i = 0; i < n; ++i) {
            if (a.p[i] != b.p[i]) {
                fail(CAT_PITCH,
                     "%s ch=%zu: C/C++ output first differs at sample %zu "
                     "(C++=%.8f C=%.8f)",
                     tag, ch, i, a.p[i], b.p[i]);
                break;
            }
        }
    }
}

// Unity-only latency contract, run per interface and window.
void checkLatency(const Stream &st, const std::vector<float> &inL,
                  const SignalSpec &burstSpec, const Stream &burst,
                  const std::vector<float> &burstInL,
                  const char *iface, const char *winName)
{
    char tag[128];
    std::snprintf(tag, sizeof(tag), "%s/%s/scale=1.0", winName, iface);
    size_t delay = st.startDelay;
    size_t n = inL.size();

    // (1) Infinite sine aligned by the reported delay.  A whole-period
    //     slip would pass this amplitude comparison, which is why the
    //     burst correlation below is also required.
    double maxErr = 0.0;
    size_t firstBad = SIZE_MAX;
    float firstOut = 0.0f, firstIn = 0.0f;
    for (size_t i = UNITY_SKIP; i + delay < n; ++i) {
        double d = std::fabs(double(st.outL.p[i + delay]) - double(inL[i]));
        if (d > maxErr) maxErr = d;
        if (d > UNITY_MAXERR && firstBad == SIZE_MAX) {
            firstBad = i; firstOut = st.outL.p[i + delay]; firstIn = inL[i];
        }
    }
    if (maxErr > UNITY_MAXERR)
        fail(CAT_LATENCY,
             "%s: unity sample error %.6e > %.2f using reported delay %zu; "
             "first diff at input sample %zu (out=%.8f in=%.8f)",
             tag, maxErr, UNITY_MAXERR, delay, firstBad, firstOut, firstIn);

    // (2) Finite burst with zero tail: find the true delay by
    //     normalised cross-correlation over the burst region.
    size_t bs = burst.blockSize;
    size_t bn = burstSpec.nblocks * bs;
    size_t burstA = burstSpec.gapBlock0 * bs; // burst start
    size_t burstB = burstSpec.gapBlock1 * bs; // burst end
    size_t lo = delay / 2;
    size_t hi = 2 * delay;
    if (hi >= bn) hi = bn - 1;
    double bestCorr = -1.0;
    size_t bestLag = 0;
    for (size_t lag = lo; lag <= hi; ++lag) {
        double num = 0.0, d1 = 0.0, d2 = 0.0;
        for (size_t i = burstA; i < burstB && i + lag < bn; ++i) {
            num += double(burst.outL.p[i + lag]) * double(burstInL[i]);
            d1 += double(burst.outL.p[i + lag]) * double(burst.outL.p[i + lag]);
            d2 += double(burstInL[i]) * double(burstInL[i]);
        }
        double corr = num / std::sqrt(d1 * d2);
        if (corr > bestCorr) { bestCorr = corr; bestLag = lag; }
    }
    if (bestLag != delay)
        fail(CAT_LATENCY,
             "%s: burst measured delay %zu != reported %zu (corr %.8f)",
             tag, bestLag, delay, bestCorr);
    if (bestCorr < BURST_CORR_MIN)
        fail(CAT_LATENCY,
             "%s: burst correlation %.8f < %.3f at lag %zu (reported %zu)",
             tag, bestCorr, BURST_CORR_MIN, bestLag, delay);
}

} // namespace

int main()
{
    const SignalSpec signals[] = {
        { "inphase",       SIG_INPHASE,      440.0, 440.0, 240, 0,   0   },
        { "antiphase",     SIG_ANTIPHASE,    440.0, 440.0, 240, 0,   0   },
        { "distinct",      SIG_DISTINCT,     330.0, 550.0, 240, 0,   0   },
        { "gap_inphase",   SIG_GAP_INPHASE,  440.0, 440.0, 300, 100, 140 },
        { "gap_antiphase", SIG_GAP_ANTIPHASE,440.0, 440.0, 300, 100, 140 },
    };
    // Burst spec: gap fields carry the burst span [block 20, block 60),
    // the rest of the stream is silence (zero tail).
    const SignalSpec burstSpec =
        { "burst", SIG_BURST, 440.0, 440.0, 240, 20, 60 };

    struct WindowCfg { const char *name; int opt; };
    const WindowCfg windows[] = {
        { "short",  RubberBandLiveShifter::OptionWindowShort },
        { "medium", RubberBandLiveShifter::OptionWindowMedium },
    };
    const double scales[] = { 0.5, 1.0, 2.0 };

    // Block size comes from the library, never assumed.
    RubberBandLiveShifter probe(SR, 2,
        RubberBandLiveShifter::OptionWindowShort |
        RubberBandLiveShifter::OptionChannelsTogether);
    const size_t bs = probe.getBlockSize();

    for (const WindowCfg &wc : windows) {
        int opts = wc.opt | RubberBandLiveShifter::OptionChannelsTogether;
        for (double scale : scales) {
            for (const SignalSpec &spec : signals) {
                std::vector<float> inL, inR;
                buildSignal(spec, bs, inL, inR);
                auto ws = analysisWindows(spec, bs);

                const char *ifCpp;
                Stream cpp = runCpp(opts, scale, inL, inR, ifCpp);
                const char *ifC;
                Stream c = runC(opts, scale, inL, inR, ifC);

                checkStream(cpp, spec, ws, "C++", wc.name, scale);
                checkStream(c, spec, ws, "C", wc.name, scale);
                checkInterfaceParity(cpp, c, spec, wc.name, scale);

                std::printf("ok %s scale=%.1f %s block=%zu delay=%zu\n",
                            wc.name, scale, spec.name,
                            cpp.blockSize, cpp.startDelay);
            }

            if (scale == 1.0) {
                // Latency contract on the in-phase infinite sine and
                // the finite burst, through both interfaces.
                std::vector<float> sineL, sineR, burstL, burstR;
                buildSignal(signals[0], bs, sineL, sineR);
                buildSignal(burstSpec, bs, burstL, burstR);

                const char *ifCpp;
                Stream cppSine = runCpp(opts, 1.0, sineL, sineR, ifCpp);
                Stream cppBurst = runCpp(opts, 1.0, burstL, burstR, ifCpp);
                checkLatency(cppSine, sineL, burstSpec, cppBurst, burstL,
                             "C++", wc.name);

                const char *ifC;
                Stream cSine = runC(opts, 1.0, sineL, sineR, ifC);
                Stream cBurst = runC(opts, 1.0, burstL, burstR, ifC);
                checkLatency(cSine, sineL, burstSpec, cBurst, burstL,
                             "C", wc.name);

                std::printf("ok %s scale=1.0 latency delay=%zu\n",
                            wc.name, cppSine.startDelay);
            }
        }
    }

    for (int k = 0; k < CAT_COUNT; ++k) {
        if (g_failed[k]) {
            std::fprintf(stderr,
                         "LIVE_CONTRACT_FAILED category=%s\n",
                         CAT_NAME[k]);
        }
    }
    for (int k = 0; k < CAT_COUNT; ++k) {
        if (g_failed[k]) return 1;
    }
    std::printf("LIVE_CONTRACT_OK\n");
    return 0;
}
