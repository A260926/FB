/*
 * Offline regression for the SISL plane-section path s1851 (topology) +
 * s1314 (marching).  Every input surface is generated in this file from an
 * independent analytic description; no windowing code, external test
 * framework or downloaded material is involved.
 *
 * The analytic models, the valid parameter ranges and the expected
 * connected components live in the Case table below and never depend on
 * the ordering or orientation of the curves returned by SISL.
 */
#include "section-qa.h"
#include "sisl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Number of samples drawn from every returned curve's own parameter span. */
#define QA_NSAMPLE 200

/* ====================================================================== */
/* Analytic ground truth                                                   */
/* ====================================================================== */
enum { FAM_TWOLINES = 0, FAM_CIRCLE = 1, FAM_CYLINDER = 2, FAM_NOHIT = 3 };
enum { VAR_BASE = 0, VAR_OFFSET = 1, VAR_SCALED = 2, VAR_REPARAM = 3 };

typedef struct {
  int family;
  const char *tag;
  double L;                 /* physical length scale of the model */
  double rmin, rmax, smin, smax, du, dv; /* spline parameter domain */
  double scale, trans[3];   /* physical point x = scale*base + trans */
  double bpoint[3], bnorm[3]; /* plane in base coordinates */
  double ppoint[3], pnorm[3], pconst; /* plane n.x + d = 0, physical */
  double h;                 /* graph plane offset (base space) */
  double line_x[2];         /* expected base-x roots of the two lines */
  double circle_r;          /* expected analytic circle radius (base) */
  double radius, th0, th1, zlo, zhi;   /* cylinder band (base space) */
} Case;

/* Base-space surface, independent of any SISL object. */
static void base_surface(const Case *c, double u, double v, double x[3])
{
  if (c->family == FAM_TWOLINES) {
    x[0] = u;
    x[1] = v;
    x[2] = (u - 0.25) * (u - 0.75);
  } else if (c->family == FAM_CIRCLE || c->family == FAM_NOHIT) {
    x[0] = u;
    x[1] = v;
    x[2] = (u - 0.5) * (u - 0.5) + (v - 0.5) * (v - 0.5) - 0.16;
  } else {
    double th = c->th0 + (c->th1 - c->th0) * u;
    x[0] = c->radius * cos(th);
    x[1] = c->radius * sin(th);
    x[2] = c->zlo + (c->zhi - c->zlo) * v;
  }
}

/* Analytic surface evaluated at spline parameters (r, s). */
static void analytic_surface(const Case *c, double r, double s, double x[3])
{
  double u = (r - c->rmin) / c->du;
  double v = (s - c->smin) / c->dv;
  double b[3];
  base_surface(c, u, v, b);
  for (int k = 0; k < 3; k++) x[k] = c->scale * b[k] + c->trans[k];
}

static double plane_norm(const Case *c)
{
  return sqrt(c->pnorm[0] * c->pnorm[0] + c->pnorm[1] * c->pnorm[1] +
              c->pnorm[2] * c->pnorm[2]);
}

/* Signed, normalized residual of the independent plane equation. */
static double plane_residual(const Case *c, const double x[3])
{
  double val = c->pnorm[0] * x[0] + c->pnorm[1] * x[1] +
               c->pnorm[2] * x[2] + c->pconst;
  return val / plane_norm(c);
}

/* Residual of the independent surface equation (physical space). */
static double surface_residual(const Case *c, const double x[3])
{
  double b[3];
  for (int k = 0; k < 3; k++) b[k] = (x[k] - c->trans[k]) / c->scale;
  if (c->family == FAM_TWOLINES)
    return (b[0] - 0.25) * (b[0] - 0.75) - b[2];
  if (c->family == FAM_CIRCLE || c->family == FAM_NOHIT)
    return (b[0] - 0.5) * (b[0] - 0.5) + (b[1] - 0.5) * (b[1] - 0.5) -
           0.16 - b[2];
  return b[0] * b[0] + b[1] * b[1] - c->radius * c->radius;
}

/* Map a base-space plane through x = scale*b + trans. */
static void install_plane(Case *c)
{
  for (int k = 0; k < 3; k++) c->pnorm[k] = c->bnorm[k] / c->scale;
  double base_d = -(c->bnorm[0] * c->bpoint[0] + c->bnorm[1] * c->bpoint[1] +
                    c->bnorm[2] * c->bpoint[2]);
  c->pconst = base_d - (c->pnorm[0] * c->trans[0] +
                        c->pnorm[1] * c->trans[1] +
                        c->pnorm[2] * c->trans[2]);
  for (int k = 0; k < 3; k++)
    c->ppoint[k] = c->scale * c->bpoint[k] + c->trans[k];
}

static void make_case(Case *c, int family, int variant)
{
  memset(c, 0, sizeof(*c));
  c->family = family;
  c->L = 1.0;
  c->scale = 1.0;
  c->rmin = c->smin = 0.0;
  c->rmax = c->smax = 1.0;
  c->du = c->dv = 1.0;
  c->tag = family == FAM_TWOLINES ? "twolines"
          : family == FAM_CIRCLE  ? "circle"
          : family == FAM_NOHIT   ? "nohit"
                                  : "cylinder";

  if (family == FAM_TWOLINES) {
    c->h = (variant == VAR_OFFSET) ? 0.02 : 0.0;
    c->line_x[0] = 0.5 - sqrt(0.0625 + c->h);
    c->line_x[1] = 0.5 + sqrt(0.0625 + c->h);
    c->bpoint[2] = c->h;
    c->bnorm[2] = 1.0;
  } else if (family == FAM_CIRCLE) {
    c->h = (variant == VAR_OFFSET) ? -0.07 : 0.0;
    c->circle_r = sqrt(0.16 + c->h);
    c->bpoint[2] = c->h;
    c->bnorm[2] = 1.0;
  } else if (family == FAM_NOHIT) {
    /* Paraboloid max on the patch is 0.34; plane at 0.4 cannot meet it. */
    c->circle_r = 0.0;
    c->bpoint[2] = 0.4;
    c->bnorm[2] = 1.0;
  } else {
    /* 270-degree open band, no periodic seam inside the parameter box. */
    c->radius = 1.0;
    c->th0 = -0.75 * M_PI;
    c->th1 = 0.75 * M_PI;
    c->zlo = 0.0;
    c->zhi = 2.0;
    c->bpoint[0] = 0.0;
    c->bpoint[2] = (variant == VAR_OFFSET) ? 0.7 : 0.4;
    c->bnorm[0] = -0.3;
    c->bnorm[2] = 1.0;
  }

  if (variant == VAR_SCALED) {
    c->scale = 2.5;
    c->trans[0] = 0.7;
    c->trans[1] = -0.4;
    c->trans[2] = 1.1;
    c->L = 2.5;
  }
  if (variant == VAR_REPARAM) {
    c->rmin = 1.3;
    c->rmax = 4.7;
    c->smin = -2.1;
    c->smax = 0.9;
    c->du = c->rmax - c->rmin;
    c->dv = c->smax - c->smin;
  }
  install_plane(c);
}

/* ====================================================================== */
/* Exact B-spline surface builders                                         */
/* ====================================================================== */
/* Quadratic blossom coefficient of q(t)=A t^2+B t+C on knot pair (t1,t2). */
static void blossom_quad(int n, const double *t, double A, double B, double C,
                         double *out)
{
  for (int i = 0; i < n; i++)
    out[i] = A * t[i + 1] * t[i + 2] + 0.5 * B * (t[i + 1] + t[i + 2]) + C;
}
/* Linear blossom coefficient of q(t)=a t+b. */
static void blossom_lin(int n, const double *t, double a, double b,
                        double *out)
{
  for (int i = 0; i < n; i++) out[i] = a * t[i + 1] + b;
}

/*
 * Polynomial graph surfaces.  The coefficients are taken from blossom
 * values of the exact coordinate polynomials, so the spline reproduces
 * the analytic surface up to round-off, including over non-unit domains.
 */
static SISLSurf *build_graph(const Case *c)
{
  int ku, kv, nu, nv;
  if (c->family == FAM_TWOLINES) {
    ku = 3; kv = 2; nu = 3; nv = 2;
  } else {
    /* Interior knots are required: a single Bezier patch cannot expose a
     * zero set that is a closed loop strictly inside the parameter box. */
    ku = 3; kv = 3; nu = 5; nv = 5;
  }
  double *uk = (double *)malloc(sizeof(double) * (nu + ku));
  double *vk = (double *)malloc(sizeof(double) * (nv + kv));
  if (!uk || !vk) {
    free(uk);
    free(vk);
    return (SISLSurf *)0;
  }
  for (int i = 0; i < ku; i++) uk[i] = c->rmin;
  for (int i = 0; i < nu - ku; i++)
    uk[ku + i] = c->rmin + c->du * (i + 1.0) / (nu - ku + 1);
  for (int i = 0; i < ku; i++) uk[nu + i] = c->rmax;
  for (int i = 0; i < kv; i++) vk[i] = c->smin;
  for (int i = 0; i < nv - kv; i++)
    vk[kv + i] = c->smin + c->dv * (i + 1.0) / (nv - kv + 1);
  for (int i = 0; i < kv; i++) vk[nv + i] = c->smax;

  double fu[16], fv[16];
  for (int i = 0; i < nu + ku; i++) fu[i] = (uk[i] - c->rmin) / c->du;
  for (int i = 0; i < nv + kv; i++) fv[i] = (vk[i] - c->smin) / c->dv;

  double bx[16], by[16];
  blossom_quad(nu, fu, 0.0, 1.0, 0.0, bx);             /* base x = u */
  if (c->family == FAM_TWOLINES)
    blossom_lin(nv, fv, 1.0, 0.0, by);                 /* base y = v */
  else
    blossom_quad(nv, fv, 0.0, 1.0, 0.0, by);

  double *coef = (double *)malloc(sizeof(double) * nu * nv * 3);
  if (!coef) {
    free(uk);
    free(vk);
    return (SISLSurf *)0;
  }
  for (int j = 0; j < nv; j++) {
    for (int i = 0; i < nu; i++) {
      double bz;
      if (c->family == FAM_TWOLINES) {
        double zu[16];
        blossom_quad(nu, fu, 1.0, -1.0, 0.1875, zu);  /* (u-.25)(u-.75) */
        bz = zu[i];
      } else {
        double zu[16], zv[16];
        blossom_quad(nu, fu, 1.0, -1.0, 0.0, zu);     /* u^2 - u */
        blossom_quad(nv, fv, 1.0, -1.0, 0.34, zv);    /* v^2 - v + .34 */
        bz = zu[i] + zv[j];
      }
      double base[3] = { bx[i], by[j], bz };
      int k = (j * nu + i) * 3;
      for (int d = 0; d < 3; d++)
        coef[k + d] = c->scale * base[d] + c->trans[d];
    }
  }
  SISLSurf *sf = newSurf(nu, nv, ku, kv, uk, vk, coef, 1, 3, 1);
  free(uk);
  free(vk);
  free(coef);
  return sf;
}

/*
 * Rational NURBS cylinder band.  Three quadratic circular arcs (each
 * 90 degrees) with strictly positive weights; the axis direction is
 * linear in the second parameter.  The homogeneous xy coordinates hold
 * R*(cos,sin), which makes the odd middle control points sit at the
 * tangent intersection R/w; the similarity transform enters through
 * the weight and the translation term.
 */
static SISLSurf *build_cylinder(const Case *c)
{
  int m = 3, nu = 2 * m + 1, ku = 3, kv = 2, nv = 2;
  double *uk = (double *)malloc(sizeof(double) * (nu + ku));
  double vk[4] = { c->smin, c->smin, c->smax, c->smax };
  if (!uk) return (SISLSurf *)0;  uk[0] = uk[1] = uk[2] = c->rmin;
  for (int i = 1; i < m; i++) {
    double val = c->rmin + c->du * ((double)i / m);
    uk[1 + 2 * i] = val;
    uk[2 + 2 * i] = val;
  }
  uk[nu] = uk[nu + 1] = uk[nu + 2] = c->rmax;

  double *coef = (double *)malloc(sizeof(double) * nu * nv * 4);
  if (!coef) {
    free(uk);
    return (SISLSurf *)0;
  }
  double dth = c->th1 - c->th0;
  for (int i = 0; i < nu; i++) {
    double f = (double)i / (nu - 1);
    double th = c->th0 + dth * f;
    double w = (i % 2 == 0) ? 1.0 : cos(0.5 * dth / m);
    for (int j = 0; j < nv; j++) {
      double z = c->zlo + (c->zhi - c->zlo) * j;
      int q = (j * nu + i) * 4;
      coef[q]     = c->scale * c->radius * cos(th) + w * c->trans[0];
      coef[q + 1] = c->scale * c->radius * sin(th) + w * c->trans[1];
      coef[q + 2] = w * (c->scale * z + c->trans[2]);
      coef[q + 3] = w;
    }
  }
  SISLSurf *sf = newSurf(nu, nv, ku, kv, uk, vk, coef, 2, 3, 1);
  free(uk);
  free(coef);
  return sf;
}

static SISLSurf *build_surface(const Case *c)
{
  return (c->family == FAM_CYLINDER) ? build_cylinder(c) : build_graph(c);
}

/* ====================================================================== */
/* Evaluation helpers                                                      */
/* ====================================================================== */
static int eval_surface(SISLSurf *sf, double r, double s, double x[3])
{
  double der[3];
  int left1 = 0, left2 = 0, st = 0;
  s1424(sf, 0, 0, (double[2]){ r, s }, &left1, &left2, der, &st);
  if (st < 0) {
    fprintf(stderr, "QA_EVAL_SURFACE_FAILED status=%d r=%.6f s=%.6f\n",
            st, r, s);
    return st;
  }
  for (int k = 0; k < 3; k++) x[k] = der[k];
  return 0;
}
static int eval_curve(SISLCurve *cv, double t, double x[])
{
  int left = 0, st = 0;
  s1227(cv, 0, t, &left, x, &st);
  if (st < 0) {
    fprintf(stderr, "QA_EVAL_CURVE_FAILED status=%d t=%.6f\n", st, t);
    return st;
  }
  return 0;
}
static double curve_tmin(SISLCurve *cv) { return cv->et[cv->ik - 1]; }
static double curve_tmax(SISLCurve *cv) { return cv->et[cv->in]; }

/* ====================================================================== */
/* s1851 + s1314 driver                                                     */
/* ====================================================================== */
typedef struct {
  int ncurve, npoint, topo_status, march_status;
  SISLIntcurve **curves;
  double *uv;
} SectionResult;

/*
 * Precision budget (see checks/sections.md):
 *   epsge = 1e-8 * L  : geometry resolution handed to s1851/s1314
 *   epsco = 1e-12     : computational resolution (parameter domain scale)
 *   amax  = 0.01 * L  : maximal marching step, scales with the model
 * plane_bias is nonzero only for the deliberately mutated library copy.
 */
static int run_section(const Case *c, SISLSurf *sf, double plane_bias,
                       SectionResult *r)
{
  r->curves = (SISLIntcurve **)0;
  r->uv = (double *)0;
  r->ncurve = r->npoint = r->march_status = 0;
  double epsco = 1e-12;
  double epsge = 1e-8 * c->L;
  double amax = 0.01 * c->L;

  s1851(sf, (double *)c->ppoint, (double *)c->pnorm, 3, epsco, epsge,
        &r->npoint, &r->uv, &r->ncurve, &r->curves, &r->topo_status);
  if (r->topo_status != 0 || (r->ncurve > 0 && !r->curves))
    return -1;

  double epoint[3];
  for (int k = 0; k < 3; k++) epoint[k] = c->ppoint[k];
  if (plane_bias != 0.0) {
    double nl = plane_norm(c);
    for (int k = 0; k < 3; k++)
      epoint[k] = c->ppoint[k] - plane_bias * c->pnorm[k] / nl;
  }

  for (int i = 0; i < r->ncurve; i++) {
    s1314(sf, epoint, (double *)c->pnorm, 3, epsco, epsge, amax,
          r->curves[i], 2, 0, &r->march_status);
    if (r->march_status != 0 || !r->curves[i]->pgeom ||
        !r->curves[i]->ppar1)
      return -2;
  }
  return 0;
}
static void free_section(SectionResult *r)
{
  freeIntcrvlist(r->curves, r->ncurve);
  free(r->uv);
}

/* Measured quantities reported for every callback. */
typedef struct {
  int ncurve, itype;
  double max_pair, max_model, max_plane_geom, max_plane_par, max_surf_geom,
         max_surf_par, coverage_metric;
} Observations;

static void zero_obs(Observations *o)
{
  memset(o, 0, sizeof(*o));
  o->itype = -1;
}

/* ====================================================================== */
/* Assertion group 1: topology (connected component counts)                */
/* ====================================================================== */
static int expected_components(const Case *c)
{
  if (c->family == FAM_NOHIT) return 0;
  if (c->family == FAM_TWOLINES) return 2;
  return 1;
}

static int topology_assert(const Case *c, const SectionResult *r,
                           Observations *o)
{
  o->ncurve = r->ncurve;
  if (r->npoint != 0) return 1;
  return r->ncurve == expected_components(c) ? 0 : 2;
}

/* ====================================================================== */
/* Assertion group 2: paired geometric/parameter curves                    */
/* ====================================================================== */
/*
 * The two curves are fitted independently, so the pairing budget is not
 * the intersection tolerance.  Values were fixed after the marching-step
 * convergence study documented in checks/sections.md.
 */
static int paired_assert(SISLSurf *sf, const Case *c, const SectionResult *r,
                         Observations *o)
{
  double tol_pair = 2e-5 * c->L;
  double tol_plane = 1e-7 * c->L;
  /* The spline reproduces the analytic models exactly, so points obtained
   * by back-substitution (xs) get a tight analytic-surface budget.  The
   * geometric curve is fitted independently, hence its own looser budget. */
  double tol_surf_par = 1e-6 * c->L;
  double tol_surf_geom = 2e-5 * c->L;
  int bad = 0;
  o->ncurve = r->ncurve;
  for (int b = 0; b < r->ncurve; b++) {
    SISLCurve *g = r->curves[b]->pgeom;
    SISLCurve *q = r->curves[b]->ppar1;
    if (b == 0) o->itype = r->curves[b]->itype;
    double t0 = curve_tmin(q), t1 = curve_tmax(q);
    for (int i = 0; i <= QA_NSAMPLE; i++) {
      double t = t0 + (t1 - t0) * i / QA_NSAMPLE;
      double xg[3], uv[2], xs[3], xa[3];
      if (eval_curve(g, t, xg) || eval_curve(q, t, uv) ||
          eval_surface(sf, uv[0], uv[1], xs)) {
        bad = 1;
        continue;
      }
      /* For the polynomial graphs the spline parametrization reproduces the
       * analytic one exactly, so equal-parameter positions must coincide.
       * The rational cylinder has an exact but nonlinear parameter-to-angle
       * map; there its independent check is the cylinder surface equation,
       * which is evaluated below for every family. */
      double d2 = 0.0, model = 0.0;
      if (c->family != FAM_CYLINDER) {
        analytic_surface(c, uv[0], uv[1], xa);
        double d2a = 0.0;
        for (int k = 0; k < 3; k++) {
          double dz = xg[k] - xs[k];
          double da = xs[k] - xa[k];
          d2 += dz * dz;
          d2a += da * da;
        }
        model = sqrt(d2a);
      } else {
        for (int k = 0; k < 3; k++) {
          double dz = xg[k] - xs[k];
          d2 += dz * dz;
        }
      }
      double pair = sqrt(d2);
      double pg = fabs(plane_residual(c, xg));
      double pp = fabs(plane_residual(c, xs));
      double sg = fabs(surface_residual(c, xg));
      double sp = fabs(surface_residual(c, xs));
      if (pair > o->max_pair) o->max_pair = pair;
      if (model > o->max_model) o->max_model = model;
      if (pg > o->max_plane_geom) o->max_plane_geom = pg;
      if (pp > o->max_plane_par) o->max_plane_par = pp;
      if (sg > o->max_surf_geom) o->max_surf_geom = sg;
      if (sp > o->max_surf_par) o->max_surf_par = sp;
      if (pair > tol_pair || model > tol_surf_par || pg > tol_plane ||
          pp > tol_plane || sg > tol_surf_geom || sp > tol_surf_par)
        bad = 1;
    }
  }
  return bad;
}

/* ====================================================================== */
/* Assertion group 3: analytic coverage                                    */
/* ====================================================================== */
static int coverage_twolines(const Case *c, const SectionResult *r,
                             Observations *o)
{
  if (r->ncurve != 2) return 1;
  int hit[2] = { 0, 0 };
  for (int b = 0; b < 2; b++) {
    SISLCurve *q = r->curves[b]->ppar1;
    double t0 = curve_tmin(q), t1 = curve_tmax(q);
    double e0[2], e1[2], mid[2];
    if (eval_curve(q, t0, e0) || eval_curve(q, t1, e1) ||
        eval_curve(q, 0.5 * (t0 + t1), mid))
      return 5;
    double base_u = (mid[0] - c->rmin) / c->du;
    int idx = fabs(base_u - c->line_x[0]) < fabs(base_u - c->line_x[1]) ? 0 : 1;
    if (hit[idx]) return 2;
    hit[idx] = 1;
    double vlo = (fmin(e0[1], e1[1]) - c->smin) / c->dv;
    double vhi = (fmax(e0[1], e1[1]) - c->smin) / c->dv;
    double err = fmax(fabs(vlo), fabs(1.0 - vhi));
    if (err > o->coverage_metric) o->coverage_metric = err;
    /* Each line runs from one parameter boundary to the other. */
    if (vlo > 1e-3 || vhi < 1.0 - 1e-3) return 3;
  }
  if (!hit[0] || !hit[1]) return 4;
  return 0;
}

static int coverage_circle(const Case *c, const SectionResult *r,
                           Observations *o)
{
  if (r->ncurve != 1) return 1;
  SISLCurve *g = r->curves[0]->pgeom;
  SISLCurve *q = r->curves[0]->ppar1;
  double t0 = curve_tmin(q), t1 = curve_tmax(q);
  double x0[3], x1[3], u0[2], u1[2];
  if (eval_curve(g, t0, x0) || eval_curve(g, t1, x1) ||
      eval_curve(q, t0, u0) || eval_curve(q, t1, u1))
    return 4;
  double geom_close = 0.0, par_close = 0.0;
  for (int k = 0; k < 3; k++) {
    double dz = x0[k] - x1[k];
    geom_close += dz * dz;
  }
  par_close = sqrt((u0[0] - u1[0]) * (u0[0] - u1[0]) +
                   (u0[1] - u1[1]) * (u0[1] - u1[1]));
  geom_close = sqrt(geom_close);
  if (geom_close > 1e-3 * c->L || par_close > 1e-3 * c->du) {
    o->coverage_metric = geom_close;
    return 2;
  }
  double ang[QA_NSAMPLE + 1];
  for (int i = 0; i <= QA_NSAMPLE; i++) {
    double uv[2];
    if (eval_curve(q, t0 + (t1 - t0) * i / QA_NSAMPLE, uv)) return 4;
    double bu = (uv[0] - c->rmin) / c->du;
    double bv = (uv[1] - c->smin) / c->dv;
    ang[i] = atan2(bv - 0.5, bu - 0.5);
    if (ang[i] < 0.0) ang[i] += 2.0 * M_PI;
  }
  for (int i = 0; i <= QA_NSAMPLE; i++)
    for (int j = i + 1; j <= QA_NSAMPLE; j++)
      if (ang[j] < ang[i]) {
        double tmp = ang[i];
        ang[i] = ang[j];
        ang[j] = tmp;
      }
  double gap = 0.0;
  for (int i = 1; i <= QA_NSAMPLE; i++)
    if (ang[i] - ang[i - 1] > gap) gap = ang[i] - ang[i - 1];
  gap = fmax(gap, ang[0] + 2.0 * M_PI - ang[QA_NSAMPLE]);
  o->coverage_metric = gap;
  /* A full circle leaves no angular gap larger than one sixth of a turn. */
  return gap > 2.0 * M_PI / 6.0 ? 3 : 0;
}

static int coverage_cylinder(SISLSurf *sf, const Case *c,
                             const SectionResult *r, Observations *o)
{
  if (r->ncurve != 1) return 1;
  SISLCurve *q = r->curves[0]->ppar1;
  double t0 = curve_tmin(q), t1 = curve_tmax(q);
  double th[QA_NSAMPLE + 1];
  for (int i = 0; i <= QA_NSAMPLE; i++) {
    double uv[2], x[3];
    if (eval_curve(q, t0 + (t1 - t0) * i / QA_NSAMPLE, uv) ||
        eval_surface(sf, uv[0], uv[1], x))
      return 4;
    double bx = (x[0] - c->trans[0]) / c->scale;
    double by = (x[1] - c->trans[1]) / c->scale;
    th[i] = atan2(by, bx);
  }
  /* Sequential unwrap: consecutive samples stay close across the +/-pi
   * cut, and the band itself never reaches the periodic seam. */
  for (int i = 1; i <= QA_NSAMPLE; i++) {
    double d = th[i] - th[i - 1];
    if (d > M_PI) d -= 2.0 * M_PI;
    else if (d < -M_PI) d += 2.0 * M_PI;
    th[i] = th[i - 1] + d;
  }
  double lo = th[0], hi = th[QA_NSAMPLE];
  if (lo > hi) {
    double tmp = lo;
    lo = hi;
    hi = tmp;
  }
  double sorted[QA_NSAMPLE + 1];
  for (int i = 0; i <= QA_NSAMPLE; i++) sorted[i] = th[i];
  for (int i = 0; i <= QA_NSAMPLE; i++)
    for (int j = i + 1; j <= QA_NSAMPLE; j++)
      if (sorted[j] < sorted[i]) {
        double tmp = sorted[i];
        sorted[i] = sorted[j];
        sorted[j] = tmp;
      }
  double gap = 0.0;
  for (int i = 1; i <= QA_NSAMPLE; i++)
    if (sorted[i] - sorted[i - 1] > gap) gap = sorted[i] - sorted[i - 1];
  double enderr = fmax(fabs(lo - c->th0), fabs(hi - c->th1));
  o->coverage_metric = fmax(gap, enderr);
  double arc = c->th1 - c->th0;
  /* The whole analytic arc must be present; returning only a segment of
   * it violates the endpoint and gap budgets below. */
  return (gap > arc / 6.0 || enderr > arc / 50.0) ? 2 : 0;
}

static int coverage_assert(SISLSurf *sf, const Case *c,
                           const SectionResult *r, Observations *o)
{
  o->ncurve = r->ncurve;
  if (r->ncurve > 0) o->itype = r->curves[0]->itype;
  if (c->family == FAM_TWOLINES) return coverage_twolines(c, r, o);
  if (c->family == FAM_CIRCLE) return coverage_circle(c, r, o);
  if (c->family == FAM_CYLINDER) return coverage_cylinder(sf, c, r, o);
  return 1;
}

/* ====================================================================== */
/* Callback plumbing                                                       */
/* ====================================================================== */
static void print_obs(const char *group, const char *name,
                      const Observations *o)
{
  printf("OBS group=%s name=%s nc=%d itype=%d pair=%.6e model=%.6e "
         "plane-geom=%.6e plane-par=%.6e surf-geom=%.6e surf-par=%.6e "
         "coverage=%.6e\n",
         group, name, o->ncurve, o->itype, o->max_pair, o->max_model,
         o->max_plane_geom, o->max_plane_par, o->max_surf_geom,
         o->max_surf_par, o->coverage_metric);
  fflush(stdout);
}

static int run_topology(int family, int variant, const char *name)
{
  Case c;
  make_case(&c, family, variant);
  SISLSurf *sf = build_surface(&c);
  if (!sf) {
    fprintf(stderr, "QA_BUILD_FAILED %s\n", name);
    return 1;
  }
  SectionResult r;
  Observations o;
  zero_obs(&o);
  int rc = run_section(&c, sf, 0.0, &r);
  if (rc == 0) rc = topology_assert(&c, &r, &o);
  print_obs("topology", name, &o);
  if (rc)
    fprintf(stderr,
            "QA_FAIL topology name=%s rc=%d topo_status=%d march_status=%d "
            "nc=%d expected=%d\n",
            name, rc, r.topo_status, r.march_status, r.ncurve,
            expected_components(&c));
  free_section(&r);
  freeSurf(sf);
  return rc ? 1 : 0;
}

static int run_paired(int family, int variant, const char *name)
{
  Case c;
  make_case(&c, family, variant);
  SISLSurf *sf = build_surface(&c);
  if (!sf) {
    fprintf(stderr, "QA_BUILD_FAILED %s\n", name);
    return 1;
  }
  SectionResult r;
  Observations o;
  zero_obs(&o);
  int rc = run_section(&c, sf, 0.0, &r);
  if (rc == 0) rc = paired_assert(sf, &c, &r, &o);
  print_obs("paired-curves", name, &o);
  if (rc)
    fprintf(stderr,
            "QA_FAIL paired-curves name=%s rc=%d topo_status=%d "
            "march_status=%d nc=%d pair=%.6e model=%.6e plane-geom=%.6e "
            "surf-geom=%.6e surf-par=%.6e\n",
            name, rc, r.topo_status, r.march_status, r.ncurve, o.max_pair,
            o.max_model, o.max_plane_geom, o.max_surf_geom,
            o.max_surf_par);
  free_section(&r);
  freeSurf(sf);
  return rc ? 1 : 0;
}

static int run_coverage(int family, int variant, const char *name)
{
  Case c;
  make_case(&c, family, variant);
  SISLSurf *sf = build_surface(&c);
  if (!sf) {
    fprintf(stderr, "QA_BUILD_FAILED %s\n", name);
    return 1;
  }
  SectionResult r;
  Observations o;
  zero_obs(&o);
  int rc = run_section(&c, sf, 0.0, &r);
  if (rc == 0) rc = coverage_assert(sf, &c, &r, &o);
  print_obs("analytic-coverage", name, &o);
  if (rc)
    fprintf(stderr,
            "QA_FAIL analytic-coverage name=%s rc=%d topo_status=%d "
            "march_status=%d nc=%d coverage=%.6e\n",
            name, rc, r.topo_status, r.march_status, r.ncurve,
            o.coverage_metric);
  free_section(&r);
  freeSurf(sf);
  return rc ? 1 : 0;
}

/* Distinct named C callbacks.  Names carry the model and the transform so
 * the output does not rely on the order in which curves come back. */
#define TOPO_CB(id, fam, var, label)                                       \
  static int topo_##id(void) { return run_topology(fam, var, label); }
#define PAIRED_CB(id, fam, var, label)                                     \
  static int paired_##id(void) { return run_paired(fam, var, label); }
#define COVER_CB(id, fam, var, label)                                      \
  static int cover_##id(void) { return run_coverage(fam, var, label); }

TOPO_CB(twolines_base, FAM_TWOLINES, VAR_BASE, "twolines-base")
TOPO_CB(twolines_offset, FAM_TWOLINES, VAR_OFFSET, "twolines-offset")
TOPO_CB(twolines_scaled, FAM_TWOLINES, VAR_SCALED, "twolines-scaled")
TOPO_CB(twolines_reparam, FAM_TWOLINES, VAR_REPARAM, "twolines-reparam")
TOPO_CB(circle_base, FAM_CIRCLE, VAR_BASE, "circle-base")
TOPO_CB(circle_offset, FAM_CIRCLE, VAR_OFFSET, "circle-offset")
TOPO_CB(circle_scaled, FAM_CIRCLE, VAR_SCALED, "circle-scaled")
TOPO_CB(circle_reparam, FAM_CIRCLE, VAR_REPARAM, "circle-reparam")
TOPO_CB(cylinder_base, FAM_CYLINDER, VAR_BASE, "cylinder-base")
TOPO_CB(cylinder_offset, FAM_CYLINDER, VAR_OFFSET, "cylinder-offset")
TOPO_CB(cylinder_scaled, FAM_CYLINDER, VAR_SCALED, "cylinder-scaled")
TOPO_CB(cylinder_reparam, FAM_CYLINDER, VAR_REPARAM, "cylinder-reparam")
TOPO_CB(nohit, FAM_NOHIT, VAR_BASE, "nohit")

/* Cylinder first: the marching-plane mutation must be caught on the
 * non-degenerate oblique section rather than on a straight-line case. */
PAIRED_CB(cylinder_base, FAM_CYLINDER, VAR_BASE, "cylinder-base")
PAIRED_CB(cylinder_offset, FAM_CYLINDER, VAR_OFFSET, "cylinder-offset")
PAIRED_CB(cylinder_scaled, FAM_CYLINDER, VAR_SCALED, "cylinder-scaled")
PAIRED_CB(cylinder_reparam, FAM_CYLINDER, VAR_REPARAM, "cylinder-reparam")
PAIRED_CB(twolines_base, FAM_TWOLINES, VAR_BASE, "twolines-base")
PAIRED_CB(twolines_offset, FAM_TWOLINES, VAR_OFFSET, "twolines-offset")
PAIRED_CB(twolines_scaled, FAM_TWOLINES, VAR_SCALED, "twolines-scaled")
PAIRED_CB(twolines_reparam, FAM_TWOLINES, VAR_REPARAM, "twolines-reparam")
PAIRED_CB(circle_base, FAM_CIRCLE, VAR_BASE, "circle-base")
PAIRED_CB(circle_offset, FAM_CIRCLE, VAR_OFFSET, "circle-offset")
PAIRED_CB(circle_scaled, FAM_CIRCLE, VAR_SCALED, "circle-scaled")
PAIRED_CB(circle_reparam, FAM_CIRCLE, VAR_REPARAM, "circle-reparam")

COVER_CB(cylinder_base, FAM_CYLINDER, VAR_BASE, "cylinder-base")
COVER_CB(cylinder_offset, FAM_CYLINDER, VAR_OFFSET, "cylinder-offset")
COVER_CB(cylinder_scaled, FAM_CYLINDER, VAR_SCALED, "cylinder-scaled")
COVER_CB(cylinder_reparam, FAM_CYLINDER, VAR_REPARAM, "cylinder-reparam")
COVER_CB(twolines_base, FAM_TWOLINES, VAR_BASE, "twolines-base")
COVER_CB(twolines_offset, FAM_TWOLINES, VAR_OFFSET, "twolines-offset")
COVER_CB(twolines_scaled, FAM_TWOLINES, VAR_SCALED, "twolines-scaled")
COVER_CB(twolines_reparam, FAM_TWOLINES, VAR_REPARAM, "twolines-reparam")
COVER_CB(circle_base, FAM_CIRCLE, VAR_BASE, "circle-base")
COVER_CB(circle_offset, FAM_CIRCLE, VAR_OFFSET, "circle-offset")
COVER_CB(circle_scaled, FAM_CIRCLE, VAR_SCALED, "circle-scaled")
COVER_CB(circle_reparam, FAM_CIRCLE, VAR_REPARAM, "circle-reparam")

void section_qa_register(SectionQARegistry *reg)
{
  /* Registration order (not group order) puts the geometric and coverage
   * assertions before the component-count assertion, so the deliberately
   * mutated library copies are rejected on geometry/coverage rather than
   * on a mere count mismatch. */
  section_qa_add(reg, "paired-curves", "cylinder-base", paired_cylinder_base);
  section_qa_add(reg, "paired-curves", "cylinder-offset",
                 paired_cylinder_offset);
  section_qa_add(reg, "paired-curves", "cylinder-scaled",
                 paired_cylinder_scaled);
  section_qa_add(reg, "paired-curves", "cylinder-reparam",
                 paired_cylinder_reparam);
  section_qa_add(reg, "paired-curves", "twolines-base", paired_twolines_base);
  section_qa_add(reg, "paired-curves", "twolines-offset",
                 paired_twolines_offset);
  section_qa_add(reg, "paired-curves", "twolines-scaled",
                 paired_twolines_scaled);
  section_qa_add(reg, "paired-curves", "twolines-reparam",
                 paired_twolines_reparam);
  section_qa_add(reg, "paired-curves", "circle-base", paired_circle_base);
  section_qa_add(reg, "paired-curves", "circle-offset", paired_circle_offset);
  section_qa_add(reg, "paired-curves", "circle-scaled", paired_circle_scaled);
  section_qa_add(reg, "paired-curves", "circle-reparam", paired_circle_reparam);

  section_qa_add(reg, "analytic-coverage", "cylinder-base",
                 cover_cylinder_base);
  section_qa_add(reg, "analytic-coverage", "cylinder-offset",
                 cover_cylinder_offset);
  section_qa_add(reg, "analytic-coverage", "cylinder-scaled",
                 cover_cylinder_scaled);
  section_qa_add(reg, "analytic-coverage", "cylinder-reparam",
                 cover_cylinder_reparam);
  section_qa_add(reg, "analytic-coverage", "twolines-base",
                 cover_twolines_base);
  section_qa_add(reg, "analytic-coverage", "twolines-offset",
                 cover_twolines_offset);
  section_qa_add(reg, "analytic-coverage", "twolines-scaled",
                 cover_twolines_scaled);
  section_qa_add(reg, "analytic-coverage", "twolines-reparam",
                 cover_twolines_reparam);
  section_qa_add(reg, "analytic-coverage", "circle-base", cover_circle_base);
  section_qa_add(reg, "analytic-coverage", "circle-offset",
                 cover_circle_offset);
  section_qa_add(reg, "analytic-coverage", "circle-scaled",
                 cover_circle_scaled);
  section_qa_add(reg, "analytic-coverage", "circle-reparam",
                 cover_circle_reparam);

  section_qa_add(reg, "topology", "twolines-base", topo_twolines_base);
  section_qa_add(reg, "topology", "twolines-offset", topo_twolines_offset);
  section_qa_add(reg, "topology", "twolines-scaled", topo_twolines_scaled);
  section_qa_add(reg, "topology", "twolines-reparam", topo_twolines_reparam);
  section_qa_add(reg, "topology", "circle-base", topo_circle_base);
  section_qa_add(reg, "topology", "circle-offset", topo_circle_offset);
  section_qa_add(reg, "topology", "circle-scaled", topo_circle_scaled);
  section_qa_add(reg, "topology", "circle-reparam", topo_circle_reparam);
  section_qa_add(reg, "topology", "cylinder-base", topo_cylinder_base);
  section_qa_add(reg, "topology", "cylinder-offset", topo_cylinder_offset);
  section_qa_add(reg, "topology", "cylinder-scaled", topo_cylinder_scaled);
  section_qa_add(reg, "topology", "cylinder-reparam", topo_cylinder_reparam);
  section_qa_add(reg, "topology", "nohit", topo_nohit);
}
