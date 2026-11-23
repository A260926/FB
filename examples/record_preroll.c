/*
  Demo for the MIX_Recorder preroll recording interface.

  Generates and plays non-zero stereo audio on a device-less mixer, records
  it (including a preroll of what was playing just before the recording
  started), and writes the captured frames to a 32-bit float WAVE file.
  Regions where frames were dropped (queue overrun) are written as silence,
  preserving absolute time. The single argument is the output file path.

  Prints PREROLL_DEMO_OK on success; exits non-zero on any failure.
*/

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_FREQ 48000
#define DEMO_CH 2
#define DEMO_FRAMESIZE (DEMO_CH * (int) sizeof (float))
#define SIGNAL_FRAMES 48000        /* one second of source audio, looped */
#define PREROLL_FRAMES 4800        /* 100 ms of history in the recording */
#define CAPACITY_FRAMES (DEMO_FREQ * 3)
#define LEAD_IN_FRAMES 48000       /* generated before the recording starts */
#define RECORDED_FRAMES 57600      /* generated while recording */

static void SDLCALL postmix_cb(void *userdata, MIX_Mixer *mixer, const SDL_AudioSpec *spec, float *pcm, int samples)
{
    (void) userdata; (void) mixer; (void) spec;
    for (int i = 0; i < samples; i++) {  /* recorded data must be post-callback data */
        pcm[i] *= 0.9f;
    }
}

static void write_le32(Uint8 *dst, Uint32 v)
{
    dst[0] = (Uint8) (v & 0xFF);
    dst[1] = (Uint8) ((v >> 8) & 0xFF);
    dst[2] = (Uint8) ((v >> 16) & 0xFF);
    dst[3] = (Uint8) ((v >> 24) & 0xFF);
}

static void write_le16(Uint8 *dst, Uint16 v)
{
    dst[0] = (Uint8) (v & 0xFF);
    dst[1] = (Uint8) ((v >> 8) & 0xFF);
}

static int write_wave_header(FILE *f, Uint32 total_frames)
{
    Uint8 header[44];
    const Uint32 data_bytes = total_frames * (Uint32) DEMO_FRAMESIZE;
    SDL_memset(header, 0, sizeof (header));
    SDL_memcpy(header + 0, "RIFF", 4);
    write_le32(header + 4, 36 + data_bytes);
    SDL_memcpy(header + 8, "WAVE", 4);
    SDL_memcpy(header + 12, "fmt ", 4);
    write_le32(header + 16, 16);          /* fmt chunk size */
    write_le16(header + 20, 3);           /* IEEE float */
    write_le16(header + 22, DEMO_CH);
    write_le32(header + 24, DEMO_FREQ);
    write_le32(header + 28, DEMO_FREQ * DEMO_FRAMESIZE);
    write_le16(header + 32, DEMO_FRAMESIZE);
    write_le16(header + 34, 32);          /* bits per sample */
    SDL_memcpy(header + 36, "data", 4);
    write_le32(header + 40, data_bytes);
    return fwrite(header, 1, sizeof (header), f) == sizeof (header) ? 0 : -1;
}

int main(int argc, char **argv)
{
    int retval = 1;
    MIX_Mixer *mixer = NULL;
    MIX_Audio *audio = NULL;
    MIX_Track *track = NULL;
    MIX_Recorder *recorder = NULL;
    float *signal = NULL;
    float *genbuf = NULL;
    float *readbuf = NULL;
    FILE *out = NULL;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <output.wav>\n", argv[0]);
        return 2;
    }

    if (!SDL_Init(0)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    } else if (!MIX_Init()) {
        fprintf(stderr, "MIX_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const SDL_AudioSpec spec = { SDL_AUDIO_F32, DEMO_CH, DEMO_FREQ };
    mixer = MIX_CreateMixer(&spec);
    if (!mixer) {
        fprintf(stderr, "MIX_CreateMixer failed: %s\n", SDL_GetError());
        goto done;
    }

    /* one second of non-zero, asymmetric stereo */
    signal = (float *) SDL_malloc(SIGNAL_FRAMES * DEMO_FRAMESIZE);
    genbuf = (float *) SDL_malloc(RECORDED_FRAMES * DEMO_FRAMESIZE);
    readbuf = (float *) SDL_malloc(8192 * DEMO_FRAMESIZE);
    if (!signal || !genbuf || !readbuf) {
        fprintf(stderr, "out of memory\n");
        goto done;
    }
    for (int i = 0; i < SIGNAL_FRAMES; i++) {
        const double t = (double) i / (double) DEMO_FREQ;
        signal[2 * i] = (float) (0.35 * SDL_sin(2.0 * SDL_PI_D * 440.0 * t) + 0.10 * SDL_sin(2.0 * SDL_PI_D * 1320.0 * t));
        signal[2 * i + 1] = (float) (0.30 * SDL_sin(2.0 * SDL_PI_D * 554.37 * t));
    }

    audio = MIX_LoadRawAudio(mixer, signal, SIGNAL_FRAMES * DEMO_FRAMESIZE, &spec);
    track = MIX_CreateTrack(mixer);
    if (!audio || !track) {
        fprintf(stderr, "setup failed: %s\n", SDL_GetError());
        goto done;
    }
    if (!MIX_SetTrackAudio(track, audio) || !MIX_SetPostMixCallback(mixer, postmix_cb, NULL)) {
        fprintf(stderr, "setup failed: %s\n", SDL_GetError());
        goto done;
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) {
        fprintf(stderr, "setup failed: %s\n", SDL_GetError());
        goto done;
    }
    SDL_SetNumberProperty(props, MIX_PROP_PLAY_LOOPS_NUMBER, -1);
    const bool playing = MIX_PlayTrack(track, props);
    SDL_DestroyProperties(props);
    if (!playing) {
        fprintf(stderr, "MIX_PlayTrack failed: %s\n", SDL_GetError());
        goto done;
    }

    recorder = MIX_CreateRecorder(mixer, PREROLL_FRAMES, CAPACITY_FRAMES);
    if (!recorder) {
        fprintf(stderr, "MIX_CreateRecorder failed: %s\n", SDL_GetError());
        goto done;
    }

    /* play for a while so there is history to preroll, then record some more */
    if (MIX_Generate(mixer, genbuf, LEAD_IN_FRAMES * DEMO_FRAMESIZE) < 0) {
        fprintf(stderr, "MIX_Generate failed: %s\n", SDL_GetError());
        goto done;
    }
    if (!MIX_StartRecorder(recorder)) {
        fprintf(stderr, "MIX_StartRecorder failed: %s\n", SDL_GetError());
        goto done;
    }
    if (MIX_Generate(mixer, genbuf, RECORDED_FRAMES * DEMO_FRAMESIZE) < 0) {
        fprintf(stderr, "MIX_Generate failed: %s\n", SDL_GetError());
        goto done;
    }
    if (!MIX_StopRecorder(recorder)) {
        fprintf(stderr, "MIX_StopRecorder failed: %s\n", SDL_GetError());
        goto done;
    }

    MIX_RecorderState state;
    if (!MIX_GetRecorderState(recorder, &state)) {
        fprintf(stderr, "MIX_GetRecorderState failed: %s\n", SDL_GetError());
        goto done;
    }
    if (state.active || state.start_frame != LEAD_IN_FRAMES - PREROLL_FRAMES || state.end_frame != LEAD_IN_FRAMES + RECORDED_FRAMES) {
        fprintf(stderr, "unexpected recorder state\n");
        goto done;
    }

    out = fopen(argv[1], "wb");
    if (!out) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        goto done;
    }
    if (write_wave_header(out, (Uint32) (state.end_frame - state.start_frame)) != 0) {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        goto done;
    }

    /* drain the queue; dropped regions (none are expected here) become
       silence so the file keeps absolute time */
    static const float zeros[8192 * DEMO_CH];  /* zero-initialized */
    Uint64 pos = state.start_frame;
    for (;;) {
        Uint64 first = 0;
        const int n = MIX_ReadRecorder(recorder, readbuf, 8192, &first);
        if (n < 0) {
            fprintf(stderr, "MIX_ReadRecorder failed: %s\n", SDL_GetError());
            goto done;
        } else if (n == 0) {
            break;
        }
        if (first < pos) {
            fprintf(stderr, "recording frames out of order\n");
            goto done;
        }
        while (pos < first) {  /* zero-fill any dropped frames */
            const Uint64 chunk = SDL_min((Uint64) 8192, first - pos);
            if (fwrite(zeros, DEMO_FRAMESIZE, (size_t) chunk, out) != (size_t) chunk) {
                fprintf(stderr, "write failed\n");
                goto done;
            }
            pos += chunk;
        }
        if (fwrite(readbuf, DEMO_FRAMESIZE, (size_t) n, out) != (size_t) n) {
            fprintf(stderr, "write failed\n");
            goto done;
        }
        pos = first + (Uint64) n;
    }
    while (pos < state.end_frame) {  /* zero-fill frames dropped at the tail */
        const Uint64 chunk = SDL_min((Uint64) 8192, state.end_frame - pos);
        if (fwrite(zeros, DEMO_FRAMESIZE, (size_t) chunk, out) != (size_t) chunk) {
            fprintf(stderr, "write failed\n");
            goto done;
        }
        pos += chunk;
    }

    if (fclose(out) != 0) {
        out = NULL;
        fprintf(stderr, "close failed\n");
        goto done;
    }
    out = NULL;

    if (state.dropped_frames != 0) {
        fprintf(stderr, "unexpected dropped frames\n");
        goto done;
    }

    printf("PREROLL_DEMO_OK\n");
    retval = 0;

done:
    if (out) {
        fclose(out);
    }
    if (recorder) {
        MIX_DestroyRecorder(recorder);
    }
    if (track) {
        MIX_DestroyTrack(track);
    }
    if (audio) {
        MIX_DestroyAudio(audio);
    }
    if (mixer) {
        MIX_DestroyMixer(mixer);
    }
    SDL_free(signal);
    SDL_free(genbuf);
    SDL_free(readbuf);
    MIX_Quit();
    SDL_Quit();
    return retval;
}
