/*
 * YARA scan adapter. The same generated cases are fed through the public
 * in-memory scanning path (yr_compiler_add_string -> yr_compiler_get_rules
 * -> yr_rules_scan_mem) in two separate library builds. This adapter is the
 * only component that touches YARA; the oracle in hex_model.c does not.
 */

#ifndef HEX_SCAN_H
#define HEX_SCAN_H

#include "hex_model.h"

typedef struct {
  int chain_parts;   // STRING_IS_CHAIN_PART strings; path-coverage only
  int scan_error;    // non-zero when yr_rules_scan_mem failed
  int compiler_error;
  char compiler_message[HM_ERR_MAX];
  hm_match observed[HM_MAX_DATA];
  int observed_count;
} hm_scan_result;

// One-time process initialization.
int hm_scan_init(void);
void hm_scan_finalize(void);

// The YR_STRING_CHAINING_THRESHOLD this translation unit was compiled with.
int hm_build_threshold(void);

// Compiles and scans one prepared case. Returns true when the scan itself
// ran; use hm_compare() to judge the observed matches against the oracle.
bool hm_scan_case(const hm_case* c, hm_scan_result* res);

// Field-by-field comparison of observed matches with the oracle. On failure,
// emits a replay-ready report (pattern structure, rule, input hex, build
// threshold and the first differing offset/length) and returns false.
bool hm_compare(const hm_case* c, int threshold, const hm_scan_result* res);

// Asserts that a hex body the model refused is also rejected by the compiler.
// A compile failure here is the expected outcome; successful compilation is
// treated as a test failure. Returns false on mismatch.
bool hm_expect_rejected(const char* hex_body, int threshold);

#endif  // HEX_SCAN_H
