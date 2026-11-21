/*
 * Copyright (c) 2021-2022 Wojciech Graj
 *
 * Licensed under the MIT license: https://opensource.org/licenses/MIT
 * Permission is granted to use, copy, modify, and redistribute the work.
 * Full license information available in the project LICENSE file.
 *
 * DESCRIPTION:
 *   Steady-state surface scattering on thin-shell triangle meshes.
 *
 *   An indexed triangle mesh is copied and tied to the real triangle objects
 *   of the scene. Vertex illumination is propagated with one linear Galerkin
 *   steady-state diffusion solve over P1 elements, and the resulting solution
 *   is published to the renderer as an immutable snapshot, which is used to
 *   shade diffuse reflections on the bound triangles.
 **/

#ifndef __SHELL_DIFFUSION_H__
#define __SHELL_DIFFUSION_H__

#include "type.h"

struct Object;
struct ShellDiffusionRenderOps;

/* Snapshot operations the renderer uses for its active solution. */
struct ShellDiffusionRenderOps {
	void *(*retain)(void *solution);
	void (*release)(void *solution);
	bool (*sample)(const void *solution, const struct Object *object, const v3 point, v3 illumination);
};

/* Opaque indexed mesh owned by the model. */
struct ShellDiffusionMesh;
/* Opaque steady-state solution (immutable snapshot). */
struct ShellDiffusionSolution;

/* Solver controls. */
struct ShellDiffusionParams {
	float relative_tolerance; /* relative residual target, strictly in (0, 1) */
	uint32_t max_iterations; /* positive iteration cap */
};

/* Solver diagnostics of one colour channel. */
struct ShellDiffusionStats {
	uint32_t iterations; /* actual preconditioned CG iteration count */
	float residual; /* recomputed ||(M+K)u - M s||_2 / ||M s||_2, 0 for zero RHS */
};

/* Diffusion tensor of a single face: symmetric positive definite, world
 * space. Its normal component does not take part in in-plane propagation. */
struct ShellDiffusionFaceTensor {
	float D[3][3];
};

/* Read-only view of the assembled (M + K) matrix. It is identical for all
 * three colour channels because the tensor is not channel dependent. */
struct ShellDiffusionMatrixView {
	size_t num_rows;
	size_t num_nonzeros;
	const size_t *row_offsets; /* num_rows + 1 entries */
	const uint32_t *columns;
	const double *values;
};

/*
 * Mesh construction.
 *
 * vertices: num_vertices world-space positions.
 * faces: num_faces triples of vertex indices, wound as the bound object's
 *        corners.
 * objects: num_faces real triangle objects, face i <-> objects[i]. Every face
 *          must bind a distinct triangle object whose stored corners equal the
 *          face vertices in the given order, and whose material must be purely
 *          diffuse (ks, kr, kt and ka all zero). The caller keeps the objects
 *          alive and unchanged while bound; the mesh never owns them.
 *
 * The model owns copies of the vertex and face arrays and of the object table;
 * it never searches triangles by position. Returns NULL on invalid input
 * (errno EINVAL).
 */
struct ShellDiffusionMesh *shell_diffusion_mesh_new(const v3 *vertices, size_t num_vertices,
	const uint32_t (*faces)[3], size_t num_faces, struct Object *const *objects, size_t num_objects);

/* Deep copy of the vertices and faces of a mesh. The copy owns no objects and
 * cannot be solved. Returns NULL on invalid input. */
struct ShellDiffusionMesh *shell_diffusion_mesh_clone(const struct ShellDiffusionMesh *mesh);

size_t shell_diffusion_mesh_vertex_count(const struct ShellDiffusionMesh *mesh);
size_t shell_diffusion_mesh_face_count(const struct ShellDiffusionMesh *mesh);

/* Copy of a world-space mesh vertex. Returns false if out of range. */
bool shell_diffusion_mesh_get_vertex(const struct ShellDiffusionMesh *mesh, size_t vertex, v3 position);

/* Copy of a face's corner indices, in winding order. Returns false if out of range. */
bool shell_diffusion_mesh_get_face(const struct ShellDiffusionMesh *mesh, size_t face, uint32_t triangle[3]);

/* Read-only bound object of a face. Returns NULL if out of range. */
const struct Object *shell_diffusion_mesh_get_object(const struct ShellDiffusionMesh *mesh, size_t face);

/* Releases the model reference held by the caller. Solutions that still refer
 * to the model keep it alive. */
void shell_diffusion_mesh_free(struct ShellDiffusionMesh *mesh);

/*
 * Solve.
 *
 * sources: num_vertices non-negative finite RGB illumination values.
 * tensors: num_faces symmetric positive definite 3x3 world-space tensors.
 *
 * Assembles (M + K) u = M s per channel and solves it with sparse
 * Jacobi-preconditioned conjugate gradients. Returns a new solution snapshot
 * on success. On failure returns NULL without disturbing any existing
 * solution; errno is EINVAL for invalid input or an unbound mesh, and ERANGE
 * when the recomputed relative residual does not reach the tolerance.
 */
struct ShellDiffusionSolution *shell_diffusion_solve(const struct ShellDiffusionMesh *mesh,
	const v3 *sources, const struct ShellDiffusionFaceTensor *tensors, const struct ShellDiffusionParams *params);

size_t shell_diffusion_solution_vertex_count(const struct ShellDiffusionSolution *solution);
size_t shell_diffusion_solution_face_count(const struct ShellDiffusionSolution *solution);

/* Copy of the solved illumination at a vertex. Returns false if out of range. */
bool shell_diffusion_solution_get_vertex(const struct ShellDiffusionSolution *solution, size_t vertex, v3 illumination);

/* Per-channel diagnostics. Returns false if out of range. */
bool shell_diffusion_solution_get_stats(const struct ShellDiffusionSolution *solution, size_t channel, struct ShellDiffusionStats *stats);

/* Recomputed relative residuals / actual iteration counts of all three channels. */
void shell_diffusion_solution_get_residuals(const struct ShellDiffusionSolution *solution, v3 residuals);
void shell_diffusion_solution_get_iterations(const struct ShellDiffusionSolution *solution, uint32_t iterations[3]);

/* Face connectivity and object-to-face mapping, without changing state. */
bool shell_diffusion_solution_get_face(const struct ShellDiffusionSolution *solution, size_t face, uint32_t triangle[3]);
const struct Object *shell_diffusion_solution_get_object(const struct ShellDiffusionSolution *solution, size_t face);
bool shell_diffusion_solution_find_face(const struct ShellDiffusionSolution *solution, const struct Object *object, size_t *face);

/* Read-only CSR view of the assembled matrix. Returns false if out of range. */
bool shell_diffusion_solution_get_matrix(const struct ShellDiffusionSolution *solution, size_t channel, struct ShellDiffusionMatrixView *view);

/*
 * Renderer support. The solution is reference counted: retaining keeps it
 * alive after the caller frees its own reference; releasing may destroy it.
 */
struct ShellDiffusionSolution *shell_diffusion_solution_retain(struct ShellDiffusionSolution *solution);
void shell_diffusion_solution_free(struct ShellDiffusionSolution *solution);

/* Interpolates the solved illumination at a world-space point on a bound
 * triangle using the face's world barycentric coordinates. Returns false when
 * the object does not belong to this snapshot. */
bool shell_diffusion_solution_interpolate(const struct ShellDiffusionSolution *solution, const struct Object *object, const v3 point, v3 illumination);

#endif /* __SHELL_DIFFUSION_H__ */
