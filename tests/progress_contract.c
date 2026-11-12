/*
** Progress analysis contract tests.
**
** Four entry points, each returning the number of positive assertions
** completed.  A failed assertion aborts the process; nothing is skipped.
** The file has no main(): baseline/target-main.c supplies the runner.
*/

#include "../mpc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EFF_ALL (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_FAIL_CONSUMED | \
                 MPC_PROGRESS_SUCC_EMPTY | MPC_PROGRESS_SUCC_CONSUMED)

#define REQUIRE(cond)                                                      \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "progress contract failed: %s (%s:%d)\n",           \
              #cond, __FILE__, __LINE__);                                  \
      abort();                                                             \
    }                                                                      \
    asserts++;                                                             \
  } while (0)

static int node_index(const mpc_progress_report_t *r, mpc_parser_t *p) {
  int j;
  for (j = 0; j < r->node_num; j++) {
    if (r->nodes[j].parser == p) { return j; }
  }
  return -1;
}

static int effects_of(const mpc_progress_report_t *r, mpc_parser_t *p) {
  int j = node_index(r, p);
  assert(j >= 0);
  return r->nodes[j].effects;
}

static int risk_present(const mpc_progress_report_t *r, int risk,
                        int node_index_value) {
  int j;
  for (j = 0; j < r->risk_num; j++) {
    if (r->risks[j].risk == risk
    && r->risks[j].node_index == node_index_value) { return 1; }
  }
  return 0;
}

static int risk_count(const mpc_progress_report_t *r, int risk) {
  int j, n = 0;
  for (j = 0; j < r->risk_num; j++) {
    if (r->risks[j].risk == risk) { n++; }
  }
  return n;
}

static int reject_always(mpc_val_t **x) { (void)x; return 0; }
static void dtor_free(mpc_val_t *x) { free(x); }
static mpc_val_t *lift_null(void) { return NULL; }

static int ctor_calls;
static int check_calls;
static int apply_calls;
static mpc_val_t *counting_lift(void) { ctor_calls++; return NULL; }
static int counting_check(mpc_val_t **x) { (void)x; check_calls++; return 1; }
static mpc_val_t *counting_apply(mpc_val_t *x) { apply_calls++; return x; }

/* ------------------------------------------------------------------ */
/* Independent iterative reference for one small recursive graph.     */
/* It re-derives effects and zero-edge cycles without calling the     */
/* production analyzer and without parsing any input.                 */
/* ------------------------------------------------------------------ */

enum { REF_CHAR = 0, REF_OR = 1, REF_MAYBE = 2 };

typedef struct {
  int kind;
  int child[2];
  int effects;
} ref_node_t;

#define RFE MPC_PROGRESS_FAIL_EMPTY
#define RFC MPC_PROGRESS_FAIL_CONSUMED
#define RSE MPC_PROGRESS_SUCC_EMPTY
#define RSC MPC_PROGRESS_SUCC_CONSUMED

static int ref_transfer(const ref_node_t *g, int idx) {
  const ref_node_t *n = &g[idx];
  int x, z, co, out;

  switch (n->kind) {
    case REF_CHAR: return RFE | RSC;
    case REF_MAYBE:
      x = g[n->child[0]].effects;
      return (x & (RSE | RSC))
           | ((x & RFE) ? RSE : 0)
           | ((x & RFC) ? RSC : 0);
    case REF_OR: {
      /* item 0 plain, item 1 behind a failing prefix */
      int f0 = g[n->child[0]].effects;
      int f1 = g[n->child[1]].effects;
      out = f0;
      z = (f0 & RFE) != 0;
      co = (f0 & RFC) != 0;
      if (z) { out |= f1; }
      if (co) {
        if (f1 & (RFE | RFC)) { out |= RFC; }
        if (f1 & (RSE | RSC)) { out |= RSC; }
      }
      return out;
    }
    default:
      return 0;
  }
  return 0;
}

static void ref_solve(ref_node_t *g, int n) {
  int changed = 1, j;
  while (changed) {
    changed = 0;
    for (j = 0; j < n; j++) {
      int fx = ref_transfer(g, j);
      if ((fx & ~g[j].effects) != 0) {
        g[j].effects |= fx;
        changed = 1;
      }
    }
  }
}

/* Reference zero edges for the graph shape or(0)=char,maybe(A):
   the call position depends only on the prefix control flow. */
static int ref_zero_edge(const ref_node_t *g, int from, int to) {
  if (from == 0 && to == 1) { return 1; }              /* plain edge  */
  if (from == 0 && to == 2) {                          /* fail prefix */
    return (g[1].effects & RFE) != 0;
  }
  if (from == 2 && to == 0) { return 1; }              /* plain edge  */
  return 0;
}

int contract_effects(void) {
  int asserts = 0;
  mpc_progress_report_t *r;
  mpc_parser_t *p, *child, *a, *b;
  mpc_result_t pr;

  /* leaf abstractions */
  p = mpc_char('a');
  r = mpc_analyze_progress(p);
  REQUIRE(r != NULL && r->complete);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_CONSUMED));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_string("");
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) == MPC_PROGRESS_SUCC_EMPTY);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_string("abc");
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_CONSUMED));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_soi();
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_EMPTY));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_eoi();
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_EMPTY));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_anchor(NULL);
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_EMPTY));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_pass();
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) == MPC_PROGRESS_SUCC_EMPTY);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_state();
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) == MPC_PROGRESS_SUCC_EMPTY);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_lift(lift_null);
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) == MPC_PROGRESS_SUCC_EMPTY);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_fail("boom");
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) == MPC_PROGRESS_FAIL_EMPTY);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  a = mpc_new("undef-rule");
  r = mpc_analyze_progress(a);
  REQUIRE(effects_of(r, a) == MPC_PROGRESS_FAIL_EMPTY);
  mpc_progress_report_delete(r);

  /* transparent wrappers */
  child = mpc_char('a');
  p = mpc_apply(child, counting_apply);
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) == effects_of(r, child));
  REQUIRE(apply_calls == 0);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  child = mpc_char('a');
  p = mpc_expect(child, "an a");
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) == effects_of(r, child));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* check: verdict treated as both possible; rejected consumption kept */
  child = mpc_char('a');
  p = mpc_check(child, dtor_free, reject_always, "reject");
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_FAIL_CONSUMED |
           MPC_PROGRESS_SUCC_CONSUMED));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_check(mpc_pass(), dtor_free, reject_always, "reject");
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_EMPTY));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* not: child success rewinds and fails empty; child failure keeps pos */
  p = mpc_not(mpc_char('a'), dtor_free);
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_EMPTY));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* check rejection retains consumption, not/maybe then succeed consumed */
  p = mpc_not(mpc_check(mpc_char('a'), dtor_free,
                        reject_always, "reject"), dtor_free);
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_EMPTY |
           MPC_PROGRESS_SUCC_CONSUMED));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_maybe(mpc_check(mpc_char('a'), dtor_free,
                          reject_always, "reject"));
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_SUCC_EMPTY | MPC_PROGRESS_SUCC_CONSUMED));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* AND failure rewinds to the entry: maybe only ever succeeds empty */
  p = mpc_maybe(mpc_and(2, mpcf_null,
        mpc_char('a'), mpc_fail("stop"), dtor_free));
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) == MPC_PROGRESS_SUCC_EMPTY);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* OR failed alternatives do not rewind: consumed failure yields FC,
     and the second alternative entered after a consumed prefix too */
  p = mpc_or(2,
        mpc_check(mpc_char('a'), dtor_free, reject_always, "reject"),
        mpc_char('b'));
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_FAIL_CONSUMED |
           MPC_PROGRESS_SUCC_CONSUMED));
  REQUIRE((effects_of(r, p) & MPC_PROGRESS_SUCC_EMPTY) == 0);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* repetition effects, including the final failed attempt.  A body that
     cannot fail has no finite return: its effect set stays empty and the
     repetition stall is reported separately as a risk. */
  p = mpc_many(mpcf_null, mpc_pass());
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) == 0);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_many1(mpcf_null, mpc_pass());
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) == 0);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_many(mpcf_null, mpc_char('a'));
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_SUCC_EMPTY | MPC_PROGRESS_SUCC_CONSUMED));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_many1(mpcf_null, mpc_char('a'));
  r = mpc_analyze_progress(p);
  /* the first success always consumes and the body can only fail empty */
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_CONSUMED));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* positive count: exact finite composition */
  p = mpc_count(3, mpcf_null, mpc_pass(), dtor_free);
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) == MPC_PROGRESS_SUCC_EMPTY);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_count(2, mpcf_null, mpc_char('a'), dtor_free);
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_FAIL_CONSUMED |
           MPC_PROGRESS_SUCC_CONSUMED));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* sepby1: empty separator with consuming element.  Overall failure only
     comes from the first element, whose failure is empty; later element
     failures end the repetition successfully while retaining position. */
  p = mpc_sepby1(mpcf_null, mpc_string(""), mpc_char('a'));
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) ==
          (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_CONSUMED));
  REQUIRE(risk_count(r, MPC_PROGRESS_RISK_REPETITION) == 0);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* sepby1: a full empty separator+element unit never returns finitely;
     effects stay empty and the repetition risk carries the diagnosis. */
  p = mpc_sepby1(mpcf_null, mpc_pass(), mpc_pass());
  r = mpc_analyze_progress(p);
  REQUIRE(effects_of(r, p) == 0);
  REQUIRE(risk_count(r, MPC_PROGRESS_RISK_REPETITION) == 1);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* fixpoint must not fill every node with all four effects */
  a = mpc_new("a");
  b = mpc_new("b");
  mpc_define(a, mpc_or(2, mpc_char('x'), b));
  mpc_define(b, mpc_maybe(a));
  r = mpc_analyze_progress(a);
  REQUIRE(r->complete);
  {
    int j;
    for (j = 0; j < r->node_num; j++) {
      REQUIRE(r->nodes[j].effects != 0);
      REQUIRE(r->nodes[j].effects != EFF_ALL);
    }
  }

  /* independent iterative reference on the small recursive graph */
  {
    /* production graph order: a(or) index 0, char index 1, b index 2 */
    ref_node_t ref[3];
    int j;
    memset(ref, 0, sizeof(ref));
    ref[0].kind = REF_OR;    ref[0].child[0] = 1; ref[0].child[1] = 2;
    ref[1].kind = REF_CHAR;
    ref[2].kind = REF_MAYBE; ref[2].child[0] = 0;
    ref_solve(ref, 3);

    REQUIRE(ref[1].effects == (RFE | RSC));
    REQUIRE((ref[2].effects & (RSE | RSC)) == (RSE | RSC));
    REQUIRE((ref[0].effects & RFE) != 0);
    REQUIRE((ref[0].effects & RSC) != 0);
    REQUIRE((ref[0].effects & RSE) != 0);
    REQUIRE((ref[0].effects & RFC) == 0);

    for (j = 0; j < r->node_num; j++) {
      mpc_parser_t *target = r->nodes[j].parser;
      int expected;
      if (target == a) { expected = ref[0].effects; }
      else if (target == b) { expected = ref[2].effects; }
      else { expected = ref[1].effects; }
      REQUIRE(r->nodes[j].effects == expected);
    }

    /* reference zero-edge cycle: 0 -> 2 -> 0 */
    REQUIRE(ref_zero_edge(ref, 0, 2));
    REQUIRE(ref_zero_edge(ref, 2, 0));
    REQUIRE(risk_count(r, MPC_PROGRESS_RISK_CYCLE) == 1);
    {
      const mpc_progress_risk_t *rk = NULL;
      for (j = 0; j < r->risk_num; j++) {
        if (r->risks[j].risk == MPC_PROGRESS_RISK_CYCLE) {
          rk = &r->risks[j];
        }
      }
      REQUIRE(rk != NULL);
      REQUIRE(rk->path_len == 3);
      REQUIRE(rk->node_path[0] == rk->node_path[2]);
      REQUIRE(rk->node_path[0] == node_index(r, a));
      REQUIRE(rk->node_path[1] == node_index(r, b));
    }
  }
  mpc_progress_report_delete(r);
  mpc_delete(a);
  mpc_delete(b);

  /* callbacks stay untouched and normal parsing still works afterwards */
  REQUIRE(ctor_calls == 0 && check_calls == 0 && apply_calls == 0);
  p = mpc_check(mpc_apply(mpc_lift(counting_lift), counting_apply),
                dtor_free, counting_check, "ok");
  REQUIRE(mpc_parse("contract", "", p, &pr));
  REQUIRE(ctor_calls == 1 && check_calls == 1 && apply_calls == 1);
  mpc_delete(p);

  return asserts;
}

int contract_cycles(void) {
  int asserts = 0;
  mpc_progress_report_t *r, *r2;
  mpc_parser_t *a, *b, *c, *root, *tail, *hidden;
  int j;

  /* mutual recursion through a zero-consumption edge */
  a = mpc_new("mut-a");
  b = mpc_new("mut-b");
  mpc_define(a, mpc_or(2, mpc_char('x'), b));
  mpc_define(b, mpc_maybe(a));
  r = mpc_analyze_progress(a);
  REQUIRE(r->complete);
  REQUIRE(risk_count(r, MPC_PROGRESS_RISK_CYCLE) == 1);
  REQUIRE(risk_present(r, MPC_PROGRESS_RISK_CYCLE, node_index(r, a)));
  {
    const mpc_progress_risk_t *rk = &r->risks[0];
    REQUIRE(rk->node_path[0] == rk->node_path[rk->path_len - 1]);
    REQUIRE(rk->path_len >= 3);
    for (j = 0; j < rk->path_len; j++) {
      REQUIRE(rk->node_path[j] >= 0);
      REQUIRE(rk->node_path[j] < r->node_num);
      REQUIRE(r->nodes[rk->node_path[j]].reachable);
    }
  }

  /* every reported cycle path is properly closed */
  for (j = 0; j < r->risk_num; j++) {
    const mpc_progress_risk_t *rk = &r->risks[j];
    REQUIRE(rk->node_path[0] == rk->node_path[rk->path_len - 1]);
  }

  /* direct self loop: one cyclic SCC, closed path of length two */
  {
    mpc_parser_t *s = mpc_new("self-loop");
    mpc_progress_report_t *sr;
    mpc_define(s, mpc_maybe(s));
    sr = mpc_analyze_progress(s);
    REQUIRE(risk_count(sr, MPC_PROGRESS_RISK_CYCLE) == 1);
    REQUIRE(sr->risks[0].path_len == 2);
    REQUIRE(sr->risks[0].node_path[0] == node_index(sr, s));
    REQUIRE(sr->risks[0].node_path[1] == node_index(sr, s));
    mpc_progress_report_delete(sr);
    mpc_delete(s);
  }

  /* identity: distinct object pointers, no duplicated node identity */
  for (j = 0; j < r->node_num; j++) {
    int k;
    REQUIRE(r->nodes[j].index == j);
    for (k = j + 1; k < r->node_num; k++) {
      REQUIRE(r->nodes[j].parser != r->nodes[k].parser);
    }
  }

  /* deterministic ordering and paths across repeated analyses */
  r2 = mpc_analyze_progress(a);
  REQUIRE(r2->node_num == r->node_num);
  REQUIRE(r2->risk_num == r->risk_num);
  for (j = 0; j < r2->risk_num; j++) {
    int k;
    REQUIRE(r2->risks[j].risk == r->risks[j].risk);
    REQUIRE(r2->risks[j].node_index == r->risks[j].node_index);
    REQUIRE(r2->risks[j].path_len == r->risks[j].path_len);
    for (k = 0; k < r2->risks[j].path_len; k++) {
      REQUIRE(r2->risks[j].node_path[k] == r->risks[j].node_path[k]);
    }
  }
  mpc_progress_report_delete(r2);
  mpc_progress_report_delete(r);
  mpc_delete(a);
  mpc_delete(b);

  /* right recursion after consumed input is not a zero-consumption cycle */
  a = mpc_new("right-a");
  mpc_define(a, mpc_or(2, mpc_char('x'),
    mpc_and(2, mpcf_null, mpc_char('y'), a, dtor_free)));
  r = mpc_analyze_progress(a);
  REQUIRE(risk_count(r, MPC_PROGRESS_RISK_CYCLE) == 0);
  REQUIRE(node_index(r, a) == 0);
  REQUIRE(r->nodes[node_index(r, a)].reachable);
  mpc_progress_report_delete(r);
  mpc_delete(a);

  /* OR branch behind a parser that cannot fail is never reached */
  c = mpc_new("self-c");
  mpc_define(c, mpc_maybe(c));
  hidden = mpc_many(mpcf_null, mpc_pass());
  root = mpc_or(2, mpc_pass(), hidden);
  r = mpc_analyze_progress(root);
  REQUIRE(r->nodes[node_index(r, hidden)].reachable == 0);
  REQUIRE(risk_present(r, MPC_PROGRESS_RISK_REPETITION,
                       node_index(r, hidden)) == 0);
  /* c is not even part of this graph */
  REQUIRE(node_index(r, c) < 0);
  mpc_progress_report_delete(r);
  mpc_delete(root);

  /* OR entry to the cyclic alternative needs a failing prefix;
     mpc_many never fails, so the cycle is structurally unreachable */
  root = mpc_or(2, mpc_many(mpcf_null, mpc_char('a')), c);
  r = mpc_analyze_progress(root);
  REQUIRE(r->nodes[node_index(r, c)].reachable == 0);
  REQUIRE(risk_count(r, MPC_PROGRESS_RISK_CYCLE) == 0);
  mpc_progress_report_delete(r);
  mpc_delete(root);
  mpc_delete(c);

  /* AND item behind a prefix that cannot succeed is unreachable */
  c = mpc_new("behind-c");
  mpc_define(c, mpc_maybe(c));
  root = mpc_and(2, mpcf_null, mpc_fail("stop"), c, dtor_free);
  r = mpc_analyze_progress(root);
  REQUIRE(r->nodes[node_index(r, c)].reachable == 0);
  REQUIRE(risk_count(r, MPC_PROGRESS_RISK_CYCLE) == 0);
  mpc_progress_report_delete(r);
  mpc_delete(root);
  mpc_delete(c);

  /* local stagnation inside a subgraph only reached after consumption
     must still be examined */
  a = mpc_new("local-a");
  mpc_define(a, mpc_maybe(a));
  root = mpc_and(2, mpcf_null, mpc_char('q'), a, dtor_free);
  r = mpc_analyze_progress(root);
  REQUIRE(r->nodes[node_index(r, a)].reachable);
  REQUIRE(risk_present(r, MPC_PROGRESS_RISK_CYCLE, node_index(r, a)));
  mpc_progress_report_delete(r);
  mpc_delete(root);
  mpc_delete(a);

  /* shared node: one identity, stable DFS numbering left-to-right */
  a = mpc_new("shared-a");
  tail = mpc_apply(a, counting_apply);
  root = mpc_and(2, mpcf_null, tail,
                 mpc_apply(a, counting_apply), dtor_free);
  r = mpc_analyze_progress(root);
  REQUIRE(node_index(r, a) >= 0);
  REQUIRE(r->node_num == 4); /* root and, tail apply, a, second apply */
  mpc_progress_report_delete(r);
  mpc_delete(root);
  mpc_delete(a);

  return asserts;
}

int contract_repetition(void) {
  int asserts = 0;
  mpc_progress_report_t *r, *r2;
  mpc_parser_t *p, *sep, *elem;
  int j;

  p = mpc_many(mpcf_null, mpc_pass());
  r = mpc_analyze_progress(p);
  REQUIRE(risk_count(r, MPC_PROGRESS_RISK_REPETITION) == 1);
  REQUIRE(risk_present(r, MPC_PROGRESS_RISK_REPETITION,
                       node_index(r, p)));
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_many1(mpcf_null, mpc_pass());
  r = mpc_analyze_progress(p);
  REQUIRE(risk_count(r, MPC_PROGRESS_RISK_REPETITION) == 1);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_many(mpcf_null, mpc_char('a'));
  r = mpc_analyze_progress(p);
  REQUIRE(risk_count(r, MPC_PROGRESS_RISK_REPETITION) == 0);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* empty separator + consuming element: no repetition stall */
  p = mpc_sepby1(mpcf_null, mpc_string(""), mpc_char('a'));
  r = mpc_analyze_progress(p);
  REQUIRE(risk_count(r, MPC_PROGRESS_RISK_REPETITION) == 0);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* consuming separator: no empty repeat unit */
  p = mpc_sepby1(mpcf_null, mpc_char(','), mpc_pass());
  r = mpc_analyze_progress(p);
  REQUIRE(risk_count(r, MPC_PROGRESS_RISK_REPETITION) == 0);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* element that can empty-succeed plus empty separator stalls */
  elem = mpc_or(2, mpc_char('a'), mpc_pass());
  sep = mpc_pass();
  p = mpc_sepby1(mpcf_null, sep, elem);
  r = mpc_analyze_progress(p);
  REQUIRE(risk_count(r, MPC_PROGRESS_RISK_REPETITION) == 1);
  {
    const mpc_progress_risk_t *rk = NULL;
    for (j = 0; j < r->risk_num; j++) {
      if (r->risks[j].risk == MPC_PROGRESS_RISK_REPETITION) {
        rk = &r->risks[j];
      }
    }
    REQUIRE(rk != NULL);
    /* closed path root -> separator -> element -> root, structural
       child order of sepby1 being element then separator */
    REQUIRE(rk->path_len == 4);
    REQUIRE(rk->node_path[0] == rk->node_path[3]);
    REQUIRE(rk->node_path[1] == node_index(r, sep));
    REQUIRE(rk->node_path[2] == node_index(r, elem));
  }
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* cycle and repetition are distinct risk kinds in one graph */
  {
    mpc_parser_t *a = mpc_new("rep-a");
    mpc_parser_t *many;
    /* char can fail empty, so the second OR item is reachable; the
       zero-edge chain a -> many -> maybe -> a closes a cycle and the
       many body can empty-succeed, giving a repetition finding too */
    many = mpc_many(mpcf_null, mpc_maybe(a));
    mpc_define(a, mpc_or(2, mpc_char('x'), many));
    r = mpc_analyze_progress(a);
    REQUIRE(node_index(r, many) >= 0);
    REQUIRE(risk_present(r, MPC_PROGRESS_RISK_CYCLE, node_index(r, a)));
    REQUIRE(risk_present(r, MPC_PROGRESS_RISK_REPETITION,
                         node_index(r, many)));
    mpc_progress_report_delete(r);
    mpc_delete(a);
  }

  /* stable ordering on repeat analysis */
  p = mpc_many(mpcf_null, mpc_pass());
  r = mpc_analyze_progress(p);
  r2 = mpc_analyze_progress(p);
  REQUIRE(r2->risk_num == r->risk_num);
  for (j = 0; j < r->risk_num; j++) {
    REQUIRE(r2->risks[j].risk == r->risks[j].risk);
    REQUIRE(r2->risks[j].node_index == r->risks[j].node_index);
  }
  mpc_progress_report_delete(r);
  mpc_progress_report_delete(r2);
  mpc_delete(p);

  return asserts;
}

int contract_ownership(void) {
  int asserts = 0;
  mpc_progress_report_t *r;
  mpc_parser_t *a, *p;
  mpc_result_t pr;
  int j;
  int saved_effects;
  int saved_type;
  char saved_name[64];

  /* NULL release is a no-op */
  mpc_progress_report_delete(NULL);

  /* analysis is read-only towards retained/name/optimisation state */
  a = mpc_new("named-rule");
  mpc_define(a, mpc_or(2, mpc_pass(), mpc_char('x')));
  r = mpc_analyze_progress(a);
  REQUIRE(r != NULL && r->complete);
  saved_effects = r->nodes[node_index(r, a)].effects;
  saved_type = r->nodes[node_index(r, a)].type;
  REQUIRE(r->nodes[node_index(r, a)].name != NULL);
  strcpy(saved_name, r->nodes[node_index(r, a)].name);
  REQUIRE(strcmp(saved_name, "named-rule") == 0);

  /* redefining the shared named parser must not mutate the old report */
  mpc_define(a, mpc_char('z'));
  REQUIRE(r->nodes[node_index(r, a)].effects == saved_effects);
  REQUIRE(r->nodes[node_index(r, a)].type == saved_type);
  REQUIRE(strcmp(r->nodes[node_index(r, a)].name, saved_name) == 0);

  /* production parsing still behaves after redefinition */
  REQUIRE(mpc_parse("contract", "z", a, &pr));
  free(pr.output);
  REQUIRE(!mpc_parse("contract", "x", a, &pr));
  mpc_err_delete(pr.error);

  /* releasing the report does not touch the parsers */
  mpc_progress_report_delete(r);
  REQUIRE(mpc_parse("contract", "z", a, &pr));
  free(pr.output);
  mpc_delete(a);

  /* report names are independent copies */
  a = mpc_new("copy-name");
  mpc_define(a, mpc_pass());
  r = mpc_analyze_progress(a);
  REQUIRE(strcmp(r->nodes[node_index(r, a)].name, "copy-name") == 0);
  mpc_define(a, mpc_fail("rewritten"));
  REQUIRE(strcmp(r->nodes[node_index(r, a)].name, "copy-name") == 0);
  mpc_progress_report_delete(r);
  mpc_delete(a);

  /* user callbacks never run during analysis, but run when parsing */
  ctor_calls = check_calls = apply_calls = 0;
  p = mpc_apply(
        mpc_check(mpc_lift(counting_lift), dtor_free,
                  counting_check, "ok"),
        counting_apply);
  r = mpc_analyze_progress(p);
  REQUIRE(ctor_calls == 0 && check_calls == 0 && apply_calls == 0);
  mpc_progress_report_delete(r);
  REQUIRE(mpc_parse("contract", "", p, &pr));
  REQUIRE(ctor_calls == 1 && check_calls == 1 && apply_calls == 1);
  mpc_delete(p);

  /* non-positive count marks the report incomplete */
  p = mpc_count(0, mpcf_null, mpc_pass(), dtor_free);
  r = mpc_analyze_progress(p);
  REQUIRE(!r->complete);
  REQUIRE(r->unsupported_num >= 1);
  REQUIRE(r->unsupported[0].reason ==
          MPC_PROGRESS_UNSUPPORTED_NONPOSITIVE_COUNT);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  p = mpc_count(-3, mpcf_null, mpc_pass(), dtor_free);
  r = mpc_analyze_progress(p);
  REQUIRE(!r->complete);
  REQUIRE(r->unsupported_num >= 1);
  mpc_progress_report_delete(r);
  mpc_delete(p);

  /* predictive marks the report incomplete, even when unreachable */
  a = mpc_new("with-predict");
  mpc_define(a, mpc_or(2, mpc_pass(), mpc_predictive(mpc_char('a'))));
  r = mpc_analyze_progress(a);
  REQUIRE(!r->complete);
  {
    int found = 0, reachable_unsup = 0;
    for (j = 0; j < r->unsupported_num; j++) {
      int ni = r->unsupported[j].node_index;
      REQUIRE(r->unsupported[j].reason ==
              MPC_PROGRESS_UNSUPPORTED_PREDICTIVE);
      REQUIRE(r->unsupported[j].name != NULL);
      if (!r->nodes[ni].reachable) { reachable_unsup++; }
      found++;
    }
    REQUIRE(found >= 1);
    /* the predictive node sits behind pass(), which cannot fail */
    REQUIRE(reachable_unsup >= 1);
  }
  mpc_progress_report_delete(r);
  mpc_delete(a);

  /* unsupported node in a wholly unreachable detached branch is listed */
  {
    mpc_parser_t *bad = mpc_count(0, mpcf_null, mpc_pass(), dtor_free);
    mpc_parser_t *top = mpc_and(2, mpcf_null, mpc_pass(), bad, dtor_free);
    r = mpc_analyze_progress(top);
    REQUIRE(!r->complete);
    REQUIRE(node_index(r, bad) >= 0);
    REQUIRE(r->nodes[node_index(r, bad)].unsupported ==
            MPC_PROGRESS_UNSUPPORTED_NONPOSITIVE_COUNT);
    mpc_progress_report_delete(r);
    mpc_delete(top);
  }

  /* allocate/release cycles stay well behaved */
  for (j = 0; j < 50; j++) {
    mpc_parser_t *q = mpc_many(mpcf_null, mpc_pass());
    mpc_progress_report_t *qrep = mpc_analyze_progress(q);
    REQUIRE(qrep != NULL);
    REQUIRE(qrep->risk_num == 1);
    mpc_progress_report_delete(qrep);
    mpc_delete(q);
  }

  return asserts;
}
