/* Clip-path preview generator: writes a P6 PPM.

   Usage: clip_preview <output.ppm>

   The picture shows a nested clip (a group clip intersected with a per-shape
   clip) and the same objectBoundingBox definition applied to elements of
   different sizes, with a checkerboard under the transparent areas. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nanosvg.h"
#include "nanosvgrast.h"

#define WIDTH 96
#define HEIGHT 48

static NSVGimage* parseImage(const char* svg)
{
	char* copy;
	NSVGimage* image;
	size_t len = strlen(svg);
	copy = (char*)malloc(len + 1);
	if (copy == NULL) return NULL;
	memcpy(copy, svg, len + 1);
	image = nsvgParse(copy, "px", 96);
	free(copy);
	return image;
}

static void checkerboard(unsigned char* dst)
{
	int x, y;
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < WIDTH; x++) {
			int on = ((x / 6) + (y / 6)) & 1;
			unsigned char* p = &dst[(y * WIDTH + x) * 4];
			unsigned v = on ? 200 : 235;
			p[0] = (unsigned char)v;
			p[1] = (unsigned char)v;
			p[2] = (unsigned char)v;
			p[3] = 255;
		}
	}
}

/* Composite non-premultiplied RGBA over an opaque background. */
static void blendOver(unsigned char* dst, const unsigned char* src, int count)
{
	int i;
	for (i = 0; i < count; i++) {
		unsigned a = src[3];
		unsigned ia = 255 - a;
		dst[0] = (unsigned char)((src[0] * a + dst[0] * ia + 127) / 255);
		dst[1] = (unsigned char)((src[1] * a + dst[1] * ia + 127) / 255);
		dst[2] = (unsigned char)((src[2] * a + dst[2] * ia + 127) / 255);
		dst += 4;
		src += 4;
	}
}

static int writePPM(const char* path, const unsigned char* rgba)
{
	FILE* fp;
	int i;
	fp = fopen(path, "wb");
	if (fp == NULL) return 0;
	if (fprintf(fp, "P6\n%d %d\n255\n", WIDTH, HEIGHT) != (int)strlen("P6\n96 48\n255\n")) {
		fclose(fp);
		return 0;
	}
	for (i = 0; i < WIDTH * HEIGHT; i++) {
		if (fwrite(&rgba[i*4], 1, 3, fp) != 3) {
			fclose(fp);
			return 0;
		}
	}
	return fclose(fp) == 0;
}

int main(int argc, char** argv)
{
	static const char* svg =
		"<svg width='96' height='48'>"
		"<defs>"
		"<clipPath id='ring'>"
		"<path d='M6,6 L42,6 L42,42 L6,42 Z M14,14 L34,14 L34,34 L14,34 Z' clip-rule='evenodd'/>"
		"</clipPath>"
		"<clipPath id='half' clipPathUnits='objectBoundingBox'>"
		"<rect x='0' y='0' width='0.55' height='1'/>"
		"</clipPath>"
		"</defs>"
		/* nested clip: group ring, shape half-offset */
		"<g clip-path='url(#ring)'>"
		"<rect x='0' y='0' width='48' height='48' fill='#e04040'/>"
		"<circle cx='24' cy='24' r='15' fill='#ffd050' clip-path='url(#half)'/>"
		"</g>"
		/* same objectBoundingBox definition on differently sized elements */
		"<rect x='52' y='8' width='16' height='28' fill='#4060e0' clip-path='url(#half)'/>"
		"<rect x='72' y='4' width='20' height='40' fill='#40b070' clip-path='url(#half)'/>"
		"</svg>";

	NSVGimage* image;
	NSVGrasterizer* rast;
	unsigned char* canvas;
	unsigned char* layer;
	int ok;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <output.ppm>\n", argv[0]);
		return 2;
	}

	image = parseImage(svg);
	if (image == NULL) {
		fprintf(stderr, "failed to parse preview svg\n");
		return 1;
	}
	rast = nsvgCreateRasterizer();
	canvas = (unsigned char*)malloc(WIDTH * HEIGHT * 4);
	layer = (unsigned char*)malloc(WIDTH * HEIGHT * 4);
	if (rast == NULL || canvas == NULL || layer == NULL) {
		fprintf(stderr, "out of memory\n");
		nsvgDelete(image);
		nsvgDeleteRasterizer(rast);
		free(canvas);
		free(layer);
		return 1;
	}

	checkerboard(canvas);
	nsvgRasterize(rast, image, 0, 0, 1.0f, layer, WIDTH, HEIGHT, WIDTH * 4);
	blendOver(canvas, layer, WIDTH * HEIGHT);
	ok = writePPM(argv[1], canvas);
	if (!ok) fprintf(stderr, "could not write %s\n", argv[1]);

	free(layer);
	free(canvas);
	nsvgDeleteRasterizer(rast);
	nsvgDelete(image);
	return ok ? 0 : 1;
}
