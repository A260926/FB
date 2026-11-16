/*
 * Framebuffer object tests.
 *
 * Every case drives the real rasterizer: geometry is rendered into
 * framebuffer backed textures, then sampled back through either
 * glGetTexturePixmap() or the normal texturing path.  No main() is
 * defined here; task_support/driver.c supplies it.
 */

#include <GL/gl.h>
#include <zbuffer.h>
#include "task_support/check.h"

#include <stdlib.h>
#include <string.h>

#define WIN 64
#define D TGL_FEATURE_TEXTURE_DIM

static ZBuffer* g_zb;

static void cleanup_context(void) {
	glClose();
	ZB_close(g_zb);
}

static int ensure_init(void) {
	if (g_zb != NULL)
		return 0;
	g_zb = ZB_open(WIN, WIN, ZB_MODE_RGBA, NULL);
	TASK_CHECK(g_zb != NULL);
	glInit(g_zb);
	atexit(cleanup_context);
	glViewport(0, 0, WIN, WIN);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	return 0;
}

static void solid_rgb(unsigned char* px, unsigned char r,
                      unsigned char g, unsigned char b) {
	int i;
	for (i = 0; i < D * D; i++) {
		px[3 * i] = r;
		px[3 * i + 1] = g;
		px[3 * i + 2] = b;
	}
}

static int make_texture(unsigned char r, unsigned char g, unsigned char b,
                        GLuint* out) {
	GLuint tex;
	unsigned char* px = malloc((size_t)D * D * 3);
	TASK_CHECK(px != NULL);
	solid_rgb(px, r, g, b);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, 3, D, D, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
	free(px);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	*out = tex;
	return 0;
}

static int tex_pixels(GLuint tex, PIXEL** out) {
	GLint xs, ys;
	PIXEL* p = glGetTexturePixmap((GLint)tex, 0, &xs, &ys);
	TASK_CHECK(p != NULL);
	TASK_CHECK(xs == D && ys == D);
	*out = p;
	return 0;
}

static void fullscreen_quad(void) {
	glBegin(GL_QUADS);
	glVertex3f(-1.0f, -1.0f, 0.0f);
	glVertex3f(1.0f, -1.0f, 0.0f);
	glVertex3f(1.0f, 1.0f, 0.0f);
	glVertex3f(-1.0f, 1.0f, 0.0f);
	glEnd();
}

static void center_quad(float z) {
	glBegin(GL_QUADS);
	glVertex3f(-0.5f, -0.5f, z);
	glVertex3f(0.5f, -0.5f, z);
	glVertex3f(0.5f, 0.5f, z);
	glVertex3f(-0.5f, 0.5f, z);
	glEnd();
}

static int expect_color(const PIXEL* p, int w, int x, int y,
                        unsigned char r, unsigned char g, unsigned char b) {
	PIXEL px = p[y * w + x];
	TASK_CHECK(abs((int)GET_RED(px) - r) <= 8);
	TASK_CHECK(abs((int)GET_GREEN(px) - g) <= 8);
	TASK_CHECK(abs((int)GET_BLUE(px) - b) <= 8);
	return 0;
}

static int make_fbo(GLuint* out) {
	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	TASK_CHECK(fbo != 0);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	*out = fbo;
	return 0;
}

static int attach(GLuint fbo, GLuint tex) {
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, tex, 0);
	return 0;
}

/* ---------------- routing ---------------- */

static int routing_two_pass(void) {
	GLuint tex, fbo;
	PIXEL* tp;
	TASK_CHECK(ensure_init() == 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	TASK_CHECK(attach(fbo, tex) == 0);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	glViewport(0, 0, D, D);
	glColor3f(1, 0, 0);
	fullscreen_quad();
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, 8, 8, 255, 0, 0) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 255, 0, 0) == 0);
	TASK_CHECK(expect_color(tp, D, D - 9, D - 9, 255, 0, 0) == 0);
	/* the default target was not touched by the FBO draw */
	TASK_CHECK(expect_color(g_zb->pbuf, WIN, WIN / 2, WIN / 2, 0, 0, 0) == 0);

	/* pass 2: texture the default target with what was rendered */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	fullscreen_quad();
	glDisable(GL_TEXTURE_2D);
	TASK_CHECK(expect_color(g_zb->pbuf, WIN, WIN / 2, WIN / 2, 255, 0, 0) == 0);
	TASK_CHECK(expect_color(g_zb->pbuf, WIN, 4, 4, 255, 0, 0) == 0);

	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	return 0;
}

static int routing_interleave(void) {
	GLuint ta, tb, fa, fb;
	PIXEL* pa;
	PIXEL* pb;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &ta) == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tb) == 0);
	TASK_CHECK(make_fbo(&fa) == 0);
	TASK_CHECK(make_fbo(&fb) == 0);

	TASK_CHECK(attach(fa, ta) == 0);
	glViewport(0, 0, D, D);
	glColor3f(1, 0, 0);
	fullscreen_quad();

	TASK_CHECK(attach(fb, tb) == 0);
	glColor3f(0, 1, 0);
	fullscreen_quad();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glClearColor(0, 0, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	/* re-interleave: each target keeps its own color storage */
	glBindFramebuffer(GL_FRAMEBUFFER, fa);
	TASK_CHECK(tex_pixels(ta, &pa) == 0);
	TASK_CHECK(expect_color(pa, D, D / 2, D / 2, 255, 0, 0) == 0);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	TASK_CHECK(tex_pixels(tb, &pb) == 0);
	TASK_CHECK(expect_color(pb, D, D / 2, D / 2, 0, 255, 0) == 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	TASK_CHECK(expect_color(g_zb->pbuf, WIN, WIN / 2, WIN / 2, 0, 0, 255) == 0);
	TASK_CHECK(pa[D / 2 * D + D / 2] != pb[D / 2 * D + D / 2]);

	glDeleteFramebuffers(1, &fa);
	glDeleteFramebuffers(1, &fb);
	glDeleteTextures(1, &ta);
	glDeleteTextures(1, &tb);
	return 0;
}

static int routing_incomplete(void) {
	GLuint fbo, tex;
	PIXEL* tp;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
	           GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);

	/* clear and draw on an incomplete target are errors that change nothing */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	TASK_CHECK(glGetError() == GL_INVALID_FRAMEBUFFER_OPERATION);
	glColor3f(1, 1, 1);
	glBegin(GL_TRIANGLES);
	glVertex3f(-0.5f, -0.5f, 0.0f);
	glVertex3f(0.5f, -0.5f, 0.0f);
	glVertex3f(0.0f, 0.5f, 0.0f);
	glEnd();
	TASK_CHECK(glGetError() == GL_INVALID_FRAMEBUFFER_OPERATION);

	/* reading an incomplete target with glCopyTexImage2D is rejected too */
	{
		GLuint dst;
		TASK_CHECK(make_texture(0, 0, 0, &dst) == 0);
		glBindTexture(GL_TEXTURE_2D, dst);
		glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, D, D, D, 0);
		TASK_CHECK(glGetError() == GL_INVALID_FRAMEBUFFER_OPERATION);
		glDeleteTextures(1, &dst);
	}

	/* glDrawPixels into an incomplete target is rejected without crashing */
	{
		GLuint one_pixel = 0xFFFFFFFFu;
		glRasterPos2f(0.0f, 0.0f);
		glDrawPixels(1, 1, GL_RGB, GL_UNSIGNED_INT_8_8_8_8, &one_pixel);
		TASK_CHECK(glGetError() == GL_INVALID_FRAMEBUFFER_OPERATION);
	}

	/* a freshly created texture has no defined image: still incomplete */
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, tex, 0);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
	           GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);
	{
		unsigned char* px = malloc((size_t)D * D * 3);
		TASK_CHECK(px != NULL);
		solid_rgb(px, 0, 0, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, 3, D, D, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
		free(px);
	}
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	glViewport(0, 0, D, D);
	glColor3f(0, 0, 1);
	fullscreen_quad();
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 0, 0, 255) == 0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	return 0;
}

static int routing_feedback(void) {
	GLuint tex, fbo, other;
	static const float verts[9] = {
	    -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
	PIXEL before;
	PIXEL* tp;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(20, 20, 20, &tex) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	TASK_CHECK(attach(fbo, tex) == 0);
	glViewport(0, 0, D, D);
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	before = tp[D / 2 * D + D / 2];

	/* sampling the texture currently attached as color is rejected */
	glBindTexture(GL_TEXTURE_2D, tex);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	glBegin(GL_TRIANGLES);
	glTexCoord2f(0, 0);
	glVertex3f(-1.0f, -1.0f, 0.0f);
	glTexCoord2f(1, 0);
	glVertex3f(1.0f, -1.0f, 0.0f);
	glTexCoord2f(0.5f, 1);
	glVertex3f(0.0f, 1.0f, 0.0f);
	glEnd();
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	TASK_CHECK(tp[D / 2 * D + D / 2] == before);

	/* the array-draw submission path must reject it as well */
	glVertexPointer(3, GL_FLOAT, 0, verts);
	glEnableClientState(GL_VERTEX_ARRAY);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	TASK_CHECK(tp[D / 2 * D + D / 2] == before);
	glDisableClientState(GL_VERTEX_ARRAY);

	/* another texture may be sampled normally while this one is attached */
	TASK_CHECK(make_texture(100, 100, 100, &other) == 0);
	glBindTexture(GL_TEXTURE_2D, other);
	glBegin(GL_TRIANGLES);
	glTexCoord2f(0, 0);
	glVertex3f(-1.0f, -1.0f, 0.0f);
	glTexCoord2f(1, 0);
	glVertex3f(1.0f, -1.0f, 0.0f);
	glTexCoord2f(0.5f, 1);
	glVertex3f(0.0f, 1.0f, 0.0f);
	glEnd();
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	TASK_CHECK(tp[D / 2 * D + D / 2] != before);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &other);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	return 0;
}

static int routing_copytex(void) {
	GLuint t1, t2, fbo;
	PIXEL* p1;
	PIXEL* p2;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &t1) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	TASK_CHECK(attach(fbo, t1) == 0);
	glViewport(0, 0, D, D);
	glColor3f(1, 0, 0);
	fullscreen_quad();

	/* full-image copy follows the original convention x=0, y=D, w=h=D */
	TASK_CHECK(make_texture(0, 0, 0, &t2) == 0);
	glBindTexture(GL_TEXTURE_2D, t2);
	glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, D, D, D, 0);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	TASK_CHECK(tex_pixels(t2, &p2) == 0);
	TASK_CHECK(expect_color(p2, D, D / 2, D / 2, 255, 0, 0) == 0);

	/* the copy is an independent snapshot */
	glBindTexture(GL_TEXTURE_2D, t1);
	glColor3f(0, 1, 0);
	fullscreen_quad();
	TASK_CHECK(tex_pixels(t1, &p1) == 0);
	TASK_CHECK(expect_color(p1, D, D / 2, D / 2, 0, 255, 0) == 0);
	TASK_CHECK(expect_color(p2, D, D / 2, D / 2, 255, 0, 0) == 0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &t1);
	glDeleteTextures(1, &t2);
	return 0;
}

static int routing_display_list(void) {
	GLuint tex, fbo, list;
	PIXEL* tp;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);

	/* framebuffer management inside a list executes immediately */
	list = glGenLists(2);
	TASK_CHECK(glIsList(list) == GL_TRUE);
	TASK_CHECK(glIsList(list + 1) == GL_TRUE);
	glNewList(list, GL_COMPILE);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, tex, 0);
	glEndList();
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

	/* replay draws into whatever target is bound at replay time */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glNewList(list + 1, GL_COMPILE);
	glColor3f(1, 1, 0);
	fullscreen_quad();
	glEndList();

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, D, D);
	glCallList(list + 1);
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 255, 255, 0) == 0);
	/* the default target stayed black: replay used the FBO binding */
	TASK_CHECK(expect_color(g_zb->pbuf, WIN, WIN / 2, WIN / 2, 0, 0, 0) == 0);

	/* the management calls were not recorded: replay leaves target 0 active */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glCallList(list);
	glColor3f(1, 0, 0);
	fullscreen_quad();
	/* a recorded bind would have sent this quad into the texture */
	TASK_CHECK(expect_color(g_zb->pbuf, WIN, WIN / 2, WIN / 2, 255, 0, 0) == 0);
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 255, 255, 0) == 0);

	glDeleteLists(list, 2);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	return 0;
}

/* ---------------- aliasing ---------------- */

static int aliasing_shared_color(void) {
	GLuint tex, fa, fb;
	PIXEL* tp;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fa) == 0);
	TASK_CHECK(make_fbo(&fb) == 0);

	TASK_CHECK(attach(fa, tex) == 0);
	glViewport(0, 0, D, D);
	glColor3f(1, 0, 0);
	fullscreen_quad();

	TASK_CHECK(attach(fb, tex) == 0);
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 255, 0, 0) == 0); /* B sees A's pixels */
	glColor3f(0, 1, 0);
	fullscreen_quad();

	glBindFramebuffer(GL_FRAMEBUFFER, fa);
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 0, 255, 0) == 0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fa);
	glDeleteFramebuffers(1, &fb);
	glDeleteTextures(1, &tex);
	return 0;
}

static int aliasing_independent_depth(void) {
	GLuint tex, fa, fb;
	PIXEL* tp;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fa) == 0);
	TASK_CHECK(make_fbo(&fb) == 0);
	glEnable(GL_DEPTH_TEST);

	TASK_CHECK(attach(fa, tex) == 0);
	glViewport(0, 0, D, D);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(1, 0, 0);
	center_quad(-0.5f); /* near */
	glColor3f(0, 1, 0);
	center_quad(0.5f);  /* far, occluded on A */
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 255, 0, 0) == 0);

	/* B shares the color image but its depth was never written */
	TASK_CHECK(attach(fb, tex) == 0);
	glColor3f(0, 1, 0);
	center_quad(0.5f); /* passes against B's fresh far depth */
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 0, 255, 0) == 0);

	/* A still owns its near depth: another far quad stays blocked */
	glBindFramebuffer(GL_FRAMEBUFFER, fa);
	glColor3f(0, 0, 1);
	center_quad(0.5f);
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 0, 255, 0) == 0);

	glDisable(GL_DEPTH_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fa);
	glDeleteFramebuffers(1, &fb);
	glDeleteTextures(1, &tex);
	return 0;
}

static int aliasing_reattach_keeps_depth(void) {
	GLuint tex, fbo;
	PIXEL* tp;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	glEnable(GL_DEPTH_TEST);
	TASK_CHECK(attach(fbo, tex) == 0);
	glViewport(0, 0, D, D);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(1, 0, 0);
	center_quad(-0.5f);
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 255, 0, 0) == 0);

	/* detach and re-attach the same image: color and depth both survive */
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, 0, 0);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
	           GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, tex, 0);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 255, 0, 0) == 0);
	glColor3f(0, 1, 0);
	center_quad(0.5f);
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 255, 0, 0) == 0);

	glDisable(GL_DEPTH_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	return 0;
}

static int aliasing_teximage_resets(void) {
	GLuint tex, fbo;
	PIXEL* tp;
	unsigned char* px;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	glEnable(GL_DEPTH_TEST);
	TASK_CHECK(attach(fbo, tex) == 0);
	glViewport(0, 0, D, D);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(1, 0, 0);
	center_quad(-0.5f); /* near geometry, then the image is redefined */

	px = malloc((size_t)D * D * 3);
	TASK_CHECK(px != NULL);
	solid_rgb(px, 0, 40, 40);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, 3, D, D, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
	free(px);
	/* attachment follows the new image storage and depth is reset */
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	glColor3f(0, 0, 1);
	center_quad(0.5f); /* far geometry now passes */
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 0, 0, 255) == 0);
	TASK_CHECK(expect_color(tp, D, 2, 2, 0, 40, 40) == 0);

	glDisable(GL_DEPTH_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	return 0;
}

/* ---------------- state ---------------- */

static int state_switch_preserves(void) {
	GLuint tex, fbo;
	GLint vp[4], mode, ival;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(50, 60, 70, &tex) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	TASK_CHECK(attach(fbo, tex) == 0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(7, 3, 40, 30);
	glMatrixMode(GL_PROJECTION);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glEnable(GL_POLYGON_STIPPLE);
	glBindTexture(GL_TEXTURE_2D, tex);

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGetIntegerv(GL_VIEWPORT, vp);
	TASK_CHECK(vp[0] == 7 && vp[1] == 3 && vp[2] == 40 && vp[3] == 30);
	glGetIntegerv(GL_MATRIX_MODE, &mode);
	TASK_CHECK(mode == GL_PROJECTION);
	glGetIntegerv(GL_DEPTH_TEST, &ival);
	TASK_CHECK(ival == 1);
	glGetIntegerv(GL_BLEND, &ival);
	TASK_CHECK(ival == 1);
	glGetIntegerv(GL_POLYGON_STIPPLE, &ival);
	TASK_CHECK(ival == 1);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glGetIntegerv(GL_VIEWPORT, vp);
	TASK_CHECK(vp[0] == 7 && vp[1] == 3 && vp[2] == 40 && vp[3] == 30);
	glGetIntegerv(GL_DEPTH_TEST, &ival);
	TASK_CHECK(ival == 1);
	/* texture binding survived the round trip: it still samples tex */
	glViewport(0, 0, WIN, WIN);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	fullscreen_quad();
	TASK_CHECK(expect_color(g_zb->pbuf, WIN, WIN / 2, WIN / 2, 50, 60, 70) == 0);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glDisable(GL_POLYGON_STIPPLE);
	glDisable(GL_DEPTH_TEST);

	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	return 0;
}

static int state_blend_follows(void) {
	GLuint tex, fbo;
	PIXEL* tp;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE); /* additive */
	TASK_CHECK(attach(fbo, tex) == 0);
	glViewport(0, 0, D, D);
	glClearColor(0, 1, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(1, 0, 0);
	fullscreen_quad(); /* red added to green gives yellow */
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 255, 255, 0) == 0);

	glDisable(GL_BLEND);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	return 0;
}

static int state_stipple_follows(void) {
	GLuint tex, fbo;
	static GLubyte pattern[TGL_POLYGON_STIPPLE_BYTES];
	PIXEL* tp;
	int i, holes = 0;
	TASK_CHECK(ensure_init() == 0);
	memset(pattern, 0xAA, sizeof(pattern));
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	glPolygonStipple(pattern);
	glEnable(GL_POLYGON_STIPPLE);
	TASK_CHECK(attach(fbo, tex) == 0);
	glViewport(0, 0, D, D);
	glColor3f(1, 1, 1);
	fullscreen_quad();
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	for (i = 0; i < D * D; i++)
		if (GET_RED(tp[i]) == 0 && GET_GREEN(tp[i]) == 0 && GET_BLUE(tp[i]) == 0)
			holes++;
	TASK_CHECK(holes > D * D / 4); /* roughly half the pixels are discarded */

	glDisable(GL_POLYGON_STIPPLE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	return 0;
}

static int state_points_lines_clip(void) {
	GLuint tex, fbo;
	PIXEL* tp;
	int i, lit = 0, px = -1, py = -1;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	TASK_CHECK(attach(fbo, tex) == 0);
	glViewport(0, 0, D, D);
	glColor3f(1, 1, 1);

	/* point and line primitives must reach the current target */
	glBegin(GL_POINTS);
	glVertex3f(0.0f, 0.0f, 0.0f);
	glEnd();
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	for (i = 0; i < D * D; i++) {
		if (GET_RED(tp[i]) > 200) {
			lit++;
			if (px < 0) {
				px = i % D;
				py = i / D;
			}
		}
	}
	TASK_CHECK(lit == 1); /* a single square point */
	TASK_CHECK(abs(px - D / 2) <= 1 && abs(py - D / 2) <= 1);

	lit = 0;
	glBegin(GL_LINES);
	glVertex3f(-0.5f, -0.5f, 0.0f);
	glVertex3f(0.5f, 0.5f, 0.0f);
	glEnd();
	for (i = 0; i < D * D; i++)
		if (GET_RED(tp[i]) > 200)
			lit++;
	TASK_CHECK(lit > D / 4);

	/* clipped triangle: inside filled, fully-outside triangle changes nothing */
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBegin(GL_TRIANGLES);
	glVertex3f(-2.0f, -2.0f, 0.0f);
	glVertex3f(2.0f, -2.0f, 0.0f);
	glVertex3f(0.0f, 2.0f, 0.0f);
	glEnd();
	TASK_CHECK(GET_RED(tp[D / 2 * D + D / 2]) > 200);
	glBegin(GL_TRIANGLES);
	glVertex3f(1.5f, 1.5f, 0.0f);
	glVertex3f(2.5f, 1.5f, 0.0f);
	glVertex3f(2.0f, 2.5f, 0.0f);
	glEnd();
	TASK_CHECK(expect_color(tp, D, 2, 2, 0, 0, 0) == 0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	return 0;
}

static int state_flat_smooth(void) {
	GLuint tex, fbo;
	PIXEL* tp;
	PIXEL a, b;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	TASK_CHECK(attach(fbo, tex) == 0);
	glViewport(0, 0, D, D);

	glShadeModel(GL_FLAT);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBegin(GL_TRIANGLES);
	glColor3f(1, 0, 0);
	glVertex3f(-0.9f, -0.9f, 0.0f);
	glColor3f(0, 1, 0);
	glVertex3f(0.9f, -0.9f, 0.0f);
	glColor3f(0, 0, 1);
	glVertex3f(0.0f, 0.9f, 0.0f);
	glEnd();
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	a = tp[(D / 2 - 20) * D + D / 2];
	b = tp[(D / 2 + 20) * D + D / 2];
	TASK_CHECK(GET_RED(a) < 40 && GET_RED(b) < 40); /* flat: last vertex color */
	TASK_CHECK(GET_BLUE(a) > 150 && GET_BLUE(b) > 150);

	glShadeModel(GL_SMOOTH);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBegin(GL_TRIANGLES);
	glColor3f(1, 0, 0);
	glVertex3f(-0.9f, -0.9f, 0.0f);
	glColor3f(0, 1, 0);
	glVertex3f(0.9f, -0.9f, 0.0f);
	glColor3f(0, 0, 1);
	glVertex3f(0.0f, 0.9f, 0.0f);
	glEnd();
	a = tp[(D / 3) * D + D / 3];
	b = tp[(2 * D / 3) * D + 2 * D / 3];
	TASK_CHECK(a != b); /* smooth: interpolated gradient */

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	return 0;
}

/* ---------------- lifetime ---------------- */

static int life_errors_and_status(void) {
	GLuint fbo, tex;
	GLsizei bad_n = -1;
	TASK_CHECK(ensure_init() == 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

	glGenFramebuffers(bad_n, &fbo);
	TASK_CHECK(glGetError() == GL_INVALID_VALUE);
	glDeleteFramebuffers(bad_n, &fbo);
	TASK_CHECK(glGetError() == GL_INVALID_VALUE);
	glGenFramebuffers(0, &fbo);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	glBindFramebuffer(0x1234, 0);
	TASK_CHECK(glGetError() == GL_INVALID_ENUM);
	glCheckFramebufferStatus(0x1234);
	TASK_CHECK(glGetError() == GL_INVALID_ENUM);
	/* binding a name that was never generated is invalid and changes nothing */
	glBindFramebuffer(GL_FRAMEBUFFER, 7777);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

	TASK_CHECK(make_fbo(&fbo) == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(attach(fbo, tex) == 0);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

	/* bad arguments leave the existing attachment untouched */
	glFramebufferTexture2D(0x1234, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	TASK_CHECK(glGetError() == GL_INVALID_ENUM);
	glFramebufferTexture2D(GL_FRAMEBUFFER, 0x8CE1, GL_TEXTURE_2D, tex, 0);
	TASK_CHECK(glGetError() == GL_INVALID_ENUM);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_1D, tex, 0);
	TASK_CHECK(glGetError() == GL_INVALID_ENUM);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, tex, 1);
	TASK_CHECK(glGetError() == GL_INVALID_VALUE);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, 9999, 0);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

	/* attachment operations on the default target are invalid */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, tex, 0);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);

	/* texture 0 detaches and makes the target incomplete */
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, 0, 0);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
	           GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	return 0;
}

static int life_delete_current(void) {
	GLuint fbo, tex;
	PIXEL* tp;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	TASK_CHECK(attach(fbo, tex) == 0);
	glViewport(0, 0, D, D);
	glColor3f(1, 0, 0);
	fullscreen_quad();
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 255, 0, 0) == 0);

	glDeleteFramebuffers(1, &fbo);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	/* deleting the current target returns to name 0 */
	glViewport(0, 0, WIN, WIN);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(0, 1, 0);
	fullscreen_quad();
	TASK_CHECK(expect_color(g_zb->pbuf, WIN, WIN / 2, WIN / 2, 0, 255, 0) == 0);

	/* repeated deletion and deleting name 0 are no-ops */
	glDeleteFramebuffers(1, &fbo);
	fbo = 0;
	glDeleteFramebuffers(1, &fbo);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	glDeleteTextures(1, &tex);
	return 0;
}

static int life_delete_texture_detaches(void) {
	GLuint tex, fa, fb;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fa) == 0);
	TASK_CHECK(make_fbo(&fb) == 0);
	TASK_CHECK(attach(fa, tex) == 0);
	TASK_CHECK(attach(fb, tex) == 0);
	TASK_CHECK(glIsTexture(tex) == GL_TRUE);

	glBindFramebuffer(GL_FRAMEBUFFER, fa);
	glDeleteTextures(1, &tex);
	TASK_CHECK(glIsTexture(tex) == GL_FALSE);
	/* every framebuffer referencing it is now detached */
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
	           GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	TASK_CHECK(glGetError() == GL_INVALID_FRAMEBUFFER_OPERATION);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
	           GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fa);
	glDeleteFramebuffers(1, &fb);
	return 0;
}

static int life_begin_protection(void) {
	GLuint fbo, tex, other;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	TASK_CHECK(make_fbo(&other) == 0);
	TASK_CHECK(attach(fbo, tex) == 0);

	glBegin(GL_TRIANGLES);
	glGenFramebuffers(1, &other);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	glBindFramebuffer(GL_FRAMEBUFFER, other);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, 0, 0);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	glCheckFramebufferStatus(GL_FRAMEBUFFER);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	glDeleteFramebuffers(1, &other);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	glEnd();
	/* binding and attachment survived the rejected calls */
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fbo);
	glDeleteFramebuffers(1, &other);
	glDeleteTextures(1, &tex);
	return 0;
}

static int life_not_in_display_list(void) {
	GLuint fbo, list;
	GLint vp[4];
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	list = glGenLists(1);
	glNewList(list, GL_COMPILE);
	/* management calls execute immediately and are never recorded */
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glEndList();
	/* immediate execution: the bind happened while compiling */
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
	           GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glCallList(list);
	glGetIntegerv(GL_VIEWPORT, vp);
	/* the replay contains no framebuffer op, so the default binding stays */
	TASK_CHECK(vp[2] == WIN && vp[3] == WIN);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

	glDeleteLists(list, 1);
	glDeleteFramebuffers(1, &fbo);
	return 0;
}

static int life_close_and_reinit(void) {
	GLuint tex, fbo;
	PIXEL* tp;
	TASK_CHECK(ensure_init() == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(make_fbo(&fbo) == 0);
	TASK_CHECK(attach(fbo, tex) == 0);
	glViewport(0, 0, D, D);
	glColor3f(1, 1, 0);
	fullscreen_quad();
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 255, 255, 0) == 0);

	glClose(); /* frees FBO-owned depth and all context objects */
	glInit(g_zb); /* caller keeps ownership of the default ZBuffer */
	glViewport(0, 0, WIN, WIN);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);

	/* a fresh name space: the full render cycle works again */
	TASK_CHECK(make_fbo(&fbo) == 0);
	TASK_CHECK(make_texture(0, 0, 0, &tex) == 0);
	TASK_CHECK(attach(fbo, tex) == 0);
	TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	glViewport(0, 0, D, D);
	glColor3f(0, 1, 1);
	fullscreen_quad();
	TASK_CHECK(tex_pixels(tex, &tp) == 0);
	TASK_CHECK(expect_color(tp, D, D / 2, D / 2, 0, 255, 255) == 0);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	return 0;
}

#define CASE(g, n) { #g, #n, n }

const struct task_case task_cases[] = {
    CASE(routing, routing_two_pass),
    CASE(routing, routing_interleave),
    CASE(routing, routing_incomplete),
    CASE(routing, routing_feedback),
    CASE(routing, routing_copytex),
    CASE(routing, routing_display_list),

    CASE(aliasing, aliasing_shared_color),
    CASE(aliasing, aliasing_independent_depth),
    CASE(aliasing, aliasing_reattach_keeps_depth),
    CASE(aliasing, aliasing_teximage_resets),

    CASE(state, state_switch_preserves),
    CASE(state, state_blend_follows),
    CASE(state, state_stipple_follows),
    CASE(state, state_points_lines_clip),
    CASE(state, state_flat_smooth),

    CASE(lifetime, life_errors_and_status),
    CASE(lifetime, life_delete_current),
    CASE(lifetime, life_delete_texture_detaches),
    CASE(lifetime, life_begin_protection),
    CASE(lifetime, life_not_in_display_list),
    CASE(lifetime, life_close_and_reinit),
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
