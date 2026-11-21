#include "GaussianMedium.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>

namespace {

constexpr double kSupportQ = 9.0;          // kernels are truncated at q <= 9

bool is_finite(const Vector3& v) {
    return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}

// Symmetric 3x3 matrix stored row-major.
struct Mat3 {
    double m[9];
    double at(int r, int c) const { return m[r * 3 + c]; }
};

Mat3 kernel_matrix(const GaussianKernel& k) {
    const auto& c = k.covariance;
    return Mat3{{c[0], c[1], c[2],
                 c[1], c[3], c[4],
                 c[2], c[4], c[5]}};
}

// LDL^T decomposition of a symmetric matrix. Returns false when the matrix is
// not (numerically) positive definite or cannot be represented.
bool ldl_decompose(const Mat3& a, Mat3& L, double d[3]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) L.m[i * 3 + j] = (i == j) ? 1.0 : 0.0;
    }
    for (int k = 0; k < 3; ++k) {
        double sum = a.at(k, k);
        for (int p = 0; p < k; ++p) sum -= L.at(k, p) * L.at(k, p) * d[p];
        if (!std::isfinite(sum) || sum <= 0.0) return false;
        d[k] = sum;
        for (int i = k + 1; i < 3; ++i) {
            double s = a.at(i, k);
            for (int p = 0; p < k; ++p) s -= L.at(i, p) * L.at(k, p) * d[p];
            double v = s / d[k];
            if (!std::isfinite(v)) return false;
            L.m[i * 3 + k] = v;
        }
    }
    return true;
}

// C^-1 via the LDL factors.
Mat3 inverse_from_ldl(const Mat3& L, const double d[3]) {
    Mat3 inv{};
    for (int col = 0; col < 3; ++col) {
        double y[3];
        // Solve L y = e_col (unit lower triangular).
        for (int i = 0; i < 3; ++i) {
            double s = (i == col) ? 1.0 : 0.0;
            for (int j = 0; j < i; ++j) s -= L.at(i, j) * y[j];
            y[i] = s;
        }
        double z[3];
        for (int i = 0; i < 3; ++i) z[i] = y[i] / d[i];
        // Solve L^T x = z.
        double x[3];
        for (int i = 2; i >= 0; --i) {
            double s = z[i];
            for (int j = i + 1; j < 3; ++j) s -= L.at(j, i) * x[j];
            x[i] = s;
        }
        for (int row = 0; row < 3; ++row) inv.m[row * 3 + col] = x[row];
    }
    return inv;
}

Vector3 mat_vec(const Mat3& a, const Vector3& v) {
    return Vector3(a.at(0, 0) * v.x() + a.at(0, 1) * v.y() + a.at(0, 2) * v.z(),
                   a.at(1, 0) * v.x() + a.at(1, 1) * v.y() + a.at(1, 2) * v.z(),
                   a.at(2, 0) * v.x() + a.at(2, 1) * v.y() + a.at(2, 2) * v.z());
}

// Axis-aligned box used by the medium's own hierarchy.
struct GBox {
    Vector3 lo{0, 0, 0};
    Vector3 hi{0, 0, 0};
};

GBox union_box(const GBox& a, const GBox& b) {
    GBox r;
    for (int i = 0; i < 3; ++i) {
        r.lo[i] = std::min(a.lo[i], b.lo[i]);
        r.hi[i] = std::max(a.hi[i], b.hi[i]);
    }
    return r;
}

// Slab test; returns the clipped ray parameter overlap with [tmin, tmax].
bool box_hit(const GBox& box, const Ray& ray, double tmin, double tmax,
             double& out_lo, double& out_hi) {
    double lo = tmin, hi = tmax;
    const Point3& o = ray.origin();
    const Vector3& d = ray.direction();
    for (int axis = 0; axis < 3; ++axis) {
        if (d[axis] == 0.0) {
            if (o[axis] < box.lo[axis] || o[axis] > box.hi[axis]) return false;
            continue;
        }
        double t0 = (box.lo[axis] - o[axis]) / d[axis];
        double t1 = (box.hi[axis] - o[axis]) / d[axis];
        if (t0 > t1) std::swap(t0, t1);
        lo = std::max(lo, t0);
        hi = std::min(hi, t1);
        if (lo > hi) return false;
    }
    out_lo = lo;
    out_hi = hi;
    return true;
}

// Prepared per-kernel data held by a snapshot.
struct KernelData {
    GaussianKernel kernel;
    Mat3 inv_cov;
    GBox box;
};

// A kernel restricted to a ray: f(t) = A (t - tc)^2 + fmin, with the support
// ellipsoid covering [t0, t1]. All quantities are in ray parameter units.
struct LineKernel {
    double A;             // D^T C^-1 D, strictly positive
    double tc;            // closest-approach parameter
    double fmin;          // f(tc)
    double t0, t1;        // support entry/exit
    double extinction;
    Radiance source;
};

// Antiderivative of g(t) = exp(-f(t)/2):
//   Phi(t) = exp(-fmin/2) sqrt(pi/(2A)) erf(sqrt(A/2) (t - tc))
struct LineProfile {
    double amp;           // exp(-fmin/2) * sqrt(pi/(2A))
    double zscale;        // sqrt(A/2)
    double tc;

    static double stable_erf(double x) {
        // erf via erfc keeps the result accurate for large |x|.
        if (x >= 0.0) return 1.0 - std::erfc(x);
        return std::erfc(-x) - 1.0;
    }

    double phi(double t) const {
        return amp * stable_erf(zscale * (t - tc));
    }
};

struct BvhNode {
    GBox box;
    int left = -1;        // child node indices for interior nodes
    int right = -1;
    int first = -1;       // leaf range into an index permutation
    int count = 0;        // 0 for interior nodes
};

} // namespace

struct GaussianMedium::Snapshot {
    std::vector<KernelData> kernels;
    std::vector<BvhNode> nodes;
    std::vector<int> perm;

    void build_bvh() {
        perm.resize(kernels.size());
        std::iota(perm.begin(), perm.end(), 0);
        nodes.clear();
        if (!perm.empty()) nodes.reserve(2 * perm.size());
        build_recursive(0, static_cast<int>(perm.size()));
    }

    int build_recursive(int start, int end) {
        GBox box = kernels[perm[start]].box;
        for (int i = start + 1; i < end; ++i)
            box = union_box(box, kernels[perm[i]].box);

        int node_index = static_cast<int>(nodes.size());
        nodes.push_back(BvhNode{});
        int count = end - start;
        if (count <= 4) {
            nodes[node_index].box = box;
            nodes[node_index].first = start;
            nodes[node_index].count = count;
            return node_index;
        }

        Vector3 extent(box.hi.x() - box.lo.x(), box.hi.y() - box.lo.y(),
                       box.hi.z() - box.lo.z());
        int axis = 0;
        if (extent[1] > extent[axis]) axis = 1;
        if (extent[2] > extent[axis]) axis = 2;
        int mid = start + count / 2;
        std::nth_element(perm.begin() + start, perm.begin() + mid,
                         perm.begin() + end,
                         [&](int ia, int ib) {
                             double ca = 0.5 * (kernels[ia].box.lo[axis] +
                                                kernels[ia].box.hi[axis]);
                             double cb = 0.5 * (kernels[ib].box.lo[axis] +
                                                kernels[ib].box.hi[axis]);
                             return ca < cb;
                         });
        int left = build_recursive(start, mid);
        int right = build_recursive(mid, end);
        nodes[node_index].box = box;
        nodes[node_index].left = left;
        nodes[node_index].right = right;
        return node_index;
    }

    // BVH-traversed candidate gathering. Every actual ellipsoid test is
    // recorded in stats.kernel_tests, including preparation work.
    std::vector<LineKernel> gather(const Ray& ray, double a, double b,
                                   MediumStats& stats) const {
        std::vector<LineKernel> active;
        if (nodes.empty()) return active;

        std::vector<int> stack;
        stack.push_back(0);
        const Point3& o = ray.origin();
        const Vector3& d = ray.direction();
        while (!stack.empty()) {
            const BvhNode& node = nodes[stack.back()];
            stack.pop_back();
            double blo, bhi;
            if (!box_hit(node.box, ray, a, b, blo, bhi)) continue;
            if (node.count > 0) {
                for (int k = 0; k < node.count; ++k) {
                    const KernelData& kd = kernels[perm[node.first + k]];
                    ++stats.kernel_tests;
                    const Vector3 mo = kd.kernel.mean - o;
                    const Mat3& ic = kd.inv_cov;
                    double A = dot(d, mat_vec(ic, d));
                    double B = dot(mo, mat_vec(ic, d));
                    double K = dot(mo, mat_vec(ic, mo));
                    if (!(A > 0.0) || !std::isfinite(A) || !std::isfinite(B) ||
                        !std::isfinite(K)) {
                        continue; // snapshot validation makes this unreachable
                    }
                    double disc = B * B - A * (K - kSupportQ);
                    if (disc < 0.0) continue;
                    double sq = std::sqrt(disc) / A;
                    double tc = B / A;
                    double t0 = tc - sq;
                    double t1 = tc + sq;
                    t0 = std::max(t0, a);
                    t1 = std::min(t1, b);
                    if (t1 <= t0) continue;
                    LineKernel lk;
                    lk.A = A;
                    lk.tc = tc;
                    lk.fmin = K - B * B / A;
                    lk.t0 = t0;
                    lk.t1 = t1;
                    lk.extinction = kd.kernel.extinction;
                    lk.source = kd.kernel.source;
                    active.push_back(lk);
                }
            } else {
                stack.push_back(node.left);
                stack.push_back(node.right);
            }
        }
        return active;
    }
};

GaussianMedium::GaussianMedium()
    : snapshot(std::make_shared<Snapshot>()) {}

GaussianMedium::GaussianMedium(std::vector<GaussianKernel> kernels)
    : snapshot(std::make_shared<Snapshot>()) {
    set_kernels(std::move(kernels));
}

std::size_t GaussianMedium::size() const { return snapshot->kernels.size(); }

bool GaussianMedium::empty() const { return snapshot->kernels.empty(); }

void GaussianMedium::set_kernels(std::vector<GaussianKernel> kernels) {
    // Validate the complete candidate snapshot before touching the live one.
    std::vector<KernelData> prepared;
    prepared.reserve(kernels.size());
    for (const GaussianKernel& k : kernels) {
        if (!is_finite(k.mean) || !is_finite(k.source))
            throw std::invalid_argument("GaussianMedium: non-finite kernel field");
        for (double c : k.covariance) {
            if (!std::isfinite(c))
                throw std::invalid_argument("GaussianMedium: non-finite covariance");
        }
        if (!std::isfinite(k.extinction) || k.extinction < 0.0)
            throw std::invalid_argument("GaussianMedium: extinction must be finite and >= 0");
        if (k.source.x() < 0.0 || k.source.y() < 0.0 || k.source.z() < 0.0)
            throw std::invalid_argument("GaussianMedium: source components must be >= 0");

        Mat3 cov = kernel_matrix(k);
        Mat3 L;
        double d[3];
        if (!ldl_decompose(cov, L, d))
            throw std::invalid_argument("GaussianMedium: covariance must be positive definite");
        Mat3 inv = inverse_from_ldl(L, d);
        for (double v : inv.m) {
            if (!std::isfinite(v))
                throw std::invalid_argument("GaussianMedium: non-finite inverse covariance");
        }

        KernelData kd;
        kd.kernel = k;
        kd.inv_cov = inv;
        for (int axis = 0; axis < 3; ++axis) {
            double half = 3.0 * std::sqrt(std::max(0.0, cov.at(axis, axis)));
            if (!std::isfinite(half))
                throw std::invalid_argument("GaussianMedium: non-finite support bounds");
            kd.box.lo[axis] = k.mean[axis] - half;
            kd.box.hi[axis] = k.mean[axis] + half;
        }
        prepared.push_back(std::move(kd));
    }

    auto next = std::make_shared<Snapshot>();
    next->kernels = std::move(prepared);
    next->build_bvh();
    for (const BvhNode& n : next->nodes) {
        if (!std::isfinite(n.box.lo.x()) || !std::isfinite(n.box.hi.x()))
            throw std::invalid_argument("GaussianMedium: non-finite hierarchy bounds");
    }
    // Replace the live snapshot only once the candidate is fully built.
    snapshot = std::move(next);
}

namespace {

void validate_query(const Ray& ray, const Interval& interval) {
    if (!is_finite(ray.origin()) || !is_finite(ray.direction()))
        throw std::invalid_argument("GaussianMedium query: ray must be finite");
    if (ray.direction().near_zero())
        throw std::invalid_argument("GaussianMedium query: ray direction must be non-zero");
    if (!std::isfinite(interval.min) || interval.min < 0.0)
        throw std::invalid_argument("GaussianMedium query: interval min must be finite and >= 0");
    if (std::isnan(interval.max) || interval.max < interval.min)
        throw std::invalid_argument("GaussianMedium query: illegal interval");
}

double f_at(const LineKernel& lk, double t) {
    double dt = t - lk.tc;
    return lk.A * dt * dt + lk.fmin;
}

LineProfile make_profile(const LineKernel& lk) {
    LineProfile p;
    p.amp = std::exp(-lk.fmin / 2.0) *
            std::sqrt(pi / (2.0 * lk.A));
    p.zscale = std::sqrt(lk.A / 2.0);
    p.tc = lk.tc;
    return p;
}

// Optical-thickness line integral |D| * sum_i ext_i * int_g over [lo, hi].
double optical_thickness(const std::vector<LineKernel>& active,
                         const std::vector<LineProfile>& profiles,
                         const Vector3& direction, double lo, double hi) {
    double tau = 0.0;
    for (std::size_t i = 0; i < active.size(); ++i) {
        const LineKernel& lk = active[i];
        double u = std::max(lo, lk.t0);
        double v = std::min(hi, lk.t1);
        if (v <= u) continue;
        tau += lk.extinction * (profiles[i].phi(v) - profiles[i].phi(u));
    }
    return direction.length() * tau;
}

struct SegmentSample {
    Radiance estimate;      // Simpson estimate of E over the segment
    Radiance m4;            // upper bound on |d4/dt4 (T*j_c)| over the segment
};

// Probabilists' Hermite polynomials He_n(y), n <= 4, with
// d^n g/dt^n = A^(n/2) (-1)^n He_n(y) g, y = sqrt(A) (t - tc).
double hermite(int n, double y) {
    double y2 = y * y;
    switch (n) {
        case 0: return 1.0;
        case 1: return y;
        case 2: return y2 - 1.0;
        case 3: return y * (y2 - 3.0);
        case 4: return y2 * y2 - 6.0 * y2 + 3.0;
        default: return 0.0;
    }
}

// Critical points of He_n(y) exp(-y^2/2) are the roots of He_{n+1}. All roots
// of He_1 .. He_5, used as enclosing extrema candidates.
const std::vector<double>& hermite_roots() {
    static const std::vector<double> roots = [] {
        const double s3 = std::sqrt(3.0);
        const double a6 = std::sqrt(6.0);
        const double a10 = std::sqrt(10.0);
        std::vector<double> r;
        auto add = [&](double y) { r.push_back(y); };
        add(0.0);                                   // He_1, He_3, He_5
        add(1.0); add(-1.0);                        // He_2
        add(s3); add(-s3);                          // He_3
        add(std::sqrt(3.0 + a6)); add(-std::sqrt(3.0 + a6)); // He_4
        add(std::sqrt(3.0 - a6)); add(-std::sqrt(3.0 - a6));
        add(std::sqrt(5.0 + a10)); add(-std::sqrt(5.0 + a10)); // He_5
        add(std::sqrt(5.0 - a10)); add(-std::sqrt(5.0 - a10));
        std::sort(r.begin(), r.end());
        r.erase(std::unique(r.begin(), r.end(),
                            [](double x, double y) {
                                return std::fabs(x - y) < 1e-15;
                            }),
                r.end());
        return r;
    }();
    return roots;
}

// Enclosing maximum of |d^n g / dt^n| over t in [cu, cv], n = 0..4.
double gaussian_derivative_bound(const LineKernel& lk, int n, double cu,
                                 double cv) {
    const double sqrt_A = std::sqrt(lk.A);
    double ylo = sqrt_A * (cu - lk.tc);
    double yhi = sqrt_A * (cv - lk.tc);
    if (ylo > yhi) std::swap(ylo, yhi);

    double best = 0.0;
    auto consider = [&](double y) {
        if (y < ylo || y > yhi) return;
        double value = std::pow(lk.A, 0.5 * n) * std::fabs(hermite(n, y)) *
                       std::exp(-0.5 * (y * y + lk.fmin));
        best = std::max(best, value);
    };
    consider(ylo);
    consider(yhi);
    for (double y : hermite_roots()) consider(y);
    return best;
}

// g(t) at a point known to lie inside the kernel support.
double gaussian_value(const LineKernel& lk, double t) {
    return std::exp(-f_at(lk, t) / 2.0);
}

// One elementary interval: the member kernel set is constant and smooth over
// [lo, hi] because every kernel support boundary is a partition node.
struct ElementaryInterval {
    double lo, hi;
    double tau_entry = 0.0; // exact tau(a, lo) over the kernels active here
    std::vector<int> members;
};

// Partition [a, bend] at every clipped kernel support boundary. Each returned
// interval also carries the exact optical depth tau(a, lo) over its members.
std::vector<ElementaryInterval> partition_line(
    const std::vector<LineKernel>& active,
    const std::vector<LineProfile>& profiles, const Vector3& direction,
    double a, double bend) {
    struct Event {
        double t;
        int kind; // +1 entry, -1 exit
        int kernel;
    };
    std::vector<Event> events;
    events.reserve(2 * active.size());
    std::vector<double> breaks;
    breaks.reserve(2 * active.size() + 2);
    breaks.push_back(a);
    breaks.push_back(bend);
    for (int i = 0; i < static_cast<int>(active.size()); ++i) {
        double t0 = std::max(active[i].t0, a);
        double t1 = std::min(active[i].t1, bend);
        if (t1 <= t0) continue;
        events.push_back(Event{t0, +1, i});
        events.push_back(Event{t1, -1, i});
        breaks.push_back(t0);
        breaks.push_back(t1);
    }
    std::sort(breaks.begin(), breaks.end());
    breaks.erase(std::unique(breaks.begin(), breaks.end(),
                             [](double x, double y) {
                                 return std::fabs(x - y) <=
                                        1e-12 * std::max(1.0, std::fabs(y));
                             }),
                 breaks.end());
    std::sort(events.begin(), events.end(),
              [](const Event& e1, const Event& e2) { return e1.t < e2.t; });

    std::vector<ElementaryInterval> intervals;
    std::vector<char> is_active(active.size(), 0);
    std::size_t ev = 0;
    double running_tau = 0.0; // exact tau(a, breaks[k]) over every kernel
    for (std::size_t k = 0; k + 1 < breaks.size(); ++k) {
        double lo = breaks[k];
        double hi = breaks[k + 1];
        while (ev < events.size() && events[ev].t <= lo) {
            is_active[events[ev].kernel] = events[ev].kind > 0;
            ++ev;
        }
        if (hi <= lo) continue;
        ElementaryInterval ei;
        ei.lo = lo;
        ei.hi = hi;
        // Optical depth already accumulated before this interval, including
        // kernels that have already exited.
        ei.tau_entry = running_tau;
        double delta = 0.0;
        for (int i = 0; i < static_cast<int>(active.size()); ++i) {
            if (!is_active[i]) continue;
            ei.members.push_back(i);
            delta += active[i].extinction *
                     (profiles[i].phi(hi) - profiles[i].phi(lo));
        }
        running_tau += direction.length() * delta;
        intervals.push_back(std::move(ei));
    }
    return intervals;
}

// Evaluate one emission segment whose active kernel set is constant and
// smooth over the whole interval (support boundaries are partition nodes).
// Composite Simpson on the segment with a rigorous 4th-derivative bound:
// |integral - S| <= |D| * length^5 / 2880 * max |h''''|, where
// h(t) = exp(-tau(a,t)) j(t); all derivative maxima are enclosed from the
// Hermite form of the Gaussian line profiles.
SegmentSample evaluate_segment(const std::vector<LineKernel>& active,
                               const std::vector<LineProfile>& profiles,
                               const ElementaryInterval& ei,
                               const Vector3& direction,
                               double u, double v) {
    const double mid = 0.5 * (u + v);
    const double length = v - u;
    const double ds = direction.length();

    // Enclosures on the first four derivatives of j_c and tau over [u, v].
    Radiance J[5] = {Radiance(0, 0, 0), Radiance(0, 0, 0), Radiance(0, 0, 0),
                     Radiance(0, 0, 0), Radiance(0, 0, 0)};
    double Tn[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
    Radiance j_u(0, 0, 0), j_mid(0, 0, 0), j_v(0, 0, 0);
    // Member-only optical-depth increments tau(ei.lo, t), physical units.
    double inc_u = 0.0, inc_mid = 0.0, inc_v = 0.0;

    for (int idx : ei.members) {
        const LineKernel& lk = active[idx];
        double bounds[5];
        for (int n = 0; n <= 4; ++n)
            bounds[n] = gaussian_derivative_bound(lk, n, u, v);
        for (int n = 1; n <= 4; ++n) Tn[n] += lk.extinction * bounds[n];

        double g_u = gaussian_value(lk, u);
        double g_mid = gaussian_value(lk, mid);
        double g_v = gaussian_value(lk, v);
        double du = lk.extinction *
                    (profiles[idx].phi(u) - profiles[idx].phi(ei.lo));
        double dm = lk.extinction *
                    (profiles[idx].phi(mid) - profiles[idx].phi(ei.lo));
        double dv = lk.extinction *
                    (profiles[idx].phi(v) - profiles[idx].phi(ei.lo));
        inc_u += du;
        inc_mid += dm;
        inc_v += dv;
        for (int c = 0; c < 3; ++c) {
            double w = lk.extinction * lk.source[c];
            for (int n = 0; n <= 4; ++n) J[n][c] += w * bounds[n];
            j_u[c] += w * g_u;
            j_mid[c] += w * g_mid;
            j_v[c] += w * g_v;
        }
    }

    // Physical optical depths tau(a, t) anchored at the interval's exact
    // entry depth; only member kernels are ever touched.
    double tau_u = ei.tau_entry + ds * inc_u;
    double tau_mid = ei.tau_entry + ds * inc_mid;
    double tau_v = ei.tau_entry + ds * inc_v;

    // Exact integrand values h(t) = exp(-tau) j(t) at the Simpson nodes.
    Radiance h_u = Radiance(std::exp(-tau_u), std::exp(-tau_u),
                            std::exp(-tau_u)) * j_u;
    Radiance h_mid = Radiance(std::exp(-tau_mid), std::exp(-tau_mid),
                              std::exp(-tau_mid)) * j_mid;
    Radiance h_v = Radiance(std::exp(-tau_v), std::exp(-tau_v),
                            std::exp(-tau_v)) * j_v;
    SegmentSample s;
    for (int c = 0; c < 3; ++c)
        s.estimate[c] = ds * length / 6.0 *
                        (h_u[c] + 4.0 * h_mid[c] + h_v[c]);

    // Faà di Bruno enclosure of |(e^-tau j)''''| with T = e^-tau <= 1.
    // tau is the physical optical depth, so its t-derivatives carry |D|.
    const double t1 = ds * Tn[1], t2 = ds * Tn[2], t3 = ds * Tn[3],
                 t4 = ds * Tn[4];
    for (int c = 0; c < 3; ++c) {
        s.m4[c] =
            J[4][c] +
            4.0 * t1 * J[3][c] +
            (6.0 * t2 + 6.0 * t1 * t1) * J[2][c] +
            (4.0 * t3 + 12.0 * t1 * t2 + 4.0 * t1 * t1 * t1) * J[1][c] +
            (t4 + 3.0 * t2 * t2 + 4.0 * t1 * t3 + 6.0 * t1 * t1 * t2 +
             t1 * t1 * t1 * t1) * J[0][c];
    }
    return s;
}

} // namespace

bool GaussianMedium::segment(const Ray& ray, const Interval& interval,
                             double abs_error, std::uint64_t max_work,
                             SegmentResult& result, MediumStats& stats) const {
    validate_query(ray, interval);
    if (!std::isfinite(abs_error) || abs_error <= 0.0)
        throw std::invalid_argument("GaussianMedium::segment: abs_error must be a positive finite number");
    if (max_work < 1)
        throw std::invalid_argument("GaussianMedium::segment: max_work must be a positive integer");

    stats = MediumStats{};
    const double a = interval.min;
    const double b = interval.max;

    // Zero-length, empty or extinction-free media are exact zero work.
    if (a == b || snapshot->kernels.empty()) {
        SegmentResult exact;
        exact.transmittance = 1.0;
        result = exact;
        return true;
    }

    std::vector<LineKernel> active = snapshot->gather(ray, a, b, stats);
    if (active.empty()) {
        SegmentResult exact;
        exact.transmittance = 1.0;
        result = exact;
        return true;
    }

    bool any_extinction = false;
    bool any_emission = false;
    double support_end = a;
    for (const LineKernel& lk : active) {
        if (lk.extinction > 0.0) any_extinction = true;
        if (lk.extinction > 0.0 &&
            (lk.source.x() > 0.0 || lk.source.y() > 0.0 || lk.source.z() > 0.0))
            any_emission = true;
        support_end = std::max(support_end, lk.t1);
    }
    double bend = std::min(b, support_end);
    if (!any_extinction || bend <= a) {
        SegmentResult exact;
        exact.transmittance = 1.0;
        result = exact;
        return true;
    }

    std::vector<LineProfile> profiles;
    profiles.reserve(active.size());
    for (const LineKernel& lk : active) profiles.push_back(make_profile(lk));

    // Transmittance from the exact truncated Gaussian line integrals.
    double tau_total = optical_thickness(active, profiles, ray.direction(), a, bend);
    double trans = std::exp(-std::max(0.0, tau_total));

    if (!any_emission) {
        SegmentResult exact;
        exact.transmittance = trans;
        result = exact;
        return true;
    }

    // Adaptive midpoint integration with max-error-first refinement. Each
    // segment carries a rigorous |D| * length^3 / 24 * max|h''| error bound;
    // always splitting the worst segment drives the total bound below the
    // requested tolerance with a near-minimal number of evaluations.
    const double error_scale = 1.0 + 1.0e-10;
    std::uint64_t used = 0;

    struct Work {
        const ElementaryInterval* ei;
        double u, v;
        Radiance estimate;    // Simpson estimate of the segment integral
        Radiance seg_error;   // L^5 / 2880 * |D| * max|h''''| enclosure
    };
    struct WorkLess {
        bool operator()(const Work& a, const Work& b) const {
            double ma = std::max({a.seg_error.x(), a.seg_error.y(),
                                  a.seg_error.z()});
            double mb = std::max({b.seg_error.x(), b.seg_error.y(),
                                  b.seg_error.z()});
            return ma < mb; // max-heap on the worst channel
        }
    };
    std::priority_queue<Work, std::vector<Work>, WorkLess> heap;
    Radiance bound(0, 0, 0);

    auto make_work = [&](const ElementaryInterval* ei, double u,
                         double v) -> Work {
        SegmentSample s = evaluate_segment(active, profiles, *ei,
                                           ray.direction(), u, v);
        ++used;
        ++stats.interval_evaluations;
        Work w;
        w.ei = ei;
        w.u = u;
        w.v = v;
        w.estimate = s.estimate;
        double param_length = v - u;
        double coeff = ray.direction().length() *
                       std::pow(param_length, 5) / 2880.0;
        for (int c = 0; c < 3; ++c) w.seg_error[c] = coeff * s.m4[c];
        return w;
    };

    std::vector<ElementaryInterval> intervals =
        partition_line(active, profiles, ray.direction(), a, bend);
    for (const ElementaryInterval& ei : intervals) {
        if (ei.members.empty()) continue;
        if (used >= max_work) return false;
        Work w = make_work(&ei, ei.lo, ei.hi);
        for (int c = 0; c < 3; ++c) bound[c] += w.seg_error[c];
        heap.push(std::move(w));
    }

    auto within_tolerance = [&]() {
        for (int c = 0; c < 3; ++c) {
            if (bound[c] * error_scale >= abs_error) return false;
        }
        return true;
    };

    while (!within_tolerance()) {
        Work worst = heap.top();
        heap.pop();
        for (int c = 0; c < 3; ++c) bound[c] -= worst.seg_error[c];
        double mid = 0.5 * (worst.u + worst.v);
        if (mid <= worst.u || mid >= worst.v) {
            // Cannot refine further (float resolution): keep this leaf.
            heap.push(worst);
            for (int c = 0; c < 3; ++c) bound[c] += worst.seg_error[c];
            break;
        }
        if (used + 2 > max_work) return false; // budget exhausted, no partial result
        Work left = make_work(worst.ei, worst.u, mid);
        Work right = make_work(worst.ei, mid, worst.v);
        for (int c = 0; c < 3; ++c)
            bound[c] += left.seg_error[c] + right.seg_error[c];
        heap.push(std::move(left));
        heap.push(std::move(right));
    }

    Radiance estimate(0, 0, 0);
    while (!heap.empty()) {
        const Work& w = heap.top();
        for (int c = 0; c < 3; ++c) estimate[c] += w.estimate[c];
        heap.pop();
    }

    SegmentResult out;
    out.transmittance = trans;
    out.emission = estimate;
    for (int c = 0; c < 3; ++c) {
        double e = bound[c] * error_scale;
        out.error[c] = (e > abs_error) ? abs_error : e;
        if (!std::isfinite(out.emission[c]) || !std::isfinite(out.error[c]))
            return false;
    }
    if (!std::isfinite(out.transmittance) || out.transmittance < 0.0 ||
        out.transmittance > 1.0)
        return false;
    result = out;
    return true;
}

double GaussianMedium::transmittance(const Ray& ray, const Interval& interval,
                                     MediumStats& stats) const {
    validate_query(ray, interval);
    stats = MediumStats{};
    const double a = interval.min;
    const double b = interval.max;
    if (a == b || snapshot->kernels.empty()) return 1.0;

    std::vector<LineKernel> active = snapshot->gather(ray, a, b, stats);
    if (active.empty()) return 1.0;

    double support_end = a;
    bool any_extinction = false;
    for (const LineKernel& lk : active) {
        if (lk.extinction > 0.0) any_extinction = true;
        support_end = std::max(support_end, lk.t1);
    }
    if (!any_extinction) return 1.0;
    double bend = std::min(b, support_end);
    if (bend <= a) return 1.0;

    std::vector<LineProfile> profiles;
    profiles.reserve(active.size());
    for (const LineKernel& lk : active) profiles.push_back(make_profile(lk));
    double tau = optical_thickness(active, profiles, ray.direction(), a, bend);
    double T = std::exp(-std::max(0.0, tau));
    // Keep the reported value inside [0, 1]; line-integral error is far below
    // the 1e-10 absolute contract tolerance.
    return std::max(0.0, std::min(1.0, T));
}
