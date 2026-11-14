/*
 * Transform feedback target tests.
 *
 * Built by task_support/run.sh target together with task_support/driver.c.
 * The cases are registered in three groups (assembly, session, replay),
 * each with independent expected records that are checked field by field.
 */

#define PORTABLEGL_IMPLEMENTATION
#include <portablegl.h>
#include "task_support/check.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W 48
#define H 48

static int feq(float a, float b)
{
	float d = fabsf(a - b);
	return d <= 1e-6f || d <= 1e-6f * fabsf(a);
}

/* Exact float-compare of a whole record: position (4) + varyings (nv). */
static int record_eq(const float* got, const float* want, int nv)
{
	int i;
	for (i = 0; i < 4 + nv; ++i) {
		if (!feq(got[i], want[i]))
			return 0;
	}
	return 1;
}

static float* buffer_ptr(GLuint buffer)
{
	void* p = NULL;
	pglGetBufferData(buffer, &p);
	return (float*)p;
}

/* ------------------------------------------------------------------ */
/* Shaders                                                            */
/* ------------------------------------------------------------------ */

/* 2 varyings, points test: copies position (arbitrary w) and derives two
 * varying floats straight from the inputs. */
static void vs_point2(float* out, vec4* a, Shader_Builtins* b, void* u)
{
	(void)u;
	b->gl_Position = a[0];
	out[0] = a[0].x + a[0].y;
	out[1] = a[0].z * a[0].w;
}

/* 1 varying: a single copied component, keeps exact float equality. */
static void vs_one(float* out, vec4* a, Shader_Builtins* b, void* u)
{
	(void)u;
	b->gl_Position = a[0];
	out[0] = a[0].w;
}

/* 4 varyings: per-instance color (divisor 1) plus gl_InstanceID. */
static void vs_inst(float* out, vec4* a, Shader_Builtins* b, void* u)
{
	(void)u;
	b->gl_Position = a[0];
	out[0] = a[1].x;
	out[1] = a[1].y;
	out[2] = a[1].z;
	out[3] = (float)b->gl_InstanceID;
}

/* Deforming VS used for the replay comparison. Purely positional deform so
 * the captured records can be replayed by a pass-through VS. */
static void vs_deform(float* out, vec4* a, Shader_Builtins* b, void* u)
{
	(void)u;
	vec4 p = a[0];
	p.x += 0.25f * p.y;
	p.z *= 0.5f;
	b->gl_Position = p;
	out[0] = a[1].x;
	out[1] = a[1].y;
	out[2] = a[1].z;
}

/* Pass-through replay VS: record layout is pos(4) then color(3). */
static void vs_passthrough(float* out, vec4* a, Shader_Builtins* b, void* u)
{
	(void)u;
	b->gl_Position = a[0];
	out[0] = a[1].x;
	out[1] = a[1].y;
	out[2] = a[1].z;
}

static void fs_color(float* in, Shader_Builtins* b, void* u)
{
	(void)u;
	b->gl_FragColor = make_v4(in[0], in[1], in[2], 1.0f);
}

/* Distinct 2-varying program for the pause/program-switch case. */
static void vs_bprog(float* out, vec4* a, Shader_Builtins* b, void* u)
{
	(void)u;
	b->gl_Position = a[0];
	out[0] = -a[0].x;
	out[1] = -a[0].y;
}

static int vs_calls;
static void vs_count(float* out, vec4* a, Shader_Builtins* b, void* u)
{
	++vs_calls;
	vs_one(out, a, b, u);
}

static int fs_calls;
static void fs_count(float* in, Shader_Builtins* b, void* u)
{
	(void)in; (void)u;
	++fs_calls;
	b->gl_FragColor = make_v4(1, 0, 0, 1);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static GLuint make_buffer(GLsizeiptr bytes, const void* data)
{
	GLuint buf;
	glGenBuffers(1, &buf);
	glBindBuffer(GL_ARRAY_BUFFER, buf);
	glBufferData(GL_ARRAY_BUFFER, bytes, data, GL_STATIC_DRAW);
	return buf;
}

/* Two overlapping-ish triangles with depth and per-vertex colors. */
static const float scene_verts[] = {
	/* pos (x y z w)          color rgb */
	-0.8f, -0.8f,  0.20f, 1.0f,  1.0f, 0.2f, 0.2f,
	 0.8f, -0.8f,  0.20f, 1.0f,  1.0f, 0.2f, 0.2f,
	 0.0f,  0.9f,  0.20f, 1.0f,  1.0f, 0.2f, 0.2f,

	-0.6f, -0.3f, -0.10f, 1.0f,  0.2f, 1.0f, 0.3f,
	 0.9f,  0.2f, -0.10f, 1.0f,  0.2f, 1.0f, 0.3f,
	-0.2f,  0.95f,-0.10f, 1.0f,  0.2f, 1.0f, 0.3f,
};

static void bind_scene_attribs(void)
{
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 7 * (int)sizeof(float), 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 7 * (int)sizeof(float),
	                      (void*)(4 * sizeof(float)));
}

static void clear_frame(void)
{
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* ================================================================== */
/* assembly group                                                     */
/* ================================================================== */

/* Non-equal w plus an indexed reorder, point topology. */
static int asm_points_w(void)
{
	glContext ctx;
	pix_t* px = NULL;
	TASK_CHECK(init_glContext(&ctx, &px, W, H));
	set_glContext(&ctx);

	GLenum interp2[2] = { PGL_SMOOTH, PGL_SMOOTH };
	GLuint prog = pglCreateProgram(vs_point2, fs_count, 2, interp2, GL_FALSE);
	glUseProgram(prog);

	float verts[] = {
		 0.1f,  0.2f,  0.3f, 1.0f,
		 0.4f,  0.5f,  0.6f, 2.0f,
		-0.1f, -0.2f, -0.3f, 0.5f,
		 0.7f,  0.8f,  0.9f, 4.0f,
	};
	GLubyte idx[] = { 3, 0, 2, 1 };
	make_buffer(sizeof verts, verts);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);
	GLuint ebo;
	glGenBuffers(1, &ebo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);

	const int rf = 6; /* 4 pos + 2 varyings */
	GLuint fb = make_buffer(4 * rf * (int)sizeof(float), NULL);

	TASK_CHECK(glGetError() == GL_NO_ERROR);
	pglBeginFeedback(fb, 0, 4 * rf * (GLsizeiptr)sizeof(float), GL_POINTS);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	glDrawElements(GL_POINTS, 4, GL_UNSIGNED_BYTE, 0);
	pglEndFeedback();

	TASK_CHECK(pglFeedbackVertexCount() == 4);
	TASK_CHECK(pglFeedbackOverflowed() == GL_FALSE);

	float* got = buffer_ptr(fb);
	static const GLubyte order[4] = { 3, 0, 2, 1 };
	int i;
	for (i = 0; i < 4; ++i) {
		const float* s = &verts[order[i] * 4];
		float want[6];
		want[0] = s[0]; want[1] = s[1]; want[2] = s[2]; want[3] = s[3];
		want[4] = s[0] + s[1];
		want[5] = s[2] * s[3];
		TASK_CHECK(record_eq(got + (size_t)i * rf, want, 2));
	}
	/* w must survive untouched, including the 0.5 and 4.0 non-unit values. */
	TASK_CHECK(got[0 * rf + 3] == 4.0f && got[2 * rf + 3] == 0.5f);

	free_glContext(&ctx);
	return 0;
}

/* Triangle strip parity winding, 5 verts -> 3 triangles. */
static int asm_tri_strip(void)
{
	glContext ctx;
	pix_t* px = NULL;
	TASK_CHECK(init_glContext(&ctx, &px, W, H));
	set_glContext(&ctx);

	GLenum interp1[1] = { PGL_SMOOTH };
	GLuint prog = pglCreateProgram(vs_one, fs_count, 1, interp1, GL_FALSE);
	glUseProgram(prog);

	float verts[5][4] = {
		{ -0.9f, -0.9f, 0.1f, 1.0f },
		{  0.9f, -0.9f, 0.2f, 2.0f },
		{ -0.5f,  0.5f, 0.3f, 3.0f },
		{  0.5f,  0.5f, 0.4f, 4.0f },
		{  0.0f,  0.9f, 0.5f, 5.0f },
	};
	make_buffer(sizeof verts, verts);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);

	const int rf = 5;
	GLuint fb = make_buffer(9 * rf * (int)sizeof(float), NULL);
	pglBeginFeedback(fb, 0, 9 * rf * (GLsizeiptr)sizeof(float), GL_TRIANGLES);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 5);
	pglEndFeedback();

	TASK_CHECK(pglFeedbackVertexCount() == 9);
	TASK_CHECK(pglFeedbackOverflowed() == GL_FALSE);

	/* Strip expansion with odd-parity reversal of the middle triangle. */
	static const int want_slots[9] = { 0, 1, 2,  2, 1, 3,  2, 3, 4 };
	float* got = buffer_ptr(fb);
	int i;
	for (i = 0; i < 9; ++i) {
		float want[5];
		const float* s = verts[want_slots[i]];
		want[0] = s[0]; want[1] = s[1]; want[2] = s[2]; want[3] = s[3];
		want[4] = s[3];
		TASK_CHECK(record_eq(got + (size_t)i * rf, want, 1));
	}
	/* Explicit parity check: records 3..5 are vertices 2,1,3 in that order. */
	TASK_CHECK(got[3 * rf + 0] == verts[2][0] && got[4 * rf + 0] == verts[1][0] &&
	           got[5 * rf + 0] == verts[3][0]);

	free_glContext(&ctx);
	return 0;
}

/* Line loop closing segment plus an incomplete tail on a GL_LINES draw. */
static int asm_line_loop_tail(void)
{
	glContext ctx;
	pix_t* px = NULL;
	TASK_CHECK(init_glContext(&ctx, &px, W, H));
	set_glContext(&ctx);

	GLenum interp1[1] = { PGL_SMOOTH };
	GLuint prog = pglCreateProgram(vs_one, fs_count, 1, interp1, GL_FALSE);
	glUseProgram(prog);

	float verts[4][4] = {
		{ -0.8f,  0.0f, 0.0f, 1.0f },
		{  0.0f, -0.8f, 0.0f, 2.0f },
		{  0.8f,  0.0f, 0.0f, 3.0f },
		{  0.0f,  0.8f, 0.0f, 4.0f },
	};
	make_buffer(sizeof verts, verts);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);

	const int rf = 5;
	GLuint fb = make_buffer(32 * rf * (int)sizeof(float), NULL);
	pglBeginFeedback(fb, 0, 32 * rf * (GLsizeiptr)sizeof(float), GL_LINES);

	glDrawArrays(GL_LINE_LOOP, 0, 4);                 /* 4 segments, 8 verts */
	TASK_CHECK(pglFeedbackVertexCount() == 8);
	glDrawArrays(GL_LINES, 0, 3);                     /* 1 segment + dropped tail */
	TASK_CHECK(pglFeedbackVertexCount() == 10);

	/* MultiDrawArrays: two sub-draws appended independently, tails dropped. */
	GLint md_first[2] = { 0, 1 };
	GLsizei md_count[2] = { 3, 2 };
	glMultiDrawArrays(GL_LINES, md_first, md_count, 2); /* (0..2) + (1..2) */
	TASK_CHECK(pglFeedbackVertexCount() == 14);

	/* MultiDrawElements through an index buffer: repeated indices preserved. */
	GLushort el[4] = { 2, 2, 0, 1 };
	GLuint ebo;
	glGenBuffers(1, &ebo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof el, el, GL_STATIC_DRAW);
	GLsizei me_count[2] = { 2, 2 };
	const void* me_off[2] = { 0, (void*)(2 * (GLintptr)sizeof(GLushort)) };
	glMultiDrawElements(GL_LINES, me_count, GL_UNSIGNED_SHORT, me_off, 2);
	TASK_CHECK(pglFeedbackVertexCount() == 18);
	pglEndFeedback();

	TASK_CHECK(pglFeedbackOverflowed() == GL_FALSE);

	float* got = buffer_ptr(fb);
	/* Closing line-loop segment is the last loop pair: verts 3 then 0. */
	TASK_CHECK(got[6 * rf + 0] == verts[3][0] && got[7 * rf + 0] == verts[0][0]);
	/* GL_LINES tail: only verts 0,1 captured, vertex 2 produced no record. */
	TASK_CHECK(got[8 * rf + 0] == verts[0][0] && got[9 * rf + 0] == verts[1][0]);
	/* Varying (w copy) rides along on the closing segment. */
	TASK_CHECK(got[6 * rf + 4] == 4.0f && got[7 * rf + 4] == 1.0f);
	/* MultiDrawArrays sub-draws: (0,1) then (1,2). */
	TASK_CHECK(got[10 * rf + 0] == verts[0][0] && got[11 * rf + 0] == verts[1][0]);
	TASK_CHECK(got[12 * rf + 0] == verts[1][0] && got[13 * rf + 0] == verts[2][0]);
	/* MultiDrawElements: repeated index 2 is captured twice. */
	TASK_CHECK(got[14 * rf + 0] == verts[2][0] && got[15 * rf + 0] == verts[2][0]);
	TASK_CHECK(got[16 * rf + 0] == verts[0][0] && got[17 * rf + 0] == verts[1][0]);

	free_glContext(&ctx);
	return 0;
}

/* ================================================================== */
/* session group                                                      */
/* ================================================================== */

/* Instanced attributes, off-frustum primitive, incomplete tail and
 * appending across separate draws into a non-zero-offset range. */
static int sess_instances_append(void)
{
	glContext ctx;
	pix_t* px = NULL;
	TASK_CHECK(init_glContext(&ctx, &px, W, H));
	set_glContext(&ctx);

	GLenum interp4[4] = { PGL_SMOOTH4 };
	GLuint prog = pglCreateProgram(vs_inst, fs_count, 4, interp4, GL_FALSE);
	glUseProgram(prog);

	/* 6 vertices: first triangle on screen, second fully off-frustum. */
	float positions[] = {
		-0.5f, -0.5f, 0.0f, 1.0f,
		 0.5f, -0.5f, 0.0f, 1.0f,
		 0.0f,  0.5f, 0.0f, 1.0f,
		 5.0f,  5.0f, 0.0f, 1.0f,
		 6.0f,  5.0f, 0.0f, 1.0f,
		 5.0f,  6.0f, 0.0f, 1.0f,
	};
	float colors[] = {
		1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
		1.0f, 1.0f, 0.0f,
		0.0f, 1.0f, 1.0f,
		1.0f, 0.0f, 1.0f,
	};
	make_buffer(sizeof positions, positions);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);
	GLuint cbo;
	glGenBuffers(1, &cbo);
	glBindBuffer(GL_ARRAY_BUFFER, cbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof colors, colors, GL_STATIC_DRAW);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, 0);
	glVertexAttribDivisor(1, 1);

	const int rf = 8; /* pos 4 + color 3 + instance id */
	const GLintptr off = 16; /* non-zero, 4-aligned */
	GLuint fb = make_buffer(off + 32 * rf * (int)sizeof(float), NULL);
	memset(buffer_ptr(fb), 0, (size_t)off + 32 * rf * (int)sizeof(float));

	pglBeginFeedback(fb, off, 32 * rf * (GLsizeiptr)sizeof(float), GL_TRIANGLES);
	/* 2 instances x 6 verts = 12 records. */
	glDrawArraysInstanced(GL_TRIANGLES, 0, 6, 2);
	TASK_CHECK(pglFeedbackVertexCount() == 12);
	/* 4 verts -> one triangle plus a dropped one-vertex tail. */
	glDrawArrays(GL_TRIANGLES, 0, 4);
	pglEndFeedback();

	TASK_CHECK(pglFeedbackVertexCount() == 15);
	TASK_CHECK(pglFeedbackOverflowed() == GL_FALSE);

	float* got = (float*)((char*)buffer_ptr(fb) + off);

	/* Off-frustum triangle is still captured (instance 0, slots 3..5). */
	TASK_CHECK(got[3 * rf + 0] == 5.0f && got[4 * rf + 0] == 6.0f);
	/* Instanced color: instance 0 gets color row 0 (red), instance 1 gets
	 * row 1 (green) because the color attribute has divisor 1. */
	TASK_CHECK(got[0 * rf + 4] == 1.0f && got[0 * rf + 5] == 0.0f);
	TASK_CHECK(got[6 * rf + 4] == 0.0f && got[6 * rf + 5] == 1.0f);
	TASK_CHECK(got[0 * rf + 7] == 0.0f && got[6 * rf + 7] == 1.0f);
	/* Second instance repeats the same six assembled vertices. */
	TASK_CHECK(got[6 * rf + 0] == got[0 * rf + 0] && got[11 * rf + 0] == got[5 * rf + 0]);
	/* Appended single-triangle draw continues right at the old cursor. */
	TASK_CHECK(got[12 * rf + 0] == positions[0] && got[14 * rf + 0] == positions[8]);
	/* Bytes before the range were never touched. */
	TASK_CHECK(((unsigned char*)buffer_ptr(fb))[0] == 0 &&
	           ((unsigned char*)buffer_ptr(fb))[15] == 0);

	free_glContext(&ctx);
	return 0;
}

/* Pause, switch programs while paused, resume rules, cursor continuation. */
static int sess_pause_program(void)
{
	glContext ctx;
	pix_t* px = NULL;
	TASK_CHECK(init_glContext(&ctx, &px, W, H));
	set_glContext(&ctx);

	GLenum interp1[1] = { PGL_SMOOTH };
	GLenum interp2[2] = { PGL_SMOOTH, PGL_SMOOTH };
	GLuint pa = pglCreateProgram(vs_one, fs_count, 1, interp1, GL_FALSE);
	GLuint pb = pglCreateProgram(vs_bprog, fs_count, 2, interp2, GL_FALSE);
	glUseProgram(pa);

	float tri[3][4] = {
		{ -0.5f, -0.5f, 0, 1 }, { 0.5f, -0.5f, 0, 1 }, { 0, 0.5f, 0, 1 }
	};
	make_buffer(sizeof tri, tri);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);

	const int rf = 5;
	GLsizeiptr bytes = 12 * rf * (GLsizeiptr)sizeof(float);
	GLuint fb = make_buffer(bytes, NULL);
	memset(buffer_ptr(fb), 0xCD, (size_t)bytes);

	pglBeginFeedback(fb, 0, bytes, GL_TRIANGLES);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	TASK_CHECK(pglFeedbackVertexCount() == 3);

	pglPauseFeedback();
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	/* Double pause is an ordering error, state unchanged. */
	pglPauseFeedback();
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);

	/* Program switch is allowed while paused. */
	glUseProgram(pb);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	glDrawArrays(GL_TRIANGLES, 0, 3); /* ordinary draw, not captured */
	TASK_CHECK(pglFeedbackVertexCount() == 3);

	/* Resume with the wrong program is rejected and stays paused. */
	pglResumeFeedback();
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	TASK_CHECK(pglFeedbackVertexCount() == 3);

	glUseProgram(pa);
	pglResumeFeedback();
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	TASK_CHECK(pglFeedbackVertexCount() == 6);
	pglEndFeedback();

	/* Paused B-program draws never wrote records: record 3 onwards still
	 * holds the sentinel, while the second A draw filled records 3..5. */
	unsigned char* raw = (unsigned char*)buffer_ptr(fb);
	unsigned char sentinel = 0xCD;
	TASK_CHECK(raw[6 * rf * (int)sizeof(float)] == sentinel);
	TASK_CHECK(raw[3 * rf * (int)sizeof(float)] != sentinel);

	/* Ordering errors once the session has ended. */
	pglEndFeedback();
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	pglResumeFeedback();
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);

	free_glContext(&ctx);
	return 0;
}

/* Pre-shader rejection and Begin validation; state survives every error. */
static int sess_errors(void)
{
	glContext ctx;
	pix_t* px = NULL;
	TASK_CHECK(init_glContext(&ctx, &px, W, H));
	set_glContext(&ctx);

	GLenum interp1[1] = { PGL_SMOOTH };
	GLuint prog = pglCreateProgram(vs_count, fs_count, 1, interp1, GL_FALSE);
	glUseProgram(prog);

	float tri[3][4] = {
		{ -0.5f, -0.5f, 0, 1 }, { 0.5f, -0.5f, 0, 1 }, { 0, 0.5f, 0, 1 }
	};
	GLuint vbo = make_buffer(sizeof tri, tri);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);

	GLuint fb = make_buffer(64, NULL);

	/* Enum errors. */
	pglBeginFeedback(fb, 0, 64, GL_TRIANGLE_STRIP);
	TASK_CHECK(glGetError() == GL_INVALID_ENUM);
	pglBeginFeedback(fb, 0, 64, GL_LINE_STRIP);
	TASK_CHECK(glGetError() == GL_INVALID_ENUM);
	/* Value errors: negative / unaligned / out-of-range. */
	pglBeginFeedback(fb, -4, 64, GL_TRIANGLES);
	TASK_CHECK(glGetError() == GL_INVALID_VALUE);
	pglBeginFeedback(fb, 1, 60, GL_TRIANGLES);
	TASK_CHECK(glGetError() == GL_INVALID_VALUE);
	pglBeginFeedback(fb, 0, 68, GL_TRIANGLES);
	TASK_CHECK(glGetError() == GL_INVALID_VALUE);
	pglBeginFeedback(fb, 64, 4, GL_TRIANGLES);
	TASK_CHECK(glGetError() == GL_INVALID_VALUE);
	/* Invalid / deleted buffer names. */
	pglBeginFeedback(0, 0, 0, GL_TRIANGLES);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	GLuint gone;
	glGenBuffers(1, &gone);
	glBindBuffer(GL_ARRAY_BUFFER, gone); /* assign a valid type before delete */
	glDeleteBuffers(1, &gone);
	pglBeginFeedback(gone, 0, 0, GL_TRIANGLES);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	/* Empty range on a valid buffer is legal. */
	pglBeginFeedback(fb, 0, 0, GL_TRIANGLES);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	/* A second Begin while active is rejected. */
	pglBeginFeedback(fb, 0, 64, GL_TRIANGLES);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);

	/* Family mismatch rejected before any vertex shader callback. */
	vs_calls = 0;
	glDrawArrays(GL_LINES, 0, 2);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	TASK_CHECK(vs_calls == 0);
	TASK_CHECK(pglFeedbackVertexCount() == 0);

	/* Source attribute aliasing the target buffer: rejected before VS. */
	glBindBuffer(GL_ARRAY_BUFFER, fb);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	TASK_CHECK(vs_calls == 0);

	/* Element buffer aliasing the target: restore the attribute source,
	 * then bind the target as the index buffer and reject before VS. */
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, fb);
	glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_BYTE, 0);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	TASK_CHECK(vs_calls == 0);

	/* Session-locked objects and layout. */
	GLuint ebo;
	glGenBuffers(1, &ebo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, 8, tri, GL_STATIC_DRAW);
	glDeleteBuffers(1, &fb);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	glDeleteProgram(prog);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	pglSetInterp(0, interp1);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	float patch = 1.0f;
	glNamedBufferSubData(fb, 0, sizeof patch, &patch);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);
	/* Switching program while actively recording is rejected. */
	GLuint other = pglCreateProgram(vs_count, fs_count, 1, interp1, GL_FALSE);
	glUseProgram(other);
	TASK_CHECK(glGetError() == GL_INVALID_OPERATION);

	pglEndFeedback();
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	/* After End the same operations are legal again. */
	glUseProgram(other);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	glDeleteProgram(prog);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	glNamedBufferSubData(fb, 0, sizeof patch, &patch);
	TASK_CHECK(glGetError() == GL_NO_ERROR);

	/* The failed Begins left no session behind and a fresh Begin now resets
	 * count/overflow and works. */
	pglBeginFeedback(fb, 0, 64, GL_TRIANGLES);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	TASK_CHECK(pglFeedbackVertexCount() == 0 && pglFeedbackOverflowed() == GL_FALSE);
	pglEndFeedback();

	free_glContext(&ctx);
	return 0;
}

/* ================================================================== */
/* replay group                                                       */
/* ================================================================== */

/* Replay captured records through a pass-through shader in the same context
 * and compare color and depth buffers pixel-for-pixel against snapshots of
 * the original deformed draw. Same-context redraw is deterministic under
 * every depth format; per-context state isolation is covered by the
 * two-context overflow case. */
static int replay_pixels_depth(void)
{
	glContext ctx;
	pix_t* px = NULL;
	TASK_CHECK(init_glContext(&ctx, &px, W, H));
	set_glContext(&ctx);

#if defined(PGL_NO_DEPTH_NO_STENCIL)
	const size_t zbytes = 0;
#elif defined(PGL_D16)
	const size_t zbytes = sizeof(u16);
#else
	const size_t zbytes = sizeof(u32);
#endif
	const size_t npix = (size_t)W * H;

	GLenum interp3[3] = { PGL_SMOOTH3 };
	GLuint deform = pglCreateProgram(vs_deform, fs_color, 3, interp3, GL_FALSE);
	glUseProgram(deform);
	make_buffer(sizeof scene_verts, scene_verts);
	bind_scene_attribs();

	const int rf = 7;
	GLuint fb = make_buffer(24 * rf * (int)sizeof(float), NULL);
	if (zbytes)
		glEnable(GL_DEPTH_TEST);
	clear_frame();
	pglBeginFeedback(fb, 0, 24 * rf * (GLsizeiptr)sizeof(float), GL_TRIANGLES);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	pglEndFeedback();
	GLuint n = pglFeedbackVertexCount();
	TASK_CHECK(n == 6);
	TASK_CHECK(pglFeedbackOverflowed() == GL_FALSE);

	/* Snapshot the deformed frame before replaying over it. */
	pix_t* color_snap = (pix_t*)malloc(npix * sizeof(pix_t));
	TASK_CHECK(color_snap != NULL);
	memcpy(color_snap, px, npix * sizeof(pix_t));
	void* depth_snap = NULL;
	if (zbytes) {
		depth_snap = malloc(npix * zbytes);
		TASK_CHECK(depth_snap != NULL);
		memcpy(depth_snap, pglGetDepthBuffer(), npix * zbytes);
	}

	/* Replay: bind the feedback buffer as ordinary vertex attributes. */
	GLuint replay = pglCreateProgram(vs_passthrough, fs_color, 3, interp3, GL_FALSE);
	glUseProgram(replay);
	glBindBuffer(GL_ARRAY_BUFFER, fb);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, rf * (int)sizeof(float), 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, rf * (int)sizeof(float),
	                      (void*)(4 * sizeof(float)));
	clear_frame();
	glDrawArrays(GL_TRIANGLES, 0, (GLsizei)n);

	int colored = 0, mismatches = 0, i;
	for (i = 0; i < W * H; ++i) {
		if (px[i] != color_snap[i])
			++mismatches;
		if (color_snap[i] != 0)
			++colored;
	}
	TASK_CHECK(colored > 0);
	TASK_CHECK(mismatches == 0);

	if (zbytes) {
		TASK_CHECK(memcmp(depth_snap, pglGetDepthBuffer(), npix * zbytes) == 0);
		free(depth_snap);
	}
	free(color_snap);

	free_glContext(&ctx);
	return 0;
}

/* Rasterizer discard skips fragments only; VS and feedback still run, and
 * pause/end never touch the switch. */
static int replay_discard(void)
{
	glContext ctx;
	pix_t* px = NULL;
	TASK_CHECK(init_glContext(&ctx, &px, W, H));
	set_glContext(&ctx);

	GLenum interp1[1] = { PGL_SMOOTH };
	GLuint prog = pglCreateProgram(vs_count, fs_count, 1, interp1, GL_FALSE);
	glUseProgram(prog);
	float tri[3][4] = {
		{ -0.5f, -0.5f, 0, 1 }, { 0.5f, -0.5f, 0, 1 }, { 0, 0.5f, 0, 1 }
	};
	make_buffer(sizeof tri, tri);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);

	const int rf = 5;
	GLuint fb = make_buffer(9 * rf * (int)sizeof(float), NULL);

	pglRasterizerDiscard(GL_TRUE);
	clear_frame();
	pix_t cleared = px[0];
	vs_calls = 0;
	fs_calls = 0;
	pglBeginFeedback(fb, 0, 9 * rf * (GLsizeiptr)sizeof(float), GL_TRIANGLES);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	TASK_CHECK(vs_calls == 3);                 /* vertex shading still runs */
	TASK_CHECK(fs_calls == 0);                 /* no fragment callbacks */
	TASK_CHECK(pglFeedbackVertexCount() == 3); /* feedback still captured */
	int unchanged = 1, i;
	for (i = 0; i < W * H; ++i)
		if (px[i] != cleared) unchanged = 0;
	TASK_CHECK(unchanged);

	pglPauseFeedback();
	fs_calls = 0;
	glDrawArrays(GL_TRIANGLES, 0, 3);         /* paused: still discarded */
	TASK_CHECK(fs_calls == 0);
	pglResumeFeedback();
	pglEndFeedback();
	fs_calls = 0;
	glDrawArrays(GL_TRIANGLES, 0, 3);         /* End does not clear the switch */
	TASK_CHECK(fs_calls == 0);
	TASK_CHECK(pglFeedbackVertexCount() == 3);

	pglRasterizerDiscard(GL_FALSE);
	fs_calls = 0;
	glDrawArrays(GL_TRIANGLES, 0, 3);
	TASK_CHECK(fs_calls > 0);

	free_glContext(&ctx);
	return 0;
}

/* Overflow sentinel semantics plus independent per-context state. */
static int replay_overflow_two_contexts(void)
{
	glContext c1;
	pix_t* p1 = NULL;
	TASK_CHECK(init_glContext(&c1, &p1, W, H));
	set_glContext(&c1);

	GLenum interp1[1] = { PGL_SMOOTH };
	GLuint prog1 = pglCreateProgram(vs_count, fs_count, 1, interp1, GL_FALSE);
	glUseProgram(prog1);
	float six[6][4] = {
		{ -0.9f, -0.9f, 0, 1 }, { 0.9f, -0.9f, 0, 1 }, { 0, 0.9f, 0, 1 },
		{ -0.9f,  0.9f, 0, 1 }, { 0.9f,  0.9f, 0, 1 }, { 0, -0.2f, 0, 1 }
	};
	make_buffer(sizeof six, six);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);

	const int rf = 5;
	GLsizeiptr bytes = 12 * rf * (GLsizeiptr)sizeof(float);
	GLuint fb = make_buffer(bytes, NULL);
	memset(buffer_ptr(fb), 0xAB, (size_t)bytes);

	/* Room for exactly one triangle (3 records). */
	pglBeginFeedback(fb, 0, 3 * rf * (GLsizeiptr)sizeof(float), GL_TRIANGLES);
	fs_calls = 0;
	glDrawArrays(GL_TRIANGLES, 0, 6); /* tri 1 fits, tri 2 overflows */
	TASK_CHECK(pglFeedbackOverflowed() == GL_TRUE);
	TASK_CHECK(pglFeedbackVertexCount() == 3);
	TASK_CHECK(fs_calls > 0); /* rasterization kept going */

	/* Once overflowed the session never appends again. */
	GLuint before = pglFeedbackVertexCount();
	glDrawArrays(GL_TRIANGLES, 0, 3);
	TASK_CHECK(pglFeedbackVertexCount() == before);
	pglEndFeedback();

	/* Complete primitive retained; everything past the cursor is untouched. */
	unsigned char* raw = (unsigned char*)buffer_ptr(fb);
	TASK_CHECK(raw[3 * rf * (int)sizeof(float)] == 0xAB);
	TASK_CHECK(raw[(GLsizeiptr)bytes - 1] == 0xAB);
	/* Count/overflow remain queryable after End. */
	TASK_CHECK(pglFeedbackVertexCount() == 3);
	TASK_CHECK(pglFeedbackOverflowed() == GL_TRUE);

	/* Second, fully independent context. */
	glContext c2;
	pix_t* p2 = NULL;
	TASK_CHECK(init_glContext(&c2, &p2, W, H));
	set_glContext(&c2);
	TASK_CHECK(pglFeedbackVertexCount() == 0);
	TASK_CHECK(pglFeedbackOverflowed() == GL_FALSE);

	GLuint prog2 = pglCreateProgram(vs_count, fs_count, 1, interp1, GL_FALSE);
	glUseProgram(prog2);
	make_buffer(sizeof six, six);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);
	GLuint fb2 = make_buffer(6 * rf * (int)sizeof(float), NULL);
	pglBeginFeedback(fb2, 0, 6 * rf * (GLsizeiptr)sizeof(float), GL_TRIANGLES);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	pglEndFeedback();
	TASK_CHECK(pglFeedbackVertexCount() == 3);
	TASK_CHECK(pglFeedbackOverflowed() == GL_FALSE);

	/* c1 state is untouched by c2's session. */
	set_glContext(&c1);
	TASK_CHECK(pglFeedbackVertexCount() == 3);
	TASK_CHECK(pglFeedbackOverflowed() == GL_TRUE);
	/* A successful Begin is the only thing that clears count/overflow. */
	pglBeginFeedback(fb, 0, bytes, GL_TRIANGLES);
	TASK_CHECK(pglFeedbackVertexCount() == 0);
	TASK_CHECK(pglFeedbackOverflowed() == GL_FALSE);
	pglEndFeedback();

	free_glContext(&c2);
	free_glContext(&c1);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	{ "assembly", "points_indexed_nonunit_w", asm_points_w },
	{ "assembly", "triangle_strip_parity",    asm_tri_strip },
	{ "assembly", "line_loop_closing_tail",   asm_line_loop_tail },

	{ "session", "instances_frustum_append",  sess_instances_append },
	{ "session", "pause_switch_program",      sess_pause_program },
	{ "session", "errors_pre_reject_state",   sess_errors },

	{ "replay", "pixel_depth_replay",         replay_pixels_depth },
	{ "replay", "rasterizer_discard",         replay_discard },
	{ "replay", "overflow_sentinel_two_ctx",  replay_overflow_two_contexts },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
