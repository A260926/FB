/*
 * Target cases for the explicit PCE-driven element layout.
 *
 * Three groups, all compared against real encoded data through task_check:
 *   layout - open-time validation, plan normalization, caller-list lifetime,
 *            and the append-only struct_size compatibility boundary.
 *   config - AudioSpecificConfig, in-band PCE and ADTS parsing through an
 *            independent MSB-first bit reader.
 *   stream - full encode/flush cycles: per-frame element identity, lengths,
 *            pairing, LFE bandwidth, map permutations and reference parity.
 *
 * The file has no main(); task_support/driver.c supplies it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "faac.h"
#include "check.h"

/* The observation points and the working element plan live on the core
 * struct; this test is built with -Ilibfaac. It only reads them. */
#include "frame.h"
#include "channels.h"

/* ------------------------------------------------------------------ */
/* Independent MSB-first bit reader                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *data;
    uint32_t len_bytes;
    uint32_t bit;
} br_t;

static void br_init(br_t *r, const uint8_t *data, uint32_t len)
{
    r->data = data;
    r->len_bytes = len;
    r->bit = 0;
}

static uint32_t br_get(br_t *r, unsigned n)
{
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < n; i++) {
        uint32_t bp = r->bit++;
        v = (v << 1) | ((r->data[bp >> 3] >> (7 - (bp & 7))) & 1u);
    }
    return v;
}

static uint32_t br_peek_at(const uint8_t *data, uint32_t bitpos, unsigned n)
{
    br_t r;
    br_init(&r, data, 0xFFFFFFFFu);
    r.bit = bitpos;
    return br_get(&r, n);
}

/* ------------------------------------------------------------------ */
/* Parsed PCE descriptor                                               */
/* ------------------------------------------------------------------ */

#define TC_MAX_ELEM 8

typedef struct {
    unsigned id_present;            /* 1 for an in-band PCE, 0 for the ASC one */
    unsigned inst_tag, profile, sr_idx;
    unsigned n_front, n_side, n_back, n_lfe, n_assoc, n_cc;
    unsigned mono_present, stereo_present, matrix_present;
    unsigned front_cpe[TC_MAX_ELEM], front_tag[TC_MAX_ELEM];
    unsigned side_cpe[TC_MAX_ELEM],  side_tag[TC_MAX_ELEM];
    unsigned back_cpe[TC_MAX_ELEM],  back_tag[TC_MAX_ELEM];
    unsigned lfe_tag[TC_MAX_ELEM];
    unsigned comment_bytes;
    unsigned align_pad;             /* trailing byte_alignment zero bits */
} pce_desc_t;

static void parse_pce(br_t *r, int with_id, pce_desc_t *d)
{
    unsigned i;

    memset(d, 0, sizeof(*d));
    if (with_id)
        d->id_present = br_get(r, 3);

    d->inst_tag = br_get(r, 4);
    d->profile  = br_get(r, 2);
    d->sr_idx   = br_get(r, 4);
    d->n_front  = br_get(r, 4);
    d->n_side   = br_get(r, 4);
    d->n_back   = br_get(r, 4);
    d->n_lfe    = br_get(r, 4);
    d->n_assoc  = br_get(r, 4);
    d->n_cc     = br_get(r, 4);
    d->mono_present   = br_get(r, 1);
    d->stereo_present = br_get(r, 1);
    d->matrix_present = br_get(r, 1);

    for (i = 0; i < d->n_front; i++) {
        d->front_cpe[i] = br_get(r, 1);
        d->front_tag[i] = br_get(r, 4);
    }
    for (i = 0; i < d->n_side; i++) {
        d->side_cpe[i] = br_get(r, 1);
        d->side_tag[i] = br_get(r, 4);
    }
    for (i = 0; i < d->n_back; i++) {
        d->back_cpe[i] = br_get(r, 1);
        d->back_tag[i] = br_get(r, 4);
    }
    for (i = 0; i < d->n_lfe; i++)
        d->lfe_tag[i] = br_get(r, 4);
    /* assoc/CC counts are zero in this encoder. */
    d->comment_bytes = br_get(r, 8);

    /* byte_alignment() pads to the next byte boundary of the containing
     * stream (the ASC or the raw_data_block), not of this PCE's fields. */
    d->align_pad = (8u - (r->bit & 7u)) & 7u;
    for (i = 0; i < d->align_pad; i++)
        task_check(br_get(r, 1) == 0, "PCE alignment bits are zero");
}

typedef struct {
    unsigned id, mpeg_id, layer, protection;
    unsigned profile, sr_idx, priv, ch_cfg;
    unsigned orig, home, cr_id, cr_start;
    unsigned frame_length, fullness, blocks;
} adts_desc_t;

static void parse_adts(br_t *r, adts_desc_t *a)
{
    memset(a, 0, sizeof(*a));
    a->id           = br_get(r, 12);
    a->mpeg_id      = br_get(r, 1);
    a->layer        = br_get(r, 2);
    a->protection   = br_get(r, 1);
    a->profile      = br_get(r, 2);
    a->sr_idx       = br_get(r, 4);
    a->priv         = br_get(r, 1);
    a->ch_cfg       = br_get(r, 3);
    a->orig         = br_get(r, 1);
    a->home         = br_get(r, 1);
    a->cr_id        = br_get(r, 1);
    a->cr_start     = br_get(r, 1);
    a->frame_length = br_get(r, 13);
    a->fullness     = br_get(r, 11);
    a->blocks       = br_get(r, 2);
}

/* ------------------------------------------------------------------ */
/* Encoder construction helpers                                        */
/* ------------------------------------------------------------------ */

static faac_params base_params(uint32_t rate, uint32_t channels,
                                enum faac_stream_format fmt)
{
    faac_params p;
    faac_status st = faac_params_init(&p, sizeof(p));
    task_check(st == FAAC_OK, "faac_params_init");
    p.sample_rate  = rate;
    p.num_channels = channels;
    p.object_type  = FAAC_OBJ_LOW;
    p.mpeg_version = FAAC_MPEG4;
    p.input_format = FAAC_INPUT_16BIT;
    p.output_format = fmt;
    p.quant_quality = 100;
    p.bit_rate = 0;
    p.rate_control = FAAC_RC_VBR;
    return p;
}

static faac_encoder *must_open(const faac_params *p)
{
    faac_encoder *e = NULL;
    faac_status st = faac_encoder_open(p, &e);
    task_check(st == FAAC_OK, "open must succeed");
    task_check(e != NULL, "handle non-NULL on success");
    return e;
}

static void must_fail(const faac_params *p, faac_status want)
{
    faac_encoder *e = (faac_encoder *)(uintptr_t)0xdeadbeefu;
    faac_status st = faac_encoder_open(p, &e);
    task_check(st == want, "open rejected with the expected status");
    task_check(e == NULL, "failed open leaves the out handle NULL");
}

static faac_pce_element elem(enum faac_pce_region region, int cpe,
                             uint32_t a, uint32_t b)
{
    faac_pce_element e;
    memset(&e, 0, sizeof(e));
    e.region = region;
    e.is_cpe = cpe ? true : false;
    e.channels[0] = a;
    e.channels[1] = cpe ? b : UINT32_MAX;
    return e;
}

/* The seven-channel front/side/back plan used across the tests. Declared
 * deliberately out of region order to prove regrouping. */
static const unsigned PLAN7_CH = 7;
static const faac_pce_element plan7[] = {
    { .region = FAAC_PCE_REGION_SIDE,  .is_cpe = true,  .channels = { 2, 6 } },
    { .region = FAAC_PCE_REGION_FRONT, .is_cpe = false, .channels = { 0, UINT32_MAX } },
    { .region = FAAC_PCE_REGION_BACK,  .is_cpe = false, .channels = { 3, UINT32_MAX } },
    { .region = FAAC_PCE_REGION_FRONT, .is_cpe = true,  .channels = { 1, 5 } },
    { .region = FAAC_PCE_REGION_BACK,  .is_cpe = false, .channels = { 4, UINT32_MAX } },
};
#define PLAN7_COUNT 5u

/* Final emission order expected from plan7. Tags are numbered per element
 * type over the final order: front SCE(0), front CPE(0), side CPE(1),
 * back SCE(1), back SCE(2). */
static const struct {
    ElementID type;
    uint8_t tag;
    int ch0, ch1;
    enum faac_pce_region region;
} plan7_expect[PLAN7_COUNT] = {
    { ID_SCE, 0, 0, -1, FAAC_PCE_REGION_FRONT },
    { ID_CPE, 0, 1,  5, FAAC_PCE_REGION_FRONT },
    { ID_CPE, 1, 2,  6, FAAC_PCE_REGION_SIDE  },
    { ID_SCE, 1, 3, -1, FAAC_PCE_REGION_BACK  },
    { ID_SCE, 2, 4, -1, FAAC_PCE_REGION_BACK  },
};

/* ------------------------------------------------------------------ */
/* layout group                                                        */
/* ------------------------------------------------------------------ */

static int test_layout(void)
{
    /* Seven channels are rejected on the legacy path but accepted with a
     * complete custom plan. */
    {
        faac_params p = base_params(48000, 7, FAAC_STREAM_ADTS);
        must_fail(&p, FAAC_ERR_INVALID_ARGUMENT);

        p.pce_elements = plan7;
        p.pce_element_count = PLAN7_COUNT;
        faac_encoder *e = must_open(&p);
        faacEncStruct *h = (faacEncStruct *)e;

        task_check(h->customPce, "handle runs in custom PCE mode");
        task_check(h->numElements == (int)PLAN7_COUNT, "element count");
        for (unsigned i = 0; i < PLAN7_COUNT; i++) {
            AACElement *el = &h->elements[i];
            task_check(el->type == plan7_expect[i].type, "element type");
            task_check(el->tag == plan7_expect[i].tag, "element tag");
            task_check(el->channels[0] == plan7_expect[i].ch0, "first slot");
            task_check(el->channels[1] == plan7_expect[i].ch1, "second slot");
            task_check((enum faac_pce_region)el->region == plan7_expect[i].region,
                       "region grouping and order");
        }
        faac_encoder_close(&e);
    }

    /* LFE need not be last and is regrouped behind front/back in emission. */
    {
        faac_pce_element list[] = {
            elem(FAAC_PCE_REGION_LFE,   0, 2, UINT32_MAX),
            elem(FAAC_PCE_REGION_BACK,  1, 3, 4),
            elem(FAAC_PCE_REGION_FRONT, 1, 0, 1),
        };
        faac_params p = base_params(48000, 5, FAAC_STREAM_RAW);
        p.pce_elements = list;
        p.pce_element_count = 3;
        faac_encoder *e = must_open(&p);
        faacEncStruct *h = (faacEncStruct *)e;

        task_check(h->elements[0].type == ID_CPE && h->elements[0].tag == 0
                   && h->elements[0].channels[0] == 0 && h->elements[0].channels[1] == 1,
                   "front CPE emitted first");
        task_check(h->elements[1].type == ID_CPE && h->elements[1].tag == 1,
                   "CPE tags number the grouped order");
        task_check(h->elements[2].type == ID_LFE && h->elements[2].tag == 0
                   && h->elements[2].channels[0] == 2, "LFE emitted last");
        task_check(h->isLfeChannel[2] && !h->isLfeChannel[0], "LFE slot map");
        faac_encoder_close(&e);
    }

    /* Failure battery: every malformed plan is refused at open. */
    {
        faac_pce_element good_front = elem(FAAC_PCE_REGION_FRONT, 0, 0, UINT32_MAX);
        faac_pce_element bad_slot_sce = elem(FAAC_PCE_REGION_FRONT, 0, 0, 0);
        faac_pce_element bad_equal_cpe = elem(FAAC_PCE_REGION_FRONT, 1, 1, 1);
        faac_pce_element bad_range = elem(FAAC_PCE_REGION_FRONT, 0, 7, UINT32_MAX);
        faac_pce_element paired_lfe;
        faac_pce_element bad_region = elem((enum faac_pce_region)99, 0, 0, UINT32_MAX);
        faac_pce_element dup_a = elem(FAAC_PCE_REGION_FRONT, 1, 0, 1);
        faac_pce_element dup_b = elem(FAAC_PCE_REGION_BACK,  1, 1, 2);
        faac_pce_element side_only_a = elem(FAAC_PCE_REGION_SIDE, 0, 0, UINT32_MAX);
        faac_pce_element side_only_b = elem(FAAC_PCE_REGION_SIDE, 0, 1, UINT32_MAX);
        faac_pce_element lfe1 = elem(FAAC_PCE_REGION_LFE, 0, 1, UINT32_MAX);
        faac_pce_element lfe2 = elem(FAAC_PCE_REGION_LFE, 0, 2, UINT32_MAX);
        faac_pce_element front3 = elem(FAAC_PCE_REGION_FRONT, 0, 0, UINT32_MAX);
        faac_pce_element short_list[2];

        memset(&paired_lfe, 0, sizeof(paired_lfe));
        paired_lfe.region = FAAC_PCE_REGION_LFE;
        paired_lfe.is_cpe = true;
        paired_lfe.channels[0] = 0;
        paired_lfe.channels[1] = 1;

        faac_params p = base_params(48000, 3, FAAC_STREAM_RAW);

        p.pce_elements = NULL; p.pce_element_count = 2;
        must_fail(&p, FAAC_ERR_INVALID_ARGUMENT);   /* NULL with non-zero count */

        p.pce_elements = &bad_slot_sce; p.pce_element_count = 1;
        must_fail(&p, FAAC_ERR_INVALID_ARGUMENT);   /* SCE second slot set */

        p.pce_elements = &bad_equal_cpe; p.pce_element_count = 1;
        must_fail(&p, FAAC_ERR_INVALID_ARGUMENT);   /* equal CPE slots */

        p.pce_elements = &bad_range; p.pce_element_count = 1;
        must_fail(&p, FAAC_ERR_INVALID_ARGUMENT);   /* slot out of range */

        p.pce_elements = &paired_lfe; p.pce_element_count = 1;
        must_fail(&p, FAAC_ERR_INVALID_ARGUMENT);   /* paired LFE */

        p.pce_elements = &bad_region; p.pce_element_count = 1;
        must_fail(&p, FAAC_ERR_INVALID_ARGUMENT);   /* invalid region */

        {
            faac_pce_element dup[2] = { dup_a, dup_b };
            p.num_channels = 3;
            p.pce_elements = dup; p.pce_element_count = 2;
            must_fail(&p, FAAC_ERR_INVALID_ARGUMENT); /* duplicated + missing slot */
        }
        {
            faac_pce_element nofront[2] = { side_only_a, side_only_b };
            p.num_channels = 2;
            p.pce_elements = nofront; p.pce_element_count = 2;
            must_fail(&p, FAAC_ERR_INVALID_ARGUMENT); /* no front element */
        }
        {
            faac_pce_element twolfe[3] = { front3, lfe1, lfe2 };
            p.num_channels = 3;
            p.pce_elements = twolfe; p.pce_element_count = 3;
            must_fail(&p, FAAC_ERR_INVALID_ARGUMENT); /* two LFEs */
        }
        {
            /* Count mismatch: two elements cannot cover three slots. */
            short_list[0] = elem(FAAC_PCE_REGION_FRONT, 1, 0, 1);
            short_list[1] = elem(FAAC_PCE_REGION_BACK, 0, 2, UINT32_MAX);
            p.num_channels = 4;
            p.pce_elements = short_list; p.pce_element_count = 2;
            must_fail(&p, FAAC_ERR_INVALID_ARGUMENT);
        }
        {
            /* Nine channels exceed the build ceiling. */
            faac_params q = base_params(48000, 9, FAAC_STREAM_RAW);
            faac_pce_element nine[1];
            nine[0] = elem(FAAC_PCE_REGION_FRONT, 0, 0, UINT32_MAX);
            q.pce_elements = nine; q.pce_element_count = 1;
            must_fail(&q, FAAC_ERR_INVALID_ARGUMENT);
        }
        (void)good_front;
    }

    /* Custom mode is MPEG-4 AAC-LC only. */
    {
        faac_params p = base_params(48000, PLAN7_CH, FAAC_STREAM_RAW);
        p.pce_elements = plan7; p.pce_element_count = PLAN7_COUNT;

        p.object_type = FAAC_OBJ_AUTO;
        must_fail(&p, FAAC_ERR_UNSUPPORTED);
        p.object_type = FAAC_OBJ_HE_AAC_V1;
        must_fail(&p, FAAC_ERR_UNSUPPORTED);
        p.object_type = FAAC_OBJ_LOW;
        p.mpeg_version = FAAC_MPEG2;
        must_fail(&p, FAAC_ERR_UNSUPPORTED);
    }

    /* The list is copied: freeing and overwriting it after open is harmless. */
    {
        faac_pce_element *owned = (faac_pce_element *)malloc(sizeof(plan7));
        faac_encoder_info info;
        faac_params p;
        faac_encoder *e;
        uint8_t *out;
        int16_t *in;
        unsigned f;

        task_check(owned != NULL, "plan alloc");
        memcpy(owned, plan7, sizeof(plan7));
        p = base_params(48000, PLAN7_CH, FAAC_STREAM_ADTS);
        p.pce_elements = owned; p.pce_element_count = PLAN7_COUNT;
        e = must_open(&p);
        free(owned);                         /* release the caller copy now */

        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        task_check(faac_encoder_get_info(e, &info) == FAAC_OK, "get_info");
        out = (uint8_t *)malloc(info.max_output_bytes);
        in = (int16_t *)malloc(info.frame_samples * PLAN7_CH * sizeof(int16_t));
        task_check(out && in, "buffers");

        for (f = 0; f < 4; f++) {
            uint32_t n = 0, i;
            for (i = 0; i < info.frame_samples * PLAN7_CH; i++)
                in[i] = (int16_t)(((i * 131 + f * 17) % 12000) - 6000);
            task_check(faac_encoder_encode(e, in, info.frame_samples * PLAN7_CH,
                                           out, info.max_output_bytes, &n) == FAAC_OK,
                       "encode after freeing the caller's plan");
        }
        free(in);
        free(out);
        faac_encoder_close(&e);
    }

    /* Append-only struct boundary: a baseline-sized faac_params has no PCE
     * fields and opens on the legacy layout even though a newer header grew. */
    {
        uint32_t base_size = (uint32_t)(offsetof(faac_params, max_bit_rate)
                                        + sizeof(uint32_t));
        uint8_t storage[sizeof(faac_params) + 16];
        faac_params *p = (faac_params *)storage;
        faac_encoder *e;
        const uint8_t *asc;
        uint32_t alen;

        memset(storage, 0, sizeof(storage));
        task_check(faac_params_init(p, base_size) == FAAC_OK, "init old prefix");
        task_check(p->struct_size == base_size, "struct_size pinned to prefix");
        p->sample_rate = 48000;
        p->num_channels = 2;
        p->object_type = FAAC_OBJ_LOW;
        p->input_format = FAAC_INPUT_16BIT;
        p->output_format = FAAC_STREAM_ADTS;
        e = must_open(p);
        task_check(faac_encoder_asc(e, &asc, &alen) == FAAC_OK && alen == 2,
                   "old prefix keeps the 2-byte legacy ASC");
        faac_encoder_close(&e);

        task_check(faac_params_init(p, base_size - 1) == FAAC_ERR_INVALID_ARGUMENT,
                   "init refuses a struct smaller than the baseline");
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* Deterministic PCM, distinct per channel with a marked transient      */
/* ------------------------------------------------------------------ */

static void fill_pcm(int16_t *in, uint32_t frames, uint32_t channels,
                     unsigned frame_idx, int transient)
{
    uint32_t i, c;

    for (i = 0; i < frames; i++)
        for (c = 0; c < channels; c++) {
            int32_t v = (int32_t)(((i * (97u + c * 53u) + c * 997u
                                    + frame_idx * 113u) % 14000u) - 7000);
            if (transient && i >= 512 && i < 640)
                v += (int32_t)((c + 1) * 2600);
            in[i * channels + c] = (int16_t)v;
        }
}

typedef struct {
    uint8_t *data;
    uint32_t cap, len;
} buf_t;

static int collect_frames(faac_encoder *e, unsigned channels,
                          unsigned feed_frames, buf_t *frames,
                          unsigned *out_count)
{
    faac_encoder_info info;
    uint32_t frame_samples, max_out;
    int16_t *in;
    uint8_t *out;
    unsigned f;
    uint32_t count = 0;

    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    if (faac_encoder_get_info(e, &info) != FAAC_OK)
        return 0;
    frame_samples = info.frame_samples;
    max_out = info.max_output_bytes;
    in = (int16_t *)malloc(frame_samples * channels * sizeof(int16_t));
    out = (uint8_t *)malloc(max_out);
    if (!in || !out) { free(in); free(out); return 0; }

    for (f = 0; f < feed_frames; f++) {
        uint32_t n = 0;
        fill_pcm(in, frame_samples, channels, f, f == feed_frames / 2);
        if (faac_encoder_encode(e, in, frame_samples * channels,
                                out, max_out, &n) != FAAC_OK)
            goto fail;
        if (n) {
            if (frames->len + n > frames->cap) goto fail;
            memcpy(frames->data + frames->len, out, n);
            frames->len += n;
            count++;
        }
    }
    for (;;) {
        uint32_t n = 0;
        if (faac_encoder_encode(e, NULL, 0, out, max_out, &n) != FAAC_OK)
            goto fail;
        if (!n)
            break;
        if (frames->len + n > frames->cap) goto fail;
        memcpy(frames->data + frames->len, out, n);
        frames->len += n;
        count++;
    }
    free(in);
    free(out);
    *out_count = count;
    return 1;
fail:
    free(in);
    free(out);
    return 0;
}

/* Split a collected byte stream into per-frame ADTS payloads. */
static int split_adts(const buf_t *all, buf_t *frames, unsigned max_frames,
                      unsigned *nframes)
{
    uint32_t off = 0;
    unsigned n = 0;

    while (off < all->len && n < max_frames) {
        br_t r;
        adts_desc_t a;
        if (all->data[off] != 0xFF)
            return 0;
        br_init(&r, all->data + off, all->len - off);
        parse_adts(&r, &a);
        if (a.id != 0xFFF || a.frame_length < 7
            || off + a.frame_length > all->len)
            return 0;
        frames[n].data = all->data + off;
        frames[n].len = a.frame_length;
        frames[n].cap = a.frame_length;
        off += a.frame_length;
        n++;
    }
    *nframes = n;
    return off == all->len;
}

/* Raw frames have no length header; the encoder emits one frame per producing
 * call, so raw collection records boundaries directly. Re-encode for that. */
typedef struct {
    uint8_t storage[1 << 13];
    uint32_t len;
} raw_frame_t;

static int collect_raw(faac_encoder *e, unsigned channels, unsigned feed_frames,
                       raw_frame_t *frames, unsigned max_frames,
                       unsigned *out_count)
{
    faac_encoder_info info;
    uint32_t frame_samples, max_out;
    int16_t *in;
    uint8_t *out;
    unsigned f, n = 0;

    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    if (faac_encoder_get_info(e, &info) != FAAC_OK)
        return 0;
    frame_samples = info.frame_samples;
    max_out = info.max_output_bytes;
    in = (int16_t *)malloc(frame_samples * channels * sizeof(int16_t));
    out = (uint8_t *)malloc(max_out);
    if (!in || !out) { free(in); free(out); return 0; }

    for (f = 0; f < feed_frames; f++) {
        uint32_t w = 0;
        fill_pcm(in, frame_samples, channels, f, f == feed_frames / 2);
        if (faac_encoder_encode(e, in, frame_samples * channels,
                                out, max_out, &w) != FAAC_OK)
            goto fail;
        if (w) {
            if (n >= max_frames || w > sizeof(frames[0].storage)) goto fail;
            memcpy(frames[n].storage, out, w);
            frames[n].len = w;
            n++;
        }
    }
    for (;;) {
        uint32_t w = 0;
        if (faac_encoder_encode(e, NULL, 0, out, max_out, &w) != FAAC_OK)
            goto fail;
        if (!w) break;
        if (n >= max_frames || w > sizeof(frames[0].storage)) goto fail;
        memcpy(frames[n].storage, out, w);
        frames[n].len = w;
        n++;
    }
    free(in); free(out);
    *out_count = n;
    return 1;
fail:
    free(in); free(out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* config group                                                        */
/* ------------------------------------------------------------------ */

static void expect_pce_matches_plan7(const pce_desc_t *d)
{
    task_check(d->profile == FAAC_OBJ_LOW - 1, "PCE profile is LC");
    task_check(d->n_assoc == 0 && d->n_cc == 0, "no assoc data or CC");
    task_check(!d->mono_present && !d->stereo_present && !d->matrix_present,
               "mixdown flags zero");
    task_check(d->comment_bytes == 0, "no comment");
    task_check(d->n_front == 2 && d->n_side == 1 && d->n_back == 2 && d->n_lfe == 0,
               "PCE element counts");
    task_check(d->front_cpe[0] == 0 && d->front_tag[0] == 0, "front SCE tag 0");
    task_check(d->front_cpe[1] == 1 && d->front_tag[1] == 0, "front CPE tag 0");
    task_check(d->side_cpe[0] == 1 && d->side_tag[0] == 1, "side CPE tag 1");
    task_check(d->back_cpe[0] == 0 && d->back_tag[0] == 1, "back SCE tag 1");
    task_check(d->back_cpe[1] == 0 && d->back_tag[1] == 2, "back SCE tag 2");
}

static int test_config(void)
{
    faac_params p = base_params(48000, PLAN7_CH, FAAC_STREAM_ADTS);
    faac_encoder *e;
    faacEncStruct *h;
    const uint8_t *asc;
    uint32_t alen;
    br_t r;
    uint32_t aot, sr_idx, ch_cfg, fl_flag, dep_flag, ext_flag;
    uint32_t asc_pce_start;
    pce_desc_t asc_pce;
    buf_t all;
    buf_t frames[64];
    unsigned nframes = 0, i;

    p.pce_elements = plan7; p.pce_element_count = PLAN7_COUNT;
    e = must_open(&p);
    h = (faacEncStruct *)e;

    /* ASC: chCfg 0, GASpecificConfig, then a PCE with no 3-bit ID. */
    task_check(faac_encoder_asc(e, &asc, &alen) == FAAC_OK, "ASC available");
    task_check(alen > 2, "custom ASC carries more than the legacy two bytes");
    br_init(&r, asc, alen);
    aot = br_get(&r, 5);
    sr_idx = br_get(&r, 4);
    ch_cfg = br_get(&r, 4);
    fl_flag = br_get(&r, 1);
    dep_flag = br_get(&r, 1);
    ext_flag = br_get(&r, 1);
    task_check(aot == FAAC_OBJ_LOW, "ASC object type LC");
    task_check(sr_idx == h->sampleRateIdx, "ASC sample rate index is the core rate");
    task_check(ch_cfg == 0, "ASC channelConfiguration 0");
    task_check(fl_flag == 0 && dep_flag == 0 && ext_flag == 0,
               "GASpecificConfig flags zero");
    asc_pce_start = r.bit;
    task_check((asc_pce_start % 8) == 0, "ASC PCE starts on a byte boundary");
    /* The ASC PCE has no ID: its first three bits are the instance tag's top
     * bits, not ID_PCE. */
    task_check(br_peek_at(asc, asc_pce_start, 3) != ID_PCE,
               "ASC PCE is not prefixed by an element ID");
    parse_pce(&r, 0, &asc_pce);
    task_check(asc_pce.id_present == 0, "ASC PCE carried no ID field");
    expect_pce_matches_plan7(&asc_pce);
    task_check(r.bit == alen * 8, "ASC ends exactly on the PCE alignment");

    /* One real ADTS frame: header chCfg 0, then an in-band PCE whose 3-bit ID
     * sits at bit 59 (56-bit header + 3), a different alignment than the ASC. */
    all.data = (uint8_t *)malloc(64u * 1024);
    all.cap = 64u * 1024;
    all.len = 0;
    task_check(all.data != NULL, "stream buffer");
    task_check(collect_frames(e, PLAN7_CH, 6, &all, &nframes), "encode frames");
    task_check(nframes >= 6, "primed frames plus flush tail");
    task_check(split_adts(&all, frames, 64, &nframes), "ADTS framing");

    {
        br_t fr;
        adts_desc_t ah;
        pce_desc_t inband;
        uint32_t inband_start;

        br_init(&fr, frames[0].data, frames[0].len);
        parse_adts(&fr, &ah);
        task_check(ah.id == 0xFFF && ah.protection == 1, "ADTS sync");
        task_check(ah.profile == FAAC_OBJ_LOW - 1, "ADTS profile LC");
        task_check(ah.sr_idx == h->sampleRateIdx, "ADTS rate index");
        task_check(ah.ch_cfg == 0, "ADTS channelConfiguration 0");
        task_check(ah.frame_length == frames[0].len, "ADTS length matches payload");
        inband_start = fr.bit;                  /* 56 */
        task_check(inband_start == 56, "in-band PCE follows the 7-byte header");
        task_check(br_get(&fr, 3) == ID_PCE, "raw_data_block opens with ID_PCE");
        task_check((inband_start % 8) == 0 && ((inband_start + 3) % 8) == 3,
                   "in-band PCE content starts off the byte grid, unlike the ASC");
        parse_pce(&fr, 0, &inband);
        inband.id_present = ID_PCE;
        expect_pce_matches_plan7(&inband);
        task_check(inband.inst_tag == asc_pce.inst_tag
                   && inband.profile == asc_pce.profile
                   && inband.sr_idx == asc_pce.sr_idx
                   && inband.n_front == asc_pce.n_front
                   && inband.n_side == asc_pce.n_side
                   && inband.n_back == asc_pce.n_back
                   && inband.n_lfe == asc_pce.n_lfe,
                   "in-band and ASC PCE describe the same program");
    }

    /* Every collected frame, flush tail included, repeats the PCE. */
    for (i = 0; i < nframes; i++) {
        br_t fr;
        adts_desc_t ah;
        pce_desc_t d;
        br_init(&fr, frames[i].data, frames[i].len);
        parse_adts(&fr, &ah);
        task_check(ah.ch_cfg == 0, "all ADTS headers use chCfg 0");
        task_check(ah.frame_length == frames[i].len, "every ADTS length is exact");
        task_check(br_get(&fr, 3) == ID_PCE, "every frame starts with a PCE");
        parse_pce(&fr, 0, &d);
        expect_pce_matches_plan7(&d);
    }

    free(all.data);
    faac_encoder_close(&e);

    /* Raw output: no ADTS header, the stream's first three bits are ID_PCE. */
    {
        raw_frame_t raw[64];
        unsigned nr = 0;
        faac_params q = base_params(48000, PLAN7_CH, FAAC_STREAM_RAW);
        q.pce_elements = plan7; q.pce_element_count = PLAN7_COUNT;
        faac_encoder *re = must_open(&q);
        task_check(collect_raw(re, PLAN7_CH, 6, raw, 64, &nr) && nr >= 6,
                   "raw encode/flush");
        task_check(br_peek_at(raw[0].storage, 0, 3) == ID_PCE,
                   "raw frame opens with ID_PCE, no ADTS prefix");
        task_check((raw[0].storage[0] & 0xF0) != 0xF0, "no ADTS sync in raw");
        faac_encoder_close(&re);
    }

    /* Legacy default layouts keep their exact 2-byte ASC and fixed chCfg. */
    {
        const uint32_t chans[4] = { 1, 2, 6, 8 };
        const uint32_t expect_cfg[4] = { 1, 2, 6, 7 };
        unsigned k;
        for (k = 0; k < 4; k++) {
            faac_params q = base_params(48000, chans[k], FAAC_STREAM_RAW);
            faac_encoder *le = must_open(&q);
            const uint8_t *a2;
            uint32_t l2;
            br_t rr;
            task_check(faac_encoder_asc(le, &a2, &l2) == FAAC_OK && l2 == 2,
                       "legacy ASC stays two bytes");
            br_init(&rr, a2, l2);
            br_get(&rr, 5);
            br_get(&rr, 4);
            task_check(br_get(&rr, 4) == expect_cfg[k], "legacy fixed channel config");
            faac_encoder_close(&le);
        }
    }

    /* Boundary plans: an 8-channel plan with an LFE, and a 1-channel plan. */
    {
        faac_pce_element eight[] = {
            { .region = FAAC_PCE_REGION_FRONT, .is_cpe = true,  .channels = { 0, 1 } },
            { .region = FAAC_PCE_REGION_FRONT, .is_cpe = false, .channels = { 2, UINT32_MAX } },
            { .region = FAAC_PCE_REGION_SIDE,  .is_cpe = true,  .channels = { 3, 4 } },
            { .region = FAAC_PCE_REGION_BACK,  .is_cpe = true,  .channels = { 5, 6 } },
            { .region = FAAC_PCE_REGION_LFE,   .is_cpe = false, .channels = { 7, UINT32_MAX } },
        };
        faac_pce_element one[] = {
            { .region = FAAC_PCE_REGION_FRONT, .is_cpe = false, .channels = { 0, UINT32_MAX } },
        };
        faac_params p8 = base_params(48000, 8, FAAC_STREAM_RAW);
        faac_params p1 = base_params(48000, 1, FAAC_STREAM_RAW);
        const uint8_t *asc;
        uint32_t alen;
        br_t rr;
        pce_desc_t d;
        faac_encoder *e8, *e1;

        p8.pce_elements = eight; p8.pce_element_count = 5;
        p1.pce_elements = one;   p1.pce_element_count = 1;
        e8 = must_open(&p8);
        e1 = must_open(&p1);

        task_check(faac_encoder_asc(e8, &asc, &alen) == FAAC_OK, "8ch ASC");
        br_init(&rr, asc, alen);
        br_get(&rr, 5); br_get(&rr, 4);
        task_check(br_get(&rr, 4) == 0, "8ch custom ASC chCfg 0");
        br_get(&rr, 3);                       /* GASpecificConfig */
        parse_pce(&rr, 0, &d);
        task_check(d.n_front == 2 && d.n_side == 1 && d.n_back == 1
                   && d.n_lfe == 1, "8ch PCE counts");
        task_check(d.front_cpe[0] == 1 && d.front_cpe[1] == 0
                   && d.side_cpe[0] == 1 && d.back_cpe[0] == 1,
                   "8ch PCE per-region pair flags");
        task_check(d.lfe_tag[0] == 0, "8ch LFE tag 0");
        task_check(rr.bit == alen * 8, "8ch ASC fully consumed");

        task_check(faac_encoder_asc(e1, &asc, &alen) == FAAC_OK, "1ch ASC");
        br_init(&rr, asc, alen);
        br_get(&rr, 5); br_get(&rr, 4);
        task_check(br_get(&rr, 4) == 0, "1ch custom ASC chCfg 0");
        br_get(&rr, 3);
        parse_pce(&rr, 0, &d);
        task_check(d.n_front == 1 && d.n_side == 0 && d.n_back == 0 && d.n_lfe == 0,
                   "1ch PCE is one front SCE");
        task_check(d.front_cpe[0] == 0 && d.front_tag[0] == 0, "1ch SCE tag");
        faac_encoder_close(&e8);
        faac_encoder_close(&e1);
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* stream group                                                        */
/* ------------------------------------------------------------------ */

static void extract_bits(const uint8_t *src, uint32_t start, uint32_t nbits,
                         uint8_t *dst)
{
    uint32_t i;
    memset(dst, 0, (nbits + 7) / 8);
    for (i = 0; i < nbits; i++) {
        uint32_t sp = start + i;
        unsigned bit = (src[sp >> 3] >> (7 - (sp & 7))) & 1u;
        if (bit)
            dst[i >> 3] |= (uint8_t)(1u << (7 - (i & 7)));
    }
}

/* Per-frame identity. The independent MSB parser reads ADTS, the in-band PCE
 * and each element header straight from the wire; the handle's committed
 * observation ring entry for the same frame must point at exactly those
 * fields. frame_index is this frame's zero-based production index. */
static void check_frame_identity(faacEncStruct *h, const uint8_t *frame,
                                 uint32_t len, int adts, unsigned nexpect,
                                 unsigned frame_index)
{
    br_t r;
    pce_desc_t d;
    unsigned i, base;
    unsigned slot = frame_index % OBS_RING;

    br_init(&r, frame, len);
    if (adts) {
        adts_desc_t a;
        parse_adts(&r, &a);
        task_check(a.frame_length == len, "frame length field exact");
        base = 56;
    } else {
        base = 0;
    }

    task_check(h->obsRingCount > frame_index, "observation ring has this frame");
    task_check(h->obsRing[slot].valid, "ring entry valid");
    task_check(h->obsRing[slot].nElems == (int)nexpect, "ring element count");
    task_check((uint32_t)h->obsRing[slot].pceStart == base, "observed PCE start");

    task_check(br_get(&r, 3) == ID_PCE, "frame begins with PCE");
    parse_pce(&r, 0, &d);
    task_check((r.bit & 7u) == 0, "PCE ends on a byte boundary");
    task_check((uint32_t)h->obsRing[slot].pceLen == r.bit - base,
               "observed PCE length matches the parser");
    /* Element headers are separated by their coded bodies, so each is located
     * through its observed start offset rather than read consecutively. */
    task_check((uint32_t)h->obsRing[slot].elemStart[0] == r.bit,
               "the first element starts exactly where the PCE ends");

    for (i = 0; i < nexpect; i++) {
        uint32_t at = (uint32_t)h->obsRing[slot].elemStart[i];
        unsigned id = br_peek_at(frame, at, 3);
        unsigned tag = br_peek_at(frame, at + 3, 4);
        task_check(id == (uint32_t)h->obsRing[slot].elemType[i],
                   "parser and observer agree on element type");
        task_check(tag == (uint32_t)h->obsRing[slot].elemTag[i],
                   "parser and observer agree on element tag");
        task_check(id == (uint32_t)h->elements[i].type, "element type matches plan");
        task_check(tag == h->elements[i].tag, "element tag matches plan");
        if (i > 0)
            task_check(at > (uint32_t)h->obsRing[slot].elemStart[i - 1],
                       "elements are laid out in plan order");
        task_check((int)(at + h->obsRing[slot].elemLen[i]) <= (int)(len * 8),
                   "element stays inside the frame");
    }
    task_check(br_peek_at(frame, (uint32_t)h->obsRing[slot].endBit, 3) == ID_END,
               "raw_data_block terminates with ID_END");
    task_check((uint32_t)((h->obsRing[slot].endBit + 3 + 7) / 8) <= len,
               "frame pads to its declared length");
}

static int test_stream(void)
{
    /* Main 7-channel ADTS run: identity, pairing pointers, LFE-less plan,
     * flush coverage and monotonic frame sequence. */
    {
        faac_params p = base_params(48000, PLAN7_CH, FAAC_STREAM_ADTS);
        faac_encoder *e;
        faacEncStruct *h;
        buf_t all;
        buf_t frames[64];
        unsigned nframes = 0, i;

        p.pce_elements = plan7; p.pce_element_count = PLAN7_COUNT;
        e = must_open(&p);
        h = (faacEncStruct *)e;

        all.data = (uint8_t *)malloc(64u * 1024);
        all.cap = 64u * 1024;
        all.len = 0;
        task_check(all.data != NULL, "alloc");
        task_check(collect_frames(e, PLAN7_CH, 8, &all, &nframes), "collect");
        task_check(split_adts(&all, frames, 64, &nframes), "split");

        {
            unsigned produced = h->obsRingCount;
            task_check(produced == nframes, "one ring entry per produced frame");
            for (i = 0; i < nframes; i++)
                check_frame_identity(h, frames[i].data, frames[i].len, 1,
                                     PLAN7_COUNT, i);
        }

        /* Joint stereo really follows the declared pairs. */
        for (i = 0; i < PLAN7_COUNT; i++) {
            AACElement *el = &h->elements[i];
            if (el->type == ID_CPE) {
                int l = el->channels[0], rr = el->channels[1];
                task_check(h->coderInfo[l].partner == &h->coderInfo[rr],
                           "CPE partner wiring follows the declared slots");
                task_check(h->coderInfo[rr].partner == NULL,
                           "the right slot is not itself a pair owner");
                task_check(h->coderInfo[l].groups.n == h->coderInfo[rr].groups.n,
                           "paired channels share window groups");
            } else {
                task_check(h->coderInfo[el->channels[0]].partner == NULL,
                           "SCE channels are unpaired");
            }
        }
        free(all.data);
        faac_encoder_close(&e);
    }

    /* LFE bandwidth is applied to the declared LFE slot, wherever it sits. */
    {
        faac_pce_element list[] = {
            elem(FAAC_PCE_REGION_FRONT, 1, 0, 1),
            elem(FAAC_PCE_REGION_LFE,   0, 2, UINT32_MAX),
            elem(FAAC_PCE_REGION_BACK,  1, 3, 4),
        };
        faac_params p = base_params(48000, 5, FAAC_STREAM_ADTS);
        faac_encoder *e;
        faacEncStruct *h;
        buf_t all;
        buf_t frames[64];
        unsigned nframes = 0;

        p.pce_elements = list; p.pce_element_count = 3;
        e = must_open(&p);
        h = (faacEncStruct *)e;
        all.data = (uint8_t *)malloc(64u * 1024);
        all.cap = 64u * 1024;
        all.len = 0;
        task_check(collect_frames(e, 5, 6, &all, &nframes) && nframes > 0, "lfe encode");
        task_check(split_adts(&all, frames, 64, &nframes), "lfe split");
        /* Re-check the last produced frame's coding state directly. */
        task_check(h->coderInfo[2].sfbn == 3, "LFE slot bandwidth limited to 3 SFB");
        task_check(h->coderInfo[0].sfbn > 3, "full-band channel not limited");
        task_check(h->isLfeChannel[2], "LFE map points at the declared slot");
        free(all.data);
        faac_encoder_close(&e);
    }

    /* Parity with the legacy layout for an equivalent element plan: every
     * element's coded bit payload, block type, window groups and LFE bandwidth
     * match the legacy reference; only the PCE transport wrapper differs. All
     * channels share one waveform so the per-element block decision is the same
     * as the legacy global decision. */
    {
        enum { EQ_CH = 6, EQ_ELEMS = 4 };
        faac_pce_element list[] = {
            elem(FAAC_PCE_REGION_FRONT, 0, 0, UINT32_MAX), /* SCE 0 */
            elem(FAAC_PCE_REGION_FRONT, 1, 1, 2),          /* CPE (1,2) */
            elem(FAAC_PCE_REGION_FRONT, 1, 3, 4),          /* CPE (3,4) */
            elem(FAAC_PCE_REGION_LFE,   0, 5, UINT32_MAX),/* LFE 5 */
        };
        faac_params cp = base_params(48000, EQ_CH, FAAC_STREAM_ADTS);
        faac_params lp = base_params(48000, EQ_CH, FAAC_STREAM_ADTS);
        faac_encoder *ce, *le;
        faacEncStruct *ch, *lh;
        faac_encoder_info info;
        int16_t *in;
        uint8_t *out_c, *out_l;
        unsigned f, eidx;

        cp.pce_elements = list; cp.pce_element_count = EQ_ELEMS;
        lp.use_lfe = true;                  /* legacy: SCE, CPE, CPE, LFE */
        ce = must_open(&cp);
        le = must_open(&lp);
        ch = (faacEncStruct *)ce;
        lh = (faacEncStruct *)le;

        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        task_check(faac_encoder_get_info(ce, &info) == FAAC_OK, "info");
        in = (int16_t *)malloc(info.frame_samples * EQ_CH * sizeof(int16_t));
        out_c = (uint8_t *)malloc(info.max_output_bytes);
        out_l = (uint8_t *)malloc(info.max_output_bytes);
        task_check(in && out_c && out_l, "alloc");

        for (f = 0; f < 8; f++) {
            uint32_t nc = 0, nl = 0, k;
            uint32_t i;
            for (i = 0; i < info.frame_samples; i++) {
                int16_t v = (int16_t)(((i * 211u + f * 977u) % 12000u) - 6000);
                for (k = 0; k < EQ_CH; k++)
                    in[i * EQ_CH + k] = v;
            }
            task_check(faac_encoder_encode(ce, in, info.frame_samples * EQ_CH,
                                           out_c, info.max_output_bytes, &nc) == FAAC_OK,
                       "custom frame");
            task_check(faac_encoder_encode(le, in, info.frame_samples * EQ_CH,
                                           out_l, info.max_output_bytes, &nl) == FAAC_OK,
                       "legacy frame");
            if (!nc || !nl)
                continue;

            task_check(nc > nl, "custom frame is larger by its in-band PCE");
            task_check(ch->numElements == lh->numElements, "same element count");
            for (eidx = 0; eidx < EQ_ELEMS; eidx++) {
                AACElement *ae = &ch->elements[eidx];
                AACElement *be = &lh->elements[eidx];
                uint8_t a[2048], b[2048];
                unsigned abits = (unsigned)ch->obsElemLen[eidx];
                unsigned bbits = (unsigned)lh->obsElemLen[eidx];
                task_check(ae->type == be->type && ae->tag == be->tag
                           && ae->channels[0] == be->channels[0]
                           && ae->channels[1] == be->channels[1],
                           "equivalent plan keeps element identity");
                task_check(abits == bbits, "element coded length identical");
                extract_bits(out_c, (uint32_t)ch->obsElemStart[eidx], abits, a);
                extract_bits(out_l, (uint32_t)lh->obsElemStart[eidx], bbits, b);
                task_check(memcmp(a, b, (abits + 7) / 8) == 0,
                           "element payload bit-identical to the legacy reference");

                /* Read-only coding-state observation: block type and groups. */
                task_check(ch->coderInfo[ae->channels[0]].block_type
                           == lh->coderInfo[be->channels[0]].block_type,
                           "block type follows the same element slots");
                task_check(ch->coderInfo[ae->channels[0]].groups.n
                           == lh->coderInfo[be->channels[0]].groups.n,
                           "short-window grouping matches the reference");
            }
            task_check(ch->coderInfo[5].sfbn == lh->coderInfo[5].sfbn &&
                       ch->coderInfo[5].sfbn == 3,
                       "LFE bandwidth identical and limited");
        }
        free(in);
        free(out_c);
        free(out_l);
        faac_encoder_close(&ce);
        faac_encoder_close(&le);
    }

    /* channel_map permutation: slots are named in post-map space. */
    {
        enum { M_CH = 4 };
        int32_t map[M_CH] = { 3, 2, 1, 0 };
        faac_pce_element list[] = {
            elem(FAAC_PCE_REGION_FRONT, 1, 0, 3),    /* non-adjacent in slot space */
            elem(FAAC_PCE_REGION_BACK,  1, 1, 2),
        };
        faac_params p = base_params(48000, M_CH, FAAC_STREAM_RAW);
        faac_encoder *e;
        faacEncStruct *h;
        faac_encoder_info info;
        int16_t *in;
        uint8_t *out;
        raw_frame_t raw[16];
        unsigned nr = 0, f;

        p.channel_map = map; p.channel_map_count = M_CH;
        p.pce_elements = list; p.pce_element_count = 2;
        e = must_open(&p);
        h = (faacEncStruct *)e;
        task_check(h->config.channel_map[0] == 3 && h->config.channel_map[3] == 0,
                   "channel_map indices keep their input-PCM units");
        task_check(h->elements[0].channels[0] == 0 && h->elements[0].channels[1] == 3,
                   "plan pairs post-map slots");

        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        task_check(faac_encoder_get_info(e, &info) == FAAC_OK, "info");
        in = (int16_t *)malloc(info.frame_samples * M_CH * sizeof(int16_t));
        out = (uint8_t *)malloc(info.max_output_bytes);
        task_check(in && out, "alloc");
        for (f = 0; f < 6; f++) {
            uint32_t n = 0, i;
            for (i = 0; i < info.frame_samples * M_CH; i++)
                in[i] = (int16_t)(((i * 173u + f * 31u) % 9000u) - 4500);
            task_check(faac_encoder_encode(e, in, info.frame_samples * M_CH,
                                           out, info.max_output_bytes, &n) == FAAC_OK,
                       "mapped encode");
            if (n && nr < 16) { memcpy(raw[nr].storage, out, n); raw[nr].len = n; nr++; }
        }
        task_check(nr > 0, "mapped frames produced");
        /* The declared pairs hold in coding state, keyed by post-map slots. */
        task_check(h->coderInfo[0].partner == &h->coderInfo[3],
                   "front pair wires post-map slots 0 and 3");
        task_check(h->coderInfo[1].partner == &h->coderInfo[2],
                   "back pair wires post-map slots 1 and 2");
        for (f = 0; f < nr; f++)
            task_check(br_peek_at(raw[f].storage, 0, 3) == ID_PCE,
                       "mapped stream still opens each frame with a PCE");
        free(in); free(out);
        faac_encoder_close(&e);
    }

    /* VBR and CBR both produce well-formed custom frames. */
    {
        raw_frame_t frames[64];
        unsigned nr = 0;
        faac_params vp = base_params(48000, PLAN7_CH, FAAC_STREAM_RAW);
        faac_encoder *ve;
        vp.pce_elements = plan7; vp.pce_element_count = PLAN7_COUNT;
        vp.rate_control = FAAC_RC_VBR;
        vp.bit_rate = 0;
        ve = must_open(&vp);
        task_check(collect_raw(ve, PLAN7_CH, 6, frames, 64, &nr) && nr > 0, "VBR run");
        faac_encoder_close(&ve);

        {
            buf_t all;
            buf_t af[64];
            unsigned nf = 0;
            faac_params cp = base_params(48000, PLAN7_CH, FAAC_STREAM_ADTS);
            faac_encoder *ce;
            cp.pce_elements = plan7; cp.pce_element_count = PLAN7_COUNT;
            cp.rate_control = FAAC_RC_CBR;
            cp.bit_rate = 48000;
            ce = must_open(&cp);
            all.data = (uint8_t *)malloc(64u * 1024);
            all.cap = 64u * 1024;
            all.len = 0;
            task_check(all.data != NULL, "alloc");
            task_check(collect_frames(ce, PLAN7_CH, 6, &all, &nf), "CBR run");
            task_check(split_adts(&all, af, 64, &nf), "CBR split");
            {
                br_t r;
                adts_desc_t a;
                br_init(&r, af[nf - 1].data, af[nf - 1].len);
                parse_adts(&r, &a);
                task_check(a.fullness != 0x7FF, "CBR declares reservoir fullness");
                task_check(a.frame_length == af[nf - 1].len, "CBR length exact");
            }
            free(all.data);
            faac_encoder_close(&ce);
        }
    }

    /* Two instances with different plans, driven interleaved, stay isolated. */
    {
        faac_pce_element three[] = {
            elem(FAAC_PCE_REGION_FRONT, 0, 0, UINT32_MAX),
            elem(FAAC_PCE_REGION_SIDE,  1, 1, 2),
        };
        faac_params p7 = base_params(48000, PLAN7_CH, FAAC_STREAM_RAW);
        faac_params p3 = base_params(48000, 3, FAAC_STREAM_RAW);
        faac_encoder *e7, *e3;
        faacEncStruct *h7, *h3;
        raw_frame_t f7[64], f3[64];
        unsigned n7 = 0, n3 = 0, f;
        int16_t *in7, *in3;
        uint8_t *out;
        faac_encoder_info i7;

        p7.pce_elements = plan7; p7.pce_element_count = PLAN7_COUNT;
        p3.pce_elements = three; p3.pce_element_count = 2;
        e7 = must_open(&p7);
        e3 = must_open(&p3);
        h7 = (faacEncStruct *)e7;
        h3 = (faacEncStruct *)e3;

        memset(&i7, 0, sizeof(i7));
        i7.struct_size = sizeof(i7);
        task_check(faac_encoder_get_info(e7, &i7) == FAAC_OK, "info");
        in7 = (int16_t *)malloc(i7.frame_samples * PLAN7_CH * sizeof(int16_t));
        in3 = (int16_t *)malloc(i7.frame_samples * 3 * sizeof(int16_t));
        out = (uint8_t *)malloc(i7.max_output_bytes);
        task_check(in7 && in3 && out, "alloc");

        for (f = 0; f < 8; f++) {
            uint32_t w = 0;
            fill_pcm(in7, i7.frame_samples, PLAN7_CH, f, 0);
            fill_pcm(in3, i7.frame_samples, 3, f, 1);
            task_check(faac_encoder_encode(e7, in7, i7.frame_samples * PLAN7_CH,
                                           out, i7.max_output_bytes, &w) == FAAC_OK, "7ch");
            if (w && n7 < 64) { memcpy(f7[n7].storage, out, w); f7[n7].len = w; n7++; }
            w = 0;
            task_check(faac_encoder_encode(e3, in3, i7.frame_samples * 3,
                                           out, i7.max_output_bytes, &w) == FAAC_OK, "3ch");
            if (w && n3 < 64) { memcpy(f3[n3].storage, out, w); f3[n3].len = w; n3++; }
        }
        task_check(n7 > 0 && n3 > 0, "both instances emitted frames");
        for (f = 0; f < n7; f++) {
            br_t r;
            pce_desc_t d;
            br_init(&r, f7[f].storage, f7[f].len);
            task_check(br_get(&r, 3) == ID_PCE, "7ch PCE");
            parse_pce(&r, 0, &d);
            task_check(d.n_front == 2 && d.n_side == 1 && d.n_back == 2, "7ch plan");
            task_check(h7->obsElemStart[0] != h3->obsElemStart[0]
                       || h7->numElements != h3->numElements,
                       "handles keep distinct element plans");
        }
        for (f = 0; f < n3; f++) {
            br_t r;
            pce_desc_t d;
            br_init(&r, f3[f].storage, f3[f].len);
            task_check(br_get(&r, 3) == ID_PCE, "3ch PCE");
            parse_pce(&r, 0, &d);
            task_check(d.n_front == 1 && d.n_side == 1 && d.n_back == 0, "3ch plan");
            task_check(d.front_cpe[0] == 0 && d.side_cpe[0] == 1, "3ch element kinds");
        }
        free(in7); free(in3); free(out);
        faac_encoder_close(&e7);
        faac_encoder_close(&e3);
    }

    /* Default 2-channel path still emits a CPE with no PCE. */
    {
        raw_frame_t frames[16];
        unsigned nr = 0;
        faac_params p = base_params(48000, 2, FAAC_STREAM_RAW);
        faac_encoder *e = must_open(&p);
        faacEncStruct *h = (faacEncStruct *)e;
        task_check(collect_raw(e, 2, 6, frames, 16, &nr) && nr > 0, "default run");
        task_check(br_peek_at(frames[0].storage, 0, 3) == ID_CPE,
                   "default frame opens with a CPE, not a PCE");
        task_check(!h->customPce, "default handle is not in custom mode");
        faac_encoder_close(&e);
    }

    return 1;
}

int task_cases(const char *group)
{
    if (strcmp(group, "layout") == 0)
        return test_layout();
    if (strcmp(group, "config") == 0)
        return test_config();
    if (strcmp(group, "stream") == 0)
        return test_stream();
    return 0;
}
