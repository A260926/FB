/* Minimal c-ray-fast example: build a fused ball field scene entirely from
 * in-memory data (no scene file, no scene parser) and write a PPM image to
 * stdout. It links the same production translation units as the CLI. The CLI
 * translation unit's main() is renamed at compile time so this file keeps it:
 *
 *   cc -std=c99 -I.. -Dmain=c_ray_cli_main -c ../c-ray-fast.c
 *   cc -std=c99 -I.. -c ../octree.c ../metaball.c
 *   cc -std=c99 -I.. memory_scene.c c-ray-fast.o octree.o metaball.o \
 *      -lm -lpthread -o memory_scene
 *   ./memory_scene > memory_scene.ppm
 *
 * Only C99, the system math library and pthreads are used.
 */
#include <math.h>
#include <stdio.h>

#include "../scene.h"
#include "../metaball.h"

#define W 120
#define H 90

int main(void)
{
	/* static storage keeps every scene object address stable for the
	 * octree, which stores pointers rather than copying objects. */
	static struct material mtl[3];
	static struct metaball group;
	static struct mb_component comp[3];
	static struct object objs[3];
	static struct sphere ball;
	static struct plane floor_plane;
	static struct light lamp;
	static struct color framebuffer[W * H];
	int x, y;

	/* materials: a waxy group, a matte sphere and a reflective floor */
	mtl[0] = def_mtl;
	mtl[0].name = "wax";
	mtl[0].kd = (struct color){0.85f, 0.55f, 0.25f, 1.0f};
	mtl[0].ks = (struct color){0.3f, 0.3f, 0.3f, 1.0f};
	mtl[0].spow = 40.0;

	mtl[1] = def_mtl;
	mtl[1].name = "ball";
	mtl[1].kd = (struct color){0.2f, 0.5f, 0.9f, 1.0f};

	mtl[2] = def_mtl;
	mtl[2].name = "floor";
	mtl[2].kd = (struct color){0.25f, 0.25f, 0.3f, 1.0f};
	mtl[2].refl = 0.3;

	/* fused ball field: two positive kernels fused, one negative notch */
	group.name = "lump";
	group.tau = 0.45;
	group.mat = mtl[0];
	group.comp = comp;
	comp[0].c = (struct vec3){-0.9, 0.6, 0.0}; comp[0].r = 1.4; comp[0].w = 1.0;
	comp[1].c = (struct vec3){ 0.9, 0.6, 0.0}; comp[1].r = 1.4; comp[1].w = 1.0;
	comp[2].c = (struct vec3){ 0.0, 0.6, 0.0}; comp[2].r = 1.0; comp[2].w = -0.55;
	group.num_comp = 3;
	group.max_comp = 3;

	/* object list: the field group, a sphere and an infinite plane */
	objs[0].type = OBJ_METABALL;
	objs[0].o.mb = &group;
	objs[0].mat = group.mat;

	objs[1].type = OBJ_SPHERE;
	ball.pos = (struct vec3){-2.6, 0.1, -1.4};
	ball.rad = 0.45;
	objs[1].o.sph = ball;
	objs[1].mat = mtl[1];

	objs[2].type = OBJ_PLANE;
	floor_plane.pos = (struct vec3){0, -1.0, 0};
	floor_plane.norm = (struct vec3){0, 1, 0};
	objs[2].o.pln = floor_plane;
	objs[2].mat = mtl[2];

	/* wire the global scene state declared in scene.h */
	objects = objs;
	num_obj = 3;
	infobj = objs + 2;		/* planes are also the infinite-object list */
	num_inf = 1;
	materials = mtl;
	num_mtl = 3;
	metaballs = &group;
	num_mb = 1;
	lights = &lamp;
	num_lt = 1;
	lamp.pos = (struct vec3){-4, 5, -6};
	lamp.col = (struct color){1, 1, 1, 1};

	cam.pos = (struct vec3){0, 2.4, -7};
	cam.targ = (struct vec3){0, 0.4, 0};
	cam.fov = M_PI * 32.0 / 180.0;

	xres = W;
	yres = H;
	aspect = (double)W / (double)H;
	rays_per_pixel = 1;
	rcp_samples = 1.0;
	ambcol = (struct color){0.05f, 0.05f, 0.05f, 0};
	bgcol = (struct color){0.02f, 0.02f, 0.03f, 0};
	pixels = framebuffer;

	/* build (or rebuild) the octree over the current objects */
	if(prepare_scene() == -1) {
		fputs("failed to prepare scene\n", stderr);
		return 1;
	}

	/* render one primary ray per pixel without the threaded block driver */
	for(y=0; y<H; y++) {
		for(x=0; x<W; x++) {
			struct ray ray;
			struct color col;
			get_primary_ray(&ray, x, y, 0);
			trace(&ray, 0, &col);
			framebuffer[y * W + x] = col;
		}
	}

	/* emit a binary PPM (P6) with the same gamma encoding as the CLI */
	printf("P6\n# c-ray-fast in-memory fused ball example\n%d %d\n255\n", W, H);
	for(x=0; x<W*H; x++) {
		double inv_gamma = 1.0 / 2.2;
		int r = (int)(pow(framebuffer[x].r, inv_gamma) * 255.0);
		int g = (int)(pow(framebuffer[x].g, inv_gamma) * 255.0);
		int b = (int)(pow(framebuffer[x].b, inv_gamma) * 255.0);
		fputc(CLAMP(r, 0, 255), stdout);
		fputc(CLAMP(g, 0, 255), stdout);
		fputc(CLAMP(b, 0, 255), stdout);
	}
	return 0;
}
