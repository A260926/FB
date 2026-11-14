/* udis86 - libudis86/cfg_match.c
 *
 * Structural matching of two normalized x86-64 functions.
 *
 * The search is an explicit backtracking isomorphism over the reduced block
 * graphs: blocks are paired in a deterministic order, each pair carrying its
 * retained-instruction correspondence (found by the in-block scheduling
 * solver) and extensions to the global GPR-family bijection.  A local failure
 * only prunes that branch; alternatives are tried before declaring
 * UD_MATCH_NO_MATCH.
 */
#include "cfg_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Jcc complement tables                                               */
/* ------------------------------------------------------------------ */

unsigned
cfg_jcc_flags(enum ud_mnemonic_code mn)
{
  switch (mn) {
  case UD_Ijo: case UD_Ijno:
    return CGF_OF;
  case UD_Ijb: case UD_Ijae:
    return CGF_CF;
  case UD_Ijz: case UD_Ijnz:
    return CGF_ZF;
  case UD_Ijbe: case UD_Ija:
    return CGF_CF | CGF_ZF;
  case UD_Ijs: case UD_Ijns:
    return CGF_SF;
  case UD_Ijp: case UD_Ijnp:
    return CGF_PF;
  case UD_Ijl: case UD_Ijge:
    return CGF_SF | CGF_OF;
  case UD_Ijle: case UD_Ijg:
    return CGF_ZF | CGF_SF | CGF_OF;
  default:
    return 0;
  }
}

int
cfg_jcc_is_complement(enum ud_mnemonic_code a, enum ud_mnemonic_code b)
{
  enum ud_mnemonic_code p;
  switch (a) {
  case UD_Ijo:  p = UD_Ijno; break;
  case UD_Ijno: p = UD_Ijo;  break;
  case UD_Ijb:  p = UD_Ijae; break;
  case UD_Ijae: p = UD_Ijb;  break;
  case UD_Ijz:  p = UD_Ijnz; break;
  case UD_Ijnz: p = UD_Ijz;  break;
  case UD_Ijbe: p = UD_Ija;  break;
  case UD_Ija:  p = UD_Ijbe; break;
  case UD_Ijs:  p = UD_Ijns; break;
  case UD_Ijns: p = UD_Ijs;  break;
  case UD_Ijp:  p = UD_Ijnp; break;
  case UD_Ijnp: p = UD_Ijp;  break;
  case UD_Ijl:  p = UD_Ijge; break;
  case UD_Ijge: p = UD_Ijl;  break;
  case UD_Ijle: p = UD_Ijg;  break;
  case UD_Ijg:  p = UD_Ijle; break;
  default: return 0;
  }
  return p == b;
}

/* ------------------------------------------------------------------ */
/* anchors                                                             */
/* ------------------------------------------------------------------ */

struct anchor_table
{
  const struct ud_addr_anchor *a;
  size_t count;
};

static enum ud_match_status
build_anchors(const struct ud_addr_anchor *in, size_t count,
              struct anchor_table *out)
{
  out->a = in;
  out->count = count;
  for (size_t i = 0; i < count; i++) {
    for (size_t j = i + 1; j < count; j++) {
      if (in[i].left_va == in[j].left_va ||
          in[i].right_va == in[j].right_va) {
        return UD_MATCH_INVALID;
      }
    }
  }
  return UD_MATCH_OK;
}

/* Reference addresses (RIP EAs, direct CALL targets) match by anchor pair or
 * by numeric equality.  Plain immediates never go through here. */
static int
ref_matches(const struct anchor_table *an, uint64_t lva, uint64_t rva)
{
  const struct ud_addr_anchor *hit_l = NULL, *hit_r = NULL;
  for (size_t i = 0; i < an->count; i++) {
    if (an->a[i].left_va == lva) {
      hit_l = &an->a[i];
    }
    if (an->a[i].right_va == rva) {
      hit_r = &an->a[i];
    }
  }
  if (hit_l != NULL) {
    return hit_l->right_va == rva;
  }
  if (hit_r != NULL) {
    return hit_r->left_va == lva;
  }
  return lva == rva;
}

/* ------------------------------------------------------------------ */
/* matching state                                                      */
/* ------------------------------------------------------------------ */

struct match_ctx
{
  struct cfg_function *L;
  struct cfg_function *R;
  struct anchor_table anchors;

  int map[UD_MATCH_FAMILY_COUNT];       /* left family -> right family */
  int inv[UD_MATCH_FAMILY_COUNT];       /* right family -> left family */
  int blk_map[64];                      /* left block -> right block */
  int blk_inv[64];                      /* right block -> left block */
  int swap[64];                         /* swapped-edge marker per L block */

  unsigned insn_pair_l[512];            /* retained index, indexed by L ri */
  unsigned insn_pair_r[512];            /* retained index, indexed by R ri */
};

static const struct cfg_insn *
ret_insn(const struct cfg_function *f, unsigned ri)
{
  return &f->insns[f->retained[ri]];
}

static int
bind_family(struct match_ctx *cx, unsigned lf, unsigned rf,
            unsigned *bound, unsigned *bound_count)
{
  if (lf >= UD_MATCH_FAMILY_COUNT || rf >= UD_MATCH_FAMILY_COUNT) {
    return 0;
  }
  /* RSP and RBP always map to themselves. */
  if ((lf == 4 && rf != 4) || (lf == 5 && rf != 5) ||
      (rf == 4 && lf != 4) || (rf == 5 && lf != 5)) {
    return 0;
  }
  if (cx->map[lf] != -1) {
    return cx->map[lf] == (int)rf;
  }
  if (cx->inv[rf] != -1) {
    return 0; /* two families may not collapse onto one */
  }
  cx->map[lf] = (int)rf;
  cx->inv[rf] = (int)lf;
  bound[(*bound_count)++] = lf;
  return 1;
}

static void
rollback_families(struct match_ctx *cx, unsigned *bound, unsigned count)
{
  for (unsigned i = 0; i < count; i++) {
    unsigned lf = bound[i];
    unsigned rf = (unsigned)cx->map[lf];
    cx->inv[rf] = -1;
    cx->map[lf] = -1;
  }
}

/* ------------------------------------------------------------------ */
/* operand / instruction compatibility                                 */
/* ------------------------------------------------------------------ */

static int
mem_matches(struct match_ctx *cx, const struct cfg_mem *lm,
            const struct cfg_mem *rm, unsigned *bound,
            unsigned *bound_count)
{
  if (lm->width != rm->width ||
      lm->has_base != rm->has_base ||
      lm->has_index != rm->has_index ||
      lm->is_rip != rm->is_rip) {
    return 0;
  }
  if (lm->has_base &&
      !bind_family(cx, lm->base_family, rm->base_family, bound,
                   bound_count)) {
    return 0;
  }
  if (lm->has_index) {
    if (lm->scale != rm->scale ||
        !bind_family(cx, lm->index_family, rm->index_family, bound,
                     bound_count)) {
      return 0;
    }
  }
  if (lm->is_rip) {
    /* Absolute RIP effective address, anchor or equality. */
    if (!ref_matches(&cx->anchors, lm->rip_ea, rm->rip_ea)) {
      return 0;
    }
  } else if (lm->disp != rm->disp) {
    return 0;
  }
  return 1;
}

static int
insn_matches(struct match_ctx *cx, const struct cfg_insn *li,
             const struct cfg_insn *ri, unsigned *bound,
             unsigned *bound_count)
{
  if (li->mn != ri->mn || li->op_count != ri->op_count) {
    return 0;
  }
  for (unsigned i = 0; i < li->op_count; i++) {
    const struct cfg_op *lo = &li->op[i];
    const struct cfg_op *ro = &ri->op[i];
    if (lo->kind != ro->kind) {
      return 0;
    }
    switch (lo->kind) {
    case CFG_OP_REG:
      if (lo->reg.width != ro->reg.width || lo->reg.high != ro->reg.high) {
        return 0;
      }
      if (!bind_family(cx, lo->reg.family, ro->reg.family, bound,
                       bound_count)) {
        return 0;
      }
      break;
    case CFG_OP_IMM:
      /* Constants are never ignored; width-normalized sign extension already
       * happened at decode classification time. */
      if (lo->imm != ro->imm) {
        return 0;
      }
      break;
    case CFG_OP_MEM:
      if (!mem_matches(cx, &lo->mem, &ro->mem, bound, bound_count)) {
        return 0;
      }
      break;
    default:
      return 0;
    }
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* dependence-aware scheduling within one block                        */
/* ------------------------------------------------------------------ */

/* Segment masks over the 64-bit family: bit0 = low 8, bit1 = high 8,
 * bit2 = 16..31, bit3 = 32..63. */
#define SEG_LOW8  (1u << 0)
#define SEG_HIGH8 (1u << 1)
#define SEG_1631  (1u << 2)
#define SEG_3263  (1u << 3)
#define SEG_ALL   (SEG_LOW8 | SEG_HIGH8 | SEG_1631 | SEG_3263)

static unsigned
view_seg_mask(const struct cfg_reg_view *v)
{
  switch (v->width) {
  case 8:  return v->high ? SEG_HIGH8 : SEG_LOW8;
  case 16: return SEG_LOW8 | SEG_HIGH8;
  case 32: return SEG_LOW8 | SEG_HIGH8 | SEG_1631; /* clears high 32 */
  default: return SEG_LOW8 | SEG_HIGH8 | SEG_1631 | SEG_3263;
  }
}

static void
reg_access_masks(const struct cfg_insn *in, unsigned writes[16],
                 unsigned reads[16])
{
  for (unsigned i = 0; i < in->op_count; i++) {
    const struct cfg_op *o = &in->op[i];
    if (o->kind == CFG_OP_REG) {
      unsigned fam = o->reg.family;
      unsigned seg = view_seg_mask(&o->reg);
      /* Position 0 is the destination for every supported two-operand form;
       * CMP/TEST only read, MOVZX/MOVSX read the source view and write the
       * destination view. */
      int is_dest = (i == 0);
      if (is_dest && (in->mn == UD_Icmp || in->mn == UD_Itest)) {
        is_dest = 0;
      }
      if (is_dest) {
        writes[fam] |= seg;
      } else {
        reads[fam] |= seg;
      }
      /* RMW arithmetic reads the old destination value before writing it. */
      if (is_dest) {
        switch (in->mn) {
        case UD_Iadd: case UD_Iadc: case UD_Isub: case UD_Isbb:
        case UD_Iand: case UD_Ior:  case UD_Ixor:
        case UD_Iinc: case UD_Idec:
          reads[fam] |= seg;
          break;
        default:
          break;
        }
      }
    } else if (o->kind == CFG_OP_MEM) {
      /* Address registers are always read, LEA included. */
      if (o->mem.has_base) {
        reads[o->mem.base_family] |= SEG_ALL;
      }
      if (o->mem.has_index) {
        reads[o->mem.index_family] |= SEG_ALL;
      }
    }
  }
}

/* True when x before y must stay in that order on this side. */
static int
conflict_on_side(const struct cfg_insn *x, const struct cfg_insn *y)
{
  unsigned wx[16] = {0}, rx[16] = {0}, wy[16] = {0}, ry[16] = {0};
  reg_access_masks(x, wx, rx);
  reg_access_masks(y, wy, ry);
  for (unsigned f = 0; f < 16; f++) {
    if ((wx[f] & (ry[f] | wy[f])) || (rx[f] & wy[f])) {
      return 1;
    }
  }
  if ((x->flags_write & (y->flags_read | y->flags_write)) ||
      (x->flags_read & y->flags_write)) {
    return 1;
  }
  /* All real memory accesses stay ordered, including two pure reads. */
  if ((x->mem_read || x->mem_write) && (y->mem_read || y->mem_write)) {
    return 1;
  }
  return 0;
}

struct sched_ctx
{
  struct match_ctx *cx;
  const struct cfg_function *L;
  const struct cfg_function *R;
  unsigned l_first, r_first, k;   /* k = count of plain (non-terminator) insns */
  unsigned posmap[128];           /* L plain position -> R plain position */
  unsigned char used[128];
};

static int
sched_recurse(struct sched_ctx *sc, unsigned pos)
{
  if (pos == sc->k) {
    return 1;
  }
  unsigned li = sc->l_first + pos;
  const struct cfg_insn *lin = ret_insn(sc->L, li);

  for (unsigned cand = 0; cand < sc->k; cand++) {
    if (sc->used[cand]) {
      continue;
    }
    unsigned ri = sc->r_first + cand;
    const struct cfg_insn *rin = ret_insn(sc->R, ri);

    unsigned bound[32];
    unsigned bound_count = 0;
    if (!insn_matches(sc->cx, lin, rin, bound, &bound_count)) {
      /* insn_matches may have bound operand-0 families before failing on a
       * later operand; those partial bindings must be undone. */
      rollback_families(sc->cx, bound, bound_count);
      continue;
    }

    int ok = 1;
    for (unsigned prev = 0; prev < pos && ok; prev++) {
      unsigned pli = sc->l_first + prev;
      unsigned pri = sc->r_first + sc->posmap[prev];
      /* A conflict that orders the L predecessor before this instruction must
       * appear in the same relative order between their R partners. */
      if (conflict_on_side(ret_insn(sc->L, pli), lin) &&
          sc->posmap[prev] >= cand) {
        ok = 0;
      }
      /* Symmetrically for an ordering required on the R side. */
      if (conflict_on_side(ret_insn(sc->R, pri), rin) &&
          sc->posmap[prev] >= cand) {
        ok = 0;
      }
    }
    if (!ok) {
      rollback_families(sc->cx, bound, bound_count);
      continue;
    }

    sc->used[cand] = 1;
    sc->posmap[pos] = cand;
    if (sched_recurse(sc, pos + 1)) {
      return 1; /* bindings stay committed as part of the witness */
    }
    sc->used[cand] = 0;
    rollback_families(sc->cx, bound, bound_count);
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* block graph isomorphism search                                      */
/* ------------------------------------------------------------------ */

static int
terminator_compatible(struct match_ctx *cx, const struct cfg_block *lb,
                      const struct cfg_block *rb, int *swapped_out)
{
  const struct cfg_insn *lt = ret_insn(cx->L, lb->last);
  const struct cfg_insn *rt = ret_insn(cx->R, rb->last);

  if (lb->is_ret != rb->is_ret || lb->is_call != rb->is_call ||
      lb->is_jcc != rb->is_jcc) {
    return 0;
  }
  if (lb->succ_count != rb->succ_count) {
    return 0;
  }
  if (lb->is_jcc) {
    if (lt->mn == rt->mn) {
      *swapped_out = 0;
    } else if (cfg_jcc_is_complement(lt->mn, rt->mn)) {
      *swapped_out = 1;
    } else {
      return 0; /* different, non-complementary conditions never pair */
    }
  } else {
    *swapped_out = 0;
  }
  if (lb->is_call &&
      !ref_matches(&cx->anchors, lt->call_target, rt->call_target)) {
    return 0;
  }
  return 1;
}

/* Records the block-edge constraints implied by a tentative block pair. */
static int
constrain_edges(struct match_ctx *cx, unsigned lb_idx, unsigned rb_idx,
                int swapped, unsigned *constrained,
                unsigned *constrained_count, unsigned *rev_constrained,
                unsigned *rev_count)
{
  const struct cfg_block *lb = &cx->L->blocks[lb_idx];
  const struct cfg_block *rb = &cx->R->blocks[rb_idx];

  for (unsigned s = 0; s < lb->succ_count; s++) {
    unsigned lt = lb->succ[s];
    unsigned r_slot = swapped ? (1 - s) : s;
    unsigned rt = rb->succ[r_slot];
    if (cx->blk_map[lt] != -1) {
      if ((unsigned)cx->blk_map[lt] != rt) {
        return 0;
      }
    } else {
      if (cx->blk_inv[rt] != -1) {
        return 0;
      }
      cx->blk_map[lt] = (int)rt;
      cx->blk_inv[rt] = (int)lt;
      constrained[(*constrained_count)++] = lt;
      rev_constrained[(*rev_count)++] = rt;
    }
  }
  return 1;
}

static int
match_blocks(struct match_ctx *cx, unsigned depth);

static int
try_block_pair(struct match_ctx *cx, unsigned lb_idx, unsigned rb_idx)
{
  const struct cfg_function *L = cx->L, *R = cx->R;
  const struct cfg_block *lb = &L->blocks[lb_idx];
  const struct cfg_block *rb = &R->blocks[rb_idx];
  int swapped;
  int has_term;
  unsigned l_plain, r_plain;

  if ((lb->last - lb->first) != (rb->last - rb->first)) {
    return 0;
  }
  if (!terminator_compatible(cx, lb, rb, &swapped)) {
    return 0;
  }
  has_term = lb->is_jcc || lb->is_call || lb->is_ret;
  l_plain = lb->last - lb->first + (has_term ? 0 : 1);
  r_plain = rb->last - rb->first + (has_term ? 0 : 1);
  if (l_plain != r_plain) {
    return 0;
  }
  if (cx->blk_map[lb_idx] != -1 && (unsigned)cx->blk_map[lb_idx] != rb_idx) {
    return 0;
  }
  if (cx->blk_inv[rb_idx] != -1 &&
      (unsigned)cx->blk_inv[rb_idx] != lb_idx) {
    return 0;
  }

  /* In-block scheduling with global register constraints.  Only the plain
   * (non-control) instructions of a block may be reordered; CALL/Jcc/RET are
   * fixed block-end nodes.  A block redirected through a folded transparent
   * JMP has no terminator and all of its instructions are schedulable. */
  struct sched_ctx sc;
  memset(&sc, 0, sizeof(sc));
  sc.cx = cx;
  sc.L = L;
  sc.R = R;
  sc.l_first = lb->first;
  sc.r_first = rb->first;
  sc.k = l_plain;
  if (sc.k > 128) {
    return 0;
  }

  unsigned fam_snapshot[16];
  unsigned fam_count = 0;
  for (unsigned f = 0; f < 16; f++) {
    if (cx->map[f] != -1) {
      fam_snapshot[fam_count++] = f;
    }
  }

  if (!sched_recurse(&sc, 0)) {
    return 0;
  }

  /* Commit block pair and edge constraints. */
  int newly_paired = (cx->blk_map[lb_idx] == -1);
  unsigned constrained[64], rev_constrained[64];
  unsigned constrained_count = 0, rev_count = 0;
  if (newly_paired) {
    cx->blk_map[lb_idx] = (int)rb_idx;
    cx->blk_inv[rb_idx] = (int)lb_idx;
  }
  if (!constrain_edges(cx, lb_idx, rb_idx, swapped, constrained,
                       &constrained_count, rev_constrained, &rev_count)) {
    for (unsigned i = 0; i < constrained_count; i++) {
      cx->blk_map[constrained[i]] = -1;
    }
    for (unsigned i = 0; i < rev_count; i++) {
      cx->blk_inv[rev_constrained[i]] = -1;
    }
    if (newly_paired) {
      cx->blk_map[lb_idx] = -1;
      cx->blk_inv[rb_idx] = -1;
    }
    goto fail_families;
  }
  cx->swap[lb_idx] = swapped;

  /* Remember the instruction correspondence: plain prefix plus the fixed
   * terminator slot when the block has one. */
  for (unsigned p = 0; p < sc.k; p++) {
    cx->insn_pair_l[sc.l_first + p] = sc.r_first + sc.posmap[p];
    cx->insn_pair_r[sc.r_first + sc.posmap[p]] = sc.l_first + p;
  }
  if (has_term) {
    cx->insn_pair_l[lb->last] = rb->last;
    cx->insn_pair_r[rb->last] = lb->last;
  }

  if (match_blocks(cx, lb_idx + 1)) {
    return 1;
  }

  /* Roll back. */
  for (unsigned p = 0; p < sc.k; p++) {
    cx->insn_pair_l[sc.l_first + p] = ~0u;
    cx->insn_pair_r[sc.r_first + sc.posmap[p]] = ~0u;
  }
  if (has_term) {
    cx->insn_pair_l[lb->last] = ~0u;
    cx->insn_pair_r[rb->last] = ~0u;
  }
  for (unsigned i = 0; i < constrained_count; i++) {
    cx->blk_map[constrained[i]] = -1;
  }
  for (unsigned i = 0; i < rev_count; i++) {
    cx->blk_inv[rev_constrained[i]] = -1;
  }
  if (newly_paired) {
    cx->blk_map[lb_idx] = -1;
    cx->blk_inv[rb_idx] = -1;
  }
fail_families:
  /* Roll back families newly bound inside this attempt. */
  for (unsigned f = 0; f < 16; f++) {
    int pre_bound = 0;
    for (unsigned i = 0; i < fam_count; i++) {
      if (fam_snapshot[i] == f) {
        pre_bound = 1;
      }
    }
    if (!pre_bound && cx->map[f] != -1) {
      unsigned rf = (unsigned)cx->map[f];
      cx->inv[rf] = -1;
      cx->map[f] = -1;
    }
  }
  return 0;
}

static int
match_blocks(struct match_ctx *cx, unsigned depth)
{
  if (depth == cx->L->block_count) {
    for (unsigned b = 0; b < cx->R->block_count; b++) {
      if (cx->blk_inv[b] == -1) {
        return 0;
      }
    }
    return 1;
  }
  if (cx->blk_map[depth] != -1) {
    /* Edge constraints already fixed this block's partner. */
    return try_block_pair(cx, depth, (unsigned)cx->blk_map[depth]);
  }
  for (unsigned rb = 0; rb < cx->R->block_count; rb++) {
    if (cx->blk_inv[rb] != -1) {
      continue;
    }
    /* The two entry blocks must correspond. */
    if (depth == 0 && rb != 0) {
      continue;
    }
    if (try_block_pair(cx, depth, rb)) {
      return 1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* result construction                                                 */
/* ------------------------------------------------------------------ */

static struct ud_match_result *
build_result(struct match_ctx *cx)
{
  const struct cfg_function *L = cx->L, *R = cx->R;
  struct ud_match_result *res = calloc(1, sizeof(*res));
  if (res == NULL) {
    return NULL;
  }
  res->status = UD_MATCH_OK;

  res->insn_count = L->retained_count;
  res->insns = calloc(res->insn_count ? res->insn_count : 1,
                      sizeof(*res->insns));
  res->block_count = L->block_count;
  res->blocks = calloc(res->block_count ? res->block_count : 1,
                       sizeof(*res->blocks));
  size_t edge_count = 0;
  for (size_t b = 0; b < L->block_count; b++) {
    edge_count += L->blocks[b].succ_count;
  }
  res->edge_count = edge_count;
  res->edges = calloc(edge_count ? edge_count : 1, sizeof(*res->edges));

  size_t reg_count = 0;
  for (unsigned f = 0; f < UD_MATCH_FAMILY_COUNT; f++) {
    if (cx->map[f] != -1) {
      reg_count++;
    }
  }
  res->register_count = reg_count;
  res->registers = calloc(reg_count ? reg_count : 1,
                          sizeof(*res->registers));
  if (res->insns == NULL || res->blocks == NULL || res->edges == NULL ||
      res->registers == NULL) {
    ud_match_result_free(res);
    return NULL;
  }

  for (size_t i = 0; i < L->retained_count; i++) {
    unsigned rri = cx->insn_pair_l[i];
    res->insns[i].left_va = ret_insn(L, (unsigned)i)->va;
    res->insns[i].right_va = ret_insn(R, rri)->va;
  }
  for (size_t b = 0; b < L->block_count; b++) {
    unsigned rb = (unsigned)cx->blk_map[b];
    res->blocks[b].left_entry_va = L->blocks[b].entry_va;
    res->blocks[b].right_entry_va = R->blocks[rb].entry_va;
  }
  size_t e = 0;
  for (size_t b = 0; b < L->block_count; b++) {
    unsigned rb = (unsigned)cx->blk_map[b];
    for (unsigned s = 0; s < L->blocks[b].succ_count; s++) {
      unsigned r_slot = cx->swap[b] ? (1 - s) : s;
      unsigned lt = L->blocks[b].succ[s];
      unsigned rt = R->blocks[rb].succ[r_slot];
      res->edges[e].left_from = L->blocks[b].entry_va;
      res->edges[e].left_to = L->blocks[lt].entry_va;
      res->edges[e].right_from = R->blocks[rb].entry_va;
      res->edges[e].right_to = R->blocks[rt].entry_va;
      res->edges[e].swapped = cx->swap[b];
      e++;
    }
  }
  size_t r = 0;
  for (unsigned f = 0; f < UD_MATCH_FAMILY_COUNT; f++) {
    if (cx->map[f] != -1) {
      res->registers[r].left_family = f;
      res->registers[r].right_family = (unsigned)cx->map[f];
      r++;
    }
  }
  return res;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

const char *
ud_match_family_name(unsigned family)
{
  static const char *names[UD_MATCH_FAMILY_COUNT] = {
    "RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI",
    "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"
  };
  if (family >= UD_MATCH_FAMILY_COUNT) {
    return NULL;
  }
  return names[family];
}

void
ud_match_result_free(struct ud_match_result *result)
{
  if (result == NULL) {
    return;
  }
  free(result->insns);
  free(result->blocks);
  free(result->edges);
  free(result->registers);
  free(result);
}

enum ud_match_status
ud_match_functions64(const struct ud_function_bytes *left,
                     const struct ud_function_bytes *right,
                     const struct ud_addr_anchor *anchors,
                     size_t anchor_count,
                     struct ud_match_result **out_result)
{
  struct cfg_function *fl = NULL, *fr = NULL;
  struct match_ctx cx;
  enum ud_match_status st;

  if (out_result == NULL) {
    return UD_MATCH_INVALID;
  }
  *out_result = NULL;

  st = cfg_normalize(left, &fl);
  if (st != UD_MATCH_OK) {
    return st;
  }
  st = cfg_normalize(right, &fr);
  if (st != UD_MATCH_OK) {
    cfg_function_free(fl);
    return st;
  }

  memset(&cx, 0, sizeof(cx));
  cx.L = fl;
  cx.R = fr;
  for (unsigned f = 0; f < UD_MATCH_FAMILY_COUNT; f++) {
    cx.map[f] = -1;
    cx.inv[f] = -1;
  }
  for (size_t b = 0; b < 64; b++) {
    cx.blk_map[b] = -1;
    cx.blk_inv[b] = -1;
    cx.swap[b] = 0;
  }
  for (size_t i = 0; i < 512; i++) {
    cx.insn_pair_l[i] = ~0u;
    cx.insn_pair_r[i] = ~0u;
  }

  st = build_anchors(anchors, anchor_count, &cx.anchors);
  if (st != UD_MATCH_OK) {
    cfg_function_free(fl);
    cfg_function_free(fr);
    return st;
  }

  if (fl->block_count > 64 || fr->block_count > 64 ||
      fl->retained_count > 512 || fr->retained_count > 512) {
    cfg_function_free(fl);
    cfg_function_free(fr);
    return UD_MATCH_UNSUPPORTED;
  }

  enum ud_match_status result_status;
  if (fl->block_count != fr->block_count ||
      fl->retained_count != fr->retained_count) {
    result_status = UD_MATCH_NO_MATCH;
  } else if (match_blocks(&cx, 0)) {
    struct ud_match_result *res = build_result(&cx);
    if (res == NULL) {
      result_status = UD_MATCH_INVALID;
    } else {
      *out_result = res;
      result_status = UD_MATCH_OK;
    }
  } else {
    result_status = UD_MATCH_NO_MATCH;
  }

  cfg_function_free(fl);
  cfg_function_free(fr);
  return result_status;
}
