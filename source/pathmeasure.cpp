#include "pathmeasure.h"

#include <algorithm>
#include <cmath>

namespace lunasvg {

namespace {

constexpr double kEpsilon = 1e-12;
constexpr int kMaxSubdivisions = 64;

struct Cubic {
    double p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y;
};

static void cubicDerivative(const Cubic& c, double t, double& dx, double& dy)
{
    auto u = 1.0 - t;
    dx = 3.0 * u * u * (c.p1x - c.p0x) + 6.0 * u * t * (c.p2x - c.p1x) + 3.0 * t * t * (c.p3x - c.p2x);
    dy = 3.0 * u * u * (c.p1y - c.p0y) + 6.0 * u * t * (c.p2y - c.p1y) + 3.0 * t * t * (c.p3y - c.p2y);
}

static void cubicPoint(const Cubic& c, double t, double& x, double& y)
{
    auto u = 1.0 - t;
    auto a = u * u * u;
    auto b = 3.0 * u * u * t;
    auto d = 3.0 * u * t * t;
    auto e = t * t * t;
    x = a * c.p0x + b * c.p1x + d * c.p2x + e * c.p3x;
    y = a * c.p0y + b * c.p1y + d * c.p2y + e * c.p3y;
}

static double cubicSpeed(const Cubic& c, double t)
{
    double dx, dy;
    cubicDerivative(c, t, dx, dy);
    return std::sqrt(dx * dx + dy * dy);
}

// 8-point Gauss-Legendre quadrature of the speed function on [a, b].
// The four positive nodes of the [-1, 1] rule are evaluated symmetrically
// about the interval midpoint with their full standard weights.
static double gaussLength(const Cubic& c, double a, double b)
{
    static const double x[4] = {
        0.1834346424956498, 0.5255324099163290, 0.7966664774136267, 0.9602898564975363
    };
    static const double w[4] = {
        0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763
    };
    auto mid = (a + b) * 0.5;
    auto half = (b - a) * 0.5;
    auto sum = 0.0;
    for(int i = 0; i < 4; ++i) {
        sum += w[i] * (cubicSpeed(c, mid - half * x[i]) + cubicSpeed(c, mid + half * x[i]));
    }
    return half * sum;
}

// Adaptive Simpson style subdivision driven against the high-order Gauss
// estimate; recursion stops only when the estimate has converged, never at a
// fixed depth.
static double adaptiveLength(const Cubic& c, double a, double b, double gauss, double tolerance, int depth)
{
    auto mid = (a + b) * 0.5;
    auto left = gaussLength(c, a, mid);
    auto right = gaussLength(c, mid, b);
    auto refined = left + right;
    if(depth < kMaxSubdivisions && std::fabs(refined - gauss) > tolerance) {
        auto tol = tolerance * 0.5;
        return adaptiveLength(c, a, mid, left, tol, depth + 1)
            + adaptiveLength(c, mid, b, right, tol, depth + 1);
    }
    return refined;
}

// Inverts the arc length function on one cubic segment using Newton iteration
// with bisection as a fallback. Returns the parameter t in [0, 1].
static double solveParameter(const Cubic& c, double targetLength, double& outSpeed)
{
    auto low = 0.0;
    auto high = 1.0;
    auto t = targetLength > 0.0 ? 0.5 : 0.0;
    for(int iteration = 0; iteration < 32; ++iteration) {
        double dx, dy;
        cubicDerivative(c, t, dx, dy);
        auto speed = std::sqrt(dx * dx + dy * dy);
        if(speed < kEpsilon)
            break;
        auto current = gaussLength(c, 0.0, t);
        auto error = current - targetLength;
        if(std::fabs(error) < 1e-10) {
            outSpeed = speed;
            return t;
        }
        auto next = t - error / speed;
        if(next > low && next < high) {
            t = next;
        } else {
            if(error > 0.0) high = t; else low = t;
            t = (low + high) * 0.5;
        }
    }

    // Fallback: bisection on the integrated length.
    low = 0.0;
    high = 1.0;
    for(int i = 0; i < 64; ++i) {
        t = (low + high) * 0.5;
        auto current = gaussLength(c, 0.0, t);
        if(current < targetLength) low = t; else high = t;
    }
    t = (low + high) * 0.5;
    double dx, dy;
    cubicDerivative(c, t, dx, dy);
    outSpeed = std::sqrt(dx * dx + dy * dy);
    return t;
}

} // namespace

PathMeasure::PathMeasure(const Path& path, const Transform& transform)
{
    if(path.isNull())
        return;

    std::vector<Segment> segments;
    auto moveCount = 0;
    auto closed = false;
    Point current;

    PathIterator it(path);
    std::array<Point, 3> points;
    while(!it.isDone()) {
        auto command = it.currentSegment(points);
        switch(command) {
        case PathCommand::MoveTo:
            ++moveCount;
            current = transform.mapPoint(points[0]);
            break;
        case PathCommand::LineTo: {
            auto end = transform.mapPoint(points[0]);
            auto delta = end - current;
            auto length = std::sqrt(double(delta.x) * delta.x + double(delta.y) * delta.y);
            if(length > kEpsilon) {
                segments.push_back(Segment{current, Point(), Point(), end, true, length, 0.0});
            }
            current = end;
            break;
        }
        case PathCommand::CubicTo: {
            auto p0 = current;
            auto p1 = transform.mapPoint(points[0]);
            auto p2 = transform.mapPoint(points[1]);
            auto p3 = transform.mapPoint(points[2]);
            Cubic cubic{
                double(p0.x), double(p0.y), double(p1.x), double(p1.y),
                double(p2.x), double(p2.y), double(p3.x), double(p3.y)
            };
            auto gauss = gaussLength(cubic, 0.0, 1.0);
            auto length = adaptiveLength(cubic, 0.0, 1.0, gauss, 1e-9, 0);
            if(length > kEpsilon) {
                segments.push_back(Segment{p0, p1, p2, p3, false, length, 0.0});
            }
            current = p3;
            break;
        }
        case PathCommand::Close:
            closed = true;
            break;
        }
        it.next();
    }

    if(closed || moveCount != 1 || segments.empty())
        return;

    // The guard limits reject geometry or results outside the supported range.
    for(const auto& segment : segments) {
        const Point* points[4] = {&segment.p0, &segment.p1, &segment.p2, &segment.p3};
        for(auto point : points) {
            if(!std::isfinite(point->x) || !std::isfinite(point->y)
                || std::fabs(point->x) > 4096.0 || std::fabs(point->y) > 4096.0) {
                return;
            }
        }
    }

    auto total = 0.0;
    for(auto& segment : segments) {
        segment.start = total;
        total += segment.length;
    }

    if(!std::isfinite(total) || total <= kEpsilon || total > 40000.0)
        return;

    m_segments = std::move(segments);
    m_length = total;
    m_valid = true;
}

bool PathMeasure::sample(double distance, Point& position, Point& unitTangent) const
{
    if(!m_valid || !std::isfinite(distance) || distance < 0.0 || distance > m_length)
        return false;

    if(distance == m_length) {
        const auto& last = m_segments.back();
        double tx, ty;
        if(last.line) {
            auto delta = last.p3 - last.p0;
            auto length = std::sqrt(double(delta.x) * delta.x + double(delta.y) * delta.y);
            tx = delta.x / length;
            ty = delta.y / length;
        } else {
            Cubic cubic{
                double(last.p0.x), double(last.p0.y), double(last.p1.x), double(last.p1.y),
                double(last.p2.x), double(last.p2.y), double(last.p3.x), double(last.p3.y)
            };
            double dx, dy;
            cubicDerivative(cubic, 1.0, dx, dy);
            auto speed = std::sqrt(dx * dx + dy * dy);
            if(speed < kEpsilon) {
                // One-sided limit at the end: use the derivative just inside.
                cubicDerivative(cubic, 1.0 - 1e-6, dx, dy);
                speed = std::sqrt(dx * dx + dy * dy);
            }
            tx = dx / speed;
            ty = dy / speed;
        }
        position = last.p3;
        unitTangent = Point(float(tx), float(ty));
        return true;
    }

    // Locate the segment owning the distance.
    size_t index = 0;
    while(index + 1 < m_segments.size() && m_segments[index + 1].start <= distance)
        ++index;
    const auto& segment = m_segments[index];
    auto local = distance - segment.start;

    double x, y, tx, ty;
    if(segment.line) {
        auto delta = segment.p3 - segment.p0;
        auto length = std::sqrt(double(delta.x) * delta.x + double(delta.y) * delta.y);
        auto t = local / length;
        x = segment.p0.x + delta.x * t;
        y = segment.p0.y + delta.y * t;
        tx = delta.x / length;
        ty = delta.y / length;
    } else {
        Cubic cubic{
            double(segment.p0.x), double(segment.p0.y), double(segment.p1.x), double(segment.p1.y),
            double(segment.p2.x), double(segment.p2.y), double(segment.p3.x), double(segment.p3.y)
        };
        auto speed = 0.0;
        auto t = solveParameter(cubic, local, speed);
        cubicPoint(cubic, t, x, y);
        double dx, dy;
        cubicDerivative(cubic, t, dx, dy);
        speed = std::sqrt(dx * dx + dy * dy);
        if(speed < kEpsilon) {
            // Zero-speed point: take the one-sided limit of the tangent.
            auto probe = std::clamp(t, 0.0, 1.0 - 1e-6);
            if(probe <= 1e-6) probe = t + 1e-6; else probe = t - 1e-6;
            cubicDerivative(cubic, probe, dx, dy);
            speed = std::sqrt(dx * dx + dy * dy);
        }
        tx = dx / speed;
        ty = dy / speed;
    }

    position = Point(float(x), float(y));
    unitTangent = Point(float(tx), float(ty));
    return true;
}

} // namespace lunasvg
