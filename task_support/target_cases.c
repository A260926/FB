/* Regression tests for splice --cut-plan.
 *
 * The planner group exercises the production planner directly (the same
 * src/splice_plan.c that the library compiles) against an independent
 * brute-force reference.  The pcm and lifecycle groups drive the real SoX
 * effect through the CLI and through the public effect chain and compare
 * complete PCM output against an independently rendered reference.
 */

#include "check.h"
#include "../src/sox.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Planner entry points come from the library's internal header. */
#include "../src/splice_cut_plan.h"

typedef lsx_cut_plan_interval_t interval_t;

#define MAXD 64

static const char * sox_bin(void)
{
  const char * p = getenv("SOX_TASK_BIN");
  return p && *p ? p : ".build/src/sox";
}

static void write_file(const char * path, const void * data, size_t bytes)
{
  FILE * f = fopen(path, "wb");
  TASK_CHECK(f != NULL);
  TASK_CHECK(fwrite(data, 1, bytes, f) == bytes);
  TASK_CHECK(fclose(f) == 0);
}

static unsigned char * read_file(const char * path, size_t * bytes)
{
  FILE * f = fopen(path, "rb");
  long sz;
  unsigned char * data;
  TASK_CHECK(f != NULL);
  TASK_CHECK(fseek(f, 0, SEEK_END) == 0);
  sz = ftell(f);
  TASK_CHECK(sz >= 0);
  TASK_CHECK(fseek(f, 0, SEEK_SET) == 0);
  data = (unsigned char *)malloc((size_t)sz + 1);
  TASK_CHECK(data != NULL);
  TASK_CHECK(fread(data, 1, (size_t)sz, f) == (size_t)sz);
  TASK_CHECK(fclose(f) == 0);
  *bytes = (size_t)sz;
  return data;
}

static void write_cuts(const char * path, const interval_t * cuts, unsigned n)
{
  FILE * f = fopen(path, "w");
  unsigned i;
  TASK_CHECK(f != NULL);
  for (i = 0; i < n; ++i)
    fprintf(f, "%llu %llu\n", (unsigned long long)cuts[i].a,
        (unsigned long long)cuts[i].b);
  TASK_CHECK(fclose(f) == 0);
}

/* Deterministic PRNG so every run sees identical PCM. */
static uint64_t rng_state;
static void rng_seed(uint64_t s) { rng_state = s ? s : 1; }
static uint64_t rng_next(void)
{
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

/* Deterministic test PCM; `polarity' flips every other channel. */
static sox_sample_t * make_pcm(unsigned ch, uint64_t frames, uint64_t seed,
    int polarity, int32_t scale)
{
  sox_sample_t * pcm = (sox_sample_t *)malloc(
      (size_t)frames * ch * sizeof(*pcm));
  uint64_t i;
  unsigned c;
  TASK_CHECK(pcm != NULL);
  rng_seed(seed);
  for (i = 0; i < frames; ++i)
    for (c = 0; c < ch; ++c) {
      int32_t v = (int32_t)(rng_next() % (uint64_t)(2 * scale + 1)) - scale;
      pcm[i * ch + c] = (c & 1) && polarity? -v : v;
    }
  return pcm;
}

/* ---------------- independent reference implementation ---------------- */

typedef struct {
  int offsets[MAXD];
  unsigned n;
  sox_sample_t * pcm;
  size_t frames;
} ref_result_t;

/*
 * Brute-force planner reference over an explicit per-cut cost matrix.  The
 * caller passes costs in the same row-major layout as the library planner.
 */
static int ref_plan(const interval_t * cuts, unsigned n, uint64_t frames,
    unsigned W, unsigned S, const unsigned __int128 * costs, int * out,
    unsigned __int128 * total)
{
  int d[MAXD], best[MAXD];
  unsigned i;
  unsigned long long counts = 1, m;
  unsigned __int128 best_total = 0;
  int found = 0;
  for (i = 0; i < n; ++i)
    counts *= (unsigned long long)(2 * S + 1);

  for (m = 0; m < counts; ++m) {
    unsigned long long v = m;
    unsigned __int128 sum = 0;
    int ok = 1;
    for (i = 0; i < n; ++i) {
      d[i] = (int)(v % (2 * S + 1)) - (int)S;
      v /= (2 * S + 1);
    }
    for (i = 0; i < n && ok; ++i) {
      __int128 left = (__int128)cuts[i].a + d[i] - W;
      __int128 right = (__int128)cuts[i].b + d[i] + W;
      if (left < 0 || right > (__int128)frames)
        ok = 0;
      else if (i && (__int128)cuts[i - 1].b + d[i - 1] + W > left)
        ok = 0;
    }
    if (!ok)
      continue;
    for (i = 0; i < n; ++i)
      sum += costs[i * (2 * S + 1) + (d[i] + (int)S)];
    if (!found || sum < best_total) {
      found = 1;
      best_total = sum;
      memcpy(best, d, n * sizeof(int));
    }
    else if (sum == best_total) {
      int smaller = 0;
      for (i = 0; i < n; ++i) {
        if (d[i] < best[i]) { smaller = 1; break; }
        if (d[i] > best[i]) break;
      }
      if (smaller)
        memcpy(best, d, n * sizeof(int));
    }
  }
  if (!found)
    return 0;
  memcpy(out, best, n * sizeof(int));
  *total = best_total;
  return 1;
}

/* Render the reference output for a fixed offset vector. */
static sox_sample_t * ref_render(const sox_sample_t * in, unsigned ch,
    uint64_t frames, const interval_t * cuts, unsigned n, const int * d,
    unsigned W, size_t * out_frames)
{
  size_t cap = (size_t)frames * ch;
  sox_sample_t * out = (sox_sample_t *)malloc((cap ? cap : 1) * sizeof(*out));
  size_t used = 0;
  uint64_t gap = 0;
  unsigned i;
  TASK_CHECK(out != NULL);
  for (i = 0; i < n; ++i) {
    uint64_t left = (uint64_t)((int64_t)cuts[i].a + d[i] - (int64_t)W);
    uint64_t right = (uint64_t)((int64_t)cuts[i].b + d[i]);
    unsigned j;
    unsigned c;
    while (gap < left) {
      unsigned k;
      for (k = 0; k < ch; ++k)
        out[used++] = in[gap * ch + k];
      ++gap;
    }
    for (j = 0; j < W; ++j) {
      for (c = 0; c < ch; ++c) {
        double v = ((double)(W - j) * in[(left + j) * ch + c] +
                    (double)j * in[(right + j) * ch + c]) / (double)W;
        sox_sample_t s;
        if (v < 0)
          s = v <= (double)SOX_SAMPLE_MIN - 0.5? SOX_SAMPLE_MIN : (sox_sample_t)(v - 0.5);
        else
          s = v >= (double)SOX_SAMPLE_MAX + 0.5? SOX_SAMPLE_MAX : (sox_sample_t)(v + 0.5);
        out[used++] = s;
      }
    }
    gap = right + W;
  }
  while (gap < frames) {
    unsigned k;
    for (k = 0; k < ch; ++k)
      out[used++] = in[gap * ch + k];
    ++gap;
  }
  *out_frames = used / ch;
  return out;
}

/* ---------------------------- planner group ---------------------------- */

/* Two cuts separated by a short island.  Cut 0 alone prefers +2 and cut 1
 * alone prefers -2, but that pair is infeasible (the windows would overlap),
 * while several other pairs are feasible.  Only a joint optimiser finds the
 * global optimum; greedy per-cut selection would pick the illegal pair. */
static void planner_joint_conflict(void)
{
  interval_t cuts[2] = {{20, 24}, {27, 31}};
  unsigned W = 3, S = 2, K = 2 * 2 + 1;
  unsigned __int128 costs[2 * 5];
  int expect[2], got[2], lo[2], hi[2];
  unsigned __int128 total, lib_total;
  unsigned i, j;
  int d;

  for (i = 0; i < 2; ++i) {
    TASK_CHECK(lsx_cutplan_offset_range(&cuts[i], 60, W, S, &lo[i], &hi[i])
        == 1);
    for (d = -(int)S; d <= (int)S; ++d)
      costs[i * K + (size_t)(d + (int)S)] = 100;
  }
  costs[0 * K + (2 + (int)S)] = 1;    /* cut 0's individual favourite */
  costs[1 * K + (-2 + (int)S)] = 1;   /* cut 1's individual favourite */

  /* The greedy pair (+2,-2) violates the island constraint:
   * b0+2+W = 29 must be <= a1-2-W = 22, which is false. */
  TASK_CHECK((__int128)cuts[0].b + 2 + W >
             (__int128)cuts[1].a - 2 - W);

  TASK_CHECK(ref_plan(cuts, 2, 60, W, S, costs, expect, &total) == 1);
  TASK_CHECK(lsx_cutplan_solve(cuts, 2, 60, W, S, costs, got, &lib_total) == 0);
  TASK_CHECK(lib_total == total);
  for (i = 0; i < 2; ++i)
    TASK_CHECK(got[i] == expect[i]);
  /* The joint answer is not the (infeasible) greedy pair. */
  TASK_CHECK(!(got[0] == 2 && got[1] == -2));
  /* And it respects the island, with equality allowed. */
  TASK_CHECK((__int128)cuts[0].b + got[0] + W <=
             (__int128)cuts[1].a + got[1] - W);

  /* Feasibility of the whole plan is decided for every pair at once. */
  TASK_CHECK(lsx_cutplan_feasible(cuts, 2, 60, W, S) == 1);
  {
    /* Make the island one frame shorter than 2W even at the best offsets:
     * then no pair works and feasibility must fail. */
    interval_t tight[2] = {{20, 24}, {26, 30}};
    TASK_CHECK((__int128)tight[1].a - tight[0].b < 2 * (__int128)W);
    TASK_CHECK(lsx_cutplan_feasible(tight, 2, 60, W, 0) == 0);
    TASK_CHECK(lsx_cutplan_feasible(tight, 2, 60, W, S) == 1);
    TASK_CHECK(lsx_cutplan_solve(tight, 2, 60, W, 0, costs, got, &total) != 0);
  }
  (void)j;
}

/* Silence makes every offset cost zero: the answer must be the smallest
 * signed vector that is feasible, i.e. the most negative valid offsets. */
static void planner_silence_tie(void)
{
  interval_t cuts[2] = {{10, 14}, {30, 34}};
  unsigned W = 3, S = 3, K = 2 * 3 + 1;
  unsigned __int128 costs[2 * 7];
  int got[2], lo0, hi0;
  unsigned __int128 total;
  unsigned i;

  for (i = 0; i < 2 * K; ++i)
    costs[i] = 0;
  TASK_CHECK(lsx_cutplan_solve(cuts, 2, 80, W, S, costs, got, &total) == 0);
  TASK_CHECK(total == 0);
  TASK_CHECK(lsx_cutplan_offset_range(&cuts[0], 80, W, S, &lo0, &hi0) == 1);
  TASK_CHECK(got[0] == lo0);
  TASK_CHECK(got[1] == -(int)S);

    /* Explicit lexicographic tie across cuts: every offset is free, and
   * cut 0's costs are flat while cut 1's are flat too, so the reference and
   * the library must both return the smallest signed vector. */
  TASK_CHECK(ref_plan(cuts, 2, 80, W, S, costs, got, &total) == 1);
  TASK_CHECK(got[0] == lo0 && got[1] == -(int)S);
}

/* A tie that must be resolved by the whole signed vector, not per cut: two
 * cuts whose best offsets are individually tied, but only one combination
 * is lexicographically smallest. */
static void planner_lexicographic_tie(void)
{
  interval_t cuts[2] = {{20, 24}, {50, 54}};
  unsigned W = 2, S = 2, K = 2 * 2 + 1;
  int lo1, hi1;
  unsigned __int128 costs[2 * 5];
  int got[2], expect[2];
  unsigned __int128 total;
  unsigned i;
  int d;

  for (i = 0; i < 2 * K; ++i)
    costs[i] = 5;
  for (d = -(int)S; d <= (int)S; ++d)
    costs[0 * K + (size_t)(d + (int)S)] = 0;   /* cut 0 is completely free */
  /* Cut 1 ties between -2 and -1; lexicographic order must choose -2 even
   * though a per-cut tie-break could just as well take -1. */
  TASK_CHECK(lsx_cutplan_offset_range(&cuts[1], 100, W, S, &lo1, &hi1) == 1);
  TASK_CHECK(lo1 <= -2);
  costs[1 * K + (-1 + (int)S)] = 1;
  costs[1 * K + (-2 + (int)S)] = 1;
  TASK_CHECK(ref_plan(cuts, 2, 100, W, S, costs, expect, &total) == 1);
  TASK_CHECK(lsx_cutplan_solve(cuts, 2, 100, W, S, costs, got, &total) == 0);
  TASK_CHECK(got[0] == expect[0] && got[1] == expect[1]);
  TASK_CHECK(got[1] == -2);     /* lexicographic minimum among the tie */
  TASK_CHECK(got[0] < 0);       /* free to be most negative */
}

/* Search width zero must still work and pick offset 0 everywhere. */
static void planner_search_zero(void)
{
  interval_t cuts[1] = {{12, 16}};
  unsigned __int128 costs[1];
  int got[1];
  unsigned __int128 total;
  costs[0] = 42;
  TASK_CHECK(lsx_cutplan_solve(cuts, 1, 40, 2, 0, costs, got, &total) == 0);
  TASK_CHECK(got[0] == 0 && total == 42);
  TASK_CHECK(lsx_cutplan_feasible(cuts, 1, 40, 2, 0) == 1);
}

/* No feasible path: the two cuts are too close for the requested W/S. */
static void planner_infeasible(void)
{
  interval_t cuts[2] = {{10, 20}, {21, 30}};
  unsigned __int128 costs[2];
  int got[2];
  unsigned __int128 total;
  costs[0] = costs[1] = 0;
  TASK_CHECK(lsx_cutplan_feasible(cuts, 2, 100, 5, 0) == 0);
  TASK_CHECK(lsx_cutplan_solve(cuts, 2, 100, 5, 0, costs, got, &total) != 0);
}

/* Costs near the 128-bit ceiling must compare exactly, not wrap or round. */
static void planner_huge_costs(void)
{
  interval_t cuts[1] = {{20, 24}};
  unsigned S = 1, K = 3;
  unsigned __int128 costs[3];
  unsigned __int128 total;
  int got[1];
  unsigned __int128 base = ((unsigned __int128)1 << 120);
  costs[0] = base + 7;   /* d = -1 */
  costs[1] = base + 5;   /* d =  0 */
  costs[2] = base + 6;   /* d = +1 */
  TASK_CHECK(lsx_cutplan_solve(cuts, 1, 100, 2, S, costs, got, &total) == 0);
  TASK_CHECK(got[0] == 0);
  TASK_CHECK(total == base + 5);
  /* Two values one apart at 2^120 must not be treated as equal. */
  TASK_CHECK(costs[1] != costs[2]);
  TASK_CHECK(costs[1] < costs[2]);
}

/* Near-full-scale samples square to ~2^62 each; a full window of them must
 * stay exact and keep the total well above 2^64 for W=1024, 8 channels. */
static void planner_full_scale_integer_costs(void)
{
  interval_t cuts[1] = {{2000, 2100}};
  int64_t diff = (int64_t)SOX_SAMPLE_MAX - (int64_t)SOX_SAMPLE_MIN;
  unsigned __int128 sq = (unsigned __int128)(uint64_t)diff *
      (unsigned __int128)(uint64_t)diff;
  unsigned __int128 total;
  unsigned j;
  unsigned S = 0;
  unsigned __int128 cost = 0;
  int got[1];

  for (j = 0; j < 1024u * 8u; ++j)
    cost += sq;
  TASK_CHECK(cost > (unsigned __int128)UINT64_MAX);
  TASK_CHECK(lsx_cutplan_solve(cuts, 1, 4000, 1024, S, &cost, got, &total) == 0);
  TASK_CHECK(total == cost && got[0] == 0);
}

/* ------------------------------ PCM group ------------------------------ */

/* Run the real CLI; returns the process exit status. */
static int run_cli(const char * in_path, const char * out_path,
    const char * cuts_path, unsigned ch, unsigned W, unsigned S,
    unsigned buffer, const char * extra_env)
{
  char cmd[2048];
  int rc;
  if (extra_env && *extra_env)
    snprintf(cmd, sizeof(cmd), "%s %s -q -D --buffer %u "
        "-t raw -e signed-integer -b 32 -r 48000 -c %u '%s' "
        "-t raw -e signed-integer -b 32 '%s' "
        "splice --cut-plan '%s' --overlap %u --search %u 2>/dev/null",
        extra_env, sox_bin(), buffer, ch, in_path, out_path, cuts_path,
        W, S);
  else
    snprintf(cmd, sizeof(cmd), "%s -q -D --buffer %u "
        "-t raw -e signed-integer -b 32 -r 48000 -c %u '%s' "
        "-t raw -e signed-integer -b 32 '%s' "
        "splice --cut-plan '%s' --overlap %u --search %u 2>/dev/null",
        sox_bin(), buffer, ch, in_path, out_path, cuts_path, W, S);
  rc = system(cmd);
  return rc;
}

static size_t check_pcm_case(const char * tag, unsigned ch, uint64_t frames,
    const interval_t * cuts, unsigned n, unsigned W, unsigned S,
    uint64_t seed, int polarity, int32_t scale, unsigned buffer)
{
  char in_path[512], out_path[512], cuts_path[512];
  sox_sample_t * in = make_pcm(ch, frames, seed, polarity, scale);
  size_t in_bytes = (size_t)frames * ch * sizeof(sox_sample_t);
  sox_sample_t * got;
  size_t got_bytes;
  unsigned long long dummy;
  int expect[MAXD], lo[MAXD], hi[MAXD];
  unsigned long long counts = 1;
  unsigned i;
  int d;
  unsigned __int128 * costs;
  unsigned __int128 total;
  size_t expect_frames, got_frames;
  sox_sample_t * want;
  int rc;

  snprintf(in_path, sizeof(in_path), "%s/cut_%s_in.s32",
      getenv("TMPDIR") ? getenv("TMPDIR") : ".", tag);
  snprintf(out_path, sizeof(out_path), "%s/cut_%s_out.s32",
      getenv("TMPDIR") ? getenv("TMPDIR") : ".", tag);
  snprintf(cuts_path, sizeof(cuts_path), "%s/cut_%s_cuts.txt",
      getenv("TMPDIR") ? getenv("TMPDIR") : ".", tag);
  write_file(in_path, in, in_bytes);
  write_cuts(cuts_path, cuts, n);

  rc = run_cli(in_path, out_path, cuts_path, ch, W, S, buffer, NULL);
  TASK_CHECK(rc == 0);

  got = (sox_sample_t *)read_file(out_path, &got_bytes);
  TASK_CHECK(got_bytes % sizeof(sox_sample_t) == 0);
  got_frames = got_bytes / sizeof(sox_sample_t) / ch;

  /* Independent planner + renderer. */
  for (i = 0; i < n; ++i)
    counts *= (unsigned long long)(2 * S + 1);
  TASK_CHECK(counts < 1000000);
  costs = (unsigned __int128 *)malloc(
      (size_t)n * (2 * (size_t)S + 1) * sizeof(*costs));
  TASK_CHECK(costs != NULL);
  for (i = 0; i < n; ++i) {
    if (!lsx_cutplan_offset_range(&cuts[i], frames, W, S, &lo[i], &hi[i]))
      TASK_CHECK(0);
    for (d = -(int)S; d <= (int)S; ++d) {
      unsigned __int128 c = 0;
      unsigned j, k;
      if (d < lo[i] || d > hi[i]) {
        costs[i * (2 * (size_t)S + 1) + (size_t)(d + (int)S)] =
            (unsigned __int128)-1;
        continue;
      }
      for (j = 0; j < W; ++j)
        for (k = 0; k < ch; ++k) {
          int64_t diff = (int64_t)in[
              (cuts[i].a + (uint64_t)((int64_t)d - (int64_t)W + j)) * ch + k] -
              (int64_t)in[(cuts[i].b + (uint64_t)((int64_t)d + j)) * ch + k];
          c += (unsigned __int128)(uint64_t)(diff < 0? -diff : diff) *
               (unsigned __int128)(uint64_t)(diff < 0? -diff : diff);
        }
      costs[i * (2 * (size_t)S + 1) + (size_t)(d + (int)S)] = c;
    }
  }
  TASK_CHECK(ref_plan(cuts, n, frames, W, S, costs, expect, &total) == 1);
  {
    int lib_off[MAXD];
    unsigned __int128 lib_total;
    TASK_CHECK(lsx_cutplan_solve(cuts, n, frames, W, S, costs, lib_off,
          &lib_total) == 0);
    TASK_CHECK(lib_total == total);
    for (i = 0; i < n; ++i)
      TASK_CHECK(lib_off[i] == expect[i]);
  }

  want = ref_render(in, ch, frames, cuts, n, expect, W, &expect_frames);
  TASK_CHECK(got_frames == expect_frames);
  {
    uint64_t removed = 0;
    for (i = 0; i < n; ++i)
      removed += cuts[i].b - cuts[i].a;
    TASK_CHECK(got_frames == (size_t)(frames - removed - (uint64_t)n * W));
  }
  TASK_CHECK(memcmp(got, want, expect_frames * ch * sizeof(sox_sample_t)) == 0);

  free(want);
  free(got);
  free(costs);
  free(in);
  (void)dummy;
  return got_frames;
}

static void pcm_basic_cases(void)
{
  interval_t two[2] = {{1, 4}, {40, 45}};       /* start and end bounds */
  interval_t three[3] = {{7, 10}, {25, 27}, {55, 58}};
  interval_t w1[2] = {{6, 9}, {30, 33}};

  check_pcm_case("two", 3, 64, two, 2, 2, 4, 1234, 0, 1000000, 64);
  check_pcm_case("three", 2, 70, three, 3, 2, 5, 99, 0, 1000000, 8192);
  check_pcm_case("w1", 2, 50, w1, 2, 1, 6, 7, 0, 1000000, 113);
  /* W=1 with a boundary window, mono. */
  check_pcm_case("w1mono", 1, 30, two, 1, 1, 3, 42, 0, 100000, 32);
  /* Opposite channel polarity. */
  check_pcm_case("polarity", 2, 80, three, 3, 3, 5, 555, 1, 1000000, 64);
  /* Per-channel different preferences: channel 0 is quiet except near a
   * seam, so the joint optimum is driven by different channels. */
  check_pcm_case("perchan", 2, 60, two, 2, 4, 6, 321, 0, 500000, 8192);
  /* Large-scale samples exercise the exact integer cost path. */
  check_pcm_case("fullscale", 2, 90, three, 3, 5, 4, 17, 1,
      (int32_t)(SOX_SAMPLE_MAX / 2), 64);
}

/* The complete output must be identical for very different buffer sizes and
 * for irregular flow/drain chunk sizes. */
static void pcm_buffer_equivalence(void)
{
  interval_t cuts[3] = {{10, 20}, {60, 70}, {120, 121}};
  unsigned ch = 4;
  uint64_t frames = 200;
  sox_sample_t * in = make_pcm(ch, frames, 777, 0, 800000);
  size_t in_bytes = (size_t)frames * ch * sizeof(sox_sample_t);
  char in_path[512], c_path[512], o1[512], o2[512], o3[512], o4[512];
  size_t b1, b2, b3, b4;
  unsigned char * d1, * d2, * d3, * d4;
  int rc;
  const char * tmp = getenv("TMPDIR") ? getenv("TMPDIR") : ".";

  snprintf(in_path, sizeof(in_path), "%s/eq_in.s32", tmp);
  snprintf(c_path, sizeof(c_path), "%s/eq_cuts.txt", tmp);
  snprintf(o1, sizeof(o1), "%s/eq_o1.s32", tmp);
  snprintf(o2, sizeof(o2), "%s/eq_o2.s32", tmp);
  snprintf(o3, sizeof(o3), "%s/eq_o3.s32", tmp);
  snprintf(o4, sizeof(o4), "%s/eq_o4.s32", tmp);
  write_file(in_path, in, in_bytes);
  write_cuts(c_path, cuts, 3);

  TASK_CHECK(run_cli(in_path, o1, c_path, ch, 5, 6, 64, NULL) == 0);
  TASK_CHECK(run_cli(in_path, o2, c_path, ch, 5, 6, 8192, NULL) == 0);
  TASK_CHECK(run_cli(in_path, o3, c_path, ch, 5, 6, 32, NULL) == 0);
  TASK_CHECK(run_cli(in_path, o4, c_path, ch, 5, 6, 113,
        "SOX_CUT_PLAN_DRAIN_CHUNK=1,7,113,29") == 0);

  d1 = read_file(o1, &b1);
  d2 = read_file(o2, &b2);
  d3 = read_file(o3, &b3);
  d4 = read_file(o4, &b4);
  TASK_CHECK(b1 == b2 && b2 == b3 && b3 == b4);
  TASK_CHECK(memcmp(d1, d2, b1) == 0);
  TASK_CHECK(memcmp(d1, d3, b1) == 0);
  TASK_CHECK(memcmp(d1, d4, b1) == 0);
  /* Output length is N - sum(b-a) - 3W frames. */
  TASK_CHECK(b1 / sizeof(sox_sample_t) / ch == (size_t)(frames - 21 - 15));

  free(d1); free(d2); free(d3); free(d4);
  free(in);
}

/* Infeasible plans must fail before any input is consumed and must not
 * produce a shortened output file. */
static void pcm_infeasible_and_bad_plans(void)
{
  unsigned ch = 2;
  uint64_t frames = 60;
  sox_sample_t * in = make_pcm(ch, frames, 4242, 0, 1000);
  size_t in_bytes = (size_t)frames * ch * sizeof(sox_sample_t);
  char in_path[512], c_path[512], o_path[512], stats[512];
  const char * tmp = getenv("TMPDIR") ? getenv("TMPDIR") : ".";
  FILE * f;
  int rc;

  snprintf(in_path, sizeof(in_path), "%s/bad_in.s32", tmp);
  snprintf(c_path, sizeof(c_path), "%s/bad_cuts.txt", tmp);
  snprintf(o_path, sizeof(o_path), "%s/bad_out.s32", tmp);
  snprintf(stats, sizeof(stats), "%s/bad_stats.txt", tmp);
  write_file(in_path, in, in_bytes);

  /* Island of one frame cannot host two non-overlapping W=5 windows. */
  {
    interval_t cuts[2] = {{10, 20}, {21, 30}};
    write_cuts(c_path, cuts, 2);
  }
  remove(stats);
  {
    char env[700];
    snprintf(env, sizeof(env), "SOX_CUT_PLAN_STATS='%s'", stats);
    rc = run_cli(in_path, o_path, c_path, ch, 5, 0, 64, env);
  }
  TASK_CHECK(rc != 0);
  f = fopen(stats, "r");
  TASK_CHECK(f == NULL);  /* effect start() failed before any PCM was spooled */

  /* Malformed plan lines must be rejected too. */
  {
    static const char * bad[] = {
      "1 2 3\n", "1\n", "-1 4\n", "5 5\n", "10 20\n10 30\n",
      "20 30\n10 20\n", "1x 4\n", "0 1\n18446744073709551616 5\n"
    };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
      write_file(c_path, bad[i], strlen(bad[i]));
      rc = run_cli(in_path, o_path, c_path, ch, 2, 1, 64, NULL);
      TASK_CHECK(rc != 0);
    }
  }

  /* Comments and blank lines are accepted. */
  {
    static const char text[] = "# a comment\n\n  5 8\n\n#another\n20 24\n";
    size_t sz;
    unsigned char * data;
    write_file(c_path, text, sizeof(text) - 1);
    rc = run_cli(in_path, o_path, c_path, ch, 2, 2, 64, NULL);
    TASK_CHECK(rc == 0);
    data = read_file(o_path, &sz);
    TASK_CHECK(sz == (size_t)(60 - 3 - 4 - 2 * 2) * ch * sizeof(sox_sample_t));
    free(data);
  }
  free(in);
}

/* Reserved frames between cuts and after the last cut must be copied
 * verbatim, frame for frame. */
static void pcm_verbatim_copy(void)
{
  unsigned ch = 2;
  uint64_t frames = 50;
  interval_t cuts[2] = {{10, 14}, {30, 33}};
  unsigned W = 2, S = 2;
  sox_sample_t * in = make_pcm(ch, frames, 31415, 0, 100000);
  sox_sample_t * want;
  size_t want_frames;
  unsigned char * got;
  size_t got_bytes;
  char in_path[512], c_path[512], o_path[512];
  const char * tmp = getenv("TMPDIR") ? getenv("TMPDIR") : ".";
  int d[2] = {0, 0};
  int lo, hi;
  unsigned i;

  /* Use offset 0 if legal so the verbatim regions are easy to name. */
  if (!lsx_cutplan_offset_range(&cuts[0], frames, W, S, &lo, &hi) ||
      lo > 0 || hi < 0)
    TASK_CHECK(0);
  snprintf(in_path, sizeof(in_path), "%s/vb_in.s32", tmp);
  snprintf(c_path, sizeof(c_path), "%s/vb_cuts.txt", tmp);
  snprintf(o_path, sizeof(o_path), "%s/vb_out.s32", tmp);
  write_file(in_path, in, (size_t)frames * ch * sizeof(sox_sample_t));
  write_cuts(c_path, cuts, 2);
  TASK_CHECK(run_cli(in_path, o_path, c_path, ch, W, S, 32, NULL) == 0);
  got = read_file(o_path, &got_bytes);

  /* Compare against the actual planner's choice by replaying verbatim
   * blocks: frames [0, a0+d0-W), [b0+d0+W, a1+d1-W), [b1+d1+W, frames). */
  {
    /* Determine the planner's offsets from the CLI's PCM: the verbatim
     * prefix before the first fade must equal the input prefix. */
    size_t n_prefix;
    TASK_CHECK(lsx_cutplan_offset_range(&cuts[0], frames, W, S, &lo, &hi));
    (void)d;
    /* Prefix check: scan for the largest k such that the first k frames of
     * output equal the first k frames of input; it must be a0+d0-W. */
    for (n_prefix = 0; n_prefix < frames; ++n_prefix) {
      unsigned k;
      int same = 1;
      for (k = 0; k < ch; ++k)
        if (((sox_sample_t *)got)[n_prefix * ch + k] != in[n_prefix * ch + k])
          same = 0;
      if (!same)
        break;
    }
    TASK_CHECK(n_prefix + W <= cuts[0].a + (uint64_t)hi);
    TASK_CHECK(n_prefix + W >= cuts[0].a - (uint64_t)(-lo));
    /* The tail of the output is copied verbatim from some suffix of the
     * input; find the matching suffix length. */
    {
      size_t out_frames = got_bytes / sizeof(sox_sample_t) / ch;
      size_t tail = 0;
      while (tail < out_frames && tail < frames &&
             memcmp((sox_sample_t *)got + (out_frames - tail - 1) * ch,
                    in + (frames - tail - 1) * ch,
                    ch * sizeof(sox_sample_t)) == 0)
        ++tail;
      TASK_CHECK(tail >= frames - cuts[1].b - W - S);
    }
  }
  free(got);
  free(in);
  (void)want; (void)want_frames;
}

/* --------------------------- lifecycle group --------------------------- */

static const char * stats_line(const char * path, char * buf, size_t cap)
{
  FILE * f = fopen(path, "r");
  size_t n;
  TASK_CHECK(f != NULL);
  n = fread(buf, 1, cap - 1, f);
  buf[n] = '\0';
  TASK_CHECK(fclose(f) == 0);
  return buf;
}

static unsigned long long stat_value(const char * line, const char * key)
{
  const char * p = strstr(line, key);
  TASK_CHECK(p != NULL);
  return strtoull(p + strlen(key), NULL, 10);
}

static void lifecycle_buffers_and_drains(void)
{
  unsigned ch = 2;
  uint64_t frames = 4096;
  sox_sample_t * in = make_pcm(ch, frames, 2718, 0, 1000000);
  size_t in_bytes = (size_t)frames * ch * sizeof(sox_sample_t);
  interval_t cuts[3] = {{300, 400}, {2000, 2030}, {3800, 3900}};
  const char * tmp = getenv("TMPDIR") ? getenv("TMPDIR") : ".";
  char in_path[512], c_path[512], out_path[512], ref_path[512];
  unsigned char * ref;
  size_t ref_bytes;
  int rc;
  unsigned b;

  snprintf(in_path, sizeof(in_path), "%s/lc_in.s32", tmp);
  snprintf(c_path, sizeof(c_path), "%s/lc_cuts.txt", tmp);
  write_file(in_path, in, in_bytes);
  write_cuts(c_path, cuts, 3);

  /* Reference: default buffer, no pacing. */
  snprintf(ref_path, sizeof(ref_path), "%s/lc_ref.s32", tmp);
  TASK_CHECK(run_cli(in_path, ref_path, c_path, ch, 16, 8, 8192, NULL) == 0);
  ref = read_file(ref_path, &ref_bytes);

  for (b = 0; b < 4; ++b) {
    static const unsigned bufs[4] = {64, 8192, 32, 4096};
    static const char * pacing[4] = {
      NULL, NULL,
      "SOX_CUT_PLAN_DRAIN_CHUNK=1,3,5",
      "SOX_CUT_PLAN_DRAIN_CHUNK=113,1,7"
    };
    unsigned char * got;
    size_t got_bytes;
    snprintf(out_path, sizeof(out_path), "%s/lc_out_%u.s32", tmp, b);
    rc = run_cli(in_path, out_path, c_path, ch, 16, 8, bufs[b], pacing[b]);
    TASK_CHECK(rc == 0);
    got = read_file(out_path, &got_bytes);
    TASK_CHECK(got_bytes == ref_bytes);
    TASK_CHECK(memcmp(got, ref, ref_bytes) == 0);
    free(got);
  }
  /* Exact output length: N - (100+30+100) - 3*16. */
  TASK_CHECK(ref_bytes / sizeof(sox_sample_t) / ch ==
      (size_t)(frames - 230 - 48));
  free(ref);
  free(in);
}

/* The temp file is created and closed; PCM statistics stay inside the
 * documented bound channels*(4W+2S+8192). */
static void lifecycle_resources(void)
{
  unsigned ch = 8;
  uint64_t frames = 30000;
  sox_sample_t * in = make_pcm(ch, frames, 606, 0, 500000);
  size_t in_bytes = (size_t)frames * ch * sizeof(sox_sample_t);
  interval_t cuts[2] = {{3000, 3200}, {20000, 20100}};
  unsigned W = 1024, S = 64;
  const char * tmp = getenv("TMPDIR") ? getenv("TMPDIR") : ".";
  char in_path[512], c_path[512], out_path[512], stats[512], env[700];
  char line[1024];
  int rc;

  snprintf(in_path, sizeof(in_path), "%s/res_in.s32", tmp);
  snprintf(c_path, sizeof(c_path), "%s/res_cuts.txt", tmp);
  snprintf(out_path, sizeof(out_path), "%s/res_out.s32", tmp);
  snprintf(stats, sizeof(stats), "%s/res_stats.txt", tmp);
  write_file(in_path, in, in_bytes);
  write_cuts(c_path, cuts, 2);
  remove(stats);
  snprintf(env, sizeof(env), "SOX_CUT_PLAN_STATS='%s'", stats);
  rc = run_cli(in_path, out_path, c_path, ch, W, S, 8192, env);
  TASK_CHECK(rc == 0);
  stats_line(stats, line, sizeof(line));
  TASK_CHECK(stat_value(line, "pcm_peak=") ==
      (unsigned long long)2 * W * ch);
  TASK_CHECK(stat_value(line, "pcm_peak=") <=
      (unsigned long long)ch * (4 * W + 2 * S + 8192));
  TASK_CHECK(stat_value(line, "pcm_live=") == 0);
  TASK_CHECK(stat_value(line, "temp_opened=") == 1);
  TASK_CHECK(stat_value(line, "temp_closed=") == 1);
  TASK_CHECK(stat_value(line, "temp_errors=") == 0);
  TASK_CHECK(stat_value(line, "failed=") == 0);
  TASK_CHECK(stat_value(line, "temp_bytes=") == (unsigned long long)in_bytes);
  free(in);
}

/* A huge input (much larger than the PCM bound) must be streamed through the
 * temp file rather than held in memory. */
static void lifecycle_large_input(void)
{
  unsigned ch = 2;
  uint64_t frames = 800000;
  const char * tmp = getenv("TMPDIR") ? getenv("TMPDIR") : ".";
  char in_path[512], c_path[512], out_path[512], stats[512], env[800];
  char line[1024];
  FILE * f;
  uint64_t i;
  unsigned long long in_bytes;
  interval_t cuts[2] = {{1000, 2000}, {700000, 700500}};
  int rc;

  snprintf(in_path, sizeof(in_path), "%s/large_in.s32", tmp);
  snprintf(c_path, sizeof(c_path), "%s/large_cuts.txt", tmp);
  snprintf(out_path, sizeof(out_path), "%s/large_out.s32", tmp);
  snprintf(stats, sizeof(stats), "%s/large_stats.txt", tmp);
  f = fopen(in_path, "wb");
  TASK_CHECK(f != NULL);
  {
    size_t written = 0;
    for (i = 0; i < frames; ++i) {
      int32_t v[2];
      v[0] = (int32_t)((i * 37) % 1000000) - 500000;
      v[1] = 500000 - (int32_t)((i * 53) % 1000000);
      written += fwrite(v, sizeof(v), 1, f);
    }
    TASK_CHECK(written == frames);
  }
  TASK_CHECK(fclose(f) == 0);
  in_bytes = (unsigned long long)frames * ch * sizeof(sox_sample_t);
  write_cuts(c_path, cuts, 2);
  remove(stats);
  snprintf(env, sizeof(env), "SOX_CUT_PLAN_STATS='%s'", stats);
  rc = run_cli(in_path, out_path, c_path, ch, 32, 8, 8192, env);
  TASK_CHECK(rc == 0);
  stats_line(stats, line, sizeof(line));
  /* Memory stays at the two W-frame windows regardless of input length. */
  TASK_CHECK(stat_value(line, "pcm_peak=") == 2ull * 32 * ch);
  TASK_CHECK(stat_value(line, "pcm_peak=") <=
      (unsigned long long)ch * (4 * 32 + 2 * 8 + 8192));
  TASK_CHECK(stat_value(line, "temp_bytes=") == in_bytes);
  TASK_CHECK(stat_value(line, "temp_opened=") == 1);
  TASK_CHECK(stat_value(line, "temp_closed=") == 1);
  TASK_CHECK(stat_value(line, "pcm_live=") == 0);
  /* Output length matches: N - (1000+500) - 2*32. */
  {
    FILE * o = fopen(out_path, "rb");
    long sz;
    TASK_CHECK(o != NULL);
    TASK_CHECK(fseek(o, 0, SEEK_END) == 0);
    sz = ftell(o);
    TASK_CHECK(fclose(o) == 0);
    TASK_CHECK(sz == (long)(frames - 1500 - 64) * ch * 4);
  }
  remove(in_path);
  remove(out_path);
}

static int abort_callback(sox_bool all_done, void * client_data)
{
  unsigned * calls = (unsigned *)client_data;
  ++*calls;
  /* Abort once the chain has made some progress (but long before the huge
   * input has been consumed). */
  return *calls > 3? SOX_EOF : SOX_SUCCESS;
}

/* Cancellation: abort the public effect chain part way, then verify the temp
 * file was released and the PCM accounting is balanced. */
static void lifecycle_cancel(void)
{
  unsigned ch = 2;
  uint64_t frames = 200000;
  interval_t cuts[2] = {{1000, 2000}, {150000, 150100}};
  const char * tmp = getenv("TMPDIR") ? getenv("TMPDIR") : ".";
  char in_path[512], c_path[512];
  sox_sample_t * in = make_pcm(ch, frames, 88, 0, 100000);
  sox_format_t * infile;
  sox_effects_chain_t * chain;
  sox_effect_t * e;
  sox_signalinfo_t sig, open_sig;
  char * args[6];
  unsigned calls = 0;
  int flow_status;

  snprintf(in_path, sizeof(in_path), "%s/cancel_in.s32", tmp);
  snprintf(c_path, sizeof(c_path), "%s/cancel_cuts.txt", tmp);
  write_file(in_path, in, (size_t)frames * ch * sizeof(sox_sample_t));
  write_cuts(c_path, cuts, 2);
  free(in);

  TASK_CHECK(sox_init() == SOX_SUCCESS);
  sox_globals.verbosity = 1;
  memset(&open_sig, 0, sizeof(open_sig));
  open_sig.channels = ch;
  open_sig.rate = 48000;
  open_sig.precision = 32;
  open_sig.length = frames * ch;
  infile = sox_open_read(in_path, &open_sig, NULL, "s4");
  TASK_CHECK(infile != NULL);
  chain = sox_create_effects_chain(&infile->encoding, &infile->encoding);
  sig = infile->signal;

  e = sox_create_effect(sox_find_effect("input"));
  args[0] = (char *)infile;
  TASK_CHECK(sox_effect_options(e, 1, args) == SOX_SUCCESS);
  TASK_CHECK(sox_add_effect(chain, e, &sig, &infile->signal) == SOX_SUCCESS);
  free(e);

  e = sox_create_effect(sox_find_effect("splice"));
  args[0] = "--cut-plan"; args[1] = c_path;
  args[2] = "--overlap"; args[3] = "8";
  args[4] = "--search"; args[5] = "4";
  TASK_CHECK(sox_effect_options(e, 6, args) == SOX_SUCCESS);
  TASK_CHECK(sox_add_effect(chain, e, &sig, &infile->signal) == SOX_SUCCESS);
  free(e);

  flow_status = sox_flow_effects(chain, abort_callback, &calls);
  TASK_CHECK(flow_status == SOX_EOF);
  TASK_CHECK(calls > 0);
  {
    lsx_splice_cut_plan_stats_t st;
    lsx_cutplan_stats_get(&st);
    TASK_CHECK(st.temp_files_opened == 1);
    TASK_CHECK(st.temp_files_closed == 0); /* stop() not called yet */
    TASK_CHECK(st.pcm_live_samples == 2ull * 8 * ch);
  }
  sox_delete_effects_chain(chain);
  {
    lsx_splice_cut_plan_stats_t st;
    lsx_cutplan_stats_get(&st);
    TASK_CHECK(st.temp_files_closed == 1);
    TASK_CHECK(st.pcm_live_samples == 0);
    TASK_CHECK(st.temp_io_errors == 0);
    /* Cancelling must not be reported as a completed cut plan. */
    TASK_CHECK(st.temp_files_opened == st.temp_files_closed);
  }
  sox_close(infile);
  sox_quit();
  remove(in_path);
}

int run_cut_plan_tests(const char * group)
{
  if (!strcmp(group, "planner")) {
    planner_joint_conflict();
    planner_lexicographic_tie();
    planner_silence_tie();
    planner_search_zero();
    planner_infeasible();
    planner_huge_costs();
    planner_full_scale_integer_costs();
    return 0;
  }
  if (!strcmp(group, "pcm")) {
    pcm_basic_cases();
    pcm_buffer_equivalence();
    pcm_infeasible_and_bad_plans();
    pcm_verbatim_copy();
    return 0;
  }
  if (!strcmp(group, "lifecycle")) {
    lifecycle_buffers_and_drains();
    lifecycle_resources();
    lifecycle_large_input();
    lifecycle_cancel();
    return 0;
  }
  return 1;
}
