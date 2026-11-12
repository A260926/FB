/*
 * ex_mbcompress.c
 *
 * Native C usage example for the sp_mbcompress stereo three-band
 * compressor. A synthetic stereo signal (three tones, one per band,
 * with an out-of-phase right channel) is processed sample by sample.
 * No audio device or file I/O is required.
 */

#include <math.h>
#include <stdio.h>
#include "soundpipe.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR 48000
#define DURATION_S 2
#define NFRAMES (SR * DURATION_S)

int main(void)
{
    sp_data *sp = NULL;
    sp_mbcompress *mb = NULL;
    int n;
    double peakIn = 0.0, peakOut = 0.0;

    sp_create(&sp);
    sp->sr = SR;

    /* fixed crossover frequencies: 250 Hz and 2.5 kHz */
    if (sp_mbcompress_create(&mb) != SP_OK ||
        sp_mbcompress_init(sp, mb, 250.0, 2500.0) != SP_OK) {
        fprintf(stderr, "mbcompress init failed\n");
        return 1;
    }

    /* gentle compression on each band, different settings per band */
    sp_mbcompress_set_threshold(mb, 0, -18.0);
    sp_mbcompress_set_ratio(mb, 0, 3.0);
    sp_mbcompress_set_attack(mb, 0, 0.01);
    sp_mbcompress_set_release(mb, 0, 0.1);

    sp_mbcompress_set_threshold(mb, 1, -15.0);
    sp_mbcompress_set_ratio(mb, 1, 2.5);

    sp_mbcompress_set_threshold(mb, 2, -12.0);
    sp_mbcompress_set_ratio(mb, 2, 2.0);

    /* blend in 20% phase-aligned dry signal */
    sp_mbcompress_set_wet(mb, 0.8);

    for (n = 0; n < NFRAMES; n++) {
        SPFLOAT inL, inR, outL, outR;
        double t = (double)n / SR;
        double low = 0.55 * sin(2.0 * M_PI * 90.0 * t);
        double mid = 0.35 * sin(2.0 * M_PI * 1000.0 * t);
        double high = 0.2 * sin(2.0 * M_PI * 6000.0 * t);

        inL = (SPFLOAT)(low + mid + high);
        inR = (SPFLOAT)(-(low + mid + high)); /* anti-phase stereo pair */

        /* in-place operation is allowed */
        outL = inL;
        outR = inR;
        sp_mbcompress_compute(sp, mb, &inL, &inR, &outL, &outR);

        if (fabs((double)inL) > peakIn) peakIn = fabs((double)inL);
        if (fabs((double)outL) > peakOut) peakOut = fabs((double)outL);
    }

    printf("processed %d stereo frames at %d Hz\n", NFRAMES, SR);
    printf("input peak  : %.4f\n", peakIn);
    printf("output peak : %.4f\n", peakOut);

    sp_mbcompress_destroy(&mb);
    sp_destroy(&sp);
    return 0;
}
