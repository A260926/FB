#include "task_oracle.h"
#include <stdlib.h>
#include <string.h>

/* Independent reference interpreter using a persistent (immutable, tail-linked)
 * continuation. A continuation is a singly linked list of frames allocated in
 * an index pool; solving a node prepends frames to build a new list and never
 * mutates an existing one, so a failed speculative attempt cannot corrupt the
 * caller's continuation. This makes two subtle backtracking rules exact:
 *
 *   - nested greedy quantifiers: (c{2,3}){1,3} on seven c's matches six,
 *     because the outer "stop after two iterations" frame is newer than the
 *     inner quantifier's frames;
 *   - alternation fall-through: a branch that succeeds locally but makes the
 *     surrounding continuation fail is abandoned and the next branch is tried
 *     against the very same continuation.
 *
 * Captures are a mutable global array with explicit snapshots: alternation and
 * capture entry use stack-local saves, while a repeat's pre-copy snapshot is
 * carried by its REPLOOP frame so backtracking restores PCRE overwrite/rollback
 * semantics. Unset and empty-but-participating captures stay distinct. */

#define TASK_ORACLE_STEP_LIMIT 4000000
#define TASK_FRAME_POOL  300000
#define TASK_SNAP_POOL   80000
#define K_END (-1)

typedef struct M M;

enum { K_SEQ = 0, K_CAPEND, K_REPLOOP };

typedef struct {
    int tag;
    const TaskNode *node;   /* K_SEQ: concat; K_REPLOOP: repeat node      */
    int idx;                /* K_SEQ: next child index                     */
    int itnext;             /* K_REPLOOP: iterations already completed     */
    int capno;              /* K_CAPEND                                    */
    int tail;               /* tail continuation index, K_END terminates   */
    int snap;               /* K_REPLOOP: pre-copy capture snapshot id     */
} KFrame;

typedef struct {
    KFrame pool[TASK_FRAME_POOL];
    int top;
    int exhausted;
} KPool;

typedef struct {
    TaskCap cap[TASK_MAX_CAPS + 1];
} CapSnap;

struct M {
    const uint32_t *subj;
    int n;
    TaskCap cap[TASK_MAX_CAPS + 1];
    int ncaps;
    long steps;
    int limit_hit;
    KPool kp;
    CapSnap snaps[TASK_SNAP_POOL];
    int snap_top;
};

#define M_FAIL (-1)

static int m_solve(M *m, const TaskNode *node, int pos, int k);
static int m_cont(M *m, int k, int pos);
static int m_rep_go(M *m, const TaskNode *rep, int it, int pos, int k);

static int kframe(M *m, KFrame fr) {
    if (m->kp.top >= TASK_FRAME_POOL) { m->limit_hit = 1; return M_FAIL; }
    m->kp.pool[m->kp.top] = fr;
    return m->kp.top++;
}

static int snap_save(M *m) {
    if (m->snap_top >= TASK_SNAP_POOL) { m->limit_hit = 1; return M_FAIL; }
    memcpy(m->snaps[m->snap_top].cap, m->cap, sizeof(m->cap));
    return m->snap_top++;
}

static void snap_restore(M *m, int id) {
    if (id >= 0) memcpy(m->cap, m->snaps[id].cap, sizeof(m->cap));
}

static int class_matches(const TaskNode *n, uint32_t cp) {
    int i, hit = 0;
    for (i = 0; i < n->nchars; i++)
        if (n->chars[i] == cp) { hit = 1; break; }
    return n->neg ? !hit : hit;
}

/* Run a continuation at pos. Tail frames (CAPEND, end-of-SEQ) are collapsed in
 * this loop; SEQ entry and REPLOOP re-enter the solver. */
static int m_cont(M *m, int k, int pos) {
    while (k != K_END) {
        KFrame fr = m->kp.pool[k];
        if (fr.tag == K_CAPEND) {
            m->cap[fr.capno].set = 1;
            m->cap[fr.capno].end = pos;   /* start==pos: empty, still set */
            k = fr.tail;
            continue;
        }
        if (fr.tag == K_SEQ) {
            const TaskNode *cat = fr.node;
            if (fr.idx >= cat->nch) { k = fr.tail; continue; }
            {
                KFrame more;
                int nk;
                memset(&more, 0, sizeof(more));
                more.tag = K_SEQ;
                more.node = cat;
                more.idx = fr.idx + 1;
                more.tail = fr.tail;
                nk = kframe(m, more);
                if (nk == M_FAIL) return M_FAIL;
                return m_solve(m, cat->ch[fr.idx], pos, nk);
            }
        }
        /* K_REPLOOP: one copy just ended at pos; continue the repeat, using
         * this frame's pre-copy snapshot if a later rollback is needed. */
        return m_rep_go(m, fr.node, fr.itnext, pos, fr.tail);
    }
    return pos;
}

/* Greedy bounded repetition: attempt another copy first; only if that cannot
 * extend do we stop (provided the minimum is met), restoring the pre-copy
 * captures on rollback. */
static int m_rep_go(M *m, const TaskNode *rep, int it, int pos, int k) {
    if (++m->steps > TASK_ORACLE_STEP_LIMIT) {
        m->limit_hit = 1;
        return M_FAIL;
    }
    if (it < rep->rmax) {
        KFrame loop;
        int snap, lk, rc;
        snap = snap_save(m);
        if (snap == M_FAIL) return M_FAIL;
        memset(&loop, 0, sizeof(loop));
        loop.tag = K_REPLOOP;
        loop.node = rep;
        loop.itnext = it + 1;
        loop.tail = k;
        loop.snap = snap;
        lk = kframe(m, loop);
        if (lk == M_FAIL) return M_FAIL;
        rc = m_solve(m, rep->ch[0], pos, lk);
        if (rc != M_FAIL) return rc;
        if (m->limit_hit) return M_FAIL;
        snap_restore(m, snap);          /* undo the whole failed take */
    }
    if (it >= rep->rmin) return m_cont(m, k, pos);   /* stop option */
    return M_FAIL;
}

static int m_solve(M *m, const TaskNode *node, int pos, int k) {
    if (++m->steps > TASK_ORACLE_STEP_LIMIT) {
        m->limit_hit = 1;
        return M_FAIL;
    }
    switch (node->kind) {
    case TASK_LIT:
        if (pos >= m->n || m->subj[pos] != node->cp) return M_FAIL;
        return m_cont(m, k, pos + 1);

    case TASK_CLASS:
        if (pos >= m->n || !class_matches(node, m->subj[pos])) return M_FAIL;
        return m_cont(m, k, pos + 1);

    case TASK_GROUP:
        return m_solve(m, node->ch[0], pos, k);

    case TASK_CAT: {
        KFrame fr;
        int nk;
        memset(&fr, 0, sizeof(fr));
        fr.tag = K_SEQ;
        fr.node = node;
        fr.idx = 1;                 /* child 0 runs now, rest via the frame */
        fr.tail = k;
        nk = kframe(m, fr);
        if (nk == M_FAIL) return M_FAIL;
        return m_solve(m, node->ch[0], pos, nk);
    }

    case TASK_ALT: {
        TaskCap save[TASK_MAX_CAPS + 1];
        int i, rc;
        memcpy(save, m->cap, sizeof(save));
        for (i = 0; i < node->nch; i++) {
            memcpy(m->cap, save, sizeof(save));   /* left branch first */
            rc = m_solve(m, node->ch[i], pos, k); /* same continuation */
            if (rc != M_FAIL) return rc;
            if (m->limit_hit) {
                memcpy(m->cap, save, sizeof(save));
                return M_FAIL;
            }
        }
        memcpy(m->cap, save, sizeof(save));
        return M_FAIL;
    }

    case TASK_CAP: {
        TaskCap save[TASK_MAX_CAPS + 1];
        KFrame end;
        int c = node->capno, ek, rc;
        if (c < 1 || c > m->ncaps) return M_FAIL;
        memcpy(save, m->cap, sizeof(save));
        m->cap[c].set = 1;                 /* participation begins */
        m->cap[c].start = pos;
        m->cap[c].end = pos;               /* empty until the child advances */
        memset(&end, 0, sizeof(end));
        end.tag = K_CAPEND;
        end.capno = c;
        end.tail = k;
        ek = kframe(m, end);
        if (ek == M_FAIL) return M_FAIL;
        rc = m_solve(m, node->ch[0], pos, ek);
        if (rc == M_FAIL) memcpy(m->cap, save, sizeof(save));
        return rc;
    }

    case TASK_REP:
        return m_rep_go(m, node, 0, pos, k);
    }
    return M_FAIL;
}

int task_oracle_run(const TaskNode *root, int ncaps,
                    const uint32_t *subj, int n, TaskOracleResult *out) {
    M *m;
    TaskCap fresh[TASK_MAX_CAPS + 1];
    int start, r, i;

    /* The frame/snapshot pools are large; keep the interpreter state off the
     * normal stack. */
    m = (M *)calloc(1, sizeof(M));
    if (!m) return TASK_ORACLE_LIMIT;
    m->subj = subj;
    m->n = n;
    m->ncaps = ncaps;
    for (i = 0; i <= TASK_MAX_CAPS; i++) {
        fresh[i].set = 0;
        fresh[i].start = fresh[i].end = TASK_CAP_UNSET;
    }

    memset(out, 0, sizeof(*out));
    out->ncaps = ncaps;
    memcpy(out->cap, fresh, sizeof(fresh));

    /* leftmost overall match: earliest starting position that succeeds */
    for (start = 0; start <= n; start++) {
        memcpy(m->cap, fresh, sizeof(fresh));
        m->kp.top = 0;
        m->kp.exhausted = 0;
        m->snap_top = 0;
        m->steps = 0;
        m->limit_hit = 0;
        r = m_solve(m, root, start, K_END);
        if (m->limit_hit) { free(m); return TASK_ORACLE_LIMIT; }
        if (r != M_FAIL) {
            out->matched = 1;
            out->start = start;
            out->end = r;
            memcpy(out->cap, m->cap, sizeof(m->cap));
            free(m);
            return TASK_ORACLE_OK;
        }
    }
    out->matched = 0;
    free(m);
    return TASK_ORACLE_NO_MATCH;
}
