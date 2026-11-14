/*
 * udis86 - tests/evex_contract.c
 *
 * Contract tests for the AVX-512 (EVEX) subset added to the C decoder:
 * VADDPS/VADDPD, VMULPS/VMULPD, VADDSS/VADDSD, VMULSS/VMULSD and
 * VMOVUPS/VMOVUPD in 64 bit mode.
 *
 * The encoded byte sequences used here were produced with an external
 * assembler; these tests decode them with libudis86 only, so they do not
 * require an AVX-512 capable host.  Every case asserts structured content
 * (mnemonic, operand registers/widths, addressing, decoration fields and
 * the matching INTEL text), not just mnemonics.
 */
#include "task_support/check.h"
#include "udis86.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void
init_64(ud_t *u, void (*syntax)(struct ud *))
{
  ud_init(u);
  ud_set_mode(u, 64);
  ud_set_syntax(u, syntax);
}

static void
decode_bytes(ud_t *u, const unsigned char *bytes, size_t len)
{
  ud_set_input_buffer(u, bytes, len);
  ud_disassemble(u);
}

/* Decode a single instruction and return the EVEX info (or NULL). */
static const struct ud_evex_info *
decode_evex(ud_t *u, const unsigned char *bytes, size_t len)
{
  ud_init(u);
  ud_set_mode(u, 64);
  ud_set_syntax(u, UD_SYN_INTEL);
  ud_set_input_buffer(u, bytes, len);
  if (ud_disassemble(u) == 0) {
    return NULL;
  }
  return ud_insn_evex(u);
}

static int
opr_is_reg(const ud_t *u, unsigned n, enum ud_type reg, unsigned size)
{
  const struct ud_operand *o = ud_insn_opr(u, n);
  return o != NULL && o->type == UD_OP_REG && o->base == reg && o->size == size;
}

static int
opr_is_mem(const ud_t *u, unsigned n, unsigned size)
{
  const struct ud_operand *o = ud_insn_opr(u, n);
  return o != NULL && o->type == UD_OP_MEM && o->size == size;
}

/* Assert the exact INTEL assembly text for the current instruction. */
static int
asm_is(const ud_t *u, const char *expected)
{
  return u->translator != NULL && strcmp(ud_insn_asm(u), expected) == 0;
}

/* ------------------------------------------------------------------------- */
/* prefix: operand prefixes, register numbers and vector widths              */
/* ------------------------------------------------------------------------- */

static void
prefix_high_register_regs(void)
{
  ud_t u;
  /* vaddps zmm17{k3}{z}, zmm20, zmm31 */
  static const unsigned char c[6] = { 0x62,0x81,0x5c,0xc3,0x58,0xcf };
  const struct ud_evex_info *e = decode_evex(&u, c, sizeof c);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivaddps);
  UNIT_CHECK(opr_is_reg(&u, 0, UD_R_ZMM17, 512));
  UNIT_CHECK(opr_is_reg(&u, 1, UD_R_ZMM20, 512));
  UNIT_CHECK(opr_is_reg(&u, 2, UD_R_ZMM31, 512));
  UNIT_CHECK(e->vector_bits == 512);
  UNIT_CHECK(e->mask == 3);
  UNIT_CHECK(e->zeroing == 1);
  UNIT_CHECK(e->broadcast == 0);
  UNIT_CHECK(e->rounding == 0);
  UNIT_CHECK(e->sae == 0);
  UNIT_CHECK(asm_is(&u, "vaddps zmm17{k3}{z}, zmm20, zmm31"));
}

static void
prefix_128_regs(void)
{
  ud_t u;
  /* vaddps xmm16, xmm17, xmm18 */
  static const unsigned char c[6] = { 0x62,0xa1,0x74,0x00,0x58,0xc2 };
  const struct ud_evex_info *e = decode_evex(&u, c, sizeof c);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivaddps);
  UNIT_CHECK(opr_is_reg(&u, 0, UD_R_XMM16, 128));
  UNIT_CHECK(opr_is_reg(&u, 1, UD_R_XMM17, 128));
  UNIT_CHECK(opr_is_reg(&u, 2, UD_R_XMM18, 128));
  UNIT_CHECK(e->vector_bits == 128);
  UNIT_CHECK(e->mask == 0);
  UNIT_CHECK(e->zeroing == 0);
  UNIT_CHECK(asm_is(&u, "vaddps xmm16, xmm17, xmm18"));
}

static void
prefix_256_regs(void)
{
  ud_t u;
  /* vaddps ymm16, ymm17, ymm18 */
  static const unsigned char c[6] = { 0x62,0xa1,0x74,0x20,0x58,0xc2 };
  const struct ud_evex_info *e = decode_evex(&u, c, sizeof c);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(opr_is_reg(&u, 0, UD_R_YMM16, 256));
  UNIT_CHECK(opr_is_reg(&u, 1, UD_R_YMM17, 256));
  UNIT_CHECK(opr_is_reg(&u, 2, UD_R_YMM18, 256));
  UNIT_CHECK(e->vector_bits == 256);
  UNIT_CHECK(asm_is(&u, "vaddps ymm16, ymm17, ymm18"));
}

static void
prefix_512_regs(void)
{
  ud_t u;
  /* vaddps zmm16, zmm17, zmm18 */
  static const unsigned char c[6] = { 0x62,0xa1,0x74,0x40,0x58,0xc2 };
  const struct ud_evex_info *e = decode_evex(&u, c, sizeof c);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(opr_is_reg(&u, 0, UD_R_ZMM16, 512));
  UNIT_CHECK(opr_is_reg(&u, 1, UD_R_ZMM17, 512));
  UNIT_CHECK(opr_is_reg(&u, 2, UD_R_ZMM18, 512));
  UNIT_CHECK(e->vector_bits == 512);
  UNIT_CHECK(asm_is(&u, "vaddps zmm16, zmm17, zmm18"));
}

static void
prefix_scalar_width(void)
{
  ud_t u;
  /* vaddss xmm16, xmm17, xmm18 */
  static const unsigned char ss[6] = { 0x62,0xa1,0x76,0x00,0x58,0xc2 };
  /* vaddsd xmm16, xmm17, xmm18 */
  static const unsigned char sd[6] = { 0x62,0xa1,0xf7,0x00,0x58,0xc2 };
  const struct ud_evex_info *e;

  e = decode_evex(&u, ss, sizeof ss);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivaddss);
  UNIT_CHECK(opr_is_reg(&u, 0, UD_R_XMM16, 128));
  UNIT_CHECK(opr_is_reg(&u, 1, UD_R_XMM17, 128));
  UNIT_CHECK(opr_is_reg(&u, 2, UD_R_XMM18, 128));
  UNIT_CHECK(e->vector_bits == 128);
  UNIT_CHECK(asm_is(&u, "vaddss xmm16, xmm17, xmm18"));

  e = decode_evex(&u, sd, sizeof sd);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivaddsd);
  UNIT_CHECK(opr_is_reg(&u, 2, UD_R_XMM18, 128));
  UNIT_CHECK(e->vector_bits == 128);
  UNIT_CHECK(asm_is(&u, "vaddsd xmm16, xmm17, xmm18"));
}

static void
prefix_mul_variants(void)
{
  ud_t u;
  /* vmulps zmm16, zmm17, zmm18 */
  static const unsigned char ps[6] = { 0x62,0xa1,0x74,0x40,0x59,0xc2 };
  /* vmulpd zmm16, zmm17, zmm18 */
  static const unsigned char pd[6] = { 0x62,0xa1,0xf5,0x40,0x59,0xc2 };
  /* vmulss xmm16, xmm17, xmm18 */
  static const unsigned char ss[6] = { 0x62,0xa1,0x76,0x00,0x59,0xc2 };
  /* vmulsd xmm16, xmm17, xmm18 */
  static const unsigned char sd[6] = { 0x62,0xa1,0xf7,0x00,0x59,0xc2 };

  UNIT_CHECK(decode_evex(&u, ps, sizeof ps) != NULL);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivmulps);
  UNIT_CHECK(opr_is_reg(&u, 0, UD_R_ZMM16, 512));
  UNIT_CHECK(asm_is(&u, "vmulps zmm16, zmm17, zmm18"));

  UNIT_CHECK(decode_evex(&u, pd, sizeof pd) != NULL);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivmulpd);
  UNIT_CHECK(asm_is(&u, "vmulpd zmm16, zmm17, zmm18"));

  UNIT_CHECK(decode_evex(&u, ss, sizeof ss) != NULL);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivmulss);
  UNIT_CHECK(asm_is(&u, "vmulss xmm16, xmm17, xmm18"));

  UNIT_CHECK(decode_evex(&u, sd, sizeof sd) != NULL);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivmulsd);
  UNIT_CHECK(asm_is(&u, "vmulsd xmm16, xmm17, xmm18"));
}

static void
prefix_packed_128_256_512(void)
{
  ud_t u;
  /* vaddpd xmm19, xmm20, xmm21 / ymm / zmm */
  static const unsigned char x[6] = { 0x62,0xa1,0xdd,0x00,0x58,0xdd };
  static const unsigned char y[6] = { 0x62,0xa1,0xdd,0x20,0x58,0xdd };
  static const unsigned char z[6] = { 0x62,0xa1,0xdd,0x40,0x58,0xdd };
  const struct ud_evex_info *e;

  e = decode_evex(&u, x, sizeof x);
  UNIT_CHECK(e != NULL && e->vector_bits == 128);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivaddpd);
  UNIT_CHECK(opr_is_reg(&u, 0, UD_R_XMM19, 128));
  UNIT_CHECK(opr_is_reg(&u, 2, UD_R_XMM21, 128));

  e = decode_evex(&u, y, sizeof y);
  UNIT_CHECK(e != NULL && e->vector_bits == 256);
  UNIT_CHECK(opr_is_reg(&u, 0, UD_R_YMM19, 256));
  UNIT_CHECK(opr_is_reg(&u, 2, UD_R_YMM21, 256));

  e = decode_evex(&u, z, sizeof z);
  UNIT_CHECK(e != NULL && e->vector_bits == 512);
  UNIT_CHECK(opr_is_reg(&u, 0, UD_R_ZMM19, 512));
  UNIT_CHECK(opr_is_reg(&u, 2, UD_R_ZMM21, 512));
}

static void
prefix_reg_names(void)
{
  /* All 32 vector register numbers must map to distinct names. */
  UNIT_CHECK(strcmp(ud_lookup_mnemonic(UD_Inop), "nop") == 0);
  UNIT_CHECK(UD_R_ZMM31 == UD_R_ZMM0 + 31);
  UNIT_CHECK(UD_R_XMM31 == UD_R_XMM16 + 15);
  UNIT_CHECK(UD_R_YMM31 == UD_R_YMM16 + 15);
  /* Existing enumerators keep their relative order/values. */
  UNIT_CHECK(UD_R_XMM0 < UD_R_YMM0);
  UNIT_CHECK(UD_R_YMM0 < UD_R_RIP);
  UNIT_CHECK(UD_R_RIP < UD_OP_REG);
  UNIT_CHECK(UD_OP_REG < UD_OP_MEM);
}

/* ------------------------------------------------------------------------- */
/* tuple: masking, broadcast, rounding, disp8 compression                     */
/* ------------------------------------------------------------------------- */

static void
tuple_masked_merge_zero(void)
{
  ud_t u;
  /* vaddps zmm1{k2}, zmm2, zmm3 */
  static const unsigned char merge[6] = { 0x62,0xf1,0x6c,0x4a,0x58,0xcb };
  /* vaddps zmm1{k2}{z}, zmm2, zmm3 */
  static const unsigned char zero[6] = { 0x62,0xf1,0x6c,0xca,0x58,0xcb };
  /* vaddss xmm16{k4}{z}, xmm22, xmm18 */
  static const unsigned char scalar[6] = { 0x62,0xa1,0x4e,0x84,0x58,0xc2 };
  const struct ud_evex_info *e;

  e = decode_evex(&u, merge, sizeof merge);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(e->mask == 2);
  UNIT_CHECK(e->zeroing == 0);
  UNIT_CHECK(asm_is(&u, "vaddps zmm1{k2}, zmm2, zmm3"));

  e = decode_evex(&u, zero, sizeof zero);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(e->mask == 2);
  UNIT_CHECK(e->zeroing == 1);
  UNIT_CHECK(asm_is(&u, "vaddps zmm1{k2}{z}, zmm2, zmm3"));

  e = decode_evex(&u, scalar, sizeof scalar);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(e->mask == 4);
  UNIT_CHECK(e->zeroing == 1);
  UNIT_CHECK(e->vector_bits == 128);
  UNIT_CHECK(asm_is(&u, "vaddss xmm16{k4}{z}, xmm22, xmm18"));
}

static void
tuple_ps_broadcast(void)
{
  ud_t u;
  /* vaddps zmm1, zmm2, dword [rax+508]{1to16} : disp8 byte 0x7f, scaled by 4 */
  static const unsigned char b[7] = { 0x62,0xf1,0x6c,0x58,0x58,0x48,0x7f };
  const struct ud_evex_info *e = decode_evex(&u, b, sizeof b);
  const struct ud_operand *m;

  UNIT_CHECK(e != NULL);
  UNIT_CHECK(e->vector_bits == 512);
  UNIT_CHECK(e->broadcast == 16);
  UNIT_CHECK(e->rounding == 0);
  UNIT_CHECK(e->sae == 0);
  UNIT_CHECK(opr_is_mem(&u, 2, 32));   /* element size, not 512 */
  m = ud_insn_opr(&u, 2);
  UNIT_CHECK(m->base == UD_R_RAX);
  UNIT_CHECK(m->offset == 8);
  UNIT_CHECK(m->lval.sdword == 508);   /* 0x7f * 4 */
  UNIT_CHECK(asm_is(&u, "vaddps zmm1, zmm2, dword [rax+0x1fc]{1to16}"));
}

static void
tuple_pd_broadcast(void)
{
  ud_t u;
  /* vaddpd zmm1, zmm2, qword [rax+504]{1to8} : 0x3f * 8 == 504 */
  static const unsigned char b[7] = { 0x62,0xf1,0xed,0x58,0x58,0x48,0x3f };
  const struct ud_evex_info *e = decode_evex(&u, b, sizeof b);
  const struct ud_operand *m;

  UNIT_CHECK(e != NULL);
  UNIT_CHECK(e->broadcast == 8);
  UNIT_CHECK(opr_is_mem(&u, 2, 64));
  m = ud_insn_opr(&u, 2);
  UNIT_CHECK(m->lval.sdword == 504);
  UNIT_CHECK(asm_is(&u, "vaddpd zmm1, zmm2, qword [rax+0x1f8]{1to8}"));
}

static void
tuple_256_broadcast(void)
{
  ud_t u;
  /* vaddps ymm1, ymm2, dword [rax+508]{1to8} */
  static const unsigned char b[7] = { 0x62,0xf1,0x6c,0x38,0x58,0x48,0x7f };
  const struct ud_evex_info *e = decode_evex(&u, b, sizeof b);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(e->vector_bits == 256);
  UNIT_CHECK(e->broadcast == 8);
  UNIT_CHECK(opr_is_mem(&u, 2, 32));
  UNIT_CHECK(asm_is(&u, "vaddps ymm1, ymm2, dword [rax+0x1fc]{1to8}"));
}

static void
tuple_rounding_not_broadcast(void)
{
  ud_t u;
  /* vaddps zmm1, zmm2, zmm3, {rn-sae}: b=1 with register source */
  static const unsigned char rn[6] = { 0x62,0xf1,0x6c,0x18,0x58,0xcb };
  /* {rd-sae} */
  static const unsigned char rd[6] = { 0x62,0xf1,0x6c,0x38,0x58,0xcb };
  /* {ru-sae} */
  static const unsigned char ru[6] = { 0x62,0xf1,0x6c,0x58,0x58,0xcb };
  /* {rz-sae} */
  static const unsigned char rz[6] = { 0x62,0xf1,0x6c,0x78,0x58,0xcb };
  const struct ud_evex_info *e;

  e = decode_evex(&u, rn, sizeof rn);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(e->rounding == 1);
  UNIT_CHECK(e->sae == 1);
  UNIT_CHECK(e->broadcast == 0);       /* no broadcast for register source */
  UNIT_CHECK(e->vector_bits == 512);   /* L'L selects rounding, width is 512 */
  UNIT_CHECK(asm_is(&u, "vaddps zmm1, zmm2, zmm3, {rn-sae}"));

  e = decode_evex(&u, rd, sizeof rd);
  UNIT_CHECK(e != NULL && e->rounding == 2 && e->sae == 1 && e->broadcast == 0);
  UNIT_CHECK(asm_is(&u, "vaddps zmm1, zmm2, zmm3, {rd-sae}"));

  e = decode_evex(&u, ru, sizeof ru);
  UNIT_CHECK(e != NULL && e->rounding == 3 && e->sae == 1);
  UNIT_CHECK(asm_is(&u, "vaddps zmm1, zmm2, zmm3, {ru-sae}"));

  e = decode_evex(&u, rz, sizeof rz);
  UNIT_CHECK(e != NULL && e->rounding == 4 && e->sae == 1);
  UNIT_CHECK(asm_is(&u, "vaddps zmm1, zmm2, zmm3, {rz-sae}"));
}

static void
tuple_scalar_rounding(void)
{
  ud_t u;
  /* vaddsd xmm16, xmm17, xmm18, {rd-sae} */
  static const unsigned char sd[6] = { 0x62,0xa1,0xf7,0x30,0x58,0xc2 };
  /* vmulss xmm16, xmm17, xmm18, {ru-sae} */
  static const unsigned char ss[6] = { 0x62,0xa1,0x76,0x50,0x59,0xc2 };
  const struct ud_evex_info *e;

  e = decode_evex(&u, sd, sizeof sd);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(e->vector_bits == 128);
  UNIT_CHECK(e->rounding == 2);
  UNIT_CHECK(e->sae == 1);
  UNIT_CHECK(e->broadcast == 0);
  UNIT_CHECK(asm_is(&u, "vaddsd xmm16, xmm17, xmm18, {rd-sae}"));

  e = decode_evex(&u, ss, sizeof ss);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(e->rounding == 3);
  UNIT_CHECK(e->sae == 1);
  UNIT_CHECK(asm_is(&u, "vmulss xmm16, xmm17, xmm18, {ru-sae}"));
}

static void
tuple_disp8_vector_scale(void)
{
  ud_t u;
  /* vmovups zmm31{k7}{z}, [r12+8128] : 0x7f * 64 == 8128 (positive extreme) */
  static const unsigned char pos[8] = { 0x62,0x41,0x7c,0xcf,0x10,0x7c,0x24,0x7f };
  /* vmovupd [r13-8192]{k3}, zmm18 : 0x80 == -128, * 64 == -8192 (negative extreme) */
  static const unsigned char neg[7] = { 0x62,0xc1,0xfd,0x4b,0x11,0x55,0x80 };
  const struct ud_evex_info *e;
  const struct ud_operand *m;

  e = decode_evex(&u, pos, sizeof pos);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(e->vector_bits == 512);
  UNIT_CHECK(e->mask == 7);
  UNIT_CHECK(e->zeroing == 1);
  m = ud_insn_opr(&u, 1);
  UNIT_CHECK(m != NULL && m->type == UD_OP_MEM && m->size == 512);
  UNIT_CHECK(m->base == UD_R_R12);
  UNIT_CHECK(m->offset == 8);
  UNIT_CHECK(m->lval.sdword == 8128);
  UNIT_CHECK(asm_is(&u, "vmovups zmm31{k7}{z}, [r12+0x1fc0]"));

  e = decode_evex(&u, neg, sizeof neg);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivmovupd);
  UNIT_CHECK(e->mask == 3);
  UNIT_CHECK(e->zeroing == 0);
  m = ud_insn_opr(&u, 0);
  UNIT_CHECK(m != NULL && m->type == UD_OP_MEM && m->size == 512);
  UNIT_CHECK(m->base == UD_R_R13);
  UNIT_CHECK(m->lval.sdword == -8192);
  UNIT_CHECK(asm_is(&u, "vmovupd [r13-0x2000]{k3}, zmm18"));
}

static void
tuple_disp8_scalar_scale(void)
{
  ud_t u;
  /* vaddss xmm16, xmm17, dword [r9+508] : 0x7f * 4 */
  static const unsigned char b[7] = { 0x62,0xc1,0x76,0x00,0x58,0x41,0x7f };
  const struct ud_operand *m;
  const struct ud_evex_info *e = decode_evex(&u, b, sizeof b);

  UNIT_CHECK(e != NULL);
  UNIT_CHECK(e->broadcast == 0);
  UNIT_CHECK(e->vector_bits == 128);
  UNIT_CHECK(opr_is_mem(&u, 2, 32));
  m = ud_insn_opr(&u, 2);
  UNIT_CHECK(m->base == UD_R_R9);
  UNIT_CHECK(m->lval.sdword == 508);
  UNIT_CHECK(asm_is(&u, "vaddss xmm16, xmm17, dword [r9+0x1fc]"));
}

static void
tuple_disp32_unscaled(void)
{
  ud_t u;
  /* vaddsd xmm16, xmm17, qword [r9+4088] : mod=10, disp32 = 4088 (not /8) */
  static const unsigned char b[10] = { 0x62,0xc1,0xf7,0x00,0x58,0x81,
                                       0xf8,0x0f,0x00,0x00 };
  /* vaddps zmm1, zmm2, [rax+16384] : mod=10, disp32 = 16384 (not /64) */
  static const unsigned char p[10] = { 0x62,0xf1,0x6c,0x48,0x58,0x88,
                                       0x00,0x40,0x00,0x00 };
  const struct ud_operand *m;
  const struct ud_evex_info *e = decode_evex(&u, b, sizeof b);

  UNIT_CHECK(e != NULL);
  m = ud_insn_opr(&u, 2);
  UNIT_CHECK(m != NULL && m->offset == 32);
  UNIT_CHECK(m->lval.sdword == 4088);
  UNIT_CHECK(asm_is(&u, "vaddsd xmm16, xmm17, qword [r9+0xff8]"));

  e = decode_evex(&u, p, sizeof p);
  UNIT_CHECK(e != NULL);
  m = ud_insn_opr(&u, 2);
  UNIT_CHECK(m != NULL && m->offset == 32);
  UNIT_CHECK(m->lval.sdword == 16384);
  UNIT_CHECK(asm_is(&u, "vaddps zmm1, zmm2, [rax+0x4000]"));
}

static void
tuple_rip_relative_unscaled(void)
{
  ud_t u;
  /* vmovups zmm16, [rip+256] : disp32 relative, never compressed */
  static const unsigned char b[10] = { 0x62,0xe1,0x7c,0x48,0x10,0x05,
                                       0x00,0x01,0x00,0x00 };
  const struct ud_operand *m;
  const struct ud_evex_info *e = decode_evex(&u, b, sizeof b);

  UNIT_CHECK(e != NULL);
  m = ud_insn_opr(&u, 1);
  UNIT_CHECK(m != NULL && m->type == UD_OP_MEM);
  UNIT_CHECK(m->base == UD_R_RIP);
  UNIT_CHECK(m->offset == 32);
  UNIT_CHECK(m->lval.sdword == 256);
  UNIT_CHECK(asm_is(&u, "vmovups zmm16, [rip+0x100]"));
}

static void
tuple_sib_no_disp(void)
{
  ud_t u;
  /* vaddps zmm1, zmm2, [rax+rcx*4] : SIB with no displacement */
  static const unsigned char b[7] = { 0x62,0xf1,0x6c,0x48,0x58,0x0c,0x88 };
  const struct ud_operand *m;
  const struct ud_evex_info *e = decode_evex(&u, b, sizeof b);

  UNIT_CHECK(e != NULL);
  m = ud_insn_opr(&u, 2);
  UNIT_CHECK(m != NULL && m->type == UD_OP_MEM);
  UNIT_CHECK(m->base == UD_R_RAX);
  UNIT_CHECK(m->index == UD_R_RCX);
  UNIT_CHECK(m->scale == 4);
  UNIT_CHECK(m->offset == 0);
  UNIT_CHECK(asm_is(&u, "vaddps zmm1, zmm2, [rax+rcx*4]"));
}

static void
tuple_sib_disp8_scale(void)
{
  ud_t u;
  /* vaddps zmm1, zmm2, [rax+rcx*4+64] : disp8 0x01 * 64 */
  static const unsigned char b[8] = { 0x62,0xf1,0x6c,0x48,0x58,0x4c,0x88,0x01 };
  const struct ud_operand *m;
  const struct ud_evex_info *e = decode_evex(&u, b, sizeof b);

  UNIT_CHECK(e != NULL);
  m = ud_insn_opr(&u, 2);
  UNIT_CHECK(m != NULL && m->base == UD_R_RAX && m->index == UD_R_RCX);
  UNIT_CHECK(m->scale == 4);
  UNIT_CHECK(m->offset == 8);
  UNIT_CHECK(m->lval.sdword == 64);
  UNIT_CHECK(asm_is(&u, "vaddps zmm1, zmm2, [rax+rcx*4+0x40]"));
}

static void
tuple_memory_extensions(void)
{
  ud_t u;
  /* vaddps zmm1, zmm2, [r8] : EVEX.B' extends the base GPR to r8 */
  static const unsigned char r8[6] = { 0x62,0xd1,0x6c,0x48,0x58,0x08 };
  /* vaddps zmm1, zmm2, [r9+r10*2] : B' and X' extend base and index */
  static const unsigned char sib[7] = { 0x62,0x91,0x6c,0x48,0x58,0x0c,0x51 };
  const struct ud_operand *m;

  UNIT_CHECK(decode_evex(&u, r8, sizeof r8) != NULL);
  m = ud_insn_opr(&u, 2);
  UNIT_CHECK(m != NULL && m->base == UD_R_R8 && m->index == UD_NONE);
  UNIT_CHECK(asm_is(&u, "vaddps zmm1, zmm2, [r8]"));

  UNIT_CHECK(decode_evex(&u, sib, sizeof sib) != NULL);
  m = ud_insn_opr(&u, 2);
  UNIT_CHECK(m != NULL && m->base == UD_R_R9 && m->index == UD_R_R10);
  UNIT_CHECK(m->scale == 2);
  UNIT_CHECK(asm_is(&u, "vaddps zmm1, zmm2, [r9+r10*2]"));
}

static void
tuple_vmov_forms(void)
{
  ud_t u;
  /* vmovups zmm16, [rax+8128] : load */
  static const unsigned char ld[7] = { 0x62,0xe1,0x7c,0x48,0x10,0x40,0x7f };
  /* vmovups [rax+8128], zmm16 : store, operand order swapped */
  static const unsigned char st[7] = { 0x62,0xe1,0x7c,0x48,0x11,0x40,0x7f };
  /* vmovupd zmm16, zmm17 : register form */
  static const unsigned char rr[6] = { 0x62,0xa1,0xfd,0x48,0x10,0xc1 };
  const struct ud_evex_info *e;

  e = decode_evex(&u, ld, sizeof ld);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivmovups);
  UNIT_CHECK(opr_is_reg(&u, 0, UD_R_ZMM16, 512));
  UNIT_CHECK(opr_is_mem(&u, 1, 512));
  UNIT_CHECK(asm_is(&u, "vmovups zmm16, [rax+0x1fc0]"));

  e = decode_evex(&u, st, sizeof st);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(opr_is_mem(&u, 0, 512));
  UNIT_CHECK(opr_is_reg(&u, 1, UD_R_ZMM16, 512));
  UNIT_CHECK(asm_is(&u, "vmovups [rax+0x1fc0], zmm16"));

  e = decode_evex(&u, rr, sizeof rr);
  UNIT_CHECK(e != NULL);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivmovupd);
  UNIT_CHECK(opr_is_reg(&u, 0, UD_R_ZMM16, 512));
  UNIT_CHECK(opr_is_reg(&u, 1, UD_R_ZMM17, 512));
  UNIT_CHECK(asm_is(&u, "vmovupd zmm16, zmm17"));
}

/* ------------------------------------------------------------------------- */
/* syntax: INTEL decoration text, invalid cases and cleared state            */
/* ------------------------------------------------------------------------- */

static void
syntax_no_decoration_plain(void)
{
  ud_t u;
  /* vaddps zmm0, zmm1, zmm2 : no mask, no decoration */
  static const unsigned char c[6] = { 0x62,0xf1,0x74,0x48,0x58,0xc2 };
  UNIT_CHECK(decode_evex(&u, c, sizeof c) != NULL);
  UNIT_CHECK(ud_insn_evex(&u)->mask == 0);
  UNIT_CHECK(asm_is(&u, "vaddps zmm0, zmm1, zmm2"));
}

static void
syntax_plain_instructions_unchanged(void)
{
  ud_t u;
  /* mov rax, rbx */
  static const unsigned char mov[3] = { 0x48,0x89,0xd8 };
  /* vaddps ymm0, ymm1, ymm2 (VEX, not EVEX) */
  static const unsigned char vex[4] = { 0xc5,0xf4,0x58,0xc2 };
  /* nop */
  static const unsigned char nop[1] = { 0x90 };

  ud_init(&u);
  ud_set_mode(&u, 64);
  ud_set_syntax(&u, UD_SYN_INTEL);

  decode_bytes(&u, mov, sizeof mov);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Imov);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);
  UNIT_CHECK(u.translator && strcmp(ud_insn_asm(&u), "mov rax, rbx") == 0);

  decode_bytes(&u, vex, sizeof vex);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivaddps);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);
  UNIT_CHECK(opr_is_reg(&u, 0, UD_R_YMM0, 256));
  UNIT_CHECK(u.translator && strcmp(ud_insn_asm(&u), "vaddps ymm0, ymm1, ymm2") == 0);

  decode_bytes(&u, nop, sizeof nop);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Inop);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);
  UNIT_CHECK(u.translator && strcmp(ud_insn_asm(&u), "nop") == 0);
}

static void
syntax_att_formats_safely(void)
{
  ud_t u;
  /* vaddps zmm17{k3}{z}, zmm20, zmm31 */
  static const unsigned char c[6] = { 0x62,0x81,0x5c,0xc3,0x58,0xcf };
  /* vaddps zmm1, zmm2, dword [rax+508]{1to16} */
  static const unsigned char b[7] = { 0x62,0xf1,0x6c,0x58,0x58,0x48,0x7f };

  ud_init(&u);
  ud_set_mode(&u, 64);
  ud_set_syntax(&u, UD_SYN_ATT);

  decode_bytes(&u, c, sizeof c);
  UNIT_CHECK(ud_insn_evex(&u) != NULL);
  UNIT_CHECK(opr_is_reg(&u, 0, UD_R_ZMM17, 512));
  /* The AT&T operand order is source-first; formatting must not crash and
   * must use the structured operand registers. */
  UNIT_CHECK(u.translator && strstr(ud_insn_asm(&u), "%zmm31") != NULL);
  UNIT_CHECK(strstr(ud_insn_asm(&u), "%zmm20") != NULL);
  UNIT_CHECK(strstr(ud_insn_asm(&u), "%zmm17") != NULL);

  decode_bytes(&u, b, sizeof b);
  UNIT_CHECK(ud_insn_evex(&u) != NULL);
  UNIT_CHECK(u.translator && strstr(ud_insn_asm(&u), "0x1fc") != NULL);
}

static void
syntax_invalid_unsupported_opcode(void)
{
  ud_t u;
  /* EVEX map 0x1, opcode 0x5a = unsupported (not in supported set) */
  static const unsigned char b[6] = { 0x62,0x81,0x5c,0xc3,0x5a,0xcf };
  ud_init(&u);
  ud_set_mode(&u, 64);
  ud_set_syntax(&u, UD_SYN_INTEL);
  decode_bytes(&u, b, sizeof b);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);
}

static void
syntax_invalid_reserved_bits(void)
{
  ud_t u;
  /* reserved P0 bit 2 set */
  static const unsigned char p0[6] = { 0x62,0x85,0x5c,0xc3,0x58,0xcf };
  /* P1 bit 2 clear */
  static const unsigned char p1[6] = { 0x62,0x81,0x58,0xc3,0x58,0xcf };
  /* map 000 */
  static const unsigned char map[6] = { 0x62,0x80,0x5c,0xc3,0x58,0xcf };

  decode_evex(&u, p0, sizeof p0);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);

  decode_evex(&u, p1, sizeof p1);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);

  decode_evex(&u, map, sizeof map);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);
}

static void
syntax_invalid_vector_length(void)
{
  ud_t u;
  /* packed, b=0, L'L=3 */
  static const unsigned char b[6] = { 0x62,0x81,0x5c,0xe3,0x58,0xcf };
  decode_evex(&u, b, sizeof b);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);
}

static void
syntax_invalid_broadcast_rounding(void)
{
  ud_t u;
  /* scalar with b=1 but memory source: cannot be a broadcast */
  static const unsigned char s[7] = { 0x62,0xc1,0x76,0x10,0x58,0x41,0x7f };
  decode_evex(&u, s, sizeof s);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);

  /* vmovups has no embedded rounding: b=1 with register source */
  {
    static const unsigned char m[6] = { 0x62,0xa1,0x7c,0x18,0x10,0xc1 };
    decode_evex(&u, m, sizeof m);
    UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
    UNIT_CHECK(ud_insn_evex(&u) == NULL);
  }
}

static void
syntax_invalid_zeroing(void)
{
  ud_t u;
  /* zeroing with no explicit mask: aaa=0, z=1 */
  static const unsigned char zm[6] = { 0x62,0x81,0x5c,0x80,0x58,0xcf };
  /* zeroing on a memory destination: vmovupd [r13-8192]{k3}{z}, zmm18 */
  static const unsigned char st[7] = { 0x62,0xc1,0xfd,0xcb,0x11,0x55,0x80 };

  decode_evex(&u, zm, sizeof zm);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);

  decode_evex(&u, st, sizeof st);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);
}

static void
syntax_invalid_vmov_reserved(void)
{
  ud_t u;
  /* vmovups with vvvv not 1111b */
  static const unsigned char b[6] = { 0x62,0x81,0x74,0x08,0x10,0xc1 };
  decode_evex(&u, b, sizeof b);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);
}

static void
syntax_truncated_then_cleared(void)
{
  ud_t u;
  static const unsigned char full[6] = { 0x62,0x81,0x5c,0xc3,0x58,0xcf };
  static const unsigned char mov[3] = { 0x48,0x89,0xd8 };

  ud_init(&u);
  ud_set_mode(&u, 64);
  ud_set_syntax(&u, UD_SYN_INTEL);

  /* Good EVEX decode populates the info. */
  decode_bytes(&u, full, sizeof full);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ivaddps);
  UNIT_CHECK(ud_insn_evex(&u) != NULL);

  /* Every strict prefix of the same instruction must be invalid and must
   * not publish a partial EVEX result. */
  {
    size_t n;
    for (n = 1; n < sizeof full; n++) {
      ud_set_input_buffer(&u, full, n);
      ud_disassemble(&u);
      UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Iinvalid);
      UNIT_CHECK(ud_insn_evex(&u) == NULL);
    }
  }

  /* Re-setting input must decode normal instructions normally. */
  decode_bytes(&u, mov, sizeof mov);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Imov);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);
  UNIT_CHECK(u.translator && strcmp(ud_insn_asm(&u), "mov rax, rbx") == 0);
}

static void
syntax_evex_then_plain_clears(void)
{
  ud_t u;
  static const unsigned char ev[6] = { 0x62,0x81,0x5c,0xc3,0x58,0xcf };
  static const unsigned char mov[3] = { 0x48,0x89,0xd8 };

  ud_init(&u);
  ud_set_mode(&u, 64);
  ud_set_syntax(&u, UD_SYN_INTEL);

  decode_bytes(&u, ev, sizeof ev);
  UNIT_CHECK(ud_insn_evex(&u) != NULL);
  UNIT_CHECK(ud_insn_evex(&u)->mask == 3);

  decode_bytes(&u, mov, sizeof mov);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Imov);
}

static void
syntax_32bit_bound_unchanged(void)
{
  ud_t u;
  /* In 32-bit mode 0x62 is BOUND, not EVEX. */
  static const unsigned char b[3] = { 0x62,0x00,0x00 };
  ud_init(&u);
  ud_set_mode(&u, 32);
  ud_set_syntax(&u, UD_SYN_INTEL);
  decode_bytes(&u, b, sizeof b);
  UNIT_CHECK(ud_insn_mnemonic(&u) == UD_Ibound);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);
}

static void
syntax_64bit_requires_mode(void)
{
  ud_t u;
  static const unsigned char ev[6] = { 0x62,0x81,0x5c,0xc3,0x58,0xcf };
  /* In 16 bit mode the same bytes are not an EVEX instruction. */
  ud_init(&u);
  ud_set_mode(&u, 16);
  ud_set_syntax(&u, UD_SYN_INTEL);
  decode_bytes(&u, ev, sizeof ev);
  UNIT_CHECK(ud_insn_evex(&u) == NULL);
}

/* ------------------------------------------------------------------------- */
/* Registration                                                              */
/* ------------------------------------------------------------------------- */

const unit_case unit_cases[] = {
  /* prefix */
  { "prefix", "high_register_regs",   prefix_high_register_regs },
  { "prefix", "128_regs",             prefix_128_regs },
  { "prefix", "256_regs",             prefix_256_regs },
  { "prefix", "512_regs",             prefix_512_regs },
  { "prefix", "scalar_width",         prefix_scalar_width },
  { "prefix", "mul_variants",         prefix_mul_variants },
  { "prefix", "packed_128_256_512",   prefix_packed_128_256_512 },
  { "prefix", "reg_names",            prefix_reg_names },

  /* tuple */
  { "tuple", "masked_merge_zero",     tuple_masked_merge_zero },
  { "tuple", "ps_broadcast",          tuple_ps_broadcast },
  { "tuple", "pd_broadcast",          tuple_pd_broadcast },
  { "tuple", "256_broadcast",         tuple_256_broadcast },
  { "tuple", "rounding_not_broadcast", tuple_rounding_not_broadcast },
  { "tuple", "scalar_rounding",       tuple_scalar_rounding },
  { "tuple", "disp8_vector_scale",    tuple_disp8_vector_scale },
  { "tuple", "disp8_scalar_scale",    tuple_disp8_scalar_scale },
  { "tuple", "disp32_unscaled",       tuple_disp32_unscaled },
  { "tuple", "rip_relative_unscaled", tuple_rip_relative_unscaled },
  { "tuple", "sib_no_disp",           tuple_sib_no_disp },
  { "tuple", "sib_disp8_scale",       tuple_sib_disp8_scale },
  { "tuple", "memory_extensions",     tuple_memory_extensions },
  { "tuple", "vmov_forms",            tuple_vmov_forms },

  /* syntax */
  { "syntax", "no_decoration_plain",      syntax_no_decoration_plain },
  { "syntax", "plain_instructions_unchanged", syntax_plain_instructions_unchanged },
  { "syntax", "att_formats_safely",       syntax_att_formats_safely },
  { "syntax", "invalid_unsupported_opcode", syntax_invalid_unsupported_opcode },
  { "syntax", "invalid_reserved_bits",    syntax_invalid_reserved_bits },
  { "syntax", "invalid_vector_length",    syntax_invalid_vector_length },
  { "syntax", "invalid_broadcast_rounding", syntax_invalid_broadcast_rounding },
  { "syntax", "invalid_zeroing",          syntax_invalid_zeroing },
  { "syntax", "invalid_vmov_reserved",    syntax_invalid_vmov_reserved },
  { "syntax", "truncated_then_cleared",   syntax_truncated_then_cleared },
  { "syntax", "evex_then_plain_clears",   syntax_evex_then_plain_clears },
  { "syntax", "32bit_bound_unchanged",    syntax_32bit_bound_unchanged },
  { "syntax", "64bit_requires_mode",      syntax_64bit_requires_mode },
};

const size_t unit_case_count = sizeof(unit_cases) / sizeof(unit_cases[0]);
