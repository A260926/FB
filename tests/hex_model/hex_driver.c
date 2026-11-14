/*
 * Ordinary deterministic driver. Provides main() and never links libFuzzer.
 * Runs the hand-checked fixed set, the unsupported-input rejection probes and
 * the seeded generated set in whatever library build it is linked against.
 *
 * Usage:
 *   hex_model_driver [--seed N] [--cases M] [--fixed-only]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yara.h>

#include "hex_runner.h"
#include "hex_scan.h"

int main(int argc, char** argv) {
  unsigned long seed = 1125;
  int cases = 240;
  int fixed_only = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = strtoul(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--cases") == 0 && i + 1 < argc) {
      cases = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--fixed-only") == 0) {
      fixed_only = 1;
    } else {
      fprintf(stderr, "unknown argument: %s\n", argv[i]);
      fprintf(stderr, "usage: %s [--seed N] [--cases M] [--fixed-only]\n",
              argv[0]);
      return 2;
    }
  }

  if (hm_scan_init() != ERROR_SUCCESS) {
    fprintf(stderr, "yr_initialize failed\n");
    return 1;
  }

  int threshold = hm_build_threshold();
  hm_stats stats;
  hm_stats_init(&stats);
  bool ok = true;

  printf("== fixed hand-computed cases (threshold=%d) ==\n", threshold);
  if (!hm_run_fixed(threshold, &stats)) ok = false;

  printf("== unsupported-input rejection probes ==\n");
  if (!hm_run_rejections(threshold, &stats)) ok = false;

  if (!fixed_only) {
    printf("== generated cases seed=%lu cases=%d ==\n", seed, cases);
    if (!hm_run_generated(seed, cases, threshold, &stats)) ok = false;
  }

  hm_stats_print("all", threshold, &stats);

  hm_scan_finalize();

  if (!ok || stats.failures != 0) {
    fprintf(stderr, "HEX MODEL FAILED: %d failure(s)\n", stats.failures);
    return 1;
  }
  printf("HEX_MODEL_PASS threshold=%d\n", threshold);
  return 0;
}
