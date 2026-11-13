////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Independent decorrelation test support               //
//              Copyright (c) 2025 Conifer Software.                     //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// decorr_observe.c
//
// Storage side of the read-only observation points.  This translation unit
// is only linked into the decorrelation certification test binaries; it is
// never part of the regular library build and introduces no callbacks or
// state into production code by itself.

#include <string.h>
#include <stdint.h>

#include "wavpack_local.h"
#include "decorr_observe.h"

#define DT_OBS_CAP 4096

static struct dt_obs_record dt_ring [DT_OBS_CAP];
static volatile unsigned dt_head, dt_tail;
static long dt_dropped;

static struct dt_obs_record *dt_obs_slot (void)
{
    unsigned next = (dt_head + 1) % DT_OBS_CAP;

    if (next == dt_tail) {
        dt_dropped++;
        return NULL;
    }

    return &dt_ring [dt_head];
}

void dt_obs_residuals (const WavpackStream *wps, const int32_t *buffer, uint32_t count, int mono)
{
    struct dt_obs_record *r = dt_obs_slot ();
    uint32_t i, n;

    if (!r || count > 128)
        return;

    memset (r, 0, sizeof (*r));
    r->kind = DT_OBS_RESIDUAL;
    r->mono = mono;
    r->start_sample = wps->sample_index;
    r->count = count;
    n = count * (mono ? 1u : 2u);

    for (i = 0; i < n && i < 256; i++)
        r->values [i] = buffer [i];

    dt_head = (dt_head + 1) % DT_OBS_CAP;
}

void dt_obs_pass (const WavpackStream *wps, const struct decorr_pass *dpp)
{
    struct dt_obs_record *r = dt_obs_slot ();

    if (!r)
        return;

    memset (r, 0, sizeof (*r));
    r->kind = DT_OBS_PASS;
    r->mono = (wps->wphdr.flags & MONO_FLAG) != 0;
    r->term = dpp->term;
    r->delta = dpp->delta;
    r->start_sample = wps->sample_index;
    r->weight_A = dpp->weight_A;
    r->weight_B = dpp->weight_B;
    memcpy (r->samples_A, dpp->samples_A, sizeof (r->samples_A));
    memcpy (r->samples_B, dpp->samples_B, sizeof (r->samples_B));
    dt_head = (dt_head + 1) % DT_OBS_CAP;
}

void dt_obs_norm (const WavpackStream *wps, const struct decorr_pass *dpp)
{
    struct dt_obs_record *r = dt_obs_slot ();

    if (!r)
        return;

    memset (r, 0, sizeof (*r));
    r->kind = DT_OBS_NORM;
    r->mono = (wps->wphdr.flags & MONO_FLAG) != 0;
    r->term = dpp->term;
    r->start_sample = wps->sample_index;
    memcpy (r->samples_A, dpp->samples_A, sizeof (r->samples_A));
    memcpy (r->samples_B, dpp->samples_B, sizeof (r->samples_B));
    dt_head = (dt_head + 1) % DT_OBS_CAP;
}

void dt_obs_reset (void)
{
    dt_head = dt_tail = 0;
    dt_dropped = 0;
}

int dt_obs_pop (struct dt_obs_record *out)
{
    if (dt_tail == dt_head)
        return 0;

    *out = dt_ring [dt_tail];
    dt_tail = (dt_tail + 1) % DT_OBS_CAP;
    return 1;
}

long dt_obs_dropped (void)
{
    return dt_dropped;
}
