#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "bvh/BoundingBox.h"

using Catch::Matchers::WithinAbs;

TEST_CASE("BoundingBox ray hitting box returns true", "[boundingbox]") {
    BoundingBox box(
        Interval(-1, 1),
        Interval(-1, 1),
        Interval(-1, 1)
    );
    Ray r(Point3(0, 0, 5), Vector3(0, 0, -1));
    REQUIRE(box.intersect(r, Interval(0, 100)));
}

TEST_CASE("BoundingBox ray missing box returns false", "[boundingbox]") {
    BoundingBox box(
        Interval(-1, 1),
        Interval(-1, 1),
        Interval(-1, 1)
    );
    Ray r(Point3(5, 5, 5), Vector3(0, 0, -1));
    REQUIRE_FALSE(box.intersect(r, Interval(0, 100)));
}

TEST_CASE("BoundingBox ray starting inside box returns true", "[boundingbox]") {
    BoundingBox box(
        Interval(-1, 1),
        Interval(-1, 1),
        Interval(-1, 1)
    );
    Ray r(Point3(0, 0, 0), Vector3(1, 0, 0));
    REQUIRE(box.intersect(r, Interval(0, 100)));
}

TEST_CASE("BoundingBox axis-aligned ray (direction component zero)", "[boundingbox]") {
    BoundingBox box(
        Interval(0, 2),
        Interval(0, 2),
        Interval(0, 2)
    );
    Ray r(Point3(1, 1, 5), Vector3(0, 0, -1));
    REQUIRE(box.intersect(r, Interval(0, 100)));
}

TEST_CASE("BoundingBox surface_area", "[boundingbox]") {
    BoundingBox box(
        Interval(0, 2),
        Interval(0, 3),
        Interval(0, 4)
    );
    // dx=2, dy=3, dz=4
    // face1=6, face2=12, face3=8
    // SA = 2*(6+12+8) = 52
    REQUIRE_THAT(box.surface_area(), WithinAbs(52.0, 1e-10));
}

TEST_CASE("BoundingBox from two points auto-sorts", "[boundingbox]") {
    BoundingBox box(Point3(5, 3, 1), Point3(1, 7, 5));
    REQUIRE(box.x.min <= box.x.max);
    REQUIRE(box.y.min <= box.y.max);
    REQUIRE(box.z.min <= box.z.max);
}

TEST_CASE("BoundingBox enclosing two boxes", "[boundingbox]") {
    BoundingBox a(Point3(0, 0, 0), Point3(1, 1, 1));
    BoundingBox b(Point3(5, 5, 5), Point3(6, 6, 6));
    BoundingBox c(a, b);
    REQUIRE(c.x.min == 0.0);
    REQUIRE(c.x.max == 6.0);
    REQUIRE(c.y.min == 0.0);
    REQUIRE(c.y.max == 6.0);
}

TEST_CASE("BoundingBox axis_interval returns correct axis", "[boundingbox]") {
    BoundingBox box(
        Interval(1, 2),
        Interval(3, 4),
        Interval(5, 6)
    );
    REQUIRE(box.axis_interval(0).min == 1.0);
    REQUIRE(box.axis_interval(1).min == 3.0);
    REQUIRE(box.axis_interval(2).min == 5.0);
}
