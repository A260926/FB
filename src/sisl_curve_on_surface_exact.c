/*
 * Copyright (C) 1998, 2000-2007, 2010, 2011, 2012, 2013 SINTEF Digital,
 * Applied Mathematics, Norway.
 *
 * Contact information: E-mail: tor.dokken@sintef.no
 * SINTEF Digital, Department of Mathematics and Cybernetics,
 * P.O. Box 124 Blindern,
 * 0314 Oslo, Norway.
 *
 * This file is part of SISL.
 *
 * SISL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * SISL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public
 * License along with SISL. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * In accordance with Section 7(b) of the GNU Affero General Public
 * License, a covered work must retain the producer line in every data
 * file that is created or manipulated using SISL.
 *
 * Other Usage
 * You can be released from the requirements of the license by purchasing
 * a commercial license. Buying such a license is mandatory as soon as you
 * develop commercial activities involving the SISL library without
 * disclosing the source code of your own applications.
 *
 * This file may be used in accordance with the terms contained in a
 * written agreement between you and SINTEF Digital.
 */

#include "sisl-copyright.h"

/*
 * Exact polynomial composition of a 2-D parameter curve with a
 * tensor-product B-spline surface.  Unlike s1383, which produces an
 * approximating trace curve, this routine constructs the exact
 * piecewise polynomial B-spline curve
 *
 *     C(t) = S(u(t), v(t))
 *
 * by coefficient arithmetic only.  No sampling, fitting or tolerance
 * driven approximation is involved.
 */

#define SISL_CURVE_ON_SURFACE_EXACT

#include "sislP.h"

#define EOS_OK             0
#define EOS_MEMORY      -101
#define EOS_ILLEGAL     -103
#define EOS_UNSUPPORTED -104
#define EOS_OUTSIDE     -105

/* Distinct geometric features are guaranteed separated by >= 1e-5. */
#define EOS_ROOT_TOL   1.0e-12
#define EOS_MAX_DEPTH  60
#define EOS_MAX_ROOTS  512
#define EOS_MERGE_TOL  1.0e-9
#define EOS_ZERO_TOL   1.0e-11
#define EOS_DOM_TOL    1.0e-9
#define EOS_PARAM_BOUND 100.0
#define EOS_SPACING     1.0e-5

static double eos_fact[19];

static int eos_dcompare(const void *pa, const void *pb);

static int
eos_finite(double x)
{
  return x == x && x < MAXDOUBLE && x > -MAXDOUBLE;
}

static int
eos_finite_array(const double *a, int n)
{
  int i;
  for (i = 0; i < n; ++i) if (!eos_finite(a[i])) return 0;
  return 1;
}

/* Validate a clamped, open, non-rational knot vector whose order lies
 * between low_order and high_order (inclusive). */
static int
eos_check_knots(const double *et, int in, int ik, int low_order,
                int high_order)
{
  int i, j, mult;
  double span;

  if (ik < low_order || ik > high_order) return EOS_UNSUPPORTED;
  if (in < ik) return EOS_ILLEGAL;
  if (et == SISL_NULL) return EOS_ILLEGAL;
  if (!eos_finite_array(et, in + ik)) return EOS_ILLEGAL;

  /* Clamped ends: the first and last ik knots coincide. */
  for (i = 1; i < ik; ++i)
    if (et[i] != et[0] || et[in + ik - 1 - i] != et[in + ik - 1])
      return EOS_ILLEGAL;
  if (!(et[ik - 1] < et[in])) return EOS_ILLEGAL;

  for (i = ik; i <= in; ++i)
    {
      if (et[i] < et[i - 1]) return EOS_ILLEGAL;
      span = et[i] - et[i - 1];
      if (span > 0.0 && span < EOS_SPACING) return EOS_ILLEGAL;
    }

  /* Internal multiplicity must be strictly below the order. */
  i = ik;
  while (i < in)
    {
      j = i;
      while (j < in && et[j] == et[i]) ++j;
      mult = j - i;
      if (mult >= ik) return EOS_ILLEGAL;
      i = j;
    }

  if (fabs(et[ik - 1]) > EOS_PARAM_BOUND ||
      fabs(et[in]) > EOS_PARAM_BOUND)
    return EOS_ILLEGAL;

  return EOS_OK;
}

static int
eos_validate_curve(const SISLCurve *c)
{
  int st;
  if (c == SISL_NULL) return EOS_ILLEGAL;
  if (c->ikind != 1 && c->ikind != 3) return EOS_UNSUPPORTED;
  if (c->idim != 2) return EOS_UNSUPPORTED;
  if (c->cuopen != SISL_CRV_OPEN) return EOS_UNSUPPORTED;
  if (c->ecoef == SISL_NULL) return EOS_ILLEGAL;
  if (c->rcoef != SISL_NULL) return EOS_UNSUPPORTED;
  st = eos_check_knots(c->et, c->in, c->ik, 2, 4);
  if (st != EOS_OK) return st;
  if (!eos_finite_array(c->ecoef, c->in * c->idim)) return EOS_ILLEGAL;
  return EOS_OK;
}

static int
eos_validate_surface(const SISLSurf *s)
{
  int st;
  if (s == SISL_NULL) return EOS_ILLEGAL;
  if (s->ikind != 1 && s->ikind != 3) return EOS_UNSUPPORTED;
  if (s->idim != 3) return EOS_UNSUPPORTED;
  if (s->cuopen_1 != SISL_SURF_OPEN ||
      s->cuopen_2 != SISL_SURF_OPEN)
    return EOS_UNSUPPORTED;
  if (s->ecoef == SISL_NULL) return EOS_ILLEGAL;
  if (s->rcoef != SISL_NULL) return EOS_UNSUPPORTED;

  st = eos_check_knots(s->et1, s->in1, s->ik1, 2, 4);
  if (st != EOS_OK) return st;
  st = eos_check_knots(s->et2, s->in2, s->ik2, 2, 4);
  if (st != EOS_OK) return st;
  if (!eos_finite_array(s->ecoef, s->in1 * s->in2 * s->idim))
    return EOS_ILLEGAL;
  return EOS_OK;
}

/* Monomial coefficients (abscissa x = t - a) of one knot interval of
 * one scalar curve component, from the Taylor derivatives at a. */
static int
eos_curve_monomial(const SISLCurve *c, int comp, int left, double a,
                   double *mon)
{
  int k, st = 0, lf = left, p = c->ik - 1;
  double *der;
  der = (double *)newarray((p + 1) * c->idim, double);
  if (der == SISL_NULL) return EOS_MEMORY;
  s1221((SISLCurve *)c, p, a, &lf, der, &st);
  if (st < 0)
    {
      freearray(der);
      return EOS_ILLEGAL;
    }
  for (k = 0; k <= p; ++k)
    mon[k] = der[k * c->idim + comp] / eos_fact[k];
  freearray(der);
  return EOS_OK;
}

/* Horner evaluation of a power polynomial in abscissa x = t - a. */
static double
eos_power_eval(const double *p, int deg, double a, double t)
{
  double x = t - a, v = p[deg];
  int k;
  for (k = deg - 1; k >= 0; --k) v = v * x + p[k];
  return v;
}

/* Convert a power polynomial a[0..deg] (abscissa on [lo,hi]) to
 * Bernstein coefficients of the same degree on [lo,hi]. */
static void
eos_power_to_bernstein(const double *a, int deg, double lo, double hi,
                       double *b)
{
  int i, k;
  double h = hi - lo, hk, cden;
  for (i = 0; i <= deg; ++i) b[i] = 0.0;
  cden = eos_fact[deg];
  hk = 1.0;
  for (k = 0; k <= deg; ++k)
    {
      double ak = a[k] * hk;
      if (ak != 0.0)
        {
          for (i = k; i <= deg; ++i)
            {
              double cn = eos_fact[i] /
                          (eos_fact[k] * eos_fact[i - k]);
              double cd = cden /
                          (eos_fact[k] * eos_fact[deg - k]);
              b[i] += ak * cn / cd;
            }
        }
      hk *= h;
    }
}

/* de Casteljau split of a Bernstein polynomial at r in [0,1]. */
static int
eos_bern_split(const double *c, int deg, double r, double *left,
               double *right)
{
  double *tmp;
  int i, k;
  tmp = (double *)newarray(deg + 1, double);
  if (tmp == SISL_NULL) return EOS_MEMORY;
  for (i = 0; i <= deg; ++i) tmp[i] = c[i];
  left[0] = c[0];
  right[deg] = c[deg];
  for (k = 1; k <= deg; ++k)
    {
      for (i = 0; i <= deg - k; ++i)
        tmp[i] = (1.0 - r) * tmp[i] + r * tmp[i + 1];
      left[k] = tmp[0];
      right[deg - k] = tmp[deg - k];
    }
  freearray(tmp);
  return EOS_OK;
}

/* A Bernstein polynomial can only vanish if 0 lies in the convex hull
 * of its control values.  The test is relative: with degree up to 18
 * and coefficients of order one, an absolute cutoff would prune genuine
 * small sign changes. */
static int
eos_bern_may_have_zero(const double *c, int deg, double scale)
{
  int i;
  double mn = c[0], mx = c[0], mag = fabs(c[0]);
  for (i = 1; i <= deg; ++i)
    {
      if (c[i] < mn) mn = c[i];
      if (c[i] > mx) mx = c[i];
      if (fabs(c[i]) > mag) mag = fabs(c[i]);
    }
  (void)scale;
  return mn <= 1.0e-12 * mag && mx >= -1.0e-12 * mag;
}

static int
eos_add_root(double t, double *roots, int *n, int cap)
{
  int i, k;
  if (*n >= cap) return EOS_MEMORY;
  for (i = 0; i < *n; ++i)
    if (fabs(roots[i] - t) <=
        EOS_MERGE_TOL * max(1.0, max(fabs(roots[i]), fabs(t))))
      return EOS_OK;
  k = (*n)++;
  while (k > 0 && roots[k - 1] > t)
    {
      roots[k] = roots[k - 1];
      --k;
    }
  roots[k] = t;
  return EOS_OK;
}

/* Recursive Bernstein subdivision collecting raw terminal intervals.
 * Multiple (tangent) roots keep both halves zero-spanning, so a single
 * root can appear as several adjacent terminals; they are clustered and
 * refined by the caller rather than merged greedily here. */
static int
eos_isolate(const double *c, int deg, double lo, double hi, int depth,
            double *raw, int *nraw, int cap)
{
  double mid, *lc, *rc;
  int st;
  double scale = max(fabs(lo), fabs(hi));
  if (!eos_bern_may_have_zero(c, deg, scale)) return EOS_OK;
  if (hi - lo <= EOS_ROOT_TOL || depth >= EOS_MAX_DEPTH || deg == 0)
    {
      if (*nraw < cap) raw[(*nraw)++] = (lo + hi) * 0.5;
      return EOS_OK;
    }

  lc = (double *)newarray(deg + 1, double);
  rc = (double *)newarray(deg + 1, double);
  if (lc == SISL_NULL || rc == SISL_NULL)
    {
      if (lc) freearray(lc);
      if (rc) freearray(rc);
      return EOS_MEMORY;
    }
  mid = (lo + hi) * 0.5;
  st = eos_bern_split(c, deg, 0.5, lc, rc);
  if (st == EOS_OK)
    st = eos_isolate(lc, deg, lo, mid, depth + 1, raw, nraw, cap);
  if (st == EOS_OK)
    st = eos_isolate(rc, deg, mid, hi, depth + 1, raw, nraw, cap);
  freearray(lc);
  freearray(rc);
  return st;
}

/* Horner value of a power polynomial centred at lo. */
static double
eos_horner(const double *a, int deg, double lo, double t)
{
  double x = t - lo, v = a[deg];
  int k;
  for (k = deg - 1; k >= 0; --k) v = v * x + a[k];
  return v;
}

/* Refine a root candidate.  A plain Newton step stalls on a multiple
 * root because the derivative vanishes there too, so Newton is run on
 * every derivative order; the candidate giving the smallest residual of
 * the original polynomial is kept. */
static double
eos_refine_root(const double *a, int deg, double lo, double hi,
                double seed)
{
  double best = seed, bestres = fabs(eos_horner(a, deg, lo, seed));
  int r, k, it;
  for (r = 0; r < deg; ++r)
    {
      int d = deg - r;
      double coef[4];
      double t = seed;
      if (d < 1) break;
      for (k = 0; k <= d; ++k)
        {
          double mul = 1.0;
          int m;
          for (m = 0; m < r; ++m) mul *= (k + r - m);
          coef[k] = a[k + r] * mul;
        }
      for (it = 0; it < 40; ++it)
        {
          double x = t - lo, f = coef[d], fp = d * coef[d];
          for (k = d - 1; k >= 1; --k)
            {
              fp = fp * x + k * coef[k];
              f = f * x + coef[k];
            }
          f = f * x + coef[0];
          if (fabs(fp) < 1.0e-300) break;
          {
            double tn = t - f / fp;
            if (tn < lo) tn = lo;
            if (tn > hi) tn = hi;
            if (fabs(tn - t) <= 1.0e-14) { t = tn; break; }
            t = tn;
          }
        }
      {
        double res = fabs(eos_horner(a, deg, lo, t));
        if (res < bestres) { bestres = res; best = t; }
      }
    }
  return best;
}

/* All roots in [lo,hi] of a power polynomial given in abscissa t-lo.
 * A polynomial identically zero is flagged, not returned as roots. */
static int
eos_roots_of(const double *a, int deg, double lo, double hi,
             int *is_constant_zero, double *roots, int *nroots, int cap)
{
  double *b, *raw;
  double scale = max(1.0, max(fabs(lo), fabs(hi)));
  int i, st, nraw = 0, rawcap = max(256, cap * 8);
  *is_constant_zero = 0;
  for (i = 0; i <= deg; ++i)
    if (fabs(a[i]) > EOS_ZERO_TOL * scale) break;
  if (i > deg)
    {
      *is_constant_zero = 1;
      return EOS_OK;
    }
  b = (double *)newarray(deg + 1, double);
  raw = (double *)newarray(rawcap, double);
  if (b == SISL_NULL || raw == SISL_NULL)
    {
      if (b) freearray(b);
      if (raw) freearray(raw);
      return EOS_MEMORY;
    }
  eos_power_to_bernstein(a, deg, lo, hi, b);
  st = eos_isolate(b, deg, lo, hi, 0, raw, &nraw, rawcap);
  if (st == EOS_OK && nraw == 0 && deg >= 1)
    {
      /* No sign change: the polynomial may still touch zero at an
       * interior extremum (even-multiplicity tangency), whose Bernstein
       * control values stay non-negative or non-positive.  Try every
       * derivative order, since a degree-d multiple root only forces a
       * sign change in the (d-1)-th derivative. */
      double *dc, *db, *draw;
      int r, order_d;
      double mag = 0.0;
      for (r = 0; r <= deg; ++r) mag = max(mag, fabs(a[r]));
      dc = (double *)newarray(deg + 1, double);
      db = (double *)newarray(deg + 1, double);
      draw = (double *)newarray(rawcap, double);
      if (dc == SISL_NULL || db == SISL_NULL || draw == SISL_NULL)
        {
          if (dc) freearray(dc);
          if (db) freearray(db);
          if (draw) freearray(draw);
          freearray(b);
          freearray(raw);
          return EOS_MEMORY;
        }
      for (order_d = 1; order_d < deg && st == EOS_OK; ++order_d)
        {
          int dd = deg - order_d, ndraw = 0, q;
          for (q = 0; q <= dd; ++q)
            {
              double mul = 1.0;
              int m;
              for (m = 0; m < order_d; ++m) mul *= (q + order_d - m);
              dc[q] = a[q + order_d] * mul;
            }
          eos_power_to_bernstein(dc, dd, lo, hi, db);
          eos_isolate(db, dd, lo, hi, 0, draw, &ndraw, rawcap);
          for (q = 0; q < ndraw; ++q)
            {
              double v = eos_horner(a, deg, lo, draw[q]);
              if (fabs(v) <= 1.0e-10 * max(1.0, mag) &&
                  nraw < rawcap)
                raw[nraw++] = draw[q];
            }
        }
      freearray(dc);
      freearray(db);
      freearray(draw);
    }
  if (st == EOS_OK && nraw > 0)
    {
      /* Adjacent terminals closer than a tenth of the minimum feature
       * separation belong to one (possibly multiple) root. */
      qsort(raw, nraw, sizeof(double), eos_dcompare);
      for (i = 0; i < nraw; )
        {
          int j = i + 1;
          double rep;
          while (j < nraw && raw[j] - raw[j - 1] < 1.0e-6) ++j;
          rep = 0.5 * (raw[i] + raw[j - 1]);
          rep = eos_refine_root(a, deg, lo, hi, rep);
          st = eos_add_root(rep, roots, nroots, cap);
          if (st != EOS_OK) break;
          i = j;
        }
    }
  freearray(b);
  freearray(raw);
  return st;
}

/* Real range test of one scalar power polynomial on [lo,hi].  The
 * extrema are the endpoints and the derivative roots, so the curve's
 * actual trace - never its control polygon - decides containment. */
static int
eos_check_containment(const double *a, int deg, double lo, double hi,
                      double dlo, double dhi)
{
  double roots[EOS_MAX_ROOTS];
  double *dp;
  int nroots = 0, i, st, k, iscz = 0;
  double tol = EOS_DOM_TOL * max(1.0, max(fabs(dlo), fabs(dhi)));
  double v;

  v = eos_power_eval(a, deg, lo, lo);
  if (v < dlo - tol || v > dhi + tol) return EOS_OUTSIDE;
  v = eos_power_eval(a, deg, lo, hi);
  if (v < dlo - tol || v > dhi + tol) return EOS_OUTSIDE;

  if (deg < 1) return EOS_OK;
  dp = (double *)newarray(deg, double);
  if (dp == SISL_NULL) return EOS_MEMORY;
  for (k = 0; k < deg; ++k) dp[k] = (k + 1) * a[k + 1];
  /* eos_roots_of handles a stationary (zero derivative) piece and
   * multiple roots without subdividing forever. */
  st = eos_roots_of(dp, deg - 1, lo, hi, &iscz, roots, &nroots,
                    EOS_MAX_ROOTS);
  if (st == EOS_OK)
    {
      for (i = 0; i < nroots; ++i)
        {
          v = eos_power_eval(a, deg, lo, roots[i]);
          if (v < dlo - tol || v > dhi + tol)
            {
              st = EOS_OUTSIDE;
              break;
            }
        }
    }
  freearray(dp);
  return st;
}

/* Product of two power polynomials. */
static void
eos_power_mul(const double *a, int da, const double *b, int db,
              double *out)
{
  int i, j, k;
  for (k = 0; k <= da + db; ++k) out[k] = 0.0;
  for (i = 0; i <= da; ++i)
    if (a[i] != 0.0)
      for (j = 0; j <= db; ++j)
        out[i + j] += a[i] * b[j];
}

/* Taylor shift: q(y) = p(y + s), both polynomials in power form. */
static void
eos_power_shift(const double *a, int deg, double s, double *q)
{
  int k, i;
  for (k = 0; k <= deg; ++k)
    {
      double acc = 0.0, spow = 1.0;
      for (i = k; i <= deg; ++i)
        {
          double bin = eos_fact[i] /
                       (eos_fact[k] * eos_fact[i - k]);
          acc += a[i] * bin * spow;
          spow *= s;
        }
      q[k] = acc;
    }
}

/* Monomial coefficients (abscissae y = u-u0, z = v-v0) of one surface
 * patch.  smon[((p1+1)*j + i)*3 + d] is the coefficient of y^i z^j. */
static int
eos_surface_monomial(const SISLSurf *s, double u0, double v0,
                     int p1, int p2, double *smon)
{
  int ider = p1 + p2, st = 0, l1 = 0, l2 = 0, i, j;
  double par[2], nrm[3], *der;
  der = (double *)newarray(3 * (ider + 1) * (ider + 2) / 2, double);
  if (der == SISL_NULL) return EOS_MEMORY;
  par[0] = u0;
  par[1] = v0;
  s1421((SISLSurf *)s, ider, par, &l1, &l2, der, nrm, &st);
  if (st < 0)
    {
      freearray(der);
      return EOS_ILLEGAL;
    }
  for (i = 0; i <= p1; ++i)
    for (j = 0; j <= p2; ++j)
      {
        int tot = i + j;
        int idx = (tot * (tot + 1)) / 2 + j;
        double fac = eos_fact[i] * eos_fact[j];
        int d;
        for (d = 0; d < 3; ++d)
          smon[(j * (p1 + 1) + i) * 3 + d] = der[idx * 3 + d] / fac;
      }
  freearray(der);
  return EOS_OK;
}

/* Bivariate Taylor shift: given a patch polynomial in (y,z) centred at
 * a midpoint, return its expansion in (y-su, z-sv), i.e. centred at the
 * piece anchor (u0,v0).  Both arrays use the same packed layout. */
static void
eos_power_shift2(const double *smon, int p1, int p2, double su,
                 double sv, double *out)
{
  int n1 = p1 + 1;
  int i, j, a, b, d;
  for (j = 0; j <= p2; ++j)
    for (i = 0; i <= p1; ++i)
      for (d = 0; d < 3; ++d)
        out[(j * n1 + i) * 3 + d] = 0.0;
  for (a = 0; a <= p1; ++a)
    for (b = 0; b <= p2; ++b)
      {
        double pu[4], pv[4];
        for (i = 0; i <= a; ++i)
          {
            double bin = eos_fact[a] /
                         (eos_fact[i] * eos_fact[a - i]);
            double acc = bin;
            int k;
            for (k = 0; k < a - i; ++k) acc *= su;
            pu[i] = acc;
          }
        for (j = 0; j <= b; ++j)
          {
            double bin = eos_fact[b] /
                         (eos_fact[j] * eos_fact[b - j]);
            double acc = bin;
            int k;
            for (k = 0; k < b - j; ++k) acc *= sv;
            pv[j] = acc;
          }
        for (i = 0; i <= a; ++i)
          for (j = 0; j <= b; ++j)
            for (d = 0; d < 3; ++d)
              out[(j * n1 + i) * 3 + d] +=
                  smon[(b * n1 + a) * 3 + d] * pu[i] * pv[j];
      }
}

/* Precompute the monomial form of every surface patch at the midpoint
 * of its parameter rectangle, where s1421 always sees an interior
 * point.  patches[(jv*npatch1+iu)*n1*n2*3 + ...]. */
static int
eos_precompute_patches(const SISLSurf *s, int p1, int p2,
                       int npatch1, int npatch2, double *patches)
{
  int iu, jv, st;
  double *smon;
  smon = (double *)newarray((p1 + 1) * (p2 + 1) * 3, double);
  if (smon == SISL_NULL) return EOS_MEMORY;
  for (jv = 0; jv < npatch2; ++jv)
    for (iu = 0; iu < npatch1; ++iu)
      {
        double mu = 0.5 * (s->et1[s->ik1 - 1 + iu] +
                           s->et1[s->ik1 + iu]);
        double mv = 0.5 * (s->et2[s->ik2 - 1 + jv] +
                           s->et2[s->ik2 + jv]);
        st = eos_surface_monomial(s, mu, mv, p1, p2, smon);
        if (st != EOS_OK)
          {
            freearray(smon);
            return st;
          }
        memcopy(patches + (jv * npatch1 + iu) * (p1 + 1) *
                            (p2 + 1) * 3,
                smon, (p1 + 1) * (p2 + 1) * 3, double);
      }
  freearray(smon);
  return EOS_OK;
}

/* Locate the patch whose interior contains x.  The caller samples a
 * piece midpoint, and every surface knot preimage is a partition
 * boundary, so x can never fall inside two different pieces.  When x
 * sits exactly on a knot line the right-hand patch is chosen, which is
 * one of the two continuous patches the specification permits. */
static int
eos_locate_patch(const double *et, int ik, int npatch, double x)
{
  int iu;
  if (x <= et[ik - 1]) return 0;
  for (iu = 0; iu < npatch - 1; ++iu)
    if (x < et[ik + iu])
      return iu;
  return npatch - 1;
}

/* Compose one partition piece [lo,hi] lying in uv interval with monomial
 * forms umon/vmon (abscissa t-a).  The surface patch is selected at the
 * piece midpoint so evaluation at a knot line never resolves to the
 * wrong side.  Output q is degree D in t-lo. */
static int
eos_compose_piece(const SISLSurf *s, const double *patches,
                  int npatch1, int npatch2,
                  const double *umon, const double *vmon, double a,
                  double lo, double hi,
                  int p, int p1, int p2, int D, double *q)
{
  int n1 = p1 + 1, n2 = p2 + 1, maxpow = max(p * p1, p * p2);
  double *smon, *ush, *vsh, *uloc, *vloc, *upow, *vpow, *prod;
  double u0, v0, umid, vmid, mu, mv, delta, tmid;
  int i, j, k, d, st = EOS_OK, stride, iu, jv;
  const double *patch;

  smon = (double *)newarray(n1 * n2 * 3, double);
  ush = (double *)newarray(p + 1, double);
  vsh = (double *)newarray(p + 1, double);
  uloc = (double *)newarray(p + 1, double);
  vloc = (double *)newarray(p + 1, double);
  upow = (double *)newarray((maxpow + 1) * n1, double);
  vpow = (double *)newarray((maxpow + 1) * n2, double);
  prod = (double *)newarray(2 * maxpow + 1, double);
  if (smon == SISL_NULL || ush == SISL_NULL || vsh == SISL_NULL ||
      uloc == SISL_NULL || vloc == SISL_NULL || upow == SISL_NULL ||
      vpow == SISL_NULL || prod == SISL_NULL)
    {
      if (smon) freearray(smon);
      if (ush) freearray(ush);
      if (vsh) freearray(vsh);
      if (uloc) freearray(uloc);
      if (vloc) freearray(vloc);
      if (upow) freearray(upow);
      if (vpow) freearray(vpow);
      if (prod) freearray(prod);
      return EOS_MEMORY;
    }

  /* Move the abscissa origin from a to lo and evaluate the entry point
   * of the piece. */
  delta = lo - a;
  eos_power_shift(umon, p, delta, ush);
  eos_power_shift(vmon, p, delta, vsh);
  u0 = ush[0];
  v0 = vsh[0];

  /* Pick the surface patch from an interior sample of the piece. */
  tmid = 0.5 * (lo + hi);
  umid = eos_power_eval(ush, p, lo, tmid);
  vmid = eos_power_eval(vsh, p, lo, tmid);
  iu = eos_locate_patch(s->et1, s->ik1, npatch1, umid);
  jv = eos_locate_patch(s->et2, s->ik2, npatch2, vmid);
  patch = patches + (jv * npatch1 + iu) * n1 * n2 * 3;
  mu = 0.5 * (s->et1[s->ik1 - 1 + iu] + s->et1[s->ik1 + iu]);
  mv = 0.5 * (s->et2[s->ik2 - 1 + jv] + s->et2[s->ik2 + jv]);
  eos_power_shift2(patch, p1, p2, u0 - mu, v0 - mv, smon);

  for (k = 0; k <= p; ++k)
    {
      uloc[k] = (k == 0) ? 0.0 : ush[k];
      vloc[k] = (k == 0) ? 0.0 : vsh[k];
    }

  /* Powers of the local parameter polynomials in power form. */
  stride = maxpow + 1;
  upow[0] = 1.0;
  for (k = 1; k < stride; ++k) upow[k] = 0.0;
  for (i = 1; i <= p1; ++i)
    eos_power_mul(upow + (i - 1) * stride, (i - 1) * p,
                  uloc, p, upow + i * stride);
  vpow[0] = 1.0;
  for (k = 1; k < stride; ++k) vpow[k] = 0.0;
  for (j = 1; j <= p2; ++j)
    eos_power_mul(vpow + (j - 1) * stride, (j - 1) * p,
                  vloc, p, vpow + j * stride);

  for (k = 0; k <= D; ++k)
    for (d = 0; d < 3; ++d)
      q[k * 3 + d] = 0.0;

  for (i = 0; i <= p1; ++i)
    for (j = 0; j <= p2; ++j)
      {
        const double *ui = upow + i * stride;
        const double *vj = vpow + j * stride;
        int di = min(i * p, maxpow);
        int dj = min(j * p, maxpow);
        eos_power_mul(ui, di, vj, dj, prod);
        for (k = 0; k <= min(di + dj, D); ++k)
          {
            double w = prod[k];
            if (w == 0.0) continue;
            for (d = 0; d < 3; ++d)
              q[k * 3 + d] += w * smon[(j * n1 + i) * 3 + d];
          }
      }

  freearray(smon);
  freearray(ush);
  freearray(vsh);
  freearray(uloc);
  freearray(vloc);
  freearray(upow);
  freearray(vpow);
  freearray(prod);
  return st;
}

static int
eos_dcompare(const void *pa, const void *pb)
{
  double a = *(const double *)pa, b = *(const double *)pb;
  return (a > b) - (a < b);
}

#if defined(SISLNEEDPROTOTYPES)
void
sisl_curve_on_surface_exact(SISLSurf *surface, SISLCurve *uv,
                            SISLCurve **result, int *status)
#else
void
sisl_curve_on_surface_exact(surface, uv, result, status)
     SISLSurf *surface;
     SISLCurve *uv;
     SISLCurve **result;
     int *status;
#endif
/*
*********************************************************************
*
* PURPOSE    : Exact polynomial composition C(t) = S(u(t),v(t)).
*              The returned 3-D non-rational B-spline curve is built
*              from coefficients only; no sampling or fitting is used.
*
* INPUT      : surface - 3-D, non-rational clamped tensor-product
*                        B-spline surface, degrees 1-3 in both axes.
*              uv      - 2-D, non-rational clamped B-spline curve,
*                        degree 1-3, whose whole trace lies in the
*                        closed parameter rectangle of the surface.
*
* OUTPUT     : result  - New independently owned 3-D SISLCurve over the
*                        same parameter interval as uv.  Set to NULL on
*                        entry and on every failure.
*              status  - 0 ok, -101 out of memory, -103 illegal
*                        structure/value, -104 unsupported type,
*                        -105 trace outside the surface domain.
*
*********************************************************************
*/
{
  int p, p1, p2, D, order, st = EOS_OK;
  int nint, i, j, k, d, left;
  int nroots, nseg, nknots, ncoef, iscz, interval;
  int npatch1, npatch2, patchsz, rootcap;
  double t0, t1, a, b, lo, hi, kv;
  double *umon = SISL_NULL, *vmon = SISL_NULL;
  double *work = SISL_NULL, *q = SISL_NULL, *patches = SISL_NULL;
  double *breaks = SISL_NULL, *roots = SISL_NULL;
  double *outknots = SISL_NULL, *outcoef = SISL_NULL;
  double *qc = SISL_NULL, *bc = SISL_NULL;
  SISLCurve *curve = SISL_NULL;

  eos_fact[0] = 1.0;
  for (i = 1; i <= 18; ++i) eos_fact[i] = eos_fact[i - 1] * i;

  if (result == SISL_NULL || status == SISL_NULL) return;
  *result = SISL_NULL;
  if (uv == SISL_NULL || surface == SISL_NULL)
    {
      *status = EOS_ILLEGAL;
      return;
    }

  st = eos_validate_surface(surface);
  if (st != EOS_OK) { *status = st; return; }
  st = eos_validate_curve(uv);
  if (st != EOS_OK) { *status = st; return; }

  p = uv->ik - 1;
  p1 = surface->ik1 - 1;
  p2 = surface->ik2 - 1;
  D = p * (p1 + p2);
  order = D + 1;
  t0 = uv->et[uv->ik - 1];
  t1 = uv->et[uv->in];
  nint = uv->in - uv->ik + 1;
  npatch1 = surface->in1 - surface->ik1 + 1;
  npatch2 = surface->in2 - surface->ik2 + 1;
  patchsz = (p1 + 1) * (p2 + 1) * 3;
  /* At most p roots per knot line per uv interval, plus the uv knots. */
  rootcap = 2 + nint +
            nint * p * ((surface->in1 - surface->ik1) +
                        (surface->in2 - surface->ik2)) + 8;
  umon = (double *)newarray(nint * (p + 1), double);
  vmon = (double *)newarray(nint * (p + 1), double);
  roots = (double *)newarray(rootcap, double);
  breaks = (double *)newarray(rootcap, double);
  work = (double *)newarray(p + 1, double);
  patches = (double *)newarray(npatch1 * npatch2 * patchsz, double);
  if (umon == SISL_NULL || vmon == SISL_NULL || roots == SISL_NULL ||
      breaks == SISL_NULL || work == SISL_NULL || patches == SISL_NULL)
    goto err101;

  st = eos_precompute_patches(surface, p1, p2, npatch1, npatch2,
                              patches);
  if (st != EOS_OK) goto fail;

  /* Monomial form of every uv interval, and a real-range containment
   * test of the trace on each interval. */
  left = uv->ik - 1;
  for (i = 0; i < nint; ++i)
    {
      a = uv->et[uv->ik - 1 + i];
      b = uv->et[uv->ik + i];
      st = eos_curve_monomial(uv, 0, left, a, umon + i * (p + 1));
      if (st != EOS_OK) goto fail;
      st = eos_curve_monomial(uv, 1, left, a, vmon + i * (p + 1));
      if (st != EOS_OK) goto fail;
      st = eos_check_containment(umon + i * (p + 1), p, a, b,
                                 surface->et1[surface->ik1 - 1],
                                 surface->et1[surface->in1]);
      if (st != EOS_OK) goto fail;
      st = eos_check_containment(vmon + i * (p + 1), p, a, b,
                                 surface->et2[surface->ik2 - 1],
                                 surface->et2[surface->in2]);
      if (st != EOS_OK) goto fail;
      ++left;
    }

  /* Partition times: every distinct uv knot value and every isolated
   * contact with an internal surface knot line in either parameter
   * axis. */
  nroots = 0;
  st = eos_add_root(t0, roots, &nroots, rootcap);
  if (st != EOS_OK) goto err101;
  st = eos_add_root(t1, roots, &nroots, rootcap);
  if (st != EOS_OK) goto err101;
  for (i = uv->ik - 1; i <= uv->in; ++i)
    {
      st = eos_add_root(uv->et[i], roots, &nroots, rootcap);
      if (st != EOS_OK) goto err101;
    }

  for (i = 0; i < nint; ++i)
    {
      a = uv->et[uv->ik - 1 + i];
      b = uv->et[uv->ik + i];

      k = surface->ik1;
      while (k < surface->in1)
        {
          int k2 = k;
          kv = surface->et1[k];
          while (k2 < surface->in1 && surface->et1[k2] == kv) ++k2;
          for (j = 0; j <= p; ++j) work[j] = umon[i * (p + 1) + j];
          work[0] -= kv;
          st = eos_roots_of(work, p, a, b, &iscz, roots, &nroots,
                            rootcap);
          if (st != EOS_OK) goto fail;
          /* iscz != 0: the whole interval follows this knot line.
           * Its ends already bound the piece, so no split is added. */
          k = k2;
        }

      k = surface->ik2;
      while (k < surface->in2)
        {
          int k2 = k;
          kv = surface->et2[k];
          while (k2 < surface->in2 && surface->et2[k2] == kv) ++k2;
          for (j = 0; j <= p; ++j) work[j] = vmon[i * (p + 1) + j];
          work[0] -= kv;
          st = eos_roots_of(work, p, a, b, &iscz, roots, &nroots,
                            rootcap);
          if (st != EOS_OK) goto fail;
          k = k2;
        }
    }

  for (i = 0; i < nroots; ++i) breaks[i] = roots[i];
  qsort(breaks, nroots, sizeof(double), eos_dcompare);
  nseg = nroots - 1;

  nknots = 2 * order + (nseg - 1) * (order - 1);
  ncoef = nknots - order;

  outknots = (double *)newarray(nknots, double);
  outcoef = (double *)newarray(ncoef * 3, double);
  q = (double *)newarray((D + 1) * 3, double);
  qc = (double *)newarray(D + 1, double);
  bc = (double *)newarray(D + 1, double);
  if (outknots == SISL_NULL || outcoef == SISL_NULL ||
      q == SISL_NULL || qc == SISL_NULL || bc == SISL_NULL)
    goto err101;

  /* Fully repeated knots: each piece is an independent Bernstein
   * segment, imposing no continuity the composite does not have. */
  k = 0;
  for (j = 0; j < order; ++j) outknots[k++] = breaks[0];
  for (i = 1; i < nseg; ++i)
    for (j = 0; j < order - 1; ++j) outknots[k++] = breaks[i];
  for (j = 0; j < order; ++j) outknots[k++] = breaks[nseg];

  for (i = 0; i < nseg; ++i)
    {
      lo = breaks[i];
      hi = breaks[i + 1];

      interval = 0;
      for (j = 0; j < nint; ++j)
        if (uv->et[uv->ik - 1 + j] <= lo + EOS_MERGE_TOL &&
            hi <= uv->et[uv->ik + j] + EOS_MERGE_TOL)
          {
            interval = j;
            break;
          }
      a = uv->et[uv->ik - 1 + interval];

      st = eos_compose_piece(surface, patches, npatch1, npatch2,
                             umon + interval * (p + 1),
                             vmon + interval * (p + 1),
                             a, lo, hi, p, p1, p2, D, q);
      if (st != EOS_OK) goto fail;

      for (d = 0; d < 3; ++d)
        {
          for (j = 0; j <= D; ++j) qc[j] = q[j * 3 + d];
          eos_power_to_bernstein(qc, D, lo, hi, bc);
          for (j = 0; j <= D; ++j)
            outcoef[(i * D + j) * 3 + d] = bc[j];
        }
    }

  /* Force the joints to a single stored value so adjacent Bernstein
   * segments cannot develop a numerical crack. */
  for (i = 1; i < nseg; ++i)
    for (d = 0; d < 3; ++d)
      outcoef[(i * D) * 3 + d] = outcoef[((i - 1) * D + D) * 3 + d];

  curve = newCurve(ncoef, order, outknots, outcoef, 1, 3, 1);
  if (curve == SISL_NULL) goto err101;
  curve->cuopen = SISL_CRV_OPEN;

  *result = curve;
  *status = EOS_OK;

  freearray(umon);
  freearray(vmon);
  freearray(roots);
  freearray(breaks);
  freearray(work);
  freearray(patches);
  freearray(outknots);
  freearray(outcoef);
  freearray(q);
  freearray(qc);
  freearray(bc);
  return;

err101:
  st = EOS_MEMORY;
fail:
  if (curve != SISL_NULL) freeCurve(curve);
  if (umon) freearray(umon);
  if (vmon) freearray(vmon);
  if (roots) freearray(roots);
  if (breaks) freearray(breaks);
  if (work) freearray(work);
  if (patches) freearray(patches);
  if (outknots) freearray(outknots);
  if (outcoef) freearray(outcoef);
  if (q) freearray(q);
  if (qc) freearray(qc);
  if (bc) freearray(bc);
  *result = SISL_NULL;
  *status = st;
}
