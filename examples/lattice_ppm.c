/*
 * Example: a large repeated pillar / grid scene rendered from an in-memory
 * scene to a binary PPM file, using the finite integer-lattice instance set.
 *
 * Two lattices are built from a handful of opaque prototypes:
 *   - a row of pillars made of stacked spheres,
 *   - a wall of triangular panels (the "grid").
 * The lattices cover a few hundred thousand copies in total while the render
 * only ever stores the prototypes and their BVH.
 *
 * Build (from the repository root):
 *   make -f Makefile.rt lattice-example
 *   ./build/lattice_ppm lattice.ppm
 *
 * Or manually, compiling all production sources except src/raytracer/main.c
 * and src/raytracer/image.c plus this file (this example supplies the
 * `struct Image image` symbol that render.c references, and performs its own
 * PPM output).
 *
 * Run:
 *   ./lattice_ppm lattice.ppm
 **/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accel.h"
#include "argv.h"
#include "calc.h"
#include "camera.h"
#include "image.h"
#include "lattice.h"
#include "material.h"
#include "object.h"
#include "render.h"

#include "production.h"

/* The build recipe also covers src/core and the bundled libraries. */
#include "system.h"

/* Definition required by render.c (image.c would normally provide it). */
struct Image image;

#define WIDTH 640
#define HEIGHT 360

static void *xmalloc(size_t size)
{
	void *ptr = malloc(size);
	if (!ptr) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return ptr;
}

static struct Material *make_material(const v3 ks, const v3 ka, const v3 kr, const v3 kt, const v3 ke, struct Texture *texture)
{
	struct Material *material = xmalloc(sizeof(struct Material));
	material_init(material, 1, ks, ka, kr, kt, ke, 32.f, 1.f, texture);
	return material;
}

static struct Object *make_sphere(struct Material *material, const v3 position, const float radius)
{
	struct Object *object = sphere_new(position, radius);
	object_init(object, material, -1.f, 0, OBJECT_SPHERE);
	object->object_data->postinit(object);
	return object;
}

static struct Object *make_triangle(struct Material *material, v3 vertices[3])
{
	struct Object *object = triangle_new(vertices);
	object_init(object, material, -1.f, 0, OBJECT_TRIANGLE);
	object->object_data->postinit(object);
	return object;
}

int main(int argc, char *argv[])
{
	const char *filename = argc > 1 ? argv[1] : "lattice.ppm";
	const v3 zero = { 0.f, 0.f, 0.f };

	/* Keep the upstream random source deterministic and the log quiet. */
	srand(20221116);

	/* Configure the renderer through the same command-line interface the
	 * engine uses: linear light attenuation and a couple of bounces. */
	{
		static char arg0[] = "lattice_ppm";
		static char arg1[] = "-l";
		static char arg2[] = "lin";
		static char arg3[] = "-b";
		static char arg4[] = "4";
		static char *args[] = { arg0, arg1, arg2, arg3, arg4 };
		myargc = 5;
		myargv = args;
		argv_init();
		render_init();
	}

	struct Texture *stone = texture_checkerboard_new((v3[2]){ { 0.52f, 0.5f, 0.47f }, { 0.42f, 0.4f, 0.38f } }, 0.35f);
	struct Texture *panel = texture_brick_new((v3[2]){ { 0.25f, 0.35f, 0.5f }, { 0.1f, 0.1f, 0.12f } }, 1.5f, 0.08f);
	struct Texture *lamp_texture = texture_uniform_new(zero);

	struct Material *stone_material = make_material((v3){ 0.2f, 0.2f, 0.2f }, (v3){ 0.35f, 0.33f, 0.3f }, (v3){ 0.15f, 0.15f, 0.15f }, zero, zero, stone);
	struct Material *panel_material = make_material((v3){ 0.3f, 0.4f, 0.5f }, (v3){ 0.2f, 0.25f, 0.3f }, (v3){ 0.5f, 0.55f, 0.6f }, zero, zero, panel);
	struct Material *lamp_material = make_material(zero, zero, zero, zero, (v3){ 60.f, 55.f, 45.f }, lamp_texture);
	struct Material *ground_material = make_material(zero, (v3){ 0.25f, 0.25f, 0.25f }, zero, zero, zero, stone);

	/* Ordinary scene: one emitter (also the lamp the camera sees) plus a
	 * large ground sphere.  Both are outside the lattices. */
	num_objects = 2;
	num_emittant_objects = 1;
#ifdef UNBOUND_OBJECTS
	num_unbound_objects = 0;
#endif
	objects_init();

	struct Object *lamp = make_sphere(lamp_material, (v3){ 6.f, 12.f, 10.f }, 1.2f);
	lamp->num_lights = 1;
	struct Object *ground = make_sphere(ground_material, (v3){ 0.f, -120.f, 0.f }, 120.f);
	objects[0] = ground;
	objects[1] = lamp;
	emittant_objects[0] = lamp;
	accel_init();

	/* Pillar prototype: a stacked pair of spheres inside one cell. */
	struct Object *pillar_bottom = make_sphere(stone_material, (v3){ 0.f, 1.2f, 0.f }, 1.2f);
	struct Object *pillar_top = make_sphere(stone_material, (v3){ 0.f, 3.4f, 0.f }, 0.9f);
	struct Object *pillar_prototypes[2] = { pillar_bottom, pillar_top };
	const v3 pillar_period = { 4.f, 2.2f, 4.f };
	const int32_t pillar_lower[3] = { -14, 0, -12 };
	const int32_t pillar_upper[3] = { 14, 0, 4 };

	/* Grid-wall prototype: two triangles forming a square panel behind the
	 * pillars. */
	v3 panel_a[3] = { { -1.8f, 0.f, 0.2f }, { 1.8f, 0.f, 0.2f }, { -1.8f, 3.6f, 0.2f } };
	v3 panel_b[3] = { { 1.8f, 0.f, 0.2f }, { 1.8f, 3.6f, 0.2f }, { -1.8f, 3.6f, 0.2f } };
	struct Object *panel_0 = make_triangle(panel_material, panel_a);
	struct Object *panel_1 = make_triangle(panel_material, panel_b);
	struct Object *panel_prototypes[2] = { panel_0, panel_1 };
	const v3 panel_period = { 4.f, 4.2f, 1.f };
	const int32_t panel_lower[3] = { -14, 0, -14 };
	const int32_t panel_upper[3] = { 14, 3, -14 };

	struct Lattice *pillars = lattice_build(pillar_prototypes, 2, pillar_period, pillar_lower, pillar_upper);
	struct Lattice *wall = lattice_build(panel_prototypes, 2, panel_period, panel_lower, panel_upper);
	if (!pillars || !wall) {
		fprintf(stderr, "failed to build lattice\n");
		return 1;
	}

	/* Camera looking down the pillar corridor.  The cross product of the
	 * two image-plane vectors is the view direction; here it is -Z. */
	v3 camera_position = { 0.f, 3.2f, 40.f };
	v3 camera_vectors[2] = { { -1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f } };
	camera_init(camera_position, camera_vectors, 60.f, 1.f);

	/* Frame setup mirrors src/raytracer/image.c. */
	image.resolution[X] = WIDTH;
	image.resolution[Y] = HEIGHT;
	image.pixels = (size_t)WIDTH * HEIGHT;
	float size_x = 2.f * camera.focal_length * tanf(camera.fov * PI / 360.f);
	float size_y = size_x * HEIGHT / WIDTH;
	v3 focal_vector, plane_center, corner_offset[2], step[2];
	mul3s(camera.vectors[2], camera.focal_length, focal_vector);
	add3v(focal_vector, camera.position, plane_center);
	mul3s(camera.vectors[0], size_x / WIDTH, step[X]);
	mul3s(camera.vectors[1], size_y / HEIGHT, step[Y]);
	mul3s(step[X], .5f - WIDTH / 2.f, corner_offset[X]);
	mul3s(step[Y], .5f - HEIGHT / 2.f, corner_offset[Y]);
	add3v3(plane_center, corner_offset[X], corner_offset[Y], image.corner);

	assign3(global_ambient_light_intensity, (v3){ 0.06f, 0.065f, 0.08f });

	/* Only one lattice can be active per frame, so build a combined pointer
	 * table: both lattices share the same opaque, non-emittant constraints.
	 * For the example we simply render the pillars first, then the wall, and
	 * add the two images.  (A real application would either merge the
	 * prototypes into a single lattice or alternate frames.) */
	unsigned char *pixels = xmalloc((size_t)WIDTH * HEIGHT * 3);
	float *accum = xmalloc((size_t)WIDTH * HEIGHT * 3 * sizeof(float));
	memset(accum, 0, (size_t)WIDTH * HEIGHT * 3 * sizeof(float));

	const v3 kr = { 1.f, 1.f, 1.f };
	int pass;
	for (pass = 0; pass < 2; pass++) {
		render_set_lattice(pass == 0 ? pillars : wall, 1000.f);
		size_t pixel = 0;
		uint32_t row, col;
		for (row = 0; row < HEIGHT; row++) {
			v3 pixel_position;
			mul3s(step[Y], (float)row, pixel_position);
			add3v(pixel_position, image.corner, pixel_position);
			for (col = 0; col < WIDTH; col++) {
				struct Ray ray;
				assign3(ray.point, camera.position);
				add3v(pixel_position, step[X], pixel_position);
				sub3v(pixel_position, camera.position, ray.direction);
				norm3(ray.direction);
				v3 color = { 0.f, 0.f, 0.f };
				cast_ray(&ray, kr, color, 3, NULL);
				for (size_t c = 0; c < 3; c++)
					accum[pixel * 3 + c] += color[c] * (pass == 0 ? 1.f : 1.f);
				pixel++;
			}
		}
	}
	render_clear_lattice();

	/* Write a binary PPM with simple tone mapping. */
	size_t i;
	for (i = 0; i < (size_t)WIDTH * HEIGHT * 3; i++) {
		float value = accum[i];
		value = value / (1.f + value);
		value = powf(value, 1.f / 2.2f);
		if (!(value > 0.f))
			value = 0.f;
		if (value > 1.f)
			value = 1.f;
		pixels[i] = (unsigned char)(value * 255.f + 0.5f);
	}

	FILE *file = fopen(filename, "wb");
	if (!file) {
		fprintf(stderr, "unable to open %s\n", filename);
		return 1;
	}
	fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
	fwrite(pixels, 1, (size_t)WIDTH * HEIGHT * 3, file);
	fclose(file);

	printf("wrote %s (%dx%d)\n", filename, WIDTH, HEIGHT);

	render_clear_lattice();
	lattice_free(pillars);
	lattice_free(wall);
	free(pillar_bottom);
	free(pillar_top);
	free(panel_0);
	free(panel_1);
	/* `lamp` and `ground` are owned by the object table. */
	accel_deinit();
	objects_deinit();
	free(stone_material);
	free(panel_material);
	free(lamp_material);
	free(ground_material);
	free(stone);
	free(panel);
	free(lamp_texture);
	free(accum);
	free(pixels);
	return 0;
}
