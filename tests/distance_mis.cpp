// Distance MIS tests: segment merging, proposal densities and full transport.
// Registered through task_support/check.h; the C driver supplies main().
#include "renderer.h"

#include "task_support/check.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <vector>

using namespace volr;

namespace {

constexpr double kEps = 1e-9;

bool close(double a, double b, double tol = 1e-9) {
    return std::abs(a - b) <= tol * std::max(1.0, std::max(std::abs(a), std::abs(b)));
}

bool close_rgb(const RGB& a, const RGB& b, double tol = 1e-9) {
    return close(a.x, b.x, tol) && close(a.y, b.y, tol) && close(a.z, b.z, tol);
}

// Independent high-accuracy 1D quadrature: adaptive Simpson on [a, b].
template <class F>
double simpson_rec(const F& f, double a, double b, double eps,
                   double whole, double fa, double fm, double fb, int depth) {
    double m = 0.5 * (a + b);
    double lm = 0.5 * (a + m), rm = 0.5 * (m + b);
    double flm = f(lm), frm = f(rm);
    double left = (m - a) * (fa + 4 * flm + fm) / 6.0;
    double right = (b - m) * (fm + 4 * frm + fb) / 6.0;
    double refined = left + right;
    if (depth <= 0 || std::abs(refined - whole) <= 15 * eps)
        return refined + (refined - whole) / 15.0;
    return simpson_rec(f, a, m, eps / 2, left, fa, flm, fm, depth - 1)
         + simpson_rec(f, m, b, eps / 2, right, fm, frm, fb, depth - 1);
}

template <class F>
double integrate(const F& f, double a, double b, double eps = 1e-10) {
    if (!(b > a)) return 0.0;
    double m = 0.5 * (a + b);
    double whole = (b - a) * (f(a) + 4 * f(m) + f(b)) / 6.0;
    return simpson_rec(f, a, b, eps, whole, f(a), f(m), f(b), 24);
}

template <class F>
double integrate_table(const volsamp::SegmentTable& table, const F& f, double eps = 1e-10) {
    double total = 0.0;
    for (const volsamp::Interval& s : table.segs)
        total += integrate(f, s.t0, s.t1, eps);
    return total;
}

VolumeParams make_volume(std::vector<volsamp::Box> boxes) {
    VolumeParams vp;
    vp.boxes = std::move(boxes);
    return vp;
}

volsamp::Box box3(double x0, double y0, double z0, double x1, double y1, double z1,
                  RGB sa, RGB ss) {
    return volsamp::Box{{x0, y0, z0}, {x1, y1, z1}, sa, ss};
}

std::vector<volsamp::LightInfo> single_light(const Vec3& pos = {10, 3, 0},
                                             RGB I = RGB{2, 2, 2}) {
    std::vector<volsamp::LightInfo> infos;
    volsamp::LightInfo info = volsamp::make_light_info(pos, I);
    info.selection_pdf = 1.0;
    infos.push_back(info);
    return infos;
}

// Independent per-channel optical depth computed straight from the box list,
// walking every piecewise-constant stretch of world length.
RGB reference_tau(const VolumeParams& vp, const Vec3& orig, const Vec3& dir, double t) {
    RGB tau{0, 0, 0};
    const int n = 20000;
    double prev = 0.0;
    for (int i = 1; i <= n; ++i) {
        double ti = t * i / n;
        double mid = 0.5 * (prev + ti);
        Vec3 p = orig + dir * mid;
        RGB st{0, 0, 0};
        for (const Box& b : vp.boxes) {
            bool inside = p.x >= b.min_b.x && p.x <= b.max_b.x
                       && p.y >= b.min_b.y && p.y <= b.max_b.y
                       && p.z >= b.min_b.z && p.z <= b.max_b.z;
            if (inside) st += b.sigma_a + b.sigma_s;
        }
        tau += st * (ti - prev);
        prev = ti;
    }
    return tau;
}

// ---------- segments group ----------

int case_overlap_merge() {
    VolumeParams vp = make_volume({
        box3(0, -1, -1, 4, 1, 1, {0.1, 0.2, 0.3}, {0.4, 0.5, 0.6}),
        box3(3, -1, -1, 7, 1, 1, {0.3, 0.1, 0.0}, {0.2, 0.4, 0.7})
    });
    Vec3 orig{0, 0, 0}, dir{1, 0, 0};
    auto table = volsamp::build_segments(vp, orig, dir, 10.0);
    TASK_CHECK(table.segs.size() == 3);
    TASK_CHECK(close(table.segs[0].t0, 0.0) && close(table.segs[0].t1, 3.0));
    TASK_CHECK(close(table.segs[1].t0, 3.0) && close(table.segs[1].t1, 4.0));
    TASK_CHECK(close(table.segs[2].t0, 4.0) && close(table.segs[2].t1, 7.0));
    TASK_CHECK(close(table.occupied_length, 7.0));
    // coefficients are summed in the overlap region
    TASK_CHECK(close_rgb(table.segs[1].sigma_a, RGB{0.4, 0.3, 0.3}));
    TASK_CHECK(close_rgb(table.segs[1].sigma_s, RGB{0.6, 0.9, 1.3}));
    // cumulative optical depth matches an independent world-length walk
    for (double t : {0.5, 3.0, 3.5, 4.0, 6.9, 7.0}) {
        RGB got = table.tau_at(t);
        RGB ref = reference_tau(vp, orig, dir, t);
        TASK_CHECK(close_rgb(got, ref, 2e-3));
        RGB T = table.transmittance(t);
        TASK_CHECK(close_rgb(T, RGB{std::exp(-ref.x), std::exp(-ref.y), std::exp(-ref.z)}, 2e-3));
    }
    TASK_CHECK(close_rgb(table.total_tau, reference_tau(vp, orig, dir, 10.0), 2e-3));
    return 0;
}

int case_separated_gaps() {
    VolumeParams vp = make_volume({
        box3(2, -1, -1, 4, 1, 1, {0, 0, 0}, {0.5, 0.5, 0.5}),
        box3(7, -1, -1, 9, 1, 1, {0, 0, 0}, {0.5, 0.5, 0.5})
    });
    auto table = volsamp::build_segments(vp, {0, 0, 0}, {1, 0, 0}, 12.0);
    TASK_CHECK(table.segs.size() == 2);
    TASK_CHECK(close(table.occupied_length, 4.0)); // vacuum gap [4,7] excluded
    RGB tau_gap_left = table.tau_at(4.5);
    RGB tau_gap_right = table.tau_at(6.5);
    TASK_CHECK(close_rgb(tau_gap_left, tau_gap_right)); // gap freezes optical depth
    RGB t_left = table.transmittance(4.5);
    RGB t_right = table.transmittance(6.5);
    TASK_CHECK(close_rgb(t_left, t_right)); // no fresh exponential start in the gap
    TASK_CHECK(table.sigma_s_at(5.5).x == 0.0);
    TASK_CHECK(close(table.sigma_s_at(3.0).x, 0.5));
    TASK_CHECK(close(table.t_end, 12.0));
    double far = volsamp::last_medium_exit(vp, {0, 0, 0}, {1, 0, 0});
    TASK_CHECK(close(far, 9.0));
    return 0;
}

int case_transparent_channel() {
    VolumeParams vp = make_volume({
        box3(1, -1, -1, 5, 1, 1, {0.0, 0.2, 0.4}, {0.0, 0.3, 0.6}) // R channel fully transparent
    });
    auto table = volsamp::build_segments(vp, {0, 0, 0}, {1, 0, 0}, 8.0);
    TASK_CHECK(table.segs.size() == 1);
    TASK_CHECK(table.total_tau.x == 0.0);
    TASK_CHECK(table.total_tau.y > 0.0 && table.total_tau.z > 0.0);
    TASK_CHECK(close(table.occupied_length, 4.0));
    for (double t : {1.5, 3.0, 4.9})
        TASK_CHECK(close(table.transmittance(t).x, 1.0));
    // samples stay inside the support even when the transparent channel is drawn
    volsamp::RNG rng(12345);
    for (int i = 0; i < 500; ++i) {
        double t = volsamp::sample_optical(table, rng);
        TASK_CHECK(t >= table.segs.front().t0 && t <= table.segs.back().t1);
    }
    return 0;
}

int case_extreme_optical_depth() {
    // tiny and huge optical depths must both stay finite and normalized
    VolumeParams thin = make_volume({
        box3(0, -1, -1, 1, 1, 1, {0, 0, 0}, {1e-12, 1e-12, 1e-12})});
    auto t_thin = volsamp::build_segments(thin, {0, 0, 0}, {1, 0, 0}, 1.0);
    TASK_CHECK(close(t_thin.total_tau.x, 1e-12, 1e-12));
    TASK_CHECK(std::isfinite(t_thin.transmittance(0.5).x));

    VolumeParams thick = make_volume({
        box3(0, -1, -1, 1, 1, 1, {0, 0, 0}, {800.0, 800.0, 800.0})});
    auto t_thick = volsamp::build_segments(thick, {0, 0, 0}, {1, 0, 0}, 1.0);
    TASK_CHECK(t_thick.transmittance(1.0).x == 0.0); // underflow, not NaN
    volsamp::RNG rng(777);
    for (int i = 0; i < 100; ++i) {
        double t = volsamp::sample_optical(t_thick, rng);
        TASK_CHECK(std::isfinite(t) && t >= 0.0 && t <= 1.0);
    }
    return 0;
}

// ---------- density group ----------

int case_optical_normalization() {
    struct Cfg { VolumeParams vp; double end; };
    Cfg cfgs[] = {
        {make_volume({box3(1, -1, -1, 5, 1, 1, {0.1, 0.2, 0.3}, {0.4, 0.5, 0.6}),
                      box3(3, -1, -1, 8, 1, 1, {0.3, 0.1, 0.0}, {0.2, 0.4, 0.7})}), 10.0},
        {make_volume({box3(2, -1, -1, 4, 1, 1, {0, 0, 0}, {0.5, 0.5, 0.5}),
                      box3(7, -1, -1, 9, 1, 1, {0, 0, 0}, {0.5, 0.5, 0.5})}), 12.0},
        {make_volume({box3(1, -1, -1, 5, 1, 1, {0.0, 0.2, 0.4}, {0.0, 0.3, 0.6})}), 8.0}
    };
    Vec3 orig{0, 0, 0}, dir{1, 0, 0};
    for (const Cfg& cfg : cfgs) {
        auto table = volsamp::build_segments(cfg.vp, orig, dir, cfg.end);
        auto infos = single_light();
        volsamp::SamplerContext ctx{orig, dir, table, infos, volsamp::SampleMode::Optical};
        double mass = integrate([&](double t) { return volsamp::optical_density(ctx, t); },
                                0.0, cfg.end);
        TASK_CHECK(close(mass, 1.0, 1e-7));
    }
    return 0;
}

int case_equiangular_normalization() {
    VolumeParams vp = make_volume({
        box3(2, -1, -1, 4, 1, 1, {0, 0, 0}, {0.5, 0.5, 0.5}),
        box3(7, -1, -1, 9, 1, 1, {0, 0, 0}, {0.5, 0.5, 0.5})});
    Vec3 orig{0, 0, 0}, dir{1, 0, 0};
    auto table = volsamp::build_segments(vp, orig, dir, 12.0);
    auto infos = single_light({10, 1.5, 0});
    volsamp::SamplerContext ctx{orig, dir, table, infos, volsamp::SampleMode::Mixed};
    double mass = integrate([&](double t) { return volsamp::equiangular_density(ctx, 0, t); },
                            0.0, 12.0);
    TASK_CHECK(close(mass, 1.0, 1e-7));
    // collinear light: falls back to uniform over occupied length
    auto in_line = single_light({-5, 0, 0});
    volsamp::SamplerContext ctx2{orig, dir, table, in_line, volsamp::SampleMode::Mixed};
    double mass2 = integrate([&](double t) { return volsamp::equiangular_density(ctx2, 0, t); },
                             0.0, 12.0);
    TASK_CHECK(close(mass2, 1.0, 1e-7));
    volsamp::RNG rng(42);
    for (int i = 0; i < 1000; ++i) {
        double t = volsamp::sample_equiangular(table, orig, dir, {-5, 0, 0}, rng);
        TASK_CHECK(table.interval_at(t) != nullptr);
    }
    return 0;
}

int case_full_joint_mixture() {
    VolumeParams vp = make_volume({
        box3(1, -1, -1, 6, 1, 1, {0.7, 0.2, 0.0}, {0.3, 0.9, 0.1})});
    Vec3 orig{0, 0, 0}, dir{1, 0, 0};
    auto table = volsamp::build_segments(vp, orig, dir, 8.0);
    std::vector<volsamp::LightInfo> infos;
    {
        volsamp::LightInfo a = volsamp::make_light_info({8, 2, 0}, {3, 1, 2});
        volsamp::LightInfo b = volsamp::make_light_info({2, -3, 1}, {1, 4, 1});
        double tot = a.luminance + b.luminance;
        a.selection_pdf = a.luminance / tot;
        b.selection_pdf = b.luminance / tot;
        infos = {a, b};
    }
    volsamp::SamplerContext ctx{orig, dir, table, infos, volsamp::SampleMode::Mixed};
    // joint density integrates to each light selection probability
    for (int l = 0; l < 2; ++l) {
        double mass = integrate([&](double t) { return volsamp::joint_density(ctx, l, t); },
                                0.0, 8.0);
        TASK_CHECK(close(mass, infos[l].selection_pdf, 1e-7));
    }
    // the optical proposal is the FULL three-channel mixture, not the channel drawn
    for (double t : {1.5, 3.0, 5.9}) {
        double qd = volsamp::optical_density(ctx, t);
        double manual = 0.0;
        const volsamp::Interval* s = table.interval_at(t);
        for (int c = 0; c < 3; ++c) {
            double tau_tot = table.total_tau[c];
            if (tau_tot > 0.0)
                manual += s->sigma_t()[c] * std::exp(-table.tau_at(t)[c]) / (-std::expm1(-tau_tot));
            else
                manual += 1.0 / table.occupied_length;
        }
        manual /= 3.0;
        TASK_CHECK(close(qd, manual, 1e-12));
        // mixed joint = p_l * (q_d + q_e) / 2 with both strategies present
        double qe = volsamp::equiangular_density(ctx, 1, t);
        double expect = infos[1].selection_pdf * 0.5 * (qd + qe);
        TASK_CHECK(close(volsamp::joint_density(ctx, 1, t), expect, 1e-12));
    }
    // inverse query on actual draws reports the same full-mixture value
    volsamp::RNG rng(99);
    for (int i = 0; i < 1000; ++i) {
        volsamp::SampleResult sr = volsamp::sample_distance(orig, dir, ctx, rng);
        TASK_CHECK(sr.light >= 0 && sr.light < 2);
        TASK_CHECK(table.interval_at(sr.distance) != nullptr);
        TASK_CHECK(close(sr.joint_density,
                         volsamp::joint_density(ctx, sr.light, sr.distance), 1e-12));
        TASK_CHECK(sr.joint_density > 0.0);
    }
    return 0;
}

int case_sample_frequencies() {
    VolumeParams vp = make_volume({
        box3(1, -1, -1, 6, 1, 1, {0.3, 0.1, 0.5}, {0.6, 0.2, 0.4})});
    Vec3 orig{0, 0, 0}, dir{1, 0, 0};
    auto table = volsamp::build_segments(vp, orig, dir, 8.0);
    auto infos = single_light();
    volsamp::SamplerContext ctx{orig, dir, table, infos, volsamp::SampleMode::Optical};
    const int N = 120000;
    volsamp::RNG rng(2024);
    for (double q : {2.0, 3.5, 5.5}) {
        double ref_cdf = integrate([&](double t) { return volsamp::optical_density(ctx, t); },
                                   0.0, q, 1e-11);
        int below = 0;
        volsamp::RNG local(2024);
        for (int i = 0; i < N; ++i)
            if (volsamp::sample_optical(table, local) <= q) ++below;
        double emp = static_cast<double>(below) / N;
        TASK_CHECK(std::abs(emp - ref_cdf) < 0.01);
    }
    // light selection frequencies follow luminance weights
    std::vector<volsamp::LightInfo> two;
    {
        volsamp::LightInfo a = volsamp::make_light_info({0, 5, 0}, {3, 3, 3});
        volsamp::LightInfo b = volsamp::make_light_info({0, -5, 0}, {1, 1, 1});
        a.selection_pdf = 0.75;
        b.selection_pdf = 0.25;
        two = {a, b};
    }
    volsamp::SamplerContext ctx2{orig, dir, table, two, volsamp::SampleMode::Mixed};
    volsamp::RNG r2(7);
    int picked0 = 0;
    for (int i = 0; i < N; ++i)
        if (volsamp::sample_distance(orig, dir, ctx2, r2).light == 0) ++picked0;
    TASK_CHECK(std::abs(static_cast<double>(picked0) / N - 0.75) < 0.02);
    return 0;
}

int case_inactive_sources() {
    VolumeParams vp = make_volume({
        box3(1, -1, -1, 6, 1, 1, {0, 0, 0}, {0.5, 0.5, 0.5})});
    Vec3 orig{0, 0, 0}, dir{1, 0, 0};
    auto empty = volsamp::build_segments(vp, orig, dir, 0.5); // ends before the box
    auto infos = single_light();
    volsamp::SamplerContext ctx_empty{orig, dir, empty, infos, volsamp::SampleMode::Mixed};
    TASK_CHECK(!volsamp::source_is_active(ctx_empty));

    auto table = volsamp::build_segments(vp, orig, dir, 8.0);
    auto black = single_light({3, 3, 3}, RGB{0, 0, 0});
    volsamp::SamplerContext ctx_black{orig, dir, table, black, volsamp::SampleMode::Mixed};
    TASK_CHECK(!volsamp::source_is_active(ctx_black));
    return 0;
}

// ---------- transport group ----------

// Independent reference: per-light sum integrated with high-accuracy 1D
// quadrature, using only production geometry/transmission helpers.
RGB reference_volume_integral(const Scene& s, const Vec3& orig, const Vec3& dir,
                              double t_end) {
    VolumeParams vp = to_volume_params(s);
    auto table = volsamp::build_segments(vp, orig, dir, t_end);
    RGB total{0, 0, 0};
    for (const volsamp::Interval& seg : table.segs) {
        for (int c = 0; c < 3; ++c) {
            auto f = [&](double t) -> double {
                Vec3 p = orig + dir * t;
                RGB sig = table.sigma_s_at(t);
                RGB Tv = table.transmittance(t);
                double v = 0.0;
                for (const PointLight& L : s.lights) {
                    bool blocked = false;
                    RGB Tl = shadow_transmittance(s, p, L.pos, &blocked);
                    if (blocked) continue;
                    double r2 = (L.pos - p).norm2();
                    v += Tv[c] * sig[c] * L.intensity[c] * Tl[c] / (4.0 * kPi * r2);
                }
                return v;
            };
            total[c] += integrate(f, seg.t0, seg.t1, 1e-9);
        }
    }
    return total;
}

Scene media_free(const Scene& s) {
    Scene out = s;
    out.media.clear();
    return out;
}

int case_validation_rejects() {
    auto must_throw = [](Scene s) -> int {
        bool threw = false;
        try { validate_scene(s); } catch (const std::exception&) { threw = true; }
        TASK_CHECK(threw);
        return 0;
    };
    Scene base = default_scene();
    {
        Scene s = base;
        s.media.push_back({{0, 0, 0}, {0, 1, 1}, {0.1, 0.1, 0.1}, {0.1, 0.1, 0.1}});
        if (must_throw(s)) return 1; // zero extent
    }
    {
        Scene s = base;
        s.media.push_back({{0, 0, 0}, {1, 1, 1}, {-0.1, 0, 0}, {0, 0, 0}});
        if (must_throw(s)) return 1; // negative coefficient
    }
    {
        Scene s = base;
        s.media.push_back({{0, 0, 0}, {1, 1, 1},
                           {HUGE_VAL, 0, 0}, {0, 0, 0}});
        if (must_throw(s)) return 1; // non-finite coefficient
    }
    {
        Scene s = base;
        s.media.push_back({{-25, 15, -30}, {25, 25, 30}, {0.1, 0.1, 0.1}, {0.1, 0.1, 0.1}});
        if (must_throw(s)) return 1; // the default light at (-20, 20, 20) lies inside this box
    }
    {
        Scene s = base;
        s.media.push_back({{-2, -2, -2}, {2, 2, 2}, {0.1, 0.1, 0.1}, {0.1, 0.1, 0.1}});
        s.lights.push_back({{0, 0, 0}, {1, 1, 1}});
        if (must_throw(s)) return 1; // light inside a box
    }
    {
        Scene s = base;
        s.lights.push_back({{1, 1, 1}, {-1, 0, 0}});
        if (must_throw(s)) return 1; // negative intensity
    }
    return 0;
}

int case_empty_media_unchanged() {
    Scene plain = default_scene();
    RenderOptions opt;
    opt.samples_per_segment = 4;
    VolBudget b;
    const Vec3 dirs[] = {{0, 0, -1}, {-3, 0, -16}, {1.5, -0.5, -18}, {0, 1, 0}};
    for (const Vec3& d : dirs) {
        Vec3 dir = d.normalized();
        RGB a = volume_cast_ray(plain, {0, 0, 0}, dir, opt, &b);
        TASK_CHECK(b.volume_samples == 0 && b.volume_shadow_queries == 0);
        Scene zero = plain;
        zero.media.push_back({{-30, -10, -30}, {30, 10, 0}, {0, 0, 0}, {0, 0, 0}});
        RGB c = volume_cast_ray(zero, {0, 0, 0}, dir, opt, &b);
        TASK_CHECK(b.volume_samples == 0 && b.volume_shadow_queries == 0);
        TASK_CHECK(close_rgb(a, c, 1e-15));
        TASK_CHECK(std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z));
    }
    return 0;
}

int case_truncated_segment() {
    // Dedicated scene: a purely diffuse surface (no reflection/refraction
    // successors) and a medium slab covering only the camera segment, so the
    // sample budget is exactly samples_per_segment and every segment ends at
    // or before the surface.
    Scene s;
    s.checkerboard = false;
    s.background = {0.02, 0.02, 0.03};
    s.lights = {{{-4.0, 5.0, -5.0}, {3, 3, 3}},
                {{4.5, -2.0, -8.0}, {1.5, 2.0, 2.5}}};
    Material diffuse{1.0, {1.4, 0.3, 0.0, 0.0}, {0.4, 0.2, 0.15}, 12.0};
    s.spheres.push_back(Sphere{{0.0, 0.0, -10.0}, 2.0, diffuse});
    s.media.push_back({{-2.0, -2.0, -7.0}, {2.0, 2.0, -3.0},
                       {0.08, 0.08, 0.08}, {0.25, 0.25, 0.25}});
    validate_scene(s);
    Vec3 orig{0, 0, 0}, dir = Vec3{0, 0, -1}.normalized();
    Hit h = intersect(s, orig, dir);
    TASK_CHECK(h.hit);
    VolumeParams vp = to_volume_params(s);
    auto table = volsamp::build_segments(vp, orig, dir, h.t);
    TASK_CHECK(!table.empty());
    for (const auto& seg : table.segs) TASK_CHECK(seg.t1 <= h.t + 1e-9);
    RGB T_end = table.transmittance(h.t);

    RenderOptions opt;
    opt.samples_per_segment = 240;
    opt.seed = 1001;

    // Exact budget check on the production camera-segment estimator itself:
    // no surface successors are involved, so counts are exactly the requested
    // sample count with one finite shadow query per sample.
    {
        VolBudget b0;
        RNG rng0(opt.seed);
        auto infos0 = make_light_infos(s);
        RGB direct = volume_source(s, vp, orig, dir, h.t, infos0, opt, rng0, b0);
        TASK_CHECK(b0.volume_samples == 240);
        TASK_CHECK(b0.volume_shadow_queries == 240);
        TASK_CHECK(close_rgb(direct, reference_volume_integral(s, orig, dir, h.t), 3e-2));
    }

    // Full recursion through the single public entry: successors that cross
    // the slab carry their own source terms; the Monte Carlo result matches
    // the independent quadrature reference.
    VolBudget budget;
    RGB got = volume_cast_ray(s, orig, dir, opt, &budget);
    TASK_CHECK(budget.volume_samples >= 240);
    TASK_CHECK(budget.volume_shadow_queries == budget.volume_samples);

    RGB ref_vol = reference_volume_integral(s, orig, dir, h.t);
    RGB terminal = volume_cast_ray(media_free(s), orig, dir, opt, &budget);
    RGB ref = ref_vol + volsamp::mul(T_end, terminal);
    TASK_CHECK(close_rgb(got, ref, 3e-2));
    return 0;
}

int case_finite_occlusion() {
    Scene s;
    s.checkerboard = false;
    s.background = {0, 0, 0};
    s.lights = {{{9, 2.5, 0}, {6, 6, 6}}};
    // slab on the x axis, blocker sphere centered on a real shadow line
    s.media.push_back({{2, -1, -1}, {12, 1, 1}, {0.05, 0.05, 0.05}, {0.4, 0.4, 0.4}});
    s.spheres.push_back(Sphere{{9.5, 1.25, 0}, 0.3,
                        Material{1.0, {1.4, 0, 0, 0}, {0.2, 0.2, 0.2}, 10.0}});
    validate_scene(s);
    Vec3 orig{0, 0, 0}, dir{1, 0, 0};

    // direct shadow-query semantics
    RGB blocked_T = shadow_transmittance(s, Vec3{10, 0, 0}, s.lights[0].pos);
    TASK_CHECK(close_rgb(blocked_T, RGB{0, 0, 0}));
    bool blocked = false;
    shadow_transmittance(s, {3, 0, 0}, s.lights[0].pos, &blocked);
    TASK_CHECK(!blocked);
    // an object beyond the light must not occlude
    Scene s2 = s;
    s2.spheres.clear();
    Vec3 beyond = s.lights[0].pos + (s.lights[0].pos - Vec3{10, 0, 0}).normalized() * 3.0;
    s2.spheres.push_back(Sphere{beyond, 0.5,
                         Material{1.0, {1.4, 0, 0, 0}, {0.2, 0.2, 0.2}, 10.0}});
    bool blocked2 = true;
    RGB Tl = shadow_transmittance(s2, Vec3{3, 0, 0}, s2.lights[0].pos, &blocked2);
    TASK_CHECK(!blocked2);
    // only the medium remains on that line; compare against the segment table
    {
        Vec3 delta = s2.lights[0].pos - Vec3{3, 0, 0};
        double r = delta.norm();
        auto tab = volsamp::build_segments(to_volume_params(s2), Vec3{3, 0, 0},
                                           delta * (1.0 / r), r);
        TASK_CHECK(close_rgb(Tl, tab.transmittance(r), 1e-12));
    }

    double t_end = volsamp::last_medium_exit(to_volume_params(s), orig, dir);
    RGB ref = reference_volume_integral(s, orig, dir, t_end);
    RenderOptions opt;
    opt.samples_per_segment = 300;
    opt.seed = 555;
    VolBudget budget;
    RGB got = volume_cast_ray(s, orig, dir, opt, &budget);
    TASK_CHECK(close_rgb(got, ref, 4e-2));
    return 0;
}

int case_specular_successors() {
    // Two separate boxes: one on the camera segment, one on the mirror bounce.
    Scene s;
    s.checkerboard = false;
    s.background = {0.05, 0.05, 0.1};
    s.lights = {{{-4, 6, 2}, {4, 4, 4}}, {{6, -3, 4}, {2, 5, 2}}};
    Material mirror{1.0, {0.0, 16.0, 0.8, 0.0}, {1, 1, 1}, 1425.0};
    s.spheres.push_back(Sphere{{0, -1.0, -10}, 2.0, mirror});
    // box on the camera segment, truncated before the sphere (z in [-6,-2])
    s.media.push_back({{-3, -3, -6}, {3, 3, -2},
                       {0.1, 0.05, 0.02}, {0.3, 0.3, 0.3}});
    // box on the upward mirror bounce, disjoint from the camera ray (y >= 1)
    s.media.push_back({{-2, 1, -8}, {2, 8, -3.5},
                       {0.04, 0.08, 0.1}, {0.2, 0.2, 0.2}});
    validate_scene(s);

    Vec3 orig{0, 0, 0}, dir = Vec3{0, 0, -1}.normalized();
    Hit h = intersect(s, orig, dir);
    TASK_CHECK(h.hit);
    Vec3 refl = reflect_dir(dir, h.normal).normalized();
    TASK_CHECK(refl.y > 0.0); // oblique bounce heads upward into the second box

    VolumeParams vp = to_volume_params(s);
    auto tab1 = volsamp::build_segments(vp, orig, dir, h.t);
    RGB T1 = tab1.transmittance(h.t);
    RGB I1 = reference_volume_integral(s, orig, dir, h.t);

    Hit h2 = intersect(s, h.point, refl);
    double t2 = h2.hit ? h2.t : volsamp::last_medium_exit(vp, h.point, refl);
    TASK_CHECK(t2 > 0.0);
    auto tab2 = volsamp::build_segments(vp, h.point, refl, t2);
    TASK_CHECK(!tab2.empty());
    RGB T2 = tab2.transmittance(t2);
    RGB I2 = reference_volume_integral(s, h.point, refl, t2);
    RenderOptions opt;
    opt.samples_per_segment = 160;
    VolBudget budget;
    RGB terminal = volume_cast_ray(media_free(s), h.point, refl, opt, &budget);
    RGB ref = I1 + volsamp::mul(T1, I2 + volsamp::mul(T2, terminal));

    opt.seed = 31337;
    RGB got = volume_cast_ray(s, orig, dir, opt, &budget);
    TASK_CHECK(close_rgb(got, ref, 5e-2));
    // both segments consumed samples and shadow queries through the recursion
    TASK_CHECK(budget.volume_samples >= 320);
    TASK_CHECK(budget.volume_shadow_queries == budget.volume_samples);
    return 0;
}

int case_refraction_successor() {
    Scene s;
    s.checkerboard = false;
    s.background = {0.05, 0.05, 0.1};
    s.lights = {{{-4, 6, 2}, {4, 4, 4}}};
    Material glass{1.5, {0.0, 0.9, 0.1, 0.8}, {0.6, 0.7, 0.8}, 125.0};
    s.spheres.push_back(Sphere{{0, 0, -10}, 2.0, glass});
    // medium only on the camera segment; refracted successor gets T(end) only
    s.media.push_back({{-3, -3, -7}, {3, 3, -3},
                       {0.08, 0.08, 0.08}, {0.25, 0.2, 0.15}});
    validate_scene(s);
    Vec3 orig{0, 0, 0}, dir = Vec3{0.1, 0.05, -1}.normalized();
    Hit h = intersect(s, orig, dir);
    TASK_CHECK(h.hit);
    VolumeParams vp = to_volume_params(s);
    auto tab = volsamp::build_segments(vp, orig, dir, h.t);
    RGB T_end = tab.transmittance(h.t);
    RGB Ivol = reference_volume_integral(s, orig, dir, h.t);
    RenderOptions opt;
    opt.samples_per_segment = 200;
    VolBudget budget;
    RGB terminal = volume_cast_ray(media_free(s), orig, dir, opt, &budget);
    RGB ref = Ivol + volsamp::mul(T_end, terminal);
    opt.seed = 8080;
    RGB got = volume_cast_ray(s, orig, dir, opt, &budget);
    TASK_CHECK(close_rgb(got, ref, 5e-2));
    TASK_CHECK(budget.volume_shadow_queries == budget.volume_samples);
    return 0;
}

// Thin occupied slab, light close to it but outside the box: the equiangular
// mixture should show lower batch variance than the pure optical proposal.
int case_mis_variance() {
    Scene s;
    s.checkerboard = false;
    s.background = {0, 0, 0};
    s.lights = {{{10, 1.5, 0}, {20, 20, 20}}};
    s.media.push_back({{8, -2, -2}, {12, 1, 2},
                       {0.02, 0.02, 0.02}, {0.6, 0.6, 0.6}});
    validate_scene(s);
    Vec3 orig{0, 0, 0}, dir{1, 0, 0};
    double t_end = volsamp::last_medium_exit(to_volume_params(s), orig, dir);
    RGB ref = reference_volume_integral(s, orig, dir, t_end);

    constexpr int batches = 16;
    constexpr int per_batch = 32;
    auto run = [&](volsamp::SampleMode mode, double& mean_out, double& var_out) -> int {
        std::vector<double> lum;
        for (int b = 0; b < batches; ++b) {
            RGB batch{0, 0, 0};
            for (int k = 0; k < per_batch; ++k) {
                RenderOptions opt;
                opt.mode = mode;
                opt.samples_per_segment = 1;
                opt.seed = 0xABCDEF01ull + 1000003ull * b + 97ull * k;
                VolBudget budget;
                RGB one = volume_cast_ray(s, orig, dir, opt, &budget);
                TASK_CHECK(budget.volume_samples == 1);
                TASK_CHECK(budget.volume_shadow_queries == 1);
                batch += one;
            }
            batch = batch * (1.0 / per_batch);
            lum.push_back(0.2126 * batch.x + 0.7152 * batch.y + 0.0722 * batch.z);
        }
        double mean = 0;
        for (double v : lum) mean += v;
        mean /= batches;
        double var = 0;
        for (double v : lum) var += (v - mean) * (v - mean);
        var /= (batches - 1);
        mean_out = mean;
        var_out = var;
        return 0;
    };
    double mean_opt, var_opt, mean_mix, var_mix;
    if (run(volsamp::SampleMode::Optical, mean_opt, var_opt)) return 1;
    if (run(volsamp::SampleMode::Mixed, mean_mix, var_mix)) return 1;
    double ref_lum = 0.2126 * ref.x + 0.7152 * ref.y + 0.0722 * ref.z;
    double err_opt = std::abs(mean_opt - ref_lum);
    double err_mix = std::abs(mean_mix - ref_lum);
    std::cout << "MIS_BATCH mode=optical batches=" << batches
              << " samples_per_batch=" << per_batch
              << " mean=" << mean_opt << " ref=" << ref_lum
              << " error=" << err_opt << " batch_variance=" << var_opt << "\n";
    std::cout << "MIS_BATCH mode=mixed   batches=" << batches
              << " samples_per_batch=" << per_batch
              << " mean=" << mean_mix << " ref=" << ref_lum
              << " error=" << err_mix << " batch_variance=" << var_mix << "\n";
    // both estimators are unbiased against the independent quadrature reference
    TASK_CHECK(err_opt < 4.0 * std::sqrt(var_opt / batches) + 1e-6);
    TASK_CHECK(err_mix < 4.0 * std::sqrt(var_mix / batches) + 1e-6);
    // the equiangular mixture is the lower-variance estimator for this beam
    TASK_CHECK(var_mix < var_opt);
    return 0;
}

int case_nonunit_direction_rejected() {
    Scene s = default_scene();
    s.media.push_back({{-6, -6, -26}, {6, 6, -4}, {0.1, 0.1, 0.1}, {0.2, 0.2, 0.2}});
    validate_scene(s);
    RenderOptions opt;
    bool threw = false;
    try { volume_cast_ray(s, {0, 0, 0}, {0, 0, -2}, opt); }
    catch (const std::exception&) { threw = true; }
    TASK_CHECK(threw);
    bool threw2 = false;
    opt.samples_per_segment = 0;
    try { volume_cast_ray(s, Vec3{0, 0, 0}, Vec3{0, 0, -1}.normalized(), opt); }
    catch (const std::exception&) { threw2 = true; }
    TASK_CHECK(threw2);
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"segments", "overlap_merge",        case_overlap_merge},
    {"segments", "separated_gaps",       case_separated_gaps},
    {"segments", "transparent_channel",  case_transparent_channel},
    {"segments", "extreme_optical_depth", case_extreme_optical_depth},
    {"density",  "optical_normalization", case_optical_normalization},
    {"density",  "equiangular_normalization", case_equiangular_normalization},
    {"density",  "full_joint_mixture",   case_full_joint_mixture},
    {"density",  "sample_frequencies",   case_sample_frequencies},
    {"density",  "inactive_sources",     case_inactive_sources},
    {"transport", "validation_rejects",  case_validation_rejects},
    {"transport", "empty_media_unchanged", case_empty_media_unchanged},
    {"transport", "truncated_segment",   case_truncated_segment},
    {"transport", "finite_occlusion",    case_finite_occlusion},
    {"transport", "specular_successors", case_specular_successors},
    {"transport", "refraction_successor", case_refraction_successor},
    {"transport", "mis_variance",        case_mis_variance},
    {"transport", "nonunit_direction",   case_nonunit_direction_rejected}
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
