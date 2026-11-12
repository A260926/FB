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

/* make the prototype of the test-only candidate hook visible while compiling
   this translation unit; it stays hidden from regular users of the header */
#define AUBIO_PITCHTRACK_TEST 1

#include "aubio_priv.h"
#include "fvec.h"
#include "pitch/pitchtrack.h"

/* window RMS below which only the silent state is kept */
#define PITCHTRACK_SILENCE_RMS 0.001
/* largest YIN cumulative mean normalized difference accepted as a valley */
#define PITCHTRACK_MAX_C 0.4
/* emission cost of the silent state */
#define PITCHTRACK_SILENT_COST 0.6
/* transition cost between a voiced and a silent state */
#define PITCHTRACK_SWITCH_COST 0.3
/* per-semitone transition weight between two voiced states */
#define PITCHTRACK_SEMITONE_COST 0.08
/* numeric tolerance used when comparing equal path costs */
#define PITCHTRACK_COST_EPS 1.e-9
/* number of pending windows that triggers a publication */
#define PITCHTRACK_PENDING_MAX 4

/** one possible state of a window */
typedef struct
{
  uint_t tau;              /**< period in samples, 0 for the silent state */
  smpl_t hz;               /**< frequency in Hz, 0 for the silent state */
  smpl_t confidence;       /**< confidence, 0 for the silent state */
  double emission;         /**< emission cost of this state */
} aubio_pitchtrack_state_t;

struct _aubio_pitchtrack_t
{
  uint_t win_size;         /**< analysis window length N */
  uint_t hop_size;         /**< hop between windows H */
  uint_t samplerate;       /**< samplerate */
  uint_t half;             /**< W = N / 2 */
  smpl_t min_hz;           /**< lower bound of the frequency range */
  smpl_t max_hz;           /**< upper bound of the frequency range */

  fvec_t *ring;            /**< ring buffer holding the latest N samples */
  fvec_t *window;          /**< ordered copy of the current window */
  uint_t ring_pos;         /**< next position to write in the ring */
  uint_t consumed;         /**< number of samples consumed so far */
  uint_t to_next;          /**< samples remaining until the next window */
  uint_t nwindows;         /**< number of windows produced so far */

  double *diff;            /**< difference function scratch, size half */
  double *cum;             /**< cumulative normalized difference, size half */

  /* pending windows, kept as a circular queue of at most four slots */
  aubio_pitchtrack_state_t *slots[PITCHTRACK_PENDING_MAX];
  uint_t slot_states[PITCHTRACK_PENDING_MAX];
  uint_t slot_index[PITCHTRACK_PENDING_MAX];
  uint_t slot_right[PITCHTRACK_PENDING_MAX];
  uint_t head;
  uint_t pending;

  /* dynamic programming scratch, one array per pending level */
  double *cost[PITCHTRACK_PENDING_MAX];
  uint_t *parent[PITCHTRACK_PENDING_MAX];

  /* scratch used while extracting candidates */
  aubio_pitchtrack_state_t *scratch;

  uint_t prev_valid;       /**< whether a state has already been published */
  aubio_pitchtrack_state_t prev; /**< last published state */
  uint_t finished;         /**< set once finish has been called */
};

static void
aubio_pitchtrack_resolve (aubio_pitchtrack_t * o, uint_t publish,
    aubio_pitchtrack_callback_t cb, void *user_data);

/* transition cost between two states */
static double
aubio_pitchtrack_transition (const aubio_pitchtrack_state_t * a,
    const aubio_pitchtrack_state_t * b)
{
  if (a->tau == 0 && b->tau == 0) return 0.;
  if (a->tau == 0 || b->tau == 0) return PITCHTRACK_SWITCH_COST;
  /* tau is inversely proportional to the frequency, and the ratio is
     independent of the samplerate: 12*log2(f2/f1) = 12*log2(tau1/tau2) */
  return PITCHTRACK_SEMITONE_COST
      * fabs (12. * log2 ((double) a->tau / (double) b->tau));
}

/* whether a YIN valley belongs to the configured candidate set */
static uint_t
aubio_pitchtrack_candidate_ok (aubio_pitchtrack_t * o, uint_t tau,
    double c)
{
  smpl_t hz;
  if (tau < 2 || tau > o->half - 2) return 0;
  if (c > PITCHTRACK_MAX_C) return 0;
  hz = o->samplerate / (smpl_t) tau;
  if (hz < o->min_hz || hz > o->max_hz) return 0;
  return 1;
}

/* fill `out` with the voiced candidates of one window, in ascending tau

   The returned entries do not include the silent state.  `out` must have
   room for at least o->half entries. */
static uint_t
aubio_pitchtrack_analyse (aubio_pitchtrack_t * o, const fvec_t * window,
    aubio_pitchtrack_state_t * out)
{
  const smpl_t *x = window->data;
  const uint_t w = o->half;
  double *d = o->diff;
  double *c = o->cum;
  double sum;
  uint_t j, tau, n = 0;

  for (tau = 1; tau < w; tau++) {
    double acc = 0.;
    for (j = 0; j < w; j++) {
      double tmp = (double) x[j] - (double) x[j + tau];
      acc += tmp * tmp;
    }
    d[tau] = acc;
  }

  /* c(tau) = tau * d(tau) / sum(d(1..tau)), or 1 when that sum is zero */
  c[0] = 1.;
  sum = 0.;
  for (tau = 1; tau < w; tau++) {
    sum += d[tau];
    c[tau] = (sum != 0.) ? (tau * d[tau] / sum) : 1.;
  }

  for (tau = 2; tau <= w - 2; tau++) {
    if (!(c[tau] <= PITCHTRACK_MAX_C)) continue;
    if (!(c[tau] < c[tau - 1])) continue;
    if (!(c[tau] <= c[tau + 1])) continue;
    if (!aubio_pitchtrack_candidate_ok (o, tau, c[tau])) continue;
    out[n].tau = tau;
    out[n].hz = o->samplerate / (smpl_t) tau;
    out[n].confidence = (smpl_t) (1. - c[tau]);
    out[n].emission = c[tau];
    n++;
  }
  return n;
}

/* root-mean-square of a full window */
static double
aubio_pitchtrack_rms (const fvec_t * window)
{
  const smpl_t *x = window->data;
  double acc = 0.;
  uint_t j;
  for (j = 0; j < window->length; j++) {
    acc += (double) x[j] * (double) x[j];
  }
  return SQRT (acc / (double) window->length);
}

/* store the state list of one pending slot

   When `silent` is non-zero the slot only carries the silent state; otherwise
   it carries the silent state followed by the `voiced_n` candidates already
   staged in o->scratch.  The silent state sorts before every voiced one since
   its tau is zero. */
static void
aubio_pitchtrack_store_slot (aubio_pitchtrack_t * o, uint_t slot,
    uint_t voiced_n, uint_t silent)
{
  aubio_pitchtrack_state_t *states = o->slots[slot];
  uint_t n = 0, i;

  states[n].tau = 0;
  states[n].hz = 0.;
  states[n].confidence = 0.;
  states[n].emission = PITCHTRACK_SILENT_COST;
  n++;
  if (!silent) {
    for (i = 0; i < voiced_n; i++) {
      states[n++] = o->scratch[i];
    }
  }
  o->slot_states[slot] = n;
}

/* append a fully built slot to the pending queue, resolving if it is full */
static uint_t
aubio_pitchtrack_commit_slot (aubio_pitchtrack_t * o,
    aubio_pitchtrack_callback_t cb, void *user_data)
{
  uint_t slot;
  if (o->pending >= PITCHTRACK_PENDING_MAX) return AUBIO_FAIL;
  slot = (o->head + o->pending) % PITCHTRACK_PENDING_MAX;
  o->slot_index[slot] = o->nwindows;
  /* the right edge is the number of samples consumed for this window: N for
     the first one, then N + k * H */
  o->slot_right[slot] = o->consumed;
  o->nwindows++;
  o->pending++;
  if (o->pending == PITCHTRACK_PENDING_MAX) {
    aubio_pitchtrack_resolve (o, 1, cb, user_data);
  }
  return AUBIO_OK;
}

/* lexicographic comparison of the two partial paths ending at states a and b
   of logical level `level`; returns 1 when path a comes first

   The whole pending path is compared, from its oldest window to `level`. */
static int
aubio_pitchtrack_path_less (aubio_pitchtrack_t * o, uint_t level,
    uint_t a, uint_t b)
{
  uint_t ta[PITCHTRACK_PENDING_MAX], tb[PITCHTRACK_PENDING_MAX];
  uint_t l, ia = a, ib = b;

  ta[level] = o->slots[(o->head + level) % PITCHTRACK_PENDING_MAX][ia].tau;
  tb[level] = o->slots[(o->head + level) % PITCHTRACK_PENDING_MAX][ib].tau;
  for (l = level; l > 0; l--) {
    ia = o->parent[l][ia];
    ib = o->parent[l][ib];
    ta[l - 1] = o->slots[(o->head + l - 1) % PITCHTRACK_PENDING_MAX][ia].tau;
    tb[l - 1] = o->slots[(o->head + l - 1) % PITCHTRACK_PENDING_MAX][ib].tau;
  }
  for (l = 0; l <= level; l++) {
    if (ta[l] < tb[l]) return 1;
    if (ta[l] > tb[l]) return 0;
  }
  return 0;
}

/* resolve the pending windows and publish the first `publish` of them

   The best path through all pending windows is computed under the constraint
   of the last published state.  The oldest `publish` windows are then frozen
   and reported; the remaining ones stay pending and may be re-resolved later. */
static void
aubio_pitchtrack_resolve (aubio_pitchtrack_t * o, uint_t publish,
    aubio_pitchtrack_callback_t cb, void *user_data)
{
  const uint_t n = o->pending;
  uint_t i, s, p;

  if (n == 0 || publish == 0 || publish > n) return;

  for (i = 0; i < n; i++) {
    const uint_t slot = (o->head + i) % PITCHTRACK_PENDING_MAX;
    const aubio_pitchtrack_state_t *states = o->slots[slot];
    const uint_t ns = o->slot_states[slot];
    if (i == 0) {
      for (s = 0; s < ns; s++) {
        o->cost[0][s] = states[s].emission;
        if (o->prev_valid) {
          o->cost[0][s] += aubio_pitchtrack_transition (&o->prev, &states[s]);
        }
        o->parent[0][s] = 0;
      }
    } else {
      const uint_t pslot = (o->head + i - 1) % PITCHTRACK_PENDING_MAX;
      const aubio_pitchtrack_state_t *pstates = o->slots[pslot];
      const uint_t pns = o->slot_states[pslot];
      for (s = 0; s < ns; s++) {
        double best = 0.;
        uint_t bestp = 0, first = 1;
        for (p = 0; p < pns; p++) {
          double v = o->cost[i - 1][p]
              + aubio_pitchtrack_transition (&pstates[p], &states[s])
              + states[s].emission;
          if (first || v < best - PITCHTRACK_COST_EPS) {
            best = v;
            bestp = p;
            first = 0;
          } else if (fabs (v - best) <= PITCHTRACK_COST_EPS
              && aubio_pitchtrack_path_less (o, i - 1, p, bestp)) {
            bestp = p;
          }
        }
        o->cost[i][s] = best;
        o->parent[i][s] = bestp;
      }
    }
  }

  /* pick the best final state, breaking cost ties on the full pending path */
  {
    const uint_t slot = (o->head + n - 1) % PITCHTRACK_PENDING_MAX;
    const uint_t ns = o->slot_states[slot];
    uint_t chosen[PITCHTRACK_PENDING_MAX];
    double best = 0.;
    uint_t bests = 0, first = 1;
    for (s = 0; s < ns; s++) {
      double v = o->cost[n - 1][s];
      if (first || v < best - PITCHTRACK_COST_EPS) {
        best = v;
        bests = s;
        first = 0;
      } else if (fabs (v - best) <= PITCHTRACK_COST_EPS
          && aubio_pitchtrack_path_less (o, n - 1, s, bests)) {
        bests = s;
      }
    }

    chosen[n - 1] = bests;
    for (i = n - 1; i > 0; i--) {
      chosen[i - 1] = o->parent[i][chosen[i]];
    }

    for (i = 0; i < publish; i++) {
      const uint_t slot_i = (o->head + i) % PITCHTRACK_PENDING_MAX;
      const aubio_pitchtrack_state_t *st = &o->slots[slot_i][chosen[i]];
      aubio_pitchtrack_result_t result;
      result.window_index = o->slot_index[slot_i];
      result.right_sample = o->slot_right[slot_i];
      result.hz = st->hz;
      result.confidence = st->confidence;
      o->prev = *st;
      o->prev_valid = 1;
      if (cb) cb (user_data, &result);
    }
  }

  /* drop the published windows from the queue */
  o->head = (o->head + publish) % PITCHTRACK_PENDING_MAX;
  o->pending -= publish;
}

aubio_pitchtrack_t *
new_aubio_pitchtrack (uint_t win_size, uint_t hop_size, uint_t samplerate,
    smpl_t min_hz, smpl_t max_hz)
{
  aubio_pitchtrack_t *o;
  uint_t i;

  if (win_size < 32 || (win_size % 2) != 0) {
    AUBIO_ERR ("pitchtrack: window size should be even and at least 32\n");
    return NULL;
  }
  if (hop_size < 1 || hop_size > win_size) {
    AUBIO_ERR ("pitchtrack: hop size should be in [1, window size]\n");
    return NULL;
  }
  if (samplerate == 0) {
    AUBIO_ERR ("pitchtrack: samplerate should be positive\n");
    return NULL;
  }
  if (!(min_hz > 0.) || !(min_hz < max_hz)
      || !(max_hz <= (smpl_t) samplerate / 2.)) {
    AUBIO_ERR ("pitchtrack: invalid frequency range\n");
    return NULL;
  }

  o = AUBIO_NEW (aubio_pitchtrack_t);
  if (!o) return NULL;
  o->win_size = win_size;
  o->hop_size = hop_size;
  o->samplerate = samplerate;
  o->half = win_size / 2;
  o->min_hz = min_hz;
  o->max_hz = max_hz;

  o->ring = new_fvec (win_size);
  o->window = new_fvec (win_size);
  o->diff = AUBIO_ARRAY (double, o->half);
  o->cum = AUBIO_ARRAY (double, o->half);
  o->scratch = AUBIO_ARRAY (aubio_pitchtrack_state_t, o->half);
  for (i = 0; i < PITCHTRACK_PENDING_MAX; i++) {
    o->slots[i] = AUBIO_ARRAY (aubio_pitchtrack_state_t, o->half);
    o->cost[i] = AUBIO_ARRAY (double, o->half);
    o->parent[i] = AUBIO_ARRAY (uint_t, o->half);
    o->slot_states[i] = 0;
    o->slot_index[i] = 0;
    o->slot_right[i] = 0;
  }
  if (!o->ring || !o->window || !o->diff || !o->cum || !o->scratch) {
    AUBIO_ERR ("pitchtrack: failed to allocate buffers\n");
    del_aubio_pitchtrack (o);
    return NULL;
  }
  for (i = 0; i < PITCHTRACK_PENDING_MAX; i++) {
    if (!o->slots[i] || !o->cost[i] || !o->parent[i]) {
      AUBIO_ERR ("pitchtrack: failed to allocate buffers\n");
      del_aubio_pitchtrack (o);
      return NULL;
    }
  }

  aubio_pitchtrack_reset (o);
  return o;
}

void
del_aubio_pitchtrack (aubio_pitchtrack_t * o)
{
  uint_t i;
  if (!o) return;
  for (i = 0; i < PITCHTRACK_PENDING_MAX; i++) {
    if (o->slots[i]) AUBIO_FREE (o->slots[i]);
    if (o->cost[i]) AUBIO_FREE (o->cost[i]);
    if (o->parent[i]) AUBIO_FREE (o->parent[i]);
  }
  if (o->scratch) AUBIO_FREE (o->scratch);
  if (o->diff) AUBIO_FREE (o->diff);
  if (o->cum) AUBIO_FREE (o->cum);
  if (o->ring) del_fvec (o->ring);
  if (o->window) del_fvec (o->window);
  AUBIO_FREE (o);
}

void
aubio_pitchtrack_reset (aubio_pitchtrack_t * o)
{
  uint_t i;
  if (!o) return;
  fvec_zeros (o->ring);
  fvec_zeros (o->window);
  o->ring_pos = 0;
  o->consumed = 0;
  o->to_next = o->win_size;
  o->nwindows = 0;
  o->head = 0;
  o->pending = 0;
  o->prev_valid = 0;
  o->prev.tau = 0;
  o->prev.hz = 0.;
  o->prev.confidence = 0.;
  o->prev.emission = PITCHTRACK_SILENT_COST;
  o->finished = 0;
  for (i = 0; i < PITCHTRACK_PENDING_MAX; i++) {
    o->slot_states[i] = 0;
    o->slot_index[i] = 0;
    o->slot_right[i] = 0;
  }
}

uint_t
aubio_pitchtrack_push (aubio_pitchtrack_t * o, const fvec_t * input,
    aubio_pitchtrack_callback_t cb, void *user_data)
{
  uint_t j;
  if (!o) return AUBIO_FAIL;
  if (o->finished) return AUBIO_FAIL;
  if (!input || input->length == 0) return AUBIO_OK;

  for (j = 0; j < input->length; j++) {
    o->ring->data[o->ring_pos] = input->data[j];
    o->ring_pos = (o->ring_pos + 1) % o->win_size;
    o->consumed++;
    o->to_next--;
    if (o->to_next == 0) {
      uint_t k, slot, n;
      for (k = 0; k < o->win_size; k++) {
        o->window->data[k] =
            o->ring->data[(o->ring_pos + k) % o->win_size];
      }
      n = aubio_pitchtrack_analyse (o, o->window, o->scratch);
      slot = (o->head + o->pending) % PITCHTRACK_PENDING_MAX;
      aubio_pitchtrack_store_slot (o, slot, n,
          aubio_pitchtrack_rms (o->window) < PITCHTRACK_SILENCE_RMS);
      if (aubio_pitchtrack_commit_slot (o, cb, user_data) != AUBIO_OK) {
        return AUBIO_FAIL;
      }
      o->to_next = o->hop_size;
    }
  }
  return AUBIO_OK;
}

uint_t
aubio_pitchtrack_finish (aubio_pitchtrack_t * o,
    aubio_pitchtrack_callback_t cb, void *user_data)
{
  if (!o) return AUBIO_FAIL;
  if (o->finished) return AUBIO_OK;
  if (o->pending > 0) {
    aubio_pitchtrack_resolve (o, o->pending, cb, user_data);
  }
  o->pending = 0;
  o->head = 0;
  o->finished = 1;
  return AUBIO_OK;
}

uint_t
aubio_pitchtrack_test_push_window (aubio_pitchtrack_t * o,
    const uint_t * taus, const smpl_t * confidences, uint_t n,
    uint_t silent, aubio_pitchtrack_callback_t cb, void *user_data)
{
  uint_t slot, i;
  if (!o) return AUBIO_FAIL;
  if (o->finished) return AUBIO_FAIL;
  if (n > 0 && (!taus || !confidences)) return AUBIO_FAIL;
  if (n >= o->half) return AUBIO_FAIL;

  for (i = 0; i < n; i++) {
    o->scratch[i].tau = taus[i];
    o->scratch[i].hz = o->samplerate / (smpl_t) taus[i];
    o->scratch[i].confidence = confidences[i];
    o->scratch[i].emission = 1. - (double) confidences[i];
  }
  slot = (o->head + o->pending) % PITCHTRACK_PENDING_MAX;
  aubio_pitchtrack_store_slot (o, slot, n, silent ? 1 : 0);
  return aubio_pitchtrack_commit_slot (o, cb, user_data);
}
