/*
 * Shared checking core used by both the deterministic driver and the
 * libFuzzer entry point. A single case is compiled, scanned in memory and
 * compared field-by-field against the independent oracle.
 */

#ifndef HEX_CHECK_H
#define HEX_CHECK_H

#include "hex_model.h"
#include "hex_scan.h"

typedef struct {
  int cases_run;
  int comparisons;     // offset/length field comparisons actually performed
  int chain_parts;     // summed STRING_IS_CHAIN_PART counts (coverage only)
  int failures;
} hm_stats;

void hm_stats_init(hm_stats* s);

// Runs one prepared case. expected_parts is the chain fragment count this
// build must expose for the pattern; it only proves the code path taken.
// Returns false (and records the failure) on any mismatch.
bool hm_core_run(hm_case* c, int threshold, int expected_parts, hm_stats* s);

// Predicts the STRING_IS_CHAIN_PART count for a pattern at this threshold.
// Mirrors yr_re_ast_split_at_chaining_point() without calling into YARA.
int hm_expected_chain_parts(const hm_pattern* p, int threshold);

// Prints the per-build summary line consumed by tests/hex_model/check.sh.
void hm_stats_print(const char* label, int threshold, const hm_stats* s);

#endif  // HEX_CHECK_H
