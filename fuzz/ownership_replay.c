/*
 * Ownership replay executor.
 *
 * Drives the real QuickJS engine through a validated plan using only the
 * public C API. It never reads internal reference counts and never feeds
 * the engine's actual finalization back into the expected model: the model
 * in ownership_model.c is the specification, and engine observations are
 * compared against it identity by identity.
 *
 * Every plan is replayed twice, each time in a brand new runtime/context;
 * the stepwise responsibility and finalization traces of the two runs must
 * match. The deterministic driver and LLVMFuzzerTestOneInput share this
 * single replay chain.
 */
#include "quickjs.h"
#include "ownership_model.h"
#include "ownership_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OWN_CLASS_NAME "OwnershipTracked"
#define HOLDER_PROP "held"

/* Per-object logical identity cell. Stored through JS_SetOpaque; it carries
 * no JSValue and therefore adds no engine-side reference. */
struct own_cell {
    int id;
    int finalized;
};

/* Reached from the finalizer through the runtime opaque. */
struct own_observer {
    JSClassID cid;
    struct own_cell cells[QJS_OWN_MAX_IDS];
    int double_finalize;
    size_t finalized_count;
};

struct own_trace_entry {
    uint64_t alive_mask;       /* identities the model considers alive */
    uint64_t finalized_mask;   /* identities the engine has finalized */
    size_t exceptions;         /* pending-exception entries so far */
};

struct own_run_trace {
    size_t count;
    size_t comparisons;
    size_t created;
    size_t finalized;
    size_t exceptions;
    struct own_trace_entry entries[QJS_OWN_MAX_STEPS];
};

static void own_finalizer(JSRuntime *rt, JSValueConst val)
{
    struct own_observer *o = JS_GetRuntimeOpaque(rt);
    struct own_cell *cell;

    if (!o)
        return;
    cell = JS_GetOpaque(val, o->cid);
    if (!cell)
        return;
    /* A repeated finalizer call is a real engine defect: record it instead
     * of terminating the process. */
    if (cell->finalized) {
        o->double_finalize = 1;
        return;
    }
    cell->finalized = 1;
    o->finalized_count++;
}

/* Native callback used by CALL_THROW. The argument is borrowed, so it is
 * duplicated before JS_Throw transfers ownership of the duplicate. */
static JSValue own_borrow_throw(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue copy;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ownership callback: missing argument");
    copy = JS_DupValue(ctx, argv[0]);
    return JS_Throw(ctx, copy);
}

static uint64_t own_id_mask(unsigned int id)
{
    return (uint64_t)1 << id;
}

/* Index of the lowest set bit, or -1 for a zero mask. Portable C11. */
static int own_first_set_bit(uint64_t mask)
{
    unsigned int i;
    if (mask == 0)
        return -1;
    for (i = 0; i < 64u; i++) {
        if (mask & ((uint64_t)1 << i))
            return (int)i;
    }
    return -1;
}

/* Every identity in [0, created): the set that mandatory cleanup must have
 * finalized by the time the runtime is torn down. */
static uint64_t own_all_created_mask(size_t created)
{
    if (created >= 64)
        return ~(uint64_t)0;
    if (created == 0)
        return 0;
    return ((uint64_t)1 << created) - 1u;
}

static uint64_t own_model_finalized_mask(const qjs_ownership_model *m)
{
    int set[QJS_OWN_MAX_IDS];
    size_t i;
    uint64_t mask = 0;

    qjs_ownership_model_finalized_set(m, set);
    for (i = 0; i < QJS_OWN_MAX_IDS; i++) {
        if (set[i])
            mask |= own_id_mask((unsigned int)i);
    }
    return mask;
}

static uint64_t own_model_alive_mask(const qjs_ownership_model *m)
{
    size_t i;
    uint64_t mask = 0;

    for (i = 0; i < m->next_id; i++) {
        if (qjs_ownership_model_alive(m, (unsigned int)i))
            mask |= own_id_mask((unsigned int)i);
    }
    return mask;
}

static uint64_t own_engine_finalized_mask(const struct own_observer *o)
{
    size_t i;
    uint64_t mask = 0;

    for (i = 0; i < QJS_OWN_MAX_IDS; i++) {
        if (o->cells[i].finalized)
            mask |= own_id_mask((unsigned int)i);
    }
    return mask;
}

struct own_engine {
    JSRuntime *rt;
    JSContext *ctx;
    JSClassID cid;
    struct own_observer *observer;
    JSValue holder;          /* writable "held" property */
    JSValue reject_holder;   /* read-only "held" property */
    JSValue callback;        /* native borrow-and-throw function */
    JSValue slot[QJS_OWN_ROOT_COUNT];
    int occupied[QJS_OWN_ROOT_COUNT];
};

#define OWN_FAIL_BUF_SIZE 256

/* One full replay in a fresh runtime. Returns 0 on success, 2 on the first
 * model/engine divergence (err filled), never terminates the process. */
static int own_run_once(const qjs_ownership_plan *plan,
                        struct own_run_trace *trace,
                        struct qjs_ownership_report *rep,
                        char *err, size_t err_size)
{
    struct own_engine e;
    qjs_ownership_model model;
    size_t i;
    int rc = 2;
    size_t created_index = 0;
    size_t exceptions_observed = 0;
    uint64_t engine_mask, expected_final, expected_alive;

    memset(&e, 0, sizeof(e));
    memset(trace, 0, sizeof(*trace));
    qjs_ownership_model_init(&model);

    e.rt = JS_NewRuntime();
    if (!e.rt) {
        snprintf(err, err_size, "JS_NewRuntime failed");
        return 2;
    }
    e.observer = calloc(1, sizeof(*e.observer));
    if (!e.observer) {
        snprintf(err, err_size, "observer allocation failed");
        JS_FreeRuntime(e.rt);
        return 2;
    }
    JS_SetRuntimeOpaque(e.rt, e.observer);

    e.ctx = JS_NewContext(e.rt);
    if (!e.ctx) {
        snprintf(err, err_size, "JS_NewContext failed");
        free(e.observer);
        JS_FreeRuntime(e.rt);
        return 2;
    }

    e.cid = 0;
    JS_NewClassID(e.rt, &e.cid);
    {
        JSClassDef def;
        memset(&def, 0, sizeof(def));
        def.class_name = OWN_CLASS_NAME;
        def.finalizer = own_finalizer;
        if (JS_NewClass(e.rt, e.cid, &def) != 0) {
            snprintf(err, err_size, "JS_NewClass failed");
            goto fail_context;
        }
    }
    e.observer->cid = e.cid;

    e.holder = JS_NewObject(e.ctx);
    e.reject_holder = JS_NewObject(e.ctx);
    e.callback = JS_NewCFunction(e.ctx, own_borrow_throw,
                                 "ownership_borrow_throw", 1);
    if (!JS_IsObject(e.holder) || !JS_IsObject(e.reject_holder) ||
        !JS_IsObject(e.callback)) {
        snprintf(err, err_size, "fixture object creation failed");
        goto fail_context;
    }
    /* Writable holder property. */
    if (JS_DefinePropertyValueStr(e.ctx, e.holder, HOLDER_PROP, JS_UNDEFINED,
                                  JS_PROP_C_W_E) < 0) {
        snprintf(err, err_size, "holder property setup failed");
        goto fail_context;
    }
    /* Pre-existing non-writable property: assignment is normally rejected
     * with a TypeError while still consuming the argument value. */
    if (JS_DefinePropertyValueStr(e.ctx, e.reject_holder, HOLDER_PROP,
                                  JS_UNDEFINED,
                                  JS_PROP_CONFIGURABLE |
                                  JS_PROP_ENUMERABLE) < 0) {
        snprintf(err, err_size, "reject holder property setup failed");
        goto fail_context;
    }

    for (i = 0; i < plan->count; i++) {
        const qjs_ownership_step *st = &plan->steps[i];
        int set_rc;

        /* The plan was pre-validated, so applying it to the model cannot
         * fail; treat any failure as an internal divergence. */
        set_rc = qjs_ownership_model_apply(&model, st);
        if (set_rc != 0) {
            qjs_ownership_format_mismatch(err, err_size, i, st->op, -1,
                                          "precondition", 0, 1);
            goto fail_context;
        }

        switch (st->op) {
        case QJS_OWN_OP_NEW: {
            JSValue v = JS_NewObjectClass(e.ctx, e.cid);
            struct own_cell *cell;
            if (JS_IsException(v) || !JS_IsObject(v)) {
                qjs_ownership_format_mismatch(err, err_size, i, st->op,
                                              (int)created_index,
                                              "new_object", 1, 0);
                goto fail_context;
            }
            if (created_index >= QJS_OWN_MAX_IDS ||
                created_index != model.next_id - 1) {
                qjs_ownership_format_mismatch(err, err_size, i, st->op,
                                              (int)created_index,
                                              "identity_allocation",
                                              (int)(model.next_id - 1),
                                              (int)created_index);
                JS_FreeValue(e.ctx, v);
                goto fail_context;
            }
            cell = &e.observer->cells[created_index];
            cell->id = (int)created_index;
            JS_SetOpaque(v, cell);
            e.slot[st->a] = v;
            e.occupied[st->a] = 1;
            created_index++;
            break;
        }
        case QJS_OWN_OP_DUP:
            e.slot[st->b] = JS_DupValue(e.ctx, e.slot[st->a]);
            e.occupied[st->b] = 1;
            break;
        case QJS_OWN_OP_DROP:
            JS_FreeValue(e.ctx, e.slot[st->a]);
            e.slot[st->a] = JS_UNDEFINED;
            e.occupied[st->a] = 0;
            break;
        case QJS_OWN_OP_THROW_TAKE: {
            JSValue transferred = e.slot[st->a];
            JSValue thrown;
            e.slot[st->a] = JS_UNDEFINED;
            e.occupied[st->a] = 0;
            thrown = JS_Throw(e.ctx, transferred); /* consumes transferred */
            if (!JS_IsException(thrown) || !JS_HasException(e.ctx)) {
                qjs_ownership_format_mismatch(err, err_size, i, st->op, -1,
                                              "throw_pending", 1, 0);
                goto fail_context;
            }
            exceptions_observed++;
            break;
        }
        case QJS_OWN_OP_CATCH:
            e.slot[st->a] = JS_GetException(e.ctx);
            e.occupied[st->a] = 1;
            if (JS_HasException(e.ctx)) {
                qjs_ownership_format_mismatch(err, err_size, i, st->op, -1,
                                              "pending_cleared", 1, 0);
                goto fail_context;
            }
            break;
        case QJS_OWN_OP_GC:
            JS_RunGC(e.rt);
            engine_mask = own_engine_finalized_mask(e.observer);
            expected_final = own_model_finalized_mask(&model);
            trace->comparisons++;
            if (engine_mask != expected_final) {
                int id;
                uint64_t diff = engine_mask ^ expected_final;
                id = own_first_set_bit(diff);
                qjs_ownership_format_mismatch(
                    err, err_size, i, st->op, id, "gc_finalize_set",
                    (int)((expected_final >> (id < 0 ? 0 : id)) & 1),
                    (int)((engine_mask >> (id < 0 ? 0 : id)) & 1));
                goto fail_context;
            }
            break;
        case QJS_OWN_OP_STORE_TAKE: {
            JSValue transferred = e.slot[st->a];
            int r;
            e.slot[st->a] = JS_UNDEFINED;
            e.occupied[st->a] = 0;
            r = JS_SetPropertyStr(e.ctx, e.holder, HOLDER_PROP, transferred);
            if (r < 0) {
                qjs_ownership_format_mismatch(err, err_size, i, st->op, -1,
                                              "store_accepted", 1, 0);
                goto fail_context;
            }
            break;
        }
        case QJS_OWN_OP_CLEAR: {
            int r = JS_SetPropertyStr(e.ctx, e.holder, HOLDER_PROP,
                                      JS_UNDEFINED);
            if (r < 0) {
                qjs_ownership_format_mismatch(err, err_size, i, st->op, -1,
                                              "clear_accepted", 1, 0);
                goto fail_context;
            }
            break;
        }
        case QJS_OWN_OP_REJECT_TAKE: {
            JSValue transferred = e.slot[st->a];
            int r;
            e.slot[st->a] = JS_UNDEFINED;
            e.occupied[st->a] = 0;
            r = JS_SetPropertyStr(e.ctx, e.reject_holder, HOLDER_PROP,
                                  transferred);
            /* Normal rejection: TypeError pending, argument consumed. */
            if (r >= 0 || !JS_HasException(e.ctx)) {
                qjs_ownership_format_mismatch(err, err_size, i, st->op, -1,
                                              "reject_pending", 1, 0);
                goto fail_context;
            }
            exceptions_observed++;
            break;
        }
        case QJS_OWN_OP_CALL_THROW: {
            JSValueConst argv[1];
            JSValue ret;
            argv[0] = e.slot[st->a]; /* borrowed: the root keeps its reference */
            ret = JS_Call(e.ctx, e.callback, JS_UNDEFINED, 1, argv);
            if (!JS_IsException(ret) || !JS_HasException(e.ctx)) {
                qjs_ownership_format_mismatch(err, err_size, i, st->op, -1,
                                              "callback_pending", 1, 0);
                goto fail_context;
            }
            if (!e.occupied[st->a]) {
                qjs_ownership_format_mismatch(err, err_size, i, st->op, -1,
                                              "borrowed_root_preserved", 1, 0);
                goto fail_context;
            }
            exceptions_observed++;
            break;
        }
        default:
            qjs_ownership_format_mismatch(err, err_size, i, st->op, -1,
                                          "unknown_op", 0, 1);
            goto fail_context;
        }

        /* Per-step liveness: nothing the model still holds may have been
         * finalized, and the exception counter must match the model. */
        engine_mask = own_engine_finalized_mask(e.observer);
        expected_alive = own_model_alive_mask(&model);
        trace->comparisons++;
        if (engine_mask & expected_alive) {
            uint64_t diff = engine_mask & expected_alive;
            int id = own_first_set_bit(diff);
            qjs_ownership_format_mismatch(err, err_size, i, st->op, id,
                                          "alive_not_finalized", 0, 1);
            goto fail_context;
        }
        trace->comparisons++;
        if (exceptions_observed != model.exceptions) {
            qjs_ownership_format_mismatch(err, err_size, i, st->op, -1,
                                          "exception_count",
                                          (int)model.exceptions,
                                          (int)exceptions_observed);
            goto fail_context;
        }

        trace->entries[trace->count].alive_mask = expected_alive;
        trace->entries[trace->count].finalized_mask = engine_mask;
        trace->entries[trace->count].exceptions = exceptions_observed;
        trace->count++;
    }

    /* Mandatory end-of-run cleanup: all roots, the holder responsibility
     * and any unretrieved pending exception are released explicitly. */
    for (i = 0; i < QJS_OWN_ROOT_COUNT; i++) {
        if (e.occupied[i]) {
            JS_FreeValue(e.ctx, e.slot[i]);
            e.slot[i] = JS_UNDEFINED;
            e.occupied[i] = 0;
        }
    }
    if (JS_HasException(e.ctx)) {
        JSValue pending = JS_GetException(e.ctx);
        JS_FreeValue(e.ctx, pending);
    }
    /* Releases the holder responsibility (held -> undefined). */
    if (JS_SetPropertyStr(e.ctx, e.holder, HOLDER_PROP, JS_UNDEFINED) < 0) {
        snprintf(err, err_size, "final holder clear failed");
        goto fail_context;
    }

    JS_RunGC(e.rt);
    engine_mask = own_engine_finalized_mask(e.observer);
    expected_final = own_all_created_mask(created_index);
    if (created_index > 0)
        trace->comparisons++;
    if (engine_mask != expected_final) {
        uint64_t diff = engine_mask ^ expected_final;
        int id = own_first_set_bit(diff);
        qjs_ownership_format_mismatch(err, err_size, plan->count, 0, id,
                                      "cleanup_finalize_set",
                                      (int)((expected_final >>
                                             (id < 0 ? 0 : id)) &
                                            1),
                                      (int)((engine_mask >>
                                             (id < 0 ? 0 : id)) &
                                            1));
        goto fail_context;
    }

    JS_FreeValue(e.ctx, e.callback);
    JS_FreeValue(e.ctx, e.reject_holder);
    JS_FreeValue(e.ctx, e.holder);
    JS_FreeContext(e.ctx);
    JS_FreeRuntime(e.rt);

    /* FreeRuntime must not have finalized a tracked object twice. */
    if (e.observer->double_finalize) {
        snprintf(err, err_size,
                 "double finalization observed during runtime teardown");
        free(e.observer);
        return 2;
    }
    if (e.observer->finalized_count != created_index) {
        snprintf(err, err_size,
                 "finalization count changed during runtime teardown");
        free(e.observer);
        return 2;
    }

    trace->created = created_index;
    trace->finalized = e.observer->finalized_count;
    trace->exceptions = exceptions_observed;
    free(e.observer);

    rep->steps = plan->count;
    rep->created = trace->created;
    rep->finalized = trace->finalized;
    rep->comparisons = trace->comparisons;
    rep->exceptions = trace->exceptions;
    return 0;

fail_context:
    /* Best-effort cleanup on divergence; never abort. */
    for (i = 0; i < QJS_OWN_ROOT_COUNT; i++) {
        if (e.occupied[i])
            JS_FreeValue(e.ctx, e.slot[i]);
    }
    if (JS_HasException(e.ctx)) {
        JSValue pending = JS_GetException(e.ctx);
        JS_FreeValue(e.ctx, pending);
    }
    JS_FreeValue(e.ctx, e.callback);
    JS_FreeValue(e.ctx, e.reject_holder);
    JS_FreeValue(e.ctx, e.holder);
    JS_FreeContext(e.ctx);
    free(e.observer);
    JS_FreeRuntime(e.rt);
    return rc;
}

int qjs_ownership_run(const uint8_t *data, size_t size,
                      struct qjs_ownership_report *out)
{
    qjs_ownership_plan plan;
    struct own_run_trace *trace1, *trace2;
    struct qjs_ownership_report rep1, rep2;
    char err[OWN_FAIL_BUF_SIZE];
    size_t i;
    int rc;

    if (out)
        memset(out, 0, sizeof(*out));

    /* Whole-plan validation on logical state only, before any engine call. */
    if (qjs_ownership_validate(data, size, &plan) != 0)
        return 1;

    trace1 = calloc(1, sizeof(*trace1));
    trace2 = calloc(1, sizeof(*trace2));
    if (!trace1 || !trace2) {
        free(trace1);
        free(trace2);
        return 1;
    }

    err[0] = '\0';
    rc = own_run_once(&plan, trace1, &rep1, err, sizeof(err));
    if (rc == 0) {
        char err2[OWN_FAIL_BUF_SIZE];
        err2[0] = '\0';
        /* Second replay in a brand new runtime: traces must match
         * step-for-step, finalization order ignored by construction. */
        rc = own_run_once(&plan, trace2, &rep2, err2, sizeof(err2));
        if (rc == 0) {
            if (trace1->count != trace2->count ||
                trace1->created != trace2->created ||
                trace1->finalized != trace2->finalized ||
                trace1->exceptions != trace2->exceptions) {
                fprintf(stderr,
                        "ownership mismatch cross-run summary id=-1 "
                        "category=trace expected=%zu actual=%zu\n",
                        trace1->count, trace2->count);
                rc = 2;
            } else {
                for (i = 0; i < trace1->count; i++) {
                    if (trace1->entries[i].alive_mask !=
                            trace2->entries[i].alive_mask ||
                        trace1->entries[i].finalized_mask !=
                            trace2->entries[i].finalized_mask ||
                        trace1->entries[i].exceptions !=
                            trace2->entries[i].exceptions) {
                        fprintf(stderr,
                                "ownership mismatch cross-run at step %zu "
                                "id=-1 category=step_trace\n",
                                i);
                        rc = 2;
                        break;
                    }
                }
            }
        } else {
            fputs(err2, stderr);
            fputc('\n', stderr);
        }
    } else {
        fputs(err, stderr);
        fputc('\n', stderr);
    }

    if (rc == 0 && out)
        *out = rep2;

    free(trace1);
    free(trace2);
    return rc;
}
