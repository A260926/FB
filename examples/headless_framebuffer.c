/*
 * Headless framebuffer example.
 *
 * Pass 1 renders a shaded triangle directly into a texture through a
 * framebuffer object.  Pass 2 samples that texture while drawing a quad
 * onto the default ZBuffer, which is then written out as a binary PPM.
 *
 * Ownership:
 *  - The ZBuffer passed to glInit() (and anything attached to it) is
 *    allocated and released by the caller with ZB_open()/ZB_close().
 *  - Framebuffer objects created with glGenFramebuffers() own their
 *    private depth buffers; glDeleteFramebuffers() (or glClose()) frees
 *    them.  They never own the attached texture's color pixels.
 *  - Texture objects created with glGenTextures()/glBindTexture() own
 *    their images until glDeleteTextures() (or glClose()).  An attached
 *    texture keeps its pixels; the framebuffer only aliases them.
 */

#include <GL/gl.h>
#include <zbuffer.h>
#include <stdio.h>
#include <stdlib.h>

#define WIN 64

static void write_ppm(const char* path, const PIXEL* pbuf, int w, int h) {
	FILE* f = fopen(path, "wb");
	int i;
	if (f == NULL) {
		perror(path);
		exit(1);
	}
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (i = 0; i < w * h; i++) {
		unsigned char rgb[3];
		rgb[0] = (unsigned char)GET_RED(pbuf[i]);
		rgb[1] = (unsigned char)GET_GREEN(pbuf[i]);
		rgb[2] = (unsigned char)GET_BLUE(pbuf[i]);
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

static void fill_rgb(unsigned char* pixels, int d,
                     unsigned char r, unsigned char g, unsigned char b) {
	int i;
	for (i = 0; i < d * d; i++) {
		pixels[3 * i] = r;
		pixels[3 * i + 1] = g;
		pixels[3 * i + 2] = b;
	}
}

int main(void) {
	const int d = TGL_FEATURE_TEXTURE_DIM;
	ZBuffer* window;
	GLuint texture, fbo;
	unsigned char* pixels;
	PIXEL center;

	window = ZB_open(WIN, WIN, ZB_MODE_RGBA, NULL);
	if (window == NULL)
		return 1;
	glInit(window);
	glViewport(0, 0, WIN, WIN);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);

	pixels = malloc((size_t)d * d * 3);
	if (pixels == NULL)
		return 2;

	/* A texture that starts life as a plain dark-blue image. */
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	fill_rgb(pixels, d, 10, 20, 60);
	glTexImage2D(GL_TEXTURE_2D, 0, 3, d, d, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);

	/* Pass 1: render into the texture through a framebuffer object. */
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return 3;

	glViewport(0, 0, d, d);
	glDisable(GL_TEXTURE_2D);
	glClearColor(0.05f, 0.08f, 0.25f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBegin(GL_TRIANGLES);
	glColor3f(1.0f, 0.1f, 0.1f);
	glVertex3f(-0.8f, -0.8f, 0.0f);
	glColor3f(0.1f, 1.0f, 0.1f);
	glVertex3f(0.8f, -0.8f, 0.0f);
	glColor3f(0.1f, 0.1f, 1.0f);
	glVertex3f(0.0f, 0.9f, 0.0f);
	glEnd();

	/* Pass 2: back on the default target, texture the screen with it. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, WIN, WIN);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBegin(GL_QUADS);
	glTexCoord2f(0.0f, 0.0f);
	glVertex3f(-1.0f, -1.0f, 0.0f);
	glTexCoord2f(1.0f, 0.0f);
	glVertex3f(1.0f, -1.0f, 0.0f);
	glTexCoord2f(1.0f, 1.0f);
	glVertex3f(1.0f, 1.0f, 0.0f);
	glTexCoord2f(0.0f, 1.0f);
	glVertex3f(-1.0f, 1.0f, 0.0f);
	glEnd();

	center = window->pbuf[(WIN / 2) * WIN + WIN / 2];
	printf("framebuffer example: center rgb=(%u,%u,%u)\n",
	       GET_RED(center), GET_GREEN(center), GET_BLUE(center));
	if (GET_RED(center) + GET_GREEN(center) + GET_BLUE(center) == 0)
		return 4;

	write_ppm(".task-build/framebuffer.ppm", window->pbuf, WIN, WIN);

	/* The FBO's depth storage goes away here; the texture image is separate. */
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &texture);
	free(pixels);
	glClose();            /* releases every object still owned by the context */
	ZB_close(window);      /* caller-owned default framebuffer */
	return 0;
}
