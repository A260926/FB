# Parent constraint evaluation

ufbx can parse Parent-Child (`UFBX_CONSTRAINT_PARENT`) constraints, but by
default they are only exposed through `ufbx_scene.constraints` and do not
affect node transforms. Setting `ufbx_evaluate_opts.evaluate_constraints` to
`true` makes `ufbx_evaluate_scene()` solve the supported Parent constraints as
part of the node hierarchy, before CPU skinning is evaluated. The option
defaults to `false`; when disabled, evaluation behaves exactly as before and
the old function signatures are unchanged.

## Supported range

The implementation is intentionally bounded. While at least one *effective*
constraint exists in a scene, evaluation fails (returns `NULL` with a
descriptive `ufbx_error`) unless every condition below holds.

- Each node has at most one active Parent constraint, with exactly one target.
- All translation channels (`constrain_translation[0..2]`) and all rotation
  channels (`constrain_rotation[0..2]`) are enabled; all scale channels
  (`constrain_scale[0..2]`) are disabled.
- Every scene node has unit local scale, `UFBX_INHERIT_MODE_NORMAL`
  inheritance, and no scale/geometry helper node or coordinate adjustment
  transform.
- The constraint's own `transform_offset` is identity (and finite).
- The single target offset may contain translation and rotation; its scale
  must be unit and all its components finite.
- Static rotation pivots, rotation offsets, pre/post rotation, the rotation
  order and geometry transforms keep working: they are already baked into the
  animated local transform and the geometry matrices.

Other active constraint types (aim, position, rotation, scale, IK), multiple
constraints or targets, partial axes, non-finite numbers, negative target
weights and out-of-range configurations all return `NULL`. The error message
names the relevant node and constraint and explains the reason.

Scenes with no effective constraint are not subject to these limits and are
evaluated exactly as before.

## Weights and active state

`active`, the overall `weight`, the target `weight` and the target offset are
all read from the values evaluated at the requested time.

- An inactive constraint is ignored and adds no dependency.
- The overall weight is clamped to `[0,1]`. A clamped weight of zero, or a
  target weight of zero, ignores the constraint completely.
- A non-finite overall or target weight, or a negative target weight, is an
  error. With a single positive target the relative weight normalizes to one.

## Solve

Matrices use column vectors. For a constrained node:

- `D` is the target pose: the target's final `node_to_world` multiplied by the
  target offset matrix.
- `G` is the blend base: the parent's final world matrix multiplied by the
  node's animated local matrix as it would be without the constraint. For a
  root node the parent matrix is identity.
- Translation is linearly interpolated and rotation is interpolated with
  shortest-arc SLERP by the clamped overall weight.
- The blended world pose is converted back to a local transform relative to
  the final parent pose.

The result is computed fresh for every call; blending never accumulates from a
previous evaluation. Parent hierarchy edges and target edges form the
dependency graph, which is solved topologically so the result does not depend
on declaration or connection order. A target may be a descendant of another
constrained node. If the combined graph contains a cycle, evaluation fails
with an error instead of silently reusing a stale pose.

After solving, `local_transform`, `node_to_parent`, `node_to_world`,
`geometry_to_world`, descendant nodes and skin cluster derived state are all
kept consistent. With `evaluate_skinning` also enabled, `skinned_position`
uses these final bone poses through the existing skinning path (morph targets
still run before skinning); no separate matrix set is created. The source
scene is never modified, repeated queries at the same time give the same
result, and queries in any time order are independent.

## C example

```c
#include "ufbx.h"

ufbx_scene *evaluate_with_parent_constraints(const ufbx_scene *source,
    const ufbx_anim *anim, double time, ufbx_error *error)
{
    ufbx_evaluate_opts opts = { 0 };
    opts.evaluate_constraints = true;
    opts.evaluate_skinning = true;

    ufbx_scene *evaluated = ufbx_evaluate_scene(source, anim, time, &opts, error);
    if (!evaluated) {
        char text[1024];
        ufbx_format_error(text, sizeof(text), error);
        // The message identifies the offending node/constraint and reason.
        return NULL;
    }
    return evaluated;
}
```
