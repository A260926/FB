#pragma once

#ifndef CONTINUOUS_RK_RENDERER_H
#define CONTINUOUS_RK_RENDERER_H

#include "MoogUtils.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace MoogLadders
{

/*
Shared four-stage Moog ladder equation structure.

The legacy RKSimulationMoog (RKSimulationModel.h) and the double precision
ContinuousRKRenderer below integrate the same differential equations:

    y1' = k * (S(x - r * y4) - S(y1))
    y2' = k * (S(y1) - S(y2))
    y3' = k * (S(y2) - S(y3))
    y4' = k * (S(y3) - S(y4))

where k = 2*pi*cutoff, r is the resonance and S is the saturation curve.
The saturation callable is injected so the legacy float-rounded clip and the
new double precision saturation can share the exact same equation wiring.
*/
template <class Saturation>
inline void rkLadderDerivatives(double input, double k, double resonance,
                                const double state[4], double dstate[4],
                                Saturation saturate)
{
    const double sat0 = saturate(state[0]);
    const double sat1 = saturate(state[1]);
    const double sat2 = saturate(state[2]);

    dstate[0] = k * (saturate(input - resonance * state[3]) - sat0);
    dstate[1] = k * (sat0 - sat1);
    dstate[2] = k * (sat1 - sat2);
    dstate[3] = k * (sat2 - saturate(state[3]));
}

// S(z) = 3 * (v - v*v*v/3), v = clamp(z / 3, -1, 1), evaluated in double.
inline double continuousRKSaturation(double z)
{
    double v = z / 3.0;
    if (v > 1.0)
        v = 1.0;
    else if (v < -1.0)
        v = -1.0;
    return 3.0 * (v - (v * v * v) / 3.0);
}

// A cutoff/resonance breakpoint located at an absolute source frame.
struct ControlPoint
{
    uint64_t frame;
    double cutoff;
    double resonance;
};

// Result of one streaming process() call.
struct ProcessReport
{
    size_t consumed;   // source frames read from the input pointers
    size_t produced;   // target frames written to the output pointers
    uint64_t first;    // absolute index of the first output (or the next one)
    bool done;         // the end of the source stream has been fully rendered
};

// Errors raised for illegal configuration/counts or exhausted step depth.
class ContinuousRKError : public std::runtime_error
{
public:
    explicit ContinuousRKError(const char *message) : std::runtime_error(message) {}
};

class StepDepthExceeded : public ContinuousRKError
{
public:
    StepDepthExceeded() : ContinuousRKError("ContinuousRKRenderer: adaptive step depth exhausted") {}
};

/*
Continuous-time renderer for the four-stage RK Moog ladder.

Source PCM (float, planar multi-channel) is linearly reconstructed between
adjacent source frames and the ladder ODE is integrated in double precision,
stopping at source segment endpoints and target sample instants.  The fourth
stage state is emitted as float at each target sample time.

All clock arithmetic (source/target ordering, first/last frame numbers) uses
integer arithmetic; only local frame differences are ever converted to
double for integration and interpolation.
*/
class ContinuousRKRenderer
{
public:
    static constexpr uint32_t kMinRate = 8000;
    static constexpr uint32_t kMaxRate = 192000;
    static constexpr size_t kMaxBlock = 4096;
    static constexpr uint64_t kMaxFramePosition = uint64_t(1) << 55;

    ContinuousRKRenderer(uint32_t sourceRate,
                         uint32_t targetRate,
                         size_t channels,
                         uint64_t sourceOrigin,
                         std::vector<ControlPoint> controls,
                         double tolerance = 1e-8,
                         uint32_t maxDepth = 20);

    ProcessReport process(const float *const *input, size_t frames, bool end,
                          float *const *output, size_t capacity);

    // Read-only diagnostic counters.
    uint64_t acceptedSteps() const noexcept { return acceptedSteps_; }
    uint64_t rejectedSteps() const noexcept { return rejectedSteps_; }
    uint64_t derivativeCalls() const noexcept { return derivativeCalls_; }

    // Absolute coordinates of the next source frame to receive and the next
    // target frame to emit.
    uint64_t nextSourceFrame() const noexcept { return nextSource_; }
    uint64_t nextOutputFrame() const noexcept { return nextOutput_; }

    // Current integration state of one channel's four ladder stages.
    double stageState(size_t channel, size_t stage) const;

    size_t channels() const noexcept { return channels_; }
    bool finished() const noexcept { return done_; }

private:
    struct Endpoint
    {
        uint64_t frame = 0;
        double value[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    };

    void validateCall(const float *const *input, size_t frames,
                      const float *const *output, size_t capacity) const;
    void ingestInput(const float *const *input, size_t index);
    void ingestZeroEndpoint();
    void advanceControlHint(uint64_t frame);
    void controlsAt(uint64_t segmentFrame, double u, double &cutoff, double &resonance) const;
    double inputAt(size_t channel, double u, const Endpoint &a, const Endpoint &b) const;
    void evaluateDerivatives(size_t channel, double u, const double state[4],
                             double dstate[4], const Endpoint &a,
                             const Endpoint &b, uint64_t segmentFrame);
    void rk4Step(size_t channel, const double state[4], double u0, double du,
                 double out[4], const Endpoint &a, const Endpoint &b,
                 uint64_t segmentFrame);
    void adaptiveInterval(double u0, double u1, int depth,
                          const Endpoint &a, const Endpoint &b,
                          uint64_t segmentFrame);
    void renderSegment(float *const *output, size_t capacity, size_t &produced);

    uint32_t sourceRate_;
    uint32_t targetRate_;
    size_t channels_;
    uint64_t nextSource_;
    uint64_t nextOutput_;
    double tolerance_;
    uint32_t maxDepth_;

    std::vector<ControlPoint> controls_;
    size_t controlHint_ = 0;

    double state_[6][4] = {};

    Endpoint buffer_[2];
    int buffered_ = 0;

    // State of the source segment currently being integrated.  A segment can
    // be paused when the output capacity fills and resumed on the next call.
    // segmentSnapshot_ holds every channel's state at the moment the segment
    // opened, so a depth failure rolls the whole integration segment back.
    bool segmentActive_ = false;
    double segmentU_ = 0.0;
    uint64_t segmentFrame_ = 0;
    double segmentSnapshot_[6][4] = {};

    bool endLatched_ = false;
    bool zeroEndpointServed_ = false;
    bool done_ = false;
    uint64_t endFrame_ = 0;

    uint64_t acceptedSteps_ = 0;
    uint64_t rejectedSteps_ = 0;
    uint64_t derivativeCalls_ = 0;
};

} // namespace MoogLadders

#endif // CONTINUOUS_RK_RENDERER_H
