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

// A device-free demonstration of SDL_CreateAudioStreamTap(): one generated
// stereo track feeds a producer (48 kHz playback) and two taps that convert
// the same shared PCM to 44.1 kHz and 32 kHz. Each tap's real converted
// content is compared against an independently converted reference.

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define INPUT_FRAMES 4800  // 100 ms at 48 kHz
#define MAX_OUT_FRAMES 8192

static void generate_tone(float *dst, int frames)
{
    for (int i = 0; i < frames; ++i) {
        const double t = (double)i / 48000.0;
        // Different frequencies and amplitudes per channel, no silence.
        dst[i * 2] = (float)(0.50 * sin(2.0 * M_PI * 440.0 * t));
        dst[i * 2 + 1] = (float)(0.25 * sin(2.0 * M_PI * 660.0 * t + 0.75));
    }
}

static int drain_all(SDL_AudioStream *stream, float *out, int max_frames, int channels)
{
    int frames = 0;
    const int frame_size = (int)sizeof(float) * channels;
    for (;;) {
        int got = SDL_GetAudioStreamData(stream, &out[frames * channels],
                                         (max_frames - frames) * frame_size);
        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            break;
        }
        frames += got / frame_size;
        if (frames >= max_frames) {
            break;
        }
    }
    return frames;
}

// Independent reference conversion: a fresh ordinary stream fed the same PCM.
static int reference_convert(const SDL_AudioSpec *in, const SDL_AudioSpec *out,
                             const float *input, int in_bytes,
                             float *output, int max_frames)
{
    SDL_AudioStream *ref = SDL_CreateAudioStream(in, out);
    if (!ref) {
        return -1;
    }
    if (!SDL_PutAudioStreamData(ref, input, in_bytes) || !SDL_FlushAudioStream(ref)) {
        SDL_DestroyAudioStream(ref);
        return -1;
    }
    int frames = drain_all(ref, output, max_frames, out->channels);
    SDL_DestroyAudioStream(ref);
    return frames;
}

static int content_matches(const float *a, const float *b, int samples)
{
    for (int i = 0; i < samples; ++i) {
        if (fabsf(a[i] - b[i]) > 2.0e-6f) {
            fprintf(stderr, "sample %d differs: %f vs %f\n", i, a[i], b[i]);
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    SDL_AudioSpec in_spec = { SDL_AUDIO_F32, 2, 48000 };
    SDL_AudioSpec out_44 = { SDL_AUDIO_F32, 2, 44100 };
    SDL_AudioSpec out_32 = { SDL_AUDIO_F32, 2, 32000 };

    float *input = (float *)SDL_malloc((size_t)INPUT_FRAMES * 2 * sizeof(float));
    if (!input) {
        return 1;
    }
    generate_tone(input, INPUT_FRAMES);
    const int in_bytes = INPUT_FRAMES * 2 * (int)sizeof(float);

    SDL_AudioStream *producer = SDL_CreateAudioStream(&in_spec, &in_spec);
    SDL_AudioStream *tap_44 = SDL_CreateAudioStreamTap(producer, &out_44);
    SDL_AudioStream *tap_32 = SDL_CreateAudioStreamTap(producer, &out_32);
    if (!producer || !tap_44 || !tap_32) {
        fprintf(stderr, "stream creation failed: %s\n", SDL_GetError());
        return 1;
    }

    if (!SDL_PutAudioStreamData(producer, input, in_bytes) || !SDL_FlushAudioStream(producer)) {
        fprintf(stderr, "put/flush failed: %s\n", SDL_GetError());
        return 1;
    }

    static float prod_out[MAX_OUT_FRAMES * 2];
    static float tap44_out[MAX_OUT_FRAMES * 2];
    static float tap32_out[MAX_OUT_FRAMES * 2];
    static float ref44[MAX_OUT_FRAMES * 2];
    static float ref32[MAX_OUT_FRAMES * 2];

    int prod_frames = drain_all(producer, prod_out, MAX_OUT_FRAMES, 2);
    int frames_44 = drain_all(tap_44, tap44_out, MAX_OUT_FRAMES, 2);
    int frames_32 = drain_all(tap_32, tap32_out, MAX_OUT_FRAMES, 2);
    int ref44_frames = reference_convert(&in_spec, &out_44, input, in_bytes, ref44, MAX_OUT_FRAMES);
    int ref32_frames = reference_convert(&in_spec, &out_32, input, in_bytes, ref32, MAX_OUT_FRAMES);

    int ok = 1;
    ok = ok && prod_frames == INPUT_FRAMES;
    ok = ok && content_matches(input, prod_out, INPUT_FRAMES * 2);
    ok = ok && frames_44 == ref44_frames && frames_44 > 0;
    ok = ok && frames_32 == ref32_frames && frames_32 > 0;
    ok = ok && frames_44 != frames_32;  // the two rates really differ
    ok = ok && content_matches(tap44_out, ref44, frames_44 * 2);
    ok = ok && content_matches(tap32_out, ref32, frames_32 * 2);

    // The stereo channels are genuinely different all the way through.
    for (int i = 0; ok && i < 64; ++i) {
        ok = (tap44_out[i * 2] != tap44_out[i * 2 + 1]);
    }

    if (ok) {
        printf("producer(48k)=%d frames, tap(44.1k)=%d frames, tap(32k)=%d frames\n",
               prod_frames, frames_44, frames_32);
        puts("AUDIO_TAP_DEMO_OK");
    } else {
        fputs("AUDIO_TAP_DEMO_FAILED\n", stderr);
    }

    SDL_DestroyAudioStream(tap_32);
    SDL_DestroyAudioStream(tap_44);
    SDL_DestroyAudioStream(producer);
    SDL_free(input);
    return ok ? 0 : 1;
}
