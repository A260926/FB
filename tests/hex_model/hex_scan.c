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

#include "hex_scan.h"

#include <stdio.h>
#include <string.h>
#include <yara.h>

typedef struct {
  hm_scan_result* res;
  bool overflow;
} callback_state;

static int scan_callback(YR_SCAN_CONTEXT* ctx, int msg, void* data, void* user)
{
  if (msg != CALLBACK_MSG_RULE_MATCHING)
    return CALLBACK_CONTINUE;

  callback_state* st = (callback_state*)user;
  YR_RULE* rule = (YR_RULE*)data;
  YR_STRING* s;
  YR_MATCH* m;

  yr_rule_strings_foreach(rule, s)
  {
    // Only the head of a string chain (or a non-split string) holds the
    // published full matches; intermediate chain parts keep unconfirmed
    // matches and must not be counted as separate results.
    if (s->chained_to != NULL)
      continue;

    yr_string_matches_foreach(ctx, s, m)
    {
      if (st->res->observed_n >= HM_OBSERVED_MAX)
      {
        st->overflow = true;
        return CALLBACK_ABORT;
      }
      st->res->observed[st->res->observed_n].offset = m->base + m->offset;
      st->res->observed[st->res->observed_n].length = m->match_length;
      st->res->observed_n++;
    }
  }

  return CALLBACK_CONTINUE;
}

static void append_hex_dump(char* diag, size_t cap, const uint8_t* data,
                            int len)
{
  size_t pos = strlen(diag);
  for (int i = 0; i < len && pos + 4 < cap; i++)
  {
    pos += (size_t)snprintf(
        diag + pos, cap - pos, "%02X%s", data[i],
        (i + 1 == len) ? "" : " ");
  }
}

static void append_match_list(char* diag, size_t cap, const char* label,
                              const hm_match* m, int n)
{
  size_t pos = strlen(diag);
  pos += (size_t)snprintf(diag + pos, cap - pos, "%s (%d):", label, n);
  for (int i = 0; i < n && pos < cap - 1; i++)
    pos += (size_t)snprintf(
        diag + pos, cap - pos, " [off=%lld len=%d]",
        (long long)m[i].offset, m[i].length);
  snprintf(diag + pos, cap - pos, "\n");
}

// Returns 1 when the compiler accepts the rule, 0 when it rejects it. Used by
// the rejection tests: unsupported pattern text must fail compilation rather
// than being silently treated as a non-matching pattern.
int hm_rule_compiles(const char* rule)
{
  YR_COMPILER* compiler = NULL;
  if (yr_compiler_create(&compiler) != ERROR_SUCCESS)
    return -1;
  int rc = yr_compiler_add_string(compiler, rule, NULL);
  yr_compiler_destroy(compiler);
  return rc == 0;
}

int hm_scan_check(const hm_case* c, int threshold, hm_scan_result* res,
                  char* diag, size_t diag_cap)
{
  memset(res, 0, sizeof(*res));
  diag[0] = '\0';

  char rule[1024];
  hm_render_rule(c->root, rule, sizeof(rule));

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;

  if (yr_compiler_create(&compiler) != ERROR_SUCCESS)
  {
    snprintf(diag, diag_cap, "yr_compiler_create failed\n");
    return HM_SCAN_COMPILE_FAILED;
  }

  if (yr_compiler_add_string(compiler, rule, NULL) != 0)
  {
    char msg[256];
    yr_compiler_get_error_message(compiler, msg, sizeof(msg));
    snprintf(
        diag, diag_cap,
        "pattern rejected by compiler although model validated it:\n"
        "rule: %s\nerror: %s\n",
        rule, msg);
    yr_compiler_destroy(compiler);
    return HM_SCAN_COMPILE_FAILED;
  }

  if (yr_compiler_get_rules(compiler, &rules) != ERROR_SUCCESS)
  {
    snprintf(diag, diag_cap, "yr_compiler_get_rules failed\n");
    yr_compiler_destroy(compiler);
    return HM_SCAN_RULES_FAILED;
  }
  yr_compiler_destroy(compiler);

  // Read the chain-part flags. This is diagnostic evidence only: the value is
  // reported and cross-checked between builds, never used by the model.
  YR_RULE* rule_ptr;
  YR_STRING* string_ptr;
  yr_rules_foreach(rules, rule_ptr)
  {
    yr_rule_strings_foreach(rule_ptr, string_ptr)
    {
      if (STRING_IS_CHAIN_PART(string_ptr))
        res->chain_parts++;
    }
  }

  callback_state st = {.res = res, .overflow = false};
  int scan_rc = yr_rules_scan_mem(
      rules, c->data, (size_t)c->data_len, 0, scan_callback, &st, 0);

  if (scan_rc != ERROR_SUCCESS)
  {
    snprintf(diag, diag_cap, "yr_rules_scan_mem failed: %d\n", scan_rc);
    yr_rules_destroy(rules);
    return HM_SCAN_SCAN_FAILED;
  }
  if (st.overflow)
  {
    snprintf(
        diag, diag_cap, "more than %d observed matches\n", HM_OBSERVED_MAX);
    yr_rules_destroy(rules);
    return HM_SCAN_TOO_MANY_OBSERVED;
  }

  hm_match expected[HM_OBSERVED_MAX];
  int expected_n =
      hm_expected(c->root, c->data, c->data_len, expected, HM_OBSERVED_MAX);
  res->comparisons = expected_n;

  int outcome = HM_SCAN_OK;
  int first_diff = -1;
  bool is_strict_subset = false;

  if (expected_n == res->observed_n)
  {
    for (int i = 0; i < expected_n; i++)
    {
      if (expected[i].offset != res->observed[i].offset ||
          expected[i].length != res->observed[i].length)
      {
        outcome = HM_SCAN_MISMATCH;
        first_diff = i;
        break;
      }
    }
  }
  else
  {
    outcome = HM_SCAN_MISMATCH;
    first_diff = 0;

    // Recognize the single documented engine limitation: the library may
    // under-report (never over-report, never change an offset or length) and
    // only on a split chain. Verify the observed list is a strict,
    // order-preserving subsequence of identical expected pairs.
    if (res->observed_n < expected_n && res->chain_parts > 0)
    {
      int ei = 0;
      is_strict_subset = true;
      for (int oi = 0; oi < res->observed_n; oi++)
      {
        while (ei < expected_n &&
               (expected[ei].offset != res->observed[oi].offset ||
                expected[ei].length != res->observed[oi].length))
          ei++;
        if (ei >= expected_n)
        {
          is_strict_subset = false;
          break;
        }
        ei++;
      }
      if (is_strict_subset)
      {
        outcome = HM_SCAN_KNOWN_DIVERGENCE;
        res->divergence = 1;
      }
    }
  }

  if (outcome != HM_SCAN_OK)
  {
    snprintf(
        diag, diag_cap,
        "HEX MODEL %s\ncase: %s\nthreshold: %d\nrule: %s\n",
        outcome == HM_SCAN_KNOWN_DIVERGENCE
            ? "KNOWN DIVERGENCE (documented chain under-report, F13)"
            : "MISMATCH",
        c->description, threshold, rule);
    hm_dump_str(c->root, diag, diag_cap, 0);

    size_t pos = strlen(diag);
    pos += (size_t)snprintf(
        diag + pos, diag_cap - pos, "input (%d bytes): ", c->data_len);
    append_hex_dump(diag, diag_cap, c->data, c->data_len);
    snprintf(diag + strlen(diag), diag_cap - strlen(diag), "\n");

    append_match_list(diag, diag_cap, "expected", expected, expected_n);
    append_match_list(
        diag, diag_cap, "observed", res->observed, res->observed_n);

    if (first_diff >= 0 && outcome == HM_SCAN_MISMATCH &&
        expected_n == res->observed_n)
    {
      snprintf(
          diag + strlen(diag), diag_cap - strlen(diag),
          "first difference at pair %d: expected offset=%lld length=%d, "
          "observed offset=%lld length=%d\n",
          first_diff, (long long)expected[first_diff].offset,
          expected[first_diff].length,
          (long long)res->observed[first_diff].offset,
          res->observed[first_diff].length);
    }
    else
    {
      snprintf(
          diag + strlen(diag), diag_cap - strlen(diag),
          "first difference is the match count (expected=%d observed=%d)\n",
          expected_n, res->observed_n);
    }
  }

  yr_rules_destroy(rules);
  return outcome;
}
