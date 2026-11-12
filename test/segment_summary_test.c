/* Unit tests for the concatenatable loudness summaries.
 *
 * This file is included textually by checks/runner.c and must not define
 * main().  Expected block energies come from a test-only direct K-weighting
 * recursion written below, never from the segment API. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ebur128.h"
#include "ebur128_internal.h"
#include "ebur128_segment.h"
#include "suite.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* Allocation ledger: interposes the process malloc/calloc/realloc.   */
/* ------------------------------------------------------------------ */

static unsigned char test_arena[1u << 26];
static size_t test_bump;
static size_t test_live_bytes;
static size_t test_total_allocs;

#define TEST_LEDGER_MAX 16384

struct test_hdr {
  size_t size;
  int used;
};

static struct test_hdr* test_slots[TEST_LEDGER_MAX];
static size_t test_slot_count;

void* malloc(size_t n) {
  size_t i;
  struct test_hdr* h;
  /* first-fit reuse of a previously freed block of exactly this size */
  for (i = 0; i < test_slot_count; ++i) {
    if (!test_slots[i]->used && test_slots[i]->size == n) {
      test_slots[i]->used = 1;
      test_live_bytes += n;
      ++test_total_allocs;
      return (unsigned char*) test_slots[i] + sizeof(struct test_hdr);
    }
  }
  if (test_slot_count == TEST_LEDGER_MAX ||
      test_bump + sizeof(struct test_hdr) + n > sizeof(test_arena)) {
    fprintf(stderr, "test arena exhausted\n");
    abort();
  }
  h = (struct test_hdr*) (test_arena + test_bump);
  test_bump += sizeof(struct test_hdr) + n;
  h->size = n;
  h->used = 1;
  test_slots[test_slot_count++] = h;
  test_live_bytes += n;
  ++test_total_allocs;
  return (unsigned char*) h + sizeof(struct test_hdr);
}

void* calloc(size_t nm, size_t n) {
  size_t total = nm * n;
  void* p = malloc(total);
  memset(p, 0, total);
  return p;
}

void free(void* p) {
  struct test_hdr* h;
  if (!p) {
    return;
  }
  if ((unsigned char*) p < test_arena ||
      (unsigned char*) p >= test_arena + sizeof(test_arena)) {
    return;
  }
  h = (struct test_hdr*) ((unsigned char*) p - sizeof(struct test_hdr));
  if (!h->used) {
    return;
  }
  h->used = 0;
  test_live_bytes -= h->size;
}

void* realloc(void* p, size_t n) {
  struct test_hdr* h;
  size_t old, copy;
  void* q;
  if (!p) {
    return malloc(n);
  }
  h = (struct test_hdr*) ((unsigned char*) p - sizeof(struct test_hdr));
  old = h->size;
  q = malloc(n);
  copy = old < n ? old : n;
  memcpy(q, p, copy);
  free(p);
  return q;
}

static size_t ledger_live(void) {
  return test_live_bytes;
}

/* The test's own signal buffers are much larger than every library
 * allocation and must not perturb the library ledger.  They come from a
 * plain static bump pool (standard C only, no mmap); the pointer resets once
 * every buffer of the previous test has been released. */
#define PCM_POOL_SIZE (64u * 1024u * 1024u)
static unsigned char pcm_pool[PCM_POOL_SIZE];
static size_t pcm_bump;
static size_t pcm_outstanding;

static void* pcm_alloc(size_t bytes) {
  size_t off = (pcm_bump + 15u) & ~(size_t) 15;
  if (off + bytes > sizeof(pcm_pool)) {
    fprintf(stderr, "pcm pool exhausted\n");
    abort();
  }
  pcm_bump = off + bytes;
  ++pcm_outstanding;
  return pcm_pool + off;
}

static void pcm_free(void* p, size_t bytes) {
  (void) p;
  (void) bytes;
  if (pcm_outstanding == 0) {
    fprintf(stderr, "pcm pool underflow\n");
    abort();
  }
  if (--pcm_outstanding == 0) {
    pcm_bump = 0;
  }
}

/* ------------------------------------------------------------------ */
/* Deterministic signal generation.                                    */
/* ------------------------------------------------------------------ */

static unsigned long t_rng;

static double trand(void) {
  t_rng = t_rng * 1103515245ul + 12345ul;
  return ((double) (t_rng & 0xfffffful) / (double) (1ul << 24)) * 2.0 - 1.0;
}

/* pattern 0: loud tone, then a weak tone, then silence (strong history
 * followed by material the filter transient must not corrupt).
 * pattern 1: DC bias plus sustained quiet tone (baseline.sh style).
 * pattern 2: alternating loud and quiet three-second passages (real LRA).
 * pattern 3: one loud stretch inside otherwise near-gate quiet material. */
static void gen_signal(double* pcm,
                       size_t frames,
                       unsigned int channels,
                       unsigned long rate,
                       const int* map,
                       int pattern,
                       unsigned long seed) {
  size_t i;
  unsigned int c;
  t_rng = seed;
  for (i = 0; i < frames; ++i) {
    double t = (double) i / (double) rate;
    double base;
    double amp;
    switch (pattern) {
      case 0:
        if (t < (double) frames / (double) rate * 0.4) {
          amp = 0.5;
        } else if (t < (double) frames / (double) rate * 0.7) {
          amp = 0.003;
        } else {
          amp = 0.0;
        }
        base = amp * sin(2.0 * M_PI * 440.0 * t);
        break;
      case 1:
        base = 0.2 + 0.05 * sin(2.0 * M_PI * 997.0 * t);
        break;
      case 2:
        amp = (((size_t) (t / 3.0) % 2u) == 0) ? 0.4 : 0.01;
        base = amp * (0.7 * sin(2.0 * M_PI * 311.0 * t) + 0.3 * trand());
        break;
      default:
        amp = (t > 2.0 && t < 5.0) ? 0.35 : 0.00005;
        base = amp * (0.8 * sin(2.0 * M_PI * 622.0 * t) + 0.2 * trand());
        break;
    }
    for (c = 0; c < channels; ++c) {
      double gain;
      switch (map[c]) {
        case EBUR128_CENTER: gain = 0.8; break;
        case EBUR128_LEFT_SURROUND:
        case EBUR128_RIGHT_SURROUND: gain = 0.6; break;
        case EBUR128_UNUSED: gain = 0.0; break;
        default: gain = 1.0; break;
      }
      pcm[i * channels + c] = gain * base + 0.05 * gain * trand();
    }
  }
}

/* ------------------------------------------------------------------ */
/* Test-only direct K-weighting recursion and independent gating.      */
/* ------------------------------------------------------------------ */

/* Exact mirror of the library's transposed direct form II loop, driven from
 * the public coefficients.  Independent of the segment SOS realization. */
typedef struct {
  double v[5];
} direct_filter;

static double direct_step(direct_filter* f,
                          double x,
                          const double b[5],
                          const double a[5]) {
  double v0 = x - a[1] * f->v[1] - a[2] * f->v[2] - a[3] * f->v[3] -
              a[4] * f->v[4];
  double y = b[0] * v0 + b[1] * f->v[1] + b[2] * f->v[2] +
             b[3] * f->v[3] + b[4] * f->v[4];
  f->v[4] = f->v[3];
  f->v[3] = f->v[2];
  f->v[2] = f->v[1];
  f->v[1] = v0;
  f->v[0] = v0;
  return y;
}

/* Per-block energies of the whole sequence measured from zero history. */
static void direct_block_energies(const double* pcm,
                                  size_t frames,
                                  unsigned int channels,
                                  const int* map,
                                  unsigned long rate,
                                  double* out) {
  size_t hop = (size_t) ((rate + 5) / 10);
  size_t blocks = frames / hop;
  double b[5], a[5];
  direct_filter* filters;
  size_t k, c, n;
  ebur128_priv_filter_coeffs(rate, b, a);
  filters = (direct_filter*) calloc(channels, sizeof(*filters));
  for (k = 0; k < blocks; ++k) {
    double energy = 0.0;
    for (c = 0; c < channels; ++c) {
      double csum = 0.0;
      if (map[c] == EBUR128_UNUSED) {
        continue;
      }
      for (n = 0; n < hop; ++n) {
        double x = pcm[((k * hop + n) * channels + c)];
        double y = direct_step(&filters[c], x, b, a);
        csum += y * y;
      }
      energy += ebur128_priv_channel_weight(map[c]) * csum;
    }
    out[k] = energy / (double) hop;
  }
  free(filters);
}

/* Independent integrated loudness straight from the block energies,
 * following BS.1770 gating with the library's constants. */
static double direct_integrated(const double* e, size_t blocks) {
  double* windows;
  size_t n = 0, k, kept = 0;
  double sum = 0.0, threshold, gated = 0.0;
  if (blocks < 4) {
    return -HUGE_VAL;
  }
  windows = (double*) malloc((blocks - 3) * sizeof(double));
  for (k = 3; k < blocks; ++k) {
    double w = (e[k - 3] + e[k - 2] + e[k - 1] + e[k]) / 4.0;
    if (w >= ebur128_priv_min_gating_energy()) {
      windows[n++] = w;
    }
  }
  if (!n) {
    free(windows);
    return -HUGE_VAL;
  }
  for (k = 0; k < n; ++k) {
    sum += windows[k];
  }
  threshold = sum / (double) n * ebur128_priv_relative_gate_factor();
  for (k = 0; k < n; ++k) {
    if (windows[k] >= threshold) {
      gated += windows[k];
      ++kept;
    }
  }
  free(windows);
  if (!kept) {
    return -HUGE_VAL;
  }
  return ebur128_priv_energy_to_loudness(gated / (double) kept);
}

static int dcmp(const void* p1, const void* p2) {
  double x = *(const double*) p1;
  double y = *(const double*) p2;
  return (x > y) - (x < y);
}

static double direct_lra(const double* e, size_t blocks) {
  double* windows;
  size_t n = 0, k;
  double power, gate, *p;
  size_t pn;
  if (blocks < 30) {
    return 0.0;
  }
  windows = (double*) malloc((1 + (blocks - 30) / 10) * sizeof(double));
  for (k = 29; k < blocks; k += 10) {
    double sum = 0.0;
    size_t j;
    for (j = k - 29; j <= k; ++j) {
      sum += e[j];
    }
    sum /= 30.0;
    if (sum >= ebur128_priv_min_gating_energy()) {
      windows[n++] = sum;
    }
  }
  if (!n) {
    free(windows);
    return 0.0;
  }
  qsort(windows, n, sizeof(double), dcmp);
  power = 0.0;
  for (k = 0; k < n; ++k) {
    power += windows[k];
  }
  power /= (double) n;
  gate = ebur128_priv_minus_twenty_decibels() * power;
  p = windows;
  pn = n;
  while (pn > 0 && *p < gate) {
    ++p;
    --pn;
  }
  if (!pn) {
    free(windows);
    return 0.0;
  }
  {
    double hi = p[(size_t) ((pn - 1) * 0.95 + 0.5)];
    double lo = p[(size_t) ((pn - 1) * 0.1 + 0.5)];
    free(windows);
    return ebur128_priv_energy_to_loudness(hi) -
           ebur128_priv_energy_to_loudness(lo);
  }
}

/* Continuous reference through the unchanged streaming API. */
static void reference_measure(const double* pcm,
                              size_t frames,
                              unsigned int channels,
                              const int* map,
                              unsigned long rate,
                              double* ref_i,
                              double* ref_lra) {
  ebur128_state* st =
      ebur128_init(channels, rate, EBUR128_MODE_I | EBUR128_MODE_LRA);
  unsigned int c;
  UNIT_CHECK(st != NULL);
  for (c = 0; c < channels; ++c) {
    UNIT_CHECK(ebur128_set_channel(st, c, map[c]) == EBUR128_SUCCESS);
  }
  UNIT_CHECK(ebur128_add_frames_double(st, pcm, frames) == EBUR128_SUCCESS);
  UNIT_CHECK(ebur128_loudness_global(st, ref_i) == EBUR128_SUCCESS);
  UNIT_CHECK(ebur128_loudness_range(st, ref_lra) == EBUR128_SUCCESS);
  ebur128_destroy(&st);
}

/* Build summaries for the block ranges in cuts[0..ncuts] and concatenate
 * them in order.  Every range is a whole number of 100 ms blocks. */
static ebur128_segment* build_chain(const double* pcm,
                                    unsigned int channels,
                                    unsigned long rate,
                                    const int* map,
                                    const size_t* cuts,
                                    size_t ncuts,
                                    ebur128_segment** parts) {
  size_t hop = (size_t) ((rate + 5) / 10);
  ebur128_segment* acc = NULL;
  size_t i;
  for (i = 0; i < ncuts; ++i) {
    size_t f0 = cuts[i] * hop;
    size_t f1 = cuts[i + 1] * hop;
    ebur128_segment* part =
        ebur128_segment_create(channels, rate, map, pcm + f0 * channels,
                               f1 - f0);
    ebur128_segment* next;
    UNIT_CHECK(part != NULL);
    parts[i] = part;
    if (!acc) {
      acc = part;
    } else {
      UNIT_CHECK(ebur128_segment_concat(acc, part, &next) == EBUR128_SUCCESS);
      acc = next;
    }
  }
  return acc;
}

static double worst_energy_diff;

static void check_block_energies(const ebur128_segment* s,
                                 const double* expected,
                                 size_t blocks) {
  size_t k;
  for (k = 0; k < blocks; ++k) {
    double got;
    double diff;
    UNIT_CHECK(ebur128_segment_block_energy(s, k, &got) == EBUR128_SUCCESS);
    diff = fabs(got - expected[k]);
    if (diff > worst_energy_diff) {
      worst_energy_diff = diff;
    }
    UNIT_CHECK(diff <= 1e-10 + 1e-5 * fabs(expected[k]));
  }
}

static const unsigned long test_rates[2] = { 44100ul, 48000ul };

/* ================================================================== */
/* filter group                                                        */
/* ================================================================== */

UNIT_TEST(filter, history_into_weak_and_silence) {
  size_t ri;
  worst_energy_diff = 0.0;
  for (ri = 0; ri < 2; ++ri) {
    unsigned long rate = test_rates[ri];
    size_t hop = (size_t) ((rate + 5) / 10);
    size_t blocks = 60; /* 6 seconds */
    size_t frames = blocks * hop;
    int map[2] = { EBUR128_LEFT, EBUR128_RIGHT };
    double* pcm = (double*) pcm_alloc(frames * 2 * sizeof(double));
    double* expected = (double*) malloc(blocks * sizeof(double));
    /* cuts include single-block sections and long sections, and split the
     * weak/silence passages in the middle. */
    size_t cuts[7] = { 0, 1, 5, 24, 25, 42, 60 };
    ebur128_segment* parts[6];
    ebur128_segment* chain;
    ebur128_segment_result res;
    size_t i;
    gen_signal(pcm, frames, 2, rate, map, 0, 100 + ri);
    direct_block_energies(pcm, frames, 2, map, rate, expected);
    chain = build_chain(pcm, 2, rate, map, cuts, 6, parts);
    UNIT_CHECK(chain != NULL);

    check_block_energies(chain, expected, blocks);

    /* Weak blocks after the loud passage must match too: history carried. */
    for (i = 25; i < blocks; ++i) {
      double got;
      UNIT_CHECK(ebur128_segment_block_energy(chain, i, &got) ==
                 EBUR128_SUCCESS);
      UNIT_CHECK(fabs(got - expected[i]) <= 1e-10 + 1e-5 * expected[i]);
    }

    UNIT_CHECK(ebur128_segment_measure(chain, &res) == EBUR128_SUCCESS);
    UNIT_CHECK(res.energy_blocks == blocks);
    UNIT_CHECK(res.integrated_windows == blocks - 3);
    UNIT_CHECK(res.shortterm_windows == 1 + (blocks - 30) / 10);
    UNIT_CHECK(isfinite(res.integrated_lufs));

    for (i = 0; i < 6; ++i) {
      ebur128_segment_destroy(&parts[i]);
    }
    ebur128_segment_destroy(&chain);
    free(expected);
    pcm_free(pcm, frames * 2 * sizeof(double));
  }
  printf("filter worst block energy diff so far: %.3e\n", worst_energy_diff);
  UNIT_CHECK(worst_energy_diff < 1e-9);
}

UNIT_TEST(filter, dc_bias_matches_continuous) {
  size_t ri;
  for (ri = 0; ri < 2; ++ri) {
    unsigned long rate = test_rates[ri];
    size_t hop = (size_t) ((rate + 5) / 10);
    size_t blocks = 41;
    size_t frames = blocks * hop;
    int map[1] = { EBUR128_LEFT };
    double* pcm = (double*) pcm_alloc(frames * sizeof(double));
    double* expected = (double*) malloc(blocks * sizeof(double));
    size_t cuts[5] = { 0, 1, 2, 30, 41 }; /* one-block pieces then a long one */
    ebur128_segment* parts[4];
    ebur128_segment* chain;
    ebur128_segment_result res;
    double ref_i, ref_lra;
    gen_signal(pcm, frames, 1, rate, map, 1, 7 + ri);
    direct_block_energies(pcm, frames, 1, map, rate, expected);
    chain = build_chain(pcm, 1, rate, map, cuts, 4, parts);
    check_block_energies(chain, expected, blocks);
    reference_measure(pcm, frames, 1, map, rate, &ref_i, &ref_lra);
    UNIT_CHECK(ebur128_segment_measure(chain, &res) == EBUR128_SUCCESS);
    UNIT_CHECK(isfinite(res.integrated_lufs));
    UNIT_CHECK(fabs(res.integrated_lufs - ref_i) <= 1e-4);
    UNIT_CHECK(fabs(res.lra_lu - ref_lra) <= 1e-4);
    UNIT_CHECK(res.frames == frames);
    {
      size_t i;
      for (i = 0; i < 4; ++i) {
        ebur128_segment_destroy(&parts[i]);
      }
    }
    ebur128_segment_destroy(&chain);
    free(expected);
    pcm_free(pcm, frames * sizeof(double));
  }
  UNIT_CHECK(worst_energy_diff < 1e-9);
}

UNIT_TEST(filter, pure_silence_and_dual_mono) {
  unsigned long rate = 48000ul;
  size_t hop = (size_t) ((rate + 5) / 10);
  size_t blocks = 12;
  size_t frames = blocks * hop;
  int map[1] = { EBUR128_DUAL_MONO };
  double* pcm = (double*) pcm_alloc(frames * sizeof(double));
  memset(pcm, 0, frames * sizeof(double));
  double* expected = (double*) calloc(blocks, sizeof(double));
  ebur128_segment* a;
  ebur128_segment* b;
  ebur128_segment* both;
  ebur128_segment_result res;
  size_t k;
  direct_block_energies(pcm, frames, 1, map, rate, expected);
  a = ebur128_segment_create(1, rate, map, pcm, frames);
  b = ebur128_segment_create(1, rate, map, pcm, hop);
  UNIT_CHECK(a != NULL && b != NULL);
  UNIT_CHECK(ebur128_segment_concat(a, b, &both) == EBUR128_SUCCESS);
  for (k = 0; k < blocks + 1; ++k) {
    double got = 1.0;
    UNIT_CHECK(ebur128_segment_block_energy(both, k, &got) ==
               EBUR128_SUCCESS);
    UNIT_CHECK(fabs(got) <= 1e-10);
  }
  UNIT_CHECK(ebur128_segment_measure(both, &res) == EBUR128_SUCCESS);
  UNIT_CHECK(isinf(res.integrated_lufs) && res.integrated_lufs < 0.0);
  UNIT_CHECK(res.lra_lu == 0.0);
  UNIT_CHECK(res.energy_blocks == blocks + 1);
  ebur128_segment_destroy(&both);
  ebur128_segment_destroy(&a);
  ebur128_segment_destroy(&b);
  free(expected);
  pcm_free(pcm, frames * sizeof(double));
}

/* ================================================================== */
/* gating group                                                        */
/* ================================================================== */

UNIT_TEST(gating, cuts_cross_window_boundaries_surround) {
  size_t ri;
  for (ri = 0; ri < 2; ++ri) {
    unsigned long rate = test_rates[ri];
    size_t hop = (size_t) ((rate + 5) / 10);
    size_t blocks = 233; /* 23.3 s: many 400 ms and 3 s windows */
    size_t frames = blocks * hop;
    unsigned int ch = 6;
    int map[6] = { EBUR128_LEFT, EBUR128_RIGHT, EBUR128_CENTER,
                   EBUR128_UNUSED, EBUR128_LEFT_SURROUND,
                   EBUR128_RIGHT_SURROUND };
    double* pcm = (double*) pcm_alloc(frames * ch * sizeof(double));
    double* expected = (double*) malloc(blocks * sizeof(double));
    /* cuts deliberately misaligned with both the 4-block and 30-block
     * window phases. */
    size_t cuts[8] = { 0, 3, 7, 29, 31, 100, 177, 233 };
    ebur128_segment* parts[7];
    ebur128_segment* chain;
    ebur128_segment_result res;
    double ref_i, ref_lra;
    double direct_i, direct_l;
    gen_signal(pcm, frames, ch, rate, map, 2, 55 + ri);
    direct_block_energies(pcm, frames, ch, map, rate, expected);
    chain = build_chain(pcm, ch, rate, map, cuts, 7, parts);
    check_block_energies(chain, expected, blocks);
    reference_measure(pcm, frames, ch, map, rate, &ref_i, &ref_lra);
    direct_i = direct_integrated(expected, blocks);
    direct_l = direct_lra(expected, blocks);
    UNIT_CHECK(ebur128_segment_measure(chain, &res) == EBUR128_SUCCESS);
    UNIT_CHECK(res.energy_blocks == blocks);
    UNIT_CHECK(res.integrated_windows == blocks - 3);
    UNIT_CHECK(res.shortterm_windows == 1 + (blocks - 30) / 10);
    UNIT_CHECK(isfinite(res.integrated_lufs) && isfinite(ref_i));
    UNIT_CHECK(fabs(res.integrated_lufs - ref_i) <= 1e-4);
    UNIT_CHECK(fabs(res.integrated_lufs - direct_i) <= 1e-4);
    UNIT_CHECK(fabs(res.lra_lu - ref_lra) <= 1e-4);
    UNIT_CHECK(fabs(res.lra_lu - direct_l) <= 1e-4);
    printf("gating rate=%lu windows i=%zu st=%zu lra=%.6f ref=%.6f\n",
           rate, res.integrated_windows, res.shortterm_windows, res.lra_lu,
           ref_lra);
    {
      size_t i;
      for (i = 0; i < 7; ++i) {
        ebur128_segment_destroy(&parts[i]);
      }
    }
    ebur128_segment_destroy(&chain);
    free(expected);
    pcm_free(pcm, frames * ch * sizeof(double));
  }
  UNIT_CHECK(worst_energy_diff < 1e-9);
}

UNIT_TEST(gating, below_gate_material_is_empty) {
  unsigned long rate = 44100ul;
  size_t hop = (size_t) ((rate + 5) / 10);
  size_t blocks = 50;
  size_t frames = blocks * hop;
  int map[2] = { EBUR128_LEFT, EBUR128_RIGHT };
  double* pcm = (double*) pcm_alloc(frames * 2 * sizeof(double));
  double* expected = (double*) malloc(blocks * sizeof(double));
  size_t cuts[4] = { 0, 1, 25, 50 };
  ebur128_segment* parts[3];
  ebur128_segment* chain;
  ebur128_segment_result res;
  double ref_i, ref_lra;
  gen_signal(pcm, frames, 2, rate, map, 3, 99);
  /* scale everything far below the -70 LUFS absolute gate */
  {
    size_t i;
    for (i = 0; i < frames * 2; ++i) {
      pcm[i] *= 1e-7;
    }
  }
  direct_block_energies(pcm, frames, 2, map, rate, expected);
  chain = build_chain(pcm, 2, rate, map, cuts, 3, parts);
  check_block_energies(chain, expected, blocks);
  reference_measure(pcm, frames, 2, map, rate, &ref_i, &ref_lra);
  UNIT_CHECK(ebur128_segment_measure(chain, &res) == EBUR128_SUCCESS);
  UNIT_CHECK(isinf(res.integrated_lufs) && res.integrated_lufs < 0.0);
  UNIT_CHECK(isinf(ref_i) && ref_i < 0.0);
  UNIT_CHECK(res.lra_lu == 0.0 && ref_lra == 0.0);
  UNIT_CHECK(res.integrated_windows == blocks - 3);
  UNIT_CHECK(res.shortterm_windows == 1 + (blocks - 30) / 10);
  {
    size_t i;
    for (i = 0; i < 3; ++i) {
      ebur128_segment_destroy(&parts[i]);
    }
  }
  ebur128_segment_destroy(&chain);
  free(expected);
  pcm_free(pcm, frames * 2 * sizeof(double));
}

UNIT_TEST(gating, threshold_inclusion_direction) {
  unsigned long rate = 48000ul;
  size_t hop = (size_t) ((rate + 5) / 10);
  size_t blocks = 80;
  size_t frames = blocks * hop;
  int map[1] = { EBUR128_LEFT };
  double* pcm = (double*) pcm_alloc(frames * sizeof(double));
  memset(pcm, 0, frames * sizeof(double));
  double* expected = (double*) malloc(blocks * sizeof(double));
  ebur128_segment* part1;
  ebur128_segment* part2;
  ebur128_segment* chain;
  ebur128_segment_result res;
  double ref_i, ref_lra;
  size_t k;
  size_t above = 0;
  size_t below = 0;
  /* Sine whose amplitude switches once: a long quiet stretch below the
   * absolute gate followed by a loud stretch above it.  Windows fully inside
   * either stretch classify cleanly; the split point itself is unimportant,
   * the inclusion set below is derived from the independent block energies. */
  for (k = 0; k < frames; ++k) {
    double t = (double) k / (double) rate;
    double amp = (k / hop) < blocks / 2 ? 0.00015 : 0.001;
    pcm[k] = amp * sin(2.0 * M_PI * 1000.0 * t);
  }
  direct_block_energies(pcm, frames, 1, map, rate, expected);
  /* independently classify the 400 ms windows against the absolute gate */
  for (k = 3; k < blocks; ++k) {
    double w = (expected[k - 3] + expected[k - 2] + expected[k - 1] +
                expected[k]) /
               4.0;
    if (w >= ebur128_priv_min_gating_energy()) {
      ++above;
    } else {
      ++below;
    }
  }
  UNIT_CHECK(above > 0 && below > 0); /* genuinely straddling the gate */

  part1 = ebur128_segment_create(1, rate, map, pcm, frames / 2);
  part2 = ebur128_segment_create(1, rate, map,
                                 pcm + frames / 2, frames - frames / 2);
  UNIT_CHECK(part1 != NULL && part2 != NULL);
  UNIT_CHECK(ebur128_segment_concat(part1, part2, &chain) == EBUR128_SUCCESS);
  reference_measure(pcm, frames, 1, map, rate, &ref_i, &ref_lra);
  UNIT_CHECK(ebur128_segment_measure(chain, &res) == EBUR128_SUCCESS);
  /* Both implementations must make the same gating calls, hence match far
   * inside the 1e-4 LU tolerance despite the borderline material. */
  UNIT_CHECK(isfinite(res.integrated_lufs) == isfinite(ref_i));
  if (isfinite(ref_i)) {
    UNIT_CHECK(fabs(res.integrated_lufs - ref_i) <= 1e-4);
  }
  UNIT_CHECK(fabs(res.lra_lu - ref_lra) <= 1e-4);
  UNIT_CHECK(res.integrated_windows == blocks - 3);
  ebur128_segment_destroy(&chain);
  ebur128_segment_destroy(&part1);
  ebur128_segment_destroy(&part2);
  free(expected);
  pcm_free(pcm, frames * sizeof(double));
}

/* ================================================================== */
/* composition group                                                   */
/* ================================================================== */

UNIT_TEST(composition, parenthesization_and_reuse) {
  unsigned long rate = 48000ul;
  size_t hop = (size_t) ((rate + 5) / 10);
  size_t blocks = 90;
  size_t frames = blocks * hop;
  int map[2] = { EBUR128_LEFT, EBUR128_RIGHT };
  double* pcm = (double*) pcm_alloc(frames * 2 * sizeof(double));
  double* expected = (double*) malloc(blocks * sizeof(double));
  ebur128_segment *a, *b, *c, *ab, *bc, *left, *right, *self;
  ebur128_segment_result r1, r2, r3;
  gen_signal(pcm, frames, 2, rate, map, 2, 31);
  direct_block_energies(pcm, frames, 2, map, rate, expected);
  a = ebur128_segment_create(2, rate, map, pcm, 30 * hop);
  b = ebur128_segment_create(2, rate, map, pcm + 30 * hop * 2, 30 * hop);
  c = ebur128_segment_create(2, rate, map, pcm + 60 * hop * 2, 30 * hop);
  UNIT_CHECK(a && b && c);

  UNIT_CHECK(ebur128_segment_concat(a, b, &ab) == EBUR128_SUCCESS);
  UNIT_CHECK(ebur128_segment_concat(ab, c, &left) == EBUR128_SUCCESS);
  UNIT_CHECK(ebur128_segment_concat(b, c, &bc) == EBUR128_SUCCESS);
  UNIT_CHECK(ebur128_segment_concat(a, bc, &right) == EBUR128_SUCCESS);

  UNIT_CHECK(ebur128_segment_measure(left, &r1) == EBUR128_SUCCESS);
  UNIT_CHECK(ebur128_segment_measure(right, &r2) == EBUR128_SUCCESS);
  UNIT_CHECK(r1.energy_blocks == r2.energy_blocks);
  UNIT_CHECK(r1.integrated_windows == r2.integrated_windows);
  UNIT_CHECK(r1.shortterm_windows == r2.shortterm_windows);
  UNIT_CHECK(fabs(r1.integrated_lufs - r2.integrated_lufs) == 0.0);
  UNIT_CHECK(fabs(r1.lra_lu - r2.lra_lu) == 0.0);
  check_block_energies(left, expected, blocks);
  check_block_energies(right, expected, blocks);

  /* reuse: a concatenated with itself doubles the programme.  The two halves
   * are not block-identical because the second carries a's ending filter
   * state, so compare against a direct recursion over the doubled PCM. */
  {
    double* doubled = (double*) pcm_alloc(60 * hop * 2 * sizeof(double));
    double* expected2 = (double*) malloc(60 * sizeof(double));
    memcpy(doubled, pcm, 30 * hop * 2 * sizeof(double));
    memcpy(doubled + 30 * hop * 2, pcm, 30 * hop * 2 * sizeof(double));
    direct_block_energies(doubled, 60 * hop, 2, map, rate, expected2);
    UNIT_CHECK(ebur128_segment_concat(a, a, &self) == EBUR128_SUCCESS);
    UNIT_CHECK(ebur128_segment_measure(self, &r3) == EBUR128_SUCCESS);
    UNIT_CHECK(r3.energy_blocks == 60);
    UNIT_CHECK(r3.frames == 60 * hop);
    check_block_energies(self, expected2, 60);
    pcm_free(doubled, 60 * hop * 2 * sizeof(double));
    free(expected2);
  }
  /* inputs of concat stay untouched */
  {
    double ea;
    UNIT_CHECK(ebur128_segment_block_energy(a, 0, &ea) == EBUR128_SUCCESS);
    UNIT_CHECK(fabs(ea - expected[0]) <= 1e-10 + 1e-5 * expected[0]);
  }

  ebur128_segment_destroy(&self);
  ebur128_segment_destroy(&left);
  ebur128_segment_destroy(&right);
  ebur128_segment_destroy(&ab);
  ebur128_segment_destroy(&bc);
  ebur128_segment_destroy(&a);
  ebur128_segment_destroy(&b);
  ebur128_segment_destroy(&c);
  free(expected);
  pcm_free(pcm, frames * 2 * sizeof(double));
}

UNIT_TEST(composition, empty_identity_and_released_inputs) {
  unsigned long rate = 44100ul;
  size_t hop = (size_t) ((rate + 5) / 10);
  size_t blocks = 20;
  size_t frames = blocks * hop;
  int map[3] = { EBUR128_LEFT, EBUR128_RIGHT, EBUR128_CENTER };
  double* pcm1 = (double*) pcm_alloc(frames * 3 * sizeof(double));
  double* pcm2 = (double*) pcm_alloc(hop * 3 * sizeof(double));
  int map_copy[3];
  ebur128_segment *empty, *s1, *s2, *e1, *e2, *chain;
  ebur128_segment_result res;
  gen_signal(pcm1, frames, 3, rate, map, 0, 44);
  gen_signal(pcm2, hop, 3, rate, map, 1, 45);
  memcpy(map_copy, map, sizeof(map_copy));

  empty = ebur128_segment_create(3, rate, map_copy, NULL, 0);
  UNIT_CHECK(empty != NULL);
  s1 = ebur128_segment_create(3, rate, map_copy, pcm1, frames);
  s2 = ebur128_segment_create(3, rate, map_copy, pcm2, hop);
  UNIT_CHECK(s1 != NULL && s2 != NULL);

  /* overwrite and release every input buffer right after creation */
  memset(pcm1, 0xAB, frames * 3 * sizeof(double));
  memset(pcm2, 0xCD, hop * 3 * sizeof(double));
  memset(map_copy, 0, sizeof(map_copy));

  UNIT_CHECK(ebur128_segment_concat(empty, s1, &e1) == EBUR128_SUCCESS);
  UNIT_CHECK(ebur128_segment_concat(s1, empty, &e2) == EBUR128_SUCCESS);
  UNIT_CHECK(ebur128_segment_concat(e1, s2, &chain) == EBUR128_SUCCESS);

  /* regenerate the expectation from fresh equivalent PCM path: the first
   * blocks equal the original signal, last block the second signal; check
   * structural properties and internal consistency instead. */
  UNIT_CHECK(ebur128_segment_measure(chain, &res) == EBUR128_SUCCESS);
  UNIT_CHECK(res.energy_blocks == blocks + 1);
  UNIT_CHECK(res.frames == frames + hop);
  UNIT_CHECK(res.integrated_windows == blocks + 1 - 3);
  {
    size_t k;
    double x, y;
    for (k = 0; k < blocks; ++k) {
      UNIT_CHECK(ebur128_segment_block_energy(chain, k, &x) ==
                 EBUR128_SUCCESS);
      UNIT_CHECK(ebur128_segment_block_energy(e1, k, &y) ==
                 EBUR128_SUCCESS);
      UNIT_CHECK(fabs(x - y) <= 1e-12);
    }
    UNIT_CHECK(ebur128_segment_block_energy(e2, blocks - 1, &x) ==
               EBUR128_SUCCESS);
    UNIT_CHECK(ebur128_segment_block_energy(s1, blocks - 1, &y) ==
               EBUR128_SUCCESS);
    UNIT_CHECK(fabs(x - y) <= 1e-12);
  }
  /* empty + empty stays the identity element */
  {
    ebur128_segment* ee;
    ebur128_segment_result er;
    UNIT_CHECK(ebur128_segment_concat(empty, empty, &ee) == EBUR128_SUCCESS);
    UNIT_CHECK(ebur128_segment_measure(ee, &er) == EBUR128_SUCCESS);
    UNIT_CHECK(er.frames == 0 && er.energy_blocks == 0);
    UNIT_CHECK(isinf(er.integrated_lufs) && er.integrated_lufs < 0.0);
    UNIT_CHECK(er.lra_lu == 0.0);
    UNIT_CHECK(ebur128_segment_storage_bytes(ee) ==
               ebur128_segment_storage_bytes(empty));
    ebur128_segment_destroy(&ee);
  }
  ebur128_segment_destroy(&chain);
  ebur128_segment_destroy(&e1);
  ebur128_segment_destroy(&e2);
  ebur128_segment_destroy(&s1);
  ebur128_segment_destroy(&s2);
  ebur128_segment_destroy(&empty);
  pcm_free(pcm1, frames * 3 * sizeof(double));
  pcm_free(pcm2, hop * 3 * sizeof(double));
}

UNIT_TEST(composition, storage_ledger_and_cap) {
  size_t ri;
  for (ri = 0; ri < 2; ++ri) {
    unsigned long rate = test_rates[ri];
    size_t hop = (size_t) ((rate + 5) / 10);
    unsigned int ch = 5;
    int map[5] = { EBUR128_LEFT, EBUR128_RIGHT, EBUR128_CENTER,
                   EBUR128_LEFT_SURROUND, EBUR128_RIGHT_SURROUND };
    size_t blocks = (ri == 0) ? 1u : 123u; /* short and long sections */
    size_t frames = blocks * hop;
    double* pcm = (double*) pcm_alloc((frames ? frames : 1) * ch *
                                      sizeof(double));
    ebur128_segment* s;
    ebur128_segment* cat;
    size_t before, after, cap;
    gen_signal(pcm, frames, ch, rate, map, 0, 70 + ri);

    before = ledger_live();
    {
      size_t allocs_before = test_total_allocs;
      s = ebur128_segment_create(ch, rate, map, pcm, frames);
      /* the library allocations must actually flow through the ledger */
      UNIT_CHECK(test_total_allocs > allocs_before);
    }
    after = ledger_live();
    UNIT_CHECK(s != NULL);
    UNIT_CHECK(after - before == ebur128_segment_storage_bytes(s));
    cap = 2048u * (size_t) ch * (1u + blocks);
    UNIT_CHECK(ebur128_segment_storage_bytes(s) <= cap);

    before = ledger_live();
    UNIT_CHECK(ebur128_segment_concat(s, s, &cat) == EBUR128_SUCCESS);
    after = ledger_live();
    UNIT_CHECK(after - before == ebur128_segment_storage_bytes(cat));
    UNIT_CHECK(ebur128_segment_storage_bytes(cat) <=
               2048u * (size_t) ch * (1u + 2 * blocks));
    UNIT_CHECK(ebur128_segment_storage_bytes(cat) >
               ebur128_segment_storage_bytes(s));

    {
      size_t catbytes = ebur128_segment_storage_bytes(cat);
      before = ledger_live();
      ebur128_segment_destroy(&cat);
      /* destruction must release exactly the accounted bytes */
      UNIT_CHECK(ledger_live() + catbytes == before);
      UNIT_CHECK(cat == NULL);
    }
    ebur128_segment_destroy(&s);
    pcm_free(pcm, (frames ? frames : 1) * ch * sizeof(double));
  }
  /* empty object is counted too */
  {
    int map[1] = { EBUR128_LEFT };
    size_t before = ledger_live();
    ebur128_segment* e =
        ebur128_segment_create(1, 48000ul, map, NULL, 0);
    UNIT_CHECK(e != NULL);
    UNIT_CHECK(ledger_live() - before ==
               ebur128_segment_storage_bytes(e));
    UNIT_CHECK(ebur128_segment_storage_bytes(e) > 0);
    UNIT_CHECK(ebur128_segment_storage_bytes(e) <= 2048u);
    ebur128_segment_destroy(&e);
    UNIT_CHECK(ledger_live() == before);
  }
}

UNIT_TEST(composition, no_sample_replay_after_creation) {
  unsigned long rate = 48000ul;
  size_t hop = (size_t) ((rate + 5) / 10);
  unsigned int ch = 4;
  int map[4] = { EBUR128_LEFT, EBUR128_RIGHT, EBUR128_UNUSED,
                 EBUR128_CENTER };
  size_t blocks = 40;
  size_t frames = blocks * hop;
  double* pcm = (double*) pcm_alloc(frames * ch * sizeof(double));
  ebur128_segment *a, *b, *cat;
  ebur128_segment_result res;
  double e;
  size_t used_channels = 3;
  size_t expected_steps;
  gen_signal(pcm, frames, ch, rate, map, 2, 81);

  ebur128_priv_reset_filter_steps();
  a = ebur128_segment_create(ch, rate, map, pcm, frames / 2);
  expected_steps = (frames / 2) * used_channels;
  UNIT_CHECK(ebur128_priv_filter_steps() == expected_steps);

  b = ebur128_segment_create(ch, rate, map, pcm + frames / 2 * ch,
                             frames - frames / 2);
  expected_steps += (frames - frames / 2) * used_channels;
  UNIT_CHECK(ebur128_priv_filter_steps() == expected_steps);

  ebur128_priv_reset_filter_steps();
  UNIT_CHECK(ebur128_segment_concat(a, b, &cat) == EBUR128_SUCCESS);
  UNIT_CHECK(ebur128_priv_filter_steps() == 0);
  UNIT_CHECK(ebur128_segment_measure(cat, &res) == EBUR128_SUCCESS);
  UNIT_CHECK(ebur128_priv_filter_steps() == 0);
  UNIT_CHECK(ebur128_segment_block_energy(cat, blocks - 1, &e) ==
             EBUR128_SUCCESS);
  UNIT_CHECK(ebur128_priv_filter_steps() == 0);
  /* repeated queries stay sample-free */
  UNIT_CHECK(ebur128_segment_measure(cat, &res) == EBUR128_SUCCESS);
  UNIT_CHECK(ebur128_priv_filter_steps() == 0);

  ebur128_segment_destroy(&cat);
  ebur128_segment_destroy(&a);
  ebur128_segment_destroy(&b);
  pcm_free(pcm, frames * ch * sizeof(double));
}

UNIT_TEST(composition, argument_rejection) {
  unsigned long rate = 48000ul;
  size_t hop = (size_t) ((rate + 5) / 10);
  int map[2] = { EBUR128_LEFT, EBUR128_RIGHT };
  int mono_map[1] = { EBUR128_DUAL_MONO };
  double* pcm = (double*) pcm_alloc(10 * hop * 2 * sizeof(double));
  double* bad_inf = (double*) pcm_alloc(hop * 2 * sizeof(double));
  double* bad_nan = (double*) pcm_alloc(hop * 2 * sizeof(double));
  ebur128_segment* s;
  ebur128_segment* other;
  ebur128_segment* out = (ebur128_segment*) 0x1;
  ebur128_segment_result res;
  double energy = -1.0;
  gen_signal(pcm, 10 * hop, 2, rate, map, 0, 5);
  memset(bad_inf, 0, hop * 2 * sizeof(double));
  memset(bad_nan, 0, hop * 2 * sizeof(double));
  bad_inf[hop] = (double) INFINITY;
  bad_nan[hop] = (double) NAN;

  UNIT_CHECK(ebur128_segment_create(0, rate, map, pcm, hop) == NULL);
  UNIT_CHECK(ebur128_segment_create(7, rate, map, pcm, hop) == NULL);
  UNIT_CHECK(ebur128_segment_create(2, 44111ul, map, pcm, hop) == NULL);
  UNIT_CHECK(ebur128_segment_create(2, rate, NULL, pcm, hop) == NULL);
  UNIT_CHECK(ebur128_segment_create(2, rate, map, pcm, hop + 1) == NULL);
  UNIT_CHECK(ebur128_segment_create(2, rate, map, NULL, hop) == NULL);
  UNIT_CHECK(ebur128_segment_create(2, rate, map, bad_inf, hop) == NULL);
  UNIT_CHECK(ebur128_segment_create(2, rate, map, bad_nan, hop) == NULL);
  {
    int bad_map[2] = { EBUR128_LEFT, 999 };
    UNIT_CHECK(ebur128_segment_create(2, rate, bad_map, pcm, hop) == NULL);
  }
  {
    int bad_dual[2] = { EBUR128_DUAL_MONO, EBUR128_RIGHT };
    UNIT_CHECK(ebur128_segment_create(2, rate, bad_dual, pcm, hop) == NULL);
  }
  UNIT_CHECK(ebur128_segment_create(1, rate, mono_map, pcm, hop) != NULL);

  s = ebur128_segment_create(2, rate, map, pcm, 5 * hop);
  UNIT_CHECK(s != NULL);
  /* a valid 44100 Hz section whose frame count is a multiple of its own
   * 100 ms block size (4410). */
  other = ebur128_segment_create(2, 44100ul, map, pcm, 4410ul);
  UNIT_CHECK(other != NULL);

  UNIT_CHECK(ebur128_segment_concat(NULL, s, &out) ==
             EBUR128_ERROR_INVALID_ARGUMENT);
  UNIT_CHECK(ebur128_segment_concat(s, NULL, &out) ==
             EBUR128_ERROR_INVALID_ARGUMENT);
  UNIT_CHECK(ebur128_segment_concat(s, s, NULL) ==
             EBUR128_ERROR_INVALID_ARGUMENT);
  UNIT_CHECK(out == (ebur128_segment*) 0x1); /* untouched on failure */
  UNIT_CHECK(ebur128_segment_concat(s, other, &out) ==
             EBUR128_ERROR_INVALID_ARGUMENT);
  {
    int other_map[2] = { EBUR128_LEFT, EBUR128_CENTER };
    ebur128_segment* diff =
        ebur128_segment_create(2, rate, other_map, pcm, 5 * hop);
    UNIT_CHECK(diff != NULL);
    UNIT_CHECK(ebur128_segment_concat(s, diff, &out) ==
               EBUR128_ERROR_INVALID_ARGUMENT);
    ebur128_segment_destroy(&diff);
  }
  {
    ebur128_segment* mono =
        ebur128_segment_create(1, rate, mono_map, pcm, 5 * hop);
    UNIT_CHECK(mono != NULL);
    UNIT_CHECK(ebur128_segment_concat(s, mono, &out) ==
               EBUR128_ERROR_INVALID_ARGUMENT);
    ebur128_segment_destroy(&mono);
  }

  UNIT_CHECK(ebur128_segment_measure(NULL, &res) ==
             EBUR128_ERROR_INVALID_ARGUMENT);
  UNIT_CHECK(ebur128_segment_measure(s, NULL) ==
             EBUR128_ERROR_INVALID_ARGUMENT);
  UNIT_CHECK(ebur128_segment_block_energy(NULL, 0, &energy) ==
             EBUR128_ERROR_INVALID_ARGUMENT);
  UNIT_CHECK(ebur128_segment_block_energy(s, 5, &energy) ==
             EBUR128_ERROR_INVALID_ARGUMENT);
  UNIT_CHECK(ebur128_segment_block_energy(s, 0, NULL) ==
             EBUR128_ERROR_INVALID_ARGUMENT);
  UNIT_CHECK(energy == -1.0); /* output not written on failure */
  UNIT_CHECK(ebur128_segment_storage_bytes(NULL) == 0);
  ebur128_segment_destroy(&s);
  ebur128_segment_destroy(&other);
  pcm_free(bad_inf, hop * 2 * sizeof(double));
  pcm_free(bad_nan, hop * 2 * sizeof(double));
  pcm_free(pcm, 10 * hop * 2 * sizeof(double));
}
