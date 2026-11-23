/*
 * Finite loop region example: preview a middle slice of a hand-built WAV,
 * repeat it, then continue into the tail. Actual decoding and an explicit
 * seek drive a 48 kHz stereo float32 file at .task-build/loop-region.f32.
 */

#include <SDL3/SDL.h>
#include <SDL3_sound/SDL_sound.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_PATH ".task-build/loop-region.f32"
#define FRAMES   256
#define RATE     32000

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static void put32(uint8_t *p, unsigned v) { put16(p, v); put16(p + 2, v >> 16); }

/* Asymmetric stereo material so channel handling is audible/visible. */
static int16_t lchan(unsigned i) { return (int16_t) (((i * 367) % 24000) - 12000); }
static int16_t rchan(unsigned i) { return (int16_t) (((i * 719) % 20000) - 10000); }

static size_t make_wav(uint8_t *p)
{
    const size_t z = 44 + (size_t) FRAMES * 4;
    memset(p, 0, z);
    memcpy(p, "RIFF", 4); put32(p + 4, (unsigned) (z - 8));
    memcpy(p + 8, "WAVEfmt ", 8);
    put32(p + 16, 16); put16(p + 20, 1); put16(p + 22, 2);
    put32(p + 24, RATE); put32(p + 28, RATE * 4);
    put16(p + 32, 4); put16(p + 34, 16);
    memcpy(p + 36, "data", 4); put32(p + 40, FRAMES * 4);
    for (unsigned i = 0; i < FRAMES; i++)
    {
        put16(p + 44 + i * 4, (uint16_t) lchan(i));
        put16(p + 46 + i * 4, (uint16_t) rchan(i));
    }
    return z;
}

int main(void)
{
    static uint8_t wav[44 + FRAMES * 4];
    const size_t wav_size = make_wav(wav);

    if (!SDL_Init(0) || !Sound_Init())
    {
        fprintf(stderr, "init failed: %s\n", Sound_GetError());
        return 1;
    }

    /* A fresh product every run; never reuse a stale file. */
    remove(OUT_PATH);

    SDL_AudioSpec want = { SDL_AUDIO_F32LE, 2, 48000 };
    Sound_Sample *s = Sound_NewSample(
        SDL_IOFromConstMem(wav, wav_size), "WAV", &want, 256);
    if (s == NULL)
    {
        fprintf(stderr, "open failed: %s\n", Sound_GetError());
        return 2;
    }

    /* Preview [40,120) three extra times, then play on to the end. */
    if (!Sound_SetLoop(s, 40, 120, 3))
    {
        fprintf(stderr, "setloop failed: %s\n", Sound_GetError());
        return 3;
    }

    FILE *f = fopen(OUT_PATH, "wb");
    if (f == NULL)
    {
        perror("open output");
        return 4;
    }

    unsigned long long frames_out = 0;
    double energy = 0.0;
    int saw_seek = 0;

    for (;;)
    {
        /* Halfway through, jump on the logical timeline to demonstrate an
           explicit seek: a zero-history suffix generation. */
        Sound_LoopPosition pos;
        if (!saw_seek && Sound_GetLoopPosition(s, &pos) &&
            pos.logical_frame >= 96)
        {
            if (!Sound_SeekLoopFrame(s, 96))
            {
                fprintf(stderr, "seek failed: %s\n", Sound_GetError());
                return 5;
            }
            saw_seek = 1;
        }

        const Uint32 br = Sound_Decode(s);
        if (s->flags & SOUND_SAMPLEFLAG_ERROR)
        {
            fprintf(stderr, "decode error\n");
            return 6;
        }
        if (br > 0)
        {
            const float *v = (const float *) s->buffer;
            const size_t floats = br / sizeof(float);
            if (fwrite(v, sizeof(float), floats, f) != floats)
            {
                perror("write output");
                return 7;
            }
            for (size_t i = 0; i < floats; i++)
                energy += fabs((double) v[i]);
            frames_out += floats / 2;
        }
        if (s->flags & SOUND_SAMPLEFLAG_EOF)
            break;
    }

    fclose(f);
    Sound_FreeSample(s);
    Sound_Quit();
    SDL_Quit();

    if (!saw_seek || frames_out < 64 || !(energy > 0.0) || !isfinite(energy))
    {
        fprintf(stderr, "bad product: seek=%d frames=%llu energy=%g\n",
                saw_seek, frames_out, energy);
        return 8;
    }

    printf("LOOP_EXAMPLE frames=%llu rate=48000 channels=2 seek=%d\n",
           frames_out, saw_seek);
    return 0;
}
