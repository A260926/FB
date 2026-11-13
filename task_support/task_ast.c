#include "task_ast.h"
#include <string.h>

/* Scalar pools. ASCII, non-ASCII BMP (Latin, Greek, CJK, symbol), and
 * supplementary plane (musical symbol, emoji, CJK ext B). None is a surrogate
 * (D800..DFFF) and none is a regex metacharacter, so raw rendering is safe. */
const uint32_t task_pool_ascii[TASK_POOL_ASCII_N] = {
    0x0061u, /* a */ 0x0062u, /* b */ 0x0063u, /* c */
    0x0041u, /* A */ 0x0030u  /* 0 */
};
const uint32_t task_pool_bmp[TASK_POOL_BMP_N] = {
    0x00E9u,  /* e-acute */
    0x03A9u,  /* Greek capital omega */
    0x4E2Du,  /* CJK "middle" */
    0x2603u   /* snowman */
};
const uint32_t task_pool_supp[TASK_POOL_SUP_N] = {
    0x1D11Eu, /* musical G clef */
    0x1F600u, /* grinning face */
    0x20000u  /* CJK Unified Ideograph Extension B */
};

static uint32_t pool_all_cache[TASK_POOL_N];
static int pool_all_built = 0;

const uint32_t *task_pool_all(void) {
    int i, j = 0;
    if (pool_all_built) return pool_all_cache;
    for (i = 0; i < TASK_POOL_ASCII_N; i++) pool_all_cache[j++] = task_pool_ascii[i];
    for (i = 0; i < TASK_POOL_BMP_N; i++)   pool_all_cache[j++] = task_pool_bmp[i];
    for (i = 0; i < TASK_POOL_SUP_N; i++)   pool_all_cache[j++] = task_pool_supp[i];
    pool_all_built = 1;
    return pool_all_cache;
}

int task_is_surrogate(uint32_t cp) {
    return cp >= 0xD800u && cp <= 0xDFFFu;
}

int task_is_supplementary(uint32_t cp) {
    return cp >= 0x10000u && cp <= 0x10FFFFu;
}

void task_arena_init(TaskArena *a, uint64_t seed) {
    memset(a, 0, sizeof(*a));
    task_rng_init(&a->rng, seed);
}

void task_arena_reset(TaskArena *a) {
    /* RNG keeps rolling: a reset arena continues the deterministic stream. */
    a->used = 0;
    a->caps = 0;
    a->depth_seen = 0;
    a->has_alt = a->has_rep = a->has_supp = a->has_bmp_nonascii = 0;
}

TaskNode *task_alloc_node(TaskArena *a) {
    TaskNode *n;
    if (a->used >= TASK_ARENA_CAP) return NULL;
    n = &a->nodes[a->used++];
    memset(n, 0, sizeof(*n));
    return n;
}

int task_tree_depth(const TaskNode *n) {
    int i, d, best = 0;
    if (!n) return 0;
    for (i = 0; i < n->nch; i++) {
        d = task_tree_depth(n->ch[i]);
        if (d > best) best = d;
    }
    return best + 1;
}

int task_tree_caps(const TaskNode *n) {
    int i, count = 0;
    if (!n) return 0;
    if (n->kind == TASK_CAP) count = 1;
    for (i = 0; i < n->nch; i++) count += task_tree_caps(n->ch[i]);
    return count;
}
