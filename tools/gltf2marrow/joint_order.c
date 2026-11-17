/* See joint_order.h. The converter and any external consumer (the demo mesh loader) produce a
 * bit-identical skin_joint -> marrow_index remap by running this same DFS. */
#include "joint_order.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_diag(char *diag, size_t cap, const char *fmt, ...) {
    if (!diag || cap == 0) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(diag, cap, fmt, ap);
    va_end(ap);
}

/* Node pointer -> index within data->nodes, or -1 when the pointer is not one of data's nodes.
 * Compared as integers: a relational test on pointers that are not members of the same array is
 * undefined in C, and a malformed skin/animation can supply any pointer. */
static int64_t node_index_of(const cgltf_data *data, const cgltf_node *node) {
    if (!node) return -1;
    uintptr_t p = (uintptr_t)node;
    uintptr_t lo = (uintptr_t)data->nodes;
    if (p < lo || p >= lo + (uintptr_t)data->nodes_count * sizeof(cgltf_node)) return -1;
    if ((p - lo) % sizeof(cgltf_node) != 0) return -1;
    return (int64_t)((p - lo) / sizeof(cgltf_node));
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

/* ---- preserve-nodes (forest) ordering ----
 *
 * Allocates every array up front, so the build is all-or-nothing: any error frees everything and
 * leaves *out zeroed. The output ordering is produced by a single iterative pre-order walk over
 * the retained forest (roots ascending node index; children ascending node index), which gives
 * parent-before-child output indices for free. */
mrw_result mrw_g2m_forest_order_build(cgltf_data *data, const cgltf_skin *skin,
                                     mrw_g2m_forest_order *out,
                                     char *diag, size_t diag_cap) {
    if (!data || !skin || !out) { set_diag(diag, diag_cap, "invalid arguments"); return MRW_E_FORMAT; }
    memset(out, 0, sizeof *out);

    if (skin->joints_count < 1 || skin->joints_count > 0xFFFEu) {
        set_diag(diag, diag_cap, "skin has %zu joints (must be 1..65534)", skin->joints_count);
        return MRW_E_RANGE;
    }
    cgltf_size nn = data->nodes_count;
    uint32_t sc = (uint32_t)skin->joints_count;
    if (nn == 0) {
        set_diag(diag, diag_cap, "glTF has no nodes");
        return MRW_E_FORMAT;
    }

    /* node index of every node (nodes are a flat array, so this is pointer arithmetic - but only
     * after proving the pointer actually belongs to this data's array). */
    uint8_t *keep            = (uint8_t *)calloc(nn ? (size_t)nn : 1, 1);
    uint32_t *node_to_output = (uint32_t *)malloc((nn ? (size_t)nn : 1) * sizeof(uint32_t));
    mrw_g2m_forest_order fo;
    memset(&fo, 0, sizeof fo);
    if (!keep || !node_to_output) {
        free(keep); free(node_to_output);
        set_diag(diag, diag_cap, "out of memory");
        return MRW_E_OVERFLOW;
    }
    for (cgltf_size i = 0; i < nn; ++i) node_to_output[i] = UINT32_MAX;

    /* -- mark the skin joints, rejecting a duplicate slot (one node may appear in the retained set
     *    only once; a repeated joint slot would make skin_to_output ambiguous). `keep` holds only
     *    joint nodes at this point, so it doubles as the duplicate test. -- */
    for (uint32_t js = 0; js < sc; ++js) {
        int64_t idx = node_index_of(data, skin->joints[js]);
        if (idx < 0) {
            free(keep); free(node_to_output);
            set_diag(diag, diag_cap, "skin joint %u is not a node of this glTF", js);
            return MRW_E_FORMAT;
        }
        uint32_t ni = (uint32_t)idx;
        if (keep[ni]) {
            free(keep); free(node_to_output);
            set_diag(diag, diag_cap, "skin joint %u duplicates node %u in another skin slot", js, ni);
            return MRW_E_FORMAT;
        }
        keep[ni] = 1;
    }

    /* -- ancestor closure (up to each real tree root; stop at the first node whose parent is NULL) -- */
    for (uint32_t js = 0; js < sc; ++js) {
        uint32_t ni = (uint32_t)node_index_of(data, skin->joints[js]);
        const cgltf_node *cur = data->nodes[ni].parent;
        uint32_t depth = 0;
        while (cur) {
            int64_t ci64 = node_index_of(data, cur);
            if (ci64 < 0) {
                free(keep); free(node_to_output);
                set_diag(diag, diag_cap, "joint %u: ancestor is not a node of this glTF", js);
                return MRW_E_FORMAT;
            }
            uint32_t ci = (uint32_t)ci64;
            keep[ci] = 1;
            if (++depth > (uint32_t)nn) {  /* longer than the node count ⇒ a parent cycle */
                free(keep); free(node_to_output);
                set_diag(diag, diag_cap, "node hierarchy has a parent cycle above joint %u", js);
                return MRW_E_FORMAT;
            }
            cur = cur->parent;
        }
    }

    /* -- count the retained set; enforce the joint limit (extension still bounded) -- */
    uint32_t jc = 0;
    for (cgltf_size i = 0; i < nn; ++i) if (keep[i]) ++jc;
    if (jc < 1 || jc > 0xFFFEu) {
        free(keep); free(node_to_output);
        set_diag(diag, diag_cap, "retained node closure has %u joints (must be 1..65534)", jc);
        return MRW_E_RANGE;
    }

    /* -- final arrays -- */
    const cgltf_node **nodes_out = (const cgltf_node **)malloc((size_t)jc * sizeof(cgltf_node *));
    uint32_t *out_node_full      = (uint32_t *)malloc((size_t)jc * sizeof(uint32_t));
    uint16_t *parent             = (uint16_t *)malloc((size_t)jc * sizeof(uint16_t));
    uint32_t *skin_to_output     = (uint32_t *)malloc((size_t)sc * sizeof(uint32_t));
    uint32_t *stack              = (uint32_t *)malloc((size_t)jc * sizeof(uint32_t)); /* DFS scratch */
    uint32_t *kids               = (uint32_t *)malloc((size_t)jc * sizeof(uint32_t)); /* children scratch */
    if (!nodes_out || !out_node_full || !parent || !skin_to_output || !stack || !kids) {
        free(keep); free(node_to_output);
        free(nodes_out); free(out_node_full); free(parent); free(skin_to_output); free(stack); free(kids);
        set_diag(diag, diag_cap, "out of memory");
        return MRW_E_OVERFLOW;
    }

    /* -- pre-order DFS. Roots are pushed in DESCENDING node index so the smallest pops first;
     *    a node's retained children are gathered and pushed in descending node index too, so they
     *    pop in ascending order. (children[] order is the file's, not necessarily node order.) -- */
    uint32_t sp = 0, cnt = 0;
    for (cgltf_size ri = nn; ri-- > 0; )
        if (keep[ri] && data->nodes[ri].parent == NULL) stack[sp++] = (uint32_t)ri;

    while (sp) {
        uint32_t ni = stack[--sp];
        /* A retained node must be reached exactly once; a second visit means two listed parents (or
         * a cycle). */
        if (node_to_output[ni] != UINT32_MAX) {
            free(keep); free(node_to_output);
            free(nodes_out); free(out_node_full); free(parent); free(skin_to_output); free(stack); free(kids);
            set_diag(diag, diag_cap, "node %u is reachable twice (child cycle or split parent)", ni);
            return MRW_E_FORMAT;
        }
        node_to_output[ni] = cnt;
        out_node_full[cnt] = ni;
        /* parent output index: by construction the parent was emitted before this node. If a node
         * lists a child whose own parent link disagrees, the DFS may reach the child first - that
         * is a malformed hierarchy, not a forest, so reject rather than emit a bogus parent. */
        const cgltf_node *pn = data->nodes[ni].parent;
        if (pn) {
            int64_t pnidx = node_index_of(data, pn);
            uint32_t po = (pnidx < 0) ? UINT32_MAX : node_to_output[(uint32_t)pnidx];
            if (po == UINT32_MAX || po >= cnt) {
                free(keep); free(stack); free(kids);
                free(nodes_out); free(out_node_full); free(parent); free(skin_to_output);
                /* node_to_output and the DFS scratch are still owned here */
                free(node_to_output);
                set_diag(diag, diag_cap, "node %u: parent/child links disagree", ni);
                return MRW_E_FORMAT;
            }
            parent[cnt] = (uint16_t)po;
        } else {
            parent[cnt] = 0xFFFFu;
        }
        ++cnt;

        const cgltf_node *node = &data->nodes[ni];
        uint32_t nk = 0;
        for (cgltf_size ci = 0; ci < node->children_count; ++ci) {
            const cgltf_node *ch = node->children[ci];
            if (!ch) continue;
            int64_t chi64 = node_index_of(data, ch);
            if (chi64 < 0) {
                free(keep); free(node_to_output);
                free(nodes_out); free(out_node_full); free(parent); free(skin_to_output); free(stack); free(kids);
                set_diag(diag, diag_cap, "node %u: child is not a node of this glTF", ni);
                return MRW_E_FORMAT;
            }
            uint32_t chi = (uint32_t)chi64;
            if (keep[chi]) kids[nk++] = chi;
        }
        /* ascending by original node index, then push largest-first */
        for (uint32_t a = 1; a < nk; ++a) { /* insertion sort: sibling lists are short */
            uint32_t v = kids[a], b = a;
            while (b > 0 && kids[b - 1] > v) { kids[b] = kids[b - 1]; --b; }
            kids[b] = v;
        }
        if (sp + nk > jc) { /* every retained node is pushed at most once ⇒ a child cycle */
            free(keep); free(node_to_output);
            free(nodes_out); free(out_node_full); free(parent); free(skin_to_output); free(stack); free(kids);
            set_diag(diag, diag_cap, "node hierarchy is not a forest (child cycle)");
            return MRW_E_FORMAT;
        }
        for (uint32_t a = nk; a-- > 0; ) stack[sp++] = kids[a];
    }
    if (cnt != jc) { /* retained nodes unreachable from a real root: a parent/child inconsistency */
        free(keep); free(node_to_output);
        free(nodes_out); free(out_node_full); free(parent); free(skin_to_output); free(stack); free(kids);
        set_diag(diag, diag_cap, "retained node set is not reachable from a real root");
        return MRW_E_FORMAT;
    }

    /* -- node -> output and skin slot -> output maps -- */
    for (uint32_t mj = 0; mj < jc; ++mj)
        nodes_out[mj] = &data->nodes[out_node_full[mj]];
    for (uint32_t js = 0; js < sc; ++js)
        skin_to_output[js] = node_to_output[(uint32_t)node_index_of(data, skin->joints[js])];

    free(keep); free(stack); free(kids);

    fo.joint_count    = jc;
    fo.nodes          = nodes_out;
    fo.node_index     = out_node_full;
    fo.parent         = parent;
    fo.skin_to_output = skin_to_output;
    fo.node_to_output = node_to_output;
    *out = fo;
    return MRW_OK;
}

void mrw_g2m_forest_order_free(mrw_g2m_forest_order *fo) {
    if (!fo) return;
    free((void *)fo->nodes);
    free(fo->node_index);
    free(fo->parent);
    free(fo->skin_to_output);
    free(fo->node_to_output);
    memset(fo, 0, sizeof *fo);
}
