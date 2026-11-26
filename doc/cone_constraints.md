# Cone (swing) constraints

Cone constraints limit how far a bone segment can swing away from a
reference direction. They participate directly in the FABRIK iterations:
after every backward/forward repositioning step an out-of-range segment is
rotated back onto the cone surface without changing its length or
detaching it from its parent.

## Attaching a constraint

A cone constraint limits the segment reaching **from the parent into the
node it is attached to**. To limit the bone between node `A` and its child
`B`, attach the constraint to `B`:

```c
struct ik_constraint_t* cone = solver->constraint->create(IK_CONE);
cone->cone_axis.x = 1.0f;   /* unit axis, parent-local space at rebuild */
cone->cone_axis.y = 0.0f;
cone->cone_axis.z = 0.0f;
cone->cone_angle = 0.5f;    /* half-angle in radians */
solver->constraint->attach(cone, B);

solver->flags |= IK_ENABLE_CONSTRAINTS;
ik.solver.set_tree(solver, root);
if (ik.solver.rebuild(solver) != IK_OK)
{
    /* the constraint data was invalid -- see validation below */
}
```

## Parameters

| Field        | Meaning                                                                                  |
|--------------|------------------------------------------------------------------------------------------|
| `cone_axis`  | Unit axis in the **parent node's local space**, captured from the reference (bind) pose |
| `cone_angle` | Half-angle of the allowed cone in radians. The default is `PI`, i.e. unrestricted       |

The solved segment may deviate from the (moving) cone axis by at most
`cone_angle`. A value of `0` pins the segment to the axis; `PI` disables
the limit.

### Validation at rebuild

`ik.solver.rebuild()` validates every cone constraint and binds the
reference pose. Rebuild fails with `IK_INVALID_CONSTRAINT` when:

* `cone_axis` is the zero vector or contains a non-finite value, or
* `cone_angle` is non-finite or lies outside `[0, PI]`.

On failure the previously bound data is dropped and `solve()` refuses to
run until a later rebuild succeeds -- the solver is never left in a
half-initialised, still-solvable state.

## Binding semantics

* Only **rebuild** binds the reference pose. It snapshots every node's
  local transform and normalises/stores each cone axis.
* `solve()` and effector target changes never re-bind anything. Edit the
  pose or the cone parameters and call `rebuild()` explicitly to take the
  new reference pose into account.
* The cone frame is **not** a fixed world axis. Each solve carries the bind
  frames down from the current animated root: animated root translation and
  rotation move the whole set of limits with the skeleton, and each
  ancestor's solved rotation moves the frames of its descendants.

## Orientation frames

For every node the solver maintains an orientation frame using this
convention:

1. The root frame is its current animated rotation.
2. Walking down the tree, the bind local rotations are accumulated.
3. The node's solved frame then applies the **shortest swing** that moves
   the bind incoming-bone direction onto the solved incoming-bone
   direction. The bind twist around the bone is preserved.
4. If the two directions are antiparallel, the swing axis is the bind-frame
   coordinate axis least parallel to the incoming bone; ties resolve in
   X, Y, Z order.

The cone axis follows this frame, so rotating a parent node cannot make the
limit meaningless.

## Joint rotations

With `IK_ENABLE_JOINT_ROTATIONS` enabled the exported local rotations are
derived from the same frames that enforced the cones, so the local
positions and rotations describe one consistent final pose. They are not
recomputed in a way that could override the constrained frame.

## Unsupported combination

Target rotations (`IK_ENABLE_TARGET_ROTATIONS`) and cone constraints cannot
be combined yet. Enabling both makes `solve()` return
`IK_CONFLICTING_SOLVER_OPTIONS` instead of silently ignoring one of them.

## Unreachable targets

When a target cannot be reached inside the cones, the solver leaves a
finite pose whose every limited segment satisfies its cone (typically on
the cone surface) and retains the remaining distance error rather than
violating a limit. Bone lengths and joint connectivity are always
preserved.

## Branched skeletons

Chains that share an ancestor are solved together: child chain targets are
averaged at their shared base node, each chain enforces its own cones, and
the shared node keeps a single coherent position -- it is never overwritten
by the last child chain.
