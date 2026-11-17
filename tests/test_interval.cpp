#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "util/Interval.h"
#include <cmath>
#include <limits>

using Catch::Matchers::WithinAbs;

TEST_CASE("Interval default constructor is empty", "[interval]") {
    Interval i;
    REQUIRE(i.min == std::numeric_limits<double>::infinity());
    REQUIRE(i.max == -std::numeric_limits<double>::infinity());
    REQUIRE(i.size() < 0);
}

TEST_CASE("Interval two-value constructor", "[interval]") {
    Interval i(1.0, 5.0);
    REQUIRE(i.min == 1.0);
    REQUIRE(i.max == 5.0);
    REQUIRE(i.size() == 4.0);
}

TEST_CASE("Interval surrounds returns true inside, false outside", "[interval]") {
    Interval i(1.0, 5.0);
    REQUIRE(i.surrounds(3.0));
    REQUIRE_FALSE(i.surrounds(0.0));
    REQUIRE_FALSE(i.surrounds(6.0));
}

TEST_CASE("Interval surrounds is exclusive on boundaries", "[interval]") {
    Interval i(1.0, 5.0);
    REQUIRE_FALSE(i.surrounds(1.0));
    REQUIRE_FALSE(i.surrounds(5.0));
}

TEST_CASE("Interval size returns max minus min", "[interval]") {
    Interval i(-3.0, 7.0);
    REQUIRE(i.size() == 10.0);
}

TEST_CASE("Interval expand adds delta/2 on each side", "[interval]") {
    Interval i(2.0, 6.0);
    Interval expanded = i.expand(4.0);
    REQUIRE(expanded.min == 0.0);
    REQUIRE(expanded.max == 8.0);
}

TEST_CASE("Interval enclosing constructor encloses both", "[interval]") {
    Interval a(1.0, 4.0);
    Interval b(2.0, 6.0);
    Interval c(a, b);
    REQUIRE(c.min == 1.0);
    REQUIRE(c.max == 6.0);
}

TEST_CASE("Interval enclosing with disjoint intervals", "[interval]") {
    Interval a(0.0, 1.0);
    Interval b(5.0, 8.0);
    Interval c(a, b);
    REQUIRE(c.min == 0.0);
    REQUIRE(c.max == 8.0);
}
