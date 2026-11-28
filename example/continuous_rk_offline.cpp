// Offline usage example for MoogLadders::ContinuousRKRenderer.
//
// This is a self-contained, file-less demo: it synthesizes a planar multi
// channel source buffer, runs the continuous-time RK Moog ladder through the
// streaming process() entry point in a single offline pass, and prints a few
// rendered samples and the adaptive-step statistics.
//
// Build (from the repository root), C++17 standard library only:
//   c++ -std=c++17 -O2 -ffp-contract=off -I. \
//       task_src/ContinuousRKRenderer.cpp example/continuous_rk_offline.cpp \
//       -o continuous_rk_offline

#include "src/ContinuousRKRenderer.h"

#include <cmath>
#include <cstdio>
#include <vector>

int main()
{
    constexpr uint32_t kSourceRate = 44100;
    constexpr uint32_t kTargetRate = 48000;
    constexpr size_t kChannels = 2;
    constexpr size_t kFrames = 1024;
    constexpr uint64_t kOrigin = 0;

    // Planar float PCM, one buffer per channel.
    std::vector<std::vector<float>> source(kChannels, std::vector<float>(kFrames));
    for (size_t i = 0; i < kFrames; ++i)
    {
        source[0][i] = static_cast<float>(0.7 * std::sin(0.05 * i) +
                                          (i == 100 ? 4.0 : 0.0));
        source[1][i] = static_cast<float>(0.5 * std::cos(0.031 * i));
    }

    // A slow cutoff/resonance sweep given as absolute source-frame breakpoints.
    std::vector<MoogLadders::ControlPoint> controls = {
        {kOrigin, 400.0, 1.2},
        {kOrigin + 400, 2200.0, 2.8},
        {kOrigin + 800, 700.0, 0.6},
    };

    MoogLadders::ContinuousRKRenderer renderer(
        kSourceRate, kTargetRate, kChannels, kOrigin, std::move(controls),
        1e-9, 20);

    const float *input[kChannels];
    for (size_t ch = 0; ch < kChannels; ++ch)
        input[ch] = source[ch].data();

    std::vector<std::vector<float>> output(kChannels,
                                           std::vector<float>(kFrames * 2));
    float *outputPtr[kChannels] = {output[0].data(), output[1].data()};

    // One offline pass: hand over the whole source stream with end == true.
    MoogLadders::ProcessReport report =
        renderer.process(input, kFrames, true, outputPtr, output[0].size());

    if (!report.done)
    {
        // The output buffer was too small; keep calling with zero source frames
        // to drain the remaining target samples until report.done becomes true.
        size_t produced = report.produced;
        while (!report.done)
        {
            report = renderer.process(nullptr, 0, false, outputPtr,
                                      output[0].size() - produced);
            for (size_t ch = 0; ch < kChannels; ++ch)
                outputPtr[ch] += report.produced;
            produced += report.produced;
        }
    }

    std::printf("first output frame: %llu\n",
                static_cast<unsigned long long>(report.first));
    std::printf("accepted steps:     %llu\n",
                static_cast<unsigned long long>(renderer.acceptedSteps()));
    std::printf("rejected steps:     %llu\n",
                static_cast<unsigned long long>(renderer.rejectedSteps()));
    std::printf("derivative calls:   %llu\n",
                static_cast<unsigned long long>(renderer.derivativeCalls()));
    std::printf("final stage-0 state per channel: %.9f %.9f\n",
                renderer.stageState(0, 3), renderer.stageState(1, 3));
    return 0;
}
