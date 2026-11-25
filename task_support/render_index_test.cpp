#include "task_support/check.h"
#include "rubberband/R2RenderIndex.h"
#include "rubberband/RubberBandStretcher.h"
#include "task_support/fft_observer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

using S = RubberBand::RubberBandStretcher;
using I = RubberBand::R2RenderIndex;

namespace
{

struct Source {
    std::vector<std::vector<float>> ch;
    size_t frames = 0;
    std::atomic<size_t> reads{0};

    Source() = default;
    Source(const Source &o) : ch(o.ch), frames(o.frames), reads(0) { }
    Source &operator=(const Source &o) {
        if (this != &o) {
            ch = o.ch;
            frames = o.frames;
            reads.store(0);
        }
        return *this;
    }
};

bool
readAt(void *ctx, size_t first, size_t count, float *const *out)
{
    Source &s = *static_cast<Source *>(ctx);
    if (first > s.frames || count > s.frames - first) return false;
    for (size_t c = 0; c < s.ch.size(); ++c) {
        std::copy_n(s.ch[c].data() + first, count, out[c]);
    }
    s.reads.fetch_add(count, std::memory_order_relaxed);
    return true;
}

bool
failRead(void *, size_t, size_t, float *const *)
{
    return false;
}

// Sequential, whole-track R2 reference for the fixed configuration.
std::vector<std::vector<float>>
sequentialReference(const Source &src, double ratio,
                    const std::map<size_t, size_t> &keyFrames)
{
    const size_t channels = src.ch.size();
    S s(48000, channels,
        S::OptionEngineFaster | S::OptionThreadingNever |
        S::OptionPhaseIndependent,
        ratio, 1.0);
    s.setExpectedInputDuration(src.frames);
    s.setMaxProcessSize(4096);
    s.setKeyFrameMap(keyFrames);

    std::vector<const float *> in(channels);
    for (size_t c = 0; c < channels; ++c) in[c] = src.ch[c].data();
    s.study(in.data(), src.frames, true);

    std::vector<std::vector<float>> out(channels);
    for (size_t pos = 0; pos < src.frames;) {
        size_t k = std::min<size_t>(4096, src.frames - pos);
        for (size_t c = 0; c < channels; ++c) in[c] = src.ch[c].data() + pos;
        s.process(in.data(), k, pos + k == src.frames);
        pos += k;
        while (s.available() > 0) {
            std::vector<std::vector<float>> planeBuf(
                channels, std::vector<float>(s.available()));
            std::vector<float *> dst(channels);
            for (size_t c = 0; c < channels; ++c) dst[c] = planeBuf[c].data();
            size_t got = s.retrieve(dst.data(), planeBuf[0].size());
            for (size_t c = 0; c < channels; ++c) {
                out[c].insert(out[c].end(), planeBuf[c].begin(),
                              planeBuf[c].begin() + got);
            }
        }
    }
    return out;
}

// Stereo material with a long silence gap (forces silence-driven
// phase resets) and irregular, aperiodic broadband transients, some
// of which land inside rather than on analysis windows.
Source
makeStereoTrack(size_t n)
{
    Source src;
    src.frames = n;
    src.ch.resize(2);
    for (size_t c = 0; c < 2; ++c) src.ch[c].resize(n);
    // silence gap long enough for >= window/increment silent chunks
    const size_t silenceStart = 9000, silenceEnd = 14000;
    // irregular transient positions, deliberately not hop-aligned
    const size_t bursts[] = {371, 2203, 5129, 16577, 19993};
    for (size_t c = 0; c < 2; ++c) {
        for (size_t i = 0; i < n; ++i) {
            double tone = 0.18 * std::sin(0.0413 * i + c) +
                          0.09 * std::cos(0.0177 * i * (1.0 + 0.3 * c));
            bool silent = (i >= silenceStart && i < silenceEnd);
            double v = silent ? 0.0 : tone;
            for (size_t b : bursts) {
                if (i >= b && i < b + 13) {
                    double env = 1.0 - double(i - b) / 13.0;
                    v += 0.35 * env * ((i % 3 == c) ? 1.0 : -0.8);
                }
            }
            src.ch[c][i] = float(v);
        }
    }
    return src;
}

Source
makeMonoTrack(size_t n, unsigned seed)
{
    Source src;
    src.frames = n;
    src.ch.resize(1);
    src.ch[0].resize(n);
    for (size_t i = 0; i < n; ++i) {
        double v = 0.2 * std::sin(0.0576 * i + seed) +
                   0.11 * std::cos(0.0331 * i * (1.0 + seed * 0.01));
        if ((i + seed * 131) % 4000 < 16) v += 0.3;
        src.ch[0][i] = float(v);
    }
    return src;
}

bool
renderRange(const I &idx, Source &src, size_t first, size_t count,
            std::vector<std::vector<float>> &out)
{
    std::vector<float *> dst(out.size());
    for (size_t c = 0; c < out.size(); ++c) {
        out[c].assign(count, 0.f);
        dst[c] = out[c].data();
    }
    return idx.render(first, count, readAt, &src, dst.data());
}

bool
matchesReference(const std::vector<std::vector<float>> &ref,
                 const std::vector<std::vector<float>> &got,
                 size_t first, double tolerance = 2e-5)
{
    if (got.empty() || got.size() != ref.size()) return false;
    size_t count = got[0].size();
    if (count == 0 || first + count > ref[0].size()) return false;
    for (size_t c = 0; c < ref.size(); ++c) {
        if (got[c].size() != count) return false;
        for (size_t i = 0; i < count; ++i) {
            if (!std::isfinite(got[c][i])) return false;
            if (std::fabs(got[c][i] - ref[c][first + i]) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

const std::map<size_t, size_t> &
smallKeyFrames(size_t n, size_t out)
{
    static const std::map<size_t, size_t> m = {
        {0, 0}, {12000, 8000}, {24000, 30000}
    };
    (void)n;
    (void)out;
    return m;
}

} // namespace

// -------------------------------------------------------------------
// phase group: phase continuity, silence resets, transients,
// per-track acoustic state and sparse boundary state scaling.
// -------------------------------------------------------------------

static int
phase_stereo_matches_sequential()
{
    Source src = makeStereoTrack(24000);
    auto key = smallKeyFrames(24000, 30000);
    auto idx = I::build(48000, 2, 24000, 1.25, 4096, key, readAt, &src);
    TASK_CHECK(idx != nullptr);
    TASK_CHECK(idx->getInputFrames() == 24000);
    TASK_CHECK(idx->getOutputFrames() == 30000);

    auto ref = sequentialReference(src, 1.25, key);
    TASK_CHECK(ref.size() == 2);
    TASK_CHECK(ref[0].size() == 30000);

    std::vector<std::vector<float>> got(2);
    TASK_CHECK(renderRange(*idx, src, 0, 30000, got));
    TASK_CHECK(matchesReference(ref, got, 0));

    // Split across the silence reset and a transient region.
    std::vector<std::vector<float>> a(2), b(2), c(2);
    TASK_CHECK(renderRange(*idx, src, 0, 9000, a));
    TASK_CHECK(renderRange(*idx, src, 9000, 12000, b));
    TASK_CHECK(renderRange(*idx, src, 21000, 9000, c));
    TASK_CHECK(matchesReference(ref, a, 0));
    TASK_CHECK(matchesReference(ref, b, 9000));
    TASK_CHECK(matchesReference(ref, c, 21000));
    return 0;
}

static int
phase_overlapping_windows_agree()
{
    Source src = makeStereoTrack(24000);
    auto key = smallKeyFrames(24000, 30000);
    auto idx = I::build(48000, 2, 24000, 1.25, 4096, key, readAt, &src);
    TASK_CHECK(idx != nullptr);
    auto ref = sequentialReference(src, 1.25, key);

    // Two overlapping renders of the same region must agree sample
    // for sample, proving boundary warm-up converges exactly.
    std::vector<std::vector<float>> a(2), b(2);
    TASK_CHECK(renderRange(*idx, src, 5000, 7000, a));
    TASK_CHECK(renderRange(*idx, src, 6000, 7000, b));
    for (size_t c = 0; c < 2; ++c) {
        for (size_t i = 0; i < 6000; ++i) {
            TASK_CHECK(a[c][1000 + i] == b[c][i]);
        }
    }
    TASK_CHECK(matchesReference(ref, a, 5000));
    TASK_CHECK(matchesReference(ref, b, 6000));
    return 0;
}

static int
phase_boundary_state_scales_with_stride()
{
    Source src = makeStereoTrack(24000);
    auto key = smallKeyFrames(24000, 30000);
    auto small = I::build(48000, 2, 24000, 1.25, 4096, key, readAt, &src);
    auto large = I::build(48000, 2, 24000, 1.25, 65536, key, readAt, &src);
    TASK_CHECK(small != nullptr && large != nullptr);

    std::ostringstream a(std::ios::binary), b(std::ios::binary);
    small->save(a);
    large->save(b);
    // Both keep the full hop schedule; the sparse boundary states are
    // fewer with the larger stride, so the index is smaller.
    TASK_CHECK(a.str().size() > b.str().size());
    TASK_CHECK(b.str().size() > 0);

    // Both configurations must still reproduce the reference.
    auto ref = sequentialReference(src, 1.25, key);
    std::vector<std::vector<float>> got(2);
    TASK_CHECK(renderRange(*large, src, 0, 30000, got));
    TASK_CHECK(matchesReference(ref, got, 0));
    return 0;
}

static int
phase_distinct_tracks_keep_own_state()
{
    // Same source/output key-frame coordinates, two acoustically
    // different tracks: each index must carry its own phase state,
    // and neither output may equal the other track's rendering.
    Source x = makeMonoTrack(24000, 1);
    Source y = makeMonoTrack(24000, 7);
    auto key = smallKeyFrames(24000, 30000);
    auto ix = I::build(48000, 1, 24000, 1.25, 4096, key, readAt, &x);
    auto iy = I::build(48000, 1, 24000, 1.25, 4096, key, readAt, &y);
    TASK_CHECK(ix != nullptr && iy != nullptr);

    auto rx = sequentialReference(x, 1.25, key);
    auto ry = sequentialReference(y, 1.25, key);
    std::vector<std::vector<float>> ox(1), oy(1);
    TASK_CHECK(renderRange(*ix, x, 0, 30000, ox));
    TASK_CHECK(renderRange(*iy, y, 0, 30000, oy));
    TASK_CHECK(matchesReference(rx, ox, 0));
    TASK_CHECK(matchesReference(ry, oy, 0));

    size_t differing = 0;
    for (size_t i = 0; i < 30000; ++i) {
        if (ox[0][i] != oy[0][i]) ++differing;
    }
    TASK_CHECK(differing > 1000);
    return 0;
}

// -------------------------------------------------------------------
// ranges group: tiny reads, out-of-order/overlapping intervals,
// edge trimming, zero requests, failure semantics.
// -------------------------------------------------------------------

static int
ranges_tiny_one_seven_113()
{
    Source src = makeMonoTrack(24000, 3);
    auto key = smallKeyFrames(24000, 30000);
    auto idx = I::build(48000, 1, 24000, 1.25, 4096, key, readAt, &src);
    TASK_CHECK(idx != nullptr);
    auto ref = sequentialReference(src, 1.25, key);

    const size_t counts[] = {1, 7, 113};
    const size_t starts[] = {0, 1, 29999, 15000, 12345};
    for (size_t cnt : counts) {
        for (size_t st : starts) {
            if (st + cnt > 30000) continue;
            std::vector<std::vector<float>> got(1);
            TASK_CHECK(renderRange(*idx, src, st, cnt, got));
            TASK_CHECK(got[0].size() == cnt);
            TASK_CHECK(matchesReference(ref, got, st));
        }
    }
    return 0;
}

static int
ranges_out_of_order_and_overlap()
{
    Source src = makeMonoTrack(24000, 4);
    auto key = smallKeyFrames(24000, 30000);
    auto idx = I::build(48000, 1, 24000, 1.25, 4096, key, readAt, &src);
    TASK_CHECK(idx != nullptr);
    auto ref = sequentialReference(src, 1.25, key);

    struct Req { size_t first, count; };
    const Req reqs[] = {
        {16000, 8000}, {0, 8000}, {8000, 8000}, {24000, 6000},
        {4000, 12000}, {20000, 5000}, {7000, 333}
    };
    for (const Req &r : reqs) {
        std::vector<std::vector<float>> got(1);
        TASK_CHECK(renderRange(*idx, src, r.first, r.count, got));
        TASK_CHECK(matchesReference(ref, got, r.first));
    }

    // Repeated renders must be deterministic and independent.
    std::vector<std::vector<float>> a(1), b(1);
    TASK_CHECK(renderRange(*idx, src, 123, 4567, a));
    TASK_CHECK(renderRange(*idx, src, 123, 4567, b));
    TASK_CHECK(a[0] == b[0]);
    return 0;
}

static int
ranges_edge_trimming_and_length()
{
    Source src = makeStereoTrack(24000);
    auto key = smallKeyFrames(24000, 30000);
    auto idx = I::build(48000, 2, 24000, 1.25, 4096, key, readAt, &src);
    TASK_CHECK(idx != nullptr);
    auto ref = sequentialReference(src, 1.25, key);

    // Head and tail trimming: the very first and last output frames
    // must match the sequential engine exactly.
    std::vector<std::vector<float>> head(2), tail(2);
    TASK_CHECK(renderRange(*idx, src, 0, 64, head));
    TASK_CHECK(renderRange(*idx, src, 30000 - 64, 64, tail));
    TASK_CHECK(matchesReference(ref, head, 0));
    TASK_CHECK(matchesReference(ref, tail, 30000 - 64));

    // A key-frame bend landing inside a window.
    std::vector<std::vector<float>> bend(2);
    TASK_CHECK(renderRange(*idx, src, 7900, 200, bend));
    TASK_CHECK(matchesReference(ref, bend, 7900));
    return 0;
}

static int
ranges_zero_and_failures_leave_output_untouched()
{
    Source src = makeMonoTrack(24000, 5);
    auto key = smallKeyFrames(24000, 30000);
    auto idx = I::build(48000, 1, 24000, 1.25, 4096, key, readAt, &src);
    TASK_CHECK(idx != nullptr);

    // Legal zero-length request: no reads, no writes, success.
    std::vector<float> z(8, 7.f);
    float *zp = z.data();
    size_t before = src.reads.load();
    TASK_CHECK(idx->render(1234, 0, readAt, &src, &zp));
    TASK_CHECK(src.reads.load() == before);
    for (float v : z) TASK_CHECK(v == 7.f);
    TASK_CHECK(idx->render(30000, 0, readAt, &src, &zp));

    // Out of bounds, failing reader and null reader: false and the
    // destination buffer is left completely untouched.
    std::vector<float> guard(32, 19.f);
    float *gp = guard.data();
    TASK_CHECK(!idx->render(30001, 7, readAt, &src, &gp));
    TASK_CHECK(!idx->render(29999, 2, failRead, nullptr, &gp));
    TASK_CHECK(!idx->render(0, 8, nullptr, nullptr, &gp));
    for (float v : guard) TASK_CHECK(v == 19.f);
    return 0;
}

// -------------------------------------------------------------------
// process group: serialisation round trips, corruption rejection,
// parallel rendering, reader budget and index size.
// -------------------------------------------------------------------

static int
process_save_load_roundtrip()
{
    Source src = makeStereoTrack(24000);
    auto key = smallKeyFrames(24000, 30000);
    auto idx = I::build(48000, 2, 24000, 1.25, 4096, key, readAt, &src);
    TASK_CHECK(idx != nullptr);
    auto ref = sequentialReference(src, 1.25, key);

    std::ostringstream bytes(std::ios::binary);
    idx->save(bytes);
    std::string blob = bytes.str();
    TASK_CHECK(!blob.empty());

    // Simulate a fresh process: drop the original index, reload from
    // the stream and render with brand-new reader state.
    idx.reset();
    std::istringstream input(blob, std::ios::binary);
    auto loaded = I::load(input);
    TASK_CHECK(loaded != nullptr);
    TASK_CHECK(loaded->getInputFrames() == 24000);
    TASK_CHECK(loaded->getOutputFrames() == 30000);

    Source src2 = src; // same material, independent object/context
    std::vector<std::vector<float>> got(2);
    TASK_CHECK(renderRange(*loaded, src2, 3333, 9999, got));
    TASK_CHECK(matchesReference(ref, got, 3333));
    return 0;
}

static int
process_rejects_corrupt_and_truncated()
{
    Source src = makeMonoTrack(24000, 6);
    auto key = smallKeyFrames(24000, 30000);
    auto idx = I::build(48000, 1, 24000, 1.25, 4096, key, readAt, &src);
    TASK_CHECK(idx != nullptr);
    std::ostringstream bytes(std::ios::binary);
    idx->save(bytes);
    std::string blob = bytes.str();

    // Truncation at several points.
    for (size_t cut : {size_t(8), blob.size() / 2, blob.size() - 1,
                       blob.size() - 8}) {
        std::istringstream is(std::string(blob.data(), cut),
                              std::ios::binary);
        TASK_CHECK(I::load(is) == nullptr);
    }

    // Corrupted magic, length field and a payload byte each fail.
    std::string badMagic = blob;
    badMagic[0] ^= 0xff;
    std::istringstream m(badMagic, std::ios::binary);
    TASK_CHECK(I::load(m) == nullptr);

    std::string badLen = blob;
    badLen[40] ^= 0x01;
    std::istringstream l(badLen, std::ios::binary);
    TASK_CHECK(I::load(l) == nullptr);

    std::string badPayload = blob;
    badPayload[blob.size() / 2] ^= 0x5a;
    std::istringstream p(badPayload, std::ios::binary);
    TASK_CHECK(I::load(p) == nullptr);

    // Trailing garbage is rejected.
    std::string withGarbage = blob;
    withGarbage.push_back('x');
    std::istringstream g(withGarbage, std::ios::binary);
    TASK_CHECK(I::load(g) == nullptr);

    // Empty stream fails.
    std::istringstream empty(std::ios::binary);
    TASK_CHECK(I::load(empty) == nullptr);
    return 0;
}

static int
process_parallel_renders_are_isolated()
{
    Source src = makeStereoTrack(24000);
    auto key = smallKeyFrames(24000, 30000);
    auto idx = I::build(48000, 2, 24000, 1.25, 4096, key, readAt, &src);
    TASK_CHECK(idx != nullptr);
    auto ref = sequentialReference(src, 1.25, key);

    struct Job { size_t first, count; };
    const Job jobs[] = {
        {0, 4000}, {4000, 4000}, {8000, 4000}, {12000, 4000},
        {16000, 4000}, {20000, 4000}, {24000, 4000}, {26000, 4000}
    };
    std::vector<std::vector<std::vector<float>>> outs(8);
    std::vector<Source> readers(8);
    for (auto &r : readers) r = src;

    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t]() {
            outs[t].resize(2);
            if (!renderRange(*idx, readers[t], jobs[t].first,
                             jobs[t].count, outs[t])) {
                failures.fetch_add(1);
            }
        });
    }
    for (auto &th : threads) th.join();
    TASK_CHECK(failures.load() == 0);
    for (int t = 0; t < 8; ++t) {
        TASK_CHECK(matchesReference(ref, outs[t], jobs[t].first));
    }
    return 0;
}

static int
process_reader_budget_and_index_growth()
{
    // At least 2^20 source frames, request far into the track.
    const size_t n = 1100000;
    const size_t out = (size_t)llround(double(n) * 1.25);
    std::map<size_t, size_t> key{
        {0, 0},
        {n / 3, llround(double(n / 3) * 0.8)},
        {2 * n / 3, llround(double(n / 3) * 0.8) +
                     llround(double(n / 3) * 1.6)},
        {n, out}
    };
    TASK_CHECK(n >= (1u << 20));

    Source src = makeMonoTrack(n, 9);
    auto idx = I::build(48000, 1, n, 1.25, 4096, key, readAt, &src);
    TASK_CHECK(idx != nullptr);
    TASK_CHECK(idx->getOutputFrames() == out);

    const size_t first = out - 60000;
    const size_t count = 32768;
    Source worker = src;
    std::vector<std::vector<float>> got(1);
    TASK_CHECK(renderRange(*idx, worker, first, count, got));
    size_t budget = 32 * (4096 + count + 8192);
    TASK_CHECK(worker.reads.load() <= budget);
    TASK_CHECK(worker.reads.load() > 0);
    // Work must not scale with the position: a request near the start
    // of comparable size reads a comparable amount.
    Source early = src;
    std::vector<std::vector<float>> earlyOut(1);
    TASK_CHECK(renderRange(*idx, early, 4096, count, earlyOut));
    TASK_CHECK(early.reads.load() <= budget);

    // Larger stride -> strictly smaller serialised index, while both
    // remain bounded by hop count, channels and boundary count.
    auto wide = I::build(48000, 1, n, 1.25, 65536, key, readAt, &src);
    TASK_CHECK(wide != nullptr);
    std::ostringstream a(std::ios::binary), b(std::ios::binary);
    idx->save(a);
    wide->save(b);
    TASK_CHECK(a.str().size() > b.str().size());

    // Spot-check the late render against a sequential reference over
    // a smaller, equivalent-curve track is covered elsewhere; here at
    // least ensure finite, bounded output of exactly count frames.
    TASK_CHECK(got[0].size() == count);
    for (float v : got[0]) TASK_CHECK(std::isfinite(v) && std::fabs(v) < 8.f);
    return 0;
}

static int
process_build_uses_no_inverse_fft()
{
    size_t inverseBefore = rb_task_fft_inverse_count();
    Source src = makeMonoTrack(24000, 2);
    auto key = smallKeyFrames(24000, 30000);
    auto idx = I::build(48000, 1, 24000, 1.25, 4096, key, readAt, &src);
    TASK_CHECK(idx != nullptr);
    // Analysis performs forward FFTs only; no synthesis PCM.
    TASK_CHECK(rb_task_fft_inverse_count() == inverseBefore);
    TASK_CHECK(src.reads.load() > 0);

    // Rendering does perform inverse FFTs, as real synthesis must.
    std::vector<std::vector<float>> got(1);
    TASK_CHECK(renderRange(*idx, src, 0, 1024, got));
    TASK_CHECK(rb_task_fft_inverse_count() > inverseBefore);
    return 0;
}

extern const struct task_case task_cases[] = {
    {"phase", "stereo_silence_reset_and_transients",
     phase_stereo_matches_sequential},
    {"phase", "overlapping_window_phase_continuity",
     phase_overlapping_windows_agree},
    {"phase", "boundary_state_scales_with_stride",
     phase_boundary_state_scales_with_stride},
    {"phase", "distinct_tracks_keep_distinct_phase_state",
     phase_distinct_tracks_keep_own_state},

    {"ranges", "tiny_reads_1_7_113", ranges_tiny_one_seven_113},
    {"ranges", "out_of_order_and_overlapping", ranges_out_of_order_and_overlap},
    {"ranges", "head_tail_trimming_and_bend", ranges_edge_trimming_and_length},
    {"ranges", "zero_request_and_failure_semantics",
     ranges_zero_and_failures_leave_output_untouched},

    {"process", "save_load_roundtrip", process_save_load_roundtrip},
    {"process", "reject_corrupt_truncated_index",
     process_rejects_corrupt_and_truncated},
    {"process", "parallel_renders_are_isolated",
     process_parallel_renders_are_isolated},
    {"process", "reader_budget_and_index_sizes",
     process_reader_budget_and_index_growth},
    {"process", "analysis_has_no_inverse_fft",
     process_build_uses_no_inverse_fft},
};

extern const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
