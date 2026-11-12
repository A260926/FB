/*
 * Regression tests for the IT 2.14/2.15 compressed sample decoder.
 *
 * Every expected PCM value comes from the symbolic generator
 * (itsex_model_generator.c); the decoder output is never frozen into a
 * checksum. The 12 core cases drive itsex_decompress8/16 directly, the
 * 4 public cases load a minimal generated IT module through both
 * xmp_load_module_from_memory and xmp_load_module_from_callbacks.
 */
#include "checks/itsex_model_contract.h"

#include "xmp.h"
#include "src/hio.h"
#include "src/loaders/it.h"

#include <stdlib.h>
#include <string.h>

/* Generator interface; the matching definitions live in
 * itsex_model_generator.c (no third header is shipped). */
#define ITSEX_NWIDTH 18		/* Widths 1..17 plus the unused slot 0. */

struct itsex_model_stats {
    size_t width_data[ITSEX_NWIDTH];
    size_t switches[3];
    size_t control_byte_crossings;
    size_t control_cache_crossings;
};

struct itsex_stream_model {
    int bits;
    int it215;
    int nblocks;
    size_t length;
    uint32_t modulus;
    uint8_t *packed;
    size_t packed_len;
    uint32_t *expected;
    uint32_t block_end_temp;
    uint32_t block_end_temp2;
    uint32_t block1_end_temp;
    uint32_t block1_end_temp2;
    size_t block1_end;
    struct itsex_model_stats stats;
};

struct itsex_stereo_model {
    int bits;
    int it215;
    size_t channel_length;
    struct itsex_stream_model left;
    struct itsex_stream_model right;
    uint8_t *packed;
    size_t packed_len;
};

#define ITSEX_LEN_SHORT 61
#define ITSEX_LEN_TAIL_EXTRA 17

int itsex_model_build(int bits, int it215, size_t length,
                      struct itsex_stream_model *out);
int itsex_model_build_stereo(int bits, int it215, size_t channel_length,
                             struct itsex_stereo_model *out);
size_t itsex_model_build_it_module(const struct itsex_stereo_model *sm,
                                   uint8_t **out_buf);
void itsex_model_free(struct itsex_stream_model *m);
void itsex_model_free_stereo(struct itsex_stereo_model *m);


#define GUARD_SAMPLES 4
#define FILL_BYTE 0xa5u

/* ------------------------------------------------------------------ */
/* Core direct-decoder cases                                          */
/* ------------------------------------------------------------------ */

static void copy_stats(struct itsex_model_result *res,
                       const struct itsex_model_stats *st)
{
    int i;
    for (i = 0; i < ITSEX_NWIDTH; i++)
        res->width_data[i] = st->width_data[i];
    for (i = 0; i < 3; i++)
        res->switches[i] = st->switches[i];
    res->control_byte_crossings = st->control_byte_crossings;
    res->control_cache_crossings = st->control_cache_crossings;
}

static int run_core(int bits, int it215, size_t length,
                    struct itsex_model_result *res)
{
    struct itsex_stream_model m;
    HIO_HANDLE *h;
    size_t total, i;
    int rc = 2;
    int is16 = bits == 16;

    if (itsex_model_build(bits, it215, length, &m) < 0)
        return 2;

    /* Multi-block material must finish its first block with both
     * accumulators nonzero: this is what lets a missing per-block reset
     * change the very first sample of the next block. */
    if (m.nblocks > 1 &&
        (m.block1_end == 0 || m.block1_end >= length ||
         m.block1_end_temp == 0 || m.block1_end_temp2 == 0)) {
        itsex_model_free(&m);
        return 2;
    }

    total = length + 2 * GUARD_SAMPLES;
    size_t nbytes = total * (is16 ? 2u : 1u);
    uint8_t *raw = (uint8_t *)malloc(nbytes);
    uint8_t tmp[65536];
    if (raw == NULL)
        goto out_model;
    memset(raw, FILL_BYTE, nbytes);

    h = hio_open_const_mem(m.packed, (long)m.packed_len);
    if (h == NULL)
        goto out_raw;

    if (is16) {
        int16_t *dst = (int16_t *)(raw + 2 * GUARD_SAMPLES);
        if (itsex_decompress16(h, dst, (int)length, tmp, sizeof(tmp),
                               it215) != 0)
            goto out_hio;
        if (hio_tell(h) != (long)m.packed_len)
            goto out_hio;
        for (i = 0; i < length; i++) {
            uint32_t actual = (uint32_t)(uint16_t)dst[i];
            if (actual != m.expected[i]) {
                res->samples = i + 1;
                res->first_sample = i;
                res->expected = m.expected[i];
                res->actual = actual;
                rc = 1;
                goto out_hio;
            }
        }
    } else {
        uint8_t *dst = raw + GUARD_SAMPLES;
        if (itsex_decompress8(h, dst, (int)length, tmp, sizeof(tmp),
                              it215) != 0)
            goto out_hio;
        if (hio_tell(h) != (long)m.packed_len)
            goto out_hio;
        for (i = 0; i < length; i++) {
            uint32_t actual = (uint32_t)dst[i];
            if (actual != m.expected[i]) {
                res->samples = i + 1;
                res->first_sample = i;
                res->expected = m.expected[i];
                res->actual = actual;
                rc = 1;
                goto out_hio;
            }
        }
    }

    /* Sentinel bytes around the target buffer must be untouched. */
    for (i = 0; i < nbytes; i++) {
        size_t lo = (size_t)GUARD_SAMPLES * (is16 ? 2u : 1u);
        size_t hi = lo + length * (is16 ? 2u : 1u);
        if ((i < lo || i >= hi) && raw[i] != (uint8_t)FILL_BYTE)
            goto out_hio;
    }

    res->samples = length;
    copy_stats(res, &m.stats);
    rc = 0;

out_hio:
    hio_close(h);
out_raw:
    free(raw);
out_model:
    itsex_model_free(&m);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Public module loading cases                                        */
/* ------------------------------------------------------------------ */

struct cb_state {
    const uint8_t *data;
    size_t size;
    long pos;
};

static unsigned long cb_read(void *dest, unsigned long len,
                             unsigned long nmemb, void *priv)
{
    struct cb_state *s = (struct cb_state *)priv;
    size_t want = (size_t)len * nmemb;
    size_t avail = (size_t)s->pos < s->size ? s->size - (size_t)s->pos : 0;
    size_t take = want < avail ? want : avail;

    memcpy(dest, s->data + s->pos, take);
    s->pos += (long)take;
    /* Full elements only: never manufacture a short-read failure. */
    return len ? (unsigned long)(take / len) : nmemb;
}

static int cb_seek(void *priv, long offset, int whence)
{
    struct cb_state *s = (struct cb_state *)priv;
    long np;

    switch (whence) {
    case SEEK_SET:
        np = offset;
        break;
    case SEEK_CUR:
        np = s->pos + offset;
        break;
    case SEEK_END:
        np = (long)s->size + offset;
        break;
    default:
        return -1;
    }
    if (np < 0 || (size_t)np > s->size)
        return -1;
    s->pos = np;
    return 0;
}

static long cb_tell(void *priv)
{
    return ((struct cb_state *)priv)->pos;
}

/* Compare the whole interleaved stereo sample against both symbolic
 * channel models, including the loader's unsigned-to-signed conversion
 * (8-bit +0x80, 16-bit xor 0x8000). */
static int compare_stereo(const struct itsex_stereo_model *sm,
                          const struct xmp_sample *xxs,
                          struct itsex_model_result *res)
{
    size_t len = sm->channel_length;
    size_t frames, i;
    int is16 = sm->bits == 16;

    if (xxs->len != (int)len || xxs->data == NULL)
        return 2;
    if (!(xxs->flg & XMP_SAMPLE_STEREO))
        return 2;
    if (is16 != !!(xxs->flg & XMP_SAMPLE_16BIT))
        return 2;

    frames = len * 2;
    for (i = 0; i < frames; i++) {
        int right = (int)(i & 1u);
        size_t frame = i >> 1;
        const struct itsex_stream_model *ch = right ? &sm->right : &sm->left;
        uint32_t expected = ch->expected[frame];
        uint32_t actual;

        if (is16) {
            const uint16_t *w = (const uint16_t *)xxs->data;
            actual = (uint32_t)w[i];
            expected = (expected ^ 0x8000u) & 0xffffu;
        } else {
            actual = (uint32_t)xxs->data[i];
            expected = (expected + 0x80u) & 0xffu;
        }

        if (actual != expected) {
            res->first_sample = i;
            res->expected = expected;
            res->actual = actual;
            return 1;
        }
    }
    res->samples += frames;
    return 0;
}

static int run_public(int bits, int it215,
                      struct itsex_model_result *res)
{
    size_t block = bits == 8 ? 32768u : 16384u;
    size_t channel_length = block + ITSEX_LEN_TAIL_EXTRA;
    struct itsex_stereo_model sm;
    uint8_t *module = NULL;
    size_t module_size;
    xmp_context ctx;
    struct xmp_module_info info;
    int rc = 2;
    int cmp;

    if (itsex_model_build_stereo(bits, it215, channel_length, &sm) < 0)
        return 2;
    if (sm.left.nblocks < 2 || sm.right.nblocks < 2 ||
        sm.left.block1_end_temp == 0 || sm.left.block1_end_temp2 == 0 ||
        sm.right.block1_end_temp == 0 || sm.right.block1_end_temp2 == 0) {
        itsex_model_free_stereo(&sm);
        return 2;
    }
    module_size = itsex_model_build_it_module(&sm, &module);
    if (module_size == 0)
        goto out;

    /* Aggregate generator-side coverage from both channel models. */
    copy_stats(res, &sm.left.stats);
    {
        int i;
        for (i = 0; i < ITSEX_NWIDTH; i++)
            res->width_data[i] += sm.right.stats.width_data[i];
        for (i = 0; i < 3; i++)
            res->switches[i] += sm.right.stats.switches[i];
        res->control_byte_crossings +=
            sm.right.stats.control_byte_crossings;
        res->control_cache_crossings +=
            sm.right.stats.control_cache_crossings;
    }

    /* Entry point 1: memory. */
    ctx = xmp_create_context();
    if (ctx == NULL)
        goto out_module;
    if (xmp_load_module_from_memory(ctx, module, (long)module_size) != 0) {
        xmp_free_context(ctx);
        goto out_module;
    }
    xmp_get_module_info(ctx, &info);
    if (info.mod == NULL || info.mod->smp < 1) {
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        goto out_module;
    }
    cmp = compare_stereo(&sm, &info.mod->xxs[0], res);
    xmp_release_module(ctx);
    xmp_free_context(ctx);
    if (cmp == 2)
        goto out_module;
    if (cmp == 1) {
        rc = 1;
        goto out_module;
    }
    res->memory_loads = 1;

    /* Entry point 2: callbacks honoring the requested element count. */
    {
        struct cb_state state;
        struct xmp_callbacks cbs;
        state.data = module;
        state.size = module_size;
        state.pos = 0;
        cbs.read_func = cb_read;
        cbs.seek_func = cb_seek;
        cbs.tell_func = cb_tell;
        cbs.close_func = NULL;

        ctx = xmp_create_context();
        if (ctx == NULL)
            goto out_module;
        if (xmp_load_module_from_callbacks(ctx, &state, cbs) != 0) {
            xmp_free_context(ctx);
            goto out_module;
        }
        xmp_get_module_info(ctx, &info);
        if (info.mod == NULL || info.mod->smp < 1) {
            xmp_release_module(ctx);
            xmp_free_context(ctx);
            goto out_module;
        }
        cmp = compare_stereo(&sm, &info.mod->xxs[0], res);
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        if (cmp == 2)
            goto out_module;
        if (cmp == 1) {
            rc = 1;
            goto out_module;
        }
        res->callback_loads = 1;
    }

    rc = 0;

out_module:
    free(module);
out:
    itsex_model_free_stereo(&sm);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Registry: 12 core cases + 4 public cases, no test-owned main()     */
/* ------------------------------------------------------------------ */

static int core_8_214_short(struct itsex_model_result *r)
    { return run_core(8, 0, ITSEX_LEN_SHORT, r); }
static int core_8_214_full(struct itsex_model_result *r)
    { return run_core(8, 0, 32768, r); }
static int core_8_214_tail(struct itsex_model_result *r)
    { return run_core(8, 0, 32768 + ITSEX_LEN_TAIL_EXTRA, r); }
static int core_8_215_short(struct itsex_model_result *r)
    { return run_core(8, 1, ITSEX_LEN_SHORT, r); }
static int core_8_215_full(struct itsex_model_result *r)
    { return run_core(8, 1, 32768, r); }
static int core_8_215_tail(struct itsex_model_result *r)
    { return run_core(8, 1, 32768 + ITSEX_LEN_TAIL_EXTRA, r); }

static int core_16_214_short(struct itsex_model_result *r)
    { return run_core(16, 0, ITSEX_LEN_SHORT, r); }
static int core_16_214_full(struct itsex_model_result *r)
    { return run_core(16, 0, 16384, r); }
static int core_16_214_tail(struct itsex_model_result *r)
    { return run_core(16, 0, 16384 + ITSEX_LEN_TAIL_EXTRA, r); }
static int core_16_215_short(struct itsex_model_result *r)
    { return run_core(16, 1, ITSEX_LEN_SHORT, r); }
static int core_16_215_full(struct itsex_model_result *r)
    { return run_core(16, 1, 16384, r); }
static int core_16_215_tail(struct itsex_model_result *r)
    { return run_core(16, 1, 16384 + ITSEX_LEN_TAIL_EXTRA, r); }

static int public_8_214_stereo(struct itsex_model_result *r)
    { return run_public(8, 0, r); }
static int public_8_215_stereo(struct itsex_model_result *r)
    { return run_public(8, 1, r); }
static int public_16_214_stereo(struct itsex_model_result *r)
    { return run_public(16, 0, r); }
static int public_16_215_stereo(struct itsex_model_result *r)
    { return run_public(16, 1, r); }

static const struct itsex_model_case itsex_registry[16] = {
    { "core-8-214-short",  core_8_214_short  },
    { "core-8-214-full",   core_8_214_full   },
    { "core-8-214-tail",   core_8_214_tail   },
    { "core-8-215-short",  core_8_215_short  },
    { "core-8-215-full",   core_8_215_full   },
    { "core-8-215-tail",   core_8_215_tail   },
    { "core-16-214-short", core_16_214_short },
    { "core-16-214-full",  core_16_214_full  },
    { "core-16-214-tail",  core_16_214_tail  },
    { "core-16-215-short", core_16_215_short },
    { "core-16-215-full",  core_16_215_full  },
    { "core-16-215-tail",  core_16_215_tail  },
    { "public-8-214-stereo",  public_8_214_stereo  },
    { "public-8-215-stereo",  public_8_215_stereo  },
    { "public-16-214-stereo", public_16_214_stereo },
    { "public-16-215-stereo", public_16_215_stereo }
};

size_t itsex_model_register(const struct itsex_model_case **cases)
{
    *cases = itsex_registry;
    return sizeof(itsex_registry) / sizeof(itsex_registry[0]);
}
