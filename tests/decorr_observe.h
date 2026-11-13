////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Independent decorrelation test support               //
//              Copyright (c) 2025 Conifer Software.                     //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// decorr_observe.h
//
// Read-only observation points for the decorrelation certification tests.
// This header is only included by the library when DECORR_TEST_OBSERVE is
// defined, which happens exclusively for the instrumented test build.  The
// shipped library configuration never defines it and therefore gains no
// callbacks, no extra state and no changed behavior.

#ifndef DECORR_OBSERVE_H
#define DECORR_OBSERVE_H

#include "wavpack_local.h"

#define DT_OBS_RESIDUAL 1
#define DT_OBS_PASS     2
#define DT_OBS_NORM     3   /* term 1-8 history after per-return normalization */

struct dt_obs_record {
    int kind;
    int mono;
    int term;
    int delta;
    int64_t start_sample;            /* absolute sample index of this decode call */
    uint32_t count;                  /* frames for residual records */
    int32_t weight_A, weight_B;
    int32_t samples_A [MAX_TERM];
    int32_t samples_B [MAX_TERM];
    int32_t values [256];            /* residual frames, interleaved stereo */
};

/* called from instrumented unpack.c only */
void dt_obs_residuals (const WavpackStream *wps, const int32_t *buffer, uint32_t count, int mono);
void dt_obs_pass (const WavpackStream *wps, const struct decorr_pass *dpp);
void dt_obs_norm (const WavpackStream *wps, const struct decorr_pass *dpp);

/* used by the test programs */
void dt_obs_reset (void);
int  dt_obs_pop (struct dt_obs_record *out);
long dt_obs_dropped (void);

#endif /* DECORR_OBSERVE_H */
