#ifndef SUBSURFACE_CASES_CPP
#define SUBSURFACE_CASES_CPP
//==============================================================================================
// Task checks for the volumetric subsurface random walk. Registered as three groups
// (distance, boundary, transport) through task_support/check.h; defines no main().
//
// To the extent possible under law, the author(s) have dedicated all copyright and related and
// neighboring rights to this software to the public domain worldwide. This software is
// distributed without any warranty.
//==============================================================================================

#include "task_support/check.h"

#include "rtweekend.h"
#include "camera.h"
#include "bvh.h"
#include "quad.h"
#include "sphere.h"
#include "hittable_list.h"
#include "material.h"
#include "constant_medium.h"
#include "subsurface.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


namespace {

    // Deterministic splitmix64 source of open-interval (0, 1) samples, so every statistical
    // check is reproducible from a fixed seed and never depends on the global rand() stream.
    struct splitmix64 {
        uint64_t state;
        explicit splitmix64(uint64_t seed) : state(seed) {}

        double next() {
            state += 0x9e3779b97f4a7c15ULL;
            uint64_t z = state;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            z = z ^ (z >> 31);
            const double u = static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
            return 1e-12 + u * (1.0 - 2e-12);  // Strictly inside (0, 1).
        }
    };

    subsurface_sampler make_sampler(splitmix64& rng) {
        return [&rng]() { return rng.next(); };
    }

    bool approx(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

    // An outside ray entering a radius-R sphere centered at `center` head-on, so the interior
    // chord length is exactly the diameter 2R. The hit record's material is intentionally null:
    // the walk only consumes geometry from the record.
    ray axial_ray(const vec3& center, double R, double time = 0.0, double dir_scale = 1.0) {
        return ray(center + vec3(0, 0, R), vec3(0, 0, -dir_scale), time);
    }

    hit_record axial_entry(const vec3& center, double R) {
        hit_record rec;
        rec.p = center + vec3(0, 0, R);
        rec.normal = vec3(0, 0, 1);
        rec.front_face = true;
        rec.t = 0.0;
        rec.u = rec.v = 0.0;
        rec.mat = nullptr;
        return rec;
    }

    std::shared_ptr<subsurface_object> make_sphere_medium(
            const vec3& center, double R, const vec3& sigma_s, const vec3& sigma_a,
            double eta, std::shared_ptr<material> boundary_mat = nullptr) {
        if (!boundary_mat) boundary_mat = std::make_shared<lambertian>(color(0.8, 0.8, 0.8));
        auto boundary = std::make_shared<sphere>(center, R, boundary_mat);
        return std::make_shared<subsurface_object>(boundary,
                                                   subsurface_params(sigma_s, sigma_a, eta));
    }

    struct ppm_image {
        int width = 0, height = 0;
        std::vector<int> bytes;
    };

    ppm_image render_ppm(camera& cam, const hittable& world, const hittable& lights) {
        std::ostringstream out;
        auto* old = std::cout.rdbuf(out.rdbuf());
        cam.render(world, lights);
        std::cout.rdbuf(old);

        ppm_image image;
        std::istringstream in(out.str());
        std::string magic;
        int max_value;
        in >> magic >> image.width >> image.height >> max_value;
        if (magic != "P3" || max_value != 255)
            return image;
        int value;
        while (in >> value)
            image.bytes.push_back(value);
        return image;
    }

    double mean_byte(const ppm_image& image) {
        double sum = 0.0;
        for (int v : image.bytes) sum += v;
        return sum / std::max<size_t>(1, image.bytes.size());
    }

}  // namespace


//==============================================================================================
// Group: distance. Independent formulas for the free-flight collision density and the
// boundary-first survival mass, using pure absorption so the first segment ends the walk.
//==============================================================================================

// Scalar medium: the mixture reduces to one exponential. Along the known chord L = 2R,
// P(collision before boundary) = 1 - exp(-sigma L), P(survival to boundary) = exp(-sigma L).
static int case_collision_density() {
    const int N = 20000;
    const double sigma = 1.5;
    const vec3 zero(0, 0, 0);

    struct point { double R; double expected_absorb; };
    const point points[] = {{1.0, 1.0 - std::exp(-sigma * 2.0)},
                            {0.5, 1.0 - std::exp(-sigma * 1.0)}};

    for (const auto& pt : points) {
        auto medium = make_sphere_medium(vec3(0, 0, 0), pt.R, zero,
                                         vec3(sigma, sigma, sigma), 1.0);
        splitmix64 rng(0xC01151DULL);
        auto sampler = make_sampler(rng);
        int absorbed = 0, exited = 0;
        for (int i = 0; i < N; ++i) {
            auto result = medium->random_walk(axial_ray(vec3(0, 0, 0), pt.R),
                                              axial_entry(vec3(0, 0, 0), pt.R),
                                              50, sampler);
            TASK_CHECK(result.reason == subsurface_termination::absorbed ||
                       result.reason == subsurface_termination::exited);
            if (result.reason == subsurface_termination::absorbed) ++absorbed;
            else ++exited;
        }
        const double measured = double(absorbed) / N;
        const double survival = double(exited) / N;
        const double error = std::fabs(measured - pt.expected_absorb);
        std::printf("distance.collision_density R=%.2f samples=%d measured=%.5f expected=%.5f "
                    "error=%.5f survival=%.5f expected_survival=%.5f\n",
                    pt.R, N, measured, pt.expected_absorb, error, survival,
                    std::exp(-sigma * 2.0 * pt.R));
        // ~3 sigma Monte Carlo tolerance, comfortably looser than the standard error (~0.002).
        TASK_CHECK(error < 0.02);
        TASK_CHECK(approx(survival, 1.0 - pt.expected_absorb, 0.02));
    }
    return 0;
}

// Scalar survival to the boundary must carry unit spectral weight: the ratio-tracking
// denominator equals the (single-channel) transmittance, so they cancel exactly.
static int case_survival_weight() {
    const int N = 20000;
    const double sigma = 1.2;
    const vec3 zero(0, 0, 0);
    auto medium = make_sphere_medium(vec3(0, 0, 0), 1.0, zero,
                                     vec3(sigma, sigma, sigma), 1.0);
    splitmix64 rng(0x5EED1111ULL);
    auto sampler = make_sampler(rng);

    int exited = 0;
    vec3 weight_sum(0, 0, 0);
    for (int i = 0; i < N; ++i) {
        auto result = medium->random_walk(axial_ray(vec3(0, 0, 0), 1.0),
                                          axial_entry(vec3(0, 0, 0), 1.0),
                                          50, sampler);
        if (result.reason == subsurface_termination::exited) {
            ++exited;
            weight_sum += result.weight;
            TASK_CHECK(approx(result.weight[0], 1.0, 1e-12));
            TASK_CHECK(approx(result.weight[1], 1.0, 1e-12));
            TASK_CHECK(approx(result.weight[2], 1.0, 1e-12));
        }
    }
    TASK_CHECK(exited > 0);
    const vec3 mean_weight = weight_sum / exited;
    std::printf("distance.survival_weight samples=%d exited=%d mean_weight=(%.6f,%.6f,%.6f)\n",
                N, exited, mean_weight[0], mean_weight[1], mean_weight[2]);
    TASK_CHECK(approx(mean_weight[0], 1.0, 1e-10));
    TASK_CHECK(approx(mean_weight[2], 1.0, 1e-10));
    return 0;
}


//==============================================================================================
// Group: boundary. Exact unpolarized Fresnel/TIR values plus sphere, rotated/translated box and
// placement/direction/time invariance of the walk.
//==============================================================================================

static int case_fresnel_tir() {
    // Normal incidence reflectance of an eta=1.5 interface: ((eta-1)/(eta+1))^2 = 0.04.
    const double r_normal = subsurface_fresnel(1.0, 1.0 / 1.5);
    TASK_CHECK(approx(r_normal, 0.04, 1e-12));
    // Reciprocity: entering or leaving at normal incidence gives the same value.
    TASK_CHECK(approx(subsurface_fresnel(1.0, 1.5), 0.04, 1e-12));

    // Matching indices never reflect, at any angle.
    TASK_CHECK(approx(subsurface_fresnel(0.3, 1.0), 0.0, 1e-12));
    TASK_CHECK(approx(subsurface_fresnel(0.97, 1.0), 0.0, 1e-12));

    // Total internal reflection for eta=1.5: critical cosine is sqrt(1 - (1/1.5)^2) ~ 0.745.
    const double cos_critical = std::sqrt(1.0 - (1.0 / 1.5) * (1.0 / 1.5));
    TASK_CHECK(approx(cos_critical, 0.74535599, 1e-6));
    TASK_CHECK(approx(subsurface_fresnel(0.5, 1.5), 1.0, 1e-12));   // Past critical: TIR.
    TASK_CHECK(approx(subsurface_fresnel(0.0, 1.5), 1.0, 1e-12));
    const double r_below = subsurface_fresnel(0.9, 1.5);             // Below critical: partial.
    TASK_CHECK(r_below > 0.04 && r_below < 1.0);

    // Reflectance stays within [0, 1] across the full angular range and is monotonic in angle.
    double previous = subsurface_fresnel(1.0, 1.5);
    for (int k = 1; k <= 20; ++k) {
        const double cos_theta = 1.0 - k * 0.045;
        const double r = subsurface_fresnel(cos_theta, 1.5);
        TASK_CHECK(r >= 0.0 && r <= 1.0 && r >= previous - 1e-15);
        previous = r;
    }

    // A sampler that always falls below the normal-incidence Fresnel probability forces an entry
    // reflection: unit weight, one event charged, ray mirrored back outside.
    auto medium = make_sphere_medium(vec3(0, 0, 0), 1.0, vec3(1, 1, 1), vec3(0.1, 0.1, 0.1), 1.5);
    subsurface_sampler always_reflect = []() { return 0.01; };  // Below R = 0.04.
    auto result = medium->random_walk(axial_ray(vec3(0, 0, 0), 1.0),
                                      axial_entry(vec3(0, 0, 0), 1.0),
                                      10, always_reflect);
    TASK_CHECK(result.reason == subsurface_termination::reflected);
    TASK_CHECK(approx(result.weight[0], 1.0, 1e-12));
    TASK_CHECK(approx(result.weight[1], 1.0, 1e-12));
    TASK_CHECK(approx(result.weight[2], 1.0, 1e-12));
    TASK_CHECK(result.events_used == 1);
    TASK_CHECK(dot(result.exit_ray.direction(), vec3(0, 0, 1)) > 0.99);
    return 0;
}

// An eta=1, non-absorbing sphere always transmits and the exit ray leaves the closed boundary
// pointing outward, with no forward re-intersection (it cannot return through the entry).
static int case_sphere_exit() {
    const int N = 5000;
    auto medium = make_sphere_medium(vec3(0, 0, 0), 1.0,
                                     vec3(2.0, 2.0, 2.0), vec3(0, 0, 0), 1.0);
    splitmix64 rng(0x5B4E7EULL);
    auto sampler = make_sampler(rng);
    int exited = 0;
    for (int i = 0; i < N; ++i) {
        auto r = axial_ray(vec3(0, 0, 0), 1.0);
        auto result = medium->random_walk(r, axial_entry(vec3(0, 0, 0), 1.0), 200, sampler);
        TASK_CHECK(result.reason == subsurface_termination::exited);
        TASK_CHECK(result.events_used >= 1 && result.events_used <= 60);
        ++exited;

        // Exit point lies on the sphere and the continuation ray points outward, away from it.
        const double radius = result.exit_ray.origin().length();
        TASK_CHECK(approx(radius, 1.0, 1e-8));
        const vec3 outward = unit_vector(result.exit_ray.origin());
        TASK_CHECK(dot(unit_vector(result.exit_ray.direction()), outward) > 0.0);

        hit_record rehit;
        TASK_CHECK(!medium->boundary().hit(
            result.exit_ray, interval(1e-9, infinity), rehit));
        // The walk preserves the ray time on the exit ray.
        TASK_CHECK(result.exit_ray.time() == r.time());
    }
    TASK_CHECK(exited == N);
    return 0;
}

// The same guarantee for a convex six-quad box after rotate_y/translate: only the wrapped
// boundary is queried inside, and the refracted exit ray never re-enters.
static int case_box_exit() {
    const int N = 5000;
    auto wall = std::make_shared<lambertian>(color(0.7, 0.7, 0.7));
    std::shared_ptr<hittable> placed =
        std::make_shared<translate>(
            std::make_shared<rotate_y>(
                box(point3(-0.8, -0.8, -0.8), point3(0.8, 0.8, 0.8), wall), 35.0),
            vec3(0.4, -0.2, 0.7));
    auto medium = std::make_shared<subsurface_object>(
        placed, subsurface_params(vec3(2.5, 2.0, 1.5), vec3(0, 0, 0), 1.0));

    splitmix64 rng(0xB0C0BBULL);
    auto sampler = make_sampler(rng);
    int exited = 0;
    for (int i = 0; i < N; ++i) {
        // Obtain the real entry record through the object's own hittable interface.
        ray outside(point3(0.4, -0.2, 5.0), vec3(0, 0, -1), 0.37);
        hit_record entry;
        TASK_CHECK(medium->hit(outside, interval(0.001, infinity), entry));
        TASK_CHECK(subsurface_object::from(entry.mat) == medium.get());
        TASK_CHECK(entry.mat != wall);  // The boundary's own material is not exposed.

        auto result = medium->random_walk(outside, entry, 300, sampler);
        TASK_CHECK(result.reason == subsurface_termination::exited);
        TASK_CHECK(result.exit_ray.time() == 0.37);

        hit_record rehit;
        TASK_CHECK(!medium->boundary().hit(
            result.exit_ray, interval(1e-9, infinity), rehit));
        ++exited;
    }
    TASK_CHECK(exited == N);
    return 0;
}

// World-length segment sampling must be invariant to non-unit incoming directions, rigid
// placement of the boundary and (for stationary geometry) ray time.
static int case_walk_invariance() {
    const int N = 3000;
    const vec3 center_a(0, 0, 0);
    const vec3 offset(1.7, -0.6, 2.3);
    auto medium_a = make_sphere_medium(center_a, 1.0, vec3(2.1, 1.6, 1.2),
                                       vec3(0.3, 0.5, 0.8), 1.33);
    auto medium_b = make_sphere_medium(center_a + offset, 1.0, vec3(2.1, 1.6, 1.2),
                                       vec3(0.3, 0.5, 0.8), 1.33);
    int placement_mismatches = 0;
    double max_translation_weight_diff = 0.0;
    for (int i = 0; i < N; ++i) {
        splitmix64 rng_a(0x1A4A1A07ULL + i), rng_b(0x1A4A1A07ULL + i),
                   rng_c(0x1A4A1A07ULL + i);
        auto sa = make_sampler(rng_a), sb = make_sampler(rng_b), sc = make_sampler(rng_c);

        const auto base = medium_a->random_walk(
            axial_ray(center_a, 1.0), axial_entry(center_a, 1.0), 40, sa);

        // Same geometry, incoming direction scaled by three: the walk normalizes on entry, so the
        // computation is bit-for-bit identical.
        const auto scaled = medium_a->random_walk(
            axial_ray(center_a, 1.0, 0.0, 3.0), axial_entry(center_a, 1.0), 40, sb);
        TASK_CHECK(scaled.reason == base.reason);
        TASK_CHECK(scaled.events_used == base.events_used);
        for (int c = 0; c < 3; ++c)
            TASK_CHECK(approx(scaled.weight[c], base.weight[c], 1e-14));

        // Translated boundary with an equally translated entry ray: the physical walk is the same
        // one expressed in shifted coordinates. Only sub-ULP boundary-distance rounding can differ,
        // so weights must agree to floating-point tolerance and full-path tie flips must be rare.
        const auto moved = medium_b->random_walk(
            axial_ray(center_a + offset, 1.0), axial_entry(center_a + offset, 1.0), 40, sc);
        if (moved.reason != base.reason || moved.events_used != base.events_used)
            ++placement_mismatches;
        for (int c = 0; c < 3; ++c)
            max_translation_weight_diff =
                std::max(max_translation_weight_diff,
                         std::fabs(moved.weight[c] - base.weight[c]));
    }
    TASK_CHECK(placement_mismatches < N / 50);  // Under 2% of near-tie paths may diverge.
    TASK_CHECK(max_translation_weight_diff < 1e-8);
    std::printf("boundary.invariance samples=%d placement_mismatches=%d max_weight_diff=%.2e\n",
                N, placement_mismatches, max_translation_weight_diff);

    // Stationary geometry: ray time changes nothing about the transport, and the same object is
    // queried so the result is bit-for-bit identical.
    splitmix64 rng_t0(0x714EULL), rng_t1(0x714EULL);
    auto st0 = make_sampler(rng_t0), st1 = make_sampler(rng_t1);
    const auto r0 = medium_a->random_walk(
        axial_ray(center_a, 1.0, 0.0), axial_entry(center_a, 1.0), 40, st0);
    const auto r1 = medium_a->random_walk(
        axial_ray(center_a, 1.0, 0.73), axial_entry(center_a, 1.0), 40, st1);
    TASK_CHECK(r0.reason == r1.reason);
    TASK_CHECK(r0.events_used == r1.events_used);
    for (int c = 0; c < 3; ++c)
        TASK_CHECK(approx(r0.weight[c], r1.weight[c], 1e-14));
    return 0;
}


//==============================================================================================
// Group: transport. Analytic finite-segment transmission, budget truncation, input validation,
// fixed-seed statistics for colored multi-collision weights and no-absorption energy, and real
// camera/PPM integration including outside occlusion and the legacy materials.
//==============================================================================================

// eta = 1, pure absorption, colored channels, one straight finite segment of length L = 2R.
// The exit frequency equals the mean survival and each surviving spectral weight equals
// exp(-sigma_c L) divided by that same mixture mean (ratio tracking, full mixed denominator).
static int case_absorption_transmission() {
    const int N = 40000;
    const vec3 sigma_a(0.5, 1.0, 1.5);
    const vec3 zero(0, 0, 0);
    const double L = 2.0;
    auto medium = make_sphere_medium(vec3(0, 0, 0), 1.0, zero, sigma_a, 1.0);

    splitmix64 rng(0xAB50AB5ULL);
    auto sampler = make_sampler(rng);
    int exited = 0;
    vec3 weight_sum(0, 0, 0);
    for (int i = 0; i < N; ++i) {
        auto result = medium->random_walk(axial_ray(vec3(0, 0, 0), 1.0),
                                          axial_entry(vec3(0, 0, 0), 1.0), 50, sampler);
        TASK_CHECK(result.reason == subsurface_termination::absorbed ||
                   result.reason == subsurface_termination::exited);
        if (result.reason == subsurface_termination::exited) {
            ++exited;
            weight_sum += result.weight;
        }
    }

    double survival_mix = 0.0;
    vec3 transmittance;
    for (int c = 0; c < 3; ++c) {
        transmittance[c] = std::exp(-sigma_a[c] * L);
        survival_mix += transmittance[c];
    }
    survival_mix /= 3.0;

    const double measured_survival = double(exited) / N;
    const double survival_error = std::fabs(measured_survival - survival_mix);
    std::printf("transport.absorption samples=%d survival=%.5f expected=%.5f error=%.5f\n",
                N, measured_survival, survival_mix, survival_error);
    TASK_CHECK(survival_error < 0.015);

    for (int c = 0; c < 3; ++c) {
        const double expected_weight = transmittance[c] / survival_mix;
        const double measured_weight = weight_sum[c] / exited;
        const double error = std::fabs(measured_weight - expected_weight);
        std::printf("transport.absorption channel=%d mean_weight=%.5f expected=%.5f error=%.5f\n",
                    c, measured_weight, expected_weight, error);
        TASK_CHECK(error < 0.02);
    }
    return 0;
}

// The event budget is shared with the camera: no internal cap exists, an exhausted budget
// returns zero weight, and reflected entries still charge their single interface event.
static int case_budget_truncation() {
    auto medium = make_sphere_medium(vec3(0, 0, 0), 0.3, vec3(30, 30, 30),
                                     vec3(0, 0, 0), 1.5);
    splitmix64 rng(0xB0D6E7ULL);
    auto sampler = make_sampler(rng);
    const int N = 10000;
    int budget = 0, reflected = 0;
    for (int i = 0; i < N; ++i) {
        auto result = medium->random_walk(axial_ray(vec3(0, 0, 0), 0.3),
                                          axial_entry(vec3(0, 0, 0), 0.3), 1, sampler);
        TASK_CHECK(result.events_used == 1);
        if (result.reason == subsurface_termination::budget) {
            ++budget;
            TASK_CHECK(result.weight[0] == 0.0 && result.weight[1] == 0.0 &&
                       result.weight[2] == 0.0);
        } else {
            TASK_CHECK(result.reason == subsurface_termination::reflected);
            ++reflected;
            TASK_CHECK(approx(result.weight[0], 1.0, 1e-12));
        }
        // With a single event no walk can enter and collide, let alone exit.
        TASK_CHECK(result.reason != subsurface_termination::exited);
    }
    TASK_CHECK(budget > 0 && reflected > 0);

    auto zero_budget = medium->random_walk(axial_ray(vec3(0, 0, 0), 0.3),
                                           axial_entry(vec3(0, 0, 0), 0.3), 0, sampler);
    TASK_CHECK(zero_budget.reason == subsurface_termination::budget);
    TASK_CHECK(zero_budget.events_used == 0);

    // A deeper budget still bounds the event count and zeroes exhausted walks.
    for (int i = 0; i < 2000; ++i) {
        auto result = medium->random_walk(axial_ray(vec3(0, 0, 0), 0.3),
                                          axial_entry(vec3(0, 0, 0), 0.3), 5, sampler);
        TASK_CHECK(result.events_used <= 5);
        if (result.reason == subsurface_termination::budget)
            TASK_CHECK(result.weight[0] == 0.0 && result.weight[2] == 0.0);
    }
    return 0;
}

static int case_invalid_inputs() {
    auto wall = std::make_shared<lambertian>(color(0.5, 0.5, 0.5));
    auto boundary = std::make_shared<sphere>(point3(0, 0, 0), 1.0, wall);
    const vec3 good_s(1, 1, 1), good_a(0.1, 0.1, 0.1);

    auto throws_invalid = [&](std::shared_ptr<hittable> b, const subsurface_params& p) {
        try {
            subsurface_object object(b, p);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    TASK_CHECK(throws_invalid(nullptr, subsurface_params(good_s, good_a, 1.4)));
    TASK_CHECK(throws_invalid(boundary, subsurface_params(vec3(-1, 1, 1), good_a, 1.4)));
    TASK_CHECK(throws_invalid(boundary, subsurface_params(good_s, vec3(1, -1, 1), 1.4)));
    // One channel whose scattering and absorption are both zero fails the strict-positive total.
    TASK_CHECK(throws_invalid(boundary,
              subsurface_params(vec3(0, 1, 1), vec3(0, 0.1, 0.1), 1.4)));
    TASK_CHECK(throws_invalid(boundary, subsurface_params(good_s, vec3(-1, 1, 1), 1.4)));
    TASK_CHECK(throws_invalid(boundary, subsurface_params(good_s, good_a, 0.0)));
    TASK_CHECK(throws_invalid(boundary, subsurface_params(good_s, good_a,
                                                          std::numeric_limits<double>::infinity())));
    TASK_CHECK(throws_invalid(boundary, subsurface_params(
        vec3(std::nan(""), 1, 1), good_a, 1.4)));

    auto medium = make_sphere_medium(vec3(0, 0, 0), 1.0, good_s, good_a, 1.4);
    auto r = axial_ray(vec3(0, 0, 0), 1.0);
    auto rec = axial_entry(vec3(0, 0, 0), 1.0);

    // Missing sampler and out-of-range samples are reported, never looped on.
    TASK_CHECK([&]() {
        try { medium->random_walk(r, rec, 10, nullptr); return false; }
        catch (const std::invalid_argument&) { return true; }
    }());
    const double bad_samples[] = {0.0, 1.0, -0.5, 1.5,
                                  std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::infinity()};
    for (double bad : bad_samples) {
        subsurface_sampler sampler = [bad]() { return bad; };
        try {
            medium->random_walk(r, rec, 10, sampler);
            TASK_CHECK(false);
        } catch (const std::invalid_argument&) {
            TASK_CHECK(true);
        }
    }

    // A walk must begin at an outside front-face entry.
    hit_record inside = rec;
    inside.front_face = false;
    splitmix64 rng(12345);
    auto good_sampler = make_sampler(rng);
    try {
        medium->random_walk(r, inside, 10, good_sampler);
        TASK_CHECK(false);
    } catch (const std::invalid_argument&) {
        TASK_CHECK(true);
    }
    return 0;
}

// No absorption: scalar coefficients make every ratio-tracking update cancel exactly, so the
// spectral weight remains unit through arbitrarily many collisions. Entry Fresnel reflection
// returns a unit-weight external path; a rare grazing TIR chain can exhaust the shared budget
// before a collision re-randomizes the direction, in which case the walk must report zero weight.
static int case_no_absorption_energy() {
    const int N = 30000;
    auto medium = make_sphere_medium(vec3(0, 0, 0), 1.0,
                                     vec3(3.0, 3.0, 3.0), vec3(0, 0, 0), 1.4);
    splitmix64 rng(0xE0E0E0ULL);
    auto sampler = make_sampler(rng);
    int exited = 0, entry_reflected = 0, budget_cut = 0;
    int multi_collision = 0;
    for (int i = 0; i < N; ++i) {
        auto result = medium->random_walk(axial_ray(vec3(0, 0, 0), 1.0),
                                          axial_entry(vec3(0, 0, 0), 1.0), 400, sampler);
        if (result.reason == subsurface_termination::reflected) {
            // External reflection at the entry interface: unit weight, one charged event.
            ++entry_reflected;
            TASK_CHECK(result.events_used == 1);
            TASK_CHECK(approx(result.weight[0], 1.0, 1e-12));
            TASK_CHECK(approx(result.weight[2], 1.0, 1e-12));
            continue;
        }
        if (result.reason == subsurface_termination::budget) {
            // The shared budget truncates the walk: it must contribute nothing.
            ++budget_cut;
            TASK_CHECK(result.weight[0] == 0.0 && result.weight[1] == 0.0 &&
                       result.weight[2] == 0.0);
            continue;
        }
        TASK_CHECK(result.reason == subsurface_termination::exited);
        // Entry eta factors cancel on exit; all internal ratio updates are exactly unit.
        TASK_CHECK(approx(result.weight[0], 1.0, 1e-10));
        TASK_CHECK(approx(result.weight[1], 1.0, 1e-10));
        TASK_CHECK(approx(result.weight[2], 1.0, 1e-10));
        if (result.events_used > 3) ++multi_collision;
        ++exited;
    }
    TASK_CHECK(exited + entry_reflected + budget_cut == N);
    TASK_CHECK(entry_reflected > 0);  // Normal-incidence Fresnel reflection really does happen.
    TASK_CHECK(budget_cut < N / 200);  // Grazing-TIR budget truncation stays under 0.5%.
    TASK_CHECK(multi_collision > 0);
    std::printf("transport.no_absorption samples=%d exited=%d entry_reflected=%d budget_cut=%d "
                "multi_collision_walks=%d weight=1.0\n",
                N, exited, entry_reflected, budget_cut, multi_collision);
    return 0;
}

// Colored coefficients with absorption and multiple collisions: fixed-seed statistics with the
// reported sample count and standard error. Stronger per-channel absorption must give a smaller
// mean transmitted weight, and every weight stays finite and non-negative.
static int case_colored_weight_statistics() {
    const int N = 40000;
    auto medium = make_sphere_medium(vec3(0, 0, 0), 1.0,
                                     vec3(2.5, 2.0, 1.5), vec3(0.2, 0.6, 1.2), 1.4);
    splitmix64 rng(0xC010AEDULL);
    auto sampler = make_sampler(rng);
    vec3 sum(0, 0, 0), sum_sq(0, 0, 0);
    int exited = 0, max_events = 0;
    for (int i = 0; i < N; ++i) {
        auto result = medium->random_walk(axial_ray(vec3(0, 0, 0), 1.0),
                                          axial_entry(vec3(0, 0, 0), 1.0), 400, sampler);
        if (result.reason != subsurface_termination::exited)
            continue;
        ++exited;
        max_events = std::max(max_events, result.events_used);
        for (int c = 0; c < 3; ++c) {
            TASK_CHECK(std::isfinite(result.weight[c]) && result.weight[c] >= 0.0);
            sum[c] += result.weight[c];
            sum_sq[c] += result.weight[c] * result.weight[c];
        }
    }
    TASK_CHECK(exited > N / 2);
    TASK_CHECK(max_events > 3);  // The configuration really does produce repeated collisions.

    for (int c = 0; c < 3; ++c) {
        const double mean = sum[c] / exited;
        const double variance = std::fmax(0.0, sum_sq[c] / exited - mean * mean);
        const double stderr = std::sqrt(variance / exited);
        std::printf("transport.colored channel=%d samples=%d mean=%.6f stderr=%.6f\n",
                    c, exited, mean, stderr);
        TASK_CHECK(stderr > 0.0 && stderr < 0.02);
        TASK_CHECK(mean > 0.0 && mean < 1.0);
    }
    const vec3 mean = vec3(sum[0] / exited, sum[1] / exited, sum[2] / exited);
    // Absorption grows R->G->B while scattering shrinks, so transmitted weight must decrease.
    TASK_CHECK(mean[0] > mean[1] && mean[1] > mean[2]);
    return 0;
}

// Full camera integration through trace_ray/render: a sphere and a rotated/translated wax box,
// legacy lambertian/metal materials and a constant medium all render together. Exit rays resume
// world queries, so the outside background (light vs. occlusion) changes the result.
static int case_camera_ppm() {
    auto wall = std::make_shared<lambertian>(color(0.9, 0.9, 0.9));
    auto build_world = [wall](color background) {
        auto world = std::make_shared<hittable_list>();
        auto lights = std::make_shared<hittable_list>();

        auto ground = std::make_shared<lambertian>(color(0.4, 0.4, 0.4));
        world->add(std::make_shared<quad>(point3(-6, -1.2, -6), vec3(12, 0, 0),
                                          vec3(0, 0, 12), ground));
        auto metal_mat = std::make_shared<metal>(color(0.8, 0.6, 0.3), 0.1);
        world->add(std::make_shared<sphere>(point3(-2.7, -0.4, -1.3), 0.8, metal_mat));

        // All solids are deliberately spaced apart: overlapping interiors are unsupported.
        auto wax_sphere = std::make_shared<sphere>(point3(1.1, 0.0, -2.0), 0.9, wall);
        world->add(std::make_shared<subsurface_object>(
            wax_sphere, subsurface_params(vec3(2.2, 1.7, 1.2), vec3(0.08, 0.15, 0.3), 1.45)));

        std::shared_ptr<hittable> box_boundary =
            std::make_shared<translate>(
                std::make_shared<rotate_y>(
                    box(point3(-0.6, -0.6, -0.6), point3(0.6, 0.6, 0.6), wall), 25.0),
                vec3(-1.3, 0.0, -2.7));
        world->add(std::make_shared<subsurface_object>(
            box_boundary, subsurface_params(vec3(1.6, 2.1, 1.7), vec3(0.25, 0.08, 0.12), 1.4)));

        // Legacy constant medium from the earlier books, kept untouched.
        auto fog_boundary = std::make_shared<sphere>(point3(2.8, 0.1, -2.9), 0.7, wall);
        world->add(std::make_shared<constant_medium>(fog_boundary, 0.8, color(0.7, 0.8, 0.9)));

        auto lamp_mat = std::make_shared<diffuse_light>(color(6, 6, 6));
        auto lamp = std::make_shared<quad>(point3(-2, 3, -4), vec3(4, 0, 0),
                                           vec3(0, 0, 2.5), lamp_mat);
        world->add(lamp);
        lights->add(lamp);

        auto accel = std::make_shared<bvh_node>(*world);
        camera cam;
        cam.image_width = 16;
        cam.aspect_ratio = 1.0;
        cam.samples_per_pixel = 16;
        cam.max_depth = 16;
        cam.background = background;
        cam.vfov = 55;
        cam.lookfrom = point3(0, 1.2, 3.5);
        cam.lookat = point3(0, 0, -2);
        return std::pair<std::shared_ptr<bvh_node>,
                         std::pair<camera, std::shared_ptr<hittable_list>>>(
            accel, {cam, lights});
    };

    std::srand(20261122);
    auto bright_scene = build_world(color(1.0, 1.0, 1.0));
    auto bright = render_ppm(bright_scene.second.first, *bright_scene.first,
                             *bright_scene.second.second);
    std::srand(20261122);
    auto dark_scene = build_world(color(0.0, 0.0, 0.0));
    auto dark = render_ppm(dark_scene.second.first, *dark_scene.first,
                           *dark_scene.second.second);

    TASK_CHECK(bright.width == 16 && bright.height == 16);
    TASK_CHECK(bright.bytes.size() == 16u * 16u * 3u);
    TASK_CHECK(dark.bytes.size() == bright.bytes.size());
    int bright_min = 255, bright_max = 0, distinct = 0;
    int previous = -1;
    for (int v : bright.bytes) {
        TASK_CHECK(v >= 0 && v <= 255);
        bright_min = std::min(bright_min, v);
        bright_max = std::max(bright_max, v);
        if (v != previous) ++distinct;
        previous = v;
    }
    TASK_CHECK(bright_max > bright_min);  // The image carries real spatial variation.
    TASK_CHECK(distinct > 5);

    const double bright_mean = mean_byte(bright);
    const double dark_mean = mean_byte(dark);
    const double delta = bright_mean - dark_mean;
    std::printf("transport.camera_ppm samples=%d bright_mean=%.3f occluded_mean=%.3f delta=%.3f\n",
                16 * 16 * 16, bright_mean, dark_mean, delta);
    // Exit rays see the bright outside background in one scene and black occlusion in the other.
    TASK_CHECK(delta > 8.0);

    // The public linear-RGB entry shares the render path and returns finite linear radiance.
    std::srand(99);
    camera& cam = bright_scene.second.first;
    for (int k = 0; k < 64; ++k) {
        ray probe(point3(0, 0.5, 1.0),
                  vec3(random_double(-0.3, 0.3), random_double(-0.2, 0.2), -1.0),
                  random_double());
        color linear = cam.trace_ray(probe, 16, *bright_scene.first,
                                     *bright_scene.second.second);
        TASK_CHECK(std::isfinite(linear[0]) && std::isfinite(linear[1]) &&
                   std::isfinite(linear[2]));
        TASK_CHECK(linear[0] >= 0 && linear[1] >= 0 && linear[2] >= 0);
    }

    // Time flows through the walk: a moving boundary gives different radiance at two times.
    auto moving_world = std::make_shared<hittable_list>();
    auto moving_boundary = std::make_shared<sphere>(
        point3(-3.0, 0.0, -2.0), point3(0.0, 0.0, -2.0), 0.8, wall);
    moving_world->add(std::make_shared<subsurface_object>(
        moving_boundary, subsurface_params(vec3(2, 2, 2), vec3(0.1, 0.1, 0.1), 1.3)));
    bvh_node moving_accel(*moving_world);
    hittable_list no_lights;
    camera time_cam;
    time_cam.background = color(1, 1, 1);
    const color at_t0 = time_cam.trace_ray(ray(point3(0, 0, 0), vec3(0, 0, -1), 0.0),
                                           12, moving_accel, no_lights);
    const color at_t1 = time_cam.trace_ray(ray(point3(0, 0, 0), vec3(0, 0, -1), 1.0),
                                           12, moving_accel, no_lights);
    std::printf("transport.camera_time t0=(%.4f,%.4f,%.4f) t1=(%.4f,%.4f,%.4f)\n",
                at_t0[0], at_t0[1], at_t0[2], at_t1[0], at_t1[1], at_t1[2]);
    TASK_CHECK(std::fabs(at_t0[0] - at_t1[0]) +
               std::fabs(at_t0[1] - at_t1[1]) > 1e-6);
    return 0;
}


//==============================================================================================
// Registration
//==============================================================================================

extern const struct task_case task_cases[] = {
    {"distance",  "collision_density",      case_collision_density},
    {"distance",  "survival_weight",        case_survival_weight},

    {"boundary",  "fresnel_tir",            case_fresnel_tir},
    {"boundary",  "sphere_exit",            case_sphere_exit},
    {"boundary",  "box_exit",               case_box_exit},
    {"boundary",  "walk_invariance",        case_walk_invariance},

    {"transport", "absorption_transmission", case_absorption_transmission},
    {"transport", "budget_truncation",      case_budget_truncation},
    {"transport", "invalid_inputs",         case_invalid_inputs},
    {"transport", "no_absorption_energy",   case_no_absorption_energy},
    {"transport", "colored_weight_stats",   case_colored_weight_statistics},
    {"transport", "camera_ppm",             case_camera_ppm},
};

extern const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);


#endif
