/*
 * Shared rotation-minimizing (Bishop, parallel-transport) frame field of a 3D
 * path curve: the one capability the swept surface and the pipe surface build
 * their cross-section frames from (qaws_sweep_rmf.h).
 *
 * The field is a table of nodes over the path's parameter. A node sits at each
 * end of every distinct knot span, and a span is split further only where it
 * needs to be: an interval subdivides while the cubic Hermite of its two
 * endpoint normals (with their analytic transport rates) misses, at the
 * interval's midpoint, the normal reached by two quarter-steps of the double
 * reflection by more than the tolerance. A narrow span that carries a whole
 * spatial turn therefore gets as many nodes as it needs, while a long gently
 * bending span gets few; nothing is densified uniformly over the whole range.
 * The endpoint normals come from the curve, carried from node to node by
 * double reflection (core/qaws_frame_core.h), never blended axis by axis.
 *
 * A query interpolates the two nodes around it with a cubic Hermite in the
 * curve parameter, then projects the result perpendicular to the tangent and
 * normalizes it. The Hermite value and derivative agree at the nodes, so the
 * field is C1: neither its value nor its first derivative jumps across a node
 * or across a cache interval. A query's derivative is the exact derivative of
 * that same interpolant. Positions, first and second partials and the surface
 * normal therefore all come from one continuous field.
 *
 * The transport rate N' = -(T' . N) T carries no component along the binormal,
 * so the frame does not rotate about the tangent. A straight piece or an
 * inflection is carried with the previous frame rather than a jumping Frenet
 * frame.
 */

#include "qaws_sweep_rmf.h"
#include "qaws_eval.h"
#include "qaws_inspect.h"
#include "internal/qaws_internal_curve.h"
#include "core/qaws_frame_core.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "qaws_platform.h"

/* the smallest and largest accepted angular tolerance, radians */
#define QAWS_SWEEP_RMF_TOLERANCE_MIN QAWS_LITERAL(1e-7)
#define QAWS_SWEEP_RMF_TOLERANCE_MAX QAWS_LITERAL(1e-3)

/* the deepest one initial interval may split; the tolerance (or the numeric floor) stops it first */
#define QAWS_SWEEP_RMF_MAX_DEPTH 40u

/* the most nodes a field may hold (far above any real path, below any allocation that would be attempted) */
#define QAWS_SWEEP_RMF_MAX_NODES 400000u

/* The refinement test compares the Hermite's midpoint against a finer transport; its error is a fraction of the field's
   error at the same place. Accepting at a fraction of the requested tolerance leaves the field's own direction error
   comfortably inside the tolerance, so a tolerance at the tight end reaches the scalar's numeric floor. */
#define QAWS_SWEEP_RMF_BUDGET_FRACTION QAWS_LITERAL(0.03125)

/* A rounding floor for the refinement test: in float the two-quarter transport it compares against is itself only accurate
   to a few ulps, so the measured error stops falling. Accepting at the floor stops the subdivision there instead of at the
   node cap; the field's own direction error is then a few 1e-6, inside the float bounds. Double is far below any
   tolerance. */
#if QAWS_SCALAR_IS_FLOAT
#define QAWS_SWEEP_RMF_ERROR_FLOOR QAWS_LITERAL(1e-6)
#else
#define QAWS_SWEEP_RMF_ERROR_FLOOR QAWS_LITERAL(1e-12)
#endif

/* one node: the curve parameter, the transported unit normal there, and its rate with respect to the parameter */
typedef struct qaws_sweep_rmf_node
{
	qaws_scalar parameter;
	qaws_vec3 normal;
	qaws_vec3 rate;
} qaws_sweep_rmf_node;

struct qaws_sweep_rmf_field
{
	qaws_curve const* path;
	qaws_range range;
	qaws_sweep_rmf_node* nodes;
	unsigned int node_count;
	unsigned int node_capacity;
	qaws_scalar tolerance;
	qaws_allocator const* allocator;
};

/* ------------------------------------------------------------------ */
/*  Vector helpers                                                     */
/* ------------------------------------------------------------------ */

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

static qaws_scalar rmf_len(qaws_vec3 a)
{
	return QAWS_SQRT(rmf_dot(a, a));
}

/* v made of unit length, 0 when it is (numerically) zero */
static int rmf_unit(qaws_vec3* v)
{
	qaws_scalar l = rmf_len(*v);
	if (!(l > QAWS_LITERAL(1e-15)))
		return 0;
	v->x /= l; v->y /= l; v->z /= l;
	return 1;
}

/* v made perpendicular to the unit tangent t and of unit length; 0 when it lies along t */
static int rmf_perpendicular(qaws_vec3* v, qaws_vec3 t)
{
	qaws_scalar before = rmf_len(*v);
	qaws_scalar d = rmf_dot(*v, t);
	v->x -= d * t.x;
	v->y -= d * t.y;
	v->z -= d * t.z;
	if (!(rmf_len(*v) > QAWS_LITERAL(1e-8) * before))
		return 0;
	return rmf_unit(v);
}

/* The pipe surface's original starting frame, kept as the field's default start: the axis least along the tangent gives a
   binormal, and the normal is that binormal crossed with the tangent. */
static void rmf_default_start(qaws_vec3* out_normal, qaws_vec3 tangent)
{
	qaws_vec3 up, b;
	if (QAWS_FABS(tangent.x) < QAWS_LITERAL(0.9))
	{
		up.x = QAWS_ONE; up.y = QAWS_ZERO; up.z = QAWS_ZERO;
	}
	else
	{
		up.x = QAWS_ZERO; up.y = QAWS_ONE; up.z = QAWS_ZERO;
	}
	b = rmf_cross(tangent, up);
	rmf_unit(&b);
	*out_normal = rmf_cross(b, tangent);
	rmf_unit(out_normal);
}

/* ------------------------------------------------------------------ */
/*  The path at a parameter                                            */
/* ------------------------------------------------------------------ */

/* the path point and unit tangent at t; 0 where the speed vanishes */
static int rmf_path(qaws_vec3* out_position, qaws_vec3* out_tangent, qaws_curve const* path, qaws_scalar t)
{
	qaws_eval_result_3d e;
	double l;
	memset(&e, 0, sizeof e);
	if (qaws_curve_evaluate_3d(&e, path, t, QAWS_EVAL_FLAG_POSITION | QAWS_EVAL_FLAG_D1) != QAWS_STATUS_OK)
		return 0;
	l = QAWS_SQRT((double)e.d1.x * e.d1.x + (double)e.d1.y * e.d1.y + (double)e.d1.z * e.d1.z);
	if (!(l > QAWS_LITERAL(1e-15)))
		return 0;
	out_tangent->x = (qaws_scalar)((double)e.d1.x / l);
	out_tangent->y = (qaws_scalar)((double)e.d1.y / l);
	out_tangent->z = (qaws_scalar)((double)e.d1.z / l);
	*out_position = e.position;
	return 1;
}

/* the unit tangent and its rate T' = (D2 - (D2 . T) T) / |D1| (per unit parameter) at t; 0 where the speed vanishes */
static int rmf_path_derivative(qaws_vec3* out_tangent, qaws_vec3* out_tangent_rate, qaws_curve const* path,
	qaws_scalar t)
{
	qaws_eval_result_3d e;
	double d1[3], d2[3], l, a;
	memset(&e, 0, sizeof e);
	if (qaws_curve_evaluate_3d(&e, path, t, QAWS_EVAL_FLAG_D1 | QAWS_EVAL_FLAG_D2) != QAWS_STATUS_OK)
		return 0;
	d1[0] = e.d1.x; d1[1] = e.d1.y; d1[2] = e.d1.z;
	d2[0] = e.d2.x; d2[1] = e.d2.y; d2[2] = e.d2.z;
	l = QAWS_SQRT(d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2]);
	if (!(l > QAWS_LITERAL(1e-15)))
		return 0;
	out_tangent->x = (qaws_scalar)(d1[0] / l);
	out_tangent->y = (qaws_scalar)(d1[1] / l);
	out_tangent->z = (qaws_scalar)(d1[2] / l);
	a = d2[0] * (double)out_tangent->x + d2[1] * (double)out_tangent->y + d2[2] * (double)out_tangent->z;
	out_tangent_rate->x = (qaws_scalar)((d2[0] - a * out_tangent->x) / l);
	out_tangent_rate->y = (qaws_scalar)((d2[1] - a * out_tangent->y) / l);
	out_tangent_rate->z = (qaws_scalar)((d2[2] - a * out_tangent->z) / l);
	return 1;
}

/* the normal's rate at t for a normal perpendicular to the path at t: N' = -(T' . N) T */
static void rmf_rate(qaws_vec3* out_rate, qaws_vec3 tangent, qaws_vec3 tangent_rate, qaws_vec3 normal)
{
	qaws_scalar k = -rmf_dot(tangent_rate, normal);
	out_rate->x = k * tangent.x;
	out_rate->y = k * tangent.y;
	out_rate->z = k * tangent.z;
}

/* one double-reflection step from (x0, t0, n0) to (x1, t1), made perpendicular to t1 and unit */
static int rmf_transport(qaws_vec3* out_normal, qaws_vec3 x0, qaws_vec3 t0, qaws_vec3 n0, qaws_vec3 x1, qaws_vec3 t1)
{
	qaws_vec3 r = qaws_double_reflection(x0, t0, n0, x1, t1);
	if (!rmf_perpendicular(&r, t1))
		return 0;
	*out_normal = r;
	return 1;
}

/* A cubic Hermite on [0, 1] at s, with its derivative there. n0, n1 are the endpoint values, d0, d1 their derivatives with
   respect to s (the caller multiplies a parameter rate by the interval length). */
static void rmf_hermite(qaws_vec3* out_value, qaws_vec3* out_derivative, qaws_vec3 n0, qaws_vec3 d0, qaws_vec3 n1,
	qaws_vec3 d1, qaws_scalar s)
{
	qaws_scalar s2 = s * s, s3 = s2 * s;
	qaws_scalar h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s;
	qaws_scalar h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
	qaws_scalar dh00 = 6 * s2 - 6 * s, dh10 = 3 * s2 - 4 * s + 1;
	qaws_scalar dh01 = -6 * s2 + 6 * s, dh11 = 3 * s2 - 2 * s;
	out_value->x = n0.x * h00 + d0.x * h10 + n1.x * h01 + d1.x * h11;
	out_value->y = n0.y * h00 + d0.y * h10 + n1.y * h01 + d1.y * h11;
	out_value->z = n0.z * h00 + d0.z * h10 + n1.z * h01 + d1.z * h11;
	out_derivative->x = n0.x * dh00 + d0.x * dh10 + n1.x * dh01 + d1.x * dh11;
	out_derivative->y = n0.y * dh00 + d0.y * dh10 + n1.y * dh01 + d1.y * dh11;
	out_derivative->z = n0.z * dh00 + d0.z * dh10 + n1.z * dh01 + d1.z * dh11;
}

/* ------------------------------------------------------------------ */
/*  Building the field                                                 */
/* ------------------------------------------------------------------ */

typedef struct qaws_sweep_rmf_builder
{
	qaws_curve const* path;
	qaws_sweep_rmf_node* nodes;
	unsigned int count;
	unsigned int capacity;
	qaws_scalar tolerance;
	qaws_status status;   /* QAWS_STATUS_OK while the build goes on */
} qaws_sweep_rmf_builder;

static void rmf_builder_fail(qaws_sweep_rmf_builder* b, qaws_status status)
{
	if (b->status == QAWS_STATUS_OK)
		b->status = status;
}

static void rmf_builder_push(qaws_sweep_rmf_builder* b, qaws_vec3 tangent, qaws_vec3 normal, qaws_scalar parameter,
	qaws_allocator const* allocator)
{
	qaws_vec3 tangent_rate;
	if (b->status != QAWS_STATUS_OK)
		return;
	if (b->count == b->capacity)
	{
		unsigned int cap = b->capacity ? 2 * b->capacity : 64;
		qaws_sweep_rmf_node* grown;
		size_t bytes = qaws_internal_mul2(sizeof(qaws_sweep_rmf_node), (size_t)cap);
		if (cap < b->capacity || cap > QAWS_SWEEP_RMF_MAX_NODES || bytes == SIZE_MAX)
		{
			rmf_builder_fail(b, QAWS_STATUS_ALLOCATION_FAILURE);
			return;
		}
		/* the first block comes from the field's allocator too (qaws_internal_realloc of NULL would take the current
		   allocator, not the surface's) */
		grown = b->nodes
			? (qaws_sweep_rmf_node*)qaws_internal_realloc(b->nodes, bytes)
			: (qaws_sweep_rmf_node*)qaws_internal_alloc(bytes, allocator);
		if (!grown)
		{
			rmf_builder_fail(b, QAWS_STATUS_ALLOCATION_FAILURE);
			return;
		}
		b->nodes = grown;
		b->capacity = cap;
	}
	if (!rmf_path_derivative(&tangent, &tangent_rate, b->path, parameter))
	{
		rmf_builder_fail(b, QAWS_STATUS_DEGENERATE_CURVE);
		return;
	}
	b->nodes[b->count].parameter = parameter;
	b->nodes[b->count].normal = normal;
	rmf_rate(&b->nodes[b->count].rate, tangent, tangent_rate, normal);
	b->count++;
}

/* Split [a, endpoint] until the Hermite's midpoint normal meets a finer transport there within the tolerance, appending
   every node after a up to and including endpoint. p_a, t_a, n_a, d_a describe the field at a (d_a its rate in the
   parameter). */
static void rmf_refine(qaws_sweep_rmf_builder* b, qaws_vec3 p_a, qaws_vec3 t_a, qaws_vec3 n_a, qaws_vec3 d_a,
	qaws_scalar a, qaws_scalar endpoint, unsigned int depth, qaws_allocator const* allocator)
{
	qaws_vec3 p_b, t_b, p_m, t_m, n_b, n_half, d_b, n_ref, delta, h_mid, h_rate, tangent_rate;
	qaws_scalar mid, quarter, h, error;
	if (b->status != QAWS_STATUS_OK)
		return;
	h = endpoint - a;
	mid = a + h * QAWS_LITERAL(0.5);
	quarter = a + h * QAWS_LITERAL(0.25);
	if (!rmf_path(&p_b, &t_b, b->path, endpoint)
		|| !rmf_path(&p_m, &t_m, b->path, mid)
		|| !rmf_transport(&n_half, p_a, t_a, n_a, p_m, t_m)
		|| !rmf_transport(&n_b, p_m, t_m, n_half, p_b, t_b))
	{
		rmf_builder_fail(b, QAWS_STATUS_DEGENERATE_CURVE);
		return;
	}
	/* the finer reference normal at the midpoint: two quarter-steps from a */
	{
		qaws_vec3 p_q, t_q, n_q;
		if (!rmf_path(&p_q, &t_q, b->path, quarter)
			|| !rmf_transport(&n_q, p_a, t_a, n_a, p_q, t_q)
			|| !rmf_transport(&n_ref, p_q, t_q, n_q, p_m, t_m))
		{
			rmf_builder_fail(b, QAWS_STATUS_DEGENERATE_CURVE);
			return;
		}
	}
	if (!rmf_path_derivative(&t_b, &tangent_rate, b->path, endpoint))
	{
		rmf_builder_fail(b, QAWS_STATUS_DEGENERATE_CURVE);
		return;
	}
	rmf_rate(&d_b, t_b, tangent_rate, n_b);
	{
		qaws_vec3 d0 = { d_a.x * h, d_a.y * h, d_a.z * h };
		qaws_vec3 d1 = { d_b.x * h, d_b.y * h, d_b.z * h };
		rmf_hermite(&h_mid, &h_rate, n_a, d0, n_b, d1, QAWS_LITERAL(0.5));
	}
	if (!rmf_unit(&h_mid))
	{
		rmf_builder_fail(b, QAWS_STATUS_DEGENERATE_CURVE);
		return;
	}
	delta.x = n_ref.x - h_mid.x;
	delta.y = n_ref.y - h_mid.y;
	delta.z = n_ref.z - h_mid.z;
	error = rmf_len(delta);
	if (error <= b->tolerance * QAWS_SWEEP_RMF_BUDGET_FRACTION || error <= QAWS_SWEEP_RMF_ERROR_FLOOR
		|| depth >= QAWS_SWEEP_RMF_MAX_DEPTH || b->count >= QAWS_SWEEP_RMF_MAX_NODES)
	{
		rmf_builder_push(b, t_b, n_b, endpoint, allocator);
		return;
	}
	rmf_refine(b, p_a, t_a, n_a, d_a, a, mid, depth + 1, allocator);
	if (b->status != QAWS_STATUS_OK)
		return;
	rmf_refine(b, p_m, t_m, b->nodes[b->count - 1].normal, b->nodes[b->count - 1].rate, mid, endpoint,
		depth + 1, allocator);
}

/* ------------------------------------------------------------------ */
/*  Querying the field                                                 */
/* ------------------------------------------------------------------ */

/* the field's normal and its rate at t, between the nodes around it: the cubic Hermite projected perpendicular to the
   tangent and normalized */
static int rmf_field_eval(qaws_vec3* out_normal, qaws_vec3* out_rate, qaws_sweep_rmf_field const* field,
	qaws_scalar t)
{
	qaws_vec3 tangent, tangent_rate, value, rate;
	qaws_scalar h, s;
	unsigned int lo, hi;
	double l, projected_dot;
	if (!rmf_path_derivative(&tangent, &tangent_rate, field->path, t))
		return 0;
	if (field->node_count == 0)
		return 0;
	if (field->node_count == 1 || t <= field->nodes[0].parameter)
	{
		*out_normal = field->nodes[0].normal;
		rmf_rate(out_rate, tangent, tangent_rate, field->nodes[0].normal);
		return 1;
	}
	hi = field->node_count - 1;
	if (t >= field->nodes[hi].parameter)
	{
		*out_normal = field->nodes[hi].normal;
		rmf_rate(out_rate, tangent, tangent_rate, field->nodes[hi].normal);
		return 1;
	}
	lo = 0;
	while (lo + 1 < hi)
	{
		unsigned int mid = lo + (hi - lo) / 2;
		if (field->nodes[mid].parameter <= t)
			lo = mid;
		else
			hi = mid;
	}
	h = field->nodes[hi].parameter - field->nodes[lo].parameter;
	if (!(h > QAWS_ZERO))
	{
		*out_normal = field->nodes[lo].normal;
		rmf_rate(out_rate, tangent, tangent_rate, field->nodes[lo].normal);
		return 1;
	}
	s = (t - field->nodes[lo].parameter) / h;
	{
		qaws_vec3 d0 = { field->nodes[lo].rate.x * h, field->nodes[lo].rate.y * h, field->nodes[lo].rate.z * h };
		qaws_vec3 d1 = { field->nodes[hi].rate.x * h, field->nodes[hi].rate.y * h, field->nodes[hi].rate.z * h };
		qaws_vec3 v, r;
		rmf_hermite(&v, &r, field->nodes[lo].normal, d0, field->nodes[hi].normal, d1, s);
		rate.x = r.x / h;
		rate.y = r.y / h;
		rate.z = r.z / h;
		value = v;
	}
	/* The field is the Hermite normalized. Its nodes are perpendicular to the tangent and carry the transport rate, so
	   the Hermite between them is perpendicular to the tangent to interpolation accuracy, and its own derivative is the
	   transport rate (which has a component along the tangent): a tangent-plane projection here would remove that
	   component and leave a wrong derivative. Renormalizing removes only the part along the normal. */
	l = QAWS_SQRT((double)value.x * value.x + (double)value.y * value.y + (double)value.z * value.z);
	if (!(l > QAWS_LITERAL(1e-15)))
		return 0;
	out_normal->x = (qaws_scalar)((double)value.x / l);
	out_normal->y = (qaws_scalar)((double)value.y / l);
	out_normal->z = (qaws_scalar)((double)value.z / l);
	projected_dot = (double)out_normal->x * rate.x + (double)out_normal->y * rate.y
		+ (double)out_normal->z * rate.z;
	out_rate->x = (qaws_scalar)(((double)rate.x - projected_dot * out_normal->x) / l);
	out_rate->y = (qaws_scalar)(((double)rate.y - projected_dot * out_normal->y) / l);
	out_rate->z = (qaws_scalar)(((double)rate.z - projected_dot * out_normal->z) / l);
	return 1;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

qaws_status qaws_sweep_rmf_create(
	qaws_sweep_rmf_field** out_field,
	qaws_curve const* path,
	qaws_vec3 const* initial_normal,
	qaws_scalar angular_tolerance,
	qaws_allocator const* allocator)
{
	qaws_sweep_rmf_field* field;
	qaws_sweep_rmf_builder builder;
	qaws_vec3 p0, t0, n0;
	unsigned int span_count, span;
	qaws_status status = QAWS_STATUS_OK;

	if (!out_field)
		return QAWS_STATUS_INVALID_ARGUMENT;
	*out_field = NULL;
	if (!path)
		return QAWS_STATUS_INVALID_ARGUMENT;
	if (qaws_curve_get_dimension(path) != QAWS_DIMENSION_3D)
		return QAWS_STATUS_INVALID_DIMENSION;
	if (!(angular_tolerance == angular_tolerance) || angular_tolerance == (qaws_scalar)INFINITY)
		return QAWS_STATUS_OUT_OF_RANGE;
	if (!(angular_tolerance >= QAWS_SWEEP_RMF_TOLERANCE_MIN) || !(angular_tolerance <= QAWS_SWEEP_RMF_TOLERANCE_MAX))
		return QAWS_STATUS_OUT_OF_RANGE;
	if (initial_normal)
	{
		double n2 = (double)initial_normal->x * initial_normal->x + (double)initial_normal->y * initial_normal->y
			+ (double)initial_normal->z * initial_normal->z;
		if (!(initial_normal->x == initial_normal->x && initial_normal->y == initial_normal->y
			&& initial_normal->z == initial_normal->z) || !(n2 > 0.0) || !(n2 == n2))
			return QAWS_STATUS_INVALID_ARGUMENT;
	}

	field = (qaws_sweep_rmf_field*)qaws_internal_alloc(sizeof(qaws_sweep_rmf_field), allocator);
	if (!field)
		return QAWS_STATUS_ALLOCATION_FAILURE;
	memset(field, 0, sizeof *field);
	field->path = path;
	field->range = qaws_curve_get_parameter_range(path);
	field->tolerance = angular_tolerance;
	field->allocator = allocator;

	if (!rmf_path(&p0, &t0, path, field->range.min_value))
	{
		qaws_internal_dealloc(field, allocator);
		return QAWS_STATUS_DEGENERATE_CURVE;
	}
	if (initial_normal)
	{
		n0 = *initial_normal;
		if (!rmf_perpendicular(&n0, t0))
		{
			/* a normal given along the tangent cannot name a perpendicular start */
			qaws_internal_dealloc(field, allocator);
			return QAWS_STATUS_INVALID_ARGUMENT;
		}
	}
	else
		rmf_default_start(&n0, t0);

	memset(&builder, 0, sizeof builder);
	builder.path = path;
	builder.tolerance = angular_tolerance;
	builder.status = QAWS_STATUS_OK;
	{
		qaws_vec3 tangent_rate;
		if (!rmf_path_derivative(&t0, &tangent_rate, path, field->range.min_value))
		{
			qaws_internal_dealloc(field, allocator);
			return QAWS_STATUS_DEGENERATE_CURVE;
		}
		rmf_builder_push(&builder, t0, n0, field->range.min_value, allocator);
	}
	span_count = qaws_curve_get_span_count(path);
	if (span_count == 0)
		span_count = 1;
	for (span = 0; span < span_count; span++)
	{
		qaws_scalar a, end;
		qaws_vec3 p_a, t_a, n_a, d_a;
		if (qaws_curve_get_span_count(path) == 0)
		{
			a = field->range.min_value;
			end = field->range.max_value;
		}
		else
		{
			a = path->span_boundaries[span];
			end = path->span_boundaries[span + 1];
		}
		if (!(end > a))
			continue;
		if (!rmf_path(&p_a, &t_a, path, a))
		{
			status = QAWS_STATUS_DEGENERATE_CURVE;
			break;
		}
		if (builder.nodes[builder.count - 1].parameter == a)
		{
			n_a = builder.nodes[builder.count - 1].normal;
			d_a = builder.nodes[builder.count - 1].rate;
		}
		else
		{
			qaws_sweep_rmf_node const* previous = &builder.nodes[builder.count - 1];
			qaws_vec3 p_p, t_p, tangent_rate;
			if (!rmf_path(&p_p, &t_p, path, previous->parameter)
				|| !rmf_transport(&n_a, p_p, t_p, previous->normal, p_a, t_a)
				|| !rmf_path_derivative(&t_a, &tangent_rate, path, a))
			{
				status = QAWS_STATUS_DEGENERATE_CURVE;
				break;
			}
			rmf_rate(&d_a, t_a, tangent_rate, n_a);
		}
		rmf_refine(&builder, p_a, t_a, n_a, d_a, a, end, 0, allocator);
		if (builder.status != QAWS_STATUS_OK)
		{
			status = builder.status;
			break;
		}
	}

	if (status == QAWS_STATUS_OK && builder.count < 2)
		status = QAWS_STATUS_DEGENERATE_CURVE;
	if (status != QAWS_STATUS_OK)
	{
		qaws_internal_dealloc(builder.nodes, allocator);
		qaws_internal_dealloc(field, allocator);
		return status;
	}
	field->nodes = builder.nodes;
	field->node_count = builder.count;
	field->node_capacity = builder.capacity;
	*out_field = field;
	return QAWS_STATUS_OK;
}

void qaws_sweep_rmf_destroy(qaws_sweep_rmf_field* field, qaws_allocator const* allocator)
{
	if (!field)
		return;
	qaws_internal_dealloc(field->nodes, allocator);
	qaws_internal_dealloc(field, allocator);
}

qaws_status qaws_sweep_rmf_frame(
	qaws_vec3* out_position,
	qaws_vec3* out_d1,
	qaws_vec3* out_normal,
	qaws_vec3* out_binormal,
	qaws_vec3* out_dnormal,
	qaws_vec3* out_dbinormal,
	qaws_sweep_rmf_field const* field,
	qaws_scalar parameter)
{
	qaws_eval_result_3d e;
	qaws_vec3 tangent, tangent_rate, normal, rate, binormal, binormal_rate;
	double l;

	if (!field || !field->nodes || field->node_count == 0)
		return QAWS_STATUS_INVALID_ARGUMENT;
	if (parameter < field->range.min_value)
		parameter = field->range.min_value;
	if (parameter > field->range.max_value)
		parameter = field->range.max_value;
	memset(&e, 0, sizeof e);
	if (qaws_curve_evaluate_3d(&e, field->path, parameter,
		QAWS_EVAL_FLAG_POSITION | QAWS_EVAL_FLAG_D1) != QAWS_STATUS_OK)
		return QAWS_STATUS_DEGENERATE_CURVE;
	l = QAWS_SQRT((double)e.d1.x * e.d1.x + (double)e.d1.y * e.d1.y + (double)e.d1.z * e.d1.z);
	if (!(l > QAWS_LITERAL(1e-15)))
		return QAWS_STATUS_DEGENERATE_CURVE;
	tangent.x = (qaws_scalar)((double)e.d1.x / l);
	tangent.y = (qaws_scalar)((double)e.d1.y / l);
	tangent.z = (qaws_scalar)((double)e.d1.z / l);
	if (!rmf_field_eval(&normal, &rate, field, parameter))
		return QAWS_STATUS_DEGENERATE_CURVE;
	binormal = rmf_cross(tangent, normal);
	if (!rmf_path_derivative(&tangent, &tangent_rate, field->path, parameter))
		return QAWS_STATUS_DEGENERATE_CURVE;
	{
		qaws_vec3 a = rmf_cross(tangent_rate, normal);
		qaws_vec3 b = rmf_cross(tangent, rate);
		binormal_rate.x = a.x + b.x;
		binormal_rate.y = a.y + b.y;
		binormal_rate.z = a.z + b.z;
	}
	if (out_position)
		*out_position = e.position;
	if (out_d1)
		*out_d1 = e.d1;
	if (out_normal)
		*out_normal = normal;
	if (out_binormal)
		*out_binormal = binormal;
	if (out_dnormal)
		*out_dnormal = rate;
	if (out_dbinormal)
		*out_dbinormal = binormal_rate;
	return QAWS_STATUS_OK;
}
