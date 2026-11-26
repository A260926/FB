/*!
 * @file cone.h
 * @brief Cone (swing) constraints for the FABRIK solver.
 *
 * A cone constraint is attached to the child node of the bone segment it
 * limits. It stores a unit rest direction (@c cone_axis) in the local space
 * of the segment's parent node, captured from the reference pose during
 * solver rebuild, plus a half-angle (@c cone_angle) in radians.
 *
 * The constraint frame is rebuilt every iteration from the solved positions:
 * it follows the animated root and all solved ancestors, so the limits move
 * with the skeleton instead of being pinned to a world axis.
 */
#ifndef IK_CONE_H
#define IK_CONE_H

#include "ik/config.h"
#include "ik/vec3.h"
#include "ik/quat.h"

C_BEGIN

struct ik_solver_t;
struct ik_node_t;
struct chain_t;
struct ik_constraint_t;

/* Per-node bookkeeping for a node carrying a cone constraint. */
struct ik_cone_record_t
{
    /* Cone axis in the parent's local frame, captured at rebuild. */
    ik_vec3_t bind_local_axis;
    /* Half-angle in radians, captured at rebuild. */
    ikreal_t  bind_angle;
    /* cos(bind_angle), cached for the swing test. */
    ikreal_t  cos_angle;
};

/*!
 * @brief Validates every cone constraint reachable from @p tree and binds
 * its reference data (local axis, half angle).
 *
 * A zero-length or non-finite axis, a non-finite angle, or an angle outside
 * [0, PI] makes this fail with IK_INVALID_CONSTRAINT. On failure no records
 * are installed, so the solver cannot be left in a half-initialised state.
 */
IK_PRIVATE_API ikret_t
ik_cone_bind_tree(struct ik_solver_t* solver);

/*!
 * @brief Removes all cone records installed by a previous rebuild.
 */
IK_PRIVATE_API void
ik_cone_clear(struct ik_solver_t* solver);

/*!
 * @brief Snapshots the local rotation and position of every node in the
 * tree. Runs as part of rebuild, while the tree is still in local space,
 * and defines the reference (bind) pose. Solving never overwrites these
 * snapshots; only another rebuild re-binds them.
 */
IK_PRIVATE_API void
ik_cone_bind_locals(struct ik_solver_t* solver);

/*!
 * @brief Returns the record of the cone constraint attached to @p node, or
 * NULL if the node carries no (active) cone constraint.
 */
IK_PRIVATE_API struct ik_cone_record_t*
ik_cone_record_of(struct ik_solver_t* solver, const struct ik_node_t* node);

/*!
 * @brief Returns non-zero if at least one bound cone constraint is active.
 */
IK_PRIVATE_API int
ik_cone_active(const struct ik_solver_t* solver);

/*!
 * @brief Rejects an unsupported flag combination. Target rotations and cone
 * constraints cannot be combined yet; returns
 * IK_CONFLICTING_SOLVER_OPTIONS in that case, IK_OK otherwise.
 */
IK_PRIVATE_API ikret_t
ik_cone_check_flags(const struct ik_solver_t* solver);

/*!
 * @brief Carries the bind frames down the tree for one solve.
 *
 * The root takes its current animated rotation; each descendant then
 * accumulates the bind local rotations captured at rebuild. This does not
 * re-bind anything -- bind local data is only written by rebuild. The
 * resulting world orientations are used by the antiparallel swing fallback
 * so that its axis choice always refers to the bind frame.
 */
IK_PRIVATE_API void
ik_cone_update_carried_frames(struct ik_solver_t* solver);

/*!
 * @brief Clamps one already-positioned bone segment to its cone.
 *
 * @p child is the node the constraint is attached to; its position was just
 * written by a FABRIK pass. @p parent is its parent node. @p frame_parent is
 * the orientation frame of the parent node (world rotation of its local
 * frame), accumulated top-down through the chain. The child frame and all
 * output data are written through the out-parameters.
 *
 * Bone lengths and joint connectivity are preserved: only the direction of
 * the segment from parent to child is rotated, never its length or the
 * position of @p parent.
 *
 * @param[out] frame_child Frame to use for the child node.
 * @param[out] q_local_child Solved local rotation of the child node.
 */
IK_PRIVATE_API void
ik_cone_apply_segment(struct ik_solver_t* solver,
                      struct ik_node_t* parent,
                      struct ik_node_t* child,
                      const ik_quat_t* frame_parent,
                      ik_quat_t* frame_child,
                      ik_quat_t* q_local_child);

/*!
 * @brief Writes solved local rotations for all chain nodes from the frames
 * accumulated during the constrained iterations, and exposes the accumulated
 * frames so the G2L pass can recover consistent local positions. Only used
 * when IK_ENABLE_JOINT_ROTATIONS is set.
 */
IK_PRIVATE_API void
ik_cone_export_joint_rotations(struct ik_solver_t* solver);

C_END

#endif /* IK_CONE_H */
