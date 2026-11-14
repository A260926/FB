/* udis86 - libudis86/cfg_normalize.c
 *
 * Recovers the direct control-flow graph of one x86-64 function from a
 * read-only byte image.
 *
 * Decoding starts at the entry and only follows direct JMP/Jcc/CALL/RET and
 * sequential flow.  Unreachable bytes never participate.  NOPs and direct
 * JMPs are transparent nodes whose chains are folded; basic blocks are then
 * partitioned and straight single-predecessor/single-successor blocks are
 * merged.  Loops and convergence points are preserved.
 */
#include "cfg_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

int64_t
cfg_sign_extend(uint64_t value, unsigned bits)
{
  uint64_t sign = (uint64_t)1 << (bits - 1);
  uint64_t mask = bits >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << bits) - 1);
  value &= mask;
  return (int64_t)((value ^ sign) - sign);
}

/* base + signed offset with uint64 overflow detection.  Returns 0 on success
 * and -1 when the absolute address would leave the address space. */
static int
addr_add(uint64_t base, int64_t off, uint64_t *out)
{
  if (off >= 0) {
    uint64_t mag = (uint64_t)off;
    if (base > UINT64_MAX - mag) {
      return -1;
    }
    *out = base + mag;
  } else {
    uint64_t mag = (uint64_t)0 - (uint64_t)off; /* unsigned negation */
    if (base < mag) {
      return -1;
    }
    *out = base - mag;
  }
  return 0;
}

static int
is_gpr_reg(enum ud_type t)
{
  return t >= UD_R_AL && t <= UD_R_R15;
}

static void
reg_view(enum ud_type t, struct cfg_reg_view *v)
{
  unsigned idx = (unsigned)(t - UD_R_AL);
  v->high = 0;
  if (idx < 16) {
    v->width = 8;
    if (idx < 4) {
      /* AL, CL, DL, BL */
      v->family = idx;
    } else if (idx < 8) {
      /* Legacy AH/CH/DH/BH: high-byte view of families 0..3. */
      v->family = idx - 4;
      v->high   = 1;
    } else {
      /* SPL..DIL (families 4..7) and R8B..R15B (families 8..15). */
      v->family = idx - 4;
    }
  } else if (idx < 32) {
    v->width = 16;
    v->family = idx - 16;
  } else if (idx >= 36 && idx < 52) {
    /* segment registers occupy enum indices 32..35 between the GPR blocks */
    v->width = 32;
    v->family = idx - 36;
  } else {
    v->width = 64;
    v->family = idx - 52;
  }
}

static int64_t
imm_value(const struct ud_operand *o)
{
  switch (o->size) {
  case 8:  return (int64_t)o->lval.sbyte;
  case 16: return cfg_sign_extend(o->lval.uword, 16);
  case 32: return cfg_sign_extend(o->lval.udword, 32);
  default: return (int64_t)o->lval.uqword;
  }
}

static int64_t
disp_value(const struct ud_operand *o)
{
  if (o->offset == 0) {
    return 0;
  }
  switch (o->offset) {
  case 8:  return (int64_t)o->lval.sbyte;
  case 16: return cfg_sign_extend(o->lval.uword, 16);
  default: return cfg_sign_extend(o->lval.udword, 32);
  }
}

static int
is_jcc(enum ud_mnemonic_code mn)
{
  switch (mn) {
  case UD_Ijo: case UD_Ijno:
  case UD_Ijb: case UD_Ijae:
  case UD_Ijz: case UD_Ijnz:
  case UD_Ijbe: case UD_Ija:
  case UD_Ijs: case UD_Ijns:
  case UD_Ijp: case UD_Ijnp:
  case UD_Ijl: case UD_Ijge:
  case UD_Ijle: case UD_Ijg:
    return 1;
  default:
    return 0;
  }
}

/* ------------------------------------------------------------------ */
/* instruction classification                                          */
/* ------------------------------------------------------------------ */

static unsigned
flags_written(enum ud_mnemonic_code mn)
{
  switch (mn) {
  case UD_Iadd: case UD_Iadc: case UD_Isub: case UD_Isbb:
  case UD_Icmp:
  case UD_Iand: case UD_Ior: case UD_Ixor: case UD_Itest:
    return CGF_ALL;
  case UD_Iinc: case UD_Idec:
    return CGF_PF | CGF_AF | CGF_ZF | CGF_SF | CGF_OF;
  default:
    return 0;
  }
}

static unsigned
flags_read(enum ud_mnemonic_code mn)
{
  switch (mn) {
  case UD_Iadc: case UD_Isbb:
    return CGF_CF;
  default:
    return 0;
  }
}

/* Fills in op[] / op_count for a retained data instruction and validates the
 * operand forms allowed by the contract. */
static enum ud_match_status
classify_data(struct cfg_insn *ins, const struct ud *u)
{
  const struct ud_operand *o0 = ud_insn_opr(u, 0);
  const struct ud_operand *o1 = ud_insn_opr(u, 1);
  enum ud_mnemonic_code mn = ins->mn;

  if (o0 == NULL) {
    return UD_MATCH_UNSUPPORTED;
  }

  switch (mn) {
  case UD_Imov: {
    if (o0->type != UD_OP_REG && o0->type != UD_OP_MEM) {
      return UD_MATCH_UNSUPPORTED;
    }
    if (o1 == NULL ||
        (o1->type != UD_OP_REG && o1->type != UD_OP_MEM &&
         o1->type != UD_OP_IMM)) {
      return UD_MATCH_UNSUPPORTED;
    }
    if (o0->type == UD_OP_MEM && o1->type == UD_OP_MEM) {
      return UD_MATCH_UNSUPPORTED;
    }
    break;
  }
  case UD_Imovzx:
  case UD_Imovsx:
  case UD_Imovsxd:
    if (o0->type != UD_OP_REG || o1 == NULL || o1->type != UD_OP_REG) {
      return UD_MATCH_UNSUPPORTED;
    }
    break;
  case UD_Ilea:
    if (o0->type != UD_OP_REG || o1 == NULL || o1->type != UD_OP_MEM) {
      return UD_MATCH_UNSUPPORTED;
    }
    if (o0->size != 32 && o0->size != 64) {
      return UD_MATCH_UNSUPPORTED;
    }
    break;
  case UD_Iadd: case UD_Iadc: case UD_Isub: case UD_Isbb:
  case UD_Iand: case UD_Ior:  case UD_Ixor:
  case UD_Icmp: case UD_Itest:
    if (o0->type != UD_OP_REG || o1 == NULL ||
        (o1->type != UD_OP_REG && o1->type != UD_OP_IMM)) {
      return UD_MATCH_UNSUPPORTED;
    }
    break;
  case UD_Iinc:
  case UD_Idec:
    if (o0->type != UD_OP_REG || o1 != NULL) {
      return UD_MATCH_UNSUPPORTED;
    }
    break;
  default:
    return UD_MATCH_UNSUPPORTED;
  }

  /* GPR-only register operands. */
  if ((o0->type == UD_OP_REG && !is_gpr_reg(o0->base)) ||
      (o1 != NULL && o1->type == UD_OP_REG && !is_gpr_reg(o1->base))) {
    return UD_MATCH_UNSUPPORTED;
  }

  ins->op_count = 0;
  for (int i = 0; i < 2; i++) {
    const struct ud_operand *o = (i == 0) ? o0 : o1;
    struct cfg_op *co;
    if (o == NULL || o->type == UD_NONE) {
      continue;
    }
    co = &ins->op[ins->op_count++];
    memset(co, 0, sizeof(*co));
    if (o->type == UD_OP_REG) {
      co->kind = CFG_OP_REG;
      reg_view(o->base, &co->reg);
    } else if (o->type == UD_OP_IMM) {
      co->kind = CFG_OP_IMM;
      co->imm  = imm_value(o);
    } else if (o->type == UD_OP_MEM) {
      co->kind = CFG_OP_MEM;
      co->mem.width = o->size;
      co->mem.disp  = disp_value(o);
      if (o->base == UD_R_RIP) {
        uint64_t ea;
        co->mem.is_rip = 1;
        if (addr_add(ins->va + ins->len, co->mem.disp, &ea) != 0) {
          return UD_MATCH_INVALID;
        }
        co->mem.rip_ea = ea;
      } else if (o->base != UD_NONE) {
        if (!is_gpr_reg(o->base)) {
          return UD_MATCH_UNSUPPORTED;
        }
        co->mem.has_base = 1;
        /* 64-bit addressing always names a 64-bit view. */
        co->mem.base_family = (unsigned)(o->base - UD_R_RAX);
      }
      if (o->index != UD_NONE) {
        if (!is_gpr_reg(o->index)) {
          return UD_MATCH_UNSUPPORTED;
        }
        co->mem.has_index = 1;
        co->mem.index_family = (unsigned)(o->index - UD_R_RAX);
        co->mem.scale = o->scale ? o->scale : 1;
      }
    } else {
      return UD_MATCH_UNSUPPORTED;
    }
  }

  ins->flags_read  = flags_read(mn);
  ins->flags_write = flags_written(mn);

  /* Real memory accesses.  LEA computes an address but does not access
   * memory; the only supported instructions that can still carry a memory
   * operand here are MOV forms. */
  if (mn != UD_Ilea) {
    for (unsigned i = 0; i < ins->op_count; i++) {
      if (ins->op[i].kind == CFG_OP_MEM) {
        if (i == 0) {
          ins->mem_write = 1;
        } else {
          ins->mem_read = 1;
        }
      }
    }
  }
  return UD_MATCH_OK;
}

/* ------------------------------------------------------------------ */
/* decode worklist                                                     */
/* ------------------------------------------------------------------ */

struct cfg_decoder
{
  struct cfg_function *f;
  const uint8_t       *data;
  uint64_t            *worklist;
  size_t              worklist_count;
  size_t              worklist_cap;
};

static struct cfg_insn *
find_insn(struct cfg_function *f, uint64_t va)
{
  for (size_t i = 0; i < f->insn_count; i++) {
    if (f->insns[i].va == va) {
      return &f->insns[i];
    }
  }
  return NULL;
}

static enum ud_match_status
push_target(struct cfg_decoder *d, uint64_t va)
{
  if (va < d->f->base || va >= d->f->base + d->f->size) {
    return UD_MATCH_INVALID;
  }
  for (size_t i = 0; i < d->worklist_count; i++) {
    if (d->worklist[i] == va) {
      return UD_MATCH_OK;
    }
  }
  if (find_insn(d->f, va) != NULL) {
    return UD_MATCH_OK;
  }
  if (d->worklist_count == d->worklist_cap) {
    size_t cap = d->worklist_cap ? d->worklist_cap * 2 : 16;
    uint64_t *nw = realloc(d->worklist, cap * sizeof(*nw));
    if (nw == NULL) {
      return UD_MATCH_INVALID;
    }
    d->worklist = nw;
    d->worklist_cap = cap;
  }
  d->worklist[d->worklist_count++] = va;
  return UD_MATCH_OK;
}

/* A target landing inside an already decoded instruction span is corrupt
 * input (jump into the middle of an instruction). */
static int
lands_inside_insn(struct cfg_function *f, uint64_t va)
{
  for (size_t i = 0; i < f->insn_count; i++) {
    struct cfg_insn *in = &f->insns[i];
    if (va > in->va && va < in->va + in->len) {
      return 1;
    }
  }
  return 0;
}

/* Any overlap between a freshly decoded span and an existing instruction, in
 * either direction, rejects overlapping decodes. */
static int
span_overlaps(struct cfg_function *f, uint64_t va, unsigned len)
{
  uint64_t end = va + len;
  for (size_t i = 0; i < f->insn_count; i++) {
    struct cfg_insn *in = &f->insns[i];
    if (va < in->va + in->len && in->va < end) {
      return 1;
    }
  }
  return 0;
}

static enum ud_match_status
decode_one(struct cfg_decoder *d, uint64_t va)
{
  struct cfg_function *f = d->f;
  uint64_t off = va - f->base;
  ud_t u;
  unsigned len;
  struct cfg_insn *ins;
  enum ud_match_status st;

  if (lands_inside_insn(f, va)) {
    return UD_MATCH_INVALID;
  }

  ud_init(&u);
  ud_set_mode(&u, 64);
  ud_set_pc(&u, va);
  ud_set_input_buffer(&u, d->data + off, (size_t)(f->size - off));
  len = ud_disassemble(&u);
  if (len == 0 || u.error ||
      ud_insn_mnemonic(&u) == UD_Iinvalid ||
      ud_insn_mnemonic(&u) == UD_Idb) {
    /* Illegal or truncated instruction.  This must be detected before any
     * mode/prefix test, since a failed decode leaves those fields unset. */
    return UD_MATCH_INVALID;
  }

  /* Only 64-bit addressing, no segment/LOCK/REP family prefixes. */
  if (u.adr_mode != 64 || u.pfx_seg || u.pfx_lock ||
      u.pfx_rep || u.pfx_repe || u.pfx_repne) {
    return UD_MATCH_UNSUPPORTED;
  }

  /* The decoded span must stay inside the image and must not overlap an
   * already decoded instruction (no overlapping decodes). */
  if ((uint64_t)len > f->base + f->size - va ||
      span_overlaps(f, va, len)) {
    return UD_MATCH_INVALID;
  }

  if (f->insn_count == f->insn_cap) {
    size_t cap = f->insn_cap ? f->insn_cap * 2 : 32;
    struct cfg_insn *ni = realloc(f->insns, cap * sizeof(*ni));
    if (ni == NULL) {
      return UD_MATCH_INVALID;
    }
    f->insns = ni;
    f->insn_cap = cap;
  }
  ins = &f->insns[f->insn_count++];
  memset(ins, 0, sizeof(*ins));
  ins->va = va;
  ins->len = len;
  ins->mn = ud_insn_mnemonic(&u);
  ins->block = ins->walk_mark = 0;

  uint64_t next = va + len;
  int next_in_range = (next < f->base + f->size);

  switch (ins->mn) {
  case UD_Inop:
    ins->kind = CFG_IK_NOP;
    ins->transparent = 1;
    if (!next_in_range) {
      return UD_MATCH_INVALID;
    }
    ins->succ[ins->succ_count++] = next;
    break;

  case UD_Ijmp: {
    const struct ud_operand *t = ud_insn_opr(&u, 0);
    if (u.br_far || t == NULL ||
        (t->type != UD_OP_JIMM && t->type != UD_OP_REG &&
         t->type != UD_OP_MEM)) {
      return UD_MATCH_UNSUPPORTED;
    }
    if (t->type != UD_OP_JIMM) {
      return UD_MATCH_UNSUPPORTED; /* indirect transfer: do not guess */
    }
    ins->kind = CFG_IK_JMP;
    ins->transparent = 1;
    {
      uint64_t tgt;
      if (addr_add(next, imm_value(t), &tgt) != 0) {
        return UD_MATCH_INVALID;
      }
      ins->succ[ins->succ_count++] = tgt;
    }
    break;
  }

  case UD_Icall: {
    const struct ud_operand *t = ud_insn_opr(&u, 0);
    if (u.br_far || t == NULL) {
      return UD_MATCH_UNSUPPORTED;
    }
    if (t->type != UD_OP_JIMM) {
      return UD_MATCH_UNSUPPORTED;
    }
    ins->kind = CFG_IK_CALL;
    {
      uint64_t tgt;
      if (addr_add(next, imm_value(t), &tgt) != 0) {
        return UD_MATCH_INVALID;
      }
      ins->call_target = tgt;
    }
    /* Calls are assumed to return; flow continues at the return site.  The
     * callee itself is never entered. */
    if (!next_in_range) {
      return UD_MATCH_INVALID;
    }
    ins->succ[ins->succ_count++] = next;
    break;
  }

  case UD_Iret:
    if (u.br_far || ud_insn_opr(&u, 0) != NULL) {
      return UD_MATCH_UNSUPPORTED; /* far return or RET imm16 */
    }
    ins->kind = CFG_IK_RET;
    break;

  case UD_Iretf:
  case UD_Iloop: case UD_Iloope: case UD_Iloopne:
  case UD_Ijcxz: case UD_Ijecxz: case UD_Ijrcxz:
    return UD_MATCH_UNSUPPORTED;

  default:
    if (is_jcc(ins->mn)) {
      const struct ud_operand *t = ud_insn_opr(&u, 0);
      if (t == NULL || t->type != UD_OP_JIMM) {
        return UD_MATCH_UNSUPPORTED;
      }
      ins->kind = CFG_IK_JCC;
      {
        uint64_t tgt;
        if (addr_add(next, imm_value(t), &tgt) != 0) {
          return UD_MATCH_INVALID;
        }
        ins->succ[ins->succ_count++] = tgt; /* taken */
      }
      if (!next_in_range) {
        return UD_MATCH_INVALID;
      }
      ins->succ[ins->succ_count++] = next;  /* fall-through */
    } else {
      st = classify_data(ins, &u);
      if (st != UD_MATCH_OK) {
        return st;
      }
      ins->kind = CFG_IK_PLAIN;
      if (!next_in_range) {
        return UD_MATCH_INVALID;
      }
      ins->succ[ins->succ_count++] = next;
    }
    break;
  }

  for (unsigned i = 0; i < ins->succ_count; i++) {
    st = push_target(d, ins->succ[i]);
    if (st != UD_MATCH_OK) {
      return st;
    }
  }
  return UD_MATCH_OK;
}

static int
cmp_insn_va(const void *a, const void *b)
{
  const struct cfg_insn *ia = a, *ib = b;
  if (ia->va < ib->va) return -1;
  if (ia->va > ib->va) return 1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* transparent chain folding                                           */
/* ------------------------------------------------------------------ */

unsigned
cfg_resolve_retained(struct cfg_function *f, uint64_t va)
{
  unsigned gen = ++f->walk_gen;
  for (;;) {
    struct cfg_insn *in = find_insn(f, va);
    if (in == NULL) {
      return ~0u;
    }
    if (!in->transparent) {
      for (size_t i = 0; i < f->retained_count; i++) {
        if (f->insns[f->retained[i]].va == va) {
          return (unsigned)i;
        }
      }
      return ~0u;
    }
    if (in->walk_mark == gen) {
      return ~0u; /* transparent-only cycle */
    }
    in->walk_mark = gen;
    va = in->succ[0];
  }
}

/* ------------------------------------------------------------------ */
/* block graph construction                                            */
/* ------------------------------------------------------------------ */

static enum ud_match_status
build_blocks(struct cfg_function *f)
{
  size_t n = f->retained_count;
  unsigned char *leader;
  unsigned *ri_to_block;
  unsigned (*folded)[2];
  unsigned char *folded_count;
  struct cfg_block *blocks;
  size_t cap = n ? n : 1;

  if (n == 0) {
    return UD_MATCH_UNSUPPORTED;
  }
  leader       = calloc(cap, 1);
  ri_to_block  = malloc(cap * sizeof(*ri_to_block));
  folded       = malloc(cap * sizeof(*folded));
  folded_count = calloc(cap, 1);
  blocks       = malloc(cap * sizeof(*blocks));
  if (leader == NULL || ri_to_block == NULL || folded == NULL ||
      folded_count == NULL || blocks == NULL) {
    free(leader); free(ri_to_block); free(folded);
    free(folded_count); free(blocks);
    return UD_MATCH_INVALID;
  }

  /* Folded successors of every retained instruction (transparent NOP/JMP
   * chains resolved to retained indices). */
  for (size_t i = 0; i < n; i++) {
    struct cfg_insn *in = &f->insns[f->retained[i]];
    folded_count[i] = 0;
    for (unsigned s = 0; s < in->succ_count; s++) {
      unsigned t = cfg_resolve_retained(f, in->succ[s]);
      if (t == ~0u) {
        free(leader); free(ri_to_block); free(folded);
        free(folded_count); free(blocks);
        return UD_MATCH_UNSUPPORTED;
      }
      folded[i][folded_count[i]++] = t;
    }
  }

  /* Leaders: the entry; the instruction after every terminator; every folded
   * target that leaves the sequential stream (jump targets, and plain flow
   * redirected through a transparent JMP).  Convergence points are leaders by
   * construction since a non-sequential edge has to reach them. */
  leader[0] = 1;
  for (size_t i = 0; i < n; i++) {
    struct cfg_insn *in = &f->insns[f->retained[i]];
    if (in->kind == CFG_IK_JCC || in->kind == CFG_IK_CALL ||
        in->kind == CFG_IK_RET) {
      if (i + 1 < n) {
        leader[i + 1] = 1;
      }
    }
    for (unsigned s = 0; s < folded_count[i]; s++) {
      unsigned t = folded[i][s];
      if (t != i + 1) {
        leader[t] = 1;
      }
    }
  }

  /* Partition the retained stream into blocks. */
  size_t bcount = 0;
  size_t start = 0;
  for (size_t i = 1; i <= n; i++) {
    if (i == n || leader[i]) {
      struct cfg_block *b = &blocks[bcount];
      memset(b, 0, sizeof(*b));
      b->entry_va = f->insns[f->retained[start]].va;
      b->first = (unsigned)start;
      b->last  = (unsigned)(i - 1);
      b->succ[0] = b->succ[1] = ~0u;
      for (size_t k = start; k < i; k++) {
        ri_to_block[k] = (unsigned)bcount;
        f->insns[f->retained[k]].block = (unsigned)bcount;
      }
      struct cfg_insn *term = &f->insns[f->retained[i - 1]];
      b->is_jcc  = term->kind == CFG_IK_JCC;
      b->is_call = term->kind == CFG_IK_CALL;
      b->is_ret  = term->kind == CFG_IK_RET;
      bcount++;
      start = i;
    }
  }

  /* Edges, derived from the terminator's folded successors. */
  for (size_t bi = 0; bi < bcount; bi++) {
    struct cfg_block *b = &blocks[bi];
    unsigned ti = b->last;
    b->succ_count = folded_count[ti];
    for (unsigned s = 0; s < folded_count[ti]; s++) {
      b->succ[s] = ri_to_block[folded[ti][s]];
    }
    b->pred_count = 0;
  }
  for (size_t bi = 0; bi < bcount; bi++) {
    for (unsigned s = 0; s < blocks[bi].succ_count; s++) {
      blocks[blocks[bi].succ[s]].pred_count++;
    }
  }

  /* Merge straight single-pred/single-succ blocks.  Never cross CALL/Jcc/RET
   * and never swallow a non-contiguous region (a shared convergence or a loop
   * body must stay its own node). */
  int changed = 1;
  while (changed) {
    changed = 0;
    for (size_t bi = 0; bi < bcount; bi++) {
      struct cfg_block *a = &blocks[bi];
      if (a->dead || a->succ_count != 1 || a->is_jcc || a->is_call ||
          a->is_ret) {
        continue;
      }
      struct cfg_block *b = &blocks[a->succ[0]];
      if (b->dead || b == a || b->pred_count != 1 ||
          b->is_jcc || b->is_call || b->is_ret) {
        continue;
      }
      if (b->first != a->last + 1) {
        continue;
      }
      a->last = b->last;
      a->succ[0] = b->succ[0];
      a->succ[1] = b->succ[1];
      a->succ_count = b->succ_count;
      a->is_jcc = b->is_jcc;
      a->is_call = b->is_call;
      a->is_ret = b->is_ret;
      for (unsigned k = b->first; k <= b->last; k++) {
        f->insns[f->retained[k]].block = (unsigned)bi;
      }
      b->dead = 1;
      changed = 1;
      break;
    }
  }

  /* Compact with an explicit old-to-new remap. */
  size_t live = 0;
  for (size_t bi = 0; bi < bcount; bi++) {
    if (blocks[bi].dead) {
      ri_to_block[bi] = ~0u;
    } else {
      ri_to_block[bi] = (unsigned)live;
      if (live != bi) {
        blocks[live] = blocks[bi];
      }
      live++;
    }
  }
  for (size_t bi = 0; bi < live; bi++) {
    for (unsigned s = 0; s < blocks[bi].succ_count; s++) {
      blocks[bi].succ[s] = ri_to_block[blocks[bi].succ[s]];
    }
    for (unsigned k = blocks[bi].first; k <= blocks[bi].last; k++) {
      f->insns[f->retained[k]].block = (unsigned)bi;
    }
  }

  f->blocks = blocks;
  f->block_count = live;
  f->block_cap = bcount;
  free(leader);
  free(folded);
  free(folded_count);
  free(ri_to_block);
  return UD_MATCH_OK;
}

/* ------------------------------------------------------------------ */
/* public entry                                                        */
/* ------------------------------------------------------------------ */

enum ud_match_status
cfg_normalize(const struct ud_function_bytes *fb,
              struct cfg_function **out_function)
{
  struct cfg_decoder d;
  struct cfg_function *f;
  enum ud_match_status st;

  *out_function = NULL;
  if (fb == NULL || fb->data == NULL || fb->size == 0) {
    return UD_MATCH_INVALID;
  }
  if (fb->entry < fb->base ||
      fb->entry >= fb->base + fb->size ||
      fb->base + fb->size < fb->base) {
    return UD_MATCH_INVALID;
  }

  f = calloc(1, sizeof(*f));
  if (f == NULL) {
    return UD_MATCH_INVALID;
  }
  f->base = fb->base;
  f->size = fb->size;
  f->entry = fb->entry;
  f->status = UD_MATCH_OK;

  memset(&d, 0, sizeof(d));
  d.f = f;
  d.data = fb->data;
  st = push_target(&d, fb->entry);
  while (st == UD_MATCH_OK && d.worklist_count > 0) {
    uint64_t va = d.worklist[--d.worklist_count];
    if (find_insn(f, va) != NULL) {
      continue;
    }
    st = decode_one(&d, va);
  }
  free(d.worklist);
  if (st != UD_MATCH_OK) {
    cfg_function_free(f);
    return st;
  }

  qsort(f->insns, f->insn_count, sizeof(*f->insns), cmp_insn_va);

  f->retained = malloc(f->insn_count * sizeof(*f->retained));
  if (f->retained == NULL) {
    cfg_function_free(f);
    return UD_MATCH_INVALID;
  }
  for (size_t i = 0; i < f->insn_count; i++) {
    if (!f->insns[i].transparent) {
      f->retained[f->retained_count++] = (unsigned)i;
    }
  }

  /* Reject transparent-only cycles anywhere a control edge can land. */
  for (size_t i = 0; i < f->insn_count; i++) {
    struct cfg_insn *in = &f->insns[i];
    for (unsigned s = 0; s < in->succ_count; s++) {
      if (cfg_resolve_retained(f, in->succ[s]) == ~0u) {
        cfg_function_free(f);
        return UD_MATCH_UNSUPPORTED;
      }
    }
  }

  st = build_blocks(f);
  if (st != UD_MATCH_OK) {
    cfg_function_free(f);
    return st;
  }

  *out_function = f;
  return UD_MATCH_OK;
}

void
cfg_function_free(struct cfg_function *f)
{
  if (f == NULL) {
    return;
  }
  free(f->insns);
  free(f->retained);
  free(f->blocks);
  free(f);
}
