/*
 * Regression tests for the SDL_mixer preroll recorder.
 *
 * The recorder is driven through the public API; signals come from real
 * tracks mixed through an explicit group, mixer gain and a postmix callback
 * that rewrites the PCM asymmetrically. Producer/consumer concurrency is
 * coordinated with SDL threads and semaphores, never sleeps.
 */

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include "task_support/check.h"

#include <math.h>

#define TEST_RATE 48000
#define TEST_CHANNELS 2
#define SIGNAL_FRAMES 256
#define BATCH_FRAMES 24
#define EPS 1e-6f

#define TRACK_A_GAIN 0.8f
#define TRACK_B_GAIN 0.6f
#define MIXER_GAIN 0.75f
#define POST_L_SCALE 0.5f
#define POST_R_SCALE 0.25f
#define POST_L_BIAS 0.003f
#define POST_R_BIAS (-0.002f)

static void SDLCALL test_postmix(void *userdata, MIX_Mixer *mixer, const SDL_AudioSpec *spec, float *pcm, int samples)
{
    (void) userdata;
    (void) mixer;
    const int frames = samples / spec->channels;
    for (int i = 0; i < frames; ++i) {
        pcm[i * spec->channels] = pcm[i * spec->channels] * POST_L_SCALE + POST_L_BIAS;
        pcm[i * spec->channels + 1] = pcm[i * spec->channels + 1] * POST_R_SCALE + POST_R_BIAS;
    }
}

// Independent model of what the final output frame `f` must contain.
static void expected_frame(Uint64 f, float *left, float *right)
{
    if (f < SIGNAL_FRAMES) {
        const float la = 0.10f + 0.001f * (float) (f % 17u);
        const float ra = -(0.08f + 0.001f * (float) (f % 13u));
        const float lb = 0.05f + 0.001f * (float) (f % 11u);
        const float rb = 0.07f + 0.001f * (float) (f % 7u);
        const float mix_l = (la * TRACK_A_GAIN + lb * TRACK_B_GAIN) * MIXER_GAIN;
        const float mix_r = (ra * TRACK_A_GAIN + rb * TRACK_B_GAIN) * MIXER_GAIN;
        *left = mix_l * POST_L_SCALE + POST_L_BIAS;
        *right = mix_r * POST_R_SCALE + POST_R_BIAS;
    } else {
        // No active tracks: pre-postmix silence, postmix still leaves a mark.
        *left = POST_L_BIAS;
        *right = POST_R_BIAS;
    }
}

struct test_env
{
    SDL_AudioSpec spec;
    MIX_Mixer *mixer;
    MIX_Group *group;
    MIX_Track *track_a;
    MIX_Track *track_b;
    MIX_Audio *audio_a;
    MIX_Audio *audio_b;
    float *buf_a;
    float *buf_b;
};

static int env_init(struct test_env *e)
{
    SDL_zerop(e);
    e->spec = (SDL_AudioSpec) { SDL_AUDIO_F32, TEST_CHANNELS, TEST_RATE };

    TASK_CHECK(SDL_Init(0));
    TASK_CHECK(MIX_Init());

    e->mixer = MIX_CreateMixer(&e->spec);
    TASK_CHECK(e->mixer != NULL);

    e->buf_a = (float *) SDL_malloc((size_t) SIGNAL_FRAMES * TEST_CHANNELS * sizeof (float));
    e->buf_b = (float *) SDL_malloc((size_t) SIGNAL_FRAMES * TEST_CHANNELS * sizeof (float));
    TASK_CHECK(e->buf_a != NULL);
    TASK_CHECK(e->buf_b != NULL);
    for (int f = 0; f < SIGNAL_FRAMES; ++f) {
        e->buf_a[f * 2] = 0.10f + 0.001f * (float) (f % 17);
        e->buf_a[f * 2 + 1] = -(0.08f + 0.001f * (float) (f % 13));
        e->buf_b[f * 2] = 0.05f + 0.001f * (float) (f % 11);
        e->buf_b[f * 2 + 1] = 0.07f + 0.001f * (float) (f % 7);
    }

    e->audio_a = MIX_LoadRawAudio(e->mixer, e->buf_a, (size_t) SIGNAL_FRAMES * TEST_CHANNELS * sizeof (float), &e->spec);
    e->audio_b = MIX_LoadRawAudio(e->mixer, e->buf_b, (size_t) SIGNAL_FRAMES * TEST_CHANNELS * sizeof (float), &e->spec);
    TASK_CHECK(e->audio_a != NULL);
    TASK_CHECK(e->audio_b != NULL);

    e->track_a = MIX_CreateTrack(e->mixer);
    e->track_b = MIX_CreateTrack(e->mixer);
    TASK_CHECK(e->track_a != NULL);
    TASK_CHECK(e->track_b != NULL);

    e->group = MIX_CreateGroup(e->mixer);
    TASK_CHECK(e->group != NULL);
    TASK_CHECK(MIX_SetTrackGroup(e->track_a, e->group));
    TASK_CHECK(MIX_SetTrackGroup(e->track_b, e->group));

    TASK_CHECK(MIX_SetTrackAudio(e->track_a, e->audio_a));
    TASK_CHECK(MIX_SetTrackAudio(e->track_b, e->audio_b));
    TASK_CHECK(MIX_SetTrackGain(e->track_a, TRACK_A_GAIN));
    TASK_CHECK(MIX_SetTrackGain(e->track_b, TRACK_B_GAIN));
    TASK_CHECK(MIX_SetMixerGain(e->mixer, MIXER_GAIN));
    TASK_CHECK(MIX_SetPostMixCallback(e->mixer, test_postmix, NULL));

    TASK_CHECK(MIX_PlayTrack(e->track_a, 0));
    TASK_CHECK(MIX_PlayTrack(e->track_b, 0));
    return 0;
}

static void env_quit(struct test_env *e)
{
    if (e->mixer) {
        MIX_DestroyMixer(e->mixer);  // also frees the tracks, group and any recorder.
    }
    if (e->audio_a) {
        MIX_DestroyAudio(e->audio_a);
    }
    if (e->audio_b) {
        MIX_DestroyAudio(e->audio_b);
    }
    SDL_free(e->buf_a);
    SDL_free(e->buf_b);
    MIX_Quit();
    SDL_Quit();
}

// Generate `frames` output frames through MIX_Generate, in fixed batches.
static int generate_frames(MIX_Mixer *mixer, Uint64 frames)
{
    float batch[BATCH_FRAMES * TEST_CHANNELS];
    Uint64 done = 0;
    while (done < frames) {
        const int now = (int) SDL_min((Uint64) BATCH_FRAMES, frames - done);
        // Zero is a legal return: the remainder was filled with silence.
        const int bytes = MIX_Generate(mixer, batch, now * TEST_CHANNELS * (int) sizeof (float));
        TASK_CHECK(bytes >= 0);
        done += (Uint64) now;
    }
    return 0;
}

// Verify a recorded run of frames against the independent signal model.
static int check_recorded(const float *pcm, Uint64 first_frame, int count)
{
    for (int i = 0; i < count; ++i) {
        float want_l, want_r;
        expected_frame(first_frame + (Uint64) i, &want_l, &want_r);
        TASK_CHECK(fabsf(pcm[i * 2] - want_l) < EPS);
        TASK_CHECK(fabsf(pcm[i * 2 + 1] - want_r) < EPS);
    }
    return 0;
}

// Drain a stopped recorder fully, returning total frames read and the sum of
// gap sizes (jumps in absolute frame numbers) observed along the way, plus
// the exclusive end of the last real PCM chunk. The gap before the first
// readable chunk, relative to the session's start frame, counts as well.
static int drain_all(MIX_Recorder *recorder, Uint64 *out_frames, Uint64 *out_gaps,
                     Uint64 *out_last_end, float *scratch, int scratch_frames)
{
    Uint64 total = 0;
    Uint64 gaps = 0;
    Uint64 expected_next = 0;
    bool have = false;
    MIX_RecorderState state;

    TASK_CHECK(MIX_GetRecorderState(recorder, &state));
    expected_next = state.start_frame;

    for (;;) {
        Uint64 first = 0;
        const int got = MIX_ReadRecorder(recorder, scratch, scratch_frames, &first);
        TASK_CHECK(got >= 0);
        if (got == 0) {
            break;
        }
        TASK_CHECK(check_recorded(scratch, first, (Uint64) got) == 0);
        TASK_CHECK(first >= expected_next);
        gaps += first - expected_next;
        expected_next = first + (Uint64) got;
        total += (Uint64) got;
        have = true;
    }

    TASK_CHECK(MIX_GetRecorderState(recorder, &state));
    TASK_CHECK(!state.active);
    if (have) {
        // Frames dropped at the tail have no successor PCM and can only be
        // measured against the fixed session end.
        gaps += state.end_frame - expected_next;
        *out_last_end = expected_next;
    } else {
        gaps += state.end_frame - state.start_frame;
        *out_last_end = state.start_frame;
    }
    *out_frames = total;
    *out_gaps = gaps;
    return 0;
}

// ---------------------------------------------------------------------------
// history group
// ---------------------------------------------------------------------------

static int history_preroll_snapshot(void)
{
    struct test_env e;
    TASK_CHECK(env_init(&e) == 0);

    const Uint64 preroll = 16;
    const Uint64 capacity = 64;
    MIX_Recorder *r = MIX_CreateRecorder(e.mixer, preroll, capacity);
    TASK_CHECK(r != NULL);

    TASK_CHECK(generate_frames(e.mixer, 40) == 0);  // more than preroll, less than capacity.

    MIX_RecorderState st;
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(!st.active);
    TASK_CHECK(st.end_frame == 0);
    TASK_CHECK(st.start_frame == 0);
    TASK_CHECK(st.dropped_frames == 0);

    TASK_CHECK(MIX_StartRecorder(r));
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(st.active);
    TASK_CHECK(st.start_frame == 40 - preroll);
    TASK_CHECK(st.end_frame == 40);

    TASK_CHECK(MIX_StopRecorder(r));

    float scratch[64 * TEST_CHANNELS];
    Uint64 frames = 0, gaps = 999, last_end = 0;
    TASK_CHECK(drain_all(r, &frames, &gaps, &last_end, scratch, 7) == 0);
    TASK_CHECK(frames == preroll);
    TASK_CHECK(gaps == 0);
    TASK_CHECK(last_end == 40);

    env_quit(&e);
    return 0;
}

static int history_short_preroll_and_silence(void)
{
    struct test_env e;
    TASK_CHECK(env_init(&e) == 0);

    MIX_Recorder *r = MIX_CreateRecorder(e.mixer, 32, 64);
    TASK_CHECK(r != NULL);

    // Fewer frames than the preroll request: everything existing is offered.
    TASK_CHECK(generate_frames(e.mixer, 10) == 0);
    TASK_CHECK(MIX_StartRecorder(r));
    MIX_RecorderState st;
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(st.start_frame == 0);
    TASK_CHECK(st.end_frame == 10);

    // Keep recording through track end: silence is counted like real audio.
    TASK_CHECK(generate_frames(e.mixer, 30) == 0);  // spans into silence.
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(st.end_frame == 40);
    TASK_CHECK(MIX_StopRecorder(r));

    float scratch[128 * TEST_CHANNELS];
    Uint64 frames = 0, gaps = 1, last_end = 0;
    TASK_CHECK(drain_all(r, &frames, &gaps, &last_end, scratch, 5) == 0);
    TASK_CHECK(frames == 40);
    TASK_CHECK(gaps == 0);
    TASK_CHECK(last_end == 40);

    // Silence frames carry the postmix mark, and are asymmetric.
    float sil[2];
    expected_frame(SIGNAL_FRAMES + 5, &sil[0], &sil[1]);
    TASK_CHECK(sil[0] != sil[1]);
    TASK_CHECK(sil[0] == POST_L_BIAS);
    TASK_CHECK(sil[1] == POST_R_BIAS);

    env_quit(&e);
    return 0;
}

static int history_ring_wraparound(void)
{
    struct test_env e;
    TASK_CHECK(env_init(&e) == 0);

    const Uint64 preroll = 5;
    const Uint64 capacity = 8;  // tiny ring forces many revolutions before Start.
    MIX_Recorder *r = MIX_CreateRecorder(e.mixer, preroll, capacity);
    TASK_CHECK(r != NULL);

    TASK_CHECK(generate_frames(e.mixer, 73) == 0);  // well past several wraps.
    TASK_CHECK(MIX_StartRecorder(r));
    MIX_RecorderState st;
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(st.start_frame == 73 - preroll);  // 68.

    // Session holds exactly capacity frames, so wraps occur on read with no drops.
    TASK_CHECK(generate_frames(e.mixer, 3) == 0);
    TASK_CHECK(MIX_StopRecorder(r));
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(st.dropped_frames == 0);

    float scratch[8 * TEST_CHANNELS];
    Uint64 frames = 0, gaps = 1, last_end = 0;
    TASK_CHECK(drain_all(r, &frames, &gaps, &last_end, scratch, 3) == 0);
    TASK_CHECK(frames == capacity);
    TASK_CHECK(gaps == 0);
    TASK_CHECK(last_end == 76);

    env_quit(&e);
    return 0;
}

static int history_fragmented_reads(void)
{
    struct test_env e;
    TASK_CHECK(env_init(&e) == 0);

    MIX_Recorder *r = MIX_CreateRecorder(e.mixer, 8, 64);
    TASK_CHECK(r != NULL);
    TASK_CHECK(generate_frames(e.mixer, 12) == 0);
    TASK_CHECK(MIX_StartRecorder(r));
    TASK_CHECK(generate_frames(e.mixer, 40) == 0);  // session span 48, fits with room to spare.
    TASK_CHECK(MIX_StopRecorder(r));

    // One-frame reads must form one continuous, exact stream.
    Uint64 expected_first = 4;  // 12 - preroll(8).
    const Uint64 span = 48;
    for (Uint64 i = 0; i < span; ++i) {
        float one[TEST_CHANNELS];
        Uint64 first = 0;
        const int got = MIX_ReadRecorder(r, one, 1, &first);
        TASK_CHECK(got == 1);
        TASK_CHECK(first == expected_first);
        float want_l, want_r;
        expected_frame(first, &want_l, &want_r);
        TASK_CHECK(fabsf(one[0] - want_l) < EPS);
        TASK_CHECK(fabsf(one[1] - want_r) < EPS);
        ++expected_first;
    }
    Uint64 first = 0;
    float tmp[TEST_CHANNELS];
    TASK_CHECK(MIX_ReadRecorder(r, tmp, 1, &first) == 0);
    MIX_RecorderState st;
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(!st.active);

    env_quit(&e);
    return 0;
}

static int history_playback_unchanged_without_recorder(void)
{
    // Two identical rigs: one with a recorder attached, one without. Their
    // MIX_Generate output must be bit-identical and match the model.
    struct test_env with_rec, without_rec;
    TASK_CHECK(env_init(&with_rec) == 0);
    TASK_CHECK(env_init(&without_rec) == 0);

    MIX_Recorder *r = MIX_CreateRecorder(with_rec.mixer, 16, 64);
    TASK_CHECK(r != NULL);
    TASK_CHECK(generate_frames(with_rec.mixer, 20) == 0);
    TASK_CHECK(generate_frames(without_rec.mixer, 20) == 0);  // keep both rigs at the same frame.
    TASK_CHECK(MIX_StartRecorder(r));

    float a[BATCH_FRAMES * TEST_CHANNELS];
    float b[BATCH_FRAMES * TEST_CHANNELS];
    for (int batch = 0; batch < 8; ++batch) {
        const int bytes_a = MIX_Generate(with_rec.mixer, a, (int) sizeof (a));
        const int bytes_b = MIX_Generate(without_rec.mixer, b, (int) sizeof (b));
        TASK_CHECK(bytes_a == bytes_b);
        TASK_CHECK(SDL_memcmp(a, b, sizeof (a)) == 0);
        TASK_CHECK(check_recorded(a, (Uint64) 20 + (Uint64) batch * BATCH_FRAMES, BATCH_FRAMES) == 0);
    }

    env_quit(&with_rec);
    env_quit(&without_rec);
    return 0;
}

// ---------------------------------------------------------------------------
// overrun group
// ---------------------------------------------------------------------------

static int overrun_gap_accounting(void)
{
    struct test_env e;
    TASK_CHECK(env_init(&e) == 0);

    const Uint64 preroll = 4;
    const Uint64 capacity = 12;
    MIX_Recorder *r = MIX_CreateRecorder(e.mixer, preroll, capacity);
    TASK_CHECK(r != NULL);
    TASK_CHECK(generate_frames(e.mixer, 10) == 0);
    TASK_CHECK(MIX_StartRecorder(r));

    MIX_RecorderState st;
    const Uint64 start = 10 - preroll;  // 6.

    // Produce far beyond capacity without reading: oldest data must survive.
    TASK_CHECK(generate_frames(e.mixer, 60) == 0);
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(st.active);
    TASK_CHECK(st.dropped_frames > 0);
    const Uint64 dropped_while_active = st.dropped_frames;

    TASK_CHECK(MIX_StopRecorder(r));
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(!st.active);
    TASK_CHECK(st.end_frame == 70);
    TASK_CHECK(st.dropped_frames == dropped_while_active);

    float scratch[12 * TEST_CHANNELS];
    Uint64 frames = 0, gaps = 0, last_end = 0;
    TASK_CHECK(drain_all(r, &frames, &gaps, &last_end, scratch, 4) == 0);

    // The queue never overwrote unread data: the oldest capacity frames
    // survive, and every other session frame is accounted for as a tail drop.
    TASK_CHECK(frames == capacity);
    TASK_CHECK(frames + gaps == 70 - start);
    TASK_CHECK(gaps == dropped_while_active);
    TASK_CHECK(last_end == start + capacity);
    TASK_CHECK(last_end < 70);

    env_quit(&e);
    return 0;
}

static int overrun_tail_drop_has_no_successor(void)
{
    struct test_env e;
    TASK_CHECK(env_init(&e) == 0);

    MIX_Recorder *r = MIX_CreateRecorder(e.mixer, 2, 6);
    TASK_CHECK(r != NULL);
    TASK_CHECK(generate_frames(e.mixer, 5) == 0);
    TASK_CHECK(MIX_StartRecorder(r));  // start frame 3.

    // Fill the six slots (4 preroll slots were already used), then produce one
    // more frame which must be dropped, and stop immediately: the dropped tail
    // has no successor PCM.
    TASK_CHECK(generate_frames(e.mixer, 5) == 0);
    TASK_CHECK(MIX_StopRecorder(r));

    MIX_RecorderState st;
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(st.dropped_frames == 1);
    TASK_CHECK(st.end_frame == 10);

    float scratch[6 * TEST_CHANNELS];
    Uint64 frames = 0, gaps = 0, last_end = 0;
    TASK_CHECK(drain_all(r, &frames, &gaps, &last_end, scratch, 2) == 0);
    TASK_CHECK(frames == 6);
    TASK_CHECK(gaps == 1);
    TASK_CHECK(last_end == 9);   // last stored frame is absolute frame 8.
    TASK_CHECK(last_end < st.end_frame);

    // Inactive + zero read is the drained signal.
    Uint64 first = 0;
    TASK_CHECK(MIX_ReadRecorder(r, scratch, 2, &first) == 0);
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(!st.active);

    env_quit(&e);
    return 0;
}

static int overrun_restart_resets_counters(void)
{
    struct test_env e;
    TASK_CHECK(env_init(&e) == 0);

    MIX_Recorder *r = MIX_CreateRecorder(e.mixer, 4, 10);
    TASK_CHECK(r != NULL);
    TASK_CHECK(generate_frames(e.mixer, 8) == 0);
    TASK_CHECK(MIX_StartRecorder(r));  // start frame 4.
    TASK_CHECK(generate_frames(e.mixer, 40) == 0);  // force drops.
    TASK_CHECK(MIX_StopRecorder(r));

    MIX_RecorderState st;
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(st.dropped_frames > 0);

    float scratch[10 * TEST_CHANNELS];
    Uint64 frames = 0, gaps = 0, last_end = 0;
    TASK_CHECK(drain_all(r, &frames, &gaps, &last_end, scratch, 3) == 0);
    TASK_CHECK(frames == 10);  // oldest capacity frames survive untouched.
    TASK_CHECK(gaps == st.dropped_frames);
    TASK_CHECK(last_end == 14);
    TASK_CHECK(last_end < 48);

    // After draining, a new session takes fresh preroll and zeroes drops.
    TASK_CHECK(generate_frames(e.mixer, 12) == 0);  // rolling preroll kept updating while stopped.
    TASK_CHECK(MIX_StartRecorder(r));
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(st.active);
    TASK_CHECK(st.dropped_frames == 0);
    TASK_CHECK(st.start_frame == 56);  // tail 60 minus preroll 4.
    TASK_CHECK(st.end_frame == 60);    // the preroll frames already exist.

    TASK_CHECK(MIX_StopRecorder(r));
    Uint64 frames2 = 0, gaps2 = 0, last2 = 0;
    TASK_CHECK(drain_all(r, &frames2, &gaps2, &last2, scratch, 5) == 0);
    TASK_CHECK(gaps2 == 0);
    TASK_CHECK(frames2 == 4);  // exactly the fresh preroll.
    TASK_CHECK(last2 == 60);

    env_quit(&e);
    return 0;
}

struct producer_ctx
{
    MIX_Mixer *mixer;
    SDL_Semaphore *go;
    SDL_Semaphore *done;
    int batches;
};

static int SDLCALL producer_thread(void *arg)
{
    struct producer_ctx *ctx = (struct producer_ctx *) arg;
    float batch[BATCH_FRAMES * TEST_CHANNELS];
    for (int i = 0; i < ctx->batches; ++i) {
        SDL_WaitSemaphore(ctx->go);
        // A zero return is legitimate: the requested bytes were filled with
        // silence because no track had real audio left. Only -1 is failure.
        const int bytes = MIX_Generate(ctx->mixer, batch, (int) sizeof (batch));
        if (bytes < 0) {
            return 1;
        }
        SDL_SignalSemaphore(ctx->done);
    }
    return 0;
}

static int overrun_concurrent_producer_consumer(void)
{
    struct test_env e;
    TASK_CHECK(env_init(&e) == 0);

    const Uint64 preroll = 4;
    const Uint64 capacity = 16;
    MIX_Recorder *r = MIX_CreateRecorder(e.mixer, preroll, capacity);
    TASK_CHECK(r != NULL);

    SDL_Semaphore *go = SDL_CreateSemaphore(0);
    SDL_Semaphore *done = SDL_CreateSemaphore(0);
    TASK_CHECK(go != NULL);
    TASK_CHECK(done != NULL);

    // Phase 1: preroll accumulates through many ring revolutions before Start.
    struct producer_ctx ctx = { e.mixer, go, done, 30 };
    SDL_Thread *t = SDL_CreateThread(producer_thread, "producer", &ctx);
    TASK_CHECK(t != NULL);
    for (int i = 0; i < ctx.batches; ++i) {
        SDL_SignalSemaphore(go);
        SDL_WaitSemaphore(done);
    }
    int thread_rc = 0;
    SDL_WaitThread(t, &thread_rc);
    TASK_CHECK(thread_rc == 0);

    TASK_CHECK(MIX_StartRecorder(r));
    MIX_RecorderState st;
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(st.start_frame == (Uint64) ctx.batches * BATCH_FRAMES - preroll);

    // Phase 2: the consumer reads on its own cadence and regularly lets the
    // queue fill. Frames already handed out must never be overwritten.
    ctx.batches = 40;
    t = SDL_CreateThread(producer_thread, "producer", &ctx);
    TASK_CHECK(t != NULL);

    Uint64 consumed = 0;
    Uint64 seen_gaps = 0;
    Uint64 expected_next = st.start_frame;
    for (int i = 0; i < ctx.batches; ++i) {
        SDL_SignalSemaphore(go);
        SDL_WaitSemaphore(done);

        if ((i % 3) != 0) {
            continue;  // deliberately stall to force overflow.
        }
        float scratch[8 * TEST_CHANNELS];
        for (;;) {
            Uint64 first = 0;
            const int got = MIX_ReadRecorder(r, scratch, 8, &first);
            TASK_CHECK(got >= 0);
            if (got == 0) {
                break;
            }
            TASK_CHECK(check_recorded(scratch, first, (Uint64) got) == 0);
            TASK_CHECK(first >= expected_next);
            seen_gaps += first - expected_next;  // includes the initial gap.
            expected_next = first + (Uint64) got;
            consumed += (Uint64) got;
        }
    }
    SDL_WaitThread(t, &thread_rc);
    TASK_CHECK(thread_rc == 0);

    TASK_CHECK(MIX_StopRecorder(r));
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(st.dropped_frames > 0);  // the stalls really did overflow the queue.

    // Drain the remainder; all surviving content must still match the model.
    float scratch[16 * TEST_CHANNELS];
    for (;;) {
        Uint64 first = 0;
        const int got = MIX_ReadRecorder(r, scratch, 5, &first);
        TASK_CHECK(got >= 0);
        if (got == 0) {
            break;
        }
        TASK_CHECK(check_recorded(scratch, first, (Uint64) got) == 0);
        TASK_CHECK(first >= expected_next);
        seen_gaps += first - expected_next;
        expected_next = first + (Uint64) got;
        consumed += (Uint64) got;
    }

    // A final drop at the tail has no successor PCM; measure it from the end.
    seen_gaps += st.end_frame - expected_next;

    TASK_CHECK(consumed > 0);
    TASK_CHECK(consumed + seen_gaps == st.end_frame - st.start_frame);
    TASK_CHECK(seen_gaps == st.dropped_frames);

    SDL_DestroySemaphore(go);
    SDL_DestroySemaphore(done);
    env_quit(&e);
    return 0;
}

// ---------------------------------------------------------------------------
// lifecycle group
// ---------------------------------------------------------------------------

static int lifecycle_invalid_creation(void)
{
    struct test_env e;
    TASK_CHECK(env_init(&e) == 0);

    TASK_CHECK(MIX_CreateRecorder(NULL, 4, 8) == NULL);
    TASK_CHECK(MIX_CreateRecorder(e.mixer, 0, 0) == NULL);
    TASK_CHECK(MIX_CreateRecorder(e.mixer, 8, 8) == NULL);   // preroll must be smaller.
    TASK_CHECK(MIX_CreateRecorder(e.mixer, 9, 8) == NULL);
    TASK_CHECK(MIX_CreateRecorder(e.mixer, SDL_MAX_UINT64, SDL_MAX_UINT64) == NULL);  // overflow guard.

    // A failed attachment leaves sounds and output intact.
    float out[BATCH_FRAMES * TEST_CHANNELS];
    const int bytes = MIX_Generate(e.mixer, out, (int) sizeof (out));
    TASK_CHECK(bytes > 0);
    float want_l, want_r;
    expected_frame(0, &want_l, &want_r);
    TASK_CHECK(fabsf(out[0] - want_l) < EPS);
    TASK_CHECK(fabsf(out[1] - want_r) < EPS);

    env_quit(&e);
    return 0;
}

static int lifecycle_single_recorder_per_mixer(void)
{
    struct test_env e;
    TASK_CHECK(env_init(&e) == 0);

    MIX_Recorder *first = MIX_CreateRecorder(e.mixer, 4, 16);
    TASK_CHECK(first != NULL);
    TASK_CHECK(MIX_CreateRecorder(e.mixer, 4, 16) == NULL);

    // The first recorder still works.
    TASK_CHECK(generate_frames(e.mixer, 8) == 0);
    TASK_CHECK(MIX_StartRecorder(first));
    TASK_CHECK(MIX_StopRecorder(first));
    MIX_RecorderState st;
    TASK_CHECK(MIX_GetRecorderState(first, &st));
    TASK_CHECK(st.start_frame == 4);

    MIX_DestroyRecorder(first);
    MIX_Recorder *second = MIX_CreateRecorder(e.mixer, 4, 16);
    TASK_CHECK(second != NULL);  // slot released.
    MIX_DestroyRecorder(second);
    MIX_DestroyRecorder(NULL);    // legal no-op.

    env_quit(&e);
    return 0;
}

static int lifecycle_start_and_stop_rules(void)
{
    struct test_env e;
    TASK_CHECK(env_init(&e) == 0);

    MIX_Recorder *r = MIX_CreateRecorder(e.mixer, 4, 16);
    TASK_CHECK(r != NULL);

    // Stop before Start is fine and changes nothing.
    TASK_CHECK(MIX_StopRecorder(r));
    MIX_RecorderState st;
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(!st.active);
    TASK_CHECK(st.start_frame == 0);
    TASK_CHECK(st.end_frame == 0);

    TASK_CHECK(generate_frames(e.mixer, 8) == 0);
    TASK_CHECK(MIX_StartRecorder(r));
    TASK_CHECK(!MIX_StartRecorder(r));  // already active: fails, history kept.

    float scratch[16 * TEST_CHANNELS];
    Uint64 first = 0;
    TASK_CHECK(MIX_ReadRecorder(r, scratch, 3, &first) == 3);
    TASK_CHECK(first == 4);
    TASK_CHECK(check_recorded(scratch, first, 3) == 0);

    TASK_CHECK(MIX_StopRecorder(r));
    TASK_CHECK(MIX_StopRecorder(r));  // repeated stop is fine and freezes state.
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(!st.active);
    const Uint64 frozen_end = st.end_frame;
    TASK_CHECK(frozen_end == 8);

    // Restart while the stopped queue still holds data must fail.
    TASK_CHECK(!MIX_StartRecorder(r));
    TASK_CHECK(generate_frames(e.mixer, 5) == 0);
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(!st.active);
    TASK_CHECK(st.end_frame == frozen_end);  // state untouched.

    // Only frame 7 remains in the session; failed Start did not consume it.
    Uint64 first2 = 0;
    TASK_CHECK(MIX_ReadRecorder(r, scratch, 3, &first2) == 1);
    TASK_CHECK(first2 == 7);
    TASK_CHECK(check_recorded(scratch, first2, 1) == 0);

    // The session is now fully drained: inactive plus a zero read.
    Uint64 first3 = 0;
    TASK_CHECK(MIX_ReadRecorder(r, scratch, 3, &first3) == 0);
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(!st.active);

    // Now restart works and resets the dropped-frame statistics.
    TASK_CHECK(MIX_StartRecorder(r));
    TASK_CHECK(MIX_GetRecorderState(r, &st));
    TASK_CHECK(st.active);
    TASK_CHECK(st.dropped_frames == 0);
    TASK_CHECK(MIX_StopRecorder(r));

    env_quit(&e);
    return 0;
}

static int lifecycle_read_validation(void)
{
    struct test_env e;
    TASK_CHECK(env_init(&e) == 0);

    MIX_Recorder *r = MIX_CreateRecorder(e.mixer, 4, 16);
    TASK_CHECK(r != NULL);
    TASK_CHECK(generate_frames(e.mixer, 10) == 0);
    TASK_CHECK(MIX_StartRecorder(r));
    TASK_CHECK(MIX_StopRecorder(r));

    float scratch[16 * TEST_CHANNELS];
    Uint64 first = 0;
    MIX_RecorderState st;

    // Bad arguments fail and consume nothing.
    TASK_CHECK(MIX_ReadRecorder(NULL, scratch, 4, &first) == -1);
    TASK_CHECK(MIX_ReadRecorder(r, NULL, 4, &first) == -1);
    TASK_CHECK(MIX_ReadRecorder(r, scratch, 4, NULL) == -1);
    TASK_CHECK(MIX_ReadRecorder(r, scratch, 0, &first) == -1);
    TASK_CHECK(MIX_ReadRecorder(r, scratch, -1, &first) == -1);
    TASK_CHECK(!MIX_GetRecorderState(NULL, &st));
    TASK_CHECK(!MIX_GetRecorderState(r, NULL));
    TASK_CHECK(!MIX_StartRecorder(NULL));
    TASK_CHECK(!MIX_StopRecorder(NULL));

    // The failed calls left the queue head untouched.
    Uint64 good_first = 999;
    TASK_CHECK(MIX_ReadRecorder(r, scratch, 2, &good_first) == 2);
    TASK_CHECK(good_first == 6);  // 10 - preroll(4).

    env_quit(&e);
    return 0;
}

static int lifecycle_destroy_with_mixer(void)
{
    struct test_env e;
    TASK_CHECK(env_init(&e) == 0);

    MIX_Recorder *r = MIX_CreateRecorder(e.mixer, 4, 16);
    TASK_CHECK(r != NULL);
    TASK_CHECK(generate_frames(e.mixer, 6) == 0);
    TASK_CHECK(MIX_StartRecorder(r));
    TASK_CHECK(generate_frames(e.mixer, 6) == 0);

    // Destroying the mixer releases the attached recorder; no explicit destroy.
    MIX_DestroyMixer(e.mixer);
    e.mixer = NULL;  // env_quit must not double-free it.
    MIX_DestroyAudio(e.audio_a);
    e.audio_a = NULL;
    MIX_DestroyAudio(e.audio_b);
    e.audio_b = NULL;
    SDL_free(e.buf_a);
    e.buf_a = NULL;
    SDL_free(e.buf_b);
    e.buf_b = NULL;
    MIX_Quit();
    SDL_Quit();
    return 0;
}

const struct task_case task_cases[] = {
    { "history", "preroll_snapshot", history_preroll_snapshot },
    { "history", "short_preroll_and_silence", history_short_preroll_and_silence },
    { "history", "ring_wraparound", history_ring_wraparound },
    { "history", "fragmented_reads", history_fragmented_reads },
    { "history", "playback_unchanged_without_recorder", history_playback_unchanged_without_recorder },

    { "overrun", "gap_accounting", overrun_gap_accounting },
    { "overrun", "tail_drop_has_no_successor", overrun_tail_drop_has_no_successor },
    { "overrun", "restart_resets_counters", overrun_restart_resets_counters },
    { "overrun", "concurrent_producer_consumer", overrun_concurrent_producer_consumer },

    { "lifecycle", "invalid_creation", lifecycle_invalid_creation },
    { "lifecycle", "single_recorder_per_mixer", lifecycle_single_recorder_per_mixer },
    { "lifecycle", "start_and_stop_rules", lifecycle_start_and_stop_rules },
    { "lifecycle", "read_validation", lifecycle_read_validation },
    { "lifecycle", "destroy_with_mixer", lifecycle_destroy_with_mixer },
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
