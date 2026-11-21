/* c-ray-fast - projective instancing
 * Copyright (C) 2006-2021 John Tsiombikas <nuclear@member.fsf.org>
 *
 * You are free to use, modify and redistribute this program under the terms
 * of the GNU General Public License v3 or later. See COPYING for details.
 *
 * Implementation notes
 * --------------------
 * A world ray R(t) = O + tD is pushed through the inverse instance matrix M^-1
 * into homogeneous local coordinates
 *
 *     (Xh(t), Yh(t), Zh(t), Wh(t)) = (A0 + A1 t, ..., A3 + A3d t)
 *
 * and then divided: local point L(t) = (Xh/Wh, Yh/Wh, Zh/Wh). A local sphere
 * (center C, radius r) is hit whenever |L(t) - C|^2 = r^2. Multiplying by
 * Wh^2 (which vanishes only at the pole) turns this into an ordinary quadratic
 * in the *world* parameter t, so local root order never has to match world
 * order. The single pole t_p = -A3/A3d, if it falls inside the query segment,
 * splits it into two intervals that are solved independently; the pole itself
 * never yields a finite surface point and is excluded.
 *
 * Every prototype owns one octree over its local spheres. The mapped ray is
 * traversed through that shared tree per instance, so a query never scans all
 * the prototype's spheres.
 */
#include "projective.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define PROJ_OCT_MAX_OBJ	64
#define PROJ_OCT_MAX_DEPTH	9
#define TOP_OCT_MAX_OBJ		16
#define TOP_OCT_MAX_DEPTH	8
#define TRAV_STACK_SIZE		128

/* accumulated scene data */
static struct prototype *protos;
static int num_proto, max_proto;
static struct instance *insts;
static int num_inst, max_inst;

/* material table binding, refreshed in prepare_scene */
static struct material *bound_mtl;
static int bound_num_mtl;

/* bounded-instance top-level spatial tree */
struct topnode {
	struct aabox aabb;
	int *inst;			/* leaf: instance indices, NULL on internal nodes */
	int num_inst;
	struct topnode *sub[8];
};
static struct topnode *toproot;
static int *unbounded_list;		/* indices of cross-pole instances */
static int num_unbounded;

/* ------------------------------------------------------------------ */
/* small matrix helpers                                                */
/* ------------------------------------------------------------------ */

static int finite_mat(const double *m)
{
	int i;
	for(i = 0; i < 16; i++) {
		if(!isfinite(m[i])) return 0;
	}
	return 1;
}

/* invert a 4x4 row-major matrix, returns 0 on success, -1 when singular */
static int mat4_invert(const double *m, double *out)
{
	double inv[16];
	double det;

	inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] +
	           m[9]*m[7]*m[14]  + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] -
	           m[8]*m[7]*m[14]  - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] +
	           m[8]*m[7]*m[13]  + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] -
	           m[8]*m[6]*m[13]  - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] -
	           m[9]*m[3]*m[14]  - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] +
	           m[8]*m[3]*m[14]  + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] -
	           m[8]*m[3]*m[13]  - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] +
	           m[8]*m[2]*m[13]  + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] +
	           m[5]*m[3]*m[14]  + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
	inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] -
	           m[4]*m[3]*m[14]  - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
	inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] +
	           m[4]*m[3]*m[13]  + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] -
	           m[4]*m[2]*m[13]  - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
	inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] -
	           m[5]*m[3]*m[10]  - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
	inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] +
	           m[4]*m[3]*m[10]  + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] -
	           m[4]*m[3]*m[9]   - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
	inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] +
	           m[4]*m[2]*m[9]   + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

	det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if(det == 0.0 || !isfinite(det)) return -1;

	{
		int i;
		double idet = 1.0 / det;
		for(i = 0; i < 16; i++) out[i] = inv[i] * idet;
	}
	if(!finite_mat(out)) return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* local prototype octree                                              */
/* ------------------------------------------------------------------ */

static void free_poct(struct poctnode *node)
{
	int i;
	if(!node) return;
	for(i = 0; i < 8; i++) free_poct(node->sub[i]);
	free(node->sph);
	free(node);
}

static int box_contains_sphere(const struct aabox *box, const struct sphere *sph)
{
	struct vec3 p, b, c;
	double dmax;

	c.x = (box->vmin.x + box->vmax.x) * 0.5;
	c.y = (box->vmin.y + box->vmax.y) * 0.5;
	c.z = (box->vmin.z + box->vmax.z) * 0.5;
	b.x = (box->vmax.x - box->vmin.x) * 0.5;
	b.y = (box->vmax.y - box->vmin.y) * 0.5;
	b.z = (box->vmax.z - box->vmin.z) * 0.5;

	p.x = fabs(sph->pos.x - c.x) - b.x;
	p.y = fabs(sph->pos.y - c.y) - b.y;
	p.z = fabs(sph->pos.z - c.z) - b.z;

	if(p.x > 0.0 && p.y > 0.0 && p.z > 0.0) {
		return p.x * p.x + p.y * p.y + p.z * p.z <= sph->rad * sph->rad;
	}
	dmax = MAX(p.x, p.y);
	if(p.z > dmax) dmax = p.z;
	return dmax <= sph->rad;
}

static int populate_poct(struct poctnode *node, const struct prototype *proto)
{
	int i;
	for(i = 0; i < proto->num_sph; i++) {
		if(box_contains_sphere(&node->aabb, &proto->sph[i].sph)) {
			node->sph[node->num_sph++] = i;
		}
	}
	return node->num_sph;
}

static int build_poct(struct poctnode *node, const struct prototype *proto, int depth)
{
	int i;
	struct poctnode *sub;
	double hx, hy, hz;

	if(node->num_sph <= PROJ_OCT_MAX_OBJ || depth >= PROJ_OCT_MAX_DEPTH) {
		return 0;
	}

	hx = (node->aabb.vmin.x + node->aabb.vmax.x) * 0.5;
	hy = (node->aabb.vmin.y + node->aabb.vmax.y) * 0.5;
	hz = (node->aabb.vmin.z + node->aabb.vmax.z) * 0.5;

	for(i = 0; i < 8; i++) {
		sub = malloc(sizeof *sub);
		if(!sub) return -1;
		sub->sph = malloc(node->num_sph * sizeof *sub->sph);
		if(!sub->sph) {
			free(sub);
			return -1;
		}
		sub->num_sph = 0;
		memset(sub->sub, 0, sizeof sub->sub);

		sub->aabb.vmin.x = LERP(node->aabb.vmin.x, hx, i & 1);
		sub->aabb.vmax.x = LERP(hx, node->aabb.vmax.x, i & 1);
		sub->aabb.vmin.y = LERP(node->aabb.vmin.y, hy, (i >> 1) & 1);
		sub->aabb.vmax.y = LERP(hy, node->aabb.vmax.y, (i >> 1) & 1);
		sub->aabb.vmin.z = LERP(node->aabb.vmin.z, hz, (i >> 2) & 1);
		sub->aabb.vmax.z = LERP(hz, node->aabb.vmax.z, (i >> 2) & 1);

		if(!populate_poct(sub, proto)) {
			free(sub->sph);
			free(sub);
			sub = 0;
		} else if(build_poct(sub, proto, depth + 1) == -1) {
			free_poct(sub);
			return -1;
		}
		node->sub[i] = sub;
	}

	free(node->sph);
	node->sph = 0;
	node->num_sph = 0;
	return 0;
}

struct map_ray {
	struct vec3 a0;		/* homogeneous local point at t = 0 */
	struct vec3 ad;		/* homogeneous local direction slope */
	double w0, wd;		/* homogeneous denominator and slope */
};

struct branch_ctx {
	const struct instance *inst;
	const struct prototype *proto;
	const struct ray *wray;
	struct map_ray mr;
	double tlo, thi;		/* one side of the pole */
	int has_pole;
	struct spoint *best;	/* NULL for occlusion queries */
	struct proj_query_stats *st;
};

/* slab culling of a local box against the rational mapped ray. The local
 * coordinate along axis k is Lk(t) = (n0 + nd t)/(w0 + wd t); on a pole-free
 * interval the denominator keeps a constant sign, so each slab plane turns
 * into a linear inequality in the world parameter t. Returns the clipped
 * t interval inside [tlo, thi].
 */
static int local_box_interval(const struct aabox *box, const struct map_ray *mr,
		double tlo, double thi, double *t0out, double *t1out)
{
	double n0[3], nd[3], lo_b[3], hi_b[3];
	double tm = 0.5 * (tlo + thi);
	double wm = mr->w0 + mr->wd * tm;
	int sgn, k;

	if(wm == 0.0) wm = mr->w0 != 0.0 ? mr->w0 : mr->w0 + mr->wd;
	sgn = wm > 0.0 ? 1 : -1;

	n0[0] = mr->a0.x; n0[1] = mr->a0.y; n0[2] = mr->a0.z;
	nd[0] = mr->ad.x; nd[1] = mr->ad.y; nd[2] = mr->ad.z;
	lo_b[0] = box->vmin.x; lo_b[1] = box->vmin.y; lo_b[2] = box->vmin.z;
	hi_b[0] = box->vmax.x; hi_b[1] = box->vmax.y; hi_b[2] = box->vmax.z;

	for(k = 0; k < 3; k++) {
		/* Lk >= lo_b : sgn * (c0 + c1 t) >= 0 */
		double c0 = n0[k] - lo_b[k] * mr->w0;
		double c1 = nd[k] - lo_b[k] * mr->wd;
		double e0 = hi_b[k] * mr->w0 - n0[k];
		double e1 = hi_b[k] * mr->wd - nd[k];

		/* lower slab */
		c0 *= sgn; c1 *= sgn;
		if(c1 == 0.0) {
			if(c0 < 0.0) return 0;
		} else {
			double t = -c0 / c1;
			if(c1 > 0.0) { if(t > tlo) tlo = t; }
			else { if(t < thi) thi = t; }
		}
		/* upper slab Lk <= hi_b : sgn * (e0 + e1 t) >= 0 where
		 * e0 = hi*w0 - n0, e1 = hi*wd - nd.
		 */
		e0 *= sgn; e1 *= sgn;
		if(e1 == 0.0) {
			if(e0 < 0.0) return 0;
		} else {
			double t = -e0 / e1;
			if(e1 > 0.0) { if(t > tlo) tlo = t; }
			else { if(t < thi) thi = t; }
		}
		if(thi <= tlo) return 0;
	}
	*t0out = tlo;
	*t1out = thi;
	return 1;
}

/* ------------------------------------------------------------------ */
/* per-instance ray mapping and sphere quadratic                       */
/* ------------------------------------------------------------------ */

static void map_world_ray(const struct instance *inst, const struct ray *ray,
		struct map_ray *mr)
{
	const double *im = inst->invm;
	double ox = ray->orig.x, oy = ray->orig.y, oz = ray->orig.z;
	double dx = ray->dir.x, dy = ray->dir.y, dz = ray->dir.z;

	mr->a0.x = im[0]*ox + im[1]*oy + im[2]*oz  + im[3];
	mr->a0.y = im[4]*ox + im[5]*oy + im[6]*oz  + im[7];
	mr->a0.z = im[8]*ox + im[9]*oy + im[10]*oz + im[11];
	mr->w0   = im[12]*ox + im[13]*oy + im[14]*oz + im[15];

	mr->ad.x = im[0]*dx + im[1]*dy + im[2]*dz;
	mr->ad.y = im[4]*dx + im[5]*dy + im[6]*dz;
	mr->ad.z = im[8]*dx + im[9]*dy + im[10]*dz;
	mr->wd   = im[12]*dx + im[13]*dy + im[14]*dz;
}

/* evaluate one local sphere on one pole-free interval of a mapped ray */
static int branch_sphere(const struct branch_ctx *bc, const struct sphere *sph,
		double *hit_t)
{
	const struct map_ray *mr = &bc->mr;
	double px, py, pz, ax, ay, az;
	double A, B, C, disc, sq, r1, r2;
	double w0 = mr->w0, wd = mr->wd;
	double tlo = bc->tlo, thi = bc->thi;
	double rad = sph->rad;

	/* P(t) = A0 - C*w0 + (Ad - C*wd) t ; Wh(t) = w0 + wd t */
	px = mr->a0.x - sph->pos.x * w0;
	py = mr->a0.y - sph->pos.y * w0;
	pz = mr->a0.z - sph->pos.z * w0;
	ax = mr->ad.x - sph->pos.x * wd;
	ay = mr->ad.y - sph->pos.y * wd;
	az = mr->ad.z - sph->pos.z * wd;

	A = ax*ax + ay*ay + az*az;
	B = 2.0 * (px*ax + py*ay + pz*az);
	C = px*px + py*py + pz*pz - rad*rad * w0*w0;
	A -= rad*rad * wd*wd;
	B -= 2.0 * rad*rad * w0*wd;

	if(A == 0.0) {
		if(B == 0.0) return 0;
		r1 = -C / B;
		r2 = r1;
	} else {
		disc = B*B - 4.0*A*C;
		if(disc < 0.0) return 0;
		sq = sqrt(disc);
		r1 = (-B - sq) / (2.0 * A);
		r2 = (-B + sq) / (2.0 * A);
	}

	if(bc->has_pole) {
		/* The polynomial was multiplied by Wh^2, but for an invertible map a
		 * finite ray point never maps to the all-zero homogeneous vector, so
		 * no real root sits exactly on the pole. The closed interval honors
		 * the ERR_MARGIN boundary; any numerical root at the pole is still
		 * rejected below by the Wh != 0 test.
		 */
		int ok1 = r1 >= tlo && r1 <= thi;
		int ok2 = r2 >= tlo && r2 <= thi;
		if(!ok1) r1 = DBL_MAX;
		if(!ok2) r2 = DBL_MAX;
		r1 = MIN(r1, r2);
		if(r1 == DBL_MAX) return 0;
	} else {
		/* no pole in the segment, ordinary range test with the error margin */
		if((r1 < ERR_MARGIN && r2 < ERR_MARGIN) || (r1 > 1.0 && r2 > 1.0)) {
			return 0;
		}
		if(r1 < ERR_MARGIN) r1 = r2;
		if(r2 < ERR_MARGIN) r2 = r1;
		r1 = MIN(r1, r2);
		if(r1 < ERR_MARGIN || r1 > 1.0) return 0;
	}

	/* the homogeneous denominator must be finite and nonzero at the hit */
	{
		double w = w0 + wd * r1;
		if(!isfinite(w) || w == 0.0) return 0;
	}

	*hit_t = r1;
	return 1;
}

/* build the world hit point and the outward unit world normal at t */
static void fill_hit(const struct branch_ctx *bc, const struct proto_sphere *ps,
		double t, struct spoint *sp)
{
	const struct map_ray *mr = &bc->mr;
	const struct ray *ray = bc->wray;
	double lx, ly, lz, w, ilen;
	struct vec3 ln, wn;

	w = mr->w0 + mr->wd * t;
	lx = (mr->a0.x + mr->ad.x * t) / w;
	ly = (mr->a0.y + mr->ad.y * t) / w;
	lz = (mr->a0.z + mr->ad.z * t) / w;

	sp->dist = t;
	sp->pos.x = ray->orig.x + ray->dir.x * t;
	sp->pos.y = ray->orig.y + ray->dir.y * t;
	sp->pos.z = ray->orig.z + ray->dir.z * t;

	/* outward normal in local space */
	ln.x = (lx - ps->sph.pos.x) / ps->sph.rad;
	ln.y = (ly - ps->sph.pos.y) / ps->sph.rad;
	ln.z = (lz - ps->sph.pos.z) / ps->sph.rad;

	/* Exact outward world normal. H = M^-1 maps a world point x to local
	 * homogeneous coordinates (Xh, Wh). The local spatial Jacobian is
	 * K = (H33*Wh - Xh*h3) / Wh^2, and the world normal satisfying
	 * n_w.dx = n_L.dX is n_w = K^T n_L. The common 1/Wh^2 factor and the
	 * sign are irrelevant after normalization, and the sign tracks the real
	 * differential, so the result points out of the solid even when the
	 * homogeneous coordinate flips orientation.
	 */
	{
		const double *im = bc->inst->invm;
		double ln_a[3] = {ln.x, ln.y, ln.z};
		double xh[3] = {mr->a0.x + mr->ad.x * t,
				mr->a0.y + mr->ad.y * t,
				mr->a0.z + mr->ad.z * t};
		double xdot = xh[0]*ln_a[0] + xh[1]*ln_a[1] + xh[2]*ln_a[2];
		int i;
		for(i = 0; i < 3; i++) {
			double s = im[0 * 4 + i] * (ln_a[0] * w)
			         + im[1 * 4 + i] * (ln_a[1] * w)
			         + im[2 * 4 + i] * (ln_a[2] * w)
			         - im[12 + i] * xdot;
			if(i == 0) wn.x = s;
			else if(i == 1) wn.y = s;
			else wn.z = s;
		}
	}
	ilen = 1.0 / sqrt(DOT(wn, wn));
	wn.x *= ilen; wn.y *= ilen; wn.z *= ilen;

	sp->normal = wn;
	reflect(&sp->vref, &ray->dir, &sp->normal);
	NORMALIZE(sp->vref);

	sp->obj = 0;
	sp->inst = (struct instance *)bc->inst;
	sp->psph = (struct proto_sphere *)ps;
	sp->mat = bound_mtl && ps->mat_index >= 0 && ps->mat_index < bound_num_mtl ?
		bound_mtl + ps->mat_index : 0;
}

/* traverse the shared local octree over one pole-free interval */
static int traverse_local(const struct poctnode *node, struct branch_ctx *bc)
{
	const struct poctnode *stack[TRAV_STACK_SIZE];
	int nstack = 0;
	int hit_any = 0;

	stack[nstack++] = node;
	while(nstack > 0) {
		const struct poctnode *cur = stack[--nstack];
		double bt0, bt1;

		if(bc->st) bc->st->node_tests++;
		if(!local_box_interval(&cur->aabb, &bc->mr, bc->tlo, bc->thi, &bt0, &bt1)) {
			continue;
		}

		if(cur->sph) {
			int i;
			for(i = 0; i < cur->num_sph; i++) {
				double t;
				int idx = cur->sph[i];
				if(bc->st) bc->st->leaf_tests++;
				if(branch_sphere(bc, &bc->proto->sph[idx].sph, &t)) {
					hit_any = 1;
					if(bc->best) {
						if(!bc->best->psph || t < bc->best->dist) {
							fill_hit(bc, &bc->proto->sph[idx], t, bc->best);
						}
					} else {
						return 1;
					}
				}
			}
		} else {
			int k;
			for(k = 7; k >= 0; k--) {
				if(cur->sub[k] && nstack < TRAV_STACK_SIZE) {
					stack[nstack++] = cur->sub[k];
				}
			}
		}
	}
	return hit_any;
}

/* run both pole intervals (or a single interval when there is no pole) for
 * one instance.
 */
static int intersect_instance(const struct instance *inst, const struct ray *ray,
		struct spoint *best, struct proj_query_stats *st)
{
	struct branch_ctx bc;
	struct prototype *proto = inst->proto;
	double tpole;

	bc.inst = inst;
	bc.proto = proto;
	bc.wray = ray;
	bc.best = best;
	bc.st = st;
	map_world_ray(inst, ray, &bc.mr);

	/* the whole segment lies on the pole plane: every mapped point is at
	 * infinity and no finite surface point exists, so produce no hit rather
	 * than fabricating the pole plane. Tested against inverse-matrix scale
	 * because a numerically computed inverse rarely yields exact zeroes.
	 */
	{
		const double *im = inst->invm;
		double in_scale = 1.0;
		double r3, tol;
		in_scale += MAX(fabs(ray->orig.x), MAX(fabs(ray->orig.y), fabs(ray->orig.z)));
		in_scale += MAX(fabs(ray->dir.x), MAX(fabs(ray->dir.y), fabs(ray->dir.z)));
		r3 = fabs(im[12]) + fabs(im[13]) + fabs(im[14]) + fabs(im[15]);
		tol = 1e-12 * r3 * in_scale;
		if(fabs(bc.mr.w0) <= tol && fabs(bc.mr.wd) <= tol) {
			return 0;
		}
	}

	if(bc.mr.wd != 0.0) {
		tpole = -bc.mr.w0 / bc.mr.wd;
	} else {
		tpole = 0.0;
	}
	bc.has_pole = (bc.mr.wd != 0.0 && tpole > ERR_MARGIN && tpole < 1.0);

	if(!bc.has_pole) {
		bc.tlo = ERR_MARGIN;
		bc.thi = 1.0;
		return traverse_local(proto->tree, &bc);
	}

	{
		int any = 0;
		/* side before the pole */
		bc.tlo = ERR_MARGIN;
		bc.thi = tpole;
		if(traverse_local(proto->tree, &bc)) any = 1;
		if(!best && any) return 1;
		/* side after the pole */
		bc.tlo = tpole;
		bc.thi = 1.0;
		if(traverse_local(proto->tree, &bc)) any = 1;
		return any;
	}
}

/* ------------------------------------------------------------------ */
/* bounded-instance top tree                                           */
/* ------------------------------------------------------------------ */

static void free_top(struct topnode *node)
{
	int i;
	if(!node) return;
	for(i = 0; i < 8; i++) free_top(node->sub[i]);
	free(node->inst);
	free(node);
}

static int boxes_overlap(const struct aabox *a, const struct aabox *b)
{
	return a->vmin.x <= b->vmax.x && a->vmax.x >= b->vmin.x &&
		a->vmin.y <= b->vmax.y && a->vmax.y >= b->vmin.y &&
		a->vmin.z <= b->vmax.z && a->vmax.z >= b->vmin.z;
}

static int populate_top(struct topnode *node)
{
	int i;
	for(i = 0; i < num_inst; i++) {
		if(insts[i].bounded && boxes_overlap(&node->aabb, &insts[i].wbox)) {
			node->inst[node->num_inst++] = i;
		}
	}
	return node->num_inst;
}

static int build_top(struct topnode *node, int depth)
{
	int i;
	struct topnode *sub;
	double hx, hy, hz;

	if(node->num_inst <= TOP_OCT_MAX_OBJ || depth >= TOP_OCT_MAX_DEPTH) {
		return 0;
	}

	hx = (node->aabb.vmin.x + node->aabb.vmax.x) * 0.5;
	hy = (node->aabb.vmin.y + node->aabb.vmax.y) * 0.5;
	hz = (node->aabb.vmin.z + node->aabb.vmax.z) * 0.5;

	for(i = 0; i < 8; i++) {
		int j;
		sub = malloc(sizeof *sub);
		if(!sub) return -1;
		sub->inst = malloc(node->num_inst * sizeof *sub->inst);
		if(!sub->inst) {
			free(sub);
			return -1;
		}
		sub->num_inst = 0;
		memset(sub->sub, 0, sizeof sub->sub);

		sub->aabb.vmin.x = LERP(node->aabb.vmin.x, hx, i & 1);
		sub->aabb.vmax.x = LERP(hx, node->aabb.vmax.x, i & 1);
		sub->aabb.vmin.y = LERP(node->aabb.vmin.y, hy, (i >> 1) & 1);
		sub->aabb.vmax.y = LERP(hy, node->aabb.vmax.y, (i >> 1) & 1);
		sub->aabb.vmin.z = LERP(node->aabb.vmin.z, hz, (i >> 2) & 1);
		sub->aabb.vmax.z = LERP(hz, node->aabb.vmax.z, (i >> 2) & 1);

		for(j = 0; j < node->num_inst; j++) {
			int idx = node->inst[j];
			if(boxes_overlap(&sub->aabb, &insts[idx].wbox)) {
				sub->inst[sub->num_inst++] = idx;
			}
		}

		if(!sub->num_inst) {
			free(sub->inst);
			free(sub);
			sub = 0;
		} else if(build_top(sub, depth + 1) == -1) {
			free_top(sub);
			return -1;
		}
		node->sub[i] = sub;
	}

	free(node->inst);
	node->inst = 0;
	node->num_inst = 0;
	return 0;
}

static int traverse_top(const struct topnode *node, const struct ray *ray,
		struct spoint *best, struct proj_query_stats *st)
{
	const struct topnode *stack[TRAV_STACK_SIZE];
	int nstack = 0, any = 0;

	stack[nstack++] = node;
	while(nstack > 0) {
		const struct topnode *cur = stack[--nstack];
		if(!ray_aabox(&cur->aabb, ray)) continue;

		if(cur->inst) {
			int i;
			for(i = 0; i < cur->num_inst; i++) {
				if(intersect_instance(&insts[cur->inst[i]], ray, best, st)) {
					any = 1;
					if(!best) return 1;
				}
			}
		} else {
			int k;
			for(k = 7; k >= 0; k--) {
				if(cur->sub[k] && nstack < TRAV_STACK_SIZE) {
					stack[nstack++] = cur->sub[k];
				}
			}
		}
	}
	return any;
}

/* ------------------------------------------------------------------ */
/* public scene accumulation                                           */
/* ------------------------------------------------------------------ */

void proj_clear(void)
{
	int i, j;

	for(i = 0; i < num_proto; i++) {
		free(protos[i].name);
		free(protos[i].sph);
		free_poct(protos[i].tree);
	}
	free(protos);
	protos = 0;
	num_proto = max_proto = 0;

	for(j = 0; j < num_inst; j++) free(insts[j].name);
	free(insts);
	insts = 0;
	num_inst = max_inst = 0;

	free_top(toproot);
	toproot = 0;
	free(unbounded_list);
	unbounded_list = 0;
	num_unbounded = 0;

	bound_mtl = 0;
	bound_num_mtl = 0;
}

struct prototype *proj_find_prototype(const char *name)
{
	int i;
	for(i = 0; i < num_proto; i++) {
		if(strcmp(protos[i].name, name) == 0) return protos + i;
	}
	return 0;
}

struct instance *proj_find_instance(const char *name)
{
	int i;
	for(i = 0; i < num_inst; i++) {
		if(strcmp(insts[i].name, name) == 0) return insts + i;
	}
	return 0;
}

int proj_instance_count(void)
{
	return num_inst;
}

int proj_unbounded_count(void)
{
	return num_unbounded;
}

void proj_bind_materials(struct material *mtl, int num_mtl)
{
	bound_mtl = mtl;
	bound_num_mtl = num_mtl;
}

int proj_add_proto_sphere(const char *name, const struct sphere *sph, int mat_index)
{
	struct prototype *p = proj_find_prototype(name);

	if(!p) {
		if(num_proto >= max_proto) {
			int nsz = max_proto ? max_proto * 2 : 8;
			struct prototype *tmp = realloc(protos, nsz * sizeof *tmp);
			if(!tmp) return -1;
			protos = tmp;
			max_proto = nsz;
		}
		p = protos + num_proto++;
		p->name = 0;
		p->sph = 0;
		p->num_sph = p->max_sph = 0;
		p->tree = 0;
		p->bounds.vmin.x = p->bounds.vmin.y = p->bounds.vmin.z = DBL_MAX;
		p->bounds.vmax.x = p->bounds.vmax.y = p->bounds.vmax.z = -DBL_MAX;
		if(!(p->name = malloc(strlen(name) + 1))) return -1;
		strcpy(p->name, name);
	}

	if(p->num_sph >= p->max_sph) {
		int nsz = p->max_sph ? p->max_sph * 2 : 8;
		struct proto_sphere *tmp = realloc(p->sph, nsz * sizeof *tmp);
		if(!tmp) return -1;
		p->sph = tmp;
		p->max_sph = nsz;
	}

	p->sph[p->num_sph].sph = *sph;
	p->sph[p->num_sph].mat_index = mat_index;
	p->num_sph++;
	return 0;
}

int proj_add_instance(const char *name, const char *proto_name, const double m[16])
{
	struct prototype *p;
	struct instance *in;

	if(!finite_mat(m)) return -1;
	{
		double scratch[16];
		/* must be invertible with a finite inverse */
		if(mat4_invert(m, scratch) == -1 || !finite_mat(scratch)) return -1;
	}
	if(proj_find_instance(name)) return -1;	/* instance names are unique */
	if(!(p = proj_find_prototype(proto_name))) return -1; /* must exist already */

	if(num_inst >= max_inst) {
		int nsz = max_inst ? max_inst * 2 : 8;
		struct instance *tmp = realloc(insts, nsz * sizeof *tmp);
		if(!tmp) return -1;
		insts = tmp;
		max_inst = nsz;
	}

	in = insts + num_inst++;
	memset(in, 0, sizeof *in);
	if(!(in->name = malloc(strlen(name) + 1))) return -1;
	strcpy(in->name, name);
	in->proto = p;
	memcpy(in->m, m, 16 * sizeof(double));
	in->bounded = 0;
	return 0;
}

/* ------------------------------------------------------------------ */
/* preparation: shared local trees, bounds, candidate tables           */
/* ------------------------------------------------------------------ */

/* world-space AABB of a bounded instance by mapping the 8 local corners */
static void compute_instance_bounds(struct instance *in)
{
	static const int corner[8][3] = {
		{0,0,0},{1,0,0},{0,1,0},{1,1,0},
		{0,0,1},{1,0,1},{0,1,1},{1,1,1}
	};
	const struct aabox *b = &in->proto->bounds;
	double lo[3] = {b->vmin.x, b->vmin.y, b->vmin.z};
	double hi[3] = {b->vmax.x, b->vmax.y, b->vmax.z};
	double wmin = DBL_MAX, wmax = -DBL_MAX, wabs_max = 0.0;
	double wx[8], wy[8], wz[8], ww[8];
	int k;

	for(k = 0; k < 8; k++) {
		double cx = corner[k][0] ? hi[0] : lo[0];
		double cy = corner[k][1] ? hi[1] : lo[1];
		double cz = corner[k][2] ? hi[2] : lo[2];
		double w = in->m[12]*cx + in->m[13]*cy + in->m[14]*cz + in->m[15];
		ww[k] = w;
		wx[k] = in->m[0]*cx + in->m[1]*cy + in->m[2]*cz + in->m[3];
		wy[k] = in->m[4]*cx + in->m[5]*cy + in->m[6]*cz + in->m[7];
		wz[k] = in->m[8]*cx + in->m[9]*cy + in->m[10]*cz + in->m[11];
		if(w < wmin) wmin = w;
		if(w > wmax) wmax = w;
		if(fabs(w) > wabs_max) wabs_max = fabs(w);
	}

	/* a zero (or near-zero relative to the matrix scale) corner denominator
	 * means the image can reach the pole plane: treat as unbounded.
	 */
	{
		double thr = 1e-12 * MAX(1.0, wabs_max);
		if(!(wmin > thr || wmax < -thr)) {
			in->bounded = 0;
			return;
		}
	}

	in->bounded = 1;
	in->wbox.vmin.x = in->wbox.vmin.y = in->wbox.vmin.z = DBL_MAX;
	in->wbox.vmax.x = in->wbox.vmax.y = in->wbox.vmax.z = -DBL_MAX;
	for(k = 0; k < 8; k++) {
		double iw = 1.0 / ww[k];
		double x = wx[k] * iw, y = wy[k] * iw, z = wz[k] * iw;
		if(x < in->wbox.vmin.x) in->wbox.vmin.x = x;
		if(x > in->wbox.vmax.x) in->wbox.vmax.x = x;
		if(y < in->wbox.vmin.y) in->wbox.vmin.y = y;
		if(y > in->wbox.vmax.y) in->wbox.vmax.y = y;
		if(z < in->wbox.vmin.z) in->wbox.vmin.z = z;
		if(z > in->wbox.vmax.z) in->wbox.vmax.z = z;
	}
}

int proj_prepare(void)
{
	int i;
	struct aabox topbox;

	/* release any indexes from a previous prepare */
	for(i = 0; i < num_proto; i++) free_poct(protos[i].tree);
	free_top(toproot);
	toproot = 0;
	free(unbounded_list);
	unbounded_list = 0;
	num_unbounded = 0;

	/* per-prototype bounds and shared local octrees */
	for(i = 0; i < num_proto; i++) {
		struct prototype *p = protos + i;
		int k;
		p->bounds.vmin.x = p->bounds.vmin.y = p->bounds.vmin.z = DBL_MAX;
		p->bounds.vmax.x = p->bounds.vmax.y = p->bounds.vmax.z = -DBL_MAX;
		for(k = 0; k < p->num_sph; k++) {
			const struct sphere *s = &p->sph[k].sph;
			p->bounds.vmin.x = MIN(p->bounds.vmin.x, s->pos.x - s->rad);
			p->bounds.vmin.y = MIN(p->bounds.vmin.y, s->pos.y - s->rad);
			p->bounds.vmin.z = MIN(p->bounds.vmin.z, s->pos.z - s->rad);
			p->bounds.vmax.x = MAX(p->bounds.vmax.x, s->pos.x + s->rad);
			p->bounds.vmax.y = MAX(p->bounds.vmax.y, s->pos.y + s->rad);
			p->bounds.vmax.z = MAX(p->bounds.vmax.z, s->pos.z + s->rad);
		}

		p->tree = malloc(sizeof *p->tree);
		if(!p->tree) return -1;
		memset(p->tree->sub, 0, sizeof p->tree->sub);
		p->tree->aabb = p->bounds;
		p->tree->sph = malloc((p->num_sph ? p->num_sph : 1) * sizeof *p->tree->sph);
		if(!p->tree->sph) return -1;
		p->tree->num_sph = 0;
		populate_poct(p->tree, p);
		if(build_poct(p->tree, p, 0) == -1) return -1;
	}

	/* inverse matrices, bounds classification and the unbounded table */
	for(i = 0; i < num_inst; i++) {
		if(mat4_invert(insts[i].m, insts[i].invm) == -1) return -1;
		compute_instance_bounds(insts + i);
	}

	unbounded_list = malloc((num_inst ? num_inst : 1) * sizeof *unbounded_list);
	if(!unbounded_list) return -1;
	for(i = 0; i < num_inst; i++) {
		if(!insts[i].bounded) unbounded_list[num_unbounded++] = i;
	}

	/* top-level tree over bounded instances */
	topbox.vmin.x = topbox.vmin.y = topbox.vmin.z = DBL_MAX;
	topbox.vmax.x = topbox.vmax.y = topbox.vmax.z = -DBL_MAX;
	{
		int any_bounded = 0;
		for(i = 0; i < num_inst; i++) {
			if(!insts[i].bounded) continue;
			any_bounded = 1;
			topbox.vmin.x = MIN(topbox.vmin.x, insts[i].wbox.vmin.x);
			topbox.vmin.y = MIN(topbox.vmin.y, insts[i].wbox.vmin.y);
			topbox.vmin.z = MIN(topbox.vmin.z, insts[i].wbox.vmin.z);
			topbox.vmax.x = MAX(topbox.vmax.x, insts[i].wbox.vmax.x);
			topbox.vmax.y = MAX(topbox.vmax.y, insts[i].wbox.vmax.y);
			topbox.vmax.z = MAX(topbox.vmax.z, insts[i].wbox.vmax.z);
		}
		if(any_bounded) {
			toproot = malloc(sizeof *toproot);
			if(!toproot) return -1;
			memset(toproot->sub, 0, sizeof toproot->sub);
			toproot->aabb = topbox;
			toproot->inst = malloc(num_inst * sizeof *toproot->inst);
			if(!toproot->inst) return -1;
			toproot->num_inst = 0;
			populate_top(toproot);
			if(build_top(toproot, 0) == -1) return -1;
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* production query entry                                              */
/* ------------------------------------------------------------------ */

int proj_intersect(const struct ray *ray, struct spoint *ret,
		struct proj_query_stats *st)
{
	int i, any = 0;

	if(st) {
		st->leaf_tests = 0;
		st->node_tests = 0;
	}

	if(ret) {
		ret->psph = 0;
		ret->inst = 0;
		ret->obj = 0;
		ret->dist = DBL_MAX;
	}

	if(toproot && traverse_top(toproot, ray, ret, st)) any = 1;

	/* cross-pole instances are always queried through their shared local tree */
	for(i = 0; i < num_unbounded; i++) {
		if(intersect_instance(&insts[unbounded_list[i]], ray, ret, st)) {
			any = 1;
			if(!ret) return 1;
		}
	}

	if(!any) return 0;
	if(!ret) return 1;
	return ret->psph != 0;
}

const char *proj_hit_instance(const struct spoint *sp)
{
	return sp && sp->inst ? sp->inst->name : 0;
}

const char *proj_hit_prototype(const struct spoint *sp)
{
	return sp && sp->inst ? sp->inst->proto->name : 0;
}

int proj_hit_sphere(const struct spoint *sp)
{
	int i;
	if(!sp || !sp->inst || !sp->psph) return -1;
	for(i = 0; i < sp->inst->proto->num_sph; i++) {
		if(&sp->inst->proto->sph[i] == sp->psph) return i;
	}
	return -1;
}
