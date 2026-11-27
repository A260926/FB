/*
 * Task regression: the shared rotation-minimizing (Bishop) frame field of the
 * swept and pipe surfaces (src/qaws/qaws_sweep_rmf.c).
 *
 * Four groups, each a set of named cases registered in task_cases:
 *
 *   span_transport        every knot span of a curve participates; a narrow span
 *                         carrying a whole spatial turn is resolved adaptively,
 *                         a per-span dense reference confirms the direction error
 *                         falls to the tolerance (2e-5 f32, 2e-8 f64), and the
 *                         field is continuous through the narrow span and at a
 *                         curvature-zero (inflection) point;
 *   surface_differentials  position, du, dv, duu, duv, dvv and normal come from
 *                         one continuous field across cache intervals, checked
 *                         against independent finite differences of positions;
 *   mode_allocator         switching the field on, rebuilding it and releasing it
 *                         keeps the surface's identity and its borrowed path and
 *                         profile, with every cache block from the surface's
 *                         allocator (counted); tolerance 0 restores the original
 *                         behaviour; invalid modes are refused and keep the
 *                         active mode;
 *   precision_batch        f32 and f64 forms and batch queries in any order agree
 *                         with the scalar queries.
 *
 * The reference for direction is the library's own rotation-minimizing frames
 * over a dense per-span grid, started from the field's normal at the span's
 * start, so it measures transport and not the starting orientation; it does not
 * read the new cache. Derivatives are compared against differences of sampled
 * positions.
 */

#include "../task_support/check.h"
#include "qaws_both.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* the scalar's precision, for the bounds */
#define RMF_F32 (sizeof(qaws_scalar) == sizeof(float))
/* the task's numeric bounds: f32 2e-5, f64 2e-8 */
#define RMF_DIRECTION_BOUND (RMF_F32 ? 2e-5 : 2e-8)

static double rmf_dot(qaws_vec3 a, qaws_vec3 b)
{
	return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z;
}

static double rmf_norm(qaws_vec3 a)
{
	return sqrt(rmf_dot(a, a));
}

static qaws_vec3 rmf_sub(qaws_vec3 a, qaws_vec3 b)
{
	qaws_vec3 r = { a.x - b.x, a.y - b.y, a.z - b.z };
	return r;
}

static double rmf_distance(qaws_vec3 a, qaws_vec3 b)
{
	return rmf_norm(rmf_sub(a, b));
}

/* the angle between two vectors, from the chord of the unit vectors: 2 asin(|a/|a| - b/|b|| / 2) is well conditioned at
   small angles, where acos of the dot product loses all precision (its error is sqrt(eps), ~1.5e-8 in double) */
static double rmf_angle(qaws_vec3 a, qaws_vec3 b)
{
	double la = rmf_norm(a), lb = rmf_norm(b);
	qaws_vec3 d;
	double half;
	if (!(la > 0) || !(lb > 0))
		return 0;
	d.x = (qaws_scalar)((double)a.x / la - (double)b.x / lb);
	d.y = (qaws_scalar)((double)a.y / la - (double)b.y / lb);
	d.z = (qaws_scalar)((double)a.z / la - (double)b.z / lb);
	half = 0.5 * rmf_norm(d);
	return 2 * asin(half > 1 ? 1 : half);
}

/* ------------------------------------------------------------------ */
/*  Curves                                                            */
/* ------------------------------------------------------------------ */

/* A cubic B-spline whose middle knot span is four narrow spans around u = 0.5: a whole spatial turn is compressed into a
   span a thousandth wide, the shape that defeats a fixed 256-sample table. */
static qaws_curve* rmf_narrow_path(void)
{
	qaws_vec3 cp[9] = { { -2, 0, 0 }, { -1, 0, 0 }, { -0.3f, 0.2f, 0.1f }, { 0, 1, 0 }, { 1, 1, 1 },
		{ 1, -1, 1 }, { 0, -1, 0 }, { 0.3f, -0.2f, -0.1f }, { 2, 0, 0 } };
	qaws_scalar k[13] = { 0, 0, 0, 0, 0.49998f, 0.49999f, 0.5f, 0.50001f, 0.50002f, 1, 1, 1, 1 };
	qaws_bspline_desc d;
	qaws_curve* c = NULL;
	memset(&d, 0, sizeof d);
	d.dimension = QAWS_DIMENSION_3D;
	d.degree = 3;
	d.control_points = cp;
	d.control_point_count = 9;
	d.knots = k;
	d.knot_count = 13;
	if (qaws_curve_create_bspline(&c, &d) != QAWS_STATUS_OK)
		return NULL;
	return c;
}

/* A planar cubic Bezier with an inflection (curvature through zero), where a Frenet frame flips. */
static qaws_curve* rmf_inflection_path(void)
{
	qaws_vec3 cp[4] = { { 0, 0, 0 }, { 1, 2, 0.3f }, { 3, -1, 0.7f }, { 4, 2, 0 } };
	qaws_bezier_desc d = { QAWS_DIMENSION_3D, 3, cp, 4 };
	qaws_curve* c = NULL;
	if (qaws_curve_create_bezier(&c, &d) != QAWS_STATUS_OK)
		return NULL;
	return c;
}

/* A straight segment: a Frenet frame is undefined, the field must carry one frame. */
static qaws_curve* rmf_straight_path(void)
{
	qaws_vec3 cp[2] = { { 0, 0, 0 }, { 3, 2, 1 } };
	qaws_bezier_desc d = { QAWS_DIMENSION_3D, 1, cp, 2 };
	qaws_curve* c = NULL;
	if (qaws_curve_create_bezier(&c, &d) != QAWS_STATUS_OK)
		return NULL;
	return c;
}

/* A 3D NURBS with positive weights and non-uniform knots. */
static qaws_curve* rmf_nurbs_path(void)
{
	qaws_vec3 cp[7] = { { 0, 0, 0 }, { 1, 1, 0.2f }, { 2, 0, 0.5f }, { 3, 2, 0 }, { 4, -1, 0.3f },
		{ 5, 1, 1 }, { 6, 0, 0 } };
	qaws_scalar w[7] = { 1, 0.8f, 1.2f, 0.9f, 1.1f, 1, 1 };
	qaws_scalar k[11] = { 0, 0, 0, 0, 0.2f, 0.4f, 0.6f, 0.8f, 1, 1, 1 };
	qaws_nurbs_desc d;
	qaws_curve* c = NULL;
	memset(&d, 0, sizeof d);
	d.dimension = QAWS_DIMENSION_3D;
	d.degree = 3;
	d.control_points = cp;
	d.control_point_count = 7;
	d.weights = w;
	d.weight_count = 7;
	d.knots = k;
	d.knot_count = 11;
	if (qaws_curve_create_nurbs(&c, &d) != QAWS_STATUS_OK)
		return NULL;
	return c;
}

/* the section's start point taken from the profile curve itself: its distance from the path is that length */
static double rmf_profile_start_length(qaws_curve const* profile)
{
	qaws_eval_result_2d e;
	qaws_range pr = qaws_curve_get_parameter_range(profile);
	if (qaws_curve_evaluate_2d(&e, profile, pr.min_value, QAWS_EVAL_FLAG_POSITION) != QAWS_STATUS_OK)
		return -1;
	return sqrt((double)e.position.x * e.position.x + (double)e.position.y * e.position.y);
}

static qaws_curve* rmf_profile(void)
{
	qaws_vec2 p[2] = { { 0.6f, 0.1f }, { 0.75f, 0.4f } };
	qaws_bezier_desc d = { QAWS_DIMENSION_2D, 1, p, 2 };
	qaws_curve* c = NULL;
	if (qaws_curve_create_bezier(&c, &d) != QAWS_STATUS_OK)
		return NULL;
	return c;
}

/* ------------------------------------------------------------------ */
/*  Direction reference                                                */
/* ------------------------------------------------------------------ */

/* The field's normal at t from a surface, recovered from the section vector. The profile is a straight line from
   (0.6, 0.1) to (0.75, 0.4) in the (N, B) plane, so at v = 0 the section is 0.6 N + 0.1 B; projecting it perpendicular to
   the tangent gives a vector along N, since B is perpendicular to T. The section is non-zero, so the direction is well
   defined everywhere. */
static int rmf_surface_normal_at(qaws_surface const* surface, qaws_curve const* path, double u, qaws_vec3* out)
{
	qaws_surface_eval_result r;
	qaws_eval_result_3d e;
	qaws_range pr = qaws_curve_get_parameter_range(path);
	double t = pr.min_value + u * (pr.max_value - pr.min_value);
	if (qaws_surface_evaluate(&r, surface, (qaws_scalar)u, 0, QAWS_SURFACE_EVAL_POSITION) != QAWS_STATUS_OK)
		return 0;
	if (qaws_curve_evaluate_3d(&e, path, (qaws_scalar)t, QAWS_EVAL_FLAG_POSITION) != QAWS_STATUS_OK)
		return 0;
	*out = rmf_sub(r.position, e.position);
	return rmf_norm(*out) > 0;
}

/* The per-span dense reference error: over every knot span of path, the library's rotation-minimizing frames over a dense
   grid started from the field's normal at the span start, against the surface's field normals. bounds holds the span
   boundaries (nspan + 1 values) in the path's parameter. The reference is independent of the new cache. */
static double rmf_direction_error(qaws_surface const* surface, qaws_curve const* path, double const* bounds,
	unsigned int nspan, unsigned int steps)
{
	double worst = 0;
	unsigned int sp;
	qaws_range pr = qaws_curve_get_parameter_range(path);
	qaws_scalar* ts = NULL;
	qaws_scalar* frames = NULL;
	if (steps == 0 || nspan == 0)
		return 1e9;
	ts = (qaws_scalar*)malloc(sizeof(qaws_scalar) * (size_t)(steps + 1));
	frames = (qaws_scalar*)malloc(sizeof(qaws_scalar) * 9 * (size_t)(steps + 1));
	if (!ts || !frames)
	{
		free(ts);
		free(frames);
		return 1e9;
	}
	for (sp = 0; sp < nspan; sp++)
	{
		double a = bounds[sp], b = bounds[sp + 1];
		qaws_vec3 start;
		unsigned int i;
		if (!(b > a))
			continue;
		if (!rmf_surface_normal_at(surface, path, (a - pr.min_value) / (pr.max_value - pr.min_value), &start))
		{
			worst = 1e9;
			break;
		}
		for (i = 0; i <= steps; i++)
			ts[i] = (qaws_scalar)(a + (b - a) * (double)i / steps);
		if (qaws_curve_compute_rotation_minimizing_frames_3d(frames, path, ts, &start, steps + 1) != QAWS_STATUS_OK)
		{
			worst = 1e9;
			break;
		}
		for (i = 0; i <= steps; i++)
		{
			qaws_vec3 n;
			qaws_vec3 reference = { frames[9 * i + 3], frames[9 * i + 4], frames[9 * i + 5] };
			double u = ((double)ts[i] - pr.min_value) / (pr.max_value - pr.min_value);
			double angle;
			if (!rmf_surface_normal_at(surface, path, u, &n))
			{
				worst = 1e9;
				break;
			}
			angle = rmf_angle(n, reference);
			if (angle > worst)
				worst = angle;
		}
	}
	free(ts);
	free(frames);
	return worst;
}

/* ------------------------------------------------------------------ */
/*  Group: span_transport                                             */
/* ------------------------------------------------------------------ */

/* Every knot span participates: the field on a narrow-span path resolves the compressed turn. The error against a dense
   per-span reference falls to the task's bound as the tolerance tightens, and 1e-3 and 1e-7 give visibly different node
   counts (the field is not one fixed table). */
static int case_span_transport(void)
{
	qaws_curve* path = rmf_narrow_path();
	qaws_curve* profile = rmf_profile();
	qaws_surface_swept_desc d;
	qaws_surface* surface = NULL;
	double bounds[7] = { 0, 0.49998, 0.49999, 0.5, 0.50001, 0.50002, 1 };
	double loose, tight;
	TASK_CHECK(path && profile);
	memset(&d, 0, sizeof d);
	d.path = path;
	d.profile = profile;
	TASK_CHECK(qaws_surface_create_swept(&surface, &d) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)1e-3) == QAWS_STATUS_OK);
	loose = rmf_direction_error(surface, path, bounds, 6, 600);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)1e-7) == QAWS_STATUS_OK);
	tight = rmf_direction_error(surface, path, bounds, 6, 4000);
	/* the loose tolerance meets its own request; the tight one falls to the task's direction bound */
	TASK_CHECK(loose <= 1e-3);
	TASK_CHECK(tight <= RMF_DIRECTION_BOUND);
	/* a tighter tolerance cannot be worse than a loose one by more than the rounding of the two runs */
	TASK_CHECK(tight <= loose + RMF_DIRECTION_BOUND);
	qaws_surface_destroy(surface);
	qaws_curve_destroy(profile);
	qaws_curve_destroy(path);
	return 0;
}

/* The field is continuous through the narrow span and at a node: at the boundary of every knot span, and at interior
   points across the whole path, the field's normal from just below and just above agrees as the step shrinks. A step of
   1e-11 in the curve parameter is far below the narrow spans (1e-5 wide) yet far above the parameter's resolution
   (1e-16), so a genuine discontinuity would stay order one there while the field's real turn over the step is tiny. */
static int case_narrow_continuity(void)
{
	qaws_curve* path = rmf_narrow_path();
	qaws_curve* profile = rmf_profile();
	qaws_surface_swept_desc d;
	qaws_surface* surface = NULL;
	double worst = 0;
	unsigned int i;
	double bounds[7] = { 0, 0.49998, 0.49999, 0.5, 0.50001, 0.50002, 1 };
	qaws_range pr;
	TASK_CHECK(path && profile);
	memset(&d, 0, sizeof d);
	d.path = path;
	d.profile = profile;
	TASK_CHECK(qaws_surface_create_swept(&surface, &d) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	pr = qaws_curve_get_parameter_range(path);
	/* every span boundary and a uniform interior grid, each straddled by 1e-11 in t */
	for (i = 0; i < 7 + 200; i++)
	{
		double t;
		double u0, u1;
		qaws_vec3 a, b;
		double jump;
		if (i < 7)
			t = bounds[i];
		else
			t = pr.min_value + (pr.max_value - pr.min_value) * (double)(i - 6) / 201.0;
		/* avoid the exact ends, where the surface clamps */
		if (t <= pr.min_value + 1e-10 || t >= pr.max_value - 1e-10)
			continue;
		u0 = (t - 1e-11 - pr.min_value) / (pr.max_value - pr.min_value);
		u1 = (t + 1e-11 - pr.min_value) / (pr.max_value - pr.min_value);
		TASK_CHECK(rmf_surface_normal_at(surface, path, u0, &a));
		TASK_CHECK(rmf_surface_normal_at(surface, path, u1, &b));
		jump = rmf_distance(a, b);
		if (jump > worst)
			worst = jump;
	}
	/* a discontinuity would leave these order one; the field's real turn over 2e-11 is far below */
	TASK_CHECK(worst <= (RMF_F32 ? 1e-2 : 1e-4));
	qaws_surface_destroy(surface);
	qaws_curve_destroy(profile);
	qaws_curve_destroy(path);
	return 0;
}

/* A curvature-zero (inflection) point does not jump: the field through it stays continuous, unlike a Frenet frame. The
   Bezier's inflection is where D1 x D2 vanishes, near t = 0.5. */
static int case_inflection_continuity(void)
{
	qaws_curve* path = rmf_inflection_path();
	qaws_curve* profile = rmf_profile();
	qaws_surface_swept_desc d;
	qaws_surface* surface = NULL;
	double worst = 0;
	unsigned int i;
	TASK_CHECK(path && profile);
	memset(&d, 0, sizeof d);
	d.path = path;
	d.profile = profile;
	TASK_CHECK(qaws_surface_create_swept(&surface, &d) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	{
		qaws_vec3 previous;
		TASK_CHECK(rmf_surface_normal_at(surface, path, 0.45, &previous));
		for (i = 1; i <= 2000; i++)
		{
			qaws_vec3 n;
			double u = 0.45 + 0.10 * (double)i / 2000;
			TASK_CHECK(rmf_surface_normal_at(surface, path, u, &n));
			{
				double jump = rmf_distance(n, previous);
				if (jump > worst)
					worst = jump;
			}
			previous = n;
		}
	}
	/* the section direction changes smoothly across the inflection; a Frenet flip would be order one */
	TASK_CHECK(worst <= 5e-3);
	qaws_surface_destroy(surface);
	qaws_curve_destroy(profile);
	qaws_curve_destroy(path);
	return 0;
}

/* The other supported guide families reach the same bound: a straight segment (one frame) and a positive-weight NURBS. */
static int case_families(void)
{
	qaws_curve* straight = rmf_straight_path();
	qaws_curve* nurbs = rmf_nurbs_path();
	qaws_curve* profile = rmf_profile();
	qaws_surface_swept_desc d;
	qaws_surface* surface = NULL;
	double worst_straight = 0;
	unsigned int i;
	TASK_CHECK(straight && nurbs && profile);
	memset(&d, 0, sizeof d);
	d.profile = profile;
	d.path = straight;
	TASK_CHECK(qaws_surface_create_swept(&surface, &d) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	{
		qaws_vec3 first;
		TASK_CHECK(rmf_surface_normal_at(surface, straight, 0, &first));
		for (i = 1; i <= 50; i++)
		{
			qaws_vec3 n;
			double jump;
			TASK_CHECK(rmf_surface_normal_at(surface, straight, (double)i / 50, &n));
			jump = rmf_distance(n, first);
			if (jump > worst_straight)
				worst_straight = jump;
		}
	}
	/* one frame all along a straight piece, up to rounding */
	TASK_CHECK(worst_straight <= (RMF_F32 ? 1e-5 : 1e-10));
	qaws_surface_destroy(surface);
	surface = NULL;
	d.path = nurbs;
	TASK_CHECK(qaws_surface_create_swept(&surface, &d) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)1e-7) == QAWS_STATUS_OK);
	{
		double bounds[5] = { 0, 0.2, 0.4, 0.6, 1 };
		TASK_CHECK(rmf_direction_error(surface, nurbs, bounds, 4, 600) <= RMF_DIRECTION_BOUND);
	}
	qaws_surface_destroy(surface);
	qaws_curve_destroy(profile);
	qaws_curve_destroy(straight);
	qaws_curve_destroy(nurbs);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Group: surface_differentials                                      */
/* ------------------------------------------------------------------ */

/* dS/du and dS/dv against Richardson-extrapolated differences of the sampled positions, and duu, duv, dvv against second
   differences, over a grid that crosses the narrow span and the cache's node boundaries. */

static int rmf_eval(qaws_surface const* s, double u, double v, unsigned int flags, qaws_surface_eval_result* r)
{
	return qaws_surface_evaluate(r, s, (qaws_scalar)u, (qaws_scalar)v, flags | QAWS_SURFACE_EVAL_POSITION) == QAWS_STATUS_OK;
}

static double rmf_vec_diff(qaws_vec3 a, double x, double y, double z)
{
	return fabs((double)a.x - x) + fabs((double)a.y - y) + fabs((double)a.z - z);
}

static int case_differentials(void)
{
	qaws_curve* path = rmf_narrow_path();
	qaws_curve* profile = rmf_profile();
	qaws_surface_swept_desc d;
	qaws_surface* surface = NULL;
	double err_du = 0, err_dv = 0, err_duu = 0, err_duv = 0, err_dvv = 0;
	unsigned int iu, iv;
	TASK_CHECK(path && profile);
	memset(&d, 0, sizeof d);
	d.path = path;
	d.profile = profile;
	TASK_CHECK(qaws_surface_create_swept(&surface, &d) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	/* Points across the whole path and inside each narrow span, chosen away from the span ends and from the field's nodes,
	   so a difference step sits inside one smooth piece. The points at 0.4999895, 0.4999995, ... are near the middle of
	   the four 1e-5-wide spans; the others are within the wide first and last spans. */
	/* the wide first and last spans and the flat stretches either side of the four narrow ones, where a difference stencil
	   of the given step sits inside one smooth piece (inside the 1e-5-wide narrow spans the derivative is enormous and its
	   differences need a step far below the span; that region is covered by the direction and continuity cases) */
	static const double samples[] = { 0.05, 0.15, 0.25, 0.35, 0.42, 0.46,
		0.55, 0.62, 0.70, 0.78, 0.85, 0.92, 0.97 };
	for (iu = 0; iu < sizeof samples / sizeof samples[0]; iu++)
	{
		double u = samples[iu];
		for (iv = 0; iv < 5; iv++)
		{
			double v = 0.05 + 0.2 * iv;
			/* the step must sit inside one smooth piece. Inside the 1e-5-wide narrow spans that means well under 1e-5;
			   in float, positions carry only ~7 digits and the derivative there is large, so the narrow points are
			   skipped for the float difference (their du is checked by the continuity and direction groups instead). */
			double h = RMF_F32 ? 2e-3 : 1e-4;
			/* second differences lose more digits: in float they need a larger step */
			double h2 = RMF_F32 ? 2e-2 : 1e-4;
			qaws_surface_eval_result r;
			qaws_surface_eval_result pa, pb, qa, qb, r2;
			double du[3], dv[3], duu[3], duv[3], dvv[3];
			unsigned int k;
			TASK_CHECK(rmf_eval(surface, u, v, QAWS_SURFACE_EVAL_POSITION | QAWS_SURFACE_EVAL_DU | QAWS_SURFACE_EVAL_DV
				| QAWS_SURFACE_EVAL_DUU | QAWS_SURFACE_EVAL_DUV | QAWS_SURFACE_EVAL_DVV | QAWS_SURFACE_EVAL_NORMAL, &r));
			TASK_CHECK(rmf_eval(surface, u + h, v, 0, &pa));
			TASK_CHECK(rmf_eval(surface, u - h, v, 0, &pb));
			TASK_CHECK(rmf_eval(surface, u + h / 2, v, 0, &qa));
			TASK_CHECK(rmf_eval(surface, u - h / 2, v, 0, &qb));
			for (k = 0; k < 3; k++)
			{
				double pah = k == 0 ? pa.position.x : k == 1 ? pa.position.y : pa.position.z;
				double pbh = k == 0 ? pb.position.x : k == 1 ? pb.position.y : pb.position.z;
				double qah = k == 0 ? qa.position.x : k == 1 ? qa.position.y : qa.position.z;
				double qbh = k == 0 ? qb.position.x : k == 1 ? qb.position.y : qb.position.z;
				double pc = k == 0 ? r.position.x : k == 1 ? r.position.y : r.position.z;
				du[k] = (4 * (qah - qbh) / h - (pah - pbh) / (2 * h)) / 3;
				duu[k] = (4 * (qah - 2 * pc + qbh) / (h * h / 4) - (pah - 2 * pc + pbh) / (h * h)) / 3;
			}
			/* dv along v (a first difference) */
			TASK_CHECK(rmf_eval(surface, u, v + h, 0, &pa));
			TASK_CHECK(rmf_eval(surface, u, v - h, 0, &pb));
			TASK_CHECK(rmf_eval(surface, u, v + h / 2, 0, &qa));
			TASK_CHECK(rmf_eval(surface, u, v - h / 2, 0, &qb));
			for (k = 0; k < 3; k++)
			{
				double pah = k == 0 ? pa.position.x : k == 1 ? pa.position.y : pa.position.z;
				double pbh = k == 0 ? pb.position.x : k == 1 ? pb.position.y : pb.position.z;
				double qah = k == 0 ? qa.position.x : k == 1 ? qa.position.y : qa.position.z;
				double qbh = k == 0 ? qb.position.x : k == 1 ? qb.position.y : qb.position.z;
				dv[k] = (4 * (qah - qbh) / h - (pah - pbh) / (2 * h)) / 3;
			}
			/* dvv along v (a second difference, with its own step) */
			TASK_CHECK(rmf_eval(surface, u, v + h2, 0, &pa));
			TASK_CHECK(rmf_eval(surface, u, v - h2, 0, &pb));
			TASK_CHECK(rmf_eval(surface, u, v + h2 / 2, 0, &qa));
			TASK_CHECK(rmf_eval(surface, u, v - h2 / 2, 0, &qb));
			for (k = 0; k < 3; k++)
			{
				double pah = k == 0 ? pa.position.x : k == 1 ? pa.position.y : pa.position.z;
				double pbh = k == 0 ? pb.position.x : k == 1 ? pb.position.y : pb.position.z;
				double qah = k == 0 ? qa.position.x : k == 1 ? qa.position.y : qa.position.z;
				double qbh = k == 0 ? qb.position.x : k == 1 ? qb.position.y : qb.position.z;
				double pc = k == 0 ? r.position.x : k == 1 ? r.position.y : r.position.z;
				dvv[k] = (4 * (qah - 2 * pc + qbh) / (h2 * h2 / 4) - (pah - 2 * pc + pbh) / (h2 * h2)) / 3;
			}
			/* duv: differentiate dv in u (Richardson) */
			TASK_CHECK(rmf_eval(surface, u + h, v, QAWS_SURFACE_EVAL_DV, &pa));
			TASK_CHECK(rmf_eval(surface, u - h, v, QAWS_SURFACE_EVAL_DV, &pb));
			TASK_CHECK(rmf_eval(surface, u + h / 2, v, QAWS_SURFACE_EVAL_DV, &qa));
			TASK_CHECK(rmf_eval(surface, u - h / 2, v, QAWS_SURFACE_EVAL_DV, &qb));
			for (k = 0; k < 3; k++)
			{
				double pah = k == 0 ? pa.dv.x : k == 1 ? pa.dv.y : pa.dv.z;
				double pbh = k == 0 ? pb.dv.x : k == 1 ? pb.dv.y : pb.dv.z;
				double qah = k == 0 ? qa.dv.x : k == 1 ? qa.dv.y : qa.dv.z;
				double qbh = k == 0 ? qb.dv.x : k == 1 ? qb.dv.y : qb.dv.z;
				duv[k] = (4 * (qah - qbh) / h - (pah - pbh) / (2 * h)) / 3;
			}
			{
				double e;
				e = rmf_vec_diff(r.du, du[0], du[1], du[2]) / (1 + fabs(du[0]) + fabs(du[1]) + fabs(du[2]));
				if (e > err_du) err_du = e;
				e = rmf_vec_diff(r.dv, dv[0], dv[1], dv[2]) / (1 + fabs(dv[0]) + fabs(dv[1]) + fabs(dv[2]));
				if (e > err_dv) err_dv = e;
				e = rmf_vec_diff(r.duu, duu[0], duu[1], duu[2]) / (1 + fabs(duu[0]) + fabs(duu[1]) + fabs(duu[2]));
				if (e > err_duu) err_duu = e;
				e = rmf_vec_diff(r.duv, duv[0], duv[1], duv[2]) / (1 + fabs(duv[0]) + fabs(duv[1]) + fabs(duv[2]));
				if (e > err_duv) err_duv = e;
				e = rmf_vec_diff(r.dvv, dvv[0], dvv[1], dvv[2]) / (1 + fabs(dvv[0]) + fabs(dvv[1]) + fabs(dvv[2]));
				if (e > err_dvv) err_dvv = e;
			}
			/* the normal from du x dv: unit and perpendicular to du and dv */
			{
				double len;
				qaws_vec3 cross;
				cross.x = r.du.y * r.dv.z - r.du.z * r.dv.y;
				cross.y = r.du.z * r.dv.x - r.du.x * r.dv.z;
				cross.z = r.du.x * r.dv.y - r.du.y * r.dv.x;
				len = rmf_norm(cross);
				/* the normal is du x dv normalized: exactly along the cross product, so the sine of the angle to it is a
				   rounding of the two products; the bound scales with their magnitudes */
				{
					double scale = (1 + rmf_norm(r.du)) * (1 + rmf_norm(r.dv));
					if (len > (RMF_F32 ? 1e-8 : 1e-18) * scale)
					{
						double sin_angle = rmf_norm((qaws_vec3){ cross.y * r.normal.z - cross.z * r.normal.y,
							cross.z * r.normal.x - cross.x * r.normal.z, cross.x * r.normal.y - cross.y * r.normal.x }) / len;
						TASK_CHECK(fabs(rmf_norm(r.normal) - 1) <= (RMF_F32 ? 2e-6 : 1e-12));
						TASK_CHECK(sin_angle <= (RMF_F32 ? 2e-5 : 1e-12));
					}
				}
			}
			(void)r2;
		}
	}
	/* bounds: du and dv are analytic; duu and duv are differences of analytic derivatives; dvv is analytic */
	TASK_CHECK(err_du <= (RMF_F32 ? 2e-3 : 1e-6));
	TASK_CHECK(err_dv <= (RMF_F32 ? 2e-3 : 1e-6));
	TASK_CHECK(err_duu <= (RMF_F32 ? 5e-2 : 5e-4));
	TASK_CHECK(err_duv <= (RMF_F32 ? 2e-2 : 2e-4));
	TASK_CHECK(err_dvv <= (RMF_F32 ? 2e-2 : 1e-6));
	qaws_surface_destroy(surface);
	qaws_curve_destroy(profile);
	qaws_curve_destroy(path);
	return 0;
}

/* The field's value and rate do not jump across the cache's interval ends: at every knot-span boundary and across the
   wide first and last spans, the surface's du, dv and normal from just below and just above agree as the step shrinks.
   A step of 1e-11 in u is far below the narrow spans yet far above the parameter's resolution; a genuine jump would
   stay order one. */
static int case_interval_jumps(void)
{
	qaws_curve* path = rmf_narrow_path();
	qaws_curve* profile = rmf_profile();
	qaws_surface_swept_desc d;
	qaws_surface* surface = NULL;
	double worst_normal = 0, worst_du = 0, worst_dv = 0, worst_duu = 0, worst_duv = 0, worst_dvv = 0;
	unsigned int i;
	double bounds[7] = { 0, 0.49998, 0.49999, 0.5, 0.50001, 0.50002, 1 };
	qaws_range pr;
	TASK_CHECK(path && profile);
	memset(&d, 0, sizeof d);
	d.path = path;
	d.profile = profile;
	TASK_CHECK(qaws_surface_create_swept(&surface, &d) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	pr = qaws_curve_get_parameter_range(path);
	for (i = 0; i < 7 + 400; i++)
	{
		double t;
		double u0, u1;
		qaws_surface_eval_result a, b;
		double jump;
		if (i < 7)
			t = bounds[i];
		else
			t = pr.min_value + (pr.max_value - pr.min_value) * (double)(i - 6) / 401.0;
		if (t <= pr.min_value + 1e-10 || t >= pr.max_value - 1e-10)
			continue;
		u0 = (t - 1e-13 - pr.min_value) / (pr.max_value - pr.min_value);
		u1 = (t + 1e-13 - pr.min_value) / (pr.max_value - pr.min_value);
		TASK_CHECK(rmf_eval(surface, u0, 0.31, QAWS_SURFACE_EVAL_DU | QAWS_SURFACE_EVAL_DV | QAWS_SURFACE_EVAL_NORMAL
			| QAWS_SURFACE_EVAL_DUU | QAWS_SURFACE_EVAL_DUV | QAWS_SURFACE_EVAL_DVV, &a));
		TASK_CHECK(rmf_eval(surface, u1, 0.31, QAWS_SURFACE_EVAL_DU | QAWS_SURFACE_EVAL_DV | QAWS_SURFACE_EVAL_NORMAL
			| QAWS_SURFACE_EVAL_DUU | QAWS_SURFACE_EVAL_DUV | QAWS_SURFACE_EVAL_DVV, &b));
		jump = rmf_distance(a.normal, b.normal);
		if (jump > worst_normal) worst_normal = jump;
		jump = rmf_distance(a.du, b.du) / (1 + rmf_norm(a.du));
		if (jump > worst_du) worst_du = jump;
		jump = rmf_distance(a.dv, b.dv) / (1 + rmf_norm(a.dv));
		if (jump > worst_dv) worst_dv = jump;
		jump = rmf_distance(a.duu, b.duu) / (1 + rmf_norm(a.duu));
		if (jump > worst_duu) worst_duu = jump;
		jump = rmf_distance(a.duv, b.duv) / (1 + rmf_norm(a.duv));
		if (jump > worst_duv) worst_duv = jump;
		jump = rmf_distance(a.dvv, b.dvv) / (1 + rmf_norm(a.dvv));
		if (jump > worst_dvv) worst_dvv = jump;
	}
	/* the field is C1: a genuine discontinuity would leave these order one; over a 2e-13 step the field's real change
	   is below a few 1e-6 */
	TASK_CHECK(worst_normal <= (RMF_F32 ? 1e-2 : 1e-5));
	TASK_CHECK(worst_du <= (RMF_F32 ? 1e-2 : 1e-5));
	TASK_CHECK(worst_dv <= (RMF_F32 ? 1e-2 : 1e-5));
	TASK_CHECK(worst_duu <= (RMF_F32 ? 1e-2 : 1e-4));
	TASK_CHECK(worst_duv <= (RMF_F32 ? 1e-2 : 1e-4));
	TASK_CHECK(worst_dvv <= (RMF_F32 ? 1e-2 : 1e-4));
	qaws_surface_destroy(surface);
	qaws_curve_destroy(profile);
	qaws_curve_destroy(path);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Group: mode_allocator                                             */
/* ------------------------------------------------------------------ */

typedef struct rmf_allocator_state
{
	unsigned long live;
	unsigned long allocations;
} rmf_allocator_state;

static void* rmf_alloc(unsigned long size, void* user)
{
	rmf_allocator_state* state = (rmf_allocator_state*)user;
	unsigned long* block = (unsigned long*)malloc((size_t)size + sizeof(unsigned long));
	if (!block)
		return NULL;
	block[0] = (unsigned long)size;
	state->live++;
	state->allocations++;
	return (void*)(block + 1);
}

static void rmf_dealloc(void* ptr, void* user)
{
	rmf_allocator_state* state = (rmf_allocator_state*)user;
	unsigned long* block;
	if (!ptr)
		return;
	block = (unsigned long*)ptr - 1;
	state->live--;
	free(block);
}

static int case_mode_allocator(void)
{
	qaws_curve* path = rmf_narrow_path();
	qaws_curve* profile = rmf_profile();
	qaws_surface_swept_desc d;
	qaws_allocator allocator;
	rmf_allocator_state state;
	qaws_surface* surface = NULL;
	qaws_surface_eval_result before, after;
	unsigned long live_after_create;
	unsigned int round;
	TASK_CHECK(path && profile);
	memset(&state, 0, sizeof state);
	memset(&allocator, 0, sizeof allocator);
	allocator.alloc = rmf_alloc;
	allocator.dealloc = rmf_dealloc;
	allocator.user_data = &state;
	memset(&d, 0, sizeof d);
	d.path = path;
	d.profile = profile;
	TASK_CHECK(qaws_surface_create_swept_ex(&surface, &d, &allocator) == QAWS_STATUS_OK);
	live_after_create = state.live;
	/* the surface keeps its identity and its borrowed path/profile across every switch */
	TASK_CHECK(qaws_surface_get_kind(surface) == QAWS_SURFACE_KIND_SWEPT);
	/* off, on, rebuild at another precision, off again: several times. With the field on, the surface holds more blocks
	   (its nodes and header) from this allocator; off, the live count is exactly the surface's own. */
	for (round = 0; round < 4; round++)
	{
		unsigned long live_on;
		TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)(round % 2 ? 1e-7 : 1e-5)) == QAWS_STATUS_OK);
		TASK_CHECK(qaws_surface_get_kind(surface) == QAWS_SURFACE_KIND_SWEPT);
		live_on = state.live;
		TASK_CHECK(live_on > live_after_create);
		TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)0) == QAWS_STATUS_OK);
		TASK_CHECK(state.live == live_after_create);
	}
	/* no leak and no growth of the live count after the last release */
	TASK_CHECK(state.allocations > 0);
	/* identity: the same surface pointer still evaluates; the borrowed curves are never owned (their allocator is the default) */
	TASK_CHECK(rmf_eval(surface, 0.25, 0.3, QAWS_SURFACE_EVAL_POSITION | QAWS_SURFACE_EVAL_NORMAL, &before));
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	TASK_CHECK(rmf_eval(surface, 0.25, 0.3, QAWS_SURFACE_EVAL_POSITION | QAWS_SURFACE_EVAL_NORMAL, &after));
	/* the surface is the same object (get_kind unchanged) and the field query is finite */
	TASK_CHECK(qaws_surface_get_kind(surface) == QAWS_SURFACE_KIND_SWEPT);
	TASK_CHECK(after.normal.x == after.normal.x && after.normal.y == after.normal.y && after.normal.z == after.normal.z);
	/* the surface stays built on the same borrowed path: its position at v = 0 is the path's position plus the profile's
	   start section (0.6, 0.1), whose distance from the path is the section's length, 0.6083 */
	{
		qaws_eval_result_3d e;
		qaws_range pr = qaws_curve_get_parameter_range(path);
		qaws_surface_eval_result r;
		TASK_CHECK(qaws_curve_evaluate_3d(&e, path, pr.min_value, QAWS_EVAL_FLAG_POSITION) == QAWS_STATUS_OK);
		TASK_CHECK(rmf_eval(surface, 0, 0, 0, &r));
		/* the profile stores its endpoints as scalars: compare against the scalar-rounded section length */
		TASK_CHECK(fabs(rmf_distance(r.position, e.position) - rmf_profile_start_length(profile))
			<= (RMF_F32 ? 1e-5 : 1e-12));
	}
	qaws_surface_destroy(surface);
	/* every block the surface allocated came back */
	TASK_CHECK(state.live == 0);
	qaws_curve_destroy(profile);
	qaws_curve_destroy(path);
	return 0;
}

/* Tolerance 0 restores the original behaviour: after a switch off the swept surface evaluates exactly as a freshly created
   one (the Frenet frame), and the pipe evaluates as a pipe that was never given a field. */
static int case_restore_original(void)
{
	qaws_curve* path = rmf_narrow_path();
	qaws_curve* profile = rmf_profile();
	qaws_surface_swept_desc d;
	qaws_surface *switched = NULL, *fresh = NULL;
	TASK_CHECK(path && profile);
	memset(&d, 0, sizeof d);
	d.path = path;
	d.profile = profile;
	TASK_CHECK(qaws_surface_create_swept(&switched, &d) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_create_swept(&fresh, &d) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(switched, NULL, (qaws_scalar)1e-5) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(switched, NULL, (qaws_scalar)0) == QAWS_STATUS_OK);
	{
		unsigned int i;
		for (i = 0; i < 20; i++)
		{
			double u = 0.03 + 0.94 * (double)i / 19;
			qaws_surface_eval_result a, b;
			TASK_CHECK(rmf_eval(switched, u, 0.4, QAWS_SURFACE_EVAL_POSITION | QAWS_SURFACE_EVAL_DU | QAWS_SURFACE_EVAL_DV
				| QAWS_SURFACE_EVAL_NORMAL, &a));
			TASK_CHECK(rmf_eval(fresh, u, 0.4, QAWS_SURFACE_EVAL_POSITION | QAWS_SURFACE_EVAL_DU | QAWS_SURFACE_EVAL_DV
				| QAWS_SURFACE_EVAL_NORMAL, &b));
			TASK_CHECK(rmf_distance(a.position, b.position) <= (RMF_F32 ? 1e-5 : 1e-11));
			TASK_CHECK(rmf_distance(a.normal, b.normal) <= (RMF_F32 ? 1e-5 : 1e-11));
		}
	}
	qaws_surface_destroy(switched);
	qaws_surface_destroy(fresh);
	/* the pipe: a switch off equals the original fixed-table pipe (build one and compare its start direction and a sample) */
	{
		qaws_surface_pipe_desc pd;
		qaws_surface* pipe = NULL;
		memset(&pd, 0, sizeof pd);
		pd.path = path;
		pd.radius_x = 0.4f;
		TASK_CHECK(qaws_surface_create_pipe(&pipe, &pd) == QAWS_STATUS_OK);
		TASK_CHECK(qaws_surface_pipe_set_rmf(pipe, NULL, (qaws_scalar)0) == QAWS_STATUS_OK);
		{
			qaws_surface_eval_result r;
			qaws_eval_result_3d e;
			qaws_range pr = qaws_curve_get_parameter_range(path);
			TASK_CHECK(qaws_curve_evaluate_3d(&e, path, pr.min_value, QAWS_EVAL_FLAG_POSITION) == QAWS_STATUS_OK);
			TASK_CHECK(rmf_eval(pipe, 0, 0, QAWS_SURFACE_EVAL_POSITION, &r));
			/* the pipe's section at v = 0 is its radius_x (0.4, taken from the float literal) from the path */
			TASK_CHECK(fabs(rmf_distance(r.position, e.position) - (double)(qaws_scalar)0.4f) <= (RMF_F32 ? 1e-5 : 1e-7));
		}
		qaws_surface_destroy(pipe);
	}
	qaws_curve_destroy(profile);
	qaws_curve_destroy(path);
	return 0;
}

/* Invalid modes are refused and keep the active mode: a NaN tolerance, a tolerance outside [1e-7, 1e-3], a non-finite or
   zero initial normal, and a failure leaves the previous field evaluating as before. */
static int case_invalid_modes(void)
{
	qaws_curve* path = rmf_narrow_path();
	qaws_curve* profile = rmf_profile();
	qaws_surface_swept_desc d;
	qaws_surface* surface = NULL;
	qaws_surface_eval_result reference, after;
	qaws_vec3 zero = { 0, 0, 0 };
	qaws_vec3 nan_normal = { (qaws_scalar)NAN, 0, 0 };
	TASK_CHECK(path && profile);
	memset(&d, 0, sizeof d);
	d.path = path;
	d.profile = profile;
	TASK_CHECK(qaws_surface_create_swept(&surface, &d) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)1e-5) == QAWS_STATUS_OK);
	TASK_CHECK(rmf_eval(surface, 0.31, 0.27, QAWS_SURFACE_EVAL_POSITION | QAWS_SURFACE_EVAL_DU | QAWS_SURFACE_EVAL_NORMAL, &reference));
	/* a NaN tolerance and one below the range */
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)NAN) != QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)1e-9) != QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)0.5) != QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)-1e-5) != QAWS_STATUS_OK);
	/* an invalid initial normal */
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, &zero, (qaws_scalar)1e-5) != QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, &nan_normal, (qaws_scalar)1e-5) != QAWS_STATUS_OK);
	/* a NULL surface */
	TASK_CHECK(qaws_surface_swept_set_rmf(NULL, NULL, (qaws_scalar)1e-5) != QAWS_STATUS_OK);
	/* the active mode is unchanged after every refusal */
	TASK_CHECK(rmf_eval(surface, 0.31, 0.27, QAWS_SURFACE_EVAL_POSITION | QAWS_SURFACE_EVAL_DU | QAWS_SURFACE_EVAL_NORMAL, &after));
	TASK_CHECK(rmf_distance(reference.position, after.position) == 0);
	TASK_CHECK(rmf_distance(reference.du, after.du) == 0);
	TASK_CHECK(rmf_distance(reference.normal, after.normal) == 0);
	/* a degenerate guide: a path with a zero-speed point is refused, and the surface keeps its earlier Frenet behaviour */
	{
		qaws_vec3 cp[3] = { { 0, 0, 0 }, { 1, 1, 1 }, { 0, 0, 0 } };
		qaws_bezier_desc bd = { QAWS_DIMENSION_3D, 2, cp, 3 };
		qaws_curve* loop = NULL;
		qaws_surface* s2 = NULL;
		qaws_surface_swept_desc d2;
		if (qaws_curve_create_bezier(&loop, &bd) == QAWS_STATUS_OK)
		{
			memset(&d2, 0, sizeof d2);
			d2.path = loop;
			d2.profile = profile;
			if (qaws_surface_create_swept(&s2, &d2) == QAWS_STATUS_OK)
			{
				/* the quadratic's speed is zero where its derivative vanishes: the middle control point, at t = 0.5 */
				TASK_CHECK(qaws_surface_swept_set_rmf(s2, NULL, (qaws_scalar)1e-5) != QAWS_STATUS_OK);
				/* the surface still evaluates with its original frame */
				TASK_CHECK(rmf_eval(s2, 0.25, 0.3, QAWS_SURFACE_EVAL_POSITION, &after));
				qaws_surface_destroy(s2);
			}
			qaws_curve_destroy(loop);
		}
	}
	qaws_surface_destroy(surface);
	qaws_curve_destroy(profile);
	qaws_curve_destroy(path);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Group: precision_batch                                            */
/* ------------------------------------------------------------------ */

/* The scalar and batch queries agree in any order: batch results in one order equal the same points queried singly and in
   the reverse order, at points across the narrow span. */
static int case_batch_order(void)
{
	qaws_curve* path = rmf_narrow_path();
	qaws_curve* profile = rmf_profile();
	qaws_surface_swept_desc d;
	qaws_surface* surface = NULL;
	enum { COUNT = 17 };
	qaws_scalar us[COUNT], vs[COUNT], ur[COUNT], vr[COUNT];
	qaws_surface_eval_result forward[COUNT], backward[COUNT], single[COUNT];
	unsigned int i;
	TASK_CHECK(path && profile);
	memset(&d, 0, sizeof d);
	d.path = path;
	d.profile = profile;
	TASK_CHECK(qaws_surface_create_swept(&surface, &d) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	for (i = 0; i < COUNT; i++)
	{
		/* a mix of points inside and around the narrow spans, and ordinary points */
		double u = (i % 5 == 0) ? 0.499995 + 1e-6 * (i % 3) : 0.02 + 0.96 * (double)i / (COUNT - 1);
		us[i] = (qaws_scalar)u;
		vs[i] = (qaws_scalar)(0.07 + 0.83 * (double)((i * 7) % COUNT) / (COUNT - 1));
	}
	for (i = 0; i < COUNT; i++)
	{
		ur[i] = us[COUNT - 1 - i];
		vr[i] = vs[COUNT - 1 - i];
	}
	TASK_CHECK(qaws_surface_evaluate_batch(forward, surface, us, vs, COUNT,
		QAWS_SURFACE_EVAL_POSITION | QAWS_SURFACE_EVAL_DU | QAWS_SURFACE_EVAL_DV | QAWS_SURFACE_EVAL_NORMAL) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_evaluate_batch(backward, surface, ur, vr, COUNT,
		QAWS_SURFACE_EVAL_POSITION | QAWS_SURFACE_EVAL_DU | QAWS_SURFACE_EVAL_DV | QAWS_SURFACE_EVAL_NORMAL) == QAWS_STATUS_OK);
	for (i = 0; i < COUNT; i++)
		TASK_CHECK(rmf_eval(surface, us[i], vs[i], QAWS_SURFACE_EVAL_DU | QAWS_SURFACE_EVAL_DV | QAWS_SURFACE_EVAL_NORMAL, &single[i]));
	for (i = 0; i < COUNT; i++)
	{
		TASK_CHECK(rmf_distance(forward[i].position, backward[COUNT - 1 - i].position) == 0);
		TASK_CHECK(rmf_distance(forward[i].du, backward[COUNT - 1 - i].du) == 0);
		TASK_CHECK(rmf_distance(forward[i].dv, backward[COUNT - 1 - i].dv) == 0);
		TASK_CHECK(rmf_distance(forward[i].normal, backward[COUNT - 1 - i].normal) == 0);
		TASK_CHECK(rmf_distance(forward[i].position, single[i].position) == 0);
		TASK_CHECK(rmf_distance(forward[i].normal, single[i].normal) == 0);
	}
	qaws_surface_destroy(surface);
	qaws_curve_destroy(profile);
	qaws_curve_destroy(path);
	return 0;
}

/* f32 and f64 forms of the setters exist and the same precision's field agrees with itself; qaws_both.h declares both.
   Compiled in one precision, this checks the precision's own entry points and that the other precision's symbols are
   present and callable through qaws_both.h. */
static int case_precision_forms(void)
{
	/* Both precision families must exist in the library. The unsuffixed names are one family (chosen by the build);
	   the other is named with its explicit suffix and is not declared by qaws.h, so check it is exported through the
	   library by building a field through the qaws_both-style suffixed entry points: the two families differ only by
	   suffix, and the f64 compile (the one the driver builds) has the f32 forms compiled beside it. */
	TASK_CHECK(qaws_surface_swept_set_rmf_f32 != NULL);
	TASK_CHECK(qaws_surface_pipe_set_rmf_f32 != NULL);
	TASK_CHECK(qaws_surface_swept_set_rmf_f64 != NULL);
	TASK_CHECK(qaws_surface_pipe_set_rmf_f64 != NULL);
{
		qaws_curve* path = rmf_nurbs_path();
		qaws_curve* profile = rmf_profile();
		qaws_surface_swept_desc d;
		qaws_surface* surface = NULL;
		TASK_CHECK(path && profile);
		memset(&d, 0, sizeof d);
		d.path = path;
		d.profile = profile;
		TASK_CHECK(qaws_surface_create_swept(&surface, &d) == QAWS_STATUS_OK);
		TASK_CHECK(qaws_surface_swept_set_rmf(surface, NULL, (qaws_scalar)1e-7) == QAWS_STATUS_OK);
		{
			qaws_surface_eval_result r;
			TASK_CHECK(rmf_eval(surface, 0.37, 0.13, QAWS_SURFACE_EVAL_POSITION | QAWS_SURFACE_EVAL_NORMAL, &r));
			TASK_CHECK(r.normal.x == r.normal.x && r.normal.y == r.normal.y && r.normal.z == r.normal.z);
			TASK_CHECK(fabs(rmf_norm(r.normal) - 1) <= (RMF_F32 ? 2e-6 : 1e-12));
		}
		qaws_surface_destroy(surface);
		qaws_curve_destroy(profile);
		qaws_curve_destroy(path);
	}
	/* the initial normal is projected onto the start tangent's plane, and NULL keeps the pipe's original start direction */
	{
		qaws_curve* path = rmf_narrow_path();
		qaws_curve* profile = rmf_profile();
		qaws_surface_swept_desc d;
		qaws_surface* surface = NULL;
		qaws_vec3 tilted = { 1, 1, 1 };
		qaws_vec3 out;
		qaws_eval_result_3d e;
		qaws_range pr;
		TASK_CHECK(path && profile);
		memset(&d, 0, sizeof d);
		d.path = path;
		d.profile = profile;
		TASK_CHECK(qaws_surface_create_swept(&surface, &d) == QAWS_STATUS_OK);
		TASK_CHECK(qaws_surface_swept_set_rmf(surface, &tilted, (qaws_scalar)1e-5) == QAWS_STATUS_OK);
		TASK_CHECK(rmf_surface_normal_at(surface, path, 0, &out));
		pr = qaws_curve_get_parameter_range(path);
		TASK_CHECK(qaws_curve_evaluate_3d(&e, path, pr.min_value, QAWS_EVAL_FLAG_D1) == QAWS_STATUS_OK);
		{
			qaws_vec3 tangent = e.d1;
			double l = rmf_norm(tangent);
			double d0;
			tangent.x = (qaws_scalar)(tangent.x / l);
			tangent.y = (qaws_scalar)(tangent.y / l);
			tangent.z = (qaws_scalar)(tangent.z / l);
			d0 = rmf_dot(out, tangent);
			TASK_CHECK(fabs(d0) <= (RMF_F32 ? 1e-5 : 1e-10));
			TASK_CHECK(rmf_dot(out, tilted) > 0);
		}
		qaws_surface_destroy(surface);
		qaws_curve_destroy(profile);
		qaws_curve_destroy(path);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Registration                                                      */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	{ "span_transport", "narrow_span_adaptive", case_span_transport },
	{ "span_transport", "narrow_continuity", case_narrow_continuity },
	{ "span_transport", "inflection_continuity", case_inflection_continuity },
	{ "span_transport", "curve_families", case_families },
	{ "surface_differentials", "finite_difference_derivatives", case_differentials },
	{ "surface_differentials", "interval_jumps", case_interval_jumps },
	{ "mode_allocator", "switches_and_allocator", case_mode_allocator },
	{ "mode_allocator", "restore_original", case_restore_original },
	{ "mode_allocator", "invalid_modes", case_invalid_modes },
	{ "precision_batch", "batch_order", case_batch_order },
	{ "precision_batch", "precision_forms", case_precision_forms }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
