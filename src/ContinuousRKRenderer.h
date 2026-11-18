/*
ContinuousRKRenderer.h

Continuous-time, double-precision rendering of the four-stage Moog ladder
model from RKSimulationModel.h.  Source PCM is linearly reconstructed between
source sample points (so the filter keeps being integrated *between* source
frames) and the fourth ladder state is sampled at an independent target rate.

The shared derivative core below uses the exact same equation structure as
RKSimulationMoog::calculateDerivatives in RKSimulationModel.h:

    y1' = k * (S(x - r * y4) - S(y1))
    y2' = k * (S(y1) - S(y2))
    y3' = k * (S(y2) - S(y3))
    y4' = k * (S(y3) - S(y4))

with k = 2*pi*cutoff and the saturation curve

    S(z) = 3 * (v - v*v*v/3),  v = clamp(z / 3, -1, 1).

Everything in this renderer is evaluated in double.  The legacy
RKSimulationMoog class is untouched and keeps its float saturation rounding,
default output, fixed-factor Oversampler and original interface.
*/

#pragma once

#ifndef CONTINUOUS_RK_RENDERER_H
#define CONTINUOUS_RK_RENDERER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace MoogLadders {

struct ControlPoint
{
    uint64_t frame;       // absolute source frame index
    double cutoff;        // Hz
    double resonance;
};

struct ProcessResult
{
    std::size_t consumed;
    std::size_t produced;
    uint64_t first;       // absolute target frame index of first output
    bool done;
};

// Shared double-precision derivative core for the four-stage ladder.
// Same equation structure as RKSimulationModel.h, all arithmetic in double.
inline void continuousRKDerivatives(double input, double k, double resonance,
                                    const double state[4], double dstate[4])
{
    auto saturation = [](double z) -> double
    {
        double v = z / 3.0;
        if (v > 1.0)
            v = 1.0;
        else if (v < -1.0)
            v = -1.0;
        return 3.0 * (v - (v * v * v) / 3.0);
    };

    const double sat0 = saturation(state[0]);
    const double sat1 = saturation(state[1]);
    const double sat2 = saturation(state[2]);
    const double sat3 = saturation(state[3]);

    dstate[0] = k * (saturation(input - resonance * state[3]) - sat0);
    dstate[1] = k * (sat0 - sat1);
    dstate[2] = k * (sat1 - sat2);
    dstate[3] = k * (sat2 - sat3);
}

// Streaming renderer that integrates the continuous ladder model from a
// source-rate, planar float PCM stream and emits the fourth ladder state at
// an independent target rate.
class ContinuousRKRenderer
{
public:
    static constexpr uint32_t kMinRate = 8000;
    static constexpr uint32_t kMaxRate = 192000;
    static constexpr std::size_t kMaxBlock = 4096;
    static constexpr uint64_t kMaxFramePosition = uint64_t(1) << 55;

    ContinuousRKRenderer(uint32_t sourceRate,
                         uint32_t targetRate,
                         std::size_t channels,
                         uint64_t sourceOrigin,
                         std::vector<ControlPoint> controls,
                         double tolerance = 1e-8,
                         uint32_t maxDepth = 20);

    // Feeds a planar float block (frames consecutive source frames) and emits
    // up to capacity planar target frames.  See task specification for the
    // streaming contract; throws std::exception derivatives on illegal use
    // or exhausted adaptive-step depth (states roll back on the latter).
    ProcessResult process(const float* const* input,
                          std::size_t frames,
                          bool end,
                          float* const* output,
                          std::size_t capacity);

    // Read-only diagnostics and coordinates.
    uint64_t acceptedSteps() const { return acceptedSteps_; }
    uint64_t rejectedSteps() const { return rejectedSteps_; }
    uint64_t derivativeCalls() const { return derivativeCalls_; }
    uint64_t nextSourceFrame() const { return nextIn_; }
    uint64_t nextOutputFrame() const { return nextOut_; }

    uint32_t sourceRate() const { return sourceRate_; }
    uint32_t targetRate() const { return targetRate_; }
    std::size_t channels() const { return channels_; }
    double tolerance() const { return tolerance_; }
    uint32_t maxDepth() const { return maxDepth_; }

    // Read one of the four integration states (stage in [0,3]) for a channel.
    double stateAt(std::size_t channel, std::size_t stage) const;
    void copyStates(std::size_t channel, double out[4]) const;

private:
    // A point on the source-frame time axis: time = anchor + num / den.
    // num is a local offset, 0 <= num < den.  Absolute frame numbers are
    // kept integer; only local differences are converted to double.
    struct TimePoint
    {
        uint64_t anchor;
        int64_t num;
        uint64_t den;
    };

    static int compareTime(uint64_t origin, const TimePoint& a,
                           const TimePoint& b);
    static TimePoint midPoint(uint64_t origin, const TimePoint& a,
                              const TimePoint& b);

    void integrateSegment(const TimePoint& a, const TimePoint& b);
    void adaptSegment(const TimePoint& a, const TimePoint& b, uint32_t depth);
    void rk4All(const TimePoint& a, const TimePoint& b,
                const double* inState, double* outState);
    void rk4Channel(std::size_t ch, const TimePoint& a, const TimePoint& b,
                    const double* inState, double* outState);
    void evaluateStage(std::size_t ch, const TimePoint& p,
                       const double state[4], double dstate[4]);

    TimePoint targetPoint(uint64_t targetFrame) const;

    // Piecewise-linear controls at a time point.
    void controlAt(const TimePoint& p, double& k, double& resonance);
    // Linearly reconstructed source sample for one channel at a time point.
    double inputAt(std::size_t ch, const TimePoint& p) const;
    float sampleAt(std::size_t ch, uint64_t absoluteFrame) const;
    void retainLastTwo(uint64_t reached);

    const uint32_t sourceRate_;
    const uint32_t targetRate_;
    const std::size_t channels_;
    const uint64_t origin_;
    const double tolerance_;
    const uint32_t maxDepth_;
    std::vector<ControlPoint> controls_;

    // Live integration states, four per channel.
    std::vector<double> state_;

    // Preallocated work buffers (no heap activity after construction).
    std::vector<double> snapshot_;
    std::vector<double> coarse_;
    std::vector<double> fine_;

    // At most two retained source endpoints between calls.
    std::vector<uint64_t> ringFrame_;
    std::vector<float> ringValue_;
    static constexpr uint64_t kInvalidFrame = ~uint64_t(0);

    // Current batch window (valid only inside a process() call).
    const float* const* batchInput_ = nullptr;
    uint64_t batchStart_ = 0;
    std::size_t batchFrames_ = 0;
    uint64_t batchTerminal_ = kInvalidFrame;

    uint64_t received_;       // one past the last frame ever made available
    uint64_t nextIn_;         // next source endpoint frame to integrate to
    uint64_t nextOut_;        // next target frame to emit
    TimePoint cursor_;        // time coordinate the states correspond to

    bool endLatched_ = false;
    bool done_ = false;
    uint64_t terminalFrame_ = kInvalidFrame;

    uint64_t acceptedSteps_ = 0;
    uint64_t rejectedSteps_ = 0;
    uint64_t derivativeCalls_ = 0;
};

} // namespace MoogLadders

#endif // CONTINUOUS_RK_RENDERER_H
