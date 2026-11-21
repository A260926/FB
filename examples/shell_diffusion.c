/*
 * Example: steady-state shell diffusion on a folded two-triangle strip.
 *
 * Builds two real triangle objects sharing an edge, solves a non-uniform
 * source, and checks both the vertex illumination far from the source and the
 * colour of a real cast_ray hit on one of the bound faces.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "argv.h"
#include "accel.h"
#include "calc.h"
#include "material.h"
#include "object.h"
#include "render.h"
#include "shell_diffusion.h"

float cast_ray(const struct Ray *ray, const v3 kr, v3 color, uint32_t remaining_bounces, struct Object *inside_object);

static struct Material material;

static struct Object *example_triangle(const v3 a, const v3 b, const v3 c)
{
	v3 vertices[3];
	assign3(vertices[0], a);
	assign3(vertices[1], b);
	assign3(vertices[2], c);
	struct Object *triangle = triangle_new(vertices);
	object_init(triangle, &material, 1e-6f, 0, OBJECT_TRIANGLE);
	triangle->object_data->postinit(triangle);
	return triangle;
}

int main(void)
{
	static char *dummy_argv[] = { "example", "-l", "none", NULL };
	myargc = 3;
	myargv = dummy_argv;
	argv_init();
	render_init();

	v3 zero = { 0.f, 0.f, 0.f };
	v3 albedo = { 0.5f, 0.35f, 0.2f };
	material_init(&material, 0, zero, zero, zero, zero, zero, 1.f, 1.f, texture_uniform_new(albedo));

	/* A folded strip: the two triangles share edge (1, 2) and are not
	 * coplanar, so the diffusion has to cross a crease. */
	static const v3 vertices[4] = {
		{ 0.f, 0.f, 0.f },
		{ 1.f, 0.f, 0.f },
		{ 0.f, 1.f, 0.6f },
		{ 0.f, -1.f, 0.6f },
	};
	static const uint32_t faces[2][3] = { { 0, 1, 2 }, { 1, 0, 3 } };

	/* Register the two triangles as real scene objects so the BVH can find
	 * them during cast_ray. */
	num_objects = 2;
	num_emittant_objects = 0;
	objects_init();
	objects[0] = example_triangle(vertices[0], vertices[1], vertices[2]);
	objects[1] = example_triangle(vertices[1], vertices[0], vertices[3]);
	accel_init();

	struct ShellDiffusionMesh *mesh = shell_diffusion_mesh_new(vertices, 4, faces, 2, objects, 2);
	if (!mesh) {
		puts("mesh construction failed");
		return 1;
	}

	/* Non-uniform source: bright at vertex 1, dark elsewhere. */
	v3 sources[4] = {
		{ 0.05f, 0.05f, 0.05f },
		{ 1.f, 1.f, 1.f },
		{ 0.2f, 0.2f, 0.2f },
		{ 0.05f, 0.05f, 0.05f },
	};
	struct ShellDiffusionFaceTensor tensors[2];
	int f, r, s;
	for (f = 0; f < 2; f++)
		for (r = 0; r < 3; r++)
			for (s = 0; s < 3; s++)
				tensors[f].D[r][s] = (r == s) ? (f ? 0.9f : 0.4f) : 0.f;

	const struct ShellDiffusionParams params = { 1e-10f, 2000u };
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(mesh, sources, tensors, &params);
	if (!solution) {
		puts("solve failed");
		return 1;
	}

	/* The far corner of the second (connected) triangle must pick up light
	 * from the source on the first one. */
	v3 far_value;
	shell_diffusion_solution_get_vertex(solution, 3, far_value);
	if (!(far_value[0] > sources[3][0] * 1.2f)) {
		printf("far vertex did not brighten: %f\n", (double)far_value[0]);
		return 1;
	}

	render_set_shell_diffusion_solution(solution);

	/* A real ray hitting the bound face blends the interpolated solution with
	 * the material texture. */
	struct Ray ray;
	assign3(ray.point, (v3){ 0.25f, 0.45f, 3.f });
	assign3(ray.direction, (v3){ 0.f, 0.f, -1.f });
	norm3(ray.direction);
	v3 color = { 0.f, 0.f, 0.f };
	v3 weight = { 1.f, 1.f, 1.f };
	float depth = cast_ray(&ray, weight, color, 1, NULL);
	if (!(depth > 0.f)) {
		puts("ray missed the bound strip");
		return 1;
	}

	v3 hit_point;
	mul3s(ray.direction, depth, hit_point);
	add3v(hit_point, ray.point, hit_point);
	v3 expected;
	if (!shell_diffusion_solution_interpolate(solution, objects[0], hit_point, expected)) {
		puts("interpolation failed");
		return 1;
	}
	const float expected_red = 0.5f * expected[0] / PI;
	if (!(fabsf(color[0] - expected_red) < 1e-3f)) {
		printf("ray colour mismatch: %f vs %f\n", (double)color[0], (double)expected_red);
		return 1;
	}
	printf("shell ray rgb=%.4f,%.4f,%.4f\n", (double)color[0], (double)color[1], (double)color[2]);

	render_clear_shell_diffusion_solution();
	shell_diffusion_solution_free(solution);
	shell_diffusion_mesh_free(mesh);
	accel_deinit();
	objects_deinit();
	free(material.texture);
	argv_deinit();

	puts("SHELL_DIFFUSION_CPU_OK");
	return 0;
}
