// Exact rational geometry reference for the libtess2 task tests.
//
// Applicable input domain (see task_tests/README.md):
//   * contour points are integer grid points with |x|,|y| <= 32;
//   * every edge is horizontal, vertical, or has slope +-1;
//   * therefore every proper edge intersection lies on a half-integer grid
//     point (multiples of 1/2), and every event coordinate has denominator 2.
//
// The reference never calls into libtess2: it builds its own event bands
// straight from the original directed segments, computes horizontal
// crossings, winding numbers and filled intervals with integer/rational
// arithmetic, and lets the tests compare the tesselator output against it.
//
// Intermediate magnitude bound: input coordinate numerators are at most 32,
// reduced event fractions have numerator <= 64 with denominator 2, and the
// two sample heights per band use quarters (denominator <= 4). A crossing
// abscissa is a sum/products of such fractions, so every unreduced numerator
// stays below roughly 10^4 and every denominator below 16. int64 already
// holds these with an enormous margin; __int128 is used for the multiply
// intermediates anyway (capacity ~1.7e38) so overflow cannot bind the logic.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace lt2ref {

using i128 = __int128_t;

struct Frac {
    long long n = 0;
    long long d = 1;  // always positive

    Frac() = default;
    Frac(long long num, long long den = 1) {
        if (den == 0) { den = 1; num = 0; }
        if (den < 0) { num = -num; den = -den; }
        long long g = std::gcd(std::llabs(num), den);
        if (g == 0) g = 1;
        n = num / g;
        d = den / g;
    }

    static i128 gcd128(i128 a, i128 b) {
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        while (b) { i128 t = a % b; a = b; b = t; }
        return a;
    }
    static Frac raw(i128 num, i128 den) {
        if (den < 0) { num = -num; den = -den; }
        i128 g = gcd128(num, den);
        if (g == 0) g = 1;
        Frac f;
        f.n = (long long)(num / g);
        f.d = (long long)(den / g);
        return f;
    }

    double toDouble() const { return (double)n / (double)d; }
};

inline Frac operator+(const Frac& a, const Frac& b) {
    return Frac::raw((i128)a.n * b.d + (i128)b.n * a.d, (i128)a.d * b.d);
}
inline Frac operator-(const Frac& a, const Frac& b) {
    return Frac::raw((i128)a.n * b.d - (i128)b.n * a.d, (i128)a.d * b.d);
}
inline Frac operator-(const Frac& a) { return Frac(-a.n, a.d); }
inline Frac operator*(const Frac& a, const Frac& b) {
    return Frac::raw((i128)a.n * b.n, (i128)a.d * b.d);
}
inline Frac operator/(const Frac& a, const Frac& b) {
    return Frac::raw((i128)a.n * b.d, (i128)a.d * b.n);
}
inline bool operator==(const Frac& a, const Frac& b) {
    return (i128)a.n * b.d == (i128)b.n * a.d;
}
inline bool operator!=(const Frac& a, const Frac& b) { return !(a == b); }
inline bool operator<(const Frac& a, const Frac& b) {
    return (i128)a.n * b.d < (i128)b.n * a.d;
}
inline bool operator<=(const Frac& a, const Frac& b) { return !(b < a); }
inline bool operator>(const Frac& a, const Frac& b) { return b < a; }
inline bool operator>=(const Frac& a, const Frac& b) { return !(a < b); }

inline Frac absFrac(const Frac& f) { return Frac(std::llabs(f.n), f.d); }
inline Frac fracMin(const Frac& a, const Frac& b) { return a < b ? a : b; }
inline Frac fracMax(const Frac& a, const Frac& b) { return a < b ? b : a; }

// Map a normalized floating coordinate onto the half-integer grid. The
// nearest half-integer must be within 1e-4; anything farther away means the
// tesselator emitted a vertex the reference grid cannot explain, which is a
// failure rather than something to snap blindly.
inline bool snapHalf(double v, Frac& out) {
    double q = std::round(2.0 * v);
    double nearest = q / 2.0;
    if (std::fabs(v - nearest) > 1e-4) return false;
    out = Frac((long long)q, 2);
    return true;
}

struct FPoint {
    Frac x, y;
    FPoint() = default;
    FPoint(Frac x_, Frac y_) : x(x_), y(y_) {}
    FPoint(long long x_, long long y_) : x(x_, 1), y(y_, 1) {}
};
inline bool operator==(const FPoint& a, const FPoint& b) {
    return a.x == b.x && a.y == b.y;
}
inline bool operator!=(const FPoint& a, const FPoint& b) { return !(a == b); }
inline FPoint operator-(const FPoint& a, const FPoint& b) {
    return FPoint(a.x - b.x, a.y - b.y);
}
inline FPoint operator+(const FPoint& a, const FPoint& b) {
    return FPoint(a.x + b.x, a.y + b.y);
}
inline FPoint operator*(const FPoint& a, const Frac& k) {
    return FPoint(a.x * k, a.y * k);
}
inline Frac cross(const FPoint& a, const FPoint& b) {
    return a.x * b.y - a.y * b.x;
}

struct FSeg {
    FPoint a, b;
    int w;  // signed contribution of the directed contour edge (+-1)
};

struct Contour {
    std::vector<FPoint> pts;
    int sign = 1;  // +1: added as given, -1: added with TESS_REVERSE_CONTOURS
};

struct Scene {
    std::string name;
    std::vector<Contour> contours;
};

inline FPoint P(long long x, long long y) { return FPoint(x, y); }

inline std::vector<FSeg> allSegments(const Scene& s) {
    std::vector<FSeg> segs;
    for (const Contour& c : s.contours) {
        int n = (int)c.pts.size();
        for (int i = 0; i < n; ++i) {
            segs.push_back({c.pts[i], c.pts[(i + 1) % n], c.sign});
        }
    }
    return segs;
}

// Proper interior intersection of two segments. Collinear overlaps and
// endpoint touches contribute no new point (their coordinates are already
// endpoint events).
inline std::optional<FPoint> properIntersect(const FSeg& e1, const FSeg& e2) {
    const FPoint p = e1.a, r = e1.b - e1.a;
    const FPoint q = e2.a, s = e2.b - e2.a;
    Frac den = cross(r, s);
    if (den == Frac(0)) return std::nullopt;
    FPoint qp = q - p;
    Frac tn = cross(qp, s);
    Frac un = cross(qp, r);
    auto between = [](const Frac& num, const Frac& den) {
        // 0 < num/den < 1 with den possibly negative
        if (den > Frac(0)) return num > Frac(0) && num < den;
        return num < Frac(0) && num > den;
    };
    if (!between(tn, den) || !between(un, den)) return std::nullopt;
    Frac t = tn / den;
    return FPoint(p.x + r.x * t, p.y + r.y * t);
}

inline std::vector<FPoint> eventPoints(const Scene& s) {
    std::vector<FSeg> segs = allSegments(s);
    std::vector<FPoint> ev;
    for (const FSeg& e : segs) ev.push_back(e.a);
    for (size_t i = 0; i < segs.size(); ++i) {
        for (size_t j = i + 1; j < segs.size(); ++j) {
            auto ip = properIntersect(segs[i], segs[j]);
            if (ip) ev.push_back(*ip);
        }
    }
    std::sort(ev.begin(), ev.end(), [](const FPoint& a, const FPoint& b) {
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    ev.erase(std::unique(ev.begin(), ev.end()), ev.end());
    return ev;
}

inline std::vector<Frac> eventYs(const Scene& s) {
    std::vector<FPoint> ev = eventPoints(s);
    std::vector<Frac> ys;
    for (const FPoint& p : ev) ys.push_back(p.y);
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    return ys;
}

struct Crossing {
    Frac x;
    int w;  // winding step when crossing upwards through the line
};

// All directed segments crossing horizontal line y (strictly between their
// endpoints), grouped by equal abscissa with their winding steps summed.
inline std::vector<Crossing> crossingsAtY(const std::vector<FSeg>& segs,
                                          const Frac& y) {
    std::vector<Crossing> raw;
    for (const FSeg& e : segs) {
        if (e.a.y == e.b.y) continue;  // horizontal edge
        bool lo = fracMin(e.a.y, e.b.y) < y;
        bool hi = y < fracMax(e.a.y, e.b.y);
        if (!(lo && hi)) continue;
        Frac t = (y - e.a.y) / (e.b.y - e.a.y);
        Frac x = e.a.x + (e.b.x - e.a.x) * t;
        // Winding contribution of this crossing to a point on its right
        // (ray cast in +x): an upward edge to the right adds +w, a downward
        // edge subtracts w. The winding of an x-interval is the suffix sum.
        int step = (e.b.y > e.a.y) ? e.w : -e.w;
        raw.push_back({x, step});
    }
    std::sort(raw.begin(), raw.end(),
              [](const Crossing& a, const Crossing& b) { return a.x < b.x; });
    std::vector<Crossing> grouped;
    for (const Crossing& c : raw) {
        if (!grouped.empty() && grouped.back().x == c.x)
            grouped.back().w += c.w;
        else
            grouped.push_back(c);
    }
    return grouped;
}

inline bool ruleInside(int windingRule, int w) {
    switch (windingRule) {
        case 0: return (w & 1) != 0;             // TESS_WINDING_ODD
        case 1: return w != 0;                    // TESS_WINDING_NONZERO
        case 2: return w > 0;                     // TESS_WINDING_POSITIVE
        case 3: return w < 0;                     // TESS_WINDING_NEGATIVE
        case 4: return w >= 2 || w <= -2;         // TESS_WINDING_ABS_GEQ_TWO
    }
    return false;
}

inline const char* ruleName(int windingRule) {
    static const char* names[] = {"ODD", "NONZERO", "POSITIVE", "NEGATIVE",
                                  "ABS_GEQ_TWO"};
    return names[windingRule];
}

struct FInterval {
    Frac lo, hi;
};

// Filled open x-intervals of the scene on horizontal line y under the rule.
// Winding on an open interval is the suffix sum of the crossing steps to its
// right; the status changes at each crossing abscissa.
inline std::vector<FInterval> fillIntervals(const Scene& s, int windingRule,
                                            const Frac& y) {
    std::vector<FSeg> segs = allSegments(s);
    std::vector<Crossing> cr = crossingsAtY(segs, y);
    std::vector<FInterval> out;
    int w = 0;
    for (const Crossing& c : cr) w += c.w;  // winding of the far-left interval
    bool inside = ruleInside(windingRule, w);
    Frac cursor;
    for (const Crossing& c : cr) {
        bool was = inside;
        w -= c.w;  // step over this crossing into the interval on its right
        inside = ruleInside(windingRule, w);
        if (was && !inside) {
            out.push_back({cursor, c.x});
        } else if (!was && inside) {
            cursor = c.x;
        }
    }
    return out;
}

// Winding number of an arbitrary point guaranteed not to lie on an edge.
inline int windingAtPoint(const Scene& s, const FPoint& p) {
    std::vector<FSeg> segs = allSegments(s);
    std::vector<Crossing> cr = crossingsAtY(segs, p.y);
    int w = 0;
    for (const Crossing& c : cr)
        if (p.x < c.x) w += c.w;  // crossings strictly to the right
    return w;
}

inline bool fillAtPoint(const Scene& s, int windingRule, const FPoint& p) {
    return ruleInside(windingRule, windingAtPoint(s, p));
}

// A triangle of the tesselator output after its vertices were snapped to the
// reference grid.
struct RefTri {
    FPoint v[3];
};

inline Frac triArea2(const RefTri& t) {
    FPoint ab = t.v[1] - t.v[0];
    FPoint ac = t.v[2] - t.v[0];
    return absFrac(cross(ab, ac));
}

// Open x-intervals cut out of horizontal line y by the triangles.
inline std::vector<FInterval> triIntervalsAtY(const std::vector<RefTri>& tris,
                                              const Frac& y) {
    std::vector<FInterval> out;
    for (const RefTri& t : tris) {
        std::vector<Frac> xs;
        const FPoint* vp[3] = {&t.v[0], &t.v[1], &t.v[2]};
        for (int i = 0; i < 3; ++i) {
            const FPoint& a = *vp[i];
            const FPoint& b = *vp[(i + 1) % 3];
            if (a.y == b.y) continue;
            if (fracMin(a.y, b.y) < y && y < fracMax(a.y, b.y)) {
                Frac tt = (y - a.y) / (b.y - a.y);
                xs.push_back(a.x + (b.x - a.x) * tt);
            }
        }
        if (xs.size() == 2) {
            if (xs[1] < xs[0]) std::swap(xs[0], xs[1]);
            if (xs[0] != xs[1]) out.push_back({xs[0], xs[1]});
        }
    }
    return out;
}

// Coverage multiplicity of the triangle soup at one x.
inline int coverCount(const std::vector<FInterval>& iv, const Frac& x) {
    int k = 0;
    for (const FInterval& i : iv)
        if (i.lo < x && x < i.hi) ++k;
    return k;
}

// Signed polygon area (twice) of an ordered ring via the shoelace formula.
inline Frac ringArea2(const std::vector<FPoint>& ring) {
    Frac sum(0);
    int n = (int)ring.size();
    for (int i = 0; i < n; ++i) {
        const FPoint& a = ring[i];
        const FPoint& b = ring[(i + 1) % n];
        sum = sum + a.x * b.y - b.x * a.y;
    }
    return sum;
}

// Small offset pointing to the geometric left of a directed half-grid edge.
// The left normal of direction (dx,dy) is (-dy,dx), scaled so axis-aligned
// offsets have magnitude 1/8 and 45-degree offsets components of 1/8. The
// direction signs matter: a westward/southward edge's left side is opposite
// to an eastward/northward one's.
inline FPoint leftOffset(const FPoint& a, const FPoint& b) {
    FPoint d = b - a;
    long long sx = d.x.n < 0 ? -1 : (d.x.n > 0 ? 1 : 0);
    long long sy = d.y.n < 0 ? -1 : (d.y.n > 0 ? 1 : 0);
    if (d.y == Frac(0)) return FPoint(Frac(0), Frac(sx, 8));    // horizontal
    if (d.x == Frac(0)) return FPoint(Frac(-sy, 8), Frac(0));   // vertical
    return FPoint(Frac(-sy, 8), Frac(sx, 8));                   // 45-degree
}

inline std::string pointStr(const FPoint& p) {
    std::ostringstream os;
    os << "(" << p.x.n << "/" << p.x.d << "," << p.y.n << "/" << p.y.d << ")";
    return os.str();
}

inline std::string fracStr(const Frac& f) {
    std::ostringstream os;
    os << f.n << "/" << f.d;
    return os.str();
}

}  // namespace lt2ref
