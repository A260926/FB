/* Canonical marrow joint ordering for a glTF skin - the single definition of "which marrow joint
 * index a skin-order joint maps to". gltf2marrow serializes the skeleton/clips in this order; any
 * consumer that must address that skeleton from glTF data (notably the demo's mesh loader, whose
 * JOINTS_0 indices are in skin order) remaps through `marrow_of`. Sharing one definition lets the
 * demo derive a provably identical remap by running the same DFS - no fragile node-name matching.
 *
 * Resolution rules (single-tree skeleton): each joint's parent is its nearest ancestor that is
 * itself a skin joint; exactly one joint must be parentless (the root); the marrow order is a DFS
 * from the root with children visited in ascending skin-order, parent always before child. */
#ifndef MRW_G2M_JOINT_ORDER_H
#define MRW_G2M_JOINT_ORDER_H

#include "marrow.h"   /* mrw_result */
#include "cgltf.h"    /* cgltf_skin, cgltf_data, cgltf_node */

typedef struct {
    uint32_t  joint_count;   /* == skin->joints_count                                     */
    int32_t  *parent_js;     /* [joint_count] skin-order joint -> parent skin-order (-1 root) */
    uint32_t *order;         /* [joint_count] marrow index -> skin-order joint index       */
    uint32_t *marrow_of;     /* [joint_count] skin-order joint -> marrow index             */
    int32_t   root_js;       /* skin-order index of the single root                        */
} mrw_g2m_joint_order;

/* Build the ordering for `skin`. On MRW_OK fills *out (free with mrw_g2m_joint_order_free). On any
 * error returns a code, writes a human-readable reason into diag (when diag != NULL && diag_cap > 0),
 * and leaves *out zeroed. Errors: MRW_E_RANGE (joint count not in [1, 0xFFFE]), MRW_E_FORMAT (not a
 * single-rooted tree - zero/multiple roots, or a cycle), MRW_E_OVERFLOW (allocation failed). */
mrw_result mrw_g2m_joint_order_build(const cgltf_skin *skin, mrw_g2m_joint_order *out,
                                     char *diag, size_t diag_cap);

void mrw_g2m_joint_order_free(mrw_g2m_joint_order *jo);

/* ---- preserve-nodes ordering: the real node forest, not the folded joint tree ----
 *
 * The fold ordering above collapses non-joint nodes between joints into one local TRS. A chain
 * that contains a non-joint ancestor with non-uniform scale and a rotated descendant is a shear
 * matrix, which has no TRS decomposition; preserve mode keeps the real nodes instead, so the
 * shear lives in the model-matrix products (legal) rather than in a local TRS.
 *
 * The retained set is every skin joint plus all its ancestors up to each node tree's root:
 * ancestors are included even when they are not joints (they carry the transform and any
 * animation), while an ancestor's unrelated siblings and their subtrees are excluded. No
 * virtual root is invented. Roots are emitted in ascending `data.nodes` index order; each node's
 * retained children are visited depth-first in ascending `data.nodes` index order, parent before
 * child. Disconnected real trees therefore stay a forest.
 *
 * Every output index denotes exactly one real node and its parent is a strictly smaller output
 * index (0xFFFF for a forest root), so the runtime's local→model loop can run in output order. The
 * original skin slot index and the output index are deliberately separate: a mesh's JOINTS_0
 * values are skin slots and must be remapped through `skin_to_output`; the extra ancestor bones
 * added by this mode must never be written into a vertex's JOINTS slot. Names may repeat (they
 * are data, not identity - look bones up by index/node pointer, never by name). */
typedef struct {
    uint32_t          joint_count;    /* number of retained nodes == output joints, [1, 0xFFFE] */
    const cgltf_node **nodes;         /* [joint_count] output index -> real node (borrowed)     */
    uint32_t         *node_index;     /* [joint_count] output index -> data.nodes original idx  */
    uint16_t         *parent;         /* [joint_count] output index -> parent output (< self)   */
                                      /*              0xFFFF = forest root                      */
    uint32_t         *skin_to_output; /* [skin->joints_count] skin slot -> output index         */
    uint32_t         *node_to_output; /* [data->nodes_count] node idx -> output, else UINT32_MAX */
} mrw_g2m_forest_order;

/* Build the preserve-mode ordering for `skin` / `data`. On MRW_OK fills *out (free with
 * mrw_g2m_forest_order_free). On error returns a code, writes a diagnostic, and leaves *out
 * zeroed. Errors: MRW_E_RANGE (skin joint count outside [1,0xFFFE], or the ancestor closure exceeds
 * 0xFFFE), MRW_E_FORMAT (a skin joint or ancestor is not a member of data.nodes, the same node is
 * listed in more than one skin slot, a node parent cycle, or an unreachable/duplicated node),
 * MRW_E_OVERFLOW (allocation failed). */
mrw_result mrw_g2m_forest_order_build(cgltf_data *data, const cgltf_skin *skin,
                                     mrw_g2m_forest_order *out,
                                     char *diag, size_t diag_cap);

void mrw_g2m_forest_order_free(mrw_g2m_forest_order *fo);

#endif /* MRW_G2M_JOINT_ORDER_H */
