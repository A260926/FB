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
#include "cgltf.h"    /* cgltf_skin */

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

/* ---- preserve-nodes ordering ----
 * The `--preserve-nodes` alternative: instead of folding each joint's non-joint ancestors into one
 * local TRS, keep the real node chain. The output "joint" set is every skin joint plus the ancestor
 * closure of those joints up to the node-tree roots. A node appears at most once; unrelated siblings
 * are excluded and no virtual root is added. Real disconnected trees stay a forest.
 *
 * Output order: roots by ascending original data.nodes index; depth-first from each root, visiting
 * that node's included children in ascending original data.nodes index, parent before child.
 *
 * Two distinct index spaces: `slot_to_out` maps an ORIGINAL skin.joints slot (the index a vertex
 * JOINTS_0 entry uses) to a marrow output index; `node_index` maps a marrow output index back to the
 * original data.nodes index. Auxiliary (non-joint, non-zero-...) ancestors occupy output indices that
 * are NOT valid skin slots - a mesh loader must remap through `slot_to_out`, never use an output index
 * as a vertex joint index. */
typedef struct {
    uint32_t  joint_count;   /* output skeleton count (closure size, <= 0xFFFE)            */
    uint32_t  node_count;    /* == the node_count passed to the build                      */
    uint32_t *node_index;    /* [joint_count] output index -> original data.nodes index    */
    int32_t  *parent;        /* [joint_count] output index -> output parent (-1 = root)    */
    int32_t  *out_of_node;   /* [node_count]  original node index -> output index (-1)     */
    int32_t  *slot_to_out;   /* [skin->joints_count] original skin slot -> output index    */
    uint32_t  root_count;    /* number of real roots (disconnected trees)                  */
    uint32_t *roots;         /* [root_count] output indices of roots, node-index ascending */
} mrw_g2m_forest_order;

/* Build the preserve-nodes ordering for `skin` over `nodes[0..node_count)`. On MRW_OK fills *out
 * (free with mrw_g2m_forest_order_free). Errors: MRW_E_RANGE (no nodes, a joint outside the node
 * array, or the closure exceeding the 0xFFFE joint cap), MRW_E_FORMAT (malformed hierarchy),
 * MRW_E_OVERFLOW (allocation failed). */
mrw_result mrw_g2m_forest_order_build(const cgltf_skin *skin,
                                      const cgltf_node *nodes, cgltf_size node_count,
                                      mrw_g2m_forest_order *out,
                                      char *diag, size_t diag_cap);

void mrw_g2m_forest_order_free(mrw_g2m_forest_order *fo);

#endif /* MRW_G2M_JOINT_ORDER_H */
