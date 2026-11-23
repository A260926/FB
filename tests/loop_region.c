/*
 * Finite loop region tests for SDL_sound.
 *
 * All WAV fixtures (16-bit PCM and Microsoft ADPCM, mono and stereo) are
 * synthesized by hand here, every expected PCM frame is produced by an
 * independent decoder/expansion, and results are compared through
 * TASK_CHECK. There is no main(): task_support/driver.c drives the cases.
 */

#include <SDL3/SDL.h>
#include <SDL3_sound/SDL_sound.h>
#include "../src/SDL_sound_loop.h"
#include "task_support/check.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t  s16;
typedef int32_t  s32;

/* ... builders ............................................................ */

static void put16(u8 *p, unsigned v) { p[0] = (u8) v; p[1] = (u8) (v >> 8); }
static void put32(u8 *p, unsigned v) { put16(p, v); put16(p + 2, v >> 16); }

/* Deterministic, deliberately asymmetric channel waveforms. */
static s16 wave_l(unsigned i) { return (s16) (((i * 367) % 24000) - 12000); }
static s16 wave_r(unsigned i) { return (s16) (((i * 719) % 20000) - 10000); }
static s16 wave_m(unsigned i) { return (s16) (((i * 521) % 22000) - 11000); }

/* Hand-built 16-bit PCM RIFF/WAVE. Returns total file size. */
static size_t build_pcm(u8 *p, unsigned frames, unsigned rate, int channels)
{
    const unsigned frame_bytes = 2u * (unsigned) channels;
    const size_t z = 44 + (size_t) frames * frame_bytes;

    memset(p, 0, z);
    memcpy(p, "RIFF", 4); put32(p + 4, (unsigned) (z - 8));
    memcpy(p + 8, "WAVEfmt ", 8);
    put32(p + 16, 16);
    put16(p + 20, 1);                       /* PCM */
    put16(p + 22, (unsigned) channels);
    put32(p + 24, rate);
    put32(p + 28, rate * frame_bytes);
    put16(p + 32, frame_bytes);
    put16(p + 34, 16);
    memcpy(p + 36, "data", 4);
    put32(p + 40, frames * frame_bytes);

    for (unsigned i = 0; i < frames; i++)
    {
        u8 *d = p + 44 + (size_t) i * frame_bytes;
        put16(d, (u16) (channels == 2 ? wave_l(i) : wave_m(i)));
        if (channels == 2)
            put16(d + 2, (u16) wave_r(i));
    }
    return z;
}

/* Hand-built 8-bit unsigned PCM WAV: a valid WAV the loop layer must reject. */
static size_t build_u8(u8 *p, unsigned frames, unsigned rate)
{
    const size_t z = 44 + frames;
    memset(p, 0, z);
    memcpy(p, "RIFF", 4); put32(p + 4, (unsigned) (z - 8));
    memcpy(p + 8, "WAVEfmt ", 8);
    put32(p + 16, 16); put16(p + 20, 1); put16(p + 22, 1);
    put32(p + 24, rate); put32(p + 28, rate); put16(p + 32, 1); put16(p + 34, 8);
    memcpy(p + 36, "data", 4); put32(p + 40, frames);
    for (unsigned i = 0; i < frames; i++)
        p[44 + i] = (u8) ((i * 37) & 0xFF);
    return z;
}

static const s32 adpcm_coef[7][2] = {
    { 256, 0 }, { 512, -256 }, { 0, 0 }, { 192, 64 },
    { 240, 0 }, { 460, -208 }, { 392, -232 }
};
#define ADPCM_SPB 20

/* Hand-built Microsoft ADPCM WAV (mono block align 16, stereo 32). */
static size_t build_adpcm(u8 *p, unsigned blocks, unsigned rate, int channels)
{
    const unsigned ba = channels == 2 ? 32u : 16u;
    const unsigned header_bytes = (unsigned) channels * 7u;
    const unsigned nibble_bytes = ba - header_bytes;
    const size_t z = 90 + (size_t) blocks * ba;

    memset(p, 0, z);
    memcpy(p, "RIFF", 4); put32(p + 4, (unsigned) (z - 8));
    memcpy(p + 8, "WAVEfmt ", 8);
    put32(p + 16, 50);
    put16(p + 20, 2);                        /* ADPCM */
    put16(p + 22, (unsigned) channels);
    put32(p + 24, rate);
    put32(p + 28, rate * ba / ADPCM_SPB);
    put16(p + 32, ba);
    put16(p + 34, 4);
    put16(p + 36, 32);                       /* cbSize */
    put16(p + 38, ADPCM_SPB);
    put16(p + 40, 7);                        /* coefficients */
    for (int i = 0; i < 7; i++)
    {
        put16(p + 42 + i * 4, (u16) adpcm_coef[i][0]);
        put16(p + 44 + i * 4, (u16) adpcm_coef[i][1]);
    }
    memcpy(p + 70, "fact", 4);
    put32(p + 74, 4);
    put32(p + 78, blocks * ADPCM_SPB);
    memcpy(p + 82, "data", 4);
    put32(p + 86, blocks * ba);

    for (unsigned k = 0; k < blocks; k++)
    {
        u8 *v = p + 90 + (size_t) k * ba;
        for (int c = 0; c < channels; c++)
        {
            v[c] = (u8) ((k + c) % 7);                  /* predictor */
            put16(v + channels + c * 2, 64 + k + c);    /* delta */
            put16(v + 3 * channels + c * 2,
                  (u16) (1000 + (s32) k * 31 - c * 50)); /* iSamp1 */
            put16(v + 5 * channels + c * 2,
                  (u16) (-900 - (s32) k * 27 + c * 40)); /* iSamp2 */
        }
        for (unsigned i = 0; i < nibble_bytes; i++)
            v[header_bytes + i] = (u8) (((i + k) % 16) * 16
                                        + ((15 - i + k) % 16));
    }
    return z;
}

/* ... independent Microsoft ADPCM reference decoder ....................... */

static const s32 adapt_table[16] = {
    230, 230, 230, 230, 307, 409, 512, 614,
    768, 614, 512, 409, 307, 230, 230, 230
};

typedef struct
{
    s32 delta;
    s32 samp1, samp2;
    int predictor;
} AdpcmChan;

static void adpcm_nibble(u8 nib, AdpcmChan *ch)
{
    const s32 pred = (ch->samp1 * adpcm_coef[ch->predictor][0]
                      + ch->samp2 * adpcm_coef[ch->predictor][1]) / 256;
    s32 ns = nib & 0x08 ? pred + ch->delta * ((int) nib - 16)
                        : pred + ch->delta * (int) nib;
    if (ns > 32767) ns = 32767;
    if (ns < -32768) ns = -32768;
    s32 d = ch->delta * adapt_table[nib] / 256;
    if (d < 16) d = 16;
    ch->delta = d;
    ch->samp2 = ch->samp1;
    ch->samp1 = ns;
}

/* Decode one ADPCM block into `out` (interleaved, ADPCM_SPB frames). The
   nibble order mirrors src/SDL_sound_wav.c exactly: stereo packs one frame
   per byte (high nibble = left, low = right), mono uses one nibble per
   frame (high nibble first). */
static void ref_decode_block(const u8 *v, int channels, s16 *out)
{
    AdpcmChan ch[2];
    const int hb = channels * 7;

    for (int c = 0; c < channels; c++)
    {
        ch[c].predictor = v[c];
        ch[c].delta = v[channels + c * 2] | (v[channels + c * 2 + 1] << 8);
        const u8 *s1 = v + 3 * channels + c * 2;
        const u8 *s2 = v + 5 * channels + c * 2;
        ch[c].samp1 = (s16) (s1[0] | (s1[1] << 8));
        ch[c].samp2 = (s16) (s2[0] | (s2[1] << 8));
    }

    /* frame 0 is the header's iSamp2, frame 1 the header's iSamp1. */
    for (int c = 0; c < channels; c++)
        out[c] = (s16) ch[c].samp2;
    for (int c = 0; c < channels; c++)
        out[channels + c] = (s16) ch[c].samp1;

    for (unsigned f = 2; f < ADPCM_SPB; f++)
    {
        for (int c = 0; c < channels; c++)
        {
            u8 nib;
            if (channels == 2)
            {
                const u8 byte = v[hb + (f - 2)];
                nib = (c == 0) ? (byte >> 4) : (byte & 0x0F);
            }
            else
            {
                const u8 byte = v[hb + (f - 2) / 2];
                nib = ((f - 2) & 1u) ? (byte & 0x0F) : (byte >> 4);
            }
            adpcm_nibble(nib, &ch[c]);
            out[(size_t) f * channels + c] = (s16) ch[c].samp1;
        }
    }
}

/* Full independent decode of an ADPCM file built by build_adpcm(). */
static s16 *ref_decode_adpcm(const u8 *file, size_t file_size, int channels,
                             unsigned *out_frames)
{
    const unsigned ba = channels == 2 ? 32u : 16u;
    const unsigned blocks = (unsigned) ((file_size - 90) / ba);
    const unsigned n = blocks * ADPCM_SPB;
    s16 *all = (s16 *) SDL_malloc((size_t) n * channels * sizeof(s16));
    if (all == NULL) return NULL;

    for (unsigned k = 0; k < blocks; k++)
        ref_decode_block(file + 90 + (size_t) k * ba, channels,
                         all + (size_t) k * ADPCM_SPB * channels);
    *out_frames = n;
    return all;
}

/* ... logical timeline reference .......................................... */

static Uint64 map_source(Uint64 q, Uint64 start, Uint64 end, Uint32 reps)
{
    const Uint64 L = end - start;
    const Uint64 span = L * reps;
    if (q < end) return q;
    if (q < end + span) return start + (q - end) % L;
    return q - span;
}

static Uint32 remaining(Uint64 q, Uint64 start, Uint64 end, Uint32 reps)
{
    const Uint64 L = end - start;
    if (q < end) return reps;
    if (q < end + L * reps)
        return reps - (Uint32) ((q - end) / L);
    return 0;
}

/* Build the fully expanded native S16 stream from a decoded source. */
static s16 *expand_native(const s16 *src, unsigned n, int channels,
                          Uint64 start, Uint64 end, Uint32 reps,
                          Uint64 *out_total)
{
    const Uint64 total = (Uint64) n + (end - start) * reps;
    s16 *out = (s16 *) SDL_malloc((size_t) total * channels * sizeof(s16));
    if (out == NULL) return NULL;
    for (Uint64 q = 0; q < total; q++)
    {
        const Uint64 s = map_source(q, start, end, reps);
        memcpy(out + q * channels, src + s * channels,
               (size_t) channels * sizeof(s16));
    }
    *out_total = total;
    return out;
}

/* Reference conversion: expanded native PCM, one single AudioStream run. */
static float *ref_convert(const s16 *native, Uint64 frames,
                          const SDL_AudioSpec *act, const SDL_AudioSpec *want,
                          Uint64 *out_frames)
{
    SDL_AudioStream *st = SDL_CreateAudioStream(act, want);
    if (st == NULL) return NULL;

    /* Upper bound from the rate ratio, plus generous slack. */
    size_t cap = (size_t) ((frames * (Uint64) want->freq / (Uint64) act->freq
                            + 64) * (unsigned) want->channels);
    float *out = (float *) SDL_malloc(cap * sizeof(float));
    if (out == NULL) { SDL_DestroyAudioStream(st); return NULL; }

    if (!SDL_PutAudioStreamData(st, native,
                                (int) (frames * act->channels * sizeof(s16)))
        || !SDL_FlushAudioStream(st))
    {
        SDL_free(out);
        SDL_DestroyAudioStream(st);
        return NULL;
    }

    size_t got = 0;
    for (;;)
    {
        const int avail = SDL_GetAudioStreamAvailable(st);
        if (avail <= 0) break;
        size_t room = (cap - got) * sizeof(float);
        const int req = (int) ((size_t) avail < room ? (size_t) avail : room);
        const int br = SDL_GetAudioStreamData(st, out + got, req);
        if (br <= 0)
        {
            SDL_free(out);
            SDL_DestroyAudioStream(st);
            return NULL;
        }
        got += (size_t) br / sizeof(float);
        if (got > cap)
        {
            SDL_free(out);
            SDL_DestroyAudioStream(st);
            return NULL;
        }
    }
    SDL_DestroyAudioStream(st);
    *out_frames = got / (unsigned) want->channels;
    return out;
}

/* ... sample drivers ...................................................... */

static unsigned char filebuf[64 * 1024];

/* driver.c supplies main() but no library initialization. */
static int ensure_init(void)
{
    static int ready = 0;
    if (!ready)
    {
        if (!SDL_Init(0) || !Sound_Init())
            return 0;
        ready = 1;
    }
    return 1;
}

static Sound_Sample *open_sample(const SDL_AudioSpec *want, unsigned bufbytes)
{
    if (!ensure_init())
        return NULL;
    return Sound_NewSample(
        SDL_IOFromConstMem(filebuf, sizeof(filebuf)), "WAV", want, bufbytes);
}

/* Decode a whole sample, rotating read sizes and resizing the buffer
   periodically. bytes_out is byte-granular storage. */
typedef struct
{
    u8 *data;
    size_t size, cap;
} ByteVec;

static int vec_append(ByteVec *v, const void *p, size_t n)
{
    if (v->size + n > v->cap)
    {
        size_t ncap = v->cap ? v->cap * 2 : 4096;
        while (ncap < v->size + n) ncap *= 2;
        void *np = SDL_realloc(v->data, ncap);
        if (np == NULL) return -1;
        v->data = (u8 *) np;
        v->cap = ncap;
    }
    memcpy(v->data + v->size, p, n);
    v->size += n;
    return 0;
}

static const int read_schedule[] = { 1, 7, 113 };

static int collect(Sound_Sample *s, ByteVec *v, int vary_buffer)
{
    const unsigned frame_bytes =
        SDL_AUDIO_BYTESIZE(s->desired.format) * s->desired.channels;
    unsigned tick = 0;
    for (;;)
    {
        if (vary_buffer && (tick % 5 == 2))
        {
            const int rf = read_schedule[tick % 3] + (tick % 4);
            TASK_CHECK(Sound_SetBufferSize(s, rf * frame_bytes));
        }
        const Uint32 br = Sound_Decode(s);
        if (s->flags & SOUND_SAMPLEFLAG_ERROR)
            return 1;
        if (br > 0)
            TASK_CHECK(vec_append(v, s->buffer, br) == 0);
        tick++;
        if (s->flags & SOUND_SAMPLEFLAG_EOF)
            return 0;
        TASK_CHECK(tick < 100000);
    }
}

/* ... native (exact) group ................................................ */

static int case_pcm_basic(void)
{
    const unsigned N = 200;
    const size_t fz = build_pcm(filebuf, N, 32000, 2);
    (void) fz;
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 64);
    TASK_CHECK(s != NULL);

    const Uint64 start = 7, end = 53;
    const Uint32 reps = 3;
    TASK_CHECK(Sound_SetLoop(s, start, end, reps));

    Sound_LoopPosition p;
    TASK_CHECK(Sound_GetLoopPosition(s, &p));
    TASK_CHECK(p.logical_frame == 0 && p.output_frame == 0 &&
               p.remaining_repeats == reps && p.source_frame == 0);

    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 1) == 0);
    const Uint64 expect_total = N + (end - start) * reps;
    TASK_CHECK(out.size == expect_total * 4);

    const s16 *got = (const s16 *) out.data;
    for (Uint64 q = 0; q < expect_total; q++)
    {
        const Uint64 src = map_source(q, start, end, reps);
        TASK_CHECK(got[q * 2]     == wave_l((unsigned) src));
        TASK_CHECK(got[q * 2 + 1] == wave_r((unsigned) src));
    }
    TASK_CHECK(s->flags & SOUND_SAMPLEFLAG_EOF);
    Sound_FreeSample(s);
    SDL_free(out.data);
    return 0;
}

static int case_pcm_zero_repeat(void)
{
    const unsigned N = 120;
    build_pcm(filebuf, N, 32000, 2);
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 40);
    TASK_CHECK(Sound_SetLoop(s, 10, 40, 0));  /* 0 repeats: plain material */

    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 0) == 0);
    TASK_CHECK(out.size == (size_t) N * 4);
    const s16 *got = (const s16 *) out.data;
    for (unsigned q = 0; q < N; q++)
    {
        TASK_CHECK(got[q * 2] == wave_l(q));
        TASK_CHECK(got[q * 2 + 1] == wave_r(q));
    }

    Sound_LoopPosition p;
    TASK_CHECK(Sound_GetLoopPosition(s, &p));
    TASK_CHECK(p.logical_frame == N && p.remaining_repeats == 0 &&
               p.source_frame == N);

    /* Zero repeats still support the seek entry point and queries. */
    TASK_CHECK(Sound_SeekLoopFrame(s, 40));
    TASK_CHECK(Sound_GetLoopPosition(s, &p));
    TASK_CHECK(p.logical_frame == 40 && p.source_frame == 40 &&
               p.output_frame == 0 && p.remaining_repeats == 0);
    Sound_FreeSample(s);
    SDL_free(out.data);
    return 0;
}

static int case_pcm_one_frame_region(void)
{
    const unsigned N = 80;
    build_pcm(filebuf, N, 44100, 2);
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 44100 };
    Sound_Sample *s = open_sample(&want, 16);
    const Uint64 start = 30, end = 31, reps = 4;
    TASK_CHECK(Sound_SetLoop(s, start, end, (Uint32) reps));

    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 1) == 0);
    const Uint64 total = N + reps;
    TASK_CHECK(out.size == total * 4);
    const s16 *got = (const s16 *) out.data;
    for (Uint64 q = 0; q < total; q++)
    {
        const Uint64 src = map_source(q, start, end, (Uint32) reps);
        TASK_CHECK(got[q * 2] == wave_l((unsigned) src));
        TASK_CHECK(got[q * 2 + 1] == wave_r((unsigned) src));
    }
    Sound_FreeSample(s);
    SDL_free(out.data);
    return 0;
}

static int case_pcm_region_to_end(void)
{
    const unsigned N = 100;
    build_pcm(filebuf, N, 32000, 2);
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 28);
    const Uint32 reps = 2;
    TASK_CHECK(Sound_SetLoop(s, 60, N, reps));   /* end == N: no tail */

    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 0) == 0);
    const Uint64 total = (Uint64) N + (N - 60) * reps;
    TASK_CHECK(out.size == total * 4);
    const s16 *got = (const s16 *) out.data;
    for (Uint64 q = 0; q < total; q++)
    {
        const Uint64 src = map_source(q, 60, N, reps);
        TASK_CHECK(got[q * 2] == wave_l((unsigned) src));
        TASK_CHECK(got[q * 2 + 1] == wave_r((unsigned) src));
    }
    Sound_FreeSample(s);
    SDL_free(out.data);
    return 0;
}

static int case_pcm_mono(void)
{
    const unsigned N = 96;
    build_pcm(filebuf, N, 32000, 1);
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 1, 32000 };
    Sound_Sample *s = open_sample(&want, 24);
    const Uint32 reps = 3;
    TASK_CHECK(Sound_SetLoop(s, 5, 17, reps));

    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 1) == 0);
    const Uint64 total = (Uint64) N + 12 * reps;
    TASK_CHECK(out.size == total * 2);
    const s16 *got = (const s16 *) out.data;
    for (Uint64 q = 0; q < total; q++)
        TASK_CHECK(got[q] == wave_m((unsigned) map_source(q, 5, 17, reps)));
    Sound_FreeSample(s);
    SDL_free(out.data);
    return 0;
}

static int case_native_clock_queries(void)
{
    const unsigned N = 120;
    build_pcm(filebuf, N, 32000, 2);
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 8);   /* 2-frame reads */
    const Uint64 start = 13, end = 47;
    const Uint32 reps = 5;
    TASK_CHECK(Sound_SetLoop(s, start, end, reps));
    const Uint64 total = (Uint64) N + (end - start) * reps;

    Uint64 delivered = 0;
    for (;;)
    {
        const Uint32 br = Sound_Decode(s);
        TASK_CHECK((s->flags & SOUND_SAMPLEFLAG_ERROR) == 0);
        if (br > 0)
        {
            const Uint64 frames = br / 4;
            delivered += frames;
            Sound_LoopPosition p;
            TASK_CHECK(Sound_GetLoopPosition(s, &p));
            const Uint64 expect_q = delivered < total ? delivered : total;
            TASK_CHECK(p.output_frame == delivered);
            TASK_CHECK(p.logical_frame == expect_q);
            TASK_CHECK(p.source_frame == map_source(expect_q, start, end, reps));
            TASK_CHECK(p.remaining_repeats ==
                       remaining(expect_q, start, end, reps));
        }
        if (s->flags & SOUND_SAMPLEFLAG_EOF) break;
    }
    TASK_CHECK(delivered == total);

    /* Queries do not advance the clock. */
    Sound_LoopPosition p1, p2;
    TASK_CHECK(Sound_GetLoopPosition(s, &p1));
    TASK_CHECK(Sound_GetLoopPosition(s, &p2));
    TASK_CHECK(memcmp(&p1, &p2, sizeof(p1)) == 0);
    Sound_FreeSample(s);
    return 0;
}

static int case_decode_all(void)
{
    const unsigned N = 100;
    build_pcm(filebuf, N, 32000, 2);
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 32);
    const Uint32 reps = 2;
    TASK_CHECK(Sound_SetLoop(s, 20, 35, reps));

    const Uint32 br = Sound_DecodeAll(s);
    const Uint64 total = (Uint64) N + 15 * reps;
    TASK_CHECK(br == total * 4);
    TASK_CHECK(s->flags & SOUND_SAMPLEFLAG_EOF);
    const s16 *got = (const s16 *) s->buffer;
    for (Uint64 q = 0; q < total; q++)
    {
        const Uint64 src = map_source(q, 20, 35, reps);
        TASK_CHECK(got[q * 2] == wave_l((unsigned) src));
        TASK_CHECK(got[q * 2 + 1] == wave_r((unsigned) src));
    }
    Sound_FreeSample(s);
    return 0;
}

static int case_adpcm_basic(void)
{
    const unsigned blocks = 8;
    const size_t fz = build_adpcm(filebuf, blocks, 32000, 2);
    unsigned n = 0;
    s16 *src = ref_decode_adpcm(filebuf, fz, 2, &n);
    TASK_CHECK(n == blocks * ADPCM_SPB);

    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 44);
    const Uint64 start = 3, end = 17;       /* strictly inside one block */
    const Uint32 reps = 2;
    TASK_CHECK(Sound_SetLoop(s, start, end, reps));

    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 1) == 0);
    Uint64 total = 0;
    s16 *exp = expand_native(src, n, 2, start, end, reps, &total);
    TASK_CHECK(out.size == total * 4);
    TASK_CHECK(memcmp(out.data, exp, out.size) == 0);
    Sound_FreeSample(s);
    SDL_free(out.data);
    SDL_free(src);
    SDL_free(exp);
    return 0;
}

static int case_adpcm_cross_block(void)
{
    const unsigned blocks = 6;
    const size_t fz = build_adpcm(filebuf, blocks, 32000, 2);
    unsigned n = 0;
    s16 *src = ref_decode_adpcm(filebuf, fz, 2, &n);

    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 20);
    const Uint64 start = 18, end = 25;      /* straddles the block boundary */
    const Uint32 reps = 3;
    TASK_CHECK(Sound_SetLoop(s, start, end, reps));

    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 1) == 0);
    Uint64 total = 0;
    s16 *exp = expand_native(src, n, 2, start, end, reps, &total);
    TASK_CHECK(out.size == total * 4);
    TASK_CHECK(memcmp(out.data, exp, out.size) == 0);
    Sound_FreeSample(s);
    SDL_free(out.data);
    SDL_free(src);
    SDL_free(exp);
    return 0;
}

static int case_adpcm_one_frame(void)
{
    const unsigned blocks = 5;
    const size_t fz = build_adpcm(filebuf, blocks, 32000, 2);
    unsigned n = 0;
    s16 *src = ref_decode_adpcm(filebuf, fz, 2, &n);

    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 12);
    const Uint32 reps = 5;
    TASK_CHECK(Sound_SetLoop(s, 11, 12, reps));

    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 0) == 0);
    Uint64 total = 0;
    s16 *exp = expand_native(src, n, 2, 11, 12, reps, &total);
    TASK_CHECK(out.size == total * 4);
    TASK_CHECK(memcmp(out.data, exp, out.size) == 0);
    Sound_FreeSample(s);
    SDL_free(out.data);
    SDL_free(src);
    SDL_free(exp);
    return 0;
}

static int case_adpcm_mono(void)
{
    const unsigned blocks = 6;
    const size_t fz = build_adpcm(filebuf, blocks, 32000, 1);
    unsigned n = 0;
    s16 *src = ref_decode_adpcm(filebuf, fz, 1, &n);

    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 1, 32000 };
    Sound_Sample *s = open_sample(&want, 18);
    const Uint64 start = 2, end = 33;       /* crosses several blocks */
    const Uint32 reps = 2;
    TASK_CHECK(Sound_SetLoop(s, start, end, reps));

    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 1) == 0);
    Uint64 total = 0;
    s16 *exp = expand_native(src, n, 1, start, end, reps, &total);
    TASK_CHECK(out.size == total * 2);
    TASK_CHECK(memcmp(out.data, exp, out.size) == 0);
    Sound_FreeSample(s);
    SDL_free(out.data);
    SDL_free(src);
    SDL_free(exp);
    return 0;
}

static int case_adpcm_zero_repeat(void)
{
    const unsigned blocks = 4;
    const size_t fz = build_adpcm(filebuf, blocks, 32000, 2);
    unsigned n = 0;
    s16 *src = ref_decode_adpcm(filebuf, fz, 2, &n);

    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 40);
    TASK_CHECK(Sound_SetLoop(s, 4, 9, 0));

    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 0) == 0);
    TASK_CHECK(out.size == (size_t) n * 4);
    TASK_CHECK(memcmp(out.data, src, out.size) == 0);
    Sound_FreeSample(s);
    SDL_free(out.data);
    SDL_free(src);
    return 0;
}

static int case_config_rules(void)
{
    build_pcm(filebuf, 100, 32000, 2);
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 40);

    /* invalid regions */
    TASK_CHECK(!Sound_SetLoop(s, 50, 50, 1));
    TASK_CHECK(!Sound_SetLoop(s, 60, 50, 1));
    TASK_CHECK(!Sound_SetLoop(s, 0, 101, 1));
    TASK_CHECK(Sound_GetError() != NULL);

    /* repeat-count multiplication overflow: the logical length must fit in
       Uint64. Checked at the pure timeline layer, where the source frame
       count is not bounded by a fixture's memory size. */
    {
        Sound_LoopState math;
        const Uint64 big33 = (Uint64) 1 << 33;
        const Uint64 big40 = (Uint64) 1 << 40;

        Sound_LoopInitState(&math);
        TASK_CHECK(!Sound_LoopConfigure(&math, SDL_MAX_UINT64,
                                        big33, SDL_MAX_UINT64, 2));
        TASK_CHECK(Sound_GetError() != NULL);
        TASK_CHECK(!math.configured);

        Sound_LoopInitState(&math);
        TASK_CHECK(!Sound_LoopConfigure(&math, big40, 0, big40,
                                        0xFFFFFFFFu));
        TASK_CHECK(Sound_GetError() != NULL);

        /* a legal large region still configures exactly */
        Sound_LoopInitState(&math);
        TASK_CHECK(Sound_LoopConfigure(&math, 1000, 100, 900, 7));
        TASK_CHECK(math.total == 1000 + 800 * 7);
        TASK_CHECK(Sound_LoopMapSource(&math, 0) == 0);
        TASK_CHECK(Sound_LoopMapSource(&math, 900) == 100);
    }

    /* unsupported format: 8-bit PCM */
    Sound_FreeSample(s);
    build_u8(filebuf, 100, 32000);
    s = open_sample(&want, 40);
    TASK_CHECK(s != NULL);
    TASK_CHECK(!Sound_SetLoop(s, 0, 10, 1));

    /* rejected configs leave no loop configured */
    Sound_LoopPosition p;
    TASK_CHECK(!Sound_GetLoopPosition(s, &p));
    TASK_CHECK(!Sound_SeekLoopFrame(s, 0));
    Sound_FreeSample(s);

    /* config lock: cannot replace after decoding, can after rewind */
    build_pcm(filebuf, 100, 32000, 2);
    s = open_sample(&want, 40);
    TASK_CHECK(Sound_SetLoop(s, 5, 20, 2));
    TASK_CHECK(Sound_Decode(s) > 0);
    TASK_CHECK(!Sound_SetLoop(s, 6, 21, 2));
    /* old config survives */
    TASK_CHECK(Sound_GetLoopPosition(s, &p));
    TASK_CHECK(p.remaining_repeats == 2);
    /* plain Sound_Seek is explicitly rejected while looping */
    TASK_CHECK(!Sound_Seek(s, 10));
    TASK_CHECK(Sound_Rewind(s));
    TASK_CHECK(Sound_SetLoop(s, 6, 21, 1));
    TASK_CHECK(Sound_GetLoopPosition(s, &p));
    TASK_CHECK(p.logical_frame == 0 && p.remaining_repeats == 1);
    Sound_FreeSample(s);
    return 0;
}

/* ... converted group ..................................................... */

static int converted_run(unsigned rate, unsigned n, int channels,
                         Uint64 start, Uint64 end, Uint32 reps, int adpcm,
                         int vary)
{
    SDL_AudioSpec act = { SDL_AUDIO_S16LE, channels, (int) rate };
    SDL_AudioSpec want = { SDL_AUDIO_F32LE, 2, 48000 };
    s16 *src;
    unsigned srcn;

    if (adpcm)
    {
        const size_t fz = build_adpcm(filebuf, n / ADPCM_SPB, rate, channels);
        src = ref_decode_adpcm(filebuf, fz, channels, &srcn);
    }
    else
    {
        build_pcm(filebuf, n, rate, channels);
        srcn = n;
        src = (s16 *) SDL_malloc((size_t) n * channels * sizeof(s16));
        TASK_CHECK(src != NULL);
        for (unsigned i = 0; i < n; i++)
            for (int c = 0; c < channels; c++)
                src[i * channels + c] =
                    channels == 2 ? (c == 0 ? wave_l(i) : wave_r(i))
                                  : wave_m(i);
    }

    Sound_Sample *s = open_sample(&want, 96);
    TASK_CHECK(s != NULL);
    TASK_CHECK(Sound_SetLoop(s, start, end, reps));

    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, vary) == 0);

    Uint64 native_total = 0;
    s16 *expanded = expand_native(src, srcn, channels, start, end, reps,
                                  &native_total);
    TASK_CHECK(expanded != NULL);
    Uint64 ref_frames = 0;
    float *ref = ref_convert(expanded, native_total, &act, &want, &ref_frames);
    TASK_CHECK(ref != NULL);

    const float *got = (const float *) out.data;
    const Uint64 got_frames = out.size / (2 * sizeof(float));
    TASK_CHECK(got_frames == ref_frames);
    for (Uint64 i = 0; i < got_frames * 2; i++)
    {
        TASK_CHECK(isfinite(got[i]));
        TASK_CHECK(fabsf((double) got[i] - (double) ref[i]) <= 3e-5);
    }

    Sound_FreeSample(s);
    SDL_free(out.data);
    SDL_free(src);
    SDL_free(expanded);
    SDL_free(ref);
    return 0;
}

static int case_convert_44100(void)
{
    return converted_run(44100, 192, 2, 9, 73, 3, 0, 1);
}

static int case_convert_32000(void)
{
    return converted_run(32000, 160, 2, 4, 40, 2, 0, 1);
}

static int case_convert_adpcm(void)
{
    return converted_run(32000, 8 * ADPCM_SPB, 2, 6, 51, 2, 1, 1);
}

static int case_convert_read_sizes(void)
{
    /* 1/7/113-frame reads with buffer resizing on both source rates. */
    int rc = converted_run(44100, 144, 2, 11, 30, 2, 0, 1);
    if (rc) return rc;
    return converted_run(32000, 128, 2, 3, 33, 4, 0, 1);
}

static int case_convert_zero_repeat_matches_plain(void)
{
    /* With zero repeats the converted stream is the plain source, run
       through one stream. Asymmetric channels make mismatches obvious. */
    return converted_run(32000, 120, 2, 20, 60, 0, 0, 0);
}

static int case_convert_clock(void)
{
    const unsigned N = 150, rate = 32000;
    build_pcm(filebuf, N, rate, 2);
    SDL_AudioSpec want = { SDL_AUDIO_F32LE, 2, 48000 };
    Sound_Sample *s = open_sample(&want, 32);
    const Uint64 start = 10, end = 40, reps = 3;
    TASK_CHECK(Sound_SetLoop(s, start, end, (Uint32) reps));
    const Uint64 total = (Uint64) N + (end - start) * reps;

    Uint64 delivered = 0, acc_num = 0;
    for (;;)
    {
        const Uint32 br = Sound_Decode(s);
        TASK_CHECK((s->flags & SOUND_SAMPLEFLAG_ERROR) == 0);
        if (br > 0)
        {
            const Uint64 frames = br / (2 * sizeof(float));
            delivered += frames;
            acc_num += frames * rate;
            Sound_LoopPosition p;
            TASK_CHECK(Sound_GetLoopPosition(s, &p));
            TASK_CHECK(p.output_frame == delivered);
            Uint64 q = acc_num / 48000u;  /* q0 is zero in this generation */
            if (q > total) q = total;
            TASK_CHECK(p.logical_frame == q);
            TASK_CHECK(p.source_frame == map_source(q, start, end, reps));
            TASK_CHECK(p.remaining_repeats == remaining(q, start, end, reps));
        }
        if (s->flags & SOUND_SAMPLEFLAG_EOF) break;
    }
    Sound_FreeSample(s);
    return 0;
}

/* DecodeAll must return the whole converted stream in one allocation. */
static int case_convert_decode_all(void)
{
    const unsigned N = 140, rate = 32000;
    build_pcm(filebuf, N, rate, 2);
    SDL_AudioSpec act = { SDL_AUDIO_S16LE, 2, rate };
    SDL_AudioSpec want = { SDL_AUDIO_F32LE, 2, 48000 };
    Sound_Sample *s = open_sample(&want, 48);
    const Uint64 start = 15, end = 55, reps = 2;
    TASK_CHECK(Sound_SetLoop(s, start, end, (Uint32) reps));

    const Uint32 br = Sound_DecodeAll(s);
    TASK_CHECK(s->flags & SOUND_SAMPLEFLAG_EOF);

    s16 *src = (s16 *) SDL_malloc((size_t) N * 2 * sizeof(s16));
    TASK_CHECK(src != NULL);
    for (unsigned i = 0; i < N; i++)
    {
        src[i * 2] = wave_l(i);
        src[i * 2 + 1] = wave_r(i);
    }
    Uint64 native_total = 0;
    s16 *expanded = expand_native(src, N, 2, start, end, reps, &native_total);
    TASK_CHECK(expanded != NULL);
    Uint64 ref_frames = 0;
    float *ref = ref_convert(expanded, native_total, &act, &want, &ref_frames);
    TASK_CHECK(ref != NULL);

    const float *got = (const float *) s->buffer;
    TASK_CHECK(br == ref_frames * 2 * sizeof(float));
    for (Uint64 i = 0; i < ref_frames * 2; i++)
        TASK_CHECK(fabsf((double) got[i] - (double) ref[i]) <= 3e-5);

    Sound_FreeSample(s);
    SDL_free(src);
    SDL_free(expanded);
    SDL_free(ref);
    return 0;
}

/* ... seek group .......................................................... */

static int case_seek_native(void)
{
    const unsigned N = 160;
    build_pcm(filebuf, N, 32000, 2);
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 24);
    const Uint64 start = 8, end = 50, reps = 3;
    TASK_CHECK(Sound_SetLoop(s, start, end, (Uint32) reps));
    const Uint64 total = (Uint64) N + (end - start) * reps;

    /* burn some frames, then seek */
    for (int i = 0; i < 4; i++) TASK_CHECK(Sound_Decode(s) > 0);
    const Uint64 q0 = 37;
    TASK_CHECK(Sound_SeekLoopFrame(s, q0));

    Sound_LoopPosition p;
    TASK_CHECK(Sound_GetLoopPosition(s, &p));
    TASK_CHECK(p.logical_frame == q0 && p.output_frame == 0 &&
               p.source_frame == map_source(q0, start, end, reps) &&
               p.remaining_repeats == remaining(q0, start, end, reps));

    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 1) == 0);
    const Uint64 suffix = total - q0;
    TASK_CHECK(out.size == suffix * 4);
    const s16 *got = (const s16 *) out.data;
    for (Uint64 i = 0; i < suffix; i++)
    {
        const Uint64 src = map_source(q0 + i, start, end, reps);
        TASK_CHECK(got[i * 2] == wave_l((unsigned) src));
        TASK_CHECK(got[i * 2 + 1] == wave_r((unsigned) src));
    }

    /* seek to the logical end: nothing remains */
    TASK_CHECK(Sound_SeekLoopFrame(s, total));
    TASK_CHECK(Sound_GetLoopPosition(s, &p));
    TASK_CHECK(p.logical_frame == total && p.source_frame == N &&
               p.remaining_repeats == 0);
    TASK_CHECK(Sound_Decode(s) == 0);
    TASK_CHECK(s->flags & SOUND_SAMPLEFLAG_EOF);
    Sound_FreeSample(s);
    SDL_free(out.data);
    return 0;
}

static int case_seek_with_preread(void)
{
    /* Converter still holds queued/resampler history when we seek: the new
       generation must have zero prior history, matching a fresh suffix
       stream. */
    const unsigned N = 180, rate = 44100;
    build_pcm(filebuf, N, rate, 2);
    SDL_AudioSpec act = { SDL_AUDIO_S16LE, 2, rate };
    SDL_AudioSpec want = { SDL_AUDIO_F32LE, 2, 48000 };
    Sound_Sample *s = open_sample(&want, 64);
    const Uint64 start = 12, end = 64, reps = 2;
    TASK_CHECK(Sound_SetLoop(s, start, end, (Uint32) reps));
    const Uint64 total = (Uint64) N + (end - start) * reps;

    /* Decode enough that the converter is holding queued PCM and resampler
       history before the seek happens. */
    for (int i = 0; i < 6; i++) TASK_CHECK(Sound_Decode(s) > 0);

    const Uint64 q0 = 55;
    TASK_CHECK(Sound_SeekLoopFrame(s, q0));
    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 1) == 0);

    s16 *src = (s16 *) SDL_malloc((size_t) N * 2 * sizeof(s16));
    TASK_CHECK(src != NULL);
    for (unsigned i = 0; i < N; i++)
    {
        src[i * 2] = wave_l(i);
        src[i * 2 + 1] = wave_r(i);
    }
    Uint64 native_total = 0;
    s16 *expanded = expand_native(src, N, 2, start, end, reps, &native_total);
    TASK_CHECK(expanded != NULL);
    TASK_CHECK(native_total == total);

    /* Fresh stream seeded only with the suffix: zero history. */
    SDL_AudioStream *st = SDL_CreateAudioStream(&act, &want);
    TASK_CHECK(st != NULL);
    const Uint64 suffix_native = total - q0;
    TASK_CHECK(SDL_PutAudioStreamData(st, expanded + q0 * 2,
                                     (int) (suffix_native * 2 * sizeof(s16))));
    TASK_CHECK(SDL_FlushAudioStream(st));
    float ref[8192];
    size_t refn = 0;
    for (;;)
    {
        const int avail = SDL_GetAudioStreamAvailable(st);
        if (avail <= 0) break;
        const int br = SDL_GetAudioStreamData(st, ref + refn,
                                              (int) sizeof(ref) - (int) (refn * sizeof(float)));
        TASK_CHECK(br > 0);
        refn += (size_t) br / sizeof(float);
    }
    SDL_DestroyAudioStream(st);

    const float *got = (const float *) out.data;
    TASK_CHECK(out.size == refn * sizeof(float));
    for (size_t i = 0; i < refn; i++)
        TASK_CHECK(fabsf((double) got[i] - (double) ref[i]) <= 3e-5);

    Sound_FreeSample(s);
    SDL_free(out.data);
    SDL_free(src);
    SDL_free(expanded);
    return 0;
}

static int case_seek_after_eof(void)
{
    const unsigned N = 120;
    build_adpcm(filebuf, 6, 32000, 2);
    size_t fz = 90 + 6 * 32;
    unsigned n = 0;
    s16 *src = ref_decode_adpcm(filebuf, fz, 2, &n);
    TASK_CHECK(n == N);

    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 36);
    const Uint64 start = 5, end = 25, reps = 2;
    TASK_CHECK(Sound_SetLoop(s, start, end, (Uint32) reps));
    const Uint64 total = (Uint64) N + (end - start) * reps;

    ByteVec first = { 0 };
    TASK_CHECK(collect(s, &first, 0) == 0);
    TASK_CHECK(s->flags & SOUND_SAMPLEFLAG_EOF);

    const Uint64 q0 = 30;
    TASK_CHECK(Sound_SeekLoopFrame(s, q0));
    TASK_CHECK((s->flags & (SOUND_SAMPLEFLAG_EOF | SOUND_SAMPLEFLAG_ERROR)) == 0);
    ByteVec second = { 0 };
    TASK_CHECK(collect(s, &second, 1) == 0);
    const Uint64 suffix = total - q0;
    TASK_CHECK(second.size == suffix * 4);
    const s16 *got = (const s16 *) second.data;
    for (Uint64 i = 0; i < suffix; i++)
    {
        const Uint64 srci = map_source(q0 + i, start, end, reps);
        TASK_CHECK(got[i * 2] == src[srci * 2]);
        TASK_CHECK(got[i * 2 + 1] == src[srci * 2 + 1]);
    }
    Sound_FreeSample(s);
    SDL_free(first.data);
    SDL_free(second.data);
    SDL_free(src);
    return 0;
}

static int case_seek_out_of_range_preserves(void)
{
    const unsigned N = 100;
    build_pcm(filebuf, N, 32000, 2);
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 20);
    const Uint32 reps = 2;
    TASK_CHECK(Sound_SetLoop(s, 10, 30, reps));
    const Uint64 total = (Uint64) N + 20 * reps;
    for (int i = 0; i < 3; i++) TASK_CHECK(Sound_Decode(s) > 0);

    Sound_LoopPosition before;
    TASK_CHECK(Sound_GetLoopPosition(s, &before));
    TASK_CHECK(!Sound_SeekLoopFrame(s, total + 1));
    TASK_CHECK(Sound_GetError() != NULL);
    Sound_LoopPosition after;
    TASK_CHECK(Sound_GetLoopPosition(s, &after));
    TASK_CHECK(memcmp(&before, &after, sizeof(before)) == 0);

    /* scene still plays exactly where it was */
    ByteVec rest = { 0 };
    TASK_CHECK(collect(s, &rest, 0) == 0);
    Sound_LoopPosition endp;
    TASK_CHECK(Sound_GetLoopPosition(s, &endp));
    TASK_CHECK(endp.logical_frame == total);
    Sound_FreeSample(s);
    SDL_free(rest.data);
    return 0;
}

/* Failing SDL_IOStream: one armed seek fails, then recovers. */
typedef struct
{
    const u8 *base;
    size_t size, pos;
    int armed;
    int failures;
} FailIO;

static Sint64 failio_size(void *ud) { return (Sint64) ((FailIO *) ud)->size; }
static Sint64 failio_seek(void *ud, Sint64 off, SDL_IOWhence whence)
{
    FailIO *f = (FailIO *) ud;
    if (f->armed)
    {
        f->armed = 0;
        f->failures++;
        return -1;
    }
    Sint64 np;
    if (whence == SDL_IO_SEEK_SET) np = off;
    else if (whence == SDL_IO_SEEK_CUR) np = (Sint64) f->pos + off;
    else np = (Sint64) f->size + off;
    if (np < 0 || (size_t) np > f->size) return -1;
    f->pos = (size_t) np;
    return np;
}
static size_t failio_read(void *ud, void *ptr, size_t size, SDL_IOStatus *st)
{
    FailIO *f = (FailIO *) ud;
    size_t n = f->size - f->pos;
    if (size < n) n = size;
    memcpy(ptr, f->base + f->pos, n);
    f->pos += n;
    (void) st;
    return n;
}
static bool failio_close(void *ud) { (void) ud; return true; }

static int case_seek_failure_recovery(void)
{
    const unsigned N = 120;
    const size_t fz = build_pcm(filebuf, N, 32000, 2);
    static FailIO fio;
    fio.base = filebuf; fio.size = fz; fio.pos = 0; fio.armed = 0; fio.failures = 0;

    SDL_IOStreamInterface iface;
    SDL_INIT_INTERFACE(&iface);
    iface.size = failio_size;
    iface.seek = failio_seek;
    iface.read = failio_read;
    iface.close = failio_close;

    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    TASK_CHECK(ensure_init());
    Sound_Sample *s = Sound_NewSample(SDL_OpenIO(&iface, &fio), "WAV", &want, 24);
    TASK_CHECK(s != NULL);
    TASK_CHECK(Sound_SetLoop(s, 10, 40, 3));

    for (int i = 0; i < 3; i++) TASK_CHECK(Sound_Decode(s) > 0);

    /* arm a failure, then seek: ERROR state, no stale audio delivered */
    fio.armed = 1;
    TASK_CHECK(!Sound_SeekLoopFrame(s, 50));
    TASK_CHECK(s->flags & SOUND_SAMPLEFLAG_ERROR);
    TASK_CHECK(fio.failures == 1);
    TASK_CHECK(Sound_Decode(s) == 0);  /* error blocks further decoding */

    /* a later successful seek recovers the sample */
    TASK_CHECK(Sound_SeekLoopFrame(s, 50));
    TASK_CHECK((s->flags & SOUND_SAMPLEFLAG_ERROR) == 0);
    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 1) == 0);
    const Uint64 total = (Uint64) N + 30 * 3;
    TASK_CHECK(out.size == (total - 50) * 4);
    const s16 *got = (const s16 *) out.data;
    for (Uint64 i = 0; i < total - 50; i++)
    {
        const Uint64 src = map_source(50 + i, 10, 40, 3);
        TASK_CHECK(got[i * 2] == wave_l((unsigned) src));
        TASK_CHECK(got[i * 2 + 1] == wave_r((unsigned) src));
    }
    Sound_FreeSample(s);
    SDL_free(out.data);
    return 0;
}

static int case_two_instances(void)
{
    const unsigned N = 100;
    build_pcm(filebuf, N, 32000, 2);
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };

    TASK_CHECK(ensure_init());
    Sound_Sample *a = Sound_NewSample(
        SDL_IOFromConstMem(filebuf, sizeof(filebuf)), "WAV", &want, 16);
    Sound_Sample *b = Sound_NewSample(
        SDL_IOFromConstMem(filebuf, sizeof(filebuf)), "WAV", &want, 28);
    TASK_CHECK(a && b);
    TASK_CHECK(Sound_SetLoop(a, 5, 25, 2));
    TASK_CHECK(Sound_SetLoop(b, 30, 60, 4));

    ByteVec oa = { 0 }, ob = { 0 };
    int alive_a = 1, alive_b = 1;
    while (alive_a || alive_b)
    {
        if (alive_a)
        {
            const Uint32 br = Sound_Decode(a);
            TASK_CHECK((a->flags & SOUND_SAMPLEFLAG_ERROR) == 0);
            if (br) TASK_CHECK(vec_append(&oa, a->buffer, br) == 0);
            if (a->flags & SOUND_SAMPLEFLAG_EOF) alive_a = 0;
        }
        if (alive_b)
        {
            const Uint32 br = Sound_Decode(b);
            TASK_CHECK((b->flags & SOUND_SAMPLEFLAG_ERROR) == 0);
            if (br) TASK_CHECK(vec_append(&ob, b->buffer, br) == 0);
            if (b->flags & SOUND_SAMPLEFLAG_EOF) alive_b = 0;
        }
    }

    const Uint64 ta = (Uint64) N + 20 * 2, tb = (Uint64) N + 30 * 4;
    TASK_CHECK(oa.size == ta * 4 && ob.size == tb * 4);
    const s16 *ga = (const s16 *) oa.data;
    const s16 *gb = (const s16 *) ob.data;
    for (Uint64 q = 0; q < ta; q++)
    {
        const Uint64 src = map_source(q, 5, 25, 2);
        TASK_CHECK(ga[q * 2] == wave_l((unsigned) src));
        TASK_CHECK(ga[q * 2 + 1] == wave_r((unsigned) src));
    }
    for (Uint64 q = 0; q < tb; q++)
    {
        const Uint64 src = map_source(q, 30, 60, 4);
        TASK_CHECK(gb[q * 2] == wave_l((unsigned) src));
        TASK_CHECK(gb[q * 2 + 1] == wave_r((unsigned) src));
    }

    /* an independent seek on one instance leaves the other untouched */
    TASK_CHECK(Sound_SeekLoopFrame(a, 7));
    Sound_LoopPosition pa, pb;
    TASK_CHECK(Sound_GetLoopPosition(a, &pa));
    TASK_CHECK(Sound_GetLoopPosition(b, &pb));
    TASK_CHECK(pa.logical_frame == 7 && pb.logical_frame == tb);
    Sound_FreeSample(a);
    Sound_FreeSample(b);
    SDL_free(oa.data);
    SDL_free(ob.data);
    return 0;
}

static int case_rewind_equals_zero_seek(void)
{
    const unsigned N = 90;
    build_pcm(filebuf, N, 32000, 2);
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 2, 32000 };
    Sound_Sample *s = open_sample(&want, 20);
    TASK_CHECK(Sound_SetLoop(s, 8, 28, 2));
    for (int i = 0; i < 5; i++) TASK_CHECK(Sound_Decode(s) > 0);
    TASK_CHECK(Sound_Rewind(s));
    Sound_LoopPosition p;
    TASK_CHECK(Sound_GetLoopPosition(s, &p));
    TASK_CHECK(p.logical_frame == 0 && p.output_frame == 0 &&
               p.source_frame == 0 && p.remaining_repeats == 2);
    ByteVec out = { 0 };
    TASK_CHECK(collect(s, &out, 1) == 0);
    const Uint64 total = (Uint64) N + 20 * 2;
    TASK_CHECK(out.size == total * 4);
    const s16 *got = (const s16 *) out.data;
    for (Uint64 q = 0; q < total; q++)
    {
        const Uint64 src = map_source(q, 8, 28, 2);
        TASK_CHECK(got[q * 2] == wave_l((unsigned) src));
        TASK_CHECK(got[q * 2 + 1] == wave_r((unsigned) src));
    }
    Sound_FreeSample(s);
    SDL_free(out.data);
    return 0;
}

/* ... registry ............................................................ */

const struct task_case task_cases[] = {
    { "native", "pcm_basic",              case_pcm_basic },
    { "native", "pcm_zero_repeat",        case_pcm_zero_repeat },
    { "native", "pcm_one_frame_region",   case_pcm_one_frame_region },
    { "native", "pcm_region_to_end",      case_pcm_region_to_end },
    { "native", "pcm_mono",               case_pcm_mono },
    { "native", "clock_queries",          case_native_clock_queries },
    { "native", "decode_all",             case_decode_all },
    { "native", "adpcm_basic",            case_adpcm_basic },
    { "native", "adpcm_cross_block",      case_adpcm_cross_block },
    { "native", "adpcm_one_frame",        case_adpcm_one_frame },
    { "native", "adpcm_mono",             case_adpcm_mono },
    { "native", "adpcm_zero_repeat",      case_adpcm_zero_repeat },
    { "native", "config_rules",           case_config_rules },

    { "converted", "upsample_44100",      case_convert_44100 },
    { "converted", "upsample_32000",      case_convert_32000 },
    { "converted", "adpcm_upsample",      case_convert_adpcm },
    { "converted", "read_sizes_resize",   case_convert_read_sizes },
    { "converted", "zero_repeat_plain",   case_convert_zero_repeat_matches_plain },
    { "converted", "clock_queries",       case_convert_clock },
    { "converted", "decode_all",          case_convert_decode_all },

    { "seek", "seek_native",              case_seek_native },
    { "seek", "seek_with_preread",        case_seek_with_preread },
    { "seek", "seek_after_eof",           case_seek_after_eof },
    { "seek", "out_of_range_preserves",   case_seek_out_of_range_preserves },
    { "seek", "failure_recovery",         case_seek_failure_recovery },
    { "seek", "two_instances",            case_two_instances },
    { "seek", "rewind_equals_zero_seek",  case_rewind_equals_zero_seek },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
