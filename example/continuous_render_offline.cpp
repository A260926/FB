/*
continuous_render_offline.cpp

Offline usage example for MoogLadders::ContinuousRKRenderer.

A synthetic planar PCM signal (a swept tone with a few hard transients) is
rendered through the continuous four-stage ladder at a different sample rate.
The source is fed in small streaming blocks; the renderer integrates the
continuous model between source frames and emits the fourth ladder state on
the independent target clock.  Everything below uses only the C++17 standard
library.

Build (from the repository root):
    c++ -std=c++17 -O2 -I. \
        example/continuous_render_offline.cpp \
        task_src/ContinuousRKRenderer.cpp -o continuous_render_offline
*/

#include "src/ContinuousRKRenderer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

int main()
{
    using MoogLadders::ContinuousRKRenderer;
    using MoogLadders::ControlPoint;

    const uint32_t sourceRate = 44100;
    const uint32_t targetRate = 48000;
    const std::size_t channels = 2;
    const std::size_t sourceFrames = 2048;
    const uint64_t origin = 0;

    // A cutoff/resonance sweep given on the absolute source-frame timeline.
    // The first point must coincide with the origin; later points are joined
    // by linear interpolation and the final value is held.
    std::vector<ControlPoint> controls = {
        {origin + 0, 400.0, 0.8},
        {origin + 700, 3200.0, 3.0},
        {origin + 1400, 600.0, 1.4},
        {origin + sourceFrames, 1500.0, 2.0}};

    // Build a planar, finite, |x| <= 6 source signal.
    std::vector<std::vector<float>> source(
        channels, std::vector<float>(sourceFrames));
    for (std::size_t c = 0; c < channels; ++c)
        for (std::size_t i = 0; i < sourceFrames; ++i)
        {
            double v = 1.6 * std::sin(0.04 * i + c) +
                       0.8 * std::sin(0.21 * i);
            if (i % 97 == 10)
                v += (c == 0 ? 5.0 : -5.0); // transient at the legal limit
            v = std::max(-6.0, std::min(6.0, v));
            source[c][i] = float(v);
        }

    ContinuousRKRenderer renderer(sourceRate, targetRate, channels, origin,
                                  std::move(controls),
                                  /*tolerance=*/1e-9,
                                  /*maxDepth=*/20);

    // Stream the source in small blocks and collect every target frame.
    const std::size_t inputChunk = 37;
    const std::size_t outputCapacity = 64;
    std::vector<std::vector<float>> rendered(channels);

    std::size_t fed = 0;
    while (true)
    {
        std::size_t take = std::min(inputChunk, sourceFrames - fed);
        bool end = (fed + take == sourceFrames);

        std::vector<const float*> in(channels);
        for (std::size_t c = 0; c < channels; ++c)
            in[c] = source[c].data() + fed;

        std::vector<std::vector<float>> block(
            channels, std::vector<float>(outputCapacity));
        std::vector<float*> out(channels);
        for (std::size_t c = 0; c < channels; ++c)
            out[c] = block[c].data();

        MoogLadders::ProcessResult result =
            renderer.process(in.data(), take, end, out.data(),
                             outputCapacity);

        for (std::size_t c = 0; c < channels; ++c)
            rendered[c].insert(rendered[c].end(), block[c].begin(),
                               block[c].begin() + result.produced);
        fed += result.consumed;

        if (result.done)
            break;

        // If the whole source was delivered but the end is not latched yet
        // (a partial final consumption), re-feed the remaining suffix rather
        // than inventing new samples.
        if (fed < sourceFrames)
            continue;
        std::vector<float*> drain(channels);
        for (std::size_t c = 0; c < channels; ++c)
            drain[c] = block[c].data();
        MoogLadders::ProcessResult tail =
            renderer.process(nullptr, 0, false, drain.data(),
                             outputCapacity);
        for (std::size_t c = 0; c < channels; ++c)
            rendered[c].insert(rendered[c].end(), block[c].begin(),
                               block[c].begin() + tail.produced);
        if (tail.done)
            break;
    }

    std::printf("rendered %zu target frames per channel\n",
                rendered[0].size());
    std::printf("next source frame: %llu, next output frame: %llu\n",
                (unsigned long long)renderer.nextSourceFrame(),
                (unsigned long long)renderer.nextOutputFrame());
    std::printf("accepted=%llu rejected=%llu derivativeCalls=%llu\n",
                (unsigned long long)renderer.acceptedSteps(),
                (unsigned long long)renderer.rejectedSteps(),
                (unsigned long long)renderer.derivativeCalls());

    double level[2] = {0.0, 0.0};
    for (std::size_t c = 0; c < channels; ++c)
        for (float v : rendered[c])
            level[c] += double(v) * double(v);
    std::printf("channel energy: %.6f %.6f\n", level[0], level[1]);

    // The four ladder states remain readable for inspection.
    double states[4];
    renderer.copyStates(0, states);
    std::printf("final channel-0 ladder states: %.6f %.6f %.6f %.6f\n",
                states[0], states[1], states[2], states[3]);
    return 0;
}
