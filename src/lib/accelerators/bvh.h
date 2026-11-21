//
//  bvh.h
//  c-ray
//
//  Created by Arsène Pérard-Gayot on 07/06/2020.
//  Copyright © 2020-2025 Arsène Pérard-Gayot (@madmann91), Valtteri Koskivuori. All rights reserved.
//

#pragma once

#include <renderer/samplers/sampler.h>
#include <renderer/instance.h>

#include <stdbool.h>
#include <stddef.h>

struct lightRay;
struct hitRecord;
struct mesh;
struct poly;
struct boundingBox;

struct bvh;

/// Returns the bounding box of the root of the given BVH
struct boundingBox get_root_bbox(const struct bvh *bvh);

/// Read-only connectivity for locality tests.
size_t bvh_node_count(const struct bvh *bvh);
long bvh_node_parent(const struct bvh *bvh, size_t node);
long bvh_node_child(const struct bvh *bvh, size_t node, unsigned slot);

/// Returns the transformed bounding box of the BVH, using a better approximation than transforming
/// the root bounding box.
struct boundingBox get_transformed_root_bbox(const struct bvh *bvh, const struct matrix4x4 *);

/// Builds a BVH for a given mesh
/// @param mesh Mesh containing polygons to process
/// @param count Amount of polygons given
struct bvh *build_mesh_bvh(const struct mesh *mesh);

/// Builds a top-level BVH for a given set of instances
/// @param instances Instances to build a top-level BVH for
/// @param instanceCount Amount of instances
struct bvh *build_top_level_bvh(const struct instance *instances);

/// Intersect a ray with a scene top-level BVH
bool traverse_top_level_bvh(
	const struct instance *instances,
	const struct bvh *bvh,
	const struct lightRay *ray,
	struct hitRecord *isect,
	sampler *sampler);

bool traverse_bottom_level_bvh(
	const struct mesh *mesh,
	const struct lightRay *ray,
	struct hitRecord *isect,
	sampler *sampler);

/// Frees the memory allocated by the given BVH
void destroy_bvh(struct bvh *);

/// Refits the BLAS in place after PN patch control nets changed. The tree
/// topology and primitive ordering are preserved; only the bounds of leaves
/// containing a changed primitive and of their ancestors are rewritten.
/// Reports the real numbers of updated leaves and inner nodes.
void bvh_refit_mesh(struct bvh *bvh,
                    const struct mesh *mesh,
                    const size_t *changed_faces,
                    size_t changed_count,
                    size_t *leaf_updates,
                    size_t *node_updates);
