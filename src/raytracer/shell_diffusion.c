/*
 * Copyright (c) 2021-2022 Wojciech Graj
 *
 * Licensed under the MIT license: https://opensource.org/licenses/MIT
 * Permission is granted to use, copy, modify, and redistribute the work.
 * Full license information available in the project LICENSE file.
 *
 * DESCRIPTION:
 *   Steady-state surface scattering on thin-shell triangle meshes
 **/

#include "shell_diffusion.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "calc.h"
#include "material.h"
#include "mem.h"
#include "object.h"
#include "render.h"

#define SHELL_CHANNELS 3
#define SHELL_CORNERS 3
#define SHELL_NO_FACE SIZE_MAX

/* A face is an element when its corners span a non-negligible area. */
#define SHELL_MIN_DOUBLE_AREA (8.0 * (double)FLT_MIN)

enum {
	SHELL_CHANNEL_X = 0,
	SHELL_CHANNEL_Y,
	SHELL_CHANNEL_Z,
};

struct ShellDiffusionMesh {
	v3 *vertices;
	size_t num_vertices;
	uint32_t (*faces)[3];
	size_t num_faces;
	struct Object **objects; /* one per face, never owned */
	size_t *face_element; /* element index for face, or SHELL_NO_FACE */
	size_t *element_faces; /* element index -> face index */
	size_t num_elements;
	size_t references; /* caller reference plus one per live solution */
};

/* Compressed sparse row matrix. All rows are sorted and hold their diagonal
 * entry, so the Jacobi preconditioner is well defined. */
struct SparseMatrix {
	size_t num_rows;
	size_t num_nonzeros;
	size_t *row_offsets;
	uint32_t *columns;
	double *values;
};

struct ShellDiffusionSolution {
	struct ShellDiffusionMesh *mesh; /* shared ownership with other references */
	size_t references;
	v3 *vertices;
	struct ShellDiffusionStats stats[SHELL_CHANNELS];
	struct SparseMatrix *matrix;
};

static bool finite3(const v3 value);
static bool material_is_pure_diffuse(const struct Material *material);
static bool face_has_distinct_indices(const uint32_t face[3]);
static bool face_is_element(const v3 a, const v3 b, const v3 c, double *area, double gradient[SHELL_CORNERS][3]);
static bool tensor_is_spd(const struct ShellDiffusionFaceTensor *tensor);

static void matrix_free(struct SparseMatrix *matrix);
static bool matrix_build(const struct ShellDiffusionMesh *mesh, struct SparseMatrix *matrix);
static bool matrix_assemble(const struct ShellDiffusionMesh *mesh, const struct ShellDiffusionFaceTensor *tensors, struct SparseMatrix *matrix);
static size_t matrix_entry(const struct SparseMatrix *matrix, size_t row, uint32_t column);
static void matrix_multiply(const struct SparseMatrix *matrix, const double *x, double *y);
static bool cg_solve(const struct SparseMatrix *matrix, const double *rhs, double tolerance, uint32_t max_iterations,
	double *x, struct ShellDiffusionStats *stats);

static bool mesh_retain(struct ShellDiffusionMesh *mesh);
static void mesh_release(struct ShellDiffusionMesh *mesh);
static void solution_release(struct ShellDiffusionSolution *solution);

static bool shell_sample(const struct ShellDiffusionSolution *solution, const struct Object *object, const v3 point, v3 illumination);
static void *shell_bridge_retain(void *solution);
static void shell_bridge_release(void *solution);
static bool shell_bridge_sample(const void *solution, const struct Object *object, const v3 point, v3 illumination);

/*******************************************************************************
 *	Validation
 *******************************************************************************/

static bool finite3(const v3 value)
{
	return isfinite(value[X]) && isfinite(value[Y]) && isfinite(value[Z]);
}

/* Bound triangles may only carry a purely diffuse configuration; any non-zero
 * ks, kr, kt or ka would double-count light. */
static bool material_is_pure_diffuse(const struct Material *material)
{
	if (!material)
		return false;
	return material->ks[X] == 0.f && material->ks[Y] == 0.f && material->ks[Z] == 0.f
		&& material->kr[X] == 0.f && material->kr[Y] == 0.f && material->kr[Z] == 0.f
		&& material->kt[X] == 0.f && material->kt[Y] == 0.f && material->kt[Z] == 0.f
		&& material->ka[X] == 0.f && material->ka[Y] == 0.f && material->ka[Z] == 0.f;
}

static bool face_has_distinct_indices(const uint32_t face[3])
{
	return face[0] != face[1] && face[1] != face[2] && face[2] != face[0];
}

/* Real world-space area and the three P1 gradients
 * grad(phi_i) = (n x opposite_edge_i) / |n|^2, so that sum_i grad(phi_i) = 0
 * and every gradient is orthogonal to the face normal. The gradients are kept
 * in double precision because they enter the stiffness matrix, whose entries
 * must match an independent dense assembly to near round-off. */
static bool face_is_element(const v3 a, const v3 b, const v3 c, double *area, double gradient[SHELL_CORNERS][3])
{
	/* All geometry is promoted to double up front: the stiffness assembly is
	 * sensitive to cancellation, and the exact gradient identity
	 * sum_i grad(phi_i) = 0 must hold to round-off so constants are preserved. */
	const double ax = (double)a[X], ay = (double)a[Y], az = (double)a[Z];
	const double bx = (double)b[X], by = (double)b[Y], bz = (double)b[Z];
	const double cx = (double)c[X], cy = (double)c[Y], cz = (double)c[Z];
	const double abx = bx - ax, aby = by - ay, abz = bz - az;
	const double acx = cx - ax, acy = cy - ay, acz = cz - az;
	const double bcx = cx - bx, bcy = cy - by, bcz = cz - bz;

	const double nx = aby * acz - abz * acy;
	const double ny = abz * acx - abx * acz;
	const double nz = abx * acy - aby * acx;
	const double magnitude_sqr = nx * nx + ny * ny + nz * nz;
	if (!isfinite(magnitude_sqr) || !(magnitude_sqr > SHELL_MIN_DOUBLE_AREA * SHELL_MIN_DOUBLE_AREA))
		return false;

	/* grad(phi_0) = (n x (c - b)) / |n|^2, and cyclically; they sum to zero,
	 * which also keeps constant fields exact. */
	const double inverse = 1.0 / magnitude_sqr;
	gradient[0][0] = (ny * bcz - nz * bcy) * inverse;
	gradient[0][1] = (nz * bcx - nx * bcz) * inverse;
	gradient[0][2] = (nx * bcy - ny * bcx) * inverse;
	gradient[1][0] = (acy * nz - acz * ny) * inverse;
	gradient[1][1] = (acz * nx - acx * nz) * inverse;
	gradient[1][2] = (acx * ny - acy * nx) * inverse;
	gradient[2][0] = (ny * abz - nz * aby) * inverse;
	gradient[2][1] = (nz * abx - nx * abz) * inverse;
	gradient[2][2] = (nx * aby - ny * abx) * inverse;

	*area = 0.5 * sqrt(magnitude_sqr);
	return true;
}

/* Symmetric positive definiteness via Sylvester's criterion, evaluated in
 * double so that small but valid tensors are not rejected by float rounding. */
static bool tensor_is_spd(const struct ShellDiffusionFaceTensor *tensor)
{
	size_t i, j;
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			if (!isfinite(tensor->D[i][j]))
				return false;

	const double a = (double)tensor->D[0][0];
	const double b = (double)tensor->D[0][1];
	const double c = (double)tensor->D[0][2];
	const double d = (double)tensor->D[1][1];
	const double e = (double)tensor->D[1][2];
	const double f = (double)tensor->D[2][2];

	const double minor1 = a;
	const double minor2 = a * d - b * b;
	const double minor3 = a * (d * f - e * e) - b * (b * f - c * e) + c * (b * e - c * d);
	return minor1 > 0.0 && minor2 > 0.0 && minor3 > 0.0;
}

/*******************************************************************************
 *	Sparse matrix
 *******************************************************************************/

static void matrix_free(struct SparseMatrix *matrix)
{
	if (!matrix)
		return;
	free(matrix->row_offsets);
	free(matrix->columns);
	free(matrix->values);
	free(matrix);
}

/* Shared sparsity pattern: the row of every element vertex owns the three
 * element vertices. Rows are sorted and duplicate-free. O(V + F). */
static bool matrix_build(const struct ShellDiffusionMesh *mesh, struct SparseMatrix *matrix)
{
	const size_t num_vertices = mesh->num_vertices;
	size_t e, k, i;

	matrix->num_rows = num_vertices;
	matrix->row_offsets = safe_malloc(sizeof(size_t) * (num_vertices + 1));
	uint64_t *count = safe_calloc(num_vertices + 1, sizeof(uint64_t));
	for (e = 0; e < mesh->num_elements; e++) {
		const uint32_t *face = mesh->faces[mesh->element_faces[e]];
		for (k = 0; k < SHELL_CORNERS; k++)
			count[face[k] + 1] += SHELL_CORNERS;
	}
	matrix->row_offsets[0] = 0;
	for (k = 0; k < num_vertices; k++)
		matrix->row_offsets[k + 1] = matrix->row_offsets[k] + count[k + 1];
	free(count);

	const size_t raw_nnz = matrix->row_offsets[num_vertices];
	uint32_t *raw = safe_malloc(sizeof(uint32_t) * raw_nnz);
	size_t *fill = safe_malloc(sizeof(size_t) * num_vertices);
	memcpy(fill, matrix->row_offsets, sizeof(size_t) * num_vertices);
	for (e = 0; e < mesh->num_elements; e++) {
		const uint32_t *face = mesh->faces[mesh->element_faces[e]];
		for (k = 0; k < SHELL_CORNERS; k++) {
			const uint32_t row = face[k];
			for (i = 0; i < SHELL_CORNERS; i++)
				raw[fill[row]++] = face[i];
		}
	}
	free(fill);

	size_t nnz = 0;
	size_t *offsets = safe_malloc(sizeof(size_t) * (num_vertices + 1));
	offsets[0] = 0;
	for (k = 0; k < num_vertices; k++) {
		const size_t begin = matrix->row_offsets[k];
		const size_t end = matrix->row_offsets[k + 1];
		size_t at;
		/* Each row segment holds at most three entries per incident element;
		 * an insertion sort keeps it sorted. */
		for (at = begin + 1; at < end; at++) {
			const uint32_t value = raw[at];
			size_t pos = at;
			while (pos > begin && raw[pos - 1] > value) {
				raw[pos] = raw[pos - 1];
				pos--;
			}
			raw[pos] = value;
		}
		for (at = begin; at < end; at++)
			if (at == begin || raw[at] != raw[at - 1])
				raw[nnz++] = raw[at];
		offsets[k + 1] = nnz;
	}

	matrix->num_nonzeros = nnz;
	matrix->columns = safe_malloc(sizeof(uint32_t) * nnz);
	memcpy(matrix->columns, raw, sizeof(uint32_t) * nnz);
	matrix->values = safe_calloc(nnz, sizeof(double));
	free(raw);
	free(matrix->row_offsets);
	matrix->row_offsets = offsets;

	/* Force each row's diagonal to the front so the Jacobi preconditioner can
	 * read it without searching. */
	for (k = 0; k < num_vertices; k++) {
		const size_t begin = matrix->row_offsets[k];
		const size_t end = matrix->row_offsets[k + 1];
		size_t at;
		for (at = begin; at < end; at++) {
			if (matrix->columns[at] == (uint32_t)k) {
				if (at != begin) {
					const uint32_t column = matrix->columns[begin];
					matrix->columns[begin] = matrix->columns[at];
					matrix->columns[at] = column;
				}
				break;
			}
		}
	}
	return true;
}

static size_t matrix_entry(const struct SparseMatrix *matrix, const size_t row, const uint32_t column)
{
	size_t at;
	for (at = matrix->row_offsets[row]; at < matrix->row_offsets[row + 1]; at++)
		if (matrix->columns[at] == column)
			return at;
	return SIZE_MAX;
}

static void matrix_multiply(const struct SparseMatrix *matrix, const double *x, double *y)
{
	size_t row, at;
	for (row = 0; row < matrix->num_rows; row++) {
		double sum = 0.0;
		for (at = matrix->row_offsets[row]; at < matrix->row_offsets[row + 1]; at++)
			sum += matrix->values[at] * x[matrix->columns[at]];
		y[row] = sum;
	}
}

/* Assembles M + K. The local mass matrix is A/12 * [[2,1,1],[1,2,1],[1,1,2]]
 * and the local stiffness is A * grad(phi_i) . D grad(phi_j) with the full
 * world-space tensor. Since the P1 gradients lie inside the face plane, the
 * normal component of D contributes nothing and needs no explicit projection. */
static bool matrix_assemble(const struct ShellDiffusionMesh *mesh, const struct ShellDiffusionFaceTensor *tensors, struct SparseMatrix *matrix)
{
	if (!matrix_build(mesh, matrix))
		return false;

	size_t e, i, j;
	for (e = 0; e < mesh->num_elements; e++) {
		const size_t face = mesh->element_faces[e];
		const uint32_t *indices = mesh->faces[face];
		double area;
		double gradient[SHELL_CORNERS][3];
		if (!face_is_element(mesh->vertices[indices[0]], mesh->vertices[indices[1]], mesh->vertices[indices[2]], &area, gradient))
			return false;

		const float *tensor = tensors[face].D[0];
		const double mass_diagonal = area / 12.0 * 2.0;
		const double mass_offdiagonal = area / 12.0;

		for (i = 0; i < SHELL_CORNERS; i++) {
			for (j = 0; j < SHELL_CORNERS; j++) {
				double stiffness = 0.0;
				size_t r, s;
				for (r = 0; r < 3; r++)
					for (s = 0; s < 3; s++)
						stiffness += gradient[i][r] * (double)tensor[r * 3 + s] * gradient[j][s];
				const double mass = (i == j) ? mass_diagonal : mass_offdiagonal;
				const size_t at = matrix_entry(matrix, indices[i], indices[j]);
				if (at == SIZE_MAX)
					return false;
				matrix->values[at] += mass + area * stiffness;
			}
		}
	}
	return true;
}

/*******************************************************************************
 *	Conjugate gradients
 *******************************************************************************/

/* Jacobi-preconditioned CG on the SPD matrix. It starts from zero, iterates
 * until the recomputed relative residual reaches `tolerance` or the positive
 * iteration cap is hit, and always reports the recomputed residual. A zero
 * right-hand side yields the exact zero solution and zero reported residual. */
static bool cg_solve(const struct SparseMatrix *matrix, const double *rhs, const double tolerance,
	const uint32_t max_iterations, double *x, struct ShellDiffusionStats *stats)
{
	const size_t n = matrix->num_rows;
	double *residual = safe_malloc(sizeof(double) * n);
	double *preconditioned = safe_malloc(sizeof(double) * n);
	double *direction = safe_malloc(sizeof(double) * n);
	double *product = safe_malloc(sizeof(double) * n);
	double *inverse_diagonal = safe_malloc(sizeof(double) * n);
	double rhs_norm_sqr = 0.0;
	size_t row;

	for (row = 0; row < n; row++) {
		const double diagonal = matrix->values[matrix->row_offsets[row]];
		inverse_diagonal[row] = (diagonal > 0.0) ? 1.0 / diagonal : 1.0;
		rhs_norm_sqr += rhs[row] * rhs[row];
	}

	stats->iterations = 0;
	if (!(rhs_norm_sqr > 0.0)) {
		memset(x, 0, sizeof(double) * n);
		stats->residual = 0.f;
		free(residual);
		free(preconditioned);
		free(direction);
		free(product);
		free(inverse_diagonal);
		return true;
	}
	const double rhs_norm = sqrt(rhs_norm_sqr);

	double *solution = safe_calloc(n, sizeof(double));
	matrix_multiply(matrix, solution, product);
	for (row = 0; row < n; row++)
		residual[row] = rhs[row] - product[row];
	for (row = 0; row < n; row++)
		preconditioned[row] = inverse_diagonal[row] * residual[row];
	for (row = 0; row < n; row++)
		direction[row] = preconditioned[row];
	double delta = 0.0;
	for (row = 0; row < n; row++)
		delta += residual[row] * preconditioned[row];

	uint32_t iteration;
	for (iteration = 0; iteration < max_iterations; iteration++) {
		matrix_multiply(matrix, direction, product);

		double denominator = 0.0;
		for (row = 0; row < n; row++)
			denominator += direction[row] * product[row];
		if (!(denominator > 0.0))
			break;
		const double alpha = delta / denominator;

		for (row = 0; row < n; row++) {
			solution[row] += alpha * direction[row];
			residual[row] -= alpha * product[row];
		}
		stats->iterations = iteration + 1;

		double residual_sqr = 0.0;
		for (row = 0; row < n; row++)
			residual_sqr += residual[row] * residual[row];
		if (sqrt(residual_sqr) / rhs_norm <= tolerance)
			break;

		for (row = 0; row < n; row++)
			preconditioned[row] = inverse_diagonal[row] * residual[row];
		double delta_new = 0.0;
		for (row = 0; row < n; row++)
			delta_new += residual[row] * preconditioned[row];
		const double beta = delta_new / delta;
		delta = delta_new;
		for (row = 0; row < n; row++)
			direction[row] = preconditioned[row] + beta * direction[row];
	}

	/* Recompute the residual from scratch so the reported value never drifts
	 * from the solution that is actually returned. */
	matrix_multiply(matrix, solution, product);
	double recomputed_sqr = 0.0;
	for (row = 0; row < n; row++) {
		const double value = rhs[row] - product[row];
		recomputed_sqr += value * value;
	}
	const double relative_residual = sqrt(recomputed_sqr) / rhs_norm;
	stats->residual = (float)relative_residual;

	memcpy(x, solution, sizeof(double) * n);

	free(solution);
	free(residual);
	free(preconditioned);
	free(direction);
	free(product);
	free(inverse_diagonal);

	if (!isfinite(relative_residual))
		return false;
	return relative_residual <= tolerance;
}

/*******************************************************************************
 *	Mesh
 *******************************************************************************/

struct ShellDiffusionMesh *shell_diffusion_mesh_new(const v3 *vertices, const size_t num_vertices,
	const uint32_t (*faces)[3], const size_t num_faces, struct Object *const *object_table, const size_t object_count)
{
	errno = 0;
	if (!vertices || !num_vertices || !faces || !num_faces || !object_table || object_count != num_faces) {
		errno = EINVAL;
		return NULL;
	}

	size_t i, j;
	for (i = 0; i < num_vertices; i++) {
		if (!finite3(vertices[i])) {
			errno = EINVAL;
			return NULL;
		}
	}

	for (i = 0; i < num_faces; i++) {
		const uint32_t *face = faces[i];
		if (!face_has_distinct_indices(face)
			|| face[0] >= num_vertices || face[1] >= num_vertices || face[2] >= num_vertices) {
			errno = EINVAL;
			return NULL;
		}
		/* Every face binds its own real triangle; no object may repeat and no
		 * neighbouring or foreign instance may be substituted. */
		for (j = 0; j < i; j++)
			if (object_table[j] == object_table[i]) {
				errno = EINVAL;
				return NULL;
			}
		if (!object_table[i] || object_table[i]->object_data->type != OBJECT_TRIANGLE) {
			errno = EINVAL;
			return NULL;
		}
		if (!material_is_pure_diffuse(object_table[i]->material)) {
			errno = EINVAL;
			return NULL;
		}
		v3 corners[SHELL_CORNERS];
		if (!object_get_triangle_vertices(object_table[i], corners)) {
			errno = EINVAL;
			return NULL;
		}
		/* The face's vertices must equal the bound object's corners in the
		 * order given, without any position search. */
		for (j = 0; j < SHELL_CORNERS; j++) {
			const v3 *given = &vertices[face[j]];
			if ((*given)[X] != corners[j][X] || (*given)[Y] != corners[j][Y] || (*given)[Z] != corners[j][Z]) {
				errno = EINVAL;
				return NULL;
			}
		}
		double area;
		double gradient[SHELL_CORNERS][3];
		if (!face_is_element(vertices[face[0]], vertices[face[1]], vertices[face[2]], &area, gradient)) {
			errno = EINVAL;
			return NULL;
		}
	}

	struct ShellDiffusionMesh *mesh = safe_malloc(sizeof(struct ShellDiffusionMesh));
	mesh->num_vertices = num_vertices;
	mesh->num_faces = num_faces;
	mesh->vertices = safe_malloc(sizeof(v3) * num_vertices);
	memcpy(mesh->vertices, vertices, sizeof(v3) * num_vertices);
	mesh->faces = safe_malloc(sizeof(uint32_t[3]) * num_faces);
	memcpy(mesh->faces, faces, sizeof(uint32_t[3]) * num_faces);
	mesh->objects = safe_malloc(sizeof(struct Object *) * num_faces);
	memcpy(mesh->objects, object_table, sizeof(struct Object *) * num_faces);
	mesh->face_element = safe_malloc(sizeof(size_t) * num_faces);
	mesh->element_faces = safe_malloc(sizeof(size_t) * num_faces);
	mesh->references = 1;

	mesh->num_elements = 0;
	for (i = 0; i < num_faces; i++) {
		const uint32_t *face = mesh->faces[i];
		double area;
		double gradient[SHELL_CORNERS][3];
		if (face_is_element(mesh->vertices[face[0]], mesh->vertices[face[1]], mesh->vertices[face[2]], &area, gradient)) {
			mesh->face_element[i] = mesh->num_elements;
			mesh->element_faces[mesh->num_elements++] = i;
		} else {
			mesh->face_element[i] = SHELL_NO_FACE;
		}
	}

	return mesh;
}

struct ShellDiffusionMesh *shell_diffusion_mesh_clone(const struct ShellDiffusionMesh *mesh)
{
	errno = 0;
	if (!mesh) {
		errno = EINVAL;
		return NULL;
	}
	struct ShellDiffusionMesh *clone = safe_malloc(sizeof(struct ShellDiffusionMesh));
	clone->num_vertices = mesh->num_vertices;
	clone->num_faces = mesh->num_faces;
	clone->vertices = safe_malloc(sizeof(v3) * mesh->num_vertices);
	memcpy(clone->vertices, mesh->vertices, sizeof(v3) * mesh->num_vertices);
	clone->faces = safe_malloc(sizeof(uint32_t[3]) * mesh->num_faces);
	memcpy(clone->faces, mesh->faces, sizeof(uint32_t[3]) * mesh->num_faces);
	clone->objects = NULL;
	clone->face_element = NULL;
	clone->element_faces = NULL;
	clone->num_elements = 0;
	clone->references = 1;
	return clone;
}

size_t shell_diffusion_mesh_vertex_count(const struct ShellDiffusionMesh *mesh)
{
	return mesh ? mesh->num_vertices : 0;
}

size_t shell_diffusion_mesh_face_count(const struct ShellDiffusionMesh *mesh)
{
	return mesh ? mesh->num_faces : 0;
}

bool shell_diffusion_mesh_get_vertex(const struct ShellDiffusionMesh *mesh, const size_t vertex, v3 position)
{
	if (!mesh || !position || vertex >= mesh->num_vertices)
		return false;
	assign3(position, mesh->vertices[vertex]);
	return true;
}

bool shell_diffusion_mesh_get_face(const struct ShellDiffusionMesh *mesh, const size_t face, uint32_t triangle[3])
{
	if (!mesh || !triangle || face >= mesh->num_faces)
		return false;
	memcpy(triangle, mesh->faces[face], sizeof(uint32_t[3]));
	return true;
}

const struct Object *shell_diffusion_mesh_get_object(const struct ShellDiffusionMesh *mesh, const size_t face)
{
	if (!mesh || !mesh->objects || face >= mesh->num_faces)
		return NULL;
	return mesh->objects[face];
}

void shell_diffusion_mesh_free(struct ShellDiffusionMesh *mesh)
{
	mesh_release(mesh);
}

/*******************************************************************************
 *	Solve
 *******************************************************************************/

struct ShellDiffusionSolution *shell_diffusion_solve(const struct ShellDiffusionMesh *mesh,
	const v3 *sources, const struct ShellDiffusionFaceTensor *tensors, const struct ShellDiffusionParams *params)
{
	errno = 0;
	if (!mesh || !sources || !tensors || !params
		|| !isfinite(params->relative_tolerance)
		|| !(params->relative_tolerance > 0.f && params->relative_tolerance < 1.f)
		|| params->max_iterations == 0) {
		errno = EINVAL;
		return NULL;
	}
	if (!mesh->objects || !mesh->num_elements) {
		/* Unbound or fully degenerate meshes cannot propagate anything. */
		errno = EINVAL;
		return NULL;
	}

	size_t i, j;
	for (i = 0; i < mesh->num_vertices; i++) {
		if (!finite3(sources[i]) || sources[i][X] < 0.f || sources[i][Y] < 0.f || sources[i][Z] < 0.f) {
			errno = EINVAL;
			return NULL;
		}
	}
	for (i = 0; i < mesh->num_faces; i++) {
		if (!tensor_is_spd(&tensors[i])) {
			errno = EINVAL;
			return NULL;
		}
	}

	struct SparseMatrix *matrix = safe_malloc(sizeof(struct SparseMatrix));
	memset(matrix, 0, sizeof(struct SparseMatrix));
	if (!matrix_assemble(mesh, tensors, matrix)) {
		matrix_free(matrix);
		errno = EINVAL;
		return NULL;
	}

	struct ShellDiffusionSolution *solution = safe_malloc(sizeof(struct ShellDiffusionSolution));
	solution->mesh = (struct ShellDiffusionMesh *)mesh;
	solution->references = 1;
	solution->vertices = safe_calloc(mesh->num_vertices, sizeof(v3));
	solution->matrix = matrix;
	memset(solution->stats, 0, sizeof(solution->stats));
	mesh_retain((struct ShellDiffusionMesh *)mesh);

	double *rhs = safe_malloc(sizeof(double) * mesh->num_vertices);
	double *channel_solution = safe_malloc(sizeof(double) * mesh->num_vertices);
	bool success = true;

	size_t channel;
	for (channel = 0; channel < SHELL_CHANNELS; channel++) {
		/* Consistent P1 mass matrix applied to the source: each corner of an
		 * element receives A/12 * (2 s_i + s_j + s_k). */
		memset(rhs, 0, sizeof(double) * mesh->num_vertices);
		size_t e;
		for (e = 0; e < mesh->num_elements; e++) {
			const size_t face = mesh->element_faces[e];
			const uint32_t *indices = mesh->faces[face];
			double area;
			double gradient[SHELL_CORNERS][3];
			face_is_element(mesh->vertices[indices[0]], mesh->vertices[indices[1]], mesh->vertices[indices[2]], &area, gradient);
			for (i = 0; i < SHELL_CORNERS; i++) {
				double sum = 0.0;
				for (j = 0; j < SHELL_CORNERS; j++)
					sum += (i == j ? 2.0 : 1.0) * (double)sources[indices[j]][channel];
				rhs[indices[i]] += area / 12.0 * sum;
			}
		}

		memset(channel_solution, 0, sizeof(double) * mesh->num_vertices);
		if (!cg_solve(matrix, rhs, (double)params->relative_tolerance, params->max_iterations, channel_solution, &solution->stats[channel])) {
			success = false;
			break;
		}
		for (i = 0; i < mesh->num_vertices; i++)
			solution->vertices[i][channel] = (float)channel_solution[i];
	}

	free(rhs);
	free(channel_solution);

	if (!success) {
		errno = ERANGE;
		solution_release(solution);
		return NULL;
	}
	return solution;
}

size_t shell_diffusion_solution_vertex_count(const struct ShellDiffusionSolution *solution)
{
	return solution ? solution->mesh->num_vertices : 0;
}

size_t shell_diffusion_solution_face_count(const struct ShellDiffusionSolution *solution)
{
	return solution ? solution->mesh->num_faces : 0;
}

bool shell_diffusion_solution_get_vertex(const struct ShellDiffusionSolution *solution, const size_t vertex, v3 illumination)
{
	if (!solution || !illumination || vertex >= solution->mesh->num_vertices)
		return false;
	assign3(illumination, solution->vertices[vertex]);
	return true;
}

bool shell_diffusion_solution_get_stats(const struct ShellDiffusionSolution *solution, const size_t channel, struct ShellDiffusionStats *stats)
{
	if (!solution || !stats || channel >= SHELL_CHANNELS)
		return false;
	*stats = solution->stats[channel];
	return true;
}

void shell_diffusion_solution_get_residuals(const struct ShellDiffusionSolution *solution, v3 residuals)
{
	size_t channel;
	for (channel = 0; channel < SHELL_CHANNELS; channel++)
		residuals[channel] = solution->stats[channel].residual;
}

void shell_diffusion_solution_get_iterations(const struct ShellDiffusionSolution *solution, uint32_t iterations[3])
{
	size_t channel;
	for (channel = 0; channel < SHELL_CHANNELS; channel++)
		iterations[channel] = solution->stats[channel].iterations;
}

bool shell_diffusion_solution_get_face(const struct ShellDiffusionSolution *solution, const size_t face, uint32_t triangle[3])
{
	return shell_diffusion_mesh_get_face(solution ? solution->mesh : NULL, face, triangle);
}

const struct Object *shell_diffusion_solution_get_object(const struct ShellDiffusionSolution *solution, const size_t face)
{
	return shell_diffusion_mesh_get_object(solution ? solution->mesh : NULL, face);
}

bool shell_diffusion_solution_find_face(const struct ShellDiffusionSolution *solution, const struct Object *object, size_t *face)
{
	if (!solution || !object)
		return false;
	size_t i;
	for (i = 0; i < solution->mesh->num_faces; i++) {
		if (solution->mesh->objects[i] == object) {
			if (face)
				*face = i;
			return true;
		}
	}
	return false;
}

bool shell_diffusion_solution_get_matrix(const struct ShellDiffusionSolution *solution, const size_t channel, struct ShellDiffusionMatrixView *view)
{
	if (!solution || !view || channel >= SHELL_CHANNELS)
		return false;
	view->num_rows = solution->matrix->num_rows;
	view->num_nonzeros = solution->matrix->num_nonzeros;
	view->row_offsets = solution->matrix->row_offsets;
	view->columns = solution->matrix->columns;
	view->values = solution->matrix->values;
	return true;
}

/* World-space barycentric interpolation on the bound face. Vertices are taken
 * from the snapshot's own mesh through the binding, so a point on one instance
 * is never confused with the same geometry of another instance. */
static bool shell_sample(const struct ShellDiffusionSolution *solution, const struct Object *object, const v3 point, v3 illumination)
{
	if (!solution || !object || !illumination)
		return false;
	size_t face;
	if (!shell_diffusion_solution_find_face(solution, object, &face))
		return false;

	const uint32_t *indices = solution->mesh->faces[face];
	const v3 *va = &solution->mesh->vertices[indices[0]];
	const v3 *vb = &solution->mesh->vertices[indices[1]];
	const v3 *vc = &solution->mesh->vertices[indices[2]];
	v3 edge_ba, edge_ca, relative, normal;
	sub3v(*vb, *va, edge_ba);
	sub3v(*vc, *va, edge_ca);
	cross(edge_ba, edge_ca, normal);
	const double area2 = (double)dot3(normal, normal);
	if (!(area2 > 0.0))
		return false;
	sub3v(point, *va, relative);

	/* Barycentric weights with the interior of the triangle positive:
	 * w_b = dot((p - a) x (c - a), n)/|n|^2 and
	 * w_c = dot((b - a) x (p - a), n)/|n|^2. */
	v3 relative_cross_ca, ba_cross_relative;
	cross(relative, edge_ca, relative_cross_ca);
	cross(edge_ba, relative, ba_cross_relative);
	const double weight_b = (double)dot3(relative_cross_ca, normal) / area2;
	const double weight_c = (double)dot3(ba_cross_relative, normal) / area2;
	const double weight_a = 1.0 - weight_b - weight_c;

	const v3 *u0 = &solution->vertices[indices[0]];
	const v3 *u1 = &solution->vertices[indices[1]];
	const v3 *u2 = &solution->vertices[indices[2]];
	illumination[X] = (float)(weight_a * (double)(*u0)[X] + weight_b * (double)(*u1)[X] + weight_c * (double)(*u2)[X]);
	illumination[Y] = (float)(weight_a * (double)(*u0)[Y] + weight_b * (double)(*u1)[Y] + weight_c * (double)(*u2)[Y]);
	illumination[Z] = (float)(weight_a * (double)(*u0)[Z] + weight_b * (double)(*u1)[Z] + weight_c * (double)(*u2)[Z]);
	return true;
}

bool shell_diffusion_solution_interpolate(const struct ShellDiffusionSolution *solution, const struct Object *object, const v3 point, v3 illumination)
{
	return shell_sample(solution, object, point, illumination);
}

/*******************************************************************************
 *	Reference counting
 *******************************************************************************/
static bool mesh_retain(struct ShellDiffusionMesh *mesh)
{
	if (!mesh)
		return false;
	mesh->references++;
	return true;
}

static void mesh_release(struct ShellDiffusionMesh *mesh)
{
	if (!mesh)
		return;
	if (--mesh->references)
		return;
	free(mesh->vertices);
	free(mesh->faces);
	free(mesh->objects);
	free(mesh->face_element);
	free(mesh->element_faces);
	free(mesh);
}

static void solution_release(struct ShellDiffusionSolution *solution)
{
	if (!solution)
		return;
	if (--solution->references)
		return;
	mesh_release(solution->mesh);
	free(solution->vertices);
	matrix_free(solution->matrix);
	free(solution);
}

struct ShellDiffusionSolution *shell_diffusion_solution_retain(struct ShellDiffusionSolution *solution)
{
	if (!solution)
		return NULL;
	solution->references++;
	return solution;
}

void shell_diffusion_solution_free(struct ShellDiffusionSolution *solution)
{
	solution_release(solution);
}

/*******************************************************************************
 *	Renderer bridge
 *******************************************************************************/

static void *shell_bridge_retain(void *solution)
{
	return shell_diffusion_solution_retain((struct ShellDiffusionSolution *)solution);
}

static void shell_bridge_release(void *solution)
{
	shell_diffusion_solution_free((struct ShellDiffusionSolution *)solution);
}

static bool shell_bridge_sample(const void *solution, const struct Object *object, const v3 point, v3 illumination)
{
	return shell_sample((const struct ShellDiffusionSolution *)solution, object, point, illumination);
}

/* Registers the snapshot operations with the renderer at load time. The
 * renderer keeps a fallback path when this module is not linked. */
__attribute__((constructor)) static void shell_diffusion_register_renderer(void)
{
	static const struct ShellDiffusionRenderOps ops = {
		.retain = &shell_bridge_retain,
		.release = &shell_bridge_release,
		.sample = &shell_bridge_sample,
	};
	render_shell_register_ops(&ops);
}
