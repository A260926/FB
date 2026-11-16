/*
 * Native SISL surface/surface intersection tracing with dual-parameter
 * grid events.  See include/sisl_trace_events.h for the public contract.
 *
 * Branch discovery is the real s1859 discovery.  Every branch is marched
 * by the ordinary s1310 kernel, which reports its final, accepted, clipped
 * station sequence through the private hook in src/s1310_hook.h: the same
 * step acceptance, step retreat and parameter-boundary clipping that the
 * public s1310 entry point performs, so no rejected trial step is ever
 * published.  Everything done on top of that (grid events, chord-error
 * refinement) is a Newton solve of the same joint station equations on both
 * original surfaces (here written out explicitly, since the packaged
 * s9iterate's monotone-distance acceptance is too strict for the tight
 * event parameters); no fitted parameter curves are sampled and welded.
 *
 * The exact exported C signatures are
 *
 *   int  sisl_trace_grid_events(const SISLSurf *surf1,
 *                               const double *grid1_u, int grid1_u_count,
 *                               const double *grid1_v, int grid1_v_count,
 *                               const SISLSurf *surf2,
 *                               const double *grid2_u, int grid2_u_count,
 *                               const double *grid2_v, int grid2_v_count,
 *                               const SislTraceOptions *options,
 *                               SislTraceResult **result);
 *   void sisl_trace_result_free(SislTraceResult *result);
 *
 * both declared in include/sisl_trace_events.h.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sisl.h"
#include "sislP.h"
#include "s1310_hook.h"
#include "sisl_trace_events.h"

#define TRACE_PARAM_STRICT 1e-12  /* event parameter solve target           */
#define TRACE_EVENT_MERGE  1e-7   /* same-station merge (events are >=1e-6)  */
#define TRACE_TOUCH_TOL    1e-9   /* grid-line hit classification (1e-9)     */
#define TRACE_SAME_LINE    1e-8   /* "overlaps the grid line" detection      */
#define TRACE_TANGENT_TOL  1e-8   /* reject tangential surface contact       */
#define TRACE_MAX_REFINE   24
#define TRACE_MAX_BRANCHES 100000

typedef struct
{
    double xyz[3];
    double uv[2][2];      /* uv[surface][direction] */
    unsigned int mask;
    int gidx[4];
} Stn;

typedef struct
{
    Stn *v;
    int n;
    int cap;
} StnVec;

typedef struct
{
    const double *g[4];   /* u1, v1, u2, v2 */
    int n[4];
} Grids;

/* ------------------------------------------------------------------ */
/* vector helpers                                                     */
/* ------------------------------------------------------------------ */

static int stnvec_push(StnVec *a, const Stn *s)
{
    if (a->n >= a->cap)
    {
        int nc = a->cap ? a->cap * 2 : 64;
        Stn *nv = (Stn *)realloc(a->v, (size_t)nc * sizeof(Stn));
        if (!nv) return -1;
        a->v = nv;
        a->cap = nc;
    }
    a->v[a->n++] = *s;
    return 0;
}

static void stnvec_free(StnVec *a)
{
    if (a) { free(a->v); a->v = NULL; a->n = a->cap = 0; }
}

static double dmax(double a, double b) { return a > b ? a : b; }
static double dmin(double a, double b) { return a < b ? a : b; }

static double vlen3(const double *a)
{
    return sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

static double vdist3(const double *a, const double *b)
{
    double d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
    return sqrt(d0 * d0 + d1 * d1 + d2 * d2);
}

static int near_value(double a, double b, double rel)
{
    double m = dmax(1.0, dmax(fabs(a), fabs(b)));
    return fabs(a - b) <= rel * m;
}

static double dist_point_segment(const double *p, const double *a, const double *b)
{
    double d[3], w[3], t, den, q[3];
    d[0] = b[0] - a[0]; d[1] = b[1] - a[1]; d[2] = b[2] - a[2];
    w[0] = p[0] - a[0]; w[1] = p[1] - a[1]; w[2] = p[2] - a[2];
    den = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (den <= 0.0) return vlen3(w);
    t = (w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / den;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    q[0] = a[0] + t * d[0]; q[1] = a[1] + t * d[1]; q[2] = a[2] + t * d[2];
    return vdist3(p, q);
}

static int grid_index_near(const double *g, int n, double x, double tol, int *idx)
{
    int lo = 0, hi = n - 1, m;
    if (n <= 0) return 0;
    while (lo < hi)
    {
        m = (lo + hi) / 2;
        if (g[m] < x) lo = m + 1; else hi = m;
    }
    m = lo;
    if (m > 0 && fabs(g[m - 1] - x) <= fabs(g[m] - x)) m--;
    if (idx) *idx = m;
    return fabs(g[m] - x) <= tol;
}

static int grid_upper(const double *g, int n, double x)
{
    int lo = 0, hi = n;
    while (lo < hi)
    {
        int m = (lo + hi) / 2;
        if (g[m] <= x) lo = m + 1; else hi = m;
    }
    return lo;
}

/* ------------------------------------------------------------------ */
/* surface / grid validation                                          */
/* ------------------------------------------------------------------ */

static const double *surf_et(const SISLSurf *s, int dir)
{
    return dir == 0 ? s->et1 : s->et2;
}
static int surf_ik(const SISLSurf *s, int dir)
{
    return dir == 0 ? s->ik1 : s->ik2;
}
static int surf_in(const SISLSurf *s, int dir)
{
    return dir == 0 ? s->in1 : s->in2;
}

static int surface_scale(const SISLSurf *s, double *scale)
{
    int i, n = s->in1 * s->in2;
    double sc = 1.0;
    for (i = 0; i < 2; ++i)
    {
        const double *et = surf_et(s, i);
        int in = surf_in(s, i), ik = surf_ik(s, i), j;
        for (j = 0; j < in + ik; ++j) sc = dmax(sc, fabs(et[j]));
    }
    /* ecoef holds the (unweighted) control points for rational surfaces as
       well, so the coordinate bound is checked against them */
    if (!s->ecoef) return -1;
    for (i = 0; i < n * 3; ++i) sc = dmax(sc, fabs(s->ecoef[i]));
    *scale = sc;
    return 0;
}

static int validate_surface(const SISLSurf *s)
{
    int dir, i, j, n;
    double scale;

    if (!s) return -1;
    if (s->idim != 3) return -1;
    if (s->ikind != 1 && s->ikind != 2) return -1;
    if (s->ik1 < 2 || s->ik1 > 4 || s->ik2 < 2 || s->ik2 > 4) return -1;
    if (s->in1 < s->ik1 || s->in2 < s->ik2) return -1;
    if (s->cuopen_1 != SISL_SURF_OPEN || s->cuopen_2 != SISL_SURF_OPEN) return -1;
    if (!s->ecoef) return -1;
    if (s->ikind == 2 && !s->rcoef) return -1;

    if (surface_scale(s, &scale) != 0) return -1;
    if (!isfinite(scale) || scale > 100.0 * (1.0 + 1e-12)) return -1;

    for (dir = 0; dir < 2; ++dir)
    {
        const double *et = surf_et(s, dir);
        int ik = surf_ik(s, dir), in = surf_in(s, dir);
        double lo, hi;
        if (!et) return -1;
        for (i = 0; i < in + ik; ++i)
            if (!isfinite(et[i]) || fabs(et[i]) > 100.0 * (1.0 + 1e-12)) return -1;
        lo = et[ik - 1];
        hi = et[in];
        for (i = 0; i < ik; ++i)
            if (!near_value(et[i], lo, 1e-12)) return -1;
        for (i = 0; i < ik; ++i)
            if (!near_value(et[in + i], hi, 1e-12)) return -1;
        if (!(hi > lo)) return -1;
        for (i = 0; i + 1 < in + ik; ++i)
        {
            double d = et[i + 1] - et[i];
            if (d < 0.0) return -1;
            if (d > 0.0 && d < 1e-5 * (1.0 - 1e-12)) return -1;
        }
        /* interior knots must be at least C1: multiplicity <= order-2 */
        for (i = ik; i < in; )
        {
            int j = i;
            while (j < in && et[j] == et[i]) ++j;
            if (j - i > ik - 2) return -1;
            i = j;
        }
    }

    if (s->ikind == 2)
    {
        double wmin = 1e300, wmax = -1e300;
        n = s->in1 * s->in2;
        for (i = 0; i < n; ++i)
        {
            double w = s->rcoef[(size_t)i * 4 + 3];
            if (!(w > 0.0) || !isfinite(w)) return -1;
            wmin = dmin(wmin, w);
            wmax = dmax(wmax, w);
        }
        if (!(wmin > 0.0) || !(wmax / wmin <= 100.0 * (1.0 + 1e-12))) return -1;
    }

    /* regularity on a sparse sample set of the parameter domain */
    for (dir = 0; dir < 2; ++dir)
    {
        const double *et = surf_et(s, dir);
        int ik = surf_ik(s, dir), in = surf_in(s, dir);
        int other = 1 - dir;
        const double *eto = surf_et(s, other);
        int iko = surf_ik(s, other), ino = surf_in(s, other);
        for (j = 0; j < 4; ++j)
        {
            double uv[2], der[21], nrm[3];
            int lu = 0, lv = 0, st = 0;
            uv[dir] = et[ik - 1] + (et[in] - et[ik - 1]) * (0.11 + 0.26 * j);
            uv[other] = eto[iko - 1] + (eto[ino] - eto[iko - 1]) *
                                       (0.19 + 0.21 * (3 - j));
            s1421((SISLSurf *)s, 1, uv, &lu, &lv, der, nrm, &st);
            if (st < 0) return -1;
            if (vlen3(nrm) <= 0.0) return -1;
        }
    }
    return 0;
}

static int validate_grid(const double *g, int n, const SISLSurf *s, int dir)
{
    const double *et = surf_et(s, dir);
    int ik = surf_ik(s, dir), in = surf_in(s, dir);
    double lo = et[ik - 1], hi = et[in];
    double sc = dmax(1.0, dmax(fabs(lo), fabs(hi)));
    int i;

    if (!g) return -11;
    if (n < 2) return -12;
    for (i = 0; i < n; ++i)
        if (!isfinite(g[i])) return -13;
    if (fabs(g[0] - lo) > 1e-9 * sc) return -14;
    if (fabs(g[n - 1] - hi) > 1e-9 * sc) return -15;
    for (i = 0; i + 1 < n; ++i)
        if (!(g[i + 1] > g[i])) return -16;

    for (i = 0; i < in + ik; ++i)
    {
        double k = et[i];
        int idx;
        if (!(k > lo) || !(k < hi)) continue;
        if (!grid_index_near(g, n, k, 1e-9 * sc, &idx)) return -17;
    }
    /* distinct grid lines are at least 1e-6 apart in their parameter chart */
    for (i = 0; i + 1 < n; ++i)
    {
        double m = dmax(1.0, dmax(fabs(g[i]), fabs(g[i + 1])));
        if (g[i + 1] - g[i] < 1e-6 * (1.0 - 1e-9) * m) return -18;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* evaluation, correction and chord checks                            */
/* ------------------------------------------------------------------ */

static int eval_surface(SISLSurf *s, const double uv[2], double der[21], double nrm[3])
{
    int lu = 0, lv = 0, st = 0;
    s1421(s, 2, (double *)uv, &lu, &lv, der, nrm, &st);
    return st;
}

/* 0 when the two normals are (numerically) parallel, 1 when transverse,
   -1 on evaluation failure. */
static int station_regular(SISLSurf *s1, SISLSurf *s2, const double uv1[2],
                           const double uv2[2])
{
    double d1[21], d2[21], n1[3], n2[3], c[3], l1, l2;
    if (eval_surface(s1, uv1, d1, n1) < 0) return -1;
    if (eval_surface(s2, uv2, d2, n2) < 0) return -1;
    l1 = vlen3(n1);
    l2 = vlen3(n2);
    if (l1 <= 0.0 || l2 <= 0.0) return 0;
    c[0] = n1[1] * n2[2] - n1[2] * n2[1];
    c[1] = n1[2] * n2[0] - n1[0] * n2[2];
    c[2] = n1[0] * n2[1] - n1[1] * n2[0];
    return vlen3(c) > TRACE_TANGENT_TOL * l1 * l2 ? 1 : 0;
}

/* Solve a 4x4 dense system with partial pivoting.  Returns 0 on success. */
static int solve4(double A[4][4], double b[4], double x[4])
{
    int i, j, k, piv;
    for (k = 0; k < 4; ++k)
    {
        double best = fabs(A[k][k]);
        piv = k;
        for (i = k + 1; i < 4; ++i)
            if (fabs(A[i][k]) > best) { best = fabs(A[i][k]); piv = i; }
        if (best <= 1e-14) return -1;
        if (piv != k)
        {
            for (j = 0; j < 4; ++j) { double t = A[k][j]; A[k][j] = A[piv][j]; A[piv][j] = t; }
            { double t = b[k]; b[k] = b[piv]; b[piv] = t; }
        }
        for (i = k + 1; i < 4; ++i)
        {
            double f = A[i][k] / A[k][k];
            for (j = k; j < 4; ++j) A[i][j] -= f * A[k][j];
            b[i] -= f * b[k];
        }
    }
    for (i = 3; i >= 0; --i)
    {
        double sum = b[i];
        for (j = i + 1; j < 4; ++j) sum -= A[i][j] * x[j];
        x[i] = sum / A[i][i];
    }
    return 0;
}

/*
 * Jointly correct a station on both original surfaces with a Newton solve
 * of the same four equations the SISL marcher's iteration uses:
 *
 *   S1(u1,v1) - S2(u2,v2) = 0                       (three components)
 *   dot(S1(u1,v1) - pplane, vplane) = 0             (pins the free parameter)
 *
 * The unknown vector is (u1,v1,u2,v2).  On success `out` holds the corrected
 * parameters, the midpoint position and the resulting half separation.
 * Returns 0 on success, 2 for a singular/degenerate system, 1 when the
 * iteration does not converge and -1 for an evaluation failure.
 */
static int joint_correct_impl(SISLSurf *s1, SISLSurf *s2,
                              const double uv1in[2], const double uv2in[2],
                              const double pplane[3], const double vplane[3],
                              Stn *out, double *residual, double param_lo[4],
                              double param_hi[4], int pin, double pin_value)
{
    double uv[4];
    double d1[21], d2[21], n1[3], n2[3];
    int it;

    uv[0] = uv1in[0]; uv[1] = uv1in[1];
    uv[2] = uv2in[0]; uv[3] = uv2in[1];

    for (it = 0; it < 40; ++it)
    {
        double F[4], A[4][4], delta[4];
        double gap;
        int i, j;

        if (eval_surface(s1, uv, d1, n1) < 0) return -1;
        if (eval_surface(s2, uv + 2, d2, n2) < 0) return -1;
        if (vlen3(n1) <= 0.0 || vlen3(n2) <= 0.0) return 2;

        F[0] = d1[0] - d2[0];
        F[1] = d1[1] - d2[1];
        F[2] = d1[2] - d2[2];
        F[3] = (d1[0] - pplane[0]) * vplane[0] +
               (d1[1] - pplane[1]) * vplane[1] +
               (d1[2] - pplane[2]) * vplane[2];
        if (pin >= 0) F[3] = uv[pin] - pin_value;

        gap = vlen3(F);
        if (gap <= 1e-14 && fabs(F[3]) <= 1e-12) break;

        for (i = 0; i < 3; ++i)
        {
            A[i][0] = d1[3 + i];      /* dS1/du1 */
            A[i][1] = d1[6 + i];      /* dS1/dv1 */
            A[i][2] = -d2[3 + i];     /* -dS2/du2 */
            A[i][3] = -d2[6 + i];     /* -dS2/dv2 */
        }
        A[3][0] = d1[3] * vplane[0] + d1[4] * vplane[1] + d1[5] * vplane[2];
        A[3][1] = d1[6] * vplane[0] + d1[7] * vplane[1] + d1[8] * vplane[2];
        A[3][2] = 0.0;
        A[3][3] = 0.0;
        if (fabs(A[3][0]) + fabs(A[3][1]) <= 1e-15) return 2;
        if (pin >= 0)
        {
            /* pin one parameter exactly to its domain edge */
            for (i = 0; i < 4; ++i) A[3][i] = (i == pin) ? 1.0 : 0.0;
            F[3] = uv[pin] - pin_value;
        }

        for (i = 0; i < 4; ++i) delta[i] = -F[i];
        if (solve4(A, delta, delta) != 0) return 2;

        /* damped update keeps the iteration inside the parameter charts */
        {
            double lambda = 1.0;
            int trial;
            for (trial = 0; trial < 12; ++trial)
            {
                double nu[4];
                int ok = 1;
                for (j = 0; j < 4; ++j)
                {
                    nu[j] = uv[j] + lambda * delta[j];
                    if (nu[j] < param_lo[j] - 1e-9 * (param_hi[j] - param_lo[j]) ||
                        nu[j] > param_hi[j] + 1e-9 * (param_hi[j] - param_lo[j]))
                        ok = 0;
                }
                if (ok)
                {
                    double dF[21], dG[21], n3[3], n4[3], ng;
                    if (eval_surface(s1, nu, dF, n3) < 0 ||
                        eval_surface(s2, nu + 2, dG, n4) < 0)
                        return -1;
                    ng = sqrt((dF[0] - dG[0]) * (dF[0] - dG[0]) +
                              (dF[1] - dG[1]) * (dF[1] - dG[1]) +
                              (dF[2] - dG[2]) * (dF[2] - dG[2]));
                    if (ng <= gap * (1.0 + 1e-12) || ng <= 1e-14)
                    {
                        for (j = 0; j < 4; ++j) uv[j] = nu[j];
                        break;
                    }
                }
                lambda *= 0.5;
            }
            if (trial == 12) return 1;
        }
    }
    if (it == 40) return 1;

    if (eval_surface(s1, uv, d1, n1) < 0) return -1;
    if (eval_surface(s2, uv + 2, d2, n2) < 0) return -1;
    if (vlen3(n1) <= 0.0 || vlen3(n2) <= 0.0) return 2;

    out->uv[0][0] = uv[0];
    out->uv[0][1] = uv[1];
    out->uv[1][0] = uv[2];
    out->uv[1][1] = uv[3];
    out->xyz[0] = 0.5 * (d1[0] + d2[0]);
    out->xyz[1] = 0.5 * (d1[1] + d2[1]);
    out->xyz[2] = 0.5 * (d1[2] + d2[2]);
    out->mask = 0;
    out->gidx[0] = out->gidx[1] = out->gidx[2] = out->gidx[3] = -1;
    *residual = 0.5 * sqrt((d1[0] - d2[0]) * (d1[0] - d2[0]) +
                           (d1[1] - d2[1]) * (d1[1] - d2[1]) +
                           (d1[2] - d2[2]) * (d1[2] - d2[2]));
    return 0;
}

static int joint_correct(SISLSurf *s1, SISLSurf *s2,
                         const double uv1in[2], const double uv2in[2],
                         const double pplane[3], const double vplane[3],
                         Stn *out, double *residual, double param_lo[4],
                         double param_hi[4])
{
    return joint_correct_impl(s1, s2, uv1in, uv2in, pplane, vplane, out,
                              residual, param_lo, param_hi, -1, 0.0);
}

/* Parameter domain bounds, in the order (u1, v1, u2, v2). */
static void param_bounds(const SISLSurf *s1, const SISLSurf *s2,
                         double lo[4], double hi[4])
{
    int i;
    for (i = 0; i < 2; ++i)
    {
        const double *e = surf_et(s1, i);
        int ik = surf_ik(s1, i), in = surf_in(s1, i);
        lo[i] = e[ik - 1];
        hi[i] = e[in];
    }
    for (i = 0; i < 2; ++i)
    {
        const double *e = surf_et(s2, i);
        int ik = surf_ik(s2, i), in = surf_in(s2, i);
        lo[2 + i] = e[ik - 1];
        hi[2 + i] = e[in];
    }
}

/* 0 on success, 1 when the station must be rejected (tangential/singular),
   -1 on a numeric failure. */
static int correct_station(SISLSurf *s1, SISLSurf *s2, Stn *st,
                           double geomtol, double scale)
{
    double d1[21], d2[21], n1[3], n2[3], cross[3], p[3], dirn[3];
    double nrm, l1, l2, res, lo[4], hi[4];
    Stn out;
    int rc;

    if (eval_surface(s1, st->uv[0], d1, n1) < 0) return -1;
    if (eval_surface(s2, st->uv[1], d2, n2) < 0) return -1;
    l1 = vlen3(n1);
    l2 = vlen3(n2);
    if (l1 <= 0.0 || l2 <= 0.0) return 1;
    cross[0] = n1[1] * n2[2] - n1[2] * n2[1];
    cross[1] = n1[2] * n2[0] - n1[0] * n2[2];
    cross[2] = n1[0] * n2[1] - n1[1] * n2[0];
    nrm = vlen3(cross);
    if (nrm <= TRACE_TANGENT_TOL * l1 * l2) return 1;
    dirn[0] = cross[0] / nrm;
    dirn[1] = cross[1] / nrm;
    dirn[2] = cross[2] / nrm;
    p[0] = 0.5 * (d1[0] + d2[0]);
    p[1] = 0.5 * (d1[1] + d2[1]);
    p[2] = 0.5 * (d1[2] + d2[2]);

    param_bounds(s1, s2, lo, hi);
    rc = joint_correct(s1, s2, st->uv[0], st->uv[1], p, dirn, &out, &res, lo, hi);
    if (rc == 2) return 1;
    if (rc != 0) return -1;
    if (!(res <= dmax(geomtol, 1e-10 * scale))) return -1;
    out.mask = st->mask;
    memcpy(out.gidx, st->gidx, sizeof(out.gidx));
    *st = out;

    rc = station_regular(s1, s2, st->uv[0], st->uv[1]);
    if (rc < 0) return -1;
    return rc == 1 ? 0 : 1;
}

static int point_at_fraction(SISLSurf *s1, SISLSurf *s2, const Stn *a,
                             const Stn *b, double f, double scale,
                             Stn *out, double *residual)
{
    double p[3], d[3], nrm, uv1[2], uv2[2], lo[4], hi[4];
    int i, rc;
    d[0] = b->xyz[0] - a->xyz[0];
    d[1] = b->xyz[1] - a->xyz[1];
    d[2] = b->xyz[2] - a->xyz[2];
    nrm = vlen3(d);
    if (nrm <= 1e-14)
    {
        *out = *a;
        if (residual) *residual = 0.0;
        return 0;
    }
    p[0] = a->xyz[0] + f * d[0];
    p[1] = a->xyz[1] + f * d[1];
    p[2] = a->xyz[2] + f * d[2];
    d[0] /= nrm; d[1] /= nrm; d[2] /= nrm;
    for (i = 0; i < 2; ++i)
    {
        uv1[i] = a->uv[0][i] + f * (b->uv[0][i] - a->uv[0][i]);
        uv2[i] = a->uv[1][i] + f * (b->uv[1][i] - a->uv[1][i]);
    }
    param_bounds(s1, s2, lo, hi);
    rc = joint_correct(s1, s2, uv1, uv2, p, d, out, residual, lo, hi);
    if (rc != 0) return -1;
    out->mask = 0;
    out->gidx[0] = out->gidx[1] = out->gidx[2] = out->gidx[3] = -1;
    return 0;
}

static int chord_ok(SISLSurf *s1, SISLSurf *s2, const Stn *a, const Stn *b,
                    double geomtol, double scale)
{
    static const double frac[3] = {0.25, 0.5, 0.75};
    double len = vdist3(a->xyz, b->xyz), floorv = dmax(geomtol, 1e-11 * scale);
    int i;
    if (len <= 1e-14) return 1;
    for (i = 0; i < 3; ++i)
    {
        Stn m;
        double res;
        if (point_at_fraction(s1, s2, a, b, frac[i], scale, &m, &res) != 0)
            return 0;
        if (!(res <= floorv)) return 0;
        if (dist_point_segment(m.xyz, a->xyz, b->xyz) > geomtol) return 0;
    }
    return 1;
}

static int refine_between(SISLSurf *s1, SISLSurf *s2, const Stn *a, const Stn *b,
                          double geomtol, double scale, int depth, StnVec *out)
{
    Stn m;
    double res;
    if (depth > TRACE_MAX_REFINE) return -1;
    if (chord_ok(s1, s2, a, b, geomtol, scale)) return 0;
    if (point_at_fraction(s1, s2, a, b, 0.5, scale, &m, &res) != 0) return -1;
    if (vdist3(m.xyz, a->xyz) <= 1e-14 * scale ||
        vdist3(m.xyz, b->xyz) <= 1e-14 * scale)
        return -1;
    if (refine_between(s1, s2, a, &m, geomtol, scale, depth + 1, out) != 0)
        return -1;
    if (stnvec_push(out, &m) != 0) return -1;
    return refine_between(s1, s2, &m, b, geomtol, scale, depth + 1, out);
}

/* ------------------------------------------------------------------ */
/* grid events                                                        */
/* ------------------------------------------------------------------ */

static double stn_comp(const Stn *s, int fam)
{
    return s->uv[fam >> 1][fam & 1];
}

static int stn_same_point(const Stn *a, const Stn *b, double tol)
{
    int s, d;
    for (s = 0; s < 2; ++s)
        for (d = 0; d < 2; ++d)
            if (fabs(a->uv[s][d] - b->uv[s][d]) > tol) return 0;
    return 1;
}

typedef struct
{
    double f;
    Stn st;
} Event;

typedef struct
{
    Event *v;
    int n, cap;
} EventVec;

static int eventvec_push(EventVec *e, double f, const Stn *st)
{
    if (e->n >= e->cap)
    {
        int nc = e->cap ? e->cap * 2 : 16;
        Event *nv = (Event *)realloc(e->v, (size_t)nc * sizeof(Event));
        if (!nv) return -1;
        e->v = nv;
        e->cap = nc;
    }
    e->v[e->n].f = f;
    e->v[e->n].st = *st;
    e->n++;
    return 0;
}

static void eventvec_free(EventVec *e)
{
    if (e) { free(e->v); e->v = NULL; e->n = e->cap = 0; }
}

static int event_cmp(const void *pa, const void *pb)
{
    const Event *a = (const Event *)pa, *b = (const Event *)pb;
    if (a->f < b->f) return -1;
    if (a->f > b->f) return 1;
    return 0;
}

/* Identify every grid line hit by this (final) station. */
static void mark_grid(Stn *st, const Grids *gr)
{
    int fam, idx;
    st->mask = 0;
    st->gidx[0] = st->gidx[1] = st->gidx[2] = st->gidx[3] = -1;
    for (fam = 0; fam < 4; ++fam)
    {
        if (!gr->g[fam] || gr->n[fam] < 2) continue;
        if (grid_index_near(gr->g[fam], gr->n[fam], stn_comp(st, fam),
                            TRACE_TOUCH_TOL, &idx))
        {
            st->mask |= (1u << fam);
            st->gidx[fam] = idx;
        }
    }
}

/*
 * Fraction at which parameter component `fam` crosses `value`, bracketed by
 * [f0,f1].  Returns 0 on success.
 */
static int solve_crossing(SISLSurf *s1, SISLSurf *s2, const Stn *a, const Stn *b,
                          int fam, double value, double f_lo, double f_hi,
                          const Stn *p_lo, const Stn *p_hi, double scale,
                          double *fout, Stn *stout)
{
    double f0 = f_lo, f1 = f_hi, ca, cb;
    int it;
    double res;

    ca = stn_comp(p_lo, fam) - value;
    cb = stn_comp(p_hi, fam) - value;
    if (ca * cb > 0.0) return -1;
    *fout = f0;
    *stout = *p_lo;
    for (it = 0; it < 100; ++it)
    {
        /* regula falsi gets there in a handful of evaluations for a smooth
           component; fall back to bisection when it stalls */
        double fm = (ca == cb) ? 0.5 * (f0 + f1) : f0 - ca * (f1 - f0) / (cb - ca);
        double cm;
        Stn pm;
        if (!(fm > f0) || !(fm < f1)) fm = 0.5 * (f0 + f1);
        if (point_at_fraction(s1, s2, a, b, fm, scale, &pm, &res) != 0) return -1;
        cm = stn_comp(&pm, fam) - value;
        *fout = fm;
        *stout = pm;
        if (fabs(cm) <= TRACE_PARAM_STRICT * dmax(1.0, fabs(value)) ||
            f1 - f0 <= 1e-15)
            return 0;
        if (ca * cm <= 0.0) { f1 = fm; cb = cm; }
        else { f0 = fm; ca = cm; }
        if (f1 - f0 <= 1e-15) return 0;
    }
    return 0;
}

/*
 * Fraction of a local parameter extremum near `fstar`.  Golden-section
 * search on the component.  Returns 0 when the extremal value is within the
 * touch tolerance of `value`.
 */
static int solve_touch(SISLSurf *s1, SISLSurf *s2, const Stn *a, const Stn *b,
                       int fam, double value, double fstar, double scale,
                       double *fout, Stn *stout)
{
    double lo = dmax(1e-4, fstar - 0.3), hi = dmin(1.0 - 1e-4, fstar + 0.3);
    const double gr = 0.6180339887498949;
    double c = hi - gr * (hi - lo), d = lo + gr * (hi - lo);
    Stn sc, sd, sb;
    double fc, fd, res, fbest, cbest;
    int it;

    if (hi - lo <= 1e-12) return -1;
    if (point_at_fraction(s1, s2, a, b, c, scale, &sc, &res) != 0) return -1;
    if (point_at_fraction(s1, s2, a, b, d, scale, &sd, &res) != 0) return -1;
    fc = stn_comp(&sc, fam);
    fd = stn_comp(&sd, fam);
    for (it = 0; it < 80; ++it)
    {
        if (hi - lo <= 1e-13) break;
        if (fc < fd)
        {
            lo = c; c = d; fc = fd;
            d = lo + gr * (hi - lo);
            if (point_at_fraction(s1, s2, a, b, d, scale, &sd, &res) != 0) return -1;
            fd = stn_comp(&sd, fam);
        }
        else
        {
            hi = d; d = c; fd = fc;
            c = hi - gr * (hi - lo);
            if (point_at_fraction(s1, s2, a, b, c, scale, &sc, &res) != 0) return -1;
            fc = stn_comp(&sc, fam);
        }
    }
    if (fc >= fd) { fbest = c; cbest = fc; sb = sc; }
    else { fbest = d; cbest = fd; sb = sd; }
    *fout = fbest;
    *stout = sb;
    return fabs(cbest - value) <= TRACE_TOUCH_TOL ? 0 : 1;
}

/* Collect all grid events strictly inside segment a-b, jointly corrected and
   with grid identity attached. */
static int segment_events(SISLSurf *s1, SISLSurf *s2, const Stn *a, const Stn *b,
                          const Grids *gr, double scale, EventVec *ev)
{
    int fam;
    for (fam = 0; fam < 4; ++fam)
    {
        const double *g = gr->g[fam];
        int n = gr->n[fam], i;
        double ac, bc;

        if (!g || n < 2) continue;
        ac = stn_comp(a, fam);
        bc = stn_comp(b, fam);
        if (fabs(ac - bc) <= 1e-15) continue;

        /* A crossing needs the component to actually pass the value at some
           fraction of the segment.  Scanning a set of corrected samples also
           catches the (pathological) case of more than one crossing per
           segment, which a purely endpoint-based test would miss. */
        {
            /* the crossing test needs only enough samples to detect that the
               component passes a line; the precise root is then found from the
               bracketing samples */
            const int ns = 8;
            double prevf = 0.0;
            Stn prevp = *a;
            for (i = 1; i <= ns; ++i)
            {
                double f = (double)i / ns, c;
                Stn p;
                double res;
                if (point_at_fraction(s1, s2, a, b, f, scale, &p, &res) != 0)
                    return -1;
                c = stn_comp(&p, fam);
                {
                    double x0 = dmin(stn_comp(&prevp, fam), c);
                    double x1 = dmax(stn_comp(&prevp, fam), c);
                    int j0 = grid_upper(g, n, x0);
                    int j1 = grid_upper(g, n, x1);
                    int j;
                    for (j = j0; j < j1; ++j)
                    {
                        double gv = g[j], ff;
                        Stn pt;
                        if (fabs(gv - ac) <= TRACE_PARAM_STRICT * dmax(1.0, fabs(gv)) ||
                            fabs(gv - bc) <= TRACE_PARAM_STRICT * dmax(1.0, fabs(gv)))
                            continue;
                        if (solve_crossing(s1, s2, a, b, fam, gv, prevf, f,
                                           &prevp, &p, scale, &ff, &pt) != 0)
                            return -1;
                        if (eventvec_push(ev, ff, &pt) != 0) return -1;
                    }
                }
                prevf = f;
                prevp = p;
            }
        }
        /* touches: a coarse scan detects an interior extremum, which is then
           refined and classified against the nearest grid line */
        {
            const int ns = 8;
            double fv[9], cv[9];
            Stn sv[9];
            double res;
            int k;
            fv[0] = 0.0;
            if (point_at_fraction(s1, s2, a, b, 0.0, scale, &sv[0], &res) != 0)
                return -1;
            cv[0] = stn_comp(&sv[0], fam);
            for (k = 1; k <= ns; ++k)
            {
                fv[k] = (double)k / ns;
                if (point_at_fraction(s1, s2, a, b, fv[k], scale, &sv[k], &res) != 0)
                    return -1;
                cv[k] = stn_comp(&sv[k], fam);
            }
            for (k = 1; k < ns; ++k)
            {
                int ismax = (cv[k] > cv[k - 1] && cv[k] >= cv[k + 1]);
                int ismin = (cv[k] < cv[k - 1] && cv[k] <= cv[k + 1]);
                double A, B, fstar, cstar;
                int idx, rc;
                Stn tst;
                double res2;
                if (!ismax && !ismin) continue;
                A = 2.0 * cv[k - 1] - 4.0 * cv[k] + 2.0 * cv[k + 1];
                B = cv[k + 1] - cv[k - 1];
                if (fabs(A) <= 1e-18 * dmax(1.0, fabs(cv[k]))) continue;
                fstar = fv[k] - 0.5 * B / A * (1.0 / ns);
                if (fstar <= 1e-3 || fstar >= 1.0 - 1e-3) continue;
                if (point_at_fraction(s1, s2, a, b, fstar, scale, &tst, &res2) != 0)
                    return -1;
                cstar = stn_comp(&tst, fam);
                if (!grid_index_near(g, n, cstar, TRACE_TOUCH_TOL, &idx)) continue;
                rc = solve_touch(s1, s2, a, b, fam, g[idx], fstar, scale,
                                 &fstar, &tst);
                if (rc < 0) return -1;
                if (rc == 1) continue;
                if (eventvec_push(ev, fstar, &tst) != 0) return -1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* branch assembly                                                    */
/* ------------------------------------------------------------------ */

/* is the segment a-b lying along one grid line for its whole length? */
static int segment_on_grid_line(SISLSurf *s1, SISLSurf *s2, const Stn *a,
                                const Stn *b, const Grids *gr, double scale)
{
    int fam;
    for (fam = 0; fam < 4; ++fam)
    {
        const double *g = gr->g[fam];
        int n = gr->n[fam], idx;
        double c0, c1;
        if (!g || n < 2) continue;
        if (!(a->mask & (1u << fam)) || !(b->mask & (1u << fam))) continue;
        if (a->gidx[fam] != b->gidx[fam]) continue;
        c0 = stn_comp(a, fam);
        c1 = stn_comp(b, fam);
        idx = a->gidx[fam];
        if (fabs(c0 - g[idx]) <= TRACE_SAME_LINE * dmax(1.0, fabs(g[idx])) &&
            fabs(c1 - g[idx]) <= TRACE_SAME_LINE * dmax(1.0, fabs(g[idx])))
        {
            /* the whole segment must keep that parameter value */
            int k;
            for (k = 1; k < 16; ++k)
            {
                Stn m;
                double res;
                double f = (double)k / 16.0;
                if (point_at_fraction(s1, s2, a, b, f, scale, &m, &res) != 0)
                    return 1;
                if (fabs(stn_comp(&m, fam) - g[idx]) >
                    TRACE_SAME_LINE * dmax(1.0, fabs(g[idx])))
                    break;
            }
            if (k == 16) return 1;
        }
    }
    return 0;
}

/*
 * Correct one raw s1310 station sequence into a publishable branch.
 * Returns 0 on success (possibly an empty branch, which is then skipped by
 * the caller), SISL_TRACE_REJECTED for a tangential/singular branch and
 * SISL_TRACE_BUDGET when the station budget would be exceeded.
 */
/*
 * An open marching branch can stop just short of the patch boundary when the
 * intersection leaves the surface domain.  Extend such an endpoint exactly
 * onto the nearest domain edge: pin the parameter that reaches the edge and
 * solve the remaining equations jointly on both surfaces.
 */
static int extend_endpoint_to_domain(SISLSurf *s1, SISLSurf *s2, Stn *st,
                                     double geomtol, double scale)
{
    double lo[4], hi[4];
    double d1[21], d2[21], n1[3], n2[3], cross[3], p[3], dirn[3];
    double nrm, l1, l2, res;
    Stn out;
    int axis, fixed = -1, side = 0;
    double best_slack = 0.0;

    param_bounds(s1, s2, lo, hi);
    for (axis = 0; axis < 4; ++axis)
    {
        double span = hi[axis] - lo[axis];
        /* an open branch can only end at a patch edge, so accept the
           nearest edge unless it is implausibly far away */
        double slab = 0.1 * dmax(1e-12, span);
        double x = st->uv[axis >> 1][axis & 1];
        double slack_lo = x - lo[axis];
        double slack_hi = hi[axis] - x;
        if (slack_lo >= -1e-12 && slack_lo <= slab &&
            (fixed < 0 || slack_lo < best_slack))
        {
            fixed = axis;
            side = 0;
            best_slack = slack_lo;
        }
        if (slack_hi >= -1e-12 && slack_hi <= slab &&
            (fixed < 0 || slack_hi < best_slack))
        {
            fixed = axis;
            side = 1;
            best_slack = slack_hi;
        }
    }
    if (fixed < 0) return 1;

    if (eval_surface(s1, st->uv[0], d1, n1) < 0) return -1;
    if (eval_surface(s2, st->uv[1], d2, n2) < 0) return -1;
    l1 = vlen3(n1);
    l2 = vlen3(n2);
    if (l1 <= 0.0 || l2 <= 0.0) return 1;
    cross[0] = n1[1] * n2[2] - n1[2] * n2[1];
    cross[1] = n1[2] * n2[0] - n1[0] * n2[2];
    cross[2] = n1[0] * n2[1] - n1[1] * n2[0];
    nrm = vlen3(cross);
    if (nrm <= TRACE_TANGENT_TOL * l1 * l2) return 1;
    dirn[0] = cross[0] / nrm;
    dirn[1] = cross[1] / nrm;
    dirn[2] = cross[2] / nrm;
    p[0] = 0.5 * (d1[0] + d2[0]);
    p[1] = 0.5 * (d1[1] + d2[1]);
    p[2] = 0.5 * (d1[2] + d2[2]);

    {
        double uv1[2], uv2[2], pin_value;
        uv1[0] = st->uv[0][0];
        uv1[1] = st->uv[0][1];
        uv2[0] = st->uv[1][0];
        uv2[1] = st->uv[1][1];
        pin_value = side ? hi[fixed] : lo[fixed];
        if (fixed < 2)
            uv1[fixed] = pin_value;
        else
            uv2[fixed - 2] = pin_value;
        if (joint_correct_impl(s1, s2, uv1, uv2, p, dirn, &out, &res,
                               lo, hi, fixed, pin_value) != 0)
            return 1;
    }
    if (!(res <= dmax(geomtol, 1e-10 * scale))) return 1;
    out.mask = st->mask;
    memcpy(out.gidx, st->gidx, sizeof(out.gidx));
    *st = out;
    return 0;
}

static int build_branch(SISLSurf *s1, SISLSurf *s2, const Stn *raw, int raw_n,
                        int raw_closed, const Grids *gr, double geomtol,
                        double scale, long station_budget, StnVec *out,
                        int *out_closed)
{
    StnVec base = {0}, merged = {0};
    int i, rc = SISL_TRACE_OK, segs;

    out->n = 0;
    *out_closed = 0;

    /* 1. joint correction of the accepted march stations */
    for (i = 0; i < raw_n; ++i)
    {
        Stn s = raw[i];
        int cr = correct_station(s1, s2, &s, geomtol, scale);
        if (cr < 0) { rc = SISL_TRACE_NUMERIC; goto done; }
        if (cr == 1) continue; /* never publish a rejected station */
        if (base.n > 0 && stn_same_point(&base.v[base.n - 1], &s, TRACE_EVENT_MERGE))
        {
            base.v[base.n - 1].mask |= s.mask;
            continue;
        }
        if (stnvec_push(&base, &s) != 0) { rc = SISL_TRACE_NUMERIC; goto done; }
    }
    if (base.n == 0) goto done;
    if (base.n == 1)
    {
        /* a branch that collapsed to a point is an isolated tangency */
        rc = SISL_TRACE_REJECTED;
        goto done;
    }
    if (stn_same_point(&base.v[0], &base.v[base.n - 1], TRACE_EVENT_MERGE))
    {
        base.n--;
        raw_closed = 1;
    }
    if (base.n < 2) { rc = SISL_TRACE_REJECTED; goto done; }
    for (i = 0; i < base.n; ++i) mark_grid(&base.v[i], gr);

    /* 2. split at every grid line crossing/touch of either surface */
    segs = raw_closed ? base.n : base.n - 1;
    for (i = 0; i < segs; ++i)
    {
        const Stn *a = &base.v[i];
        const Stn *b = &base.v[(i + 1) % base.n];
        EventVec ev = {0};
        int k;
        if (stnvec_push(&merged, a) != 0) { eventvec_free(&ev); rc = SISL_TRACE_NUMERIC; goto done; }
        if (segment_events(s1, s2, a, b, gr, scale, &ev) != 0)
        {
            eventvec_free(&ev);
            rc = SISL_TRACE_NUMERIC;
            goto done;
        }
        if (ev.n > 1) qsort(ev.v, (size_t)ev.n, sizeof(Event), event_cmp);
        for (k = 0; k < ev.n; ++k)
        {
            mark_grid(&ev.v[k].st, gr);
            if (stn_same_point(&ev.v[k].st, &merged.v[merged.n - 1],
                               TRACE_EVENT_MERGE))
            {
                merged.v[merged.n - 1].mask |= ev.v[k].st.mask;
                continue;
            }
            if (stnvec_push(&merged, &ev.v[k].st) != 0)
            {
                eventvec_free(&ev);
                rc = SISL_TRACE_NUMERIC;
                goto done;
            }
        }
        eventvec_free(&ev);
    }
    if (!raw_closed)
    {
        if (stnvec_push(&merged, &base.v[base.n - 1]) != 0)
        {
            rc = SISL_TRACE_NUMERIC;
            goto done;
        }
    }
    else if (merged.n > 1 &&
             stn_same_point(&merged.v[0], &merged.v[merged.n - 1],
                            TRACE_EVENT_MERGE))
    {
        merged.v[0].mask |= merged.v[merged.n - 1].mask;
        merged.n--;
    }
    if (merged.n < 2) { rc = SISL_TRACE_REJECTED; goto done; }

    /* an open branch that stops short of a patch edge is extended exactly
       onto that edge, so both sheets can share the boundary station */
    if (!raw_closed)
    {
        (void)extend_endpoint_to_domain(s1, s2, &merged.v[0], geomtol, scale);
        (void)extend_endpoint_to_domain(s1, s2, &merged.v[merged.n - 1],
                                        geomtol, scale);
        mark_grid(&merged.v[0], gr);
        mark_grid(&merged.v[merged.n - 1], gr);
    }

    /* 3. an intersection running along a grid line for a whole segment is
       an explicitly rejected degenerate input */
    {
        int msegs = raw_closed ? merged.n : merged.n - 1;
        for (i = 0; i < msegs; ++i)
        {
            if (segment_on_grid_line(s1, s2, &merged.v[i],
                                     &merged.v[(i + 1) % merged.n], gr, scale))
            {
                rc = SISL_TRACE_REJECTED;
                goto done;
            }
        }
    }

    /* 4. chord-error refinement between consecutive stations */
    {
        int msegs = raw_closed ? merged.n : merged.n - 1;
        if (stnvec_push(out, &merged.v[0]) != 0)
        {
            rc = SISL_TRACE_NUMERIC;
            goto done;
        }
        for (i = 0; i < msegs; ++i)
        {
            const Stn *a = &merged.v[i];
            const Stn *b = (raw_closed && i == msegs - 1)
                               ? &merged.v[0]
                               : &merged.v[i + 1];
            StnVec local = {0};
            int k;
            if (refine_between(s1, s2, a, b, geomtol, scale, 0, &local) != 0)
            {
                stnvec_free(&local);
                rc = SISL_TRACE_NUMERIC;
                goto done;
            }
            for (k = 0; k < local.n; ++k)
                if (stnvec_push(out, &local.v[k]) != 0)
                {
                    stnvec_free(&local);
                    rc = SISL_TRACE_NUMERIC;
                    goto done;
                }
            stnvec_free(&local);
            if (!(raw_closed && i == msegs - 1) &&
                stnvec_push(out, &merged.v[i + 1]) != 0)
            {
                rc = SISL_TRACE_NUMERIC;
                goto done;
            }
            if (station_budget > 0 && (long)out->n > station_budget)
            {
                rc = SISL_TRACE_BUDGET;
                goto done;
            }
        }
    }
    *out_closed = raw_closed;

done:
    stnvec_free(&base);
    stnvec_free(&merged);
    if (rc != SISL_TRACE_OK) stnvec_free(out);
    return rc;
}

/* ------------------------------------------------------------------ */
/* hook                                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    StnVec vec;
    int closed;
    int overflow;
    long limit;
} HookCtx;

static int hook_emit(void *user, int count, const double *geo,
                     const double *geo1, const double *geo2, int closed)
{
    HookCtx *ctx = (HookCtx *)user;
    int i;
    if (count < 0) return -1;
    if (ctx->limit > 0 && (long)ctx->vec.n + count > ctx->limit)
    {
        ctx->overflow = 1;
        return -1;
    }
    for (i = 0; i < count; ++i)
    {
        Stn s;
        s.xyz[0] = geo[10 * i];
        s.xyz[1] = geo[10 * i + 1];
        s.xyz[2] = geo[10 * i + 2];
        s.uv[0][0] = geo1[7 * i];
        s.uv[0][1] = geo1[7 * i + 1];
        s.uv[1][0] = geo2[7 * i];
        s.uv[1][1] = geo2[7 * i + 1];
        s.mask = 0;
        s.gidx[0] = s.gidx[1] = s.gidx[2] = s.gidx[3] = -1;
        if (stnvec_push(&ctx->vec, &s) != 0) return -1;
    }
    ctx->closed = closed ? 1 : 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* public entry points                                                */
/* ------------------------------------------------------------------ */

static void free_result_inner(SislTraceResult *r)
{
    size_t i;
    if (!r) return;
    for (i = 0; i < r->branch_count; ++i) free(r->branches[i].stations);
    free(r->branches);
    free(r);
}

void sisl_trace_result_free(SislTraceResult *result)
{
    free_result_inner(result);
}

static int result_append(SislTraceResult **acc, size_t *cap, size_t *count,
                         const StnVec *v, int closed, size_t *total)
{
    SislTraceBranch *nb;
    size_t i;
    if (*count == *cap)
    {
        size_t nc = *cap ? *cap * 2 : 8;
        SislTraceBranch *p = (SislTraceBranch *)realloc(
            (*acc)->branches, nc * sizeof(SislTraceBranch));
        if (!p) return -1;
        (*acc)->branches = p;
        *cap = nc;
    }
    nb = &(*acc)->branches[*count];
    nb->count = (size_t)v->n;
    nb->closed = closed;
    nb->stations = (SislTraceStation *)calloc((size_t)v->n, sizeof(SislTraceStation));
    if (!nb->stations) return -1;
    for (i = 0; i < (size_t)v->n; ++i)
    {
        const Stn *s = &v->v[i];
        SislTraceStation *t = &nb->stations[i];
        t->xyz[0] = s->xyz[0];
        t->xyz[1] = s->xyz[1];
        t->xyz[2] = s->xyz[2];
        t->uv1[0] = s->uv[0][0];
        t->uv1[1] = s->uv[0][1];
        t->uv2[0] = s->uv[1][0];
        t->uv2[1] = s->uv[1][1];
        t->grid_mask = s->mask;
        t->grid_index[0] = s->gidx[0];
        t->grid_index[1] = s->gidx[1];
        t->grid_index[2] = s->gidx[2];
        t->grid_index[3] = s->gidx[3];
    }
    (*count)++;
    *total += (size_t)v->n;
    return 0;
}

int sisl_trace_grid_events(const SISLSurf *surf1,
                           const double *grid1_u, int grid1_u_count,
                           const double *grid1_v, int grid1_v_count,
                           const SISLSurf *surf2,
                           const double *grid2_u, int grid2_u_count,
                           const double *grid2_v, int grid2_v_count,
                           const SislTraceOptions *options,
                           SislTraceResult **result)
{
    SISLSurf *s1 = NULL, *s2 = NULL, *c1 = NULL, *c2 = NULL;
    SISLIntcurve **curves = NULL;
    double *uv1 = NULL, *uv2 = NULL;
    int np = 0, nc = 0, st = 0, rc = SISL_TRACE_OK, i;
    int jstat = 0;
    size_t count = 0, cap = 0, total = 0;
    double scale = 1.0, geomtol;
    long maxst;
    Grids gr;

    if (result) *result = NULL;
    if (!result || !options) return SISL_TRACE_INVALID_ARGUMENT;
    if (!(options->geometric_tolerance > 0.0) ||
        !isfinite(options->geometric_tolerance))
        return SISL_TRACE_INVALID_ARGUMENT;
    if (options->max_stations <= 0) return SISL_TRACE_INVALID_ARGUMENT;
    geomtol = options->geometric_tolerance;
    maxst = options->max_stations;

    if (!surf1 || !surf2) return SISL_TRACE_INVALID_ARGUMENT;
    if (validate_surface(surf1) != 0) return SISL_TRACE_INVALID_ARGUMENT;
    if (validate_surface(surf2) != 0) return SISL_TRACE_INVALID_ARGUMENT;
    if (validate_grid(grid1_u, grid1_u_count, surf1, 0) != 0 ||
        validate_grid(grid1_v, grid1_v_count, surf1, 1) != 0 ||
        validate_grid(grid2_u, grid2_u_count, surf2, 0) != 0 ||
        validate_grid(grid2_v, grid2_v_count, surf2, 1) != 0)
        return SISL_TRACE_INVALID_ARGUMENT;

    {
        double sA, sB;
        if (surface_scale(surf1, &sA) != 0 || surface_scale(surf2, &sB) != 0)
            return SISL_TRACE_INVALID_ARGUMENT;
        scale = dmax(sA, sB);
    }

    /* the input surfaces are read-only; the marching kernel occasionally
       annotates its surfaces, so work on copies */
    c1 = copySurface((SISLSurf *)surf1);
    c2 = copySurface((SISLSurf *)surf2);
    if (!c1 || !c2) { rc = SISL_TRACE_NUMERIC; goto cleanup; }
    s1 = c1;
    s2 = c2;

    gr.g[0] = grid1_u; gr.n[0] = grid1_u_count;
    gr.g[1] = grid1_v; gr.n[1] = grid1_v_count;
    gr.g[2] = grid2_u; gr.n[2] = grid2_u_count;
    gr.g[3] = grid2_v; gr.n[3] = grid2_v_count;

    /* branch discovery: the existing s1859 discovery */
    s1859(s1, s2, 1e-12, geomtol, &np, &uv1, &uv2, &nc, &curves, &st);
    if (st < 0) { rc = SISL_TRACE_INTERSECTION; goto cleanup; }
    if (st > 0) { rc = SISL_TRACE_INTERSECTION; goto cleanup; }
    if (nc > TRACE_MAX_BRANCHES) { rc = SISL_TRACE_INTERSECTION; goto cleanup; }

    {
        SislTraceResult *res = (SislTraceResult *)calloc(1, sizeof(SislTraceResult));
        if (!res) { rc = SISL_TRACE_NUMERIC; goto cleanup; }

        for (i = 0; i < nc; ++i)
        {
            SISLIntcurve *ic = curves[i];
            HookCtx ctx;
            StnVec out = {0};
            int closed = 0, brc;
            long remaining;

            if (ic->itype == 9)
            {
                /* an "exact" curve means the surfaces coincide along a
                   parameter line: explicit rejection */
                rc = SISL_TRACE_REJECTED;
                free_result_inner(res);
                goto cleanup;
            }

            memset(&ctx, 0, sizeof(ctx));
            remaining = maxst - (long)total;
            if (remaining < 0) remaining = 0;
            ctx.limit = remaining;

            {
                S1310StationSink sink;
                sink.user = &ctx;
                sink.emit = hook_emit;
                s1310_station_sink = &sink;
                jstat = 0;
                s1310(s1, s2, ic, geomtol, 0.0, 2, 0, &jstat);
                s1310_station_sink = NULL;
            }

            if (ctx.overflow || jstat < 0)
            {
                stnvec_free(&ctx.vec);
                stnvec_free(&out);
                rc = ctx.overflow ? SISL_TRACE_BUDGET : SISL_TRACE_INTERSECTION;
                free_result_inner(res);
                goto cleanup;
            }
            if (ctx.vec.n == 0)
            {
                stnvec_free(&ctx.vec);
                stnvec_free(&out);
                rc = SISL_TRACE_INTERSECTION;
                free_result_inner(res);
                goto cleanup;
            }

            brc = build_branch(s1, s2, ctx.vec.v, ctx.vec.n, ctx.closed, &gr,
                               geomtol, scale, remaining, &out, &closed);
            stnvec_free(&ctx.vec);
            if (brc != SISL_TRACE_OK)
            {
                stnvec_free(&out);
                rc = brc;
                free_result_inner(res);
                goto cleanup;
            }
            if (out.n == 0)
            {
                stnvec_free(&out);
                continue;
            }
            if (maxst > 0 && total + (size_t)out.n > (size_t)maxst)
            {
                stnvec_free(&out);
                rc = SISL_TRACE_BUDGET;
                free_result_inner(res);
                goto cleanup;
            }
            if (result_append(&res, &cap, &count, &out, closed, &total) != 0)
            {
                stnvec_free(&out);
                rc = SISL_TRACE_NUMERIC;
                free_result_inner(res);
                goto cleanup;
            }
            stnvec_free(&out);
        }

        res->branch_count = count;
        res->station_count = total;
        *result = res;
        rc = SISL_TRACE_OK;
        if (count == 0)
        {
            /* no intersection: a valid empty result */
        }
    }

cleanup:
    if (curves) freeIntcrvlist(curves, nc);
    if (uv1) free(uv1);
    if (uv2) free(uv2);
    if (c1) freeSurf(c1);
    if (c2) freeSurf(c2);
    if (rc != SISL_TRACE_OK && result) *result = NULL;
    return rc;
}
