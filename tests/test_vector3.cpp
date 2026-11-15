#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "util/Vector3.h"
#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("Vector3 default constructor zeros all elements", "[vector3]") {
    Vector3 v;
    REQUIRE(v.x() == 0.0);
    REQUIRE(v.y() == 0.0);
    REQUIRE(v.z() == 0.0);
}

TEST_CASE("Vector3 parameterized constructor sets elements", "[vector3]") {
    Vector3 v(1.0, 2.0, 3.0);
    REQUIRE(v.x() == 1.0);
    REQUIRE(v.y() == 2.0);
    REQUIRE(v.z() == 3.0);
}

TEST_CASE("Vector3 length and length_squared", "[vector3]") {
    Vector3 v(3.0, 4.0, 0.0);
    REQUIRE(v.length_squared() == 25.0);
    REQUIRE_THAT(v.length(), WithinAbs(5.0, 1e-12));
}

TEST_CASE("dot product: perpendicular vectors give zero", "[vector3]") {
    Vector3 a(1, 0, 0);
    Vector3 b(0, 1, 0);
    REQUIRE(dot(a, b) == 0.0);
}

TEST_CASE("dot product: parallel vectors give product of lengths", "[vector3]") {
    Vector3 a(3, 0, 0);
    Vector3 b(4, 0, 0);
    REQUIRE(dot(a, b) == 12.0);
}

TEST_CASE("dot product: arbitrary vectors", "[vector3]") {
    Vector3 a(1, 2, 3);
    Vector3 b(4, 5, 6);
    // 1*4 + 2*5 + 3*6 = 4 + 10 + 18 = 32
    REQUIRE(dot(a, b) == 32.0);
}

TEST_CASE("cross product: i x j = k", "[vector3]") {
    Vector3 i(1, 0, 0);
    Vector3 j(0, 1, 0);
    Vector3 result = cross(i, j);
    REQUIRE(result.x() == 0.0);
    REQUIRE(result.y() == 0.0);
    REQUIRE(result.z() == 1.0);
}

TEST_CASE("cross product perpendicular to inputs", "[vector3]") {
    Vector3 a(1, 2, 3);
    Vector3 b(4, 5, 6);
    Vector3 c = cross(a, b);
    REQUIRE_THAT(dot(a, c), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(dot(b, c), WithinAbs(0.0, 1e-12));
}

TEST_CASE("cross product magnitude equals |a||b|sin(theta)", "[vector3]") {
    Vector3 a(1, 0, 0);
    Vector3 b(0, 1, 0);
    Vector3 c = cross(a, b);
    // |a|=1, |b|=1, sin(90)=1, so |c|=1
    REQUIRE_THAT(c.length(), WithinAbs(1.0, 1e-12));
}

TEST_CASE("unit_vector returns unit length", "[vector3]") {
    Vector3 v(3, 4, 0);
    Vector3 u = unit_vector(v);
    REQUIRE_THAT(u.length(), WithinAbs(1.0, 1e-12));
    // Direction preserved: normalized components
    REQUIRE_THAT(u.x(), WithinAbs(0.6, 1e-12));
    REQUIRE_THAT(u.y(), WithinAbs(0.8, 1e-12));
}

TEST_CASE("unit_vector on zero vector returns zero", "[vector3]") {
    Vector3 zero(0, 0, 0);
    Vector3 result = unit_vector(zero);
    REQUIRE(result.x() == 0.0);
    REQUIRE(result.y() == 0.0);
    REQUIRE(result.z() == 0.0);
}

TEST_CASE("normalize returns unit length", "[vector3]") {
    Vector3 v(0, 0, 5);
    Vector3 n = normalize(v);
    REQUIRE_THAT(n.length(), WithinAbs(1.0, 1e-12));
    REQUIRE(n.x() == 0.0);
    REQUIRE(n.y() == 0.0);
    REQUIRE_THAT(n.z(), WithinAbs(1.0, 1e-12));
}

TEST_CASE("normalize zero returns zero", "[vector3]") {
    Vector3 zero(0, 0, 0);
    Vector3 result = normalize(zero);
    REQUIRE(result.x() == 0.0);
    REQUIRE(result.y() == 0.0);
    REQUIRE(result.z() == 0.0);
}

TEST_CASE("normalize with fallback returns fallback for zero", "[vector3]") {
    Vector3 zero(0, 0, 0);
    Vector3 fallback(1, 0, 0);
    Vector3 result = normalize(zero, fallback);
    REQUIRE(result.x() == fallback.x());
    REQUIRE(result.y() == fallback.y());
    REQUIRE(result.z() == fallback.z());
}

TEST_CASE("normalize with fallback returns normalized for nonzero", "[vector3]") {
    Vector3 v(0, 0, 3);
    Vector3 fallback(1, 0, 0);
    Vector3 result = normalize(v, fallback);
    REQUIRE_THAT(result.length(), WithinAbs(1.0, 1e-12));
}

TEST_CASE("Vector3 addition", "[vector3]") {
    Vector3 a(1, 2, 3);
    Vector3 b(4, 5, 6);
    Vector3 c = a + b;
    REQUIRE(c.x() == 5.0);
    REQUIRE(c.y() == 7.0);
    REQUIRE(c.z() == 9.0);
}

TEST_CASE("Vector3 subtraction", "[vector3]") {
    Vector3 a(4, 5, 6);
    Vector3 b(1, 2, 3);
    Vector3 c = a - b;
    REQUIRE(c.x() == 3.0);
    REQUIRE(c.y() == 3.0);
    REQUIRE(c.z() == 3.0);
}

TEST_CASE("Vector3 scalar multiplication", "[vector3]") {
    Vector3 v(1, 2, 3);
    Vector3 r = v * 2;
    REQUIRE(r.x() == 2.0);
    REQUIRE(r.y() == 4.0);
    REQUIRE(r.z() == 6.0);
    // Commutative
    Vector3 r2 = 2 * v;
    REQUIRE(r2.x() == r.x());
    REQUIRE(r2.y() == r.y());
    REQUIRE(r2.z() == r.z());
}

TEST_CASE("Vector3 scalar division", "[vector3]") {
    Vector3 v(6, 4, 2);
    Vector3 r = v / 2;
    REQUIRE(r.x() == 3.0);
    REQUIRE(r.y() == 2.0);
    REQUIRE(r.z() == 1.0);
}

TEST_CASE("Vector3 division by zero returns zero", "[vector3]") {
    Vector3 v(1, 2, 3);
    Vector3 r = v / 0;
    REQUIRE(r.x() == 0.0);
    REQUIRE(r.y() == 0.0);
    REQUIRE(r.z() == 0.0);
}

TEST_CASE("Vector3 negation", "[vector3]") {
    Vector3 v(1, -2, 3);
    Vector3 n = -v;
    REQUIRE(n.x() == -1.0);
    REQUIRE(n.y() == 2.0);
    REQUIRE(n.z() == -3.0);
}

TEST_CASE("Vector3 element-wise multiplication", "[vector3]") {
    Vector3 a(1, 2, 3);
    Vector3 b(4, 5, 6);
    Vector3 r = a * b;
    REQUIRE(r.x() == 4.0);
    REQUIRE(r.y() == 10.0);
    REQUIRE(r.z() == 18.0);
}

TEST_CASE("Vector3 near_zero returns true for small vectors", "[vector3]") {
    Vector3 tiny(1e-9, -1e-9, 1e-10);
    REQUIRE(tiny.near_zero());
    Vector3 not_tiny(0.01, 0, 0);
    REQUIRE_FALSE(not_tiny.near_zero());
}

TEST_CASE("perpendicular returns orthogonal vector", "[vector3]") {
    Vector3 v(1, 2, 3);
    Vector3 p = perpendicular(v);
    REQUIRE_THAT(dot(v, p), WithinAbs(0.0, 1e-12));
}

TEST_CASE("saturate clamps to [0,1]", "[vector3]") {
    Vector3 v(-0.5, 0.5, 1.5);
    Vector3 s = saturate(v);
    REQUIRE(s.x() == 0.0);
    REQUIRE(s.y() == 0.5);
    REQUIRE(s.z() == 1.0);
}
