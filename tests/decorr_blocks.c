////////////////////////////////////////////////////////////////////////////
//                       **** WAVPACK ****                                 //
//   Block-level regression for decorrelation using planned bitstreams     //
//                                                                         //
//  tests/decorr_blocks.c                                                  //
//                                                                         //
//  Unlike tests/decorr_reference.c, which drives the model directly, this  //
//  program builds real WavPack blocks by hand, writes the decorrelation   //
//  metadata itself (terms in a chosen declaration order, initial weights, //
//  initial log-quantized history and a residual plan), reuses only the     //
//  library's entropy writer for the residual bitstream, and decodes the    //
//  result through WavpackOpenFileInputEx64 + WavpackUnpackSamples. The      //
//  expected PCM is computed by the independent natural-time model, and the  //
//  block CRC is computed from that same expected PCM, so a CRC complaint    //
//  cannot be mistaken for a modelling error.                              //
//                                                                         //
//  It requires the observation-enabled library build, whose read-only      //
//  hooks report the entropy residuals entering each pass and the normalized //
//  state each pass leaves behind. The model's per-pass state is compared   //
//  against those hooks and against metadata re-parsed with dm_plan_from_    //
//  metadata() (never against the decoder's restored structs).              //
////////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "wavpack.h"
#include "decorr_model.h"

// From the internal header (observation hooks only exist in the special build).
extern void WavpackSetDecorrObserver (
    void (*residual_obs)(void *, int, int, int, int32_t, int, int32_t),
    void (*state_obs)(void *, int, int, int32_t, int, const char *, int64_t, int64_t),
    void *ctx);

static int failures;
static long comparisons;
static int term_hits [64];
static long chain_frames_compared;

#define TERM_INDEX(t) ((t) + 8)

static int g_saw_wide_source;

#define FAILF(...) do { \
    fprintf (stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf (stderr, __VA_ARGS__); \
    fputc ('\n', stderr); \
    failures++; \
} while (0)

// ---------------------------------------------------------------------------
// Byte buffer helpers
// ---------------------------------------------------------------------------

typedef struct { unsigned char *data; size_t len, cap; } Buffer;

static void buf_init (Buffer *b) { b->data = NULL; b->len = b->cap = 0; }

static void buf_reserve (Buffer *b, size_t extra)
{
    if (b->len + extra > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;

        while (ncap < b->len + extra)
            ncap *= 2;

        b->data = realloc (b->data, ncap);
        b->cap = ncap;
    }
}

static void buf_u8 (Buffer *b, unsigned v) { buf_reserve (b, 1); b->data [b->len++] = (unsigned char) v; }

static void buf_bytes (Buffer *b, const void *p, size_t n)
{
    buf_reserve (b, n);
    memcpy (b->data + b->len, p, n);
    b->len += n;
}

static void buf_free (Buffer *b) { free (b->data); b->data = NULL; b->len = b->cap = 0; }

// ---------------------------------------------------------------------------
// Planned block assembly
// ---------------------------------------------------------------------------

typedef struct {
    unsigned char terms [DM_MAX_TERMS];
    int terms_len;
    unsigned char weights [DM_MAX_TERMS * 2];
    int weights_len;
    unsigned char samples [DM_MAX_TERMS * DM_MAX_DELAY * 4];
    int samples_len;
} Meta;

// Encode the decorrelation metadata. We emit declaration order for terms and
// weights and the format's per-term history layout: 1..8 oldest-first, 17/18
// newest-first, negative one saved value per channel. Only the first declared
// term's history is written, exercising the "trailing history reads as zero"
// fill rule.
static void build_meta (Meta *m, const DmPlan *plan, int mono)
{
    int i, n = plan->num_terms, doff = 0;

    memset (m, 0, sizeof (*m));
    m->terms_len = n;

    for (i = 0; i < n; i++)
        m->terms [i] = (unsigned char) (((plan->term [i] + 5) & 0x1f) | ((plan->delta [i] & 0x7) << 5));

    m->weights_len = mono ? n : n * 2;

    for (i = 0; i < n; i++) {
        m->weights [mono ? i : i * 2] = (unsigned char) dm_store_weight ((int) plan->weight_A [i]);

        if (!mono)
            m->weights [i * 2 + 1] = (unsigned char) dm_store_weight ((int) plan->weight_B [i]);
    }

    for (i = 0; i < n; i++) {
        int term = plan->term [i];

        if (i > 0)
            break;              // trailing histories omitted -> zero fill

        if (term > 8) {
            int16_t l0 = dm_log2s ((int32_t) plan->hist_A [i][0]);
            int16_t l1 = dm_log2s ((int32_t) plan->hist_A [i][1]);
            m->samples [doff++] = (unsigned char) l0; m->samples [doff++] = (unsigned char) (l0 >> 8);
            m->samples [doff++] = (unsigned char) l1; m->samples [doff++] = (unsigned char) (l1 >> 8);

            if (!mono) {
                int16_t b0 = dm_log2s ((int32_t) plan->hist_B [i][0]);
                int16_t b1 = dm_log2s ((int32_t) plan->hist_B [i][1]);
                m->samples [doff++] = (unsigned char) b0; m->samples [doff++] = (unsigned char) (b0 >> 8);
                m->samples [doff++] = (unsigned char) b1; m->samples [doff++] = (unsigned char) (b1 >> 8);
            }
        }
        else if (term < 0) {
            int16_t la = dm_log2s ((int32_t) plan->hist_A [i][0]);
            int16_t lb = dm_log2s ((int32_t) plan->hist_B [i][0]);
            m->samples [doff++] = (unsigned char) la; m->samples [doff++] = (unsigned char) (la >> 8);
            m->samples [doff++] = (unsigned char) lb; m->samples [doff++] = (unsigned char) (lb >> 8);
        }
        else {
            for (int k = 0; k < term; k++) {
                int16_t la = dm_log2s ((int32_t) plan->hist_A [i][k]);
                m->samples [doff++] = (unsigned char) la; m->samples [doff++] = (unsigned char) (la >> 8);

                if (!mono) {
                    int16_t lb = dm_log2s ((int32_t) plan->hist_B [i][k]);
                    m->samples [doff++] = (unsigned char) lb; m->samples [doff++] = (unsigned char) (lb >> 8);
                }
            }
        }
    }

    m->samples_len = doff;
}

static void add_meta_item (Buffer *blk, unsigned char id, const unsigned char *data, int len)
{
    unsigned char w [3];
    int odd = len & 1;

    w [0] = (unsigned char) ((len + 1) >> 1);
    w [1] = (unsigned char) ((len + 1) >> 9);
    w [2] = (unsigned char) ((len + 1) >> 17);

    if (odd)
        id |= 0x40;

    if (w [1] || w [2])
        id |= 0x80;

    buf_u8 (blk, id);
    buf_u8 (blk, w [0]);

    if (id & 0x80) {
        buf_u8 (blk, w [1]);
        buf_u8 (blk, w [2]);
    }

    if (len) {
        buf_bytes (blk, data, len);

        if (odd)
            buf_u8 (blk, 0);
    }
}

static uint32_t block_crc (const int32_t *pcm, int frames, int mono)
{
    uint32_t crc = 0xffffffff;

    if (mono)
        for (int i = 0; i < frames; i++)
            crc = crc * 3 + (uint32_t) pcm [i];
    else
        for (int i = 0; i < frames; i++) {
            uint32_t l = (uint32_t) pcm [i * 2], r = (uint32_t) pcm [i * 2 + 1];
            crc += (crc << 3) + (l << 1) + l + r;
        }

    return crc;
}

static void emit_block (Buffer *file, const Meta *meta, const int32_t *pcm, int frames,
    int mono, int bits, uint32_t total_samples, uint32_t block_index,
    const unsigned char *bitstream, int bitstream_len, int final, int sample_rate)
{
    Buffer body;
    unsigned char hdr [32];
    uint32_t flags, crc;

    buf_init (&body);

    if (meta->terms_len)
        add_meta_item (&body, 0x2, meta->terms, meta->terms_len);

    if (meta->weights_len)
        add_meta_item (&body, 0x3, meta->weights, meta->weights_len);

    if (meta->samples_len)
        add_meta_item (&body, 0x4, meta->samples, meta->samples_len);

    // Explicit sample rate in the first block only.
    if (block_index == 0) {
        unsigned char sr [3] = { sample_rate & 0xff, (sample_rate >> 8) & 0xff,
            (sample_rate >> 16) & 0xff };
        add_meta_item (&body, 0x27, sr, 3);
    }

    add_meta_item (&body, 0xa, bitstream, bitstream_len);   // ID_WV_BITSTREAM

    flags = (bits == 16 ? 1u : 2u);                         // BYTES_STORED (bytes-1)
    if (mono)
        flags |= 0x4;                                       // MONO_FLAG

    flags |= ((uint32_t) (bits - 1)) << 18;                 // MAG
    flags |= ((uint32_t) 10) << 23;                         // SRATE index (48000)

    // A single mono/stereo stream is a one-block multichannel set: each block
    // is both the initial and the final block of its set.
    flags |= 0x800;                                         // INITIAL_BLOCK
    flags |= 0x1000;                                        // FINAL_BLOCK
    (void) final;

    crc = block_crc (pcm, frames, mono);

    memset (hdr, 0, sizeof (hdr));
    memcpy (hdr, "wvpk", 4);
    {
        uint32_t ckSize = (uint32_t) (24 + body.len);
        hdr [4] = ckSize & 0xff; hdr [5] = (ckSize >> 8) & 0xff;
        hdr [6] = (ckSize >> 16) & 0xff; hdr [7] = (ckSize >> 24) & 0xff;
    }
    hdr [8] = 0x07; hdr [9] = 0x04;                         // version 0x407
    hdr [12] = total_samples & 0xff; hdr [13] = (total_samples >> 8) & 0xff;
    hdr [14] = (total_samples >> 16) & 0xff; hdr [15] = (total_samples >> 24) & 0xff;
    hdr [16] = block_index & 0xff; hdr [17] = (block_index >> 8) & 0xff;
    hdr [18] = (block_index >> 16) & 0xff; hdr [19] = (block_index >> 24) & 0xff;
    hdr [20] = frames & 0xff; hdr [21] = (frames >> 8) & 0xff;
    hdr [22] = (frames >> 16) & 0xff; hdr [23] = (frames >> 24) & 0xff;
    hdr [24] = flags & 0xff; hdr [25] = (flags >> 8) & 0xff;
    hdr [26] = (flags >> 16) & 0xff; hdr [27] = (flags >> 24) & 0xff;
    hdr [28] = crc & 0xff; hdr [29] = (crc >> 8) & 0xff;
    hdr [30] = (crc >> 16) & 0xff; hdr [31] = (crc >> 24) & 0xff;

    buf_bytes (file, hdr, 32);
    buf_bytes (file, body.data, body.len);
    buf_free (&body);
}

// ---------------------------------------------------------------------------
// Library-backed entropy writer (shim), plus declared entropy state readback
// ---------------------------------------------------------------------------

int shim_encode_residuals (int mono, int bits, const int32_t *residual, int frames,
    unsigned char *out, int out_cap, int *out_len);

// ---------------------------------------------------------------------------
// Chain spec and plan construction
// ---------------------------------------------------------------------------

typedef struct {
    int terms [DM_MAX_TERMS];
    int deltas [DM_MAX_TERMS];
    int weights_A [DM_MAX_TERMS], weights_B [DM_MAX_TERMS];
    int nterms;
} ChainSpec;

static void plan_from_spec (DmPlan *plan, const ChainSpec *cs, int mono)
{
    memset (plan, 0, sizeof (*plan));
    plan->num_terms = cs->nterms;
    plan->mono = mono;

    for (int i = 0; i < cs->nterms; i++) {
        int term = cs->terms [i];

        plan->term [i] = term;
        plan->delta [i] = cs->deltas [i];
        plan->weight_A [i] = cs->weights_A [i];
        plan->weight_B [i] = cs->weights_B [i];
        plan->exec_order [i] = cs->nterms - 1 - i;

        if (term > 8) {
            plan->hist_A [i][0] = 700 + 13 * i;
            plan->hist_A [i][1] = 350 - 7 * i;
            plan->hist_B [i][0] = -650 - 11 * i;
            plan->hist_B [i][1] = -300 + 5 * i;
        }
        else if (term < 0) {
            plan->hist_A [i][0] = 123 + 17 * i;
            plan->hist_B [i][0] = -321 - 19 * i;
        }
        else {
            for (int k = 0; k < term; k++) {
                plan->hist_A [i][k] = 40 + 3 * k + 11 * i;
                plan->hist_B [i][k] = -40 - 3 * k - 11 * i;
            }
        }
    }
}

// Choose bounded PCM for a block. Small asymmetric drive plus a couple of
// larger excursions gives every pass a non-trivial source while keeping the
// whole chain well inside int32.
static void build_target_pcm (int frames, int mono, int32_t *pcm, int amp)
{
    int nchan = mono ? 1 : 2;

    for (int f = 0; f < frames; f++)
        for (int c = 0; c < nchan; c++) {
            int v = ((f * 37 + c * 19 + 5) % (2 * amp + 1)) - amp;

            if ((f % 11) == 0)
                v = 0;                          // zero residual region
            else if ((f % 13) == 0)
                v = -((amp + 1) / 2);           // negative half-step region

            pcm [f * nchan + c] = v;
        }

    // A couple of larger excursions (bounded by the caller's amplitude).
    pcm [3 * nchan] = amp + amp / 2;
    pcm [5 * nchan] = -(amp + amp / 2);
}

// ---------------------------------------------------------------------------
// Public decoder over an in-memory image
// ---------------------------------------------------------------------------

typedef struct {
    const unsigned char *data;
    size_t len, pos;
    WavpackStreamReader64 reader;   // must outlive the context (library keeps this pointer)
} MemReader;

static int32_t mr_read (void *id, void *out, int32_t bcount)
{
    MemReader *m = id;
    size_t avail = m->len - m->pos, n = (size_t) bcount < avail ? (size_t) bcount : avail;

    if (!n) return 0;
    memcpy (out, m->data + m->pos, n);
    m->pos += n;
    return (int32_t) n;
}

static int64_t mr_tell (void *id) { return (int64_t) ((MemReader *) id)->pos; }
static int mr_seek_abs (void *id, int64_t pos)
{
    MemReader *m = id;
    if (pos < 0 || (size_t) pos > m->len) return -1;
    m->pos = (size_t) pos;
    return 0;
}
static int mr_seek_rel (void *id, int64_t d, int mode)
{
    MemReader *m = id;
    int64_t base = mode == SEEK_SET ? 0 : mode == SEEK_CUR ? (int64_t) m->pos : (int64_t) m->len;
    return mr_seek_abs (id, base + d);
}
static int mr_push_back (void *id, int c)
{
    MemReader *m = id;
    if (!m->pos) return -1;
    m->pos--;
    return m->data [m->pos] == c ? c : -1;
}
static int64_t mr_length (void *id) { return (int64_t) ((MemReader *) id)->len; }
static int mr_can_seek (void *id) { (void) id; return 1; }
static int mr_close (void *id) { (void) id; return 0; }

static WavpackContext *open_mem (const unsigned char *data, size_t len, MemReader *mr)
{
    char error [80];

    mr->data = data;
    mr->len = len;
    mr->pos = 0;
    memset (&mr->reader, 0, sizeof (mr->reader));
    mr->reader.read_bytes = mr_read;
    mr->reader.get_pos = mr_tell;
    mr->reader.set_pos_abs = mr_seek_abs;
    mr->reader.set_pos_rel = mr_seek_rel;
    mr->reader.push_back_byte = mr_push_back;
    mr->reader.get_length = mr_length;
    mr->reader.can_seek = mr_can_seek;
    mr->reader.close = mr_close;
    return WavpackOpenFileInputEx64 (&mr->reader, mr, NULL, error, 0, 0);
}

// ---------------------------------------------------------------------------
// Observer: capture residuals and end-of-pass state from the real decoder
// ---------------------------------------------------------------------------

#define MAX_OBS_RESIDUAL 200000

typedef struct {
    int32_t value;
    int32_t sample;
    int chan, pass, stream;
} ObsResidual;

typedef struct {
    int pass, chan, stream;
    int32_t sample;
    char field [16];
    int64_t value, aux;
} ObsState;

static ObsResidual obs_res [MAX_OBS_RESIDUAL];
static int obs_res_count;
static ObsState obs_st [MAX_OBS_RESIDUAL];
static int obs_st_count;

static void residual_obs (void *ctx, int stream, int pass, int pass_count, int32_t sample,
    int chan, int32_t residual)
{
    (void) ctx; (void) pass_count;

    if (obs_res_count < MAX_OBS_RESIDUAL) {
        obs_res [obs_res_count].value = residual;
        obs_res [obs_res_count].sample = sample;
        obs_res [obs_res_count].chan = chan;
        obs_res [obs_res_count].pass = pass;
        obs_res [obs_res_count].stream = stream;
        obs_res_count++;
    }
}

static void state_obs (void *ctx, int stream, int pass, int32_t sample, int chan,
    const char *field, int64_t value, int64_t aux)
{
    (void) ctx;

    if (obs_st_count < MAX_OBS_RESIDUAL) {
        ObsState *o = &obs_st [obs_st_count++];
        o->pass = pass;
        o->chan = chan;
        o->stream = stream;
        o->sample = sample;
        o->value = value;
        o->aux = aux;
        snprintf (o->field, sizeof (o->field), "%s", field);
    }
}

// ---------------------------------------------------------------------------
// Built scenario
// ---------------------------------------------------------------------------

typedef struct {
    Buffer image;
    int32_t *expect;
    int frames_total;
    int mono, bits, num_blocks, block_frames, amp;
    DmPlan plans [16];          // per-block *initial* plan (as parsed from metadata)
    Meta metas [16];
} Built;

// Build the whole image. Each block's metadata declares the model state at the
// start of that block; the model is re-seeded from a fresh parse of those same
// bytes so the decoder and model start each block from identical, metadata-
// recovered initial state.
static int build_scenario (const ChainSpec *cs, int mono, int bits, int block_frames,
    int num_blocks, int amp, const char *name, Built *b, int *saw_wide_source)
{
    int nchan = mono ? 1 : 2;
    int frames_total = block_frames * num_blocks;
    int32_t *res = malloc (sizeof (int32_t) * block_frames * nchan);
    int32_t *pcm = malloc (sizeof (int32_t) * block_frames * nchan);
    int32_t *target = malloc (sizeof (int32_t) * block_frames * nchan);
    unsigned char stream [1 << 20];
    DmPlan cur, parsed;
    DmState model;
    int stream_len, blk;

    memset (b, 0, sizeof (*b));
    buf_init (&b->image);
    b->expect = malloc (sizeof (int32_t) * frames_total * nchan);
    b->frames_total = frames_total;
    b->mono = mono;
    b->bits = bits;
    b->num_blocks = num_blocks;
    b->block_frames = block_frames;
    b->amp = amp;

    plan_from_spec (&cur, cs, mono);

    for (blk = 0; blk < num_blocks; blk++) {
        Meta meta;

        build_meta (&meta, &cur, mono);
        b->metas [blk] = meta;

        // The decoder parses this metadata; the model must start from the same
        // declared bytes, not from the previous state directly.
        if (dm_plan_from_metadata (&parsed, mono, meta.terms, meta.terms_len,
            meta.weights, meta.weights_len, meta.samples, meta.samples_len) != 0) {
                FAILF ("%s block %d: model could not parse its own metadata", name, blk);
                return -1;
        }

        b->plans [blk] = parsed;
        dm_init (&model, &parsed);

        build_target_pcm (block_frames, mono, target, amp);

        // Forward-encode the chosen PCM through the model to get the residual
        // plan the decoder will apply. This keeps the decode bounded.
        if (dm_encode_block (&model, target, res, block_frames) != 0) {
            FAILF ("%s block %d: forward model left int32; test is mis-sized", name, blk);
            return -1;
        }

        if (shim_encode_residuals (mono, bits, res, block_frames, stream, sizeof (stream), &stream_len)) {
            FAILF ("%s block %d: entropy encode failed", name, blk);
            return -1;
        }

        // Decode from a *fresh* parse of the same metadata; this is what the
        // library will do, and it is independent of the encode-side state.
        {
            DmState dec;

            dm_init (&dec, &parsed);
            dm_decode_block (&dec, res, pcm, block_frames);

            if (dec.overflow) {
                FAILF ("%s block %d: decode intermediate left int32", name, blk);
                return -1;
            }

            // Round-trip must reproduce the target PCM exactly (this catches a
            // model asymmetry before we ever involve the library).
            for (int i = 0; i < block_frames * nchan; i++) {
                comparisons++;

                if (pcm [i] != target [i]) {
                    FAILF ("%s block %d: model round-trip mismatch at sample %d (%d vs %d)",
                        name, blk, i, pcm [i], target [i]);
                    return -1;
                }
            }

            if (dec.max_abs_source > 32767)
                *saw_wide_source = 1;
        }

        memcpy (b->expect + (size_t) blk * block_frames * nchan, pcm,
            sizeof (int32_t) * block_frames * nchan);

        emit_block (&b->image, &meta, pcm, block_frames, mono, bits, frames_total,
            (uint32_t) (blk * block_frames), stream, stream_len, blk == num_blocks - 1, 48000);

        // Roll the planned metadata forward to the state this block ends with.
        dm_plan_from_state (&cur, &model);
    }

    free (res);
    free (pcm);
    free (target);
    return 0;
}

// ---------------------------------------------------------------------------
// Decode + compare, with observer cross-checks
// ---------------------------------------------------------------------------

typedef struct { const char *name; int block, pass, chan; int64_t sample; } ReportCtx;

static void report_first_deviation (const ReportCtx *rc, const char *field, int64_t got, int64_t want)
{
    fprintf (stderr, "first deviation: case=%s block=%d pass=%d ch=%d sample=%lld field=%s got=%lld want=%lld\n",
        rc->name, rc->block, rc->pass, rc->chan, (long long) rc->sample, field,
        (long long) got, (long long) want);
}

// Compare the model's state after running a block's residuals against the
// per-pass state captured from the real decoder. Both are in the format's
// normalized layout. Also check each pass's input residuals.
static int compare_observer (const Built *b, const char *label, int block)
{
    const DmPlan *plan = &b->plans [block];
    int nchan = b->mono ? 1 : 2;
    DmState model, enc;
    int32_t *pcm, *res, *target;
    int rc = 0;

    // Rebuild exactly as the builder did: seed from this block's parsed
    // metadata, choose the same target PCM, forward-encode to get residuals,
    // then decode from a fresh parse.
    dm_init (&model, plan);
    dm_init (&enc, plan);
    res = malloc (sizeof (int32_t) * b->block_frames * nchan);
    pcm = malloc (sizeof (int32_t) * b->block_frames * nchan);
    target = malloc (sizeof (int32_t) * b->block_frames * nchan);

    build_target_pcm (b->block_frames, b->mono, target, b->amp);
    dm_encode_block (&enc, target, res, b->block_frames);
    dm_decode_block (&model, res, pcm, b->block_frames);

    // Check the residuals the real decoder saw, pass 0 only (input to decoder),
    // against ours. The observer reports sample indices offset by the block
    // index; we match by (sample - block_start).
    {
        int32_t base = block * b->block_frames;
        int found = 0;

        for (int i = 0; i < obs_res_count; i++) {
            if (obs_res [i].pass != 0 || obs_res [i].stream != 0)
                continue;

            if (obs_res [i].sample < base || obs_res [i].sample >= base + b->block_frames)
                continue;

            {
                int f = obs_res [i].sample - base;
                int c = obs_res [i].chan;

                comparisons++;

                if (obs_res [i].value != res [f * nchan + c]) {
                    ReportCtx r = { label, block, 0, c, obs_res [i].sample };
                    report_first_deviation (&r, "residual", obs_res [i].value,
                        res [f * nchan + c]);
                    FAILF ("%s block %d: entropy residual mismatch at frame %d ch %d", label,
                        block, f, c);
                    rc = -1;
                }

                found++;
            }
        }

        if (!found) {
            FAILF ("%s block %d: observer reported no pass-0 residuals", label, block);
            rc = -1;
        }
    }

    // Compare per-pass state after the block. The observer reports at sample
    // "last sample of the call"; for a whole-block single call that is the
    // block's last frame. Model state is likewise its end-of-block state.
    {
        int32_t last = block * b->block_frames + b->block_frames - 1;

        for (int i = 0; i < obs_st_count; i++) {
            ObsState *o = &obs_st [i];
            int decl = b->plans [block].num_terms - 1 - o->pass;   // exec -> decl
            DmPassView v;

            if (o->sample != last || o->stream != 0)
                continue;

            if (decl < 0 || decl >= b->plans [block].num_terms)
                continue;

            dm_pass_view (&model, decl, &v);

            if (!strcmp (o->field, "pass_weight_A")) {
                comparisons++;

                if (o->value != v.weight_A) {
                    ReportCtx r = { label, block, o->pass, 0, o->sample };
                    report_first_deviation (&r, "weight_A", o->value, v.weight_A);
                    FAILF ("%s block %d pass %d: weight_A mismatch", label, block, o->pass);
                    rc = -1;
                }
            }
            else if (!strcmp (o->field, "pass_weight_B")) {
                comparisons++;

                if (o->value != v.weight_B) {
                    ReportCtx r = { label, block, o->pass, 1, o->sample };
                    report_first_deviation (&r, "weight_B", o->value, v.weight_B);
                    FAILF ("%s block %d pass %d: weight_B mismatch", label, block, o->pass);
                    rc = -1;
                }
            }
            else if (!strcmp (o->field, "history_A")) {
                int k = (int) o->aux;
                int term = b->plans [block].term [decl];

                // For terms 1..8 only the first "term" slots are normalized
                // by the decoder; higher slots are stale padding, not history.
                if (term > 0 && term <= 8 && k >= term)
                    continue;

                comparisons++;

                if (o->value != v.hist_A [k]) {
                    ReportCtx r = { label, block, o->pass, 0, o->sample };
                    report_first_deviation (&r, "history_A", o->value, v.hist_A [k]);
                    FAILF ("%s block %d pass %d: history_A[%d] mismatch", label, block, o->pass, k);
                    rc = -1;
                }
            }
            else if (!strcmp (o->field, "history_B")) {
                int k = (int) o->aux;
                int term = b->plans [block].term [decl];

                if (term > 0 && term <= 8 && k >= term)
                    continue;

                comparisons++;

                if (o->value != v.hist_B [k]) {
                    ReportCtx r = { label, block, o->pass, 1, o->sample };
                    report_first_deviation (&r, "history_B", o->value, v.hist_B [k]);
                    FAILF ("%s block %d pass %d: history_B[%d] mismatch", label, block, o->pass, k);
                    rc = -1;
                }
            }
        }
    }

    free (res);
    free (pcm);
    free (target);
    return rc;
}

// Verify per-return history normalization for delayed terms. Decode one block
// in small chunks and, after each return, compare the model's natural-time
// state (which never rotates) against the normalized state the decoder leaves
// at that same return. This specifically exercises terms 1..8 crossing a
// chunk boundary mid-block.
static int check_per_return_normalization (const Built *b, const char *label, int block)
{
    MemReader mr;
    WavpackContext *wpc;
    const DmPlan *plan = &b->plans [block];
    int nchan = b->mono ? 1 : 2;
    int32_t *res, *target, *out;
    DmState model, enc;
    int rc = 0;
    int produced = 0;

    dm_init (&model, plan);
    dm_init (&enc, plan);
    res = malloc (sizeof (int32_t) * b->block_frames * nchan);
    target = malloc (sizeof (int32_t) * b->block_frames * nchan);

    build_target_pcm (b->block_frames, b->mono, target, b->amp);
    dm_encode_block (&enc, target, res, b->block_frames);

    obs_res_count = obs_st_count = 0;
    wpc = open_mem (b->image.data, b->image.len, &mr);

    if (!wpc) {
        FAILF ("%s: per-return open failed", label);
        free (res); free (target);
        return -1;
    }

    if (block)
        WavpackSeekSample64 (wpc, (int64_t) block * b->block_frames);

    out = malloc (sizeof (int32_t) * 7 * nchan);

    while (produced < b->block_frames) {
        int32_t want = b->block_frames - produced;
        uint32_t got;

        if (want > 7)
            want = 7;

        got = WavpackUnpackSamples (wpc, out, (uint32_t) want);

        if (!got)
            break;

        // Advance the model over exactly this chunk (natural-time, no rotation
        // performed by the model).
        {
            int32_t *scratch = malloc (sizeof (int32_t) * got * nchan);

            dm_decode_block (&model, res + (size_t) produced * nchan, scratch, (int) got);
            free (scratch);
        }

        produced += got;

        // After this return the decoder's normalized state is reported at the
        // last sample of this call.
        {
            int32_t last = block * b->block_frames + produced - 1;

            for (int i = 0; i < obs_st_count; i++) {
                ObsState *o = &obs_st [i];
                int decl = plan->num_terms - 1 - o->pass;
                DmPassView v;

                if (o->sample != last || o->stream != 0)
                    continue;

                if (decl < 0 || decl >= plan->num_terms)
                    continue;

                dm_pass_view (&model, decl, &v);

                if (!strcmp (o->field, "history_A")) {
                    int k = (int) o->aux;
                    int term = plan->term [decl];

                    if (term > 0 && term <= 8 && k >= term)
                        continue;       // stale padding slot

                    comparisons++;

                    if (o->value != v.hist_A [k]) {
                        ReportCtx r = { label, block, o->pass, 0, o->sample };
                        report_first_deviation (&r, "history_A", o->value, v.hist_A [k]);
                        FAILF ("%s block %d chunk-return pass %d: history_A[%d] mismatch",
                            label, block, o->pass, k);
                        rc = -1;
                    }
                }
                else if (!strcmp (o->field, "history_B")) {
                    int k = (int) o->aux;
                    int term = plan->term [decl];

                    if (term > 0 && term <= 8 && k >= term)
                        continue;

                    comparisons++;

                    if (o->value != v.hist_B [k]) {
                        ReportCtx r = { label, block, o->pass, 1, o->sample };
                        report_first_deviation (&r, "history_B", o->value, v.hist_B [k]);
                        FAILF ("%s block %d chunk-return pass %d: history_B[%d] mismatch",
                            label, block, o->pass, k);
                        rc = -1;
                    }
                }
            }
        }
    }

    comparisons++;

    if (produced != b->block_frames)
        FAILF ("%s block %d: chunked observer read got %d/%d frames", label, block,
            produced, b->block_frames);

    free (out);
    free (res);
    free (target);
    WavpackCloseFile (wpc);
    return rc;
}

// Decode the image in chunks and compare PCM value by value.
static int decode_and_compare (const Built *b, int chunk, const char *label)
{
    MemReader mr;
    WavpackContext *wpc;
    int32_t *out;
    int nchan = b->mono ? 1 : 2;
    int64_t total = 0;
    int rc = 0;

    obs_res_count = obs_st_count = 0;

    wpc = open_mem (b->image.data, b->image.len, &mr);

    if (!wpc) {
        FAILF ("%s: open failed", label);
        return -1;
    }

    out = malloc (sizeof (int32_t) * (size_t) chunk * nchan);

    while (1) {
        uint32_t got = WavpackUnpackSamples (wpc, out, chunk);

        if (!got)
            break;

        if (total + got > b->frames_total) {
            FAILF ("%s: decoder returned extra frames", label);
            rc = -1;
            break;
        }

        for (int f = 0; f < (int) got; f++)
            for (int c = 0; c < nchan; c++) {
                int64_t idx = (total + f) * nchan + c;
                comparisons++;

                if (out [f * nchan + c] != b->expect [idx]) {
                    ReportCtx r = { label, (int) ((total + f) / b->block_frames), -1, c, total + f };
                    report_first_deviation (&r, "pcm", out [f * nchan + c], b->expect [idx]);
                    FAILF ("%s: pcm mismatch at frame %lld ch %d", label, (long long) (total + f), c);
                    rc = -1;
                    goto done;
                }
            }

        total += got;
    }

    if (total != b->frames_total) {
        FAILF ("%s: decoded %lld of %d frames", label, (long long) total, b->frames_total);
        rc = -1;
    }

    if (WavpackGetNumErrors (wpc)) {
        FAILF ("%s: decoder reported %d errors", label, WavpackGetNumErrors (wpc));
        rc = -1;
    }

done:
    free (out);
    WavpackCloseFile (wpc);
    return rc;
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

static void scenario_mixed_stereo_16 (void)
{
    ChainSpec cs;
    Built b;
    int saw_wide = 0;

    memset (&cs, 0, sizeof (cs));
    cs.nterms = 6;
    cs.terms [0] = 2;  cs.deltas [0] = 2;  cs.weights_A [0] = 700;  cs.weights_B [0] = -700;
    cs.terms [1] = -1; cs.deltas [1] = 2;  cs.weights_A [1] = 800;  cs.weights_B [1] = -800;
    cs.terms [2] = 17; cs.deltas [2] = 2;  cs.weights_A [2] = 600;  cs.weights_B [2] = -600;
    cs.terms [3] = 3;  cs.deltas [3] = 2;  cs.weights_A [3] = 500;  cs.weights_B [3] = -500;
    cs.terms [4] = -3; cs.deltas [4] = 2;  cs.weights_A [4] = 900;  cs.weights_B [4] = -900;
    cs.terms [5] = 18; cs.deltas [5] = 2;  cs.weights_A [5] = 750;  cs.weights_B [5] = -750;

    if (build_scenario (&cs, 0, 16, 60, 3, 8000, "mixed-stereo-16", &b, &saw_wide) != 0)
        return;

    decode_and_compare (&b, 60, "mixed-stereo-16/whole");
    decode_and_compare (&b, 1, "mixed-stereo-16/1");
    decode_and_compare (&b, 7, "mixed-stereo-16/7");
    decode_and_compare (&b, 17, "mixed-stereo-16/17");

    for (int blk = 0; blk < b.num_blocks; blk++) {
        obs_res_count = obs_st_count = 0;
        {
            MemReader mr;
            WavpackContext *wpc = open_mem (b.image.data, b.image.len, &mr);
            int32_t *tmp = malloc (sizeof (int32_t) * 60 * 2);
            int64_t skip = (int64_t) blk * 60;

            if (wpc) {
                if (skip)
                    WavpackSeekSample64 (wpc, skip);

                WavpackUnpackSamples (wpc, tmp, 60);
                compare_observer (&b, "mixed-stereo-16", blk);
                WavpackCloseFile (wpc);
            }

            free (tmp);
        }

        check_per_return_normalization (&b, "mixed-stereo-16", blk);
    }

    for (int i = 0; i < cs.nterms; i++)
        term_hits [TERM_INDEX (cs.terms [i])] += b.frames_total * 2;

    chain_frames_compared += b.frames_total;
    g_saw_wide_source |= saw_wide;

    buf_free (&b.image);
    free (b.expect);
}

static void scenario_mono_24 (void)
{
    ChainSpec cs;
    Built b;
    int saw_wide = 0;

    memset (&cs, 0, sizeof (cs));
    // Cover every positive delay 1..8 plus a two-point extrapolation, in a
    // declaration order that is the reverse of decoding order.
    cs.nterms = 9;
    cs.terms [0] = 1;  cs.deltas [0] = 1;  cs.weights_A [0] = 400;
    cs.terms [1] = 2;  cs.deltas [1] = 2;  cs.weights_A [1] = 500;
    cs.terms [2] = 3;  cs.deltas [2] = 3;  cs.weights_A [2] = 650;
    cs.terms [3] = 4;  cs.deltas [3] = 2;  cs.weights_A [3] = 550;
    cs.terms [4] = 5;  cs.deltas [4] = 2;  cs.weights_A [4] = 600;
    cs.terms [5] = 6;  cs.deltas [5] = 1;  cs.weights_A [5] = 450;
    cs.terms [6] = 7;  cs.deltas [6] = 2;  cs.weights_A [6] = 700;
    cs.terms [7] = 8;  cs.deltas [7] = 2;  cs.weights_A [7] = 850;
    cs.terms [8] = 18; cs.deltas [8] = 2;  cs.weights_A [8] = 300;

    if (build_scenario (&cs, 1, 24, 45, 2, 200000, "mono-24", &b, &saw_wide) != 0)
        return;

    decode_and_compare (&b, 45, "mono-24/whole");
    decode_and_compare (&b, 1, "mono-24/1");
    decode_and_compare (&b, 7, "mono-24/7");
    decode_and_compare (&b, 17, "mono-24/17");

    for (int blk = 0; blk < b.num_blocks; blk++) {
        obs_res_count = obs_st_count = 0;
        {
            MemReader mr;
            WavpackContext *wpc = open_mem (b.image.data, b.image.len, &mr);
            int32_t *tmp = malloc (sizeof (int32_t) * 45);

            if (wpc) {
                if (blk)
                    WavpackSeekSample64 (wpc, (int64_t) blk * 45);

                WavpackUnpackSamples (wpc, tmp, 45);
                compare_observer (&b, "mono-24", blk);
                WavpackCloseFile (wpc);
            }

            free (tmp);
        }

        check_per_return_normalization (&b, "mono-24", blk);
    }

    for (int i = 0; i < cs.nterms; i++)
        term_hits [TERM_INDEX (cs.terms [i])] += b.frames_total;

    chain_frames_compared += b.frames_total;
    g_saw_wide_source |= saw_wide;

    buf_free (&b.image);
    free (b.expect);
}

static void scenario_negative_stereo_24 (void)
{
    ChainSpec cs;
    Built b;
    int saw_wide = 0;

    memset (&cs, 0, sizeof (cs));
    cs.nterms = 4;
    cs.terms [0] = -3; cs.deltas [0] = 3;  cs.weights_A [0] = 1000; cs.weights_B [0] = -1000;
    cs.terms [1] = -1; cs.deltas [1] = 3;  cs.weights_A [1] = -950; cs.weights_B [1] = 950;
    cs.terms [2] = -2; cs.deltas [2] = 2;  cs.weights_A [2] = 880;  cs.weights_B [2] = -880;
    cs.terms [3] = -3; cs.deltas [3] = 2;  cs.weights_A [3] = -700; cs.weights_B [3] = 700;

    if (build_scenario (&cs, 0, 24, 32, 4, 150000, "negative-stereo-24", &b, &saw_wide) != 0)
        return;

    decode_and_compare (&b, 32, "negative-stereo-24/whole");
    decode_and_compare (&b, 1, "negative-stereo-24/1");
    decode_and_compare (&b, 7, "negative-stereo-24/7");
    decode_and_compare (&b, 17, "negative-stereo-24/17");

    for (int blk = 0; blk < b.num_blocks; blk++) {
        obs_res_count = obs_st_count = 0;
        {
            MemReader mr;
            WavpackContext *wpc = open_mem (b.image.data, b.image.len, &mr);
            int32_t *tmp = malloc (sizeof (int32_t) * 32 * 2);

            if (wpc) {
                if (blk)
                    WavpackSeekSample64 (wpc, (int64_t) blk * 32);

                WavpackUnpackSamples (wpc, tmp, 32);
                compare_observer (&b, "negative-stereo-24", blk);
                WavpackCloseFile (wpc);
            }

            free (tmp);
        }

        check_per_return_normalization (&b, "negative-stereo-24", blk);
    }

    for (int i = 0; i < cs.nterms; i++)
        term_hits [TERM_INDEX (cs.terms [i])] += b.frames_total * 2;

    chain_frames_compared += b.frames_total;
    g_saw_wide_source |= saw_wide;

    buf_free (&b.image);
    free (b.expect);
}

// Seek after partial decode, and two interleaved instances.
static void scenario_seek_and_interleave (void)
{
    ChainSpec cs;
    Built b;
    int saw_wide = 0;
    int nchan = 2;

    memset (&cs, 0, sizeof (cs));
    cs.nterms = 3;
    cs.terms [0] = 2;  cs.deltas [0] = 2; cs.weights_A [0] = 600; cs.weights_B [0] = -600;
    cs.terms [1] = -1; cs.deltas [1] = 2; cs.weights_A [1] = 700; cs.weights_B [1] = -700;
    cs.terms [2] = 17; cs.deltas [2] = 2; cs.weights_A [2] = 500; cs.weights_B [2] = -500;

    if (build_scenario (&cs, 0, 16, 40, 3, 8000, "seek-interleave", &b, &saw_wide) != 0)
        return;

    // whole and interleaved reads
    decode_and_compare (&b, 40, "seek-interleave/whole");
    decode_and_compare (&b, 1, "seek-interleave/1");
    decode_and_compare (&b, 7, "seek-interleave/7");
    decode_and_compare (&b, 17, "seek-interleave/17");

    // public seek + reread in the middle
    {
        MemReader mr;
        WavpackContext *wpc = open_mem (b.image.data, b.image.len, &mr);
        int32_t *out = malloc (sizeof (int32_t) * 17 * nchan);
        int64_t target = 55;

        if (!wpc) {
            FAILF ("seek: open failed");
        }
        else {
            uint32_t got = WavpackUnpackSamples (wpc, out, 10);

            if (got != 10)
                FAILF ("seek: initial read got %u", got);

            if (!WavpackSeekSample64 (wpc, target))
                FAILF ("seek: WavpackSeekSample64(%lld) failed", (long long) target);
            else {
                got = WavpackUnpackSamples (wpc, out, 17);

                if (got != 17)
                    FAILF ("seek: post-seek read got %u", got);
                else
                    for (int f = 0; f < 17; f++)
                        for (int c = 0; c < nchan; c++) {
                            int64_t idx = (target + f) * nchan + c;
                            comparisons++;

                            if (out [f * nchan + c] != b.expect [idx]) {
                                ReportCtx r = { "seek", (int) ((target + f) / b.block_frames), -1, c,
                                    target + f };
                                report_first_deviation (&r, "pcm", out [f * nchan + c], b.expect [idx]);
                                FAILF ("seek: mismatch at frame %lld", (long long) (target + f));
                            }
                        }
            }

            if (WavpackGetNumErrors (wpc))
                FAILF ("seek: decoder reported errors");

            WavpackCloseFile (wpc);
        }

        free (out);
    }

    // two instances advancing at different chunk sizes
    {
        MemReader mr1, mr2;
        WavpackContext *w1 = open_mem (b.image.data, b.image.len, &mr1);
        WavpackContext *w2 = open_mem (b.image.data, b.image.len, &mr2);
        int32_t *o1 = malloc (sizeof (int32_t) * 7 * nchan);
        int32_t *o2 = malloc (sizeof (int32_t) * 13 * nchan);
        int64_t p1 = 0, p2 = 0;
        int ok = 1;

        if (!w1 || !w2) {
            FAILF ("interleave: open failed");
            ok = 0;
        }

        while (ok && (p1 < b.frames_total || p2 < b.frames_total)) {
            if (p1 < b.frames_total) {
                uint32_t got = WavpackUnpackSamples (w1, o1, 7);

                if (!got)
                    p1 = b.frames_total;
                else {
                    for (int f = 0; f < (int) got; f++)
                        for (int c = 0; c < nchan; c++) {
                            comparisons++;

                            if (o1 [f * nchan + c] != b.expect [(p1 + f) * nchan + c]) {
                                FAILF ("interleave: instance1 mismatch at frame %lld",
                                    (long long) (p1 + f));
                                ok = 0;
                            }
                        }

                    p1 += got;
                }
            }

            if (p2 < b.frames_total) {
                uint32_t got = WavpackUnpackSamples (w2, o2, 13);

                if (!got)
                    p2 = b.frames_total;
                else {
                    for (int f = 0; f < (int) got; f++)
                        for (int c = 0; c < nchan; c++) {
                            comparisons++;

                            if (o2 [f * nchan + c] != b.expect [(p2 + f) * nchan + c]) {
                                FAILF ("interleave: instance2 mismatch at frame %lld",
                                    (long long) (p2 + f));
                                ok = 0;
                            }
                        }

                    p2 += got;
                }
            }
        }

        comparisons += 2;

        if (p1 != b.frames_total || p2 != b.frames_total)
            FAILF ("interleave: instances covered %lld/%lld of %d frames",
                (long long) p1, (long long) p2, b.frames_total);

        if (w1) WavpackCloseFile (w1);
        if (w2) WavpackCloseFile (w2);
        free (o1);
        free (o2);
    }

    buf_free (&b.image);
    free (b.expect);
}

// Compatibility control with the real library encoder. Not a substitute for
// the planned blocks; just confirms the public path still round-trips.
typedef struct { Buffer *buf; } OutSink;

static int sink_write (void *id, void *data, int32_t count)
{
    OutSink *o = id;
    buf_bytes (o->buf, data, (size_t) count);
    return 1;
}

static void scenario_library_control (void)
{
    enum { FRAMES = 300, CH = 2 };
    static int32_t samples [FRAMES * CH];
    WavpackConfig cfg;
    WavpackContext *w;
    Buffer img;
    MemReader mr;
    OutSink sink;
    WavpackContext *d;
    int32_t *out;
    int64_t total = 0;
    int ok = 1;

    for (int f = 0; f < FRAMES; f++) {
        samples [f * 2] = (int32_t) (((uint32_t) (f * 1103515245 + 12345) >> 13) % 200000) - 100000;
        samples [f * 2 + 1] = (int32_t) (((uint32_t) (f * 2654435761u + 7) >> 11) % 200000) - 100000;
    }

    buf_init (&img);
    sink.buf = &img;
    w = WavpackOpenFileOutput (sink_write, &sink, NULL);

    if (!w) {
        FAILF ("control: output open failed");
        return;
    }

    memset (&cfg, 0, sizeof (cfg));
    cfg.bits_per_sample = 24;
    cfg.bytes_per_sample = 3;
    cfg.num_channels = CH;
    cfg.channel_mask = 3;
    cfg.sample_rate = 48000;
    cfg.block_samples = 64;
    cfg.flags = 0;

    if (!WavpackSetConfiguration64 (w, &cfg, FRAMES, NULL) || !WavpackPackInit (w) ||
        !WavpackPackSamples (w, samples, FRAMES) || !WavpackFlushSamples (w)) {
            FAILF ("control: encode failed");
            WavpackCloseFile (w);
            buf_free (&img);
            return;
    }

    WavpackCloseFile (w);
    d = open_mem (img.data, img.len, &mr);

    if (!d) {
        FAILF ("control: decode open failed");
        buf_free (&img);
        return;
    }

    out = malloc (sizeof (int32_t) * 13 * CH);

    while (1) {
        uint32_t got = WavpackUnpackSamples (d, out, 13);

        if (!got)
            break;

        for (int f = 0; f < (int) got; f++)
            for (int c = 0; c < CH; c++) {
                comparisons++;

                if (out [f * CH + c] != samples [(total + f) * CH + c]) {
                    FAILF ("control: mismatch at frame %lld", (long long) (total + f));
                    ok = 0;
                }
            }

        total += got;
    }

    comparisons++;

    if (total != FRAMES || WavpackGetNumErrors (d))
        FAILF ("control: decoded %lld/%d frames, %d errors",
            (long long) total, FRAMES, WavpackGetNumErrors (d));

    if (ok)
        printf ("compat-control: library-encoded stereo 24-bit, %d frames, OK\n", FRAMES);

    free (out);
    WavpackCloseFile (d);
    buf_free (&img);
}

// ---------------------------------------------------------------------------

int main (void)
{
    WavpackSetDecorrObserver (residual_obs, state_obs, NULL);

    scenario_mixed_stereo_16 ();
    scenario_mono_24 ();
    scenario_negative_stereo_24 ();
    scenario_seek_and_interleave ();
    scenario_library_control ();

    if (failures) {
        fprintf (stderr, "DECORR_BLOCKS FAIL comparisons=%ld failures=%d\n", comparisons, failures);
        return 1;
    }

    if (!g_saw_wide_source) {
        fprintf (stderr, "FAIL: no planned chain exercised a >16-bit prediction source\n");
        return 1;
    }

    printf ("DECORR_BLOCKS OK comparisons=%ld chain_frames=%ld\n", comparisons, chain_frames_compared);
    printf ("term-comparisons:");

    for (int t = -4; t <= 18; t++)
        if (term_hits [TERM_INDEX (t)])
            printf (" %d=%d", t, term_hits [TERM_INDEX (t)]);

    printf ("\n");
    return 0;
}
