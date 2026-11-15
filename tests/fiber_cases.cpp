// Fiber geometry, scattering model and transport tests.
// Registered through task_support/check.h; this file defines no main().
#include "fiber_renderer.h"
#include "task_support/check.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double TWO_PI = 6.28318530717958647692;

bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b));
}
bool near3(const dvec3& a, const dvec3& b, double tol = 1e-9) {
    return near(a.x, b.x, tol) && near(a.y, b.y, tol) && near(a.z, b.z, tol);
}
dvec3 rodrigues(const dvec3& v, const dvec3& axis, double angle) {
    double c = std::cos(angle), s = std::sin(angle);
    return v * c + axis * (axis * v) * (1.0 - c) + cross(axis, v) * s;
}

FiberMaterial make_mat() {
    FiberMaterial m;
    m.eta = 1.45;
    m.sigma_a = {0.35, 0.8, 1.6};   // blue absorbed most: warm tinted fiber
    m.beta_m = 0.18;
    m.beta_n = 0.3;
    return m;
}

// Stream returning a fixed script of samples; lets tests pin bounce draws.
struct ScriptedStream : FiberSampleStream {
    std::vector<double> values;
    std::size_t idx = 0;
    double next() override {
        if (idx >= values.size()) return 0.0;
        return values[idx++];
    }
    std::uint64_t consumed() const override {
        return static_cast<std::uint64_t>(idx);
    }
};

// ============================= geometry ====================================

int geom_oblique_section_h() {
    Fiber f;
    f.A = {0, 0, -2};
    f.B = {0, 0, 2};
    f.radius = 1.0;
    f.material = make_mat();

    dvec3 orig{-3, 0.5, 0.3}, dir{1, 0, 0};
    FiberHit hit;
    TASK_CHECK(fiber_intersect(f, 7, orig, dir, 0.001, 1e10, hit));
    TASK_CHECK(hit.fiber_id == 7);
    TASK_CHECK(hit.u >= 0.0 && hit.u <= 1.0);
    TASK_CHECK(near(hit.u, (0.3 + 2.0) / 4.0));
    double expected_x = -std::sqrt(1.0 - 0.25);
    TASK_CHECK(near(hit.point.x, expected_x, 1e-12));
    TASK_CHECK(near(hit.point.y, 0.5, 1e-12));
    TASK_CHECK(near(hit.point.z, 0.3, 1e-12));
    TASK_CHECK(near(hit.normal.norm(), 1.0, 1e-12));
    TASK_CHECK(near(hit.normal * dvec3{0, 0, 1}, 0.0, 1e-12));
    // T = +z, wo = -x: X = -x, Y = cross(T,X) = -y; N = (-sqrt(.75),.5,0)
    // so h = dot(N, Y) = -0.5. The material must receive this geometric h,
    // not a centerline value of zero.
    TASK_CHECK(near(hit.h, -0.5, 1e-12));
    TASK_CHECK(near(hit.t, 3.0 + expected_x, 1e-12));

    // Half-open interval convention [tmin, tmax): the lower bound is
    // inclusive, the upper bound exclusive. Pushing tmin past the entry
    // root exposes the exit root of the same side surface.
    FiberHit h2;
    TASK_CHECK(fiber_intersect(f, 0, orig, dir, hit.t, 1e10, h2));
    TASK_CHECK(near(h2.t, hit.t, 1e-12));
    FiberHit h4;
    TASK_CHECK(fiber_intersect(f, 0, orig, dir, hit.t + 1e-6, 1e10, h4));
    TASK_CHECK(h4.t > hit.t);
    TASK_CHECK(near3(h4.point, orig + dir * h4.t, 1e-10));
    TASK_CHECK(!fiber_intersect(f, 0, orig, dir, h4.t + 1e-6, 1e10, h2));
    FiberHit h3;
    TASK_CHECK(!fiber_intersect(f, 0, orig, dir, 0.001, hit.t, h3));
    TASK_CHECK(fiber_intersect(f, 0, orig, dir, 0.001, hit.t + 1e-6, h3));
    TASK_CHECK(near(h3.t, hit.t, 1e-12));
    return 0;
}

int geom_tangent_parallel_caps() {
    Fiber f;
    f.A = {0, 0, -2};
    f.B = {0, 0, 2};
    f.radius = 1.0;
    f.material = make_mat();

    // Tangent ray: zero discriminant still counts as a side hit.
    FiberHit hit;
    TASK_CHECK(fiber_intersect(f, 0, {-3, 1.0, 0.3}, {1, 0, 0},
                               0.001, 1e10, hit));
    TASK_CHECK(near(hit.point.x, 0.0, 1e-10));
    TASK_CHECK(near3(hit.normal, {0, 1, 0}, 1e-10));

    // Axis-parallel rays have no lateral surface to cross, even inside.
    TASK_CHECK(!fiber_intersect(f, 0, {0.5, 0, 0}, {0, 0, 1},
                                0.001, 1e10, hit));
    TASK_CHECK(!fiber_intersect(f, 0, {2, 0, 0}, {0, 0, 1},
                                0.001, 1e10, hit));

    // Ray crossing the infinite cylinder beyond the endpoints: open
    // cylinder, no end caps.
    TASK_CHECK(!fiber_intersect(f, 0, {-3, 0.0, 3.0}, {1, 0, 0},
                                0.001, 1e10, hit));
    TASK_CHECK(!fiber_intersect(f, 0, {-3, 0.0, -3.0}, {1, 0, 0},
                                0.001, 1e10, hit));

    // Non-unit and non-finite rays are rejected.
    TASK_CHECK(!fiber_intersect(f, 0, {-3, .5, .3}, {2, 0, 0},
                                0.001, 1e10, hit));
    TASK_CHECK(!fiber_intersect(f, 0, {std::nan(""), 0, 0}, {1, 0, 0},
                                0.001, 1e10, hit));
    return 0;
}

int geom_reverse_endpoints() {
    Fiber f;
    f.A = {0, 0, -2};
    f.B = {0, 0, 2};
    f.radius = 1.0;
    f.material = make_mat();
    Fiber g = f;
    g.A = f.B;
    g.B = f.A;

    dvec3 orig{-3, 0.5, 0.3}, dir{1, 0, 0};
    FiberHit hf, hg;
    TASK_CHECK(fiber_intersect(f, 0, orig, dir, 0.001, 1e10, hf));
    TASK_CHECK(fiber_intersect(g, 1, orig, dir, 0.001, 1e10, hg));
    TASK_CHECK(near(hf.t, hg.t, 1e-12));
    TASK_CHECK(near3(hf.point, hg.point, 1e-12));
    TASK_CHECK(near3(hf.normal, hg.normal, 1e-12));
    TASK_CHECK(near(hf.u + hg.u, 1.0, 1e-12));
    // T flips, X stays, Y flips -> the cross-section coordinate changes sign.
    TASK_CHECK(near(hf.h, -hg.h, 1e-12));
    return 0;
}

int geom_rigid_rotation_equivalence() {
    Fiber f;
    f.A = {0, -1, 0.2};
    f.B = {0, 1, -0.2};
    f.radius = 0.3;
    f.material = make_mat();

    dvec3 axis{0.3, -0.7, 0.65};
    axis = axis.normalized();
    double angle = 0.91;
    Fiber g = f;
    g.A = rodrigues(f.A, axis, angle);
    g.B = rodrigues(f.B, axis, angle);

    dvec3 orig{-2, 0.35, 0.4};
    dvec3 dir = (dvec3{0, 0.1, -0.1} - orig).normalized();
    dvec3 orig_r = rodrigues(orig, axis, angle);
    dvec3 dir_r = rodrigues(dir, axis, angle);
    TASK_CHECK(near(dir_r.norm(), 1.0, 1e-14));

    FiberHit hf, hg;
    TASK_CHECK(fiber_intersect(f, 0, orig, dir, 0.001, 1e10, hf));
    TASK_CHECK(fiber_intersect(g, 0, orig_r, dir_r, 0.001, 1e10, hg));
    TASK_CHECK(near(hf.t, hg.t, 1e-10));
    TASK_CHECK(near3(hg.point, rodrigues(hf.point, axis, angle), 1e-10));
    TASK_CHECK(near3(hg.normal, rodrigues(hf.normal, axis, angle), 1e-10));
    TASK_CHECK(near(hf.h, hg.h, 1e-10));

    // Same physical outgoing direction must give identical K and pdf.
    dvec3 Tf = (f.B - f.A).normalized();
    dvec3 Tg = (g.B - g.A).normalized();
    FiberFrame frf = make_fiber_frame(Tf, dir * -1.0);
    FiberFrame frg = make_fiber_frame(Tg, dir_r * -1.0);
    for (int k = 0; k < 12; ++k) {
        dvec3 wi = dvec3{std::cos(2.1*k + .3), std::sin(1.7*k),
                         std::cos(0.8*k + 1.1)}.normalized();
        dvec3 wi_r = rodrigues(wi, axis, angle);
        dvec3 Kf = fiber_evaluate_K(f.material, frf, hf.h, f.radius, wi);
        dvec3 Kg = fiber_evaluate_K(g.material, frg, hg.h, g.radius, wi_r);
        double pf = fiber_pdf(f.material, frf, hf.h, f.radius, wi);
        double pg = fiber_pdf(g.material, frg, hg.h, g.radius, wi_r);
        TASK_CHECK(near3(Kf, Kg, 1e-12));
        TASK_CHECK(near(pf, pg, 1e-12));
    }
    return 0;
}

int geom_validation() {
    Fiber f;
    f.A = {0, 0, -1};
    f.B = {0, 0, 1};
    f.radius = 0.2;
    f.material = make_mat();
    TASK_CHECK(fiber_valid(f));

    Fiber bad = f;
    bad.B = bad.A;
    TASK_CHECK(!fiber_valid(bad));
    bad = f; bad.radius = 0.0;
    TASK_CHECK(!fiber_valid(bad));
    bad = f; bad.radius = -1.0;
    TASK_CHECK(!fiber_valid(bad));
    bad = f; bad.material.eta = 1.0;
    TASK_CHECK(!fiber_valid(bad));
    bad = f; bad.material.eta = 4.0;
    TASK_CHECK(!fiber_valid(bad));
    bad = f; bad.material.beta_m = 0.01;
    TASK_CHECK(!fiber_valid(bad));
    bad = f; bad.material.beta_n = 1.5;
    TASK_CHECK(!fiber_valid(bad));
    bad = f; bad.material.sigma_a = {0, -0.1, 0};
    TASK_CHECK(!fiber_valid(bad));

    FiberHit hit;
    TASK_CHECK(!fiber_intersect(bad, 0, {0,0,0}, {1,0,0}, 0.001, 1e10, hit));
    return 0;
}

// ============================ scattering ===================================

FiberFrame standard_frame() {
    return make_fiber_frame({0, 0, 1}, {-1, 0, 0}); // so=0, co=1, X=-x, Y=-y
}

int scat_fresnel_and_chord() {
    FiberMaterial m = make_mat();
    FiberFrame fr = standard_frame();
    // h = 0: normal-incidence geometry, gamma angles zero, chord = 2r.
    FiberLobes L = fiber_compute_lobes(m, fr, 0.0, 0.25);
    double Fn = (1.0 - m.eta) / (1.0 + m.eta);
    Fn *= Fn;
    TASK_CHECK(near(L.F, Fn, 1e-12));
    TASK_CHECK(near(L.gamma_o, 0.0, 1e-14));
    TASK_CHECK(near(L.gamma_t, 0.0, 1e-14));
    TASK_CHECK(near(L.eta_p, m.eta, 1e-12));
    TASK_CHECK(near(L.chord, 0.5, 1e-12));
    for (int c = 0; c < 3; ++c)
        TASK_CHECK(near(L.C[c], std::exp(-m.sigma_a[c] * 0.5), 1e-12));

    // Grazing incidence: Fresnel tends to one.
    FiberLobes Lg = fiber_compute_lobes(m, fr, 0.999999, 0.25);
    TASK_CHECK(Lg.F > 0.99);

    // Exact unpolarized Fresnel against an independent scalar computation.
    double cos_i = 0.6;
    double st2 = 1.0 - cos_i*cos_i;
    double ct = std::sqrt(1.0 - st2 / (m.eta*m.eta));
    double rs = (cos_i - m.eta*ct) / (cos_i + m.eta*ct);
    double rp = (m.eta*cos_i - ct) / (m.eta*cos_i + ct);
    double Fref = 0.5*(rs*rs + rp*rp);
    // Build a frame/h combination with co*cos(gamma_o) = cos_i: pick
    // co = 0.8, gamma_o = acos(0.75).
    dvec3 wo = (dvec3{-0.8, 0, 0.6}).normalized();
    FiberFrame fr2 = make_fiber_frame({0, 0, 1}, wo);
    TASK_CHECK(near(fr2.co, 0.8, 1e-12));
    double h = std::sin(std::acos(0.75));
    FiberLobes L2 = fiber_compute_lobes(m, fr2, h, 0.25);
    TASK_CHECK(near(L2.F, Fref, 1e-12));
    return 0;
}

int scat_quadrature_energy_and_pdf() {
    FiberMaterial m = make_mat();
    FiberFrame fr = standard_frame();
    double h = 0.35, r = 0.2;
    FiberLobes L = fiber_compute_lobes(m, fr, h, r);

    const int NS = 160, NP = 320;
    double ds = 2.0 / NS, dp = TWO_PI / NP;
    dvec3 intK{0,0,0};
    double intPdf = 0.0;
    for (int i = 0; i < NS; ++i) {
        double s = -1.0 + (i + 0.5) * ds;
        for (int j = 0; j < NP; ++j) {
            double phi = -PI + (j + 0.5) * dp;
            dvec3 wi = fr.T * s +
                       (fr.X * std::cos(phi) + fr.Y * std::sin(phi)) *
                       std::sqrt(std::max(0.0, 1.0 - s*s));
            dvec3 K = fiber_evaluate_K(m, fr, h, r, wi);
            double p = fiber_pdf(m, fr, h, r, wi);
            intK = intK + K * (ds * dp);
            intPdf += p * ds * dp;
        }
    }
    // Every longitudinal/azimuthal density is normalized, so the integral
    // of K over solid angle is exactly the sum of lobe attenuations.
    for (int c = 0; c < 3; ++c) {
        double total = L.A[0][c] + L.A[1][c] + L.A[2][c] + L.A[3][c];
        TASK_CHECK(near(intK[c], total, 2e-3));
    }
    TASK_CHECK(near(intPdf, 1.0, 2e-3));
    return 0;
}

int scat_stratified_sampling() {
    FiberMaterial m = make_mat();
    FiberFrame fr = standard_frame();
    double h = -0.4, r = 0.22;
    FiberLobes L = fiber_compute_lobes(m, fr, h, r);

    const int N = 16;
    const int SB = 4, PB = 4;
    long counts[SB][PB] = {};
    long total = 0;
    dvec3 mean_ratio{0,0,0};
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            double u1 = (i + 0.5) / N;
            double u2 = (j + 0.5) / N;
            double u3 = ((i * 7 + j * 13) % 256 + 0.5) / 256.0;
            FiberSample s = fiber_sample(m, fr, h, r, u1, u2, u3);
            TASK_CHECK(s.ok);
            TASK_CHECK(near(s.wi.norm(), 1.0, 1e-12));
            TASK_CHECK(s.pdf > 0.0);
            // Returned values must agree with the public evaluation API.
            dvec3 K = fiber_evaluate_K(m, fr, h, r, s.wi);
            double p = fiber_pdf(m, fr, h, r, s.wi);
            TASK_CHECK(near3(s.K, K, 1e-13));
            TASK_CHECK(near(s.pdf, p, 1e-13));
            double sv = s.wi * fr.T;
            double phi = std::atan2(s.wi * fr.Y, s.wi * fr.X);
            int sb = int((sv + 1.0) * 0.5 * SB);
            int pb = int((phi + PI) / TWO_PI * PB);
            if (sb >= SB) sb = SB - 1;
            if (pb >= PB) pb = PB - 1;
            ++counts[sb][pb];
            ++total;
            for (int c = 0; c < 3; ++c)
                mean_ratio[c] += s.K[c] / s.pdf;
        }
    }
    TASK_CHECK(total == N * N);

    // Expected region probabilities from an independent pdf quadrature.
    for (int a = 0; a < SB; ++a) {
        for (int b = 0; b < PB; ++b) {
            double prob = 0.0;
            const int QS = 24, QP = 24;
            for (int i = 0; i < QS; ++i) {
                double s = -1.0 + ((a + (i + .5) / QS) / SB) * 2.0;
                for (int j = 0; j < QP; ++j) {
                    double phi = -PI + ((b + (j + .5) / QP) / PB) * TWO_PI;
                    dvec3 wi = fr.T * s +
                               (fr.X * std::cos(phi) + fr.Y * std::sin(phi)) *
                               std::sqrt(std::max(0.0, 1.0 - s*s));
                    prob += fiber_pdf(m, fr, h, r, wi) *
                            (2.0 / SB / QS) * (TWO_PI / PB / QP);
                }
            }
            double expected = total * prob;
            double tol = 4.0 * std::sqrt(total * prob) + 2.0;
            TASK_CHECK(std::fabs(double(counts[a][b]) - expected) <= tol);
        }
    }

    // Monte Carlo mean of K/pdf equals the analytic four-lobe energy.
    for (int c = 0; c < 3; ++c) {
        double analytic = L.A[0][c] + L.A[1][c] + L.A[2][c] + L.A[3][c];
        TASK_CHECK(near(mean_ratio[c] / total, analytic, 5e-2));
    }
    return 0;
}

int scat_absorption_tail_and_grazing() {
    FiberMaterial m = make_mat();
    FiberFrame fr = standard_frame();
    double r = 0.3;
    FiberLobes L = fiber_compute_lobes(m, fr, 0.2, r);

    // Absorption is active and the merged high-order tail carries energy.
    for (int c = 0; c < 3; ++c) {
        TASK_CHECK(L.C[c] > 0.0 && L.C[c] < 1.0);
        TASK_CHECK(L.A[3][c] > 0.0);
        TASK_CHECK(L.A[2][c] > L.A[3][c]);
    }
    // Stronger absorption on blue shifts the transmitted color.
    TASK_CHECK(L.A[1].x > L.A[1].z);
    TASK_CHECK(L.A[3].x > L.A[3].z);

    // Without absorption the four lobes conserve energy exactly (they are
    // never renormalized, yet the geometric series sums to one).
    FiberMaterial clear = m;
    clear.sigma_a = {0, 0, 0};
    FiberLobes Lc = fiber_compute_lobes(clear, fr, 0.2, r);
    for (int c = 0; c < 3; ++c) {
        double tot = Lc.A[0][c] + Lc.A[1][c] + Lc.A[2][c] + Lc.A[3][c];
        TASK_CHECK(near(tot, 1.0, 1e-12));
    }

    // Grazing 0/0 limit: F -> 1, CF -> 1, A3 is defined as zero and no
    // non-finite values appear. For a clear fiber the closed form is
    // A3 = (1-F) F^2 <= 1-F, and it shrinks toward zero as the hit becomes
    // more grazing (the exact limit, not a renormalization jump).
    FiberLobes Lg0 = fiber_compute_lobes(clear, fr, 0.9999, r);
    FiberLobes Lg  = fiber_compute_lobes(clear, fr, 0.999999999, r);
    for (int c = 0; c < 3; ++c) {
        TASK_CHECK(std::isfinite(Lg.A[3][c]));
        TASK_CHECK(Lg.A[3][c] >= 0.0);
        TASK_CHECK(Lg.A[3][c] <= 1.0 - Lg.F + 1e-12);
        double tot = Lg.A[0][c] + Lg.A[1][c] + Lg.A[2][c] + Lg.A[3][c];
        TASK_CHECK(near(tot, 1.0, 1e-8));
        TASK_CHECK(Lg.A[3][c] < Lg0.A[3][c]);
    }

    // Absorbed energy really disappears: no rescaling back to one.
    FiberLobes La = fiber_compute_lobes(m, fr, 0.0, r);
    for (int c = 0; c < 3; ++c) {
        double tot = La.A[0][c] + La.A[1][c] + La.A[2][c] + La.A[3][c];
        TASK_CHECK(tot < 1.0 - 1e-4);
    }
    return 0;
}

int scat_sample_contract() {
    FiberMaterial m = make_mat();
    FiberFrame fr = standard_frame();
    double h = 0.1, r = 0.2;

    // u1 = 0 deterministically lands on the s = -1 pole, where azimuth
    // collapses and wi = -T.
    FiberSample pole = fiber_sample(m, fr, h, r, 0.0, 0.7, 0.2);
    TASK_CHECK(pole.ok);
    TASK_CHECK(near3(pole.wi, fr.T * -1.0, 1e-12));
    TASK_CHECK(pole.pdf > 0.0 && std::isfinite(pole.pdf));

    // Samples must live in [0,1); non-finite and out-of-range are refused.
    TASK_CHECK(!fiber_sample(m, fr, h, r, 1.0, 0.5, 0.5).ok);
    TASK_CHECK(!fiber_sample(m, fr, h, r, -0.1, 0.5, 0.5).ok);
    TASK_CHECK(!fiber_sample(m, fr, h, r, 0.5, std::nan(""), 0.5).ok);
    FiberMaterial bad = m;
    bad.eta = 0.5;
    TASK_CHECK(!fiber_sample(bad, fr, h, r, 0.5, 0.5, 0.5).ok);

    // Lobe probabilities sum to one and follow the RGB-mean rule.
    FiberLobes L = fiber_compute_lobes(m, fr, h, r);
    double probs[4];
    fiber_lobe_probabilities(L, probs);
    double psum = 0, means[4];
    for (int p = 0; p < 4; ++p) {
        means[p] = (L.A[p].x + L.A[p].y + L.A[p].z) / 3.0;
        psum += probs[p];
        TASK_CHECK(probs[p] >= 0.0 && probs[p] <= 1.0);
    }
    TASK_CHECK(near(psum, 1.0, 1e-14));
    for (int p = 0; p < 4; ++p)
        TASK_CHECK(near(probs[p], means[p] / (means[0]+means[1]+means[2]+means[3]),
                        1e-14));
    return 0;
}

// ============================= transport ===================================

dvec3 unit_z_fiber_origin() { return {-2, 0.25, 0.1}; }

Fiber make_z_fiber(int /*id*/, const FiberMaterial& m, double r = 0.12) {
    Fiber f;
    f.A = {0, -1.5, 0};
    f.B = {0, 1.5, 0};
    f.radius = r;
    f.material = m;
    return f;
}

int tr_miss_background() {
    FiberScene scene; // empty
    ScriptedStream st;
    int calls = 0;
    dvec3 bg_color{0.1, 0.2, 0.4};
    bool bg_unit_ok = true;
    auto bg = [&](const dvec3& d) -> dvec3 {
        ++calls;
        bg_unit_ok = bg_unit_ok && near(d.norm(), 1.0, 1e-14);
        return bg_color;
    };
    dvec3 c = trace_fiber(scene, {0, 0, 0}, {0, 0, -1}, 3, st, bg);
    TASK_CHECK(near3(c, bg_color, 1e-14));
    TASK_CHECK(calls == 1);
    TASK_CHECK(bg_unit_ok);
    TASK_CHECK(st.consumed() == 0);

    // Invalid scene and non-unit rays are rejected before tracing.
    scene.fibers.push_back(make_z_fiber(0, make_mat()));
    scene.fibers[0].B = scene.fibers[0].A;
    TASK_CHECK(near3(trace_fiber(scene, {0,0,0}, {0,0,-1}, 3, st, bg),
                     {0,0,0}, 0.0));
    TASK_CHECK(near3(trace_fiber(FiberScene{}, {0,0,0}, {0,0,-2}, 3, st, bg),
                     {0,0,0}, 0.0));
    return 0;
}

int tr_sphere_block_and_tie() {
    FiberScene scene;
    scene.fibers.push_back(make_z_fiber(0, make_mat()));
    dvec3 orig = unit_z_fiber_origin();
    dvec3 dir = (dvec3{0, 0, 0} - orig).normalized();

    // Sphere between the origin and the fiber: black zero.
    scene.occluders.push_back({{-1.2f, 0.15f, 0.06f}, 0.2f, Material{}});
    ScriptedStream st;
    dvec3 c = trace_fiber(scene, orig, dir, 3, st,
                          [](const dvec3&) { return dvec3{1, 1, 1}; });
    TASK_CHECK(near3(c, {0, 0, 0}, 0.0));
    TASK_CHECK(st.consumed() == 0);

    // Exact same distance: the sphere still wins over the fiber.
    FiberScene scene2;
    scene2.fibers.push_back(make_z_fiber(0, make_mat()));
    FiberHit fh;
    TASK_CHECK(fiber_intersect(scene2.fibers[0], 0, orig, dir,
                               0.001, 1e10, fh));
    Sphere sp;
    sp.center = to_vec3(orig + dir * (fh.t + 0.25));
    sp.radius = 0.25f;
    scene2.occluders.push_back(sp);
    double st_t;
    TASK_CHECK(ray_sphere_intersect_d(orig, dir, to_dvec3(sp.center), 0.25,
                                      0.001, 1e10, st_t));
    // The sphere center is stored in the original float vec3, so the tie
    // only holds to single-precision round-off; it is still closer or equal.
    TASK_CHECK(std::fabs(st_t - fh.t) < 1e-6);
    TASK_CHECK(st_t <= fh.t + 1e-9);
    ScriptedStream st2;
    dvec3 c2 = trace_fiber(scene2, orig, dir, 3, st2,
                           [](const dvec3&) { return dvec3{1, 1, 1}; });
    TASK_CHECK(near3(c2, {0, 0, 0}, 0.0));
    TASK_CHECK(st2.consumed() == 0);
    return 0;
}

int tr_single_bounce_weight_and_skip() {
    FiberScene scene;
    scene.fibers.push_back(make_z_fiber(0, make_mat()));
    dvec3 orig = unit_z_fiber_origin();
    dvec3 dir = (dvec3{0, 0, 0} - orig).normalized();

    FiberHit fh;
    TASK_CHECK(fiber_intersect(scene.fibers[0], 0, orig, dir,
                               0.001, 1e10, fh));
    dvec3 T = (scene.fibers[0].B - scene.fibers[0].A).normalized();
    FiberFrame fr = make_fiber_frame(T, dir * -1.0);

    // Pick a deterministic sample whose wi points back inside the cylinder;
    // the self-skip rule must keep the next segment from re-hitting it.
    ScriptedStream st;
    FiberSample chosen;
    for (int a = 1; a < 16 && !chosen.ok; ++a) {
        for (int b = 1; b < 16; ++b) {
            FiberSample s = fiber_sample(scene.fibers[0].material, fr,
                                         fh.h, scene.fibers[0].radius,
                                         a / 16.0, b / 16.0, 0.3);
            if (s.ok && s.wi * fh.normal < -0.3) { chosen = s; break; }
        }
    }
    TASK_CHECK(chosen.ok);
    st.values = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    // Overwrite the first three draws with the located script.
    // Re-run the same search to recover the exact (u1,u2,u3).
    bool found = false;
    for (int a = 1; a < 16 && !found; ++a) {
        for (int b = 1; b < 16; ++b) {
            FiberSample s = fiber_sample(scene.fibers[0].material, fr,
                                         fh.h, scene.fibers[0].radius,
                                         a / 16.0, b / 16.0, 0.3);
            if (s.ok && s.wi * fh.normal < -0.3) {
                st.values = {a / 16.0, b / 16.0, 0.3};
                found = true;
                break;
            }
        }
    }
    TASK_CHECK(found);

    dvec3 bg{0.9, 0.7, 0.5};
    dvec3 c = trace_fiber(scene, orig, dir, 1, st,
                          [&](const dvec3&) { return bg; });
    TASK_CHECK(st.consumed() == 3);                 // exactly one event
    dvec3 expected = cmul(bg, dvec3{chosen.K.x / chosen.pdf,
                                    chosen.K.y / chosen.pdf,
                                    chosen.K.z / chosen.pdf});
    TASK_CHECK(near3(c, expected, 1e-12));

    // Zero scatter budget at a fiber hit returns zero, not the background.
    ScriptedStream st0;
    TASK_CHECK(near3(trace_fiber(scene, orig, dir, 0, st0,
                                 [&](const dvec3&) { return bg; }),
                     {0, 0, 0}, 0.0));
    TASK_CHECK(st0.consumed() == 0);
    return 0;
}

int tr_two_fibers_and_reencounter() {
    FiberScene scene;
    scene.fibers.push_back(make_z_fiber(0, make_mat()));
    dvec3 orig = unit_z_fiber_origin();
    dvec3 dir = (dvec3{0, 0, 0} - orig).normalized();

    FiberHit h1;
    TASK_CHECK(fiber_intersect(scene.fibers[0], 0, orig, dir,
                               0.001, 1e10, h1));
    dvec3 T1 = (scene.fibers[0].B - scene.fibers[0].A).normalized();
    FiberFrame fr1 = make_fiber_frame(T1, dir * -1.0);

    // First event: fixed script.
    double s1[3] = {0.42, 0.71, 0.27};
    FiberSample smp1 = fiber_sample(scene.fibers[0].material, fr1,
                                    h1.h, scene.fibers[0].radius,
                                    s1[0], s1[1], s1[2]);
    TASK_CHECK(smp1.ok);

    // Place the second fiber across the sampled continuation ray, close
    // enough that later samples can reach fiber 0 again.
    dvec3 C2 = h1.point + smp1.wi * 0.5;
    dvec3 ref = std::fabs(smp1.wi.y) < 0.9 ? dvec3{0, 1, 0} : dvec3{1, 0, 0};
    dvec3 T2 = cross(smp1.wi, ref).normalized();
    Fiber f2;
    f2.A = C2 - T2 * 1.5;
    f2.B = C2 + T2 * 1.5;
    f2.radius = 0.12;
    FiberMaterial m2 = make_mat();
    m2.sigma_a = {1.4, 0.5, 0.2};     // green/red survive: cool tint
    f2.material = m2;
    scene.fibers.push_back(f2);

    // The scene guarantee: the two lateral surfaces never intersect, i.e.
    // the axes stay farther apart than the sum of radii.
    auto seg_distance = [](const dvec3& p0, const dvec3& p1,
                           const dvec3& q0, const dvec3& q1) {
        dvec3 u = p1 - p0, v = q1 - q0, w0 = p0 - q0;
        double a = u*u, b = u*v, c = v*v, d = u*w0, e = v*w0;
        double den = a*c - b*b;
        double s, sn, sd = den, tn;
        if (den < 1e-12) { sn = 0.0; sd = 1.0; }
        else { sn = b*e - c*d; if (sn < 0.0) { sn = 0.0; } else if (sn > sd) sn = sd; }
        tn = (b*sn + e) / c;
        if (tn < 0.0) { tn = 0.0; } else if (tn > 1.0) { tn = 1.0; }
        s = sn / sd;
        double best = 1e300;
        for (int ii = 0; ii < 2; ++ii) {
            double ss = ii == 0 ? s : (ii == 1 ? 0.0 : 1.0);
            for (int jj = 0; jj < 3; ++jj) {
                double tt = jj == 0 ? tn : (jj == 1 ? 0.0 : 1.0);
                double dist = (w0 + u*ss - v*tt).norm();
                best = std::min(best, dist);
            }
        }
        return best;
    };
    double axis_gap = seg_distance(scene.fibers[0].A, scene.fibers[0].B,
                                   f2.A, f2.B);
    TASK_CHECK(axis_gap > scene.fibers[0].radius + f2.radius + 1e-6);

    FiberHit h2;
    TASK_CHECK(fiber_intersect(f2, 1, h1.point, smp1.wi, 0.001, 1e10, h2));
    FiberFrame fr2 = make_fiber_frame(T2, smp1.wi * -1.0);

    // Search deterministic scripts for an event-2 direction that hits
    // fiber 0 again: a later bounce may re-encounter the skipped fiber.
    double s2[3] = {0, 0, 0};
    FiberSample smp2;
    bool found2 = false;
    for (int a = 1; a < 20 && !found2; ++a)
        for (int b = 1; b < 20 && !found2; ++b)
            for (int k = 1; k < 20 && !found2; ++k) {
                FiberSample s = fiber_sample(m2, fr2, h2.h, f2.radius,
                                             a / 20.0, b / 20.0, k / 20.0);
                FiberHit tmp;
                if (s.ok && fiber_intersect(scene.fibers[0], 0, h2.point,
                                            s.wi, 0.001, 1e10, tmp)) {
                    smp2 = s;
                    s2[0] = a / 20.0; s2[1] = b / 20.0; s2[2] = k / 20.0;
                    found2 = true;
                }
            }
    TASK_CHECK(found2);

    FiberHit h3;
    TASK_CHECK(fiber_intersect(scene.fibers[0], 0, h2.point, smp2.wi,
                               0.001, 1e10, h3));
    TASK_CHECK(h3.fiber_id == 0);
    FiberFrame fr3 = make_fiber_frame(T1, smp2.wi * -1.0);
    double s3[3] = {0.55, 0.2, 0.8};
    FiberSample smp3 = fiber_sample(scene.fibers[0].material, fr3,
                                    h3.h, scene.fibers[0].radius,
                                    s3[0], s3[1], s3[2]);
    TASK_CHECK(smp3.ok);
    // Final segment escapes into the background.
    FiberHit guard;
    TASK_CHECK(!fiber_intersect(scene.fibers[0], 0, h3.point, smp3.wi,
                                0.001, 1e10, guard));
    TASK_CHECK(!fiber_intersect(f2, 1, h3.point, smp3.wi,
                                0.001, 1e10, guard));

    ScriptedStream st;
    st.values = {s1[0], s1[1], s1[2], s2[0], s2[1], s2[2],
                 s3[0], s3[1], s3[2]};
    dvec3 bg{0.8, 0.85, 0.9};
    dvec3 col = trace_fiber(scene, orig, dir, 4, st,
                            [&](const dvec3&) { return bg; });
    TASK_CHECK(st.consumed() == 9);

    dvec3 w{1, 1, 1};
    const FiberSample* steps[3] = {&smp1, &smp2, &smp3};
    for (const FiberSample* s : steps)
        for (int c = 0; c < 3; ++c)
            w[c] *= s->K[c] / s->pdf;
    TASK_CHECK(near3(col, cmul(bg, w), 1e-10));

    // Budget exhausted exactly at the second fiber hit yields zero.
    ScriptedStream st1;
    st1.values = st.values;
    TASK_CHECK(near3(trace_fiber(scene, orig, dir, 1, st1,
                                 [&](const dvec3&) { return bg; }),
                     {0, 0, 0}, 0.0));
    TASK_CHECK(st1.consumed() == 3);
    return 0;
}

int tr_fiber_id_tie() {
    // Exact equal-distance tie between two fibers: identical geometry (the
    // hit point is bit-identical, so the tie rule is exercised without
    // intersecting surface volumes) with distinguishable absorption. The
    // smaller id must win.
    FiberMaterial m0 = make_mat();
    m0.sigma_a = {0.2, 0.9, 1.8};
    FiberMaterial m1 = make_mat();
    m1.sigma_a = {1.8, 0.9, 0.2};
    Fiber f0; f0.A = {0, -1, -5}; f0.B = {0, 1, -5};
    f0.radius = 0.1; f0.material = m0;
    Fiber f1 = f0; f1.material = m1;

    dvec3 orig{0, 0, 0}, dir{0, 0, -1};
    FiberHit h0, h1;
    TASK_CHECK(fiber_intersect(f0, 0, orig, dir, 0.001, 1e10, h0));
    TASK_CHECK(fiber_intersect(f1, 1, orig, dir, 0.001, 1e10, h1));
    TASK_CHECK(h0.t == h1.t);
    TASK_CHECK(near(h0.h, h1.h, 1e-14));

    dvec3 T = {0, 1, 0};
    FiberFrame fr = make_fiber_frame(T, {0, 0, 1});

    // Locate a surface-reflection script whose wi leaves the cylinder
    // outward, so after the event the ray escapes both coincident surfaces.
    double su[3] = {0, 0, 0};
    FiberSample s0;
    bool found = false;
    for (int a = 1; a < 20 && !found; ++a)
        for (int b = 1; b < 20 && !found; ++b)
            for (int k = 1; k < 6 && !found; ++k) { // small u3 -> lobe 0
                FiberSample s = fiber_sample(m0, fr, h0.h, f0.radius,
                                             a / 20.0, b / 20.0, k / 20.0);
                FiberHit guard;
                if (s.ok && s.wi * h0.normal > 0.5 &&
                    !fiber_intersect(f0, 0, h0.point, s.wi, 0.001, 1e10, guard)) {
                    s0 = s;
                    su[0] = a / 20.0; su[1] = b / 20.0; su[2] = k / 20.0;
                    found = true;
                }
            }
    TASK_CHECK(found);
    FiberSample s1 = fiber_sample(m1, fr, h1.h, f1.radius,
                                  su[0], su[1], su[2]);
    TASK_CHECK(s1.ok);
    TASK_CHECK(!near3(s0.K, s1.K, 1e-6));

    ScriptedStream st;
    st.values = {su[0], su[1], su[2]};
    dvec3 bg{1, 1, 1};
    FiberScene scene;
    scene.fibers = {f0, f1};
    dvec3 col = trace_fiber(scene, orig, dir, 1, st,
                            [&](const dvec3&) { return bg; });
    dvec3 expected{ s0.K.x / s0.pdf, s0.K.y / s0.pdf, s0.K.z / s0.pdf };
    TASK_CHECK(near3(col, expected, 1e-12));
    TASK_CHECK(st.consumed() == 3);

    // The same script on a scene containing only fiber 1 gives a visibly
    // different answer, proving id 0 (not id 1) was selected above.
    ScriptedStream st2;
    st2.values = {su[0], su[1], su[2]};
    FiberScene scene1;
    scene1.fibers = {f1};
    dvec3 col1 = trace_fiber(scene1, orig, dir, 1, st2,
                             [&](const dvec3&) { return bg; });
    TASK_CHECK(!near3(col, col1, 1e-6));
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"geometry",   "oblique_section_h",                 geom_oblique_section_h},
    {"geometry",   "tangent_parallel_open_caps",        geom_tangent_parallel_caps},
    {"geometry",   "reverse_endpoints",                 geom_reverse_endpoints},
    {"geometry",   "rigid_rotation_equivalence",        geom_rigid_rotation_equivalence},
    {"geometry",   "input_validation",                  geom_validation},
    {"scattering", "fresnel_and_chord",                 scat_fresnel_and_chord},
    {"scattering", "quadrature_energy_and_pdf",         scat_quadrature_energy_and_pdf},
    {"scattering", "stratified_region_frequency_rgb",   scat_stratified_sampling},
    {"scattering", "absorption_tail_grazing_limit",     scat_absorption_tail_and_grazing},
    {"scattering", "sample_contract_and_poles",         scat_sample_contract},
    {"transport",  "miss_background_and_rejection",     tr_miss_background},
    {"transport",  "sphere_blocks_and_tie_wins",        tr_sphere_block_and_tie},
    {"transport",  "single_bounce_weight_self_skip",    tr_single_bounce_weight_and_skip},
    {"transport",  "two_fibers_reencounter_budget",     tr_two_fibers_and_reencounter},
    {"transport",  "fiber_id_tie_breaks_smaller",       tr_fiber_id_tie},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
