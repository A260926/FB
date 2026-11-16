// Target callbacks for the exact curve-on-surface composition.
//
// The three groups exercise sisl_curve_on_surface_exact() against an
// independent Bernstein reference, check the exact knot-preimage
// partition (including tangencies, repeated crossings, knot-line
// segments and simultaneous arrivals), and verify compatibility
// behaviour (status codes, legacy routines and resource ownership).

#include "registry.hpp"
#include "sisl.h"

#include <cmath>
#include <cstddef>
#include <vector>
#include <string>
#include <algorithm>

// s1384 is part of the internal SISL prototype set (sislP.h); declare
// the legacy routine here so the test stays on the public sisl.h.
extern "C" void s1384(SISLCurve *, SISLSurf *, int, int, double, int *,
                      int *, int *, double [], double [], double [],
                      double [], int *);

namespace {

const double TOL = 1.0e-8;

SISLCurve *make_curve(int in, int ik, std::vector<double> knots,
                      std::vector<double> coef, int idim)
{
  return newCurve(in, ik, knots.data(), coef.data(), 1, idim, 1);
}

SISLSurf *make_surf(int in1, int in2, int ik1, int ik2,
                    std::vector<double> k1, std::vector<double> k2,
                    std::vector<double> coef)
{
  return newSurf(in1, in2, ik1, ik2, k1.data(), k2.data(), coef.data(),
                 1, 3, 1);
}

// Distinct interior break times of a SISL curve knot vector.
std::vector<double> interior_breaks(const SISLCurve *c)
{
  std::vector<double> out;
  double t0 = c->et[c->ik - 1];
  double t1 = c->et[c->in];
  for (int i = c->ik; i < c->in; ++i)
    {
      double t = c->et[i];
      if (t > t0 + 1.0e-12 && t < t1 - 1.0e-12 &&
          (out.empty() || fabs(out.back() - t) > 1.0e-10))
        out.push_back(t);
    }
  return out;
}

bool has_break(const std::vector<double> &breaks, double t,
               double tol = 1.0e-9)
{
  for (size_t i = 0; i < breaks.size(); ++i)
    if (fabs(breaks[i] - t) <= tol) return true;
  return false;
}

double binom(int n, int k)
{
  double v = 1.0;
  for (int i = 1; i <= k; ++i) v = v * (n - k + i) / i;
  return v;
}

// Bernstein basis B_{i,n} on the interval [a,b].
double bernstein(int i, int n, double a, double b, double t)
{
  double x = (t - a) / (b - a);
  return binom(n, i) * std::pow(x, i) * std::pow(1.0 - x, n - i);
}

// Independent reference: a cubic x cubic Bezier tensor-product patch
// evaluated directly from its Bernstein form, with no SISL calls.
struct RefPatch
{
  double u0, u1, v0, v1;
  double net[4][4][3];   // net[i][j], i along u, j along v
  double q[4][2];        // cubic Bezier control points of (u(t),v(t))
  double t0, t1;

  double uv_comp(int k, double t) const
  {
    double x = (t - t0) / (t1 - t0);
    return bernstein(k, 3, 0.0, 1.0, x);
  }

  void eval_uv(double t, double uv[2], double duv[2]) const
  {
    double x = (t - t0) / (t1 - t0);
    uv[0] = uv[1] = 0.0;
    for (int k = 0; k < 4; ++k)
      {
        double b = bernstein(k, 3, 0.0, 1.0, x);
        uv[0] += b * q[k][0];
        uv[1] += b * q[k][1];
      }
    if (duv)
      {
        duv[0] = duv[1] = 0.0;
        for (int k = 0; k < 3; ++k)
          {
            double b = bernstein(k, 2, 0.0, 1.0, x);
            for (int c = 0; c < 2; ++c)
              duv[c] += 3.0 * b * (q[k + 1][c] - q[k][c]) / (t1 - t0);
          }
      }
  }

  void eval_surf(double u, double v, double p[3],
                 double su[3], double sv[3]) const
  {
    double bu[4], bv[4], dbu[4], dbv[4];
    for (int i = 0; i < 4; ++i)
      {
        bu[i] = bernstein(i, 3, u0, u1, u);
        bv[i] = bernstein(i, 3, v0, v1, v);
      }
    for (int d = 0; d < 3; ++d) p[d] = 0.0;
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j)
        for (int d = 0; d < 3; ++d)
          p[d] += bu[i] * bv[j] * net[i][j][d];
    if (su && sv)
      {
        // Derivatives of the degree-3 Bernstein basis on [u0,u1]
        // and [v0,v1].
        for (int i = 0; i < 4; ++i) { dbu[i] = 0.0; dbv[i] = 0.0; }
        for (int k = 0; k < 3; ++k)
          {
            double bku = bernstein(k, 2, u0, u1, u);
            double bkv = bernstein(k, 2, v0, v1, v);
            dbu[k]     -= 3.0 * bku / (u1 - u0);
            dbu[k + 1] += 3.0 * bku / (u1 - u0);
            dbv[k]     -= 3.0 * bkv / (v1 - v0);
            dbv[k + 1] += 3.0 * bkv / (v1 - v0);
          }
        for (int d = 0; d < 3; ++d) { su[d] = sv[d] = 0.0; }
        for (int i = 0; i < 4; ++i)
          for (int j = 0; j < 4; ++j)
            for (int d = 0; d < 3; ++d)
              {
                su[d] += dbu[i] * bv[j] * net[i][j][d];
                sv[d] += bu[i] * dbv[j] * net[i][j][d];
              }
      }
  }
};

// Point and first-order chain-rule comparison against the reference
// on points strictly inside smooth pieces.
void reference_check(Case &qcase, const RefPatch &ref, SISLCurve *res,
                     const std::vector<double> &breaks)
{
  std::vector<double> cuts;
  cuts.push_back(ref.t0);
  for (size_t i = 0; i < breaks.size(); ++i) cuts.push_back(breaks[i]);
  cuts.push_back(ref.t1);

  double worst_point = 0.0, worst_deriv = 0.0;
  int st = 0, left = 0;
  for (size_t seg = 0; seg + 1 < cuts.size(); ++seg)
    {
      double a = cuts[seg], b = cuts[seg + 1];
      if (!(b > a)) continue;
      for (int m = 1; m <= 6; ++m)
        {
          double t = a + (b - a) * (0.15 + 0.7 * m / 6.0);
          double uv[2], duv[2];
          ref.eval_uv(t, uv, duv);
          double rp[3], rsu[3], rsv[3];
          ref.eval_surf(uv[0], uv[1], rp, rsu, rsv);

          double got[6];
          left = 0;
          s1221(res, 1, t, &left, got, &st);
          qcase.check(st == 0, "s1221 evaluation of result failed");
          for (int d = 0; d < 3; ++d)
            {
              worst_point = std::max(worst_point, fabs(got[d] - rp[d]));
              double chain = rsu[d] * duv[0] + rsv[d] * duv[1];
              worst_deriv = std::max(worst_deriv,
                                     fabs(got[3 + d] - chain));
            }
        }
    }
  qcase.check(worst_point <= TOL, "Bernstein point mismatch");
  qcase.check(worst_deriv <= TOL, "Bernstein chain-rule mismatch");
}

RefPatch make_unit_ref()
{
  RefPatch ref;
  ref.u0 = ref.v0 = ref.t0 = 0.0;
  ref.u1 = ref.v1 = ref.t1 = 1.0;
  // A fixed, non-symmetric coefficient net.
  double seed = 7.0;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int d = 0; d < 3; ++d)
        {
          seed = std::fmod(seed * 13.0 + 3.0 * i + 5.0 * j + 7.0 * d,
                           17.0);
          ref.net[i][j][d] = (seed - 8.0) * 0.25;
        }
  double q[4][2] = {{0.10, 0.20}, {0.85, 0.15}, {0.20, 0.90},
                    {0.90, 0.80}};
  for (int k = 0; k < 4; ++k) { ref.q[k][0] = q[k][0]; ref.q[k][1] = q[k][1]; }
  return ref;
}

SISLSurf *ref_surface(const RefPatch &ref)
{
  std::vector<double> ku(8), kv(8), coef(4 * 4 * 3);
  for (int i = 0; i < 4; ++i)
    {
      ku[i] = ref.u0; ku[i + 4] = ref.u1;
      kv[i] = ref.v0; kv[i + 4] = ref.v1;
    }
  for (int j = 0; j < 4; ++j)
    for (int i = 0; i < 4; ++i)
      for (int d = 0; d < 3; ++d)
        coef[(j * 4 + i) * 3 + d] = ref.net[i][j][d];
  return make_surf(4, 4, 4, 4, ku, kv, coef);
}

SISLCurve *ref_uv_curve(const RefPatch &ref)
{
  int n = 4, ik = 4;
  std::vector<double> knots(n + ik), coef(n * 2);
  for (int i = 0; i < ik; ++i) { knots[i] = ref.t0; knots[i + n] = ref.t1; }
  for (int k = 0; k < n; ++k)
    { coef[2 * k] = ref.q[k][0]; coef[2 * k + 1] = ref.q[k][1]; }
  return make_curve(n, ik, knots, coef, 2);
}

// ---------------------------------------------------------------------
// exact-composition group
// ---------------------------------------------------------------------
int exact_bicubic_reference(Case &qcase)
{
  RefPatch ref = make_unit_ref();
  SISLSurf *s = ref_surface(ref);
  SISLCurve *uv = ref_uv_curve(ref);
  qcase.check(s != NULL && uv != NULL, "reference objects built");

  SISLCurve *res = NULL;
  int st = 99;
  sisl_curve_on_surface_exact(s, uv, &res, &st);
  qcase.check(st == 0, "bicubic x cubic composition status");
  qcase.check(res != NULL, "result produced");
  if (res)
    {
      // Degree bound is p*(p1+p2) = 3*(3+3) = 18; a single Bezier piece
      // must realise exactly that order.
      qcase.check(res->ik - 1 == 18, "output degree must be 18");
      qcase.check(res->in == 19, "single piece has 19 coefficients");
      qcase.check(fabs(res->et[res->ik - 1]) < 1.0e-12 &&
                  fabs(res->et[res->in] - 1.0) < 1.0e-12,
                  "parameter interval preserved");
      reference_check(qcase, ref, res, interior_breaks(res));
    }
  if (res) freeCurve(res);
  freeCurve(uv);
  freeSurf(s);
  return 0;
}

int exact_bezier_decomposition(Case &qcase)
{
  RefPatch ref = make_unit_ref();
  SISLSurf *s = ref_surface(ref);
  SISLCurve *uv = ref_uv_curve(ref);
  SISLCurve *res = NULL, *bez = NULL;
  int st = 99;
  sisl_curve_on_surface_exact(s, uv, &res, &st);
  qcase.check(st == 0 && res != NULL, "composition for s1730");

  s1730(res, &bez, &st);
  qcase.check(st == 0 && bez != NULL, "Bezier decomposition via s1730");
  if (bez)
    {
      double worst = 0.0;
      int l1 = 0, l2 = 0;
      for (int m = 0; m <= 80; ++m)
        {
          double t = m / 80.0, a[3], b[3];
          l1 = l2 = 0;
          s1221(res, 0, t, &l1, a, &st);
          s1221(bez, 0, t, &l2, b, &st);
          for (int d = 0; d < 3; ++d)
            worst = std::max(worst, fabs(a[d] - b[d]));
        }
      qcase.check(worst <= 1.0e-10, "Bezier form disagrees with result");
      freeCurve(bez);
    }
  if (res) freeCurve(res);
  freeCurve(uv);
  freeSurf(s);
  return 0;
}

int exact_non_unit_domain(Case &qcase)
{
  RefPatch ref = make_unit_ref();
  ref.u0 = 2.0; ref.u1 = 5.0;
  ref.v0 = -1.0; ref.v1 = 2.0;
  ref.t0 = 2.0; ref.t1 = 5.0;
  // Remap the unit-square uv controls into the new parameter rectangle.
  for (int k = 0; k < 4; ++k)
    {
      ref.q[k][0] = ref.u0 + (ref.u1 - ref.u0) * ref.q[k][0];
      ref.q[k][1] = ref.v0 + (ref.v1 - ref.v0) * ref.q[k][1];
    }
  SISLSurf *s = ref_surface(ref);
  SISLCurve *uv = ref_uv_curve(ref);
  qcase.check(s != NULL && uv != NULL, "non-unit domain objects");

  SISLCurve *res = NULL;
  int st = 99;
  sisl_curve_on_surface_exact(s, uv, &res, &st);
  qcase.check(st == 0, "non-unit domain composition");
  if (res)
    {
      qcase.check(fabs(res->et[res->ik - 1] - 2.0) < 1.0e-12 &&
                  fabs(res->et[res->in] - 5.0) < 1.0e-12,
                  "non-unit parameter interval preserved");
      reference_check(qcase, ref, res, interior_breaks(res));
    }
  if (res) freeCurve(res);
  freeCurve(uv);
  freeSurf(s);
  return 0;
}

int exact_constant_piece(Case &qcase)
{
  // Linear uv curve whose first piece is stationary.
  std::vector<double> ku = {0, 0, 0, 0, 1, 1, 1, 1};
  std::vector<double> kv = {0, 0, 0, 0, 1, 1, 1, 1};
  std::vector<double> sc(4 * 4 * 3, 0.0);
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      {
        sc[(j * 4 + i) * 3 + 0] = 0.25 * i;
        sc[(j * 4 + i) * 3 + 1] = 0.25 * j;
        sc[(j * 4 + i) * 3 + 2] = 0.1 * i * j;
      }
  SISLSurf *s = make_surf(4, 4, 4, 4, ku, kv, sc);

  std::vector<double> ck = {0, 0, 0.5, 1, 1};
  std::vector<double> cc = {0.2, 0.3, 0.2, 0.3, 0.8, 0.7};
  SISLCurve *uv = make_curve(3, 2, ck, cc, 2);

  SISLCurve *res = NULL;
  int st = 99;
  sisl_curve_on_surface_exact(s, uv, &res, &st);
  qcase.check(st == 0 && res != NULL, "constant uv piece accepted");
  if (res)
    {
      int lc = 0, l1 = 0, l2 = 0;
      double worst = 0.0;
      for (int m = 0; m <= 60; ++m)
        {
          double t = m / 60.0, cval[8], sder[30], nrm[3], got[3];
          lc = 0;
          s1221(uv, 1, t, &lc, cval, &st);
          l1 = l2 = 0;
          s1421(s, 0, cval, &l1, &l2, sder, nrm, &st);
          lc = 0;
          s1221(res, 0, t, &lc, got, &st);
          for (int d = 0; d < 3; ++d)
            worst = std::max(worst, fabs(got[d] - sder[d]));
        }
      qcase.check(worst <= 1.0e-10, "constant piece value mismatch");
      freeCurve(res);
    }
  freeCurve(uv);
  freeSurf(s);
  return 0;
}

int exact_ownership_after_free(Case &qcase)
{
  RefPatch ref = make_unit_ref();
  SISLSurf *s = ref_surface(ref);
  SISLCurve *uv = ref_uv_curve(ref);
  SISLCurve *res = NULL;
  int st = 99;
  sisl_curve_on_surface_exact(s, uv, &res, &st);
  qcase.check(st == 0 && res != NULL, "composition for ownership test");
  if (!res) { freeCurve(uv); freeSurf(s); return 0; }

  double saved[3], again[6];
  int left = 0;
  s1221(res, 0, 0.37, &left, saved, &st);
  int st2 = 0;
  freeCurve(uv);
  freeSurf(s);
  left = 0;
  s1221(res, 1, 0.37, &left, again, &st2);
  qcase.check(st2 == 0, "evaluation after freeing sources failed");
  qcase.check(fabs(again[0] - saved[0]) < 1.0e-14 &&
              fabs(again[1] - saved[1]) < 1.0e-14 &&
              fabs(again[2] - saved[2]) < 1.0e-14,
              "result borrows input memory");
  freeCurve(res);
  return 0;
}

// ---------------------------------------------------------------------
// knot-preimages group
// ---------------------------------------------------------------------

// Surface shared by the partition tests: quadratic u axis with the
// internal knots .3 and .7 of multiplicity 2, linear v axis.
SISLSurf *partition_surface()
{
  std::vector<double> ku = {0, 0, 0, 0.3, 0.3, 0.7, 0.7, 1, 1, 1};
  std::vector<double> kv = {0, 0, 1, 1};
  int in1 = 7, in2 = 2;
  std::vector<double> coef(in1 * in2 * 3);
  for (int i = 0; i < in1; ++i)
    for (int j = 0; j < in2; ++j)
      {
        coef[(j * in1 + i) * 3 + 0] = 0.2 * i;
        coef[(j * in1 + i) * 3 + 1] = j;
        coef[(j * in1 + i) * 3 + 2] = 0.1 * i * j;
      }
  return make_surf(in1, in2, 3, 2, ku, kv, coef);
}

int preimage_repeated_crossings(Case &qcase)
{
  SISLSurf *s = partition_surface();
  // Linear uv curve over four intervals; u oscillates across both
  // internal knot lines .3 and .7.
  std::vector<double> ck = {0, 0, 0.25, 0.5, 0.75, 1, 1};
  std::vector<double> cc = {0.1, 0.2,  0.8, 0.3,  0.2, 0.7,
                            0.9, 0.4,  0.1, 0.8};
  SISLCurve *uv = make_curve(5, 2, ck, cc, 2);
  qcase.check(s != NULL && uv != NULL, "repeated-crossing objects");

  SISLCurve *res = NULL;
  int st = 99;
  sisl_curve_on_surface_exact(s, uv, &res, &st);
  qcase.check(st == 0 && res != NULL, "repeated crossings status");
  if (!res) { freeCurve(uv); freeSurf(s); return 0; }

  std::vector<double> breaks = interior_breaks(res);

  // Analytic crossing times from the piecewise-linear definition.
  const double uc[5] = {0.1, 0.8, 0.2, 0.9, 0.1};
  const double knots[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
  std::vector<double> expected = {0.25, 0.5, 0.75};
  for (int seg = 0; seg < 4; ++seg)
    {
      double a = knots[seg], b = knots[seg + 1];
      double ua = uc[seg], ub = uc[seg + 1];
      for (int k = 0; k < 2; ++k)
        {
          double line = k == 0 ? 0.3 : 0.7;
          if ((ua - line) * (ub - line) < 0.0)
            {
              double t = a + (line - ua) / (ub - ua) * (b - a);
              expected.push_back(t);
            }
        }
    }
  for (size_t i = 0; i < expected.size(); ++i)
    qcase.check(has_break(breaks, expected[i]),
                "missing analytic knot preimage");

  // Pointwise agreement with s1421.
  double worst = 0.0;
  int lc = 0, l1 = 0, l2 = 0;
  for (int m = 0; m <= 120; ++m)
    {
      double t = m / 120.0, cval[8], sder[30], nrm[3], got[3];
      lc = 0; s1221(uv, 1, t, &lc, cval, &st);
      l1 = l2 = 0; s1421(s, 0, cval, &l1, &l2, sder, nrm, &st);
      lc = 0; s1221(res, 0, t, &lc, got, &st);
      for (int d = 0; d < 3; ++d)
        worst = std::max(worst, fabs(got[d] - sder[d]));
    }
  qcase.check(worst <= 1.0e-10, "repeated-crossing value mismatch");

  freeCurve(res);
  freeCurve(uv);
  freeSurf(s);
  return 0;
}

int preimage_double_root_tangency(Case &qcase)
{
  SISLSurf *s = partition_surface();
  // Quadratic Bezier u(t) = 0.6(1-2t+2t^2), minimum u(.5)=.3 with
  // u'(.5)=0: a tangential contact with knot line u=.3 that returns.
  std::vector<double> ck = {0, 0, 0, 1, 1, 1};
  std::vector<double> cc = {0.6, 0.2,  0.0, 0.5,  0.6, 0.8};
  SISLCurve *uv = make_curve(3, 3, ck, cc, 2);

  SISLCurve *res = NULL;
  int st = 99;
  sisl_curve_on_surface_exact(s, uv, &res, &st);
  qcase.check(st == 0 && res != NULL, "tangency composition status");
  if (res)
    {
      std::vector<double> breaks = interior_breaks(res);
      qcase.check(has_break(breaks, 0.5, 1.0e-9),
                  "double-root contact time missing");
      // Exactly one isolated time near .5, not a cluster of splits.
      int near = 0;
      for (size_t i = 0; i < breaks.size(); ++i)
        if (fabs(breaks[i] - 0.5) < 1.0e-6) ++near;
      qcase.check(near == 1, "tangency produced repeated partitions");

      double worst = 0.0;
      int lc = 0, l1 = 0, l2 = 0;
      for (int m = 0; m <= 100; ++m)
        {
          double t = m / 100.0, cval[8], sder[30], nrm[3], got[3];
          lc = 0; s1221(uv, 1, t, &lc, cval, &st);
          l1 = l2 = 0; s1421(s, 0, cval, &l1, &l2, sder, nrm, &st);
          lc = 0; s1221(res, 0, t, &lc, got, &st);
          for (int d = 0; d < 3; ++d)
            worst = std::max(worst, fabs(got[d] - sder[d]));
        }
      qcase.check(worst <= 1.0e-10, "tangency value mismatch");
      freeCurve(res);
    }
  freeCurve(uv);
  freeSurf(s);
  return 0;
}

int preimage_knot_line_segment(Case &qcase)
{
  SISLSurf *s = partition_surface();
  // u == .3 throughout the middle interval [.4,.6].
  std::vector<double> ck = {0, 0, 0.4, 0.6, 1, 1};
  std::vector<double> cc = {0.1, 0.2, 0.3, 0.4, 0.3, 0.7, 0.9, 0.8};
  SISLCurve *uv = make_curve(4, 2, ck, cc, 2);

  SISLCurve *res = NULL;
  int st = 99;
  sisl_curve_on_surface_exact(s, uv, &res, &st);
  qcase.check(st == 0 && res != NULL, "knot-line segment status");
  if (res)
    {
      std::vector<double> breaks = interior_breaks(res);
      qcase.check(has_break(breaks, 0.4) && has_break(breaks, 0.6),
                  "knot-line interval endpoints missing");
      for (size_t i = 0; i < breaks.size(); ++i)
        qcase.check(!(breaks[i] > 0.4 + 1.0e-9 &&
                      breaks[i] < 0.6 - 1.0e-9),
                    "infinite splits along a knot line");

      double worst = 0.0;
      int lc = 0, l1 = 0, l2 = 0;
      for (int m = 0; m <= 100; ++m)
        {
          double t = m / 100.0, cval[8], sder[30], nrm[3], got[3];
          lc = 0; s1221(uv, 1, t, &lc, cval, &st);
          l1 = l2 = 0; s1421(s, 0, cval, &l1, &l2, sder, nrm, &st);
          lc = 0; s1221(res, 0, t, &lc, got, &st);
          for (int d = 0; d < 3; ++d)
            worst = std::max(worst, fabs(got[d] - sder[d]));
        }
      qcase.check(worst <= 1.0e-10, "knot-line segment value mismatch");
      freeCurve(res);
    }
  freeCurve(uv);
  freeSurf(s);
  return 0;
}

int preimage_uv_own_knots(Case &qcase)
{
  SISLSurf *s = partition_surface();
  // Cubic uv curve with an interior knot of multiplicity 3 at .5.
  std::vector<double> ck = {0, 0, 0, 0, 0.5, 0.5, 0.5, 1, 1, 1, 1};
  std::vector<double> cc = {0.1, 0.1, 0.4, 0.2, 0.6, 0.8, 0.2, 0.3,
                            0.7, 0.7, 0.9, 0.9, 0.5, 0.5};
  SISLCurve *uv = make_curve(7, 4, ck, cc, 2);
  qcase.check(uv != NULL, "uv curve with C0 knot");

  SISLCurve *res = NULL;
  int st = 99;
  sisl_curve_on_surface_exact(s, uv, &res, &st);
  qcase.check(st == 0 && res != NULL, "uv-knot composition status");
  if (res)
    {
      std::vector<double> breaks = interior_breaks(res);
      qcase.check(has_break(breaks, 0.5), "uv interior knot not kept");

      // The joint must be crack free even though the tangent jumps.
      double lp[3], rp[3];
      int left = 0;
      s1221(res, 0, 0.5, &left, lp, &st);
      left = 0;
      s1221(res, 0, 0.5, &left, rp, &st);
      double gap = 0.0;
      for (int d = 0; d < 3; ++d) gap = std::max(gap, fabs(lp[d] - rp[d]));
      qcase.check(gap <= 1.0e-13, "crack at uv knot");
      freeCurve(res);
    }
  freeCurve(uv);
  freeSurf(s);
  return 0;
}

int preimage_simultaneous_lines(Case &qcase)
{
  // Quadratic in both axes with matching internal knots .5,.5.
  std::vector<double> ku = {0, 0, 0, 0.5, 0.5, 1, 1, 1};
  std::vector<double> coef(5 * 5 * 3);
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 5; ++j)
      {
        coef[(j * 5 + i) * 3 + 0] = 0.2 * i;
        coef[(j * 5 + i) * 3 + 1] = 0.2 * j;
        coef[(j * 5 + i) * 3 + 2] = 0.05 * i * j;
      }
  SISLSurf *s = make_surf(5, 5, 3, 3, ku, ku, coef);

  // Linear uv through (.5,.5) at t=.5: both knot lines hit together.
  std::vector<double> ck = {0, 0, 1, 1};
  std::vector<double> cc = {0.1, 0.1, 0.9, 0.9};
  SISLCurve *uv = make_curve(2, 2, ck, cc, 2);

  SISLCurve *res = NULL;
  int st = 99;
  sisl_curve_on_surface_exact(s, uv, &res, &st);
  qcase.check(st == 0 && res != NULL, "simultaneous arrival status");
  if (res)
    {
      std::vector<double> breaks = interior_breaks(res);
      qcase.check(has_break(breaks, 0.5), "simultaneous time missing");
      int count = 0;
      for (size_t i = 0; i < breaks.size(); ++i)
        if (fabs(breaks[i] - 0.5) < 1.0e-9) ++count;
      qcase.check(count == 1, "duplicate partition for joint arrival");
      freeCurve(res);
    }
  freeCurve(uv);
  freeSurf(s);
  return 0;
}

// ---------------------------------------------------------------------
// compatibility group
// ---------------------------------------------------------------------
int compat_trace_outside(Case &qcase)
{
  RefPatch ref = make_unit_ref();
  SISLSurf *s = ref_surface(ref);
  std::vector<double> ck = {0, 0, 0, 0, 1, 1, 1, 1};
  std::vector<double> cc = {0.2, 0.2, 0.5, 0.5, 1.3, 0.5, 0.9, 0.9};
  SISLCurve *uv = make_curve(4, 4, ck, cc, 2);

  SISLCurve *res = (SISLCurve *)0x1;
  int st = 0;
  sisl_curve_on_surface_exact(s, uv, &res, &st);
  qcase.check(st == -105, "outside trace must return -105");
  qcase.check(res == NULL, "result must be null on -105");
  freeCurve(uv);
  freeSurf(s);
  return 0;
}

int compat_control_outside_trace_inside(Case &qcase)
{
  RefPatch ref = make_unit_ref();
  SISLSurf *s = ref_surface(ref);
  // Quadratic Bezier: u(t)=.5+2t(1-t), v(t)=.5-2t(1-t).  The middle
  // control points leave [0,1]^2 although the trace stays inside.
  std::vector<double> ck = {0, 0, 0, 1, 1, 1};
  std::vector<double> cc = {0.5, 0.5, 1.5, -0.5, 0.5, 0.5};
  SISLCurve *uv = make_curve(3, 3, ck, cc, 2);

  SISLCurve *res = NULL;
  int st = 99;
  sisl_curve_on_surface_exact(s, uv, &res, &st);
  qcase.check(st == 0, "inside trace with outside control points");
  if (res)
    {
      double worst = 0.0;
      int lc = 0, l1 = 0, l2 = 0;
      for (int m = 0; m <= 80; ++m)
        {
          double t = m / 80.0, cval[8], sder[30], nrm[3], got[3];
          lc = 0; s1221(uv, 1, t, &lc, cval, &st);
          l1 = l2 = 0; s1421(s, 0, cval, &l1, &l2, sder, nrm, &st);
          lc = 0; s1221(res, 0, t, &lc, got, &st);
          for (int d = 0; d < 3; ++d)
            worst = std::max(worst, fabs(got[d] - sder[d]));
        }
      qcase.check(worst <= 1.0e-10, "outside-control value mismatch");
      freeCurve(res);
    }
  freeCurve(uv);
  freeSurf(s);
  return 0;
}

int compat_bad_structures(Case &qcase)
{
  RefPatch ref = make_unit_ref();
  SISLSurf *good_s = ref_surface(ref);
  SISLCurve *good_uv = ref_uv_curve(ref);
  SISLCurve *res = NULL;
  int st = 99;

  // NULL inputs.
  res = (SISLCurve *)0x1;
  sisl_curve_on_surface_exact(good_s, NULL, &res, &st);
  qcase.check(st == -103 && res == NULL, "null uv -> -103");
  res = (SISLCurve *)0x1;
  sisl_curve_on_surface_exact(NULL, good_uv, &res, &st);
  qcase.check(st == -103 && res == NULL, "null surface -> -103");

  // Rational curve: unsupported type.
  SISLCurve *rat = copyCurve(good_uv);
  rat->ikind = 2;
  rat->rcoef = NULL;
  res = (SISLCurve *)0x1;
  sisl_curve_on_surface_exact(good_s, rat, &res, &st);
  qcase.check(st == -104 && res == NULL, "rational curve -> -104");
  freeCurve(rat);

  // Periodic flag: unsupported.
  SISLCurve *per = copyCurve(good_uv);
  per->cuopen = SISL_CRV_PERIODIC;
  res = (SISLCurve *)0x1;
  sisl_curve_on_surface_exact(good_s, per, &res, &st);
  qcase.check(st == -104 && res == NULL, "periodic curve -> -104");
  freeCurve(per);

  // Decreasing knot: illegal structure.
  SISLCurve *badk = copyCurve(good_uv);
  badk->et[good_uv->ik] = badk->et[good_uv->ik - 1] - 1.0;
  res = (SISLCurve *)0x1;
  sisl_curve_on_surface_exact(good_s, badk, &res, &st);
  qcase.check(st == -103 && res == NULL, "decreasing knots -> -103");
  freeCurve(badk);

  // Knot spacing below 1e-5: illegal numeric structure.
  SISLCurve *fine = copyCurve(good_uv);
  fine->et[good_uv->ik] = fine->et[good_uv->ik - 1] + 1.0e-7;
  res = (SISLCurve *)0x1;
  sisl_curve_on_surface_exact(good_s, fine, &res, &st);
  qcase.check(st == -103 && res == NULL, "tiny knot spacing -> -103");
  freeCurve(fine);

  // Non-finite control value.
  SISLCurve *nan_c = copyCurve(good_uv);
  nan_c->ecoef[0] = std::nan("");
  res = (SISLCurve *)0x1;
  sisl_curve_on_surface_exact(good_s, nan_c, &res, &st);
  qcase.check(st == -103 && res == NULL, "non-finite coefficient -> -103");
  freeCurve(nan_c);

  // Wrong surface dimension.
  SISLSurf *dim = copySurface(good_s);
  dim->idim = 2;
  res = (SISLCurve *)0x1;
  sisl_curve_on_surface_exact(dim, good_uv, &res, &st);
  qcase.check(st == -104 && res == NULL, "2-D surface -> -104");
  freeSurf(dim);

  freeCurve(good_uv);
  freeSurf(good_s);
  return 0;
}

int compat_legacy_interfaces(Case &qcase)
{
  // Bilinear surface and a 2-D curve mirroring the shipped baseline.
  double sk[] = {0, 0, 1, 1};
  double scp[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1};
  SISLSurf *s = newSurf(2, 2, 2, 2, sk, sk, scp, 1, 3, 1);
  double ck[] = {2, 2, 2, 5, 5, 5};
  double uvp[] = {0, 0, 0.5, 0, 1, 1};
  SISLCurve *uv = newCurve(3, 3, ck, uvp, 1, 2, 1);
  qcase.check(s != NULL && uv != NULL, "legacy objects");

  // Exact composition over the same 17 parameter samples as baseline.
  SISLCurve *exact = NULL;
  int st = 99;
  sisl_curve_on_surface_exact(s, uv, &exact, &st);
  qcase.check(st == 0 && exact != NULL, "exact baseline composition");
  if (exact)
    {
      int lc = 0, l1 = 0, l2 = 0;
      for (int i = 0; i <= 16; ++i)
        {
          double t = 2.0 + 3.0 * i / 16.0;
          double cval[8], sder[30], nrm[3], got[3];
          lc = 0; s1221(uv, 1, t, &lc, cval, &st);
          l1 = l2 = 0; s1421(s, 0, cval, &l1, &l2, sder, nrm, &st);
          lc = 0; s1221(exact, 0, t, &lc, got, &st);
          for (int d = 0; d < 3; ++d)
            qcase.check(fabs(got[d] - sder[d]) <= 1.0e-10,
                        "exact curve disagrees at baseline sample");
        }
    }

  // s1384 point/derivative evaluation still works unchanged.
  int lc = 0, l1 = 0, l2 = 0;
  double eder[9], eu[6], ev[6], en[6];
  s1384(uv, s, 3, 1, 3.5, &lc, &l1, &l2, eder, eu, ev, en, &st);
  qcase.check(st == 0, "s1384 legacy evaluation");

  // s1383 approximating trace still builds and can be evaluated.
  SISLCurve *approx = NULL, *d1 = NULL, *d2 = NULL;
  s1383(s, uv, 1.0e-6, 0.0, 0, &approx, &d1, &d2, &st);
  qcase.check(st == 0 && approx != NULL, "s1383 legacy approximation");
  if (approx)
    {
      int left = 0;
      double p[3];
      s1221(approx, 0, 3.5, &left, p, &st);
      qcase.check(st == 0, "s1383 result evaluable");
      freeCurve(approx);
    }
  if (exact) freeCurve(exact);
  freeCurve(uv);
  freeSurf(s);
  return 0;
}

} // namespace

void register_task_tests(Registry &registry)
{
  registry.add("exact-composition", "bicubic-cubic-reference",
               exact_bicubic_reference);
  registry.add("exact-composition", "bezier-decomposition",
               exact_bezier_decomposition);
  registry.add("exact-composition", "non-unit-domain",
               exact_non_unit_domain);
  registry.add("exact-composition", "constant-uv-piece",
               exact_constant_piece);
  registry.add("exact-composition", "ownership-after-free",
               exact_ownership_after_free);

  registry.add("knot-preimages", "repeated-crossings",
               preimage_repeated_crossings);
  registry.add("knot-preimages", "double-root-tangency",
               preimage_double_root_tangency);
  registry.add("knot-preimages", "knot-line-segment",
               preimage_knot_line_segment);
  registry.add("knot-preimages", "uv-own-knots",
               preimage_uv_own_knots);
  registry.add("knot-preimages", "simultaneous-lines",
               preimage_simultaneous_lines);

  registry.add("compatibility", "trace-outside", compat_trace_outside);
  registry.add("compatibility", "control-outside-trace-inside",
               compat_control_outside_trace_inside);
  registry.add("compatibility", "bad-structures", compat_bad_structures);
  registry.add("compatibility", "legacy-interfaces",
               compat_legacy_interfaces);
}
