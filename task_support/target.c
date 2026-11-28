#include "check.h"
#include "task_ast.h"
#include "task_engine.h"

#include <stdio.h>
#include <string.h>

/*
 * Replayable property testing driver.
 *
 * Modules are independent on purpose:
 *   task_rng_ast.c  - deterministic PRNG, Unicode-scalar AST generator,
 *                     scalar serializer and metamorphic transformations
 *   task_oracle.c   - reference backtracking interpreter over scalars
 *   task_utf.c      - UTF-8/16/32 encoders and boundary tables
 *   task_run8/16/32.c - real pcre2_compile/pcre2_match at each width
 *   task_engine.c   - comparison, coverage and discrepancy simulation
 *
 * A fixed seed reproduces every case byte-for-byte.
 */

#define PAT_CAP 4096
#define FINGER_CAP 600

typedef struct {
  uint64_t seed;
  uint32_t pat_hash;
  uint32_t text_hash;
  int oracle_matched;
  int ngroups;
  size_t oracle_ov[(TASK_MAX_GROUPS + 1) * 2];
  LibResult r8, r16, r32;
} Fingerprint;

static uint32_t fnv_scalars(const uint32_t *s, int n) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) {
    h ^= s[i] & 0xffu;
    h *= 16777619u;
    h ^= (s[i] >> 8) & 0xffu;
    h *= 16777619u;
    h ^= (s[i] >> 16) & 0xffu;
    h *= 16777619u;
    h ^= (s[i] >> 24) & 0xffu;
    h *= 16777619u;
  }
  return h;
}

static int group_count(const Node *root) { return ast_group_count(root); }

static void fmt_ovec(char *out, int cap, int matched, int ng,
                     const size_t *ov) {
  int pos = 0;
  if (!matched) {
    snprintf(out, (size_t)cap, "NOMATCH");
    return;
  }
  for (int g = 0; g <= ng; g++) {
    size_t lo = ov[2 * g], hi = ov[2 * g + 1];
    int m;
    if (lo == SIZE_MAX)
      m = snprintf(out + pos, (size_t)(cap - pos), "%s-", g ? "g" : "m");
    else
      m = snprintf(out + pos, (size_t)(cap - pos), "%s[%lu,%lu)",
                   g ? "g" : "m", (unsigned long)lo, (unsigned long)hi);
    if (m < 0 || pos + m >= cap) break;
    pos += m;
  }
}

/* Generate one case, run oracle and all three widths, compare, print. */
static int run_one(uint64_t seed, Coverage *cov, FirstDiff *diff,
                   Fingerprint *fp) {
  TaskRng rng;
  rng_seed(&rng, seed);

  int ng_gen = 0;
  Node *ast = ast_generate(&rng, &ng_gen);
  TASK_CHECK(ast != NULL);
  TASK_CHECK(ast->depth <= TASK_MAX_DEPTH);
  TASK_CHECK(ng_gen >= 1 && ng_gen <= TASK_MAX_GROUPS);

  uint32_t pattern[PAT_CAP];
  int plen = ast_serialize(ast, pattern, PAT_CAP);
  TASK_CHECK(plen > 0 && plen < PAT_CAP);

  uint32_t text[TASK_MAX_TEXT + 1];
  int tlen = gen_text(&rng, text, TASK_MAX_TEXT);
  TASK_CHECK(tlen >= 0 && tlen <= TASK_MAX_TEXT);

  int ng = group_count(ast);
  TASK_CHECK(ng == ng_gen);

  size_t ovec[(TASK_MAX_GROUPS + 1) * 2];
  memset(ovec, 0, sizeof(ovec));
  int om = oracle_match(ast, text, tlen, ovec,
                        (TASK_MAX_GROUPS + 1) * 2);
  TASK_CHECK(om >= 0);

  LibResult r8 = lib_run(8, pattern, plen, text, tlen);
  LibResult r16 = lib_run(16, pattern, plen, text, tlen);
  LibResult r32 = lib_run(32, pattern, plen, text, tlen);

  int ok8 = compare_results(seed, (int)(cov ? cov->cases : 0), 8, om, ovec,
                            ng, &r8, diff);
  int ok16 = compare_results(seed, (int)(cov ? cov->cases : 0), 16, om,
                             ovec, ng, &r16, diff);
  int ok32 = compare_results(seed, (int)(cov ? cov->cases : 0), 32, om,
                             ovec, ng, &r32, diff);

  char astbuf[768];
  ast_render(ast, astbuf, sizeof(astbuf));
  char textbuf[512];
  render_scalars(text, tlen, textbuf, sizeof(textbuf));
  char s8[160], s16[160], s32[160];
  fmt_ovec(s8, sizeof(s8), r8.matched, ng, r8.ovec);
  fmt_ovec(s16, sizeof(s16), r16.matched, ng, r16.ovec);
  fmt_ovec(s32, sizeof(s32), r32.matched, ng, r32.ovec);
  printf("CASE seed=%llu AST=%s TEXT=(%d)%s\n",
         (unsigned long long)seed, astbuf, tlen, textbuf);
  printf("     oracle=%s w8=%s w16=%s w32=%s diff=%s\n",
         om ? "MATCH" : "NOMATCH", s8, s16, s32,
         diff->case_index >= 0 ? diff->msg : "none");
  if (r8.compile_error) printf("     w8 compile: %s\n", r8.note);
  if (r16.compile_error) printf("     w16 compile: %s\n", r16.note);
  if (r32.compile_error) printf("     w32 compile: %s\n", r32.note);

  if (cov) {
    coverage_record(cov, ast, text, tlen, om, ovec, &r8, &r16, &r32);
    if (!ok8 || !ok16 || !ok32) cov->mismatches++;
  }
  if (fp) {
    fp->seed = seed;
    fp->pat_hash = fnv_scalars(pattern, plen);
    fp->text_hash = fnv_scalars(text, tlen);
    fp->oracle_matched = om;
    fp->ngroups = ng;
    memcpy(fp->oracle_ov, ovec, sizeof(fp->oracle_ov));
    fp->r8 = r8;
    fp->r16 = r16;
    fp->r32 = r32;
  }

  ast_free(ast);
  return ok8 && ok16 && ok32;
}

#define REPLAY_BASE 0x5eed000000000001ULL

static int run_suite(int n, Coverage *cov, FirstDiff *diff,
                     Fingerprint *prints) {
  int all_ok = 1;
  for (int i = 0; i < n; i++) {
    Fingerprint *fp = prints ? &prints[i] : NULL;
    int ok = run_one(REPLAY_BASE + (uint64_t)i, cov, diff, fp);
    if (!ok) all_ok = 0;
  }
  return all_ok;
}

/* ---------------- Hand-built model cases ------------------------------- */

static void hand_case(const char *name, Node *ast, const uint32_t *text,
                      int tlen, int exp_matched, size_t exp_lo,
                      size_t exp_hi, int check_g1, size_t exp_g1lo,
                      size_t exp_g1hi) {
  uint32_t pattern[PAT_CAP];
  int plen = ast_serialize(ast, pattern, PAT_CAP);
  TASK_CHECK(plen > 0);
  int ng = group_count(ast);
  size_t ovec[(TASK_MAX_GROUPS + 1) * 2];
  memset(ovec, 0, sizeof(ovec));
  int om = oracle_match(ast, text, tlen, ovec,
                        (TASK_MAX_GROUPS + 1) * 2);
  TASK_CHECK(om == exp_matched);
  if (exp_matched) {
    TASK_CHECK(ovec[0] == exp_lo && ovec[1] == exp_hi);
    if (check_g1) TASK_CHECK(ovec[2] == exp_g1lo && ovec[3] == exp_g1hi);
  }
  LibResult r8 = lib_run(8, pattern, plen, text, tlen);
  LibResult r16 = lib_run(16, pattern, plen, text, tlen);
  LibResult r32 = lib_run(32, pattern, plen, text, tlen);
  FirstDiff d;
  diff_reset(&d);
  TASK_CHECK(compare_results(1, 0, 8, om, ovec, ng, &r8, &d));
  TASK_CHECK(compare_results(1, 0, 16, om, ovec, ng, &r16, &d));
  TASK_CHECK(compare_results(1, 0, 32, om, ovec, ng, &r32, &d));
  printf("HAND %s: ok (all three widths agree with the oracle)\n", name);
  ast_free(ast);
}

static int group_model(void) {
  Coverage cov;
  FirstDiff diff;
  coverage_init(&cov);
  diff_reset(&diff);

  /* 1. The generated corpus is non-empty and nothing is filtered away. */
  int ok = run_suite(TASK_CASES_DEFAULT, &cov, &diff, NULL);
  TASK_CHECK(cov.cases == TASK_CASES_DEFAULT);

  /* 2. Every supported pattern compiles: compile errors are surfaced,
   *    never swallowed, and none are expected for this grammar. */
  TASK_CHECK(cov.compile_errors == 0);

  /* 3. No code-unit offset ever maps inside a code point. */
  TASK_CHECK(cov.boundary_violations == 0);

  /* 4. Oracle and all three widths agree on every case, including
   *    no-match decisions and UNSET-vs-empty capture distinctions. */
  TASK_CHECK(ok);
  TASK_CHECK(cov.mismatches == 0);
  TASK_CHECK(diff.case_index == -1);

  /* 5. Both outcomes occur, so no-match paths are really compared. */
  TASK_CHECK(cov.matches > 0 && cov.no_matches > 0);

  /* 6. The pools and grammar exercise every required feature. */
  TASK_CHECK(cov.supplementary_scalars > 0);
  TASK_CHECK(cov.branch_nodes > 0);
  TASK_CHECK(cov.repetition_nodes > 0);
  TASK_CHECK(cov.class_nodes > 0);
  TASK_CHECK(cov.empty_captures > 0 || cov.unset_captures > 0);

  coverage_print(&cov);

  /* Hand-verified semantics of the independent interpreter. */
  {
    uint32_t t1[] = {'b', 'a'};
    hand_case("literal-group", n_grp(n_lit('a'), 1), t1, 2, 1, 1, 2, 1, 1,
              2);
  }
  {
    uint32_t cps[] = {'x', 0x00e9u, 0x1f600u};
    uint32_t t[] = {0x1f600u};
    hand_case("explicit-class", n_grp(n_cls(cps, 3), 1), t, 1, 1, 0, 1, 1,
              0, 1);
  }
  {
    Node **kids = malloc(2 * sizeof(Node *));
    kids[0] = n_lit('a');
    kids[1] = n_lit('b');
    uint32_t t[] = {'a'};
    hand_case("leftmost-alternation", n_alt(kids, 2), t, 1, 1, 0, 1, 0, 0,
              0);
  }
  {
    Node **kids = malloc(2 * sizeof(Node *));
    kids[0] = n_rep(n_lit('a'), 1, 4);
    kids[1] = n_lit('a');
    uint32_t t[] = {'a', 'a', 'a', 'a'};
    hand_case("greedy-backtrack", n_cat(kids, 2), t, 4, 1, 0, 4, 0, 0, 0);
  }
  {
    /* Branch not taken: group 1 never participates -> UNSET. */
    Node **kids = malloc(2 * sizeof(Node *));
    kids[0] = n_grp(n_lit('b'), 1);
    kids[1] = n_lit('a');
    uint32_t t[] = {'a'};
    hand_case("unset-capture", n_alt(kids, 2), t, 1, 1, 0, 1, 1, SIZE_MAX,
              SIZE_MAX);
  }
  {
    /* Group participates and matches the empty string: [0,0), not UNSET. */
    Node **kids = malloc(2 * sizeof(Node *));
    kids[0] = n_grp(n_rep(n_lit('b'), 0, 0), 1);
    kids[1] = n_lit('a');
    uint32_t t[] = {'a'};
    hand_case("empty-capture", n_cat(kids, 2), t, 1, 1, 0, 1, 1, 0, 0);
  }
  return 0;
}

/* ---------------- Metamorphic group ------------------------------------ */

static int patterns_equiv_oracle(const Node *a, const Node *b,
                                 const uint32_t *text, int tlen) {
  size_t oa[(TASK_MAX_GROUPS + 1) * 2];
  size_t ob[(TASK_MAX_GROUPS + 1) * 2];
  int ma = oracle_match(a, text, tlen, oa, (TASK_MAX_GROUPS + 1) * 2);
  int mb = oracle_match(b, text, tlen, ob, (TASK_MAX_GROUPS + 1) * 2);
  if (ma != mb) return 0;
  if (!ma) return 1;
  int na = group_count(a);
  for (int i = 0; i <= na; i++)
    if (oa[2 * i] != ob[2 * i] || oa[2 * i + 1] != ob[2 * i + 1]) return 0;
  return 1;
}

static int group_metamorphic(void) {
  int noncap_cases = 0, drop_cases = 0, wrap_cases = 0, rename_cases = 0;
  int lib_noncap = 0, lib_wrap = 0, lib_rename = 0;

  for (int i = 0; i < 40; i++) {
    TaskRng rng;
    rng_seed(&rng, 0xabc0000ULL + (uint64_t)i);
    int ng_gen = 0;
    Node *a = ast_generate(&rng, &ng_gen);
    uint32_t text[TASK_MAX_TEXT + 1];
    int tlen = gen_text(&rng, text, TASK_MAX_TEXT);

    /* (1) Adding non-capturing groups inside concatenations must not
     *     change the language or any capture. */
    Node *b = xform_noncap_groups(a);
    TASK_CHECK(group_count(b) == ng_gen);
    if (patterns_equiv_oracle(a, b, text, tlen)) noncap_cases++;

    uint32_t pa[PAT_CAP], pb[PAT_CAP];
    int la = ast_serialize(a, pa, PAT_CAP);
    int lb = ast_serialize(b, pb, PAT_CAP);
    TASK_CHECK(la > 0 && lb > 0);
    LibResult xa = lib_run(32, pa, la, text, tlen);
    LibResult xb = lib_run(32, pb, lb, text, tlen);
    TASK_CHECK(!xa.compile_error && !xb.compile_error);
    if (xa.matched == xb.matched) {
      int same = 1;
      if (xa.matched)
        for (int g = 0; g <= ng_gen; g++)
          if (xa.ovec[2 * g] != xb.ovec[2 * g] ||
              xa.ovec[2 * g + 1] != xb.ovec[2 * g + 1])
            same = 0;
      if (same) lib_noncap++;
    }

    /* (2) Removing those groups inverts the transform at the oracle. */
    Node *c = xform_drop_noncap(b);
    if (patterns_equiv_oracle(a, c, text, tlen)) drop_cases++;
    ast_free(c);
    ast_free(b);

    /* (3) Capturing wrap: old group i becomes new group i+1, and the new
     *     group 1 equals the whole-match span. Explicit mapping instead
     *     of assuming identical ovectors. */
    int map[TASK_MAX_GROUPS + 2];
    int newcount = 0;
    Node *w = xform_capture_wrap(a, map, TASK_MAX_GROUPS + 2, &newcount);
    TASK_CHECK(newcount == ng_gen + 1);
    {
      size_t oa[(TASK_MAX_GROUPS + 1) * 2];
      size_t ow[(TASK_MAX_GROUPS + 2) * 2];
      int ma = oracle_match(a, text, tlen, oa,
                            (TASK_MAX_GROUPS + 1) * 2);
      int mw = oracle_match(w, text, tlen, ow,
                            (TASK_MAX_GROUPS + 2) * 2);
      TASK_CHECK(ma == mw);
      if (ma) {
        TASK_CHECK(ow[0] == oa[0] && ow[1] == oa[1]);
        TASK_CHECK(ow[2] == oa[0] && ow[3] == oa[1]); /* new group 1 */
        for (int g = 1; g <= ng_gen; g++) {
          TASK_CHECK(map[g] == g + 1);
          TASK_CHECK(ow[2 * map[g]] == oa[2 * g]);
          TASK_CHECK(ow[2 * map[g] + 1] == oa[2 * g + 1]);
        }
        wrap_cases++;
      }
      uint32_t pw[PAT_CAP];
      int lw = ast_serialize(w, pw, PAT_CAP);
      LibResult xw = lib_run(16, pw, lw, text, tlen);
      TASK_CHECK(!xw.compile_error && !xw.boundary_violation);
      TASK_CHECK(xw.matched == ma && xw.ngroups == newcount);
      if (ma) {
        TASK_CHECK(xw.ovec[2] == oa[0] && xw.ovec[3] == oa[1]);
        int mapped = 1;
        for (int g = 1; g <= ng_gen; g++)
          if (xw.ovec[2 * map[g]] != oa[2 * g] ||
              xw.ovec[2 * map[g] + 1] != oa[2 * g + 1])
            mapped = 0;
        if (mapped) lib_wrap++;
      }
    }
    ast_free(w);

    /* (4) Safe renaming to unique named groups: numbers and spans stay. */
    Node *r = xform_rename(a);
    if (patterns_equiv_oracle(a, r, text, tlen)) rename_cases++;
    uint32_t pr[PAT_CAP];
    int lr = ast_serialize(r, pr, PAT_CAP);
    LibResult xr = lib_run(8, pa, la, text, tlen);
    LibResult xrn = lib_run(8, pr, lr, text, tlen);
    TASK_CHECK(!xrn.compile_error);
    TASK_CHECK(xr.matched == xrn.matched && xr.ngroups == xrn.ngroups);
    int rename_same = 1;
    if (xr.matched) {
      for (int g = 0; g <= ng_gen; g++)
        if (xr.ovec[2 * g] != xrn.ovec[2 * g] ||
            xr.ovec[2 * g + 1] != xrn.ovec[2 * g + 1])
          rename_same = 0;
    }
    if (rename_same) lib_rename++;
    ast_free(r);
    ast_free(a);
  }

  TASK_CHECK(noncap_cases == 40);
  TASK_CHECK(drop_cases == 40);
  TASK_CHECK(wrap_cases > 0);
  TASK_CHECK(rename_cases == 40);
  TASK_CHECK(lib_noncap == 40);
  TASK_CHECK(lib_wrap > 0);
  TASK_CHECK(lib_rename == 40);
  printf("METAMORPHIC noncap=%d drop=%d wrap=%d rename=%d "
         "(library-verified noncap=%d wrap=%d rename=%d)\n",
         noncap_cases, drop_cases, wrap_cases, rename_cases, lib_noncap,
         lib_wrap, lib_rename);
  return 0;
}

/* ---------------- Replay / resource group ------------------------------ */

static int group_replay(void) {
  static Fingerprint first[FINGER_CAP];
  static Fingerprint second[FINGER_CAP];
  TASK_CHECK(TASK_CASES_DEFAULT <= FINGER_CAP);

  Coverage c1, c2;
  FirstDiff d1, d2;
  coverage_init(&c1);
  coverage_init(&c2);
  diff_reset(&d1);
  diff_reset(&d2);

  int ok1 = run_suite(TASK_CASES_DEFAULT, &c1, &d1, first);
  int ok2 = run_suite(TASK_CASES_DEFAULT, &c2, &d2, second);

  /* 1. Both runs stay within the declared resource bounds. */
  TASK_CHECK(c1.cases == TASK_CASES_DEFAULT);
  TASK_CHECK(c2.cases == TASK_CASES_DEFAULT);
  TASK_CHECK(ok1 && ok2);

  /* 2. Fixed seeds reproduce every case: pattern, text, oracle answer and
   *     all three library answers compare identical byte-for-byte. */
  int identical = 0;
  for (int i = 0; i < TASK_CASES_DEFAULT; i++) {
    TASK_CHECK(first[i].seed == second[i].seed);
    TASK_CHECK(first[i].pat_hash == second[i].pat_hash);
    TASK_CHECK(first[i].text_hash == second[i].text_hash);
    TASK_CHECK(first[i].oracle_matched == second[i].oracle_matched);
    TASK_CHECK(memcmp(first[i].oracle_ov, second[i].oracle_ov,
                      sizeof(first[i].oracle_ov)) == 0);
    TASK_CHECK(memcmp(&first[i].r8, &second[i].r8, sizeof(LibResult)) == 0);
    TASK_CHECK(memcmp(&first[i].r16, &second[i].r16, sizeof(LibResult)) == 0);
    TASK_CHECK(memcmp(&first[i].r32, &second[i].r32,
                      sizeof(LibResult)) == 0);
    identical++;
  }
  TASK_CHECK(identical == TASK_CASES_DEFAULT);

  /* 3. Aggregate coverage also reproduces. */
  TASK_CHECK(c1.matches == c2.matches && c1.no_matches == c2.no_matches);
  TASK_CHECK(c1.mismatches == 0 && c2.mismatches == 0);
  TASK_CHECK(d1.case_index == -1 && d2.case_index == -1);

  /* 4. The predetermined in-memory discrepancy simulator proves the
   *    comparison harness reports a planted oracle difference and keeps
   *    the first failing case. No upstream code is modified. */
  TASK_CHECK(simulator_self_check() == 1);

  /* 5. Boundary tables themselves: hand-check UTF-8/16/32 boundaries. */
  uint32_t scalars[] = {'a', 0x00e9u, 0x1f600u, 'b'};
  uint8_t u8[16];
  uint16_t u16[16];
  uint32_t u32[8];
  int n8 = enc_utf8(scalars, 4, u8, 16);
  int n16 = enc_utf16(scalars, 4, u16, 16);
  int n32 = enc_utf32(scalars, 4, u32, 8);
  uint8_t t8[17], t16[17], t32[9];
  bound_utf8(u8, n8, t8);
  bound_utf16(u16, n16, t16);
  bound_utf32(u32, n32, t32);
  TASK_CHECK(n8 == 1 + 2 + 4 + 1);
  TASK_CHECK(n16 == 1 + 1 + 2 + 1);
  TASK_CHECK(n32 == 4);
  TASK_CHECK(map_offset(t8, n8, 0) == 0);
  TASK_CHECK(map_offset(t8, n8, 1) == 1);       /* start of U+00E9 */
  TASK_CHECK(map_offset(t8, n8, 2) == -1);      /* inside U+00E9 */
  TASK_CHECK(map_offset(t8, n8, 3) == 2);       /* start of U+1F600 */
  TASK_CHECK(map_offset(t16, n16, 2) == 2);     /* high surrogate */
  TASK_CHECK(map_offset(t16, n16, 3) == -1);    /* low surrogate */
  TASK_CHECK(map_offset(t32, n32, 3) == 3);
  TASK_CHECK(map_offset(t32, n32, 4) == 4);     /* one-past-end */

  printf("REPLAY %d cases reproduced identically; discrepancy simulator "
         "and boundary checks passed\n", identical);
  return 0;
}

int task_cases(const char *group) {
  if (strcmp(group, "model") == 0) return group_model();
  if (strcmp(group, "metamorphic") == 0) return group_metamorphic();
  if (strcmp(group, "replay") == 0) return group_replay();
  return 1;
}
