////////////////////////////////////////////////////////////////////////////////
///
/// time_anchor_test.cpp : anchored time-stretch test suite.
///
/// Registered through register_time_anchor_tests() and driven by the harness
/// in task_support/test_main.cpp. All audio is read back through the public
/// SoundTouch putSamples/receiveSamples/flush interface.
///
////////////////////////////////////////////////////////////////////////////////

#include "test_api.h"
#include "SoundTouch.h"

#include <cmath>
#include <cstdint>
#include <vector>
#include <random>
#include <stdexcept>
#include <algorithm>

using namespace soundtouch;

namespace
{

const double PI = 3.14159265358979323846;

struct RunResult
{
    std::vector<float> pcm;       // interleaved
    uint64_t maxStorage = 0;      // max frames held inside the processor
    uint64_t firstOutputAt = 0;   // input frames fed when first output appeared
};

static SoundTouch makeProcessor(int channels)
{
    SoundTouch st;
    st.setSampleRate(48000);
    st.setChannels((uint)channels);
    // rate, pitch and tempo are set to 1 before the anchors, as required.
    st.setRate(1.0f);
    st.setPitch(1.0f);
    st.setTempo(1.0f);
    st.setSetting(SETTING_USE_QUICKSEEK, 0);
    st.setSetting(SETTING_SEQUENCE_MS, 40);
    st.setSetting(SETTING_SEEKWINDOW_MS, 15);
    st.setSetting(SETTING_OVERLAP_MS, 8);
    return st;
}

// Feeds all N frames using 'putChunk' frames per call and reads with a buffer
// of 'readCap' frames, exercising the streaming interface throughout.
static RunResult runStream(const std::vector<float> &input, int channels,
                           const std::vector<TimeAnchor> &anchors, uint P,
                           int putChunk, int readCap)
{
    SoundTouch st = makeProcessor(channels);
    st.setTimeAnchors(anchors, P);

    RunResult res;
    const uint64_t N = anchors.back().inputFrame;
    std::vector<float> rbuf((size_t)readCap * (size_t)channels);

    uint64_t put = 0;
    while (put < N)
    {
        uint n = (uint)std::min((uint64_t)putChunk, N - put);
        st.putSamples(input.data() + put * (size_t)channels, n);
        put += n;

        uint got;
        while ((got = st.receiveSamples(rbuf.data(), (uint)readCap)))
        {
            res.pcm.insert(res.pcm.end(), rbuf.begin(),
                           rbuf.begin() + (size_t)got * (size_t)channels);
        }
        if (res.firstOutputAt == 0 && !res.pcm.empty())
        {
            res.firstOutputAt = put;
        }
        uint64_t held = (uint64_t)st.numSamples() + st.numUnprocessedSamples();
        if (held > res.maxStorage) res.maxStorage = held;
    }

    st.flush();
    uint got;
    while ((got = st.receiveSamples(rbuf.data(), (uint)readCap)))
    {
        res.pcm.insert(res.pcm.end(), rbuf.begin(),
                       rbuf.begin() + (size_t)got * (size_t)channels);
    }
    return res;
}

static float maxAbsDiff(const std::vector<float> &a, const std::vector<float> &b)
{
    ANCHOR_CHECK(a.size() == b.size());
    float d = 0.0f;
    for (size_t i = 0; i < a.size(); ++i)
    {
        d = std::max(d, std::abs(a[i] - b[i]));
    }
    return d;
}

// Independent aperiodic content: low-level shaped noise plus isolated,
// non-repeating short pulses.
static std::vector<float> makePulseSignal(int frames, int channels, uint32_t seed,
                                          const std::vector<int> &pulseFrames)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> noise(-0.04f, 0.04f);
    std::vector<float> x((size_t)frames * (size_t)channels, 0.0f);
    for (int i = 0; i < frames; ++i)
    {
        float v = noise(rng);
        for (int c = 0; c < channels; ++c)
        {
            x[(size_t)i * channels + c] = c ? -v : v;
        }
    }
    for (size_t k = 0; k < pulseFrames.size(); ++k)
    {
        int h = pulseFrames[k];
        // short raised-cosine attack; every hit gets a distinct peak level so
        // its multi-frame signature is unique and a duplicated copy can be
        // detected unambiguously anywhere in the output.
        float peak = 0.55f + 0.07f * (float)k;
        for (int j = 0; j < 8 && h + j < frames; ++j)
        {
            float env = 0.5f * (1.0f - (float)std::cos(PI * (j + 0.5) / 8.0));
            float amp = peak * env;
            for (int c = 0; c < channels; ++c)
            {
                x[(size_t)(h + j) * channels + c] = c ? -amp : amp;
            }
        }
    }
    return x;
}

static std::vector<float> makeSine(int frames, int channels, float freq, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> noise(-0.01f, 0.01f);
    std::vector<float> x((size_t)frames * (size_t)channels);
    for (int i = 0; i < frames; ++i)
    {
        float v = 0.4f * (float)std::sin(2.0 * PI * freq * i / 48000.0) + noise(rng);
        for (int c = 0; c < channels; ++c)
        {
            x[(size_t)i * channels + c] = c ? -v : v;
        }
    }
    return x;
}

static double goertzelPeak(const std::vector<float> &x, int channels,
                           long begin, long end, double target)
{
    double bestFreq = target;
    double bestMag = -1.0;
    for (double f = target - 5.0; f <= target + 5.0; f += 1.0)
    {
        double w = 2.0 * PI * f / 48000.0;
        double coeff = 2.0 * std::cos(w);
        double p1 = 0.0, p2 = 0.0;
        for (long i = begin; i < end; ++i)
        {
            double s = (double)x[(size_t)i * channels] + coeff * p1 - p2;
            p2 = p1;
            p1 = s;
        }
        double mag = std::sqrt(p1 * p1 + p2 * p2 - coeff * p1 * p2);
        if (mag > bestMag)
        {
            bestMag = mag;
            bestFreq = f;
        }
    }
    return bestFreq;
}

static double rmsMono(const std::vector<float> &x, int channels, long begin, long end)
{
    if (end <= begin) return 0.0;
    double acc = 0.0;
    for (long i = begin; i < end; ++i)
    {
        double v = (double)x[(size_t)i * channels];
        acc += v * v;
    }
    return std::sqrt(acc / (double)(end - begin));
}

// Iterates over the stretch gaps (between neighbouring protected zones).
struct Gap
{
    uint64_t inBegin, inEnd, outBegin, outEnd;
};

static std::vector<Gap> gapList(const std::vector<TimeAnchor> &a, uint P)
{
    std::vector<Gap> g;
    for (size_t i = 0; i + 1 < a.size(); ++i)
    {
        g.push_back({
            a[i].inputFrame + P, a[i + 1].inputFrame - P,
            a[i].outputFrame + P, a[i + 1].outputFrame - P
        });
    }
    return g;
}

} // namespace


void register_time_anchor_tests(anchor_test::Registry &reg)
{
    //----------------------------------------------------------------------
    // islands : verbatim protected zones, exact placement, single attacks,
    // head/tail coverage, mono and stereo.
    //----------------------------------------------------------------------

    reg.add("islands", "required_mapping_verbatim", []()
    {
        const uint P = 128;
        const std::vector<TimeAnchor> anchors = {
            {0, 0}, {24000, 32000}, {48000, 48000}, {96000, 104000}
        };
        const std::vector<int> pulses = {0, 64, 24000, 48000, 95992};
        for (int ch : {1, 2})
        {
            auto x = makePulseSignal(96000, ch, 101u, pulses);
            auto r = runStream(x, ch, anchors, P, 4096, 4096);
            ANCHOR_CHECK(r.pcm.size() == (size_t)104000 * (size_t)ch);

            // head, every internal zone and the tail must be bit-for-bit the
            // original samples, at their exact mapped output positions.
            auto checkZone = [&](uint64_t in0, uint64_t out0, uint len)
            {
                for (uint i = 0; i < len; ++i)
                    for (int c = 0; c < ch; ++c)
                    {
                        float a = x[(size_t)(in0 + i) * ch + c];
                        float b = r.pcm[(size_t)(out0 + i) * ch + c];
                        ANCHOR_CHECK(std::abs(a - b) <= 2e-6f);
                    }
            };
            checkZone(0, 0, P);                                   // head
            checkZone(24000 - P, 32000 - P, 2 * P);               // internal
            checkZone(48000 - P, 48000 - P, 2 * P);               // internal
            checkZone(96000 - P, 104000 - P, P);                  // tail

            // each attack must appear exactly once: count the strong pulses in
            // the whole output and compare with the number of pulse attacks.
            int hits = 0;
            for (size_t i = 0; i < r.pcm.size(); i += (size_t)ch)
            {
                if (std::abs(r.pcm[i]) > 0.5f) ++hits;
            }
            // each attack spans several frames above the threshold
            ANCHOR_CHECK(hits >= (int)pulses.size());
            ANCHOR_CHECK(hits <= (int)pulses.size() * 8);

            // stronger "no second copy" check: the exact multi-frame signature
            // of every attack must occur in the output at exactly one location,
            // namely its protected, verbatim position. A WSOLA duplication would
            // never reproduce the signature bit-for-bit outside that location.
            for (int h : pulses)
            {
                if (h + 4 >= 96000) continue;
                const int sigLen = 4;
                int occurrences = 0;
                for (size_t o = 0; o + sigLen <= r.pcm.size() / (size_t)ch; ++o)
                {
                    bool match = true;
                    for (int j = 0; j < sigLen && match; ++j)
                    {
                        if (std::abs(r.pcm[(size_t)(o + j) * ch]
                                     - x[(size_t)(h + j) * ch]) > 1e-5f)
                        {
                            match = false;
                        }
                    }
                    if (match) ++occurrences;
                }
                ANCHOR_CHECK(occurrences == 1);
            }

            // the anchor attacks sit at their exact mapped output frames
            ANCHOR_CHECK(std::abs(r.pcm[(size_t)32000 * ch] - x[(size_t)24000 * ch]) <= 2e-6f);
            ANCHOR_CHECK(std::abs(r.pcm[(size_t)48000 * ch] - x[(size_t)48000 * ch]) <= 2e-6f);

            // stereo phase/scale relationship is preserved
            if (ch == 2)
            {
                for (size_t i = 0; i + 1 < r.pcm.size(); i += 2)
                {
                    ANCHOR_CHECK(std::abs(r.pcm[i] + r.pcm[i + 1]) <= 2e-6f);
                }
            }
        }
    });

    reg.add("islands", "non_divisible_coordinates", []()
    {
        const uint P = 64;
        // deliberately awkward coordinates that do not line up with any hop,
        // with gap ratios kept inside the legal [0.5, 2] band
        const std::vector<TimeAnchor> anchors = {
            {0, 0}, {30001, 40003}, {60007, 55211}, {100000, 110009}
        };
        const std::vector<int> pulses = {0, 30001, 60007, 99999};
        auto x = makePulseSignal(100000, 1, 202u, pulses);
        auto r = runStream(x, 1, anchors, P, 113, 37);
        ANCHOR_CHECK(r.pcm.size() == (size_t)110009);
        auto checkZone = [&](uint64_t in0, uint64_t out0, uint len)
        {
            for (uint i = 0; i < len; ++i)
                ANCHOR_CHECK(std::abs(x[(size_t)(in0 + i)] - r.pcm[(size_t)(out0 + i)]) <= 2e-6f);
        };
        checkZone(0, 0, P);
        checkZone(30001 - P, 40003 - P, 2 * P);
        checkZone(60007 - P, 55211 - P, 2 * P);
        checkZone(100000 - P, 110009 - P, P);
    });

    reg.add("islands", "all_silence", []()
    {
        const uint P = 128;
        const std::vector<TimeAnchor> anchors = {
            {0, 0}, {24000, 32000}, {48000, 48000}, {96000, 104000}
        };
        std::vector<float> x((size_t)96000 * 2, 0.0f);
        auto r = runStream(x, 2, anchors, P, 7, 113);
        ANCHOR_CHECK(r.pcm.size() == (size_t)104000 * 2);
        for (float v : r.pcm) ANCHOR_CHECK(v == 0.0f);
    });

    //----------------------------------------------------------------------
    // stretch : no fade-in at zone edges, pitch and RMS preservation.
    //----------------------------------------------------------------------

    reg.add("stretch", "constant_signal_no_edge_fade", []()
    {
        const uint P = 128;
        const std::vector<TimeAnchor> anchors = {
            {0, 0}, {24000, 32000}, {48000, 48000}, {96000, 104000}
        };
        const int N = 96000;
        std::vector<float> x((size_t)N * 2);
        for (int i = 0; i < N; ++i)
            for (int c = 0; c < 2; ++c)
                x[(size_t)i * 2 + c] = c ? -0.3f : 0.3f;
        auto r = runStream(x, 2, anchors, P, 4096, 4096);
        ANCHOR_CHECK(r.pcm.size() == (size_t)104000 * 2);

        // A constant signal stretched with overlap-add stays constant. Check
        // the stretch frames directly adjacent to every protected zone so a
        // fade to/from silence at the junction would be caught.
        for (const Gap &g : gapList(anchors, P))
        {
            for (uint64_t o = g.outBegin; o < g.outBegin + 4096 && o < g.outEnd; ++o)
            {
                ANCHOR_CHECK(std::abs(r.pcm[(size_t)o * 2] - 0.3f) <= 2e-6f);
                ANCHOR_CHECK(std::abs(r.pcm[(size_t)o * 2 + 1] + 0.3f) <= 2e-6f);
            }
            for (uint64_t o = g.outEnd - 4096; o < g.outEnd; ++o)
            {
                ANCHOR_CHECK(std::abs(r.pcm[(size_t)o * 2] - 0.3f) <= 2e-6f);
                ANCHOR_CHECK(std::abs(r.pcm[(size_t)o * 2 + 1] + 0.3f) <= 2e-6f);
            }
        }
    });

    static const auto sineQuality = [](float freq)
    {
        const uint P = 128;
        // long gaps: one expansion (~1.5x) and one compression (~0.52x)
        const std::vector<TimeAnchor> anchors = {
            {0, 0}, {120000, 180000}, {240000, 242000}
        };
        auto x = makeSine(240000, 1, freq, 303u);
        auto r = runStream(x, 1, anchors, P, 4096, 4096);
        ANCHOR_CHECK(r.pcm.size() == (size_t)242000);

        const long margin = 4096;   // keep away from zones and junctions
        for (const Gap &g : gapList(anchors, P))
        {
            long ob = (long)g.outBegin + margin;
            long oe = (long)g.outEnd - margin;
            long ib = (long)g.inBegin + margin;
            long ie = (long)g.inEnd - margin;
            ANCHOR_CHECK(oe - ob > 20000);

            double peak = goertzelPeak(r.pcm, 1, ob, oe, (double)freq);
            ANCHOR_CHECK(std::abs(peak - (double)freq) <= 2.0);

            double rmsOut = rmsMono(r.pcm, 1, ob, oe);
            double rmsIn = rmsMono(x, 1, ib, ie);
            double db = 20.0 * std::log10(rmsOut / rmsIn);
            ANCHOR_CHECK(std::abs(db) <= 2.0);
        }
    };

    reg.add("stretch", "pitch_rms_220hz", []() { sineQuality(220.0f); });
    reg.add("stretch", "pitch_rms_997hz", []() { sineQuality(997.0f); });

    reg.add("stretch", "fast_then_slow", []()
    {
        const uint P = 128;
        // first gap compressed (out < in), second expanded (out > in)
        const std::vector<TimeAnchor> anchors = {
            {0, 0}, {60000, 40000}, {120000, 110000}
        };
        auto x = makeSine(120000, 2, 440.0f, 404u);
        auto r = runStream(x, 2, anchors, P, 1024, 2048);
        ANCHOR_CHECK(r.pcm.size() == (size_t)110000 * 2);
        for (float v : r.pcm) ANCHOR_CHECK(std::isfinite(v));
        // verbatim anchor zones still hold
        for (int c = 0; c < 2; ++c)
            ANCHOR_CHECK(std::abs(r.pcm[(size_t)40000 * 2 + c] - x[(size_t)60000 * 2 + c]) <= 2e-6f);
    });

    //----------------------------------------------------------------------
    // stream : chunking equivalence, lifecycle/error rules, bounded storage
    // and early output.
    //----------------------------------------------------------------------

    reg.add("stream", "chunk_and_cap_equivalence", []()
    {
        const uint P = 128;
        const std::vector<TimeAnchor> anchors = {
            {0, 0}, {24000, 32000}, {48000, 48000}, {96000, 104000}
        };
        for (int ch : {1, 2})
        {
            auto x = makePulseSignal(96000, ch, 505u, {0, 24000, 48000, 95999});
            auto big = runStream(x, ch, anchors, P, 4096, 4096);
            auto odd = runStream(x, ch, anchors, P, 1, 1);
            auto seven = runStream(x, ch, anchors, P, 7, 113);
            auto large = runStream(x, ch, anchors, P, 113, 7);

            const uint64_t M = 104000;
            ANCHOR_CHECK(big.pcm.size() == (size_t)M * (size_t)ch);
            ANCHOR_CHECK(odd.pcm.size() == big.pcm.size());
            ANCHOR_CHECK(seven.pcm.size() == big.pcm.size());
            ANCHOR_CHECK(large.pcm.size() == big.pcm.size());
            ANCHOR_CHECK(maxAbsDiff(big.pcm, odd.pcm) <= 2e-5f);
            ANCHOR_CHECK(maxAbsDiff(big.pcm, seven.pcm) <= 2e-5f);
            ANCHOR_CHECK(maxAbsDiff(big.pcm, large.pcm) <= 2e-5f);
        }
    });

    reg.add("stream", "early_flush_rejected_then_completes", []()
    {
        const uint P = 128;
        const std::vector<TimeAnchor> anchors = {{0, 0}, {40000, 50000}};
        auto x = makeSine(40000, 1, 220.0f, 606u);
        SoundTouch st = makeProcessor(1);
        st.setTimeAnchors(anchors, P);

        std::vector<float> rbuf(4096), out;
        auto drain = [&]()
        {
            uint got;
            while ((got = st.receiveSamples(rbuf.data(), 4096)))
                out.insert(out.end(), rbuf.begin(), rbuf.begin() + got);
        };
        st.putSamples(x.data(), 20000);
        drain();

        bool threw = false;
        try { st.flush(); }
        catch (const std::logic_error &) { threw = true; }
        ANCHOR_CHECK(threw);

        // state unchanged: can still feed the remainder and finish
        st.putSamples(x.data() + 20000, 20000);
        st.flush();
        drain();
        ANCHOR_CHECK(out.size() == (size_t)50000);
    });

    reg.add("stream", "overshoot_rejected_before_write", []()
    {
        const uint P = 128;
        const std::vector<TimeAnchor> anchors = {{0, 0}, {40000, 40000}};
        auto x = makeSine(40000, 1, 220.0f, 707u);
        SoundTouch st = makeProcessor(1);
        st.setTimeAnchors(anchors, P);

        std::vector<float> rbuf(4096), out;
        auto drain = [&]()
        {
            uint got;
            while ((got = st.receiveSamples(rbuf.data(), 4096)))
                out.insert(out.end(), rbuf.begin(), rbuf.begin() + got);
        };

        st.putSamples(x.data(), 30000);
        drain();
        uint unprocBefore = st.numUnprocessedSamples();
        bool threw = false;
        try { st.putSamples(x.data(), 20000); }   // would exceed N = 40000
        catch (const std::invalid_argument &) { threw = true; }
        ANCHOR_CHECK(threw);
        ANCHOR_CHECK(st.numUnprocessedSamples() == unprocBefore);

        // the exact remaining amount is accepted
        st.putSamples(x.data() + 30000, 10000);
        st.flush();
        drain();
        ANCHOR_CHECK(out.size() == (size_t)40000);

        // after finish, further non-empty input is rejected and repeat flush is silent
        bool logic = false;
        try { st.putSamples(x.data(), 1); }
        catch (const std::logic_error &) { logic = true; }
        ANCHOR_CHECK(logic);
        size_t before = out.size();
        st.flush();
        ANCHOR_CHECK(st.receiveSamples(rbuf.data(), 4096) == 0);
        ANCHOR_CHECK(out.size() == before);
    });

    reg.add("stream", "zero_input_and_empty_read_no_advance", []()
    {
        const uint P = 128;
        const std::vector<TimeAnchor> anchors = {{0, 0}, {40000, 40000}};
        SoundTouch st = makeProcessor(1);
        st.setTimeAnchors(anchors, P);
        std::vector<float> empty;
        st.putSamples(empty.data(), 0);                 // zero-length input
        ANCHOR_CHECK(st.numSamples() == 0);
        std::vector<float> rbuf(16);
        ANCHOR_CHECK(st.receiveSamples(rbuf.data(), 16) == 0);  // empty read
        // configuring again while idle is still allowed
        st.setTimeAnchors({{0, 0}, {40000, 30000}}, P);
        st.setTimeAnchors(std::vector<TimeAnchor>(), P);        // revoke with empty list
        // after revoking, the legacy tempo/pitch/rate controls are usable again
        st.setTempo(1.5f);
        bool legacyWorked = false;
        std::vector<float> tone8k = makeSine(8192, 1, 220.0f, 1212u);
        st.putSamples(tone8k.data(), 8192);
        st.flush();
        std::vector<float> legacyOut(8192);
        if (st.receiveSamples(legacyOut.data(), 8192) > 0) legacyWorked = true;
        ANCHOR_CHECK(legacyWorked);
    });

    reg.add("stream", "invalid_arguments_and_config_locking", []()
    {
        SoundTouch st = makeProcessor(1);
        auto expectInvalid = [&](const std::vector<TimeAnchor> &a, uint pp)
        {
            bool threw = false;
            try { st.setTimeAnchors(a, pp); }
            catch (const std::invalid_argument &) { threw = true; }
            ANCHOR_CHECK(threw);
        };
        expectInvalid({{1, 0}, {100000, 100000}}, 128);          // first not (0,0)
        expectInvalid({{0, 0}, {100000, 100000}}, 8);            // P too small
        expectInvalid({{0, 0}, {100000, 100000}}, 300);          // P too big
        expectInvalid({{0, 0}, {5000, 5000}}, 128);              // zones too close
        expectInvalid({{0, 0}, {100000, 40000}}, 128);           // ratio out of range
        expectInvalid({{0, 0}, {50000, 50000}, {50000, 80000}}, 128); // not increasing
        expectInvalid({{0, 0}, {(uint64_t(1) << 54), 100}}, 128);     // coordinate > 2^53

        // a valid list starts the stream; afterwards config/anchors are locked
        st.setTimeAnchors({{0, 0}, {100000, 100000}}, 128);
        auto x = makeSine(100000, 1, 220.0f, 808u);
        st.putSamples(x.data(), 4096);
        bool logic = false;
        try { st.setTimeAnchors({{0, 0}, {100000, 100000}}, 128); }
        catch (const std::logic_error &) { logic = true; }
        ANCHOR_CHECK(logic);
        logic = false;
        try { st.setTempo(2.0f); }
        catch (const std::logic_error &) { logic = true; }
        ANCHOR_CHECK(logic);
        logic = false;
        try { st.setChannels(2); }
        catch (const std::logic_error &) { logic = true; }
        ANCHOR_CHECK(logic);
        logic = false;
        try { st.setSampleRate(44100); }
        catch (const std::logic_error &) { logic = true; }
        ANCHOR_CHECK(logic);
    });

    reg.add("stream", "clear_then_reuse", []()
    {
        const uint P = 128;
        SoundTouch st = makeProcessor(2);
        st.setTimeAnchors({{0, 0}, {50000, 60000}}, P);
        auto first = makeSine(50000, 2, 220.0f, 909u);
        std::vector<float> rb(4096 * 2), out;
        for (uint64_t p = 0; p < 50000;)
        {
            uint n = (uint)std::min(4096ull, 50000 - p);
            st.putSamples(first.data() + p * 2, n);
            p += n;
            uint g;
            while ((g = st.receiveSamples(rb.data(), 4096)))
                out.insert(out.end(), rb.begin(), rb.begin() + (size_t)g * 2);
        }
        st.flush();
        uint g;
        while ((g = st.receiveSamples(rb.data(), 4096)))
            out.insert(out.end(), rb.begin(), rb.begin() + (size_t)g * 2);
        ANCHOR_CHECK(out.size() == (size_t)60000 * 2);

        // clear drops history and anchors but keeps rate/channels
        st.clear();
        ANCHOR_CHECK(st.numSamples() == 0);
        ANCHOR_CHECK(st.numUnprocessedSamples() == 0);
        st.setTimeAnchors({{0, 0}, {40000, 30000}}, 64);
        auto second = makeSine(40000, 2, 997.0f, 910u);
        std::vector<float> out2;
        for (uint64_t p = 0; p < 40000;)
        {
            uint n = (uint)std::min(4096ull, 40000 - p);
            st.putSamples(second.data() + p * 2, n);
            p += n;
            uint gg;
            while ((gg = st.receiveSamples(rb.data(), 4096)))
                out2.insert(out2.end(), rb.begin(), rb.begin() + (size_t)gg * 2);
        }
        st.flush();
        uint gg;
        while ((gg = st.receiveSamples(rb.data(), 4096)))
            out2.insert(out2.end(), rb.begin(), rb.begin() + (size_t)gg * 2);
        ANCHOR_CHECK(out2.size() == (size_t)30000 * 2);
    });

    reg.add("stream", "clear_restores_numeric_and_audio_settings", []()
    {
        const uint P = 128;
        SoundTouch st = makeProcessor(1);
        st.setTempo(1.5f);                 // ordinary setting to be preserved
        st.setTimeAnchors({{0, 0}, {40000, 40000}}, P);
        auto x = makeSine(40000, 1, 220.0f, 1313u);
        std::vector<float> rb(4096), anchored;
        for (uint64_t p = 0; p < 40000;)
        {
            uint n = (uint)std::min(4096ull, 40000 - p);
            st.putSamples(x.data() + p, n);
            p += n;
            uint g;
            while ((g = st.receiveSamples(rb.data(), 4096)))
                anchored.insert(anchored.end(), rb.begin(), rb.begin() + g);
        }
        st.flush();
        uint g;
        while ((g = st.receiveSamples(rb.data(), 4096)))
            anchored.insert(anchored.end(), rb.begin(), rb.begin() + g);
        ANCHOR_CHECK(anchored.size() == (size_t)40000);

        st.clear();
        // sample rate / channels survive clear(), and the tempo 1.5 setting is
        // restored, so a legacy pass now produces fewer frames than the input.
        ANCHOR_CHECK(st.numSamples() == 0);
        std::vector<float> legacy;
        for (uint64_t p = 0; p < 40000;)
        {
            uint n = (uint)std::min(4096ull, 40000 - p);
            st.putSamples(x.data() + p, n);
            p += n;
            uint gg;
            while ((gg = st.receiveSamples(rb.data(), 4096)))
                legacy.insert(legacy.end(), rb.begin(), rb.begin() + gg);
        }
        st.flush();
        uint gg;
        while ((gg = st.receiveSamples(rb.data(), 4096)))
            legacy.insert(legacy.end(), rb.begin(), rb.begin() + gg);
        // tempo 1.5 shortens the stream: clearly fewer than the 40000 inputs
        ANCHOR_CHECK(legacy.size() > 0);
        ANCHOR_CHECK(legacy.size() < 40000u);
        ANCHOR_CHECK(legacy.size() < 32000u);     // comfortably in the 1.5-tempo range
    });

    reg.add("stream", "long_gap_early_output_and_bounded_storage", []()
    {
        const uint P = 128;
        // very long gaps; storage must not grow with the gap length
        const std::vector<TimeAnchor> anchors = {
            {0, 0}, {400000, 440000}, {800000, 800000}
        };
        auto x = makeSine(800000, 2, 997.0f, 111u);
        auto r = runStream(x, 2, anchors, P, 4096, 4096);
        ANCHOR_CHECK(r.pcm.size() == (size_t)800000 * 2);
        // output starts flowing after only a small, bounded amount of input,
        // long before the first gap or the whole stream is available
        ANCHOR_CHECK(r.firstOutputAt > 0);
        ANCHOR_CHECK(r.firstOutputAt <= 8192);
        // total internal PCM storage stays well under the 65536 frame cap
        ANCHOR_CHECK(r.maxStorage <= 65536);
    });
}
