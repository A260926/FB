/*
  Task regression tests for the MIX_Recorder preroll recording interface.

  Signal flow under test: real looping tracks -> track gains -> a group with
  a gain-style postmix -> mixer gain -> a mixer postmix that rewrites the PCM
  -> recorder tap. The signal is asymmetric (left != right) and depends on
  the frame position, so misaligned frames are detectable. A second,
  recorder-less reference mixer generates the canonical PCM model; every
  frame read from the recorder is checked against it bit-exactly, and the
  recorder-equipped mixer's own playback is checked against the reference
  playback on every generate.
*/

#include "task_support/check.h"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <stdlib.h>
#include <string.h>

#define TEST_FREQ 48000
#define TEST_CH 2
#define TEST_FRAMESIZE (TEST_CH * (int) sizeof (float))
#define SIGNAL_FRAMES 4096    /* source audio length; tracks loop it forever */
#define MODEL_FRAMES 8192     /* how many frames of reference PCM we keep */
#define SCRATCH_FRAMES 4096   /* generate/read scratch buffer size */

typedef struct Fixture
{
    MIX_Mixer *mix;       /* has the recorder attached */
    MIX_Mixer *ref;       /* no recorder: the playback reference */
    MIX_Audio *audio_mix;
    MIX_Audio *audio_ref;
    MIX_Track *ta_mix;
    MIX_Track *tb_mix;
    MIX_Track *ta_ref;
    MIX_Track *tb_ref;
    MIX_Recorder *rec;
    float *signal;        /* SIGNAL_FRAMES frames of asymmetric stereo */
    float *ref_pcm;       /* every frame generated since recorder creation */
    Uint64 ref_frames;
    float *gen_a;
    float *gen_b;
    float *read_buf;
} Fixture;

/* a "group gain": scales the whole group's mixed output */
static void SDLCALL group_gain_cb(void *userdata, MIX_Group *group, const SDL_AudioSpec *spec, float *pcm, int samples)
{
    (void) userdata; (void) group; (void) spec;
    for (int i = 0; i < samples; i++) {
        pcm[i] *= 0.75f;
    }
}

/* a mixer postmix that really changes the PCM, so recorded data is post-callback data */
static void SDLCALL postmix_cb(void *userdata, MIX_Mixer *mixer, const SDL_AudioSpec *spec, float *pcm, int samples)
{
    (void) userdata; (void) mixer; (void) spec;
    for (int i = 0; i < samples; i++) {
        pcm[i] *= 0.5f;
    }
}

/* asymmetric, position-dependent, non-zero */
static void fill_signal(float *pcm, int frames)
{
    for (int i = 0; i < frames; i++) {
        pcm[2 * i] = 0.40f * (float) (i % 997) / 997.0f - 0.20f;
        pcm[2 * i + 1] = 0.30f * (float) ((i * 31) % 641) / 641.0f - 0.15f;
    }
}

static int play_pair(MIX_Track *ta, MIX_Track *tb)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) {
        return 1;
    }
    SDL_SetNumberProperty(props, MIX_PROP_PLAY_LOOPS_NUMBER, -1);  /* loop forever */
    const bool ok = MIX_PlayTrack(ta, props) && MIX_PlayTrack(tb, props);
    SDL_DestroyProperties(props);
    return ok ? 0 : 1;
}

static int build_mixer(Fixture *f, MIX_Mixer **mixer_out, MIX_Audio **audio_out, MIX_Track **ta_out, MIX_Track **tb_out, bool play)
{
    const SDL_AudioSpec spec = { SDL_AUDIO_F32, TEST_CH, TEST_FREQ };
    MIX_Mixer *mixer = MIX_CreateMixer(&spec);
    TASK_CHECK(mixer != NULL);
    MIX_Audio *audio = MIX_LoadRawAudio(mixer, f->signal, SIGNAL_FRAMES * TEST_FRAMESIZE, &spec);
    TASK_CHECK(audio != NULL);
    MIX_Group *group = MIX_CreateGroup(mixer);
    TASK_CHECK(group != NULL);
    MIX_Track *ta = MIX_CreateTrack(mixer);
    MIX_Track *tb = MIX_CreateTrack(mixer);
    TASK_CHECK(ta != NULL && tb != NULL);
    TASK_CHECK(MIX_SetTrackAudio(ta, audio));
    TASK_CHECK(MIX_SetTrackAudio(tb, audio));
    TASK_CHECK(MIX_SetTrackGroup(ta, group));
    TASK_CHECK(MIX_SetTrackGroup(tb, group));
    TASK_CHECK(MIX_SetTrackGain(tb, 0.5f));
    TASK_CHECK(MIX_SetMixerGain(mixer, 0.8f));
    TASK_CHECK(MIX_SetGroupPostMixCallback(group, group_gain_cb, NULL));
    TASK_CHECK(MIX_SetPostMixCallback(mixer, postmix_cb, NULL));
    if (play) {
        TASK_CHECK(play_pair(ta, tb) == 0);
    }
    *mixer_out = mixer;
    *audio_out = audio;
    *ta_out = ta;
    *tb_out = tb;
    return 0;
}

static int fixture_setup(Fixture *f, int preroll, int capacity, bool play)
{
    SDL_memset(f, 0, sizeof (*f));
    TASK_CHECK(SDL_Init(0));
    TASK_CHECK(MIX_Init());
    f->signal = (float *) SDL_malloc(SIGNAL_FRAMES * TEST_FRAMESIZE);
    f->ref_pcm = (float *) SDL_malloc(MODEL_FRAMES * TEST_FRAMESIZE);
    f->gen_a = (float *) SDL_malloc(SCRATCH_FRAMES * TEST_FRAMESIZE);
    f->gen_b = (float *) SDL_malloc(SCRATCH_FRAMES * TEST_FRAMESIZE);
    f->read_buf = (float *) SDL_malloc(SCRATCH_FRAMES * TEST_FRAMESIZE);
    TASK_CHECK(f->signal && f->ref_pcm && f->gen_a && f->gen_b && f->read_buf);
    fill_signal(f->signal, SIGNAL_FRAMES);
    TASK_CHECK(build_mixer(f, &f->mix, &f->audio_mix, &f->ta_mix, &f->tb_mix, play) == 0);
    TASK_CHECK(build_mixer(f, &f->ref, &f->audio_ref, &f->ta_ref, &f->tb_ref, play) == 0);
    f->rec = MIX_CreateRecorder(f->mix, preroll, capacity);
    TASK_CHECK(f->rec != NULL);
    return 0;
}

static void fixture_teardown(Fixture *f)
{
    if (f->rec) {
        MIX_DestroyRecorder(f->rec);
    }
    if (f->mix) {
        MIX_DestroyMixer(f->mix);
    }
    if (f->ref) {
        MIX_DestroyMixer(f->ref);
    }
    if (f->audio_mix) {
        MIX_DestroyAudio(f->audio_mix);
    }
    if (f->audio_ref) {
        MIX_DestroyAudio(f->audio_ref);
    }
    SDL_free(f->signal);
    SDL_free(f->ref_pcm);
    SDL_free(f->gen_a);
    SDL_free(f->gen_b);
    SDL_free(f->read_buf);
    MIX_Quit();
    SDL_Quit();
}

/* generate `frames` frames on both mixers; playback must be bit-identical
   with and without the recorder, and the reference output becomes the
   canonical model for the recorder's absolute frame numbers. */
static int generate(Fixture *f, int frames)
{
    TASK_CHECK(frames > 0 && frames <= SCRATCH_FRAMES);
    TASK_CHECK(f->ref_frames + (Uint64) frames <= MODEL_FRAMES);
    const int br = MIX_Generate(f->mix, f->gen_a, frames * TEST_FRAMESIZE);
    const int rr = MIX_Generate(f->ref, f->gen_b, frames * TEST_FRAMESIZE);
    TASK_CHECK(br >= 0);
    TASK_CHECK(br == rr);  /* MIX_Generate's return meaning is unchanged by the recorder */
    TASK_CHECK(SDL_memcmp(f->gen_a, f->gen_b, (size_t) frames * TEST_FRAMESIZE) == 0);
    SDL_memcpy(f->ref_pcm + f->ref_frames * TEST_CH, f->gen_b, (size_t) frames * TEST_FRAMESIZE);
    f->ref_frames += (Uint64) frames;
    return 0;
}

/* reference-only generate, for when the recorded mixer is driven by another thread */
static int generate_ref(Fixture *f, int frames)
{
    TASK_CHECK(frames > 0 && frames <= SCRATCH_FRAMES);
    TASK_CHECK(f->ref_frames + (Uint64) frames <= MODEL_FRAMES);
    TASK_CHECK(MIX_Generate(f->ref, f->gen_b, frames * TEST_FRAMESIZE) >= 0);
    SDL_memcpy(f->ref_pcm + f->ref_frames * TEST_CH, f->gen_b, (size_t) frames * TEST_FRAMESIZE);
    f->ref_frames += (Uint64) frames;
    return 0;
}

static int read_expect(Fixture *f, int max_frames, Uint64 expect_first, int expect_n)
{
    Uint64 first = 0;
    const int n = MIX_ReadRecorder(f->rec, f->read_buf, max_frames, &first);
    TASK_CHECK(n == expect_n);
    if (n > 0) {
        TASK_CHECK(first == expect_first);
        TASK_CHECK(expect_first + (Uint64) n <= f->ref_frames);
        TASK_CHECK(SDL_memcmp(f->read_buf, f->ref_pcm + first * TEST_CH, (size_t) n * TEST_FRAMESIZE) == 0);
    }
    return 0;
}

static int check_state(Fixture *f, int active, Uint64 start, Uint64 end, Uint64 dropped)
{
    MIX_RecorderState st;
    TASK_CHECK(MIX_GetRecorderState(f->rec, &st));
    TASK_CHECK(!!st.active == !!active);
    TASK_CHECK(st.start_frame == start);
    TASK_CHECK(st.end_frame == end);
    TASK_CHECK(st.dropped_frames == dropped);
    return 0;
}

/* --- history group: preroll, silence, ring wraparound, restart --- */

static int case_preroll_and_continuation(void)
{
    Fixture f;
    if (fixture_setup(&f, 100, 500, true)) {
        return 1;
    }
    if (generate(&f, 250)) {
        return 1;
    }
    if (check_state(&f, 0, 0, 0, 0)) {  /* never started */
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));
    if (check_state(&f, 1, 150, 250, 0)) {  /* preroll included in start_frame */
        return 1;
    }
    if (read_expect(&f, 1000, 150, 100)) {  /* exactly the preroll is readable */
        return 1;
    }
    if (generate(&f, 60) || read_expect(&f, 1000, 250, 60)) {  /* new mixing continues seamlessly */
        return 1;
    }
    if (generate(&f, 1) || read_expect(&f, 1, 310, 1)) {
        return 1;
    }
    TASK_CHECK(MIX_StopRecorder(f.rec));
    if (check_state(&f, 0, 150, 311, 0)) {
        return 1;
    }
    if (read_expect(&f, 16, 0, 0)) {  /* drained */
        return 1;
    }
    fixture_teardown(&f);
    return 0;
}

static int case_silence_counts(void)
{
    Fixture f;
    if (fixture_setup(&f, 80, 200, false)) {  /* tracks exist but are not playing */
        return 1;
    }
    if (generate(&f, 120)) {  /* pure silence; still counted and prerolled */
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));
    if (check_state(&f, 1, 40, 120, 0)) {  /* silence counted in the frame numbers */
        return 1;
    }
    if (read_expect(&f, 1000, 40, 80)) {  /* preroll captured the silence */
        return 1;
    }
    TASK_CHECK(play_pair(f.ta_mix, f.tb_mix) == 0);
    TASK_CHECK(play_pair(f.ta_ref, f.tb_ref) == 0);
    if (generate(&f, 50) || read_expect(&f, 1000, 120, 50)) {  /* real signal follows the silence */
        return 1;
    }
    TASK_CHECK(MIX_StopRecorder(f.rec));
    if (check_state(&f, 0, 40, 170, 0)) {
        return 1;
    }
    fixture_teardown(&f);
    return 0;
}

static int case_ring_wraparound(void)
{
    Fixture f;
    if (fixture_setup(&f, 64, 128, true)) {
        return 1;
    }
    if (generate(&f, 200)) {  /* preroll ring has wrapped several times */
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));
    if (check_state(&f, 1, 136, 200, 0)) {
        return 1;
    }
    if (read_expect(&f, 64, 136, 64)) {  /* preroll content correct across the ring wrap */
        return 1;
    }
    Uint64 expected = 200;
    for (int i = 0; i < 10; i++) {  /* 400 more frames through a 128-frame queue: queue ring wraps */
        if (generate(&f, 40) || read_expect(&f, 64, expected, 40)) {
            return 1;
        }
        expected += 40;
    }
    if (generate(&f, 75) || read_expect(&f, 25, 600, 25)) {  /* uneven produce/consume */
        return 1;
    }
    if (generate(&f, 75) || read_expect(&f, 200, 625, 125)) {
        return 1;
    }
    TASK_CHECK(MIX_StopRecorder(f.rec));
    fixture_teardown(&f);
    return 0;
}

static int case_restart_recording(void)
{
    Fixture f;
    if (fixture_setup(&f, 50, 100, true)) {
        return 1;
    }
    if (generate(&f, 80)) {
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));
    if (generate(&f, 40)) {
        return 1;
    }
    TASK_CHECK(!MIX_StartRecorder(f.rec));  /* already recording: fails */
    if (check_state(&f, 1, 30, 120, 0)) {  /* and changes nothing */
        return 1;
    }
    TASK_CHECK(MIX_StopRecorder(f.rec));
    if (generate(&f, 30)) {  /* preroll keeps rolling while stopped */
        return 1;
    }
    TASK_CHECK(!MIX_StartRecorder(f.rec));  /* previous queue not drained: fails */
    if (check_state(&f, 0, 30, 120, 0)) {  /* history not cleared */
        return 1;
    }
    if (read_expect(&f, 1000, 30, 90)) {  /* previous data still readable */
        return 1;
    }
    if (read_expect(&f, 16, 0, 0)) {  /* now drained */
        return 1;
    }
    if (generate(&f, 20)) {
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));  /* drained: may start again */
    if (check_state(&f, 1, 120, 170, 0)) {  /* fresh preroll, drop stats reset */
        return 1;
    }
    if (read_expect(&f, 1000, 120, 50)) {  /* preroll captured while stopped */
        return 1;
    }
    if (generate(&f, 10) || read_expect(&f, 1000, 170, 10)) {
        return 1;
    }
    TASK_CHECK(MIX_StopRecorder(f.rec));
    fixture_teardown(&f);
    return 0;
}

/* --- overrun group: capacity, drops, gaps, fragmented reads, concurrency --- */

static int case_queue_full_drops(void)
{
    Fixture f;
    if (fixture_setup(&f, 100, 500, true)) {
        return 1;
    }
    if (generate(&f, 300)) {
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));
    if (generate(&f, 500)) {  /* 400 fit, 100 are dropped */
        return 1;
    }
    if (check_state(&f, 1, 200, 800, 100)) {
        return 1;
    }
    if (read_expect(&f, 1000, 200, 500)) {  /* one contiguous run: preroll + 400 */
        return 1;
    }
    if (read_expect(&f, 16, 0, 0)) {  /* momentarily empty, still recording */
        return 1;
    }
    if (generate(&f, 30) || read_expect(&f, 1000, 800, 30)) {  /* resumes after the gap */
        return 1;
    }
    if (check_state(&f, 1, 200, 830, 100)) {
        return 1;
    }
    TASK_CHECK(MIX_StopRecorder(f.rec));
    fixture_teardown(&f);
    return 0;
}

static int case_fragmented_reads(void)
{
    Fixture f;
    if (fixture_setup(&f, 64, 256, true)) {
        return 1;
    }
    if (generate(&f, 100)) {
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));
    if (generate(&f, 192)) {  /* queue exactly full, no drops */
        return 1;
    }
    if (check_state(&f, 1, 36, 292, 0)) {
        return 1;
    }
    for (int i = 0; i < 8; i++) {  /* drain in 32-frame fragments */
        if (read_expect(&f, 32, 36 + (Uint64) (32 * i), 32)) {
            return 1;
        }
    }
    if (read_expect(&f, 32, 0, 0)) {
        return 1;
    }
    if (generate(&f, 300)) {  /* 256 stored, 44 dropped */
        return 1;
    }
    if (read_expect(&f, 100, 292, 100) || read_expect(&f, 100, 392, 100) || read_expect(&f, 100, 492, 56)) {
        return 1;
    }
    if (read_expect(&f, 100, 0, 0)) {  /* nothing left but the gap */
        return 1;
    }
    if (generate(&f, 10) || read_expect(&f, 100, 592, 10)) {  /* first frame after the gap */
        return 1;
    }
    if (check_state(&f, 1, 36, 602, 44)) {
        return 1;
    }
    TASK_CHECK(MIX_StopRecorder(f.rec));
    fixture_teardown(&f);
    return 0;
}

static int case_tail_drop_and_drain(void)
{
    Fixture f;
    if (fixture_setup(&f, 10, 64, true)) {
        return 1;
    }
    if (generate(&f, 20)) {
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));
    if (generate(&f, 200)) {  /* 54 stored, 146 dropped */
        return 1;
    }
    TASK_CHECK(MIX_StopRecorder(f.rec));  /* drops at the very tail: no successor PCM will ever exist */
    if (check_state(&f, 0, 10, 220, 146)) {
        return 1;
    }
    if (read_expect(&f, 1000, 10, 64)) {  /* everything that was stored */
        return 1;
    }
    if (read_expect(&f, 1000, 0, 0)) {  /* 0 + inactive == drained; tail drops only visible in state */
        return 1;
    }
    if (check_state(&f, 0, 10, 220, 146)) {
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));  /* drained: a new recording may start */
    if (check_state(&f, 1, 210, 220, 0)) {
        return 1;
    }
    if (read_expect(&f, 1000, 210, 10)) {
        return 1;
    }
    TASK_CHECK(MIX_StopRecorder(f.rec));
    fixture_teardown(&f);
    return 0;
}

typedef struct Producer
{
    MIX_Mixer *mixer;
    SDL_Semaphore *go;
    SDL_Semaphore *done;
    float *buf;
    int frames;   /* frames per generate; 0 tells the thread to exit */
    int error;
} Producer;

static int SDLCALL producer_main(void *userdata)
{
    Producer *p = (Producer *) userdata;
    for (;;) {
        SDL_WaitSemaphore(p->go);
        const int frames = p->frames;
        if (frames <= 0) {
            break;
        }
        if (MIX_Generate(p->mixer, p->buf, frames * TEST_FRAMESIZE) < 0) {
            p->error = 1;
        }
        SDL_SignalSemaphore(p->done);
    }
    return 0;
}

static int case_concurrent_no_overwrite(void)
{
    Fixture f;
    if (fixture_setup(&f, 32, 128, true)) {
        return 1;
    }

    Producer p;
    SDL_memset(&p, 0, sizeof (p));
    p.mixer = f.mix;
    p.go = SDL_CreateSemaphore(0);
    p.done = SDL_CreateSemaphore(0);
    p.buf = (float *) SDL_malloc(SCRATCH_FRAMES * TEST_FRAMESIZE);
    TASK_CHECK(p.go != NULL && p.done != NULL && p.buf != NULL);
    SDL_Thread *thread = SDL_CreateThread(producer_main, "recproducer", &p);
    TASK_CHECK(thread != NULL);

    /* deterministic phase: fill the queue exactly, then overflow it */
    p.frames = 64;
    SDL_SignalSemaphore(p.go);
    SDL_WaitSemaphore(p.done);
    if (generate_ref(&f, 64)) {
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));
    p.frames = 160;
    SDL_SignalSemaphore(p.go);
    SDL_WaitSemaphore(p.done);
    if (generate_ref(&f, 160)) {
        return 1;
    }
    if (check_state(&f, 1, 32, 224, 64)) {  /* 32 preroll + 96 stored, 64 dropped */
        return 1;
    }
    if (read_expect(&f, 1000, 32, 128)) {  /* unread frames survived the overflow */
        return 1;
    }

    /* concurrent phase: the producer free-runs while this thread reads.
       Model all of its frames on the reference mixer first (the reference is
       deterministic, so this is the same PCM no matter when it is generated);
       the producer may run arbitrarily far ahead of the reads below. */
    if (generate_ref(&f, 260)) {
        return 1;
    }
    p.frames = 13;
    for (int i = 0; i < 20; i++) {
        SDL_SignalSemaphore(p.go);
    }
    Uint64 expected_next = 160;
    Uint64 total_read = 128;
    for (int i = 0; i < 20; i++) {
        SDL_WaitSemaphore(p.done);
        Uint64 first = 0;
        const int n = MIX_ReadRecorder(f.rec, f.read_buf, SCRATCH_FRAMES, &first);
        TASK_CHECK(n >= 0);
        if (n > 0) {
            TASK_CHECK(first >= expected_next);  /* gaps allowed, reordering/overwrite is not */
            TASK_CHECK(first + (Uint64) n <= f.ref_frames);
            TASK_CHECK(SDL_memcmp(f.read_buf, f.ref_pcm + first * TEST_CH, (size_t) n * TEST_FRAMESIZE) == 0);
            expected_next = first + (Uint64) n;
            total_read += (Uint64) n;
        }
    }

    TASK_CHECK(MIX_StopRecorder(f.rec));
    for (;;) {  /* drain whatever is left */
        Uint64 first = 0;
        const int n = MIX_ReadRecorder(f.rec, f.read_buf, SCRATCH_FRAMES, &first);
        TASK_CHECK(n >= 0);
        if (n == 0) {
            break;
        }
        TASK_CHECK(first >= expected_next);
        TASK_CHECK(SDL_memcmp(f.read_buf, f.ref_pcm + first * TEST_CH, (size_t) n * TEST_FRAMESIZE) == 0);
        expected_next = first + (Uint64) n;
        total_read += (Uint64) n;
    }

    /* every frame in the recording window was either read or counted as dropped */
    MIX_RecorderState st;
    TASK_CHECK(MIX_GetRecorderState(f.rec, &st));
    TASK_CHECK(!st.active);
    TASK_CHECK(st.start_frame == 32);
    TASK_CHECK(st.end_frame == 484);
    TASK_CHECK(total_read + st.dropped_frames == 484 - 32);
    TASK_CHECK(!p.error);

    p.frames = 0;
    SDL_SignalSemaphore(p.go);
    SDL_WaitThread(thread, NULL);
    SDL_DestroySemaphore(p.go);
    SDL_DestroySemaphore(p.done);
    SDL_free(p.buf);
    fixture_teardown(&f);
    return 0;
}

/* --- lifecycle group: creation, arguments, state machine, playback parity --- */

static int case_create_and_attach(void)
{
    TASK_CHECK(SDL_Init(0));
    TASK_CHECK(MIX_Init());
    const SDL_AudioSpec spec = { SDL_AUDIO_F32, TEST_CH, TEST_FREQ };
    MIX_Mixer *mixer = MIX_CreateMixer(&spec);
    TASK_CHECK(mixer != NULL);

    TASK_CHECK(MIX_CreateRecorder(NULL, 1, 2) == NULL);
    TASK_CHECK(MIX_CreateRecorder(mixer, -1, 2) == NULL);   /* negative preroll */
    TASK_CHECK(MIX_CreateRecorder(mixer, 5, 5) == NULL);    /* capacity must exceed preroll */
    TASK_CHECK(MIX_CreateRecorder(mixer, 10, 5) == NULL);
    TASK_CHECK(MIX_CreateRecorder(mixer, 0, 0) == NULL);

    MIX_Recorder *r0 = MIX_CreateRecorder(mixer, 0, 1);  /* zero preroll is valid */
    TASK_CHECK(r0 != NULL);

    /* exercise the minimal queue: capacity 1, no preroll */
    float buf[256 * TEST_CH];
    TASK_CHECK(MIX_Generate(mixer, buf, sizeof (buf)) >= 0);  /* 256 frames of silence */
    TASK_CHECK(MIX_StartRecorder(r0));
    TASK_CHECK(MIX_Generate(mixer, buf, 3 * TEST_FRAMESIZE) >= 0);  /* 1 fits, 2 are dropped */
    MIX_RecorderState st;
    TASK_CHECK(MIX_GetRecorderState(r0, &st));
    TASK_CHECK(!!st.active);
    TASK_CHECK(st.start_frame == 256);
    TASK_CHECK(st.end_frame == 259);
    TASK_CHECK(st.dropped_frames == 2);
    float pcm[16 * TEST_CH];
    Uint64 first = 0;
    TASK_CHECK(MIX_ReadRecorder(r0, pcm, 16, &first) == 1);
    TASK_CHECK(first == 256);
    TASK_CHECK(MIX_ReadRecorder(r0, pcm, 16, &first) == 0);
    TASK_CHECK(MIX_StopRecorder(r0));
    MIX_DestroyRecorder(r0);

    MIX_Recorder *r1 = MIX_CreateRecorder(mixer, 10, 64);
    TASK_CHECK(r1 != NULL);
    TASK_CHECK(MIX_CreateRecorder(mixer, 10, 64) == NULL);  /* one recorder per mixer */

    /* the failed attempts changed nothing: r1 still records */
    TASK_CHECK(MIX_Generate(mixer, buf, sizeof (buf)) >= 0);
    TASK_CHECK(MIX_StartRecorder(r1));
    TASK_CHECK(MIX_Generate(mixer, buf, sizeof (buf)) >= 0);
    TASK_CHECK(MIX_StopRecorder(r1));
    TASK_CHECK(MIX_ReadRecorder(r1, pcm, 16, &first) >= 0);
    MIX_DestroyRecorder(r1);

    MIX_Recorder *r3 = MIX_CreateRecorder(mixer, 10, 64);  /* attach slot freed by destroy */
    TASK_CHECK(r3 != NULL);
    MIX_DestroyMixer(mixer);  /* destroys the attached recorder along with the mixer */
    MIX_Quit();
    SDL_Quit();
    return 0;
}

static int case_argument_validation(void)
{
    Fixture f;
    if (fixture_setup(&f, 10, 64, true)) {
        return 1;
    }
    MIX_RecorderState st;
    float pcm[8 * TEST_CH];
    Uint64 first = 0;
    TASK_CHECK(!MIX_StartRecorder(NULL));
    TASK_CHECK(!MIX_StopRecorder(NULL));
    TASK_CHECK(!MIX_GetRecorderState(NULL, &st));
    TASK_CHECK(!MIX_GetRecorderState(f.rec, NULL));
    TASK_CHECK(MIX_ReadRecorder(NULL, pcm, 1, &first) == -1);
    TASK_CHECK(MIX_ReadRecorder(f.rec, NULL, 1, &first) == -1);
    TASK_CHECK(MIX_ReadRecorder(f.rec, pcm, 0, &first) == -1);
    TASK_CHECK(MIX_ReadRecorder(f.rec, pcm, -3, &first) == -1);
    TASK_CHECK(MIX_ReadRecorder(f.rec, pcm, 1, NULL) == -1);
    MIX_DestroyRecorder(NULL);  /* harmless no-op */

    TASK_CHECK(MIX_StopRecorder(f.rec));  /* never started: succeeds, changes nothing */
    if (check_state(&f, 0, 0, 0, 0)) {
        return 1;
    }
    if (generate(&f, 30)) {
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));
    TASK_CHECK(MIX_ReadRecorder(f.rec, NULL, 5, &first) == -1);  /* errors must not consume data */
    TASK_CHECK(MIX_ReadRecorder(f.rec, pcm, 0, &first) == -1);
    if (read_expect(&f, 100, 20, 10)) {  /* the preroll is still there */
        return 1;
    }
    TASK_CHECK(MIX_StopRecorder(f.rec));
    TASK_CHECK(MIX_StopRecorder(f.rec));  /* already stopped: succeeds */
    if (check_state(&f, 0, 20, 30, 0)) {
        return 1;
    }
    fixture_teardown(&f);
    return 0;
}

static int case_state_transitions(void)
{
    Fixture f;
    if (fixture_setup(&f, 20, 100, true)) {
        return 1;
    }
    if (check_state(&f, 0, 0, 0, 0)) {
        return 1;
    }
    if (generate(&f, 50) || check_state(&f, 0, 0, 0, 0)) {  /* no session yet: nothing advances */
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));
    if (check_state(&f, 1, 30, 50, 0)) {
        return 1;
    }
    if (generate(&f, 25) || check_state(&f, 1, 30, 75, 0)) {  /* end_frame walks with the mix */
        return 1;
    }
    TASK_CHECK(MIX_StopRecorder(f.rec));
    if (generate(&f, 15) || check_state(&f, 0, 30, 75, 0)) {  /* frozen once stopped */
        return 1;
    }
    if (read_expect(&f, 1000, 30, 45) || read_expect(&f, 10, 0, 0)) {  /* drain */
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));
    if (check_state(&f, 1, 70, 90, 0)) {  /* fresh session: new preroll, counters reset */
        return 1;
    }
    TASK_CHECK(MIX_StopRecorder(f.rec));
    fixture_teardown(&f);
    return 0;
}

static int case_playback_reference(void)
{
    Fixture f;
    if (fixture_setup(&f, 16, 64, true)) {
        return 1;
    }
    for (int i = 0; i < 20; i++) {  /* every generate() checks playback parity with the recorder-less mixer */
        if (generate(&f, 37)) {
            return 1;
        }
    }
    if (generate(&f, 1) || generate(&f, 512) || generate(&f, 255)) {
        return 1;
    }
    MIX_StopAllTracks(f.mix, 0);  /* silence must also match, including MIX_Generate's return value */
    MIX_StopAllTracks(f.ref, 0);
    if (generate(&f, 100)) {
        return 1;
    }
    TASK_CHECK(MIX_StartRecorder(f.rec));
    if (generate(&f, 100)) {  /* parity continues while recording */
        return 1;
    }
    TASK_CHECK(MIX_StopRecorder(f.rec));
    fixture_teardown(&f);
    return 0;
}

const struct task_case task_cases[] = {
    { "history", "preroll_and_continuation", case_preroll_and_continuation },
    { "history", "silence_counts", case_silence_counts },
    { "history", "ring_wraparound", case_ring_wraparound },
    { "history", "restart_recording", case_restart_recording },
    { "overrun", "queue_full_drops", case_queue_full_drops },
    { "overrun", "fragmented_reads", case_fragmented_reads },
    { "overrun", "tail_drop_and_drain", case_tail_drop_and_drain },
    { "overrun", "concurrent_no_overwrite", case_concurrent_no_overwrite },
    { "lifecycle", "create_and_attach", case_create_and_attach },
    { "lifecycle", "argument_validation", case_argument_validation },
    { "lifecycle", "state_transitions", case_state_transitions },
    { "lifecycle", "playback_reference", case_playback_reference }
};

const size_t task_case_count = SDL_arraysize(task_cases);
