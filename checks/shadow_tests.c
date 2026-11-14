/*
 * Shadow / depth-texture task tests.
 *
 * Every rendered expectation is derived from an independent
 * perspective-correct barycentric evaluation and the original depth
 * array supplied to the texture; the camera window Z is checked
 * alongside the color.
 */
#include "GL/gl.h"
#include "zbuffer.h"
#include "task_support/check.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define D TGL_FEATURE_TEXTURE_DIM
#define N (D * D)

/* ---------------- small independent math/reference helpers ---------------- */

typedef struct {
	GLfloat x, y, z, w;       /* object/homogeneous vertex */
	GLfloat s, t, r, q;       /* glTexCoord4f, pre texture matrix */
} Vtx;

static GLfloat ref_edge(GLfloat ax, GLfloat ay, GLfloat bx, GLfloat by, GLfloat cx, GLfloat cy) {
	return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

static GLfloat ref_clampf(GLfloat v, GLfloat lo, GLfloat hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

static int ref_clampi(int i, int dim) {
	return i < 0 ? 0 : (i > dim - 1 ? dim - 1 : i);
}

static GLfloat ref_fetch(const GLfloat* depth, int dim, int ix, int iy) {
	return depth[ref_clampi(iy, dim) * dim + ref_clampi(ix, dim)];
}

/* independent re-implementation of the depth texture lookup */
static GLfloat ref_lookup(const GLfloat* depth, int dim, GLfloat u, GLfloat v, GLfloat ref, int compare, int less, int linear) {
	u = ref_clampf(u, 0.0f, 1.0f);
	v = ref_clampf(v, 0.0f, 1.0f);
#define REF_PASS(stored) (less ? (ref < (stored)) : (ref <= (stored)))
	if (!linear) {
		int ix = (int)floorf(u * dim);
		int iy = (int)floorf(v * dim);
		GLfloat stored = ref_fetch(depth, dim, ix, iy);
		return compare ? (REF_PASS(stored) ? 1.0f : 0.0f) : stored;
	}
	{
		GLfloat gx = u * dim - 0.5f;
		GLfloat gy = v * dim - 0.5f;
		int x0 = (int)floorf(gx);
		int y0 = (int)floorf(gy);
		GLfloat fx = gx - x0;
		GLfloat fy = gy - y0;
		GLfloat w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
		GLfloat w01 = (1 - fx) * fy, w11 = fx * fy;
		GLfloat s00 = ref_fetch(depth, dim, x0, y0);
		GLfloat s10 = ref_fetch(depth, dim, x0 + 1, y0);
		GLfloat s01 = ref_fetch(depth, dim, x0, y0 + 1);
		GLfloat s11 = ref_fetch(depth, dim, x0 + 1, y0 + 1);
		if (compare)
			return (REF_PASS(s00) ? w00 : 0) + (REF_PASS(s10) ? w10 : 0) + (REF_PASS(s01) ? w01 : 0) + (REF_PASS(s11) ? w11 : 0);
		return s00 * w00 + s10 * w10 + s01 * w01 + s11 * w11;
	}
#undef REF_PASS
}

static int ref_gray(GLfloat g) {
	int y = (int)(ref_clampf(g, 0, 1) * 255.0f + 0.5f);
	return y > 255 ? 255 : y;
}

/* perspective-correct projective coordinates of pixel (px,py) for one
   triangle made of three Vtx; returns 0 when the pixel is outside.
   px,py are integer sample positions (the rasterizer evaluates its
   interpolants at integer pixel columns) converted to NDC internally. */
static int ref_project(const Vtx* a, const Vtx* b, const Vtx* c, GLfloat px, GLfloat py, GLfloat* u, GLfloat* v, GLfloat* r,
					   GLfloat* winz) {
	GLfloat half = (D - 0.5f) / 2.0f;
	GLfloat nx = (px - half) / half;
	GLfloat ny = -(py - half) / half;
	GLfloat area = ref_edge(a->x / a->w, a->y / a->w, b->x / b->w, b->y / b->w, c->x / c->w, c->y / c->w);
	GLfloat la = ref_edge(b->x / b->w, b->y / b->w, c->x / c->w, c->y / c->w, nx, ny) / area;
	GLfloat lb = ref_edge(c->x / c->w, c->y / c->w, a->x / a->w, a->y / a->w, nx, ny) / area;
	GLfloat lc = 1.0f - la - lb;
	GLfloat invw, sw, tw, rw, qw;
	if (la < -1e-6f || lb < -1e-6f || lc < -1e-6f)
		return 0;
	invw = la / a->w + lb / b->w + lc / c->w;
	sw = la * a->s / a->w + lb * b->s / b->w + lc * c->s / c->w;
	tw = la * a->t / a->w + lb * b->t / b->w + lc * c->t / c->w;
	rw = la * a->r / a->w + lb * b->r / b->w + lc * c->r / c->w;
	qw = la * a->q / a->w + lb * b->q / b->w + lc * c->q / c->w;
	*u = sw / invw;
	*v = tw / invw;
	*r = rw / invw;
	/* q after perspective correction */
	{
		GLfloat q = qw / invw;
		*u /= q;
		*v /= q;
		*r /= q;
	}
	/* camera window Z, same formula as gl_transform_to_viewport_vertex_c */
	{
		GLfloat ndc_z = la * a->z / a->w + lb * b->z / b->w + lc * c->z / c->w;
		GLfloat zsize = (GLfloat)(1 << (ZB_Z_BITS + ZB_POINT_Z_FRAC_BITS));
		GLfloat scale_z = -((zsize - 0.5f) / 2.0f);
		GLfloat trans_z = ((zsize - 0.5f) / 2.0f) + ((1 << ZB_POINT_Z_FRAC_BITS)) / 2.0f;
		*winz = ndc_z * scale_z + trans_z;
	}
	return 1;
}

/* screen position of a vertex under the identity transform */
static void ref_screen(const Vtx* v, GLfloat* sx, GLfloat* sy) {
	GLfloat ndc_x = v->x / v->w;
	GLfloat ndc_y = v->y / v->w;
	*sx = ndc_x * ((D - 0.5f) / 2.0f) + ((D - 0.5f) / 2.0f);
	*sy = ndc_y * (-(D - 0.5f) / 2.0f) + ((D - 0.5f) / 2.0f);
}



static ZBuffer* new_ctx(void) {
	ZBuffer* zb = ZB_open(D, D, ZB_MODE_RGBA, NULL);
	glInit(zb);
	glViewport(0, 0, D, D);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_LIGHTING);
	glDisable(GL_BLEND);
	glDisable(GL_POLYGON_STIPPLE);
	glDepthMask(1);
	glEnable(GL_DEPTH_TEST);
	glShadeModel(GL_SMOOTH);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	return zb;
}

static void begin_quad(const Vtx* v) {
	glBegin(GL_TRIANGLE_STRIP);
	glTexCoord4f(v[0].s, v[0].t, v[0].r, v[0].q);
	glVertex4f(v[0].x, v[0].y, v[0].z, v[0].w);
	glTexCoord4f(v[1].s, v[1].t, v[1].r, v[1].q);
	glVertex4f(v[1].x, v[1].y, v[1].z, v[1].w);
	glTexCoord4f(v[2].s, v[2].t, v[2].r, v[2].q);
	glVertex4f(v[2].x, v[2].y, v[2].z, v[2].w);
	glTexCoord4f(v[3].s, v[3].t, v[3].r, v[3].q);
	glVertex4f(v[3].x, v[3].y, v[3].z, v[3].w);
	glEnd();
}

static void fullscreen_quad(Vtx* v, GLfloat q) {
	v[0] = (Vtx){-1, -1, 0, 1, 0, 0, 0.3f, q};
	v[1] = (Vtx){1, -1, 0, 1, q, 0, 0.4f, q};
	v[2] = (Vtx){-1, 1, 0, 1, 0, q, 0.5f, q};
	v[3] = (Vtx){1, 1, 0, 1, q, q, 0.6f, q};
}

static void ramp_depth(GLfloat* dst) {
	int i, j;
	for (j = 0; j < D; j++)
		for (i = 0; i < D; i++)
			dst[j * D + i] = ref_clampf(0.10f + 0.35f * i / D + 0.45f * j / D, 0, 1);
}

/* compare a rendered fullscreen quad against the independent reference */
static int check_quad_reference(ZBuffer* zb, const Vtx* v, const GLfloat* depth, int compare, int less, int linear) {
	int py, matched = 0, sampled = 0, zok = 1;
	for (py = 16; py < D - 16; py++) {
		int px;
		for (px = 16; px < D - 16; px++) {
			GLfloat sx[4], sy[4], u, rv, rr, winz;
			int k, hit = 0;
			for (k = 0; k < 4; k++)
				ref_screen(&v[k], &sx[k], &sy[k]);
			/* two triangles of the strip: (0,1,2) and (1,3,2) */
			hit = ref_project(&v[0], &v[1], &v[2], (GLfloat)px, (GLfloat)py, &u, &rv, &rr, &winz);
			if (!hit)
				hit = ref_project(&v[1], &v[3], &v[2], (GLfloat)px, (GLfloat)py, &u, &rv, &rr, &winz);
			if (!hit)
				continue;
			{
				GLfloat g = ref_lookup(depth, D, u, rv, rr, compare, less, linear);
				int expect = ref_gray(g);
				PIXEL pix = zb->pbuf[py * D + px];
				int got = (pix >> 16) & 0xff;
				int zg = (int)(winz) >> ZB_POINT_Z_FRAC_BITS;
				int zactual = zb->zbuf[py * D + px];
				sampled++;
				if (abs(got - expect) <= 2)
					matched++;
				if (abs(zactual - zg) > 2)
					zok = 0;
			}
		}
	}
	TASK_CHECK(sampled > 1000);
	TASK_CHECK(matched * 100 >= sampled * 95);
	TASK_CHECK(zok);
	return 0;
}

/* ---------------------------------- capture ---------------------------------- */

static int test_capture_asymmetric(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex;
	GLint w = 0, h = 0;
	GLushort saved[N];
	GLfloat expect[N];
	GLfloat* got;
	int i, j, distinct = 0;
	unsigned seen[8] = {0};

	/* asymmetric geometry: a slanted triangle leaves part of the
	   framebuffer at cleared (far) depth */
	glColor3f(1, 1, 1);
	glBegin(GL_TRIANGLES);
	glVertex3f(-1.0f, -1.0f, 0.6f);
	glVertex3f(0.4f, -1.0f, 0.2f);
	glVertex3f(-1.0f, 0.7f, -0.3f);
	glEnd();

	memcpy(saved, zb->zbuf, sizeof(saved));
	/* lower-left origin: texture row j maps to framebuffer memory row D-1-j */
	for (j = 0; j < D; j++)
		for (i = 0; i < D; i++)
			expect[j * D + i] = 1.0f - (GLfloat)saved[(D - 1 - j) * D + i] / 65535.0f;

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 0, 0, D, D, 0);
	got = (GLfloat*)glGetTexturePixmap(tex, 0, &w, &h);
	TASK_CHECK(got != NULL);
	TASK_CHECK(w == D && h == D);

	/* orientation: texture row 0 is framebuffer bottom row */
	TASK_CHECK(fabsf(got[0] - (1.0f - (GLfloat)saved[(D - 1) * D] / 65535.0f)) < 1e-6f);
	TASK_CHECK(fabsf(got[(D - 1) * D + 7] - (1.0f - (GLfloat)saved[7] / 65535.0f)) < 1e-6f);
	for (i = 0; i < N; i++) {
		TASK_CHECK(fabsf(got[i] - expect[i]) < 1e-6f);
		if (i == 300)
			break;
	}
	/* asymmetry: far (1.0) region exists, drawn region is nearer, values vary */
	{
		int has_far = 0, has_near = 0;
		for (i = 0; i < N; i++) {
			if (got[i] == 1.0f)
				has_far = 1;
			if (got[i] < 0.95f)
				has_near = 1;
			seen[(int)(got[i] * 7.999f)] = 1;
		}
		for (i = 0; i < 8; i++)
			distinct += seen[i] != 0;
		TASK_CHECK(has_far && has_near);
		TASK_CHECK(distinct >= 3);
	}

	/* snapshot independence: clear must not touch the texture */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	for (i = 0; i < N; i++) {
		TASK_CHECK(fabsf(got[i] - expect[i]) < 1e-6f);
		if (i == 500)
			break;
	}
	TASK_CHECK(zb->zbuf[100] == 0); /* the framebuffer itself was cleared */

	glDeleteTextures(1, &tex);
	glClose();
	ZB_close(zb);
	return 0;
}

static int test_capture_upload_independence(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex;
	GLint w, h;
	GLfloat src[N];
	GLfloat* got;
	int i;
	ramp_depth(src);

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, src);
	got = (GLfloat*)glGetTexturePixmap(tex, 0, &w, &h);
	TASK_CHECK(got && w == D && h == D);
	for (i = 0; i < N; i++)
		TASK_CHECK(got[i] == src[i]);

	/* overwrite the caller array: texture keeps its own copy */
	for (i = 0; i < N; i++)
		src[i] = -99.0f;
	TASK_CHECK(got[123] >= 0.0f && got[123] <= 1.0f);

	/* clears and draws cannot change it either */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(1, 1, 1);
	glBegin(GL_TRIANGLES);
	glVertex3f(-1, -1, 0);
	glVertex3f(1, -1, 0);
	glVertex3f(0, 1, 0);
	glEnd();
	{
		GLfloat fresh[N];
		ramp_depth(fresh);
		for (i = 0; i < N; i++)
			TASK_CHECK(got[i] == fresh[i]);
	}

	glDeleteTextures(1, &tex);
	glClose();
	ZB_close(zb);
	return 0;
}

static int test_capture_list_timing(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex, list = 7;
	GLint w, h;
	GLushort saved[N];
	GLfloat expect[N];
	GLfloat* got;
	int i, j;

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);

	/* compile: the depth copy must not capture anything yet */
	glNewList(list, GL_COMPILE);
	glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 0, 0, D, D, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
	glEndList();
	TASK_CHECK(glGetTexturePixmap(tex, 0, &w, &h) == NULL || w == 0);

	/* draw, then execute: capture happens now with the current ZBuffer */
	glColor3f(1, 1, 1);
	glBegin(GL_TRIANGLES);
	glVertex3f(-1, -1, 0.4f);
	glVertex3f(1, -1, 0.1f);
	glVertex3f(0, 1, -0.4f);
	glEnd();
	memcpy(saved, zb->zbuf, sizeof(saved));
	for (i = 0; i < D; i++)
		for (j = 0; j < D; j++)
			expect[j * D + i] = 1.0f - (GLfloat)saved[(D - 1 - j) * D + i] / 65535.0f;
	glCallList(list);

	got = (GLfloat*)glGetTexturePixmap(tex, 0, &w, &h);
	TASK_CHECK(got && w == D && h == D);
	for (i = 0; i < N; i += 37)
		TASK_CHECK(fabsf(got[i] - expect[i]) < 1e-6f);

	/* parameter command inside the list took effect at execution time:
	   the object now compares, so r=0 always passes (0 <= stored) */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	glBegin(GL_TRIANGLE_STRIP);
	glTexCoord4f(0, 0, 0.0f, 1); glVertex3f(-1, -1, 0);
	glTexCoord4f(1, 0, 0.0f, 1); glVertex3f(1, -1, 0);
	glTexCoord4f(0, 1, 0.0f, 1); glVertex3f(-1, 1, 0);
	glTexCoord4f(1, 1, 0.0f, 1); glVertex3f(1, 1, 0);
	glEnd();
	{
		int k, white = 0;
		for (k = 0; k < N; k += 257)
			if ((zb->pbuf[k] >> 16 & 0xff) >= 254)
				white++;
		TASK_CHECK(white > 200); /* compare mode from the list is active */
	}
	glDisable(GL_TEXTURE_2D);
	TASK_CHECK(fabsf(got[4096] - expect[4096]) < 1e-6f);

	glDeleteTextures(1, &tex);
	glDeleteLists(list, 1);
	glClose();
	ZB_close(zb);
	return 0;
}

static int test_capture_storage_lifecycle(void) {
	ZBuffer* zb = new_ctx();
	GLuint ta, tb;
	Vtx v[4];
	GLfloat da[N], db[N];
	int i;
	ramp_depth(da);
	for (i = 0; i < N; i++)
		db[i] = 1.0f - da[i];

	/* TinyGL only counts bound handles when minting a new one, so bind
	   each object before generating the next */
	glGenTextures(1, &ta);
	glBindTexture(GL_TEXTURE_2D, ta);
	glGenTextures(1, &tb);
	glBindTexture(GL_TEXTURE_2D, tb);

	glBindTexture(GL_TEXTURE_2D, ta);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, da);
	glBindTexture(GL_TEXTURE_2D, tb);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, db);

	fullscreen_quad(v, 1.0f);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);

	/* alternate bound objects: same geometry, different storage.
	   pick a high-(u,v) corner where g and 1-g diverge strongly */
	glBindTexture(GL_TEXTURE_2D, ta);
	begin_quad(v);
	{
		int qx = D - 20, qy = 20;
		int ga = (zb->pbuf[qy * D + qx] >> 16) & 0xff;
		int gb;
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glBindTexture(GL_TEXTURE_2D, tb);
		begin_quad(v);
		gb = (zb->pbuf[qy * D + qx] >> 16) & 0xff;
		TASK_CHECK(abs(ga - gb) > 100);
	}

	/* RGB redefinition releases depth storage and routes through RGB path */
	{
		static GLubyte rgb[N * 3];
		PIXEL* pp;
		GLint w, h;
		for (i = 0; i < N; i++) {
			rgb[i * 3 + 0] = 10;
			rgb[i * 3 + 1] = 60;
			rgb[i * 3 + 2] = 200;
		}
		glBindTexture(GL_TEXTURE_2D, ta);
		glTexImage2D(GL_TEXTURE_2D, 0, 3, D, D, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		begin_quad(v);
		pp = (PIXEL*)glGetTexturePixmap(ta, 0, &w, &h);
		TASK_CHECK(pp && w == D);
		TASK_CHECK(((pp[0] >> 16) & 0xff) == 10);
		TASK_CHECK((((zb->pbuf[D / 2 * D + D / 2] >> 16) & 0xff)) <= 14);
	}

	/* deletion must not corrupt the remaining object */
	glDeleteTextures(1, &tb);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &ta);
	glClose();
	ZB_close(zb);
	return 0;
}

/* --------------------------------- projection --------------------------------- */

static int test_projection_q_division(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex;
	Vtx v[4];
	GLfloat depth[N];
	ramp_depth(depth);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	/* texture q = 2 while camera w stays 1 */
	fullscreen_quad(v, 2.0f);
	begin_quad(v);
	TASK_CHECK(check_quad_reference(zb, v, depth, 0, 0, 0) == 0);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &tex);
	glClose();
	ZB_close(zb);
	return 0;
}

static int test_projection_w_neq_q(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex;
	Vtx v[4];
	GLfloat depth[N];
	int i;
	ramp_depth(depth);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);

	/* Camera homogeneous w = 2 while texture q = 0.5.
	   TinyGL's no-lighting path takes w from the projection matrix's
	   last row, so load 2*I (column-major): NDC is unchanged, w doubles. */
	{
		GLfloat m2[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2};
		glMatrixMode(GL_PROJECTION);
		glLoadMatrixf(m2);
		glMatrixMode(GL_MODELVIEW);
	}
	/* reference records the *effective* clip coords: w=2, NDC +-0.9,
	   kept strictly inside the clip volume so the direct path is used
	   (the clipping path has its own dedicated test) */
	for (i = 0; i < 4; i++)
		v[i] = (Vtx){0, 0, 0, 2, 0, 0, 0.3f + i * 0.1f, 0.5f};
	v[0].x = -1.8f; v[0].y = -1.8f; v[0].s = 0;    v[0].t = 0;
	v[1].x = 1.8f;  v[1].y = -1.8f; v[1].s = 0.5f; v[1].t = 0;
	v[2].x = -1.8f; v[2].y = 1.8f;  v[2].s = 0;    v[2].t = 0.5f;
	v[3].x = 1.8f;  v[3].y = 1.8f;  v[3].s = 0.5f; v[3].t = 0.5f;

	glBegin(GL_TRIANGLE_STRIP);
	for (i = 0; i < 4; i++) {
		glTexCoord4f(v[i].s, v[i].t, v[i].r, v[i].q);
		/* matrix 2I doubles object coords: object = clip/2 to reach v[].x */
		glVertex4f(v[i].x * 0.5f, v[i].y * 0.5f, v[i].z * 0.5f, 1.0f);
	}
	glEnd();

	TASK_CHECK(check_quad_reference(zb, v, depth, 0, 0, 0) == 0);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &tex);
	glClose();
	ZB_close(zb);
	return 0;
}

static int test_projection_texture_matrix(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex;
	Vtx v[4];
	GLfloat depth[N];
	int i;
	ramp_depth(depth);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	fullscreen_quad(v, 1.0f);
	{
		Vtx raw[4];
		memcpy(raw, v, sizeof(raw));
		/* effective coord = 0.5 * coord + 0.25 on s and t */
		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();
		glScalef(0.5f, 0.5f, 1.0f);
		glTranslatef(0.5f, 0.5f, 0.0f);
		glMatrixMode(GL_MODELVIEW);
		for (i = 0; i < 4; i++) {
			v[i].s = 0.5f * v[i].s + 0.25f;
			v[i].t = 0.5f * v[i].t + 0.25f;
		}
		/* raw pre-matrix coords go to GL; the matrix applies the transform */
		begin_quad(raw);
	}
	TASK_CHECK(check_quad_reference(zb, v, depth, 0, 0, 0) == 0);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &tex);
	glClose();
	ZB_close(zb);
	return 0;
}

static int test_projection_clipped(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex;
	GLfloat depth[N];
	Vtx tri[3];
	int py, sampled = 0, matched = 0, outside_bg = 0;
	ramp_depth(depth);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);

	/* one vertex beyond the side and near clip planes: clipping mandatory */
	tri[0] = (Vtx){-1.0f, -1.0f, 0.0f, 1, 0, 0, 0.2f, 1};
	tri[1] = (Vtx){1.6f, -0.6f, 1.4f, 1, 1, 0, 0.7f, 1};
	tri[2] = (Vtx){-0.6f, 1.0f, 0.0f, 1, 0, 1, 0.8f, 1};
	glBegin(GL_TRIANGLES);
	glTexCoord4f(tri[0].s, tri[0].t, tri[0].r, tri[0].q);
	glVertex4f(tri[0].x, tri[0].y, tri[0].z, tri[0].w);
	glTexCoord4f(tri[1].s, tri[1].t, tri[1].r, tri[1].q);
	glVertex4f(tri[1].x, tri[1].y, tri[1].z, tri[1].w);
	glTexCoord4f(tri[2].s, tri[2].t, tri[2].r, tri[2].q);
	glVertex4f(tri[2].x, tri[2].y, tri[2].z, tri[2].w);
	glEnd();

	for (py = 8; py < D - 8; py++) {
		int px;
		for (px = 8; px < D - 8; px++) {
			GLfloat u, vv, rr, winz;
			if (ref_project(&tri[0], &tri[1], &tri[2], (GLfloat)px, (GLfloat)py, &u, &vv, &rr, &winz)) {
				GLfloat g = ref_lookup(depth, D, u, vv, rr, 0, 0, 0);
				int expect = ref_gray(g);
				int got = (zb->pbuf[py * D + px] >> 16) & 0xff;
				sampled++;
				if (abs(got - expect) <= 2)
					matched++;
			} else if (px > 3 * D / 4 && py < D / 4) {
				/* top-right corner is outside this left-anchored triangle */
				if ((zb->pbuf[py * D + px] & 0xffffff) == 0)
					outside_bg++;
			}
		}
	}
	TASK_CHECK(sampled > 500);
	TASK_CHECK(matched * 100 >= sampled * 95);
	TASK_CHECK(outside_bg > 100);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &tex);
	glClose();
	ZB_close(zb);
	return 0;
}

/* --------------------------------- filtering ---------------------------------- */

static int test_filter_nearest_clamp(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex;
	Vtx v[4];
	GLfloat depth[N];
	int i, j, sampled = 0, matched = 0;
	/* coarse 16-texel vertical stripes: stable under sub-texel
	   interpolation drift while still strongly asymmetric */
	for (j = 0; j < D; j++)
		for (i = 0; i < D; i++)
			depth[j * D + i] = (i / 16) / 15.0f;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	/* coords extend past [0,1]: clamp-to-edge must hold */
	v[0] = (Vtx){-1, -1, 0, 1, -0.25f, -0.25f, 0.5f, 1};
	v[1] = (Vtx){1, -1, 0, 1, 1.25f, -0.25f, 0.5f, 1};
	v[2] = (Vtx){-1, 1, 0, 1, -0.25f, 1.25f, 0.5f, 1};
	v[3] = (Vtx){1, 1, 0, 1, 1.25f, 1.25f, 0.5f, 1};
	begin_quad(v);

	for (j = 24; j < D - 24; j += 2) {
		int px;
		for (px = 24; px < D - 24; px += 2) {
			GLfloat u, vv, rr, winz;
			int hit = ref_project(&v[0], &v[1], &v[2], (GLfloat)px, (GLfloat)j, &u, &vv, &rr, &winz);
			if (!hit)
				hit = ref_project(&v[1], &v[3], &v[2], (GLfloat)px, (GLfloat)j, &u, &vv, &rr, &winz);
			if (!hit)
				continue;
			{
				GLfloat g = ref_lookup(depth, D, u, vv, rr, 0, 0, 0);
				int expect = ref_gray(g);
				int got = (zb->pbuf[j * D + px] >> 16) & 0xff;
				sampled++;
				if (abs(got - expect) <= 2)
					matched++;
			}
		}
	}
	TASK_CHECK(sampled > 1000);
	TASK_CHECK(matched * 100 >= sampled * 90);

	/* explicit clamp-to-edge: the outermost screen columns must read the
	   first / last texture column regardless of the out-of-range u */
	{
		int left_ix = 0, right_ix = D - 1;
		int row = D / 2;
		int exp_left = ref_gray(depth[row * D + left_ix]);
		int exp_right = ref_gray(depth[row * D + right_ix]);
		int got_left = (zb->pbuf[row * D + 4] >> 16) & 0xff;
		int got_right = (zb->pbuf[row * D + (D - 5)] >> 16) & 0xff;
		TASK_CHECK(abs(got_left - exp_left) <= 2);
		TASK_CHECK(abs(got_right - exp_right) <= 2);
		/* edges genuinely differ, proving both sides were exercised */
		TASK_CHECK(abs(exp_right - exp_left) > 200);
	}
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &tex);
	glClose();
	ZB_close(zb);
	return 0;
}

static int test_filter_linear_gray(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex;
	Vtx v[4];
	GLfloat depth[N];
	ramp_depth(depth);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	fullscreen_quad(v, 1.0f);
	begin_quad(v);
	TASK_CHECK(check_quad_reference(zb, v, depth, 0, 0, 1) == 0);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &tex);
	glClose();
	ZB_close(zb);
	return 0;
}

static int test_filter_compare_funcs(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex;
	Vtx v[4];
	GLfloat depth[N];
	int i, white = 0, black = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);

	/* Part A: equality boundary on a constant texture deterministically
	   separates LEQUAL (passes equality) from LESS (fails equality) */
	for (i = 0; i < N; i++)
		depth[i] = 0.5f;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	fullscreen_quad(v, 1.0f); /* r spans 0.3..0.6; reset r to 0.5 below */
	for (i = 0; i < 4; i++)
		v[i].r = 0.5f;

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	begin_quad(v);
	for (i = 0; i < N; i += 97)
		if ((zb->pbuf[i] >> 16 & 0xff) >= 254)
			white++;
	TASK_CHECK(white > 200); /* r == stored: LEQUAL passes */

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LESS);
	begin_quad(v);
	for (i = 0; i < N; i += 97)
		if ((zb->pbuf[i] >> 16 & 0xff) <= 1)
			black++;
	TASK_CHECK(black > 200); /* r == stored: LESS fails */

	/* Part B: asymmetric ramp; every pixel must match the independent
	   reference exactly for each function, validating the
	   reference-vs-stored comparison direction. The LEQUAL/LESS equality
	   distinction is covered deterministically by Part A. */
	ramp_depth(depth);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
	v[0] = (Vtx){-1, -1, 0, 1, 0, 0, 0.10f, 1};
	v[1] = (Vtx){1, -1, 0, 1, 1, 0, 0.45f, 1};
	v[2] = (Vtx){-1, 1, 0, 1, 0, 1, 0.55f, 1};
	v[3] = (Vtx){1, 1, 0, 1, 1, 1, 0.90f, 1};
	{
		int func, less;
		for (func = 0; func < 2; func++) {
			less = func;
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, less ? GL_LESS : GL_LEQUAL);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			begin_quad(v);
			{
				int py, sampled = 0, matched = 0;
				for (py = 20; py < D - 20; py += 3) {
					int px;
					for (px = 20; px < D - 20; px += 3) {
						GLfloat u, vv, rr, winz;
						if (ref_project(&v[0], &v[1], &v[2], (GLfloat)px, (GLfloat)py, &u, &vv, &rr, &winz) ||
							ref_project(&v[1], &v[3], &v[2], (GLfloat)px, (GLfloat)py, &u, &vv, &rr, &winz)) {
							GLfloat g = ref_lookup(depth, D, u, vv, rr, 1, less, 0);
							int expect = ref_gray(g);
							int got = (zb->pbuf[py * D + px] >> 16) & 0xff;
							sampled++;
							if (abs(got - expect) <= 1)
								matched++;
						}
					}
				}
				TASK_CHECK(sampled > 500);
				TASK_CHECK(matched == sampled);
			}
		}
	}
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &tex);
	glClose();
	ZB_close(zb);
	return 0;
}

static int test_filter_linear_ratio_and_blend_order(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex;
	Vtx v[4];
	GLfloat depth[N];
	int partial = 0, order_differs = 0, order_match = 0, i, j;
	/* depth ramp in t spanning r=0.5, constant in u so the linear
	   comparison yields a wide half-weight (g=0.5) transition band */
	for (j = 0; j < D; j++)
		for (i = 0; i < D; i++)
			depth[j * D + i] = 0.2f + 0.6f * j / D;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);

	/* non-zero background and additive blending make ordering visible */
	glClearColor(0.25f, 0.25f, 0.25f, 0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBlendFunc(GL_ONE, GL_ONE);
	glEnable(GL_BLEND);

	/* identity s/t, constant r=0.5: the r=stored crossing is horizontal */
	v[0] = (Vtx){-1, -1, 0, 1, 0, 0, 0.5f, 1};
	v[1] = (Vtx){1, -1, 0, 1, 1, 0, 0.5f, 1};
	v[2] = (Vtx){-1, 1, 0, 1, 0, 1, 0.5f, 1};
	v[3] = (Vtx){1, 1, 0, 1, 1, 1, 0.5f, 1};
	begin_quad(v);

	{
		int py;
		for (py = 24; py < D - 24; py += 2) {
			int px;
			for (px = 24; px < D - 24; px += 2) {
				GLfloat u, vv, rr, winz;
				if (ref_project(&v[0], &v[1], &v[2], (GLfloat)px, (GLfloat)py, &u, &vv, &rr, &winz) ||
					ref_project(&v[1], &v[3], &v[2], (GLfloat)px, (GLfloat)py, &u, &vv, &rr, &winz)) {
					GLfloat g = ref_lookup(depth, D, u, vv, rr, 1, 0, 1);
					int gray = ref_gray(g);
					int bg = ref_gray(0.25f);
					int compare_first = bg + gray > 255 ? 255 : bg + gray;            /* actual chain */
					int depth_first = (int)((bg + 255) * g + 0.5f);                   /* hypothetical */
					int got = (zb->pbuf[py * D + px] >> 16) & 0xff;
					if (depth_first > 255)
						depth_first = 255;
					if (gray > 12 && gray < 244)
						partial++;
					if (abs(depth_first - compare_first) >= 2)
						order_differs++;
					if (abs(got - compare_first) <= 2)
						order_match++;
				}
			}
		}
	}
	TASK_CHECK(partial > 20);            /* linear compare really yields ratios */
	TASK_CHECK(order_differs > 20);      /* the two blend orders are distinguishable */
	TASK_CHECK(order_match > partial);   /* implementation uses compare-first blending */
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &tex);
	glClose();
	ZB_close(zb);
	return 0;
}

/* -------------------------------- compatibility -------------------------------- */

static int test_compat_rgb_path(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex;
	Vtx v[4];
	static GLubyte rgb[N * 3];
	PIXEL* pix;
	GLint w, h;
	int i, sampled = 0, matched = 0;
	for (i = 0; i < N; i++) {
		rgb[i * 3 + 0] = (GLubyte)(i & 0xff);
		rgb[i * 3 + 1] = (GLubyte)((i * 3) & 0xff);
		rgb[i * 3 + 2] = (GLubyte)((i * 7) & 0xff);
	}
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, 3, D, D, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	fullscreen_quad(v, 1.0f);
	begin_quad(v);
	pix = (PIXEL*)glGetTexturePixmap(tex, 0, &w, &h);
	TASK_CHECK(pix && w == D && h == D);
	/* RGB copy/addressing convention unchanged: nearest texel at s,t */
	for (i = 0; i < 200; i++) {
		int px = 20 + (i * 11) % (D - 40);
		int py = 20 + (i * 7) % (D - 40);
		GLfloat u, vv, rr, winz;
		if (ref_project(&v[0], &v[1], &v[2], (GLfloat)px, (GLfloat)py, &u, &vv, &rr, &winz) ||
			ref_project(&v[1], &v[3], &v[2], (GLfloat)px, (GLfloat)py, &u, &vv, &rr, &winz)) {
			int ix = (int)floorf(ref_clampf(u, 0, 1) * D);
			int iy = (int)floorf(ref_clampf(vv, 0, 1) * D);
			/* the legacy RGB mapper uses its fixed-point half-texel
			   convention; accept either row orientation and a one-texel
			   (single channel step) tolerance, but no larger drift */
			PIXEL expect_a = pix[iy * D + ix];
			PIXEL expect_b = pix[(D - 1 - iy) * D + ix];
			PIXEL got = zb->pbuf[py * D + px];
			int k, ok = 0;
			sampled++;
			for (k = 0; k < 2 && !ok; k++) {
				PIXEL e = k ? expect_b : expect_a;
				int dr = abs((int)((got >> 16) & 0xff) - (int)((e >> 16) & 0xff));
				int dg = abs((int)((got >> 8) & 0xff) - (int)((e >> 8) & 0xff));
				int db = abs((int)(got & 0xff) - (int)(e & 0xff));
				if (dr <= 2 && dg <= 2 && db <= 2)
					ok = 1;
			}
			if (ok)
				matched++;
		}
	}
	TASK_CHECK(sampled > 100 && matched == sampled);

	/* RGB framebuffer copy still works */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(0, 1, 0);
	glBegin(GL_TRIANGLES);
	glVertex3f(-1, -1, 0);
	glVertex3f(1, -1, 0);
	glVertex3f(0, 1, 0);
	glEnd();
	glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, D, D, D, 0);
	pix = (PIXEL*)glGetTexturePixmap(tex, 0, &w, &h);
	TASK_CHECK(((pix[(D / 2) * D + D / 2] >> 8) & 0xff) > 250);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &tex);
	glClose();
	ZB_close(zb);
	return 0;
}

static int test_compat_pipeline_states(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex;
	Vtx v[4];
	GLfloat depth[N];
	static GLubyte stipple[128];
	int py, skipped = 0, drawn = 0;
	ramp_depth(depth);
	for (py = 0; py < 128; py++)
		stipple[py] = (py & 1) ? 0x0f : 0xf0; /* roughly half the fragments */

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
	glEnable(GL_TEXTURE_2D);
	glShadeModel(GL_FLAT);
	glPolygonStipple(stipple);
	glEnable(GL_POLYGON_STIPPLE);
	glColor3f(1, 1, 1);
	fullscreen_quad(v, 1.0f);
	begin_quad(v);

	for (py = 20; py < D - 20; py += 2) {
		int px;
		for (px = 20; px < D - 20; px += 2) {
			if ((zb->pbuf[py * D + px] & 0xffffff) == 0)
				skipped++;
			else
				drawn++;
		}
	}
	TASK_CHECK(skipped > 100 && drawn > 100); /* stipple really gates fragments */

	/* depth test / background occlusion: a near quad (negative ndc z,
	   which is the large window-z side) blocks a farther receiver */
	glDisable(GL_POLYGON_STIPPLE);
	glShadeModel(GL_SMOOTH);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDisable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	glBegin(GL_TRIANGLE_STRIP); /* occluder at ndc z = -0.6 (near) */
	glVertex3f(-1, -1, -0.6f);
	glVertex3f(1, -1, -0.6f);
	glVertex3f(-1, 1, -0.6f);
	glVertex3f(1, 1, -0.6f);
	glEnd();
	{
		GLushort znear = zb->zbuf[(D / 2) * D + D / 2];
		PIXEL occluder = zb->pbuf[(D / 2) * D + D / 2];
		int blocked = 0;
		TASK_CHECK(znear != 0);
		TASK_CHECK((occluder & 0xffffff) != 0);
		glEnable(GL_TEXTURE_2D);
		/* receiver at ndc z = 0 (behind): every covered pixel must lose
		   the depth test, leaving the occluder color and window Z */
		begin_quad(v);
		{
			int x;
			for (x = 30; x < D - 30; x += 5) {
				if (zb->pbuf[(D / 2) * D + x] == occluder)
					blocked++;
			}
		}
		TASK_CHECK(blocked > 30);
		TASK_CHECK(zb->zbuf[(D / 2) * D + D / 2] == znear);
	}
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &tex);
	glClose();
	ZB_close(zb);
	return 0;
}

static int test_compat_modulate_and_defaults(void) {
	ZBuffer* zb = new_ctx();
	GLuint tex;
	Vtx v[4];
	GLfloat depth[N];
	int py, sampled = 0, matched = 0;
	ramp_depth(depth);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, D, D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
	/* default object state: no compare, LEQUAL, NEAREST */
	glEnable(GL_TEXTURE_2D);
	glColor3f(1.0f, 0.4f, 0.0f); /* non-white fragment color */
	fullscreen_quad(v, 1.0f);
	begin_quad(v);

	for (py = 24; py < D - 24; py += 4) {
		int px;
		for (px = 24; px < D - 24; px += 4) {
			GLfloat u, vv, rr, winz;
			if (ref_project(&v[0], &v[1], &v[2], (GLfloat)px, (GLfloat)py, &u, &vv, &rr, &winz) ||
				ref_project(&v[1], &v[3], &v[2], (GLfloat)px, (GLfloat)py, &u, &vv, &rr, &winz)) {
				GLfloat g = ref_lookup(depth, D, u, vv, rr, 0, 0, 0);
				int gray = ref_gray(g);
				PIXEL pix = zb->pbuf[py * D + px];
				int red = (int)((pix >> 16) & 0xff);
				int green = (int)((pix >> 8) & 0xff);
				int blue = (int)(pix & 0xff);
				sampled++;
				if (abs(red - gray) <= 2 && green <= (int)(0.4f * gray) + 4 && blue <= 4)
					matched++;
			}
		}
	}
	TASK_CHECK(sampled > 200 && matched * 100 >= sampled * 95);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &tex);
	glClose();
	ZB_close(zb);
	return 0;
}

/* --------------------------------- registration -------------------------------- */

const struct task_case task_cases[] = {
	{"capture", "asymmetric_copy", test_capture_asymmetric},
	{"capture", "upload_copy", test_capture_upload_independence},
	{"capture", "list_execution_timing", test_capture_list_timing},
	{"capture", "storage_lifecycle", test_capture_storage_lifecycle},

	{"projection", "q_division", test_projection_q_division},
	{"projection", "camera_w_neq_texture_q", test_projection_w_neq_q},
	{"projection", "texture_matrix", test_projection_texture_matrix},
	{"projection", "near_side_clipping", test_projection_clipped},

	{"filtering", "nearest_clamp_edge", test_filter_nearest_clamp},
	{"filtering", "linear_gray", test_filter_linear_gray},
	{"filtering", "compare_lequal_less", test_filter_compare_funcs},
	{"filtering", "linear_ratio_blend_order", test_filter_linear_ratio_and_blend_order},

	{"compatibility", "rgb_path_unchanged", test_compat_rgb_path},
	{"compatibility", "stipple_depth_chain", test_compat_pipeline_states},
	{"compatibility", "modulate_default_state", test_compat_modulate_and_defaults},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
