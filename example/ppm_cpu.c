//
// Minimal off-screen NanoVG CPU back-end example.
//
// Renders a small scene entirely on the CPU (no window or GPU) into a
// caller-owned RGBA8 buffer and writes it out as a binary PPM (P6) image.
//
// Build from the repository root with a plain C99 compiler:
//
//	cc -std=c99 -O1 -Isrc example/ppm_cpu.c src/nanovg.c src/nanovg_cpu.c \
//		-lm -o ppm_cpu
//	./ppm_cpu out.ppm
//
// The resulting PPM composites the premultiplied RGBA buffer over the opaque
// white background it was cleared to, so it can be viewed directly.
//

#include "nanovg_cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 180
#define HEIGHT 110

int main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "cpu_preview.ppm";
	const int stride = WIDTH * 4;
	unsigned char* rgba = (unsigned char*)malloc((size_t)stride * HEIGHT);
	FILE* f;
	int x, y;
	if (!rgba) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	// Opaque white background, top-left origin, premultiplied RGBA8.
	for (y = 0; y < HEIGHT; ++y)
		for (x = 0; x < WIDTH; ++x) {
			unsigned char* px = rgba + (size_t)y * stride + (size_t)x * 4;
			px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 255;
		}

	NVGcontext* vg = nvgCreateCPU(rgba, WIDTH, HEIGHT, stride);
	if (!vg) {
		fprintf(stderr, "failed to create CPU context\n");
		free(rgba);
		return 1;
	}

	nvgBeginFrame(vg, WIDTH, HEIGHT, 1.0f);

	// Soft box-gradient rounded rectangle.
	NVGpaint bg = nvgBoxGradient(vg, 10, 10, WIDTH - 20, HEIGHT - 30, 12, 8,
								nvgRGBA(64, 140, 220, 255), nvgRGBA(16, 40, 90, 255));
	nvgBeginPath(vg);
	nvgRoundedRect(vg, 10, 10, WIDTH - 20, HEIGHT - 30, 12);
	nvgFillPaint(vg, bg);
	nvgFill(vg);

	// A translucent stroked polyline; overlapping joins stay uniform because a
	// single stroke merges coverage into a union.
	nvgBeginPath(vg);
	nvgMoveTo(vg, 24, 70);
	nvgLineTo(vg, 70, 30);
	nvgLineTo(vg, 110, 72);
	nvgLineTo(vg, 156, 34);
	nvgStrokeWidth(vg, 4);
	nvgStrokeColor(vg, nvgRGBA(255, 210, 60, 200));
	nvgStroke(vg);

	// Text rendered through the alpha font atlas.
	int font = nvgCreateFont(vg, "roboto", "example/Roboto-Regular.ttf");
	if (font >= 0) {
		nvgFontFaceId(vg, font);
		nvgFontSize(vg, 22);
		nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
		nvgText(vg, 18, 96, "NanoVG CPU", NULL);
	}

	nvgEndFrame(vg); // the borrowed bytes are only published here
	nvgDeleteCPU(vg);

	f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		free(rgba);
		return 1;
	}
	fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
	// The buffer is already composited over an opaque background, so its
	// premultiplied RGB channels are the final display colors.
	for (y = 0; y < HEIGHT; ++y)
		for (x = 0; x < WIDTH; ++x) {
			const unsigned char* px = rgba + (size_t)y * stride + (size_t)x * 4;
			fputc(px[0], f);
			fputc(px[1], f);
			fputc(px[2], f);
		}
	fclose(f);
	free(rgba);
	printf("wrote %s (%dx%d PPM)\n", path, WIDTH, HEIGHT);
	return 0;
}
