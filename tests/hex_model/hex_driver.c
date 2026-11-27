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
// Fixed-seed driver. This translation unit never links libFuzzer; the fuzz
// entry point lives in hex_fuzz.c.
//
// Usage: hex_driver [--seed N] [--cases M] [--fixed-only]
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yara.h>
#include <yara/limits.h>

#include "hex_check.h"

static void usage(const char* prog)
{
  fprintf(
      stderr, "usage: %s [--seed N] [--cases M] [--fixed-only]\n", prog);
}

int main(int argc, char** argv)
{
  uint64_t seed = 1125;
  int cases = 240;
  int fixed_only = 0;

  for (int i = 1; i < argc; i++)
  {
    if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
      seed = strtoull(argv[++i], NULL, 10);
    else if (strcmp(argv[i], "--cases") == 0 && i + 1 < argc)
      cases = atoi(argv[++i]);
    else if (strcmp(argv[i], "--fixed-only") == 0)
      fixed_only = 1;
    else
    {
      usage(argv[0]);
      return 2;
    }
  }

  if (yr_initialize() != ERROR_SUCCESS)
  {
    fprintf(stderr, "yr_initialize failed\n");
    return 1;
  }

  char diag[8192];
  hm_run_stats fixed_stats;
  hm_run_stats gen_stats;

  if (hm_check_fixed(YR_STRING_CHAINING_THRESHOLD, &fixed_stats, diag,
                     sizeof(diag)) != 0)
  {
    fprintf(stderr, "%s", diag);
    yr_finalize();
    return 1;
  }

  if (!fixed_only &&
      hm_check_generated(
          seed, cases, YR_STRING_CHAINING_THRESHOLD, &gen_stats, diag,
          sizeof(diag)) != 0)
  {
    fprintf(stderr, "%s", diag);
    yr_finalize();
    return 1;
  }

  printf("HEX_MODEL build threshold=%d\n", YR_STRING_CHAINING_THRESHOLD);
  printf(
      "fixed: cases=%d comparisons=%d chain_parts=%d known_divergences=%d\n",
      fixed_stats.cases_run, fixed_stats.comparisons, fixed_stats.chain_parts,
      fixed_stats.divergences);

  if (!fixed_only)
  {
    printf(
        "generated seed=%llu cases=%d comparisons=%d chain_parts=%d "
        "three_piece_cases=%d unsplit_long_cases=%d known_divergences=%d\n",
        (unsigned long long)seed, gen_stats.cases_run, gen_stats.comparisons,
        gen_stats.chain_parts, gen_stats.three_piece_cases,
        gen_stats.unsplit_long_cases, gen_stats.divergences);
  }

  yr_finalize();
  return 0;
}
