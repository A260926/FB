//
//  pn_cases.c
//  c-ray PN triangle task tests.
//
//  No main() here: task_support/driver.c drives the registered cases.
//
//  Reference strategy:
//  - Flat/degenerate behaviour is checked against an analytic Moller-Trumbore
//    triangle reference written independently in this file.
//  - Curved surfaces are checked against an independent double-precision
//    implementation of the PN control net and Bernstein evaluation, plus a
//    dense multi-start Newton root finder. Dense seeding over the whole
//    parameter triangle makes every isolated transverse root attract at
//    least one seed into its basin; Newton then converges quadratically, and
//    every candidate is accepted only after an independent residual check.
//

#include <task_support/check.h>

#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <v.h>
#include <c-ray/c-ray.h>
#include <common/vector.h>
#include <common/node_parse.h>
#include <common/vendored/cJSON.h>
#include <datatypes/mesh.h>
#include <datatypes/poly.h>
#include <datatypes/pn_triangle.h>
#include <datatypes/hitrecord.h>
#include <datatypes/bbox.h>
#include <datatypes/scene.h>
#include <accelerators/bvh.h>
#include <renderer/renderer.h>
#include <renderer/instance.h>
#include <protocol/protocol.h>

// ---------------------------------------------------------------- helpers

static struct vector v3(float x, float y, float z) { return (struct vector){ x, y, z }; }
static struct cr_vector cv3(float x, float y, float z) { return (struct cr_vector){ x, y, z }; }

static bool closef(float a, float b, float eps) { return fabsf(a - b) <= eps; }
static bool closev(struct vector a, struct vector b, float eps) {
	return vec_length(vec_sub(a, b)) <= eps;
}

static struct poly make_poly(int v0, int v1, int v2, int n0, int n1, int n2,
                             int t0, int t1, int t2, unsigned mat, bool normals) {
	struct poly p = { 0 };
	p.vertexIndex[0] = v0; p.vertexIndex[1] = v1; p.vertexIndex[2] = v2;
	p.normalIndex[0] = n0; p.normalIndex[1] = n1; p.normalIndex[2] = n2;
	p.textureIndex[0] = t0; p.textureIndex[1] = t1; p.textureIndex[2] = t2;
	p.materialIndex = mat;
	p.hasNormals = normals;
	return p;
}

// Builds a raw internal mesh (same style as task_support/baseline.c) and its
// BLAS, optionally in PN mode.
static struct mesh build_raw_mesh(struct vector *verts, size_t nv,
                                   struct vector *nrm, size_t nn,
                                   struct coord *uvs, size_t nu,
                                   struct poly *faces, size_t nf,
                                   bool pn) {
	struct mesh m = { 0 };
	for (size_t i = 0; i < nv; ++i) v_arr_add(m.vbuf.vertices, verts[i]);
	for (size_t i = 0; i < nn; ++i) v_arr_add(m.vbuf.normals, nrm[i]);
	for (size_t i = 0; i < nu; ++i) v_arr_add(m.vbuf.texture_coords, uvs[i]);
	for (size_t i = 0; i < nf; ++i) v_arr_add(m.polygons, faces[i]);
	m.pn_enabled = pn;
	if (pn) m.pn = pn_mesh_setup(&m);
	m.bvh = build_mesh_bvh(&m);
	return m;
}

static struct lightRay ray3(struct vector o, struct vector d) {
	return (struct lightRay){ .start = o, .direction = d, .type = rt_camera };
}

static struct hitRecord fresh_record(void) {
	return (struct hitRecord){ .distance = FLT_MAX, .instIndex = -1 };
}

// ------------------------------------------------- double precision reference

struct dvec { double x, y, z; };

static struct dvec dsub(struct dvec a, struct dvec b) {
	return (struct dvec){ a.x - b.x, a.y - b.y, a.z - b.z };
}
static struct dvec dadd(struct dvec a, struct dvec b) {
	return (struct dvec){ a.x + b.x, a.y + b.y, a.z + b.z };
}
static struct dvec dscale(struct dvec a, double s) {
	return (struct dvec){ a.x * s, a.y * s, a.z * s };
}
static double ddot(struct dvec a, struct dvec b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
static double dlen(struct dvec a) { return sqrt(ddot(a, a)); }
static struct dvec dnorm(struct dvec a) { double l = dlen(a); return dscale(a, 1.0 / l); }

// Independent double control-net construction straight from the task spec.
static void ref_control_net(const struct dvec P[3], const struct dvec Nraw[3],
                            struct dvec B[10]) {
	struct dvec N[3] = { dnorm(Nraw[0]), dnorm(Nraw[1]), dnorm(Nraw[2]) };
	B[0] = P[0]; B[1] = P[1]; B[2] = P[2];
	int from[6] = { 0, 1, 1, 2, 2, 0 };
	int to[6]   = { 1, 0, 2, 1, 0, 2 };
	struct dvec E = { 0, 0, 0 };
	for (int e = 0; e < 6; ++e) {
		struct dvec d = dsub(P[to[e]], P[from[e]]);
		double w = ddot(d, N[from[e]]);
		struct dvec q = dsub(dadd(dscale(P[from[e]], 2.0), P[to[e]]),
		                     dscale(N[from[e]], w));
		B[3 + e] = dscale(q, 1.0 / 3.0);
		E = dadd(E, B[3 + e]);
	}
	E = dscale(E, 1.0 / 6.0);
	struct dvec V = dscale(dadd(dadd(P[0], P[1]), P[2]), 1.0 / 3.0);
	B[9] = dadd(E, dscale(dsub(E, V), 0.5));
}

static const double bern_coeff[10] = { 1, 1, 1, 3, 3, 3, 3, 3, 3, 6 };
static const int bern_p0[10] = { 3, 0, 0, 2, 1, 0, 0, 1, 2, 1 };
static const int bern_p1[10] = { 0, 3, 0, 1, 2, 2, 1, 0, 0, 1 };
static const int bern_p2[10] = { 0, 0, 3, 0, 0, 1, 2, 2, 1, 1 };

static struct dvec ref_eval(const struct dvec B[10], double u, double v) {
	double w0 = 1.0 - u - v, w1 = u, w2 = v;
	struct dvec acc = { 0, 0, 0 };
	for (int i = 0; i < 10; ++i) {
		double t = bern_coeff[i];
		for (int k = 0; k < bern_p0[i]; ++k) t *= w0;
		for (int k = 0; k < bern_p1[i]; ++k) t *= w1;
		for (int k = 0; k < bern_p2[i]; ++k) t *= w2;
		acc = dadd(acc, dscale(B[i], t));
	}
	return acc;
}

static void ref_eval_deriv(const struct dvec B[10], double u, double v,
                           struct dvec *pu, struct dvec *pv) {
	const double e = 1e-7;
	struct dvec a = ref_eval(B, u + e, v);
	struct dvec b = ref_eval(B, u - e, v);
	struct dvec c = ref_eval(B, u, v + e);
	struct dvec d = ref_eval(B, u, v - e);
	*pu = dscale(dsub(a, b), 1.0 / (2.0 * e));
	*pv = dscale(dsub(c, d), 1.0 / (2.0 * e));
}

// 3x3 Gaussian solve with partial pivot.
static bool solve3(double A[3][3], double b[3], double x[3]) {
	for (int col = 0; col < 3; ++col) {
		int piv = col;
		for (int r = col + 1; r < 3; ++r)
			if (fabs(A[r][col]) > fabs(A[piv][col])) piv = r;
		if (fabs(A[piv][col]) < 1e-18) return false;
		if (piv != col) {
			for (int k = 0; k < 3; ++k) {
				double tmp = A[col][k]; A[col][k] = A[piv][k]; A[piv][k] = tmp;
			}
			double tmp = b[col]; b[col] = b[piv]; b[piv] = tmp;
		}
		for (int r = col + 1; r < 3; ++r) {
			double f = A[r][col] / A[col][col];
			for (int k = col; k < 3; ++k) A[r][k] -= f * A[col][k];
			b[r] -= f * b[col];
		}
	}
	for (int r = 2; r >= 0; --r) {
		double s = b[r];
		for (int k = r + 1; k < 3; ++k) s -= A[r][k] * x[k];
		x[r] = s / A[r][r];
	}
	return true;
}

struct ref_root { double t, u, v; };

// Dense multi-start Newton in double precision.
static int ref_intersect(const struct dvec B[10], const struct dvec O,
                         const struct dvec D, float tmax,
                         struct ref_root *out, int max_roots) {
	const int N = 48;
	int n = 0;
	for (int i = 0; i < N; ++i) {
		for (int j = 0; j < N - i; ++j) {
			double u = ((double)i + 0.5) / N;
			double v = ((double)j + 0.5) / N;
			double t = 0.0;
			for (int it = 0; it < 30; ++it) {
				struct dvec P = ref_eval(B, u, v), pu, pv;
				ref_eval_deriv(B, u, v, &pu, &pv);
				struct dvec f = dsub(P, O);
				double nt = ddot(f, D) / ddot(D, D);
				struct dvec g = dsub(f, dscale(D, nt));
				double J[3][3] = {
					{ pu.x, pv.x, -D.x },
					{ pu.y, pv.y, -D.y },
					{ pu.z, pv.z, -D.z },
				};
				double rhs[3] = { -g.x, -g.y, -g.z };
				double delta[3];
				if (!solve3(J, rhs, delta)) break;
				u += delta[0]; v += delta[1]; t = nt + delta[2];
				if (fabs(delta[0]) + fabs(delta[1]) + fabs(delta[2]) < 1e-13) break;
			}
			if (u < -1e-6 || v < -1e-6 || u + v > 1.0 + 1e-6) continue;
			if (t < 0.0 || t >= (double)tmax) continue;
			u = max(0.0, min(1.0, u));
			v = max(0.0, min(1.0 - u, v));
			struct dvec P = ref_eval(B, u, v);
			struct dvec resid = dsub(P, dadd(O, dscale(D, t)));
			if (dlen(resid) > 1e-8) continue;
			bool dup = false;
			for (int k = 0; k < n; ++k) {
				if (fabs(out[k].t - t) < 1e-6 &&
				    fabs(out[k].u - u) < 1e-5 && fabs(out[k].v - v) < 1e-5) {
					dup = true;
					break;
				}
			}
			if (!dup && n < max_roots) out[n++] = (struct ref_root){ t, u, v };
		}
	}
	// Sort by t.
	for (int i = 0; i < n; ++i)
		for (int j = i + 1; j < n; ++j)
			if (out[j].t < out[i].t) {
				struct ref_root tmp = out[i]; out[i] = out[j]; out[j] = tmp;
			}
	return n;
}

// Analytic flat triangle reference (Moller-Trumbore).
static bool flat_hit(struct vector O, struct vector D,
                     struct vector A, struct vector B, struct vector C,
                     float tmax, float *t, float *u, float *v) {
	struct vector e1 = vec_sub(A, B);
	struct vector e2 = vec_sub(C, A);
	struct vector n = vec_cross(e1, e2);
	struct vector cv = vec_sub(A, O);
	struct vector r = vec_cross(D, cv);
	float inv = 1.0f / vec_dot(n, D);
	float fu = vec_dot(r, e2) * inv;
	float fv = vec_dot(r, e1) * inv;
	if (!(fu >= 0.0f && fv >= 0.0f && fu + fv <= 1.0f)) return false;
	float ft = vec_dot(n, cv) * inv;
	if (!(ft >= 0.0f && ft < tmax)) return false;
	*t = ft; *u = fu; *v = fv;
	return true;
}

static void dvec_from_patch(const struct pn_patch *p, struct dvec B[10]) {
	for (int i = 0; i < 10; ++i)
		B[i] = (struct dvec){ p->b[i].x, p->b[i].y, p->b[i].z };
}

// ------------------------------------------------------------- geometry cases

static int case_control_net_formula(void) {
	struct vector verts[3] = { v3(0, 0, 0), v3(1, 0, 0), v3(0, 1, 0) };
	struct vector nrm[3] = {
		v3(0, 0, 1),
		vec_normalize(v3(1, 1, 1)),
		vec_normalize(v3(-1, 1, 1)),
	};
	struct poly face = make_poly(0, 1, 2, 0, 1, 2, -1, -1, -1, 0, true);
	struct mesh m = build_raw_mesh(verts, 3, nrm, 3, NULL, 0, &face, 1, true);
	const struct pn_patch *p = pn_mesh_get_patch(&m, 0);
	TASK_CHECK(p && p->curved);

	// Exact hand value: P0->P1 edge point with N0 = +z is simply (1/3,0,0).
	TASK_CHECK(closev(p->b[3], v3(1.0f / 3.0f, 0, 0), 1e-6f));

	struct dvec P[3] = { {0,0,0}, {1,0,0}, {0,1,0} };
	struct dvec Nr[3] = { {0,0,1}, dnorm((struct dvec){1,1,1}), dnorm((struct dvec){-1,1,1}) };
	struct dvec B[10];
	ref_control_net(P, Nr, B);
	for (int i = 0; i < 10; ++i) {
		TASK_CHECK(closef(p->b[i].x, (float)B[i].x, 1e-6f));
		TASK_CHECK(closef(p->b[i].y, (float)B[i].y, 1e-6f));
		TASK_CHECK(closef(p->b[i].z, (float)B[i].z, 1e-6f));
	}
	// Interior point identity: E + (E-V)/2, i.e. 1.5 E - 0.5 V.
	struct dvec E = { 0, 0, 0 };
	for (int i = 3; i < 9; ++i) E = dadd(E, B[i]);
	E = dscale(E, 1.0 / 6.0);
	struct dvec V = dscale(dadd(dadd(P[0], P[1]), P[2]), 1.0 / 3.0);
	struct dvec interior = dadd(E, dscale(dsub(E, V), 0.5));
	TASK_CHECK(closef(p->b[9].x, (float)interior.x, 1e-6f));
	TASK_CHECK(closef(p->b[9].z, (float)interior.z, 1e-6f));
	mesh_free(&m);
	return 0;
}

static int case_hull_bulge_beyond_vertex_box(void) {
	// Normals tilted toward +z drag edge/interior points toward the camera,
	// so the control hull must protrude outside the flat vertex box.
	struct vector verts[3] = { v3(-1, -1, 2), v3(1, -1, 2), v3(0, 1, 2) };
	struct vector nrm[3] = {
		vec_normalize(v3(0.4f, 0.0f, 1.0f)),
		vec_normalize(v3(-0.4f, 0.0f, 1.0f)),
		vec_normalize(v3(0.0f, 0.4f, 1.0f)),
	};
	struct poly face = make_poly(0, 1, 2, 0, 1, 2, -1, -1, -1, 0, true);
	struct mesh m = build_raw_mesh(verts, 3, nrm, 3, NULL, 0, &face, 1, true);
	const struct pn_patch *p = pn_mesh_get_patch(&m, 0);
	bool bulges = false;
	for (int i = 3; i < 10; ++i)
		if (p->b[i].z < 2.0f - 1e-4f) bulges = true;
	TASK_CHECK(bulges);
	// The BLAS box contains every control point and protrudes past z=2.
	struct boundingBox root = get_root_bbox(m.bvh);
	for (int i = 0; i < 10; ++i) {
		TASK_CHECK(p->b[i].x >= root.min.x - 1e-5f && p->b[i].x <= root.max.x + 1e-5f);
		TASK_CHECK(p->b[i].y >= root.min.y - 1e-5f && p->b[i].y <= root.max.y + 1e-5f);
		TASK_CHECK(p->b[i].z >= root.min.z - 1e-5f && p->b[i].z <= root.max.z + 1e-5f);
	}
	TASK_CHECK(root.min.z < 2.0f);
	TASK_CHECK(closef(p->scale, max(1.0f, bboxDiagonal(p->bbox)), 0.0f));
	mesh_free(&m);
	return 0;
}

static int case_missing_normals_flat_fallback(void) {
	struct vector verts[3] = { v3(-1, -1, 2), v3(1, -1, 2), v3(0, 1, 2) };
	struct vector nrm[1] = { v3(0, 0, 1) };
	struct poly flat = make_poly(0, 1, 2, 0, 0, 0, -1, -1, -1, 3, false);
	struct mesh m = build_raw_mesh(verts, 3, nrm, 1, NULL, 0, &flat, 1, true);
	const struct pn_patch *p = pn_mesh_get_patch(&m, 0);
	TASK_CHECK(p && !p->curved);

	struct lightRay r = ray3(v3(0, 0, 0), v3(0, 0, 1));
	struct hitRecord h = fresh_record();
	TASK_CHECK(traverse_bottom_level_bvh(&m, &r, &h, NULL));
	float ft, fu, fv;
	TASK_CHECK(flat_hit(r.start, r.direction, verts[0], verts[1], verts[2], FLT_MAX,
	                    &ft, &fu, &fv));
	TASK_CHECK(closef(h.distance, ft, 1e-6f));
	TASK_CHECK(closef(h.uv.x, fu, 1e-6f) && closef(h.uv.y, fv, 1e-6f));
	TASK_CHECK(h.polygon == m.polygons && h.polygon->materialIndex == 3);
	mesh_free(&m);
	return 0;
}

static int case_non_unit_normals_get_normalized(void) {
	struct vector verts[3] = { v3(0, 0, 0), v3(1, 0, 0), v3(0, 1, 0) };
	struct vector unit[3] = {
		vec_normalize(v3(0.2f, 0.1f, 1.0f)),
		vec_normalize(v3(-0.3f, 0.2f, 1.0f)),
		vec_normalize(v3(0.1f, -0.2f, 1.0f)),
	};
	struct vector scaled[3] = {
		vec_scale(unit[0], 3.0f), vec_scale(unit[1], 0.5f), vec_scale(unit[2], 7.0f),
	};
	struct poly f1 = make_poly(0, 1, 2, 0, 1, 2, -1, -1, -1, 0, true);
	struct mesh a = build_raw_mesh(verts, 3, unit, 3, NULL, 0, &f1, 1, true);
	struct mesh b = build_raw_mesh(verts, 3, scaled, 3, NULL, 0, &f1, 1, true);
	const struct pn_patch *pa = pn_mesh_get_patch(&a, 0);
	const struct pn_patch *pb = pn_mesh_get_patch(&b, 0);
	for (int i = 0; i < 10; ++i)
		TASK_CHECK(closev(pa->b[i], pb->b[i], 1e-6f));
	mesh_free(&a);
	mesh_free(&b);
	return 0;
}

// ---------------------------------------------------------------- roots cases

static struct mesh curved_demo_mesh(bool fold) {
	struct vector verts[3] = { v3(-2, -1, 0), v3(2, -1, 0), v3(0, 2, 0) };
	struct vector nrm[3];
	if (fold) {
		// Found by offline double-precision search: these normals make the
		// cubic patch fold over itself so some transverse rays have two
		// isolated in-domain roots.
		nrm[0] = vec_normalize(v3(-0.4270921f, -0.8819793f, 0.1992607f));
		nrm[1] = vec_normalize(v3(-0.6703370f, -0.6533911f, 0.3517504f));
		nrm[2] = vec_normalize(v3(0.7413495f, 0.6304683f, 0.2300230f));
	} else {
		nrm[0] = vec_normalize(v3(0.3f, 0.0f, 1.0f));
		nrm[1] = vec_normalize(v3(-0.3f, 0.0f, 1.0f));
		nrm[2] = vec_normalize(v3(0.0f, 0.3f, 1.0f));
	}
	struct poly face = make_poly(0, 1, 2, 0, 1, 2, -1, -1, -1, 0, true);
	return build_raw_mesh(verts, 3, nrm, 3, NULL, 0, &face, 1, true);
}

static int case_folded_multiple_branches(void) {
	struct mesh m = curved_demo_mesh(true);
	const struct pn_patch *p = pn_mesh_get_patch(&m, 0);
	struct dvec B[10];
	dvec_from_patch(p, B);

	// Rays established by the independent double solver to pierce the folded
	// patch twice, with both roots inside the parameter triangle.
	const float O[5][3] = {
		{ 1.928499f, 7.756928f, 0.333098f },
		{ -7.394426f, 3.053271f, -0.001912f },
		{ -7.174350f, 3.538952f, 0.067250f },
		{ 0.168674f, 7.997675f, 0.093546f },
		{ 3.237947f, 7.312797f, 0.196737f },
	};
	const float D[5][3] = {
		{ -0.241062f, -0.969616f, -0.041637f },
		{ 0.924303f, -0.381659f, 0.000239f },
		{ 0.896794f, -0.442369f, -0.008406f },
		{ -0.021084f, -0.999709f, -0.011693f },
		{ -0.404743f, -0.914100f, -0.024592f },
	};
	int folded_rays = 0;
	for (int r = 0; r < 5; ++r) {
		struct dvec Od = { O[r][0], O[r][1], O[r][2] };
		struct dvec Dd = { D[r][0], D[r][1], D[r][2] };
		struct ref_root roots[8];
		int nr = ref_intersect(B, Od, Dd, FLT_MAX, roots, 8);
		TASK_CHECK(nr >= 2);
		folded_rays += (nr >= 2);

		// Branch 1: nearest root via the production BLAS path.
		struct lightRay ray = ray3(v3(O[r][0], O[r][1], O[r][2]),
		                           v3(D[r][0], D[r][1], D[r][2]));
		struct hitRecord h = fresh_record();
		TASK_CHECK(traverse_bottom_level_bvh(&m, &ray, &h, NULL));
		TASK_CHECK(closef(h.distance, (float)roots[0].t, 2e-4f * p->scale));
		struct dvec p1 = ref_eval(B, roots[0].u, roots[0].v);
		TASK_CHECK(closev(h.hitPoint, v3((float)p1.x, (float)p1.y, (float)p1.z),
		                  2e-4f * p->scale));
		// Parameters stay on the original face domain.
		TASK_CHECK(h.uv.x >= 0.0f && h.uv.y >= 0.0f && h.uv.x + h.uv.y <= 1.0f);

		// Branch 2: move the origin past the first root, so the second root is
		// now the nearest valid one. A tmax bound alone cannot expose it
		// (t is a single scalar axis), hence the origin shift.
		const float shift = (float)roots[0].t + 0.5f;
		struct vector o2 = {
			O[r][0] + D[r][0] * shift,
			O[r][1] + D[r][1] * shift,
			O[r][2] + D[r][2] * shift,
		};
		struct lightRay ray2 = ray3(o2, v3(D[r][0], D[r][1], D[r][2]));
		struct hitRecord h2 = fresh_record();
		TASK_CHECK(traverse_bottom_level_bvh(&m, &ray2, &h2, NULL));
		TASK_CHECK(closef(h2.distance, (float)(roots[1].t - shift), 2e-4f * p->scale));
		struct dvec p2pt = ref_eval(B, roots[1].u, roots[1].v);
		TASK_CHECK(closev(h2.hitPoint, v3((float)p2pt.x, (float)p2pt.y, (float)p2pt.z),
		                  2e-4f * p->scale));
	}
	TASK_CHECK(folded_rays == 5); // Multiple branches really exist and are all hit.
	mesh_free(&m);
	return 0;
}

static int case_out_of_domain_root_rejected(void) {
	struct mesh m = curved_demo_mesh(false);
	// A ray aimed beside the control hull can only meet the infinite surface
	// outside the parameter triangle; it must be rejected and leave the
	// candidate record untouched.
	struct hitRecord h = fresh_record();
	h.distance = 7.5f;
	struct lightRay r = ray3(v3(5.0f, 0.1f, -3.0f), v3(0, 0, 1));
	TASK_CHECK(!traverse_bottom_level_bvh(&m, &r, &h, NULL));
	TASK_CHECK(h.distance == 7.5f);
	TASK_CHECK(h.instIndex == -1);
	TASK_CHECK(h.polygon == NULL);

	// Same ray translated onto the patch does hit.
	struct lightRay good = ray3(v3(0.0f, 0.1f, -3.0f), v3(0, 0, 1));
	struct hitRecord h2 = fresh_record();
	TASK_CHECK(traverse_bottom_level_bvh(&m, &good, &h2, NULL));
	mesh_free(&m);
	return 0;
}

static int case_non_unit_direction_and_finite_distance(void) {
	struct mesh m = curved_demo_mesh(false);
	const struct pn_patch *p = pn_mesh_get_patch(&m, 0);
	struct vector o = v3(0.2f, 0.3f, -4.0f);
	struct lightRay r1 = ray3(o, v3(0, 0, 1));
	struct lightRay r4 = ray3(o, v3(0, 0, 4.0f));
	struct lightRay rq = ray3(o, v3(0, 0, 0.25f));
	struct hitRecord h1 = fresh_record(), h4 = fresh_record(), hq = fresh_record();
	TASK_CHECK(traverse_bottom_level_bvh(&m, &r1, &h1, NULL));
	TASK_CHECK(traverse_bottom_level_bvh(&m, &r4, &h4, NULL));
	TASK_CHECK(traverse_bottom_level_bvh(&m, &rq, &hq, NULL));
	// t lives on the passed ray's parameter axis.
	TASK_CHECK(closef(h4.distance * 4.0f, h1.distance, 1e-5f));
	TASK_CHECK(closef(hq.distance * 0.25f, h1.distance, 1e-4f));
	TASK_CHECK(closev(h1.hitPoint, h4.hitPoint, 2e-4f * p->scale));
	TASK_CHECK(closev(h1.hitPoint, hq.hitPoint, 2e-4f * p->scale));

	// Finite distance: a tighter bound rejects the hit and keeps the record.
	struct hitRecord limited = fresh_record();
	limited.distance = h1.distance * 0.5f;
	TASK_CHECK(!traverse_bottom_level_bvh(&m, &r1, &limited, NULL));
	TASK_CHECK(closef(limited.distance, h1.distance * 0.5f, 0.0f));
	TASK_CHECK(limited.polygon == NULL);

	// A bound just beyond the hit accepts it.
	struct hitRecord enough = fresh_record();
	enough.distance = h1.distance + 1.0f;
	TASK_CHECK(traverse_bottom_level_bvh(&m, &r1, &enough, NULL));
	TASK_CHECK(closef(enough.distance, h1.distance, 1e-6f));
	mesh_free(&m);
	return 0;
}

static int case_accuracy_and_clear_miss(void) {
	struct mesh m = curved_demo_mesh(false);
	const struct pn_patch *p = pn_mesh_get_patch(&m, 0);
	struct dvec B[10];
	dvec_from_patch(p, B);
	const float tol = 2e-4f * p->scale;

	// Rays constructed through known interior surface points.
	double us[4] = { 0.2, 0.5, 0.7, 0.15 };
	double vs[4] = { 0.3, 0.1, 0.2, 0.6 };
	for (int k = 0; k < 4; ++k) {
		struct dvec P = ref_eval(B, us[k], vs[k]);
		struct lightRay r = ray3(v3((float)P.x, (float)P.y, (float)P.z - 2.5f),
		                         v3(0, 0, 1));
		struct hitRecord h = fresh_record();
		TASK_CHECK(traverse_bottom_level_bvh(&m, &r, &h, NULL));
		TASK_CHECK(closev(h.hitPoint, v3((float)P.x, (float)P.y, (float)P.z), tol));
		// Position residual along the ray.
		TASK_CHECK(fabsf(h.hitPoint.z - (float)P.z) <= tol);
		TASK_CHECK(h.uv.x >= 0.0f && h.uv.y >= 0.0f && h.uv.x + h.uv.y <= 1.0f);
	}

	// Rays whose closest approach is at least 1e-3*S away are clear misses.
	// The conservative Newton residual gate (2e-4*S) rejects them even close
	// to the boundary; check both the exact boundary and a wider gap.
	float near_gap = 1.2e-3f * p->scale;
	float far_gap = 5.0e-3f * p->scale;
	struct lightRay near_miss = ray3(v3(2.0f + near_gap, 0.0f, -2.0f), v3(0, 0, 1));
	struct lightRay far_miss = ray3(v3(2.0f + far_gap, 0.0f, -2.0f), v3(0, 0, 1));
	struct hitRecord hn = fresh_record();
	struct hitRecord hf = fresh_record();
	TASK_CHECK(!traverse_bottom_level_bvh(&m, &near_miss, &hn, NULL));
	TASK_CHECK(!traverse_bottom_level_bvh(&m, &far_miss, &hf, NULL));
	TASK_CHECK(hn.polygon == NULL && hf.polygon == NULL);
	mesh_free(&m);
	return 0;
}

static int case_near_planar_is_not_degenerate(void) {
	struct vector verts[3] = { v3(-1, -1, 1), v3(1, -1, 1), v3(0, 1, 1) };
	struct vector nrm[3] = {
		vec_normalize(v3(1e-4f, 0.0f, 1.0f)),
		vec_normalize(v3(-1e-4f, 0.0f, 1.0f)),
		vec_normalize(v3(0.0f, 1e-4f, 1.0f)),
	};
	struct poly face = make_poly(0, 1, 2, 0, 1, 2, -1, -1, -1, 0, true);
	struct mesh m = build_raw_mesh(verts, 3, nrm, 3, NULL, 0, &face, 1, true);
	const struct pn_patch *p = pn_mesh_get_patch(&m, 0);
	TASK_CHECK(p->curved);
	struct lightRay r = ray3(v3(0, 0, 0), v3(0, 0, 1));
	struct hitRecord h = fresh_record();
	TASK_CHECK(traverse_bottom_level_bvh(&m, &r, &h, NULL));
	// Nearly flat: the hit stays very close to the plane z=1.
	TASK_CHECK(fabsf(h.hitPoint.z - 1.0f) <= 2e-4f * p->scale);
	TASK_CHECK(vec_length(h.surfaceNormal) > 0.5f);
	mesh_free(&m);
	return 0;
}

// ------------------------------------------------------ integration cases

static int case_multi_face_nearest_and_material(void) {
	// Two curved faces at different depths; nearest face's material wins.
	struct vector verts[6] = {
		v3(-1, -1, 3), v3(1, -1, 3), v3(0, 1, 3),
		v3(-1, -1, 5), v3(1, -1, 5), v3(0, 1, 5),
	};
	struct vector nrm[3] = {
		vec_normalize(v3(0.3f, 0, 1)), vec_normalize(v3(-0.3f, 0, 1)),
		vec_normalize(v3(0, 0.3f, 1)),
	};
	struct poly faces[2] = {
		make_poly(0, 1, 2, 0, 1, 2, -1, -1, -1, 7, true),
		make_poly(3, 4, 5, 0, 1, 2, -1, -1, -1, 11, true),
	};
	struct mesh m = build_raw_mesh(verts, 6, nrm, 3, NULL, 0, faces, 2, true);
	struct lightRay r = ray3(v3(0, 0, 0), v3(0, 0, 2));
	struct hitRecord h = fresh_record();
	TASK_CHECK(traverse_bottom_level_bvh(&m, &r, &h, NULL));
	TASK_CHECK(h.polygon == &m.polygons[0]);
	TASK_CHECK(h.polygon->materialIndex == 7);
	TASK_CHECK(h.distance < 2.0f); // Nearer than the back face at t=2.5.
	// Remove the near face's line of sight: ray offset still resolves to back.
	struct lightRay r2 = ray3(v3(3, 0, 0), v3(0, 0, 1));
	struct hitRecord h2 = fresh_record();
	TASK_CHECK(!traverse_bottom_level_bvh(&m, &r2, &h2, NULL));
	mesh_free(&m);
	return 0;
}

static int case_per_corner_uv_seam_and_material(void) {
	struct vector verts[6] = {
		v3(-1, 0, 2), v3(0, 1, 2), v3(0, -1, 2),
		v3(0, 1, 2), v3(1, 0, 2), v3(0, -1, 2),
	};
	struct vector nrm[4] = {
		vec_normalize(v3(0.2f, 0, 1)), vec_normalize(v3(-0.2f, 0, 1)),
		vec_normalize(v3(0, 0.2f, 1)), vec_normalize(v3(0, -0.2f, 1)),
	};
	struct coord uvs[6] = {
		{ 0, 0 }, { 0, 1 }, { 1, 0 },
		{ 0, 1 }, { 1, 1 }, { 1, 0 },
	};
	// Distinct normal indices at the shared position prove per-face seams.
	struct poly faces[2] = {
		make_poly(0, 1, 2, 0, 1, 2, 0, 1, 2, 2, true),
		make_poly(3, 4, 5, 1, 3, 2, 3, 4, 5, 4, true),
	};
	struct mesh m = build_raw_mesh(verts, 6, nrm, 4, uvs, 6, faces, 2, true);

	struct lightRay left = ray3(v3(-0.4f, 0.0f, -1), v3(0, 0, 1));
	struct hitRecord hl = fresh_record();
	TASK_CHECK(traverse_bottom_level_bvh(&m, &left, &hl, NULL));
	TASK_CHECK(hl.polygon == &m.polygons[0] && hl.polygon->materialIndex == 2);
	TASK_CHECK(hl.uv.x >= 0 && hl.uv.y >= 0 && hl.uv.x + hl.uv.y <= 1.0f);
	// Replicate mesh_uv(): original-face barycentrics select original UVs.
	float bu = hl.uv.x, bv = hl.uv.y, bw = 1.0f - bu - bv;
	struct coord expect_l = {
		bu * uvs[1].x + bv * uvs[2].x + bw * uvs[0].x,
		bu * uvs[1].y + bv * uvs[2].y + bw * uvs[0].y,
	};
	struct coord got_l = {
		bu * m.vbuf.texture_coords[hl.polygon->textureIndex[1]].x +
		bv * m.vbuf.texture_coords[hl.polygon->textureIndex[2]].x +
		bw * m.vbuf.texture_coords[hl.polygon->textureIndex[0]].x,
		0,
	};
	got_l.y = bu * m.vbuf.texture_coords[hl.polygon->textureIndex[1]].y +
	          bv * m.vbuf.texture_coords[hl.polygon->textureIndex[2]].y +
	          bw * m.vbuf.texture_coords[hl.polygon->textureIndex[0]].y;
	TASK_CHECK(closef(got_l.x, expect_l.x, 1e-6f) && closef(got_l.y, expect_l.y, 1e-6f));

	struct lightRay right = ray3(v3(0.4f, 0.0f, -1), v3(0, 0, 1));
	struct hitRecord hr = fresh_record();
	TASK_CHECK(traverse_bottom_level_bvh(&m, &right, &hr, NULL));
	TASK_CHECK(hr.polygon == &m.polygons[1] && hr.polygon->materialIndex == 4);
	TASK_CHECK(hr.uv.x >= 0 && hr.uv.y >= 0 && hr.uv.x + hr.uv.y <= 1.0f);
	mesh_free(&m);
	return 0;
}

// Builds the 256 unique-normal far faces plus two adjacent faces sharing one
// normal index. Output arrays are sized for the caller to keep on the stack.
#define FAR_FACES 256
#define SHARED_FACES 2
#define TOTAL_FACES (FAR_FACES + SHARED_FACES)
#define FAR_NORMALS (FAR_FACES * 3)

static struct mesh build_locality_mesh(void) {
	static struct vector verts[3 * TOTAL_FACES];
	static struct vector nrm[FAR_NORMALS + 5];
	static struct poly faces[TOTAL_FACES];
	size_t vi = 0, ni = 0, fi = 0;
	for (size_t f = 0; f < FAR_FACES; ++f) {
		float bx = (float)(f % 16) * 3.0f;
		float by = (float)(f / 16) * 3.0f;
		verts[vi + 0] = v3(bx, by, 10.0f);
		verts[vi + 1] = v3(bx + 1.0f, by, 10.0f);
		verts[vi + 2] = v3(bx, by + 1.0f, 10.0f);
		for (int c = 0; c < 3; ++c)
			nrm[ni + c] = vec_normalize(v3(0.1f * (c - 1), 0.05f * c, 1.0f));
		faces[fi++] = make_poly((int)vi, (int)vi + 1, (int)vi + 2,
		                        (int)ni, (int)ni + 1, (int)ni + 2,
		                        -1, -1, -1, (unsigned)f, true);
		vi += 3;
		ni += 3;
	}
	// Adjacent pair near the origin sharing normal index ni.
	size_t shared_ni = ni;
	verts[vi + 0] = v3(-1, -1, 2);
	verts[vi + 1] = v3(0, 1, 2);
	verts[vi + 2] = v3(0, -1, 2);
	verts[vi + 3] = v3(0, 1, 2);
	verts[vi + 4] = v3(1, -1, 2);
	verts[vi + 5] = v3(0, -1, 2);
	nrm[ni + 0] = vec_normalize(v3(0.2f, 0.0f, 1.0f)); // shared
	nrm[ni + 1] = vec_normalize(v3(-0.2f, 0.1f, 1.0f));
	nrm[ni + 2] = vec_normalize(v3(0.0f, 0.2f, 1.0f));
	nrm[ni + 3] = vec_normalize(v3(0.2f, -0.1f, 1.0f));
	nrm[ni + 4] = vec_normalize(v3(0.0f, -0.2f, 1.0f));
	faces[fi++] = make_poly((int)vi, (int)vi + 1, (int)vi + 2,
	                        (int)shared_ni, (int)(shared_ni + 1), (int)(shared_ni + 2),
	                        -1, -1, -1, 1, true);
	faces[fi++] = make_poly((int)vi + 3, (int)vi + 4, (int)vi + 5,
	                        (int)shared_ni, (int)(shared_ni + 3), (int)(shared_ni + 4),
	                        -1, -1, -1, 2, true);
	vi += 6;
	ni += 5;
	return build_raw_mesh(verts, vi, nrm, ni, NULL, 0, faces, TOTAL_FACES, true);
}

static int case_sparse_update_locality(void) {
	struct mesh m = build_locality_mesh();
	TASK_CHECK(v_arr_len(m.polygons) >= 258);
	const size_t shared_ni = FAR_NORMALS;

	size_t nodes_before = bvh_node_count(m.bvh);
	long *parents_before = malloc(sizeof(long) * nodes_before);
	long (*children_before)[2] = malloc(sizeof(long[2]) * nodes_before);
	for (size_t i = 0; i < nodes_before; ++i) {
		parents_before[i] = bvh_node_parent(m.bvh, i);
		children_before[i][0] = bvh_node_child(m.bvh, i, 0);
		children_before[i][1] = bvh_node_child(m.bvh, i, 1);
	}

	// Snapshot a far, unaffected control net and the old shared-face hull.
	const struct pn_patch *far_patch = pn_mesh_get_patch(&m, 7);
	struct pn_patch far_snapshot = *far_patch;
	struct boundingBox old_root = get_root_bbox(m.bvh);
	const struct pn_patch *shared0 = pn_mesh_get_patch(&m, FAR_FACES);
	struct vector old_b9 = shared0->b[9];
	struct boundingBox old_leaf_box = shared0->bbox;

	// First update of the shared normal index: exactly the two adjacent faces.
	size_t idx1[1] = { shared_ni };
	struct cr_vector val1[1] = { cv3(0.9f, 0.1f, 0.5f) };
	TASK_CHECK(pn_mesh_update_normals(&m, idx1, val1, 1));
	struct cr_pn_update_stats stats = {
		m.pn->last_stats.updated_patches,
		m.pn->last_stats.leaf_updates,
		m.pn->last_stats.node_updates,
	};
	TASK_CHECK(stats.updated_patches == 2);
	TASK_CHECK(stats.leaf_updates >= 1 && stats.node_updates >= 1);
	TASK_CHECK(!closev(shared0->b[9], old_b9, 1e-6f));
	// Unrelated faces are fully reused.
	TASK_CHECK(memcmp(pn_mesh_get_patch(&m, 7)->b, far_snapshot.b,
	                  sizeof(far_snapshot.b)) == 0);

	// Second update, again touching only the shared pair. Pick a normal that
	// pushes the control hull outside its previous leaf bounds.
	struct vector old_b9_2 = shared0->b[9];
	struct cr_vector val2[1] = { cv3(-0.8f, 0.3f, 0.4f) };
	TASK_CHECK(pn_mesh_update_normals(&m, idx1, val2, 1));
	TASK_CHECK(m.pn->last_stats.updated_patches == 2);
	TASK_CHECK(m.pn->last_stats.leaf_updates == stats.leaf_updates);
	TASK_CHECK(!closev(shared0->b[9], old_b9_2, 1e-6f));
	TASK_CHECK(memcmp(pn_mesh_get_patch(&m, 7)->b, far_snapshot.b,
	                  sizeof(far_snapshot.b)) == 0);

	// Topology is untouched.
	TASK_CHECK(bvh_node_count(m.bvh) == nodes_before);
	for (size_t i = 0; i < nodes_before; ++i) {
		TASK_CHECK(bvh_node_parent(m.bvh, i) == parents_before[i]);
		TASK_CHECK(bvh_node_child(m.bvh, i, 0) == children_before[i][0]);
		TASK_CHECK(bvh_node_child(m.bvh, i, 1) == children_before[i][1]);
	}

	// The refitted BLAS hull of the changed leaf contains the moved control
	// net and the ancestor chain was refitted to cover it, even when the
	// aggregate root box is already dominated by the 256 far faces.
	struct boundingBox new_leaf_box = pn_mesh_get_patch(&m, FAR_FACES)->bbox;
	bool leaf_grew =
		new_leaf_box.min.x < old_leaf_box.min.x - 1e-6f ||
		new_leaf_box.max.x > old_leaf_box.max.x + 1e-6f ||
		new_leaf_box.min.y < old_leaf_box.min.y - 1e-6f ||
		new_leaf_box.max.y > old_leaf_box.max.y + 1e-6f ||
		new_leaf_box.min.z < old_leaf_box.min.z - 1e-6f ||
		new_leaf_box.max.z > old_leaf_box.max.z + 1e-6f;
	TASK_CHECK(leaf_grew);
	struct boundingBox new_root = get_root_bbox(m.bvh);
	for (int c = 0; c < 10; ++c) {
		const struct vector b = pn_mesh_get_patch(&m, FAR_FACES)->b[c];
		TASK_CHECK(b.x >= new_root.min.x - 1e-5f && b.x <= new_root.max.x + 1e-5f);
		TASK_CHECK(b.y >= new_root.min.y - 1e-5f && b.y <= new_root.max.y + 1e-5f);
		TASK_CHECK(b.z >= new_root.min.z - 1e-5f && b.z <= new_root.max.z + 1e-5f);
	}
	(void)old_root;
	struct lightRay r = ray3(v3(0.0f, 0.0f, -3.0f), v3(0, 0, 1));
	struct hitRecord h = fresh_record();
	TASK_CHECK(traverse_bottom_level_bvh(&m, &r, &h, NULL));
	TASK_CHECK(h.polygon == &m.polygons[FAR_FACES] ||
	           h.polygon == &m.polygons[FAR_FACES + 1]);

	// Invalid updates must reject atomically.
	struct pn_patch before0 = *pn_mesh_get_patch(&m, FAR_FACES);
	size_t bad_idx[1] = { FAR_NORMALS + 99 };
	struct cr_vector bad_val[1] = { cv3(0, 0, 1) };
	TASK_CHECK(!pn_mesh_update_normals(&m, bad_idx, bad_val, 1));
	struct cr_vector nan_val[1] = { cv3(NAN, 0, 1) };
	TASK_CHECK(!pn_mesh_update_normals(&m, idx1, nan_val, 1));
	struct cr_vector zero_val[1] = { cv3(0, 0, 0) };
	TASK_CHECK(!pn_mesh_update_normals(&m, idx1, zero_val, 1));
	TASK_CHECK(memcmp(pn_mesh_get_patch(&m, FAR_FACES)->b, before0.b,
	                  sizeof(before0.b)) == 0);
	TASK_CHECK(m.pn->last_stats.updated_patches == 2);

	// Empty update is a successful no-op.
	TASK_CHECK(pn_mesh_update_normals(&m, NULL, NULL, 0));
	free(parents_before);
	free(children_before);
	mesh_free(&m);
	return 0;
}

static int case_shared_position_distinct_normal_no_overupdate(void) {
	// Two faces share a position but use different normal indices there.
	struct vector verts[6] = {
		v3(-1, 0, 2), v3(0, 1, 2), v3(0, -1, 2),
		v3(0, 1, 2), v3(1, 0, 2), v3(0, -1, 2),
	};
	struct vector nrm[6] = {
		vec_normalize(v3(0.2f, 0, 1)), vec_normalize(v3(-0.2f, 0, 1)),
		vec_normalize(v3(0, 0.2f, 1)), vec_normalize(v3(0.5f, 0.5f, 1)),
		vec_normalize(v3(0, -0.2f, 1)), vec_normalize(v3(0.3f, 0.3f, 1)),
	};
	struct poly faces[2] = {
		make_poly(0, 1, 2, 0, 1, 2, -1, -1, -1, 0, true),
		make_poly(3, 4, 5, 3, 5, 4, -1, -1, -1, 1, true),
	};
	struct mesh m = build_raw_mesh(verts, 6, nrm, 6, NULL, 0, faces, 2, true);
	struct pn_patch second_before = *pn_mesh_get_patch(&m, 1);
	size_t idx[1] = { 0 }; // Only used by face 0.
	struct cr_vector val[1] = { cv3(0.9f, 0.0f, 0.2f) };
	TASK_CHECK(pn_mesh_update_normals(&m, idx, val, 1));
	TASK_CHECK(m.pn->last_stats.updated_patches == 1);
	TASK_CHECK(memcmp(pn_mesh_get_patch(&m, 1)->b, second_before.b,
	                  sizeof(second_before.b)) == 0);
	mesh_free(&m);
	return 0;
}

static int case_watertight_shared_edge(void) {
	// Opposite-winding patches meeting on a shared edge with identical
	// positions and normals: no geometric crack along or beside the edge.
	struct vector verts[6] = {
		v3(-1, 0, 2), v3(0, 1, 2), v3(0, -1, 2),
		v3(0, 1, 2), v3(-1, 0, 2), v3(1, -1, 2),
	};
	struct vector nrm[4] = {
		vec_normalize(v3(0.2f, 0.0f, 1.0f)),
		vec_normalize(v3(-0.2f, 0.1f, 1.0f)),
		vec_normalize(v3(0.0f, 0.2f, 1.0f)),
		vec_normalize(v3(0.3f, -0.2f, 1.0f)),
	};
	struct poly faces[2] = {
		// Shared edge is verts 0-1; matching normal indices 0 and 1.
		make_poly(0, 1, 2, 0, 1, 2, -1, -1, -1, 0, true),
		make_poly(3, 4, 5, 1, 0, 3, -1, -1, -1, 1, true),
	};
	struct mesh m = build_raw_mesh(verts, 6, nrm, 4, NULL, 0, faces, 2, true);
	const struct pn_patch *p0 = pn_mesh_get_patch(&m, 0);
	const struct pn_patch *p1 = pn_mesh_get_patch(&m, 1);
	struct dvec B0[10], B1[10];
	dvec_from_patch(p0, B0);
	dvec_from_patch(p1, B1);
	const float tol = 2e-4f * max(p0->scale, p1->scale);

	for (int k = 0; k <= 10; ++k) {
		double s = (double)k / 10.0;
		// Shared edge: face 0 traces P0->P1 at (u=s, v=0). Face 1 winds the
		// edge oppositely (its vertex1 is face0's P0), so its matching edge
		// is P1'->P0' at (u=1-s, v=0). The geometric control points coincide.
		struct dvec q0 = ref_eval(B0, s, 0.0);
		struct dvec q1 = ref_eval(B1, 1.0 - s, 0.0);
		TASK_CHECK(fabs(q0.x - q1.x) <= tol && fabs(q0.y - q1.y) <= tol &&
		           fabs(q0.z - q1.z) <= tol);

		// A ray aimed just inside face 0 from this edge lands on face 0;
		// no crack opens between the two patches along the seam.
		struct dvec inside = ref_eval(B0, s * 0.5 + 0.25, 0.05);
		struct lightRay near_edge = ray3(
			v3((float)inside.x, (float)inside.y, (float)inside.z - 2.0f),
			v3(0, 0, 1));
		struct hitRecord h = fresh_record();
		TASK_CHECK(traverse_bottom_level_bvh(&m, &near_edge, &h, NULL));
		TASK_CHECK(closev(h.hitPoint,
		                  v3((float)inside.x, (float)inside.y, (float)inside.z), tol));
	}
	// Rays offset to either side land on the respective face.
	struct dvec left_q = ref_eval(B0, 0.5, 0.25);
	struct dvec right_q = ref_eval(B1, 0.25, 0.5);
	struct lightRay rl = ray3(v3((float)left_q.x, (float)left_q.y, (float)left_q.z - 2),
	                          v3(0, 0, 1));
	struct lightRay rr = ray3(v3((float)right_q.x, (float)right_q.y, (float)right_q.z - 2),
	                          v3(0, 0, 1));
	struct hitRecord hl = fresh_record(), hr = fresh_record();
	TASK_CHECK(traverse_bottom_level_bvh(&m, &rl, &hl, NULL));
	TASK_CHECK(traverse_bottom_level_bvh(&m, &rr, &hr, NULL));
	TASK_CHECK(hl.polygon == &m.polygons[0]);
	TASK_CHECK(hr.polygon == &m.polygons[1]);
	mesh_free(&m);
	return 0;
}

static int case_instances_pick_up_world_bounds(void) {
	struct mesh m = curved_demo_mesh(false);
	struct boundingBox old_local = get_root_bbox(m.bvh);

	// Mimic two instances sharing the mesh. The instance collection must be a
	// real v_arr because build_top_level_bvh reads its length through it.
	struct mesh *meshes[1] = { &m };
	struct instance *instances = NULL;
	v_arr_add(instances, new_mesh_instance(&meshes[0], 0, NULL, NULL));
	v_arr_add(instances, new_mesh_instance(&meshes[0], 0, NULL, NULL));
	instances[1].composite = tform_new_translate(0.0f, 5.0f, 0.0f);

	// Refit the BLAS after a sparse normal change that grows the hull.
	size_t idx[1] = { 0 };
	struct cr_vector val[1] = { cv3(-0.9f, 0.2f, 0.3f) };
	TASK_CHECK(pn_mesh_update_normals(&m, idx, val, 1));
	struct bvh *top = build_top_level_bvh(instances);
	struct boundingBox world = get_root_bbox(top);
	struct boundingBox grown_local = get_root_bbox(m.bvh);
	TASK_CHECK(grown_local.min.x <= old_local.min.x ||
	           grown_local.max.x >= old_local.max.x ||
	           grown_local.min.z <= old_local.min.z);
	// World box must cover both translated copies of the grown hull: the
	// upper copy raises world.max.y, the lower copy keeps world.min.y at the
	// local minimum.
	TASK_CHECK(world.max.y >= grown_local.max.y + 5.0f - 1e-4f);
	TASK_CHECK(world.min.y <= grown_local.min.y + 1e-4f);
	TASK_CHECK(world.max.y >= grown_local.max.y + 5.0f + 1e-4f ||
	           world.min.x <= grown_local.min.x || world.max.x >= grown_local.max.x);
	destroy_bvh(top);
	v_arr_free(instances);
	mesh_free(&m);
	return 0;
}

// ------------------------------------------------------------- protocol cases

static struct cr_shader_node *emissive_desc(struct cr_color c, float strength) {
	struct cr_value_node *r = calloc(1, sizeof(*r));
	struct cr_value_node *g = calloc(1, sizeof(*g));
	struct cr_value_node *b = calloc(1, sizeof(*b));
	r->type = g->type = b->type = cr_vn_constant;
	r->arg.constant = c.r; g->arg.constant = c.g; b->arg.constant = c.b;
	struct cr_color_node *col = calloc(1, sizeof(*col));
	col->type = cr_cn_rgb;
	col->arg.rgb.red = r;
	col->arg.rgb.green = g;
	col->arg.rgb.blue = b;
	struct cr_value_node *s = calloc(1, sizeof(*s));
	s->type = cr_vn_constant;
	s->arg.constant = strength;
	struct cr_shader_node *node = calloc(1, sizeof(*node));
	node->type = cr_bsdf_emissive;
	node->arg.emissive.color = col;
	node->arg.emissive.strength = s;
	return node;
}

static struct cr_shader_node *background_desc(struct cr_color c) {
	struct cr_value_node *r = calloc(1, sizeof(*r));
	struct cr_value_node *g = calloc(1, sizeof(*g));
	struct cr_value_node *b = calloc(1, sizeof(*b));
	r->type = g->type = b->type = cr_vn_constant;
	r->arg.constant = c.r; g->arg.constant = c.g; b->arg.constant = c.b;
	struct cr_color_node *col = calloc(1, sizeof(*col));
	col->type = cr_cn_rgb;
	col->arg.rgb.red = r;
	col->arg.rgb.green = g;
	col->arg.rgb.blue = b;
	struct cr_value_node *s = calloc(1, sizeof(*s));
	struct cr_vector_node *pose = calloc(1, sizeof(*pose));
	s->type = cr_vn_constant; s->arg.constant = 1.0;
	pose->type = cr_vec_constant; pose->arg.constant = cv3(0, 0, 0);
	struct cr_shader_node *node = calloc(1, sizeof(*node));
	node->type = cr_bsdf_background;
	node->arg.background.color = col;
	node->arg.background.strength = s;
	node->arg.background.pose = pose;
	return node;
}

static struct cr_renderer *build_pn_renderer(bool enable_pn) {
	struct cr_renderer *r = cr_new_renderer();
	struct cr_scene *s = cr_renderer_scene_get(r);
	cr_mesh mesh = cr_scene_mesh_new(s, "pn");
	struct cr_vector verts[3] = {
		cv3(-1, -1, 3), cv3(1, -1, 3), cv3(0, 1, 3),
	};
	struct cr_vector nrm[3] = {
		cv3(0.4f, 0.0f, 1.0f), cv3(-0.4f, 0.0f, 1.0f), cv3(0.0f, 0.4f, 1.0f),
	};
	struct cr_vertex_buf_param buf = {
		.vertices = verts, .vertex_count = 3,
		.normals = nrm, .normal_count = 3,
	};
	cr_mesh_bind_vertex_buf(s, mesh, buf);
	struct cr_face face = {
		.vertex_idx = { 0, 1, 2 },
		.normal_idx = { 0, 1, 2 },
		.texture_idx = { -1, -1, -1 },
		.mat_idx = 0,
		.has_normals = true,
	};
	cr_mesh_bind_faces(s, mesh, &face, 1);
	cr_material_set set = cr_scene_new_material_set(s);
	struct cr_shader_node *mat = emissive_desc((struct cr_color){ 0.9f, 0.2f, 0.1f, 1 }, 1.0f);
	cr_material_set_add(s, set, mat);
	cr_shader_node_free(mat);
	if (enable_pn && !cr_mesh_set_pn_triangles(s, mesh, true))
		return NULL;
	cr_mesh_finalize(s, mesh);
	cr_instance inst = cr_instance_new(s, mesh, cr_object_mesh);
	if (!cr_instance_bind_material_set(s, inst, set))
		return NULL;
	cr_camera cam = cr_camera_new(s);
	cr_camera_set_num_pref(s, cam, cr_camera_res_x, 16);
	cr_camera_set_num_pref(s, cam, cr_camera_res_y, 16);
	cr_camera_update(s, cam);
	return r;
}

static int case_protocol_roundtrip_production_query(void) {
	struct cr_renderer *r = build_pn_renderer(true);
	TASK_CHECK(r != NULL);
	struct renderer *internal = (struct renderer *)r;
	v_threadpool_wait(internal->scene->bg_worker);
	TASK_CHECK(internal->scene->meshes[0].pn_enabled);
	TASK_CHECK(internal->scene->meshes[0].pn != NULL);

	char *data = serialize_renderer(internal);
	TASK_CHECK(data && strstr(data, "pn_triangles") != NULL);
	struct renderer *restored = deserialize_renderer(data);
	TASK_CHECK(restored);
	v_threadpool_wait(restored->scene->bg_worker);
	TASK_CHECK(restored->scene->meshes[0].pn_enabled);
	TASK_CHECK(restored->scene->meshes[0].pn != NULL);

	// Real production path: prepare the top-level BVH exactly like a worker,
	// then trace a ray through the instance hierarchy.
	update_toplevel_bvh(restored->scene);
	struct lightRay ray = ray3(v3(0, 0, 0), v3(0, 0, 1));
	struct hitRecord h = fresh_record();
	TASK_CHECK(traverse_top_level_bvh(restored->scene->instances,
	                                  restored->scene->topLevel, &ray, &h, NULL));
	TASK_CHECK(h.instIndex >= 0);
	TASK_CHECK(h.polygon != NULL);
	// The curved patch is not coplanar with the flat z=3 triangle: its hull
	// protrudes on both sides, and the production hit matches the local BLAS
	// intersection computed on the deserialized mesh.
	const struct pn_patch *patch = pn_mesh_get_patch(&restored->scene->meshes[0], 0);
	TASK_CHECK(patch != NULL && patch->curved);
	TASK_CHECK(patch->bbox.min.z < 3.0f && patch->bbox.max.z > 3.0f);
	struct hitRecord local = fresh_record();
	TASK_CHECK(traverse_bottom_level_bvh(&restored->scene->meshes[0], &ray, &local, NULL));
	TASK_CHECK(closef(h.hitPoint.z, local.hitPoint.z, 2e-4f * patch->scale));
	TASK_CHECK(fabsf(h.hitPoint.z - 3.0f) > 1e-4f);

	// A second roundtrip preserves the PN flag and full control net exactly.
	// (Whole-payload byte equality is not asserted because the pre-existing
	// shader-node serializer normalizes rgb color nodes into constants.)
	char *data2 = serialize_renderer(restored);
	struct renderer *restored2 = deserialize_renderer(data2);
	TASK_CHECK(restored2);
	v_threadpool_wait(restored2->scene->bg_worker);
	TASK_CHECK(restored2->scene->meshes[0].pn_enabled);
	const struct pn_patch *pa = pn_mesh_get_patch(&restored->scene->meshes[0], 0);
	const struct pn_patch *pb = pn_mesh_get_patch(&restored2->scene->meshes[0], 0);
	TASK_CHECK(pa && pb);
	for (int i = 0; i < PN_CONTROL_POINT_COUNT; ++i)
		TASK_CHECK(closev(pa->b[i], pb->b[i], 1e-6f));
	free(data);
	free(data2);
	cr_destroy_renderer(r);
	cr_destroy_renderer((struct cr_renderer *)restored);
	cr_destroy_renderer((struct cr_renderer *)restored2);
	return 0;
}

static int case_legacy_payload_defaults_disabled(void) {
	struct cr_renderer *r = build_pn_renderer(true);
	TASK_CHECK(r != NULL);
	struct renderer *internal = (struct renderer *)r;
	v_threadpool_wait(internal->scene->bg_worker);
	char *data = serialize_renderer(internal);

	cJSON *json = cJSON_Parse(data);
	cJSON *meshes = cJSON_GetObjectItem(json, "scene") ?
	                cJSON_GetObjectItem(cJSON_GetObjectItem(json, "scene"), "meshes") : NULL;
	TASK_CHECK(cJSON_IsArray(meshes));
	cJSON_DeleteItemFromObject(cJSON_GetArrayItem(meshes, 0), "pn_triangles");
	char *legacy = cJSON_PrintUnformatted(json);
	cJSON_Delete(json);

	struct renderer *restored = deserialize_renderer(legacy);
	TASK_CHECK(restored);
	v_threadpool_wait(restored->scene->bg_worker);
	TASK_CHECK(!restored->scene->meshes[0].pn_enabled);
	TASK_CHECK(restored->scene->meshes[0].pn == NULL);

	update_toplevel_bvh(restored->scene);
	struct lightRay ray = ray3(v3(0, 0, 0), v3(0, 0, 1));
	struct hitRecord h = fresh_record();
	TASK_CHECK(traverse_top_level_bvh(restored->scene->instances,
	                                  restored->scene->topLevel, &ray, &h, NULL));
	// Flat triangle lies exactly on z=3; a curved patch would sit in front.
	TASK_CHECK(closef(h.hitPoint.z, 3.0f, 1e-5f));

	free(data);
	free(legacy);
	cr_destroy_renderer(r);
	cr_destroy_renderer((struct cr_renderer *)restored);
	return 0;
}

// PPM produced by examples/pn_preview.c via task_support/run.sh.
static int case_preview_image_contents(void) {
	FILE *f = fopen(".task-build/pn.ppm", "r");
	TASK_CHECK(f != NULL);
	char magic[8];
	int w, h, maxv;
	TASK_CHECK(fscanf(f, "%7s %d %d %d", magic, &w, &h, &maxv) == 4);
	TASK_CHECK(strcmp(magic, "P3") == 0 && w == 48 && h == 32 && maxv == 255);
	int bg = 0, red = 0, green = 0, other = 0;
	for (int i = 0; i < w * h; ++i) {
		int rr, gg, bb;
		if (fscanf(f, "%d %d %d", &rr, &gg, &bb) != 3) { fclose(f); TASK_CHECK(0); }
		if (bb > 150 && rr < 100 && gg < 100) ++bg;
		else if (rr > 150 && gg < 110 && bb < 110) ++red;
		else if (gg > 150 && rr < 110 && bb < 110) ++green;
		else ++other;
	}
	fclose(f);
	TASK_CHECK(bg > 0 && red > 0 && green > 0);
	// The curved patch silhouette differs from the flat one.
	TASK_CHECK(abs(red - green) > 0);
	return 0;
}

// -------------------------------------------------------------- registration

const struct task_case task_cases[] = {
	{ "geometry", "control_net_formula", case_control_net_formula },
	{ "geometry", "hull_bulge_beyond_vertex_box", case_hull_bulge_beyond_vertex_box },
	{ "geometry", "missing_normals_flat_fallback", case_missing_normals_flat_fallback },
	{ "geometry", "non_unit_normals_normalized", case_non_unit_normals_get_normalized },

	{ "roots", "folded_multiple_branches", case_folded_multiple_branches },
	{ "roots", "out_of_domain_root_rejected", case_out_of_domain_root_rejected },
	{ "roots", "non_unit_direction_finite_distance", case_non_unit_direction_and_finite_distance },
	{ "roots", "accuracy_and_clear_miss", case_accuracy_and_clear_miss },
	{ "roots", "near_planar_not_degenerate", case_near_planar_is_not_degenerate },

	{ "integration", "multi_face_nearest_and_material", case_multi_face_nearest_and_material },
	{ "integration", "per_corner_uv_seam_and_material", case_per_corner_uv_seam_and_material },
	{ "integration", "sparse_update_locality", case_sparse_update_locality },
	{ "integration", "shared_position_distinct_normal", case_shared_position_distinct_normal_no_overupdate },
	{ "integration", "watertight_shared_edge", case_watertight_shared_edge },
	{ "integration", "instances_world_bounds", case_instances_pick_up_world_bounds },

	{ "protocol", "roundtrip_production_query", case_protocol_roundtrip_production_query },
	{ "protocol", "legacy_payload_disabled", case_legacy_payload_defaults_disabled },
	{ "protocol", "preview_image_contents", case_preview_image_contents },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
