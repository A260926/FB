# Skin link modes (`Total1` and `Additive` cluster `Mode`)

FBX skin clusters carry a string property named `Mode` that selects how the
per-cluster weights are combined when deforming a vertex. Historically ufbx
only implemented the normalized behavior. ufbx now reads the cluster `Mode` and
exposes it through `ufbx_skin_link_mode`.

## Matrix orientation

All matrices in ufbx are **column-major affine transforms** (a 3x3 linear block
plus a translation column) that map points from local space to world space.
For a point written as a column vector `p`, a transform `M` is applied as
`M * p`.

For a skinned mesh let:

- `p` be the local (geometry space) control vertex,
- `W_i` be the current world matrix of cluster `i`, i.e.
  `bone_node.node_to_world * geometry_to_bone`, available as
  `ufbx_skin_cluster.geometry_to_world`,
- `w_i` be the weight of cluster `i` for the vertex,
- `F` be the current geometry-to-world matrix of the single mesh instance,
  available as `mesh.instances[0].geometry_to_world`.

## The three link modes

### `UFBX_SKIN_LINK_NORMALIZE` (value `0`, the default)

Used when the cluster `Mode` is missing, `"Average"` or `"Normalize"`.
Weights are normalized by their sum before blending, so the residual weight is
absorbed by the bone matrices:

```
result = sum(w_i * W_i) * p / sum(w_i)
```

Vertices with no effective influence fall back to `F * p`. This is the original
ufbx behavior and is unchanged, including the `RIGID`, `DUAL_QUATERNION` and
`BLENDED_DQ_LINEAR` skinning methods and their weight ordering.

### `UFBX_SKIN_LINK_TOTAL_ONE` (`"Total1"`)

Weights are treated as fractions that already total one over the whole skin.
The missing fraction is applied to the mesh instance transform:

```
result = sum(w_i * W_i) * p + (1 - sum(w_i)) * F * p
```

The residual term is **never truncated or renormalized**: individual weights may
be larger than one and the sum of the weights may exceed one, in which case the
residual `(1 - sum)` is simply negative. A vertex with no influence is
transformed by `F` alone.

### `UFBX_SKIN_LINK_ADDITIVE` (`"Additive"`)

Each influence is first converted back into mesh space and blended with the
identity transform:

```
C_i = inverse(F) * W_i
A_i = (1 - w_i) * I + w_i * C_i
```

The per-vertex matrices are then chained in the order of the OO cluster
connections stored in `ufbx_skin_deformer.clusters[]` (this order is independent
of element id, name and the descending per-vertex weight order). Only the
clusters that actually influence the vertex participate, keeping their relative
connection order. If the participating clusters in order are
`i_1, i_2, ..., i_n` then:

```
result = F * A_i_n * ... * A_i_2 * A_i_1 * p
```

A vertex with no influence, or whose effective weights are all zero, is
transformed by `F` alone.

## The `fallback` argument

`ufbx_get_skin_vertex_matrix(skin, vertex, fallback)` returns the single matrix
that maps the local vertex to its deformed world position.

- For `NORMALIZE` `fallback` is the matrix returned for an unskinned vertex.
- For `TOTAL_ONE` and `ADDITIVE` `fallback` is the mesh instance transform `F`.
  Pass `NULL` to use the identity matrix. A non-`NULL` fallback is expected to
  be invertible for `ADDITIVE`.

`evaluate_scene()` always passes the current mesh instance
`geometry_to_world` matrix as `F`, so the matrix entry point and
`ufbx_mesh.skinned_position` agree for the same pose. The skinned positions are
world positions and are not multiplied by the instance transform a second time.

## Loading and validation

`ufbx_skin_deformer.link_mode` and `ufbx_skin_cluster.link_mode` report the
resolved mode. All clusters of one skin must use the same mode; otherwise
loading fails with an error naming the offending cluster. An unknown `Mode`
string is also a load error. Files without a cluster `Mode` keep loading
exactly as before.

The non-normalized modes only support a restricted setup. When a `TOTAL_ONE` or
`ADDITIVE` skin encounters one of the following, loading fails (the error
message contains the cluster name) instead of silently degrading:

- an `AssociateModel` or `TransformAssociateModel` subnode,
- a cluster without a valid bone node,
- a skinning method other than `LINEAR`,
- a mesh with more than one instance,
- a mesh driven by more than one skin deformer,
- a mesh that also uses a geometry cache.

These restrictions only apply to the new modes; plain normalized skins are
unaffected.

## Scope and default settings

The new modes are supported with the default load settings: no target axis
conversion (`target_axes` unset), no unit conversion
(`target_unit_meters == 0`) and the default
`UFBX_GEOMETRY_TRANSFORM_HANDLING_PRESERVE` geometry transform handling.
Coordinate/unit conversion and geometry-transform helper nodes are not extended
to the new link modes. Animated bone and mesh-instance hierarchies, non-identity
binding matrices and mesh geometry transforms are supported within that scope.

## Example

```c
#include "ufbx.h"

ufbx_scene *scene = ufbx_load_file("character.fbx", NULL, NULL);
ufbx_mesh *mesh = scene->meshes.data[0];
ufbx_skin_deformer *skin = mesh->skin_deformers.data[0];

if (skin->link_mode == UFBX_SKIN_LINK_TOTAL_ONE ||
    skin->link_mode == UFBX_SKIN_LINK_ADDITIVE) {
    ufbx_matrix F = mesh->instances.data[0]->geometry_to_world;
    for (size_t i = 0; i < mesh->num_vertices; i++) {
        ufbx_matrix deform = ufbx_get_skin_vertex_matrix(skin, i, &F);
        ufbx_vec3 local = mesh->vertices.data[i];
        ufbx_vec3 world = ufbx_transform_position(&deform, local);
        (void)world;
    }
}
```
