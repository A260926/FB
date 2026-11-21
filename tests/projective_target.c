/* Projective instancing regression tests.
 *
 * Registered through task_support/check.h as three groups (parameter,
 * hierarchy, render); no main() is defined here.
 *
 * The independent reference pushes a world ray through the inverse matrix and
 * solves the local sphere equation in homogeneous coordinates itself, rather
 * than calling the production intersection. Normals are cross-checked through
 * a numerically differentiated forward map (the production code instead uses
 * the inverse-matrix Jacobian), and outward orientation is confirmed by
 * finite-differencing the transformed implicit surface.
 */
#include "../task_support/check.h"
#include "projective.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

/* ---- renderer globals/functions reused through the production pipeline --- */
extern struct object *objects;
extern struct object *infobj;
extern struct material *materials;
extern struct light *lights;
extern int num_obj, num_inf, num_mtl, num_lt;
extern struct color ambcol, bgcol;
void trace(struct ray *ray, int depth, struct color *col);
int ray_scene(struct ray *ray, struct spoint *sp);

/* ------------------------------------------------------------------ */
/* independent reference                                               */
/* ------------------------------------------------------------------ */

static int ref_invert4(const double *m, double *o)
{
	double inv[16];
	inv[0]  = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
	inv[4]  = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
	inv[8]  = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
	inv[1]  = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
	inv[5]  = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]-m[12]*m[2]*m[11]+m[12]*m[3]*m[10];
	inv[9]  = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
	inv[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
	inv[2]  = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
	inv[6]  = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
	inv[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
	inv[3]  = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
	inv[7]  = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
	inv[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
	{
		double det = m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
		int i;
		if(det == 0.0 || !isfinite(det)) return -1;
		for(i = 0; i < 16; i++) o[i] = inv[i] / det;
	}
	return 0;
}

/* forward projective map of a local point, used for the numeric normal */
static void ref_forward(const double *m, const double *L, double *out)
{
	double h[4] = {L[0], L[1], L[2], 1.0};
	double num[3], den;
	int k;
	for(k = 0; k < 3; k++) {
		num[k] = m[4*k]*h[0] + m[4*k+1]*h[1] + m[4*k+2]*h[2] + m[4*k+3]*h[3];
	}
	den = m[12]*h[0] + m[13]*h[1] + m[14]*h[2] + m[15]*h[3];
	out[0] = num[0] / den;
	out[1] = num[1] / den;
	out[2] = num[2] / den;
}

struct ref_hit {
	double t;
	double n[3];
	int sphere;
};

/* Independent nearest-hit reference over a set of local spheres. Returns 1
 * when a finite hit exists in ERR_MARGIN <= t <= 1 with Wh != 0.
 */
static int ref_nearest(const double *m, const struct sphere *sph, int nsph,
		const struct ray *ray, struct ref_hit *out)
{
	double H[16];
	double O[3], D[3], A0[3], Ad[3], w0, wd;
	int i, found = 0;
	double best = 0.0;
	int best_i = -1;

	if(ref_invert4(m, H)) return 0;
	O[0] = ray->orig.x; O[1] = ray->orig.y; O[2] = ray->orig.z;
	D[0] = ray->dir.x; D[1] = ray->dir.y; D[2] = ray->dir.z;
	for(i = 0; i < 3; i++) {
		int r = 4 * i;
		A0[i] = H[r]*O[0] + H[r+1]*O[1] + H[r+2]*O[2] + H[r+3];
		Ad[i] = H[r]*D[0] + H[r+1]*D[1] + H[r+2]*D[2];
	}
	w0 = H[12]*O[0] + H[13]*O[1] + H[14]*O[2] + H[15];
	wd = H[12]*D[0] + H[13]*D[1] + H[14]*D[2];

	for(i = 0; i < nsph; i++) {
		double C[3] = {sph[i].pos.x, sph[i].pos.y, sph[i].pos.z};
		double r = sph[i].rad;
		double P[3], Q[3], a, b, c, disc, roots[2];
		int k, nr = 0;

		for(k = 0; k < 3; k++) {
			P[k] = A0[k] - C[k] * w0;
			Q[k] = Ad[k] - C[k] * wd;
		}
		a = Q[0]*Q[0]+Q[1]*Q[1]+Q[2]*Q[2] - r*r*wd*wd;
		b = 2.0*(P[0]*Q[0]+P[1]*Q[1]+P[2]*Q[2] - r*r*w0*wd);
		c = P[0]*P[0]+P[1]*P[1]+P[2]*P[2] - r*r*w0*w0;

		if(a == 0.0) {
			if(b != 0.0) roots[nr++] = -c / b;
		} else {
			disc = b*b - 4.0*a*c;
			if(disc >= 0.0) {
				double q = sqrt(disc);
				roots[nr++] = (-b - q) / (2.0 * a);
				roots[nr++] = (-b + q) / (2.0 * a);
			}
		}
		for(k = 0; k < nr; k++) {
			double t = roots[k];
			double w = w0 + wd * t;
			if(t >= ERR_MARGIN && t <= 1.0 && w != 0.0 && isfinite(w)) {
				if(!found || t < best) { best = t; best_i = i; found = 1; }
			}
		}
	}

	if(!found) return 0;
	out->t = best;
	out->sphere = best_i;

	/* independent normal: numeric forward-map Jacobian, inverse transpose */
	{
		double t = best, w = w0 + wd * t;
		double L[3], nL[3], F0[3], K[3][3], Kinv[3][3];
		const double eps = 1e-6;
		int k, j;

		for(k = 0; k < 3; k++) L[k] = (A0[k] + Ad[k] * t) / w;
		for(k = 0; k < 3; k++) nL[k] = (L[k] - (k==0?sph[best_i].pos.x:k==1?sph[best_i].pos.y:sph[best_i].pos.z)) / sph[best_i].rad;

		ref_forward(m, L, F0);
		for(j = 0; j < 3; j++) {
			double Lp[3], Fp[3], Lm[3], Fm[3];
			for(k = 0; k < 3; k++) { Lp[k] = L[k]; Lm[k] = L[k]; }
			Lp[j] += eps; Lm[j] -= eps;
			ref_forward(m, Lp, Fp);
			ref_forward(m, Lm, Fm);
			for(k = 0; k < 3; k++) K[k][j] = (Fp[k] - Fm[k]) / (2.0 * eps);
		}
		/* invert 3x3 K */
		{
			double det;
			for(k = 0; k < 3; k++) for(j = 0; j < 3; j++) Kinv[k][j] = 0.0;
			det = K[0][0]*(K[1][1]*K[2][2]-K[1][2]*K[2][1])
			    - K[0][1]*(K[1][0]*K[2][2]-K[1][2]*K[2][0])
			    + K[0][2]*(K[1][0]*K[2][1]-K[1][1]*K[2][0]);
			if(det == 0.0) return 0;
			Kinv[0][0] = (K[1][1]*K[2][2]-K[1][2]*K[2][1])/det;
			Kinv[0][1] = (K[0][2]*K[2][1]-K[0][1]*K[2][2])/det;
			Kinv[0][2] = (K[0][1]*K[1][2]-K[0][2]*K[1][1])/det;
			Kinv[1][0] = (K[1][2]*K[2][0]-K[1][0]*K[2][2])/det;
			Kinv[1][1] = (K[0][0]*K[2][2]-K[0][2]*K[2][0])/det;
			Kinv[1][2] = (K[0][2]*K[1][0]-K[0][0]*K[1][2])/det;
			Kinv[2][0] = (K[1][0]*K[2][1]-K[1][1]*K[2][0])/det;
			Kinv[2][1] = (K[0][1]*K[2][0]-K[0][0]*K[2][1])/det;
			Kinv[2][2] = (K[0][0]*K[1][1]-K[0][1]*K[1][0])/det;
		}
		/* n_w = K^-T nL */
		for(k = 0; k < 3; k++) {
			out->n[k] = Kinv[0][k]*nL[0] + Kinv[1][k]*nL[1] + Kinv[2][k]*nL[2];
		}
		{
			double len = sqrt(out->n[0]*out->n[0]+out->n[1]*out->n[1]+out->n[2]*out->n[2]);
			for(k = 0; k < 3; k++) out->n[k] /= len;
		}
	}
	return 1;
}

/* finite-difference outward check on the transformed implicit surface */
static int ref_outward(const double *m, const struct spoint *sp, const struct sphere *s)
{
	double H[16];
	double base[3] = {sp->pos.x, sp->pos.y, sp->pos.z};
	double n[3] = {sp->normal.x, sp->normal.y, sp->normal.z};
	double step = 1e-5;
	double po[3], mo[3], ph[4], mh[4], d_out, d_in;
	int k;

	if(ref_invert4(m, H)) return 0;
	for(k = 0; k < 3; k++) {
		po[k] = base[k] + n[k] * step;
		mo[k] = base[k] - n[k] * step;
	}
	for(k = 0; k < 4; k++) {
		ph[k] = H[4*k]*po[0] + H[4*k+1]*po[1] + H[4*k+2]*po[2] + H[4*k+3];
		mh[k] = H[4*k]*mo[0] + H[4*k+1]*mo[1] + H[4*k+2]*mo[2] + H[4*k+3];
	}
	{
		double lp[3], lm[3];
		for(k = 0; k < 3; k++) { lp[k] = ph[k] / ph[3]; lm[k] = mh[k] / mh[3]; }
		d_out = sqrt(SQ(lp[0]-s->pos.x)+SQ(lp[1]-s->pos.y)+SQ(lp[2]-s->pos.z)) - s->rad;
		d_in  = sqrt(SQ(lm[0]-s->pos.x)+SQ(lm[1]-s->pos.y)+SQ(lm[2]-s->pos.z)) - s->rad;
	}
	return d_out > 0.0 && d_in < 0.0;
}

static FILE *scene_file(const char *text)
{
	const char *dir = getenv("TMPDIR");
	char path[512];
	FILE *f;
	if(!dir || !*dir) dir = ".";
	snprintf(path, sizeof path, "%s/cray_proj_%ld.scn", dir, (long)getpid());
	f = fopen(path, "w+b");
	if(!f) return 0;
	fputs(text, f);
	rewind(f);
	remove(path);
	return f;
}

static int load_text(const char *text)
{
	FILE *f = scene_file(text);
	int r;
	if(!f) return -1;
	r = load_scene(f);
	fclose(f);
	return r;
}

/* ------------------------------------------------------------------ */
/* parameter group                                                     */
/* ------------------------------------------------------------------ */

static int case_illegal_records(void)
{
	/* instance referencing a prototype that does not exist yet */
	TASK_CHECK(load_text(
		"M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"i \"bad\" \"ghost\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n") == -1);

	/* unknown material on a prototype sphere */
	TASK_CHECK(load_text(
		"M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 1 \"nope\"\n") == -1);

	/* non-positive radius */
	TASK_CHECK(load_text(
		"M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 0 \"m\"\n") == -1);
	TASK_CHECK(load_text(
		"M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 -2 \"m\"\n") == -1);

	/* non-finite radius */
	TASK_CHECK(load_text(
		"M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 inf \"m\"\n") == -1);

	/* singular matrix */
	TASK_CHECK(load_text(
		"M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 1 \"m\"\n"
		"i \"I\" \"P\" 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1\n") == -1);

	/* non-finite matrix entry */
	TASK_CHECK(load_text(
		"M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 1 \"m\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 nan 0 0 1\n") == -1);

	/* duplicate instance name */
	TASK_CHECK(load_text(
		"M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 1 \"m\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n") == -1);

	/* a fully valid scene loads cleanly */
	TASK_CHECK(load_text(
		"M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 1 \"m\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n") == 0);
	return 0;
}

static int case_append_and_identity(void)
{
	struct spoint sp;
	struct ray ray = {{-5, 0, 0}, {10, 0, 0}};

	TASK_CHECK(load_text(
		"M \"a\" 1 0 0 1 1 1 50 0 0 0\n"
		"M \"b\" 0 1 0 1 1 1 50 0 0 0\n"
		/* three separate spheres appended to the same prototype */
		"p \"P\" 0 0 0 1 \"a\"\n"
		"p \"P\" 3 0 0 1 \"b\"\n"
		"p \"P\" 6 0 0 1 \"a\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n") == 0);
	TASK_CHECK(prepare_scene() == 0);
	TASK_CHECK(proj_find_prototype("P") != 0);
	TASK_CHECK(proj_find_prototype("P")->num_sph == 3);

	/* the ray along +x meets the sphere at local x = -1 first */
	TASK_CHECK(proj_intersect(&ray, &sp, 0) == 1);
	TASK_CHECK(fabs(sp.dist - 0.4) < 1e-7);
	TASK_CHECK(strcmp(proj_hit_instance(&sp), "I") == 0);
	TASK_CHECK(strcmp(proj_hit_prototype(&sp), "P") == 0);
	TASK_CHECK(proj_hit_sphere(&sp) == 0);
	TASK_CHECK(sp.obj == 0 && sp.inst != 0 && sp.psph != 0);
	return 0;
}

static int case_matrix_scalar_invariance(void)
{
	struct spoint s1, s2;
	struct ray ray = {{-4, 1, -2}, {8, -2, 4}};
	double base[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
	double scaled[16];
	int k;

	for(k = 0; k < 16; k++) scaled[k] = base[k] * -3.7;

	load_text("M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 1 \"m\"\n"
		"i \"A\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n");
	prepare_scene();
	TASK_CHECK(proj_intersect(&ray, &s1, 0) == 1);

	proj_clear();
	proj_bind_materials(materials, num_mtl);
	{
		struct sphere sph = {{0,0,0}, 1.0};
		TASK_CHECK(proj_add_proto_sphere("P", &sph, 0) == 0);
		TASK_CHECK(proj_add_instance("B", "P", scaled) == 0);
	}
	prepare_scene();
	TASK_CHECK(proj_intersect(&ray, &s2, 0) == 1);

	TASK_CHECK(fabs(s1.dist - s2.dist) < 1e-9);
	TASK_CHECK(fabs(s1.normal.x - s2.normal.x) < 1e-9);
	TASK_CHECK(fabs(s1.normal.y - s2.normal.y) < 1e-9);
	TASK_CHECK(fabs(s1.normal.z - s2.normal.z) < 1e-9);
	return 0;
}

static int case_nonunit_direction(void)
{
	struct spoint sp;
	struct ref_hit rh;
	struct sphere sph = {{0,0,0}, 1.0};
	double M[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
	/* deliberately long, non-normalized direction */
	struct ray ray = {{-5, 0.3, -0.2}, {12.0, -0.7, 0.4}};

	load_text("M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 1 \"m\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n");
	prepare_scene();
	TASK_CHECK(proj_intersect(&ray, &sp, 0) == 1);
	TASK_CHECK(sp.dist >= ERR_MARGIN && sp.dist <= 1.0);
	TASK_CHECK(ref_nearest(M, &sph, 1, &ray, &rh) == 1);
	TASK_CHECK(fabs(sp.dist - rh.t) < 1e-7);
	return 0;
}

static int case_internal_start(void)
{
	struct spoint sp;
	/* origin inside the unit sphere; nearest hit is the far surface */
	struct ray ray = {{0.2, 0, 0}, {5, 0, 0}};

	load_text("M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 1 \"m\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n");
	prepare_scene();
	TASK_CHECK(proj_intersect(&ray, &sp, 0) == 1);
	/* far side of the unit sphere at world x = 1 */
	TASK_CHECK(fabs((ray.orig.x + ray.dir.x * sp.dist) - 1.0) < 1e-7);
	TASK_CHECK(sp.normal.x > 0.99); /* outward, toward +x */
	TASK_CHECK(fabs(sqrt(DOT(sp.normal, sp.normal)) - 1.0) < 1e-12);
	return 0;
}

static int case_pole_excluded(void)
{
	/* perspective projection: world homogeneous Wh = 1 - Z_local. The
	 * inverse denominator is Wh = 1 + z_world, so the pole plane in world
	 * space is z = -1. A ray parallel to that plane through z = -1 maps
	 * entirely to the pole and must produce no finite hit.
	 */
	struct ray on_pole = {{-5, 0, -1}, {10, 0, 0}};
	struct spoint sp;

	load_text("M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 1.5 1 \"m\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 -1 1\n");
	prepare_scene();
	TASK_CHECK(proj_unbounded_count() == 1);
	TASK_CHECK(proj_intersect(&on_pole, &sp, 0) == 0);
	return 0;
}

static int case_shadow_range(void)
{
	struct ray inside, beyond;
	struct spoint sp;

	load_text("M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 1 \"m\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n");
	prepare_scene();

	/* occlusion query finds the sphere */
	inside = (struct ray){{-5, 0, 0}, {10, 0, 0}};
	TASK_CHECK(proj_intersect(&inside, 0, 0) == 1);
	TASK_CHECK(ray_scene(&inside, 0) == 1);

	/* sphere only exists beyond t = 1, so the segment reports no hit */
	beyond = (struct ray){{-50, 0, 0}, {10, 0, 0}};
	TASK_CHECK(proj_intersect(&beyond, &sp, 0) == 0);
	TASK_CHECK(ray_scene(&beyond, 0) == 0);

	/* direction too short to reach it */
	beyond = (struct ray){{-5, 0, 0}, {1, 0, 0}};
	TASK_CHECK(proj_intersect(&beyond, 0, 0) == 0);
	return 0;
}

static int case_normal_direction(void)
{
	struct spoint sp;
	struct sphere sph = {{0,0,1.5}, 1.0};
	double M[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,-1,1};
	struct ray ray = {{-4, 0.4, 0}, {8, -0.8, 2.0}};

	load_text("M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 1.5 1 \"m\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 -1 1\n");
	prepare_scene();
	TASK_CHECK(proj_intersect(&ray, &sp, 0) == 1);
	TASK_CHECK(fabs(sqrt(DOT(sp.normal, sp.normal)) - 1.0) < 1e-12);
	TASK_CHECK(ref_outward(M, &sp, &sph) == 1);
	return 0;
}

/* ------------------------------------------------------------------ */
/* hierarchy group                                                     */
/* ------------------------------------------------------------------ */

static int case_cross_pole_branches(void)
{
	struct sphere sph = {{0,0,1.5}, 1.0};
	double M[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,-1,1};
	unsigned seed = 987654321u;
	int trials = 0, crossed = 0, i;

	load_text("M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 1.5 1 \"m\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 -1 1\n");
	prepare_scene();

	for(i = 0; i < 4000; i++) {
		struct ray ray;
		struct spoint sp;
		struct ref_hit rh;
		int got, ref;
		double tpole;

		seed = seed * 1103515245u + 12345u;
		ray.orig.x = 16.0 * ((seed >> 8) & 0xffff) / 65535.0 - 8.0;
		seed = seed * 1103515245u + 12345u;
		ray.orig.y = 16.0 * ((seed >> 8) & 0xffff) / 65535.0 - 8.0;
		seed = seed * 1103515245u + 12345u;
		ray.orig.z = 16.0 * ((seed >> 8) & 0xffff) / 65535.0 - 8.0;
		seed = seed * 1103515245u + 12345u;
		ray.dir.x = 16.0 * ((seed >> 8) & 0xffff) / 65535.0 - 8.0;
		seed = seed * 1103515245u + 12345u;
		ray.dir.y = 16.0 * ((seed >> 8) & 0xffff) / 65535.0 - 8.0;
		seed = seed * 1103515245u + 12345u;
		ray.dir.z = 16.0 * ((seed >> 8) & 0xffff) / 65535.0 - 8.0;
		if(DOT(ray.dir, ray.dir) < 1e-6) continue;
		trials++;

		/* pole plane in world space is z = -1 (inverse Wh = 1 + z_w) */
		if(ray.dir.z != 0.0) {
			tpole = (-1.0 - ray.orig.z) / ray.dir.z;
			if(tpole > ERR_MARGIN && tpole < 1.0) crossed++;
		}

		got = proj_intersect(&ray, &sp, 0);
		ref = ref_nearest(M, &sph, 1, &ray, &rh);
		TASK_CHECK(got == ref);
		if(got) {
			TASK_CHECK(fabs(sp.dist - rh.t) < 1e-7);
			TASK_CHECK(fabs(sp.normal.x - rh.n[0]) < 1e-5);
			TASK_CHECK(fabs(sp.normal.y - rh.n[1]) < 1e-5);
			TASK_CHECK(fabs(sp.normal.z - rh.n[2]) < 1e-5);
		}
	}
	TASK_CHECK(trials > 3000);
	TASK_CHECK(crossed > 100); /* the suite really exercised both branches */
	return 0;
}

static int case_cross_pole_two_branches(void)
{
	/* fixed ray whose two sphere roots straddle the pole, so a valid finite
	 * surface point exists on BOTH sides of the pole. Production must still
	 * return the single earliest world hit regardless of local branch order.
	 */
	struct sphere sph = {{0,0,1.5}, 1.0};
	double M[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,-1,1};
	struct ray ray = {{-0.5800, -0.7299, -2.7719}, {3.2028, 0.2975, 7.7786}};
	struct spoint sp;
	struct ref_hit rh;
	double tpole;
	double H[16], A0[3], Ad[3], w0, wd, P[3], Q[3], a, b, c, disc, q, roots[2];
	int k;

	load_text("M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 1.5 1 \"m\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 -1 1\n");
	prepare_scene();
	TASK_CHECK(proj_unbounded_count() == 1);

	tpole = (-1.0 - ray.orig.z) / ray.dir.z;
	TASK_CHECK(tpole > ERR_MARGIN && tpole < 1.0);

	/* independent quadratic roots, proven to straddle the pole */
	TASK_CHECK(ref_invert4(M, H) == 0);
	for(k = 0; k < 3; k++) {
		int rr = 4 * k;
		A0[k] = H[rr]*ray.orig.x + H[rr+1]*ray.orig.y + H[rr+2]*ray.orig.z + H[rr+3];
		Ad[k] = H[rr]*ray.dir.x + H[rr+1]*ray.dir.y + H[rr+2]*ray.dir.z;
	}
	w0 = H[12]*ray.orig.x + H[13]*ray.orig.y + H[14]*ray.orig.z + H[15];
	wd = H[12]*ray.dir.x + H[13]*ray.dir.y + H[14]*ray.dir.z;
	for(k = 0; k < 3; k++) {
		double C = k == 0 ? sph.pos.x : k == 1 ? sph.pos.y : sph.pos.z;
		P[k] = A0[k] - C * w0;
		Q[k] = Ad[k] - C * wd;
	}
	a = Q[0]*Q[0]+Q[1]*Q[1]+Q[2]*Q[2] - sph.rad*sph.rad*wd*wd;
	b = 2.0*(P[0]*Q[0]+P[1]*Q[1]+P[2]*Q[2] - sph.rad*sph.rad*w0*wd);
	c = P[0]*P[0]+P[1]*P[1]+P[2]*P[2] - sph.rad*sph.rad*w0*w0;
	disc = b*b - 4.0*a*c;
	TASK_CHECK(disc >= 0.0 && a != 0.0);
	q = sqrt(disc);
	roots[0] = (-b - q) / (2.0 * a);
	roots[1] = (-b + q) / (2.0 * a);
	for(k = 0; k < 2; k++) {
		double w = w0 + wd * roots[k];
		TASK_CHECK(roots[k] >= ERR_MARGIN && roots[k] <= 1.0);
		TASK_CHECK(w != 0.0 && isfinite(w));
	}
	/* one finite hit on each side of the pole */
	TASK_CHECK((roots[0] < tpole && roots[1] > tpole) ||
	           (roots[1] < tpole && roots[0] > tpole));

	/* production returns the earliest world hit, matching the reference */
	TASK_CHECK(proj_intersect(&ray, &sp, 0) == 1);
	TASK_CHECK(ref_nearest(M, &sph, 1, &ray, &rh) == 1);
	TASK_CHECK(fabs(sp.dist - rh.t) < 1e-7);
	TASK_CHECK(fabs(sp.dist - MIN(roots[0], roots[1])) < 1e-7);
	TASK_CHECK(fabs(sp.normal.x - rh.n[0]) < 1e-5);
	TASK_CHECK(fabs(sp.normal.y - rh.n[1]) < 1e-5);
	TASK_CHECK(fabs(sp.normal.z - rh.n[2]) < 1e-5);
	TASK_CHECK(ref_outward(M, &sp, &sph) == 1);
	return 0;
}

static int case_octree_pruning(void)
{
	/* two well-separated clusters recorded as their own sphere groups */
	const int N = 16;
	int ix, iy;
	struct proj_query_stats miss_st, hit_st;
	struct ray miss_ray, hit_ray;
	struct spoint sp;

	proj_clear();
	{
		static struct material mtls[1];
		mtls[0].name = "m";
		mtls[0].kd = (struct color){1,1,1,1};
		mtls[0].ks = (struct color){1,1,1,1};
		mtls[0].spow = 50;
		proj_bind_materials(mtls, 1);
	}
	for(ix = 0; ix < N; ix++) {
		for(iy = 0; iy < N; iy++) {
			struct sphere s = {{ix * 3.0, iy * 3.0, 0}, 0.5};
			TASK_CHECK(proj_add_proto_sphere("grid", &s, 0) == 0);
		}
	}
	{
		double M[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
		TASK_CHECK(proj_add_instance("g", "grid", M) == 0);
	}
	TASK_CHECK(prepare_scene() == 0);
	TASK_CHECK(proj_unbounded_count() == 0);

	/* ray that threads the gaps: traverses the tree but hits nothing */
	miss_ray = (struct ray){{7.5, 7.5, -10}, {0, 0, 20}};
	TASK_CHECK(proj_intersect(&miss_ray, &sp, &miss_st) == 0);

	/* ray that reaches one leaf sphere */
	hit_ray = (struct ray){{0.5, 0.0, -10}, {0, 0, 20}};
	TASK_CHECK(proj_intersect(&hit_ray, &sp, &hit_st) == 1);

	/* the local tree must prune: far fewer leaf tests than total spheres,
	 * and nodes were actually culled.
	 */
	TASK_CHECK(miss_st.leaf_tests > 0);
	TASK_CHECK(miss_st.leaf_tests < (unsigned long)N * N);
	TASK_CHECK(hit_st.leaf_tests < (unsigned long)N * N);
	TASK_CHECK(miss_st.node_tests > 1);
	return 0;
}

static int case_shared_tree_multi_instance(void)
{
	struct spoint s1, s2;
	struct ray r1 = {{-5, 0, 0}, {10, 0, 0}};
	struct ray r2 = {{5, 0, 0}, {-10, 0, 0}};
	struct instance *a, *b;

	load_text(
		"M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 1 \"m\"\n"
		/* instance A at x = -3, instance B at x = +3 */
		"i \"A\" \"P\" 1 0 0 -3 0 1 0 0 0 0 1 0 0 0 0 1\n"
		"i \"B\" \"P\" 1 0 0 3 0 1 0 0 0 0 1 0 0 0 0 1\n");
	TASK_CHECK(prepare_scene() == 0);
	TASK_CHECK(proj_instance_count() == 2);

	a = proj_find_instance("A");
	b = proj_find_instance("B");
	TASK_CHECK(a != 0 && b != 0);
	/* both instances share the single prototype tree */
	TASK_CHECK(a->proto == b->proto);
	TASK_CHECK(a->proto->tree != 0);

	TASK_CHECK(proj_intersect(&r1, &s1, 0) == 1);
	TASK_CHECK(strcmp(proj_hit_instance(&s1), "A") == 0);
	TASK_CHECK(strcmp(proj_hit_prototype(&s1), "P") == 0);
	TASK_CHECK(fabs(s1.pos.x - (-4.0)) < 1e-7);

	TASK_CHECK(proj_intersect(&r2, &s2, 0) == 1);
	TASK_CHECK(strcmp(proj_hit_instance(&s2), "B") == 0);
	TASK_CHECK(fabs(s2.pos.x - 4.0) < 1e-7);
	return 0;
}

static int case_rebuild_update(void)
{
	struct spoint sp;
	struct ray ray = {{-5, 0, 0}, {10, 0, 0}};
	struct instance *in;
	double *m;

	load_text("M \"m\" 1 1 1 1 1 1 50 0 0 0\n"
		"p \"P\" 0 0 0 1 \"m\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n");
	prepare_scene();
	TASK_CHECK(proj_intersect(&ray, &sp, 0) == 1);
	TASK_CHECK(fabs(sp.dist - 0.4) < 1e-9); /* sphere surface at x = -1 */

	/* move the instance two units to the right, rebuild, and re-query */
	in = proj_find_instance("I");
	TASK_CHECK(in != 0);
	m = in->m;
	m[3] = 2.0; /* translation column */
	TASK_CHECK(prepare_scene() == 0);
	TASK_CHECK(proj_intersect(&ray, &sp, 0) == 1);
	/* surface now at world x = 1 => t = (1 - (-5)) / 10 = 0.6 */
	TASK_CHECK(fabs(sp.dist - 0.6) < 1e-9);
	TASK_CHECK(fabs(sp.pos.x - 1.0) < 1e-9);

	/* grow the prototype sphere, rebuild again */
	in->proto->sph[0].sph.rad = 2.0;
	TASK_CHECK(prepare_scene() == 0);
	TASK_CHECK(proj_intersect(&ray, &sp, 0) == 1);
	/* near surface at x = 2 - 2 = 0 => t = 0.5 */
	TASK_CHECK(fabs(sp.dist - 0.5) < 1e-9);
	return 0;
}

/* ------------------------------------------------------------------ */
/* render group                                                        */
/* ------------------------------------------------------------------ */

static void setup_lighting(void)
{
	ambcol = (struct color){0.02f, 0.02f, 0.02f, 0};
	bgcol = (struct color){0.0f, 0.0f, 0.05f, 0};
}

static int case_render_instance_color(void)
{
	struct ray ray = {{-5, 0, 0}, {10, 0, 0}};
	struct color col;
	struct spoint sp;

	setup_lighting();
	TASK_CHECK(load_text(
		"M \"white\" 1 1 1 1 1 1 0 0 0 0\n"
		"p \"P\" 0 0 0 1 \"white\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n"
		"l -10 0 0 1 1 1\n") == 0);
	TASK_CHECK(prepare_scene() == 0);

	/* production trace/shade path */
	trace(&ray, 0, &col);
	TASK_CHECK(proj_intersect(&ray, &sp, 0) == 1);
	/* lit frontally: ambient + full diffuse, clearly above the background */
	TASK_CHECK(col.r > 0.9f);
	TASK_CHECK(col.g > 0.9f);
	TASK_CHECK(col.b > 0.9f);
	TASK_CHECK(col.a > 0.5f);
	TASK_CHECK(col.r > bgcol.r + 0.5f);
	return 0;
}

static int case_render_shadow_occlusion(void)
{
	struct ray ray = {{-5, 0, 0}, {10, 0, 0}};
	struct color lit, shadowed;
	struct ray shadow_ray;
	struct spoint sp;

	setup_lighting();

	/* unoccluded: light at -x, nothing between the hit and the light */
	TASK_CHECK(load_text(
		"M \"white\" 1 1 1 1 1 1 0 0 0 0\n"
		"p \"P\" 0 0 0 1 \"white\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n"
		"l -10 0 0 1 1 1\n") == 0);
	prepare_scene();
	trace(&ray, 0, &lit);

	/* add a legacy occluder sphere between the surface and the light */
	TASK_CHECK(load_text(
		"M \"white\" 1 1 1 1 1 1 0 0 0 0\n"
		"M \"dark\" 0.1 0.1 0.1 0 0 0 0 0 0 0\n"
		"p \"P\" 0 0 0 1 \"white\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n"
		"s -2.5 0 0 1 \"dark\"\n"
		"l -10 0 0 1 1 1\n") == 0);
	prepare_scene();
	trace(&ray, 0, &shadowed);

	/* the point-light shadow ray uses the same occlusion production path */
	TASK_CHECK(proj_intersect(&ray, &sp, 0) == 1);
	shadow_ray.orig = sp.pos;
	shadow_ray.dir.x = -10.0 - sp.pos.x;
	shadow_ray.dir.y = 0;
	shadow_ray.dir.z = 0;
	TASK_CHECK(ray_scene(&shadow_ray, 0) == 1); /* blocked */

	TASK_CHECK(shadowed.r < lit.r - 0.5f);
	TASK_CHECK(shadowed.g < lit.g - 0.5f);
	return 0;
}

static int case_render_reflection(void)
{
	struct ray ray = {{-5, 0, 0}, {10, 0, 0}};
	struct color plain, reflected;

	setup_lighting();

	TASK_CHECK(load_text(
		"M \"mirror\" 0.2 0.2 0.2 1 1 1 50 1.0 0 0\n"
		"p \"P\" 0 0 0 1 \"mirror\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n"
		"l 0 10 0 1 1 1\n") == 0);
	prepare_scene();
	trace(&ray, 0, &plain);

	/* a red legacy sphere sits in the reflection direction (-x); the light
	 * above lights the face the mirror sees without the mirror shadowing it.
	 */
	TASK_CHECK(load_text(
		"M \"mirror\" 0.2 0.2 0.2 1 1 1 50 1.0 0 0\n"
		"M \"red\" 1 0 0 0 0 0 0 0 0 0\n"
		"p \"P\" 0 0 0 1 \"mirror\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n"
		"s -5 0 0 1 \"red\"\n"
		"l 0 10 0 1 1 1\n") == 0);
	prepare_scene();
	trace(&ray, 0, &reflected);

	/* the reflection now picks up the red sphere: red channel rises clearly
	 * above the plain mirror's grey ambient, while green/blue stay at zero.
	 */
	TASK_CHECK(reflected.r > plain.r);
	TASK_CHECK(reflected.r > 0.015f);
	TASK_CHECK(reflected.g < 1e-5f);
	TASK_CHECK(reflected.b < 1e-5f);
	return 0;
}

static int case_render_legacy_mix(void)
{
	/* a nearer legacy sphere must win over the instance, and vice versa */
	struct ray ray = {{-5, 0, 0}, {10, 0, 0}};
	struct spoint sp;

	setup_lighting();
	TASK_CHECK(load_text(
		"M \"m\" 1 1 1 1 1 1 0 0 0 0\n"
		"M \"legacy\" 0 1 0 0 0 0 0 0 0 0\n"
		"p \"P\" 0 0 0 1 \"m\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n"
		"s -2 0 0 0.5 \"legacy\"\n"
		"l -10 0 0 1 1 1\n") == 0);
	prepare_scene();

	TASK_CHECK(ray_scene(&ray, &sp) == 1);
	TASK_CHECK(sp.obj != 0);		/* legacy sphere is nearer */
	TASK_CHECK(sp.inst == 0);
	TASK_CHECK(fabs(sp.pos.x - (-2.5)) < 1e-7);

	/* move the occluder away; the instance is now the nearest hit */
	TASK_CHECK(load_text(
		"M \"m\" 1 1 1 1 1 1 0 0 0 0\n"
		"M \"legacy\" 0 1 0 0 0 0 0 0 0 0\n"
		"p \"P\" 0 0 0 1 \"m\"\n"
		"i \"I\" \"P\" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1\n"
		"s 8 0 0 0.5 \"legacy\"\n"
		"l -10 0 0 1 1 1\n") == 0);
	prepare_scene();
	TASK_CHECK(ray_scene(&ray, &sp) == 1);
	TASK_CHECK(sp.inst != 0);
	TASK_CHECK(strcmp(proj_hit_instance(&sp), "I") == 0);
	return 0;
}

const struct task_case task_cases[] = {
	/* parameter */
	{"parameter", "illegal_records", case_illegal_records},
	{"parameter", "append_and_identity", case_append_and_identity},
	{"parameter", "matrix_scalar_invariance", case_matrix_scalar_invariance},
	{"parameter", "nonunit_direction", case_nonunit_direction},
	{"parameter", "internal_start", case_internal_start},
	{"parameter", "pole_excluded", case_pole_excluded},
	{"parameter", "shadow_range", case_shadow_range},
	{"parameter", "normal_direction", case_normal_direction},
	/* hierarchy */
	{"hierarchy", "cross_pole_branches", case_cross_pole_branches},
	{"hierarchy", "cross_pole_two_branches", case_cross_pole_two_branches},
	{"hierarchy", "octree_pruning", case_octree_pruning},
	{"hierarchy", "shared_tree_multi_instance", case_shared_tree_multi_instance},
	{"hierarchy", "rebuild_update", case_rebuild_update},
	/* render */
	{"render", "instance_color", case_render_instance_color},
	{"render", "shadow_occlusion", case_render_shadow_occlusion},
	{"render", "reflection", case_render_reflection},
	{"render", "legacy_mix", case_render_legacy_mix},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
