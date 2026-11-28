#ifndef TASK_ENGINE_H
#define TASK_ENGINE_H

#include "task_ast.h"

#include <stddef.h>
#include <stdint.h>

#define TASK_CASES_DEFAULT 500
#define TASK_DIFF_MSG 192

/* Node constructors shared with hand-built cases. */
Node *n_lit(uint32_t cp);
Node *n_cls(const uint32_t *cps, int n);
Node *n_cat(Node **kids, int n);
Node *n_alt(Node **kids, int n);
Node *n_grp(Node *child, int group);
Node *n_rep(Node *child, int lo, int hi);

/* ---------------- UTF encoders (scalar array -> code units) ------------ */

int enc_utf8(const uint32_t *scalars, int n, uint8_t *out, int cap);
int enc_utf16(const uint32_t *scalars, int n, uint16_t *out, int cap);
int enc_utf32(const uint32_t *scalars, int n, uint32_t *out, int cap);

/*
 * Boundary tables: b[k] == 1 iff code-unit offset k is the start of a
 * scalar (offset 0 and the offset just past the end are boundaries).
 * table length must be unit_count+1.
 */
void bound_utf8(const uint8_t *units, int n, uint8_t *table);
void bound_utf16(const uint16_t *units, int n, uint8_t *table);
void bound_utf32(const uint32_t *units, int n, uint8_t *table);

/* Map a code-unit offset to its scalar index using a boundary table.
 * Returns -1 when the offset lands inside a code point. */
int map_offset(const uint8_t *table, int unit_count, size_t offset);

/* ---------------- Real library execution ------------------------------- */

typedef struct {
  int width;                 /* 8, 16 or 32 */
  int compile_error;         /* 1 if pcre2_compile failed (never swallowed) */
  int runtime_error;         /* 1 if pcre2_match returned an untracked rc */
  int matched;               /* 1 match, 0 no match */
  int ngroups;
  size_t ovec[(TASK_MAX_GROUPS + 1) * 2]; /* scalar offsets; SIZE_MAX unset */
  int boundary_violation;    /* an offset landed inside a code point */
  char note[96];
} LibResult;

/* Compile the serialized pattern and match the scalar text at one width.
 * Pattern scalars are ASCII, so they go through the same encoder. */
LibResult lib_run(int width, const uint32_t *pattern, int plen,
                  const uint32_t *text, int tlen);

/* ---------------- Oracle vs library comparison ------------------------- */

typedef struct {
  int case_index;
  uint64_t seed;
  int width;
  char msg[TASK_DIFF_MSG];
} FirstDiff;

void diff_reset(FirstDiff *d);

/* Compare a scalar-level oracle ovector against a mapped library result.
 * Records the first mismatch into diff (case_index stays -1 when equal). */
int compare_results(uint64_t seed, int case_index, int width,
                    int oracle_matched, const size_t *oracle_ov,
                    int ngroups, const LibResult *lib, FirstDiff *diff);

/* ---------------- Coverage accounting ---------------------------------- */

typedef struct {
  long cases;
  long depth_hist[TASK_MAX_DEPTH + 1];
  long captures_hist[TASK_MAX_GROUPS + 1];
  long supplementary_scalars;
  long empty_captures;    /* participated, matched the empty string */
  long unset_captures;    /* never participated */
  long branch_nodes;
  long repetition_nodes;
  long class_nodes;
  long matches;
  long no_matches;
  long compile_errors;
  long boundary_violations;
  long mismatches;
} Coverage;

void coverage_init(Coverage *c);
void coverage_record(Coverage *c, const Node *root,
                     const uint32_t *text, int tlen,
                     int oracle_matched, const size_t *oracle_ov,
                     const LibResult *r8, const LibResult *r16,
                     const LibResult *r32);
void coverage_print(const Coverage *c);

/* Render scalar text as compact "U+xxxx" sequence. */
int render_scalars(const uint32_t *s, int n, char *out, int cap);

/* ---------------- Deterministic discrepancy simulator ------------------ */

/* Deliberately mutates an oracle answer in-memory and proves the
 * comparison harness flags it and retains the first failing case. This
 * lives entirely on the test side; no upstream code is modified. */
int simulator_self_check(void);

#endif
