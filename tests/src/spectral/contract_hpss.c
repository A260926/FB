/*
  Contract tests for the native aubio_hpss_t harmonic / percussive source
  separation. No main(): task_support/runner.c drives the three groups.

  The median and soft-mask checks use an independent copy-sort reference,
  and the PCM checks use a short-window naive DFT/IDFT reference that
  re-derives the whole analysis/separation/synthesis pipeline without
  calling any aubio spectral routine.
*/

#define AUBIO_HPSS_TEST_HOOKS 1
#include <aubio.h>

#include "check.h"
#include "fft_probe.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* independent copy-sort median reference                              */
/* ------------------------------------------------------------------ */

static int cmp_double(const void *a, const void *b)
{
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/** median of a copy of vals[0..n-1], averaging the middle pair when even */
static double ref_median(const double *vals, uint_t n)
{
  double *copy;
  double out;
  if (n == 0) return 0.;
  copy = (double *)malloc(n * sizeof(double));
  REQUIRE(copy != NULL);
  memcpy(copy, vals, n * sizeof(double));
  qsort(copy, n, sizeof(double), cmp_double);
  if (n & 1u) out = copy[n / 2];
  else out = 0.5 * (copy[n / 2 - 1] + copy[n / 2]);
  free(copy);
  return out;
}

/* ------------------------------------------------------------------ */
/* naive DFT / IDFT reference, double precision                        */
/* ------------------------------------------------------------------ */

static void ref_dft(const double *x, uint_t N, double *re, double *im)
{
  uint_t k, n;
  for (k = 0; k < N; k++) {
    double sr = 0., si = 0.;
    for (n = 0; n < N; n++) {
      double a = 2. * M_PI * (double)k * (double)n / (double)N;
      sr += x[n] * cos(a);
      si -= x[n] * sin(a);
    }
    re[k] = sr;
    im[k] = si;
  }
}

static void ref_idft(const double *re, const double *im, uint_t N,
    double *x)
{
  uint_t k, n;
  for (n = 0; n < N; n++) {
    double acc = re[0]; /* DC is real */
    for (k = 1; k < N / 2; k++) {
      double a = 2. * M_PI * (double)k * (double)n / (double)N;
      /* positive frequencies count twice (conjugate negative bins) */
      acc += 2. * (re[k] * cos(a) - im[k] * sin(a));
    }
    acc += re[N / 2] * cos(M_PI * (double)n); /* Nyquist is real */
    x[n] = acc / (double)N;
  }
}

static double ref_window(uint_t i, uint_t N)
{
  return 0.5 * (1. - cos(2. * M_PI * (double)i / (double)N));
}

/* ------------------------------------------------------------------ */
/* contract_medians: bounded medians and complementary masks           */
/* ------------------------------------------------------------------ */

static void medians_time_axis(void)
{
  const uint_t N = 64, H = 16, tr = 2, fr = 1, bins = N / 2 + 1;
  const uint_t cap = 2 * tr + 1;
  const uint_t frames = 13;
  aubio_hpss_t *o = new_aubio_hpss(N, H, 1, tr, fr);
  smpl_t *mag = (smpl_t *)malloc(bins * sizeof(smpl_t));
  double *history = (double *)malloc(frames * bins * sizeof(double));
  uint_t m, k, rep;
  REQUIRE(o != NULL && mag != NULL && history != NULL);
  CASE("time median: duplicates, equal values leaving, ring eviction");
  for (m = 0; m < frames; m++) {
    for (k = 0; k < bins; k++) {
      /* repeated values and exact zeros on purpose: identities must keep
       * the sliding window exact when equal members leave */
      mag[k] = (smpl_t)((double)((m * 7 + k * 3) % 5) * 0.25);
      history[(size_t)m * bins + k] = mag[k];
    }
    aubio_hpss_test_push_magnitude(o, mag);
    CHECK(aubio_hpss_test_ring_filled(o) == (m + 1 < cap ? m + 1 : cap));
    for (rep = 0; rep < 4; rep++) {
      k = (uint_t)((rep * 11 + m) % bins);
      uint_t lo = (m + 1 > cap) ? m + 1 - cap : 0;
      double vals[64];
      uint_t n = 0, j;
      for (j = lo; j <= m; j++) vals[n++] = history[(size_t)j * bins + k];
      CHECK(fabs((double)aubio_hpss_test_harm_median(o, k)
          - ref_median(vals, n)) < 1e-6);
    }
  }
  /* shrinking tail: even member counts average the two middle values */
  CASE("time median: tail shrink with even truncated windows");
  {
    uint_t remaining = cap;
    while (remaining > 0) {
      for (k = 0; k < bins; k += 5) {
        double vals[64];
        uint_t n = 0, j;
        for (j = 0; j < remaining; j++) {
          uint_t frame = frames - remaining + j;
          vals[n++] = history[(size_t)frame * bins + k];
        }
        CHECK(fabs((double)aubio_hpss_test_harm_median(o, k)
            - ref_median(vals, n)) < 1e-6);
      }
      aubio_hpss_test_shrink(o);
      remaining--;
      CHECK(aubio_hpss_test_ring_filled(o) == remaining);
    }
  }
  free(history);
  free(mag);
  del_aubio_hpss(o);
}

static void medians_freq_axis(void)
{
  const uint_t N = 64, H = 16, bins = N / 2 + 1;
  smpl_t *mag = (smpl_t *)malloc(bins * sizeof(smpl_t));
  smpl_t *got = (smpl_t *)malloc(bins * sizeof(smpl_t));
  uint_t fr;
  REQUIRE(mag != NULL && got != NULL);
  for (fr = 1; fr <= 3; fr++) {
    aubio_hpss_t *o = new_aubio_hpss(N, H, 1, 1, fr);
    uint_t k, j;
    REQUIRE(o != NULL);
    CASE("frequency median: clipped DC/Nyquist neighbourhoods");
    for (k = 0; k < bins; k++)
      mag[k] = (smpl_t)((double)((k * 5 + 2) % 7) * 0.125); /* duplicates */
    mag[0] = 0.f;
    mag[bins - 1] = 0.f;
    aubio_hpss_test_freq_medians(o, mag, got);
    for (k = 0; k < bins; k++) {
      uint_t lo = (k > fr) ? k - fr : 0;
      uint_t hi = (k + fr < bins) ? k + fr : bins - 1;
      double vals[64];
      uint_t n = 0;
      for (j = lo; j <= hi; j++) vals[n++] = mag[j];
      CHECK(fabs((double)got[k] - ref_median(vals, n)) < 1e-6);
    }
    del_aubio_hpss(o);
  }
  free(got);
  free(mag);
}

static void medians_masks(void)
{
  const uint_t N = 64, H = 16, bins = N / 2 + 1;
  aubio_hpss_t *o = new_aubio_hpss(N, H, 1, 1, 1);
  smpl_t *h = (smpl_t *)malloc(bins * sizeof(smpl_t));
  smpl_t *p = (smpl_t *)malloc(bins * sizeof(smpl_t));
  smpl_t *mh = (smpl_t *)malloc(bins * sizeof(smpl_t));
  smpl_t *mp = (smpl_t *)malloc(bins * sizeof(smpl_t));
  uint_t k;
  REQUIRE(o != NULL && h && p && mh && mp);
  CASE("soft masks: formula, both-zero half, strict complement");
  for (k = 0; k < bins; k++) {
    h[k] = (smpl_t)(0.1 * (k % 4));
    p[k] = (smpl_t)(0.1 * ((k + 2) % 5));
  }
  h[3] = 0.f; p[3] = 0.f;
  aubio_hpss_test_compute_mask(o, h, p, mh, mp);
  for (k = 0; k < bins; k++) {
    double expect;
    if (h[k] == 0.f && p[k] == 0.f) expect = 0.5;
    else {
      double h2 = (double)h[k] * h[k], p2 = (double)p[k] * p[k];
      expect = h2 / (h2 + p2);
    }
    CHECK(fabs((double)mh[k] - expect) < 1e-6);
    CHECK(fabs((double)(mh[k] + mp[k]) - 1.) < 1e-7);
    CHECK(fabs((double)mp[k] - (1. - (double)mh[k])) < 1e-7);
  }
  free(mp);
  free(mh);
  free(p);
  free(h);
  del_aubio_hpss(o);
}

void contract_medians(void)
{
  medians_time_axis();
  medians_freq_axis();
  medians_masks();
}

/* ------------------------------------------------------------------ */
/* full double-precision reference HPSS pipeline for the PCM checks    */
/* ------------------------------------------------------------------ */

typedef struct {
  uint_t N, H, bins, channels, tr, fr, frames;
  double **norm;     /* [frames][channels*bins] */
  double **phas;
  double **mag;      /* [frames][bins] */
  double *window;
} ref_hpss_t;

static ref_hpss_t *ref_analyze(uint_t N, uint_t H, uint_t channels,
    uint_t B, const fmat_t *in)
{
  uint_t end = N - H, frames = B + 3, m, c, k, n;
  ref_hpss_t *r = (ref_hpss_t *)calloc(1, sizeof(ref_hpss_t));
  double *dataold = (double *)calloc(channels * end, sizeof(double));
  double *data = (double *)calloc(N, sizeof(double));
  double *grain = (double *)calloc(N, sizeof(double));
  double *re = (double *)calloc(N, sizeof(double));
  double *im = (double *)calloc(N, sizeof(double));
  REQUIRE(r && dataold && data && grain && re && im);
  r->N = N; r->H = H; r->bins = N / 2 + 1; r->channels = channels;
  r->tr = 0; r->fr = 0; r->frames = frames;
  r->window = (double *)calloc(N, sizeof(double));
  r->norm = (double **)calloc(frames, sizeof(double *));
  r->phas = (double **)calloc(frames, sizeof(double *));
  r->mag = (double **)calloc(frames, sizeof(double *));
  for (n = 0; n < N; n++) r->window[n] = ref_window(n, N);
  for (m = 0; m < frames; m++) {
    r->norm[m] = (double *)calloc(channels * r->bins, sizeof(double));
    r->phas[m] = (double *)calloc(channels * r->bins, sizeof(double));
    r->mag[m] = (double *)calloc(r->bins, sizeof(double));
    for (c = 0; c < channels; c++) {
      for (n = 0; n < end; n++)
        data[n] = dataold[c * end + n];
      for (n = 0; n < H; n++)
        data[end + n] = (m < B) ? (double)in->data[c][m * H + n] : 0.;
      for (n = 0; n < end; n++)
        dataold[c * end + n] = data[n + H];
      for (n = 0; n < N; n++)
        grain[n] = data[(n + N / 2) % N] * r->window[(n + N / 2) % N];
      ref_dft(grain, N, re, im);
      for (k = 0; k < r->bins; k++) {
        double rv = re[k];
        double iv = (k == 0 || k == N / 2) ? 0. : im[k];
        r->norm[m][c * r->bins + k] = sqrt(rv * rv + iv * iv);
        r->phas[m][c * r->bins + k] = atan2(iv, rv);
      }
    }
    for (k = 0; k < r->bins; k++) {
      double acc = 0.;
      for (c = 0; c < channels; c++) {
        double v = r->norm[m][c * r->bins + k];
        acc += v * v;
      }
      r->mag[m][k] = sqrt(acc / (double)channels);
    }
  }
  free(im); free(re); free(grain); free(data); free(dataold);
  return r;
}

/** reference overlap-add synthesis of one masked frame for one channel */
static void ref_synth(ref_hpss_t *r, const double *masked_norm,
    const double *phas, double *synthold, double *out_block)
{
  uint_t N = r->N, H = r->H, end = N - H, start = N - 2 * H;
  double *re = (double *)calloc(N, sizeof(double));
  double *im = (double *)calloc(N, sizeof(double));
  double *x = (double *)calloc(N, sizeof(double));
  double *g = (double *)calloc(N, sizeof(double));
  uint_t k, n;
  for (k = 0; k < r->bins; k++) {
    double rv = masked_norm[k] * cos(phas[k]);
    double iv = masked_norm[k] * sin(phas[k]);
    re[k] = rv;
    if (k > 0 && k < N / 2) im[k] = iv;
  }
  ref_idft(re, im, N, x);
  /* inverse half-window swap, then the synthesis hanningz window */
  for (n = 0; n < N; n++) {
    uint_t src = (n + N / 2) % N;
    g[n] = r->window[n] * x[src];
  }
  for (n = 0; n < H; n++) out_block[n] = (2. / 3.) * g[n];
  for (n = 0; n < H; n++) out_block[n] += synthold[n];
  for (n = 0; n < start; n++) synthold[n] = synthold[n + H];
  for (n = start; n < end; n++) synthold[n] = 0.;
  for (n = 0; n < end; n++) synthold[n] += (2. / 3.) * g[n + H];
  free(g);
  free(x); free(im); free(re);
}

/** full reference stems over the B+tr+3 output blocks of the timeline */
static void ref_run(ref_hpss_t *r, uint_t tr, uint_t fr,
    double **out_h, double **out_p)
{
  uint_t blocks = r->frames + tr; /* (B+3) + tr */
  uint_t total = blocks * r->H;
  double *old_h, *old_p, *mn, *ph, *hv, *pv;
  double **synth_h, **synth_p;
  uint_t b, c, k;
  uint_t end = r->N - r->H;
  old_h = (double *)calloc(r->channels * end, sizeof(double));
  old_p = (double *)calloc(r->channels * end, sizeof(double));
  synth_h = (double **)calloc(r->channels, sizeof(double *));
  synth_p = (double **)calloc(r->channels, sizeof(double *));
  mn = (double *)calloc(r->bins, sizeof(double));
  ph = (double *)calloc(r->bins, sizeof(double));
  hv = (double *)calloc(r->bins, sizeof(double));
  pv = (double *)calloc(r->bins, sizeof(double));
  for (c = 0; c < r->channels; c++) {
    out_h[c] = (double *)calloc(total, sizeof(double));
    out_p[c] = (double *)calloc(total, sizeof(double));
    synth_h[c] = (double *)calloc(r->H, sizeof(double));
    synth_p[c] = (double *)calloc(r->H, sizeof(double));
  }
  for (b = 0; b < blocks; b++) {
    if (b < tr) continue; /* leading look-ahead zeros */
    uint_t m = b - tr;
    uint_t lo = (m > tr) ? m - tr : 0;
    uint_t hi = (m + tr < r->frames) ? m + tr : r->frames - 1;
    for (k = 0; k < r->bins; k++) {
      double tv[128], fv[128];
      uint_t n = 0, j, flo, fhi;
      for (j = lo; j <= hi; j++) tv[n++] = r->mag[j][k];
      hv[k] = ref_median(tv, n);
      flo = (k > fr) ? k - fr : 0;
      fhi = (k + fr < r->bins) ? k + fr : r->bins - 1;
      n = 0;
      for (j = flo; j <= fhi; j++) fv[n++] = r->mag[m][j];
      pv[k] = ref_median(fv, n);
    }
    for (c = 0; c < r->channels; c++) {
      for (k = 0; k < r->bins; k++) {
        double h2 = hv[k] * hv[k], p2 = pv[k] * pv[k];
        double mask = (h2 == 0. && p2 == 0.) ? 0.5 : h2 / (h2 + p2);
        double nm = r->norm[m][c * r->bins + k];
        ph[k] = r->phas[m][c * r->bins + k];
        mn[k] = nm * mask;
      }
      ref_synth(r, mn, ph, old_h + c * end, synth_h[c]);
      for (k = 0; k < r->bins; k++) {
        double h2 = hv[k] * hv[k], p2 = pv[k] * pv[k];
        double mask = (h2 == 0. && p2 == 0.) ? 0.5 : h2 / (h2 + p2);
        mn[k] = r->norm[m][c * r->bins + k] * (1. - mask);
      }
      ref_synth(r, mn, ph, old_p + c * end, synth_p[c]);
      memcpy(out_h[c] + b * r->H, synth_h[c], r->H * sizeof(double));
      memcpy(out_p[c] + b * r->H, synth_p[c], r->H * sizeof(double));
    }
  }
  free(pv); free(hv); free(ph); free(mn);
  for (c = 0; c < r->channels; c++) {
    free(synth_h[c]); free(synth_p[c]);
  }
  free(synth_p); free(synth_h); free(old_p); free(old_h);
}

static void ref_del(ref_hpss_t *r)
{
  uint_t m;
  if (!r) return;
  for (m = 0; m < r->frames; m++) {
    free(r->norm[m]); free(r->phas[m]); free(r->mag[m]);
  }
  free(r->norm); free(r->phas); free(r->mag); free(r->window);
  free(r);
}

/* ------------------------------------------------------------------ */
/* signal generators and energy helpers                                */
/* ------------------------------------------------------------------ */

static double signal_tone(uint_t n)
{
  return 0.35 * sin(2. * M_PI * 7. * (double)n / 64.)
       + 0.20 * cos(2. * M_PI * 13. * (double)n / 64.);
}

static double signal_pulse(uint_t n)
{
  static const uint_t positions[] = { 3, 27, 71, 130 };
  uint_t i;
  for (i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
    if (n == positions[i]) return 0.8;
    if (n == positions[i] + 1) return -0.5;
  }
  return 0.;
}

static double rms(const smpl_t *x, uint_t n)
{
  double acc = 0.;
  uint_t i;
  for (i = 0; i < n; i++) acc += (double)x[i] * x[i];
  return sqrt(acc / (double)(n ? n : 1));
}

static double dot_norm(const smpl_t *a, const smpl_t *b, uint_t n)
{
  double da = 0., db = 0., ab = 0.;
  uint_t i;
  for (i = 0; i < n; i++) {
    da += (double)a[i] * a[i];
    db += (double)b[i] * b[i];
    ab += (double)a[i] * b[i];
  }
  return (da > 0. && db > 0.) ? ab / sqrt(da * db) : 0.;
}

/* ------------------------------------------------------------------ */
/* contract_stems: content, reconstruction, reference PCM              */
/* ------------------------------------------------------------------ */

static void stems_against_reference(uint_t channels, uint_t tr, uint_t fr,
    int pulse_signal)
{
  const uint_t N = 64, H = 16, B = 12;
  const uint_t latency = (N - H) + tr * H;
  aubio_hpss_t *o = new_aubio_hpss(N, H, channels, tr, fr);
  fmat_t *in = new_fmat(channels, H);
  fmat_t *hmat = new_fmat(channels, H);
  fmat_t *pmat = new_fmat(channels, H);
  uint_t blocks = B + tr + 3, total = blocks * H;
  smpl_t **prod_h = (smpl_t **)calloc(channels, sizeof(smpl_t *));
  smpl_t **prod_p = (smpl_t **)calloc(channels, sizeof(smpl_t *));
  double **ref_h = (double **)calloc(channels, sizeof(double *));
  double **ref_p = (double **)calloc(channels, sizeof(double *));
  fmat_t *full_in = new_fmat(channels, B * H);
  ref_hpss_t *r;
  uint_t b, c, n;
  uint_t tail_ones = 0;
  double maxerr = 0.;
  REQUIRE(o && in && hmat && pmat && prod_h && prod_p && full_in);
  CASE("stems: streaming schedule, tail drain and transform accounting");
  for (c = 0; c < channels; c++) {
    prod_h[c] = (smpl_t *)calloc(total, sizeof(smpl_t));
    prod_p[c] = (smpl_t *)calloc(total, sizeof(smpl_t));
    for (n = 0; n < B * H; n++) {
      double v;
      if (channels == 2) v = (c == 0) ? signal_tone(n) : signal_pulse(n);
      else v = pulse_signal ? signal_pulse(n) : signal_tone(n);
      full_in->data[c][n] = (smpl_t)v;
    }
  }
  task_fft_reset();
  for (b = 0; b < B; b++) {
    for (c = 0; c < channels; c++)
      for (n = 0; n < H; n++)
        in->data[c][n] = full_in->data[c][b * H + n];
    CHECK(aubio_hpss_do(o, in, hmat, pmat) == 0);
    for (c = 0; c < channels; c++) {
      memcpy(prod_h[c] + b * H, hmat->data[c], H * sizeof(smpl_t));
      memcpy(prod_p[c] + b * H, pmat->data[c], H * sizeof(smpl_t));
    }
  }
  for (b = 0; b < tr + 3; b++) {
    uint_t rc = aubio_hpss_finish(o, hmat, pmat);
    CHECK(rc == 1);
    tail_ones++;
    for (c = 0; c < channels; c++) {
      memcpy(prod_h[c] + (B + b) * H, hmat->data[c], H * sizeof(smpl_t));
      memcpy(prod_p[c] + (B + b) * H, pmat->data[c], H * sizeof(smpl_t));
    }
  }
  CHECK(tail_ones == tr + 3);
  CHECK(aubio_hpss_finish(o, hmat, pmat) == 0);
  CHECK(aubio_hpss_finish(o, hmat, pmat) == 0);
  /* exact transform accounting: B+3 analyses, two stems synthesized */
  CHECK(task_fft_forward() == (size_t)channels * (B + 3));
  CHECK(task_fft_inverse() == (size_t)2 * channels * (B + 3));

  r = ref_analyze(N, H, channels, B, full_in);
  r->tr = tr; r->fr = fr;
  ref_run(r, tr, fr, ref_h, ref_p);
  CASE("stems: production PCM matches naive DFT/IDFT reference");
  for (c = 0; c < channels; c++) {
    for (n = 0; n < total; n++) {
      double e = fabs((double)prod_h[c][n] - ref_h[c][n]);
      if (e > maxerr) maxerr = e;
      e = fabs((double)prod_p[c][n] - ref_p[c][n]);
      if (e > maxerr) maxerr = e;
    }
  }
  CHECK(maxerr < 2e-4);

  CASE("stems: exact timeline length and perfect sum reconstruction");
  CHECK(total - latency == B * H);
  for (c = 0; c < channels; c++) {
    double sumerr = 0.;
    for (n = 0; n < B * H; n++) {
      double rebuilt = (double)prod_h[c][latency + n]
        + (double)prod_p[c][latency + n];
      double e = fabs(rebuilt - (double)full_in->data[c][n]);
      if (e > sumerr) sumerr = e;
    }
    CHECK(sumerr < 2e-4);
  }

  if (channels == 2) {
    CASE("stems: stereo probe keeps channel identity and shared masks");
    CHECK(rms(prod_h[0] + latency, B * H)
        > rms(prod_p[0] + latency, B * H));
    CHECK(rms(prod_p[1] + latency, B * H)
        > rms(prod_h[1] + latency, B * H));
    /* the percussive channel must not leak into the tone channel */
    CHECK(rms(prod_p[0] + latency, B * H)
        < 0.25 * rms(prod_h[0] + latency, B * H));
  }
  for (c = 0; c < channels; c++) {
    free(prod_h[c]); free(prod_p[c]); free(ref_h[c]); free(ref_p[c]);
  }
  free(prod_h); free(prod_p); free(ref_h); free(ref_p);
  ref_del(r);
  del_fmat(full_in);
  del_fmat(hmat); del_fmat(pmat); del_fmat(in);
  del_aubio_hpss(o);
}

/* ------------------------------------------------------------------ */
/* content discrimination on a longer, bin-aligned recording           */
/* ------------------------------------------------------------------ */

static double aligned_tone(uint_t n)
{
  /* N=256 below: bins 8 and 13 are exactly periodic, no spectral leakage */
  return 0.35 * sin(2. * M_PI * 8. * (double)n / 256.)
       + 0.20 * cos(2. * M_PI * 13. * (double)n / 256.);
}

static const uint_t click_positions[] = { 7, 121, 277, 440, 600, 780 };

static double sparse_click(uint_t n)
{
  uint_t i;
  for (i = 0; i < sizeof(click_positions) / sizeof(click_positions[0]); i++)
    if (n == click_positions[i]) return 0.9;
  return 0.;
}

/** run one HPSS recording and return the assembled, latency-aligned stems */
static void stems_run(uint_t N, uint_t tr, uint_t fr, uint_t B,
    double (*generator)(uint_t), smpl_t **out_h, smpl_t **out_p)
{
  uint_t H = N / 4, blocks = B + tr + 3, total = blocks * H, b, n;
  aubio_hpss_t *o = new_aubio_hpss(N, H, 1, tr, fr);
  fmat_t *in = new_fmat(1, H), *hm = new_fmat(1, H), *pm = new_fmat(1, H);
  smpl_t *all_h = (smpl_t *)calloc(total, sizeof(smpl_t));
  smpl_t *all_p = (smpl_t *)calloc(total, sizeof(smpl_t));
  REQUIRE(o && in && hm && pm && all_h && all_p);
  for (b = 0; b < B; b++) {
    for (n = 0; n < H; n++)
      in->data[0][n] = (smpl_t)generator(b * H + n);
    CHECK(aubio_hpss_do(o, in, hm, pm) == 0);
    memcpy(all_h + b * H, hm->data[0], H * sizeof(smpl_t));
    memcpy(all_p + b * H, pm->data[0], H * sizeof(smpl_t));
  }
  for (b = 0; b < tr + 3; b++) {
    CHECK(aubio_hpss_finish(o, hm, pm) == 1);
    memcpy(all_h + (B + b) * H, hm->data[0], H * sizeof(smpl_t));
    memcpy(all_p + (B + b) * H, pm->data[0], H * sizeof(smpl_t));
  }
  CHECK(aubio_hpss_finish(o, hm, pm) == 0);
  *out_h = all_h;
  *out_p = all_p;
  del_fmat(pm); del_fmat(hm); del_fmat(in);
  del_aubio_hpss(o);
}

static void stems_content_separation(void)
{
  const uint_t N = 256, H = 64, B = 16;
  const uint_t latency_tone = (N - H) + 3 * H;
  smpl_t *th, *tp;
  uint_t n;

  CASE("stems: steady bin-aligned tones are overwhelmingly harmonic");
  stems_run(N, 3, 3, B, aligned_tone, &th, &tp);
  {
    double rh = rms(th + latency_tone, B * H);
    double rp = rms(tp + latency_tone, B * H);
    double corr = dot_norm(th + latency_tone, th + latency_tone, B * H);
    CHECK(rh > 5. * rp);
    CHECK(rh > 0.2);
    CHECK(corr > 0.99);
    /* the split is asymmetric, not a 50/50 share-out */
    CHECK(fabs(rh - rp) > 0.15);
  }
  free(th); free(tp);

  CASE("stems: sparse clicks are localized in the percussive stem");
  {
    const uint_t tr = 7, fr = 4;
    const uint_t latency = (N - H) + tr * H;
    smpl_t *ph, *pp;
    double p_near = 0., p_far = 0., h_near = 0., h_far = 0.;
    stems_run(N, tr, fr, B, sparse_click, &ph, &pp);
    for (n = 0; n < B * H; n++) {
      uint_t i, near = 0;
      for (i = 0; i < sizeof(click_positions) / sizeof(click_positions[0]);
          i++) {
        sint_t d = (sint_t)n - (sint_t)click_positions[i];
        if (d >= -8 && d <= 8) near = 1;
      }
      if (near) {
        p_near += (double)pp[latency + n] * pp[latency + n];
        h_near += (double)ph[latency + n] * ph[latency + n];
      } else {
        p_far += (double)pp[latency + n] * pp[latency + n];
        h_far += (double)ph[latency + n] * ph[latency + n];
      }
    }
    /* over 99% of percussive energy sits on the impulse neighbourhoods */
    CHECK(p_near / (p_near + p_far) > 0.99);
    /* around each click the percussive stem dominates the harmonic one */
    CHECK(p_near > 1.5 * h_near);
    /* away from clicks the harmonic stem carries the residual energy */
    CHECK(h_far > p_far);
    free(ph); free(pp);
  }

  CASE("stems: the harmonic stem carries the tonal content, not a 50/50 split");
  {
    const uint_t tr = 3, latency = (N - H) + tr * H;
    smpl_t *ah, *ap;
    fmat_t *src = new_fmat(1, B * H);
    double l1_diff = 0., l1_sum = 0.;
    uint_t n2;
    for (n2 = 0; n2 < B * H; n2++)
      src->data[0][n2] = (smpl_t)aligned_tone(n2);
    stems_run(N, tr, 3, B, aligned_tone, &ah, &ap);
    CHECK(dot_norm(ah + latency, src->data[0], B * H) > 0.99);
    CHECK(dot_norm(ap + latency, src->data[0], B * H)
        < 0.8 * dot_norm(ah + latency, src->data[0], B * H));
    for (n2 = 0; n2 < B * H; n2++) {
      l1_diff += fabs((double)ah[latency + n2] - (double)ap[latency + n2]);
      l1_sum += fabs((double)ah[latency + n2])
        + fabs((double)ap[latency + n2]);
    }
    CHECK(l1_sum > 0.);
    CHECK(l1_diff / l1_sum > 0.7);
    free(ah); free(ap);
    del_fmat(src);
  }
}

void contract_stems(void)
{
  stems_against_reference(1, 1, 1, 0);
  stems_against_reference(1, 3, 2, 0);
  stems_against_reference(1, 2, 2, 1);
  stems_against_reference(2, 2, 3, 0);
  stems_content_separation();
}

/* ------------------------------------------------------------------ */
/* contract_stream: scheduling, errors, reset, bounded updates         */
/* ------------------------------------------------------------------ */

static uint_t log2_ceil(uint_t v)
{
  uint_t l = 0;
  v--;
  while (v) { v >>= 1; l++; }
  return l ? l : 1;
}

static void stream_config(uint_t N, uint_t channels, uint_t tr, uint_t fr,
    uint_t B, int silent)
{
  uint_t H = N / 4, bins = N / 2 + 1, cap = 2 * tr + 1;
  uint_t blocks = B + tr + 3, total = blocks * H;
  uint_t latency = (N - H) + tr * H;
  aubio_hpss_t *o = new_aubio_hpss(N, H, channels, tr, fr);
  fmat_t *in = new_fmat(channels, H);
  fmat_t *hmat = new_fmat(channels, H);
  fmat_t *pmat = new_fmat(channels, H);
  fmat_t *full = new_fmat(channels, B * H ? B * H : 1);
  smpl_t **all_h = (smpl_t **)calloc(channels, sizeof(smpl_t *));
  smpl_t **all_p = (smpl_t **)calloc(channels, sizeof(smpl_t *));
  uint_t b, c, n, tail = 0;
  size_t cmp_freq_before;
  uint_t bound_time, bound_freq;
  REQUIRE(o && in && hmat && pmat && full && all_h && all_p);
  CASE("stream: do/finish schedule, latency and bounded median updates");
  CHECK(aubio_hpss_get_latency(o) == latency);
  for (c = 0; c < channels; c++) {
    all_h[c] = (smpl_t *)calloc(total, sizeof(smpl_t));
    all_p[c] = (smpl_t *)calloc(total, sizeof(smpl_t));
    for (n = 0; n < B * H; n++) {
      if (silent) full->data[c][n] = 0.f;
      else {
        full->data[c][n] = (smpl_t)(0.3
            * sin(2. * M_PI * (5. + c) * n / N)
            * (0.6 + 0.4 * sin((double)n * 0.07)));
      }
    }
  }
  task_fft_reset();
  for (b = 0; b < B; b++) {
    size_t d;
    for (c = 0; c < channels; c++)
      for (n = 0; n < H; n++)
        in->data[c][n] = full->data[c][b * H + n];
    d = aubio_hpss_test_time_comparisons(o);
    CHECK(aubio_hpss_do(o, in, hmat, pmat) == 0);
    /* bounded update: no per-center sort, comparisons stay O(log cap) */
    CHECK(aubio_hpss_test_time_comparisons(o) - d
        <= (size_t)bins * 4 * (log2_ceil(cap) + 1));
    for (c = 0; c < channels; c++) {
      memcpy(all_h[c] + b * H, hmat->data[c], H * sizeof(smpl_t));
      memcpy(all_p[c] + b * H, pmat->data[c], H * sizeof(smpl_t));
    }
    CHECK(aubio_hpss_test_ring_filled(o) <= cap);
  }
  cmp_freq_before = aubio_hpss_test_freq_comparisons(o);
  for (b = 0; b < tr + 3; b++) {
    CHECK(aubio_hpss_finish(o, hmat, pmat) == 1);
    tail++;
    for (c = 0; c < channels; c++) {
      memcpy(all_h[c] + (B + b) * H, hmat->data[c], H * sizeof(smpl_t));
      memcpy(all_p[c] + (B + b) * H, pmat->data[c], H * sizeof(smpl_t));
    }
  }
  CHECK(tail == tr + 3);
  CHECK(aubio_hpss_finish(o, hmat, pmat) == 0);
  CHECK(aubio_hpss_finish(o, hmat, pmat) == 0);
  CHECK(task_fft_forward() == (size_t)channels * (B + 3));
  CHECK(task_fft_inverse() == (size_t)2 * channels * (B + 3));

  CASE("stream: counters expose bounded insert/remove updates");
  CHECK(aubio_hpss_test_time_inserts(o) == (uint_t)bins * (B + 3));
  /* the last emitted center leaves a window clipped at both timeline ends */
  {
    uint_t final_members = cap;
    if (B + 3 < final_members) final_members = B + 3;
    if (tr + 1 < final_members) final_members = tr + 1;
    CHECK(aubio_hpss_test_time_removes(o)
        == aubio_hpss_test_time_inserts(o) - bins * final_members);
    CHECK(aubio_hpss_test_ring_filled(o) == final_members);
  }
  bound_time = bins * 4 * (log2_ceil(cap) + 1);
  bound_freq = bins * 4 * (log2_ceil(2 * fr + 1) + 1);
  CHECK(aubio_hpss_test_time_comparisons(o)
      <= (size_t)(B + 3) * bound_time);
  CHECK(aubio_hpss_test_freq_comparisons(o) - cmp_freq_before
      <= (size_t)(B + 3) * bound_freq + bound_freq);

  CASE("stream: trimmed timeline rebuilds the input sample by sample");
  CHECK(total - latency == B * H);
  for (c = 0; c < channels; c++) {
    double maxerr = 0.;
    for (n = 0; n < B * H; n++) {
      double e = fabs((double)(all_h[c][latency + n]
            + all_p[c][latency + n]) - (double)full->data[c][n]);
      if (e > maxerr) maxerr = e;
    }
    CHECK(maxerr < 2e-4);
    if (silent) CHECK(rms(all_h[c], total) == 0.);
  }
  for (c = 0; c < channels; c++) {
    free(all_h[c]); free(all_p[c]);
  }
  free(all_h); free(all_p);
  del_fmat(full);
  del_fmat(hmat); del_fmat(pmat); del_fmat(in);
  del_aubio_hpss(o);
}

static void stream_errors_and_reset(void)
{
  const uint_t N = 64, H = 16, tr = 2;
  aubio_hpss_t *o = new_aubio_hpss(N, H, 2, tr, 2);
  aubio_hpss_t *fresh = new_aubio_hpss(N, H, 2, tr, 2);
  fmat_t *in = new_fmat(2, H);
  fmat_t *bad_h = new_fmat(1, H);
  fmat_t *bad_l = new_fmat(2, H / 2);
  fmat_t *h1 = new_fmat(2, H), *p1 = new_fmat(2, H);
  fmat_t *h2 = new_fmat(2, H), *p2 = new_fmat(2, H);
  uint_t c, n, b;
  REQUIRE(o && fresh && in && bad_h && bad_l && h1 && p1 && h2 && p2);
  CASE("stream: construction rejects every invalid parameter set");
  CHECK(new_aubio_hpss(32, 8, 1, 1, 1) == NULL);
  CHECK(new_aubio_hpss(100, 25, 1, 1, 1) == NULL);
  CHECK(new_aubio_hpss(64, 8, 1, 1, 1) == NULL);
  CHECK(new_aubio_hpss(64, 16, 3, 1, 1) == NULL);
  CHECK(new_aubio_hpss(64, 16, 1, 0, 1) == NULL);
  CHECK(new_aubio_hpss(64, 16, 1, 32, 1) == NULL);
  CHECK(new_aubio_hpss(64, 16, 1, 1, 32) == NULL);

  CASE("stream: size errors do not touch history or outputs");
  for (c = 0; c < 2; c++)
    for (n = 0; n < H; n++)
      in->data[c][n] = (smpl_t)(0.25 * sin(0.3 * (c * H + n)));
  for (c = 0; c < 2; c++)
    for (n = 0; n < H; n++) {
      h1->data[c][n] = p1->data[c][n] = 1234.5f;
    }
  CHECK(aubio_hpss_do(o, in, bad_h, p1) != 0);
  CHECK(aubio_hpss_do(o, in, h1, bad_l) != 0);
  CHECK(h1->data[0][0] == 1234.5f && p1->data[1][H - 1] == 1234.5f);
  /* the failed calls left no trace: good call matches a fresh object */
  CHECK(aubio_hpss_do(o, in, h1, p1) == 0);
  CHECK(aubio_hpss_do(fresh, in, h2, p2) == 0);
  for (c = 0; c < 2; c++)
    for (n = 0; n < H; n++) {
      CHECK(h1->data[c][n] == h2->data[c][n]);
      CHECK(p1->data[c][n] == p2->data[c][n]);
    }

  CASE("stream: finish size error returns 2 without sealing");
  CHECK(aubio_hpss_finish(o, bad_h, p1) == 2);
  CHECK(aubio_hpss_do(o, in, h1, p1) == 0); /* still accepts input */

  CASE("stream: do after a real finish is rejected, then reset restores");
  CHECK(aubio_hpss_finish(o, h1, p1) == 1);
  CHECK(aubio_hpss_do(o, in, h1, p1) != 0);
  aubio_hpss_reset(o);
  aubio_hpss_reset(fresh);
  for (b = 0; b < 6; b++) {
    CHECK(aubio_hpss_do(o, in, h1, p1) == 0);
    CHECK(aubio_hpss_do(fresh, in, h2, p2) == 0);
    for (c = 0; c < 2; c++)
      for (n = 0; n < H; n++) {
        CHECK(h1->data[c][n] == h2->data[c][n]);
        CHECK(p1->data[c][n] == p2->data[c][n]);
      }
  }
  del_fmat(h2); del_fmat(p2); del_fmat(h1); del_fmat(p1);
  del_fmat(bad_l); del_fmat(bad_h); del_fmat(in);
  del_aubio_hpss(fresh);
  del_aubio_hpss(o);
}

static void stream_zero_input(void)
{
  const uint_t N = 64, H = 16, tr = 3;
  aubio_hpss_t *o = new_aubio_hpss(N, H, 1, tr, 2);
  fmat_t *h = new_fmat(1, H), *p = new_fmat(1, H);
  uint_t b, ones = 0;
  REQUIRE(o && h && p);
  task_fft_reset();
  CASE("stream: finish with no input hop still yields tr+3 zero blocks");
  for (b = 0; b < tr + 3; b++) {
    CHECK(aubio_hpss_finish(o, h, p) == 1);
    ones++;
    for (uint_t n = 0; n < H; n++) {
      CHECK(h->data[0][n] == 0.f);
      CHECK(p->data[0][n] == 0.f);
    }
  }
  CHECK(ones == tr + 3);
  CHECK(aubio_hpss_finish(o, h, p) == 0);
  CHECK(task_fft_forward() == (size_t)3);
  CHECK(task_fft_inverse() == (size_t)2 * 3);
  del_fmat(h); del_fmat(p);
  del_aubio_hpss(o);
}

void contract_stream(void)
{
  /* channels 1/2, radii 1/3/31, short recordings, silence, wraparound */
  stream_config(64, 1, 1, 1, 0, 0);
  stream_config(64, 1, 1, 1, 1, 0);
  stream_config(64, 2, 3, 2, 2, 0);
  stream_config(128, 1, 3, 3, 5, 0);
  stream_config(64, 2, 31, 31, 40, 0);
  stream_config(64, 1, 31, 1, 140, 0); /* ring wraps many times */
  stream_config(256, 2, 1, 31, 30, 1); /* long silence, larger window */
  stream_errors_and_reset();
  stream_zero_input();
}
