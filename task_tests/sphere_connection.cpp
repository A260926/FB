// Regression tests for the single-sphere lens connection feature.
// Three groups: connections (solver math), visibility (per-segment
// occlusion), lighting (ColorAt integration and a real Trace render).
// No main() is defined here; task_support/driver.c supplies it.
#include "../task_support/check.h"
#include "sphere_connection.hpp"
#include "light.hpp"
#include "ray_tracer.hpp"
#include "camera.hpp"
#include "ppm_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------
// Independent double-precision vector math (kept separate from the
// production V3d inside sphere_connection.cpp)
// ---------------------------------------------------------------------
struct DV {
  double x{0}, y{0}, z{0};
  DV() = default;
  DV(double a, double b, double c) : x(a), y(b), z(c) {}
  explicit DV(const Vec3f &v) : x(v.x), y(v.y), z(v.z) {}
};
DV operator+(const DV &a, const DV &b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
DV operator-(const DV &a, const DV &b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
DV operator*(const DV &a, double s) {
  return {a.x * s, a.y * s, a.z * s};
}
DV operator/(const DV &a, double s) {
  return {a.x / s, a.y / s, a.z / s};
}
double Dot(const DV &a, const DV &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
DV Cross(const DV &a, const DV &b) {
  return {a.y * b.z - a.z * b.y,
          a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
double Norm(const DV &a) { return std::sqrt(Dot(a, a)); }
DV Unit(const DV &a) { return a / Norm(a); }
DV ToD(const Vec3f &v) { return DV(v); }

double Clamp(double x, double lo, double hi) {
  return std::max(lo, std::min(hi, x));
}

// Independent unpolarized Fresnel power transmittance, air side cosine.
double RefFresnelT(double eta, double cos_air) {
  cos_air = Clamp(cos_air, 0.0, 1.0);
  const double cos_med =
      std::sqrt(std::max(0.0, 1.0 - (1.0 - cos_air * cos_air) /
                                       (eta * eta)));
  const double rs = (cos_air - eta * cos_med) /
                    (cos_air + eta * cos_med);
  const double rp = (eta * cos_air - cos_med) /
                    (eta * cos_air + cos_med);
  return 1.0 - 0.5 * (rs * rs + rp * rp);
}

// Forward vector Snell: incident unit vector `inc`, outward interface
// normal `n`, eta = n_incident / n_transmitted.
bool RefRefract(const DV &inc, const DV &n, double eta, DV *out) {
  const double cosi = -Dot(inc, n); // > 0 when entering against n
  const double k = 1.0 - eta * eta * (1.0 - cosi * cosi);
  if (k < 0.0)
    return false;
  const DV t = inc * eta + n * (eta * cosi - std::sqrt(k));
  *out = Unit(t);
  return true;
}

double AngleBetween(const DV &a, const DV &b) {
  return std::acos(Clamp(Dot(Unit(a), Unit(b)), -1.0, 1.0));
}

// Independent forward-Snell + sphere back-substitution check of one
// returned connection.
struct CheckReport {
  bool valid{false};
  double pos_err{0};
  double dir_err{0};
  double cos_entry{0};
  double cos_exit{0};
};
CheckReport CheckConnection(const Sphere &sph, const DV &Pw,
                            const DV &Lw, const SphereConnection &c) {
  CheckReport rep;
  const double R = sph.radius;
  const double eta = sph.material.refractive_index;
  const DV C(sph.center);
  const DV p = Pw - C, l = Lw - C;
  const DV e = ToD(c.entry) - C;
  const DV x = ToD(c.exit) - C;

  rep.pos_err = std::max(std::abs(Norm(e) - R), std::abs(Norm(x) - R));
  if (rep.pos_err > 1e-6 * R)
    return rep;

  const double s1 = Norm(e - p), s2 = Norm(x - e), s3 = Norm(l - x);
  if (!(s1 > 0 && s2 > 0 && s3 > 0))
    return rep;

  const DV u = Unit(e - p), w = Unit(x - e), v = Unit(l - x);
  const DV ni = e / R, ne = x / R;

  // first segment enters through the near sphere intersection
  const double b = Dot(u, p), cc = Dot(p, p) - R * R;
  const double disc = b * b - cc;
  if (disc <= 0)
    return rep;
  const double t_near = -b - std::sqrt(disc);
  if (std::abs(t_near - s1) > 1e-6 * R)
    return rep;
  // the chord's midpoint is strictly interior
  if (Norm((e + x) * 0.5) >= R)
    return rep;
  if (Dot(v, ne) <= 0 || Dot(w, ne) <= 0 || Dot(w, ni) >= 0)
    return rep;

  DV w_ref{}, v_ref{};
  if (!RefRefract(u, ni, 1.0 / eta, &w_ref))
    return rep;
  // exit: the outward normal points along the incident ray, so the
  // normal term carries the opposite sign to the entry formula
  const double cosi_exit = Dot(w, ne);
  const double k2 = 1.0 - eta * eta * (1.0 - cosi_exit * cosi_exit);
  if (k2 < 0)
    return rep;
  v_ref = Unit(w * eta + ne * (std::sqrt(k2) - eta * cosi_exit));

  rep.dir_err = std::max(AngleBetween(w, w_ref), AngleBetween(v, v_ref));

  // scalar Snell cross-check through the interface angles. The surface
  // points are stored as float, so the achievable residual is bounded by
  // the contracted 2e-5 rad direction budget rather than machine epsilon.
  const double ta1 = std::acos(Clamp(-Dot(u, ni), -1.0, 1.0));
  const double tm1 = std::acos(Clamp(-Dot(w, ni), -1.0, 1.0));
  const double tm2 = std::acos(Clamp(Dot(w, ne), -1.0, 1.0));
  const double ta2 = std::acos(Clamp(Dot(v, ne), -1.0, 1.0));
  if (std::abs(std::sin(ta1) - eta * std::sin(tm1)) > 2e-5 ||
      std::abs(eta * std::sin(tm2) - std::sin(ta2)) > 2e-5)
    return rep;

  // ray-sphere back-substitution of the reconstructed interior ray
  const double chord = -2.0 * Dot(w_ref, e);
  const DV x_back = e + w_ref * chord;
  rep.pos_err = std::max(rep.pos_err, Norm(x_back - x));

  rep.cos_entry = -Dot(u, ni);
  rep.cos_exit = Dot(v, ne);
  rep.valid = rep.cos_entry >= 0.1 && rep.cos_exit >= 0.1 &&
              rep.pos_err <= 2e-5 * R && rep.dir_err <= 2e-5;
  return rep;
}

// Independent reference solver: scans the whole feasible entry cone for
// sign changes of an independently built residual, then bisects.
struct RefSolver {
  double R, eta;
  DV p, l, axis, eb, en;
  double amax;
  RefSolver(double radius, double ior, const DV &P, const DV &L)
      : R(radius), eta(ior), p(P), l(L) {
    axis = p / Norm(p);
    const DV perp = l - axis * Dot(l, axis);
    eb = Unit(perp);
    en = Cross(axis, eb);
    amax = std::asin(R / Norm(p));
  }
  bool residual(double alpha, double *g) const {
    const DV u = Unit(axis * (-std::cos(alpha)) + eb * std::sin(alpha));
    const double disc = R * R -
        Norm(p) * Norm(p) * std::sin(alpha) * std::sin(alpha);
    if (disc <= 0)
      return false;
    const double s1 = Norm(p) * std::cos(alpha) - std::sqrt(disc);
    if (s1 <= 0)
      return false;
    const DV e = p + u * s1;
    const DV ni = e / R;
    DV w;
    if (!RefRefract(u, ni, 1.0 / eta, &w))
      return false;
    const double s2 = -2.0 * Dot(w, e);
    if (s2 <= 0)
      return false;
    const DV x = e + w * s2;
    const DV ne = x / R;
    const double ci = Dot(w, ne);
    const double k = 1.0 - eta * eta * (1.0 - ci * ci);
    if (k < 0)
      return false;
    // outward normal along the incident ray: flip the normal-term sign
    const DV vpred = Unit(w * eta + ne * (std::sqrt(k) - eta * ci));
    const DV v = Unit(l - x);
    *g = std::atan2(Dot(en, Cross(vpred, v)), Dot(vpred, v));
    return std::isfinite(*g);
  }
  // air-side interface cosines at a given emission angle; false if the
  // entry ray misses the sphere or an interface cannot be built.
  bool cosines(double alpha, double *cos_entry, double *cos_exit) const {
    const DV u = Unit(axis * (-std::cos(alpha)) + eb * std::sin(alpha));
    const double disc = R * R -
        Norm(p) * Norm(p) * std::sin(alpha) * std::sin(alpha);
    if (disc <= 0)
      return false;
    const double s1 = Norm(p) * std::cos(alpha) - std::sqrt(disc);
    if (s1 <= 0)
      return false;
    const DV e = p + u * s1;
    const DV ni = e / R;
    DV w;
    if (!RefRefract(u, ni, 1.0 / eta, &w))
      return false;
    const double s2 = -2.0 * Dot(w, e);
    if (s2 <= 0)
      return false;
    const DV x = e + w * s2;
    const DV ne = x / R;
    *cos_entry = -Dot(u, ni);
    *cos_exit = Dot(Unit(l - x), ne);
    return true;
  }
  std::vector<double> Roots() const {
    constexpr int N = 30000;
    const double lo = -amax * (1 - 1e-12), hi = amax * (1 - 1e-12);
    std::vector<double> out;
    double pa = lo, pf = 0.0;
    residual(pa, &pf);
    for (int i = 1; i <= N; ++i) {
      const double a = lo + (hi - lo) * i / N;
      double cf = 0.0;
      if (!residual(a, &cf)) {
        pa = a;
        continue;
      }
      if (pf * cf < 0) {
        double x0 = pa, x1 = a, f0 = pf;
        for (int it = 0; it < 80; ++it) {
          double fm = 0.0;
          const double m = 0.5 * (x0 + x1);
          if (!residual(m, &fm))
            break;
          if (f0 * fm <= 0)
            x1 = m;
          else {
            x0 = m;
            f0 = fm;
          }
        }
        const double root = 0.5 * (x0 + x1);
        // only isolated simple solutions with non-grazing (cos >= 0.1)
        // air-side angles are observable, matching the production filter
        double ce = 0.0, cx = 0.0;
        if (cosines(root, &ce, &cx) && ce >= 0.1 && cx >= 0.1)
          out.push_back(root);
      }
      pa = a;
      pf = cf;
    }
    std::sort(out.begin(), out.end());
    return out;
  }
  double AngleOf(const SphereConnection &c, const DV &Pw) const {
    const DV u = Unit(ToD(c.entry) - Pw);
    return std::atan2(Dot(u, eb), -Dot(u, axis));
  }
};

Material MakeGlass(float ior = 1.5f, float trans = 1.0f) {
  Material m;
  m.transparency = trans;
  m.refractive_index = ior;
  m.color = {200, 220, 230};
  return m;
}
Material MakeOpaque(Vec3u8 color = {120, 90, 60}, float spec = 0.0f) {
  Material m;
  m.transparency = 0.0f;
  m.refractive_index = 1.0f;
  m.color = color;
  m.specular = spec;
  return m;
}

// ---------------------------------------------------------------------
// connections group
// ---------------------------------------------------------------------
int TestCanonicalThree() {
  Sphere sph(Vec3f{0, 0, 0}, 1.0f, MakeGlass());
  const DV P(-3, 0, 0), L(2.5, 0.01, 0);
  auto conns = ConnectSphere(sph, Vec3f{-3, 0, 0}, Vec3f{2.5f, 0.01f, 0});
  TASK_CHECK(conns.size() == 3);
  RefSolver ref(1.0, 1.5, P, L);
  const auto roots = ref.Roots();
  TASK_CHECK(roots.size() == conns.size());
  double prev = -1e9;
  for (size_t i = 0; i < conns.size(); ++i) {
    CheckReport rep = CheckConnection(sph, P, L, conns[i]);
    TASK_CHECK(rep.valid);
    const double alpha = ref.AngleOf(conns[i], P);
    TASK_CHECK(std::abs(alpha - roots[i]) < 1e-6);
    TASK_CHECK(alpha > prev); // strictly increasing signed angle
    if (i + 1 < conns.size())
      TASK_CHECK(roots[i + 1] - roots[i] >= 1e-4);
    prev = alpha;
    const double expect = RefFresnelT(1.5, rep.cos_entry) *
                          RefFresnelT(1.5, rep.cos_exit);
    TASK_CHECK(std::abs(conns[i].transmission - expect) < 1e-6);
    TASK_CHECK(conns[i].transmission > 0 &&
               conns[i].transmission <= 1.0);
  }
  return 0;
}

int TestBackSubstitutionPrecision() {
  Sphere sph(Vec3f{0, 0, 0}, 1.0f, MakeGlass(1.5f, 0.7f));
  const DV P(-3, 0, 0), L(2.5, 0.01, 0);
  auto conns = ConnectSphere(sph, Vec3f{-3, 0, 0}, Vec3f{2.5f, 0.01f, 0});
  TASK_CHECK(conns.size() == 3);
  for (const auto &c : conns) {
    CheckReport rep = CheckConnection(sph, P, L, c);
    TASK_CHECK(rep.valid);
    TASK_CHECK(rep.pos_err <= 2e-5 * 1.0);
    TASK_CHECK(rep.dir_err <= 2e-5);
    const double expect = 0.49 * RefFresnelT(1.5, rep.cos_entry) *
                          RefFresnelT(1.5, rep.cos_exit);
    TASK_CHECK(std::abs(c.transmission - expect) < 1e-6);
  }
  return 0;
}

int TestReciprocity() {
  Sphere sph(Vec3f{0, 0, 0}, 1.0f, MakeGlass());
  Vec3f P{-3, 0, 0}, L{2.5f, 0.01f, 0};
  auto fwd = ConnectSphere(sph, P, L);
  auto rev = ConnectSphere(sph, L, P);
  TASK_CHECK(fwd.size() == 3 && rev.size() == 3);
  for (const auto &a : fwd) {
    double best = 1e9;
    for (const auto &b : rev) {
      const double err =
          std::max(std::max(std::abs(a.entry.x - b.exit.x),
                            std::abs(a.entry.y - b.exit.y)),
                   std::max(std::abs(a.exit.x - b.entry.x),
                            std::abs(a.exit.y - b.entry.y)));
      best = std::min(best, err);
    }
    TASK_CHECK(best < 2e-5);
  }
  return 0;
}

int TestNoSolution() {
  Sphere sph(Vec3f{0, 0, 0}, 1.0f, MakeGlass());
  const DV P(1.6, 0, 0), L(0, 3.5, 0.2);
  auto conns = ConnectSphere(sph, Vec3f{1.6f, 0, 0},
                             Vec3f{0, 3.5f, 0.2f});
  TASK_CHECK(conns.empty());
  RefSolver ref(1.0, 1.5, P, L);
  TASK_CHECK(ref.Roots().empty());
  return 0;
}

int TestTransformAndScale() {
  Sphere base(Vec3f{0, 0, 0}, 1.0f, MakeGlass(1.4f, 0.9f));
  const Vec3f Pb{-3, 0, 0}, Lb{2.5f, 0.01f, 0};
  auto cb = ConnectSphere(base, Pb, Lb);
  TASK_CHECK(cb.size() == 3);

  const double th = 0.6, s = 1.5;
  const DV t(0.3, -0.4, 0.2);
  const double ct = std::cos(th), st = std::sin(th);
  auto rot = [&](const DV &v) {
    return DV(ct * v.x - st * v.y, st * v.x + ct * v.y, v.z);
  };
  auto T = [&](const DV &v) { return rot(v) * s + t; };
  Sphere xform(Vec3f{float(t.x), float(t.y), float(t.z)}, float(s),
               MakeGlass(1.4f, 0.9f));
  const DV P2 = T(DV(Pb)), L2 = T(DV(Lb));
  TASK_CHECK(std::abs(t.x) <= s && std::abs(t.y) <= s &&
             std::abs(t.z) <= s);
  TASK_CHECK(Norm(P2 - t) / s >= 1.1 && Norm(P2 - t) / s <= 10);
  TASK_CHECK(Norm(L2 - t) / s >= 1.1 && Norm(L2 - t) / s <= 10);

  auto cf = ConnectSphere(
      xform, Vec3f{float(P2.x), float(P2.y), float(P2.z)},
      Vec3f{float(L2.x), float(L2.y), float(L2.z)});
  TASK_CHECK(cf.size() == cb.size());
  for (size_t i = 0; i < cb.size(); ++i) {
    TASK_CHECK(Norm(DV(cf[i].entry) - T(DV(cb[i].entry))) <= 2e-5 * s);
    TASK_CHECK(Norm(DV(cf[i].exit) - T(DV(cb[i].exit))) <= 2e-5 * s);
    TASK_CHECK(std::abs(cf[i].transmission - cb[i].transmission) < 1e-6);
  }
  return 0;
}

int TestCompletenessRandom() {
  struct Cfg {
    double R, eta;
    DV P, L;
  };
  const Cfg cfgs[] = {
      {1.2, 1.35, DV(-2.0, 0.7, 0.4), DV(1.8, -1.1, 0.5)},
      {0.7, 1.8, DV(1.1, 2.0, -0.6), DV(-1.5, 0.9, 0.8)},
      {2.0, 1.2, DV(-6.0, -3.0, 2.0), DV(4.0, 5.0, -1.0)},
      {1.0, 1.5, DV(-4.0, 2.5, 1.0), DV(3.0, -3.0, -1.5)},
      {1.5, 1.12, DV(2.2, -2.2, 1.0), DV(-2.6, 1.4, 2.0)},
  };
  for (const auto &cfg : cfgs) {
    TASK_CHECK(Norm(cfg.P) / cfg.R >= 1.1 &&
               Norm(cfg.P) / cfg.R <= 10);
    TASK_CHECK(Norm(cfg.L) / cfg.R >= 1.1 &&
               Norm(cfg.L) / cfg.R <= 10);
    Sphere sph(Vec3f{0, 0, 0}, float(cfg.R),
               MakeGlass(float(cfg.eta)));
    Vec3f Pf{float(cfg.P.x), float(cfg.P.y), float(cfg.P.z)};
    Vec3f Lf{float(cfg.L.x), float(cfg.L.y), float(cfg.L.z)};
    auto got = ConnectSphere(sph, Pf, Lf);
    RefSolver ref(cfg.R, cfg.eta, cfg.P, cfg.L);
    const auto want = ref.Roots();
    TASK_CHECK(got.size() == want.size());
    for (size_t i = 0; i < got.size(); ++i) {
      CheckReport rep = CheckConnection(sph, cfg.P, cfg.L, got[i]);
      TASK_CHECK(rep.valid);
      TASK_CHECK(std::abs(ref.AngleOf(got[i], cfg.P) - want[i]) < 1e-6);
    }
  }
  return 0;
}

// ---------------------------------------------------------------------
// visibility / lighting shared scene.
// The very Sphere instances placed in the object vector are the ones
// referenced by Lights::SetPointRefractor, so object identity matches
// inside the renderer and the lens is never treated as an occluder.
// ---------------------------------------------------------------------
struct BendScene {
  std::vector<std::unique_ptr<Object>> objs;
  Sphere *lens{nullptr};
  Sphere *receiver{nullptr};
  DV P, L, N;
  SphereConnection conn;
  BendScene()
      : P(-2.5, -0.5, 0.3), L(3, -2, -0.2),
        N(0.1771633543, 0.9809696228, -0.0794464919) {
    auto lp = std::make_unique<Sphere>(Vec3f{0, 0, 0}, 1.0f,
                                       MakeGlass());
    auto rp = std::make_unique<Sphere>(
        Vec3f{float(P.x), float(P.y), float(P.z)}, 0.05f,
        MakeOpaque({140, 110, 80}));
    lens = lp.get();
    receiver = rp.get();
    objs.push_back(std::move(lp));
    objs.push_back(std::move(rp));
    conn = ConnectSphere(*lens,
        Vec3f{float(P.x), float(P.y), float(P.z)},
        Vec3f{float(L.x), float(L.y), float(L.z)}).front();
  }
};

// ---------------------------------------------------------------------
// visibility group
// ---------------------------------------------------------------------
int TestBlockerEachSegment() {
  BendScene sc;
  TASK_CHECK(Dot(sc.N, Unit(ToD(sc.conn.entry) - sc.P)) > 0.5);
  TASK_CHECK(Dot(sc.N, Unit(sc.L - sc.P)) < 0); // straight ray backlit
  const DV e = ToD(sc.conn.entry), x = ToD(sc.conn.exit);
  const DV mids[3] = {(sc.P + e) * 0.5, (e + x) * 0.5,
                      (x + sc.L) * 0.5};

  Camera cam(24, 20, 20, Vec3f{float(sc.P.x), float(sc.P.y),
                               float(sc.P.z)});
  const Vec3f Nf{float(sc.N.x), float(sc.N.y), float(sc.N.z)};
  const Vec3f Pf{float(sc.P.x), float(sc.P.y), float(sc.P.z)};

  Lights base;
  base.AddPoint(1.0f, float(sc.L.x), float(sc.L.y), float(sc.L.z));
  base.SetPointRefractor(sc.lens);
  const Vec3u8 lit = base.ColorAt(sc.objs, *sc.receiver, Pf, cam, Nf);
  TASK_CHECK(lit.x > 0); // bent path lights the receiver

  // the first-segment blocker must not intersect the straight P->L line
  Ray straight(Pf, Vec3f{float(sc.L.x), float(sc.L.y), float(sc.L.z)});
  Sphere off_line(Vec3f{float(mids[0].x), float(mids[0].y),
                        float(mids[0].z)},
                  0.05f, MakeOpaque());
  TASK_CHECK(!off_line.Intersects(straight).is_hit);

  for (int seg = 0; seg < 3; ++seg) {
    BendScene s; // fresh scene; this blocker is the only extra object
    s.objs.emplace_back(std::make_unique<Sphere>(
        Vec3f{float(mids[seg].x), float(mids[seg].y),
              float(mids[seg].z)},
        0.05f, MakeOpaque({10, 10, 10})));
    Lights lights;
    lights.AddPoint(1.0f, float(sc.L.x), float(sc.L.y),
                    float(sc.L.z));
    lights.SetPointRefractor(s.lens);
    const Vec3u8 dark =
        lights.ColorAt(s.objs, *s.receiver, Pf, cam, Nf);
    TASK_CHECK(dark.x == 0 && dark.y == 0 && dark.z == 0);
  }

  BendScene clean;
  const Vec3u8 restored =
      base.ColorAt(clean.objs, *clean.receiver, Pf, cam, Nf);
  TASK_CHECK(restored == lit);
  return 0;
}

int TestBlockerBehindLight() {
  BendScene sc;
  const DV x = ToD(sc.conn.exit);
  const DV behind = sc.L + Unit(sc.L - x) * 0.5;
  Camera cam(24, 20, 20, Vec3f{float(sc.P.x), float(sc.P.y),
                               float(sc.P.z)});
  const Vec3f Nf{float(sc.N.x), float(sc.N.y), float(sc.N.z)};
  const Vec3f Pf{float(sc.P.x), float(sc.P.y), float(sc.P.z)};

  sc.objs.emplace_back(std::make_unique<Sphere>(
      Vec3f{float(behind.x), float(behind.y), float(behind.z)}, 0.05f,
      MakeOpaque()));
  Lights lights;
  lights.AddPoint(1.0f, float(sc.L.x), float(sc.L.y), float(sc.L.z));
  lights.SetPointRefractor(sc.lens);
  const Vec3u8 col = lights.ColorAt(sc.objs, *sc.receiver, Pf, cam, Nf);
  TASK_CHECK(col.x > 0); // object past the light cannot shade it
  return 0;
}

int TestDirectAndRefractedCoexist() {
  const DV P(-2.5, -0.5, 0.3), L(3, -2, -0.2);
  const Vec3f Pf{float(P.x), float(P.y), float(P.z)};
  const Vec3f Lf{float(L.x), float(L.y), float(L.z)};

  std::vector<std::unique_ptr<Object>> objs;
  auto lp = std::make_unique<Sphere>(Vec3f{0, 0, 0}, 1.0f, MakeGlass());
  auto rp = std::make_unique<Sphere>(Pf, 0.05f, MakeOpaque({140, 110, 80}));
  Sphere *lens = lp.get();
  Sphere *receiver = rp.get();
  objs.push_back(std::move(lp));
  objs.push_back(std::move(rp));

  auto conns = ConnectSphere(*lens, Pf, Lf);
  TASK_CHECK(conns.size() == 1);
  Ray straight(Pf, Lf);
  TASK_CHECK(!lens->Intersects(straight).is_hit ||
             lens->Intersects(straight).t > (Lf - Pf).Norm());

  const DV N = Unit(Unit(L - P) + Unit(ToD(conns[0].entry) - P));
  const Vec3f Nf{float(N.x), float(N.y), float(N.z)};
  Camera cam(24, 20, 20, Vec3f{float(P.x), float(P.y), float(P.z)});

  Lights with_lens;
  with_lens.AddPoint(1, float(L.x), float(L.y), float(L.z));
  with_lens.SetPointRefractor(lens);
  const Vec3u8 both = with_lens.ColorAt(objs, *receiver, Pf, cam, Nf);

  Lights no_lens;
  no_lens.AddPoint(1, float(L.x), float(L.y), float(L.z));
  const Vec3u8 direct_only =
      no_lens.ColorAt(objs, *receiver, Pf, cam, Nf);
  TASK_CHECK(both.x > direct_only.x); // refracted energy is additive

  const DV block = (P + L) * 0.5;
  objs.emplace_back(std::make_unique<Sphere>(
      Vec3f{float(block.x), float(block.y), float(block.z)}, 0.05f,
      MakeOpaque()));
  const Vec3u8 bent_only =
      with_lens.ColorAt(objs, *receiver, Pf, cam, Nf);
  TASK_CHECK(bent_only.x > 0 && bent_only.x < both.x);
  const Vec3u8 direct_killed =
      no_lens.ColorAt(objs, *receiver, Pf, cam, Nf);
  // the blocker dims the legacy straight path (its soft-shadow heuristic
  // is deliberately non-black and must remain unchanged without the lens)
  TASK_CHECK(direct_killed.x < direct_only.x);
  return 0;
}

// ---------------------------------------------------------------------
// lighting group
// ---------------------------------------------------------------------
int TestIndependentShadingReference() {
  BendScene sc;
  const DV d1 = Unit(ToD(sc.conn.entry) - sc.P);
  Camera cam(24, 20, 20, Vec3f{float(sc.P.x), float(sc.P.y),
                               float(sc.P.z)});
  const Vec3f Pf{float(sc.P.x), float(sc.P.y), float(sc.P.z)};
  const Vec3f Nf{float(sc.N.x), float(sc.N.y), float(sc.N.z)};
  Lights lights;
  lights.AddPoint(1, float(sc.L.x), float(sc.L.y), float(sc.L.z));
  lights.SetPointRefractor(sc.lens);
  const Vec3u8 got = lights.ColorAt(sc.objs, *sc.receiver, Pf, cam, Nf);

  // single visible bent path; matte receiver; straight ray back-facing
  const double ndotl = Dot(sc.N, d1);
  TASK_CHECK(ndotl > 0);
  const double diffuse = Clamp(ndotl * sc.conn.transmission, 0, 1);
  const Vec3u8 sample = sc.receiver->SampleColor(Pf, Nf);
  const uint8_t vals[3] = {sample.x, sample.y, sample.z};
  const uint8_t gots[3] = {got.x, got.y, got.z};
  for (int ch = 0; ch < 3; ++ch) {
    const auto expect = static_cast<uint8_t>(
        std::min(vals[ch] * diffuse, 255.0));
    TASK_CHECK(std::abs(int(gots[ch]) - int(expect)) <= 1);
  }
  return 0;
}

int TestSpecularDirection() {
  BendScene sc;
  const DV d1 = Unit(ToD(sc.conn.entry) - sc.P);
  const DV refl = Unit(d1 * (-1.0) + sc.N * (2.0 * Dot(d1, sc.N)));
  const DV cam_pos = sc.P + refl * 2.0;

  // rebuild the scene with a shiny receiver at the same point
  std::vector<std::unique_ptr<Object>> objs;
  auto lp = std::make_unique<Sphere>(Vec3f{0, 0, 0}, 1.0f, MakeGlass());
  Sphere *lens = lp.get();
  const Vec3f Pf{float(sc.P.x), float(sc.P.y), float(sc.P.z)};
  auto sp = std::make_unique<Sphere>(Pf, 0.05f,
                                      MakeOpaque({140, 110, 80}, 32.0f));
  Sphere *shiny = sp.get();
  objs.push_back(std::move(lp));
  objs.push_back(std::move(sp));
  const Vec3f Nf{float(sc.N.x), float(sc.N.y), float(sc.N.z)};

  Camera spec_cam(24, 20, 20,
                  Vec3f{float(cam_pos.x), float(cam_pos.y),
                        float(cam_pos.z)});
  Lights lights;
  lights.AddPoint(1, float(sc.L.x), float(sc.L.y), float(sc.L.z));
  lights.SetPointRefractor(lens);
  const Vec3u8 peak = lights.ColorAt(objs, *shiny, Pf, spec_cam, Nf);

  Camera off_cam(24, 20, 20,
                 Vec3f{float(sc.P.x), float(sc.P.y),
                       float(sc.P.z + 2.0f)});
  const Vec3u8 weak = lights.ColorAt(objs, *shiny, Pf, off_cam, Nf);
  TASK_CHECK(peak.x >= weak.x);

  // independent specular reference at the aligned camera (dot ~ 1)
  const DV view = Unit(cam_pos - sc.P);
  const double lobe = std::pow(std::max(Dot(refl, view), 0.0),
                               shiny->material.specular);
  const double spec = lobe * sc.conn.transmission;
  const double diffuse =
      Clamp(Dot(sc.N, d1) * sc.conn.transmission, 0, 1);
  const Vec3u8 sample = shiny->SampleColor(Pf, Nf);
  const auto expect = static_cast<uint8_t>(std::min(
      sample.x * diffuse + 255.0 * spec, 255.0));
  TASK_CHECK(std::abs(int(peak.x) - int(expect)) <= 1);
  return 0;
}

int TestTwoLightsInstances() {
  BendScene sc;
  Camera cam(24, 20, 20, Vec3f{float(sc.P.x), float(sc.P.y),
                               float(sc.P.z)});
  const Vec3f Pf{float(sc.P.x), float(sc.P.y), float(sc.P.z)};
  const Vec3f Nf{float(sc.N.x), float(sc.N.y), float(sc.N.z)};

  Lights a, b;
  a.AddPoint(1, float(sc.L.x), float(sc.L.y), float(sc.L.z));
  b.AddPoint(1, float(sc.L.x), float(sc.L.y), float(sc.L.z));
  a.SetPointRefractor(sc.lens);
  b.SetPointRefractor(sc.lens);
  const Vec3u8 ca = a.ColorAt(sc.objs, *sc.receiver, Pf, cam, Nf);
  const Vec3u8 cb = b.ColorAt(sc.objs, *sc.receiver, Pf, cam, Nf);
  TASK_CHECK(ca == cb);

  Lights off;
  off.AddPoint(1, float(sc.L.x), float(sc.L.y), float(sc.L.z));
  const Vec3u8 co = off.ColorAt(sc.objs, *sc.receiver, Pf, cam, Nf);
  TASK_CHECK(!(co == ca)); // enabling the lens changes the result
  return 0;
}

int TestReplaceAndDisable() {
  BendScene sc;
  Camera cam(24, 20, 20, Vec3f{float(sc.P.x), float(sc.P.y),
                               float(sc.P.z)});
  const Vec3f Pf{float(sc.P.x), float(sc.P.y), float(sc.P.z)};
  const Vec3f Nf{float(sc.N.x), float(sc.N.y), float(sc.N.z)};

  // a second lens; kept out of the object vector so it cannot occlude the
  // first lens' paths, but referenced directly like a scene-owned object
  Sphere lens_b(Vec3f{0.3f, 0.2f, 0.0f}, 0.8f, MakeGlass(1.7f));

  Lights lights;
  lights.AddPoint(1, float(sc.L.x), float(sc.L.y), float(sc.L.z));
  lights.SetPointRefractor(sc.lens);
  const Vec3u8 col_a =
      lights.ColorAt(sc.objs, *sc.receiver, Pf, cam, Nf);

  lights.SetPointRefractor(&lens_b); // swap between two Trace calls
  const Vec3u8 col_b =
      lights.ColorAt(sc.objs, *sc.receiver, Pf, cam, Nf);
  TASK_CHECK(!(col_a == col_b));

  lights.SetPointRefractor(nullptr); // disable: legacy straight path
  const Vec3u8 col_off =
      lights.ColorAt(sc.objs, *sc.receiver, Pf, cam, Nf);
  TASK_CHECK(Dot(sc.N, Unit(sc.L - sc.P)) < 0);
  TASK_CHECK(col_off.x == 0); // straight ray is back-facing

  lights.SetPointRefractor(sc.lens); // re-enable restores the result
  const Vec3u8 col_restore =
      lights.ColorAt(sc.objs, *sc.receiver, Pf, cam, Nf);
  TASK_CHECK(col_restore == col_a);
  return 0;
}

int TestRealTraceRendersPpm() {
  // drive the actual RayTracer::TraceRay -> ColorAt connection path.
  // Camera sits on the -x side looking towards +x; a -90 deg Y rotation
  // maps the local +z view axis onto world +x in this code base.
  const Mat3x3 look_plus_x(0.0f,
      static_cast<float>(-M_PI) / 2.0f, 0.0f);
  Camera camera(24, 20, 20, Vec3f{-7, 0, 0}, look_plus_x);
  Lights lights;
  lights.AddPoint(1, 2.5f, 0.01f, 0);
  RayTracer tracer(camera, lights);
  tracer.SetBackground(0, 0, 0);
  Sphere &lens = tracer.AddObject<Sphere>(Vec3f{0, 0, 0}, 1.0f,
                                          MakeGlass());
  lights.SetPointRefractor(&lens);
  // receiver quad in the x = -3 plane wound with normal +x
  tracer.AddObject<Quad>(Vec3f{-3, -2, -2}, Vec3f{-3, 2, -2},
                         Vec3f{-3, 2, 2}, Vec3f{-3, -2, 2},
                         MakeOpaque({150, 120, 90}));
  tracer.Trace(0);
  const std::string path = ".task-build/lens.ppm";
  Ppm::Write(tracer.image(), path);
  FILE *f = std::fopen(path.c_str(), "rb");
  TASK_CHECK(f != nullptr);
  if (f) {
    std::fseek(f, 0, SEEK_END);
    const long bytes = std::ftell(f);
    std::fclose(f);
    TASK_CHECK(bytes > 0);
  }
  unsigned foreground = 0;
  for (const auto &px : tracer.image().data)
    if (!(px == Vec3u8{0, 0, 0}))
      ++foreground;
  TASK_CHECK(foreground > 0); // bent light reaches the receiver
  return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"connections", "canonical_three_roots", TestCanonicalThree},
    {"connections", "back_substitution_precision",
     TestBackSubstitutionPrecision},
    {"connections", "p_l_reverse_reciprocity", TestReciprocity},
    {"connections", "no_solution_empty", TestNoSolution},
    {"connections", "rigid_transform_uniform_scale",
     TestTransformAndScale},
    {"connections", "completeness_matches_reference",
     TestCompletenessRandom},
    {"visibility", "blocker_on_each_segment", TestBlockerEachSegment},
    {"visibility", "blocker_behind_light", TestBlockerBehindLight},
    {"visibility", "direct_and_refracted_coexist",
     TestDirectAndRefractedCoexist},
    {"lighting", "independent_shading_reference",
     TestIndependentShadingReference},
    {"lighting", "specular_uses_first_segment", TestSpecularDirection},
    {"lighting", "two_lights_instances", TestTwoLightsInstances},
    {"lighting", "replace_and_disable_refractor", TestReplaceAndDisable},
    {"lighting", "real_trace_writes_lens_ppm", TestRealTraceRendersPpm},
};
const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
