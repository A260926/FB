/*
  Copyright (C) 2003-2026 Paul Brossier <piem@aubio.org>

  This file is part of aubio.

  aubio is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  aubio is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with aubio.  If not, see <http://www.gnu.org/licenses/>.

*/

/** \file

  Streaming single-voice pitch tracking from YIN period candidates

  This object runs the YIN cumulative mean normalized difference function on a
  sliding window and keeps *all* period candidates found in the configured
  frequency range.  Instead of returning the first candidate below a tolerance
  (as ::aubio_pitchyin_t does), it keeps the candidates from each window and
  resolves a continuous trajectory over a bounded look-ahead of four windows.
  The existing pitch objects and their latency are left unchanged.

  Candidates are taken directly from the complete YIN curve: for
  \f$W = N/2\f$,

  \f[ d(\tau) = \sum_{j=0}^{W-1} (x_j - x_{j+\tau})^2 \f]
  \f[ c(\tau) = \tau d(\tau) / \sum_{k=1}^{\tau} d(k) \f]

  with \f$c(\tau) = 1\f$ when the denominator is zero.  Every \f$\tau\f$ in
  \f$[2, W-2]\f$ whose \f$c(\tau)\f$ does not exceed 0.4, is a local minimum
  (\f$c(\tau) < c(\tau-1)\f$ and \f$c(\tau) \leq c(\tau+1)\f$), and whose
  frequency `samplerate / tau` lies in the configured range is kept.  No
  interpolation is performed.

  Each window also has a silent state (`hz = 0`, `confidence = 0`).  A window
  with RMS below 0.001 only exposes that silent state.

  The path cost is the sum of the per-window emission costs (c(tau) for voiced
  states, 0.6 for silence) plus transition costs between adjacent windows:
  0.08 * abs(12 * log2(f2 / f1)) between two voiced states, 0.3 when switching
  between voiced and silent, and 0 between two silent states.

  The object publishes at most one record per complete window, and only once
  four windows are pending (or when ::aubio_pitchtrack_finish is called).  The
  oldest pending window is frozen onto the best path through the pending
  windows under the constraint of the previously published state; the other
  windows may still be re-resolved when more audio arrives.  Ties in total
  cost are broken by the lexicographic order of the whole path's tau sequence,
  silence sorting before every voiced candidate.

  The tuneable window size N must be even and at least 32, the hop H must
  satisfy 1 <= H <= N, and 0 < min_hz < max_hz <= samplerate/2.

*/

#ifndef AUBIO_PITCHTRACK_H
#define AUBIO_PITCHTRACK_H

#ifdef __cplusplus
extern "C" {
#endif

/** pitch tracking object */
typedef struct _aubio_pitchtrack_t aubio_pitchtrack_t;

/** one published pitch tracking result

  The window index is zero-based.  `right_sample` is the position of the
  right edge of the window, counted in samples consumed since creation (or
  since the last call to ::aubio_pitchtrack_reset).

  A silent window has `hz = 0` and `confidence = 0`.

*/
typedef struct {
  uint_t window_index;  /**< zero-based index of the analysed window */
  uint_t right_sample;  /**< sample position of the window's right edge */
  smpl_t hz;            /**< estimated frequency in Hz, 0 for silence */
  smpl_t confidence;    /**< confidence in [0, 1], 0 for silence */
} aubio_pitchtrack_result_t;

/** callback invoked synchronously for each published result

  The callback must not re-enter the pitch tracking object.

  \param user_data pointer passed to ::aubio_pitchtrack_push or
         ::aubio_pitchtrack_finish
  \param result pointer to the published result, valid only during the call

*/
typedef void (*aubio_pitchtrack_callback_t) (void *user_data,
    const aubio_pitchtrack_result_t * result);

/** creation of the pitch tracking object

  \param win_size even analysis window length N, at least 32
  \param hop_size step H between consecutive windows, 1 <= H <= N
  \param samplerate sampling rate of the input signal
  \param min_hz lower bound of the candidate frequency range, > 0
  \param max_hz upper bound of the candidate frequency range, <= samplerate/2

  \return newly created ::aubio_pitchtrack_t, or NULL on invalid parameters

  The first window is emitted as soon as N real samples have been consumed; it
  is not zero-padded.  Consecutive windows then follow every H samples.  The
  internal buffers have a size fixed at creation and do not grow with the
  amount of audio processed.

*/
aubio_pitchtrack_t *new_aubio_pitchtrack (uint_t win_size, uint_t hop_size,
    uint_t samplerate, smpl_t min_hz, smpl_t max_hz);

/** deletion of the pitch tracking object

  \param o pitch tracking object as returned by ::new_aubio_pitchtrack()

*/
void del_aubio_pitchtrack (aubio_pitchtrack_t * o);

/** feed mono samples to the pitch tracker

  The input vector may have any length and is treated as mono.  Empty input
  (NULL or a vector of length 0) does nothing.  The way the host splits the
  stream into vectors does not change the candidates, the trajectory, or the
  reported positions.

  Every record ready to be published is passed synchronously to \p cb before
  this function returns.  A NULL callback discards the records but still
  advances the trajectory state.

  \param o pitch tracking object as returned by ::new_aubio_pitchtrack()
  \param input mono input samples of any length
  \param cb callback invoked for each published record, or NULL
  \param user_data opaque pointer passed to \p cb

  \return 0 if successful, non-zero otherwise (in particular after
  ::aubio_pitchtrack_finish has been called)

*/
uint_t aubio_pitchtrack_push (aubio_pitchtrack_t * o, const fvec_t * input,
    aubio_pitchtrack_callback_t cb, void *user_data);

/** flush the pending trajectory and publish every remaining window

  The remaining windows are resolved in one optimal path under the constraint
  of the last published state and all records are emitted.  No fake silence is
  appended: trailing samples shorter than one window produce no record.  After
  this call the object is finished; calling it again produces no output and
  subsequent ::aubio_pitchtrack_push calls fail until
  ::aubio_pitchtrack_reset is called.

  \param o pitch tracking object as returned by ::new_aubio_pitchtrack()
  \param cb callback invoked for each published record, or NULL
  \param user_data opaque pointer passed to \p cb

  \return 0 if successful, non-zero otherwise

*/
uint_t aubio_pitchtrack_finish (aubio_pitchtrack_t * o,
    aubio_pitchtrack_callback_t cb, void *user_data);

/** reset the pitch tracking object

  Clears all buffered audio, pending windows, published state, and counters.
  The next window is again window 0, ending at sample N.

  \param o pitch tracking object as returned by ::new_aubio_pitchtrack()

*/
void aubio_pitchtrack_reset (aubio_pitchtrack_t * o);

#if defined(AUBIO_PITCHTRACK_TEST)
/** test-only: append a window from an explicit candidate list

  This hook lets the regression tests feed reproducible candidate sequences
  without synthesising audio that produces them.  The same trajectory logic as
  the real PCM path is used.  It is only declared when the build defines
  AUBIO_PITCHTRACK_TEST; it is not part of the documented API.

  \param o pitch tracking object
  \param taus periods in samples, in ascending order
  \param confidences confidence of each candidate, in [0, 1]
  \param n number of candidates
  \param silent when non-zero, expose only the silent state
  \param cb callback invoked for each published record, or NULL
  \param user_data opaque pointer passed to \p cb

  \return 0 if successful, non-zero otherwise

*/
uint_t aubio_pitchtrack_test_push_window (aubio_pitchtrack_t * o,
    const uint_t * taus, const smpl_t * confidences, uint_t n,
    uint_t silent, aubio_pitchtrack_callback_t cb, void *user_data);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AUBIO_PITCHTRACK_H */
