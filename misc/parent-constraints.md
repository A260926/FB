# Parent-Child constraints in `ufbx_evaluate_scene()`

ufbx normally reads `Parent-Child` constraints from the FBX file and exposes
them through `ufbx_scene.constraints`, but evaluating a scene only follows the
node parent/child hierarchy. This document describes the opt-in support for
applying `Parent-Child` constraints during scene evaluation and using the
resulting bone poses for CPU skinning.

## Enabling

Set `ufbx_evaluate_opts.evaluate_constraints` to `true`. The flag defaults to
`false` and the behavior of `ufbx_evaluate_scene()` is unchanged when it is
off; existing code and function signatures are untouched.

```c
ufbx_evaluate_opts opts = { 0 };
opts.evaluate_skinning = true;
opts.evaluate_constraints = true;

ufbx_error error;
ufbx_scene *evaluated = ufbx_evaluate_scene(scene, scene->anim, time, &opts, &error);
if (!evaluated) {
	char message[1024];
	ufbx_format_error(message, sizeof(message), &error);
	fprintf(stderr, "%s\n", message);
}
```

The returned object is a normal evaluated `ufbx_scene`: nodes, skin clusters,
meshes and constraints are regular evaluated elements that reference the
evaluated scene. Free it with `ufbx_free_scene()`. The source scene is never
modified, and evaluating the same time repeatedly (or querying times in any
order) always produces the same result.

## Supported subset

The implementation intentionally accepts only a small, well-defined subset of
constraints. If any *effective* constraint is outside this subset,
`ufbx_evaluate_scene()` fails and returns `NULL` with an error describing the
offending node/constraint and the reason.

A constraint is **effective** when `active` is true, its overall weight
(`Weight / 100`) is greater than zero, and its single target has a weight
greater than zero. Ignored constraints are simply skipped: they do not
contribute a dependency and do not impose any of the restrictions below.

For every effective constraint:

- Type must be `UFBX_CONSTRAINT_PARENT` (`"Parent-Child"`). Other types
  (`Aim`, `Position`, `Rotation`, `Scale`, `Single Chain IK`, `Unknown`) are
  rejected.
- `ufbx_constraint.node` must be a node of the scene, and there must be
  exactly **one** target.
- The target node must be part of the scene.
- All translation axes (`constrain_translation[0..2]`) and all rotation axes
  (`constrain_rotation[0..2]`) must be enabled.
- All scale axes (`constrain_scale[0..2]`) must be disabled.
- The target offset (`ufbx_constraint_target.transform`) may translate and
  rotate; its scale must be unit (within `1e-6`) and its rotation must be a
  non-degenerate quaternion. The rotation is normalized before use.
- `ufbx_constraint.transform_offset` must be identity.
- The target weight must be finite and non-negative. A positive target weight
  is normalized to one, since there is only a single target.
- The overall weight must be finite; it is clamped to `[0, 1]`.

Additionally, if at least one effective constraint exists, every node of the
scene must satisfy:

- Unit local scale (within `1e-6`).
- `UFBX_INHERIT_MODE_NORMAL`.
- No scale or geometry transform helper (`scale_helper`,
  `geometry_transform_helper`, `is_scale_helper`,
  `is_geometry_transform_helper`, `is_scale_compensate_parent`,
  `inherit_scale_node`).
- No coordinate adjustment transforms (`has_adjust_transform`,
  `has_root_adjust_transform`, `adjust_mirror_axis`).

Static pivots, pre/post rotations, rotation order and geometry transforms are
still honored because the unconstrained local transform is computed with the
regular property evaluation. Non-finite (NaN/Inf) or out-of-range
(absolute value above `1e30`) numbers in relevant values are rejected as well.

When there are no effective constraints the original evaluation path is used
verbatim, including all of the above restrictions being irrelevant.

## Math

The solver works with column-vector matrices, exactly like the rest of ufbx.
For each constrained node let:

- `G` be the *blend base*: `parent_final_world * animated_local_without_constraint`,
  where the parent matrix of the root node is identity.
- `D` be the *target pose*: `target_final_world * target_offset_matrix`.

The blended world matrix is built by interpolating translation linearly and
rotation with shortest-arc SLERP, using the effective overall weight `w`:

```
blended.translation = G.translation + (D.translation - G.translation) * w
blended.rotation    = slerp(G.rotation, D.rotation, w)
blended.scale       = (1, 1, 1)
```

The new local transform is `inverse(parent_final_world) * blended`, which also
keeps `node_to_parent`, `node_to_world`, `geometry_to_world`, descendants and
the skin clusters consistent. Blending is always performed against the
animated local pose of the current time, never against a previous constraint
result.

Nodes are solved in a topological order over the combined graph of
parent/child edges and effective target dependencies. The target of a
constraint may be a descendant of another constrained node, and the result
does not depend on the declaration or connection order of constraints. If the
union of those dependencies contains a cycle, evaluation fails instead of
silently using a stale pose.

## Skinning

When `evaluate_skinning` is enabled together with `evaluate_constraints`, the
usual CPU skinning runs after the constraints have been solved, using the
final bone `node_to_world` matrices via each cluster's `geometry_to_world`.
The order is unchanged: morph/blend offsets are applied first, then the skin
cluster matrices. No parallel set of matrices is derived outside the scene;
`ufbx_mesh.skinned_position`, `skinned_normal` and `skinned_is_local` follow
the regular evaluation rules.

## Example: constraint chain

Given the ASCII FBX snippet below (see `task_support/parent_chain.fbx` for a
complete document), node `A` is constrained to follow node `T` and node `B`
follows node `C`:

```
Constraint: 21, "Constraint::FollowT", "Parent-Child" {
    Type: "Parent-Child"
    Properties70: {
        P: "Constrained object (Child)", "object", "", ""
        P: "Active", "bool", "", "",1
        P: "Weight", "Number", "", "A",100
        P: "T.Weight", "Number", "", "A",100
        P: "T.Offset T", "Translation", "", "A",1,0,0
    }
}
```

Evaluating the scene with constraints:

```c
#include "ufbx.h"
#include <stdio.h>

int main(void)
{
	ufbx_error error = { 0 };
	ufbx_load_opts load_opts = { 0 };
	ufbx_scene *scene = ufbx_load_file("parent_chain.fbx", &load_opts, &error);
	if (!scene) {
		char message[1024];
		ufbx_format_error(message, sizeof(message), &error);
		fprintf(stderr, "load failed: %s\n", message);
		return 1;
	}

	ufbx_evaluate_opts opts = { 0 };
	opts.evaluate_constraints = true;
	opts.evaluate_skinning = true;

	ufbx_scene *evaluated = ufbx_evaluate_scene(scene, scene->anim, 0.0, &opts, &error);
	if (!evaluated) {
		char message[1024];
		ufbx_format_error(message, sizeof(message), &error);
		fprintf(stderr, "evaluate failed: %s\n", message);
		ufbx_free_scene(scene);
		return 1;
	}

	const ufbx_node *a = ufbx_find_node(evaluated, "A");
	const ufbx_mesh *mesh = evaluated->meshes.data[0];

	printf("A world = (%.3f, %.3f, %.3f)\n",
		a->node_to_world.m03, a->node_to_world.m13, a->node_to_world.m23);

	if (mesh->skinned_position.values.count > 0) {
		ufbx_vec3 v = mesh->skinned_position.values.data[0];
		printf("first skinned vertex = (%.3f, %.3f, %.3f)\n", v.x, v.y, v.z);
	}

	ufbx_free_scene(evaluated);
	ufbx_free_scene(scene);
	return 0;
}
```

## Error reporting

Failures populate `ufbx_error` through the usual mechanism. `ufbx_error.info`
contains the constraint name, the constrained node name and, when relevant,
the target node name, followed by the reason. For example:

```
ufbx v0.23.1 error: Constraint dependency cycle (constraint "FollowC" on node "B" targeting "D": Constraint dependency cycle)
```

`ufbx_format_error()` prints the description, the info in parentheses and the
error stack. On failure no partial scene is returned and temporary allocations
are released the same way as for other evaluation errors.
