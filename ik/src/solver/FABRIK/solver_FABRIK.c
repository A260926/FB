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
    return sizeof(struct ik_solver_t);
}

/* ------------------------------------------------------------------------- */
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

    /*
     * If there are no child chains, then the first node in the chain must
     * contain an effector. The target position is the effector's target
     * position.
     *
     * If we are an intermediate chain, then our target position is not the
     * effector's target position (there is no effector), instead, it is the
     * average position of all of child chains.
     */
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

    /*
     * Iterate through each segment and apply the FABRIK algorithm.
     */
    node_count = chain_length(chain);
    for (node_idx = 0; node_idx < node_count - 1; ++node_idx)
    {
        struct ik_node_t* child_node  = chain_get_node(chain, node_idx + 0);
        struct ik_node_t* parent_node = chain_get_node(chain, node_idx + 1);

        /* move node to target */
        child_node->position = target.position;

        /* lerp between direction vector and segment vector */
        ik_vec3_static_sub_vec3(target.position.f, parent_node->position.f);        /* segment vector */
        ik_vec3_static_normalize(target.position.f);                                /* normalizeso we have segment direction vector */
        ik_vec3_static_sub_vec3(target.position.f, target.direction.f);             /* for lerp, subtract target direction... */
        ik_vec3_static_mul_scalar(target.position.f, parent_node->rotation_weight); /* ...mul with weight... */
        ik_vec3_static_add_vec3(target.position.f, parent_node->position.f);        /* ...and attach this lerp'd direction to the parent node */

        /* point segment to previous node */
        ik_vec3_static_sub_vec3(target.position.f, child_node->position.f);         /* this computes the correct direction the segment should have */
        ik_vec3_static_normalize(target.position.f);
        ik_vec3_static_mul_scalar(target.position.f, child_node->dist_to_parent);
        ik_vec3_static_add_vec3(target.position.f, child_node->position.f);         /* attach to child -- this is the new target for the next segment */
    }

    return target;
}

/* ------------------------------------------------------------------------- */
/*
 * Cone-constrained FABRIK passes.
 *
 * The positional FABRIK steps are unchanged, but every segment, directly
 * after being repositioned, is clamped to the cone constraint attached to
 * its child node. Clamping rotates only the segment direction around the
 * parent node, so bone lengths and joint connectivity are preserved.
 *
 * Each pass also walks the orientation frames top-down: the parent frame
 * follows the animated root and every solved ancestor, and the cone axis
 * (bound in parent-local space) is rotated into the world through that
 * frame. There is therefore no fixed world axis that a parent could rotate
 * its way out of.
 */
static ik_vec3_t
solve_chain_forwards_cone(struct ik_solver_t* solver, struct chain_t* chain)
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
        ik_vec3_t child_base_position = solve_chain_forwards_cone(solver, child);
        ik_vec3_static_add_vec3(target_position.f, child_base_position.f);
        ++average_count;
    CHAIN_END_EACH

    if (average_count == 0)
    {
        struct ik_node_t* effector_node = chain_get_node(chain, 0);
        target_position = effector_node->effector->_actual_target;
    }
    else
    {
        ik_vec3_static_div_scalar(target_position.f, average_count);
    }

    node_count = chain_length(chain);
    for (node_idx = 0; node_idx < node_count - 1; ++node_idx)
    {
        struct ik_node_FABRIK_t* child_node  = (struct ik_node_FABRIK_t*)chain_get_node(chain, node_idx + 0);
        struct ik_node_FABRIK_t* parent_node = (struct ik_node_FABRIK_t*)chain_get_node(chain, node_idx + 1);
        ik_quat_t frame_child, local_rotation;

        /* move node to target */
        child_node->position = target_position;

        /* point segment to previous node and set target position to its end */
        ik_vec3_static_sub_vec3(target_position.f, parent_node->position.f);
        ik_vec3_static_normalize(target_position.f);
        ik_vec3_static_mul_scalar(target_position.f, -child_node->dist_to_parent);
        ik_vec3_static_add_vec3(target_position.f, child_node->position.f);

        /* enforce the cone on the parent->child segment */
        ik_cone_apply_segment(solver,
                              (struct ik_node_t*)parent_node,
                              (struct ik_node_t*)child_node,
                              &parent_node->frame_rotation,
                              &frame_child, &local_rotation);

        /*
         * Keep walking backwards with the point one bone length away from
         * the child; do not restart from the child's (possibly clamped)
         * position or the iteration cannot converge.
         */
    }

    return target_position;
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

    /*
     * If there are no child chains, then the first node in the chain must
     * contain an effector. The target position is the effector's target
     * position. Otherwise, average the data we've been accumulating from the
     * child chains.
     */
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

    /*
     * Iterate through each segment and apply the FABRIK algorithm.
     */
    node_count = chain_length(chain);
    for (node_idx = 0; node_idx < node_count - 1; ++node_idx)
    {
        struct ik_node_t* child_node  = chain_get_node(chain, node_idx + 0);
        struct ik_node_t* parent_node = chain_get_node(chain, node_idx + 1);

        /* move node to target */
        child_node->position = target_position;

        /* point segment to previous node and set target position to its end */
        ik_vec3_static_sub_vec3(target_position.f, parent_node->position.f);        /* parent points to child */
        ik_vec3_static_normalize(target_position.f);                                /* normalise */
        ik_vec3_static_mul_scalar(target_position.f, -child_node->dist_to_parent);  /* child points to parent */
        ik_vec3_static_add_vec3(target_position.f, child_node->position.f);         /* attach to child -- this is the new target for next iteration */
    }

    return target_position;
}

/* ------------------------------------------------------------------------- */
static void
solve_chain_backwards_cone(struct ik_solver_t* solver,
                           struct chain_t* chain, ik_vec3_t target_position)
{
    int node_idx = chain_length(chain) - 1;

    /*
     * The base node must be set to the target position before iterating.
     * XXX: Why is this conditional?
     */
    if (node_idx > 1)
    {
        struct ik_node_t* base_node = chain_get_node(chain, node_idx);
        base_node->position = target_position;
    }

    /*
     * Iterate from the fixed base back to the tip and enforce the cone on
     * every segment. The base node's frame was either seeded from the
     * animated root or computed by the parent chain that shares this node.
     */
    while (node_idx-- > 0)
    {
        struct ik_node_FABRIK_t* child_node  = (struct ik_node_FABRIK_t*)chain_get_node(chain, node_idx + 0);
        struct ik_node_FABRIK_t* parent_node = (struct ik_node_FABRIK_t*)chain_get_node(chain, node_idx + 1);
        ik_quat_t frame_child, local_rotation;

        /* point segment to child node and set target position to its beginning */
        ik_vec3_static_sub_vec3(target_position.f, child_node->position.f);
        ik_vec3_static_normalize(target_position.f);
        ik_vec3_static_mul_scalar(target_position.f, -child_node->dist_to_parent);
        ik_vec3_static_add_vec3(target_position.f, parent_node->position.f);

        /* move node to target */
        child_node->position = target_position;

        /* enforce the cone on the parent->child segment */
        ik_cone_apply_segment(solver,
                              (struct ik_node_t*)parent_node,
                              (struct ik_node_t*)child_node,
                              &parent_node->frame_rotation,
                              &frame_child, &local_rotation);
    }

    CHAIN_FOR_EACH_CHILD(chain, child)
        solve_chain_backwards_cone(solver, child, target_position);
    CHAIN_END_EACH
}

/* ------------------------------------------------------------------------- */
static void
solve_chain_backwards(struct chain_t* chain, ik_vec3_t target_position)
{
    int node_idx = chain_length(chain) - 1;

    /*
     * The base node must be set to the target position before iterating.
     * XXX: Why is this conditional?
     */
    if (node_idx > 1)
    {
        struct ik_node_t* base_node = chain_get_node(chain, node_idx);
        base_node->position = target_position;
    }

    /*
     * Iterate through each segment the other way around and apply the FABRIK
     * algorithm.
     */
    while (node_idx-- > 0)
    {
        struct ik_node_t* child_node  = chain_get_node(chain, node_idx + 0);
        struct ik_node_t* parent_node = chain_get_node(chain, node_idx + 1);

        /* point segment to child node and set target position to its beginning */
        ik_vec3_static_sub_vec3(target_position.f, child_node->position.f); /* child points to parent */
        ik_vec3_static_normalize(target_position.f);                                  /* normalise */
        ik_vec3_static_mul_scalar(target_position.f, -child_node->dist_to_parent);    /* parent points to child */
        ik_vec3_static_add_vec3(target_position.f, parent_node->position.f);/* attach to parent -- this is the new target */

        /* move node to target */
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
    /* typical default values */
    solver->max_iterations = 20;
    solver->tolerance = 1e-3;

    return IK_OK;
}

/* ------------------------------------------------------------------------- */
void
ik_solver_FABRIK_destruct(struct ik_solver_t* solver)
{
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

    /* Recurse into children chains */
    CHAIN_FOR_EACH_CHILD(chain, child)
        ik_quat_t rotation;
        calculate_joint_rotations_for_chain(child);

        rotation = chain_get_base_node(chain)->rotation;

        /*
         * Averaging quaternions taken from here
         * http://wiki.unity3d.com/index.php/Averaging_Quaternions_and_Vectors
         */
        ik_quat_static_normalize_sign(rotation.f);
        ik_quat_static_add_quat(average_rotation.f, rotation.f);
        ++average_count;
    CHAIN_END_EACH

    /*
     * Assuming there was more than 1 child chain and assuming we aren't the
     * base node, then the child chains we just iterated must share the same
     * sub-base node (which is our tip node). Average the accumulated
     * quaternion and set this node's correct solved rotation.
     */
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
    /*
     * Calculate all of the delta rotations of the joints and store them into
     * node->rotation.
     */
    int node_idx = chain_length(chain) - 1;
    while (node_idx-- > 0)
    {
        struct ik_node_FABRIK_t* child_node  = (struct ik_node_FABRIK_t*)chain_get_node(chain, node_idx + 0);
        struct ik_node_FABRIK_t* parent_node = (struct ik_node_FABRIK_t*)chain_get_node(chain, node_idx + 1);

        /* calculate vectors for original and solved segments */
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

    /*
     * Calculates the "global" (world) angles of each joint and writes them to
     * each node->rotation field.
     *
     * The angle between the original and solved segments are calculated using
     * standard vector math (dot product). The axis of rotation is calculated
     * with the cross product. From this data, a quaternion is constructed,
     * describing this delta rotation. Finally, in order to make the rotations
     * global instead of relative, the delta rotation is multiplied with
     * node->original_rotation, which should be a quaternion describing the
     * node's global rotation in the unsolved tree.
     *
     * The rotation of the base joint in the chain is returned so it can be
     * averaged by parent chains.
     */
    recurse_into_children(chain);
    calculate_delta_rotation_of_each_segment(chain);

    /*
     * It's not possible to calculate rotations for the end effector nodes,
     * since there are no child nodes to compare. Because we're in global space
     * leaving the rotations untouched will cause the end effector to keep a
     * global orientation regardless of what the rest of the tree is doing.
     * this may or may not be desirable, so the user can enable the effector
     * node to inherit its parent rotation.
     */
    effector_node = chain_get_tip_node(chain);
    if (effector_node->effector && effector_node->effector->flags & IK_INHERIT_ROTATION)
    {
        effector_node->rotation = chain_get_node(chain, 1)->rotation; /* parent node */
    }

    /*
     * Finally, apply initial global rotations to calculated delta rotations to
     * obtain the solved global rotations.
     */
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
ikret_t
ik_solver_FABRIK_solve(struct ik_solver_t* solver)
{
    ikret_t result = IK_OK;
    int iteration = solver->max_iterations;
    ikreal_t tolerance_squared = solver->tolerance * solver->tolerance;
    int use_cones;

    /* A failed rebuild leaves no half-initialised state worth solving. */
    if (!solver->rebuild_ok)
        return solver->rebuild_result != IK_OK ? solver->rebuild_result : -1;

    use_cones = (solver->flags & IK_ENABLE_CONSTRAINTS) && ik_cone_active(solver);

    /*
     * Target rotations cannot be combined with active cone constraints yet.
     * Refuse explicitly instead of silently dropping one of the two requests.
     * Without any bound cone the target-rotation path stays available.
     */
    if (use_cones && (solver->flags & IK_ENABLE_TARGET_ROTATIONS))
        return ik_cone_check_flags(solver);

    /* Tree is in local space -- FABRIK needs only global node positions */
    ik_transform_chain_list(&solver->chain_list, TR_L2G | TR_TRANSLATIONS);

    /*
     * Carry the bind frames down the tree from the current animated root.
     * Cone axes are expressed through these moving frames, so animated root
     * translation/rotation takes the whole limit setup along with it.
     */
    if (use_cones)
        ik_cone_update_carried_frames(solver);

    /*
     * Joint rotations are calculated by comparing positional differences
     * before and after solving the tree. This comparison needs to occur in
     * global space (doesn't work in local as far as I can see). Store the
     * positions and locations before solving for later.
     */
    if (!use_cones && (solver->flags & IK_ENABLE_JOINT_ROTATIONS))
        store_initial_transform(&solver->chain_list);

    while (iteration-- > 0)
    {
        /* Actual algorithm here */
        SOLVER_FOR_EACH_CHAIN(solver, chain)
            struct  ik_node_t* base_node;
            int idx;

            /*
             * The algorithm assumes chains have at least one bone. This should
             * be asserted while building the chain trees, but it can't hurt
             * to double check
             */
            idx = chain_length(chain) - 1;
            assert(idx > 0);

            base_node = chain_get_node(chain, idx);

            if (solver->flags & IK_ENABLE_TARGET_ROTATIONS)
                solve_chain_forwards_with_target_rotation(chain);
            else if (use_cones)
                solve_chain_forwards_cone(solver, chain);
            else
                solve_chain_forwards(chain);

            if (use_cones)
                solve_chain_backwards_cone(solver, chain, base_node->position);
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

    if (use_cones && (solver->flags & IK_ENABLE_JOINT_ROTATIONS))
    {
        /*
         * Use the frames accumulated by the constrained iterations, so the
         * output local rotations describe exactly the same final pose as the
         * solved positions rather than recomputing and overriding it.
         */
        ik_cone_export_joint_rotations(solver);
    }
    else if (solver->flags & IK_ENABLE_JOINT_ROTATIONS)
    {
        calculate_joint_rotations(&solver->chain_list);
    }

    /* Transform back to local space now that solving is complete */
    ik_transform_chain_list(&solver->chain_list, TR_G2L | TR_TRANSLATIONS);

    return result;
}
