/* SoX Resampler Library      Copyright (c) 2007-18 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Internal, test-only diagnostics for the variable-rate engine.  This is not
 * part of the public API; it lets the test-suite prove that a clone point was
 * taken while stage-crossing fades, slews and buffered input were live. */

#if !defined soxr_vr_diag_included
#define soxr_vr_diag_included

#include "soxr.h"

typedef struct {
  unsigned long stage_switches;  /* Total rate-stage crossings entered. */
  unsigned long xfade_starts;    /* Total stage-interpolation x-fades begun. */
  unsigned long fade_remaining;  /* Output samples left in the current crossfade.*/
  unsigned long slew_remaining;  /* Output samples left in the current slew. */
  unsigned long input_buffered;  /* Input samples held in the engine FIFOs. */
  unsigned long xfade_remaining; /* Input samples left in the current x-fade. */
} _soxr_vr_diag_t;

void _soxr_vr_diag(soxr_t p, _soxr_vr_diag_t * diag);

#endif
