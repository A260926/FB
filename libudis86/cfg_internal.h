/* udis86 - libudis86/cfg_internal.h
 *
 * Internal structures shared by cfg_normalize.c (control-flow recovery and
 * instruction classification) and cfg_match.c (structural matching).  Not a
 * public header.
 */
#ifndef UD_CFG_INTERNAL_H
#define UD_CFG_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

#include "udis86.h"
#include "cfg_match.h"

/* RFLAGS bits tracked by the supported instruction set. */
#define CGF_CF   (1u << 0)
#define CGF_PF   (1u << 1)
#define CGF_AF   (1u << 2)
#define CGF_ZF   (1u << 3)
#define CGF_SF   (1u << 4)
#define CGF_OF   (1u << 5)
#define CGF_ALL  (CGF_CF | CGF_PF | CGF_AF | CGF_ZF | CGF_SF | CGF_OF)

enum cfg_op_kind
{
  CFG_OP_NONE = 0,
  CFG_OP_REG  = 1,
  CFG_OP_MEM  = 2,
  CFG_OP_IMM  = 3
};

struct cfg_reg_view
{
  unsigned family; /* 0..15, index of the 64-bit view (RAX..R15) */
  unsigned width;  /* 8, 16, 32 or 64 bits */
  int      high;   /* nonzero for AH/CH/DH/BH */
};

struct cfg_mem
{
  unsigned width;       /* access width in bits */
  int      has_base;
  unsigned base_family;
  int      has_index;
  unsigned index_family;
  unsigned scale;       /* 1/2/4/8, 0 when there is no index */
  int      is_rip;
  int64_t  disp;        /* sign-extended displacement */
  uint64_t rip_ea;      /* absolute EA when is_rip, else unused */
};

struct cfg_op
{
  enum cfg_op_kind    kind;
  struct cfg_reg_view reg;
  struct cfg_mem      mem;
  int64_t             imm; /* sign-extended at the operand's own width */
};

enum cfg_insn_kind
{
  CFG_IK_PLAIN = 0, /* retained data instruction */
  CFG_IK_JCC   = 1,
  CFG_IK_CALL  = 2,
  CFG_IK_RET   = 3,
  CFG_IK_NOP   = 4, /* transparent */
  CFG_IK_JMP   = 5  /* transparent, direct only */
};

struct cfg_insn
{
  uint64_t va;
  unsigned len;
  enum ud_mnemonic_code mn;
  enum cfg_insn_kind    kind;
  int      transparent;

  struct cfg_op op[2];
  unsigned      op_count;

  unsigned flags_read;
  unsigned flags_write;
  int      mem_read;   /* real memory access; LEA never sets this */
  int      mem_write;

  /* Direct successors in the raw graph (fall-through / branch / return
   * site), before transparent-chain folding. */
  uint64_t succ[2];
  unsigned succ_count;
  uint64_t call_target;

  /* Bookkeeping filled in while building the block graph. */
  unsigned block;
  unsigned walk_mark;
};

struct cfg_block
{
  uint64_t entry_va;
  /* Half-open span of positions in cfg_function.retained[]. */
  unsigned first;
  unsigned last;
  unsigned succ[2];    /* block indices; -1u slots are unused */
  unsigned succ_count;
  unsigned pred_count;
  int      dead;
  int      is_jcc;
  int      is_call;
  int      is_ret;
};

struct cfg_function
{
  uint64_t base;
  uint64_t size;
  uint64_t entry;

  struct cfg_insn *insns;
  size_t           insn_count;
  size_t           insn_cap;

  /* Positions into insns[], transparent nodes removed, in VA order. */
  unsigned        *retained;
  size_t           retained_count;

  struct cfg_block *blocks;
  size_t           block_count;
  size_t           block_cap;

  enum ud_match_status status;
  unsigned             walk_gen;
};

/* cfg_normalize.c */
enum ud_match_status
cfg_normalize(const struct ud_function_bytes *fb,
              struct cfg_function **out_function);

void cfg_function_free(struct cfg_function *f);

/* Returns the retained-insn index reached after folding transparent
 * (NOP / direct JMP) chains starting at va.  Returns -1u on a
 * transparent-only cycle. */
unsigned cfg_resolve_retained(struct cfg_function *f, uint64_t va);

/* Sign-extend a value that occupies the low 'bits' bits. */
int64_t cfg_sign_extend(uint64_t value, unsigned bits);

/* cfg_match.c helpers reused by the result builder. */
int cfg_jcc_is_complement(enum ud_mnemonic_code a, enum ud_mnemonic_code b);
unsigned cfg_jcc_flags(enum ud_mnemonic_code mn);

#endif /* UD_CFG_INTERNAL_H */
