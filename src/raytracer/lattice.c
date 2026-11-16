/*
 * Copyright (c) 2021-2022 Wojciech Graj
 *
 * Licensed under the MIT license: https://opensource.org/licenses/MIT
 * Permission is granted to use, copy, modify, and redistribute the work.
 * Full license information available in the project LICENSE file.
 *
 * DESCRIPTION:
 *   Finite integer-lattice instancing of opaque spheres and triangles.
 *
 *   The lattice keeps one read-only BVH over the borrowed prototype objects in
 *   their own local coordinates.  Copies are never materialised: a query walks
 *   only the integer cells that the ray actually traverses, and for each such
 *   cell it visits the constant number of owning cells whose translated
 *   prototype bounds can reach into that cell.  Build work and query memory
 *   therefore depend on the prototype count, never on the number of copies.
 *
 *   Each query is clipped to the lattice domain and to the conservative union
 *   of all translated prototype bounds.  Hits are only accepted inside the
 *   ray-cell currently being traversed, so a prototype that sticks out of its
 *   owning cell is still found from the neighbouring cell it geometrically
 *   reaches, but a distant hit is never attributed to an earlier cell.
 **/

#include "lattice.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "calc.h"
#include "material.h"
#include "mem.h"

/* A small tolerance, in cells, for the owning-cell search.  It makes the
 * closed cell intervals robust against floating-point rounding. */
#define LATTICE_OWNER_SLACK 1e-5

struct LatticeNode {
	v3 corners[2]; /* local-space bounds of the subtree */
	int32_t child[2]; /* subtree node indices (-1 in a leaf) */
	size_t prototype; /* prototype input index (leaf only) */
	bool is_leaf;
};

struct Lattice {
	struct Object **prototypes; /* borrowed read-only pointer table */
	size_t count;
	v3 period;
	int32_t lower[3];
	int32_t upper[3];
	v3 local_min; /* union of the prototypes' local bounds */
	v3 local_max;
	bool has_support;
	struct LatticeNode *nodes; /* complete BVH, 2 * count - 1 nodes */
	size_t num_nodes;
};

struct LatticeQuery {
	bool found;
	float best_distance;
	int32_t best_cell[3];
	size_t best_prototype;
	v3 best_normal;
};

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static bool lattice_finite(const float value)
{
	return isfinite(value) != 0;
}

/* Ray / axis-aligned box slab test in double precision.  Zero direction
 * components are handled explicitly (a directional ray never divides by
 * zero).  The interval [*t_enter, *t_exit] is where the ray overlaps the box;
 * the caller decides how to clip it. */
static bool lattice_aabb_interval(const v3 corners[2], const struct Ray *ray, double *t_enter, double *t_exit)
{
	double enter = -INFINITY;
	double exit = INFINITY;
	size_t axis;
	for (axis = 0; axis < 3; axis++) {
		double origin = (double)ray->point[axis];
		double direction = (double)ray->direction[axis];
		if (direction == 0.0) {
			if (origin < (double)corners[0][axis] || origin > (double)corners[1][axis])
				return false;
			continue;
		}
		double inv = 1.0 / direction;
		double t0 = ((double)corners[0][axis] - origin) * inv;
		double t1 = ((double)corners[1][axis] - origin) * inv;
		if (t0 > t1) {
			double tmp = t0;
			t0 = t1;
			t1 = tmp;
		}
		if (t0 > enter)
			enter = t0;
		if (t1 < exit)
			exit = t1;
		if (enter > exit)
			return false;
	}
	*t_enter = enter;
	*t_exit = exit;
	return true;
}

/* Double-precision variant used for the (possibly huge) lattice support box,
 * where float coordinates would lose whole cells. */
static bool lattice_aabb_interval_double(const double corners[2][3], const struct Ray *ray, double *t_enter, double *t_exit)
{
	double enter = -INFINITY;
	double exit = INFINITY;
	size_t axis;
	for (axis = 0; axis < 3; axis++) {
		double origin = (double)ray->point[axis];
		double direction = (double)ray->direction[axis];
		if (direction == 0.0) {
			if (origin < corners[0][axis] || origin > corners[1][axis])
				return false;
			continue;
		}
		double inv = 1.0 / direction;
		double t0 = (corners[0][axis] - origin) * inv;
		double t1 = (corners[1][axis] - origin) * inv;
		if (t0 > t1) {
			double tmp = t0;
			t0 = t1;
			t1 = tmp;
		}
		if (t0 > enter)
			enter = t0;
		if (t1 < exit)
			exit = t1;
		if (enter > exit)
			return false;
	}
	*t_enter = enter;
	*t_exit = exit;
	return true;
}

static void lattice_union_bounds(v3 corners[2], const v3 other[2])
{
	size_t axis;
	for (axis = 0; axis < 3; axis++) {
		if (other[0][axis] < corners[0][axis])
			corners[0][axis] = other[0][axis];
		if (other[1][axis] > corners[1][axis])
			corners[1][axis] = other[1][axis];
	}
}

static int lattice_order_compare(const size_t *order, const v3 *centroids, const int32_t axis, const size_t a, const size_t b)
{
	size_t ia = order[a], ib = order[b];
	float ca = centroids[ia][axis];
	float cb = centroids[ib][axis];
	if (ca < cb)
		return -1;
	if (ca > cb)
		return 1;
	return (ia > ib) - (ia < ib);
}

static void lattice_order_swap(size_t *order, const size_t a, const size_t b)
{
	size_t tmp = order[a];
	order[a] = order[b];
	order[b] = tmp;
}

/* Heapsort of the prototype order by centroid along one axis.  Kept local so
 * the build needs no qsort context (and therefore no mutable global). */
static void lattice_order_sift(size_t *order, const v3 *centroids, const int32_t axis, const size_t start, const size_t count)
{
	size_t root = start;
	while (root * 2 + 1 < count) {
		size_t child = root * 2 + 1;
		if (child + 1 < count && lattice_order_compare(order, centroids, axis, child, child + 1) < 0)
			child++;
		if (lattice_order_compare(order, centroids, axis, root, child) < 0) {
			lattice_order_swap(order, root, child);
			root = child;
		} else {
			break;
		}
	}
}

static void lattice_order_sort(size_t *order, const size_t count, const v3 *centroids, const int32_t axis)
{
	if (count < 2)
		return;
	size_t start = count / 2;
	while (start > 0) {
		start--;
		lattice_order_sift(order, centroids, axis, start, count);
	}
	size_t end = count;
	while (end > 1) {
		end--;
		lattice_order_swap(order, 0, end);
		lattice_order_sift(order, centroids, axis, 0, end);
	}
}

/* ------------------------------------------------------------------------- */
/* Build                                                                     */
/* ------------------------------------------------------------------------- */

static int32_t lattice_build_node(struct Lattice *lattice, const size_t first, const size_t last, size_t *order, const v3 *centroids, size_t *nodes_used)
{
	int32_t index = (int32_t)*nodes_used;
	(*nodes_used)++;
	struct LatticeNode *node = &lattice->nodes[index];

	v3 corners[2] = {
		{ FLT_MAX, FLT_MAX, FLT_MAX },
		{ -FLT_MAX, -FLT_MAX, -FLT_MAX },
	};
	size_t i;
	for (i = first; i <= last; i++) {
		v3 prototype_corners[2];
		lattice->prototypes[order[i]]->object_data->get_corners(lattice->prototypes[order[i]], prototype_corners);
		lattice_union_bounds(corners, prototype_corners);
	}
	assign3(node->corners[0], corners[0]);
	assign3(node->corners[1], corners[1]);
	node->child[0] = -1;
	node->child[1] = -1;
	node->prototype = order[first];

	if (first == last) {
		node->is_leaf = true;
		return index;
	}

	int32_t axis = X;
	float best_extent = corners[1][X] - corners[0][X];
	if (corners[1][Y] - corners[0][Y] > best_extent) {
		best_extent = corners[1][Y] - corners[0][Y];
		axis = Y;
	}
	if (corners[1][Z] - corners[0][Z] > best_extent)
		axis = Z;

	lattice_order_sort(&order[first], last - first + 1, centroids, axis);

	size_t mid = first + (last - first) / 2;
	int32_t left = lattice_build_node(lattice, first, mid, order, centroids, nodes_used);
	int32_t right = lattice_build_node(lattice, mid + 1, last, order, centroids, nodes_used);
	node = &lattice->nodes[index];
	node->is_leaf = false;
	node->child[0] = left;
	node->child[1] = right;
	return index;
}

struct Lattice *lattice_build(struct Object *const *prototypes, size_t count, const v3 period, const int32_t lower[3], const int32_t upper[3])
{
	size_t i;
	int32_t axis;

	for (axis = 0; axis < 3; axis++) {
		if (!lattice_finite(period[axis]) || period[axis] <= 0.f)
			return NULL;
		if (lower[axis] > upper[axis])
			return NULL;
	}
	if (count && !prototypes)
		return NULL;

	struct Lattice *lattice = safe_calloc(1, sizeof(struct Lattice));
	lattice->count = count;
	lattice->lower[X] = lower[X];
	lattice->lower[Y] = lower[Y];
	lattice->lower[Z] = lower[Z];
	lattice->upper[X] = upper[X];
	lattice->upper[Y] = upper[Y];
	lattice->upper[Z] = upper[Z];
	assign3(lattice->period, period);

	if (count == 0)
		return lattice;

	lattice->prototypes = safe_malloc(sizeof(struct Object *) * count);
	memcpy(lattice->prototypes, prototypes, sizeof(struct Object *) * count);

	v3 local_min = { FLT_MAX, FLT_MAX, FLT_MAX };
	v3 local_max = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (i = 0; i < count; i++) {
		struct Object *prototype = lattice->prototypes[i];
		if (!prototype || !prototype->object_data || !prototype->material)
			goto fail;
		enum ObjectType type = prototype->object_data->type;
		if (type != OBJECT_SPHERE && type != OBJECT_TRIANGLE)
			goto fail;
		if (prototype->material->transparent || prototype->material->emittant)
			goto fail;

		v3 corners[2];
		prototype->object_data->get_corners(prototype, corners);
		for (axis = 0; axis < 3; axis++) {
			if (!lattice_finite(corners[0][axis]) || !lattice_finite(corners[1][axis]))
				goto fail;
			if (corners[0][axis] > corners[1][axis])
				goto fail;
			if (corners[0][axis] < -period[axis] || corners[1][axis] > 2.f * period[axis])
				goto fail;
		}
		v3 union_corners[2] = {
			{ local_min[X], local_min[Y], local_min[Z] },
			{ local_max[X], local_max[Y], local_max[Z] },
		};
		lattice_union_bounds(union_corners, corners);
		assign3(local_min, union_corners[0]);
		assign3(local_max, union_corners[1]);
	}
	assign3(lattice->local_min, local_min);
	assign3(lattice->local_max, local_max);
	lattice->has_support = true;

	size_t *order = safe_malloc(sizeof(size_t) * count);
	v3 *centroids = safe_malloc(sizeof(v3) * count);
	for (i = 0; i < count; i++) {
		order[i] = i;
		v3 corners[2];
		lattice->prototypes[i]->object_data->get_corners(lattice->prototypes[i], corners);
		centroids[i][X] = 0.5f * (corners[0][X] + corners[1][X]);
		centroids[i][Y] = 0.5f * (corners[0][Y] + corners[1][Y]);
		centroids[i][Z] = 0.5f * (corners[0][Z] + corners[1][Z]);
	}

	lattice->num_nodes = count * 2 - 1;
	lattice->nodes = safe_malloc(sizeof(struct LatticeNode) * lattice->num_nodes);
	size_t nodes_used = 0;
	lattice_build_node(lattice, 0, count - 1, order, centroids, &nodes_used);
	free(centroids);
	free(order);
	return lattice;

fail:
	free(lattice->nodes);
	free(lattice->prototypes);
	free(lattice);
	return NULL;
}

void lattice_free(struct Lattice *lattice)
{
	if (!lattice)
		return;
	free(lattice->nodes);
	free(lattice->prototypes);
	free(lattice);
}

struct Object *lattice_prototype(const struct Lattice *lattice, const size_t index)
{
	if (!lattice || index >= lattice->count)
		return NULL;
	return lattice->prototypes[index];
}

void lattice_get_period(const struct Lattice *lattice, v3 period)
{
	if (!lattice) {
		period[X] = 0.f;
		period[Y] = 0.f;
		period[Z] = 0.f;
		return;
	}
	assign3(period, lattice->period);
}

/* ------------------------------------------------------------------------- */
/* Query                                                                     */
/* ------------------------------------------------------------------------- */

static void lattice_query_init(struct LatticeQuery *query)
{
	query->found = false;
	query->best_distance = FLT_MAX;
	query->best_prototype = 0;
	query->best_cell[X] = 0;
	query->best_cell[Y] = 0;
	query->best_cell[Z] = 0;
	query->best_normal[X] = 0.f;
	query->best_normal[Y] = 0.f;
	query->best_normal[Z] = 0.f;
}

static void lattice_stats_reset(struct LatticeStats *stats)
{
	if (!stats)
		return;
	stats->cells = 0;
	stats->prototype_queries = 0;
	stats->primitive_tests = 0;
}

/* Distance ties are broken by cell x, then y, then z, then prototype index. */
static bool lattice_candidate_is_better(const struct LatticeQuery *query, const float distance, const int32_t cell[3], const size_t prototype)
{
	if (!query->found)
		return true;
	if (distance < query->best_distance)
		return true;
	if (distance > query->best_distance)
		return false;
	if (cell[X] != query->best_cell[X])
		return cell[X] < query->best_cell[X];
	if (cell[Y] != query->best_cell[Y])
		return cell[Y] < query->best_cell[Y];
	if (cell[Z] != query->best_cell[Z])
		return cell[Z] < query->best_cell[Z];
	return prototype < query->best_prototype;
}

static void lattice_query_record(struct LatticeQuery *query, const float distance, const int32_t cell[3], const size_t prototype, const v3 normal)
{
	query->found = true;
	query->best_distance = distance;
	query->best_cell[X] = cell[X];
	query->best_cell[Y] = cell[Y];
	query->best_cell[Z] = cell[Z];
	query->best_prototype = prototype;
	assign3(query->best_normal, normal);
}

static bool lattice_node_interval(const struct LatticeNode *node, const struct Ray *ray, double *t_enter, double *t_exit)
{
	return lattice_aabb_interval((const v3 *)node->corners, ray, t_enter, t_exit);
}

/* Closest hit of one prototype tree, in the prototype's local frame.  The
 * window is expressed in the same (translation-invariant) ray parameter. */
static void lattice_bvh_closest(const struct Lattice *lattice, const int32_t node_index, const struct Ray *local_ray, const float window_min, const float window_max, const int32_t owner[3], struct LatticeQuery *query, struct LatticeStats *stats)
{
	const struct LatticeNode *node = &lattice->nodes[node_index];
	if (stats)
		stats->prototype_queries++;

	double t_enter, t_exit;
	if (!lattice_node_interval(node, local_ray, &t_enter, &t_exit))
		return;
	if (t_exit <= (double)window_min || t_enter >= (double)window_max)
		return;
	if (query->found && t_enter > (double)query->best_distance)
		return;

	if (node->is_leaf) {
		if (stats)
			stats->primitive_tests++;
		float distance;
		v3 normal;
		if (object_get_intersection_in_window(lattice->prototypes[node->prototype], local_ray, window_min, window_max, &distance, normal)
			&& lattice_candidate_is_better(query, distance, owner, node->prototype))
			lattice_query_record(query, distance, owner, node->prototype, normal);
		return;
	}

	int32_t first = node->child[0];
	int32_t second = node->child[1];
	double first_enter, first_exit, second_enter, second_exit;
	bool first_hit = lattice_node_interval(&lattice->nodes[first], local_ray, &first_enter, &first_exit);
	bool second_hit = lattice_node_interval(&lattice->nodes[second], local_ray, &second_enter, &second_exit);
	bool first_valid = first_hit && first_exit > (double)window_min && first_enter < (double)window_max;
	bool second_valid = second_hit && second_exit > (double)window_min && second_enter < (double)window_max;
	if (first_valid && second_valid && second_enter < first_enter) {
		int32_t swap_node = first;
		first = second;
		second = swap_node;
		double swap = first_enter;
		first_enter = second_enter;
		second_enter = swap;
		bool swap_valid = first_valid;
		first_valid = second_valid;
		second_valid = swap_valid;
	}
	if (first_valid)
		lattice_bvh_closest(lattice, first, local_ray, window_min, window_max, owner, query, stats);
	if (second_valid && (!query->found || second_enter <= (double)query->best_distance))
		lattice_bvh_closest(lattice, second, local_ray, window_min, window_max, owner, query, stats);
}

static bool lattice_bvh_occluded(const struct Lattice *lattice, const int32_t node_index, const struct Ray *local_ray, const float window_min, const float window_max, struct LatticeStats *stats)
{
	const struct LatticeNode *node = &lattice->nodes[node_index];
	if (stats)
		stats->prototype_queries++;

	double t_enter, t_exit;
	if (!lattice_node_interval(node, local_ray, &t_enter, &t_exit))
		return false;
	if (t_exit <= (double)window_min || t_enter >= (double)window_max)
		return false;

	if (node->is_leaf) {
		if (stats)
			stats->primitive_tests++;
		float distance;
		v3 normal;
		return object_get_intersection_in_window(lattice->prototypes[node->prototype], local_ray, window_min, window_max, &distance, normal);
	}

	int32_t first = node->child[0];
	int32_t second = node->child[1];
	double first_enter, first_exit, second_enter, second_exit;
	bool first_hit = lattice_node_interval(&lattice->nodes[first], local_ray, &first_enter, &first_exit);
	bool second_hit = lattice_node_interval(&lattice->nodes[second], local_ray, &second_enter, &second_exit);
	bool first_valid = first_hit && first_exit > (double)window_min && first_enter < (double)window_max;
	bool second_valid = second_hit && second_exit > (double)window_min && second_enter < (double)window_max;
	if (first_valid && second_valid && second_enter < first_enter) {
		int32_t swap_node = first;
		first = second;
		second = swap_node;
		double swap = first_enter;
		first_enter = second_enter;
		second_enter = swap;
		bool swap_valid = first_valid;
		first_valid = second_valid;
		second_valid = swap_valid;
	}
	if (first_valid && lattice_bvh_occluded(lattice, first, local_ray, window_min, window_max, stats))
		return true;
	if (second_valid && lattice_bvh_occluded(lattice, second, local_ray, window_min, window_max, stats))
		return true;
	return false;
}

/* Owning cells along one axis whose translated bounds can reach the spatial
 * interval [cell * period, (cell + 1) * period].  At most a small constant
 * number of values; the formula follows from
 *   owner * p + local_max >= cell * p   and   owner * p + local_min <= (cell + 1) * p.
 */
static void lattice_owner_range(const struct Lattice *lattice, const int32_t axis, const int64_t cell, int64_t *first, int64_t *last)
{
	double period = (double)lattice->period[axis];
	double lower_bound = (double)cell - (double)lattice->local_max[axis] / period - LATTICE_OWNER_SLACK;
	double upper_bound = (double)cell + 1.0 - (double)lattice->local_min[axis] / period + LATTICE_OWNER_SLACK;
	double lo = ceil(lower_bound);
	double hi = floor(upper_bound);
	if (lo < (double)lattice->lower[axis])
		lo = (double)lattice->lower[axis];
	if (hi > (double)lattice->upper[axis])
		hi = (double)lattice->upper[axis];
	*first = (int64_t)lo;
	*last = (int64_t)hi;
}

static void lattice_make_local_ray(const struct Lattice *lattice, const struct Ray *ray, const int32_t owner[3], struct Ray *local_ray)
{
	assign3(local_ray->direction, ray->direction);
	int32_t axis;
	for (axis = 0; axis < 3; axis++)
		local_ray->point[axis] = ray->point[axis] - (float)((double)owner[axis] * (double)lattice->period[axis]);
}

/* Clip [t_enter, t_exit] to the caller's half-open [tmin, tmax) interval and
 * round the float bounds outward by a small relative epsilon.  This keeps
 * hits that lie exactly on a cell boundary (or on a flat support boundary)
 * from being lost to float rounding, while never admitting a hit outside the
 * caller's query window. */
static void lattice_window(const double t_enter, const double t_exit, const float tmin, const float tmax, float *window_min, float *window_max)
{
	double lo = t_enter > (double)tmin ? t_enter : (double)tmin;
	double hi = t_exit < (double)tmax ? t_exit : (double)tmax;
	if (!(lo <= hi))
		hi = lo;
	double lo_slack = 8.0 * (double)FLT_EPSILON * (fabs(lo) > 1.0 ? fabs(lo) : 1.0);
	double hi_slack = 8.0 * (double)FLT_EPSILON * (fabs(hi) > 1.0 ? fabs(hi) : 1.0);
	float flo = (float)(lo - lo_slack);
	float fhi = (float)(hi + hi_slack);
	if ((double)flo > lo)
		flo = nextafterf(flo, -FLT_MAX);
	if ((double)fhi < hi)
		fhi = nextafterf(fhi, FLT_MAX);
	if (flo < tmin)
		flo = tmin;
	if (fhi > tmax)
		fhi = tmax;
	*window_min = flo;
	*window_max = fhi;
}

static void lattice_visit_cell_closest(const struct Lattice *lattice, const int32_t cell[3], const struct Ray *ray, const float window_min, const float window_max, struct LatticeQuery *query, struct LatticeStats *stats)
{
	int64_t owners[3][2];
	int32_t axis;
	for (axis = 0; axis < 3; axis++) {
		lattice_owner_range(lattice, axis, cell[axis], &owners[axis][0], &owners[axis][1]);
		if (owners[axis][0] > owners[axis][1])
			return;
	}
	int64_t ox, oy, oz;
	for (ox = owners[X][0]; ox <= owners[X][1]; ox++) {
		for (oy = owners[Y][0]; oy <= owners[Y][1]; oy++) {
			for (oz = owners[Z][0]; oz <= owners[Z][1]; oz++) {
				int32_t owner[3] = { (int32_t)ox, (int32_t)oy, (int32_t)oz };
				struct Ray local_ray;
				lattice_make_local_ray(lattice, ray, owner, &local_ray);
				lattice_bvh_closest(lattice, 0, &local_ray, window_min, window_max, owner, query, stats);
			}
		}
	}
}

static bool lattice_visit_cell_occluded(const struct Lattice *lattice, const int32_t cell[3], const struct Ray *ray, const float window_min, const float window_max, struct LatticeStats *stats)
{
	int64_t owners[3][2];
	int32_t axis;
	for (axis = 0; axis < 3; axis++) {
		lattice_owner_range(lattice, axis, cell[axis], &owners[axis][0], &owners[axis][1]);
		if (owners[axis][0] > owners[axis][1])
			return false;
	}
	int64_t ox, oy, oz;
	for (ox = owners[X][0]; ox <= owners[X][1]; ox++) {
		for (oy = owners[Y][0]; oy <= owners[Y][1]; oy++) {
			for (oz = owners[Z][0]; oz <= owners[Z][1]; oz++) {
				int32_t owner[3] = { (int32_t)ox, (int32_t)oy, (int32_t)oz };
				struct Ray local_ray;
				lattice_make_local_ray(lattice, ray, owner, &local_ray);
				if (lattice_bvh_occluded(lattice, 0, &local_ray, window_min, window_max, stats))
					return true;
			}
		}
	}
	return false;
}

/* Integer-cell 3D DDA clipped to the lattice support and to [tmin, tmax).
 * Returns true when the occlusion query found a hit. */
static bool lattice_traverse(const struct Lattice *lattice, const struct Ray *ray, const float tmin, const float tmax, const bool closest_mode, struct LatticeQuery *query, struct LatticeStats *stats)
{
	if (!lattice->has_support)
		return false;

	double support_double[2][3];
	int32_t axis;
	for (axis = 0; axis < 3; axis++) {
		double lo = (double)lattice->lower[axis] * (double)lattice->period[axis] + (double)lattice->local_min[axis];
		double hi = (double)lattice->upper[axis] * (double)lattice->period[axis] + (double)lattice->local_max[axis];
		support_double[0][axis] = lo;
		support_double[1][axis] = hi;
	}

	double support_enter, support_exit;
	if (!lattice_aabb_interval_double((const double(*)[3])support_double, ray, &support_enter, &support_exit))
		return false;
	/* Zero-thickness support (e.g. a triangle lying in a lattice plane) can
	 * be touched at a single ray parameter; keep that point reachable. */
	if (support_enter == support_exit)
		support_exit = nextafter(support_enter, INFINITY);
	/* Pad the entry/exit by a relative epsilon so a ray touching a prototype
	 * exactly on the support boundary is not clipped away by rounding. */
	{
		double pad = 16.0 * (double)FLT_EPSILON * ((fabs(support_enter) > fabs(support_exit) ? fabs(support_enter) : fabs(support_exit)) + 1.0);
		support_enter -= pad;
		support_exit += pad;
	}
	double t0 = support_enter > (double)tmin ? support_enter : (double)tmin;
	double t1 = support_exit < (double)tmax ? support_exit : (double)tmax;
	if (!(t0 < t1))
		return false;

	/* Start exactly at the clipped entry.  A ray starting on a cell corner
	 * resolves to the cell the interval actually occupies because the cell
	 * coordinate is computed from the centre of the first cell interval. */
	double t_start = t0;
	if (!(t_start < t1))
		return false;

	int64_t cell[3];
	double t_max[3];
	int32_t step[3];
	for (axis = 0; axis < 3; axis++) {
		double direction = (double)ray->direction[axis];
		double period = (double)lattice->period[axis];
		double origin = (double)ray->point[axis];
		double coordinate = (origin + direction * t_start) / period;
		cell[axis] = (int64_t)floor(coordinate);
		if (direction > 0.0) {
			step[axis] = 1;
			t_max[axis] = ((double)(cell[axis] + 1) * period - origin) / direction;
		} else if (direction < 0.0) {
			step[axis] = -1;
			t_max[axis] = ((double)cell[axis] * period - origin) / direction;
		} else {
			step[axis] = 0;
			t_max[axis] = INFINITY;
		}
	}

	double t_enter = t_start;
	while (t_enter < t1) {
		double next_enter = t_max[X];
		if (t_max[Y] < next_enter)
			next_enter = t_max[Y];
		if (t_max[Z] < next_enter)
			next_enter = t_max[Z];
		double t_exit = next_enter < t1 ? next_enter : t1;
		if (t_exit < t_enter)
			t_exit = t_enter;

		/* Degenerate (zero-length) intervals still need one visit: a flat
		 * prototype can be touched exactly on the cell boundary. */
		{
			if (stats)
				stats->cells++;
			float window_min, window_max;
			lattice_window(t_enter, t_exit, tmin, tmax, &window_min, &window_max);
			if (window_min < window_max) {
				bool cell_fits = cell[X] >= INT32_MIN && cell[X] <= INT32_MAX
					&& cell[Y] >= INT32_MIN && cell[Y] <= INT32_MAX
					&& cell[Z] >= INT32_MIN && cell[Z] <= INT32_MAX;
				if (cell_fits) {
					int32_t cell32[3] = { (int32_t)cell[X], (int32_t)cell[Y], (int32_t)cell[Z] };
					if (closest_mode) {
						lattice_visit_cell_closest(lattice, cell32, ray, window_min, window_max, query, stats);
					} else if (lattice_visit_cell_occluded(lattice, cell32, ray, window_min, window_max, stats)) {
						return true;
					}
				}
			}
		}

		if (!(next_enter < t1))
			break;
		if (closest_mode && query->found && next_enter > (double)query->best_distance)
			break;

		double new_enter = next_enter;
		if (!(new_enter > t_enter))
			new_enter = nextafter(t_enter, t1);
		for (axis = 0; axis < 3; axis++) {
			if (step[axis] == 0 || t_max[axis] > new_enter)
				continue;
			cell[axis] += step[axis];
			double direction = (double)ray->direction[axis];
			double period = (double)lattice->period[axis];
			double origin = (double)ray->point[axis];
			if (direction > 0.0)
				t_max[axis] = ((double)(cell[axis] + 1) * period - origin) / direction;
			else
				t_max[axis] = ((double)cell[axis] * period - origin) / direction;
		}
		t_enter = new_enter;
	}
	return false;
}

bool lattice_closest(const struct Lattice *lattice, const struct Ray *ray, const float tmin, const float tmax, struct LatticeHit *hit, struct LatticeStats *stats)
{
	struct LatticeQuery query;
	lattice_query_init(&query);
	lattice_stats_reset(stats);
	if (!lattice || !ray || !lattice_finite(tmin) || !lattice_finite(tmax))
		return false;
	if (!(tmin >= 0.f) || !(tmin < tmax))
		return false;

	lattice_traverse(lattice, ray, tmin, tmax, true, &query, stats);
	if (!query.found)
		return false;
	if (hit) {
		hit->prototype = query.best_prototype;
		hit->cell[X] = query.best_cell[X];
		hit->cell[Y] = query.best_cell[Y];
		hit->cell[Z] = query.best_cell[Z];
		hit->distance = query.best_distance;
		assign3(hit->normal, query.best_normal);
	}
	return true;
}

bool lattice_occluded(const struct Lattice *lattice, const struct Ray *ray, const float tmin, const float tmax, struct LatticeStats *stats)
{
	struct LatticeQuery query;
	lattice_query_init(&query);
	lattice_stats_reset(stats);
	if (!lattice || !ray || !lattice_finite(tmin) || !lattice_finite(tmax))
		return false;
	if (!(tmin >= 0.f) || !(tmin < tmax))
		return false;
	return lattice_traverse(lattice, ray, tmin, tmax, false, &query, stats);
}
