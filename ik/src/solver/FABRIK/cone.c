#include "ik/cone.h"
#include "ik/ik.h"
#include "ik/chain.h"
#include "ik/memory.h"
#include "ik/vec3_static.h"
#include "ik/quat_static.h"
#include <math.h>
#include <string.h>

#define IK_CONE_PI  3.14159265358979323846
#define IK_CONE_EPS 1e-12

/* ------------------------------------------------------------------------- */
static int
is_finite_scalar(ikreal_t v)
{
    return isfinite(v);
}

static int
is_finite_vec3(const ikreal_t v[3])
{
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

static ikreal_t
clamp_scalar(ikreal_t v, ikreal_t lo, ikreal_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ------------------------------------------------------------------------- */
/*
 * Validate every cone in the user tree. A zero/non-finite axis, a non-finite
 * angle, an angle outside [0, pi], or a cone attached to the root (which has
 * no entering segment) is rejected outright.
 */
static ikret_t
validate_cones_recursive(struct ik_node_t* node)
{
    if (node->constraint != NULL && node->constraint->type == IK_CONE)
    {
        struct ik_constraint_t* c = node->constraint;
        ikreal_t axis_length;

        if (!is_finite_vec3(c->cone_axis.f) ||
            !is_finite_scalar(c->cone_angle) ||
            c->cone_angle < 0.0 || c->cone_angle > IK_CONE_PI)
        {
            IKAPI.log.message("Invalid cone constraint: axis must be finite and angle must be a finite value in [0, pi]");
            return IK_INVALID_CONSTRAINT;
        }

        axis_length = ik_vec3_static_length(c->cone_axis.f);
        if (axis_length == 0.0)
        {
            IKAPI.log.message("Invalid cone constraint: cone_axis must not be the zero vector");
            return IK_INVALID_CONSTRAINT;
        }

        if (node->parent == NULL)
        {
            IKAPI.log.message("Invalid cone constraint: a cone cannot constrain the root node, which has no entering bone segment");
            return IK_INVALID_CONSTRAINT;
        }
    }

    NODE_FOR_EACH(node, guid, child)
        ikret_t result = validate_cones_recursive(child);
        if (result != IK_OK)
            return result;
    NODE_END_EACH

    return IK_OK;
}

/* ------------------------------------------------------------------------- */
struct ik_cone_node_t*
ik_cone_node_find(struct vector_t* cone_nodes, const struct ik_node_t* node)
{
    VECTOR_FOR_EACH(cone_nodes, struct ik_cone_node_t, record)
        if (record->node == node)
            return record;
    VECTOR_END_EACH
    return NULL;
}

struct ik_cone_t*
ik_cone_find(struct vector_t* cones, const struct ik_node_t* node)
{
    VECTOR_FOR_EACH(cones, struct ik_cone_t, cone)
        if (cone->node == node)
            return cone;
    VECTOR_END_EACH
    return NULL;
}

/* ------------------------------------------------------------------------- */
static ikret_t
collect_chain(struct chain_t* chain,
              struct vector_t* cone_nodes,
              struct vector_t* cones)
{
    int idx = chain_length(chain);

    /* chains store the tip node first, so walk from the base to the tip */
    while (idx-- > 0)
    {
        struct ik_node_t* node = chain_get_node(chain, idx);

        if (ik_cone_node_find(cone_nodes, node) == NULL)
        {
            struct ik_cone_node_t record;
            memset(&record, 0, sizeof record);
            record.node = node;
            ik_quat_static_set_identity(record.frame);
            ik_quat_static_set(record.bind_rotation, node->rotation.f);
            if (node->parent != NULL)
            {
                /*
                 * The local position vector points along the entering bone
                 * segment in the parent node's local frame, which is exactly
                 * the bind-time in-bone direction needed later.
                 */
                ik_vec3_static_set(record.bind_dir, node->position.f);
                ik_vec3_static_normalize(record.bind_dir);
            }
            if (vector_push(cone_nodes, &record) != IK_OK)
                return IK_RAN_OUT_OF_MEMORY;
        }

        if (node->constraint != NULL && node->constraint->type == IK_CONE &&
            ik_cone_find(cones, node) == NULL)
        {
            struct ik_cone_t cone;
            cone.node = node;
            ik_vec3_static_set(cone.axis_local, node->constraint->cone_axis.f);
            ik_vec3_static_normalize(cone.axis_local);
            cone.angle = node->constraint->cone_angle;
            cone.active = cone.angle < IK_CONE_PI;
            if (vector_push(cones, &cone) != IK_OK)
                return IK_RAN_OUT_OF_MEMORY;
        }
    }

    CHAIN_FOR_EACH_CHILD(chain, child)
        ikret_t result = collect_chain(child, cone_nodes, cones);
        if (result != IK_OK)
            return result;
    CHAIN_END_EACH

    return IK_OK;
}

/* ------------------------------------------------------------------------- */
ikret_t
ik_cone_rebuild_data(struct ik_solver_t* solver,
                     struct vector_t* cone_nodes,
                     struct vector_t* cones)
{
    ikret_t result;

    vector_clear(cone_nodes);
    vector_clear(cones);

    if (solver->tree == NULL)
        return IK_SOLVER_HAS_NO_TREE;

    result = validate_cones_recursive(solver->tree);
    if (result != IK_OK)
        goto failed;

    VECTOR_FOR_EACH(&solver->chain_list, struct chain_t, chain)
        result = collect_chain(chain, cone_nodes, cones);
        if (result != IK_OK)
            goto failed;
    VECTOR_END_EACH

    return IK_OK;

failed:
    vector_clear(cone_nodes);
    vector_clear(cones);
    return result;
}

/* ------------------------------------------------------------------------- */
void
ik_cone_clear(struct vector_t* cone_nodes, struct vector_t* cones)
{
    vector_clear(cone_nodes);
    vector_clear(cones);
}

/* ------------------------------------------------------------------------- */
void
ik_cone_set_root_frame(struct ik_cone_node_t* root_record,
                       const struct ik_node_t* root)
{
    ik_quat_static_set(root_record->frame, root->rotation.f);
    ik_quat_static_normalize(root_record->frame);
}

/* ------------------------------------------------------------------------- */
/*
 * Shortest-arc quaternion rotating @from onto @to. For the antiparallel case
 * the rotation axis is taken from the bind frame: the local axis least
 * parallel to @from, ties broken in X, Y, Z order.
 */
static void
shortest_arc_quat(ikreal_t q[4],
                  const ikreal_t from[3],
                  const ikreal_t to[3],
                  const ikreal_t bind_frame[4])
{
    static const ikreal_t local_axes[3][3] = {
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
        {0.0, 0.0, 1.0}
    };
    ikreal_t d = clamp_scalar(ik_vec3_static_dot(from, to), -1.0, 1.0);
    ikreal_t axis[3];

    if (d >= 1.0 - IK_CONE_EPS)
    {
        ik_quat_static_set_identity(q);
        return;
    }

    if (d <= -1.0 + IK_CONE_EPS)
    {
        int i, best = 0;
        ikreal_t best_score = 2.0;
        for (i = 0; i < 3; ++i)
        {
            ikreal_t world_axis[3];
            ikreal_t score;
            ik_vec3_static_set(world_axis, local_axes[i]);
            ik_vec3_static_rotate(world_axis, bind_frame);
            score = fabs(ik_vec3_static_dot(world_axis, from));
            if (score < best_score) /* strict compare keeps X,Y,Z tie order */
            {
                best_score = score;
                best = i;
            }
        }
        ik_vec3_static_set(axis, local_axes[best]);
        ik_vec3_static_rotate(axis, bind_frame);
        ik_vec3_static_normalize(axis);
        q[0] = axis[0];
        q[1] = axis[1];
        q[2] = axis[2];
        q[3] = 0.0;
        return;
    }

    ik_vec3_static_set(axis, from);
    ik_vec3_static_cross(axis, to);
    ik_vec3_static_normalize(axis);
    {
        ikreal_t angle = acos(d);
        ikreal_t half_sin = sin(angle * 0.5);
        q[0] = axis[0] * half_sin;
        q[1] = axis[1] * half_sin;
        q[2] = axis[2] * half_sin;
        q[3] = cos(angle * 0.5);
    }
}

/* ------------------------------------------------------------------------- */
/*
 * Pick a world-space direction perpendicular to @axis, taken from the axes of
 * the supplied frame. Ties are broken in X, Y, Z order.
 */
static void
fallback_perpendicular(ikreal_t out[3],
                       const ikreal_t axis[3],
                       const ikreal_t frame[4])
{
    static const ikreal_t local_axes[3][3] = {
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
        {0.0, 0.0, 1.0}
    };
    int i, best = 0;
    ikreal_t best_score = 2.0;
    ikreal_t world_axis[3];
    ikreal_t axis_component[3];
    for (i = 0; i < 3; ++i)
    {
        ikreal_t candidate[3];
        ikreal_t score;
        ik_vec3_static_set(candidate, local_axes[i]);
        ik_vec3_static_rotate(candidate, frame);
        score = fabs(ik_vec3_static_dot(candidate, axis));
        if (score < best_score) /* strict compare keeps X,Y,Z tie order */
        {
            best_score = score;
            best = i;
        }
    }
    ik_vec3_static_set(world_axis, local_axes[best]);
    ik_vec3_static_rotate(world_axis, frame);

    /* project the frame axis onto the plane perpendicular to axis */
    ik_vec3_static_set(axis_component, axis);
    ik_vec3_static_mul_scalar(axis_component,
                              ik_vec3_static_dot(world_axis, axis));
    ik_vec3_static_set(out, world_axis);
    ik_vec3_static_sub_vec3(out, axis_component);
    ik_vec3_static_normalize(out);
}

/* ------------------------------------------------------------------------- */
void
ik_cone_apply_segment(struct vector_t* cones,
                      const struct ik_cone_node_t* parent_record,
                      struct ik_cone_node_t* child_record,
                      const struct ik_node_t* parent,
                      struct ik_node_t* child)
{
    ikreal_t segment[3];
    ikreal_t d_solve[3];
    ikreal_t d_bind[3];
    ikreal_t q_bind_child[4];
    ikreal_t swing[4];
    ikreal_t bone_length;
    struct ik_cone_t* cone;

    /* solved in-bone direction of this segment */
    ik_vec3_static_set(segment, child->position.f);
    ik_vec3_static_sub_vec3(segment, parent->position.f);
    bone_length = ik_vec3_static_length(segment);
    ik_vec3_static_set(d_solve, segment);
    if (bone_length > 0.0)
        ik_vec3_static_normalize(d_solve);

    /* bind-time in-bone direction carried into the world by the parent frame */
    ik_vec3_static_set(d_bind, child_record->bind_dir);
    ik_vec3_static_rotate(d_bind, parent_record->frame);
    ik_vec3_static_normalize(d_bind);

    /*
     * Accumulate the bound local rotation on top of the solved parent frame.
     * bind_rotation (captured at rebuild time) is used rather than
     * node->rotation, which a previous solve may have overwritten with derived
     * joint rotations.
     */
    ik_quat_static_set(q_bind_child, parent_record->frame);
    ik_quat_static_mul_quat(q_bind_child, child_record->bind_rotation);

    /* enforce the cone on the segment direction before committing the frame */
    cone = ik_cone_find(cones, child);
    if (cone != NULL && cone->active && bone_length > 0.0)
    {
        ikreal_t axis_world[3];
        ikreal_t cos_angle;
        ikreal_t d_dot_axis;

        /* the cone axis travels with the solved parent frame */
        ik_vec3_static_set(axis_world, cone->axis_local);
        ik_vec3_static_rotate(axis_world, parent_record->frame);
        ik_vec3_static_normalize(axis_world);

        cos_angle = cos(cone->angle);
        d_dot_axis = clamp_scalar(ik_vec3_static_dot(d_solve, axis_world),
                                  -1.0, 1.0);

        if (d_dot_axis < cos_angle)
        {
            ikreal_t d_clamped[3];

            if (cone->angle <= IK_CONE_EPS)
            {
                /* zero half-angle: the segment must point along the axis */
                ik_vec3_static_set(d_clamped, axis_world);
            }
            else
            {
                ikreal_t projection[3];
                ikreal_t proj_length;

                /* component of the solved direction in the plane of the cone */
                ik_vec3_static_set(projection, axis_world);
                ik_vec3_static_mul_scalar(projection, d_dot_axis); /* dot*axis */
                ik_vec3_static_mul_scalar(projection, -1.0);       /* -dot*axis */
                ik_vec3_static_add_vec3(projection, d_solve);      /* d_solve - dot*axis */
                proj_length = ik_vec3_static_length(projection);

                if (proj_length <= IK_CONE_EPS)
                {
                    ikreal_t side[3];
                    fallback_perpendicular(side, axis_world,
                                           parent_record->frame);
                    ik_vec3_static_set(d_clamped, axis_world);
                    ik_vec3_static_mul_scalar(d_clamped, cos_angle);
                    ik_vec3_static_mul_scalar(side, sin(cone->angle));
                    ik_vec3_static_add_vec3(d_clamped, side);
                }
                else
                {
                    ik_vec3_static_set(d_clamped, axis_world);
                    ik_vec3_static_mul_scalar(d_clamped, cos_angle);
                    ik_vec3_static_mul_scalar(projection,
                        sin(cone->angle) / proj_length);
                    ik_vec3_static_add_vec3(d_clamped, projection);
                }
                ik_vec3_static_normalize(d_clamped);
            }

            /* reposition at the unchanged bone length on the cone boundary */
            ik_vec3_static_set(child->position.f, parent->position.f);
            ik_vec3_static_mul_scalar(d_clamped, child->dist_to_parent);
            ik_vec3_static_add_vec3(child->position.f, d_clamped);
            ik_vec3_static_set(d_solve, d_clamped);

            /* the output pose must always be finite */
            if (!is_finite_vec3(child->position.f))
            {
                ik_vec3_static_set(child->position.f, parent->position.f);
                ik_vec3_static_mul_scalar(axis_world, child->dist_to_parent);
                ik_vec3_static_add_vec3(child->position.f, axis_world);
                ik_vec3_static_set(d_solve, axis_world);
            }
        }
    }

    /*
     * Shortest swing from the bind in-bone direction to the final solved
     * direction. The child frame keeps the bound twist; only the swing is
     * applied, and it is evaluated against the bound child frame. A
     * zero-length segment has no defined direction, so it keeps the pure
     * accumulated bind frame and stays finite.
     */
    if (bone_length > 0.0)
        shortest_arc_quat(swing, d_bind, d_solve, q_bind_child);
    else
        ik_quat_static_set_identity(swing);
    ik_quat_static_set(child_record->frame, swing);
    ik_quat_static_mul_quat(child_record->frame, q_bind_child);
}
