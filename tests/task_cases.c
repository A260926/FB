// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

// Regression coverage for incremental MemoryMap population and resumable
// DWARF location-expression evaluation. These cases call the production
// memory_map_read / dw_eval entry points directly, with fixed-byte
// expressions and memory fragments.

#include "task_support/check.h"

//- fixed virtual addresses used by every case
#define BASE_VADDR  0x1000ULL
#define OTHER_VADDR 0x2000ULL

//- DWARF expression opcodes used below (values per DWARF 5)
#define OP_CONST1U    ((U8)DW_ExprOp_Const1U)
#define OP_CONSTU     ((U8)DW_ExprOp_ConstU)
#define OP_DEREF      ((U8)DW_ExprOp_Deref)
#define OP_DEREFSIZE  ((U8)DW_ExprOp_DerefSize)
#define OP_PLUS       ((U8)DW_ExprOp_Plus)

static unsigned
eval_stack_depth(DW_EvalState *s)
{
  unsigned n = 0;
  for(DW_EvalValNode *p = s->top_val; p != 0; p = p->next) n += 1;
  return n;
}

static DW_Eval
run_eval(Arena *a, MemoryMap *map, DW_EvalState *st, String8 expr)
{
  return dw_eval(a, DW_Format_32Bit, Arch_x64, map, 0, 0, 0, 0, st, expr, 100);
}

// // // // // // // // // // // // // // // // // // // // // // // // //
// fragments: MemoryMap override, gap and prefix-length semantics
// // // // // // // // // // // // // // // // // // // // // // // // //

// newer fragments override older ones byte-by-byte; older, larger fragments
// never cover the newer small fragment.
static int
fragments_newer_overrides_older(void)
{
  Arena *a = arena_alloc();
  U8 old_bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  U8 new_bytes[2] = {9, 10};
  MemoryMap map = {0};
  memory_map_push(a, &map, r1u64(BASE_VADDR, BASE_VADDR+8), old_bytes);
  memory_map_push(a, &map, r1u64(BASE_VADDR+2, BASE_VADDR+4), new_bytes);
  U8 out[8] = {0};
  U64 got = memory_map_read(&map, r1u64(BASE_VADDR, BASE_VADDR+8), out);
  TASK_CHECK(got == 8);
  TASK_CHECK(out[0] == 1 && out[1] == 2);
  TASK_CHECK(out[2] == 9 && out[3] == 10);
  TASK_CHECK(out[4] == 5 && out[5] == 6 && out[6] == 7 && out[7] == 8);
  arena_release(a);
  return 0;
}

// reads may cross adjacent fragments, but stop at the first uncovered byte
// and never skip a gap to stitch later fragments together.
static int
fragments_gap_stops_prefix(void)
{
  Arena *a = arena_alloc();
  U8 first[2] = {0x11, 0x22};
  U8 after_gap[2] = {0x33, 0x44};
  MemoryMap map = {0};
  memory_map_push(a, &map, r1u64(BASE_VADDR, BASE_VADDR+2), first);
  memory_map_push(a, &map, r1u64(BASE_VADDR+4, BASE_VADDR+6), after_gap);
  U8 out[6] = {0};
  U64 got = memory_map_read(&map, r1u64(BASE_VADDR, BASE_VADDR+6), out);
  TASK_CHECK(got == 2);
  TASK_CHECK(out[0] == 0x11 && out[1] == 0x22);
  TASK_CHECK(out[2] == 0 && out[3] == 0 && out[4] == 0 && out[5] == 0);

  // filling the gap with an adjacent fragment makes the full range readable.
  U8 bridge[2] = {0x55, 0x66};
  memory_map_push(a, &map, r1u64(BASE_VADDR+2, BASE_VADDR+4), bridge);
  U8 joined[6] = {0};
  got = memory_map_read(&map, r1u64(BASE_VADDR, BASE_VADDR+6), joined);
  TASK_CHECK(got == 6);
  TASK_CHECK(joined[0] == 0x11 && joined[1] == 0x22);
  TASK_CHECK(joined[2] == 0x55 && joined[3] == 0x66);
  TASK_CHECK(joined[4] == 0x33 && joined[5] == 0x44);
  arena_release(a);
  return 0;
}

// empty reads return zero and never touch the destination buffer.
static int
fragments_empty_read_untouched(void)
{
  Arena *a = arena_alloc();
  U8 bytes[4] = {1, 2, 3, 4};
  MemoryMap map = {0};
  memory_map_push(a, &map, r1u64(BASE_VADDR, BASE_VADDR+4), bytes);
  U8 out[4] = {0xaa, 0xbb, 0xcc, 0xdd};
  U64 got = memory_map_read(&map, r1u64(BASE_VADDR, BASE_VADDR), out);
  TASK_CHECK(got == 0);
  TASK_CHECK(out[0] == 0xaa && out[1] == 0xbb && out[2] == 0xcc && out[3] == 0xdd);
  arena_release(a);
  return 0;
}

// a complete, non-overlapping read returns every requested byte unchanged.
static int
fragments_full_read_unchanged(void)
{
  Arena *a = arena_alloc();
  U8 bytes[8] = {8, 7, 6, 5, 4, 3, 2, 1};
  MemoryMap map = {0};
  memory_map_push(a, &map, r1u64(BASE_VADDR, BASE_VADDR+8), bytes);
  U8 out[8] = {0};
  U64 got = memory_map_read(&map, r1u64(BASE_VADDR, BASE_VADDR+8), out);
  TASK_CHECK(got == 8);
  for(unsigned i = 0; i < 8; i += 1) TASK_CHECK(out[i] == bytes[i]);
  arena_release(a);
  return 0;
}

// // // // // // // // // // // // // // // // // // // // // // // // //
// resume: paused dereferences and incremental memory population
// // // // // // // // // // // // // // // // // // // // // // // // //

// a partial deref pauses, and repeated resumes with still-incomplete memory
// keep the opcode's pops, off and op_idx frozen at pre-execution state.
// once complete, the result matches a one-shot evaluation over the same
// final memory.
static int
resume_partial_then_complete_matches_oneshot(void)
{
  // constu BASE_VADDR ; deref_size 8
  U8 code[] = {OP_CONSTU, 0x80, 0x20, OP_DEREFSIZE, 8};
  String8 expr = str8(code, sizeof(code));
  U8 full_bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};

  Arena *a = arena_alloc();
  MemoryMap map = {0};
  DW_EvalState st = {0};

  // first two bytes of the deref are present, the rest is missing.
  // constu already completed (1 op); the paused deref undid its pop, so the
  // address is back on the stack and off points at the deref opcode.
  U8 partial[2] = {1, 2};
  memory_map_push(a, &map, r1u64(BASE_VADDR, BASE_VADDR+2), partial);
  DW_Eval r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_FailedMemoryRead);
  TASK_CHECK(r.missed_read_vaddr_range.min == BASE_VADDR);
  TASK_CHECK(r.missed_read_vaddr_range.max == BASE_VADDR+8);
  TASK_CHECK(st.off == 3);
  TASK_CHECK(st.op_idx == 1);
  TASK_CHECK(eval_stack_depth(&st) == 1);
  TASK_CHECK(st.top_val->v.u512.u64[0] == BASE_VADDR);

  // resuming without adding anything must fail identically: no duplicated
  // pops, no half-published value, no consumed instruction.
  r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_FailedMemoryRead);
  TASK_CHECK(r.missed_read_vaddr_range.min == BASE_VADDR);
  TASK_CHECK(r.missed_read_vaddr_range.max == BASE_VADDR+8);
  TASK_CHECK(st.off == 3 && st.op_idx == 1);
  TASK_CHECK(eval_stack_depth(&st) == 1);
  TASK_CHECK(st.top_val->v.u512.u64[0] == BASE_VADDR);

  // add two more bytes: still short, still paused at the same opcode.
  U8 more[2] = {3, 4};
  memory_map_push(a, &map, r1u64(BASE_VADDR+2, BASE_VADDR+4), more);
  r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_FailedMemoryRead);
  TASK_CHECK(st.off == 3 && st.op_idx == 1);
  TASK_CHECK(eval_stack_depth(&st) == 1);
  TASK_CHECK(st.top_val->v.u512.u64[0] == BASE_VADDR);

  // complete the read and resume: one instruction is counted exactly once.
  U8 rest[4] = {5, 6, 7, 8};
  memory_map_push(a, &map, r1u64(BASE_VADDR+4, BASE_VADDR+8), rest);
  r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_Good);
  TASK_CHECK(st.off == sizeof(code));
  TASK_CHECK(st.op_idx == 2);
  TASK_CHECK(eval_stack_depth(&st) == 1);
  TASK_CHECK(r.val.u512.u64[0] == 0x0807060504030201ULL);

  // one-shot evaluation with the same final memory must agree exactly.
  MemoryMap full_map = {0};
  memory_map_push(a, &full_map, r1u64(BASE_VADDR, BASE_VADDR+8), full_bytes);
  DW_EvalState full_st = {0};
  DW_Eval full = run_eval(a, &full_map, &full_st, expr);
  TASK_CHECK(full.status == DW_EvalStatus_Good);
  TASK_CHECK(full.val.u512.u64[0] == r.val.u512.u64[0]);
  TASK_CHECK(full_st.off == st.off);
  TASK_CHECK(full_st.op_idx == st.op_idx);
  TASK_CHECK(eval_stack_depth(&full_st) == eval_stack_depth(&st));
  arena_release(a);
  return 0;
}

// a deref with another value already beneath it on the stack: pausing leaves
// that value untouched, and completing pops the address and leaves the
// dereferenced value on top of the pre-existing one.
static int
resume_preserves_existing_stack_value(void)
{
  // const1u 7 ; constu BASE_VADDR ; deref_size 4
  U8 code[] = {OP_CONST1U, 7, OP_CONSTU, 0x80, 0x20, OP_DEREFSIZE, 4};
  String8 expr = str8(code, sizeof(code));

  Arena *a = arena_alloc();
  MemoryMap map = {0};
  DW_EvalState st = {0};
  DW_Eval r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_FailedMemoryRead);
  // const1u and constu completed; the deref's pop was undone, so both values
  // remain on the stack exactly as they were before the deref ran.
  TASK_CHECK(st.op_idx == 2);
  TASK_CHECK(st.off == 5);
  TASK_CHECK(eval_stack_depth(&st) == 2);
  TASK_CHECK(st.top_val->v.u512.u64[0] == BASE_VADDR);
  TASK_CHECK(st.top_val->next->v.u512.u64[0] == 7);
  TASK_CHECK(r.missed_read_vaddr_range.min == BASE_VADDR);
  TASK_CHECK(r.missed_read_vaddr_range.max == BASE_VADDR+4);

  // resuming while still missing must not eat the address a second time.
  r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_FailedMemoryRead);
  TASK_CHECK(st.op_idx == 2 && st.off == 5);
  TASK_CHECK(eval_stack_depth(&st) == 2);
  TASK_CHECK(st.top_val->v.u512.u64[0] == BASE_VADDR);
  TASK_CHECK(st.top_val->next->v.u512.u64[0] == 7);

  U8 bytes[4] = {0xde, 0xad, 0xbe, 0xef};
  memory_map_push(a, &map, r1u64(BASE_VADDR, BASE_VADDR+4), bytes);
  r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_Good);
  TASK_CHECK(st.op_idx == 3);
  TASK_CHECK(eval_stack_depth(&st) == 2);
  TASK_CHECK(r.val.u512.u64[0] == 0xefbeaddeULL);
  TASK_CHECK(st.top_val->next->v.u512.u64[0] == 7);

  // one-shot control with the same final memory.
  MemoryMap full_map = {0};
  memory_map_push(a, &full_map, r1u64(BASE_VADDR, BASE_VADDR+4), bytes);
  DW_EvalState full_st = {0};
  DW_Eval full = run_eval(a, &full_map, &full_st, expr);
  TASK_CHECK(full.status == DW_EvalStatus_Good);
  TASK_CHECK(full.val.u512.u64[0] == r.val.u512.u64[0]);
  TASK_CHECK(full_st.op_idx == st.op_idx);
  TASK_CHECK(eval_stack_depth(&full_st) == eval_stack_depth(&st));
  arena_release(a);
  return 0;
}

// two dereferences in a row, each needing its own memory fill, using
// DW_OP_deref (architecture-sized) for the second one.
static int
resume_two_consecutive_derefs(void)
{
  // constu OTHER_VADDR ; deref_size 8 ; deref
  U8 code[] = {OP_CONSTU, 0x80, 0x40, OP_DEREFSIZE, 8, OP_DEREF};
  String8 expr = str8(code, sizeof(code));
  U8 pointer_bytes[8] = {0};
  U8 target_bytes[8] = {0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00};
  // encode BASE_VADDR little-endian as the pointer stored at OTHER_VADDR
  for(unsigned i = 0; i < 8; i += 1) pointer_bytes[i] = (U8)(BASE_VADDR >> (8*i));

  Arena *a = arena_alloc();
  MemoryMap map = {0};
  DW_EvalState st = {0};

  // nothing mapped: constu completes, then the first deref pauses and undoes
  // its pop, leaving the address back on the stack at the deref opcode.
  DW_Eval r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_FailedMemoryRead);
  TASK_CHECK(r.missed_read_vaddr_range.min == OTHER_VADDR);
  TASK_CHECK(r.missed_read_vaddr_range.max == OTHER_VADDR+8);
  TASK_CHECK(st.off == 3);
  TASK_CHECK(st.op_idx == 1);
  TASK_CHECK(eval_stack_depth(&st) == 1);
  TASK_CHECK(st.top_val->v.u512.u64[0] == OTHER_VADDR);

  // supply the pointer fragment: first deref completes, second pauses.
  memory_map_push(a, &map, r1u64(OTHER_VADDR, OTHER_VADDR+8), pointer_bytes);
  r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_FailedMemoryRead);
  TASK_CHECK(r.missed_read_vaddr_range.min == BASE_VADDR);
  TASK_CHECK(r.missed_read_vaddr_range.max == BASE_VADDR+8);
  TASK_CHECK(st.off == 5);
  TASK_CHECK(st.op_idx == 2);
  TASK_CHECK(eval_stack_depth(&st) == 1);
  TASK_CHECK(st.top_val->v.u512.u64[0] == BASE_VADDR);

  // second miss on the same opcode must be a stable pause.
  r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_FailedMemoryRead);
  TASK_CHECK(st.off == 5 && st.op_idx == 2);
  TASK_CHECK(eval_stack_depth(&st) == 1);

  // supply the final target: second deref completes, three ops total.
  memory_map_push(a, &map, r1u64(BASE_VADDR, BASE_VADDR+8), target_bytes);
  r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_Good);
  TASK_CHECK(st.off == sizeof(code));
  TASK_CHECK(st.op_idx == 3);
  TASK_CHECK(eval_stack_depth(&st) == 1);
  TASK_CHECK(r.val.u512.u64[0] == 0x0011223344556677ULL);

  // one-shot control with both fragments present from the start.
  MemoryMap full_map = {0};
  memory_map_push(a, &full_map, r1u64(OTHER_VADDR, OTHER_VADDR+8), pointer_bytes);
  memory_map_push(a, &full_map, r1u64(BASE_VADDR, BASE_VADDR+8), target_bytes);
  DW_EvalState full_st = {0};
  DW_Eval full = run_eval(a, &full_map, &full_st, expr);
  TASK_CHECK(full.status == DW_EvalStatus_Good);
  TASK_CHECK(full.val.u512.u64[0] == r.val.u512.u64[0]);
  TASK_CHECK(full_st.op_idx == st.op_idx && full_st.off == st.off);
  TASK_CHECK(eval_stack_depth(&full_st) == eval_stack_depth(&st));
  arena_release(a);
  return 0;
}

// // // // // // // // // // // // // // // // // // // // // // // // //
// terminal: end-of-expression behavior and non-memory controls
// // // // // // // // // // // // // // // // // // // // // // // // //

// when the final instruction is a deref, an incomplete read is a pause, not
// Good; after filling memory the same state resumes to Good with the value.
static int
terminal_deref_pause_is_not_good(void)
{
  // constu BASE_VADDR ; deref_size 2  (deref is the last opcode)
  U8 code[] = {OP_CONSTU, 0x80, 0x20, OP_DEREFSIZE, 2};
  String8 expr = str8(code, sizeof(code));

  Arena *a = arena_alloc();
  MemoryMap map = {0};
  U8 one_byte[1] = {0xab};
  memory_map_push(a, &map, r1u64(BASE_VADDR, BASE_VADDR+1), one_byte);
  DW_EvalState st = {0};
  DW_Eval r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_FailedMemoryRead);
  TASK_CHECK(st.off == 3 && st.op_idx == 1);
  TASK_CHECK(eval_stack_depth(&st) == 1);
  TASK_CHECK(st.top_val->v.u512.u64[0] == BASE_VADDR);
  TASK_CHECK(r.missed_read_vaddr_range.min == BASE_VADDR);
  TASK_CHECK(r.missed_read_vaddr_range.max == BASE_VADDR+2);

  U8 second_byte[1] = {0xcd};
  memory_map_push(a, &map, r1u64(BASE_VADDR+1, BASE_VADDR+2), second_byte);
  r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_Good);
  TASK_CHECK(st.off == sizeof(code));
  TASK_CHECK(st.op_idx == 2);
  TASK_CHECK(eval_stack_depth(&st) == 1);
  TASK_CHECK(r.val.u512.u64[0] == 0xcdabULL);
  arena_release(a);
  return 0;
}

// a normal, fully-memory-backed expression evaluates in one pass.
static int
terminal_full_memory_control(void)
{
  // constu BASE_VADDR ; deref_size 8
  U8 code[] = {OP_CONSTU, 0x80, 0x20, OP_DEREFSIZE, 8};
  String8 expr = str8(code, sizeof(code));
  U8 bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};

  Arena *a = arena_alloc();
  MemoryMap map = {0};
  memory_map_push(a, &map, r1u64(BASE_VADDR, BASE_VADDR+8), bytes);
  DW_EvalState st = {0};
  DW_Eval r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_Good);
  TASK_CHECK(r.val.u512.u64[0] == 0x0807060504030201ULL);
  TASK_CHECK(st.off == sizeof(code));
  TASK_CHECK(st.op_idx == 2);
  TASK_CHECK(eval_stack_depth(&st) == 1);
  arena_release(a);
  return 0;
}

// a no-memory expression (pure stack arithmetic) never touches the map and
// still completes normally.
static int
terminal_no_memory_control(void)
{
  // const1u 1 ; const1u 2 ; plus  -> 1 + 2 = 3, no memory reads at all
  U8 code[] = {OP_CONST1U, 1, OP_CONST1U, 2, OP_PLUS};
  String8 expr = str8(code, sizeof(code));

  Arena *a = arena_alloc();
  MemoryMap map = {0};
  DW_EvalState st = {0};
  DW_Eval r = run_eval(a, &map, &st, expr);
  TASK_CHECK(r.status == DW_EvalStatus_Good);
  TASK_CHECK(r.val.u512.u64[0] == 3);
  TASK_CHECK(st.off == sizeof(code));
  TASK_CHECK(st.op_idx == 3);
  TASK_CHECK(eval_stack_depth(&st) == 1);

  // op_idx_cap still bounds completed instructions, and a pause never
  // consumed one: here the cap is hit on the third instruction instead.
  DW_EvalState capped_st = {0};
  DW_Eval capped = dw_eval(a, DW_Format_32Bit, Arch_x64, &map, 0, 0, 0, 0,
                           &capped_st, expr, 2);
  TASK_CHECK(capped.status == DW_EvalStatus_ExecOpLimitReached);
  TASK_CHECK(capped_st.op_idx == 2);
  TASK_CHECK(capped_st.off == 4);
  arena_release(a);
  return 0;
}

// // // // // // // // // // // // // // // // // // // // // // // // //
// case registration
// // // // // // // // // // // // // // // // // // // // // // // // //

const struct task_case task_cases[] =
{
  { "fragments", "newer_overrides_older",  fragments_newer_overrides_older  },
  { "fragments", "gap_stops_prefix",       fragments_gap_stops_prefix       },
  { "fragments", "empty_read_untouched",   fragments_empty_read_untouched   },
  { "fragments", "full_read_unchanged",    fragments_full_read_unchanged    },

  { "resume",    "partial_then_complete_matches_oneshot",
    resume_partial_then_complete_matches_oneshot },
  { "resume",    "preserves_existing_stack_value",
    resume_preserves_existing_stack_value },
  { "resume",    "two_consecutive_derefs", resume_two_consecutive_derefs    },

  { "terminal",  "deref_pause_is_not_good", terminal_deref_pause_is_not_good },
  { "terminal",  "full_memory_control",     terminal_full_memory_control     },
  { "terminal",  "no_memory_control",       terminal_no_memory_control       },
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
