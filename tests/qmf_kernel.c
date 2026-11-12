/*
 * Kernel level bit exactness tests for the AArch64 NEON QMF implementation.
 *
 * The oracle is the independent 64bit scalar dot product plus explicit
 * midpoint rounding and 24bit saturation in qmf_ref.h. Every comparison
 * covers the produced samples *and* the full circular buffer / pos state,
 * across all 16 starting positions, both QMF tree stages and both tree
 * directions. A plain encode->decode round trip would let errors cancel, so
 * it is intentionally not used here.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openaptx_internal.h"
#include "qmf_ref.h"

#if !APTX_NEON_KERNELS
#error "qmf_kernel.c must be built with the NEON kernels enabled (AArch64, no APTX_FORCE_SCALAR)"
#endif

static uint32_t rng_state;

static uint32_t xs32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* Mostly random data, regularly spiced with the exact 24/32bit extremes. */
static int32_t rand_extreme(void)
{
    uint32_t r = xs32() % 16;
    if (r == 0)  return INT32_MAX;
    if (r == 1)  return INT32_MIN;
    if (r == 2)  return (int32_t)((1L << 23) - 1);
    if (r == 3)  return -(int32_t)(1L << 23);
    return (int32_t)xs32();
}

static void fail(const char *case_name, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("case %s: FAIL: ", case_name);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}

#define CHECK(case_name, cond, ...) \
    do { if (!(cond)) fail(case_name, __VA_ARGS__); } while (0)

static void fill_signal_random(struct aptx_filter_signal *s, uint32_t seed,
                               unsigned forced_pos)
{
    unsigned i;
    rng_state = seed;
    for (i = 0; i < 2 * FILTER_TAPS; i++)
        s->buffer[i] = rand_extreme();
    s->pos = (uint8_t)forced_pos;
}

static void fill_tree_random(struct aptx_QMF_analysis *q, uint32_t seed,
                             unsigned forced_pos)
{
    rng_state = seed;
    memset(q, 0, sizeof(*q));
    fill_signal_random(&q->outer_filter_signal[0], xs32(), forced_pos);
    fill_signal_random(&q->outer_filter_signal[1], xs32(), forced_pos);
    fill_signal_random(&q->inner_filter_signal[0][0], xs32(), forced_pos);
    fill_signal_random(&q->inner_filter_signal[0][1], xs32(), forced_pos);
    fill_signal_random(&q->inner_filter_signal[1][0], xs32(), forced_pos);
    fill_signal_random(&q->inner_filter_signal[1][1], xs32(), forced_pos);
}

/*
 * Case 1: raw 16 tap convolution against the independent scalar dot product,
 * every starting pos, both coefficient tables, all three scaling positions.
 * The convolution must not mutate the ring state.
 */
static void case_convolution_dotproduct(void)
{
    const char *name = "conv_dotproduct_allpos";
    const int32_t (*tables[NB_FILTERS])[FILTER_TAPS] = {
        aptx_qmf_outer_coeffs, aptx_qmf_inner_coeffs
    };
    const unsigned shifts[3] = { 23, 22, 21 };
    unsigned t, branch, sh, p, k;

    for (t = 0; t < NB_FILTERS; t++) {
        for (branch = 0; branch < NB_FILTERS; branch++) {
            for (sh = 0; sh < 3; sh++) {
                for (p = 0; p < FILTER_TAPS; p++) {
                    struct aptx_filter_signal sig, snapshot;
                    int32_t got, want;
                    fill_signal_random(&sig, 0x1000 + t * 256 + branch * 64 + p, p);
                    for (k = 0; k < 8; k++) {
                        snapshot = sig;
                        got = aptx_qmf_convolution_neon(&sig, tables[t][branch], shifts[sh]);
                        want = ref_convolution(&sig, tables[t][branch], shifts[sh]);
                        CHECK(name, got == want,
                              "table=%u branch=%u shift=%u pos=%u iter=%u got=%d want=%d",
                              t, branch, shifts[sh], p, k, got, want);
                        /* convolution is read-only: state must be untouched */
                        CHECK(name, memcmp(&sig, &snapshot, sizeof(sig)) == 0,
                              "convolution mutated ring state");
                        sig.buffer[(p + k) & (FILTER_TAPS - 1)] = rand_extreme();
                    }
                }
            }
        }
    }
    printf("case %s: PASS\n", name);
}

/*
 * Case 2: rounding midpoint convention from both sides and 24bit saturation.
 * A custom one-tap coefficient vector makes the 64bit dot product an exact
 * chosen value, so the midpoint rule and clipping can be targeted directly
 * instead of hoped for from random data.
 */
static void case_rounding_midpoint(void)
{
    const char *name = "rounding_midpoint_and_clip";
    const unsigned shifts[3] = { 23, 22, 21 };
    int32_t coeffs[FILTER_TAPS];
    struct aptx_filter_signal sig;
    unsigned sh, p, k;
    int64_t step;

    memset(coeffs, 0, sizeof(coeffs));
    coeffs[0] = 1;
    memset(&sig, 0, sizeof(sig));

    for (sh = 0; sh < 3; sh++) {
        const unsigned s = shifts[sh];
        const int64_t half = (int64_t)1 << (s - 1);

        /* Explicit tie behaviour: exact midpoint rounds towards -infinity. */
        for (p = 0; p < FILTER_TAPS; p++) {
            static const int64_t offsets[3] = { -1, 0, 1 };
            sig.pos = (uint8_t)p;
            for (k = 0; k < 3; k++) {
                int64_t e = half + offsets[k];
                int32_t got, want;
                sig.buffer[p] = (int32_t)e;
                got = aptx_qmf_convolution_neon(&sig, coeffs, s);
                want = ref_rshift64_clip24(e, s);
                CHECK(name, got == want, "positive midpoint s=%u p=%u e=%lld got=%d want=%d",
                      s, p, (long long)e, got, want);
            }
            /* negative midpoints behave the same way */
            for (k = 0; k < 3; k++) {
                int64_t e = -half + offsets[k];
                int32_t got, want;
                sig.buffer[p] = (int32_t)e;
                got = aptx_qmf_convolution_neon(&sig, coeffs, s);
                want = ref_rshift64_clip24(e, s);
                CHECK(name, got == want, "negative midpoint s=%u p=%u e=%lld got=%d want=%d",
                      s, p, (long long)e, got, want);
            }
            sig.buffer[p] = 0;
        }

        /* Sweep both sides of several multiples, positive and negative. */
        for (step = -4; step <= 4; step++) {
            int64_t base = step * ((int64_t)1 << s);
            int64_t e;
            for (e = base - half - 2; e <= base + half + 2; e++) {
                int32_t got, want;
                sig.pos = 0;
                sig.buffer[0] = (int32_t)e;
                got = aptx_qmf_convolution_neon(&sig, coeffs, s);
                want = ref_rshift64_clip24(e, s);
                CHECK(name, got == want, "sweep s=%u e=%lld got=%d want=%d",
                      s, (long long)e, got, want);
            }
        }
    }

    /* Saturation: scale the lone coefficient so products dwarf 24 bits. */
    coeffs[0] = (int32_t)(1L << 20);
    sig.pos = 0;
    sig.buffer[0] = INT32_MAX;
    CHECK(name, aptx_qmf_convolution_neon(&sig, coeffs, 21) == (int32_t)((1L << 23) - 1),
          "positive 24bit saturation missing");
    sig.buffer[0] = INT32_MIN;
    CHECK(name, aptx_qmf_convolution_neon(&sig, coeffs, 21) == -(int32_t)(1L << 23),
          "negative 24bit saturation missing");
    sig.buffer[0] = INT32_MAX;
    CHECK(name, aptx_qmf_convolution_neon(&sig, coeffs, 23) == (int32_t)((1L << 23) - 1),
          "saturation at analysis shift missing");

    printf("case %s: PASS\n", name);
}

static void case_analysis_tree_allpos(void)
{
    const char *name = "analysis_tree_allpos";
    unsigned p, iter;

    for (p = 0; p < FILTER_TAPS; p++) {
        struct aptx_QMF_analysis ref_q, neon_q;
        int32_t in[4], ref_out[NB_SUBBANDS], neon_out[NB_SUBBANDS];

        fill_tree_random(&ref_q, 0xA000 + p * 101, p);
        neon_q = ref_q;

        for (iter = 0; iter < 40; iter++) {
            unsigned k;
            rng_state = 0x5000 + p * 977 + iter;
            for (k = 0; k < 4; k++)
                in[k] = rand_extreme();

            ref_tree_analysis(&ref_q, in, ref_out);
            aptx_qmf_tree_analysis_neon(&neon_q, in, neon_out);

            CHECK(name, memcmp(neon_out, ref_out, sizeof(ref_out)) == 0,
                  "pos=%u iter=%u subband mismatch", p, iter);
            CHECK(name, memcmp(&neon_q, &ref_q, sizeof(ref_q)) == 0,
                  "pos=%u iter=%u ring state mismatch", p, iter);
        }
    }
    printf("case %s: PASS\n", name);
}

static void case_synthesis_tree_allpos(void)
{
    const char *name = "synthesis_tree_allpos";
    unsigned p, iter;

    for (p = 0; p < FILTER_TAPS; p++) {
        struct aptx_QMF_analysis ref_q, neon_q;
        int32_t in[NB_SUBBANDS], ref_out[4], neon_out[4];

        fill_tree_random(&ref_q, 0xB000 + p * 131, p);
        neon_q = ref_q;

        for (iter = 0; iter < 40; iter++) {
            unsigned k;
            rng_state = 0x6000 + p * 919 + iter;
            for (k = 0; k < NB_SUBBANDS; k++)
                in[k] = rand_extreme();

            ref_tree_synthesis(&ref_q, in, ref_out);
            aptx_qmf_tree_synthesis_neon(&neon_q, in, neon_out);

            CHECK(name, memcmp(neon_out, ref_out, sizeof(ref_out)) == 0,
                  "pos=%u iter=%u output mismatch (22/21 scaling)", p, iter);
            CHECK(name, memcmp(&neon_q, &ref_q, sizeof(ref_q)) == 0,
                  "pos=%u iter=%u ring state mismatch", p, iter);
        }
    }
    printf("case %s: PASS\n", name);
}

/*
 * Case 5: stereo entry points with deliberately different, opposite channel
 * histories. Each channel is checked against its own independent reference;
 * the two channels must never collapse to the same state.
 */
static void case_stereo_opposite_channels(void)
{
    const char *name = "stereo_opposite_channels";
    struct aptx_QMF_analysis ref_l, ref_r, neon_l, neon_r;
    unsigned long long before;
    unsigned iter;

    fill_tree_random(&ref_l, 0x1111, 3);
    fill_tree_random(&ref_r, 0x2222, 11);
    neon_l = ref_l;
    neon_r = ref_r;

    before = aptx_qmf_neon_kernel_calls;
    for (iter = 0; iter < 40; iter++) {
        int32_t lin[4], rin[4];
        int32_t ref_lsub[NB_SUBBANDS], ref_rsub[NB_SUBBANDS];
        int32_t neon_lsub[NB_SUBBANDS], neon_rsub[NB_SUBBANDS];
        unsigned k;

        rng_state = 0x7000 + iter * 53;
        for (k = 0; k < 4; k++) {
            lin[k] = rand_extreme();
            rin[k] = (k & 1) ? -lin[k] : ~lin[k]; /* opposite content */
        }

        ref_tree_analysis(&ref_l, lin, ref_lsub);
        ref_tree_analysis(&ref_r, rin, ref_rsub);
        aptx_qmf_tree_analysis_stereo_neon(&neon_l, &neon_r, lin, rin,
                                           neon_lsub, neon_rsub);

        CHECK(name, memcmp(neon_lsub, ref_lsub, sizeof(ref_lsub)) == 0,
              "iter=%u left subbands", iter);
        CHECK(name, memcmp(neon_rsub, ref_rsub, sizeof(ref_rsub)) == 0,
              "iter=%u right subbands", iter);
        CHECK(name, memcmp(&neon_l, &ref_l, sizeof(ref_l)) == 0,
              "iter=%u left ring state", iter);
        CHECK(name, memcmp(&neon_r, &ref_r, sizeof(ref_r)) == 0,
              "iter=%u right ring state", iter);
        CHECK(name, memcmp(&neon_l, &neon_r, sizeof(neon_l)) != 0,
              "iter=%u left/right state collapsed together", iter);
    }
    CHECK(name, aptx_qmf_neon_kernel_calls - before == 2 * 40,
          "stereo analysis call accounting wrong");

    before = aptx_qmf_neon_kernel_calls;
    for (iter = 0; iter < 40; iter++) {
        int32_t lsub[NB_SUBBANDS], rsub[NB_SUBBANDS];
        int32_t ref_lout[4], ref_rout[4], neon_lout[4], neon_rout[4];
        unsigned k;

        rng_state = 0x8000 + iter * 71;
        for (k = 0; k < NB_SUBBANDS; k++) {
            lsub[k] = rand_extreme();
            rsub[k] = -lsub[k];
        }

        ref_tree_synthesis(&ref_l, lsub, ref_lout);
        ref_tree_synthesis(&ref_r, rsub, ref_rout);
        aptx_qmf_tree_synthesis_stereo_neon(&neon_l, &neon_r, lsub, rsub,
                                            neon_lout, neon_rout);

        CHECK(name, memcmp(neon_lout, ref_lout, sizeof(ref_lout)) == 0,
              "iter=%u left synthesis output", iter);
        CHECK(name, memcmp(neon_rout, ref_rout, sizeof(ref_rout)) == 0,
              "iter=%u right synthesis output", iter);
        CHECK(name, memcmp(&neon_l, &ref_l, sizeof(ref_l)) == 0,
              "iter=%u left synthesis ring state", iter);
        CHECK(name, memcmp(&neon_r, &ref_r, sizeof(ref_r)) == 0,
              "iter=%u right synthesis ring state", iter);
    }
    CHECK(name, aptx_qmf_neon_kernel_calls - before == 2 * 40,
          "stereo synthesis call accounting wrong");

    printf("case %s: PASS\n", name);
}

/*
 * Case 6: unaligned storage (no 16 byte alignment requirement) and
 * single-sided impulses, for both tree directions and the raw convolution.
 */
static void case_unaligned_and_pulse(void)
{
    const char *name = "unaligned_storage_and_pulses";
    static const unsigned offsets[3] = { 1, 2, 3 };
    unsigned oi, iter;

    for (oi = 0; oi < 3; oi++) {
        unsigned char arena_ref[sizeof(struct aptx_QMF_analysis) + 16];
        unsigned char arena_neon[sizeof(struct aptx_QMF_analysis) + 16];
        struct aptx_QMF_analysis tmp;
        struct aptx_QMF_analysis *rp, *np;

        fill_tree_random(&tmp, 0xC000 + oi * 211, (oi + 5) & 15);
        memset(arena_ref, 0xAB, sizeof(arena_ref));
        memset(arena_neon, 0xAB, sizeof(arena_neon));
        memcpy(arena_ref + offsets[oi], &tmp, sizeof(tmp));
        memcpy(arena_neon + offsets[oi], &tmp, sizeof(tmp));
        /* Both the reference and the kernel work from unaligned storage. */
        rp = (struct aptx_QMF_analysis *)(arena_ref + offsets[oi]);
        np = (struct aptx_QMF_analysis *)(arena_neon + offsets[oi]);

        for (iter = 0; iter < 40; iter++) {
            int32_t in[4], ref_out[NB_SUBBANDS], neon_out[NB_SUBBANDS];

            memset(in, 0, sizeof(in));
            if ((iter % 5) == 0)
                in[iter % 4] = (iter & 1) ? INT32_MIN : INT32_MAX; /* impulse */
            else if ((iter % 7) == 0)
                in[3] = (int32_t)((1L << 23) - 1);

            ref_tree_analysis(rp, in, ref_out);
            aptx_qmf_tree_analysis_neon(np, in, neon_out);

            CHECK(name, memcmp(neon_out, ref_out, sizeof(ref_out)) == 0,
                  "off=%u iter=%u unaligned analysis output", offsets[oi], iter);
            /* Whole arenas, including the guard bytes, must stay identical. */
            CHECK(name, memcmp(arena_neon, arena_ref, sizeof(arena_ref)) == 0,
                  "off=%u iter=%u unaligned analysis state", offsets[oi], iter);
        }

        /* raw convolution on an unaligned signal */
        {
            unsigned char sa[sizeof(struct aptx_filter_signal) + 8];
            struct aptx_filter_signal sig, snapshot, aligned_sig;
            int32_t got, want;
            fill_signal_random(&sig, 0xD000 + oi, 7 + oi);
            memcpy(sa + offsets[oi], &sig, sizeof(sig));
            aligned_sig = sig;
            got = aptx_qmf_convolution_neon(
                (const struct aptx_filter_signal *)(sa + offsets[oi]),
                aptx_qmf_outer_coeffs[0], 23);
            want = ref_convolution(&aligned_sig, aptx_qmf_outer_coeffs[0], 23);
            CHECK(name, got == want, "off=%u unaligned convolution", offsets[oi]);
            memcpy(&snapshot, sa + offsets[oi], sizeof(snapshot));
            CHECK(name, memcmp(&snapshot, &sig, sizeof(sig)) == 0,
                  "off=%u unaligned convolution mutated state", offsets[oi]);
        }
    }

    printf("case %s: PASS\n", name);
}

int main(void)
{
    unsigned long long calls_start, calls_end;

    /* Fail loudly if the build silently produced the scalar path. */
    assert(APTX_NEON_KERNELS == 1);

    calls_start = aptx_qmf_neon_kernel_calls;
    case_convolution_dotproduct();
    case_rounding_midpoint();
    case_analysis_tree_allpos();
    case_synthesis_tree_allpos();
    case_stereo_opposite_channels();
    case_unaligned_and_pulse();
    calls_end = aptx_qmf_neon_kernel_calls;

    if (calls_end <= calls_start) {
        printf("case accounting: FAIL: NEON kernel counter did not advance\n");
        return 1;
    }

    printf("qmf_kernel: all 6 named cases passed, neon_kernel_calls=%llu\n",
           (unsigned long long)calls_end);
    return 0;
}
