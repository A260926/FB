/*
 * Timing comparison between the scalar QMF/codec and the AArch64 NEON
 * implementation. Everything is built with the same compiler at -O3 with
 * the compiler's normal auto-vectorization left enabled; the point is the
 * measured speedup of the hand written NEON kernels, not a handicap on the
 * scalar build.
 *
 * Two workloads are measured:
 *  - QMF only: stereo two-stage analysis + synthesis trees. The scalar side
 *    is the independent scalar reference (qmf_ref.h): a genuinely scalar
 *    int64 dot product (the interleaved pushes/convolutions do not
 *    auto-vectorize). The NEON side uses the stereo kernel entry points.
 *    Both perform the same number of tree executions on identical states.
 *  - Full codec: public API encode (+finish) and decode on a long stereo
 *    batch, comparing the renamed scalar library symbols (compiled normally
 *    at -O3 with auto-vectorization enabled) against the NEON library
 *    symbols in the same process.
 *
 * No millisecond threshold is enforced: medians and the actual measured
 * speedup ratio are reported.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "openaptx.h"
#include "openaptx_internal.h"
#include "qmf_ref.h"

/* Scalar reference public symbols, renamed at compile time by run_neon.sh. */
struct aptx_context;
extern struct aptx_context *aptx_init_ref(int hd);
extern void aptx_reset_ref(struct aptx_context *ctx);
extern void aptx_finish_ref(struct aptx_context *ctx);
extern size_t aptx_encode_ref(struct aptx_context *ctx, const unsigned char *input,
                              size_t input_size, unsigned char *output,
                              size_t output_size, size_t *written);
extern int aptx_encode_finish_ref(struct aptx_context *ctx, unsigned char *output,
                                  size_t output_size, size_t *written);
extern size_t aptx_decode_ref(struct aptx_context *ctx, const unsigned char *input,
                              size_t input_size, unsigned char *output,
                              size_t output_size, size_t *written);

#define ITER 100000          /* stereo codec samples / tree pairs per run */
#define RUNS 11              /* timed runs (odd, median is the middle one) */
#define WARMUP 5
#define PCM_BYTES (ITER * 24)

static unsigned char pcm[PCM_BYTES];
static unsigned char code_neon[ITER * 6 + 256];
static unsigned char code_ref[ITER * 6 + 256];
static unsigned char out_neon[PCM_BYTES + 4096];
static unsigned char out_ref[PCM_BYTES + 4096];

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

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t median_u64(uint64_t *v, size_t n)
{
    qsort(v, n, sizeof(*v), cmp_u64);
    return v[n / 2];
}

static void make_input(void)
{
    size_t i;
    rng_state = 0x243f6a88u;
    for (i = 0; i < sizeof(pcm); i++)
        pcm[i] = (unsigned char)xs32();
}

/* QMF-only timing, scalar reference trees vs stereo NEON trees. */
static void bench_qmf(uint64_t *scalar_ns, uint64_t *neon_ns)
{
    struct aptx_QMF_analysis *sl = calloc(1, sizeof(*sl));
    struct aptx_QMF_analysis *sr = calloc(1, sizeof(*sr));
    struct aptx_QMF_analysis *nl = calloc(1, sizeof(*nl));
    struct aptx_QMF_analysis *nr = calloc(1, sizeof(*nr));
    int32_t in[4], sub[NB_SUBBANDS], tmp_s[4], tmp_n[4];
    uint64_t times_s[RUNS], times_n[RUNS];
    int run, i;
    assert(sl && sr && nl && nr);

    for (run = 0; run < RUNS + WARMUP; run++) {
        uint64_t t0, t1;
        rng_state = 0x85ebca6bu;

        t0 = now_ns();
        for (i = 0; i < ITER; i++) {
            int32_t ls[NB_SUBBANDS];
            unsigned k;
            for (k = 0; k < 4; k++)
                in[k] = (int32_t)xs32();
            ref_tree_analysis(sl, in, ls);
            ref_tree_analysis(sr, in, ls); /* distinct state, same work shape */
            for (k = 0; k < NB_SUBBANDS; k++)
                sub[k] = ls[k];
            ref_tree_synthesis(sl, sub, tmp_s);
            ref_tree_synthesis(sr, sub, tmp_s);
        }
        t1 = now_ns();
        if (run >= WARMUP)
            times_s[run - WARMUP] = t1 - t0;

        rng_state = 0x85ebca6bu;
        t0 = now_ns();
        for (i = 0; i < ITER; i++) {
            int32_t ls[NB_SUBBANDS], rs[NB_SUBBANDS];
            unsigned k;
            for (k = 0; k < 4; k++)
                in[k] = (int32_t)xs32();
            aptx_qmf_tree_analysis_stereo_neon(nl, nr, in, in, ls, rs);
            for (k = 0; k < NB_SUBBANDS; k++)
                sub[k] = ls[k];
            aptx_qmf_tree_synthesis_stereo_neon(nl, nr, sub, sub, tmp_n, tmp_n);
        }
        t1 = now_ns();
        if (run >= WARMUP)
            times_n[run - WARMUP] = t1 - t0;
    }

    /* The two implementations must stay bit identical while benchmarked. */
    assert(memcmp(sl, nl, sizeof(*sl)) == 0);
    assert(memcmp(sr, nr, sizeof(*sr)) == 0);

    *scalar_ns = median_u64(times_s, RUNS);
    *neon_ns = median_u64(times_n, RUNS);
    free(sl); free(sr); free(nl); free(nr);
}

/* Full public API codec timing, scalar vs NEON. Contexts are allocated once
 * outside the timed region and reset through the public API inside it, so
 * the measurement reflects the encode/finish/decode hot path rather than
 * allocator noise. */
static void bench_codec(uint64_t *scalar_ns, uint64_t *neon_ns)
{
    uint64_t times_s[RUNS], times_n[RUNS];
    struct aptx_context *re = aptx_init_ref(0), *rd = aptx_init_ref(0);
    struct aptx_context *ne = aptx_init(0), *nd = aptx_init(0);
    int run;
    assert(re && rd && ne && nd);

    for (run = 0; run < RUNS + WARMUP; run++) {
        size_t used, w, tail, len_n, len_r, dec_n, dec_r;
        uint64_t t0, t1;

        aptx_reset_ref(re);
        aptx_reset_ref(rd);
        t0 = now_ns();
        used = aptx_encode_ref(re, pcm, sizeof(pcm), code_ref, sizeof(code_ref), &w);
        assert(used == sizeof(pcm));
        assert(aptx_encode_finish_ref(re, code_ref + w, sizeof(code_ref) - w, &tail));
        len_r = w + tail;
        dec_r = aptx_decode_ref(rd, code_ref, len_r, out_ref, sizeof(out_ref), &tail);
        assert(dec_r == len_r);
        t1 = now_ns();
        if (run >= WARMUP)
            times_s[run - WARMUP] = t1 - t0;

        aptx_reset(ne);
        aptx_reset(nd);
        t0 = now_ns();
        used = aptx_encode(ne, pcm, sizeof(pcm), code_neon, sizeof(code_neon), &w);
        assert(used == sizeof(pcm));
        assert(aptx_encode_finish(ne, code_neon + w, sizeof(code_neon) - w, &tail));
        len_n = w + tail;
        dec_n = aptx_decode(nd, code_neon, len_n, out_neon, sizeof(out_neon), &tail);
        assert(dec_n == len_n);
        t1 = now_ns();
        if (run >= WARMUP)
            times_n[run - WARMUP] = t1 - t0;

        /* Timing workloads must remain bit identical across the two builds. */
        assert(len_n == len_r);
        assert(memcmp(code_ref, code_neon, len_n) == 0);
        assert(memcmp(out_ref, out_neon, tail) == 0);
    }

    aptx_finish_ref(re);
    aptx_finish_ref(rd);
    aptx_finish(ne);
    aptx_finish(nd);
    *scalar_ns = median_u64(times_s, RUNS);
    *neon_ns = median_u64(times_n, RUNS);
}

static void report(const char *label, uint64_t scalar_ns, uint64_t neon_ns)
{
    double speedup = (double)scalar_ns / (double)neon_ns;
    printf("bench %-16s scalar=%10llu ns  neon=%10llu ns  speedup=%5.2fx\n",
           label, (unsigned long long)scalar_ns, (unsigned long long)neon_ns,
           speedup);
}

int main(void)
{
    uint64_t qs, qn, cs, cn;

#if !APTX_NEON_KERNELS
    printf("qmf_bench: FAIL: built without NEON kernels\n");
    return 1;
#endif

    make_input();
    bench_qmf(&qs, &qn);
    bench_codec(&cs, &cn);

    printf("qmf_bench: %d stereo samples/run, median of %d runs after %d warmup, -O3 auto-vectorization on\n",
           ITER, RUNS, WARMUP);
    report("qmf_only", qs, qn);
    report("full_codec", cs, cn);
    return 0;
}
