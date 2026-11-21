//
//  pn_triangle.h
//  c-ray
//
//  PN triangle (curved Bézier triangle) support for low-poly meshes with
//  adjusted vertex normals. The control net is defined purely from the
//  corner positions and unit face-corner normals, see pn_build_patch().
//
//  Created for the PN triangle preview task.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <v.h>
#include <c-ray/c-ray.h>
#include <common/vector.h>
#include <datatypes/bbox.h>

struct lightRay;
struct hitRecord;
struct mesh;
struct poly;

// Ten cubic triangular Bernstein control points, indexed as
// b[i+j+k = 3]:
//   0: b300 (corner P0)      1: b030 (corner P1)      2: b003 (corner P2)
//   3: b210 (edge P0->P1)    4: b120 (edge P1->P0)
//   5: b021 (edge P1->P2)    6: b012 (edge P2->P1)
//   7: b102 (edge P2->P0)    8: b201 (edge P0->P2)
//   9: b111 (interior)
#define PN_CONTROL_POINT_COUNT 10

struct pn_patch {
	struct vector b[PN_CONTROL_POINT_COUNT];
	struct boundingBox bbox; // Conservative AABB of the whole control hull
	float scale;             // S = max(1, control hull diagonal length)
	bool curved;             // False for faces without corner normals: flat behaviour
};

// Real counters from the last sparse normal update, exposed for locality checks.
struct pn_update_stats {
	size_t updated_patches;
	size_t leaf_updates;
	size_t node_updates;
};

// Per-mesh PN data, created at finalize when PN mode is enabled.
struct pn_mesh_data {
	struct pn_patch *patches; // One entry per original face
	size_t face_count;
	size_t normal_count;

	// Reverse lookup (CSR): faces referencing a given original normal index.
	size_t *normal_face_offsets; // normal_count + 1
	size_t *normal_face_data;    // face indices, length normal_face_offsets[normal_count]

	struct pn_update_stats last_stats;
};

// Builds the control net of a single face. Returns false (and leaves *out
// untouched) when the face has no corner normals, in which case the caller
// must keep the original flat triangle behaviour. Supplied normals are
// normalized before use.
bool pn_build_patch(const struct mesh *mesh, const struct poly *p, struct pn_patch *out);

// Allocates the per-mesh PN data: one patch per face and the reverse
// normal-index -> face lookup used by sparse updates.
struct pn_mesh_data *pn_mesh_setup(const struct mesh *mesh);
void pn_mesh_free(struct pn_mesh_data *pn);

// Conservative per-primitive BLAS bounds (control hull for curved faces,
// plain vertex bounds for flat ones).
struct boundingBox pn_prim_bbox(const struct mesh *mesh, size_t face_index);
struct vector pn_prim_center(const struct mesh *mesh, size_t face_index);

// Curved-patch ray intersection. t is measured on the passed ray's parameter
// axis and only a root in [0, isect->distance) may tighten the record. A miss
// never modifies *isect. On a hit, isect->uv holds the original face (u,v),
// the normal comes from dP/du x dP/dv followed by the existing two-sided
// orientation handling, and polygon/material selection stays with the face.
bool pn_ray_intersect(const struct mesh *mesh,
                      const struct poly *poly,
                      const struct pn_patch *patch,
                      const struct lightRay *ray,
                      struct hitRecord *isect);

// Sparse normal update. All indices/values are validated before any state is
// touched; an out of range index or a non-finite/zero normal rejects the whole
// call. Only control nets of faces referencing a changed normalIndex are
// rebuilt, the BLAS topology and primitive ordering are preserved, and the
// affected leaves and ancestors are refitted in place.
bool pn_mesh_update_normals(struct mesh *mesh,
                            const size_t *indices,
                            const struct cr_vector *values,
                            size_t count);

// Read-only accessors used by the task tests.
const struct pn_mesh_data *pn_mesh_data_of(const struct mesh *mesh);
const struct pn_patch *pn_mesh_get_patch(const struct mesh *mesh, size_t face_index);
