/*
 * regex_graph.c - offline example: compile a pattern, build the
 * read-only regex graph, and print it as JSON or DOT.
 *
 * Usage:
 *   regex_graph --pattern-file PATH [--format json|dot]
 *   regex_graph --pattern TEXT      [--format json|dot]
 *
 * The pattern is read as raw bytes; its length is the file's actual
 * byte length (no trimming, no newline assumption).  Compilation and
 * graph-construction failures are reported as structured errors and
 * exit non-zero.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reggraph.h"

static void usage(FILE* f)
{
  fprintf(f,
          "usage: regex_graph (--pattern-file PATH | --pattern TEXT) "
          "[--format json|dot]\n");
}

static int
read_file(const char* path, unsigned char** out_data, size_t* out_len,
          char* err, size_t err_size)
{
  FILE* f;
  unsigned char* data = 0;
  size_t cap = 0, len = 0;

  f = fopen(path, "rb");
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
        free(data);
        fclose(f);
        snprintf(err, err_size, "out of memory reading pattern file");
        return 0;
      }
      data = nd;
      cap = ncap;
    }
    got = fread(data + len, 1, cap - len, f);
    len += got;
    if (got == 0) {
      if (ferror(f)) {
        free(data);
        fclose(f);
        snprintf(err, err_size, "read error on pattern file");
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
/* JSON                                                                */
/* ------------------------------------------------------------------ */

static void
json_escape(FILE* f, const unsigned char* s, int len)
{
  int i;
  fputc('"', f);
  for (i = 0; i < len; i++) {
    unsigned char c = s[i];
    switch (c) {
    case '"': fputs("\\\"", f); break;
    case '\\': fputs("\\\\", f); break;
    case '\b': fputs("\\b", f); break;
    case '\f': fputs("\\f", f); break;
    case '\n': fputs("\\n", f); break;
    case '\r': fputs("\\r", f); break;
    case '\t': fputs("\\t", f); break;
    default:
      if (c < 0x20 || c >= 0x7f) {
        /* Byte values, not code points: the pattern is raw bytes. */
        fprintf(f, "\\u%04x", (unsigned int)c);
      }
      else {
        fputc((int)c, f);
      }
      break;
    }
  }
  fputc('"', f);
}

static void
json_string(FILE* f, const char* s)
{
  json_escape(f, (const unsigned char* )s, (int )strlen(s));
}

static void
flag_array(FILE* f, unsigned int flags)
{
  int first = 1;
  fputc('[', f);
#define FLAG_BIT(mask, text) \
  do { if ((flags & (mask)) != 0) { \
    if (! first) fputs(", ", f); \
    json_string(f, text); first = 0; \
  } } while (0)
  FLAG_BIT(ONIG_GRAPH_NODE_ENTRY, "entry");
  FLAG_BIT(ONIG_GRAPH_NODE_REACHABLE, "reachable");
  FLAG_BIT(ONIG_GRAPH_NODE_UNREACHABLE, "unreachable");
  FLAG_BIT(ONIG_GRAPH_NODE_SUCCESS_EXIT, "success-exit");
  FLAG_BIT(ONIG_GRAPH_NODE_FAIL_EXIT, "fail-exit");
  FLAG_BIT(ONIG_GRAPH_NODE_DYNAMIC_BACKTRACK_EXIT, "dynamic-backtrack-exit");
  FLAG_BIT(ONIG_GRAPH_NODE_FINISH, "finish");
  FLAG_BIT(ONIG_GRAPH_NODE_DYNAMIC_RETURN, "dynamic-return");
  FLAG_BIT(ONIG_GRAPH_NODE_CALL, "call");
  FLAG_BIT(ONIG_GRAPH_NODE_REPEAT_ENTRY, "repeat-entry");
  FLAG_BIT(ONIG_GRAPH_NODE_REPEAT_INC, "repeat-inc");
#undef FLAG_BIT
  fputc(']', f);
}

static void
print_json(FILE* f, const OnigGraph* g, const unsigned char* pattern,
           size_t plen)
{
  int i, j;

  fputs("{\n", f);
  fputs("  \"format\": \"onig-regex-graph\",\n", f);
  fprintf(f, "  \"version\": %d,\n", ONIG_GRAPH_API_VERSION);
  fputs("  \"pattern\": ", f);
  json_escape(f, pattern, (int )plen);
  fputs(",\n", f);

  fputs("  \"nodes\": [\n", f);
  for (i = 0; i < onig_graph_node_count(g); i++) {
    const OnigGraphNode* n = onig_graph_node(g, i);
    fputs("    {\"index\": ", f);
    fprintf(f, "%d, \"opcode\": ", n->index);
    json_string(f, n->opcode_name);
    fprintf(f, ", \"opcode_id\": %d, \"flags\": ", n->opcode);
    flag_array(f, n->flags);
    fprintf(f, ", \"out_edges\": %d", n->out_edge_count);
    if (n->literal != 0) {
      fprintf(f, ", \"literal_len\": %d, \"literal\": ", n->literal_len);
      json_escape(f, n->literal, n->literal_len);
      fputs(", \"literal_bytes\": [", f);
      for (j = 0; j < n->literal_len; j++)
        fprintf(f, "%s%u", (j ? ", " : ""), (unsigned)n->literal[j]);
      fputc(']', f);
    }
    fputs("}", f);
    if (i + 1 < onig_graph_node_count(g)) fputc(',', f);
    fputc('\n', f);
  }
  fputs("  ],\n", f);

  fputs("  \"edges\": [\n", f);
  for (i = 0; i < onig_graph_edge_count(g); i++) {
    const OnigGraphEdge* e = onig_graph_edge(g, i);
    fprintf(f, "    {\"from\": %d, \"to\": %d, \"type\": ", e->from, e->to);
    json_string(f, onig_graph_edge_type_name(e->type));
    fprintf(f, ", \"type_id\": %d, \"deferred\": %s, \"conditional\": %s}",
            e->type, (e->deferred ? "true" : "false"),
            (e->conditional ? "true" : "false"));
    if (i + 1 < onig_graph_edge_count(g)) fputc(',', f);
    fputc('\n', f);
  }
  fputs("  ],\n", f);

  fputs("  \"calls\": [\n", f);
  for (i = 0; i < onig_graph_call_count(g); i++) {
    const OnigGraphCall* c = onig_graph_call(g, i);
    fprintf(f, "    {\"call_index\": %d, \"target\": %d, "
               "\"resume_index\": %d}",
            c->call_index, c->target, c->resume_index);
    if (i + 1 < onig_graph_call_count(g)) fputc(',', f);
    fputc('\n', f);
  }
  fputs("  ],\n", f);

  fputs("  \"repeats\": [\n", f);
  for (i = 0; i < onig_graph_repeat_count(g); i++) {
    const OnigGraphRepeat* r = onig_graph_repeat(g, i);
    fprintf(f, "    {\"entry_index\": %d, \"inc_index\": %d, "
               "\"body_index\": %d, \"lower\": %d, \"upper\": ",
            r->entry_index, r->inc_index, r->body_index, r->lower);
    if (r->upper == ONIG_GRAPH_INFINITE_REPEAT) fputs("null", f);
    else fprintf(f, "%d", r->upper);
    fprintf(f, ", \"infinite\": %s, \"greedy\": %s}",
            (r->upper == ONIG_GRAPH_INFINITE_REPEAT ? "true" : "false"),
            (r->greedy ? "true" : "false"));
    if (i + 1 < onig_graph_repeat_count(g)) fputc(',', f);
    fputc('\n', f);
  }
  fputs("  ],\n", f);

  fputs("  \"entries\": [", f);
  for (i = 0; i < onig_graph_entry_count(g); i++)
    fprintf(f, "%s%d", (i ? ", " : ""), onig_graph_entry(g, i));
  fputs("],\n", f);

  fputs("  \"call_edges\": [\n", f);
  for (i = 0; i < onig_graph_call_edge_count(g); i++) {
    const OnigGraphCallEdge* e = onig_graph_call_edge(g, i);
    fprintf(f, "    {\"from_entry\": %d, \"to_entry\": %d, "
               "\"first_call_index\": %d, \"call_count\": %d}",
            e->from_entry, e->to_entry, e->first_call_index, e->call_count);
    if (i + 1 < onig_graph_call_edge_count(g)) fputc(',', f);
    fputc('\n', f);
  }
  fputs("  ],\n", f);

  fputs("  \"sccs\": [\n", f);
  for (i = 0; i < onig_graph_scc_count(g); i++) {
    const OnigGraphScc* s = onig_graph_scc(g, i);
    fprintf(f, "    {\"id\": %d, \"recursive\": %s, \"members\": [",
            s->id, (s->recursive ? "true" : "false"));
    for (j = 0; j < s->member_count; j++)
      fprintf(f, "%s%d", (j ? ", " : ""), s->members[j]);
    fputs("]}", f);
    if (i + 1 < onig_graph_scc_count(g)) fputc(',', f);
    fputc('\n', f);
  }
  fputs("  ]\n", f);
  fputs("}\n", f);
}

/* ------------------------------------------------------------------ */
/* DOT                                                                 */
/* ------------------------------------------------------------------ */

static void
print_dot(FILE* f, const OnigGraph* g)
{
  int i;

  fputs("digraph regex_graph {\n", f);
  fputs("  rankdir=TB;\n", f);
  fputs("  node [shape=box, fontname=\"monospace\"];\n", f);

  for (i = 0; i < onig_graph_node_count(g); i++) {
    const OnigGraphNode* n = onig_graph_node(g, i);
    fprintf(f, "  n%d [label=\"%d: %s\\nflags=%u", n->index, n->index,
            n->opcode_name, n->flags);
    if (n->literal != 0) {
      fprintf(f, "\\n%d bytes=", n->literal_len);
      {
        int k;
        for (k = 0; k < n->literal_len; k++)
          fprintf(f, "%02x", (unsigned)n->literal[k]);
      }
    }
    fputs("\"];\n", f);
  }

  /* Edges are already globally sorted by (from, type, to).  Render the
     global list so DOT ordering matches the JSON edge ordering. */
  for (i = 0; i < onig_graph_edge_count(g); i++) {
    const OnigGraphEdge* e = onig_graph_edge(g, i);
    const char* style = "solid";
    const char* color = "black";

    switch (e->type) {
    case ONIG_GRAPH_EDGE_ALT_PUSH:
    case ONIG_GRAPH_EDGE_COND_PUSH:
    case ONIG_GRAPH_EDGE_REPEAT_SKIP:
    case ONIG_GRAPH_EDGE_REPEAT_BACK:
    case ONIG_GRAPH_EDGE_REPEAT_NG_SKIP:
    case ONIG_GRAPH_EDGE_REPEAT_NG_BACK:
      style = (e->deferred ? "dashed" : "solid");
      break;
    case ONIG_GRAPH_EDGE_RESUME_SUMMARY:
      style = "dotted";
      color = "gray40";
      break;
    case ONIG_GRAPH_EDGE_CALL:
      color = "blue";
      break;
    default:
      break;
    }
    fprintf(f, "  n%d -> n%d [label=\"%s\", style=%s, color=%s];\n",
            e->from, e->to, onig_graph_edge_type_name(e->type), style, color);
  }

  /* Call-graph summary as a separate subgraph cluster of entries. */
  if (onig_graph_call_edge_count(g) > 0) {
    fputs("  subgraph cluster_calls {\n", f);
    fputs("    label=\"call graph\";\n    style=dashed;\n", f);
    for (i = 0; i < onig_graph_entry_count(g); i++)
      fprintf(f, "    e%d [label=\"entry %d\", shape=ellipse];\n",
              onig_graph_entry(g, i), onig_graph_entry(g, i));
    for (i = 0; i < onig_graph_call_edge_count(g); i++) {
      const OnigGraphCallEdge* e = onig_graph_call_edge(g, i);
      fprintf(f, "    e%d -> e%d [label=\"call\", color=blue];\n",
              e->from_entry, e->to_entry);
    }
    fputs("  }\n", f);
  }

  fputs("}\n", f);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int
main(int argc, char** argv)
{
  const char* pattern_file = 0;
  const char* pattern_text = 0;
  const char* format = "json";
  unsigned char* data = 0;
  size_t len = 0;
  int i;
  int rc;
  OnigEncoding enc = ONIG_ENCODING_ASCII;
  OnigRegex regex = 0;
  OnigErrorInfo einfo;
  OnigGraphError gerr;
  OnigGraph* graph = 0;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--pattern-file") == 0) {
      if (++i >= argc) { usage(stderr); return 2; }
      pattern_file = argv[i];
    }
    else if (strcmp(argv[i], "--pattern") == 0) {
      if (++i >= argc) { usage(stderr); return 2; }
      pattern_text = argv[i];
    }
    else if (strcmp(argv[i], "--format") == 0) {
      if (++i >= argc) { usage(stderr); return 2; }
      format = argv[i];
    }
    else if (strcmp(argv[i], "--help") == 0) {
      usage(stdout);
      return 0;
    }
    else {
      fprintf(stderr, "unknown argument: %s\n", argv[i]);
      usage(stderr);
      return 2;
    }
  }

  if ((pattern_file == 0) == (pattern_text == 0)) {
    fprintf(stderr, "exactly one of --pattern-file or --pattern is required\n");
    usage(stderr);
    return 2;
  }
  if (strcmp(format, "json") != 0 && strcmp(format, "dot") != 0) {
    fprintf(stderr, "unknown format: %s\n", format);
    return 2;
  }

  if (pattern_file != 0) {
    char err[256];
    if (! read_file(pattern_file, &data, &len, err, sizeof(err))) {
      fprintf(stderr, "regex_graph: %s\n", err);
      return 3;
    }
  }
  else {
    len = strlen(pattern_text);
    data = (unsigned char* )malloc(len == 0 ? 1 : len);
    if (data == 0) {
      fprintf(stderr, "regex_graph: out of memory\n");
      return 3;
    }
    if (len > 0) memcpy(data, pattern_text, len);
  }

  rc = onig_initialize(&enc, 1);
  if (rc != ONIG_NORMAL) {
    fprintf(stderr, "regex_graph: onig_initialize failed: %d\n", rc);
    free(data);
    return 4;
  }

  rc = onig_new(&regex, data, data + len, ONIG_OPTION_NONE, enc,
                ONIG_SYNTAX_RUBY, &einfo);
  if (rc != ONIG_NORMAL) {
    UChar message[ONIG_MAX_ERROR_MESSAGE_LEN];
    onig_error_code_to_str(message, rc, &einfo);
    fprintf(stderr, "regex_graph: compile failed (%d): %s\n", rc,
            (char* )message);
    onig_end();
    free(data);
    return 5;
  }

  graph = onig_graph_new(regex, &gerr);
  if (graph == 0) {
    fprintf(stderr,
            "regex_graph: graph failed: reason=%s op_index=%d opcode=%d: %s\n",
            onig_graph_error_reason_name((int)gerr.reason), gerr.op_index,
            gerr.opcode, gerr.message);
    onig_free(regex);
    onig_end();
    free(data);
    return 6;
  }

  /* The graph owns its data; the regex is not needed anymore. */
  onig_free(regex);
  regex = 0;

  if (strcmp(format, "json") == 0) print_json(stdout, graph, data, len);
  else print_dot(stdout, graph);

  onig_graph_free(graph);
  onig_end();
  free(data);
  return 0;
}
