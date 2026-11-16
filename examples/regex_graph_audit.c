/*
 * regex_graph_audit.c - read-only structural risk candidate audit.
 *
 * Builds the production graph from a compiled pattern and lists
 * structures that deserve human review:
 *
 *   1. recursive-call-scc   A call-graph SCC that is recursive.
 *   2. nested-repeat        A repeat body that itself contains a
 *                           repeat entry.
 *   3. repeat-body-alt      A repeat body that contains a deferred
 *                           alternative (backtrack) edge.
 *
 * These rules are structural only.  They do NOT prove that a pattern is
 * vulnerable to catastrophic backtracking, do not run long inputs and
 * do not time anything.  They never emit a "safe" conclusion.  When the
 * pattern uses an opcode outside the supported set, the seed opcode is
 * reported as coverage-insufficient.
 *
 * Usage:
 *   regex_graph_audit --pattern-file PATH
 *   regex_graph_audit --pattern TEXT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reggraph.h"

static void usage(FILE* f)
{
  fprintf(f, "usage: regex_graph_audit (--pattern-file PATH | --pattern TEXT)\n");
}

static int
read_file(const char* path, unsigned char** out_data, size_t* out_len,
          char* err, size_t err_size)
{
  FILE* f = fopen(path, "rb");
  unsigned char* data = 0;
  size_t cap = 0, len = 0;

  if (f == 0) {
    snprintf(err, err_size, "cannot open pattern file: %s", path);
    return 0;
  }
  for (;;) {
    size_t got;
    if (len == cap) {
      size_t ncap = (cap == 0 ? 4096 : cap * 2);
      unsigned char* nd = (unsigned char* )realloc(data, ncap);
      if (nd == 0) {
        free(data); fclose(f);
        snprintf(err, err_size, "out of memory");
        return 0;
      }
      data = nd; cap = ncap;
    }
    got = fread(data + len, 1, cap - len, f);
    len += got;
    if (got == 0) {
      if (ferror(f)) {
        free(data); fclose(f);
        snprintf(err, err_size, "read error");
        return 0;
      }
      break;
    }
  }
  fclose(f);
  *out_data = data;
  *out_len = len;
  return 1;
}

/* ------------------------------------------------------------------ */
/* Pattern identifier                                                   */
/* ------------------------------------------------------------------ */

#define ID_MAX 96

static void
make_pattern_id(const unsigned char* pat, size_t len, char* out, size_t outsz)
{
  size_t i, o = 0;
  unsigned long hash = 2166136261u;   /* FNV-1a, for a stable short id */

  for (i = 0; i < len; i++) {
    hash ^= pat[i];
    hash *= 16777619u;
  }
  snprintf(out, outsz, "pat-%08lx-", hash);
  o = strlen(out);
  for (i = 0; i < len && o + 5 < outsz; i++) {
    unsigned char c = pat[i];
    if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') out[o++] = (char)c;
    else o += (size_t)snprintf(out + o, outsz - o, "\\x%02x", (unsigned)c);
  }
  out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* JSON output                                                          */
/* ------------------------------------------------------------------ */

static void
json_str(FILE* f, const char* s)
{
  const unsigned char* p = (const unsigned char* )s;
  fputc('"', f);
  while (*p) {
    unsigned char c = *p++;
    if (c == '"' || c == '\\') { fputc('\\', f); fputc((int)c, f); }
    else if (c < 0x20 || c >= 0x7f) fprintf(f, "\\u%04x", (unsigned)c);
    else fputc((int)c, f);
  }
  fputc('"', f);
}

static void
json_escaped(FILE* f, const unsigned char* s, int len)
{
  int i;
  fputc('"', f);
  for (i = 0; i < len; i++) {
    unsigned char c = s[i];
    if (c == '"' || c == '\\') { fputc('\\', f); fputc((int)c, f); }
    else if (c < 0x20 || c >= 0x7f) fprintf(f, "\\u%04x", (unsigned)c);
    else fputc((int)c, f);
  }
  fputc('"', f);
}

/* Does the repeat body contain another repeat entry?  The walk starts at
   the body, stays inside the repeat, and follows neither call edges nor
   resume summaries. */
static int
body_has_nested_repeat(const OnigGraph* g, int body, int inc)
{
  int n = onig_graph_node_count(g);
  unsigned char* seen = (unsigned char* )calloc((size_t)n, 1);
  int* stack = (int* )malloc(sizeof(int) * (size_t)n);
  int top = 0;
  int found = 0;

  if (seen == 0 || stack == 0) { free(seen); free(stack); return 0; }
  seen[body] = 1;
  stack[top++] = body;
  while (top > 0) {
    int v = stack[--top];
    int k;
    if (v == inc) continue;
    if (v != body &&
        (onig_graph_node(g, v)->flags & ONIG_GRAPH_NODE_REPEAT_ENTRY) != 0) {
      found = 1;
      break;
    }
    for (k = 0; k < onig_graph_out_edge_count(g, v); k++) {
      const OnigGraphEdge* e = onig_graph_out_edge(g, v, k);
      if (e->type == ONIG_GRAPH_EDGE_CALL ||
          e->type == ONIG_GRAPH_EDGE_RESUME_SUMMARY) continue;
      if (! seen[e->to]) { seen[e->to] = 1; stack[top++] = e->to; }
    }
  }
  free(seen);
  free(stack);
  return found;
}

/* Find the global edge index of e.  The edge pointer always comes from
   the graph's own edge store. */
static int
global_edge_index(const OnigGraph* g, const OnigGraphEdge* e)
{
  return (int)(e - onig_graph_edge(g, 0));
}

/* Collect deferred alternative edges whose source is inside the repeat
   body proper (not the increment). */
static void
collect_body_alts(const OnigGraph* g, int body, int inc, int* edges,
                  int max_edges, int* count)
{
  int n = onig_graph_node_count(g);
  unsigned char* seen = (unsigned char* )calloc((size_t)n, 1);
  int* stack = (int* )malloc(sizeof(int) * (size_t)n);
  int top = 0;

  *count = 0;
  if (seen == 0 || stack == 0) { free(seen); free(stack); return; }
  seen[body] = 1;
  stack[top++] = body;
  while (top > 0) {
    int v = stack[--top];
    int k;
    if (v == inc) continue;
    for (k = 0; k < onig_graph_out_edge_count(g, v); k++) {
      const OnigGraphEdge* e = onig_graph_out_edge(g, v, k);
      if (e->type == ONIG_GRAPH_EDGE_CALL ||
          e->type == ONIG_GRAPH_EDGE_RESUME_SUMMARY) continue;
      if (e->deferred && *count < max_edges)
        edges[(*count)++] = global_edge_index(g, e);
      if (! seen[e->to]) { seen[e->to] = 1; stack[top++] = e->to; }
    }
  }
  free(seen);
  free(stack);
}

static void
print_rule_opening(FILE* f, int first, const char* id, const char* rule)
{
  if (! first) fputs(",\n", f);
  fputs("    {\"pattern_id\": ", f);
  json_str(f, id);
  fputs(", \"rule\": ", f);
  json_str(f, rule);
}

int
main(int argc, char** argv)
{
  const char* pattern_file = 0;
  const char* pattern_text = 0;
  unsigned char* data = 0;
  size_t len = 0;
  char pid[ID_MAX];
  const char* file_for_id;
  int i, rc;
  OnigEncoding enc = ONIG_ENCODING_ASCII;
  OnigRegex regex = 0;
  OnigErrorInfo einfo;
  OnigGraphError gerr;
  OnigGraph* g = 0;
  int emitted = 0;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--pattern-file") == 0) {
      if (++i >= argc) { usage(stderr); return 2; }
      pattern_file = argv[i];
    }
    else if (strcmp(argv[i], "--pattern") == 0) {
      if (++i >= argc) { usage(stderr); return 2; }
      pattern_text = argv[i];
    }
    else if (strcmp(argv[i], "--help") == 0) { usage(stdout); return 0; }
    else { fprintf(stderr, "unknown argument: %s\n", argv[i]); usage(stderr); return 2; }
  }
  if ((pattern_file == 0) == (pattern_text == 0)) {
    fprintf(stderr, "exactly one of --pattern-file or --pattern is required\n");
    usage(stderr);
    return 2;
  }

  if (pattern_file != 0) {
    char err[256];
    if (! read_file(pattern_file, &data, &len, err, sizeof(err))) {
      fprintf(stderr, "regex_graph_audit: %s\n", err);
      return 3;
    }
    file_for_id = pattern_file;
  }
  else {
    file_for_id = "<arg>";
    len = strlen(pattern_text);
    data = (unsigned char* )malloc(len == 0 ? 1 : len);
    if (data == 0) { fprintf(stderr, "regex_graph_audit: oom\n"); return 3; }
    if (len > 0) memcpy(data, pattern_text, len);
  }

  rc = onig_initialize(&enc, 1);
  if (rc != ONIG_NORMAL) {
    fprintf(stderr, "regex_graph_audit: init failed: %d\n", rc);
    free(data);
    return 4;
  }
  rc = onig_new(&regex, data, data + len, ONIG_OPTION_NONE, enc,
                ONIG_SYNTAX_RUBY, &einfo);
  if (rc != ONIG_NORMAL) {
    UChar msg[ONIG_MAX_ERROR_MESSAGE_LEN];
    onig_error_code_to_str(msg, rc, &einfo);
    fprintf(stderr, "regex_graph_audit: compile failed (%d): %s\n", rc,
            (char* )msg);
    onig_end();
    free(data);
    return 5;
  }

  (void)file_for_id;
  make_pattern_id(data, len, pid, sizeof(pid));

  g = onig_graph_new(regex, &gerr);
  onig_free(regex);
  regex = 0;

  if (g == 0) {
    /* Coverage-insufficient: never a safe conclusion. */
    printf("{\n  \"format\": \"onig-regex-audit\",\n  \"status\": ");
    if (gerr.reason == ONIG_GRAPH_ERR_UNSUPPORTED_OPCODE ||
        gerr.reason == ONIG_GRAPH_ERR_UNKNOWN_OPCODE) {
      fputs("\"coverage-insufficient\",\n", stdout);
      fputs("  \"coverage\": {\"sufficient\": false, \"reason\": ", stdout);
      json_str(stdout, onig_graph_error_reason_name((int)gerr.reason));
      printf(", \"op_index\": %d, \"opcode\": ", gerr.op_index);
      if (gerr.opcode >= 0) json_str(stdout, onig_graph_opcode_name(gerr.opcode));
      else fputs("null", stdout);
      fputs(", \"message\": ", stdout);
      json_str(stdout, gerr.message);
      fputs("},\n", stdout);
    }
    else {
      fputs("\"error\",\n", stdout);
      fputs("  \"error\": {\"reason\": ", stdout);
      json_str(stdout, onig_graph_error_reason_name((int)gerr.reason));
      printf(", \"op_index\": %d, \"message\": ", gerr.op_index);
      json_str(stdout, gerr.message);
      fputs("},\n", stdout);
    }
    fputs("  \"pattern_id\": ", stdout);
    json_str(stdout, pid);
    fputs(",\n  \"pattern\": ", stdout);
    json_escaped(stdout, data, (int)len);
    fputs(",\n  \"candidates\": []\n}\n", stdout);
    onig_end();
    free(data);
    return (gerr.reason == ONIG_GRAPH_ERR_UNSUPPORTED_OPCODE ||
            gerr.reason == ONIG_GRAPH_ERR_UNKNOWN_OPCODE) ? 0 : 7;
  }

  printf("{\n  \"format\": \"onig-regex-audit\",\n");
  fputs("  \"status\": \"structural-candidates\",\n", stdout);
  fputs("  \"coverage\": {\"sufficient\": true},\n", stdout);
  fputs("  \"pattern_id\": ", stdout);
  json_str(stdout, pid);
  fputs(",\n  \"pattern\": ", stdout);
  json_escaped(stdout, data, (int)len);
  fputs(",\n  \"note\": \"structural risk candidates only; not a proof of "
        "ReDoS and not a safety verdict\",\n", stdout);
  fputs("  \"candidates\": [\n", stdout);

  /* Rule 1: recursive call SCCs. */
  for (i = 0; i < onig_graph_scc_count(g); i++) {
    const OnigGraphScc* s = onig_graph_scc(g, i);
    int k;
    if (! s->recursive) continue;
    print_rule_opening(stdout, emitted == 0, pid, "recursive-call-scc");
    fprintf(stdout, ", \"scc_id\": %d, \"scc_members\": [", s->id);
    for (k = 0; k < s->member_count; k++)
      printf("%s%d", (k ? ", " : ""), s->members[k]);
    fputs("], \"call_edges\": [", stdout);
    {
      int first_edge = 1;
      int e;
      for (e = 0; e < onig_graph_call_edge_count(g); e++) {
        const OnigGraphCallEdge* ce = onig_graph_call_edge(g, e);
        int from_in = 0, to_in = 0;
        for (k = 0; k < s->member_count; k++) {
          if (s->members[k] == ce->from_entry) from_in = 1;
          if (s->members[k] == ce->to_entry) to_in = 1;
        }
        if (from_in && to_in) {
          if (! first_edge) fputs(", ", stdout);
          printf("{\"from_entry\": %d, \"to_entry\": %d, "
                 "\"first_call_index\": %d, \"call_count\": %d}",
                 ce->from_entry, ce->to_entry, ce->first_call_index,
                 ce->call_count);
          first_edge = 0;
        }
      }
    }
    fputs("], \"op_indices\": [", stdout);
    {
      int first_op = 1;
      int e;
      for (e = 0; e < onig_graph_call_edge_count(g); e++) {
        const OnigGraphCallEdge* ce = onig_graph_call_edge(g, e);
        int from_in = 0;
        for (k = 0; k < s->member_count; k++)
          if (s->members[k] == ce->from_entry) from_in = 1;
        if (! from_in) continue;
        if (! first_op) fputs(", ", stdout);
        printf("%d", ce->first_call_index);
        first_op = 0;
      }
    }
    fputs("]}", stdout);
    emitted++;
  }

  /* Rule 2: nested repeat. */
  for (i = 0; i < onig_graph_repeat_count(g); i++) {
    const OnigGraphRepeat* r = onig_graph_repeat(g, i);
    if (! body_has_nested_repeat(g, r->body_index, r->inc_index)) continue;
    print_rule_opening(stdout, emitted == 0, pid, "nested-repeat");
    fprintf(stdout, ", \"outer_entry\": %d, \"outer_inc\": %d, "
            "\"outer_body\": %d, \"op_indices\": [%d, %d, %d]}",
            r->entry_index, r->inc_index, r->body_index, r->entry_index,
            r->body_index, r->inc_index);
    emitted++;
  }

  /* Rule 3: deferred alternative inside a repeat body. */
  for (i = 0; i < onig_graph_repeat_count(g); i++) {
    const OnigGraphRepeat* r = onig_graph_repeat(g, i);
    int edge_ids[64];
    int edge_count = 0;
    int k;
    collect_body_alts(g, r->body_index, r->inc_index, edge_ids, 64,
                      &edge_count);
    if (edge_count == 0) continue;
    print_rule_opening(stdout, emitted == 0, pid, "repeat-body-alt");
    fprintf(stdout, ", \"outer_entry\": %d, \"outer_inc\": %d, "
            "\"outer_body\": %d, \"edges\": [", r->entry_index,
            r->inc_index, r->body_index);
    for (k = 0; k < edge_count; k++) {
      const OnigGraphEdge* e = onig_graph_edge(g, edge_ids[k]);
      printf("%s{\"edge_index\": %d, \"from\": %d, \"to\": %d, \"type\": ",
             (k ? ", " : ""), edge_ids[k], e->from, e->to);
      json_str(stdout, onig_graph_edge_type_name(e->type));
      fputc('}', stdout);
    }
    fputs("]}", stdout);
    emitted++;
  }

  fputs("\n  ]\n}\n", stdout);

  onig_graph_free(g);
  onig_end();
  free(data);
  return 0;
}
