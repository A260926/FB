/*
 * regex_graph_contract.c - contract tests for the read-only regex graph.
 *
 * Four independent groups, each with its own assertion count:
 *
 *   G1 linear / branch    - linear instructions, typed jumps, conditional
 *                           alternatives, index-based node identity
 *   G2 repeat             - repeat_range decoding, greedy / non-greedy,
 *                           bounded / unbounded, nested repeat
 *   G3 call / recursion   - CALL/RETURN, resume summaries, call graph,
 *                           shared callees, SCCs, self and mutual recursion
 *   G4 identity/lifetime  - node identity, graph ownership after
 *                           onig_free(), deterministic ordering, dense
 *                           unreachable nodes retained, failure cleanup
 *
 * Expected graphs are hand-derived from the 6.9.5 compiler output and
 * are independent of the production builder: they are literal tables of
 * (opcode, edge type, target) triples, not re-serialisations compared
 * against the builder.
 *
 * A group is only reported as passing when it ran at least
 * MIN_GROUP_CHECKS assertions; an empty group is a failure.  If an
 * assertion fails the process exits non-zero immediately with the
 * group and check number.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reggraph.h"

#define MIN_GROUP_CHECKS 12
#define MAX_FAILURES 1

static int g_failures = 0;
static int g_checks_this_group = 0;
static const char* g_group = "?";

#define CHECK(cond, fmt, ...) \
  do { \
    g_checks_this_group++; \
    if (! (cond)) { \
      fprintf(stderr, "FAIL [%s] check %d: ", g_group, g_checks_this_group); \
      fprintf(stderr, fmt, ##__VA_ARGS__); \
      fprintf(stderr, "\n"); \
      g_failures++; \
      return 0; \
    } \
  } while (0)

#define CHECK_EQ_INT(actual, expected, what) \
  CHECK((actual) == (expected), "%s: expected %d, got %d", what, \
        (int)(expected), (int)(actual))

/* ------------------------------------------------------------------ */
/* Small independent helpers.                                          */
/* ------------------------------------------------------------------ */

typedef struct {
  int from;
  int to;
  int type;
} RefEdge;

typedef struct {
  const char* pattern;
  int node_count;
} RefShape;

static OnigEncoding kEnc;

static int
compile_pattern(const char* pattern, OnigRegex* out)
{
  OnigErrorInfo e;
  return onig_new(out, (const UChar* )pattern,
                  (const UChar* )(pattern + strlen(pattern)),
                  ONIG_OPTION_NONE, kEnc, ONIG_SYNTAX_RUBY, &e);
}

static int
graph_for(const char* pattern, OnigGraph** out_graph, OnigRegex* out_regex)
{
  OnigRegex re = 0;
  OnigGraphError err;
  OnigGraph* g;
  int rc = compile_pattern(pattern, &re);
  if (rc != ONIG_NORMAL) {
    fprintf(stderr, "compile failed for %s: %d\n", pattern, rc);
    return 0;
  }
  g = onig_graph_new(re, &err);
  if (g == 0) {
    fprintf(stderr, "graph failed for %s: %s\n", pattern, err.message);
    onig_free(re);
    return 0;
  }
  *out_graph = g;
  *out_regex = re;
  return 1;
}

static int
has_edge(const OnigGraph* g, int from, int to, int type)
{
  int i;
  for (i = 0; i < onig_graph_out_edge_count(g, from); i++) {
    const OnigGraphEdge* e = onig_graph_out_edge(g, from, i);
    if (e->to == to && e->type == type) return 1;
  }
  return 0;
}

static int
count_type_out(const OnigGraph* g, int from, int type)
{
  int i, n = 0;
  for (i = 0; i < onig_graph_out_edge_count(g, from); i++)
    if (onig_graph_out_edge(g, from, i)->type == type) n++;
  return n;
}

static int
node_opcode(const OnigGraph* g, int i)
{
  const OnigGraphNode* n = onig_graph_node(g, i);
  return (n == 0 ? -1 : n->opcode);
}

/* Build a canonical (from, type, to) signature of the whole graph so
   ordering can be compared across two independent builds. */
static int
signature(const OnigGraph* g, char* buf, size_t bufsz)
{
  int i, o = 0;
  for (i = 0; i < onig_graph_edge_count(g); i++) {
    const OnigGraphEdge* e = onig_graph_edge(g, i);
    int w = snprintf(buf + o, (o < (int)bufsz ? bufsz - (size_t)o : 0),
                     "%d:%d:%d:%d:%d;", e->from, e->to, e->type, e->deferred,
                     e->conditional);
    if (w < 0 || o + w >= (int)bufsz) return 0;
    o += w;
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* G1: linear and branch                                              */
/* ------------------------------------------------------------------ */

static int
group_linear_and_branch(void)
{
  static const RefEdge abcd_edges[] = {
    { 0, 1, ONIG_GRAPH_EDGE_FALLTHROUGH },
    { 1, 2, ONIG_GRAPH_EDGE_FALLTHROUGH },
    { 2, 3, ONIG_GRAPH_EDGE_FALLTHROUGH }
  };
  static const RefEdge alt_edges[] = {
    { 0, 1, ONIG_GRAPH_EDGE_FALLTHROUGH },
    { 0, 3, ONIG_GRAPH_EDGE_ALT_PUSH },
    { 1, 2, ONIG_GRAPH_EDGE_FALLTHROUGH },
    { 2, 5, ONIG_GRAPH_EDGE_JUMP },
    { 3, 4, ONIG_GRAPH_EDGE_FALLTHROUGH },
    { 4, 5, ONIG_GRAPH_EDGE_FALLTHROUGH },
    { 5, 6, ONIG_GRAPH_EDGE_FALLTHROUGH }
  };
  static const RefEdge chain_edges[] = {
    { 0, 1, ONIG_GRAPH_EDGE_FALLTHROUGH },
    { 0, 3, ONIG_GRAPH_EDGE_ALT_PUSH },
    { 1, 2, ONIG_GRAPH_EDGE_FALLTHROUGH },
    { 2, 7, ONIG_GRAPH_EDGE_JUMP },
    { 3, 4, ONIG_GRAPH_EDGE_FALLTHROUGH },
    { 3, 6, ONIG_GRAPH_EDGE_ALT_PUSH },
    { 4, 5, ONIG_GRAPH_EDGE_FALLTHROUGH },
    { 5, 7, ONIG_GRAPH_EDGE_JUMP },
    { 6, 7, ONIG_GRAPH_EDGE_FALLTHROUGH },
    { 7, 8, ONIG_GRAPH_EDGE_FALLTHROUGH }
  };
  struct {
    const char* pattern;
    int nodes;
    const RefEdge* edges;
    int edge_count;
  } cases[] = {
    { "ab[cd].", 4, abcd_edges, (int)(sizeof(abcd_edges)/sizeof(abcd_edges[0])) },
    { "(?:ab|c[de])f", 7, alt_edges,
      (int)(sizeof(alt_edges)/sizeof(alt_edges[0])) },
    { "(?:a|b|c)d", 9, chain_edges,
      (int)(sizeof(chain_edges)/sizeof(chain_edges[0])) }
  };
  int c;

  g_group = "linear-branch";
  g_checks_this_group = 0;

  for (c = 0; c < (int)(sizeof(cases)/sizeof(cases[0])); c++) {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    int i;

    if (! graph_for(cases[c].pattern, &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_node_count(g), cases[c].nodes, "node count");
    CHECK_EQ_INT(onig_graph_edge_count(g), cases[c].edge_count, "edge count");
    for (i = 0; i < cases[c].edge_count; i++) {
      CHECK(has_edge(g, cases[c].edges[i].from, cases[c].edges[i].to,
                     cases[c].edges[i].type),
            "missing hand-derived edge %d->%d type %d", cases[c].edges[i].from,
            cases[c].edges[i].to, cases[c].edges[i].type);
    }
    for (i = 0; i < onig_graph_node_count(g); i++) {
      CHECK_EQ_INT(onig_graph_node(g, i)->index, i, "node identity");
    }
    onig_graph_free(g);
    onig_free(re);
  }

  /* Literal decode: "ab" is one STR_2 with two owned bytes. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    const OnigGraphNode* n;
    if (! graph_for("ab[cd].", &g, &re)) return 0;
    n = onig_graph_node(g, 0);
    CHECK(n->opcode == ONIG_GRAPH_OPCODE_STR_2, "op 0 should be str_2");
    CHECK_EQ_INT(n->literal_len, 2, "literal length");
    CHECK(n->literal != 0 && n->literal[0] == 'a' && n->literal[1] == 'b',
          "literal bytes");
    /* Deleting the regex must not invalidate the literal copy. */
    onig_free(re);
    CHECK(n->literal[0] == 'a' && n->literal[1] == 'b',
          "literal copy survives onig_free");
    onig_graph_free(g);
  }

  /* Conditional branch: OP_PUSH_OR_JUMP_EXACT1 has immediate condition
     edges and a deferred push; it must not be modelled as an
     unconditional jump or a fallthrough pair. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    const OnigGraphEdge* e;
    int i, found_cond_jump = 0, found_cond_push = 0;
    if (! graph_for("a*", &g, &re)) return 0;
    CHECK_EQ_INT(node_opcode(g, 0), ONIG_GRAPH_OPCODE_PUSH_OR_JUMP_EXACT1,
                 "a* head opcode");
    for (i = 0; i < onig_graph_out_edge_count(g, 0); i++) {
      e = onig_graph_out_edge(g, 0, i);
      if (e->type == ONIG_GRAPH_EDGE_COND_JUMP) {
        found_cond_jump = 1;
        CHECK_EQ_INT(e->to, 3, "cond jump target");
        CHECK_EQ_INT(e->deferred, 0, "cond jump is immediate");
      }
      if (e->type == ONIG_GRAPH_EDGE_COND_PUSH) {
        found_cond_push = 1;
        CHECK_EQ_INT(e->to, 3, "cond push target");
        CHECK_EQ_INT(e->deferred, 1, "cond push is deferred");
      }
      CHECK(e->type != ONIG_GRAPH_EDGE_FALLTHROUGH,
            "conditional op must not have a plain fallthrough");
    }
    CHECK(found_cond_jump, "missing cond-jump");
    CHECK(found_cond_push, "missing cond-push");
    CHECK(has_edge(g, 0, 1, ONIG_GRAPH_EDGE_COND_CONTINUE),
          "missing cond-continue");
    onig_graph_free(g);
    onig_free(re);
  }

  /* FAIL / END are exits with no static successor.  (?!) compiles to a
     PUSH/MARK/POP_TO_MARK/POP/FAIL sequence in 6.9.5; MARK,
     POP_TO_MARK and POP are part of the bookkeeping subset, so this must
     build successfully and expose FAIL as a dynamic backtrack exit. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    int i, fail_at = -1;
    if (! graph_for("(?!)", &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_node_count(g), 6, "(?!) node count");
    for (i = 0; i < onig_graph_node_count(g); i++)
      if (node_opcode(g, i) == ONIG_GRAPH_OPCODE_FAIL) fail_at = i;
    CHECK_EQ_INT(fail_at, 4, "FAIL index");
    CHECK_EQ_INT(onig_graph_out_edge_count(g, fail_at), 0,
                 "FAIL has no static successor");
    CHECK((onig_graph_node(g, fail_at)->flags & ONIG_GRAPH_NODE_FAIL_EXIT) != 0,
          "FAIL flagged as dynamic backtrack exit");
    CHECK((onig_graph_node(g, fail_at)->flags &
           ONIG_GRAPH_NODE_DYNAMIC_BACKTRACK_EXIT) != 0,
          "FAIL flagged as dynamic backtrack exit (alias)");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Positive lookahead: MARK ... CUT_TO_MARK is a linear bookkeeping
     pair with ordinary fallthrough, not a rejected opcode. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    if (! graph_for("(?=a)b", &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_node_count(g), 5, "lookahead node count");
    CHECK_EQ_INT(node_opcode(g, 0), ONIG_GRAPH_OPCODE_MARK, "mark op");
    CHECK_EQ_INT(node_opcode(g, 2), ONIG_GRAPH_OPCODE_CUT_TO_MARK, "cut op");
    CHECK(has_edge(g, 0, 1, ONIG_GRAPH_EDGE_FALLTHROUGH), "mark fallthrough");
    CHECK(has_edge(g, 2, 3, ONIG_GRAPH_EDGE_FALLTHROUGH), "cut fallthrough");
    onig_graph_free(g);
    onig_free(re);
  }

  /* \K uses SAVE_VAL / UPDATE_VAR, both bookkeeping instructions. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    int i, found = 0;
    if (! graph_for("a\\Kb", &g, &re)) return 0;
    CHECK_EQ_INT(node_opcode(g, 1), ONIG_GRAPH_OPCODE_SAVE_VAL, "keep save");
    CHECK_EQ_INT(node_opcode(g, 3), ONIG_GRAPH_OPCODE_UPDATE_VAR,
                 "keep update");
    for (i = 0; i < onig_graph_node_count(g); i++)
      if (node_opcode(g, i) == ONIG_GRAPH_OPCODE_FAIL) found = 1;
    CHECK(! found, "a\\Kb has no FAIL");
    onig_graph_free(g);
    onig_free(re);
  }
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    if (! graph_for("ab[cd].", &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_out_edge_count(g, 3), 0, "END has no edges");
    CHECK((onig_graph_node(g, 3)->flags & ONIG_GRAPH_NODE_SUCCESS_EXIT) != 0,
          "END flagged as success exit");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Node identity is the Operation index, dense and never compacted:
     every compiled instruction keeps its own index even when an earlier
     jump makes a whole run unreachable. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    int i, unreachable = 0;
    if (! graph_for("(?<piece>ab){0}\\g<piece>\\g<piece>", &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_node_count(g), 10, "dense node count");
    for (i = 0; i < onig_graph_node_count(g); i++) {
      CHECK_EQ_INT(onig_graph_node(g, i)->index, i, "dense identity");
      if (! onig_graph_node_reachable(g, i)) unreachable++;
    }
    /* Hand-derived: ops 1 (CALL) and 2 (JUMP) are skipped by the jump at
       op 0, so they stay present but flagged unreachable. */
    CHECK_EQ_INT(unreachable, 2, "unreachable node count");
    CHECK_EQ_INT(node_opcode(g, 3), ONIG_GRAPH_OPCODE_MEM_START_PUSH, "op 3");
    onig_graph_free(g);
    onig_free(re);
  }

  return g_checks_this_group >= MIN_GROUP_CHECKS;
}

/* ------------------------------------------------------------------ */
/* G2: repeat                                                         */
/* ------------------------------------------------------------------ */

static int
group_repeat(void)
{
  g_group = "repeat";
  g_checks_this_group = 0;

  /* Greedy bounded range: REPEAT / REPEAT_INC with repeat_range
     lower=2 upper=5 and body converted to an operation index.  The
     body must be long enough that 6.9.5 does not expand the quantifier
     into a PUSH chain (hand-checked: an alternation body does not). */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    const OnigGraphRepeat* r;
    if (! graph_for("(?:ab|cd){2,5}", &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_repeat_count(g), 1, "one repeat record");
    r = onig_graph_repeat(g, 0);
    CHECK_EQ_INT(r->entry_index, 0, "repeat entry");
    CHECK_EQ_INT(r->inc_index, 5, "repeat inc");
    CHECK_EQ_INT(r->body_index, 1, "repeat body");
    CHECK_EQ_INT(r->lower, 2, "repeat lower");
    CHECK_EQ_INT(r->upper, 5, "repeat upper");
    CHECK_EQ_INT(r->greedy, 1, "repeat greedy");
    CHECK_EQ_INT(node_opcode(g, r->body_index), ONIG_GRAPH_OPCODE_PUSH,
                 "body opcode");
    CHECK(onig_graph_repeat_at(g, 0) == r, "repeat_at(entry)");
    CHECK(onig_graph_repeat_at(g, 5) == r, "repeat_at(inc)");
    /* Typed edges: immediate body entry, conditional exit(s). */
    CHECK(has_edge(g, 0, 1, ONIG_GRAPH_EDGE_REPEAT_ENTER),
          "repeat enter");
    CHECK(has_edge(g, 5, 1, ONIG_GRAPH_EDGE_REPEAT_BACK), "repeat back");
    CHECK(has_edge(g, 5, 6, ONIG_GRAPH_EDGE_REPEAT_EXIT), "repeat exit");
    /* No ordinary fallthrough on the repeat entry: the body relation is
       its own edge type. */
    CHECK_EQ_INT(count_type_out(g, 0, ONIG_GRAPH_EDGE_FALLTHROUGH), 0,
                 "repeat entry must not fallthrough");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Non-greedy range: REPEAT_NG / REPEAT_INC_NG, greedy == 0. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    const OnigGraphRepeat* r;
    if (! graph_for("(?:a|bc){2,5}?", &g, &re)) return 0;
    r = onig_graph_repeat(g, 0);
    CHECK_EQ_INT(r->greedy, 0, "non-greedy flag");
    CHECK_EQ_INT(r->entry_index, 0, "ng entry");
    CHECK_EQ_INT(r->inc_index, 5, "ng inc");
    CHECK_EQ_INT(r->body_index, 1, "ng body");
    CHECK(has_edge(g, 0, 1, ONIG_GRAPH_EDGE_REPEAT_NG_ENTER),
          "ng enter");
    CHECK(has_edge(g, 5, 6, ONIG_GRAPH_EDGE_REPEAT_NG_EXIT), "ng exit");
    CHECK(has_edge(g, 5, 1, ONIG_GRAPH_EDGE_REPEAT_NG_BACK), "ng back");
    CHECK_EQ_INT(node_opcode(g, 0), ONIG_GRAPH_OPCODE_REPEAT_NG, "ng opcode");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Unbounded range: upper is the sentinel, not a fake finite number.
     Only patterns whose body is long enough to defeat quantifier
     expansion keep a REPEAT/REPEAT_INC pair (hand-checked against 6.9.5
     output for "(?:a|bc){3,}"). */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    const OnigGraphRepeat* r;
    if (! graph_for("(?:a|bc){3,}", &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_repeat_count(g), 1, "unbounded repeat count");
    r = onig_graph_repeat(g, 0);
    CHECK_EQ_INT(r->lower, 3, "unbounded lower");
    CHECK_EQ_INT(r->upper, ONIG_GRAPH_INFINITE_REPEAT, "unbounded upper");
    CHECK_EQ_INT(r->greedy, 1, "unbounded greedy");
    CHECK_EQ_INT(r->entry_index, 0, "unbounded entry");
    CHECK_EQ_INT(r->inc_index, 5, "unbounded inc");
    CHECK_EQ_INT(r->body_index, 1, "unbounded body");
    CHECK(has_edge(g, 5, 1, ONIG_GRAPH_EDGE_REPEAT_BACK), "unbounded back");
    CHECK(has_edge(g, 5, 6, ONIG_GRAPH_EDGE_REPEAT_EXIT), "unbounded exit");
    /* Unbounded: the loop-exit edge exists only as the deferred
       alternative the VM pushes; there is no "n >= upper" exit. */
    CHECK_EQ_INT(count_type_out(g, 5, ONIG_GRAPH_EDGE_REPEAT_EXIT), 1,
                 "unbounded has one deferred exit");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Alternation body inside a repeat: the body index is the first
     compiled instruction of the body, and the branch keeps its own
     deferred alternative. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    const OnigGraphRepeat* r;
    if (! graph_for("(?:a|bc){3,12}", &g, &re)) return 0;
    r = onig_graph_repeat(g, 0);
    CHECK_EQ_INT(r->entry_index, 0, "alt body entry");
    CHECK_EQ_INT(r->body_index, 1, "alt body index");
    CHECK_EQ_INT(node_opcode(g, 1), ONIG_GRAPH_OPCODE_PUSH, "alt body push");
    CHECK(has_edge(g, 1, 2, ONIG_GRAPH_EDGE_FALLTHROUGH), "alt body first");
    CHECK(has_edge(g, 1, 4, ONIG_GRAPH_EDGE_ALT_PUSH), "alt body second");
    CHECK(has_edge(g, 5, 1, ONIG_GRAPH_EDGE_REPEAT_BACK), "alt repeat back");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Nested repeat: two independent repeat_range records with distinct
     body indices. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    const OnigGraphRepeat* outer;
    const OnigGraphRepeat* inner;
    if (! graph_for("((?:a|b){2,4}){2,4}", &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_repeat_count(g), 2, "nested repeat count");
    outer = onig_graph_repeat_at(g, 0);
    inner = onig_graph_repeat_at(g, 2);
    CHECK(outer != 0 && inner != 0, "both repeats resolvable");
    CHECK_EQ_INT(outer->entry_index, 0, "outer entry");
    CHECK_EQ_INT(outer->inc_index, 9, "outer inc");
    CHECK_EQ_INT(outer->body_index, 1, "outer body");
    CHECK_EQ_INT(inner->entry_index, 2, "inner entry");
    CHECK_EQ_INT(inner->inc_index, 7, "inner inc");
    CHECK_EQ_INT(inner->body_index, 3, "inner body");
    CHECK(inner->body_index > outer->body_index,
          "nested body is inside the outer body");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Zero-lower greedy repeat ({0,3} expands to PUSH chains): the skip
     relation is a deferred alternative, never a fallthrough. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    if (! graph_for("a{0,3}", &g, &re)) return 0;
    CHECK_EQ_INT(node_opcode(g, 0), ONIG_GRAPH_OPCODE_PUSH, "push head");
    CHECK(has_edge(g, 0, 1, ONIG_GRAPH_EDGE_FALLTHROUGH), "push continue");
    CHECK(has_edge(g, 0, 6, ONIG_GRAPH_EDGE_ALT_PUSH), "push skip");
    CHECK_EQ_INT(onig_graph_repeat_count(g), 0,
                 "expanded {0,3} has no repeat_range record");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Empty-check loop: EMPTY_CHECK_END has an ordinary fallthrough plus a
     distinct conditional skip edge.  Hand-derived for "(?:a|)*":
     0 PUSH, 1 EMPTY_CHECK_START, 2 PUSH, 3 STR_1, 4 JUMP->5,
     5 EMPTY_CHECK_END, 6 JUMP->0, 7 END. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    if (! graph_for("(?:a|)*", &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_node_count(g), 8, "empty-check node count");
    CHECK_EQ_INT(node_opcode(g, 1), ONIG_GRAPH_OPCODE_EMPTY_CHECK_START,
                 "empty-check start");
    CHECK_EQ_INT(node_opcode(g, 5), ONIG_GRAPH_OPCODE_EMPTY_CHECK_END,
                 "empty-check end");
    CHECK(has_edge(g, 5, 6, ONIG_GRAPH_EDGE_FALLTHROUGH),
          "empty-check fallthrough");
    CHECK(has_edge(g, 5, 7, ONIG_GRAPH_EDGE_EMPTY_CHECK_SKIP),
          "empty-check skip");
    CHECK_EQ_INT(count_type_out(g, 5, ONIG_GRAPH_EDGE_EMPTY_CHECK_SKIP), 1,
                 "exactly one skip edge");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Bounded repetition that expands to a chain: hand-derived edges. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    static const RefEdge chain[] = {
      { 0, 1, ONIG_GRAPH_EDGE_FALLTHROUGH },
      { 1, 2, ONIG_GRAPH_EDGE_FALLTHROUGH },
      { 2, 3, ONIG_GRAPH_EDGE_FALLTHROUGH },
      { 3, 4, ONIG_GRAPH_EDGE_FALLTHROUGH },
      { 4, 5, ONIG_GRAPH_EDGE_FALLTHROUGH },
      { 5, 6, ONIG_GRAPH_EDGE_FALLTHROUGH }
    };
    int i;
    if (! graph_for("a{0,3}", &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_node_count(g), 7, "chain node count");
    for (i = 0; i < 6; i++) {
      CHECK(has_edge(g, chain[i].from, chain[i].to, chain[i].type),
            "chain edge %d->%d", chain[i].from, chain[i].to);
    }
    onig_graph_free(g);
    onig_free(re);
  }

  return g_checks_this_group >= MIN_GROUP_CHECKS;
}

/* ------------------------------------------------------------------ */
/* G3: calls and recursion                                            */
/* ------------------------------------------------------------------ */

static int
group_calls_and_recursion(void)
{
  g_group = "calls-recursion";
  g_checks_this_group = 0;

  /* Shared callee: two explicit calls to the same subexpression.  The
     implicit call that runs the group definition once (op 1) is a third
     call site, so the call *instruction* count is 3 while the call graph
     still has the two hand-visible call edges. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    const OnigGraphCall* c;
    const OnigGraphCallEdge* ce;
    if (! graph_for("(?<piece>ab){0}\\g<piece>\\g<piece>", &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_call_count(g), 3, "call instruction count");
    c = onig_graph_call(g, 0);
    CHECK_EQ_INT(c->call_index, 1, "implicit call index");
    CHECK_EQ_INT(c->target, 3, "implicit call target");
    c = onig_graph_call(g, 1);
    CHECK_EQ_INT(c->call_index, 7, "first call index");
    CHECK_EQ_INT(c->target, 3, "first call target");
    CHECK_EQ_INT(c->resume_index, 8, "first resume");
    CHECK(onig_graph_call_at(g, 8) != 0, "call_at 8");
    CHECK_EQ_INT(onig_graph_call_at(g, 8)->target, 3, "second call target");
    /* Calls have a call edge and a resume summary, never a plain
       fallthrough. */
    CHECK(has_edge(g, 7, 3, ONIG_GRAPH_EDGE_CALL), "call edge 7->3");
    CHECK(has_edge(g, 7, 8, ONIG_GRAPH_EDGE_RESUME_SUMMARY),
          "resume summary 7->8");
    CHECK_EQ_INT(count_type_out(g, 7, ONIG_GRAPH_EDGE_FALLTHROUGH), 0,
                 "call has no fallthrough");
    /* Resume summary is annotated as a summary, not a VM edge. */
    {
      int i, seen = 0;
      for (i = 0; i < onig_graph_out_edge_count(g, 7); i++) {
        const OnigGraphEdge* e = onig_graph_out_edge(g, 7, i);
        if (e->type == ONIG_GRAPH_EDGE_RESUME_SUMMARY) {
          seen = 1;
          CHECK_EQ_INT(e->deferred, 0, "summary is immediate");
          CHECK_EQ_INT(e->conditional, 0, "summary is unconditional");
        }
      }
      CHECK(seen, "summary edge present");
    }
    /* RET is a dynamic return exit with no static successor. */
    CHECK_EQ_INT(node_opcode(g, 6), ONIG_GRAPH_OPCODE_RETURN, "return op");
    CHECK_EQ_INT(onig_graph_out_edge_count(g, 6), 0, "return has no edges");
    CHECK((onig_graph_node(g, 6)->flags & ONIG_GRAPH_NODE_DYNAMIC_RETURN) != 0,
          "return flagged dynamic");
    /* Entry set: main entry 0 plus the call target 3. */
    CHECK_EQ_INT(onig_graph_entry_count(g), 2, "entry count");
    CHECK_EQ_INT(onig_graph_entry(g, 0), 0, "entry 0");
    CHECK_EQ_INT(onig_graph_entry(g, 1), 3, "entry 1");
    CHECK(onig_graph_is_entry(g, 3), "3 is an entry");
    /* Call graph: the two call sites 7 and 8 collapse to one 0 -> 3
       edge.  Op 1 (the implicit body call) is not reached from entry 0
       because the first instruction jumps over it, so it contributes no
       call-graph edge. */
    CHECK_EQ_INT(onig_graph_call_edge_count(g), 1, "call edge count");
    ce = onig_graph_call_edge(g, 0);
    CHECK_EQ_INT(ce->from_entry, 0, "call edge from");
    CHECK_EQ_INT(ce->to_entry, 3, "call edge to");
    CHECK_EQ_INT(ce->call_count, 2, "collapsed call site count");
    CHECK_EQ_INT(ce->first_call_index, 7, "first call site index");
    /* No recursion: two singleton SCCs. */
    CHECK_EQ_INT(onig_graph_scc_count(g), 2, "scc count");
    CHECK_EQ_INT(onig_graph_scc(g, 0)->recursive, 0, "entry 0 not recursive");
    CHECK_EQ_INT(onig_graph_scc(g, 1)->recursive, 0, "entry 3 not recursive");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Self recursion: the callee calls itself. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    const OnigGraphScc* s;
    if (! graph_for("(?<node>a(?:\\g<node>)?b)", &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_call_count(g), 2, "self-recursive calls");
    CHECK_EQ_INT(onig_graph_call_edge_count(g), 2, "self-recursive call edges");
    /* 0 -> 2 and 2 -> 2. */
    CHECK_EQ_INT(onig_graph_call_edge(g, 0)->from_entry, 0, "0->2 from");
    CHECK_EQ_INT(onig_graph_call_edge(g, 0)->to_entry, 2, "0->2 to");
    CHECK_EQ_INT(onig_graph_call_edge(g, 1)->from_entry, 2, "2->2 from");
    CHECK_EQ_INT(onig_graph_call_edge(g, 1)->to_entry, 2, "2->2 to");
    s = onig_graph_scc_for_entry(g, 2);
    CHECK(s != 0, "scc for entry 2");
    CHECK_EQ_INT(s->recursive, 1, "entry 2 is recursive");
    CHECK_EQ_INT(s->member_count, 1, "singleton self-loop");
    s = onig_graph_scc_for_entry(g, 0);
    CHECK(s != 0 && s->recursive == 0, "entry 0 not recursive");
    /* The recursive SCC is a self loop, not a size-2 component. */
    CHECK_EQ_INT(onig_graph_scc_count(g), 2, "self recursion scc count");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Mutual recursion: two entries in one recursive SCC, stable id is
     the smallest entry index. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    const OnigGraphScc* s;
    int i, found = 0;
    if (! graph_for("(?<left>a\\g<right>?b){0}(?<right>c\\g<left>?d){0}"
                    "\\g<left>", &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_entry_count(g), 3, "mutual entry count");
    for (i = 0; i < onig_graph_scc_count(g); i++) {
      s = onig_graph_scc(g, i);
      if (s->member_count == 2 && s->recursive) {
        found = 1;
        CHECK_EQ_INT(s->id, 3, "mutual scc stable id");
        CHECK_EQ_INT(s->members[0], 3, "first member");
        CHECK_EQ_INT(s->members[1], 13, "second member");
      }
    }
    CHECK(found, "mutual recursion SCC missing");
    /* Both bodies are shared: entry 3 calls 13 and entry 13 calls 3. */
    {
      int has_3_13 = 0, has_13_3 = 0;
      for (i = 0; i < onig_graph_call_edge_count(g); i++) {
        const OnigGraphCallEdge* ce = onig_graph_call_edge(g, i);
        if (ce->from_entry == 3 && ce->to_entry == 13) has_3_13 = 1;
        if (ce->from_entry == 13 && ce->to_entry == 3) has_13_3 = 1;
      }
      CHECK(has_3_13, "3 -> 13 call edge");
      CHECK(has_13_3, "13 -> 3 call edge");
    }
    /* Singleton components carry no recursion flag. */
    for (i = 0; i < onig_graph_scc_count(g); i++) {
      s = onig_graph_scc(g, i);
      if (s->member_count == 1 && s->id == 0)
        CHECK_EQ_INT(s->recursive, 0, "entry 0 not recursive");
    }
    onig_graph_free(g);
    onig_free(re);
  }

  /* The call graph does not follow neighbouring addresses: a plain
     sequential pattern is one entry and one SCC. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    if (! graph_for("ab[cd].", &g, &re)) return 0;
    CHECK_EQ_INT(onig_graph_entry_count(g), 1, "linear entry count");
    CHECK_EQ_INT(onig_graph_call_edge_count(g), 0, "linear call edges");
    CHECK_EQ_INT(onig_graph_scc_count(g), 1, "linear scc count");
    onig_graph_free(g);
    onig_free(re);
  }

  return g_checks_this_group >= MIN_GROUP_CHECKS;
}

/* ------------------------------------------------------------------ */
/* G4: identity and lifetime                                          */
/* ------------------------------------------------------------------ */

static int
group_identity_lifetime(void)
{
  g_group = "identity-lifetime";
  g_checks_this_group = 0;

  /* Ownership: the graph stays fully readable after onig_free, and a
     later allocation/free churn does not change it. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    char before[4096], after[4096];
    int i;
    if (! graph_for("(?:a|bc){3,12}", &g, &re)) return 0;
    CHECK(signature(g, before, sizeof(before)), "signature before free");
    onig_free(re);
    /* Churn the allocator so a borrowed pointer would be overwritten. */
    for (i = 0; i < 64; i++) {
      void* p = malloc(4096);
      CHECK(p != 0, "allocator churn");
      memset(p, 0xa5, 4096);
      free(p);
    }
    CHECK(signature(g, after, sizeof(after)), "signature after free");
    CHECK(strcmp(before, after) == 0, "graph immutable after regex free");
    CHECK(onig_graph_node_count(g) == 7, "node count stable after free");
    CHECK(node_opcode(g, 1) == ONIG_GRAPH_OPCODE_PUSH, "opcode stable");
    onig_graph_free(g);
  }

  /* Determinism: two independently compiled copies of the same pattern
     produce byte-identical edge signatures and identical identity
     numbering. */
  {
    OnigGraph* g1 = 0;
    OnigGraph* g2 = 0;
    OnigRegex r1 = 0;
    OnigRegex r2 = 0;
    char s1[4096], s2[4096];
    int i;
    if (! graph_for("(?<node>a(?:\\g<node>)?b)", &g1, &r1)) return 0;
    if (! graph_for("(?<node>a(?:\\g<node>)?b)", &g2, &r2)) {
      onig_graph_free(g1); onig_free(r1); return 0;
    }
    CHECK_EQ_INT(onig_graph_node_count(g1), onig_graph_node_count(g2),
                 "deterministic node count");
    CHECK_EQ_INT(onig_graph_edge_count(g1), onig_graph_edge_count(g2),
                 "deterministic edge count");
    CHECK(signature(g1, s1, sizeof(s1)), "signature 1");
    CHECK(signature(g2, s2, sizeof(s2)), "signature 2");
    CHECK(strcmp(s1, s2) == 0, "edge signatures identical");
    for (i = 0; i < onig_graph_node_count(g1); i++) {
      CHECK_EQ_INT(onig_graph_node(g1, i)->opcode,
                   onig_graph_node(g2, i)->opcode, "opcode identity");
    }
    CHECK_EQ_INT(onig_graph_call_edge_count(g1),
                 onig_graph_call_edge_count(g2), "call edges identical");
    onig_graph_free(g1);
    onig_graph_free(g2);
    onig_free(r1);
    onig_free(r2);
  }

  /* Edges are globally sorted by (from, type, to) and out-edge slices
     agree with the global list. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    int i, j, k;
    if (! graph_for("((?:a|b){2,4}){2,4}", &g, &re)) return 0;
    for (i = 1; i < onig_graph_edge_count(g); i++) {
      const OnigGraphEdge* a = onig_graph_edge(g, i - 1);
      const OnigGraphEdge* b = onig_graph_edge(g, i);
      CHECK(a->from < b->from ||
            (a->from == b->from && (a->type < b->type ||
             (a->type == b->type && a->to <= b->to))),
            "global edge order at %d", i);
    }
    k = 0;
    for (i = 0; i < onig_graph_node_count(g); i++) {
      CHECK_EQ_INT(onig_graph_out_edge_count(g, i),
                   onig_graph_node(g, i)->out_edge_count,
                   "out edge count agrees");
      for (j = 0; j < onig_graph_out_edge_count(g, i); j++) {
        const OnigGraphEdge* e = onig_graph_out_edge(g, i, j);
        CHECK_EQ_INT(e->from, i, "out edge source");
        CHECK(onig_graph_edge(g, k) == e, "global/out slice agreement");
        k++;
      }
    }
    CHECK_EQ_INT(k, onig_graph_edge_count(g), "slice total");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Out-of-range accessors are safe and return no bogus node. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    if (! graph_for("ab[cd].", &g, &re)) return 0;
    CHECK(onig_graph_node(g, -1) == 0, "negative node");
    CHECK(onig_graph_node(g, onig_graph_node_count(g)) == 0, "past-end node");
    CHECK(onig_graph_edge(g, onig_graph_edge_count(g)) == 0, "past-end edge");
    CHECK(onig_graph_out_edge(g, 0, 99) == 0, "past-end out edge");
    CHECK(onig_graph_call(g, 0) == 0, "no calls -> null");
    CHECK(onig_graph_repeat(g, 0) == 0, "no repeats -> null");
    CHECK(onig_graph_entry(g, 1) == -1, "past-end entry");
    CHECK(onig_graph_scc_for_entry(g, 999) == 0, "unknown entry scc");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Failure path: an opcode outside the supported subset yields NULL
     plus a structured error, and no partial graph.  Backreferences and
     callouts are explicitly rejected. */
  {
    OnigRegex re = 0;
    OnigGraphError err;
    OnigGraph* g;
    const char* unsupported[] = {
      "(a)\\1",          /* BACKREF1 */
      "(?<n>a)\\k<n>",   /* named backreference */
      "(*FAIL)",         /* callout-like control verb */
      "a(*sr:c)b"        /* builtin callout */
    };
    int i, tried = 0;
    for (i = 0; i < (int)(sizeof(unsupported)/sizeof(unsupported[0])); i++) {
      int rc = compile_pattern(unsupported[i], &re);
      if (rc != ONIG_NORMAL) continue;
      tried++;
      memset(&err, 0, sizeof(err));
      g = onig_graph_new(re, &err);
      CHECK(g == 0, "expected rejection for %s", unsupported[i]);
      CHECK(err.reason != ONIG_GRAPH_OK, "error reason set for %s",
            unsupported[i]);
      CHECK(err.message[0] != '\0', "error message set");
      CHECK(err.op_index >= 0, "error carries an instruction index");
      onig_free(re);
    }
    CHECK(tried >= 1, "at least one unsupported pattern compiled");
  }

  /* Reachability is structural: a node behind an unconditional jump is
     marked unreachable while still present. */
  {
    OnigGraph* g = 0;
    OnigRegex re = 0;
    if (! graph_for("(?<piece>ab){0}\\g<piece>", &g, &re)) return 0;
    CHECK(onig_graph_node_reachable(g, 0), "entry reachable");
    CHECK(onig_graph_node_reachable(g, 3), "call target reachable");
    CHECK(onig_graph_node_reachable(g, 8), "second call reachable");
    CHECK(! onig_graph_node_reachable(g, 1),
          "skipped call body is structurally unreachable");
    CHECK(! onig_graph_node_reachable(g, 2), "skipped jump unreachable");
    CHECK((onig_graph_node(g, 1)->flags & ONIG_GRAPH_NODE_UNREACHABLE) != 0,
          "unreachable flag set");
    onig_graph_free(g);
    onig_free(re);
  }

  /* Empty / NULL arguments are rejected, not crashed on. */
  {
    OnigGraphError err;
    OnigGraph* g = onig_graph_new(0, &err);
    CHECK(g == 0, "NULL regex rejected");
    CHECK_EQ_INT(err.reason, ONIG_GRAPH_ERR_INVALID_ARGUMENT, "null reason");
    CHECK_EQ_INT((int)onig_graph_node_count(0), 0, "null graph node count");
    onig_graph_free(0);
  }

  return g_checks_this_group >= MIN_GROUP_CHECKS;
}

/* ------------------------------------------------------------------ */

int
main(int argc, char** argv)
{
  int rc;
  int total = 0;
  int q;

  (void)argc; (void)argv;

  kEnc = ONIG_ENCODING_ASCII;
  rc = onig_initialize(&kEnc, 1);
  if (rc != ONIG_NORMAL) {
    fprintf(stderr, "onig_initialize failed: %d\n", rc);
    return 2;
  }

  for (q = 0; q < 1; q++) {
    if (! group_linear_and_branch()) {
      fprintf(stderr, "group linear-branch FAILED after %d checks\n",
              g_checks_this_group);
      g_failures++;
    }
    else if (g_checks_this_group < MIN_GROUP_CHECKS) {
      fprintf(stderr, "group linear-branch ran only %d checks\n",
              g_checks_this_group);
      g_failures++;
    }
    else {
      printf("GROUP linear-branch checks=%d ok\n", g_checks_this_group);
      total += g_checks_this_group;
    }

    if (! group_repeat()) {
      fprintf(stderr, "group repeat FAILED after %d checks\n",
              g_checks_this_group);
      g_failures++;
    }
    else if (g_checks_this_group < MIN_GROUP_CHECKS) {
      fprintf(stderr, "group repeat ran only %d checks\n", g_checks_this_group);
      g_failures++;
    }
    else {
      printf("GROUP repeat checks=%d ok\n", g_checks_this_group);
      total += g_checks_this_group;
    }

    if (! group_calls_and_recursion()) {
      fprintf(stderr, "group calls-recursion FAILED after %d checks\n",
              g_checks_this_group);
      g_failures++;
    }
    else if (g_checks_this_group < MIN_GROUP_CHECKS) {
      fprintf(stderr, "group calls-recursion ran only %d checks\n",
              g_checks_this_group);
      g_failures++;
    }
    else {
      printf("GROUP calls-recursion checks=%d ok\n", g_checks_this_group);
      total += g_checks_this_group;
    }

    if (! group_identity_lifetime()) {
      fprintf(stderr, "group identity-lifetime FAILED after %d checks\n",
              g_checks_this_group);
      g_failures++;
    }
    else if (g_checks_this_group < MIN_GROUP_CHECKS) {
      fprintf(stderr, "group identity-lifetime ran only %d checks\n",
              g_checks_this_group);
      g_failures++;
    }
    else {
      printf("GROUP identity-lifetime checks=%d ok\n", g_checks_this_group);
      total += g_checks_this_group;
    }
  }

  onig_end();

  if (g_failures != 0) {
    fprintf(stderr, "RESULT FAIL groups=4 total_checks=%d\n", total);
    return 1;
  }
  printf("RESULT PASS groups=4 total_checks=%d\n", total);
  return 0;
}
