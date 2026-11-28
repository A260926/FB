#include "task_ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- PRNG: splitmix64, fully deterministic ---------------- */

void rng_seed(TaskRng *r, uint64_t seed) { r->state = seed; }

uint64_t rng_next(TaskRng *r) {
  uint64_t z = (r->state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

uint32_t rng_below(TaskRng *r, uint32_t n) {
  return n ? (uint32_t)(rng_next(r) % n) : 0;
}

/* ---------------- Scalar pools (no surrogates, all valid Unicode) ------ */

static const uint32_t pool_ascii[] = {
    'a', 'b', 'c', 'd', 'e', '0', '1', '2', '%', '$'};
static const uint32_t pool_bmp[] = {
    0x00e9u, 0x00f1u, 0x05d0u, 0x03b1u, 0x042fu, 0x3042u, 0x4e2du, 0x2603u};
static const uint32_t pool_supp[] = {
    0x1f600u, 0x1f4a9u, 0x10348u, 0x1f0a1u, 0x2f800u, 0xe0001u};

#define N_ASCII (int)(sizeof(pool_ascii) / sizeof(pool_ascii[0]))
#define N_BMP (int)(sizeof(pool_bmp) / sizeof(pool_bmp[0]))
#define N_SUPP (int)(sizeof(pool_supp) / sizeof(pool_supp[0]))

uint32_t gen_scalar(TaskRng *r, int plane_bias) {
  if (plane_bias == 1) return pool_ascii[rng_below(r, N_ASCII)];
  if (plane_bias == 2) return pool_bmp[rng_below(r, N_BMP)];
  if (plane_bias == 3) return pool_supp[rng_below(r, N_SUPP)];
  uint32_t k = rng_below(r, (uint32_t)(N_ASCII + N_BMP + N_SUPP));
  if (k < N_ASCII) return pool_ascii[k];
  k -= N_ASCII;
  if (k < N_BMP) return pool_bmp[k];
  return pool_supp[k - N_BMP];
}

int gen_text(TaskRng *r, uint32_t *out, int cap) {
  int len = (int)rng_below(r, TASK_MAX_TEXT + 1);
  if (len > cap) len = cap;
  for (int i = 0; i < len; i++) out[i] = gen_scalar(r, 0);
  return len;
}

/* ---------------- Node constructors / destruction ---------------------- */

static int node_depth(const Node *n) {
  int d = 1;
  if (n->kind == N_GRP || n->kind == N_REP) {
    if (n->a && n->a->depth + 1 > d) d = n->a->depth + 1;
  } else if (n->kind == N_CAT || n->kind == N_ALT) {
    for (int i = 0; i < n->nkids; i++)
      if (n->kids[i]->depth + 1 > d) d = n->kids[i]->depth + 1;
  }
  return d;
}

Node *n_lit(uint32_t cp) {
  Node *n = calloc(1, sizeof(Node));
  n->kind = N_LIT;
  n->cp = cp;
  n->depth = 1;
  return n;
}

static int cmp_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

Node *n_cls(const uint32_t *cps, int n) {
  Node *nd = calloc(1, sizeof(Node));
  nd->kind = N_CLS;
  uint32_t *uniq = malloc((size_t)n * sizeof(uint32_t));
  int m = 0;
  for (int i = 0; i < n; i++) {
    int seen = 0;
    for (int j = 0; j < m; j++)
      if (uniq[j] == cps[i]) { seen = 1; break; }
    if (!seen) uniq[m++] = cps[i];
  }
  qsort(uniq, (size_t)m, sizeof(uint32_t), cmp_u32);
  nd->cps = uniq;
  nd->ncls = m;
  nd->depth = 1;
  return nd;
}

Node *n_cat(Node **kids, int n) {
  Node *nd = calloc(1, sizeof(Node));
  nd->kind = N_CAT;
  nd->kids = kids;
  nd->nkids = n;
  nd->depth = node_depth(nd);
  return nd;
}

Node *n_alt(Node **kids, int n) {
  Node *nd = calloc(1, sizeof(Node));
  nd->kind = N_ALT;
  nd->kids = kids;
  nd->nkids = n;
  nd->depth = node_depth(nd);
  return nd;
}

Node *n_grp(Node *child, int group) {
  Node *nd = calloc(1, sizeof(Node));
  nd->kind = N_GRP;
  nd->a = child;
  nd->group = group; /* 0 means non-capturing */
  nd->depth = node_depth(nd);
  return nd;
}

Node *n_rep(Node *child, int lo, int hi) {
  Node *nd = calloc(1, sizeof(Node));
  nd->kind = N_REP;
  nd->a = child;
  nd->rep_min = lo;
  nd->rep_max = hi;
  nd->depth = node_depth(nd);
  return nd;
}

void ast_free(Node *n) {
  if (!n) return;
  if (n->kind == N_CAT || n->kind == N_ALT) {
    for (int i = 0; i < n->nkids; i++) ast_free(n->kids[i]);
    free(n->kids);
  } else if (n->kind == N_GRP || n->kind == N_REP) {
    ast_free(n->a);
  } else if (n->kind == N_CLS) {
    free(n->cps);
  }
  free(n);
}

Node *ast_clone(const Node *n) {
  if (!n) return NULL;
  Node *c = calloc(1, sizeof(Node));
  c->kind = n->kind;
  c->cp = n->cp;
  c->rep_min = n->rep_min;
  c->rep_max = n->rep_max;
  c->group = n->group;
  c->named = n->named;
  c->depth = n->depth;
  if (n->kind == N_CLS) {
    c->ncls = n->ncls;
    c->cps = malloc((size_t)n->ncls * sizeof(uint32_t));
    memcpy(c->cps, n->cps, (size_t)n->ncls * sizeof(uint32_t));
  } else if (n->kind == N_GRP || n->kind == N_REP) {
    c->a = ast_clone(n->a);
  } else if (n->kind == N_CAT || n->kind == N_ALT) {
    c->nkids = n->nkids;
    c->kids = malloc((size_t)n->nkids * sizeof(Node *));
    for (int i = 0; i < n->nkids; i++) c->kids[i] = ast_clone(n->kids[i]);
  }
  return c;
}

/* ---------------- AST generation --------------------------------------- */

static Node *gen_node(TaskRng *r, int depth, int *groups, int capture_bias) {
  if (depth <= 1) {
    if (rng_below(r, 2)) return n_lit(gen_scalar(r, 0));
    int k = 1 + (int)rng_below(r, TASK_MAX_CLS);
    uint32_t cps[TASK_MAX_CLS];
    for (int i = 0; i < k; i++) cps[i] = gen_scalar(r, 0);
    return n_cls(cps, k);
  }
  /* Capture-heavy mode: every composite level opens a capturing group
   * while slots remain, so trees reach the upper capture count. */
  if (capture_bias && *groups < TASK_MAX_GROUPS && rng_below(r, 2)) {
    int g = ++(*groups);
    Node *c = gen_node(r, depth - 1, groups, capture_bias);
    return n_grp(c, g);
  }
  switch (rng_below(r, 12)) {
    case 0: case 1:
      return n_lit(gen_scalar(r, 0));
    case 2: case 3: {
      int k = 1 + (int)rng_below(r, TASK_MAX_CLS);
      uint32_t cps[TASK_MAX_CLS];
      for (int i = 0; i < k; i++) cps[i] = gen_scalar(r, 0);
      return n_cls(cps, k);
    }
    case 4: case 5: {
      int k = 2 + (int)rng_below(r, 2); /* 2 or 3 children */
      Node **kids = malloc((size_t)k * sizeof(Node *));
      for (int i = 0; i < k; i++) kids[i] = gen_node(r, depth - 1, groups, capture_bias);
      return n_cat(kids, k);
    }
    case 6: {
      int k = 2;
      Node **kids = malloc((size_t)k * sizeof(Node *));
      for (int i = 0; i < k; i++) kids[i] = gen_node(r, depth - 1, groups, capture_bias);
      return n_alt(kids, k);
    }
    case 7: case 8: case 9: {
      if (*groups >= TASK_MAX_GROUPS) {
        Node *c = gen_node(r, depth - 1, groups, capture_bias);
        int lo = (int)rng_below(r, TASK_REP_MAX + 1);
        int hi = lo + (int)rng_below(r, (uint32_t)(TASK_REP_MAX - lo + 1));
        return n_rep(c, lo, hi);
      }
      int g = ++(*groups);
      Node *c = gen_node(r, depth - 1, groups, capture_bias);
      Node *grp = n_grp(c, g);
      grp->named = rng_below(r, 3) == 0;
      return grp;
    }
    default: { /* 10..11 */
      Node *c = gen_node(r, depth - 1, groups, capture_bias);
      int lo = (int)rng_below(r, TASK_REP_MAX + 1);
      int hi = lo + (int)rng_below(r, (uint32_t)(TASK_REP_MAX - lo + 1));
      return n_rep(c, lo, hi);
    }
  }
}

Node *ast_generate(TaskRng *r, int *out_captures) {
  Node *n = NULL;
  int g = 0;
  for (int tries = 0; tries < 100; tries++) {
    g = 0;
    n = gen_node(r, TASK_MAX_DEPTH, &g, rng_below(r, 8) == 0);
    if (g >= 1) break;
    ast_free(n);
    n = NULL;
  }
  *out_captures = g;
  return n;
}

/* ---------------- Scalar-level serialization --------------------------- */

static int put_scalar_lit(uint32_t cp, uint32_t *out, int pos, int cap) {
  /* Only plain letters, digits and '%' are emitted bare; every other
   * ASCII scalar (e.g. '$', a regex metacharacter) goes through \x{..},
   * which is safe both inside and outside character classes. */
  int safe = (cp < 0x80u) &&
             ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
              (cp >= '0' && cp <= '9') || cp == '%');
  if (safe) {
    if (pos < cap) out[pos] = cp;
    return pos + 1;
  }
  char buf[16];
  int m = snprintf(buf, sizeof(buf), "\\x{%x}", cp);
  for (int i = 0; i < m; i++)
    if (pos + i < cap) out[pos + i] = (uint32_t)(unsigned char)buf[i];
  return pos + m;
}

static int serialize_node(const Node *n, uint32_t *out, int pos, int cap);

static int serialize_atom(const Node *n, uint32_t *out, int pos, int cap) {
  if (n->kind == N_LIT) return put_scalar_lit(n->cp, out, pos, cap);
  if (n->kind == N_CLS) {
    if (pos < cap) out[pos] = '[';
    pos++;
    for (int i = 0; i < n->ncls; i++)
      pos = put_scalar_lit(n->cps[i], out, pos, cap);
    if (pos < cap) out[pos] = ']';
    return pos + 1;
  }
  /* Composite atom: parenthesize explicitly. */
  if (pos < cap) out[pos] = '(';
  pos++;
  if (pos < cap) out[pos] = '?';
  pos++;
  if (pos < cap) out[pos] = ':';
  pos++;
  pos = serialize_node(n, out, pos, cap);
  if (pos < cap) out[pos] = ')';
  return pos + 1;
}

static int serialize_node(const Node *n, uint32_t *out, int pos, int cap) {
  switch (n->kind) {
    case N_LIT:
      return put_scalar_lit(n->cp, out, pos, cap);
    case N_CLS: {
      if (pos < cap) out[pos] = '[';
      pos++;
      for (int i = 0; i < n->ncls; i++)
        pos = put_scalar_lit(n->cps[i], out, pos, cap);
      if (pos < cap) out[pos] = ']';
      return pos + 1;
    }
    case N_CAT: {
      for (int i = 0; i < n->nkids; i++) {
        /* Alternations need grouping inside a concatenation. */
        if (n->kids[i]->kind == N_ALT)
          pos = serialize_atom(n->kids[i], out, pos, cap);
        else
          pos = serialize_node(n->kids[i], out, pos, cap);
      }
      return pos;
    }
    case N_ALT: {
      for (int i = 0; i < n->nkids; i++) {
        if (i) {
          if (pos < cap) out[pos] = '|';
          pos++;
        }
        pos = serialize_node(n->kids[i], out, pos, cap);
      }
      return pos;
    }
    case N_GRP: {
      if (n->group == 0) {
        if (pos < cap) out[pos] = '(';
        pos++;
        if (pos < cap) out[pos] = '?';
        pos++;
        if (pos < cap) out[pos] = ':';
        pos++;
      } else if (n->named) {
        char buf[24];
        int m = snprintf(buf, sizeof(buf), "(?P<g%d>", n->group);
        for (int i = 0; i < m; i++)
          if (pos + i < cap) out[pos + i] = (uint32_t)(unsigned char)buf[i];
        pos += m;
      } else {
        if (pos < cap) out[pos] = '(';
        pos++;
      }
      pos = serialize_node(n->a, out, pos, cap);
      if (pos < cap) out[pos] = ')';
      return pos + 1;
    }
    case N_REP: {
      char buf[24];
      pos = serialize_atom(n->a, out, pos, cap);
      int m = snprintf(buf, sizeof(buf), "{%d,%d}", n->rep_min, n->rep_max);
      for (int i = 0; i < m; i++)
        if (pos + i < cap) out[pos + i] = (uint32_t)(unsigned char)buf[i];
      return pos + m;
    }
  }
  return pos;
}

int ast_serialize(const Node *n, uint32_t *out, int cap) {
  int pos = serialize_node(n, out, 0, cap);
  if (pos < cap) out[pos] = 0;
  else out[cap - 1] = 0;
  return pos;
}

/* ---------------- Human-readable AST rendering ------------------------- */

static int render_char(char *out, int pos, int cap, uint32_t cp) {
  char buf[16];
  int m = cp < 0x80u ? snprintf(buf, sizeof(buf), "%c", (int)cp)
                     : snprintf(buf, sizeof(buf), "\\x{%x}", cp);
  for (int i = 0; i < m; i++)
    if (pos + i < cap - 1) out[pos + i] = buf[i];
  return pos + m;
}

static int render_node(const Node *n, char *out, int pos, int cap);

static int render_node(const Node *n, char *out, int pos, int cap) {
  switch (n->kind) {
    case N_LIT:
      return render_char(out, pos, cap, n->cp);
    case N_CLS: {
      if (pos < cap - 1) out[pos] = '[';
      pos++;
      for (int i = 0; i < n->ncls; i++)
        pos = render_char(out, pos, cap, n->cps[i]);
      if (pos < cap - 1) out[pos] = ']';
      return pos + 1;
    }
    case N_CAT: {
      if (pos < cap - 1) out[pos] = 'C';
      pos++;
      if (pos < cap - 1) out[pos] = '(';
      pos++;
      for (int i = 0; i < n->nkids; i++) {
        if (i) { if (pos < cap - 1) out[pos] = ','; pos++; }
        pos = render_node(n->kids[i], out, pos, cap);
      }
      if (pos < cap - 1) out[pos] = ')';
      return pos + 1;
    }
    case N_ALT: {
      if (pos < cap - 1) out[pos] = 'A';
      pos++;
      if (pos < cap - 1) out[pos] = '(';
      pos++;
      for (int i = 0; i < n->nkids; i++) {
        if (i) { if (pos < cap - 1) out[pos] = '|'; pos++; }
        pos = render_node(n->kids[i], out, pos, cap);
      }
      if (pos < cap - 1) out[pos] = ')';
      return pos + 1;
    }
    case N_GRP: {
      char buf[24];
      int m = n->group == 0
                  ? snprintf(buf, sizeof(buf), "G?:(")
                  : (n->named ? snprintf(buf, sizeof(buf), "G%d<g%d>(", n->group, n->group)
                              : snprintf(buf, sizeof(buf), "G%d(", n->group));
      for (int i = 0; i < m; i++)
        if (pos + i < cap - 1) out[pos + i] = buf[i];
      pos += m;
      pos = render_node(n->a, out, pos, cap);
      if (pos < cap - 1) out[pos] = ')';
      return pos + 1;
    }
    case N_REP: {
      char buf[24];
      int m = snprintf(buf, sizeof(buf), "R{%d,%d}(", n->rep_min, n->rep_max);
      for (int i = 0; i < m; i++)
        if (pos + i < cap - 1) out[pos + i] = buf[i];
      pos += m;
      pos = render_node(n->a, out, pos, cap);
      if (pos < cap - 1) out[pos] = ')';
      return pos + 1;
    }
  }
  return pos;
}

int ast_render(const Node *n, char *out, int cap) {
  int pos = render_node(n, out, 0, cap);
  if (pos < cap) out[pos] = 0;
  else out[cap - 1] = 0;
  return pos;
}

/* ---------------- Metamorphic transformations -------------------------- */

/* Recursively wrap every concatenation child in a non-capturing group. */
static Node *add_noncap(const Node *n) {
  switch (n->kind) {
    case N_LIT:
      return n_lit(n->cp);
    case N_CLS:
      return n_cls(n->cps, n->ncls);
    case N_CAT: case N_ALT: {
      Node **kids = malloc((size_t)n->nkids * sizeof(Node *));
      for (int i = 0; i < n->nkids; i++) {
        Node *child = add_noncap(n->kids[i]);
        kids[i] = n->kind == N_CAT ? n_grp(child, 0) : child;
      }
      return n->kind == N_CAT ? n_cat(kids, n->nkids) : n_alt(kids, n->nkids);
    }
    case N_GRP:
      return n_grp(add_noncap(n->a), n->group);
    case N_REP:
      return n_rep(add_noncap(n->a), n->rep_min, n->rep_max);
  }
  return NULL;
}

Node *xform_noncap_groups(const Node *n) { return add_noncap(n); }

/* Recursively remove every non-capturing group. */
static Node *drop_noncap(const Node *n) {
  switch (n->kind) {
    case N_LIT:
      return n_lit(n->cp);
    case N_CLS:
      return n_cls(n->cps, n->ncls);
    case N_CAT: case N_ALT: {
      Node **kids = malloc((size_t)n->nkids * sizeof(Node *));
      for (int i = 0; i < n->nkids; i++) kids[i] = drop_noncap(n->kids[i]);
      return n->kind == N_CAT ? n_cat(kids, n->nkids) : n_alt(kids, n->nkids);
    }
    case N_GRP: {
      Node *inner = drop_noncap(n->a);
      return n->group == 0 ? inner : n_grp(inner, n->group);
    }
    case N_REP:
      return n_rep(drop_noncap(n->a), n->rep_min, n->rep_max);
  }
  return NULL;
}

Node *xform_drop_noncap(const Node *n) { return drop_noncap(n); }

static void renumber(Node *n, int delta) {
  if (n->kind == N_GRP) {
    if (n->group != 0) n->group += delta;
    renumber(n->a, delta);
  } else if (n->kind == N_REP) {
    renumber(n->a, delta);
  } else if (n->kind == N_CAT || n->kind == N_ALT) {
    for (int i = 0; i < n->nkids; i++) renumber(n->kids[i], delta);
  }
}

int ast_group_count(const Node *root) {
  const Node *stack[128];
  int sp = 0, maxg = 0;
  stack[sp++] = root;
  while (sp > 0) {
    const Node *cur = stack[--sp];
    if (cur->kind == N_GRP) {
      if (cur->group > maxg) maxg = cur->group;
      stack[sp++] = cur->a;
    } else if (cur->kind == N_REP) {
      stack[sp++] = cur->a;
    } else if (cur->kind == N_CAT || cur->kind == N_ALT) {
      for (int i = 0; i < cur->nkids; i++) stack[sp++] = cur->kids[i];
    }
  }
  return maxg;
}

Node *xform_capture_wrap(const Node *n, int *map, int mapcap,
                         int *out_newcount) {
  int old = ast_group_count(n);
  Node *inner = ast_clone(n);
  renumber(inner, 1); /* old group i becomes i+1 under the new outer group */
  for (int i = 1; i <= old && i < mapcap; i++) map[i] = i + 1;
  if (out_newcount) *out_newcount = old + 1;
  return n_grp(inner, 1);
}

Node *xform_rename(const Node *n) {
  Node *c = ast_clone(n);
  Node *stack[128];
  int sp = 0;
  stack[sp++] = c;
  while (sp > 0) {
    Node *cur = stack[--sp];
    if (cur->kind == N_GRP) {
      if (cur->group != 0) cur->named = 1;
      stack[sp++] = cur->a;
    } else if (cur->kind == N_REP) {
      stack[sp++] = cur->a;
    } else if (cur->kind == N_CAT || cur->kind == N_ALT) {
      for (int i = 0; i < cur->nkids; i++) stack[sp++] = cur->kids[i];
    }
  }
  return c;
}
