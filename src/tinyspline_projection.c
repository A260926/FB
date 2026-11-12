#define TINYSPLINE_EXPORT
#include "tinyspline_projection.h"

#include <stdlib.h> /* malloc, calloc, realloc, free */
#include <math.h>   /* sqrt, fabs, isnan, isinf, INFINITY */
#include <string.h> /* memcpy, memset */
#include <stdio.h>  /* sprintf, used by the TS_RETURN macros */

/* Maximum supported spline order (degree 3). */
#define TS_PROJ_MAX_ORDER 4

/* Components per point are at most four. */
#define TS_PROJ_MAX_DIM 4

/* Safety bound on Bezier subdivision depth. At level 64 an enclosure box is
 * at most 2^-64 (~5.4e-20) of the original piece, orders of magnitude smaller
 * than the smallest admissible epsilon (1e-10 times the bounding-box
 * diagonal), so a depth-capped leaf always satisfies the distance bracket by
 * its box diameter alone. */
#define TS_PROJ_MAX_DEPTH 64

/* Initial capacity of the per-query min-heap; it grows on demand and lives on
 * the heap, so concurrent queries on the same const index stay independent. */
#define TS_PROJ_HEAP_INIT_CAP 128

/* Query point bounds (see ts_projection_index_query). */
#define TS_PROJ_MAX_COORD 1.0e6
#define TS_PROJ_EPS_FACTOR 1.0e-10

/*! @name Internal Structs and Functions
 *
 * @{
 */
/**
 * A single non-zero knot interval, stored as the Bezier control points of the
 * restriction of the spline to that interval. The axis-aligned bounding box
 * of the control polygon is a certified enclosure of the segment: a Bezier
 * curve is contained in the convex hull (and hence in the bounding box) of
 * its control points.
 */
struct tsPPiece
{
	size_t span;              /**< Left index in the original knot vector. */
	tsReal u0, u1;            /**< Interval bounds in the spline's domain. */
	tsReal bez[TS_PROJ_MAX_ORDER * TS_PROJ_MAX_DIM]; /**< Bezier points. */
	tsReal lo[TS_PROJ_MAX_DIM]; /**< Bounding box, lower corner. */
	tsReal hi[TS_PROJ_MAX_DIM]; /**< Bounding box, upper corner. */
};

/**
 * A node of the branch-and-bound search: a Bezier sub-segment of an original
 * piece, given in local parameter [t0, t1] of that piece.
 */
struct tsPNode
{
	size_t piece;             /**< Index in tsProjectionIndexImpl::pieces. */
	int depth;                /**< Subdivision depth relative to the piece. */
	tsReal t0, t1;            /**< Local parameter interval. */
	tsReal lb;                /**< Bounding-box distance lower bound. */
	tsReal bez[TS_PROJ_MAX_ORDER * TS_PROJ_MAX_DIM]; /**< Bezier points. */
	tsReal lo[TS_PROJ_MAX_DIM]; /**< Bounding box, lower corner. */
	tsReal hi[TS_PROJ_MAX_DIM]; /**< Bounding box, upper corner. */
};

struct tsProjectionIndexImpl
{
	size_t deg;               /**< Degree of the indexed spline. */
	size_t dim;               /**< Dimension of the indexed spline. */
	size_t n_ctrlp;           /**< Number of snapshotted control points. */
	size_t n_knots;           /**< Number of snapshotted knots. */
	size_t n_pieces;          /**< Number of non-zero knot intervals. */
	tsReal bbox_diag;         /**< Diagonal of the control point bbox. */
	tsReal min_eps;           /**< Smallest admissible query epsilon. */
	tsReal *ctrlp;            /**< Snapshot of the control points. */
	tsReal *knots;            /**< Snapshot of the knot vector. */
	struct tsPPiece *pieces;  /**< One entry per non-zero interval. */
};

/** Working state of a single query. The pending nodes are kept in a binary
 * min-heap keyed by the certified lower bound, giving O(log n) selection. */
struct tsPQuery
{
	const struct tsProjectionIndexImpl *impl;
	const tsReal *q;          /**< Query point. */
	tsReal eps;               /**< Requested bracket width. */
	struct tsPNode *heap;     /**< Min-heap of pending nodes. */
	int nheap;                /**< Number of entries in the heap. */
	int cap;                  /**< Allocated capacity of the heap. */
	unsigned char *seen;      /**< Pieces that entered refinement. */
	size_t visited;           /**< Number of distinct pieces refined. */

	/* Current best, achieved solution. */
	tsReal upper;             /**< Distance to `best_point`. */
	size_t best_piece;        /**< Ordinal of the winning piece. */
	tsReal best_knot;
	size_t best_span;
	tsReal best_point[TS_PROJ_MAX_DIM];
};

static int
ts_p_is_finite(tsReal v)
{
	return !isnan(v) && !isinf(v);
}

/**
 * Distance from a point to an axis-aligned bounding box. This is a certified
 * lower bound of the distance from the point to every point enclosed by the
 * box (component-wise projection onto the box).
 */
static tsReal
ts_p_box_distance(const tsReal *lo,
                  const tsReal *hi,
                  const tsReal *q,
                  size_t dim)
{
	size_t d;
	tsReal delta, sum = 0.0;
	for (d = 0; d < dim; d++) {
		if (q[d] < lo[d])
			delta = lo[d] - q[d];
		else if (q[d] > hi[d])
			delta = q[d] - hi[d];
		else
			delta = 0.0;
		sum += delta * delta;
	}
	return (tsReal) sqrt(sum);
}

static tsReal
ts_p_point_distance(const tsReal *x,
                    const tsReal *y,
                    size_t dim)
{
	size_t d;
	tsReal sum = 0.0;
	for (d = 0; d < dim; d++)
		sum += (x[d] - y[d]) * (x[d] - y[d]);
	return (tsReal) sqrt(sum);
}

/**
 * Evaluates a Bezier segment (in Bernstein form) at parameter \p t using de
 * Casteljau's algorithm.
 */
static void
ts_p_bezier_eval(const tsReal *bez,
                 size_t deg,
                 size_t dim,
                 tsReal t,
                 tsReal *out)
{
	tsReal work[TS_PROJ_MAX_ORDER * TS_PROJ_MAX_DIM];
	size_t r, i, d;
	tsReal a_hat = (tsReal) (1.0 - t);
	memcpy(work, bez, (deg + 1) * dim * sizeof(tsReal));
	for (r = 1; r <= deg; r++) {
		for (i = 0; i <= deg - r; i++) {
			for (d = 0; d < dim; d++) {
				work[i * dim + d] =
					a_hat * work[i * dim + d] +
					t * work[(i + 1) * dim + d];
			}
		}
	}
	memcpy(out, work, dim * sizeof(tsReal));
}

/**
 * Splits a Bezier segment at parameter \p t. The control points of the left
 * ([0, t]) and the right ([t, 1]) sub-segment are written to \p left and
 * \p right, respectively.
 */
static void
ts_p_bezier_split(const tsReal *bez,
                  size_t deg,
                  size_t dim,
                  tsReal t,
                  tsReal *left,
                  tsReal *right)
{
	tsReal work[TS_PROJ_MAX_ORDER * TS_PROJ_MAX_DIM];
	size_t r, i, d;
	tsReal a_hat = (tsReal) (1.0 - t);
	memcpy(work, bez, (deg + 1) * dim * sizeof(tsReal));
	for (d = 0; d < dim; d++) {
		left[d] = bez[d];
		right[deg * dim + d] = bez[deg * dim + d];
	}
	for (r = 1; r <= deg; r++) {
		for (i = 0; i <= deg - r; i++) {
			for (d = 0; d < dim; d++) {
				work[i * dim + d] =
					a_hat * work[i * dim + d] +
					t * work[(i + 1) * dim + d];
			}
		}
		for (d = 0; d < dim; d++) {
			left[r * dim + d] = work[d];
			right[(deg - r) * dim + d] =
				work[(deg - r) * dim + d];
		}
	}
}

static void
ts_p_box_of(const tsReal *bez,
            size_t deg,
            size_t dim,
            tsReal *lo,
            tsReal *hi)
{
	size_t i, d;
	for (d = 0; d < dim; d++)
		lo[d] = hi[d] = bez[d];
	for (i = 1; i <= deg; i++) {
		for (d = 0; d < dim; d++) {
			tsReal v = bez[i * dim + d];
			if (v < lo[d])
				lo[d] = v;
			if (v > hi[d])
				hi[d] = v;
		}
	}
}
/*! @} */



tsProjectionIndex
ts_projection_index_init(void)
{
	tsProjectionIndex index;
	index.pImpl = NULL;
	return index;
}

tsError
ts_projection_index_new(const tsBSpline *spline,
                        tsProjectionIndex *index,
                        tsStatus *status)
{
	struct tsProjectionIndexImpl *impl = NULL;
	tsBSpline beziers = ts_bspline_init();
	const tsReal *src_ctrlp, *src_knots, *bez_ctrlp;
	size_t deg, dim, order, n_ctrlp, n_knots, k, p, n_pieces;
	tsReal lo[TS_PROJ_MAX_DIM], hi[TS_PROJ_MAX_DIM];
	tsReal diag_sq = 0.0;
	tsError err;

	if (index == NULL)
		TS_RETURN_0(status, TS_INDEX_ERROR, "index is NULL")
	if (spline == NULL || spline->pImpl == NULL)
		TS_RETURN_0(status, TS_INDEX_ERROR, "spline is NULL or empty")

	deg = ts_bspline_degree(spline);
	dim = ts_bspline_dimension(spline);
	order = deg + 1;
	n_ctrlp = ts_bspline_num_control_points(spline);
	n_knots = ts_bspline_num_knots(spline);

	if (dim < 1 || dim > TS_PROJ_MAX_DIM)
		TS_RETURN_1(status, TS_DIM_ZERO,
		            "unsupported dimension: %lu",
		            (unsigned long) dim)
	if (deg < 1 || deg > 3)
		TS_RETURN_1(status, TS_DEG_GE_NCTRLP,
		            "unsupported degree: %lu",
		            (unsigned long) deg)

	/* Clamped splines repeat both domain knots with full multiplicity. */
	src_knots = ts_bspline_knots_ptr(spline);
	if (!ts_knots_equal(src_knots[0], src_knots[deg]) ||
	    !ts_knots_equal(src_knots[n_knots - order],
	                    src_knots[n_knots - 1]))
		TS_RETURN_0(status, TS_MULTIPLICITY,
		            "spline is not clamped")

	src_ctrlp = ts_bspline_control_points_ptr(spline);
	for (k = 0; k < n_ctrlp * dim; k++) {
		if (!ts_p_is_finite(src_ctrlp[k]))
			TS_RETURN_0(status, TS_NO_RESULT,
			            "control point is not finite")
	}
	for (k = 0; k < n_knots; k++) {
		if (!ts_p_is_finite(src_knots[k]))
			TS_RETURN_0(status, TS_NO_RESULT,
			            "knot is not finite")
	}

	/* Control point bounding box and its diagonal. */
	for (k = 0; k < dim; k++)
		lo[k] = hi[k] = src_ctrlp[k];
	for (p = 1; p < n_ctrlp; p++) {
		for (k = 0; k < dim; k++) {
			tsReal v = src_ctrlp[p * dim + k];
			if (v < lo[k])
				lo[k] = v;
			if (v > hi[k])
				hi[k] = v;
		}
	}
	for (k = 0; k < dim; k++) {
		tsReal delta = hi[k] - lo[k];
		diag_sq += delta * delta;
	}

	impl = (struct tsProjectionIndexImpl *)
	       malloc(sizeof(struct tsProjectionIndexImpl));
	if (!impl)
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	memset(impl, 0, sizeof(*impl));
	impl->deg = deg;
	impl->dim = dim;
	impl->n_ctrlp = n_ctrlp;
	impl->n_knots = n_knots;
	impl->bbox_diag = (tsReal) sqrt(diag_sq);
	impl->min_eps = TS_PROJ_EPS_FACTOR *
	                (impl->bbox_diag > 1.0 ? impl->bbox_diag : 1.0);

	/* Independent snapshot of the spline data. */
	impl->ctrlp = (tsReal *) malloc(n_ctrlp * dim * sizeof(tsReal));
	impl->knots = (tsReal *) malloc(n_knots * sizeof(tsReal));
	if (!impl->ctrlp || !impl->knots) {
		free(impl->ctrlp);
		free(impl->knots);
		free(impl);
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	}
	memcpy(impl->ctrlp, src_ctrlp, n_ctrlp * dim * sizeof(tsReal));
	memcpy(impl->knots, src_knots, n_knots * sizeof(tsReal));

	/* Count the non-zero knot intervals. */
	for (k = deg; k < n_knots - order; k++) {
		if (impl->knots[k] < impl->knots[k + 1])
			impl->n_pieces++;
	}
	if (impl->n_pieces == 0) {
		free(impl->ctrlp);
		free(impl->knots);
		free(impl);
		TS_RETURN_0(status, TS_NO_RESULT,
		            "spline has no non-zero knot interval")
	}

	/* Decompose once, at construction time. Queries never redo this. The
	 * library's own decomposition correctly handles full-multiplicity
	 * internal knots (genuine discontinuities): each non-zero interval
	 * becomes one independent Bezier segment and nothing is connected
	 * across a break. */
	err = ts_bspline_to_beziers(spline, &beziers, status);
	if (err) {
		free(impl->ctrlp);
		free(impl->knots);
		free(impl);
		return err;
	}
	n_pieces = ts_bspline_num_control_points(&beziers) / order;
	if (n_pieces != impl->n_pieces) {
		ts_bspline_free(&beziers);
		free(impl->ctrlp);
		free(impl->knots);
		free(impl);
		TS_RETURN_0(status, TS_NO_RESULT,
		            "Bezier decomposition does not match knot intervals")
	}

	impl->pieces = (struct tsPPiece *)
	               malloc(impl->n_pieces * sizeof(struct tsPPiece));
	if (!impl->pieces) {
		ts_bspline_free(&beziers);
		free(impl->ctrlp);
		free(impl->knots);
		free(impl);
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	}
	memset(impl->pieces, 0,
	       impl->n_pieces * sizeof(struct tsPPiece));

	bez_ctrlp = ts_bspline_control_points_ptr(&beziers);
	p = 0;
	for (k = deg; k < n_knots - order; k++) {
		struct tsPPiece *piece;
		if (!(impl->knots[k] < impl->knots[k + 1]))
			continue; /* Full-multiplicity break: no interval. */
		piece = &impl->pieces[p];
		piece->span = k;
		piece->u0 = impl->knots[k];
		piece->u1 = impl->knots[k + 1];
		memcpy(piece->bez,
		       bez_ctrlp + p * order * dim,
		       order * dim * sizeof(tsReal));
		ts_p_box_of(piece->bez, deg, dim, piece->lo, piece->hi);
		p++;
	}
	ts_bspline_free(&beziers);

	ts_projection_index_free(index);
	index->pImpl = impl;
	TS_RETURN_SUCCESS(status)
}

tsError
ts_projection_index_copy(const tsProjectionIndex *src,
                         tsProjectionIndex *dest,
                         tsStatus *status)
{
	struct tsProjectionIndexImpl *copy = NULL;
	const struct tsProjectionIndexImpl *other;

	if (dest == NULL)
		TS_RETURN_0(status, TS_INDEX_ERROR, "dest is NULL")
	if (src == NULL || src->pImpl == NULL)
		TS_RETURN_0(status, TS_INDEX_ERROR, "src is NULL or empty")
	other = src->pImpl;

	copy = (struct tsProjectionIndexImpl *)
	       malloc(sizeof(struct tsProjectionIndexImpl));
	if (!copy)
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	*copy = *other;
	copy->ctrlp = NULL;
	copy->knots = NULL;
	copy->pieces = NULL;

	copy->ctrlp = (tsReal *)
	              malloc(other->n_ctrlp * other->dim * sizeof(tsReal));
	copy->knots = (tsReal *)
	              malloc(other->n_knots * sizeof(tsReal));
	copy->pieces = (struct tsPPiece *)
	               malloc(other->n_pieces * sizeof(struct tsPPiece));
	if (!copy->ctrlp || !copy->knots || !copy->pieces) {
		free(copy->ctrlp);
		free(copy->knots);
		free(copy->pieces);
		free(copy);
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	}
	memcpy(copy->ctrlp, other->ctrlp,
	       other->n_ctrlp * other->dim * sizeof(tsReal));
	memcpy(copy->knots, other->knots,
	       other->n_knots * sizeof(tsReal));
	memcpy(copy->pieces, other->pieces,
	       other->n_pieces * sizeof(struct tsPPiece));

	ts_projection_index_free(dest);
	dest->pImpl = copy;
	TS_RETURN_SUCCESS(status)
}

void
ts_projection_index_free(tsProjectionIndex *index)
{
	if (index == NULL || index->pImpl == NULL)
		return;
	free(index->pImpl->pieces);
	free(index->pImpl->knots);
	free(index->pImpl->ctrlp);
	free(index->pImpl);
	index->pImpl = NULL;
}

/*! @name Query
 *
 * Best-first branch-and-bound over the per-piece Bezier segments. Pending
 * nodes are stored in a binary min-heap keyed by their certified bounding-box
 * lower bound, so the globally most promising region is always expanded
 * first. Every split also achieves a real curve point, which tightens the
 * incumbent upper bound immediately and makes pruning effective after only a
 * few subdivisions.
 *
 * @{
 */
/** Marks a piece as visited and counts it on the first refinement only. */
static void
ts_p_mark_visited(struct tsPQuery *q,
                  size_t piece)
{
	if (!q->seen[piece]) {
		q->seen[piece] = 1;
		q->visited++;
	}
}

/** Records a real curve point as candidate solution. Strict comparison keeps
 * repeated queries deterministic when several points are equidistant. */
static void
ts_p_consider(struct tsPQuery *q,
              size_t piece,
              tsReal t,
              const tsReal *point)
{
	const struct tsPPiece *p = &q->impl->pieces[piece];
	tsReal d = ts_p_point_distance(point, q->q, q->impl->dim);
	if (d < q->upper) {
		q->upper = d;
		q->best_piece = piece;
		q->best_span = p->span;
		q->best_knot = p->u0 + t * (p->u1 - p->u0);
		memcpy(q->best_point, point,
		       q->impl->dim * sizeof(tsReal));
	}
}

/** Inserts a node into the min-heap (smallest lower bound at the root). */
static tsError
ts_p_heap_push(struct tsPQuery *q,
               const struct tsPNode *node)
{
	int i, parent;
	struct tsPNode *grown;
	if (q->nheap == q->cap) {
		size_t ncap = q->cap ? (size_t) q->cap * 2
		                     : TS_PROJ_HEAP_INIT_CAP;
		grown = (struct tsPNode *)
		        realloc(q->heap, ncap * sizeof(struct tsPNode));
		if (!grown)
			return TS_MALLOC;
		q->heap = grown;
		q->cap = (int) ncap;
	}
	i = q->nheap++;
	q->heap[i] = *node;
	/* Sift up; ties do not move, which keeps the traversal deterministic. */
	while (i > 0) {
		parent = (i - 1) / 2;
		if (q->heap[i].lb < q->heap[parent].lb) {
			struct tsPNode tmp = q->heap[i];
			q->heap[i] = q->heap[parent];
			q->heap[parent] = tmp;
			i = parent;
		} else {
			break;
		}
	}
	return TS_SUCCESS;
}

/** Removes and returns the root (smallest lower bound) of the heap. */
static struct tsPNode
ts_p_heap_pop(struct tsPQuery *q)
{
	struct tsPNode root = q->heap[0];
	int i = 0, n;
	q->nheap--;
	n = q->nheap;
	if (n > 0) {
		struct tsPNode tmp;
		q->heap[0] = q->heap[n];
		/* Sift down. */
		for (;;) {
			int left = 2 * i + 1;
			int right = left + 1;
			int smallest = i;
			if (left < n &&
			    q->heap[left].lb < q->heap[smallest].lb)
				smallest = left;
			if (right < n &&
			    q->heap[right].lb < q->heap[smallest].lb)
				smallest = right;
			if (smallest == i)
				break;
			tmp = q->heap[i];
			q->heap[i] = q->heap[smallest];
			q->heap[smallest] = tmp;
			i = smallest;
		}
	}
	return root;
}

/**
 * Achieves the real curve point of a node at the midpoint of its parameter
 * interval. The node's interval is stored in the local parameter of the
 * original piece (not in the local parameter of the node's own, repeatedly
 * split Bezier coefficients), so the point must always be evaluated on the
 * original piece's Bezier representation. This keeps the returned point and
 * knot consistent with the source spline.
 */
static void
ts_p_node_point(const struct tsPQuery *q,
                const struct tsPNode *node,
                tsReal *t_out,
                tsReal *point)
{
	const struct tsPPiece *piece = &q->impl->pieces[node->piece];
	tsReal t = (node->t0 + node->t1) * 0.5;
	ts_p_bezier_eval(piece->bez, q->impl->deg, q->impl->dim, t, point);
	if (t_out)
		*t_out = t;
}

static tsError
ts_p_refine_node(struct tsPQuery *q,
                 const struct tsPNode *node)
{
	const size_t deg = q->impl->deg;
	const size_t dim = q->impl->dim;
	tsReal left[TS_PROJ_MAX_ORDER * TS_PROJ_MAX_DIM];
	tsReal right[TS_PROJ_MAX_ORDER * TS_PROJ_MAX_DIM];
	tsReal llo[TS_PROJ_MAX_DIM], lhi[TS_PROJ_MAX_DIM];
	tsReal rlo[TS_PROJ_MAX_DIM], rhi[TS_PROJ_MAX_DIM];
	tsReal mid, point[TS_PROJ_MAX_DIM];
	struct tsPNode child;
	tsError err;

	ts_p_mark_visited(q, node->piece);

	/* Backstop: at the depth cap the enclosure is far smaller than any
	 * admissible epsilon, so achieving its midpoint already represents the
	 * whole leaf within the requested distance bracket. */
	if (node->depth >= TS_PROJ_MAX_DEPTH) {
		ts_p_node_point(q, node, &mid, point);
		ts_p_consider(q, node->piece, mid, point);
		return TS_SUCCESS;
	}

	mid = (node->t0 + node->t1) * 0.5;
	ts_p_bezier_split(node->bez, deg, dim, 0.5, left, right);
	/* The split midpoint is a real curve point on the original piece. */
	ts_p_node_point(q, node, &mid, point);
	ts_p_consider(q, node->piece, mid, point);
	ts_p_box_of(left, deg, dim, llo, lhi);
	ts_p_box_of(right, deg, dim, rlo, rhi);

	memset(&child, 0, sizeof(child));
	child.piece = node->piece;
	child.depth = node->depth + 1;

	child.t0 = node->t0;
	child.t1 = mid;
	memcpy(child.bez, left, sizeof(left));
	memcpy(child.lo, llo, sizeof(llo));
	memcpy(child.hi, lhi, sizeof(lhi));
	child.lb = ts_p_box_distance(llo, lhi, q->q, dim);
	/* Pruned children enclose points no closer than upper - epsilon and
	 * therefore already satisfy the final lower bound. */
	if (child.lb < q->upper - q->eps) {
		err = ts_p_heap_push(q, &child);
		if (err)
			return err;
	}

	child.t0 = mid;
	child.t1 = node->t1;
	memcpy(child.bez, right, sizeof(right));
	memcpy(child.lo, rlo, sizeof(rlo));
	memcpy(child.hi, rhi, sizeof(rhi));
	child.lb = ts_p_box_distance(rlo, rhi, q->q, dim);
	if (child.lb < q->upper - q->eps) {
		err = ts_p_heap_push(q, &child);
		if (err)
			return err;
	}
	return TS_SUCCESS;
}

/**
 * Greedy local polish on the piece that supplied the incumbent solution. The
 * certified branch-and-bound only guarantees the distance bracket to within
 * epsilon; near a smooth minimum that leaves a tangential (parameter) error
 * of order sqrt(epsilon). Snapping needs an accurate point, so after the
 * global certificate is established we descend the winning piece along the
 * child with the smaller box lower bound and achieve its midpoint at every
 * level. This only ever considers real points on the same piece whose
 * distance is no larger, so it tightens the incumbent without invalidating
 * the certified lower bound. The descent is deterministic and cheap.
 */
static void
ts_p_polish(struct tsPQuery *q,
            size_t piece)
{
	const size_t deg = q->impl->deg;
	const size_t dim = q->impl->dim;
	const struct tsPPiece *ppiece = &q->impl->pieces[piece];
	tsReal cur[TS_PROJ_MAX_ORDER * TS_PROJ_MAX_DIM];
	tsReal t0 = 0.0, t1 = 1.0, mid, point[TS_PROJ_MAX_DIM];
	int depth;

	/* The polish performs local subdivision on this original interval, so
	 * it counts as a visited piece. */
	ts_p_mark_visited(q, piece);
	memcpy(cur, ppiece->bez, sizeof(cur));
	for (depth = 0; depth < TS_PROJ_MAX_DEPTH; depth++) {
		tsReal left[TS_PROJ_MAX_ORDER * TS_PROJ_MAX_DIM];
		tsReal right[TS_PROJ_MAX_ORDER * TS_PROJ_MAX_DIM];
		tsReal llo[TS_PROJ_MAX_DIM], lhi[TS_PROJ_MAX_DIM];
		tsReal rlo[TS_PROJ_MAX_DIM], rhi[TS_PROJ_MAX_DIM];
		tsReal llb, rlb;

		mid = (t0 + t1) * 0.5;
		/* t0/t1 are absolute in the original piece's local parameter,
		 * so evaluate on the original piece, not on the reparametrized
		 * sub-segment `cur`. */
		ts_p_bezier_eval(ppiece->bez, deg, dim, mid, point);
		ts_p_consider(q, piece, mid, point);

		ts_p_bezier_split(cur, deg, dim, 0.5, left, right);
		ts_p_box_of(left, deg, dim, llo, lhi);
		ts_p_box_of(right, deg, dim, rlo, rhi);
		llb = ts_p_box_distance(llo, lhi, q->q, dim);
		rlb = ts_p_box_distance(rlo, rhi, q->q, dim);

		if (llb <= rlb) {
			memcpy(cur, left, sizeof(cur));
			t1 = mid;
		} else {
			memcpy(cur, right, sizeof(cur));
			t0 = mid;
		}
	}
	/* Final achieved point of the deepest interval. */
	mid = (t0 + t1) * 0.5;
	ts_p_bezier_eval(ppiece->bez, deg, dim, mid, point);
	ts_p_consider(q, piece, mid, point);
}

tsError
ts_projection_index_query(const tsProjectionIndex *index,
                          const tsReal *point,
                          size_t dimension,
                          tsReal epsilon,
                          tsProjection *result,
                          tsStatus *status)
{
	const struct tsProjectionIndexImpl *impl;
	struct tsPQuery q;
	tsReal start[TS_PROJ_MAX_DIM];
	tsReal lower;
	size_t i, d;
	tsError err;

	if (index == NULL || index->pImpl == NULL)
		TS_RETURN_0(status, TS_INDEX_ERROR, "index is NULL or empty")
	if (point == NULL || result == NULL)
		TS_RETURN_0(status, TS_INDEX_ERROR,
		            "query point or result is NULL")
	impl = index->pImpl;
	if (dimension != impl->dim)
		TS_RETURN_2(status, TS_LCTRLP_DIM_MISMATCH,
		            "dimension (%lu) does not match index dimension (%lu)",
		            (unsigned long) dimension,
		            (unsigned long) impl->dim)
	if (!ts_p_is_finite(epsilon) || epsilon < impl->min_eps)
		TS_RETURN_1(status, TS_NO_RESULT,
		            "epsilon must be finite and at least %.3e",
		            (double) impl->min_eps)
	for (d = 0; d < dimension; d++) {
		if (!ts_p_is_finite(point[d]) ||
		    fabs(point[d]) > TS_PROJ_MAX_COORD)
			TS_RETURN_0(status, TS_NO_RESULT,
			            "query point component is not finite or out of range")
	}

	memset(&q, 0, sizeof(q));
	q.impl = impl;
	q.q = point;
	q.eps = epsilon;
	q.heap = NULL;
	q.nheap = 0;
	q.cap = 0;
	q.visited = 0;
	q.upper = (tsReal) INFINITY;
	q.seen = (unsigned char *) calloc(impl->n_pieces, 1);
	if (!q.seen)
		TS_RETURN_0(status, TS_MALLOC, "out of memory")

	/* Seed the incumbent with the real start point of the first piece so
	 * that pruning has a finite upper bound from the first iteration. */
	ts_p_bezier_eval(impl->pieces[0].bez, impl->deg, impl->dim,
	                 0.0, start);
	ts_p_consider(&q, 0, 0.0, start);

	/* Insert the top-level pieces, pruning from the box lower bound. This
	 * is where the global distance bound avoids rescanning the curve. */
	for (i = 0; i < impl->n_pieces; i++) {
		const struct tsPPiece *piece = &impl->pieces[i];
		struct tsPNode root;
		tsReal lb = ts_p_box_distance(piece->lo, piece->hi,
		                              point, impl->dim);
		if (lb >= q.upper - epsilon)
			continue; /* Cannot improve the bracket: no local work. */
		memset(&root, 0, sizeof(root));
		root.piece = i;
		root.depth = 0;
		root.t0 = 0.0;
		root.t1 = 1.0;
		root.lb = lb;
		memcpy(root.bez, piece->bez, sizeof(piece->bez));
		memcpy(root.lo, piece->lo, sizeof(piece->lo));
		memcpy(root.hi, piece->hi, sizeof(piece->hi));
		err = ts_p_heap_push(&q, &root);
		if (err) {
			free(q.seen);
			free(q.heap);
			TS_RETURN_0(status, err, "out of memory")
		}
	}

	/* Best-first refinement. Because the heap is ordered by lower bound,
	 * once the smallest pending bound cannot improve the incumbent, neither
	 * can any other node and the search is certified complete. */
	while (q.nheap > 0) {
		struct tsPNode node = ts_p_heap_pop(&q);
		if (node.lb >= q.upper - epsilon)
			break;
		err = ts_p_refine_node(&q, &node);
		if (err) {
			free(q.seen);
			free(q.heap);
			TS_RETURN_0(status, err, "out of memory")
		}
	}

	/* Polish the winning piece for an accurate snap point. This can only
	 * lower the incumbent, so the certified lower bound computed below
	 * remains valid against every other region. */
	ts_p_polish(&q, q.best_piece);

	free(q.seen);
	free(q.heap);

	/* Every remaining or pruned node encloses points whose distance is at
	 * least upper - epsilon, so upper - epsilon is a certified global lower
	 * bound of the true minimum. */
	lower = q.upper - epsilon;
	if (lower < 0.0)
		lower = 0.0;

	memset(result, 0, sizeof(*result));
	result->knot = q.best_knot;
	result->span_index = q.best_span;
	for (d = 0; d < impl->dim; d++)
		result->point[d] = q.best_point[d];
	/* Recompute from the returned point so that upper equals its Euclidean
	 * distance to the query point exactly. */
	result->distance_upper = ts_p_point_distance(q.best_point, point,
	                                             impl->dim);
	result->distance_lower = lower;
	result->visited_pieces = q.visited;

	TS_RETURN_SUCCESS(status)
}
/*! @} */
