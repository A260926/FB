#include "task_engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Width-specific real library entry points (separate TUs because pcre2.h
 * is idempotent-guarded and binds to one width per inclusion). */
LibResult lib8_run(const uint32_t *pat, int plen,
                   const uint32_t *text, int tlen);
LibResult lib16_run(const uint32_t *pat, int plen,
                    const uint32_t *text, int tlen);
LibResult lib32_run(const uint32_t *pat, int plen,
                    const uint32_t *text, int tlen);

LibResult lib_run(int width, const uint32_t *pattern, int plen,
                  const uint32_t *text, int tlen) {
  if (width == 8) return lib8_run(pattern, plen, text, tlen);
  if (width == 16) return lib16_run(pattern, plen, text, tlen);
  LibResult r;
  memset(&r, 0, sizeof(r));
  r.width = 32;
  return lib32_run(pattern, plen, text, tlen);
}

/* ---------------- First-difference bookkeeping ------------------------- */

void diff_reset(FirstDiff *d) {
  d->case_index = -1;
  d->seed = 0;
  d->width = 0;
  d->msg[0] = 0;
}

static void diff_record(FirstDiff *diff, uint64_t seed, int case_index,
                        int width, const char *fmt, ...) {
  if (diff->case_index >= 0) return; /* keep the very first failure only */
  va_list ap;
  va_start(ap, fmt);
  diff->case_index = case_index;
  diff->seed = seed;
  diff->width = width;
  vsnprintf(diff->msg, sizeof(diff->msg), fmt, ap);
  va_end(ap);
}

int compare_results(uint64_t seed, int case_index, int width,
                    int oracle_matched, const size_t *oracle_ov,
                    int ngroups, const LibResult *lib, FirstDiff *diff) {
  if (lib->compile_error) {
    diff_record(diff, seed, case_index, width,
                "library rejected supported pattern: %s", lib->note);
    return 0;
  }
  if (lib->runtime_error) {
    diff_record(diff, seed, case_index, width,
                "untracked runtime return: %s", lib->note);
    return 0;
  }
  if (lib->boundary_violation) {
    diff_record(diff, seed, case_index, width,
                "an offset mapped inside a multi-unit code point");
    return 0;
  }
  if (lib->ngroups != ngroups) {
    diff_record(diff, seed, case_index, width,
                "capture count oracle=%d library=%d", ngroups,
                lib->ngroups);
    return 0;
  }
  if (lib->matched != oracle_matched) {
    diff_record(diff, seed, case_index, width,
                "match decision oracle=%d library=%d", oracle_matched,
                lib->matched);
    return 0;
  }
  if (!oracle_matched) return 1; /* both report no match */

  for (int g = 0; g <= ngroups; g++) {
    size_t olo = oracle_ov[2 * g], ohi = oracle_ov[2 * g + 1];
    size_t llo = lib->ovec[2 * g], lhi = lib->ovec[2 * g + 1];
    if (olo != llo || ohi != lhi) {
      const char *kind = g == 0 ? "whole" : "group";
      if (olo == SIZE_MAX || llo == SIZE_MAX) {
        diff_record(diff, seed, case_index, width,
                    "%s %d participation: oracle=%s library=%s", kind, g,
                    olo == SIZE_MAX ? "UNSET" : "set",
                    llo == SIZE_MAX ? "UNSET" : "set");
      } else {
        diff_record(diff, seed, case_index, width,
                    "%s %d span: oracle=[%lu,%lu) library=[%lu,%lu)", kind, g,
                    (unsigned long)olo, (unsigned long)ohi,
                    (unsigned long)llo, (unsigned long)lhi);
      }
      return 0;
    }
  }
  return 1;
}

/* ---------------- Coverage ---------------------------------------------- */

void coverage_init(Coverage *c) { memset(c, 0, sizeof(*c)); }

static void walk_counts(const Node *n, Coverage *c) {
  switch (n->kind) {
    case N_CLS:
      c->class_nodes++;
      return;
    case N_CAT:
      for (int i = 0; i < n->nkids; i++) walk_counts(n->kids[i], c);
      return;
    case N_ALT:
      c->branch_nodes++;
      for (int i = 0; i < n->nkids; i++) walk_counts(n->kids[i], c);
      return;
    case N_GRP:
      walk_counts(n->a, c);
      return;
    case N_REP:
      c->repetition_nodes++;
      walk_counts(n->a, c);
      return;
    case N_LIT:
      return;
  }
}

void coverage_record(Coverage *c, const Node *root,
                     const uint32_t *text, int tlen,
                     int oracle_matched, const size_t *oracle_ov,
                     const LibResult *r8, const LibResult *r16,
                     const LibResult *r32) {
  c->cases++;
  if (root->depth <= TASK_MAX_DEPTH) c->depth_hist[root->depth]++;
  int ng = ast_group_count(root);
  c->captures_hist[ng]++;
  for (int i = 0; i < tlen; i++)
    if (text[i] >= 0x10000u) c->supplementary_scalars++;
  if (oracle_matched) {
    c->matches++;
    for (int g = 1; g <= ng; g++) {
      if (oracle_ov[2 * g] == SIZE_MAX) c->unset_captures++;
      else if (oracle_ov[2 * g] == oracle_ov[2 * g + 1])
        c->empty_captures++;
    }
  } else {
    c->no_matches++;
  }
  walk_counts(root, c);
  if (r8->compile_error || r16->compile_error || r32->compile_error)
    c->compile_errors++;
  if (r8->boundary_violation || r16->boundary_violation ||
      r32->boundary_violation)
    c->boundary_violations++;
}

void coverage_print(const Coverage *c) {
  printf("COVERAGE cases=%ld\n", c->cases);
  printf("  depth:");
  for (int d = 1; d <= TASK_MAX_DEPTH; d++)
    printf(" d%d=%ld", d, c->depth_hist[d]);
  printf("\n  captures:");
  for (int g = 0; g <= TASK_MAX_GROUPS; g++)
    printf(" g%d=%ld", g, c->captures_hist[g]);
  printf("\n  supplementary_scalars=%ld empty_captures=%ld "
         "unset_captures=%ld\n",
         c->supplementary_scalars, c->empty_captures, c->unset_captures);
  printf("  branch_nodes=%ld repetition_nodes=%ld class_nodes=%ld\n",
         c->branch_nodes, c->repetition_nodes, c->class_nodes);
  printf("  oracle_matches=%ld oracle_no_matches=%ld compile_errors=%ld "
         "boundary_violations=%ld mismatches=%ld\n",
         c->matches, c->no_matches, c->compile_errors,
         c->boundary_violations, c->mismatches);
}

/* ---------------- Predetermined discrepancy simulator ------------------ */

/*
 * In-memory logic-difference simulator: it never touches upstream code.
 * A known-good pair is constructed, the oracle side is deliberately
 * corrupted, and the harness must (a) report the mismatch and (b) keep the
 * first failing case even when later cases also fail or succeed.
 */
int simulator_self_check(void) {
  FirstDiff diff;
  diff_reset(&diff);

  size_t good_ov[6] = {0, 2, 0, 1, SIZE_MAX, SIZE_MAX};
  size_t bad_ov[6];
  memcpy(bad_ov, good_ov, sizeof(good_ov));

  LibResult lib;
  memset(&lib, 0, sizeof(lib));
  lib.width = 8;
  lib.matched = 1;
  lib.ngroups = 2;
  memcpy(lib.ovec, good_ov, sizeof(good_ov));
  for (int i = 6; i < (TASK_MAX_GROUPS + 1) * 2; i++) lib.ovec[i] = SIZE_MAX;

  /* 1. Equal answers pass. */
  if (!compare_results(42, 0, 8, 1, good_ov, 2, &lib, &diff)) return 0;
  if (diff.case_index != -1) return 0;

  /* 2. Corrupt the span of group 1: the mismatch is detected. */
  bad_ov[3] = 2;
  if (compare_results(42, 7, 8, 1, bad_ov, 2, &lib, &diff)) return 0;
  if (diff.case_index != 7 || diff.width != 8) return 0;

  /* 3. A later failure at another case must not replace the first. */
  size_t bad2[6];
  memcpy(bad2, good_ov, sizeof(good_ov));
  bad2[0] = 1;
  if (compare_results(42, 9, 16, 1, bad2, 2, &lib, &diff)) return 0;
  if (diff.case_index != 7) return 0;

  /* 4. UNSET vs participated-empty is reported as a participation
   *    difference, not silently treated as equal. */
  FirstDiff d2;
  diff_reset(&d2);
  size_t empty_ov[6] = {0, 1, 0, 0, SIZE_MAX, SIZE_MAX};
  size_t unset_ov[6] = {0, 1, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
  LibResult lib2;
  memset(&lib2, 0, sizeof(lib2));
  lib2.matched = 1;
  lib2.ngroups = 2;
  memcpy(lib2.ovec, empty_ov, sizeof(empty_ov));
  for (int i = 6; i < (TASK_MAX_GROUPS + 1) * 2; i++)
    lib2.ovec[i] = SIZE_MAX;
  if (compare_results(42, 3, 32, 1, unset_ov, 2, &lib2, &d2)) return 0;
  if (d2.case_index != 3) return 0;

  /* 5. A boundary violation is surfaced even with equal scalar spans. */
  FirstDiff d3;
  diff_reset(&d3);
  LibResult lib3 = lib;
  lib3.boundary_violation = 1;
  if (compare_results(42, 4, 16, 1, good_ov, 2, &lib3, &d3)) return 0;
  if (d3.case_index != 4) return 0;

  return 1;
}
