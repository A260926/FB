/*
 * Shared runner implementation. All randomness comes from the deterministic
 * splitmix64 generator; the same seed and case index produce byte-identical
 * patterns and inputs in both library builds.
 */

#include "hex_runner.h"

#include <stdio.h>
#include <string.h>

#include "hex_cases.h"

static bool hand_matches_oracle(const hm_fixed* f) {
  if (f->hand_count != f->c.expected_count) {
    fprintf(stderr,
            "fixed case %s: hand count %d differs from oracle count %d\n",
            f->name, f->hand_count, f->c.expected_count);
    return false;
  }
  for (int i = 0; i < f->hand_count; i++) {
    if (f->hand[i].start != f->c.expected[i].start ||
        f->hand[i].length != f->c.expected[i].length) {
      fprintf(stderr,
              "fixed case %s: hand[%d]=(%d,%d) oracle=(%d,%d)\n", f->name, i,
              f->hand[i].start, f->hand[i].length, f->c.expected[i].start,
              f->c.expected[i].length);
      return false;
    }
  }
  return true;
}

static bool same_sets(const hm_match* a, int an, const hm_match* b, int bn) {
  if (an != bn) return false;
  for (int i = 0; i < an; i++)
    if (a[i].start != b[i].start || a[i].length != b[i].length) return false;
  return true;
}

// Pinned real-difference case. The oracle answer (hand) is authoritative and
// is never adjusted. The harness accepts either the recorded buggy output
// (defect still present) or the correct output (defect fixed); anything else
// is a genuine new failure.
static bool run_pinned(hm_fixed* f, int threshold, hm_stats* stats) {
  hm_scan_result res;
  bool scanned = hm_scan_case(&f->c, &res);
  stats->cases_run++;
  if (!scanned) {
    fprintf(stderr, "pinned case %s: scanner failure: %s\n", f->name,
            res.compiler_message);
    stats->failures++;
    return false;
  }
  stats->chain_parts += res.chain_parts;
  stats->comparisons += 1 + 2 * f->hand_count;

  int want_parts = hm_expected_chain_parts(&f->c.pattern, threshold);
  if (res.chain_parts != want_parts) {
    fprintf(stderr,
            "pinned case %s: chain fragment mismatch: expected %d, got %d\n",
            f->name, want_parts, res.chain_parts);
    stats->failures++;
    return false;
  }

  if (same_sets(res.observed, res.observed_count, f->hand, f->hand_count)) {
    printf("PINNED %s: scanner now matches the oracle at threshold=%d; "
           "the documented defect appears fixed (update buggy[])\n",
           f->c.name, threshold);
    return true;
  }
  if (same_sets(res.observed, res.observed_count, f->buggy,
                f->buggy_count)) {
    printf("PINNED %s: known scanner defect present at threshold=%d "
           "(oracle expects %d match(es), scanner publishes %d)\n",
           f->c.name, threshold, f->hand_count, f->buggy_count);
    return true;
  }

  fprintf(stderr,
          "pinned case %s: scanner output matches neither the oracle nor the "
          "recorded buggy set\n",
          f->c.name);
  hm_compare(&f->c, threshold, &res);
  stats->failures++;
  return false;
}

bool hm_run_fixed(int threshold, hm_stats* stats) {
  hm_fixed fixed[32];
  int count = hm_build_fixed(fixed, (int) (sizeof(fixed) / sizeof(fixed[0])));
  bool ok = true;

  for (int i = 0; i < count; i++) {
    char err[HM_ERR_MAX];
    if (!hm_validate(&fixed[i].c.pattern, err, sizeof(err))) {
      fprintf(stderr, "fixed case %s failed model validation: %s\n",
              fixed[i].name, err);
      ok = false;
      stats->failures++;
      continue;
    }

    hm_case_prepare(&fixed[i].c, fixed[i].name);

    if (!hand_matches_oracle(&fixed[i])) {
      ok = false;
      stats->failures++;
      continue;
    }

    if (fixed[i].known_diff) {
      if (!run_pinned(&fixed[i], threshold, stats)) ok = false;
      continue;
    }

    int parts = hm_expected_chain_parts(&fixed[i].c.pattern, threshold);
    if (!hm_core_run(&fixed[i].c, threshold, parts, stats)) ok = false;
  }
  return ok;
}

bool hm_run_generated(unsigned long seed, int cases, int threshold,
                      hm_stats* stats) {
  bool ok = true;

  for (int i = 0; i < cases; i++) {
    hm_rng r;
    hm_rng_seed(&r, (uint64_t) seed * 1000003ULL + (uint64_t) i + 1);

    hm_case c;
    memset(&c, 0, sizeof(c));
    hm_generate_pattern(&r, &c.pattern);

    char err[HM_ERR_MAX];
    if (!hm_validate(&c.pattern, err, sizeof(err))) {
      fprintf(stderr, "generated case %d produced an invalid pattern: %s\n", i,
              err);
      ok = false;
      stats->failures++;
      continue;
    }

    // Roughly one in four buffers is fully random (frequently zero matches);
    // the rest contain a planted derivation so at least one match exists.
    bool planted = (hm_rand_range(&r, 0, 3) != 0);
    int len = -1;
    for (int attempt = 0; attempt < 8 && len < 0; attempt++) {
      if (planted)
        len = hm_plant_data(&r, &c.pattern, c.data, HM_MAX_DATA);
      else
        len = hm_random_data(&r, c.data, HM_MAX_DATA);
    }
    if (len < 0) {
      fprintf(stderr, "generated case %d could not build input data\n", i);
      ok = false;
      stats->failures++;
      continue;
    }
    c.data_len = len;

    char name[40];
    snprintf(name, sizeof(name), "generated_%d", i);
    hm_case_prepare(&c, name);

    int parts = hm_expected_chain_parts(&c.pattern, threshold);
    if (!hm_core_run(&c, threshold, parts, stats)) ok = false;
  }
  return ok;
}

bool hm_run_rejections(int threshold, hm_stats* stats) {
  bool ok = true;

  for (int i = 0; i < hm_invalid_count(); i++) {
    hm_pattern p;
    hm_build_invalid(&p, i);

    char err[HM_ERR_MAX];
    if (hm_validate(&p, err, sizeof(err))) {
      fprintf(stderr,
              "rejection probe %d: unsupported pattern was accepted by the "
              "model\n",
              i);
      ok = false;
      stats->failures++;
      continue;
    }
    stats->cases_run++;
  }

  // Grammar-illegal bodies must also be refused by the YARA compiler. These
  // three constructions are invalid hex strings in their own right; the
  // other rejected patterns are legal YARA but outside this test's surface.
  static const char* grammar_illegal[] = {
      "{ [2-3] 41 }",      // jump at the start
      "{ 41 [2-3] }",      // jump at the end
      "{ ~?? 42 }",        // negated wildcard
  };
  for (int i = 0; i < (int) (sizeof(grammar_illegal) /
                            sizeof(grammar_illegal[0]));
       i++) {
    if (!hm_expect_rejected(grammar_illegal[i], threshold)) {
      ok = false;
      stats->failures++;
    }
  }
  return ok;
}

int hm_fuzz_one(const unsigned char* data, unsigned long size) {
  // Fold the arbitrary input into a deterministic seed and a mode bit.
  uint64_t seed = 0x9E3779B97F4A7C15ULL;
  for (unsigned long i = 0; i < size; i++)
    seed = seed * 1099511628211ULL ^ (uint64_t) data[i];

  hm_rng r;
  hm_rng_seed(&r, seed);

  hm_case c;
  memset(&c, 0, sizeof(c));
  hm_generate_pattern(&r, &c.pattern);

  char err[HM_ERR_MAX];
  if (!hm_validate(&c.pattern, err, sizeof(err))) return 0;

  bool plant = (hm_rand_range(&r, 0, 1) == 0);
  int len = plant ? hm_plant_data(&r, &c.pattern, c.data, HM_MAX_DATA)
                  : hm_random_data(&r, c.data, HM_MAX_DATA);
  if (len < 0) return 0;
  c.data_len = len;

  hm_case_prepare(&c, "fuzz");
  hm_stats stats;
  hm_stats_init(&stats);
  int threshold = hm_build_threshold();
  int parts = hm_expected_chain_parts(&c.pattern, threshold);
  hm_core_run(&c, threshold, parts, &stats);
  // Non-zero makes hex_fuzz.c abort so libFuzzer keeps the crashing input.
  return stats.failures;
}
