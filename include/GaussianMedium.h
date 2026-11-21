#ifndef GAUSSIAN_MEDIUM_H
#define GAUSSIAN_MEDIUM_H

#include "util/Utilities.h"
#include "util/Ray.h"
#include "util/Interval.h"
#include "util/Radiance.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// A single anisotropic Gaussian participating-medium kernel.
//
// For x, q(x) = (x-mean)^T C^-1 (x-mean); the kernel has finite support q <= 9
// where g(x) = exp(-q/2), and g = 0 outside that ellipsoid.
//   sigma(x) = sum_i extinction_i * g_i(x)
//   j(x)     = sum_i extinction_i * g_i(x) * source_i(x)
// `covariance` stores the symmetric world-space positive-definite matrix C as
// (xx, xy, xz, yy, yz, zz). `source` is an RGB source per unit extinction.
struct GaussianKernel {
    Point3 mean = Point3(0, 0, 0);
    std::array<double, 6> covariance = {1, 0, 0, 1, 0, 1};
    double extinction = 0.0;
    Radiance source = Radiance(0, 0, 0);
};

// Per-query instrumentation. Counters are reset at the start of every query.
struct MediumStats {
    // Number of actual ray vs kernel support-ellipsoid tests, including every
    // test performed while gathering BVH candidates.
    std::uint64_t kernel_tests = 0;
    // Number of intervals actually evaluated by the adaptive emission
    // integrator. Optical-thickness line integrals are not counted here.
    std::uint64_t interval_evaluations = 0;
};

// Output of a successful segment query.
struct SegmentResult {
    double transmittance = 1.0;       // exp(-tau), finite, in [0, 1]
    Radiance emission = Radiance(0, 0, 0);   // E, RGB emission integral value
    Radiance error = Radiance(0, 0, 0);      // conservative per-channel |error| bound
};

// Owning three-channel camera frame. value/error are linear RGB,
// transmittance is a scalar per pixel (first camera segment only).
struct GaussianFrame {
    int width = 0;
    int height = 0;
    std::vector<Radiance> value;
    std::vector<Radiance> error;
    std::vector<double> transmittance;
};

// A collection of interpenetrating Gaussian medium kernels with its own
// spatial acceleration hierarchy. Queries are read-only; reconfiguration
// validates a full candidate snapshot before replacing the live one.
class GaussianMedium {
  public:
    GaussianMedium();
    explicit GaussianMedium(std::vector<GaussianKernel> kernels);

    // Validates every kernel first; throws std::invalid_argument and keeps the
    // previous snapshot intact when any kernel (or derived quantity) is invalid.
    void set_kernels(std::vector<GaussianKernel> kernels);

    std::size_t size() const;
    bool empty() const;

    // Integrate extinction and emission along ray over interval [a, b]
    // (b may be +infinity). On success returns true and fills result:
    // finite transmittance, RGB emission and a conservative per-channel
    // absolute error bound not exceeding abs_error. Returns false when the
    // max_work interval budget is exhausted; in that case result is untouched.
    // Illegal arguments throw std::invalid_argument (result untouched).
    bool segment(const Ray& ray, const Interval& interval, double abs_error,
                 std::uint64_t max_work, SegmentResult& result,
                 MediumStats& stats) const;

    // Extinction-only query over the interval: T = exp(-tau). Never performs
    // emission integration and so never consumes the emission interval budget.
    double transmittance(const Ray& ray, const Interval& interval,
                         MediumStats& stats) const;

  private:
    struct Snapshot;
    std::shared_ptr<const Snapshot> snapshot;
};

#endif // GAUSSIAN_MEDIUM_H
