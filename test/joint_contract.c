/* Contract tests for optional joint (coordinated) shrinking.
 *
 * This file has no main(): task_support/driver.c calls the three
 * contract_* entry points. Every check goes through TASK_CHECK, so the
 * wrapper counts the real assertions. An entry returns a positive value
 * after running to completion, or a negative value if theft reported an
 * unexpected run error.
 *
 * All test types keep a per-type allocation ledger, so ownership can be
 * checked explicitly: after every scenario (accept, reject, callback
 * error, property error), each ledger must show allocs == frees and no
 * live objects. */

#include "theft.h"
#include "../task_support/check.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


/*********************
 * Allocation ledger *
 *********************/

struct ledger {
    size_t allocs;
    size_t frees;
    size_t live;
};

/* Every type-specific environment starts with its ledger pointer, so
 * the joint callback can account a fresh instance against the ledger
 * of the slot's type. */
struct box_env {
    struct ledger *ledger;
    int fixed;                  /* fixed alloc value, or -1 for random */
};

struct auto_env {
    struct ledger *ledger;
};

static int *
ledger_int_alloc(struct ledger *l, int value) {
    int *p = malloc(sizeof(*p));
    assert(p != NULL);
    *p = value;
    l->allocs++;
    l->live++;
    return p;
}

static enum theft_alloc_res
plain_alloc(struct theft *t, void *penv, void **output) {
    struct box_env *be = penv;
    int value = (be->fixed < 0 ? (int)theft_random_bits(t, 8) : be->fixed);
    *output = ledger_int_alloc(be->ledger, value);
    return THEFT_ALLOC_OK;
}

static void
plain_free(void *instance, void *penv) {
    struct box_env *be = penv;
    if (instance == NULL) { return; }
    be->ledger->frees++;
    be->ledger->live--;
    free(instance);
}

static theft_hash
plain_hash(const void *instance, void *penv) {
    (void)penv;
    /* +1 so value 0 still has a non-trivial hash. */
    return (theft_hash)(*(const int *)instance) + 1;
}

/* One tactic: decrement by one, then no more tactics. */
static enum theft_shrink_res
plain_shrink(struct theft *t, const void *instance, uint32_t tactic,
    void *penv, void **output) {
    (void)t;
    struct box_env *be = penv;
    if (tactic > 0) { return THEFT_SHRINK_NO_MORE_TACTICS; }
    const int value = *(const int *)instance;
    if (value <= 0) { return THEFT_SHRINK_NO_MORE_TACTICS; }
    *output = ledger_int_alloc(be->ledger, value - 1);
    return THEFT_SHRINK_OK;
}

static void
plain_info_init(struct theft_type_info *ti, struct box_env *be,
    struct ledger *l, int fixed, bool use_hash, bool use_shrink) {
    memset(ti, 0, sizeof(*ti));
    be->ledger = l;
    be->fixed = fixed;
    ti->alloc = plain_alloc;
    ti->free = plain_free;
    ti->hash = use_hash ? plain_hash : NULL;
    ti->shrink = use_shrink ? plain_shrink : NULL;
    ti->env = be;
}

static enum theft_alloc_res
auto_alloc(struct theft *t, void *penv, void **output) {
    struct auto_env *ae = penv;
    /* Always >= 2, so the initial trial fails the slot-0 condition. */
    int value = (int)(theft_random_bits(t, 8) % 250) + 2;
    *output = ledger_int_alloc(ae->ledger, value);
    return THEFT_ALLOC_OK;
}

static void
auto_free(void *instance, void *penv) {
    struct auto_env *ae = penv;
    if (instance == NULL) { return; }
    ae->ledger->frees++;
    ae->ledger->live--;
    free(instance);
}

static void
auto_info_init(struct theft_type_info *ti, struct auto_env *ae,
    struct ledger *l, size_t max_failed_shrinks) {
    memset(ti, 0, sizeof(*ti));
    ae->ledger = l;
    ti->alloc = auto_alloc;
    ti->free = auto_free;
    ti->autoshrink_config.enable = true;
    ti->autoshrink_config.max_failed_shrinks = max_failed_shrinks;
    ti->env = ae;
}

/* Ledger of the type owning SLOT. The joint callback only receives
 * the public handle, so the setup helpers register each slot's ledger
 * in this test-side table. Scenarios run sequentially. */
static struct ledger *slot_ledgers[THEFT_MAX_ARITY];

static int *
joint_new_int(uint8_t slot, int value) {
    return ledger_int_alloc(slot_ledgers[slot], value);
}


/***********************
 * Property under test *
 ***********************/

struct prop_env {
    size_t calls;
    int skip_once_value;        /* skip once when the pair equals this */
    int pass_once_value;        /* pass once when the pair equals this */
    int error_value;            /* error when the pair equals this */
    bool skipped;
    bool passed_override;
};

/* Fails iff a == b && a >= 3. The hook environment always starts with
 * an embedded struct prop_env, so theft_hook_get_env() can be cast
 * directly to it. */
static enum theft_trial_res
correlated_prop(struct theft *t, void *arg1, void *arg2) {
    struct prop_env *pe = theft_hook_get_env(t);
    pe->calls++;
    int a = *(int *)arg1;
    int b = *(int *)arg2;
    if (pe->error_value >= 0 && a == b && a == pe->error_value) {
        return THEFT_TRIAL_ERROR;
    }
    if (!pe->skipped && pe->skip_once_value >= 0
        && a == b && a == pe->skip_once_value) {
        pe->skipped = true;
        return THEFT_TRIAL_SKIP;
    }
    if (!pe->passed_override && pe->pass_once_value >= 0
        && a == b && a == pe->pass_once_value) {
        pe->passed_override = true;
        return THEFT_TRIAL_PASS;
    }
    return (a == b && a >= 3) ? THEFT_TRIAL_FAIL : THEFT_TRIAL_PASS;
}

/* Single-argument property used for the legacy one-arg baseline:
 * fails once the value reaches 3, matching task_support/baseline.c. */
static enum theft_trial_res
unary_prop(struct theft *t, void *arg1) {
    struct prop_env *pe = theft_hook_get_env(t);
    pe->calls++;
    return (*(int *)arg1 >= 3) ? THEFT_TRIAL_FAIL : THEFT_TRIAL_PASS;
}

/* Three-arg variant: the correlated pair is slots 1 and 2, and the
 * autoshrink-managed slot 0 must stay >= 3 for the property to fail.
 * This gives slot 0's own autoshrinking a deterministic floor so it can
 * reach a dead end, while joint shrinking still never replaces it. */
static enum theft_trial_res
correlated_prop3(struct theft *t, void *arg1, void *arg2, void *arg3) {
    enum theft_trial_res r = correlated_prop(t, arg2, arg3);
    if (r != THEFT_TRIAL_FAIL) {
        return r;
    }
    return (*(int *)arg1 >= 3) ? THEFT_TRIAL_FAIL : THEFT_TRIAL_PASS;
}

/* Three-arg variant that ignores slot 0 entirely. Safe where slot 0
 * uses the bounded plain shrink callback (it terminates at 0). */
static enum theft_trial_res
correlated_prop3_ignore0(struct theft *t, void *arg1, void *arg2,
    void *arg3) {
    (void)arg1;
    return correlated_prop(t, arg2, arg3);
}


/*************************
 * Joint shrink callback *
 *************************/

struct joint_env {
    uint32_t no_more_at;        /* tactics at/after this end the round */
    bool dead_end_first;
    bool equal_rank_first;      /* fresh same-valued candidates */
    bool dup_first;             /* (a-1, b): already tried per-arg */
    bool error_first;
    bool one_slot_only;
    bool touch_autoshrink;
    bool replace_all;           /* arity 3: replace every slot */
    size_t ok_calls;
    size_t dead_end_calls;
    const void *last_view[THEFT_MAX_ARITY];
};

static uint64_t
sum_rank(const void **args, uint8_t arity, void *env) {
    (void)env;
    uint64_t rank = 0;
    for (uint8_t i = 0; i < arity; i++) {
        rank += (uint64_t)*(const int *)args[i];
    }
    return rank;
}

static enum theft_shrink_res
joint_shrink(struct theft *t, const void **args, uint8_t arity,
    uint32_t tactic, void *env, void **output) {
    (void)t;
    struct joint_env *je = env;
    for (uint8_t i = 0; i < arity; i++) {
        je->last_view[i] = args[i];
        output[i] = NULL;
    }

    if (tactic >= je->no_more_at) {
        return THEFT_SHRINK_NO_MORE_TACTICS;
    }

    if (tactic == 0 && je->error_first) {
        return THEFT_SHRINK_ERROR;
    }
    if (tactic == 0 && je->dead_end_first) {
        je->dead_end_calls++;
        return THEFT_SHRINK_DEAD_END;
    }

    if (tactic == 0 && je->one_slot_only) {
        /* Contract violation: only one replacement slot. */
        output[0] = joint_new_int(0, *(const int *)args[0] - 1);
        je->ok_calls++;
        return THEFT_SHRINK_OK;
    }
    if (tactic == 0 && je->touch_autoshrink) {
        /* Contract violation: slot 0 is autoshrink-managed. */
        output[0] = joint_new_int(0, *(const int *)args[0]);
        output[1] = joint_new_int(1, *(const int *)args[1] - 1);
        je->ok_calls++;
        return THEFT_SHRINK_OK;
    }
    if (tactic == 0 && je->equal_rank_first) {
        /* Fresh instances with the same values: rank cannot drop. */
        output[0] = joint_new_int(0, *(const int *)args[0]);
        output[1] = joint_new_int(1, *(const int *)args[1]);
        je->ok_calls++;
        return THEFT_SHRINK_OK;
    }
    if (tactic == 0 && je->dup_first) {
        /* (a-1, b): the per-arg process already called this group. */
        output[0] = joint_new_int(0, *(const int *)args[0] - 1);
        output[1] = joint_new_int(1, *(const int *)args[1]);
        je->ok_calls++;
        return THEFT_SHRINK_OK;
    }

    /* Normal coordinated step: decrement every replaceable slot,
     * clamping at 0 so a slot already at its floor keeps its value. */
    uint8_t first = (arity == 2 || je->replace_all) ? 0 : 1;
    for (uint8_t i = first; i < arity; i++) {
        int v = *(const int *)args[i];
        output[i] = joint_new_int(i, v > 0 ? v - 1 : v);
    }
    je->ok_calls++;
    return THEFT_SHRINK_OK;
}


/******************************
 * Hooks and observation state *
 ******************************/

#define EV_MAX 512
#define JOINT_POST_MAX 128

struct event_log {
    char ev[EV_MAX];
    size_t n;
};

struct hook_env {
    /* Must be first: the property casts its hook env to prop_env. */
    struct prop_env pe;

    struct event_log *log;
    struct joint_env *joint;

    /* counterexample capture */
    int ce_values[THEFT_MAX_ARITY];
    void *ce_ptrs[THEFT_MAX_ARITY];
    bool ce_seen;

    /* final trial_post capture */
    int final_values[THEFT_MAX_ARITY];
    bool final_fail_seen;

    /* joint_shrink_post observation */
    size_t joint_post_calls;
    uint32_t joint_tactics[JOINT_POST_MAX];
    enum theft_trial_res joint_results[JOINT_POST_MAX];
    size_t joint_shrink_counts[JOINT_POST_MAX];
    size_t joint_succ_counts[JOINT_POST_MAX];
    const void *joint_slot0[JOINT_POST_MAX];

    /* observer steering */
    int halt_on_fail_value;     /* halt after FAIL on this pair */
    bool halt_on_pass;
    bool error_on_post;

    /* per-arg hook bookkeeping */
    size_t single_hook_calls;
    uint8_t seen_indices[256];
    size_t n_indices;
};

static void
log_event(struct event_log *log, char c) {
    if (log->n < EV_MAX) {
        log->ev[log->n++] = c;
    }
}

static enum theft_hook_run_pre_res
quiet_run_pre(const struct theft_hook_run_pre_info *info, void *env) {
    (void)info; (void)env;
    return THEFT_HOOK_RUN_PRE_CONTINUE;
}

static enum theft_hook_run_post_res
quiet_run_post(const struct theft_hook_run_post_info *info, void *env) {
    (void)info; (void)env;
    return THEFT_HOOK_RUN_POST_CONTINUE;
}

static enum theft_hook_trial_post_res
quiet_trial_post(const struct theft_hook_trial_post_info *info, void *env) {
    struct hook_env *he = env;
    if (info->result == THEFT_TRIAL_FAIL) {
        he->final_fail_seen = true;
        for (uint8_t i = 0; i < info->arity; i++) {
            he->final_values[i] = *(int *)info->args[i];
        }
    }
    return THEFT_HOOK_TRIAL_POST_CONTINUE;
}

static enum theft_hook_counterexample_res
capture_counterexample(const struct theft_hook_counterexample_info *info,
    void *env) {
    struct hook_env *he = env;
    he->ce_seen = true;
    for (uint8_t i = 0; i < info->arity; i++) {
        he->ce_values[i] = *(int *)info->args[i];
        he->ce_ptrs[i] = info->args[i];
    }
    return THEFT_HOOK_COUNTEREXAMPLE_CONTINUE;
}

/* The per-arg shrink hooks must only ever see real arg indices, and
 * must never be invoked by joint shrinking. */
static enum theft_hook_shrink_pre_res
single_shrink_pre(const struct theft_hook_shrink_pre_info *info, void *env) {
    struct hook_env *he = env;
    he->single_hook_calls++;
    TASK_CHECK(info->arg_index < info->arity);
    if (he->n_indices < sizeof(he->seen_indices)) {
        he->seen_indices[he->n_indices++] = info->arg_index;
    }
    log_event(he->log, 's');
    return THEFT_HOOK_SHRINK_PRE_CONTINUE;
}

static enum theft_hook_shrink_post_res
single_shrink_post(const struct theft_hook_shrink_post_info *info, void *env) {
    (void)env;
    TASK_CHECK(info->arg_index < info->arity);
    return THEFT_HOOK_SHRINK_POST_CONTINUE;
}

static enum theft_hook_shrink_trial_post_res
single_shrink_trial_post(const struct theft_hook_shrink_trial_post_info *info,
    void *env) {
    (void)env;
    TASK_CHECK(info->arg_index < info->arity);
    return THEFT_HOOK_SHRINK_TRIAL_POST_CONTINUE;
}

static enum theft_hook_joint_shrink_post_res
joint_post(const struct theft_hook_joint_shrink_post_info *info, void *env) {
    struct hook_env *he = env;
    size_t n = he->joint_post_calls;
    TASK_CHECK(n < JOINT_POST_MAX);
    if (n < JOINT_POST_MAX) {
        he->joint_tactics[n] = info->tactic;
        he->joint_results[n] = info->result;
        he->joint_shrink_counts[n] = info->shrink_count;
        he->joint_succ_counts[n] = info->successful_shrinks;
        he->joint_slot0[n] = info->args[0];
        he->joint_post_calls++;
    }
    log_event(he->log, 'J');

    int a = *(int *)info->args[0];
    int b = *(int *)info->args[1];
    if (info->result == THEFT_TRIAL_FAIL
        && he->halt_on_fail_value >= 0
        && a == b && a == he->halt_on_fail_value) {
        return THEFT_HOOK_JOINT_SHRINK_POST_HALT;
    }
    if (he->halt_on_pass
        && (info->result == THEFT_TRIAL_PASS
            || info->result == THEFT_TRIAL_SKIP)) {
        return THEFT_HOOK_JOINT_SHRINK_POST_HALT;
    }
    if (he->error_on_post) {
        return THEFT_HOOK_JOINT_SHRINK_POST_ERROR;
    }
    return THEFT_HOOK_JOINT_SHRINK_POST_CONTINUE;
}


/***********************
 * Shared scaffolding *
 ***********************/

static void
check_ledger_balanced(const struct ledger *l) {
    TASK_CHECK(l->allocs == l->frees);
    TASK_CHECK(l->live == 0);
    TASK_CHECK(l->allocs > 0);
}

/* Build the standard two-slot, fixed-8 setup. */
static void
setup_pair(struct ledger *la, struct ledger *lb,
    struct box_env *ba, struct box_env *bb,
    struct theft_type_info *tia, struct theft_type_info *tib,
    bool with_hash) {
    memset(la, 0, sizeof(*la));
    memset(lb, 0, sizeof(*lb));
    plain_info_init(tia, ba, la, 8, with_hash, true);
    plain_info_init(tib, bb, lb, 8, with_hash, true);
    memset(slot_ledgers, 0, sizeof(slot_ledgers));
    slot_ledgers[0] = la;
    slot_ledgers[1] = lb;
}

/* Register the ledgers for a three-slot scenario. */
static void
setup_triple_ledgers(struct ledger *l0, struct ledger *l1,
    struct ledger *l2) {
    memset(slot_ledgers, 0, sizeof(slot_ledgers));
    slot_ledgers[0] = l0;
    slot_ledgers[1] = l1;
    slot_ledgers[2] = l2;
}

static void
hook_env_init(struct hook_env *he, struct event_log *log,
    struct joint_env *je) {
    memset(he, 0, sizeof(*he));
    he->pe.skip_once_value = -1;
    he->pe.pass_once_value = -1;
    he->pe.error_value = -1;
    he->log = log;
    he->joint = je;
    he->halt_on_fail_value = -1;
}


/******************************************
 * Group 1: joint shrinking core contract *
 ******************************************/

/* 8,8 must shrink to 3,3 even though no single-arg change preserves
 * the failure, and per-arg shrinking must re-run (and joint tactics
 * restart at 0) after every accepted joint candidate. */
static int
joint_reaches_three_three(bool dead_end_first, bool equal_rank_first,
    bool dup_first, bool with_hash) {
    struct ledger la, lb;
    struct box_env ba, bb;
    struct theft_type_info tia, tib;
    setup_pair(&la, &lb, &ba, &bb, &tia, &tib, with_hash);

    bool first_tactic_consumed =
        dead_end_first || equal_rank_first || dup_first;
    struct joint_env je = {
        .no_more_at = first_tactic_consumed ? 2 : 1,
        .dead_end_first = dead_end_first,
        .equal_rank_first = equal_rank_first,
        .dup_first = dup_first,
    };
    struct event_log log = { .n = 0 };
    struct hook_env he;
    hook_env_init(&he, &log, &je);

    struct theft_run_config cfg = {
        .prop2 = correlated_prop,
        .type_info = { &tia, &tib },
        .trials = 1,
        .seed = 1644,
        .hooks = {
            .run_pre = quiet_run_pre,
            .run_post = quiet_run_post,
            .trial_post = quiet_trial_post,
            .counterexample = capture_counterexample,
            .shrink_pre = single_shrink_pre,
            .shrink_post = single_shrink_post,
            .shrink_trial_post = single_shrink_trial_post,
            .joint_shrink_post = joint_post,
            .env = &he,
        },
        .joint = {
            .shrink = joint_shrink,
            .rank = sum_rank,
            .env = &je,
        },
    };

    enum theft_run_res res = theft_run(&cfg);
    TASK_CHECK(res == THEFT_RUN_FAIL);
    if (res != THEFT_RUN_FAIL) { return -1; }

    TASK_CHECK(he.ce_seen);
    TASK_CHECK(he.ce_values[0] == 3);
    TASK_CHECK(he.ce_values[1] == 3);
    TASK_CHECK(he.final_fail_seen);
    TASK_CHECK(he.final_values[0] == 3);
    TASK_CHECK(he.final_values[1] == 3);

    /* Five accepted joint steps: 8->7, 7->6, 6->5, 5->4, 4->3. */
    TASK_CHECK(je.ok_calls >= 5);
    TASK_CHECK(he.joint_post_calls >= 6);

    /* The last joint proposal, (2,2), strictly decreases rank but the
     * property passes: it must not be accepted. */
    TASK_CHECK(he.joint_results[he.joint_post_calls - 1]
        == THEFT_TRIAL_PASS);

    /* Every joint property call saw a complete, assembled group. */
    for (size_t i = 0; i < he.joint_post_calls; i++) {
        TASK_CHECK(he.joint_slot0[i] != NULL);
    }

    /* Per-arg shrinking ran between joint attempts: the event log must
     * contain single-arg attempts both before and after joint calls. */
    TASK_CHECK(log.n > 0);
    TASK_CHECK(log.ev[0] == 's');
    bool saw_joint = false, saw_single_after_joint = false;
    for (size_t i = 0; i < log.n; i++) {
        if (log.ev[i] == 'J') {
            saw_joint = true;
        } else if (saw_joint) {
            saw_single_after_joint = true;
        }
    }
    TASK_CHECK(saw_joint);
    TASK_CHECK(saw_single_after_joint);

    /* Tactics restart at 0 for each new argument group: every accepted
     * FAIL candidate was proposed on the same first effective tactic. */
    const uint32_t fail_tactic = first_tactic_consumed ? 1 : 0;
    size_t fail_calls = 0;
    for (size_t i = 0; i < he.joint_post_calls; i++) {
        if (he.joint_results[i] == THEFT_TRIAL_FAIL) {
            TASK_CHECK(he.joint_tactics[i] == fail_tactic);
            fail_calls++;
        }
    }
    TASK_CHECK(fail_calls == 5);

    if (dead_end_first) {
        /* DEAD_END tactics produce no property call. There are six
         * joint rounds (five accepts plus the final stall). */
        TASK_CHECK(je.dead_end_calls >= 6);
        for (size_t i = 0; i < he.joint_post_calls; i++) {
            TASK_CHECK(he.joint_tactics[i] == 1);
        }
    }
    if (equal_rank_first || dup_first) {
        /* Rejected first tactics never reached the property, so the
         * property call count is unchanged from the plain run. */
        TASK_CHECK(he.pe.calls == 19);
    }
    if (!first_tactic_consumed) {
        /* 1 initial trial, 12 single-arg attempts, 6 joint proposals. */
        TASK_CHECK(he.pe.calls == 19);
    }

    /* Per-arg hooks were only invoked for real indices. */
    TASK_CHECK(he.single_hook_calls > 0);
    for (size_t i = 0; i < he.n_indices; i++) {
        TASK_CHECK(he.seen_indices[i] == 0 || he.seen_indices[i] == 1);
    }

    check_ledger_balanced(&la);
    check_ledger_balanced(&lb);
    return 1;
}

/* PASS and SKIP joint candidates keep the whole old group; a
 * non-decreasing rank is discarded before the property is called. */
static int
joint_rejections(int kind) {
    struct ledger la, lb;
    struct box_env ba, bb;
    struct theft_type_info tia, tib;
    setup_pair(&la, &lb, &ba, &bb, &tia, &tib, true);

    /* kind 3 needs a follow-up tactic after the discarded candidate. */
    struct joint_env je = {
        .no_more_at = (kind == 3 ? 2 : 1),
        .equal_rank_first = (kind == 3),
    };
    struct event_log log = { .n = 0 };
    struct hook_env he;
    hook_env_init(&he, &log, &je);
    he.pe.skip_once_value = (kind == 1 ? 7 : -1);
    he.pe.pass_once_value = (kind == 2 ? 7 : -1);

    struct theft_run_config cfg = {
        .prop2 = correlated_prop,
        .type_info = { &tia, &tib },
        .trials = 1,
        .seed = 1644,
        .hooks = {
            .run_pre = quiet_run_pre,
            .run_post = quiet_run_post,
            .trial_post = quiet_trial_post,
            .counterexample = capture_counterexample,
            .joint_shrink_post = joint_post,
            .env = &he,
        },
        .joint = {
            .shrink = joint_shrink,
            .rank = sum_rank,
            .env = &je,
        },
    };

    enum theft_run_res res = theft_run(&cfg);
    TASK_CHECK(res == THEFT_RUN_FAIL);
    if (res != THEFT_RUN_FAIL) { return -1; }

    switch (kind) {
    case 1: /* SKIP: (7,7) rejected, stall at (8,8) */
        TASK_CHECK(he.pe.skipped);
        TASK_CHECK(he.ce_values[0] == 8);
        TASK_CHECK(he.ce_values[1] == 8);
        TASK_CHECK(he.joint_results[0] == THEFT_TRIAL_SKIP);
        break;
    case 2: /* forced PASS: same outcome */
        TASK_CHECK(he.pe.passed_override);
        TASK_CHECK(he.ce_values[0] == 8);
        TASK_CHECK(he.ce_values[1] == 8);
        TASK_CHECK(he.joint_results[0] == THEFT_TRIAL_PASS);
        break;
    case 3: /* equal rank: no property call for it, then normal shrink */
        TASK_CHECK(he.ce_values[0] == 3);
        TASK_CHECK(he.ce_values[1] == 3);
        /* The equal-rank candidate was discarded pre-property, so the
         * total call count matches the plain run. */
        TASK_CHECK(he.pe.calls == 19);
        break;
    default:
        TASK_CHECK(0);
        return -1;
    }

    check_ledger_balanced(&la);
    check_ledger_balanced(&lb);
    return 1;
}

/* The joint observer can halt or error after a real property call. */
static int
joint_observer_steering(int kind) {
    struct ledger la, lb;
    struct box_env ba, bb;
    struct theft_type_info tia, tib;
    setup_pair(&la, &lb, &ba, &bb, &tia, &tib, true);

    struct joint_env je = { .no_more_at = 1 };
    struct event_log log = { .n = 0 };
    struct hook_env he;
    hook_env_init(&he, &log, &je);
    he.pe.pass_once_value = (kind == 2 ? 7 : -1);
    he.halt_on_fail_value = (kind == 1 ? 7 : -1);
    he.halt_on_pass = (kind == 2);
    he.error_on_post = (kind == 3);

    struct theft_run_config cfg = {
        .prop2 = correlated_prop,
        .type_info = { &tia, &tib },
        .trials = 1,
        .seed = 1644,
        .hooks = {
            .run_pre = quiet_run_pre,
            .run_post = quiet_run_post,
            .trial_post = quiet_trial_post,
            .counterexample = capture_counterexample,
            .joint_shrink_post = joint_post,
            .env = &he,
        },
        .joint = {
            .shrink = joint_shrink,
            .rank = sum_rank,
            .env = &je,
        },
    };

    enum theft_run_res res = theft_run(&cfg);
    switch (kind) {
    case 1: /* halt on FAIL: the FAIL candidate is the final example */
        TASK_CHECK(res == THEFT_RUN_FAIL);
        if (res != THEFT_RUN_FAIL) { return -1; }
        TASK_CHECK(he.ce_values[0] == 7);
        TASK_CHECK(he.ce_values[1] == 7);
        TASK_CHECK(he.joint_post_calls == 1);
        break;
    case 2: /* halt on PASS: the old (8,8) counter-example is kept */
        TASK_CHECK(res == THEFT_RUN_FAIL);
        if (res != THEFT_RUN_FAIL) { return -1; }
        TASK_CHECK(he.pe.passed_override);
        TASK_CHECK(he.ce_values[0] == 8);
        TASK_CHECK(he.ce_values[1] == 8);
        break;
    case 3: /* observer error: run error, everything still cleaned up */
        TASK_CHECK(res == THEFT_RUN_ERROR);
        if (res != THEFT_RUN_ERROR) { return -1; }
        TASK_CHECK(he.joint_post_calls == 1);
        break;
    default:
        TASK_CHECK(0);
        return -1;
    }

    check_ledger_balanced(&la);
    check_ledger_balanced(&lb);
    return 1;
}

/* Configuration validation happens before any input is generated. */
static int
joint_config_validation(void) {
    struct ledger la, lb;
    struct box_env ba, bb;
    struct theft_type_info tia, tib;
    setup_pair(&la, &lb, &ba, &bb, &tia, &tib, true);

    struct prop_env pe = {
        .skip_once_value = -1,
        .pass_once_value = -1,
        .error_value = -1,
    };
    struct joint_env je = { .no_more_at = 1 };

    struct theft_run_config base = {
        .prop2 = correlated_prop,
        .type_info = { &tia, &tib },
        .trials = 1,
        .hooks = { .env = &pe },
        .joint = {
            .shrink = joint_shrink,
            .rank = sum_rank,
            .env = &je,
        },
    };

    /* Only shrink set. */
    struct theft_run_config cfg = base;
    cfg.joint.rank = NULL;
    TASK_CHECK(theft_run(&cfg) == THEFT_RUN_ERROR_BAD_ARGS);
    TASK_CHECK(la.allocs == 0 && lb.allocs == 0);

    /* Only rank set. */
    cfg = base;
    cfg.joint.shrink = NULL;
    TASK_CHECK(theft_run(&cfg) == THEFT_RUN_ERROR_BAD_ARGS);
    TASK_CHECK(la.allocs == 0 && lb.allocs == 0);

    /* Joint shrinking requires at least two arguments. */
    cfg = base;
    cfg.prop2 = NULL;
    cfg.prop1 = unary_prop;
    cfg.type_info[0] = &tia;
    cfg.type_info[1] = NULL;
    TASK_CHECK(theft_run(&cfg) == THEFT_RUN_ERROR_BAD_ARGS);
    TASK_CHECK(la.allocs == 0);

    /* Both NULL: legacy behavior, the joint callback is never reached.
     * The property here gets its env directly. */
    memset(&je, 0, sizeof(je));
    je.no_more_at = 1;
    cfg = base;
    cfg.joint.shrink = NULL;
    cfg.joint.rank = NULL;
    enum theft_run_res res = theft_run(&cfg);
    TASK_CHECK(res == THEFT_RUN_FAIL);
    if (res != THEFT_RUN_FAIL) { return -1; }
    /* Without joint shrinking the correlated pair stays at (8,8). */
    TASK_CHECK(je.ok_calls == 0);

    check_ledger_balanced(&la);
    check_ledger_balanced(&lb);
    return 1;
}

int
contract_joint(void) {
    int rc;
    rc = joint_reaches_three_three(false, false, false, true);
    if (rc < 0) { return rc; }
    rc = joint_reaches_three_three(true, false, false, true);
    if (rc < 0) { return rc; }
    rc = joint_reaches_three_three(false, true, false, true);
    if (rc < 0) { return rc; }
    rc = joint_reaches_three_three(false, false, true, true);
    if (rc < 0) { return rc; }
    /* The same core scenario must work without hashes (no bloom). */
    rc = joint_reaches_three_three(false, false, false, false);
    if (rc < 0) { return rc; }

    rc = joint_rejections(1);
    if (rc < 0) { return rc; }
    rc = joint_rejections(2);
    if (rc < 0) { return rc; }
    rc = joint_rejections(3);
    if (rc < 0) { return rc; }

    rc = joint_observer_steering(1);
    if (rc < 0) { return rc; }
    rc = joint_observer_steering(2);
    if (rc < 0) { return rc; }
    rc = joint_observer_steering(3);
    if (rc < 0) { return rc; }

    rc = joint_config_validation();
    if (rc < 0) { return rc; }
    return 1;
}


/*******************************
 * Group 2: ownership ledger *
 *******************************/

/* Callback-side and property-side errors must leave every object
 * freed exactly once, with the old group retained on property error. */
static int
ownership_error_paths(bool callback_error, bool property_error) {
    struct ledger la, lb;
    struct box_env ba, bb;
    struct theft_type_info tia, tib;
    setup_pair(&la, &lb, &ba, &bb, &tia, &tib, true);

    struct joint_env je = {
        .no_more_at = 1,
        .error_first = callback_error,
    };
    struct event_log log = { .n = 0 };
    struct hook_env he;
    hook_env_init(&he, &log, &je);
    he.pe.error_value = property_error ? 7 : -1;

    struct theft_run_config cfg = {
        .prop2 = correlated_prop,
        .type_info = { &tia, &tib },
        .trials = 1,
        .seed = 1644,
        .hooks = {
            .run_pre = quiet_run_pre,
            .run_post = quiet_run_post,
            .trial_post = quiet_trial_post,
            .joint_shrink_post = joint_post,
            .env = &he,
        },
        .joint = {
            .shrink = joint_shrink,
            .rank = sum_rank,
            .env = &je,
        },
    };

    enum theft_run_res res = theft_run(&cfg);
    TASK_CHECK(res == THEFT_RUN_ERROR);
    if (res != THEFT_RUN_ERROR) { return -1; }

    if (callback_error) {
        /* The error happened before any candidate property call. */
        TASK_CHECK(he.joint_post_calls == 0);
    } else {
        TASK_CHECK(he.joint_post_calls == 1);
        TASK_CHECK(he.joint_results[0] == THEFT_TRIAL_ERROR);
    }

    check_ledger_balanced(&la);
    check_ledger_balanced(&lb);
    return 1;
}

/* An OK result replacing fewer than two slots is a controlled error. */
static int
ownership_one_slot_violation(void) {
    struct ledger la, lb;
    struct box_env ba, bb;
    struct theft_type_info tia, tib;
    setup_pair(&la, &lb, &ba, &bb, &tia, &tib, true);

    struct joint_env je = {
        .no_more_at = 1,
        .one_slot_only = true,
    };
    struct event_log log = { .n = 0 };
    struct hook_env he;
    hook_env_init(&he, &log, &je);

    struct theft_run_config cfg = {
        .prop2 = correlated_prop,
        .type_info = { &tia, &tib },
        .trials = 1,
        .seed = 1644,
        .hooks = {
            .run_pre = quiet_run_pre,
            .run_post = quiet_run_post,
            .trial_post = quiet_trial_post,
            .joint_shrink_post = joint_post,
            .env = &he,
        },
        .joint = {
            .shrink = joint_shrink,
            .rank = sum_rank,
            .env = &je,
        },
    };

    enum theft_run_res res = theft_run(&cfg);
    TASK_CHECK(res == THEFT_RUN_ERROR);
    if (res != THEFT_RUN_ERROR) { return -1; }
    /* The single candidate instance was cleaned up, no property call. */
    TASK_CHECK(he.joint_post_calls == 0);
    check_ledger_balanced(&la);
    check_ledger_balanced(&lb);
    return 1;
}

/* Mix an unreplaced autoshrink slot with joint shrinking of the other
 * two slots, with no hash callbacks (bloom disabled). */
static int
ownership_autoshrink_mix(bool violate_autoshrink) {
    struct ledger l0, l1, l2;
    struct auto_env a0;
    struct box_env b1, b2;
    struct theft_type_info ti0, ti1, ti2;
    memset(&l0, 0, sizeof(l0));
    memset(&l1, 0, sizeof(l1));
    memset(&l2, 0, sizeof(l2));
    auto_info_init(&ti0, &a0, &l0, 4);
    plain_info_init(&ti1, &b1, &l1, 8, false, true);
    plain_info_init(&ti2, &b2, &l2, 8, false, true);
    setup_triple_ledgers(&l0, &l1, &l2);

    struct joint_env je = {
        .no_more_at = 1,
        .touch_autoshrink = violate_autoshrink,
    };
    struct event_log log = { .n = 0 };
    struct hook_env he;
    hook_env_init(&he, &log, &je);

    struct theft_run_config cfg = {
        .prop3 = correlated_prop3,
        .type_info = { &ti0, &ti1, &ti2 },
        .trials = 1,
        .seed = 1644,
        .hooks = {
            .run_pre = quiet_run_pre,
            .run_post = quiet_run_post,
            .trial_post = quiet_trial_post,
            .counterexample = capture_counterexample,
            .joint_shrink_post = joint_post,
            .env = &he,
        },
        .joint = {
            .shrink = joint_shrink,
            .rank = sum_rank,
            .env = &je,
        },
    };

    enum theft_run_res res = theft_run(&cfg);
    if (violate_autoshrink) {
        TASK_CHECK(res == THEFT_RUN_ERROR);
        if (res != THEFT_RUN_ERROR) { return -1; }
        TASK_CHECK(he.joint_post_calls == 0);
    } else {
        TASK_CHECK(res == THEFT_RUN_FAIL);
        if (res != THEFT_RUN_FAIL) { return -1; }
        /* The correlated pair still reaches (3, 3), and slot 0's own
         * autoshrinking bottoms out at the property's floor of 3. */
        TASK_CHECK(he.ce_values[0] == 3);
        TASK_CHECK(he.ce_values[1] == 3);
        TASK_CHECK(he.ce_values[2] == 3);

        /* Joint shrinking never replaced slot 0: in every joint view
         * it is exactly the instance the shrink callback was given. */
        for (size_t i = 0; i < he.joint_post_calls; i++) {
            TASK_CHECK(he.joint_slot0[i] == je.last_view[0]);
        }
        TASK_CHECK(je.ok_calls >= 5);
    }

    check_ledger_balanced(&l0);
    check_ledger_balanced(&l1);
    check_ledger_balanced(&l2);
    return 1;
}

/* On acceptance, unreplaced slots keep the exact same object for the
 * whole run; replaced slots free only the old object. */
static int
ownership_unreplaced_slot_identity(void) {
    struct ledger la, lb, lc;
    struct box_env ba, bb, bc;
    struct theft_type_info tia, tib, tic;
    memset(&la, 0, sizeof(la));
    memset(&lb, 0, sizeof(lb));
    memset(&lc, 0, sizeof(lc));
    plain_info_init(&tia, &ba, &la, 11, true, false);
    plain_info_init(&tib, &bb, &lb, 8, true, true);
    plain_info_init(&tic, &bc, &lc, 8, true, true);
    setup_triple_ledgers(&la, &lb, &lc);

    struct joint_env je = { .no_more_at = 1 };
    struct event_log log = { .n = 0 };
    struct hook_env he;
    hook_env_init(&he, &log, &je);

    struct theft_run_config cfg = {
        .prop3 = correlated_prop3_ignore0,
        .type_info = { &tia, &tib, &tic },
        .trials = 1,
        .seed = 1644,
        .hooks = {
            .run_pre = quiet_run_pre,
            .run_post = quiet_run_post,
            .trial_post = quiet_trial_post,
            .counterexample = capture_counterexample,
            .joint_shrink_post = joint_post,
            .env = &he,
        },
        .joint = {
            .shrink = joint_shrink,
            .rank = sum_rank,
            .env = &je,
        },
    };

    enum theft_run_res res = theft_run(&cfg);
    TASK_CHECK(res == THEFT_RUN_FAIL);
    if (res != THEFT_RUN_FAIL) { return -1; }

    /* Slot 0 has no shrink callback and is never touched by joint
     * shrinking: its sole allocation is the counter-example object. */
    TASK_CHECK(la.allocs == 1);
    TASK_CHECK(he.ce_values[0] == 11);
    TASK_CHECK(he.ce_values[1] == 3);
    TASK_CHECK(he.ce_values[2] == 3);
    if (he.joint_post_calls > 0) {
        const void *slot0 = he.joint_slot0[0];
        for (size_t i = 1; i < he.joint_post_calls; i++) {
            TASK_CHECK(he.joint_slot0[i] == slot0);
        }
    }

    check_ledger_balanced(&la);
    check_ledger_balanced(&lb);
    check_ledger_balanced(&lc);
    return 1;
}

int
contract_ownership(void) {
    int rc;
    rc = ownership_error_paths(true, false);
    if (rc < 0) { return rc; }
    rc = ownership_error_paths(false, true);
    if (rc < 0) { return rc; }
    rc = ownership_one_slot_violation();
    if (rc < 0) { return rc; }
    rc = ownership_autoshrink_mix(false);
    if (rc < 0) { return rc; }
    rc = ownership_autoshrink_mix(true);
    if (rc < 0) { return rc; }
    rc = ownership_unreplaced_slot_identity();
    if (rc < 0) { return rc; }
    return 1;
}


/******************************************
 * Group 3: backward compatibility *
 ******************************************/

/* Without joint shrinking, results are identical to the old engine. */
static int
compat_legacy_results(void) {
    /* Two-arg legacy run: the correlated pair is stuck at (8,8). */
    struct ledger la, lb;
    struct box_env ba, bb;
    struct theft_type_info tia, tib;
    setup_pair(&la, &lb, &ba, &bb, &tia, &tib, true);

    struct event_log log = { .n = 0 };
    struct hook_env he;
    hook_env_init(&he, &log, NULL);

    struct theft_run_config cfg = {
        .prop2 = correlated_prop,
        .type_info = { &tia, &tib },
        .trials = 1,
        .seed = 1644,
        .hooks = {
            .run_pre = quiet_run_pre,
            .run_post = quiet_run_post,
            .trial_post = quiet_trial_post,
            .counterexample = capture_counterexample,
            .shrink_pre = single_shrink_pre,
            .shrink_post = single_shrink_post,
            .shrink_trial_post = single_shrink_trial_post,
            .env = &he,
        },
    };

    enum theft_run_res res = theft_run(&cfg);
    TASK_CHECK(res == THEFT_RUN_FAIL);
    if (res != THEFT_RUN_FAIL) { return -1; }
    TASK_CHECK(he.ce_values[0] == 8);
    TASK_CHECK(he.ce_values[1] == 8);
    /* Initial trial plus two single-arg attempts, both rejected. */
    TASK_CHECK(he.pe.calls == 3);
    TASK_CHECK(he.joint_post_calls == 0);
    /* Each arg tries tactic 0 and then reports no more tactics: two
     * shrink_pre calls per real index. */
    TASK_CHECK(he.single_hook_calls == 4);
    check_ledger_balanced(&la);
    check_ledger_balanced(&lb);

    /* Single-arg legacy run shrinks exactly like the baseline. */
    struct ledger l1;
    struct box_env b1;
    struct theft_type_info ti1;
    memset(&l1, 0, sizeof(l1));
    plain_info_init(&ti1, &b1, &l1, 8, true, true);

    struct event_log log1 = { .n = 0 };
    struct hook_env he1;
    hook_env_init(&he1, &log1, NULL);

    struct theft_run_config cfg1 = {
        .prop1 = unary_prop,
        .type_info = { &ti1 },
        .trials = 1,
        .seed = 1644,
        .hooks = {
            .run_pre = quiet_run_pre,
            .run_post = quiet_run_post,
            .trial_post = quiet_trial_post,
            .counterexample = capture_counterexample,
            .env = &he1,
        },
    };
    res = theft_run(&cfg1);
    TASK_CHECK(res == THEFT_RUN_FAIL);
    if (res != THEFT_RUN_FAIL) { return -1; }
    TASK_CHECK(he1.ce_values[0] == 3);
    TASK_CHECK(he1.final_values[0] == 3);
    check_ledger_balanced(&l1);
    return 1;
}

/* Joint shrinking counts one group-level attempt regardless of how
 * many slots the candidate replaces. */
static int
compat_group_level_counts(void) {
    struct ledger la, lb, lc;
    struct box_env ba, bb, bc;
    struct theft_type_info tia, tib, tic;
    memset(&la, 0, sizeof(la));
    memset(&lb, 0, sizeof(lb));
    memset(&lc, 0, sizeof(lc));
    plain_info_init(&tia, &ba, &la, 8, true, true);
    plain_info_init(&tib, &bb, &lb, 8, true, true);
    plain_info_init(&tic, &bc, &lc, 8, true, true);
    setup_triple_ledgers(&la, &lb, &lc);

    struct joint_env je = {
        .no_more_at = 1,
        .replace_all = true,
    };
    struct event_log log = { .n = 0 };
    struct hook_env he;
    hook_env_init(&he, &log, &je);

    struct theft_run_config cfg = {
        .prop3 = correlated_prop3_ignore0,
        .type_info = { &tia, &tib, &tic },
        .trials = 1,
        .seed = 1644,
        .hooks = {
            .run_pre = quiet_run_pre,
            .run_post = quiet_run_post,
            .trial_post = quiet_trial_post,
            .counterexample = capture_counterexample,
            .joint_shrink_post = joint_post,
            .env = &he,
        },
        .joint = {
            .shrink = joint_shrink,
            .rank = sum_rank,
            .env = &je,
        },
    };

    enum theft_run_res res = theft_run(&cfg);
    TASK_CHECK(res == THEFT_RUN_FAIL);
    if (res != THEFT_RUN_FAIL) { return -1; }
    /* Slot 0 is ignored by the property: per-arg shrinking drives it
     * to 0; joint steps then carry the correlated pair to (3, 3). */
    TASK_CHECK(he.ce_values[0] == 0);
    TASK_CHECK(he.ce_values[1] == 3);
    TASK_CHECK(he.ce_values[2] == 3);

    /* Every joint observation reflects one group-level shrink attempt,
     * even though three slots were replaced at once. successful_shrinks
     * also moves during the intervening per-arg passes, so assert on
     * joint-level deltas: each joint FAIL adds exactly one (never one
     * per replaced slot), and nothing decreases. */
    TASK_CHECK(he.joint_post_calls >= 6);
    for (size_t i = 1; i < he.joint_post_calls; i++) {
        TASK_CHECK(he.joint_shrink_counts[i]
            > he.joint_shrink_counts[i - 1]);
    }
    size_t succ = 0;
    for (size_t i = 0; i < he.joint_post_calls; i++) {
        if (he.joint_results[i] == THEFT_TRIAL_FAIL) {
            succ++;
            if (i > 0) {
                TASK_CHECK(he.joint_succ_counts[i]
                    > he.joint_succ_counts[i - 1]);
            }
        } else if (i > 0) {
            TASK_CHECK(he.joint_succ_counts[i]
                >= he.joint_succ_counts[i - 1]);
        }
    }
    TASK_CHECK(succ == 5);
    /* Exact final count: slot 0 accepts 8 per-arg shrinks (8 down to
     * 0), the correlated pair accepts none per-arg, and the five joint
     * steps count once per group -- not three times per replaced slot. */
    TASK_CHECK(he.joint_succ_counts[he.joint_post_calls - 1]
        == 8 + 5);

    check_ledger_balanced(&la);
    check_ledger_balanced(&lb);
    check_ledger_balanced(&lc);
    return 1;
}

/* The old per-arg hooks keep seeing real arg indices even while joint
 * shrinking is enabled, and counterexample/trial_post receive the
 * complete, still-failing argument group. */
static int
compat_old_hooks_and_final_group(void) {
    struct ledger la, lb;
    struct box_env ba, bb;
    struct theft_type_info tia, tib;
    setup_pair(&la, &lb, &ba, &bb, &tia, &tib, true);

    struct joint_env je = { .no_more_at = 1 };
    struct event_log log = { .n = 0 };
    struct hook_env he;
    hook_env_init(&he, &log, &je);

    struct theft_run_config cfg = {
        .prop2 = correlated_prop,
        .type_info = { &tia, &tib },
        .trials = 1,
        .seed = 1644,
        .hooks = {
            .run_pre = quiet_run_pre,
            .run_post = quiet_run_post,
            .trial_post = quiet_trial_post,
            .counterexample = capture_counterexample,
            .shrink_pre = single_shrink_pre,
            .shrink_post = single_shrink_post,
            .shrink_trial_post = single_shrink_trial_post,
            .joint_shrink_post = joint_post,
            .env = &he,
        },
        .joint = {
            .shrink = joint_shrink,
            .rank = sum_rank,
            .env = &je,
        },
    };

    enum theft_run_res res = theft_run(&cfg);
    TASK_CHECK(res == THEFT_RUN_FAIL);
    if (res != THEFT_RUN_FAIL) { return -1; }

    /* Every per-arg hook call carried a real index; joint shrinking
     * never fed a pseudo-index into those hooks. */
    TASK_CHECK(he.n_indices == he.single_hook_calls);
    for (size_t i = 0; i < he.n_indices; i++) {
        TASK_CHECK(he.seen_indices[i] == 0 || he.seen_indices[i] == 1);
    }

    /* The final counter-example and trial_post view are the complete,
     * still-failing group (3, 3). */
    TASK_CHECK(he.ce_seen);
    TASK_CHECK(he.ce_ptrs[0] != NULL && he.ce_ptrs[1] != NULL);
    TASK_CHECK(he.ce_ptrs[0] != he.ce_ptrs[1]);
    TASK_CHECK(he.ce_values[0] == 3 && he.ce_values[1] == 3);
    TASK_CHECK(he.final_fail_seen);
    TASK_CHECK(he.final_values[0] == 3 && he.final_values[1] == 3);

    check_ledger_balanced(&la);
    check_ledger_balanced(&lb);
    return 1;
}

int
contract_compat(void) {
    int rc;
    rc = compat_legacy_results();
    if (rc < 0) { return rc; }
    rc = compat_group_level_counts();
    if (rc < 0) { return rc; }
    rc = compat_old_hooks_and_final_group();
    if (rc < 0) { return rc; }
    return 1;
}
