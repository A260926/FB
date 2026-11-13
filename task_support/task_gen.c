#include "task_gen.h"
#include <stdio.h>
#include <string.h>

static TaskNode *gen(TaskArena *a, int depth);

static uint32_t pick_pool_scalar(TaskArena *a) {
    const uint32_t *p = task_pool_all();
    uint32_t cp = p[task_rng_below(&a->rng, TASK_POOL_N)];
    if (task_is_supplementary(cp)) a->has_supp = 1;
    else if (cp >= 0x80u) a->has_bmp_nonascii = 1;
    return cp;
}

static TaskNode *make_lit(TaskArena *a) {
    TaskNode *n = task_alloc_node(a);
    if (!n) return NULL;
    n->kind = TASK_LIT;
    n->cp = pick_pool_scalar(a);
    return n;
}

static TaskNode *make_class(TaskArena *a) {
    TaskNode *n = task_alloc_node(a);
    int want, i, j;
    if (!n) return NULL;
    n->kind = TASK_CLASS;
    n->neg = task_rng_chance(&a->rng, 25);
    want = 1 + (int)task_rng_below(&a->rng, 6);   /* 1..6 members, non-empty */
    if (want > 8) want = 8;
    for (i = 0; i < want; i++) {
        uint32_t cp = pick_pool_scalar(a);
        for (j = 0; j < n->nchars; j++) if (n->chars[j] == cp) break;
        if (j == n->nchars && n->nchars < 8) n->chars[n->nchars++] = cp;
    }
    return n;
}

static TaskNode *make_leaf(TaskArena *a) {
    return task_rng_chance(&a->rng, 55) ? make_lit(a) : make_class(a);
}

static TaskNode *make_cap(TaskArena *a, int depth) {
    TaskNode *n = task_alloc_node(a);
    if (!n) return NULL;
    n->kind = TASK_CAP;
    n->capno = ++a->caps;                            /* preorder = PCRE order */
    if (task_rng_chance(&a->rng, 40))
        snprintf(n->name, sizeof(n->name), "n%d", n->capno);
    n->ch[0] = gen(a, depth + 1);
    n->nch = n->ch[0] ? 1 : 0;
    return n->ch[0] ? n : NULL;
}

static TaskNode *make_rep(TaskArena *a, int depth) {
    TaskNode *n = task_alloc_node(a);
    if (!n) return NULL;
    n->kind = TASK_REP;
    n->rmin = (int)task_rng_below(&a->rng, 5);       /* 0..4 */
    n->rmax = n->rmin + (int)task_rng_below(&a->rng, 5 - n->rmin);
    n->ch[0] = gen(a, depth + 1);
    n->nch = n->ch[0] ? 1 : 0;
    a->has_rep = 1;
    return n->ch[0] ? n : NULL;
}

static TaskNode *make_fixed(TaskArena *a, TaskKind kind, int depth) {
    TaskNode *n = task_alloc_node(a);
    int i, branches;
    if (!n) return NULL;
    n->kind = kind;
    branches = 2 + (int)task_rng_below(&a->rng, 2);  /* 2 or 3 */
    for (i = 0; i < branches; i++) {
        n->ch[i] = gen(a, depth + 1);
        if (!n->ch[i]) return NULL;
        n->nch++;
    }
    if (kind == TASK_ALT) a->has_alt = 1;
    return n;
}

static TaskNode *gen(TaskArena *a, int depth) {
    /* Weighted choice among only the constructs still legal at this depth. */
    enum { W_LIT, W_CLASS, W_CAT, W_ALT, W_GROUP, W_CAP, W_REP, W_N };
    int weights[W_N] = {2, 2, 3, 2, 2, 5, 3};
    int total, pick, i;
    if (depth >= TASK_MAX_DEPTH) return make_leaf(a);
    if (a->caps >= TASK_MAX_CAPS) weights[W_CAP] = 0;
    total = 0;
    for (i = 0; i < W_N; i++) total += weights[i];
    pick = (int)task_rng_below(&a->rng, (uint64_t)total);
    for (i = 0; i < W_N; i++) {
        pick -= weights[i];
        if (pick < 0) {
            switch (i) {
            case W_LIT:   return make_lit(a);
            case W_CLASS: return make_class(a);
            case W_CAT:   return make_fixed(a, TASK_CAT, depth);
            case W_ALT:   return make_fixed(a, TASK_ALT, depth);
            case W_GROUP: {
                TaskNode *n = task_alloc_node(a);
                if (!n) return NULL;
                n->kind = TASK_GROUP;
                n->ch[0] = gen(a, depth + 1);
                n->nch = n->ch[0] ? 1 : 0;
                return n->ch[0] ? n : NULL;
            }
            case W_CAP:   return make_cap(a, depth);
            default:      return make_rep(a, depth);
            }
        }
    }
    return make_leaf(a);
}

TaskNode *task_gen_tree(TaskArena *a) {
    TaskNode *n;
    task_arena_reset(a);
    n = gen(a, 1);
    if (n) a->depth_seen = task_tree_depth(n);
    return n;
}

static void collect(const TaskNode *n, uint32_t *out, int cap, int *cnt) {
    int i, j;
    if (!n) return;
    if (n->kind == TASK_LIT) {
        for (j = 0; j < *cnt; j++) if (out[j] == n->cp) break;
        if (j == *cnt && *cnt < cap) out[(*cnt)++] = n->cp;
    }
    if (n->kind == TASK_CLASS) {
        for (i = 0; i < n->nchars; i++) {
            for (j = 0; j < *cnt; j++) if (out[j] == n->chars[i]) break;
            if (j == *cnt && *cnt < cap) out[(*cnt)++] = n->chars[i];
        }
    }
    for (i = 0; i < n->nch; i++) collect(n->ch[i], out, cap, cnt);
}

int task_tree_alphabet(const TaskNode *n, uint32_t *out, int cap) {
    int cnt = 0;
    collect(n, out, cap, &cnt);
    return cnt;
}

int task_gen_subject(TaskArena *a, const TaskNode *tree,
                     uint32_t *out, int cap) {
    uint32_t alpha[TASK_POOL_N + 16];
    int nalpha = task_tree_alphabet(tree, alpha, (int)(sizeof(alpha)/sizeof(alpha[0])));
    int len, i;
    if (cap < TASK_MAX_SUBJ) return -1;
    /* 15% empty subjects (empty match / non-match coverage). */
    if (task_rng_chance(&a->rng, 15)) return 0;
    len = 1 + (int)task_rng_below(&a->rng, TASK_MAX_SUBJ);
    for (i = 0; i < len; i++) {
        if (nalpha > 0 && task_rng_chance(&a->rng, 70))
            out[i] = alpha[task_rng_below(&a->rng, (uint64_t)nalpha)];
        else
            out[i] = pick_pool_scalar(a);
    }
    return len;
}

int task_tree_has_kind(const TaskNode *n, TaskKind k) {
    int i;
    if (!n) return 0;
    if (n->kind == k) return 1;
    for (i = 0; i < n->nch; i++)
        if (task_tree_has_kind(n->ch[i], k)) return 1;
    return 0;
}

static int node_has_supp(const TaskNode *n) {
    int i;
    if (n->kind == TASK_LIT && task_is_supplementary(n->cp)) return 1;
    if (n->kind == TASK_CLASS)
        for (i = 0; i < n->nchars; i++)
            if (task_is_supplementary(n->chars[i])) return 1;
    for (i = 0; i < n->nch; i++)
        if (node_has_supp(n->ch[i])) return 1;
    return 0;
}

int task_tree_has_supplementary(const TaskNode *n) {
    return n ? node_has_supp(n) : 0;
}
