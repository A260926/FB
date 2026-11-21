#include "task_support/check.h"

#include "GaussianMedium.h"
#include "Camera.h"
#include "object/ObjectList.h"
#include "object/Triangle.h"
#include "light/PointLight.h"
#include "light/LightList.h"
#include "light/RectangularLight.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <random>
#include <vector>

namespace {

constexpr double PI = 3.14159265358979323846;

constexpr double SQRT2 = 1.4142135623730950488;
constexpr double SQRT2PI = 2.5066282746310005024;

GaussianKernel make_kernel(const Point3& mean, double vxx, double vxy,
                           double vxz, double vyy, double vyz, double vzz,
                           double ext, const Radiance& src) {
    GaussianKernel k;
    k.mean = mean;
    k.covariance = {vxx, vxy, vxz, vyy, vyz, vzz};
    k.extinction = ext;
    k.source = src;
    return k;
}

// Analytic optical thickness of a 1D centered Gaussian line profile
// g(t) = exp(-t^2 / (2 var)) restricted to [lo, hi].
double analytic_line_tau(double ext, double var, double lo, double hi) {
    const double scale = std::sqrt(2.0 * var);
    return ext * std::sqrt(2.0 * PI * var) * 0.5 *
           (std::erf(hi / scale) - std::erf(lo / scale));
}

bool close(double a, double b, double eps) { return std::fabs(a - b) <= eps; }
bool close_rgb(const Radiance& a, const Radiance& b, double eps) {
    return close(a.x(), b.x(), eps) && close(a.y(), b.y(), eps) &&
           close(a.z(), b.z(), eps);
}

// ---------------------------------------------------------------------------
// Independent high-precision reference: direct 3D kernel evaluation with a
// generic matrix inverse and a fine trapezoid grid. No production code is used.
// ---------------------------------------------------------------------------

struct RefKernel {
    Point3 mean;
    double inv[9];
    double ext;
    Radiance src;
};

bool invert3(const double m[9], double out[9]) {
    double det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                 m[1] * (m[3] * m[8] - m[5] * m[6]) +
                 m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::fabs(det) < 1e-30) return false;
    double id = 1.0 / det;
    out[0] = (m[4] * m[8] - m[5] * m[7]) * id;
    out[1] = (m[2] * m[7] - m[1] * m[8]) * id;
    out[2] = (m[1] * m[5] - m[2] * m[4]) * id;
    out[3] = (m[5] * m[6] - m[3] * m[8]) * id;
    out[4] = (m[0] * m[8] - m[2] * m[6]) * id;
    out[5] = (m[2] * m[3] - m[0] * m[5]) * id;
    out[6] = (m[3] * m[7] - m[4] * m[6]) * id;
    out[7] = (m[1] * m[6] - m[0] * m[7]) * id;
    out[8] = (m[0] * m[4] - m[1] * m[3]) * id;
    return true;
}

RefKernel make_ref(const GaussianKernel& k) {
    RefKernel r;
    r.mean = k.mean;
    r.ext = k.extinction;
    r.src = k.source;
    double c[9] = {k.covariance[0], k.covariance[1], k.covariance[2],
                   k.covariance[1], k.covariance[3], k.covariance[4],
                   k.covariance[2], k.covariance[4], k.covariance[5]};
    invert3(c, r.inv);
    return r;
}

double ref_gaussian(const RefKernel& k, const Point3& x) {
    double d[3] = {x.x() - k.mean.x(), x.y() - k.mean.y(),
                   x.z() - k.mean.z()};
    double q = 0.0;
    for (int i = 0; i < 3; ++i) {
        double row = 0.0;
        for (int j = 0; j < 3; ++j) row += k.inv[i * 3 + j] * d[j];
        q += d[i] * row;
    }
    // Tolerance at the q = 9 boundary so a support node cannot snap to zero
    // through floating-point rounding; genuinely exterior points stay zero.
    return q <= 9.0 + 1.0e-9 ? std::exp(-0.5 * q) : 0.0;
}

struct RefResult {
    double transmittance;
    Radiance emission;
};

// Fine-grid integration over elementary smooth subintervals. Partitioning at
// every kernel support boundary removes the truncation discontinuities, so
// composite Simpson (and trapezoid tau accumulation) converge rapidly without
// reusing any production integration code.
RefResult reference_integrate(const std::vector<GaussianKernel>& kernels,
                              const Ray& ray, double lo, double hi) {
    std::vector<RefKernel> refs;
    refs.reserve(kernels.size());
    for (const auto& k : kernels) refs.push_back(make_ref(k));

    // Support entry/exit of every kernel.
    std::vector<double> breaks{lo, hi};
    for (const auto& k : refs) {
        Vector3 d = ray.direction();
        Vector3 mo = k.mean - ray.origin();
        double A = 0.0, B = 0.0, K = 0.0;
        for (int i = 0; i < 3; ++i) {
            double cd = 0.0, cm = 0.0;
            for (int j = 0; j < 3; ++j) {
                cd += k.inv[i * 3 + j] * d[j];
                cm += k.inv[i * 3 + j] * mo[j];
            }
            A += d[i] * cd;
            B += mo[i] * cd;
            K += mo[i] * cm;
        }
        double disc = B * B - A * (K - 9.0);
        if (disc < 0.0) continue;
        double sq = std::sqrt(disc) / A;
        double tc = B / A;
        double t0 = std::max(lo, tc - sq);
        double t1 = std::min(hi, tc + sq);
        if (t1 > t0) {
            breaks.push_back(t0);
            breaks.push_back(t1);
        }
    }
    std::sort(breaks.begin(), breaks.end());
    breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());

    const int n = 200000; // subdivisions per elementary interval
    const double ds = ray.direction().length();
    auto fields = [&](double t, double& sigma, Radiance& j) {
        Point3 x = ray.at(t);
        sigma = 0.0;
        j = Radiance(0, 0, 0);
        for (const auto& k : refs) {
            double g = ref_gaussian(k, x);
            sigma += k.ext * g;
            j += Radiance(k.ext * g * k.src.x(), k.ext * g * k.src.y(),
                          k.ext * g * k.src.z());
        }
    };

    double tau = 0.0;
    Radiance e(0, 0, 0);
    for (std::size_t bi = 0; bi + 1 < breaks.size(); ++bi) {
        double a2 = breaks[bi];
        double b2 = breaks[bi + 1];
        if (b2 <= a2) continue;
        double h = (b2 - a2) / n;
        double prev_sigma;
        Radiance prev_j;
        fields(a2, prev_sigma, prev_j);
        // Simpson's leading node carries coefficient 1.
        Radiance simpson = Radiance(std::exp(-tau), std::exp(-tau),
                                     std::exp(-tau)) * prev_j;
        for (int step = 1; step <= n; ++step) {
            double t = a2 + step * h;
            double sigma;
            Radiance j;
            fields(t, sigma, j);
            // Trapezoid accumulation of the (smooth here) optical depth.
            tau += 0.5 * (prev_sigma + sigma) * h * ds;
            double weight = std::exp(-tau);
            double coeff = (step == n) ? 1.0
                                       : (step % 2 == 1 ? 4.0 : 2.0);
            for (int c = 0; c < 3; ++c) simpson[c] += coeff * weight * j[c];
            prev_sigma = sigma;
            prev_j = j;
        }
        for (int c = 0; c < 3; ++c) e[c] += h * ds / 3.0 * simpson[c];
    }
    return RefResult{std::exp(-tau), e};
}

// Parameter range covering every kernel support along the ray.
bool reference_support_range(const std::vector<GaussianKernel>& kernels,
                             const Ray& ray, double a, double b,
                             double& lo, double& hi) {
    lo = b;
    hi = a;
    bool any = false;
    for (const auto& k : kernels) {
        RefKernel r = make_ref(k);
        Vector3 d = ray.direction();
        Vector3 mo = k.mean - ray.origin();
        double A = 0.0, B = 0.0, K = 0.0;
        for (int i = 0; i < 3; ++i) {
            double cd = 0.0, cm = 0.0;
            for (int j = 0; j < 3; ++j) {
                cd += r.inv[i * 3 + j] * d[j];
                cm += r.inv[i * 3 + j] * mo[j];
            }
            A += d[i] * cd;
            B += mo[i] * cd;
            K += mo[i] * cm;
        }
        double disc = B * B - A * (K - 9.0);
        if (disc < 0.0) continue;
        double sq = std::sqrt(disc) / A;
        double tc = B / A;
        double t0 = std::max(a, tc - sq);
        double t1 = std::min(b, tc + sq);
        if (t1 <= t0) continue;
        lo = std::min(lo, t0);
        hi = std::max(hi, t1);
        any = true;
    }
    return any;
}

Camera make_test_camera() {
    Camera cam;
    cam.image_width = 1;
    cam.image_height = 1;
    cam.lookfrom = Point3(0, 0, 0);
    cam.lookat = Point3(0, 0, -1);
    cam.vup = Vector3(0, 1, 0);
    cam.vfov = 60;
    cam.ambient_light = Radiance(0, 0, 0);
    cam.background = Radiance(0.1, 0.2, 0.3);
    cam.nbounces = 4;
    return cam;
}

std::shared_ptr<Triangle> make_plane(Material* mat) {
    return std::make_shared<Triangle>(Point3(-20, -20, -3),
                                      Point3(20, -20, -3),
                                      Point3(0, 20, -3), mat);
}

} // namespace

// ===========================================================================
// optical group
// ===========================================================================

static int optical_single_analytic() {
    const double var = 0.25;
    auto kernels = std::vector<GaussianKernel>{
        make_kernel(Point3(0, 0, 0), var, 0, 0, var, 0, var, 0.7,
                    Radiance(0, 0, 0))};
    GaussianMedium medium(kernels);

    Ray ray(Point3(-6, 0, 0), Vector3(1, 0, 0));
    MediumStats stats;
    double tau = analytic_line_tau(0.7, var, -3.0 * std::sqrt(var),
                                   3.0 * std::sqrt(var));
    double expected_T = std::exp(-tau);

    SegmentResult seg;
    TASK_CHECK(medium.segment(ray, Interval(0.0, 12.0), 1e-7, 1000, seg,
                              stats));
    TASK_CHECK(close(seg.transmittance, expected_T, 1e-10));
    TASK_CHECK(close(medium.transmittance(ray, Interval(0.0, 12.0), stats),
                     expected_T, 1e-10));
    TASK_CHECK(std::isfinite(seg.transmittance));
    TASK_CHECK(seg.emission.x() == 0.0 && seg.error.x() == 0.0);
    TASK_CHECK(stats.interval_evaluations == 0);
    TASK_CHECK(stats.kernel_tests >= 1);

    // Zero-length segment is an exact identity.
    SegmentResult zero;
    TASK_CHECK(medium.segment(ray, Interval(2.0, 2.0), 1e-7, 1000, zero,
                              stats));
    TASK_CHECK(zero.transmittance == 1.0 && zero.error.x() == 0.0);
    return 0;
}

static int optical_clipped_partial() {
    const double var = 1.0;
    GaussianMedium medium(std::vector<GaussianKernel>{
        make_kernel(Point3(0, 0, 0), var, 0, 0, var, 0, var, 1.3,
                    Radiance(0, 0, 0))});
    Ray ray(Point3(-6, 0, 0), Vector3(1, 0, 0));
    MediumStats stats;
    // The kernel support starts at x = -3 (t = 3); only [-3, 0.5] contributes.
    double tau = analytic_line_tau(1.3, var, -3.0, 0.5);
    SegmentResult seg;
    TASK_CHECK(medium.segment(ray, Interval(0.0, 6.5), 1e-8, 1000, seg,
                              stats));
    TASK_CHECK(close(seg.transmittance, std::exp(-tau), 1e-10));
    return 0;
}

static int optical_reverse_and_scale() {
    const double var = 0.36;
    auto kernels = std::vector<GaussianKernel>{
        make_kernel(Point3(0, 0, 0), var, 0, 0, var, 0, var, 0.9,
                    Radiance(1, 0.4, 0.2))};
    GaussianMedium medium(kernels);

    Ray forward(Point3(-6, 0, 0), Vector3(1, 0, 0));
    Ray reversed(Point3(6, 0, 0), Vector3(-1, 0, 0));
    Ray scaled(Point3(-6, 0, 0), Vector3(2, 0, 0));
    MediumStats stats;
    SegmentResult a, b, c;
    TASK_CHECK(medium.segment(forward, Interval(0.0, 12.0), 1e-9, 10000, a,
                              stats));
    TASK_CHECK(medium.segment(reversed, Interval(0.0, 12.0), 1e-9, 10000, b,
                              stats));
    TASK_CHECK(medium.segment(scaled, Interval(0.0, 6.0), 1e-9, 10000, c,
                              stats));
    TASK_CHECK(close(a.transmittance, b.transmittance, 1e-9));
    TASK_CHECK(close(a.transmittance, c.transmittance, 1e-9));
    TASK_CHECK(close_rgb(a.emission, b.emission, 1e-9));
    TASK_CHECK(close_rgb(a.emission, c.emission, 1e-9));
    return 0;
}

static int optical_narrow_kernel() {
    const double var = 1e-6; // standard deviation 1e-3
    GaussianMedium medium(std::vector<GaussianKernel>{
        make_kernel(Point3(0, 0, 0), var, 0, 0, var, 0, var, 2.0,
                    Radiance(0, 0, 0))});
    Ray ray(Point3(-1, 0, 0), Vector3(1, 0, 0));
    MediumStats stats;
    double tau = analytic_line_tau(2.0, var, -3e-3, 3e-3);
    SegmentResult seg;
    TASK_CHECK(medium.segment(ray, Interval(0.0, 2.0), 1e-8, 1000, seg,
                              stats));
    TASK_CHECK(close(seg.transmittance, std::exp(-tau), 1e-10));
    TASK_CHECK(seg.transmittance < 1.0);
    return 0;
}

static int optical_nondiagonal_covariance() {
    const double angle = 0.7;
    const double cs = std::cos(angle), sn = std::sin(angle);
    // C = R diag(0.25, 1, 4) R^T, rotation about z.
    double d[3] = {0.25, 1.0, 4.0};
    double R[9] = {cs, -sn, 0, sn, cs, 0, 0, 0, 1};
    double C[9] = {0};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                C[i * 3 + j] += R[i * 3 + k] * d[k] * R[j * 3 + k];

    auto kernels = std::vector<GaussianKernel>{make_kernel(
        Point3(0, 0, 0), C[0], C[1], C[2], C[4], C[5], C[8], 1.1,
        Radiance(0.5, 0.6, 0.7))};
    GaussianMedium medium(kernels);

    Ray ray(Point3(-4, 0.3, -0.2), Vector3(1, 0, 0));
    double lo, hi;
    TASK_CHECK(reference_support_range(kernels, ray, 0.0,
                                       std::numeric_limits<double>::infinity(),
                                       lo, hi));
    RefResult ref = reference_integrate(kernels, ray, lo, hi);

    MediumStats stats;
    SegmentResult seg;
    TASK_CHECK(medium.segment(ray, Interval(0.0, infinity), 1e-10, 1000000,
                              seg, stats));
    TASK_CHECK(close(seg.transmittance, ref.transmittance, 1e-9));

    // Rotating the whole scene by R^T maps the kernel to diag(d) and the ray
    // to R^T O + t R^T D, which must give identical physics.
    GaussianKernel aligned = make_kernel(Point3(0, 0, 0), d[0], 0, 0, d[1],
                                         0, d[2], 1.1,
                                         Radiance(0.5, 0.6, 0.7));
    GaussianMedium aligned_medium(std::vector<GaussianKernel>{aligned});
    Vector3 dir(cs, -sn, 0.0);
    Vector3 origin = Point3(-4, 0.3, -0.2);
    Vector3 rot_origin(cs * origin.x() + sn * origin.y(),
                       -sn * origin.x() + cs * origin.y(), origin.z());
    Ray rotated_ray(rot_origin, dir);
    SegmentResult seg_rot;
    TASK_CHECK(aligned_medium.segment(rotated_ray, Interval(0.0, infinity),
                                      1e-10, 1000000, seg_rot, stats));
    TASK_CHECK(close(seg.transmittance, seg_rot.transmittance, 1e-9));
    TASK_CHECK(close_rgb(seg.emission, seg_rot.emission, 1e-9));
    return 0;
}

static int optical_input_order_invariance() {
    auto k1 = make_kernel(Point3(0, 0, 0), 0.25, 0.05, 0.02, 0.4, -0.03, 0.3,
                          0.8, Radiance(0.9, 0.1, 0.2));
    auto k2 = make_kernel(Point3(0.4, 0.1, 0.0), 0.3, -0.07, 0.04, 0.25, 0.02,
                          0.35, 1.2, Radiance(0.1, 0.8, 0.3));
    auto k3 = make_kernel(Point3(-0.3, 0.2, 0.1), 0.2, 0.03, -0.05, 0.35, 0.04,
                          0.28, 0.6, Radiance(0.2, 0.3, 0.9));
    GaussianMedium m1(std::vector<GaussianKernel>{k1, k2, k3});
    GaussianMedium m2(std::vector<GaussianKernel>{k3, k1, k2});
    GaussianMedium m3(std::vector<GaussianKernel>{k2, k3, k1});

    Ray ray(Point3(-3, 0, 0), Vector3(1, 0.2, -0.1));
    MediumStats s1, s2, s3;
    SegmentResult r1, r2, r3;
    TASK_CHECK(m1.segment(ray, Interval(0.0, 8.0), 1e-9, 100000, r1, s1));
    TASK_CHECK(m2.segment(ray, Interval(0.0, 8.0), 1e-9, 100000, r2, s2));
    TASK_CHECK(m3.segment(ray, Interval(0.0, 8.0), 1e-9, 100000, r3, s3));
    TASK_CHECK(close(r1.transmittance, r2.transmittance, 1e-9));
    TASK_CHECK(close(r1.transmittance, r3.transmittance, 1e-9));
    TASK_CHECK(close_rgb(r1.emission, r2.emission, 1e-9));
    TASK_CHECK(close_rgb(r1.emission, r3.emission, 1e-9));
    return 0;
}

static int optical_bvh_culling_counts() {
    std::vector<GaussianKernel> kernels;
    kernels.reserve(4096);
    const double var = 0.0025; // 3-sigma support radius 0.15
    for (int ix = 0; ix < 16; ++ix)
        for (int iy = 0; iy < 16; ++iy)
            for (int iz = 0; iz < 16; ++iz) {
                // Centered so rows pass through y = z = 0.
                Point3 center((ix - 8) * 0.5, (iy - 8) * 0.5, (iz - 8) * 0.5);
                kernels.push_back(make_kernel(center, var, 0, 0, var, 0, var,
                                               1.0, Radiance(0, 0, 0)));
            }
    GaussianMedium medium(kernels);
    TASK_CHECK(medium.size() == 4096);

    std::uint64_t total_tests = 0;
    int touched = 0;
    for (int r = 0; r < 32; ++r) {
        double y = 0.02 * ((r % 8) - 4);
        double z = 0.02 * ((r / 8) - 4);
        Ray ray(Point3(-10, y, z), Vector3(1, 0, 0));
        MediumStats stats;
        SegmentResult seg;
        TASK_CHECK(medium.segment(ray, Interval(0.0, 20.0), 1e-6, 1000, seg,
                                  stats));
        TASK_CHECK(stats.kernel_tests > 0);
        total_tests += stats.kernel_tests;
        if (seg.transmittance < 1.0) ++touched;
    }
    TASK_CHECK(touched == 32);
    // Required bound: fewer than 4096*32/8 actual ellipsoid tests in total.
    TASK_CHECK(total_tests < 4096ULL * 32ULL / 8ULL);

    // Counters reset on every query, including preparation geometry tests.
    Ray miss_ray(Point3(-10, 100, 100), Vector3(1, 0, 0));
    MediumStats miss_stats;
    double T = medium.transmittance(miss_ray, Interval(0.0, 20.0), miss_stats);
    TASK_CHECK(T == 1.0);
    TASK_CHECK(miss_stats.interval_evaluations == 0);
    TASK_CHECK(miss_stats.kernel_tests < 4096);
    return 0;
}

static int optical_invalid_update_keeps_snapshot() {
    auto good = make_kernel(Point3(0, 0, 0), 1, 0, 0, 1, 0, 1, 0.5,
                            Radiance(1, 1, 1));
    GaussianMedium medium(std::vector<GaussianKernel>{good});

    auto try_bad = [&](const GaussianKernel& bad) {
        try {
            medium.set_kernels(std::vector<GaussianKernel>{bad});
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    auto neg_eig = good;
    neg_eig.covariance[0] = -1.0;
    TASK_CHECK(try_bad(neg_eig));
    auto singular = good;
    singular.covariance = {1, 1, 0, 1, 0, 1}; // determinant zero
    TASK_CHECK(try_bad(singular));
    auto nan_field = good;
    nan_field.mean = Point3(std::nan(""), 0, 0);
    TASK_CHECK(try_bad(nan_field));
    auto neg_ext = good;
    neg_ext.extinction = -0.1;
    TASK_CHECK(try_bad(neg_ext));
    auto neg_src = good;
    neg_src.source = Radiance(0, -1, 0);
    TASK_CHECK(try_bad(neg_src));
    auto non_symmetric_ok = good; // control: a valid off-diagonal kernel works
    non_symmetric_ok.covariance = {1, 0.2, 0.1, 1, 0.05, 1};
    medium.set_kernels(std::vector<GaussianKernel>{non_symmetric_ok});

    // The last valid snapshot is queryable; failed updates never damaged it.
    Ray ray(Point3(-6, 0, 0), Vector3(1, 0, 0));
    MediumStats stats;
    SegmentResult seg;
    TASK_CHECK(medium.segment(ray, Interval(0.0, 12.0), 1e-7, 1000, seg,
                              stats));
    TASK_CHECK(medium.size() == 1);

    // Illegal queries throw and leave the result object untouched.
    SegmentResult sentinel;
    sentinel.transmittance = -123.0;
    bool threw = false;
    try {
        medium.segment(Ray(Point3(0, 0, 0), Vector3(0, 0, 0)),
                       Interval(0.0, 1.0), 1e-7, 1000, sentinel, stats);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TASK_CHECK(threw && sentinel.transmittance == -123.0);
    return 0;
}

// ===========================================================================
// emission group
// ===========================================================================

static int emission_single_kernel_analytic() {
    const double var = 0.25;
    const Radiance src(0.8, 0.4, 0.2);
    GaussianMedium medium(std::vector<GaussianKernel>{
        make_kernel(Point3(0, 0, 0), var, 0, 0, var, 0, var, 0.7, src)});
    Ray ray(Point3(-6, 0, 0), Vector3(1, 0, 0));
    MediumStats stats;

    for (double tol : {1e-4, 1e-7, 1e-10}) {
        SegmentResult seg;
        TASK_CHECK(medium.segment(ray, Interval(0.0, 12.0), tol, 1000000, seg,
                                  stats));
        // With a constant source, E = S (1 - T) exactly; the returned bound
        // must cover the difference and never exceed the requested tolerance.
        Radiance expected(src.x() * (1.0 - seg.transmittance),
                          src.y() * (1.0 - seg.transmittance),
                          src.z() * (1.0 - seg.transmittance));
        for (int c = 0; c < 3; ++c) {
            TASK_CHECK(std::fabs(seg.emission[c] - expected[c]) <=
                       seg.error[c] + 1e-10);
            TASK_CHECK(seg.error[c] <= tol + 1e-10);
        }
        if (tol == 1e-10) TASK_CHECK(close_rgb(seg.emission, expected, 1e-9));
    }
    return 0;
}

static int emission_same_color_overlap_analytic() {
    const Radiance src(0.5, 0.5, 0.5);
    auto k1 = make_kernel(Point3(0, 0, 0), 0.25, 0, 0, 0.25, 0, 0.25, 0.9, src);
    auto k2 = make_kernel(Point3(0.3, 0, 0), 0.36, 0, 0, 0.36, 0, 0.36, 0.6,
                          src);
    GaussianMedium medium(std::vector<GaussianKernel>{k1, k2});
    Ray ray(Point3(-6, 0, 0), Vector3(1, 0, 0));
    MediumStats stats;
    SegmentResult seg;
    TASK_CHECK(medium.segment(ray, Interval(0.0, 12.0), 1e-9, 1000000, seg,
                              stats));

    // Analytic optical thickness is the sum of the two erf line integrals.
    double tau1 = analytic_line_tau(0.9, 0.25, -1.5, 1.5);
    double tau2 = analytic_line_tau(0.6, 0.36, -1.8, 1.8);
    double T = std::exp(-(tau1 + tau2));
    TASK_CHECK(close(seg.transmittance, T, 1e-10));
    // Same source color everywhere: E = S (1 - T) remains exact.
    TASK_CHECK(close_rgb(seg.emission,
                         Radiance(0.5 * (1 - T), 0.5 * (1 - T),
                                  0.5 * (1 - T)),
                         1e-9));
    TASK_CHECK(seg.error.x() <= 1e-9 + 1e-10);
    return 0;
}

static int emission_different_color_reference() {
    auto k1 = make_kernel(Point3(0, 0, 0), 0.25, 0.08, 0.03, 0.3, -0.04, 0.2,
                          1.1, Radiance(0.9, 0.1, 0.2));
    auto k2 = make_kernel(Point3(0.4, 0.15, -0.1), 0.3, -0.06, 0.05, 0.25,
                          0.02, 0.35, 0.8, Radiance(0.1, 0.9, 0.3));
    auto k3 = make_kernel(Point3(-0.3, -0.1, 0.2), 0.2, 0.04, -0.06, 0.35,
                          0.03, 0.28, 1.3, Radiance(0.2, 0.3, 0.9));
    std::vector<GaussianKernel> kernels{k1, k2, k3};

    Ray ray(Point3(-3, 0.1, -0.1), Vector3(1, 0.15, 0.07));
    double lo, hi;
    TASK_CHECK(reference_support_range(
        kernels, ray, 0.0, std::numeric_limits<double>::infinity(), lo, hi));
    RefResult ref = reference_integrate(kernels, ray, lo, hi);

    GaussianMedium medium(kernels);
    MediumStats stats;
    for (double tol : {1e-5, 1e-8}) {
        SegmentResult seg;
        TASK_CHECK(medium.segment(ray, Interval(0.0, infinity), tol, 1000000,
                                  seg, stats));
        TASK_CHECK(std::fabs(seg.emission.x() - ref.emission.x()) <=
                   seg.error.x() + 1e-9);
        TASK_CHECK(std::fabs(seg.emission.y() - ref.emission.y()) <=
                   seg.error.y() + 1e-9);
        TASK_CHECK(std::fabs(seg.emission.z() - ref.emission.z()) <=
                   seg.error.z() + 1e-9);
        TASK_CHECK(seg.error.x() <= tol + 1e-10);
        // Transmittance is checked analytically to 1e-10 in the optical
        // group; here the grid reference only needs coarse agreement.
        TASK_CHECK(close(seg.transmittance, ref.transmittance, 1e-7));
    }
    return 0;
}

static int emission_budget_exhaustion() {
    std::vector<GaussianKernel> kernels;
    for (int i = 0; i < 6; ++i) {
        kernels.push_back(make_kernel(
            Point3(0.9 * i, 0.2 * (i % 2), 0.1 * (i - 3)), 0.5,
            0.05 * (i - 2), 0.02, 0.6, -0.03, 0.4, 1.5 + 0.2 * i,
            Radiance(0.7, 0.3 * i, 0.9 - 0.1 * i)));
    }
    GaussianMedium medium(kernels);
    Ray ray(Point3(-2, 0, 0), Vector3(1, 0, 0));
    MediumStats stats;

    SegmentResult sentinel;
    sentinel.transmittance = -7.0;
    sentinel.emission = Radiance(-1, -2, -3);
    SegmentResult copy = sentinel;
    bool ok = medium.segment(ray, Interval(0.0, 8.0), 1e-12, 1, sentinel,
                             stats);
    TASK_CHECK(!ok);
    TASK_CHECK(sentinel.transmittance == copy.transmittance);
    TASK_CHECK(close_rgb(sentinel.emission, copy.emission, 0.0));
    TASK_CHECK(stats.interval_evaluations >= 1);

    // The same query succeeds once the budget is large enough.
    SegmentResult good;
    TASK_CHECK(medium.segment(ray, Interval(0.0, 8.0), 1e-8, 1000000, good,
                              stats));
    TASK_CHECK(good.error.x() <= 1e-8 + 1e-10);
    TASK_CHECK(good.emission.x() > 0.0);
    return 0;
}

static int emission_empty_and_zero_extinction() {
    GaussianMedium empty;
    TASK_CHECK(empty.empty() && empty.size() == 0);
    Ray ray(Point3(0, 0, 0), Vector3(1, 0, 0));
    MediumStats stats;
    SegmentResult seg;
    TASK_CHECK(empty.segment(ray, Interval(0.0, infinity), 1e-6, 10, seg,
                             stats));
    TASK_CHECK(seg.transmittance == 1.0 && seg.error.x() == 0.0);
    TASK_CHECK(empty.transmittance(ray, Interval(0.0, 5.0), stats) == 1.0);

    GaussianMedium zero_medium(std::vector<GaussianKernel>{
        make_kernel(Point3(0, 0, 0), 0.25, 0, 0, 0.25, 0, 0.25, 0.0,
                    Radiance(1, 1, 1))});
    TASK_CHECK(zero_medium.segment(ray, Interval(0.0, 5.0), 1e-6, 10, seg,
                                   stats));
    TASK_CHECK(seg.transmittance == 1.0 && seg.emission.x() == 0.0);
    return 0;
}

// ===========================================================================
// transport group
// ===========================================================================

static int transport_empty_matches_phong() {
    Material mat;
    mat.kd = 1;
    mat.ks = 0;
    mat.ka = 0;
    mat.diffusecolor = Radiance(.2, .4, .6);
    auto world = std::make_shared<ObjectList>();
    world->add(make_plane(&mat));
    world->build();

    auto light = std::make_shared<PointLight>(Point3(0, 0, 0),
                                              Radiance(9, 9, 9));
    Camera cam = make_test_camera();
    GaussianMedium empty;
    GaussianFrame frame;
    TASK_CHECK(cam.render_gaussian(*world, *light, empty, 1e-6, 1000, frame));
    TASK_CHECK(frame.width == 1 && frame.height == 1);
    TASK_CHECK(close_rgb(frame.value[0], Radiance(0.2, 0.4, 0.6), 1e-10));
    TASK_CHECK(frame.error[0].x() == 0.0);
    TASK_CHECK(frame.transmittance[0] == 1.0);
    return 0;
}

static int transport_surface_truncation_and_first_T() {
    Material mat;
    mat.kd = 1;
    mat.ks = 0;
    mat.ka = 0;
    mat.diffusecolor = Radiance(.5, .5, .5);
    auto world = std::make_shared<ObjectList>();
    world->add(make_plane(&mat));
    world->build();
    auto light = std::make_shared<PointLight>(Point3(3, 3, 0),
                                              Radiance(9, 9, 9));
    Camera cam = make_test_camera();

    // Kernel entirely behind the surface at z = -3: no first-segment effect.
    auto behind = std::vector<GaussianKernel>{make_kernel(
        Point3(0, 0, -5), 0.25, 0, 0, 0.25, 0, 0.25, 0.8, Radiance(0, 0, 0))};
    GaussianMedium medium_behind(behind);
    GaussianFrame f_behind;
    TASK_CHECK(cam.render_gaussian(*world, *light, medium_behind, 1e-6, 1000,
                                   f_behind));
    TASK_CHECK(f_behind.transmittance[0] == 1.0);

    // Kernel between camera and surface: first-segment T matches the direct
    // medium query truncated at the surface hit t = 1.
    auto front = std::vector<GaussianKernel>{make_kernel(
        Point3(0, 0, -1.5), 0.25, 0, 0, 0.25, 0, 0.25, 0.8,
        Radiance(0.3, 0.2, 0.1))};
    GaussianMedium medium_front(front);
    GaussianFrame f_front;
    TASK_CHECK(cam.render_gaussian(*world, *light, medium_front, 1e-7, 10000,
                                   f_front));
    TASK_CHECK(f_front.transmittance[0] < 1.0);

    Ray camera_ray(Point3(0, 0, 0), Vector3(0, 0, -3));
    MediumStats stats;
    SegmentResult direct;
    TASK_CHECK(medium_front.segment(camera_ray, Interval(0.0, 1.0), 1e-9,
                                    10000, direct, stats));
    TASK_CHECK(close(f_front.transmittance[0], direct.transmittance, 1e-10));
    return 0;
}

static int transport_shadow_line_extinction() {
    Material mat;
    mat.kd = 1;
    mat.ks = 0;
    mat.ka = 0;
    mat.diffusecolor = Radiance(.8, .8, .8);
    auto world = std::make_shared<ObjectList>();
    world->add(make_plane(&mat));
    world->build();
    // Light sits between the camera and the surface.
    auto light = std::make_shared<PointLight>(Point3(0, 0, -2),
                                              Radiance(8, 8, 8));
    Camera cam = make_test_camera();
    GaussianMedium empty;
    GaussianFrame f_empty;
    TASK_CHECK(cam.render_gaussian(*world, *light, empty, 1e-7, 1000, f_empty));

    // Kernel past the light: shadow line must not see it.
    auto behind_light = std::vector<GaussianKernel>{make_kernel(
        Point3(0, 0, -1), 0.04, 0, 0, 0.04, 0, 0.04, 1.0,
        Radiance(0, 0, 0))};
    GaussianMedium m_behind(behind_light);
    GaussianFrame f_behind;
    TASK_CHECK(cam.render_gaussian(*world, *light, m_behind, 1e-7, 1000,
                                   f_behind));
    Radiance surface_behind = f_behind.value[0] / f_behind.transmittance[0];
    TASK_CHECK(close_rgb(surface_behind, f_empty.value[0], 1e-10));

    // Kernel on the finite shadow segment: shadow shading is attenuated.
    auto between = std::vector<GaussianKernel>{make_kernel(
        Point3(0, 0, -2.5), 0.04, 0, 0, 0.04, 0, 0.04, 1.0,
        Radiance(0, 0, 0))};
    GaussianMedium m_between(between);
    GaussianFrame f_between;
    TASK_CHECK(cam.render_gaussian(*world, *light, m_between, 1e-7, 1000,
                                   f_between));
    TASK_CHECK(f_between.transmittance[0] < 1.0);
    Radiance surface_between =
        f_between.value[0] / f_between.transmittance[0];
    TASK_CHECK(surface_between.x() < f_empty.value[0].x() - 1e-9);

    // Nested LightList of point lights behaves like the flat light list.
    auto nested = std::make_shared<LightList>();
    nested->add(std::make_shared<LightList>(light));
    GaussianFrame f_nested;
    TASK_CHECK(cam.render_gaussian(*world, *nested, empty, 1e-7, 1000,
                                   f_nested));
    TASK_CHECK(close_rgb(f_nested.value[0], f_empty.value[0], 1e-10));
    return 0;
}

static int transport_mirror_recrosses_medium() {
    Material mirror;
    mirror.kd = 1;
    mirror.ks = 0;
    mirror.ka = 0;
    mirror.diffusecolor = Radiance(.3, .3, .3);
    mirror.is_reflective = true;
    mirror.reflectivity = 1.0;
    auto world = std::make_shared<ObjectList>();
    world->add(make_plane(&mirror));
    world->build();
    auto light = std::make_shared<PointLight>(Point3(4, 4, 0),
                                              Radiance(4, 4, 4));
    Camera cam = make_test_camera();

    auto kernels = std::vector<GaussianKernel>{
        make_kernel(Point3(0, 0, -1.5), 0.25, 0, 0, 0.25, 0, 0.25, 0.7,
                    Radiance(0.6, 0.1, 0.1)),
        make_kernel(Point3(0, 0, 1.5), 0.25, 0, 0, 0.25, 0, 0.25, 0.9,
                    Radiance(0.1, 0.6, 0.1))};
    GaussianMedium medium(kernels);
    GaussianFrame frame;
    TASK_CHECK(cam.render_gaussian(*world, *light, medium, 1e-8, 1000000,
                                   frame));
    for (int c = 0; c < 3; ++c)
        TASK_CHECK(frame.error[0][c] <= 1e-8 + 1e-10);

    // Independent decomposition into the two physical medium segments.
    Ray camera_ray(Point3(0, 0, 0), Vector3(0, 0, -3));
    Ray reflect_ray(Point3(0, 0, -2.999), Vector3(0, 0, 1));
    MediumStats stats;
    SegmentResult first, second;
    TASK_CHECK(medium.segment(camera_ray, Interval(0.0, 1.0), 1e-9, 100000,
                              first, stats));
    TASK_CHECK(medium.segment(reflect_ray, Interval(0.0, infinity), 1e-9,
                              100000, second, stats));
    TASK_CHECK(second.transmittance < 1.0);
    Radiance expected = first.emission +
                        first.transmittance *
                            (second.emission +
                             second.transmittance * cam.background);
    Radiance expected_err = first.error +
                            first.transmittance * second.error;
    TASK_CHECK(close_rgb(frame.value[0], expected,
                         expected_err.x() + expected_err.y() + 1e-9));
    // The reported transmittance is the camera segment, not the mirror child.
    TASK_CHECK(close(frame.transmittance[0], first.transmittance, 1e-10));
    TASK_CHECK(std::fabs(frame.transmittance[0] - second.transmittance) >
               1e-6);
    return 0;
}

static int transport_unsupported_inputs_preserve_output() {
    Material mat;
    mat.kd = 1;
    mat.ks = 0;
    mat.ka = 0;
    auto world = std::make_shared<ObjectList>();
    world->add(make_plane(&mat));
    world->build();
    auto point = std::make_shared<PointLight>(Point3(0, 0, 0),
                                              Radiance(9, 9, 9));
    Camera cam = make_test_camera();
    GaussianMedium empty;

    GaussianFrame sentinel;
    sentinel.width = -99;
    GaussianFrame copy = sentinel;

    auto rect = std::make_shared<RectangularLight>(
        Point3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0), 1.0, 1.0,
        Radiance(1, 1, 1), 4);
    TASK_CHECK(!cam.render_gaussian(*world, *rect, empty, 1e-6, 1000,
                                    sentinel));
    TASK_CHECK(sentinel.width == copy.width);

    auto bad_leaf = std::make_shared<LightList>();
    bad_leaf->add(rect);
    TASK_CHECK(!cam.render_gaussian(*world, *bad_leaf, empty, 1e-6, 1000,
                                    sentinel));
    TASK_CHECK(sentinel.width == copy.width);

    Material glass;
    glass.is_refractive = true;
    glass.refractiveindex = 1.5;
    auto glass_world = std::make_shared<ObjectList>();
    glass_world->add(make_plane(&glass));
    glass_world->build();
    TASK_CHECK(!cam.render_gaussian(*glass_world, *point, empty, 1e-6, 1000,
                                    sentinel));
    TASK_CHECK(sentinel.width == copy.width);

    Material brdf_mat;
    brdf_mat.has_brdf = true;
    auto brdf_world = std::make_shared<ObjectList>();
    brdf_world->add(make_plane(&brdf_mat));
    brdf_world->build();
    TASK_CHECK(!cam.render_gaussian(*brdf_world, *point, empty, 1e-6, 1000,
                                    sentinel));
    TASK_CHECK(sentinel.width == copy.width);

    TASK_CHECK(!cam.render_gaussian(*world, *point, empty, 0.0, 1000,
                                    sentinel));
    TASK_CHECK(!cam.render_gaussian(*world, *point, empty, 1e-6, 0, sentinel));
    TASK_CHECK(sentinel.width == copy.width);
    return 0;
}

static int transport_pixel_budget_failure_preserves_output() {
    Material mat;
    mat.kd = 1;
    mat.ks = 0;
    mat.ka = 0;
    auto world = std::make_shared<ObjectList>();
    world->add(make_plane(&mat));
    world->build();
    auto light = std::make_shared<PointLight>(Point3(3, 3, 0),
                                              Radiance(9, 9, 9));
    std::vector<GaussianKernel> kernels;
    for (int i = 0; i < 8; ++i) {
        kernels.push_back(make_kernel(
            Point3(0.2 * (i - 4), 0.1 * (i % 3), -0.4 * i), 0.4,
            0.04 * (i - 3), 0.02, 0.5, -0.03, 0.45, 1.4,
            Radiance(0.8, 0.2 + 0.05 * i, 0.5)));
    }
    GaussianMedium medium(kernels);
    Camera cam = make_test_camera();
    GaussianFrame sentinel;
    sentinel.width = -42;
    TASK_CHECK(!cam.render_gaussian(*world, *light, medium, 1e-12, 1,
                                    sentinel));
    TASK_CHECK(sentinel.width == -42);

    GaussianFrame ok;
    TASK_CHECK(cam.render_gaussian(*world, *light, medium, 1e-5, 1000000, ok));
    TASK_CHECK(ok.width == 1 && ok.error[0].x() <= 1e-5 + 1e-10);
    return 0;
}

static int transport_linear_output_no_tonemap() {
    Material mat;
    mat.kd = 1;
    mat.ks = 0;
    mat.ka = 0;
    mat.diffusecolor = Radiance(1, 1, 1);
    auto world = std::make_shared<ObjectList>();
    world->add(make_plane(&mat));
    world->build();
    auto light = std::make_shared<PointLight>(Point3(0, 0, 0),
                                              Radiance(100, 100, 100));
    Camera cam = make_test_camera();
    GaussianMedium empty;
    GaussianFrame frame;
    TASK_CHECK(cam.render_gaussian(*world, *light, empty, 1e-7, 1000, frame));
    // Linear values above 1 are stored untouched (no tone mapping/clamping).
    TASK_CHECK(frame.value[0].x() > 1.0);
    TASK_CHECK(close(frame.value[0].x(), 100.0 / 9.0, 1e-10));
    return 0;
}

// ===========================================================================
// Registration
// ===========================================================================

const struct task_case task_cases[] = {
    {"optical", "single_kernel_analytic", optical_single_analytic},
    {"optical", "clipped_partial_interval", optical_clipped_partial},
    {"optical", "reverse_and_nonunit_rays", optical_reverse_and_scale},
    {"optical", "narrow_kernel", optical_narrow_kernel},
    {"optical", "nondiagonal_covariance", optical_nondiagonal_covariance},
    {"optical", "input_order_invariance", optical_input_order_invariance},
    {"optical", "bvh_culling_counts", optical_bvh_culling_counts},
    {"optical", "invalid_update_keeps_snapshot",
     optical_invalid_update_keeps_snapshot},

    {"emission", "single_kernel_analytic_identity",
     emission_single_kernel_analytic},
    {"emission", "same_color_overlap_analytic",
     emission_same_color_overlap_analytic},
    {"emission", "different_color_reference",
     emission_different_color_reference},
    {"emission", "budget_exhaustion", emission_budget_exhaustion},
    {"emission", "empty_and_zero_extinction",
     emission_empty_and_zero_extinction},

    {"transport", "empty_medium_matches_phong",
     transport_empty_matches_phong},
    {"transport", "surface_truncation_first_transmittance",
     transport_surface_truncation_and_first_T},
    {"transport", "shadow_line_extinction", transport_shadow_line_extinction},
    {"transport", "mirror_recrosses_medium", transport_mirror_recrosses_medium},
    {"transport", "unsupported_inputs_preserved",
     transport_unsupported_inputs_preserve_output},
    {"transport", "pixel_budget_failure_preserved",
     transport_pixel_budget_failure_preserves_output},
    {"transport", "linear_output_without_tonemap",
     transport_linear_output_no_tonemap},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
