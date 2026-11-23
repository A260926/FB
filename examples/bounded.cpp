////////////////////////////////////////////////////////////////////////////////
///
/// bounded.cpp - example playback-thread driver for SoundTouch's bounded
/// processing mode. A six-channel float stream is produced in chunks and
/// pushed through processBounded() into a small fixed output buffer; frames
/// that were not consumed are resent on the next iteration. The resulting
/// interleaved PCM is written to .task-build/bounded.pcm.
///
////////////////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "SoundTouch.h"

using namespace soundtouch;

namespace
{
    constexpr uint kChannels = 6;
    constexpr uint kSampleRate = 48000;
    constexpr uint kStreamFrames = 48000; // one second of six-channel audio
    constexpr uint kInputChunk = 512;
    constexpr uint kOutputFrames = 256;
}

// Generates one non-periodic six-channel frame. Each channel carries a
// different harmonic mixture plus an occasional impulse so the file is not a
// trivial steady tone.
static float frameSample(uint frame, uint channel)
{
    float v = 0.28f * std::sin(frame * 0.031f + channel * 0.73f)
            + 0.12f * std::sin(frame * 0.113f + channel * 1.9f)
            + 0.05f * std::sin(frame * frame * 0.000011f + channel);
    if ((frame + channel * 211u) % (1753u + 97u * channel) == 0)
    {
        v += 0.45f * (1.0f - 0.1f * static_cast<float>(channel));
    }
    return v;
}

int main()
{
    SoundTouch touch;
    touch.setSampleRate(kSampleRate);
    touch.setChannels(kChannels);
    touch.setRate(1.2f);
    touch.setTempo(0.9f);
    touch.setSetting(SETTING_USE_AA_FILTER, 1);
    touch.prepareBounded();

    std::vector<float> out(kOutputFrames * kChannels);

    FILE *file = std::fopen(".task-build/bounded.pcm", "wb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "cannot open .task-build/bounded.pcm for writing\n");
        return 1;
    }

    uint generated = 0;   // frames synthesized so far
    uint consumedTotal = 0;
    uint producedTotal = 0;
    bool finished = false;

    while (!finished)
    {
        // Build the next input chunk on demand.
        uint offer = std::min(kInputChunk, kStreamFrames - generated);
        std::vector<float> in(offer * kChannels);
        for (uint f = 0; f < offer; ++f)
            for (uint c = 0; c < kChannels; ++c)
                in[f * kChannels + c] = frameSample(generated + f, c);

        bool lastInput = (generated + offer == kStreamFrames);
        uint inCursor = 0;

        // Keep resending whatever the pipeline could not accept until this
        // chunk is fully consumed, draining output between attempts.
        while (!finished)
        {
            BoundedResult r = touch.processBounded(
                inCursor < offer ? in.data() + inCursor * kChannels : nullptr,
                offer - inCursor,
                out.data(), kOutputFrames,
                lastInput && inCursor == offer);

            if (r.produced)
            {
                std::fwrite(out.data(), sizeof(float),
                            static_cast<size_t>(r.produced) * kChannels, file);
                producedTotal += r.produced;
            }
            inCursor += r.consumed;
            consumedTotal += r.consumed;
            generated += r.consumed;
            finished = r.finished;

            if (inCursor == offer || finished) break;
        }
    }

    std::fclose(file);

    uint expected = static_cast<uint>(
        std::floor(kStreamFrames / (1.2 * 0.9) + 0.5));
    std::printf("bounded example: input=%u consumed=%u produced=%u expected=%u channels=%u\n",
                kStreamFrames, consumedTotal, producedTotal, expected, kChannels);
    std::printf("wrote .task-build/bounded.pcm (%zu bytes)\n",
                static_cast<size_t>(producedTotal) * kChannels * sizeof(float));

    return (consumedTotal == kStreamFrames && producedTotal == expected) ? 0 : 2;
}
