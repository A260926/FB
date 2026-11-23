/*
tests/stems.c - dual subband stem conformance tests.

The system under test is the external drmp3_stems_* session API (linked from
dr_mp3_stems.c) and drmp3dec_decode_frame_stems. The independent frequency-band
reference below is built in this translation unit from the canonical post-antialias
subband spectrum: every subband gets its own IMDCT overlap history and is assigned to
the low or high path only after the IMDCT, then each path runs its own polyphase
synthesis. Keeping per-band histories means a cut move lets the abandoned path's tail
ring out exactly as the real decoder must.

The whole dr_mp3 implementation is included with DRMP3_API forced to static so these
internal helpers are reachable without exporting symbols that would collide with the
production objects linked from task_support/codec.o.
*/
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_FLOAT_OUTPUT
#define DRMP3_API static
#include "dr_mp3.h"

#undef DRMP3_API
#include "dr_mp3_stems.h"

#include "task_support/check.h"
#include "task_support/codec.h"
#include "task_support/stem_fixture.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STEM_TEST_BANDS      32
#define STEM_TEST_GRANULE    576
#define STEM_TEST_MAXFRAMES  80
#define STEM_TEST_EPS        3e-5

/* ------------------------------------------------------------------ */
/* Independent canonical-subband reference.                            */
/* ------------------------------------------------------------------ */

typedef struct
{
    drmp3dec dec;
    /* Per-path, per-band IMDCT overlap. A path keeps every band's history; when a band
       moves to the other path, this path feeds it zero new spectrum so its retained
       overlap rings out as the required tail instead of being cleared. */
    float band_ovl[2][STEM_TEST_BANDS][2][9 * 32];
    float qmf[2][15 * 64];                        /* Per-path QMF history. */
    float lins[2][(18 + 15) * 2 * 32];
    float path_spec[2][2 * 576];
    float band_spec[2 * 576];
} stem_ref;

static void stem_ref_init(stem_ref *r)
{
    memset(r, 0, sizeof(*r));
    drmp3dec_init(&r->dec);
}

/* Single-band IMDCT, selecting the window exactly as drmp3_L3_imdct_gr does for band b.
   Keeping the bands separate (instead of one grouped call) is what makes this an
   independent per-band reference while exercising the same primitives. */
static void stem_ref_imdct_band(float *band18, float *ovl9, unsigned block_type,
    unsigned b, unsigned n_long)
{
    static const float win_long[18] = {
        0.99904822f,0.99144486f,0.97629601f,0.95371695f,0.92387953f,0.88701083f,0.84339145f,0.79335334f,0.73727734f,
        0.04361938f,0.13052619f,0.21643961f,0.30070580f,0.38268343f,0.46174861f,0.53729961f,0.60876143f,0.67559021f };
    static const float win_stop[18] = {
        1,1,1,1,1,1,0.99144486f,0.92387953f,0.79335334f,0,0,0,0,0,0,0.13052619f,0.38268343f,0.60876143f };

    if (b < n_long)
    {
        drmp3_L3_imdct36(band18, ovl9, win_long, 1);
    } else if (block_type == DRMP3_SHORT_BLOCK_TYPE)
    {
        drmp3_L3_imdct_short(band18, ovl9, 1);
    } else
    {
        drmp3_L3_imdct36(band18, ovl9,
            block_type == DRMP3_STOP_BLOCK_TYPE ? win_stop : win_long, 1);
    }
}

/* Reconstruct one granule's two paths from the canonical post-antialias spectrum.
   Each (path, band) pair runs its own single-band IMDCT against that path's retained
   band history; bands not currently owned by a path receive zero input so their old
   overlap decays naturally. This is an independent, explicitly per-band re-derivation
   of the same split/tail rule the production decoder implements with grouped IMDCT. */
static void stem_ref_granule(stem_ref *r, drmp3_L3_gr_info *gr_info, int nch,
    unsigned cut, float *low_pcm, float *high_pcm)
{
    unsigned block_type = gr_info[0].block_type;
    unsigned n_long = gr_info[0].mixed_block_flag
        ? (2u << (unsigned)(DRMP3_HDR_GET_MY_SAMPLE_RATE(r->dec.header) == 2)) : 0u;
    int b, ch, p;

    DRMP3_ZERO_MEMORY(r->dec.scratch.grbuf[0], 576 * 2 * sizeof(float));
    drmp3_L3_decode_spectral(&r->dec, &r->dec.scratch, gr_info, nch);

    memset(r->path_spec, 0, sizeof(r->path_spec));

    for (p = 0; p < 2; p++)
    {
        for (b = 0; b < STEM_TEST_BANDS; b++)
        {
            int owned = p == 0 ? (b < (int)cut) : (b >= (int)cut);
            for (ch = 0; ch < nch; ch++)
            {
                float *dst = r->path_spec[p] + 576 * ch + 18 * b;
                memset(r->band_spec, 0, sizeof(r->band_spec));
                if (owned)
                {
                    memcpy(r->band_spec + 576 * ch + 18 * b,
                           r->dec.scratch.grbuf[ch] + 18 * b, 18 * sizeof(float));
                }
                /* Zero input for a non-owned band still advances this path's retained
                   band overlap, emitting its windowed tail. */
                stem_ref_imdct_band(r->band_spec + 576 * ch + 18 * b,
                    r->band_ovl[p][b][ch], block_type, (unsigned)b, n_long);
                drmp3_L3_change_sign(r->band_spec + 576 * ch);
                memcpy(dst, r->band_spec + 576 * ch + 18 * b, 18 * sizeof(float));
            }
        }
    }

    for (p = 0; p < 2; p++)
    {
        float *pcm = p == 0 ? low_pcm : high_pcm;
        drmp3d_DCT_II(r->path_spec[p], 18);
        if (nch == 2)
        {
            drmp3d_DCT_II(r->path_spec[p] + 576, 18);
        }
        memcpy(r->lins[p], r->qmf[p], sizeof(float) * 15 * 64);
        for (int i = 0; i < 18; i += 2)
        {
            drmp3d_synth(r->path_spec[p] + i, pcm + 32 * nch * i, nch,
                r->lins[p] + i * 64);
        }
        /* Mirror drmp3d_synth_granule's mono state rule. */
#ifndef DR_MP3_NONSTANDARD_BUT_LOGICAL
        if (nch == 1)
        {
            for (int i = 0; i < 15 * 64; i += 2)
            {
                r->qmf[p][i] = r->lins[p][18 * 64 + i];
            }
        } else
#endif
        {
            memcpy(r->qmf[p], r->lins[p] + 18 * 64, sizeof(float) * 15 * 64);
        }
    }
}

/* Drive one physical frame through the reference; returns raw PCM frames (0 on fail). */
static drmp3_uint64 stem_ref_frame(stem_ref *r, const drmp3_uint8 *mp3, int bytes,
    const unsigned cuts[2], float *low, float *high, int *frame_bytes)
{
    drmp3_bs bs[1];
    const drmp3_uint8 *hdr;
    int frame_size = 0, off, nch, mdb;

    off = drmp3d_find_frame(mp3, bytes, &r->dec.free_format_bytes, &frame_size);
    if (!frame_size || off + frame_size > bytes)
    {
        if (frame_bytes) *frame_bytes = off;
        return 0;
    }
    hdr = mp3 + off;
    memcpy(r->dec.header, hdr, DRMP3_HDR_SIZE);
    if (frame_bytes) *frame_bytes = off + frame_size;
    nch = DRMP3_HDR_IS_MONO(hdr) ? 1 : 2;

    drmp3_bs_init(bs, hdr + DRMP3_HDR_SIZE, frame_size - DRMP3_HDR_SIZE);
    if (DRMP3_HDR_IS_CRC(hdr))
    {
        drmp3_bs_get_bits(bs, 16);
    }
    mdb = drmp3_L3_read_side_info(bs, r->dec.scratch.gr_info, hdr);
    if (mdb < 0 || bs->pos > bs->limit)
    {
        return 0;
    }
    if (!drmp3_L3_restore_reservoir(&r->dec, bs, &r->dec.scratch, mdb))
    {
        drmp3_L3_save_reservoir(&r->dec, &r->dec.scratch);
        return 0;
    }

    for (int igr = 0; igr < 2; igr++)
    {
        stem_ref_granule(r, r->dec.scratch.gr_info + igr * nch, nch, cuts[igr],
            low + (size_t)igr * STEM_TEST_GRANULE * nch,
            high + (size_t)igr * STEM_TEST_GRANULE * nch);
    }
    drmp3_L3_save_reservoir(&r->dec, &r->dec.scratch);
    return 2 * STEM_TEST_GRANULE;
}

/* Cut in effect at absolute granule g. */
static unsigned ref_cut_at(const drmp3_stem_point *pts, size_t npts, drmp3_uint64 g)
{
    size_t k = 0;
    while (k + 1 < npts && pts[k + 1].granule <= g)
    {
        k++;
    }
    return pts[k].low_subbands;
}

/*
Full linear reference. A native probe supplies only the stream start offset and the
gapless delay/padding interpretation; every audio sample is produced by the independent
per-band reference following the absolute-granule schedule, then trimmed once on the
shared clock. Writes trimmed per-channel frames and returns their count.
*/
static drmp3_uint64 reference_stream(const unsigned char *data, size_t size,
    const drmp3_stem_point *pts, size_t npts, float *low, float *high)
{
    drmp3 probe;
    stem_ref r;
    size_t pos, scan_end;
    drmp3_uint64 granule = 0, raw = 0, kept = 0;
    drmp3_uint64 delay, padding, total, visible_end;
    unsigned nch;
    float tl[DRMP3_MAX_SAMPLES_PER_FRAME], th[DRMP3_MAX_SAMPLES_PER_FRAME];

    if (!drmp3_init_memory(&probe, data, size, NULL))
    {
        return 0;
    }
    pos = (size_t)probe.streamStartOffset;
    scan_end = probe.memory.dataSize;
    delay = probe.delayInPCMFrames;
    padding = probe.paddingInPCMFrames;
    nch = probe.channels;
    drmp3_uninit(&probe);

    /* Pass 1: count audio granules to establish the raw total and padding boundary. */
    stem_ref_init(&r);
    while (pos < scan_end)
    {
        unsigned cuts[2] = {
            ref_cut_at(pts, npts, granule),
            ref_cut_at(pts, npts, granule + 1)
        };
        int fb = 0;
        drmp3_uint64 got = stem_ref_frame(&r, data + pos, (int)(scan_end - pos),
            cuts, tl, th, &fb);
        if (fb <= 0)
        {
            break;
        }
        pos += (size_t)fb;
        if (got)
        {
            granule += 2;
        }
    }

    total = granule * STEM_TEST_GRANULE;
    if (delay >= total) delay = 0;
    if (padding >= total) padding = 0;
    visible_end = total - padding;

    /* Pass 2: rebuild and emit only the gapless-visible frames. */
    if (!drmp3_init_memory(&probe, data, size, NULL))
    {
        return 0;
    }
    pos = (size_t)probe.streamStartOffset;
    scan_end = probe.memory.dataSize;
    drmp3_uninit(&probe);

    stem_ref_init(&r);
    granule = 0;
    while (pos < scan_end)
    {
        unsigned cuts[2] = {
            ref_cut_at(pts, npts, granule),
            ref_cut_at(pts, npts, granule + 1)
        };
        int fb = 0;
        drmp3_uint64 got = stem_ref_frame(&r, data + pos, (int)(scan_end - pos),
            cuts, tl, th, &fb);
        if (fb <= 0)
        {
            break;
        }
        pos += (size_t)fb;
        if (!got)
        {
            continue;
        }
        for (drmp3_uint64 i = 0; i < got; i++)
        {
            drmp3_uint64 rw = raw + i;
            if (rw >= delay && rw < visible_end)
            {
                memcpy(low + kept * nch, tl + i * nch, nch * sizeof(float));
                memcpy(high + kept * nch, th + i * nch, nch * sizeof(float));
                kept++;
            }
        }
        raw += got;
        granule += 2;
    }
    return kept;
}

/* ------------------------------------------------------------------ */
/* Shared helpers.                                                     */
/* ------------------------------------------------------------------ */

static unsigned char g_bytes[STEM_TEST_MAXFRAMES * STEM_FIX_MAX_FRAME_BYTES + 512];

static double max_abs_diff(const float *a, const float *b, size_t n)
{
    double m = 0;
    for (size_t i = 0; i < n; i++)
    {
        double e = fabs((double)a[i] - b[i]);
        if (e > m) m = e;
    }
    return m;
}

static double l1(const float *a, size_t n)
{
    double e = 0;
    for (size_t i = 0; i < n; i++) e += fabs(a[i]);
    return e;
}

static float *xalloc(size_t frames, unsigned ch)
{
    return (float*)malloc(frames * ch * sizeof(float));
}

static drmp3_uint64 native_decode(const unsigned char *data, size_t size, float *out,
    unsigned *ch, unsigned *rate)
{
    drmp3 d;
    drmp3_uint64 n;
    if (!drmp3_init_memory(&d, data, size, NULL)) return 0;
    if (ch) *ch = d.channels;
    if (rate) *rate = d.sampleRate;
    n = drmp3_read_pcm_frames_f32(&d, STEM_TEST_MAXFRAMES * 1152, out);
    drmp3_uninit(&d);
    return n;
}

static size_t build_long(unsigned char *out, unsigned nf, unsigned ch, unsigned pairs,
    int ms, int ist, int backref, unsigned seed0)
{
    stem_fix_item items[STEM_TEST_MAXFRAMES];
    for (unsigned i = 0; i < nf; i++)
    {
        items[i].desc = stem_fix_long(0, ch, i % 2, 0, pairs, ms, ist, seed0 + i);
        if (ist)
        {
            items[i].desc.gr[0].table1_pairs[1] = pairs / 4;
            items[i].desc.gr[1].table1_pairs[1] = pairs / 4;
        }
        items[i].backref = backref && i > 0;
    }
    return stem_fix_build(out, items, nf);
}

static size_t build_blocks(unsigned char *out, unsigned nf, unsigned block_type,
    int mixed)
{
    stem_fix_item items[STEM_TEST_MAXFRAMES];
    for (unsigned i = 0; i < nf; i++)
    {
        items[i].desc = stem_fix_long(0, 2, i % 2, 0, 100, 0, 0, i + 1);
        items[i].desc.gr[0].block_type = block_type;
        items[i].desc.gr[0].mixed = (drmp3_uint8)mixed;
        items[i].desc.gr[1].block_type = block_type;
        items[i].desc.gr[1].mixed = (drmp3_uint8)mixed;
        items[i].backref = 0;
    }
    return stem_fix_build(out, items, nf);
}

/* Read a whole session in deliberately small, odd-sized chunks. */
static drmp3_uint64 read_chunked(drmp3_stems *s, drmp3_uint64 total,
    float *low, float *high, unsigned ch)
{
    static const unsigned chunks[] = {1, 7, 13, 3, 37, 2};
    drmp3_uint64 done = 0;
    size_t ci = 0;
    while (done < total)
    {
        drmp3_uint64 want = chunks[ci++ % (sizeof(chunks) / sizeof(chunks[0]))];
        drmp3_uint64 r = drmp3_stems_read(s, want,
            low + done * ch, high + done * ch);
        if (r == 0) break;
        done += r;
    }
    return done;
}

/* ================================================================== */
/* split group                                                         */
/* ================================================================== */

/* Boundary cuts: cut 32 puts everything in low, cut 0 everything in high. */
static int split_boundary_cuts(void)
{
    const unsigned nf = 10;
    size_t z = build_long(g_bytes, nf, 2, 120, 0, 0, 0, 1);
    TASK_CHECK(z > 0);
    drmp3_uint64 total = (drmp3_uint64)nf * 1152;
    unsigned ch = 2;
    float *nat = xalloc(total, ch), *lo = xalloc(total, ch), *hi = xalloc(total, ch);
    TASK_CHECK(nat && lo && hi);
    TASK_CHECK(native_decode(g_bytes, z, nat, &ch, NULL) == total);

    drmp3_stem_point p32[] = {{0, 32}};
    drmp3_stems *s = drmp3_stems_open_memory(g_bytes, z, p32, 1);
    TASK_CHECK(s != NULL);
    TASK_CHECK(drmp3_stems_read(s, total, lo, hi) == total);
    TASK_CHECK(max_abs_diff(lo, nat, total * ch) <= STEM_TEST_EPS);
    TASK_CHECK(l1(hi, total * ch) == 0.0);
    drmp3_stems_close(s);

    drmp3_stem_point p0[] = {{0, 0}};
    s = drmp3_stems_open_memory(g_bytes, z, p0, 1);
    TASK_CHECK(s != NULL);
    TASK_CHECK(drmp3_stems_read(s, total, lo, hi) == total);
    TASK_CHECK(l1(lo, total * ch) == 0.0);
    TASK_CHECK(max_abs_diff(hi, nat, total * ch) <= STEM_TEST_EPS);
    drmp3_stems_close(s);

    free(nat); free(lo); free(hi);
    return 0;
}

/* Interior cut: each stem independently matches the per-band reference and carries
   energy, and their sum equals the native decode. */
static int split_interior_reference(void)
{
    const unsigned nf = 12;
    size_t z = build_long(g_bytes, nf, 2, 120, 0, 0, 0, 3);
    TASK_CHECK(z > 0);
    drmp3_uint64 total = (drmp3_uint64)nf * 1152;
    unsigned ch = 2;
    float *nat = xalloc(total, ch), *lo = xalloc(total, ch), *hi = xalloc(total, ch);
    float *rlo = xalloc(total, ch), *rhi = xalloc(total, ch);
    TASK_CHECK(nat && lo && hi && rlo && rhi);
    TASK_CHECK(native_decode(g_bytes, z, nat, &ch, NULL) == total);

    drmp3_stem_point p[] = {{0, 8}};
    drmp3_stems *s = drmp3_stems_open_memory(g_bytes, z, p, 1);
    TASK_CHECK(s != NULL);
    TASK_CHECK(drmp3_stems_read(s, total, lo, hi) == total);
    drmp3_stems_close(s);

    TASK_CHECK(reference_stream(g_bytes, z, p, 1, rlo, rhi) == total);

    /* Independent per-stem checks (not just the sum). */
    TASK_CHECK(max_abs_diff(lo, rlo, total * ch) <= STEM_TEST_EPS);
    TASK_CHECK(max_abs_diff(hi, rhi, total * ch) <= STEM_TEST_EPS);
    TASK_CHECK(l1(lo, total * ch) > 1.0);
    TASK_CHECK(l1(hi, total * ch) > 1.0);

    /* And the additive reconstruction of the native output. */
    double maxe = 0;
    for (size_t i = 0; i < total * ch; i++)
    {
        double e = fabs((double)lo[i] + hi[i] - nat[i]);
        if (e > maxe) maxe = e;
    }
    TASK_CHECK(maxe <= STEM_TEST_EPS);

    free(nat); free(lo); free(hi); free(rlo); free(rhi);
    return 0;
}

/* Exactly one spectral pass per audio granule, counted at the real shared stage. */
static int split_spectral_pass_count(void)
{
    const unsigned nf = 11;
    size_t z = build_long(g_bytes, nf, 2, 100, 0, 0, 0, 5);
    TASK_CHECK(z > 0);
    drmp3_stem_point p[] = {{0, 16}};
    drmp3_stems *s = drmp3_stems_open_memory(g_bytes, z, p, 1);
    TASK_CHECK(s != NULL);
    drmp3_uint64 total = drmp3_stems_length(s);
    float *lo = xalloc(total, 2), *hi = xalloc(total, 2);
    TASK_CHECK(lo && hi);

    unsigned long long before = task_spectral_passes;
    TASK_CHECK(drmp3_stems_read(s, total, lo, hi) == total);
    unsigned long long used = task_spectral_passes - before;
    TASK_CHECK(used == (unsigned long long)nf * 2);
    drmp3_stems_close(s);
    free(lo); free(hi);
    return 0;
}

/* Low level entry: argument validation and mode re-init behaviour. */
static int split_low_level_contract(void)
{
    const unsigned nf = 4;
    size_t z = build_long(g_bytes, nf, 2, 80, 0, 0, 0, 2);
    TASK_CHECK(z > 0);
    drmp3dec dec;
    drmp3dec_frame_info info;
    float lo[DRMP3_MAX_SAMPLES_PER_FRAME], hi[DRMP3_MAX_SAMPLES_PER_FRAME];
    unsigned good[2] = {8, 8};

    drmp3dec_init(&dec);
    memset(&info, 0x55, sizeof(info));
    /* Illegal cut / NULL arguments: zero return, cleared info, no crash. */
    unsigned bad[2] = {33, 8};
    TASK_CHECK(drmp3dec_decode_frame_stems(&dec, g_bytes, (int)z, lo, hi, &info, bad) == 0);
    TASK_CHECK(info.frame_bytes == 0 && info.layer == 0);
    TASK_CHECK(drmp3dec_decode_frame_stems(NULL, g_bytes, (int)z, lo, hi, &info, good) == 0);
    TASK_CHECK(drmp3dec_decode_frame_stems(&dec, g_bytes, (int)z, NULL, hi, &info, good) == 0);

    /* Valid decode reports frame bytes and 1152 frames per channel. */
    drmp3dec_init(&dec);
    TASK_CHECK(drmp3dec_decode_frame_stems(&dec, g_bytes, (int)z, lo, hi, &info, good) == 1152);
    TASK_CHECK(info.frame_bytes > 0 && info.channels == 2 && info.sample_rate == 44100);

    /* Switching back to native after a re-init still works. */
    float nat[DRMP3_MAX_SAMPLES_PER_FRAME];
    drmp3dec_init(&dec);
    TASK_CHECK(drmp3dec_decode_frame(&dec, g_bytes, (int)z, nat, &info) == 1152);
    return 0;
}

/* Every block/stereo/reservoir fixture variant reconstructs natively and keeps sound
   in both stems, checked against the independent reference. */
static int split_all_variants(void)
{
    struct { unsigned bt; int mixed; const char *nm; } blocks[] = {
        {0, 0, "long"}, {1, 0, "start"}, {2, 0, "short"}, {2, 1, "mixed"}, {3, 0, "stop"}
    };
    for (size_t v = 0; v < sizeof(blocks) / sizeof(blocks[0]); v++)
    {
        const unsigned nf = 8;
        size_t z = build_blocks(g_bytes, nf, blocks[v].bt, blocks[v].mixed);
        TASK_CHECK(z > 0);
        drmp3_uint64 total = (drmp3_uint64)nf * 1152;
        float *lo = xalloc(total, 2), *hi = xalloc(total, 2);
        float *rlo = xalloc(total, 2), *rhi = xalloc(total, 2);
        TASK_CHECK(lo && hi && rlo && rhi);
        drmp3_stem_point p[] = {{0, 8}, {6, 24}, {10, 16}};
        drmp3_stems *s = drmp3_stems_open_memory(g_bytes, z, p, 3);
        TASK_CHECK(s != NULL);
        TASK_CHECK(drmp3_stems_length(s) == total);
        TASK_CHECK(drmp3_stems_read(s, total, lo, hi) == total);
        drmp3_stems_close(s);
        TASK_CHECK(reference_stream(g_bytes, z, p, 3, rlo, rhi) == total);
        TASK_CHECK(max_abs_diff(lo, rlo, total * 2) <= STEM_TEST_EPS);
        TASK_CHECK(max_abs_diff(hi, rhi, total * 2) <= STEM_TEST_EPS);
        TASK_CHECK(l1(lo, total * 2) > 1.0 && l1(hi, total * 2) > 1.0);
        free(lo); free(hi); free(rlo); free(rhi);
    }

    /* M/S, intensity and reservoir fixtures. */
    struct { int ms; int ist; int backref; } modes[] = {
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}
    };
    for (size_t v = 0; v < sizeof(modes) / sizeof(modes[0]); v++)
    {
        const unsigned nf = 10;
        size_t z = build_long(g_bytes, nf, 2, 90, modes[v].ms, modes[v].ist,
            modes[v].backref, 7);
        TASK_CHECK(z > 0);
        drmp3_uint64 total = (drmp3_uint64)nf * 1152;
        float *lo = xalloc(total, 2), *hi = xalloc(total, 2);
        float *rlo = xalloc(total, 2), *rhi = xalloc(total, 2);
        TASK_CHECK(lo && hi && rlo && rhi);
        drmp3_stem_point p[] = {{0, 16}, {8, 4}};
        drmp3_stems *s = drmp3_stems_open_memory(g_bytes, z, p, 2);
        TASK_CHECK(s != NULL);
        TASK_CHECK(drmp3_stems_read(s, total, lo, hi) == total);
        drmp3_stems_close(s);
        TASK_CHECK(reference_stream(g_bytes, z, p, 2, rlo, rhi) == total);
        TASK_CHECK(max_abs_diff(lo, rlo, total * 2) <= STEM_TEST_EPS);
        TASK_CHECK(max_abs_diff(hi, rhi, total * 2) <= STEM_TEST_EPS);
        free(lo); free(hi); free(rlo); free(rhi);
    }
    return 0;
}

/* ================================================================== */
/* timeline group                                                      */
/* ================================================================== */

/* Schedule crossing cut values 0/8/24/32, including old-path tail ring-out. */
static int timeline_cut_schedule(void)
{
    const unsigned nf = 14;
    size_t z = build_long(g_bytes, nf, 2, 120, 0, 0, 0, 9);
    TASK_CHECK(z > 0);
    drmp3_uint64 total = (drmp3_uint64)nf * 1152;
    float *lo = xalloc(total, 2), *hi = xalloc(total, 2);
    float *rlo = xalloc(total, 2), *rhi = xalloc(total, 2);
    TASK_CHECK(lo && hi && rlo && rhi);

    /* Granule coordinates: each frame has two granules. Sweep through every
       boundary value and reverse direction so a formerly-silent path must ring. */
    drmp3_stem_point p[] = {
        {0, 32}, {4, 24}, {8, 8}, {12, 0}, {16, 8}, {20, 24}, {24, 32}
    };
    drmp3_stems *s = drmp3_stems_open_memory(g_bytes, z, p,
        sizeof(p) / sizeof(p[0]));
    TASK_CHECK(s != NULL);
    TASK_CHECK(drmp3_stems_read(s, total, lo, hi) == total);
    drmp3_stems_close(s);

    TASK_CHECK(reference_stream(g_bytes, z, p, sizeof(p) / sizeof(p[0]), rlo, rhi)
        == total);
    TASK_CHECK(max_abs_diff(lo, rlo, total * 2) <= STEM_TEST_EPS);
    TASK_CHECK(max_abs_diff(hi, rhi, total * 2) <= STEM_TEST_EPS);

    free(lo); free(hi); free(rlo); free(rhi);
    return 0;
}

/* Small reads always deliver matching frame counts; a zero read does not advance. */
static int timeline_small_and_zero_reads(void)
{
    const unsigned nf = 8;
    size_t z = build_long(g_bytes, nf, 2, 100, 0, 0, 0, 4);
    TASK_CHECK(z > 0);
    drmp3_stem_point p[] = {{0, 16}, {6, 8}};
    drmp3_stems *s = drmp3_stems_open_memory(g_bytes, z, p, 2);
    TASK_CHECK(s != NULL);
    drmp3_uint64 total = drmp3_stems_length(s);

    float *lo = xalloc(total, 2), *hi = xalloc(total, 2);
    TASK_CHECK(lo && hi);
    TASK_CHECK(drmp3_stems_tell(s) == 0);
    TASK_CHECK(drmp3_stems_read(s, 0, NULL, NULL) == 0);
    TASK_CHECK(drmp3_stems_tell(s) == 0);

    TASK_CHECK(read_chunked(s, total, lo, hi, 2) == total);
    TASK_CHECK(drmp3_stems_tell(s) == total);

    /* Re-decode linearly and confirm the chunked result matches. */
    drmp3_stems *s2 = drmp3_stems_open_memory(g_bytes, z, p, 2);
    TASK_CHECK(s2 != NULL);
    float *lo2 = xalloc(total, 2), *hi2 = xalloc(total, 2);
    TASK_CHECK(lo2 && hi2);
    TASK_CHECK(drmp3_stems_read(s2, total, lo2, hi2) == total);
    TASK_CHECK(max_abs_diff(lo, lo2, total * 2) <= STEM_TEST_EPS);
    TASK_CHECK(max_abs_diff(hi, hi2, total * 2) <= STEM_TEST_EPS);
    drmp3_stems_close(s); drmp3_stems_close(s2);
    free(lo); free(hi); free(lo2); free(hi2);
    return 0;
}

/* Odd gapless delay/padding from an Info/Xing tag is applied once on the shared clock. */
static int timeline_gapless_trim(void)
{
    const unsigned nf = 9;
    size_t z = 0;
    /* Synthetic Info tag: enc_delay 24 (=>553), enc_padding 556 (=>27), FRAMES=nf. */
    {
        size_t slot = 417;
        memset(g_bytes, 0, slot);
        drmp3_uint32 head = 0xfffb9000u;
        for (int g = 0; g < 4; g++)
        {
            g_bytes[g] = (drmp3_uint8)(head >> (24 - 8 * g));
        }
        unsigned char *tag = g_bytes + 4 + 32;
        memcpy(tag, "Info", 4);
        tag[7] = 0x01;
        tag[8] = (drmp3_uint8)(nf >> 24); tag[9] = (drmp3_uint8)(nf >> 16);
        tag[10] = (drmp3_uint8)(nf >> 8); tag[11] = (drmp3_uint8)nf;
        unsigned char *lame = tag + 12;
        lame[0] = 'L';
        unsigned char *lp = lame + 21;
        lp[0] = (drmp3_uint8)(24 >> 4);
        lp[1] = (drmp3_uint8)(((24 & 0xF) << 4) | ((556 >> 8) & 0xF));
        lp[2] = (drmp3_uint8)(556 & 0xFF);
        z += slot;
    }
    {
        stem_fix_item items[STEM_TEST_MAXFRAMES];
        for (unsigned i = 0; i < nf; i++)
        {
            items[i].desc = stem_fix_long(0, 2, i % 2, 0, 100, 0, 0, i);
            items[i].backref = 0;
        }
        z += stem_fix_build(g_bytes + z, items, nf);
    }

    drmp3 probe;
    TASK_CHECK(drmp3_init_memory(&probe, g_bytes, z, NULL));
    drmp3_uint64 native_len = drmp3_get_pcm_frame_count(&probe);
    drmp3_uninit(&probe);
    /* raw = nf*1152; trimmed = raw - 553 - 27. */
    TASK_CHECK(native_len == (drmp3_uint64)nf * 1152 - 553 - 27);

    drmp3_stem_point p[] = {{0, 16}, {10, 8}};
    drmp3_stems *s = drmp3_stems_open_memory(g_bytes, z, p, 2);
    TASK_CHECK(s != NULL);
    TASK_CHECK(drmp3_stems_length(s) == native_len);

    float *lo = xalloc(native_len, 2), *hi = xalloc(native_len, 2);
    float *rlo = xalloc(native_len, 2), *rhi = xalloc(native_len, 2);
    TASK_CHECK(lo && hi && rlo && rhi);
    TASK_CHECK(drmp3_stems_read(s, native_len, lo, hi) == native_len);
    drmp3_stems_close(s);
    TASK_CHECK(reference_stream(g_bytes, z, p, 2, rlo, rhi) == native_len);
    TASK_CHECK(max_abs_diff(lo, rlo, native_len * 2) <= STEM_TEST_EPS);
    TASK_CHECK(max_abs_diff(hi, rhi, native_len * 2) <= STEM_TEST_EPS);

    free(lo); free(hi); free(rlo); free(rhi);
    return 0;
}

/* ================================================================== */
/* seek group                                                          */
/* ================================================================== */

/* Forward and backward seeks around schedule points and inside frames; the suffix
   must equal a from-the-start linear read with the same schedule. */
static int seek_suffix_match(void)
{
    const unsigned nf = 16;
    size_t z = build_long(g_bytes, nf, 2, 110, 0, 0, 0, 6);
    TASK_CHECK(z > 0);
    drmp3_stem_point p[] = {{0, 32}, {6, 8}, {14, 24}, {22, 0}, {28, 16}};
    drmp3_stems *s = drmp3_stems_open_memory(g_bytes, z, p, 5);
    TASK_CHECK(s != NULL);
    drmp3_uint64 total = drmp3_stems_length(s);

    float *flo = xalloc(total, 2), *fhi = xalloc(total, 2);
    TASK_CHECK(flo && fhi);
    TASK_CHECK(drmp3_stems_read(s, total, flo, fhi) == total);
    drmp3_stems_close(s);

    s = drmp3_stems_open_memory(g_bytes, z, p, 5);
    TASK_CHECK(s != NULL);
    drmp3_uint64 targets[] = {
        0, 1, 3 * 1152, 3 * 1152 + 7, 7 * 1152 + 500, 14 * 1152,
        11 * 1152 + 13, 2 * 1152 + 900, total - 1
    };
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++)
    {
        drmp3_uint64 tgt = targets[t];
        TASK_CHECK(drmp3_stems_seek(s, tgt) == 1);
        TASK_CHECK(drmp3_stems_tell(s) == tgt);
        drmp3_uint64 want = (tgt + 50 <= total) ? 50 : (total - tgt);
        float a[128], c[128];
        TASK_CHECK(drmp3_stems_read(s, want, a, c) == want);
        TASK_CHECK(max_abs_diff(a, flo + tgt * 2, want * 2) <= STEM_TEST_EPS);
        TASK_CHECK(max_abs_diff(c, fhi + tgt * 2, want * 2) <= STEM_TEST_EPS);
    }
    drmp3_stems_close(s);
    free(flo); free(fhi);
    return 0;
}

/* Seeking beyond the end is rejected with state preserved; EOF is reachable and
   another seek can leave it. */
static int seek_bounds_and_eof(void)
{
    const unsigned nf = 8;
    size_t z = build_long(g_bytes, nf, 2, 90, 0, 0, 0, 3);
    TASK_CHECK(z > 0);
    drmp3_stem_point p[] = {{0, 16}};
    drmp3_stems *s = drmp3_stems_open_memory(g_bytes, z, p, 1);
    TASK_CHECK(s != NULL);
    drmp3_uint64 total = drmp3_stems_length(s);

    TASK_CHECK(drmp3_stems_seek(s, 100) == 1);
    TASK_CHECK(drmp3_stems_tell(s) == 100);
    TASK_CHECK(drmp3_stems_seek(s, total + 1) == 0);
    TASK_CHECK(drmp3_stems_tell(s) == 100);  /* State preserved. */

    TASK_CHECK(drmp3_stems_seek(s, total) == 1);  /* Seek to end is legal. */
    TASK_CHECK(drmp3_stems_tell(s) == total);
    float tmp[8];
    TASK_CHECK(drmp3_stems_read(s, 1, tmp, tmp) == 0);  /* Immediate EOF. */

    TASK_CHECK(drmp3_stems_seek(s, 40) == 1);  /* Leave EOF by seeking back. */
    TASK_CHECK(drmp3_stems_read(s, 8, tmp, tmp) == 8);
    drmp3_stems_close(s);
    return 0;
}

/* A long stream forces recovery from more than 64 frames back; the suffix still
   matches the linear read, proving the bounded pre-roll rebuilds both histories. */
static int seek_recovery_window(void)
{
    const unsigned nf = 72;
    size_t z = build_long(g_bytes, nf, 2, 80, 0, 0, 0, 11);
    TASK_CHECK(z > 0);
    drmp3_stem_point p[] = {{0, 8}, {40, 24}, {100, 16}};
    drmp3_stems *s = drmp3_stems_open_memory(g_bytes, z, p, 3);
    TASK_CHECK(s != NULL);
    drmp3_uint64 total = drmp3_stems_length(s);
    TASK_CHECK(total == (drmp3_uint64)nf * 1152);

    float *flo = xalloc(total, 2), *fhi = xalloc(total, 2);
    TASK_CHECK(flo && fhi);
    TASK_CHECK(drmp3_stems_read(s, total, flo, fhi) == total);
    drmp3_stems_close(s);

    s = drmp3_stems_open_memory(g_bytes, z, p, 3);
    TASK_CHECK(s != NULL);
    drmp3_uint64 tgt = 70 * 1152 + 333;  /* Well past the 64-frame recovery window. */
    TASK_CHECK(drmp3_stems_seek(s, tgt) == 1);
    float a[256], c[256];
    TASK_CHECK(drmp3_stems_read(s, 64, a, c) == 64);
    TASK_CHECK(max_abs_diff(a, flo + tgt * 2, 128) <= STEM_TEST_EPS);
    TASK_CHECK(max_abs_diff(c, fhi + tgt * 2, 128) <= STEM_TEST_EPS);
    drmp3_stems_close(s);
    free(flo); free(fhi);
    return 0;
}

/* Two sessions over the same data interleave reads and seeks without interference. */
static int seek_interleaved_sessions(void)
{
    const unsigned nf = 10;
    size_t z = build_long(g_bytes, nf, 2, 100, 0, 0, 0, 8);
    TASK_CHECK(z > 0);
    drmp3_stem_point pa[] = {{0, 8}};
    drmp3_stem_point pb[] = {{0, 24}};
    drmp3_stems *a = drmp3_stems_open_memory(g_bytes, z, pa, 1);
    drmp3_stems *b = drmp3_stems_open_memory(g_bytes, z, pb, 1);
    TASK_CHECK(a && b);
    drmp3_uint64 total = drmp3_stems_length(a);
    TASK_CHECK(drmp3_stems_length(b) == total);

    float alo[64], ahi[64], blo[64], bhi[64];
    TASK_CHECK(drmp3_stems_read(a, 32, alo, ahi) == 32);
    TASK_CHECK(drmp3_stems_read(b, 17, blo, bhi) == 17);
    TASK_CHECK(drmp3_stems_tell(a) == 32 && drmp3_stems_tell(b) == 17);
    TASK_CHECK(drmp3_stems_seek(b, 500) == 1);
    TASK_CHECK(drmp3_stems_tell(a) == 32);  /* Untouched by b's seek. */
    TASK_CHECK(drmp3_stems_read(a, 16, alo, ahi) == 16);
    TASK_CHECK(drmp3_stems_tell(a) == 48);
    TASK_CHECK(drmp3_stems_read(b, 16, blo, bhi) == 16);
    TASK_CHECK(drmp3_stems_tell(b) == 516);

    /* Different schedules give genuinely different splits at the same frame. Read past
       the overlap startup transient where both low stems are still ramping. */
    drmp3_stems *a2 = drmp3_stems_open_memory(g_bytes, z, pa, 1);
    drmp3_stems *b2 = drmp3_stems_open_memory(g_bytes, z, pb, 1);
    TASK_CHECK(a2 && b2);
    enum { DIFF_N = 300 };
    float xa[DIFF_N * 2], ya[DIFF_N * 2], xb[DIFF_N * 2], yb[DIFF_N * 2];
    TASK_CHECK(drmp3_stems_read(a2, DIFF_N, xa, ya) == DIFF_N);
    TASK_CHECK(drmp3_stems_read(b2, DIFF_N, xb, yb) == DIFF_N);
    TASK_CHECK(max_abs_diff(ya, yb, DIFF_N * 2) > 1e-4);
    /* Both still reconstruct the same native signal. */
    double sume = 0;
    for (int i = 0; i < DIFF_N * 2; i++)
    {
        double e = fabs((double)(xa[i] + ya[i]) - (double)(xb[i] + yb[i]));
        if (e > sume) sume = e;
    }
    TASK_CHECK(sume <= STEM_TEST_EPS);
    drmp3_stems_close(a); drmp3_stems_close(b);
    drmp3_stems_close(a2); drmp3_stems_close(b2);
    return 0;
}

/* ================================================================== */
/* Registry                                                            */
/* ================================================================== */

const struct task_case task_cases[] = {
    {"split", "boundary_cuts",        split_boundary_cuts},
    {"split", "interior_reference",   split_interior_reference},
    {"split", "spectral_pass_count",  split_spectral_pass_count},
    {"split", "low_level_contract",   split_low_level_contract},
    {"split", "all_variants",         split_all_variants},

    {"timeline", "cut_schedule",          timeline_cut_schedule},
    {"timeline", "small_and_zero_reads",  timeline_small_and_zero_reads},
    {"timeline", "gapless_trim",          timeline_gapless_trim},

    {"seek", "suffix_match",      seek_suffix_match},
    {"seek", "bounds_and_eof",    seek_bounds_and_eof},
    {"seek", "recovery_window",   seek_recovery_window},
    {"seek", "interleaved_sessions", seek_interleaved_sessions},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
