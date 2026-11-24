/*
 * Ownership model implementation.
 *
 * Pure C-library logic: no quickjs.h, no JSValue. The model is the
 * specification of who (root slot, fixed holder property, pending
 * exception) is responsible for every logical object identity at every
 * step. Identities are allocated monotonically and never reused.
 */
#include "ownership_model.h"

#include <stdio.h>
#include <string.h>

static int is_valid_slot(unsigned int v)
{
    return v < QJS_OWN_ROOT_COUNT;
}

void qjs_ownership_model_init(qjs_ownership_model *m)
{
    size_t i;
    memset(m, 0, sizeof(*m));
    for (i = 0; i < QJS_OWN_ROOT_COUNT; i++)
        m->slot_id[i] = QJS_OWN_EMPTY;
    m->holder_id = QJS_OWN_EMPTY;
    m->pending_id = QJS_OWN_EMPTY;
}

static void add_resp(qjs_ownership_model *m, unsigned int id,
                     qjs_ownership_resp cat)
{
    qjs_ownership_id_state *s = &m->ids[id];
    s->resp[cat]++;
    s->total_resp++;
    s->alive = 1;
}

/* Release one responsibility. When the last responsibility disappears the
 * identity becomes logically finalized; it can never be reactivated. */
static void release_resp(qjs_ownership_model *m, unsigned int id,
                         qjs_ownership_resp cat)
{
    qjs_ownership_id_state *s = &m->ids[id];
    s->resp[cat]--;
    s->total_resp--;
    if (s->total_resp == 0)
        s->alive = 0;
}

static void clear_pending(qjs_ownership_model *m)
{
    if (m->pending_id >= 0)
        release_resp(m, (unsigned int)m->pending_id, QJS_OWN_RESP_PENDING);
    m->pending_id = QJS_OWN_EMPTY;
}

int qjs_ownership_model_apply(qjs_ownership_model *m,
                              const qjs_ownership_step *step)
{
    unsigned int a = step->a, b = step->b;

    switch (step->op) {
    case QJS_OWN_OP_NEW:
        if (!is_valid_slot(a) || b != 0)
            return 1;
        if (m->slot_id[a] != QJS_OWN_EMPTY)
            return 1; /* no slot overwrites */
        if (m->next_id >= QJS_OWN_MAX_IDS)
            return 1;
        break;
    case QJS_OWN_OP_DUP:
        if (!is_valid_slot(a) || !is_valid_slot(b))
            return 1;
        if (m->slot_id[a] == QJS_OWN_EMPTY)
            return 1; /* cannot duplicate a released root */
        if (m->slot_id[b] != QJS_OWN_EMPTY)
            return 1;
        break;
    case QJS_OWN_OP_DROP:
    case QJS_OWN_OP_THROW_TAKE:
        /* Tracked identities and untracked caught exceptions may both be
         * dropped or re-thrown from a root. */
        if (!is_valid_slot(a) || b != 0)
            return 1;
        if (m->slot_id[a] == QJS_OWN_EMPTY)
            return 1; /* live references only; no stale roots */
        break;
    case QJS_OWN_OP_STORE_TAKE:
    case QJS_OWN_OP_REJECT_TAKE:
    case QJS_OWN_OP_CALL_THROW:
        if (!is_valid_slot(a) || b != 0)
            return 1;
        if (m->slot_id[a] == QJS_OWN_EMPTY)
            return 1; /* live references only; no stale roots */
        if (m->slot_id[a] == QJS_OWN_UNTRACKED)
            return 1; /* object actions need a tracked identity */
        break;
    case QJS_OWN_OP_CATCH:
        if (!is_valid_slot(a) || b != 0)
            return 1;
        if (m->pending_id == QJS_OWN_EMPTY)
            return 1; /* CATCH only with a pending exception */
        if (m->slot_id[a] != QJS_OWN_EMPTY)
            return 1;
        break;
    case QJS_OWN_OP_GC:
    case QJS_OWN_OP_CLEAR:
        if (a != 0 || b != 0)
            return 1;
        if (step->op == QJS_OWN_OP_CLEAR && m->holder_id == QJS_OWN_EMPTY)
            return 1;
        break;
    default:
        return 1;
    }

    switch (step->op) {
    case QJS_OWN_OP_NEW: {
        unsigned int id = m->next_id++;
        qjs_ownership_id_state *s = &m->ids[id];
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->tracked = 1;
        add_resp(m, id, QJS_OWN_RESP_ROOT);
        m->slot_id[a] = (int)id;
        m->created++;
        break;
    }
    case QJS_OWN_OP_DUP:
        if (m->slot_id[a] >= 0)
            add_resp(m, (unsigned int)m->slot_id[a], QJS_OWN_RESP_ROOT);
        m->slot_id[b] = m->slot_id[a]; /* tracked id or UNTRACKED */
        break;
    case QJS_OWN_OP_DROP:
        if (m->slot_id[a] >= 0)
            release_resp(m, (unsigned int)m->slot_id[a],
                         QJS_OWN_RESP_ROOT);
        m->slot_id[a] = QJS_OWN_EMPTY;
        break;
    case QJS_OWN_OP_THROW_TAKE:
        /* JS_Throw transfers the reference and replaces the old pending. */
        clear_pending(m);
        if (m->slot_id[a] >= 0) {
            int id = m->slot_id[a];
            release_resp(m, (unsigned int)id, QJS_OWN_RESP_ROOT);
            add_resp(m, (unsigned int)id, QJS_OWN_RESP_PENDING);
            m->pending_id = id;
        } else {
            m->pending_id = QJS_OWN_UNTRACKED;
        }
        m->slot_id[a] = QJS_OWN_EMPTY;
        m->exceptions++;
        break;
    case QJS_OWN_OP_CATCH:
        if (m->pending_id >= 0) {
            int id = m->pending_id;
            release_resp(m, (unsigned int)id, QJS_OWN_RESP_PENDING);
            add_resp(m, (unsigned int)id, QJS_OWN_RESP_ROOT);
            m->slot_id[a] = id;
        } else {
            m->slot_id[a] = QJS_OWN_UNTRACKED;
        }
        m->pending_id = QJS_OWN_EMPTY;
        break;
    case QJS_OWN_OP_GC:
        /* No ownership change; the executor compares finalize identities. */
        break;
    case QJS_OWN_OP_STORE_TAKE: {
        int id = m->slot_id[a];
        if (m->holder_id >= 0)
            release_resp(m, (unsigned int)m->holder_id,
                         QJS_OWN_RESP_HOLDER);
        release_resp(m, (unsigned int)id, QJS_OWN_RESP_ROOT);
        add_resp(m, (unsigned int)id, QJS_OWN_RESP_HOLDER);
        m->holder_id = id;
        m->slot_id[a] = QJS_OWN_EMPTY;
        break;
    }
    case QJS_OWN_OP_CLEAR:
        release_resp(m, (unsigned int)m->holder_id, QJS_OWN_RESP_HOLDER);
        m->holder_id = QJS_OWN_EMPTY;
        break;
    case QJS_OWN_OP_REJECT_TAKE: {
        int id = m->slot_id[a];
        /* The normal rejection still consumes the argument. */
        release_resp(m, (unsigned int)id, QJS_OWN_RESP_ROOT);
        m->slot_id[a] = QJS_OWN_EMPTY;
        /* The TypeError is an untracked exception and replaces pending. */
        clear_pending(m);
        m->pending_id = QJS_OWN_UNTRACKED;
        m->exceptions++;
        break;
    }
    case QJS_OWN_OP_CALL_THROW: {
        int id = m->slot_id[a];
        /* The borrowed argument stays in the root; the callback duplicates
         * it and JS_Throw transfers the duplicate to pending. */
        clear_pending(m);
        add_resp(m, (unsigned int)id, QJS_OWN_RESP_PENDING);
        m->pending_id = id;
        m->exceptions++;
        break;
    }
    default:
        return 1;
    }

    m->steps++;
    return 0;
}

int qjs_ownership_validate(const uint8_t *data, size_t size,
                           qjs_ownership_plan *plan_out)
{
    qjs_ownership_model scratch;
    size_t count, i;

    if (size % 3u != 0)
        return 1; /* truncated record */
    count = size / 3u;
    if (count > QJS_OWN_MAX_STEPS)
        return 1;

    plan_out->count = count;
    for (i = 0; i < count; i++) {
        plan_out->steps[i].op = data[i * 3u];
        plan_out->steps[i].a = data[i * 3u + 1u];
        plan_out->steps[i].b = data[i * 3u + 2u];
    }

    /* Whole-plan pre-validation on logical state only, before any engine
     * call: an illegal plan is rejected as a unit. */
    qjs_ownership_model_init(&scratch);
    for (i = 0; i < count; i++) {
        if (qjs_ownership_model_apply(&scratch, &plan_out->steps[i]))
            return 1;
    }
    return 0;
}

int qjs_ownership_model_resp(const qjs_ownership_model *m, unsigned int id,
                             qjs_ownership_resp cat)
{
    if (id >= QJS_OWN_MAX_IDS || !m->ids[id].used ||
        (unsigned int)cat >= QJS_OWN_RESP__COUNT)
        return 0;
    return m->ids[id].resp[cat];
}

int qjs_ownership_model_alive(const qjs_ownership_model *m, unsigned int id)
{
    if (id >= QJS_OWN_MAX_IDS)
        return 0;
    return m->ids[id].used && m->ids[id].alive;
}

int qjs_ownership_model_finalized(const qjs_ownership_model *m,
                                  unsigned int id)
{
    if (id >= QJS_OWN_MAX_IDS)
        return 0;
    return m->ids[id].used && !m->ids[id].alive;
}

size_t qjs_ownership_model_finalized_set(const qjs_ownership_model *m,
                                         int out[QJS_OWN_MAX_IDS])
{
    size_t i, n = 0;
    for (i = 0; i < QJS_OWN_MAX_IDS; i++) {
        out[i] = m->ids[i].used && !m->ids[i].alive;
        if (out[i])
            n++;
    }
    return n;
}

const char *qjs_ownership_resp_name(qjs_ownership_resp cat)
{
    switch (cat) {
    case QJS_OWN_RESP_ROOT:
        return "root";
    case QJS_OWN_RESP_HOLDER:
        return "holder";
    case QJS_OWN_RESP_PENDING:
        return "pending";
    default:
        return "unknown";
    }
}

const char *qjs_ownership_op_name(uint8_t op)
{
    switch (op) {
    case QJS_OWN_OP_NEW:
        return "NEW";
    case QJS_OWN_OP_DUP:
        return "DUP";
    case QJS_OWN_OP_DROP:
        return "DROP";
    case QJS_OWN_OP_THROW_TAKE:
        return "THROW_TAKE";
    case QJS_OWN_OP_CATCH:
        return "CATCH";
    case QJS_OWN_OP_GC:
        return "GC";
    case QJS_OWN_OP_STORE_TAKE:
        return "STORE_TAKE";
    case QJS_OWN_OP_CLEAR:
        return "CLEAR";
    case QJS_OWN_OP_REJECT_TAKE:
        return "REJECT_TAKE";
    case QJS_OWN_OP_CALL_THROW:
        return "CALL_THROW";
    default:
        return "UNKNOWN";
    }
}

void qjs_ownership_format_mismatch(char *buf, size_t buf_size,
                                   size_t step_index, uint8_t op, int id,
                                   const char *category,
                                   int expected, int actual)
{
    if (!buf || buf_size == 0)
        return;
    snprintf(buf, buf_size,
             "ownership mismatch at step %zu op=%s id=%d category=%s "
             "expected=%d actual=%d",
             step_index, qjs_ownership_op_name(op), id,
             category ? category : "?", expected, actual);
}
