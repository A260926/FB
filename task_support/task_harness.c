#include "task_harness.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TASK_PINNED 8
#define RNG_BASE    0x243f6a8885a308d3ULL
#define RNG_GOLDEN  11400714819323198485ULL

task_cov task_coverage;
int task_fault_inject;

typedef struct tslot {
    z_stream z;
    int active;
    int dict_done;
    int finished;
    int parent;          /* slot this one was copied from, or -1 */
    task_branch b;
} tslot;

void task_cov_reset(void)
{
    memset(&task_coverage, 0, sizeof(task_coverage));
}

void task_program_free(task_program *p)
{
    unsigned i;
    task_buf_free(&p->dict);
    task_buf_free(&p->source);
    for (i = 0; i < TASK_MAX_ACTIONS; i++) {
        task_buf_free(&p->cap_act[i]);
        p->has_cap_act[i] = 0;
    }
}

/* ---------------- generator helpers ---------------- */

static void gen_feed(task_program *p, task_rng *r, unsigned char kind,
                     unsigned char slot, unsigned len, unsigned leave)
{
    task_action *a = &p->act[p->nact++];
    a->kind = kind;
    a->slot = slot;
    a->dst = 0;
    a->arg = (unsigned char)leave;
    a->src_off = p->source.len;
    a->len = len;
    task_model_bytes(r, &p->source, len);
    p->plain_total += len;
}

static void gen_simple(task_program *p, unsigned char kind, unsigned char slot,
                       unsigned char dst, unsigned char arg)
{
    task_action *a = &p->act[p->nact++];
    a->kind = kind;
    a->slot = slot;
    a->dst = dst;
    a->arg = arg;
    a->len = 0;
    a->src_off = 0;
}

static void gen_dict(task_program *p, task_rng *r)
{
    unsigned n = 8 + task_rng_below(r, 193);
    task_model_bytes(r, &p->dict, n);
}

/* Hand pinned programs so every required path is covered regardless of RNG. */
static void gen_pinned(task_program *p, task_rng *r, unsigned index)
{
    p->window_bits = (index & 1) ? -15 : 15;
    p->use_dict = (index != 2 && index != 3 && index != 6);
    p->level = (index == 2) ? 0 : 1 + (int)task_rng_below(r, 9);
    if (p->use_dict)
        gen_dict(p, r);

    gen_simple(p, A_INIT, 0, 0, 0);

    switch (index) {
    case 0: /* zlib+dict, partial input at copy, divergent suffixes */
        gen_feed(p, r, A_FEED, 0, 512, 0);
        gen_feed(p, r, A_PARTFEED, 0, 300, 7);
        gen_simple(p, A_DCOPY, 0, 1, 0);
        gen_simple(p, A_FLUSH, 0, 0, TASK_ARG_SYNC);
        gen_feed(p, r, A_FEED, 0, 200, 0);
        gen_feed(p, r, A_FEED, 1, 256, 0);
        break;
    case 1: /* raw+dict, FULL_FLUSH, pending output at copy */
        gen_feed(p, r, A_FEED, 0, 400, 0);
        gen_simple(p, A_FLUSH, 0, 0, TASK_ARG_FULL);
        gen_feed(p, r, A_PARTFEED, 0, 260, 11);
        gen_simple(p, A_DCOPY, 0, 1, 1);
        gen_feed(p, r, A_FEED, 0, 128, 0);
        gen_feed(p, r, A_FEED, 1, 140, 0);
        break;
    case 2: /* zlib, stored blocks, four-stream copy chain */
        gen_feed(p, r, A_FEED, 0, 300, 0);
        gen_simple(p, A_DCOPY, 0, 1, 0);
        gen_feed(p, r, A_FEED, 1, 100, 0);
        gen_simple(p, A_DCOPY, 1, 2, 0);
        gen_feed(p, r, A_FEED, 2, 100, 0);
        gen_simple(p, A_DCOPY, 2, 3, 0);
        gen_feed(p, r, A_FEED, 3, 100, 0);
        break;
    case 3: /* raw, partial input then copy, FULL_FLUSH both branches */
        gen_feed(p, r, A_PARTFEED, 0, 220, 5);
        gen_simple(p, A_DCOPY, 0, 1, 0);
        gen_simple(p, A_FLUSH, 0, 0, TASK_ARG_FULL);
        gen_simple(p, A_FLUSH, 1, 0, TASK_ARG_FULL);
        gen_feed(p, r, A_FEED, 0, 90, 0);
        gen_feed(p, r, A_FEED, 1, 90, 0);
        break;
    case 4: /* zlib+dict, SYNC_FLUSH then a clean copy */
        gen_feed(p, r, A_FEED, 0, 350, 0);
        gen_simple(p, A_FLUSH, 0, 0, TASK_ARG_SYNC);
        gen_simple(p, A_DCOPY, 0, 1, 0);
        gen_feed(p, r, A_FEED, 0, 80, 0);
        gen_feed(p, r, A_FEED, 1, 80, 0);
        break;
    case 5: /* raw+dict, double fork, four active streams */
        gen_feed(p, r, A_FEED, 0, 256, 0);
        gen_simple(p, A_DCOPY, 0, 1, 0);
        gen_simple(p, A_DCOPY, 0, 2, 0);
        gen_simple(p, A_DCOPY, 1, 3, 0);
        gen_feed(p, r, A_FEED, 0, 64, 0);
        gen_feed(p, r, A_FEED, 1, 64, 0);
        gen_feed(p, r, A_FEED, 2, 64, 0);
        gen_feed(p, r, A_FEED, 3, 64, 0);
        break;
    case 6: /* zlib, both flush kinds, pending-output copy */
        gen_feed(p, r, A_FEED, 0, 500, 0);
        gen_simple(p, A_FLUSH, 0, 0, TASK_ARG_SYNC);
        gen_simple(p, A_FLUSH, 0, 0, TASK_ARG_FULL);
        gen_feed(p, r, A_PARTFEED, 0, 180, 3);
        gen_simple(p, A_DCOPY, 0, 1, 1);
        gen_feed(p, r, A_FEED, 1, 70, 0);
        break;
    default: { /* raw+dict, many tiny feeds and pending output */
        unsigned k;
        for (k = 0; k < 10; k++)
            gen_feed(p, r, A_FEED, 0, 1 + (k * 7) % 13, 0);
        gen_feed(p, r, A_PARTFEED, 0, 64, 2);
        gen_simple(p, A_DCOPY, 0, 1, 1);
        gen_feed(p, r, A_FEED, 1, 50, 0);
        break;
    }
    }

    {
        int s;
        for (s = 0; s < TASK_MAX_STREAMS; s++) {
            int used = 0, k;
            for (k = 0; k < (int)p->nact; k++) {
                task_action *a = &p->act[k];
                if ((a->kind == A_INIT && a->slot == s) ||
                    (a->kind == A_DCOPY && a->dst == s) ||
                    (a->kind != A_DCOPY && a->kind != A_INIT &&
                     a->slot == s))
                    used = 1;
            }
            if (used) {
                gen_simple(p, A_FINISH, (unsigned char)s, 0, 0);
                gen_simple(p, A_END, (unsigned char)s, 0, 0);
            }
        }
    }
}

static void gen_random(task_program *p, task_rng *r)
{
    int active[TASK_MAX_STREAMS] = {1, 0, 0, 0};
    unsigned n;

    p->window_bits = (task_rng_below(r, 2)) ? -15 : 15;
    p->use_dict = (task_rng_below(r, 3) == 0);
    p->level = 1 + (int)task_rng_below(r, 9);
    if (p->use_dict)
        gen_dict(p, r);
    gen_simple(p, A_INIT, 0, 0, 0);

    while (p->nact < TASK_MAX_ACTIONS - 2 * TASK_MAX_STREAMS) {
        unsigned choice = task_rng_below(r, 100);
        int s, dst, count = 0;
        for (s = 0; s < TASK_MAX_STREAMS; s++)
            count += active[s];
        if (count == 0)
            break; /* the single INIT stream was already closed */

        if (choice < 52 || count == TASK_MAX_STREAMS) {
            unsigned remain = TASK_MAX_PLAIN - p->plain_total;
            unsigned maxlen = remain < 1024 ? remain : 1024;
            unsigned len;
            if (maxlen == 0) break;
            len = 1 + task_rng_below(r, maxlen);
            do { s = task_rng_below(r, TASK_MAX_STREAMS); } while (!active[s]);
            if (choice < 14 && len > 1)
                gen_feed(p, r, A_PARTFEED, (unsigned char)s, len,
                         1 + task_rng_below(r, len < 8 ? len - 1 : 7));
            else
                gen_feed(p, r, A_FEED, (unsigned char)s, len, 0);
        } else if (choice < 64) {
            do { s = task_rng_below(r, TASK_MAX_STREAMS); } while (!active[s]);
            gen_simple(p, A_FLUSH, (unsigned char)s, 0,
                       task_rng_below(r, 2) ? TASK_ARG_SYNC : TASK_ARG_FULL);
        } else if (choice < 86 && count < TASK_MAX_STREAMS) {
            do { s = task_rng_below(r, TASK_MAX_STREAMS); } while (!active[s]);
            for (dst = 0; dst < TASK_MAX_STREAMS && active[dst]; dst++)
                ;
            gen_simple(p, A_DCOPY, (unsigned char)s, (unsigned char)dst,
                       task_rng_below(r, 4) == 0);
            active[dst] = 1;
        } else {
            do { s = task_rng_below(r, TASK_MAX_STREAMS); } while (!active[s]);
            gen_simple(p, A_FINISH, (unsigned char)s, 0, 0);
            gen_simple(p, A_END, (unsigned char)s, 0, 0);
            active[s] = 0;
        }
    }

    for (n = 0; n < TASK_MAX_STREAMS; n++) {
        if (active[n]) {
            gen_simple(p, A_FINISH, (unsigned char)n, 0, 0);
            gen_simple(p, A_END, (unsigned char)n, 0, 0);
        }
    }
}

void task_generate(task_program *prog, unsigned index)
{
    task_rng r;
    memset(prog, 0, sizeof(*prog));
    prog->tag = (int)index;
    task_rng_seed(&r, RNG_BASE + (unsigned long long)index * RNG_GOLDEN);
    if (index < TASK_PINNED)
        gen_pinned(prog, &r, index);
    else
        gen_random(prog, &r);
}

/* ---------------- interpreter ---------------- */

static void bind_input(tslot *s)
{
    s->z.next_in = s->b.store.p + s->b.cursor;
    s->z.avail_in = (uInt)(s->b.store.len - s->b.cursor);
}

static int pump_all_input(tslot *s)
{
    const unsigned char *start = s->z.next_in;
    while (s->z.avail_in > 0) {
        int rc = deflate(&s->z, Z_NO_FLUSH);
        if (rc != Z_OK)
            return rc;
        if (s->z.avail_out == 0)
            task_branch_drain(&s->b, &s->z);
    }
    s->b.cursor += (size_t)(s->z.next_in - start);
    return Z_OK;
}

static int pump_flush(tslot *s, int flush)
{
    int rc;
    do {
        rc = deflate(&s->z, flush);
        if (s->z.avail_out == 0)
            task_branch_drain(&s->b, &s->z);
    } while (rc == Z_OK && s->z.avail_out == 0);
    /* a second flush with no new input in between is a documented no-op:
     * the library reports Z_BUF_ERROR, which is not a failure */
    return (rc == Z_BUF_ERROR) ? Z_OK : rc;
}

static int pump_finish(tslot *s)
{
    int rc;
    /* any bytes the caller still held back at next_in must cross the API
     * before the stream is closed */
    bind_input(s);
    rc = pump_all_input(s);
    if (rc != Z_OK)
        return rc;
    s->z.next_in = NULL;
    s->z.avail_in = 0;
    do {
        rc = deflate(&s->z, Z_FINISH);
        if (s->z.avail_out == 0)
            task_branch_drain(&s->b, &s->z);
    } while (rc != Z_STREAM_END && rc == Z_OK);
    if (s->z.avail_out != s->b.out_cap)
        task_branch_drain(&s->b, &s->z);
    return rc;
}

static void oracle_decode(const task_program *p, const task_buf *compressed,
                          const task_branch *b, unsigned index)
{
    task_rng r;
    task_buf got;
    task_buf_init(&got);
    task_rng_seed(&r, 0x517cc1b727220a95ULL + index * RNG_GOLDEN);
    unsigned in_chunk = 1 + task_rng_below(&r, 257);
    unsigned out_chunk = 1 + task_rng_below(&r, 129);

    int rc = task_ref_inflate(compressed, &got, p->window_bits,
                              p->dict.p, (unsigned)p->dict.len,
                              in_chunk, out_chunk);
    TASK_CHECK(rc == 0);
    /* predetermined logic-difference simulator: force a one-byte divergence
     * between the oracle output and the model expectation */
    if (task_fault_inject && got.len && b->expect.len)
        got.p[0] = (unsigned char)(b->expect.p[0] ^ 0xFFu);
    TASK_CHECK(task_buf_equal(&got, &b->expect));
    task_buf_free(&got);
}

void task_run_deflate(task_program *p, int capture)
{
    tslot slots[TASK_MAX_STREAMS];
    task_rng r;
    unsigned i;
    int s;

    memset(slots, 0, sizeof(slots));
    for (s = 0; s < TASK_MAX_STREAMS; s++) {
        task_branch_init(&slots[s].b);
        slots[s].parent = -1;
    }
    task_rng_seed(&r, 0x6c62272e07bb0142ULL + (unsigned long long)p->tag * 7
                      + (unsigned long long)(p->window_bits < 0 ? 1 : 0));

    if (capture)
        task_coverage.programs++;
    if (p->window_bits < 0)
        task_coverage.raw_programs++;
    else
        task_coverage.zlib_programs++;
    if (p->use_dict)
        task_coverage.dict_paths++;

    for (i = 0; i < p->nact; i++) {
        task_action *a = &p->act[i];

        switch (a->kind) {
        case A_INIT: {
            tslot *q = &slots[a->slot];
            int rc;
            TASK_CHECK(!q->active);
            memset(&q->z, 0, sizeof(q->z));
            q->z.zalloc = task_zalloc;
            q->z.zfree = task_zfree;
            rc = deflateInit2(&q->z, p->level, Z_DEFLATED, p->window_bits,
                              8, Z_DEFAULT_STRATEGY);
            TASK_CHECK(rc == Z_OK);
            if (p->use_dict) {
                rc = deflateSetDictionary(&q->z, p->dict.p,
                                          (uInt)p->dict.len);
                TASK_CHECK(rc == Z_OK);
                q->dict_done = 1;
            }
            task_branch_init(&q->b);
            task_branch_bind_out(&r, &q->b, &q->z);
            q->active = 1;
            q->finished = 0;
            q->parent = -1;
            break;
        }

        case A_FEED:
        case A_PARTFEED: {
            tslot *q = &slots[a->slot];
            unsigned leave = (a->kind == A_PARTFEED) ? a->arg : 0;
            int rc;
            TASK_CHECK(q->active && !q->finished);
            TASK_CHECK(a->src_off + a->len <= p->source.len);
            task_buf_append(&q->b.expect, p->source.p + a->src_off, a->len);
            task_buf_append(&q->b.store, p->source.p + a->src_off, a->len);
            bind_input(q);
            if (leave) {
                TASK_CHECK(leave < q->z.avail_in);
                q->z.avail_in -= leave; /* part of the caller store is held back */
            }
            rc = pump_all_input(q);
            TASK_CHECK(rc == Z_OK);
            if (leave)
                TASK_CHECK(q->b.store.len - q->b.cursor == leave);
            break;
        }

        case A_FLUSH: {
            tslot *q = &slots[a->slot];
            int rc;
            TASK_CHECK(q->active && !q->finished);
            TASK_CHECK(a->arg == TASK_ARG_SYNC || a->arg == TASK_ARG_FULL);
            q->z.next_in = NULL;
            q->z.avail_in = 0;
            rc = pump_flush(q, a->arg);
            TASK_CHECK(rc == Z_OK);
            if (a->arg == TASK_ARG_SYNC)
                task_coverage.sync_flushes++;
            else
                task_coverage.full_flushes++;
            break;
        }

        case A_DCOPY: {
            tslot *src = &slots[a->slot];
            tslot *dst = &slots[a->dst];
            size_t pending_in, pending_out;
            int rc, k;
            TASK_CHECK(src->active && !src->finished && !dst->active);

            if (a->arg & 1) {
                /* constrain the output slot and feed held-back input so the
                 * copy instant has both pending input and pending output */
                const Bytef *in0;
                task_branch_drain(&src->b, &src->z);
                if (src->z.avail_in == 0)
                    bind_input(src);
                if (src->z.avail_in) {
                    free(src->b.out);
                    src->b.out_cap = 17;
                    src->b.out = (unsigned char *)malloc(src->b.out_cap);
                    TASK_CHECK(src->b.out != NULL);
                    src->z.next_out = src->b.out;
                    src->z.avail_out = (uInt)src->b.out_cap;
                    in0 = src->z.next_in;
                    rc = deflate(&src->z, Z_NO_FLUSH);
                    TASK_CHECK(rc == Z_OK);
                    /* keep the external cursor aligned with the API cursor */
                    src->b.cursor += (size_t)(src->z.next_in - in0);
                }
            }

            pending_in = src->z.avail_in;
            pending_out = src->b.out_cap - src->z.avail_out;
            /* the caller-owned input store may still hold bytes that were
             * never offered to the API at all */
            if (src->b.store.len - src->b.cursor > 0)
                task_coverage.copy_partial_input++;
            if (pending_out)
                task_coverage.copy_partial_output++;

            /* copy while the library still holds internal pending output and
             * the caller's next_in cursor; those pointers are caller-owned
             * and get explicitly rebound below, never shared between the two
             * branches */
            rc = deflateCopy(&dst->z, &src->z);
            TASK_CHECK(rc == Z_OK);
            task_coverage.deflate_copies++;
            task_coverage.branch_forks++;

            /* reclaim whatever the source had produced into the shared slot
             * before either branch can touch it again */
            task_branch_drain(&src->b, &src->z);

            /* model-side duplication: plain stores only, never deflateCopy */
            task_branch_duplicate(&src->b, &dst->b);
            dst->active = 1;
            dst->finished = 0;
            dst->dict_done = src->dict_done;
            dst->parent = a->slot;
            {
                unsigned live = 0, k2;
                for (k2 = 0; k2 < TASK_MAX_STREAMS; k2++)
                    live += slots[k2].active;
                if (live > task_coverage.active_peak)
                    task_coverage.active_peak = live;
            }

            /* every branch gets its own, separately allocated stores.
             * next_in starts at the model cursor and only the bytes the
             * source had actually offered (pending_in) are exposed; any
             * bytes the caller held back stay past avail_in */
            task_branch_bind_out(&r, &dst->b, &dst->z);
            TASK_CHECK(dst->b.out != src->b.out);
            dst->z.next_in = dst->b.store.p + dst->b.cursor;
            dst->z.avail_in = (uInt)pending_in;
            TASK_CHECK(dst->b.store.len - dst->b.cursor >= pending_in);

            /* shared compressed prefix, independent backing memory */
            TASK_CHECK(task_buf_equal(&dst->b.prefix, &src->b.prefix));
            if (src->b.prefix.len)
                TASK_CHECK(dst->b.prefix.p != src->b.prefix.p);
            for (k = 0; k < TASK_MAX_STREAMS; k++)
                if (slots[k].active && k != a->dst)
                    TASK_CHECK(slots[k].b.out != dst->b.out);
            break;
        }

        case A_FINISH: {
            tslot *q = &slots[a->slot];
            int rc, k, descendant = 0;
            TASK_CHECK(q->active && !q->finished);
            for (k = 0; k < TASK_MAX_STREAMS; k++)
                if (slots[k].active && slots[k].parent == a->slot)
                    descendant = 1;
            if (descendant)
                task_coverage.finish_original_first++;
            rc = pump_finish(q);
            TASK_CHECK(rc == Z_STREAM_END);
            q->finished = 1;

            oracle_decode(p, &q->b.prefix, &q->b, i);

            if (capture) {
                task_buf_take(&p->cap_act[i], &q->b.prefix);
                p->has_cap_act[i] = 1;
            } else {
                TASK_CHECK(p->has_cap_act[i]);
                TASK_CHECK(task_buf_equal(&p->cap_act[i], &q->b.prefix));
            }
            break;
        }

        case A_END: {
            tslot *q = &slots[a->slot];
            int rc;
            TASK_CHECK(q->active && q->finished);
            TASK_CHECK(q->b.cursor == q->b.store.len);
            rc = deflateEnd(&q->z);
            TASK_CHECK(rc == Z_OK);
            task_branch_free(&q->b);
            q->active = 0;
            break;
        }

        default:
            TASK_CHECK(0);
        }
    }

    for (s = 0; s < TASK_MAX_STREAMS; s++)
        TASK_CHECK(!slots[s].active);
}

/* ---------------- inflateCopy scenario ---------------- */

void task_run_inflate_copy(task_rng *r, int raw, int use_dict)
{
    const int wb = raw ? -15 : 15;
    task_buf plain, dict, comp;
    task_buf instore, cpstore, pre, got_orig, got_copy;
    z_stream orig, cp;
    unsigned char *out_a, *out_b;
    size_t cap_a = 33, cap_b = 47;
    size_t k, consumed;
    int rc, dict_done = 0;

    task_buf_init(&plain);
    task_buf_init(&dict);
    task_buf_init(&comp);
    task_buf_init(&instore);
    task_buf_init(&cpstore);
    task_buf_init(&pre);
    task_buf_init(&got_orig);
    task_buf_init(&got_copy);

    task_model_bytes(r, &plain, 2048 + task_rng_below(r, 6144));
    if (use_dict)
        task_model_bytes(r, &dict, 16 + task_rng_below(r, 180));

    /* the driver only ever feeds streams it encoded itself */
    rc = task_ref_deflate(&plain, &comp, wb, 6, dict.p,
                          (unsigned)dict.len,
                          task_rng_below(r, 2) ? Z_SYNC_FLUSH : 0,
                          1 + task_rng_below(r, 300));
    TASK_CHECK(rc == 0);
    TASK_CHECK(comp.len > 4);

    task_buf_take(&instore, &comp);          /* caller-owned input store */
    memset(&orig, 0, sizeof(orig));
    orig.zalloc = task_zalloc;
    orig.zfree = task_zfree;
    rc = inflateInit2(&orig, wb);
    TASK_CHECK(rc == Z_OK);
    if (raw && dict.len) {
        rc = inflateSetDictionary(&orig, dict.p, (uInt)dict.len);
        TASK_CHECK(rc == Z_OK);
        dict_done = 1;
    }

    out_a = (unsigned char *)malloc(cap_a);
    TASK_CHECK(out_a != NULL);

    /* constrain both sides: only the first part of the input is offered, and
     * the output slot is deliberately tiny */
    k = comp.len / 2 + task_rng_below(r, (unsigned)(comp.len / 4) + 1);
    orig.next_in = instore.p;
    orig.avail_in = (uInt)k;
    orig.next_out = out_a;
    orig.avail_out = (uInt)cap_a;

    for (;;) {
        rc = inflate(&orig, Z_NO_FLUSH);
        if (rc == Z_NEED_DICT) {
            TASK_CHECK(!raw && dict.len && !dict_done);
            TASK_CHECK(orig.adler ==
                       adler32(adler32(0L, Z_NULL, 0), dict.p,
                               (uInt)dict.len));
            rc = inflateSetDictionary(&orig, dict.p, (uInt)dict.len);
            TASK_CHECK(rc == Z_OK);
            dict_done = 1;
            continue;
        }
        TASK_CHECK(rc == Z_OK || rc == Z_BUF_ERROR);
        if (orig.avail_out == 0 || rc == Z_BUF_ERROR)
            break;
    }
    TASK_CHECK(orig.total_in > 0 && orig.total_in < comp.len);
    TASK_CHECK(orig.avail_out == 0); /* output slot saturated at copy time */

    task_buf_append(&pre, out_a, cap_a - orig.avail_out);
    /* total_in excludes wrapper header bytes, so the caller-store offset must
     * come from the next_in pointer, not from total_in */
    consumed = (size_t)(orig.next_in - instore.p);
    TASK_CHECK(consumed > 0 && consumed < instore.len);
    /* the prefix already produced must agree with the independent plaintext
     * model before either branch continues */
    TASK_CHECK(pre.len <= plain.len);
    TASK_CHECK(memcmp(pre.p, plain.p, pre.len) == 0);
    if (orig.avail_in > 0)
        task_coverage.copy_partial_input++;
    task_coverage.copy_partial_output++;

    rc = inflateCopy(&cp, &orig);
    TASK_CHECK(rc == Z_OK);
    task_coverage.inflate_copies++;
    if (use_dict)
        task_coverage.inflate_dict_paths++;

    /* rebind: original keeps its own stores, the copy receives deep,
     * independent caller-owned stores */
    orig.next_in = instore.p + consumed;
    orig.avail_in = (uInt)(instore.len - consumed);
    orig.next_out = out_a;
    orig.avail_out = (uInt)cap_a;

    task_buf_take(&cpstore, &instore);
    out_b = (unsigned char *)malloc(cap_b);
    TASK_CHECK(out_b != NULL);
    cp.next_in = cpstore.p + consumed;
    cp.avail_in = (uInt)(cpstore.len - consumed);
    cp.next_out = out_b;
    cp.avail_out = (uInt)cap_b;
    TASK_CHECK(cp.next_in != orig.next_in);
    TASK_CHECK(cp.next_out != orig.next_out);

    /* finish the original stream first */
    for (;;) {
        rc = inflate(&orig, Z_NO_FLUSH);
        size_t made = cap_a - orig.avail_out;
        if (made)
            task_buf_append(&got_orig, out_a, made);
        orig.next_out = out_a;
        orig.avail_out = (uInt)cap_a;
        if (rc == Z_NEED_DICT) {
            rc = inflateSetDictionary(&orig, dict.p, (uInt)dict.len);
            TASK_CHECK(rc == Z_OK);
            continue;
        }
        if (rc == Z_STREAM_END)
            break;
        TASK_CHECK(rc == Z_OK || rc == Z_BUF_ERROR);
        if (rc == Z_BUF_ERROR && orig.avail_in == 0)
            break;
    }
    TASK_CHECK(rc == Z_STREAM_END);
    task_coverage.finish_original_first++;
    /* the original continues after the copy instant: its new bytes are the
     * model plaintext suffix, and pre + suffix reconstructs everything */
    TASK_CHECK(got_orig.len == plain.len - pre.len);
    TASK_CHECK(memcmp(got_orig.p, plain.p + pre.len, got_orig.len) == 0);
    rc = inflateEnd(&orig);
    TASK_CHECK(rc == Z_OK);

    /* then continue the copy: it owns the same internal decoding state but
     * caller-owned rebound stores, and must reach the identical suffix */
    for (;;) {
        rc = inflate(&cp, Z_NO_FLUSH);
        size_t made = cap_b - cp.avail_out;
        if (made)
            task_buf_append(&got_copy, out_b, made);
        cp.next_out = out_b;
        cp.avail_out = (uInt)cap_b;
        if (rc == Z_NEED_DICT) {
            rc = inflateSetDictionary(&cp, dict.p, (uInt)dict.len);
            TASK_CHECK(rc == Z_OK);
            continue;
        }
        if (rc == Z_STREAM_END)
            break;
        TASK_CHECK(rc == Z_OK || rc == Z_BUF_ERROR);
        if (rc == Z_BUF_ERROR && cp.avail_in == 0)
            break;
    }
    TASK_CHECK(rc == Z_STREAM_END);
    TASK_CHECK(got_copy.len == plain.len - pre.len);
    TASK_CHECK(memcmp(got_copy.p, plain.p + pre.len, got_copy.len) == 0);
    rc = inflateEnd(&cp);
    TASK_CHECK(rc == Z_OK);

    free(out_a);
    free(out_b);
    task_buf_free(&plain);
    task_buf_free(&dict);
    task_buf_free(&comp);
    task_buf_free(&instore);
    task_buf_free(&cpstore);
    task_buf_free(&pre);
    task_buf_free(&got_orig);
    task_buf_free(&got_copy);
}
