/*
  Copyright (C) 2026 Paul Brossier <piem@aubio.org>

  This file is part of aubio.

  aubio is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  aubio is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  Both the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with aubio.  If not, see <http://www.gnu.org/licenses/>.

*/

/** \file

  Harmonic / Percussive Source Separation (HPSS)

  This object splits a PCM stream into harmonic and percussive stems using
  median filtering of the short-time magnitude spectrum:

  - the harmonic estimate is the median of each frequency bin along the time
    axis,
  - the percussive estimate is the median of each analysis frame along the
    frequency axis,
  - complementary soft masks h^2 / (h^2 + p^2) and 1 - mask are applied to
    the original complex spectrum, keeping each channel's own phase.

  Analysis and synthesis reuse the phase vocoder path: a HanningZ window with
  hop size N/4 and 75% overlap. No external audio library is called.

  The object is fully streaming and allocates no heap memory outside of
  new_aubio_hpss() and del_aubio_hpss().

  \example aubiohpss.c

*/

#ifndef AUBIO_HPSS_H
#define AUBIO_HPSS_H

#ifdef __cplusplus
extern "C" {
#endif

/** harmonic / percussive source separation object */
typedef struct _aubio_hpss_t aubio_hpss_t;

/** create harmonic / percussive source separation object

  \param N analysis window and FFT size, a power of two not smaller than 64
  \param H hop size, must equal N/4
  \param channels number of channels, 1 or 2
  \param time_radius radius of the time median filter, in [1, 31]
  \param freq_radius radius of the frequency median filter, in [1, 31]

  \return the newly created object, or NULL if any argument is invalid

*/
aubio_hpss_t * new_aubio_hpss(uint_t N, uint_t H, uint_t channels,
    uint_t time_radius, uint_t freq_radius);

/** delete harmonic / percussive source separation object

  \param o hpss object as returned by new_aubio_hpss()

*/
void del_aubio_hpss(aubio_hpss_t *o);

/** process one hop of PCM samples

  Each call consumes exactly H samples per channel and writes H samples of
  each stem. The first time_radius calls output zeros while the look-ahead
  buffer fills; the total input/output latency, in samples, is returned by
  aubio_hpss_get_latency().

  \param o hpss object as returned by new_aubio_hpss()
  \param in input matrix, channels x H
  \param harmonic harmonic output matrix, channels x H
  \param percussive percussive output matrix, channels x H

  \return 0 on success; non-zero if the matrices have the wrong size, or if
  aubio_hpss_finish() has already been called. On failure no history is
  modified and the output matrices are not written.

*/
uint_t aubio_hpss_do(aubio_hpss_t *o, const fmat_t *in,
    fmat_t *harmonic, fmat_t *percussive);

/** close the input stream and drain the delayed stems

  The first call seals the input and appends the three zero-input analysis
  hops that cover the last input samples. Every call then delivers one block
  of H samples per stem.

  \param o hpss object as returned by new_aubio_hpss()
  \param harmonic harmonic output matrix, channels x H
  \param percussive percussive output matrix, channels x H

  \return 1 while a block was written, 0 once all time_radius + 3 tail blocks
  have been delivered (further calls keep returning 0 and write nothing),
  2 on a matrix size error (the input is not sealed and no state advances)

*/
uint_t aubio_hpss_finish(aubio_hpss_t *o,
    fmat_t *harmonic, fmat_t *percussive);

/** reset the object to its initial all-zero history

  Configuration (window, hop, channel count and median radii) is preserved
  and every buffered frame, median state and synthesis tail is cleared.

  \param o hpss object as returned by new_aubio_hpss()

*/
void aubio_hpss_reset(aubio_hpss_t *o);

/** get the stream latency in samples

  \param o hpss object as returned by new_aubio_hpss()

  \return latency D = (N - H) + time_radius * H

*/
uint_t aubio_hpss_get_latency(const aubio_hpss_t *o);

#ifdef AUBIO_HPSS_TEST_HOOKS
/* Test-only entry points. They exercise the exact production ring, median
 * engines and mask routine; no separate test implementation is kept. */

/** push one magnitude frame through the production time-median ring */
void aubio_hpss_test_push_magnitude(aubio_hpss_t *o, const smpl_t *mag);
/** current time median of one bin */
smpl_t aubio_hpss_test_harm_median(const aubio_hpss_t *o, uint_t bin);
/** production frequency median engine applied to a supplied frame */
void aubio_hpss_test_freq_medians(const aubio_hpss_t *o, const smpl_t *mag,
    smpl_t *out);
/** production soft-mask formula, returning both complementary masks */
void aubio_hpss_test_compute_mask(aubio_hpss_t *o, const smpl_t *h,
    const smpl_t *p, smpl_t *harm, smpl_t *perc);
/** drop the oldest frame, as done by the shrinking tail windows */
void aubio_hpss_test_shrink(aubio_hpss_t *o);
/** bounded-update observability counters */
uint_t aubio_hpss_test_time_inserts(const aubio_hpss_t *o);
uint_t aubio_hpss_test_time_removes(const aubio_hpss_t *o);
uint_t aubio_hpss_test_time_comparisons(const aubio_hpss_t *o);
uint_t aubio_hpss_test_freq_comparisons(const aubio_hpss_t *o);
uint_t aubio_hpss_test_ring_filled(const aubio_hpss_t *o);
uint_t aubio_hpss_test_ring_capacity(const aubio_hpss_t *o);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AUBIO_HPSS_H */
