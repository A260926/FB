/*
 * qaws_sweep_rmf.c - the span-adaptive Bishop (rotation-minimizing) frame field
 * shared by the swept and pipe surfaces.
 *
 * A fixed table over the whole parameter domain misses a turn compressed into
 * a narrow knot span: the samples on either side of it are far apart in the
 * curve's geometry and the single double reflection between them skips the
 * rotation. The field here instead gives every real knot span of the source
 * path its own subdivision: each span is transported from the frame at its
 * start and refined in place until its normals meet the angular target, then
 * the last frame carries into the next span. Queries take one double
 * reflection from the sample at or below them, so the field stays continuous
 * across straight pieces, curvature zeros and the cache intervals, and its
 * answer is independent of the order of the queries.
 *
 * Compiled once per precision (the qaws_names.h map gives every entry its
 * _f32/_f64 form); both backends call this one implementation.
 */

#include "internal/qaws_internal_rmf.h"
#include "internal/qaws_internal_types.h"
#include "internal/qaws_internal_curve.h"
#include "qaws_eval.h"
#include "qaws_inspect.h"
#include "core/qaws_frame_core.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define QAWS_RMF_MAX_STEPS_PER_SPAN 4096u

struct qaws_rmf_field
{
	qaws_curve const* path;
	qaws_range range;
	qaws_scalar tolerance;
	unsigned int span_count;
	qaws_scalar* bounds;       /* span_count + 1 global parameters */
	unsigned int* offsets;     /* span_count + 1 node offsets; span s owns nodes[offsets[s] .. offsets[s+1]] */
	unsigned int node_count;
	qaws_vec3* P;
	qaws_vec3* T;
	qaws_vec3* N;
	qaws_vec3* B;
};

static int rmf_finite_v3(qaws_vec3 v)
{
	return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static qaws_scalar rmf_dot(qaws_vec3 a, qaws_vec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static qaws_vec3 rmf_cross(qaws_vec3 a, qaws_vec3 b)
{
	qaws_vec3 r;
	r.x = a.y * b.z - a.z * b.y;
	r.y = a.z * b.x - a.x * b.z;
	r.z = a.x * b.y - a.y * b.x;
	return r;
}

static int rmf_normalize(qaws_vec3* v)
{
	qaws_scalar l = QAWS_SQRT(rmf_dot(*v, *v));
	if (!(l > QAWS_ZERO))
		return 0;
	v->x /= l; v->y /= l; v->z /= l;
	return 1;
}

/* the path point and unit tangent at t; DEGENERATE_CURVE where the speed vanishes */
static qaws_status rmf_sample(qaws_curve const* path, qaws_scalar t, qaws_vec3* P, qaws_vec3* T)
{
	qaws_eval_result_3d e;
	qaws_status st = qaws_curve_evaluate_3d(&e, path, t, QAWS_EVAL_FLAG_POSITION | QAWS_EVAL_FLAG_D1);
	if (st != QAWS_STATUS_OK)
		return st;
	if (!rmf_finite_v3(e.position) || !rmf_finite_v3(e.d1))
		return QAWS_STATUS_NUMERICAL_FAILURE;
	*P = e.position;
	*T = e.d1;
	if (!rmf_normalize(T))
		return QAWS_STATUS_DEGENERATE_CURVE;
	return QAWS_STATUS_OK;
}

/* one double-reflection transport from (p0, t0, n0) to (p1, t1): the reflected normal, made unit and perpendicular */
static qaws_vec3 rmf_transport_one(qaws_vec3 p0, qaws_vec3 t0, qaws_vec3 n0, qaws_vec3 p1, qaws_vec3 t1)
{
	qaws_vec3 r = qaws_double_reflection(p0, t0, n0, p1, t1);
	qaws_scalar d = rmf_dot(r, t1);
	r.x -= d * t1.x; r.y -= d * t1.y; r.z -= d * t1.z;
	rmf_normalize(&r);
	return r;
}

/* the angle between two unit normals, from their chord (acos of the dot product errs by sqrt(eps) near 0) */
static qaws_scalar rmf_angle(qaws_vec3 a, qaws_vec3 b)
{
	qaws_scalar dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	qaws_scalar ch = QAWS_SQRT(dx * dx + dy * dy + dz * dz) / QAWS_LITERAL(2.0);
	if (ch > QAWS_ONE) ch = QAWS_ONE;
	return QAWS_LITERAL(2.0) * asin(ch);
}

/* Transport one span [t0, t1] in m equal steps from the incoming frame, into caller arrays of m + 1 entries.
   The start node (index 0) is written from the incoming frame. */
static qaws_status rmf_run_span(qaws_curve const* path, qaws_scalar t0, qaws_scalar t1, unsigned int m,
	qaws_vec3 p0, qaws_vec3 tg0, qaws_vec3 n0, qaws_vec3* P, qaws_vec3* T, qaws_vec3* N, qaws_vec3* B)
{
	unsigned int j;
	P[0] = p0; T[0] = tg0; N[0] = n0; B[0] = rmf_cross(tg0, n0);
	for (j = 1; j <= m; j++)
	{
		qaws_scalar t = t0 + (t1 - t0) * ((qaws_scalar)j / (qaws_scalar)m);
		qaws_vec3 p1, tg1, n1;
		qaws_status st = rmf_sample(path, t, &p1, &tg1);
		if (st != QAWS_STATUS_OK)
			return st;
		n1 = rmf_transport_one(P[j - 1], T[j - 1], N[j - 1], p1, tg1);
		P[j] = p1; T[j] = tg1; N[j] = n1; B[j] = rmf_cross(tg1, n1);
	}
	return QAWS_STATUS_OK;
}

/* growable node buffers; every block comes from the surface's allocator, so the field and its scratch are freed by it */
typedef struct rmf_nodes
{
	qaws_vec3 *P, *T, *N, *B;
	unsigned int count, cap;
} rmf_nodes;

/* grow one vec3 block through the given allocator (explicit alloc, copy, free: even the first block is its, not the thread's) */
static qaws_vec3* rmf_grow(qaws_vec3* old, unsigned int old_cap, unsigned int cap, qaws_allocator const* allocator)
{
	qaws_vec3* nb = (qaws_vec3*)qaws_internal_alloc((size_t)cap * sizeof(qaws_vec3), allocator);
	if (!nb)
		return NULL;
	if (old)
	{
		memcpy(nb, old, (size_t)old_cap * sizeof(qaws_vec3));
		qaws_internal_dealloc(old, allocator);
	}
	return nb;
}

static qaws_status rmf_reserve(rmf_nodes* a, unsigned int need, qaws_allocator const* allocator)
{
	qaws_vec3* np;
	unsigned int cap = a->cap ? a->cap : 16;
	if (need <= a->cap)
		return QAWS_STATUS_OK;
	while (cap < need) cap *= 2;
	np = rmf_grow(a->P, a->cap, cap, allocator);
	if (!np) return QAWS_STATUS_ALLOCATION_FAILURE;
	a->P = np;
	np = rmf_grow(a->T, a->cap, cap, allocator);
	if (!np) return QAWS_STATUS_ALLOCATION_FAILURE;
	a->T = np;
	np = rmf_grow(a->N, a->cap, cap, allocator);
	if (!np) return QAWS_STATUS_ALLOCATION_FAILURE;
	a->N = np;
	np = rmf_grow(a->B, a->cap, cap, allocator);
	if (!np) return QAWS_STATUS_ALLOCATION_FAILURE;
	a->B = np;
	a->cap = cap;
	return QAWS_STATUS_OK;
}

/* the angular error of using table t (m + 1 nodes) exactly as the field's queries use it - one double reflection from
   the stored node at or below the point - against the independent reference run at every reference node */
static qaws_scalar rmf_interp_error(rmf_nodes const* t, unsigned int m, rmf_nodes const* ref, unsigned int rm)
{
	unsigned int j;
	qaws_scalar est = QAWS_ZERO;
	for (j = 0; j <= rm; j++)
	{
		unsigned int ci = (unsigned int)((qaws_scalar)j * (qaws_scalar)m / (qaws_scalar)rm);
		qaws_vec3 nq;
		qaws_scalar a;
		if (ci > m) ci = m;
		nq = rmf_transport_one(t->P[ci], t->T[ci], t->N[ci], ref->P[j], ref->T[j]);
		a = rmf_angle(nq, ref->N[j]);
		if (a > est) est = a;
	}
	return est;
}

/* the most steps a span can use before its parameter increments fall under the scalar's resolution: a step finer than
   a few ulps cannot place its nodes at distinct parameters, and the duplicate samples would make a reflection skip
   geometry instead of resolving it. f64 stays under the hard cap; f32 is bounded by its ~7-digit parameters */
static unsigned int rmf_span_max_steps(qaws_scalar t0, qaws_scalar t1)
{
#if QAWS_SCALAR_IS_FLOAT
	qaws_scalar const machine_eps = QAWS_LITERAL(1.19209290e-7);   /* float epsilon */
#else
	qaws_scalar const machine_eps = QAWS_LITERAL(2.2204460492503131e-16);   /* double epsilon */
#endif
	qaws_scalar min_inc = QAWS_LITERAL(16.0) * machine_eps
		* qaws_max(QAWS_ONE, qaws_max(QAWS_FABS(t0), QAWS_FABS(t1)));
	qaws_scalar mm = (t1 - t0) / min_inc;
	if (!(mm > QAWS_ONE))
		return 1;
	if (mm > (qaws_scalar)QAWS_RMF_MAX_STEPS_PER_SPAN)
		return QAWS_RMF_MAX_STEPS_PER_SPAN;
	return (unsigned int)mm;
}

/* the largest power-of-two run whose doubled final table stays within max_steps (the doubling loops need a power of two
   they can reach without ever going past the parameter-resolution cap) */
static unsigned int rmf_pow2_under(unsigned int max_steps)
{
	unsigned int c = 1;
	while (c * 2 <= max_steps && c < QAWS_RMF_MAX_STEPS_PER_SPAN / 2)
		c *= 2;
	return c;
}

/* the smallest number of steps a span must carry: enough that the interpolated frame's derivative stays consistent
   with the analytic frame rate at the surface's normalized-u resolution. The old uniform table placed a node about
   every 1/256 of the domain, which kept du, duv and duu within their position-difference bounds; a per-span minimum at
   the same spacing preserves that, while angular refinement still adds nodes where a narrow span needs them. This is
   per-span density, not a fixed table over the whole domain. */
static unsigned int rmf_span_min_steps(qaws_scalar t0, qaws_scalar t1, qaws_range range)
{
	qaws_scalar domain = range.max_value - range.min_value;
	qaws_scalar width = t1 - t0;
	qaws_scalar need;
	if (!(domain > QAWS_ZERO) || !(width > QAWS_ZERO))
		return 1;
	need = width / domain * QAWS_LITERAL(256.0);
	if (need < QAWS_ONE) need = QAWS_ONE;
	if (need > (qaws_scalar)QAWS_RMF_MAX_STEPS_PER_SPAN)
		need = (qaws_scalar)QAWS_RMF_MAX_STEPS_PER_SPAN;
	return (unsigned int)need;
}

/* the next power of two >= n */
static unsigned int rmf_pow2_ceiling(unsigned int n)
{
	unsigned int c = 1;
	while (c < n && c < QAWS_RMF_MAX_STEPS_PER_SPAN / 2)
		c *= 2;
	return c;
}

/* The subdivision of one span. Two independent adaptive loops over its own interval:
   1. a deep reference run, doubled until its own query interpolation against a run twice as fine converges (a fall by
      less than 8x means the fourth-order truncation has met the rounding floor), capped at the parameter-resolution
      limit so no two nodes ever share a parameter;
   2. the table the field stores, doubled until its query interpolation against that reference meets the target, or it
      reaches the cap (the scalar cannot resolve a finer step there).
   Nothing is sampled on a fixed grid over the whole domain: each span earns its own depth. */
static qaws_status rmf_refine_span(qaws_curve const* path, qaws_scalar t0, qaws_scalar t1, qaws_range range,
	qaws_vec3 p0, qaws_vec3 tg0, qaws_vec3 n0, qaws_scalar tolerance,
	rmf_nodes* coarse, rmf_nodes* fine, qaws_allocator const* allocator,
	unsigned int* out_m, qaws_vec3* out_p1, qaws_vec3* out_t1, qaws_vec3* out_n1)
{
	rmf_nodes ref_lo = { 0 }, ref_hi = { 0 }, cand = { 0 };
	unsigned int max_steps = rmf_span_max_steps(t0, t1);
	unsigned int cap = rmf_pow2_under(max_steps);
	unsigned int min_steps = rmf_pow2_ceiling(rmf_span_min_steps(t0, t1, range));
	unsigned int r, m;
	qaws_scalar prev_ref_est = QAWS_LITERAL(1e30);
	qaws_status st;
	if (min_steps > cap) min_steps = cap;
	/* the deep reference must be at least as fine as the densest table we may store, so start it at min_steps */
	r = min_steps;
	if (r < 64) r = 64;
	if (r > cap) r = cap;

	/* the deep reference: r and 2r runs (2r never past the resolution cap) until the coarser one's query
	   interpolation against the finer has converged */
	for (;;)
	{
		qaws_scalar est;
		st = rmf_reserve(&ref_lo, r + 1, allocator);
		if (st != QAWS_STATUS_OK) return st;
		st = rmf_run_span(path, t0, t1, r, p0, tg0, n0, ref_lo.P, ref_lo.T, ref_lo.N, ref_lo.B);
		if (st != QAWS_STATUS_OK) return st;
		st = rmf_reserve(&ref_hi, 2 * r + 1, allocator);
		if (st != QAWS_STATUS_OK) return st;
		st = rmf_run_span(path, t0, t1, 2 * r, p0, tg0, n0, ref_hi.P, ref_hi.T, ref_hi.N, ref_hi.B);
		if (st != QAWS_STATUS_OK) return st;
		est = rmf_interp_error(&ref_lo, r, &ref_hi, 2 * r);
		if (est <= tolerance * QAWS_LITERAL(0.0625) || est >= QAWS_LITERAL(0.125) * prev_ref_est
			|| r >= cap)
			break;
		prev_ref_est = est;
		r *= 2;
	}

	/* the stored table: refine against the converged deep reference (ref_hi, 2r nodes); never below the per-span
	   density the analytic derivative consistency needs, never past the parameter-resolution cap */
	m = min_steps;
	for (;;)
	{
		qaws_scalar est;
		st = rmf_reserve(&cand, m + 1, allocator);
		if (st != QAWS_STATUS_OK) return st;
		st = rmf_run_span(path, t0, t1, m, p0, tg0, n0, cand.P, cand.T, cand.N, cand.B);
		if (st != QAWS_STATUS_OK) return st;
		est = rmf_interp_error(&cand, m, &ref_hi, 2 * r);
		if (est <= tolerance || m >= cap)
			break;
		m *= 2;
	}

	/* hand the accepted run back through the caller's scratch buffers */
	st = rmf_reserve(coarse, m + 1, allocator);
	if (st != QAWS_STATUS_OK) return st;
	memcpy(coarse->P, cand.P, (size_t)(m + 1) * sizeof(qaws_vec3));
	memcpy(coarse->T, cand.T, (size_t)(m + 1) * sizeof(qaws_vec3));
	memcpy(coarse->N, cand.N, (size_t)(m + 1) * sizeof(qaws_vec3));
	memcpy(coarse->B, cand.B, (size_t)(m + 1) * sizeof(qaws_vec3));
	coarse->count = m + 1;
	st = rmf_reserve(fine, 2 * m + 1, allocator);
	if (st != QAWS_STATUS_OK) return st;
	st = rmf_run_span(path, t0, t1, 2 * m, p0, tg0, n0, fine->P, fine->T, fine->N, fine->B);
	if (st != QAWS_STATUS_OK) return st;

	qaws_internal_dealloc(cand.P, allocator); qaws_internal_dealloc(cand.T, allocator);
	qaws_internal_dealloc(cand.N, allocator); qaws_internal_dealloc(cand.B, allocator);
	qaws_internal_dealloc(ref_lo.P, allocator); qaws_internal_dealloc(ref_lo.T, allocator);
	qaws_internal_dealloc(ref_lo.N, allocator); qaws_internal_dealloc(ref_lo.B, allocator);
	qaws_internal_dealloc(ref_hi.P, allocator); qaws_internal_dealloc(ref_hi.T, allocator);
	qaws_internal_dealloc(ref_hi.N, allocator); qaws_internal_dealloc(ref_hi.B, allocator);

	*out_m = 2 * m;
	*out_p1 = fine->P[2 * m];
	*out_t1 = fine->T[2 * m];
	*out_n1 = fine->N[2 * m];
	return QAWS_STATUS_OK;
}

qaws_status qaws_rmf_validate_tolerance(qaws_scalar angular_tolerance)
{
	if (!isfinite(angular_tolerance))
		return QAWS_STATUS_INVALID_ARGUMENT;
	if (angular_tolerance < QAWS_ZERO || angular_tolerance > QAWS_RMF_TOLERANCE_MAX || angular_tolerance < QAWS_RMF_TOLERANCE_MIN)
		return QAWS_STATUS_OUT_OF_RANGE;
	return QAWS_STATUS_OK;
}

/* the default first normal: the Frenet normal where it is defined, else the axis least along the tangent made perpendicular */
static qaws_status rmf_default_normal(qaws_curve const* path, qaws_scalar t, qaws_vec3 T, qaws_vec3* N)
{
	qaws_vec3 tg, fr_n, fr_b;
	qaws_status st = qaws_curve_compute_frenet_frame_3d(&tg, &fr_n, &fr_b, path, t);
	if (st == QAWS_STATUS_OK && rmf_finite_v3(fr_n))
	{
		qaws_scalar d = rmf_dot(fr_n, T);
		fr_n.x -= d * T.x; fr_n.y -= d * T.y; fr_n.z -= d * T.z;
		if (rmf_normalize(&fr_n))
		{
			*N = fr_n;
			return QAWS_STATUS_OK;
		}
	}
	{
		qaws_scalar ax = QAWS_FABS(T.x), ay = QAWS_FABS(T.y), az = QAWS_FABS(T.z);
		qaws_vec3 r;
		r.x = ax <= ay && ax <= az ? QAWS_ONE : QAWS_ZERO;
		r.y = r.x == QAWS_ZERO && ay <= az ? QAWS_ONE : QAWS_ZERO;
		r.z = r.x == QAWS_ZERO && r.y == QAWS_ZERO ? QAWS_ONE : QAWS_ZERO;
		{
			qaws_scalar d = rmf_dot(r, T);
			r.x -= d * T.x; r.y -= d * T.y; r.z -= d * T.z;
		}
		if (!rmf_normalize(&r))
			return QAWS_STATUS_NUMERICAL_FAILURE;
		*N = r;
	}
	return QAWS_STATUS_OK;
}

qaws_status qaws_rmf_field_build(
	qaws_rmf_field** out_field,
	qaws_curve const* path,
	qaws_range path_range,
	qaws_vec3 const* initial_normal,
	qaws_scalar angular_tolerance,
	qaws_allocator const* allocator)
{
	qaws_rmf_field* field;
	qaws_vec3 p0, tg0, n0, p1, t1, n1;
	qaws_scalar const* src_bounds;
	rmf_nodes coarse = { 0 }, fine = { 0 }, accepted = { 0 };
	unsigned int span, span_count;
	qaws_status st;

	if (!out_field || !path)
		return QAWS_STATUS_INVALID_ARGUMENT;
	st = qaws_rmf_validate_tolerance(angular_tolerance);
	if (st != QAWS_STATUS_OK)
		return st;
	if (path->dimension != QAWS_DIMENSION_3D || path->span_count == 0 || !path->span_boundaries)
		return QAWS_STATUS_INVALID_DIMENSION;
	if (!isfinite(path_range.min_value) || !isfinite(path_range.max_value)
		|| !(path_range.max_value > path_range.min_value))
		return QAWS_STATUS_INVALID_PARAMETER_RANGE;

	/* the first frame: a finite speed and a usable initial normal */
	st = rmf_sample(path, path_range.min_value, &p0, &tg0);
	if (st != QAWS_STATUS_OK)
		return st;
	if (initial_normal)
	{
		qaws_scalar d, l0;
		if (!rmf_finite_v3(*initial_normal))
			return QAWS_STATUS_INVALID_ARGUMENT;
		n0 = *initial_normal;
		l0 = QAWS_SQRT(rmf_dot(n0, n0));
		if (!(l0 > QAWS_ZERO))
			return QAWS_STATUS_INVALID_ARGUMENT;
		d = rmf_dot(n0, tg0);
		n0.x -= d * tg0.x; n0.y -= d * tg0.y; n0.z -= d * tg0.z;
		/* parallel to the tangent (or near enough that nothing survives the projection) */
		if (QAWS_SQRT(rmf_dot(n0, n0)) <= QAWS_LITERAL(1e-9) * l0 || !rmf_normalize(&n0))
			return QAWS_STATUS_INVALID_ARGUMENT;
	}
	else
	{
		st = rmf_default_normal(path, path_range.min_value, tg0, &n0);
		if (st != QAWS_STATUS_OK)
			return st;
	}

	field = (qaws_rmf_field*)qaws_internal_alloc(sizeof(qaws_rmf_field), allocator);
	if (!field)
		return QAWS_STATUS_ALLOCATION_FAILURE;
	memset(field, 0, sizeof *field);
	field->path = path;
	field->range = path_range;
	field->tolerance = angular_tolerance;
	field->span_count = path->span_count;
	span_count = path->span_count;
	src_bounds = path->span_boundaries;

	field->bounds = (qaws_scalar*)qaws_internal_alloc((size_t)(span_count + 1) * sizeof(qaws_scalar), allocator);
	field->offsets = (unsigned int*)qaws_internal_alloc((size_t)(span_count + 1) * sizeof(unsigned int), allocator);
	if (!field->bounds || !field->offsets)
	{
		qaws_rmf_field_free(field, allocator);
		return QAWS_STATUS_ALLOCATION_FAILURE;
	}
	/* the real knot spans, each bound clipped to the build range so the table stays monotone over a non-unit domain */
	field->bounds[0] = path_range.min_value;
	for (span = 0; span < span_count; span++)
	{
		qaws_scalar hi = src_bounds[span + 1];
		if (hi < path_range.min_value) hi = path_range.min_value;
		if (hi > path_range.max_value) hi = path_range.max_value;
		field->bounds[span + 1] = hi;
	}
	field->bounds[span_count] = path_range.max_value;

	/* sequential transport: every span refined on its own, the accepted frame carrying into the next */
	st = rmf_reserve(&accepted, 16, allocator);
	if (st != QAWS_STATUS_OK) { qaws_rmf_field_free(field, allocator); return st; }
	accepted.P[0] = p0; accepted.T[0] = tg0; accepted.N[0] = n0; accepted.B[0] = rmf_cross(tg0, n0);
	accepted.count = 1;
	field->offsets[0] = 0;
	p1 = p0; t1 = tg0; n1 = n0;
	for (span = 0; span < span_count; span++)
	{
		unsigned int m;
		qaws_scalar t0 = field->bounds[span], t_hi = field->bounds[span + 1];
		if (!(t_hi > t0))
		{
			/* a zero-length piece outside the build range carries no geometry */
			field->offsets[span + 1] = accepted.count - 1;
			continue;
		}
		st = rmf_refine_span(path, t0, t_hi, path_range, p1, t1, n1, angular_tolerance, &coarse, &fine, allocator,
			&m, &p1, &t1, &n1);
		if (st != QAWS_STATUS_OK)
		{
			qaws_internal_free(coarse.P); qaws_internal_free(coarse.T); qaws_internal_free(coarse.N); qaws_internal_free(coarse.B);
			qaws_internal_free(fine.P); qaws_internal_free(fine.T); qaws_internal_free(fine.N); qaws_internal_free(fine.B);
			qaws_rmf_field_free(field, allocator);
			return st;
		}
		/* the accepted run is the fine (2m) one; append its nodes except the shared start */
		st = rmf_reserve(&accepted, accepted.count + m, allocator);
		if (st != QAWS_STATUS_OK)
		{
			qaws_internal_free(coarse.P); qaws_internal_free(coarse.T); qaws_internal_free(coarse.N); qaws_internal_free(coarse.B);
			qaws_internal_free(fine.P); qaws_internal_free(fine.T); qaws_internal_free(fine.N); qaws_internal_free(fine.B);
			qaws_rmf_field_free(field, allocator);
			return st;
		}
		memcpy(accepted.P + accepted.count, fine.P + 1, (size_t)m * sizeof(qaws_vec3));
		memcpy(accepted.T + accepted.count, fine.T + 1, (size_t)m * sizeof(qaws_vec3));
		memcpy(accepted.N + accepted.count, fine.N + 1, (size_t)m * sizeof(qaws_vec3));
		memcpy(accepted.B + accepted.count, fine.B + 1, (size_t)m * sizeof(qaws_vec3));
		accepted.count += m;
		field->offsets[span + 1] = accepted.count - 1;
	}
	qaws_internal_free(coarse.P); qaws_internal_free(coarse.T); qaws_internal_free(coarse.N); qaws_internal_free(coarse.B);
	qaws_internal_free(fine.P); qaws_internal_free(fine.T); qaws_internal_free(fine.N); qaws_internal_free(fine.B);

	/* compact into exact-size blocks owned by the field */
	field->node_count = accepted.count;
	field->P = (qaws_vec3*)qaws_internal_alloc((size_t)accepted.count * sizeof(qaws_vec3), allocator);
	field->T = (qaws_vec3*)qaws_internal_alloc((size_t)accepted.count * sizeof(qaws_vec3), allocator);
	field->N = (qaws_vec3*)qaws_internal_alloc((size_t)accepted.count * sizeof(qaws_vec3), allocator);
	field->B = (qaws_vec3*)qaws_internal_alloc((size_t)accepted.count * sizeof(qaws_vec3), allocator);
	if (!field->P || !field->T || !field->N || !field->B)
	{
		qaws_internal_free(accepted.P); qaws_internal_free(accepted.T); qaws_internal_free(accepted.N); qaws_internal_free(accepted.B);
		qaws_rmf_field_free(field, allocator);
		return QAWS_STATUS_ALLOCATION_FAILURE;
	}
	memcpy(field->P, accepted.P, (size_t)accepted.count * sizeof(qaws_vec3));
	memcpy(field->T, accepted.T, (size_t)accepted.count * sizeof(qaws_vec3));
	memcpy(field->N, accepted.N, (size_t)accepted.count * sizeof(qaws_vec3));
	memcpy(field->B, accepted.B, (size_t)accepted.count * sizeof(qaws_vec3));
	qaws_internal_free(accepted.P); qaws_internal_free(accepted.T); qaws_internal_free(accepted.N); qaws_internal_free(accepted.B);

	*out_field = field;
	return QAWS_STATUS_OK;
}

void qaws_rmf_field_free(qaws_rmf_field* field, qaws_allocator const* allocator)
{
	if (!field)
		return;
	qaws_internal_dealloc(field->P, allocator);
	qaws_internal_dealloc(field->T, allocator);
	qaws_internal_dealloc(field->N, allocator);
	qaws_internal_dealloc(field->B, allocator);
	qaws_internal_dealloc(field->bounds, allocator);
	qaws_internal_dealloc(field->offsets, allocator);
	qaws_internal_dealloc(field, allocator);
}

/* the span and the local fraction of u within it */
static unsigned int rmf_locate(qaws_rmf_field const* field, qaws_scalar u, qaws_scalar* out_local)
{
	unsigned int lo = 0, hi = field->span_count;
	qaws_scalar t = field->range.min_value
		+ u * (field->range.max_value - field->range.min_value);
	while (lo < hi)
	{
		unsigned int mid = lo + (hi - lo) / 2;
		if (t < field->bounds[mid + 1])
			hi = mid;
		else
			lo = mid + 1;
	}
	if (lo >= field->span_count)
		lo = field->span_count - 1;
	{
		qaws_scalar b0 = field->bounds[lo], b1 = field->bounds[lo + 1];
		qaws_scalar frac = b1 > b0 ? (t - b0) / (b1 - b0) : QAWS_ZERO;
		if (frac < QAWS_ZERO) frac = QAWS_ZERO;
		if (frac > QAWS_ONE) frac = QAWS_ONE;
		*out_local = frac;
	}
	return lo;
}

qaws_status qaws_rmf_field_frame(
	qaws_rmf_field const* field,
	qaws_scalar u,
	qaws_eval_result_3d* out_path,
	qaws_vec3* out_tangent,
	qaws_vec3* out_normal,
	qaws_vec3* out_binormal)
{
	qaws_scalar local, t;
	unsigned int span, i0, i1, step;
	qaws_vec3 P, T;
	qaws_status st;

	if (!field || !out_path || !out_tangent || !out_normal || !out_binormal)
		return QAWS_STATUS_INVALID_ARGUMENT;
	if (u < QAWS_ZERO) u = QAWS_ZERO;
	if (u > QAWS_ONE) u = QAWS_ONE;

	span = rmf_locate(field, u, &local);
	i0 = field->offsets[span];
	i1 = field->offsets[span + 1];
	step = i1 - i0;
	t = field->range.min_value + u * (field->range.max_value - field->range.min_value);

	st = qaws_curve_evaluate_3d(out_path, field->path, t,
		QAWS_EVAL_FLAG_POSITION | QAWS_EVAL_FLAG_D1 | QAWS_EVAL_FLAG_D2);
	if (st != QAWS_STATUS_OK)
		return st;
	P = out_path->position;
	T = out_path->d1;
	if (!rmf_normalize(&T))
		T = field->T[i0];   /* the guaranteed-nonzero-speed tables fall back to the carried tangent */
	{
		/* one double reflection from the cached node at or below the query: smooth inside every cache interval */
		unsigned int k = i0 + (unsigned int)(local * (qaws_scalar)step);
		qaws_vec3 N;
		if (k > i1) k = i1;
		N = rmf_transport_one(field->P[k], field->T[k], field->N[k], P, T);
		*out_tangent = T;
		*out_normal = N;
		*out_binormal = rmf_cross(T, N);
	}
	return QAWS_STATUS_OK;
}

void qaws_rmf_frame_rates(
	qaws_eval_result_3d const* c,
	qaws_vec3 T,
	qaws_vec3 N,
	qaws_vec3 B,
	qaws_vec3* out_dn,
	qaws_vec3* out_db)
{
	qaws_scalar sp = QAWS_SQRT(rmf_dot(c->d1, c->d1));
	qaws_vec3 dt = { QAWS_ZERO, QAWS_ZERO, QAWS_ZERO };
	qaws_scalar kn, kb;
	if (sp > QAWS_LITERAL(1e-15))
	{
		qaws_scalar a = rmf_dot(c->d2, T);
		dt.x = (c->d2.x - a * T.x) / sp;
		dt.y = (c->d2.y - a * T.y) / sp;
		dt.z = (c->d2.z - a * T.z) / sp;
	}
	kn = rmf_dot(dt, N);
	kb = rmf_dot(dt, B);
	out_dn->x = -kn * T.x;
	out_dn->y = -kn * T.y;
	out_dn->z = -kn * T.z;
	out_db->x = -kb * T.x;
	out_db->y = -kb * T.y;
	out_db->z = -kb * T.z;
}
