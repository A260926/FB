/*
 * GL_FEEDBACK contract tests for TinyGL (C99, no SDL/OpenMP/device).
 *
 * The test parses the real feedback float array field by field. Expected
 * clip coordinates, clipping intersection parameters and interpolated
 * attributes are computed independently in this file (generic 4x4 matrix
 * math plus a hand written Liang-Barsky / Sutherland-Hodgman clipper); they
 * are never taken from the library's clipping code paths.
 */
#include <GL/gl.h>
#include <zbuffer.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                                                                                          \
	do {                                                                                                                                           \
		if (!(cond)) {                                                                                                                             \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                                            \
			printf(__VA_ARGS__);                                                                                                                   \
			putchar('\n');                                                                                                                         \
			failures++;                                                                                                                            \
		}                                                                                                                                          \
	} while (0)

/* ------------------------------------------------------------------ */
/* independent reference math                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	float m[16]; /* row major */
} Mat4;

typedef struct {
	float pc[4];   /* clip coordinates */
	float win[4];  /* feedback window x,y,z and clip w */
	float col[4];  /* rgba */
	float tex[4];  /* post texture matrix, no perspective divide */
} RefV;

/* non zero viewport used for every test: glViewport(4, 6, 40, 24) */
#define VP_XMIN 4
#define VP_YMIN 6
#define VP_XSIZE 40
#define VP_YSIZE 24
static const float vp_sx = (VP_XSIZE - 0.5f) / 2.0f;
static const float vp_tx = (VP_XSIZE - 0.5f) / 2.0f + VP_XMIN;
static const float vp_sy = -(VP_YSIZE - 0.5f) / 2.0f;
static const float vp_ty = (VP_YSIZE - 0.5f) / 2.0f + VP_YMIN;

static Mat4 mat_identity(void) {
	Mat4 r;
	memset(&r, 0, sizeof(r));
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
	return r;
}

static Mat4 mat_mul(const Mat4* a, const Mat4* b) {
	Mat4 r;
	int i, j, k;
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++) {
			float s = 0.0f;
			for (k = 0; k < 4; k++)
				s += a->m[4 * i + k] * b->m[4 * k + j];
			r.m[4 * i + j] = s;
		}
	return r;
}

static void mat_vec(const Mat4* m, const float in[4], float out[4]) {
	int i, j;
	for (i = 0; i < 4; i++) {
		float s = 0.0f;
		for (j = 0; j < 4; j++)
			s += m->m[4 * i + j] * in[j];
		out[i] = s;
	}
}

/* independent glFrustum reimplementation */
static Mat4 mat_frustum(float l, float r, float b, float t, float n, float f) {
	Mat4 m;
	memset(&m, 0, sizeof(m));
	m.m[0] = (2.0f * n) / (r - l);
	m.m[2] = (r + l) / (r - l);
	m.m[5] = (2.0f * n) / (t - b);
	m.m[6] = (t + b) / (t - b);
	m.m[10] = -(f + n) / (f - n);
	m.m[11] = -(2.0f * f * n) / (f - n);
	m.m[14] = -1.0f;
	return m;
}

static Mat4 mat_translate(float x, float y, float z) {
	Mat4 m = mat_identity();
	m.m[3] = x;
	m.m[7] = y;
	m.m[11] = z;
	return m;
}

static void ref_window(RefV* v) {
	float inv = 1.0f / v->pc[3];
	v->win[0] = v->pc[0] * inv * vp_sx + vp_tx;
	v->win[1] = v->pc[1] * inv * vp_sy + vp_ty;
	v->win[2] = (v->pc[2] * inv + 1.0f) * 0.5f;
	v->win[3] = v->pc[3];
}

static RefV ref_make(const Mat4* mp, const Mat4* mv, const Mat4* mt, const float obj[4], const float col[4], const float texin[4]) {
	RefV v;
	float post[4];
	Mat4 mpv = mat_mul(mp, mv);
	mat_vec(&mpv, obj, v.pc);
	mat_vec(mt, texin, post);
	memcpy(v.col, col, sizeof(v.col));
	memcpy(v.tex, post, sizeof(v.tex));
	ref_window(&v);
	return v;
}

static RefV ref_lerp(const RefV* a, const RefV* b, float t) {
	RefV q;
	int i;
	for (i = 0; i < 4; i++) {
		q.pc[i] = a->pc[i] + (b->pc[i] - a->pc[i]) * t;
		q.col[i] = a->col[i] + (b->col[i] - a->col[i]) * t;
		q.tex[i] = a->tex[i] + (b->tex[i] - a->tex[i]) * t;
	}
	ref_window(&q);
	return q;
}

/* signed plane distances, inside means d <= 0 */
static float plane_d(int k, const float p[4]) {
	switch (k) {
	case 0:
		return -p[0] - p[3]; /* x >= -w */
	case 1:
		return p[0] - p[3]; /* x <=  w */
	case 2:
		return -p[1] - p[3];
	case 3:
		return p[1] - p[3];
	case 4:
		return -p[2] - p[3];
	case 5:
		return p[2] - p[3];
	}
	return 0.0f;
}

/* independent Liang-Barsky line clipping; returns 0 when fully outside */
static int ref_clip_line(const RefV* a, const RefV* b, RefV* q1, RefV* q2) {
	float tmin = 0.0f, tmax = 1.0f;
	int k;
	for (k = 0; k < 6; k++) {
		float d0 = plane_d(k, a->pc);
		float d1 = plane_d(k, b->pc);
		if (d0 > 0.0f && d1 > 0.0f)
			return 0;
		if (d0 * d1 < 0.0f || (d0 > 0.0f) != (d1 > 0.0f)) {
			float t = d0 / (d0 - d1);
			if (d0 > 0.0f) {
				if (t > tmin)
					tmin = t;
			} else {
				if (t < tmax)
					tmax = t;
			}
		}
	}
	if (tmin > tmax)
		return 0;
	*q1 = ref_lerp(a, b, tmin);
	*q2 = ref_lerp(a, b, tmax);
	return 1;
}

/* independent Sutherland-Hodgman polygon clipping in clip space */
static int ref_clip_poly(const RefV in[], int nin, RefV out[], int cap) {
	RefV cur[16], nxt[16];
	int count = nin, k, i, nc;
	memcpy(cur, in, sizeof(RefV) * (size_t)nin);
	for (k = 0; k < 6 && count > 0; k++) {
		nc = 0;
		for (i = 0; i < count; i++) {
			const RefV* a = &cur[(i + count - 1) % count];
			const RefV* b = &cur[i];
			float da = plane_d(k, a->pc);
			float db = plane_d(k, b->pc);
			int ain = da <= 0.0f;
			int bin = db <= 0.0f;
			if (ain != bin && nc < cap)
				nxt[nc++] = ref_lerp(a, b, da / (da - db));
			if (bin && nc < cap)
				nxt[nc++] = *b;
		}
		memcpy(cur, nxt, sizeof(RefV) * (size_t)nc);
		count = nc;
	}
	memcpy(out, cur, sizeof(RefV) * (size_t)count);
	return count;
}

/* ------------------------------------------------------------------ */
/* feedback stream parsing                                             */
/* ------------------------------------------------------------------ */

static int vertex_size(GLenum type) {
	switch (type) {
	case GL_2D:
		return 2;
	case GL_3D:
		return 3;
	case GL_3D_COLOR:
		return 7;
	case GL_3D_COLOR_TEXTURE:
		return 11;
	case GL_4D_COLOR_TEXTURE:
		return 12;
	default:
		return 0;
	}
}

static int pos_count(GLenum type) {
	switch (type) {
	case GL_2D:
		return 2;
	case GL_4D_COLOR_TEXTURE:
		return 4;
	default:
		return 3;
	}
}

static int has_color(GLenum type) {
	return type == GL_3D_COLOR || type == GL_3D_COLOR_TEXTURE || type == GL_4D_COLOR_TEXTURE;
}

static int has_tex(GLenum type) {
	return type == GL_3D_COLOR_TEXTURE || type == GL_4D_COLOR_TEXTURE;
}

static int feq(float a, float b, float eps) {
	return fabsf(a - b) <= eps;
}

/* compare one feedback vertex against the independent reference */
static void check_vertex(const char* what, const float* p, GLenum type, const RefV* r) {
	int np = pos_count(type);
	float eps = 1e-4f;
	CHECK(feq(p[0], r->win[0], eps), "%s x %.6g != %.6g", what, p[0], r->win[0]);
	CHECK(feq(p[1], r->win[1], eps), "%s y %.6g != %.6g", what, p[1], r->win[1]);
	if (np >= 3)
		CHECK(feq(p[2], r->win[2], eps), "%s z %.6g != %.6g", what, p[2], r->win[2]);
	if (np >= 4)
		CHECK(feq(p[3], r->win[3], eps), "%s w %.6g != %.6g", what, p[3], r->win[3]);
	if (has_color(type)) {
		int off = np;
		int k;
		for (k = 0; k < 4; k++)
			CHECK(feq(p[off + k], r->col[k], 1e-5f), "%s color%d %.6g != %.6g", what, k, p[off + k], r->col[k]);
	}
	if (has_tex(type)) {
		int off = np + (has_color(type) ? 4 : 0);
		int k;
		for (k = 0; k < 4; k++)
			CHECK(feq(p[off + k], r->tex[k], 1e-5f), "%s tex%d %.6g != %.6g", what, k, p[off + k], r->tex[k]);
	}
}

/* greedy multiset match of produced vertices against the reference polygon */
static void check_vertex_set(const char* what, const float* recs, int nverts, GLenum type, const RefV* ref, int nref) {
	char used[16] = {0};
	int vs = vertex_size(type);
	int i, j, matched = 0;
	CHECK(nref <= 16, "%s reference polygon too large", what);
	for (i = 0; i < nverts; i++) {
		const float* p = recs + i * vs;
		for (j = 0; j < nref; j++) {
			if (used[j])
				continue;
			/* cheap proximity gate on window x/y then full compare */
			if (feq(p[0], ref[j].win[0], 1e-4f) && feq(p[1], ref[j].win[1], 1e-4f)) {
				int before = failures;
				check_vertex(what, p, type, &ref[j]);
				if (failures == before) {
					used[j] = 1;
					matched++;
					break;
				}
			}
		}
	}
	CHECK(matched == nverts, "%s only %d/%d feedback vertices matched a reference point", what, matched, nverts);
}

#define SENTINEL -777.0f
#define TAIL 4

static float pool[4096];

static float* alloc_buffer(int floats) {
	int i;
	(void)floats;
	for (i = 0; i < (int)(sizeof(pool) / sizeof(pool[0])); i++)
		pool[i] = SENTINEL;
	return pool;
}

static void check_tail(const float* b, int used_floats, int total) {
	int i;
	for (i = used_floats; i < total + TAIL; i++)
		CHECK(b[i] == SENTINEL, "sentinel overwritten at %d (got %.6g)", i, b[i]);
}

/* ------------------------------------------------------------------ */
/* scenarios                                                           */
/* ------------------------------------------------------------------ */

static ZBuffer* z;
static Mat4 proj, model, texm;

static void setup_projection_perspective(void) {
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 10.0);
	proj = mat_frustum(-1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 10.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	model = mat_identity();
}

static void setup_projection_ortho(void) {
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	proj = mat_identity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	model = mat_identity();
}

static void reset_texture_matrix(void) {
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	texm = mat_identity();
	glMatrixMode(GL_MODELVIEW);
}

static RefV mk(const float obj[4], const float col[4], const float tex[4]) {
	return ref_make(&proj, &model, &texm, obj, col, tex);
}

/* the original failure: passthrough followed by a point */
static void test_passthrough_and_point(void) {
	enum { N = 16 };
	float* b = alloc_buffer(N + TAIL);
	RefV r;
	float obj[4] = {0.125f, 0.25f, 0.0f, 1.0f};
	float col[4] = {1.0f, 0.0f, 0.0f, 1.0f};
	float tex[4] = {0, 0, 0, 1};
	int n;
	setup_projection_ortho();
	reset_texture_matrix();
	glFeedbackBuffer(N, GL_3D, b);
	n = glRenderMode(GL_FEEDBACK);
	CHECK(n == 0, "entering feedback returned %d", n);
	glPassThrough(7.25f);
	glColor4f(col[0], col[1], col[2], col[3]);
	glBegin(GL_POINTS);
	glVertex3f(obj[0], obj[1], obj[2]);
	glEnd();
	n = glRenderMode(GL_RENDER);
	CHECK(n == 6, "passthrough+point length %d != 6", n);
	CHECK(b[0] == (float)GL_PASS_THROUGH_TOKEN, "passthrough token %.6g not numeric", b[0]);
	CHECK(b[1] == 7.25f, "passthrough value %.6g", b[1]);
	CHECK(b[2] == (float)GL_POINT_TOKEN, "point token %.6g not numeric", b[2]);
	r = mk(obj, col, tex);
	check_vertex("point", &b[3], GL_3D, &r);
	check_tail(b, 6, N);
}

/* every feedback type emits a complete, numerically readable record */
static void test_all_formats(void) {
	GLenum types[5] = {GL_2D, GL_3D, GL_3D_COLOR, GL_3D_COLOR_TEXTURE, GL_4D_COLOR_TEXTURE};
	float obj[4] = {0.25f, -0.5f, 0.25f, 1.0f};
	float col[4] = {0.125f, 0.5f, 0.875f, 0.375f};
	float texin[4] = {0.2f, 0.3f, 0.4f, 1.0f};
	int k;
	setup_projection_ortho();
	/* non identity texture matrix: translate (0.1, 0.2, 0.3) */
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glTranslatef(0.1f, 0.2f, 0.3f);
	texm = mat_translate(0.1f, 0.2f, 0.3f);
	glMatrixMode(GL_MODELVIEW);
	for (k = 0; k < 5; k++) {
		GLenum type = types[k];
		enum { N = 32 };
		float* b = alloc_buffer(N + TAIL);
		RefV r;
		int n, vs = vertex_size(type);
		glColor4f(col[0], col[1], col[2], col[3]);
		glTexCoord4f(texin[0], texin[1], texin[2], texin[3]);
		glFeedbackBuffer(N, type, b);
		glRenderMode(GL_FEEDBACK);
		glBegin(GL_POINTS);
		glVertex3f(obj[0], obj[1], obj[2]);
		glEnd();
		n = glRenderMode(GL_RENDER);
		CHECK(n == 1 + vs, "type %d point length %d != %d", type, n, 1 + vs);
		CHECK(b[0] == (float)GL_POINT_TOKEN, "type %d token not numeric", type);
		r = mk(obj, col, texin);
		check_vertex("format", &b[1], type, &r);
		check_tail(b, n, N);
	}
	reset_texture_matrix();
}

/* line crossing the near plane under perspective, independently clipped */
static void test_line_near_clip(void) {
	enum { N = 32 };
	float* b = alloc_buffer(N + TAIL);
	float o1[4] = {-0.3f, 0.2f, -0.5f, 1.0f};
	float o2[4] = {0.4f, -0.3f, -5.0f, 1.0f};
	float c1[4] = {0.9f, 0.1f, 0.2f, 1.0f};
	float c2[4] = {0.1f, 0.8f, 0.3f, 0.5f};
	float t1[4] = {0.1f, 0.2f, 0.3f, 1.0f};
	float t2[4] = {0.7f, 0.6f, 0.5f, 2.0f};
	RefV a, bb, q1, q2;
	int n;
	setup_projection_perspective();
	reset_texture_matrix();
	a = mk(o1, c1, t1);
	bb = mk(o2, c2, t2);
	CHECK(ref_clip_line(&a, &bb, &q1, &q2), "reference says line is visible");
	glColor4f(1, 1, 1, 1);
	glFeedbackBuffer(N, GL_3D_COLOR_TEXTURE, b);
	glRenderMode(GL_FEEDBACK);
	glColor4f(c1[0], c1[1], c1[2], c1[3]);
	glTexCoord4f(t1[0], t1[1], t1[2], t1[3]);
	glBegin(GL_LINES);
	glVertex3f(o1[0], o1[1], o1[2]);
	glColor4f(c2[0], c2[1], c2[2], c2[3]);
	glTexCoord4f(t2[0], t2[1], t2[2], t2[3]);
	glVertex3f(o2[0], o2[1], o2[2]);
	glEnd();
	n = glRenderMode(GL_RENDER);
	CHECK(n == 1 + 2 * vertex_size(GL_3D_COLOR_TEXTURE), "near clipped line length %d", n);
	CHECK(b[0] == (float)GL_LINE_TOKEN, "line token not numeric");
	check_vertex("near-line-q1", &b[1], GL_3D_COLOR_TEXTURE, &q1);
	check_vertex("near-line-q2", &b[1 + vertex_size(GL_3D_COLOR_TEXTURE)], GL_3D_COLOR_TEXTURE, &q2);
	CHECK(q1.win[2] >= 0.0f && q1.win[2] <= 1.0f, "interpolated depth out of range %.6g", q1.win[2]);
	check_tail(b, n, N);
}

/* line crossing a side plane, including the orthographic case */
static void test_line_side_clip(void) {
	struct case_t {
		float o1[4], o2[4];
	} cases[2];
	int ci;
	memset(cases, 0, sizeof(cases));
	/* perspective side clip, distinct clip w */
	cases[0].o1[0] = 3.0f;
	cases[0].o1[1] = 0.0f;
	cases[0].o1[2] = -2.0f;
	cases[0].o1[3] = 1.0f;
	cases[0].o2[0] = -0.5f;
	cases[0].o2[1] = 0.1f;
	cases[0].o2[2] = -4.0f;
	cases[0].o2[3] = 1.0f;
	/* orthographic side clip, w stays 1 */
	cases[1].o1[0] = 1.5f;
	cases[1].o1[1] = 0.0f;
	cases[1].o1[2] = 0.0f;
	cases[1].o1[3] = 1.0f;
	cases[1].o2[0] = 0.2f;
	cases[1].o2[1] = 0.0f;
	cases[1].o2[2] = 0.0f;
	cases[1].o2[3] = 1.0f;
	for (ci = 0; ci < 2; ci++) {
		enum { N = 32 };
		float* b = alloc_buffer(N + TAIL);
		float col[4] = {0.4f, 0.5f, 0.6f, 0.7f};
		float tx[4] = {0.2f, 0.4f, 0.6f, 1.0f};
		RefV a, bb, q1, q2;
		int n;
		if (ci == 0)
			setup_projection_perspective();
		else
			setup_projection_ortho();
		reset_texture_matrix();
		a = mk(cases[ci].o1, col, tx);
		bb = mk(cases[ci].o2, col, tx);
		CHECK(ref_clip_line(&a, &bb, &q1, &q2), "side line reference visible (case %d)", ci);
		glFeedbackBuffer(N, GL_4D_COLOR_TEXTURE, b);
		glRenderMode(GL_FEEDBACK);
		glColor4f(col[0], col[1], col[2], col[3]);
		glTexCoord4f(tx[0], tx[1], tx[2], tx[3]);
		glBegin(GL_LINES);
		glVertex3f(cases[ci].o1[0], cases[ci].o1[1], cases[ci].o1[2]);
		glVertex3f(cases[ci].o2[0], cases[ci].o2[1], cases[ci].o2[2]);
		glEnd();
		n = glRenderMode(GL_RENDER);
		CHECK(n == 1 + 2 * vertex_size(GL_4D_COLOR_TEXTURE), "side line length %d (case %d)", n, ci);
		CHECK(b[0] == (float)GL_LINE_TOKEN, "side line token");
		check_vertex("side-line-q1", &b[1], GL_4D_COLOR_TEXTURE, &q1);
		check_vertex("side-line-q2", &b[1 + vertex_size(GL_4D_COLOR_TEXTURE)], GL_4D_COLOR_TEXTURE, &q2);
		if (ci == 0) {
			/* the two endpoints carry different clip w values */
			CHECK(feq(q1.win[3], a.pc[3] + (bb.pc[3] - a.pc[3]) * (plane_d(1, a.pc) / (plane_d(1, a.pc) - plane_d(1, bb.pc))), 1e-4f), "clip w interpolation");
			CHECK(q1.win[3] > 1.5f && q2.win[3] > 3.0f, "expected varied clip w, got %.6g %.6g", q1.win[3], q2.win[3]);
		} else {
			CHECK(feq(q1.win[3], 1.0f, 1e-6f) && feq(q2.win[3], 1.0f, 1e-6f), "ortho clip w must be 1");
		}
		check_tail(b, n, N);
	}
}

static void feed_triangle(const float o[3][4], const float c[3][4], const float tx[3][4]) {
	int i;
	glBegin(GL_TRIANGLES);
	for (i = 0; i < 3; i++) {
		glColor4f(c[i][0], c[i][1], c[i][2], c[i][3]);
		glTexCoord4f(tx[i][0], tx[i][1], tx[i][2], tx[i][3]);
		glVertex3f(o[i][0], o[i][1], o[i][2]);
	}
	glEnd();
}

/* triangle crossing only the near plane: the fan emits two polygon records */
static void test_triangle_near_clip(void) {
	enum { N = 128 };
	float* b = alloc_buffer(N + TAIL);
	/* near vertex is in front of the near plane but inside x/y; the two far
	   vertices are fully inside, so exactly one clip plane is involved */
	float o[3][4] = {{0.0f, 0.0f, -0.5f, 1.0f}, {-0.3f, -0.3f, -5.0f, 1.0f}, {0.3f, -0.3f, -5.0f, 1.0f}};
	float c[3][4] = {{0.9f, 0.2f, 0.1f, 1.0f}, {0.1f, 0.9f, 0.2f, 0.8f}, {0.2f, 0.1f, 0.9f, 0.6f}};
	float tx[3][4] = {{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.2f, 1.0f}, {0.0f, 1.0f, 0.4f, 2.0f}};
	RefV in[3], ref[16];
	int nref, n, idx = 0, polys = 0, polyverts = 0, vs = vertex_size(GL_3D_COLOR_TEXTURE);
	setup_projection_perspective();
	reset_texture_matrix();
	in[0] = mk(o[0], c[0], tx[0]);
	in[1] = mk(o[1], c[1], tx[1]);
	in[2] = mk(o[2], c[2], tx[2]);
	nref = ref_clip_poly(in, 3, ref, 16);
	CHECK(nref == 4, "near clip reference polygon has %d != 4 vertices", nref);
	glFeedbackBuffer(N, GL_3D_COLOR_TEXTURE, b);
	glRenderMode(GL_FEEDBACK);
	feed_triangle(o, c, tx);
	n = glRenderMode(GL_RENDER);
	/* parse every polygon record structurally */
	while (idx < n) {
		int nv, j;
		CHECK(b[idx] == (float)GL_POLYGON_TOKEN, "expected polygon token, got %.6g", b[idx]);
		nv = (int)b[idx + 1];
		CHECK(nv == 3, "fan record must hold 3 vertices, got %d", nv);
		for (j = 0; j < nv; j++) {
			CHECK(b[idx + 2 + j * vs + 2] >= 0.0f && b[idx + 2 + j * vs + 2] <= 1.0f, "clipped depth outside [0,1]");
		}
		polyverts += nv;
		polys++;
		idx += 2 + nv * vs;
	}
	CHECK(idx == n, "parsed %d of %d floats", idx, n);
	CHECK(polys == 2, "near clipped triangle emitted %d != 2 fan records", polys);
	CHECK(polyverts == 6, "fan produced %d != 6 vertex slots", polyverts);
	check_vertex_set("near-tri", &b[2], 3, GL_3D_COLOR_TEXTURE, ref, nref);
	check_vertex_set("near-tri", &b[2 + 2 + 3 * vs], 3, GL_3D_COLOR_TEXTURE, ref, nref);
	check_tail(b, n, N);
}

/* triangle crossing a side plane */
static void test_triangle_side_clip(void) {
	enum { N = 128 };
	float* b = alloc_buffer(N + TAIL);
	float o[3][4] = {{3.0f, 0.2f, -2.0f, 1.0f}, {-0.8f, -0.5f, -3.0f, 1.0f}, {0.2f, -0.5f, -3.0f, 1.0f}};
	float c[3][4] = {{0.7f, 0.7f, 0.1f, 1.0f}, {0.1f, 0.2f, 0.7f, 1.0f}, {0.6f, 0.2f, 0.6f, 0.9f}};
	float tx[3][4] = {{0.5f, 0.5f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.1f, 1.0f}};
	RefV in[3], ref[16];
	int nref, n, idx = 0, polys = 0, polyverts = 0, vs = vertex_size(GL_3D_COLOR_TEXTURE);
	setup_projection_perspective();
	reset_texture_matrix();
	in[0] = mk(o[0], c[0], tx[0]);
	in[1] = mk(o[1], c[1], tx[1]);
	in[2] = mk(o[2], c[2], tx[2]);
	nref = ref_clip_poly(in, 3, ref, 16);
	CHECK(nref == 4, "side clip reference polygon has %d != 4 vertices", nref);
	glFeedbackBuffer(N, GL_3D_COLOR_TEXTURE, b);
	glRenderMode(GL_FEEDBACK);
	feed_triangle(o, c, tx);
	n = glRenderMode(GL_RENDER);
	while (idx < n) {
		int nv;
		CHECK(b[idx] == (float)GL_POLYGON_TOKEN, "side: expected polygon token");
		nv = (int)b[idx + 1];
		CHECK(nv == 3, "side: fan vertex count %d", nv);
		polyverts += nv;
		polys++;
		idx += 2 + nv * vs;
	}
	CHECK(idx == n, "side parse length");
	CHECK(polys == 2 && polyverts == 6, "side fan polys=%d verts=%d", polys, polyverts);
	check_vertex_set("side-tri", &b[2], 3, GL_3D_COLOR_TEXTURE, ref, nref);
	check_vertex_set("side-tri", &b[2 + 2 + 3 * vs], 3, GL_3D_COLOR_TEXTURE, ref, nref);
	check_tail(b, n, N);
}

static void test_triangle_inside_and_outside(void) {
	/* fully inside: exact ordered comparison */
	{
		enum { N = 64 };
		float* b = alloc_buffer(N + TAIL);
		float o[3][4] = {{-0.5f, -0.4f, -2.0f, 1.0f}, {0.5f, -0.4f, -2.0f, 1.0f}, {0.0f, 0.6f, -3.0f, 1.0f}};
		float c[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
		float tx[3][4] = {{0, 0, 0, 1}, {1, 0, 0, 1}, {0, 1, 0, 1}};
		RefV r[3];
		int n, vs = vertex_size(GL_3D_COLOR_TEXTURE), i;
		setup_projection_perspective();
		reset_texture_matrix();
		for (i = 0; i < 3; i++)
			r[i] = mk(o[i], c[i], tx[i]);
		glFeedbackBuffer(N, GL_3D_COLOR_TEXTURE, b);
		glRenderMode(GL_FEEDBACK);
		feed_triangle(o, c, tx);
		n = glRenderMode(GL_RENDER);
		CHECK(n == 2 + 3 * vs, "inside triangle length %d", n);
		CHECK(b[0] == (float)GL_POLYGON_TOKEN && b[1] == 3.0f, "inside triangle header");
		for (i = 0; i < 3; i++)
			check_vertex("inside-tri", &b[2 + i * vs], GL_3D_COLOR_TEXTURE, &r[i]);
		check_tail(b, n, N);
	}
	/* fully outside: nothing may be written */
	{
		enum { N = 16 };
		float* b = alloc_buffer(N + TAIL);
		int n;
		setup_projection_perspective();
		glFeedbackBuffer(N, GL_3D, b);
		glRenderMode(GL_FEEDBACK);
		glBegin(GL_TRIANGLES);
		glVertex3f(-0.5f, 0.0f, -12.0f);
		glVertex3f(0.5f, 0.0f, -12.0f);
		glVertex3f(0.0f, 0.5f, -11.5f);
		glEnd();
		n = glRenderMode(GL_RENDER);
		CHECK(n == 0, "fully outside triangle wrote %d floats", n);
		check_tail(b, 0, N);
	}
}

/* capacity: exact fit, one float short, and pointer lock after overflow */
static void test_capacity(void) {
	/* GL_3D point record is 4 floats; exact fit then one short */
	{
		float exact[4 + TAIL];
		float shortb[3 + TAIL];
		int i, n;
		for (i = 0; i < 4 + TAIL; i++)
			exact[i] = SENTINEL;
		for (i = 0; i < 3 + TAIL; i++)
			shortb[i] = SENTINEL;
		setup_projection_ortho();
		glFeedbackBuffer(4, GL_3D, exact);
		glRenderMode(GL_FEEDBACK);
		glBegin(GL_POINTS);
		glVertex3f(0, 0, 0);
		glEnd();
		n = glRenderMode(GL_RENDER);
		CHECK(n == 4, "exact fit returned %d", n);
		CHECK(exact[0] == (float)GL_POINT_TOKEN, "exact fit token");
		for (i = 4; i < 4 + TAIL; i++)
			CHECK(exact[i] == SENTINEL, "exact fit tail %d", i);

		glFeedbackBuffer(3, GL_3D, shortb);
		glRenderMode(GL_FEEDBACK);
		glBegin(GL_POINTS);
		glVertex3f(0, 0, 0);
		glEnd();
		n = glRenderMode(GL_RENDER);
		CHECK(n == -1, "one float short must return -1, got %d", n);
		for (i = 0; i < 3 + TAIL; i++)
			CHECK(shortb[i] == SENTINEL, "failed record must not touch buffer at %d", i);
	}
	/* GL_3D_COLOR polygon record is 23 floats; 22 fits nothing */
	{
		float b[22 + TAIL];
		int i, n;
		for (i = 0; i < 22 + TAIL; i++)
			b[i] = SENTINEL;
		glFeedbackBuffer(22, GL_3D_COLOR, b);
		glRenderMode(GL_FEEDBACK);
		glBegin(GL_TRIANGLES);
		glVertex3f(-0.2f, -0.2f, 0);
		glVertex3f(0.2f, -0.2f, 0);
		glVertex3f(0, 0.2f, 0);
		glEnd();
		n = glRenderMode(GL_RENDER);
		CHECK(n == -1, "polygon one short returned %d", n);
		for (i = 0; i < 22 + TAIL; i++)
			CHECK(b[i] == SENTINEL, "overflow touched index %d", i);
	}
	/* partial: passthrough (2) fits, point (4) overflows; pointer then locked */
	{
		enum { N = 5 };
		float* b = alloc_buffer(N + TAIL);
		int n;
		glFeedbackBuffer(N, GL_3D, b);
		glRenderMode(GL_FEEDBACK);
		glPassThrough(1.5f);
		glBegin(GL_POINTS);
		glVertex3f(0, 0, 0);
		glEnd();
		glPassThrough(2.5f); /* must be ignored after overflow */
		n = glRenderMode(GL_RENDER);
		CHECK(n == -2, "partial overflow returned %d != -2", n);
		CHECK(b[0] == (float)GL_PASS_THROUGH_TOKEN && b[1] == 1.5f, "passthrough before overflow");
		check_tail(b, 2, N);
	}
}

/* display lists: compile vs compile and execute, repeated calls */
static void test_lists(void) {
	enum { N = 256 };
	float direct[64], *b = alloc_buffer(N + TAIL);
	float obj[4] = {0.3f, -0.2f, -2.0f, 1.0f};
	float col[4] = {0.33f, 0.44f, 0.55f, 0.66f};
	float texin[4] = {0.25f, 0.5f, 0.75f, 2.0f};
	RefV r;
	int nd, n, i;
	setup_projection_perspective();
	/* texture translate stays in effect; modelview is reset inside the
	   recorded sequence so repeated list calls are identical */
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glTranslatef(0.1f, 0.2f, 0.3f);
	texm = mat_translate(0.1f, 0.2f, 0.3f);
	glMatrixMode(GL_MODELVIEW);
	model = mat_translate(0.05f, -0.05f, 0.0f);

#define EMIT_GEOMETRY()                                                                                                                                  \
	do {                                                                                                                                                 \
		glMatrixMode(GL_MODELVIEW);                                                                                                                      \
		glLoadIdentity();                                                                                                                                \
		glTranslatef(0.05f, -0.05f, 0.0f);                                                                                                               \
		glColor4f(col[0], col[1], col[2], col[3]);                                                                                                      \
		glTexCoord4f(texin[0], texin[1], texin[2], texin[3]);                                                                                           \
		glPassThrough(9.0f);                                                                                                                            \
		glBegin(GL_POINTS);                                                                                                                             \
		glVertex3f(obj[0], obj[1], obj[2]);                                                                                                             \
		glEnd();                                                                                                                                        \
	} while (0)

	/* direct execution reference stream */
	for (i = 0; i < 64; i++)
		direct[i] = SENTINEL;
	glFeedbackBuffer(64, GL_3D_COLOR_TEXTURE, direct);
	glRenderMode(GL_FEEDBACK);
	EMIT_GEOMETRY();
	nd = glRenderMode(GL_RENDER);
	CHECK(nd == 2 + 1 + vertex_size(GL_3D_COLOR_TEXTURE), "direct list-equivalent length %d", nd);
	r = mk(obj, col, texin);

	/* GL_COMPILE: nothing while compiling, output only on call, in the then current mode */
	glNewList(1, GL_COMPILE);
	EMIT_GEOMETRY();
	glEndList();
	glFeedbackBuffer(N, GL_3D_COLOR_TEXTURE, b);
	glRenderMode(GL_FEEDBACK);
	/* no emission yet */
	glCallList(1);
	glCallList(1); /* repeated calls keep producing fresh records */
	n = glRenderMode(GL_RENDER);
	CHECK(n == 2 * nd, "compiled list twice gave %d != %d", n, 2 * nd);
	CHECK(memcmp(direct, b, sizeof(float) * (size_t)nd) == 0, "compiled list stream differs from direct");
	CHECK(memcmp(b, b + nd, sizeof(float) * (size_t)nd) == 0, "repeated list call differs");
	check_vertex("compile-list", &b[2 + 1], GL_3D_COLOR_TEXTURE, &r);
	check_tail(b, n, N);

	/* GL_COMPILE_AND_EXECUTE: emits immediately and records */
	b = alloc_buffer(N + TAIL);
	glFeedbackBuffer(N, GL_3D_COLOR_TEXTURE, b);
	glRenderMode(GL_FEEDBACK);
	glNewList(2, GL_COMPILE_AND_EXECUTE);
	EMIT_GEOMETRY();
	glEndList();
	glCallList(2);
	n = glRenderMode(GL_RENDER);
	CHECK(n == 2 * nd, "compile-and-execute gave %d != %d", n, 2 * nd);
	CHECK(memcmp(direct, b, sizeof(float) * (size_t)nd) == 0, "immediate list stream differs");
	CHECK(memcmp(b, b + nd, sizeof(float) * (size_t)nd) == 0, "recorded list replay differs");
	check_tail(b, n, N);

	/* a list compiled outside feedback stays silent until called inside it */
	setup_projection_ortho();
	reset_texture_matrix();
	glNewList(3, GL_COMPILE);
	glBegin(GL_POINTS);
	glVertex3f(0, 0, 0);
	glEnd();
	glEndList();
	b = alloc_buffer(N + TAIL);
	glFeedbackBuffer(N, GL_3D, b);
	glRenderMode(GL_FEEDBACK);
	glCallList(3);
	n = glRenderMode(GL_RENDER);
	CHECK(n == 1 + vertex_size(GL_3D), "list called in feedback emitted %d", n);
	CHECK(b[0] == (float)GL_POINT_TOKEN, "list point token");
	check_tail(b, n, N);
#undef EMIT_GEOMETRY
}

/* feedback must not touch the frame or depth buffer; rendering still works */
static void test_no_raster_side_effects(void) {
	unsigned before_p[16], after_p[16];
	unsigned short before_z[16], after_z[16];
	int i, lit = 0;
	setup_projection_ortho();
	reset_texture_matrix();
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	for (i = 0; i < 16; i++) {
		before_p[i] = z->pbuf[i];
		before_z[i] = z->zbuf[i];
	}
	{
		float b[64];
		glFeedbackBuffer(64, GL_3D_COLOR, b);
		glRenderMode(GL_FEEDBACK);
		glColor4f(1, 1, 1, 1);
		glBegin(GL_TRIANGLES);
		glVertex3f(-0.8f, -0.8f, 0);
		glVertex3f(0.8f, -0.8f, 0);
		glVertex3f(0, 0.8f, 0);
		glEnd();
		CHECK(glRenderMode(GL_RENDER) > 0, "feedback triangle produced no record");
	}
	for (i = 0; i < 16; i++) {
		after_p[i] = z->pbuf[i];
		after_z[i] = z->zbuf[i];
		CHECK(after_p[i] == before_p[i], "feedback modified color buffer at %d", i);
		CHECK(after_z[i] == before_z[i], "feedback modified depth buffer at %d", i);
	}
	/* ordinary rendering after feedback still rasterizes */
	glColor3f(1, 0, 0);
	glBegin(GL_TRIANGLES);
	glVertex3f(-0.8f, -0.8f, 0);
	glVertex3f(0.8f, -0.8f, 0);
	glVertex3f(0, 0.8f, 0);
	glEnd();
	for (i = 0; i < z->xsize * z->ysize; i++)
		if ((z->pbuf[i] & 0xffffff) != 0)
			lit++;
	CHECK(lit > 50, "post feedback render lit only %d pixels", lit);
}

/* after an overflow, rebinding and starting another session works again */
static void test_reuse_after_overflow(void) {
	float small[3], big[16 + TAIL];
	int i, n;
	setup_projection_ortho();
	for (i = 0; i < 3; i++)
		small[i] = SENTINEL;
	for (i = 0; i < 16 + TAIL; i++)
		big[i] = SENTINEL;
	glFeedbackBuffer(3, GL_3D, small);
	glRenderMode(GL_FEEDBACK);
	glBegin(GL_POINTS);
	glVertex3f(0, 0, 0);
	glEnd();
	n = glRenderMode(GL_RENDER);
	CHECK(n == -1, "overflow session returned %d", n);
	/* rebind a fresh buffer and continue */
	glFeedbackBuffer(16, GL_3D, big);
	glRenderMode(GL_FEEDBACK);
	glBegin(GL_POINTS);
	glVertex3f(0, 0, 0);
	glEnd();
	n = glRenderMode(GL_RENDER);
	CHECK(n == 4, "rebound feedback returned %d", n);
	CHECK(big[0] == (float)GL_POINT_TOKEN, "rebound token");
	/* re-enter on the same buffer without rebinding */
	glRenderMode(GL_FEEDBACK);
	glBegin(GL_POINTS);
	glVertex3f(0, 0, 0);
	glEnd();
	n = glRenderMode(GL_RENDER);
	CHECK(n == 4, "re-entered feedback returned %d", n);
	check_tail(big, 4, 16);
}

/* point, line and triangle each work in every feedback type */
static void test_primitives_all_formats(void) {
	GLenum types[5] = {GL_2D, GL_3D, GL_3D_COLOR, GL_3D_COLOR_TEXTURE, GL_4D_COLOR_TEXTURE};
	float po[4] = {0.2f, 0.1f, 0.3f, 1.0f};
	float lo[2][4] = {{-0.6f, -0.4f, 0.2f, 1.0f}, {0.5f, 0.3f, -0.2f, 1.0f}};
	float to[3][4] = {{-0.5f, -0.5f, 0.0f, 1.0f}, {0.5f, -0.5f, 0.0f, 1.0f}, {0.0f, 0.5f, 0.0f, 1.0f}};
	float col[4] = {0.31f, 0.62f, 0.83f, 0.47f};
	float tex[4] = {0.13f, 0.27f, 0.59f, 2.0f};
	int k;
	setup_projection_ortho();
	reset_texture_matrix();
	for (k = 0; k < 5; k++) {
		GLenum type = types[k];
		enum { N = 128 };
		float* b = alloc_buffer(N + TAIL);
		int vs = vertex_size(type), n, idx;
		RefV rp, rl0, rl1, rt[3];
		glColor4f(col[0], col[1], col[2], col[3]);
		glTexCoord4f(tex[0], tex[1], tex[2], tex[3]);
		rp = mk(po, col, tex);
		rl0 = mk(lo[0], col, tex);
		rl1 = mk(lo[1], col, tex);
		rt[0] = mk(to[0], col, tex);
		rt[1] = mk(to[1], col, tex);
		rt[2] = mk(to[2], col, tex);
		glFeedbackBuffer(N, type, b);
		glRenderMode(GL_FEEDBACK);
		glBegin(GL_POINTS);
		glVertex3f(po[0], po[1], po[2]);
		glEnd();
		glBegin(GL_LINES);
		glVertex3f(lo[0][0], lo[0][1], lo[0][2]);
		glVertex3f(lo[1][0], lo[1][1], lo[1][2]);
		glEnd();
		glBegin(GL_TRIANGLES);
		glVertex3f(to[0][0], to[0][1], to[0][2]);
		glVertex3f(to[1][0], to[1][1], to[1][2]);
		glVertex3f(to[2][0], to[2][1], to[2][2]);
		glEnd();
		n = glRenderMode(GL_RENDER);
		CHECK(n == (1 + vs) + (1 + 2 * vs) + (2 + 3 * vs), "format %d combined length %d", type, n);
		idx = 0;
		CHECK(b[idx] == (float)GL_POINT_TOKEN, "fmt %d point token", type);
		check_vertex("fmt-point", &b[idx + 1], type, &rp);
		idx += 1 + vs;
		CHECK(b[idx] == (float)GL_LINE_TOKEN, "fmt %d line token", type);
		check_vertex("fmt-line0", &b[idx + 1], type, &rl0);
		check_vertex("fmt-line1", &b[idx + 1 + vs], type, &rl1);
		idx += 1 + 2 * vs;
		CHECK(b[idx] == (float)GL_POLYGON_TOKEN && b[idx + 1] == 3.0f, "fmt %d polygon header", type);
		{
			int j;
			for (j = 0; j < 3; j++)
				check_vertex("fmt-tri", &b[idx + 2 + j * vs], type, &rt[j]);
		}
		idx += 2 + 3 * vs;
		CHECK(idx == n, "fmt %d parsed %d of %d", type, idx, n);
		check_tail(b, n, N);
	}
}

/* glFeedbackBuffer stays immediate and its address is never recorded */
static void test_feedback_buffer_not_listed(void) {
	enum { N = 32 };
	float* ba = alloc_buffer(N + TAIL);
	float bb[32];
	int i, n;
	for (i = 0; i < 32; i++)
		bb[i] = SENTINEL;
	setup_projection_ortho();
	reset_texture_matrix();
	glFeedbackBuffer(N, GL_3D, ba);
	glNewList(9, GL_COMPILE);
	/* immediate rebind: must not be replayed from the list later */
	glFeedbackBuffer(N, GL_3D, bb);
	glBegin(GL_POINTS);
	glVertex3f(0.1f, 0.1f, 0.0f);
	glEnd();
	glEndList();
	/* restore the buffer the caller wants the list to write to */
	glFeedbackBuffer(N, GL_3D, ba);
	glRenderMode(GL_FEEDBACK);
	glCallList(9);
	n = glRenderMode(GL_RENDER);
	CHECK(n == 1 + vertex_size(GL_3D), "listed geometry emitted %d floats", n);
	CHECK(ba[0] == (float)GL_POINT_TOKEN, "record went to the rebound buffer");
	for (i = 0; i < 32; i++)
		CHECK(bb[i] == SENTINEL, "list replayed a captured feedback buffer at %d", i);
	check_tail(ba, n, N);
}

/* GL_SELECT must keep behaving as before */
static void test_select_smoke(void) {
	GLuint sb[32];
	int hits, i;
	for (i = 0; i < 32; i++)
		sb[i] = 0;
	setup_projection_ortho();
	glSelectBuffer(32, sb);
	CHECK(glRenderMode(GL_SELECT) == 0, "enter select");
	glInitNames();
	glPushName(7);
	glBegin(GL_POINTS);
	glVertex3f(0, 0, 0);
	glEnd();
	hits = glRenderMode(GL_RENDER);
	CHECK(hits == 1, "select hits %d", hits);
	CHECK(sb[0] == 1 && sb[3] == 7, "select record contents");
}

int main(void) {
	z = ZB_open(VP_XSIZE + 8, VP_YSIZE + 8, ZB_MODE_RGBA, 0);
	if (!z) {
		printf("FAIL: cannot open zbuffer\n");
		return 1;
	}
	glInit(z);
	glViewport(VP_XMIN, VP_YMIN, VP_XSIZE, VP_YSIZE);

	test_passthrough_and_point();
	test_all_formats();
	test_primitives_all_formats();
	test_line_near_clip();
	test_line_side_clip();
	test_triangle_near_clip();
	test_triangle_side_clip();
	test_triangle_inside_and_outside();
	test_capacity();
	test_lists();
	test_feedback_buffer_not_listed();
	test_no_raster_side_effects();
	test_reuse_after_overflow();
	test_select_smoke();

	glClose();
	ZB_close(z);
	if (failures) {
		printf("TINYGL_TARGET_FAILURES %d\n", failures);
		return 1;
	}
	puts("TINYGL_TARGET_OK");
	return 0;
}
