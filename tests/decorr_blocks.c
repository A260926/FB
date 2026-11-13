////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Independent decorrelation test support               //
//              Copyright (c) 2025 Conifer Software.                     //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// decorr_blocks.c
//
// Level-2 certification across WavPack block boundaries and the public
// decode/seek chain:
//
//   * multi-level mixed positive/negative term chains where the declared
//     metadata order differs from the decoder's execution order (mono and
//     stereo),
//   * whole-block reads and 1/7/17-frame interleaved reads producing
//     value-identical PCM and end-of-call state against one model sequence,
//   * cross-block state restoration from per-block metadata,
//   * public WavpackSeekSample64() re-reads and two concurrently open
//     instances reading in alternation,
//   * a real-encoder multichannel stream kept purely as a compatibility
//     reference (it never replaces a planned block).
//
// All expected PCM comes from the independent model in decorr_test.h.

#define DT_BLOCK_BEATS 24
#define DT_BLOCK_BEATS_LOCAL DT_BLOCK_BEATS
#include "decorr_test.h"
#include "decorr_observe.h"

#define DT_CHAIN_BLOCKS 3
#define DT_CHAIN_TOTAL (DT_BLOCK_BEATS * DT_CHAIN_BLOCKS)

static int failures;
static DtCounts counts;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf (stderr, "BLOCKS FAIL: " __VA_ARGS__); \
        fputc ('\n', stderr); \
        failures++; \
    } \
} while (0)

/* --------------------------------------------------------------------- */
/* Deterministic small asymmetric pseudo-random source of residuals        */
/* --------------------------------------------------------------------- */

static uint32_t rng_state;

static uint32_t xrnd (void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* residual pattern covering zero entries, negative half-grid values,
 * ordinary deltas and magnitudes that stay safely inside 24-bit PCM once
 * the decorrelation predictions are added */
static int32_t planned_residual (int beat, int chan, int blockno)
{
    static const int32_t specials[] = { 0, -1, 1, 0, -513, 513, 4000, -4000, 0, 3, -3 };
    int idx = (beat * 5 + chan * 11 + blockno * 7) %
              (int)(sizeof (specials) / sizeof (specials [0]));
    int32_t v = specials [idx];

    if (idx % 3 == 1)
        v += (int32_t)(xrnd () % 257) - 128;

    return v;
}

/* --------------------------------------------------------------------- */
/* Build one chain plan.  The declared order deliberately mixes negative   */
/* cross terms, ordinary delays and extrapolations so that declaration     */
/* order cannot equal execution order.                                     */
/* --------------------------------------------------------------------- */

static DtBlockPlan build_chain_plan (int mono, int blockno)
{
    DtBlockPlan pl;
    int i, c;

    memset (&pl, 0, sizeof (pl));
    pl.mono = mono;
    pl.bits = 24;
    pl.beats = DT_BLOCK_BEATS;
    pl.block_index = blockno * DT_BLOCK_BEATS;
    pl.total_samples = DT_CHAIN_TOTAL;
    pl.label = mono ? "chain-mono" : "chain-stereo";

    if (mono) {
        /* mono cannot use negative terms; mix delays and extrapolations */
        pl.nterms = 4;
        pl.terms [0].term = 17; pl.terms [0].delta = 2;
        pl.terms [1].term = 3;  pl.terms [1].delta = 1;
        pl.terms [2].term = 18; pl.terms [2].delta = 3;
        pl.terms [3].term = 1;  pl.terms [3].delta = 2;
    }
    else {
        /* declared order: -3, 2, -1, 17, 4 ; decoder executes the reverse */
        pl.nterms = 5;
        pl.terms [0].term = -3; pl.terms [0].delta = 2;
        pl.terms [1].term = 2;   pl.terms [1].delta = 1;
        pl.terms [2].term = -1;  pl.terms [2].delta = 3;
        pl.terms [3].term = 17;  pl.terms [3].delta = 2;
        pl.terms [4].term = 4;   pl.terms [4].delta = 1;
    }

    for (i = 0; i < pl.nterms; i++) {
        int term = pl.terms [i].term;

        /* Keep weights and histories modest so every 64-bit intermediate
         * lands inside int32: |w| <= ~300 and |history| <= ~4000 keeps the
         * worst weight*source product well under 2^31 even after the
         * extrapolation passes compound. */
        for (c = 0; c < 2; c++) {
            int sign = c ? -1 : 1;
            pl.terms [i].init_w [c] = mono && c ? 0 :
                sign * (40 + ((i * 37 + blockno * 23 + c * 19) % 240));

            if (term < 0) {
                pl.terms [i].init_hist [c] [0] =
                    (c ? -1 : 1) * (100 + ((i * 31 + blockno * 17) % 900));
            }
            else if (term > 8) {
                pl.terms [i].init_hist [c] [0] =
                    (c ? -1 : 1) * (200 + ((i * 29 + blockno * 13) % 1400));
                pl.terms [i].init_hist [c] [1] =
                    (c ? 1 : -1) * (100 + ((i * 23 + blockno * 11) % 900));
            }
            else {
                int k;
                for (k = 0; k < term; k++)
                    pl.terms [i].init_hist [c] [k] =
                        ((k + c) & 1 ? -1 : 1) *
                        (80 + ((i * 41 + k * 53 + blockno * 17 + c * 11) % 1200));
            }
        }
    }

    for (c = 0; c < 2; c++) {
        pl.init_median [c] [0] = 8 + blockno * 4;
        pl.init_median [c] [1] = 24 + blockno * 8;
        pl.init_median [c] [2] = 60 + blockno * 12;
    }

    for (i = 0; i < DT_BLOCK_BEATS; i++)
        for (c = 0; c < (mono ? 1 : 2); c++)
            pl.resid [i] [c] = planned_residual (i, c, blockno);

    return pl;
}

/* concatenate the per-block traces into one file-wide model sequence */
static void concat_trace (DtTrace *whole, const DtTrace *blk, int blockno)
{
    int base = blockno * DT_BLOCK_BEATS;
    int i;

    for (i = 0; i < blk->beats; i++) {
        whole->pcm [base + i] [0] = blk->pcm [i] [0];
        whole->pcm [base + i] [1] = blk->pcm [i] [1];
        whole->resid [base + i] [0] = blk->resid [i] [0];
        whole->resid [base + i] [1] = blk->resid [i] [1];
        whole->snap [base + i] = blk->snap [i];
    }

    whole->beats = base + blk->beats;
}

/* --------------------------------------------------------------------- */
/* Encoder output capture for the real-encoder compatibility reference     */
/* --------------------------------------------------------------------- */

typedef struct {
    unsigned char *buf;
    int len, cap;
} DtCap;

static int dt_cap_cb (void *id, void *data, int32_t bcount)
{
    DtCap *cap = (DtCap *) id;

    if (cap->len + bcount > cap->cap)
        return 0;

    memcpy (cap->buf + cap->len, data, (size_t) bcount);
    cap->len += bcount;
    return 1;
}

/* --------------------------------------------------------------------- */
/* Certification of one complete multi-block file                         */
/* --------------------------------------------------------------------- */

static int certify_chain (int mono)
{
    DtBlockPlan *plans[DT_CHAIN_BLOCKS];
    DtTrace *traces[DT_CHAIN_BLOCKS];
    static DtBlockPlan plan_store[DT_CHAIN_BLOCKS];
    static DtTrace trace_store[DT_CHAIN_BLOCKS];
    DtTrace whole;
    static unsigned char file[3u << 20];
    static int32_t out[DT_CHAIN_TOTAL * 2];
    uint32_t filelen = 0;
    int b, i, nch = mono ? 1 : 2;
    DtMemFile mf;
    char error[80] = "";

    memset (&whole, 0, sizeof (whole));
    rng_state = mono ? 0x12345678u : 0x9abcdef0u;

    for (b = 0; b < DT_CHAIN_BLOCKS; b++) {
        plan_store [b] = build_chain_plan (mono, b);
        plans [b] = &plan_store [b];
        traces [b] = &trace_store [b];
        dt_run_plan (plans [b], traces [b]);
        concat_trace (&whole, traces [b], b);

        uint32_t n = dt_build_block (plans [b], traces [b],
                                     file + filelen,
                                     (uint32_t) sizeof (file) - filelen);
        CHECK (n > 0, "chain block %d build failed", b);
        if (!n)
            return 1;
        filelen += n;
    }

    /* ---- read A: one whole-file pull ---- */
    mf.data = file;
    mf.len = (int64_t) filelen;
    mf.pos = 0;
    dt_obs_reset ();

    {
        WavpackContext *wpc = WavpackOpenFileInputEx64 (&dt_mem_reader, &mf, NULL, error, 0, 0);
        CHECK (wpc != NULL, "chain open failed: %s", error);
        if (!wpc)
            return 1;

        CHECK (WavpackGetNumChannels (wpc) == nch, "channel count");
        CHECK ((int) WavpackGetNumSamples64 (wpc) == DT_CHAIN_TOTAL, "total sample count");

        uint32_t got = WavpackUnpackSamples (wpc, out, DT_CHAIN_TOTAL);
        CHECK (got == DT_CHAIN_TOTAL, "whole read returned %u", got);
        CHECK (!WavpackGetNumErrors (wpc), "whole read errors");

        for (i = 0; i < DT_CHAIN_TOTAL * nch; i++) {
            int beat = i / nch, chan = i % nch;
            counts.pcm_compare++;
            if (out [i] != whole.pcm [beat] [chan])
                CHECK (0, "whole-read pcm diff beat=%d chan=%d got=%d want=%d",
                       beat, chan, out [i], whole.pcm [beat] [chan]);
        }

        if (dt_compare_call (&counts, plans, traces, DT_CHAIN_BLOCKS, 0, DT_CHAIN_TOTAL))
            CHECK (0, "whole-read state mismatch");

        WavpackCloseFile (wpc);
    }

    /* ---- read B: 1-frame interleaved pulls ---- */
    {
        static int32_t buf[2];
        WavpackContext *wpc;
        mf.pos = 0;
        wpc = WavpackOpenFileInputEx64 (&dt_mem_reader, &mf, NULL, error, 0, 0);
        CHECK (wpc != NULL, "1-frame open failed");

        for (i = 0; i < DT_CHAIN_TOTAL; i++) {
            dt_obs_reset ();
            CHECK (WavpackUnpackSamples (wpc, buf, 1) == 1, "1-frame pull at %d", i);

            for (int c = 0; c < nch; c++) {
                counts.pcm_compare++;
                if (buf [c] != whole.pcm [i] [c])
                    CHECK (0, "1-frame pcm diff beat=%d chan=%d", i, c);
            }

            if (dt_compare_call (&counts, plans, traces, DT_CHAIN_BLOCKS, i, 1))
                CHECK (0, "1-frame state mismatch beat=%d", i);
        }

        CHECK (!WavpackGetNumErrors (wpc), "1-frame errors");
        WavpackCloseFile (wpc);
    }

    /* ---- read C: 7-frame and 17-frame alternating pulls ---- */
    {
        static int32_t buf[17 * 2];
        WavpackContext *wpc;
        int pos = 0, step = 7;
        mf.pos = 0;
        dt_obs_reset ();
        wpc = WavpackOpenFileInputEx64 (&dt_mem_reader, &mf, NULL, error, 0, 0);
        CHECK (wpc != NULL, "7/17-frame open failed");

        while (pos < DT_CHAIN_TOTAL) {
            int ask = step;
            if (pos + ask > DT_CHAIN_TOTAL)
                ask = DT_CHAIN_TOTAL - pos;

            uint32_t got = WavpackUnpackSamples (wpc, buf, (uint32_t) ask);
            CHECK ((int) got == ask, "7/17 pull at %d got %u want %d", pos, got, ask);

            for (i = 0; i < ask * nch; i++) {
                int beat = pos + i / nch, chan = i % nch;
                counts.pcm_compare++;
                if (buf [i] != whole.pcm [beat] [chan])
                    CHECK (0, "7/17 pcm diff beat=%d chan=%d", beat, chan);
            }

            if (dt_compare_call (&counts, plans, traces, DT_CHAIN_BLOCKS, pos, ask))
                CHECK (0, "7/17 state mismatch pos=%d", pos);

            pos += ask;
            step = (step == 7) ? 17 : 7;
        }

        CHECK (!WavpackGetNumErrors (wpc), "7/17 errors");
        WavpackCloseFile (wpc);
    }

    /* ---- public seek then re-read; targets land in different blocks ---- */
    {
        static const int seek_targets[] = { 5, 23, 24, 30, 47, 48, 60 };
        WavpackContext *wpc;
        mf.pos = 0;
        wpc = WavpackOpenFileInputEx64 (&dt_mem_reader, &mf, NULL, error, 0, 0);
        CHECK (wpc != NULL, "seek open failed");

        for (unsigned k = 0; k < sizeof (seek_targets) / sizeof (seek_targets [0]); k++) {
            int target = seek_targets [k];
            static int32_t buf[8 * 2];
            int ask = DT_CHAIN_TOTAL - target;
            if (ask > 8)
                ask = 8;

            CHECK (WavpackSeekSample64 (wpc, target), "seek to %d", target);
            dt_obs_reset ();   /* discard records emitted while skipping */
            CHECK (WavpackUnpackSamples (wpc, buf, (uint32_t) ask) == (uint32_t) ask,
                   "post-seek read at %d", target);

            for (i = 0; i < ask * nch; i++) {
                int beat = target + i / nch, chan = i % nch;
                counts.pcm_compare++;
                if (buf [i] != whole.pcm [beat] [chan])
                    CHECK (0, "seek pcm diff beat=%d chan=%d", beat, chan);
            }

            if (dt_compare_call (&counts, plans, traces, DT_CHAIN_BLOCKS, target, ask))
                CHECK (0, "post-seek state mismatch target=%d", target);
        }

        CHECK (!WavpackGetNumErrors (wpc), "seek errors");
        WavpackCloseFile (wpc);
    }

    /* ---- two independent instances reading in alternation ---- */
    {
        DtMemFile mf1 = { file, (int64_t) filelen, 0 };
        DtMemFile mf2 = { file, (int64_t) filelen, 0 };
        WavpackContext *w1 = WavpackOpenFileInputEx64 (&dt_mem_reader, &mf1, NULL, error, 0, 0);
        WavpackContext *w2 = WavpackOpenFileInputEx64 (&dt_mem_reader, &mf2, NULL, error, 0, 0);
        static int32_t b1[3 * 2], b2[5 * 2];
        int p1 = 0, p2 = 0;

        CHECK (w1 && w2, "dual-instance open failed");

        while (p1 < DT_CHAIN_TOTAL || p2 < DT_CHAIN_TOTAL) {
            if (p1 < DT_CHAIN_TOTAL) {
                int ask = DT_CHAIN_TOTAL - p1;
                if (ask > 3) ask = 3;
                dt_obs_reset ();
                CHECK (WavpackUnpackSamples (w1, b1, (uint32_t) ask) == (uint32_t) ask, "dual w1");
                for (i = 0; i < ask * nch; i++) {
                    int beat = p1 + i / nch, chan = i % nch;
                    counts.pcm_compare++;
                    if (b1 [i] != whole.pcm [beat] [chan])
                        CHECK (0, "dual w1 pcm diff beat=%d", beat);
                }
                if (dt_compare_call (&counts, plans, traces, DT_CHAIN_BLOCKS, p1, ask))
                    CHECK (0, "dual w1 state mismatch p1=%d", p1);
                p1 += ask;
            }
            if (p2 < DT_CHAIN_TOTAL) {
                int ask = DT_CHAIN_TOTAL - p2;
                if (ask > 5) ask = 5;
                dt_obs_reset ();
                CHECK (WavpackUnpackSamples (w2, b2, (uint32_t) ask) == (uint32_t) ask, "dual w2");
                for (i = 0; i < ask * nch; i++) {
                    int beat = p2 + i / nch, chan = i % nch;
                    counts.pcm_compare++;
                    if (b2 [i] != whole.pcm [beat] [chan])
                        CHECK (0, "dual w2 pcm diff beat=%d", beat);
                }
                if (dt_compare_call (&counts, plans, traces, DT_CHAIN_BLOCKS, p2, ask))
                    CHECK (0, "dual w2 state mismatch p2=%d", p2);
                p2 += ask;
            }
        }

        CHECK (!WavpackGetNumErrors (w1) && !WavpackGetNumErrors (w2), "dual errors");
        WavpackCloseFile (w1);
        WavpackCloseFile (w2);
    }

    if (dt_obs_dropped ())
        CHECK (0, "dropped %ld observations", dt_obs_dropped ());

    return failures != 0;
}

/* --------------------------------------------------------------------- */
/* Real-encoder multichannel compatibility reference.  Exercises the same  */
/* public open/decode path on library-produced blocks, but can never stand */
/* in for a planned block because term selection stays ours in those.      */
/* --------------------------------------------------------------------- */

static int certify_real_encoder_compat (void)
{
    enum { FRAMES = 193, CHANS = 6 };
    static int32_t original[FRAMES * CHANS], work[FRAMES * CHANS], out[9 * CHANS];
    static unsigned char wvbuf[1u << 20];
    DtCap cap = { wvbuf, 0, (int) sizeof (wvbuf) };
    DtMemFile mf;
    char error[80] = "";
    int f, c, i;

    for (f = 0; f < FRAMES; f++)
        for (c = 0; c < CHANS; c++)
            original [f * CHANS + c] =
                (int32_t)(((f * 7919u + c * 137u) % 200000) - 100000) +
                (c & 1 ? f * 9 : -f * 5);

    memcpy (work, original, sizeof (work));

    WavpackContext *w = WavpackOpenFileOutput (dt_cap_cb, &cap, NULL);
    CHECK (w != NULL, "compat encoder open");
    if (!w)
        return 1;

    WavpackConfig cfg;
    memset (&cfg, 0, sizeof (cfg));
    cfg.bits_per_sample = 24;
    cfg.bytes_per_sample = 3;
    cfg.num_channels = CHANS;
    cfg.channel_mask = 0x3f;
    cfg.sample_rate = 48000;
    cfg.block_samples = 64;
    cfg.flags = CONFIG_HIGH_FLAG;

    CHECK (WavpackSetConfiguration64 (w, &cfg, FRAMES, NULL), "compat config");
    CHECK (WavpackPackInit (w), "compat init");
    CHECK (WavpackPackSamples (w, work, FRAMES), "compat pack");
    CHECK (WavpackFlushSamples (w), "compat flush");
    WavpackCloseFile (w);

    CHECK (cap.len > 0, "compat produced no bytes");

    mf.data = wvbuf;
    mf.len = cap.len;
    mf.pos = 0;

    WavpackContext *d = WavpackOpenFileInputEx64 (&dt_mem_reader, &mf, NULL, error, 0, 0);
    CHECK (d != NULL, "compat decode open: %s", error);
    if (!d)
        return 1;

    CHECK (WavpackGetNumChannels (d) == CHANS, "compat channels");

    unsigned total = 0;
    uint32_t n;
    while ((n = WavpackUnpackSamples (d, out, 9)) > 0) {
        CHECK (total + n <= FRAMES, "compat overrun");
        for (unsigned j = 0; j < n * CHANS; j++) {
            counts.pcm_compare++;
            if (out [j] != original [total * CHANS + j])
                CHECK (0, "compat pcm diff at %u word %u", total, j);
        }
        total += n;
    }

    CHECK (total == FRAMES, "compat frame total %u", total);
    CHECK (!WavpackGetNumErrors (d), "compat decode errors");

    CHECK (WavpackSeekSample64 (d, 100), "compat seek");
    CHECK (WavpackUnpackSamples (d, out, 7) == 7, "compat post-seek read");
    for (i = 0; i < 7 * CHANS; i++)
        if (out [i] != original [100 * CHANS + i])
            CHECK (0, "compat seek pcm diff word %d", i);

    WavpackCloseFile (d);
    return 0;
}

int main (void)
{
    dt_counts_init (&counts);

    if (certify_chain (1))
        CHECK (0, "mono chain certification failed");

    if (certify_chain (0))
        CHECK (0, "stereo chain certification failed");

    if (certify_real_encoder_compat ())
        CHECK (0, "real-encoder compatibility failed");

    printf ("BLOCKS multi-level chain comparison counts:\n");
    for (int term = -3; term <= 18; term++) {
        if (term == 0)
            continue;
        if (counts.term_compare [term + 3])
            printf ("  term %3d: %ld state comparisons\n", term, counts.term_compare [term + 3]);
    }
    printf ("  chains=%d pcm=%ld weights=%ld history=%ld residuals=%ld\n",
            2, counts.pcm_compare, counts.weight_compare,
            counts.history_compare, counts.residual_compare);

    if (failures || counts.failed) {
        printf ("DECORRELATION BLOCKS: FAIL (%d)\n", failures);
        return 1;
    }

    printf ("DECORRELATION BLOCKS: PASS\n");
    return 0;
}
