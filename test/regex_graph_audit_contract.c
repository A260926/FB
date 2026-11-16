/*
 * regex_graph_audit_contract.c - regression for the read-only structural
 * audit example.
 *
 * The audit is driven through its command-line binary so the test covers
 * the documented offline path (compile -> graph -> candidates) end to
 * end.  Positive cases assert the exact candidate locations; negative
 * cases assert that no candidate is reported; unsupported patterns must
 * be reported as coverage-insufficient and never as "no risk".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reggraph.h"

#define MIN_CHECKS 24

static int g_checks = 0;
static int g_failures = 0;
static const char* g_audit_bin = 0;

#define CHECK(cond, ...) \
  do { \
    g_checks++; \
    if (! (cond)) { \
      fprintf(stderr, "FAIL check %d: ", g_checks); \
      fprintf(stderr, __VA_ARGS__); \
      fprintf(stderr, "\n"); \
      g_failures++; \
    } \
  } while (0)

/* Run the audit binary on a pattern file; return its stdout in a malloc'd
   buffer (caller frees) and the exit status in *status. */
static char*
run_audit_file(const char* path, int* status)
{
  char cmd[1024];
  FILE* p;
  char* out = 0;
  size_t cap = 0, len = 0;
  int st;

  snprintf(cmd, sizeof(cmd), "\"%s\" --pattern-file \"%s\"", g_audit_bin, path);
  p = popen(cmd, "r");
  if (p == 0) { *status = -1; return 0; }
  for (;;) {
    size_t got;
    if (len == cap) {
      size_t ncap = (cap == 0 ? 4096 : cap * 2);
      char* n = (char* )realloc(out, ncap);
      if (n == 0) { free(out); pclose(p); *status = -2; return 0; }
      out = n; cap = ncap;
    }
    got = fread(out + len, 1, cap - len - 1, p);
    len += got;
    if (got == 0) break;
  }
  if (out == 0) {
    out = (char* )malloc(1);
    if (out == 0) { pclose(p); *status = -2; return 0; }
  }
  out[len] = '\0';
  st = pclose(p);
  *status = st;
  return out;
}

static int
contains(const char* hay, const char* needle)
{
  return strstr(hay, needle) != 0;
}

static int
count_occurrences(const char* hay, const char* needle)
{
  int n = 0;
  const char* p = hay;
  size_t nl = strlen(needle);
  while ((p = strstr(p, needle)) != 0) { n++; p += nl; }
  return n;
}

/* Write a pattern to a temp file; return 0 on failure. */
static int
write_pattern(const char* dir, const char* name, const char* pattern,
              char* path, size_t pathsz)
{
  FILE* f;
  snprintf(path, pathsz, "%s/%s", dir, name);
  f = fopen(path, "wb");
  if (f == 0) return 0;
  if (*pattern != '\0')
    fwrite(pattern, 1, strlen(pattern), f);
  fclose(f);
  return 1;
}

int
main(int argc, char** argv)
{
  char dir[512];
  char path[512];
  int status;
  char* out;

  if (argc < 2) {
    fprintf(stderr, "usage: %s AUDIT_BINARY\n", argv[0]);
    return 2;
  }
  g_audit_bin = argv[1];
  snprintf(dir, sizeof(dir), "%s", getenv("TMPDIR") ? getenv("TMPDIR") : ".");

  /* 1. Positive nested repeat + body alternatives.
     Hand-derived: outer repeat {0,9} body 1; inner {2,7} body 3 with a
     PUSH alternative. */
  if (! write_pattern(dir, "rg_audit_nested.txt", "((?:a|b){2,4}){2,4}",
                      path, sizeof(path))) return 3;
  out = run_audit_file(path, &status);
  CHECK(out != 0 && status == 0, "nested audit exit status");
  CHECK(contains(out, "\"status\": \"structural-candidates\""),
        "nested status");
  CHECK(contains(out, "\"rule\": \"nested-repeat\""), "nested rule present");
  CHECK(contains(out, "\"outer_entry\": 0, \"outer_inc\": 9, "
                      "\"outer_body\": 1"),
        "nested outer location");
  CHECK(! contains(out, "\"outer_entry\": 2, \"outer_inc\": 7, "
                        "\"outer_body\": 3, \"op_indices\": [2, 3, 7]}"),
        "inner repeat is not reported as nested");
  CHECK(count_occurrences(out, "\"rule\": \"nested-repeat\"") >= 1,
        "at least one nested candidate");
  CHECK(contains(out, "\"rule\": \"repeat-body-alt\""),
        "body alt rule present");
  CHECK(contains(out, "\"from\": 3, \"to\": 6, \"type\": \"alt-push\""),
        "specific alt-push location");
  /* Inner repeat increment 7 offers its exit 8 as a deferred alternative
     while another iteration is still allowed. */
  CHECK(contains(out, "\"from\": 7, \"to\": 8, \"type\": \"repeat-exit\""),
        "specific deferred repeat exit location");
  CHECK(contains(out, "\"coverage\": {\"sufficient\": true}"),
        "nested coverage sufficient");
  CHECK(contains(out, "structural risk candidates"),
        "explicit candidate wording");
  CHECK(! contains(out, "\"safe\""), "no safety verdict field");
  CHECK(! contains(out, "\"status\": \"safe\""), "no safe status");
  free(out);

  /* 2. Positive recursive SCC. */
  if (! write_pattern(dir, "rg_audit_recur.txt",
                      "(?<node>a(?:\\g<node>)?b)", path, sizeof(path)))
    return 3;
  out = run_audit_file(path, &status);
  CHECK(out != 0 && status == 0, "recursive audit exit status");
  CHECK(contains(out, "\"rule\": \"recursive-call-scc\""),
        "recursive rule present");
  CHECK(contains(out, "\"scc_members\": [2]"), "recursive scc member");
  CHECK(contains(out, "\"first_call_index\": 5"), "recursive call site");
  CHECK(! contains(out, "\"rule\": \"nested-repeat\""),
        "recursive pattern has no nested-repeat");
  free(out);

  /* 3. Positive mutual recursion: SCC with two members. */
  if (! write_pattern(dir, "rg_audit_mutual.txt",
                      "(?<left>a\\g<right>?b){0}(?<right>c\\g<left>?d){0}"
                      "\\g<left>", path, sizeof(path))) return 3;
  out = run_audit_file(path, &status);
  CHECK(out != 0 && status == 0, "mutual audit exit status");
  CHECK(contains(out, "\"rule\": \"recursive-call-scc\""),
        "mutual recursive rule");
  CHECK(contains(out, "\"scc_members\": [3, 13]"), "mutual scc members");
  CHECK(contains(out, "\"scc_id\": 3"), "mutual stable scc id");
  free(out);

  /* 4. Negative: a plain linear pattern has no candidates. */
  if (! write_pattern(dir, "rg_audit_linear.txt", "ab[cd].", path,
                      sizeof(path))) return 3;
  out = run_audit_file(path, &status);
  CHECK(out != 0 && status == 0, "linear audit exit status");
  CHECK(contains(out, "\"candidates\": [\n\n  ]") ||
        contains(out, "\"candidates\": ["), "linear candidates key");
  CHECK(! contains(out, "\"rule\":"), "linear has no rule entries");
  CHECK(contains(out, "\"coverage\": {\"sufficient\": true}"),
        "linear coverage sufficient");
  free(out);

  /* 5. Negative: a single bounded repeat with no alternative and no
     nested repeat must not be flagged. */
  write_pattern(dir, "rg_audit_plain_repeat.txt", "(?:ab){2,}", path,
                sizeof(path));
  out = run_audit_file(path, &status);
  CHECK(out != 0 && status == 0, "plain repeat exit status");
  CHECK(! contains(out, "\"rule\": \"recursive-call-scc\""),
        "plain repeat not recursive");
  CHECK(! contains(out, "\"rule\": \"nested-repeat\""),
        "plain repeat not nested");
  free(out);

  /* 6. Coverage-insufficient: a backreference must be reported, and must
     not be listed as a clean result. */
  if (! write_pattern(dir, "rg_audit_unsupported.txt", "(a)\\1", path,
                      sizeof(path))) return 3;
  out = run_audit_file(path, &status);
  CHECK(out != 0, "unsupported audit produced output");
  CHECK(status == 0, "coverage-insufficient is not an error exit");
  CHECK(contains(out, "\"status\": \"coverage-insufficient\""),
        "unsupported status");
  CHECK(contains(out, "\"sufficient\": false"), "unsupported coverage flag");
  CHECK(contains(out, "\"reason\": \"unsupported-opcode\""),
        "unsupported reason");
  CHECK(contains(out, "\"opcode\": \"backref1\""), "unsupported opcode");
  CHECK(contains(out, "\"op_index\": 3"), "unsupported op index");
  CHECK(! contains(out, "\"candidates\": [\n    {"), "no candidates emitted");
  free(out);

  /* 7. Independent structural comparison: the audit's nested-repeat claim
     must agree with the production graph inspected directly. */
  {
    OnigEncoding enc = ONIG_ENCODING_ASCII;
    OnigRegex re = 0;
    OnigErrorInfo ei;
    OnigGraphError ge;
    OnigGraph* g;
    const char* pat = "((?:a|b){2,4}){2,4}";
    int i, inner_repeat_inside_body = 0;
    int rc;
    onig_initialize(&enc, 1);
    rc = onig_new(&re, (const UChar* )pat,
                  (const UChar* )(pat + strlen(pat)), ONIG_OPTION_NONE,
                  enc, ONIG_SYNTAX_RUBY, &ei);
    CHECK(rc == ONIG_NORMAL, "direct compile");
    g = onig_graph_new(re, &ge);
    CHECK(g != 0, "direct graph build");
    if (g != 0) {
      /* outer body 1 .. inc 9 contains inner entry 2. */
      const OnigGraphRepeat* outer = onig_graph_repeat(g, 0);
      for (i = 0; i < onig_graph_node_count(g); i++) {
        if (i > outer->body_index && i < outer->inc_index &&
            (onig_graph_node(g, i)->flags & ONIG_GRAPH_NODE_REPEAT_ENTRY))
          inner_repeat_inside_body = 1;
      }
      CHECK(inner_repeat_inside_body,
            "inner repeat entry lies inside the outer body");
      onig_graph_free(g);
    }
    onig_free(re);
    onig_end();
  }

  if (g_checks < MIN_CHECKS) {
    fprintf(stderr, "audit test ran only %d checks\n", g_checks);
    return 1;
  }
  if (g_failures != 0) {
    fprintf(stderr, "RESULT FAIL checks=%d failures=%d\n", g_checks,
            g_failures);
    return 1;
  }
  printf("RESULT PASS checks=%d\n", g_checks);
  return 0;
}
