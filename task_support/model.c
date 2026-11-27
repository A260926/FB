/*
 * model.c - independent reference model implementation.
 *
 * This translation unit never includes zlib.h (the #error below makes any
 * accidental dependency a build failure).  It generates bounded, legal
 * lifecycle programs and remembers the expected plaintext, but contains no
 * compression code and cannot call deflateCopy/inflateCopy: the oracle in
 * target.c compares real library behaviour against these expectations.
 */
#include "model.h"

#include <stdlib.h>
#include <string.h>

#if defined(ZLIB_H) || defined(ZLIB_VERSION)
#error "the independent model must not depend on zlib headers"
#endif

/* Numeric values intentionally match zlib's flush/strategy constants. */
#define MODEL_SYNC_FLUSH 2
#define MODEL_FULL_FLUSH 3
#define Z_DEFAULT_STRATEGY_NUM 0
#define Z_FILTERED_NUM 1
#define Z_HUFFMAN_ONLY_NUM 2
#define Z_RLE_NUM 3
#define Z_FIXED_NUM 4

/* ------------------------------------------------------------------ */
/* Byte buffers                                                        */
/* ------------------------------------------------------------------ */

void buf_init(Buf *b)
{
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b)
{
    free(b->p);
    buf_init(b);
}

void buf_reset(Buf *b)
{
    b->len = 0;
}

void buf_put(Buf *b, const void *data, size_t n)
{
    if (n == 0)
        return;
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap : 16;
        while (ncap < b->len + n)
            ncap *= 2;
        unsigned char *np = (unsigned char *)realloc(b->p, ncap);
        if (np == NULL) {
            fprintf(stderr, "model: out of memory\n");
            exit(2);
        }
        b->p = np;
        b->cap = ncap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

void buf_put_byte(Buf *b, unsigned char c)
{
    buf_put(b, &c, 1);
}

int buf_equal(const Buf *a, const Buf *b)
{
    return a->len == b->len && (a->len == 0 || memcmp(a->p, b->p, a->len) == 0);
}

/* ------------------------------------------------------------------ */
/* Deterministic PRNG (splitmix64)                                     */
/* ------------------------------------------------------------------ */

void rng_seed(Rng *r, unsigned long long seed)
{
    r->s = seed;
}

unsigned long long rng_next(Rng *r)
{
    unsigned long long z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

unsigned rng_between(Rng *r, unsigned lo, unsigned hi)
{
    return lo + (unsigned)(rng_next(r) % (unsigned long long)(hi - lo + 1));
}

unsigned char rng_byte(Rng *r)
{
    return (unsigned char)(rng_next(r) >> 33);
}

/* ------------------------------------------------------------------ */
/* Independent Adler-32 (RFC 1950 section 9, appendix)                 */
/* ------------------------------------------------------------------ */

unsigned long model_adler32(const unsigned char *p, size_t n)
{
    const unsigned long MOD = 65521UL;
    unsigned long s1 = 1, s2 = 0;
    size_t i;
    /* NMAX block bound from the RFC keeps the sums within range. */
    for (i = 0; i < n; i++) {
        s1 += p[i];
        s2 += s1;
        if ((i & 5551) == 5551) {
            s1 %= MOD;
            s2 %= MOD;
        }
    }
    s1 %= MOD;
    s2 %= MOD;
    return (s2 << 16) | s1;
}

/* Independent 64-bit FNV-1a for replay byte identity. */
unsigned long long model_fnv64(const unsigned char *p, size_t n)
{
    unsigned long long h = 0xcbf29ce484222325ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Program lifecycle                                                   */
/* ------------------------------------------------------------------ */

void program_init(Program *pg)
{
    int i;
    memset(pg, 0, sizeof(*pg));
    for (i = 0; i < TASK_MAX_ACTIONS; i++)
        buf_init(&pg->act[i].data);
    for (i = 0; i < TASK_DICT_MAX; i++)
        buf_init(&pg->dict[i]);
    for (i = 0; i < TASK_MAX_ACTIONS; i++) {
        buf_init(&pg->expect[i]);
        pg->wrapper[i] = 0;
        pg->outdict[i] = -1;
    }
}

void program_free(Program *pg)
{
    int i;
    for (i = 0; i < TASK_MAX_ACTIONS; i++)
        buf_free(&pg->act[i].data);
    for (i = 0; i < TASK_DICT_MAX; i++)
        buf_free(&pg->dict[i]);
    for (i = 0; i < TASK_MAX_ACTIONS; i++)
        buf_free(&pg->expect[i]);
    pg->nact = 0;
    pg->ndict = 0;
    pg->noutputs = 0;
    pg->total_input = 0;
}

/* ------------------------------------------------------------------ */
/* Generator state                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    Rng rng;
    int dslot[TASK_MAX_STREAMS];  /* output id per deflate slot, else -1 */
    int islot[TASK_MAX_STREAMS];  /* output id decoded by each inflate slot */
    int dended[TASK_MAX_STREAMS]; /* FINISH seen, slot waits for DEND */
    int dlist[TASK_MAX_STREAMS], nd;
    int ilist[TASK_MAX_STREAMS], ni;
    int iended[TASK_MAX_STREAMS];
    int noutputs;
    int outdone[TASK_MAX_ACTIONS]; /* output id reached FINISH */
    size_t total;
    int saw_dcopy, saw_icopy, saw_sync, saw_full, saw_dict, saw_raw;
} Gen;

static Action *new_action(Program *pg)
{
    Action *a;
    if (pg->nact >= TASK_MAX_ACTIONS)
        return NULL;
    a = &pg->act[pg->nact++];
    memset(a, 0, sizeof(*a));
    buf_init(&a->data);
    a->outid = -1;
    a->e = -1;
    return a;
}

static void gen_init(Gen *g)
{
    int i;
    memset(g, 0, sizeof(*g));
    for (i = 0; i < TASK_MAX_STREAMS; i++) {
        g->dslot[i] = -1;
        g->islot[i] = -1;
    }
}

static int free_slot(const int *map)
{
    int i;
    for (i = 0; i < TASK_MAX_STREAMS; i++)
        if (map[i] < 0)
            return i;
    return -1;
}

static int weighted_choice(Rng *r, const int *cand, const int *weight, int n)
{
    int sum = 0, i, roll;
    for (i = 0; i < n; i++)
        sum += weight[i];
    roll = (int)(rng_next(r) % (unsigned long long)sum);
    for (i = 0; i < n; i++) {
        roll -= weight[i];
        if (roll < 0)
            return cand[i];
    }
    return cand[n - 1];
}

static void gen_dictionaries(Program *pg, Gen *g)
{
    int i, k, j;
    pg->ndict = (int)rng_between(&g->rng, 1, TASK_DICT_MAX);
    for (i = 0; i < pg->ndict; i++) {
        k = (int)rng_between(&g->rng, 1, 64);
        for (j = 0; j < k; j++) {
            unsigned char c;
            /* Mix random and repetitive bytes so dictionaries can matter. */
            if ((rng_next(&g->rng) & 3) == 0)
                c = (unsigned char)('a' + rng_between(&g->rng, 0, 5));
            else
                c = rng_byte(&g->rng);
            buf_put_byte(&pg->dict[i], c);
        }
    }
}

static void gen_dinit(Program *pg, Gen *g)
{
    Action *a = new_action(pg);
    int id = g->noutputs, slot = free_slot(g->dslot), raw, wb, use_dict;
    if (a == NULL || slot < 0)
        return;
    a->op = OP_DINIT;
    a->a = slot;
    raw = (int)(rng_next(&g->rng) & 1);
    wb = (int)rng_between(&g->rng, 9, 15); /* 8 is remapped by zlib; avoid it */
    a->b = (int)rng_between(&g->rng, 0, 9);    /* level */
    a->c = (int)rng_between(&g->rng, 1, 9);    /* memLevel */
    a->d = (int)rng_between(&g->rng, Z_DEFAULT_STRATEGY_NUM, Z_FIXED_NUM);
    if (raw) {
        pg->wrapper[id] = -wb;
        g->saw_raw = 1;
    } else {
        pg->wrapper[id] = wb;
    }
    use_dict = (int)(rng_next(&g->rng) & 1);
    if (use_dict && pg->ndict > 0) {
        a->e = (int)rng_between(&g->rng, 0, (unsigned)pg->ndict - 1);
        g->saw_dict = 1;
    }
    pg->outdict[id] = a->e;
    g->dslot[slot] = id;
    g->dlist[g->nd++] = slot;
    g->noutputs = id + 1;
    pg->noutputs = g->noutputs;
}

static void gen_feed_n(Program *pg, Gen *g, int slot, unsigned n)
{
    Action *a;
    int id = g->dslot[slot];
    unsigned i;
    if (n == 0 || g->total >= TASK_MAX_INPUT)
        return;
    if ((size_t)n > TASK_MAX_INPUT - g->total)
        n = (unsigned)(TASK_MAX_INPUT - g->total);
    a = new_action(pg);
    if (a == NULL)
        return;
    a->op = OP_FEED;
    a->a = slot;
    for (i = 0; i < n; i++) {
        unsigned char c;
        if ((rng_next(&g->rng) & 7) == 0)
            c = (unsigned char)('A' + rng_between(&g->rng, 0, 7));
        else
            c = rng_byte(&g->rng);
        buf_put_byte(&a->data, c);
    }
    buf_put(&pg->expect[id], a->data.p, a->data.len);
    g->total += n;
    pg->total_input = g->total;
}

static void gen_flush(Program *pg, Gen *g, int full)
{
    Action *a;
    int slot, i, n = 0;
    int live[TASK_MAX_STREAMS];
    /* dlist keeps every active slot; filter locally, never mutate it here. */
    for (i = 0; i < g->nd; i++)
        if (!g->dended[g->dlist[i]])
            live[n++] = g->dlist[i];
    if (n == 0)
        return;
    slot = live[rng_between(&g->rng, 0, (unsigned)n - 1)];
    a = new_action(pg);
    if (a == NULL)
        return;
    a->op = OP_FLUSH;
    a->a = slot;
    a->b = full ? MODEL_FULL_FLUSH : MODEL_SYNC_FLUSH;
    if (full)
        g->saw_full = 1;
    else
        g->saw_sync = 1;
}

/*
 * deflateCopy fork: the model itself duplicates the caller-side plaintext
 * prefix into a new output identity (no zlib copy call is involved here).
 * Both branches keep independent caller storage from this point on.
 */
static void gen_dcopy(Program *pg, Gen *g)
{
    Action *a;
    int slot, dst, id, nid;
    int n = 0, i;
    int live[TASK_MAX_STREAMS];
    for (i = 0; i < g->nd; i++)
        if (!g->dended[g->dlist[i]])
            live[n++] = g->dlist[i];
    if (n == 0 || g->nd >= TASK_MAX_STREAMS)
        return;
    slot = live[rng_between(&g->rng, 0, (unsigned)n - 1)];
    dst = free_slot(g->dslot);
    if (dst < 0)
        return;
    id = g->dslot[slot];
    nid = g->noutputs;
    if (pg->expect[id].len == 0)
        return; /* a fork needs a real shared prefix to carry forward */
    a = new_action(pg);
    if (a == NULL)
        return;
    a->op = OP_DCOPY;
    a->a = slot;
    a->b = dst;
    a->d = id;
    a->outid = nid;
    /* Record the true shared plaintext prefix length at the fork. */
    a->e = (int)pg->expect[id].len;
    pg->wrapper[nid] = pg->wrapper[id];
    pg->outdict[nid] = pg->outdict[id];
    buf_put(&pg->expect[nid], pg->expect[id].p, pg->expect[id].len);
    g->dslot[dst] = nid;
    g->dlist[g->nd++] = dst;
    g->noutputs = nid + 1;
    pg->noutputs = g->noutputs;
    g->saw_dcopy = 1;
}

static void gen_finish_slot(Program *pg, Gen *g, int slot)
{
    Action *a;
    int id;
    if (slot < 0 || g->dslot[slot] < 0 || g->dended[slot])
        return;
    id = g->dslot[slot];
    a = new_action(pg);
    if (a == NULL)
        return;
    a->op = OP_FINISH;
    a->a = slot;
    a->outid = id;
    g->dended[slot] = 1;
    g->outdone[id] = 1;
}

static void gen_finish(Program *pg, Gen *g)
{
    int slot, i, n = 0;
    int live[TASK_MAX_STREAMS];
    for (i = 0; i < g->nd; i++)
        if (!g->dended[g->dlist[i]])
            live[n++] = g->dlist[i];
    if (n == 0)
        return;
    slot = live[rng_between(&g->rng, 0, (unsigned)n - 1)];
    gen_finish_slot(pg, g, slot);
}

static void gen_dend_slot(Program *pg, Gen *g, int slot)
{
    Action *a;
    int i;
    if (slot < 0 || g->dslot[slot] < 0 || !g->dended[slot])
        return; /* DEND is only legal after FINISH: output must be complete */
    a = new_action(pg);
    if (a == NULL)
        return;
    a->op = OP_DEND;
    a->a = slot;
    for (i = 0; i < g->nd; i++)
        if (g->dlist[i] == slot) {
            g->dlist[i] = g->dlist[--g->nd];
            break;
        }
    g->dslot[slot] = -1;
    g->dended[slot] = 0;
}

static void gen_dend(Program *pg, Gen *g)
{
    int slot, i, n = 0;
    int done[TASK_MAX_STREAMS];
    for (i = 0; i < g->nd; i++)
        if (g->dended[g->dlist[i]])
            done[n++] = g->dlist[i];
    if (n == 0)
        return;
    slot = done[rng_between(&g->rng, 0, (unsigned)n - 1)];
    gen_dend_slot(pg, g, slot);
}

static void gen_iinit(Program *pg, Gen *g)
{
    Action *a;
    int slot, id, n = 0;
    int done[TASK_MAX_ACTIONS];
    if (g->ni >= TASK_MAX_STREAMS)
        return;
    slot = free_slot(g->islot);
    if (slot < 0)
        return;
    /* An output is inflatable once its producing slot saw FINISH. */
    for (id = 0; id < g->noutputs; id++)
        if (g->outdone[id])
            done[n++] = id;
    if (n == 0)
        return;
    id = done[rng_between(&g->rng, 0, (unsigned)n - 1)];
    a = new_action(pg);
    if (a == NULL)
        return;
    a->op = OP_IINIT;
    a->a = slot;
    a->b = id;
    a->e = pg->outdict[id];
    g->islot[slot] = id;
    g->ilist[g->ni++] = slot;
}

static void gen_ifeed(Program *pg, Gen *g)
{
    Action *a;
    int slot, n = 0, i;
    int live[TASK_MAX_STREAMS];
    for (i = 0; i < g->ni; i++)
        if (!g->iended[g->ilist[i]])
            live[n++] = g->ilist[i];
    if (n == 0)
        return;
    slot = live[rng_between(&g->rng, 0, (unsigned)n - 1)];
    a = new_action(pg);
    if (a == NULL)
        return;
    a->op = OP_IFEED;
    a->a = slot;
    a->b = (int)rng_between(&g->rng, 1, 64);  /* compressed bytes per call */
    a->c = (int)rng_between(&g->rng, 1, 128); /* output buffer capacity */
}

static void gen_icopy(Program *pg, Gen *g)
{
    Action *a;
    int slot, dst, n = 0, i;
    int live[TASK_MAX_STREAMS];
    for (i = 0; i < g->ni; i++)
        if (!g->iended[g->ilist[i]])
            live[n++] = g->ilist[i];
    if (n == 0 || g->ni >= TASK_MAX_STREAMS)
        return;
    slot = live[rng_between(&g->rng, 0, (unsigned)n - 1)];
    dst = free_slot(g->islot);
    if (dst < 0)
        return;
    a = new_action(pg);
    if (a == NULL)
        return;
    a->op = OP_ICOPY;
    a->a = slot;
    a->b = dst;
    g->islot[dst] = g->islot[slot];
    g->ilist[g->ni++] = dst;
    g->saw_icopy = 1;
}

static void gen_ifinish(Program *pg, Gen *g)
{
    Action *a;
    int slot, n = 0, i;
    int live[TASK_MAX_STREAMS];
    for (i = 0; i < g->ni; i++)
        if (!g->iended[g->ilist[i]])
            live[n++] = g->ilist[i];
    if (n == 0)
        return;
    slot = live[rng_between(&g->rng, 0, (unsigned)n - 1)];
    a = new_action(pg);
    if (a == NULL)
        return;
    a->op = OP_IFINISH;
    a->a = slot;
    g->iended[slot] = 1;
}

static void gen_iend(Program *pg, Gen *g, int slot)
{
    Action *a;
    int i;
    /* IEND is only legal after IFINISH: the full decode must complete first. */
    if (slot < 0 || g->islot[slot] < 0 || !g->iended[slot])
        return;
    a = new_action(pg);
    if (a == NULL)
        return;
    a->op = OP_IEND;
    a->a = slot;
    for (i = 0; i < g->ni; i++)
        if (g->ilist[i] == slot) {
            g->ilist[i] = g->ilist[--g->ni];
            break;
        }
    g->islot[slot] = -1;
    g->iended[slot] = 0;
}

/* Outputs become inflatable once they have seen FINISH. */
static int output_finished(const Gen *g, int id)
{
    return id >= 0 && id < g->noutputs && g->outdone[id];
}

/* ------------------------------------------------------------------ */
/* Deterministic scenario blocks (the required metamorphic shapes)     */
/* ------------------------------------------------------------------ */

/* Deterministic inflate lifecycle for one finished output (bounded buffers). */
static void gen_inflate_output(Program *pg, Gen *g, int id, int feeds)
{
    Action *a;
    int slot = free_slot(g->islot), i;
    if (slot < 0)
        return;
    a = new_action(pg);
    if (a == NULL) return;
    a->op = OP_IINIT; a->a = slot; a->b = id; a->e = pg->outdict[id];
    g->islot[slot] = id; g->ilist[g->ni++] = slot;
    for (i = 0; i < feeds; i++)
        gen_ifeed(pg, g);
    a = new_action(pg);
    if (a != NULL) { a->op = OP_IFINISH; a->a = slot; g->iended[slot] = 1; }
    gen_iend(pg, g, slot);
}

/* Same prefix, deflateCopy, two distinct suffixes, independent decode. */
static void scenario_branch_suffix(Program *pg, Gen *g)
{
    unsigned p1 = rng_between(&g->rng, 256, 4096);
    unsigned sa = rng_between(&g->rng, 128, 2048);
    unsigned sb = rng_between(&g->rng, 128, 2048);
    gen_feed_n(pg, g, 0, p1);
    if ((rng_next(&g->rng) & 1))
        gen_flush(pg, g, (int)(rng_next(&g->rng) & 1));
    gen_dcopy(pg, g);                 /* slot 0 -> slot 1 */
    gen_feed_n(pg, g, 0, sa);
    gen_feed_n(pg, g, 1, sb);
    gen_finish_slot(pg, g, 0);
    gen_finish_slot(pg, g, 1);
    gen_dend_slot(pg, g, 0);
    gen_dend_slot(pg, g, 1);
    /* Each branch output is decoded by its own independent inflate stream. */
    gen_inflate_output(pg, g, 0, 3);
    gen_inflate_output(pg, g, 1, 3);
}

/* Partial inflate with bounded buffers, inflateCopy, finish original first. */
static void scenario_inflate_copy(Program *pg, Gen *g)
{
    Action *a;
    int feeds = (int)rng_between(&g->rng, 2, 4), i;
    gen_feed_n(pg, g, 0, rng_between(&g->rng, 8192, 32768));
    gen_finish_slot(pg, g, 0);
    gen_dend_slot(pg, g, 0);
    a = new_action(pg); /* IINIT slot 0 -> output 0 */
    a->op = OP_IINIT; a->a = 0; a->b = 0; a->e = pg->outdict[0];
    g->islot[0] = 0; g->ilist[g->ni++] = 0;
    for (i = 0; i < feeds; i++)
        gen_ifeed(pg, g);             /* deliberately small in/out limits */
    a = new_action(pg);               /* ICOPY slot 0 -> slot 1 mid-decode */
    a->op = OP_ICOPY; a->a = 0; a->b = 1;
    g->islot[1] = 0; g->ilist[g->ni++] = 1;
    /* The original stream is driven to Z_STREAM_END before the copy. */
    a = new_action(pg); a->op = OP_IFINISH; a->a = 0; g->iended[0] = 1;
    a = new_action(pg); a->op = OP_IFINISH; a->a = 1; g->iended[1] = 1;
    gen_iend(pg, g, 0);
    gen_iend(pg, g, 1);
}

static void cleanup(Program *pg, Gen *g)
{
    int i;
    for (i = 0; i < TASK_MAX_STREAMS; i++) {
        if (g->dslot[i] >= 0) {
            gen_finish_slot(pg, g, i);
            gen_dend_slot(pg, g, i);
        }
    }
    for (i = 0; i < TASK_MAX_STREAMS; i++) {
        if (g->islot[i] >= 0) {
            if (!g->iended[i]) {
                Action *a = new_action(pg);
                if (a != NULL) {
                    a->op = OP_IFINISH;
                    a->a = i;
                    g->iended[i] = 1;
                }
            }
            gen_iend(pg, g, i);
        }
    }
}

void program_generate(Program *pg, int seed)
{
    Gen g;
    gen_init(&g);
    rng_seed(&g.rng, 0x9E3779B97F4A7C15ULL ^
             ((unsigned long long)(unsigned)seed * 0xD1B54A32D192ED03ULL)
             ^ (unsigned long long)(seed + 1));
    gen_dictionaries(pg, &g);

    /* First action is always a deflate stream: every program has a subject. */
    gen_dinit(pg, &g);

    if (seed % 3 == 0) {
        scenario_branch_suffix(pg, &g);
    } else if (seed % 3 == 1) {
        scenario_inflate_copy(pg, &g);
    } else {
        while (pg->nact < 48) {
            int cand[16], w[16], n = 0;
            int inflatable = 0, o, pick;
            int avail_dend = 0, avail_iend = 0, x;
            for (o = 0; o < g.noutputs; o++)
                if (output_finished(&g, o))
                    inflatable = 1;
            for (x = 0; x < g.nd; x++)
                if (g.dended[g.dlist[x]])
                    avail_dend = 1;
            for (x = 0; x < g.ni; x++)
                if (g.iended[g.ilist[x]])
                    avail_iend = 1;

            if (g.nd < TASK_MAX_STREAMS)
                { cand[n] = OP_DINIT; w[n] = 2; n++; }
            if (g.nd > 0 && g.total < TASK_MAX_INPUT)
                { cand[n] = OP_FEED; w[n] = 4; n++; }
            if (g.nd > 0)
                { cand[n] = OP_FLUSH; w[n] = 2; n++; }
            if (g.nd > 0 && g.nd < TASK_MAX_STREAMS &&
                (!g.saw_dcopy || (rng_next(&g.rng) & 3) == 0))
                { cand[n] = OP_DCOPY; w[n] = g.saw_dcopy ? 2 : 5; n++; }
            if (g.nd > 0)
                { cand[n] = OP_FINISH; w[n] = 2; n++; }
            if (avail_dend)
                { cand[n] = OP_DEND; w[n] = 1; n++; }
            if (inflatable && g.ni < TASK_MAX_STREAMS)
                { cand[n] = OP_IINIT; w[n] = 3; n++; }
            if (g.ni > 0)
                { cand[n] = OP_IFEED; w[n] = 4; n++; }
            if (g.ni > 0 && g.ni < TASK_MAX_STREAMS &&
                (!g.saw_icopy || (rng_next(&g.rng) & 3) == 0))
                { cand[n] = OP_ICOPY; w[n] = g.saw_icopy ? 2 : 5; n++; }
            if (g.ni > 0)
                { cand[n] = OP_IFINISH; w[n] = 2; n++; }
            if (avail_iend)
                { cand[n] = OP_IEND; w[n] = 1; n++; }

            pick = weighted_choice(&g.rng, cand, w, n);
            switch (pick) {
            case OP_DINIT:   gen_dinit(pg, &g); break;
            case OP_FEED: {
                int live[TASK_MAX_STREAMS], cnt = 0, j, k;
                for (j = 0; j < g.nd; j++)
                    if (!g.dended[g.dlist[j]])
                        live[cnt++] = g.dlist[j];
                if (cnt > 0) {
                    k = live[rng_between(&g.rng, 0, (unsigned)cnt - 1)];
                    gen_feed_n(pg, &g, k, rng_between(&g.rng, 64, 4096));
                }
                break;
            }
            case OP_FLUSH:   gen_flush(pg, &g, (int)(rng_next(&g.rng) & 1)); break;
            case OP_DCOPY:   gen_dcopy(pg, &g); break;
            case OP_FINISH:  gen_finish(pg, &g); break;
            case OP_DEND:    gen_dend(pg, &g); break;
            case OP_IINIT:   gen_iinit(pg, &g); break;
            case OP_IFEED:   gen_ifeed(pg, &g); break;
            case OP_ICOPY:   gen_icopy(pg, &g); break;
            case OP_IFINISH: gen_ifinish(pg, &g); break;
            case OP_IEND: {
                int done[TASK_MAX_STREAMS], cnt = 0, j, k;
                for (j = 0; j < g.ni; j++)
                    if (g.iended[g.ilist[j]])
                        done[cnt++] = g.ilist[j];
                if (cnt > 0) {
                    k = done[rng_between(&g.rng, 0, (unsigned)cnt - 1)];
                    gen_iend(pg, &g, k);
                }
                break;
            }
            default: break;
            }
        }
    }

    cleanup(pg, &g);
    pg->noutputs = g.noutputs;
}

/* ------------------------------------------------------------------ */
/* Independent legality check of the whole lifecycle state machine     */
/* ------------------------------------------------------------------ */

int program_legal(const Program *pg)
{
    int dslot[TASK_MAX_STREAMS], islot[TASK_MAX_STREAMS];
    int dended[TASK_MAX_STREAMS], iended[TASK_MAX_STREAMS];
    int outdone[TASK_MAX_ACTIONS];
    int nd = 0, ni = 0, noutputs = 0, i, j;
    size_t total = 0;
    /* The tag documents which API precondition rejected the transition. */
#define LEGAL_FAIL(tag) do { (void)(tag); return 0; } while (0)

    if (pg->nact < 1 || pg->nact > TASK_MAX_ACTIONS)
        return 0;
    if (pg->ndict < 0 || pg->ndict > TASK_DICT_MAX)
        return 0;
    for (i = 0; i < TASK_MAX_STREAMS; i++) {
        dslot[i] = -1;
        islot[i] = -1;
        dended[i] = 0;
        iended[i] = 0;
    }
    for (i = 0; i < TASK_MAX_ACTIONS; i++)
        outdone[i] = 0;

    for (i = 0; i < pg->nact; i++) {
        const Action *a = &pg->act[i];
        int slot, dst, id;
        switch (a->op) {
        case OP_DINIT:
            if (nd >= TASK_MAX_STREAMS) LEGAL_FAIL("dinit-cap");
            slot = a->a;
            if (slot < 0 || slot >= TASK_MAX_STREAMS || dslot[slot] >= 0)
                LEGAL_FAIL("dinit-slot");
            if (a->b < 0 || a->b > 9) LEGAL_FAIL("dinit-level");
            if (a->c < 1 || a->c > 9) LEGAL_FAIL("dinit-mem");
            if (a->d < 0 || a->d > 4) LEGAL_FAIL("dinit-strategy");
            if (pg->wrapper[noutputs] < -15 || pg->wrapper[noutputs] > 15 ||
                pg->wrapper[noutputs] == 0)
                LEGAL_FAIL("dinit-wrapper");
            if (a->e < -1 || a->e >= pg->ndict) LEGAL_FAIL("dinit-dict");
            if (pg->outdict[noutputs] != a->e) LEGAL_FAIL("dinit-dictmap");
            dslot[slot] = noutputs++;
            nd++;
            break;
        case OP_FEED:
            slot = a->a;
            if (slot < 0 || dslot[slot] < 0 || dended[slot]) LEGAL_FAIL("feed-slot");
            if (a->data.len == 0) LEGAL_FAIL("feed-empty");
            total += a->data.len;
            if (total > TASK_MAX_INPUT) LEGAL_FAIL("feed-total");
            break;
        case OP_FLUSH:
            slot = a->a;
            if (slot < 0 || dslot[slot] < 0 || dended[slot]) LEGAL_FAIL("flush-slot");
            if (a->b != MODEL_SYNC_FLUSH && a->b != MODEL_FULL_FLUSH)
                LEGAL_FAIL("flush-kind");
            break;
        case OP_DCOPY:
            if (nd >= TASK_MAX_STREAMS) LEGAL_FAIL("dcopy-cap");
            slot = a->a; dst = a->b;
            if (slot < 0 || dslot[slot] < 0 || dended[slot]) LEGAL_FAIL("dcopy-src");
            if (dst < 0 || dst >= TASK_MAX_STREAMS || dslot[dst] >= 0)
                LEGAL_FAIL("dcopy-dst");
            id = dslot[slot];
            if (a->d != id) LEGAL_FAIL("dcopy-srcid");
            if (a->e < 0 || (size_t)a->e > pg->expect[id].len) LEGAL_FAIL("dcopy-prefix");
            if (noutputs >= TASK_MAX_ACTIONS) LEGAL_FAIL("dcopy-outcap");
            if (a->outid != noutputs) LEGAL_FAIL("dcopy-outid");
            if (pg->wrapper[noutputs] != pg->wrapper[id]) LEGAL_FAIL("dcopy-wrapper");
            if (pg->outdict[noutputs] != pg->outdict[id]) LEGAL_FAIL("dcopy-dict");
            /*
             * At the fork the new output is a full copy of the prefix; both
             * branches only append afterwards, so their first e bytes must
             * still match and both must be at least that long.
             */
            if (pg->expect[noutputs].len < (size_t)a->e) LEGAL_FAIL("dcopy-expect");
            if (a->e > 0 && memcmp(pg->expect[noutputs].p, pg->expect[id].p,
                                   (size_t)a->e) != 0)
                LEGAL_FAIL("dcopy-expect");
            dslot[dst] = noutputs++;
            nd++;
            break;
        case OP_FINISH:
            slot = a->a;
            if (slot < 0 || dslot[slot] < 0 || dended[slot]) LEGAL_FAIL("finish-slot");
            id = dslot[slot];
            if (a->outid != id) LEGAL_FAIL("finish-outid");
            if (outdone[id]) LEGAL_FAIL("finish-twice");
            dended[slot] = 1;
            outdone[id] = 1;
            break;
        case OP_DEND:
            slot = a->a;
            if (slot < 0 || dslot[slot] < 0 || !dended[slot]) LEGAL_FAIL("dend-order");
            dslot[slot] = -1;
            dended[slot] = 0;
            nd--;
            break;
        case OP_IINIT:
            if (ni >= TASK_MAX_STREAMS) LEGAL_FAIL("iinit-cap");
            slot = a->a; id = a->b;
            if (slot < 0 || slot >= TASK_MAX_STREAMS || islot[slot] >= 0)
                LEGAL_FAIL("iinit-slot");
            if (id < 0 || id >= noutputs || !outdone[id]) LEGAL_FAIL("iinit-not-done");
            if (a->e != pg->outdict[id]) LEGAL_FAIL("iinit-dict");
            islot[slot] = id;
            ni++;
            break;
        case OP_IFEED:
            slot = a->a;
            if (slot < 0 || islot[slot] < 0 || iended[slot]) LEGAL_FAIL("ifeed-slot");
            if (a->b < 1 || a->c < 1) LEGAL_FAIL("ifeed-limits");
            break;
        case OP_ICOPY:
            if (ni >= TASK_MAX_STREAMS) LEGAL_FAIL("icopy-cap");
            slot = a->a; dst = a->b;
            if (slot < 0 || islot[slot] < 0 || iended[slot]) LEGAL_FAIL("icopy-src");
            if (dst < 0 || dst >= TASK_MAX_STREAMS || islot[dst] >= 0)
                LEGAL_FAIL("icopy-dst");
            islot[dst] = islot[slot];
            ni++;
            break;
        case OP_IFINISH:
            slot = a->a;
            if (slot < 0 || islot[slot] < 0 || iended[slot]) LEGAL_FAIL("ifinish-slot");
            iended[slot] = 1;
            break;
        case OP_IEND:
            slot = a->a;
            if (slot < 0 || islot[slot] < 0 || !iended[slot]) LEGAL_FAIL("iend-order");
            islot[slot] = -1;
            iended[slot] = 0;
            ni--;
            break;
        default:
            LEGAL_FAIL("bad-op");
        }
    }

    if (nd != 0 || ni != 0)
        LEGAL_FAIL("open-streams");
    if (total != pg->total_input || total > TASK_MAX_INPUT)
        LEGAL_FAIL("total");
    for (j = 0; j < noutputs; j++) {
        /* Empty plaintext is a legal zlib stream (header/trailer only). */
        if (pg->expect[j].len > TASK_MAX_INPUT)
            LEGAL_FAIL("expect-bounds");
        if (!outdone[j])
            LEGAL_FAIL("output-not-finished");
    }
#undef LEGAL_FAIL
    return 1;
}

/* ------------------------------------------------------------------ */
/* Coverage                                                            */
/* ------------------------------------------------------------------ */

void coverage_zero(Coverage *c)
{
    memset(c, 0, sizeof(*c));
}

void coverage_print(const Coverage *c, FILE *f)
{
    fprintf(f, "coverage: programs=%lu replays=%lu\n", c->programs, c->replays);
    fprintf(f, "  deflate streams=%lu inflate streams=%lu\n",
            c->def_streams, c->inf_streams);
    fprintf(f, "  deflateCopy=%lu (partial_in=%lu partial_out=%lu)\n",
            c->dcopy, c->dcopy_partial_in, c->dcopy_partial_out);
    fprintf(f, "  inflateCopy=%lu (partial_in=%lu partial_out=%lu)\n",
            c->icopy, c->icopy_partial_in, c->icopy_partial_out);
    fprintf(f, "  inflate original-ended-before-copy-completion=%lu\n",
            c->end_original_first);
    fprintf(f, "  SYNC_FLUSH=%lu FULL_FLUSH=%lu\n", c->sync_flush, c->full_flush);
    fprintf(f, "  dict zlib NEED_DICT path=%lu raw dict path=%lu\n",
            c->dict_zlib_need, c->dict_raw);
    fprintf(f, "  raw programs=%lu zlib programs=%lu branch-suffix runs=%lu\n",
            c->raw_programs, c->zlib_programs, c->branch_suffix);
    fprintf(f, "  clean end checks=%lu live allocations peak=%lu\n",
            c->end_ok, c->alloc_peak);
}
