# Recursive planar portals

Depth-limited recursive portal rendering for the offline software
renderer. Public entry point:

```c
int scene_draw_portals(scene_t *scene, framebuffer_t *dst,
                       const perframe_t *frame,
                       const portal_t *portals, int count,
                       int max_depth, float clip_bias);
```

It reuses the existing `model.update`/`model.draw` path, the Blinn and
both PBR materials, and `graphics_draw_triangle`. No second rasterizer
is introduced.

## Coordinates

- A portal is a rectangle in its own local frame: the plane is local
  `z = 0`, spanning `[-half_size.x, +half_size.x]` by
  `[-half_size.y, +half_size.y]`. Local `+Z` is the front face.
- `local_to_world` is a rigid body transform (orthonormal rotation plus
  translation). Scale and reflection frames are rejected. The fourth row
  must be `(0, 0, 0, 1)` and the 3x3 block must be right-handed with
  unit-length, mutually perpendicular columns.
- Portals are paired: each portal's `linked` index points at its
  partner, the partner points back, and the two share the same half
  size. Paired doors face one another, so an exit door is typically a
  translation followed by `Ry(pi)` (for example
  `translate * mat4_rotate_y(PI)`).
- A portal is visible only while the camera is on its front side and
  farther from the plane than `clip_bias`. The back face is never drawn
  and never occludes the scene behind it.
- The camera is right handed looking down `-Z`; the projection must be
  ordinary finite perspective (`w = -view.z`, NDC z in `[-1, 1]`).
  Asymmetric frusta built with `mat4_frustum` are allowed; orthographic
  and infinite-far projections are rejected.

## Camera transform

When looking through entry door A toward exit door B, the camera world
matrix becomes

```
C' = B.local_to_world * Ry(pi) * inverse(A.local_to_world) * C
```

`Ry(pi)` flips local x and z. It is a 180 degree rotation, never a mirror
reflection. Each hop applies the same formula to the current camera, so
the transform accumulates through the recursion while the projection,
frame time, lights and model world transforms stay fixed for the whole
frame.

## Clipping and depth

- Each sub-view inherits the root projection. The exit door's world
  plane is converted to the sub-view's homogeneous clip space (planes
  transform with the inverse transpose of the point matrix) and handed
  to `program_set_clip_plane`. Geometry is kept where
  `dot(plane, clip_position) >= 0`, which corresponds to
  `local/exit-space z >= clip_bias`.
- The user clip plane works together with the existing frustum clipping.
  New vertices are interpolated in clip space, so their varyings remain
  perspective correct. Triangles crossing the exit plane are split; they
  are neither wholly kept nor wholly dropped.
- A sub-view is rendered into its own framebuffer and sampled back at
  the exact parent screen pixel (texel-center aligned), never stretched
  over the door rectangle as an ordinary texture.
- The parent buffer only ever receives the door face's depth in the
  parent view. The deeper sub-view depth is never copied back, so an
  object behind the door cannot cover the doorway and an object in front
  of the door still occludes it. Overlapping doors resolve by their real
  parent-view plane depth using the existing `<=` rule; models draw
  first, then doors in array order, so equal-depth later doors win.

## Recursion limits

- `max_depth` is in `[0, 4]`. At `0`, every visible door is its opaque
  `fallback` plane. At `n`, up to `n` hops are rendered and any further
  visible door uses its fallback.
- Mutually visible doors terminate purely on the depth budget. There is
  no global visited set, so a door that is legitimately visible again is
  not suppressed.
- Each recursion level owns independent color, depth and camera state.

## Lifecycle

A call proceeds as follows:

1. Validate the frame, scene, portals, depth range and clip bias. Any
   failure returns non-zero before drawing and `dst` is left byte-for-byte
   unchanged.
2. Snapshot `dst` color and depth.
3. Create the per-call door-face program and a scratch texture, then
   recurse. Each child view allocates and releases its own framebuffer.
4. On return, disable clip planes on every program and re-run
   `model.update` with the caller's original frame so model uniforms
   match the camera that was passed in.
5. Release the face program and scratch texture, free the snapshot, and
   return 0.

The call keeps no persistent global state: the caller's `frame`, model
transforms, portal definitions and `scene.models` order are never
modified. Two sibling doors, interleaved scenes and successive frames
cannot borrow each other's results. Model meshes and textures still go
through the existing resource cache, whose `cache_cleanup()` remains the
caller's responsibility when tearing scenes down.

## Limitations

- Only opaque and alpha-cutout models are supported. Alpha-cutout
  fragments are discarded by the unchanged model path and therefore
  never write color or depth inside a doorway.
- No skybox, no alpha-blended (transparent) models, and no
  `shadow_buffer`/`shadow_map`. A scene configured with any of these is
  rejected up front.
- Portal frames must be rigid: no scale and no reflection.
- Finite perspective projection only; see the coordinate rules above.
- The implementation is pure C99 plus the standard library and adds no
  window, GPU or network dependency.
