# Cone (IK_CONE) joint constraints

Cone constraints limit how far a bone segment can swing away from a reference
axis. They are used, for example, to keep shoulder or tail-root motion inside a
natural range of motion. Unlike the original no-op stub, the cone is enforced
**inside every FABRIK iteration**, so the returned skeleton always satisfies
the limits while keeping every bone at its bind length.

All interaction goes through the public `ik.solver` / `ik.constraint` entry
points; the constraint runtime data is rebuilt by `ik.solver.rebuild()`.

## Where the limit is attached

A constraint is attached to the **child** node of the segment it limits.

```
   parent
      \
       bone segment being limited
          \
           child   <- the ik_constraint_t (IK_CONE) is attached here
```

Attach and detach with the usual API, then rebuild before solving:

```c
struct ik_constraint_t* cone = s->constraint->create(IK_CONE);
s->constraint->attach(cone, child_node);
/* ... */
ik.solver.rebuild(solver);
```

## Parameters

The public `ik_constraint_t` gains two fields used by `IK_CONE`:

| Field        | Meaning                                                                 |
| ------------ | ----------------------------------------------------------------------- |
| `cone_axis`  | Unit centre axis of the cone, in the **parent node's local frame at bind time**. |
| `cone_angle` | Half-angle of the cone in radians, in the range `[0, pi]`.             |

```c
cone->cone_axis = ik.vec3.vec3(1.0, 0.0, 0.0); /* parent local +X */
cone->cone_angle = 3.14159265358979 / 6.0;     /* 30 degree half-angle */
```

Defaults:

* `cone_axis = (0, 0, 1)`
* `cone_angle = pi`

A half-angle of `pi` means "no directional restriction", so a freshly created
cone passes validation and leaves solving unchanged until you narrow the
angle. A half-angle of `0` locks the segment exactly onto the cone axis.

The cone axis is expressed in the parent's **bind** local frame, but it is not
a fixed world axis. During solving the axis is carried by the parent's solved
orientation frame, so animating the root (translation **and** rotation) or any
ancestor moves the whole limit with the skeleton.

## Reference frames and the bind pose

Each node's orientation frame is rebuilt on every backward pass using the
convention:

1. The root takes its current animated rotation.
2. Walking down, the bound local rotations are accumulated.
3. A shortest-arc swing then maps the node's bind in-bone direction onto its
   solved in-bone direction; the bind twist is preserved.
4. For antiparallel directions, the rotation axis is the bind-frame axis least
   parallel to the in-bone direction, with ties broken in `X, Y, Z` order.

This makes the limit independent of arbitrary parent-rotation choices: the
cone centre follows the node's own frame rather than a world direction.

### Rebinding

`ik.solver.rebuild()` explicitly re-binds the reference pose from the current
node transforms. Plain `solve()` calls and effector target changes **never**
re-bind silently. To move the limit to a new animated rest pose, set the node
rotations/positions and call `rebuild()` again.

## Validation

`ik.solver.rebuild()` validates every cone and returns an error instead of
leaving a half-built, still-solvable state:

* zero-length or non-finite `cone_axis`
* non-finite `cone_angle`, or an angle outside `[0, pi]`
* a cone attached to the tree root (there is no entering segment to limit)

These return `IK_INVALID_CONSTRAINT`. On failure the internal chain and cone
tables are cleared, so the solver cannot be used until a successful rebuild.

## Solver flags

* Enable enforcement with `IK_ENABLE_CONSTRAINTS`. With the flag clear, cones
  are ignored and ordinary FABRIK runs unchanged.
* `IK_ENABLE_JOINT_ROTATIONS` is fully supported. The reported local rotations
  are derived from the same cone-satisfying frames as the positions, so
  rotations and positions always describe one consistent final pose (the
  rotations are not recomputed in a way that could move the cone frame).
* `IK_ENABLE_TARGET_ROTATIONS` cannot currently be combined with cone
  constraints. Enabling both is rejected with `IK_INCOMPATIBLE_FLAGS` from
  both `rebuild()` and `solve()` rather than silently dropping either feature.

## Reachable and unreachable targets

* A reachable target inside the cones is hit normally.
* An unreachable target may leave a residual distance error, but the output
  pose is always finite, every segment keeps its bind length, child joints
  stay connected, and every cone is satisfied (segments end up on the cone
  boundary).
* On branched skeletons the shared (sub-base) node is solved once and shared
  by every child chain; a later chain never overwrites it.

## Full example

```c
struct ik_solver_t* s = ik.solver.create(IK_FABRIK);

struct ik_node_t* root = s->node->create(0);
struct ik_node_t* a    = s->node->create(1);
struct ik_node_t* b    = s->node->create(2);
root->rotation.w = 1.0;
a->position = ik.vec3.vec3(1.0, 0.0, 0.0);
b->position = ik.vec3.vec3(1.0, 0.0, 0.0);
a->rotation.w = b->rotation.w = 1.0;
s->node->add_child(root, a);
s->node->add_child(a, b);

struct ik_effector_t* eff = s->effector->create();
eff->target_position = ik.vec3.vec3(1.5, 1.0, 0.0);
s->effector->attach(eff, b);

/* limit the root->a segment to a 30 degree cone around parent-local +X */
struct ik_constraint_t* cone = s->constraint->create(IK_CONE);
cone->cone_axis  = ik.vec3.vec3(1.0, 0.0, 0.0);
cone->cone_angle = 3.14159265358979 / 6.0;
s->constraint->attach(cone, a);

s->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
ik.solver.set_tree(s, root);

if (ik.solver.rebuild(s) != IK_OK)
{
    /* invalid cone parameters: fix them and rebuild; do not solve */
}

ik.solver.solve(s);
```

## Programmatic regressions

The cone behaviour is covered by `test/cone_cases.c`, registered through
`task_support/check.h` in three groups:

* `cone`    -- single chains, validation, flag conflicts, root animation
* `branch`  -- two end-effector chains sharing an ancestor
* `rebind`  -- explicit re-binding and stability across target changes

Build and run without a window or graphics device:

```sh
bash task_support/run.sh baseline  # unconstrained reference behaviour
bash task_support/run.sh target    # production library + cone regressions
```
