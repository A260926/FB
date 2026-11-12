/*
 * Example: sp_mbcompress
 *
 * Feeds a synthetic stereo signal (a low tone on the left, a high tone
 * on the right, plus a shared thump) through the three-band compressor
 * and prints the per-sample levels. No audio files are required.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "soundpipe.h"

int main(void)
{
    sp_data *sp;
    sp_mbcompress *mbc;
    long n, nframes;
    SPFLOAT inL, inR, outL, outR;

    sp_create(&sp);
    sp->sr = 44100;

    if (sp_mbcompress_create(&mbc) != SP_OK) {
        fprintf(stderr, "sp_mbcompress_create failed\n");
        return 1;
    }
    /* 250 Hz and 4 kHz crossover points, fixed for the whole run */
    if (sp_mbcompress_init(sp, mbc, 250.0, 4000.0) != SP_OK) {
        fprintf(stderr, "sp_mbcompress_init rejected the crossover freqs\n");
        return 1;
    }

    /* only the low band compresses; mid and high stay at ratio 1 */
    sp_mbcompress_threshold(mbc, 0, -20.0);
    sp_mbcompress_ratio(mbc, 0, 4.0);
    sp_mbcompress_attack(mbc, 0, 0.005);
    sp_mbcompress_release(mbc, 0, 0.08);
    sp_mbcompress_wet(mbc, 1.0);

    nframes = sp->sr / 8; /* 125 ms */
    for (n = 0; n < nframes; n++) {
        SPFLOAT t = (SPFLOAT)n / (SPFLOAT)sp->sr;
        /* left: loud 80 Hz tone, right: quiet 8 kHz tone */
        inL = 0.9 * sin(2.0 * M_PI * 80.0 * t);
        inR = 0.2 * sin(2.0 * M_PI * 8000.0 * t);

        /* in-place processing is supported */
        outL = inL;
        outR = inR;
        sp_mbcompress_compute(sp, mbc, &outL, &outR, &outL, &outR);

        if (n % 2048 == 0) {
            printf("n=%5ld in=(%+.4f %+.4f) out=(%+.4f %+.4f)\n",
                   n, inL, inR, outL, outR);
        }
    }

    sp_mbcompress_destroy(&mbc);
    sp_destroy(&sp);
    return 0;
}
