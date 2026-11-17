// Regression tests for the optional single-sphere lens connection.
// Registered through task_support/check.h; this file defines no main().
#include "task_support/check.h"
#include "sphere_connection.hpp"
#include "light.hpp"
#include "ray_tracer.hpp"
#include "ppm_writer.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

// ----------------------------- double helpers -----------------------------
struct D3 {
  double x, y, z;
};
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 Cross(D3 a, D3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
double Norm(D3 a) { return std::sqrt(Dot(a, a)); }
D3 Unit(D3 a) { return a * (1.0 / Norm(a)); }
D3 D(Vec3f v) { return {v.x, v.y, v.z}; }

// Independent vector Snell law. n is the normal pointing against the
// incident ray; eta12 = n1/n2. Returns the refracted unit direction and
// the two incidence cosines.
D3 RefractIndependent(D3 inc, D3 n, double eta12, double& cos_i,
                      double& cos_t) {
  cos_i = -Dot(n, inc);
  double sin2_i = std::max(0.0, 1.0 - cos_i * cos_i);
  double sin2_t = eta12 * eta12 * sin2_i;
  cos_t = std::sqrt(std::max(0.0, 1.0 - sin2_t));
  return Unit(inc * eta12 + n * (eta12 * cos_i - cos_t));
}

double FresnelT(double n1, double n2, double ci, double ct) {
  double rs = (n1 * ci - n2 * ct) / (n1 * ci + n2 * ct);
  double rp = (n2 * ci - n1 * ct) / (n2 * ci + n1 * ct);
  return 1.0 - 0.5 * (rs * rs + rp * rp);
}

// ---------------- independent 2-D forward Snell solver ----------------
// Unit sphere at the origin, P = (-p, 0), L = (lx, ly). This is a
// separate implementation from the production solver: it marches the
// emission angle over the whole feasible domain and bisects every sign
// change of the direct forward-construction residual, so it acts as an
// oracle for solution completeness.
struct PathInfo {
  double alpha, ex, ey, xx, xy, te, tx, tL, ci;
};

bool ForwardPath(double alpha, double p, double lx, double ly, double eta,
                 PathInfo& out) {
  double sa = std::sin(alpha), ca = std::cos(alpha);
  double b = p * sa;
  if (std::abs(b) >= 1.0) return false;
  double ci = std::sqrt(1.0 - b * b);
  double sg = b / eta;
  if (std::abs(sg) >= 1.0) return false;
  double ct = std::sqrt(1.0 - sg * sg);
  double te = p * ca - ci;
  double ex = -p + te * ca, ey = te * sa;
  double ri = 1.0 / eta;
  double k = ri * ci - ct;
  D3 d2 = Unit({ri * ca + k * ex, ri * sa + k * ey, 0.0});
  double tx = -2.0 * (ex * d2.x + ey * d2.y);
  double xx = ex + tx * d2.x, xy = ey + tx * d2.y;
  double k3 = eta * ct - ci;
  D3 d3 = Unit({eta * d2.x - k3 * xx, eta * d2.y - k3 * xy, 0.0});
  double rx = lx - xx, ry = ly - xy;
  double tL = rx * d3.x + ry * d3.y;
  out = {alpha, ex, ey, xx, xy, te, tx, tL, ci};
  return true;
}

double ForwardResidual(double alpha, double p, double lx, double ly,
                       double eta) {
  double sa = std::sin(alpha), ca = std::cos(alpha);
  double b = p * sa;
  if (std::abs(b) >= 1.0) return 0.0;
  double ci = std::sqrt(1.0 - b * b);
  double ct = std::sqrt(std::max(0.0, 1.0 - (b / eta) * (b / eta)));
  double te = p * ca - ci;
  double ex = -p + te * ca, ey = te * sa;
  double ri = 1.0 / eta;
  double k = ri * ci - ct;
  D3 d2 = Unit({ri * ca + k * ex, ri * sa + k * ey, 0.0});
  double tx = -2.0 * (ex * d2.x + ey * d2.y);
  double xx = ex + tx * d2.x, xy = ey + tx * d2.y;
  double k3 = eta * ct - ci;
  D3 d3 = Unit({eta * d2.x - k3 * xx, eta * d2.y - k3 * xy, 0.0});
  return (lx - xx) * d3.y - (ly - xy) * d3.x;
}

std::vector<PathInfo> IndependentSolve(double p, double lx, double ly,
                                       double eta) {
  // ci = sqrt(1-b^2) >= 0.1  <=>  |b| <= sqrt(0.99); use the matching
  // emission-angle bound so the oracle covers the same feasible domain
  const double amax = std::asin(std::sqrt(0.99) / p);
  const int N = 120000;
  std::vector<PathInfo> roots;
  double prev = ForwardResidual(-amax, p, lx, ly, eta);
  double pa = -amax;
  for (int i = 1; i <= N; ++i) {
    double a = -amax + 2.0 * amax * i / N;
    double cur = ForwardResidual(a, p, lx, ly, eta);
    if (prev * cur < 0.0) {
      double lo = pa, hi = a, flo = prev;
      for (int it = 0; it < 80; ++it) {
        double m = 0.5 * (lo + hi);
        double fm = ForwardResidual(m, p, lx, ly, eta);
        if (flo * fm <= 0.0) hi = m;
        else { lo = m; flo = fm; }
      }
      double root = 0.5 * (lo + hi);
      PathInfo info;
      if (ForwardPath(root, p, lx, ly, eta, info)) roots.push_back(info);
    }
    prev = cur;
    pa = a;
  }
  return roots;
}

Material GlassMat(double transparency = 1.0, float ior = 1.5f) {
  Material m;
  m.color = {200, 220, 230};
  m.specular = 0.0f;
  m.transparency = static_cast<float>(transparency);
  m.refractive_index = ior;
  return m;
}

Material OpaqueMat(uint8_t v = 128) {
  Material m;
  m.color = {v, v, v};
  m.specular = 0.0f;
  m.transparency = 0.0f;
  return m;
}

// Opaque quad receiver centered at `center`, normal `normal` (unit).
std::unique_ptr<Quad> MakeReceiver(Vec3f center, Vec3f normal,
                                   const Material& mat, float half = 0.5f) {
  // build two in-plane tangents
  Vec3f ref = std::abs(normal.x) < 0.9f ? Vec3f{1, 0, 0} : Vec3f{0, 1, 0};
  Vec3f t1 = normal.Cross(ref).Unit();
  Vec3f t2 = normal.Cross(t1).Unit();
  Vec3f a = center - t1 * half - t2 * half;
  Vec3f b = center + t1 * half - t2 * half;
  Vec3f c = center + t1 * half + t2 * half;
  Vec3f d = center - t1 * half + t2 * half;
  return std::make_unique<Quad>(a, b, c, d, mat);
}

constexpr double POS_TOL = 2e-5;
constexpr double DIR_TOL = 2e-5;

// ============================ connections group ============================
int CanonThreeRoots() {
  Sphere sphere(Vec3f{0, 0, 0}, 1.0f, GlassMat());
  auto conns = ConnectSphere(sphere, Vec3f{-3, 0, 0},
                             Vec3f{2.5f, 0.01f, 0});
  TASK_CHECK(conns.size() == 3);
  // sorted by signed emission angle: entry y-coordinates follow the
  // same ordering as the angle in this canonical frame
  for (size_t i = 1; i < conns.size(); ++i) {
    double a_prev = std::atan2((double)conns[i - 1].entry.y,
                               (double)conns[i - 1].entry.x + 3.0);
    double a_cur = std::atan2((double)conns[i].entry.y,
                              (double)conns[i].entry.x + 3.0);
    TASK_CHECK(a_cur > a_prev);
  }
  // distinct solutions are well separated
  for (size_t i = 1; i < conns.size(); ++i) {
    TASK_CHECK(Norm(D(conns[i].entry) - D(conns[i - 1].entry)) > 1e-4);
  }
  return 0;
}

int ForwardSnellAndSphereBackSub() {
  const double eta = 1.5;
  Sphere sphere(Vec3f{0, 0, 0}, 1.0f, GlassMat(0.7));
  Vec3f Pf{-3, 0, 0}, Lf{2.5f, 0.01f, 0};
  auto conns = ConnectSphere(sphere, Pf, Lf);
  TASK_CHECK(conns.size() == 3);
  D3 C{0, 0, 0}, P = D(Pf), L = D(Lf);
  for (const auto& c : conns) {
    D3 E = D(c.entry), X = D(c.exit);
    // surface points lie on the sphere within position tolerance
    TASK_CHECK(std::abs(Norm(E - C) - 1.0) < POS_TOL);
    TASK_CHECK(std::abs(Norm(X - C) - 1.0) < POS_TOL);
    D3 d1 = Unit(E - P), d2 = Unit(X - E), d3 = Unit(L - X);
    D3 ne = Unit(E - C), nx = Unit(X - C);
    // strictly positive segment lengths
    TASK_CHECK(Norm(E - P) > 1e-9 && Norm(X - E) > 1e-9
               && Norm(L - X) > 1e-9);
    // entry is a first entry, internal chord stays inside, exit goes out
    TASK_CHECK(Dot(d1, ne) < 0.0);
    TASK_CHECK(Norm((E + X) * 0.5 - C) < 1.0);
    TASK_CHECK(Dot(d3, nx) > 0.0);
    // air-side incidence cosines >= 0.1
    double ci_entry = -Dot(d1, ne), ci_exit_air = Dot(d3, nx);
    TASK_CHECK(ci_entry >= 0.1 - 1e-12);
    TASK_CHECK(ci_exit_air >= 0.1 - 1e-12);
    // independent forward Snell at both interfaces, direction tolerance
    double ci1, ct1, ci2, ct2;
    D3 r1 = RefractIndependent(d1, ne, 1.0 / eta, ci1, ct1);
    TASK_CHECK(Norm(r1 - d2) < DIR_TOL);
    D3 r2 = RefractIndependent(d2, nx * -1.0, eta, ci2, ct2);
    TASK_CHECK(Norm(r2 - d3) < DIR_TOL);
    // transmission = transparency^2 * exact Fresnel of both interfaces
    double expected = 0.7 * 0.7
                      * FresnelT(1.0, eta, ci1, ct1)
                      * FresnelT(eta, 1.0, ci2, ct2);
    TASK_CHECK(std::abs(expected - c.transmission) < 1e-6);
    TASK_CHECK(c.transmission > 0.0 && c.transmission <= 1.0);
  }
  return 0;
}

int CompletenessRandom() {
  std::mt19937_64 rng(2026);
  std::uniform_real_distribution<double> ur(1.1, 10.0);
  std::uniform_real_distribution<double> ue(1.05, 2.0);
  std::normal_distribution<double> nd(0.0, 1.0);
  for (int trial = 0; trial < 400; ++trial) {
    double p = ur(rng), l = ur(rng), eta = ue(rng);
    D3 dir{nd(rng), nd(rng), nd(rng)};
    dir = Unit(dir);
    if (std::hypot(dir.y, dir.z) < 2e-4) dir.y = 2e-4;
    dir = Unit(dir);
    D3 Lv = dir * l;
    // build the same reduced frame as the production solver:
    // e0 points P -> center (+x here), e1 is the perpendicular part of L
    D3 e0{1, 0, 0};
    D3 perp = Lv - e0 * Dot(Lv, e0);
    D3 e1 = Unit(perp);
    double lx = Dot(Lv, e0), ly = Dot(Lv, e1);
    // independent oracle on the reduced 2-D geometry
    auto oracle = IndependentSolve(p, lx, ly, eta);
    std::vector<PathInfo> valid;
    for (const auto& q : oracle) {
      if (q.te > 1e-9 && q.tx > 1e-9 && q.tL > 1e-9 && q.ci >= 0.1)
        valid.push_back(q);
    }
    Material m = GlassMat(1.0, static_cast<float>(eta));
    Sphere sphere(Vec3f{0, 0, 0}, 1.0f, m);
    Vec3f Pf{static_cast<float>(-p), 0, 0};
    Vec3f Lf{static_cast<float>(Lv.x), static_cast<float>(Lv.y),
             static_cast<float>(Lv.z)};
    auto conns = ConnectSphere(sphere, Pf, Lf);
    TASK_CHECK(conns.size() == valid.size());
    // lift every oracle 2-D point into world coordinates and match
    auto lift = [&](double x2, double y2) {
      return e0 * x2 + e1 * y2;
    };
    for (const auto& c : conns) {
      bool matched = false;
      for (const auto& q : valid) {
        double eerr = Norm(D(c.entry) - lift(q.ex, q.ey));
        double xerr = Norm(D(c.exit) - lift(q.xx, q.xy));
        if (eerr < POS_TOL && xerr < POS_TOL) matched = true;
      }
      TASK_CHECK(matched);
    }
  }
  return 0;
}

int Reciprocity() {
  Sphere sphere(Vec3f{0, 0, 0}, 1.0f, GlassMat());
  Vec3f P{-3, 0, 0}, L{2.5f, 0.01f, 0};
  auto forward = ConnectSphere(sphere, P, L);
  auto reverse = ConnectSphere(sphere, L, P);
  TASK_CHECK(forward.size() == 3);
  TASK_CHECK(reverse.size() == forward.size());
  for (const auto& c : forward) {
    bool swapped = false;
    for (const auto& r : reverse) {
      if (Norm(D(r.entry) - D(c.exit)) < POS_TOL
          && Norm(D(r.exit) - D(c.entry)) < POS_TOL) {
        swapped = true;
        TASK_CHECK(std::abs(r.transmission - c.transmission) < 1e-6);
      }
    }
    TASK_CHECK(swapped);
  }
  return 0;
}

int NoSolution() {
  Sphere sphere(Vec3f{0, 0, 0}, 1.0f, GlassMat());
  // light on the receiver side, behind the sphere: no forward path
  TASK_CHECK(ConnectSphere(sphere, Vec3f{-3, 0, 0},
                           Vec3f{-2.0f, 0.5f, 0}).empty());
  TASK_CHECK(ConnectSphere(sphere, Vec3f{-3, 0, 0},
                           Vec3f{-1.5f, 0.02f, 0}).empty());
  return 0;
}

int RigidTransformAndScale() {
  std::mt19937_64 rng(11);
  std::normal_distribution<double> nd(0.0, 1.0);
  std::uniform_real_distribution<double> us(0.5, 3.0);
  Material base = GlassMat(0.83);
  Sphere ref(Vec3f{0, 0, 0}, 1.0f, base);
  Vec3f P0{-3, 0, 0}, L0{2.5f, 0.01f, 0};
  auto refConns = ConnectSphere(ref, P0, L0);
  TASK_CHECK(refConns.size() == 3);
  for (int trial = 0; trial < 100; ++trial) {
    D3 a = Unit({nd(rng), nd(rng), nd(rng)});
    D3 v{nd(rng), nd(rng), nd(rng)};
    D3 b = v - a * Dot(v, a);
    if (Norm(b) < 1e-6) b = D3{1, 0, 0} - a * a.x;
    b = Unit(b);
    D3 ccol = Cross(a, b);
    auto rotate = [&](D3 q) {
      return a * Dot(q, a) + b * Dot(q, b) + ccol * Dot(q, ccol);
    };
    double scale = us(rng);
    // keep |center component| <= R as required by the input contract
    D3 tr{nd(rng), nd(rng), nd(rng)};
    tr = tr * (0.9 * scale / Norm(tr));
    auto xform = [&](D3 q) { return rotate(q) * scale + tr; };
    D3 Cw = xform({0, 0, 0});
    Sphere moved(Vec3f{(float)Cw.x, (float)Cw.y, (float)Cw.z},
                 (float)scale, base);
    D3 Pw = xform(D(P0)), Lw = xform(D(L0));
    Vec3f Pf{(float)Pw.x, (float)Pw.y, (float)Pw.z};
    Vec3f Lf{(float)Lw.x, (float)Lw.y, (float)Lw.z};
    // transformed endpoints still respect the distance contract
    TASK_CHECK(Norm(Pw - Cw) >= 1.1 * scale - 1e-9);
    TASK_CHECK(Norm(Lw - Cw) >= 1.1 * scale - 1e-9);
    auto got = ConnectSphere(moved, Pf, Lf);
    TASK_CHECK(got.size() == refConns.size());
    for (size_t i = 0; i < got.size() && i < refConns.size(); ++i) {
      D3 eExp = xform(D(refConns[i].entry));
      D3 xExp = xform(D(refConns[i].exit));
      TASK_CHECK(Norm(D(got[i].entry) - eExp) < POS_TOL * scale);
      TASK_CHECK(Norm(D(got[i].exit) - xExp) < POS_TOL * scale);
      TASK_CHECK(std::abs(got[i].transmission
                          - refConns[i].transmission) < 1e-6);
    }
  }
  return 0;
}

} // namespace

// ============================ visibility group ============================
namespace {

// Canonical scene: unit lens at origin, P=(-3,0,0), L=(2.5,0.01,0).
struct VisScene {
  std::vector<std::unique_ptr<Object>> objects;
  Sphere* lens;
  Quad* receiver;
  Vec3f P{-3, 0, 0};
  Vec3f L{2.5f, 0.01f, 0};
  static constexpr float R = 1.0f;

  VisScene() {
    auto lensPtr = std::make_unique<Sphere>(Vec3f{0, 0, 0}, R, GlassMat());
    lens = lensPtr.get();
    objects.push_back(std::move(lensPtr));
    auto quad = MakeReceiver(P, Vec3f{1, 0, 0}, OpaqueMat(128));
    receiver = quad.get();
    objects.push_back(std::move(quad));
  }
  Sphere* AddBlocker(Vec3f center, float radius = 0.05f) {
    auto blk = std::make_unique<Sphere>(center, radius, OpaqueMat(20));
    Sphere* raw = blk.get();
    objects.push_back(std::move(blk));
    return raw;
  }
  uint8_t Shade(const Sphere* refractor) {
    Lights lights;
    lights.AddPoint(1.0f, L.x, L.y, L.z);
    lights.SetPointRefractor(refractor);
    return lights.ColorAt(objects, *receiver, P,
                          Camera(10, 30, 30, {0, 0, -4}),
                          receiver->normal).x;
  }
};

// Single-connection geometry (found numerically): the straight P->L line
// crosses the lens and exactly one air->sphere->air path survives, so
// blocking any segment changes the shade unambiguously. The midpoint of
// every bent segment also misses the original straight line.
constexpr float VIS_ETA = 1.474394f;
const Vec3f VIS_P{-5.530618f, 0, 0};
const Vec3f VIS_L{8.658605f, 1.598092f, 0};

struct SingleVisScene {
  std::vector<std::unique_ptr<Object>> objects;
  Sphere* lens;
  Quad* receiver;

  SingleVisScene() {
    auto lp = std::make_unique<Sphere>(Vec3f{0, 0, 0}, 1.0f,
                                       GlassMat(1.0, VIS_ETA));
    lens = lp.get();
    objects.push_back(std::move(lp));
    auto q = MakeReceiver(VIS_P, Vec3f{1, 0, 0}, OpaqueMat(128));
    receiver = q.get();
    objects.push_back(std::move(q));
  }
  void AddBlocker(Vec3f center, float radius = 0.05f) {
    objects.push_back(
        std::make_unique<Sphere>(center, radius, OpaqueMat(20)));
  }
  uint8_t Shade() {
    Lights lights;
    lights.AddPoint(1.0f, VIS_L.x, VIS_L.y, VIS_L.z);
    lights.SetPointRefractor(lens);
    return lights.ColorAt(objects, *receiver, VIS_P,
                          Camera(10, 30, 30, {0, 0, -4}),
                          receiver->normal).x;
  }
};

int EachSegmentBlocked() {
  SingleVisScene base;
  auto conns = ConnectSphere(*base.lens, VIS_P, VIS_L);
  TASK_CHECK(conns.size() == 1);
  const uint8_t full = base.Shade();
  TASK_CHECK(full > 0);
  Vec3f ends[3][2] = {{VIS_P, conns[0].entry},
                      {conns[0].entry, conns[0].exit},
                      {conns[0].exit, VIS_L}};
  for (int s = 0; s < 3; ++s) {
    SingleVisScene sc;
    Vec3f mid = (ends[s][0] + ends[s][1]) * 0.5f;
    // blocker genuinely intersects the open bent segment, away from
    // both endpoints by at least 1e-2 * R
    Ray segRay(ends[s][0], ends[s][1]);
    float segLen = (ends[s][1] - ends[s][0]).Norm();
    Sphere probe(mid, 0.05f, OpaqueMat(20));
    HitRecord h = probe.Intersects(segRay);
    TASK_CHECK(h.is_hit && h.t > 1e-2f && h.t < segLen - 1e-2f);
    sc.AddBlocker(mid);
    TASK_CHECK(sc.Shade() < full);
  }
  return 0;
}

int BlockerMissesOriginalLine() {
  SingleVisScene sc;
  auto conns = ConnectSphere(*sc.lens, VIS_P, VIS_L);
  TASK_CHECK(conns.size() == 1);
  Ray straight(VIS_P, VIS_L);
  float straightLen = (VIS_L - VIS_P).Norm();
  const uint8_t full = sc.Shade();
  Vec3f ends[3][2] = {{VIS_P, conns[0].entry},
                      {conns[0].entry, conns[0].exit},
                      {conns[0].exit, VIS_L}};
  int bentOnly = 0;
  for (int s = 0; s < 3; ++s) {
    Vec3f mid = (ends[s][0] + ends[s][1]) * 0.5f;
    Sphere probe(mid, 0.05f, OpaqueMat(20));
    HitRecord hs = probe.Intersects(straight);
    bool hitsLine = hs.is_hit && hs.t > 0.0f && hs.t < straightLen;
    Ray segRay(ends[s][0], ends[s][1]);
    float segLen = (ends[s][1] - ends[s][0]).Norm();
    HitRecord hb = probe.Intersects(segRay);
    bool hitsBent = hb.is_hit && hb.t > 0.0f && hb.t < segLen;
    TASK_CHECK(hitsBent);
    if (!hitsLine) {
      ++bentOnly;
      SingleVisScene blocked;
      blocked.AddBlocker(mid);
      TASK_CHECK(blocked.Shade() < full);
    }
  }
  // at least one occluder blocks the bent path without meeting the
  // original straight P->L line
  TASK_CHECK(bentOnly >= 1);
  return 0;
}

int BehindLightDoesNotOcclude() {
  VisScene sc;
  auto conns = ConnectSphere(*sc.lens, sc.P, sc.L);
  const uint8_t full = sc.Shade(sc.lens);
  for (const auto& c : conns) {
    Vec3f d3 = (sc.L - c.exit).Unit();
    Vec3f behind = sc.L + d3 * 0.6f;
    // geometry: the blocker does not meet the open exit->L segment
    Ray last(c.exit, sc.L);
    float len = (sc.L - c.exit).Norm();
    Sphere probe(behind, 0.05f, OpaqueMat(20));
    HitRecord h = probe.Intersects(last);
    TASK_CHECK(!(h.is_hit && h.t > 0.0f && h.t < len));
    VisScene blocked;
    blocked.AddBlocker(behind);
    TASK_CHECK(blocked.Shade(blocked.lens) == full);
  }
  return 0;
}

int StraightAndBentCoexist() {
  // geometry found numerically: straight P->L misses the sphere while
  // a bent connection still exists
  const float eta = 1.46419f;
  auto lensPtr = std::make_unique<Sphere>(Vec3f{0, 0, 0}, 1.0f,
                                          GlassMat(1.0, eta));
  std::vector<std::unique_ptr<Object>> objs;
  Sphere* lens = lensPtr.get();
  objs.push_back(std::move(lensPtr));
  Vec3f P{-3.63962f, 0, 0}, L{5.96709f, -2.83101f, 0};
  auto quad = MakeReceiver(P, Vec3f{1, 0, 0}, OpaqueMat(128));
  Quad* receiver = quad.get();
  objs.push_back(std::move(quad));
  auto conns = ConnectSphere(*lens, P, L);
  TASK_CHECK(conns.size() >= 1);
  Vec3f d = (L - P).Unit();
  float proj = (-P).Dot(d);
  float lineDist = (P + d * proj).Norm();
  TASK_CHECK(lineDist > 1.0f); // straight segment stays outside
  Lights off, on;
  off.AddPoint(1.0f, L.x, L.y, L.z);
  on.AddPoint(1.0f, L.x, L.y, L.z);
  on.SetPointRefractor(lens);
  Camera cam(10, 30, 30, {0, 0, -4});
  uint8_t cOff = off.ColorAt(objs, *receiver, P, cam, receiver->normal).x;
  uint8_t cOn = on.ColorAt(objs, *receiver, P, cam, receiver->normal).x;
  TASK_CHECK(cOff > 0);       // straight connection alone contributes
  TASK_CHECK(cOn >= cOff);    // bent connections add energy
  return 0;
}

int ReceiverIgnoreSemantics() {
  // Single-connection geometry (found numerically): the straight line
  // crosses the lens, so exactly one bent path carries light. This makes
  // per-segment blocking deterministic.
  const float eta = 1.474394f;
  Vec3f P{-5.530618f, 0, 0}, L{8.658605f, 1.598092f, 0};
  auto build = [&](std::vector<std::unique_ptr<Object>>& objs,
                   Sphere*& lens, Quad*& receiver) {
    auto lp = std::make_unique<Sphere>(Vec3f{0, 0, 0}, 1.0f,
                                       GlassMat(1.0, eta));
    lens = lp.get();
    objs.push_back(std::move(lp));
    auto q = MakeReceiver(P, Vec3f{1, 0, 0}, OpaqueMat(128));
    receiver = q.get();
    objs.push_back(std::move(q));
  };
  auto shade = [&](std::vector<std::unique_ptr<Object>>& objs,
                   Sphere* lens, Quad* receiver) {
    Lights l;
    l.AddPoint(1.0f, L.x, L.y, L.z);
    l.SetPointRefractor(lens);
    return l.ColorAt(objs, *receiver, P, Camera(10, 30, 30, {0, 0, -4}),
                     receiver->normal).x;
  };
  std::vector<std::unique_ptr<Object>> baseObjs;
  Sphere* baseLens; Quad* baseRcv;
  build(baseObjs, baseLens, baseRcv);
  auto conns = ConnectSphere(*baseLens, P, L);
  TASK_CHECK(conns.size() == 1);
  const SphereConnection& c = conns[0];
  uint8_t full = shade(baseObjs, baseLens, baseRcv);
  TASK_CHECK(full > 0); // receiver ignores itself on the first segment

  // an opaque object on the first segment (not the receiver) blocks it
  {
    std::vector<std::unique_ptr<Object>> objs;
    Sphere* lens; Quad* rcv;
    build(objs, lens, rcv);
    Vec3f mid = (P + c.entry) * 0.5f;
    objs.push_back(std::make_unique<Sphere>(mid, 0.05f, OpaqueMat(20)));
    TASK_CHECK(shade(objs, lens, rcv) < full);
  }
  // the receiver still occludes a LATER segment: put an object there and
  // confirm segments two and three are each tested independently
  {
    std::vector<std::unique_ptr<Object>> objs;
    Sphere* lens; Quad* rcv;
    build(objs, lens, rcv);
    Vec3f mid = (c.entry + c.exit) * 0.5f;
    objs.push_back(std::make_unique<Sphere>(mid, 0.05f, OpaqueMat(20)));
    TASK_CHECK(shade(objs, lens, rcv) < full);
  }
  {
    std::vector<std::unique_ptr<Object>> objs;
    Sphere* lens; Quad* rcv;
    build(objs, lens, rcv);
    Vec3f mid = (c.exit + L) * 0.5f;
    objs.push_back(std::make_unique<Sphere>(mid, 0.05f, OpaqueMat(20)));
    TASK_CHECK(shade(objs, lens, rcv) < full);
  }
  return 0;
}

} // namespace

// ============================= lighting group =============================
namespace {

// Independent re-implementation of the local Phong accumulation used by
// Lights::ColorAt for a set of (direction, weight) light paths.
struct PathRef {
  Vec3f dir;
  float weight;
};
uint8_t ReferenceShade(const Material& mat, Vec3f N, Vec3f view,
                       const std::vector<PathRef>& paths) {
  float diffuse = 0.0f, specular = 0.0f;
  for (const auto& p : paths) {
    float ndotl = N.Dot(p.dir);
    if (ndotl <= 0.0f || p.weight <= 0.0f) continue;
    diffuse += ndotl * p.weight;
    if (mat.specular > 0.0f) {
      Vec3f refl = (-p.dir).ReflectAbout(N, true);
      float rv = std::max(refl.Dot(view), 0.0f);
      specular += std::pow(rv, mat.specular) * p.weight;
    }
  }
  diffuse = std::min(diffuse, 1.0f);
  specular = std::min(specular, 1.0f);
  return static_cast<uint8_t>(std::min(
      mat.color.x * diffuse + 255.0f * specular, 255.0f));
}

int ReferenceDiffuse() {
  VisScene sc;
  auto conns = ConnectSphere(*sc.lens, sc.P, sc.L);
  TASK_CHECK(conns.size() == 3);
  // straight segment crosses the sphere, so only the three bent paths
  // survive; each weighted by its transmission with its own direction
  std::vector<PathRef> paths;
  for (const auto& c : conns)
    paths.push_back({(c.entry - sc.P).Unit(),
                     static_cast<float>(c.transmission)});
  Material gray = OpaqueMat(128);
  uint8_t expected = ReferenceShade(gray, Vec3f{1, 0, 0},
                                    Vec3f{0, 0, -1}.Unit(), paths);
  Lights lights;
  lights.AddPoint(1.0f, sc.L.x, sc.L.y, sc.L.z);
  lights.SetPointRefractor(sc.lens);
  uint8_t got = lights.ColorAt(sc.objects, *sc.receiver, sc.P,
                               Camera(10, 30, 30, {0, 0, -4}),
                               sc.receiver->normal).x;
  TASK_CHECK(std::abs((int)got - (int)expected) <= 1);
  return 0;
}

int BacklitButBentLit() {
  VisScene sc;
  // normal ~91.6 degrees from the straight +x direction: the straight
  // path is backlit, but at least one bent first segment is lit
  Vec3f backNormal{std::cos(1.6f), std::sin(1.6f), 0};
  auto quad = MakeReceiver(sc.P, backNormal, OpaqueMat(128));
  sc.objects.push_back(std::move(quad));
  Quad* receiver = static_cast<Quad*>(sc.objects.back().get());
  Camera cam(10, 30, 30, {0, 0, -4});
  Lights legacy;
  legacy.AddPoint(1.0f, sc.L.x, sc.L.y, sc.L.z);
  uint8_t dark = legacy.ColorAt(sc.objects, *receiver, sc.P, cam,
                                receiver->normal).x;
  TASK_CHECK(dark == 0); // no early-exit path reaches the light
  Lights lens;
  lens.AddPoint(1.0f, sc.L.x, sc.L.y, sc.L.z);
  lens.SetPointRefractor(sc.lens);
  uint8_t lit = lens.ColorAt(sc.objects, *receiver, sc.P, cam,
                             receiver->normal).x;
  TASK_CHECK(lit > 0);
  // cross-check with the independent reference over bent first segments
  auto conns = ConnectSphere(*sc.lens, sc.P, sc.L);
  std::vector<PathRef> paths;
  for (const auto& c : conns)
    paths.push_back({(c.entry - sc.P).Unit(),
                     static_cast<float>(c.transmission)});
  uint8_t ref = ReferenceShade(OpaqueMat(128), backNormal,
                               Vec3f{0, 0, -1}.Unit(), paths);
  TASK_CHECK(std::abs((int)lit - (int)ref) <= 1);
  return 0;
}

int SpecularDirection() {
  VisScene sc;
  Material shiny = OpaqueMat(128);
  shiny.specular = 30.0f;
  auto quad = MakeReceiver(sc.P, Vec3f{1, 0, 0}, shiny);
  sc.objects.push_back(std::move(quad));
  Quad* receiver = static_cast<Quad*>(sc.objects.back().get());
  // camera inside the path plane (z=0), looking along +x so the view
  // direction nearly coincides with the near-axis path's reflection
  Camera cam(10, 30, 30, {2.0f, 0.0f, 0.0f});
  Vec3f view = (cam.center() - sc.P).Unit();
  auto conns = ConnectSphere(*sc.lens, sc.P, sc.L);
  std::vector<PathRef> paths;
  for (const auto& c : conns)
    paths.push_back({(c.entry - sc.P).Unit(),
                     static_cast<float>(c.transmission)});
  uint8_t expected = ReferenceShade(shiny, Vec3f{1, 0, 0}, view, paths);
  Lights lights;
  lights.AddPoint(1.0f, sc.L.x, sc.L.y, sc.L.z);
  lights.SetPointRefractor(sc.lens);
  uint8_t got = lights.ColorAt(sc.objects, *receiver, sc.P, cam,
                               receiver->normal).x;
  TASK_CHECK(std::abs((int)got - (int)expected) <= 1);
  TASK_CHECK(got > 128); // specular adds on top of diffuse
  return 0;
}

int TwoLightInstances() {
  VisScene sc;
  Camera cam(10, 30, 30, {0, 0, -4});
  Lights enabled, disabled;
  enabled.AddPoint(1.0f, sc.L.x, sc.L.y, sc.L.z);
  disabled.AddPoint(1.0f, sc.L.x, sc.L.y, sc.L.z);
  enabled.SetPointRefractor(sc.lens);
  uint8_t a = enabled.ColorAt(sc.objects, *sc.receiver, sc.P, cam,
                              sc.receiver->normal).x;
  uint8_t b = disabled.ColorAt(sc.objects, *sc.receiver, sc.P, cam,
                               sc.receiver->normal).x;
  TASK_CHECK(a != b); // independent instances keep independent modes
  TASK_CHECK(a > 0);
  return 0;
}

int ReplaceAndDisable() {
  VisScene sc;
  Camera cam(10, 30, 30, {0, 0, -4});
  Lights lights;
  lights.AddPoint(1.0f, sc.L.x, sc.L.y, sc.L.z);
  // disabled matches the legacy straight-line behaviour
  Lights legacy;
  legacy.AddPoint(1.0f, sc.L.x, sc.L.y, sc.L.z);
  TASK_CHECK(lights.ColorAt(sc.objects, *sc.receiver, sc.P, cam,
                            sc.receiver->normal)
             == legacy.ColorAt(sc.objects, *sc.receiver, sc.P, cam,
                               sc.receiver->normal));
  // enable: changes the shade
  lights.SetPointRefractor(sc.lens);
  uint8_t enabled = lights.ColorAt(sc.objects, *sc.receiver, sc.P, cam,
                                   sc.receiver->normal).x;
  TASK_CHECK(enabled > 0);
  // replace with a different sphere (different IOR): shade changes
  auto other = std::make_unique<Sphere>(
      Vec3f{0, 0, 0}, 1.0f, GlassMat(1.0, 1.8f));
  sc.objects.push_back(std::move(other));
  Sphere* second = static_cast<Sphere*>(sc.objects.back().get());
  lights.SetPointRefractor(second);
  uint8_t replaced = lights.ColorAt(sc.objects, *sc.receiver, sc.P, cam,
                                    sc.receiver->normal).x;
  TASK_CHECK(replaced != enabled);
  // disable again: back to legacy
  lights.SetPointRefractor(nullptr);
  TASK_CHECK(lights.ColorAt(sc.objects, *sc.receiver, sc.P, cam,
                            sc.receiver->normal)
             == legacy.ColorAt(sc.objects, *sc.receiver, sc.P, cam,
                               sc.receiver->normal));
  return 0;
}

int RealTraceWritesPpm() {
  // camera at -z looking towards +z; lens at the origin, receiver behind
  // it and the light on the camera side, so receiver pixels are lit
  // through air -> sphere -> air connections
  Camera camera(12, 40, 40, {0, 0, -8});
  Lights lights;
  lights.AddPoint(1.0f, -0.4f, 0.3f, -2.5f);
  RayTracer tracer(camera, lights);
  tracer.SetBackground(2, 3, 7);
  // AddObject returns the owned object's reference; its address stays
  // valid for the tracer's lifetime and is what SetPointRefractor keeps
  Sphere& lens = tracer.AddObject<Sphere>(Vec3f{0, 0, 0}, 1.0f,
                                          GlassMat(1.0, 1.5f));
  Material wallMat = OpaqueMat(170);
  wallMat.specular = 15.0f;
  // winding chosen so the quad normal points towards -z (camera/light)
  tracer.AddObject<Quad>(Vec3f{-1.5f, -1.5f, 3},
                         Vec3f{-1.5f, 1.5f, 3},
                         Vec3f{1.5f, 1.5f, 3},
                         Vec3f{1.5f, -1.5f, 3}, wallMat);
  lights.SetPointRefractor(&lens);
  tracer.Trace(4);
  Ppm::Write(tracer.image(), ".task-build/lens.ppm");
  unsigned foreground = 0;
  for (const auto& px : tracer.image().data)
    if (!(px == Vec3u8{2, 3, 7})) ++foreground;
  TASK_CHECK(foreground > 0);

  // a second render without the refractor must differ somewhere,
  // proving the real TraceRay path consumes the connections
  Lights plainLights;
  plainLights.AddPoint(1.0f, -0.4f, 0.3f, -2.5f);
  RayTracer reference(camera, plainLights);
  reference.SetBackground(2, 3, 7);
  reference.AddObject<Sphere>(Vec3f{0, 0, 0}, 1.0f,
                              GlassMat(1.0, 1.5f));
  reference.AddObject<Quad>(Vec3f{-1.5f, -1.5f, 3},
                            Vec3f{-1.5f, 1.5f, 3},
                            Vec3f{1.5f, 1.5f, 3},
                            Vec3f{1.5f, -1.5f, 3}, wallMat);
  reference.Trace(4);
  bool differs = false;
  const auto& a = tracer.image().data;
  const auto& b = reference.image().data;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i] != b[i]) differs = true;
  TASK_CHECK(differs);
  return 0;
}

} // namespace

// ============================== registration ==============================
extern const struct task_case task_cases[] = {
    {"connections", "canon_three_roots", CanonThreeRoots},
    {"connections", "forward_snell_sphere_backsub",
     ForwardSnellAndSphereBackSub},
    {"connections", "completeness_random", CompletenessRandom},
    {"connections", "reciprocity", Reciprocity},
    {"connections", "no_solution", NoSolution},
    {"connections", "rigid_transform_scale", RigidTransformAndScale},
    {"visibility", "each_segment_blocked", EachSegmentBlocked},
    {"visibility", "blocker_misses_original_line",
     BlockerMissesOriginalLine},
    {"visibility", "behind_light_no_occlusion", BehindLightDoesNotOcclude},
    {"visibility", "straight_bent_coexist", StraightAndBentCoexist},
    {"visibility", "receiver_ignore_semantics", ReceiverIgnoreSemantics},
    {"lighting", "reference_diffuse", ReferenceDiffuse},
    {"lighting", "backlit_bent_lit", BacklitButBentLit},
    {"lighting", "specular_direction", SpecularDirection},
    {"lighting", "two_light_instances", TwoLightInstances},
    {"lighting", "replace_and_disable", ReplaceAndDisable},
    {"lighting", "real_trace_writes_ppm", RealTraceWritesPpm},
};
extern const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
