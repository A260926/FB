/*
** Copyright (C) 2026 Erik de Castro Lopo <erikd@mega-nerd.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published by
** the Free Software Foundation; either version 2.1 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#ifndef TPDF_SHAPED_H
#define TPDF_SHAPED_H

#include "sndfile.h"
#include "common.h"

/*
** SFD_TPDF_SHAPED implementation. Triangular PDF dither (two independent
** uniform variates per sample) with first order error feedback, driven by a
** per-channel 64 bit integer PRNG (SplitMix64, see tpdf_shaped.c and
** DITHER_SHAPED.md). Only SFM_WRITE PCM_16/PCM_24 files with normalized
** float/double input are supported.
*/

/* Validate the SF_DITHER_INFO and install the TPDF write callbacks.
** Re-arming an already active session with the same type is a no-op that
** keeps the PRNG stream and error state. Returns 0 on success, an SFE_*
** code on failure (the previous configuration is left untouched). */
int tpdf_activate (SF_PRIVATE *psf, const SF_DITHER_INFO *info) ;

/* Restore the codec level write callbacks. The session state is kept so a
** later tpdf_activate() restarts from zero error and the configured seed. */
void tpdf_deactivate (SF_PRIVATE *psf) ;

/* Nonzero while the TPDF callbacks are installed on this file. */
int tpdf_is_active (SF_PRIVATE *psf) ;

/* Nonzero if psf->dither is owned by this module (as opposed to the legacy
** copy-only dither in dither.c). */
int tpdf_owns (SF_PRIVATE *psf) ;

/* SFC_SET_DITHER_SEED handler. data points to a uint64_t, datasize must be
** sizeof (uint64_t). Only allowed on SFM_WRITE before the first audio
** sample. Returns 0 on success, an SFE_* code on failure. */
int tpdf_set_seed (SF_PRIVATE *psf, void *data, int datasize) ;

/*
** Read only test hook. This symbol is deliberately not part of the public
** API (it is absent from the exported symbol lists) and only the test driver
** links against it. It exposes the raw PRNG state and the uniform variates
** actually drawn; it never exposes the production error term e or the
** quantized value q. Any output pointer may be NULL.
**
**	base_seed : configured seed (the documented default if never set)
**	bits      : 16 or 24
**	samples   : number of dithered samples (PRNG pairs) drawn on the channel
**	last_u/v  : most recent [0,1) variates drawn on the channel
**	rng_state : SplitMix64 state after the most recent draw on the channel
**
** Returns 0 on success, non-zero if there is no active session or the
** channel index is invalid.
*/
int sf_tpdf_test_observer (SNDFILE *sndfile, int channel, uint64_t *base_seed, int *bits,
							sf_count_t *samples, double *last_u, double *last_v, uint64_t *rng_state) ;

#endif
