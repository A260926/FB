/*
 * regex_graph_determinism.c - byte-identical output across processes and
 * allocation orders.
 *
 * Modes:
 *   emit FILE   Build graphs for a fixed pattern set, dump a canonical
 *               signature (never containing a raw address) and the JSON
 *               and DOT text, all into FILE.
 *   Patterns are compiled in a shuffled order and freed in a different
 *   order than they were built, so the allocator hands out different
 *   addresses than in the reference run.  If any output embedded a
 *   pointer or depended on allocation order, two runs would differ.
 *
 * The shell driver compares the output of two separate processes, and
 * the output of the direct-threaded and switch builds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reggraph.h"

static const char* kPatterns[] = {
  "ab[cd].",
  "(?:ab|c[de])f",
  "(?:a|b|c)d",
  "(?:ab|cd){2,5}",
  "(?:a|bc){3,12}",
  "(?:a|bc){2,5}?",
  "(?<piece>ab){0}\\g<piece>\\g<piece>",
  "(?<node>a(?:\\g<node>)?b)",
  "(?<left>a\\g<right>?b){0}(?<right>c\\g<left>?d){0}\\g<left>",
  "((?:a|b){2,4}){2,4}",
  "(?=a)b",
  "a\\Kb"
};
#define NPAT ((int)(sizeof(kPatterns)/sizeof(kPatterns[0])))

static void
dump_escape(FILE* f, const unsigned char* s, int len)
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

static int
build(const char* pattern, OnigGraph** out_graph, OnigRegex* out_regex)
{
  OnigEncoding enc = ONIG_ENCODING_ASCII;
  OnigErrorInfo ei;
  OnigGraphError ge;
  OnigRegex re = 0;
  int rc = onig_new(&re, (const UChar* )pattern,
                    (const UChar* )(pattern + strlen(pattern)),
                    ONIG_OPTION_NONE, enc, ONIG_SYNTAX_RUBY, &ei);
  if (rc != ONIG_NORMAL) {
    fprintf(stderr, "compile failed: %s\n", pattern);
    return 0;
  }
  *out_graph = onig_graph_new(re, &ge);
  if (*out_graph == 0) {
    fprintf(stderr, "graph failed: %s: %s\n", pattern, ge.message);
    onig_free(re);
    return 0;
  }
  *out_regex = re;
  return 1;
}

static void
dump_graph(FILE* f, const OnigGraph* g, int slot)
{
  int i, j;

  fprintf(f, "GRAPH %d\n", slot);
  fprintf(f, "nodes=%d edges=%d entries=%d calls=%d repeats=%d "
             "call_edges=%d sccs=%d\n",
          onig_graph_node_count(g), onig_graph_edge_count(g),
          onig_graph_entry_count(g), onig_graph_call_count(g),
          onig_graph_repeat_count(g), onig_graph_call_edge_count(g),
          onig_graph_scc_count(g));
  for (i = 0; i < onig_graph_node_count(g); i++) {
    const OnigGraphNode* n = onig_graph_node(g, i);
    fprintf(f, "N %d %s %u %d", n->index, n->opcode_name, n->flags,
            n->out_edge_count);
    if (n->literal != 0) {
      fprintf(f, " lit=");
      dump_escape(f, n->literal, n->literal_len);
    }
    fputc('\n', f);
  }
  for (i = 0; i < onig_graph_edge_count(g); i++) {
    const OnigGraphEdge* e = onig_graph_edge(g, i);
    fprintf(f, "E %d %d %s %d %d\n", e->from, e->to,
            onig_graph_edge_type_name(e->type), e->deferred, e->conditional);
  }
  for (i = 0; i < onig_graph_call_count(g); i++) {
    const OnigGraphCall* c = onig_graph_call(g, i);
    fprintf(f, "C %d %d %d\n", c->call_index, c->target, c->resume_index);
  }
  for (i = 0; i < onig_graph_repeat_count(g); i++) {
    const OnigGraphRepeat* r = onig_graph_repeat(g, i);
    fprintf(f, "R %d %d %d %d %d %d\n", r->entry_index, r->inc_index,
            r->body_index, r->lower, r->upper, r->greedy);
  }
  for (i = 0; i < onig_graph_entry_count(g); i++)
    fprintf(f, "ENT %d\n", onig_graph_entry(g, i));
  for (i = 0; i < onig_graph_call_edge_count(g); i++) {
    const OnigGraphCallEdge* e = onig_graph_call_edge(g, i);
    fprintf(f, "CE %d %d %d %d\n", e->from_entry, e->to_entry,
            e->first_call_index, e->call_count);
  }
  for (i = 0; i < onig_graph_scc_count(g); i++) {
    const OnigGraphScc* s = onig_graph_scc(g, i);
    fprintf(f, "S %d %d %d", s->id, s->recursive, s->member_count);
    for (j = 0; j < s->member_count; j++) fprintf(f, " %d", s->members[j]);
    fputc('\n', f);
  }
  for (i = 0; i < onig_graph_node_count(g); i++)
    fprintf(f, "REACH %d %d\n", i, onig_graph_node_reachable(g, i));
}

int
main(int argc, char** argv)
{
  OnigEncoding enc = ONIG_ENCODING_ASCII;
  OnigGraph* graphs[NPAT];
  OnigRegex regexes[NPAT];
  int order[NPAT];
  int i;
  FILE* out;
  unsigned int seed;

  if (argc < 3 || strcmp(argv[1], "emit") != 0) {
    fprintf(stderr, "usage: %s emit FILE [seed]\n", argv[0]);
    return 2;
  }
  seed = (argc >= 4 ? (unsigned int)strtoul(argv[3], 0, 10) : 1u);

  if (onig_initialize(&enc, 1) != ONIG_NORMAL) return 3;

  /* Shuffle the build order deterministically from the seed. */
  for (i = 0; i < NPAT; i++) order[i] = i;
  for (i = NPAT - 1; i > 0; i--) {
    unsigned int r;
    int t;
    seed = seed * 1103515245u + 12345u;
    r = (seed >> 8) % (unsigned int)(i + 1);
    t = order[i]; order[i] = order[(int)r]; order[(int)r] = t;
  }

  for (i = 0; i < NPAT; i++) {
    int p = order[i];
    if (! build(kPatterns[p], &graphs[p], &regexes[p])) {
      onig_end();
      return 4;
    }
  }

  /* Free in a different order than built, so allocation addresses differ
     from the reference run. */
  for (i = 0; i < NPAT; i++) {
    int p = (i % 2 == 0 ? i / 2 : NPAT - 1 - i / 2);
    onig_free(regexes[p]);
    regexes[p] = 0;
  }

  out = fopen(argv[2], "wb");
  if (out == 0) {
    fprintf(stderr, "cannot open output file\n");
    return 5;
  }
  for (i = 0; i < NPAT; i++) {
    dump_graph(out, graphs[i], i);
    onig_graph_free(graphs[i]);
  }
  fclose(out);
  onig_end();
  return 0;
}
