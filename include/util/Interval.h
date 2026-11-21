#ifndef INTERVAL_H
#define INTERVAL_H

#include <limits>


class Interval {
  public:
    double min, max; // Interval bounds

    Interval() : min(+std::numeric_limits<double>::infinity()), max(-std::numeric_limits<double>::infinity()) {}

    Interval(double min, double max) : min(min), max(max) {}

    Interval(const Interval& a, const Interval& b) {
        // Create the interval tightly enclosing the two input intervals.
        min = a.min <= b.min ? a.min : b.min;
        max = a.max >= b.max ? a.max : b.max;
    }

    double size() const {
        // Return the size of the interval
        return max - min;
    }

    bool surrounds(double x) const {
        // Check if the interval surrounds the value x
        return min < x && x < max;
    }

    Interval expand(double delta) const {
        // Expand the interval by delta on both sides
        auto padding = delta/2;
        return Interval(min - padding, max + padding);
    }
};

#endif