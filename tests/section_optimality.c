/*
 * Section/codebook optimality tests for libfaac's huffbook selector.
 *
 * The production selector is exercised through its public entry points
 * (huffbook / writebooks / the quantizer and channel writer).  The expected
 * minimum is derived independently: the test has its own codeword-length
 * accounting that reads huffdata.c's public tables directly, its own section
 * length grammar, and an explicit dynamic program over legal (book, run)
 * states.  It never calls the production costers or the production selector.
 *
 * Every case additionally writes a real bit buffer, parses the section data
 * back (book numbers, run lengths, group boundaries), decodes the spectral
 * codewords with the public tables, and compares the recovered quantized
 * spectrum against the input.  A case only reports roundtrip_ok = 1 when the
 * whole buffer was consumed exactly.
 */

#include "section_cases.h"

#include "coder.h"
#include "huff2.h"
#include "huffdata.h"
#include "bitstream.h"
#include "quantize.h"
#include "channels.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define MAXBANDS MAX_SCFAC_BANDS

/* ------------------------------------------------------------------ */
/* Public Huffman tables, indexed by codebook value.                    */
/* ------------------------------------------------------------------ */

static const hcode16_t *const htab[HCB_NONE + 1] = {
    NULL,
    book01, book02, book03, book04, book05, book06,
    book07, book08, book09, book10, book11,
    NULL,           /* HCB_DELTA: scalefactors, never a spectral section */
    NULL, NULL, NULL,
    NULL
};

static const int htab_len[HCB_NONE + 1] = {
    0, 81, 81, 81, 81, 81, 81, 64, 64, 169, 169, 289,
    0, 0, 0, 0, 0
};

/* ------------------------------------------------------------------ */
/* Bit reader.                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    const unsigned char *buf;
    long cap;   /* bits */
    long pos;
    int err;
} BitReader;

static int br_bit(BitReader *br)
{
    int bit = 0;
    if (br->pos < br->cap) {
        bit = (br->buf[br->pos >> 3] >> (7 - (br->pos & 7))) & 1;
        br->pos++;
    } else {
        br->err = 1;
    }
    return bit;
}

static uint32_t br_bits(BitReader *br, int n)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < n; i++)
        v = (v << 1) | (uint32_t)br_bit(br);
    return v;
}

/* ------------------------------------------------------------------ */
/* Symbol decoding from the public tables.                             */
/* ------------------------------------------------------------------ */

static int decode_symbol(BitReader *br, int bk)
{
    const hcode16_t *tab = htab[bk];
    int n = htab_len[bk];
    uint32_t acc = 0;
    int l, i;

    for (l = 1; l <= 19; l++) {
        acc = (acc << 1) | (uint32_t)br_bit(br);
        for (i = 0; i < n; i++)
            if (tab[i].len == (uint16_t)l && tab[i].data == (uint16_t)acc)
                return i;
    }
    br->err = 1;
    return -1;
}

/* Escape suffix as produced by libfaac's escape(): unary prefix of `preflen`
 * ones then a zero, followed by preflen+4 value bits biased by 2^(preflen+4). */
static int decode_escape(BitReader *br)
{
    int preflen = 0, suffix;
    while (br_bit(br) == 1)
        preflen++;
    if (preflen > 11) { br->err = 1; return 0; }
    suffix = (int)br_bits(br, preflen + 4);
    return (1 << (preflen + 4)) + suffix;
}

static int decode_sign(BitReader *br)
{
    return br_bit(br) ? -1 : 1;
}

static void base_digits(int idx, int base, int count, int *out)
{
    int i;
    for (i = count - 1; i >= 0; i--) {
        out[i] = idx % base;
        idx /= base;
    }
}

/* Decodes one tuple of `bk` and returns coefficients (optionally signed). */
static void decode_tuple(BitReader *br, int bk, int *out, int *nout)
{
    int idx, step = (bk <= 4) ? 4 : 2;
    int d[4], i;

    idx = decode_symbol(br, bk);
    if (idx < 0) {
        *nout = 0;
        return;
    }

    switch (bk) {
    case HCB_1:
    case HCB_2:
        /* Signed tuple: index 40 + 27*q0 + 9*q1 + 3*q2 + q3 with q in
         * [-1,1].  40 is 1111 base 3, so the base-3 digits of the whole
         * index are exactly q+1. */
        base_digits(idx, 3, 4, d);
        for (i = 0; i < 4; i++) out[i] = d[i] - 1;
        break;
    case HCB_5:
    case HCB_6:
        /* Signed 2-tuple: index 40 + 9*q0 + q1 with q in [-1,1], and 40 is
         * 44 base 9, so the base-9 digits are q+4. */
        base_digits(idx, 9, 2, d);
        for (i = 0; i < 2; i++) out[i] = d[i] - 4;
        break;
    case HCB_3:
    case HCB_4:
        /* huffcode_write appends sign bits left-to-right (q0 first), so
         * read them back in tuple order after the codeword. */
        base_digits(idx, 3, 4, d);
        for (i = 0; i < 4; i++) {
            out[i] = d[i];
            if (out[i]) out[i] *= decode_sign(br);
        }
        break;
    case HCB_7:
    case HCB_8:
        base_digits(idx, 8, 2, d);
        for (i = 0; i < 2; i++) {
            out[i] = d[i];
            if (out[i]) out[i] *= decode_sign(br);
        }
        break;
    case HCB_9:
    case HCB_10:
        base_digits(idx, 13, 2, d);
        for (i = 0; i < 2; i++) {
            out[i] = d[i];
            if (out[i]) out[i] *= decode_sign(br);
        }
        break;
    case HCB_ESC:
        base_digits(idx, 17, 2, d);
        for (i = 0; i < 2; i++) {
            out[i] = d[i];
            if (out[i]) out[i] *= decode_sign(br);
        }
        /* Escape suffixes go after both sign bits, in tuple order. */
        for (i = 0; i < 2; i++)
            if (d[i] == LAV_ESC)
                out[i] = decode_escape(br) * (out[i] < 0 ? -1 : 1);
        break;
    default:
        br->err = 1;
        *nout = 0;
        return;
    }
    *nout = step;
}

/* Decodes a band's coefficients; returns 0 on success. */
static int decode_band(BitReader *br, int bk, int ncoef, int *out)
{
    int step = (bk <= 4) ? 4 : 2;
    int n = 0;

    if (bk < HCB_1 || bk > HCB_ESC || (ncoef % step) != 0)
        return -1;
    while (n < ncoef) {
        int tuple[4], nout, i;
        decode_tuple(br, bk, tuple, &nout);
        if (br->err || nout == 0)
            return -1;
        for (i = 0; i < nout && n < ncoef; i++)
            out[n++] = tuple[i];
    }
    return br->err ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Independent cost accounting.                                         */
/* ------------------------------------------------------------------ */

static int abs_int(int x) { return x < 0 ? -x : x; }

static int bit_length(int x)
{
    int n = 0;
    while (x) { n++; x >>= 1; }
    return n;
}

/* Exact suffix length libfaac's escape() emits for magnitude x >= 16. */
static int ref_escape_bits(int x)
{
    int preflen = bit_length(x) - 5;
    return (preflen + 1) + (preflen + 4);
}

/* Exact spectral cost of a band in a given book: symbol lengths from the
 * public tables plus sign bits and escape suffixes.  This mirrors the
 * bitstream grammar, not the production coster. */
static int ref_spec_bits(const int *q, int len, int bk)
{
    const hcode16_t *tab = htab[bk];
    int bits = 0, nnz = 0, i;

    if (bk == HCB_1 || bk == HCB_2) {
        for (i = 0; i < len; i += 4) {
            int idx = 40 + 27 * q[i] + 9 * q[i + 1] + 3 * q[i + 2] + q[i + 3];
            bits += tab[idx].len;
        }
        return bits;
    }
    if (bk == HCB_5 || bk == HCB_6) {
        for (i = 0; i < len; i += 2) {
            int idx = 40 + 9 * q[i] + q[i + 1];
            bits += tab[idx].len;
        }
        return bits;
    }
    if (bk == HCB_3 || bk == HCB_4) {
        for (i = 0; i < len; i += 4) {
            int idx = 27 * abs_int(q[i]) + 9 * abs_int(q[i + 1])
                    + 3 * abs_int(q[i + 2]) + abs_int(q[i + 3]);
            bits += tab[idx].len;
            nnz += q[i] != 0;
            nnz += q[i + 1] != 0;
            nnz += q[i + 2] != 0;
            nnz += q[i + 3] != 0;
        }
        return bits + nnz;
    }
    if (bk == HCB_7 || bk == HCB_8) {
        for (i = 0; i < len; i += 2) {
            int idx = 8 * abs_int(q[i]) + abs_int(q[i + 1]);
            bits += tab[idx].len;
            nnz += q[i] != 0;
            nnz += q[i + 1] != 0;
        }
        return bits + nnz;
    }
    if (bk == HCB_9 || bk == HCB_10) {
        for (i = 0; i < len; i += 2) {
            int idx = 13 * abs_int(q[i]) + abs_int(q[i + 1]);
            bits += tab[idx].len;
            nnz += q[i] != 0;
            nnz += q[i + 1] != 0;
        }
        return bits + nnz;
    }
    if (bk == HCB_ESC) {
        for (i = 0; i < len; i += 2) {
            int a0 = abs_int(q[i]), a1 = abs_int(q[i + 1]);
            int v0 = a0 > LAV_ESC ? LAV_ESC : a0;
            int v1 = a1 > LAV_ESC ? LAV_ESC : a1;
            int idx = 17 * v0 + v1;
            bits += tab[idx].len;
            nnz += q[i] != 0;
            nnz += q[i + 1] != 0;
            if (a0 >= LAV_ESC) bits += ref_escape_bits(a0);
            if (a1 >= LAV_ESC) bits += ref_escape_bits(a1);
        }
        return bits + nnz;
    }
    return INT_MAX / 4;
}

/* Section cost of a set of consecutive bands: 4-bit book number plus
 * floor(R/max_run)+1 run fields (a zero remainder after an exact multiple
 * is still a field). */
static int ref_section_cost(int run, int max_run, int run_bits)
{
    return 4 + (run / max_run + 1) * run_bits;
}

/* Lowest spectral book that can carry a band whose peak magnitude is `peak`,
 * matching the encoder's own initial choice. */
static int lowest_book(int peak)
{
    if (peak <= LAV_1) return HCB_1;
    if (peak <= LAV_2) return HCB_3;
    if (peak <= LAV_4) return HCB_5;
    if (peak <= LAV_7) return HCB_7;
    if (peak <= LAV_12) return HCB_9;
    return HCB_ESC;
}

/* ------------------------------------------------------------------ */
/* A band layout used by hand-built cases.                              */
/* ------------------------------------------------------------------ */

struct layout {
    int block_type;
    int sfbn;
    int ngroup;
    int glen[MAX_SHORT_WINDOWS];
    int width[NSFB_LONG];
    int off[NSFB_LONG + 1];
    /* special[b] < 0 marks an ordinary spectral band; otherwise it is the
     * fixed special book (HCB_ZERO / HCB_PNS / both intensity codes). */
    int special[MAXBANDS];
    int q[4096];        /* ordinary bands' quantized values, back to back */
    int qlen;
    int qoff[MAXBANDS];
    int blen[MAXBANDS];
};

static void layout_prepare(struct layout *L)
{
    int g, s, b, off = 0;

    L->off[0] = 0;
    for (s = 0; s < L->sfbn; s++)
        L->off[s + 1] = L->off[s] + L->width[s];

    for (g = 0; g < L->ngroup; g++) {
        for (s = 0; s < L->sfbn; s++) {
            b = g * L->sfbn + s;
            L->blen[b] = L->width[s] * L->glen[g];
            if (L->special[b] < 0) {
                L->qoff[b] = off;
                off += L->blen[b];
            } else {
                L->qoff[b] = -1;
            }
        }
    }
    L->qlen = off;
}

static int band_peak(const struct layout *L, int b)
{
    const int *q = L->q + L->qoff[b];
    int i, peak = 0;
    for (i = 0; i < L->blen[b]; i++) {
        int a = abs_int(q[i]);
        if (a > peak) peak = a;
    }
    return peak;
}

static void layout_init_coder(const struct layout *L, CoderInfo *c)
{
    int b, g;

    memset(c, 0, sizeof(*c));
    c->block_type = L->block_type;
    c->sfbn = L->sfbn;
    c->groups.n = L->ngroup;
    for (g = 0; g < L->ngroup; g++)
        c->groups.len[g] = L->glen[g];
    c->bandcnt = L->ngroup * L->sfbn;
    c->sfb_offset = L->off;

    for (b = 0; b < c->bandcnt; b++) {
        if (L->special[b] >= 0)
            c->book[b] = L->special[b];
        else
            c->book[b] = lowest_book(band_peak(L, b));
    }
}

/* Independent minimum over all legal book assignments and section
 * partitions of a group.  dp[book][run % max_run]; run 0 means an open
 * section holding a positive multiple of max_run bands. */
static int ref_min_group(const struct layout *L, int g)
{
    int max_run = (L->block_type == ONLY_SHORT_WINDOW) ? 7 : 31;
    int run_bits = max_run == 7 ? 3 : 5;
    int open_cost = 4 + run_bits;
    int dp[HCB_NONE + 1][31], ndp[HCB_NONE + 1][31];
    int bk, r, s;
    int best;
    const int INF = INT_MAX / 4;

    for (bk = 0; bk <= HCB_NONE; bk++)
        for (r = 0; r < max_run; r++)
            dp[bk][r] = INF;

    for (s = 0; s < L->sfbn; s++) {
        int gi = g * L->sfbn + s;
        int cand[HCB_NONE + 1], cost[HCB_NONE + 1], ncand = 0;
        int same[HCB_NONE + 1], same_run[HCB_NONE + 1];

        if (L->special[gi] >= 0) {
            cand[ncand] = L->special[gi];
            cost[ncand] = 0;
            ncand++;
        } else {
            const int *q = L->q + L->qoff[gi];
            int len = L->blen[gi];
            int lo = lowest_book(band_peak(L, gi));
            for (bk = lo; bk <= HCB_ESC; bk++) {
                cand[ncand] = bk;
                cost[ncand] = ref_spec_bits(q, len, bk);
                ncand++;
            }
        }

        if (s == 0) {
            for (bk = 0; bk <= HCB_NONE; bk++)
                for (r = 0; r < max_run; r++)
                    ndp[bk][r] = INF;
            for (r = 0; r < ncand; r++)
                ndp[cand[r]][1] = open_cost + cost[r];
        } else {
            for (bk = 0; bk <= HCB_NONE; bk++) {
                int m = INF, arg = 0;
                for (r = 0; r < max_run; r++)
                    if (dp[bk][r] < m) { m = dp[bk][r]; arg = r; }
                same[bk] = m;
                same_run[bk] = arg;
            }
            for (bk = 0; bk <= HCB_NONE; bk++)
                for (r = 0; r < max_run; r++)
                    ndp[bk][r] = INF;

            for (r = 0; r < ncand; r++) {
                int cb = cand[r], cst = cost[r];
                int prev, other = INF;

                /* Continue a section already in this book. */
                for (prev = 0; prev < max_run; prev++) {
                    int v = dp[cb][prev], nr, nv;
                    if (v >= INF) continue;
                    nr = (prev + 1) % max_run;
                    nv = v + cst + (nr == 0 ? run_bits : 0);
                    if (nv < ndp[cb][nr])
                        ndp[cb][nr] = nv;
                }
                /* Open a new section: the previous band must use another
                 * book, since same-book neighbours are one emitted run. */
                for (bk = 0; bk <= HCB_NONE; bk++)
                    if (bk != cb && same[bk] < other)
                        other = same[bk];
                if (other < INF) {
                    int nv = other + open_cost + cst;
                    if (nv < ndp[cb][1])
                        ndp[cb][1] = nv;
                }
            }
        }
        memcpy(dp, ndp, sizeof(dp));
    }

    best = INF;
    for (bk = 0; bk <= HCB_NONE; bk++)
        for (r = 0; r < max_run; r++)
            if (dp[bk][r] < best) best = dp[bk][r];
    return best;
}

/* Exhaustive reference for a small group: enumerate every legal partition
 * (all run boundaries) and every legal book choice for each run, cost the
 * spectrum exactly, and take the true minimum. Used to cross-check the
 * dynamic program above on tiny layouts, so a bug in either is caught before
 * either is trusted. Returns -1 if the group is too large to enumerate. */
static int ref_enumerate_group(const struct layout *L, int g)
{
    int max_run = (L->block_type == ONLY_SHORT_WINDOW) ? 7 : 31;
    int run_bits = max_run == 7 ? 3 : 5;
    int n = L->sfbn;
    int band_cand[NSFB_LONG][HCB_NONE + 1];
    int band_cost[NSFB_LONG][HCB_NONE + 1];
    int ncand[NSFB_LONG];
    int best = INT_MAX / 4, s, i;
    const int INF = INT_MAX / 4;

    for (s = 0; s < n; s++) {
        int gi = g * L->sfbn + s;
        if (L->special[gi] >= 0) {
            band_cand[s][0] = L->special[gi];
            band_cost[s][0] = 0;
            ncand[s] = 1;
        } else {
            const int *q = L->q + L->qoff[gi];
            int lo = lowest_book(band_peak(L, gi));
            int nc = 0;
            for (i = lo; i <= HCB_ESC; i++) {
                band_cand[s][nc] = i;
                band_cost[s][nc] = ref_spec_bits(q, L->blen[gi], i);
                nc++;
            }
            ncand[s] = nc;
        }
    }

    /* Every partition of n bands into consecutive runs. */
    for (i = 0; i < (1 << (n - 1)); i++) {
        int starts[NSFB_LONG], nruns = 0, ok = 1, pos = 0, r;
        long cost = 0;

        starts[nruns++] = 0;
        for (s = 1; s < n; s++)
            if (i & (1 << (s - 1)))
                starts[nruns++] = s;

        for (r = 0; r < nruns && ok; r++) {
            int end = (r + 1 < nruns) ? starts[r + 1] : n;
            int run = end - starts[r];
            int best_run = INF, k;

            /* A run of same-book neighbours is what writebooks emits: all
            * its bands must share one book, which is why a book is chosen
            * once per run, not per band. */
            for (k = 0; k < ncand[starts[r]]; k++) {
                int bk = band_cand[starts[r]][k];
                int c = ref_section_cost(run, max_run, run_bits);
                int j;
                for (j = starts[r]; j < end && c < best_run; j++) {
                    int cc;
                    for (cc = 0; cc < ncand[j]; cc++)
                        if (band_cand[j][cc] == bk) break;
                    if (cc == ncand[j]) { c = INF; break; }
                    c += band_cost[j][cc];
                }
                if (c < best_run) best_run = c;
            }
            if (best_run >= INF) ok = 0;
            cost += best_run;
        }
        if (ok && cost < best) best = cost;
        pos = 0; (void)pos;
    }
    return best;
}

static int ref_total(const struct layout *L)
{
    int g, total = 0;
    for (g = 0; g < L->ngroup; g++) {
        /* Small groups are cheap enough to enumerate every partition and
         * every codebook, so use that as the reference directly; larger
         * ones need the dynamic program.  Either way the two must agree
         * wherever both are usable. */
        if (L->sfbn <= 10) {
            int e = ref_enumerate_group(L, g);
            int d = ref_min_group(L, g);
            if (e != d) {
                fprintf(stderr, "reference disagreement: enumerate %d dp %d\n",
                        e, d);
                return -1;
            }
            total += e;
        } else {
            total += ref_min_group(L, g);
        }
    }
    return total;
}


/* ------------------------------------------------------------------ */
/* Parsing of the emitted section data.                                */
/* ------------------------------------------------------------------ */

static int parse_sections(BitReader *br, int ngroup, int sfbn,
                          int max_run, int run_bits, int *books)
{
    int g, b;
    long start = br->pos;

    for (g = 0; g < ngroup; g++) {
        b = g * sfbn;
        while (b < g * sfbn + sfbn) {
            int bk = (int)br_bits(br, 4);
            int run = 0;
            for (;;) {
                int field = (int)br_bits(br, run_bits);
                run += field;
                if (field < max_run) break;
                if (run > sfbn) return -1;
            }
            if (run <= 0 || b + run > g * sfbn + sfbn || bk > HCB_NONE)
                return -1;
            {
                int i;
                for (i = 0; i < run; i++)
                    books[b + i] = bk;
            }
            b += run;
        }
    }
    return (int)(br->pos - start);
}

/* ------------------------------------------------------------------ */
/* One full case evaluation: select, write, parse back, compare.        */
/* ------------------------------------------------------------------ */

static int eval_case(const struct layout *L, int *out_written,
                     int *out_min, int *out_rt)
{
    static unsigned char buf[1 << 16];
    CoderInfo c, d;
    BitStream bs;
    BitReader br;
    int books[MAXBANDS];
    int sec, spec = 0, i, b;
    int secbits, qi = 0, ok = 1;
    long total, spstart, spend;
    int *qdec;

    layout_init_coder(L, &c);
    layout_init_coder(L, &d);
    huffbook(&c, L->q);
    huffbook(&d, L->q);
    if (memcmp(c.book, d.book, sizeof(int) * (size_t)c.bandcnt) != 0) {
        fprintf(stderr, "nondeterministic book selection\n");
        return 1;
    }

    InitBitStream(&bs, buf, sizeof(buf));
    sec = writebooks(&c, &bs);
    for (i = 0; i < c.datacnt; i++) {
        if (PutBit(&bs, (uint32_t)c.s[i].data, c.s[i].len) < 0) {
            fprintf(stderr, "spectrum overflow\n");
            return 1;
        }
        spec += c.s[i].len;
    }
    total = (long)bs.currentBit;

    br.buf = buf;
    br.cap = total;
    br.pos = 0;
    br.err = 0;

    secbits = parse_sections(&br, L->ngroup, L->sfbn,
                             (L->block_type == ONLY_SHORT_WINDOW) ? 7 : 31,
                             (L->block_type == ONLY_SHORT_WINDOW) ? 3 : 5,
                             books);
    if (secbits < 0 || secbits != sec) {
        fprintf(stderr, "section parse mismatch %d vs %d\n", secbits, sec);
        return 1;
    }
    if (br.err) return 1;

    spstart = br.pos;
    qdec = (int *)L->q; /* scratch compare target below is separate */
    {
        static int decoded[4096];
        int di = 0;
        for (b = 0; b < c.bandcnt; b++) {
            int bk = books[b];
            if (bk == HCB_ZERO || bk == HCB_PNS ||
                bk == HCB_INTENSITY || bk == HCB_INTENSITY2) {
                if (L->special[b] != bk) {
                    fprintf(stderr, "special identity lost at band %d: %d vs %d\n",
                            b, bk, L->special[b]);
                    ok = 0;
                }
                continue;
            }
            if (bk < HCB_1 || bk > HCB_ESC) {
                fprintf(stderr, "illegal spectral book %d at band %d\n", bk, b);
                ok = 0;
                continue;
            }
            if (L->special[b] >= 0) {
                fprintf(stderr, "special band %d was coded as spectrum\n", b);
                ok = 0;
                continue;
            }
            if (bk < lowest_book(band_peak(L, b))) {
                fprintf(stderr, "book %d cannot carry band %d\n", bk, b);
                ok = 0;
            }
            if (decode_band(&br, bk, L->blen[b], decoded + di) != 0) {
                fprintf(stderr, "spectrum decode failed at band %d\n", b);
                ok = 0;
                break;
            }
            if (memcmp(decoded + di, L->q + L->qoff[b],
                       sizeof(int) * (size_t)L->blen[b]) != 0) {
                fprintf(stderr, "spectrum mismatch at band %d\n", b);
                ok = 0;
            }
            di += L->blen[b];
        }
        qi = di;
        spend = br.pos;
    }
    (void)qdec;

    if (br.pos != total) {
        fprintf(stderr, "buffer not fully consumed: %ld of %ld\n", br.pos, total);
        ok = 0;
    }
    if (spend - spstart != spec) {
        fprintf(stderr, "spectrum bit count mismatch %ld vs %d\n",
                spend - spstart, spec);
        ok = 0;
    }
    if (qi != L->qlen) {
        fprintf(stderr, "decoded %d coefficients, expected %d\n", qi, L->qlen);
        ok = 0;
    }

    *out_written = sec + spec;
    *out_min = ref_total(L);
    *out_rt = ok ? 1 : 0;
    return 0;
}

static int finish_case(const struct layout *L, struct section_result *r)
{
    int written = -1, minimum = -1, rt = 0;
    if (eval_case(L, &written, &minimum, &rt) != 0)
        return 1;
    if (!rt || written != minimum)
        return 1;
    r->written_bits = written;
    r->minimum_bits = minimum;
    r->roundtrip_ok = rt;
    return 0;
}

/* Fills one ordinary band with 4*glen values from a simple pattern. */
static void fill_pattern(struct layout *L, int g, int s, const int *pat, int np)
{
    int b = g * L->sfbn + s;
    int *q = L->q + L->qoff[b];
    int n = L->blen[b], i;
    for (i = 0; i < n; i++)
        q[i] = pat[i % np];
}

/* ------------------------------------------------------------------ */
/* Case 1: the shipped 244/243 witness.                                */
/* ------------------------------------------------------------------ */

static int case_witness(struct section_result *r)
{
    static const int q[128] = {
        1,0,0,1,0,1,1,-1,-1,0,1,0,0,-1,1,0,0,-1,0,1,0,-1,1,0,0,-1,1,0,
        -1,-1,0,1,0,0,0,-1,0,-1,0,-1,1,0,-1,1,0,0,-1,-1,1,0,0,1,1,-1,0,
        0,0,0,1,1,1,-1,0,0,0,0,0,1,0,1,-1,0,1,1,0,1,1,1,0,1,0,0,-1,-1,0,
        -1,1,-1,-1,0,1,0,1,1,-1,1,-1,-1,1,-1,1,1,-1,-1,-1,-1,0,1,1,-1,1,1,
        0,1,0,1,-1,0,0,1,-1,1,-1,0,-1,1,1,1
    };
    static const int widths[14] = {4,4,4,4,4,8,8,8,12,12,12,16,16,16};
    struct layout L;
    int b;

    memset(&L, 0, sizeof(L));
    L.block_type = ONLY_SHORT_WINDOW;
    L.sfbn = 14;
    L.ngroup = 2;
    L.glen[0] = 1;
    L.glen[1] = 7;
    memcpy(L.width, widths, sizeof(widths));
    for (b = 0; b < 28; b++)
        L.special[b] = (b < 14) ? -1 : HCB_ZERO;
    layout_prepare(&L);
    memcpy(L.q, q, sizeof(q));
    if (L.qlen != 128) return 1;

    return finish_case(&L, r);
}

/* ------------------------------------------------------------------ */
/* Case 2: long window, one run of exactly 31 bands.                   */
/* ------------------------------------------------------------------ */

static int case_long_31(struct section_result *r)
{
    struct layout L;
    static const int pat[4] = {1, 0, -1, 1};
    int s, b;
    int32_t peaks[31];

    memset(&L, 0, sizeof(L));
    L.block_type = ONLY_LONG_WINDOW;
    L.sfbn = 31;
    L.ngroup = 1;
    L.glen[0] = 1;
    for (s = 0; s < 31; s++) {
        L.width[s] = 4;
        L.special[s] = -1;
        /* A mild pattern everywhere keeps one book viable across all 31. */
        peaks[s] = (s == 30) ? 2 : 1;
    }
    layout_prepare(&L);
    for (b = 0; b < 31; b++) {
        int *q = L.q + L.qoff[b];
        q[0] = pat[0]; q[1] = pat[1]; q[2] = pat[2]; q[3] = pat[3];
        if (peaks[b] == 2)
            q[0] = 2;
    }
    return finish_case(&L, r);
}

/* ------------------------------------------------------------------ */
/* Case 3: short window, one run of exactly 7 bands.                   */
/* ------------------------------------------------------------------ */

static int case_short_7(struct section_result *r)
{
    struct layout L;
    static const int pat[4] = {1, -1, 0, 1};
    int s, b;

    memset(&L, 0, sizeof(L));
    L.block_type = ONLY_SHORT_WINDOW;
    L.sfbn = 7;
    L.ngroup = 1;
    L.glen[0] = 8;
    for (s = 0; s < 7; s++) {
        L.width[s] = 4;
        L.special[s] = -1;
    }
    layout_prepare(&L);
    for (b = 0; b < 7; b++)
        fill_pattern(&L, 0, b, pat, 4);
    return finish_case(&L, r);
}

/* ------------------------------------------------------------------ */
/* Case 4: short window, one run of exactly 14 bands.                  */
/* ------------------------------------------------------------------ */

static int case_short_14(struct section_result *r)
{
    struct layout L;
    static const int pat[4] = {1, 1, -1, 0};
    int s, b;

    memset(&L, 0, sizeof(L));
    L.block_type = ONLY_SHORT_WINDOW;
    L.sfbn = 14;
    L.ngroup = 1;
    L.glen[0] = 8;
    for (s = 0; s < 14; s++) {
        L.width[s] = 4;
        L.special[s] = -1;
    }
    layout_prepare(&L);
    for (b = 0; b < 14; b++)
        fill_pattern(&L, 0, b, pat, 4);
    return finish_case(&L, r);
}

/* ------------------------------------------------------------------ */
/* Case 5: every legal grouping of the eight short windows.            */
/* ------------------------------------------------------------------ */

struct regroup_ctx {
    int mask;
    int failed;
    int first_bits;
    int first_min;
};

static void regroup_walk(struct regroup_ctx *ctx, struct layout *L)
{
    static const int pat[4] = {1, 0, -1, 1};
    struct section_result r = {-1, -1, 0};
    int b, g;

    /* Recompose the group lengths from the boundary mask. */
    L->ngroup = 0;
    L->glen[0] = 1;
    for (b = 1; b < MAX_SHORT_WINDOWS; b++) {
        if (ctx->mask & (1 << (b - 1)))
            L->glen[L->ngroup++] += 1;
        else {
            L->ngroup++;
            L->glen[L->ngroup] = 1;
        }
    }
    L->ngroup++;

    layout_prepare(L);
    for (g = 0; g < L->ngroup; g++)
        for (b = 0; b < L->sfbn; b++)
            fill_pattern(L, g, b, pat, 4);

    if (finish_case(L, &r) != 0) {
        fprintf(stderr, "grouping mask %d failed\n", ctx->mask);
        ctx->failed = 1;
        return;
    }
    if (ctx->first_bits < 0) {
        ctx->first_bits = r.written_bits;
        ctx->first_min = r.minimum_bits;
    }
}

static int case_group_restart(struct section_result *r)
{
    struct layout L;
    struct regroup_ctx ctx;
    int s, mask;

    memset(&ctx, 0, sizeof(ctx));
    ctx.first_bits = -1;
    ctx.first_min = -1;

    memset(&L, 0, sizeof(L));
    L.block_type = ONLY_SHORT_WINDOW;
    L.sfbn = 14;
    for (s = 0; s < 14; s++) {
        L.width[s] = 4;
        L.special[s] = -1;
    }

    for (mask = 0; mask < 128; mask++) {
        ctx.mask = mask;
        regroup_walk(&ctx, &L);
        if (ctx.failed)
            return 1;
    }
    r->written_bits = ctx.first_bits;
    r->minimum_bits = ctx.first_min;
    r->roundtrip_ok = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Special-band identity cases.                                        */
/* ------------------------------------------------------------------ */

static void build_identity_layout(struct layout *L, const int *pat, int np,
                                  int special_at, int special_book)
{
    int s, b;

    memset(L, 0, sizeof(*L));
    L->block_type = ONLY_LONG_WINDOW;
    L->sfbn = 12;
    L->ngroup = 1;
    L->glen[0] = 1;
    for (s = 0; s < 12; s++) {
        L->width[s] = 4;
        L->special[s] = (s == special_at || s == special_at + 1)
                        ? special_book : -1;
    }
    layout_prepare(L);
    for (b = 0; b < 12; b++)
        if (L->special[b] < 0)
            fill_pattern(L, 0, b, pat, np);
}

static int case_zero_identity(struct section_result *r)
{
    struct layout L;
    static const int pat[4] = {1, 0, -1, 1};
    build_identity_layout(&L, pat, 4, 3, HCB_ZERO);
    return finish_case(&L, r);
}

static int case_pns_identity(struct section_result *r)
{
    struct layout L;
    static const int pat[4] = {1, -1, 0, 1};
    build_identity_layout(&L, pat, 4, 4, HCB_PNS);
    return finish_case(&L, r);
}

static int case_intensity_identity(struct section_result *r)
{
    struct layout L;
    static const int pat[4] = {1, 1, 0, -1};
    int b;

    memset(&L, 0, sizeof(L));
    L.block_type = ONLY_LONG_WINDOW;
    L.sfbn = 12;
    L.ngroup = 1;
    L.glen[0] = 1;
    for (b = 0; b < 12; b++) {
        L.width[b] = 4;
        /* Adjacent distinct intensity codes (15 and 14) must stay separate
         * sections; a single "special" state would merge them. */
        if (b == 3) L.special[b] = HCB_INTENSITY;
        else if (b == 4) L.special[b] = HCB_INTENSITY2;
        else if (b == 5) L.special[b] = HCB_INTENSITY;
        else L.special[b] = -1;
    }
    layout_prepare(&L);
    for (b = 0; b < 12; b++)
        if (L.special[b] < 0)
            fill_pattern(&L, 0, b, pat, 4);
    return finish_case(&L, r);
}

/* ------------------------------------------------------------------ */
/* Case 9: signed nonzero values across signed and magnitude books.     */
/* ------------------------------------------------------------------ */

static int case_signed_spectrum(struct section_result *r)
{
    struct layout L;
    static const int pat_a[4] = {-1, 1, 1, -1};   /* should pick HCB_1/2 */
    static const int pat_b[4] = {-2, 2, -1, 0};   /* magnitude book 3/4 */
    static const int pat_c[2 * 4] = {3, -3, 4, -4, 2, -2, 0, 1};
    int b;

    memset(&L, 0, sizeof(L));
    L.block_type = ONLY_LONG_WINDOW;
    L.sfbn = 9;
    L.ngroup = 1;
    L.glen[0] = 1;
    for (b = 0; b < 9; b++) {
        L.width[b] = 4;
        L.special[b] = -1;
    }
    layout_prepare(&L);
    fill_pattern(&L, 0, 0, pat_a, 4);
    fill_pattern(&L, 0, 1, pat_c, 8);
    fill_pattern(&L, 0, 2, pat_b, 4);
    fill_pattern(&L, 0, 3, pat_c, 8);
    fill_pattern(&L, 0, 4, pat_a, 4);
    fill_pattern(&L, 0, 5, pat_b, 4);
    fill_pattern(&L, 0, 6, pat_a, 4);
    fill_pattern(&L, 0, 7, pat_c, 8);
    fill_pattern(&L, 0, 8, pat_b, 4);
    return finish_case(&L, r);
}

/* ------------------------------------------------------------------ */
/* Cases 10/11: escapes and escape-only bands.                          */
/* ------------------------------------------------------------------ */

static int case_escape_16_plus(struct section_result *r)
{
    struct layout L;
    static const int pat_a[4] = {16, -16, 17, -17};
    static const int pat_b[4] = {31, -31, 32, -32};
    static const int pat_c[4] = {100, -100, 1000, -1000};
    int b;

    memset(&L, 0, sizeof(L));
    L.block_type = ONLY_LONG_WINDOW;
    L.sfbn = 8;
    L.ngroup = 1;
    L.glen[0] = 1;
    for (b = 0; b < 8; b++) {
        L.width[b] = 4;
        L.special[b] = -1;
    }
    layout_prepare(&L);
    fill_pattern(&L, 0, 0, pat_a, 4);
    fill_pattern(&L, 0, 1, pat_b, 4);
    fill_pattern(&L, 0, 2, pat_c, 4);
    fill_pattern(&L, 0, 3, pat_a, 4);
    fill_pattern(&L, 0, 4, pat_b, 4);
    fill_pattern(&L, 0, 5, pat_c, 4);
    fill_pattern(&L, 0, 6, pat_a, 4);
    fill_pattern(&L, 0, 7, pat_c, 4);
    return finish_case(&L, r);
}

static int case_escape_only(struct section_result *r)
{
    struct layout L;
    static const int pat[4] = {20, -20, 8191, -8191};
    int b;

    memset(&L, 0, sizeof(L));
    L.block_type = ONLY_LONG_WINDOW;
    L.sfbn = 6;
    L.ngroup = 1;
    L.glen[0] = 1;
    for (b = 0; b < 6; b++) {
        L.width[b] = 4;
        L.special[b] = -1;
    }
    layout_prepare(&L);
    /* Peak >= 16 leaves book 11 as the only legal choice. */
    for (b = 0; b < 6; b++)
        fill_pattern(&L, 0, b, pat, 4);
    return finish_case(&L, r);
}

/* ------------------------------------------------------------------ */
/* Case 12: real quantize.c -> Huffman write -> assembled AAC frame.    */
/* ------------------------------------------------------------------ */

#define QFRAME_SFBN 20
#define QFRAME_COEFS (QFRAME_SFBN * 4)

/* Decodes one scalefactor-delta symbol (book12) and returns its index. */
static int decode_delta(BitReader *br)
{
    uint32_t acc = 0;
    int l, i;

    for (l = 1; l <= 19; l++) {
        acc = (acc << 1) | (uint32_t)br_bit(br);
        for (i = 0; i < (int)(sizeof(book12) / sizeof(book12[0])); i++)
            if (book12[i].len == (uint32_t)l && book12[i].data == acc)
                return i;
    }
    br->err = 1;
    return -1;
}

/* Decodes the scalefactor field exactly as writesf emits it, so the reader
 * lands on the spectrum. */
static int skip_scalefactors(BitReader *br, const int *books, int bandcnt,
                             int global_gain)
{
    int lastsf = global_gain, lastis = 0;
    int lastpns = global_gain - SF_PNS_OFFSET, firstpns = 1;
    int b;
    (void)lastsf; (void)lastis; (void)lastpns;

    for (b = 0; b < bandcnt; b++) {
        int bk = books[b];
        int idx;
        if (bk == HCB_ZERO || bk == HCB_NONE)
            continue;
        if (bk == HCB_INTENSITY || bk == HCB_INTENSITY2) {
            idx = decode_delta(br);
            if (idx < 0) return -1;
            lastis += idx - SF_DELTA;
        } else if (bk == HCB_PNS) {
            if (firstpns) {
                (void)br_bits(br, 9);
                firstpns = 0;
            } else {
                idx = decode_delta(br);
                if (idx < 0) return -1;
                lastpns += idx - SF_DELTA;
            }
        } else {
            idx = decode_delta(br);
            if (idx < 0) return -1;
            lastsf += idx - SF_DELTA;
        }
    }
    return br->err ? -1 : 0;
}

static int case_quantize_frame(struct section_result *r)
{
    static unsigned char frame[1 << 16];
    static CoderInfo c;
    struct layout L;
    BitStream bs;
    BitReader br;
    AACElement el;
    AACQuantCfg cfg;
    float xr[BLOCK_LEN_LONG];
    int off[QFRAME_SFBN + 1];
    int books[MAXBANDS];
    int secbits, elbits, pad, total, i, b;
    int global_gain, winseq, maxsfb;
    long secstart, secend, spstart, spend;
    int qfroms[4096], qfromf[4096], nfroms = 0, nfromf = 0;
    int written, minimum, rt = 1;
    int specialcount = 0, ordinarycount = 0;

    /* Deterministic spectral input: a few noisy bands over a silent floor.
     * BlocQuant is the real quantizer, so the scalefactors and quantized
     * values are whatever it computes. */
    memset(xr, 0, sizeof(xr));
    for (i = 0; i < BLOCK_LEN_LONG; i++) {
        unsigned int r = (unsigned int)(i * 1103515245u + 12345u);
        r = r ^ (r >> 13);
        if (i < 40)
            xr[i] = (float)((int)(r % 2001u) - 1000) * 1.0f;
        else if (i >= 40 && i < 52)
            xr[i] = (float)((int)(r % 2001u) - 1000) * 0.01f;
        else
            xr[i] = (float)((int)(r % 21u) - 10) * 1.0e-6f;
    }

    for (i = 0; i < QFRAME_SFBN; i++)
        off[i] = 4 * i;
    off[QFRAME_SFBN] = QFRAME_COEFS;

    memset(&c, 0, sizeof(c));
    c.block_type = ONLY_LONG_WINDOW;
    c.sfbn = QFRAME_SFBN;
    c.sfb_offset = off;
    c.groups.n = 1;
    c.groups.len[0] = 1;
    ResetCoderSections(&c);

    memset(&cfg, 0, sizeof(cfg));
    cfg.quality = 300.0f;
    cfg.max_cbl = QFRAME_SFBN;
    cfg.max_cbs = 14;
    cfg.max_l = QFRAME_COEFS;
    cfg.pnslevel = 4;
    cfg.treble_slope = 1.0f;

    QuantizeInit();
    if (!BlocQuant(&c, xr, &cfg))
        return 1;
    if (c.bandcnt != QFRAME_SFBN)
        return 1;

    for (b = 0; b < c.bandcnt; b++) {
        if (c.book[b] == HCB_ZERO || c.book[b] == HCB_PNS)
            specialcount++;
        else if (c.book[b] >= HCB_1 && c.book[b] <= HCB_ESC)
            ordinarycount++;
        else
            return 1;
    }

    /* Assemble the AAC frame exactly as an ADTS payload: a 7-byte ADTS
     * header, then one SCE, the ID_END terminator and byte padding.  The
     * header is transport, so the reader skips it before parsing and the
     * optimality cost below never includes it. */
    memset(&el, 0, sizeof(el));
    el.type = ID_SCE;
    el.tag = 0;
    el.channels[0] = 0;
    InitBitStream(&bs, frame, sizeof(frame));

    PutBit(&bs, 0xFFF, LEN_ADTS_SYNC);
    PutBit(&bs, 0, LEN_ADTS_ID);
    PutBit(&bs, 0, LEN_ADTS_LAYER);
    PutBit(&bs, 1, LEN_ADTS_ABSENT);
    PutBit(&bs, 1, LEN_ADTS_PROFILE);  /* AAC-LC profile index */
    PutBit(&bs, 3, LEN_ADTS_FREQ);         /* 48 kHz */
    PutBit(&bs, 0, LEN_ADTS_PRIV);
    PutBit(&bs, 1, LEN_ADTS_CH_CFG);       /* mono SCE */
    PutBit(&bs, 0, LEN_ADTS_ORIG + LEN_ADTS_HOME + LEN_ADTS_COPY_ID + LEN_ADTS_COPY_ST);
    PutBit(&bs, 0, LEN_ADTS_FRAME);        /* patched below */
    PutBit(&bs, 0x7FF, LEN_ADTS_FULL);
    PutBit(&bs, 0, LEN_ADTS_BLOCKS);

    elbits = WriteElement(&bs, &el, &c);
    PutBit(&bs, ID_END, LEN_SE_ID);
    pad = (8 - ((ADTS_HEADER_SIZE * 8 + elbits + LEN_SE_ID) & 7)) & 7;
    for (i = 0; i < pad; i++)
        PutBit(&bs, 0, 1);
    total = (int)bs.currentBit;
    if (total != ADTS_HEADER_SIZE * 8 + elbits + LEN_SE_ID + pad)
        return 1;
    /* Patch the frame length, as PatchADTSHeader does. */
    {
        int len = total / 8;
        frame[4] = (unsigned char)((frame[4] & 0xFC) | ((len >> 11) & 3));
        frame[5] = (unsigned char)((len >> 3) & 0xFF);
        frame[6] = (unsigned char)((frame[6] & 0x1F) | ((len & 7) << 5));
    }

    br.buf = frame;
    br.cap = total;
    br.pos = 0;
    br.err = 0;

    /* Skip the transport header; the raw_data_block starts after it. */
    (void)br_bits(&br, ADTS_HEADER_SIZE * 8);

    if ((int)br_bits(&br, LEN_SE_ID) != ID_SCE)
        return 1;
    (void)br_bits(&br, LEN_TAG);
    global_gain = (int)br_bits(&br, LEN_GLOB_GAIN);
    (void)br_bits(&br, LEN_ICS_RESERV);
    winseq = (int)br_bits(&br, LEN_WIN_SEQ);
    (void)br_bits(&br, LEN_WIN_SH);
    maxsfb = (int)br_bits(&br, LEN_MAX_SFBL);
    (void)br_bits(&br, LEN_PRED_PRES);
    if (winseq != ONLY_LONG_WINDOW || maxsfb != QFRAME_SFBN)
        return 1;

    secstart = br.pos;
    secbits = parse_sections(&br, 1, maxsfb, 31, 5, books);
    secend = br.pos;
    if (secbits < 0 || br.err)
        return 1;

    if (skip_scalefactors(&br, books, c.bandcnt, global_gain) != 0)
        return 1;
    if ((int)br_bits(&br, LEN_PULSE_PRES) != 0) return 1;
    if ((int)br_bits(&br, LEN_TNS_PRES) != 0) return 1;
    if ((int)br_bits(&br, LEN_GAIN_PRES) != 0) return 1;

    spstart = br.pos;
    for (b = 0; b < c.bandcnt; b++) {
        int bk = books[b];
        if (bk == HCB_ZERO || bk == HCB_PNS ||
            bk == HCB_INTENSITY || bk == HCB_INTENSITY2) {
            if (c.book[b] != bk) {
                fprintf(stderr, "special identity lost in frame band %d\n", b);
                rt = 0;
            }
            continue;
        }
        if (bk < HCB_1 || bk > HCB_ESC || c.book[b] != bk) {
            fprintf(stderr, "frame book mismatch at band %d: %d vs %d\n",
                    b, bk, c.book[b]);
            rt = 0;
            break;
        }
        if (decode_band(&br, bk, 4, qfromf + nfromf) != 0) {
            rt = 0;
            break;
        }
        nfromf += 4;
    }
    spend = br.pos;

    /* The frame must end exactly at the terminator and padding. */
    if (br.err || (int)br_bits(&br, LEN_SE_ID) != ID_END)
        rt = 0;
    while (br.pos < total)
        if (br_bit(&br) != 0) { rt = 0; break; }
    if (br.pos != total)
        rt = 0;
    if (nfromf != QFRAME_COEFS - specialcount * 4) {
        fprintf(stderr, "frame spectrum length %d is short\n", nfromf);
        rt = 0;
    }

    /* The encoder's own emitted spectrum, decoded from coder->s with the
     * same reader; the frame must carry it exactly. */
    {
        static unsigned char sbuf[1 << 16];
        BitStream sbs;
        BitReader sbr;
        int di = 0;
        InitBitStream(&sbs, sbuf, sizeof(sbuf));
        for (i = 0; i < c.datacnt; i++)
            PutBit(&sbs, (uint32_t)c.s[i].data, c.s[i].len);
        sbr.buf = sbuf;
        sbr.cap = (long)sbs.currentBit;
        sbr.pos = 0;
        sbr.err = 0;
        for (b = 0; b < c.bandcnt; b++) {
            int bk = c.book[b];
            if (bk == HCB_ZERO || bk == HCB_PNS ||
                bk == HCB_INTENSITY || bk == HCB_INTENSITY2)
                continue;
            if (decode_band(&sbr, bk, 4, qfroms + di) != 0) {
                rt = 0;
                break;
            }
            di += 4;
        }
        if (sbr.err || sbr.pos != sbr.cap)
            rt = 0;
        nfroms = di;
        if (nfroms != nfromf || memcmp(qfroms, qfromf,
                                       sizeof(int) * (size_t)nfroms) != 0) {
            fprintf(stderr, "frame spectrum differs from encoder output\n");
            rt = 0;
        }
    }

    written = (int)((secend - secstart) + (spend - spstart));

    /* Independent minimum over the decoded spectrum and section books. */
    memset(&L, 0, sizeof(L));
    L.block_type = ONLY_LONG_WINDOW;
    L.sfbn = maxsfb;
    L.ngroup = 1;
    L.glen[0] = 1;
    for (b = 0; b < maxsfb; b++)
        L.width[b] = 4;
    for (b = 0; b < c.bandcnt; b++)
        L.special[b] = (c.book[b] >= HCB_1 && c.book[b] <= HCB_ESC) ? -1 : c.book[b];
    layout_prepare(&L);
    if (L.qlen != nfromf || L.qlen != QFRAME_COEFS - specialcount * 4) {
        fprintf(stderr, "layout qlen %d unexpected\n", L.qlen);
        return 1;
    }
    memcpy(L.q, qfromf, sizeof(int) * (size_t)L.qlen);
    minimum = ref_total(&L);

    if (specialcount == 0 || ordinarycount == 0) {
        fprintf(stderr, "frame did not exercise both band kinds\n");
        return 1;
    }
    if (!rt || written != minimum) {
        fprintf(stderr, "frame optimality: written %d minimum %d rt %d\n",
                written, minimum, rt);
        return 1;
    }

    r->written_bits = written;
    r->minimum_bits = minimum;
    r->roundtrip_ok = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registry.                                                           */
/* ------------------------------------------------------------------ */

static const struct section_case registry[] = {
    { "witness_244_243",    case_witness },
    { "long_31",            case_long_31 },
    { "short_7",            case_short_7 },
    { "short_14",           case_short_14 },
    { "group_restart",      case_group_restart },
    { "zero_identity",      case_zero_identity },
    { "pns_identity",       case_pns_identity },
    { "intensity_identity", case_intensity_identity },
    { "signed_spectrum",    case_signed_spectrum },
    { "escape_16_plus",     case_escape_16_plus },
    { "escape_only",        case_escape_only },
    { "quantize_frame",     case_quantize_frame }
};

const struct section_case *section_optimality_cases(size_t *count)
{
    *count = sizeof(registry) / sizeof(registry[0]);
    return registry;
}
