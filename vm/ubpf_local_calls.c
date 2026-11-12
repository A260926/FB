/*
 * Local (intra-bytecode) function calls for the uBPF interpreter.
 *
 * A CALL instruction with src == 1 is a local call.  Its target is
 * (call_pc + 1 + imm) in 8-byte instruction slots (LDDW occupies two slots).
 * Calls with src == 0 keep the historical helper behaviour and are not
 * examined here.
 *
 * Programs containing local calls are validated before they are loaded:
 *   1. every local-call target must be an in-range, real instruction;
 *   2. function entries are instruction 0 plus every local-call target;
 *   3. all ordinary control flow (fallthrough, jump targets and local-call
 *      continuation points) must stay inside one function segment;
 *   4. the call graph must be free of direct and indirect recursion.
 *
 * These checks only run when at least one local call is present, so programs
 * that only use src == 0 helper calls keep their historical validation rules.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include "ubpf_int.h"

#define LOCAL_INVALID ((uint32_t)-1)
#define MAX_FUNC_ENTRIES UBPF_MAX_INSTS

bool
ubpf_has_local_calls(const struct ebpf_inst *insts, uint32_t num_insts)
{
    uint32_t i;
    for (i = 0; i < num_insts; i++) {
        if (insts[i].opcode == EBPF_OP_LDDW) {
            /* The second LDDW slot holds immediate data, never an opcode. */
            if (i + 1 < num_insts) {
                i++;
            }
            continue;
        }
        if (insts[i].opcode == EBPF_OP_CALL && insts[i].src == 1) {
            return true;
        }
    }
    return false;
}

static bool
is_conditional_jump(uint8_t opcode)
{
    switch (opcode) {
    case EBPF_OP_JEQ_REG:
    case EBPF_OP_JEQ_IMM:
    case EBPF_OP_JGT_REG:
    case EBPF_OP_JGT_IMM:
    case EBPF_OP_JGE_REG:
    case EBPF_OP_JGE_IMM:
    case EBPF_OP_JLT_REG:
    case EBPF_OP_JLT_IMM:
    case EBPF_OP_JLE_REG:
    case EBPF_OP_JLE_IMM:
    case EBPF_OP_JSET_REG:
    case EBPF_OP_JSET_IMM:
    case EBPF_OP_JNE_REG:
    case EBPF_OP_JNE_IMM:
    case EBPF_OP_JSGT_IMM:
    case EBPF_OP_JSGT_REG:
    case EBPF_OP_JSGE_IMM:
    case EBPF_OP_JSGE_REG:
    case EBPF_OP_JSLT_IMM:
    case EBPF_OP_JSLT_REG:
    case EBPF_OP_JSLE_IMM:
    case EBPF_OP_JSLE_REG:
        return true;
    default:
        return false;
    }
}

static int
compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/*
 * Compute pc + 1 + imm without letting the addition wrap around.  Returns
 * LOCAL_INVALID when the result is not representable.
 */
static uint32_t
target_pc(uint32_t pc, int32_t imm)
{
    int64_t idx = (int64_t)pc + 1 + (int64_t)imm;
    if (idx < 0 || idx > (int64_t)UINT32_MAX) {
        return LOCAL_INVALID;
    }
    return (uint32_t)idx;
}

/* Index of the function whose segment contains pc.  entries[] is sorted
 * ascending and entries[0] == 0. */
static uint32_t
find_function(const uint32_t *entries, uint32_t num_entries, uint32_t pc)
{
    uint32_t lo = 0, hi = num_entries;
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (entries[mid] <= pc) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/*
 * Iterative depth-first search over the call graph.
 * states: 0 = unvisited, 1 = on the current path, 2 = fully explored.
 * Outgoing edges for function f are adj_edge[adj_start[f] .. adj_start[f+1]),
 * each holding a destination function index; the matching origin PC is in
 * edge_pc.  On a cycle, *bad_pc gets the PC of the call that closes it.
 */
static bool
graph_has_cycle(const uint32_t *adj_start, const uint32_t *adj_edge,
                const uint32_t *edge_pc, uint32_t num_entries,
                uint32_t *bad_pc)
{
    uint8_t *states = calloc(num_entries, 1);
    uint32_t *frame_fn = calloc(num_entries, sizeof(uint32_t));
    uint32_t *frame_pos = calloc(num_entries, sizeof(uint32_t));
    bool cycle = false;

    if (states == NULL || frame_fn == NULL || frame_pos == NULL) {
        free(states);
        free(frame_fn);
        free(frame_pos);
        return true; /* reported as a generic validation failure */
    }

    uint32_t root;
    for (root = 0; root < num_entries && !cycle; root++) {
        if (states[root] != 0) {
            continue;
        }

        uint32_t top = 0;
        frame_fn[top] = root;
        frame_pos[top] = adj_start[root];
        states[root] = 1;
        top++;

        while (top > 0) {
            uint32_t f = frame_fn[top - 1];
            uint32_t pos = frame_pos[top - 1];

            if (pos >= adj_start[f + 1]) {
                states[f] = 2;
                top--;
                continue;
            }

            frame_pos[top - 1] = pos + 1;
            uint32_t target = adj_edge[pos];
            if (states[target] == 1) {
                *bad_pc = edge_pc[pos];
                cycle = true;
                break;
            }
            if (states[target] == 0) {
                states[target] = 1;
                frame_fn[top] = target;
                frame_pos[top] = adj_start[target];
                top++;
            }
        }
    }

    free(states);
    free(frame_fn);
    free(frame_pos);
    return cycle;
}

/*
 * Longest call chain, expressed in frames.  depth[f] is 1 for a function that
 * makes no calls and otherwise 1 + the deepest callee chain.  Shared callees
 * are counted once per path, which is what the frame limit is about.
 *
 * The graph is known to be acyclic at this point.  Returns true when any
 * function's chain exceeds UBPF_MAX_FRAMES; *bad_pc then receives the PC of a
 * call edge on the deepest chain.
 */
static bool
graph_too_deep(const uint32_t *adj_start, const uint32_t *adj_edge,
               const uint32_t *edge_pc, uint32_t num_entries,
               uint32_t *bad_pc)
{
    uint32_t *indeg = calloc(num_entries, sizeof(uint32_t));
    uint32_t *order = calloc(num_entries, sizeof(uint32_t));
    uint32_t *depth = calloc(num_entries, sizeof(uint32_t));
    uint32_t *queue = calloc(num_entries, sizeof(uint32_t));
    bool too_deep = true;
    uint32_t i;

    if (indeg == NULL || order == NULL || depth == NULL || queue == NULL) {
        goto out;
    }

    for (i = 0; i < num_entries; i++) {
        uint32_t e;
        for (e = adj_start[i]; e < adj_start[i + 1]; e++) {
            indeg[adj_edge[e]]++;
        }
    }

    uint32_t head = 0, tail = 0;
    for (i = 0; i < num_entries; i++) {
        if (indeg[i] == 0) {
            queue[tail++] = i;
        }
    }

    uint32_t n = 0;
    while (head < tail) {
        uint32_t f = queue[head++];
        order[n++] = f;
        uint32_t e;
        for (e = adj_start[f]; e < adj_start[f + 1]; e++) {
            uint32_t t = adj_edge[e];
            if (--indeg[t] == 0) {
                queue[tail++] = t;
            }
        }
    }

    /* order[] is a topological order (callers first).  Walk it backwards so
     * every callee's depth is final before its caller is processed. */
    for (i = n; i > 0; i--) {
        uint32_t f = order[i - 1];
        depth[f] = 1;
        uint32_t e;
        for (e = adj_start[f]; e < adj_start[f + 1]; e++) {
            uint32_t t = adj_edge[e];
            if (depth[t] + 1 > depth[f]) {
                depth[f] = depth[t] + 1;
            }
        }
    }

    too_deep = false;
    for (i = 0; i < num_entries; i++) {
        if (depth[i] > UBPF_MAX_FRAMES) {
            uint32_t e;
            *bad_pc = 0;
            for (e = adj_start[i]; e < adj_start[i + 1]; e++) {
                if (depth[adj_edge[e]] + 1 == depth[i]) {
                    *bad_pc = edge_pc[e];
                    break;
                }
            }
            too_deep = true;
            break;
        }
    }

out:
    free(indeg);
    free(order);
    free(depth);
    free(queue);
    return too_deep;
}

bool
ubpf_validate_local_calls(const struct ebpf_inst *insts, uint32_t num_insts, char **errmsg)
{
    uint32_t *entries = NULL;
    uint32_t *site_pc = NULL;
    uint32_t *site_target = NULL;
    uint32_t *adj_start = NULL;
    uint32_t *adj_edge = NULL;
    uint32_t *edge_pc = NULL;
    uint32_t *cursor = NULL;
    bool ok = false;

    entries = calloc(MAX_FUNC_ENTRIES + 1, sizeof(entries[0]));
    site_pc = calloc(MAX_FUNC_ENTRIES, sizeof(site_pc[0]));
    site_target = calloc(MAX_FUNC_ENTRIES, sizeof(site_target[0]));
    if (entries == NULL || site_pc == NULL || site_target == NULL) {
        goto oom;
    }

    /* Pass 1: validate CALL encodings and collect function entries and call
     * sites.  The scan is ascending, so both lists stay sorted. */
    uint32_t num_entries = 0;
    uint32_t num_calls = 0;
    uint32_t i;

    entries[num_entries++] = 0;

    for (i = 0; i < num_insts; i++) {
        const struct ebpf_inst inst = insts[i];

        if (inst.opcode == EBPF_OP_LDDW) {
            /* validate() already established the second slot exists and has
             * opcode 0. */
            i++;
            continue;
        }

        if (inst.opcode != EBPF_OP_CALL || inst.src == 0) {
            continue;
        }

        /* src > 1 is already rejected by validate(); be defensive here. */
        if (inst.src != 1) {
            *errmsg = ubpf_error("invalid call src %u at PC %u", inst.src, i);
            goto out;
        }

        if (inst.dst != 0) {
            *errmsg = ubpf_error("local call with nonzero dst at PC %u", i);
            goto out;
        }
        if (inst.offset != 0) {
            *errmsg = ubpf_error("local call with nonzero offset at PC %u", i);
            goto out;
        }

        uint32_t target = target_pc(i, inst.imm);
        if (target == LOCAL_INVALID || target >= num_insts) {
            *errmsg = ubpf_error("local call target out of bounds at PC %u", i);
            goto out;
        }
        if (insts[target].opcode == 0) {
            *errmsg = ubpf_error("local call target is middle of lddw at PC %u", i);
            goto out;
        }

        if (num_entries > MAX_FUNC_ENTRIES || num_calls >= MAX_FUNC_ENTRIES) {
            *errmsg = ubpf_error("too many local call sites");
            goto out;
        }
        entries[num_entries++] = target;

        /* Until resolved below this holds the raw target PC. */
        site_pc[num_calls] = i;
        site_target[num_calls] = target;
        num_calls++;
    }

    /* Function entries are discovered in the order the calls are scanned,
     * which is not their position order.  Sort them and drop duplicates;
     * segments are defined by position order. */
    qsort(entries, num_entries, sizeof(entries[0]), compare_u32);
    uint32_t write = 1;
    for (i = 1; i < num_entries; i++) {
        if (entries[i] != entries[write - 1]) {
            entries[write++] = entries[i];
        }
    }
    num_entries = write;

    /* Resolve each call target to the index of its owning function.  A target
     * is always a function entry, so find_function returns its exact index. */
    adj_start = calloc(num_entries + 1, sizeof(adj_start[0]));
    adj_edge = calloc(num_calls > 0 ? num_calls : 1, sizeof(adj_edge[0]));
    edge_pc = calloc(num_calls > 0 ? num_calls : 1, sizeof(edge_pc[0]));
    cursor = calloc(num_entries, sizeof(cursor[0]));
    if (adj_start == NULL || adj_edge == NULL || edge_pc == NULL || cursor == NULL) {
        goto oom;
    }

    for (i = 0; i < num_calls; i++) {
        site_target[i] = find_function(entries, num_entries, site_target[i]);
        adj_start[find_function(entries, num_entries, site_pc[i]) + 1]++;
    }
    for (i = 0; i < num_entries; i++) {
        adj_start[i + 1] += adj_start[i];
    }
    for (i = 0; i < num_calls; i++) {
        uint32_t src_fn = find_function(entries, num_entries, site_pc[i]);
        uint32_t slot = adj_start[src_fn] + cursor[src_fn]++;
        adj_edge[slot] = site_target[i];
        edge_pc[slot] = site_pc[i];
    }

    /* Pass 2: control-flow containment for every instruction slot, whether
     * reachable or not. */
    for (i = 0; i < num_insts; i++) {
        const struct ebpf_inst inst = insts[i];
        uint32_t fn;

        if (inst.opcode == EBPF_OP_LDDW) {
            /* The LDDW occupies i and i+1 and falls through to i+2.  That
             * successor must also stay inside the same function. */
            fn = find_function(entries, num_entries, i);
            if (i + 2 >= num_insts) {
                *errmsg = ubpf_error("fallthrough out of bounds at PC %u", i);
                goto out;
            }
            if (find_function(entries, num_entries, i + 2) != fn) {
                *errmsg = ubpf_error("fallthrough crosses function boundary at PC %u", i);
                goto out;
            }
            i++;
            continue;
        }

        fn = find_function(entries, num_entries, i);

        if (inst.opcode == EBPF_OP_EXIT) {
            /* EXIT has no fallthrough successor. */
            continue;
        }

        if (inst.opcode == EBPF_OP_JA) {
            uint32_t t = target_pc(i, inst.offset);
            if (t == LOCAL_INVALID || t >= num_insts || insts[t].opcode == 0) {
                *errmsg = ubpf_error("invalid jump at PC %u", i);
                goto out;
            }
            if (find_function(entries, num_entries, t) != fn) {
                *errmsg = ubpf_error("cross-function jump at PC %u", i);
                goto out;
            }
            continue;
        }

        /* Everything else falls through to i + 1: conditional jumps also have
         * a branch target and local calls resume at i + 1. */
        uint32_t succ = i + 1;
        if (succ >= num_insts) {
            *errmsg = ubpf_error("fallthrough out of bounds at PC %u", i);
            goto out;
        }
        if (find_function(entries, num_entries, succ) != fn) {
            if (inst.opcode == EBPF_OP_CALL && inst.src == 1) {
                *errmsg = ubpf_error("local call continuation crosses function boundary at PC %u", i);
            } else {
                *errmsg = ubpf_error("fallthrough crosses function boundary at PC %u", i);
            }
            goto out;
        }

        if (is_conditional_jump(inst.opcode)) {
            uint32_t t = target_pc(i, inst.offset);
            if (t == LOCAL_INVALID || t >= num_insts || insts[t].opcode == 0) {
                *errmsg = ubpf_error("invalid jump at PC %u", i);
                goto out;
            }
            if (find_function(entries, num_entries, t) != fn) {
                *errmsg = ubpf_error("cross-function jump at PC %u", i);
                goto out;
            }
        }
    }

    /* Pass 3: reject direct or indirect recursion in the call graph. */
    {
        uint32_t bad_pc = 0;
        if (graph_has_cycle(adj_start, adj_edge, edge_pc, num_entries, &bad_pc)) {
            *errmsg = ubpf_error("recursive local call at PC %u", bad_pc);
            goto out;
        }

        /* Pass 4: the longest call chain, entry frame included, must fit in
         * UBPF_MAX_FRAMES.  This is a depth limit, not a count of distinct
         * functions. */
        if (graph_too_deep(adj_start, adj_edge, edge_pc, num_entries, &bad_pc)) {
            *errmsg = ubpf_error("local call chain too deep at PC %u", bad_pc);
            goto out;
        }
    }

    ok = true;
    goto out;

oom:
    *errmsg = ubpf_error("out of memory");

out:
    free(entries);
    free(site_pc);
    free(site_target);
    free(adj_start);
    free(adj_edge);
    free(edge_pc);
    free(cursor);
    if (!ok && *errmsg == NULL) {
        *errmsg = ubpf_error("local call validation failed");
    }
    return ok;
}
