/* Half sample-rate preview example.
 *
 * Decodes an MP3 with MP3D_HALF_RATE, seeks into the middle, reads the suffix and verifies
 * it matches decoding the same file from the start. No MINIMP3_IMPLEMENTATION: the
 * implementation is linked from task_support/codec.c.
 *
 * Usage: half_rate <file.mp3>
 */
#include "minimp3_ex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_all(mp3dec_ex_t *dec, mp3d_sample_t *out, size_t samples)
{
    size_t done = 0;
    while (done < samples)
    {
        size_t g = mp3dec_ex_read(dec, out + done, samples - done);
        if (g == 0)
            break;
        done += g;
    }
    return done == samples;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "vectors/M2L3_noise.bit";
    mp3dec_ex_t dec;
    int ret = mp3dec_ex_open(&dec, path, MP3D_SEEK_TO_SAMPLE | MP3D_HALF_RATE);
    if (ret)
    {
        fprintf(stderr, "open failed: %d\n", ret);
        return 1;
    }

    printf("half-rate preview: hz=%d channels=%d half_samples=%zu\n",
           dec.info.hz, dec.info.channels, (size_t)dec.samples);

    mp3d_sample_t *full = (mp3d_sample_t *)malloc(dec.samples*sizeof(mp3d_sample_t));
    if (!full)
    {
        mp3dec_ex_close(&dec);
        return 1;
    }
    if (!read_all(&dec, full, dec.samples))
    {
        fprintf(stderr, "full read failed\n");
        free(full);
        mp3dec_ex_close(&dec);
        return 1;
    }

    /* seek to a channel-aligned point halfway through and read the suffix */
    size_t ch = (size_t)dec.info.channels;
    uint64_t target = ((dec.samples/2)/ch)*ch;
    ret = mp3dec_ex_seek(&dec, target);
    if (ret)
    {
        fprintf(stderr, "seek failed: %d\n", ret);
        free(full);
        mp3dec_ex_close(&dec);
        return 1;
    }
    size_t suffix_len = dec.samples - (size_t)target;
    mp3d_sample_t *suffix = (mp3d_sample_t *)malloc(suffix_len*sizeof(mp3d_sample_t));
    if (!suffix)
    {
        free(full);
        mp3dec_ex_close(&dec);
        return 1;
    }
    if (!read_all(&dec, suffix, suffix_len))
    {
        fprintf(stderr, "suffix read failed\n");
        free(full);
        free(suffix);
        mp3dec_ex_close(&dec);
        return 1;
    }

    if (memcmp(suffix, full + target, suffix_len*sizeof(mp3d_sample_t)) != 0)
    {
        fprintf(stderr, "suffix mismatch after seek\n");
        free(full);
        free(suffix);
        mp3dec_ex_close(&dec);
        return 1;
    }

    printf("seek to %llu verified: %zu suffix samples match the linear decode\n",
           (unsigned long long)target, suffix_len);
    free(full);
    free(suffix);
    mp3dec_ex_close(&dec);
    return 0;
}
