/**********************************************************************
  reggraph.c - read-only compiled-regex control-flow / call graph view
  for Oniguruma 6.9.5
**********************************************************************/
#include "regint.h"
#include "reggraph.h"

#include <stdio.h>   /* snprintf */
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Public opcode name table (fixed numbering, independent of macro
   configuration).  The mapping from the internal enum to the public
   numbering is done by map_opcode(), never by casting.  That keeps the
   numbering stable when a build disables one of the guarded opcodes.  */
/* ------------------------------------------------------------------ */

typedef struct {
  int code;
  const char* name;
} GraphOpName;

static const GraphOpName GraphOpNames[ONIG_GRAPH_OPCODE_COUNT] = {
  { ONIG_GRAPH_OPCODE_FINISH, "finish" },
  { ONIG_GRAPH_OPCODE_END, "end" },
  { ONIG_GRAPH_OPCODE_STR_1, "str_1" },
  { ONIG_GRAPH_OPCODE_STR_2, "str_2" },
  { ONIG_GRAPH_OPCODE_STR_3, "str_3" },
  { ONIG_GRAPH_OPCODE_STR_4, "str_4" },
  { ONIG_GRAPH_OPCODE_STR_5, "str_5" },
  { ONIG_GRAPH_OPCODE_STR_N, "str_n" },
  { ONIG_GRAPH_OPCODE_STR_MB2N1, "str_mb2-n1" },
  { ONIG_GRAPH_OPCODE_STR_MB2N2, "str_mb2-n2" },
  { ONIG_GRAPH_OPCODE_STR_MB2N3, "str_mb2-n3" },
  { ONIG_GRAPH_OPCODE_STR_MB2N, "str_mb2-n" },
  { ONIG_GRAPH_OPCODE_STR_MB3N, "str_mb3n" },
  { ONIG_GRAPH_OPCODE_STR_MBN, "str_mbn" },
  { ONIG_GRAPH_OPCODE_CCLASS, "cclass" },
  { ONIG_GRAPH_OPCODE_CCLASS_MB, "cclass-mb" },
  { ONIG_GRAPH_OPCODE_CCLASS_MIX, "cclass-mix" },
  { ONIG_GRAPH_OPCODE_CCLASS_NOT, "cclass-not" },
  { ONIG_GRAPH_OPCODE_CCLASS_MB_NOT, "cclass-mb-not" },
  { ONIG_GRAPH_OPCODE_CCLASS_MIX_NOT, "cclass-mix-not" },
  { ONIG_GRAPH_OPCODE_ANYCHAR, "anychar" },
  { ONIG_GRAPH_OPCODE_ANYCHAR_ML, "anychar-ml" },
  { ONIG_GRAPH_OPCODE_ANYCHAR_STAR, "anychar*" },
  { ONIG_GRAPH_OPCODE_ANYCHAR_ML_STAR, "anychar-ml*" },
  { ONIG_GRAPH_OPCODE_ANYCHAR_STAR_PEEK_NEXT, "anychar*-peek-next" },
  { ONIG_GRAPH_OPCODE_ANYCHAR_ML_STAR_PEEK_NEXT, "anychar-ml*-peek-next" },
  { ONIG_GRAPH_OPCODE_WORD, "word" },
  { ONIG_GRAPH_OPCODE_WORD_ASCII, "word-ascii" },
  { ONIG_GRAPH_OPCODE_NO_WORD, "not-word" },
  { ONIG_GRAPH_OPCODE_NO_WORD_ASCII, "not-word-ascii" },
  { ONIG_GRAPH_OPCODE_WORD_BOUNDARY, "word-boundary" },
  { ONIG_GRAPH_OPCODE_NO_WORD_BOUNDARY, "not-word-boundary" },
  { ONIG_GRAPH_OPCODE_WORD_BEGIN, "word-begin" },
  { ONIG_GRAPH_OPCODE_WORD_END, "word-end" },
  { ONIG_GRAPH_OPCODE_TEXT_SEGMENT_BOUNDARY, "text-segment-boundary" },
  { ONIG_GRAPH_OPCODE_BEGIN_BUF, "begin-buf" },
  { ONIG_GRAPH_OPCODE_END_BUF, "end-buf" },
  { ONIG_GRAPH_OPCODE_BEGIN_LINE, "begin-line" },
  { ONIG_GRAPH_OPCODE_END_LINE, "end-line" },
  { ONIG_GRAPH_OPCODE_SEMI_END_BUF, "semi-end-buf" },
  { ONIG_GRAPH_OPCODE_CHECK_POSITION, "check-position" },
  { ONIG_GRAPH_OPCODE_BACKREF1, "backref1" },
  { ONIG_GRAPH_OPCODE_BACKREF2, "backref2" },
  { ONIG_GRAPH_OPCODE_BACKREF_N, "backref-n" },
  { ONIG_GRAPH_OPCODE_BACKREF_N_IC, "backref-n-ic" },
  { ONIG_GRAPH_OPCODE_BACKREF_MULTI, "backref_multi" },
  { ONIG_GRAPH_OPCODE_BACKREF_MULTI_IC, "backref_multi-ic" },
  { ONIG_GRAPH_OPCODE_BACKREF_WITH_LEVEL, "backref_with_level" },
  { ONIG_GRAPH_OPCODE_BACKREF_WITH_LEVEL_IC, "backref_with_level-c" },
  { ONIG_GRAPH_OPCODE_BACKREF_CHECK, "backref_check" },
  { ONIG_GRAPH_OPCODE_BACKREF_CHECK_WITH_LEVEL, "backref_check_with_level" },
  { ONIG_GRAPH_OPCODE_MEM_START, "mem-start" },
  { ONIG_GRAPH_OPCODE_MEM_START_PUSH, "mem-start-push" },
  { ONIG_GRAPH_OPCODE_MEM_END_PUSH, "mem-end-push" },
  { ONIG_GRAPH_OPCODE_MEM_END_PUSH_REC, "mem-end-push-rec" },
  { ONIG_GRAPH_OPCODE_MEM_END, "mem-end" },
  { ONIG_GRAPH_OPCODE_MEM_END_REC, "mem-end-rec" },
  { ONIG_GRAPH_OPCODE_FAIL, "fail" },
  { ONIG_GRAPH_OPCODE_JUMP, "jump" },
  { ONIG_GRAPH_OPCODE_PUSH, "push" },
  { ONIG_GRAPH_OPCODE_PUSH_SUPER, "push-super" },
  { ONIG_GRAPH_OPCODE_POP, "pop" },
  { ONIG_GRAPH_OPCODE_POP_TO_MARK, "pop-to-mark" },
  { ONIG_GRAPH_OPCODE_PUSH_OR_JUMP_EXACT1, "push-or-jump-e1" },
  { ONIG_GRAPH_OPCODE_PUSH_IF_PEEK_NEXT, "push-if-peek-next" },
  { ONIG_GRAPH_OPCODE_REPEAT, "repeat" },
  { ONIG_GRAPH_OPCODE_REPEAT_NG, "repeat-ng" },
  { ONIG_GRAPH_OPCODE_REPEAT_INC, "repeat-inc" },
  { ONIG_GRAPH_OPCODE_REPEAT_INC_NG, "repeat-inc-ng" },
  { ONIG_GRAPH_OPCODE_EMPTY_CHECK_START, "empty-check-start" },
  { ONIG_GRAPH_OPCODE_EMPTY_CHECK_END, "empty-check-end" },
  { ONIG_GRAPH_OPCODE_EMPTY_CHECK_END_MEMST, "empty-check-end-memst" },
  { ONIG_GRAPH_OPCODE_EMPTY_CHECK_END_MEMST_PUSH, "empty-check-end-memst-push" },
  { ONIG_GRAPH_OPCODE_MOVE, "move" },
  { ONIG_GRAPH_OPCODE_STEP_BACK_START, "step-back-start" },
  { ONIG_GRAPH_OPCODE_STEP_BACK_NEXT, "step-back-next" },
  { ONIG_GRAPH_OPCODE_CUT_TO_MARK, "cut-to-mark" },
  { ONIG_GRAPH_OPCODE_MARK, "mark" },
  { ONIG_GRAPH_OPCODE_SAVE_VAL, "save-val" },
  { ONIG_GRAPH_OPCODE_UPDATE_VAR, "update-var" },
  { ONIG_GRAPH_OPCODE_CALL, "call" },
  { ONIG_GRAPH_OPCODE_RETURN, "return" },
  { ONIG_GRAPH_OPCODE_CALLOUT_CONTENTS, "callout-contents" },
  { ONIG_GRAPH_OPCODE_CALLOUT_NAME, "callout-name" }
};

static const char* const GraphEdgeNames[ONIG_GRAPH_EDGE_TYPE_COUNT] = {
  "fallthrough",
  "jump",
  "alt-push",
  "cond-continue",
  "cond-jump",
  "cond-push",
  "repeat-enter",
  "repeat-skip",
  "repeat-back",
  "repeat-exit",
  "repeat-ng-enter",
  "repeat-ng-skip",
  "repeat-ng-back",
  "repeat-ng-exit",
  "call",
  "resume-summary",
  "empty-check-skip"
};

static const char* const GraphErrorNames[] = {
  "ok",
  "invalid-argument",
  "empty-program",
  "memory",
  "decode",
  "unknown-opcode",
  "unsupported-opcode",
  "invalid-target",
  "invalid-repeat",
  "inconsistent-repeat",
  "invalid-memory"
};

/* Map the internal enum to the fixed public numbering.  This uses the
   enum identifiers, never the numeric value, so a build with a guarded
   opcode disabled still maps correctly.  Returns -1 for anything not
   part of the fixed release. */
static int
map_opcode(enum OpCode oc)
{
  switch (oc) {
  case OP_FINISH: return ONIG_GRAPH_OPCODE_FINISH;
  case OP_END: return ONIG_GRAPH_OPCODE_END;
  case OP_STR_1: return ONIG_GRAPH_OPCODE_STR_1;
  case OP_STR_2: return ONIG_GRAPH_OPCODE_STR_2;
  case OP_STR_3: return ONIG_GRAPH_OPCODE_STR_3;
  case OP_STR_4: return ONIG_GRAPH_OPCODE_STR_4;
  case OP_STR_5: return ONIG_GRAPH_OPCODE_STR_5;
  case OP_STR_N: return ONIG_GRAPH_OPCODE_STR_N;
  case OP_STR_MB2N1: return ONIG_GRAPH_OPCODE_STR_MB2N1;
  case OP_STR_MB2N2: return ONIG_GRAPH_OPCODE_STR_MB2N2;
  case OP_STR_MB2N3: return ONIG_GRAPH_OPCODE_STR_MB2N3;
  case OP_STR_MB2N: return ONIG_GRAPH_OPCODE_STR_MB2N;
  case OP_STR_MB3N: return ONIG_GRAPH_OPCODE_STR_MB3N;
  case OP_STR_MBN: return ONIG_GRAPH_OPCODE_STR_MBN;
  case OP_CCLASS: return ONIG_GRAPH_OPCODE_CCLASS;
  case OP_CCLASS_MB: return ONIG_GRAPH_OPCODE_CCLASS_MB;
  case OP_CCLASS_MIX: return ONIG_GRAPH_OPCODE_CCLASS_MIX;
  case OP_CCLASS_NOT: return ONIG_GRAPH_OPCODE_CCLASS_NOT;
  case OP_CCLASS_MB_NOT: return ONIG_GRAPH_OPCODE_CCLASS_MB_NOT;
  case OP_CCLASS_MIX_NOT: return ONIG_GRAPH_OPCODE_CCLASS_MIX_NOT;
  case OP_ANYCHAR: return ONIG_GRAPH_OPCODE_ANYCHAR;
  case OP_ANYCHAR_ML: return ONIG_GRAPH_OPCODE_ANYCHAR_ML;
  case OP_ANYCHAR_STAR: return ONIG_GRAPH_OPCODE_ANYCHAR_STAR;
  case OP_ANYCHAR_ML_STAR: return ONIG_GRAPH_OPCODE_ANYCHAR_ML_STAR;
  case OP_ANYCHAR_STAR_PEEK_NEXT:
    return ONIG_GRAPH_OPCODE_ANYCHAR_STAR_PEEK_NEXT;
  case OP_ANYCHAR_ML_STAR_PEEK_NEXT:
    return ONIG_GRAPH_OPCODE_ANYCHAR_ML_STAR_PEEK_NEXT;
  case OP_WORD: return ONIG_GRAPH_OPCODE_WORD;
  case OP_WORD_ASCII: return ONIG_GRAPH_OPCODE_WORD_ASCII;
  case OP_NO_WORD: return ONIG_GRAPH_OPCODE_NO_WORD;
  case OP_NO_WORD_ASCII: return ONIG_GRAPH_OPCODE_NO_WORD_ASCII;
  case OP_WORD_BOUNDARY: return ONIG_GRAPH_OPCODE_WORD_BOUNDARY;
  case OP_NO_WORD_BOUNDARY: return ONIG_GRAPH_OPCODE_NO_WORD_BOUNDARY;
  case OP_WORD_BEGIN: return ONIG_GRAPH_OPCODE_WORD_BEGIN;
  case OP_WORD_END: return ONIG_GRAPH_OPCODE_WORD_END;
  case OP_TEXT_SEGMENT_BOUNDARY:
    return ONIG_GRAPH_OPCODE_TEXT_SEGMENT_BOUNDARY;
  case OP_BEGIN_BUF: return ONIG_GRAPH_OPCODE_BEGIN_BUF;
  case OP_END_BUF: return ONIG_GRAPH_OPCODE_END_BUF;
  case OP_BEGIN_LINE: return ONIG_GRAPH_OPCODE_BEGIN_LINE;
  case OP_END_LINE: return ONIG_GRAPH_OPCODE_END_LINE;
  case OP_SEMI_END_BUF: return ONIG_GRAPH_OPCODE_SEMI_END_BUF;
  case OP_CHECK_POSITION: return ONIG_GRAPH_OPCODE_CHECK_POSITION;
  case OP_BACKREF1: return ONIG_GRAPH_OPCODE_BACKREF1;
  case OP_BACKREF2: return ONIG_GRAPH_OPCODE_BACKREF2;
  case OP_BACKREF_N: return ONIG_GRAPH_OPCODE_BACKREF_N;
  case OP_BACKREF_N_IC: return ONIG_GRAPH_OPCODE_BACKREF_N_IC;
  case OP_BACKREF_MULTI: return ONIG_GRAPH_OPCODE_BACKREF_MULTI;
  case OP_BACKREF_MULTI_IC: return ONIG_GRAPH_OPCODE_BACKREF_MULTI_IC;
  case OP_BACKREF_WITH_LEVEL: return ONIG_GRAPH_OPCODE_BACKREF_WITH_LEVEL;
  case OP_BACKREF_WITH_LEVEL_IC:
    return ONIG_GRAPH_OPCODE_BACKREF_WITH_LEVEL_IC;
  case OP_BACKREF_CHECK: return ONIG_GRAPH_OPCODE_BACKREF_CHECK;
  case OP_BACKREF_CHECK_WITH_LEVEL:
    return ONIG_GRAPH_OPCODE_BACKREF_CHECK_WITH_LEVEL;
  case OP_MEM_START: return ONIG_GRAPH_OPCODE_MEM_START;
  case OP_MEM_START_PUSH: return ONIG_GRAPH_OPCODE_MEM_START_PUSH;
  case OP_MEM_END_PUSH: return ONIG_GRAPH_OPCODE_MEM_END_PUSH;
#ifdef USE_CALL
  case OP_MEM_END_PUSH_REC: return ONIG_GRAPH_OPCODE_MEM_END_PUSH_REC;
#endif
  case OP_MEM_END: return ONIG_GRAPH_OPCODE_MEM_END;
#ifdef USE_CALL
  case OP_MEM_END_REC: return ONIG_GRAPH_OPCODE_MEM_END_REC;
#endif
  case OP_FAIL: return ONIG_GRAPH_OPCODE_FAIL;
  case OP_JUMP: return ONIG_GRAPH_OPCODE_JUMP;
  case OP_PUSH: return ONIG_GRAPH_OPCODE_PUSH;
  case OP_PUSH_SUPER: return ONIG_GRAPH_OPCODE_PUSH_SUPER;
  case OP_POP: return ONIG_GRAPH_OPCODE_POP;
  case OP_POP_TO_MARK: return ONIG_GRAPH_OPCODE_POP_TO_MARK;
#ifdef USE_OP_PUSH_OR_JUMP_EXACT
  case OP_PUSH_OR_JUMP_EXACT1:
    return ONIG_GRAPH_OPCODE_PUSH_OR_JUMP_EXACT1;
#endif
  case OP_PUSH_IF_PEEK_NEXT: return ONIG_GRAPH_OPCODE_PUSH_IF_PEEK_NEXT;
  case OP_REPEAT: return ONIG_GRAPH_OPCODE_REPEAT;
  case OP_REPEAT_NG: return ONIG_GRAPH_OPCODE_REPEAT_NG;
  case OP_REPEAT_INC: return ONIG_GRAPH_OPCODE_REPEAT_INC;
  case OP_REPEAT_INC_NG: return ONIG_GRAPH_OPCODE_REPEAT_INC_NG;
  case OP_EMPTY_CHECK_START: return ONIG_GRAPH_OPCODE_EMPTY_CHECK_START;
  case OP_EMPTY_CHECK_END: return ONIG_GRAPH_OPCODE_EMPTY_CHECK_END;
  case OP_EMPTY_CHECK_END_MEMST:
    return ONIG_GRAPH_OPCODE_EMPTY_CHECK_END_MEMST;
#ifdef USE_CALL
  case OP_EMPTY_CHECK_END_MEMST_PUSH:
    return ONIG_GRAPH_OPCODE_EMPTY_CHECK_END_MEMST_PUSH;
#endif
  case OP_MOVE: return ONIG_GRAPH_OPCODE_MOVE;
  case OP_STEP_BACK_START: return ONIG_GRAPH_OPCODE_STEP_BACK_START;
  case OP_STEP_BACK_NEXT: return ONIG_GRAPH_OPCODE_STEP_BACK_NEXT;
  case OP_CUT_TO_MARK: return ONIG_GRAPH_OPCODE_CUT_TO_MARK;
  case OP_MARK: return ONIG_GRAPH_OPCODE_MARK;
  case OP_SAVE_VAL: return ONIG_GRAPH_OPCODE_SAVE_VAL;
  case OP_UPDATE_VAR: return ONIG_GRAPH_OPCODE_UPDATE_VAR;
#ifdef USE_CALL
  case OP_CALL: return ONIG_GRAPH_OPCODE_CALL;
  case OP_RETURN: return ONIG_GRAPH_OPCODE_RETURN;
#endif
#ifdef USE_CALLOUT
  case OP_CALLOUT_CONTENTS: return ONIG_GRAPH_OPCODE_CALLOUT_CONTENTS;
  case OP_CALLOUT_NAME: return ONIG_GRAPH_OPCODE_CALLOUT_NAME;
#endif
  default: return -1;
  }
}

/* ------------------------------------------------------------------ */
/* Graph storage.                                                      */
/* ------------------------------------------------------------------ */

struct OnigGraph {
  int node_count;
  OnigGraphNode* nodes;

  /* Sorted by (from, type, to, deferred, conditional). */
  OnigGraphEdge* edges;
  int edge_count;
  int* out_start;          /* node_count + 1 offsets into edges */

  OnigGraphCall* calls;
  int call_count;
  int* call_of_node;       /* node_count, -1 or call record index */

  OnigGraphRepeat* repeats;
  int repeat_count;
  int* repeat_of_node;     /* node_count, -1 or repeat record index */

  int* entries;
  int entry_count;

  OnigGraphCallEdge* call_edges;
  int call_edge_count;

  OnigGraphScc* sccs;
  int scc_count;
  int* scc_members;        /* backing store for sccs[].members */
  int* entry_scc;          /* entry position -> scc index, -1 */
};

static void
set_graph_error(OnigGraphError* error, OnigGraphErrorReason reason,
                int op_index, int opcode, const char* detail)
{
  if (IS_NULL(error)) return;
  error->reason = reason;
  error->op_index = op_index;
  error->opcode = opcode;
  if (op_index >= 0) {
    snprintf(error->message, sizeof(error->message),
             "%s at op %d (opcode %s)%s%s",
             onig_graph_error_reason_name((int)reason), op_index,
             (opcode >= 0 ? onig_graph_opcode_name(opcode) : "?"),
             (detail != 0 ? ": " : ""), (detail != 0 ? detail : ""));
  }
  else {
    snprintf(error->message, sizeof(error->message), "%s%s%s",
             onig_graph_error_reason_name((int)reason),
             (detail != 0 ? ": " : ""), (detail != 0 ? detail : ""));
  }
}

const char*
onig_graph_opcode_name(int opcode)
{
  if (opcode < 0 || opcode >= ONIG_GRAPH_OPCODE_COUNT) return "?";
  return GraphOpNames[opcode].name;
}

const char*
onig_graph_edge_type_name(int edge_type)
{
  if (edge_type < 0 || edge_type >= ONIG_GRAPH_EDGE_TYPE_COUNT) return "?";
  return GraphEdgeNames[edge_type];
}

const char*
onig_graph_error_reason_name(int reason)
{
  if (reason < 0 ||
      reason >= (int)(sizeof(GraphErrorNames) / sizeof(GraphErrorNames[0])))
    return "?";
  return GraphErrorNames[reason];
}

/* ------------------------------------------------------------------ */
/* Dynamic edge buffer.                                                */
/* ------------------------------------------------------------------ */

typedef struct {
  OnigGraphEdge* e;
  int n;
  int cap;
} EdgeBuf;

static void
edge_buf_free(EdgeBuf* b)
{
  if (b->e != 0) free(b->e);
  b->e = 0;
  b->n = b->cap = 0;
}

static int
edge_buf_push(EdgeBuf* b, int from, int to, int type, int deferred,
              int conditional)
{
  OnigGraphEdge* x;
  int cap;

  if (b->n >= b->cap) {
    cap = (b->cap == 0 ? 32 : b->cap * 2);
    x = (OnigGraphEdge* )realloc(b->e, sizeof(OnigGraphEdge) * (size_t)cap);
    if (IS_NULL(x)) return 0;
    b->e = x;
    b->cap = cap;
  }
  b->e[b->n].from = from;
  b->e[b->n].to = to;
  b->e[b->n].type = type;
  b->e[b->n].deferred = deferred;
  b->e[b->n].conditional = conditional;
  b->n++;
  return 1;
}

static int
edge_compare(const void* pa, const void* pb)
{
  const OnigGraphEdge* a = (const OnigGraphEdge* )pa;
  const OnigGraphEdge* b = (const OnigGraphEdge* )pb;

  if (a->from != b->from) return (a->from < b->from ? -1 : 1);
  if (a->type != b->type) return (a->type < b->type ? -1 : 1);
  if (a->to != b->to) return (a->to < b->to ? -1 : 1);
  if (a->deferred != b->deferred) return (a->deferred < b->deferred ? -1 : 1);
  if (a->conditional != b->conditional)
    return (a->conditional < b->conditional ? -1 : 1);
  return 0;
}

static int
int_compare(const void* pa, const void* pb)
{
  int a = *(const int* )pa;
  int b = *(const int* )pb;
  if (a != b) return (a < b ? -1 : 1);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Decoding helpers.                                                   */
/* ------------------------------------------------------------------ */

static enum OpCode
decode_opcode(const regex_t* reg, int index)
{
#ifdef USE_DIRECT_THREADED_CODE
  return reg->ocs[index];
#else
  return reg->ops[index].opcode;
#endif
}

static int
literal_bytes(enum OpCode oc, const Operation* op, size_t* len,
              const UChar** src)
{
  size_t n;
  const UChar* p;

  *len = 0;
  *src = 0;
  switch (oc) {
  case OP_STR_1: *len = 1; *src = op->exact.s; break;
  case OP_STR_2: *len = 2; *src = op->exact.s; break;
  case OP_STR_3: *len = 3; *src = op->exact.s; break;
  case OP_STR_4: *len = 4; *src = op->exact.s; break;
  case OP_STR_5: *len = 5; *src = op->exact.s; break;
  case OP_STR_MB2N1: *len = 2; *src = op->exact.s; break;
  case OP_STR_MB2N2: *len = 4; *src = op->exact.s; break;
  case OP_STR_MB2N3: *len = 6; *src = op->exact.s; break;

  case OP_STR_N:
    if (op->exact_n.n < 0) return 0;
    n = (size_t)op->exact_n.n;
    *len = n; *src = op->exact_n.s; break;
  case OP_STR_MB2N:
    if (op->exact_n.n < 0) return 0;
    n = (size_t)op->exact_n.n * 2;
    *len = n; *src = op->exact_n.s; break;
  case OP_STR_MB3N:
    if (op->exact_n.n < 0) return 0;
    n = (size_t)op->exact_n.n * 3;
    *len = n; *src = op->exact_n.s; break;
  case OP_STR_MBN:
    if (op->exact_len_n.n < 0 || op->exact_len_n.len <= 0) return 0;
    n = (size_t)op->exact_len_n.n * (size_t)op->exact_len_n.len;
    *len = n; *src = op->exact_len_n.s; break;

  default:
    return 1;   /* not a literal; empty copy */
  }
  if (*len != 0 && IS_NULL(*src)) return 0;
  return 1;
}

/* Is this opcode part of the supported structural subset? */
static int
opcode_supported(int pub)
{
  switch (pub) {
  case ONIG_GRAPH_OPCODE_FINISH:
  case ONIG_GRAPH_OPCODE_END:
  case ONIG_GRAPH_OPCODE_STR_1:
  case ONIG_GRAPH_OPCODE_STR_2:
  case ONIG_GRAPH_OPCODE_STR_3:
  case ONIG_GRAPH_OPCODE_STR_4:
  case ONIG_GRAPH_OPCODE_STR_5:
  case ONIG_GRAPH_OPCODE_STR_N:
  case ONIG_GRAPH_OPCODE_STR_MB2N1:
  case ONIG_GRAPH_OPCODE_STR_MB2N2:
  case ONIG_GRAPH_OPCODE_STR_MB2N3:
  case ONIG_GRAPH_OPCODE_STR_MB2N:
  case ONIG_GRAPH_OPCODE_STR_MB3N:
  case ONIG_GRAPH_OPCODE_STR_MBN:
  case ONIG_GRAPH_OPCODE_CCLASS:
  case ONIG_GRAPH_OPCODE_CCLASS_MB:
  case ONIG_GRAPH_OPCODE_CCLASS_MIX:
  case ONIG_GRAPH_OPCODE_CCLASS_NOT:
  case ONIG_GRAPH_OPCODE_CCLASS_MB_NOT:
  case ONIG_GRAPH_OPCODE_CCLASS_MIX_NOT:
  case ONIG_GRAPH_OPCODE_ANYCHAR:
  case ONIG_GRAPH_OPCODE_ANYCHAR_ML:
  case ONIG_GRAPH_OPCODE_ANYCHAR_STAR:
  case ONIG_GRAPH_OPCODE_ANYCHAR_ML_STAR:
  case ONIG_GRAPH_OPCODE_ANYCHAR_STAR_PEEK_NEXT:
  case ONIG_GRAPH_OPCODE_ANYCHAR_ML_STAR_PEEK_NEXT:
  case ONIG_GRAPH_OPCODE_WORD:
  case ONIG_GRAPH_OPCODE_WORD_ASCII:
  case ONIG_GRAPH_OPCODE_NO_WORD:
  case ONIG_GRAPH_OPCODE_NO_WORD_ASCII:
  case ONIG_GRAPH_OPCODE_WORD_BOUNDARY:
  case ONIG_GRAPH_OPCODE_NO_WORD_BOUNDARY:
  case ONIG_GRAPH_OPCODE_WORD_BEGIN:
  case ONIG_GRAPH_OPCODE_WORD_END:
  case ONIG_GRAPH_OPCODE_TEXT_SEGMENT_BOUNDARY:
  case ONIG_GRAPH_OPCODE_BEGIN_BUF:
  case ONIG_GRAPH_OPCODE_END_BUF:
  case ONIG_GRAPH_OPCODE_BEGIN_LINE:
  case ONIG_GRAPH_OPCODE_END_LINE:
  case ONIG_GRAPH_OPCODE_SEMI_END_BUF:
  case ONIG_GRAPH_OPCODE_CHECK_POSITION:
  case ONIG_GRAPH_OPCODE_MEM_START:
  case ONIG_GRAPH_OPCODE_MEM_START_PUSH:
  case ONIG_GRAPH_OPCODE_MEM_END_PUSH:
  case ONIG_GRAPH_OPCODE_MEM_END_PUSH_REC:
  case ONIG_GRAPH_OPCODE_MEM_END:
  case ONIG_GRAPH_OPCODE_MEM_END_REC:
  case ONIG_GRAPH_OPCODE_FAIL:
  case ONIG_GRAPH_OPCODE_JUMP:
  case ONIG_GRAPH_OPCODE_PUSH:
  case ONIG_GRAPH_OPCODE_PUSH_SUPER:
  case ONIG_GRAPH_OPCODE_PUSH_OR_JUMP_EXACT1:
  case ONIG_GRAPH_OPCODE_PUSH_IF_PEEK_NEXT:
  case ONIG_GRAPH_OPCODE_REPEAT:
  case ONIG_GRAPH_OPCODE_REPEAT_NG:
  case ONIG_GRAPH_OPCODE_REPEAT_INC:
  case ONIG_GRAPH_OPCODE_REPEAT_INC_NG:
  case ONIG_GRAPH_OPCODE_EMPTY_CHECK_START:
  case ONIG_GRAPH_OPCODE_EMPTY_CHECK_END:
  case ONIG_GRAPH_OPCODE_EMPTY_CHECK_END_MEMST:
  case ONIG_GRAPH_OPCODE_EMPTY_CHECK_END_MEMST_PUSH:
  case ONIG_GRAPH_OPCODE_CALL:
  case ONIG_GRAPH_OPCODE_RETURN:
    return 1;

  /* Bookkeeping / group-boundary instructions.  These are emitted by
     captures, atomic groups, \K and lookaround.  They are modelled as
     ordinary single-successor instructions; the graph does not evaluate
     the runtime condition they consult. */
  case ONIG_GRAPH_OPCODE_POP:
  case ONIG_GRAPH_OPCODE_POP_TO_MARK:
  case ONIG_GRAPH_OPCODE_MARK:
  case ONIG_GRAPH_OPCODE_CUT_TO_MARK:
  case ONIG_GRAPH_OPCODE_SAVE_VAL:
  case ONIG_GRAPH_OPCODE_UPDATE_VAR:
  case ONIG_GRAPH_OPCODE_MOVE:
  case ONIG_GRAPH_OPCODE_STEP_BACK_START:
  case ONIG_GRAPH_OPCODE_STEP_BACK_NEXT:
    return 1;
  default:
    return 0;   /* never fall through to a sequential edge */
  }
}

/* ------------------------------------------------------------------ */
/* Repeat and call tables.                                             */
/* ------------------------------------------------------------------ */

static int
build_repeats(const regex_t* reg, const int* pubcodes, OnigGraph* g,
              OnigGraphError* error)
{
  int i, id, n = (int )reg->ops_used;
  int num_repeat = reg->num_repeat;
  int* entry_idx;
  int* inc_idx;

  if (num_repeat == 0) return 1;

  entry_idx = (int* )malloc(sizeof(int) * (size_t)num_repeat);
  inc_idx = (int* )malloc(sizeof(int) * (size_t)num_repeat);
  if (IS_NULL(entry_idx) || IS_NULL(inc_idx)) {
    if (entry_idx) free(entry_idx);
    if (inc_idx) free(inc_idx);
    set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "repeat table");
    return 0;
  }
  for (i = 0; i < num_repeat; i++) { entry_idx[i] = -1; inc_idx[i] = -1; }

  for (i = 0; i < n; i++) {
    int p = pubcodes[i];
    if (p == ONIG_GRAPH_OPCODE_REPEAT || p == ONIG_GRAPH_OPCODE_REPEAT_NG) {
      id = reg->ops[i].repeat.id;
      if (id < 0 || id >= num_repeat) {
        set_graph_error(error, ONIG_GRAPH_ERR_INVALID_REPEAT, i, p,
                        "repeat id out of range");
        free(entry_idx); free(inc_idx);
        return 0;
      }
      if (entry_idx[id] != -1) {
        set_graph_error(error, ONIG_GRAPH_ERR_INCONSISTENT_REPEAT, i, p,
                        "duplicate repeat entry for the same id");
        free(entry_idx); free(inc_idx);
        return 0;
      }
      entry_idx[id] = i;
    }
    else if (p == ONIG_GRAPH_OPCODE_REPEAT_INC ||
             p == ONIG_GRAPH_OPCODE_REPEAT_INC_NG) {
      id = reg->ops[i].repeat_inc.id;
      if (id < 0 || id >= num_repeat) {
        set_graph_error(error, ONIG_GRAPH_ERR_INVALID_REPEAT, i, p,
                        "repeat id out of range");
        free(entry_idx); free(inc_idx);
        return 0;
      }
      if (inc_idx[id] != -1) {
        set_graph_error(error, ONIG_GRAPH_ERR_INCONSISTENT_REPEAT, i, p,
                        "duplicate repeat increment for the same id");
        free(entry_idx); free(inc_idx);
        return 0;
      }
      inc_idx[id] = i;
    }
  }

  g->repeat_count = num_repeat;
  g->repeats = (OnigGraphRepeat* )calloc((size_t)num_repeat,
                                         sizeof(OnigGraphRepeat));
  g->repeat_of_node = (int* )malloc(sizeof(int) * (size_t)n);
  if (IS_NULL(g->repeats) || IS_NULL(g->repeat_of_node)) {
    free(entry_idx); free(inc_idx);
    set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "repeat records");
    return 0;
  }
  for (i = 0; i < n; i++) g->repeat_of_node[i] = -1;

  for (id = 0; id < num_repeat; id++) {
    int e = entry_idx[id];
    int inc = inc_idx[id];
    RepeatRange* rr;
    ptrdiff_t off;
    int body;
    int pub = 0;
    int exit_target;
    OnigGraphRepeat* rec;

    if (e < 0 || inc < 0) {
      int at = (e >= 0 ? e : inc);
      int atpub = (at >= 0 ? pubcodes[at] : -1);
      set_graph_error(error, ONIG_GRAPH_ERR_INCONSISTENT_REPEAT, at, atpub,
                      "repeat entry/increment pair is incomplete");
      free(entry_idx); free(inc_idx);
      return 0;
    }
    if (e == inc) {
      set_graph_error(error, ONIG_GRAPH_ERR_INCONSISTENT_REPEAT, e,
                      pubcodes[e], "repeat entry and increment are the same op");
      free(entry_idx); free(inc_idx);
      return 0;
    }

    pub = pubcodes[e];
    if (! ((pub == ONIG_GRAPH_OPCODE_REPEAT &&
            pubcodes[inc] == ONIG_GRAPH_OPCODE_REPEAT_INC) ||
           (pub == ONIG_GRAPH_OPCODE_REPEAT_NG &&
            pubcodes[inc] == ONIG_GRAPH_OPCODE_REPEAT_INC_NG))) {
      set_graph_error(error, ONIG_GRAPH_ERR_INCONSISTENT_REPEAT, inc,
                      pubcodes[inc],
                      "repeat entry and increment kinds disagree");
      free(entry_idx); free(inc_idx);
      return 0;
    }

    exit_target = e + reg->ops[e].repeat.addr;
    if (exit_target < 0 || exit_target >= n || exit_target != inc + 1) {
      set_graph_error(error, ONIG_GRAPH_ERR_INCONSISTENT_REPEAT, e, pub,
                      "repeat exit target does not match the increment");
      free(entry_idx); free(inc_idx);
      return 0;
    }

    if (IS_NULL(reg->repeat_range)) {
      set_graph_error(error, ONIG_GRAPH_ERR_INVALID_REPEAT, e, pub,
                      "missing repeat_range table");
      free(entry_idx); free(inc_idx);
      return 0;
    }
    rr = &reg->repeat_range[id];
    if (IS_NULL(rr->u.pcode)) {
      set_graph_error(error, ONIG_GRAPH_ERR_INVALID_REPEAT, e, pub,
                      "missing repeat body pointer");
      free(entry_idx); free(inc_idx);
      return 0;
    }
    off = (char* )rr->u.pcode - (char* )reg->ops;
    if (off < 0 || (off % (ptrdiff_t)sizeof(Operation)) != 0 ||
        off / (ptrdiff_t)sizeof(Operation) >= n) {
      set_graph_error(error, ONIG_GRAPH_ERR_INVALID_REPEAT, e, pub,
                      "repeat body pointer is out of range or misaligned");
      free(entry_idx); free(inc_idx);
      return 0;
    }
    body = (int )(off / (ptrdiff_t)sizeof(Operation));
    if (rr->lower < 0 ||
        (rr->upper != ONIG_GRAPH_INFINITE_REPEAT && rr->upper < rr->lower)) {
      set_graph_error(error, ONIG_GRAPH_ERR_INVALID_REPEAT, e, pub,
                      "repeat bounds are invalid");
      free(entry_idx); free(inc_idx);
      return 0;
    }

    rec = &g->repeats[id];
    rec->entry_index = e;
    rec->inc_index = inc;
    rec->body_index = body;
    rec->lower = rr->lower;
    rec->upper = rr->upper;
    rec->greedy = (pub == ONIG_GRAPH_OPCODE_REPEAT ? 1 : 0);
    g->repeat_of_node[e] = id;
    g->repeat_of_node[inc] = id;
  }

  /* Deterministic order: by entry_index.  The node->record map must be
     rewritten after the sort because the components move. */
  if (num_repeat > 1) {
    int* order = (int* )malloc(sizeof(int) * (size_t)num_repeat);
    OnigGraphRepeat* sorted =
      (OnigGraphRepeat* )malloc(sizeof(OnigGraphRepeat) * (size_t)num_repeat);
    if (IS_NULL(order) || IS_NULL(sorted)) {
      if (order) free(order);
      if (sorted) free(sorted);
      free(entry_idx); free(inc_idx);
      set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "repeat sort");
      return 0;
    }
    for (i = 0; i < num_repeat; i++) order[i] = i;
    for (i = 0; i < num_repeat; i++) {
      int j;
      for (j = i + 1; j < num_repeat; j++) {
        if (g->repeats[order[j]].entry_index <
            g->repeats[order[i]].entry_index) {
          int t = order[i]; order[i] = order[j]; order[j] = t;
        }
      }
    }
    for (i = 0; i < num_repeat; i++) {
      sorted[i] = g->repeats[order[i]];
      g->repeat_of_node[sorted[i].entry_index] = i;
      g->repeat_of_node[sorted[i].inc_index] = i;
    }
    free(g->repeats);
    g->repeats = sorted;
    free(order);
  }

  free(entry_idx); free(inc_idx);
  return 1;
}

/* ------------------------------------------------------------------ */
/* Edge construction.                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
  int ok;
  OnigGraphErrorReason reason;
  int op_index;
  int opcode;
  const char* detail;
} BuildStatus;

static void
edge_fail(BuildStatus* st, OnigGraphErrorReason reason, int op_index,
          int opcode, const char* detail)
{
  if (! st->ok) return;
  st->ok = 0;
  st->reason = reason;
  st->op_index = op_index;
  st->opcode = opcode;
  st->detail = detail;
}

static void
emit(BuildStatus* st, EdgeBuf* b, int from, int to, int type, int deferred,
     int conditional, int n, int opcode)
{
  if (! st->ok) return;
  if (to < 0 || to >= n) {
    edge_fail(st, ONIG_GRAPH_ERR_INVALID_TARGET, from, opcode,
              "edge target is outside the Operation array");
    return;
  }
  if (! edge_buf_push(b, from, to, type, deferred, conditional)) {
    edge_fail(st, ONIG_GRAPH_ERR_MEMORY, from, opcode, "edge buffer");
  }
}

static void
emit_fallthrough(BuildStatus* st, EdgeBuf* b, int from, int n, int opcode,
                 int conditional)
{
  emit(st, b, from, from + 1, ONIG_GRAPH_EDGE_FALLTHROUGH, 0, conditional,
       n, opcode);
}

/* Is a deferred exit / deferred body available for this repeat? */


static void
build_edges(const regex_t* reg, const int* pubcodes, OnigGraph* g,
            EdgeBuf* edges, BuildStatus* st)
{
  int i, n = (int )reg->ops_used;

  for (i = 0; i < n; i++) {
    int pub = pubcodes[i];
    Operation* op = &reg->ops[i];

    switch (pub) {
    case ONIG_GRAPH_OPCODE_FINISH:
    case ONIG_GRAPH_OPCODE_END:
    case ONIG_GRAPH_OPCODE_FAIL:
    case ONIG_GRAPH_OPCODE_RETURN:
      /* Exits.  OP_FAIL resolves dynamically through the backtrack
         stack; OP_RETURN through the dynamic call stack.  Neither has
         a static successor. */
      break;

    /* Pure consumers and assertions: a single success successor. */
    case ONIG_GRAPH_OPCODE_STR_1:
    case ONIG_GRAPH_OPCODE_STR_2:
    case ONIG_GRAPH_OPCODE_STR_3:
    case ONIG_GRAPH_OPCODE_STR_4:
    case ONIG_GRAPH_OPCODE_STR_5:
    case ONIG_GRAPH_OPCODE_STR_N:
    case ONIG_GRAPH_OPCODE_STR_MB2N1:
    case ONIG_GRAPH_OPCODE_STR_MB2N2:
    case ONIG_GRAPH_OPCODE_STR_MB2N3:
    case ONIG_GRAPH_OPCODE_STR_MB2N:
    case ONIG_GRAPH_OPCODE_STR_MB3N:
    case ONIG_GRAPH_OPCODE_STR_MBN:
    case ONIG_GRAPH_OPCODE_CCLASS:
    case ONIG_GRAPH_OPCODE_CCLASS_MB:
    case ONIG_GRAPH_OPCODE_CCLASS_MIX:
    case ONIG_GRAPH_OPCODE_CCLASS_NOT:
    case ONIG_GRAPH_OPCODE_CCLASS_MB_NOT:
    case ONIG_GRAPH_OPCODE_CCLASS_MIX_NOT:
    case ONIG_GRAPH_OPCODE_ANYCHAR:
    case ONIG_GRAPH_OPCODE_ANYCHAR_ML:
    case ONIG_GRAPH_OPCODE_WORD:
    case ONIG_GRAPH_OPCODE_WORD_ASCII:
    case ONIG_GRAPH_OPCODE_NO_WORD:
    case ONIG_GRAPH_OPCODE_NO_WORD_ASCII:
    case ONIG_GRAPH_OPCODE_WORD_BOUNDARY:
    case ONIG_GRAPH_OPCODE_NO_WORD_BOUNDARY:
    case ONIG_GRAPH_OPCODE_WORD_BEGIN:
    case ONIG_GRAPH_OPCODE_WORD_END:
    case ONIG_GRAPH_OPCODE_TEXT_SEGMENT_BOUNDARY:
    case ONIG_GRAPH_OPCODE_BEGIN_BUF:
    case ONIG_GRAPH_OPCODE_END_BUF:
    case ONIG_GRAPH_OPCODE_BEGIN_LINE:
    case ONIG_GRAPH_OPCODE_END_LINE:
    case ONIG_GRAPH_OPCODE_SEMI_END_BUF:
    case ONIG_GRAPH_OPCODE_CHECK_POSITION:
    case ONIG_GRAPH_OPCODE_MEM_START:
    case ONIG_GRAPH_OPCODE_MEM_START_PUSH:
    case ONIG_GRAPH_OPCODE_MEM_END:
    case ONIG_GRAPH_OPCODE_MEM_END_PUSH:
    case ONIG_GRAPH_OPCODE_MEM_END_REC:
    case ONIG_GRAPH_OPCODE_MEM_END_PUSH_REC:
    case ONIG_GRAPH_OPCODE_EMPTY_CHECK_START:
    case ONIG_GRAPH_OPCODE_POP:
    case ONIG_GRAPH_OPCODE_POP_TO_MARK:
    case ONIG_GRAPH_OPCODE_MARK:
    case ONIG_GRAPH_OPCODE_CUT_TO_MARK:
    case ONIG_GRAPH_OPCODE_SAVE_VAL:
    case ONIG_GRAPH_OPCODE_UPDATE_VAR:
    case ONIG_GRAPH_OPCODE_MOVE:
      emit_fallthrough(st, edges, i, n, pub, 0);
      break;

    case ONIG_GRAPH_OPCODE_STEP_BACK_START:
      /* remaining == 0: continue at from+1.  remaining != 0: push the
         trial position from+1 as a deferred alternative, then jump to
         from+addr.  Both transfer targets are conditional. */
      emit_fallthrough(st, edges, i, n, pub, 1);
      emit(st, edges, i, i + 1, ONIG_GRAPH_EDGE_COND_PUSH, 1, 1, n, pub);
      if (op->step_back_start.addr != 1)
        emit(st, edges, i, i + op->step_back_start.addr,
             ONIG_GRAPH_EDGE_COND_JUMP, 0, 1, n, pub);
      break;

    case ONIG_GRAPH_OPCODE_STEP_BACK_NEXT:
      /* Always continues at from+1; when more trial lengths remain it
         also pushes from itself as a deferred alternative. */
      emit_fallthrough(st, edges, i, n, pub, 0);
      emit(st, edges, i, i, ONIG_GRAPH_EDGE_ALT_PUSH, 1, 1, n, pub);
      break;

    case ONIG_GRAPH_OPCODE_ANYCHAR_STAR:
    case ONIG_GRAPH_OPCODE_ANYCHAR_ML_STAR:
      /* INC_OP then the loop pushes p (= from+1) as a deferred
         alternative and finally falls through to from+1. */
      emit_fallthrough(st, edges, i, n, pub, 0);
      emit(st, edges, i, i + 1, ONIG_GRAPH_EDGE_ALT_PUSH, 1, 1, n, pub);
      break;

    case ONIG_GRAPH_OPCODE_ANYCHAR_STAR_PEEK_NEXT:
    case ONIG_GRAPH_OPCODE_ANYCHAR_ML_STAR_PEEK_NEXT:
      emit_fallthrough(st, edges, i, n, pub, 0);
      emit(st, edges, i, i + 1, ONIG_GRAPH_EDGE_COND_PUSH, 1, 1, n, pub);
      break;

    case ONIG_GRAPH_OPCODE_JUMP:
      emit(st, edges, i, i + op->jump.addr, ONIG_GRAPH_EDGE_JUMP, 0, 0,
           n, pub);
      break;

    case ONIG_GRAPH_OPCODE_PUSH:
    case ONIG_GRAPH_OPCODE_PUSH_SUPER:
      emit_fallthrough(st, edges, i, n, pub, 0);
      emit(st, edges, i, i + op->push.addr, ONIG_GRAPH_EDGE_ALT_PUSH, 1, 1,
           n, pub);
      break;

#ifdef USE_OP_PUSH_OR_JUMP_EXACT
    case ONIG_GRAPH_OPCODE_PUSH_OR_JUMP_EXACT1:
      emit(st, edges, i, i + 1, ONIG_GRAPH_EDGE_COND_CONTINUE, 0, 1,
           n, pub);
      emit(st, edges, i, i + op->push_or_jump_exact1.addr,
           ONIG_GRAPH_EDGE_COND_JUMP, 0, 1, n, pub);
      emit(st, edges, i, i + op->push_or_jump_exact1.addr,
           ONIG_GRAPH_EDGE_COND_PUSH, 1, 1, n, pub);
      break;
#endif

    case ONIG_GRAPH_OPCODE_PUSH_IF_PEEK_NEXT:
      emit_fallthrough(st, edges, i, n, pub, 0);
      emit(st, edges, i, i + op->push_if_peek_next.addr,
           ONIG_GRAPH_EDGE_COND_PUSH, 1, 1, n, pub);
      break;

    case ONIG_GRAPH_OPCODE_REPEAT:
    case ONIG_GRAPH_OPCODE_REPEAT_NG:
      {
        int rec_idx = g->repeat_of_node[i];
        OnigGraphRepeat* rec;
        int exit_target = i + op->repeat.addr;

        if (rec_idx < 0) {
          edge_fail(st, ONIG_GRAPH_ERR_INCONSISTENT_REPEAT, i, pub,
                    "unpaired repeat entry");
          break;
        }
        rec = &g->repeats[rec_idx];
        if (pub == ONIG_GRAPH_OPCODE_REPEAT) {
          /* PUSH_REPEAT_INC, then INC_OP: the body is always the
             immediate continuation.  When lower == 0 an exit is also
             pushed as a deferred alternative. */
          emit(st, edges, i, i + 1, ONIG_GRAPH_EDGE_REPEAT_ENTER, 0, 0,
               n, pub);
          if (rec->lower == 0) {
            emit(st, edges, i, exit_target, ONIG_GRAPH_EDGE_REPEAT_SKIP,
                 1, 1, n, pub);
          }
        }
        else {
          if (rec->lower == 0) {
            /* Push the body as a deferred alternative, then exit. */
            emit(st, edges, i, i + 1, ONIG_GRAPH_EDGE_REPEAT_NG_SKIP,
                 1, 1, n, pub);
            emit(st, edges, i, exit_target, ONIG_GRAPH_EDGE_REPEAT_NG_EXIT,
                 0, 0, n, pub);
          }
          else {
            emit(st, edges, i, i + 1, ONIG_GRAPH_EDGE_REPEAT_NG_ENTER,
                 0, 0, n, pub);
          }
        }
      }
      break;

    case ONIG_GRAPH_OPCODE_REPEAT_INC:
    case ONIG_GRAPH_OPCODE_REPEAT_INC_NG:
      {
        int rec_idx = g->repeat_of_node[i];
        OnigGraphRepeat* rec;
        int body;
        int exit_target = i + 1;

        if (rec_idx < 0) {
          edge_fail(st, ONIG_GRAPH_ERR_INCONSISTENT_REPEAT, i, pub,
                    "unpaired repeat increment");
          break;
        }
        rec = &g->repeats[rec_idx];
        body = rec->body_index;

        /* Which path is immediate depends on the running iteration
           count, which this graph deliberately does not solve.  Both
           immediate candidates are therefore marked conditional. */
        if (pub == ONIG_GRAPH_OPCODE_REPEAT_INC) {
          /* n < upper: move on to the body. */
          emit(st, edges, i, body, ONIG_GRAPH_EDGE_REPEAT_BACK, 0, 1,
               n, pub);
          /* n >= upper: leave the loop.  With an unbounded upper this
             branch never fires, so it is not drawn. */
          if (rec->upper != ONIG_GRAPH_INFINITE_REPEAT) {
            emit(st, edges, i, exit_target, ONIG_GRAPH_EDGE_REPEAT_EXIT,
                 0, 1, n, pub);
          }
          if (rec->lower < rec->upper) {
            /* lower <= n < upper: the VM increments the instruction
               pointer and pushes the new position (the exit) as a
               deferred alternative, then jumps to the body.  So the
               exit is reachable through the backtrack stack while the
               body is the immediate path. */
            emit(st, edges, i, exit_target, ONIG_GRAPH_EDGE_REPEAT_EXIT,
                 1, 1, n, pub);
          }
        }
        else {
          /* n >= lower: the loop exit is the immediate path. */
          emit(st, edges, i, exit_target, ONIG_GRAPH_EDGE_REPEAT_NG_EXIT,
               0, 1, n, pub);
          if (rec->lower > 0) {
            /* n < lower: move on to the body immediately. */
            emit(st, edges, i, body, ONIG_GRAPH_EDGE_REPEAT_NG_BACK, 0, 1,
                 n, pub);
          }
          if (rec->lower < rec->upper) {
            /* lower <= n < upper: the body is pushed as a deferred
               alternative. */
            emit(st, edges, i, body, ONIG_GRAPH_EDGE_REPEAT_NG_BACK, 1, 1,
                 n, pub);
          }
        }
      }
      break;

    case ONIG_GRAPH_OPCODE_EMPTY_CHECK_END:
    case ONIG_GRAPH_OPCODE_EMPTY_CHECK_END_MEMST:
    case ONIG_GRAPH_OPCODE_EMPTY_CHECK_END_MEMST_PUSH:
      {
        int mem = op->empty_check_end.mem;
        if (mem < 0 || mem >= reg->num_empty_check) {
          edge_fail(st, ONIG_GRAPH_ERR_INVALID_MEMORY, i, pub,
                    "empty-check id out of range");
          break;
        }
        /* Not empty: continue at from+1.  Empty: skip the following
           instruction and continue at from+2. */
        emit_fallthrough(st, edges, i, n, pub, 1);
        if (i + 2 >= n) {
          edge_fail(st, ONIG_GRAPH_ERR_INVALID_TARGET, i, pub,
                    "empty-check skip target is outside the Operation array");
          break;
        }
        emit(st, edges, i, i + 2, ONIG_GRAPH_EDGE_EMPTY_CHECK_SKIP, 0, 1,
             n, pub);
      }
      break;

    case ONIG_GRAPH_OPCODE_CALL:
      {
        int target = op->call.addr;
        if (target < 0 || target >= n) {
          edge_fail(st, ONIG_GRAPH_ERR_INVALID_TARGET, i, pub,
                    "CALL target is outside the Operation array");
          break;
        }
        emit(st, edges, i, target, ONIG_GRAPH_EDGE_CALL, 0, 0, n, pub);
        if (i + 1 < n) {
          /* Summary continuation only; not a VM execution edge. */
          emit(st, edges, i, i + 1, ONIG_GRAPH_EDGE_RESUME_SUMMARY,
               0, 0, n, pub);
        }
      }
      break;

    default:
      /* Supported-set invariant broken: reject rather than invent an
         edge. */
      edge_fail(st, ONIG_GRAPH_ERR_UNSUPPORTED_OPCODE, i, pub,
                "opcode is not implemented by the graph builder");
      break;
    }
  }

  /* Validate MEM / empty-check operand ranges for the fallthrough ops
     above (done after the switch so a single helper covers all of
     them). */
  if (st->ok) {
    for (i = 0; i < n; i++) {
      int pub = pubcodes[i];
      Operation* op = &reg->ops[i];
      switch (pub) {
      case ONIG_GRAPH_OPCODE_MEM_START:
      case ONIG_GRAPH_OPCODE_MEM_START_PUSH:
        if (op->memory_start.num < 0 ||
            op->memory_start.num > reg->num_mem) {
          edge_fail(st, ONIG_GRAPH_ERR_INVALID_MEMORY, i, pub,
                    "capture id out of range");
        }
        break;
      case ONIG_GRAPH_OPCODE_MEM_END:
      case ONIG_GRAPH_OPCODE_MEM_END_PUSH:
      case ONIG_GRAPH_OPCODE_MEM_END_REC:
      case ONIG_GRAPH_OPCODE_MEM_END_PUSH_REC:
        if (op->memory_end.num < 0 || op->memory_end.num > reg->num_mem) {
          edge_fail(st, ONIG_GRAPH_ERR_INVALID_MEMORY, i, pub,
                    "capture id out of range");
        }
        break;
      default:
        break;
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/* Call table and reachability.                                       */
/* ------------------------------------------------------------------ */

static int
build_calls(const regex_t* reg, const int* pubcodes, OnigGraph* g,
            OnigGraphError* error)
{
  int i, n = (int )reg->ops_used;

  for (i = 0; i < n; i++) {
    if (pubcodes[i] != ONIG_GRAPH_OPCODE_CALL) continue;
    g->call_count++;
  }

  if (g->call_count == 0) return 1;

  g->calls = (OnigGraphCall* )calloc((size_t)g->call_count,
                                     sizeof(OnigGraphCall));
  g->call_of_node = (int* )malloc(sizeof(int) * (size_t)n);
  if (IS_NULL(g->calls) || IS_NULL(g->call_of_node)) {
    set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "call records");
    return 0;
  }
  for (i = 0; i < n; i++) g->call_of_node[i] = -1;

  g->call_count = 0;
  for (i = 0; i < n; i++) {
    if (pubcodes[i] != ONIG_GRAPH_OPCODE_CALL) continue;
    g->calls[g->call_count].call_index = i;
    g->calls[g->call_count].target = reg->ops[i].call.addr;
    g->calls[g->call_count].resume_index =
      (i + 1 < n ? i + 1 : -1);
    g->call_of_node[i] = g->call_count;
    g->call_count++;
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Structural reachability from the main entry.                       */
/* ------------------------------------------------------------------ */

static int
compute_reachability(OnigGraph* g)
{
  int i;
  int* stack;
  unsigned char* seen;
  int top = 0;
  int n = g->node_count;

  if (n == 0) return 1;
  stack = (int* )malloc(sizeof(int) * (size_t)n);
  seen = (unsigned char* )calloc((size_t)n, 1);
  if (IS_NULL(stack) || IS_NULL(seen)) {
    if (stack) free(stack);
    if (seen) free(seen);
    return 0;
  }

  stack[top++] = 0;
  seen[0] = 1;
  while (top > 0) {
    int v = stack[--top];
    int b = g->out_start[v];
    int e = g->out_start[v + 1];
    int j;
    for (j = b; j < e; j++) {
      int to = g->edges[j].to;
      if (! seen[to]) {
        seen[to] = 1;
        stack[top++] = to;
      }
    }
  }

  for (i = 0; i < n; i++) {
    if (seen[i]) g->nodes[i].flags |= ONIG_GRAPH_NODE_REACHABLE;
    else         g->nodes[i].flags |= ONIG_GRAPH_NODE_UNREACHABLE;
  }
  free(stack);
  free(seen);
  return 1;
}

/* ------------------------------------------------------------------ */
/* Call graph, SCCs.                                                  */
/* ------------------------------------------------------------------ */

static int
entry_position(const OnigGraph* g, int value)
{
  int lo = 0, hi = g->entry_count - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    if (g->entries[mid] == value) return mid;
    if (g->entries[mid] < value) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
}

typedef struct {
  int from_entry;
  int to_entry;
  int call_index;
} CallCandidate;

static int
candidate_compare(const void* pa, const void* pb)
{
  const CallCandidate* a = (const CallCandidate* )pa;
  const CallCandidate* b = (const CallCandidate* )pb;
  if (a->from_entry != b->from_entry)
    return (a->from_entry < b->from_entry ? -1 : 1);
  if (a->to_entry != b->to_entry)
    return (a->to_entry < b->to_entry ? -1 : 1);
  if (a->call_index != b->call_index)
    return (a->call_index < b->call_index ? -1 : 1);
  return 0;
}

static int
build_call_graph(OnigGraph* g, OnigGraphError* error)
{
  int i, j, n = g->node_count;
  int* visited = 0;
  int* stack = 0;
  CallCandidate* cand = 0;
  int cand_count = 0, cand_cap = 0;

  /* Entry set: main entry 0 plus every CALL target. */
  {
    unsigned char* is_entry = (unsigned char* )calloc((size_t)n, 1);
    if (IS_NULL(is_entry)) {
      set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "entry set");
      return 0;
    }
    is_entry[0] = 1;
    for (i = 0; i < g->call_count; i++) {
      int t = g->calls[i].target;
      if (t >= 0 && t < n) is_entry[t] = 1;
    }
    for (i = 0; i < n; i++) if (is_entry[i]) g->entry_count++;
    g->entries = (int* )malloc(sizeof(int) * (size_t)g->entry_count);
    if (IS_NULL(g->entries)) {
      free(is_entry);
      set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "entry list");
      return 0;
    }
    j = 0;
    for (i = 0; i < n; i++) if (is_entry[i]) g->entries[j++] = i;
    free(is_entry);
  }

  visited = (int* )malloc(sizeof(int) * (size_t)n);
  stack = (int* )malloc(sizeof(int) * (size_t)n);
  if (IS_NULL(visited) || IS_NULL(stack)) {
    if (visited) free(visited);
    if (stack) free(stack);
    set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "call walk");
    return 0;
  }

  for (i = 0; i < g->entry_count; i++) {
    int entry = g->entries[i];
    int top = 0;
    for (j = 0; j < n; j++) visited[j] = 0;
    stack[top++] = entry;
    visited[entry] = 1;
    while (top > 0) {
      int v = stack[--top];
      int b = g->out_start[v];
      int e = g->out_start[v + 1];
      int k;

      if (g->nodes[v].opcode == ONIG_GRAPH_OPCODE_CALL &&
          g->call_of_node[v] >= 0) {
        int target = g->calls[g->call_of_node[v]].target;
        if (cand_count >= cand_cap) {
          int cap = (cand_cap == 0 ? 32 : cand_cap * 2);
          CallCandidate* x = (CallCandidate* )realloc(
            cand, sizeof(CallCandidate) * (size_t)cap);
          if (IS_NULL(x)) {
            free(visited); free(stack); free(cand);
            set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "call edges");
            return 0;
          }
          cand = x; cand_cap = cap;
        }
        cand[cand_count].from_entry = entry;
        cand[cand_count].to_entry = target;
        cand[cand_count].call_index = v;
        cand_count++;
      }

      for (k = b; k < e; k++) {
        /* Do not enter a callee through the call graph here; the call
           graph has its own edge for that.  The resume-summary edge is
           an explicit structural continuation and is followed. */
        if (g->edges[k].type == ONIG_GRAPH_EDGE_CALL) continue;
        if (! visited[g->edges[k].to]) {
          visited[g->edges[k].to] = 1;
          stack[top++] = g->edges[k].to;
        }
      }
    }
  }

  if (cand_count == 0) {
    free(visited); free(stack); free(cand);
    /* No call edges; one singleton SCC per entry. */
    return 1;
  }

  qsort(cand, (size_t)cand_count, sizeof(CallCandidate), candidate_compare);

  g->call_edges = (OnigGraphCallEdge* )malloc(
    sizeof(OnigGraphCallEdge) * (size_t)cand_count);
  if (IS_NULL(g->call_edges)) {
    free(visited); free(stack); free(cand);
    set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "call graph");
    return 0;
  }
  g->call_edge_count = 0;
  for (i = 0; i < cand_count; ) {
    int from = cand[i].from_entry;
    int to = cand[i].to_entry;
    int first = cand[i].call_index;
    int count = 0;
    j = i;
    while (j < cand_count && cand[j].from_entry == from &&
           cand[j].to_entry == to) {
      count++;
      j++;
    }
    g->call_edges[g->call_edge_count].from_entry = from;
    g->call_edges[g->call_edge_count].to_entry = to;
    g->call_edges[g->call_edge_count].first_call_index = first;
    g->call_edges[g->call_edge_count].call_count = count;
    g->call_edge_count++;
    i = j;
  }

  free(visited); free(stack); free(cand);
  return 1;
}

static int
build_sccs(OnigGraph* g, OnigGraphError* error)
{
  int n = g->entry_count;
  int m = g->call_edge_count;
  int i, j;
  int* out_start = 0;
  int* out_to = 0;
  int* rev_start = 0;
  int* rev_to = 0;
  int* order = 0;
  int* comp = 0;
  int* stack = 0;

  g->entry_scc = (int* )malloc(sizeof(int) * (size_t)n);
  if (IS_NULL(g->entry_scc)) {
    set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "scc map");
    return 0;
  }
  for (i = 0; i < n; i++) g->entry_scc[i] = -1;

  if (m == 0) {
    g->scc_count = n;
    g->sccs = (OnigGraphScc* )calloc((size_t)n, sizeof(OnigGraphScc));
    g->scc_members = (int* )malloc(sizeof(int) * (size_t)n);
    if (IS_NULL(g->sccs) || IS_NULL(g->scc_members)) {
      set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "scc records");
      return 0;
    }
    for (i = 0; i < n; i++) {
      g->sccs[i].id = g->entries[i];
      g->sccs[i].recursive = 0;
      g->sccs[i].member_count = 1;
      g->scc_members[i] = g->entries[i];
      g->sccs[i].members = &g->scc_members[i];
      g->entry_scc[i] = i;
    }
    return 1;
  }

  out_start = (int* )calloc((size_t)n + 1, sizeof(int));
  rev_start = (int* )calloc((size_t)n + 1, sizeof(int));
  out_to = (int* )malloc(sizeof(int) * (size_t)m);
  rev_to = (int* )malloc(sizeof(int) * (size_t)m);
  order = (int* )malloc(sizeof(int) * (size_t)n);
  comp = (int* )malloc(sizeof(int) * (size_t)n);
  stack = (int* )malloc(sizeof(int) * (size_t)n);
  if (IS_NULL(out_start) || IS_NULL(rev_start) || IS_NULL(out_to) ||
      IS_NULL(rev_to) || IS_NULL(order) || IS_NULL(comp) || IS_NULL(stack)) {
    if (out_start) free(out_start);
    if (rev_start) free(rev_start);
    if (out_to) free(out_to);
    if (rev_to) free(rev_to);
    if (order) free(order);
    if (comp) free(comp);
    if (stack) free(stack);
    set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "scc arrays");
    return 0;
  }

  /* Build adjacency and reverse adjacency (entry positions). */
  for (i = 0; i < m; i++) {
    int f = entry_position(g, g->call_edges[i].from_entry);
    int t = entry_position(g, g->call_edges[i].to_entry);
    if (f < 0 || t < 0) {
      /* Should not happen: both are in the entry set. */
      free(out_start); free(rev_start); free(out_to); free(rev_to);
      free(order); free(comp); free(stack);
      set_graph_error(error, ONIG_GRAPH_ERR_INVALID_TARGET, -1, -1,
                      "call graph edge refers to a non-entry");
      return 0;
    }
    out_start[f + 1]++;
    rev_start[t + 1]++;
  }
  for (i = 0; i < n; i++) {
    out_start[i + 1] += out_start[i];
    rev_start[i + 1] += rev_start[i];
  }
  {
    int* ofill = (int* )malloc(sizeof(int) * (size_t)n);
    int* rfill = (int* )malloc(sizeof(int) * (size_t)n);
    if (IS_NULL(ofill) || IS_NULL(rfill)) {
      if (ofill) free(ofill);
      if (rfill) free(rfill);
      free(out_start); free(rev_start); free(out_to); free(rev_to);
      free(order); free(comp); free(stack);
      set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "scc fill");
      return 0;
    }
    for (i = 0; i < n; i++) { ofill[i] = out_start[i]; rfill[i] = rev_start[i]; }
    for (i = 0; i < m; i++) {
      int f = entry_position(g, g->call_edges[i].from_entry);
      int t = entry_position(g, g->call_edges[i].to_entry);
      out_to[ofill[f]++] = t;
      rev_to[rfill[t]++] = f;
    }
    free(ofill); free(rfill);
  }

  /* First pass: iterative DFS post-order on the call graph. */
  {
    int* iter = (int* )calloc((size_t)n, sizeof(int));
    unsigned char* seen = (unsigned char* )calloc((size_t)n, 1);
    int oi = 0;
    if (IS_NULL(iter) || IS_NULL(seen)) {
      if (iter) free(iter);
      if (seen) free(seen);
      free(out_start); free(rev_start); free(out_to); free(rev_to);
      free(order); free(comp); free(stack);
      set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "scc dfs");
      return 0;
    }
    for (i = 0; i < n; i++) {
      int top;
      if (seen[i]) continue;
      seen[i] = 1;
      top = 0;
      stack[top++] = i;
      while (top > 0) {
        int v = stack[top - 1];
        if (iter[v] < out_start[v + 1] - out_start[v]) {
          int to = out_to[out_start[v] + iter[v]];
          iter[v]++;
          if (! seen[to]) {
            seen[to] = 1;
            stack[top++] = to;
          }
        }
        else {
          top--;
          order[oi++] = v;
        }
      }
    }
    free(iter); free(seen);
  }

  /* Second pass: process vertices in reverse post-order on the
     reverse graph.  Each DFS tree is one SCC. */
  {
    int* comp_id = (int* )calloc((size_t)n, sizeof(int));
    int nc = 0;
    if (IS_NULL(comp_id)) {
      free(out_start); free(rev_start); free(out_to); free(rev_to);
      free(order); free(comp); free(stack);
      set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "scc comp");
      return 0;
    }
    for (i = 0; i < n; i++) comp_id[i] = -1;
    for (j = n - 1; j >= 0; j--) {
      int root = order[j];
      int top;
      if (comp_id[root] != -1) continue;
      comp_id[root] = nc;
      top = 0;
      stack[top++] = root;
      while (top > 0) {
        int v = stack[--top];
        int b = rev_start[v];
        int e = rev_start[v + 1];
        int k;
        for (k = b; k < e; k++) {
          int to = rev_to[k];
          if (comp_id[to] == -1) {
            comp_id[to] = nc;
            stack[top++] = to;
          }
        }
      }
      nc++;
    }

    g->scc_count = nc;
    g->sccs = (OnigGraphScc* )calloc((size_t)nc, sizeof(OnigGraphScc));
    g->scc_members = (int* )malloc(sizeof(int) * (size_t)n);
    if (IS_NULL(g->sccs) || IS_NULL(g->scc_members)) {
      free(comp_id);
      free(out_start); free(rev_start); free(out_to); free(rev_to);
      free(order); free(comp); free(stack);
      set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "scc result");
      return 0;
    }
    {
      int* slot = (int* )calloc((size_t)nc, sizeof(int));
      if (IS_NULL(slot)) {
        free(comp_id);
        free(out_start); free(rev_start); free(out_to); free(rev_to);
        free(order); free(comp); free(stack);
        set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "scc fill2");
        return 0;
      }
      for (i = 0; i < n; i++) {
        g->sccs[comp_id[i]].member_count++;
      }
      /* Reserve slices then fill. */
      {
        int off = 0;
        for (i = 0; i < nc; i++) {
          slot[i] = off;
          g->sccs[i].members = &g->scc_members[off];
          off += g->sccs[i].member_count;
          g->sccs[i].member_count = 0;
        }
      }
      for (i = 0; i < n; i++) {
        int c = comp_id[i];
        g->scc_members[slot[c] + g->sccs[c].member_count] = g->entries[i];
        g->sccs[c].member_count++;
      }

      /* Sort members ascending in the writable backing store and set
         the stable id / recursion flag. */
      for (i = 0; i < nc; i++) {
        int k;
        int* m = &g->scc_members[slot[i]];
        g->sccs[i].id = 0x7fffffff;
        for (k = 0; k < g->sccs[i].member_count; k++) {
          if (m[k] < g->sccs[i].id) g->sccs[i].id = m[k];
        }
        for (k = 1; k < g->sccs[i].member_count; k++) {
          int v = m[k];
          int q = k - 1;
          while (q >= 0 && m[q] > v) {
            m[q + 1] = m[q];
            q--;
          }
          m[q + 1] = v;
        }
        g->sccs[i].recursive = (g->sccs[i].member_count > 1 ? 1 : 0);
      }
      free(slot);
    }
    /* Self loops make a singleton recursive. */
    for (i = 0; i < m; i++) {
      if (g->call_edges[i].from_entry == g->call_edges[i].to_entry) {
        int pos = entry_position(g, g->call_edges[i].from_entry);
        if (pos >= 0) g->sccs[comp_id[pos]].recursive = 1;
      }
    }
    /* Sort SCCs by stable id; keep the entry->scc mapping consistent. */
    if (nc > 1) {
      int* order2 = (int* )malloc(sizeof(int) * (size_t)nc);
      OnigGraphScc* sorted =
        (OnigGraphScc* )malloc(sizeof(OnigGraphScc) * (size_t)nc);
      int* newpos = (int* )malloc(sizeof(int) * (size_t)nc);
      if (IS_NULL(order2) || IS_NULL(sorted) || IS_NULL(newpos)) {
        if (order2) free(order2);
        if (sorted) free(sorted);
        if (newpos) free(newpos);
        free(comp_id);
        free(out_start); free(rev_start); free(out_to); free(rev_to);
        free(order); free(comp); free(stack);
        set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "scc sort");
        return 0;
      }
      for (i = 0; i < nc; i++) order2[i] = i;
      for (i = 0; i < nc; i++) {
        int q;
        for (q = i + 1; q < nc; q++) {
          if (g->sccs[order2[q]].id < g->sccs[order2[i]].id) {
            int t = order2[i]; order2[i] = order2[q]; order2[q] = t;
          }
        }
      }
      for (i = 0; i < nc; i++) {
        sorted[i] = g->sccs[order2[i]];
        newpos[order2[i]] = i;
      }
      /* Remap entry_scc through comp_id -> new position. */
      for (i = 0; i < n; i++) {
        g->entry_scc[i] = newpos[comp_id[i]];
      }
      free(g->sccs);
      g->sccs = sorted;
      free(order2);
      free(newpos);
    }
    else {
      for (i = 0; i < n; i++) g->entry_scc[i] = 0;
    }

    free(comp_id);
  }

  free(out_start); free(rev_start); free(out_to); free(rev_to);
  free(order); free(comp); free(stack);
  return 1;
}

/* ------------------------------------------------------------------ */
/* Public creation / free.                                            */
/* ------------------------------------------------------------------ */

OnigGraph*
onig_graph_new(const OnigRegexType* regex, OnigGraphError* error)
{
  const regex_t* reg = (const regex_t* )regex;
  OnigGraph* g;
  int i, n;
  int* pubcodes = 0;
  EdgeBuf edges;
  BuildStatus st;

  if (error != 0) {
    error->reason = ONIG_GRAPH_OK;
    error->op_index = -1;
    error->opcode = -1;
    error->message[0] = '\0';
  }

  if (IS_NULL(reg)) {
    set_graph_error(error, ONIG_GRAPH_ERR_INVALID_ARGUMENT, -1, -1,
                    "regex is NULL");
    return 0;
  }
  if (reg->ops_used == 0 || IS_NULL(reg->ops)) {
    set_graph_error(error, ONIG_GRAPH_ERR_EMPTY_PROGRAM, -1, -1,
                    "compiled regex has no operations");
    return 0;
  }
#ifdef USE_DIRECT_THREADED_CODE
  if (IS_NULL(reg->ocs)) {
    set_graph_error(error, ONIG_GRAPH_ERR_DECODE, -1, -1,
                    "direct-threaded build has no opcode table");
    return 0;
  }
#endif
  if (reg->ops_used > (unsigned int)0x7fffffff) {
    set_graph_error(error, ONIG_GRAPH_ERR_DECODE, -1, -1,
                    "operation count is too large");
    return 0;
  }

  g = (OnigGraph* )calloc(1, sizeof(OnigGraph));
  if (IS_NULL(g)) {
    set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "graph");
    return 0;
  }
  n = (int )reg->ops_used;
  g->node_count = n;

  g->nodes = (OnigGraphNode* )calloc((size_t)n, sizeof(OnigGraphNode));
  pubcodes = (int* )malloc(sizeof(int) * (size_t)n);
  if (IS_NULL(g->nodes) || IS_NULL(pubcodes)) {
    if (pubcodes) free(pubcodes);
    set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "node table");
    onig_graph_free(g);
    return 0;
  }

  /* Decode first: reject anything that is not in the supported set
     before any edge is invented. */
  for (i = 0; i < n; i++) {
    enum OpCode oc = decode_opcode(reg, i);
    int pub = map_opcode(oc);
    if (pub < 0) {
      set_graph_error(error, ONIG_GRAPH_ERR_UNKNOWN_OPCODE, i, -1,
                      "opcode is not part of Oniguruma 6.9.5");
      free(pubcodes);
      onig_graph_free(g);
      return 0;
    }
    if (! opcode_supported(pub)) {
      set_graph_error(error, ONIG_GRAPH_ERR_UNSUPPORTED_OPCODE, i, pub,
                      "opcode is outside the documented supported subset");
      free(pubcodes);
      onig_graph_free(g);
      return 0;
    }
    pubcodes[i] = pub;
    g->nodes[i].index = i;
    g->nodes[i].opcode = pub;
    g->nodes[i].opcode_name = GraphOpNames[pub].name;
  }

  if (! build_repeats(reg, pubcodes, g, error)) {
    free(pubcodes);
    onig_graph_free(g);
    return 0;
  }
  if (! build_calls(reg, pubcodes, g, error)) {
    free(pubcodes);
    onig_graph_free(g);
    return 0;
  }

  edges.e = 0; edges.n = 0; edges.cap = 0;
  st.ok = 1;
  st.reason = ONIG_GRAPH_OK;
  st.op_index = -1;
  st.opcode = -1;
  st.detail = 0;
  build_edges(reg, pubcodes, g, &edges, &st);
  if (! st.ok) {
    edge_buf_free(&edges);
    free(pubcodes);
    set_graph_error(error, st.reason, st.op_index, st.opcode,
                    (st.detail != 0 ? st.detail : "graph build failed"));
    onig_graph_free(g);
    return 0;
  }

  /* Sort and deduplicate edges; establish per-node slices. */
  if (edges.n > 0) {
    int w = 0;
    qsort(edges.e, (size_t)edges.n, sizeof(OnigGraphEdge), edge_compare);
    for (i = 0; i < edges.n; i++) {
      if (w > 0 && edge_compare(&edges.e[w - 1], &edges.e[i]) == 0) continue;
      edges.e[w++] = edges.e[i];
    }
    edges.n = w;
  }
  g->edges = edges.e;
  g->edge_count = edges.n;

  g->out_start = (int* )calloc((size_t)n + 1, sizeof(int));
  if (IS_NULL(g->out_start)) {
    free(pubcodes);
    set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "edge slices");
    onig_graph_free(g);
    return 0;
  }
  for (i = 0; i < g->edge_count; i++) g->out_start[g->edges[i].from + 1]++;
  for (i = 0; i < n; i++) g->out_start[i + 1] += g->out_start[i];
  for (i = 0; i < n; i++)
    g->nodes[i].out_edge_count = g->out_start[i + 1] - g->out_start[i];

  /* Node flags and literal copies. */
  for (i = 0; i < n; i++) {
    enum OpCode oc = decode_opcode(reg, i);
    size_t len;
    const UChar* src;

    switch (pubcodes[i]) {
    case ONIG_GRAPH_OPCODE_END:
      g->nodes[i].flags |= ONIG_GRAPH_NODE_SUCCESS_EXIT; break;
    case ONIG_GRAPH_OPCODE_FAIL:
      g->nodes[i].flags |= ONIG_GRAPH_NODE_FAIL_EXIT; break;
    case ONIG_GRAPH_OPCODE_FINISH:
      g->nodes[i].flags |= ONIG_GRAPH_NODE_FINISH; break;
    case ONIG_GRAPH_OPCODE_RETURN:
      g->nodes[i].flags |= ONIG_GRAPH_NODE_DYNAMIC_RETURN; break;
    case ONIG_GRAPH_OPCODE_CALL:
      g->nodes[i].flags |= ONIG_GRAPH_NODE_CALL; break;
    case ONIG_GRAPH_OPCODE_REPEAT:
    case ONIG_GRAPH_OPCODE_REPEAT_NG:
      g->nodes[i].flags |= ONIG_GRAPH_NODE_REPEAT_ENTRY; break;
    case ONIG_GRAPH_OPCODE_REPEAT_INC:
    case ONIG_GRAPH_OPCODE_REPEAT_INC_NG:
      g->nodes[i].flags |= ONIG_GRAPH_NODE_REPEAT_INC; break;
    default:
      break;
    }

    if (! literal_bytes(oc, &reg->ops[i], &len, &src)) {
      free(pubcodes);
      set_graph_error(error, ONIG_GRAPH_ERR_DECODE, i, pubcodes[i],
                      "literal payload has an invalid length or pointer");
      onig_graph_free(g);
      return 0;
    }
    if (len > 0) {
      unsigned char* copy = (unsigned char* )malloc(len);
      if (IS_NULL(copy)) {
        free(pubcodes);
        set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, i, pubcodes[i],
                        "literal copy");
        onig_graph_free(g);
        return 0;
      }
      memcpy(copy, src, len);
      g->nodes[i].literal_len = (int )len;
      g->nodes[i].literal = copy;
    }
  }

  free(pubcodes);

  if (! compute_reachability(g)) {
    set_graph_error(error, ONIG_GRAPH_ERR_MEMORY, -1, -1, "reachability");
    onig_graph_free(g);
    return 0;
  }

  if (! build_call_graph(g, error)) {
    onig_graph_free(g);
    return 0;
  }
  for (i = 0; i < g->entry_count; i++) {
    g->nodes[g->entries[i]].flags |= ONIG_GRAPH_NODE_ENTRY;
  }

  if (! build_sccs(g, error)) {
    onig_graph_free(g);
    return 0;
  }

  return g;
}

void
onig_graph_free(OnigGraph* graph)
{
  int i;

  if (IS_NULL(graph)) return;
  if (IS_NOT_NULL(graph->nodes)) {
    for (i = 0; i < graph->node_count; i++) {
      if (IS_NOT_NULL(graph->nodes[i].literal))
        free((void* )graph->nodes[i].literal);
    }
    free(graph->nodes);
  }
  if (IS_NOT_NULL(graph->edges)) free(graph->edges);
  if (IS_NOT_NULL(graph->out_start)) free(graph->out_start);
  if (IS_NOT_NULL(graph->calls)) free(graph->calls);
  if (IS_NOT_NULL(graph->call_of_node)) free(graph->call_of_node);
  if (IS_NOT_NULL(graph->repeats)) free(graph->repeats);
  if (IS_NOT_NULL(graph->repeat_of_node)) free(graph->repeat_of_node);
  if (IS_NOT_NULL(graph->entries)) free(graph->entries);
  if (IS_NOT_NULL(graph->call_edges)) free(graph->call_edges);
  if (IS_NOT_NULL(graph->sccs)) free(graph->sccs);
  if (IS_NOT_NULL(graph->scc_members)) free(graph->scc_members);
  if (IS_NOT_NULL(graph->entry_scc)) free(graph->entry_scc);
  free(graph);
}

/* ------------------------------------------------------------------ */
/* Accessors.                                                         */
/* ------------------------------------------------------------------ */

int
onig_graph_node_count(const OnigGraph* graph)
{
  return (graph == 0 ? 0 : graph->node_count);
}

const OnigGraphNode*
onig_graph_node(const OnigGraph* graph, int index)
{
  if (graph == 0 || index < 0 || index >= graph->node_count) return 0;
  return &graph->nodes[index];
}

int
onig_graph_edge_count(const OnigGraph* graph)
{
  return (graph == 0 ? 0 : graph->edge_count);
}

const OnigGraphEdge*
onig_graph_edge(const OnigGraph* graph, int index)
{
  if (graph == 0 || index < 0 || index >= graph->edge_count) return 0;
  return &graph->edges[index];
}

int
onig_graph_out_edge_count(const OnigGraph* graph, int node_index)
{
  if (graph == 0 || node_index < 0 || node_index >= graph->node_count) return 0;
  return graph->out_start[node_index + 1] - graph->out_start[node_index];
}

const OnigGraphEdge*
onig_graph_out_edge(const OnigGraph* graph, int node_index, int index)
{
  int b;
  if (graph == 0 || node_index < 0 || node_index >= graph->node_count)
    return 0;
  if (index < 0 ||
      index >= graph->out_start[node_index + 1] - graph->out_start[node_index])
    return 0;
  b = graph->out_start[node_index];
  return &graph->edges[b + index];
}

int
onig_graph_call_count(const OnigGraph* graph)
{
  return (graph == 0 ? 0 : graph->call_count);
}

const OnigGraphCall*
onig_graph_call(const OnigGraph* graph, int index)
{
  if (graph == 0 || index < 0 || index >= graph->call_count) return 0;
  return &graph->calls[index];
}

const OnigGraphCall*
onig_graph_call_at(const OnigGraph* graph, int node_index)
{
  if (graph == 0 || node_index < 0 || node_index >= graph->node_count)
    return 0;
  if (graph->call_of_node == 0) return 0;
  if (graph->call_of_node[node_index] < 0) return 0;
  return &graph->calls[graph->call_of_node[node_index]];
}

int
onig_graph_repeat_count(const OnigGraph* graph)
{
  return (graph == 0 ? 0 : graph->repeat_count);
}

const OnigGraphRepeat*
onig_graph_repeat(const OnigGraph* graph, int index)
{
  if (graph == 0 || index < 0 || index >= graph->repeat_count) return 0;
  return &graph->repeats[index];
}

const OnigGraphRepeat*
onig_graph_repeat_at(const OnigGraph* graph, int node_index)
{
  if (graph == 0 || node_index < 0 || node_index >= graph->node_count)
    return 0;
  if (graph->repeat_of_node == 0) return 0;
  if (graph->repeat_of_node[node_index] < 0) return 0;
  return &graph->repeats[graph->repeat_of_node[node_index]];
}

int
onig_graph_entry_count(const OnigGraph* graph)
{
  return (graph == 0 ? 0 : graph->entry_count);
}

int
onig_graph_entry(const OnigGraph* graph, int index)
{
  if (graph == 0 || index < 0 || index >= graph->entry_count) return -1;
  return graph->entries[index];
}

int
onig_graph_is_entry(const OnigGraph* graph, int node_index)
{
  if (graph == 0 || node_index < 0 || node_index >= graph->node_count)
    return 0;
  return (graph->nodes[node_index].flags & ONIG_GRAPH_NODE_ENTRY) != 0;
}

int
onig_graph_call_edge_count(const OnigGraph* graph)
{
  return (graph == 0 ? 0 : graph->call_edge_count);
}

const OnigGraphCallEdge*
onig_graph_call_edge(const OnigGraph* graph, int index)
{
  if (graph == 0 || index < 0 || index >= graph->call_edge_count) return 0;
  return &graph->call_edges[index];
}

int
onig_graph_scc_count(const OnigGraph* graph)
{
  return (graph == 0 ? 0 : graph->scc_count);
}

const OnigGraphScc*
onig_graph_scc(const OnigGraph* graph, int index)
{
  if (graph == 0 || index < 0 || index >= graph->scc_count) return 0;
  return &graph->sccs[index];
}

const OnigGraphScc*
onig_graph_scc_for_entry(const OnigGraph* graph, int entry_index)
{
  int pos;
  if (graph == 0 || graph->entry_scc == 0) return 0;
  pos = entry_position(graph, entry_index);
  if (pos < 0) return 0;
  if (graph->entry_scc[pos] < 0) return 0;
  return &graph->sccs[graph->entry_scc[pos]];
}

int
onig_graph_node_reachable(const OnigGraph* graph, int node_index)
{
  if (graph == 0 || node_index < 0 || node_index >= graph->node_count)
    return 0;
  return (graph->nodes[node_index].flags & ONIG_GRAPH_NODE_REACHABLE) != 0;
}
