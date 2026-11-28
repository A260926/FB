#ifndef TASK_AST_H
#define TASK_AST_H

#include <stddef.h>
#include <stdint.h>

/*
 * Logical regex AST over Unicode code points (scalar values only:
 * surrogates and out-of-range values are never produced).
 *
 * Node kinds, deliberately restricted to the defined, supported subset:
 *   N_LIT  literal scalar
 *   N_CLS  non-empty explicit character class [...]
 *   N_CAT  concatenation of >=2 children
 *   N_ALT  alternation of >=2 branches (leftmost-first semantics)
 *   N_GRP  capturing group (...), number assigned left-to-right
 *   N_REP  bounded repetition {min,max}, 0 <= min <= max <= 4, greedy
 */
typedef enum { N_LIT = 0, N_CLS, N_CAT, N_ALT, N_GRP, N_REP } NodeKind;

#define TASK_MAX_GROUPS 8
#define TASK_MAX_DEPTH 5
#define TASK_MAX_TEXT 128
#define TASK_REP_MAX 4
#define TASK_MAX_CLS 5

typedef struct Node Node;
struct Node {
  NodeKind kind;
  Node *a;          /* N_GRP child, N_REP child */
  Node **kids;      /* N_CAT / N_ALT children */
  int nkids;
  uint32_t cp;      /* N_LIT scalar */
  uint32_t *cps;    /* N_CLS scalars (sorted, unique), length ncls */
  int ncls;
  int rep_min;      /* N_REP */
  int rep_max;      /* N_REP */
  int group;        /* N_GRP: 1-based capture number, 0 if non-capturing */
  int named;        /* N_GRP: 1 if (?P<name>...) */
  int depth;        /* max node depth, leaves = 1 */
};

/* Deterministic, self-contained PRNG (splitmix64), seeded explicitly so a
 * fixed seed replays every generated case byte-for-byte. */
typedef struct {
  uint64_t state;
} TaskRng;

void rng_seed(TaskRng *r, uint64_t seed);
uint64_t rng_next(TaskRng *r);
uint32_t rng_below(TaskRng *r, uint32_t n);

/* Scalar pools: ASCII, non-ASCII BMP, supplementary planes; no surrogates. */
uint32_t gen_scalar(TaskRng *r, int plane_bias);

/* AST generation. Returns NULL only when no capturing slot can be obtained
 * (caller retries with a fresh tree); caller frees with ast_free. */
Node *ast_generate(TaskRng *r, int *out_captures);
void ast_free(Node *n);
Node *ast_clone(const Node *n);
int ast_group_count(const Node *root);

/* Serialize the logical AST into a PCRE2 pattern expressed as Unicode
 * scalar text (ASCII syntax; non-ASCII scalars emitted as \x{HHHH}).
 * Writes NUL-terminated scalar array; returns scalar count excluding NUL. */
int ast_serialize(const Node *n, uint32_t *out, int cap);

/* Human-readable one-line rendering of the logical AST. */
int ast_render(const Node *n, char *out, int cap);

/* Scalar text generation (length 0..TASK_MAX_TEXT), same three pools. */
int gen_text(TaskRng *r, uint32_t *out, int cap);

/* ---- Metamorphic transformations ---- */

/* Wrap every top-level child of the root in an extra non-capturing group
 * (root is forced to N_CAT): (?:a)(?:b)... Language of the pattern and all
 * capture numbers/values are unchanged. */
Node *xform_noncap_groups(const Node *n);

/* Wrap the whole pattern in one new capturing group: (?:pattern) ->
 * (pattern). Old group i maps to new group i+1; new group 1 equals the
 * overall match. Returns the mapping in map[old+1] = new. */
Node *xform_capture_wrap(const Node *n, int *map, int mapcap, int *out_newcount);

/* Convert every capturing group to a named group (?P<gN>...) preserving
 * order and numbers (safe renaming: fresh, unique ASCII names). */
Node *xform_rename(const Node *n);

/* Drop redundant non-capturing groups produced by xform_noncap_groups:
 * rebuilds the same language; used to prove the add/remove pair inverts. */
Node *xform_drop_noncap(const Node *n);

/* ---- Match result of the independent oracle / the real library ---- */
/* Scalar-level match. ovec stores scalar offsets; the value SIZE_MAX marks
 * groups that did not participate, distinguishing them from groups that
 * matched the empty string. Returns 1 on match, 0 on no match. */
int oracle_match(const Node *root, const uint32_t *text, int tlen,
                 size_t *ovec, int ovecap);

#endif
