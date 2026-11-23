// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

// Audio renderer regression tests.  Fixtures (short PCM16 WAV files and OTIO
// JSON projects) are generated here so the tests do not depend on external
// media.  The driver in task_support/driver.c runs the three registered
// groups: coordinates, dissolve and streaming.

#include "check.h"

#include <opentimelineio/audioRender.h>
#include <opentimelineio/clip.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/item.h>
#include <opentimelineio/linearTimeWarp.h>
#include <opentimelineio/pcmWave.h>
#include <opentimelineio/serializableObject.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>
#include <opentimelineio/transition.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#    include <sys/wait.h>
#    include <unistd.h>
#    define OTIO_HAVE_POSIX_SPAWN 1
#endif

namespace o = opentimelineio::OPENTIMELINEIO_VERSION_NS;

namespace {

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

std::string
unique_dir(char const* tag)
{
    char const* base = std::getenv("TMPDIR");
    std::filesystem::path dir =
        std::filesystem::path(base ? base : "/tmp")
        / (std::string("otio-audio-") + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

void
put_u16(std::ostream& out, uint16_t v)
{
    char b[2] = { char(v & 0xff), char((v >> 8) & 0xff) };
    out.write(b, 2);
}

void
put_u32(std::ostream& out, uint32_t v)
{
    char b[4] = { char(v & 0xff),
                  char((v >> 8) & 0xff),
                  char((v >> 16) & 0xff),
                  char((v >> 24) & 0xff) };
    out.write(b, 4);
}

void
put_tag(std::ostream& out, char const* tag)
{
    out.write(tag, 4);
}

// Write a RIFF/WAVE PCM16 file.  An optional unknown chunk with an odd
// payload is inserted (plus its pad byte) to prove the parser does not assume
// a canonical 44-byte header.
void
write_wav(
    std::string const&           path,
    int                          channels,
    uint32_t                     rate,
    std::vector<int16_t> const&  interleaved,
    bool                         odd_chunk = false)
{
    std::vector<uint8_t> data;
    for (int16_t s: interleaved)
    {
        data.push_back(uint8_t(uint16_t(s) & 0xff));
        data.push_back(uint8_t((uint16_t(s) >> 8) & 0xff));
    }

    uint32_t const data_bytes = uint32_t(data.size());
    std::vector<uint8_t> body;

    std::ostringstream fmt;
    put_tag(fmt, "fmt ");
    put_u32(fmt, 16);
    put_u16(fmt, 1);
    put_u16(fmt, uint16_t(channels));
    put_u32(fmt, rate);
    put_u32(fmt, rate * uint32_t(channels) * 2);
    put_u16(fmt, uint16_t(channels * 2));
    put_u16(fmt, 16);
    std::string const fmt_str = fmt.str();

    std::ostringstream odd;
    if (odd_chunk)
    {
        put_tag(odd, "junk");
        put_u32(odd, 3);      // odd payload
        odd.write("abc", 3);
        odd.put('\0');        // pad byte
    }
    std::string const odd_str = odd.str();

    std::ostringstream dat;
    put_tag(dat, "data");
    put_u32(dat, data_bytes);
    std::string const dat_str = dat.str();

    uint32_t const riff_size = uint32_t(
        4 + fmt_str.size() + odd_str.size() + dat_str.size() + data.size());

    std::ofstream out(path, std::ios::binary);
    put_tag(out, "RIFF");
    put_u32(out, riff_size);
    put_tag(out, "WAVE");
    out.write(fmt_str.data(), std::streamsize(fmt_str.size()));
    out.write(odd_str.data(), std::streamsize(odd_str.size()));
    out.write(dat_str.data(), std::streamsize(dat_str.size()));
    out.write(reinterpret_cast<char const*>(data.data()),
              std::streamsize(data.size()));
}

// A ramp that makes each frame identifiable at a glance.  Frame k on channel
// c is value (k + 1) * (c + 1) / 32768.
std::vector<int16_t>
ramp(int frames, int channels, int scale = 1)
{
    std::vector<int16_t> samples;
    samples.reserve(size_t(frames) * size_t(channels));
    for (int k = 0; k < frames; ++k)
    {
        for (int c = 0; c < channels; ++c)
        {
            samples.push_back(int16_t((k + 1) * (c + 1) * scale));
        }
    }
    return samples;
}

o::TimeRange
range(double start, double duration, double rate)
{
    return o::TimeRange(
        o::RationalTime(start, rate),
        o::RationalTime(duration, rate));
}

o::Clip*
clip(
    char const*        name,
    char const*        path,
    o::TimeRange       media_available,
    std::optional<o::TimeRange> source_range)
{
    return new o::Clip(
        name,
        new o::ExternalReference(path, media_available),
        source_range);
}

std::string
write_project(std::string const& dir, o::Timeline* timeline)
{
    o::ErrorStatus error;
    std::string    json = timeline->to_json_string(&error);
    if (o::is_error(error))
    {
        return std::string();
    }
    std::string const path = dir + "/project.otio";
    std::ofstream     out(path);
    out << json;
    return path;
}

o::SerializableObject::Retainer<o::Timeline>
load_timeline(std::string const& path)
{
    o::ErrorStatus error;
    o::SerializableObject* object =
        o::SerializableObject::from_json_file(path, &error);
    if (o::is_error(error) || !object)
    {
        return o::SerializableObject::Retainer<o::Timeline>();
    }
    auto timeline = dynamic_cast<o::Timeline*>(object);
    if (!timeline)
    {
        object->possibly_delete();
        return o::SerializableObject::Retainer<o::Timeline>();
    }
    return o::SerializableObject::Retainer<o::Timeline>(timeline);
}

// Render the entire timeline in one call.
std::vector<float>
render_all(o::AudioRenderer& renderer, o::AudioRenderStatus* status = nullptr)
{
    std::vector<float> out;
    out.resize(size_t(renderer.frame_count()) * 2);
    o::AudioRenderStatus local;
    int64_t const got = renderer.read(
        renderer.frame_count(),
        out.empty() ? nullptr : out.data(),
        status ? status : &local);
    if (got < 0)
    {
        return {};
    }
    out.resize(size_t(got) * 2);
    return out;
}

std::vector<float>
render_timeline(
    o::Timeline*       timeline,
    std::string const& media_root,
    double             rate)
{
    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, media_root, rate, &status);
    if (!renderer.is_valid())
    {
        return {};
    }
    return render_all(renderer);
}

bool
near(float a, float b, float tolerance = 1e-6f)
{
    return std::fabs(double(a) - double(b)) <= double(tolerance);
}

// ---------------------------------------------------------------------------
// CLI helpers (the example is exercised as a real process)
// ---------------------------------------------------------------------------

#if defined(OTIO_HAVE_POSIX_SPAWN)
int
run_process(std::vector<std::string> const& args)
{
    pid_t pid = fork();
    if (pid < 0)
    {
        return -1;
    }
    if (pid == 0)
    {
        std::vector<char const*> argv;
        for (auto const& a: args)
        {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);
        ::execv(argv[0], const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
    {
        return -1;
    }
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    return -1;
}
#endif

std::string
renderer_cli()
{
    char const* path = std::getenv("TASK_AUDIO_RENDER");
    return path ? path : std::string();
}

struct WavPcm
{
    bool                 stereo_pcm16 = false;
    uint32_t             rate         = 0;
    std::vector<int16_t> samples; // interleaved
};

// Parse a canonical-or-not RIFF/WAVE file, verifying it holds stereo PCM16.
bool
read_wav_pcm(std::string const& path, WavPcm& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }
    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    {
        return false;
    }
    size_t   data_offset = 0;
    uint32_t data_size   = 0;
    bool     have_fmt    = false;

    size_t pos = 12;
    while (pos + 8 <= bytes.size())
    {
        uint32_t size = uint32_t(bytes[pos + 4]) | (uint32_t(bytes[pos + 5]) << 8)
                        | (uint32_t(bytes[pos + 6]) << 16)
                        | (uint32_t(bytes[pos + 7]) << 24);
        std::string id(reinterpret_cast<char const*>(bytes.data() + pos), 4);
        if (id == "fmt ")
        {
            if (size < 16 || pos + 8 + 16 > bytes.size())
            {
                return false;
            }
            uint8_t const* f = bytes.data() + pos + 8;
            auto u16 = [](uint8_t const* p) {
                return uint16_t(p[0] | (p[1] << 8));
            };
            auto u32 = [](uint8_t const* p) {
                return uint32_t(p[0]) | (uint32_t(p[1]) << 8)
                       | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
            };
            out.rate         = u32(f + 4);
            out.stereo_pcm16 = u16(f) == 1 && u16(f + 2) == 2
                               && u16(f + 14) == 16 && u16(f + 12) == 4;
            have_fmt = true;
        }
        else if (id == "data")
        {
            data_offset = pos + 8;
            data_size   = size;
        }
        pos += 8 + size + (size & 1);
    }

    if (!have_fmt || !out.stereo_pcm16 || data_offset == 0
        || data_offset + data_size > bytes.size() || (data_size & 3) != 0)
    {
        return false;
    }
    out.samples.resize(data_size / 2);
    for (size_t i = 0; i < out.samples.size(); ++i)
    {
        out.samples[i] = int16_t(
            uint16_t(bytes[data_offset + 2 * i]
                     | (bytes[data_offset + 2 * i + 1] << 8)));
    }
    return true;
}

int16_t
quantize_reference(float sample)
{
    double x = double(sample);
    if (!(x == x))
    {
        x = 0.0;
    }
    x = std::max(x, -1.0);
    x = std::min(x, 32767.0 / 32768.0);
    double const scaled = x * 32768.0;
    double const rounded =
        scaled >= 0.0 ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
    return int16_t(std::max(-32768.0, std::min(32767.0, rounded)));
}

// ---------------------------------------------------------------------------
// PCM reader tests
// ---------------------------------------------------------------------------

int
test_pcm_odd_chunk()
{
    std::string const dir = unique_dir("pcm-odd");
    std::string const wav = dir + "/a.wav";

    // 4 stereo frames.
    write_wav(wav, 2, 48000, {
        1000, -1000,
        2000, -2000,
        3000, -3000,
        4000, -4000,
    }, /*odd_chunk=*/true);

    o::PcmWaveReader   reader;
    o::PcmWaveStatus   status;
    TASK_CHECK(reader.open(wav, &status));
    TASK_CHECK(status.ok());
    TASK_CHECK(reader.channel_count() == 2);
    TASK_CHECK(reader.sample_rate() == 48000.0);
    TASK_CHECK(reader.frame_count() == 4);

    float l = 0, r = 0;
    TASK_CHECK(reader.sample_at(0.0, l, r, &status));
    TASK_CHECK(near(l, 1000.0f / 32768.0f));
    TASK_CHECK(near(r, -1000.0f / 32768.0f));

    // Interpolation between frame 0 and frame 1.
    TASK_CHECK(reader.sample_at(0.5, l, r, &status));
    TASK_CHECK(near(l, 0.5f * (1000.0f + 2000.0f) / 32768.0f));
    TASK_CHECK(near(r, 0.5f * (-1000.0f - 2000.0f) / 32768.0f));

    // Past the physical media.
    TASK_CHECK(reader.sample_at(4.0, l, r, &status));
    TASK_CHECK(l == 0.0f && r == 0.0f);
    TASK_CHECK(reader.sample_at(-1.0, l, r, &status));
    TASK_CHECK(l == 0.0f && r == 0.0f);
    return 0;
}

int
test_pcm_mono_and_streaming()
{
    std::string const dir = unique_dir("pcm-mono");
    std::string const wav = dir + "/m.wav";

    std::vector<int16_t> samples;
    for (int k = 0; k < 100; ++k)
    {
        samples.push_back(int16_t((k + 1) * 10));
    }
    write_wav(wav, 1, 44100, samples);

    o::PcmWaveReader reader;
    o::PcmWaveStatus status;
    TASK_CHECK(reader.open(wav, &status));
    TASK_CHECK(reader.channel_count() == 1);
    TASK_CHECK(reader.frame_count() == 100);

    // Mono is duplicated to both channels.
    float l = 0, r = 0;
    TASK_CHECK(reader.sample_at(3.0, l, r, &status));
    TASK_CHECK(near(l, 40.0f / 32768.0f));
    TASK_CHECK(near(l, r));

    // Enough for exactly one 1-frame cache entry; frames beyond the cache are
    // still identical whether read in one shot or with tiny blocks.
    reader.set_cache_frame_count(1);
    std::vector<float> one(size_t(100) * 2, 0.0f);
    TASK_CHECK(reader.read_frames(0, 100, one.data(), &status));

    std::vector<float> chunked(size_t(100) * 2, 0.0f);
    for (int k = 0; k < 100; ++k)
    {
        TASK_CHECK(reader.read_frames(k, 1, chunked.data() + size_t(k) * 2, &status));
    }
    TASK_CHECK(one == chunked);
    return 0;
}

int
test_pcm_rejects_bad_format()
{
    std::string const dir = unique_dir("pcm-bad");
    std::string const wav = dir + "/x.wav";

    // 8-bit PCM is refused.
    {
        std::ofstream out(wav, std::ios::binary);
        put_tag(out, "RIFF");
        put_u32(out, 36 + 2);
        put_tag(out, "WAVE");
        put_tag(out, "fmt ");
        put_u32(out, 16);
        put_u16(out, 1);
        put_u16(out, 1);
        put_u32(out, 8000);
        put_u32(out, 8000);
        put_u16(out, 1);
        put_u16(out, 8);
        put_tag(out, "data");
        put_u32(out, 2);
        put_u16(out, 0);
    }
    o::PcmWaveReader reader;
    o::PcmWaveStatus status;
    TASK_CHECK(!reader.open(wav, &status));
    TASK_CHECK(status.error == o::PcmWaveError::UnsupportedFormat);

    // A missing file.
    TASK_CHECK(!reader.open(dir + "/does-not-exist.wav", &status));
    TASK_CHECK(status.error == o::PcmWaveError::FileOpenFailed);
    return 0;
}

// ---------------------------------------------------------------------------
// Graph helpers for the renderer tests
// ---------------------------------------------------------------------------

o::Track*
audio_track()
{
    return new o::Track("audio", std::nullopt, o::Track::Kind::audio);
}

o::Track*
video_track()
{
    return new o::Track("video", std::nullopt, o::Track::Kind::video);
}

} // namespace

// ===========================================================================
// Group: coordinates
// ===========================================================================

static int
coordinates_simple_gain_and_rate_conversion()
{
    // 48000 Hz media rendered at 24000 Hz: output frame n maps to program
    // time n/24000, i.e. media frame 2n.  A LinearTimeWarp must be rejected.
    std::string const dir = unique_dir("coord-gain");
    write_wav(dir + "/ramp.wav", 2, 48000, ramp(16, 2));

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("A", "ramp.wav", range(0, 16, 48000), range(0, 16, 48000)),
        nullptr);
    timeline->tracks()->append_child(track, nullptr);

    std::vector<float> const out = render_timeline(timeline, dir, 24000);
    TASK_CHECK(out.size() == 16); // 16/48000 s * 24000 = 8 output frames

    for (int n = 0; n < 8; ++n)
    {
        int const media_frame = 2 * n;
        TASK_CHECK(near(out[size_t(n) * 2], float(media_frame + 1) / 32768.0f));
        TASK_CHECK(near(
            out[size_t(n) * 2 + 1],
            float(2 * (media_frame + 1)) / 32768.0f));
    }
    return 0;
}

static int
coordinates_at_44100_to_48000()
{
    // 44100 Hz media rendered at 48000 Hz.  Output frame n is at media frame
    // n * 44100 / 48000, which is fractional and must be interpolated.
    std::string const dir = unique_dir("coord-441");

    // A bounded sawtooth so every sample fits comfortably in int16_t.
    int64_t const media_frames = 44100;
    auto value_at = [](int64_t k) -> double {
        return double(((k * 37) % 1000) - 500);
    };
    std::vector<int16_t> samples;
    samples.reserve(size_t(media_frames));
    for (int64_t k = 0; k < media_frames; ++k)
    {
        samples.push_back(int16_t(value_at(k)));
    }
    write_wav(dir + "/ramp.wav", 1, 44100, samples);

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    // One second of media at 44100 frames.
    track->append_child(
        clip("A", "ramp.wav", range(0, 44100, 44100), range(0, 44100, 44100)),
        nullptr);
    timeline->tracks()->append_child(track, nullptr);

    std::vector<float> const out = render_timeline(timeline, dir, 48000);
    TASK_CHECK(out.size() == size_t(48000) * 2);

    for (int n: { 0, 1, 2, 100, 47998, 47999 })
    {
        // The renderer derives program time as (n / 48000) seconds and then
        // multiplies by the media rate, so mirror that arithmetic here rather
        // than using a separately simplified ratio.
        double const frame_pos = (double(n) / 48000.0) * 44100.0;
        int64_t const base     = int64_t(std::floor(frame_pos));
        double const  frac     = frame_pos - std::floor(frame_pos);
        // A frame index past the media is silence.
        double const v0 = value_at(base);
        double const v1 = (base + 1 < media_frames) ? value_at(base + 1) : 0.0;
        float const  want = float((v0 + (v1 - v0) * frac) / 32768.0);
        TASK_CHECK(near(out[size_t(n) * 2], want, 2e-6f));
        TASK_CHECK(near(out[size_t(n) * 2 + 1], want, 2e-6f));
    }
    return 0;
}

static int
coordinates_nested_crop_and_gap()
{
    // The baseline graph: a track of A (4 frames) + dissolve + B (4 frames)
    // cropped by a Stack whose source_range starts one frame in.  The crop
    // moves the start of the program and shortens the duration.
    std::string const dir = unique_dir("coord-nested");
    write_wav(dir + "/a.wav", 2, 48000, ramp(16, 2, 100));
    write_wav(dir + "/b.wav", 2, 48000, ramp(16, 2, 200));

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("A", "a.wav", range(0, 16, 48000), range(2, 4, 48000)), nullptr);
    track->append_child(
        clip("B", "b.wav", range(0, 16, 48000), range(5, 4, 48000)), nullptr);

    auto stack = new o::Stack("crop", range(1, 6, 48000));
    stack->append_child(track, nullptr);
    timeline->tracks()->append_child(stack, nullptr);

    // duration = 6 frames at 48 kHz.
    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 48000, &status);
    TASK_CHECK(renderer.is_valid());
    TASK_CHECK(renderer.frame_count() == 6);

    std::vector<float> const out = render_all(renderer);
    TASK_CHECK(out.size() == 12);

    // The Stack's source_range starts one frame in, and OTIO maps a
    // composition's local time to its parent with a uniform offset equal to
    // that start (see Item::transformed_time).  So the stack-local time is
    // program + 1, and the track is laid out at 0 with A at source frame 2.
    //
    //   output 0 -> stack-local 1 -> track-local 1 -> A source frame 3
    //   output 1 -> stack-local 2 -> track-local 2 -> A source frame 4
    //   output 2 -> stack-local 3 -> track-local 3 -> A source frame 5
    //   output 3 -> stack-local 4 -> track-local 4 -> B source frame 5
    //   output 4 -> stack-local 5 -> track-local 5 -> B source frame 6
    //   output 5 -> stack-local 6 -> track-local 6 -> B source frame 7
    TASK_CHECK(near(out[0], float(4 * 100) / 32768.0f));
    TASK_CHECK(near(out[2], float(5 * 100) / 32768.0f));
    TASK_CHECK(near(out[4], float(6 * 100) / 32768.0f));
    TASK_CHECK(near(out[6], float(6 * 200) / 32768.0f));
    TASK_CHECK(near(out[8], float(7 * 200) / 32768.0f));
    TASK_CHECK(near(out[10], float(8 * 200) / 32768.0f));
    return 0;
}

static int
coordinates_mixed_timebases_and_short_media()
{
    // 24 fps media reference times mixed with a 25 fps clip and a 48000 Hz
    // output.  The clip covers 4 seconds; media has only 3 seconds, so the
    // remainder must render as silence.
    std::string const dir = unique_dir("coord-mixed");
    write_wav(dir + "/a.wav", 1, 24000, ramp(48000, 1)); // 2 seconds

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    // Media available range declared at 24 fps for 2 s.
    track->append_child(
        clip("A", "a.wav", range(0, 48, 24), range(0, 96, 25)),
        nullptr);
    timeline->tracks()->append_child(track, nullptr);

    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 48000, &status);
    TASK_CHECK(renderer.is_valid());
    TASK_CHECK(renderer.frame_count() == 48000 * 96 / 25); // ceil(3.84 s)

    // The clip is 96 frames at 25 fps = 3.84 s.  Media is only 2 s long from
    // source frame 0, so everything past 2 s is silence.
    // 3.84 s - 1/48000 s is past the 2 s of media.
    TASK_CHECK(renderer.seek(renderer.frame_count() - 1, &status));
    float tail[2] = { 1.0f, 1.0f };
    TASK_CHECK(renderer.read(1, tail, &status) == 1);
    TASK_CHECK(tail[0] == 0.0f && tail[1] == 0.0f);

    TASK_CHECK(renderer.seek(0, &status));
    std::vector<float> const out = render_all(renderer);
    TASK_CHECK(!out.empty());
    TASK_CHECK(near(out[0], 1.0f / 32768.0f));
    // The tail beyond the media must be zero.
    TASK_CHECK(out[out.size() - 2] == 0.0f);
    TASK_CHECK(out[out.size() - 1] == 0.0f);
    return 0;
}

static int
coordinates_global_start_time_is_not_an_offset()
{
    // global_start_time is a timecode only; it must not shift the render.
    std::string const dir = unique_dir("coord-gst");
    write_wav(dir + "/a.wav", 1, 48000, ramp(8, 1));

    o::SerializableObject::Retainer<o::Timeline> timeline(
        new o::Timeline("t", o::RationalTime(100, 48000)));
    auto track = audio_track();
    track->append_child(
        clip("A", "a.wav", range(0, 8, 48000), range(0, 8, 48000)),
        nullptr);
    timeline->tracks()->append_child(track, nullptr);

    std::vector<float> const out = render_timeline(timeline, dir, 48000);
    TASK_CHECK(out.size() == 16);
    TASK_CHECK(near(out[0], 1.0f / 32768.0f));
    TASK_CHECK(near(out[2], 2.0f / 32768.0f));
    return 0;
}

static int
coordinates_video_track_and_disabled_branch()
{
    // A video track with a clip contributes nothing; an audio Stack child
    // with enabled=false is muted, while the other branch still mixes in.
    std::string const dir = unique_dir("coord-mute");
    write_wav(dir + "/a.wav", 1, 48000, ramp(4, 1, 100));
    write_wav(dir + "/b.wav", 1, 48000, ramp(4, 1, 200));

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));

    auto video = video_track();
    video->append_child(
        clip("V", "a.wav", range(0, 4, 48000), range(0, 4, 48000)), nullptr);
    timeline->tracks()->append_child(video, nullptr);

    auto stack = new o::Stack("mix");
    auto muted = audio_track();
    muted->set_enabled(false);
    muted->append_child(
        clip("M", "b.wav", range(0, 4, 48000), range(0, 4, 48000)), nullptr);
    stack->append_child(muted, nullptr);

    auto live = audio_track();
    live->append_child(
        clip("L", "a.wav", range(0, 4, 48000), range(0, 4, 48000)), nullptr);
    stack->append_child(live, nullptr);
    timeline->tracks()->append_child(stack, nullptr);

    std::vector<float> const out = render_timeline(timeline, dir, 48000);
    TASK_CHECK(out.size() == 8);
    TASK_CHECK(near(out[0], 100.0f / 32768.0f));
    TASK_CHECK(near(out[6], 400.0f / 32768.0f));
    return 0;
}

static int
coordinates_stack_sums_and_track_sequences()
{
    // Two audio tracks in a Stack add; a Gap on one track leaves the other's
    // audio intact.
    std::string const dir = unique_dir("coord-sum");
    write_wav(dir + "/a.wav", 2, 48000, ramp(4, 2, 100));
    write_wav(dir + "/b.wav", 2, 48000, ramp(4, 2, 300));

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));

    auto top = audio_track();
    top->append_child(new o::Gap(o::RationalTime(2, 48000)), nullptr);
    top->append_child(
        clip("T2", "a.wav", range(0, 4, 48000), range(2, 2, 48000)), nullptr);
    timeline->tracks()->append_child(top, nullptr);

    auto bottom = audio_track();
    bottom->append_child(
        clip("B", "b.wav", range(0, 4, 48000), range(0, 4, 48000)), nullptr);
    timeline->tracks()->append_child(bottom, nullptr);

    std::vector<float> const out = render_timeline(timeline, dir, 48000);
    TASK_CHECK(out.size() == 8);

    // Frames 0-1: silence + B.
    TASK_CHECK(near(out[0], 0.0f + 300.0f / 32768.0f));
    // Frames 2-3: A frame 2 (300*100) + B frame 2 (300*300).  Wait: A is
    // scaled 100, so A value is 3*100 = 300, B value is 3*300 = 900.
    TASK_CHECK(near(out[4], (300.0f + 900.0f) / 32768.0f));
    TASK_CHECK(near(out[6], (400.0f + 1200.0f) / 32768.0f));
    return 0;
}

static int
coordinates_time_effect_is_rejected()
{
    std::string const dir = unique_dir("coord-effect");
    write_wav(dir + "/a.wav", 1, 48000, ramp(4, 1));

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    auto c = clip("A", "a.wav", range(0, 4, 48000), range(0, 4, 48000));
    c->effects().push_back(new o::LinearTimeWarp("warp", "warp", 2.0));
    track->append_child(c, nullptr);
    timeline->tracks()->append_child(track, nullptr);

    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 48000, &status);
    TASK_CHECK(!renderer.is_valid());
    TASK_CHECK(status.error == o::AudioRenderError::UnsupportedFeature);
    return 0;
}

static int
coordinates_missing_media_is_rejected()
{
    std::string const dir = unique_dir("coord-missing");

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("A", "nope.wav", range(0, 4, 48000), range(0, 4, 48000)),
        nullptr);
    timeline->tracks()->append_child(track, nullptr);

    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 48000, &status);
    TASK_CHECK(!renderer.is_valid());
    TASK_CHECK(status.error == o::AudioRenderError::MediaReadError);
    return 0;
}

static int
coordinates_invalid_arguments()
{
    std::string const dir = unique_dir("coord-args");
    write_wav(dir + "/a.wav", 1, 48000, ramp(4, 1));

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("A", "a.wav", range(0, 4, 48000), range(0, 4, 48000)), nullptr);
    timeline->tracks()->append_child(track, nullptr);

    {
        o::AudioRenderStatus status;
        o::AudioRenderer renderer(timeline, dir, 0.0, &status);
        TASK_CHECK(!renderer.is_valid());
        TASK_CHECK(status.error == o::AudioRenderError::InvalidArgument);
    }
    {
        o::AudioRenderStatus status;
        o::AudioRenderer renderer(nullptr, dir, 48000, &status);
        TASK_CHECK(!renderer.is_valid());
        TASK_CHECK(status.error == o::AudioRenderError::InvalidArgument);
    }

    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 48000, &status);
    TASK_CHECK(renderer.is_valid());
    TASK_CHECK(renderer.frame_count() == 4);

    // Out-of-range seeks report an error and keep the position.
    TASK_CHECK(renderer.seek(2, &status));
    TASK_CHECK(renderer.tell() == 2);
    TASK_CHECK(!renderer.seek(-1, &status));
    TASK_CHECK(status.error == o::AudioRenderError::InvalidSeek);
    TASK_CHECK(renderer.tell() == 2);
    TASK_CHECK(!renderer.seek(renderer.frame_count() + 1, &status));
    TASK_CHECK(status.error == o::AudioRenderError::InvalidSeek);
    TASK_CHECK(renderer.tell() == 2);

    // Reading at the end returns zero frames.
    TASK_CHECK(renderer.seek(renderer.frame_count(), &status));
    float sample[2] = { 1.0f, 1.0f };
    TASK_CHECK(renderer.read(4, sample, &status) == 0);
    return 0;
}

static int
coordinates_zero_duration_project()
{
    std::string const dir = unique_dir("coord-zero");

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    timeline->tracks()->append_child(audio_track(), nullptr);

    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 48000, &status);
    TASK_CHECK(renderer.is_valid());
    TASK_CHECK(renderer.frame_count() == 0);
    float sample[2] = { 0.0f, 0.0f };
    TASK_CHECK(renderer.read(1, sample, &status) == 0);
    return 0;
}

// ===========================================================================
// Group: dissolve
// ===========================================================================

static int
dissolve_known_samples()
{
    // A and B are four 48 kHz frames each, with one frame of in/out handles.
    // The transition therefore covers output frames [3, 5).
    //
    //   frame 3: u = 0    -> only A
    //   frame 4: u = 0.5  -> cos(pi/4) * A + sin(pi/4) * B
    //
    // A's source at program frame 4 is source frame 4, and B's is frame 0.
    std::string const dir = unique_dir("dissolve-basic");
    write_wav(dir + "/a.wav", 2, 48000, {
        100, 0,
        200, 0,
        300, 0,
        400, 0,
        500, 0, // handle for the dissolve
        600, 0,
        700, 0,
    });
    write_wav(dir + "/b.wav", 2, 48000, {
        -100, 0,
        -200, 0,
        -300, 0,
        -400, 0,
    });

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    auto a = clip("A", "a.wav", range(0, 7, 48000), range(0, 4, 48000));
    auto b = clip("B", "b.wav", range(0, 4, 48000), range(0, 4, 48000));
    auto dissolve = new o::Transition(
        "xfade",
        o::Transition::Type::SMPTE_Dissolve,
        o::RationalTime(1, 48000),
        o::RationalTime(1, 48000));
    track->append_child(a, nullptr);
    track->append_child(dissolve, nullptr);
    track->append_child(b, nullptr);
    timeline->tracks()->append_child(track, nullptr);

    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 48000, &status);
    TASK_CHECK(renderer.is_valid());
    TASK_CHECK(renderer.frame_count() == 8);

    std::vector<float> const out = render_all(renderer);
    TASK_CHECK(out.size() == 16);

    TASK_CHECK(near(out[0], 100.0f / 32768.0f));
    TASK_CHECK(near(out[2], 200.0f / 32768.0f));
    TASK_CHECK(near(out[4], 300.0f / 32768.0f));
    // Frame 3: transition progress 0, so pure A at source frame 3.
    TASK_CHECK(near(out[6], 400.0f / 32768.0f));

    double const root_half = std::sqrt(0.5);
    // Frame 4: half-way through, A source frame 4 and B source frame 0.
    float const want =
        float((root_half * 500.0 + root_half * -100.0) / 32768.0);
    TASK_CHECK(near(out[8], want, 2e-6f));

    // Frames 5-7: pure B.
    TASK_CHECK(near(out[10], -200.0f / 32768.0f));
    TASK_CHECK(near(out[12], -300.0f / 32768.0f));
    TASK_CHECK(near(out[14], -400.0f / 32768.0f));
    return 0;
}

static int
dissolve_progress_is_not_restarted()
{
    // A nested trim cuts into the transition.  The progress must still be
    // measured from the original transition start, so a crop beginning one
    // frame into the transition starts at u = 0.5 (equal-power mix), not u = 0.
    std::string const dir = unique_dir("dissolve-progress");
    write_wav(dir + "/a.wav", 1, 48000, {
        100, 200, 300, 400, 500, 600, 700, 800,
    });
    write_wav(dir + "/b.wav", 1, 48000, {
        -100, -200, -300, -400, -500, -600, -700, -800,
    });

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("A", "a.wav", range(0, 8, 48000), range(0, 4, 48000)), nullptr);
    track->append_child(
        new o::Transition(
            "xfade",
            o::Transition::Type::SMPTE_Dissolve,
            o::RationalTime(1, 48000),
            o::RationalTime(1, 48000)),
        nullptr);
    track->append_child(
        clip("B", "b.wav", range(0, 8, 48000), range(0, 4, 48000)), nullptr);

    auto stack = new o::Stack("crop", range(1, 6, 48000));
    stack->append_child(track, nullptr);
    timeline->tracks()->append_child(stack, nullptr);

    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 48000, &status);
    TASK_CHECK(renderer.is_valid());
    TASK_CHECK(renderer.frame_count() == 6);

    std::vector<float> const out = render_all(renderer);
    TASK_CHECK(out.size() == 12);

    // Output frame 0 is track-local frame 1 (A at source frame 1).
    TASK_CHECK(near(out[0], 200.0f / 32768.0f));
    // Output frame 1 is track-local 2 (A source frame 2).
    TASK_CHECK(near(out[2], 300.0f / 32768.0f));
    // Output frame 2 is track-local 3, the transition start: pure A at
    // source frame 3.
    TASK_CHECK(near(out[4], 400.0f / 32768.0f));
    // Output frame 3 is track-local 4, u = 0.5: A source frame 4 and B
    // source frame 0.
    double const root_half = std::sqrt(0.5);
    TASK_CHECK(near(
        out[6],
        float((root_half * 500.0 + root_half * -100.0) / 32768.0),
        2e-6f));
    // Output frame 4 is track-local 5: pure B at source frame 1.
    TASK_CHECK(near(out[8], -200.0f / 32768.0f));
    // Output frame 5 is track-local 6: B source frame 2.
    TASK_CHECK(near(out[10], -300.0f / 32768.0f));
    return 0;
}

static int
dissolve_across_read_blocks()
{
    // Same graph as dissolve_known_samples, but read one frame at a time so
    // the transition straddles block boundaries.
    std::string const dir = unique_dir("dissolve-blocks");
    write_wav(dir + "/a.wav", 1, 48000, { 100, 200, 300, 400, 500, 600, 700 });
    write_wav(dir + "/b.wav", 1, 48000, { -100, -200, -300, -400 });

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("A", "a.wav", range(0, 7, 48000), range(0, 4, 48000)), nullptr);
    track->append_child(
        new o::Transition(
            "xfade",
            o::Transition::Type::SMPTE_Dissolve,
            o::RationalTime(1, 48000),
            o::RationalTime(1, 48000)),
        nullptr);
    track->append_child(
        clip("B", "b.wav", range(0, 4, 48000), range(0, 4, 48000)), nullptr);
    timeline->tracks()->append_child(track, nullptr);

    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 48000, &status);
    TASK_CHECK(renderer.is_valid());

    std::vector<float> chunked(size_t(renderer.frame_count()) * 2, 0.0f);
    for (int64_t n = 0; n < renderer.frame_count(); ++n)
    {
        TASK_CHECK(renderer.read(1, chunked.data() + size_t(n) * 2, &status) == 1);
    }

    o::AudioRenderer again(timeline, dir, 48000, &status);
    std::vector<float> const whole = render_all(again);
    TASK_CHECK(!whole.empty());
    TASK_CHECK(whole == chunked);
    return 0;
}

static int
dissolve_requires_two_clips()
{
    std::string const dir = unique_dir("dissolve-neighbors");
    write_wav(dir + "/a.wav", 1, 48000, ramp(8, 1));

    {
        // Transition with a Gap neighbour.
        o::SerializableObject::Retainer<o::Timeline> timeline(
            new o::Timeline("t"));
        auto track = audio_track();
        track->append_child(
            clip("A", "a.wav", range(0, 8, 48000), range(0, 4, 48000)),
            nullptr);
        track->append_child(
            new o::Transition(
                "xfade",
                o::Transition::Type::SMPTE_Dissolve,
                o::RationalTime(1, 48000),
                o::RationalTime(1, 48000)),
            nullptr);
        track->append_child(new o::Gap(o::RationalTime(4, 48000)), nullptr);
        timeline->tracks()->append_child(track, nullptr);

        o::AudioRenderStatus status;
        o::AudioRenderer renderer(timeline, dir, 48000, &status);
        TASK_CHECK(!renderer.is_valid());
        TASK_CHECK(status.error == o::AudioRenderError::UnsupportedFeature);
    }

    {
        // Non-dissolve transition type.
        o::SerializableObject::Retainer<o::Timeline> timeline(
            new o::Timeline("t"));
        auto track = audio_track();
        track->append_child(
            clip("A", "a.wav", range(0, 8, 48000), range(0, 4, 48000)),
            nullptr);
        track->append_child(
            new o::Transition(
                "wipe",
                "Custom_Transition",
                o::RationalTime(1, 48000),
                o::RationalTime(1, 48000)),
            nullptr);
        track->append_child(
            clip("B", "a.wav", range(0, 8, 48000), range(0, 4, 48000)),
            nullptr);
        timeline->tracks()->append_child(track, nullptr);

        o::AudioRenderStatus status;
        o::AudioRenderer renderer(timeline, dir, 48000, &status);
        TASK_CHECK(!renderer.is_valid());
        TASK_CHECK(status.error == o::AudioRenderError::UnsupportedFeature);
    }
    return 0;
}

static int
dissolve_overlapping_transitions_rejected()
{
    std::string const dir = unique_dir("dissolve-overlap");
    write_wav(dir + "/a.wav", 1, 48000, ramp(16, 1));

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    auto make = [&](char const* name, double start) {
        return clip(name, "a.wav", range(0, 16, 48000), range(start, 4, 48000));
    };
    auto x = [&]() {
        return new o::Transition(
            "x",
            o::Transition::Type::SMPTE_Dissolve,
            o::RationalTime(3, 48000),
            o::RationalTime(3, 48000));
    };
    // Three four-frame clips with two three-frame handles: the first
    // transition covers [1, 7) and the second [5, 11), which intersect.
    track->append_child(make("A", 0), nullptr);
    track->append_child(x(), nullptr);
    track->append_child(make("B", 4), nullptr);
    track->append_child(x(), nullptr);
    track->append_child(make("C", 8), nullptr);
    timeline->tracks()->append_child(track, nullptr);

    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 48000, &status);
    TASK_CHECK(!renderer.is_valid());
    TASK_CHECK(status.error == o::AudioRenderError::UnsupportedFeature);
    return 0;
}

// ===========================================================================
// Group: streaming
// ===========================================================================

static int
streaming_block_reads_match_one_shot()
{
    std::string const dir = unique_dir("stream-blocks");
    write_wav(dir + "/a.wav", 2, 48000, ramp(97, 2, 7));

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("A", "a.wav", range(0, 97, 48000), range(0, 97, 48000)), nullptr);
    timeline->tracks()->append_child(track, nullptr);

    o::AudioRenderStatus status;
    o::AudioRenderer whole(timeline, dir, 48000, &status);
    TASK_CHECK(whole.is_valid());
    std::vector<float> const reference = render_all(whole);
    TASK_CHECK(reference.size() == size_t(97) * 2);

    for (int64_t block: { 1, 2, 3, 7, 32, 96, 97, 1000 })
    {
        o::AudioRenderer renderer(timeline, dir, 48000, &status);
        TASK_CHECK(renderer.is_valid());
        std::vector<float> got(size_t(97) * 2, 0.0f);
        int64_t            offset = 0;
        while (offset < 97)
        {
            int64_t const count =
                renderer.read(block, got.data() + size_t(offset) * 2, &status);
            TASK_CHECK(count >= 0);
            if (count == 0)
            {
                break;
            }
            offset += count;
        }
        TASK_CHECK(offset == 97);
        TASK_CHECK(got == reference);
    }
    return 0;
}

static int
streaming_seek_rereads_identical_samples()
{
    std::string const dir = unique_dir("stream-seek");
    write_wav(dir + "/a.wav", 1, 48000, ramp(200, 1, 3));

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("A", "a.wav", range(0, 200, 48000), range(0, 200, 48000)),
        nullptr);
    timeline->tracks()->append_child(track, nullptr);

    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 48000, &status);
    TASK_CHECK(renderer.is_valid());

    std::vector<float> const reference = render_all(renderer);
    TASK_CHECK(reference.size() == size_t(200) * 2);

    // Re-read an arbitrary interior span after seeking.
    TASK_CHECK(renderer.seek(37, &status));
    std::vector<float> span(size_t(50) * 2, 0.0f);
    TASK_CHECK(renderer.read(50, span.data(), &status) == 50);
    for (int i = 0; i < 50; ++i)
    {
        TASK_CHECK(span[size_t(i) * 2] == reference[size_t(37 + i) * 2]);
        TASK_CHECK(span[size_t(i) * 2 + 1] == reference[size_t(37 + i) * 2 + 1]);
    }

    // Seek back to the beginning and read everything again.
    TASK_CHECK(renderer.seek(0, &status));
    std::vector<float> const again = render_all(renderer);
    TASK_CHECK(again == reference);

    // Seeking to the end then reading yields zero frames.
    TASK_CHECK(renderer.seek(renderer.frame_count(), &status));
    float sample[2];
    TASK_CHECK(renderer.read(10, sample, &status) == 0);
    return 0;
}

static int
streaming_read_short_at_end()
{
    std::string const dir = unique_dir("stream-end");
    write_wav(dir + "/a.wav", 1, 48000, ramp(10, 1));

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("A", "a.wav", range(0, 10, 48000), range(0, 10, 48000)), nullptr);
    timeline->tracks()->append_child(track, nullptr);

    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 48000, &status);
    TASK_CHECK(renderer.is_valid());
    TASK_CHECK(renderer.frame_count() == 10);

    TASK_CHECK(renderer.seek(8, &status));
    float samples[10] = { 0 };
    TASK_CHECK(renderer.read(5, samples, &status) == 2);
    TASK_CHECK(renderer.tell() == 10);
    TASK_CHECK(renderer.read(5, samples, &status) == 0);
    return 0;
}

static int
streaming_snapshot_is_isolated_from_edits()
{
    // Renderer holds a snapshot: after construction, editing the original
    // timeline must not change what the renderer produces.
    std::string const dir = unique_dir("stream-snapshot");
    write_wav(dir + "/a.wav", 1, 48000, ramp(8, 1, 100));
    write_wav(dir + "/b.wav", 1, 48000, ramp(8, 1, 200));

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("A", "a.wav", range(0, 8, 48000), range(0, 4, 48000)), nullptr);
    timeline->tracks()->append_child(track, nullptr);

    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 48000, &status);
    TASK_CHECK(renderer.is_valid());
    std::vector<float> const before = render_all(renderer);
    TASK_CHECK(before.size() == 8);

    // Mutate the original project: replace the clip and change its range.
    track->remove_child(0, nullptr);
    track->append_child(
        clip("B", "b.wav", range(0, 8, 48000), range(0, 4, 48000)), nullptr);
    timeline->set_global_start_time(o::RationalTime(999, 48000));

    TASK_CHECK(renderer.seek(0, &status));
    std::vector<float> const after = render_all(renderer);
    TASK_CHECK(after == before);
    return 0;
}

static int
streaming_json_round_trip()
{
    // The example loads a real JSON project.  Exercise that path here: write
    // JSON, load it back, render, and confirm the sample data.
    std::string const dir = unique_dir("stream-json");
    write_wav(dir + "/a.wav", 2, 48000, {
        1000, -1000, 2000, -2000, 3000, -3000, 4000, -4000,
    });

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("A", "a.wav", range(0, 4, 48000), range(0, 4, 48000)), nullptr);
    timeline->tracks()->append_child(track, nullptr);

    std::string const project = write_project(dir, timeline);
    TASK_CHECK(!project.empty());

    auto loaded = load_timeline(project);
    TASK_CHECK(bool(loaded));

    std::vector<float> const out = render_timeline(loaded, dir, 48000);
    TASK_CHECK(out.size() == 8);
    TASK_CHECK(near(out[0], 1000.0f / 32768.0f));
    TASK_CHECK(near(out[1], -1000.0f / 32768.0f));
    TASK_CHECK(near(out[6], 4000.0f / 32768.0f));
    TASK_CHECK(near(out[7], -4000.0f / 32768.0f));
    return 0;
}

static int
streaming_mono_and_cache_size()
{
    std::string const dir = unique_dir("stream-mono");
    write_wav(dir + "/m.wav", 1, 24000, ramp(37, 1, 11));

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("M", "m.wav", range(0, 37, 24000), range(0, 37, 24000)), nullptr);
    timeline->tracks()->append_child(track, nullptr);

    o::AudioRenderStatus status;
    o::AudioRenderer renderer(timeline, dir, 24000, &status);
    TASK_CHECK(renderer.is_valid());
    std::vector<float> const out = render_all(renderer);
    TASK_CHECK(out.size() == size_t(37) * 2);
    for (int i = 0; i < 37; ++i)
    {
        TASK_CHECK(near(out[size_t(i) * 2], float((i + 1) * 11) / 32768.0f));
        TASK_CHECK(out[size_t(i) * 2] == out[size_t(i) * 2 + 1]);
    }
    return 0;
}

// ===========================================================================
// Example CLI
// ===========================================================================

static int
streaming_example_matches_library_and_quantization()
{
    std::string const cli = renderer_cli();
    TASK_CHECK(!cli.empty());

#if defined(OTIO_HAVE_POSIX_SPAWN)
    std::string const dir = unique_dir("cli-match");

    // Values deliberately land on exact halves after scaling so the
    // round-half-away-from-zero rule is observable.  We render at 48000 Hz,
    // so output frame n maps one-to-one to media frame n.
    std::vector<int16_t> samples = {
        0,      1,      2,      3,
        16384,  -16384, 16385,  -16385,
        32767,  -32768, 100,    -100,
    };
    write_wav(dir + "/m.wav", 2, 48000, samples);

    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("A", "m.wav", range(0, 6, 48000), range(0, 6, 48000)), nullptr);
    timeline->tracks()->append_child(track, nullptr);
    std::string const project = write_project(dir, timeline);
    TASK_CHECK(!project.empty());

    std::string const out_path = dir + "/out.wav";
    int const status = run_process(
        { cli, project, dir, out_path, "48000", "2" });
    TASK_CHECK(status == 0);

    WavPcm wav;
    TASK_CHECK(read_wav_pcm(out_path, wav));
    TASK_CHECK(wav.rate == 48000);
    TASK_CHECK(wav.samples.size() == 12);

    // Independent expectation: the same library rendered in memory.
    o::AudioRenderStatus render_status;
    o::AudioRenderer renderer(timeline, dir, 48000, &render_status);
    TASK_CHECK(renderer.is_valid());
    std::vector<float> const reference = render_all(renderer);
    TASK_CHECK(reference.size() == 12);

    for (size_t i = 0; i < 12; ++i)
    {
        TASK_CHECK(wav.samples[i] == quantize_reference(reference[i]));
    }
    // A few spot checks of the quantization rule itself.
    TASK_CHECK(wav.samples[0] == 0);
    TASK_CHECK(wav.samples[1] == 1);
    TASK_CHECK(wav.samples[8] == 32767);

    // The example must not leave a partial file behind on success.
    TASK_CHECK(!std::filesystem::exists(out_path + ".partial"));
#endif
    return 0;
}

static int
streaming_example_failure_deletes_output()
{
    std::string const cli = renderer_cli();
    TASK_CHECK(!cli.empty());

#if defined(OTIO_HAVE_POSIX_SPAWN)
    std::string const dir = unique_dir("cli-fail");
    std::string const out_path = dir + "/failed.wav";

    // A project that references media that does not exist.  The CLI must
    // fail (non-zero) and must not leave an output or partial file.
    o::SerializableObject::Retainer<o::Timeline> timeline(new o::Timeline("t"));
    auto track = audio_track();
    track->append_child(
        clip("A", "missing.wav", range(0, 4, 48000), range(0, 4, 48000)),
        nullptr);
    timeline->tracks()->append_child(track, nullptr);
    std::string const project = write_project(dir, timeline);
    TASK_CHECK(!project.empty());

    int const status = run_process(
        { cli, project, dir, out_path, "48000", "4" });
    TASK_CHECK(status != 0);
    TASK_CHECK(!std::filesystem::exists(out_path));
    TASK_CHECK(!std::filesystem::exists(out_path + ".partial"));

    // A malformed project and bad arguments must also fail without output.
    TASK_CHECK(run_process({ cli, dir + "/no-such.otio", dir, out_path, "48000", "4" }) != 0);
    TASK_CHECK(run_process({ cli, project, dir, out_path, "0", "4" }) != 0);
    TASK_CHECK(run_process({ cli }) != 0);
    TASK_CHECK(!std::filesystem::exists(out_path));
#endif
    return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    { "coordinates", "simple_gain_and_rate_conversion",
      coordinates_simple_gain_and_rate_conversion },
    { "coordinates", "at_44100_to_48000", coordinates_at_44100_to_48000 },
    { "coordinates", "nested_crop_and_gap", coordinates_nested_crop_and_gap },
    { "coordinates", "mixed_timebases_and_short_media",
      coordinates_mixed_timebases_and_short_media },
    { "coordinates", "global_start_time_is_not_an_offset",
      coordinates_global_start_time_is_not_an_offset },
    { "coordinates", "video_track_and_disabled_branch",
      coordinates_video_track_and_disabled_branch },
    { "coordinates", "stack_sums_and_track_sequences",
      coordinates_stack_sums_and_track_sequences },
    { "coordinates", "time_effect_is_rejected",
      coordinates_time_effect_is_rejected },
    { "coordinates", "missing_media_is_rejected",
      coordinates_missing_media_is_rejected },
    { "coordinates", "invalid_arguments", coordinates_invalid_arguments },
    { "coordinates", "zero_duration_project",
      coordinates_zero_duration_project },
    { "coordinates", "pcm_odd_chunk", test_pcm_odd_chunk },
    { "coordinates", "pcm_mono_and_streaming", test_pcm_mono_and_streaming },
    { "coordinates", "pcm_rejects_bad_format", test_pcm_rejects_bad_format },

    { "dissolve", "known_samples", dissolve_known_samples },
    { "dissolve", "progress_is_not_restarted",
      dissolve_progress_is_not_restarted },
    { "dissolve", "across_read_blocks", dissolve_across_read_blocks },
    { "dissolve", "requires_two_clips", dissolve_requires_two_clips },
    { "dissolve", "overlapping_transitions_rejected",
      dissolve_overlapping_transitions_rejected },

    { "streaming", "block_reads_match_one_shot",
      streaming_block_reads_match_one_shot },
    { "streaming", "seek_rereads_identical_samples",
      streaming_seek_rereads_identical_samples },
    { "streaming", "read_short_at_end", streaming_read_short_at_end },
    { "streaming", "snapshot_is_isolated_from_edits",
      streaming_snapshot_is_isolated_from_edits },
    { "streaming", "json_round_trip", streaming_json_round_trip },
    { "streaming", "mono_and_cache_size", streaming_mono_and_cache_size },
    { "streaming", "example_matches_library_and_quantization",
      streaming_example_matches_library_and_quantization },
    { "streaming", "example_failure_deletes_output",
      streaming_example_failure_deletes_output },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
