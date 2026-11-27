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

#ifndef HEX_CHECK_H
#define HEX_CHECK_H

#include <stddef.h>
#include <stdint.h>

#include "hex_model.h"

typedef struct {
  int cases_run;
  int comparisons;       // total model (offset, length) pairs checked
  int mismatches;
  int divergences;       // documented chain under-reports (F13 class)
  int chain_parts;       // total CHAIN_PART flags observed across cases
  int three_piece_cases; // cases compiled as exactly three chain pieces
  int unsplit_long_cases;// two-jump cases kept as one string (zero parts)
} hm_run_stats;

// Run the fixed, hand-computed regression set plus the rejection checks.
// Returns 0 on success; on failure *diag holds a replay-ready report.
int hm_check_fixed(int threshold, hm_run_stats* stats, char* diag,
                   size_t diag_cap);

// Run `cases` generated patterns from `seed`. The first case is forced to
// contain two long jumps and the following few straddle the 199..202
// boundary. Returns 0 on success; on failure *diag holds the report.
int hm_check_generated(uint64_t seed, int cases, int threshold,
                       hm_run_stats* stats, char* diag, size_t diag_cap);

// Fuzzer entry shared with LLVMFuzzerTestOneInput: interpret raw bytes as a
// serialized pattern when possible, otherwise derive a deterministic case
// from them. Always runs the full model-vs-library comparison. Returns 0.
int hm_check_one_fuzz_input(const uint8_t* data, size_t size);

#endif
