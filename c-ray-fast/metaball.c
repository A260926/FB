/* c-ray-fast - fused ball field groups (metaballs).
 * Copyright (C) 2006-2021 John Tsiombikas <nuclear@member.fsf.org>
 *
 * You are free to use, modify and redistribute this program under the terms
 * of the GNU General Public License v3 or later. See COPYING for details.
 * ---------------------------------------------------------------------
 * Production implementation of the fused ball field. Along a ray
 * p(t) = O + t D every active kernel is a quadratic in t squared, so inside
 * each interval where the set of contributing components is fixed the field
 * is a polynomial of degree at most 4. The supporting spheres only delimit
 * the active intervals; the kernels are always summed before the isosurface
 * F = tau is solved. Roots are isolated with a Sturm sequence (which counts
 * distinct roots without relying on sign changes, so tangent and other
 * even-multiplicity roots are found as well) and then refined to machine
 * granularity by sign bisection on g for crossing roots or on g' for tangent
 * extrema. No meshing and no fixed-step sampling take place.
 */
#include <stdlib.h>
#include <math.h>
#include <float.h>

#include "metaball.h"

#define STACK_COMP	32		/* larger groups fall back to heap storage */

void metaball_bounds(struct metaball *mb, struct aabox *box)
{
	int i;
	struct mb_component *cp;

	box->vmin.x = box->vmin.y = box->vmin.z = DBL_MAX;
	box->vmax.x = box->vmax.y = box->vmax.z = -DBL_MAX;

	for(i=0; i<mb->num_comp; i++) {
		double d;
		cp = mb->comp + i;

		d = cp->c.x - cp->r;
		if(d < box->vmin.x) box->vmin.x = d;
		d = cp->c.x + cp->r;
		if(d > box->vmax.x) box->vmax.x = d;
		d = cp->c.y - cp->r;
		if(d < box->vmin.y) box->vmin.y = d;
		d = cp->c.y + cp->r;
		if(d > box->vmax.y) box->vmax.y = d;
		d = cp->c.z - cp->r;
		if(d < box->vmin.z) box->vmin.z = d;
		d = cp->c.z + cp->r;
		if(d > box->vmax.z) box->vmax.z = d;
	}
}

double mb_field(struct metaball *mb, struct vec3 *p)
{
	int i;
	double f, dx, dy, dz, d2, r2, u;
	struct mb_component *cp;

	f = 0.0;
	for(i=0; i<mb->num_comp; i++) {
		cp = mb->comp + i;
		dx = p->x - cp->c.x;
		dy = p->y - cp->c.y;
		dz = p->z - cp->c.z;
		d2 = dx * dx + dy * dy + dz * dz;
		r2 = cp->r * cp->r;
		if(d2 >= r2) continue;	/* max(0, ...): outside the support it contributes nothing */
		u = 1.0 - d2 / r2;
		f += cp->w * u * u;
	}
	return f;
}

void mb_gradient(struct metaball *mb, struct vec3 *p, struct vec3 *grad)
{
	int i;
	double dx, dy, dz, d2, r2, u, k;
	struct mb_component *cp;

	grad->x = grad->y = grad->z = 0.0;
	for(i=0; i<mb->num_comp; i++) {
		cp = mb->comp + i;
		dx = p->x - cp->c.x;
		dy = p->y - cp->c.y;
		dz = p->z - cp->c.z;
		d2 = dx * dx + dy * dy + dz * dz;
		r2 = cp->r * cp->r;
		if(d2 >= r2) continue;
		u = 1.0 - d2 / r2;
		/* d/dp [w (1 - |p-c|^2/R^2)^2] = -4 w u (p-c) / R^2 */
		k = -4.0 * cp->w * u / r2;
		grad->x += k * dx;
		grad->y += k * dy;
		grad->z += k * dz;
	}
}

/* --- polynomial helpers (coefficients stored in ascending power order) --- */

static double poly_eval(const double *a, int deg, double x)
{
	double v = a[deg];
	while(deg-- > 0) {
		v = v * x + a[deg];
	}
	return v;
}

/* value and first derivative in a single Horner pass */
static void poly_eval_d(const double *a, int deg, double x, double *v, double *dv)
{
	double val = a[deg], der = 0.0;
	int i;

	for(i=deg - 1; i>=0; i--) {
		der = der * x + val;
		val = val * x + a[i];
	}
	*v = val;
	*dv = der;
}

static int poly_degree(const double *a, int maxdeg)
{
	int i;
	double mx = 0.0, eps;

	for(i=0; i<=maxdeg; i++) {
		double av = fabs(a[i]);
		if(av > mx) mx = av;
	}
	if(mx == 0.0) return -1;
	eps = mx * 1e-14;
	for(i=maxdeg; i>=0; i--) {
		if(fabs(a[i]) > eps) return i;
	}
	return -1;
}

/* Build the Sturm sequence of polynomial p (ascending coefficients, degree
 * deg). Returns the number of polynomials in the sequence. The sequence ends
 * at the last nonzero remainder, which keeps root counting exact even when p
 * carries repeated (tangent) roots. */
static int sturm_build(const double *p, int deg, double st[5][5], int sdeg[5])
{
	int i, k;

	for(i=0; i<5; i++) {
		st[0][i] = st[1][i] = 0.0;
	}
	for(i=0; i<=deg; i++) st[0][i] = p[i];
	sdeg[0] = deg;

	if(deg == 0) return 1;

	for(i=0; i<deg; i++) st[1][i] = (i + 1) * p[i + 1];
	sdeg[1] = deg - 1;

	k = 2;
	while(k < 5 && sdeg[k - 1] > 0) {
		double r[5], b[5];
		int da, db, j;

		for(i=0; i<5; i++) {
			r[i] = st[k - 2][i];
			b[i] = st[k - 1][i];
		}
		da = sdeg[k - 2];
		db = sdeg[k - 1];

		/* r = remainder(r, b), then negate for the Sturm sequence */
		while(da >= db) {
			double lead = r[da] / b[db];
			int shift = da - db;
			for(j=0; j<=db; j++) {
				r[shift + j] -= lead * b[j];
			}
			r[da] = 0.0;
			da = poly_degree(r, da - 1);
			if(da < 0) break;
		}
		for(i=0; i<5; i++) st[k][i] = -r[i];
		sdeg[k] = poly_degree(st[k], 4);
		if(sdeg[k] < 0) break;
		k++;
	}
	return k;
}

/* Sign variations of the Sturm sequence at x; zero evaluations are ignored,
 * which makes endpoint roots cancel out of the V(a)-V(b) difference. */
static int sturm_variations(double st[5][5], const int sdeg[5], int nseq, double x)
{
	int i, prev, sign, var;

	prev = 0;
	var = 0;
	for(i=0; i<nseq; i++) {
		double v = poly_eval(st[i], sdeg[i], x);
		if(v > 0.0) sign = 1;
		else if(v < 0.0) sign = -1;
		else sign = 0;
		if(!sign) continue;
		if(prev && sign != prev) var++;
		prev = sign;
	}
	return var;
}

/* Sign-bisection for h with opposite (or zero) signs at the bracket ends.
 * Runs to machine-granularity convergence, so the parametric error is set by
 * floating point resolution rather than by a fixed iteration count. When
 * deriv is non-zero the bisection runs on g' (used for tangent roots). */
static double sign_bisect(const double *poly, int polydeg, int deriv, double l, double r)
{
	double lo = l, hi = r, slo, shi, m, sm, dummy;
	int k;

	if(deriv) {
		poly_eval_d(poly, polydeg, lo, &dummy, &slo);
		poly_eval_d(poly, polydeg, hi, &dummy, &shi);
	} else {
		slo = poly_eval(poly, polydeg, lo);
		shi = poly_eval(poly, polydeg, hi);
	}

	for(k=0; k<80; k++) {
		m = (lo + hi) * 0.5;
		if(m == lo || m == hi) break;
		if(deriv) {
			poly_eval_d(poly, polydeg, m, &dummy, &sm);
		} else {
			sm = poly_eval(poly, polydeg, m);
		}
		if(sm == 0.0) return m;
		if((slo < 0.0) == (sm < 0.0)) {
			lo = m;
			slo = sm;
		} else {
			hi = m;
			shi = sm;
		}
	}
	return (lo + hi) * 0.5;
}

/* Refine the single distinct root known to lie in (l, r). The Sturm machinery
 * only isolates the bracket (it never relies on sign changes, so tangent and
 * other even-multiplicity roots are found as well). With exactly one distinct
 * root in the bracket, endpoint signs classify it: a sign change of g means an
 * odd-multiplicity root, refined by bisection on g; otherwise the root is a
 * tangent extremum, where g' changes sign, refined by bisection on g'. Both
 * run to machine-granularity convergence. */
static double sturm_refine(const double *poly, int polydeg, double l, double r)
{
	double gl, gr, dl, dr;
	int k;

	poly_eval_d(poly, polydeg, l, &gl, &dl);
	poly_eval_d(poly, polydeg, r, &gr, &dr);

	if(gl == 0.0) return l;
	if(gr == 0.0) return r;

	if((gl < 0.0) != (gr < 0.0)) {
		/* odd multiplicity: g crosses zero */
		return sign_bisect(poly, polydeg, 0, l, r);
	}

	/* even multiplicity (tangent): the touching extremum of g is the root. */
	{
		double t = sign_bisect(poly, polydeg, 1, l, r);
		double gt = poly_eval(poly, polydeg, t);
		double scale = MAX(1.0, MAX(fabs(gl), fabs(gr)));

		if(fabs(gt) <= 1e-10 * scale) {
			return t;	/* confirmed: the extremum sits on the zero level */
		}

		/* defensive: the bracketed g' extremum was not the touch point.
		 * Fall back to guarded Newton on g, which converges to the (double)
		 * root as long as every step stays inside the isolated bracket. */
		for(k=0; k<32; k++) {
			double v, dv, xn;
			poly_eval_d(poly, polydeg, t, &v, &dv);
			if(dv == 0.0) break;
			xn = t - v / dv;
			if(xn <= l || xn >= r || xn == t) break;
			t = xn;
		}
		return t;
	}
}

/* Isolate every distinct root of the Sturm-sequence polynomial inside (l, r)
 * and append the earliest one to *best when it is smaller than the current
 * value. Intervals are split left-first so roots arrive in ascending order. */
static void sturm_find_earliest(const double *poly, int polydeg,
		double st[5][5], const int sdeg[5], int nseq,
		double l, double r, int nroot, double *best)
{
	double m, nleft;

	if(nroot <= 0) return;
	if(nroot == 1) {
		double root = sturm_refine(poly, polydeg, l, r);
		if(root < *best) *best = root;
		return;
	}
	m = (l + r) * 0.5;
	if(m == l || m == r) {
		/* no more representable split: treat the cluster as one root */
		if(m < *best) *best = m;
		return;
	}
	/* a root landing exactly on the split point is captured explicitly, so
	 * it can never fall through the open-interval root counts. The left
	 * subinterval is still visited first (it may hold an earlier root). */
	nleft = sturm_variations(st, sdeg, nseq, l) - sturm_variations(st, sdeg, nseq, m);
	sturm_find_earliest(poly, polydeg, st, sdeg, nseq, l, m, (int)nleft, best);
	if(poly_eval(poly, polydeg, m) == 0.0 && m < *best) {
		*best = m;
	}
	sturm_find_earliest(poly, polydeg, st, sdeg, nseq, m, r, nroot - (int)nleft, best);
}

struct mb_event {
	double t;
	int comp;
	int enter;		/* 1: entering the support, 0: leaving it */
};

static int event_cmp(const void *ap, const void *bp)
{
	const struct mb_event *a = ap, *b = bp;
	if(a->t < b->t) return -1;
	if(a->t > b->t) return 1;
	if(a->comp != b->comp) return a->comp - b->comp;
	return a->enter - b->enter;
}

int ray_metaball(struct metaball *mb, struct ray *ray, struct spoint *sp)
{
	int i, n, heap;
	double lo, hi, best;
	double *ua = 0, *ub = 0, *uc = 0;		/* per-component quadratic u_i(t) */
	char *active = 0;
	struct mb_event *events = 0;
	int nev;
	double ua_stack[STACK_COMP], ub_stack[STACK_COMP], uc_stack[STACK_COMP];
	char active_stack[STACK_COMP];
	struct mb_event ev_stack[STACK_COMP * 2];

	if(mb->num_comp <= 0) return 0;

	n = mb->num_comp;
	lo = ERR_MARGIN;
	hi = 1.0;
	best = DBL_MAX;

	heap = 0;
	if(n <= STACK_COMP) {
		ua = ua_stack;
		ub = ub_stack;
		uc = uc_stack;
		active = active_stack;
		events = ev_stack;
	} else {
		heap = 1;
		if(!(ua = malloc(n * sizeof *ua)) || !(ub = malloc(n * sizeof *ub)) ||
				!(uc = malloc(n * sizeof *uc)) || !(active = malloc(n * sizeof *active)) ||
				!(events = malloc(n * 2 * sizeof *events))) {
			free(ua); free(ub); free(uc); free(active); free(events);
			return 0;
		}
	}

	nev = 0;
	for(i=0; i<n; i++) {
		struct mb_component *cp = mb->comp + i;
		double ex, ey, ez, r2, aa, bb, cc, disc, sq, t0, t1;

		ex = ray->orig.x - cp->c.x;
		ey = ray->orig.y - cp->c.y;
		ez = ray->orig.z - cp->c.z;
		r2 = cp->r * cp->r;

		/* u_i(t) = 1 - |e + t D|^2 / R^2 = ua t^2 + ub t + uc */
		ua[i] = -DOT(ray->dir, ray->dir) / r2;
		ub[i] = -2.0 * (ex * ray->dir.x + ey * ray->dir.y + ez * ray->dir.z) / r2;
		uc[i] = 1.0 - (ex * ex + ey * ey + ez * ez) / r2;

		/* supporting sphere intersection gives the entry/exit events */
		aa = DOT(ray->dir, ray->dir);
		bb = 2.0 * (ex * ray->dir.x + ey * ray->dir.y + ez * ray->dir.z);
		cc = ex * ex + ey * ey + ez * ez - r2;
		disc = bb * bb - 4.0 * aa * cc;
		if(disc <= 0.0) {
			/* no crossing, or a tangent touch that never changes activity */
			continue;
		}
		sq = sqrt(disc);
		t0 = (-bb - sq) / (2.0 * aa);
		t1 = (-bb + sq) / (2.0 * aa);

		if(t0 > lo && t0 < hi) {
			events[nev].t = t0;
			events[nev].comp = i;
			events[nev].enter = 1;
			nev++;
		}
		if(t1 > lo && t1 < hi) {
			events[nev].t = t1;
			events[nev].comp = i;
			events[nev].enter = 0;
			nev++;
		}
	}

	qsort(events, nev, sizeof *events, event_cmp);
	for(i=0; i<n; i++) active[i] = 0;

	/* initial activity at t = lo */
	for(i=0; i<n; i++) {
		double u = ua[i] * lo * lo + ub[i] * lo + uc[i];
		if(u > 0.0) active[i] = 1;
	}

	/* explicit check of the left endpoint: valid roots start exactly at lo.
	 * The tolerance is a few ulps of the field arithmetic, not a geometric
	 * epsilon, so a genuinely distinct surface only 1e-5 away is not swallowed. */
	{
		struct vec3 p;
		double fv;
		p.x = ray->orig.x + ray->dir.x * lo;
		p.y = ray->orig.y + ray->dir.y * lo;
		p.z = ray->orig.z + ray->dir.z * lo;
		fv = mb_field(mb, &p);
		if(fabs(fv - mb->tau) <= 8e-15 * MAX(1.0, MAX(fabs(fv), fabs(mb->tau)))) {
			best = lo;
		}
	}

	{
		double cur = lo;
		int evidx = 0;

		while(cur < hi) {
			double nxt, coef[5], st[5][5];
			int sdeg[5], nseq, nroot, any, j;

			if(evidx < nev) {
				nxt = events[evidx].t;
				if(nxt <= cur) nxt = hi;	/* defensive, events are sorted and interior */
			} else {
				nxt = hi;
			}

			/* roots strictly inside (cur, nxt), activity is constant here */
			coef[0] = coef[1] = coef[2] = coef[3] = coef[4] = 0.0;
			any = 0;
			for(i=0; i<n; i++) {
				if(!active[i]) continue;
				any = 1;
				/* w * (ua t^2 + ub t + uc)^2 */
				double w = mb->comp[i].w;
				double a = ua[i], b = ub[i], c = uc[i];
				coef[0] += w * c * c;
				coef[1] += w * 2.0 * b * c;
				coef[2] += w * (b * b + 2.0 * a * c);
				coef[3] += w * 2.0 * a * b;
				coef[4] += w * a * a;
			}
			coef[0] -= mb->tau;		/* solve g(t) - tau = 0 */

			if(any) {
				int deg = poly_degree(coef, 4);
				if(deg >= 0) {
					nseq = sturm_build(coef, deg, st, sdeg);
					nroot = sturm_variations(st, sdeg, nseq, cur) -
							sturm_variations(st, sdeg, nseq, nxt);
					if(nroot > 0 && cur < best) {
						sturm_find_earliest(coef, deg, st, sdeg, nseq, cur, nxt, nroot, &best);
					}
				}
			}

			/* endpoint check at nxt (covers roots sitting on a support event).
			 * ulp-scale tolerance so nearby but distinct surfaces stay apart. */
			{
				struct vec3 p;
				double fv;
				p.x = ray->orig.x + ray->dir.x * nxt;
				p.y = ray->orig.y + ray->dir.y * nxt;
				p.z = ray->orig.z + ray->dir.z * nxt;
				fv = mb_field(mb, &p);
				if(fabs(fv - mb->tau) <= 8e-15 * MAX(1.0, MAX(fabs(fv), fabs(mb->tau)))) {
					if(nxt < best) best = nxt;
				}
			}

			if(best != DBL_MAX) break;	/* segments are visited left to right */

			/* apply every event at nxt and advance */
			while(evidx < nev && events[evidx].t == nxt) {
				j = events[evidx].comp;
				active[j] = events[evidx].enter ? 1 : 0;
				evidx++;
			}
			cur = nxt;
			if(cur >= hi) break;
		}
	}

	if(heap) {
		free(ua);
		free(ub);
		free(uc);
		free(active);
		free(events);
	}

	if(best == DBL_MAX) return 0;

	if(sp) {
		struct vec3 grad;
		double len;

		sp->dist = best;
		sp->pos.x = ray->orig.x + ray->dir.x * best;
		sp->pos.y = ray->orig.y + ray->dir.y * best;
		sp->pos.z = ray->orig.z + ray->dir.z * best;

		mb_gradient(mb, &sp->pos, &grad);
		len = sqrt(DOT(grad, grad));
		if(len == 0.0) return 0;	/* guaranteed not to happen for valid scenes */
		/* outward normal points toward decreasing F */
		sp->normal.x = -grad.x / len;
		sp->normal.y = -grad.y / len;
		sp->normal.z = -grad.z / len;

		reflect(&sp->vref, &ray->dir, &sp->normal);
		NORMALIZE(sp->vref);
	}
	return 1;
}
