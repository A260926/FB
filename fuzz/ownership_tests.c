/*
 * Ownership-model tests.
 *
 * Registered through task_support/check.h as four groups (transfer,
 * exceptions, replay, diagnostics) of real assertions. The fixed scenarios
 * exercise alias release, holder replacement, surviving aliases after a
 * rejected assignment, consecutive throws, catch-and-rethrow and the
 * borrow-and-throw callback. A deterministic generator builds at least 256
 * valid plans; each is replayed through the shared chain, which runs it
 * twice in separate runtimes. Fixed cases and the generator live here, as
 * permitted by task_support/contract.md.
 */
#include "task_support/check.h"
#include "fuzz/ownership_model.h"
#include "fuzz/ownership_replay.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------- plan construction helpers ---------- */

static void plan_init(qjs_ownership_plan *p)
{
    p->count = 0;
}

static int plan_add(qjs_ownership_plan *p, uint8_t op, uint8_t a, uint8_t b)
{
    if (p->count >= QJS_OWN_MAX_STEPS)
        return 1;
    p->steps[p->count].op = op;
    p->steps[p->count].a = a;
    p->steps[p->count].b = b;
    p->count++;
    return 0;
}

static size_t plan_serialize(const qjs_ownership_plan *p, uint8_t *buf,
                             size_t buf_size)
{
    size_t i;
    if (buf_size < p->count * 3u)
        return 0;
    for (i = 0; i < p->count; i++) {
        buf[i * 3u] = p->steps[i].op;
        buf[i * 3u + 1u] = p->steps[i].a;
        buf[i * 3u + 2u] = p->steps[i].b;
    }
    return p->count * 3u;
}

static int run_plan(const qjs_ownership_plan *p,
                    struct qjs_ownership_report *rep)
{
    uint8_t buf[QJS_OWN_PLAN_MAX_SIZE];
    size_t n = plan_serialize(p, buf, sizeof(buf));
    TASK_CHECK(n == p->count * 3u);
    return qjs_ownership_run(buf, n, rep);
}

/* ---------- transfer group ---------- */

/* Alias release: one identity behind two roots survives the first DROP and
 * is finalized only once the last alias goes away. */
static int test_alias_release(void)
{
    qjs_ownership_plan p;
    struct qjs_ownership_report rep;
    qjs_ownership_model m;

    plan_init(&p);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_NEW, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_DUP, 0, 1) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_DROP, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_GC, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_DROP, 1, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_GC, 0, 0) == 0);

    TASK_CHECK(run_plan(&p, &rep) == 0);
    TASK_CHECK(rep.steps == 6);
    TASK_CHECK(rep.created == 1);
    TASK_CHECK(rep.finalized == 1);
    TASK_CHECK(rep.comparisons > 0);
    TASK_CHECK(rep.exceptions == 0);

    /* The model independently agrees about the responsibility movement. */
    qjs_ownership_model_init(&m);
    TASK_CHECK(qjs_ownership_model_apply(&m, &p.steps[0]) == 0);
    TASK_CHECK(qjs_ownership_model_apply(&m, &p.steps[1]) == 0);
    TASK_CHECK(qjs_ownership_model_resp(&m, 0, QJS_OWN_RESP_ROOT) == 2);
    TASK_CHECK(qjs_ownership_model_alive(&m, 0));
    TASK_CHECK(qjs_ownership_model_apply(&m, &p.steps[2]) == 0);
    TASK_CHECK(qjs_ownership_model_resp(&m, 0, QJS_OWN_RESP_ROOT) == 1);
    TASK_CHECK(qjs_ownership_model_alive(&m, 0));
    TASK_CHECK(qjs_ownership_model_apply(&m, &p.steps[4]) == 0);
    TASK_CHECK(qjs_ownership_model_finalized(&m, 0));
    return 0;
}

/* Holder replacement: storing a second identity releases the first one's
 * holder responsibility; CLEAR releases the survivor. */
static int test_holder_replace(void)
{
    qjs_ownership_plan p;
    struct qjs_ownership_report rep;

    plan_init(&p);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_NEW, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_NEW, 1, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_STORE_TAKE, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_GC, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_STORE_TAKE, 1, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_CLEAR, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_GC, 0, 0) == 0);

    TASK_CHECK(run_plan(&p, &rep) == 0);
    TASK_CHECK(rep.created == 2);
    TASK_CHECK(rep.finalized == 2);
    TASK_CHECK(rep.exceptions == 0);
    return 0;
}

/* A normally rejected assignment still consumes the argument root, but the
 * duplicate alias keeps the identity alive until it is dropped too. */
static int test_reject_alias_survives(void)
{
    qjs_ownership_plan p;
    struct qjs_ownership_report rep;
    qjs_ownership_model m;

    plan_init(&p);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_NEW, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_DUP, 0, 1) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_REJECT_TAKE, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_GC, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_CATCH, 2, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_DROP, 1, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_DROP, 2, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_GC, 0, 0) == 0);

    TASK_CHECK(run_plan(&p, &rep) == 0);
    TASK_CHECK(rep.created == 1);
    TASK_CHECK(rep.finalized == 1);
    TASK_CHECK(rep.exceptions == 1);

    qjs_ownership_model_init(&m);
    TASK_CHECK(qjs_ownership_model_apply(&m, &p.steps[0]) == 0);
    TASK_CHECK(qjs_ownership_model_apply(&m, &p.steps[1]) == 0);
    TASK_CHECK(qjs_ownership_model_apply(&m, &p.steps[2]) == 0);
    /* The rejected root is gone but the alias keeps identity 0 alive. */
    TASK_CHECK(m.slot_id[0] == QJS_OWN_EMPTY);
    TASK_CHECK(m.slot_id[1] == 0);
    TASK_CHECK(qjs_ownership_model_alive(&m, 0));
    TASK_CHECK(m.pending_id == QJS_OWN_UNTRACKED);
    /* The caught TypeError is an untracked root, never a new identity. */
    TASK_CHECK(qjs_ownership_model_apply(&m, &p.steps[4]) == 0);
    TASK_CHECK(m.slot_id[2] == QJS_OWN_UNTRACKED);
    TASK_CHECK(m.next_id == 1);
    return 0;
}

/* ---------- exceptions group ---------- */

/* Two consecutive throws: the second JS_Throw replaces and releases the
 * first pending exception. */
static int test_two_throws(void)
{
    qjs_ownership_plan p;
    struct qjs_ownership_report rep;

    plan_init(&p);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_NEW, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_NEW, 1, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_THROW_TAKE, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_THROW_TAKE, 1, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_GC, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_CATCH, 2, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_DROP, 2, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_GC, 0, 0) == 0);

    TASK_CHECK(run_plan(&p, &rep) == 0);
    TASK_CHECK(rep.created == 2);
    TASK_CHECK(rep.finalized == 2);
    TASK_CHECK(rep.exceptions == 2);
    return 0;
}

/* An exception can be retrieved and re-thrown; ownership transfers each
 * time without an early finalization. */
static int test_catch_rethrow(void)
{
    qjs_ownership_plan p;
    struct qjs_ownership_report rep;

    plan_init(&p);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_NEW, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_THROW_TAKE, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_CATCH, 1, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_THROW_TAKE, 1, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_CATCH, 2, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_DROP, 2, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_GC, 0, 0) == 0);

    TASK_CHECK(run_plan(&p, &rep) == 0);
    TASK_CHECK(rep.created == 1);
    TASK_CHECK(rep.finalized == 1);
    TASK_CHECK(rep.exceptions == 2);
    return 0;
}

/* The native callback duplicates a borrowed argument before throwing: the
 * caller's root remains valid and the identity stays alive. */
static int test_callback_borrow(void)
{
    qjs_ownership_plan p;
    struct qjs_ownership_report rep;
    qjs_ownership_model m;

    plan_init(&p);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_NEW, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_CALL_THROW, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_GC, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_CATCH, 1, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_DROP, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_GC, 0, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_DROP, 1, 0) == 0);
    TASK_CHECK(plan_add(&p, QJS_OWN_OP_GC, 0, 0) == 0);

    TASK_CHECK(run_plan(&p, &rep) == 0);
    TASK_CHECK(rep.created == 1);
    TASK_CHECK(rep.finalized == 1);
    TASK_CHECK(rep.exceptions == 1);

    qjs_ownership_model_init(&m);
    TASK_CHECK(qjs_ownership_model_apply(&m, &p.steps[0]) == 0);
    TASK_CHECK(qjs_ownership_model_apply(&m, &p.steps[1]) == 0);
    /* Root and pending each hold one responsibility for the same identity. */
    TASK_CHECK(m.slot_id[0] == 0);
    TASK_CHECK(m.pending_id == 0);
    TASK_CHECK(qjs_ownership_model_resp(&m, 0, QJS_OWN_RESP_ROOT) == 1);
    TASK_CHECK(qjs_ownership_model_resp(&m, 0, QJS_OWN_RESP_PENDING) == 1);
    TASK_CHECK(qjs_ownership_model_alive(&m, 0));
    return 0;
}

/* ---------- replay group ---------- */

/* Exactly the plan consumed by task_support/api_consumer.c. */
static int test_api_consumer_plan(void)
{
    static const uint8_t plan[] = {
        0, 0, 0, 1, 0, 1, 3, 0, 0, 4, 2, 0,
        2, 1, 0, 2, 2, 0, 5, 0, 0,
    };
    struct qjs_ownership_report r1, r2;

    TASK_CHECK(qjs_ownership_run(plan, sizeof(plan), &r1) == 0);
    TASK_CHECK(r1.steps == 7);
    TASK_CHECK(r1.created == 1);
    TASK_CHECK(r1.finalized == 1);
    TASK_CHECK(r1.comparisons > 0);
    TASK_CHECK(r1.exceptions == 1);

    /* Re-running the same bytes is deterministic across fresh runtimes. */
    TASK_CHECK(qjs_ownership_run(plan, sizeof(plan), &r2) == 0);
    TASK_CHECK(memcmp(&r1, &r2, sizeof(r1)) == 0);
    return 0;
}

/* The empty plan is valid and reports all zeros. */
static int test_empty_plan(void)
{
    struct qjs_ownership_report rep;
    qjs_ownership_plan plan;

    TASK_CHECK(qjs_ownership_run((const uint8_t *)"", 0, &rep) == 0);
    TASK_CHECK(rep.steps == 0 && rep.created == 0 && rep.finalized == 0 &&
               rep.comparisons == 0 && rep.exceptions == 0);

    plan_init(&plan);
    TASK_CHECK(qjs_ownership_validate((const uint8_t *)"", 0, &plan) == 0);
    TASK_CHECK(plan.count == 0);
    return 0;
}

/* Truncated and illegal plans are rejected as a whole before the engine
 * runs. */
static int test_invalid_plans_rejected(void)
{
    static const uint8_t truncated1[] = {0};
    static const uint8_t truncated2[] = {0, 1};
    static const uint8_t truncated4[] = {0, 0, 0, 1};
    static const uint8_t slot_overwrite[] = {0, 0, 0, 0, 0, 0};
    static const uint8_t catch_without_pending[] = {4, 0, 0};
    static const uint8_t unknown_op[] = {10, 0, 0};
    static const uint8_t bad_gc_operand[] = {5, 1, 0};
    static const uint8_t drop_empty[] = {2, 0, 0};
    static const uint8_t store_untracked[] = {
        0, 0, 0, 8, 0, 0, 4, 1, 0, 6, 1, 0,
    };
    uint8_t oversized[QJS_OWN_PLAN_MAX_SIZE + 3];
    struct qjs_ownership_report rep;

    TASK_CHECK(qjs_ownership_run(truncated1, sizeof(truncated1), &rep) == 1);
    TASK_CHECK(qjs_ownership_run(truncated2, sizeof(truncated2), &rep) == 1);
    TASK_CHECK(qjs_ownership_run(truncated4, sizeof(truncated4), &rep) == 1);
    TASK_CHECK(qjs_ownership_run(slot_overwrite, sizeof(slot_overwrite),
                                 &rep) == 1);
    TASK_CHECK(qjs_ownership_run(catch_without_pending,
                                 sizeof(catch_without_pending), &rep) == 1);
    TASK_CHECK(qjs_ownership_run(unknown_op, sizeof(unknown_op), &rep) == 1);
    TASK_CHECK(qjs_ownership_run(bad_gc_operand, sizeof(bad_gc_operand),
                                 &rep) == 1);
    TASK_CHECK(qjs_ownership_run(drop_empty, sizeof(drop_empty), &rep) == 1);
    TASK_CHECK(qjs_ownership_run(store_untracked, sizeof(store_untracked),
                                 &rep) == 1);
    memset(oversized, 0, sizeof(oversized));
    TASK_CHECK(qjs_ownership_run(oversized, sizeof(oversized), &rep) == 1);
    return 0;
}

/* Deterministic LCG generator. A candidate step is only appended when the
 * model accepts it, so every generated plan is valid by construction. */
static uint32_t lcg_state;

static uint32_t lcg_next(void)
{
    lcg_state = lcg_state * 1103515245u + 12345u;
    return lcg_state;
}

static int first_empty_slot(const qjs_ownership_model *m)
{
    size_t i;
    for (i = 0; i < QJS_OWN_ROOT_COUNT; i++) {
        if (m->slot_id[i] == QJS_OWN_EMPTY)
            return (int)i;
    }
    return -1;
}

static int pick_occupied(const qjs_ownership_model *m, int tracked_only)
{
    size_t i;
    int candidates[QJS_OWN_ROOT_COUNT];
    int n = 0;
    for (i = 0; i < QJS_OWN_ROOT_COUNT; i++) {
        if (m->slot_id[i] == QJS_OWN_EMPTY)
            continue;
        if (tracked_only && m->slot_id[i] == QJS_OWN_UNTRACKED)
            continue;
        candidates[n++] = (int)i;
    }
    if (n == 0)
        return -1;
    return candidates[lcg_next() % (uint32_t)n];
}

static int build_generated_plan(qjs_ownership_plan *p, uint32_t seed)
{
    qjs_ownership_model m;
    size_t target;
    size_t attempts;

    lcg_state = seed ? seed : 1u;
    qjs_ownership_model_init(&m);
    plan_init(p);
    target = 6u + (lcg_next() % 43u); /* 6..48 records */

    while (p->count < target) {
        qjs_ownership_step cand;
        int slot, dest;
        uint32_t choice = lcg_next() % 10u;

        memset(&cand, 0, sizeof(cand));
        switch (choice) {
        case 0: /* NEW */
            if (m.next_id >= QJS_OWN_MAX_IDS)
                cand.op = QJS_OWN_OP_GC;
            else {
                dest = first_empty_slot(&m);
                if (dest < 0) {
                    cand.op = QJS_OWN_OP_GC;
                } else {
                    cand.op = QJS_OWN_OP_NEW;
                    cand.a = (uint8_t)dest;
                }
            }
            break;
        case 1: /* DUP */
            slot = pick_occupied(&m, 0);
            dest = first_empty_slot(&m);
            if (slot < 0 || dest < 0) {
                cand.op = QJS_OWN_OP_GC;
            } else {
                cand.op = QJS_OWN_OP_DUP;
                cand.a = (uint8_t)slot;
                cand.b = (uint8_t)dest;
            }
            break;
        case 2: /* DROP */
            slot = pick_occupied(&m, 0);
            if (slot < 0) {
                cand.op = QJS_OWN_OP_GC;
            } else {
                cand.op = QJS_OWN_OP_DROP;
                cand.a = (uint8_t)slot;
            }
            break;
        case 3: /* THROW_TAKE */
            slot = pick_occupied(&m, 0);
            if (slot < 0) {
                cand.op = QJS_OWN_OP_GC;
            } else {
                cand.op = QJS_OWN_OP_THROW_TAKE;
                cand.a = (uint8_t)slot;
            }
            break;
        case 4: /* CATCH */
            dest = first_empty_slot(&m);
            if (m.pending_id == QJS_OWN_EMPTY || dest < 0) {
                cand.op = QJS_OWN_OP_GC;
            } else {
                cand.op = QJS_OWN_OP_CATCH;
                cand.a = (uint8_t)dest;
            }
            break;
        case 5: /* GC */
            cand.op = QJS_OWN_OP_GC;
            break;
        case 6: /* STORE_TAKE */
            slot = pick_occupied(&m, 1);
            if (slot < 0) {
                cand.op = QJS_OWN_OP_GC;
            } else {
                cand.op = QJS_OWN_OP_STORE_TAKE;
                cand.a = (uint8_t)slot;
            }
            break;
        case 7: /* CLEAR */
            if (m.holder_id == QJS_OWN_EMPTY) {
                cand.op = QJS_OWN_OP_GC;
            } else {
                cand.op = QJS_OWN_OP_CLEAR;
            }
            break;
        case 8: /* REJECT_TAKE */
            slot = pick_occupied(&m, 1);
            if (slot < 0) {
                cand.op = QJS_OWN_OP_GC;
            } else {
                cand.op = QJS_OWN_OP_REJECT_TAKE;
                cand.a = (uint8_t)slot;
            }
            break;
        default: /* CALL_THROW */
            slot = pick_occupied(&m, 1);
            if (slot < 0) {
                cand.op = QJS_OWN_OP_GC;
            } else {
                cand.op = QJS_OWN_OP_CALL_THROW;
                cand.a = (uint8_t)slot;
            }
            break;
        }

        /* A failed apply leaves the model untouched; retry a bounded number
         * of times so generation always terminates. */
        attempts = 0;
        while (qjs_ownership_model_apply(&m, &cand) != 0) {
            if (++attempts > 8) {
                cand.op = QJS_OWN_OP_GC;
                cand.a = 0;
                cand.b = 0;
                TASK_CHECK(qjs_ownership_model_apply(&m, &cand) == 0);
            }
        }
        TASK_CHECK(plan_add(p, cand.op, cand.a, cand.b) == 0);
    }
    return 0;
}

static int test_generated_plans(void)
{
    enum { GEN_COUNT = 256 };
    int k;

    for (k = 0; k < GEN_COUNT; k++) {
        qjs_ownership_plan p;
        struct qjs_ownership_report rep;
        uint8_t buf[QJS_OWN_PLAN_MAX_SIZE];
        size_t n;

        TASK_CHECK(build_generated_plan(&p, (uint32_t)k + 1u) == 0);
        TASK_CHECK(p.count >= 6);        /* The generator output must pass the standalone whole-plan
         * validator as well. */
        n = plan_serialize(&p, buf, sizeof(buf));
        TASK_CHECK(n == p.count * 3u);
        {
            qjs_ownership_plan reparsed;
            TASK_CHECK(qjs_ownership_validate(buf, n, &reparsed) == 0);
            TASK_CHECK(reparsed.count == p.count);
        }
        /* Each plan runs twice in separate runtimes inside the chain. */
        TASK_CHECK(qjs_ownership_run(buf, n, &rep) == 0);
        TASK_CHECK(rep.steps == p.count);
        TASK_CHECK(rep.created == rep.finalized);
        TASK_CHECK(rep.comparisons > 0);
    }
    return 0;
}

/* ---------- diagnostics group ---------- */

static int test_mismatch_format(void)
{
    char buf[256];

    qjs_ownership_format_mismatch(buf, sizeof(buf), 7, QJS_OWN_OP_DROP, 3,
                                  qjs_ownership_resp_name(QJS_OWN_RESP_ROOT),
                                  1, 0);
    TASK_CHECK(strstr(buf, "step 7") != NULL);
    TASK_CHECK(strstr(buf, "op=DROP") != NULL);
    TASK_CHECK(strstr(buf, "id=3") != NULL);
    TASK_CHECK(strstr(buf, "category=root") != NULL);
    TASK_CHECK(strstr(buf, "expected=1") != NULL);
    TASK_CHECK(strstr(buf, "actual=0") != NULL);
    return 0;
}

/* Identities are monotonic and never reused after finalization. */
static int test_identity_never_reused(void)
{
    qjs_ownership_model m;
    qjs_ownership_step s;

    qjs_ownership_model_init(&m);
    memset(&s, 0, sizeof(s));

    s.op = QJS_OWN_OP_NEW; s.a = 0;
    TASK_CHECK(qjs_ownership_model_apply(&m, &s) == 0);
    s.op = QJS_OWN_OP_DROP; s.a = 0;
    TASK_CHECK(qjs_ownership_model_apply(&m, &s) == 0);
    TASK_CHECK(qjs_ownership_model_finalized(&m, 0));
    TASK_CHECK(!qjs_ownership_model_alive(&m, 0));

    s.op = QJS_OWN_OP_NEW; s.a = 0;
    TASK_CHECK(qjs_ownership_model_apply(&m, &s) == 0);
    TASK_CHECK(m.slot_id[0] == 1);
    TASK_CHECK(m.next_id == 2);
    TASK_CHECK(qjs_ownership_model_finalized(&m, 0));
    TASK_CHECK(qjs_ownership_model_alive(&m, 1));

    {
        int set[QJS_OWN_MAX_IDS];
        TASK_CHECK(qjs_ownership_model_finalized_set(&m, set) == 1);
        TASK_CHECK(set[0] && !set[1]);
    }
    return 0;
}

static int test_naming_helpers(void)
{
    TASK_CHECK(strcmp(qjs_ownership_resp_name(QJS_OWN_RESP_ROOT),
                      "root") == 0);
    TASK_CHECK(strcmp(qjs_ownership_resp_name(QJS_OWN_RESP_HOLDER),
                      "holder") == 0);
    TASK_CHECK(strcmp(qjs_ownership_resp_name(QJS_OWN_RESP_PENDING),
                      "pending") == 0);
    TASK_CHECK(strcmp(qjs_ownership_op_name(QJS_OWN_OP_STORE_TAKE),
                      "STORE_TAKE") == 0);
    TASK_CHECK(strcmp(qjs_ownership_op_name(QJS_OWN_OP_REJECT_TAKE),
                      "REJECT_TAKE") == 0);
    TASK_CHECK(strcmp(qjs_ownership_op_name(QJS_OWN_OP_CALL_THROW),
                      "CALL_THROW") == 0);
    return 0;
}

/* ---------- registration ---------- */

const struct task_case task_cases[] = {
    {"transfer", "alias_release", test_alias_release},
    {"transfer", "holder_replace", test_holder_replace},
    {"transfer", "reject_alias_survives", test_reject_alias_survives},

    {"exceptions", "two_throws", test_two_throws},
    {"exceptions", "catch_rethrow", test_catch_rethrow},
    {"exceptions", "callback_borrow", test_callback_borrow},

    {"replay", "api_consumer_plan", test_api_consumer_plan},
    {"replay", "empty_plan", test_empty_plan},
    {"replay", "invalid_plans_rejected", test_invalid_plans_rejected},
    {"replay", "generated_plans_256", test_generated_plans},

    {"diagnostics", "mismatch_format", test_mismatch_format},
    {"diagnostics", "identity_never_reused", test_identity_never_reused},
    {"diagnostics", "naming_helpers", test_naming_helpers},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
