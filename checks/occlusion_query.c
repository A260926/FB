/*
 * Occlusion query extension tests (pglBeginOcclusionQuery /
 * pglEndOcclusionQuery). Windowless, like checks/baseline.c.
 *
 * Every scenario runs the identical draw sequence twice: once without a
 * query and once wrapped in a query. The color/depth/stencil storage must
 * match byte-for-byte (the query is observation-only), and the reported
 * sample counts are checked against the exact rasterized coverage.
 */
#define PORTABLEGL_IMPLEMENTATION
#include "portablegl.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 24
#define H 24
#define NPIX (W * H)

static glContext the_Context;
static pix_t* pixels;

/* ------------------------------------------------------------------ */
/* Shaders                                                            */
/* ------------------------------------------------------------------ */

static void vs_identity(float* o, vec4* a, Shader_Builtins* b, void* u)
{
	(void)o; (void)u;
	b->gl_Position = a[0];
}

static void fs_uniform(float* i, Shader_Builtins* b, void* u)
{
	(void)i;
	b->gl_FragColor = *(vec4*)u;
}

/* Discards the left half of the screen (fragCoord origin is bottom-left). */
static void fs_discard_hole(float* i, Shader_Builtins* b, void* u)
{
	(void)i; (void)u;
	if (b->gl_FragCoord.x < (float)W * 0.5f)
		b->discard = GL_TRUE;
	b->gl_FragColor = make_v4(0.f, 1.f, 0.f, 1.f);
}

#ifndef PGL_NO_DEPTH_NO_STENCIL
/* Right half pushes gl_FragDepth to the far plane and must fail LESS. */
static void fs_fragdepth(float* i, Shader_Builtins* b, void* u)
{
	(void)i; (void)u;
	if (b->gl_FragCoord.x >= (float)W * 0.5f)
		b->gl_FragDepth = 1.0f; /* clear depth, LESS rejects it */
	b->gl_FragColor = make_v4(0.f, 1.f, 1.f, 1.f);
}
#endif

/* Two color attachments, one fragment -> one counted sample. */
#ifndef PGL_NO_DEPTH_NO_STENCIL
static void fs_mrt(float* i, Shader_Builtins* b, void* u)
{
	(void)i; (void)u;
	b->gl_FragData[0] = make_v4(1.f, 0.f, 0.f, 1.f);
	b->gl_FragData[1] = make_v4(0.f, 0.f, 1.f, 1.f);
}
#endif

/* Instance 0/1 are shifted left/right so the two triangles never overlap. */
static void vs_instance(float* o, vec4* a, Shader_Builtins* b, void* u)
{
	(void)o; (void)u;
	vec4 p = a[0];
	p.x = p.x + (b->gl_InstanceID ? 0.6f : -0.6f);
	b->gl_Position = p;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static GLuint make_program(vert_func vs, frag_func fs, GLboolean fdod)
{
	GLuint p = pglCreateProgram(vs, fs, 0, NULL, fdod);
	assert(p);
	return p;
}

static GLuint upload_floats(const float* data, size_t bytes)
{
	GLuint buf;
	glGenBuffers(1, &buf);
	glBindBuffer(GL_ARRAY_BUFFER, buf);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, data, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
	return buf;
}

/* Fullscreen quad as a triangle strip (z supplied per quad). */
static void upload_quad(float z)
{
	float q[] = {
		-1.f, -1.f, z,
		 1.f, -1.f, z,
		-1.f,  1.f, z,
		 1.f,  1.f, z,
	};
	upload_floats(q, sizeof(q));
}

static void expect_no_error(void)
{
#ifndef PGL_UNSAFE
	assert(glGetError() == GL_NO_ERROR);
#endif
}

static void begin_ok(void)
{
	assert(pglBeginOcclusionQuery() == GL_TRUE);
}

static GLuint64 end_ok(void)
{
	GLuint64 s = 999;
	assert(pglEndOcclusionQuery(&s) == GL_TRUE);
	return s;
}

/* ------------------------------------------------------------------ */
/* Storage snapshots for query/no-query equivalence                   */
/* ------------------------------------------------------------------ */

typedef struct {
	pix_t* color;
	size_t color_n;
#ifndef PGL_NO_DEPTH_NO_STENCIL
	void* depth;
	size_t depth_n;
#  ifndef PGL_NO_STENCIL
	void* stencil;
	size_t stencil_n;
#  endif
#endif
} Snap;

static void snap_take(Snap* s)
{
	s->color_n = NPIX * sizeof(pix_t);
	s->color = (pix_t*)malloc(s->color_n);
	assert(s->color);
	memcpy(s->color, pixels, s->color_n);
#ifndef PGL_NO_DEPTH_NO_STENCIL
	glFramebuffer* zb = &the_Context.zbuf;
	s->depth_n = (size_t)zb->w * zb->h *
	             (the_Context.zbuf_float ? sizeof(float) :
#  ifdef PGL_D16
	              sizeof(u16));
#  else
	              sizeof(u32));
#  endif
	s->depth = malloc(s->depth_n);
	assert(s->depth);
	memcpy(s->depth, zb->buf, s->depth_n);
#  ifndef PGL_NO_STENCIL
#    ifdef PGL_D16
	glFramebuffer* sb = &the_Context.stencil_buf;
	s->stencil_n = (size_t)sb->w * sb->h;
	s->stencil = malloc(s->stencil_n);
	assert(s->stencil);
	memcpy(s->stencil, sb->buf, s->stencil_n);
#    else
	/* D24S8: stencil is the low byte of each depth word, already snapshotted. */
	s->stencil = NULL;
	s->stencil_n = 0;
#    endif
#  endif
#endif
}

static void snap_equal(const Snap* a, const Snap* b)
{
	assert(a->color_n == b->color_n);
	assert(memcmp(a->color, b->color, a->color_n) == 0);
#ifndef PGL_NO_DEPTH_NO_STENCIL
	assert(a->depth_n == b->depth_n);
	assert(memcmp(a->depth, b->depth, a->depth_n) == 0);
#  ifndef PGL_NO_STENCIL
#    ifdef PGL_D16
	assert(a->stencil_n == b->stencil_n);
	assert(memcmp(a->stencil, b->stencil, a->stencil_n) == 0);
#  endif
#  endif
#endif
}

static void snap_free(Snap* s)
{
	free(s->color);
#ifndef PGL_NO_DEPTH_NO_STENCIL
	free(s->depth);
#  ifndef PGL_NO_STENCIL
	free(s->stencil);
#  endif
#endif
}

/* ------------------------------------------------------------------ */
/* Individual scenarios. Each defines seq() (the draws under test) and */
/* runs it without and with an active query.                          */
/* ------------------------------------------------------------------ */

/* Reset pipeline state so scenarios never inherit each other's enables.
 * Every sequence enables exactly the tests it relies on. */
static void reset_state(void)
{
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
#ifndef PGL_NO_DEPTH_NO_STENCIL
	glDisable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glDisable(GL_SCISSOR_TEST);
#  ifndef PGL_NO_STENCIL
	glDisable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, 0, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glStencilMask(0xFF);
#  endif
#endif
}

static void clear_all(void)
{
	reset_state();
	glClearColor(0.05f, 0.05f, 0.1f, 1.f);
#ifndef PGL_NO_DEPTH_NO_STENCIL
	glClearDepth(1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT
#  ifndef PGL_NO_STENCIL
	        | GL_STENCIL_BUFFER_BIT
#  endif
	);
#else
	glClear(GL_COLOR_BUFFER_BIT);
#endif
}

/* 1. Basic front/back: far fullscreen quad then near fullscreen quad. */
static void seq_frontback(void)
{
	GLuint p = make_program(vs_identity, fs_uniform, GL_FALSE);
	glUseProgram(p);
	vec4 far = make_v4(1.f, 0.f, 0.f, 1.f);
	vec4 near = make_v4(0.f, 1.f, 0.f, 1.f);

#ifndef PGL_NO_DEPTH_NO_STENCIL
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
#endif
	pglSetUniform(&far);
	upload_quad(0.5f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	pglSetUniform(&near);
	upload_quad(-0.5f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* 2/3. Color writes fully masked, depth writes off, drawn 3x. Passing
 * samples still count and the framebuffer must not change. */
static void seq_masked(void)
{
	GLuint p = make_program(vs_identity, fs_uniform, GL_FALSE);
	glUseProgram(p);
	vec4 white = make_v4(1.f, 1.f, 1.f, 1.f);
	pglSetUniform(&white);

	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
#ifndef PGL_NO_DEPTH_NO_STENCIL
	glDepthMask(GL_FALSE);
	glEnable(GL_DEPTH_TEST);
#endif
	upload_quad(-0.5f);
	for (int k = 0; k < 3; ++k)
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
#ifndef PGL_NO_DEPTH_NO_STENCIL
	glDepthMask(GL_TRUE);
#endif
}

#ifndef PGL_NO_STENCIL
/* 4. Stencil NEVER rejects every fragment. */
static void seq_stencil_reject(void)
{
	GLuint p = make_program(vs_identity, fs_uniform, GL_FALSE);
	glUseProgram(p);
	vec4 red = make_v4(1.f, 0.f, 0.f, 1.f);
	pglSetUniform(&red);

	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_NEVER, 0, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glStencilMask(0x00);
	upload_quad(-0.5f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisable(GL_STENCIL_TEST);
}
#endif

/* 5. Discard holes: exactly the right half survives. */
static void seq_discard(void)
{
	GLuint p = make_program(vs_identity, fs_discard_hole, GL_TRUE);
	glUseProgram(p);
	upload_quad(-0.5f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

#ifndef PGL_NO_DEPTH_NO_STENCIL
/* 6. gl_FragDepth rewrite: right half forces far depth, fails LESS. */
static void seq_fragdepth(void)
{
	GLuint p = make_program(vs_identity, fs_fragdepth, GL_TRUE);
	glUseProgram(p);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	upload_quad(-0.5f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* 7. Scissor clips a fullscreen quad to a 6x4 box. */
static void seq_scissor(void)
{
	GLuint p = make_program(vs_identity, fs_uniform, GL_FALSE);
	glUseProgram(p);
	vec4 yellow = make_v4(1.f, 1.f, 0.f, 1.f);
	pglSetUniform(&yellow);

	glEnable(GL_SCISSOR_TEST);
	glScissor(2, 3, 6, 4);
	upload_quad(-0.5f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisable(GL_SCISSOR_TEST);
}
#endif

/* 8. Indexed quad (two triangles, 6 indices). */
static void seq_indexed(void)
{
	/* Quad spanning x [-0.5, 0.5], y [-0.5, 0.5] in NDC. */
	float v[] = {
		-0.5f, -0.5f, 0.f,
		 0.5f, -0.5f, 0.f,
		 0.5f,  0.5f, 0.f,
		-0.5f,  0.5f, 0.f,
	};
	GLubyte idx[] = { 0, 1, 2, 0, 2, 3 };
	upload_floats(v, sizeof(v));

	GLuint ebo;
	glGenBuffers(1, &ebo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

	GLuint p = make_program(vs_identity, fs_uniform, GL_FALSE);
	glUseProgram(p);
	vec4 cyan = make_v4(0.f, 1.f, 1.f, 1.f);
	pglSetUniform(&cyan);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, 0);
}

/* 9. Instanced, two non-overlapping small triangles. */
static void seq_instanced(void)
{
	/* Triangle centered at origin; vertex shader shifts per instance. */
	float tri[] = {
		-0.2f, -0.2f, 0.f,
		 0.2f, -0.2f, 0.f,
		 0.0f,  0.2f, 0.f,
	};
	upload_floats(tri, sizeof(tri));
	GLuint p = make_program(vs_instance, fs_uniform, GL_FALSE);
	glUseProgram(p);
	vec4 magenta = make_v4(1.f, 0.f, 1.f, 1.f);
	pglSetUniform(&magenta);
	glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 2);
}

/* 10. Points: with size 1 each surviving vertex is exactly one sample. */
static void seq_points(void)
{
	float pts[] = {
		-0.75f, -0.75f, 0.f,
		 0.00f,  0.00f, 0.f,
		 0.75f,  0.75f, 0.f,
	};
	upload_floats(pts, sizeof(pts));
	GLuint p = make_program(vs_identity, fs_uniform, GL_FALSE);
	glUseProgram(p);
	vec4 white = make_v4(1.f, 1.f, 1.f, 1.f);
	pglSetUniform(&white);
	glPointSize(1.0f);
	glDrawArrays(GL_POINTS, 0, 3);
}

/* 11. Lines: every fragment along one horizontal segment counts once. */
static void seq_lines(void)
{
	float seg[] = {
		-0.75f, 0.0f, 0.f,
		 0.75f, 0.0f, 0.f,
	};
	upload_floats(seg, sizeof(seg));
	GLuint p = make_program(vs_identity, fs_uniform, GL_FALSE);
	glUseProgram(p);
	vec4 white = make_v4(1.f, 1.f, 1.f, 1.f);
	pglSetUniform(&white);
	glLineWidth(1.0f);
	glDrawArrays(GL_LINES, 0, 2);
}

/* Runs seq twice (no query / query) and checks identical framebuffers. */
static GLuint64 run_equivalent(void (*seq)(void))
{
	Snap plain, queried;

	clear_all();
	seq();
	snap_take(&plain);

	clear_all();
	begin_ok();
	seq();
	GLuint64 count = end_ok();
	snap_take(&queried);

	snap_equal(&plain, &queried);
	snap_free(&plain);
	snap_free(&queried);
	return count;
}

/* Count pixels of the default FB that differ from the clear color. */
static int colored_pixels(void)
{
	pix_t clear = pixels[0];
	int n = 0;
	for (int i = 0; i < NPIX; ++i)
		if (pixels[i] != clear)
			++n;
	return n;
}

/* ------------------------------------------------------------------ */
/* Two contexts interleaved                                           */
/* ------------------------------------------------------------------ */

static void test_two_contexts(void)
{
	glContext ctx2;
	pix_t* px2 = NULL;
	assert(init_glContext(&ctx2, &px2, W, H));
	/* init_glContext() makes the new context current; switch back. */
	set_glContext(&the_Context);

	clear_all(); /* context 1 */

	GLuint p = make_program(vs_identity, fs_uniform, GL_FALSE);
	glUseProgram(p);
	vec4 red = make_v4(1.f, 0.f, 0.f, 1.f);
	pglSetUniform(&red);

	/* Begin on context 1, then switch and run a full query on context 2. */
	assert(pglBeginOcclusionQuery() == GL_TRUE);
	upload_quad(-0.5f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); /* half of ctx1's sequence */

	set_glContext(&ctx2);
	{
		glClearColor(0.f, 0.f, 0.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);
		GLuint p2 = make_program(vs_identity, fs_uniform, GL_FALSE);
		glUseProgram(p2);
		vec4 green = make_v4(0.f, 1.f, 0.f, 1.f);
		pglSetUniform(&green);
		GLuint64 c2;
		assert(pglBeginOcclusionQuery() == GL_TRUE);
		upload_quad(-0.5f);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		assert(pglEndOcclusionQuery(&c2) == GL_TRUE);
		assert(c2 == (GLuint64)NPIX);
	}

	/* Back to context 1: its query is still active and holds one quad. */
	set_glContext(&the_Context);
	upload_quad(-0.5f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	GLuint64 c1;
	assert(pglEndOcclusionQuery(&c1) == GL_TRUE);
	assert(c1 == 2 * (GLuint64)NPIX);

	free_glContext(&ctx2);
	set_glContext(&the_Context);
	expect_no_error();
}

/* ------------------------------------------------------------------ */
/* FBO: multiple color attachments and GL_NONE draw buffers           */
/* ------------------------------------------------------------------ */

#ifndef PGL_NO_DEPTH_NO_STENCIL
static GLuint make_u8_color_tex(int w, int h)
{
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	return tex;
}

/* Read one FBO color attachment through glReadPixels (bottom-left origin). */
static void read_attachment(GLenum attachment, int w, int h, unsigned char* out)
{
	glReadBuffer(attachment);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
}
#endif

static void test_fbo_mrt_and_none(void)
{
#ifndef PGL_NO_DEPTH_NO_STENCIL
	const int S = 16;
	const size_t color_bytes = (size_t)S * S * 4;
	GLuint c0 = make_u8_color_tex(S, S);
	GLuint c1 = make_u8_color_tex(S, S);

	GLuint depth;
	glGenTextures(1, &depth);
	glBindTexture(GL_TEXTURE_2D, depth);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	float* dpx = (float*)calloc((size_t)S * S, sizeof(float));
	assert(dpx);
	pglTextureImage2D(depth, 0, GL_DEPTH_COMPONENT, S, S, 0,
	                  GL_DEPTH_COMPONENT, GL_FLOAT, dpx);

	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, c0, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
	                       GL_TEXTURE_2D, c1, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
	                       GL_TEXTURE_2D, depth, 0);

	GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	glDrawBuffers(2, bufs);
	assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	glViewport(0, 0, S, S);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);

	GLuint p = make_program(vs_identity, fs_mrt, GL_FALSE);
	glUseProgram(p);
	upload_quad(-0.5f);

	unsigned char* ref0 = (unsigned char*)malloc(color_bytes);
	unsigned char* ref1 = (unsigned char*)malloc(color_bytes);
	unsigned char* cur0 = (unsigned char*)malloc(color_bytes);
	assert(ref0 && ref1 && cur0);

	/* Reference run without a query. */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	read_attachment(GL_COLOR_ATTACHMENT0, S, S, ref0);
	read_attachment(GL_COLOR_ATTACHMENT1, S, S, ref1);

	/* Same run under a query: identical attachments, exact sample count. */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	GLuint64 n;
	assert(pglBeginOcclusionQuery() == GL_TRUE);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	assert(pglEndOcclusionQuery(&n) == GL_TRUE);
	assert(n == (GLuint64)(S * S)); /* two attachments, one sample each */
	read_attachment(GL_COLOR_ATTACHMENT0, S, S, cur0);
	assert(memcmp(ref0, cur0, color_bytes) == 0);

	/* MRT colors: attachment 0 red-dominant, attachment 1 blue-dominant.
	 * Under an RGB565 window build the FBO U8 path still scales by PGL_RMAX
	 * (31), so a sampled full channel is ~0.12, not 1.0; compare dominant
	 * channels like testing/test_fbo.cpp does. */
	vec4 t0 = texelFetch2D(c0, S / 2, S / 2, 0);
	vec4 t1 = texelFetch2D(c1, S / 2, S / 2, 0);
	assert(t0.x > t0.y && t0.x > t0.z && t0.x > 0.05f);
	assert(t1.z > t1.x && t1.z > t1.y && t1.z > 0.05f);

	/* All draw buffers GL_NONE: samples still pass, color is untouched,
	 * and the float depth attachment is still written. */
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	glClear(GL_DEPTH_BUFFER_BIT);
	GLuint64 none_n;
	assert(pglBeginOcclusionQuery() == GL_TRUE);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	assert(pglEndOcclusionQuery(&none_n) == GL_TRUE);
	assert(none_n == (GLuint64)(S * S));

	/* Re-attach the read buffer and confirm the color image is unchanged. */
	read_attachment(GL_COLOR_ATTACHMENT0, S, S, cur0);
	assert(memcmp(ref0, cur0, color_bytes) == 0);

	/* Near fragments overwrote the cleared 1.0 in the float depth texture. */
	float min_d = 1.0f;
	for (int k = 0; k < S * S; ++k)
		if (dpx[k] < min_d)
			min_d = dpx[k];
	assert(min_d < 0.5f);

	free(ref0);
	free(ref1);
	free(cur0);
	free(dpx);
	glDisable(GL_DEPTH_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, W, H);
	expect_no_error();
#endif
}

/* ------------------------------------------------------------------ */
/* Error semantics                                                    */
/* ------------------------------------------------------------------ */

static void test_errors(void)
{
	/* PGL_UNSAFE compiles out all error checks (and the NULL guard), so the
	 * GL_INVALID_OPERATION contract only exists in checking builds. */
#ifndef PGL_UNSAFE
	GLuint64 out = 777;

	/* The cases below deliberately raise GL_INVALID_OPERATION; quiet the
	 * default debug callback so a passing run stays silent apart from the
	 * final marker. */
	glDisable(GL_DEBUG_OUTPUT);

	/* End without Begin. */
	assert(pglEndOcclusionQuery(&out) == GL_FALSE);
	assert(out == 777); /* caller storage untouched */
	assert(glGetError() == GL_INVALID_OPERATION);

	/* NULL output with no active query. */
	assert(pglEndOcclusionQuery(NULL) == GL_FALSE);
	assert(glGetError() == GL_INVALID_OPERATION);

	/* Nested Begin, then NULL End: active query and its count survive. */
	assert(pglBeginOcclusionQuery() == GL_TRUE);
	assert(pglBeginOcclusionQuery() == GL_FALSE);
	assert(glGetError() == GL_INVALID_OPERATION);
	assert(pglEndOcclusionQuery(NULL) == GL_FALSE);
	assert(glGetError() == GL_INVALID_OPERATION);
	GLuint64 got = 0;
	assert(pglEndOcclusionQuery(&got) == GL_TRUE);
	assert(got == 0); /* no draws happened while it was active */
	expect_no_error();
#endif
}

/* ------------------------------------------------------------------ */
/* Clear and put_pixel are never counted                              */
/* ------------------------------------------------------------------ */

static void test_clear_and_put_pixel_ignored(void)
{
	begin_ok();
	glClearColor(1.f, 1.f, 1.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
#ifndef PGL_NO_DEPTH_NO_STENCIL
	glClearDepth(1.0);
	glClear(GL_DEPTH_BUFFER_BIT);
#endif
	put_pixel(make_Color(10, 20, 30, 255), 0, 0);

	/* pglDrawFrame() bypasses the glDraw* pipeline (no scissor/stencil/depth
	 * test), so its fragments must not be counted either. */
	GLuint p = make_program(vs_identity, fs_uniform, GL_FALSE);
	glUseProgram(p);
	vec4 gray = make_v4(0.25f, 0.25f, 0.25f, 1.f);
	pglSetUniform(&gray);
	pglDrawFrame();

	GLuint64 n = end_ok();
	assert(n == 0);
	expect_no_error();
}

/* ------------------------------------------------------------------ */
/* Usage example (also exercised by the test)                         */
/* ------------------------------------------------------------------ */

/*
 * Example: skip an expensive material pass when the cheap proxy geometry
 * produced no samples that survived the stencil/depth tests.
 *
 *  GLuint64 visible_samples = 0;
 *  if (pglBeginOcclusionQuery()) {
 *      draw_cheap_proxy_geometry();
 *      if (!pglEndOcclusionQuery(&visible_samples)) {
 *          // GL_INVALID_OPERATION: mismatched Begin/End; handle as needed
 *      }
 *  }
 *  if (visible_samples > 0)
 *      draw_expensive_material();
 */
static void usage_example(void)
{
	GLuint64 visible_samples = 0;
	if (pglBeginOcclusionQuery()) {
		GLuint p = make_program(vs_identity, fs_uniform, GL_FALSE);
		glUseProgram(p);
		vec4 gray = make_v4(0.5f, 0.5f, 0.5f, 1.f);
		pglSetUniform(&gray);
		upload_quad(-0.5f);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		if (!pglEndOcclusionQuery(&visible_samples))
			assert(0);
	}
	assert(visible_samples == (GLuint64)NPIX);
}

/* ------------------------------------------------------------------ */
/* PPM coverage map: white where at least one sample passed           */
/* ------------------------------------------------------------------ */

static void write_coverage_ppm(const char* path)
{
	const int S = W; /* full framebuffer, default viewport */
	unsigned char* img = (unsigned char*)calloc((size_t)S * S * 3, 1);
	assert(img);

	clear_all();
	GLuint64 total;
	begin_ok();
	{
		/* Centered box: NDC +/-0.5 maps to pixels [S/4, 3*S/4). */
		GLuint p = make_program(vs_identity, fs_uniform, GL_FALSE);
		glUseProgram(p);
		vec4 white = make_v4(1.f, 1.f, 1.f, 1.f);
		pglSetUniform(&white);
		float x0 = -0.5f, x1 = 0.5f, y0 = -0.5f, y1 = 0.5f;
		float box[] = {
			x0, y0, 0.f,  x1, y0, 0.f,  x0, y1, 0.f,
			x0, y1, 0.f,  x1, y0, 0.f,  x1, y1, 0.f,
		};
		upload_floats(box, sizeof(box));
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}
	total = end_ok();

	pix_t clear = pixels[0];
	int covered = 0;
	/* PPM rows are top-down; the FB pixel array is top-down as well. */
	for (int row = 0; row < S; ++row) {
		for (int col = 0; col < S; ++col) {
			int hit = pixels[row * W + col] != clear;
			covered += hit;
			unsigned char v = (unsigned char)(hit ? 255 : 0);
			img[(row * S + col) * 3 + 0] = v;
			img[(row * S + col) * 3 + 1] = v;
			img[(row * S + col) * 3 + 2] = v;
		}
	}
	/* This box has no overdraw, so the query count equals the covered area. */
	assert((GLuint64)covered == total);
	assert(total == (GLuint64)(S / 2) * (GLuint64)(S / 2));

	FILE* f = fopen(path, "wb");
	assert(f);
	fprintf(f, "P6\n%d %d\n255\n", S, S);
	size_t wr = fwrite(img, 1, (size_t)S * S * 3, f);
	assert(wr == (size_t)S * S * 3);
	fclose(f);
	free(img);
}

/* ------------------------------------------------------------------ */

int main(int argc, char** argv)
{
	pix_t* px = NULL;
	assert(init_glContext(&the_Context, &px, W, H));
	pixels = px;

	GLuint64 n;

	/* 1. Front/back depth: two fullscreen quads both pass. */
	n = run_equivalent(seq_frontback);
	assert(n == 2 * (GLuint64)NPIX);

	/* 2/3. All color channels masked + depth write off, repeated coverage. */
	n = run_equivalent(seq_masked);
	assert(n == 3 * (GLuint64)NPIX);

#ifndef PGL_NO_STENCIL
	/* 4. Stencil rejects everything. */
	n = run_equivalent(seq_stencil_reject);
	assert(n == 0);
#endif

	/* 5. Discard holes: right half only. */
	n = run_equivalent(seq_discard);
	assert(n == (GLuint64)NPIX / 2);

#ifndef PGL_NO_DEPTH_NO_STENCIL
	/* 6. gl_FragDepth rewrite: left half only. */
	n = run_equivalent(seq_fragdepth);
	assert(n == (GLuint64)NPIX / 2);

	/* 7. Scissor 6x4. */
	n = run_equivalent(seq_scissor);
	assert(n == 24);
#endif

	/* 8. Indexed quad. */
	n = run_equivalent(seq_indexed);
	/* Rasterized coverage must match the count (no overdraw here). */
	clear_all();
	seq_indexed();
	assert((GLuint64)colored_pixels() == n);
	assert(n > 0);

	/* 9. Instanced: two instances give twice one instance. */
	clear_all();
	begin_ok();
	{
		float tri[] = {
			-0.2f, -0.2f, 0.f,
			 0.2f, -0.2f, 0.f,
			 0.0f,  0.2f, 0.f,
		};
		upload_floats(tri, sizeof(tri));
		GLuint p = make_program(vs_instance, fs_uniform, GL_FALSE);
		glUseProgram(p);
		vec4 m = make_v4(1.f, 0.f, 1.f, 1.f);
		pglSetUniform(&m);
		glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 1);
	}
	GLuint64 one_inst = end_ok();
	n = run_equivalent(seq_instanced);
	assert(n == 2 * one_inst);
	assert(n > 0);

	/* 10. Points: one size-1 point per surviving vertex, exact sample count. */
	n = run_equivalent(seq_points);
	clear_all();
	seq_points();
	assert((GLuint64)colored_pixels() == n);
	assert(n == 3);

	/* 11. Lines: the query count equals the rasterized 1px segment length. */
	n = run_equivalent(seq_lines);
	clear_all();
	seq_lines();
	assert((GLuint64)colored_pixels() == n);
	assert(n > 0);

	test_two_contexts();
	test_fbo_mrt_and_none();
	test_errors();
	test_clear_and_put_pixel_ignored();
	usage_example();

	const char* ppm_path = (argc > 1) ? argv[1] : "occlusion_query_coverage.ppm";
	write_coverage_ppm(ppm_path);

	free_glContext(&the_Context);
	printf("OCCLUSION_QUERY_OK\n");
	return 0;
}
