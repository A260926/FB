// geometry_ribbon: expand real GL_LINES input into width-bearing triangle
// strips with the optional geometry callback stage.
//
// Usage: geometry_ribbon <output.ppm>
//
// Renders a 64x64 P6 PPM.  No window system or external assets are used.
#define PORTABLEGL_IMPLEMENTATION
#include "portablegl.h"

#include <stdio.h>
#include <string.h>

#define WIDTH  64
#define HEIGHT 64

// Vertex shader: attribute 0 is a clip-space position, attribute 1 a color.
static void ribbon_vs(float* vs_output, vec4* vertex_attribs, Shader_Builtins* builtins, void* uniforms)
{
	(void)uniforms;
	builtins->gl_Position = vertex_attribs[0];
	((vec4*)vs_output)[0] = vertex_attribs[1];
}

static void ribbon_fs(float* fs_input, Shader_Builtins* builtins, void* uniforms)
{
	(void)uniforms;
	builtins->gl_FragColor = ((vec4*)fs_input)[0];
}

// Geometry callback: turn one input line into a quad (two triangles) whose
// half-width follows the line's normal in clip space.
//
// `uniforms` is the program's pglSetUniform pointer, as required by the API.
typedef struct RibbonUniforms
{
	float half_width;
} RibbonUniforms;

static void ribbon_geometry(const pglGeometryInput* input, pglGeometryEmitter* emitter, void* uniforms)
{
	RibbonUniforms* u = (RibbonUniforms*)uniforms;
	pglGeometryVertex a = input->vertices[0];
	pglGeometryVertex b = input->vertices[1];

	// Work in clip space; w is 1 for this orthographic-ish scene.
	vec4 pa = a.position;
	vec4 pb = b.position;
	float dx = pb.x - pa.x;
	float dy = pb.y - pa.y;
	float len = sqrtf(dx*dx + dy*dy);
	if (len < 1e-6f)
		return;

	// Unit normal, scaled to the requested half width.
	float nx = -dy / len * u->half_width;
	float ny =  dx / len * u->half_width;

	// Emit as a triangle strip: a0, a1, b0, b1
	pglGeometryVertex v = a;
	v.position.x = pa.x + nx; v.position.y = pa.y + ny;
	pglEmitGeometryVertex(emitter, &v);

	v = a;
	v.position.x = pa.x - nx; v.position.y = pa.y - ny;
	pglEmitGeometryVertex(emitter, &v);

	v = b;
	v.position.x = pb.x + nx; v.position.y = pb.y + ny;
	pglEmitGeometryVertex(emitter, &v);

	v = b;
	v.position.x = pb.x - nx; v.position.y = pb.y - ny;
	pglEmitGeometryVertex(emitter, &v);
	// Implicit End closes the strip.
}

static int write_ppm(const char* path, pix_t* pixels)
{
	FILE* f = fopen(path, "wb");
	if (!f)
		return 0;

	if (fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT) < 0) {
		fclose(f);
		return 0;
	}

	for (int y = 0; y < HEIGHT; ++y) {
		for (int x = 0; x < WIDTH; ++x) {
			Color c = PIXEL_TO_COLOR(pixels[y * WIDTH + x]);
			unsigned char rgb[3];
			rgb[0] = (unsigned char)c.r;
			rgb[1] = (unsigned char)c.g;
			rgb[2] = (unsigned char)c.b;
			if (fwrite(rgb, 1, 3, f) != 3) {
				fclose(f);
				return 0;
			}
		}
	}

	fclose(f);
	return 1;
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <output.ppm>\n", argv[0]);
		return 2;
	}

	glContext ctx;
	pix_t* pixels = NULL;
	if (!init_glContext(&ctx, &pixels, WIDTH, HEIGHT)) {
		fprintf(stderr, "init_glContext failed\n");
		return 1;
	}

	// Real line input: a zig-zag that crosses the viewport.
	float pos[] = {
		-0.8f, -0.6f, 0.0f, 1.0f,
		 0.8f, -0.2f, 0.0f, 1.0f,
		 0.8f, -0.2f, 0.0f, 1.0f,
		-0.8f,  0.2f, 0.0f, 1.0f,
		-0.8f,  0.2f, 0.0f, 1.0f,
		 0.8f,  0.6f, 0.0f, 1.0f,
	};
	float colors[] = {
		1.0f, 0.2f, 0.2f, 1.0f,
		1.0f, 0.2f, 0.2f, 1.0f,
		0.2f, 1.0f, 0.2f, 1.0f,
		0.2f, 1.0f, 0.2f, 1.0f,
		0.2f, 0.4f, 1.0f, 1.0f,
		0.2f, 0.4f, 1.0f, 1.0f,
	};

	GLuint vbo, cbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(pos), pos, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);

	glGenBuffers(1, &cbo);
	glBindBuffer(GL_ARRAY_BUFFER, cbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(colors), colors, GL_STATIC_DRAW);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, 0);

	GLenum interp[4] = { PGL_SMOOTH, PGL_SMOOTH, PGL_SMOOTH, PGL_SMOOTH };
	GLuint program = pglCreateProgram(ribbon_vs, ribbon_fs, 4, interp, GL_FALSE);
	glUseProgram(program);

	// The callback receives this same pointer as its trailing void*.
	RibbonUniforms uniforms;
	uniforms.half_width = 4.0f / WIDTH;  // ~4 pixels wide
	pglSetUniform(&uniforms);

	if (!pglSetGeometryShader(program, GL_LINES, GL_TRIANGLE_STRIP, 4, ribbon_geometry)) {
		fprintf(stderr, "pglSetGeometryShader failed\n");
		free_glContext(&ctx);
		return 1;
	}

	glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDrawArrays(GL_LINES, 0, 6);

	int ok = write_ppm(argv[1], pixels);

	free_glContext(&ctx);
	if (!ok) {
		fprintf(stderr, "failed to write %s\n", argv[1]);
		return 1;
	}

	printf("GEOMETRY_RIBBON_OK wrote %s\n", argv[1]);
	return 0;
}
