/* Half sample-rate preview tests.
 *
 * No MINIMP3_IMPLEMENTATION here: the implementation is linked from task_support/codec.c.
 * Cases are registered in three groups (synthesis, timeline, compatibility) and driven by
 * task_support/driver.c. Numeric comparisons target real PCM: integer output tolerates at
 * most 4 LSB per sample with an RMS below one LSB, float output an absolute error <= 1e-5.
 */
#include "check.h"
#include "codec.h"
#include "minimp3_ex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define VEC_SI_BLOCK   "vectors/l3-si_block.bit"
#define VEC_COMPL      "vectors/l3-compl.bit"
#define VEC_M2_NOISE   "vectors/M2L3_noise.bit"
#define VEC_SIN1K      "vectors/l3-sin1k0db.bit"
#define HR_FLAGS       (MP3D_SEEK_TO_SAMPLE | MP3D_HALF_RATE)
#define MAX_CAP        (MINIMP3_MAX_SAMPLES_PER_FRAME)

static uint8_t *load_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long z = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)z);
    if (fread(b, 1, (size_t)z, f) != (size_t)z)
    {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)z;
    return b;
}

struct frame_span
{
    size_t byte_offset;   /* offset of the compressed frame in the source buffer */
    size_t frame_bytes;   /* compressed bytes consumed by the frame */
    size_t sample_index;  /* first interleaved scalar of this frame in native PCM */
    int    samples;       /* interleaved scalar count of this frame */
    int    channels, hz;
};

struct native_track
{
    mp3d_sample_t *pcm;
    size_t         samples;
    size_t         nframes;
    struct frame_span *frames;
    int            channels, hz;
};

static void free_native_track(struct native_track *t)
{
    free(t->pcm);
    free(t->frames);
    memset(t, 0, sizeof(*t));
}

/* Full frame-by-frame reference decode, keeping every compressed frame's byte consumption
   and its interleaved sample position, so half-rate output can be checked against the
   unmodified native entry point rather than a re-derived expectation. */
static int build_native_track(const uint8_t *buf, size_t size, struct native_track *t)
{
    memset(t, 0, sizeof(*t));
    size_t frame_cap = 4096, pcm_cap = 65536;
    t->frames = (struct frame_span *)malloc(frame_cap*sizeof(*t->frames));
    t->pcm = (mp3d_sample_t *)malloc(pcm_cap*sizeof(mp3d_sample_t));
    mp3dec_t dec;
    mp3dec_init(&dec);
    const uint8_t *p = buf;
    size_t left = size;
    while (left > 4)
    {
        mp3dec_frame_info_t info;
        mp3d_sample_t tmp[MAX_CAP];
        int r = mp3dec_decode_frame(&dec, p, (int)left, tmp, &info);
        size_t step = (size_t)info.frame_bytes;
        if (!step)
            break;
        if (r > 0)
        {
            if (t->nframes == frame_cap)
            {
                frame_cap *= 2;
                t->frames = (struct frame_span *)realloc(t->frames, frame_cap*sizeof(*t->frames));
            }
            struct frame_span *fs = &t->frames[t->nframes++];
            fs->byte_offset = (size_t)(p - buf);
            fs->frame_bytes = step;
            fs->sample_index = t->samples;
            fs->samples = r*info.channels;
            fs->channels = info.channels;
            fs->hz = info.hz;
            t->channels = info.channels;
            t->hz = info.hz;
            if (t->samples + (size_t)fs->samples > pcm_cap)
            {
                while (t->samples + (size_t)fs->samples > pcm_cap)
                    pcm_cap *= 2;
                t->pcm = (mp3d_sample_t *)realloc(t->pcm, pcm_cap*sizeof(mp3d_sample_t));
            }
            memcpy(t->pcm + t->samples, tmp, (size_t)fs->samples*sizeof(mp3d_sample_t));
            t->samples += (size_t)fs->samples;
        }
        p += step;
        left -= step;
    }
    return t->nframes != 0;
}

/* Whole-segment tolerance check: integer output allows at most 4 LSB on any single sample
 * while the aggregate RMS over the whole compared segment stays below one LSB; float output
 * allows an absolute error of at most 1e-5. The RMS is intentionally computed over the whole
 * segment rather than per sample, matching the conformance definition. */
static int segment_matches(const mp3d_sample_t *a, const mp3d_sample_t *b, size_t n)
{
    if (!n)
        return 1;
#ifndef MINIMP3_FLOAT_OUTPUT
    long worst = 0;
    double sse = 0.0;
    for (size_t i = 0; i < n; i++)
    {
        long d = (long)a[i] - (long)b[i];
        if (d < 0)
            d = -d;
        if (d > worst)
            worst = d;
        sse += (double)d*d;
    }
    return worst <= 4 && sqrt(sse/(double)n) < 1.0;
#else
    for (size_t i = 0; i < n; i++)
    {
        float d = a[i] - b[i];
        if (d < 0)
            d = -d;
        if (d > 1e-5f)
            return 0;
    }
    return 1;
#endif
}

/* Compare half-rate interleaved output against native samples 0,2,4... (or 1,3,5...) of the
 * same per-channel timeline, gathering every compared scalar into one contiguous buffer so
 * the RMS is measured over the full segment. */
static int half_matches_native(const struct native_track *t, const mp3d_sample_t *half,
                               size_t half_scalars, int phase)
{
    size_t ch = (size_t)t->channels;
    size_t half_frames = half_scalars/ch;
    size_t native_frames = t->samples/ch;
    if (half_frames > (native_frames + 1)/2)
        return 0;
    mp3d_sample_t *gathered = (mp3d_sample_t *)malloc(half_scalars*sizeof(mp3d_sample_t));
    if (!gathered)
        return 0;
    for (size_t k = 0; k < half_frames; k++)
    {
        size_t native_frame = 2*k + (size_t)phase;
        if (native_frame >= native_frames)
        {
            free(gathered);
            return 0;
        }
        memcpy(gathered + k*ch, t->pcm + native_frame*ch, ch*sizeof(mp3d_sample_t));
    }
    int ok = segment_matches(gathered, half, half_scalars);
    free(gathered);
    return ok;
}

/* ------------------------------------------------------------------ synthesis */

static int synth_lockstep(const char *path, int phase)
{
    size_t size;
    uint8_t *buf = load_file(path, &size);
    TASK_CHECK(buf != NULL);
    mp3dec_t full, half;
    mp3dec_init(&full);
    mp3dec_init(&half);
    const uint8_t *pf = buf, *ph = buf;
    size_t nf = size, nh = size;
    size_t frames = 0;
    while (nf > 4 && nh > 4)
    {
        mp3dec_frame_info_t inf, inh;
        mp3d_sample_t fpcm[MAX_CAP], hpcm[MAX_CAP];
        int rf = mp3dec_decode_frame(&full, pf, (int)nf, fpcm, &inf);
        int rh = mp3dec_decode_frame_half(&half, ph, (int)nh, hpcm, &inh, phase);
        TASK_CHECK(inf.frame_bytes == inh.frame_bytes);
        TASK_CHECK(inf.frame_offset == inh.frame_offset);
        TASK_CHECK(inf.channels == inh.channels);
        TASK_CHECK(inf.layer == inh.layer);
        TASK_CHECK(inf.bitrate_kbps == inh.bitrate_kbps);
        size_t step = (size_t)inf.frame_bytes;
        if (!step)
            break;
        if (rf > 0)
        {
            TASK_CHECK(rh == rf/2);
            TASK_CHECK(inh.hz == inf.hz/2);
            struct native_track one;
            memset(&one, 0, sizeof(one));
            one.pcm = fpcm;
            one.samples = (size_t)rf*inf.channels;
            one.channels = inf.channels;
            TASK_CHECK(half_matches_native(&one, hpcm, (size_t)rh*inf.channels, phase));
            frames++;
        }
        pf += step;
        nf -= step;
        ph += step;
        nh -= step;
    }
    TASK_CHECK(frames > 0);
    free(buf);
    return 0;
}

static int case_synth_mono_mpeg1(void)
{
    TASK_CHECK(synth_lockstep(VEC_COMPL, 0) == 0);
    TASK_CHECK(synth_lockstep(VEC_COMPL, 1) == 0);
    return 0;
}

static int case_synth_short_blocks_mono(void)
{   /* l3-si_block is the short/stop-block conformance vector, MPEG-1 mono */
    TASK_CHECK(synth_lockstep(VEC_SI_BLOCK, 0) == 0);
    TASK_CHECK(synth_lockstep(VEC_SI_BLOCK, 1) == 0);
    return 0;
}

static int case_synth_stereo_mpeg2(void)
{
    TASK_CHECK(synth_lockstep(VEC_M2_NOISE, 0) == 0);
    TASK_CHECK(synth_lockstep(VEC_M2_NOISE, 1) == 0);
    return 0;
}

static int case_synth_stereo_mpeg1(void)
{
    TASK_CHECK(synth_lockstep(VEC_SIN1K, 0) == 0);
    TASK_CHECK(synth_lockstep(VEC_SIN1K, 1) == 0);
    return 0;
}

/* Alternate full and half entry points on one instance and verify every emitted sample
 * against a pure full-rate reference, including a leading resync that produces no output. */
static int case_synth_alternating_resync(void)
{
    size_t size;
    uint8_t *buf = load_file(VEC_M2_NOISE, &size);
    TASK_CHECK(buf != NULL);
    struct native_track ref;
    TASK_CHECK(build_native_track(buf, size, &ref));

    for (int phase = 0; phase < 2; phase++)
    {
        mp3dec_t dec;
        mp3dec_init(&dec);
        mp3dec_frame_info_t info;
        mp3d_sample_t out[MAX_CAP];

        /* a junk-only call resyncs with no output and reports the consumed garbage bytes */
        uint8_t junk[16];
        memset(junk, 0, sizeof(junk));
        int rj = mp3dec_decode_frame_half(&dec, junk, (int)sizeof(junk), out, &info, phase);
        TASK_CHECK(rj == 0);
        TASK_CHECK((size_t)info.frame_bytes == sizeof(junk));

        const uint8_t *p = buf;
        size_t left = size;
        long src_frame = 0;
        size_t call = 0;
        size_t got_scalars = 0;
        mp3d_sample_t *actual = (mp3d_sample_t *)malloc(ref.samples*sizeof(mp3d_sample_t));
        mp3d_sample_t *expect = (mp3d_sample_t *)malloc(ref.samples*sizeof(mp3d_sample_t));
        TASK_CHECK(actual && expect);
        while (left > 4)
        {
            mp3d_sample_t tmp[MAX_CAP];
            int use_half = (call & 1);
            int r;
            if (use_half)
                r = mp3dec_decode_frame_half(&dec, p, (int)left, tmp, &info, phase);
            else
                r = mp3dec_decode_frame(&dec, p, (int)left, tmp, &info);
            size_t step = (size_t)info.frame_bytes;
            if (!step)
                break;
            if (r > 0)
            {
                int ch = info.channels;
                int per = r; /* per-channel frames this call */
                for (int s = 0; s < per; s++)
                {
                    long native_frame = use_half ? (src_frame + 2*s + phase) : (src_frame + s);
                    memcpy(expect + got_scalars, ref.pcm + native_frame*ch, (size_t)ch*sizeof(mp3d_sample_t));
                    memcpy(actual + got_scalars, tmp + s*ch, (size_t)ch*sizeof(mp3d_sample_t));
                    got_scalars += (size_t)ch;
                }
                src_frame += use_half ? 2*r : r;
            }
            p += step;
            left -= step;
            call++;
        }
        TASK_CHECK(call > 4);
        TASK_CHECK(got_scalars > 0);
        /* one aggregate comparison so the RMS is measured over the whole interleaved segment */
        TASK_CHECK(segment_matches(expect, actual, got_scalars));
        free(actual);
        free(expect);
    }
    free_native_track(&ref);
    free(buf);
    return 0;
}

/* The skipped phase only drops final QMF weighting: DCT/IMDCT, reservoir and QMF history
 * must advance identically, and the executed weighting work must stay under 60%. */
static int case_synth_history_and_work(void)
{
    size_t size;
    uint8_t *buf = load_file(VEC_SIN1K, &size);
    TASK_CHECK(buf != NULL);
    size_t segment_frames = 40;
    for (int phase = 0; phase < 2; phase++)
    {
        mp3dec_t df, dh;
        mp3dec_init(&df);
        mp3dec_init(&dh);
        const uint8_t *pf = buf, *ph = buf;
        size_t nf = size, nh = size;
        size_t done = 0;
        uint64_t work_full = 0, work_half = 0;
        while (done < segment_frames && nf > 4 && nh > 4)
        {
            mp3dec_frame_info_t inf, inh;
            mp3d_sample_t fa[MAX_CAP], ha[MAX_CAP];
            task_synth_work = 0;
            int rf = mp3dec_decode_frame(&df, pf, (int)nf, fa, &inf);
            work_full += task_synth_work;
            task_synth_work = 0;
            int rh = mp3dec_decode_frame_half(&dh, ph, (int)nh, ha, &inh, phase);
            work_half += task_synth_work;
            TASK_CHECK(inf.frame_bytes == inh.frame_bytes);
            if (rf > 0)
            {   /* internal state must be byte-identical: catches any skipped history write */
                TASK_CHECK(rh == rf/2);
                TASK_CHECK(memcmp(df.qmf_state, dh.qmf_state, sizeof(df.qmf_state)) == 0);
                TASK_CHECK(memcmp(df.mdct_overlap, dh.mdct_overlap, sizeof(df.mdct_overlap)) == 0);
                TASK_CHECK(memcmp(df.reserv_buf, dh.reserv_buf, sizeof(df.reserv_buf)) == 0);
                TASK_CHECK(df.reserv == dh.reserv);
                done++;
            }
            size_t step = (size_t)inf.frame_bytes;
            pf += step;
            nf -= step;
            ph += step;
            nh -= step;
        }
        TASK_CHECK(done == segment_frames);
        TASK_CHECK(work_full > 0);
        TASK_CHECK(work_half > 0);
        TASK_CHECK(work_half*100 <= work_full*60); /* at most 60% of the full-rate work */
    }
    free(buf);
    return 0;
}

static int case_synth_invalid_phase(void)
{
    size_t size;
    uint8_t *buf = load_file(VEC_COMPL, &size);
    TASK_CHECK(buf != NULL);
    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_t saved;
    memcpy(&saved, &dec, sizeof(saved));
    mp3dec_frame_info_t info;
    mp3d_sample_t out[MAX_CAP];
    int bad_phase[] = { -1, 2, 3, 1000 };
    for (size_t i = 0; i < sizeof(bad_phase)/sizeof(bad_phase[0]); i++)
    {
        memset(&info, 0x55, sizeof(info));
        int r = mp3dec_decode_frame_half(&dec, buf, (int)size, out, &info, bad_phase[i]);
        TASK_CHECK(r == 0);
        TASK_CHECK(info.frame_bytes == 0 && info.hz == 0 && info.channels == 0);
        TASK_CHECK(memcmp(&dec, &saved, sizeof(dec)) == 0); /* instance not advanced */
    }
    /* the instance still decodes normally afterwards */
    int r = mp3dec_decode_frame_half(&dec, buf, (int)size, out, &info, 0);
    TASK_CHECK(r > 0);
    TASK_CHECK(info.hz > 0);
    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ timeline */

struct first_frame { const uint8_t *hdr; int frame_size; int mpeg1; int mono; };

static int collect_first_frame(void *user, const uint8_t *frame, int frame_size, int free_format_bytes,
                               size_t buf_size, uint64_t offset, mp3dec_frame_info_t *info)
{
    (void)free_format_bytes;
    (void)buf_size;
    (void)offset;
    (void)info;
    struct first_frame *ff = (struct first_frame *)user;
    if (!ff->hdr)
    {
        ff->hdr = frame;
        ff->frame_size = frame_size;
        ff->mpeg1 = (frame[1] & 0x08) != 0;
        ff->mono = ((frame[3] & 0xC0) == 0xC0);
    }
    return 1; /* stop after the first frame */
}

struct frame_counter { size_t count; };
static int count_frame(void *user, const uint8_t *frame, int frame_size, int free_format_bytes,
                       size_t buf_size, uint64_t offset, mp3dec_frame_info_t *info)
{
    (void)frame;
    (void)frame_size;
    (void)free_format_bytes;
    (void)buf_size;
    (void)offset;
    (void)info;
    ((struct frame_counter *)user)->count++;
    return 0;
}

/* Build a valid Xing frame (with an optional LAME extension carrying delay/padding) in front
 * of real audio. The tag frame itself is not audio. */
static uint8_t *build_tagged_stream(const uint8_t *audio, size_t audio_size,
                                    int delay, int padding, size_t *out_size)
{
    struct first_frame ff;
    memset(&ff, 0, sizeof(ff));
    mp3dec_iterate_buf(audio, audio_size, collect_first_frame, &ff);
    if (!ff.hdr)
        return NULL;
    uint8_t *tag = (uint8_t *)calloc(1, (size_t)ff.frame_size);
    memcpy(tag, ff.hdr, 4);
    int side = ff.mpeg1 ? (ff.mono ? 17 : 32) : (ff.mono ? 9 : 17);
    int off = 4 + side;
    memcpy(tag + off, "Xing", 4);
    tag[off + 7] = 1; /* FRAMES_FLAG */
    struct frame_counter fc;
    memset(&fc, 0, sizeof(fc));
    mp3dec_iterate_buf(audio, audio_size, count_frame, &fc);
    uint32_t nframes = (uint32_t)fc.count;
    tag[off + 8] = (uint8_t)(nframes >> 24);
    tag[off + 9] = (uint8_t)(nframes >> 16);
    tag[off + 10] = (uint8_t)(nframes >> 8);
    tag[off + 11] = (uint8_t)nframes;
    if (delay || padding)
    {   /* LAME extension: the library reconstructs delay/padding as stored + 529, so the
           raw 12-bit fields are the requested values minus the 529-sample base */
        int b = off + 12;
        tag[b] = 'L';
        int d = delay - (528 + 1);
        int p = padding + (528 + 1);
        tag[b + 21] = (uint8_t)((d >> 4) & 0xFF);
        tag[b + 22] = (uint8_t)(((d & 15) << 4) | ((p >> 8) & 15));
        tag[b + 23] = (uint8_t)(p & 0xFF);
    }
    uint8_t *whole = (uint8_t *)malloc((size_t)ff.frame_size + audio_size);
    memcpy(whole, tag, (size_t)ff.frame_size);
    memcpy(whole + ff.frame_size, audio, audio_size);
    *out_size = (size_t)ff.frame_size + audio_size;
    free(tag);
    return whole;
}

struct tag_case { const char *path; int delay; int padding; };

static int run_tagged_case(const struct tag_case *tc)
{
    size_t audio_size;
    uint8_t *audio = load_file(tc->path, &audio_size);
    TASK_CHECK(audio != NULL);
    size_t whole_size;
    uint8_t *whole = build_tagged_stream(audio, audio_size, tc->delay, tc->padding, &whole_size);
    TASK_CHECK(whole != NULL);

    mp3dec_t refdec;
    mp3dec_file_info_t ref;
    memset(&ref, 0, sizeof(ref));
    TASK_CHECK(mp3dec_load_buf(&refdec, whole, whole_size, &ref, NULL, NULL) == 0);
    TASK_CHECK(ref.samples > 0);

    mp3dec_ex_t ex;
    TASK_CHECK(mp3dec_ex_open_buf(&ex, whole, whole_size, HR_FLAGS) == 0);
    TASK_CHECK(ex.vbr_tag_found == 1);
    TASK_CHECK(ex.info.hz == ref.hz/2);
    TASK_CHECK(ex.info.channels == ref.channels);
    size_t ch = (size_t)ref.channels;
    size_t native_frames = ref.samples/ch;
    size_t expect = ((native_frames + 1)/2)*ch;
    TASK_CHECK(ex.samples == expect);
    TASK_CHECK((size_t)ex.start_delay == (size_t)tc->delay*(int)ch || tc->delay == 0);
    TASK_CHECK(ex.half_phase == (tc->delay & 1));

    mp3d_sample_t *out = (mp3d_sample_t *)malloc(expect*sizeof(mp3d_sample_t));
    size_t got = mp3dec_ex_read(&ex, out, expect);
    TASK_CHECK(got == expect);
    /* the trimmed half-rate stream is always the trimmed native PCM's even per-channel
       frames 0,2,4...; half_phase only selects the frame-local parity inside the reader */
    TASK_CHECK(half_matches_native(&(struct native_track){ ref.buffer, ref.samples, 0, NULL, ref.channels, ref.hz },
                                   out, got, 0));
    TASK_CHECK(ex.cur_sample == expect);
    mp3dec_ex_close(&ex);
    free(out);
    free(ref.buffer);
    free(whole);
    free(audio);
    return 0;
}

static int case_timeline_even_delay(void)
{
    struct tag_case tc = { VEC_SI_BLOCK, 530, 0 };
    return run_tagged_case(&tc);
}

static int case_timeline_odd_delay(void)
{
    struct tag_case tc = { VEC_M2_NOISE, 529, 0 };
    return run_tagged_case(&tc);
}

static int case_timeline_odd_length_and_padding(void)
{   /* nonzero padding trims the tail; odd native remainder must round up via ceil(N/2) */
    struct tag_case tc = { VEC_COMPL, 529, 7 };
    return run_tagged_case(&tc);
}

static int case_timeline_block_vs_chunk_reads(void)
{
    size_t audio_size;
    uint8_t *audio = load_file(VEC_M2_NOISE, &audio_size);
    TASK_CHECK(audio != NULL);
    size_t whole_size;
    uint8_t *whole = build_tagged_stream(audio, audio_size, 529, 3, &whole_size);
    TASK_CHECK(whole != NULL);

    mp3dec_ex_t bulk;
    TASK_CHECK(mp3dec_ex_open_buf(&bulk, whole, whole_size, HR_FLAGS) == 0);
    mp3d_sample_t *big = (mp3d_sample_t *)malloc(bulk.samples*sizeof(mp3d_sample_t));
    TASK_CHECK(mp3dec_ex_read(&bulk, big, bulk.samples) == bulk.samples);

    mp3dec_ex_t small;
    TASK_CHECK(mp3dec_ex_open_buf(&small, whole, whole_size, HR_FLAGS) == 0);
    TASK_CHECK(small.samples == bulk.samples);
    mp3d_sample_t *lit = (mp3d_sample_t *)malloc(small.samples*sizeof(mp3d_sample_t));
    size_t pos = 0;
    size_t ch = (size_t)small.info.channels;
    while (pos < small.samples)
    {   /* deliberately small, channel-aligned chunks */
        size_t want = (2*ch + (pos % (6*ch)));
        if (want > small.samples - pos)
            want = small.samples - pos;
        size_t g = mp3dec_ex_read(&small, lit + pos, want);
        TASK_CHECK(g > 0);
        pos += g;
    }
    TASK_CHECK(pos == bulk.samples);
    TASK_CHECK(memcmp(lit, big, pos*sizeof(mp3d_sample_t)) == 0);
    mp3dec_ex_close(&bulk);
    mp3dec_ex_close(&small);
    free(big);
    free(lit);
    free(whole);
    free(audio);
    return 0;
}

/* Seeking inside and across compressed frames must reproduce the from-start half stream. */
static int case_timeline_seek_suffix(void)
{
    size_t audio_size;
    uint8_t *audio = load_file(VEC_SIN1K, &audio_size);
    TASK_CHECK(audio != NULL);
    size_t whole_size;
    uint8_t *whole = build_tagged_stream(audio, audio_size, 529, 0, &whole_size);
    TASK_CHECK(whole != NULL);

    mp3dec_ex_t ref;
    TASK_CHECK(mp3dec_ex_open_buf(&ref, whole, whole_size, HR_FLAGS) == 0);
    mp3d_sample_t *full = (mp3d_sample_t *)malloc(ref.samples*sizeof(mp3d_sample_t));
    TASK_CHECK(mp3dec_ex_read(&ref, full, ref.samples) == ref.samples);

    mp3dec_ex_t ex;
    TASK_CHECK(mp3dec_ex_open_buf(&ex, whole, whole_size, HR_FLAGS) == 0);
    size_t ch = (size_t)ex.info.channels;
    uint64_t targets[] = {
        0,
        (ref.samples/5) & ~((uint64_t)ch - 1),
        (ref.samples/2) & ~((uint64_t)ch - 1),
        ((ref.samples/ch*2)/3)*ch,
        ref.samples - ch
    };
    mp3d_sample_t *tail = (mp3d_sample_t *)malloc(ref.samples*sizeof(mp3d_sample_t));
    for (size_t i = 0; i < sizeof(targets)/sizeof(targets[0]); i++)
    {
        uint64_t T = targets[i];
        TASK_CHECK(mp3dec_ex_seek(&ex, T) == 0);
        TASK_CHECK(ex.cur_sample == T);
        size_t g = mp3dec_ex_read(&ex, tail, ref.samples);
        TASK_CHECK(g == ref.samples - T);
        TASK_CHECK(memcmp(tail, full + T, g*sizeof(mp3d_sample_t)) == 0);
        /* seekable again after reaching EOF */
        TASK_CHECK(mp3dec_ex_seek(&ex, 0) == 0);
    }
    mp3dec_ex_close(&ref);
    mp3dec_ex_close(&ex);
    free(full);
    free(tail);
    free(whole);
    free(audio);
    return 0;
}

static int case_timeline_clamp_and_alignment(void)
{
    size_t audio_size;
    uint8_t *audio = load_file(VEC_M2_NOISE, &audio_size);
    TASK_CHECK(audio != NULL);
    size_t whole_size;
    uint8_t *whole = build_tagged_stream(audio, audio_size, 530, 1, &whole_size);
    TASK_CHECK(whole != NULL);

    mp3dec_ex_t ex;
    TASK_CHECK(mp3dec_ex_open_buf(&ex, whole, whole_size, HR_FLAGS) == 0);
    size_t total = ex.samples;
    /* past-the-end target clamps to the end */
    TASK_CHECK(mp3dec_ex_seek(&ex, total + 1000*ex.info.channels) == 0);
    TASK_CHECK(ex.cur_sample == total);
    mp3d_sample_t tmp[64];
    TASK_CHECK(mp3dec_ex_read(&ex, tmp, sizeof(tmp)/sizeof(tmp[0])) == 0);

    if (ex.info.channels == 2)
    {   /* misaligned target is rejected and changes nothing */
        size_t ch2 = (size_t)ex.info.channels;
        uint64_t aligned_mid = ((total/2)/ch2)*ch2;
        TASK_CHECK(mp3dec_ex_seek(&ex, aligned_mid) == 0);
        uint64_t saved = ex.cur_sample;
        TASK_CHECK(mp3dec_ex_seek(&ex, saved + 1) == MP3D_E_PARAM);
        TASK_CHECK(ex.cur_sample == saved);

        /* non-channel-multiple capacity is a param error and preserves the field */
        size_t before = ex.cur_sample;
        TASK_CHECK(mp3dec_ex_read(&ex, tmp, 3) == 0);
        TASK_CHECK(ex.last_error == MP3D_E_PARAM);
        TASK_CHECK(ex.cur_sample == before);
        ex.last_error = 0;
        /* zero capacity does not advance anything */
        TASK_CHECK(mp3dec_ex_read(&ex, tmp, 0) == 0);
        TASK_CHECK(ex.cur_sample == before);
        /* a good aligned read still works after the rejected calls */
        size_t g = mp3dec_ex_read(&ex, tmp, 4);
        TASK_CHECK(g == 4);
        TASK_CHECK(ex.cur_sample == before + 4);
    }
    mp3dec_ex_close(&ex);
    free(whole);
    free(audio);
    return 0;
}

/* ------------------------------------------------------------------ compatibility */

#ifndef MINIMP3_FLOAT_OUTPUT
static int case_compat_reference_pcm(void)
{   /* native entry point must still match the shipped real-PCM conformance references.
       The ISO references may be a frame or two shorter than an untagged raw decode, so the
       comparison covers the common prefix like the upstream conformance harness. */
    struct { const char *bit; const char *pcm; } refs[] = {
        { VEC_SI_BLOCK, "vectors/l3-si_block.pcm" },
        { VEC_COMPL,    "vectors/l3-compl.pcm" },
        { VEC_M2_NOISE, "vectors/M2L3_noise.pcm" },
        { VEC_SIN1K,    "vectors/l3-sin1k0db.pcm" },
    };
    for (size_t i = 0; i < sizeof(refs)/sizeof(refs[0]); i++)
    {
        size_t bit_size, pcm_size;
        uint8_t *bit = load_file(refs[i].bit, &bit_size);
        uint8_t *pcm = load_file(refs[i].pcm, &pcm_size);
        TASK_CHECK(bit && pcm);
        mp3dec_t dec;
        mp3dec_file_info_t info;
        memset(&info, 0, sizeof(info));
        TASK_CHECK(mp3dec_load_buf(&dec, bit, bit_size, &info, NULL, NULL) == 0);
        size_t ref_samples = pcm_size/sizeof(int16_t);
        TASK_CHECK(info.samples > 0 && ref_samples > 0);
        size_t common = info.samples < ref_samples ? info.samples : ref_samples;
        const int16_t *ref16 = (const int16_t *)pcm;
        long worst = 0;
        for (size_t k = 0; k < common; k++)
        {
            long d = (long)info.buffer[k] - (long)ref16[k];
            if (d < 0)
                d = -d;
            if (d > worst)
                worst = d;
        }
        TASK_CHECK(worst <= 1); /* upstream conformance tolerance */
        free(info.buffer);
        free(bit);
        free(pcm);
    }
    return 0;
}
#endif

static int case_compat_flag_rejection(void)
{
    size_t size;
    uint8_t *buf = load_file(VEC_COMPL, &size);
    TASK_CHECK(buf != NULL);
    mp3dec_ex_t ex;
    TASK_CHECK(mp3dec_ex_open_buf(&ex, buf, size, MP3D_HALF_RATE) == MP3D_E_PARAM);
    TASK_CHECK(mp3dec_ex_open_buf(&ex, buf, size, MP3D_HALF_RATE | MP3D_SEEK_TO_BYTE) == MP3D_E_PARAM);
    TASK_CHECK(mp3dec_ex_open_buf(&ex, buf, size, MP3D_HALF_RATE | MP3D_SEEK_TO_SAMPLE | MP3D_DO_NOT_SCAN) == MP3D_E_PARAM);
    TASK_CHECK(mp3dec_ex_open_buf(&ex, buf, size, MP3D_HALF_RATE | MP3D_SEEK_TO_SAMPLE | 0x10) == MP3D_E_PARAM);
    /* ordinary full-rate opens keep working with the old flags */
    TASK_CHECK(mp3dec_ex_open_buf(&ex, buf, size, MP3D_SEEK_TO_SAMPLE) == 0);
    TASK_CHECK(ex.half_rate == 0);
    TASK_CHECK(ex.info.hz > 0);
    mp3dec_ex_close(&ex);
    free(buf);
    return 0;
}

struct mem_io { const uint8_t *data; size_t size, pos; };
static size_t mem_read(void *dst, size_t size, void *user)
{
    struct mem_io *m = (struct mem_io *)user;
    size_t avail = m->size - m->pos;
    if (size > avail)
        size = avail;
    memcpy(dst, m->data + m->pos, size);
    m->pos += size;
    return size;
}
static int mem_seek(uint64_t position, void *user)
{
    struct mem_io *m = (struct mem_io *)user;
    if (position > m->size)
        return 1;
    m->pos = (size_t)position;
    return 0;
}

static int case_compat_callback_file_dual(void)
{
    size_t audio_size;
    uint8_t *audio = load_file(VEC_M2_NOISE, &audio_size);
    TASK_CHECK(audio != NULL);
    size_t whole_size;
    uint8_t *whole = build_tagged_stream(audio, audio_size, 529, 0, &whole_size);
    TASK_CHECK(whole != NULL);

    mp3dec_ex_t ref;
    TASK_CHECK(mp3dec_ex_open_buf(&ref, whole, whole_size, HR_FLAGS) == 0);
    mp3d_sample_t *refpcm = (mp3d_sample_t *)malloc(ref.samples*sizeof(mp3d_sample_t));
    TASK_CHECK(mp3dec_ex_read(&ref, refpcm, ref.samples) == ref.samples);

    /* callback IO instance */
    struct mem_io mio;
    mio.data = whole;
    mio.size = whole_size;
    mio.pos = 0;
    mp3dec_io_t io;
    io.read = mem_read;
    io.read_data = &mio;
    io.seek = mem_seek;
    io.seek_data = &mio;
    mp3dec_ex_t cb;
    TASK_CHECK(mp3dec_ex_open_cb(&cb, &io, HR_FLAGS) == 0);
    TASK_CHECK(cb.samples == ref.samples);
    mp3d_sample_t *cbpcm = (mp3d_sample_t *)malloc(cb.samples*sizeof(mp3d_sample_t));
    size_t pos = 0;
    while (pos < cb.samples)
    {
        size_t want = 1024 < cb.samples - pos ? 1024 : cb.samples - pos;
        size_t g = mp3dec_ex_read(&cb, cbpcm + pos, want);
        TASK_CHECK(g > 0);
        pos += g;
    }
    TASK_CHECK(pos == ref.samples);
    TASK_CHECK(memcmp(cbpcm, refpcm, pos*sizeof(mp3d_sample_t)) == 0);

    /* stdio file entry */
    const char *tmpdir = getenv("TMPDIR");
    char path[300];
    snprintf(path, sizeof(path), "%s/hr_compat.mp3", tmpdir ? tmpdir : "/tmp");
    FILE *f = fopen(path, "wb");
    TASK_CHECK(f != NULL);
    TASK_CHECK(fwrite(whole, 1, whole_size, f) == whole_size);
    fclose(f);
    mp3dec_ex_t fl;
    TASK_CHECK(mp3dec_ex_open(&fl, path, HR_FLAGS) == 0);
    TASK_CHECK(fl.samples == ref.samples);
    mp3d_sample_t *flpcm = (mp3d_sample_t *)malloc(fl.samples*sizeof(mp3d_sample_t));
    TASK_CHECK(mp3dec_ex_read(&fl, flpcm, fl.samples) == fl.samples);
    TASK_CHECK(memcmp(flpcm, refpcm, fl.samples*sizeof(mp3d_sample_t)) == 0);
    remove(path);

    /* two instances keep independent cursors when interleaved */
    mp3dec_ex_t a, b;
    TASK_CHECK(mp3dec_ex_open_buf(&a, whole, whole_size, HR_FLAGS) == 0);
    TASK_CHECK(mp3dec_ex_open_buf(&b, whole, whole_size, HR_FLAGS) == 0);
    mp3d_sample_t xa[256], xb[128];
    TASK_CHECK(mp3dec_ex_read(&a, xa, 256) == 256);
    TASK_CHECK(mp3dec_ex_read(&b, xb, 128) == 128);
    TASK_CHECK(a.cur_sample == 256 && b.cur_sample == 128);
    TASK_CHECK(memcmp(xa, refpcm, 256*sizeof(mp3d_sample_t)) == 0);
    TASK_CHECK(memcmp(xb, refpcm, 128*sizeof(mp3d_sample_t)) == 0);
    TASK_CHECK(mp3dec_ex_read(&b, xb, 128) == 128);
    TASK_CHECK(b.cur_sample == 256);
    TASK_CHECK(memcmp(xb, refpcm + 128, 128*sizeof(mp3d_sample_t)) == 0);

    mp3dec_ex_close(&ref);
    mp3dec_ex_close(&cb);
    mp3dec_ex_close(&fl);
    mp3dec_ex_close(&a);
    mp3dec_ex_close(&b);
    free(refpcm);
    free(cbpcm);
    free(flpcm);
    free(whole);
    free(audio);
    return 0;
}

static int case_compat_reject_then_continue(void)
{   /* a rejected read leaves the field intact and the next read continues correctly */
    size_t audio_size;
    uint8_t *audio = load_file(VEC_COMPL, &audio_size);
    TASK_CHECK(audio != NULL);
    size_t whole_size;
    uint8_t *whole = build_tagged_stream(audio, audio_size, 530, 0, &whole_size);
    TASK_CHECK(whole != NULL);

    mp3dec_ex_t ref, ex;
    TASK_CHECK(mp3dec_ex_open_buf(&ref, whole, whole_size, HR_FLAGS) == 0);
    TASK_CHECK(mp3dec_ex_open_buf(&ex, whole, whole_size, HR_FLAGS) == 0);
    mp3d_sample_t good[128];
    TASK_CHECK(mp3dec_ex_read(&ex, good, 128) == 128);
    uint64_t saved = ex.cur_sample;
    /* mono is always aligned; force the param error through a NULL buffer instead */
    TASK_CHECK(mp3dec_ex_read(&ex, NULL, 128) == 0);
    TASK_CHECK(ex.last_error == MP3D_E_PARAM);
    TASK_CHECK(ex.cur_sample == saved);
    ex.last_error = 0;
    mp3d_sample_t more[128];
    TASK_CHECK(mp3dec_ex_read(&ex, more, 128) == 128);
    mp3d_sample_t refbuf[256];
    TASK_CHECK(mp3dec_ex_seek(&ref, 0) == 0);
    TASK_CHECK(mp3dec_ex_read(&ref, refbuf, 256) == 256);
    TASK_CHECK(memcmp(more, refbuf + 128, 128*sizeof(mp3d_sample_t)) == 0);
    mp3dec_ex_close(&ref);
    mp3dec_ex_close(&ex);
    free(whole);
    free(audio);
    return 0;
}

const struct task_case task_cases[] = {
    { "synthesis", "mono_mpeg1",            case_synth_mono_mpeg1 },
    { "synthesis", "short_blocks_mono",     case_synth_short_blocks_mono },
    { "synthesis", "stereo_mpeg2",          case_synth_stereo_mpeg2 },
    { "synthesis", "stereo_mpeg1",          case_synth_stereo_mpeg1 },
    { "synthesis", "alternating_resync",    case_synth_alternating_resync },
    { "synthesis", "history_and_work",      case_synth_history_and_work },
    { "synthesis", "invalid_phase",         case_synth_invalid_phase },
    { "timeline",  "even_delay",            case_timeline_even_delay },
    { "timeline",  "odd_delay",             case_timeline_odd_delay },
    { "timeline",  "odd_length_padding",    case_timeline_odd_length_and_padding },
    { "timeline",  "block_vs_chunk_reads",  case_timeline_block_vs_chunk_reads },
    { "timeline",  "seek_suffix",           case_timeline_seek_suffix },
    { "timeline",  "clamp_alignment",       case_timeline_clamp_and_alignment },
#ifndef MINIMP3_FLOAT_OUTPUT
    { "compatibility", "reference_pcm",        case_compat_reference_pcm },
#endif
    { "compatibility", "flag_rejection",       case_compat_flag_rejection },
    { "compatibility", "callback_file_dual",   case_compat_callback_file_dual },
    { "compatibility", "reject_then_continue", case_compat_reject_then_continue },
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
