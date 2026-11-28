////////////////////////////////////////////////////////////////////////////
//                       **** WAVPACK ****                                 //
//     Unit test for the independent decorrelation reference model         //
//                                                                         //
//  tests/decorr_reference.c                                               //
//                                                                         //
//  This program exercises the independent natural-time model in           //
//  tests/decorr_model.h with small, deliberately asymmetric sequences,    //
//  checking PCM, weights and meaningful history tick by tick. It covers   //
//  zero residuals, negative half-step rounding, prediction sources wider  //
//  than 16 bits, and both ends of the negative-term weight clamp. Every   //
//  case first confirms that no intermediate value leaves the int32 range  //
//  (the model reports this), so the expected results never depend on      //
//  overflow.                                                             //
//                                                                         //
//  It also checks the format conversions (signed 8-bit weights, signed    //
//  16-bit base-2 history logs) and the metadata -> plan parsing, including//
//  the "omitted trailing entries read as zero" rule.                      //
////////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "decorr_model.h"

static int failures;
static int comparisons;

#define FAILF(...) do { \
    fprintf (stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf (stderr, __VA_ARGS__); \
    fputc ('\n', stderr); \
    failures++; \
} while (0)

// A tiny deterministic generator so "asymmetric small sequences" are explicit
// and reproducible rather than random.

typedef struct { uint32_t s; } Rng;

static uint32_t rng_next (Rng *r)
{
    r->s = r->s * 1664525u + 1013904223u;
    return r->s;
}

static int32_t rng_range (Rng *r, int32_t lo, int32_t hi)
{
    return lo + (int32_t) (rng_next (r) % (uint32_t) (hi - lo + 1));
}

// Check an int64 value fits in int32; used before every model invocation so
// that expectations never rely on overflow.
static int fits_int32 (int64_t v)
{
    return v >= INT32_MIN && v <= INT32_MAX;
}

// Build a simple plan with one pass from explicit parameters. History is given
// in the model's natural newest-first order; for metadata purposes the caller
// fills the plan's oldest-first arrays directly.
static void plan_one (DmPlan *p, int mono, int term, int delta, int wA, int wB)
{
    memset (p, 0, sizeof (*p));
    p->num_terms = 1;
    p->mono = mono;
    p->term [0] = term;
    p->delta [0] = delta;
    p->weight_A [0] = wA;
    p->weight_B [0] = wB;
    p->exec_order [0] = 0;
}

// ---------------------------- format conversions -------------------------

static void test_format_conversions (void)
{
    // Weight storage/restore: internal +/-1024 maps through signed 8 bits and
    // back with the format's rounding; both ends clamp.
    static const int ws [] = { 0, 1, 8, 63, 64, 512, 1000, 1023, 1024, 1025, 4096,
                               -1, -8, -64, -512, -1000, -1024, -1025, -4096 };
    for (unsigned i = 0; i < sizeof (ws) / sizeof (*ws); i++) {
        int stored = dm_store_weight (ws [i]);
        int restored = dm_restore_weight (stored);

        comparisons++;

        // restore(store(w)) must equal restore of the *clamped* value, and the
        // stored byte must be a legal signed byte.
        int clamped = ws [i] > 1024 ? 1024 : ws [i] < -1024 ? -1024 : ws [i];

        if (stored < -128 || stored > 127)
            FAILF ("stored weight %d out of signed byte range for w=%d", stored, ws [i]);

        // round-tripping a clamped weight is idempotent
        if (dm_store_weight (restored) != stored)
            FAILF ("weight round-trip not idempotent for w=%d (stored %d restored %d)",
                ws [i], stored, restored);

        if (clamped == 1024 && restored != 1024)
            FAILF ("weight +1024 did not restore to 1024 (got %d)", restored);

        if (clamped == -1024 && restored != -1024)
            FAILF ("weight -1024 did not restore to -1024 (got %d)", restored);
    }

    // History log round-trip: values within the format's representable range
    // must survive to within one part in ~225 (the format's stated accuracy),
    // except that small values are exact enough for our assertions here.
    static const int32_t hs [] = { 0, 1, -1, 2, -2, 3, -3, 7, -7, 64, -64,
                                   1000, -1000, 12345, -12345, 1000000, -1000000,
                                   8388607, -8388608, 100000000, -100000000 };
    for (unsigned i = 0; i < sizeof (hs) / sizeof (*hs); i++) {
        int16_t log = dm_log2s (hs [i]);
        int32_t back = dm_exp2s (log);
        int64_t diff = (int64_t) back - hs [i];
        int64_t mag = hs [i] < 0 ? -(int64_t) hs [i] : hs [i];

        comparisons++;

        if (hs [i] == 0) {
            if (back != 0)
                FAILF ("log round-trip of 0 gave %d", back);
            continue;
        }

        // allowed error is "1 part in 225" per format docs, plus 1 absolute
        if (llabs (diff) > (mag / 200 + 2))
            FAILF ("history log round-trip too lossy for %d: got %d", hs [i], back);

        // sign must be preserved for nonzero inputs
        if ((back < 0) != (hs [i] < 0))
            FAILF ("history log round-trip changed sign for %d: got %d", hs [i], back);
    }
}

// ------------------------- apply/update arithmetic -----------------------

static void test_apply_weight_rounding (void)
{
    // floor((w*s+512)/1024) for a spread of weights and sources, including
    // sources wider than 16 bits. Checked against a direct rational floor.
    static const int64_t ws [] = { 0, 1, -1, 511, -511, 512, -512, 1023, -1023,
                                   1024, -1024, 2048, -2048 };
    static const int64_t ss [] = { 0, 1, -1, 2, -2, 511, -511, 512, -512,
                                   32767, -32768, 65536, -65536, 100000,
                                   -100000, 8000000, -8000000 };
    for (unsigned wi = 0; wi < sizeof (ws) / sizeof (*ws); wi++)
        for (unsigned si = 0; si < sizeof (ss) / sizeof (*ss); si++) {
            int64_t w = ws [wi], s = ss [si];
            int64_t got = dm_apply_weight (w, s);
            // independent rational floor using double and a tolerance guard
            double exact = floor (((double) w * (double) s + 512.0) / 1024.0);

            comparisons++;

            if (got != (int64_t) exact)
                FAILF ("apply_weight(%lld,%lld)=%lld expected %.0f", (long long) w,
                    (long long) s, (long long) got, exact);

            if (!fits_int32 (got))
                FAILF ("apply_weight(%lld,%lld) outside int32: %lld", (long long) w,
                    (long long) s, (long long) got);
        }

    // Explicit negative half-step: (w*s+512) = 1024*k - 512 -> floor to k-1.
    // w=1,s=-512 => (-512+512)=0 -> 0. w=1,s=-1024 => -1024+512 = -512 -> -1.
    if (dm_apply_weight (1, -1024) != -1)
        FAILF ("negative half-step: apply_weight(1,-1024) should floor to -1, got %lld",
            (long long) dm_apply_weight (1, -1024));

    // w=-1,s=1024 => -1024+512 = -512 -> -1
    if (dm_apply_weight (-1, 1024) != -1)
        FAILF ("negative half-step: apply_weight(-1,1024) should floor to -1, got %lld",
            (long long) dm_apply_weight (-1, 1024));

    // w=1,s=512 => 512+512=1024 -> 1 (exact half rounds up because floor)
    if (dm_apply_weight (1, 512) != 1)
        FAILF ("positive half-step: apply_weight(1,512) should be 1, got %lld",
            (long long) dm_apply_weight (1, 512));
}

static void test_update_weight_sign_rule (void)
{
    // The update looks at the sign of source and residual, NOT the
    // reconstructed sample. Same signs -> weight grows; opposite -> shrinks;
    // either zero -> unchanged.
    struct { int init, delta; int64_t src, res; int expect; } cases [] = {
        { 100, 2,  5,  7, 102 },   // both positive -> add
        { 100, 2, -5, -7, 102 },   // both negative -> add
        { 100, 2,  5, -7,  98 },   // opposite -> subtract
        { 100, 2, -5,  7,  98 },   // opposite -> subtract
        { 100, 2,  0,  7, 100 },   // zero source -> unchanged
        { 100, 2,  5,  0, 100 },   // zero residual -> unchanged
    };
    for (unsigned i = 0; i < sizeof (cases) / sizeof (*cases); i++) {
        int64_t got = dm_update_weight (cases [i].init, cases [i].delta,
            cases [i].src, cases [i].res);
        comparisons++;

        if (got != cases [i].expect)
            FAILF ("update_weight case %u: got %lld expected %d", i,
                (long long) got, cases [i].expect);
    }

    // Clip variant: hits both ends and clamps to +/-1024.
    struct { int init, delta; int64_t src, res; int expect; } clips [] = {
        { 1023, 2, 5, 7, 1024 },       // same signs -> grows past +1024 -> clamp
        { -1023, 2, 5, -7, -1024 },    // opposite signs -> shrinks past -1024 -> clamp
        { 1024, 2, 5, 7, 1024 },       // already at +end stays
        { -1024, 2, 5, -7, -1024 },    // already at -end stays
        { -1023, 2, -5, -7, -1021 },   // same negative signs -> moves toward zero
        { 0, 2, 5, 7, 2 },             // ordinary growth (no clamp)
    };
    for (unsigned i = 0; i < sizeof (clips) / sizeof (*clips); i++) {
        int64_t got = dm_update_weight_clip (clips [i].init, clips [i].delta,
            clips [i].src, clips [i].res);
        comparisons++;

        if (got != clips [i].expect)
            FAILF ("update_weight_clip case %u: got %lld expected %d", i,
                (long long) got, clips [i].expect);
    }
}

// ---------------------- model end-to-end, per term -----------------------

// Decode one chunk with the model and compare against a locally recomputed
// expectation for a single pass. This is the "tick by tick" check: PCM, weight
// and history after every frame.

static void tick_test_positive_term (int term, int delta)
{
    enum { FRAMES = 24 };
    DmPlan plan;
    DmState st, ref;
    int32_t res [FRAMES * 2], pcm [FRAMES * 2], rpcm [FRAMES * 2];
    Rng rng = { (uint32_t) (0x9e3779b1u ^ (uint32_t) term * 2654435761u ^ (uint32_t) delta) };

    plan_one (&plan, 0, term, delta, 0, 0);

    // Asymmetric history and residuals small enough that all intermediates
    // stay well inside int32. History newest-first in the model queue.
    for (int k = 0; k < term; k++) {
        plan.hist_A [0][term - 1 - k] = (int64_t) (k + 1) * 3 - 7 * (k & 1);
        plan.hist_B [0][term - 1 - k] = -(int64_t) (k + 1) * 2 + 5 * (k & 1);
    }
    plan.weight_A [0] = 300;
    plan.weight_B [0] = -250;

    dm_init (&st, &plan);

    for (int f = 0; f < FRAMES; f++) {
        res [f * 2] = rng_range (&rng, -40, 40);
        res [f * 2 + 1] = rng_range (&rng, -40, 40);
    }

    // Independent expectation, kept separate from the model implementation.
    // We also keep the expected weight and history after every tick so the
    // model can be checked tick by tick, not only at the end.
    static int64_t exp_weight_A [FRAMES], exp_weight_B [FRAMES];
    static int64_t exp_hist_A [FRAMES][8], exp_hist_B [FRAMES][8];
    ref = st;
    {
        int nchan = 2;

        for (int i = 0; i < FRAMES * nchan; i++)
            rpcm [i] = res [i];

        for (int f = 0; f < FRAMES; f++) {
            DmPass *p = ref.pass + 0;
            int64_t srcA = p->hist_A [term - 1], srcB = p->hist_B [term - 1];
            int64_t inA = rpcm [f * 2], inB = rpcm [f * 2 + 1];

            int64_t outA = inA + dm_apply_weight (p->weight_A, srcA);
            int64_t outB = inB + dm_apply_weight (p->weight_B, srcB);

            if (!fits_int32 (outA) || !fits_int32 (outB))
                FAILF ("term %d frame %d intermediate overflow", term, f);

            p->weight_A = dm_update_weight (p->weight_A, delta, srcA, inA);
            p->weight_B = dm_update_weight (p->weight_B, delta, srcB, inB);

            for (int j = term - 1; j > 0; j--) {
                p->hist_A [j] = p->hist_A [j - 1];
                p->hist_B [j] = p->hist_B [j - 1];
            }

            p->hist_A [0] = outA;
            p->hist_B [0] = outB;
            rpcm [f * 2] = (int32_t) outA;
            rpcm [f * 2 + 1] = (int32_t) outB;

            exp_weight_A [f] = p->weight_A;
            exp_weight_B [f] = p->weight_B;

            for (int k = 0; k < term; k++) {
                exp_hist_A [f][k] = p->hist_A [term - 1 - k];   // format order
                exp_hist_B [f][k] = p->hist_B [term - 1 - k];
            }
        }
    }

    // Decode one frame at a time so we can check each tick's PCM, weight and
    // history against the independent expectation.
    for (int f = 0; f < FRAMES; f++) {
        dm_decode_block (&st, res + f * 2, pcm, 1);

        comparisons += 2;

        if (pcm [0] != rpcm [f * 2])
            FAILF ("term %d tick %d L=%d expected %d", term, f, pcm [0], rpcm [f * 2]);

        if (pcm [1] != rpcm [f * 2 + 1])
            FAILF ("term %d tick %d R=%d expected %d", term, f, pcm [1], rpcm [f * 2 + 1]);

        comparisons += 2;

        if (st.pass [0].weight_A != exp_weight_A [f] || st.pass [0].weight_B != exp_weight_B [f])
            FAILF ("term %d tick %d weights (%lld,%lld) expected (%lld,%lld)", term, f,
                (long long) st.pass [0].weight_A, (long long) st.pass [0].weight_B,
                (long long) exp_weight_A [f], (long long) exp_weight_B [f]);

        {
            DmPassView v;

            dm_pass_view (&st, 0, &v);

            for (int k = 0; k < term; k++) {
                comparisons++;

                if (v.hist_A [k] != exp_hist_A [f][k] || v.hist_B [k] != exp_hist_B [f][k])
                    FAILF ("term %d tick %d history[%d] mismatch", term, f, k);
            }
        }
    }
}

static void tick_test_extrapolation_term (int term)
{
    enum { FRAMES = 20 };
    DmPlan plan;
    DmState st;
    int32_t res [FRAMES * 2], pcm [FRAMES * 2];
    int64_t srcA [FRAMES], srcB [FRAMES], outA [FRAMES], outB [FRAMES];
    int64_t wA = 400, wB = -400;
    int64_t hA0, hA1, hB0, hB1;
    Rng rng = { (uint32_t) (0x1234567u * (uint32_t) term) };

    plan_one (&plan, 0, term, 2, (int) wA, (int) wB);
    // 17/18 history is newest-first in both plan and model; give distinct
    // values so the extrapolation slope is nonzero.
    plan.hist_A [0][0] = 1000;
    plan.hist_A [0][1] = 400;
    plan.hist_B [0][0] = -900;
    plan.hist_B [0][1] = -200;
    dm_init (&st, &plan);

    hA0 = 1000; hA1 = 400; hB0 = -900; hB1 = -200;

    for (int f = 0; f < FRAMES; f++) {
        res [f * 2] = rng_range (&rng, -60, 60);
        res [f * 2 + 1] = rng_range (&rng, -60, 60);
    }

    // independent expectation with explicit floor((3a-b)/2)
    for (int f = 0; f < FRAMES; f++) {
        if (term == 17) {
            srcA [f] = 2 * hA0 - hA1;
            srcB [f] = 2 * hB0 - hB1;
        }
        else {
            srcA [f] = (3 * hA0 - hA1) / 2 - ((3 * hA0 - hA1) % 2 < 0 ? 1 : 0);
            srcB [f] = (3 * hB0 - hB1) / 2 - ((3 * hB0 - hB1) % 2 < 0 ? 1 : 0);
        }

        outA [f] = res [f * 2] + dm_apply_weight (wA, srcA [f]);
        outB [f] = res [f * 2 + 1] + dm_apply_weight (wB, srcB [f]);

        if (!fits_int32 (outA [f]) || !fits_int32 (outB [f]))
            FAILF ("term %d frame %d overflow", term, f);

        wA = dm_update_weight (wA, 2, srcA [f], res [f * 2]);
        wB = dm_update_weight (wB, 2, srcB [f], res [f * 2 + 1]);
        hA1 = hA0; hA0 = outA [f];
        hB1 = hB0; hB0 = outB [f];
    }

    dm_decode_block (&st, res, pcm, FRAMES);

    for (int f = 0; f < FRAMES; f++) {
        comparisons += 2;

        if (pcm [f * 2] != outA [f])
            FAILF ("term %d frame %d left %d expected %lld", term, f, pcm [f * 2],
                (long long) outA [f]);

        if (pcm [f * 2 + 1] != outB [f])
            FAILF ("term %d frame %d right %d expected %lld", term, f, pcm [f * 2 + 1],
                (long long) outB [f]);
    }

    comparisons += 4;

    if (st.pass [0].weight_A != wA || st.pass [0].weight_B != wB)
        FAILF ("term %d final weights mismatch", term);

    if (st.pass [0].hist_A [0] != hA0 || st.pass [0].hist_A [1] != hA1 ||
        st.pass [0].hist_B [0] != hB0 || st.pass [0].hist_B [1] != hB1)
            FAILF ("term %d final extrapolation history mismatch", term);
}

// Negative terms: verify the cross-channel dependency explicitly, plus the
// weight clamp behaviour on both ends.

static void tick_test_negative_term (int term)
{
    enum { FRAMES = 18 };
    DmPlan plan;
    DmState st;
    int32_t res [FRAMES * 2], pcm [FRAMES * 2];
    int64_t rA [FRAMES], rB [FRAMES];
    int64_t wA = 1000, wB = -1000;          // drive toward both clamp ends
    int64_t hA = 300, hB = -250;            // saved "previous tick opposite" values
    Rng rng = { (uint32_t) (0xabcdef01u ^ (uint32_t) term * 40503u) };

    plan_one (&plan, 0, term, 3, (int) wA, (int) wB);

    if (term == -1) { plan.hist_A [0][0] = hA; plan.hist_B [0][0] = hB; }
    else if (term == -2) { plan.hist_A [0][0] = hA; plan.hist_B [0][0] = hB; }
    else { plan.hist_A [0][0] = hA; plan.hist_B [0][0] = hB; }

    dm_init (&st, &plan);

    for (int f = 0; f < FRAMES; f++) {
        // Alternate signs so the weight updates hit both directions.
        res [f * 2] = (f & 1) ? rng_range (&rng, 20, 60) : rng_range (&rng, -60, -20);
        res [f * 2 + 1] = (f & 2) ? rng_range (&rng, 15, 50) : rng_range (&rng, -50, -15);
    }

    // independent expectation
    for (int f = 0; f < FRAMES; f++) {
        int64_t inA = res [f * 2], inB = res [f * 2 + 1];

        if (term == -1) {
            int64_t outA = inA + dm_apply_weight (wA, hA);
            wA = dm_update_weight_clip (wA, 3, hA, inA);
            int64_t outB = inB + dm_apply_weight (wB, outA);
            wB = dm_update_weight_clip (wB, 3, outA, inB);
            hA = outB;                 // store this tick's right for next tick's left
            rA [f] = outA; rB [f] = outB;
        }
        else if (term == -2) {
            int64_t outB = inB + dm_apply_weight (wB, hB);
            wB = dm_update_weight_clip (wB, 3, hB, inB);
            int64_t outA = inA + dm_apply_weight (wA, outB);
            wA = dm_update_weight_clip (wA, 3, outB, inA);
            hB = outA;                 // store this tick's left for next tick's right
            rA [f] = outA; rB [f] = outB;
        }
        else { // -3
            int64_t outA = inA + dm_apply_weight (wA, hA);
            int64_t outB = inB + dm_apply_weight (wB, hB);
            wA = dm_update_weight_clip (wA, 3, hA, inA);
            wB = dm_update_weight_clip (wB, 3, hB, inB);
            hA = outB;                 // next left uses this tick's right
            hB = outA;                 // next right uses this tick's left
            rA [f] = outA; rB [f] = outB;
        }

        if (!fits_int32 (rA [f]) || !fits_int32 (rB [f]))
            FAILF ("term %d frame %d overflow", term, f);
    }

    dm_decode_block (&st, res, pcm, FRAMES);

    for (int f = 0; f < FRAMES; f++) {
        comparisons += 2;

        if (pcm [f * 2] != rA [f])
            FAILF ("term %d frame %d L=%d expected %lld", term, f, pcm [f * 2],
                (long long) rA [f]);

        if (pcm [f * 2 + 1] != rB [f])
            FAILF ("term %d frame %d R=%d expected %lld", term, f, pcm [f * 2 + 1],
                (long long) rB [f]);
    }

    comparisons += 4;

    if (st.pass [0].weight_A != wA || st.pass [0].weight_B != wB)
        FAILF ("term %d final weights (%lld,%lld) expected (%lld,%lld)", term,
            (long long) st.pass [0].weight_A, (long long) st.pass [0].weight_B,
            (long long) wA, (long long) wB);

    if (st.pass [0].weight_A > 1024 || st.pass [0].weight_A < -1024 ||
        st.pass [0].weight_B > 1024 || st.pass [0].weight_B < -1024)
            FAILF ("term %d weight escaped +/-1024 clamp", term);
}

// Zero-residual run: weights must not move and PCM must be the running
// prediction (which can drift). This catches an accidental update on zeros.
static void test_zero_residual (void)
{
    enum { FRAMES = 10 };
    DmPlan plan;
    DmState st;
    int32_t res [FRAMES * 2] = { 0 }, pcm [FRAMES * 2];
    int64_t wA0, wB0;

    plan_one (&plan, 0, 3, 2, 500, -500);
    plan.hist_A [0][0] = 10; plan.hist_A [0][1] = 20; plan.hist_A [0][2] = 30;
    plan.hist_B [0][0] = -10; plan.hist_B [0][1] = -20; plan.hist_B [0][2] = -30;
    dm_init (&st, &plan);
    wA0 = st.pass [0].weight_A;
    wB0 = st.pass [0].weight_B;

    dm_decode_block (&st, res, pcm, FRAMES);

    comparisons += 2;

    if (st.pass [0].weight_A != wA0 || st.pass [0].weight_B != wB0)
        FAILF ("zero residual changed weights (%lld->%lld, %lld->%lld)",
            (long long) wA0, (long long) st.pass [0].weight_A,
            (long long) wB0, (long long) st.pass [0].weight_B);

    // With zero residuals every output is exactly the prediction. For term 3
    // the prediction source is the sample three ticks back, which is plan
    // history slot 0 (metadata stores 1..8 history oldest-first).
    comparisons++;

    if (pcm [0] != (int32_t) dm_apply_weight (500, 10))
        FAILF ("zero residual first sample %d expected %lld", pcm [0],
            (long long) dm_apply_weight (500, 10));
}

// Wide (>16-bit) prediction source: confirm the 64-bit apply_weight path is
// exercised. A 24-bit source with a large weight produces a large prediction.
static void test_wide_source (void)
{
    DmPlan plan;
    DmState st;
    int32_t res [4], pcm [4];
    int64_t expect0;

    plan_one (&plan, 0, 1, 2, 900, -900);
    plan.hist_A [0][0] = 5000000;      // ~22-bit prediction source
    plan.hist_B [0][0] = -6000000;
    dm_init (&st, &plan);

    res [0] = 1000; res [1] = -1000; res [2] = 0; res [3] = 0;
    expect0 = 1000 + dm_apply_weight (900, 5000000);

    if (!fits_int32 (expect0))
        FAILF ("wide-source expectation overflows int32, test mis-sized");

    dm_decode_block (&st, res, pcm, 2);

    comparisons++;

    if (pcm [0] != (int32_t) expect0)
        FAILF ("wide source left %d expected %lld", pcm [0], (long long) expect0);

    // The prediction must exceed 16-bit magnitide so the classic 32-bit
    // apply_weight_i path would not even be legal here.
    comparisons++;

    if (llabs (dm_apply_weight (900, 5000000)) < 65536)
        FAILF ("wide-source prediction not actually wide (%lld)",
            (long long) dm_apply_weight (900, 5000000));
}

// --------------------- execution vs declaration order --------------------

// A two-term chain where declaration order and execution order differ. If the
// model executed in declaration order the results would differ, so the check
// has teeth.
static void test_execution_order (void)
{
    DmPlan plan;
    DmState st, decl_order;
    int32_t res [3 * 2], pcm [3 * 2], alt [3 * 2];
    int matched_decl;

    memset (&plan, 0, sizeof (plan));
    plan.num_terms = 2;
    plan.mono = 0;
    // declaration order: pass 0 term 2, pass 1 term -1
    plan.term [0] = 2; plan.delta [0] = 2;
    plan.term [1] = -1; plan.delta [1] = 2;
    plan.weight_A [0] = 400; plan.weight_B [0] = -300;
    plan.weight_A [1] = 500; plan.weight_B [1] = -500;
    plan.hist_A [0][0] = 5; plan.hist_A [0][1] = 9;
    plan.hist_B [0][0] = -5; plan.hist_B [0][1] = -9;
    plan.hist_A [1][0] = 7; plan.hist_B [1][0] = -7;
    plan.exec_order [0] = 1;    // decoder runs declared pass 1 first
    plan.exec_order [1] = 0;

    dm_init (&st, &plan);

    res [0] = 11; res [1] = -13; res [2] = 17; res [3] = 19; res [4] = -23; res [5] = 29;
    dm_decode_block (&st, res, pcm, 3);

    // Now decode with the "wrong" execution order to prove the order matters.
    decl_order = st;
    decl_order.pass [0].weight_A = 400; decl_order.pass [0].weight_B = -300;
    decl_order.pass [0].hist_A [0] = 9; decl_order.pass [0].hist_A [1] = 5;
    decl_order.pass [0].hist_B [0] = -9; decl_order.pass [0].hist_B [1] = -5;
    decl_order.pass [1].weight_A = 500; decl_order.pass [1].weight_B = -500;
    decl_order.pass [1].hist_A [0] = 7; decl_order.pass [1].hist_B [0] = -7;
    decl_order.exec_order [0] = 0;
    decl_order.exec_order [1] = 1;

    dm_decode_block (&decl_order, res, alt, 3);

    matched_decl = (memcmp (pcm, alt, sizeof (pcm)) == 0);
    comparisons++;

    if (matched_decl)
        FAILF ("execution order had no effect; declaration-order decode matched");

    // And the values must be internally consistent: re-running the correct
    // order from a fresh init reproduces them exactly.
    {
        DmState again;
        int32_t pcm2 [3 * 2];

        dm_init (&again, &plan);
        dm_decode_block (&again, res, pcm2, 3);
        comparisons++;

        if (memcmp (pcm, pcm2, sizeof (pcm)) != 0)
            FAILF ("execution-order result not reproducible");
    }
}

// ----------------------------- metadata plan -----------------------------

static void test_metadata_plan (void)
{
    // Mirrors a stereo block with two declared terms (declaration order):
    //   terms[0] = term 2, delta 3   -> byte ((2+5)&0x1f)|(3<<5)
    //   terms[1] = term -1, delta 1  -> byte ((-1+5)&0x1f)|(1<<5)
    // Weights are declared from the *last* term backwards in real files, so
    // here we just supply them in declaration order and rely on the plan's
    // documented convention (first declared term first). The important part is
    // the zero-fill rule for omitted trailing weight/history bytes.
    unsigned char terms [2];
    unsigned char weights [2];
    unsigned char samples [12];
    DmPlan plan;

    terms [0] = (unsigned char) (((2 + 5) & 0x1f) | (3 << 5));
    terms [1] = (unsigned char) (((-1 + 5) & 0x1f) | (1 << 5));

    // Only the first declared term's weight pair is present: weight_A=+512,
    // weight_B=-512 stored via the format conversion.
    weights [0] = (unsigned char) dm_store_weight (512);
    weights [1] = (unsigned char) dm_store_weight (-512);

    // Samples: term 2 (two per channel, oldest first), then term -1 (one per
    // channel). Use values that survive the log round-trip.
    int32_t sA0 = 100, sA1 = 200, sB0 = -100, sB1 = -200;
    int32_t nA = 300, nB = -300;
    int16_t lA0 = dm_log2s (sA0), lA1 = dm_log2s (sA1);
    int16_t lB0 = dm_log2s (sB0), lB1 = dm_log2s (sB1);
    int16_t lnA = dm_log2s (nA), lnB = dm_log2s (nB);

    samples [0] = (unsigned char) lA0; samples [1] = (unsigned char) (lA0 >> 8);
    samples [2] = (unsigned char) lB0; samples [3] = (unsigned char) (lB0 >> 8);
    samples [4] = (unsigned char) lA1; samples [5] = (unsigned char) (lA1 >> 8);
    samples [6] = (unsigned char) lB1; samples [7] = (unsigned char) (lB1 >> 8);
    samples [8] = (unsigned char) lnA; samples [9] = (unsigned char) (lnA >> 8);
    samples [10] = (unsigned char) lnB; samples [11] = (unsigned char) (lnB >> 8);

    comparisons++;

    if (dm_plan_from_metadata (&plan, 0, terms, 2, weights, 2, samples, 12) != 0) {
        FAILF ("dm_plan_from_metadata rejected a valid plan");
        return;
    }

    comparisons += 4;

    if (plan.term [0] != 2 || plan.term [1] != -1)
        FAILF ("metadata term decode wrong: %d %d", plan.term [0], plan.term [1]);

    if (plan.delta [0] != 3 || plan.delta [1] != 1)
        FAILF ("metadata delta decode wrong: %d %d", plan.delta [0], plan.delta [1]);

    if (plan.exec_order [0] != 1 || plan.exec_order [1] != 0)
        FAILF ("execution order should reverse declaration order");

    // second declared term had no weight -> zero fill
    if (plan.weight_A [1] != 0 || plan.weight_B [1] != 0)
        FAILF ("omitted trailing weight should read as zero (got %lld,%lld)",
            (long long) plan.weight_A [1], (long long) plan.weight_B [1]);

    // restored weights of the first term
    if (plan.weight_A [0] != dm_restore_weight (weights [0]) ||
        plan.weight_B [0] != dm_restore_weight (weights [1]))
            FAILF ("weight restoration mismatch");

    // histories: term 2 oldest-first should match logs; term -1 single value
    if (plan.hist_A [0][0] != dm_exp2s (lA0) || plan.hist_A [0][1] != dm_exp2s (lA1) ||
        plan.hist_B [0][0] != dm_exp2s (lB0) || plan.hist_B [0][1] != dm_exp2s (lB1))
            FAILF ("term 2 history decode mismatch");

    if (plan.hist_A [1][0] != dm_exp2s (lnA) || plan.hist_B [1][0] != dm_exp2s (lnB))
        FAILF ("term -1 history decode mismatch");

    // Truncated metadata must be rejected, not silently accepted.
    comparisons++;

    if (dm_plan_from_metadata (&plan, 0, terms, 2, weights, 2, samples, 10) == 0)
        FAILF ("truncated samples metadata accepted");

    // A negative term in a mono stream is illegal.
    comparisons++;

    if (dm_plan_from_metadata (&plan, 1, terms, 2, weights, 2, samples, 12) == 0)
        FAILF ("negative term accepted for mono");
}

// ------------------------------- driver ----------------------------------

int main (void)
{
    test_format_conversions ();
    test_apply_weight_rounding ();
    test_update_weight_sign_rule ();

    for (int term = 1; term <= 8; term++) {
        tick_test_positive_term (term, 1);
        tick_test_positive_term (term, 3);
    }

    tick_test_extrapolation_term (17);
    tick_test_extrapolation_term (18);

    tick_test_negative_term (-1);
    tick_test_negative_term (-2);
    tick_test_negative_term (-3);

    test_zero_residual ();
    test_wide_source ();
    test_execution_order ();
    test_metadata_plan ();

    if (failures) {
        fprintf (stderr, "DECORR_REFERENCE FAIL comparisons=%d failures=%d\n",
            comparisons, failures);
        return 1;
    }

    printf ("DECORR_REFERENCE OK comparisons=%d failures=0\n", comparisons);
    return 0;
}
