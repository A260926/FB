/*
 * Copyright (c) 2021-2022 Wojciech Graj
 *
 * Licensed under the MIT license: https://opensource.org/licenses/MIT
 * Permission is granted to use, copy, modify, and redistribute the work.
 * Full license information available in the project LICENSE file.
 *
 * DESCRIPTION:
 *   Regression contract for coloured soft shadows.
 *
 *   Verifies the per-object kt multiplication model used by is_light_blocked:
 *   a light segment crossing a transparent object multiplies the per-channel
 *   intensity by that object's kt exactly once (even for a sphere, which has
 *   two intersection points); an opaque object anywhere on the segment makes
 *   blocked=true. The path under test spans render.c (is_light_blocked),
 *   accel.c (BVH over bounded objects) and object.c (unbound plane traversal,
 *   UNBOUND_OBJECTS build).
 *
 *   The checker carries an independent analytic reference: it computes the
 *   earliest positive intersection of spheres, triangles and planes itself,
 *   in double precision, using each object's own epsilon to reject
 *   self-intersections, and never calls the library's get_intersection,
 *   intersects_in_range or any BVH query. Only objects whose intersection
 *   distance is strictly smaller than the light sample distance participate;
 *   the sampled emittant object is excluded. When no opaque object is hit,
 *   the expected per-channel result is the initial intensity times the kt of
 *   every participating object (order is not asserted). When an opaque
 *   object is hit, only blocked=true is asserted.
 *
 *   Also exercises cast_ray end to end against an independently computed
 *   Lambert term, the existing square distance attenuation and the coloured
 *   shadow, and writes a small PPM.
 **/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accel.h"
#include "camera.h"
#include "image.h"
#include "material.h"
#include "object.h"
#include "system.h"

/* entry points defined in render.c (not exported via render.h) */
bool is_light_blocked(const struct Ray *, float, v3, const struct Object *);
float cast_ray(const struct Ray *, const v3, v3, uint32_t, struct Object *);

/* globals referenced by render.c */
struct Image image;
struct Camera camera;

#define KT_TOL 2e-6
#define N_RAND_SCENES 25
#define MAX_SCENE_OBJECTS 16

/*******************************************************************************
*	Independent analytic reference
*******************************************************************************/

enum RefType {
	REF_SPHERE,
	REF_TRIANGLE,
	REF_PLANE,
};

struct RefObj {
	int type;
	float a[3], b[3], c[3]; /* sphere: a=centre; triangle: a,b,c=vertices; plane: a=point, b=normal */
	float r; /* sphere radius */
	float eps;
	float kt[3];
	int opaque;
	int is_light;
	struct Object *lib;
};

static struct RefObj mk_sphere(const float c[3], const float r, const float kt[3], const float eps, const int opaque, const int is_light)
{
	struct RefObj o;
	memset(&o, 0, sizeof(o));
	o.type = REF_SPHERE;
	memcpy(o.a, c, sizeof(v3));
	o.r = r;
	o.eps = eps;
	if (kt)
		memcpy(o.kt, kt, sizeof(v3));
	o.opaque = opaque;
	o.is_light = is_light;
	return o;
}

static struct RefObj mk_triangle(const float va[3], const float vb[3], const float vc[3], const float kt[3], const float eps, const int opaque)
{
	struct RefObj o;
	memset(&o, 0, sizeof(o));
	o.type = REF_TRIANGLE;
	memcpy(o.a, va, sizeof(v3));
	memcpy(o.b, vb, sizeof(v3));
	memcpy(o.c, vc, sizeof(v3));
	o.eps = eps;
	if (kt)
		memcpy(o.kt, kt, sizeof(v3));
	o.opaque = opaque;
	return o;
}

static struct RefObj mk_plane(const float p[3], const float nrm[3], const float kt[3], const float eps, const int opaque)
{
	struct RefObj o;
	memset(&o, 0, sizeof(o));
	o.type = REF_PLANE;
	memcpy(o.a, p, sizeof(v3));
	float m = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
	for (int i = 0; i < 3; i++)
		o.b[i] = nrm[i] / m;
	o.eps = eps;
	if (kt)
		memcpy(o.kt, kt, sizeof(v3));
	o.opaque = opaque;
	return o;
}

static double dot3d(const double a[3], const double b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Earliest intersection distance past the object's own epsilon, or -1.
 * Computed independently of the library's intersection routines. */
static double ref_intersect(const struct RefObj *o, const double ro[3], const double rd[3])
{
	switch (o->type) {
	case REF_SPHERE: {
		double rel[3] = { ro[0] - o->a[0], ro[1] - o->a[1], ro[2] - o->a[2] };
		double b = -dot3d(rd, rel);
		double cq = dot3d(rel, rel) - (double)o->r * o->r;
		double det = b * b - cq;
		if (det < 0.)
			return -1.;
		double s = sqrt(det);
		double t = b - s;
		if (t > (double)o->eps)
			return t;
		t = b + s;
		return t > (double)o->eps ? t : -1.;
	}
	case REF_TRIANGLE: {
		double e1[3] = { o->b[0] - o->a[0], o->b[1] - o->a[1], o->b[2] - o->a[2] };
		double e2[3] = { o->c[0] - o->a[0], o->c[1] - o->a[1], o->c[2] - o->a[2] };
		double n[3] = {
			e1[1] * e2[2] - e1[2] * e2[1],
			e1[2] * e2[0] - e1[0] * e2[2],
			e1[0] * e2[1] - e1[1] * e2[0],
		};
		double den = dot3d(n, rd);
		if (fabs(den) < 1e-11)
			return -1.;
		double pa[3] = { o->a[0], o->a[1], o->a[2] };
		double t = (dot3d(n, pa) - dot3d(n, ro)) / den;
		if (t <= (double)o->eps)
			return -1.;
		double p[3] = { ro[0] + t * rd[0], ro[1] + t * rd[1], ro[2] + t * rd[2] };
		double v2[3] = { p[0] - o->a[0], p[1] - o->a[1], p[2] - o->a[2] };
		double d00 = dot3d(e1, e1), d01 = dot3d(e1, e2), d11 = dot3d(e2, e2);
		double d20 = dot3d(v2, e1), d21 = dot3d(v2, e2);
		double denom = d00 * d11 - d01 * d01;
		if (denom == 0.)
			return -1.;
		double v = (d11 * d20 - d01 * d21) / denom;
		double w = (d00 * d21 - d01 * d20) / denom;
		if (v < -1e-9 || w < -1e-9 || v + w > 1. + 1e-9)
			return -1.;
		return t;
	}
	case REF_PLANE: {
		double n[3] = { o->b[0], o->b[1], o->b[2] };
		double den = dot3d(n, rd);
		if (fabs(den) < (double)o->eps)
			return -1.;
		double pa[3] = { o->a[0], o->a[1], o->a[2] };
		double t = (dot3d(n, pa) - dot3d(n, ro)) / den;
		return t > (double)o->eps ? t : -1.;
	}
	}
	return -1.;
}

/* Reference shadow contract: initial intensity (1,1,1) times the kt of every
 * non-emittant object whose intersection distance is strictly below the light
 * sample distance. blocked only requires an opaque participant. */
static void ref_shadow(const struct RefObj *refs, const int n, const float ro_f[3], const float rd_f[3],
	const float L_f, const int light_idx, double out_i[3], int *out_blocked)
{
	double ro[3] = { ro_f[0], ro_f[1], ro_f[2] };
	double rd[3] = { rd_f[0], rd_f[1], rd_f[2] };
	double L = L_f;
	out_i[0] = out_i[1] = out_i[2] = 1.;
	*out_blocked = 0;
	for (int i = 0; i < n; i++) {
		if (i == light_idx)
			continue;
		double t = ref_intersect(&refs[i], ro, rd);
		if (t < 0. || t >= L)
			continue;
		if (refs[i].opaque) {
			*out_blocked = 1;
		} else {
			for (int c = 0; c < 3; c++)
				out_i[c] *= (double)refs[i].kt[c];
		}
	}
}

/*******************************************************************************
*	Library scene handling
*******************************************************************************/

static void build_lib_objects(struct RefObj *refs, struct Material *mats, const int n)
{
	v3 zero = { 0.f, 0.f, 0.f }, one = { 1.f, 1.f, 1.f };
	for (int i = 0; i < n; i++) {
		struct RefObj *r = &refs[i];
		v3 kt = { 0.f, 0.f, 0.f }, ke = { 0.f, 0.f, 0.f };
		if (!r->opaque && !r->is_light)
			memcpy(kt, r->kt, sizeof(v3));
		if (r->is_light)
			memcpy(ke, one, sizeof(v3));
		material_init(&mats[i], i, zero, zero, zero, kt, ke, 8.f, 1.f, texture_uniform_new(one));
		enum ObjectType t;
		switch (r->type) {
		case REF_SPHERE:
			r->lib = sphere_new(r->a, r->r);
			t = OBJECT_SPHERE;
			break;
		case REF_TRIANGLE: {
			v3 vs[3];
			memcpy(vs[0], r->a, sizeof(v3));
			memcpy(vs[1], r->b, sizeof(v3));
			memcpy(vs[2], r->c, sizeof(v3));
			r->lib = triangle_new(vs);
			t = OBJECT_TRIANGLE;
		} break;
		case REF_PLANE:
			r->lib = plane_new(r->a, r->b);
			t = OBJECT_PLANE;
			break;
		default:
			exit(1);
		}
		object_init(r->lib, &mats[i], r->eps, 1, t);
		r->lib->object_data->postinit(r->lib);
	}
}

static void scene_install(struct RefObj *refs, const int n, const int light_idx, const int *unbound_idx, const int n_unbound)
{
	objects = malloc(sizeof(struct Object *) * n);
	for (int i = 0; i < n; i++)
		objects[i] = refs[i].lib;
	num_objects = n;
	unbound_objects = malloc(sizeof(struct Object *) * n_unbound);
	for (int i = 0; i < n_unbound; i++)
		unbound_objects[i] = refs[unbound_idx[i]].lib;
	num_unbound_objects = n_unbound;
	emittant_objects = malloc(sizeof(struct Object *));
	emittant_objects[0] = refs[light_idx].lib;
	num_emittant_objects = 1;
	accel_init();
}

static void scene_teardown(struct RefObj *refs, struct Material *mats, const int n)
{
	accel_deinit();
	for (int i = 0; i < n; i++) {
		refs[i].lib->object_data->delete(refs[i].lib);
		free(mats[i].texture);
		refs[i].lib = NULL;
	}
	free(objects);
	free(unbound_objects);
	free(emittant_objects);
	objects = NULL;
	unbound_objects = NULL;
	emittant_objects = NULL;
	num_objects = num_unbound_objects = num_emittant_objects = 0;
}

/*******************************************************************************
*	Case evaluation
*******************************************************************************/

static void fail_blocked(const char *name, const int exp, const int act)
{
	printf("SHADOW_CONTRACT_FAIL case=%s expected_blocked=%d actual_blocked=%d\n", name, exp, act);
	exit(1);
}

static void fail_channel(const char *name, const int ch, const double exp, const double act)
{
	printf("SHADOW_CONTRACT_FAIL case=%s channel=%d expected=%.9g actual=%.9g\n", name, ch, exp, act);
	exit(1);
}

/* Runs one light segment against the currently installed scene and compares
 * is_light_blocked with the analytic reference. */
static void run_shadow_case(const char *name, const struct RefObj *refs, const int n, const int light_idx,
	const float ro[3], const float rd[3], const float L)
{
	double exp_i[3];
	int exp_blocked;
	ref_shadow(refs, n, ro, rd, L, light_idx, exp_i, &exp_blocked);

	struct Ray ray;
	v3 intensity = { 1.f, 1.f, 1.f };
	memcpy(ray.direction, rd, sizeof(v3));
	memcpy(ray.point, ro, sizeof(v3));
	bool blocked = is_light_blocked(&ray, L, intensity, refs[light_idx].lib);

	if ((int)blocked != exp_blocked)
		fail_blocked(name, exp_blocked, (int)blocked);
	if (!exp_blocked)
		for (int c = 0; c < 3; c++)
			if (fabs((double)intensity[c] - exp_i[c]) > KT_TOL)
				fail_channel(name, c, exp_i[c], (double)intensity[c]);
	printf("SHADOW_CONTRACT_PASS case=%s blocked=%d intensity=%.6f %.6f %.6f\n",
		name, exp_blocked, (double)intensity[0], (double)intensity[1], (double)intensity[2]);
}

/*******************************************************************************
*	Fixed scene: explicit geometry for the boundary cases
*******************************************************************************/

static void check_fixed_scene(void)
{
	struct RefObj refs[8];
	struct Material mats[8];
	/* two transparent spheres of different colours */
	refs[0] = mk_sphere((v3){ 0.f, 0.f, 4.f }, 1.f, (v3){ .5f, .25f, 1.f }, 1e-3f, 0, 0); /* S1 */
	refs[1] = mk_sphere((v3){ 0.f, 0.f, 8.f }, 1.f, (v3){ .4f, .8f, .5f }, 1e-4f, 0, 0); /* S2 */
	/* transparent sphere used for the light-distance bound near its bounding cuboid */
	refs[2] = mk_sphere((v3){ 0.f, 2.5f, 10.f }, 1.f, (v3){ .7f, .6f, .9f }, 1e-4f, 0, 0); /* S3 */
	/* ordinary opaque blocker */
	refs[3] = mk_sphere((v3){ 3.f, 0.f, 16.f }, 1.f, NULL, 1e-4f, 1, 0); /* B1 */
	/* transparent triangle */
	refs[4] = mk_triangle((v3){ 10.f, 10.f, 2.f }, (v3){ 12.f, 10.f, 2.f }, (v3){ 11.f, 12.f, 2.f },
		(v3){ .9f, .7f, .3f }, 1e-4f, 0); /* T1 */
	/* opaque triangle */
	refs[5] = mk_triangle((v3){ 10.f, 20.f, 2.f }, (v3){ 12.f, 20.f, 2.f }, (v3){ 11.f, 22.f, 2.f },
		NULL, 1e-4f, 1); /* T2 */
	/* transparent unbound plane (UNBOUND_OBJECTS path) */
	refs[6] = mk_plane((v3){ 0.f, 0.f, 12.f }, (v3){ 0.f, 0.f, 1.f }, (v3){ .8f, .9f, .6f }, 1e-6f, 0); /* P1 */
	/* emittant lamp, bounded so it sits inside the BVH */
	refs[7] = mk_sphere((v3){ 0.f, 0.f, 20.f }, .5f, NULL, 1e-4f, 0, 1); /* LAMP */

	const int light_idx = 7;
	const int unbound_idx[1] = { 6 };
	build_lib_objects(refs, mats, 8);
	scene_install(refs, 8, light_idx, unbound_idx, 1);

	/* reference self-check against the known product kt_S1 * kt_S2 * kt_P1 */
	{
		double si[3];
		int bl;
		ref_shadow(refs, 8, (v3){ 0.f, 0.f, 0.f }, (v3){ 0.f, 0.f, 1.f }, 19.f, light_idx, si, &bl);
		if (bl || fabs(si[0] - .16) > 1e-7 || fabs(si[1] - .18) > 1e-7 || fabs(si[2] - .30) > 1e-7) {
			fprintf(stderr, "SHADOW_CONTRACT_FAIL case=A0 reference sanity\n");
			exit(1);
		}
	}

	static const struct {
		const char *name;
		float ro[3], rd[3], L;
	} cases[] = {
		/* one ray crossing both transparent spheres and the transparent plane */
		{ "A1", { 0.f, 0.f, 0.f }, { 0.f, 0.f, 1.f }, 19.f },
		/* opaque blocker between origin and light */
		{ "A2", { 3.f, 0.f, 0.f }, { 0.f, 0.f, 1.f }, 20.f },
		/* same blocker behind the light sample distance: must not block */
		{ "A3", { 3.f, 0.f, 0.f }, { 0.f, 0.f, 1.f }, 10.f },
		/* S1 nearest intersection exactly at the light distance: excluded (strict <) */
		{ "A4", { 0.f, 0.f, 0.f }, { 0.f, 0.f, 1.f }, 3.f },
		/* origin inside S1; segment passes through the emittant lamp itself */
		{ "A5", { 0.f, 0.f, 4.f }, { 0.f, 0.f, 1.f }, 16.f },
		/* triangle hit inside its edges */
		{ "A6", { 11.f, 11.f, 0.f }, { 0.f, 0.f, 1.f }, 10.f },
		/* triangle miss outside its edges */
		{ "A7", { 11.f, 9.5f, 0.f }, { 0.f, 0.f, 1.f }, 10.f },
		/* opaque triangle blocks */
		{ "A8", { 11.f, 21.f, 0.f }, { 0.f, 0.f, 1.f }, 10.f },
		/* origin within epsilon of S1's surface: self-intersection excluded */
		{ "A9", { 0.f, 0.f, 3.0005f }, { 0.f, 0.f, -1.f }, 10.f },
		/* axis-parallel ray along x; plane parallel to the ray */
		{ "A10", { -3.f, 0.f, 4.f }, { 1.f, 0.f, 0.f }, 20.f },
		/* S3 beyond the light distance, but its bounding cuboid starts before it */
		{ "A11", { 0.f, 2.f, 0.f }, { 0.f, 0.f, 1.f }, 9.05f },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		run_shadow_case(cases[i].name, refs, 8, light_idx, cases[i].ro, cases[i].rd, cases[i].L);

	scene_teardown(refs, mats, 8);
}

/*******************************************************************************
*	Random scenes: fixed seed, margins away from tangencies and thresholds,
*	object order permuted and BVH rebuilt between runs
*******************************************************************************/

static uint32_t rng_state = 0xC0FFEE11u;

static uint32_t xr(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

static float frand(const float lo, const float hi)
{
	return lo + (hi - lo) * (float)(xr() >> 8) * (1.f / 16777216.f);
}

static void rand_unit(float d[3])
{
	for (;;) {
		d[0] = frand(-1.f, 1.f);
		d[1] = frand(-1.f, 1.f);
		d[2] = frand(-1.f, 1.f);
		float m2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		if (m2 > 0.04f) {
			float inv = 1.f / sqrtf(m2);
			d[0] *= inv;
			d[1] *= inv;
			d[2] *= inv;
			return;
		}
	}
}

static void rand_perp(const float d[3], float out[3])
{
	for (;;) {
		float v[3];
		rand_unit(v);
		float proj = v[0] * d[0] + v[1] * d[1] + v[2] * d[2];
		for (int i = 0; i < 3; i++)
			out[i] = v[i] - proj * d[i];
		float m2 = out[0] * out[0] + out[1] * out[1] + out[2] * out[2];
		if (m2 > 0.04f) {
			float inv = 1.f / sqrtf(m2);
			out[0] *= inv;
			out[1] *= inv;
			out[2] *= inv;
			return;
		}
	}
}

/* Rejects objects too close to tangencies, epsilons or the light-distance
 * threshold, so the reference and the library cannot disagree on float
 * rounding. */
static int validate_margins(const struct RefObj *o, const float ro_f[3], const float rd_f[3], const float L)
{
	double ro[3] = { ro_f[0], ro_f[1], ro_f[2] };
	double rd[3] = { rd_f[0], rd_f[1], rd_f[2] };
	double t = ref_intersect(o, ro, rd);
	if (t >= 0.) {
		if (t <= (double)o->eps + 2e-3)
			return 0;
		if (fabs(t - (double)L) <= 2e-3)
			return 0;
	}
	switch (o->type) {
	case REF_SPHERE: {
		double w[3] = { o->a[0] - ro[0], o->a[1] - ro[1], o->a[2] - ro[2] };
		double proj = dot3d(w, rd);
		double perp2 = dot3d(w, w) - proj * proj;
		double perp = perp2 > 0. ? sqrt(perp2) : 0.;
		if (fabs(perp - (double)o->r) < 1e-2)
			return 0;
	} break;
	case REF_TRIANGLE: {
		double e1[3] = { o->b[0] - o->a[0], o->b[1] - o->a[1], o->b[2] - o->a[2] };
		double e2[3] = { o->c[0] - o->a[0], o->c[1] - o->a[1], o->c[2] - o->a[2] };
		double n[3] = {
			e1[1] * e2[2] - e1[2] * e2[1],
			e1[2] * e2[0] - e1[0] * e2[2],
			e1[0] * e2[1] - e1[1] * e2[0],
		};
		double nm = sqrt(dot3d(n, n));
		if (nm < 1e-3)
			return 0;
		double den = dot3d(n, rd);
		if (fabs(den) < 1e-2 * nm)
			return 0;
		double pa[3] = { o->a[0], o->a[1], o->a[2] };
		double tp = (dot3d(n, pa) - dot3d(n, ro)) / den;
		if (tp > 0.) {
			double p[3] = { ro[0] + tp * rd[0], ro[1] + tp * rd[1], ro[2] + tp * rd[2] };
			double v2[3] = { p[0] - o->a[0], p[1] - o->a[1], p[2] - o->a[2] };
			double d00 = dot3d(e1, e1), d01 = dot3d(e1, e2), d11 = dot3d(e2, e2);
			double d20 = dot3d(v2, e1), d21 = dot3d(v2, e2);
			double denom = d00 * d11 - d01 * d01;
			double v = (d11 * d20 - d01 * d21) / denom;
			double w = (d00 * d21 - d01 * d20) / denom;
			int inside = v > 1e-3 && w > 1e-3 && v + w < 1. - 1e-3;
			int outside = v < -1e-3 || w < -1e-3 || v + w > 1. + 1e-3;
			if (!inside && !outside)
				return 0;
		}
	} break;
	case REF_PLANE: {
		double n[3] = { o->b[0], o->b[1], o->b[2] };
		if (fabs(dot3d(n, rd)) < 1e-2)
			return 0;
	} break;
	}
	return 1;
}

static void shuffle_objects(struct Object **arr, const size_t n)
{
	for (size_t i = n - 1; i > 0; i--) {
		size_t j = xr() % (i + 1);
		struct Object *tmp = arr[i];
		arr[i] = arr[j];
		arr[j] = tmp;
	}
}

static void check_random_scenes(void)
{
	for (int s = 0; s < N_RAND_SCENES; s++) {
		struct RefObj refs[MAX_SCENE_OBJECTS];
		struct Material mats[MAX_SCENE_OBJECTS];
		int n = 0;
		float ro[3] = { frand(-5.f, 5.f), frand(-5.f, 5.f), frand(-5.f, 5.f) };
		float rd[3];
		rand_unit(rd);
		float L = frand(4.f, 10.f);

		/* emittant lamp sitting on the shadow ray just inside the light distance */
		int lamp_idx = n;
		{
			float c[3];
			for (int i = 0; i < 3; i++)
				c[i] = ro[i] + 0.95f * L * rd[i];
			refs[n++] = mk_sphere(c, 0.4f, NULL, 1e-4f, 0, 1);
		}
		/* two transparent spheres straddling the ray */
		for (int k = 0; k < 2; k++) {
			float t = (k ? 0.6f : 0.3f) * L;
			float perp[3];
			rand_perp(rd, perp);
			float off = frand(0.f, 0.3f);
			float c[3], kt[3];
			for (int i = 0; i < 3; i++) {
				c[i] = ro[i] + t * rd[i] + off * perp[i];
				kt[i] = frand(0.3f, 0.9f);
			}
			refs[n++] = mk_sphere(c, frand(0.4f, 0.6f), kt, 1e-4f, 0, 0);
		}
		/* opaque straddler every third scene, so blocked=true is exercised */
		if (s % 3 == 0) {
			float perp[3];
			rand_perp(rd, perp);
			float off = frand(0.f, 0.2f);
			float c[3];
			for (int i = 0; i < 3; i++)
				c[i] = ro[i] + 0.5f * L * rd[i] + off * perp[i];
			refs[n++] = mk_sphere(c, frand(0.4f, 0.6f), NULL, 1e-4f, 1, 0);
		}
		/* extra bounded objects at random */
		int n_extra = 2 + (int)(xr() % 3u);
		for (int e = 0; e < n_extra; e++) {
			for (int attempt = 0; attempt < 100; attempt++) {
				struct RefObj o;
				if (xr() & 1u) {
					float c[3] = { frand(-6.f, 6.f), frand(-6.f, 6.f), frand(-6.f, 6.f) };
					int opaque = xr() % 3u == 0u;
					float kt[3] = { frand(0.3f, 0.9f), frand(0.3f, 0.9f), frand(0.3f, 0.9f) };
					o = mk_sphere(c, frand(0.3f, 1.f), opaque ? NULL : kt, 1e-4f, opaque, 0);
				} else {
					float va[3] = { frand(-6.f, 6.f), frand(-6.f, 6.f), frand(-6.f, 6.f) };
					float e1[3], e2[3], vb[3], vc[3];
					rand_unit(e1);
					rand_unit(e2);
					float l1 = frand(0.5f, 1.5f), l2 = frand(0.5f, 1.5f);
					for (int i = 0; i < 3; i++) {
						vb[i] = va[i] + l1 * e1[i];
						vc[i] = va[i] + l2 * e2[i];
					}
					int opaque = xr() % 3u == 0u;
					float kt[3] = { frand(0.3f, 0.9f), frand(0.3f, 0.9f), frand(0.3f, 0.9f) };
					o = mk_triangle(va, vb, vc, opaque ? NULL : kt, 1e-4f, opaque);
				}
				if (validate_margins(&o, ro, rd, L)) {
					refs[n++] = o;
					break;
				}
			}
		}
		/* one unbound transparent plane (UNBOUND_OBJECTS path) */
		int plane_idx = -1;
		for (int attempt = 0; attempt < 1000 && plane_idx < 0; attempt++) {
			float nrm[3], p[3], kt[3];
			rand_unit(nrm);
			for (int i = 0; i < 3; i++) {
				p[i] = frand(-6.f, 6.f);
				kt[i] = frand(0.3f, 0.9f);
			}
			struct RefObj o = mk_plane(p, nrm, kt, 1e-6f, 0);
			if (validate_margins(&o, ro, rd, L)) {
				plane_idx = n;
				refs[n++] = o;
			}
		}
		if (plane_idx < 0) {
			/* deterministic fallback: plane perpendicular to the ray */
			float p[3];
			for (int i = 0; i < 3; i++)
				p[i] = ro[i] + 0.8f * L * rd[i];
			plane_idx = n;
			refs[n++] = mk_plane(p, rd, (v3){ .7f, .8f, .9f }, 1e-6f, 0);
		}

		build_lib_objects(refs, mats, n);
		scene_install(refs, n, lamp_idx, &plane_idx, 1);
		for (int perm = 0; perm < 3; perm++) {
			if (perm) {
				/* same objects, different input order, rebuilt BVH */
				shuffle_objects(objects, num_objects);
				accel_deinit();
				accel_init();
			}
			char name[32];
			snprintf(name, sizeof(name), "R%02dP%d", s, perm);
			run_shadow_case(name, refs, n, lamp_idx, ro, rd, L);
		}
		scene_teardown(refs, mats, n);
	}
}

/*******************************************************************************
*	cast_ray end-to-end: white diffuse receiver, pinned light sample,
*	independent Lambert + distance attenuation + coloured shadow
*******************************************************************************/

static void check_cast_ray(const char *ppm_path)
{
	struct RefObj refs[4];
	struct Material mats[4];
	int n = 0;
	/* white diffuse receiving surface (unbound plane) */
	refs[n++] = mk_plane((v3){ 0.f, 0.f, 0.f }, (v3){ 0.f, 0.f, 1.f }, NULL, 1e-6f, 1);
	/* transparent sphere on the light path (bounded, BVH) */
	refs[n++] = mk_sphere((v3){ 1.5f, 0.f, 2.5f }, .5f, (v3){ .5f, .25f, 1.f }, 1e-4f, 0, 0);
	/* transparent plane on the light path (unbound) */
	refs[n++] = mk_plane((v3){ 0.f, 0.f, 3.5f }, (v3){ 0.f, 0.f, 1.f }, (v3){ .8f, .9f, .6f }, 1e-6f, 0);
	/* lamp */
	refs[n++] = mk_sphere((v3){ 3.f, 0.f, 5.f }, .25f, NULL, 1e-4f, 0, 1);
	const int lamp_idx = 3;

	build_lib_objects(refs, mats, n);
	const int unbound_idx[2] = { 0, 2 };
	scene_install(refs, n, lamp_idx, unbound_idx, 2);

	/* Pin the light sample: cast_ray draws exactly two rand_flt() values
	 * (sphere_get_light_point) before doing anything else random, so the same
	 * seed reproduces the sample position on both sides. */
	const unsigned seed = 20261111u;
	srand(seed);
	float r1 = rand_flt();
	float r2 = rand_flt();
	float inclination = r1 * 2.f * PI;
	float azimuth = r2 * 2.f * PI;
	float ld[3] = {
		.25f * cosf(azimuth) * sinf(inclination),
		.25f * sinf(azimuth) * sinf(inclination),
		.25f * cosf(inclination),
	};
	/* shaded point P = (0,0,0); lamp centre (3,0,5) */
	float nrm[3] = { 3.f, 0.f, 5.f };
	if (nrm[0] * ld[0] + nrm[1] * ld[1] + nrm[2] * ld[2]) {
		ld[0] = -ld[0];
		ld[1] = -ld[1];
		ld[2] = -ld[2];
	}
	float lp[3] = { 3.f + ld[0], ld[1], 5.f + ld[2] };

	float dv[3] = { lp[0], lp[1], lp[2] };
	float ms = dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2];
	float dist = sqrtf(ms);
	float invd = 1.f / dist;
	float dir[3] = { dv[0] * invd, dv[1] * invd, dv[2] * invd };
	float a = dir[2]; /* dot(direction, receiver normal (0,0,1)) */

	/* reference coloured shadow along the light segment */
	double si[3];
	int sblocked;
	ref_shadow(refs, n, (v3){ 0.f, 0.f, 0.f }, dir, dist, lamp_idx, si, &sblocked);
	if (sblocked) {
		fprintf(stderr, "SHADOW_CONTRACT_FAIL case=C1 reference unexpectedly blocked\n");
		exit(1);
	}

	/* expected linear RGB: Lambert term, existing square distance
	 * attenuation (offset 1) for light and view, coloured shadow */
	float att = 1.f / (1.f + ms);
	float am = fmaxf(0.f, a);
	float vatt = 1.f / ((1.f + 2.f) * (1.f + 2.f)); /* view distance is 2 */
	double expected[3];
	for (int c = 0; c < 3; c++)
		expected[c] = si[c] * (double)att * (double)am * (double)vatt;

	srand(seed);
	v3 color = { 0.f, 0.f, 0.f };
	v3 kr = { 1.f, 1.f, 1.f };
	struct Ray view = { .direction = { 0.f, 0.f, -1.f }, .point = { 0.f, 0.f, 2.f } };
	cast_ray(&view, kr, color, 0, NULL);
	for (int c = 0; c < 3; c++)
		if (fabs((double)color[c] - expected[c]) > KT_TOL)
			fail_channel("C1", c, expected[c], (double)color[c]);
	printf("SHADOW_CONTRACT_PASS case=C1 rgb=%.9f %.9f %.9f\n",
		(double)color[0], (double)color[1], (double)color[2]);

	/* small PPM of the same scene (orthographic rays onto the receiver) */
	enum { W = 64, H = 48 };
	float *pix = malloc(sizeof(float) * 3 * W * H);
	float maxc = 1e-12f;
	srand(seed + 1u);
	for (int j = 0; j < H; j++) {
		for (int i = 0; i < W; i++) {
			float ox = -2.f + 7.f * ((float)i + .5f) / W;
			float oy = -2.f + 4.f * ((float)j + .5f) / H;
			struct Ray ray = { .direction = { 0.f, 0.f, -1.f }, .point = { ox, oy, 2.f } };
			v3 col = { 0.f, 0.f, 0.f };
			cast_ray(&ray, kr, col, 0, NULL);
			for (int c = 0; c < 3; c++) {
				pix[3 * (j * W + i) + c] = col[c];
				if (col[c] > maxc)
					maxc = col[c];
			}
		}
	}
	FILE *f = fopen(ppm_path, "wb");
	if (!f) {
		perror("SHADOW_CONTRACT_FAIL fopen");
		exit(1);
	}
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	float scale = 254.9f / maxc;
	for (int i = 0; i < 3 * W * H; i++) {
		float v = pix[i] * scale;
		if (v > 255.f)
			v = 255.f;
		fputc((int)v, f);
	}
	fclose(f);
	free(pix);
	printf("SHADOW_CONTRACT_PPM path=%s\n", ppm_path);

	scene_teardown(refs, mats, n);
}

int main(const int argc, char **argv)
{
	const char *ppm_path = argc > 1 ? argv[1] : "shadow_contract.ppm";
	check_fixed_scene();
	check_random_scenes();
	check_cast_ray(ppm_path);
	puts("SHADOW_CONTRACT_OK");
	return 0;
}
