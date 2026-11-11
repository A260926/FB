/*
 * depth_state_test.c
 *
 * Numeric regression for the TinyGL depth test / depth write mask state:
 *   - GL_DEPTH_TEST only controls the depth comparison, never depth storage
 *   - glDepthMask only controls depth storage, never the comparison
 *   - glClear(GL_DEPTH_BUFFER_BIT) honors the write mask, not GL_DEPTH_TEST
 *   - glDepthMask is a display list state command (compile vs. compile+exec)
 *
 * The checks assert on rendered colors and on depth buffer contents, never on
 * internal flags alone. Geometry is fully synthetic and the only texture is a
 * malloc-free in-memory RGB bitmap; no SDL and no external assets are used.
 */

#include "GL/gl.h"
#include "zbuffer.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 32
#define H 32
#define NPIX (W * H)

static ZBuffer* zb;
static int failures;
static int checks;

static GLuint pix_rgb(unsigned r, unsigned g, unsigned b) {
	return ((r & 255u) << 16) | ((g & 255u) << 8) | (b & 255u);
}
static unsigned pr(GLuint p) { return (p >> 16) & 255u; }
static unsigned pg(GLuint p) { return (p >> 8) & 255u; }
static unsigned pb(GLuint p) { return p & 255u; }

static void check(int cond, const char* fmt, ...) {
	va_list ap;
	checks++;
	if (!cond) {
		va_start(ap, fmt);
		printf("FAIL: ");
		vprintf(fmt, ap);
		printf("\n");
		va_end(ap);
		failures++;
	}
}

/* integer depth value the rasterizer stores for an NDC z coordinate,
   replicating gl_eval_viewport() and the >> ZB_POINT_Z_FRAC_BITS shift */
static GLushort expect_z(GLfloat ndc_z) {
	GLfloat zsize = (GLfloat)(1 << (ZB_Z_BITS + ZB_POINT_Z_FRAC_BITS));
	GLfloat scale_z = -(zsize - 0.5f) / 2.0f;
	GLfloat trans_z = (zsize - 0.5f) / 2.0f + (GLfloat)(1 << ZB_POINT_Z_FRAC_BITS) / 2.0f;
	GLint fixed = (GLint)(ndc_z * scale_z + trans_z);
	return (GLushort)(fixed >> ZB_POINT_Z_FRAC_BITS);
}
static int z_close(GLushort got, GLfloat ndc_z) {
	int e = (int)expect_z(ndc_z);
	int d = (int)got - e;
	if (d < 0)
		d = -d;
	return d <= 2;
}

static int count_nonz(void) {
	int n = 0, i;
	for (i = 0; i < NPIX; i++)
		n += zb->zbuf[i] != 0;
	return n;
}
static void snap_z(GLushort* dst) { memcpy(dst, zb->zbuf, NPIX * sizeof(GLushort)); }
static int zbuf_unchanged(const GLushort* ref) { return memcmp(ref, zb->zbuf, NPIX * sizeof(GLushort)) == 0; }
static void snap_color(GLuint* dst) { memcpy(dst, zb->pbuf, NPIX * sizeof(GLuint)); }

static int find_pixel(unsigned r, unsigned g, unsigned b, int tol, int* ox, int* oy) {
	int x, y;
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			GLuint p = zb->pbuf[y * W + x];
			if (abs((int)pr(p) - (int)r) <= tol && abs((int)pg(p) - (int)g) <= tol && abs((int)pb(p) - (int)b) <= tol) {
				*ox = x;
				*oy = y;
				return 1;
			}
		}
	return 0;
}

static void tri(GLfloat z, GLfloat r, GLfloat g, GLfloat b) {
	glColor3f(r, g, b);
	glBegin(GL_TRIANGLES);
	glVertex3f(-0.9f, -0.9f, z);
	glVertex3f(0.9f, -0.9f, z);
	glVertex3f(0.0f, 0.9f, z);
	glEnd();
}

/* smooth shaded triangle: per-vertex colors, same footprint as tri() */
static void tri_smooth(GLfloat z) {
	glBegin(GL_TRIANGLES);
	glColor3f(1, 0, 0);
	glVertex3f(-0.9f, -0.9f, z);
	glColor3f(0, 1, 0);
	glVertex3f(0.9f, -0.9f, z);
	glColor3f(0, 0, 1);
	glVertex3f(0.0f, 0.9f, z);
	glEnd();
}

static void point_at(GLfloat x, GLfloat y, GLfloat z, GLfloat r, GLfloat g, GLfloat b) {
	glColor3f(r, g, b);
	glBegin(GL_POINTS);
	glVertex3f(x, y, z);
	glEnd();
}

static void line_at(GLfloat y, GLfloat z, GLfloat r, GLfloat g, GLfloat b) {
	glColor3f(r, g, b);
	glBegin(GL_LINES);
	glVertex3f(-0.8f, y, z);
	glVertex3f(0.8f, y, z);
	glEnd();
}

static void reset_state(void) {
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);
	glShadeModel(GL_FLAT);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static GLint get_int(GLenum name) {
	GLint v = -1;
	glGetIntegerv(name, &v);
	return v;
}

/* ------------------------------------------------------------------ */
/* four test/mask combinations on flat triangles                       */
/* ------------------------------------------------------------------ */
static void test_combo_readwrite(void) {
	GLushort ref[NPIX];
	reset_state();
	/* test on, mask on: far blue then near red -> red wins and is stored */
	tri(0.5f, 0, 0, 1);
	tri(-0.5f, 1, 0, 0);
	check(pr(zb->pbuf[16 * W + 16]) > 200 && pg(zb->pbuf[16 * W + 16]) < 40 && pb(zb->pbuf[16 * W + 16]) < 40,
	      "readwrite: center should be the near red triangle");
	check(z_close(zb->zbuf[16 * W + 16], -0.5f), "readwrite: near depth stored, got %u", zb->zbuf[16 * W + 16]);
	check(count_nonz() > 100, "readwrite: triangle should cover many depth pixels");

	/* opposite draw order: the far triangle must lose the comparison */
	reset_state();
	tri(-0.5f, 1, 0, 0);
	snap_z(ref);
	tri(0.5f, 0, 0, 1);
	check(pr(zb->pbuf[16 * W + 16]) > 200, "readwrite: far blue must not overwrite near red");
	check(zbuf_unchanged(ref), "readwrite: failed far draw must not touch depth");
}

static void test_combo_readonly(void) {
	GLushort ref[NPIX];
	int x, y;
	reset_state();
	/* seed a near depth with the mask open */
	tri(-0.5f, 1, 0, 0);
	check(z_close(zb->zbuf[16 * W + 16], -0.5f), "readonly: seed depth stored");
	snap_z(ref);

	/* mask closed: far draw loses the comparison, color and depth frozen */
	glDepthMask(GL_FALSE);
	tri(0.5f, 0, 0, 1);
	check(pr(zb->pbuf[16 * W + 16]) > 200 && pb(zb->pbuf[16 * W + 16]) < 40,
	      "readonly: occluded far draw must not change color");
	check(zbuf_unchanged(ref), "readonly: occluded far draw must not change depth");

	/* a nearer draw passes the comparison and paints, but depth stays put */
	tri(-0.6f, 0, 1, 0);
	check(pg(zb->pbuf[16 * W + 16]) > 200, "readonly: passing draw still paints color");
	check(zbuf_unchanged(ref), "readonly: passing draw must still leave depth frozen");
	check(z_close(zb->zbuf[16 * W + 16], -0.5f), "readonly: frozen depth is the seeded value");

	/* every covered pixel kept its depth */
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			check(zb->zbuf[y * W + x] == ref[y * W + x], "readonly: pixel %d,%d depth changed", x, y);
	glDepthMask(GL_TRUE);
}

static void test_combo_notest_write(void) {
	GLushort ref[NPIX];
	reset_state();
	/* seed a near depth with the test on */
	tri(-0.5f, 1, 0, 0);
	check(z_close(zb->zbuf[16 * W + 16], -0.5f), "notest-write: seeded near depth");
	snap_z(ref);

	/* test off while the mask is still open: colors paint, but no primitive
	   may update depth, so the seed value survives */
	glDisable(GL_DEPTH_TEST);
	tri(0.5f, 0, 0, 1);
	check(pb(zb->pbuf[16 * W + 16]) > 200, "notest-write: last color wins without comparison");
	check(zbuf_unchanged(ref), "notest-write: test-off draw must not write even with the mask open");

	/* re-enabling the test picks up exactly the preserved depth again */
	glEnable(GL_DEPTH_TEST);
	tri(0.5f, 0, 1, 0);
	check(pb(zb->pbuf[16 * W + 16]) > 200 && pg(zb->pbuf[16 * W + 16]) < 40,
	      "notest-write: re-enabled test still occludes on the preserved depth");
	check(zbuf_unchanged(ref), "notest-write: re-enabled test keeps the preserved depth");
}

static void test_combo_notest_nowrite(void) {
	reset_state();
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	tri(-0.5f, 1, 0, 0);
	check(pr(zb->pbuf[16 * W + 16]) > 200, "notest-nowrite: color is painted with everything off");
	check(count_nonz() == 0, "notest-nowrite: no depth may be written");
	tri(0.5f, 0, 0, 1);
	check(pb(zb->pbuf[16 * W + 16]) > 200, "notest-nowrite: second color still paints");
	check(count_nonz() == 0, "notest-nowrite: depth buffer stays untouched");
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);
}

/* smooth shaded triangle must follow the same state rules */
static void test_smooth(void) {
	GLushort ref[NPIX];
	reset_state();
	glShadeModel(GL_SMOOTH);
	tri_smooth(-0.5f);
	check(count_nonz() > 100, "smooth: near smooth triangle stores depth");
	check(z_close(zb->zbuf[16 * W + 16], -0.5f), "smooth: center depth matches the vertex depth");
	snap_z(ref);
	glDepthMask(GL_FALSE);
	tri_smooth(0.5f);
	check(zbuf_unchanged(ref), "smooth: masked far draw leaves depth frozen");
	glDisable(GL_DEPTH_TEST);
	tri_smooth(0.5f);
	check(zbuf_unchanged(ref), "smooth: test-off masked draw leaves depth frozen");
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
}

/* ------------------------------------------------------------------ */
/* glClear depth masking                                               */
/* ------------------------------------------------------------------ */
static void test_clear(void) {
	GLushort ref[NPIX];
	reset_state();
	tri(-0.5f, 1, 0, 0);
	check(count_nonz() > 100, "clear: seed geometry writes depth");
	snap_z(ref);

	/* masked depth clear: depth survives, color survives */
	glDepthMask(GL_FALSE);
	glClear(GL_DEPTH_BUFFER_BIT);
	check(zbuf_unchanged(ref), "clear: masked depth clear must preserve depth");
	check(pr(zb->pbuf[16 * W + 16]) > 200, "clear: depth-only clear must not touch color");

	/* combined clear with the mask still closed: color goes, depth stays */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	check(zbuf_unchanged(ref), "clear: masked combined clear must still preserve depth");
	check(zb->pbuf[16 * W + 16] == 0, "clear: color channel must still clear");

	/* GL_DEPTH_TEST has no say in clears */
	glDisable(GL_DEPTH_TEST);
	glClear(GL_DEPTH_BUFFER_BIT);
	check(zbuf_unchanged(ref), "clear: test-off masked clear must still preserve depth");

	/* mask open: depth now clears even with the test disabled */
	glDepthMask(GL_TRUE);
	glClear(GL_DEPTH_BUFFER_BIT);
	check(count_nonz() == 0, "clear: unmasked depth clear works with the test off");
	check(zb->pbuf[16 * W + 16] == 0, "clear: depth-only clear leaves the cleared color alone");
	glEnable(GL_DEPTH_TEST);
}

/* ------------------------------------------------------------------ */
/* points and lines                                                    */
/* ------------------------------------------------------------------ */
static void test_points(void) {
	GLushort ref[NPIX];
	int x, y;
	reset_state();
	point_at(-0.5f, -0.5f, 0.5f, 0, 0, 1);
	check(find_pixel(0, 0, 255, 20, &x, &y), "points: far point drawn");
	check(z_close(zb->zbuf[y * W + x], 0.5f), "points: far point stored its depth");
	point_at(-0.5f, -0.5f, -0.5f, 1, 0, 0);
	check(find_pixel(255, 0, 0, 20, &x, &y), "points: near point overwrites the far one");
	check(z_close(zb->zbuf[y * W + x], -0.5f), "points: near point stored its depth");
	snap_z(ref);

	glDepthMask(GL_FALSE);
	point_at(-0.5f, -0.5f, 0.5f, 0, 1, 0);
	check(find_pixel(255, 0, 0, 20, &x, &y), "points: masked far point is occluded");
	check(!find_pixel(0, 255, 0, 200, &x, &y), "points: masked far point painted nothing");
	check(zbuf_unchanged(ref), "points: masked point leaves depth frozen");

	glDisable(GL_DEPTH_TEST);
	point_at(0.5f, 0.5f, 0.5f, 0, 1, 0);
	check(find_pixel(0, 255, 0, 20, &x, &y), "points: test-off point paints color");
	check(zb->zbuf[y * W + x] == 0, "points: test-off masked point writes no depth");
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
}

static void test_lines(void) {
	GLushort ref[NPIX];
	int x, y;
	reset_state();
	line_at(0.0f, 0.5f, 0, 0, 1);
	check(find_pixel(0, 0, 255, 20, &x, &y), "lines: far line drawn");
	line_at(0.0f, -0.5f, 1, 0, 0);
	check(find_pixel(255, 0, 0, 20, &x, &y), "lines: near line overwrites the far one");
	check(z_close(zb->zbuf[y * W + x], -0.5f), "lines: near line stored its depth");
	snap_z(ref);

	glDepthMask(GL_FALSE);
	line_at(0.0f, 0.5f, 0, 1, 0);
	check(find_pixel(255, 0, 0, 20, &x, &y), "lines: masked far line is occluded");
	check(zbuf_unchanged(ref), "lines: masked line leaves depth frozen");

	glDisable(GL_DEPTH_TEST);
	line_at(0.4f, 0.5f, 0, 1, 0);
	check(find_pixel(0, 255, 0, 20, &x, &y), "lines: test-off line paints color");
	check(zb->zbuf[y * W + x] == 0, "lines: test-off masked line writes no depth");
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
}

/* ------------------------------------------------------------------ */
/* glDrawPixels path                                                   */
/* ------------------------------------------------------------------ */
static void test_drawpixels(void) {
	GLushort ref[NPIX];
	PIXEL block[16];
	int x, y, i;
	for (i = 0; i < 16; i++)
		block[i] = pix_rgb(0, 0, 255);

	reset_state();
	glRasterPos3f(-0.25f, -0.25f, 0.5f);
	glDrawPixels(4, 4, GL_RGB, GL_UNSIGNED_INT, block);
	check(find_pixel(0, 0, 255, 20, &x, &y), "drawpixels: block painted");
	check(z_close(zb->zbuf[y * W + x], 0.5f), "drawpixels: raster depth stored");
	snap_z(ref);

	/* nearer geometry occludes the block and overwrites the depth */
	tri(-0.5f, 1, 0, 0);
	check(pr(zb->pbuf[y * W + x]) > 200, "drawpixels: near triangle occludes the block");
	check(z_close(zb->zbuf[y * W + x], -0.5f), "drawpixels: near triangle stores its depth");
	snap_z(ref);

	/* masked far block: comparison still runs, nothing passes, depth frozen */
	glDepthMask(GL_FALSE);
	for (i = 0; i < 16; i++)
		block[i] = pix_rgb(0, 255, 0);
	glRasterPos3f(-0.25f, -0.25f, 0.5f);
	glDrawPixels(4, 4, GL_RGB, GL_UNSIGNED_INT, block);
	check(!find_pixel(0, 255, 0, 200, &x, &y), "drawpixels: masked far block is fully occluded");
	check(zbuf_unchanged(ref), "drawpixels: masked block leaves depth frozen");

	/* test off + mask off: color paints, depth stays */
	glDisable(GL_DEPTH_TEST);
	glRasterPos3f(0.25f, 0.25f, 0.5f);
	glDrawPixels(4, 4, GL_RGB, GL_UNSIGNED_INT, block);
	check(find_pixel(0, 255, 0, 20, &x, &y), "drawpixels: test-off block paints");
	check(zb->zbuf[y * W + x] == 0, "drawpixels: test-off masked block writes no depth");
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
}

/* ------------------------------------------------------------------ */
/* textured triangles, with and without blending                       */
/* ------------------------------------------------------------------ */
static GLubyte tex_mem[256 * 256 * 3];
static void make_texture(void) {
	int x, y, i = 0;
	for (y = 0; y < 256; y++)
		for (x = 0; x < 256; x++) {
			if (x < 128) {
				tex_mem[i++] = 200;
				tex_mem[i++] = 40;
				tex_mem[i++] = 40;
			} else {
				tex_mem[i++] = 40;
				tex_mem[i++] = 40;
				tex_mem[i++] = 200;
			}
		}
}

static void tex_tri(GLfloat z) {
	glColor3f(1, 1, 1);
	glBegin(GL_TRIANGLES);
	glTexCoord2f(0, 0);
	glVertex3f(-0.9f, -0.9f, z);
	glTexCoord2f(1, 0);
	glVertex3f(0.9f, -0.9f, z);
	glTexCoord2f(0.5f, 1);
	glVertex3f(0.0f, 0.9f, z);
	glEnd();
}

static void test_textured(void) {
	GLushort ref[NPIX];
	int x, y;
	reset_state();
	make_texture();
	glBindTexture(GL_TEXTURE_2D, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, tex_mem);
	glEnable(GL_TEXTURE_2D);

	tex_tri(-0.5f);
	check(find_pixel(200, 40, 40, 25, &x, &y), "textured: left texel half visible");
	check(find_pixel(40, 40, 200, 25, &x, &y), "textured: right texel half visible");
	check(z_close(zb->zbuf[16 * W + 16], -0.5f), "textured: near depth stored");
	snap_z(ref);

	/* far textured draw is occluded; masked depth frozen */
	glDepthMask(GL_FALSE);
	tex_tri(0.5f);
	check(zbuf_unchanged(ref), "textured: masked far draw leaves depth frozen");
	glDisable(GL_DEPTH_TEST);
	tex_tri(0.5f);
	check(zbuf_unchanged(ref), "textured: test-off masked draw leaves depth frozen");
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);

	/* blended textured draw: result follows src + dst*(1-src), depth rules unchanged */
	reset_state();
	glBindTexture(GL_TEXTURE_2D, 1);
	glEnable(GL_TEXTURE_2D);
	tex_tri(-0.5f);
	snap_z(ref);
	{
		GLuint before = zb->pbuf[16 * W + 16];
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
		glDepthMask(GL_FALSE);
		tex_tri(0.5f); /* occluded: blending must not even show up */
		check(zbuf_unchanged(ref), "textured-blend: occluded blended draw leaves depth frozen");
		check(zb->pbuf[16 * W + 16] == before, "textured-blend: occluded blended draw paints nothing");
		glDisable(GL_DEPTH_TEST);
		tex_tri(0.5f); /* now it blends over the existing texels */
		check(zbuf_unchanged(ref), "textured-blend: test-off blended draw leaves depth frozen");
		{
			GLuint p = zb->pbuf[16 * W + 16];
			/* src and dst are the same texel, so every channel may only grow */
			check(pr(p) >= pr(before) && pg(p) >= pg(before) && pb(p) >= pb(before) && p != before,
			      "textured-blend: blended color composited (was %u,%u,%u now %u,%u,%u)",
			      pr(before), pg(before), pb(before), pr(p), pg(p), pb(p));
		}
	}
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);
}

/* ------------------------------------------------------------------ */
/* triangle straddling the near clipping plane                         */
/* ------------------------------------------------------------------ */
static void clip_tri(GLfloat z0, GLfloat r, GLfloat g, GLfloat b) {
	glColor3f(r, g, b);
	glBegin(GL_TRIANGLES);
	glVertex3f(-0.8f, -0.8f, z0); /* one vertex past the near plane at z=-1 */
	glVertex3f(0.8f, -0.8f, 0.5f);
	glVertex3f(0.0f, 0.8f, 0.5f);
	glEnd();
}

static void test_clipping(void) {
	GLushort ref[NPIX];
	reset_state();
	clip_tri(-1.5f, 1, 0, 0);
	check(count_nonz() > 50, "clip: clipped triangle still rasterizes depth");
	check(pr(zb->pbuf[16 * W + 16]) > 200, "clip: clipped triangle paints the center");
	snap_z(ref);

	glDepthMask(GL_FALSE);
	clip_tri(-1.5f, 0, 0, 1);
	/* comparison still happens with a closed mask; an equal depth passes, so
	   the color is allowed to update while the stored depth is frozen */
	check(pb(zb->pbuf[16 * W + 16]) > 200, "clip: equal-depth masked re-draw updates color only");
	check(zbuf_unchanged(ref), "clip: masked clipped draw leaves depth frozen");

	glDisable(GL_DEPTH_TEST);
	clip_tri(-1.5f, 0, 1, 0);
	check(pg(zb->pbuf[16 * W + 16]) > 200, "clip: test-off clipped draw paints the center");
	check(zbuf_unchanged(ref), "clip: test-off masked clipped draw leaves depth frozen");
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
}

/* ------------------------------------------------------------------ */
/* display lists: GL_COMPILE vs GL_COMPILE_AND_EXECUTE, repeated calls */
/* ------------------------------------------------------------------ */
static void test_lists(void) {
	GLushort ref[NPIX];

	/* GL_COMPILE records but never executes until glCallList */
	reset_state();
	check(get_int(GL_DEPTH_WRITEMASK) == 1, "lists: mask starts open");
	glNewList(10, GL_COMPILE);
	glDepthMask(GL_FALSE);
	glEndList();
	check(get_int(GL_DEPTH_WRITEMASK) == 1, "lists: GL_COMPILE must not change the mask immediately");
	tri(-0.5f, 1, 0, 0);
	check(count_nonz() > 100, "lists: draw after a compile-only list still writes depth");

	/* the list body runs in recorded order: mask off, far draw occluded,
	   clear, then mask back open and a near draw */
	reset_state();
	tri(-0.5f, 1, 0, 0);
	snap_z(ref);
	glDeleteList(10);
	glNewList(10, GL_COMPILE);
	glDepthMask(GL_FALSE);
	tri(0.5f, 0, 0, 1);                 /* must be occluded, depth frozen */
	glClear(GL_COLOR_BUFFER_BIT);      /* color clears, depth preserved */
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	tri(-0.7f, 0, 1, 0);               /* passes and stores */
	glEndList();
	glCallList(10);
	check(pb(zb->pbuf[16 * W + 16]) < 40 && pg(zb->pbuf[16 * W + 16]) > 200,
	      "lists: compiled list ends with the near green triangle");
	check(zb->pbuf[0] == 0, "lists: in-list glClear ran in recorded order");
	check(z_close(zb->zbuf[16 * W + 16], -0.7f), "lists: list re-opens the mask and stores new depth");
	check(get_int(GL_DEPTH_TEST) == 1, "lists: in-list glEnable took effect");

	/* GL_COMPILE_AND_EXECUTE executes immediately, and re-calling re-applies
	   the state; the compile-time mask must never be baked into the draws */
	reset_state();
	glDepthMask(GL_FALSE); /* different mask while compiling */
	glNewList(11, GL_COMPILE_AND_EXECUTE);
	glDepthMask(GL_TRUE);
	tri(-0.5f, 1, 0, 0);
	glDepthMask(GL_FALSE);
	glEndList();
	check(get_int(GL_DEPTH_WRITEMASK) == 0, "lists: compile-and-execute applies state at once");
	check(count_nonz() > 100, "lists: compile-and-execute drew immediately with the list's own mask");

	glDepthMask(GL_TRUE);
	check(get_int(GL_DEPTH_WRITEMASK) == 1, "lists: external mask change is visible");
	glCallList(11); /* repeats the whole recorded sequence */
	glCallList(11);
	check(get_int(GL_DEPTH_WRITEMASK) == 0, "lists: repeated calls re-execute the recorded mask");
	reset_state();
	snap_z(ref); /* all-zero depth */
	glCallList(11);
	/* the recorded near triangle runs with the list's own mask-true state */
	check(count_nonz() > 100, "lists: list's internal draw executes on every call");
	check(z_close(zb->zbuf[16 * W + 16], -0.5f), "lists: repeated call stores the near depth");
	check(get_int(GL_DEPTH_WRITEMASK) == 0, "lists: list ends masked, so its last state sticks");
	glDepthMask(GL_TRUE);

	/* GL_COMPILE list created while masked: its internal mask-true draw must
	   still write when called later from an open-mask outside context */
	reset_state();
	glDepthMask(GL_FALSE);
	glNewList(12, GL_COMPILE);
	glDepthMask(GL_TRUE);
	tri(-0.5f, 0, 0, 1);
	glDepthMask(GL_FALSE);
	glEndList();
	glDepthMask(GL_TRUE);
	glCallList(12);
	check(count_nonz() > 100, "lists: compile-time mask is not baked into recorded primitives");
	check(get_int(GL_DEPTH_WRITEMASK) == 0, "lists: recorded final mask is applied on call");
	glDepthMask(GL_TRUE);
}

/* ------------------------------------------------------------------ */
/* PPM story panels: foreground, read-only depth draw, background     */
/* ------------------------------------------------------------------ */
static void render_panel(GLuint* dst) {
	tri(0.5f, 0, 0, 1);  /* far background blue */
	tri(-0.5f, 1, 0, 0); /* near foreground red */
	snap_color(dst);
}

static void write_ppm(const char* path, const GLuint* p1, const GLuint* p2, const GLuint* p3) {
	FILE* f = fopen(path, "wb");
	const int scale = 3;
	int x, y, panel;
	const GLuint* panels[3] = {p1, p2, p3};
	if (!f) {
		printf("FAIL: cannot open %s\n", path);
		failures++;
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", 3 * W * scale, H * scale);
	for (y = 0; y < H * scale; y++) {
		for (panel = 0; panel < 3; panel++)
			for (x = 0; x < W * scale; x++) {
				GLuint p = panels[panel][(y / scale) * W + (x / scale)];
				fputc((int)pr(p), f);
				fputc((int)pg(p), f);
				fputc((int)pb(p), f);
			}
	}
	fclose(f);
}

int main(int argc, char** argv) {
	const char* ppm_path = (argc > 1) ? argv[1] : "depth_state.ppm";
	static GLuint panel1[NPIX], panel2[NPIX], panel3[NPIX];

	zb = ZB_open(W, H, ZB_MODE_RGBA, NULL);
	check(zb != NULL, "main: ZB_open failed");
	if (!zb)
		return 1;
	glInit(zb);
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	test_combo_readwrite();
	test_combo_readonly();
	test_combo_notest_write();
	test_combo_notest_nowrite();
	test_smooth();
	test_clear();
	test_points();
	test_lines();
	test_drawpixels();
	test_textured();
	test_clipping();
	test_lists();

	/* visual record of the three states */
	reset_state();
	render_panel(panel1);
	glDepthMask(GL_FALSE);
	tri(0.5f, 0, 1, 0); /* read-only depth: occluded, panel keeps the foreground */
	snap_color(panel2);
	glDisable(GL_DEPTH_TEST);
	tri(0.5f, 0, 0, 1); /* background overlays once the test is off */
	snap_color(panel3);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	write_ppm(ppm_path, panel1, panel2, panel3);
	printf("ppm: %s\n", ppm_path);

	glClose();
	ZB_close(zb);

	printf("%d checks, %d failures\n", checks, failures);
	if (failures == 0)
		printf("DEPTH_STATE_OK\n");
	return failures ? 1 : 0;
}
