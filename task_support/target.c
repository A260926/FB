/*
 * target.c - replayable property tests driving the real zlib API.
 *
 * The generator and the independent expectations live in model.c (which is
 * compiled without any zlib header).  This file is the only translation unit
 * that touches zlib: it interprets a bounded Program through deflateInit2,
 * deflate, SYNC_FLUSH/FULL_FLUSH, deflateCopy, inflateInit2, inflate,
 * inflateCopy and the matching End calls, then checks every result against
 * the model.  No corrupted input is ever fed to inflate: the only compressed
 * bytes used are ones this same engine produced from model plaintext.
 */
#include "check.h"
#include "model.h"
#include "zlib.h"

#include <string.h>
#include <stdlib.h>

#define TASK_SCRATCH 256

/* ------------------------------------------------------------------ */
/* Instrumented allocator: proves every zlib allocation is released    */
/* ------------------------------------------------------------------ */

static unsigned long z_live;
static unsigned long z_peak;

static voidpf task_zalloc(voidpf opaque, unsigned items, unsigned size)
{
    size_t n = (size_t)items * (size_t)size;
    void *p;
    (void)opaque;
    p = malloc(n ? n : 1);
    if (p != NULL) {
        z_live++;
        if (z_live > z_peak)
            z_peak = z_live;
    }
    return p;
}

static void task_zfree(voidpf opaque, voidpf addr)
{
    (void)opaque;
    if (addr != NULL) {
        TASK_CHECK(z_live > 0);
        z_live--;
        free(addr);
    }
}

/* ------------------------------------------------------------------ */
/* Engine runtime slots: caller-owned storage kept outside zlib        */
/* ------------------------------------------------------------------ */

typedef struct {
    int used;
    int kind;               /* 0 = deflate, 1 = inflate */
    int ended;              /* FINISH / IFINISH observed */
    int stream_end;         /* library already reported Z_STREAM_END */
    int outid;
    int copied_to;          /* inflate: slot this one was copied into, or -1 */
    int ever_out_full;      /* an output buffer ever came back completely full */
    z_stream z;
    Buf in;                 /* deflate: caller-owned pending plaintext */
    Buf out;                /* caller-owned collected output bytes */
    Buf inbuf;              /* inflate: private copy of the compressed bytes */
    size_t pos;             /* inflate: cursor into inbuf */
    unsigned char scratch[TASK_SCRATCH];
    size_t scratch_cap;
} Slot;

typedef struct {
    Program *pg;
    Slot d[TASK_MAX_STREAMS];
    Slot i[TASK_MAX_STREAMS];
    Buf comp[TASK_MAX_ACTIONS];
    Coverage *cov;
    int mutate;             /* predetermined oracle fault injection */
} Engine;

static void slot_init(Slot *s, int kind)
{
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->outid = -1;
    s->copied_to = -1;
    buf_init(&s->in);
    buf_init(&s->out);
    buf_init(&s->inbuf);
    s->scratch_cap = TASK_SCRATCH;
}

static void engine_init(Engine *e, Program *pg, Coverage *cov, int mutate)
{
    int k;
    memset(e, 0, sizeof(*e));
    e->pg = pg;
    e->cov = cov;
    e->mutate = mutate;
    for (k = 0; k < TASK_MAX_STREAMS; k++) {
        slot_init(&e->d[k], 0);
        slot_init(&e->i[k], 1);
    }
    for (k = 0; k < TASK_MAX_ACTIONS; k++)
        buf_init(&e->comp[k]);
}

static void engine_free(Engine *e)
{
    int k;
    for (k = 0; k < TASK_MAX_STREAMS; k++) {
        buf_free(&e->d[k].in);
        buf_free(&e->d[k].out);
        buf_free(&e->d[k].inbuf);
        buf_free(&e->i[k].in);
        buf_free(&e->i[k].out);
        buf_free(&e->i[k].inbuf);
    }
    for (k = 0; k < TASK_MAX_ACTIONS; k++)
        buf_free(&e->comp[k]);
}

/* Bind a fresh caller-owned output scratch before every zlib call. */
static void bind_out(Slot *s, size_t cap)
{
    if (cap == 0 || cap > TASK_SCRATCH)
        cap = TASK_SCRATCH;
    s->z.next_out = s->scratch;
    s->z.avail_out = (uInt)cap;
    s->scratch_cap = cap;
}

static void collect_out(Slot *s)
{
    size_t made = s->scratch_cap - s->z.avail_out;
    if (made)
        buf_put(&s->out, s->scratch, made);
    if (s->z.avail_out == 0)
        s->ever_out_full = 1;
}

/* Remove the bytes deflate just consumed from the caller-owned input. */
static void consume_in(Slot *s, uInt before)
{
    size_t used = before - s->z.avail_in;
    if (used) {
        memmove(s->in.p, s->in.p + used, s->in.len - used);
        s->in.len -= used;
    }
}

/* Drain a deflate slot. flush is a real zlib flush constant. */
static int pump_deflate(Slot *s, int flush)
{
    int rc = Z_OK;
    for (;;) {
        uInt in_before;
        size_t out_before;
        /* Caller re-binds its own input/output storage on every call. */
        s->z.next_in = s->in.p;
        s->z.avail_in = (uInt)s->in.len;
        in_before = s->z.avail_in;
        bind_out(s, 64);
        out_before = s->out.len;
        rc = deflate(&s->z, flush);
        TASK_CHECK(rc == Z_OK || rc == Z_STREAM_END || rc == Z_BUF_ERROR);
        consume_in(s, in_before);
        collect_out(s);
        if (rc == Z_STREAM_END) {
            TASK_CHECK(s->in.len == 0);
            break;
        }
        if (flush != Z_FINISH) {
            /* With no input left, Z_BUF_ERROR only means "fully flushed". */
            if (s->in.len == 0 &&
                (rc == Z_BUF_ERROR || s->z.avail_out > 0))
                return Z_OK;
            /* No progress despite free space: legal streams never stall. */
            if (rc == Z_BUF_ERROR && s->z.avail_out > 0)
                TASK_CHECK(0);
        } else if (rc == Z_BUF_ERROR && s->in.len == 0 &&
                   s->out.len == out_before) {
            TASK_CHECK(0); /* a legal FINISH must keep making progress */
        }
    }
    return rc;
}

/* One bounded inflate call; resolves the dictionary precondition itself. */
static int inflate_step(Engine *e, Slot *s, uInt in_limit, uInt out_cap)
{
    Program *pg = e->pg;
    size_t remain = s->inbuf.len - s->pos;
    uInt n = in_limit;
    int rc;
    if (s->stream_end)
        return Z_STREAM_END;
    if ((size_t)n > remain)
        n = (uInt)remain;
    s->z.next_in = s->inbuf.p + s->pos;
    s->z.avail_in = n;
    bind_out(s, out_cap);
    rc = inflate(&s->z, Z_NO_FLUSH);
    s->pos += n - s->z.avail_in;
    collect_out(s);
    if (rc == Z_NEED_DICT) {
        int did = pg->outdict[s->outid];
        TASK_CHECK(did >= 0);
        /* The library names the dictionary by Adler-32; the model knows it. */
        TASK_CHECK(s->z.adler ==
                   model_adler32(pg->dict[did].p, pg->dict[did].len));
        TASK_CHECK(inflateSetDictionary(&s->z, pg->dict[did].p,
                                        (uInt)pg->dict[did].len) == Z_OK);
        e->cov->dict_zlib_need++;
    } else {
        TASK_CHECK(rc == Z_OK || rc == Z_STREAM_END || rc == Z_BUF_ERROR);
    }
    if (rc == Z_STREAM_END)
        s->stream_end = 1;
    return rc;
}

/* Drive an inflate slot to Z_STREAM_END with bounded caller buffers. */
static void inflate_drain(Engine *e, Slot *s)
{
    int guard = 0;
    while (!s->stream_end) {
        size_t out_before = s->out.len;
        int rc = inflate_step(e, s, 64, 128);
        if (rc == Z_STREAM_END)
            return;
        if (rc == Z_BUF_ERROR && s->pos == s->inbuf.len &&
            s->out.len == out_before)
            TASK_CHECK(0); /* complete, well-formed input must not stall */
        if (++guard > 100000)
            TASK_CHECK(0);
    }
}

/* ------------------------------------------------------------------ */
/* Action interpretation                                               */
/* ------------------------------------------------------------------ */

/* The generator assigns output ids in DINIT order; mirror that ordering. */
static int next_outid;

static void do_dinit(Engine *e, const Action *a)
{
    Slot *s = &e->d[a->a];
    Program *pg = e->pg;
    int id = next_outid++, rc;
    TASK_CHECK(!s->used);
    slot_init(s, 0);
    s->used = 1;
    s->outid = id;
    memset(&s->z, 0, sizeof(s->z));
    s->z.zalloc = task_zalloc;
    s->z.zfree = task_zfree;
    rc = deflateInit2(&s->z, a->b, Z_DEFLATED, pg->wrapper[id], a->c, a->d);
    TASK_CHECK(rc == Z_OK);
    e->cov->def_streams++;
    if (pg->wrapper[id] < 0)
        e->cov->raw_programs++;
    else
        e->cov->zlib_programs++;
    /* Dictionary is set immediately after init, before any deflate call. */
    if (a->e >= 0) {
        rc = deflateSetDictionary(&s->z, pg->dict[a->e].p,
                                  (uInt)pg->dict[a->e].len);
        TASK_CHECK(rc == Z_OK);
    }
}

static void do_feed(Engine *e, const Action *a)
{
    Slot *s = &e->d[a->a];
    unsigned rounds = 3; /* deliberately leave part of the input pending */
    TASK_CHECK(s->used && !s->ended);
    buf_put(&s->in, a->data.p, a->data.len);
    while (s->in.len && rounds--) {
        uInt n = s->in.len < 32 ? (uInt)s->in.len : 32;
        uInt before;
        s->z.next_in = s->in.p;
        s->z.avail_in = n;
        before = n;
        bind_out(s, 64);
        TASK_CHECK(deflate(&s->z, Z_NO_FLUSH) != Z_STREAM_ERROR);
        consume_in(s, before);
        collect_out(s);
    }
}

static void do_flush(Engine *e, const Action *a)
{
    Slot *s = &e->d[a->a];
    TASK_CHECK(s->used && !s->ended);
    TASK_CHECK(pump_deflate(s, a->b) == Z_OK);
    if (a->b == Z_SYNC_FLUSH)
        e->cov->sync_flush++;
    else
        e->cov->full_flush++;
}

static void do_dcopy(Engine *e, const Action *a)
{
    Slot *src = &e->d[a->a], *dst = &e->d[a->b];
    Program *pg = e->pg;
    int rc;
    TASK_CHECK(src->used && !src->ended && !dst->used);
    TASK_CHECK(a->outid >= 0 && a->outid < pg->noutputs);
    /* One bounded call first so partial caller state is real, not nominal. */
    if (src->in.len) {
        uInt n = src->in.len < 16 ? (uInt)src->in.len : 16;
        uInt before = n;
        src->z.next_in = src->in.p;
        src->z.avail_in = n;
        bind_out(src, 8);
        TASK_CHECK(deflate(&src->z, Z_NO_FLUSH) != Z_STREAM_ERROR);
        consume_in(src, before);
        collect_out(src);
    }
    if (src->in.len)
        e->cov->dcopy_partial_in++;
    if (src->ever_out_full)
        e->cov->dcopy_partial_out++;

    slot_init(dst, 0);
    dst->used = 1;
    /* A fork creates the next output identity, mirroring the generator. */
    TASK_CHECK(a->outid == next_outid);
    dst->outid = a->outid;
    next_outid++;
    /* Caller re-binds independent input/output storage on the copy. */
    buf_put(&dst->in, src->in.p, src->in.len);
    buf_put(&dst->out, src->out.p, src->out.len);
    memset(&dst->z, 0, sizeof(dst->z));
    dst->z.zalloc = task_zalloc;
    dst->z.zfree = task_zfree;
    rc = deflateCopy(&dst->z, &src->z); /* library owns window/internal state */
    TASK_CHECK(rc == Z_OK);
    /* The two copies must never share caller-owned output/input storage.
       Empty buffers carry no storage, so NULL==NULL is harmless. */
    TASK_CHECK(dst->scratch != src->scratch);
    if (dst->out.len && src->out.len)
        TASK_CHECK(dst->out.p != src->out.p);
    if (dst->in.len && src->in.len)
        TASK_CHECK(dst->in.p != src->in.p);
    e->cov->dcopy++;
    e->cov->branch_suffix++;
    /* The model's prefix length stays within the plaintext it recorded. */
    TASK_CHECK((size_t)a->e <= pg->expect[src->outid].len);
}

static void do_finish(Engine *e, const Action *a)
{
    Slot *s = &e->d[a->a];
    TASK_CHECK(s->used && !s->ended);
    TASK_CHECK(pump_deflate(s, Z_FINISH) == Z_STREAM_END);
    s->ended = 1;
    TASK_CHECK(a->outid == s->outid);
    buf_put(&e->comp[s->outid], s->out.p, s->out.len);
}

static void do_dend(Engine *e, const Action *a)
{
    Slot *s = &e->d[a->a];
    TASK_CHECK(s->used && s->ended);
    TASK_CHECK(deflateEnd(&s->z) == Z_OK);
    buf_free(&s->in);
    buf_free(&s->out);
    s->used = 0;
    e->cov->end_ok++;
}

static void do_iinit(Engine *e, const Action *a)
{
    Slot *s = &e->i[a->a];
    Program *pg = e->pg;
    int rc;
    TASK_CHECK(!s->used);
    TASK_CHECK(e->comp[a->b].len > 0);
    slot_init(s, 1);
    s->used = 1;
    s->outid = a->b;
    /* Every inflate stream gets its own caller-owned copy of the input. */
    buf_put(&s->inbuf, e->comp[a->b].p, e->comp[a->b].len);
    memset(&s->z, 0, sizeof(s->z));
    s->z.zalloc = task_zalloc;
    s->z.zfree = task_zfree;
    rc = inflateInit2(&s->z, pg->wrapper[a->b]);
    TASK_CHECK(rc == Z_OK);
    e->cov->inf_streams++;
    if (pg->wrapper[a->b] < 0 && a->e >= 0) {
        /* Raw inflate accepts a dictionary at any time; prime it up front. */
        rc = inflateSetDictionary(&s->z, pg->dict[a->e].p,
                                  (uInt)pg->dict[a->e].len);
        TASK_CHECK(rc == Z_OK);
        e->cov->dict_raw++;
    }
}

static void do_ifeed(Engine *e, const Action *a)
{
    Slot *s = &e->i[a->a];
    TASK_CHECK(s->used && !s->ended);
    (void)inflate_step(e, s, (uInt)a->b, (uInt)a->c);
}

static void do_icopy(Engine *e, const Action *a)
{
    Slot *src = &e->i[a->a], *dst = &e->i[a->b];
    int rc;
    TASK_CHECK(src->used && !src->ended && !dst->used);
    if (src->pos < src->inbuf.len)
        e->cov->icopy_partial_in++;
    if (src->ever_out_full)
        e->cov->icopy_partial_out++;

    slot_init(dst, 1);
    dst->used = 1;
    dst->outid = src->outid;
    /* Re-bind independent caller storage: duplicated input, separate output. */
    buf_put(&dst->inbuf, src->inbuf.p, src->inbuf.len);
    dst->pos = src->pos;
    buf_put(&dst->out, src->out.p, src->out.len);
    memset(&dst->z, 0, sizeof(dst->z));
    dst->z.zalloc = task_zalloc;
    dst->z.zfree = task_zfree;
    rc = inflateCopy(&dst->z, &src->z);
    TASK_CHECK(rc == Z_OK);
    TASK_CHECK(dst->scratch != src->scratch);
    if (dst->out.len && src->out.len)
        TASK_CHECK(dst->out.p != src->out.p);
    if (dst->inbuf.len && src->inbuf.len)
        TASK_CHECK(dst->inbuf.p != src->inbuf.p);
    /* The decompressed prefix already produced must match byte for byte. */
    TASK_CHECK(buf_equal(&dst->out, &src->out));
    src->copied_to = a->b;
    e->cov->icopy++;
}

static void do_ifinish(Engine *e, const Action *a, int seed)
{
    Slot *s = &e->i[a->a];
    Program *pg = e->pg;
    const Buf *want = &pg->expect[s->outid];
    TASK_CHECK(s->used && !s->ended);
    inflate_drain(e, s);
    TASK_CHECK(s->stream_end);
    if (s->copied_to >= 0 && !e->i[s->copied_to].ended)
        e->cov->end_original_first++;
    s->ended = 1;
    /* Independent oracle: model plaintext vs. real decompressed bytes. */
    if (e->mutate) {
        /*
         * Predetermined logic-difference simulator: corrupt only the oracle's
         * expected first byte.  Library and upstream code are untouched; the
         * check must fire and stop on the first failing case.
         */
        Buf poisoned;
        buf_init(&poisoned);
        buf_put(&poisoned, want->p, want->len);
        poisoned.p[0] ^= 0x01;
        fprintf(stderr, "mutation: expecting oracle failure at seed %d\n",
                seed);
        TASK_CHECK(buf_equal(&s->out, &poisoned));
        buf_free(&poisoned);
    } else {
        TASK_CHECK(s->out.len == want->len);
        TASK_CHECK(buf_equal(&s->out, want));
        TASK_CHECK(model_fnv64(s->out.p, s->out.len) ==
                   model_fnv64(want->p, want->len));
    }
    /* The library's checksum must equal the independently computed Adler-32. */
    if (pg->wrapper[s->outid] > 0)
        TASK_CHECK(s->z.adler == model_adler32(want->p, want->len));
}

static void do_iend(Engine *e, const Action *a)
{
    Slot *s = &e->i[a->a];
    TASK_CHECK(s->used && s->ended);
    TASK_CHECK(inflateEnd(&s->z) == Z_OK);
    buf_free(&s->inbuf);
    buf_free(&s->out);
    s->used = 0;
    e->cov->end_ok++;
}

/* ------------------------------------------------------------------ */
/* Whole-program execution                                             */
/* ------------------------------------------------------------------ */

static void run_program(Program *pg, Coverage *cov, int mutate, int seed,
                        unsigned long long *comp_hash)
{
    Engine e;
    unsigned long long h = 0xcbf29ce484222325ULL;
    int k;
    engine_init(&e, pg, cov, mutate);
    next_outid = 0;
    cov->programs++;
    for (k = 0; k < pg->nact; k++) {
        const Action *a = &pg->act[k];
        switch (a->op) {
        case OP_DINIT:   do_dinit(&e, a); break;
        case OP_FEED:    do_feed(&e, a); break;
        case OP_FLUSH:   do_flush(&e, a); break;
        case OP_DCOPY:   do_dcopy(&e, a); break;
        case OP_FINISH:  do_finish(&e, a); break;
        case OP_DEND:    do_dend(&e, a); break;
        case OP_IINIT:   do_iinit(&e, a); break;
        case OP_IFEED:   do_ifeed(&e, a); break;
        case OP_ICOPY:   do_icopy(&e, a); break;
        case OP_IFINISH: do_ifinish(&e, a, seed); break;
        case OP_IEND:    do_iend(&e, a); break;
        default: TASK_CHECK(0);
        }
    }
    for (k = 0; k < TASK_MAX_STREAMS; k++) {
        TASK_CHECK(!e.d[k].used);
        TASK_CHECK(!e.i[k].used);
    }
    TASK_CHECK(z_live == 0); /* every library allocation released */
    for (k = 0; k < pg->noutputs; k++) {
        unsigned long long one = model_fnv64(e.comp[k].p, e.comp[k].len);
        h ^= one;
        h *= 1099511628211ULL;
    }
    *comp_hash = h;
    engine_free(&e);
}

/* Independent structural hash of a program (replay determinism). */
static unsigned long long program_hash(const Program *pg)
{
    unsigned long long h = 0xcbf29ce484222325ULL;
    int k;
    for (k = 0; k < pg->nact; k++) {
        const Action *a = &pg->act[k];
        h ^= (unsigned long long)a->op * 0x9E3779B97F4A7C15ULL;
        h *= 1099511628211ULL;
        h ^= model_fnv64(a->data.p, a->data.len);
        h *= 1099511628211ULL;
        h ^= (unsigned long long)(unsigned)a->a +
             ((unsigned long long)(unsigned)a->b << 8) +
             ((unsigned long long)(unsigned)a->c << 16) +
             ((unsigned long long)(unsigned)a->outid << 24);
        h *= 1099511628211ULL;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Group entry points                                                  */
/* ------------------------------------------------------------------ */

static int mutate_requested(void)
{
    const char *v = getenv("TASK_MUTATE");
    return v != NULL && v[0] == '1';
}

int task_cases(const char *group)
{
    Coverage cov;
    int seed, mutate = mutate_requested();
    coverage_zero(&cov);

    if (strcmp(group, "model") == 0) {
        /* Fixed vectors for the independent checksum/hash primitives. */
        static const unsigned char wiki[] = "Wikipedia";
        static const unsigned char one[] = "a";
        TASK_CHECK(model_adler32(NULL, 0) == 1UL);
        TASK_CHECK(model_adler32(wiki, 9) == 0x11E60398UL);
        TASK_CHECK(model_fnv64(NULL, 0) == 0xcbf29ce484222325ULL);
        TASK_CHECK(model_fnv64(one, 1) == 0xaf63dc4c8601ec8cULL);

        for (seed = 0; seed < TASK_SEED_COUNT; seed++) {
            Program pg, again;
            unsigned long long h1;
            program_init(&pg);
            program_init(&again);
            program_generate(&pg, seed);
            TASK_CHECK(program_legal(&pg));
            TASK_CHECK(pg.nact >= 1 && pg.nact <= TASK_MAX_ACTIONS);
            TASK_CHECK(pg.total_input <= TASK_MAX_INPUT);
            TASK_CHECK(pg.noutputs >= 1);
            /* Same seed must regenerate a byte-identical program. */
            program_generate(&again, seed);
            TASK_CHECK(again.nact == pg.nact);
            TASK_CHECK(program_hash(&again) == program_hash(&pg));
            /*
             * Driving the real library must satisfy the model; inside the
             * engine the zlib-wrapper Adler-32 is compared against the model's
             * independent computation for every decoded stream.
             */
            run_program(&pg, &cov, mutate, seed, &h1);
            program_free(&pg);
            program_free(&again);
        }
        TASK_CHECK(cov.def_streams >= TASK_SEED_COUNT);
        TASK_CHECK(cov.inf_streams >= TASK_SEED_COUNT);
        return 0;
    }

    if (strcmp(group, "metamorphic") == 0) {
        int saw_branch = 0, saw_icopy = 0, saw_sync = 0, saw_full = 0;
        int saw_raw = 0, saw_zlib = 0, saw_dict = 0;
        for (seed = 0; seed < TASK_SEED_COUNT; seed++) {
            Program pg;
            unsigned long long h;
            int k;
            program_init(&pg);
            program_generate(&pg, seed);
            TASK_CHECK(program_legal(&pg));
            for (k = 0; k < pg.nact; k++) {
                if (pg.act[k].op == OP_FLUSH && pg.act[k].b == 2)
                    saw_sync = 1;
                if (pg.act[k].op == OP_FLUSH && pg.act[k].b == 3)
                    saw_full = 1;
                if (pg.act[k].op == OP_DCOPY) {
                    const Action *cp = &pg.act[k];
                    Buf pre0, pre1;
                    saw_branch = 1;
                    /*
                     * Metamorphic relation on the independent model: after a
                     * fork both output identities carry the same plaintext
                     * prefix (length cp->e); each branch then grows its own
                     * suffix.  The engine separately proves each compressed
                     * output decodes back to its own branch plaintext.
                     */
                    buf_init(&pre0);
                    buf_init(&pre1);
                    TASK_CHECK(cp->d >= 0 && cp->d < pg.noutputs);
                    TASK_CHECK(cp->outid >= 1 && cp->outid < pg.noutputs);
                    TASK_CHECK((size_t)cp->e <= pg.expect[cp->d].len);
                    TASK_CHECK((size_t)cp->e <= pg.expect[cp->outid].len);
                    buf_put(&pre0, pg.expect[cp->d].p, (size_t)cp->e);
                    buf_put(&pre1, pg.expect[cp->outid].p, (size_t)cp->e);
                    TASK_CHECK(buf_equal(&pre0, &pre1));
                    buf_free(&pre0);
                    buf_free(&pre1);
                }
                if (pg.act[k].op == OP_ICOPY) saw_icopy = 1;
            }
            for (k = 0; k < pg.noutputs; k++) {
                if (pg.wrapper[k] < 0) saw_raw = 1; else saw_zlib = 1;
                if (pg.outdict[k] >= 0) saw_dict = 1;
            }
            run_program(&pg, &cov, mutate, seed, &h);
            program_free(&pg);
        }
        /* Grammar-level coverage of every required transformation path. */
        TASK_CHECK(saw_branch);
        TASK_CHECK(saw_icopy);
        TASK_CHECK(saw_sync);
        TASK_CHECK(saw_full);
        TASK_CHECK(saw_raw && saw_zlib);
        TASK_CHECK(saw_dict);
        /* Real engine coverage of both copies and partial caller state. */
        TASK_CHECK(cov.dcopy > 0 && cov.icopy > 0);
        TASK_CHECK(cov.dcopy_partial_in > 0 && cov.dcopy_partial_out > 0);
        TASK_CHECK(cov.icopy_partial_in > 0 && cov.icopy_partial_out > 0);
        TASK_CHECK(cov.end_original_first > 0);
        TASK_CHECK(cov.branch_suffix == cov.dcopy);
        return 0;
    }

    if (strcmp(group, "replay") == 0) {
        for (seed = 0; seed < TASK_SEED_COUNT; seed++) {
            Program pg;
            unsigned long long h1, h2;
            program_init(&pg);
            program_generate(&pg, seed);
            run_program(&pg, &cov, 0, seed, &h1);
            /* Fresh engine, same seed: no reliance on prior addresses/state. */
            run_program(&pg, &cov, 0, seed, &h2);
            TASK_CHECK(h1 == h2);
            cov.replays++;
            program_free(&pg);
        }
        TASK_CHECK(cov.programs == 2U * TASK_SEED_COUNT);
        TASK_CHECK(cov.replays == TASK_SEED_COUNT);
        TASK_CHECK(z_live == 0);
        TASK_CHECK(z_peak > 0);
        cov.alloc_peak = z_peak;
        TASK_CHECK(cov.end_ok >= 2U * TASK_SEED_COUNT);
        coverage_print(&cov, stdout);
        return 0;
    }

    return 1;
}
