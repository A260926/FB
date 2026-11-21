//
//  pn_triangle.c
//  c-ray
//
//  PN triangle (curved cubic triangular Bézier patch) implementation.
//
//  Convergence note: ray intersection uses adaptive subdivision of the full
//  (u,v) parameter domain with conservative convex-hull culling, so every
//  geometric branch (including folded patches) retains candidate leaves.
//  Each retained leaf seeds its own damped Newton iteration on the 3x3
//  system P(u,v) = O + tD (never a single one-shot Newton from one guess).
//  Newton converges quadratically for an isolated transverse root, and every
//  accepted root is re-validated against the residual tolerance and the
//  closed parameter domain before it is allowed to tighten hitRecord.
//

#include "../../includes.h"
#include "pn_triangle.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <datatypes/mesh.h>
#include <datatypes/poly.h>
#include <datatypes/hitrecord.h>
#include "lightray.h"
#include <accelerators/bvh.h>

// Bernstein basis term table: coefficient and powers of (w0, w1, w2),
// where w0 = 1-u-v, w1 = u, w2 = v.
struct basis_term {
	double coeff;
	int p0, p1, p2;
};

static const struct basis_term pn_basis[PN_CONTROL_POINT_COUNT] = {
	{ 1.0, 3, 0, 0 }, // b300
	{ 1.0, 0, 3, 0 }, // b030
	{ 1.0, 0, 0, 3 }, // b003
	{ 3.0, 2, 1, 0 }, // b210
	{ 3.0, 1, 2, 0 }, // b120
	{ 3.0, 0, 2, 1 }, // b021
	{ 3.0, 0, 1, 2 }, // b012
	{ 3.0, 1, 0, 2 }, // b102
	{ 3.0, 2, 0, 1 }, // b201
	{ 6.0, 1, 1, 1 }, // b111
};

static inline double pn_w0(double u, double v) { return 1.0 - u - v; }

static void pn_basis_weights(double u, double v, double w[PN_CONTROL_POINT_COUNT]) {
	const double w0 = pn_w0(u, v);
	const double w1 = u;
	const double w2 = v;
	for (int i = 0; i < PN_CONTROL_POINT_COUNT; ++i) {
		double term = pn_basis[i].coeff;
		for (int k = 0; k < pn_basis[i].p0; ++k) term *= w0;
		for (int k = 0; k < pn_basis[i].p1; ++k) term *= w1;
		for (int k = 0; k < pn_basis[i].p2; ++k) term *= w2;
		w[i] = term;
	}
}

// Evaluation and first derivatives in double precision, used for refinement.
static void pn_eval_d(const struct pn_patch *patch, double u, double v,
                      double p[3], double du[3], double dv[3]) {
	const double w0 = pn_w0(u, v);
	const double w1 = u;
	const double w2 = v;
	for (int c = 0; c < 3; ++c) {
		p[c] = 0.0;
		du[c] = 0.0;
		dv[c] = 0.0;
	}
	for (int i = 0; i < PN_CONTROL_POINT_COUNT; ++i) {
		double term = pn_basis[i].coeff;
		for (int k = 0; k < pn_basis[i].p0; ++k) term *= w0;
		for (int k = 0; k < pn_basis[i].p1; ++k) term *= w1;
		for (int k = 0; k < pn_basis[i].p2; ++k) term *= w2;

		// Logarithmic derivatives of w0^p0 w1^p1 w2^p2.
		double dterm_du = 0.0, dterm_dv = 0.0;
		if (pn_basis[i].p0) {
			const double q = pn_basis[i].p0 * term / w0; // dw0/du = dw0/dv = -1
			dterm_du -= q;
			dterm_dv -= q;
		}
		if (pn_basis[i].p1) dterm_du += pn_basis[i].p1 * term / w1; // dw1/du = 1
		if (pn_basis[i].p2) dterm_dv += pn_basis[i].p2 * term / w2; // dw2/dv = 1

		for (int c = 0; c < 3; ++c) {
			const double b = (&patch->b[i].x)[c];
			p[c] += term * b;
			du[c] += dterm_du * b;
			dv[c] += dterm_dv * b;
		}
	}
}

static struct vector pn_eval_f(const struct pn_patch *patch, float u, float v) {
	double w[PN_CONTROL_POINT_COUNT];
	pn_basis_weights(u, v, w);
	struct vector out = { 0 };
	for (int i = 0; i < PN_CONTROL_POINT_COUNT; ++i) {
		out.x += (float)(w[i] * patch->b[i].x);
		out.y += (float)(w[i] * patch->b[i].y);
		out.z += (float)(w[i] * patch->b[i].z);
	}
	return out;
}

static bool finite_vector(const struct vector v) {
	return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static struct boundingBox hull_bbox(const struct vector *points, size_t count) {
	struct boundingBox box = emptyBBox;
	for (size_t i = 0; i < count; ++i) {
		box.min = vec_min(box.min, points[i]);
		box.max = vec_max(box.max, points[i]);
	}
	return box;
}

bool pn_build_patch(const struct mesh *mesh, const struct poly *p, struct pn_patch *out) {
	if (!p->hasNormals)
		return false;

	struct vector n[3];
	for (int c = 0; c < 3; ++c) {
		struct vector raw = mesh->vbuf.normals[p->normalIndex[c]];
		if (!finite_vector(raw))
			return false;
		const float len = vec_length(raw);
		if (!(len > 0.0f) || !isfinite(len))
			return false; // Illegal normal: keep flat behaviour rather than explode.
		n[c] = vec_scale(raw, 1.0f / len);
	}

	const struct vector P[3] = {
		mesh->vbuf.vertices[p->vertexIndex[0]],
		mesh->vbuf.vertices[p->vertexIndex[1]],
		mesh->vbuf.vertices[p->vertexIndex[2]],
	};
	for (int c = 0; c < 3; ++c)
		if (!finite_vector(P[c]))
			return false;

	// Directed edge control point Pi -> Pj:
	// (2 Pi + Pj - dot(Pj-Pi, Ni) Ni) / 3
	struct vector edge[6];
	const int edge_from[6] = { 0, 1, 1, 2, 2, 0 };
	const int edge_to[6]   = { 1, 0, 2, 1, 0, 2 };
	for (int e = 0; e < 6; ++e) {
		const struct vector Pi = P[edge_from[e]];
		const struct vector Pj = P[edge_to[e]];
		const struct vector Ni = n[edge_from[e]];
		const float d = vec_dot(vec_sub(Pj, Pi), Ni);
		edge[e] = vec_scale(vec_sub(vec_add(vec_scale(Pi, 2.0f), Pj), vec_scale(Ni, d)), 1.0f / 3.0f);
	}

	struct vector E = vec_zero();
	for (int e = 0; e < 6; ++e) E = vec_add(E, edge[e]);
	E = vec_scale(E, 1.0f / 6.0f);
	const struct vector V = vec_scale(vec_add(vec_add(P[0], P[1]), P[2]), 1.0f / 3.0f);
	// Interior point: E + (E - V)/2
	const struct vector interior = vec_add(E, vec_scale(vec_sub(E, V), 0.5f));

	struct pn_patch patch = { 0 };
	patch.b[0] = P[0];
	patch.b[1] = P[1];
	patch.b[2] = P[2];
	patch.b[3] = edge[0]; // b210 P0->P1
	patch.b[4] = edge[1]; // b120 P1->P0
	patch.b[5] = edge[2]; // b021 P1->P2
	patch.b[6] = edge[3]; // b012 P2->P1
	patch.b[7] = edge[4]; // b102 P2->P0
	patch.b[8] = edge[5]; // b201 P0->P2
	patch.b[9] = interior;
	patch.bbox = hull_bbox(patch.b, PN_CONTROL_POINT_COUNT);
	const float diag = bboxDiagonal(patch.bbox);
	patch.scale = max(1.0f, diag);
	patch.curved = true;
	*out = patch;
	return true;
}

// Global derivative magnitude bound. A patch derivative is three times a
// convex combination of control-point differences, and every difference is
// bounded by the maximum pairwise control-point distance, hence 6 * Dmax
// bounds both |dP/du| and |dP/dv| over the whole parameter triangle.
static float patch_derivative_bound(const struct pn_patch *patch) {
	float dmax = 0.0f;
	for (int i = 0; i < PN_CONTROL_POINT_COUNT; ++i)
		for (int j = i + 1; j < PN_CONTROL_POINT_COUNT; ++j)
			dmax = max(dmax, vec_distance_to(patch->b[i], patch->b[j]));
	return 6.0f * dmax;
}

// Slab test against an AABB; t is measured on the (not necessarily unit)
// ray direction. Returns false when the box has no overlap with [0, tmax).
static bool ray_aabb(const struct vector o, const struct vector d,
                     const struct boundingBox box, float tmax, float *hit_t) {
	float tmin = 0.0f;
	float tbest = tmax;
	for (int axis = 0; axis < 3; ++axis) {
		const float oc = (&o.x)[axis];
		const float dc = (&d.x)[axis];
		const float lo = (&box.min.x)[axis];
		const float hi = (&box.max.x)[axis];
		if (fabsf(dc) <= FLT_EPSILON) {
			if (oc < lo || oc > hi) return false;
			continue;
		}
		float t1 = (lo - oc) / dc;
		float t2 = (hi - oc) / dc;
		if (t1 > t2) { const float tmp = t1; t1 = t2; t2 = tmp; }
		tmin = max(tmin, t1);
		tbest = min(tbest, t2);
		if (tmin > tbest) return false;
	}
	if (hit_t) *hit_t = tmin;
	return true;
}

struct pn_domain_point { float u, v; };

struct pn_traversal {
	const struct pn_patch *patch;
	const struct lightRay *ray;
	float best_t;
	float best_u, best_v;
	float deriv_bound;
	float leaf_radius;
	unsigned max_depth;
};

// Project a parameter point into the closed domain u >= 0, v >= 0, u+v <= 1.
static void clamp_domain(double *u, double *v) {
	if (*u < 0.0) *u = 0.0;
	if (*v < 0.0) *v = 0.0;
	if (*u + *v > 1.0) {
		const double s = 1.0 / (*u + *v);
		*u *= s;
		*v *= s;
	}
}

// Damped Newton for P(u,v) = O + tD, starting from (u0, v0). A validated
// converged root is returned; transverse roots converge quadratically.
static bool newton_root(const struct pn_traversal *tr, double u0, double v0,
                        float *out_t, float *out_u, float *out_v) {
	const struct pn_patch *patch = tr->patch;
	const double ox = tr->ray->start.x, oy = tr->ray->start.y, oz = tr->ray->start.z;
	const double dx = tr->ray->direction.x, dy = tr->ray->direction.y, dz = tr->ray->direction.z;
	double u = u0, v = v0;
	double t = 0.0;
	const double tol = 2.0e-4 * (double)patch->scale;

	for (int iter = 0; iter < 24; ++iter) {
		double p[3], pu[3], pv[3];
		pn_eval_d(patch, u, v, p, pu, pv);

		// F = P - O - tD, Jacobian columns [Pu, Pv, -D].
		const double fx = p[0] - ox;
		const double fy = p[1] - oy;
		const double fz = p[2] - oz;

		if (dx != 0.0) t = fx / dx;
		else if (dy != 0.0) t = fy / dy;
		else if (dz != 0.0) t = fz / dz;
		const double gx = fx - t * dx;
		const double gy = fy - t * dy;
		const double gz = fz - t * dz;

		// Cramer's rule for J * [du, dv, dt] = -g.
		const double m[3][3] = {
			{ pu[0], pv[0], -dx },
			{ pu[1], pv[1], -dy },
			{ pu[2], pv[2], -dz },
		};
		const double det =
			m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
			m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
			m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
		if (!(fabs(det) > 1e-20))
			return false;
		const double rhs[3] = { -gx, -gy, -gz };
		double delta[3];
		for (int col = 0; col < 3; ++col) {
			double sub[3][3];
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					sub[r][c] = (c == col) ? rhs[r] : m[r][c];
			const double sdet =
				sub[0][0] * (sub[1][1] * sub[2][2] - sub[1][2] * sub[2][1]) -
				sub[0][1] * (sub[1][0] * sub[2][2] - sub[1][2] * sub[2][0]) +
				sub[0][2] * (sub[1][0] * sub[2][1] - sub[1][1] * sub[2][0]);
			delta[col] = sdet / det;
		}

		// Damped step so a wild early iteration cannot jump across the patch.
		double step = 1.0;
		double nu = u + step * delta[0];
		double nv = v + step * delta[1];
		for (int back = 0; back < 8 && (nu < -0.2 || nv < -0.2 || nu + nv > 1.2); ++back) {
			step *= 0.5;
			nu = u + step * delta[0];
			nv = v + step * delta[1];
		}
		clamp_domain(&nu, &nv);
		const double nt = t + step * delta[2];
		const double moved = fabs(nu - u) + fabs(nv - v) +
			fabs(nt - t) / max(1.0, fabs(dx) + fabs(dy) + fabs(dz));
		u = nu;
		v = nv;
		t = nt;
		if (moved < 1e-13)
			break;
	}

	// Final validation against the actual surface, independent of Newton state.
	double p[3], pu[3], pv[3];
	pn_eval_d(patch, u, v, p, pu, pv);
	const double rv[3] = { p[0] - (ox + t * dx), p[1] - (oy + t * dy), p[2] - (oz + t * dz) };
	const double residual = sqrt(rv[0] * rv[0] + rv[1] * rv[1] + rv[2] * rv[2]);
	const double eps = 1e-5;
	const bool in_domain = u >= -eps && v >= -eps && (u + v) <= 1.0 + eps;
	if (!in_domain || t < 0.0 || t >= (double)tr->best_t || residual > tol)
		return false;
	clamp_domain(&u, &v);

	// Reject degenerate normals (zero-area control net / exact tangency fold).
	const double nxn =
		(pu[1] * pv[2] - pu[2] * pv[1]) * (pu[1] * pv[2] - pu[2] * pv[1]) +
		(pu[2] * pv[0] - pu[0] * pv[2]) * (pu[2] * pv[0] - pu[0] * pv[2]) +
		(pu[0] * pv[1] - pu[1] * pv[0]) * (pu[0] * pv[1] - pu[1] * pv[0]);
	if (!(nxn > 1e-20))
		return false;

	*out_t = (float)t;
	*out_u = (float)u;
	*out_v = (float)v;
	return true;
}

// Keep the nearest validated root together with its face parameters.
static void consider_root(struct pn_traversal *tr, double u0, double v0) {
	float t, u, v;
	if (newton_root(tr, u0, v0, &t, &u, &v) && t < tr->best_t) {
		tr->best_t = t;
		tr->best_u = u;
		tr->best_v = v;
	}
}

// True when (pu,pv) lies inside the parameter triangle (a,b,c).
static bool point_in_cell(const struct pn_domain_point p,
                          const struct pn_domain_point a,
                          const struct pn_domain_point b,
                          const struct pn_domain_point c) {
	const float eps = 1e-5f;
	const float d1 = (p.u - b.u) * (a.v - b.v) - (a.u - b.u) * (p.v - b.v);
	const float d2 = (p.u - c.u) * (b.v - c.v) - (b.u - c.u) * (p.v - c.v);
	const float d3 = (p.u - a.u) * (c.v - a.v) - (c.u - a.u) * (p.v - a.v);
	const bool neg = (d1 < -eps) || (d2 < -eps) || (d3 < -eps);
	const bool pos = (d1 > eps) || (d2 > eps) || (d3 > eps);
	return !(neg && pos);
}

// Conservative AABB of the patch over a parameter triangle: surface at the
// centroid plus a derivative-bound margin, valid for every point inside.
static struct boundingBox leaf_bounds(const struct pn_traversal *tr,
                                      const struct pn_domain_point a,
                                      const struct pn_domain_point b,
                                      const struct pn_domain_point c) {
	const float cu = (a.u + b.u + c.u) / 3.0f;
	const float cv = (a.v + b.v + c.v) / 3.0f;
	const struct vector center = pn_eval_f(tr->patch, cu, cv);
	const float ra = hypotf(a.u - cu, a.v - cv);
	const float rb = hypotf(b.u - cu, b.v - cv);
	const float rc = hypotf(c.u - cu, c.v - cv);
	const float radius = tr->deriv_bound * max(ra, max(rb, rc)) + 1e-6f * tr->patch->scale;
	struct boundingBox box;
	box.min = vec_sub(center, (struct vector){ radius, radius, radius });
	box.max = vec_add(center, (struct vector){ radius, radius, radius });
	return box;
}

static void subdivide(struct pn_traversal *tr,
                      const struct pn_domain_point a,
                      const struct pn_domain_point b,
                      const struct pn_domain_point c,
                      unsigned depth) {
	const float cu = (a.u + b.u + c.u) / 3.0f;
	const float cv = (a.v + b.v + c.v) / 3.0f;
	const float radius_param = max(hypotf(a.u - cu, a.v - cv),
	                          max(hypotf(b.u - cu, b.v - cv), hypotf(c.u - cu, c.v - cv)));

	const struct boundingBox box = leaf_bounds(tr, a, b, c);
	float box_t;
	if (!ray_aabb(tr->ray->start, tr->ray->direction, box, tr->best_t, &box_t))
		return; // Conservative prune: this region cannot hold a closer root.

	// Once the cell is small enough for the accuracy budget, refine from its
	// centroid. A root that lands inside the cell is owned by it, so the
	// branch can stop; otherwise keep splitting to separate folded branches.
	const bool fine_enough = tr->deriv_bound * radius_param <= tr->leaf_radius;
	if (depth >= 2 && (fine_enough || depth >= tr->max_depth)) {
		float t, u, v;
		if (newton_root(tr, cu, cv, &t, &u, &v) &&
		    t < tr->best_t &&
		    point_in_cell((struct pn_domain_point){ u, v }, a, b, c)) {
			tr->best_t = t;
			tr->best_u = u;
			tr->best_v = v;
			return;
		}
		if (depth >= tr->max_depth) {
			consider_root(tr, cu, cv); // Last resort: still validate globally.
			return;
		}
	}

	const struct pn_domain_point ab = { (a.u + b.u) * 0.5f, (a.v + b.v) * 0.5f };
	const struct pn_domain_point bc = { (b.u + c.u) * 0.5f, (b.v + c.v) * 0.5f };
	const struct pn_domain_point ca = { (c.u + a.u) * 0.5f, (c.v + a.v) * 0.5f };
	subdivide(tr, a, ab, ca, depth + 1);
	subdivide(tr, ab, b, bc, depth + 1);
	subdivide(tr, ca, bc, c, depth + 1);
	subdivide(tr, ab, bc, ca, depth + 1);
}

bool pn_ray_intersect(const struct mesh *mesh,
                      const struct poly *poly,
                      const struct pn_patch *patch,
                      const struct lightRay *ray,
                      struct hitRecord *isect) {
	if (!patch->curved)
		return rayIntersectsWithPolygon(mesh, ray, poly, isect);

	const float tmax = isect->distance;
	struct pn_traversal tr = {
		.patch = patch,
		.ray = ray,
		.best_t = tmax,
		.best_u = 0.0f,
		.best_v = 0.0f,
		.deriv_bound = patch_derivative_bound(patch),
		.leaf_radius = 2.0e-3f * patch->scale,
		.max_depth = 8,
	};

	// Seed 1: the flat-triangle (Moller-Trumbore) parameter point, which is
	// always close for mildly curved patches.
	{
		const struct vector p0 = mesh->vbuf.vertices[poly->vertexIndex[0]];
		const struct vector p1 = mesh->vbuf.vertices[poly->vertexIndex[1]];
		const struct vector p2 = mesh->vbuf.vertices[poly->vertexIndex[2]];
		const struct vector e1 = vec_sub(p0, p1);
		const struct vector e2 = vec_sub(p2, p0);
		const struct vector n = vec_cross(e1, e2);
		const struct vector cv = vec_sub(p0, ray->start);
		const struct vector r = vec_cross(ray->direction, cv);
		const float inv_det = 1.0f / vec_dot(n, ray->direction);
		const float fu = vec_dot(r, e2) * inv_det;
		const float fv = vec_dot(r, e1) * inv_det;
		if (isfinite(fu) && isfinite(fv) && fu >= 0.0f && fv >= 0.0f && fu + fv <= 1.0f)
			consider_root(&tr, fu, fv);
	}

	// Seed 2..N: adaptive full-domain subdivision reaches every branch.
	subdivide(&tr,
	          (struct pn_domain_point){ 0.0f, 0.0f },
	          (struct pn_domain_point){ 1.0f, 0.0f },
	          (struct pn_domain_point){ 0.0f, 1.0f },
	          0);

	if (!(tr.best_t < tmax))
		return false;

	const struct vector hit = alongRay(ray, tr.best_t);

	// Surface normal from the parametric derivatives, then the existing
	// two-sided orientation convention.
	double pd[3], ddu[3], ddv[3];
	pn_eval_d(patch, tr.best_u, tr.best_v, pd, ddu, ddv);
	struct vector normal = vec_cross(
		(struct vector){ (float)ddu[0], (float)ddu[1], (float)ddu[2] },
		(struct vector){ (float)ddv[0], (float)ddv[1], (float)ddv[2] });
	if (vec_dot(ray->direction, normal) >= 0.0f)
		normal = vec_negate(normal);

	isect->uv = (struct coord){ tr.best_u, tr.best_v };
	isect->distance = tr.best_t;
	isect->surfaceNormal = normal;
	isect->hitPoint = hit;
	return true;
}

// -- Per-mesh setup and sparse updates -------------------------------------

struct pn_mesh_data *pn_mesh_setup(const struct mesh *mesh) {
	const size_t face_count = v_arr_len(mesh->polygons);
	const size_t normal_count = v_arr_len(mesh->vbuf.normals);
	if (!face_count)
		return NULL;

	struct pn_mesh_data *pn = calloc(1, sizeof(*pn));
	pn->face_count = face_count;
	pn->normal_count = normal_count;
	pn->patches = calloc(face_count, sizeof(*pn->patches));

	pn->normal_face_offsets = calloc(normal_count + 1, sizeof(size_t));
	for (size_t f = 0; f < face_count; ++f) {
		const struct poly *p = &mesh->polygons[f];
		struct pn_patch patch = { 0 };
		if (pn_build_patch(mesh, p, &patch))
			pn->patches[f] = patch;
		if (p->hasNormals) {
			for (int c = 0; c < 3; ++c)
				pn->normal_face_offsets[p->normalIndex[c] + 1]++;
		}
	}
	for (size_t i = 0; i < normal_count; ++i)
		pn->normal_face_offsets[i + 1] += pn->normal_face_offsets[i];

	pn->normal_face_data = calloc(pn->normal_face_offsets[normal_count], sizeof(size_t));
	size_t *cursor = calloc(normal_count, sizeof(size_t));
	for (size_t f = 0; f < face_count; ++f) {
		const struct poly *p = &mesh->polygons[f];
		if (!p->hasNormals)
			continue;
		for (int c = 0; c < 3; ++c) {
			const size_t ni = p->normalIndex[c];
			pn->normal_face_data[pn->normal_face_offsets[ni] + cursor[ni]] = f;
			cursor[ni]++;
		}
	}
	free(cursor);
	return pn;
}

void pn_mesh_free(struct pn_mesh_data *pn) {
	if (!pn) return;
	free(pn->patches);
	free(pn->normal_face_offsets);
	free(pn->normal_face_data);
	free(pn);
}

struct boundingBox pn_prim_bbox(const struct mesh *mesh, size_t face_index) {
	if (mesh->pn && mesh->pn->patches[face_index].curved)
		return mesh->pn->patches[face_index].bbox;
	const struct poly *p = &mesh->polygons[face_index];
	struct vector v0 = mesh->vbuf.vertices[p->vertexIndex[0]];
	struct vector v1 = mesh->vbuf.vertices[p->vertexIndex[1]];
	struct vector v2 = mesh->vbuf.vertices[p->vertexIndex[2]];
	struct boundingBox box;
	box.min = vec_min(v0, vec_min(v1, v2));
	box.max = vec_max(v0, vec_max(v1, v2));
	return box;
}

struct vector pn_prim_center(const struct mesh *mesh, size_t face_index) {
	const struct poly *p = &mesh->polygons[face_index];
	struct vector v0 = mesh->vbuf.vertices[p->vertexIndex[0]];
	struct vector v1 = mesh->vbuf.vertices[p->vertexIndex[1]];
	struct vector v2 = mesh->vbuf.vertices[p->vertexIndex[2]];
	return vec_get_midpoint(v0, v1, v2);
}

bool pn_mesh_update_normals(struct mesh *mesh,
                            const size_t *indices,
                            const struct cr_vector *values,
                            size_t count) {
	if (!mesh->pn)
		return false;
	if (count == 0)
		return true;
	if (!indices || !values)
		return false;

	// Validate everything first; a rejected call must not change any state.
	for (size_t i = 0; i < count; ++i) {
		if (indices[i] >= mesh->pn->normal_count)
			return false;
		if (!isfinite(values[i].x) || !isfinite(values[i].y) || !isfinite(values[i].z))
			return false;
		if (values[i].x == 0.0f && values[i].y == 0.0f && values[i].z == 0.0f)
			return false;
	}

	// Apply new normals (repeated indices resolve to the last supplied value).
	for (size_t i = 0; i < count; ++i)
		mesh->vbuf.normals[indices[i]] = (struct vector){ values[i].x, values[i].y, values[i].z };

	// Collect the unique set of affected faces through the reverse lookup.
	unsigned char *face_stamp = calloc(mesh->pn->face_count, sizeof(unsigned char));
	size_t *changed_faces = malloc(mesh->pn->face_count * sizeof(size_t));
	size_t changed_count = 0;
	for (size_t i = 0; i < count; ++i) {
		const size_t ni = indices[i];
		for (size_t k = mesh->pn->normal_face_offsets[ni];
		     k < mesh->pn->normal_face_offsets[ni + 1]; ++k) {
			const size_t f = mesh->pn->normal_face_data[k];
			if (!face_stamp[f]) {
				face_stamp[f] = 1;
				changed_faces[changed_count++] = f;
			}
		}
	}

	// Rebuild only the control nets that reference a changed normal.
	for (size_t i = 0; i < changed_count; ++i) {
		const size_t f = changed_faces[i];
		struct pn_patch rebuilt = { 0 };
		if (pn_build_patch(mesh, &mesh->polygons[f], &rebuilt))
			mesh->pn->patches[f] = rebuilt;
	}

	size_t leaf_updates = 0;
	size_t node_updates = 0;
	if (mesh->bvh && changed_count)
		bvh_refit_mesh(mesh->bvh, mesh, changed_faces, changed_count, &leaf_updates, &node_updates);

	mesh->pn->last_stats = (struct pn_update_stats){
		.updated_patches = changed_count,
		.leaf_updates = leaf_updates,
		.node_updates = node_updates,
	};

	free(face_stamp);
	free(changed_faces);
	return true;
}

const struct pn_mesh_data *pn_mesh_data_of(const struct mesh *mesh) {
	return mesh->pn;
}

const struct pn_patch *pn_mesh_get_patch(const struct mesh *mesh, size_t face_index) {
	if (!mesh->pn || face_index >= mesh->pn->face_count)
		return NULL;
	return &mesh->pn->patches[face_index];
}
