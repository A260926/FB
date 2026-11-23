// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

// Render an OTIO timeline with local PCM16 WAV media to a stereo PCM16 WAV.
//
// Usage:
//   audio_render input.otio media_root output.wav rate block_frames

#include <opentimelineio/audioRender.h>
#include <opentimelineio/serializableObject.h>
#include <opentimelineio/timeline.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace o = opentimelineio::OPENTIMELINEIO_VERSION_NS;

namespace {

void
put_u16(std::ostream& out, uint16_t value)
{
    char bytes[2] = { char(value & 0xff), char((value >> 8) & 0xff) };
    out.write(bytes, 2);
}

void
put_u32(std::ostream& out, uint32_t value)
{
    char bytes[4] = { char(value & 0xff),
                      char((value >> 8) & 0xff),
                      char((value >> 16) & 0xff),
                      char((value >> 24) & 0xff) };
    out.write(bytes, 4);
}

int16_t
quantize(float sample)
{
    double clamped = double(sample);
    if (!(clamped == clamped)) // NaN
    {
        clamped = 0.0;
    }
    if (clamped < -1.0)
    {
        clamped = -1.0;
    }
    if (clamped > 32767.0 / 32768.0)
    {
        clamped = 32767.0 / 32768.0;
    }

    double const scaled = clamped * 32768.0;
    // Round half away from zero.
    double const rounded = scaled >= 0.0 ? std::floor(scaled + 0.5)
                                         : std::ceil(scaled - 0.5);
    if (rounded >= 32767.0)
    {
        return 32767;
    }
    if (rounded <= -32768.0)
    {
        return -32768;
    }
    return int16_t(rounded);
}

void
fail(std::string const& message)
{
    std::fprintf(stderr, "audio_render: %s\n", message.c_str());
}

} // namespace

int
main(int argc, char** argv)
{
    if (argc != 6)
    {
        fail("usage: audio_render input.otio media_root output.wav rate "
             "block_frames");
        return 2;
    }

    std::string const input_path  = argv[1];
    std::string const media_root  = argv[2];
    std::string const output_path = argv[3];

    char*     end  = nullptr;
    double const rate = std::strtod(argv[4], &end);
    if (!end || *end != '\0' || !(rate > 0.0) || !std::isfinite(rate))
    {
        fail("rate must be a positive number");
        return 2;
    }
    end = nullptr;
    long const block_frames = std::strtol(argv[5], &end, 10);
    if (!end || *end != '\0' || block_frames <= 0)
    {
        fail("block_frames must be a positive integer");
        return 2;
    }

    o::ErrorStatus error;
    o::SerializableObject::Retainer<o::SerializableObject> object(
        o::SerializableObject::from_json_file(input_path, &error));
    if (o::is_error(error) || !object)
    {
        fail(
            "cannot read '" + input_path + "': "
            + (o::is_error(error) ? error.full_description
                                  : std::string("unknown error")));
        return 3;
    }

    auto timeline = dynamic_retainer_cast<o::Timeline>(object);
    if (!timeline)
    {
        fail("'" + input_path + "' does not contain a Timeline");
        return 3;
    }

    o::AudioRenderStatus status;
    o::AudioRenderer      renderer(timeline, media_root, rate, &status);
    if (!renderer.is_valid())
    {
        fail(
            "cannot render timeline: "
            + (status.message.empty() ? std::string("unknown error")
                                      : status.message));
        return 4;
    }

    int64_t const total_frames = renderer.frame_count();
    uint64_t const data_bytes  = uint64_t(total_frames) * 4u;

    std::string const partial_path = output_path + ".partial";
    std::ofstream     out(partial_path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        fail("cannot open '" + partial_path + "' for writing");
        std::remove(partial_path.c_str());
        return 5;
    }

    // RIFF header with an empty data-chunk size; patched at the end.  The
    // 16-byte fmt chunk describes stereo 16-bit PCM.
    out.write("RIFF", 4);
    put_u32(out, uint32_t(36u + data_bytes));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    put_u32(out, 16);
    put_u16(out, 1);                 // PCM
    put_u16(out, 2);                 // stereo
    put_u32(out, uint32_t(rate));    // sample rate
    put_u32(out, uint32_t(rate) * 4u); // byte rate
    put_u16(out, 4);                 // block align
    put_u16(out, 16);                // bits per sample
    out.write("data", 4);
    put_u32(out, uint32_t(data_bytes));

    std::vector<float> block(size_t(block_frames) * 2);
    int64_t            remaining = total_frames;
    bool               ok        = bool(out);

    while (ok && remaining > 0)
    {
        int64_t const request = std::min<int64_t>(block_frames, remaining);
        int64_t const got =
            renderer.read(request, block.data(), &status);
        if (got < 0)
        {
            fail(
                "render failed: "
                + (status.message.empty() ? std::string("unknown error")
                                          : status.message));
            ok = false;
            break;
        }
        if (got == 0)
        {
            break;
        }
        for (int64_t i = 0; i < got; ++i)
        {
            put_u16(out, uint16_t(quantize(block[size_t(i) * 2])));
            put_u16(out, uint16_t(quantize(block[size_t(i) * 2 + 1])));
        }
        remaining -= got;
    }

    if (ok && remaining != 0)
    {
        fail("render ended before the whole timeline was produced");
        ok = false;
    }

    out.flush();
    if (!out)
    {
        fail("failed writing '" + partial_path + "'");
        ok = false;
    }
    out.close();

    if (!ok)
    {
        std::remove(partial_path.c_str());
        return 6;
    }

    if (std::rename(partial_path.c_str(), output_path.c_str()) != 0)
    {
        fail("cannot move the finished WAV into place");
        std::remove(partial_path.c_str());
        return 7;
    }

    return 0;
}
