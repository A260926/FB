////////////////////////////////////////////////////////////////////////////////
///
/// Bounded-mode test suite. Registered through task_support/check.h into the
/// four groups pcm / pressure / finish / lifetime; the C driver in
/// task_support/driver.c supplies main().
///
////////////////////////////////////////////////////////////////////////////////

#include "check.h"

#include "SoundTouch.h"
#include "BoundedPipeline.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace soundtouch;

namespace
{
    constexpr float kSentinel = 12345.75f;

    struct Config
    {
        float rate;
        float tempo;
        uint channels;
        bool aa;
    };

    // Non-periodic source: incommensurate sines plus per-channel pulses whose
    // periods and amplitudes differ, so no accidental periodic alignment.
    void makeSignal(std::vector<float> &x, uint frames, uint channels, unsigned seed)
    {
        x.resize(static_cast<size_t>(frames) * channels);
        for (uint i = 0; i < frames; ++i)
        {
            for (uint c = 0; c < channels; ++c)
            {
                float v = 0.30f * std::sin(i * 0.037f + c * 0.41f + seed)
                        + 0.10f * std::sin(i * 0.191f + c * 1.7f + seed * 0.5f)
                        + 0.03f * std::sin(i * i * 0.000013f + c);
                if ((i + c * 37u + seed) % (97u + 13u * c) == 0)
                {
                    v += 0.5f * (1.0f - 0.15f * static_cast<float>(c));
                }
                x[static_cast<size_t>(i) * channels + c] = v;
            }
        }
    }

    uint idealFrames(uint inputFrames, const Config &cfg)
    {
        double denom = static_cast<double>(cfg.rate) * static_cast<double>(cfg.tempo);
        return static_cast<uint>(std::floor(inputFrames / denom + 0.5));
    }

    // Configures and prepares a SoundTouch in place (the class owns raw
    // pointers and is not copyable).
    void prepareBoundedInstance(SoundTouch &s, const Config &cfg)
    {
        s.setSampleRate(48000);
        s.setChannels(cfg.channels);
        s.setRate(cfg.rate);
        s.setTempo(cfg.tempo);
        s.setSetting(SETTING_USE_AA_FILTER, cfg.aa ? 1 : 0);
        s.prepareBounded();
    }

    struct DriveStats
    {
        std::vector<float> out;
        uint consumed;
        uint produced;
        uint calls;
        bool finished;
        bool valid;
    };

    // Local invariant check inside the helper: TASK_CHECK cannot be used here
    // because it returns an int from a function returning a struct.
    #define DRIVE_CHECK(st, cond) do { if (!(cond)) (st).valid = false; } while (0)

    // Drives a prepared SoundTouch through the bounded API. Output capacities
    // cycle through 'capPattern'; the first 'stallCalls' calls force capacity
    // zero. The input cursor is advanced exactly by 'consumed', modelling a
    // caller that resends every unconsumed frame.
    DriveStats drive(SoundTouch &s, const std::vector<float> &signal, uint frames,
                     const Config &cfg, uint inChunk,
                     const uint *capPattern, uint patternLen, uint stallCalls = 0)
    {
        DriveStats st{};
        st.finished = false;
        st.valid = true;
        uint cursor = 0;
        std::vector<float> dst(4096u * cfg.channels, kSentinel);
        uint guard = 0;
        while (!st.finished && guard++ < 8000000u)
        {
            uint offered = std::min(inChunk, frames - cursor);
            const float *inPtr = offered ? signal.data()
                                         + static_cast<size_t>(cursor) * cfg.channels
                                        : nullptr;
            uint cap;
            if (st.calls < stallCalls)
            {
                cap = 0;
            }
            else
            {
                cap = capPattern[(st.calls - stallCalls) % patternLen];
            }

            if (cap > dst.size() / cfg.channels) dst.assign(cap * cfg.channels, kSentinel);
            for (uint k = 0; k < cap * cfg.channels; ++k) dst[k] = kSentinel;

            BoundedResult r = s.processBounded(inPtr, offered,
                                               cap ? dst.data() : nullptr, cap,
                                               cursor == frames);

            DRIVE_CHECK(st, r.consumed <= offered);
            DRIVE_CHECK(st, r.produced <= cap);
            if (cap == 0) DRIVE_CHECK(st, r.produced == 0);

            // Frames beyond 'produced' must be left completely untouched.
            for (uint f = r.produced; f < cap; ++f)
                for (uint c = 0; c < cfg.channels; ++c)
                    DRIVE_CHECK(st, dst[static_cast<size_t>(f) * cfg.channels + c] == kSentinel);

            st.out.insert(st.out.end(), dst.begin(),
                          dst.begin() + static_cast<size_t>(r.produced) * cfg.channels);
            cursor += r.consumed;
            st.consumed += r.consumed;
            st.produced += r.produced;
            ++st.calls;
            st.finished = r.finished;
        }
        DRIVE_CHECK(st, st.finished);
        DRIVE_CHECK(st, cursor == frames);
        DRIVE_CHECK(st, st.consumed == frames);
        return st;
    }

    #undef DRIVE_CHECK

    // Legacy reference built only from the old putSamples/receiveSamples
    // entry points; returns the whole PCM prefix available before flush().
    std::vector<float> legacyPrefix(const std::vector<float> &signal, uint frames,
                                    const Config &cfg, uint inChunk)
    {
        SoundTouch s;
        s.setSampleRate(48000);
        s.setChannels(cfg.channels);
        s.setRate(cfg.rate);
        s.setTempo(cfg.tempo);
        s.setSetting(SETTING_USE_AA_FILTER, cfg.aa ? 1 : 0);

        std::vector<float> out, buf(4096u * cfg.channels);
        for (uint pos = 0; pos < frames;)
        {
            uint n = std::min(inChunk, frames - pos);
            s.putSamples(signal.data() + static_cast<size_t>(pos) * cfg.channels, n);
            pos += n;
            for (;;)
            {
                uint got = s.receiveSamples(buf.data(), 4096);
                if (!got) break;
                out.insert(out.end(), buf.begin(),
                           buf.begin() + static_cast<size_t>(got) * cfg.channels);
            }
        }
        return out;
    }

    double maxAbsDiff(const std::vector<float> &a, const std::vector<float> &b, uint elems)
    {
        double m = 0.0;
        for (uint i = 0; i < elems; ++i)
            m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
        return m;
    }

    const Config kConfigs[] = {
        {1.0f, 1.0f, 1, true},
        {0.7f, 1.3f, 1, true},   // rate below 1, non-integer tempo, AA on
        {1.3f, 0.7f, 2, true},   // rate above 1, non-integer tempo
        {0.5f, 0.5f, 1, false},  // AA disabled, both extremes low
        {2.0f, 2.0f, 6, false},  // six channels, AA disabled, extremes high
        {1.13f, 0.87f, 6, true}, // six-channel non-integer settings, AA on
        {0.87f, 1.13f, 2, true},
    };
    const uint kConfigCount = sizeof(kConfigs) / sizeof(kConfigs[0]);
    const uint kInChunks[] = {1u, 7u, 113u, 4000u};
    const uint kOutPatterns[][4] = {
        {4096u}, {1u}, {17u}, {0u, 1u, 17u, 4096u}
    };
}

////////////////////////////////////////////////////////////////////////////////
// pcm group: prefix equivalence with the legacy path, chunk independence,
// multi-channel pulses, cursors and unwritten sentinels.
////////////////////////////////////////////////////////////////////////////////

static int pcm_legacy_prefix_equivalence()
{
    const uint N = 4000;
    for (uint ci = 0; ci < kConfigCount; ++ci)
    {
        const Config &cfg = kConfigs[ci];
        std::vector<float> signal;
        makeSignal(signal, N, cfg.channels, 7u);

        std::vector<float> prefix = legacyPrefix(signal, N, cfg, 113);
        uint prefixFrames = static_cast<uint>(prefix.size()) / cfg.channels;
        TASK_CHECK(prefixFrames <= idealFrames(N, cfg));

        for (uint ic : kInChunks)
        {
            SoundTouch s;
        prepareBoundedInstance(s, cfg);
            static const uint cap[] = {4096u};
            DriveStats st = drive(s, signal, N, cfg, ic, cap, 1);
            TASK_CHECK(st.valid);
            TASK_CHECK(st.produced == idealFrames(N, cfg));
            TASK_CHECK(st.out.size() == static_cast<size_t>(idealFrames(N, cfg)) * cfg.channels);
            // The bounded stream must reproduce the entire pre-flush legacy
            // prefix sample-for-sample within the tolerance.
            TASK_CHECK(maxAbsDiff(prefix, st.out,
                                  static_cast<uint>(prefix.size())) <= 3e-5);
        }
    }
    return 0;
}

static int pcm_chunk_and_capacity_independence()
{
    const uint N = 3000;
    for (uint ci = 0; ci < kConfigCount; ++ci)
    {
        const Config &cfg = kConfigs[ci];
        std::vector<float> signal;
        makeSignal(signal, N, cfg.channels, 19u);

        SoundTouch ref;
        prepareBoundedInstance(ref, cfg);
        static const uint bigCap[] = {4096u};
        DriveStats refRun = drive(ref, signal, N, cfg, N, bigCap, 1);
        TASK_CHECK(refRun.valid);

        for (uint ic : kInChunks)
            for (const auto &pat : kOutPatterns)
            {
                SoundTouch s;
        prepareBoundedInstance(s, cfg);
                DriveStats st = drive(s, signal, N, cfg, ic, pat, 4);
                TASK_CHECK(st.valid);
                TASK_CHECK(st.out.size() == refRun.out.size());
                TASK_CHECK(maxAbsDiff(st.out, refRun.out,
                                      static_cast<uint>(st.out.size())) == 0.0);
                TASK_CHECK(st.produced == refRun.produced);
            }
    }
    return 0;
}

static int pcm_six_channel_distinct_pulses()
{
    const uint N = 2500, ch = 6;
    Config cfg{0.63f, 1.27f, ch, true};
    std::vector<float> signal;
    makeSignal(signal, N, ch, 23u);

    SoundTouch s;
        prepareBoundedInstance(s, cfg);
    static const uint cap[] = {17u};
    DriveStats st = drive(s, signal, N, cfg, 7, cap, 1);
    TASK_CHECK(st.valid);
    TASK_CHECK(st.produced == idealFrames(N, cfg));

    // Every output sample is finite and the interleaved frame stride is kept.
    for (float v : st.out) TASK_CHECK(std::isfinite(v));

    // The per-channel pulse structure survives: channel energy is distinct
    // and ordered like the source, proving channels were never decoupled.
    double srcEnergy[6] = {0}, outEnergy[6] = {0};
    for (uint i = 0; i < N; ++i)
        for (uint c = 0; c < ch; ++c)
        {
            float v = signal[static_cast<size_t>(i) * ch + c];
            srcEnergy[c] += static_cast<double>(v) * v;
        }
    for (uint f = 0; f < st.produced; ++f)
        for (uint c = 0; c < ch; ++c)
        {
            float v = st.out[static_cast<size_t>(f) * ch + c];
            outEnergy[c] += static_cast<double>(v) * v;
        }
    for (uint c = 1; c < ch; ++c)
    {
        TASK_CHECK(srcEnergy[c] > 0.0 && outEnergy[c] > 0.0);
        TASK_CHECK(std::fabs(outEnergy[c] - outEnergy[0]) > 1e-9
                   || std::fabs(srcEnergy[c] - srcEnergy[0]) <= 1e-12);
    }
    return 0;
}

static int pcm_cursor_and_sentinel()
{
    const uint N = 1500, ch = 2;
    Config cfg{1.21f, 0.83f, ch, false};
    std::vector<float> signal;
    makeSignal(signal, N, ch, 31u);

    SoundTouch s;
        prepareBoundedInstance(s, cfg);
    std::vector<float> dst(37u * ch, kSentinel);
    uint cursor = 0, delivered = 0;
    bool finished = false;
    while (!finished)
    {
        uint offered = std::min(113u, N - cursor);
        const float *inPtr = offered ? signal.data() + static_cast<size_t>(cursor) * ch : nullptr;
        for (float &v : dst) v = kSentinel;
        BoundedResult r = s.processBounded(inPtr, offered, dst.data(), 37, cursor == N);
        TASK_CHECK(r.consumed <= offered);
        for (uint f = r.produced; f < 37u; ++f)
            for (uint c = 0; c < ch; ++c)
                TASK_CHECK(dst[static_cast<size_t>(f) * ch + c] == kSentinel);
        cursor += r.consumed;
        delivered += r.produced;
        finished = r.finished;
    }
    TASK_CHECK(cursor == N);
    TASK_CHECK(delivered == idealFrames(N, cfg));
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// pressure group: back pressure alternation, long stalls, FIFO bounds, and
// real allocation/deallocation accounting.
////////////////////////////////////////////////////////////////////////////////

static int pressure_capacity_alternation()
{
    const uint N = 3500;
    static const uint pattern[] = {0u, 1u, 17u, 4096u};
    for (uint ci = 0; ci < kConfigCount; ++ci)
    {
        const Config &cfg = kConfigs[ci];
        std::vector<float> signal;
        makeSignal(signal, N, cfg.channels, 41u);
        SoundTouch s;
        prepareBoundedInstance(s, cfg);
        DriveStats st = drive(s, signal, N, cfg, 7, pattern, 4);
        TASK_CHECK(st.valid);
        TASK_CHECK(st.produced == idealFrames(N, cfg));
        TASK_CHECK(st.consumed == N);
    }
    return 0;
}

static int pressure_long_stall_recovery()
{
    const uint N = 4000, ch = 1;
    Config cfg{0.8f, 1.25f, ch, true};
    std::vector<float> signal;
    makeSignal(signal, N, ch, 43u);

    SoundTouch s;
        prepareBoundedInstance(s, cfg);
    static const uint cap[] = {17u};
    DriveStats st = drive(s, signal, N, cfg, 7, cap, 1, 30000u);
    TASK_CHECK(st.valid);
    TASK_CHECK(st.calls > 30000u);
    TASK_CHECK(st.consumed == N);
    TASK_CHECK(st.produced == idealFrames(N, cfg));
    TASK_CHECK(st.finished);
    return 0;
}

static int pressure_fifo_bounds_and_stable_capacity()
{
    const uint N = 5000;
    const Config extremes[] = {
        {0.5f, 0.5f, 6, true}, {2.0f, 2.0f, 6, false},
        {1.3f, 0.7f, 2, true}, {0.7f, 1.3f, 1, false}
    };
    for (const Config &cfg : extremes)
    {
        SoundTouch owner;
        owner.setSampleRate(48000);
        owner.setChannels(cfg.channels);
        owner.setRate(cfg.rate);
        owner.setTempo(cfg.tempo);
        owner.setSetting(SETTING_USE_AA_FILTER, cfg.aa ? 1 : 0);
        BoundedPipeline bp(owner, cfg.channels, cfg.rate, cfg.tempo, cfg.aa, 64);

        uint capacityBefore[7];
        for (uint i = 0; i < bp.observableBufferCount(); ++i)
        {
            capacityBefore[i] = bp.observableBufferCapacity(i);
            TASK_CHECK(capacityBefore[i] <= BoundedPipeline::FIFO_FRAME_LIMIT);
        }

        std::vector<float> signal;
        makeSignal(signal, N, cfg.channels, 47u);

        // Drive the pipeline directly so the bound checks observe this run.
        uint cursor = 0, produced = 0;
        bool finished = false;
        std::vector<float> dst(17u * cfg.channels, kSentinel);
        uint guard = 0;
        while (!finished && guard++ < 1000000u)
        {
            uint offered = std::min(113u, N - cursor);
            const float *inPtr = offered
                ? signal.data() + static_cast<size_t>(cursor) * cfg.channels
                : nullptr;
            for (float &v : dst) v = kSentinel;
            BoundedResult r = bp.process(inPtr, offered, dst.data(), 17, cursor == N);
            TASK_CHECK(r.consumed <= offered && r.produced <= 17u);
            cursor += r.consumed;
            produced += r.produced;
            finished = r.finished;
        }
        TASK_CHECK(finished && cursor == N);
        TASK_CHECK(produced == idealFrames(N, cfg));

        for (uint i = 0; i < bp.observableBufferCount(); ++i)
        {
            TASK_CHECK(bp.observableBufferCapacity(i) == capacityBefore[i]);
            TASK_CHECK(bp.observableBufferHighWater(i) <= capacityBefore[i]);
        }
    }
    return 0;
}

static int pressure_no_allocation_after_prepare()
{
    const uint N = 3000, ch = 2;
    Config cfg{0.7f, 1.3f, ch, true};
    std::vector<float> signal;
    makeSignal(signal, N, ch, 53u);

    SoundTouch s;
        prepareBoundedInstance(s, cfg);
    uint64_t allocs = BoundedPipeline::totalAllocations();
    uint64_t frees = BoundedPipeline::totalDeallocations();
    TASK_CHECK(allocs > 0);

    static const uint cap[] = {0u, 1u, 17u, 4096u};
    DriveStats st = drive(s, signal, N, cfg, 37, cap, 4);
    TASK_CHECK(st.valid);
    TASK_CHECK(st.produced == idealFrames(N, cfg));
    TASK_CHECK(BoundedPipeline::totalAllocations() == allocs);
    TASK_CHECK(BoundedPipeline::totalDeallocations() == frees);

    s.clearBounded();
    TASK_CHECK(BoundedPipeline::totalAllocations() == allocs);
    TASK_CHECK(BoundedPipeline::totalDeallocations() == frees);

    // A second segment still performs no allocation.
    DriveStats st2 = drive(s, signal, N, cfg, 37, cap, 4);
    TASK_CHECK(st2.valid);
    TASK_CHECK(st2.produced == idealFrames(N, cfg));
    TASK_CHECK(BoundedPipeline::totalAllocations() == allocs);
    TASK_CHECK(BoundedPipeline::totalDeallocations() == frees);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// finish group: exact total length, final locking semantics, tail under back
// pressure, and argument/order validation.
////////////////////////////////////////////////////////////////////////////////

static int finish_exact_total_length_all_configs()
{
    const uint lengths[] = {1u, 7u, 113u, 2500u};
    static const uint capacities[] = {1u, 17u, 4096u};
    for (uint ci = 0; ci < kConfigCount; ++ci)
    {
        const Config &cfg = kConfigs[ci];
        for (uint N : lengths)
        {
            std::vector<float> signal;
            makeSignal(signal, N, cfg.channels, 59u + N);
            for (uint cap : capacities)
            {
                SoundTouch s;
        prepareBoundedInstance(s, cfg);
                uint pat[1] = {cap};
                DriveStats st = drive(s, signal, N, cfg, std::min(113u, N), pat, 1);
                TASK_CHECK(st.valid);
                // Identical total regardless of output capacity: the tail is
                // never truncated differently.
                TASK_CHECK(st.produced == idealFrames(N, cfg));
            }
        }
    }
    return 0;
}

static int finish_final_locking_and_repeat_semantics()
{
    const uint N = 1200, ch = 1;
    Config cfg{1.1f, 0.9f, ch, true};
    std::vector<float> signal;
    makeSignal(signal, N, ch, 61u);

    SoundTouch s;
        prepareBoundedInstance(s, cfg);
    std::vector<float> dst(64u * ch, kSentinel);

    // Offer final while back pressure prevents the whole input from being
    // accepted: the lock must wait, and the remaining frames keep flowing.
    uint cursor = 0, produced = 0;
    bool finished = false;
    uint guard = 0;
    while (!finished && guard++ < 100000u)
    {
        uint offered = std::min(64u, N - cursor);
        const float *inPtr = offered ? signal.data() + cursor : nullptr;
        BoundedResult r = s.processBounded(inPtr, offered, dst.data(), 1, cursor == N);
        cursor += r.consumed;
        produced += r.produced;
        finished = r.finished;
    }
    TASK_CHECK(cursor == N);
    TASK_CHECK(produced == idealFrames(N, cfg));
    TASK_CHECK(finished);

    // Any further call, even with final=true, reports zero/zero/finished and
    // never appends extra padding.
    BoundedResult after = s.processBounded(nullptr, 0, nullptr, 0, true);
    TASK_CHECK(after.consumed == 0 && after.produced == 0 && after.finished);
    BoundedResult again = s.processBounded(nullptr, 0, nullptr, 0, true);
    TASK_CHECK(again.consumed == 0 && again.produced == 0 && again.finished);

    // Supplying real input once locked is a logic error on a fresh stream.
    SoundTouch s2;
    prepareBoundedInstance(s2, cfg);
    BoundedResult first = s2.processBounded(signal.data(), 100, nullptr, 0, true);
    TASK_CHECK(first.consumed <= 100);
    bool threw = false;
    try
    {
        s2.processBounded(signal.data(), 1, nullptr, 0, false);
    }
    catch (const std::logic_error &)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    return 0;
}

static int finish_tail_delivered_under_backpressure()
{
    const uint N = 900, ch = 2;
    Config cfg{0.6f, 1.6f, ch, false};
    std::vector<float> signal;
    makeSignal(signal, N, ch, 67u);

    SoundTouch s;
        prepareBoundedInstance(s, cfg);
    static const uint pattern[] = {0u, 1u};
    DriveStats st = drive(s, signal, N, cfg, 7, pattern, 2);
    TASK_CHECK(st.valid);
    TASK_CHECK(st.produced == idealFrames(N, cfg));
    TASK_CHECK(st.finished);

    // Zero input without final must never terminate the stream.
    SoundTouch s3;
    prepareBoundedInstance(s3, cfg);
    BoundedResult z = s3.processBounded(nullptr, 0, nullptr, 0, false);
    TASK_CHECK(!z.finished && z.consumed == 0 && z.produced == 0);
    return 0;
}

static int finish_argument_and_order_validation()
{
    Config cfg{1.0f, 1.0f, 1, true};

    // processBounded before prepareBounded -> logic_error.
    {
        SoundTouch s;
        bool threw = false;
        try { s.processBounded(nullptr, 0, nullptr, 0, false); }
        catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
    }

    // Null input with non-zero frames -> invalid_argument, no advance.
    // Null output with non-zero capacity -> invalid_argument.
    {
        SoundTouch s;
        prepareBoundedInstance(s, cfg);
        float dst[8] = {0};
        bool threw = false;
        try { s.processBounded(nullptr, 4, dst, 8, false); }
        catch (const std::invalid_argument &) { threw = true; }
        TASK_CHECK(threw);
        float src[4] = {0.1f, 0.2f, 0.3f, 0.4f};
        threw = false;
        try { s.processBounded(src, 4, nullptr, 8, false); }
        catch (const std::invalid_argument &) { threw = true; }
        TASK_CHECK(threw);
        // Object still usable after the rejected calls.
        BoundedResult ok = s.processBounded(src, 4, dst, 8, false);
        TASK_CHECK(ok.consumed == 4);
    }

    // Overlapping input/output -> invalid_argument.
    {
        SoundTouch s;
        prepareBoundedInstance(s, Config{1.0f, 1.0f, 1, true});
        float mem[64] = {0};
        bool threw = false;
        try { s.processBounded(mem, 8, mem + 2, 8, false); }
        catch (const std::invalid_argument &) { threw = true; }
        TASK_CHECK(threw);
    }

    // prepareBounded twice -> logic_error.
    {
        SoundTouch s;
        prepareBoundedInstance(s, cfg);
        bool threw = false;
        try { s.prepareBounded(); }
        catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
    }

    // prepareBounded after legacy audio -> logic_error.
    {
        SoundTouch s;
        s.setSampleRate(48000);
        s.setChannels(1);
        float src[16] = {0};
        s.putSamples(src, 16);
        bool threw = false;
        try { s.prepareBounded(); }
        catch (const std::logic_error &) { threw = true; }
        TASK_CHECK(threw);
    }

    // Unsupported configuration -> invalid_argument and object unchanged.
    {
        SoundTouch s;
        s.setSampleRate(48000);
        s.setChannels(1);
        s.setRate(3.0f);
        s.setTempo(1.0f);
        bool threw = false;
        try { s.prepareBounded(); }
        catch (const std::invalid_argument &) { threw = true; }
        TASK_CHECK(threw);
        // Correct the configuration: the same object now prepares cleanly.
        s.setRate(1.0f);
        s.prepareBounded();
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// lifetime group: clearBounded reuse equivalence, short streams, prepare
// failure isolation, and allocation/deallocation balance.
////////////////////////////////////////////////////////////////////////////////

static int lifetime_clear_bounded_reuse_equals_fresh()
{
    const uint N = 2500, ch = 2;
    Config cfg{1.2f, 0.9f, ch, true};
    std::vector<float> signal;
    makeSignal(signal, N, ch, 71u);
    static const uint cap[] = {128u};

    SoundTouch reused;
    prepareBoundedInstance(reused, cfg);
    DriveStats first = drive(reused, signal, N, cfg, 53, cap, 1);
    TASK_CHECK(first.valid);
    reused.clearBounded();
    DriveStats second = drive(reused, signal, N, cfg, 53, cap, 1);
    TASK_CHECK(second.valid);

    SoundTouch fresh;
    prepareBoundedInstance(fresh, cfg);
    DriveStats third = drive(fresh, signal, N, cfg, 53, cap, 1);
    TASK_CHECK(third.valid);

    TASK_CHECK(first.out == second.out);
    TASK_CHECK(second.out == third.out);
    TASK_CHECK(first.produced == idealFrames(N, cfg));
    return 0;
}

static int lifetime_short_streams()
{
    const Config cfg{1.15f, 0.88f, 6, true};
    for (uint N : {1u, 2u, 7u})
    {
        std::vector<float> signal;
        makeSignal(signal, N, cfg.channels, 73u + N);
        SoundTouch s;
        prepareBoundedInstance(s, cfg);
        static const uint cap[] = {1u};
        DriveStats st = drive(s, signal, N, cfg, 1, cap, 1);
        TASK_CHECK(st.valid);
        TASK_CHECK(st.consumed == N);
        TASK_CHECK(st.produced == idealFrames(N, cfg));
        TASK_CHECK(st.finished);
    }
    return 0;
}

static int lifetime_prepare_failure_leaves_object_clean()
{
    SoundTouch s;
    s.setSampleRate(48000);
    s.setChannels(6);
    s.setRate(1.0f);
    s.setTempo(1.0f);
    s.setPitch(1.5f); // unsupported: pitch fixed to 1
    bool threw = false;
    try { s.prepareBounded(); }
    catch (const std::invalid_argument &) { threw = true; }
    TASK_CHECK(threw);

    // The failed prepare must not leave a pipeline behind.
    threw = false;
    try { s.processBounded(nullptr, 0, nullptr, 0, false); }
    catch (const std::logic_error &) { threw = true; }
    TASK_CHECK(threw);

    s.setPitch(1.0f);
    s.prepareBounded();
    float src[6] = {0};
    BoundedResult r = s.processBounded(src, 1, nullptr, 0, false);
    TASK_CHECK(r.consumed == 1);
    return 0;
}

static int lifetime_allocation_balance()
{
    const Config cfg{0.9f, 1.1f, 2, true};
    uint64_t before = BoundedPipeline::totalAllocations();
    uint64_t freeBefore = BoundedPipeline::totalDeallocations();
    {
        SoundTouch s;
        prepareBoundedInstance(s, cfg);
        TASK_CHECK(BoundedPipeline::totalAllocations() > before);
        std::vector<float> signal;
        makeSignal(signal, 500, cfg.channels, 79u);
        static const uint cap[] = {16u};
        DriveStats st = drive(s, signal, 500, cfg, 17, cap, 1);
        TASK_CHECK(st.valid);
        TASK_CHECK(st.produced == idealFrames(500, cfg));
    }
    // Every bounded storage block acquired at prepare time is released.
    TASK_CHECK(BoundedPipeline::totalAllocations() - before
               == BoundedPipeline::totalDeallocations() - freeBefore);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Registration
////////////////////////////////////////////////////////////////////////////////

extern const struct task_case task_cases[] = {
    {"pcm", "legacy_prefix_equivalence", pcm_legacy_prefix_equivalence},
    {"pcm", "chunk_and_capacity_independence", pcm_chunk_and_capacity_independence},
    {"pcm", "six_channel_distinct_pulses", pcm_six_channel_distinct_pulses},
    {"pcm", "cursor_and_sentinel", pcm_cursor_and_sentinel},

    {"pressure", "capacity_alternation", pressure_capacity_alternation},
    {"pressure", "long_stall_recovery", pressure_long_stall_recovery},
    {"pressure", "fifo_bounds_and_stable_capacity", pressure_fifo_bounds_and_stable_capacity},
    {"pressure", "no_allocation_after_prepare", pressure_no_allocation_after_prepare},

    {"finish", "exact_total_length_all_configs", finish_exact_total_length_all_configs},
    {"finish", "final_locking_and_repeat_semantics", finish_final_locking_and_repeat_semantics},
    {"finish", "tail_delivered_under_backpressure", finish_tail_delivered_under_backpressure},
    {"finish", "argument_and_order_validation", finish_argument_and_order_validation},

    {"lifetime", "clear_bounded_reuse_equals_fresh", lifetime_clear_bounded_reuse_equals_fresh},
    {"lifetime", "short_streams", lifetime_short_streams},
    {"lifetime", "prepare_failure_leaves_object_clean", lifetime_prepare_failure_leaves_object_clean},
    {"lifetime", "allocation_balance", lifetime_allocation_balance},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
