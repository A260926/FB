/*
 * FAAC - Freeware Advanced Audio Coder
 * Huffman coding per ISO/IEC 14496-3
 * Copyright (C) 2026 Nils Schimmelmann
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "coder.h"
#include "huffdata.h"
#include "huff2.h"
#include "bitstream.h"
#include "util.h"

/* Escape coding for HCB_ESC as per ISO/IEC 14496-3.
 * Represents values |q| >= 16 by sending 16 plus an escape suffix. */
static int escape(int x, int *code)
{
    if (x > MAX_HUFF_ESC_VAL) {
        fprintf(stderr, "Huffman escape value out of range: %d\n", x);
        return 0;
    }

    int preflen = 31 - CountLeadingZeros(x) - 4;
    int base = 1 << (preflen + 4);

    if (code) {
        /* Unary prefix: preflen 1s followed by a 0 */
        *code = (1 << (preflen + 1)) - 2;
        /* Escape suffix is (preflen+4) bits: base starts at 16 (= 2^4), so the
         * value field is always at least 4 bits; each additional doubling adds one. */
        *code = (*code << (preflen + 4)) | (x - base);
    }

    return (preflen + 1) + (preflen + 4);
}

static const hcode16_t * const hmap[12] = {
    NULL, book01, book02, book03, book04, book05,
    book06, book07, book08, book09, book10, book11
};


/* Both books of a pair share the index expression; only the table differs.
 * One walk, two lookups.
 *
 * ISO 14496-3 multidimensional Huffman section tuple indexing. Constant dimensions
 * (DIM_S4, DIM_M4, DIM_S2) allow constant folding into shift-adds.
 *
 * bnum is HCB_1, HCB_3 or HCB_5; size_books walks the unsigned pair books
 * itself. HCB_3 leaves out its sign bits, which size_books adds. */
static void huffcode_size_pair(const int * __restrict qs, int len, int bnum, int *bits_a, int *bits_b)
{
    const hcode16_t *booka = hmap[bnum];
    const hcode16_t *bookb = hmap[bnum + 1];
    int a = 0, b = 0;
    int i;

    switch (bnum) {
    case HCB_1:
        for (i = 0; i < len; i += 4) {
            int idx = 40 + DIM_S4*DIM_S4*DIM_S4 * qs[i] + DIM_S4*DIM_S4 * qs[i+1] + DIM_S4 * qs[i+2] + qs[i+3];
            a += booka[idx].len;
            b += bookb[idx].len;
        }
        break;
    case HCB_3:
        for (i = 0; i < len; i += 4) {
            int a0 = abs(qs[i]), a1 = abs(qs[i+1]), a2 = abs(qs[i+2]), a3 = abs(qs[i+3]);
            int idx = DIM_M4*DIM_M4*DIM_M4 * a0 + DIM_M4*DIM_M4 * a1 + DIM_M4 * a2 + a3;
            a += booka[idx].len;
            b += bookb[idx].len;
        }
        break;
    case HCB_5:
        for (i = 0; i < len; i += 2) {
            int idx = 40 + DIM_S2 * qs[i] + qs[i+1];
            a += booka[idx].len;
            b += bookb[idx].len;
        }
        break;
    default:
        break;
    }

    *bits_a = a;
    *bits_b = b;
}

/* Sizes a band in every book from lo up. The unsigned pair books share one
 * walk: magnitudes are clamped to each table, and a book whose LAV the band
 * exceeds is sized but never picked, as lo is above it. Unsigned books pay one
 * sign bit per nonzero value, counted once. */
static void size_books(const int * __restrict qs, int len, int lo, int * __restrict c)
{
    int i, nnz = 0;

    for (i = 0; i < len; i++)
        nnz += qs[i] != 0;
    for (i = lo; i < HCB_7; i += 2) {
        huffcode_size_pair(qs, len, i, &c[i], &c[i + 1]);
        if (i == HCB_3) {
            c[i] += nnz;
            c[i + 1] += nnz;
        }
    }
    for (i = HCB_7; i <= HCB_ESC; i++)
        c[i] = nnz;
    for (i = 0; i < len; i += 2) {
        int x0 = abs(qs[i]), x1 = abs(qs[i + 1]);
        int i7 = DIM_M2_7 * ((x0 > LAV_7) ? LAV_7 : x0) + ((x1 > LAV_7) ? LAV_7 : x1);
        int i9 = DIM_M2_12 * ((x0 > LAV_12) ? LAV_12 : x0) + ((x1 > LAV_12) ? LAV_12 : x1);
        int ie = DIM_ESC * ((x0 > LAV_ESC) ? LAV_ESC : x0) + ((x1 > LAV_ESC) ? LAV_ESC : x1);
        c[HCB_7] += book07[i7].len;
        c[HCB_8] += book08[i7].len;
        c[HCB_9] += book09[i9].len;
        c[HCB_10] += book10[i9].len;
        c[HCB_ESC] += book11[ie].len + ((x0 >= LAV_ESC) ? escape(x0, NULL) : 0)
                    + ((x1 >= LAV_ESC) ? escape(x1, NULL) : 0);
    }
}

/* Appends the band's codewords to coder->s. */
static void huffcode_write(const int * __restrict qs, int len, int bnum, CoderInfo *coder)
{
    const hcode16_t *book = hmap[bnum];
    int i;
    int datacnt = coder->datacnt;

    switch (bnum) {
    case HCB_1:
    case HCB_2:
        for (i = 0; i < len; i += 4) {
            int idx = 40 + DIM_S4*DIM_S4*DIM_S4 * qs[i] + DIM_S4*DIM_S4 * qs[i+1] + DIM_S4 * qs[i+2] + qs[i+3];
            coder->s[datacnt].data = book[idx].data;
            coder->s[datacnt++].len = book[idx].len;
        }
        break;
    case HCB_3:
    case HCB_4:
        for (i = 0; i < len; i += 4) {
            int q0 = qs[i], q1 = qs[i+1], q2 = qs[i+2], q3 = qs[i+3];
            int a0 = abs(q0), a1 = abs(q1), a2 = abs(q2), a3 = abs(q3);
            int idx = DIM_M4*DIM_M4*DIM_M4 * a0 + DIM_M4*DIM_M4 * a1 + DIM_M4 * a2 + a3;
            int blen = book[idx].len;
            int data = book[idx].data;
            if (q0) { blen++; data = (data << 1) | (q0 < 0); }
            if (q1) { blen++; data = (data << 1) | (q1 < 0); }
            if (q2) { blen++; data = (data << 1) | (q2 < 0); }
            if (q3) { blen++; data = (data << 1) | (q3 < 0); }
            coder->s[datacnt].data = data;
            coder->s[datacnt++].len = blen;
        }
        break;
    case HCB_5:
    case HCB_6:
        for (i = 0; i < len; i += 2) {
            int idx = 40 + DIM_S2 * qs[i] + qs[i+1];
            coder->s[datacnt].data = book[idx].data;
            coder->s[datacnt++].len = book[idx].len;
        }
        break;
    case HCB_7:
    case HCB_8:
        for (i = 0; i < len; i += 2) {
            int q0 = qs[i], q1 = qs[i+1];
            int a0 = abs(q0), a1 = abs(q1);
            int idx = DIM_M2_7 * a0 + a1;
            int blen = book[idx].len;
            int data = book[idx].data;
            if (q0) { blen++; data = (data << 1) | (q0 < 0); }
            if (q1) { blen++; data = (data << 1) | (q1 < 0); }
            coder->s[datacnt].data = data;
            coder->s[datacnt++].len = blen;
        }
        break;
    case HCB_9:
    case HCB_10:
        for (i = 0; i < len; i += 2) {
            int q0 = qs[i], q1 = qs[i+1];
            int a0 = abs(q0), a1 = abs(q1);
            int idx = DIM_M2_12 * a0 + a1;
            int blen = book[idx].len;
            int data = book[idx].data;
            if (q0) { blen++; data = (data << 1) | (q0 < 0); }
            if (q1) { blen++; data = (data << 1) | (q1 < 0); }
            coder->s[datacnt].data = data;
            coder->s[datacnt++].len = blen;
        }
        break;
    case HCB_ESC:
        for (i = 0; i < len; i += 2) {
            int x0 = abs(qs[i]), x1 = abs(qs[i+1]);
            int v0 = (x0 > LAV_ESC) ? LAV_ESC : x0;
            int v1 = (x1 > LAV_ESC) ? LAV_ESC : x1;
            int idx = DIM_ESC * v0 + v1;
            int blen = book[idx].len;
            int data = book[idx].data;
            if (qs[i]) {
                blen++;
                data = (data << 1) | (qs[i] < 0);
            }
            if (qs[i+1]) {
                blen++;
                data = (data << 1) | (qs[i+1] < 0);
            }
            coder->s[datacnt].data = data;
            coder->s[datacnt++].len = blen;
            if (x0 >= LAV_ESC) {
                int esc_code = 0;
                int esc_len = escape(x0, &esc_code);
                coder->s[datacnt].data = esc_code;
                coder->s[datacnt++].len = esc_len;
            }
            if (x1 >= LAV_ESC) {
                int esc_code = 0;
                int esc_len = escape(x1, &esc_code);
                coder->s[datacnt].data = esc_code;
                coder->s[datacnt++].len = esc_len;
            }
        }
        break;
    default:
        break;
    }

    coder->datacnt = datacnt;
}

/* Choose every band's book jointly: per window group, a Viterbi over books
 * where a band costs its codewords in that book and a section costs exactly
 * what writebooks emits for it: a 4-bit book number followed by its run
 * length in fields of max_run, where every full field means "more follows"
 * and the final field (a zero remainder after an exact multiple too)
 * terminates the section. A run of R bands therefore costs
 * (floor(R / max_run) + 1) run fields, so extending a section is free except
 * when the run crosses a multiple of max_run.
 *
 * The state is the current book plus the run length modulo max_run. Special
 * books (zero, PNS, both intensity codes) keep their own identity and carry
 * no spectrum cost: a special band has exactly one candidate book, and a
 * different special value is a different section. qs holds the regular
 * bands' values back to back. Sections never merge across a window-group
 * boundary. Emits the codewords once the books are known. */
void huffbook(CoderInfo *coder, const int *qs)
{
    /* States are indexed by the book value writebooks will emit: 0..11 are
     * the spectral books, 13..16 the special ones. HCB_DELTA (12) never
     * names a spectral section, so its slot stays unused. */
    enum { NBOOK = HCB_NONE + 1, MAX_RUN = 31 };
    enum { MAX_STATES = NBOOK * MAX_RUN };
    /* For each band and book, how the run-state 1 there was reached: an
     * opening after some other book's state, or a continuation from that
     * book's run-state 0. Every other run-state is an unambiguous
     * continuation. 0xFFFF marks the continuation. */
    unsigned short src1[MAX_SCFAC_BANDS][NBOOK];
    int dp[MAX_STATES], ndp[MAX_STATES];
    int run_bits = (coder->block_type == ONLY_SHORT_WINDOW) ? 3 : 5;
    int max_run = (coder->block_type == ONLY_SHORT_WINDOW) ? 7 : 31;
    int open_cost = 4 + run_bits;
    int g, b, k, off = 0;
    const int INF = INT_MAX / 4;

    for (g = 0, b = 0; g < coder->groups.n; g++) {
        int start = b;
        int end = b + coder->sfbn;
        int state, prev, nstates = NBOOK * max_run;

        for (state = 0; state < nstates; state++)
            dp[state] = INF;

        for (; b < end; b++) {
            int book = coder->book[b], lo, hi, cost[NBOOK], c[HCB_ESC + 1];
            int sfb = b % coder->sfbn;
            int len = (coder->sfb_offset[sfb + 1] - coder->sfb_offset[sfb])
                    * coder->groups.len[g];

            for (k = 0; k < NBOOK; k++)
                cost[k] = INF;
            if (book >= HCB_1 && book <= HCB_ESC) {
                int first = ((book - 1) & ~1) + 1;
                lo = first;
                hi = HCB_ESC;
                size_books(qs + off, len, first, c);
                for (k = lo; k <= hi; k++)
                    cost[k] = c[k];
                off += len;
            } else {
                /* A special band has one book only: its own identity. */
                lo = hi = (book >= 0 && book <= HCB_NONE) ? book : HCB_NONE;
                cost[lo] = 0;
            }

            if (b == start) {
                /* First band of a group: only a section can open; nothing
                 * merges in from the previous group, whatever its book. */
                for (k = lo; k <= hi; k++)
                    dp[k * max_run + 1] = (cost[k] < INF) ? open_cost + cost[k] : INF;
            } else {
                /* Cheapest state of every book in the previous band, with
                 * the run it sat at, so both continuations and openings can
                 * see it. */
                int same[NBOOK], same_run[NBOOK];

                for (k = 0; k < NBOOK; k++) {
                    int m = INF, arg = 0;
                    for (state = 0; state < max_run; state++) {
                        int v = dp[k * max_run + state];
                        if (v < m) { m = v; arg = state; }
                    }
                    same[k] = m;
                    same_run[k] = arg;
                }

                for (state = 0; state < nstates; state++)
                    ndp[state] = INF;
                for (k = 0; k < NBOOK; k++)
                    src1[b][k] = 0xFFFF;

                for (k = lo; k <= hi; k++) {
                    int cst = cost[k];
                    int other, obook = -1, orun = 0;

                    if (cst >= INF)
                        continue;

                    /* Continue this book's section: one more band, plus one
                     * more run field when the run reaches max_run.  A
                     * continuation out of run-state 0 lands in run-state 1
                     * too, so it must clear the opening mark if it wins. */
                    for (prev = 0; prev < max_run; prev++) {
                        int v = dp[k * max_run + prev];
                        int nr, nv;
                        if (v >= INF)
                            continue;
                        nr = prev + 1;
                        nv = v + cst;
                        if (nr == max_run) { nr = 0; nv += run_bits; }
                        if (nv < ndp[k * max_run + nr]) {
                            ndp[k * max_run + nr] = nv;
                            if (nr == 1)
                                src1[b][k] = 0xFFFF;
                        }
                    }

                    /* Or open a section in this book after one ending in a
                     * different book; same-book neighbours would be emitted
                     * as one run, so they must continue instead. */
                    other = INF;
                    for (state = 0; state < NBOOK; state++) {
                        if (state != k && same[state] < other) {
                            other = same[state];
                            obook = state;
                            orun = same_run[state];
                        }
                    }
                    if (other < INF) {
                        int nv = other + open_cost + cst;
                        int slot = k * max_run + 1;
                        if (nv < ndp[slot]) {
                            ndp[slot] = nv;
                            src1[b][k] = (unsigned short)(obook * max_run + orun);
                        }
                    }
                }

                for (state = 0; state < nstates; state++)
                    dp[state] = ndp[state];
            }
        }

        /* Backtrack from the cheapest final state, reconstructing each
         * band's book. Special bands were never candidates here, so their
         * own books stay untouched. */
        {
            int kbest = -1, rbest = 0, m = INF;
            for (state = 0; state < nstates; state++) {
                if (dp[state] < m) {
                    m = dp[state];
                    kbest = state / max_run;
                    rbest = state % max_run;
                }
            }
            for (b = end - 1; b >= start; b--) {
                if (kbest < 0)
                    break;
                if (kbest >= HCB_1 && kbest <= HCB_ESC)
                    coder->book[b] = kbest;
                if (b == start)
                    break;
                if (rbest == 0) {
                    rbest = max_run - 1;          /* continued at a boundary */
                } else if (rbest == 1) {
                    if (src1[b][kbest] == 0xFFFF) {
                        rbest = 0;                /* continued in this book */
                    } else {
                        unsigned short p2 = src1[b][kbest];
                        kbest = p2 / max_run;     /* opened after another book */
                        rbest = p2 % max_run;
                    }
                } else {
                    rbest--;                      /* continued in this book */
                }
            }
        }
        b = end;
    }

    for (b = 0, off = 0; b < coder->bandcnt; b++) {
        int book = coder->book[b];
        if (book >= HCB_1 && book <= HCB_ESC) {
            int sfb = b % coder->sfbn;
            int len = (coder->sfb_offset[sfb + 1] - coder->sfb_offset[sfb]) * coder->groups.len[b / coder->sfbn];
            huffcode_write(qs + off, len, book, coder);
            off += len;
        }
    }
}

/* Encode the section data (codebook indices and run lengths). */
int writebooks(CoderInfo *coder, BitStream *stream)
{
    int bits = 0;
    /* Section run field is 3 bits for short windows (max 7 windows/section) and
     * 5 bits for long windows (max 31 bands/section) — ISO 14496-3 §4.6.8.2. */
    int max_run = (coder->block_type == ONLY_SHORT_WINDOW) ? 7 : 31;
    int run_bits = (coder->block_type == ONLY_SHORT_WINDOW) ? 3 : 5;
    int g;
    BitAccumulator acc = {0};

    AccumBegin(&acc, stream);

    for (g = 0; g < coder->groups.n; g++) {
        int b = g * coder->sfbn;
        int end = b + coder->sfbn;
        while (b < end) {
            int book = coder->book[b];
            int run = 0;
            while (b + run < end && coder->book[b + run] == book) run++;
            b += run;

            AccumPutBits(&acc, (uint32_t)book, 4);
            bits += 4;

            while (run >= max_run) {
                AccumPutBits(&acc, (uint32_t)max_run, run_bits);
                bits += run_bits;
                run -= max_run;
            }
            AccumPutBits(&acc, (uint32_t)run, run_bits);
            bits += run_bits;
        }
    }

    AccumEnd(&acc);
    return bits;
}

/* Encode scalefactor deltas using HCB_DELTA (book12). */
int writesf(CoderInfo *coder, BitStream *stream)
{
    int i, bits = 0;
    int lastsf = coder->global_gain;
    int lastis = 0;
    int lastpns = coder->global_gain - SF_PNS_OFFSET;
    int is_first_pns = 1;
    BitAccumulator acc = {0};

    AccumBegin(&acc, stream);

    for (i = 0; i < coder->bandcnt; i++) {
        int book = coder->book[i];
        int val = coder->sf[i];
        int diff, code, len;

        if (book == HCB_ZERO || book == HCB_NONE) continue;

        if (book == HCB_INTENSITY || book == HCB_INTENSITY2) {
            diff = clamp_sf_diff(val - lastis);
            lastis += diff;
        } else if (book == HCB_PNS) {
            diff = val - lastpns;
            if (is_first_pns) {
                /* First PNS band is coded as an absolute 9-bit value (biased by 256)
                 * because there is no prior PNS entry to delta from yet. */
                AccumPutBits(&acc, (uint32_t)(diff + 256), 9);
                bits += 9;
                lastpns = val;
                is_first_pns = 0;
                continue;
            }
            diff = clamp_sf_diff(diff);
            lastpns += diff;
        } else {
            diff = clamp_sf_diff(val - lastsf);
            lastsf += diff;
        }

        code = book12[SF_DELTA + diff].data;
        len = book12[SF_DELTA + diff].len;
        AccumPutBits(&acc, (uint32_t)code, len);
        bits += len;
    }

    AccumEnd(&acc);
    return bits;
}
