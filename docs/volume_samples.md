# Density and surface opacity

This note documents the voxel sample API that separates a voxel's physical
**density** from its **surface color (RGBA)**. Previously the single alpha
channel controlled both: lowering it to make a surface translucent also made
the voxel disappear. A voxel can now be fully solid (density 255) while its
surface carries any alpha, including 0.

## Data type

```c
typedef struct {
    uint8_t rgba[4];  // Appearance color of the surface, including alpha.
    uint8_t density;  // Physical density; 0 means empty.
} volume_sample_t;
```

## Functions

All three follow the calling convention of the existing `volume_set_at`,
`volume_get_at` and `volume_read`: the iterator/accessor is optional and may
be `NULL` for one-off access.

```c
void volume_set_sample_at(volume_t *volume, volume_iterator_t *it,
                          const int pos[3], const volume_sample_t *v);

void volume_get_sample_at(const volume_t *volume, volume_iterator_t *it,
                          const int pos[3], volume_sample_t *out);

void volume_read_samples(const volume_t *volume,
                         const int pos[3], const int size[3],
                         volume_sample_t *data);
```

`volume_read_samples` supports exactly the same aligned block as
`volume_read` — one tile plus a one voxel border (`pos` on a `TILE_SIZE - 1`
boundary, `size` all `TILE_SIZE + 2`) — and uses the same xyz ordering. It
writes `volume_sample_t` records; the five byte records are never returned
through the legacy four byte `volume_read` interface.

## Legacy interop and mixed writes

The two write styles can be mixed freely inside one volume; each voxel
remembers which style last wrote it.

- `volume_set_at` (legacy) keeps its old meaning: the stored alpha is the
  density, and the four stored RGBA bytes are unchanged.
- Reading such a legacy voxel with `volume_get_sample_at` returns
  `density == stored alpha`, and an appearance alpha of `255` when the
  density is nonzero, otherwise `0`. RGB is returned as stored.
- `volume_set_sample_at` stores the five bytes independently: a later
  `volume_get_at`/`volume_read` still returns the original four RGBA bytes,
  while `volume_get_sample_at` returns exactly what was written.
- Writing a voxel again with the legacy setter after an explicit sample
  reverts that voxel to the legacy interpretation, and vice versa.

## What density controls

Density, never appearance alpha, defines the shape:

- A voxel with density 0 is empty. Exact bounding boxes, empty tile cleanup
  (including discarding colors on zero density voxels) and skipped empty
  tiles all use density.
- Cubic faces: the visibility threshold stays at 127. Neighbor face
  culling, occlusion and gradients use density only. Vertex RGBA uses the
  appearance value, so density 255 with alpha 0 still emits geometry.
- Marching cubes: the sign test (`>= 127`), edge interpolation, smoothing
  and position quantization all use density. An edge's color is taken from
  the endpoint with the higher density (ties keep the existing second
  endpoint), and the output alpha is that endpoint's appearance alpha — it
  is no longer forced to 255.

Changing only the opacity therefore leaves cubic and marching cube
positions, normals and triangle connectivity unchanged (including flat mode
triangle splitting). Changing the density changes the surface as expected.
Legacy RGBA data keeps its previous geometry and opaque output.

`volume_generate_mesh` with `palette == NULL` and `simplify == 0` preserves
the same surface and carries the linear appearance alpha into the mesh
vertex colors.

## Lifetime and caching

The new density and per-voxel style arrays participate in the existing tile
copy-on-write machinery (`volume_copy`, `volume_set`, `volume_copy_tile`,
`volume_clear`, reference counted `volume_dup`, tile COW). A snapshot only
sees its own changes: editing either the density or the opacity on one
volume never leaks into another snapshot. Either kind of change advances the
volume key and the tile data id, which also invalidates cached neighboring
tile data.
