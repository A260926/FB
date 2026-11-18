/*
 * sweep_rmf_task.c - the span-adaptive Bishop (rotation-minimizing) frame field
 *
 * Four groups:
 *   span_transport        narrow knot spans cannot skip a turn; the field matches
 *                         a per-span dense reference on Bezier, B-spline and NURBS
 *                         rails, stays continuous through an inflection and constant
 *                         on a straight rail, and beats the legacy fixed table;
 *   surface_differentials position, du, dv, duu, duv, dvv and the normal come from one
 *                         continuous field (differences of the positions, independently),
 *                         including the normalized-u to source-range chain scale;
 *   mode_allocator        enable/rebuild/disable keeps the surface identity and the
 *                         borrowed path/profile, every cache block uses the surface's
 *                         allocator, and refused calls keep the current mode;
 *   precision_batch       f32 and f64 agree, tighter tolerances reduce the reference
 *                         error to the target (past the scalar floor), and the answers
 *                         are independent of query order and of the batch path.
 *
 * The direction reference is independent of the new cache: it drives the public
 * qaws_curve_compute_rotation_minimizing_frames_3d over a dense per-span sampling
 * built from the rail's own knots. Every derivative check differences surface
 * positions rather than reading the field's tables.
 */

#include "task_support/check.h"
#include "qaws_both.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define IS_F32 (sizeof(qaws_scalar) == sizeof(float))
#define NUM_FLOOR (IS_F32 ? 2e-5 : 2e-8)
#define REF_STEPS (IS_F32 ? 512u : 2048u)
#define ALL_FLAGS (QAWS_SURFACE_EVAL_POSITION | QAWS_SURFACE_EVAL_DU | QAWS_SURFACE_EVAL_DV \
	| QAWS_SURFACE_EVAL_NORMAL | QAWS_SURFACE_EVAL_DUU | QAWS_SURFACE_EVAL_DUV | QAWS_SURFACE_EVAL_DVV)

static double vdot(qaws_vec3 a, qaws_vec3 b)
{
	return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z;
}

static qaws_vec3 vsub(qaws_vec3 a, qaws_vec3 b)
{
	qaws_vec3 r = { a.x - b.x, a.y - b.y, a.z - b.z };
	return r;
}

static double vlen(qaws_vec3 a)
{
	return sqrt(vdot(a, a));
}

/* the angle between two unit vectors, from their chord */
static double vangle(qaws_vec3 a, qaws_vec3 b)
{
	double ch = vlen(vsub(a, b)) * 0.5;
	if (ch > 1.0) ch = 1.0;
	return 2.0 * asin(ch);
}

static qaws_curve* make_bezier3(qaws_vec3 const* p, unsigned n)
{
	qaws_bezier_desc d;
	qaws_curve* c = NULL;
	memset(&d, 0, sizeof d);
	d.dimension = QAWS_DIMENSION_3D;
	d.degree = n - 1;
	d.control_points = p;
	d.control_point_count = n;
	if (qaws_curve_create_bezier(&c, &d) != QAWS_STATUS_OK)
		return NULL;
	return c;
}

/* the baseline narrow rail: degree-3 B-spline, 9 points, a turn compressed into spans around 0.5 */
static qaws_curve* make_narrow_bspline(void)
{
	qaws_vec3 cp[9] = { {-2,0,0},{-1,0,0},{-.3f,.2f,.1f},{0,1,0},{1,1,1},{1,-1,1},{0,-1,0},{.3f,-.2f,-.1f},{2,0,0} };
	qaws_scalar knots[13] = { 0,0,0,0,.49998f,.49999f,.5f,.50001f,.50002f,1,1,1,1 };
	qaws_bspline_desc d;
	qaws_curve* c = NULL;
	memset(&d, 0, sizeof d);
	d.dimension = QAWS_DIMENSION_3D;
	d.degree = 3;
	d.control_points = cp;
	d.control_point_count = 9;
	d.knots = knots;
	d.knot_count = 13;
	if (qaws_curve_create_bspline(&c, &d) != QAWS_STATUS_OK)
		return NULL;
	return c;
}

/* a moderately narrow rail for the derivative checks: one span of width 0.02, so a finite-difference step stays
   well inside one span while the subdivision still has to adapt to a span ~8x narrower than the others */
static qaws_curve* make_diff_bspline(void)
{
	qaws_vec3 cp[8] = { {0,0,0},{1,1,.4f},{2,-1,1},{2.5f,.5f,-.5f},{3,1,1},{4,-.5f,.3f},{5,.8f,-.6f},{6,0,0} };
	qaws_scalar knots[12] = { 0,0,0,0,.3f,.32f,.7f,1,1,1,1,1 };
	qaws_bspline_desc d;
	qaws_curve* c = NULL;
	memset(&d, 0, sizeof d);
	d.dimension = QAWS_DIMENSION_3D;
	d.degree = 3;
	d.control_points = cp;
	d.control_point_count = 8;
	d.knots = knots;
	d.knot_count = 12;
	if (qaws_curve_create_bspline(&c, &d) != QAWS_STATUS_OK)
		return NULL;
	return c;
}

/* a smooth uniform-knot rail: no pathological geometry, but the field still spans several cache intervals */
static qaws_curve* make_smooth_bspline(void)
{
	qaws_vec3 cp[9] = { {0,0,0},{1,.8f,.3f},{2,-.6f,1},{3,.4f,-.5f},{4,1,.8f},{5,-.7f,.2f},{6,.5f,-.8f},{7,.9f,.4f},{8,0,0} };
	qaws_bspline_desc d;
	qaws_curve* c = NULL;
	memset(&d, 0, sizeof d);
	d.dimension = QAWS_DIMENSION_3D;
	d.degree = 3;
	d.control_points = cp;
	d.control_point_count = 9;
	d.is_uniform = 1;
	if (qaws_curve_create_bspline(&c, &d) != QAWS_STATUS_OK)
		return NULL;
	return c;
}

/* a NURBS rail on the non-unit domain [lo, hi], positive weights, two spans (one narrow: [lo, mid]) */
static qaws_curve* make_nurbs(qaws_scalar lo, qaws_scalar hi, qaws_scalar mid, qaws_scalar knots_out[9])
{
	qaws_vec3 cp[5] = { {0,0,0},{1,1.5f,0.4f},{2,-1,1},{3,1,-0.6f},{4,0,0.5f} };
	qaws_scalar w[5] = { 1,1.4f,0.7f,1.2f,1 };
	qaws_scalar knots[9];
	qaws_nurbs_desc d;
	qaws_curve* c = NULL;
	/* degree 3, 5 points: the valid domain is [knots[3], knots[5]]; clamp both ends and put one interior knot */
	knots[0] = knots[1] = knots[2] = knots[3] = lo;
	knots[4] = mid;
	knots[5] = knots[6] = knots[7] = knots[8] = hi;
	memcpy(knots_out, knots, sizeof knots);
	memset(&d, 0, sizeof d);
	d.dimension = QAWS_DIMENSION_3D;
	d.degree = 3;
	d.control_points = cp;
	d.control_point_count = 5;
	d.knots = knots;
	d.knot_count = 9;
	d.weights = w;
	d.weight_count = 5;
	if (qaws_curve_create_nurbs(&c, &d) != QAWS_STATUS_OK)
		return NULL;
	return c;
}

/* a curved 2D profile (degree-2 Bezier) so both dv and a nonzero dvv are meaningful */
static qaws_curve* make_profile(void)
{
	qaws_vec2 p[3] = { {1,0},{1.3f,.6f},{.7f,1} };
	qaws_bezier_desc d;
	qaws_curve* c = NULL;
	memset(&d, 0, sizeof d);
	d.dimension = QAWS_DIMENSION_2D;
	d.degree = 2;
	d.control_points = p;
	d.control_point_count = 3;
	if (qaws_curve_create_bezier(&c, &d) != QAWS_STATUS_OK)
		return NULL;
	return c;
}

/* the distinct knot values of a clamped knot vector */
static unsigned unique_knots(qaws_scalar const* knots, unsigned knot_count, qaws_scalar* out)
{
	unsigned i, n = 0;
	for (i = 0; i < knot_count; i++)
		if (n == 0 || knots[i] != out[n - 1])
			out[n++] = knots[i];
	return n;
}

/* a strictly increasing per-span dense parameter list over the given span boundaries;
   the rows of *out_frames are (T, N, B) as qaws_curve_compute_rotation_minimizing_frames_3d returns */
static int dense_reference(qaws_curve const* path, qaws_scalar const* bounds, unsigned span_count,
	qaws_vec3 const* n0, qaws_scalar** out_params, qaws_scalar** out_frames, unsigned* out_count)
{
	unsigned count = 1 + span_count * REF_STEPS, k, s;
	qaws_scalar* params = (qaws_scalar*)malloc((size_t)count * sizeof(qaws_scalar));
	qaws_scalar* frames = (qaws_scalar*)malloc((size_t)count * 9 * sizeof(qaws_scalar));
	if (!params || !frames)
	{
		free(params); free(frames);
		return 1;
	}
	k = 0;
	params[k++] = bounds[0];
	for (s = 0; s < span_count; s++)
	{
		unsigned j;
		for (j = 1; j <= REF_STEPS; j++)
			params[k++] = bounds[s] + (bounds[s + 1] - bounds[s]) * (qaws_scalar)j / (qaws_scalar)REF_STEPS;
	}
	if (k != count) { free(params); free(frames); return 1; }
	if (qaws_curve_compute_rotation_minimizing_frames_3d(frames, path, params, n0, count) != QAWS_STATUS_OK)
	{
		free(params); free(frames);
		return 1;
	}
	*out_params = params;
	*out_frames = frames;
	*out_count = count;
	return 0;
}

/* the surface section normal N(u) of a pipe (v = 0 puts the radius on N) or of the unit profile swept surface */
static int section_normal(qaws_surface const* s, qaws_curve const* path, qaws_scalar u, qaws_scalar radius, qaws_vec3* out)
{
	qaws_surface_eval_result r;
	qaws_eval_result_3d c;
	qaws_range pr = qaws_curve_get_parameter_range(path);
	if (qaws_surface_evaluate(&r, s, u, 0, QAWS_SURFACE_EVAL_POSITION) != QAWS_STATUS_OK)
		return 1;
	if (qaws_curve_evaluate_3d(&c, path, pr.min_value + u * (pr.max_value - pr.min_value), QAWS_EVAL_FLAG_POSITION) != QAWS_STATUS_OK)
		return 1;
	out->x = (r.position.x - c.position.x) / radius;
	out->y = (r.position.y - c.position.y) / radius;
	out->z = (r.position.z - c.position.z) / radius;
	return 0;
}

static int eval_at(qaws_surface const* s, double u, double v, unsigned flags, qaws_surface_eval_result* out)
{
	return qaws_surface_evaluate(out, s, (qaws_scalar)u, (qaws_scalar)v, flags) == QAWS_STATUS_OK ? 0 : 1;
}

static double l1diff(qaws_vec3 a, qaws_vec3 b)
{
	return fabs((double)a.x - b.x) + fabs((double)a.y - b.y) + fabs((double)a.z - b.z);
}

/* largest angle between the surface section normals and the dense reference, at grid points the reference contains */
static int transport_error(qaws_surface const* s, qaws_curve const* path, qaws_scalar radius,
	qaws_scalar const* bounds, unsigned span_count, qaws_scalar const* frames, double* out_worst)
{
	qaws_range pr = qaws_curve_get_parameter_range(path);
	double worst = 0;
	unsigned sp, j;
	for (sp = 0; sp < span_count; sp++)
	{
		for (j = 0; j <= REF_STEPS; j += (REF_STEPS >= 2048 ? 127u : 63u))
		{
			qaws_scalar t = bounds[sp] + (bounds[sp + 1] - bounds[sp]) * (qaws_scalar)j / (qaws_scalar)REF_STEPS;
			qaws_scalar u = (t - pr.min_value) / (pr.max_value - pr.min_value);
			unsigned row = sp * REF_STEPS + j;
			qaws_vec3 got, ref = { frames[9 * row + 3], frames[9 * row + 4], frames[9 * row + 5] };
			if (section_normal(s, path, u, radius, &got))
				return 1;
			{
				double e = vangle(got, ref);
				if (e > worst) worst = e;
			}
		}
	}
	*out_worst = worst;
	return 0;
}

/* ------------------------------------------------------------ span_transport */

static int case_narrow_pipe(void)
{
	qaws_curve* path = make_narrow_bspline();
	qaws_surface_pipe_desc pd = { path, (qaws_scalar)0.6, (qaws_scalar)0.6 };
	qaws_surface* pipe = NULL;
	qaws_scalar bounds[7] = { 0,.49998f,.49999f,.5f,.50001f,.50002f,1 };
	qaws_scalar *params, *frames;
	unsigned count;
	qaws_vec3 n0;
	double err_adaptive, err_legacy;
	TASK_CHECK(path != NULL);
	TASK_CHECK(qaws_surface_create_pipe(&pipe, &pd) == QAWS_STATUS_OK);
	/* pipes leave creation on the adaptive 1e-5 field */
	TASK_CHECK(section_normal(pipe, path, 0, (qaws_scalar)0.6, &n0) == 0);
	TASK_CHECK(dense_reference(path, bounds, 6, &n0, &params, &frames, &count) == 0);
	TASK_CHECK(transport_error(pipe, path, (qaws_scalar)0.6, bounds, 6, frames, &err_adaptive) == 0);
	/* the legacy fixed table skips the turn in the narrow spans (the baseline measured ~0.088 rad) */
	TASK_CHECK(qaws_surface_pipe_set_rmf(pipe, NULL, 0) == QAWS_STATUS_OK);
	TASK_CHECK(transport_error(pipe, path, (qaws_scalar)0.6, bounds, 6, frames, &err_legacy) == 0);
	TASK_CHECK(err_legacy > 0.01);
	/* f64 resolves the 1e-5 spans and meets the 1e-5 target; f32's parameter resolution bottoms out earlier, but its
	   adaptive field still beats the fixed table decisively */
	if (IS_F32)
	{
		TASK_CHECK(err_adaptive <= fmax(1e-1, NUM_FLOOR));
		TASK_CHECK(err_adaptive < err_legacy / 2.0);
	}
	else
	{
		TASK_CHECK(err_adaptive <= fmax(1e-5, NUM_FLOOR));
		TASK_CHECK(err_adaptive < err_legacy / 10.0);
	}
	free(params);
	free(frames);
	qaws_surface_destroy(pipe);
	qaws_curve_destroy(path);
	return 0;
}

static int case_inflection_and_line(void)
{
	/* planar cubic with a curvature zero at u = 0.5 (the baseline rail) */
	qaws_vec3 cz[4] = { {0,-.125f,0},{1.f/3,.125f,0},{2.f/3,-.125f,0},{1,.125f,0} };
	qaws_curve* bend = make_bezier3(cz, 4);
	qaws_curve* prof = make_profile();
	qaws_surface_swept_desc sd;
	qaws_surface* sweep = NULL;
	qaws_scalar bounds[2] = { 0,1 }, *params, *frames;
	unsigned count, i;
	qaws_vec3 n0;
	double worst;
	TASK_CHECK(bend && prof);
	memset(&sd, 0, sizeof sd);
	sd.path = bend; sd.profile = prof;
	TASK_CHECK(qaws_surface_create_swept(&sweep, &sd) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(sweep, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	TASK_CHECK(section_normal(sweep, bend, 0, 1, &n0) == 0);
	TASK_CHECK(dense_reference(bend, bounds, 1, &n0, &params, &frames, &count) == 0);
	TASK_CHECK(transport_error(sweep, bend, 1, bounds, 1, frames, &worst) == 0);
	TASK_CHECK(worst <= fmax(1e-6, NUM_FLOOR));
	free(params); free(frames);
	/* continuity through the inflection and the cache nodes: the normal turns smoothly with the tangent,
	   so a jump shows as a spike in the second difference, not in the (legitimate) first difference */
	for (i = 1; i < 40; i++)
	{
		qaws_scalar u = (qaws_scalar)i / 40, h = (qaws_scalar)1e-4;
		qaws_vec3 a, b, c;
		double d2;
		TASK_CHECK(section_normal(sweep, bend, u - h, 1, &a) == 0);
		TASK_CHECK(section_normal(sweep, bend, u, 1, &b) == 0);
		TASK_CHECK(section_normal(sweep, bend, u + h, 1, &c) == 0);
		d2 = vlen(vsub(vsub(c, b), vsub(b, a)));   /* N(u+h) - 2 N(u) + N(u-h) */
		TASK_CHECK(d2 < (IS_F32 ? 1e-3 : 1e-5));
	}
	qaws_surface_destroy(sweep);
	qaws_curve_destroy(prof);
	qaws_curve_destroy(bend);

	/* a straight rail: one frame all along */
	{
		qaws_vec3 lp[2] = { {0,0,0},{1,2,3} };
		qaws_curve* line = make_bezier3(lp, 2);
		qaws_curve* pr = make_profile();
		qaws_surface_swept_desc d2;
		qaws_surface* sw = NULL;
		qaws_scalar b2[2] = { 0,1 }, *pp, *ff;
		unsigned nc, k;
		qaws_vec3 first;
		double w = 0;
		TASK_CHECK(line && pr);
		memset(&d2, 0, sizeof d2);
		d2.path = line; d2.profile = pr;
		TASK_CHECK(qaws_surface_create_swept(&sw, &d2) == QAWS_STATUS_OK);
		TASK_CHECK(qaws_surface_swept_set_rmf(sw, NULL, (qaws_scalar)1e-5) == QAWS_STATUS_OK);
		TASK_CHECK(section_normal(sw, line, 0, 1, &first) == 0);
		TASK_CHECK(dense_reference(line, b2, 1, &first, &pp, &ff, &nc) == 0);
		for (k = 1; k < nc; k += 37)
		{
			qaws_vec3 got, ref = { ff[9 * k + 3], ff[9 * k + 4], ff[9 * k + 5] };
			TASK_CHECK(section_normal(sw, line, (qaws_scalar)k / (nc - 1), 1, &got) == 0);
			{
				double e = vangle(got, ref);
				if (e > w) w = e;
			}
		}
		TASK_CHECK(w <= fmax(1e-6, NUM_FLOOR));
		free(pp); free(ff);
		qaws_surface_destroy(sw);
		qaws_curve_destroy(pr);
		qaws_curve_destroy(line);
	}
	return 0;
}

static int case_nurbs_transport(void)
{
	qaws_scalar knots[9], bounds[9];
	unsigned span_count;
	qaws_curve* path = make_nurbs(3, 7, 4, knots);
	qaws_surface_pipe_desc pd = { path, (qaws_scalar)0.4, (qaws_scalar)0.3 };
	qaws_surface* pipe = NULL;
	qaws_scalar *params, *frames;
	unsigned count;
	qaws_vec3 n0;
	double err;
	TASK_CHECK(path != NULL);
	span_count = unique_knots(knots, 9, bounds) - 1;
	TASK_CHECK(span_count >= 2);
	TASK_CHECK(qaws_surface_create_pipe(&pipe, &pd) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_pipe_set_rmf(pipe, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	TASK_CHECK(section_normal(pipe, path, 0, (qaws_scalar)0.4, &n0) == 0);
	TASK_CHECK(dense_reference(path, bounds, span_count, &n0, &params, &frames, &count) == 0);
	TASK_CHECK(transport_error(pipe, path, (qaws_scalar)0.4, bounds, span_count, frames, &err) == 0);
	TASK_CHECK(err <= fmax(1e-6, NUM_FLOOR));
	free(params); free(frames);
	qaws_surface_destroy(pipe);
	qaws_curve_destroy(path);
	return 0;
}

/* ------------------------------------------------------------ surface_differentials */

static int case_swept_differentials(void)
{
	qaws_curve* path = make_diff_bspline();
	qaws_curve* prof = make_profile();
	qaws_surface_swept_desc sd;
	qaws_surface* s = NULL;
	/* the half-step stays inside the 0.02-wide narrow span even for a grid point at its middle */
	double h = IS_F32 ? 0.004 : 1e-4;
	unsigned i, j;
	TASK_CHECK(path && prof);
	memset(&sd, 0, sizeof sd);
	sd.path = path; sd.profile = prof;
	TASK_CHECK(qaws_surface_create_swept(&s, &sd) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(s, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	for (i = 0; i < 11; i++)
		for (j = 0; j < 5; j++)
		{
			double u = (i + 0.5) / 11, v = 0.17 * j + 0.05;
			qaws_surface_eval_result r, tmp;
			qaws_vec3 pa, pb, pah, pbh, va, vb, qdu, qdv, qduu, qdvv, qduv, nn;
			double rel;
			TASK_CHECK(eval_at(s, u, v, ALL_FLAGS, &r) == 0);
			/* Richardson central differences of the positions: all from position queries alone */
			TASK_CHECK(eval_at(s, u + h, v, QAWS_SURFACE_EVAL_POSITION, &tmp) == 0); pa = tmp.position;
			TASK_CHECK(eval_at(s, u - h, v, QAWS_SURFACE_EVAL_POSITION, &tmp) == 0); pb = tmp.position;
			TASK_CHECK(eval_at(s, u + h / 2, v, QAWS_SURFACE_EVAL_POSITION, &tmp) == 0); pah = tmp.position;
			TASK_CHECK(eval_at(s, u - h / 2, v, QAWS_SURFACE_EVAL_POSITION, &tmp) == 0); pbh = tmp.position;
			qdu.x = (qaws_scalar)((4 * (pah.x - pbh.x) / h - (pa.x - pb.x) / (2 * h)) / 3);
			qdu.y = (qaws_scalar)((4 * (pah.y - pbh.y) / h - (pa.y - pb.y) / (2 * h)) / 3);
			qdu.z = (qaws_scalar)((4 * (pah.z - pbh.z) / h - (pa.z - pb.z) / (2 * h)) / 3);
			qduu.x = (qaws_scalar)((4 * (pah.x - 2 * r.position.x + pbh.x) / (h * h / 4)
				- (pa.x - 2 * r.position.x + pb.x) / (h * h)) / 3);
			qduu.y = (qaws_scalar)((4 * (pah.y - 2 * r.position.y + pbh.y) / (h * h / 4)
				- (pa.y - 2 * r.position.y + pb.y) / (h * h)) / 3);
			qduu.z = (qaws_scalar)((4 * (pah.z - 2 * r.position.z + pbh.z) / (h * h / 4)
				- (pa.z - 2 * r.position.z + pb.z) / (h * h)) / 3);
			TASK_CHECK(eval_at(s, u, v + h, QAWS_SURFACE_EVAL_POSITION, &tmp) == 0); pa = tmp.position;
			TASK_CHECK(eval_at(s, u, v - h, QAWS_SURFACE_EVAL_POSITION, &tmp) == 0); pb = tmp.position;
			TASK_CHECK(eval_at(s, u, v + h / 2, QAWS_SURFACE_EVAL_POSITION, &tmp) == 0); pah = tmp.position;
			TASK_CHECK(eval_at(s, u, v - h / 2, QAWS_SURFACE_EVAL_POSITION, &tmp) == 0); pbh = tmp.position;
			qdv.x = (qaws_scalar)((4 * (pah.x - pbh.x) / h - (pa.x - pb.x) / (2 * h)) / 3);
			qdv.y = (qaws_scalar)((4 * (pah.y - pbh.y) / h - (pa.y - pb.y) / (2 * h)) / 3);
			qdv.z = (qaws_scalar)((4 * (pah.z - pbh.z) / h - (pa.z - pb.z) / (2 * h)) / 3);
			qdvv.x = (qaws_scalar)((4 * (pah.x - 2 * r.position.x + pbh.x) / (h * h / 4)
				- (pa.x - 2 * r.position.x + pb.x) / (h * h)) / 3);
			qdvv.y = (qaws_scalar)((4 * (pah.y - 2 * r.position.y + pbh.y) / (h * h / 4)
				- (pa.y - 2 * r.position.y + pb.y) / (h * h)) / 3);
			qdvv.z = (qaws_scalar)((4 * (pah.z - 2 * r.position.z + pbh.z) / (h * h / 4)
				- (pa.z - 2 * r.position.z + pb.z) / (h * h)) / 3);
			/* duv from dv differences in u */
			TASK_CHECK(eval_at(s, u + h, v, QAWS_SURFACE_EVAL_DV, &tmp) == 0); va = tmp.dv;
			TASK_CHECK(eval_at(s, u - h, v, QAWS_SURFACE_EVAL_DV, &tmp) == 0); vb = tmp.dv;
			qduv.x = (va.x - vb.x) / (2 * h);
			qduv.y = (va.y - vb.y) / (2 * h);
			qduv.z = (va.z - vb.z) / (2 * h);

			rel = 1.0 + fabs(qdu.x) + fabs(qdu.y) + fabs(qdu.z);
			TASK_CHECK(l1diff(r.du, qdu) / rel < (IS_F32 ? 2e-2 : 1e-6));
			rel = 1.0 + fabs(qdv.x) + fabs(qdv.y) + fabs(qdv.z);
			TASK_CHECK(l1diff(r.dv, qdv) / rel < (IS_F32 ? 2e-3 : 1e-6));
			/* second differences divide by h^2, so in float they are rounding dominated on a high-curvature rail:
			   the second derivatives are checked against the position differences in double only; in float the
			   analytic derivatives are still verified through the self-consistent normal below */
			if (!IS_F32)
			{
				rel = 1.0 + fabs(qduu.x) + fabs(qduu.y) + fabs(qduu.z);
				TASK_CHECK(l1diff(r.duu, qduu) / rel < 1e-4);
				rel = 1.0 + fabs(qdvv.x) + fabs(qdvv.y) + fabs(qdvv.z);
				TASK_CHECK(l1diff(r.dvv, qdvv) / rel < 1e-4);
				rel = 1.0 + fabs(qduv.x) + fabs(qduv.y) + fabs(qduv.z);
				TASK_CHECK(l1diff(r.duv, qduv) / rel < 1e-4);
			}
			/* the normal is the unit cross of the same field's analytic derivatives */
			TASK_CHECK(fabs(vlen(r.normal) - 1) < (IS_F32 ? 1e-5 : 1e-9));
			TASK_CHECK(fabs(vdot(r.normal, r.du)) < (IS_F32 ? 1e-4 : 1e-7));
			TASK_CHECK(fabs(vdot(r.normal, r.dv)) < (IS_F32 ? 1e-4 : 1e-7));
			nn.x = (qaws_scalar)(qdu.y * qdv.z - qdu.z * qdv.y);
			nn.y = (qaws_scalar)(qdu.z * qdv.x - qdu.x * qdv.z);
			nn.z = (qaws_scalar)(qdu.x * qdv.y - qdu.y * qdv.x);
			{
				double l = vlen(nn);
				if (l > 0) { nn.x = (qaws_scalar)(nn.x / l); nn.y = (qaws_scalar)(nn.y / l); nn.z = (qaws_scalar)(nn.z / l); }
			}
			TASK_CHECK(vangle(r.normal, nn) < (IS_F32 ? 2e-2 : 1e-6));
		}
	/* the field tracks the independent dense reference over the diff rail, including its narrow span */
	{
		qaws_scalar knots[12] = { 0,0,0,0,.3f,.32f,.7f,1,1,1,1,1 }, bounds[12];
		unsigned span_count = unique_knots(knots, 12, bounds) - 1;
		qaws_scalar *params, *frames;
		unsigned count;
		double ref_err;
		qaws_vec3 n0;
		TASK_CHECK(section_normal(s, path, 0, 1, &n0) == 0);
		TASK_CHECK(dense_reference(path, bounds, span_count, &n0, &params, &frames, &count) == 0);
		TASK_CHECK(transport_error(s, path, 1, bounds, span_count, frames, &ref_err) == 0);
		TASK_CHECK(ref_err <= fmax(1e-6, NUM_FLOOR));
		free(params); free(frames);
	}
	/* no jump across the cache intervals on a smooth rail: the field agrees with the independent dense reference at
	   points inside every span and on both sides of every knot boundary, so a cache node cannot introduce a jump */
	{
		qaws_curve* smooth = make_smooth_bspline();
		qaws_surface_swept_desc smd;
		qaws_surface* sm = NULL;
		qaws_scalar bounds[7];
		unsigned span_count;
		qaws_scalar *params, *frames;
		unsigned count;
		double ref_err;
		qaws_vec3 n0;
		TASK_CHECK(smooth != NULL);
		memset(&smd, 0, sizeof smd);
		smd.path = smooth; smd.profile = prof;
		TASK_CHECK(qaws_surface_create_swept(&sm, &smd) == QAWS_STATUS_OK);
		TASK_CHECK(qaws_surface_swept_set_rmf(sm, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
		/* the uniform rail's spans subdivide its own (non-unit) parameter range */
		span_count = qaws_curve_get_span_count(smooth);
		TASK_CHECK(span_count == 6);
		{
			qaws_range pr2 = qaws_curve_get_parameter_range(smooth);
			unsigned k2;
			for (k2 = 0; k2 <= span_count; k2++)
				bounds[k2] = pr2.min_value + (pr2.max_value - pr2.min_value) * (qaws_scalar)k2 / span_count;
		}
		TASK_CHECK(section_normal(sm, smooth, 0, 1, &n0) == 0);
		TASK_CHECK(dense_reference(smooth, bounds, span_count, &n0, &params, &frames, &count) == 0);
		TASK_CHECK(transport_error(sm, smooth, 1, bounds, span_count, frames, &ref_err) == 0);
		TASK_CHECK(ref_err <= fmax(1e-6, NUM_FLOOR));
		free(params); free(frames);
		qaws_surface_destroy(sm);
		qaws_curve_destroy(smooth);
	}
	qaws_surface_destroy(s);
	qaws_curve_destroy(prof);
	qaws_curve_destroy(path);
	return 0;
}

static int case_chain_scale(void)
{
	/* non-unit source domain [3, 7]: the surface's normalized u must carry the x4 chain scale into du */
	qaws_scalar knots[9];
	qaws_curve* path = make_nurbs(3, 7, 4, knots);
	qaws_curve* prof = make_profile();
	qaws_surface_swept_desc sd;
	qaws_surface* s = NULL;
	double h = IS_F32 ? 0.004 : 1e-3, u = 0.37, v = 0.31;
	qaws_surface_eval_result r, tmp;
	qaws_vec3 pa, pb, pah, pbh, qdu;
	double rel;
	qaws_range pr;
	TASK_CHECK(path && prof);
	memset(&sd, 0, sizeof sd);
	sd.path = path; sd.profile = prof;
	TASK_CHECK(qaws_surface_create_swept(&s, &sd) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(s, NULL, (qaws_scalar)1e-5) == QAWS_STATUS_OK);
	TASK_CHECK(eval_at(s, u, v, QAWS_SURFACE_EVAL_DU, &r) == 0);
	TASK_CHECK(eval_at(s, u + h, v, QAWS_SURFACE_EVAL_POSITION, &tmp) == 0); pa = tmp.position;
	TASK_CHECK(eval_at(s, u - h, v, QAWS_SURFACE_EVAL_POSITION, &tmp) == 0); pb = tmp.position;
	TASK_CHECK(eval_at(s, u + h / 2, v, QAWS_SURFACE_EVAL_POSITION, &tmp) == 0); pah = tmp.position;
	TASK_CHECK(eval_at(s, u - h / 2, v, QAWS_SURFACE_EVAL_POSITION, &tmp) == 0); pbh = tmp.position;
	qdu.x = (qaws_scalar)((4 * (pah.x - pbh.x) / h - (pa.x - pb.x) / (2 * h)) / 3);
	qdu.y = (qaws_scalar)((4 * (pah.y - pbh.y) / h - (pa.y - pb.y) / (2 * h)) / 3);
	qdu.z = (qaws_scalar)((4 * (pah.z - pbh.z) / h - (pa.z - pb.z) / (2 * h)) / 3);
	rel = 1.0 + fabs(qdu.x) + fabs(qdu.y) + fabs(qdu.z);
	TASK_CHECK(l1diff(r.du, qdu) / rel < (IS_F32 ? 2e-3 : 1e-6));
	/* the path itself still answers in its own [3, 7] parameter interval */
	pr = qaws_curve_get_parameter_range(path);
	TASK_CHECK(fabs(pr.min_value - 3) < 1e-12 && fabs(pr.max_value - 7) < 1e-12);
	qaws_surface_destroy(s);
	qaws_curve_destroy(prof);
	qaws_curve_destroy(path);
	return 0;
}

/* ------------------------------------------------------------ mode_allocator */

typedef struct acct
{
	unsigned long allocs;
	unsigned long deallocs;
} acct;

static void* acct_alloc(unsigned long size, void* user)
{
	acct* a = (acct*)user;
	a->allocs++;
	return malloc(size);
}

static void acct_dealloc(void* ptr, void* user)
{
	acct* a = (acct*)user;
	if (ptr) a->deallocs++;
	free(ptr);
}

static int case_mode_lifecycle(void)
{
	acct a = { 0, 0 };
	qaws_allocator al;
	qaws_curve* path = make_narrow_bspline();
	qaws_curve* prof = make_profile();
	qaws_surface_pipe_desc pd = { path, (qaws_scalar)0.5, 0 };
	qaws_surface_swept_desc sd;
	qaws_surface *pipe = NULL, *sweep = NULL;
	qaws_surface_eval_result r;
	TASK_CHECK(path && prof);
	memset(&al, 0, sizeof al);
	al.alloc = acct_alloc;
	al.dealloc = acct_dealloc;
	al.user_data = &a;
	memset(&sd, 0, sizeof sd);
	sd.path = path; sd.profile = prof;

	TASK_CHECK(qaws_surface_create_pipe_ex(&pipe, &pd, &al) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_create_swept_ex(&sweep, &sd, &al) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_get_kind(pipe) == QAWS_SURFACE_KIND_PIPE);
	TASK_CHECK(qaws_surface_get_kind(sweep) == QAWS_SURFACE_KIND_SWEPT);
	TASK_CHECK(a.allocs > 0);

	/* enable, rebuild, disable, rebuild again; the handle, kind and borrowed curves never move */
	TASK_CHECK(qaws_surface_pipe_set_rmf(pipe, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_pipe_set_rmf(pipe, NULL, (qaws_scalar)1e-4) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_pipe_set_rmf(pipe, NULL, 0) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_pipe_set_rmf(pipe, NULL, (qaws_scalar)1e-5) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(sweep, NULL, (qaws_scalar)1e-5) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(sweep, NULL, (qaws_scalar)1e-7) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(sweep, NULL, 0) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(sweep, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_evaluate(&r, pipe, .41f, .27f, QAWS_SURFACE_EVAL_POSITION) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_evaluate(&r, sweep, .41f, .27f, QAWS_SURFACE_EVAL_POSITION) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_get_kind(pipe) == QAWS_SURFACE_KIND_PIPE);
	TASK_CHECK(qaws_surface_get_kind(sweep) == QAWS_SURFACE_KIND_SWEPT);

	qaws_surface_destroy(pipe);
	qaws_surface_destroy(sweep);
	/* every block the surfaces took came back through the same allocator */
	TASK_CHECK(a.allocs == a.deallocs);
	qaws_curve_destroy(prof);
	qaws_curve_destroy(path);
	return 0;
}

static int case_rejected_keeps_mode(void)
{
	qaws_curve* path = make_narrow_bspline();
	qaws_curve* prof = make_profile();
	qaws_surface_swept_desc sd;
	qaws_surface* sweep = NULL;
	qaws_surface_pipe_desc pd = { path, (qaws_scalar)0.5, 0 };
	qaws_surface* pipe = NULL;
	qaws_eval_result_3d c0;
	qaws_vec3 tangent, bad_zero = { 0, 0, 0 }, probe[5];
	unsigned i;
	qaws_scalar const bad_tol[4] = { (qaws_scalar)1e-2, (qaws_scalar)1e-8, -(qaws_scalar)1e-5, (qaws_scalar)INFINITY };
	TASK_CHECK(path && prof);
	memset(&sd, 0, sizeof sd);
	sd.path = path; sd.profile = prof;
	TASK_CHECK(qaws_surface_create_swept(&sweep, &sd) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_create_pipe(&pipe, &pd) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(sweep, NULL, (qaws_scalar)1e-5) == QAWS_STATUS_OK);

	TASK_CHECK(qaws_curve_evaluate_3d(&c0, path, 0, QAWS_EVAL_FLAG_D1) == QAWS_STATUS_OK);
	tangent = c0.d1;
	{
		double l = vlen(tangent);
		tangent.x = (qaws_scalar)(tangent.x / l); tangent.y = (qaws_scalar)(tangent.y / l); tangent.z = (qaws_scalar)(tangent.z / l);
	}
	for (i = 0; i < 4; i++)
		TASK_CHECK(qaws_surface_swept_set_rmf(sweep, NULL, bad_tol[i]) != QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(sweep, NULL, (qaws_scalar)NAN) != QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_pipe_set_rmf(pipe, NULL, (qaws_scalar)NAN) != QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(sweep, &bad_zero, (qaws_scalar)1e-5) != QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(sweep, &tangent, (qaws_scalar)1e-5) != QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_pipe_set_rmf(pipe, &bad_zero, (qaws_scalar)1e-5) != QAWS_STATUS_OK);
	/* wrong surface kind is refused too */
	TASK_CHECK(qaws_surface_pipe_set_rmf(sweep, NULL, (qaws_scalar)1e-5) != QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(pipe, NULL, (qaws_scalar)1e-5) != QAWS_STATUS_OK);

	/* the current mode survived: identical answers at the same parameters in any query order */
	for (i = 0; i < 5; i++)
	{
		qaws_surface_eval_result r;
		TASK_CHECK(eval_at(sweep, (i + 0.5) / 5, .3, QAWS_SURFACE_EVAL_POSITION, &r) == 0);
		probe[i] = r.position;
	}
	for (i = 0; i < 5; i++)
	{
		qaws_surface_eval_result r;
		TASK_CHECK(eval_at(sweep, (4 - i + 0.5) / 5, .3, QAWS_SURFACE_EVAL_POSITION, &r) == 0);
	}
	for (i = 0; i < 5; i++)
	{
		qaws_surface_eval_result r;
		TASK_CHECK(eval_at(sweep, (i + 0.5) / 5, .3, QAWS_SURFACE_EVAL_POSITION, &r) == 0);
		TASK_CHECK(r.position.x == probe[i].x && r.position.y == probe[i].y && r.position.z == probe[i].z);
	}
	/* tolerance 0 still switches both surfaces back to their original behavior */
	TASK_CHECK(qaws_surface_swept_set_rmf(sweep, NULL, 0) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_pipe_set_rmf(pipe, NULL, 0) == QAWS_STATUS_OK);
	{
		qaws_surface_eval_result r;
		TASK_CHECK(qaws_surface_evaluate(&r, sweep, .2f, .2f, QAWS_SURFACE_EVAL_POSITION) == QAWS_STATUS_OK);
		TASK_CHECK(r.valid_flags & QAWS_SURFACE_EVAL_POSITION);
		TASK_CHECK(qaws_surface_evaluate(&r, pipe, .2f, .2f, QAWS_SURFACE_EVAL_POSITION) == QAWS_STATUS_OK);
		TASK_CHECK(r.valid_flags & QAWS_SURFACE_EVAL_POSITION);
	}

	/* a zero-speed rail: the build is refused and the surface stays on its previous mode */
	{
		qaws_vec3 zp[4] = { {0,0,0},{0,0,0},{1,1,0},{2,0,0} };
		qaws_curve* zpath = make_bezier3(zp, 4);
		qaws_curve* zprof = make_profile();
		qaws_surface_swept_desc zd;
		qaws_surface* zs = NULL;
		qaws_status st;
		TASK_CHECK(zpath && zprof);
		memset(&zd, 0, sizeof zd);
		zd.path = zpath; zd.profile = zprof;
		TASK_CHECK(qaws_surface_create_swept(&zs, &zd) == QAWS_STATUS_OK);
		st = qaws_surface_swept_set_rmf(zs, NULL, (qaws_scalar)1e-5);
		TASK_CHECK(st == QAWS_STATUS_DEGENERATE_CURVE || st == QAWS_STATUS_NUMERICAL_FAILURE);
		{
			qaws_surface_eval_result r;
			TASK_CHECK(qaws_surface_evaluate(&r, zs, .8f, .2f, QAWS_SURFACE_EVAL_POSITION) == QAWS_STATUS_OK);
			TASK_CHECK(r.valid_flags & QAWS_SURFACE_EVAL_POSITION);
		}
		qaws_surface_destroy(zs);
		qaws_curve_destroy(zprof);
		qaws_curve_destroy(zpath);
	}
	qaws_surface_destroy(pipe);
	qaws_surface_destroy(sweep);
	qaws_curve_destroy(prof);
	qaws_curve_destroy(path);
	return 0;
}

/* ------------------------------------------------------------ precision_batch */

static int case_precision_agreement(void)
{
	/* the same rail in both precisions, RMF at 1e-6, compared through the section normal. The narrow span (0.02) is
	   comfortably resolvable in float, so the two precisions' fields must agree to the float floor */
	qaws_vec3_f32 cp32[8] = { {0,0,0},{1,1,.4f},{2,-1,1},{2.5f,.5f,-.5f},{3,1,1},{4,-.5f,.3f},{5,.8f,-.6f},{6,0,0} };
	qaws_f32 k32[12] = { 0,0,0,0,.3f,.32f,.7f,1,1,1,1,1 };
	qaws_bspline_desc_f32 d32;
	qaws_curve_f32* path32 = NULL;
	qaws_surface_pipe_desc_f32 pd32;
	qaws_surface_f32* pipe32 = NULL;
	qaws_curve* path64 = make_diff_bspline();
	qaws_surface_pipe_desc pd64 = { path64, (qaws_scalar)0.5, 0 };
	qaws_surface* pipe64 = NULL;
	unsigned i;
	double worst = 0;
	TASK_CHECK(path64 != NULL);
	memset(&d32, 0, sizeof d32);
	d32.dimension = QAWS_DIMENSION_3D;
	d32.degree = 3;
	d32.control_points = cp32;
	d32.control_point_count = 8;
	d32.knots = k32;
	d32.knot_count = 12;
	TASK_CHECK(qaws_curve_create_bspline_f32(&path32, &d32) == QAWS_STATUS_OK);
	memset(&pd32, 0, sizeof pd32);
	pd32.path = path32;
	pd32.radius_x = 0.5f;
	TASK_CHECK(qaws_surface_create_pipe_f32(&pipe32, &pd32) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_create_pipe(&pipe64, &pd64) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_pipe_set_rmf(pipe64, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_pipe_set_rmf_f32(pipe32, NULL, 1e-6f) == QAWS_STATUS_OK);
	for (i = 0; i < 60; i++)
	{
		qaws_scalar u = (qaws_scalar)(i + 0.5) / 60;
		qaws_vec3 n64;
		qaws_surface_eval_result_f32 r32;
		qaws_eval_result_3d_f32 c32;
		qaws_vec3_f32 n32;
		qaws_range_f32 pr;
		TASK_CHECK(section_normal(pipe64, path64, u, (qaws_scalar)0.5, &n64) == 0);
		TASK_CHECK(qaws_surface_evaluate_f32(&r32, pipe32, (qaws_f32)u, 0, QAWS_SURFACE_EVAL_POSITION) == QAWS_STATUS_OK);
		pr = qaws_curve_get_parameter_range_f32(path32);
		TASK_CHECK(qaws_curve_evaluate_3d_f32(&c32, path32, pr.min_value + (qaws_f32)u * (pr.max_value - pr.min_value),
			QAWS_EVAL_FLAG_POSITION) == QAWS_STATUS_OK);
		n32.x = (r32.position.x - c32.position.x) / 0.5f;
		n32.y = (r32.position.y - c32.position.y) / 0.5f;
		n32.z = (r32.position.z - c32.position.z) / 0.5f;
		{
			qaws_vec3 b = { n32.x, n32.y, n32.z };
			double e = vangle(n64, b);
			if (e > worst) worst = e;
		}
	}
	TASK_CHECK(worst <= 2e-5);
	qaws_surface_destroy_f32(pipe32);
	qaws_curve_destroy_f32(path32);
	qaws_surface_destroy(pipe64);
	qaws_curve_destroy(path64);
	return 0;
}

static int case_tolerance_hierarchy(void)
{
	/* the resolvable diff rail: a span 0.02 wide so both precisions can subdivide it past every target */
	qaws_curve* path = make_diff_bspline();
	qaws_surface_pipe_desc pd = { path, (qaws_scalar)0.5, 0 };
	qaws_surface* pipe = NULL;
	qaws_scalar bounds[5] = { 0, .3f, .32f, .7f, 1 };
	qaws_scalar const tols[3] = { (qaws_scalar)1e-3, (qaws_scalar)1e-5, (qaws_scalar)1e-7 };
	double errors[3];
	unsigned k;
	TASK_CHECK(path != NULL);
	TASK_CHECK(qaws_surface_create_pipe(&pipe, &pd) == QAWS_STATUS_OK);
	for (k = 0; k < 3; k++)
	{
		qaws_scalar *params, *frames;
		unsigned count;
		qaws_vec3 n0;
		TASK_CHECK(qaws_surface_pipe_set_rmf(pipe, NULL, tols[k]) == QAWS_STATUS_OK);
		TASK_CHECK(section_normal(pipe, path, 0, (qaws_scalar)0.5, &n0) == 0);
		TASK_CHECK(dense_reference(path, bounds, 4, &n0, &params, &frames, &count) == 0);
		TASK_CHECK(transport_error(pipe, path, (qaws_scalar)0.5, bounds, 4, frames, &errors[k]) == 0);
		/* the target, or the scalar's own floor where that is larger */
		TASK_CHECK(errors[k] <= fmax((double)tols[k], NUM_FLOOR));
		free(params); free(frames);
	}
	/* every requested target is met (or the scalar's floor, where that is larger). The per-span minimum density keeps
	   the analytic derivatives consistent, so on an easy rail even the loosest target over-delivers; the requirement
	   is therefore that tightening the target never worsens the field, which a coarser-to-finer adaptive build gives */
	TASK_CHECK(errors[1] <= errors[0] + NUM_FLOOR);
	TASK_CHECK(errors[2] <= errors[1] + NUM_FLOOR);
	TASK_CHECK(errors[2] <= fmax(1e-7, NUM_FLOOR));
	qaws_surface_destroy(pipe);
	qaws_curve_destroy(path);
	return 0;
}

static int case_query_order_and_batch(void)
{
	qaws_scalar knots[9];
	qaws_curve* path = make_nurbs(3, 7, 4, knots);
	qaws_curve* prof = make_profile();
	qaws_surface_swept_desc sd;
	qaws_surface* s = NULL;
	enum { NQ = 24 };
	qaws_scalar us[NQ], vs[NQ];
	qaws_surface_eval_result sequential[NQ], shuffled[NQ];
	qaws_vec3 grid[25];
	unsigned perm[NQ] = { 7,0,19,3,11,22,14,1,17,9,5,23,12,4,20,8,2,16,21,6,13,10,18,15 };
	unsigned i, got = 0;
	TASK_CHECK(path && prof);
	memset(&sd, 0, sizeof sd);
	sd.path = path; sd.profile = prof;
	TASK_CHECK(qaws_surface_create_swept(&s, &sd) == QAWS_STATUS_OK);
	TASK_CHECK(qaws_surface_swept_set_rmf(s, NULL, (qaws_scalar)1e-6) == QAWS_STATUS_OK);
	for (i = 0; i < NQ; i++)
	{
		us[i] = (qaws_scalar)((i * 37 + 13) % 101) / 101;
		vs[i] = (qaws_scalar)((i * 53 + 7) % 97) / 97;
		TASK_CHECK(qaws_surface_evaluate(&sequential[i], s, us[i], vs[i], ALL_FLAGS) == QAWS_STATUS_OK);
	}
	/* a different query order must give bit-identical results */
	for (i = 0; i < NQ; i++)
	{
		unsigned k = perm[i];
		TASK_CHECK(qaws_surface_evaluate(&shuffled[i], s, us[k], vs[k], ALL_FLAGS) == QAWS_STATUS_OK);
		TASK_CHECK(memcmp(&shuffled[i], &sequential[k], sizeof(qaws_surface_eval_result)) == 0);
	}
	/* the grid batch path agrees with the scalar queries at the same (u, v) */
	TASK_CHECK(qaws_surface_sample(grid, &got, s, 5, 5, 25) == QAWS_STATUS_OK);
	TASK_CHECK(got == 25);
	for (i = 0; i < 25; i++)
	{
		qaws_scalar u = (qaws_scalar)(i / 5) / 4, v = (qaws_scalar)(i % 5) / 4;
		qaws_surface_eval_result r;
		TASK_CHECK(eval_at(s, u, v, QAWS_SURFACE_EVAL_POSITION, &r) == 0);
		TASK_CHECK(memcmp(&grid[i], &r.position, sizeof(qaws_vec3)) == 0);
	}
	qaws_surface_destroy(s);
	qaws_curve_destroy(prof);
	qaws_curve_destroy(path);
	return 0;
}

/* ------------------------------------------------------------ registration */

const struct task_case task_cases[] = {
	{ "span_transport", "narrow_pipe_vs_dense_reference", case_narrow_pipe },
	{ "span_transport", "inflection_and_straight_rail", case_inflection_and_line },
	{ "span_transport", "nurbs_rail_transport", case_nurbs_transport },
	{ "surface_differentials", "swept_derivatives_from_position_differences", case_swept_differentials },
	{ "surface_differentials", "normalized_u_chain_scale", case_chain_scale },
	{ "mode_allocator", "enable_rebuild_disable_with_surface_allocator", case_mode_lifecycle },
	{ "mode_allocator", "rejected_calls_keep_current_mode", case_rejected_keeps_mode },
	{ "precision_batch", "f32_f64_agreement", case_precision_agreement },
	{ "precision_batch", "tolerance_hierarchy", case_tolerance_hierarchy },
	{ "precision_batch", "query_order_and_batch", case_query_order_and_batch },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
