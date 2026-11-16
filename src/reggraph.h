/**********************************************************************
  reggraph.h - read-only compiled-regex control-flow / call graph view
  for Oniguruma 6.9.5
**********************************************************************/
#ifndef REGGRAPH_H
#define REGGRAPH_H

#include "oniguruma.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OnigGraph is a read-only, self-contained structural view of the
 * Operation array produced by onig_new()/onig_compile().
 *
 * It describes static instruction relationships only.  It does not
 * decide whether any text matches, does not simulate the matching VM,
 * and does not solve backtracking, repetition counts or capture state.
 *
 * The graph owns every array it exposes.  It copies no pointers into
 * the regex_t, its Operation array, its string pool or its
 * repeat_range.  A graph remains valid after onig_free() of the regex
 * it was built from.
 */

#define ONIG_GRAPH_API_VERSION 1

/* Upper bound used for unbounded repetition ranges ({n,}). */
#define ONIG_GRAPH_INFINITE_REPEAT 0x7fffffff

typedef struct OnigGraph OnigGraph;

/* ------------------------------------------------------------------ */
/* Opcodes (fixed numbering for Oniguruma 6.9.5).                      */
/*                                                                     */
/* The numbering follows the internal enum OpCode order of the fixed   */
/* source release.  Entries guarded by USE_CALL, USE_CALLOUT or        */
/* USE_OP_PUSH_OR_JUMP_EXACT are still part of this public numbering;  */
/* a build without one of those macros simply never produces that      */
/* opcode.  The docs list every entry and its guard.                   */
/* ------------------------------------------------------------------ */
typedef enum {
  ONIG_GRAPH_OPCODE_FINISH = 0,
  ONIG_GRAPH_OPCODE_END = 1,
  ONIG_GRAPH_OPCODE_STR_1 = 2,
  ONIG_GRAPH_OPCODE_STR_2 = 3,
  ONIG_GRAPH_OPCODE_STR_3 = 4,
  ONIG_GRAPH_OPCODE_STR_4 = 5,
  ONIG_GRAPH_OPCODE_STR_5 = 6,
  ONIG_GRAPH_OPCODE_STR_N = 7,
  ONIG_GRAPH_OPCODE_STR_MB2N1 = 8,
  ONIG_GRAPH_OPCODE_STR_MB2N2 = 9,
  ONIG_GRAPH_OPCODE_STR_MB2N3 = 10,
  ONIG_GRAPH_OPCODE_STR_MB2N = 11,
  ONIG_GRAPH_OPCODE_STR_MB3N = 12,
  ONIG_GRAPH_OPCODE_STR_MBN = 13,
  ONIG_GRAPH_OPCODE_CCLASS = 14,
  ONIG_GRAPH_OPCODE_CCLASS_MB = 15,
  ONIG_GRAPH_OPCODE_CCLASS_MIX = 16,
  ONIG_GRAPH_OPCODE_CCLASS_NOT = 17,
  ONIG_GRAPH_OPCODE_CCLASS_MB_NOT = 18,
  ONIG_GRAPH_OPCODE_CCLASS_MIX_NOT = 19,
  ONIG_GRAPH_OPCODE_ANYCHAR = 20,
  ONIG_GRAPH_OPCODE_ANYCHAR_ML = 21,
  ONIG_GRAPH_OPCODE_ANYCHAR_STAR = 22,
  ONIG_GRAPH_OPCODE_ANYCHAR_ML_STAR = 23,
  ONIG_GRAPH_OPCODE_ANYCHAR_STAR_PEEK_NEXT = 24,
  ONIG_GRAPH_OPCODE_ANYCHAR_ML_STAR_PEEK_NEXT = 25,
  ONIG_GRAPH_OPCODE_WORD = 26,
  ONIG_GRAPH_OPCODE_WORD_ASCII = 27,
  ONIG_GRAPH_OPCODE_NO_WORD = 28,
  ONIG_GRAPH_OPCODE_NO_WORD_ASCII = 29,
  ONIG_GRAPH_OPCODE_WORD_BOUNDARY = 30,
  ONIG_GRAPH_OPCODE_NO_WORD_BOUNDARY = 31,
  ONIG_GRAPH_OPCODE_WORD_BEGIN = 32,
  ONIG_GRAPH_OPCODE_WORD_END = 33,
  ONIG_GRAPH_OPCODE_TEXT_SEGMENT_BOUNDARY = 34,
  ONIG_GRAPH_OPCODE_BEGIN_BUF = 35,
  ONIG_GRAPH_OPCODE_END_BUF = 36,
  ONIG_GRAPH_OPCODE_BEGIN_LINE = 37,
  ONIG_GRAPH_OPCODE_END_LINE = 38,
  ONIG_GRAPH_OPCODE_SEMI_END_BUF = 39,
  ONIG_GRAPH_OPCODE_CHECK_POSITION = 40,
  ONIG_GRAPH_OPCODE_BACKREF1 = 41,
  ONIG_GRAPH_OPCODE_BACKREF2 = 42,
  ONIG_GRAPH_OPCODE_BACKREF_N = 43,
  ONIG_GRAPH_OPCODE_BACKREF_N_IC = 44,
  ONIG_GRAPH_OPCODE_BACKREF_MULTI = 45,
  ONIG_GRAPH_OPCODE_BACKREF_MULTI_IC = 46,
  ONIG_GRAPH_OPCODE_BACKREF_WITH_LEVEL = 47,
  ONIG_GRAPH_OPCODE_BACKREF_WITH_LEVEL_IC = 48,
  ONIG_GRAPH_OPCODE_BACKREF_CHECK = 49,
  ONIG_GRAPH_OPCODE_BACKREF_CHECK_WITH_LEVEL = 50,
  ONIG_GRAPH_OPCODE_MEM_START = 51,
  ONIG_GRAPH_OPCODE_MEM_START_PUSH = 52,
  ONIG_GRAPH_OPCODE_MEM_END_PUSH = 53,
  ONIG_GRAPH_OPCODE_MEM_END_PUSH_REC = 54,   /* USE_CALL */
  ONIG_GRAPH_OPCODE_MEM_END = 55,
  ONIG_GRAPH_OPCODE_MEM_END_REC = 56,        /* USE_CALL */
  ONIG_GRAPH_OPCODE_FAIL = 57,
  ONIG_GRAPH_OPCODE_JUMP = 58,
  ONIG_GRAPH_OPCODE_PUSH = 59,
  ONIG_GRAPH_OPCODE_PUSH_SUPER = 60,
  ONIG_GRAPH_OPCODE_POP = 61,
  ONIG_GRAPH_OPCODE_POP_TO_MARK = 62,
  ONIG_GRAPH_OPCODE_PUSH_OR_JUMP_EXACT1 = 63, /* USE_OP_PUSH_OR_JUMP_EXACT */
  ONIG_GRAPH_OPCODE_PUSH_IF_PEEK_NEXT = 64,
  ONIG_GRAPH_OPCODE_REPEAT = 65,
  ONIG_GRAPH_OPCODE_REPEAT_NG = 66,
  ONIG_GRAPH_OPCODE_REPEAT_INC = 67,
  ONIG_GRAPH_OPCODE_REPEAT_INC_NG = 68,
  ONIG_GRAPH_OPCODE_EMPTY_CHECK_START = 69,
  ONIG_GRAPH_OPCODE_EMPTY_CHECK_END = 70,
  ONIG_GRAPH_OPCODE_EMPTY_CHECK_END_MEMST = 71,
  ONIG_GRAPH_OPCODE_EMPTY_CHECK_END_MEMST_PUSH = 72, /* USE_CALL */
  ONIG_GRAPH_OPCODE_MOVE = 73,
  ONIG_GRAPH_OPCODE_STEP_BACK_START = 74,
  ONIG_GRAPH_OPCODE_STEP_BACK_NEXT = 75,
  ONIG_GRAPH_OPCODE_CUT_TO_MARK = 76,
  ONIG_GRAPH_OPCODE_MARK = 77,
  ONIG_GRAPH_OPCODE_SAVE_VAL = 78,
  ONIG_GRAPH_OPCODE_UPDATE_VAR = 79,
  ONIG_GRAPH_OPCODE_CALL = 80,               /* USE_CALL */
  ONIG_GRAPH_OPCODE_RETURN = 81,             /* USE_CALL */
  ONIG_GRAPH_OPCODE_CALLOUT_CONTENTS = 82,   /* USE_CALLOUT */
  ONIG_GRAPH_OPCODE_CALLOUT_NAME = 83,       /* USE_CALLOUT */
  ONIG_GRAPH_OPCODE_COUNT = 84
} OnigGraphOpcode;

/* ------------------------------------------------------------------ */
/* Typed edges.                                                        */
/*                                                                     */
/* Every edge is a static structural relationship.  Deferred           */
/* backtracking alternatives (the addresses pushed onto the VM stack)  */
/* use their own edge types and are never reported as fallthrough or   */
/* as an immediate branch.                                             */
/* ------------------------------------------------------------------ */
typedef enum {
  /* Ordinary sequential success successor.  Consumer, assertion,
     capture and bookkeeping instructions use this for their single
     success continuation. */
  ONIG_GRAPH_EDGE_FALLTHROUGH = 0,

  /* Unconditional relocation of the instruction pointer (OP_JUMP). */
  ONIG_GRAPH_EDGE_JUMP = 1,

  /* Deferred alternative address pushed on the backtrack stack by
     OP_PUSH / OP_PUSH_SUPER.  Execution continues at from+1. */
  ONIG_GRAPH_EDGE_ALT_PUSH = 2,

  /* OP_PUSH_OR_JUMP_EXACT1 / OP_PUSH_IF_PEEK_NEXT: immediate
     continuation (from+1) when the peeked condition holds. */
  ONIG_GRAPH_EDGE_COND_CONTINUE = 3,

  /* Conditional instruction: immediate relocation when the peeked
     condition does not hold.  This is a VM execution target, not a
     deferred stack entry. */
  ONIG_GRAPH_EDGE_COND_JUMP = 4,

  /* Conditional instruction: address pushed on the backtrack stack
     when the peeked condition holds.  Deferred, not immediate. */
  ONIG_GRAPH_EDGE_COND_PUSH = 5,

  /* OP_REPEAT: immediate entry into the repeated body. */
  ONIG_GRAPH_EDGE_REPEAT_ENTER = 6,

  /* OP_REPEAT with lower == 0: greedy zero-iteration path, stored as a
     deferred alternative pointing past OP_REPEAT_INC. */
  ONIG_GRAPH_EDGE_REPEAT_SKIP = 7,

  /* OP_REPEAT_INC: body entry for another iteration (immediate when
     n < upper; conditional on the running count). */
  ONIG_GRAPH_EDGE_REPEAT_BACK = 8,

  /* OP_REPEAT_INC: loop exit continuation (immediate when n >= upper;
     also emitted deferred when the exit is pushed as an alternative). */
  ONIG_GRAPH_EDGE_REPEAT_EXIT = 9,

  /* OP_REPEAT_NG: immediate body entry (lower > 0 case). */
  ONIG_GRAPH_EDGE_REPEAT_NG_ENTER = 10,

  /* OP_REPEAT_NG with lower == 0: deferred body alternative. */
  ONIG_GRAPH_EDGE_REPEAT_NG_SKIP = 11,

  /* OP_REPEAT_INC_NG: body back edge (immediate when n < lower,
     deferred when the body is pushed as an alternative). */
  ONIG_GRAPH_EDGE_REPEAT_NG_BACK = 12,

  /* OP_REPEAT_INC_NG: loop exit continuation. */
  ONIG_GRAPH_EDGE_REPEAT_NG_EXIT = 13,

  /* OP_CALL: absolute call edge to the called body.  There is no
     ordinary fallthrough for OP_CALL. */
  ONIG_GRAPH_EDGE_CALL = 14,

  /* Conservative continuation after a call.  This edge is a summary
     relationship derived from the call site (from+1); it is not a VM
     execution edge emitted by OP_CALL and it does not enumerate any
     runtime call stack. */
  ONIG_GRAPH_EDGE_RESUME_SUMMARY = 15,

  /* EMPTY_CHECK_END / _MEMST / _MEMST_PUSH: when the empty check
     succeeds the VM skips the following instruction.  This edge is the
     conditional skip target (from+2); it is distinct from the ordinary
     fallthrough (from+1) that is taken when the check does not
     succeed. */
  ONIG_GRAPH_EDGE_EMPTY_CHECK_SKIP = 16,

  ONIG_GRAPH_EDGE_TYPE_COUNT = 17
} OnigGraphEdgeType;

/* Node flags. */
#define ONIG_GRAPH_NODE_ENTRY           0x0001u /* in the entry set */
#define ONIG_GRAPH_NODE_REACHABLE       0x0002u /* structurally reachable from op 0 */
#define ONIG_GRAPH_NODE_SUCCESS_EXIT    0x0004u /* OP_END */
#define ONIG_GRAPH_NODE_FAIL_EXIT       0x0008u /* OP_FAIL: pops the backtrack stack */
#define ONIG_GRAPH_NODE_DYNAMIC_BACKTRACK_EXIT ONIG_GRAPH_NODE_FAIL_EXIT
#define ONIG_GRAPH_NODE_FINISH          0x0010u /* OP_FINISH sentinel */
#define ONIG_GRAPH_NODE_DYNAMIC_RETURN  0x0020u /* OP_RETURN: dynamic return exit */
#define ONIG_GRAPH_NODE_CALL            0x0040u /* OP_CALL instruction */
#define ONIG_GRAPH_NODE_REPEAT_ENTRY    0x0080u /* OP_REPEAT / OP_REPEAT_NG */
#define ONIG_GRAPH_NODE_REPEAT_INC      0x0100u /* OP_REPEAT_INC / _NG */
#define ONIG_GRAPH_NODE_UNREACHABLE     0x0200u /* explicit counterpart of REACHABLE */

typedef struct {
  int index;                /* Operation index: the node identity */
  int opcode;               /* OnigGraphOpcode */
  const char* opcode_name;  /* static string, safe after graph free */
  unsigned int flags;       /* ONIG_GRAPH_NODE_* */
  int out_edge_count;       /* typed outgoing edges (including summary) */

  /* For OP_STR_* nodes only: an independently owned copy of the exact
     bytes the instruction compares.  Already decoded from the string
     pool (or the inline 16-byte buffer); never a borrowed pointer.
     literal_len is 0 and literal is NULL for every other opcode. */
  int literal_len;
  const unsigned char* literal;
} OnigGraphNode;

typedef struct {
  int from;                 /* Operation index */
  int to;                   /* Operation index */
  int type;                 /* OnigGraphEdgeType */

  /*
   * Structural annotations, so a consumer never has to infer them from
   * the opcode again:
   *   deferred    - 1 when the target is reached through the VM
   *                 backtrack stack (a deferred alternative), 0 when the
   *                 VM can transfer control immediately.
   *   conditional - 1 when the edge is only taken under a runtime
   *                 condition (peek match, empty check, iteration
   *                 count); 0 when it is unconditional.
   */
  int deferred;
  int conditional;
} OnigGraphEdge;

/* One OP_CALL instruction. */
typedef struct {
  int call_index;           /* index of the OP_CALL instruction */
  int target;               /* absolute Operation index of the callee */
  int resume_index;         /* call_index + 1, or -1 when there is none */
} OnigGraphCall;

/* One repetition construct, decoded from the repeat_range table. */
typedef struct {
  int entry_index;          /* OP_REPEAT / OP_REPEAT_NG index */
  int inc_index;            /* matching OP_REPEAT_INC / _NG index */
  int body_index;           /* Operation index converted from repeat_range body */
  int lower;                /* minimum iteration count */
  int upper;                /* maximum; ONIG_GRAPH_INFINITE_REPEAT if unbounded */
  int greedy;               /* 1 greedy, 0 non-greedy */
} OnigGraphRepeat;

/* One call-graph edge: entry -> target of a CALL reached while walking
   the entry's body.  Multiple call sites to the same target collapse
   into one edge; first_call_index/call_count describe the sites. */
typedef struct {
  int from_entry;
  int to_entry;
  int first_call_index;
  int call_count;
} OnigGraphCallEdge;

/* One strongly connected component of the call graph. */
typedef struct {
  int id;                   /* stable component id = smallest entry index */
  int recursive;            /* size > 1 or a self loop */
  int member_count;
  const int* members;       /* ascending entry indices, graph-owned */
} OnigGraphScc;

/* ------------------------------------------------------------------ */
/* Error reporting.                                                    */
/* ------------------------------------------------------------------ */
typedef enum {
  ONIG_GRAPH_OK = 0,
  ONIG_GRAPH_ERR_INVALID_ARGUMENT = 1,
  ONIG_GRAPH_ERR_EMPTY_PROGRAM = 2,
  ONIG_GRAPH_ERR_MEMORY = 3,
  ONIG_GRAPH_ERR_DECODE = 4,           /* missing opcode table for the build */
  ONIG_GRAPH_ERR_UNKNOWN_OPCODE = 5,   /* not part of this fixed version */
  ONIG_GRAPH_ERR_UNSUPPORTED_OPCODE = 6,
  ONIG_GRAPH_ERR_INVALID_TARGET = 7,   /* jump/fallthrough target out of range */
  ONIG_GRAPH_ERR_INVALID_REPEAT = 8,   /* repeat id / repeat_range invalid */
  ONIG_GRAPH_ERR_INCONSISTENT_REPEAT = 9, /* entry/inc/body mismatch */
  ONIG_GRAPH_ERR_INVALID_MEMORY = 10   /* capture / empty-check id out of range */
} OnigGraphErrorReason;

typedef struct {
  OnigGraphErrorReason reason;
  int op_index;             /* -1 when not instruction-specific */
  int opcode;               /* OnigGraphOpcode or -1 */
  char message[192];
} OnigGraphError;

/* ------------------------------------------------------------------ */
/* Creation, lifetime and error strings.                               */
/* ------------------------------------------------------------------ */

/* Build a graph from an already compiled regex.  Read-only.
   Returns NULL on failure; when error is non-NULL it is filled in.
   On failure no partial graph is returned. */
OnigGraph* onig_graph_new(const OnigRegexType* regex, OnigGraphError* error);

/* Frees the graph and all arrays it owns.  NULL is accepted. */
void onig_graph_free(OnigGraph* graph);

const char* onig_graph_opcode_name(int opcode);
const char* onig_graph_edge_type_name(int edge_type);
const char* onig_graph_error_reason_name(int reason);

/* ------------------------------------------------------------------ */
/* Read-only accessors.  All pointers are owned by the graph and stay  */
/* valid until onig_graph_free().                                      */
/* ------------------------------------------------------------------ */

int onig_graph_node_count(const OnigGraph* graph);
const OnigGraphNode* onig_graph_node(const OnigGraph* graph, int index);

int onig_graph_edge_count(const OnigGraph* graph);
/* Global edge list, sorted by (from, type, to). */
const OnigGraphEdge* onig_graph_edge(const OnigGraph* graph, int index);
/* Outgoing edges of one node, sorted by (type, to). */
int onig_graph_out_edge_count(const OnigGraph* graph, int node_index);
const OnigGraphEdge* onig_graph_out_edge(const OnigGraph* graph,
                                         int node_index, int index);

int onig_graph_call_count(const OnigGraph* graph);
/* CALL records, sorted by call_index. */
const OnigGraphCall* onig_graph_call(const OnigGraph* graph, int index);
/* CALL record for a specific Operation index, or NULL. */
const OnigGraphCall* onig_graph_call_at(const OnigGraph* graph, int node_index);

int onig_graph_repeat_count(const OnigGraph* graph);
/* Repeat records, sorted by entry_index. */
const OnigGraphRepeat* onig_graph_repeat(const OnigGraph* graph, int index);
/* Repeat record for a REPEAT/REPEAT_INC node, or NULL. */
const OnigGraphRepeat* onig_graph_repeat_at(const OnigGraph* graph,
                                            int node_index);

int onig_graph_entry_count(const OnigGraph* graph);
/* Entry set: main entry 0 plus every CALL target, ascending. */
int onig_graph_entry(const OnigGraph* graph, int index);
int onig_graph_is_entry(const OnigGraph* graph, int node_index);

int onig_graph_call_edge_count(const OnigGraph* graph);
/* Call-graph edges, sorted by (from_entry, to_entry). */
const OnigGraphCallEdge* onig_graph_call_edge(const OnigGraph* graph, int index);

int onig_graph_scc_count(const OnigGraph* graph);
/* Call-graph SCCs, sorted by stable id (smallest entry index). */
const OnigGraphScc* onig_graph_scc(const OnigGraph* graph, int index);
/* SCC containing an entry, or NULL if node_index is not an entry. */
const OnigGraphScc* onig_graph_scc_for_entry(const OnigGraph* graph,
                                             int entry_index);

int onig_graph_node_reachable(const OnigGraph* graph, int node_index);

#ifdef __cplusplus
}
#endif

#endif /* REGGRAPH_H */
