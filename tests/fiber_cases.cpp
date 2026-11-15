#include "task_support/check.h"
#include "fiber.h"
#include "fiber_renderer.h"
#include <cmath>
#include <cstddef>

namespace {
const double PI = 3.14159265358979323846;

dvec3 nrm(dvec3 v) { return v.normalized(); }

bool close(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool close_v(const dvec3 &a, const dvec3 &b, double tol) {
    return close(a.x, b.x, tol) && close(a.y, b.y, tol) && close(a.z, b.z, tol);
}

dvec3 rotate_q(const dvec3 &v, const dvec3 &axis, double ang) {
    const double c = std::cos(ang), s = std::sin(ang);
    const dvec3 k = axis.normalized();
    return v*c + cross(k, v)*s + k*(k*v)*(1.0 - c);
}

Fiber make_fiber(int id) {
    Fiber f;
    f.a = {0.0, 0.0, -2.0};
    f.b = {0.0, 0.0,  2.0};
    f.radius = 0.3;
    f.id = id;
    f.material.eta = 1.5;
    f.material.sigma_a = {0.2, 0.5, 1.2};
    f.material.beta_m = 0.25;
    f.material.beta_n = 0.5;
    return f;
}

// ===========================================================================
// geometry group
// ===========================================================================
int geo_oblique_section_h() {
    Fiber f = make_fiber(7);
    const dvec3 orig{1.0, 0.15, 0.7};
    const dvec3 dir = nrm({-1.0, -0.075, -0.2});
    FiberHit hit;
    TASK_CHECK(intersect_fiber(f, orig, dir, 0.001, 1.0e10, &hit));
    TASK_CHECK(hit.id == 7);

    // Hit lies on the lateral surface; the outward normal is unit length and
    // perpendicular to the tangent.
    const dvec3 rel = hit.point - f.a;
    const double u = rel*hit.frame.T/4.0;
    TASK_CHECK(u >= 0.0 && u <= 1.0);
    const dvec3 radial = hit.point - (f.a + hit.frame.T*(u*4.0));
    TASK_CHECK(close(radial.norm(), f.radius, 1.0e-9));
    TASK_CHECK(close(hit.normal.norm(), 1.0, 1.0e-12));
    TASK_CHECK(std::fabs(hit.normal*hit.frame.T) < 1.0e-10);

    // Independent recomputation of the section coordinate straight from the
    // ray/cylinder equations, without going through intersect_fiber().
    const dvec3 q = (orig - f.a) - hit.frame.T*((orig - f.a)*hit.frame.T);
    const dvec3 v = dir - hit.frame.T*(dir*hit.frame.T);
    const double av = v*v, bv = q*v;
    const double disc = bv*bv - av*(q*q - f.radius*f.radius);
    TASK_CHECK(disc >= 0.0);
    const double t0 = (-bv - std::sqrt(disc))/av;
    const dvec3 n_independent = (q + v*t0)*(1.0/f.radius);
    const double h_independent = n_independent*hit.frame.Y;
    TASK_CHECK(close(hit.h, h_independent, 1.0e-10));
    TASK_CHECK(hit.h > -1.0 && hit.h < 1.0 && hit.h != 0.0);
    TASK_CHECK(close(hit.h, hit.normal*hit.frame.Y, 1.0e-12));

    // Frame handedness: X is the wo projection, Y = cross(T, X), unit and
    // mutually orthogonal.
    TASK_CHECK(close(hit.frame.X.norm(), 1.0, 1e-12));
    TASK_CHECK(close(hit.frame.Y.norm(), 1.0, 1e-12));
    TASK_CHECK(close_v(hit.frame.Y, cross(hit.frame.T, hit.frame.X), 1e-12));
    TASK_CHECK(std::fabs(hit.frame.X*hit.frame.T) < 1e-12);
    return 0;
}

int geo_interval_and_endpoints() {
    Fiber f = make_fiber(1);
    const dvec3 orig{1.0, 0.15, 0.7};
    const dvec3 dir = nrm({-1.0, -0.075, -0.2});
    FiberHit hit;
    TASK_CHECK(intersect_fiber(f, orig, dir, 0.001, 1.0e10, &hit));
    const double t = hit.t;

    FiberHit tmp;
    TASK_CHECK( intersect_fiber(f, orig, dir, t, 1.0e10, &tmp));   // tmin inclusive
    TASK_CHECK(close(tmp.t, t, 1e-12));
    TASK_CHECK(!intersect_fiber(f, orig, dir, 0.001, t, &tmp));    // tmax exclusive
    TASK_CHECK(!intersect_fiber(f, orig, dir, t + 1.0, 1.0e10, &tmp));

    // A ray whose crossing falls outside segment [A, B] must miss.
    Fiber short_f = f;
    short_f.b = {0.0, 0.0, -1.0};                       // ends before the hit z
    TASK_CHECK(short_f.valid());
    TASK_CHECK(!intersect_fiber(short_f, orig, dir, 0.001, 1.0e10, &tmp));
    return 0;
}

int geo_tangent_and_parallel() {
    Fiber f = make_fiber(2);
    // Ray perpendicular to the axis, grazing the cross-section circle: its
    // perpendicular-plane offset equals the radius.
    const dvec3 orig{-2.0, f.radius, 0.0};
    const dvec3 dir = nrm({1.0, 0.0, 0.0});
    FiberHit hit;
    TASK_CHECK(intersect_fiber(f, orig, dir, 0.001, 1.0e10, &hit));  // tangent counts
    TASK_CHECK(close(hit.t, 2.0, 1.0e-9));
    TASK_CHECK(close(std::fabs(hit.h), 1.0, 1.0e-9));

    // A ray parallel to the axis never hits the side surface.
    TASK_CHECK(!intersect_fiber(f, {0.05, 0.05, 0.0}, nrm({0.0, 0.0, 1.0}),
                                0.001, 1.0e10, &hit));
    return 0;
}

int geo_rotation_and_reversal() {
    Fiber f1 = make_fiber(0);
    const dvec3 axis = nrm({0.3, -1.1, 0.7});
    const double ang = 0.93;
    Fiber f2 = f1;
    f2.a = rotate_q(f1.a, axis, ang);
    f2.b = rotate_q(f1.b, axis, ang);
    f2.id = 1;
    Fiber f3 = f1;
    std::swap(f3.a, f3.b);                                 // reversed endpoints
    f3.id = 2;

    const dvec3 o1{1.0, 0.15, 0.7};
    const dvec3 d1 = nrm({-1.0, -0.075, -0.2});
    FiberHit h1, h2, h3;
    TASK_CHECK(intersect_fiber(f1, o1, d1, 0.001, 1e10, &h1));
    TASK_CHECK(intersect_fiber(f2, rotate_q(o1, axis, ang),
                               rotate_q(d1, axis, ang).normalized(), 0.001, 1e10, &h2));
    TASK_CHECK(intersect_fiber(f3, o1, d1, 0.001, 1e10, &h3));
    TASK_CHECK(close(h1.h, h2.h, 1e-12));                 // rotation keeps h
    TASK_CHECK(close(h1.h, -h3.h, 1e-12));                // reversal flips Y, hence h

    const FiberLobes L1 = fiber_attenuation(f1.material, h1.h, f1.radius, h1.frame);
    const FiberLobes L2 = fiber_attenuation(f2.material, h2.h, f2.radius, h2.frame);
    const FiberLobes L3 = fiber_attenuation(f3.material, h3.h, f3.radius, h3.frame);

    double worst = 0.0;
    for (int i = 0; i < 80; ++i) {
        const double th = 0.05 + i*0.012;
        const double ph = -PI + i*0.17;
        const dvec3 w1 = nrm(h1.frame.T*std::cos(th) +
                             (h1.frame.X*std::cos(ph) + h1.frame.Y*std::sin(ph))*std::sin(th));
        const dvec3 w2 = rotate_q(w1, axis, ang).normalized();
        const dvec3 k1 = fiber_kernel(f1.material, L1, h1.frame, w1);
        const dvec3 k2 = fiber_kernel(f2.material, L2, h2.frame, w2);
        const dvec3 k3 = fiber_kernel(f3.material, L3, h3.frame, w1);
        const double p1 = fiber_pdf(f1.material, L1, h1.frame, w1);
        const double p2 = fiber_pdf(f2.material, L2, h2.frame, w2);
        const double p3 = fiber_pdf(f3.material, L3, h3.frame, w1);
        worst = std::max(worst, std::max({
            std::fabs(k1.x-k2.x), std::fabs(k1.y-k2.y), std::fabs(k1.z-k2.z),
            std::fabs(k1.x-k3.x), std::fabs(k1.y-k3.y), std::fabs(k1.z-k3.z),
            std::fabs(p1-p2), std::fabs(p1-p3)}));
    }
    TASK_CHECK(worst < 1.0e-10);                          // same physical direction
    return 0;
}

int geo_validation_rejects_bad_inputs() {
    Fiber f = make_fiber(0);
    TASK_CHECK(f.valid());
    Fiber bad = f; bad.radius = 0.0;        TASK_CHECK(!bad.valid());
    bad = f; bad.b = bad.a;                 TASK_CHECK(!bad.valid());
    bad = f; bad.material.eta = 1.0;        TASK_CHECK(!bad.valid());
    bad = f; bad.material.eta = 3.5;        TASK_CHECK(!bad.valid());
    bad = f; bad.material.sigma_a.z = -1.0; TASK_CHECK(!bad.valid());
    bad = f; bad.material.beta_m = 0.01;    TASK_CHECK(!bad.valid());
    bad = f; bad.material.beta_n = 1.5;     TASK_CHECK(!bad.valid());

    FiberHit tmp;
    const dvec3 nan_origin{std::nan(""), 0.0, 0.0};
    TASK_CHECK(!intersect_fiber(f, nan_origin, nrm({0,0,-1.0}), 0.001, 1e10, &tmp));
    TASK_CHECK(!intersect_fiber(f, {1,0,0}, nan_origin, 0.001, 1e10, &tmp));
    return 0;
}

// ===========================================================================
// scattering group
// ===========================================================================
int sca_fresnel_and_lobe_values() {
    // Normal-incidence Fresnel at eta=1.5: ((1-1.5)/(1+1.5))^2 = 0.04.
    TASK_CHECK(close(fresnel_unpolarized(1.0, 1.5), 0.04, 1e-12));
    TASK_CHECK(close(fresnel_unpolarized(0.0, 1.5), 1.0, 1e-12));   // grazing limit
    TASK_CHECK(fresnel_unpolarized(0.3, 1.5) > fresnel_unpolarized(0.9, 1.5));

    Fiber f = make_fiber(0);
    FiberHit h;
    TASK_CHECK(intersect_fiber(f, {1.0, 0.15, 0.7}, nrm({-1.0, -0.075, -0.2}),
                                0.001, 1e10, &h));
    const FiberLobes L = fiber_attenuation(f.material, h.h, f.radius, h.frame);
    TASK_CHECK(L.chord > 0.0);
    TASK_CHECK(L.gamma_t < std::fabs(L.gamma_o) + 1e-12);  // refraction bends in

    for (int k = 0; k < 3; ++k) {
        const double sa = f.material.sigma_a[k];
        const double C = std::exp(-sa*L.chord);
        TASK_CHECK(close(L.A[0][k], L.F, 1e-14));
        TASK_CHECK(close(L.A[1][k], (1-L.F)*(1-L.F)*C, 1e-14));
        TASK_CHECK(close(L.A[2][k], L.A[1][k]*C*L.F, 1e-14));
        const double q = C*L.F;
        TASK_CHECK(close(L.A[3][k], L.A[2][k]*q/(1-q), 1e-12));
        TASK_CHECK(L.A[1][k] < (1-L.F)*(1-L.F));          // absorption removes energy
    }
    // RGB channels separate: the strongly absorbed blue channel is darkest.
    TASK_CHECK(L.A[1].x > L.A[1].y && L.A[1].y > L.A[1].z);
    TASK_CHECK(L.A[3].x > 0.0 && L.A[3].y > 0.0 && L.A[3].z > 0.0); // tail kept
    return 0;
}

int sca_quadrature_matches_lobe_energy() {
    Fiber f = make_fiber(0);
    FiberHit h;
    TASK_CHECK(intersect_fiber(f, {1.0, 0.15, 0.7}, nrm({-1.0, -0.075, -0.2}),
                                0.001, 1e10, &h));
    const FiberLobes L = fiber_attenuation(f.material, h.h, f.radius, h.frame);

    dvec3 energy{0,0,0};
    for (int p = 0; p < 4; ++p) energy = energy + L.A[p];

    const int NS = 360, NP = 720;
    dvec3 int_k{0,0,0};
    double int_pdf = 0.0;
    for (int i = 0; i < NS; ++i) {
        const double s = -1.0 + (i + 0.5)*2.0/NS;
        for (int j = 0; j < NP; ++j) {
            const double phi = -PI + (j + 0.5)*2.0*PI/NP;
            const double rxy = std::sqrt(std::max(0.0, 1.0 - s*s));
            const dvec3 wi = h.frame.T*s +
                             h.frame.X*(rxy*std::cos(phi)) + h.frame.Y*(rxy*std::sin(phi));
            const dvec3 K = fiber_kernel(f.material, L, h.frame, wi);
            const double pdf = fiber_pdf(f.material, L, h.frame, wi);
            const double dA = (2.0/NS)*(2.0*PI/NP);
            int_k = int_k + K*dA;
            int_pdf += pdf*dA;
        }
    }
    TASK_CHECK(close_v(int_k, energy, 2.0e-3));
    TASK_CHECK(close(int_pdf, 1.0, 1.0e-4));
    // The absorbed four-lobe energy must not be renormalized back to one.
    TASK_CHECK(energy.x < 1.0 && energy.y < 1.0 && energy.z < 1.0);
    TASK_CHECK(energy.x > energy.y && energy.y > energy.z);
    return 0;
}

int sca_grazing_tail_convention() {
    Fiber f = make_fiber(0);
    f.material.sigma_a = {0.0, 0.0, 0.0};                 // no absorption
    FiberHit h;
    // Perpendicular tangent ray -> gamma_o = +/- pi/2 -> F = 1: numerator and
    // denominator of the geometric tail vanish together; A3 is fixed to zero.
    TASK_CHECK(intersect_fiber(f, {-2.0, f.radius, 0.0}, nrm({1.0, 0.0, 0.0}),
                                0.001, 1e10, &h));
    const FiberLobes L = fiber_attenuation(f.material, h.h, f.radius, h.frame);
    TASK_CHECK(close(L.F, 1.0, 1e-9));
    TASK_CHECK(L.A[3].x == 0.0 && L.A[3].y == 0.0 && L.A[3].z == 0.0);
    dvec3 e{0,0,0};
    for (int p = 0; p < 4; ++p) e = e + L.A[p];
    TASK_CHECK(close_v(e, dvec3{1,1,1}, 1e-9));           // energy still conserved

    // With absorption the colored attenuation stays strictly below one and
    // retains a nonzero tail away from grazing.
    Fiber g = make_fiber(0);
    FiberHit hg;
    TASK_CHECK(intersect_fiber(g, {1.0, 0.15, 0.7}, nrm({-1.0, -0.075, -0.2}),
                                0.001, 1e10, &hg));
    const FiberLobes Lg = fiber_attenuation(g.material, hg.h, g.radius, hg.frame);
    TASK_CHECK(Lg.A[3].z > 0.0);
    TASK_CHECK(Lg.A[1].z < Lg.A[1].x && Lg.A[1].z > 0.0);
    return 0;
}

int sca_stratified_samples() {
    Fiber f = make_fiber(0);
    FiberHit h;
    TASK_CHECK(intersect_fiber(f, {1.0, 0.15, 0.7}, nrm({-1.0, -0.075, -0.2}),
                                0.001, 1e10, &h));
    const FiberLobes L = fiber_attenuation(f.material, h.h, f.radius, h.frame);

    double probs[4];
    dvec3 energy{0,0,0};
    {
        double raw[4], total = 0.0;
        for (int p = 0; p < 4; ++p) {
            raw[p] = (L.A[p].x + L.A[p].y + L.A[p].z)/3.0;
            total += raw[p];
        }
        for (int p = 0; p < 4; ++p) { probs[p] = raw[p]/total; energy = energy + L.A[p]; }
    }

    const int N = 12;                                      // fixed stratified grid
    long lobe_count[4] = {0,0,0,0};
    long region[4][8] = {};
    dvec3 sum_weight{0,0,0};
    long total = 0;
    for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j)
    for (int k = 0; k < N; ++k) {
        const double u = (i + 0.5)/N, v = (j + 0.5)/N, w = (k + 0.5)/N;
        const FiberSample fs = fiber_sample(f.material, L, h.frame, u, v, w);
        TASK_CHECK(fs.lobe >= 0 && fs.lobe <= 3);
        TASK_CHECK(fs.pdf > 0.0 && std::isfinite(fs.pdf));
        TASK_CHECK(all_finite(fs.kernel) && all_finite(fs.wi));
        TASK_CHECK(close(fs.wi.norm(), 1.0, 1e-12));
        ++lobe_count[fs.lobe];
        const double s = fs.wi*h.frame.T;
        const double phi = std::atan2(fs.wi*h.frame.Y, fs.wi*h.frame.X);
        const int sb = static_cast<int>((s + 1.0)*2.0);    // 4 longitudinal bands
        const int pb = static_cast<int>((phi + PI)*(8.0/(2.0*PI))); // 8 azimuth sectors
        ++region[std::max(0, std::min(3, sb))][std::max(0, std::min(7, pb))];
        sum_weight = sum_weight + dvec3(fs.kernel.x/fs.pdf,
                                        fs.kernel.y/fs.pdf, fs.kernel.z/fs.pdf);
        ++total;
    }
    TASK_CHECK(total == N*N*N);

    // Lobe selection frequencies follow the RGB-mean attenuation probabilities.
    for (int p = 0; p < 4; ++p) {
        if (probs[p] > 0.02)
            TASK_CHECK(std::fabs(double(lobe_count[p])/total - probs[p]) < 0.04);
    }

    // Reference region masses from a fine quadrature of the mixture pdf.
    const int QS = 200, QP = 400;
    double region_pdf[4][8] = {};
    for (int i = 0; i < QS; ++i) {
        const double s = -1.0 + (i + 0.5)*2.0/QS;
        for (int j = 0; j < QP; ++j) {
            const double phi = -PI + (j + 0.5)*2.0*PI/QP;
            const double rxy = std::sqrt(std::max(0.0, 1.0 - s*s));
            const dvec3 wi = h.frame.T*s +
                             h.frame.X*(rxy*std::cos(phi)) + h.frame.Y*(rxy*std::sin(phi));
            const double pdf = fiber_pdf(f.material, L, h.frame, wi);
            const int sb = static_cast<int>((s + 1.0)*2.0);
            const int pb = static_cast<int>((phi + PI)*(8.0/(2.0*PI)));
            region_pdf[std::max(0, std::min(3, sb))][std::max(0, std::min(7, pb))]
                += pdf*(2.0/QS)*(2.0*PI/QP);
        }
    }
    for (int a = 0; a < 4; ++a) for (int b = 0; b < 8; ++b) {
        const double empirical = double(region[a][b])/total;
        TASK_CHECK(std::fabs(empirical - region_pdf[a][b]) < 0.03);
    }

    // Monte Carlo mean of K/pdf equals the integrated kernel, i.e. sum_p A_p.
    const dvec3 mean_weight = sum_weight*(1.0/total);
    TASK_CHECK(close_v(mean_weight, energy, 0.05));

    // Closed endpoint conventions stay finite; sampling never retries.
    for (double v : {0.0, 1.0}) for (double w : {0.0, 1.0}) {
        const FiberSample fs = fiber_sample(f.material, L, h.frame, 0.5, v, w);
        TASK_CHECK(std::isfinite(fs.pdf) && fs.pdf > 0.0 && all_finite(fs.wi));
    }
    return 0;
}

// ===========================================================================
// transport group
// ===========================================================================
const dvec3 BG_COLOR{0.2, 0.7, 0.8};
dvec3 background_fn(const dvec3 &) { return BG_COLOR; }

int tr_escape_occlusion_and_budget() {
    Fiber g = make_fiber(10);
    g.a = {-1.5, 0.0, -1.0};
    g.b = { 1.5, 0.0, -1.0};
    g.radius = 0.25;
    Sphere sp;
    sp.center = {0.0f, -2.0f, -1.0f};
    sp.radius = 1.0f;
    FiberScene scene;
    scene.fibers = &g; scene.fiber_count = 1;
    scene.spheres = &sp; scene.sphere_count = 1;

    SampleStream miss_stream(1);
    const dvec3 escaped = trace_fiber_ray(scene, {0,0,0}, nrm({1,0,0}), 3,
                                          miss_stream, background_fn);
    TASK_CHECK(close_v(escaped, BG_COLOR, 1e-12));           // background unchanged
    TASK_CHECK(miss_stream.consumed() == 0);                 // no event, no samples

    SampleStream blocked_stream(1);
    const dvec3 blocked = trace_fiber_ray(scene, {0,0,0}, nrm({0,-1,-0.4}), 3,
                                          blocked_stream, background_fn);
    TASK_CHECK(close_v(blocked, dvec3{0,0,0}, 1e-12));      // black sphere first
    TASK_CHECK(blocked_stream.consumed() == 0);

    SampleStream zero_stream(1);
    const dvec3 killed = trace_fiber_ray(scene, {0,0,0}, nrm({0,0,-1}), 0,
                                         zero_stream, background_fn);
    TASK_CHECK(close_v(killed, dvec3{0,0,0}, 1e-12));       // budget exhausted
    TASK_CHECK(zero_stream.consumed() == 0);                // killed event draws nothing
    return 0;
}

int tr_single_event_weight_and_consumption() {
    Fiber g = make_fiber(10);
    g.a = {-1.5, 0.0, -1.0};
    g.b = { 1.5, 0.0, -1.0};
    g.radius = 0.25;
    FiberScene scene;
    scene.fibers = &g; scene.fiber_count = 1;

    const dvec3 origin{0,0,0}, ray = nrm({0,0,-1});
    SampleStream stream(999);
    const dvec3 color = trace_fiber_ray(scene, origin, ray, 3, stream, background_fn);
    TASK_CHECK(stream.consumed() == 3);                     // exactly one material event

    // Independently replay the same event and expect K/pdf * background.
    FiberHit hit;
    TASK_CHECK(intersect_fiber(g, origin, ray, 0.001, 1e10, &hit));
    const FiberLobes L = fiber_attenuation(g.material, hit.h, g.radius, hit.frame);
    SampleStream replay(999);
    const FiberSample fs = fiber_sample(g.material, L, hit.frame,
                                        replay.draw(), replay.draw(), replay.draw());
    TASK_CHECK(replay.consumed() == 3);
    const dvec3 expected = cmul(dvec3(fs.kernel.x/fs.pdf, fs.kernel.y/fs.pdf,
                                      fs.kernel.z/fs.pdf), BG_COLOR);
    TASK_CHECK(close_v(color, expected, 1e-10));
    TASK_CHECK(all_finite(color) && color.x > 0.0);
    return 0;
}

int tr_two_fibers_double_scatter() {
    Fiber g1 = make_fiber(10);
    g1.a = {-1.5, 0.0, -1.0};
    g1.b = { 1.5, 0.0, -1.0};
    g1.radius = 0.25;

    const dvec3 origin{0,0,0}, ray = nrm({0,0,-1});
    FiberHit first;
    TASK_CHECK(intersect_fiber(g1, origin, ray, 0.001, 1e10, &first));
    const FiberLobes L1 = fiber_attenuation(g1.material, first.h, g1.radius, first.frame);

    // Use the actual first sampled continuation to place a second real fiber
    // across that ray, keeping the whole double scatter deterministic.
    SampleStream probe(7);
    const FiberSample s1 = fiber_sample(g1.material, L1, first.frame,
                                        probe.draw(), probe.draw(), probe.draw());
    const dvec3 c2 = first.point + s1.wi*1.5;
    const dvec3 ref0{0.13, 0.77, 0.31}, ref1{1.0, 0.0, 0.0};
    dvec3 axis2 = cross(s1.wi, std::fabs(s1.wi*ref0) > 0.9 ? ref1 : ref0);
    axis2 = axis2.normalized();                            // perpendicular to wi
    Fiber g2 = make_fiber(11);
    g2.a = c2 - axis2*1.0;
    g2.b = c2 + axis2*1.0;
    g2.radius = 0.3;
    TASK_CHECK(g2.valid());

    Fiber fibers[2] = {g1, g2};
    // A black sphere participates in the same scene; it is placed off the
    // constructed two-fiber path but blocks a separate ray directly.
    Sphere sp;
    sp.center = {2.0f, 0.0f, -2.0f};
    sp.radius = 0.5f;
    FiberScene scene;
    scene.fibers = fibers; scene.fiber_count = 2;
    scene.spheres = &sp; scene.sphere_count = 1;

    // Two events max: after the second scattering event the budget is zero, so
    // any further fiber hit deterministically returns zero without drawing,
    // while an escape still returns the background.
    SampleStream stream(7);
    const dvec3 color = trace_fiber_ray(scene, origin, ray, 2, stream, background_fn);
    TASK_CHECK(stream.consumed() == 6);                     // two events, 3 scalars each

    // Manual lockstep replay of both events.
    SampleStream replay(7);
    const FiberSample r1 = fiber_sample(g1.material, L1, first.frame,
                                        replay.draw(), replay.draw(), replay.draw());
    FiberHit second;
    TASK_CHECK(intersect_fiber(g2, first.point, r1.wi, 0.001, 1e10, &second));
    TASK_CHECK(second.id == 11);
    const FiberLobes L2 = fiber_attenuation(g2.material, second.h, g2.radius, second.frame);
    const FiberSample r2 = fiber_sample(g2.material, L2, second.frame,
                                        replay.draw(), replay.draw(), replay.draw());
    TASK_CHECK(replay.consumed() == 6);
    dvec3 weight{1,1,1};
    for (const FiberSample *s : {&r1, &r2})
        weight = cmul(weight, dvec3(s->kernel.x/s->pdf, s->kernel.y/s->pdf,
                                    s->kernel.z/s->pdf));

    // Determine what the third segment does with the exhausted budget.
    FiberHit third;
    const bool hits_again = intersect_fiber(g1, second.point, r2.wi, 0.001, 1e10, &third);
    if (hits_again) {
        TASK_CHECK(close_v(color, dvec3{0,0,0}, 1e-12));   // budget zero -> path dies
    } else {
        TASK_CHECK(close_v(color, cmul(weight, BG_COLOR), 1e-9)); // escaped
    }

    // On the segment leaving fiber 10, exactly that fiber is skipped; after
    // scattering off fiber 11 it participates again.
    const FiberHit skipped = intersect_fibers(fibers, 2, first.point, r1.wi,
                                              0.001, 1e10, 10);
    TASK_CHECK(skipped.id != 10);

    // The same scene type also supports a pure black occluder: a sphere-only
    // scene blocks the ray and returns zero without drawing any samples.
    FiberScene sphere_scene;
    sphere_scene.spheres = &sp; sphere_scene.sphere_count = 1;
    SampleStream sphere_stream(11);
    const dvec3 blocked = trace_fiber_ray(sphere_scene, {0,0,0},
                                          nrm({2.0, 0.0, -2.0}), 2,
                                          sphere_stream, background_fn);
    TASK_CHECK(close_v(blocked, dvec3{0,0,0}, 1e-12));
    TASK_CHECK(sphere_stream.consumed() == 0);
    return 0;
}

int tr_tie_breaks_and_skips() {
    // Exact equal distance: the sphere surface and the tangent fiber side both
    // pass through (0, 0, -4), i.e. t = 4 for the ray along -z.
    Fiber g = make_fiber(10);
    g.a = {-1.5, 0.25, -4.0};
    g.b = { 1.5, 0.25, -4.0};
    g.radius = 0.25;
    Sphere sp;
    sp.center = {0.0f, 0.0f, -5.0f};
    sp.radius = 1.0f;
    FiberScene scene;
    scene.fibers = &g; scene.fiber_count = 1;
    scene.spheres = &sp; scene.sphere_count = 1;

    SampleStream s_with(3), s_without(3);
    const dvec3 blocked = trace_fiber_ray(scene, {0,0,0}, nrm({0,0,-1}), 3,
                                          s_with, background_fn);
    TASK_CHECK(close_v(blocked, dvec3{0,0,0}, 1e-12));      // sphere wins the tie
    TASK_CHECK(s_with.consumed() == 0);

    scene.spheres = nullptr; scene.sphere_count = 0;
    const dvec3 lit = trace_fiber_ray(scene, {0,0,0}, nrm({0,0,-1}), 3,
                                      s_without, background_fn);
    TASK_CHECK(s_without.consumed() == 3);                  // fiber alone is hit
    TASK_CHECK(!close_v(lit, dvec3{0,0,0}, 1e-9));

    // Exact equal fiber distance resolves to the smaller stable id.
    Fiber a = make_fiber(22), b = make_fiber(11);
    FiberHit ha, hb;
    TASK_CHECK(intersect_fiber(a, {0,0,0}, nrm({1,0,0}), 0.001, 1e10, &ha));
    TASK_CHECK(intersect_fiber(b, {0,0,0}, nrm({1,0,0}), 0.001, 1e10, &hb));
    TASK_CHECK(close(ha.t, hb.t, 1e-12));
    Fiber both[2] = {a, b};
    const FiberHit winner = intersect_fibers(both, 2, {0,0,0}, nrm({1,0,0}),
                                             0.001, 1e10, -1);
    TASK_CHECK(winner.id == 11);

    // skip_id removes exactly the named fiber from the next segment.
    const FiberHit skipped = intersect_fibers(both, 2, {0,0,0}, nrm({1,0,0}),
                                              0.001, 1e10, 11);
    TASK_CHECK(skipped.id == 22);
    return 0;
}
} // namespace

const task_case task_cases[] = {
    {"geometry",   "oblique_section_h",            geo_oblique_section_h},
    {"geometry",   "interval_and_endpoints",       geo_interval_and_endpoints},
    {"geometry",   "tangent_and_parallel",         geo_tangent_and_parallel},
    {"geometry",   "rotation_and_reversal",        geo_rotation_and_reversal},
    {"geometry",   "validation_rejects_bad_inputs",geo_validation_rejects_bad_inputs},
    {"scattering", "fresnel_and_lobe_values",      sca_fresnel_and_lobe_values},
    {"scattering", "quadrature_matches_lobe_energy", sca_quadrature_matches_lobe_energy},
    {"scattering", "grazing_tail_convention",      sca_grazing_tail_convention},
    {"scattering", "stratified_samples",           sca_stratified_samples},
    {"transport",  "escape_occlusion_and_budget",  tr_escape_occlusion_and_budget},
    {"transport",  "single_event_weight_and_consumption", tr_single_event_weight_and_consumption},
    {"transport",  "two_fibers_double_scatter",    tr_two_fibers_double_scatter},
    {"transport",  "tie_breaks_and_skips",         tr_tie_breaks_and_skips},
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
