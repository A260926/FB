/*
 * Shared checking core. A case is scanned through the public YARA API, the
 * published chain-head matches are compared field-by-field with the oracle,
 * and the observed chain fragment count is checked against the representation
 * this build must produce.
 */

#include "hex_check.h"

#include <stdio.h>

void hm_stats_init(hm_stats* s) {
  s->cases_run = 0;
  s->comparisons = 0;
  s->chain_parts = 0;
  s->failures = 0;
}

// Faithful simulation of the parser's repeated
// yr_re_ast_split_at_chaining_point() calls. Each pass walks the current
// outer-concatenation and splits at the FIRST jump that (a) crosses the
// threshold and (b) has both a previous and a next sibling. The remainder
// starts with the segment after the consumed jump, so a jump that lands at
// the head of a remainder can never itself split (no previous sibling).
// Returns the number of splits; the emitted string count is splits + 1.
static int count_chain_splits(const hm_node* root, int threshold) {
  if (root == NULL || root->type != HM_CONCAT) return 0;

  int splits = 0;
  int begin = 0;  // index in root->kids[] where the current remainder starts

  for (;;) {
    int found = -1;
    for (int i = begin + 1; i < root->n - 1; i++) {
      const hm_node* j = root->kids[i];
      if (j->type != HM_JUMP) continue;
      if (j->lo > threshold || j->hi > threshold) {
        found = i;
        break;
      }
    }
    if (found < 0) break;
    splits++;
    begin = found + 1;  // remainder begins right after the consumed jump
  }
  return splits;
}

int hm_expected_chain_parts(const hm_pattern* p, int threshold) {
  int splits = count_chain_splits(p->root, threshold);
  return splits == 0 ? 0 : splits + 1;
}

bool hm_core_run(hm_case* c, int threshold, int expected_parts, hm_stats* s) {
  hm_scan_result res;
  bool scanned = hm_scan_case(c, &res);
  s->cases_run++;

  if (!scanned) {
    fprintf(stderr, "case %s: scanner/compiler failure: %s (error=%d)\n",
            c->name, res.compiler_message, res.compiler_error);
    s->failures++;
    return false;
  }

  s->chain_parts += res.chain_parts;

  // Path-coverage assertion: the build must split exactly where the threshold
  // predicts. It never participates in oracle match judgements.
  if (res.chain_parts != expected_parts) {
    fprintf(stderr,
            "case %s: chain fragment mismatch at threshold=%d: "
            "expected %d parts, observed %d\n",
            c->name, threshold, expected_parts, res.chain_parts);
    s->failures++;
    return false;
  }

  // One count comparison, then offset and length for every paired match.
  s->comparisons++;
  int paired = c->expected_count;
  if (res.observed_count < paired) paired = res.observed_count;
  s->comparisons += 2 * paired;

  if (!hm_compare(c, threshold, &res)) {
    s->failures++;
    return false;
  }
  return true;
}

void hm_stats_print(const char* label, int threshold, const hm_stats* s) {
  printf(
      "SUMMARY label=%s threshold=%d cases=%d comparisons=%d "
      "chain_parts=%d failures=%d\n",
      label, threshold, s->cases_run, s->comparisons, s->chain_parts,
      s->failures);
}
