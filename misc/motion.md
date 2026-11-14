# Analytic motion evaluation

`ufbx_evaluate_motion()` evaluates the world-space motion of a whole scene at a
single time instant while propagating analytic time derivatives through the
whole evaluation chain. It never calls `ufbx_evaluate_scene()` at adjacent
times, uses no finite differencing, baking, or `nextafter()` style tricks, and
the same `ufbx_motion_side` is threaded through every curve, node, and
deformation decision.

## API

```c
// Curve level: value plus derivative with respect to time (seconds).
ufbx_motion_sample s;
bool ok = ufbx_evaluate_curve_motion(curve, time, default_value,
    UFBX_MOTION_LEFT, &s);

// Scene level: self-contained result, valid after the source scene is freed.
ufbx_error error;
ufbx_motion *motion = ufbx_evaluate_motion(scene, anim, time,
    UFBX_MOTION_RIGHT, &error);
// ... use motion->nodes[] and motion->instances[] ...
ufbx_free_motion(motion);
```

`ufbx_motion.nodes` is sorted by `ufbx_node.typed_id`; every entry contains the
node-to-world matrix and its per-second derivative. `ufbx_motion.instances`
contains one entry per node that has a mesh attached, also sorted by the node's
typed id, with world-space positions and velocities indexed by the mesh control
vertex index. The result owns all of its arrays, stores only source element ids,
and multiple results may be held simultaneously.

## Side semantics

At an exact keyframe, both the value and the derivative are taken from the
one-sided limit of the requested side:

- `UFBX_MOTION_LEFT` uses the segment ending at the keyframe (or the constant
  extension before the first key).
- `UFBX_MOTION_RIGHT` uses the segment starting at the keyframe (or the
  constant extension after the last key).

This makes stepped (`CONSTANT_PREV`/`CONSTANT_NEXT`) curves and continuous
keyframes with different slopes on either side well defined. Away from
keyframes the two sides return the same result.

## Curve support

All four interpolation modes are supported:

- `UFBX_INTERPOLATION_CONSTANT_PREV`, `UFBX_INTERPOLATION_CONSTANT_NEXT`:
  piecewise constant value, zero derivative.
- `UFBX_INTERPOLATION_LINEAR`: analytic segment slope.
- `UFBX_INTERPOLATION_CUBIC`: the curve is treated as a time-parameterized
  two-axis Bezier. The normalized segment time is never used as the Bezier
  parameter directly: the x-axis polynomial is solved for the Bezier parameter
  `u`, after which `dy/dt = (dy/du) / (dt/du)`. The time control points must be
  non-decreasing and `dt/du` must be strictly positive, otherwise the function
  reports failure.

Only constant extrapolation is accepted. An empty curve returns
`default_value` with zero derivative; a single-key curve and the regions
outside the keyframe range are constant. Invalid `time`/`side`, non-finite
results, and non-constant extrapolation make `ufbx_evaluate_curve_motion()`
return `false` without modifying the result argument.

## Scene support matrix

Supported:

- Zero or one animation layer; a fully static scene (no layers) evaluates to
  zero derivatives. Property and transform overrides are rejected.
- Animated node `Lcl Translation`, `Lcl Rotation`, and `Lcl Scaling`, and
  blend channel `DeformPercent`.
- All six Euler rotation orders. The stored `RotationOrder` is only honored
  when `RotationActive` is set, matching `ufbxi_get_transform()`; otherwise the
  lcl rotation is interpreted as XYZ.
- Static pre/post rotations (the full `pre * r * post_inv` chain), rotation and
  scaling pivots and offsets.
- Static geometry transforms.
- `UFBX_INHERIT_MODE_NORMAL` scale inheritance. Finite positive local scales.
- Linear blend skinning, including weight normalization and the
  no-effective-weight fallback convention used by
  `ufbx_get_skin_vertex_matrix()`. Non-identity `geometry_to_bone` matrices and
  the full parent bone chain contribute to the velocity.
- Sparse blend shapes, multiple blend channels/deformers, per-vertex offset
  weights, and in-between morph targets. At an exact in-between target weight
  the LEFT/RIGHT interval is selected to match the one-sided limit, including
  descending weights.
- Static nodes and static side branches, and meshes shared by multiple
  instances (one motion entry per instance).

Explicitly rejected with an error (never silently treated as static):

- More than one animation layer, property/transform overrides, constraints.
- Connected (`driven`) properties.
- Non-normal scale inheritance, scale/geometry-helper nodes, coordinate
  adjustment transforms.
- The spheric rotation order (when rotation space is active).
- Animated pivot, offset, or geometry-transform properties.
- Dual quaternion / blended dual quaternion / rigid skinning, multiple skin
  deformers per mesh, geometry caches.

## Deformation order

For each mesh the local morph target is accumulated first (sparse blend shape
offsets and their time derivatives), then linear skinning is applied:

- Skinned vertices are already in world space, so no instance matrix is
  multiplied on top.
- Vertices without effective skin weights use the instance
  geometry-to-world motion (including its derivative), matching ufbx.
- Unskinned meshes follow the instance geometry transform; their morph
  velocity is transformed as a direction vector.
