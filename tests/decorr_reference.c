////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Independent decorrelation test support               //
//              Copyright (c) 2025 Conifer Software.                     //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// decorr_reference.c
//
// Level-1 certification of the independent natural-time decorrelation
// model: beat-by-beat numeric invariants (floor rounding, sign-based weight
// updates, negative-term clipping, positive-term non-clipping, the -1/-2/-3
// left/right dependencies and the two-point extrapolations), followed by a
// whole-block public-chain decode of every planned case through
// WavpackOpenFileInputEx64() / WavpackUnpackSamples().
//
// The production decorrelation functions are never called; the expected PCM
// is produced solely by the model in decorr_test.h.

#include "decorr_test.h"
#include "decorr_observe.h"

static int failures;
static DtCounts counts;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf (stderr, "REFERENCE FAIL: " __VA_ARGS__); \
        fputc ('\n', stderr); \
        failures++; \
    } \
} while (0)

/* ---- explicit arithmetic edge cases that do not even need a block ---- */

static void check_apply_floor (void)
{
    struct { int32_t w, s, expect; } cases[] = {
        {    1,  512,    1 },   /* exact */
        {    1,  511,    1 },   /* (1023)/1024 floor = 0? see below */
        {    1, -513,   -1 },   /* (-1)/1024 must floor to -1, not truncate to 0 */
        {   -1,  513,   -1 },
        {    1, -1536,  -1 },   /* (-1024)/1024 */
        { -512, 1000, -500 },   /* (-512000+512)/1024 = -499.5 -> -500 */
        {  512, 1000,  500 },
        { 1024,  777,  777 },   /* unity weight */
        { 1023,    1,    1 },   /* (1535)/1024 = 1 */
        { 1023,   -1,   -1 },   /* (-511)/1024 floor = -1 */
        {    3,  341,    1 },   /* (1535)/1024 = 1 */
        {    3, -341,   -1 }    /* (-511)/1024 = -1 */
    };
    int i;

    /* fix the one illustrative expectation: (511+512)/1024 = 1023/1024 -> 0 */
    cases [1].expect = 0;

    for (i = 0; i < (int)(sizeof (cases) / sizeof (cases [0])); i++) {
        int32_t got = dt_apply (cases [i].w, cases [i].s);
        CHECK (got == cases [i].expect,
               "dt_apply(%d,%d)=%d want %d", cases [i].w, cases [i].s, got, cases [i].expect);
    }
}

static void check_update_signs (void)
{
    /* same sign -> up, opposite -> down, zero source/residual -> frozen */
    CHECK (dt_update (100, 2, 5, 7, 0) == 102, "same-sign update");
    CHECK (dt_update (100, 2, -5, -7, 0) == 102, "same-sign negative update");
    CHECK (dt_update (100, 2, 5, -7, 0) == 98, "opposite-sign update");
    CHECK (dt_update (100, 2, 0, 7, 0) == 100, "zero source freezes weight");
    CHECK (dt_update (100, 2, 5, 0, 0) == 100, "zero residual freezes weight");

    /* negative terms clip at both ends; positive terms must not */
    CHECK (dt_update (1024, 4, 9, 9, 1) == 1024, "negative-term positive clip");
    CHECK (dt_update (-1024, 4, 9, -9, 1) == -1024, "negative-term negative clip");
    CHECK (dt_update (1020, 4, 9, 9, 1) == 1024, "negative-term rises into clip");
    CHECK (dt_update (-1020, 4, 9, -9, 1) == -1024, "negative-term falls into clip");
    CHECK (dt_update (1024, 4, 9, 9, 0) == 1028, "positive term is allowed past +1024");
    CHECK (dt_update (-1024, 4, 9, -9, 0) == -1028, "positive term is allowed past -1024");
}

/* ---- plan helpers ---- */

static DtBlockPlan make_plan (const char *label, int mono, int bits, int beats,
                              const int terms[][2], int nterms,
                              const int32_t init_w[][2],
                              const int32_t init_hist[][2][DT_HIST_CAP])
{
    DtBlockPlan pl;
    int i, c;

    memset (&pl, 0, sizeof (pl));
    pl.label = label;
    pl.mono = mono;
    pl.bits = bits;
    pl.beats = beats;
    pl.nterms = nterms;
    pl.total_samples = beats;
    pl.block_index = 0;

    for (i = 0; i < nterms; i++) {
        pl.terms [i].term = terms [i] [0];
        pl.terms [i].delta = terms [i] [1];

        for (c = 0; c < 2; c++) {
            pl.terms [i].init_w [c] = init_w [i] [c];
            if (init_hist)
                memcpy (pl.terms [i].init_hist [c], init_hist [i] [c],
                        sizeof (pl.terms [i].init_hist [c]));
        }
    }

    for (c = 0; c < 2; c++) {
        pl.init_median [c] [0] = 16;
        pl.init_median [c] [1] = 32;
        pl.init_median [c] [2] = 64;
    }

    return pl;
}

static void set_resid (DtBlockPlan *pl, int beat, int chan, int32_t v)
{
    pl->resid [beat] [chan] = v;
}

/* asymmetric residual sequence: deliberately irregular, sign-changing */
static void fill_asym (DtBlockPlan *pl, int seed)
{
    static const int32_t seq[] = {
        0, 3, -1, 7, -5, 0, 11, -13, 2, 19, -23, 5,
        31, -37, 0, 9, -3, 17, -29, 41, -47, 11, 0, -2
    };
    int i, nch = pl->mono ? 1 : 2, c;

    for (i = 0; i < pl->beats; i++)
        for (c = 0; c < nch; c++)
            pl->resid [i] [c] = seq [(i * 3 + c * 7 + seed) % (int)(sizeof (seq) / sizeof (seq [0]))];
}

/* ---- model-only invariant checks on a trace ---- */

static void check_trace_invariants (const DtBlockPlan *pl, const DtTrace *tr)
{
    int pi, i, c;

    for (i = 0; i < tr->beats; i++)
        for (c = 0; c < (pl->mono ? 1 : 2); c++) {
            CHECK (tr->pcm [i] [c] >= INT32_MIN && tr->pcm [i] [c] <= INT32_MAX,
                   "%s pcm out of int32 at beat %d chan %d", pl->label, i, c);

            if (pl->bits == 16)
                CHECK (tr->pcm [i] [c] >= -32768 && tr->pcm [i] [c] <= 32767,
                       "%s 16-bit pcm out of range at beat %d chan %d", pl->label, i, c);
            else
                CHECK (tr->pcm [i] [c] >= -(1 << 24) && tr->pcm [i] [c] <= (1 << 24) - 1,
                       "%s 24-bit pcm out of range at beat %d chan %d", pl->label, i, c);
        }

    for (pi = 0; pi < pl->nterms; pi++) {
        int term = pl->terms [pl->nterms - 1 - pi].term;

        for (i = 0; i < tr->beats; i++)
            for (c = 0; c < (pl->mono ? 1 : 2); c++) {
                int32_t w = tr->snap [i].weight [pi] [c];

                if (term < 0)
                    CHECK (w >= -1024 && w <= 1024,
                           "%s term %d weight escaped clip at beat %d: %d",
                           pl->label, term, i, w);
            }
    }
}

/*
 * With all-zero entropy residuals, the pass that directly consumes the
 * entropy output (execution pass 0) must freeze every weight, because the
 * update rule keys on the residual entering that pass.  Later passes see
 * partially reconstructed values and therefore may legitimately move.
 */
static void check_frozen_weights (const DtBlockPlan *pl, const DtTrace *tr)
{
    int i, c;

    for (c = 0; c < (pl->mono ? 1 : 2); c++)
        for (i = 1; i < tr->beats; i++)
            CHECK (tr->snap [i].weight [0] [c] == tr->snap [0].weight [0] [c],
                   "%s first-pass weight moved with zero residual chan %d beat %d",
                   pl->label, c, i);
}

/* single-pass zero-residual plans: every weight must remain frozen end to end */
static void check_zero_residual_freeze (void)
{
    int t2[1][2] = { { 2, 2 } };
    int t17[1][2] = { { 17, 3 } };
    int32_t w[1][2] = { { 400, -300 } };
    int32_t h2[1][2][DT_HIST_CAP] = { { { 900, -400, 0, 0, 0, 0, 0, 0 },
                                        { 700, 300, 0, 0, 0, 0, 0, 0 } } };
    int32_t h17[1][2][DT_HIST_CAP] = { { { 1200, -800, 0, 0, 0, 0, 0, 0 },
                                         { -950, 600, 0, 0, 0, 0, 0, 0 } } };
    DtBlockPlan p2 = make_plan ("zero-delay", 0, 24, 12, t2, 1, w, h2);
    DtBlockPlan p17 = make_plan ("zero-extrap", 0, 24, 12, t17, 1, w, h17);
    DtTrace r2, r17;
    int pi, c, i;

    dt_run_plan (&p2, &r2);
    dt_run_plan (&p17, &r17);
    check_frozen_weights (&p2, &r2);
    check_frozen_weights (&p17, &r17);

    for (pi = 0; pi < p2.nterms; pi++)
        for (c = 0; c < 2; c++)
            for (i = 1; i < r2.beats; i++)
                CHECK (r2.snap [i].weight [pi] [c] == r2.snap [0].weight [pi] [c],
                       "zero-delay weight moved pass %d chan %d", pi, c);
}

/*
 * The update must key off source/residual signs, not the reconstructed
 * sample.  Crafted case: positive source, negative residual but a positive
 * reconstruction must still lower the weight.  Weight 8 and history 1024 are
 * exact fixed points of the metadata quantization, so model and block state
 * coincide.
 */
static void check_residual_not_reconstruction (void)
{
    int terms[1][2] = { { 1, 2 } };
    int32_t w[1][2] = { { 8, 0 } };
    int32_t hist[1][2][DT_HIST_CAP] = { { { 1024, 0 }, { 0, 0 } } };
    DtBlockPlan pl = make_plan ("sign-vs-recon", 1, 16, 4, terms, 1, w, hist);
    DtTrace tr;
    int32_t src0 = dt_exp2s (dt_log2s (1024));
    int32_t w0 = dt_restore_weight (dt_store_weight (8));
    int32_t pred0 = dt_apply (w0, src0);

    CHECK (w0 == 8 && src0 == 1024, "quantization fixed points");

    set_resid (&pl, 0, 0, -1);     /* recon = pred0 - 1, still positive */
    set_resid (&pl, 1, 0, 0);
    set_resid (&pl, 2, 0, 0);
    set_resid (&pl, 3, 0, 0);
    dt_run_plan (&pl, &tr);

    CHECK (tr.pcm [0] [0] == pred0 - 1 && tr.pcm [0] [0] > 0,
           "crafted reconstruction stays positive");
    CHECK (tr.snap [0].weight [0] [0] == 6,
           "weight must follow residual sign (8-2=6), got %d",
           tr.snap [0].weight [0] [0]);
    check_trace_invariants (&pl, &tr);
}

/* verify the -1/-2/-3 cross-channel dependencies on single-pass plans;
 * initial weight 8 and power-of-two histories are exact metadata fixed points */
static void check_negative_dependencies (void)
{
    int t1[1][2] = { { -1, 2 } }, t2[1][2] = { { -2, 2 } }, t3[1][2] = { { -3, 2 } };
    int32_t w[1][2] = { { 8, -8 } };
    int32_t h1[1][2][DT_HIST_CAP] = { { { 128, 0 }, { 0, 0 } } };
    int32_t h2[1][2][DT_HIST_CAP] = { { { 0, 0 }, { 256, 0 } } };
    int32_t h3[1][2][DT_HIST_CAP] = { { { 128, 0 }, { 256, 0 } } };
    DtBlockPlan p1 = make_plan ("neg1", 0, 16, 8, t1, 1, w, h1);
    DtBlockPlan p2 = make_plan ("neg2", 0, 16, 8, t2, 1, w, h2);
    DtBlockPlan p3 = make_plan ("neg3", 0, 16, 8, t3, 1, w, h3);
    DtTrace r1, r2, r3;
    int i;

    fill_asym (&p1, 1);
    fill_asym (&p2, 2);
    fill_asym (&p3, 3);
    dt_run_plan (&p1, &r1);
    dt_run_plan (&p2, &r2);
    dt_run_plan (&p3, &r3);

    /* term -1: right's source at beat i is the left reconstructed at the same beat */
    for (i = 1; i < r1.beats; i++) {
        int64_t expect_r = (int64_t) r1.resid [i] [1] +
                           dt_apply (-8, r1.pcm [i] [0]);
        CHECK (r1.pcm [i] [1] == (int32_t) expect_r, "-1 same-beat left->right at %d", i);
    }

    /* term -2: left's source at beat i is the right reconstructed at the same beat */
    for (i = 1; i < r2.beats; i++) {
        int64_t expect_l = (int64_t) r2.resid [i] [0] +
                           dt_apply (8, r2.pcm [i] [1]);
        CHECK (r2.pcm [i] [0] == (int32_t) expect_l, "-2 same-beat right->left at %d", i);
    }

    /* term -3: each side predicts from the opposite side of the PREVIOUS beat */
    for (i = 1; i < r3.beats; i++) {
        int64_t el = (int64_t) r3.resid [i] [0] + dt_apply (8, r3.pcm [i - 1] [1]);
        int64_t er = (int64_t) r3.resid [i] [1] + dt_apply (-8, r3.pcm [i - 1] [0]);
        CHECK (r3.pcm [i] [0] == (int32_t) el, "-3 previous-beat right->left at %d", i);
        CHECK (r3.pcm [i] [1] == (int32_t) er, "-3 previous-beat left->right at %d", i);
    }

    /* first beat of -3 uses the stored previous-beat opposites from metadata */
    CHECK (r3.pcm [0] [0] == r3.resid [0] [0] + dt_apply (8, 128), "-3 initial left source");
    CHECK (r3.pcm [0] [1] == r3.resid [0] [1] + dt_apply (-8, 256), "-3 initial right source");

    check_trace_invariants (&p1, &r1);
    check_trace_invariants (&p2, &r2);
    check_trace_invariants (&p3, &r3);
}

/* negative-term clip: model-level check driven directly, starting on both limits */
static void check_clip_ends (void)
{
    DtModel m;
    int i, hit_hi = 0, hit_lo = 0;

    memset (&m, 0, sizeof (m));
    m.npass = 1;
    m.pass [0].term = -1;
    m.pass [0].delta = 7;
    m.pass [0].w [0] = 1024;
    m.pass [0].w [1] = -1024;
    m.pass [0].hist [0] [0] = 1000;
    m.pass [0].hlen [0] = 1;

    for (i = 0; i < 8; i++) {
        int32_t r[2] = { 1, -1 }, x[2];
        dt_model_beat (&m, i, r, x);

        CHECK (m.pass [0].w [0] <= 1024 && m.pass [0].w [0] >= -1024,
               "clip A out of range at %d (%d)", i, m.pass [0].w [0]);
        CHECK (m.pass [0].w [1] <= 1024 && m.pass [0].w [1] >= -1024,
               "clip B out of range at %d (%d)", i, m.pass [0].w [1]);

        if (m.pass [0].w [0] == 1024) hit_hi = 1;
        if (m.pass [0].w [1] == -1024) hit_lo = 1;
    }

    CHECK (hit_hi, "never held the +1024 clip under upward pressure");
    CHECK (hit_lo, "never held the -1024 clip under downward pressure");
}

/* positive terms must be allowed to travel beyond +/-1024 (model level) */
static void check_positive_no_clip (void)
{
    DtModel m;
    int i, past = 0;

    memset (&m, 0, sizeof (m));
    m.npass = 1;
    m.pass [0].term = 1;
    m.pass [0].delta = 4;
    m.pass [0].w [0] = 1020;
    m.pass [0].hist [0] [0] = 500;
    m.pass [0].hlen [0] = 1;

    for (i = 0; i < 12; i++) {
        int32_t r[2] = { 2, 0 }, x[2];
        dt_model_beat (&m, i, r, x);
        if (m.pass [0].w [0] > 1024)
            past = 1;
    }

    CHECK (past, "positive-term weight was wrongly clipped at +1024");
}

/* ---- whole-block public chain certification for one plan ---- */

static void run_block_case (DtBlockPlan *pl)
{
    static unsigned char block[1 << 18];
    static int32_t pcm[DT_MAX_BEATS * 2];
    DtTrace tr;
    uint32_t len;
    WavpackContext *wpc;
    char error[80] = "";
    int i, nch = pl->mono ? 1 : 2;

    dt_run_plan (pl, &tr);
    check_trace_invariants (pl, &tr);
    len = dt_build_block (pl, &tr, block, sizeof (block));
    CHECK (len > 0, "%s block build failed", pl->label);

    dt_obs_reset ();
    wpc = WavpackOpenFileInputEx64 (&dt_mem_reader,
        &(DtMemFile){ block, (int64_t) len, 0 }, NULL, error, 0, 0);
    CHECK (wpc != NULL, "%s open failed: %s", pl->label, error);

    if (wpc) {
        uint32_t n = WavpackUnpackSamples (wpc, pcm, (uint32_t) pl->beats);
        CHECK (n == (uint32_t) pl->beats, "%s decoded %u of %d", pl->label, n, pl->beats);
        CHECK (!WavpackGetNumErrors (wpc), "%s decoder errors", pl->label);

        for (i = 0; i < pl->beats * nch; i++) {
            int beat = i / nch, chan = i % nch;
            counts.pcm_compare++;

            if (pcm [i] != tr.pcm [beat] [chan]) {
                CHECK (0, "%s public-chain pcm diff beat=%d chan=%d got=%d want=%d",
                       pl->label, beat, chan, pcm [i], tr.pcm [beat] [chan]);
            }
        }

        if (dt_compare_observed (&counts, pl, &tr, 0, 0, pl->beats))
            CHECK (0, "%s observation mismatch", pl->label);

        if (dt_obs_dropped ())
            CHECK (0, "%s dropped %ld observations", pl->label, dt_obs_dropped ());

        WavpackCloseFile (wpc);
    }
}

int main (void)
{
    int term;
    static const int delays[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unsigned seed = 1;

    dt_counts_init (&counts);

    check_apply_floor ();
    check_update_signs ();
    check_residual_not_reconstruction ();
    check_negative_dependencies ();
    check_clip_ends ();
    check_positive_no_clip ();

    /* mono: every positive delay plus both extrapolations */
    for (unsigned k = 0; k < sizeof (delays) / sizeof (delays [0]); k++) {
        int t[1][2] = { { delays [k], 2 } };
        int32_t w[1][2] = { { 300 + (int) k * 37, 0 } };
        int32_t h[1][2][DT_HIST_CAP];
        char label[32];
        DtBlockPlan pl;

        memset (h, 0, sizeof (h));
        h [0] [0] [0] = 400 + (int) k * 55;
        if (delays [k] > 1)
            h [0] [0] [delays [k] - 1] = -120 - (int) k * 31;

        snprintf (label, sizeof (label), "mono-term%d", delays [k]);
        pl = make_plan (label, 1, 16, 20, t, 1, w, h);
        fill_asym (&pl, seed++);
        run_block_case (&pl);
    }

    for (term = 17; term <= 18; term++) {
        int t[1][2] = { { term, 2 } };
        int32_t w[1][2] = { { 400, 0 } };
        int32_t h[1][2][DT_HIST_CAP] = { { { 1500, -900 }, { 0, 0 } } };
        char label[32];
        DtBlockPlan pl;

        snprintf (label, sizeof (label), "mono-term%d", term);
        pl = make_plan (label, 1, 24, 20, t, 1, w, h);
        fill_asym (&pl, seed++);
        run_block_case (&pl);
    }

    /* stereo: every positive delay */
    for (unsigned k = 0; k < sizeof (delays) / sizeof (delays [0]); k++) {
        int t[1][2] = { { delays [k], 1 + (k % 3) } };
        int32_t w[1][2] = { { 260 + (int) k * 29, -310 - (int) k * 23 } };
        int32_t h[1][2][DT_HIST_CAP];
        char label[32];
        DtBlockPlan pl;

        memset (h, 0, sizeof (h));
        h [0] [0] [0] = 500 + (int) k * 41;
        h [0] [1] [0] = -600 - (int) k * 27;

        snprintf (label, sizeof (label), "stereo-term%d", delays [k]);
        pl = make_plan (label, 0, 16, 20, t, 1, w, h);
        fill_asym (&pl, seed++);
        run_block_case (&pl);
    }

    for (term = 17; term <= 18; term++) {
        int t[1][2] = { { term, 2 } };
        int32_t w[1][2] = { { 350, -450 } };
        int32_t h[1][2][DT_HIST_CAP] = { { { 2000, -1100 }, { -1700, 1300 } } };
        char label[32];
        DtBlockPlan pl;

        snprintf (label, sizeof (label), "stereo-term%d", term);
        pl = make_plan (label, 0, 24, 20, t, 1, w, h);
        fill_asym (&pl, seed++);
        run_block_case (&pl);
    }

    /* each negative term as its own plan */
    for (term = -3; term <= -1; term++) {
        int t[1][2] = { { term, 2 } };
        int32_t w[1][2] = { { 500, -500 } };
        int32_t h[1][2][DT_HIST_CAP] = { { { 800, 0 }, { -750, 0 } } };
        char label[32];
        DtBlockPlan pl;

        snprintf (label, sizeof (label), "stereo-term%d", term);
        pl = make_plan (label, 0, 16, 20, t, 1, w, h);
        fill_asym (&pl, seed++);
        run_block_case (&pl);
    }

    /* >16-bit prediction sources, 24-bit */
    {
        int t[1][2] = { { 1, 2 } };
        int32_t w[1][2] = { { 700, -600 } };
        int32_t h[1][2][DT_HIST_CAP] = { { { 40000, 0 }, { -55000, 0 } } };
        DtBlockPlan pl = make_plan ("stereo-bigsrc", 0, 24, 16, t, 1, w, h);
        fill_asym (&pl, seed++);
        run_block_case (&pl);
    }

    /* a fully non-zero multi-pass block through the public chain; this is
     * the case that distinguishes declared term order from execution order */
    {
        int t[2][2] = { { 2, 2 }, { 17, 1 } };
        int32_t w[2][2] = { { 400, -300 }, { 250, 350 } };
        int32_t h[2][2][DT_HIST_CAP] = {
            { { 900, -400, 0, 0, 0, 0, 0, 0 }, { 700, 300, 0, 0, 0, 0, 0, 0 } },
            { { 1200, -800, 0, 0, 0, 0, 0, 0 }, { -950, 600, 0, 0, 0, 0, 0, 0 } }
        };
        DtBlockPlan pl = make_plan ("multi-pass", 0, 24, 12, t, 2, w, h);
        DtTrace tr;

        fill_asym (&pl, 11);
        dt_run_plan (&pl, &tr);
        run_block_case (&pl);
    }

    check_zero_residual_freeze ();

    printf ("REFERENCE term comparison counts:\n");
    for (term = -3; term <= 18; term++) {
        if (term == 0)
            continue;
        if (counts.term_compare [term + 3])
            printf ("  term %3d: %ld state comparisons\n", term, counts.term_compare [term + 3]);
    }
    printf ("  pcm=%ld weights=%ld history=%ld residuals=%ld\n",
            counts.pcm_compare, counts.weight_compare,
            counts.history_compare, counts.residual_compare);

    if (failures || counts.failed) {
        printf ("DECORRELATION REFERENCE: FAIL (%d)\n", failures);
        return 1;
    }

    printf ("DECORRELATION REFERENCE: PASS\n");
    return 0;
}
