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

#ifndef HEX_FIXED_H
#define HEX_FIXED_H

#include "hex_model.h"

#define HM_FIXED_WANT_MAX 16

typedef struct {
  hm_case c;
  hm_match want[HM_FIXED_WANT_MAX];  // hand-computed expected pairs
  int want_n;
  int expect_chain_parts_t2;         // chain parts under threshold 2
  int expect_chain_parts_t200;       // chain parts under threshold 200
  // Set for F13 only: the threshold-2 split chain under-reports this case
  // (documented engine limitation). The model's hand table stays the full
  // answer; any divergence on any other case is a hard failure.
  bool known_divergence_t2;
} hm_fixed_case;

// Build the fixed, hand-computable regression set into out (caller-owned,
// HM_FIXED_COUNT entries), allocating patterns from a.
#define HM_FIXED_COUNT 13
void hm_fixed_build(hm_arena* a, hm_fixed_case* out);

// Model-side rejection cases: patterns that violate the supported subset.
// They must be refused (validation/deserialization error), never silently
// treated as zero-match inputs. Returns the number of rejection checks.
int hm_fixed_rejection_checks(void);

#endif
