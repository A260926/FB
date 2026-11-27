// CPU back-end example: render a small NanoVG scene off-screen and write it
// to a binary PPM (P6) file. No window or GPU is required; the program only
// links the NanoVG core and the CPU back-end.
//
// Build:
//   cc -std=c99 -O1 -Isrc src/nanovg.c src/nanovg_cpu.c example/example_cpu.c -lm -o example_cpu
// Run:
//   ./example_cpu preview.ppm

#include "nanovg_cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH  128
#define HEIGHT  96

int main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "preview.ppm";
	unsigned char* rgba = (unsigned char*)malloc((size_t)WIDTH * HEIGHT * 4);
	if (!rgba) return 1;
	memset(rgba, 0, (size_t)WIDTH * HEIGHT * 4); // transparent background

	NVGcontext* vg = nvgCreateCPU(rgba, WIDTH, HEIGHT, WIDTH * 4);
	if (!vg) {
		free(rgba);
		return 1;
	}

	nvgBeginFrame(vg, WIDTH, HEIGHT, 1.0f);

	// Filled rounded rectangle with a circular hole.
	nvgBeginPath(vg);
	nvgRoundedRect(vg, 8, 8, 72, 56, 8);
	nvgCircle(vg, 44, 36, 14);
	nvgPathWinding(vg, NVG_HOLE);
	nvgFillPaint(vg, nvgLinearGradient(vg, 8, 8, 80, 64,
									  nvgRGBA(255, 96, 0, 255),
									  nvgRGBA(120, 0, 200, 255)));
	nvgFill(vg);

	// Translucent polyline stroke.
	nvgBeginPath(vg);
	nvgMoveTo(vg, 16, 80);
	nvgLineTo(vg, 48, 66);
	nvgLineTo(vg, 96, 84);
	nvgStrokeWidth(vg, 3.0f);
	nvgStrokeColor(vg, nvgRGBA(0, 160, 220, 180));
	nvgStroke(vg);

	// Text, using the font shipped in the repository.
	int font = nvgCreateFont(vg, "roboto", "example/Roboto-Regular.ttf");
	if (font >= 0) {
		nvgFontFaceId(vg, font);
		nvgFontSize(vg, 16);
		nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
		nvgText(vg, 88, 28, "CPU", NULL);
	}

	nvgEndFrame(vg);

	// PPM has no alpha channel; write the premultiplied RGB bytes directly.
	FILE* f = fopen(path, "wb");
	if (!f) {
		nvgDeleteCPU(vg);
		free(rgba);
		return 1;
	}
	fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
	for (int i = 0; i < WIDTH * HEIGHT; i++)
		fwrite(rgba + i * 4, 1, 3, f);
	fclose(f);

	nvgDeleteCPU(vg);
	free(rgba);
	printf("wrote %s (%dx%d)\n", path, WIDTH, HEIGHT);
	return 0;
}
