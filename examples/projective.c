/* Projective-instancing example.
 *
 * Usage: projective <output.ppm>
 *
 * Builds a scene made of a single multi-sphere prototype placed through
 * several projective instances (one of them genuinely cross-pole/unbounded),
 * then renders it through the production trace/shade pipeline and writes a
 * PPM. Only C99, libm and pthreads are used.
 */
#include "projective.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

/* renderer globals/functions supplied by the production build */
extern int xres, yres;
extern struct color *pixels;
extern double inv_gamma;
extern struct camera cam;
extern int rays_per_pixel;
void get_primary_ray(struct ray *ray, int x, int y, int sample);
void trace(struct ray *ray, int depth, struct color *col);

static const char *scene_text =
	"? ambient 0.04 0.04 0.04\n"
	"M \"floor\" 0.25 0.28 0.35 1 1 1 40 0.2 0 0\n"
	"M \"ruby\"  0.85 0.12 0.18 1 1 1 60 0.35 0 0\n"
	"M \"jade\"  0.15 0.75 0.35 1 1 1 60 0.30 0 0\n"
	"M \"sky\"   0.20 0.45 0.90 1 1 1 60 0.40 0 0\n"
	/* one prototype: three local spheres, each with its own material */
	"p \"cluster\" 0 0 0 1 \"ruby\"\n"
	"p \"cluster\" 1.6 0.3 0 0.55 \"jade\"\n"
	"p \"cluster\" -1.4 0.4 0.4 0.5 \"sky\"\n"
	/* affine instances sharing that prototype */
	"i \"left\"  \"cluster\" 1 0 0 -3.5 0 1 0 0 0 0 1 0 0 0 0 1\n"
	"i \"right\" \"cluster\" 1 0 0 3.5 0 1 0 0 0 0 1 0 0 0 0 1\n"
	/* an instance scaled/rotated into a tall column */
	"i \"tower\" \"cluster\" 1 0 0 0 0 1.8 0 0 0 0 0.8 0 0 0 0 1\n"
	/* a genuinely projective, cross-pole instance: Wh = 1 - Z_local */
	"i \"warp\"  \"cluster\" 1 0 0 0 0 1 0 0.6 0 0 1 0 0 0 -1 1\n"
	/* legacy floor plane and sphere, plus lights */
	"p 0 -1.2 0 0 1 0 \"floor\"\n"
	"s 0 -0.6 3.2 0.6 \"sky\"\n"
	"l -8 10 -8 1 1 1\n"
	"l 7 6 6 0.7 0.7 0.8\n";

static FILE *write_temp_scene(void)
{
	const char *dir = getenv("TMPDIR");
	char path[512];
	FILE *f;

	if(!dir || !*dir) dir = ".";
	snprintf(path, sizeof path, "%s/cray_projective_example.scn", dir);
	f = fopen(path, "w+b");
	if(!f) return 0;
	fputs(scene_text, f);
	rewind(f);
	remove(path);
	return f;
}

int main(int argc, char **argv)
{
	FILE *scene, *out;
	int x, y;

	if(argc != 2) {
		fprintf(stderr, "usage: %s <output.ppm>\n", argv[0]);
		return 1;
	}

	xres = 120;
	yres = 90;
	inv_gamma = 1.0 / 2.2;
	rays_per_pixel = 1;

	scene = write_temp_scene();
	if(!scene) {
		perror("failed to create temporary scene");
		return 1;
	}
	if(load_scene(scene) == -1) {
		fclose(scene);
		return 1;
	}
	fclose(scene);
	if(prepare_scene() == -1) {
		return 1;
	}

	cam.pos = (struct vec3){0, 3.2, -11};
	cam.targ = (struct vec3){0, 0.2, 0};
	cam.fov = M_PI * 45.0 / 180.0;

	pixels = malloc((size_t)xres * yres * sizeof *pixels);
	if(!pixels) {
		perror("pixel buffer allocation failed");
		return 1;
	}

	/* single-threaded production render: same trace() entry the pthreads
	 * workers use, so shading, reflections and shadow occlusion all run the
	 * exact production path.
	 */
	for(y = 0; y < yres; y++) {
		for(x = 0; x < xres; x++) {
			struct ray ray;
			struct color col;
			get_primary_ray(&ray, x, y, 0);
			trace(&ray, 0, &col);
			pixels[y * xres + x] = col;
		}
	}

	out = fopen(argv[1], "wb");
	if(!out) {
		perror("failed to open output PPM");
		return 1;
	}
	fprintf(out, "P6\n# c-ray-fast projective example\n%d %d\n255\n", xres, yres);
	for(y = 0; y < xres * yres; y++) {
		int r = (int)(pow(pixels[y].r, inv_gamma) * 255.0);
		int g = (int)(pow(pixels[y].g, inv_gamma) * 255.0);
		int b = (int)(pow(pixels[y].b, inv_gamma) * 255.0);
		fputc(CLAMP(r, 0, 255), out);
		fputc(CLAMP(g, 0, 255), out);
		fputc(CLAMP(b, 0, 255), out);
	}
	fclose(out);
	fprintf(stderr, "wrote %s (%dx%d)\n", argv[1], xres, yres);
	return 0;
}
