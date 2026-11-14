/*
 * feedback_replay.c
 *
 * Headless transform-feedback example, no window or GPU.
 *
 * A deforming vertex shader runs once while a feedback session captures the
 * pre-clip gl_Position and every varying into an ordinary buffer. The buffer
 * is then bound back through the normal vertex attribute APIs and replayed
 * by a pass-through shader, so the deformation is never recomputed. A short
 * rasterizer-discard section shows feedback running with zero fragments.
 *
 * Build from the repository root:
 *
 *     cc -std=c99 -O1 -I. examples/original/feedback_replay.c -lm -o feedback_replay
 *     ./feedback_replay
 */

#define PGL_EXCLUDE_STUBS
#define PORTABLEGL_IMPLEMENTATION
#include "portablegl.h"

#include <stdio.h>

#define W 64
#define H 64

/* The expensive step we only want to run once. */
static void deform_vs(float* out, vec4* a, Shader_Builtins* b, void* u)
{
	(void)u;
	vec4 p = a[0];
	p.x += 0.30f * p.y;
	p.z *= 0.5f;
	b->gl_Position = p;
	out[0] = a[1].x;
	out[1] = a[1].y;
	out[2] = a[1].z;
}

/* Replay shader: the record is already the final position + color. */
static void passthrough_vs(float* out, vec4* a, Shader_Builtins* b, void* u)
{
	(void)u;
	b->gl_Position = a[0];
	out[0] = a[1].x;
	out[1] = a[1].y;
	out[2] = a[1].z;
}

static void color_fs(float* in, Shader_Builtins* b, void* u)
{
	(void)u;
	b->gl_FragColor = make_v4(in[0], in[1], in[2], 1.0f);
}

static int frag_count;
static void counting_fs(float* in, Shader_Builtins* b, void* u)
{
	color_fs(in, b, u);
	++frag_count;
}

static int colored_pixels(const pix_t* px)
{
	int n = 0, i;
	for (i = 0; i < W * H; ++i)
		if (px[i] != 0)
			++n;
	return n;
}

int main(void)
{
	glContext ctx;
	pix_t* px = NULL;
	if (!init_glContext(&ctx, &px, W, H)) {
		puts("init_glContext failed");
		return 1;
	}
	set_glContext(&ctx);

	GLenum interp[3] = { PGL_SMOOTH3 };

	/* Two triangles, interleaved position (vec4) + color (vec3). */
	const float scene[] = {
		-0.8f, -0.8f,  0.2f, 1.0f,  1.0f, 0.2f, 0.2f,
		 0.8f, -0.8f,  0.2f, 1.0f,  1.0f, 0.2f, 0.2f,
		 0.0f,  0.9f,  0.2f, 1.0f,  1.0f, 0.2f, 0.2f,

		-0.6f, -0.3f, -0.1f, 1.0f,  0.2f, 1.0f, 0.3f,
		 0.9f,  0.2f, -0.1f, 1.0f,  0.2f, 1.0f, 0.3f,
		-0.2f,  0.95f,-0.1f, 1.0f,  0.2f, 1.0f, 0.3f,
	};
	const int nverts = 6;
	const int record_floats = 4 + 3; /* gl_Position then the three varyings */

	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof scene, scene, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 7 * (int)sizeof(float), 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 7 * (int)sizeof(float),
	                      (void*)(4 * sizeof(float)));

	/* Feedback target: room for every assembled vertex, no padding. */
	GLuint fb;
	glGenBuffers(1, &fb);
	glBindBuffer(GL_ARRAY_BUFFER, fb);
	glBufferData(GL_ARRAY_BUFFER,
	             nverts * record_floats * (GLsizeiptr)sizeof(float),
	             NULL, GL_STATIC_DRAW);

	glUseProgram(pglCreateProgram(deform_vs, color_fs, 3, interp, GL_FALSE));
	glEnable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	/* Capture the deformed geometry before clipping. */
	pglBeginFeedback(fb, 0,
	                 nverts * record_floats * (GLsizeiptr)sizeof(float),
	                 GL_TRIANGLES);
	glDrawArrays(GL_TRIANGLES, 0, nverts);
	pglEndFeedback();

	GLuint captured = pglFeedbackVertexCount();
	printf("captured vertices : %u (overflow=%u)\n",
	       captured, pglFeedbackOverflowed());

	void* records = NULL;
	pglGetBufferData(fb, &records);
	const float* r = (const float*)records;
	printf("first record      : pos(%.3f %.3f %.3f %.3f) color(%.3f %.3f %.3f)\n",
	       r[0], r[1], r[2], r[3], r[4], r[5], r[6]);

	int first_pass = colored_pixels(px);

	/* Replay: same buffer, ordinary vertex attributes, pass-through shader.
	 * The deformation shader is never called again. */
	glBindBuffer(GL_ARRAY_BUFFER, fb);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE,
	                      record_floats * (int)sizeof(float), 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
	                      record_floats * (int)sizeof(float),
	                      (void*)(4 * sizeof(float)));
	glUseProgram(pglCreateProgram(passthrough_vs, color_fs, 3, interp, GL_FALSE));
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, captured);

	int replay_pass = colored_pixels(px);
	printf("colored pixels    : deform=%d replay=%d (match=%s)\n",
	       first_pass, replay_pass, first_pass == replay_pass ? "yes" : "NO");

	/* Rasterizer discard: vertex shading and feedback still happen, but no
	 * fragment shader runs. Pause/End leave the switch untouched. The source
	 * attributes must come from the original VBO, not the feedback target. */
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 7 * (int)sizeof(float), 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 7 * (int)sizeof(float),
	                      (void*)(4 * sizeof(float)));
	glUseProgram(pglCreateProgram(passthrough_vs, counting_fs, 3, interp, GL_FALSE));
	frag_count = 0;
	pglRasterizerDiscard(GL_TRUE);
	pglBeginFeedback(fb, 0,
	                 nverts * record_floats * (GLsizeiptr)sizeof(float),
	                 GL_TRIANGLES);
	glDrawArrays(GL_TRIANGLES, 0, nverts);
	pglEndFeedback();
	pglRasterizerDiscard(GL_FALSE);
	GLuint recaptured = pglFeedbackVertexCount();
	printf("discarded draw    : fragments=%u recaptured=%u\n",
	       frag_count, recaptured);

	int ok = (captured == (GLuint)nverts && first_pass == replay_pass &&
	          frag_count == 0 && recaptured == (GLuint)nverts);
	free_glContext(&ctx);
	return ok ? 0 : 2;
}
