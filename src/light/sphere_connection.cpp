#include "sphere_connection.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// pi is not part of strict C++17, so define it locally rather than rely
// on the kPi extension macro
constexpr double kPi = 3.14159265358979323846;

// ---------- double-precision 3-vector (the solver never uses float) ----------
struct DVec3 {
  double x{0.0}, y{0.0}, z{0.0};
  DVec3() = default;
  DVec3(double xx, double yy, double zz) : x(xx), y(yy), z(zz) {}
  DVec3(const Vec3f& v) : x(v.x), y(v.y), z(v.z) {}
  DVec3 operator+(const DVec3& o) const {
    return {x + o.x, y + o.y, z + o.z};
  }
  DVec3 operator-(const DVec3& o) const {
    return {x - o.x, y - o.y, z - o.z};
  }
  DVec3 operator*(double s) const { return {x * s, y * s, z * s}; }
  double Dot(const DVec3& o) const {
    return x * o.x + y * o.y + z * o.z;
  }
  DVec3 Cross(const DVec3& o) const {
    return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
  }
  double NormSq() const { return Dot(*this); }
  double Norm() const { return std::sqrt(NormSq()); }
  DVec3 Unit() const {
    const double n = Norm();
    return {x / n, y / n, z / n};
  }
  Vec3f ToFloat() const {
    return {static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(z)};
  }
};

// ----------------------------- interval arithmetic -----------------------------
struct Interval {
  double lo, hi;
  Interval(double l = 0.0, double h = 0.0) : lo(l), hi(h) {}
};

Interval operator+(Interval a, Interval b) {
  return {a.lo + b.lo, a.hi + b.hi};
}
Interval operator-(Interval a, Interval b) {
  return {a.lo - b.hi, a.hi - b.lo};
}
Interval operator-(Interval a) { return {-a.hi, -a.lo}; }
Interval operator*(Interval a, Interval b) {
  double v[4] = {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
  return {*std::min_element(v, v + 4), *std::max_element(v, v + 4)};
}
Interval ISqrt(Interval a) {
  return {std::sqrt(std::max(0.0, a.lo)), std::sqrt(std::max(0.0, a.hi))};
}
Interval ISin(Interval x) {
  double a = std::sin(x.lo), b = std::sin(x.hi);
  double lo = std::min(a, b), hi = std::max(a, b);
  // widen for extrema (pi/2 + 2k pi maxima, -pi/2 + 2k pi minima)
  for (long k = std::floor(x.lo / (2.0 * kPi)) - 1;
       k <= std::ceil(x.hi / (2.0 * kPi)) + 1; ++k) {
    double mx = kPi / 2.0 + 2.0 * kPi * k;
    double mn = -kPi / 2.0 + 2.0 * kPi * k;
    if (mx >= x.lo && mx <= x.hi) hi = 1.0;
    if (mn >= x.lo && mn <= x.hi) lo = -1.0;
  }
  return {lo, hi};
}
Interval ICos(Interval x) {
  double a = std::cos(x.lo), b = std::cos(x.hi);
  double lo = std::min(a, b), hi = std::max(a, b);
  for (long k = std::floor(x.lo / kPi) - 1;
       k <= std::ceil(x.hi / kPi) + 1; ++k) {
    double p = k * kPi;
    if (p >= x.lo && p <= x.hi) {
      if (k % 2 == 0) hi = 1.0;
      else lo = -1.0;
    }
  }
  return {lo, hi};
}
Interval IAsin(Interval x) {
  return {std::asin(std::clamp(x.lo, -1.0, 1.0)),
          std::asin(std::clamp(x.hi, -1.0, 1.0))};
}
bool ContainsZero(Interval x) { return x.lo <= 0.0 && x.hi >= 0.0; }

// Geometry of the reduced 2-D problem (unit sphere, P = (-p, 0),
// L = (lx, ly), sphere refractive index eta; air is 1).
struct Geo2D {
  double p, lx, ly, eta;
};

// Residual F(alpha): signed sine of the angle between the outgoing ray
// d3 and the vector exit -> L. A connection is a root F = 0.
// Derived from the two Snell refractions:
//   b = p sin alpha,  theta = asin b,  gamma = asin(b/eta),
//   delta = alpha - 2 theta + 2 gamma,
//   F = (lx+p) sin delta - ly cos delta
//       - sin(gamma-theta) * (2 te cos(gamma-theta) + 2 cos gamma)
// with te = p cos alpha - cos theta.
Interval FInterval(Interval a, const Geo2D& g) {
  Interval pp(g.p, g.p), ie(1.0 / g.eta, 1.0 / g.eta);
  Interval two(2.0, 2.0), one(1.0, 1.0);
  Interval b = pp * ISin(a);
  Interval ci = ISqrt(one - b * b);
  Interval ct = ISqrt(one - (b * ie) * (b * ie));
  Interval th = IAsin(b);
  Interval ga = IAsin(b * ie);
  Interval te = pp * ICos(a) - ci;
  Interval delta = a - th * two + ga * two;
  Interval q = ga - th;
  Interval s = ISin(q), c = ICos(q);
  Interval h = te * c * two + ct * two;
  return Interval(g.lx + g.p, g.lx + g.p) * ISin(delta)
         - Interval(g.ly, g.ly) * ICos(delta) - s * h;
}

// Analytic interval derivative F'(alpha), used to prove monotonicity.
Interval DInterval(Interval a, const Geo2D& g) {
  Interval pp(g.p, g.p), ie(1.0 / g.eta, 1.0 / g.eta);
  Interval two(2.0, 2.0), one(1.0, 1.0);
  Interval b = pp * ISin(a);
  Interval bp = pp * ICos(a);
  Interval ci = ISqrt(one - b * b);
  Interval ct = ISqrt(one - (b * ie) * (b * ie));
  Interval invCi(1.0 / ci.hi, 1.0 / ci.lo);
  Interval invCt(1.0 / ct.hi, 1.0 / ct.lo);
  Interval thetaP = bp * invCi;
  Interval gammaP = bp * ie * invCt;
  Interval ciP = -(b * bp) * invCi;
  Interval ctP = -(b * bp
                   * Interval(1.0 / (g.eta * g.eta), 1.0 / (g.eta * g.eta)))
                 * invCt;
  Interval teP = -pp * ISin(a) - ciP;
  Interval delta = a - IAsin(b) * two + IAsin(b * ie) * two;
  Interval deltaP = one - thetaP * two + gammaP * two;
  Interval q = IAsin(b * ie) - IAsin(b);
  Interval qP = gammaP - thetaP;
  Interval s = ISin(q), c = ICos(q);
  Interval sP = c * qP, cP = -s * qP;
  Interval te = pp * ICos(a) - ci;
  Interval h = te * c * two + ct * two;
  Interval hP = (teP * c + te * cP + ctP) * two;
  return Interval(g.lx + g.p, g.lx + g.p) * ICos(delta) * deltaP
         + Interval(g.ly, g.ly) * ISin(delta) * deltaP - sP * h - s * hP;
}

double FScalar(double a, const Geo2D& g) {
  return FInterval(Interval(a, a), g).lo;
}
double DScalar(double a, const Geo2D& g) {
  return DInterval(Interval(a, a), g).lo;
}

// Full forward construction of a path for a candidate emission angle.
// Returns the unit-sphere 2-D entry/exit points, segment lengths and
// both unit directions, applying Snell's law directly.
struct Path2D {
  double ex, ey;       // entry point
  double xx, xy;       // exit point
  double d1x, d1y;     // air direction P -> entry
  double d2x, d2y;     // internal direction entry -> exit
  double d3x, d3y;     // air direction exit -> L
  double te, tx, tL;   // segment lengths (unit-sphere scale)
  double ci, ct;       // air/glass side cosines of incidence
};

bool BuildPath(double alpha, const Geo2D& g, Path2D& out) {
  double sa = std::sin(alpha), ca = std::cos(alpha);
  double b = g.p * sa;
  if (b <= -1.0 || b >= 1.0) return false;
  double ci = std::sqrt(1.0 - b * b);
  double sg = b / g.eta;
  if (std::abs(sg) >= 1.0) return false; // total internal reflection
  double ct = std::sqrt(1.0 - sg * sg);
  // first air -> sphere intersection of the ray from (-p, 0)
  double te = g.p * ca - ci;
  double ex = -g.p + te * ca, ey = te * sa;
  // vector Snell: t = eta12 i + (eta12 cos_i - cos_t) n, eta12 = 1/eta
  double ri = 1.0 / g.eta;
  double k = ri * ci - ct;
  double d2x = ri * ca + k * ex;
  double d2y = ri * sa + k * ey;
  double n2 = std::hypot(d2x, d2y);
  d2x /= n2;
  d2y /= n2;
  // second intersection of the internal ray with the unit sphere
  double tx = -2.0 * (ex * d2x + ey * d2y);
  double xx = ex + tx * d2x, xy = ey + tx * d2y;
  // sphere -> air: eta12 = eta, outward normal at exit is (xx, xy)
  double k3 = g.eta * ct - ci;
  double d3x = g.eta * d2x - k3 * xx;
  double d3y = g.eta * d2y - k3 * xy;
  double n3 = std::hypot(d3x, d3y);
  d3x /= n3;
  d3y /= n3;
  double rx = g.lx - xx, ry = g.ly - xy;
  double tL = rx * d3x + ry * d3y;
  out = {ex, ey, xx, xy, ca, sa, d2x, d2y, d3x, d3y,
         te, tx, tL, ci, ct};
  return true;
}

// Exact unpolarized Fresnel power transmittance of one interface.
// n1/n2 are the two indices; cos_i/cos_t are measured against the
// (oriented) normal, both positive.
double FresnelTransmission(double n1, double n2, double cos_i,
                           double cos_t) {
  double rs = (n1 * cos_i - n2 * cos_t) / (n1 * cos_i + n2 * cos_t);
  double rp = (n2 * cos_i - n1 * cos_t) / (n2 * cos_i + n1 * cos_t);
  double reflectance = 0.5 * (rs * rs + rp * rp);
  return 1.0 - reflectance;
}

// Branch-and-prune root isolation over the whole feasible angle domain,
// with interval evaluations certifying sign changes and an analytic
// interval derivative certifying monotonicity inside each leaf.
std::vector<double> IsolateRoots(const Geo2D& g) {
  // |b| = p |sin alpha| <= sqrt(0.99) keeps the air-side incidence
  // cosine >= 0.1 at every point of the search domain.
  const double alpha_max = std::asin(std::sqrt(0.99) / g.p);
  std::vector<Interval> stack;
  stack.emplace_back(-alpha_max, alpha_max);
  std::vector<double> roots;
  long guard = 0;
  while (!stack.empty() && guard++ < 2'000'000) {
    Interval x = stack.back();
    stack.pop_back();
    if (!ContainsZero(FInterval(x, g))) continue;
    if (!ContainsZero(DInterval(x, g))) {
      // F is monotone on this interval: require a real sign change
      double fa = FScalar(x.lo, g);
      double fb = FScalar(x.hi, g);
      if (fa * fb > 0.0) continue;
      double lo = x.lo, hi = x.hi, flo = fa;
      for (int it = 0; it < 80; ++it) {
        double mid = 0.5 * (lo + hi);
        double fm = FScalar(mid, g);
        if (flo * fm <= 0.0) {
          hi = mid;
        } else {
          lo = mid;
          flo = fm;
        }
        if (hi - lo < 1e-16) break;
      }
      double root = 0.5 * (lo + hi);
      // Newton polish, rejected if it ever leaves the bracket
      for (int it = 0; it < 15; ++it) {
        double d = DScalar(root, g);
        if (d == 0.0) break;
        double next = root - FScalar(root, g) / d;
        if (!std::isfinite(next) || next < x.lo || next > x.hi) break;
        if (std::abs(next - root) < 1e-16) {
          root = next;
          break;
        }
        root = next;
      }
      roots.push_back(root);
      continue;
    }
    if (x.hi - x.lo < 1e-13) continue; // unresolved: not an isolated root
    double mid = 0.5 * (x.lo + x.hi);
    stack.emplace_back(x.lo, mid);
    stack.emplace_back(mid, x.hi);
  }
  std::sort(roots.begin(), roots.end());
  return roots;
}

} // namespace

std::vector<SphereConnection> ConnectSphere(const Sphere& sphere,
                                            const Vec3f& Pf,
                                            const Vec3f& Lf) {
  std::vector<SphereConnection> result;
  const DVec3 C(sphere.center);
  const double R = static_cast<double>(sphere.radius);
  const double eta = static_cast<double>(sphere.material.refractive_index);
  const DVec3 P(Pf), L(Lf);

  // orthonormal frame: e0 points from P towards the center; e1 is the
  // direction of the off-axis component of (L - center) in the plane
  // perpendicular to e0, fixing the positive signed-angle direction.
  // The whole path lies in the plane spanned by e0 and e1.
  DVec3 toCenter = C - P;
  const double pDist = toCenter.Norm();
  DVec3 e0 = toCenter * (1.0 / pDist);
  DVec3 lOff = L - C;
  DVec3 lPerp = lOff - e0 * lOff.Dot(e0);
  DVec3 e1 = lPerp.Unit();

  Geo2D g;
  g.p = pDist / R;
  g.lx = lOff.Dot(e0) / R;
  g.ly = lOff.Dot(e1) / R; // strictly positive by construction of e1
  g.eta = eta;

  const double transparency =
      static_cast<double>(sphere.material.transparency);

  for (double alpha : IsolateRoots(g)) {
    Path2D q;
    if (!BuildPath(alpha, g, q)) continue;
    // all three open segments must have strictly positive length
    if (!(q.te > 1e-12 && q.tx > 1e-12 && q.tL > 1e-12)) continue;
    // isolated simple roots only (the interval isolation already
    // proves F' != 0 over the bracketing interval)
    if (std::abs(DScalar(alpha, g)) < 1e-12) continue;
    if (std::abs(FScalar(alpha, g)) > 1e-9) continue;

    // lift the 2-D unit-sphere points into world coordinates
    DVec3 entry =
        C + (e0 * q.ex + e1 * q.ey) * R;
    DVec3 exit =
        C + (e0 * q.xx + e1 * q.xy) * R;
    DVec3 d1 = (entry - P).Unit();
    DVec3 d2 = (exit - entry).Unit();
    DVec3 d3 = (L - exit).Unit();
    DVec3 nEntry = (entry - C).Unit(); // outward normal at entry
    DVec3 nExit = (exit - C).Unit();   // outward normal at exit

    // residual back-substitution: both points lie on the sphere and
    // each segment is collinear with its endpoint pair
    auto close = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    if (!close((entry - C).Norm(), R) || !close((exit - C).Norm(), R))
      continue;
    if (d1.Dot(nEntry) >= 0.0) continue; // must enter from outside
    // midpoint of the internal chord must be inside the sphere
    if (((entry + exit) * 0.5 - C).Norm() >= R * (1.0 - 1e-12)) continue;

    // air-side incidence cosines at both interfaces (recomputed in 3-D,
    // not trusted from the 2-D construction); must be >= 0.1
    double cosEntry = -d1.Dot(nEntry);
    double cosExit = d3.Dot(nExit);
    if (cosEntry < 0.1 || cosExit < 0.1) continue;
    // internal incidence cosine against the exit outward normal; the
    // internal ray travels outward so this dot product is positive
    double cosInternal = d2.Dot(nExit);
    if (!(cosInternal > 0.0)) continue;

    // Snell back-substitution in vector form: tangential component of
    // the direction must satisfy n1 sin theta1 = n2 sin theta2
    auto snellResidual = [](DVec3 inc, DVec3 nrm, DVec3 refr, double n1,
                            double n2) {
      DVec3 t1 = inc - nrm * inc.Dot(nrm);
      DVec3 t2 = refr - nrm * refr.Dot(nrm);
      return std::abs(n1 * t1.Norm() - n2 * t2.Norm());
    };
    if (snellResidual(d1, nEntry, d2, 1.0, eta) > 1e-9 ||
        snellResidual(d2, DVec3(0, 0, 0) - nExit, d3, eta, 1.0) > 1e-9)
      continue;

    // exact unpolarized Fresnel transmittance, one factor per interface
    double sinEntry = std::sqrt(
        std::max(0.0, 1.0 - cosEntry * cosEntry));
    double cosTentry = std::sqrt(
        std::max(0.0, 1.0 - (sinEntry / eta) * (sinEntry / eta)));
    double tEntry = FresnelTransmission(1.0, eta, cosEntry, cosTentry);
    double sinExit = std::sqrt(
        std::max(0.0, 1.0 - cosInternal * cosInternal));
    double cosTexit = std::sqrt(
        std::max(0.0, 1.0 - (eta * sinExit) * (eta * sinExit)));
    double tExit =
        FresnelTransmission(eta, 1.0, cosInternal, cosTexit);

    SphereConnection conn;
    conn.entry = entry.ToFloat();
    conn.exit = exit.ToFloat();
    conn.transmission =
        transparency * transparency * tEntry * tExit;
    result.push_back(conn);
  }

  // roots were isolated in increasing signed emission angle; merge any
  // duplicate brackets (valid solutions are guaranteed >= 1e-4 apart)
  std::vector<SphereConnection> dedup;
  for (const auto& conn : result) {
    if (!dedup.empty()) {
      DVec3 prevE(dedup.back().entry);
      DVec3 curE(conn.entry);
      if ((curE - prevE).Norm() < R * 1e-7) continue;
    }
    dedup.push_back(conn);
  }
  return dedup;
}
