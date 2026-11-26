#include "ik/cone.h"
#include "ik/bstv.h"
#include "ik/chain.h"
#include "ik/constraint.h"
#include "ik/ik.h"
#include "ik/memory.h"
#include "ik/node.h"
#include "ik/node_FABRIK.h"
#include "ik/quat_static.h"
#include "ik/vec3_static.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------------- */
static struct bstv_t*
record_table(const struct ik_solver_t* solver)
{
    return (struct bstv_t*)solver->cone_data;
}

/* ------------------------------------------------------------------------- */
static int
vec3_is_finite(const ikreal_t v[3])
{
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

/* ------------------------------------------------------------------------- */
static ikret_t
validate_recursive(const struct ik_node_t* node)
{
    const struct ik_constraint_t* constraint = node->constraint;
    if (constraint != NULL && constraint->type == IK_CONE)
    {
        ikreal_t axis_length;

        if (!vec3_is_finite(constraint->cone_axis.f) ||
            !isfinite(constraint->cone_angle) ||
            constraint->cone_angle < 0.0 ||
            constraint->cone_angle > M_PI)
        {
            IKAPI.log.message(
                "Cone constraint is invalid: cone_axis must be a finite unit "
                "vector and cone_angle must be a finite angle in [0, PI].");
            return IK_INVALID_CONSTRAINT;
        }

        axis_length = ik_vec3_static_length(constraint->cone_axis.f);
        if (axis_length == 0.0)
        {
            IKAPI.log.message(
                "Cone constraint is invalid: cone_axis must not be the zero vector.");
            return IK_INVALID_CONSTRAINT;
        }
    }

    NODE_FOR_EACH(node, guid, child)
        ikret_t result = validate_recursive(child);
        if (result != IK_OK)
            return result;
    NODE_END_EACH

    return IK_OK;
}

/* ------------------------------------------------------------------------- */
static ikret_t
bind_recursive(struct ik_node_t* node, struct bstv_t* table)
{
    struct ik_constraint_t* constraint = node->constraint;
    if (constraint != NULL && constraint->type == IK_CONE)
    {
        struct ik_cone_record_t* record = MALLOC(sizeof *record);
        if (record == NULL)
        {
            IKAPI.log.message("Failed to allocate cone constraint record: out of memory");
            return IK_RAN_OUT_OF_MEMORY;
        }

        record->bind_angle = constraint->cone_angle;
        record->cos_angle = cos(constraint->cone_angle);
        ik_vec3_static_set(record->bind_local_axis.f, constraint->cone_axis.f);
        ik_vec3_static_normalize(record->bind_local_axis.f);

        if (bstv_insert(table, node->guid, record) != IK_OK)
        {
            FREE(record);
            return IK_RAN_OUT_OF_MEMORY;
        }
    }

    NODE_FOR_EACH(node, guid, child)
        ikret_t result = bind_recursive(child, table);
        if (result != IK_OK)
            return result;
    NODE_END_EACH

    return IK_OK;
}

/* ------------------------------------------------------------------------- */
ikret_t
ik_cone_bind_tree(struct ik_solver_t* solver)
{
    struct bstv_t* table;
    ikret_t result;

    if (solver->tree == NULL)
        return IK_SOLVER_HAS_NO_TREE;

    /* Validate everything before touching any previously bound state. */
    if ((result = validate_recursive(solver->tree)) != IK_OK)
    {
        /*
         * The old bindings can no longer be trusted: drop them as well so a
         * failed rebuild leaves behind nothing that could still be solved.
         */
        ik_cone_clear(solver);
        return result;
    }

    ik_cone_clear(solver);

    table = MALLOC(sizeof *table);
    if (table == NULL)
        return IK_RAN_OUT_OF_MEMORY;
    bstv_construct(table);

    if ((result = bind_recursive(solver->tree, table)) != IK_OK)
    {
        /* The table never reached the solver, so its records are freed here. */
        BSTV_FOR_EACH(table, struct ik_cone_record_t, guid, record)
            FREE(record);
        BSTV_END_EACH
        bstv_clear_free(table);
        FREE(table);
        solver->cone_data = NULL;
        return result;
    }

    solver->cone_data = table;
    return IK_OK;
}

/* ------------------------------------------------------------------------- */
void
ik_cone_clear(struct ik_solver_t* solver)
{
    struct bstv_t* table = record_table(solver);
    if (table == NULL)
        return;

    /* Records are independent allocations, so the tree need not be walked. */
    BSTV_FOR_EACH(table, struct ik_cone_record_t, guid, record)
        FREE(record);
    BSTV_END_EACH

    bstv_clear_free(table);
    FREE(table);
    solver->cone_data = NULL;
}

/* ------------------------------------------------------------------------- */
struct ik_cone_record_t*
ik_cone_record_of(struct ik_solver_t* solver, const struct ik_node_t* node)
{
    struct bstv_t* table = record_table(solver);
    if (table == NULL)
        return NULL;
    return (struct ik_cone_record_t*)bstv_find(table, node->guid);
}

/* ------------------------------------------------------------------------- */
int
ik_cone_active(const struct ik_solver_t* solver)
{
    struct bstv_t* table = record_table(solver);
    return table != NULL && bstv_count(table) > 0;
}

/* ------------------------------------------------------------------------- */
ikret_t
ik_cone_check_flags(const struct ik_solver_t* solver)
{
    if ((solver->flags & IK_ENABLE_CONSTRAINTS) &&
        (solver->flags & IK_ENABLE_TARGET_ROTATIONS))
    {
        IKAPI.log.message(
            "Cone constraints cannot be combined with target rotations yet. "
            "Disable IK_ENABLE_TARGET_ROTATIONS or IK_ENABLE_CONSTRAINTS.");
        return IK_CONFLICTING_SOLVER_OPTIONS;
    }
    return IK_OK;
}

/* ------------------------------------------------------------------------- */
static void
bind_locals_recursive(struct ik_node_t* node, const ik_quat_t* parent_world)
{
    struct ik_node_FABRIK_t* fabrik = (struct ik_node_FABRIK_t*)node;
    ik_quat_t world;

    fabrik->bind_local_rotation = node->rotation;
    fabrik->bind_local_position = node->position;

    /* Bind world orientation: parent bind frame * local rotation. */
    world = *parent_world;
    ik_quat_static_mul_quat(world.f, node->rotation.f);
    fabrik->bind_rotation = world;

    NODE_FOR_EACH(node, guid, child)
        bind_locals_recursive(child, &world);
    NODE_END_EACH
}

void
ik_cone_bind_locals(struct ik_solver_t* solver)
{
    if (solver->tree == NULL)
        return;

    /*
     * The root has no parent in the solver's tree, so its local rotation is
     * its world rotation. The tree is in local space when rebuild runs.
     */
    ik_quat_t root_world = solver->tree->rotation;
    struct ik_node_FABRIK_t* root = (struct ik_node_FABRIK_t*)solver->tree;
    root->bind_local_rotation = solver->tree->rotation;
    root->bind_local_position = solver->tree->position;
    root->bind_rotation = root_world;

    NODE_FOR_EACH(solver->tree, guid, child)
        bind_locals_recursive(child, &root_world);
    NODE_END_EACH
}

/* ------------------------------------------------------------------------- */
static void
update_carried_frames_recursive(struct ik_node_t* node, const ik_quat_t* parent_frame)
{
    struct ik_node_FABRIK_t* fabrik = (struct ik_node_FABRIK_t*)node;
    ik_quat_t frame;

    /*
     * The carried bind frame follows the current animated root and the bind
     * local rotations captured at rebuild. This is a re-evaluation, not a
     * re-bind: bind local data is never overwritten here.
     */
    frame = *parent_frame;
    ik_quat_static_mul_quat(frame.f, fabrik->bind_local_rotation.f);
    fabrik->bind_rotation = frame;

    /* Solved frames start out aligned with the carried bind frames. */
    fabrik->frame_rotation = frame;

    NODE_FOR_EACH(node, guid, child)
        update_carried_frames_recursive(child, &frame);
    NODE_END_EACH
}

void
ik_cone_update_carried_frames(struct ik_solver_t* solver)
{
    struct ik_node_FABRIK_t* root;

    if (solver->tree == NULL)
        return;

    root = (struct ik_node_FABRIK_t*)solver->tree;

    /* Root takes its current animated rotation. */
    root->bind_rotation = root->rotation;
    root->frame_rotation = root->rotation;

    NODE_FOR_EACH(solver->tree, guid, child)
        update_carried_frames_recursive(child, &root->rotation);
    NODE_END_EACH
}

/* ------------------------------------------------------------------------- */
static ikreal_t
clamp_scalar(ikreal_t value, ikreal_t low, ikreal_t high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

/* ------------------------------------------------------------------------- */
static void
quat_from_axis_angle(ik_quat_t* q, const ik_vec3_t* axis, ikreal_t angle)
{
    ik_vec3_t v = *axis;
    ik_vec3_static_mul_scalar(v.f, sin(angle * 0.5));
    q->v = v;
    q->w = cos(angle * 0.5);
}

/* ------------------------------------------------------------------------- */
static void
perpendicular_unit_vector(ik_vec3_t* out, const ik_vec3_t* axis)
{
    static const ik_vec3_t basis[3] = {
        {{1.0, 0.0, 0.0}},
        {{0.0, 1.0, 0.0}},
        {{0.0, 0.0, 1.0}}
    };
    int i;
    ikreal_t best = 2.0;
    ik_vec3_t choice = basis[0];

    /* Least aligned world basis vector is guaranteed usable as a perpendicular. */
    for (i = 0; i < 3; ++i)
    {
        ikreal_t parallel = fabs(ik_vec3_static_dot(axis->f, basis[i].f));
        if (parallel < best)
        {
            best = parallel;
            choice = basis[i];
        }
    }

    ik_vec3_static_cross(choice.f, axis->f);
    ik_vec3_static_normalize(choice.f);
    *out = choice;
}

/* ------------------------------------------------------------------------- */
void
ik_cone_apply_segment(struct ik_solver_t* solver,
                      struct ik_node_t* parent_node,
                      struct ik_node_t* child_node,
                      const ik_quat_t* frame_parent,
                      ik_quat_t* frame_child,
                      ik_quat_t* q_local_child)
{
    struct ik_node_FABRIK_t* parent = (struct ik_node_FABRIK_t*)parent_node;
    struct ik_node_FABRIK_t* child  = (struct ik_node_FABRIK_t*)child_node;
    struct ik_cone_record_t* record = ik_cone_record_of(solver, child_node);
    ik_vec3_t segment;
    ik_vec3_t direction;
    ikreal_t segment_length;
    ik_vec3_t d0_local;
    ik_vec3_t d0_world;
    ik_quat_t swing;

    /* Current solved incoming bone direction (world space). */
    ik_vec3_static_set(segment.f, child_node->position.f);
    ik_vec3_static_sub_vec3(segment.f, parent_node->position.f);
    segment_length = ik_vec3_static_length(segment.f);
    ik_vec3_static_set(direction.f, segment.f);
    ik_vec3_static_normalize(direction.f);

    /* Reference incoming direction, lifted from parent-local into world space. */
    d0_local = child->bind_local_position;
    ik_vec3_static_normalize(d0_local.f);
    d0_world = d0_local;
    ik_vec3_static_rotate(d0_world.f, frame_parent->f);

    /* Clamp the solved direction to the cone before any frame is derived. */
    if (record != NULL)
    {
        ik_vec3_t cone_axis_world;
        ikreal_t cos_theta;

        ik_vec3_static_set(cone_axis_world.f, record->bind_local_axis.f);
        ik_vec3_static_rotate(cone_axis_world.f, frame_parent->f);

        cos_theta = clamp_scalar(
            ik_vec3_static_dot(cone_axis_world.f, direction.f), -1.0, 1.0);

        if (cos_theta < record->cos_angle)
        {
            ikreal_t theta = acos(cos_theta);
            ikreal_t excess = theta - record->bind_angle;
            ik_vec3_t rotate_axis;
            ik_quat_t correction;

            ik_vec3_static_set(rotate_axis.f, cone_axis_world.f);
            ik_vec3_static_cross(rotate_axis.f, direction.f);
            if (ik_vec3_static_length(rotate_axis.f) < 1e-12)
                perpendicular_unit_vector(&rotate_axis, &cone_axis_world);
            else
                ik_vec3_static_normalize(rotate_axis.f);

            /* Rotate the direction back onto the cone surface. */
            quat_from_axis_angle(&correction, &rotate_axis, -excess);
            ik_vec3_static_rotate(direction.f, correction.f);
            ik_vec3_static_normalize(direction.f);

            /* Keep the original bone length and stay attached to the parent. */
            child_node->position = direction;
            ik_vec3_static_mul_scalar(child_node->position.f, segment_length);
            {
                ik_vec3_t offset = child_node->position;
                child_node->position = parent_node->position;
                ik_vec3_static_add_vec3(child_node->position.f, offset.f);
            }

            ik_vec3_static_set(direction.f, child_node->position.f);
            ik_vec3_static_sub_vec3(direction.f, parent_node->position.f);
            ik_vec3_static_normalize(direction.f);
        }
    }

    /*
     * Shortest swing that moves the reference incoming direction onto the
     * solved one, with a deterministic axis for the antiparallel case.
     */
    {
        ikreal_t dot = clamp_scalar(
            ik_vec3_static_dot(d0_world.f, direction.f), -1.0, 1.0);

        if (dot > 1.0 - 1e-12)
        {
            ik_quat_static_set_identity(swing.f);
        }
        else if (dot < -1.0 + 1e-12)
        {
            static const ik_vec3_t local_basis[3] = {
                {{1.0, 0.0, 0.0}},
                {{0.0, 1.0, 0.0}},
                {{0.0, 0.0, 1.0}}
            };
            int i, best_axis = 0;
            ikreal_t best_parallel = 2.0;
            ik_vec3_t axis;

            /*
             * Pick the bind-frame axis least parallel to the incoming bone.
             * Ties resolve in X, Y, Z order because the comparison is strict.
             */
            for (i = 0; i < 3; ++i)
            {
                ik_vec3_t world_axis = local_basis[i];
                ik_vec3_static_rotate(world_axis.f, parent->bind_rotation.f);
                {
                    ikreal_t parallel =
                        fabs(ik_vec3_static_dot(world_axis.f, d0_world.f));
                    if (parallel < best_parallel)
                    {
                        best_parallel = parallel;
                        best_axis = i;
                    }
                }
            }

            axis = local_basis[best_axis];
            ik_vec3_static_rotate(axis.f, parent->bind_rotation.f);
            quat_from_axis_angle(&swing, &axis, M_PI);
        }
        else
        {
            ik_vec3_t axis;
            ikreal_t angle = acos(dot);
            ik_vec3_static_set(axis.f, d0_world.f);
            ik_vec3_static_cross(axis.f, direction.f);
            ik_vec3_static_normalize(axis.f);
            quat_from_axis_angle(&swing, &axis, angle);
        }
    }

    /* Child frame: swing * parent frame * bind local rotation. */
    {
        ik_quat_t frame;
        ik_quat_static_set(frame.f, swing.f);
        ik_quat_static_mul_quat(frame.f, frame_parent->f);
        ik_quat_static_mul_quat(frame.f, child->bind_local_rotation.f);
        child->frame_rotation = frame;
        *frame_child = frame;
    }

    /* Solved local rotation: inverse parent frame * child frame. */
    {
        ik_quat_t local;
        ik_quat_static_set(local.f, frame_parent->f);
        ik_quat_static_conj(local.f);
        ik_quat_static_mul_quat(local.f, child->frame_rotation.f);
        child->solved_local_rotation = local;
        *q_local_child = local;
    }
}

/* ------------------------------------------------------------------------- */
static void
export_rotations_for_chain(struct chain_t* chain)
{
    int idx = chain_length(chain) - 1;

    /* Base nodes keep their animated local rotation; skip the last index. */
    while (idx-- > 0)
    {
        struct ik_node_FABRIK_t* node = (struct ik_node_FABRIK_t*)chain_get_node(chain, idx);
        node->rotation = node->solved_local_rotation;
    }

    CHAIN_FOR_EACH_CHILD(chain, child)
        export_rotations_for_chain(child);
    CHAIN_END_EACH
}

void
ik_cone_export_joint_rotations(struct ik_solver_t* solver)
{
    SOLVER_FOR_EACH_CHAIN(solver, chain)
        export_rotations_for_chain(chain);
    SOLVER_END_EACH
}
