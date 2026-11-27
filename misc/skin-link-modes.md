# Skin link modes

This document describes the skin cluster `Mode` implementations added to ufbx:
`UFBX_SKIN_LINK_TOTAL_ONE` and `UFBX_SKIN_LINK_ADDITIVE`, along with the default
`UFBX_SKIN_LINK_NORMALIZE`. It also documents the matrix conventions, the meaning
of the `fallback` argument of `ufbx_get_skin_vertex_matrix()` for these modes and
the supported scope.

## Public API

```c
typedef enum ufbx_skin_link_mode {
    // Weights are relative proportions, divided by the total weight. This is
    // the classic linear blend skinning behavior and the default.
    UFBX_SKIN_LINK_NORMALIZE = 0,
    // Weights are absolute amounts, the missing weight is blended from `F`.
    UFBX_SKIN_LINK_TOTAL_ONE,
    // Clusters are applied one after another in connection order.
    UFBX_SKIN_LINK_ADDITIVE,
} ufbx_skin_link_mode;
```

Both `ufbx_skin_cluster` and `ufbx_skin_deformer` gained a `link_mode` field.
The mode is read from the cluster `Mode` field:

| `Mode` string           | `link_mode`                  |
|-------------------------|------------------------------|
| missing                 | `UFBX_SKIN_LINK_NORMALIZE`   |
| `"Average"`             | `UFBX_SKIN_LINK_NORMALIZE`   |
| `"Normalize"`           | `UFBX_SKIN_LINK_NORMALIZE`   |
| `"Total1"`              | `UFBX_SKIN_LINK_TOTAL_ONE`   |
| `"Additive"`            | `UFBX_SKIN_LINK_ADDITIVE`    |

All clusters inside a single skin must use the same mode. An unknown `Mode`
string or a mix of modes inside one skin fails loading with an error that
contains the offending cluster name. Files without `Mode` (the vast majority)
behave exactly like before, including `LINEAR`, `DUAL_QUATERNION` and
`BLENDED_DQ_LINEAR` normalization and the descending weight ordering of
`ufbx_skin_deformer.weights[]`.

## Matrix conventions

All matrices are column-vector matrices (`v' = M * v`), matching
`ufbx_matrix`, `ufbx_transform_position()` and the rest of ufbx. Composition is
`ufbx_matrix_mul(&a, &b)` meaning `a * b` applied to a column vector as
`a * (b * v)`.

For a vertex with local control point `p`:

* `F` is the current mesh instance `geometry_to_world`, ie. the
  `ufbx_node.geometry_to_world` of `ufbx_mesh.instances[0]`. It includes the
  node parent chain, the node's own transform and the geometry transform.
* `W_i` is the cluster's `geometry_to_world`, ie.
  `bone_node->node_to_world * cluster->geometry_to_bone`.
* `w_i` are the cluster weights for the vertex.

### Normalize (default, unchanged)

The classic behavior is preserved bit-for-bit:

```
sum(w_i * W_i) / sum(w_i)          * p
```

Weight order in `ufbx_skin_deformer.weights[]` is sorted by descending weight.
The `fallback` argument of `ufbx_get_skin_vertex_matrix()` is returned for
vertices without any weights.

### Total1 (`UFBX_SKIN_LINK_TOTAL_ONE`)

The weights are absolute amounts, so no division by the total happens and the
residual `1 - sum(w_i)` is blended from `F`:

```
sum(w_i * W_i * p) + (1 - sum(w_i)) * F * p
```

The residual term is **not clamped** to `[0, 1]` and is **not divided** by
`sum(w_i)`, so weights may sum to more than one and a single weight may be
larger than one. A vertex without any (positive) weight gets `F * p`.

Example: with `F = identity`, two clusters translating by `(4, 0, 0)` and
`(0, 6, 0)` and weights `0.2`/`0.3`, the origin maps to `(0.8, 1.8, 0)`, while
the normalized path maps it to `(1.6, 3.6, 0)`.

When `F` carries rotation and translation the actual `F` is used for the
residual, the effect is not treated as if `F` were the identity.

### Additive (`UFBX_SKIN_LINK_ADDITIVE`)

Each influence is first moved to the current mesh space, then applied as a
matrix that blends towards the identity by its weight, one after another:

```
C_i   = inverse(F) * W_i
A_i   = (1 - w_i) * I + w_i * C_i
world = F * A_last * ... * A_first * p
```

The application order is the order of the `OO` connections from the clusters to
the skin in the source file (`ufbx_skin_cluster.link_order`), restricted to the
clusters that influence the vertex. It is **not**:

* a spherical interpolation of rotations,
* a sum of the `W_i` matrices,
* the descending-weight order of `ufbx_skin_deformer.weights[]`,
* the `element_id` order, the cluster name order or the `Objects` declaration
  order.

Applying the clusters in a different order produces a different transform. For
example with `F = identity`, a first cluster rotating 90 degrees about Z with
weight `0.25` and a second cluster translating by `(2, 0, 0)` with weight
`0.75`, the point `(1, 0, 0)` maps to `(2.25, 0.25, 0)`; swapping the two
cluster connections gives `(1.875, 0.625, 0)`. Renaming or renumbering the
clusters does not change the result.

A vertex without any (positive) influence, or with all-zero weights, gets
`F * p`.

## `fallback` argument

`ufbx_get_skin_vertex_matrix(skin, vertex, fallback)` implements the same rules
and returns a matrix from local vertex space to world space.

For the new modes `fallback` is the mesh instance matrix `F`:

* `NULL` means the identity matrix.
* A non-`NULL` matrix must be invertible (it is inverted for the additive
  mode).
* The result already includes `F`, so the caller must not multiply the
  resulting world position by the instance matrix again.

For `UFBX_SKIN_LINK_NORMALIZE`, `fallback` keeps its original meaning: the
matrix returned for a vertex without weights.

## Scene evaluation

`ufbx_evaluate_scene(..., evaluate_skinning = true)` passes the current mesh
instance `geometry_to_world` as `F`, not the transform from load time, and the
resulting `ufbx_mesh.skinned_position` values are world-space
(`skinned_is_local == false`). Loading directly with
`ufbx_load_opts.evaluate_skinning = true` goes through the same implementation.
The existing order (blend shapes first, then skinning, then normal
reconstruction) is unchanged, and no new morph algorithm is added.

Repeated or out-of-order evaluation of animation poses produces identical
results and does not modify the source scene.

## Supported scope

The new modes deliberately support only a narrow subset:

* one mesh instance (`ufbx_mesh.instances.count == 1`),
* one `LINEAR` skin (`UFBX_SKINNING_METHOD_LINEAR`) per mesh,
* no geometry cache deformers,
* every cluster has a valid `bone_node`,
* finite non-negative weights; a single weight may exceed one and the total may
  exceed one,
* mesh, bone and parent-chain transforms are finite and invertible with a
  positive local scale,
* animation on the bones and on the mesh parent chain, non-identity bind
  matrices, and a mesh geometry transform are all supported.

Loading fails explicitly (never silently falling back to normalized skinning)
for the unsupported combinations with the new modes:

* `AssociateModel` objects or `TransformAssociateModel` matrices,
* a non-`LINEAR` skinning method,
* multiple mesh instances,
* multiple skins on the mesh,
* geometry cache deformers.

Files using `UFBX_SKIN_LINK_NORMALIZE` are unaffected by these restrictions.

Coordinate system conversion (`ufbx_load_opts.target_axes`,
`target_unit_meters`) and `UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES` are
not extended for the new modes; the scope above assumes the default load
settings (`geometry_transform_handling == UFBX_GEOMETRY_TRANSFORM_HANDLING_PRESERVE`,
no target axes/units).

## C example

```c
#include "ufbx.h"
#include <stdio.h>

static void print_skinned(ufbx_scene *scene)
{
    for (size_t mi = 0; mi < scene->meshes.count; mi++) {
        ufbx_mesh *mesh = scene->meshes.data[mi];
        if (mesh->skin_deformers.count == 0) continue;
        if (mesh->instances.count == 0) continue;

        ufbx_skin_deformer *skin = mesh->skin_deformers.data[0];
        // The current mesh instance matrix, used as `F` by the new modes.
        const ufbx_matrix *fallback = &mesh->instances.data[0]->geometry_to_world;

        for (size_t vi = 0; vi < mesh->num_vertices; vi++) {
            // Local-to-world matrix for a single vertex.
            ufbx_matrix mat = ufbx_get_skin_vertex_matrix(skin, vi, fallback);
            ufbx_vec3 world = ufbx_transform_position(&mat, mesh->vertices.data[vi]);
            printf("v%zu mode=%d -> (%f, %f, %f)\n", vi,
                (int)skin->link_mode, world.x, world.y, world.z);
        }
    }
}

int main(void)
{
    ufbx_error error;
    ufbx_load_opts opts = { 0 };
    opts.evaluate_skinning = true; // World-space ufbx_mesh.skinned_position
    ufbx_scene *scene = ufbx_load_file("character.fbx", &opts, &error);
    if (!scene) {
        fprintf(stderr, "load failed: %s\n", error.description.data);
        return 1;
    }

    print_skinned(scene);
    ufbx_free_scene(scene);
    return 0;
}
```

Compile with `cc -std=c99 example.c ufbx.c -lm`.
