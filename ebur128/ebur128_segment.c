/* See COPYING file for copyright and license details. */

/* Concatenatable loudness summaries.
 *
 * The BS.1770 K-weighting filter is the cascade of two biquads: a shelving
 * pre-filter (pb/pa) followed by a high-pass (rb/ra).  Using the transposed
 * direct-form II state w = [d1_0, d1_1, d2_0, d2_1], one sample is
 *
 *   u0 = x      - pa1*w0 - pa2*w1
 *   u  = pb0*u0 + pb1*w0 + pb2*w1
 *   t0 = u      - ra1*w2 - ra2*w3
 *   y  = rb0*t0 + rb1*w2 + rb2*w3
 *   w' = [u0, w0, t0, w2]
 *
 * hence w' = A w + e1*x with a 4x4 matrix A and, on zero input, the output
 * row y = c*w.  This cascade realization is numerically well conditioned:
 * the zero-input output rows c*A^k stay bounded, unlike the single 5-tap
 * controllable form whose internal states transiently grow by orders of
 * magnitude.
 *
 * For every 100 ms block and every used channel the creation pass stores only
 * fixed-size quantities:
 *   z[4]  filter state after the block when the section starts from a zero
 *         filter history,
 *   r[4]  cross-correlation of the block's zero-history filtered samples with
 *         the zero-input impulse response rows c*A^k,
 *   ysq   sum of the squared zero-history filtered samples.
 * Together with the per-rate tables
 *   Am = A^m, the zero-input state transition over one m-frame block,
 *   Q  = sum over the block of (c*A^k)^T * (c*A^k),
 * the energy of any block in an arbitrarily concatenated sequence is
 *
 *   E(w) = (weight / m) * (ysq + 2 * r . w + w^T Q w),
 *
 * and the state handed to the next block is z + Am*w, where w is the state
 * carried into the block.  No original sample is ever stored or replayed:
 * concatenation copies block records and queries cost O(blocks * channels).
 *
 * The tables are immutable per sample rate and are copied into every object,
 * so construction and read-only queries share no mutable state and are safe
 * to run concurrently on different summaries.
 */

#include "ebur128_segment.h"
#include "ebur128_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Dimension of the biquad-cascade state. */
#define SEGMENT_STATE_DIM 4
/* z[4] + r[4] + ysq */
#define SEGMENT_RECORD_FIELDS (2 * SEGMENT_STATE_DIM + 1)

/* Immutable per-sample-rate linear-algebra tables. */
typedef struct {
  /** State transition matrix A for one sample, row major 4x4. */
  double A[16];
  /** A^block_frames, row major 4x4. */
  double Am[16];
  /** Zero-input output row c. */
  double c[4];
  /** Sum of (c A^k)^T (c A^k) over the block, symmetric 4x4. */
  double Q[16];
} segment_tables;

struct ebur128_segment {
  unsigned int channels;
  unsigned long samplerate;
  /** Frames in one 100 ms block: (samplerate + 5) / 10. */
  size_t block_frames;
  /** Total number of represented PCM frames. */
  size_t frames;
  /** Number of complete 100 ms blocks. */
  size_t blocks;
  /** Own copy of the channel map, channels entries. */
  int* channel_map;
  /** BS.1770 channel weight per channel. */
  double* weight;
  /** blocks * channels * SEGMENT_RECORD_FIELDS doubles, block major. */
  double* records;
  /** Heap bytes requested for everything owned by this object. */
  size_t bytes;
  /* K-weighting biquad coefficients. */
  double pb[3], pa[3], rb[3], ra[3];
  /* Per-rate tables copied into this object (no shared mutable cache). */
  segment_tables tables;
};

/* Per-sample filter entry instrumentation: lets tests prove that concat and
 * the queries never advance the sample-by-sample recursion. */
#if defined(__GNUC__) || defined(__clang__)
static __thread size_t filter_step_calls;
#else
static size_t filter_step_calls;
#endif

void ebur128_priv_reset_filter_steps(void) {
  filter_step_calls = 0;
}

size_t ebur128_priv_filter_steps(void) {
  return filter_step_calls;
}

static void mat4_vec(const double M[16],
                     const double in[SEGMENT_STATE_DIM],
                     double out[SEGMENT_STATE_DIM]) {
  int i, j;
  for (i = 0; i < SEGMENT_STATE_DIM; ++i) {
    double acc = 0.0;
    for (j = 0; j < SEGMENT_STATE_DIM; ++j) {
      acc += M[i * SEGMENT_STATE_DIM + j] * in[j];
    }
    out[i] = acc;
  }
}

/* Build A^m by iterating the actual sample recursion once per basis vector.
 * The matrix is non-normal, so repeated squaring would cancel large
 * intermediates against each other; the sequential path follows the same
 * arithmetic as the per-sample filter. */
static void power_m(const double A[16], size_t m, double Am[16]) {
  int col, row;
  memset(Am, 0, 16 * sizeof(double));
  for (col = 0; col < SEGMENT_STATE_DIM; ++col) {
    double w[SEGMENT_STATE_DIM] = { 0.0, 0.0, 0.0, 0.0 };
    size_t k;
    w[col] = 1.0;
    for (k = 0; k < m; ++k) {
      double next[SEGMENT_STATE_DIM];
      mat4_vec(A, w, next);
      for (row = 0; row < SEGMENT_STATE_DIM; ++row) {
        w[row] = next[row];
      }
    }
    for (row = 0; row < SEGMENT_STATE_DIM; ++row) {
      Am[row * SEGMENT_STATE_DIM + col] = w[row];
    }
  }
}

/* Compute the immutable tables and biquad coefficients for one sample rate. */
static void build_tables(unsigned long samplerate,
                         double pb[3],
                         double pa[3],
                         double rb[3],
                         double ra[3],
                         segment_tables* tables) {
  size_t block_frames = (size_t) ((samplerate + 5) / 10);
  double g1, g2;
  double ck[SEGMENT_STATE_DIM];
  size_t k, i, j;

  ebur128_priv_biquad_coeffs(samplerate, pb, pa, rb, ra);

  /* u  = pb0*x + (pb1 - pb0 pa1) d1_0 + (pb2 - pb0 pa2) d1_1. */
  g1 = pb[1] - pb[0] * pa[1];
  g2 = pb[2] - pb[0] * pa[2];

  memset(tables, 0, sizeof(*tables));
  /* row 0: new d1_0 = u0 = x - pa1 d1_0 - pa2 d1_1 */
  tables->A[0 * 4 + 0] = -pa[1];
  tables->A[0 * 4 + 1] = -pa[2];
  /* row 1: new d1_1 = old d1_0 */
  tables->A[1 * 4 + 0] = 1.0;
  /* row 2: new d2_0 = t0 = u - ra1 d2_0 - ra2 d2_1 */
  tables->A[2 * 4 + 0] = g1;
  tables->A[2 * 4 + 1] = g2;
  tables->A[2 * 4 + 2] = -ra[1];
  tables->A[2 * 4 + 3] = -ra[2];
  /* row 3: new d2_1 = old d2_0 */
  tables->A[3 * 4 + 2] = 1.0;

  power_m(tables->A, block_frames, tables->Am);

  /* Zero-input output row: y = t0 + rb1 d2_0 + rb2 d2_1, and on zero input
   * t0 = g1 d1_0 + g2 d1_1 - ra1 d2_0 - ra2 d2_1. */
  tables->c[0] = g1;
  tables->c[1] = g2;
  tables->c[2] = rb[1] - ra[1];
  tables->c[3] = rb[2] - ra[2];

  memcpy(ck, tables->c, sizeof(ck));
  for (k = 0; k < block_frames; ++k) {
    for (i = 0; i < SEGMENT_STATE_DIM; ++i) {
      for (j = 0; j < SEGMENT_STATE_DIM; ++j) {
        tables->Q[i * 4 + j] += ck[i] * ck[j];
      }
    }
    /* ck <- ck A */
    double next[SEGMENT_STATE_DIM];
    double tmp[SEGMENT_STATE_DIM];
    for (i = 0; i < SEGMENT_STATE_DIM; ++i) {
      double acc = 0.0;
      for (j = 0; j < SEGMENT_STATE_DIM; ++j) {
        acc += ck[j] * tables->A[j * 4 + i];
      }
      tmp[i] = acc;
    }
    memcpy(next, tmp, sizeof(next));
    memcpy(ck, next, sizeof(ck));
  }
}

/** Advance the biquad-cascade K-weighting recursion by one sample; returns
 *  the output.  w holds [d1_0, d1_1, d2_0, d2_1]. */
static double filter_step(double w[SEGMENT_STATE_DIM],
                          double x,
                          const ebur128_segment* s) {
  double u0 = x - s->pa[1] * w[0] - s->pa[2] * w[1];
  double u = s->pb[0] * u0 + s->pb[1] * w[0] + s->pb[2] * w[1];
  double t0 = u - s->ra[1] * w[2] - s->ra[2] * w[3];
  double y = s->rb[0] * t0 + s->rb[1] * w[2] + s->rb[2] * w[3];
  w[3] = w[2];
  w[2] = t0;
  w[1] = w[0];
  w[0] = u0;
  ++filter_step_calls;
  return y;
}

static int valid_channel_value(int value, unsigned int channels) {
  switch (value) {
    case EBUR128_LEFT:
    case EBUR128_RIGHT:
    case EBUR128_CENTER:
    case EBUR128_LEFT_SURROUND:
    case EBUR128_RIGHT_SURROUND:
    case EBUR128_UNUSED:
      return 1;
    case EBUR128_DUAL_MONO:
      return channels == 1;
    default:
      return 0;
  }
}

static int mul_overflow(size_t nmemb, size_t size, size_t* result) {
  if (nmemb != 0 && ((size_t) -1) / nmemb < size) {
    return 1;
  }
  *result = nmemb * size;
  return 0;
}

ebur128_segment*
ebur128_segment_create(unsigned int channels,
                       unsigned long samplerate,
                       const int* channel_map,
                       const double* pcm,
                       size_t frames) {
  ebur128_segment* s;
  size_t block_frames;
  size_t blocks;
  size_t sample_count;
  size_t i;
  unsigned int c;
  size_t bytes;
  size_t map_bytes;
  size_t weight_bytes;
  size_t record_bytes;

  if (channels < 1 || channels > 6) {
    return NULL;
  }
  if (samplerate != 44100ul && samplerate != 48000ul) {
    return NULL;
  }
  if (!channel_map) {
    return NULL;
  }
  block_frames = (size_t) ((samplerate + 5) / 10);
  if (frames % block_frames != 0) {
    return NULL;
  }
  if (frames != 0 && !pcm) {
    return NULL;
  }
  for (c = 0; c < channels; ++c) {
    if (!valid_channel_value(channel_map[c], channels)) {
      return NULL;
    }
  }
  if (mul_overflow(frames, (size_t) channels, &sample_count)) {
    return NULL;
  }
  for (i = 0; i < sample_count; ++i) {
    if (!isfinite(pcm[i])) {
      return NULL;
    }
  }

  blocks = frames / block_frames;
  if (mul_overflow((size_t) channels, sizeof(int), &map_bytes) ||
      mul_overflow((size_t) channels, sizeof(double), &weight_bytes) ||
      mul_overflow(blocks * (size_t) channels,
                   SEGMENT_RECORD_FIELDS * sizeof(double), &record_bytes)) {
    return NULL;
  }
  bytes = sizeof(*s) + map_bytes + weight_bytes + record_bytes;

  s = (ebur128_segment*) malloc(sizeof(*s));
  if (!s) {
    return NULL;
  }
  memset(s, 0, sizeof(*s));
  s->channel_map = (int*) malloc(map_bytes);
  s->weight = (double*) malloc(weight_bytes);
  s->records = record_bytes ? (double*) malloc(record_bytes) : NULL;
  if (!s->channel_map || !s->weight || (record_bytes && !s->records)) {
    free(s->records);
    free(s->weight);
    free(s->channel_map);
    free(s);
    return NULL;
  }

  ebur128_priv_ensure_gating_constants();

  s->channels = channels;
  s->samplerate = samplerate;
  s->block_frames = block_frames;
  s->frames = frames;
  s->blocks = blocks;
  s->bytes = bytes;
  memcpy(s->channel_map, channel_map, map_bytes);
  build_tables(samplerate, s->pb, s->pa, s->rb, s->ra, &s->tables);
  for (c = 0; c < channels; ++c) {
    s->weight[c] = ebur128_priv_channel_weight(channel_map[c]);
  }
  if (record_bytes) {
    memset(s->records, 0, record_bytes);
  }

  for (c = 0; c < channels; ++c) {
    size_t block;
    if (channel_map[c] == EBUR128_UNUSED) {
      continue; /* matches ebur128_filter_double, which skips UNUSED channels */
    }
    for (block = 0; block < blocks; ++block) {
      double w[SEGMENT_STATE_DIM] = { 0.0, 0.0, 0.0, 0.0 };
      double* rec =
          s->records + (block * (size_t) channels + c) * SEGMENT_RECORD_FIELDS;
      double* z = rec;
      double* r = rec + SEGMENT_STATE_DIM;
      double* ysq = rec + 2 * SEGMENT_STATE_DIM;
      double ck[SEGMENT_STATE_DIM];
      double rr[SEGMENT_STATE_DIM] = { 0.0, 0.0, 0.0, 0.0 };
      double sum_sq = 0.0;
      size_t k;
      memcpy(ck, s->tables.c, sizeof(ck));
      for (k = 0; k < block_frames; ++k) {
        double x = pcm[(block * block_frames + k) * (size_t) channels + c];
        double y = filter_step(w, x, s);
        size_t q;
        sum_sq += y * y;
        for (q = 0; q < SEGMENT_STATE_DIM; ++q) {
          rr[q] += y * ck[q];
        }
        /* ck <- ck A */
        double next[SEGMENT_STATE_DIM];
        double tmp[SEGMENT_STATE_DIM];
        for (q = 0; q < SEGMENT_STATE_DIM; ++q) {
          double acc = 0.0;
          size_t p;
          for (p = 0; p < SEGMENT_STATE_DIM; ++p) {
            acc += ck[p] * s->tables.A[p * 4 + q];
          }
          tmp[q] = acc;
        }
        memcpy(next, tmp, sizeof(next));
        memcpy(ck, next, sizeof(ck));
      }
      memcpy(z, w, sizeof(w));
      memcpy(r, rr, sizeof(rr));
      *ysq = sum_sq;
    }
  }

  return s;
}

int ebur128_segment_concat(const ebur128_segment* a,
                           const ebur128_segment* b,
                           ebur128_segment** out) {
  ebur128_segment* s;
  size_t map_bytes, weight_bytes, record_bytes, bytes;
  size_t blocks;

  if (!a || !b || !out) {
    return EBUR128_ERROR_INVALID_ARGUMENT;
  }
  if (a->samplerate != b->samplerate || a->channels != b->channels ||
      memcmp(a->channel_map, b->channel_map,
             (size_t) a->channels * sizeof(int)) != 0) {
    return EBUR128_ERROR_INVALID_ARGUMENT;
  }

  blocks = a->blocks + b->blocks;
  map_bytes = (size_t) a->channels * sizeof(int);
  weight_bytes = (size_t) a->channels * sizeof(double);
  if (mul_overflow(blocks * (size_t) a->channels,
                   SEGMENT_RECORD_FIELDS * sizeof(double), &record_bytes)) {
    return EBUR128_ERROR_NOMEM;
  }
  bytes = sizeof(*s) + map_bytes + weight_bytes + record_bytes;
  /* Guard the frame counter as well. */
  if (b->frames > (size_t) -1 - a->frames) {
    return EBUR128_ERROR_NOMEM;
  }

  s = (ebur128_segment*) malloc(sizeof(*s));
  if (!s) {
    return EBUR128_ERROR_NOMEM;
  }
  memset(s, 0, sizeof(*s));
  s->channel_map = (int*) malloc(map_bytes);
  s->weight = (double*) malloc(weight_bytes);
  s->records = record_bytes ? (double*) malloc(record_bytes) : NULL;
  if (!s->channel_map || !s->weight || (record_bytes && !s->records)) {
    free(s->records);
    free(s->weight);
    free(s->channel_map);
    free(s);
    return EBUR128_ERROR_NOMEM;
  }

  s->channels = a->channels;
  s->samplerate = a->samplerate;
  s->block_frames = a->block_frames;
  s->frames = a->frames + b->frames;
  s->blocks = blocks;
  s->bytes = bytes;
  memcpy(s->pb, a->pb, sizeof(s->pb));
  memcpy(s->pa, a->pa, sizeof(s->pa));
  memcpy(s->rb, a->rb, sizeof(s->rb));
  memcpy(s->ra, a->ra, sizeof(s->ra));
  memcpy(&s->tables, &a->tables, sizeof(s->tables));
  memcpy(s->channel_map, a->channel_map, map_bytes);
  memcpy(s->weight, a->weight, weight_bytes);
  if (a->records) {
    memcpy(s->records, a->records,
           a->blocks * (size_t) a->channels * SEGMENT_RECORD_FIELDS *
               sizeof(double));
  }
  if (b->records) {
    memcpy(s->records +
               a->blocks * (size_t) a->channels * SEGMENT_RECORD_FIELDS,
           b->records,
           b->blocks * (size_t) b->channels * SEGMENT_RECORD_FIELDS *
               sizeof(double));
  }

  *out = s;
  return EBUR128_SUCCESS;
}

void ebur128_segment_destroy(ebur128_segment** p) {
  ebur128_segment* s;
  if (!p || !*p) {
    return;
  }
  s = *p;
  free(s->records);
  free(s->weight);
  free(s->channel_map);
  free(s);
  *p = NULL;
}

size_t
ebur128_segment_storage_bytes(const ebur128_segment* s) {
  if (!s) {
    return 0;
  }
  return s->bytes;
}

/** Reconstruct the 100 ms block energies of the whole concatenated sequence
 *  as evaluated from zero filter history.  Grows only with block count and
 *  channel count. */
static int reconstruct_block_energies(const ebur128_segment* s,
                                      double** energies_out) {
  double* energies;
  double* state;
  size_t k, c;

  energies = (double*) malloc((s->blocks ? s->blocks : 1) * sizeof(double));
  state = (double*) calloc((size_t) s->channels,
                           SEGMENT_STATE_DIM * sizeof(double));
  if (!energies || !state) {
    free(energies);
    free(state);
    return EBUR128_ERROR_NOMEM;
  }

  for (k = 0; k < s->blocks; ++k) {
    double block_energy = 0.0;
    for (c = 0; c < s->channels; ++c) {
      const double* rec =
          s->records + (k * (size_t) s->channels + c) * SEGMENT_RECORD_FIELDS;
      const double* z = rec;
      const double* r = rec + SEGMENT_STATE_DIM;
      double ysq = rec[2 * SEGMENT_STATE_DIM];
      double* w = state + c * SEGMENT_STATE_DIM;
      double hw[SEGMENT_STATE_DIM];
      double next_w[SEGMENT_STATE_DIM];
      double usq = 0.0;
      double cross = 0.0;
      size_t i, j;

      if (s->channel_map[c] == EBUR128_UNUSED) {
        continue;
      }

      mat4_vec(s->tables.Am, w, hw);
      for (i = 0; i < SEGMENT_STATE_DIM; ++i) {
        double acc = 0.0;
        for (j = 0; j < SEGMENT_STATE_DIM; ++j) {
          acc += w[i] * s->tables.Q[i * 4 + j] * w[j];
        }
        usq += acc;
        cross += r[i] * w[i];
        next_w[i] = z[i] + hw[i];
      }
      block_energy += s->weight[c] / (double) s->block_frames *
                      (ysq + 2.0 * cross + usq);
      memcpy(w, next_w, sizeof(next_w));
    }
    energies[k] = block_energy;
  }

  free(state);
  *energies_out = energies;
  return EBUR128_SUCCESS;
}

static int double_cmp(const void* p1, const void* p2) {
  const double* d1 = (const double*) p1;
  const double* d2 = (const double*) p2;
  return (*d1 > *d2) - (*d1 < *d2);
}

/** Integrated loudness, mirroring ebur128_gated_loudness() for mode I. */
static double integrated_from_blocks(const double* energies, size_t blocks) {
  double* kept;
  double sum = 0.0;
  double relative_threshold;
  double gated_sum = 0.0;
  size_t kept_count = 0;
  size_t gated_count = 0;
  size_t k;
  double result;

  if (blocks < 4) {
    return -HUGE_VAL;
  }
  kept = (double*) malloc((blocks - 3) * sizeof(double));
  if (!kept) {
    return -HUGE_VAL;
  }
  /* 400 ms windows: one at every 100 ms hop, each the mean of four
   * consecutive 100 ms block energies. */
  for (k = 3; k < blocks; ++k) {
    double window_energy =
        (energies[k - 3] + energies[k - 2] + energies[k - 1] + energies[k]) /
        4.0;
    if (window_energy >= ebur128_priv_min_gating_energy()) {
      kept[kept_count++] = window_energy;
    }
  }
  if (!kept_count) {
    free(kept);
    return -HUGE_VAL;
  }

  for (k = 0; k < kept_count; ++k) {
    sum += kept[k];
  }
  relative_threshold =
      sum / (double) kept_count * ebur128_priv_relative_gate_factor();
  for (k = 0; k < kept_count; ++k) {
    if (kept[k] >= relative_threshold) {
      gated_sum += kept[k];
      ++gated_count;
    }
  }
  free(kept);
  if (!gated_count) {
    return -HUGE_VAL;
  }
  result = ebur128_priv_energy_to_loudness(gated_sum / (double) gated_count);
  return result;
}

/** Loudness range, mirroring the non-histogram path of
 * ebur128_loudness_range_multiple(). */
static double lra_from_blocks(const double* energies, size_t blocks) {
  double* windows;
  size_t window_count = 0;
  size_t k;
  double stl_power;
  double stl_integrated;
  double* relgated;
  size_t relgated_size;
  double h_en, l_en;

  /* First complete 3 s window ends on block 29; afterwards one window every
   * ten blocks. */
  if (blocks < 30) {
    return 0.0;
  }
  windows = (double*) malloc((1 + (blocks - 30) / 10) * sizeof(double));
  if (!windows) {
    return 0.0;
  }
  for (k = 29; k < blocks; k += 10) {
    double sum = 0.0;
    size_t j;
    for (j = k - 29; j <= k; ++j) {
      sum += energies[j];
    }
    sum /= 30.0;
    if (sum >= ebur128_priv_min_gating_energy()) {
      windows[window_count++] = sum;
    }
  }
  if (!window_count) {
    free(windows);
    return 0.0;
  }

  qsort(windows, window_count, sizeof(double), double_cmp);
  stl_power = 0.0;
  for (k = 0; k < window_count; ++k) {
    stl_power += windows[k];
  }
  stl_power /= (double) window_count;
  stl_integrated = ebur128_priv_minus_twenty_decibels() * stl_power;

  relgated = windows;
  relgated_size = window_count;
  while (relgated_size > 0 && *relgated < stl_integrated) {
    ++relgated;
    --relgated_size;
  }
  if (!relgated_size) {
    free(windows);
    return 0.0;
  }

  h_en = relgated[(size_t) ((relgated_size - 1) * 0.95 + 0.5)];
  l_en = relgated[(size_t) ((relgated_size - 1) * 0.1 + 0.5)];
  free(windows);
  return ebur128_priv_energy_to_loudness(h_en) -
         ebur128_priv_energy_to_loudness(l_en);
}

int ebur128_segment_measure(const ebur128_segment* s,
                            ebur128_segment_result* out) {
  double* energies;
  int error;

  if (!s || !out) {
    return EBUR128_ERROR_INVALID_ARGUMENT;
  }

  error = reconstruct_block_energies(s, &energies);
  if (error) {
    return error;
  }

  out->frames = s->frames;
  out->energy_blocks = s->blocks;
  out->integrated_windows = s->blocks >= 4 ? s->blocks - 3 : 0;
  out->shortterm_windows =
      s->blocks >= 30 ? 1 + (s->blocks - 30) / 10 : 0;
  out->integrated_lufs = integrated_from_blocks(energies, s->blocks);
  out->lra_lu = lra_from_blocks(energies, s->blocks);

  free(energies);
  return EBUR128_SUCCESS;
}

int ebur128_segment_block_energy(const ebur128_segment* s,
                                 size_t index,
                                 double* out) {
  double* energies;
  int error;

  if (!s || !out) {
    return EBUR128_ERROR_INVALID_ARGUMENT;
  }
  if (index >= s->blocks) {
    return EBUR128_ERROR_INVALID_ARGUMENT;
  }

  error = reconstruct_block_energies(s, &energies);
  if (error) {
    return error;
  }
  *out = energies[index];
  free(energies);
  return EBUR128_SUCCESS;
}
