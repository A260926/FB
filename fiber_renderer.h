#ifndef FIBER_RENDERER_H
#define FIBER_RENDERER_H
// Fiber transport: nearest-hit traversal over fiber side surfaces and the
// original Sphere geometry (spheres are pure black occluders here), with a
// caller-supplied directional background and a deterministic sample stream.
#include "fiber.h"
#include <cstdint>
#include <functional>
#include <vector>

class FiberSampleStream {
public:
    virtual ~FiberSampleStream() = default;
    // One deterministic sample in [0,1). Must never retry internally:
    // every scattering event consumes exactly three draws in a fixed order.
    virtual double next() = 0;
    // Total number of draws handed out so far, so callers and tests can
    // verify the sample budget of a path.
    virtual std::uint64_t consumed() const = 0;
    void next3(double& u1, double& u2, double& u3) {
        u1 = next(); u2 = next(); u3 = next();
    }
};

// Counter-based deterministic stream: a fixed-point LCG with a caller-chosen
// seed. Same seed and same number of draws always reproduce the same path.
class DeterministicStream : public FiberSampleStream {
public:
    explicit DeterministicStream(std::uint64_t seed = 0x9E3779B97F4A7C15ull)
        : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    double next() override {
        // 64-bit MMIX LCG constants.
        state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
        ++draws_;
        // Top 53 bits -> a double in [0,1).
        return double(state_ >> 11) * (1.0 / 9007199254740992.0);
    }
    std::uint64_t consumed() const override { return draws_; }
private:
    std::uint64_t state_;
    std::uint64_t draws_ = 0;
};

struct FiberScene {
    std::vector<Fiber>  fibers;
    std::vector<Sphere> occluders; // original sphere type, treated as black
};

// Rejects non-finite data, zero-length or invalid fibers, bad radii and
// out-of-range material parameters before any ray is traced.
bool fiber_scene_valid(const FiberScene& scene);

using BackgroundFn = std::function<dvec3(const dvec3& dir)>;

// Trace one unit ray.
//  * a sphere hit in front returns black (zero);
//  * no geometry hit returns background(dir);
//  * a fiber hit performs one material event (the whole internal chord is
//    folded into the four-lobe attenuation), multiplies the per-channel
//    weight by K/pdf and continues along the sampled direction;
//  * only the fiber just processed is skipped by the next segment, other
//    fibers and all spheres still participate, so a fiber can be hit again
//    after scattering off another one;
//  * the scatter count only counts fiber events; if the budget is spent at
//    a fiber hit the result is zero, while a clean escape still returns the
//    background;
//  * the minimum world intersection distance is 0.001; exact ties go to the
//    sphere, then to the fiber with the smaller id.
// Returns zero for invalid scenes or non-finite/non-unit rays.
dvec3 trace_fiber(const FiberScene& scene,
                  const dvec3& origin, const dvec3& dir,
                  int max_scatters,
                  FiberSampleStream& stream,
                  const BackgroundFn& background);

#endif // FIBER_RENDERER_H
