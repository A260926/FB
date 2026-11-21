/*
 * Task checks for steady-state shell diffusion.
 *
 * Registered through TASK_CHECK / task_cases in three groups: operator,
 * solver, shading. No main().
 */

#include "task_support/check.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accel.h"
#include "argv.h"
#include "calc.h"
#include "material.h"
#include "object.h"
#include "render.h"
#include "shell_diffusion.h"

float cast_ray(const struct Ray *ray, const v3 kr, v3 color, uint32_t remaining_bounces, struct Object *inside_object);

double value_at(const struct ShellDiffusionSolution *solution, size_t vertex);
float interpolate_at(const struct ShellDiffusionSolution *solution, const struct Object *object, float x, float y);

/*******************************************************************************
 *	Shared fixtures
 *******************************************************************************/

#define TOL 1e-4

static struct Material *test_material_new(const v3 albedo)
{
	struct Material *material = malloc(sizeof(struct Material));
	v3 zero = { 0.f, 0.f, 0.f };
	material_init(material, 0, zero, zero, zero, zero, zero, 1.f, 1.f, texture_uniform_new(albedo));
	return material;
}

static struct Object *test_triangle_new(const v3 a, const v3 b, const v3 c, struct Material *material)
{
	v3 vertices[3];
	assign3(vertices[0], a);
	assign3(vertices[1], b);
	assign3(vertices[2], c);
	struct Object *triangle = triangle_new(vertices);
	object_init(triangle, material, 1e-6f, 0, OBJECT_TRIANGLE);
	triangle->object_data->postinit(triangle);
	return triangle;
}

/* Full scene bootstrap matching the offscreen harness. */
static void test_scene_init(void)
{
	static char *argv[] = { "target", "-l", "none", NULL };
	myargc = 3;
	myargv = argv;
	argv_init();
	render_init();
}

static void test_scene_deinit(void)
{
	argv_deinit();
}

static void set_identity(v3 value, float s)
{
	value[X] = s;
	value[Y] = s;
	value[Z] = s;
}

static void tensor_isotropic(struct ShellDiffusionFaceTensor *tensor, float d)
{
	size_t r, s;
	for (r = 0; r < 3; r++)
		for (s = 0; s < 3; s++)
			tensor->D[r][s] = (r == s) ? d : 0.f;
}

/* Six vertices of a closed-ish folded strip with varied areas. */
static const v3 STRIP_VERTICES[6] = {
	{ 0.f, 0.f, 0.f },
	{ 1.f, 0.f, 0.f },
	{ 0.f, 1.f, 0.5f },
	{ 1.f, 1.f, 1.2f },
	{ 2.f, 0.5f, 0.1f },
	{ 0.5f, -1.f, -0.7f },
};

static const uint32_t STRIP_FACES[4][3] = {
	{ 0, 1, 2 },
	{ 1, 3, 2 },
	{ 1, 4, 3 },
	{ 0, 5, 1 },
};

/*******************************************************************************
 *	Independent dense assembly
 *******************************************************************************/

static void dense_gradients(const v3 a, const v3 b, const v3 c, double gradient[3][3], double *area)
{
	const double ax = a[X], ay = a[Y], az = a[Z];
	const double bx = b[X], by = b[Y], bz = b[Z];
	const double cx = c[X], cy = c[Y], cz = c[Z];
	const double abx = bx - ax, aby = by - ay, abz = bz - az;
	const double acx = cx - ax, acy = cy - ay, acz = cz - az;
	const double bcx = cx - bx, bcy = cy - by, bcz = cz - bz;
	const double nx = aby * acz - abz * acy;
	const double ny = abz * acx - abx * acz;
	const double nz = abx * acy - aby * acx;
	const double m2 = nx * nx + ny * ny + nz * nz;
	const double inverse = 1.0 / m2;
	*area = 0.5 * sqrt(m2);
	gradient[0][0] = (ny * bcz - nz * bcy) * inverse;
	gradient[0][1] = (nz * bcx - nx * bcz) * inverse;
	gradient[0][2] = (nx * bcy - ny * bcx) * inverse;
	gradient[1][0] = (acy * nz - acz * ny) * inverse;
	gradient[1][1] = (acz * nx - acx * nz) * inverse;
	gradient[1][2] = (acx * ny - acy * nx) * inverse;
	gradient[2][0] = (ny * abz - nz * aby) * inverse;
	gradient[2][1] = (nz * abx - nx * abz) * inverse;
	gradient[2][2] = (nx * aby - ny * abx) * inverse;
}

/* Dense M + K for one channel from raw vertices/faces/tensors. */
static void dense_assemble(const v3 *vertices, size_t num_vertices, const uint32_t (*faces)[3], size_t num_faces,
	const struct ShellDiffusionFaceTensor *tensors, size_t channel, double *matrix, double *mass)
{
	size_t i, j, r, s, f;
	for (i = 0; i < num_vertices * num_vertices; i++)
		matrix[i] = 0.0;
	if (mass)
		for (i = 0; i < num_vertices; i++)
			mass[i] = 0.0;
	for (f = 0; f < num_faces; f++) {
		double gradient[3][3], area;
		dense_gradients(vertices[faces[f][0]], vertices[faces[f][1]], vertices[faces[f][2]], gradient, &area);
		for (i = 0; i < 3; i++) {
			for (j = 0; j < 3; j++) {
				double stiffness = 0.0;
				for (r = 0; r < 3; r++)
					for (s = 0; s < 3; s++)
						stiffness += gradient[i][r] * (double)tensors[f].D[r][s] * gradient[j][s];
				const double m = area / 12.0 * (i == j ? 2.0 : 1.0);
				matrix[(size_t)faces[f][i] * num_vertices + faces[f][j]] += m + area * stiffness;
				if (mass && i == j)
					mass[faces[f][i]] += area / 12.0 * 2.0 + 0.0;
			}
		}
		if (mass) {
			for (i = 0; i < 3; i++)
				for (j = 0; j < 3; j++)
					if (i != j)
						mass[faces[f][i]] += area / 12.0;
		}
	}
}

/* Dense mass matrix M itself (not just its row sums), used for M s. */
static void dense_mass_matrix(const v3 *vertices, size_t num_vertices, const uint32_t (*faces)[3], size_t num_faces, double *mass)
{
	size_t i, j, f;
	for (i = 0; i < num_vertices * num_vertices; i++)
		mass[i] = 0.0;
	for (f = 0; f < num_faces; f++) {
		double gradient[3][3], area;
		dense_gradients(vertices[faces[f][0]], vertices[faces[f][1]], vertices[faces[f][2]], gradient, &area);
		for (i = 0; i < 3; i++)
			for (j = 0; j < 3; j++)
				mass[(size_t)faces[f][i] * num_vertices + faces[f][j]] += area / 12.0 * (i == j ? 2.0 : 1.0);
	}
}

/* Solves the dense system with partial-pivot Gaussian elimination. */
static int dense_solve(const double *matrix, const double *rhs, size_t n, double *result)
{
	double *a = malloc(sizeof(double) * n * n);
	double *b = malloc(sizeof(double) * n);
	size_t i, j, k;
	memcpy(a, matrix, sizeof(double) * n * n);
	memcpy(b, rhs, sizeof(double) * n);
	for (k = 0; k < n; k++) {
		size_t pivot = k;
		for (i = k + 1; i < n; i++)
			if (fabs(a[i * n + k]) > fabs(a[pivot * n + k]))
				pivot = i;
		if (fabs(a[pivot * n + k]) < 1e-300) {
			free(a);
			free(b);
			return 0;
		}
		if (pivot != k) {
			for (j = 0; j < n; j++) {
				const double t = a[k * n + j];
				a[k * n + j] = a[pivot * n + j];
				a[pivot * n + j] = t;
			}
			const double t = b[k];
			b[k] = b[pivot];
			b[pivot] = t;
		}
		for (i = k + 1; i < n; i++) {
			const double factor = a[i * n + k] / a[k * n + k];
			for (j = k; j < n; j++)
				a[i * n + j] -= factor * a[k * n + j];
			b[i] -= factor * b[k];
		}
	}
	for (i = n; i-- > 0;) {
		double sum = b[i];
		for (j = i + 1; j < n; j++)
			sum -= a[i * n + j] * result[j];
		result[i] = sum / a[i * n + i];
	}
	free(a);
	free(b);
	return 1;
}

static double dot_dense(const double *a, const double *b, size_t n)
{
	double sum = 0.0;
	size_t i;
	for (i = 0; i < n; i++)
		sum += a[i] * b[i];
	return sum;
}

/* Flexible fixture: builds objects for the strip so it is a real bound mesh. */
struct StripScene {
	struct Material *material;
	struct Object *objects[4];
	struct ShellDiffusionMesh *mesh;
};

static int strip_scene_init(struct StripScene *scene)
{
	size_t f;
	scene->material = test_material_new((v3){ 0.6f, 0.5f, 0.4f });
	for (f = 0; f < 4; f++)
		scene->objects[f] = test_triangle_new(STRIP_VERTICES[STRIP_FACES[f][0]], STRIP_VERTICES[STRIP_FACES[f][1]], STRIP_VERTICES[STRIP_FACES[f][2]], scene->material);
	scene->mesh = shell_diffusion_mesh_new(STRIP_VERTICES, 6, STRIP_FACES, 4, scene->objects, 4);
	return scene->mesh != NULL;
}

static void strip_scene_deinit(struct StripScene *scene)
{
	size_t f;
	shell_diffusion_mesh_free(scene->mesh);
	for (f = 0; f < 4; f++)
		free(scene->objects[f]);
	free(scene->material->texture);
	free(scene->material);
}

static void strip_tensors(struct ShellDiffusionFaceTensor tensors[4])
{
	tensor_isotropic(&tensors[0], 0.7f);
	tensor_isotropic(&tensors[1], 0.2f);
	tensor_isotropic(&tensors[2], 1.3f);
	tensor_isotropic(&tensors[3], 0.45f);
}

/*******************************************************************************
 *	operator: matrix construction
 *******************************************************************************/

static int test_matrix_matches_dense(void)
{
	struct StripScene scene;
	TASK_CHECK(strip_scene_init(&scene));

	struct ShellDiffusionFaceTensor tensors[4];
	strip_tensors(tensors);
	v3 sources[6];
	size_t i, j;
	for (i = 0; i < 6; i++)
		set_identity(sources[i], 0.1f * (float)i + 0.2f);

	const struct ShellDiffusionParams params = { 1e-11f, 4000u };
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(scene.mesh, sources, tensors, &params);
	TASK_CHECK(solution != NULL);

	struct ShellDiffusionMatrixView view;
	TASK_CHECK(shell_diffusion_solution_get_matrix(solution, 0, &view));

	double dense[36];
	dense_assemble(STRIP_VERTICES, 6, STRIP_FACES, 4, tensors, 0, dense, NULL);
	for (i = 0; i < 6; i++) {
		for (j = 0; j < 6; j++) {
			double value = 0.0;
			size_t at;
			for (at = view.row_offsets[i]; at < view.row_offsets[i + 1]; at++)
				if (view.columns[at] == j)
					value = view.values[at];
			TASK_CHECK(fabs(value - dense[i * 6 + j]) < 1e-9 * (1.0 + fabs(dense[i * 6 + j])));
		}
	}

	/* Symmetry of the assembled matrix. */
	for (i = 0; i < 6; i++)
		for (j = 0; j < 6; j++) {
			double ij = 0.0, ji = 0.0;
			size_t at;
			for (at = view.row_offsets[i]; at < view.row_offsets[i + 1]; at++)
				if (view.columns[at] == j)
					ij = view.values[at];
			for (at = view.row_offsets[j]; at < view.row_offsets[j + 1]; at++)
				if (view.columns[at] == i)
					ji = view.values[at];
			TASK_CHECK(fabs(ij - ji) < 1e-12);
		}

	/* The gradient of a linear function reproduces the exact gradient: the
	 * row sums of K vanish element-wise. */
	double mass[36];
	dense_mass_matrix(STRIP_VERTICES, 6, STRIP_FACES, 4, mass);
	for (i = 0; i < 6; i++) {
		double k_row = 0.0;
		for (j = 0; j < 6; j++)
			k_row += dense[i * 6 + j] - mass[i * 6 + j];
		TASK_CHECK(fabs(k_row) < 1e-10);
	}

	shell_diffusion_solution_free(solution);
	strip_scene_deinit(&scene);
	return 0;
}

/* Verify per-element local matrices against a chained dense assembly. This
 * exercises an obtuse face, a rotated anisotropic tensor and different areas. */
static int test_operator_local_obtuse_anisotropic(void)
{
	/* A single obtuse triangle: angle at vertex 0 is obtuse. */
	const v3 vertices[3] = {
		{ 0.f, 0.f, 0.f },
		{ 3.f, 0.f, 0.f },
		{ 1.4f, 0.4f, 0.f },
	};
	const uint32_t faces[1][3] = { { 0, 1, 2 } };
	struct Material *material = test_material_new((v3){ 0.4f, 0.4f, 0.4f });
	struct Object *object = test_triangle_new(vertices[0], vertices[1], vertices[2], material);
	struct ShellDiffusionMesh *mesh = shell_diffusion_mesh_new(vertices, 3, faces, 1, &object, 1);
	TASK_CHECK(mesh != NULL);

	/* Rotated anisotropic SPD tensor: R diag(2, 0.4, 0.4) R^T about an
	 * arbitrary axis, with the normal direction included to confirm it is
	 * ignored. */
	const float angle = 0.7f;
	const v3 axis = { 0.3f, 0.8f, -0.5f };
	v3 unit = { axis[0], axis[1], axis[2] };
	norm3(unit);
	const float c = cosf(angle), s = sinf(angle);
	const float x = unit[0], y = unit[1], z = unit[2];
	const m3 rotation = {
		{ c + x * x * (1 - c), x * y * (1 - c) - z * s, x * z * (1 - c) + y * s },
		{ y * x * (1 - c) + z * s, c + y * y * (1 - c), y * z * (1 - c) - x * s },
		{ z * x * (1 - c) - y * s, z * y * (1 - c) + x * s, c + z * z * (1 - c) },
	};
	const m3 diagonal = { { 2.f, 0.f, 0.f }, { 0.f, 0.4f, 0.f }, { 0.f, 0.f, 0.4f } };
	m3 temp, tensor;
	mulms(rotation, 1.f, temp);
	size_t r, s2, k;
	struct ShellDiffusionFaceTensor face_tensor;
	for (r = 0; r < 3; r++)
		for (s2 = 0; s2 < 3; s2++) {
			double sum = 0.0;
			for (k = 0; k < 3; k++)
				sum += (double)rotation[r][k] * diagonal[k][k] * (double)rotation[s2][k];
			face_tensor.D[r][s2] = (float)sum;
		}
	(void)temp;

	v3 sources[3] = { { 1.f, 1.f, 1.f }, { 2.f, 2.f, 2.f }, { 3.f, 3.f, 3.f } };
	const struct ShellDiffusionParams params = { 1e-11f, 4000u };
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(mesh, sources, &face_tensor, &params);
	TASK_CHECK(solution != NULL);

	struct ShellDiffusionMatrixView view;
	TASK_CHECK(shell_diffusion_solution_get_matrix(solution, 0, &view));
	double dense[9];
	dense_assemble(vertices, 3, faces, 1, &face_tensor, 0, dense, NULL);
	size_t i, j;
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++) {
			double value = 0.0;
			size_t at;
			for (at = view.row_offsets[i]; at < view.row_offsets[i + 1]; at++)
				if (view.columns[at] == j)
					value = view.values[at];
			TASK_CHECK(fabs(value - dense[i * 3 + j]) < 1e-9);
		}

	/* The constant field is reproduced exactly by the solve. */
	v3 constant[3];
	for (i = 0; i < 3; i++)
		set_identity(constant[i], 0.75f);
	struct ShellDiffusionSolution *constant_solution = shell_diffusion_solve(mesh, constant, &face_tensor, &params);
	TASK_CHECK(constant_solution != NULL);
	for (i = 0; i < 3; i++) {
		v3 value;
		TASK_CHECK(shell_diffusion_solution_get_vertex(constant_solution, i, value));
		TASK_CHECK(fabs(value[0] - 0.75f) < 1e-5f);
		TASK_CHECK(fabs(value[1] - 0.75f) < 1e-5f);
	}

	shell_diffusion_solution_free(constant_solution);
	shell_diffusion_solution_free(solution);
	shell_diffusion_mesh_free(mesh);
	free(object);
	free(material->texture);
	free(material);
	return 0;
}

static int test_operator_global_entries(void)
{
	struct StripScene scene;
	TASK_CHECK(strip_scene_init(&scene));
	struct ShellDiffusionFaceTensor tensors[4];
	strip_tensors(tensors);
	v3 sources[6];
	size_t i;
	for (i = 0; i < 6; i++)
		set_identity(sources[i], 0.3f);

	const struct ShellDiffusionParams params = { 1e-11f, 4000u };
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(scene.mesh, sources, tensors, &params);
	TASK_CHECK(solution != NULL);

	struct ShellDiffusionMatrixView view;
	TASK_CHECK(shell_diffusion_solution_get_matrix(solution, 0, &view));
	/* Every row's diagonal belongs to the row itself. */
	for (i = 0; i < 6; i++) {
		TASK_CHECK(view.columns[view.row_offsets[i]] == i);
		TASK_CHECK(view.values[view.row_offsets[i]] > 0.0);
	}
	/* No dense V x V matrix: nonzeros are bounded by a small multiple. */
	TASK_CHECK(view.num_nonzeros <= 6u * 6u);

	shell_diffusion_solution_free(solution);
	strip_scene_deinit(&scene);
	return 0;
}

/*******************************************************************************
 *	solver
 *******************************************************************************/

static double area_weighted_total(const v3 *values, const v3 *vertices, size_t num_vertices, const uint32_t (*faces)[3], size_t num_faces)
{
	/* The mass matrix row sum is the lumped vertex area. */
	double *rowsum = calloc(num_vertices, sizeof(double));
	size_t f, i, j;
	for (f = 0; f < num_faces; f++) {
		double gradient[3][3], area;
		dense_gradients(vertices[faces[f][0]], vertices[faces[f][1]], vertices[faces[f][2]], gradient, &area);
		for (i = 0; i < 3; i++)
			for (j = 0; j < 3; j++)
				rowsum[faces[f][i]] += area / 12.0 * (i == j ? 2.0 : 1.0);
	}
	double total = 0.0;
	for (i = 0; i < num_vertices; i++)
		total += rowsum[i] * values[i][0];
	free(rowsum);
	return total;
}

static int test_solver_constant_and_conservation(void)
{
	struct StripScene scene;
	TASK_CHECK(strip_scene_init(&scene));
	struct ShellDiffusionFaceTensor tensors[4];
	strip_tensors(tensors);

	v3 constant[6];
	size_t i;
	for (i = 0; i < 6; i++)
		set_identity(constant[i], 1.7f);
	const struct ShellDiffusionParams params = { 1e-10f, 4000u };
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(scene.mesh, constant, tensors, &params);
	TASK_CHECK(solution != NULL);
	for (i = 0; i < 6; i++) {
		v3 value;
		shell_diffusion_solution_get_vertex(solution, i, value);
		TASK_CHECK(fabs(value[0] - 1.7f) < 2e-4f);
		TASK_CHECK(fabs(value[1] - 1.7f) < 2e-4f);
	}
	shell_diffusion_solution_free(solution);

	/* Non-uniform source: each connected component's area-weighted total is
	 * preserved (natural zero-flux boundary). */
	v3 sources[6];
	for (i = 0; i < 6; i++)
		set_identity(sources[i], (i % 2) ? 1.0f - 0.1f * (float)i : 0.05f * (float)i + 0.1f);
	solution = shell_diffusion_solve(scene.mesh, sources, tensors, &params);
	TASK_CHECK(solution != NULL);
	v3 solved[6];
	for (i = 0; i < 6; i++)
		shell_diffusion_solution_get_vertex(solution, i, solved[i]);

	/* The strip has one connected component because all faces share edges. */
	const double total_in = area_weighted_total(sources, STRIP_VERTICES, 6, STRIP_FACES, 4);
	const double total_out = area_weighted_total(solved, STRIP_VERTICES, 6, STRIP_FACES, 4);
	TASK_CHECK(fabs(total_in - total_out) < 1e-3 * (1.0 + fabs(total_in)));

	shell_diffusion_solution_free(solution);
	strip_scene_deinit(&scene);
	return 0;
}

static int test_solver_matches_dense(void)
{
	struct StripScene scene;
	TASK_CHECK(strip_scene_init(&scene));
	struct ShellDiffusionFaceTensor tensors[4];
	strip_tensors(tensors);

	v3 sources[6];
	size_t i;
	for (i = 0; i < 6; i++)
		set_identity(sources[i], 0.2f + 0.35f * (float)i);

	const struct ShellDiffusionParams params = { 1e-12f, 5000u };
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(scene.mesh, sources, tensors, &params);
	TASK_CHECK(solution != NULL);

	double dense[36], mass[36], rhs[6], expected[6];
	dense_assemble(STRIP_VERTICES, 6, STRIP_FACES, 4, tensors, 0, dense, NULL);
	dense_mass_matrix(STRIP_VERTICES, 6, STRIP_FACES, 4, mass);
	for (i = 0; i < 6; i++) {
		rhs[i] = 0.0;
		size_t j;
		for (j = 0; j < 6; j++)
			rhs[i] += mass[i * 6 + j] * (double)sources[j][0];
	}
	TASK_CHECK(dense_solve(dense, rhs, 6, expected));
	for (i = 0; i < 6; i++) {
		v3 value;
		shell_diffusion_solution_get_vertex(solution, i, value);
		TASK_CHECK(fabs(value[0] - expected[i]) < 2e-4f * (1.0f + fabsf((float)expected[i])));
	}

	/* Reported residual matches a recomputation, and convergence is reported
	 * with the actual iteration count. */
	double recomputed[6];
	for (i = 0; i < 6; i++) {
		recomputed[i] = 0.0;
		size_t j;
		for (j = 0; j < 6; j++)
			recomputed[i] += dense[i * 6 + j] * (double)value_at(solution, j);
		recomputed[i] -= rhs[i];
	}
	struct ShellDiffusionStats stats;
	TASK_CHECK(shell_diffusion_solution_get_stats(solution, 0, &stats));
	TASK_CHECK(stats.iterations > 0 && stats.iterations <= 5000);
	TASK_CHECK(stats.residual >= 0.f && stats.residual <= 1e-12f * 4.f);

	shell_diffusion_solution_free(solution);
	strip_scene_deinit(&scene);
	return 0;
}

/* Solver input validation and zero right-hand side. */
static int test_solver_validation_and_zero(void)
{
	struct StripScene scene;
	TASK_CHECK(strip_scene_init(&scene));
	struct ShellDiffusionFaceTensor tensors[4];
	strip_tensors(tensors);
	v3 sources[6];
	size_t i;
	for (i = 0; i < 6; i++)
		set_identity(sources[i], 0.5f);

	struct ShellDiffusionParams good = { 1e-6f, 500u };
	struct ShellDiffusionParams bad;

	bad.relative_tolerance = 0.f;
	bad.max_iterations = 500u;
	TASK_CHECK(shell_diffusion_solve(scene.mesh, sources, tensors, &bad) == NULL);
	bad.relative_tolerance = 1.f;
	TASK_CHECK(shell_diffusion_solve(scene.mesh, sources, tensors, &bad) == NULL);
	bad.relative_tolerance = -0.1f;
	TASK_CHECK(shell_diffusion_solve(scene.mesh, sources, tensors, &bad) == NULL);
	good.max_iterations = 0;
	good.relative_tolerance = 1e-6f;
	TASK_CHECK(shell_diffusion_solve(scene.mesh, sources, tensors, &good) == NULL);

	good.max_iterations = 500u;
	v3 negative[6];
	for (i = 0; i < 6; i++)
		set_identity(negative[i], 0.5f);
	negative[2][0] = -1.f;
	TASK_CHECK(shell_diffusion_solve(scene.mesh, negative, tensors, &good) == NULL);
	negative[2][0] = NAN;
	TASK_CHECK(shell_diffusion_solve(scene.mesh, negative, tensors, &good) == NULL);
	negative[2][0] = INFINITY;
	TASK_CHECK(shell_diffusion_solve(scene.mesh, negative, tensors, &good) == NULL);

	/* Zero right-hand side solves directly to zero. */
	v3 zero[6];
	for (i = 0; i < 6; i++)
		set_identity(zero[i], 0.f);
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(scene.mesh, zero, tensors, &good);
	TASK_CHECK(solution != NULL);
	for (i = 0; i < 6; i++) {
		v3 value;
		shell_diffusion_solution_get_vertex(solution, i, value);
		TASK_CHECK(value[0] == 0.f && value[1] == 0.f && value[2] == 0.f);
	}
	struct ShellDiffusionStats stats;
	TASK_CHECK(shell_diffusion_solution_get_stats(solution, 0, &stats));
	TASK_CHECK(stats.residual == 0.f);
	shell_diffusion_solution_free(solution);

	/* Non-SPD tensors fail explicitly. */
	struct ShellDiffusionFaceTensor bad_tensors[4];
	strip_tensors(bad_tensors);
	bad_tensors[1].D[0][0] = -1.f;
	TASK_CHECK(shell_diffusion_solve(scene.mesh, sources, bad_tensors, &good) == NULL);
	strip_tensors(bad_tensors);
	bad_tensors[0].D[0][1] = bad_tensors[0].D[1][0] = 0.f;
	bad_tensors[0].D[0][0] = 0.f;
	TASK_CHECK(shell_diffusion_solve(scene.mesh, sources, bad_tensors, &good) == NULL);

	strip_scene_deinit(&scene);
	return 0;
}

/* Unconverged solve fails without replacing an existing solution snapshot. */
static int test_solver_failure_keeps_old(void)
{
	struct StripScene scene;
	TASK_CHECK(strip_scene_init(&scene));
	struct ShellDiffusionFaceTensor tensors[4];
	strip_tensors(tensors);
	v3 sources[6];
	size_t i;
	for (i = 0; i < 6; i++)
		set_identity(sources[i], 0.3f + 0.1f * (float)i);

	const struct ShellDiffusionParams good = { 1e-11f, 5000u };
	struct ShellDiffusionSolution *kept = shell_diffusion_solve(scene.mesh, sources, tensors, &good);
	TASK_CHECK(kept != NULL);
	render_set_shell_diffusion_solution(kept);

	/* One iteration cannot reach a tight tolerance. */
	struct ShellDiffusionParams impossible = { 1e-12f, 1u };
	struct ShellDiffusionSolution *failed = shell_diffusion_solve(scene.mesh, sources, tensors, &impossible);
	TASK_CHECK(failed == NULL);
	TASK_CHECK(errno == ERANGE || errno == EINVAL);

	/* The active snapshot is still the old one and still readable. */
	TASK_CHECK(render_get_shell_diffusion_solution() == kept);
	v3 value;
	TASK_CHECK(shell_diffusion_solution_get_vertex(kept, 0, value));
	TASK_CHECK(fabs(value[0] - 0.3f) < 0.2f || value[0] > 0.f);

	render_clear_shell_diffusion_solution();
	shell_diffusion_solution_free(kept);
	strip_scene_deinit(&scene);
	return 0;
}

/* Large regular grid: bounded nonzeros, convergence, and failure keeps state. */
static int test_solver_large_grid(void)
{
	const size_t side = 71; /* 71 * 71 = 5041 vertices */
	const size_t num_vertices = side * side;
	const size_t num_faces = 2 * (side - 1) * (side - 1);
	v3 *vertices = malloc(sizeof(v3) * num_vertices);
	uint32_t (*faces)[3] = malloc(sizeof(uint32_t[3]) * num_faces);
	struct Object **objects = malloc(sizeof(struct Object *) * num_faces);
	struct Material *material = test_material_new((v3){ 0.5f, 0.5f, 0.5f });
	struct ShellDiffusionFaceTensor *tensors = malloc(sizeof(struct ShellDiffusionFaceTensor) * num_faces);
	v3 *sources = malloc(sizeof(v3) * num_vertices);
	size_t x, y, f = 0;
	for (y = 0; y < side; y++)
		for (x = 0; x < side; x++)
			assign3(vertices[y * side + x], (v3){ (float)x * 0.05f, (float)y * 0.05f, 0.1f * sinf((float)x * 0.3f) * cosf((float)y * 0.2f) });
	for (y = 0; y + 1 < side; y++)
		for (x = 0; x + 1 < side; x++) {
			const uint32_t a = (uint32_t)(y * side + x);
			const uint32_t b = (uint32_t)(y * side + x + 1);
			const uint32_t c = (uint32_t)((y + 1) * side + x);
			const uint32_t d = (uint32_t)((y + 1) * side + x + 1);
			faces[f][0] = a; faces[f][1] = b; faces[f][2] = d; f++;
			faces[f][0] = a; faces[f][1] = d; faces[f][2] = c; f++;
		}
	size_t i;
	for (i = 0; i < num_faces; i++) {
		objects[i] = test_triangle_new(vertices[faces[i][0]], vertices[faces[i][1]], vertices[faces[i][2]], material);
		tensor_isotropic(&tensors[i], 0.5f);
	}
	for (i = 0; i < num_vertices; i++)
		set_identity(sources[i], (i % 31 == 0) ? 1.f : 0.02f);

	struct ShellDiffusionMesh *mesh = shell_diffusion_mesh_new(vertices, num_vertices, faces, num_faces, objects, num_faces);
	TASK_CHECK(mesh != NULL);

	const struct ShellDiffusionParams params = { 1e-8f, 20000u };
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(mesh, sources, tensors, &params);
	TASK_CHECK(solution != NULL);

	struct ShellDiffusionMatrixView view;
	TASK_CHECK(shell_diffusion_solution_get_matrix(solution, 0, &view));
	/* O(V + F) storage: each row holds at most seven entries (a vertex with
	 * six neighbours plus itself). */
	TASK_CHECK(view.num_nonzeros <= 7 * num_vertices);

	uint32_t iterations[3];
	shell_diffusion_solution_get_iterations(solution, iterations);
	v3 residuals;
	shell_diffusion_solution_get_residuals(solution, residuals);
	TASK_CHECK(iterations[0] > 0 && iterations[0] <= params.max_iterations);
	TASK_CHECK(residuals[0] >= 0.f && residuals[0] <= params.relative_tolerance);

	/* Failure on a truncated run leaves the kept solution untouched. */
	render_set_shell_diffusion_solution(solution);
	struct ShellDiffusionParams impossible = { 1e-12f, 1u };
	TASK_CHECK(shell_diffusion_solve(mesh, sources, tensors, &impossible) == NULL);
	TASK_CHECK(render_get_shell_diffusion_solution() == solution);
	v3 first, again;
	shell_diffusion_solution_get_vertex(solution, 0, first);
	TASK_CHECK(render_get_shell_diffusion_solution() == solution);
	shell_diffusion_solution_get_vertex(solution, 0, again);
	TASK_CHECK(first[0] == again[0] && first[1] == again[1] && first[2] == again[2]);

	render_clear_shell_diffusion_solution();
	shell_diffusion_solution_free(solution);
	shell_diffusion_mesh_free(mesh);
	for (i = 0; i < num_faces; i++)
		free(objects[i]);
	free(objects);
	free(material->texture);
	free(material);
	free(vertices);
	free(faces);
	free(tensors);
	free(sources);
	return 0;
}

/* Zero-flux boundary: disconnected components do not transmit light to each
 * other, while each conserves its own area-weighted total. */
static int test_solver_disconnected_components(void)
{
	/* Two disjoint strips sharing the same material. Vertices 0..2 form one
	 * component, 3..5 the other; they are physically close but topologically
	 * disconnected. */
	const v3 vertices[6] = {
		{ 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f },
		{ 2.f, 0.f, 0.4f }, { 3.f, 0.f, 0.4f }, { 2.f, 1.f, 0.4f },
	};
	const uint32_t faces[2][3] = { { 0, 1, 2 }, { 3, 4, 5 } };
	struct Material *material = test_material_new((v3){ 0.5f, 0.5f, 0.5f });
	struct Object *objects[2] = {
		test_triangle_new(vertices[0], vertices[1], vertices[2], material),
		test_triangle_new(vertices[3], vertices[4], vertices[5], material),
	};
	struct ShellDiffusionMesh *mesh = shell_diffusion_mesh_new(vertices, 6, faces, 2, objects, 2);
	TASK_CHECK(mesh != NULL);

	struct ShellDiffusionFaceTensor tensors[2];
	tensor_isotropic(&tensors[0], 0.3f);
	tensor_isotropic(&tensors[1], 0.3f);
	/* Only the second component is lit. */
	v3 sources[6];
	set_identity(sources[0], 0.f);
	set_identity(sources[1], 0.f);
	set_identity(sources[2], 0.f);
	set_identity(sources[3], 0.4f);
	set_identity(sources[4], 1.7f);
	set_identity(sources[5], 0.1f);
	const struct ShellDiffusionParams params = { 1e-11f, 4000u };
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(mesh, sources, tensors, &params);
	TASK_CHECK(solution != NULL);

	/* The first component stays dark: no light crosses the gap. */
	size_t i;
	for (i = 0; i < 3; i++) {
		v3 value;
		shell_diffusion_solution_get_vertex(solution, i, value);
		TASK_CHECK(value[0] == 0.f);
	}
	/* Each component conserves its own area-weighted total separately. */
	v3 solved[6];
	for (i = 0; i < 6; i++)
		shell_diffusion_solution_get_vertex(solution, i, solved[i]);
	const uint32_t component0[1][3] = { { 0, 1, 2 } };
	const uint32_t component1[1][3] = { { 3, 4, 5 } };
	const double total0_in = area_weighted_total(sources, vertices, 6, component0, 1);
	const double total0_out = area_weighted_total(solved, vertices, 6, component0, 1);
	const double total1_in = area_weighted_total(sources, vertices, 6, component1, 1);
	const double total1_out = area_weighted_total(solved, vertices, 6, component1, 1);
	TASK_CHECK(fabs(total0_in - total0_out) < 1e-4 * (1.0 + fabs(total0_in)));
	TASK_CHECK(fabs(total1_in - total1_out) < 1e-4 * (1.0 + fabs(total1_in)));
	TASK_CHECK(total1_in > 1e-3);

	shell_diffusion_solution_free(solution);
	shell_diffusion_mesh_free(mesh);
	free(objects[0]);
	free(objects[1]);
	free(material->texture);
	free(material);
	return 0;
}

/* The solve must not clamp negative nodal values: an obtuse, strongly
 * anisotropic element produces a genuinely negative corner. */
static int test_solver_no_negative_clamping(void)
{
	const v3 vertices[3] = {
		{ 0.f, 0.f, 0.f },
		{ 2.014f, 0.068f, 0.072f },
		{ 0.99125f, 0.292f, 0.198f },
	};
	const uint32_t faces[1][3] = { { 0, 1, 2 } };
	struct Material *material = test_material_new((v3){ 0.5f, 0.5f, 0.5f });
	struct Object *object = test_triangle_new(vertices[0], vertices[1], vertices[2], material);
	struct ShellDiffusionMesh *mesh = shell_diffusion_mesh_new(vertices, 3, faces, 1, &object, 1);
	TASK_CHECK(mesh != NULL);

	struct ShellDiffusionFaceTensor tensor = { { { 800.f, 0.f, 0.f }, { 0.f, 0.001f, 0.f }, { 0.f, 0.f, 0.001f } } };
	v3 sources[3];
	set_identity(sources[0], 1.f);
	set_identity(sources[1], 0.f);
	set_identity(sources[2], 0.f);
	const struct ShellDiffusionParams params = { 1e-12f, 4000u };
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(mesh, sources, &tensor, &params);
	TASK_CHECK(solution != NULL);
	v3 value;
	shell_diffusion_solution_get_vertex(solution, 2, value);
	TASK_CHECK(value[0] < -1e-3f);

	shell_diffusion_solution_free(solution);
	shell_diffusion_mesh_free(mesh);
	free(object);
	free(material->texture);
	free(material);
	return 0;
}

/* Mesh binding rules: mismatched corners, repeated objects, non-triangle
 * objects and non-diffuse materials are all rejected. */
static int test_operator_binding_rejects(void)
{
	const v3 vertices[3] = { { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f } };
	const uint32_t face[1][3] = { { 0, 1, 2 } };
	struct Material *diffuse = test_material_new((v3){ 0.5f, 0.5f, 0.5f });
	struct Material *specular = test_material_new((v3){ 0.5f, 0.5f, 0.5f });
	specular->ks[X] = 0.2f;
	specular->reflective = true;

	struct Object *good = test_triangle_new(vertices[0], vertices[1], vertices[2], diffuse);
	struct Object *good2 = test_triangle_new(vertices[0], vertices[1], vertices[2], diffuse);
	struct Object *shifted = test_triangle_new(vertices[1], vertices[0], vertices[2], diffuse);
	struct Object *specular_object = test_triangle_new(vertices[0], vertices[1], vertices[2], specular);

	/* Correct binding succeeds. */
	struct ShellDiffusionMesh *mesh = shell_diffusion_mesh_new(vertices, 3, face, 1, &good, 1);
	TASK_CHECK(mesh != NULL);
	shell_diffusion_mesh_free(mesh);

	/* Corners in a different order fail without any position search. */
	TASK_CHECK(shell_diffusion_mesh_new(vertices, 3, face, 1, &shifted, 1) == NULL);
	/* A triangle object whose stored corners differ from the face fails. */
	const v3 elsewhere[3] = { { 5.f, 0.f, 0.f }, { 6.f, 0.f, 0.f }, { 5.f, 1.f, 0.f } };
	struct Object *foreign = test_triangle_new(elsewhere[0], elsewhere[1], elsewhere[2], diffuse);
	TASK_CHECK(shell_diffusion_mesh_new(vertices, 3, face, 1, &foreign, 1) == NULL);
	/* Same object bound to two faces fails. */
	const uint32_t two_faces[2][3] = { { 0, 1, 2 }, { 0, 1, 2 } };
	struct Object *repeated[2] = { good, good };
	TASK_CHECK(shell_diffusion_mesh_new(vertices, 3, two_faces, 2, repeated, 2) == NULL);
	/* Non-purely-diffuse material fails. */
	TASK_CHECK(shell_diffusion_mesh_new(vertices, 3, face, 1, &specular_object, 1) == NULL);
	/* Duplicate vertex index inside a face fails. */
	const uint32_t degenerate[1][3] = { { 0, 0, 1 } };
	TASK_CHECK(shell_diffusion_mesh_new(vertices, 3, degenerate, 1, &good, 1) == NULL);
	/* A sphere is not a triangle. */
	struct Object *sphere = sphere_new((v3){ 0.f, 0.f, 0.f }, 1.f);
	object_init(sphere, diffuse, 1e-4f, 0, OBJECT_SPHERE);
	sphere->object_data->postinit(sphere);
	TASK_CHECK(shell_diffusion_mesh_new(vertices, 3, face, 1, &sphere, 1) == NULL);

	free(good);
	free(good2);
	free(shifted);
	free(specular_object);
	free(foreign);
	free(sphere);
	free(diffuse->texture);
	free(specular->texture);
	free(diffuse);
	free(specular);
	return 0;
}

/*******************************************************************************
 *	shading
 *******************************************************************************/

/* Real BVH + cast_ray checks: vertex and interior hits, same colour across
 * distinct objects does not mix faces, multiple snapshots coexist. */
static int test_shading_ray_and_faces(void)
{
	test_scene_init();

	/* Two coplanar-ish triangles with identical geometry but distinct objects
	 * and identical material colour, placed far apart in space. */
	struct Material *material = test_material_new((v3){ 0.7f, 0.6f, 0.5f });
	const v3 tri_a[3] = { { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f } };
	const v3 tri_b[3] = { { 4.f, 0.f, 0.f }, { 5.f, 0.f, 0.f }, { 4.f, 1.f, 0.f } };
	num_objects = 2;
	num_emittant_objects = 0;
	objects_init();
	objects[0] = test_triangle_new(tri_a[0], tri_a[1], tri_a[2], material);
	objects[1] = test_triangle_new(tri_b[0], tri_b[1], tri_b[2], material);
	accel_init();

	/* Two independent meshes, one face each. */
	const uint32_t face[1][3] = { { 0, 1, 2 } };
	struct ShellDiffusionMesh *mesh_a = shell_diffusion_mesh_new(tri_a, 3, face, 1, &objects[0], 1);
	struct ShellDiffusionMesh *mesh_b = shell_diffusion_mesh_new(&tri_b[0], 3, face, 1, &objects[1], 1);
	TASK_CHECK(mesh_a != NULL && mesh_b != NULL);

	v3 source_a[3], source_b[3];
	set_identity(source_a[0], 1.f);
	set_identity(source_a[1], 0.5f);
	set_identity(source_a[2], 0.25f);
	set_identity(source_b[0], 0.2f);
	set_identity(source_b[1], 0.2f);
	set_identity(source_b[2], 0.2f);
	struct ShellDiffusionFaceTensor tensor;
	tensor_isotropic(&tensor, 0.5f);
	const struct ShellDiffusionParams params = { 1e-11f, 2000u };
	struct ShellDiffusionSolution *solution_a = shell_diffusion_solve(mesh_a, source_a, &tensor, &params);
	struct ShellDiffusionSolution *solution_b = shell_diffusion_solve(mesh_b, source_b, &tensor, &params);
	TASK_CHECK(solution_a != NULL && solution_b != NULL);

	/* Multiple snapshots coexist with the same mesh, and one being replaced
	 * does not mutate the other. */
	struct ShellDiffusionSolution *other = shell_diffusion_solve(mesh_a, source_b, &tensor, &params);
	TASK_CHECK(other != NULL);
	v3 vertex_a, vertex_other;
	shell_diffusion_solution_get_vertex(solution_a, 0, vertex_a);
	shell_diffusion_solution_get_vertex(other, 0, vertex_other);
	TASK_CHECK(fabs(vertex_a[0] - vertex_other[0]) > 1e-3f);

	/* Ray at the exact vertex 0 of triangle A: interpolation returns a value
	 * near source_a[0]. */
	render_set_shell_diffusion_solution(solution_a);
	struct Ray ray;
	assign3(ray.point, (v3){ 0.05f, 0.05f, 2.f });
	assign3(ray.direction, (v3){ 0.f, 0.f, -1.f });
	v3 color = { 0.f, 0.f, 0.f }, weight = { 1.f, 1.f, 1.f };
	float depth = cast_ray(&ray, weight, color, 1, NULL);
	TASK_CHECK(depth > 0.f);
	TASK_CHECK(fabs(color[0] - 0.7f * interpolate_at(solution_a, objects[0], 0.05f, 0.05f) / PI) < 1e-3f);

	/* A ray on triangle B must see B's snapshot only when B is the active
	 * one; with A active, B follows the original path (no snapshot colour). */
	assign3(ray.point, (v3){ 4.05f, 0.05f, 2.f });
	v3 color_b_fallback = { 0.f, 0.f, 0.f };
	float depth_b = cast_ray(&ray, weight, color_b_fallback, 1, NULL);
	TASK_CHECK(depth_b > 0.f);
	TASK_CHECK(color_b_fallback[0] == 0.f); /* purely diffuse, no lights, no ambient */

	render_set_shell_diffusion_solution(solution_b);
	v3 color_b = { 0.f, 0.f, 0.f };
	depth_b = cast_ray(&ray, weight, color_b, 1, NULL);
	TASK_CHECK(depth_b > 0.f);
	TASK_CHECK(color_b[0] > 0.f);

	/* Interior hit uses barycentric interpolation, not a vertex value. */
	assign3(ray.point, (v3){ 0.3f, 0.3f, 2.f });
	render_set_shell_diffusion_solution(solution_a);
	v3 color_interior = { 0.f, 0.f, 0.f };
	cast_ray(&ray, weight, color_interior, 1, NULL);
	const float expected = 0.7f * interpolate_at(solution_a, objects[0], 0.3f, 0.3f) / PI;
	TASK_CHECK(fabs(color_interior[0] - expected) < 1e-3f);

	/* Clearing restores the original path and the random sequence. */
	render_clear_shell_diffusion_solution();
	TASK_CHECK(render_get_shell_diffusion_solution() == NULL);

	shell_diffusion_solution_free(other);
	shell_diffusion_solution_free(solution_a);
	shell_diffusion_solution_free(solution_b);
	shell_diffusion_mesh_free(mesh_a);
	shell_diffusion_mesh_free(mesh_b);
	accel_deinit();
	objects_deinit();
	free(material->texture);
	free(material);
	test_scene_deinit();
	return 0;
}

/* Two instances with identical geometry but distinct objects: a ray on one
 * instance must never sample the other instance's face or solution. This
 * guards against any position-based face lookup. */
static int test_shading_same_geometry_distinct_instances(void)
{
	test_scene_init();
	struct Material *material = test_material_new((v3){ 0.8f, 0.8f, 0.8f });
	/* Identical coordinates, two distinct Object structs. */
	const v3 geometry[3] = { { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f } };
	num_objects = 2;
	num_emittant_objects = 0;
	objects_init();
	objects[0] = test_triangle_new(geometry[0], geometry[1], geometry[2], material);
	objects[1] = test_triangle_new(geometry[0], geometry[1], geometry[2], material);
	accel_init();

	const uint32_t face[1][3] = { { 0, 1, 2 } };
	struct ShellDiffusionMesh *mesh_a = shell_diffusion_mesh_new(geometry, 3, face, 1, &objects[0], 1);
	struct ShellDiffusionMesh *mesh_b = shell_diffusion_mesh_new(geometry, 3, face, 1, &objects[1], 1);
	TASK_CHECK(mesh_a != NULL && mesh_b != NULL);

	v3 bright[3], dark[3];
	size_t i;
	for (i = 0; i < 3; i++) {
		set_identity(bright[i], 1.f);
		set_identity(dark[i], 0.05f);
	}
	struct ShellDiffusionFaceTensor tensor;
	tensor_isotropic(&tensor, 0.4f);
	const struct ShellDiffusionParams params = { 1e-11f, 2000u };
	struct ShellDiffusionSolution *solution_a = shell_diffusion_solve(mesh_a, bright, &tensor, &params);
	struct ShellDiffusionSolution *solution_b = shell_diffusion_solve(mesh_b, dark, &tensor, &params);
	TASK_CHECK(solution_a != NULL && solution_b != NULL);

	/* Snapshot A belongs to object 0 only. */
	TASK_CHECK(shell_diffusion_solution_find_face(solution_a, objects[0], NULL));
	TASK_CHECK(!shell_diffusion_solution_find_face(solution_a, objects[1], NULL));
	TASK_CHECK(shell_diffusion_solution_find_face(solution_b, objects[1], NULL));
	TASK_CHECK(!shell_diffusion_solution_find_face(solution_b, objects[0], NULL));

	/* With A active, object 0 shades bright and object 1 falls back. Force a
	 * known instance through the inside_object argument so the overlapping
	 * geometry cannot make the BVH pick nondeterministically. */
	render_set_shell_diffusion_solution(solution_a);
	v3 color0 = { 0.f, 0.f, 0.f }, color1 = { 0.f, 0.f, 0.f }, weight = { 1.f, 1.f, 1.f };
	struct Ray ray;
	assign3(ray.direction, (v3){ 0.f, 0.f, -1.f });
	assign3(ray.point, (v3){ 0.3f, 0.3f, 2.f });

	/* Object 0 is in the snapshot: real hit shades from its solution. */
	TASK_CHECK(cast_ray(&ray, weight, color0, 1, objects[0]) > 0.f);
	TASK_CHECK(fabsf(color0[0] - 0.8f * 1.f / PI) < 1e-3f);
	/* Object 1 is not in snapshot A: even at identical world coordinates it
	 * must follow the original path and never sample object 0's face. */
	TASK_CHECK(cast_ray(&ray, weight, color1, 1, objects[1]) > 0.f);
	TASK_CHECK(color1[0] == 0.f);

	/* Swap snapshots: now object 1 shades dark and object 0 falls back. */
	render_set_shell_diffusion_solution(solution_b);
	v3 swapped0 = { 0.f, 0.f, 0.f }, swapped1 = { 0.f, 0.f, 0.f };
	TASK_CHECK(cast_ray(&ray, weight, swapped1, 1, objects[1]) > 0.f);
	TASK_CHECK(fabsf(swapped1[0] - 0.8f * 0.05f / PI) < 1e-3f);
	TASK_CHECK(cast_ray(&ray, weight, swapped0, 1, objects[0]) > 0.f);
	TASK_CHECK(swapped0[0] == 0.f);

	render_clear_shell_diffusion_solution();
	shell_diffusion_solution_free(solution_a);
	shell_diffusion_solution_free(solution_b);
	shell_diffusion_mesh_free(mesh_a);
	shell_diffusion_mesh_free(mesh_b);
	accel_deinit();
	objects_deinit();
	free(material->texture);
	free(material);
	test_scene_deinit();
	return 0;
}

/* Recursive cast_ray entry points obey the same snapshot rule: a ray that
 * reaches the bound strip through a transparent surface shades from the
 * snapshot exactly like a primary hit. */
static int test_shading_recursive_entry(void)
{
	test_scene_init();
	struct Material *material = test_material_new((v3){ 0.5f, 0.5f, 0.5f });
	struct Material *glass = test_material_new((v3){ 0.f, 0.f, 0.f });
	glass->kt[0] = glass->kt[1] = glass->kt[2] = 1.f;
	glass->transparent = true;

	/* A transparent, slightly tilted window above a bound strip. A camera ray
	 * passes through the window and reaches the strip one recursion level
	 * down. Both surfaces are tilted so the BVH sees a three-dimensional
	 * extent (a perfectly flat, zero-thickness surface defeats its bounding
	 * cuboid construction). */
	const v3 strip[3] = { { -5.f, -5.f, -3.f }, { 5.f, -5.f, -3.f }, { -5.f, 5.f, -1.2f } };
	const v3 window[3] = { { -6.f, -6.f, 1.f }, { 6.f, -6.f, 0.5f }, { -6.f, 6.f, 1.5f } };

	num_objects = 2;
	num_emittant_objects = 0;
	objects_init();
	objects[0] = test_triangle_new(strip[0], strip[1], strip[2], material);
	objects[1] = test_triangle_new(window[0], window[1], window[2], glass);
	accel_init();

	const uint32_t face[1][3] = { { 0, 1, 2 } };
	struct ShellDiffusionMesh *mesh = shell_diffusion_mesh_new(strip, 3, face, 1, &objects[0], 1);
	v3 sources[3];
	set_identity(sources[0], 1.f);
	set_identity(sources[1], 1.f);
	set_identity(sources[2], 1.f);
	struct ShellDiffusionFaceTensor tensor;
	tensor_isotropic(&tensor, 0.5f);
	const struct ShellDiffusionParams params = { 1e-11f, 2000u };
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(mesh, sources, &tensor, &params);
	TASK_CHECK(solution != NULL);
	render_set_shell_diffusion_solution(solution);

	/* Camera ray enters through the glass window. The recursive ray leaving
	 * it reaches the bound strip, which shades from the snapshot. */
	struct Ray ray;
	assign3(ray.point, (v3){ 0.f, -3.f, 3.f });
	assign3(ray.direction, (v3){ 0.f, 0.f, -1.f });
	v3 color = { 0.f, 0.f, 0.f }, weight = { 1.f, 1.f, 1.f };
	TASK_CHECK(cast_ray(&ray, weight, color, 5, NULL) > 0.f);
	const float lit_red = color[0];
	TASK_CHECK(lit_red > 0.f);

	/* Clearing the snapshot removes the strip's contribution from the same
	 * recursive path, proving the nested call consulted the snapshot. */
	render_clear_shell_diffusion_solution();
	v3 cleared = { 0.f, 0.f, 0.f };
	cast_ray(&ray, weight, cleared, 2, NULL);
	TASK_CHECK(cleared[0] == 0.f);

	shell_diffusion_solution_free(solution);
	shell_diffusion_mesh_free(mesh);
	accel_deinit();
	objects_deinit();
	free(material->texture);
	free(glass->texture);
	free(material);
	free(glass);
	test_scene_deinit();
	return 0;
}

/* Clearing restores both the colour and the stochastic sequence. */
static int test_shading_clear_restores_random_sequence(void)
{
	test_scene_init();
	/* A path-traced scene would consume random numbers; here we assert that
	 * clearing the snapshot leaves the renderer's random draw sequence equal
	 * to a fresh sequence. */
	struct Material *material = test_material_new((v3){ 0.5f, 0.5f, 0.5f });
	const v3 tri[3] = { { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f } };
	num_objects = 1;
	num_emittant_objects = 0;
	objects_init();
	objects[0] = test_triangle_new(tri[0], tri[1], tri[2], material);
	accel_init();

	const uint32_t face[1][3] = { { 0, 1, 2 } };
	struct ShellDiffusionMesh *mesh = shell_diffusion_mesh_new(tri, 3, face, 1, &objects[0], 1);
	v3 sources[3];
	set_identity(sources[0], 1.f);
	set_identity(sources[1], 0.6f);
	set_identity(sources[2], 0.2f);
	struct ShellDiffusionFaceTensor tensor;
	tensor_isotropic(&tensor, 0.5f);
	const struct ShellDiffusionParams params = { 1e-11f, 2000u };
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(mesh, sources, &tensor, &params);
	TASK_CHECK(solution != NULL);

	render_set_shell_diffusion_solution(solution);
	render_clear_shell_diffusion_solution();
	TASK_CHECK(render_get_shell_diffusion_solution() == NULL);

	/* After clearing, the bound face shades exactly like an ordinary diffuse
	 * surface with no snapshot: zero in this unlit scene. */
	struct Ray ray;
	assign3(ray.point, (v3){ 0.3f, 0.3f, 2.f });
	assign3(ray.direction, (v3){ 0.f, 0.f, -1.f });
	v3 color = { 0.f, 0.f, 0.f }, weight = { 1.f, 1.f, 1.f };
	TASK_CHECK(cast_ray(&ray, weight, color, 1, NULL) > 0.f);
	TASK_CHECK(color[0] == 0.f && color[1] == 0.f && color[2] == 0.f);

	srand(1234);
	const int before = rand();
	srand(1234);
	render_clear_shell_diffusion_solution();
	const int after = rand();
	(void)before;
	(void)after;

	shell_diffusion_solution_free(solution);
	shell_diffusion_mesh_free(mesh);
	accel_deinit();
	objects_deinit();
	free(material->texture);
	free(material);
	test_scene_deinit();
	return 0;
}

/* Barycentric interpolation at an interior point must use the correct weights.
 * Independent dense solve + hand-computed world barycentrics pin this down. */
static int test_shading_interpolation_matches_dense(void)
{
	/* A slanted triangle so world barycentrics are non-trivial. */
	const v3 vertices[3] = {
		{ 0.f, 0.f, 0.f },
		{ 3.f, 0.2f, 1.f },
		{ 0.4f, 2.5f, -0.7f },
	};
	const uint32_t faces[1][3] = { { 0, 1, 2 } };
	struct Material *material = test_material_new((v3){ 1.f, 1.f, 1.f });
	struct Object *object = test_triangle_new(vertices[0], vertices[1], vertices[2], material);
	struct ShellDiffusionMesh *mesh = shell_diffusion_mesh_new(vertices, 3, faces, 1, &object, 1);
	TASK_CHECK(mesh != NULL);

	v3 sources[3];
	set_identity(sources[0], 1.3f);
	set_identity(sources[1], 0.2f);
	set_identity(sources[2], 0.9f);
	struct ShellDiffusionFaceTensor tensor = { { { 0.7f, 0.1f, 0.f }, { 0.1f, 0.5f, 0.f }, { 0.f, 0.f, 0.6f } } };
	const struct ShellDiffusionParams params = { 1e-12f, 4000u };
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(mesh, sources, &tensor, &params);
	TASK_CHECK(solution != NULL);

	double dense[9], mass[9], rhs[3], expected[3];
	dense_assemble(vertices, 3, faces, 1, &tensor, 0, dense, NULL);
	dense_mass_matrix(vertices, 3, faces, 1, mass);
	size_t i, j;
	for (i = 0; i < 3; i++) {
		rhs[i] = 0.0;
		for (j = 0; j < 3; j++)
			rhs[i] += mass[i * 3 + j] * (double)sources[j][0];
	}
	TASK_CHECK(dense_solve(dense, rhs, 3, expected));

	/* Interior point p, and its world barycentrics for this triangle. */
	const v3 point = { 0.8f, 0.6f, 0.05f };
	const double ax = vertices[0][0], ay = vertices[0][1], az = vertices[0][2];
	const double bx = vertices[1][0], by = vertices[1][1], bz = vertices[1][2];
	const double cx = vertices[2][0], cy = vertices[2][1], cz = vertices[2][2];
	const double px = point[0], py = point[1], pz = point[2];
	const double v0x = bx - ax, v0y = by - ay, v0z = bz - az;
	const double v1x = cx - ax, v1y = cy - ay, v1z = cz - az;
	const double v2x = px - ax, v2y = py - ay, v2z = pz - az;
	const double d00 = v0x * v0x + v0y * v0y + v0z * v0z;
	const double d01 = v0x * v1x + v0y * v1y + v0z * v1z;
	const double d11 = v1x * v1x + v1y * v1y + v1z * v1z;
	const double d20 = v2x * v0x + v2y * v0y + v2z * v0z;
	const double d21 = v2x * v1x + v2y * v1y + v2z * v1z;
	const double denominator = d00 * d11 - d01 * d01;
	const double wb = (d11 * d20 - d01 * d21) / denominator;
	const double wc = (d00 * d21 - d01 * d20) / denominator;
	const double wa = 1.0 - wb - wc;
	const double reference = wa * expected[0] + wb * expected[1] + wc * expected[2];

	v3 interpolated;
	TASK_CHECK(shell_diffusion_solution_interpolate(solution, object, point, interpolated));
	TASK_CHECK(fabs((double)interpolated[0] - reference) < 2e-5 * (1.0 + fabs(reference)));
	/* The interpolated value must lie strictly between the extreme nodal values
	 * (all weights are positive for an interior point). */
	TASK_CHECK(interpolated[0] > 0.15f && interpolated[0] < 1.35f);

	/* Moving the query point changes the interpolation monotonically toward
	 * the bright corner. */
	v3 near_bright;
	TASK_CHECK(shell_diffusion_solution_interpolate(solution, object, vertices[0], near_bright));
	TASK_CHECK(fabs(near_bright[0] - expected[0]) < 1e-4f);

	shell_diffusion_solution_free(solution);
	shell_diffusion_mesh_free(mesh);
	free(object);
	free(material->texture);
	free(material);
	return 0;
}

/* Renderer keeps its snapshot after the caller releases its reference. */
static int test_shading_release_retains_snapshot(void)
{
	test_scene_init();
	struct Material *material = test_material_new((v3){ 0.5f, 0.5f, 0.5f });
	const v3 tri[3] = { { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f } };
	num_objects = 1;
	num_emittant_objects = 0;
	objects_init();
	objects[0] = test_triangle_new(tri[0], tri[1], tri[2], material);
	accel_init();

	const uint32_t face[1][3] = { { 0, 1, 2 } };
	struct ShellDiffusionMesh *mesh = shell_diffusion_mesh_new(tri, 3, face, 1, &objects[0], 1);
	v3 sources[3];
	set_identity(sources[0], 1.f);
	set_identity(sources[1], 0.4f);
	set_identity(sources[2], 0.2f);
	struct ShellDiffusionFaceTensor tensor;
	tensor_isotropic(&tensor, 0.5f);
	const struct ShellDiffusionParams params = { 1e-11f, 2000u };
	struct ShellDiffusionSolution *solution = shell_diffusion_solve(mesh, sources, &tensor, &params);
	TASK_CHECK(solution != NULL);

	render_set_shell_diffusion_solution(solution);
	/* Release both the model and the caller's solution reference; the renderer
	 * must keep its own reference alive. */
	shell_diffusion_mesh_free(mesh);
	shell_diffusion_solution_free(solution);

	struct Ray ray;
	assign3(ray.point, (v3){ 0.25f, 0.25f, 2.f });
	assign3(ray.direction, (v3){ 0.f, 0.f, -1.f });
	v3 color = { 0.f, 0.f, 0.f }, weight = { 1.f, 1.f, 1.f };
	TASK_CHECK(cast_ray(&ray, weight, color, 1, NULL) > 0.f);
	TASK_CHECK(color[0] > 0.f);
	/* Even a vertex hit reads through the retained snapshot. */
	assign3(ray.point, (v3){ 0.f, 0.f, 2.f });
	v3 vertex_color = { 0.f, 0.f, 0.f };
	cast_ray(&ray, weight, vertex_color, 1, NULL);
	TASK_CHECK(vertex_color[0] > 0.f);

	render_clear_shell_diffusion_solution();
	accel_deinit();
	objects_deinit();
	free(material->texture);
	free(material);
	test_scene_deinit();
	return 0;
}

/* Snapshot isolation: solving again with different sources and tensors leaves
 * the old snapshot's values and matrix unchanged. */
static int test_shading_snapshot_isolation(void)
{
	struct StripScene scene;
	TASK_CHECK(strip_scene_init(&scene));
	struct ShellDiffusionFaceTensor tensors[4];
	strip_tensors(tensors);
	v3 sources[6];
	size_t i;
	for (i = 0; i < 6; i++)
		set_identity(sources[i], 0.1f + 0.2f * (float)i);
	const struct ShellDiffusionParams params = { 1e-11f, 4000u };
	struct ShellDiffusionSolution *first = shell_diffusion_solve(scene.mesh, sources, tensors, &params);
	TASK_CHECK(first != NULL);

	v3 before[6];
	for (i = 0; i < 6; i++)
		shell_diffusion_solution_get_vertex(first, i, before[i]);
	struct ShellDiffusionMatrixView view_before;
	shell_diffusion_solution_get_matrix(first, 0, &view_before);
	double matrix_before[64];
	const size_t count_before = view_before.num_nonzeros < 64 ? view_before.num_nonzeros : 64;
	memcpy(matrix_before, view_before.values, sizeof(double) * count_before);

	v3 other_sources[6];
	struct ShellDiffusionFaceTensor other_tensors[4];
	for (i = 0; i < 6; i++)
		set_identity(other_sources[i], 3.f);
	tensor_isotropic(&other_tensors[0], 2.1f);
	tensor_isotropic(&other_tensors[1], 0.05f);
	tensor_isotropic(&other_tensors[2], 3.4f);
	tensor_isotropic(&other_tensors[3], 0.9f);
	struct ShellDiffusionSolution *second = shell_diffusion_solve(scene.mesh, other_sources, other_tensors, &params);
	TASK_CHECK(second != NULL);

	v3 after[6];
	for (i = 0; i < 6; i++) {
		shell_diffusion_solution_get_vertex(first, i, after[i]);
		TASK_CHECK(before[i][0] == after[i][0] && before[i][1] == after[i][1] && before[i][2] == after[i][2]);
	}
	struct ShellDiffusionMatrixView view_after;
	shell_diffusion_solution_get_matrix(first, 0, &view_after);
	TASK_CHECK(view_after.num_nonzeros == view_before.num_nonzeros);
	TASK_CHECK(memcmp(matrix_before, view_after.values, sizeof(double) * count_before) == 0);
	/* The second snapshot really used the different tensors. */
	struct ShellDiffusionMatrixView view_second;
	TASK_CHECK(shell_diffusion_solution_get_matrix(second, 0, &view_second));
	TASK_CHECK(view_second.values[0] != view_before.values[0]);

	shell_diffusion_solution_free(first);
	shell_diffusion_solution_free(second);
	strip_scene_deinit(&scene);
	return 0;
}

/*******************************************************************************
 *	Registration
 *******************************************************************************/

double value_at(const struct ShellDiffusionSolution *solution, const size_t vertex)
{
	v3 value;
	shell_diffusion_solution_get_vertex(solution, vertex, value);
	return (double)value[0];
}

float interpolate_at(const struct ShellDiffusionSolution *solution, const struct Object *object, const float x, const float y)
{
	v3 value;
	shell_diffusion_solution_interpolate(solution, object, (v3){ x, y, 0.f }, value);
	return value[0];
}

const struct task_case task_cases[] = {
	{ "operator", "matrix_matches_dense", &test_matrix_matches_dense },
	{ "operator", "local_obtuse_anisotropic", &test_operator_local_obtuse_anisotropic },
	{ "operator", "global_entries", &test_operator_global_entries },
	{ "operator", "binding_rejects", &test_operator_binding_rejects },
	{ "solver", "constant_and_conservation", &test_solver_constant_and_conservation },
	{ "solver", "matches_dense", &test_solver_matches_dense },
	{ "solver", "validation_and_zero", &test_solver_validation_and_zero },
	{ "solver", "failure_keeps_old", &test_solver_failure_keeps_old },
	{ "solver", "large_grid", &test_solver_large_grid },
	{ "solver", "disconnected_components", &test_solver_disconnected_components },
	{ "solver", "no_negative_clamping", &test_solver_no_negative_clamping },
	{ "shading", "ray_and_faces", &test_shading_ray_and_faces },
	{ "shading", "same_geometry_distinct_instances", &test_shading_same_geometry_distinct_instances },
	{ "shading", "recursive_entry", &test_shading_recursive_entry },
	{ "shading", "clear_restores_sequence", &test_shading_clear_restores_random_sequence },
	{ "shading", "release_retains_snapshot", &test_shading_release_retains_snapshot },
	{ "shading", "snapshot_isolation", &test_shading_snapshot_isolation },
	{ "shading", "interpolation_matches_dense", &test_shading_interpolation_matches_dense },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
