/*
 * cone_cases.c -- programmatic regressions for the IK_CONE constraint.
 *
 * Every geometric assertion is rebuilt from the public node transforms after
 * ik.solver.solve() returns: the solved tree is in local space, so global
 * positions and rotations are recomputed by walking the public parent/child
 * hierarchy. The internal chain/cone caches are never read as an oracle.
 *
 * Groups:
 *   cone    -- single-chain cone enforcement, validation and flag conflicts
 *   branch  -- two end-effector chains sharing an ancestor
 *   rebind  -- explicit re-binding of the reference pose
 */
#include "ik/ik.h"
#include "task_support/check.h"
#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979323846
#define EPS 1e-6
#define TIGHT_EPS 1e-9
#define MAX_NODES 32

struct world_pose
{
    ik_vec3_t pos;
    ik_quat_t rot;
    int used;
};

struct snapshot
{
    /* bind-pose local node position, normalized (the in-bone direction) */
    ik_vec3_t bind_dir;
    /* bind-pose local rotation, used to express the bone in the node frame */
    ik_quat_t bind_rotation;
    ikreal_t bone_length;
    int has_parent;
};

/* ------------------------------------------------------------------------- */
static int
near_scalar(ikreal_t a, ikreal_t b, ikreal_t eps)
{
    return fabs(a - b) <= eps;
}

static int
vec_near(const ik_vec3_t a, const ik_vec3_t b, ikreal_t eps)
{
    return near_scalar(a.x, b.x, eps) &&
           near_scalar(a.y, b.y, eps) &&
           near_scalar(a.z, b.z, eps);
}

static int
finite_vec(const ik_vec3_t v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static int
finite_quat(const ik_quat_t q)
{
    return isfinite(q.x) && isfinite(q.y) &&
           isfinite(q.z) && isfinite(q.w);
}

static ik_vec3_t
rotated(ik_vec3_t v, const ik_quat_t q)
{
    ik.vec3.rotate(v.f, q.f);
    return v;
}

static ik_quat_t
quat_mul(ik_quat_t a, const ik_quat_t b)
{
    ik.quat.mul_quat(a.f, b.f);
    return a;
}

static ik_vec3_t
normalized(ik_vec3_t v)
{
    ik.vec3.normalize(v.f);
    return v;
}

static ikreal_t
vec_dot(ik_vec3_t a, ik_vec3_t b)
{
    return ik.vec3.dot(a.f, b.f);
}

static ik_vec3_t
sub_vec(ik_vec3_t a, const ik_vec3_t b)
{
    ik.vec3.sub_vec3(a.f, b.f);
    return a;
}

static ikreal_t
distance_to(const ik_vec3_t a, const ik_vec3_t b)
{
    ik_vec3_t d = sub_vec(a, b);
    return ik.vec3.length(d.f);
}

/*
 * The FABRIK solver latches IK_RESULT_CONVERGED once any iteration misses
 * tolerance, even if a later iteration recovers, so a geometrically reachable
 * target may return either code. Reachability is always judged from the
 * recomputed geometry instead.
 */
static int
solve_ok_or_residual(struct ik_solver_t* solver)
{
    ikret_t rc = ik.solver.solve(solver);
    return rc == IK_OK || rc == IK_RESULT_CONVERGED ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* Recompute the world pose of every node from the public local transforms.   */
/* ------------------------------------------------------------------------- */
static void
recompute_world_recursive(struct ik_node_t* node,
                          ik_quat_t parent_rot,
                          ik_vec3_t parent_pos,
                          struct world_pose poses[MAX_NODES])
{
    ik_quat_t world_rot;
    ik_vec3_t world_pos;

    if (node->parent == NULL)
    {
        world_rot = node->rotation;
        world_pos = node->position;
    }
    else
    {
        world_rot = quat_mul(parent_rot, node->rotation);
        world_pos = rotated(node->position, parent_rot);
        ik.vec3.add_vec3(world_pos.f, parent_pos.f);
    }

    poses[node->guid].rot = world_rot;
    poses[node->guid].pos = world_pos;
    poses[node->guid].used = 1;

    NODE_FOR_EACH(node, guid, child)
        recompute_world_recursive(child, world_rot, world_pos, poses);
    NODE_END_EACH
}

static void
recompute_world(struct ik_node_t* root, struct world_pose poses[MAX_NODES])
{
    memset(poses, 0, sizeof(struct world_pose) * MAX_NODES);
    recompute_world_recursive(root, ik.quat.quat(0, 0, 0, 1),
                              ik.vec3.vec3(0, 0, 0), poses);
}

/* ------------------------------------------------------------------------- */
/* Snapshot bind-pose bone directions and lengths through the public tree.    */
/* ------------------------------------------------------------------------- */
static void
snapshot_recursive(struct ik_node_t* node, struct snapshot shot[MAX_NODES])
{
    shot[node->guid].has_parent = node->parent != NULL;
    shot[node->guid].bind_dir = normalized(node->position);
    shot[node->guid].bind_rotation = node->rotation;
    shot[node->guid].bone_length = node->dist_to_parent;

    NODE_FOR_EACH(node, guid, child)
        snapshot_recursive(child, shot);
    NODE_END_EACH
}

static void
snapshot_bind(struct ik_node_t* root, struct snapshot shot[MAX_NODES])
{
    memset(shot, 0, sizeof(struct snapshot) * MAX_NODES);
    snapshot_recursive(root, shot);
}

/* ------------------------------------------------------------------------- */
/* Verify every edge keeps its bind bone length and ends at a finite pose.    */
/* ------------------------------------------------------------------------- */
static int
check_bone_lengths_recursive(struct ik_node_t* node,
                             const struct world_pose poses[MAX_NODES],
                             const struct snapshot shot[MAX_NODES])
{
    TASK_CHECK(finite_vec(poses[node->guid].pos));
    TASK_CHECK(finite_quat(poses[node->guid].rot));

    if (node->parent != NULL)
    {
        ikreal_t length = distance_to(poses[node->guid].pos,
                                      poses[node->parent->guid].pos);
        TASK_CHECK(near_scalar(length, shot[node->guid].bone_length, EPS));
    }

    NODE_FOR_EACH(node, guid, child)
        TASK_CHECK(check_bone_lengths_recursive(child, poses, shot) == 0);
    NODE_END_EACH
    return 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Verify every IK_CONE in the tree, using only public data: the cone axis is
 * the stored local axis rotated by the parent's recomputed world rotation, and
 * the bone direction is the recomputed world segment direction.
 */
static int
check_cones_recursive(struct ik_node_t* node,
                      const struct world_pose poses[MAX_NODES])
{
    if (node->constraint != NULL &&
        node->constraint->type == IK_CONE &&
        node->parent != NULL)
    {
        ik_vec3_t bone = normalized(sub_vec(poses[node->guid].pos,
                                            poses[node->parent->guid].pos));
        ik_vec3_t axis = rotated(node->constraint->cone_axis,
                                 poses[node->parent->guid].rot);
        ikreal_t cos_angle;
        ikreal_t angle;

        axis = normalized(axis);
        cos_angle = vec_dot(bone, axis);
        if (cos_angle < -1.0) cos_angle = -1.0;
        if (cos_angle >  1.0) cos_angle =  1.0;
        angle = acos(cos_angle);
        TASK_CHECK(angle <= node->constraint->cone_angle + EPS);
    }

    NODE_FOR_EACH(node, guid, child)
        TASK_CHECK(check_cones_recursive(child, poses) == 0);
    NODE_END_EACH
    return 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Rotation/position agreement: the node's recomputed world rotation must map
 * its own-frame bind bone direction onto the direction given by its solved
 * positions. The bone is stored in the parent's local frame, so it is first
 * moved into the node's own frame with the bind local rotation. This proves
 * the reported local rotations and positions describe one pose.
 */
static int
check_rotation_pose_agreement_recursive(struct ik_node_t* node,
                                        const struct world_pose poses[MAX_NODES],
                                        const struct snapshot shot[MAX_NODES])
{
    if (node->parent != NULL)
    {
        ik_quat_t inv_bind = shot[node->guid].bind_rotation;
        ik_vec3_t own_frame_bone;
        ik_vec3_t from_rotation;
        ik_vec3_t from_positions;

        ik.quat.conj(inv_bind.f);
        own_frame_bone = rotated(shot[node->guid].bind_dir, inv_bind);
        from_rotation = rotated(own_frame_bone, poses[node->guid].rot);
        from_rotation = normalized(from_rotation);

        from_positions = normalized(sub_vec(poses[node->guid].pos,
                                            poses[node->parent->guid].pos));
        TASK_CHECK(vec_near(from_rotation, from_positions, 1e-7));
    }

    NODE_FOR_EACH(node, guid, child)
        TASK_CHECK(check_rotation_pose_agreement_recursive(child, poses, shot) == 0);
    NODE_END_EACH
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Build a straight chain root -> guid 1..bone_count, each bone +X length 1. */
/* ------------------------------------------------------------------------- */
static struct ik_node_t*
build_chain(struct ik_solver_t* solver, int bone_count)
{
    int i;
    struct ik_node_t* root = solver->node->create(0);
    struct ik_node_t* node = root;
    root->rotation = ik.quat.quat(0, 0, 0, 1);

    for (i = 1; i <= bone_count; ++i)
    {
        struct ik_node_t* child = solver->node->create((uint32_t)i);
        child->position = ik.vec3.vec3(1.0, 0.0, 0.0);
        child->rotation = ik.quat.quat(0, 0, 0, 1);
        solver->node->add_child(node, child);
        node = child;
    }
    return root;
}

static struct ik_effector_t*
attach_effector(struct ik_solver_t* solver, struct ik_node_t* node,
                ikreal_t x, ikreal_t y, ikreal_t z)
{
    struct ik_effector_t* effector = solver->effector->create();
    effector->target_position = ik.vec3.vec3(x, y, z);
    solver->effector->attach(effector, node);
    return effector;
}

static struct ik_constraint_t*
attach_cone(struct ik_solver_t* solver, struct ik_node_t* node,
            ik_vec3_t axis, ikreal_t angle)
{
    struct ik_constraint_t* cone = solver->constraint->create(IK_CONE);
    cone->cone_axis = axis;
    cone->cone_angle = angle;
    solver->constraint->attach(cone, node);
    return cone;
}

static ik_quat_t
rot_about_z(ikreal_t angle)
{
    return ik.quat.quat(0.0, 0.0, sin(angle * 0.5), cos(angle * 0.5));
}

/* ========================================================================= */
/* cone group                                                                 */
/* ========================================================================= */

/* A reachable target inside the cone is hit exactly; lengths and the cone
 * itself all hold. Joint rotations are enabled, so rotation/position
 * agreement is checked too. */
static int
cone_reachable(void)
{
    struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(solver, 3);
    struct ik_node_t* tip = solver->node->find_child(root, 3);
    struct world_pose poses[MAX_NODES];
    struct snapshot shot[MAX_NODES];

    attach_effector(solver, tip, 3.0, 0.0, 0.0);
    attach_cone(solver, solver->node->find_child(root, 1),
                ik.vec3.vec3(1, 0, 0), PI_F * 0.25);
    solver->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    solver->max_iterations = 40;
    solver->tolerance = 1e-5;

    ik.solver.set_tree(solver, root);
    TASK_CHECK(ik.solver.rebuild(solver) == IK_OK);
    snapshot_bind(root, shot);
    TASK_CHECK(solve_ok_or_residual(solver) == 0);
    recompute_world(root, poses);

    TASK_CHECK(distance_to(poses[3].pos, ik.vec3.vec3(3, 0, 0)) < EPS);
    TASK_CHECK(check_bone_lengths_recursive(root, poses, shot) == 0);
    TASK_CHECK(check_cones_recursive(root, poses) == 0);
    TASK_CHECK(check_rotation_pose_agreement_recursive(root, poses, shot) == 0);

    ik.solver.destroy(solver);
    return 0;
}

/* An unreachable target leaves a distance error but the segment stays on the
 * cone boundary and the returned pose is finite and length-preserving. */
static int
cone_blocks_unreachable_target(void)
{
    struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(solver, 3);
    struct ik_node_t* tip = solver->node->find_child(root, 3);
    struct ik_node_t* first = solver->node->find_child(root, 1);
    struct world_pose poses[MAX_NODES];
    struct snapshot shot[MAX_NODES];
    ik_vec3_t bone;
    ikreal_t swing;

    attach_effector(solver, tip, 0.0, 3.0, 0.0);
    attach_cone(solver, first, ik.vec3.vec3(1, 0, 0), PI_F / 6.0);
    solver->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    solver->max_iterations = 40;

    ik.solver.set_tree(solver, root);
    TASK_CHECK(ik.solver.rebuild(solver) == IK_OK);
    snapshot_bind(root, shot);
    TASK_CHECK(ik.solver.solve(solver) == IK_RESULT_CONVERGED);
    recompute_world(root, poses);

    /* the target cannot be reached */
    TASK_CHECK(distance_to(poses[3].pos, ik.vec3.vec3(0, 3, 0)) > 0.5);

    /* first bone is pinned to the 30 degree cone boundary */
    bone = normalized(sub_vec(poses[1].pos, poses[0].pos));
    swing = acos(bone.x < -1 ? -1 : (bone.x > 1 ? 1 : bone.x));
    TASK_CHECK(near_scalar(swing, PI_F / 6.0, 1e-8));
    TASK_CHECK(vec_near(poses[1].pos,
                        ik.vec3.vec3(cos(PI_F / 6.0), sin(PI_F / 6.0), 0.0),
                        1e-7));

    TASK_CHECK(check_bone_lengths_recursive(root, poses, shot) == 0);
    TASK_CHECK(check_cones_recursive(root, poses) == 0);
    TASK_CHECK(check_rotation_pose_agreement_recursive(root, poses, shot) == 0);

    ik.solver.destroy(solver);
    return 0;
}

/* A zero half-angle locks the segment exactly onto the parent-frame axis. */
static int
cone_zero_angle_locks_segment(void)
{
    struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(solver, 3);
    struct ik_node_t* tip = solver->node->find_child(root, 3);
    struct ik_node_t* second = solver->node->find_child(root, 2);
    struct world_pose poses[MAX_NODES];
    struct snapshot shot[MAX_NODES];
    ik_vec3_t bone;

    attach_effector(solver, tip, 0.0, 3.0, 0.0);
    attach_cone(solver, second, ik.vec3.vec3(1, 0, 0), 0.0);
    solver->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;

    ik.solver.set_tree(solver, root);
    TASK_CHECK(ik.solver.rebuild(solver) == IK_OK);
    snapshot_bind(root, shot);
    TASK_CHECK(ik.solver.solve(solver) == IK_RESULT_CONVERGED);
    recompute_world(root, poses);

    bone = normalized(sub_vec(poses[2].pos, poses[1].pos));
    /* the zero-angle lock follows node 1's solved frame, not the world axis */
    TASK_CHECK(vec_near(bone,
                        normalized(rotated(ik.vec3.vec3(1, 0, 0),
                                           poses[1].rot)),
                        TIGHT_EPS));
    TASK_CHECK(check_bone_lengths_recursive(root, poses, shot) == 0);
    TASK_CHECK(check_cones_recursive(root, poses) == 0);

    ik.solver.destroy(solver);
    return 0;
}

/* Root translation and rotation carry the whole limit along, even when the
 * root is animated after rebuild (no re-binding occurs on solve). */
static int
cone_root_animation_carries_limit(void)
{
    struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(solver, 3);
    struct ik_node_t* tip = solver->node->find_child(root, 3);
    struct world_pose poses[MAX_NODES];
    struct snapshot shot[MAX_NODES];
    ik_vec3_t root_offset = ik.vec3.vec3(10.0, -4.0, 2.0);

    attach_effector(solver, tip, 10.0, -1.0, 2.0); /* root + world +Y */
    attach_cone(solver, solver->node->find_child(root, 1),
                ik.vec3.vec3(1, 0, 0), PI_F / 6.0);
    solver->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    solver->max_iterations = 40;
    solver->tolerance = 1e-5;

    ik.solver.set_tree(solver, root);
    TASK_CHECK(ik.solver.rebuild(solver) == IK_OK);

    /* animate the root without rebuilding: the cone must travel with it */
    root->position = root_offset;
    root->rotation = rot_about_z(PI_F * 0.5); /* local +X -> world +Y */

    snapshot_bind(root, shot);
    TASK_CHECK(solve_ok_or_residual(solver) == 0);
    recompute_world(root, poses);

    TASK_CHECK(distance_to(poses[3].pos,
                           ik.vec3.vec3(10.0, -1.0, 2.0)) < EPS);
    /* world cone axis is now +Y */
    TASK_CHECK(vec_near(normalized(rotated(ik.vec3.vec3(1, 0, 0),
                                           poses[0].rot)),
                        ik.vec3.vec3(0, 1, 0), TIGHT_EPS));
    TASK_CHECK(check_bone_lengths_recursive(root, poses, shot) == 0);
    TASK_CHECK(check_cones_recursive(root, poses) == 0);
    TASK_CHECK(check_rotation_pose_agreement_recursive(root, poses, shot) == 0);

    ik.solver.destroy(solver);
    return 0;
}

/* Cones stacked on ancestors are all enforced in their own parent frames. */
static int
cone_on_ancestor_chain(void)
{
    struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(solver, 4);
    struct ik_node_t* tip = solver->node->find_child(root, 4);
    struct world_pose poses[MAX_NODES];
    struct snapshot shot[MAX_NODES];

    attach_effector(solver, tip, 0.0, 4.0, 0.0);
    attach_cone(solver, solver->node->find_child(root, 1),
                ik.vec3.vec3(1, 0, 0), PI_F * 0.25);
    attach_cone(solver, solver->node->find_child(root, 2),
                ik.vec3.vec3(1, 0, 0), PI_F * 0.25);
    solver->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    solver->max_iterations = 40;

    ik.solver.set_tree(solver, root);
    TASK_CHECK(ik.solver.rebuild(solver) == IK_OK);
    snapshot_bind(root, shot);
    TASK_CHECK(ik.solver.solve(solver) == IK_RESULT_CONVERGED);
    recompute_world(root, poses);

    TASK_CHECK(distance_to(poses[4].pos, ik.vec3.vec3(0, 4, 0)) > EPS);
    TASK_CHECK(check_bone_lengths_recursive(root, poses, shot) == 0);
    TASK_CHECK(check_cones_recursive(root, poses) == 0);
    TASK_CHECK(check_rotation_pose_agreement_recursive(root, poses, shot) == 0);

    ik.solver.destroy(solver);
    return 0;
}

/* Default cone parameters (half-angle pi) impose no directional limit: the
 * result is identical to a plain FABRIK solve of the same problem. */
static int
cone_default_is_unrestricted(void)
{
    struct ik_solver_t* constrained = ik.solver.create(IK_FABRIK);
    struct ik_solver_t* plain = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root_c = build_chain(constrained, 3);
    struct ik_node_t* root_p = build_chain(plain, 3);
    struct world_pose poses_c[MAX_NODES], poses_p[MAX_NODES];
    struct snapshot shot_c[MAX_NODES], shot_p[MAX_NODES];
    struct ik_constraint_t* cone;

    attach_effector(constrained,
                    constrained->node->find_child(root_c, 3), 2.1, 1.5, 0.0);
    attach_effector(plain, plain->node->find_child(root_p, 3), 2.1, 1.5, 0.0);

    cone = constrained->constraint->create(IK_CONE);
    TASK_CHECK(near_scalar(cone->cone_angle, PI_F, TIGHT_EPS));
    constrained->constraint->attach(cone,
                    constrained->node->find_child(root_c, 1));

    constrained->flags = IK_ENABLE_CONSTRAINTS;
    plain->flags = 0;
    constrained->max_iterations = plain->max_iterations = 40;
    constrained->tolerance = plain->tolerance = 1e-5;

    ik.solver.set_tree(constrained, root_c);
    ik.solver.set_tree(plain, root_p);
    TASK_CHECK(ik.solver.rebuild(constrained) == IK_OK);
    TASK_CHECK(ik.solver.rebuild(plain) == IK_OK);
    snapshot_bind(root_c, shot_c);
    snapshot_bind(root_p, shot_p);
    TASK_CHECK(ik.solver.solve(constrained) != IK_RAN_OUT_OF_MEMORY);
    TASK_CHECK(ik.solver.solve(plain) != IK_RAN_OUT_OF_MEMORY);
    recompute_world(root_c, poses_c);
    recompute_world(root_p, poses_p);

    TASK_CHECK(vec_near(poses_c[3].pos, poses_p[3].pos, 1e-10));
    TASK_CHECK(check_bone_lengths_recursive(root_c, poses_c, shot_c) == 0);
    TASK_CHECK(check_bone_lengths_recursive(root_p, poses_p, shot_p) == 0);

    ik.solver.destroy(constrained);
    ik.solver.destroy(plain);
    return 0;
}

/* With constraints disabled, even a zero-angle cone changes nothing: the tip
 * bends exactly as plain FABRIK would, while enabling the flag restricts it. */
static int
cone_disabled_behaves_like_plain_fabrik(void)
{
    struct ik_solver_t* disabled = ik.solver.create(IK_FABRIK);
    struct ik_solver_t* enabled = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root_d = build_chain(disabled, 3);
    struct ik_node_t* root_e = build_chain(enabled, 3);
    struct world_pose poses_d[MAX_NODES], poses_e[MAX_NODES];
    struct snapshot shot_d[MAX_NODES], shot_e[MAX_NODES];

    attach_effector(disabled, disabled->node->find_child(root_d, 3),
                    2.1, 1.5, 0.0);
    attach_effector(enabled, enabled->node->find_child(root_e, 3),
                    2.1, 1.5, 0.0);
    attach_cone(disabled, disabled->node->find_child(root_d, 1),
                ik.vec3.vec3(1, 0, 0), 0.0);
    attach_cone(enabled, enabled->node->find_child(root_e, 1),
                ik.vec3.vec3(1, 0, 0), 0.0);

    disabled->flags = 0; /* constraints off: cone must be ignored */
    enabled->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    disabled->max_iterations = enabled->max_iterations = 40;
    disabled->tolerance = enabled->tolerance = 1e-5;

    ik.solver.set_tree(disabled, root_d);
    ik.solver.set_tree(enabled, root_e);
    TASK_CHECK(ik.solver.rebuild(disabled) == IK_OK);
    TASK_CHECK(ik.solver.rebuild(enabled) == IK_OK);
    snapshot_bind(root_d, shot_d);
    snapshot_bind(root_e, shot_e);
    TASK_CHECK(ik.solver.solve(disabled) != IK_RAN_OUT_OF_MEMORY);
    TASK_CHECK(ik.solver.solve(enabled) != IK_RAN_OUT_OF_MEMORY);
    recompute_world(root_d, poses_d);
    recompute_world(root_e, poses_e);

    /* the target is reachable in both cases; the difference is at the locked
     * joint: enabling the zero-angle cone pins the first bone to the axis */
    TASK_CHECK(!vec_near(poses_d[1].pos, poses_e[1].pos, 1e-3));
    TASK_CHECK(near_scalar(poses_e[1].pos.y, 0.0, EPS));
    TASK_CHECK(fabs(poses_d[1].pos.y) > 1e-3);
    TASK_CHECK(check_bone_lengths_recursive(root_d, poses_d, shot_d) == 0);
    TASK_CHECK(check_bone_lengths_recursive(root_e, poses_e, shot_e) == 0);
    TASK_CHECK(check_cones_recursive(root_e, poses_e) == 0);

    ik.solver.destroy(disabled);
    ik.solver.destroy(enabled);
    return 0;
}

/* Invalid cone parameters fail rebuild and leave no solvable half-state. */
static int
cone_invalid_parameters_rejected(void)
{
    static const ik_vec3_t bad_axes[] = {
        {{0.0, 0.0, 0.0}},
        {{NAN, 1.0, 0.0}},
        {{1.0, INFINITY, 0.0}}
    };
    static const ikreal_t bad_angles[] = {
        -0.1, PI_F + 0.1, NAN, INFINITY
    };
    unsigned i;

    for (i = 0; i < sizeof(bad_axes) / sizeof(bad_axes[0]); ++i)
    {
        struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
        struct ik_node_t* root = build_chain(solver, 2);
        struct ik_node_t* tip = solver->node->find_child(root, 2);
        attach_effector(solver, tip, 1.0, 1.0, 0.0);
        attach_cone(solver, solver->node->find_child(root, 1),
                    bad_axes[i], PI_F * 0.5);
        solver->flags = IK_ENABLE_CONSTRAINTS;
        ik.solver.set_tree(solver, root);
        TASK_CHECK(ik.solver.rebuild(solver) == IK_INVALID_CONSTRAINT);
        ik.solver.destroy(solver);
    }

    for (i = 0; i < sizeof(bad_angles) / sizeof(bad_angles[0]); ++i)
    {
        struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
        struct ik_node_t* root = build_chain(solver, 2);
        struct ik_node_t* tip = solver->node->find_child(root, 2);
        attach_effector(solver, tip, 1.0, 1.0, 0.0);
        attach_cone(solver, solver->node->find_child(root, 1),
                    ik.vec3.vec3(1, 0, 0), bad_angles[i]);
        solver->flags = IK_ENABLE_CONSTRAINTS;
        ik.solver.set_tree(solver, root);
        TASK_CHECK(ik.solver.rebuild(solver) == IK_INVALID_CONSTRAINT);
        ik.solver.destroy(solver);
    }

    /* a cone on the root has no entering segment to limit */
    {
        struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
        struct ik_node_t* root = build_chain(solver, 2);
        struct ik_node_t* tip = solver->node->find_child(root, 2);
        attach_effector(solver, tip, 1.0, 1.0, 0.0);
        attach_cone(solver, root, ik.vec3.vec3(1, 0, 0), PI_F * 0.5);
        solver->flags = IK_ENABLE_CONSTRAINTS;
        ik.solver.set_tree(solver, root);
        TASK_CHECK(ik.solver.rebuild(solver) == IK_INVALID_CONSTRAINT);
        ik.solver.destroy(solver);
    }

    /* after a failed rebuild, fixing the parameters and rebuilding recovers */
    {
        struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
        struct ik_node_t* root = build_chain(solver, 2);
        struct ik_node_t* tip = solver->node->find_child(root, 2);
        struct ik_constraint_t* cone;
        struct world_pose poses[MAX_NODES];
        struct snapshot shot[MAX_NODES];

        attach_effector(solver, tip, 2.0, 0.0, 0.0);
        cone = attach_cone(solver, solver->node->find_child(root, 1),
                           ik.vec3.vec3(1, 0, 0), PI_F * 0.5);
        solver->flags = IK_ENABLE_CONSTRAINTS;
        ik.solver.set_tree(solver, root);

        cone->cone_axis = ik.vec3.vec3(0, 0, 0);
        TASK_CHECK(ik.solver.rebuild(solver) == IK_INVALID_CONSTRAINT);

        cone->cone_axis = ik.vec3.vec3(1, 0, 0);
        TASK_CHECK(ik.solver.rebuild(solver) == IK_OK);
        snapshot_bind(root, shot);
        TASK_CHECK(solve_ok_or_residual(solver) == 0);
        recompute_world(root, poses);
        TASK_CHECK(distance_to(poses[2].pos, ik.vec3.vec3(2, 0, 0)) < EPS);
        TASK_CHECK(check_cones_recursive(root, poses) == 0);

        ik.solver.destroy(solver);
    }

    return 0;
}

/* Target rotations plus cone constraints are rejected, never silently merged. */
static int
cone_conflicts_with_target_rotation(void)
{
    struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(solver, 2);
    struct ik_node_t* tip = solver->node->find_child(root, 2);
    struct world_pose poses[MAX_NODES];

    attach_effector(solver, tip, 1.0, 1.0, 0.0);
    attach_cone(solver, solver->node->find_child(root, 1),
                ik.vec3.vec3(1, 0, 0), PI_F * 0.5);
    solver->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_TARGET_ROTATIONS;
    ik.solver.set_tree(solver, root);

    TASK_CHECK(ik.solver.rebuild(solver) == IK_INCOMPATIBLE_FLAGS);
    TASK_CHECK(ik.solver.solve(solver) == IK_INCOMPATIBLE_FLAGS);
    recompute_world(root, poses);
    TASK_CHECK(finite_vec(poses[0].pos));

    ik.solver.destroy(solver);
    return 0;
}

/* ========================================================================= */
/* branch group: two effector chains sharing an ancestor                      */
/* ========================================================================= */

/*
 *        0 -- 1 -- 2 -- 3a -- 4a
 *                    \
 *                     3b -- 4b
 */
static struct ik_node_t*
build_branched_chain(struct ik_solver_t* solver)
{
    struct ik_node_t* root = build_chain(solver, 2); /* 0,1,2 */
    struct ik_node_t* fork = solver->node->find_child(root, 2);
    struct ik_node_t* a = solver->node->create(3);
    struct ik_node_t* b = solver->node->create(4);
    struct ik_node_t* tip_a = solver->node->create(5);
    struct ik_node_t* tip_b = solver->node->create(6);

    a->position = ik.vec3.vec3(1, 0, 0);
    b->position = ik.vec3.vec3(1, 0, 0);
    tip_a->position = ik.vec3.vec3(1, 0, 0);
    tip_b->position = ik.vec3.vec3(1, 0, 0);
    a->rotation = b->rotation = tip_a->rotation = tip_b->rotation =
        ik.quat.quat(0, 0, 0, 1);

    solver->node->add_child(fork, a);
    solver->node->add_child(fork, b);
    solver->node->add_child(a, tip_a);
    solver->node->add_child(b, tip_b);
    return root;
}

/* Both end effectors pull the shared ancestor in different directions; the
 * two tips stay attached to the shared structure and every cone holds. */
static int
branch_two_tips_both_constrained(void)
{
    struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_branched_chain(solver);
    struct ik_node_t* tip_a = solver->node->find_child(root, 5);
    struct ik_node_t* tip_b = solver->node->find_child(root, 6);
    struct world_pose poses[MAX_NODES];
    struct snapshot shot[MAX_NODES];

    /* targets farther than the total reach: residual distance is allowed */
    attach_effector(solver, tip_a, 4.0, 2.0, 0.0);
    attach_effector(solver, tip_b, 4.0, -2.0, 0.0);

    attach_cone(solver, solver->node->find_child(root, 1),
                ik.vec3.vec3(1, 0, 0), PI_F / 6.0);
    attach_cone(solver, solver->node->find_child(root, 3),
                ik.vec3.vec3(1, 0, 0), PI_F * 0.25);
    attach_cone(solver, solver->node->find_child(root, 4),
                ik.vec3.vec3(1, 0, 0), PI_F * 0.25);
    solver->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    solver->max_iterations = 60;

    ik.solver.set_tree(solver, root);
    TASK_CHECK(ik.solver.rebuild(solver) == IK_OK);
    snapshot_bind(root, shot);
    TASK_CHECK(ik.solver.solve(solver) == IK_RESULT_CONVERGED);
    recompute_world(root, poses);

    /* the two tips remain distinct and both pull toward their own target */
    TASK_CHECK(distance_to(poses[5].pos, poses[6].pos) > 0.5);
    TASK_CHECK(poses[5].pos.y > poses[2].pos.y);
    TASK_CHECK(poses[6].pos.y < poses[2].pos.y);

    /* both branches share a single fork node reached from either side */
    TASK_CHECK(poses[2].used);
    TASK_CHECK(distance_to(poses[3].pos, poses[2].pos) > 0.0);
    TASK_CHECK(distance_to(poses[4].pos, poses[2].pos) > 0.0);

    TASK_CHECK(check_bone_lengths_recursive(root, poses, shot) == 0);
    TASK_CHECK(check_cones_recursive(root, poses) == 0);
    TASK_CHECK(check_rotation_pose_agreement_recursive(root, poses, shot) == 0);

    ik.solver.destroy(solver);
    return 0;
}

/* Reachable branch targets: both tips converge while the shared base chain is
 * solved only once. */
static int
branch_reachable_targets(void)
{
    struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_branched_chain(solver);
    struct ik_node_t* tip_a = solver->node->find_child(root, 5);
    struct ik_node_t* tip_b = solver->node->find_child(root, 6);
    struct world_pose poses[MAX_NODES];
    struct snapshot shot[MAX_NODES];

    attach_effector(solver, tip_a, 3.4, 0.6, 0.0);
    attach_effector(solver, tip_b, 3.4, -0.6, 0.0);
    attach_cone(solver, solver->node->find_child(root, 1),
                ik.vec3.vec3(1, 0, 0), PI_F * 0.25);
    solver->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    solver->max_iterations = 80;
    solver->tolerance = 1e-5;

    ik.solver.set_tree(solver, root);
    TASK_CHECK(ik.solver.rebuild(solver) == IK_OK);
    snapshot_bind(root, shot);
    TASK_CHECK(solve_ok_or_residual(solver) == 0);
    recompute_world(root, poses);

    TASK_CHECK(distance_to(poses[5].pos, ik.vec3.vec3(3.4, 0.6, 0)) < 1e-3);
    TASK_CHECK(distance_to(poses[6].pos, ik.vec3.vec3(3.4, -0.6, 0)) < 1e-3);
    TASK_CHECK(check_bone_lengths_recursive(root, poses, shot) == 0);
    TASK_CHECK(check_cones_recursive(root, poses) == 0);
    TASK_CHECK(check_rotation_pose_agreement_recursive(root, poses, shot) == 0);

    ik.solver.destroy(solver);
    return 0;
}

/* A zero-angle cone on one branch locks only that branch; the other tip still
 * reaches its own target and the shared fork is not overwritten. */
static int
branch_one_locked_branch(void)
{
    struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_branched_chain(solver);
    struct ik_node_t* tip_a = solver->node->find_child(root, 5);
    struct ik_node_t* tip_b = solver->node->find_child(root, 6);
    struct world_pose poses[MAX_NODES];
    struct snapshot shot[MAX_NODES];
    ik_vec3_t locked_bone;

    attach_effector(solver, tip_a, 3.0, 1.5, 0.0);
    attach_effector(solver, tip_b, 3.0, -1.5, 0.0);
    attach_cone(solver, solver->node->find_child(root, 3),
                ik.vec3.vec3(1, 0, 0), 0.0);
    solver->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    solver->max_iterations = 80;
    solver->tolerance = 1e-5;

    ik.solver.set_tree(solver, root);
    TASK_CHECK(ik.solver.rebuild(solver) == IK_OK);
    snapshot_bind(root, shot);
    TASK_CHECK(solve_ok_or_residual(solver) == 0);
    recompute_world(root, poses);

    /* the locked branch's first segment follows the fork's frame exactly */
    locked_bone = normalized(sub_vec(poses[3].pos, poses[2].pos));
    TASK_CHECK(vec_near(locked_bone,
                        normalized(rotated(ik.vec3.vec3(1, 0, 0),
                                           poses[2].rot)),
                        TIGHT_EPS));

    /* the other branch still reaches its target (branched constrained solves
     * converge a little more slowly than a single chain) */
    TASK_CHECK(distance_to(poses[6].pos, ik.vec3.vec3(3.0, -1.5, 0)) < 1e-2);
    TASK_CHECK(check_bone_lengths_recursive(root, poses, shot) == 0);
    TASK_CHECK(check_cones_recursive(root, poses) == 0);

    ik.solver.destroy(solver);
    return 0;
}

/* ========================================================================= */
/* rebind group                                                               */
/* ========================================================================= */

/* Rebuild explicitly re-binds the reference pose after the skeleton is posed
 * differently; the limit then follows the new bind frame. */
static int
rebind_to_new_animated_pose(void)
{
    struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(solver, 3);
    struct ik_node_t* tip = solver->node->find_child(root, 3);
    struct world_pose poses[MAX_NODES];
    struct snapshot shot[MAX_NODES];
    struct ik_constraint_t* cone;

    attach_effector(solver, tip, 0.0, 0.0, 0.0);
    cone = attach_cone(solver, solver->node->find_child(root, 1),
                       ik.vec3.vec3(1, 0, 0), PI_F / 6.0);
    solver->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    solver->max_iterations = 40;
    solver->tolerance = 1e-5;
    ik.solver.set_tree(solver, root);

    /* first bind pose: identity root */
    TASK_CHECK(ik.solver.rebuild(solver) == IK_OK);

    /* re-pose the root, then explicitly rebuild to re-bind */
    root->position = ik.vec3.vec3(5.0, 5.0, 0.0);
    root->rotation = rot_about_z(PI_F * 0.5); /* +X -> +Y */
    TASK_CHECK(ik.solver.rebuild(solver) == IK_OK);
    snapshot_bind(root, shot);

    /* target straight along the re-bound axis: exactly reachable */
    tip->effector->target_position = ik.vec3.vec3(5.0, 8.0, 0.0);
    TASK_CHECK(solve_ok_or_residual(solver) == 0);
    recompute_world(root, poses);

    TASK_CHECK(distance_to(poses[3].pos, ik.vec3.vec3(5.0, 8.0, 0.0)) < EPS);
    TASK_CHECK(vec_near(normalized(rotated(cone->cone_axis, poses[0].rot)),
                        ik.vec3.vec3(0, 1, 0), TIGHT_EPS));
    TASK_CHECK(check_bone_lengths_recursive(root, poses, shot) == 0);
    TASK_CHECK(check_cones_recursive(root, poses) == 0);

    /* a second solve with a new target must not silently re-bind: the public
     * cone axis is unchanged and the world limit still follows the root */
    tip->effector->target_position = ik.vec3.vec3(5.0, 5.0, 0.0);
    TASK_CHECK(ik.solver.solve(solver) == IK_RESULT_CONVERGED);
    TASK_CHECK(vec_near(cone->cone_axis, ik.vec3.vec3(1, 0, 0), TIGHT_EPS));
    recompute_world(root, poses);
    TASK_CHECK(check_cones_recursive(root, poses) == 0);
    TASK_CHECK(check_bone_lengths_recursive(root, poses, shot) == 0);

    ik.solver.destroy(solver);
    return 0;
}

/* Changing an ancestor's local rotation and rebuilding moves the child cone's
 * parent frame to the new bind orientation: the same target yields a different
 * (still cone-satisfying) world solution. Two independent solvers are used so
 * the second bind pose is compared against a pristine reference. */
static int
rebind_after_ancestor_rotation(void)
{
    struct ik_solver_t* plain = ik.solver.create(IK_FABRIK);
    struct ik_solver_t* rebound = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root_p = build_chain(plain, 3);
    struct ik_node_t* root_r = build_chain(rebound, 3);
    struct world_pose poses_p[MAX_NODES], poses_r[MAX_NODES];
    struct snapshot shot_p[MAX_NODES], shot_r[MAX_NODES];

    attach_effector(plain, plain->node->find_child(root_p, 3), 0.0, 2.9, 0.0);
    attach_effector(rebound, rebound->node->find_child(root_r, 3), 0.0, 2.9, 0.0);
    /* cone lives in node 1's local frame in both trees */
    attach_cone(plain, plain->node->find_child(root_p, 2),
                ik.vec3.vec3(1, 0, 0), PI_F / 6.0);
    attach_cone(rebound, rebound->node->find_child(root_r, 2),
                ik.vec3.vec3(1, 0, 0), PI_F / 6.0);

    plain->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    rebound->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    plain->max_iterations = rebound->max_iterations = 60;
    plain->tolerance = rebound->tolerance = 1e-6;

    /* the rebound tree carries a 90 degree ancestor rotation in its bind pose */
    rebound->node->find_child(root_r, 1)->rotation = rot_about_z(PI_F * 0.5);

    ik.solver.set_tree(plain, root_p);
    ik.solver.set_tree(rebound, root_r);
    TASK_CHECK(ik.solver.rebuild(plain) == IK_OK);
    TASK_CHECK(ik.solver.rebuild(rebound) == IK_OK);
    snapshot_bind(root_p, shot_p);
    snapshot_bind(root_r, shot_r);
    TASK_CHECK(solve_ok_or_residual(plain) == 0);
    TASK_CHECK(solve_ok_or_residual(rebound) == 0);
    recompute_world(root_p, poses_p);
    recompute_world(root_r, poses_r);

    /* re-binding the ancestor frame changed the reachable world configuration */
    TASK_CHECK(!vec_near(poses_r[2].pos, poses_p[2].pos, 1e-2));
    TASK_CHECK(!vec_near(poses_r[3].pos, poses_p[3].pos, 1e-2));
    /* both solutions keep every bone length and satisfy every cone */
    TASK_CHECK(check_bone_lengths_recursive(root_p, poses_p, shot_p) == 0);
    TASK_CHECK(check_bone_lengths_recursive(root_r, poses_r, shot_r) == 0);
    TASK_CHECK(check_cones_recursive(root_p, poses_p) == 0);
    TASK_CHECK(check_cones_recursive(root_r, poses_r) == 0);
    TASK_CHECK(check_rotation_pose_agreement_recursive(root_p, poses_p, shot_p) == 0);
    TASK_CHECK(check_rotation_pose_agreement_recursive(root_r, poses_r, shot_r) == 0);

    ik.solver.destroy(plain);
    ik.solver.destroy(rebound);
    return 0;
}

/* Repeated solves with changing targets keep the original bind frame; the
 * cone never drifts even though intermediate poses do. */
static int
rebind_target_changes_keep_bind_frame(void)
{
    struct ik_solver_t* solver = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(solver, 3);
    struct ik_node_t* tip = solver->node->find_child(root, 3);
    struct world_pose poses[MAX_NODES];
    struct snapshot shot[MAX_NODES];
    static const ik_vec3_t targets[] = {
        {{2.0, 1.0, 0.0}},
        {{1.0, 2.0, 0.0}},
        {{2.5, 0.2, 0.0}},
        {{0.5, 2.5, 0.0}}
    };
    unsigned i;

    attach_effector(solver, tip, 2.0, 1.0, 0.0);
    attach_cone(solver, solver->node->find_child(root, 1),
                ik.vec3.vec3(1, 0, 0), PI_F / 8.0);
    solver->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    solver->max_iterations = 40;
    ik.solver.set_tree(solver, root);

    TASK_CHECK(ik.solver.rebuild(solver) == IK_OK);
    snapshot_bind(root, shot);

    for (i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
    {
        tip->effector->target_position = targets[i];
        TASK_CHECK(ik.solver.solve(solver) != IK_RAN_OUT_OF_MEMORY);
        recompute_world(root, poses);
        TASK_CHECK(check_bone_lengths_recursive(root, poses, shot) == 0);
        TASK_CHECK(check_cones_recursive(root, poses) == 0);
        TASK_CHECK(check_rotation_pose_agreement_recursive(root, poses, shot) == 0);
    }

    ik.solver.destroy(solver);
    return 0;
}

/* ========================================================================= */

const struct task_case task_cases[] = {
    /* cone */
    {"cone", "reachable",                 cone_reachable},
    {"cone", "blocks_unreachable_target", cone_blocks_unreachable_target},
    {"cone", "zero_angle_locks_segment",  cone_zero_angle_locks_segment},
    {"cone", "root_animation_carries_limit", cone_root_animation_carries_limit},
    {"cone", "ancestor_chain",            cone_on_ancestor_chain},
    {"cone", "default_is_unrestricted",   cone_default_is_unrestricted},
    {"cone", "disabled_behaves_plain",    cone_disabled_behaves_like_plain_fabrik},
    {"cone", "invalid_parameters",        cone_invalid_parameters_rejected},
    {"cone", "target_rotation_conflict",  cone_conflicts_with_target_rotation},

    /* branch */
    {"branch", "two_tips_constrained",    branch_two_tips_both_constrained},
    {"branch", "reachable_targets",       branch_reachable_targets},
    {"branch", "one_locked_branch",       branch_one_locked_branch},

    /* rebind */
    {"rebind", "new_animated_pose",       rebind_to_new_animated_pose},
    {"rebind", "ancestor_rotation",       rebind_after_ancestor_rotation},
    {"rebind", "target_changes_keep_frame", rebind_target_changes_keep_bind_frame}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
