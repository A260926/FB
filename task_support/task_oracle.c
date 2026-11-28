#include "task_ast.h"

#include <stdint.h>
#include <string.h>

/*
 * Independent reference interpreter.
 *
 * A small recursive backtracking matcher over Unicode scalar positions,
 * using explicit continuation frames (the rest of the pattern after the
 * current node) allocated on the C stack. It knows nothing about PCRE2 or
 * any UTF encoding.
 *
 * Modelled semantics:
 *   - leftmost overall match (start positions tried 0..tlen)
 *   - leftmost-first alternation (later branches only after full failure,
 *     with captures restored to the branch-entry snapshot)
 *   - greedy bounded repetition: counts tried max..min, each count re-run
 *     from the entry snapshot so capture state stays consistent
 *   - PCRE2 capture rules: a repeated group keeps its last iteration's
 *     span; a group that never participates stays UNSET, which is distinct
 *     from a group that participates and captures the empty string.
 *
 * The grammar bounds (depth <= 5, repetition <= 4, text <= 128 scalars)
 * keep both recursion depth and retry work trivially small.
 */

#define ORACLE_UNSET ((size_t)-1)
#define ORACLE_OVPAIRS (TASK_MAX_GROUPS + 1)

typedef struct Cont Cont;
struct Cont {
  int kind; /* 0 identity, 1 next concat child, 2 close group, 3 rep iter */
  const Node *node;
  int idx;             /* kind 1: next child; kind 3: iterations left */
  int group;           /* kind 2: capture number */
  size_t save_lo, save_hi;
  const Cont *next;
};

typedef struct {
  const uint32_t *text;
  int tlen;
  size_t ovec[ORACLE_OVPAIRS * 2];
} Machine;

static int match_node(Machine *m, const Node *n, int pos, const Cont *k);

/* Apply a continuation: run whatever comes after the current node. */
static int apply_cont(Machine *m, const Cont *k, int pos) {
  if (k->kind == 0) return pos;
  if (k->kind == 2) {
    m->ovec[2 * k->group + 1] = (size_t)pos; /* may equal lo: empty capture */
    return apply_cont(m, k->next, pos);
  }
  if (k->kind == 3) {
    /* One repetition iteration completed; run the iterations still due,
     * then the enclosing continuation. Failing here lets the child's own
     * quantifier backtrack to a smaller count, which is what makes nested
     * repetitions behave like a real backtracking engine. */
    if (k->idx == 0) return apply_cont(m, k->next, pos);
    Cont again;
    again.kind = 3;
    again.node = k->node;
    again.idx = k->idx - 1;
    again.group = 0;
    again.save_lo = again.save_hi = 0;
    again.next = k->next;
    return match_node(m, k->node->a, pos, &again);
  }
  if (k->idx >= k->node->nkids) return apply_cont(m, k->next, pos);
  Cont rest;
  rest.kind = 1;
  rest.node = k->node;
  rest.idx = k->idx + 1;
  rest.group = 0;
  rest.save_lo = rest.save_hi = 0;
  rest.next = k->next;
  return match_node(m, k->node->kids[k->idx], pos, &rest);
}

static int match_node(Machine *m, const Node *n, int pos, const Cont *k) {
  switch (n->kind) {
    case N_LIT:
      if (pos >= m->tlen || m->text[pos] != n->cp) return -1;
      return apply_cont(m, k, pos + 1);

    case N_CLS: {
      if (pos >= m->tlen) return -1;
      uint32_t c = m->text[pos];
      for (int i = 0; i < n->ncls; i++)
        if (n->cps[i] == c) return apply_cont(m, k, pos + 1);
      return -1;
    }

    case N_CAT: {
      Cont first;
      first.kind = 1;
      first.node = n;
      first.idx = 1;
      first.next = k;
      return match_node(m, n->kids[0], pos, &first);
    }

    case N_ALT: {
      size_t snap[ORACLE_OVPAIRS * 2];
      memcpy(snap, m->ovec, sizeof(snap));
      for (int i = 0; i < n->nkids; i++) {
        int r = match_node(m, n->kids[i], pos, k);
        if (r >= 0) return r;
        memcpy(m->ovec, snap, sizeof(snap));
      }
      return -1;
    }

    case N_GRP: {
      int g = n->group;
      size_t save_lo = 0, save_hi = 0;
      if (g) {
        save_lo = m->ovec[2 * g];
        save_hi = m->ovec[2 * g + 1];
        m->ovec[2 * g] = (size_t)pos;
        m->ovec[2 * g + 1] = ORACLE_UNSET;
      }
      Cont close_c;
      close_c.kind = 2;
      close_c.group = g;
      close_c.save_lo = save_lo;
      close_c.save_hi = save_hi;
      close_c.next = k;
      int r = match_node(m, n->a, pos, &close_c);
      if (r < 0 && g) {
        m->ovec[2 * g] = save_lo;
        m->ovec[2 * g + 1] = save_hi;
      }
      return r;
    }

    case N_REP: {
      size_t entry[ORACLE_OVPAIRS * 2];
      memcpy(entry, m->ovec, sizeof(entry));
      /* Greedy: the largest legal count is attempted first. Every attempt
       * starts from the entry snapshot; the per-iteration continuation
       * (kind 3) carries the iterations still due so nested quantifiers
       * backtrack through the enclosing continuation. A count of zero
       * falls straight through to the next continuation. */
      for (int count = n->rep_max; count >= n->rep_min; count--) {
        memcpy(m->ovec, entry, sizeof(entry));
        Cont iter;
        iter.kind = 3;
        iter.node = n;
        iter.idx = count;
        iter.group = 0;
        iter.save_lo = iter.save_hi = 0;
        iter.next = k;
        int r = apply_cont(m, &iter, pos);
        if (r >= 0) return r;
      }
      memcpy(m->ovec, entry, sizeof(entry));
      return -1;
    }
  }
  return -1;
}

static int max_group(const Node *root) { return ast_group_count(root); }

int oracle_match(const Node *root, const uint32_t *text, int tlen,
                 size_t *ovec, int ovecap) {
  int ngroups = max_group(root);
  if (2 * (ngroups + 1) > ovecap) return -1;

  Machine m;
  m.text = text;
  m.tlen = tlen;
  static const Cont identity = {0, NULL, 0, 0, 0, 0, NULL};

  for (int start = 0; start <= tlen; start++) {
    for (int i = 0; i < ORACLE_OVPAIRS * 2; i++) m.ovec[i] = ORACLE_UNSET;
    int end = match_node(&m, root, start, &identity);
    if (end >= 0) {
      m.ovec[0] = (size_t)start;
      m.ovec[1] = (size_t)end;
      memcpy(ovec, m.ovec, (size_t)(2 * (ngroups + 1)) * sizeof(size_t));
      return 1;
    }
  }
  for (int i = 0; i <= ngroups; i++) {
    ovec[2 * i] = ORACLE_UNSET;
    ovec[2 * i + 1] = ORACLE_UNSET;
  }
  return 0;
}
