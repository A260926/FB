/*
    Offline multi-track render through real background read-ahead and the
    atomic BufferedAudioTimeline.

    Generates two aperiodic in-memory tracks, wraps each in a
    BufferingAudioSource driven by a TimeSliceThread, mixes them through a
    BufferedAudioTimeline and writes 256 stereo f32 frames (interleaved) to
    <output directory>/atomic-mix.f32. The timeline is re-timed once halfway
    through.

    Usage: atomic_timeline_render <output directory>
*/

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    // A deterministic, deliberately non-periodic positionable source: two
    // incommensurate sines plus a hashed, sample-uncorrelated component.
    struct GeneratedSource : juce::PositionableAudioSource
    {
        const int id;
        const int channelCount;
        const juce::int64 length;
        juce::int64 position = 0;

        GeneratedSource (int id_, int channels, juce::int64 length_)
            : id (id_), channelCount (channels), length (length_) {}

        static float waveform (int id, juce::int64 n, int channel)
        {
            const auto x = (double) n;
            const auto tonal = 0.30 * std::sin (x * 0.013 + id * 0.7 + channel * 0.31);
            const auto overtone = 0.15 * std::sin (x * 0.0271 + id * 1.3);
            const auto hashed = (double) ((juce::uint32) (n * 1103515245u + id * 12345u) % 1009u) / 1009.0 - 0.5;

            return float (tonal + overtone + 0.08 * hashed);
        }

        void prepareToPlay (int, double) override { position = 0; }
        void releaseResources() override {}

        void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
        {
            for (int c = 0; c < info.buffer->getNumChannels(); ++c)
                for (int n = 0; n < info.numSamples; ++n)
                    info.buffer->setSample (c, info.startSample + n,
                                            waveform (id, position + n, c));

            position += info.numSamples;
        }

        void setNextReadPosition (juce::int64 p) override { position = p; }
        juce::int64 getNextReadPosition() const override { return position; }
        juce::int64 getTotalLength() const override { return length; }
        bool isLooping() const override { return false; }
    };

    bool renderBlock (juce::BufferedAudioTimeline& timeline,
                      juce::BufferingAudioSource& a, juce::BufferingAudioSource& b,
                      const juce::AudioSourceChannelInfo& info)
    {
        juce::AudioBuffer<float> probe (2, info.numSamples);
        juce::AudioSourceChannelInfo probeInfo (&probe, 0, info.numSamples);

        for (int attempt = 0; attempt < 100000; ++attempt)
        {
            // Use the existing waiting API to let the background threads catch
            // up; the actual consumption stays a single atomic group commit.
            a.waitForNextAudioBlockReady (probeInfo, 1000);
            b.waitForNextAudioBlockReady (probeInfo, 1000);

            if (timeline.tryRender (info))
                return true;

            juce::Thread::sleep (1);
        }

        return false;
    }
}

int main (int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf (stderr, "usage: %s <output directory>\n", argv[0]);
        return 2;
    }

    constexpr int channels = 2;
    constexpr juce::int64 sourceLength = 4096;
    constexpr int totalFrames = 256;
    constexpr int blockFrames = 32;
    constexpr int retimeAfter = 128;

    juce::TimeSliceThread worker ("atomic-render-prefetch");

    if (! worker.startThread())
    {
        std::fprintf (stderr, "failed to start prefetch thread\n");
        return 3;
    }

    GeneratedSource sourceA (1, channels, sourceLength);
    GeneratedSource sourceB (2, channels, sourceLength);

    juce::BufferingAudioSource bufferedA (&sourceA, worker, false, 4096, channels, false);
    juce::BufferingAudioSource bufferedB (&sourceB, worker, false, 4096, channels, false);

    juce::BufferedAudioTimeline timeline ({
        { &bufferedA, 0, 0.8f },
        { &bufferedB, 100, 0.6f }
    });

    if (! timeline.prepareToPlay (blockFrames, 48000.0))
    {
        std::fprintf (stderr, "prepareToPlay failed\n");
        worker.stopThread (5000);
        return 4;
    }

    juce::AudioBuffer<float> output (channels, totalFrames);

    for (int frame = 0; frame < totalFrames; frame += blockFrames)
    {
        if (frame == retimeAfter)
        {
            // Re-time once, keeping the current output position. Both new
            // starts must still leave room for the remaining frames.
            if (! timeline.retime ({ 37, 211 }))
            {
                std::fprintf (stderr, "retime failed at frame %d\n", frame);
                timeline.releaseResources();
                worker.stopThread (5000);
                return 5;
            }
        }

        juce::AudioSourceChannelInfo info (&output, frame, blockFrames);

        if (! renderBlock (timeline, bufferedA, bufferedB, info))
        {
            std::fprintf (stderr, "render failed at frame %d\n", frame);
            timeline.releaseResources();
            worker.stopThread (5000);
            return 6;
        }
    }

    timeline.releaseResources();
    worker.stopThread (5000);

    if (timeline.getOutputPosition() != totalFrames)
    {
        std::fprintf (stderr, "unexpected output position\n");
        return 7;
    }

    const std::string outputPath = std::string (argv[1]) + "/atomic-mix.f32";
    FILE* file = std::fopen (outputPath.c_str(), "wb");

    if (file == nullptr)
    {
        std::fprintf (stderr, "cannot open %s\n", outputPath.c_str());
        return 8;
    }

    // Interleave the two channels into raw host-order float32.
    std::vector<float> interleaved ((size_t) totalFrames * channels);

    for (int n = 0; n < totalFrames; ++n)
        for (int c = 0; c < channels; ++c)
            interleaved[(size_t) n * channels + c] = output.getSample (c, n);

    const auto written = std::fwrite (interleaved.data(), sizeof (float),
                                      interleaved.size(), file);
    std::fclose (file);

    if (written != interleaved.size())
    {
        std::fprintf (stderr, "short write to %s\n", outputPath.c_str());
        return 9;
    }

    std::printf ("ATOMIC_RENDER_OK frames=%d channels=%d bytes=%d path=%s\n",
                 totalFrames, channels, totalFrames * channels * (int) sizeof (float),
                 outputPath.c_str());
    return 0;
}
