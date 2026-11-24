/*
 * Preroll recorder demo.
 *
 * Generates an asymmetric stereo signal, plays it through a memory mixer,
 * starts a recording that includes preroll, and writes the captured PCM to
 * the float32 WAVE file given as the program's sole argument. Frames lost to
 * queue overflow are written as silence so the file keeps absolute time.
 */

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define RATE 48000
#define CHANNELS 2
#define SIGNAL_FRAMES 4096
#define PRE_FRAMES 1024
#define POST_FRAMES 2048
#define PREROLL_FRAMES 512
#define QUEUE_FRAMES 4096

static void SDLCALL demo_postmix(void *userdata, MIX_Mixer *mixer, const SDL_AudioSpec *spec, float *pcm, int samples)
{
    (void) userdata;
    (void) mixer;
    const int frames = samples / spec->channels;
    for (int i = 0; i < frames; ++i) {
        pcm[i * spec->channels] = pcm[i * spec->channels] * 0.5f + 0.001f;
        pcm[i * spec->channels + 1] = pcm[i * spec->channels + 1] * 0.25f - 0.001f;
    }
}

static void put_u16le(FILE *f, Uint16 v)
{
    unsigned char b[2] = { (unsigned char) (v & 0xff), (unsigned char) (v >> 8) };
    if (fwrite(b, 1, sizeof (b), f) != sizeof (b)) {
        SDL_OutOfMemory();
    }
}

static void put_u32le(FILE *f, Uint32 v)
{
    unsigned char b[4] = {
        (unsigned char) (v & 0xff),
        (unsigned char) ((v >> 8) & 0xff),
        (unsigned char) ((v >> 16) & 0xff),
        (unsigned char) ((v >> 24) & 0xff)
    };
    if (fwrite(b, 1, sizeof (b), f) != sizeof (b)) {
        SDL_OutOfMemory();
    }
}

static int write_float_wave(const char *path, const float *frames, Uint64 frame_count)
{
    const Uint32 data_bytes = (Uint32) (frame_count * CHANNELS * sizeof (float));
    const Uint32 riff_size = 36u + data_bytes;
    FILE *f = fopen(path, "wb");
    if (!f) {
        SDL_SetError("Could not open '%s' for writing", path);
        return -1;
    }

    int ok = 1;
    ok = ok && (fwrite("RIFF", 1, 4, f) == 4);
    put_u32le(f, riff_size);
    ok = ok && (fwrite("WAVE", 1, 4, f) == 4);

    ok = ok && (fwrite("fmt ", 1, 4, f) == 4);
    put_u32le(f, 16);                 // fmt chunk size.
    put_u16le(f, 3);                  // WAVE_FORMAT_IEEE_FLOAT.
    put_u16le(f, CHANNELS);
    put_u32le(f, RATE);
    put_u32le(f, (Uint32) RATE * CHANNELS * (Uint32) sizeof (float));
    put_u16le(f, CHANNELS * (Uint16) sizeof (float));
    put_u16le(f, 32);

    ok = ok && (fwrite("data", 1, 4, f) == 4);
    put_u32le(f, data_bytes);
    ok = ok && (fwrite(frames, sizeof (float), (size_t) frame_count * CHANNELS, f) == (size_t) frame_count * CHANNELS);

    if (fclose(f) != 0) {
        ok = 0;
    }
    if (!ok) {
        SDL_SetError("Failed writing WAVE file '%s'", path);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 1;
    MIX_Mixer *mixer = NULL;
    MIX_Recorder *recorder = NULL;
    MIX_Audio *audio = NULL;
    float *signal = NULL;
    float *output = NULL;
    float play_buf[64 * CHANNELS];

    if (argc != 2) {
        fprintf(stderr, "usage: %s output.wav\n", argv[0]);
        return 1;
    }

    if (!SDL_Init(0) || !MIX_Init()) {
        goto done;
    }

    SDL_AudioSpec spec = { SDL_AUDIO_F32, CHANNELS, RATE };
    mixer = MIX_CreateMixer(&spec);
    if (!mixer) {
        goto done;
    }

    // A deliberately asymmetric, non-zero stereo signal.
    signal = (float *) SDL_malloc((size_t) SIGNAL_FRAMES * CHANNELS * sizeof (float));
    if (!signal) {
        goto done;
    }
    for (int i = 0; i < SIGNAL_FRAMES; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * (double) i / 97.0;
        signal[i * 2] = 0.30f * (float) sin(phase) + 0.05f;
        signal[i * 2 + 1] = -0.22f * (float) sin(phase * 1.5) - 0.04f;
    }

    audio = MIX_LoadRawAudio(mixer, signal, (size_t) SIGNAL_FRAMES * CHANNELS * sizeof (float), &spec);
    if (!audio) {
        goto done;
    }
    MIX_Track *track = MIX_CreateTrack(mixer);
    if (!track || !MIX_SetTrackAudio(track, audio) || !MIX_PlayTrack(track, 0)) {
        goto done;
    }
    if (!MIX_SetMixerGain(mixer, 0.9f) || !MIX_SetPostMixCallback(mixer, demo_postmix, NULL)) {
        goto done;
    }

    recorder = MIX_CreateRecorder(mixer, PREROLL_FRAMES, QUEUE_FRAMES);
    if (!recorder) {
        goto done;
    }

    // Play a little before starting so preroll has real content.
    for (int i = 0; i < PRE_FRAMES; i += 64) {
        if (MIX_Generate(mixer, play_buf, (int) sizeof (play_buf)) < 0) {
            goto done;
        }
    }

    if (!MIX_StartRecorder(recorder)) {
        goto done;
    }
    for (int i = 0; i < POST_FRAMES; i += 64) {
        if (MIX_Generate(mixer, play_buf, (int) sizeof (play_buf)) < 0) {
            goto done;
        }
    }
    if (!MIX_StopRecorder(recorder)) {
        goto done;
    }

    MIX_RecorderState state;
    if (!MIX_GetRecorderState(recorder, &state)) {
        goto done;
    }
    const Uint64 session_frames = state.end_frame - state.start_frame;
    if (session_frames == 0 || session_frames > (Uint64) SDL_MAX_SINT32) {
        SDL_SetError("Unexpected recording length");
        goto done;
    }

    output = (float *) SDL_calloc((size_t) session_frames * CHANNELS, sizeof (float));
    if (!output) {
        goto done;
    }

    // Drain the queue; zero-fill every gap so the file timeline stays absolute.
    Uint64 cursor = 0;
    for (;;) {
        Uint64 first_frame = 0;
        float chunk[128 * CHANNELS];
        const int got = MIX_ReadRecorder(recorder, chunk, 128, &first_frame);
        if (got < 0) {
            goto done;
        } else if (got == 0) {
            MIX_RecorderState now;
            if (!MIX_GetRecorderState(recorder, &now)) {
                goto done;
            }
            if (!now.active) {
                break;  // fully drained.
            }
            continue;
        }

        const Uint64 offset = first_frame - state.start_frame;
        for (int i = 0; i < got; ++i) {
            output[(offset + (Uint64) i) * CHANNELS] = chunk[i * CHANNELS];
            output[(offset + (Uint64) i) * CHANNELS + 1] = chunk[i * CHANNELS + 1];
        }
        cursor = SDL_max(cursor, offset + (Uint64) got);
    }

    if (write_float_wave(argv[1], output, session_frames) < 0) {
        goto done;
    }

    printf("PREROLL_DEMO_OK\n");
    rc = 0;

done:
    if (rc != 0) {
        fprintf(stderr, "record_demo failed: %s\n", SDL_GetError());
    }
    SDL_free(output);
    if (recorder) {
        MIX_DestroyRecorder(recorder);
    }
    if (mixer) {
        MIX_DestroyMixer(mixer);
    }
    if (audio) {
        MIX_DestroyAudio(audio);
    }
    SDL_free(signal);
    MIX_Quit();
    SDL_Quit();
    return rc;
}
