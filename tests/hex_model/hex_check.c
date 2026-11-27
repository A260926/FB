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

#include "hex_check.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yara.h>
#include <yara/limits.h>

#include "hex_fixed.h"
#include "hex_scan.h"

static void stats_init(hm_run_stats* s)
{
  memset(s, 0, sizeof(*s));
}

static int fail(char* diag, size_t cap, const hm_case* c, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(diag, cap, fmt, ap);
  va_end(ap);
  if (c != NULL)
  {
    size_t pos = strlen(diag);
    snprintf(diag + pos, cap - pos, "case: %s\n", c->description);
  }
  return -1;
}

//
// Rejection cases. Each constructs a pattern that violates one rule of the
// supported subset and checks that the model refuses it; two of them also go
// through serialization to prove the deserialization path rejects malformed
// input. Refusal is an explicit error, never a zero-match result.
//
static int check_rejections(hm_arena* a, char* diag, size_t cap)
{
  hm_node* root;
  hm_node* alt;
  hm_node* branch;
  const char* reason;
  int checks = 0;

  // ~?? is not a legal token.
  root = hm_new_concat(a);
  hm_append(root, hm_new_not(a, 0, 0x00));
  if (hm_validate(root) == NULL)
    return fail(diag, cap, NULL, "model accepted ~??\n");
  checks++;

  // A pattern may not start with a jump.
  root = hm_new_concat(a);
  hm_append(root, hm_new_jump(a, 1, 3));
  hm_append(root, hm_new_byte(a, 0x41, 0xFF));
  if (hm_validate(root) == NULL)
    return fail(diag, cap, NULL, "model accepted leading jump\n");
  checks++;

  // A pattern may not end with a jump.
  root = hm_new_concat(a);
  hm_append(root, hm_new_byte(a, 0x41, 0xFF));
  hm_append(root, hm_new_jump(a, 1, 3));
  if (hm_validate(root) == NULL)
    return fail(diag, cap, NULL, "model accepted trailing jump\n");
  checks++;

  // Jumps are forbidden inside alternation branches.
  root = hm_new_concat(a);
  alt = hm_new_alt(a);
  branch = hm_new_concat(a);
  hm_append(branch, hm_new_jump(a, 1, 2));
  hm_append(alt, branch);
  branch = hm_new_concat(a);
  hm_append(branch, hm_new_byte(a, 0x41, 0xFF));
  hm_append(alt, branch);
  hm_append(root, alt);
  if (hm_validate(root) == NULL)
    return fail(diag, cap, NULL, "model accepted jump inside alternation\n");
  checks++;

  // Alternation branches must be equal width.
  root = hm_new_concat(a);
  alt = hm_new_alt(a);
  branch = hm_new_concat(a);
  hm_append(branch, hm_new_byte(a, 0x41, 0xFF));
  hm_append(alt, branch);
  branch = hm_new_concat(a);
  hm_append(branch, hm_new_byte(a, 0x42, 0xFF));
  hm_append(branch, hm_new_byte(a, 0x43, 0xFF));
  hm_append(alt, branch);
  hm_append(root, alt);
  if (hm_validate(root) == NULL)
    return fail(diag, cap, NULL, "model accepted unequal-width branches\n");
  checks++;

  // Jump endpoints are bounded by HM_MAX_JUMP.
  root = hm_new_concat(a);
  hm_append(root, hm_new_byte(a, 0x41, 0xFF));
  hm_append(root, hm_new_jump(a, 200, 203));
  hm_append(root, hm_new_byte(a, 0x42, 0xFF));
  if (hm_validate(root) == NULL)
    return fail(diag, cap, NULL, "model accepted jump beyond HM_MAX_JUMP\n");
  checks++;

  // Garbled serialization is rejected with a reason.
  uint8_t garbage[] = {99, 7, 1, 2, 3};
  hm_arena garbage_arena = {0};
  if (hm_deserialize(garbage, sizeof(garbage), &garbage_arena, &reason) != NULL)
  {
    hm_arena_free(&garbage_arena);
    return fail(diag, cap, NULL, "deserializer accepted unknown tag\n");
  }
  hm_arena_free(&garbage_arena);
  checks++;

  // A truncated serialization is rejected, not decoded into a partial tree.
  uint8_t truncated[] = {4, 2, 1, 255, 255};
  hm_arena trunc_arena = {0};
  if (hm_deserialize(truncated, sizeof(truncated), &trunc_arena, &reason) !=
      NULL)
  {
    hm_arena_free(&trunc_arena);
    return fail(diag, cap, NULL, "deserializer accepted truncated input\n");
  }
  hm_arena_free(&trunc_arena);
  checks++;

  if (checks != hm_fixed_rejection_checks())
    return fail(
        diag, cap, NULL, "rejection check count drift: %d vs %d\n", checks,
        hm_fixed_rejection_checks());

  return 0;
}

int hm_check_fixed(int threshold, hm_run_stats* stats, char* diag,
                   size_t cap)
{
  stats_init(stats);
  hm_arena arena = {0};
  hm_fixed_case fixed[HM_FIXED_COUNT];
  memset(fixed, 0, sizeof(fixed));
  hm_fixed_build(&arena, fixed);

  if (check_rejections(&arena, diag, cap) != 0)
  {
    hm_arena_free(&arena);
    return -1;
  }

  for (int i = 0; i < HM_FIXED_COUNT; i++)
  {
    hm_fixed_case* f = &fixed[i];
    stats->cases_run++;

    // Regression-test the model itself against the hand-computed table.
    hm_match got[HM_OBSERVED_MAX];
    int got_n =
        hm_expected(f->c.root, f->c.data, f->c.data_len, got, HM_OBSERVED_MAX);

    if (got_n != f->want_n)
    {
      int rc = fail(
          diag, cap, &f->c,
          "model/hand-table count drift: model=%d hand=%d\n", got_n,
          f->want_n);
      hm_arena_free(&arena);
      return rc;
    }
    for (int j = 0; j < got_n; j++)
    {
      if (got[j].offset != f->want[j].offset ||
          got[j].length != f->want[j].length)
      {
        int rc = fail(
            diag, cap, &f->c,
            "model/hand-table mismatch at %d: model=(%lld,%d) hand=(%lld,%d)\n",
            j, (long long)got[j].offset, got[j].length,
            (long long)f->want[j].offset, f->want[j].length);
        hm_arena_free(&arena);
        return rc;
      }
    }

    hm_scan_result res;
    int rc = hm_scan_check(&f->c, threshold, &res, diag, cap);

    if (rc == HM_SCAN_KNOWN_DIVERGENCE)
    {
      // The only tolerated divergence is the marked F13 class, and only on a
      // build where the string is actually split. Everything else is a hard
      // failure even if it happens to share the divergence shape.
      if (!f->known_divergence_t2 || res.chain_parts == 0)
      {
        int rc2 = fail(
            diag, cap, &f->c,
            "unexpected chain divergence on a case not marked F13\n");
        hm_arena_free(&arena);
        return rc2;
      }
      stats->divergences++;
    }
    else if (rc != HM_SCAN_OK)
    {
      hm_arena_free(&arena);
      return -1;
    }

    int expected_parts = threshold <= 2 ? f->expect_chain_parts_t2
                                        : f->expect_chain_parts_t200;
    if (res.chain_parts != expected_parts)
    {
      int rc2 = fail(
          diag, cap, &f->c,
          "chain-part flag drift: observed=%d expected=%d\n", res.chain_parts,
          expected_parts);
      hm_arena_free(&arena);
      return rc2;
    }

    stats->comparisons += res.comparisons;
    stats->chain_parts += res.chain_parts;
  }

  hm_arena_free(&arena);
  return 0;
}

int hm_check_generated(uint64_t seed, int cases, int threshold,
                       hm_run_stats* stats, char* diag, size_t cap)
{
  stats_init(stats);
  hm_rng rng = {.state = seed ? seed : 1};

  for (int i = 0; i < cases; i++)
  {
    hm_arena case_arena = {0};
    hm_case c;
    memset(&c, 0, sizeof(c));

    bool force_two = (i == 0);
    bool near_boundary = (i >= 1 && i <= 4);
    hm_generate(&rng, &case_arena, &c, force_two, near_boundary);

    if (hm_validate(c.root) != NULL)
    {
      int rc = fail(diag, cap, &c, "generator produced an invalid pattern\n");
      hm_arena_free(&case_arena);
      return rc;
    }

    hm_scan_result res;
    int rc = hm_scan_check(&c, threshold, &res, diag, cap);
    if (rc == HM_SCAN_KNOWN_DIVERGENCE)
      stats->divergences++;
    else if (rc != HM_SCAN_OK)
    {
      hm_arena_free(&case_arena);
      return -1;
    }

    stats->cases_run++;
    stats->comparisons += res.comparisons;
    stats->chain_parts += res.chain_parts;
    if (res.chain_parts == 3)
      stats->three_piece_cases++;

    if (force_two)
    {
      // The headline chaining-path proof: one pattern with two jumps, kept as
      // a single string at threshold 200 but split into three pieces at 2.
      if (threshold <= 2 && res.chain_parts != 3)
      {
        int rc2 = fail(
            diag, cap, &c,
            "two-jump pattern did not form a three-piece chain at threshold 2 "
            "(parts=%d)\n",
            res.chain_parts);
        hm_arena_free(&case_arena);
        return rc2;
      }
      if (threshold > 2 && res.chain_parts != 0)
      {
        int rc2 = fail(
            diag, cap, &c,
            "two-jump pattern was unexpectedly split at threshold 200 "
            "(parts=%d)\n",
            res.chain_parts);
        hm_arena_free(&case_arena);
        return rc2;
      }
      if (res.chain_parts == 0)
        stats->unsplit_long_cases++;
    }

    hm_arena_free(&case_arena);
  }

  return 0;
}

static uint64_t hash_input(const uint8_t* data, size_t size)
{
  uint64_t h = 0xCBF29CE484222325ULL;
  for (size_t i = 0; i < size; i++)
  {
    h ^= data[i];
    h *= 0x100000001B3ULL;
  }
  return h;
}

int hm_check_one_fuzz_input(const uint8_t* data, size_t size)
{
  if (size == 0 || size > 256)
    return 0;

  if (yr_initialize() != ERROR_SUCCESS)
    return 0;

  hm_arena arena = {0};
  const char* reason;
  hm_node* root = hm_deserialize(data, size, &arena, &reason);

  if (root == NULL)
  {
    // Explicit rejection of unsupported input: not a match failure and not a
    // fuzzer finding.
    hm_arena_free(&arena);
    yr_finalize();
    return 0;
  }

  hm_rng rng = {.state = hash_input(data, size) | 1};
  hm_case c;
  memset(&c, 0, sizeof(c));
  c.root = root;
  c.data_len = hm_rand_range(&rng, 32, HM_MAX_DATA);
  hm_fill_data(&rng, c.data, c.data_len);
  hm_plant_random(root, c.data, c.data_len, &rng);
  snprintf(c.description, sizeof(c.description), "fuzz");

  hm_scan_result res;
  char diag[4096];
  int rc = hm_scan_check(&c, YR_STRING_CHAINING_THRESHOLD, &res, diag,
                         sizeof(diag));

  // The documented chain-shadow class is an accepted, recorded limitation;
  // any other disagreement is a real finding.
  if (rc != HM_SCAN_OK && rc != HM_SCAN_KNOWN_DIVERGENCE)
  {
    fprintf(stderr, "%s", diag);
    fprintf(stderr, "serialized input:");
    for (size_t i = 0; i < size; i++)
      fprintf(stderr, " %02X", data[i]);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
  }

  hm_arena_free(&arena);
  yr_finalize();
  return 0;
}
