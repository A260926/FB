// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

//
// AudioRenderer regression tests.
//
// The fixtures (short PCM16 WAV files carrying an unknown, odd-length chunk
// and real JSON timelines) are generated on the fly into TMPDIR, so the tests
// never depend on checked-in media.
//

#include "task_support/check.h"

#include "opentimelineio/audioRender.h"
#include "opentimelineio/clip.h"
#include "opentimelineio/externalReference.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/linearTimeWarp.h"
#include "opentimelineio/pcmWave.h"
#include "opentimelineio/serializableObject.h"
#include "opentimelineio/stack.h"
#include "opentimelineio/timeline.h"
#include "opentimelineio/track.h"
#include "opentimelineio/transition.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace o = opentimelineio::OPENTIMELINEIO_VERSION_NS;

namespace {

using o::RationalTime;
using o::TimeRange;

constexpr double k_pi = 3.14159265358979323846;

std::string
work_dir()
{
    const char* tmp = std::getenv("TMPDIR");
    std::string root = (tmp && *tmp) ? tmp : "/tmp";
    std::string dir = root + "/audio_render_test_" + std::to_string(getpid());
    ::mkdir(dir.c_str(), 0755);
    return dir;
}

const std::string g_dir = work_dir();

void
put_u32(std::ostream& f, uint32_t v)
{
    char b[4] = {
        char(v & 0xff), char((v >> 8) & 0xff), char((v >> 16) & 0xff),
        char((v >> 24) & 0xff)
    };
    f.write(b, 4);
}

void
put_u16(std::ostream& f, uint16_t v)
{
    char b[2] = { char(v & 0xff), char((v >> 8) & 0xff) };
    f.write(b, 2);
}

// Write a PCM16 WAV that deliberately contains an unknown, odd-sized chunk
// (with its mandatory even-byte pad) between fmt and data, so the parser
// cannot assume a 44-byte header.
bool
write_wav(
    std::string const&           path,
    int                          rate,
    int                          channels,
    std::vector<int16_t> const&  interleaved)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        return false;
    }
    uint32_t data_bytes = uint32_t(interleaved.size() * 2);
    uint32_t junk_bytes = 3; // intentionally odd
    uint32_t riff_size =
        4u + (8u + 16u) + (8u + junk_bytes + 1u) + (8u + data_bytes);

    f.write("RIFF", 4);
    put_u32(f, riff_size);
    f.write("WAVE", 4);

    f.write("fmt ", 4);
    put_u32(f, 16);
    put_u16(f, 1);                       // PCM
    put_u16(f, uint16_t(channels));
    put_u32(f, uint32_t(rate));
    put_u32(f, uint32_t(rate) * channels * 2);
    put_u16(f, uint16_t(channels * 2));  // block align
    put_u16(f, 16);                      // bits per sample

    f.write("JUNK", 4);
    put_u32(f, junk_bytes);
    f.write("xyz", 3);
    f.put(char(0));                      // even-byte padding

    f.write("data", 4);
    put_u32(f, data_bytes);
    for (int16_t v: interleaved)
    {
        put_u16(f, uint16_t(v));
    }
    f.flush();
    return bool(f);
}

std::string
wav_path(std::string const& name)
{
    return g_dir + "/" + name;
}

float
norm(int16_t v)
{
    return float(v) / 32768.0f;
}

bool
near(float a, float b, double tol = 1e-6)
{
    return std::fabs(double(a) - double(b)) <= tol;
}

TimeRange
tr(double start, double duration, double rate)
{
    return TimeRange(RationalTime(start, rate), RationalTime(duration, rate));
}

// Round-trip a timeline through real JSON and return the parsed copy.
o::SerializableObject::Retainer<o::Timeline>
json_round_trip(o::Timeline* timeline)
{
    o::ErrorStatus e;
    std::string json = timeline->to_json_string(&e);
    if (o::is_error(e))
    {
        return o::SerializableObject::Retainer<o::Timeline>(nullptr);
    }
    o::SerializableObject::Retainer<o::SerializableObject> loaded(
        o::SerializableObject::from_json_string(json, &e));
    if (o::is_error(e))
    {
        return o::SerializableObject::Retainer<o::Timeline>(nullptr);
    }
    return o::SerializableObject::Retainer<o::Timeline>(
        dynamic_cast<o::Timeline*>(loaded.value));
}

// Read the whole render using a fixed block size.
bool
read_all(
    o::AudioRenderer& renderer,
    int64_t           block,
    std::vector<float>& out,
    o::ErrorStatus* e)
{
    out.clear();
    std::vector<float> chunk;
    while (true)
    {
        if (!renderer.read(block, chunk, e))
        {
            return false;
        }
        if (chunk.empty())
        {
            break;
        }
        out.insert(out.end(), chunk.begin(), chunk.end());
        if (int64_t(out.size() / 2) >= renderer.frame_count())
        {
            break;
        }
    }
    return true;
}

// Convenience: build a single-clip mono/stereo timeline and render it.
bool
render_simple(
    std::string const& wav,
    int                source_rate,
    int                channels,
    std::vector<int16_t> const& samples,
    int                output_rate,
    double             clip_start,
    double             clip_duration,
    double             timebase,
    std::vector<float>& out,
    o::ErrorStatus*    e)
{
    if (!write_wav(wav, source_rate, channels, samples))
    {
        return false;
    }
    o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
    auto* track = new o::Track("a", std::nullopt, o::Track::Kind::audio);
    auto* ref = new o::ExternalReference(
        wav, tr(0, double(samples.size() / channels), source_rate));
    auto* clip = new o::Clip(
        "c", ref, tr(clip_start, clip_duration, timebase));
    track->append_child(clip, e);
    if (o::is_error(e))
    {
        return false;
    }
    t->tracks()->append_child(track, e);
    if (o::is_error(e))
    {
        return false;
    }
    auto parsed = json_round_trip(t.value);
    if (!parsed)
    {
        return false;
    }
    o::AudioRenderer renderer(*parsed, g_dir, output_rate, e);
    if (o::is_error(e))
    {
        return false;
    }
    return read_all(renderer, 64, out, e);
}

// ---------------------------------------------------------------------------
// coordinates group
// ---------------------------------------------------------------------------

int
case_mono_duplication()
{
    o::ErrorStatus e;
    std::vector<int16_t> samples = { 10, -20, 30, -40 };
    std::vector<float>   out;
    TASK_CHECK(render_simple(
        wav_path("mono.wav"), 48000, 1, samples, 48000, 0, 4, 48000, out, &e));
    TASK_CHECK(out.size() == size_t(4 * 2));
    for (int i = 0; i < 4; ++i)
    {
        TASK_CHECK(near(out[2 * i], norm(samples[i])));
        TASK_CHECK(near(out[2 * i + 1], norm(samples[i]))); // mono -> both
    }
    return 0;
}

int
case_gap_silence()
{
    o::ErrorStatus e;
    TASK_CHECK(write_wav(
        wav_path("gap.wav"), 48000, 1, std::vector<int16_t>(2, 300)));

    o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
    auto* track = new o::Track("a", std::nullopt, o::Track::Kind::audio);
    track->append_child(new o::Gap(RationalTime(2, 48000)), &e); // leading gap
    track->append_child(
        new o::Clip(
            "c",
            new o::ExternalReference("gap.wav", tr(0, 2, 48000)),
            tr(0, 2, 48000)),
        &e);
    track->append_child(new o::Gap(RationalTime(1, 48000)), &e); // trailing gap
    t->tracks()->append_child(track, &e);
    TASK_CHECK(!o::is_error(e));

    auto parsed = json_round_trip(t.value);
    TASK_CHECK(bool(parsed));
    o::AudioRenderer r(*parsed, g_dir, 48000, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(r.frame_count() == 5);
    std::vector<float> out;
    TASK_CHECK(read_all(r, 2, out, &e));
    for (int n = 0; n < 5; ++n)
    {
        if (n >= 2 && n < 4)
        {
            TASK_CHECK(near(out[2 * n], norm(300)));
        }
        else
        {
            TASK_CHECK(near(out[2 * n], 0.0f)); // gaps are silence
        }
        TASK_CHECK(near(out[2 * n + 1], out[2 * n], 0.0));
    }
    return 0;
}

int
case_half_rate_interpolation()
{
    // 24000 Hz source rendered at 48000 Hz: output frame n lands on source
    // position n/2, so odd output frames sit exactly between two samples.
    o::ErrorStatus e;
    std::vector<int16_t> samples = { 0, 1000, 2000, 3000, 4000 };
    std::vector<float>   out;
    TASK_CHECK(render_simple(
        wav_path("half.wav"), 24000, 1, samples, 48000, 0, 5, 24000, out, &e));
    TASK_CHECK(out.size() == size_t(10 * 2));
    TASK_CHECK(near(out[0], norm(0)));          // source pos 0
    TASK_CHECK(near(out[2], norm(500)));        // midpoint 0/1
    TASK_CHECK(near(out[4], norm(1000)));       // source pos 1
    TASK_CHECK(near(out[6], norm(1500)));       // midpoint 1/2
    TASK_CHECK(near(out[8], norm(2000)));       // source pos 2
    return 0;
}

int
case_44100_to_48000()
{
    o::ErrorStatus e;
    std::vector<int16_t> samples(100);
    for (int i = 0; i < 100; ++i)
    {
        samples[i] = int16_t(i * 100); // linear ramp, so interpolation is exact
    }
    std::vector<float> out;
    TASK_CHECK(render_simple(
        wav_path("r44100.wav"), 44100, 1, samples, 48000, 0, 4, 48000, out,
        &e));
    TASK_CHECK(out.size() == size_t(4 * 2));
    // Output frame 1 -> source position 44100/48000 = 0.91875.
    double pos = 44100.0 / 48000.0;
    double expected = (0.0 + (100.0 - 0.0) * pos) / 32768.0;
    TASK_CHECK(near(out[2], float(expected), 1e-6));
    TASK_CHECK(near(out[3], float(expected), 1e-6));
    // Output frame 3 -> position 2.75625 between samples 2 and 3.
    pos = 3.0 * 44100.0 / 48000.0;
    expected = (200.0 + (300.0 - 200.0) * (pos - 2.0)) / 32768.0;
    TASK_CHECK(near(out[6], float(expected), 1e-6));
    return 0;
}

int
case_mixed_timebases_frame_count()
{
    o::ErrorStatus e;
    // Whole-timeline duration expressed at three different rates; the output
    // frame count is ceil(seconds * R), never rounded per clip.
    struct Spec
    {
        double rate;
        int64_t expected;
    };
    Spec specs[] = {
        { 24.0, int64_t(std::ceil(48000.0 / 24.0)) },     // 2000
        { 25.0, int64_t(std::ceil(48000.0 / 25.0)) },     // 1920
        { 48000.0, 1 }
    };
    for (Spec const& spec: specs)
    {
        o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
        auto* track = new o::Track("a", std::nullopt, o::Track::Kind::audio);
        auto* gap = new o::Gap(RationalTime(1, spec.rate));
        track->append_child(gap, &e);
        TASK_CHECK(!o::is_error(e));
        t->tracks()->append_child(track, &e);
        TASK_CHECK(!o::is_error(e));
        auto parsed = json_round_trip(t.value);
        TASK_CHECK(bool(parsed));
        o::AudioRenderer renderer(*parsed, g_dir, 48000, &e);
        TASK_CHECK(!o::is_error(e));
        TASK_CHECK(renderer.frame_count() == spec.expected);
    }
    return 0;
}

int
case_media_timebases()
{
    // Real media whose clips are trimmed at 24 fps and 25 fps while the file
    // itself is 48 kHz. Fractional source times must map correctly through the
    // parent chain, and the output count follows ceil(seconds * 48000).
    o::ErrorStatus e;
    std::vector<int16_t> ramp(4000);
    for (int i = 0; i < 4000; ++i)
    {
        ramp[i] = int16_t(i);
    }
    TASK_CHECK(write_wav(wav_path("tb.wav"), 48000, 1, ramp));

    auto render_one = [&](double tb, int64_t expected_frames) -> int {
        o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
        auto* track = new o::Track("a", std::nullopt, o::Track::Kind::audio);
        track->append_child(
            new o::Clip(
                "c",
                new o::ExternalReference("tb.wav", tr(0, 4000, 48000)),
                tr(0, 1, tb)),
            &e);
        t->tracks()->append_child(track, &e);
        auto parsed = json_round_trip(t.value);
        TASK_CHECK(bool(parsed));
        o::AudioRenderer r(*parsed, g_dir, 48000, &e);
        TASK_CHECK(!o::is_error(e));
        TASK_CHECK(r.frame_count() == expected_frames);
        std::vector<float> out;
        TASK_CHECK(read_all(r, 37, out, &e));
        // Output frame n lands on source frame n (a linear ramp, so the
        // linear interpolation is exact).
        TASK_CHECK(near(out[0], norm(0)));
        int64_t last = expected_frames - 1;
        TASK_CHECK(near(out[size_t(last) * 2], norm(int16_t(last)), 1e-6));
        return 0;
    };

    TASK_CHECK(render_one(24.0, 2000) == 0); // 1/24 s -> 2000 output frames
    TASK_CHECK(render_one(25.0, 1920) == 0); // 1/25 s -> 1920 output frames
    return 0;
}

int
case_nested_crop()
{
    // An 8-frame clip inside a track inside a Stack cropped to source [2,6):
    // program frame n plays inner source frame n+2.
    o::ErrorStatus e;
    std::vector<int16_t> samples = { 1, 2, 3, 4, 5, 6, 7, 8 };
    TASK_CHECK(write_wav(wav_path("crop.wav"), 48000, 1, samples));

    o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
    auto* track = new o::Track("inner", std::nullopt, o::Track::Kind::audio);
    auto* ref = new o::ExternalReference("crop.wav", tr(0, 8, 48000));
    track->append_child(new o::Clip("c", ref, tr(0, 8, 48000)), &e);
    TASK_CHECK(!o::is_error(e));
    auto* nest = new o::Stack("crop", tr(2, 4, 48000));
    nest->append_child(track, &e);
    TASK_CHECK(!o::is_error(e));
    t->tracks()->append_child(nest, &e);
    TASK_CHECK(!o::is_error(e));

    auto parsed = json_round_trip(t.value);
    TASK_CHECK(bool(parsed));
    o::AudioRenderer renderer(*parsed, g_dir, 48000, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(renderer.frame_count() == 4);
    std::vector<float> out;
    TASK_CHECK(read_all(renderer, 2, out, &e));
    for (int n = 0; n < 4; ++n)
    {
        TASK_CHECK(near(out[2 * n], norm(int16_t(n + 3)))); // source 3,4,5,6
    }
    return 0;
}

int
case_short_media_zero_fill()
{
    // The clip asks for 8 frames but the file only holds 4.
    o::ErrorStatus e;
    std::vector<int16_t> samples = { 11, 22, 33, 44 };
    std::vector<float>   out;
    TASK_CHECK(render_simple(
        wav_path("short.wav"), 48000, 1, samples, 48000, 0, 8, 48000, out,
        &e));
    TASK_CHECK(out.size() == size_t(8 * 2));
    for (int i = 0; i < 4; ++i)
    {
        TASK_CHECK(near(out[2 * i], norm(samples[i])));
    }
    for (int i = 4; i < 8; ++i)
    {
        TASK_CHECK(near(out[2 * i], 0.0f));
        TASK_CHECK(near(out[2 * i + 1], 0.0f));
    }
    return 0;
}

int
case_track_summing_and_video_ignored()
{
    o::ErrorStatus e;
    TASK_CHECK(write_wav(
        wav_path("sum1.wav"), 48000, 1, std::vector<int16_t>(4, 1000)));
    TASK_CHECK(write_wav(
        wav_path("sum2.wav"), 48000, 1, std::vector<int16_t>(4, 2000)));
    TASK_CHECK(write_wav(
        wav_path("sumv.wav"), 48000, 1, std::vector<int16_t>(4, 9000)));

    o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
    auto* a1 = new o::Track("a1", std::nullopt, o::Track::Kind::audio);
    a1->append_child(
        new o::Clip(
            "c1",
            new o::ExternalReference("sum1.wav", tr(0, 4, 48000)),
            tr(0, 4, 48000)),
        &e);
    auto* a2 = new o::Track("a2", std::nullopt, o::Track::Kind::audio);
    a2->append_child(
        new o::Clip(
            "c2",
            new o::ExternalReference("sum2.wav", tr(0, 4, 48000)),
            tr(0, 4, 48000)),
        &e);
    // A video track must be ignored entirely.
    auto* v = new o::Track("v", std::nullopt, o::Track::Kind::video);
    v->append_child(
        new o::Clip(
            "cv",
            new o::ExternalReference("sumv.wav", tr(0, 4, 48000)),
            tr(0, 4, 48000)),
        &e);
    t->tracks()->append_child(a1, &e);
    t->tracks()->append_child(v, &e);
    t->tracks()->append_child(a2, &e);
    TASK_CHECK(!o::is_error(e));

    auto parsed = json_round_trip(t.value);
    TASK_CHECK(bool(parsed));
    o::AudioRenderer r(*parsed, g_dir, 48000, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(r.frame_count() == 4);
    std::vector<float> out;
    TASK_CHECK(read_all(r, 2, out, &e));
    float summed = norm(1000) + norm(2000); // video's 9000 excluded
    for (int i = 0; i < 4; ++i)
    {
        TASK_CHECK(near(out[2 * i], summed, 1e-6));
        TASK_CHECK(near(out[2 * i + 1], summed, 1e-6));
    }
    return 0;
}

int
case_global_start_time_ignored()
{
    o::ErrorStatus e;
    std::vector<int16_t> samples = { 5, 6, 7 };
    TASK_CHECK(write_wav(wav_path("gst.wav"), 48000, 1, samples));

    auto build = [&](std::optional<RationalTime> gst) {
        o::SerializableObject::Retainer<o::Timeline> t(
            new o::Timeline("t", gst));
        auto* track =
            new o::Track("a", std::nullopt, o::Track::Kind::audio);
        auto* ref =
            new o::ExternalReference("gst.wav", tr(0, 3, 48000));
        track->append_child(new o::Clip("c", ref, tr(0, 3, 48000)), &e);
        t->tracks()->append_child(track, &e);
        return t;
    };

    auto with_gst = json_round_trip(build(RationalTime(90000, 24)).value);
    auto without  = json_round_trip(build(std::nullopt).value);
    TASK_CHECK(bool(with_gst) && bool(without));

    o::AudioRenderer a(*with_gst, g_dir, 48000, &e);
    o::AudioRenderer b(*without, g_dir, 48000, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(a.frame_count() == b.frame_count());
    std::vector<float> va, vb;
    TASK_CHECK(read_all(a, 2, va, &e) && read_all(b, 2, vb, &e));
    TASK_CHECK(va.size() == vb.size());
    for (size_t i = 0; i < va.size(); ++i)
    {
        TASK_CHECK(near(va[i], vb[i], 0.0));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// dissolve group
// ---------------------------------------------------------------------------

// Build the canonical 8-frame A/dissolve/B timeline and return its renderer.
bool
build_dissolve(
    o::SerializableObject::Retainer<o::Timeline>& holder,
    std::vector<int16_t> const& a_samples,
    std::vector<int16_t> const& b_samples)
{
    o::ErrorStatus e;
    if (!write_wav(wav_path("a.wav"), 48000, 2, a_samples))
    {
        return false;
    }
    if (!write_wav(wav_path("b.wav"), 48000, 2, b_samples))
    {
        return false;
    }

    o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("mix"));
    auto* track = new o::Track("audio", std::nullopt, o::Track::Kind::audio);
    auto* ref_a =
        new o::ExternalReference("a.wav", tr(0, a_samples.size() / 2, 48000));
    auto* ref_b =
        new o::ExternalReference("b.wav", tr(0, b_samples.size() / 2, 48000));
    auto* clip_a = new o::Clip("A", ref_a, tr(2, 4, 48000));
    auto* clip_b = new o::Clip("B", ref_b, tr(5, 4, 48000));
    auto* dissolve = new o::Transition(
        "xfade", o::Transition::Type::SMPTE_Dissolve,
        RationalTime(1, 48000), RationalTime(1, 48000));
    track->append_child(clip_a, &e);
    track->append_child(dissolve, &e);
    track->append_child(clip_b, &e);
    if (o::is_error(e))
    {
        return false;
    }
    t->tracks()->append_child(track, &e);
    if (o::is_error(e))
    {
        return false;
    }

    holder = json_round_trip(t.value);
    return bool(holder);
}

int
case_dissolve_mix()
{
    o::ErrorStatus e;
    std::vector<int16_t> a(20), b(20);
    for (int i = 0; i < 10; ++i)
    {
        int16_t av = int16_t(100 + i);
        int16_t bv = int16_t(200 + i);
        a[2 * i] = a[2 * i + 1] = av;
        b[2 * i] = b[2 * i + 1] = bv;
    }
    o::SerializableObject::Retainer<o::Timeline> holder;
    TASK_CHECK(build_dissolve(holder, a, b));
    o::AudioRenderer r(*holder, g_dir, 48000, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(r.frame_count() == 8);

    std::vector<float> out;
    TASK_CHECK(read_all(r, 8, out, &e));
    TASK_CHECK(out.size() == size_t(16));

    // Frames 0..2 are A ordinary source frames 2..4.
    TASK_CHECK(near(out[0], norm(102)));
    TASK_CHECK(near(out[2], norm(103)));
    TASK_CHECK(near(out[4], norm(104)));
    // Frame 3: transition progress 0 -> A only, A source frame 5.
    TASK_CHECK(near(out[6], norm(105)));
    TASK_CHECK(near(out[7], norm(105)));
    // Frame 4: progress 1/2, A source 6 (106) and B source 5 (205).
    double mix = std::cos(k_pi / 4.0) * (norm(106) + norm(205));
    TASK_CHECK(near(out[8], float(mix), 1e-6));
    TASK_CHECK(near(out[9], float(mix), 1e-6));
    // Frame 5 is past the half-open transition: B ordinary source frame 6.
    TASK_CHECK(near(out[10], norm(206)));
    TASK_CHECK(near(out[12], norm(207)));
    TASK_CHECK(near(out[14], norm(208)));
    return 0;
}

int
case_dissolve_block_independence()
{
    o::ErrorStatus e;
    std::vector<int16_t> a(20), b(20);
    for (int i = 0; i < 10; ++i)
    {
        a[2 * i] = a[2 * i + 1] = int16_t(10 + i);
        b[2 * i] = b[2 * i + 1] = int16_t(50 + i);
    }
    o::SerializableObject::Retainer<o::Timeline> holder;
    TASK_CHECK(build_dissolve(holder, a, b));

    std::vector<float> reference;
    {
        o::AudioRenderer r(*holder, g_dir, 48000, &e);
        TASK_CHECK(!o::is_error(e));
        TASK_CHECK(read_all(r, 1000, reference, &e)); // one shot
    }
    for (int64_t block: { int64_t(1), int64_t(2), int64_t(3), int64_t(7) })
    {
        o::AudioRenderer r(*holder, g_dir, 48000, &e);
        TASK_CHECK(!o::is_error(e));
        std::vector<float> chunked;
        TASK_CHECK(read_all(r, block, chunked, &e));
        TASK_CHECK(chunked.size() == reference.size());
        for (size_t i = 0; i < reference.size(); ++i)
        {
            TASK_CHECK(near(chunked[i], reference[i], 0.0));
        }
    }
    return 0;
}

int
case_dissolve_nested_progress()
{
    // Crop the dissolve timeline so only part of the [3,5) transition shows;
    // the progress must remain the original, not restart at the crop edge.
    o::ErrorStatus e;
    std::vector<int16_t> a(20), b(20);
    for (int i = 0; i < 10; ++i)
    {
        a[2 * i] = a[2 * i + 1] = int16_t(100 + i);
        b[2 * i] = b[2 * i + 1] = int16_t(200 + i);
    }
    TASK_CHECK(write_wav(wav_path("na.wav"), 48000, 2, a));
    TASK_CHECK(write_wav(wav_path("nb.wav"), 48000, 2, b));

    o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("mix"));
    auto* track = new o::Track("audio", std::nullopt, o::Track::Kind::audio);
    auto* clip_a = new o::Clip(
        "A", new o::ExternalReference("na.wav", tr(0, 10, 48000)),
        tr(2, 4, 48000));
    auto* clip_b = new o::Clip(
        "B", new o::ExternalReference("nb.wav", tr(0, 10, 48000)),
        tr(5, 4, 48000));
    auto* dissolve = new o::Transition(
        "xfade", o::Transition::Type::SMPTE_Dissolve,
        RationalTime(1, 48000), RationalTime(1, 48000));
    track->append_child(clip_a, &e);
    track->append_child(dissolve, &e);
    track->append_child(clip_b, &e);
    auto* nest = new o::Stack("crop", tr(3, 2, 48000)); // inner [3,5)
    nest->append_child(track, &e);
    t->tracks()->append_child(nest, &e);
    TASK_CHECK(!o::is_error(e));

    auto parsed = json_round_trip(t.value);
    TASK_CHECK(bool(parsed));
    o::AudioRenderer r(*parsed, g_dir, 48000, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(r.frame_count() == 2);
    std::vector<float> out;
    TASK_CHECK(read_all(r, 1, out, &e));

    // Program 0 -> inner 3 -> u=0 -> A source 5 (105).
    TASK_CHECK(near(out[0], norm(105)));
    // Program 1 -> inner 4 -> u=1/2 (original progress), A6 + B5.
    double mix = std::cos(k_pi / 4.0) * (norm(106) + norm(205));
    TASK_CHECK(near(out[2], float(mix), 1e-6));
    return 0;
}

int
case_dissolve_errors()
{
    o::ErrorStatus e;

    // Transition with no following clip.
    {
        TASK_CHECK(write_wav(
            wav_path("x.wav"), 48000, 1, std::vector<int16_t>(4, 7)));
        o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
        auto* track =
            new o::Track("a", std::nullopt, o::Track::Kind::audio);
        auto* clip = new o::Clip(
            "c", new o::ExternalReference("x.wav", tr(0, 4, 48000)),
            tr(0, 4, 48000));
        auto* tr = new o::Transition(
            "x", o::Transition::Type::SMPTE_Dissolve,
            RationalTime(1, 48000), RationalTime(1, 48000));
        track->append_child(clip, &e);
        track->append_child(tr, &e);
        t->tracks()->append_child(track, &e);
        auto parsed = json_round_trip(t.value);
        TASK_CHECK(bool(parsed));
        o::AudioRenderer r(*parsed, g_dir, 48000, &e);
        TASK_CHECK(o::is_error(e));
    }

    // Non-dissolve transition type.
    e = o::ErrorStatus();
    {
        TASK_CHECK(write_wav(wav_path("c1.wav"), 48000, 1, { 1, 2, 3, 4 }));
        TASK_CHECK(write_wav(wav_path("c2.wav"), 48000, 1, { 5, 6, 7, 8 }));
        o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
        auto* track =
            new o::Track("a", std::nullopt, o::Track::Kind::audio);
        auto* c1 = new o::Clip(
            "c1", new o::ExternalReference("c1.wav", tr(0, 4, 48000)),
            tr(0, 4, 48000));
        auto* c2 = new o::Clip(
            "c2", new o::ExternalReference("c2.wav", tr(0, 4, 48000)),
            tr(0, 4, 48000));
        auto* tr = new o::Transition(
            "custom", o::Transition::Type::Custom,
            RationalTime(1, 48000), RationalTime(1, 48000));
        track->append_child(c1, &e);
        track->append_child(tr, &e);
        track->append_child(c2, &e);
        t->tracks()->append_child(track, &e);
        auto parsed = json_round_trip(t.value);
        TASK_CHECK(bool(parsed));
        o::AudioRenderer r(*parsed, g_dir, 48000, &e);
        TASK_CHECK(o::is_error(e));
    }

    // Overlapping transition coverage.
    e = o::ErrorStatus();
    {
        o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
        auto* track =
            new o::Track("a", std::nullopt, o::Track::Kind::audio);
        auto mk = [&](std::string const& n) {
            return new o::Clip(
                n, new o::ExternalReference(n + ".wav", tr(0, 8, 48000)),
                tr(0, 8, 48000));
        };
        TASK_CHECK(write_wav(wav_path("p1.wav"), 48000, 1,
                             std::vector<int16_t>(8, 1)));
        TASK_CHECK(write_wav(wav_path("p2.wav"), 48000, 1,
                             std::vector<int16_t>(8, 2)));
        TASK_CHECK(write_wav(wav_path("p3.wav"), 48000, 1,
                             std::vector<int16_t>(8, 3)));
        auto* t1 = new o::Transition(
            "t1", o::Transition::Type::SMPTE_Dissolve,
            RationalTime(1, 48000), RationalTime(5, 48000));
        auto* t2 = new o::Transition(
            "t2", o::Transition::Type::SMPTE_Dissolve,
            RationalTime(4, 48000), RationalTime(1, 48000));
        // t1 covers [7,13), t2 covers [12,17): they overlap on [12,13).
        track->append_child(mk("p1"), &e);
        track->append_child(t1, &e);
        track->append_child(mk("p2"), &e);
        track->append_child(t2, &e);
        track->append_child(mk("p3"), &e);
        t->tracks()->append_child(track, &e);
        auto parsed = json_round_trip(t.value);
        TASK_CHECK(bool(parsed));
        o::AudioRenderer r(*parsed, g_dir, 48000, &e);
        TASK_CHECK(o::is_error(e));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// streaming group
// ---------------------------------------------------------------------------

int
case_seek_and_reread()
{
    o::ErrorStatus e;
    std::vector<int16_t> samples(12);
    for (int i = 0; i < 12; ++i)
    {
        samples[i] = int16_t(i * 7);
    }
    std::vector<float> full;
    TASK_CHECK(render_simple(
        wav_path("seek.wav"), 48000, 1, samples, 48000, 0, 12, 48000, full,
        &e));
    TASK_CHECK(full.size() == size_t(24));

    // Re-render and exercise seek + block reads.
    o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
    auto* track = new o::Track("a", std::nullopt, o::Track::Kind::audio);
    track->append_child(
        new o::Clip(
            "c", new o::ExternalReference("seek.wav", tr(0, 12, 48000)),
            tr(0, 12, 48000)),
        &e);
    t->tracks()->append_child(track, &e);
    auto parsed = json_round_trip(t.value);
    TASK_CHECK(bool(parsed));
    o::AudioRenderer r(*parsed, g_dir, 48000, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(r.frame_count() == 12);

    // Seek to the middle and read to the end.
    TASK_CHECK(r.seek(5, &e));
    TASK_CHECK(r.position() == 5);
    std::vector<float> tail;
    TASK_CHECK(read_all(r, 3, tail, &e));
    TASK_CHECK(tail.size() == size_t(7 * 2));
    for (int i = 0; i < 7; ++i)
    {
        TASK_CHECK(near(tail[2 * i], full[2 * (5 + i)], 0.0));
    }

    // Seek back to zero and confirm identical samples.
    TASK_CHECK(r.seek(0, &e));
    std::vector<float> again;
    TASK_CHECK(read_all(r, 5, again, &e));
    TASK_CHECK(again.size() == full.size());
    for (size_t i = 0; i < full.size(); ++i)
    {
        TASK_CHECK(near(again[i], full[i], 0.0));
    }

    // Seeking to the end is legal; the next read yields zero frames.
    TASK_CHECK(r.seek(12, &e));
    std::vector<float> none;
    TASK_CHECK(r.read(4, none, &e));
    TASK_CHECK(none.empty());

    // Out-of-range seeks fail and leave the position untouched.
    TASK_CHECK(!r.seek(-1, &e));
    TASK_CHECK(r.position() == 12);
    e = o::ErrorStatus();
    TASK_CHECK(!r.seek(13, &e));
    TASK_CHECK(r.position() == 12);
    return 0;
}

int
case_disabled_branch()
{
    o::ErrorStatus e;
    TASK_CHECK(write_wav(
        wav_path("en.wav"), 48000, 1, std::vector<int16_t>(4, 100)));
    TASK_CHECK(write_wav(
        wav_path("dis.wav"), 48000, 1, std::vector<int16_t>(4, 200)));

    o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
    auto* stack = new o::Stack("s");
    auto* enabled_track =
        new o::Track("en", std::nullopt, o::Track::Kind::audio);
    enabled_track->append_child(
        new o::Clip(
            "en", new o::ExternalReference("en.wav", tr(0, 4, 48000)),
            tr(0, 4, 48000)),
        &e);
    auto* disabled_track =
        new o::Track("dis", std::nullopt, o::Track::Kind::audio);
    disabled_track->append_child(
        new o::Clip(
            "dis", new o::ExternalReference("dis.wav", tr(0, 4, 48000)),
            tr(0, 4, 48000)),
        &e);
    disabled_track->set_enabled(false);
    stack->append_child(enabled_track, &e);
    stack->append_child(disabled_track, &e);
    t->tracks()->append_child(stack, &e);
    TASK_CHECK(!o::is_error(e));

    auto parsed = json_round_trip(t.value);
    TASK_CHECK(bool(parsed));
    o::AudioRenderer r(*parsed, g_dir, 48000, &e);
    TASK_CHECK(!o::is_error(e));
    std::vector<float> out;
    TASK_CHECK(read_all(r, 2, out, &e));
    TASK_CHECK(out.size() == size_t(8));
    for (int i = 0; i < 4; ++i)
    {
        TASK_CHECK(near(out[2 * i], norm(100))); // only the enabled branch
    }
    return 0;
}

int
case_time_effect_and_bad_media()
{
    o::ErrorStatus e;

    // A time effect must be rejected.
    {
        TASK_CHECK(write_wav(
            wav_path("fx.wav"), 48000, 1, std::vector<int16_t>(4, 9)));
        o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
        auto* track =
            new o::Track("a", std::nullopt, o::Track::Kind::audio);
        std::vector<o::Effect*> fx = { new o::LinearTimeWarp("w", "warp", 2.0) };
        auto* clip = new o::Clip(
            "c",
            new o::ExternalReference("fx.wav", tr(0, 4, 48000)),
            tr(0, 4, 48000), o::AnyDictionary(), fx);
        track->append_child(clip, &e);
        t->tracks()->append_child(track, &e);
        auto parsed = json_round_trip(t.value);
        TASK_CHECK(bool(parsed));
        o::AudioRenderer r(*parsed, g_dir, 48000, &e);
        TASK_CHECK(o::is_error(e));
    }

    // A visible clip whose media cannot be read must be rejected.
    e = o::ErrorStatus();
    {
        o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
        auto* track =
            new o::Track("a", std::nullopt, o::Track::Kind::audio);
        track->append_child(
            new o::Clip(
                "ghost",
                new o::ExternalReference("does_not_exist.wav",
                                         tr(0, 4, 48000)),
                tr(0, 4, 48000)),
            &e);
        t->tracks()->append_child(track, &e);
        auto parsed = json_round_trip(t.value);
        TASK_CHECK(bool(parsed));
        o::AudioRenderer r(*parsed, g_dir, 48000, &e);
        TASK_CHECK(o::is_error(e));
    }
    return 0;
}

int
case_zero_duration()
{
    o::ErrorStatus e;
    o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("empty"));
    auto parsed = json_round_trip(t.value);
    TASK_CHECK(bool(parsed));
    o::AudioRenderer r(*parsed, g_dir, 48000, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(r.frame_count() == 0);
    TASK_CHECK(r.seek(0, &e));
    std::vector<float> out;
    TASK_CHECK(r.read(8, out, &e));
    TASK_CHECK(out.empty());
    return 0;
}

int
case_snapshot_isolation()
{
    o::ErrorStatus e;
    TASK_CHECK(write_wav(
        wav_path("iso.wav"), 48000, 1, std::vector<int16_t>(6, 42)));

    o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("t"));
    auto* track = new o::Track("a", std::nullopt, o::Track::Kind::audio);
    track->append_child(
        new o::Clip(
            "c", new o::ExternalReference("iso.wav", tr(0, 6, 48000)),
            tr(0, 6, 48000)),
        &e);
    t->tracks()->append_child(track, &e);
    TASK_CHECK(!o::is_error(e));

    o::AudioRenderer r(*t, g_dir, 48000, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(r.frame_count() == 6);

    // Mutate the original timeline after the renderer took its snapshot.
    t->tracks()->remove_child(0, &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(t->duration(&e) == RationalTime(0, 1));

    std::vector<float> out;
    TASK_CHECK(read_all(r, 2, out, &e));
    TASK_CHECK(out.size() == size_t(12));
    for (float v: out)
    {
        TASK_CHECK(near(v, norm(42)));
    }
    return 0;
}

uint32_t
read_u32_file(unsigned char const* p)
{
    return uint32_t(
        p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16)
        | (uint32_t(p[3]) << 24));
}

uint16_t
read_u16_file(unsigned char const* p)
{
    return uint16_t(p[0] | (uint16_t(p[1]) << 8));
}

int
case_example_cli_and_wav_bytes()
{
    o::ErrorStatus e;
    // Build a timeline + media and render in-process for reference.
    std::vector<int16_t> samples = { -8000, 4000, -6000, 3000,
                                     -4000, 2000, -2000, 1000 };
    TASK_CHECK(write_wav(wav_path("cli_src.wav"), 48000, 1, samples));

    o::SerializableObject::Retainer<o::Timeline> t(new o::Timeline("cli"));
    auto* track = new o::Track("a", std::nullopt, o::Track::Kind::audio);
    track->append_child(
        new o::Clip(
            "c", new o::ExternalReference("cli_src.wav", tr(0, 8, 48000)),
            tr(0, 8, 48000)),
        &e);
    t->tracks()->append_child(track, &e);
    std::string json = t->to_json_string(&e);
    TASK_CHECK(!o::is_error(e));
    std::string otio_path = g_dir + "/cli.otio";
    {
        std::ofstream f(otio_path, std::ios::trunc);
        f << json;
        TASK_CHECK(bool(f));
    }

    std::vector<float> reference;
    {
        auto parsed = json_round_trip(t.value);
        TASK_CHECK(bool(parsed));
        o::AudioRenderer r(*parsed, g_dir, 48000, &e);
        TASK_CHECK(!o::is_error(e));
        TASK_CHECK(read_all(r, 3, reference, &e));
        TASK_CHECK(reference.size() == size_t(16));
    }

    const char* bin = std::getenv("TASK_AUDIO_RENDER");
    if (bin && *bin)
    {
        std::string out_path = g_dir + "/cli_out.wav";
        std::string cmd = std::string(bin) + " " + otio_path + " " + g_dir
                          + " " + out_path + " 48000 3";
        int rc = std::system(cmd.c_str());
        TASK_CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 0);

        // Independently parse the produced WAV and compare every sample with
        // the in-process renderer output.
        std::ifstream f(out_path, std::ios::binary);
        std::vector<unsigned char> bytes(
            (std::istreambuf_iterator<char>(f)), {});
        TASK_CHECK(bytes.size() >= 44);
        TASK_CHECK(std::memcmp(bytes.data(), "RIFF", 4) == 0);
        TASK_CHECK(std::memcmp(bytes.data() + 8, "WAVE", 4) == 0);
        // Standard PCM header layout: format tag @20, channels @22,
        // sample rate @24, block align @32, bits per sample @34.
        TASK_CHECK(read_u16_file(bytes.data() + 20) == 1);  // PCM
        TASK_CHECK(read_u16_file(bytes.data() + 22) == 2);  // stereo
        TASK_CHECK(read_u32_file(bytes.data() + 24) == 48000);
        TASK_CHECK(read_u16_file(bytes.data() + 32) == 4);  // block align
        TASK_CHECK(read_u16_file(bytes.data() + 34) == 16); // bits/sample
        size_t data_off = 0;
        uint32_t data_size = 0;
        for (size_t p = 12; p + 8 <= bytes.size();)
        {
            uint32_t n = read_u32_file(bytes.data() + p + 4);
            if (std::memcmp(bytes.data() + p, "data", 4) == 0)
            {
                data_off = p + 8;
                data_size = n;
                break;
            }
            p += 8 + n + (n & 1u);
        }
        TASK_CHECK(data_size == 32);
        for (int i = 0; i < 16; ++i)
        {
            int16_t v = int16_t(read_u16_file(bytes.data() + data_off + 2 * i));
            TASK_CHECK(near(float(v) / 32768.0f, reference[i], 1.0 / 32768.0));
        }

        // A failing run must exit non-zero and leave no output file.
        std::string bad_otio = g_dir + "/bad.otio";
        {
            o::SerializableObject::Retainer<o::Timeline> bad(
                new o::Timeline("bad"));
            auto* bt =
                new o::Track("a", std::nullopt, o::Track::Kind::audio);
            bt->append_child(
                new o::Clip(
                    "g",
                    new o::ExternalReference("nope.wav", tr(0, 4, 48000)),
                    tr(0, 4, 48000)),
                &e);
            bad->tracks()->append_child(bt, &e);
            std::ofstream bf(bad_otio, std::ios::trunc);
            bf << bad->to_json_string(&e);
        }
        std::string bad_out = g_dir + "/bad_out.wav";
        std::remove(bad_out.c_str());
        std::string bad_cmd = std::string(bin) + " " + bad_otio + " " + g_dir
                              + " " + bad_out + " 48000 3";
        int brc = std::system(bad_cmd.c_str());
        TASK_CHECK(!(WIFEXITED(brc) && WEXITSTATUS(brc) == 0));
        struct stat st;
        TASK_CHECK(::stat(bad_out.c_str(), &st) != 0); // partial file removed
    }
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    { "coordinates", "mono_duplication", case_mono_duplication },
    { "coordinates", "half_rate_interpolation", case_half_rate_interpolation },
    { "coordinates", "44100_to_48000", case_44100_to_48000 },
    { "coordinates", "mixed_timebases_frame_count",
      case_mixed_timebases_frame_count },
    { "coordinates", "media_timebases", case_media_timebases },
    { "coordinates", "nested_crop", case_nested_crop },
    { "coordinates", "short_media_zero_fill", case_short_media_zero_fill },
    { "coordinates", "gap_silence", case_gap_silence },
    { "coordinates", "global_start_time_ignored",
      case_global_start_time_ignored },
    { "coordinates", "track_summing_and_video_ignored",
      case_track_summing_and_video_ignored },

    { "dissolve", "dissolve_mix", case_dissolve_mix },
    { "dissolve", "block_independence", case_dissolve_block_independence },
    { "dissolve", "nested_progress", case_dissolve_nested_progress },
    { "dissolve", "dissolve_errors", case_dissolve_errors },

    { "streaming", "seek_and_reread", case_seek_and_reread },
    { "streaming", "disabled_branch", case_disabled_branch },
    { "streaming", "time_effect_and_bad_media",
      case_time_effect_and_bad_media },
    { "streaming", "zero_duration", case_zero_duration },
    { "streaming", "snapshot_isolation", case_snapshot_isolation },
    { "streaming", "example_cli_and_wav_bytes", case_example_cli_and_wav_bytes },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
