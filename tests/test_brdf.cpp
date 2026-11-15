#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "brdf/FancyBRDF.h"
#include "brdf/Lambert.h"
#include "brdf/Microfacet.h"
#include "util/Utilities.h"

#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// Helper: generate a direction in the upper hemisphere around normal n
// using uniform hemisphere sampling. Returns (direction, cos_theta).
static std::pair<Vector3, double> uniform_hemisphere_sample(const Vector3& n) {
    double u1 = random_double();
    double u2 = random_double();
    double phi = 2.0 * pi * u2;
    double cos_theta = 1.0 - u1; // uniform: cos_theta in [-1,1] but for upper: [0,1]
    double sin_theta = sqrt(std::max(0.0, 1.0 - cos_theta * cos_theta));

    Vector3 tangent = unit_vector(perpendicular(n));
    Vector3 bitangent = cross(n, tangent);

    Vector3 dir = sin_theta * cos(phi) * tangent
                + sin_theta * sin(phi) * bitangent
                + cos_theta * n;
    return {unit_vector(dir), cos_theta};
}

// Helper: make a direction at a given polar angle from normal
static Vector3 make_direction(const Vector3& n, double angle_rad) {
    double cos_a = cos(angle_rad);
    double sin_a = sin(angle_rad);
    Vector3 tangent = unit_vector(perpendicular(n));
    return unit_vector(sin_a * tangent + cos_a * n);
}

// Helper: check if a Vector3 is finite (no NaN, no inf)
static bool is_finite_vec(const Vector3& v) {
    return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}

// Helper: check if all components >= 0
static bool is_nonnegative_vec(const Vector3& v) {
    return v.x() >= 0.0 && v.y() >= 0.0 && v.z() >= 0.0;
}

// ============================================================================
// Lambert Tests
// ============================================================================

TEST_CASE("Lambert::Evaluate is non-negative for valid inputs", "[brdf][lambert]") {
    auto ndotl = GENERATE(0.0, 0.1, 0.5, 0.9, 1.0);
    auto albedo = Vector3(0.5, 0.5, 0.5);

    Vector3 result = Lambert::Evaluate(ndotl, albedo);

    CHECK(is_nonnegative_vec(result));
}

TEST_CASE("Lambert::Evaluate matches formula albedo*ndotl/pi", "[brdf][lambert]") {
    double ndotl = GENERATE(0.0, 0.25, 0.5, 0.75, 1.0);
    Vector3 albedo = GENERATE(
        Vector3(0.5, 0.5, 0.5),
        Vector3(1.0, 0.0, 0.0),
        Vector3(0.2, 0.8, 0.4)
    );

    Vector3 result = Lambert::Evaluate(ndotl, albedo);
    Vector3 expected = albedo * ndotl / pi;

    REQUIRE_THAT(result.x(), WithinRel(expected.x(), 1e-10));
    REQUIRE_THAT(result.y(), WithinRel(expected.y(), 1e-10));
    REQUIRE_THAT(result.z(), WithinRel(expected.z(), 1e-10));
}

TEST_CASE("Lambert::CalculateLightDirection returns unit vector", "[brdf][lambert]") {
    Vector3 normal(0, 0, 1);

    for (int i = 0; i < 100; ++i) {
        Vector3 sample(random_double(), random_double(), 0);
        Vector3 dir = Lambert::CalculateLightDirection(normal, sample);

        REQUIRE_THAT(dir.length(), WithinAbs(1.0, 1e-9));
    }
}

TEST_CASE("Lambert::CalculateLightDirection stays in upper hemisphere", "[brdf][lambert]") {
    Vector3 normal(0, 0, 1);

    for (int i = 0; i < 100; ++i) {
        Vector3 sample(random_double(0.01, 1.0), random_double(0.01, 1.0), 0);
        Vector3 dir = Lambert::CalculateLightDirection(normal, sample);

        CHECK(dot(dir, normal) > 0.0);
    }
}

// ============================================================================
// Microfacet Tests
// ============================================================================

TEST_CASE("Microfacet::D is non-negative for valid inputs", "[brdf][microfacet]") {
    double alpha = GENERATE(0.01, 0.1, 0.5, 1.0);
    double ndoth = GENERATE(0.01, 0.1, 0.5, 0.9, 1.0);

    double d = Microfacet::D(alpha, ndoth);

    CHECK(d >= 0.0);
}

TEST_CASE("Microfacet::D integrates to ~1 over hemisphere (Monte Carlo)", "[brdf][microfacet]") {
    // GGX NDF normalization: ∫ D(h) cos(θ_h) dω_h = 1
    double alpha = GENERATE(0.1, 0.5, 1.0);
    const int N = 10000;
    const Vector3 n(0, 0, 1);

    double integral = 0.0;
    for (int i = 0; i < N; ++i) {
        auto [h, cos_theta] = uniform_hemisphere_sample(n);
        double ndoth = saturate(dot(n, h));
        double d = Microfacet::D(alpha, ndoth);
        // Uniform hemisphere: pdf = 1/(2π), weight = 2π * cos_theta * D
        integral += d * cos_theta;
    }
    integral *= (2.0 * pi / N);

    // Generous tolerance — Monte Carlo with 10k uniform samples has high variance
    // for peaked GGX distributions (low alpha)
    REQUIRE_THAT(integral, WithinAbs(1.0, 0.25));
}

TEST_CASE("Microfacet::G1 is in [0, 1] for valid inputs", "[brdf][microfacet]") {
    double alpha = GENERATE(0.01, 0.1, 0.5, 1.0);
    double ndotx = GENERATE(0.01, 0.1, 0.3, 0.5, 0.7, 0.9, 1.0);

    double g1 = Microfacet::G1(alpha, ndotx);

    CHECK(g1 >= 0.0);
    CHECK(g1 <= 1.0 + 1e-10);
}

TEST_CASE("Microfacet::V is non-negative for valid inputs", "[brdf][microfacet]") {
    double alpha = GENERATE(0.01, 0.1, 0.5, 1.0);
    double ndotl = GENERATE(0.01, 0.3, 0.6, 1.0);
    double ndotv = GENERATE(0.01, 0.3, 0.6, 1.0);

    double v = Microfacet::V(alpha, ndotl, ndotv);

    CHECK(v >= 0.0);
    CHECK(std::isfinite(v));
}

TEST_CASE("Microfacet::F (Schlick) returns f0 at vdoth=1", "[brdf][microfacet]") {
    Vector3 f0(0.5, 0.3, 0.1);

    Vector3 f = Microfacet::F(f0, 1.0);

    REQUIRE_THAT(f.x(), WithinAbs(f0.x(), 1e-10));
    REQUIRE_THAT(f.y(), WithinAbs(f0.y(), 1e-10));
    REQUIRE_THAT(f.z(), WithinAbs(f0.z(), 1e-10));
}

TEST_CASE("Microfacet::F (Schlick) returns 1 at vdoth=0 for any f0", "[brdf][microfacet]") {
    Vector3 f0 = GENERATE(
        Vector3(0.0, 0.0, 0.0),
        Vector3(0.5, 0.5, 0.5),
        Vector3(1.0, 0.0, 0.0)
    );

    Vector3 f = Microfacet::F(f0, 0.0);

    REQUIRE_THAT(f.x(), WithinAbs(1.0, 1e-10));
    REQUIRE_THAT(f.y(), WithinAbs(1.0, 1e-10));
    REQUIRE_THAT(f.z(), WithinAbs(1.0, 1e-10));
}

// ============================================================================
// FancyBRDF Tests
// ============================================================================

TEST_CASE("FancyBRDF::Evaluate is non-negative for valid l, v directions", "[brdf][fancy]") {
    Vector3 normal(0, 0, 1);
    double roughness = GENERATE(0.1, 0.5, 1.0);
    Vector3 albedo = GENERATE(
        Vector3(0.5, 0.5, 0.5),
        Vector3(1.0, 0.0, 0.0),
        Vector3(0.2, 0.8, 0.4)
    );
    Vector3 reflectance = GENERATE(
        Vector3(0.0, 0.0, 0.0),
        Vector3(0.5, 0.5, 0.5),
        Vector3(1.0, 1.0, 1.0)
    );

    FancyBRDF brdf(normal, albedo, reflectance, roughness);

    double view_angle = GENERATE(0.0, 0.7854 /* 45deg */, 1.4 /* ~80deg */);
    Vector3 v = make_direction(normal, view_angle);

    for (int i = 0; i < 50; ++i) {
        auto [l, cos_theta] = uniform_hemisphere_sample(normal);
        Vector3 result = brdf.Evaluate(l, v);

        CHECK(is_nonnegative_vec(result));
    }
}

TEST_CASE("FancyBRDF::Sample returns unit-length direction", "[brdf][fancy]") {
    Vector3 normal(0, 0, 1);
    double roughness = GENERATE(0.1, 0.5, 1.0);
    Vector3 albedo(0.5, 0.5, 0.5);
    Vector3 reflectance(0.5, 0.5, 0.5);

    FancyBRDF brdf(normal, albedo, reflectance, roughness);

    double view_angle = GENERATE(0.0, 0.7854);
    Vector3 v = make_direction(normal, view_angle);

    for (int i = 0; i < 100; ++i) {
        Vector3 l;
        Vector3 sample(random_double(), random_double(), 0);
        Vector3 weight = brdf.Sample(l, v, sample);

        CHECK_THAT(l.length(), WithinAbs(1.0, 1e-6));
    }
}

TEST_CASE("FancyBRDF::Sample weight is finite", "[brdf][fancy]") {
    Vector3 normal(0, 0, 1);
    double roughness = GENERATE(0.1, 0.5, 1.0);
    Vector3 albedo(0.5, 0.5, 0.5);
    Vector3 reflectance(0.5, 0.5, 0.5);

    FancyBRDF brdf(normal, albedo, reflectance, roughness);

    double view_angle = GENERATE(0.0, 0.7854, 1.4);
    Vector3 v = make_direction(normal, view_angle);

    for (int i = 0; i < 100; ++i) {
        Vector3 l;
        Vector3 sample_vec(random_double(), random_double(), 0);
        Vector3 weight = brdf.Sample(l, v, sample_vec);

        CHECK(is_finite_vec(weight));
    }
}

TEST_CASE("FancyBRDF energy conservation (moderate roughness)", "[brdf][fancy]") {
    // Monte Carlo estimate of ∫ f(l,v) cos(θ_l) dω_l for moderate roughness.
    // FancyBRDF::Evaluate returns the cosine-weighted BRDF already.
    // Uniform hemisphere sampling gives low variance for wide GGX lobes.
    // True integral is ≤ 1.0 per channel; 1.1 gives 10% margin.
    Vector3 normal(0, 0, 1);
    double roughness = GENERATE(0.5, 1.0);
    Vector3 albedo = GENERATE(
        Vector3(0.5, 0.5, 0.5),
        Vector3(1.0, 0.0, 0.0),
        Vector3(0.2, 0.8, 0.4)
    );
    Vector3 reflectance = GENERATE(
        Vector3(0.0, 0.0, 0.0),
        Vector3(0.5, 0.5, 0.5),
        Vector3(1.0, 1.0, 1.0)
    );

    FancyBRDF brdf(normal, albedo, reflectance, roughness);
    Vector3 v = make_direction(normal, 0.5); // ~29 degree view angle

    const int N = 5000;
    Vector3 integral(0, 0, 0);

    for (int i = 0; i < N; ++i) {
        auto [l, cos_theta] = uniform_hemisphere_sample(normal);
        Vector3 eval = brdf.Evaluate(l, v);
        integral += eval;
    }
    integral = integral * (2.0 * pi / N);

    CHECK(integral.x() < 1.1);
    CHECK(integral.y() < 1.1);
    CHECK(integral.z() < 1.1);
    CHECK(is_finite_vec(integral));
}

TEST_CASE("FancyBRDF boundedness smoke (all roughness)", "[brdf][fancy]") {
    // Smoke test: verify Evaluate stays bounded for all roughness values.
    // Glossy roughness (0.1 → alpha=0.01) produces narrow GGX spikes that
    // cause high variance under uniform hemisphere sampling. Using N=100000
    // to keep SE manageable (~0.14 worst case). This catches gross bugs
    // (NaN, infinity, wild energy gain) but does NOT test energy conservation.
    Vector3 normal(0, 0, 1);
    double roughness = GENERATE(0.1, 0.5, 1.0);
    Vector3 albedo = GENERATE(
        Vector3(0.5, 0.5, 0.5),
        Vector3(1.0, 0.0, 0.0),
        Vector3(0.2, 0.8, 0.4)
    );
    Vector3 reflectance = GENERATE(
        Vector3(0.0, 0.0, 0.0),
        Vector3(0.5, 0.5, 0.5),
        Vector3(1.0, 1.0, 1.0)
    );

    FancyBRDF brdf(normal, albedo, reflectance, roughness);
    Vector3 v = make_direction(normal, 0.5);

    const int N = 100000;
    Vector3 integral(0, 0, 0);

    for (int i = 0; i < N; ++i) {
        auto [l, cos_theta] = uniform_hemisphere_sample(normal);
        Vector3 eval = brdf.Evaluate(l, v);
        integral += eval;
    }
    integral = integral * (2.0 * pi / N);

    CHECK(integral.x() < 2.1);
    CHECK(integral.y() < 2.1);
    CHECK(integral.z() < 2.1);
    CHECK(is_finite_vec(integral));
}
