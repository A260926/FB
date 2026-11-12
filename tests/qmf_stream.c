/*
 * Public API bit exactness test: the scalar implementation (compiled with
 * APTX_FORCE_SCALAR and symbol-renamed by the build script, *_ref names) is
 * run in lockstep with the AArch64 NEON implementation through the public
 * openaptx.h interface only. Every encoded stream byte, every decoded PCM
 * byte, every consumed length and every written length is compared, along
 * with sync state and dropped byte accounting. No output is trimmed to make
 * comparisons pass; the two implementations must agree completely,
 * including the two padding frames produced by encode_finish().
 */

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openaptx.h"
#include "openaptx_internal.h"

/* Scalar reference symbols, renamed at compile time by run_neon.sh. */
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
extern size_t aptx_decode_sync_ref(struct aptx_context *ctx, const unsigned char *input,
                                   size_t input_size, unsigned char *output,
                                   size_t output_size, size_t *written,
                                   int *synced, size_t *dropped);
extern size_t aptx_decode_sync_finish_ref(struct aptx_context *ctx);

struct aptx_ops {
    const char *name;
    struct aptx_context *(*init)(int hd);
    void (*reset)(struct aptx_context *ctx);
    void (*finish)(struct aptx_context *ctx);
    size_t (*encode)(struct aptx_context *ctx, const unsigned char *input,
                     size_t input_size, unsigned char *output,
                     size_t output_size, size_t *written);
    int (*encode_finish)(struct aptx_context *ctx, unsigned char *output,
                         size_t output_size, size_t *written);
    size_t (*decode)(struct aptx_context *ctx, const unsigned char *input,
                     size_t input_size, unsigned char *output,
                     size_t output_size, size_t *written);
    size_t (*decode_sync)(struct aptx_context *ctx, const unsigned char *input,
                          size_t input_size, unsigned char *output,
                          size_t output_size, size_t *written,
                          int *synced, size_t *dropped);
    size_t (*decode_sync_finish)(struct aptx_context *ctx);
};

static const struct aptx_ops neon_ops = {
    "NEON", aptx_init, aptx_reset, aptx_finish, aptx_encode, aptx_encode_finish,
    aptx_decode, aptx_decode_sync, aptx_decode_sync_finish,
};
static const struct aptx_ops ref_ops = {
    "scalar", aptx_init_ref, aptx_reset_ref, aptx_finish_ref, aptx_encode_ref,
    aptx_encode_finish_ref, aptx_decode_ref, aptx_decode_sync_ref,
    aptx_decode_sync_finish_ref,
};

#define NSAMP 1500
#define PCM_BYTES (NSAMP * 24)
#define MAX_CODE (NSAMP * 6 + 256)
#define MAX_PCM (PCM_BYTES + 4096)

static unsigned char pcm[PCM_BYTES];

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

static void die(const char *scenario, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("case %s: FAIL: ", scenario);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}

/* Deterministic 24bit signed stereo PCM with regular extreme samples. */
static void generate_pcm(void)
{
    size_t i;
    rng_state = 0x9e3779b9u;
    for (i = 0; i < PCM_BYTES; i++) {
        pcm[i] = (unsigned char)xs32();
        if ((i % (24 * 37)) == 0) {
            static const unsigned char extremes[][3] = {
                { 0xFF, 0xFF, 0x7F }, /*  2^23 - 1 */
                { 0x00, 0x00, 0x80 }, /* -2^23       */
                { 0xFF, 0xFF, 0xFF }, /* -1          */
                { 0x00, 0x00, 0x00 }, /*  0          */
            };
            memcpy(pcm + i, extremes[(i / 24 / 37) & 3], 3);
        }
    }
}

struct encode_result {
    unsigned char data[MAX_CODE];
    size_t len;
};

/* Chunked encode followed by a single full-capacity finish. */
static struct encode_result encode_stream(const struct aptx_ops *ops, int hd,
                                          const char *scenario)
{
    struct encode_result r;
    struct aptx_context *ctx = ops->init(hd);
    size_t ipos = 0, opos = 0, used, written, tail;
    int finished;
    assert(ctx);
    r.len = 0;

    while (ipos < PCM_BYTES) {
        size_t in_chunk = (size_t)(24 * (1 + (xs32() % 13)));
        size_t out_cap = (size_t)(hd ? 6 : 4) * (1 + (xs32() % 7));
        if (in_chunk > PCM_BYTES - ipos)
            in_chunk = PCM_BYTES - ipos;
        used = ops->encode(ctx, pcm + ipos, in_chunk, r.data + opos,
                           out_cap, &written);
        if (used > in_chunk || written > out_cap)
            die(scenario, "%s encode overflow used=%zu written=%zu cap=%zu",
                ops->name, used, written, out_cap);
        if (used == 0)
            die(scenario, "%s encode stall with capacity for one sample", ops->name);
        ipos += used;
        opos += written;
    }

    finished = ops->encode_finish(ctx, r.data + opos, sizeof(r.data) - opos, &tail);
    if (!finished)
        die(scenario, "%s encode_finish did not complete in one call", ops->name);
    opos += tail;
    r.len = opos;
    ops->finish(ctx);
    return r;
}

static void expect_bytes(const char *scenario, const char *what,
                         const unsigned char *a, const unsigned char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            die(scenario, "%s byte %zu differs: %u != %u", what, i, a[i], b[i]);
}

/* Lockstep chunked raw decode with varying input/output capacities. */
static void raw_decode_lockstep(int hd, const unsigned char *code, size_t code_len,
                                const char *scenario)
{
    struct aptx_context *cn = neon_ops.init(hd);
    struct aptx_context *cr = ref_ops.init(hd);
    unsigned char on[MAX_PCM], orf[MAX_PCM];
    const size_t sample_size = hd ? 6 : 4;
    size_t ipos = 0, onpos = 0, orpos = 0;
    unsigned step = 0;

    while (ipos < code_len) {
        size_t in_chunk = sample_size * (1 + (step % 5));
        size_t out_cap = 24 * (1 + (step % 4));
        size_t un, ur, wn, wr;
        if (in_chunk > code_len - ipos)
            in_chunk = code_len - ipos;
        un = neon_ops.decode(cn, code + ipos, in_chunk, on + onpos,
                             out_cap, &wn);
        ur = ref_ops.decode(cr, code + ipos, in_chunk, orf + orpos,
                            out_cap, &wr);
        if (un != ur)
            die(scenario, "decode consumed step=%u neon=%zu ref=%zu", step, un, ur);
        if (wn != wr || wn > out_cap)
            die(scenario, "decode written step=%u neon=%zu ref=%zu cap=%zu",
                step, wn, wr, out_cap);
        expect_bytes(scenario, "decoded PCM", on + onpos, orf + orpos, wn);
        ipos += un;
        onpos += wn;
        orpos += wr;
        step++;
    }
    if (onpos == 0)
        die(scenario, "raw decode produced no output");
    if (onpos != orpos)
        die(scenario, "total decoded PCM length differs");
    neon_ops.finish(cn);
    ref_ops.finish(cr);
    printf("case %s: PASS (%zu PCM bytes, %u steps)\n", scenario, onpos, step);
}

static void scenario_continuous(int hd)
{
    char scenario[32];
    struct encode_result rn, rr;
    snprintf(scenario, sizeof(scenario), "stream_continuous_%s", hd ? "hd" : "aptx");

    rng_state = 0x12340000u + hd;
    rn = encode_stream(&neon_ops, hd, scenario);
    rng_state = 0x12340000u + hd;
    rr = encode_stream(&ref_ops, hd, scenario);

    if (rn.len != rr.len)
        die(scenario, "stream length neon=%zu ref=%zu", rn.len, rr.len);
    expect_bytes(scenario, "encoded stream", rn.data, rr.data, rn.len);
    /* finish appends encode_remaining = (LATENCY+3)/4 codec packets. */
    if (rn.len != (size_t)(hd ? 6 : 4) * (NSAMP + (LATENCY_SAMPLES + 3) / 4))
        die(scenario, "unexpected stream length %zu", rn.len);
    printf("case %s: PASS (%zu stream bytes incl. padding)\n", scenario, rn.len);

    raw_decode_lockstep(hd, rn.data, rn.len,
                        hd ? "stream_raw_decode_hd" : "stream_raw_decode_aptx");
}

/* Encode the whole PCM without finishing, using one big output buffer. */
static size_t encode_all_no_finish(const struct aptx_ops *ops,
                                   struct aptx_context *ctx,
                                   unsigned char *out, size_t out_size,
                                   const char *scenario)
{
    size_t ipos = 0, opos = 0, used, written;
    while (ipos < PCM_BYTES) {
        used = ops->encode(ctx, pcm + ipos, PCM_BYTES - ipos,
                           out + opos, out_size - opos, &written);
        if (used == 0)
            die(scenario, "%s encode stall", ops->name);
        ipos += used;
        opos += written;
    }
    return opos;
}

/*
 * encode_finish must keep returning 0 and partial data when the output
 * buffer is too small, and both implementations must agree at every step.
 * A full-capacity finish on a second context pair provides the reference
 * tail; the partial finishes must reproduce it byte for byte.
 */
static void scenario_finish_capacities(int hd)
{
    const char *scenario = "stream_finish_capacities";
    const size_t sample_size = hd ? 6 : 4;
    const size_t caps[6] = { 1, sample_size, sample_size * 2, 3, sample_size, 7 };
    struct aptx_context *full_n = neon_ops.init(hd);
    struct aptx_context *full_r = ref_ops.init(hd);
    struct aptx_context *part_n = neon_ops.init(hd);
    struct aptx_context *part_r = ref_ops.init(hd);
    unsigned char stream_n[MAX_CODE], stream_r[MAX_CODE];
    unsigned char tail_n[256], tail_r[256];
    size_t fn_len, fr_len, pn_len, pr_len, wn, wr, full_tail_n, full_tail_r;
    size_t tn = 0, tr = 0;
    int ret_n, ret_r, ci = 0;

    fn_len = encode_all_no_finish(&neon_ops, full_n, stream_n, sizeof(stream_n), scenario);
    fr_len = encode_all_no_finish(&ref_ops, full_r, stream_r, sizeof(stream_r), scenario);
    if (fn_len != fr_len)
        die(scenario, "pre-finish length mismatch");
    expect_bytes(scenario, "pre-finish stream", stream_n, stream_r, fn_len);

    pn_len = encode_all_no_finish(&neon_ops, part_n, stream_n, sizeof(stream_n), scenario);
    pr_len = encode_all_no_finish(&ref_ops, part_r, stream_r, sizeof(stream_r), scenario);
    if (pn_len != fn_len || pr_len != fr_len)
        die(scenario, "re-encode length mismatch");

    /* Full capacity finish: the reference tail. */
    ret_n = neon_ops.encode_finish(full_n, tail_n, sizeof(tail_n), &full_tail_n);
    ret_r = ref_ops.encode_finish(full_r, tail_r, sizeof(tail_r), &full_tail_r);
    if (!ret_n || !ret_r || full_tail_n != full_tail_r)
        die(scenario, "full capacity finish mismatch ret=%d/%d w=%zu/%zu",
            ret_n, ret_r, full_tail_n, full_tail_r);
    expect_bytes(scenario, "full finish tail", tail_n, tail_r, full_tail_n);

    /* Fragmented finish with deliberately awkward capacities. */
    do {
        size_t cap = caps[ci % (sizeof(caps) / sizeof(caps[0]))];
        ret_n = neon_ops.encode_finish(part_n, tail_n + tn, cap, &wn);
        ret_r = ref_ops.encode_finish(part_r, tail_r + tr, cap, &wr);
        if (ret_n != ret_r || wn != wr)
            die(scenario, "partial finish step %d cap=%zu ret=%d/%d w=%zu/%zu",
                ci, cap, ret_n, ret_r, wn, wr);
        expect_bytes(scenario, "partial finish tail", tail_n + tn, tail_r + tr, wn);
        tn += wn;
        tr += wr;
        ci++;
        if (ci > 200)
            die(scenario, "finish never completed");
    } while (!ret_n || !ret_r);

    if (tn != full_tail_n || tr != tn)
        die(scenario, "partial finish total %zu/%zu != full finish %zu",
            tn, tr, full_tail_n);
    expect_bytes(scenario, "reassembled tail", tail_n, tail_r, tn);

    neon_ops.finish(full_n);
    ref_ops.finish(full_r);
    neon_ops.finish(part_n);
    ref_ops.finish(part_r);
    printf("case %s%s: PASS (%zu finish bytes in %d partial calls)\n",
           scenario, hd ? "_hd" : "_aptx", tn, ci);
}

/* Reset must make a reused context indistinguishable from a fresh one. */
static void scenario_reset_reuse(int hd)
{
    const char *scenario = "stream_reset_reuse";
    struct aptx_context *cn = neon_ops.init(hd);
    struct aptx_context *cr = ref_ops.init(hd);
    struct aptx_context *fn = neon_ops.init(hd);
    struct aptx_context *fr = ref_ops.init(hd);
    unsigned char reset_n[4096], reset_r[4096], fresh_n[4096], fresh_r[4096];
    const size_t prefix = 137 * 24;
    size_t used_n, used_r, wn, wr, ipos;
    size_t rn = 0, rr = 0, bn = 0, br = 0;

    /* First life: encode some PCM, then reset the reused contexts. */
    used_n = neon_ops.encode(cn, pcm, prefix, reset_n, sizeof(reset_n), &wn);
    used_r = ref_ops.encode(cr, pcm, prefix, reset_r, sizeof(reset_r), &wr);
    if (used_n != used_r || wn != wr)
        die(scenario, "first-life encode mismatch");
    expect_bytes(scenario, "first-life stream", reset_n, reset_r, wn);

    neon_ops.reset(cn);
    ref_ops.reset(cr);

    /* Second life: same PCM through reset contexts and fresh contexts. */
    for (ipos = 0; ipos < prefix; ) {
        size_t chunk = prefix - ipos;
        if (chunk > 408)
            chunk = 408;

        used_n = neon_ops.encode(cn, pcm + ipos, chunk, reset_n + rn,
                                 sizeof(reset_n) - rn, &wn);
        used_r = ref_ops.encode(cr, pcm + ipos, chunk, reset_r + rr,
                                sizeof(reset_r) - rr, &wr);
        if (used_n != used_r || wn != wr)
            die(scenario, "reset context lockstep mismatch");
        expect_bytes(scenario, "reset-context stream", reset_n + rn, reset_r + rr, wn);

        used_n = neon_ops.encode(fn, pcm + ipos, chunk, fresh_n + bn,
                                 sizeof(fresh_n) - bn, &wn);
        used_r = ref_ops.encode(fr, pcm + ipos, chunk, fresh_r + br,
                                sizeof(fresh_r) - br, &wr);
        if (used_n != used_r || wn != wr)
            die(scenario, "fresh context lockstep mismatch");
        expect_bytes(scenario, "fresh-context stream", fresh_n + bn, fresh_r + br, wr);

        if (rn != bn || memcmp(reset_n + rn, fresh_n + bn, wn) != 0)
            die(scenario, "reset context differs from fresh context at %zu", rn);

        ipos += used_n;
        rn += wn;
        rr += wr;
        bn += wn;
        br += wr;
    }

    /* Finishing the reset and the fresh contexts must agree as well. */
    {
        size_t fw_n, fw_r;
        int ok_n = neon_ops.encode_finish(cn, reset_n + rn, sizeof(reset_n) - rn, &fw_n);
        int ok_r = ref_ops.encode_finish(cr, reset_r + rr, sizeof(reset_r) - rr, &fw_r);
        size_t gw_n, gw_r;
        int hn = neon_ops.encode_finish(fn, fresh_n + bn, sizeof(fresh_n) - bn, &gw_n);
        int hr = ref_ops.encode_finish(fr, fresh_r + br, sizeof(fresh_r) - br, &gw_r);
        if (!ok_n || !ok_r || !hn || !hr || fw_n != fw_r || gw_n != gw_r || fw_n != gw_n)
            die(scenario, "finish after reset mismatch");
        expect_bytes(scenario, "finish after reset", reset_n + rn, fresh_n + bn, fw_n);
    }

    neon_ops.finish(cn);
    ref_ops.finish(cr);
    neon_ops.finish(fn);
    ref_ops.finish(fr);
    printf("case %s%s: PASS\n", scenario, hd ? "_hd" : "_aptx");
}

/*
 * Delete one encoded byte and recover through fragmented aptx_decode_sync
 * calls. synced/dropped accounting and every output byte must match.
 */
static void scenario_sync_deleted_byte(int hd)
{
    const char *scenario = "stream_sync_deleted_byte";
    const size_t sample_size = hd ? 6 : 4;
    const size_t frags[9] = { 1, 2, 3, 5, 7, 11, 4, sample_size + 1, 13 };
    struct encode_result full;
    unsigned char *damaged;
    struct aptx_context *cn, *cr;
    unsigned char on[MAX_PCM], orf[MAX_PCM];
    size_t del, ipos = 0, onpos = 0, orpos = 0, total_drop_n = 0, total_drop_r = 0;
    size_t leftover_n, leftover_r;
    unsigned fi = 0, steps = 0;
    int saw_desync = 0, final_sync_n = -1, final_sync_r = -1;

    rng_state = 0xdead0000u + hd;
    full = encode_stream(&neon_ops, hd, scenario);
    damaged = (unsigned char *)malloc(full.len - 1);
    assert(damaged);
    del = sample_size * 7 + 1; /* inside the 8th codec sample */
    memcpy(damaged, full.data, del);
    memcpy(damaged + del, full.data + del + 1, full.len - del - 1);

    cn = neon_ops.init(hd);
    cr = ref_ops.init(hd);

    while (ipos < full.len - 1) {
        size_t chunk = frags[fi % (sizeof(frags) / sizeof(frags[0]))];
        size_t un, ur, wn, wr, dn, dr;
        int sn, sr;
        if (chunk > (full.len - 1) - ipos)
            chunk = (full.len - 1) - ipos;
        un = neon_ops.decode_sync(cn, damaged + ipos, chunk, on + onpos,
                                  sizeof(on) - onpos, &wn, &sn, &dn);
        ur = ref_ops.decode_sync(cr, damaged + ipos, chunk, orf + orpos,
                                 sizeof(orf) - orpos, &wr, &sr, &dr);
        if (un != ur || wn != wr || sn != sr || dn != dr)
            die(scenario, "step %u mismatch used %zu/%zu written %zu/%zu "
                          "synced %d/%d dropped %zu/%zu",
                steps, un, ur, wn, wr, sn, sr, dn, dr);
        expect_bytes(scenario, "sync decoded PCM", on + onpos, orf + orpos, wn);
        if (!sn)
            saw_desync = 1;
        final_sync_n = sn;
        final_sync_r = sr;
        total_drop_n += dn;
        total_drop_r += dr;
        ipos += un;
        onpos += wn;
        orpos += wr;
        fi++;
        steps++;
        if (steps > 100000)
            die(scenario, "sync decode did not converge");
    }

    leftover_n = neon_ops.decode_sync_finish(cn);
    leftover_r = ref_ops.decode_sync_finish(cr);
    if (leftover_n != leftover_r)
        die(scenario, "finish leftover %zu != %zu", leftover_n, leftover_r);
    if (!saw_desync || final_sync_n != 1 || final_sync_r != 1)
        die(scenario, "expected desync then resync, saw_desync=%d final=%d/%d",
            saw_desync, final_sync_n, final_sync_r);
    if (total_drop_n == 0 || total_drop_n != total_drop_r || onpos != orpos)
        die(scenario, "dropped/output accounting mismatch drops=%zu/%zu out=%zu/%zu",
            total_drop_n, total_drop_r, onpos, orpos);
    if (onpos == 0)
        die(scenario, "sync decode produced no output");

    neon_ops.finish(cn);
    ref_ops.finish(cr);
    free(damaged);
    printf("case %s%s: PASS (%u steps, dropped=%zu, %zu PCM bytes)\n",
           scenario, hd ? "_hd" : "_aptx", steps, total_drop_n, onpos);
}

int main(void)
{
    unsigned long long calls_before, calls_after;

#if !APTX_NEON_KERNELS
    printf("qmf_stream: FAIL: built without NEON kernels\n");
    return 1;
#endif
    assert(APTX_NEON_KERNELS == 1);

    generate_pcm();

    calls_before = aptx_qmf_neon_kernel_calls;

    scenario_continuous(0);
    scenario_continuous(1);
    scenario_finish_capacities(0);
    scenario_finish_capacities(1);
    scenario_reset_reuse(0);
    scenario_reset_reuse(1);
    scenario_sync_deleted_byte(0);
    scenario_sync_deleted_byte(1);

    calls_after = aptx_qmf_neon_kernel_calls;
    if (calls_after <= calls_before) {
        printf("qmf_stream: FAIL: NEON kernel counter never advanced via public API\n");
        return 1;
    }

    printf("qmf_stream: all public API scenarios passed, "
           "neon_kernel_calls_delta=%llu\n",
           (unsigned long long)(calls_after - calls_before));
    return 0;
}
