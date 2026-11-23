/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

// Tests for SDL_CreateAudioStreamTap(): fan-out, buffer ownership and
// independent conversion. Registered through task_support/check.h; this
// file has no main() of its own.

#include <SDL3/SDL.h>
#include "task_support/check.h"

// Asymmetric, strictly non-zero stereo test signal: the left channel is
// always positive and the right channel always negative, so the two channels
// never match each other or silence. Channel swaps or dropped frames show up.
static void make_frame(int frame, float *left, float *right)
{
    *left = (float)(((frame * 7) % 13) + 1) / 16.0f;
    *right = -(float)(((frame * 11) % 17) + 1) / 32.0f;
}

static void fill_stereo(float *dst, int num_frames, int frame_offset)
{
    for (int i = 0; i < num_frames; ++i) {
        make_frame(frame_offset + i, &dst[i * 2], &dst[i * 2 + 1]);
    }
}

static SDL_AudioSpec make_spec(SDL_AudioFormat format, int channels, int freq)
{
    SDL_AudioSpec spec;
    spec.format = format;
    spec.channels = channels;
    spec.freq = freq;
    return spec;
}

// Drain a flushed stream in small, irregular chunks until no more data comes.
// Returns the number of frames read, or -1 on an API error.
static int drain_f32(SDL_AudioStream *stream, float *out, int max_frames, int channels)
{
    int got_frames = 0;
    const int frame_size = (int)sizeof(float) * channels;
    int chunk_frames = 7;

    while (got_frames < max_frames) {
        int want = chunk_frames;
        if (want > max_frames - got_frames) {
            want = max_frames - got_frames;
        }
        int got = SDL_GetAudioStreamData(stream, &out[got_frames * channels], want * frame_size);
        if (got < 0 || (got % frame_size) != 0) {
            return -1;
        }
        if (got == 0) {
            break;
        }
        got_frames += got / frame_size;
        chunk_frames = (chunk_frames == 7) ? 13 : 7;  // alternate odd chunk sizes
    }
    return got_frames;
}

static bool f32_equal(float a, float b)
{
    return SDL_fabsf(a - b) <= 2.0e-6f;
}

// Compare every sample of two equal-length float buffers.
static bool buffers_match(const float *a, const float *b, int num_samples)
{
    for (int i = 0; i < num_samples; ++i) {
        if (!f32_equal(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

// Feed `num_frames` of the shared stereo signal into an independent reference
// stream (the same way the producer was fed) and return its drained output.
// Mono reference material uses the left channel of the signal.
static int reference_output(const SDL_AudioSpec *in_spec, const SDL_AudioSpec *out_spec,
                            int num_frames, int frame_offset, float *out, int max_frames)
{
    SDL_AudioStream *ref = SDL_CreateAudioStream(in_spec, out_spec);
    if (!ref) {
        return -1;
    }

    const int in_frame_size = SDL_AUDIO_FRAMESIZE(*in_spec);
    Uint8 *in = (Uint8 *)SDL_malloc((size_t)num_frames * (size_t)in_frame_size);
    if (!in) {
        SDL_DestroyAudioStream(ref);
        return -1;
    }

    if (in_spec->format == SDL_AUDIO_F32 && in_spec->channels == 2) {
        fill_stereo((float *)in, num_frames, frame_offset);
    } else {
        float *f = (float *)in;
        for (int i = 0; i < num_frames; ++i) {
            float l, r;
            make_frame(frame_offset + i, &l, &r);
            f[i] = l;
        }
    }

    if (!SDL_PutAudioStreamData(ref, in, num_frames * in_frame_size) ||
        !SDL_FlushAudioStream(ref)) {
        SDL_free(in);
        SDL_DestroyAudioStream(ref);
        return -1;
    }
    int got = drain_f32(ref, out, max_frames, out_spec->channels);

    SDL_free(in);
    SDL_DestroyAudioStream(ref);
    return got;
}

// ---------------------------------------------------------------------------
// fanout group
// ---------------------------------------------------------------------------

static int fanout_basic(void)
{
    const SDL_AudioSpec spec = make_spec(SDL_AUDIO_F32, 2, 48000);
    const int N = 257;  // odd number of frames, deliberately not chunk-aligned

    SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
    TASK_CHECK(p != NULL);
    SDL_AudioStream *t1 = SDL_CreateAudioStreamTap(p, &spec);
    TASK_CHECK(t1 != NULL && t1 != p);
    SDL_AudioStream *t2 = SDL_CreateAudioStreamTap(p, &spec);
    TASK_CHECK(t2 != NULL && t2 != p && t2 != t1);

    float in[257 * 2];
    fill_stereo(in, N, 0);

    TASK_CHECK(SDL_PutAudioStreamData(p, in, sizeof(in)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    float out_p[257 * 2], out_t1[257 * 2], out_t2[257 * 2];
    TASK_CHECK(drain_f32(p, out_p, N, 2) == N);
    TASK_CHECK(drain_f32(t1, out_t1, N, 2) == N);
    TASK_CHECK(drain_f32(t2, out_t2, N, 2) == N);
    TASK_CHECK(buffers_match(in, out_p, N * 2));
    TASK_CHECK(buffers_match(in, out_t1, N * 2));
    TASK_CHECK(buffers_match(in, out_t2, N * 2));

    SDL_DestroyAudioStream(t1);
    SDL_DestroyAudioStream(t2);
    SDL_DestroyAudioStream(p);
    return 0;
}

static int nocopy_dones;
static void SDLCALL nocopy_done(void *userdata, const void *buf, int len)
{
    (void)userdata;
    (void)buf;
    (void)len;
    ++nocopy_dones;
}

static void SDLCALL stream_cb(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
    (void)userdata;
    (void)stream;
    (void)additional_amount;
    (void)total_amount;
}

static int fanout_put_kinds(void)
{
    const SDL_AudioSpec spec = make_spec(SDL_AUDIO_F32, 2, 48000);
    const int N = 64;

    SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
    SDL_AudioStream *t = SDL_CreateAudioStreamTap(p, &spec);
    TASK_CHECK(p && t);

    // 1. Ordinary Put (copied before the call returns, so a stack buffer is fine).
    float ordinary[64 * 2];
    fill_stereo(ordinary, N, 0);
    TASK_CHECK(SDL_PutAudioStreamData(p, ordinary, sizeof(ordinary)));

    // 2. NoCopy Put with a completion callback.
    float *nocopy = (float *)SDL_malloc(sizeof(float) * 2 * N);
    TASK_CHECK(nocopy != NULL);
    fill_stereo(nocopy, N, N);
    nocopy_dones = 0;
    TASK_CHECK(SDL_PutAudioStreamDataNoCopy(p, nocopy, (int)sizeof(float) * 2 * N, nocopy_done, NULL));
    TASK_CHECK(nocopy_dones == 0);  // still queued, not released yet

    // 3. Planar Put.
    float planar_l[64], planar_r[64];
    for (int i = 0; i < N; ++i) {
        make_frame(2 * N + i, &planar_l[i], &planar_r[i]);
    }
    const void *planes[2] = { planar_l, planar_r };
    TASK_CHECK(SDL_PutAudioStreamPlanarData(p, planes, 2, N));

    TASK_CHECK(SDL_FlushAudioStream(p));

    float expect[192 * 2], got_p[192 * 2], got_t[192 * 2];
    fill_stereo(expect, 3 * N, 0);
    TASK_CHECK(drain_f32(p, got_p, 3 * N, 2) == 3 * N);
    TASK_CHECK(drain_f32(t, got_t, 3 * N, 2) == 3 * N);
    TASK_CHECK(buffers_match(expect, got_p, 3 * N * 2));
    TASK_CHECK(buffers_match(expect, got_t, 3 * N * 2));

    // Both consumers drained the NoCopy buffer; the callback fired exactly once.
    TASK_CHECK(nocopy_dones == 1);

    SDL_DestroyAudioStream(t);
    SDL_DestroyAudioStream(p);
    TASK_CHECK(nocopy_dones == 1);
    return 0;
}

static int fanout_late_join(void)
{
    const SDL_AudioSpec spec = make_spec(SDL_AUDIO_F32, 2, 48000);

    SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
    TASK_CHECK(p);

    float early[32 * 2];
    fill_stereo(early, 32, 0);
    TASK_CHECK(SDL_PutAudioStreamData(p, early, sizeof(early)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    // The tap is created after the first submission and must not see it.
    SDL_AudioStream *t = SDL_CreateAudioStreamTap(p, &spec);
    TASK_CHECK(t);
    TASK_CHECK(SDL_GetAudioStreamAvailable(t) == 0);

    float later[48 * 2];
    fill_stereo(later, 48, 32);
    TASK_CHECK(SDL_PutAudioStreamData(p, later, sizeof(later)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    float got[48 * 2];
    TASK_CHECK(drain_f32(t, got, 48, 2) == 48);
    TASK_CHECK(buffers_match(later, got, 48 * 2));

    float got_p[80 * 2];
    TASK_CHECK(drain_f32(p, got_p, 80, 2) == 80);
    TASK_CHECK(buffers_match(early, got_p, 32 * 2));
    TASK_CHECK(buffers_match(later, &got_p[32 * 2], 48 * 2));

    SDL_DestroyAudioStream(t);
    SDL_DestroyAudioStream(p);
    return 0;
}

static int fanout_individual_settings(void)
{
    const SDL_AudioSpec in = make_spec(SDL_AUDIO_F32, 2, 48000);
    const SDL_AudioSpec out = make_spec(SDL_AUDIO_F32, 2, 48000);
    const int N = 96;

    SDL_AudioStream *p = SDL_CreateAudioStream(&in, &out);
    SDL_AudioStream *quiet = SDL_CreateAudioStreamTap(p, &out);
    SDL_AudioStream *loud = SDL_CreateAudioStreamTap(p, &out);
    TASK_CHECK(p && quiet && loud);

    TASK_CHECK(SDL_SetAudioStreamGain(quiet, 0.25f));
    TASK_CHECK(SDL_SetAudioStreamGain(loud, 2.0f));
    TASK_CHECK(SDL_GetAudioStreamGain(quiet) == 0.25f);
    TASK_CHECK(SDL_GetAudioStreamGain(loud) == 2.0f);
    TASK_CHECK(SDL_GetAudioStreamGain(p) == 1.0f);  // producer untouched

    float sig[96 * 2];
    fill_stereo(sig, N, 0);
    TASK_CHECK(SDL_PutAudioStreamData(p, sig, sizeof(sig)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    float gq[96 * 2], gl[96 * 2], gp[96 * 2];
    TASK_CHECK(drain_f32(quiet, gq, N, 2) == N);
    TASK_CHECK(drain_f32(loud, gl, N, 2) == N);
    TASK_CHECK(drain_f32(p, gp, N, 2) == N);

    for (int i = 0; i < N * 2; ++i) {
        TASK_CHECK(f32_equal(gq[i], sig[i] * 0.25f));
        TASK_CHECK(f32_equal(gl[i], sig[i] * 2.0f));
        TASK_CHECK(f32_equal(gp[i], sig[i]));
        TASK_CHECK(!f32_equal(gq[i], gl[i]));  // strict non-zero signal keeps the gains distinct
    }

    SDL_DestroyAudioStream(quiet);
    SDL_DestroyAudioStream(loud);
    SDL_DestroyAudioStream(p);
    return 0;
}

static int fanout_slow_clear(void)
{
    const SDL_AudioSpec in = make_spec(SDL_AUDIO_F32, 2, 48000);
    const SDL_AudioSpec out = make_spec(SDL_AUDIO_F32, 2, 32000);  // slow consumer

    SDL_AudioStream *p = SDL_CreateAudioStream(&in, &in);
    SDL_AudioStream *slow = SDL_CreateAudioStreamTap(p, &out);
    TASK_CHECK(p && slow);

    float first[100 * 2];
    fill_stereo(first, 100, 0);
    TASK_CHECK(SDL_PutAudioStreamData(p, first, sizeof(first)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    // The slow tap has queued audio; clearing drops only its own copy.
    TASK_CHECK(SDL_GetAudioStreamAvailable(slow) > 0);
    TASK_CHECK(SDL_ClearAudioStream(slow));
    TASK_CHECK(SDL_GetAudioStreamAvailable(slow) == 0);
    TASK_CHECK(SDL_GetAudioStreamQueued(slow) == 0);

    // Future submissions still arrive at the tap; the producer never noticed.
    float second[100 * 2];
    fill_stereo(second, 100, 100);
    TASK_CHECK(SDL_PutAudioStreamData(p, second, sizeof(second)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    float ref[256], got[256];
    int ref_frames = reference_output(&in, &out, 100, 100, ref, 256);
    TASK_CHECK(ref_frames > 0);
    int got_frames = drain_f32(slow, got, 256, out.channels);
    TASK_CHECK(got_frames == ref_frames);
    TASK_CHECK(buffers_match(ref, got, ref_frames * out.channels));

    float got_p[200 * 2];
    TASK_CHECK(drain_f32(p, got_p, 200, 2) == 200);  // the producer kept both segments
    TASK_CHECK(buffers_match(first, got_p, 100 * 2));
    TASK_CHECK(buffers_match(second, &got_p[100 * 2], 100 * 2));

    SDL_DestroyAudioStream(slow);
    SDL_DestroyAudioStream(p);
    return 0;
}

static int fanout_individual_freq_ratio(void)
{
    const SDL_AudioSpec in = make_spec(SDL_AUDIO_F32, 2, 48000);
    const SDL_AudioSpec out = make_spec(SDL_AUDIO_F32, 2, 48000);
    const int N = 480;

    SDL_AudioStream *p = SDL_CreateAudioStream(&in, &out);
    SDL_AudioStream *normal = SDL_CreateAudioStreamTap(p, &out);
    SDL_AudioStream *slowed = SDL_CreateAudioStreamTap(p, &out);
    TASK_CHECK(p && normal && slowed);
    TASK_CHECK(SDL_SetAudioStreamFrequencyRatio(slowed, 0.5f));
    TASK_CHECK(SDL_GetAudioStreamFrequencyRatio(slowed) == 0.5f);
    TASK_CHECK(SDL_GetAudioStreamFrequencyRatio(normal) == 1.0f);

    float sig[480 * 2];
    fill_stereo(sig, N, 0);
    TASK_CHECK(SDL_PutAudioStreamData(p, sig, sizeof(sig)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    // Independent reference streams with the same ratios.
    SDL_AudioStream *rn = SDL_CreateAudioStream(&in, &out);
    SDL_AudioStream *rs = SDL_CreateAudioStream(&in, &out);
    TASK_CHECK(rn && rs);
    TASK_CHECK(SDL_SetAudioStreamFrequencyRatio(rs, 0.5f));
    TASK_CHECK(SDL_PutAudioStreamData(rn, sig, sizeof(sig)));
    TASK_CHECK(SDL_PutAudioStreamData(rs, sig, sizeof(sig)));
    TASK_CHECK(SDL_FlushAudioStream(rn));
    TASK_CHECK(SDL_FlushAudioStream(rs));

    float gn[512 * 2], gs[512 * 2], en[512 * 2], es[512 * 2];
    int cn = drain_f32(normal, gn, 512, 2);
    int cs = drain_f32(slowed, gs, 512, 2);
    int en_n = drain_f32(rn, en, 512, 2);
    int es_n = drain_f32(rs, es, 512, 2);
    TASK_CHECK(cn == en_n && cs == es_n);
    // A 0.5 ratio halves the effective input rate, so the input spans twice
    // as many output frames.
    TASK_CHECK(cn > 0 && cs > 0 && cs > cn);
    TASK_CHECK(buffers_match(en, gn, cn * 2));
    TASK_CHECK(buffers_match(es, gs, cs * 2));

    SDL_DestroyAudioStream(rn);
    SDL_DestroyAudioStream(rs);
    SDL_DestroyAudioStream(slowed);
    SDL_DestroyAudioStream(normal);
    SDL_DestroyAudioStream(p);
    return 0;
}

static int fanout_input_channel_map(void)
{
    const SDL_AudioSpec spec = make_spec(SDL_AUDIO_F32, 2, 48000);
    const int swap[2] = { 1, 0 };

    SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
    TASK_CHECK(p);
    TASK_CHECK(SDL_SetAudioStreamInputChannelMap(p, swap, 2));
    SDL_AudioStream *t = SDL_CreateAudioStreamTap(p, &spec);
    TASK_CHECK(t);

    const int N = 64;
    float sig[64 * 2];
    fill_stereo(sig, N, 0);
    TASK_CHECK(SDL_PutAudioStreamData(p, sig, sizeof(sig)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    // An ordinary reference stream with the same input map.
    SDL_AudioStream *ref = SDL_CreateAudioStream(&spec, &spec);
    TASK_CHECK(ref);
    TASK_CHECK(SDL_SetAudioStreamInputChannelMap(ref, swap, 2));
    TASK_CHECK(SDL_PutAudioStreamData(ref, sig, sizeof(sig)));
    TASK_CHECK(SDL_FlushAudioStream(ref));

    float got[64 * 2], expect[64 * 2];
    TASK_CHECK(drain_f32(t, got, N, 2) == N);
    TASK_CHECK(drain_f32(ref, expect, N, 2) == N);
    TASK_CHECK(buffers_match(expect, got, N * 2));

    // The channels really were swapped on both consumers.
    for (int i = 0; i < N; ++i) {
        TASK_CHECK(f32_equal(got[i * 2], sig[i * 2 + 1]));
        TASK_CHECK(f32_equal(got[i * 2 + 1], sig[i * 2]));
    }

    // A later channel-map change on the producer propagates to the existing tap.
    TASK_CHECK(SDL_SetAudioStreamInputChannelMap(p, NULL, 2));
    int *map = SDL_GetAudioStreamInputChannelMap(t, NULL);
    TASK_CHECK(map == NULL);  // default mapping again
    SDL_free(map);

    SDL_DestroyAudioStream(ref);
    SDL_DestroyAudioStream(t);
    SDL_DestroyAudioStream(p);
    return 0;
}

static int put_cb_calls;
static int put_cb_bytes;
static void SDLCALL recording_put_cb(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
    (void)userdata;
    (void)stream;
    (void)total_amount;
    ++put_cb_calls;
    put_cb_bytes += additional_amount;
}

static int get_cb_calls;
static void SDLCALL feeding_get_cb(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
    (void)userdata;
    (void)total_amount;
    ++get_cb_calls;
    if (additional_amount > 0) {
        int frames = additional_amount / (2 * (int)sizeof(float));
        if (frames > 64) {
            frames = 64;
        }
        float refill[64 * 2];
        fill_stereo(refill, frames, 0);
        SDL_PutAudioStreamData(stream, refill, frames * 2 * (int)sizeof(float));
    }
}

static int fanout_producer_callbacks(void)
{
    const SDL_AudioSpec spec = make_spec(SDL_AUDIO_F32, 2, 48000);
    const int N = 48;

    SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
    SDL_AudioStream *t = SDL_CreateAudioStreamTap(p, &spec);
    TASK_CHECK(p && t);
    TASK_CHECK(SDL_SetAudioStreamPutCallback(p, recording_put_cb, NULL));

    float sig[48 * 2];
    fill_stereo(sig, N, 0);
    put_cb_calls = 0;
    put_cb_bytes = 0;
    TASK_CHECK(SDL_PutAudioStreamData(p, sig, sizeof(sig)));
    TASK_CHECK(put_cb_calls == 1);
    TASK_CHECK(put_cb_bytes == (int)sizeof(sig));  // producer callback unchanged

    TASK_CHECK(SDL_FlushAudioStream(p));
    float got_t[48 * 2];
    TASK_CHECK(drain_f32(t, got_t, N, 2) == N);
    TASK_CHECK(buffers_match(sig, got_t, N * 2));

    // The producer's get callback still supplies data on demand; the tap
    // neither needs nor is allowed to have its own get callback.
    TASK_CHECK(SDL_SetAudioStreamGetCallback(p, feeding_get_cb, NULL));
    get_cb_calls = 0;
    float demand[32 * 2];
    int got = SDL_GetAudioStreamData(p, demand, sizeof(demand));
    TASK_CHECK(got == (int)sizeof(demand));
    TASK_CHECK(get_cb_calls >= 1);

    SDL_DestroyAudioStream(t);
    SDL_DestroyAudioStream(p);
    return 0;
}

// Condvar-driven overlap: the reader is released into a Get loop immediately
// before the writer starts putting, with no sleeping.
static SDL_Mutex *overlap_mtx;
static SDL_Condition *overlap_cond;
static SDL_AtomicInt overlap_go;
static SDL_AtomicInt overlap_stop;
static SDL_AudioStream *overlap_tap;
static int overlap_read_frames;

static int SDLCALL overlap_reader(void *userdata)
{
    (void)userdata;
    SDL_LockMutex(overlap_mtx);
    while (!SDL_GetAtomicInt(&overlap_go)) {
        SDL_WaitCondition(overlap_cond, overlap_mtx);
    }
    SDL_UnlockMutex(overlap_mtx);

    float tmp[16];
    while (!SDL_GetAtomicInt(&overlap_stop)) {
        int got = SDL_GetAudioStreamData(overlap_tap, tmp, sizeof(tmp));
        if (got > 0) {
            overlap_read_frames += got / (2 * (int)sizeof(float));
        }
    }
    // Drain whatever remains after the writer stops.
    for (;;) {
        int got = SDL_GetAudioStreamData(overlap_tap, tmp, sizeof(tmp));
        if (got <= 0) {
            break;
        }
        overlap_read_frames += got / (2 * (int)sizeof(float));
    }
    return 0;
}

static int fanout_thread_overlap(void)
{
    const SDL_AudioSpec spec = make_spec(SDL_AUDIO_F32, 2, 48000);

    overlap_mtx = SDL_CreateMutex();
    overlap_cond = SDL_CreateCondition();
    TASK_CHECK(overlap_mtx && overlap_cond);
    SDL_SetAtomicInt(&overlap_go, 0);
    SDL_SetAtomicInt(&overlap_stop, 0);
    overlap_read_frames = 0;

    SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
    overlap_tap = SDL_CreateAudioStreamTap(p, &spec);
    TASK_CHECK(p && overlap_tap);

    SDL_Thread *th = SDL_CreateThread(overlap_reader, "tap-reader", NULL);
    TASK_CHECK(th != NULL);

    SDL_LockMutex(overlap_mtx);
    SDL_SetAtomicInt(&overlap_go, 1);
    SDL_BroadcastCondition(overlap_cond);
    SDL_UnlockMutex(overlap_mtx);

    // The reader is now spinning in SDL_GetAudioStreamData; interleave puts.
    const int BATCH = 50, BATCHES = 10;
    float block[50 * 2];
    for (int b = 0; b < BATCHES; ++b) {
        fill_stereo(block, BATCH, b * BATCH);
        TASK_CHECK(SDL_PutAudioStreamData(p, block, sizeof(block)));
    }
    TASK_CHECK(SDL_FlushAudioStream(p));

    SDL_SetAtomicInt(&overlap_stop, 1);
    int status = -1;
    SDL_WaitThread(th, &status);
    TASK_CHECK(status == 0);

    TASK_CHECK(overlap_read_frames == BATCH * BATCHES);

    // Nothing left in the tap after the reader's final drain.
    TASK_CHECK(SDL_GetAudioStreamAvailable(overlap_tap) == 0);

    SDL_DestroyMutex(overlap_mtx);
    SDL_DestroyCondition(overlap_cond);
    SDL_DestroyAudioStream(overlap_tap);
    SDL_DestroyAudioStream(p);
    return 0;
}

static int fanout_destroy_orders(void)
{
    const SDL_AudioSpec spec = make_spec(SDL_AUDIO_F32, 2, 48000);
    float sig[32 * 2];
    fill_stereo(sig, 32, 0);

    // Order 1: destroy taps first, then the producer.
    {
        SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
        SDL_AudioStream *t1 = SDL_CreateAudioStreamTap(p, &spec);
        SDL_AudioStream *t2 = SDL_CreateAudioStreamTap(p, &spec);
        TASK_CHECK(p && t1 && t2);
        TASK_CHECK(SDL_PutAudioStreamData(p, sig, sizeof(sig)));
        SDL_DestroyAudioStream(t1);
        SDL_DestroyAudioStream(t2);
        // The producer keeps working after its taps are gone.
        TASK_CHECK(SDL_PutAudioStreamData(p, sig, sizeof(sig)));
        TASK_CHECK(SDL_FlushAudioStream(p));
        float got[64 * 2];
        TASK_CHECK(drain_f32(p, got, 64, 2) == 64);
        SDL_DestroyAudioStream(p);
    }

    // Order 2: destroy the producer first; surviving taps drain accepted audio.
    {
        SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
        SDL_AudioStream *t = SDL_CreateAudioStreamTap(p, &spec);
        TASK_CHECK(p && t);
        TASK_CHECK(SDL_PutAudioStreamData(p, sig, sizeof(sig)));
        TASK_CHECK(SDL_FlushAudioStream(p));
        SDL_DestroyAudioStream(p);

        float got[32 * 2];
        TASK_CHECK(drain_f32(t, got, 32, 2) == 32);
        TASK_CHECK(buffers_match(sig, got, 32 * 2));
        SDL_DestroyAudioStream(t);
    }

    // Order 3: destroy a tap while another tap and the producer keep running.
    {
        SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
        SDL_AudioStream *t1 = SDL_CreateAudioStreamTap(p, &spec);
        SDL_AudioStream *t2 = SDL_CreateAudioStreamTap(p, &spec);
        TASK_CHECK(p && t1 && t2);
        SDL_DestroyAudioStream(t1);
        TASK_CHECK(SDL_PutAudioStreamData(p, sig, sizeof(sig)));
        TASK_CHECK(SDL_FlushAudioStream(p));
        float got[32 * 2];
        TASK_CHECK(drain_f32(t2, got, 32, 2) == 32);
        TASK_CHECK(buffers_match(sig, got, 32 * 2));
        SDL_DestroyAudioStream(t2);
        SDL_DestroyAudioStream(p);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ownership group
// ---------------------------------------------------------------------------

static int ownership_nocopy_lifetime(void)
{
    const SDL_AudioSpec spec = make_spec(SDL_AUDIO_F32, 2, 48000);
    const int N = 40;

    // With 1, 2 and 4 consumers the single NoCopy callback fires exactly
    // once, and only after every consumer (producer + taps) released it.
    for (int num_taps = 0; num_taps <= 3; num_taps += (num_taps == 0 ? 1 : (num_taps == 1 ? 1 : 2))) {
        SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
        TASK_CHECK(p);
        SDL_AudioStream *taps[4];
        for (int i = 0; i < num_taps; ++i) {
            taps[i] = SDL_CreateAudioStreamTap(p, &spec);
            TASK_CHECK(taps[i]);
        }

        float *buf = (float *)SDL_malloc(sizeof(float) * 2 * N);
        TASK_CHECK(buf);
        fill_stereo(buf, N, 0);
        nocopy_dones = 0;
        TASK_CHECK(SDL_PutAudioStreamDataNoCopy(p, buf, (int)sizeof(float) * 2 * N, nocopy_done, NULL));
        TASK_CHECK(nocopy_dones == 0);
        TASK_CHECK(SDL_FlushAudioStream(p));

        if (num_taps >= 1) {
            // Producer disappears first: the buffer must stay alive for taps.
            SDL_DestroyAudioStream(p);
            TASK_CHECK(nocopy_dones == 0);
            for (int i = 0; i < num_taps; ++i) {
                float got[40 * 2];
                TASK_CHECK(drain_f32(taps[i], got, N, 2) == N);
                TASK_CHECK(buffers_match(buf, got, N * 2));
                if (i + 1 < num_taps) {
                    TASK_CHECK(nocopy_dones == 0);  // another consumer still needs it
                }
            }
            // The exact-size final read leaves the flushed head in place; one
            // more (empty) read on each tap releases its shared-buffer ref.
            for (int i = 0; i < num_taps; ++i) {
                float one[2];
                TASK_CHECK(SDL_GetAudioStreamData(taps[i], one, sizeof(one)) == 0);
            }
            TASK_CHECK(nocopy_dones == 1);
            for (int i = 0; i < num_taps; ++i) {
                SDL_DestroyAudioStream(taps[i]);
            }
        } else {
            float got[40 * 2];
            TASK_CHECK(drain_f32(p, got, N, 2) == N);
            float one[2];
            TASK_CHECK(SDL_GetAudioStreamData(p, one, sizeof(one)) == 0);
            TASK_CHECK(nocopy_dones == 1);
            SDL_DestroyAudioStream(p);
        }
        TASK_CHECK(nocopy_dones == 1);
    }
    return 0;
}

// Counting allocator: SDL_GetNumAllocations() is compiled out of this
// configuration, so wrap the original memory functions and count cumulative
// allocations made inside a measured region. The wrappers always delegate to
// the original functions, so replacing them late stays correct.
static SDL_malloc_func real_malloc;
static SDL_calloc_func real_calloc;
static SDL_realloc_func real_realloc;
static SDL_free_func real_free;
static SDL_AtomicInt count_active;
static SDL_AtomicInt count_allocs;
static SDL_AtomicInt count_bytes;
static SDL_AtomicInt inject_fault;  // when set, the next malloc/calloc fails once

static void *count_malloc(size_t size)
{
    if (SDL_GetAtomicInt(&inject_fault)) {
        SDL_SetAtomicInt(&inject_fault, 0);
        return NULL;
    }
    void *p = real_malloc(size);
    if (p && SDL_GetAtomicInt(&count_active)) {
        SDL_AtomicIncRef(&count_allocs);
        SDL_AddAtomicInt(&count_bytes, (int)SDL_min(size, (size_t)SDL_MAX_SINT32));
    }
    return p;
}

static void *count_calloc(size_t nmemb, size_t size)
{
    if (SDL_GetAtomicInt(&inject_fault)) {
        SDL_SetAtomicInt(&inject_fault, 0);
        return NULL;
    }
    void *p = real_calloc(nmemb, size);
    if (p && SDL_GetAtomicInt(&count_active)) {
        SDL_AtomicIncRef(&count_allocs);
        SDL_AddAtomicInt(&count_bytes, (int)SDL_min(nmemb * size, (size_t)SDL_MAX_SINT32));
    }
    return p;
}

static void *count_realloc(void *mem, size_t size)
{
    if (!mem) {
        void *p = real_realloc(NULL, size);
        if (p && SDL_GetAtomicInt(&count_active)) {
            SDL_AtomicIncRef(&count_allocs);
            SDL_AddAtomicInt(&count_bytes, (int)SDL_min(size, (size_t)SDL_MAX_SINT32));
        }
        return p;
    }
    return real_realloc(mem, size);
}

static void count_free(void *mem)
{
    if (mem) {
        real_free(mem);
    }
}

static void install_counter(void)
{
    if (!real_malloc) {
        SDL_GetOriginalMemoryFunctions(&real_malloc, &real_calloc, &real_realloc, &real_free);
        SDL_SetMemoryFunctions(count_malloc, count_calloc, count_realloc, count_free);
    }
}

// Measure how many allocations (and bytes) one long segment adds. The PCM
// backing is shared, so the per-tap growth must be tiny metadata and must not
// grow when the segment itself gets four times larger.
static int ownership_memory_growth(void)
{
    const SDL_AudioSpec spec = make_spec(SDL_AUDIO_F32, 2, 48000);
    const int segment_bytes_small = 96 * 1024;
    const int segment_bytes_large = segment_bytes_small * 4;
    int allocs_small[5], bytes_small[5];
    int allocs_large[5], bytes_large[5];

    install_counter();

    for (int pass = 0; pass < 2; ++pass) {
        const int segment_bytes = pass ? segment_bytes_large : segment_bytes_small;
        int *allocs = pass ? allocs_large : allocs_small;
        int *bytes = pass ? bytes_large : bytes_small;

        for (int num_taps = 0; num_taps <= 4; ++num_taps) {
            SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
            SDL_AudioStream *taps[4];
            for (int i = 0; i < num_taps; ++i) {
                taps[i] = SDL_CreateAudioStreamTap(p, &spec);
                TASK_CHECK(taps[i]);
            }

            Uint8 *segment = (Uint8 *)SDL_malloc(segment_bytes);
            TASK_CHECK(segment);
            SDL_memset(segment, 0x55, segment_bytes);

            SDL_SetAtomicInt(&count_allocs, 0);
            SDL_SetAtomicInt(&count_bytes, 0);
            SDL_SetAtomicInt(&count_active, 1);
            TASK_CHECK(SDL_PutAudioStreamData(p, segment, segment_bytes));
            SDL_SetAtomicInt(&count_active, 0);
            allocs[num_taps] = SDL_GetAtomicInt(&count_allocs);
            bytes[num_taps] = SDL_GetAtomicInt(&count_bytes);

            SDL_free(segment);  // ordinary Put copied it; the caller may free immediately
            TASK_CHECK(SDL_FlushAudioStream(p));
            for (int i = 0; i < num_taps; ++i) {
                SDL_DestroyAudioStream(taps[i]);
            }
            SDL_DestroyAudioStream(p);
        }
    }

    for (int n = 0; n <= 4; ++n) {
        // Exactly one segment-sized PCM backing plus small bookkeeping:
        // never a second 96 KB copy, regardless of how many taps exist.
        TASK_CHECK(allocs_small[n] >= 1);
        TASK_CHECK(bytes_small[n] >= segment_bytes_small);
        TASK_CHECK(bytes_small[n] <= segment_bytes_small + 4096);
        TASK_CHECK(bytes_large[n] >= segment_bytes_large);
        TASK_CHECK(bytes_large[n] <= segment_bytes_large + 4096);

        // Per-tap allocation growth is bounded metadata, not PCM duplicates.
        TASK_CHECK(allocs_small[n] - allocs_small[0] <= 8);
        TASK_CHECK(allocs_large[n] - allocs_large[0] <= 8);
        TASK_CHECK(bytes_small[n] - bytes_small[0] < 8192);
        TASK_CHECK(bytes_large[n] - bytes_large[0] < 8192);
    }

    // Quadrupling the segment adds one segment's worth of bytes at every tap
    // count; with per-tap PCM copies it would add (num_taps + 1) segments.
    for (int n = 0; n <= 4; ++n) {
        const int added = bytes_large[n] - bytes_small[n];
        TASK_CHECK(added >= 3 * segment_bytes_small - 4096);
        TASK_CHECK(added <= 3 * segment_bytes_small + 4096);
    }
    return 0;
}

static int ownership_illegal_ops(void)
{
    const SDL_AudioSpec spec = make_spec(SDL_AUDIO_F32, 2, 48000);
    const SDL_AudioSpec other = make_spec(SDL_AUDIO_S16, 2, 48000);
    float x[4] = { 0.1f, -0.2f, 0.3f, -0.4f };
    int map[2] = { 1, 0 };

    SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
    SDL_AudioStream *t = SDL_CreateAudioStreamTap(p, &spec);
    TASK_CHECK(p && t);

    // A tap cannot be tapped itself.
    TASK_CHECK(SDL_CreateAudioStreamTap(t, &spec) == NULL);

    // No data entry point accepts a tap.
    TASK_CHECK(!SDL_PutAudioStreamData(t, x, sizeof(x)));
    TASK_CHECK(!SDL_PutAudioStreamDataNoCopy(t, x, sizeof(x), nocopy_done, NULL));
    const void *planes[2] = { x, x + 2 };
    TASK_CHECK(!SDL_PutAudioStreamPlanarData(t, planes, 2, 2));

    // Input-side configuration is off-limits.
    TASK_CHECK(!SDL_SetAudioStreamFormat(t, &other, NULL));
    TASK_CHECK(!SDL_SetAudioStreamInputChannelMap(t, map, 2));
    TASK_CHECK(!SDL_SetAudioStreamPutCallback(t, stream_cb, NULL));
    TASK_CHECK(!SDL_SetAudioStreamGetCallback(t, stream_cb, NULL));

    // Every failed operation left the producer and its fan-out intact.
    TASK_CHECK(SDL_PutAudioStreamData(p, x, sizeof(x)));
    TASK_CHECK(SDL_FlushAudioStream(p));
    float got[4];
    TASK_CHECK(SDL_GetAudioStreamData(t, got, sizeof(got)) == (int)sizeof(got));
    TASK_CHECK(buffers_match(x, got, 4));

    // Output-side configuration remains legal on a tap.
    TASK_CHECK(SDL_SetAudioStreamFormat(t, NULL, &other));
    TASK_CHECK(SDL_SetAudioStreamOutputChannelMap(t, NULL, 2));

    // Invalid constructor arguments fail without touching the producer.
    TASK_CHECK(SDL_CreateAudioStreamTap(NULL, &spec) == NULL);
    TASK_CHECK(SDL_CreateAudioStreamTap(p, NULL) == NULL);
    SDL_AudioSpec bogus = make_spec((SDL_AudioFormat)0, 2, 48000);
    TASK_CHECK(SDL_CreateAudioStreamTap(p, &bogus) == NULL);

    SDL_DestroyAudioStream(t);
    SDL_DestroyAudioStream(p);
    return 0;
}

// A failed submission (allocation fault) must not leave a partial track on
// the producer or any tap; the next good submission behaves exactly like a
// single-track reference.
static int ownership_submit_failure_rollback(void)
{
    const SDL_AudioSpec spec = make_spec(SDL_AUDIO_F32, 2, 48000);
    const int N = 64;

    install_counter();

    SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
    SDL_AudioStream *t1 = SDL_CreateAudioStreamTap(p, &spec);
    SDL_AudioStream *t2 = SDL_CreateAudioStreamTap(p, &spec);
    TASK_CHECK(p && t1 && t2);

    float first[64 * 2];
    fill_stereo(first, N, 0);
    TASK_CHECK(SDL_PutAudioStreamData(p, first, sizeof(first)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    float second[64 * 2];
    fill_stereo(second, N, N);

    // Fault the backing-buffer copy: the whole submission fails atomically.
    SDL_SetAtomicInt(&inject_fault, 1);
    TASK_CHECK(!SDL_PutAudioStreamData(p, second, sizeof(second)));
    TASK_CHECK(SDL_GetAtomicInt(&inject_fault) == 0);  // the fault was consumed

    // Nobody gained a partial track: drainable content is still just segment 1.
    float gp[128 * 2], g1[128 * 2], g2[128 * 2];
    TASK_CHECK(drain_f32(p, gp, 128, 2) == N);
    TASK_CHECK(drain_f32(t1, g1, 128, 2) == N);
    TASK_CHECK(drain_f32(t2, g2, 128, 2) == N);
    TASK_CHECK(buffers_match(first, gp, N * 2));
    TASK_CHECK(buffers_match(first, g1, N * 2));
    TASK_CHECK(buffers_match(first, g2, N * 2));

    // The follow-up submission works and fans out normally.
    TASK_CHECK(SDL_PutAudioStreamData(p, second, sizeof(second)));
    TASK_CHECK(SDL_FlushAudioStream(p));
    TASK_CHECK(drain_f32(t1, g1, 128, 2) == N);
    TASK_CHECK(buffers_match(second, g1, N * 2));
    TASK_CHECK(drain_f32(t2, g2, 128, 2) == N);
    TASK_CHECK(buffers_match(second, g2, N * 2));
    SDL_DestroyAudioStream(t1);
    SDL_DestroyAudioStream(t2);
    SDL_DestroyAudioStream(p);

    // Large ordinary Put (over the internal copy threshold) and NoCopy Put
    // must also fail atomically without leaking or double-freeing.
    SDL_AudioStream *p2 = SDL_CreateAudioStream(&spec, &spec);
    SDL_AudioStream *u1 = SDL_CreateAudioStreamTap(p2, &spec);
    TASK_CHECK(p2 && u1);
    const int big_bytes = 80 * 1024;
    float *big = (float *)SDL_malloc(big_bytes);
    TASK_CHECK(big);
    SDL_memset(big, 0x33, big_bytes);
    SDL_SetAtomicInt(&inject_fault, 1);
    TASK_CHECK(!SDL_PutAudioStreamData(p2, big, big_bytes));
    TASK_CHECK(SDL_GetAudioStreamAvailable(p2) == 0);
    TASK_CHECK(SDL_GetAudioStreamAvailable(u1) == 0);
    SDL_free(big);  // still ours: the failed submission did not free it

    float *nc = (float *)SDL_malloc(256);
    TASK_CHECK(nc);
    nocopy_dones = 0;
    SDL_SetAtomicInt(&inject_fault, 1);
    TASK_CHECK(!SDL_PutAudioStreamDataNoCopy(p2, nc, 256, nocopy_done, NULL));
    TASK_CHECK(nocopy_dones == 0);  // a rejected NoCopy never fires its callback
    TASK_CHECK(SDL_GetAudioStreamAvailable(u1) == 0);
    SDL_free(nc);

    SDL_DestroyAudioStream(u1);
    SDL_DestroyAudioStream(p2);
    return 0;
}

static int ownership_clear_isolation(void)
{
    const SDL_AudioSpec spec = make_spec(SDL_AUDIO_F32, 2, 48000);
    const int N = 48;

    SDL_AudioStream *p = SDL_CreateAudioStream(&spec, &spec);
    SDL_AudioStream *t1 = SDL_CreateAudioStreamTap(p, &spec);
    SDL_AudioStream *t2 = SDL_CreateAudioStreamTap(p, &spec);
    TASK_CHECK(p && t1 && t2);

    float sig[48 * 2];
    fill_stereo(sig, N, 0);
    TASK_CHECK(SDL_PutAudioStreamData(p, sig, sizeof(sig)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    TASK_CHECK(SDL_ClearAudioStream(t1));
    TASK_CHECK(SDL_GetAudioStreamAvailable(t1) == 0);

    float g2[48 * 2], gp[48 * 2];
    TASK_CHECK(drain_f32(t2, g2, N, 2) == N);
    TASK_CHECK(drain_f32(p, gp, N, 2) == N);
    TASK_CHECK(buffers_match(sig, g2, N * 2));
    TASK_CHECK(buffers_match(sig, gp, N * 2));

    SDL_DestroyAudioStream(t1);
    SDL_DestroyAudioStream(t2);
    SDL_DestroyAudioStream(p);
    return 0;
}

// ---------------------------------------------------------------------------
// conversion group
// ---------------------------------------------------------------------------

static int conversion_noninteger_rate(void)
{
    const SDL_AudioSpec in = make_spec(SDL_AUDIO_F32, 2, 48000);
    const SDL_AudioSpec fast_out = make_spec(SDL_AUDIO_F32, 2, 44100);
    const SDL_AudioSpec slow_out = make_spec(SDL_AUDIO_F32, 2, 32000);
    const int N = 503;  // deliberately awkward length

    SDL_AudioStream *p = SDL_CreateAudioStream(&in, &fast_out);
    SDL_AudioStream *fast = SDL_CreateAudioStreamTap(p, &fast_out);
    SDL_AudioStream *slow = SDL_CreateAudioStreamTap(p, &slow_out);
    TASK_CHECK(p && fast && slow);

    float sig[503 * 2];
    fill_stereo(sig, N, 0);
    TASK_CHECK(SDL_PutAudioStreamData(p, sig, sizeof(sig)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    float ref_fast[1024], ref_slow[1024], got_fast[1024], got_slow[1024];
    int rf = reference_output(&in, &fast_out, N, 0, ref_fast, 1024);
    int rs = reference_output(&in, &slow_out, N, 0, ref_slow, 1024);
    TASK_CHECK(rf > 0 && rs > 0 && rf != rs);

    int gf = drain_f32(fast, got_fast, 1024, 2);
    int gs = drain_f32(slow, got_slow, 1024, 2);
    TASK_CHECK(gf == rf);
    TASK_CHECK(gs == rs);
    TASK_CHECK(buffers_match(ref_fast, got_fast, rf * 2));
    TASK_CHECK(buffers_match(ref_slow, got_slow, rs * 2));

    // A tap joining late converts only its own input boundary onward.
    SDL_AudioStream *late = SDL_CreateAudioStreamTap(p, &slow_out);
    TASK_CHECK(late);
    const int N2 = 301;
    float more[301 * 2];
    fill_stereo(more, N2, N);
    TASK_CHECK(SDL_PutAudioStreamData(p, more, sizeof(more)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    float ref_late[1024], got_late[1024];
    int rl = reference_output(&in, &slow_out, N2, N, ref_late, 1024);
    int gl = drain_f32(late, got_late, 1024, 2);
    TASK_CHECK(rl > 0 && gl == rl);
    TASK_CHECK(buffers_match(ref_late, got_late, rl * 2));

    SDL_DestroyAudioStream(late);
    SDL_DestroyAudioStream(fast);
    SDL_DestroyAudioStream(slow);
    SDL_DestroyAudioStream(p);
    return 0;
}

// Switch the producer's input format mid-stream; every consumer must follow
// the change and match a reference stream fed the same segment sequence.
static int conversion_format_switch(void)
{
    const SDL_AudioSpec f32 = make_spec(SDL_AUDIO_F32, 2, 48000);
    const SDL_AudioSpec s16 = make_spec(SDL_AUDIO_S16, 2, 48000);
    const SDL_AudioSpec out = make_spec(SDL_AUDIO_F32, 2, 44100);

    SDL_AudioStream *p = SDL_CreateAudioStream(&f32, &out);
    SDL_AudioStream *t = SDL_CreateAudioStreamTap(p, &out);
    TASK_CHECK(p && t);

    const int N = 128;
    float fbuf[128 * 2];
    fill_stereo(fbuf, N, 0);
    TASK_CHECK(SDL_PutAudioStreamData(p, fbuf, sizeof(fbuf)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    // Reconfigure the producer's input end; the tap must follow automatically.
    TASK_CHECK(SDL_SetAudioStreamFormat(p, &s16, NULL));
    SDL_AudioSpec tap_src;
    TASK_CHECK(SDL_GetAudioStreamFormat(t, &tap_src, NULL) && tap_src.format == SDL_AUDIO_S16);

    Sint16 sbuf[128 * 2];
    for (int i = 0; i < N * 2; ++i) {
        sbuf[i] = (Sint16)((i * 257) - 32000);
    }
    TASK_CHECK(SDL_PutAudioStreamData(p, sbuf, sizeof(sbuf)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    float ref1[256], ref2[256], got[512];
    int r1 = reference_output(&f32, &out, N, 0, ref1, 256);
    TASK_CHECK(r1 > 0);

    // Reference for the s16 segment: convert through an s16 input stream.
    SDL_AudioStream *ref2s = SDL_CreateAudioStream(&s16, &out);
    TASK_CHECK(ref2s);
    TASK_CHECK(SDL_PutAudioStreamData(ref2s, sbuf, sizeof(sbuf)));
    TASK_CHECK(SDL_FlushAudioStream(ref2s));
    int r2 = drain_f32(ref2s, ref2, 256, 2);
    TASK_CHECK(r2 > 0);
    SDL_DestroyAudioStream(ref2s);

    int got_total = drain_f32(t, got, 512, 2);
    TASK_CHECK(got_total == r1 + r2);
    TASK_CHECK(buffers_match(ref1, got, r1 * 2));
    TASK_CHECK(buffers_match(ref2, &got[r1 * 2], r2 * 2));

    SDL_DestroyAudioStream(t);
    SDL_DestroyAudioStream(p);
    return 0;
}

static int conversion_channel_switch(void)
{
    const SDL_AudioSpec stereo = make_spec(SDL_AUDIO_F32, 2, 48000);
    const SDL_AudioSpec mono = make_spec(SDL_AUDIO_F32, 1, 48000);
    const SDL_AudioSpec out = make_spec(SDL_AUDIO_F32, 2, 32000);

    SDL_AudioStream *p = SDL_CreateAudioStream(&stereo, &out);
    SDL_AudioStream *t = SDL_CreateAudioStreamTap(p, &out);
    TASK_CHECK(p && t);

    const int N = 100;
    float sbuf[100 * 2];
    fill_stereo(sbuf, N, 0);
    TASK_CHECK(SDL_PutAudioStreamData(p, sbuf, sizeof(sbuf)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    TASK_CHECK(SDL_SetAudioStreamFormat(p, &mono, NULL));
    float mbuf[100];
    for (int i = 0; i < N; ++i) {
        float l, r;
        make_frame(N + i, &l, &r);
        mbuf[i] = l;  // match reference_output(): mono uses the left channel
    }
    TASK_CHECK(SDL_PutAudioStreamData(p, mbuf, sizeof(mbuf)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    // Back to stereo again.
    TASK_CHECK(SDL_SetAudioStreamFormat(p, &stereo, NULL));
    float sbuf2[50 * 2];
    fill_stereo(sbuf2, 50, 2 * N);
    TASK_CHECK(SDL_PutAudioStreamData(p, sbuf2, sizeof(sbuf2)));
    TASK_CHECK(SDL_FlushAudioStream(p));

    // Independent references, one per input configuration.
    float r1[256], r2[256], r3[256], got[768];
    int c1 = reference_output(&stereo, &out, N, 0, r1, 256);
    int c2 = reference_output(&mono, &out, N, N, r2, 256);
    int c3 = reference_output(&stereo, &out, 50, 2 * N, r3, 256);
    TASK_CHECK(c1 > 0 && c2 > 0 && c3 > 0);

    int total = drain_f32(t, got, 768, 2);
    TASK_CHECK(total == c1 + c2 + c3);
    TASK_CHECK(buffers_match(r1, got, c1 * 2));
    TASK_CHECK(buffers_match(r2, &got[c1 * 2], c2 * 2));
    TASK_CHECK(buffers_match(r3, &got[(c1 + c2) * 2], c3 * 2));

    SDL_DestroyAudioStream(t);
    SDL_DestroyAudioStream(p);
    return 0;
}

static int conversion_short_tail(void)
{
    const SDL_AudioSpec in = make_spec(SDL_AUDIO_F32, 2, 48000);
    const SDL_AudioSpec out = make_spec(SDL_AUDIO_F32, 2, 44100);

    SDL_AudioStream *p = SDL_CreateAudioStream(&in, &out);
    SDL_AudioStream *t = SDL_CreateAudioStreamTap(p, &out);
    TASK_CHECK(p && t);

    // Many tiny flushed fragments: resampler state must not drift between
    // the tap and an identically-fed reference.
    const int FRAG = 3;
    float ref_got[512], tap_got[512];
    SDL_AudioStream *ref = SDL_CreateAudioStream(&in, &out);
    TASK_CHECK(ref);

    int frame = 0;
    for (int frag = 0; frag < 20; ++frag) {
        float frag_buf[3 * 2];
        fill_stereo(frag_buf, FRAG, frame);
        TASK_CHECK(SDL_PutAudioStreamData(p, frag_buf, sizeof(frag_buf)));
        TASK_CHECK(SDL_FlushAudioStream(p));
        TASK_CHECK(SDL_PutAudioStreamData(ref, frag_buf, sizeof(frag_buf)));
        TASK_CHECK(SDL_FlushAudioStream(ref));
        frame += FRAG;
    }

    int ref_frames = drain_f32(ref, ref_got, 512, 2);
    int tap_frames = drain_f32(t, tap_got, 512, 2);
    TASK_CHECK(ref_frames > 0);
    TASK_CHECK(tap_frames == ref_frames);
    TASK_CHECK(buffers_match(ref_got, tap_got, ref_frames * 2));

    SDL_DestroyAudioStream(ref);
    SDL_DestroyAudioStream(t);
    SDL_DestroyAudioStream(p);
    return 0;
}

static int conversion_refill_after_flush(void)
{
    const SDL_AudioSpec in = make_spec(SDL_AUDIO_F32, 2, 48000);
    const SDL_AudioSpec out = make_spec(SDL_AUDIO_F32, 1, 22050);  // downmix + non-integer rate

    SDL_AudioStream *p = SDL_CreateAudioStream(&in, &out);
    SDL_AudioStream *t = SDL_CreateAudioStreamTap(p, &out);
    SDL_AudioStream *ref = SDL_CreateAudioStream(&in, &out);
    TASK_CHECK(p && t && ref);

    int frame = 0;
    float tap_all[1024], ref_all[1024];
    int tap_frames = 0, ref_frames = 0;

    for (int round = 0; round < 4; ++round) {
        const int N = 71;
        float buf[71 * 2];
        fill_stereo(buf, N, frame);
        TASK_CHECK(SDL_PutAudioStreamData(p, buf, sizeof(buf)));
        TASK_CHECK(SDL_PutAudioStreamData(ref, buf, sizeof(buf)));
        TASK_CHECK(SDL_FlushAudioStream(p));
        TASK_CHECK(SDL_FlushAudioStream(ref));
        frame += N;

        // Partially drain between rounds, then keep feeding after the flush.
        float tmp[64];
        int g = SDL_GetAudioStreamData(t, tmp, 17 * (int)sizeof(float));
        TASK_CHECK(g >= 0);
        SDL_memcpy(&tap_all[tap_frames], tmp, (size_t)g);
        tap_frames += g / (int)sizeof(float);

        g = SDL_GetAudioStreamData(ref, tmp, 17 * (int)sizeof(float));
        TASK_CHECK(g >= 0);
        SDL_memcpy(&ref_all[ref_frames], tmp, (size_t)g);
        ref_frames += g / (int)sizeof(float);
    }

    int t_rest = drain_f32(t, &tap_all[tap_frames], 1024 - tap_frames, 1);
    int r_rest = drain_f32(ref, &ref_all[ref_frames], 1024 - ref_frames, 1);
    tap_frames += t_rest;
    ref_frames += r_rest;
    TASK_CHECK(tap_frames > 0 && tap_frames == ref_frames);
    TASK_CHECK(buffers_match(ref_all, tap_all, ref_frames));

    SDL_DestroyAudioStream(ref);
    SDL_DestroyAudioStream(t);
    SDL_DestroyAudioStream(p);
    return 0;
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    { "fanout", "basic_three_way", fanout_basic },
    { "fanout", "all_three_put_kinds", fanout_put_kinds },
    { "fanout", "late_join", fanout_late_join },
    { "fanout", "independent_gain", fanout_individual_settings },
    { "fanout", "independent_frequency_ratio", fanout_individual_freq_ratio },
    { "fanout", "input_channel_map", fanout_input_channel_map },
    { "fanout", "slow_tap_clear", fanout_slow_clear },
    { "fanout", "threaded_put_get_overlap", fanout_thread_overlap },
    { "fanout", "destroy_orders", fanout_destroy_orders },
    { "fanout", "producer_callbacks", fanout_producer_callbacks },

    { "ownership", "nocopy_single_callback", ownership_nocopy_lifetime },
    { "ownership", "bounded_memory_growth", ownership_memory_growth },
    { "ownership", "illegal_tap_writes", ownership_illegal_ops },
    { "ownership", "submit_failure_rollback", ownership_submit_failure_rollback },
    { "ownership", "clear_isolation", ownership_clear_isolation },

    { "conversion", "noninteger_rate", conversion_noninteger_rate },
    { "conversion", "input_format_switch", conversion_format_switch },
    { "conversion", "channel_count_switch", conversion_channel_switch },
    { "conversion", "short_flushed_tails", conversion_short_tail },
    { "conversion", "refill_after_flush", conversion_refill_after_flush },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
