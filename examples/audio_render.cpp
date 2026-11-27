// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

//
// audio_render: render an OTIO timeline's PCM audio tracks to a stereo WAV.
//
// Usage:
//   audio_render input.otio media_root output.wav rate block_frames
//
// Reads a real JSON timeline, streams it through the public AudioRenderer
// interface in fixed blocks and writes a 16-bit stereo PCM WAVE. On any error
// the process exits non-zero and removes the partially written output file.
//

#include "opentimelineio/audioRender.h"
#include "opentimelineio/errorStatus.h"
#include "opentimelineio/serializableObject.h"
#include "opentimelineio/timeline.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace o = opentimelineio::OPENTIMELINEIO_VERSION_NS;

namespace {

int
fail_and_remove(std::string const& output_path, std::string const& message)
{
    std::fprintf(stderr, "audio_render: %s\n", message.c_str());
    std::remove(output_path.c_str());
    return 1;
}

} // namespace

int
main(int argc, char** argv)
{
    if (argc != 6)
    {
        std::fprintf(
            stderr,
            "usage: %s input.otio media_root output.wav rate block_frames\n",
            argc > 0 ? argv[0] : "audio_render");
        return 2;
    }

    std::string input_path  = argv[1];
    std::string media_root  = argv[2];
    std::string output_path = argv[3];

    long parsed_rate    = std::strtol(argv[4], nullptr, 10);
    long parsed_block   = std::strtol(argv[5], nullptr, 10);
    if (parsed_rate <= 0 || parsed_block <= 0)
    {
        return fail_and_remove(
            output_path, "rate and block_frames must be positive integers");
    }
    int     output_rate  = int(parsed_rate);
    int64_t block_frames = parsed_block;

    // Load the JSON timeline.
    o::ErrorStatus error;
    o::SerializableObject::Retainer<o::SerializableObject> loaded(
        o::SerializableObject::from_json_file(input_path, &error));
    if (o::is_error(error) || !loaded)
    {
        return fail_and_remove(
            output_path,
            "cannot load timeline '" + input_path + "': "
                + error.full_description);
    }
    auto* timeline = dynamic_cast<o::Timeline*>(loaded.value);
    if (!timeline)
    {
        return fail_and_remove(
            output_path, "input JSON is not a Timeline: " + input_path);
    }

    // The renderer takes its own deep snapshot and opens media lazily.
    o::AudioRenderer renderer(*timeline, media_root, output_rate, &error);
    if (o::is_error(error))
    {
        return fail_and_remove(
            output_path, "renderer setup failed: " + error.full_description);
    }

    // Opening the writer truncates/creates the output immediately; if anything
    // below fails the file is removed on the way out.
    o::PcmWaveWriter writer(output_path, output_rate, &error);
    if (o::is_error(error))
    {
        return fail_and_remove(
            output_path, "cannot open output: " + error.full_description);
    }

    // Repeatedly call the same public streaming interface.
    std::vector<float> block;
    while (renderer.position() < renderer.frame_count())
    {
        if (!renderer.read(block_frames, block, &error))
        {
            return fail_and_remove(
                output_path, "render failed: " + error.full_description);
        }
        if (block.empty())
        {
            break; // reached end of stream
        }
        if (!writer.write_frames(block, &error))
        {
            return fail_and_remove(
                output_path, "write failed: " + error.full_description);
        }
    }

    if (!writer.close(&error))
    {
        return fail_and_remove(
            output_path, "finalize failed: " + error.full_description);
    }

    std::printf(
        "wrote %lld stereo frames at %d Hz to %s\n",
        static_cast<long long>(writer.frame_count()),
        output_rate,
        output_path.c_str());
    return 0;
}
