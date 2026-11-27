/*
Copyright (c) 2026. The YARA Authors. All Rights Reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

//
// Scan adapter: renders a model pattern into a YARA rule, compiles it through
// yr_compiler_add_string / yr_compiler_get_rules, scans the benign buffer with
// yr_rules_scan_mem and compares every match published at the head of the
// string chain against the model's expected (offset, match_length) pairs.
//
// The chain-part count is read from the compiled strings' flags purely to
// prove which chaining path was taken; it never feeds the model's judgement.
//
#ifndef HEX_SCAN_H
#define HEX_SCAN_H

#include <stddef.h>

#include "hex_model.h"

#define HM_OBSERVED_MAX 1100

typedef struct {
  int comparisons;   // model (offset, length) pairs for this case
  int observed_n;    // pairs the library actually published
  int chain_parts;   // compiled strings with STRING_FLAGS_CHAIN_PART
  int divergence;    // 1 only for the documented chain-shadow under-report
  hm_match observed[HM_OBSERVED_MAX];
} hm_scan_result;

// Outcome codes. HM_SCAN_OK means every published pair equals the model.
enum {
  HM_SCAN_OK = 0,
  HM_SCAN_COMPILE_FAILED = 1,
  HM_SCAN_RULES_FAILED = 2,
  HM_SCAN_SCAN_FAILED = 3,
  HM_SCAN_MISMATCH = 4,
  HM_SCAN_TOO_MANY_OBSERVED = 5,
  // The single documented engine limitation: on a split string chain, an
  // earlier short match of a middle piece shadows a longer match at the same
  // offset; when the short match is negatively confirmed and removed, a chain
  // head that was only reachable through the longer match is lost, so the
  // library publishes a strict, order-preserving subset of the model's
  // matches. Recorded by fixed regression F13; the model is never adjusted.
  HM_SCAN_KNOWN_DIVERGENCE = 6
};

// Run one case against the library linked into this binary. diag receives a
// replay-ready report (pattern structure, rule text, data hex, threshold and
// the first differing pair) whenever the outcome is not HM_SCAN_OK.
int hm_scan_check(const hm_case* c, int threshold, hm_scan_result* res,
                  char* diag, size_t diag_cap);

// 1 when the rule text compiles, 0 when it is rejected. Rejection is a
// distinct outcome from a pattern that compiles but produces no match.
int hm_rule_compiles(const char* rule);

#endif
