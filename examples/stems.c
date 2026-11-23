/*
examples/stems.c

End-to-end demonstration of the dual subband stem decoder, using only the in-memory
session API and the synthetic frame generator (no sound card and no external decoder).

It builds a short MPEG-1 Layer III stream, decodes it twice:
  1. natively, to obtain the reference float PCM, and
  2. through drmp3_stems with a time-varying cut schedule into low/high paths,
then checks that low + high reconstructs the native output, that both paths carry
sound, and that a seek returns exactly the suffix of a from-the-start linear read.
*/
#include "dr_mp3_stems.h"
#include "task_support/codec.h"
#include "task_support/stem_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define EX_FRAMES   16
#define EX_EPS      3e-5

static unsigned char g_stream[EX_FRAMES * STEM_FIX_MAX_FRAME_BYTES];

static size_t build_stream(void)
{
    stem_fix_item items[EX_FRAMES];
    unsigned i;
    for (i = 0; i < EX_FRAMES; i++)
    {
        /* Long blocks, dense content in both low and high subbands, alternating
           padding so the example covers VBR-style frame sizes. */
        items[i].desc = stem_fix_long(0, 2, i % 2, 0, 120, 0, 0, i + 1);
        items[i].backref = 0;
    }
    return stem_fix_build(g_stream, items, EX_FRAMES);
}

int main(void)
{
    size_t size = build_stream();
    if (size == 0)
    {
        fprintf(stderr, "example: frame build failed\n");
        return 1;
    }

    /* Reference native decode. */
    drmp3 native;
    if (!drmp3_init_memory(&native, g_stream, size, NULL))
    {
        fprintf(stderr, "example: native init failed\n");
        return 2;
    }
    unsigned channels = native.channels;
    unsigned rate = native.sampleRate;
    drmp3_uint64 total = drmp3_get_pcm_frame_count(&native);
    float *ref = (float*)malloc((size_t)total * channels * sizeof(float));
    if (ref == NULL) return 3;
    if (drmp3_read_pcm_frames_f32(&native, total, ref) != total)
    {
        fprintf(stderr, "example: native short read\n");
        return 4;
    }
    drmp3_uninit(&native);

    /* Dual subband decode. Cut moves at absolute audio granules: 2 per frame. */
    drmp3_stem_point schedule[] = {
        {0, 32}, {4, 16}, {12, 8}, {20, 24}, {28, 0}
    };
    drmp3_stems *stems = drmp3_stems_open_memory(g_stream, size, schedule,
        sizeof(schedule) / sizeof(schedule[0]));
    if (stems == NULL)
    {
        fprintf(stderr, "example: stems open failed\n");
        return 5;
    }
    if (drmp3_stems_channels(stems) != channels ||
        drmp3_stems_sample_rate(stems) != rate ||
        drmp3_stems_length(stems) != total)
    {
        fprintf(stderr, "example: format/length mismatch\n");
        return 6;
    }

    float *low = (float*)malloc((size_t)total * channels * sizeof(float));
    float *high = (float*)malloc((size_t)total * channels * sizeof(float));
    if (low == NULL || high == NULL) return 7;

    /* Read both paths in small chunks to exercise arbitrary block sizes. */
    drmp3_uint64 done = 0;
    while (done < total)
    {
        drmp3_uint64 r = drmp3_stems_read(stems, 53,
            low + done * channels, high + done * channels);
        if (r == 0) break;
        done += r;
    }
    if (done != total)
    {
        fprintf(stderr, "example: stems short read\n");
        return 8;
    }

    /* Verify the additive reconstruction and that both paths are genuinely active. */
    double max_err = 0, low_energy = 0, high_energy = 0;
    size_t i;
    for (i = 0; i < (size_t)total * channels; i++)
    {
        double err = fabs((double)low[i] + high[i] - ref[i]);
        if (err > max_err) max_err = err;
        low_energy += fabs(low[i]);
        high_energy += fabs(high[i]);
    }
    printf("frames=%llu channels=%u rate=%u max|low+high-native|=%.3g\n",
        (unsigned long long)total, channels, rate, max_err);
    printf("path energy  low=%.4g  high=%.4g\n", low_energy, high_energy);
    if (max_err > EX_EPS || low_energy <= 1.0 || high_energy <= 1.0)
    {
        fprintf(stderr, "example: split verification failed\n");
        return 9;
    }

    /* Seek to an interior, mid-frame position and compare the suffix. */
    drmp3_uint64 target = 7 * 1152 + 251;
    if (!drmp3_stems_seek(stems, target))
    {
        fprintf(stderr, "example: seek failed\n");
        return 10;
    }
    if (drmp3_stems_tell(stems) != target)
    {
        fprintf(stderr, "example: tell after seek wrong\n");
        return 11;
    }
    enum { SUFFIX = 128 };
    float s_low[SUFFIX * 2], s_high[SUFFIX * 2];
    if (drmp3_stems_read(stems, SUFFIX, s_low, s_high) != SUFFIX)
    {
        fprintf(stderr, "example: suffix read failed\n");
        return 12;
    }
    double suffix_err = 0;
    for (i = 0; i < SUFFIX * channels; i++)
    {
        double e = fabs((double)s_low[i] - low[(size_t)target * channels + i]);
        double f = fabs((double)s_high[i] - high[(size_t)target * channels + i]);
        if (e > suffix_err) suffix_err = e;
        if (f > suffix_err) suffix_err = f;
    }
    printf("seek=%llu suffix_frames=%d max_suffix_diff=%.3g\n",
        (unsigned long long)target, SUFFIX, suffix_err);
    if (suffix_err > EX_EPS)
    {
        fprintf(stderr, "example: suffix mismatch\n");
        return 13;
    }

    drmp3_stems_close(stems);
    free(ref);
    free(low);
    free(high);
    printf("EXAMPLE_OK dual paths active, sum matches native, seek suffix matches\n");
    return 0;
}
