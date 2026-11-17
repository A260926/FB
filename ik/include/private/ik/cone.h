/*!
 * @file cone.h
 * @brief Runtime data and helpers for cone (IK_CONE) joint constraints.
 *
 * The public constraint parameters (ik_constraint_t::cone_axis /
 * ik_constraint_t::cone_angle) describe the limit in the parent node's local
 * frame at bind time. Everything in this header is solver-side state that is
 * (re)built by ik_solver_rebuild() and consumed while FABRIK iterates.
 *
 * A cone is attached to the child node of the restricted segment. The cone's
 * parent frame is not a fixed world frame: it is re-derived every backward
 * pass from the solved ancestor positions and the bound local rotations, so
 * the limit moves together with the animated skeleton.
 */
#ifndef IK_CONE_H
#define IK_CONE_H

#include "ik/config.h"
#include "ik/vec3.h"
#include "ik/quat.h"
#include "ik/solver.h"

C_BEGIN

struct vector_t;

/*!
 * @brief Per-node scratch used while enforcing cone constraints. One record
 * exists for every node that participates in the generated chain trees.
 */
struct ik_cone_node_t
{
    struct ik_node_t* node;

    /*! Unit in-bone direction at bind time, in the parent's local frame. */
    ikreal_t bind_dir[3];

    /*!
     * Local rotation of the node at bind time. Captured by rebuild() so that
     * repeated solves keep the same reference frame even when the solver
     * overwrites node->rotation with the derived joint rotations.
     */
    ikreal_t bind_rotation[4];

    /*!
     * Current solved orientation frame of the node in world space. The frame
     * is rebuilt on every backward pass and is only valid while solving.
     */
    ikreal_t frame[4];
};

/*!
 * @brief One active cone limit. The constraint hangs on the child node of
 * the segment whose swing is limited.
 */
struct ik_cone_t
{
    struct ik_node_t* node;

    /*! Unit cone centre axis, in the parent's local frame at bind time. */
    ikreal_t axis_local[3];

    /*! Half-angle in radians, [0, pi]. */
    ikreal_t angle;

    /*! False for the unrestricted default angle of pi. */
    int active;
};

/*!
 * @brief FABRIK solver state. The solver derives from the base solver so it
 * can own the cone runtime tables without touching the public ABI.
 */
struct ik_solver_FABRIK_t
{
    IK_SOLVER_HEAD
    struct vector_t cone_nodes;   /* struct ik_cone_node_t */
    struct vector_t cone_list;    /* struct ik_cone_t */
};

/*!
 * @brief Validates every IK_CONE constraint in the solver tree and rebuilds
 * the cone runtime tables from the current bind pose.
 *
 * @return IK_OK on success. IK_INVALID_CONSTRAINT if a cone carries a zero or
 * non-finite axis, a non-finite angle, an angle outside [0, pi], or is
 * attached where no segment exists. On failure the tables are emptied and the
 * caller must also discard its chain trees, so no solvable half-state remains.
 */
IK_PRIVATE_API ikret_t
ik_cone_rebuild_data(struct ik_solver_t* solver,
                     struct vector_t* cone_nodes,
                     struct vector_t* cones);

/*!
 * @brief Releases the runtime tables (does not free the vectors themselves).
 */
IK_PRIVATE_API void
ik_cone_clear(struct vector_t* cone_nodes, struct vector_t* cones);

IK_PRIVATE_API struct ik_cone_node_t*
ik_cone_node_find(struct vector_t* cone_nodes, const struct ik_node_t* node);

IK_PRIVATE_API struct ik_cone_t*
ik_cone_find(struct vector_t* cones, const struct ik_node_t* node);

/*!
 * @brief Sets the world-space frame of the tree root. The root takes its
 * current animated rotation directly; there is no in-bone swing to solve.
 */
IK_PRIVATE_API void
ik_cone_set_root_frame(struct ik_cone_node_t* root_record,
                       const struct ik_node_t* root);

/*!
 * @brief Advances the orientation frame from the parent node to the child
 * node and enforces the child's cone limit (if any) on the segment direction.
 *
 * @param[in] cones Cone table, used to look up the child's limit.
 * @param[in] parent_record Frame record of the parent.
 * @param[in] child_record Frame record of the child; its frame is written.
 * @param[in] parent Parent node (solved world positions).
 * @param[in,out] child Child node. On entry its solved position is the
 * candidate produced by the forward/backward FABRIK pass; on exit it is
 * repositioned at the unchanged bone length and inside the cone.
 */
IK_PRIVATE_API void
ik_cone_apply_segment(struct vector_t* cones,
                      const struct ik_cone_node_t* parent_record,
                      struct ik_cone_node_t* child_record,
                      const struct ik_node_t* parent,
                      struct ik_node_t* child);

C_END

#endif /* IK_CONE_H */
