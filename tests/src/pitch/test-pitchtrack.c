/*
  Copyright (C) 2026 Paul Brossier <piem@aubio.org>

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

/* Regression test for the streaming YIN pitch tracker.

   The reference code below is written independently from
   src/pitch/pitchtrack.c.  It recomputes the YIN difference and cumulative
   mean normalized difference functions from the raw samples, extracts the
   candidates with the same documented rules, and enumerates every path
   through short candidate sequences by brute force (no dynamic programming).
   The object under test is then checked window by window for frequency,
   confidence, freeze timing and sample positions.

   All audio material is generated in C: low-amplitude sine tones, harmonic
   tones, short silences, and continuous glides.  No external audio is read.

   The explicit-candidate hook (aubio_pitchtrack_test_push_window) is used to
   build reproducible candidate sequences; the real PCM path, including
   candidate extraction, is exercised by the fixtures further down. */

#define AUBIO_PITCHTRACK_TEST 1

#include <aubio.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* test bookkeeping                                                          */

static unsigned test_count = 0;
static unsigned test_failures = 0;

#define TEST_CHECK(cond, ...)                                                 \
  do {                                                                        \
    test_count++;                                                             \
    if (!(cond)) {                                                            \
      test_failures++;                                                        \
      fprintf (stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                   \
      fprintf (stderr, __VA_ARGS__);                                          \
      fprintf (stderr, "\n");                                                 \
    }                                                                         \
  } while (0)

#define TEST_REQUIRE(cond, ...)                                               \
  do {                                                                        \
    int test_ok_ = (cond) ? 1 : 0;                                            \
    TEST_CHECK(test_ok_, __VA_ARGS__);                                        \
    if (!test_ok_) return;                                                    \
  } while (0)

/* ------------------------------------------------------------------------- */
/* mono signals, always generated locally                                    */

typedef struct
{
  smpl_t *data;
  unsigned length;
} test_sig_t;

static test_sig_t *
sig_new (unsigned length)
{
  test_sig_t *s = (test_sig_t *) calloc (1, sizeof (test_sig_t));
  if (!s) return NULL;
  s->length = length;
  s->data = (smpl_t *) calloc (length ? length : 1, sizeof (smpl_t));
  if (!s->data) {
    free (s);
    return NULL;
  }
  return s;
}

static void
sig_free (test_sig_t * s)
{
  if (!s) return;
  free (s->data);
  free (s);
}

static test_sig_t *
sig_sine (unsigned length, smpl_t freq, smpl_t amp, unsigned rate,
    unsigned offset)
{
  test_sig_t *s = sig_new (length);
  unsigned n;
  if (!s) return NULL;
  for (n = 0; n < length; n++) {
    s->data[n] = amp * sinf (2.f * (smpl_t) M_PI * freq
        * (smpl_t) (n + offset) / (smpl_t) rate);
  }
  return s;
}

static test_sig_t *
sig_silence (unsigned length)
{
  return sig_new (length);
}

/* phasor-continuous glide, so the waveform does not jump at the ends */
static test_sig_t *
sig_glide (unsigned length, smpl_t f0, smpl_t f1, smpl_t amp, unsigned rate)
{
  test_sig_t *s = sig_new (length);
  double phase = 0.;
  unsigned n;
  if (!s) return NULL;
  for (n = 0; n < length; n++) {
    double f = f0 + (f1 - f0) * (double) n / (double) length;
    s->data[n] = (smpl_t) (amp * sin (phase));
    phase += 2. * M_PI * f / (double) rate;
  }
  return s;
}

/* tone with a strong harmonic: several YIN valleys per window */
static test_sig_t *
sig_harmonic (unsigned length, smpl_t f0, double a1, double a2, unsigned rate)
{
  test_sig_t *s = sig_new (length);
  unsigned n;
  if (!s) return NULL;
  for (n = 0; n < length; n++) {
    s->data[n] = (smpl_t) (a1 * sin (2. * M_PI * f0 * n / (double) rate)
        + a2 * sin (2. * M_PI * 2. * f0 * n / (double) rate));
  }
  return s;
}

static test_sig_t *
sig_concat (const test_sig_t * a, const test_sig_t * b)
{
  test_sig_t *s = sig_new (a->length + b->length);
  if (!s) return NULL;
  memcpy (s->data, a->data, a->length * sizeof (smpl_t));
  memcpy (s->data + a->length, b->data, b->length * sizeof (smpl_t));
  return s;
}

/* ------------------------------------------------------------------------- */
/* independent reference: YIN candidates                                     */

#define REF_MAX_CAND 4096

typedef struct
{
  uint_t tau;
  smpl_t hz;
  smpl_t confidence;
  double c;
} ref_cand_t;

typedef struct
{
  unsigned n;
  int silent;
  ref_cand_t cand[REF_MAX_CAND];
} ref_window_t;

static void
ref_candidates (const smpl_t * x, unsigned n, unsigned rate, smpl_t min_hz,
    smpl_t max_hz, ref_window_t * out)
{
  unsigned w = n / 2, tau, j;
  double *d = (double *) calloc (w, sizeof (double));
  double *c = (double *) calloc (w, sizeof (double));
  double acc = 0., sum = 0.;
  for (j = 0; j < n; j++) acc += (double) x[j] * (double) x[j];
  out->n = 0;
  out->silent = sqrt (acc / (double) n) < 0.001;
  if (d && c) {
    for (tau = 1; tau < w; tau++) {
      double t, s2 = 0.;
      for (j = 0; j < w; j++) {
        t = (double) x[j] - (double) x[j + tau];
        s2 += t * t;
      }
      d[tau] = s2;
    }
    c[0] = 1.;
    for (tau = 1; tau < w; tau++) {
      sum += d[tau];
      c[tau] = (sum != 0.) ? ((double) tau * d[tau] / sum) : 1.;
    }
    if (!out->silent) {
      for (tau = 2; tau + 2 <= w; tau++) {
        smpl_t hz;
        if (!(c[tau] <= 0.4)) continue;
        if (!(c[tau] < c[tau - 1])) continue;
        if (!(c[tau] <= c[tau + 1])) continue;
        hz = (smpl_t) rate / (smpl_t) tau;
        if (hz < min_hz || hz > max_hz) continue;
        if (out->n >= REF_MAX_CAND) break;
        out->cand[out->n].tau = tau;
        out->cand[out->n].hz = hz;
        out->cand[out->n].confidence = (smpl_t) (1. - c[tau]);
        out->cand[out->n].c = c[tau];
        out->n++;
      }
    }
  }
  free (d);
  free (c);
}

/* ------------------------------------------------------------------------- */
/* independent reference: brute-force path search                            */

#define REF_MAX_WINDOWS 64
#define REF_MAX_STATES 16

typedef struct
{
  int silent;              /* 1 for the silent state */
  uint_t tau;
  smpl_t hz;
  smpl_t confidence;
  double emission;
} ref_state_t;

typedef struct
{
  unsigned n;
  ref_state_t state[REF_MAX_STATES];
} ref_states_t;

typedef struct
{
  unsigned n;
  ref_states_t win[REF_MAX_WINDOWS];
} ref_seq_t;

static double
ref_transition (const ref_state_t * a, const ref_state_t * b)
{
  if (a->silent && b->silent) return 0.;
  if (a->silent || b->silent) return 0.3;
  /* f2 / f1 = (rate / tau2) / (rate / tau1) = tau1 / tau2 */
  return 0.08 * fabs (12. * (log2 ((double) a->tau) - log2 ((double) b->tau)));
}

static double
ref_path_cost (const ref_state_t * path, unsigned n, const ref_state_t * prev)
{
  double total = 0.;
  unsigned i;
  for (i = 0; i < n; i++) {
    total += path[i].emission;
    if (i > 0) total += ref_transition (&path[i - 1], &path[i]);
    else if (prev) total += ref_transition (prev, &path[i]);
  }
  return total;
}

/* compare two paths by the tau sequence, silence (tau 0) first */
static int
ref_path_key_cmp (const ref_state_t * a, const ref_state_t * b, unsigned n)
{
  unsigned i;
  for (i = 0; i < n; i++) {
    uint_t ta = a[i].silent ? 0 : a[i].tau;
    uint_t tb = b[i].silent ? 0 : b[i].tau;
    if (ta < tb) return -1;
    if (ta > tb) return 1;
  }
  return 0;
}

/* enumerate every path through seq; lowest cost first, tau key second */
static double
ref_best_path (const ref_seq_t * seq, const ref_state_t * prev,
    ref_state_t * out)
{
  unsigned idx[REF_MAX_WINDOWS];
  ref_state_t path[REF_MAX_WINDOWS];
  double best = 0.;
  int have_best = 0;
  unsigned i, k, total = 1;

  for (i = 0; i < seq->n; i++) {
    if (seq->win[i].n > REF_MAX_STATES) return -1.;
    total *= seq->win[i].n;
  }
  for (i = 0; i < seq->n; i++) idx[i] = 0;

  for (i = 0; i < total; i++) {
    double cost;
    int better = 0;
    unsigned carry = 1;
    for (k = 0; k < seq->n; k++) path[k] = seq->win[k].state[idx[k]];
    cost = ref_path_cost (path, seq->n, prev);
    if (!have_best) {
      better = 1;
    } else if (cost < best - 1e-9) {
      better = 1;
    } else if (fabs (cost - best) <= 1e-9
        && ref_path_key_cmp (path, out, seq->n) < 0) {
      better = 1;
    }
    if (better) {
      best = cost;
      have_best = 1;
      for (k = 0; k < seq->n; k++) out[k] = path[k];
    }
    /* advance the mixed-radix odometer */
    for (k = 0; k < seq->n && carry; k++) {
      idx[k]++;
      if (idx[k] >= seq->win[k].n) {
        idx[k] = 0;
      } else {
        carry = 0;
      }
    }
  }
  return best;
}

/* cheapest state of each window separately: the answer to stay away from */
static void
ref_per_frame (const ref_seq_t * seq, ref_state_t * out)
{
  unsigned i, s;
  for (i = 0; i < seq->n; i++) {
    const ref_states_t *w = &seq->win[i];
    unsigned best = 0;
    for (s = 1; s < w->n; s++) {
      uint_t tb = w->state[s].silent ? 0 : w->state[s].tau;
      uint_t bb = w->state[best].silent ? 0 : w->state[best].tau;
      if (w->state[s].emission < w->state[best].emission - 1e-9
          || (fabs (w->state[s].emission - w->state[best].emission) <= 1e-9
              && tb < bb))
        best = s;
    }
    out[i] = w->state[best];
  }
}

typedef struct
{
  unsigned window_index;
  unsigned right_sample;
  smpl_t hz;
  smpl_t confidence;
} ref_result_t;

/* simulate the documented streaming policy: resolve at four pending
   windows, publish only the oldest, keep the rest pending; finish resolves
   the remainder */
static void
ref_stream (const ref_seq_t * seq, unsigned win_size, unsigned hop_size,
    ref_result_t * out, unsigned *out_n)
{
  const ref_states_t *pending[4];
  unsigned pending_right[4], pending_index[4];
  unsigned npending = 0, i, nout = 0;
  ref_state_t prev;
  int have_prev = 0;

  for (i = 0; i < seq->n; i++) {
    unsigned k;
    pending[npending] = &seq->win[i];
    pending_right[npending] = win_size + i * hop_size;
    pending_index[npending] = i;
    npending++;
    if (npending == 4) {
      ref_seq_t sub;
      ref_state_t path[REF_MAX_WINDOWS];
      sub.n = npending;
      for (k = 0; k < npending; k++) sub.win[k] = *pending[k];
      ref_best_path (&sub, have_prev ? &prev : NULL, path);
      out[nout].window_index = pending_index[0];
      out[nout].right_sample = pending_right[0];
      out[nout].hz = path[0].silent ? 0. : path[0].hz;
      out[nout].confidence = path[0].silent ? 0. : path[0].confidence;
      nout++;
      prev = path[0];
      have_prev = 1;
      for (k = 0; k + 1 < npending; k++) {
        pending[k] = pending[k + 1];
        pending_right[k] = pending_right[k + 1];
        pending_index[k] = pending_index[k + 1];
      }
      npending--;
    }
  }
  if (npending > 0) {
    ref_seq_t sub;
    ref_state_t path[REF_MAX_WINDOWS];
    unsigned k;
    sub.n = npending;
    for (k = 0; k < npending; k++) sub.win[k] = *pending[k];
    ref_best_path (&sub, have_prev ? &prev : NULL, path);
    for (k = 0; k < npending; k++) {
      out[nout].window_index = pending_index[k];
      out[nout].right_sample = pending_right[k];
      out[nout].hz = path[k].silent ? 0. : path[k].hz;
      out[nout].confidence = path[k].silent ? 0. : path[k].confidence;
      nout++;
    }
  }
  *out_n = nout;
}

/* ------------------------------------------------------------------------- */
/* capture of results published by the object under test                     */

#define MAX_RESULTS 1024

typedef struct
{
  unsigned n;
  aubio_pitchtrack_result_t res[MAX_RESULTS];
} capture_t;

static void
capture_cb (void *user_data, const aubio_pitchtrack_result_t * r)
{
  capture_t *c = (capture_t *) user_data;
  if (c->n < MAX_RESULTS) c->res[c->n] = *r;
  c->n++;
}

static int
same_result (const aubio_pitchtrack_result_t * a, const ref_result_t * b,
    double hz_tol, double conf_tol)
{
  if (a->window_index != b->window_index) return 0;
  if (a->right_sample != b->right_sample) return 0;
  if (fabs ((double) a->hz - (double) b->hz) > hz_tol) return 0;
  if (fabs ((double) a->confidence - (double) b->confidence) > conf_tol)
    return 0;
  return 1;
}

/* ------------------------------------------------------------------------- */
/* chunked PCM helpers                                                       */

static unsigned
push_chunks (aubio_pitchtrack_t * o, const test_sig_t * s, const unsigned *sizes,
    unsigned nsizes, capture_t * cap)
{
  unsigned pos = 0, i = 0;
  while (pos < s->length) {
    unsigned len = sizes[i % nsizes];
    fvec_t *chunk;
    if (len > s->length - pos) len = s->length - pos;
    if (len == 0) return 1;
    chunk = new_fvec (len);
    if (!chunk) return 1;
    memcpy (chunk->data, s->data + pos, len * sizeof (smpl_t));
    if (aubio_pitchtrack_push (o, chunk, capture_cb, cap) != 0) {
      del_fvec (chunk);
      return 1;
    }
    del_fvec (chunk);
    pos += len;
    i++;
  }
  return 0;
}

/* Build the reference candidate sequence for the windows of `s`.  Window k
   covers samples [k*hop, k*hop + win). */
static void
ref_sequence_from_pcm (const test_sig_t * s, unsigned win_size, unsigned hop_size,
    unsigned rate, smpl_t min_hz, smpl_t max_hz, ref_seq_t * seq)
{
  unsigned k, j;
  unsigned nwindows = (s->length - win_size) / hop_size + 1;
  seq->n = 0;
  for (k = 0; k < nwindows && k < REF_MAX_WINDOWS; k++) {
    unsigned start = k * hop_size;
    smpl_t *frame = (smpl_t *) calloc (win_size, sizeof (smpl_t));
    ref_window_t w;
    if (!frame) break;
    for (j = 0; j < win_size; j++) frame[j] = s->data[start + j];
    ref_candidates (frame, win_size, rate, min_hz, max_hz, &w);
    free (frame);
    if (w.n + 1 > REF_MAX_STATES) break;
    seq->win[k].n = w.n + 1;
    seq->win[k].state[0].silent = 1;
    seq->win[k].state[0].tau = 0;
    seq->win[k].state[0].hz = 0.;
    seq->win[k].state[0].confidence = 0.;
    seq->win[k].state[0].emission = 0.6;
    for (j = 0; j < w.n; j++) {
      seq->win[k].state[j + 1].silent = 0;
      seq->win[k].state[j + 1].tau = w.cand[j].tau;
      seq->win[k].state[j + 1].hz = w.cand[j].hz;
      seq->win[k].state[j + 1].confidence = w.cand[j].confidence;
      seq->win[k].state[j + 1].emission = w.cand[j].c;
    }
    if (w.silent) seq->win[k].n = 1;    /* silent window: silence only */
    seq->n = k + 1;
  }
}

/* feed a PCM signal, publish through finish, and compare with the reference */
static void
check_pcm_against_reference (const char *name, const test_sig_t * s,
    unsigned win_size, unsigned hop_size, unsigned rate, smpl_t min_hz,
    smpl_t max_hz, const unsigned *sizes, unsigned nsizes)
{
  ref_seq_t seq;
  ref_result_t expect[MAX_RESULTS];
  unsigned nexpect = 0, nwindows, i, before_finish;
  aubio_pitchtrack_t *o;
  capture_t cap;

  TEST_REQUIRE (s->length >= win_size, "%s: signal shorter than one window",
      name);
  nwindows = (s->length - win_size) / hop_size + 1;
  TEST_REQUIRE (nwindows <= REF_MAX_WINDOWS, "%s: too many windows", name);
  ref_sequence_from_pcm (s, win_size, hop_size, rate, min_hz, max_hz, &seq);
  TEST_REQUIRE (seq.n == nwindows, "%s: reference built %u of %u windows",
      name, seq.n, nwindows);
  ref_stream (&seq, win_size, hop_size, expect, &nexpect);

  o = new_aubio_pitchtrack (win_size, hop_size, rate, min_hz, max_hz);
  TEST_REQUIRE (o != NULL, "%s: failed to create pitchtrack", name);

  cap.n = 0;
  TEST_REQUIRE (push_chunks (o, s, sizes, nsizes, &cap) == 0,
      "%s: push failed", name);
  /* records are only published once four windows are pending */
  before_finish = (nwindows >= 4) ? nwindows - 3 : 0;
  TEST_CHECK (cap.n == before_finish,
      "%s: %u records published before finish, expected %u", name, cap.n,
      before_finish);
  TEST_REQUIRE (aubio_pitchtrack_finish (o, capture_cb, &cap) == 0,
      "%s: finish failed", name);

  TEST_CHECK (cap.n == nexpect, "%s: got %u records, expected %u", name,
      cap.n, nexpect);
  for (i = 0; i < cap.n && i < nexpect; i++) {
    TEST_CHECK (same_result (&cap.res[i], &expect[i], 2e-3, 2e-6),
        "%s: window %u got (%.4f, %.6f) at %u, expected (%.4f, %.6f) at %u",
        name, i, (double) cap.res[i].hz, (double) cap.res[i].confidence,
        cap.res[i].right_sample, (double) expect[i].hz,
        (double) expect[i].confidence, expect[i].right_sample);
  }
  del_aubio_pitchtrack (o);
}

/* whole signal, per sample, and alternating 1/7/113 chunks must agree */
static void
check_chunking_invariance (const test_sig_t * s, unsigned win_size,
    unsigned hop_size, unsigned rate, smpl_t min_hz, smpl_t max_hz)
{
  const unsigned whole[] = { MAX_RESULTS };
  const unsigned per_sample[] = { 1 };
  const unsigned mixed[] = { 1, 7, 113 };
  const unsigned *variants[] = { whole, per_sample, mixed };
  const unsigned sizes[] = { 1, 1, 3 };
  unsigned nvariants = 3, v, i;
  capture_t caps[3];
  aubio_pitchtrack_t *objs[3];
  int ok = 1;

  for (v = 0; v < nvariants; v++) {
    objs[v] = new_aubio_pitchtrack (win_size, hop_size, rate, min_hz, max_hz);
    TEST_CHECK (objs[v] != NULL, "chunking: failed to create object %u", v);
    if (!objs[v]) {
      ok = 0;
      caps[v].n = 0;
      continue;
    }
    caps[v].n = 0;
    if (push_chunks (objs[v], s, variants[v], sizes[v], &caps[v]) != 0
        || aubio_pitchtrack_finish (objs[v], capture_cb, &caps[v]) != 0) {
      TEST_CHECK (0, "chunking: run %u failed", v);
      ok = 0;
    }
  }

  if (ok) {
    for (v = 1; v < nvariants; v++) {
      TEST_CHECK (caps[v].n == caps[0].n, "chunking: record counts differ");
      for (i = 0; i < caps[0].n && i < caps[v].n; i++) {
        TEST_CHECK (caps[v].res[i].window_index
            == caps[0].res[i].window_index,
            "chunking: index differs at %u", i);
        TEST_CHECK (caps[v].res[i].right_sample
            == caps[0].res[i].right_sample,
            "chunking: right_sample differs at %u", i);
        TEST_CHECK (fabs ((double) caps[v].res[i].hz
                - (double) caps[0].res[i].hz) < 1e-6,
            "chunking: hz differs at %u", i);
        TEST_CHECK (fabs ((double) caps[v].res[i].confidence
                - (double) caps[0].res[i].confidence) < 1e-6,
            "chunking: confidence differs at %u", i);
      }
    }
  }
  for (v = 0; v < nvariants; v++) del_aubio_pitchtrack (objs[v]);
}

/* ------------------------------------------------------------------------- */
/* construction rules                                                        */

static void
check_construction_and_validation (void)
{
  aubio_pitchtrack_t *o;
  TEST_CHECK (new_aubio_pitchtrack (30, 10, 16000, 100, 1000) == NULL,
      "window smaller than 32 accepted");
  TEST_CHECK (new_aubio_pitchtrack (33, 10, 16000, 100, 1000) == NULL,
      "odd window size accepted");
  TEST_CHECK (new_aubio_pitchtrack (32, 0, 16000, 100, 1000) == NULL,
      "zero hop accepted");
  TEST_CHECK (new_aubio_pitchtrack (32, 33, 16000, 100, 1000) == NULL,
      "hop larger than window accepted");
  TEST_CHECK (new_aubio_pitchtrack (32, 1, 0, 100, 1000) == NULL,
      "zero samplerate accepted");
  TEST_CHECK (new_aubio_pitchtrack (32, 1, 16000, 0, 1000) == NULL,
      "zero min_hz accepted");
  TEST_CHECK (new_aubio_pitchtrack (32, 1, 16000, 1000, 100) == NULL,
      "min_hz > max_hz accepted");
  TEST_CHECK (new_aubio_pitchtrack (32, 1, 16000, 1000, 1000) == NULL,
      "min_hz == max_hz accepted");
  TEST_CHECK (new_aubio_pitchtrack (32, 1, 16000, 100, 8001) == NULL,
      "max_hz > samplerate/2 accepted");
  o = new_aubio_pitchtrack (32, 1, 16000, 100, 8000);
  TEST_CHECK (o != NULL, "valid boundary rejected");
  del_aubio_pitchtrack (o);
  o = new_aubio_pitchtrack (32, 32, 16000, 100, 8000);
  TEST_CHECK (o != NULL, "hop == window rejected");
  del_aubio_pitchtrack (o);
}

/* ------------------------------------------------------------------------- */
/* empty input, finish, reset                                                */

static void
check_empty_and_finish_semantics (void)
{
  const unsigned rate = 16000, win = 256, hop = 64;
  aubio_pitchtrack_t *o = new_aubio_pitchtrack (win, hop, rate, 50, 2000);
  fvec_t *empty = new_fvec (1);
  capture_t cap;

  TEST_REQUIRE (o != NULL && empty != NULL, "creation failed");
  cap.n = 0;

  /* empty vectors and NULL input do nothing */
  empty->length = 0;
  TEST_CHECK (aubio_pitchtrack_push (o, empty, capture_cb, &cap) == 0,
      "empty push failed");
  TEST_CHECK (cap.n == 0, "empty push produced records");
  TEST_CHECK (aubio_pitchtrack_push (o, NULL, capture_cb, &cap) == 0,
      "NULL push failed");
  TEST_CHECK (cap.n == 0, "NULL push produced records");

  /* fewer than one window: finish must not invent silence or records */
  {
    test_sig_t *s = sig_sine (win - 1, 250, 0.05, rate, 0);
    fvec_t *chunk = new_fvec (s->length);
    TEST_REQUIRE (s && chunk, "allocation failed");
    memcpy (chunk->data, s->data, s->length * sizeof (smpl_t));
    TEST_CHECK (aubio_pitchtrack_push (o, chunk, capture_cb, &cap) == 0,
        "short push failed");
    TEST_CHECK (aubio_pitchtrack_finish (o, capture_cb, &cap) == 0,
        "finish failed");
    TEST_CHECK (cap.n == 0, "finish produced a record for a partial window");
    del_fvec (chunk);
    sig_free (s);
  }

  /* repeated finish has no output, push afterwards fails */
  {
    TEST_CHECK (aubio_pitchtrack_finish (o, capture_cb, &cap) == 0,
        "second finish failed");
    TEST_CHECK (cap.n == 0, "second finish produced records");
    {
      test_sig_t *s = sig_sine (win, 250, 0.05, rate, 0);
      fvec_t *chunk = new_fvec (win);
      TEST_REQUIRE (s && chunk, "allocation failed");
      memcpy (chunk->data, s->data, win * sizeof (smpl_t));
      TEST_CHECK (aubio_pitchtrack_push (o, chunk, capture_cb, &cap) != 0,
          "push after finish succeeded");
      del_fvec (chunk);
      sig_free (s);
    }
  }

  /* reset restarts at window 0 and sample position N */
  {
    const unsigned nwin = 5;
    test_sig_t *s = sig_sine (win + (nwin - 1) * hop, 250, 0.05, rate, 0);
    fvec_t *chunk = new_fvec (s->length);
    TEST_REQUIRE (s && chunk, "allocation failed");
    memcpy (chunk->data, s->data, s->length * sizeof (smpl_t));
    cap.n = 0;
    aubio_pitchtrack_reset (o);
    TEST_CHECK (aubio_pitchtrack_push (o, chunk, capture_cb, &cap) == 0,
        "push after reset failed");
    TEST_CHECK (aubio_pitchtrack_finish (o, capture_cb, &cap) == 0,
        "finish after reset failed");
    TEST_CHECK (cap.n == nwin, "reset run produced %u records", cap.n);
    TEST_CHECK (cap.res[0].window_index == 0,
        "reset did not restart at window 0");
    TEST_CHECK (cap.res[0].right_sample == win,
        "reset did not restart at right_sample N");
    del_fvec (chunk);
    sig_free (s);
  }

  del_fvec (empty);
  del_aubio_pitchtrack (o);
}

/* publication happens exactly when the fourth pending window is complete,
   independent of how the chunks were split */
static void
check_freeze_timing (void)
{
  const unsigned rate = 16000, win = 256, hop = 64, nwin = 6;
  test_sig_t *s = sig_sine (win + (nwin - 1) * hop, 250, 0.05, rate, 0);
  aubio_pitchtrack_t *o = new_aubio_pitchtrack (win, hop, rate, 100, 1000);
  capture_t cap;
  unsigned published_at[8], n = 0, pos = 0;

  TEST_REQUIRE (s && o, "creation failed");
  cap.n = 0;
  while (pos < s->length) {
    fvec_t *one = new_fvec (1);
    unsigned before = cap.n;
    TEST_REQUIRE (one, "allocation failed");
    one->data[0] = s->data[pos];
    TEST_CHECK (aubio_pitchtrack_push (o, one, capture_cb, &cap) == 0,
        "sample push failed");
    while (before < cap.n) {
      if (n < 8) published_at[n] = pos + 1;
      n++;
      before++;
    }
    del_fvec (one);
    pos++;
  }
  TEST_CHECK (aubio_pitchtrack_finish (o, capture_cb, &cap) == 0,
      "finish failed");

  TEST_CHECK (n == nwin - 3, "expected %u records before finish, got %u",
      nwin - 3, n);
  TEST_CHECK (cap.n == nwin, "expected %u records, got %u", nwin, cap.n);
  TEST_CHECK (n > 0 && published_at[0] == win + 3 * hop,
      "first freeze at sample %u, expected %u",
      n > 0 ? published_at[0] : 0, win + 3 * hop);
  TEST_CHECK (n > 1 && published_at[1] == win + 4 * hop,
      "second freeze at sample %u", n > 1 ? published_at[1] : 0);
  TEST_CHECK (n > 2 && published_at[2] == win + 5 * hop,
      "third freeze at sample %u", n > 2 ? published_at[2] : 0);
  TEST_CHECK (cap.n > 0 && cap.res[0].right_sample == win,
      "first window does not end at N");
  del_aubio_pitchtrack (o);
  sig_free (s);
}

/* ------------------------------------------------------------------------- */
/* explicit candidate sequences                                              */

typedef struct
{
  uint_t taus[8];
  smpl_t conf[8];
  unsigned n;
  int silent;
} hook_window_t;

typedef struct
{
  unsigned n;
  hook_window_t win[REF_MAX_WINDOWS];
} hook_seq_t;

static void
hook_add (hook_seq_t * h, const uint_t * taus, const smpl_t * conf,
    unsigned n, int silent)
{
  unsigned i;
  h->win[h->n].n = n;
  h->win[h->n].silent = silent;
  for (i = 0; i < n; i++) {
    h->win[h->n].taus[i] = taus[i];
    h->win[h->n].conf[i] = conf[i];
  }
  h->n++;
}

static void
hook_to_ref_seq (const hook_seq_t * h, unsigned rate, ref_seq_t * seq)
{
  unsigned k, j;
  seq->n = h->n;
  for (k = 0; k < h->n; k++) {
    const hook_window_t *w = &h->win[k];
    seq->win[k].n = (w->silent ? 0 : w->n) + 1;
    seq->win[k].state[0].silent = 1;
    seq->win[k].state[0].tau = 0;
    seq->win[k].state[0].hz = 0.;
    seq->win[k].state[0].confidence = 0.;
    seq->win[k].state[0].emission = 0.6;
    for (j = 0; j < (w->silent ? 0 : w->n); j++) {
      seq->win[k].state[j + 1].silent = 0;
      seq->win[k].state[j + 1].tau = w->taus[j];
      seq->win[k].state[j + 1].hz = (smpl_t) rate / (smpl_t) w->taus[j];
      seq->win[k].state[j + 1].confidence = w->conf[j];
      seq->win[k].state[j + 1].emission = 1. - (double) w->conf[j];
    }
  }
}

static void
run_hook (const hook_seq_t * h, unsigned win, unsigned hop, unsigned rate,
    capture_t * cap)
{
  aubio_pitchtrack_t *o = new_aubio_pitchtrack (win, hop, rate, 10, 4000);
  unsigned k;
  TEST_REQUIRE (o != NULL, "hook: creation failed");
  cap->n = 0;
  for (k = 0; k < h->n; k++) {
    const hook_window_t *w = &h->win[k];
    TEST_CHECK (aubio_pitchtrack_test_push_window (o, w->taus, w->conf,
            w->n, w->silent, capture_cb, cap) == 0, "hook: push failed");
  }
  TEST_CHECK (aubio_pitchtrack_finish (o, capture_cb, cap) == 0,
      "hook: finish failed");
  del_aubio_pitchtrack (o);
}

static void
check_hook_against_reference (const char *name, const hook_seq_t * h,
    unsigned win, unsigned hop, unsigned rate)
{
  ref_seq_t seq;
  ref_result_t expect[MAX_RESULTS];
  unsigned nexpect = 0, i;
  capture_t cap;

  hook_to_ref_seq (h, rate, &seq);
  ref_stream (&seq, win, hop, expect, &nexpect);
  run_hook (h, win, hop, rate, &cap);
  TEST_CHECK (cap.n == nexpect, "%s: got %u records, expected %u", name,
      cap.n, nexpect);
  for (i = 0; i < cap.n && i < nexpect; i++) {
    TEST_CHECK (same_result (&cap.res[i], &expect[i], 1e-4, 1e-6),
        "%s: record %u differs: got (%.4f, %.6f) at %u, expected "
        "(%.4f, %.6f) at %u", name, i, (double) cap.res[i].hz,
        (double) cap.res[i].confidence, cap.res[i].right_sample,
        (double) expect[i].hz, (double) expect[i].confidence,
        expect[i].right_sample);
  }
}

/* optimal path differs from the per-frame minimum */
static void
check_path_not_per_frame (void)
{
  const unsigned rate = 16000, win = 256, hop = 64;
  hook_seq_t h;
  ref_seq_t seq;
  ref_state_t off[REF_MAX_WINDOWS], greedy[REF_MAX_WINDOWS];
  uint_t taus0[] = { 40, 37 };
  uint_t taus1[] = { 40, 37 };
  smpl_t conf0[] = { 0.90f, 0.90f };
  smpl_t conf1[] = { 0.92f, 0.90f };

  h.n = 0;
  hook_add (&h, taus0, conf0, 2, 0);
  hook_add (&h, taus1, conf1, 2, 0);
  hook_to_ref_seq (&h, rate, &seq);
  ref_best_path (&seq, NULL, off);
  ref_per_frame (&seq, greedy);
  TEST_CHECK (off[0].tau == 40 && off[1].tau == 40,
      "unexpected optimal path (%u, %u)", off[0].tau, off[1].tau);
  TEST_CHECK (greedy[0].tau == 37 && greedy[1].tau == 40,
      "unexpected per-frame path (%u, %u)", greedy[0].tau, greedy[1].tau);
  TEST_CHECK (off[0].tau != greedy[0].tau || off[1].tau != greedy[1].tau,
      "fixture does not separate the path and per-frame minima");
  check_hook_against_reference ("path-vs-per-frame", &h, win, hop, rate);
}

/* four-window local optimum differs from the whole-run offline optimum */
static void
check_local_vs_global (void)
{
  const unsigned rate = 16000, win = 256, hop = 64;
  hook_seq_t h;
  ref_seq_t seq;
  ref_result_t streamed[MAX_RESULTS], offline[MAX_RESULTS];
  ref_state_t offline_path[REF_MAX_WINDOWS];
  unsigned ns = 0, noff = 0, i, k;
  uint_t taus[] = { 64, 60 };
  smpl_t base[] = { 0.95f, 0.95f };
  smpl_t last[] = { 0.96f, 0.95f };

  h.n = 0;
  for (k = 0; k < 4; k++) hook_add (&h, taus, base, 2, 0);
  hook_add (&h, taus, last, 2, 0);
  hook_to_ref_seq (&h, rate, &seq);

  /* distinguish: first four windows locally, whole file globally */
  {
    ref_seq_t first4;
    ref_state_t local4[REF_MAX_WINDOWS];
    first4.n = 4;
    for (k = 0; k < 4; k++) first4.win[k] = seq.win[k];
    ref_best_path (&first4, NULL, local4);
    ref_best_path (&seq, NULL, offline_path);
    TEST_CHECK (local4[0].tau == 60 && offline_path[0].tau == 64,
        "fixture does not separate the four-window local optimum (%u) from "
        "the offline optimum (%u)", local4[0].tau, offline_path[0].tau);
  }

  ref_stream (&seq, win, hop, streamed, &ns);
  for (i = 0; i < seq.n; i++) {
    offline[i].window_index = i;
    offline[i].right_sample = win + i * hop;
    offline[i].hz = offline_path[i].silent ? 0. : offline_path[i].hz;
    offline[i].confidence = offline_path[i].silent ? 0.
        : offline_path[i].confidence;
  }
  noff = seq.n;
  TEST_CHECK (ns == noff, "unexpected record count");
  {
    int differs = 0;
    for (i = 0; i < ns && i < noff; i++) {
      if (fabs ((double) streamed[i].hz - (double) offline[i].hz) > 1e-3)
        differs = 1;
    }
    TEST_CHECK (differs, "streaming result matches the offline Viterbi");
  }
  check_hook_against_reference ("local-vs-global", &h, win, hop, rate);
}

/* silence must win inside the hook when flagged silent */
static void
check_hook_silence (void)
{
  const unsigned rate = 16000, win = 256, hop = 64;
  hook_seq_t h;
  uint_t taus[] = { 64 };
  smpl_t conf[] = { 0.9f };
  h.n = 0;
  hook_add (&h, taus, conf, 1, 0);
  hook_add (&h, taus, conf, 1, 1);
  hook_add (&h, taus, conf, 1, 1);
  hook_add (&h, taus, conf, 1, 0);
  check_hook_against_reference ("hook-silence", &h, win, hop, rate);
}

/* ------------------------------------------------------------------------- */
/* PCM fixtures                                                              */

static void
check_pcm_sine (void)
{
  const unsigned rate = 16000, win = 256, hop = 64, nwin = 5;
  test_sig_t *s = sig_sine (win + (nwin - 1) * hop, 250, 0.05, rate, 0);
  const unsigned chunks[] = { 1, 7, 113 };
  TEST_REQUIRE (s != NULL, "allocation failed");
  check_pcm_against_reference ("sine-250", s, win, hop, rate, 100, 1000,
      chunks, 3);
  check_chunking_invariance (s, win, hop, rate, 100, 1000);
  sig_free (s);
}

static void
check_pcm_harmonic (void)
{
  const unsigned rate = 16000, win = 256, hop = 64, nwin = 5;
  test_sig_t *s = sig_harmonic (win + (nwin - 1) * hop, 250, 0.05, 0.10, rate);
  const unsigned chunks[] = { 7, 1, 113 };
  TEST_REQUIRE (s != NULL, "allocation failed");
  check_pcm_against_reference ("harmonic-250", s, win, hop, rate, 100,
      1000, chunks, 3);
  check_chunking_invariance (s, win, hop, rate, 100, 1000);
  sig_free (s);
}

static void
check_pcm_silence_segment (void)
{
  const unsigned rate = 16000, win = 256, hop = 64;
  test_sig_t *a = sig_sine (win + 2 * hop, 250, 0.05, rate, 0);
  test_sig_t *b = sig_silence (3 * hop);
  test_sig_t *c = sig_sine (win + hop, 300, 0.05, rate, win + 2 * hop);
  test_sig_t *ab, *s;
  const unsigned chunks[] = { 113, 1, 7 };
  TEST_REQUIRE (a && b && c, "allocation failed");
  ab = sig_concat (a, b);
  TEST_REQUIRE (ab != NULL, "allocation failed");
  s = sig_concat (ab, c);
  TEST_REQUIRE (s != NULL, "allocation failed");
  check_pcm_against_reference ("silence-segment", s, win, hop, rate, 100,
      1000, chunks, 3);
  check_chunking_invariance (s, win, hop, rate, 100, 1000);
  sig_free (a);
  sig_free (b);
  sig_free (c);
  sig_free (ab);
  sig_free (s);
}

static void
check_pcm_glide (void)
{
  const unsigned rate = 16000, win = 256, hop = 64, nwin = 6;
  test_sig_t *s = sig_glide (win + (nwin - 1) * hop, 180, 320, 0.05, rate);
  const unsigned chunks[] = { 1, 113, 7 };
  TEST_REQUIRE (s != NULL, "allocation failed");
  check_pcm_against_reference ("glide", s, win, hop, rate, 100, 1000,
      chunks, 3);
  check_chunking_invariance (s, win, hop, rate, 100, 1000);
  sig_free (s);
}

/* candidates exactly on min_hz and max_hz are kept; outside -> silence */
static void
check_pcm_range (void)
{
  const unsigned rate = 16000, win = 256, hop = 64, nwin = 5;
  test_sig_t *s = sig_sine (win + (nwin - 1) * hop, 250, 0.05, rate, 0);
  const unsigned whole[] = { MAX_RESULTS };
  capture_t cap;
  aubio_pitchtrack_t *o;
  unsigned i;
  TEST_REQUIRE (s != NULL, "allocation failed");

  /* 250 Hz exactly at min_hz */
  o = new_aubio_pitchtrack (win, hop, rate, 250, 251);
  TEST_REQUIRE (o != NULL, "min-boundary object rejected");
  cap.n = 0;
  TEST_CHECK (push_chunks (o, s, whole, 1, &cap) == 0
      && aubio_pitchtrack_finish (o, capture_cb, &cap) == 0,
      "min-boundary run failed");
  TEST_CHECK (cap.n == nwin, "min-boundary record count %u", cap.n);
  for (i = 0; i < cap.n; i++) {
    TEST_CHECK (fabs ((double) cap.res[i].hz - 250.) < 1e-3,
        "candidate at min_hz was not kept at %u", i);
  }
  del_aubio_pitchtrack (o);

  /* 250 Hz exactly at max_hz */
  o = new_aubio_pitchtrack (win, hop, rate, 249, 250);
  TEST_REQUIRE (o != NULL, "max-boundary object rejected");
  cap.n = 0;
  TEST_CHECK (push_chunks (o, s, whole, 1, &cap) == 0
      && aubio_pitchtrack_finish (o, capture_cb, &cap) == 0,
      "max-boundary run failed");
  TEST_CHECK (cap.n == nwin, "max-boundary record count %u", cap.n);
  for (i = 0; i < cap.n; i++) {
    TEST_CHECK (fabs ((double) cap.res[i].hz - 250.) < 1e-3,
        "candidate at max_hz was not kept at %u", i);
  }
  del_aubio_pitchtrack (o);

  /* a range avoiding every valley can only produce silence */
  o = new_aubio_pitchtrack (win, hop, rate, 260, 1000);
  TEST_REQUIRE (o != NULL, "outside-range object rejected");
  cap.n = 0;
  TEST_CHECK (push_chunks (o, s, whole, 1, &cap) == 0
      && aubio_pitchtrack_finish (o, capture_cb, &cap) == 0,
      "outside-range run failed");
  TEST_CHECK (cap.n == nwin, "outside-range record count %u", cap.n);
  for (i = 0; i < cap.n; i++) {
    TEST_CHECK (cap.res[i].hz == 0. && cap.res[i].confidence == 0.,
        "outside-range produced a voiced state at %u", i);
  }
  del_aubio_pitchtrack (o);
  sig_free (s);
}

/* hop == window: one window every N samples */
static void
check_pcm_hop_equals_window (void)
{
  const unsigned rate = 16000, win = 256, hop = 256, nwin = 5;
  test_sig_t *s = sig_sine (win + (nwin - 1) * hop, 250, 0.05, rate, 0);
  const unsigned chunks[] = { 7, 113, 1 };
  TEST_REQUIRE (s != NULL, "allocation failed");
  check_pcm_against_reference ("hop-eq-win", s, win, hop, rate, 100, 1000,
      chunks, 3);
  sig_free (s);
}

/* the first window is not zero padded: with fewer than N samples nothing
   is published, and the first record ends exactly at sample N */
static void
check_no_zero_padding (void)
{
  const unsigned rate = 16000, win = 256, hop = 64, nwin = 4;
  test_sig_t *s = sig_sine (win + (nwin - 1) * hop, 250, 0.05, rate, 0);
  aubio_pitchtrack_t *o;
  capture_t cap;
  unsigned pos;
  TEST_REQUIRE (s != NULL, "allocation failed");
  o = new_aubio_pitchtrack (win, hop, rate, 100, 1000);
  TEST_REQUIRE (o != NULL, "creation failed");
  cap.n = 0;

  {
    fvec_t *part = new_fvec (win - 1);
    TEST_REQUIRE (part != NULL, "allocation failed");
    memcpy (part->data, s->data, (win - 1) * sizeof (smpl_t));
    TEST_CHECK (aubio_pitchtrack_push (o, part, capture_cb, &cap) == 0,
        "partial push failed");
    TEST_CHECK (cap.n == 0, "published before a full window");
    del_fvec (part);
  }
  for (pos = win - 1; pos < s->length; pos++) {
    fvec_t *one = new_fvec (1);
    TEST_REQUIRE (one != NULL, "allocation failed");
    one->data[0] = s->data[pos];
    TEST_CHECK (aubio_pitchtrack_push (o, one, capture_cb, &cap) == 0,
        "incremental push failed");
    del_fvec (one);
  }
  TEST_CHECK (aubio_pitchtrack_finish (o, capture_cb, &cap) == 0,
      "finish failed");
  TEST_CHECK (cap.n == nwin, "expected %u windows, got %u", nwin, cap.n);
  TEST_CHECK (cap.n > 0 && cap.res[0].right_sample == win,
      "first window right edge is %u, expected %u",
      cap.n > 0 ? cap.res[0].right_sample : 0, win);
  del_aubio_pitchtrack (o);
  sig_free (s);
}

/* ------------------------------------------------------------------------- */

int
main (void)
{
  check_construction_and_validation ();
  check_empty_and_finish_semantics ();
  check_freeze_timing ();
  check_path_not_per_frame ();
  check_local_vs_global ();
  check_hook_silence ();
  check_pcm_sine ();
  check_pcm_harmonic ();
  check_pcm_silence_segment ();
  check_pcm_glide ();
  check_pcm_range ();
  check_pcm_hop_equals_window ();
  check_no_zero_padding ();

  if (test_count == 0) {
    fprintf (stderr, "FAIL: no check was executed\n");
    printf ("PITCHTRACK_CHECKS_RUN=0\n");
    return 1;
  }
  if (test_failures != 0) {
    fprintf (stderr, "%u of %u checks failed\n", test_failures, test_count);
    printf ("PITCHTRACK_CHECKS_RUN=%u\n", test_count);
    return 1;
  }
  printf ("%u pitchtrack checks passed\n", test_count);
  printf ("PITCHTRACK_CHECKS_RUN=%u\n", test_count);
  return 0;
}
