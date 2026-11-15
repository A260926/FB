#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "util/Ray.h"

using Catch::Matchers::WithinAbs;

TEST_CASE("Ray preserves origin and direction", "[ray]") {
    Point3 origin(1, 2, 3);
    Vector3 dir(4, 5, 6);
    Ray r(origin, dir);
    REQUIRE(r.origin().x() == 1.0);
    REQUIRE(r.origin().y() == 2.0);
    REQUIRE(r.origin().z() == 3.0);
    REQUIRE(r.direction().x() == 4.0);
    REQUIRE(r.direction().y() == 5.0);
    REQUIRE(r.direction().z() == 6.0);
}

TEST_CASE("Ray at(t=0) returns origin", "[ray]") {
    Point3 origin(10, 20, 30);
    Vector3 dir(1, 0, 0);
    Ray r(origin, dir);
    Point3 p = r.at(0);
    REQUIRE(p.x() == 10.0);
    REQUIRE(p.y() == 20.0);
    REQUIRE(p.z() == 30.0);
}

TEST_CASE("Ray at(t) computes origin + t*direction", "[ray]") {
    Point3 origin(0, 0, 0);
    Vector3 dir(1, 2, 3);
    Ray r(origin, dir);
    Point3 p = r.at(5.0);
    REQUIRE(p.x() == 5.0);
    REQUIRE(p.y() == 10.0);
    REQUIRE(p.z() == 15.0);
}

TEST_CASE("Ray at with negative t goes backward", "[ray]") {
    Point3 origin(10, 0, 0);
    Vector3 dir(1, 0, 0);
    Ray r(origin, dir);
    Point3 p = r.at(-3.0);
    REQUIRE(p.x() == 7.0);
    REQUIRE(p.y() == 0.0);
    REQUIRE(p.z() == 0.0);
}

TEST_CASE("Ray default constructor", "[ray]") {
    Ray r;
    REQUIRE(r.origin().x() == 0.0);
    REQUIRE(r.origin().y() == 0.0);
    REQUIRE(r.origin().z() == 0.0);
    REQUIRE(r.direction().x() == 0.0);
    REQUIRE(r.direction().y() == 0.0);
    REQUIRE(r.direction().z() == 0.0);
}
