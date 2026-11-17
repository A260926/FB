#include "ik/solver_FABRIK.h"
#include "ik/bstv.h"
#include "ik/chain.h"
#include "ik/cone.h"
#include "ik/ik.h"
#include "ik/memory.h"
#include "ik/node_FABRIK.h"
#include "ik/quat_static.h"
#include "ik/transform.h"
#include "ik/vec3_static.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

struct position_direction_t
{
    union {
        struct {
            ik_vec3_t position;
            ik_vec3_t direction;
        };
        ikreal_t f[6];
    };
};

/* ------------------------------------------------------------------------- */
uintptr_t
ik_solver_FABRIK_type_size(void)
{
    return sizeof(struct ik_solver_FABRIK_t);
}

/* ------------------------------------------------------------------------- */
/* Forward pass (tip -> base): plain FABRIK, unchanged from the base algorithm
 */
static struct position_direction_t
solve_chain_forwards_with_target_rotation(struct chain_t* chain)
{
    int node_count, node_idx;
    int average_count;
    struct position_direction_t target;

    /*
     * Target position (and direction) is the average of all solved child chain base positions.
     */
    ik_vec3_static_set_zero(target.position.f);
    ik_vec3_static_set_zero(target.direction.f);
    average_count = 0;
    CHAIN_FOR_EACH_CHILD(chain, child)
        struct position_direction_t child_posdir = solve_chain_forwards_with_target_rotation(child);
        ik_vec3_static_add_vec3(target.position.f, child_posdir.position.f);
        ik_vec3_static_add_vec3(target.direction.f, child_posdir.direction.f);
        ++average_count;
    CHAIN_END_EACH

    if (average_count == 0)
    {
        struct ik_node_t* effector_node = chain_get_node(chain, 0);
        struct ik_effector_t* effector = effector_node->effector;
        target.position = effector->_actual_target;

        /* TODO This "global direction" could be made configurable if needed */
        target.direction.x = 0.0;
        target.direction.y = 0.0;
        target.direction.z = 1.0;
        ik_vec3_static_rotate(target.direction.f, effector->target_rotation.f);
    }
    else
    {
        ik_vec3_static_div_scalar(target.position.f, average_count);
        ik_vec3_static_normalize(target.direction.f);
    }

    node_count = chain_length(chain);
    for (node_idx = 0; node_idx < node_count - 1; ++node_idx)
    {
        struct ik_node_t* child_node  = chain_get_node(chain, node_idx + 0);
        struct ik_node_t* parent_node = chain_get_node(chain, node_idx + 1);

        /* move node to target */
        child_node->position = target.position;

        /* lerp between direction vector and segment vector */
        ik_vec3_static_sub_vec3(target.position.f, parent_node->position.f);
        ik_vec3_static_normalize(target.position.f);
        ik_vec3_static_sub_vec3(target.position.f, target.direction.f);
        ik_vec3_static_mul_scalar(target.position.f, parent_node->rotation_weight);
        ik_vec3_static_add_vec3(target.position.f, parent_node->position.f);

        /* point segment to previous node */
        ik_vec3_static_sub_vec3(target.position.f, child_node->position.f);
        ik_vec3_static_normalize(target.position.f);
        ik_vec3_static_mul_scalar(target.position.f, child_node->dist_to_parent);
        ik_vec3_static_add_vec3(target.position.f, child_node->position.f);
    }

    return target;
}

/* ------------------------------------------------------------------------- */
static ik_vec3_t
solve_chain_forwards(struct chain_t* chain)
{
    int node_count, node_idx;
    int average_count;
    ik_vec3_t target_position;

    /*
     * Target position is the average of all solved child chain base positions.
     */
    ik_vec3_static_set_zero(target_position.f);
    average_count = 0;
    CHAIN_FOR_EACH_CHILD(chain, child)
        ik_vec3_t child_base_position = solve_chain_forwards(child);
        ik_vec3_static_add_vec3(target_position.f, child_base_position.f);
        ++average_count;
    CHAIN_END_EACH

    if (average_count == 0)
    {
        struct ik_node_t* effector_node = chain_get_node(chain, 0);
        struct ik_effector_t* effector = effector_node->effector;
        target_position = effector->_actual_target;
    }
    else
    {
        ik_vec3_static_div_scalar(target_position.f, average_count);
    }

    node_count = chain_length(chain);
    for (node_idx = 0; node_idx < node_count - 1; ++node_idx)
    {
        struct ik_node_t* child_node  = chain_get_node(chain, node_idx + 0);
        struct ik_node_t* parent_node = chain_get_node(chain, node_idx + 1);

        /* move node to target */
        child_node->position = target_position;

        /* point segment to previous node and set target position to its end */
        ik_vec3_static_sub_vec3(target_position.f, parent_node->position.f);
        ik_vec3_static_normalize(target_position.f);
        ik_vec3_static_mul_scalar(target_position.f, -child_node->dist_to_parent);
        ik_vec3_static_add_vec3(target_position.f, child_node->position.f);
    }

    return target_position;
}

/* ------------------------------------------------------------------------- */
/* Backward pass (base -> tip) with cone constraints.
 *
 * Every segment keeps its bind length and every child stays attached to its
 * parent joint. After the ordinary FABRIK backward repositioning, each segment
 * is projected back into its cone and the node's orientation frame is advanced
 * according to the bind-pose convention (see ik/cone.h).
 *
 * @param parent_record Frame record of the chain's base node. NULL for a
 * top-level chain, in which case the root takes its current animated rotation.
 * Child chains pass the shared tip record so a shared node is never recomputed
 * or overwritten by the last sibling chain.
 */
static void
solve_chain_backwards_cone(struct chain_t* chain,
                           struct ik_cone_node_t* parent_record,
                           ik_vec3_t base_position,
                           struct vector_t* cone_nodes,
                           struct vector_t* cones)
{
    int node_idx = chain_length(chain) - 1;
    struct ik_node_t* base_node = chain_get_node(chain, node_idx);
    struct ik_cone_node_t* base_record;

    /* The base node keeps the position decided by the parent chain */
    if (node_idx > 1)
        base_node->position = base_position;

    base_record = ik_cone_node_find(cone_nodes, base_node);
    assert(base_record != NULL);
    if (parent_record == NULL)
    {
        /* top-level chain: root frame is the animated root rotation */
        ik_cone_set_root_frame(base_record, base_node);
    }
    else
    {
        /* shared sub-base node: keep the frame computed by the parent chain */
        assert(parent_record == base_record);
    }

    while (node_idx-- > 0)
    {
        struct ik_node_t* child_node  = chain_get_node(chain, node_idx + 0);
        struct ik_node_t* parent_node = chain_get_node(chain, node_idx + 1);
        struct ik_cone_node_t* child_record;
        struct ik_cone_node_t* parent_rec;
        ik_vec3_t direction;

        /* ordinary FABRIK backward step: preserve the bind bone length */
        direction = child_node->position;
        ik_vec3_static_sub_vec3(direction.f, parent_node->position.f);
        ik_vec3_static_normalize(direction.f);
        ik_vec3_static_mul_scalar(direction.f, child_node->dist_to_parent);
        child_node->position = parent_node->position;
        ik_vec3_static_add_vec3(child_node->position.f, direction.f);

        parent_rec = ik_cone_node_find(cone_nodes, parent_node);
        child_record = ik_cone_node_find(cone_nodes, child_node);
        assert(parent_rec != NULL && child_record != NULL);

        /* enforce the cone (if any) and commit the node's solved frame */
        ik_cone_apply_segment(cones, parent_rec, child_record,
                              parent_node, child_node);
    }

    /*
     * Recurse into child chains. Their base node is this chain's tip node;
     * pass its record and position so the shared joint cannot be overwritten.
     */
    CHAIN_FOR_EACH_CHILD(chain, child)
        struct ik_node_t* tip = chain_get_tip_node(chain);
        struct ik_cone_node_t* tip_record = ik_cone_node_find(cone_nodes, tip);
        solve_chain_backwards_cone(child, tip_record, tip->position,
                                   cone_nodes, cones);
    CHAIN_END_EACH
}

/* ------------------------------------------------------------------------- */
static void
solve_chain_backwards(struct chain_t* chain, ik_vec3_t target_position)
{
    int node_idx = chain_length(chain) - 1;

    if (node_idx > 1)
    {
        struct ik_node_t* base_node = chain_get_node(chain, node_idx);
        base_node->position = target_position;
    }

    while (node_idx-- > 0)
    {
        struct ik_node_t* child_node  = chain_get_node(chain, node_idx + 0);
        struct ik_node_t* parent_node = chain_get_node(chain, node_idx + 1);

        ik_vec3_static_sub_vec3(target_position.f, child_node->position.f);
        ik_vec3_static_normalize(target_position.f);
        ik_vec3_static_mul_scalar(target_position.f, -child_node->dist_to_parent);
        ik_vec3_static_add_vec3(target_position.f, parent_node->position.f);

        child_node->position = target_position;
    }

    CHAIN_FOR_EACH_CHILD(chain, child)
        solve_chain_backwards(child, target_position);
    CHAIN_END_EACH
}

/* ------------------------------------------------------------------------- */
ikret_t
ik_solver_FABRIK_construct(struct ik_solver_t* solver)
{
    struct ik_solver_FABRIK_t* fabrik = (struct ik_solver_FABRIK_t*)solver;

    /* typical default values */
    solver->max_iterations = 20;
    solver->tolerance = 1e-3;

    vector_construct(&fabrik->cone_nodes, sizeof(struct ik_cone_node_t));
    vector_construct(&fabrik->cone_list, sizeof(struct ik_cone_t));

    return IK_OK;
}

/* ------------------------------------------------------------------------- */
void
ik_solver_FABRIK_destruct(struct ik_solver_t* solver)
{
    struct ik_solver_FABRIK_t* fabrik = (struct ik_solver_FABRIK_t*)solver;
    vector_clear_free(&fabrik->cone_nodes);
    vector_clear_free(&fabrik->cone_list);
}

/* ------------------------------------------------------------------------- */
ikret_t
ik_solver_FABRIK_rebuild(struct ik_solver_t* solver)
{
    struct ik_solver_FABRIK_t* fabrik = (struct ik_solver_FABRIK_t*)solver;
    ikret_t result;

    /*
     * Cone limits and target rotations cannot be combined yet: the cone pass
     * owns segment directions while target rotations would steer them too.
     * Refuse explicitly rather than silently dropping either feature.
     */
    if ((solver->flags & IK_ENABLE_CONSTRAINTS) &&
        (solver->flags & IK_ENABLE_TARGET_ROTATIONS))
    {
        IKAPI.log.message("IK_ENABLE_CONSTRAINTS cannot be combined with IK_ENABLE_TARGET_ROTATIONS");
        ik_cone_clear(&fabrik->cone_nodes, &fabrik->cone_list);
        return IK_INCOMPATIBLE_FLAGS;
    }

    /* (re)build chains, effector list and segment lengths from the bind pose */
    result = ik_solver_base_rebuild(solver);
    if (result != IK_OK)
    {
        ik_cone_clear(&fabrik->cone_nodes, &fabrik->cone_list);
        return result;
    }

    /* validate cone parameters and bind the reference pose */
    result = ik_cone_rebuild_data(solver, &fabrik->cone_nodes,
                                  &fabrik->cone_list);
    if (result != IK_OK)
    {
        /* no solvable half-state: discard the chains built above as well */
        SOLVER_FOR_EACH_CHAIN(solver, chain)
            chain_destruct(chain);
        SOLVER_END_EACH
        vector_clear_free(&solver->chain_list);
        vector_construct(&solver->chain_list, sizeof(struct chain_t));
        ik_cone_clear(&fabrik->cone_nodes, &fabrik->cone_list);
        return result;
    }

    return IK_OK;
}

/* ------------------------------------------------------------------------- */
static void
store_initial_transform_for_chain(struct chain_t* chain)
{
    CHAIN_FOR_EACH_NODE(chain, node_base)
        struct ik_node_FABRIK_t* node = (struct ik_node_FABRIK_t*)node_base;
        node->initial_position = node->position;
        node->initial_rotation = node->rotation;
    CHAIN_END_EACH

    CHAIN_FOR_EACH_CHILD(chain, child)
        store_initial_transform_for_chain(child);
    CHAIN_END_EACH
}
static void
store_initial_transform(const struct vector_t* chain_list)
{
    VECTOR_FOR_EACH(chain_list, struct chain_t, chain)
        store_initial_transform_for_chain(chain);
    VECTOR_END_EACH
}

/* ------------------------------------------------------------------------- */
static void
calculate_joint_rotations_for_chain(struct chain_t* chain);
static void
recurse_into_children(struct chain_t* chain)
{
    int average_count = 0;
    ik_quat_t average_rotation = ik_quat_static_quat(0, 0, 0, 0);

    CHAIN_FOR_EACH_CHILD(chain, child)
        ik_quat_t rotation;
        calculate_joint_rotations_for_chain(child);

        rotation = chain_get_base_node(chain)->rotation;

        ik_quat_static_normalize_sign(rotation.f);
        ik_quat_static_add_quat(average_rotation.f, rotation.f);
        ++average_count;
    CHAIN_END_EACH

    if (average_count > 0)
    {
        ik_quat_static_div_scalar(average_rotation.f, average_count);
        ik_quat_static_normalize(average_rotation.f);
        chain_get_tip_node(chain)->rotation = average_rotation;
    }
}

/* ------------------------------------------------------------------------- */
static void
calculate_delta_rotation_of_each_segment(struct chain_t* chain)
{
    int node_idx = chain_length(chain) - 1;
    while (node_idx-- > 0)
    {
        struct ik_node_FABRIK_t* child_node  = (struct ik_node_FABRIK_t*)chain_get_node(chain, node_idx + 0);
        struct ik_node_FABRIK_t* parent_node = (struct ik_node_FABRIK_t*)chain_get_node(chain, node_idx + 1);

        ik_vec3_t segment_original = child_node->initial_position;
        ik_vec3_t segment_solved   = child_node->position;
        ik_vec3_static_sub_vec3(segment_original.f, parent_node->initial_position.f);
        ik_vec3_static_sub_vec3(segment_solved.f, parent_node->position.f);
        ik_quat_static_angle(parent_node->rotation.f, segment_original.f, segment_solved.f);
    }
}

/* ------------------------------------------------------------------------- */
static void
calculate_joint_rotations_for_chain(struct chain_t* chain)
{
    struct ik_node_t* effector_node;

    assert(chain_length(chain) >= 2);

    recurse_into_children(chain);
    calculate_delta_rotation_of_each_segment(chain);

    effector_node = chain_get_tip_node(chain);
    if (effector_node->effector && effector_node->effector->flags & IK_INHERIT_ROTATION)
        effector_node->rotation = chain_get_node(chain, 1)->rotation;

    CHAIN_FOR_EACH_NODE(chain, node_base)
        struct ik_node_FABRIK_t* node = (struct ik_node_FABRIK_t*)node_base;
        ik_quat_static_mul_quat(node->rotation.f, node->initial_rotation.f);
    CHAIN_END_EACH
}
static void
calculate_joint_rotations(struct vector_t* chain_list)
{
    VECTOR_FOR_EACH(chain_list, struct chain_t, chain)
        calculate_joint_rotations_for_chain(chain);
    VECTOR_END_EACH
}

/* ------------------------------------------------------------------------- */
/*
 * Convert the cone-frame world orientations into local node rotations so that
 * the reported local rotation and local position describe the same final pose.
 * The frames were built during the backward passes and therefore already
 * satisfy every cone.
 */
static void
derive_local_rotations_for_chain(struct chain_t* chain,
                                 struct ik_cone_node_t* parent_record,
                                 struct vector_t* cone_nodes)
{
    int node_idx = chain_length(chain) - 1;
    struct ik_node_t* base_node = chain_get_node(chain, node_idx);
    struct ik_cone_node_t* base_record = ik_cone_node_find(cone_nodes, base_node);

    if (parent_record == NULL)
    {
        /* the root keeps its animated local rotation */
        struct ik_node_FABRIK_t* root_fabrik = (struct ik_node_FABRIK_t*)base_node;
        base_node->rotation = root_fabrik->initial_rotation;
    }
    else
    {
        assert(parent_record == base_record);
    }

    while (node_idx-- > 0)
    {
        struct ik_node_t* child_node  = chain_get_node(chain, node_idx + 0);
        struct ik_node_t* parent_node = chain_get_node(chain, node_idx + 1);
        struct ik_cone_node_t* child_record  = ik_cone_node_find(cone_nodes, child_node);
        struct ik_cone_node_t* parent_rec    = ik_cone_node_find(cone_nodes, parent_node);
        ik_quat_t local_rotation;

        /* Q_local = conj(Q_world_parent) * Q_world_child */
        ik_quat_static_set(local_rotation.f, parent_rec->frame);
        ik_quat_static_conj(local_rotation.f);
        ik_quat_static_mul_quat(local_rotation.f, child_record->frame);
        child_node->rotation = local_rotation;
    }

    /* child chains share this chain's tip node as their base node */
    CHAIN_FOR_EACH_CHILD(chain, child)
        struct ik_node_t* tip = chain_get_tip_node(chain);
        struct ik_cone_node_t* tip_record = ik_cone_node_find(cone_nodes, tip);
        derive_local_rotations_for_chain(child, tip_record, cone_nodes);
    CHAIN_END_EACH
}
static void
derive_local_rotations(struct vector_t* chain_list, struct vector_t* cone_nodes)
{
    VECTOR_FOR_EACH(chain_list, struct chain_t, chain)
        derive_local_rotations_for_chain(chain, NULL, cone_nodes);
    VECTOR_END_EACH
}

/* ------------------------------------------------------------------------- */
ikret_t
ik_solver_FABRIK_solve(struct ik_solver_t* solver)
{
    struct ik_solver_FABRIK_t* fabrik = (struct ik_solver_FABRIK_t*)solver;
    ikret_t result = IK_OK;
    int iteration = solver->max_iterations;
    ikreal_t tolerance_squared = solver->tolerance * solver->tolerance;
    int constraints_enabled = solver->flags & IK_ENABLE_CONSTRAINTS;

    if (constraints_enabled &&
        (solver->flags & IK_ENABLE_TARGET_ROTATIONS))
    {
        IKAPI.log.message("IK_ENABLE_CONSTRAINTS cannot be combined with IK_ENABLE_TARGET_ROTATIONS");
        return IK_INCOMPATIBLE_FLAGS;
    }

    /* Tree is in local space -- FABRIK needs only global node positions */
    ik_transform_chain_list(&solver->chain_list, TR_L2G | TR_TRANSLATIONS);

    /*
     * Joint rotations are calculated by comparing positional differences
     * before and after solving the tree. Store the bind transforms first.
     * Solving itself never re-binds: the cone tables built in rebuild() are
     * the only reference pose used during iteration.
     */
    if (solver->flags & IK_ENABLE_JOINT_ROTATIONS)
        store_initial_transform(&solver->chain_list);

    while (iteration-- > 0)
    {
        SOLVER_FOR_EACH_CHAIN(solver, chain)
            struct ik_node_t* base_node;
            int idx;

            idx = chain_length(chain) - 1;
            assert(idx > 0);

            base_node = chain_get_node(chain, idx);

            if (solver->flags & IK_ENABLE_TARGET_ROTATIONS)
                solve_chain_forwards_with_target_rotation(chain);
            else
                solve_chain_forwards(chain);

            if (constraints_enabled)
                solve_chain_backwards_cone(chain, NULL, base_node->position,
                                           &fabrik->cone_nodes,
                                           &fabrik->cone_list);
            else
                solve_chain_backwards(chain, base_node->position);
        SOLVER_END_EACH

        /* Check if all effectors are within range */
        SOLVER_FOR_EACH_EFFECTOR_NODE(solver, node)
            ik_vec3_t diff = node->position;
            ik_vec3_static_sub_vec3(diff.f, node->effector->target_position.f);
            if (ik_vec3_static_length_squared(diff.f) > tolerance_squared)
            {
                result = IK_RESULT_CONVERGED;
                break;
            }
        SOLVER_END_EACH
    }

    if (solver->flags & IK_ENABLE_JOINT_ROTATIONS)
    {
        if (constraints_enabled)
            derive_local_rotations(&solver->chain_list, &fabrik->cone_nodes);
        else
            calculate_joint_rotations(&solver->chain_list);
    }

    /* Transform back to local space now that solving is complete */
    ik_transform_chain_list(&solver->chain_list, TR_G2L | TR_TRANSLATIONS);

    return result;
}
