#include "sphere_connection.hpp"
#include <algorithm>
#include <cmath>

namespace {

// Minimal double-precision 3-vector. All root finding and Snell math is
// done in double precision as required; the float Vec3f of the scene is
// only converted at the boundaries.
struct V3d {
  double x{0.0}, y{0.0}, z{0.0};
  V3d() = default;
  V3d(double xx, double yy, double zz) : x(xx), y(yy), z(zz) {}
  explicit V3d(const Vec3f &v)
      : x(static_cast<double>(v.x)),
        y(static_cast<double>(v.y)),
        z(static_cast<double>(v.z)) {}

  V3d operator+(const V3d &o) const {
    return {x + o.x, y + o.y, z + o.z};
  }
  V3d operator-(const V3d &o) const {
    return {x - o.x, y - o.y, z - o.z};
  }
  V3d operator*(double s) const { return {x * s, y * s, z * s}; }
  V3d &operator+=(const V3d &o) {
    x += o.x;
    y += o.y;
    z += o.z;
    return *this;
  }
};

double Dot(const V3d &a, const V3d &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
V3d Cross(const V3d &a, const V3d &b) {
  return {a.y * b.z - a.z * b.y,
          a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
double NormSq(const V3d &a) { return Dot(a, a); }
double Norm(const V3d &a) { return std::sqrt(NormSq(a)); }
V3d Unit(const V3d &a) {
  const double n = Norm(a);
  return {a.x / n, a.y / n, a.z / n};
}

// Exact unpolarized (s/p averaged) Fresnel power transmittance for a
// single interface, air (n1=1) -> dielectric (n2=eta) or the reverse.
// `cos_air` is the angle cosine measured on the air side.
double FresnelTransmission(double eta, double cos_air) {
  cos_air = std::clamp(cos_air, 0.0, 1.0);
  const double sin2_air = 1.0 - cos_air * cos_air;
  const double cos2_med = 1.0 - sin2_air / (eta * eta);
  const double cos_med = std::sqrt(std::max(cos2_med, 0.0));
  // amplitude reflection coefficients for s and p polarization
  const double rs = (cos_air - eta * cos_med) /
                    (cos_air + eta * cos_med);
  const double rp = (eta * cos_air - cos_med) /
                    (eta * cos_air + cos_med);
  const double reflectance = 0.5 * (rs * rs + rp * rp);
  return 1.0 - reflectance;
}

// A fully traced candidate path for an emission angle alpha, together
// with the signed exit-interface residual (radians).
struct PathInfo {
  bool valid{false};
  double residual{0.0};
  double cos_entry_air{0.0};
  double cos_exit_air{0.0};
  double s1{0.0}, s2{0.0}, s3{0.0}; // positive segment lengths
  V3d u{};                          // first segment unit direction
  V3d entry{};
  V3d exit_;
};

struct Solver {
  double R, eta;
  V3d P, L;                 // sphere-center frame
  double d;                 // |P|
  V3d e_axis{};             // P / d (points from center to P)
  V3d e_b{};                // positive signed-angle direction
  V3d e_n{};                // plane normal e_axis x e_b
  double alpha_max{0.0};

  Solver(double radius, double ior, const V3d &p, const V3d &l)
      : R(radius), eta(ior), P(p), L(l) {
    d = Norm(P);
    e_axis = P * (1.0 / d);
    const double proj = Dot(L, e_axis);
    V3d perp = L - e_axis * proj;
    e_b = Unit(perp);
    e_n = Cross(e_axis, e_b);
    alpha_max = std::asin(R / d);
  }

  // Trace the entry ray emitted at signed angle alpha and refract it
  // through both interfaces; the residual compares the Snell-predicted
  // exit direction with the geometric direction towards L.
  PathInfo Trace(double alpha) const {
    PathInfo out;
    const double ca = std::cos(alpha);
    const double sa = std::sin(alpha);
    // axis from P towards the center is -e_axis
    const V3d u = Unit(e_axis * (-ca) + e_b * sa);

    // first sphere intersection: |P + s u|^2 = R^2, keep the near root
    const double disc_in = R * R - d * d * sa * sa;
    if (!(disc_in > 0.0))
      return out;
    const double s1 = d * ca - std::sqrt(disc_in);
    if (!(s1 > 0.0))
      return out;
    const V3d E = P + u * s1;
    const V3d ni = E * (1.0 / R); // outward entry normal

    // Snell entry, air -> sphere: tangential component scales by 1/eta
    const double cos_i = -Dot(u, ni);
    if (!(cos_i > 0.0))
      return out;
    const V3d w_tan = (u + ni * cos_i) * (1.0 / eta);
    const double sin2_b = NormSq(w_tan);
    if (!(sin2_b < 1.0))
      return out;
    const double cos_b = std::sqrt(1.0 - sin2_b);
    const V3d w = w_tan - ni * cos_b; // interior unit direction
    if (std::abs(Norm(w) - 1.0) > 1e-10 || Dot(w, ni) >= 0.0)
      return out;

    // second sphere intersection along E + s w (chord through sphere)
    const double s2 = -2.0 * Dot(w, E);
    if (!(s2 > 0.0))
      return out;
    const V3d X = E + w * s2;
    const V3d ne = X * (1.0 / R); // outward exit normal
    if (Dot(w, ne) <= 0.0)
      return out;

    // Snell exit, sphere -> air: tangential component scales by eta.
    // eta*sin(beta) = sin(alpha) <= 1 by entry Snell, so this never
    // undergoes total internal reflection inside the entry cone.
    const double cos_b2 = Dot(w, ne);
    const V3d v_tan = (w - ne * cos_b2) * eta;
    const double sin2_g = NormSq(v_tan);
    if (!(sin2_g < 1.0))
      return out;
    const double cos_g = std::sqrt(1.0 - sin2_g);
    const V3d v_pred = v_tan + ne * cos_g;
    if (std::abs(Norm(v_pred) - 1.0) > 1e-10)
      return out;

    const V3d to_light = L - X;
    const double s3 = Norm(to_light);
    if (!(s3 > 0.0))
      return out;
    const V3d v = to_light * (1.0 / s3);

    // signed angle from v_pred to v about the plane normal
    const double residual =
        std::atan2(Dot(e_n, Cross(v_pred, v)), Dot(v_pred, v));
    if (!std::isfinite(residual))
      return out;

    out.valid = true;
    out.residual = residual;
    out.cos_entry_air = cos_i;
    out.cos_exit_air = Dot(ne, v);
    out.s1 = s1;
    out.s2 = s2;
    out.s3 = s3;
    out.u = u;
    out.entry = E;
    out.exit_ = X;
    return out;
  }
};

} // namespace

std::vector<SphereConnection>
ConnectSphere(const Sphere &sphere, const Vec3f &P_in, const Vec3f &L_in) {
  const double R = static_cast<double>(sphere.radius);
  const double eta = static_cast<double>(sphere.material.refractive_index);
  const V3d C(sphere.center);
  const V3d P = V3d(P_in) - C;
  const V3d L = V3d(L_in) - C;

  Solver solver(R, eta, P, L);

  // Root isolation over the complete feasible angular domain (the entry
  // cone |alpha| <= asin(R/d)). The grid only locates sign changes; every
  // bracket is refined by bisection and accepted only after a residual
  // back-substitution. Contracted roots are isolated simple solutions
  // spaced at least 1e-4 rad apart, so with this grid width the interval
  // between any two adjacent nodes is always far narrower than one root
  // spacing and every sign change (or a residual that hits zero directly
  // on a node) is captured; this is bracketed root isolation, not angle
  // sampling used as a substitute for the solve.
  constexpr int kGrid = 20000;
  constexpr double kAngleTol = 2e-5;
  constexpr double kRootGap = 1e-4;
  constexpr double kZeroEps = 1e-12;
  const double lo = -solver.alpha_max * (1.0 - 1e-12);
  const double hi = solver.alpha_max * (1.0 - 1e-12);

  struct Root {
    double alpha;
    double residual;
  };
  std::vector<Root> roots;

  // refine a sign bracket [a,b] (fa is the residual at a) by bisection
  auto bisect = [&](double a, double b, double fa) {
    PathInfo mid;
    for (int it = 0; it < 80; ++it) {
      const double m = 0.5 * (a + b);
      mid = solver.Trace(m);
      if (!mid.valid)
        break;
      if (fa * mid.residual <= 0.0) {
        b = m;
      } else {
        a = m;
        fa = mid.residual;
      }
      if (b - a < 1e-14)
        break;
    }
    const double root_alpha = 0.5 * (a + b);
    PathInfo info = solver.Trace(root_alpha);
    if (info.valid && std::abs(info.residual) <= kAngleTol)
      roots.push_back({root_alpha, info.residual});
  };

  double prev_alpha = lo;
  PathInfo prev = solver.Trace(prev_alpha);
  for (int i = 1; i <= kGrid; ++i) {
    const double cur_alpha = lo + (hi - lo) * i / kGrid;
    PathInfo cur = solver.Trace(cur_alpha);
    if (cur.valid && std::abs(cur.residual) <= kZeroEps) {
      // a root lying exactly on a grid node
      roots.push_back({cur_alpha, cur.residual});
    } else if (prev.valid && cur.valid &&
               prev.residual * cur.residual < 0.0) {
      bisect(prev_alpha, cur_alpha, prev.residual);
    }
    prev = cur;
    prev_alpha = cur_alpha;
  }

  std::sort(roots.begin(), roots.end(),
            [](const Root &a, const Root &b) { return a.alpha < b.alpha; });
  // de-duplicate (distinct physical roots are guaranteed >= 1e-4 apart)
  std::vector<Root> unique_roots;
  for (const Root &r : roots) {
    if (!unique_roots.empty() &&
        r.alpha - unique_roots.back().alpha < kRootGap) {
      if (std::abs(r.residual) < std::abs(unique_roots.back().residual))
        unique_roots.back() = r;
      continue;
    }
    unique_roots.push_back(r);
  }

  const double transparency =
      static_cast<double>(sphere.material.transparency);
  const double mat_factor = transparency * transparency;
  constexpr double kCosMin = 0.1;
  constexpr double kPosTol = 2e-5;

  std::vector<SphereConnection> result;
  for (const Root &r : unique_roots) {
    PathInfo info = solver.Trace(r.alpha);
    if (!info.valid)
      continue;
    // geometry and feasibility gates from the task contract
    if (info.s1 <= 0.0 || info.s2 <= 0.0 || info.s3 <= 0.0)
      continue;
    if (info.cos_entry_air < kCosMin || info.cos_exit_air < kCosMin)
      continue;
    if (std::abs(Norm(info.entry) - R) > kPosTol * R ||
        std::abs(Norm(info.exit_) - R) > kPosTol * R)
      continue;
    // chord must lie inside the sphere: its midpoint is interior
    const V3d mid = (info.entry + info.exit_) * 0.5;
    if (Norm(mid) >= R)
      continue;
    // entry is the first surface crossing of the ray from P
    if (Norm(P + info.u * info.s1 - info.entry) > kPosTol * R)
      continue;

    const double t1 = FresnelTransmission(eta, info.cos_entry_air);
    const double t2 = FresnelTransmission(eta, info.cos_exit_air);
    SphereConnection conn;
    const V3d entry_world = info.entry + C;
    const V3d exit_world = info.exit_ + C;
    conn.entry = Vec3f{static_cast<float>(entry_world.x),
                       static_cast<float>(entry_world.y),
                       static_cast<float>(entry_world.z)};
    conn.exit = Vec3f{static_cast<float>(exit_world.x),
                      static_cast<float>(exit_world.y),
                      static_cast<float>(exit_world.z)};
    conn.transmission = mat_factor * t1 * t2;
    result.push_back(conn);
  }
  return result;
}
