/* See joint_order.h. The converter and any external consumer (the demo mesh loader) produce a
 * bit-identical skin_joint -> marrow_index remap by running this same DFS. */
#include "joint_order.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_diag(char *diag, size_t cap, const char *fmt, ...) {
    if (!diag || cap == 0) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(diag, cap, fmt, ap);
    va_end(ap);
}

/* skin-joint index of a node, or -1 if the node is not one of the skin's joints. */
static int node_to_jointset(const cgltf_skin *skin, const cgltf_node *node) {
    for (cgltf_size i = 0; i < skin->joints_count; ++i) if (skin->joints[i] == node) return (int)i;
    return -1;
}

/* DFS the joint forest (children in ascending skin-order) from the root, filling order[] with
 * skin-order indices parent-before-child. Returns the number of joints visited (== jc on success;
 * < jc signals a malformed hierarchy: a cycle or an unreachable joint). */
static uint32_t topo_order(const int32_t *parent_js, uint32_t jc, uint32_t root_js, uint32_t *order) {
    /* explicit stack to avoid recursion depth limits; emit children in ascending skin-order by
     * pushing them in descending order so the smallest is popped first */
    uint32_t *stack = (uint32_t *)malloc((size_t)jc * sizeof(uint32_t));
    if (!stack) return 0;
    uint32_t sp = 0, cnt = 0;
    stack[sp++] = root_js;
    while (sp) {
        uint32_t cur = stack[--sp];
        order[cnt++] = cur;
        /* push children (parent_js[child]==cur) in descending index order */
        for (uint32_t ch = jc; ch-- > 0; )
            if (parent_js[ch] == (int32_t)cur) {
                if (sp >= jc) { free(stack); return 0; } /* malformed (cycle) */
                stack[sp++] = ch;
            }
    }
    free(stack);
    return cnt;
}

mrw_result mrw_g2m_joint_order_build(const cgltf_skin *skin, mrw_g2m_joint_order *out,
                                     char *diag, size_t diag_cap) {
    if (!skin || !out) { set_diag(diag, diag_cap, "invalid arguments"); return MRW_E_FORMAT; }
    memset(out, 0, sizeof *out);

    if (skin->joints_count < 1 || skin->joints_count > 0xFFFEu) {
        set_diag(diag, diag_cap, "skin has %zu joints (must be 1..65534)", skin->joints_count);
        return MRW_E_RANGE;
    }
    uint32_t jc = (uint32_t)skin->joints_count;

    int32_t  *parent_js = (int32_t  *)malloc((size_t)jc * sizeof(int32_t));
    uint32_t *order     = (uint32_t *)malloc((size_t)jc * sizeof(uint32_t));
    uint32_t *marrow_of = (uint32_t *)malloc((size_t)jc * sizeof(uint32_t));
    if (!parent_js || !order || !marrow_of) {
        free(parent_js); free(order); free(marrow_of);
        set_diag(diag, diag_cap, "out of memory");
        return MRW_E_OVERFLOW;
    }

    /* parent-joint resolution + single-root requirement: each joint's parent is the nearest
     * ancestor node that is itself one of the skin's joints (intervening non-joint nodes are folded
     * over by the caller's pose math, not here). */
    int32_t root_js = -1; uint32_t root_count = 0;
    for (uint32_t i = 0; i < jc; ++i) {
        const cgltf_node *anc = skin->joints[i]->parent;
        int pj = -1;
        while (anc) { int idx = node_to_jointset(skin, anc); if (idx >= 0) { pj = idx; break; } anc = anc->parent; }
        parent_js[i] = pj;
        if (pj < 0) { root_js = (int32_t)i; ++root_count; }
    }
    if (root_count != 1) {
        free(parent_js); free(order); free(marrow_of);
        set_diag(diag, diag_cap, "skin has %u skeleton roots (marrow requires exactly 1)", root_count);
        return MRW_E_FORMAT;
    }

    /* topological order (parent before child) + its inverse map */
    if (topo_order(parent_js, jc, (uint32_t)root_js, order) != jc) {
        free(parent_js); free(order); free(marrow_of);
        set_diag(diag, diag_cap, "joint hierarchy is not a single tree");
        return MRW_E_FORMAT;
    }
    for (uint32_t mj = 0; mj < jc; ++mj) marrow_of[order[mj]] = mj;

    out->joint_count = jc;
    out->parent_js   = parent_js;
    out->order       = order;
    out->marrow_of   = marrow_of;
    out->root_js     = root_js;
    return MRW_OK;
}

void mrw_g2m_joint_order_free(mrw_g2m_joint_order *jo) {
    if (!jo) return;
    free(jo->parent_js);
    free(jo->order);
    free(jo->marrow_of);
    memset(jo, 0, sizeof *jo);
}

/* ---- preserve-nodes (forest) ordering: see joint_order.h ---- */

/* index of a node in data->nodes, or -1 if the pointer is outside [nodes, nodes+node_count). */
static int64_t node_slot(const cgltf_node *nodes, cgltf_size node_count, const cgltf_node *n) {
    if (!n || n < nodes) return -1;
    uint64_t i = (uint64_t)(n - nodes);
    return (i < (uint64_t)node_count) ? (int64_t)i : -1;
}

mrw_result mrw_g2m_forest_order_build(const cgltf_skin *skin,
                                      const cgltf_node *nodes, cgltf_size node_count,
                                      mrw_g2m_forest_order *out,
                                      char *diag, size_t diag_cap) {
    if (!skin || !nodes || !out) { set_diag(diag, diag_cap, "invalid arguments"); return MRW_E_FORMAT; }
    memset(out, 0, sizeof *out);
    if (node_count < 1 || node_count > 0xFFFFFFFFu) {
        set_diag(diag, diag_cap, "node count %zu out of range", (size_t)node_count);
        return MRW_E_RANGE;
    }
    uint32_t nn = (uint32_t)node_count;
    uint32_t jc_slots = (uint32_t)skin->joints_count;
    if (jc_slots < 1 || jc_slots > 0xFFFEu) {
        set_diag(diag, diag_cap, "skin has %u joints (must be 1..65534)", jc_slots);
        return MRW_E_RANGE;
    }

    uint8_t *include = (uint8_t *)calloc(nn, 1);
    int32_t *pidx = (int32_t *)malloc((size_t)nn * sizeof(int32_t));
    int32_t *head = (int32_t *)malloc((size_t)nn * sizeof(int32_t));
    int32_t *next = (int32_t *)malloc((size_t)nn * sizeof(int32_t));
    int32_t *out_of_node = (int32_t *)malloc((size_t)nn * sizeof(int32_t));
    int32_t *slot_to_out = (int32_t *)malloc((size_t)jc_slots * sizeof(int32_t));
    if (!include || !pidx || !head || !next || !out_of_node || !slot_to_out) {
        free(include); free(pidx); free(head); free(next); free(out_of_node); free(slot_to_out);
        set_diag(diag, diag_cap, "out of memory");
        return MRW_E_OVERFLOW;
    }
    for (uint32_t i = 0; i < nn; ++i) { out_of_node[i] = -1; head[i] = -1; next[i] = -1; }

    /* Validate every node's parent pointer resolves inside the node array. */
    for (uint32_t i = 0; i < nn; ++i) {
        int64_t p = node_slot(nodes, node_count, nodes[i].parent);
        if (nodes[i].parent && p < 0) {
            free(include); free(pidx); free(head); free(next); free(out_of_node); free(slot_to_out);
            set_diag(diag, diag_cap, "node %u: parent outside the node array", i);
            return MRW_E_FORMAT;
        }
        pidx[i] = (int32_t)p;
    }

    /* Ancestor closure: every skin joint and everything above it up to its tree root. */
    for (cgltf_size si = 0; si < skin->joints_count; ++si) {
        int64_t s = node_slot(nodes, node_count, skin->joints[si]);
        if (s < 0) {
            free(include); free(pidx); free(head); free(next); free(out_of_node); free(slot_to_out);
            set_diag(diag, diag_cap, "skin joint %zu is outside the node array", (size_t)si);
            return MRW_E_RANGE;
        }
        int32_t cur = (int32_t)s;
        uint32_t steps = 0;
        for (;;) {
            include[cur] = 1;
            if (pidx[cur] < 0) break;
            if (++steps > nn) { /* parent chain never reaches a root ⇒ cycle */
                free(include); free(pidx); free(head); free(next); free(out_of_node); free(slot_to_out);
                set_diag(diag, diag_cap, "node hierarchy contains a cycle");
                return MRW_E_FORMAT;
            }
            cur = pidx[cur];
        }
    }

    uint32_t jc = 0;
    for (uint32_t i = 0; i < nn; ++i) if (include[i]) ++jc;
    if (jc > 0xFFFEu) {
        free(include); free(pidx); free(head); free(next); free(out_of_node); free(slot_to_out);
        set_diag(diag, diag_cap, "preserved node closure has %u nodes (must be <= 65534)", jc);
        return MRW_E_RANGE;
    }

    /* Included-children adjacency in ascending original index: prepending in ascending order leaves
     * each list in DESCENDING order (pop order ascending). Unrelated siblings are simply excluded. */
    for (uint32_t i = 0; i < nn; ++i) {
        if (!include[i]) continue;
        int32_t p = pidx[i];
        if (p >= 0 && include[p]) { next[i] = head[p]; head[p] = i; }
    }

    uint32_t *node_index = (uint32_t *)malloc((size_t)jc * sizeof(uint32_t));
    int32_t  *parent     = (int32_t *)malloc((size_t)jc * sizeof(int32_t));
    uint32_t *roots      = (uint32_t *)malloc((size_t)jc * sizeof(uint32_t));
    int32_t  *stack      = (int32_t *)malloc((size_t)jc * sizeof(int32_t));
    if (!node_index || !parent || !roots || !stack) {
        free(include); free(pidx); free(head); free(next); free(out_of_node); free(slot_to_out);
        free(node_index); free(parent); free(roots); free(stack);
        set_diag(diag, diag_cap, "out of memory");
        return MRW_E_OVERFLOW;
    }

    /* Roots by ascending original node index; each tree DFS'd parent-first, children ascending. The
     * split into separate roots keeps disjoint trees as a forest (no synthetic common root). */
    uint32_t emitted = 0, root_count = 0;
    for (uint32_t r = 0; r < nn; ++r) {
        if (!include[r] || pidx[r] >= 0) continue;
        roots[root_count++] = emitted;
        uint32_t sp = 0;
        stack[sp++] = (int32_t)r;
        while (sp) {
            int32_t cur = stack[--sp];
            uint32_t o = emitted++;
            node_index[o] = (uint32_t)cur;
            int32_t p = pidx[cur];
            parent[o] = (p >= 0) ? out_of_node[p] : -1; /* parent emitted before child */
            out_of_node[cur] = (int32_t)o;
            for (int32_t ch = head[cur]; ch >= 0; ch = next[ch]) {
                if (sp >= jc) { /* unreachable for a valid forest; guards the stack bound */
                    free(include); free(pidx); free(head); free(next); free(out_of_node); free(slot_to_out);
                    free(node_index); free(parent); free(roots); free(stack);
                    set_diag(diag, diag_cap, "joint hierarchy is malformed");
                    return MRW_E_FORMAT;
                }
                stack[sp++] = ch;
            }
        }
    }
    if (emitted != jc) { /* included nodes not reachable from any root ⇒ a cycle */
        free(include); free(pidx); free(head); free(next); free(out_of_node); free(slot_to_out);
        free(node_index); free(parent); free(roots); free(stack);
        set_diag(diag, diag_cap, "node hierarchy is not a forest");
        return MRW_E_FORMAT;
    }

    for (cgltf_size si = 0; si < skin->joints_count; ++si) {
        int64_t s = node_slot(nodes, node_count, skin->joints[si]);
        slot_to_out[si] = out_of_node[(uint32_t)s]; /* every skin joint is in the closure */
    }

    out->joint_count = jc;
    out->node_count  = nn;
    out->node_index  = node_index;
    out->parent      = parent;
    out->out_of_node = out_of_node; /* owned by *out now */
    out->slot_to_out = slot_to_out;
    out->root_count  = root_count;
    out->roots       = roots;

    free(include); free(pidx); free(head); free(next); free(stack);
    return MRW_OK;
}

void mrw_g2m_forest_order_free(mrw_g2m_forest_order *fo) {
    if (!fo) return;
    free(fo->node_index);
    free(fo->parent);
    free(fo->out_of_node);
    free(fo->slot_to_out);
    free(fo->roots);
    memset(fo, 0, sizeof *fo);
}
