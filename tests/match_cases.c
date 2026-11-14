/* tests/match_cases.c
 *
 * Hand-written x86-64 machine-code cases for ud_match_functions64.  The
 * expected correspondences are written out independently of the decoder;
 * groups: topology, aliases, schedule.  Each positive case has at least one
 * negative companion that changes a single dependency or branch connection.
 */
#include "../task_support/check.h"

#include "cfg_match.h"

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static struct ud_match_result *
match_bytes(const uint8_t *l, size_t ln, const uint8_t *r, size_t rn,
            const struct ud_addr_anchor *an, size_t anc)
{
  struct ud_function_bytes L = { l, ln, 0x1000ULL, 0x1000ULL };
  struct ud_function_bytes R = { r, rn, 0x2000ULL, 0x2000ULL };
  struct ud_match_result *res = NULL;
  enum ud_match_status st = ud_match_functions64(&L, &R, an, anc, &res);
  if (st != UD_MATCH_OK) {
    return NULL;
  }
  return res;
}

static int
status_of(const uint8_t *l, size_t ln, const uint8_t *r, size_t rn,
          const struct ud_addr_anchor *an, size_t anc)
{
  struct ud_function_bytes L = { l, ln, 0x1000ULL, 0x1000ULL };
  struct ud_function_bytes R = { r, rn, 0x2000ULL, 0x2000ULL };
  struct ud_match_result *res = NULL;
  enum ud_match_status st = ud_match_functions64(&L, &R, an, anc, &res);
  ud_match_result_free(res);
  return (int)st;
}

static int
find_map(const struct ud_match_result *res, unsigned left_family)
{
  for (size_t i = 0; i < res->register_count; i++) {
    if (res->registers[i].left_family == left_family) {
      return (int)res->registers[i].right_family;
    }
  }
  return -1;
}

static int
has_swapped_edge(const struct ud_match_result *res)
{
  for (size_t i = 0; i < res->edge_count; i++) {
    if (res->edges[i].swapped) {
      return 1;
    }
  }
  return 0;
}

static int
has_back_edge(const struct ud_match_result *res)
{
  for (size_t i = 0; i < res->edge_count; i++) {
    if (res->edges[i].left_to <= res->edges[i].left_from) {
      return 1;
    }
  }
  return 0;
}

static int
maps_insn(const struct ud_match_result *res, uint64_t lva, uint64_t rva)
{
  for (size_t i = 0; i < res->insn_count; i++) {
    if (res->insns[i].left_va == lva && res->insns[i].right_va == rva) {
      return 1;
    }
  }
  return 0;
}

/* ================================================================== */
/* topology group                                                      */
/* ================================================================== */

/* mov eax,ecx ; ret  versus  mov edx,ebx ; ret */
static int
topo_simple_ret(void)
{
  const uint8_t l[] = { 0x89, 0xc8, 0xc3 };
  const uint8_t r[] = { 0x89, 0xda, 0xc3 };
  struct ud_match_result *res = match_bytes(l, sizeof l, r, sizeof r, NULL, 0);
  TASK_CHECK(res != NULL);
  TASK_CHECK(res->insn_count == 2);
  TASK_CHECK(res->block_count == 1);
  TASK_CHECK(find_map(res, 0) == 2);  /* RAX -> RDX */
  TASK_CHECK(find_map(res, 1) == 3);  /* RCX -> RBX */
  ud_match_result_free(res);
  return 0;
}

/* Loop with a back edge and a convergence; right side uses another family,
 * an extra NOP and the near (rel32) branch encodings. */
static int
topo_backedge_merge(void)
{
  /* mov eax,0; cmp eax,5; jae end; inc eax; jmp loop; ret */
  const uint8_t l[] = {
    0xb8, 0x00, 0x00, 0x00, 0x00,
    0x83, 0xf8, 0x05,
    0x73, 0x04,
    0xff, 0xc0,
    0xeb, 0xf7,
    0xc3
  };
  /* mov ebx,0; nop; cmp ebx,5; jae end; inc ebx; jmp loop; ret */
  const uint8_t r[] = {
    0xbb, 0x00, 0x00, 0x00, 0x00,
    0x90,
    0x83, 0xfb, 0x05,
    0x0f, 0x83, 0x07, 0x00, 0x00, 0x00,
    0xff, 0xc3,
    0xe9, 0xf0, 0xff, 0xff, 0xff,
    0xc3
  };
  struct ud_match_result *res = match_bytes(l, sizeof l, r, sizeof r, NULL, 0);
  TASK_CHECK(res != NULL);
  TASK_CHECK(res->block_count >= 3);
  TASK_CHECK(has_back_edge(res));
  TASK_CHECK(find_map(res, 0) == 3); /* RAX -> RBX */
  /* the NOP and the folded JMP must not appear in the correspondence */
  TASK_CHECK(res->insn_count == 5);
  ud_match_result_free(res);
  return 0;
}

/* JZ with explicit successors versus complementary JNZ with the two
 * successors exchanged. */
static int
topo_inverted_branch(void)
{
  /* cmp eax,0; jz eq; mov eax,1; jmp done; eq: xor eax,eax; done: ret */
  const uint8_t l[] = {
    0x83, 0xf8, 0x00,
    0x74, 0x07,
    0xb8, 0x01, 0x00, 0x00, 0x00,
    0xeb, 0x02,
    0x31, 0xc0,
    0xc3
  };
  /* cmp eax,0; jnz ne; xor eax,eax; jmp done; ne: mov eax,1; done: ret */
  const uint8_t r[] = {
    0x83, 0xf8, 0x00,
    0x75, 0x04,
    0x31, 0xc0,
    0xeb, 0x05,
    0xb8, 0x01, 0x00, 0x00, 0x00,
    0xc3
  };
  struct ud_match_result *res = match_bytes(l, sizeof l, r, sizeof r, NULL, 0);
  TASK_CHECK(res != NULL);
  TASK_CHECK(has_swapped_edge(res));
  /* the mov eax,1 instructions must still correspond across the swap */
  TASK_CHECK(maps_insn(res, 0x1005ULL, 0x2009ULL));
  ud_match_result_free(res);
  return 0;
}

/* Reconnect one branch: the JZ target is changed to the common exit, leaving
 * the xor block unreachable, so the branch structure no longer matches. */
static int
topo_negative_bad_connection(void)
{
  const uint8_t l[] = {
    0x83, 0xf8, 0x00,
    0x74, 0x07,
    0xb8, 0x01, 0x00, 0x00, 0x00,
    0xeb, 0x02,
    0x31, 0xc0,
    0xc3
  };
  /* jz now jumps straight to the exit instead of the eq block */
  const uint8_t r[] = {
    0x83, 0xf8, 0x00,
    0x74, 0x09,
    0xb8, 0x01, 0x00, 0x00, 0x00,
    0xeb, 0x02,
    0x31, 0xc0,
    0xc3
  };
  TASK_CHECK(status_of(l, sizeof l, r, sizeof r, NULL, 0) ==
             UD_MATCH_NO_MATCH);
  return 0;
}

/* A direct CALL is recorded as a reference and flow continues after it. */
static int
topo_call_reference(void)
{
  /* call rel32 ; ret -- identical images, so the absolute targets agree */
  const uint8_t l[] = { 0xe8, 0x00, 0x10, 0x00, 0x00, 0xc3 };
  const uint8_t r[] = { 0xe8, 0x00, 0x10, 0x00, 0x00, 0xc3 };
  struct ud_function_bytes L = { l, sizeof l, 0x1000ULL, 0x1000ULL };
  struct ud_function_bytes R = { r, sizeof r, 0x1000ULL, 0x1000ULL };
  struct ud_match_result *res = NULL;
  TASK_CHECK(ud_match_functions64(&L, &R, NULL, 0, &res) == UD_MATCH_OK);
  TASK_CHECK(res != NULL);
  TASK_CHECK(res->insn_count == 2);
  ud_match_result_free(res);

  /* moving the call target breaks the reference match */
  const uint8_t r2[] = { 0xe8, 0x01, 0x10, 0x00, 0x00, 0xc3 };
  struct ud_function_bytes R2 = { r2, sizeof r2, 0x1000ULL, 0x1000ULL };
  TASK_CHECK(ud_match_functions64(&L, &R2, NULL, 0, &res) ==
             UD_MATCH_NO_MATCH);
  ud_match_result_free(res);
  return 0;
}

/* A cycle made only of transparent nodes is explicitly unsupported. */
static int
topo_transparent_cycle(void)
{
  const uint8_t l[] = { 0xeb, 0xfe }; /* jmp self */
  const uint8_t r[] = { 0xeb, 0xfe };
  TASK_CHECK(status_of(l, sizeof l, r, sizeof r, NULL, 0) ==
             UD_MATCH_UNSUPPORTED);
  return 0;
}

/* Corrupt input is reported distinctly from a normal mismatch. */
static int
topo_invalid_inputs(void)
{
  const uint8_t good[] = { 0x89, 0xc8, 0xc3 };

  /* truncated instruction */
  const uint8_t trunc[] = { 0x83 };
  TASK_CHECK(status_of(trunc, sizeof trunc, good, sizeof good, NULL, 0) ==
             UD_MATCH_INVALID);

  /* branch landing in the middle of an otherwise decoded instruction */
  const uint8_t middle[] = { 0x70, 0x01, 0x89, 0xc8, 0xc3 };
  TASK_CHECK(status_of(middle, sizeof middle, good, sizeof good, NULL, 0) ==
             UD_MATCH_INVALID);

  /* control successor outside the image */
  const uint8_t outside[] = { 0xeb, 0x10 };
  TASK_CHECK(status_of(outside, sizeof outside, good, sizeof good, NULL, 0) ==
             UD_MATCH_INVALID);

  /* indirect control transfer is not guessed: unsupported, not invalid */
  const uint8_t indirect[] = { 0xff, 0xe0, 0xc3 };
  TASK_CHECK(status_of(indirect, sizeof indirect, indirect,
                       sizeof indirect, NULL, 0) == UD_MATCH_UNSUPPORTED);
  return 0;
}

/* ================================================================== */
/* aliases group                                                       */
/* ================================================================== */

/* AH and AL are disjoint views of one family; high-byte position survives. */
static int
alias_ah_al(void)
{
  /* mov al,0x11; mov ah,0x22; mov ah,al; ret */
  const uint8_t l[] = { 0xb0, 0x11, 0xb4, 0x22, 0x88, 0xc4, 0xc3 };
  /* mov dl,0x11; mov dh,0x22; mov dh,dl; ret */
  const uint8_t r[] = { 0xb2, 0x11, 0xb6, 0x22, 0x88, 0xd6, 0xc3 };
  struct ud_match_result *res = match_bytes(l, sizeof l, r, sizeof r, NULL, 0);
  TASK_CHECK(res != NULL);
  TASK_CHECK(find_map(res, 0) == 2); /* RAX -> RDX, AH maps onto DH */
  ud_match_result_free(res);

  /* mixing the high-byte view with a different family cannot match */
  /* mov dl,0x11; mov ch,0x22; mov ch,dl; ret */
  const uint8_t bad[] = { 0xb2, 0x11, 0xb5, 0x22, 0x88, 0xd5, 0xc3 };
  TASK_CHECK(status_of(l, sizeof l, bad, sizeof bad, NULL, 0) ==
             UD_MATCH_NO_MATCH);
  return 0;
}

/* Writing a 32-bit view zeroes the high 32 bits; view widths must agree. */
static int
alias_eax_widths(void)
{
  /* mov al,1; mov eax,2; ret */
  const uint8_t l32[] = { 0xb0, 0x01, 0xb8, 0x02, 0x00, 0x00, 0x00, 0xc3 };
  const uint8_t r32[] = { 0xb1, 0x01, 0xb9, 0x02, 0x00, 0x00, 0x00, 0xc3 };
  struct ud_match_result *res =
    match_bytes(l32, sizeof l32, r32, sizeof r32, NULL, 0);
  TASK_CHECK(res != NULL);
  TASK_CHECK(find_map(res, 0) == 1);
  ud_match_result_free(res);

  /* mov cl,1; mov cx,2; ret -- 16-bit view cannot pair with the 32-bit write */
  const uint8_t r16[] = { 0xb1, 0x01, 0x66, 0xb9, 0x02, 0x00, 0xc3 };
  TASK_CHECK(status_of(l32, sizeof l32, r16, sizeof r16, NULL, 0) ==
             UD_MATCH_NO_MATCH);
  return 0;
}

/* Two distinct families can never collapse onto one. */
static int
alias_bijection(void)
{
  /* mov eax,1; mov ecx,2; ret */
  const uint8_t l[] = {
    0xb8, 0x01, 0x00, 0x00, 0x00,
    0xb9, 0x02, 0x00, 0x00, 0x00,
    0xc3
  };
  /* mov eax,1; mov eax,2; ret */
  const uint8_t r[] = {
    0xb8, 0x01, 0x00, 0x00, 0x00,
    0xb8, 0x02, 0x00, 0x00, 0x00,
    0xc3
  };
  TASK_CHECK(status_of(l, sizeof l, r, sizeof r, NULL, 0) ==
             UD_MATCH_NO_MATCH);
  return 0;
}

/* RSP and RBP are fixed to themselves. */
static int
alias_fixed_sp_bp(void)
{
  /* mov rax,rsp; ret */
  const uint8_t l[] = { 0x48, 0x89, 0xe0, 0xc3 };
  /* mov rbx,rsp; ret */
  const uint8_t ok[] = { 0x48, 0x89, 0xe3, 0xc3 };
  struct ud_match_result *res = match_bytes(l, sizeof l, ok, sizeof ok,
                                            NULL, 0);
  TASK_CHECK(res != NULL);
  TASK_CHECK(find_map(res, 4) == 4);  /* RSP -> RSP */
  TASK_CHECK(find_map(res, 0) == 3);  /* RAX -> RBX */
  ud_match_result_free(res);

  /* mov rbx,rbp; ret -- would force RSP onto RBP */
  const uint8_t bad[] = { 0x48, 0x89, 0xeb, 0xc3 };
  TASK_CHECK(status_of(l, sizeof l, bad, sizeof bad, NULL, 0) ==
             UD_MATCH_NO_MATCH);
  return 0;
}

/* ================================================================== */
/* schedule group                                                      */
/* ================================================================== */

/* Independent moves are reordered; local pairing stays ambiguous until the
 * later ADD constrains the global family mapping. */
static int
sched_delayed_constraint(void)
{
  /* mov eax,1; mov ecx,2; add eax,ecx; ret */
  const uint8_t l[] = {
    0xb8, 0x01, 0x00, 0x00, 0x00,
    0xb9, 0x02, 0x00, 0x00, 0x00,
    0x01, 0xc8,
    0xc3
  };
  /* mov edx,2; mov ebx,1; add ebx,edx; ret */
  const uint8_t r[] = {
    0xba, 0x02, 0x00, 0x00, 0x00,
    0xbb, 0x01, 0x00, 0x00, 0x00,
    0x01, 0xd3,
    0xc3
  };
  struct ud_match_result *res = match_bytes(l, sizeof l, r, sizeof r, NULL, 0);
  TASK_CHECK(res != NULL);
  TASK_CHECK(find_map(res, 0) == 3); /* RAX -> RBX */
  TASK_CHECK(find_map(res, 1) == 2); /* RCX -> RDX */
  TASK_CHECK(maps_insn(res, 0x1000ULL, 0x2005ULL)); /* mov eax,1 -> mov ebx,1 */
  ud_match_result_free(res);

  /* one data dependency changed: add eax,eax instead of add eax,ecx */
  const uint8_t bad[] = {
    0xb8, 0x01, 0x00, 0x00, 0x00,
    0xb9, 0x02, 0x00, 0x00, 0x00,
    0x01, 0xc0,
    0xc3
  };
  TASK_CHECK(status_of(bad, sizeof bad, r, sizeof r, NULL, 0) ==
             UD_MATCH_NO_MATCH);
  return 0;
}

/* INC leaves CF untouched; an independent MOV may cross it, but two flag
 * writers may not exchange positions. */
static int
sched_inc_adc_flags(void)
{
  /* mov ecx,1; inc ecx; mov eax,7; adc edx,ebx; ret */
  const uint8_t l[] = {
    0xb9, 0x01, 0x00, 0x00, 0x00,
    0xff, 0xc1,
    0xb8, 0x07, 0x00, 0x00, 0x00,
    0x11, 0xda,
    0xc3
  };
  /* mov ecx,1; mov eax,7; inc ecx; adc edx,ebx; ret -- MOV crosses INC */
  const uint8_t ok[] = {
    0xb9, 0x01, 0x00, 0x00, 0x00,
    0xb8, 0x07, 0x00, 0x00, 0x00,
    0xff, 0xc1,
    0x11, 0xda,
    0xc3
  };
  struct ud_match_result *res = match_bytes(l, sizeof l, ok, sizeof ok,
                                            NULL, 0);
  TASK_CHECK(res != NULL);
  ud_match_result_free(res);

  /* adc edx,ebx; inc ecx -- reversed order of two flag writers */
  const uint8_t bad[] = {
    0xb9, 0x01, 0x00, 0x00, 0x00,
    0xb8, 0x07, 0x00, 0x00, 0x00,
    0x11, 0xda,
    0xff, 0xc1,
    0xc3
  };
  TASK_CHECK(status_of(l, sizeof l, bad, sizeof bad, NULL, 0) ==
             UD_MATCH_NO_MATCH);
  return 0;
}

/* Real memory accesses keep their order; LEA is not a memory access. */
static int
sched_memory_order(void)
{
  /* mov [rbx],eax; mov ecx,[rbx]; ret */
  const uint8_t l[] = { 0x89, 0x03, 0x8b, 0x0b, 0xc3 };
  const uint8_t reversed[] = { 0x8b, 0x0b, 0x89, 0x03, 0xc3 };
  TASK_CHECK(status_of(l, sizeof l, reversed, sizeof reversed, NULL, 0) ==
             UD_MATCH_NO_MATCH);

  /* lea r8,[rbx+4]; mov [rbx],eax; ret */
  const uint8_t with_lea_l[] = {
    0x4c, 0x8d, 0x43, 0x04,
    0x89, 0x03,
    0xc3
  };
  /* mov [rbx],eax; lea r8,[rbx+4]; ret -- LEA may be rescheduled */
  const uint8_t with_lea_r[] = {
    0x89, 0x03,
    0x4c, 0x8d, 0x43, 0x04,
    0xc3
  };
  struct ud_match_result *res =
    match_bytes(with_lea_l, sizeof with_lea_l, with_lea_r, sizeof with_lea_r,
                NULL, 0);
  TASK_CHECK(res != NULL);
  TASK_CHECK(res->insn_count == 3);
  ud_match_result_free(res);
  return 0;
}

/* RIP-relative effective addresses and direct CALL targets pair through
 * anchors; plain immediates do not. */
static int
sched_anchors(void)
{
  /* mov eax,[rip+0xfa]; call 0x3000; ret */
  const uint8_t l[] = {
    0x8b, 0x05, 0xfa, 0x00, 0x00, 0x00,
    0xe8, 0xf5, 0x1f, 0x00, 0x00,
    0xc3
  };
  /* mov eax,[rip+0xfa]; call 0x4000; ret */
  const uint8_t r[] = {
    0x8b, 0x05, 0xfa, 0x00, 0x00, 0x00,
    0xe8, 0xf5, 0x1f, 0x00, 0x00,
    0xc3
  };
  TASK_CHECK(status_of(l, sizeof l, r, sizeof r, NULL, 0) ==
             UD_MATCH_NO_MATCH);

  struct ud_addr_anchor an[2] = {
    { 0x1100ULL, 0x2100ULL }, /* RIP effective addresses */
    { 0x3000ULL, 0x4000ULL }  /* direct call targets */
  };
  struct ud_match_result *res =
    match_bytes(l, sizeof l, r, sizeof r, an, 2);
  TASK_CHECK(res != NULL);
  TASK_CHECK(res->insn_count == 3);
  ud_match_result_free(res);

  /* reversing the anchor table must yield the same stable witness */
  struct ud_addr_anchor reversed[2] = { an[1], an[0] };
  struct ud_function_bytes LF = { l, sizeof l, 0x1000ULL, 0x1000ULL };
  struct ud_function_bytes RF = { r, sizeof r, 0x2000ULL, 0x2000ULL };
  struct ud_match_result *res2 = NULL;
  TASK_CHECK(ud_match_functions64(&LF, &RF, reversed, 2, &res2) ==
             UD_MATCH_OK);
  res = match_bytes(l, sizeof l, r, sizeof r, an, 2);
  TASK_CHECK(res != NULL && res2 != NULL);
  TASK_CHECK(res->insn_count == res2->insn_count);
  {
    int same = 1;
    for (size_t i = 0; i < res->insn_count; i++) {
      if (res->insns[i].left_va != res2->insns[i].left_va ||
          res->insns[i].right_va != res2->insns[i].right_va) {
        same = 0;
      }
    }
    TASK_CHECK(same);
  }
  ud_match_result_free(res);
  ud_match_result_free(res2);

  /* a plain immediate keeps its numeric value even when an anchor exists */
  const uint8_t imm_l[] = { 0xb8, 0x00, 0x11, 0x00, 0x00, 0xc3 };
  const uint8_t imm_r[] = { 0xb8, 0x00, 0x21, 0x00, 0x00, 0xc3 };
  TASK_CHECK(status_of(imm_l, sizeof imm_l, imm_r, sizeof imm_r, an, 2) ==
             UD_MATCH_NO_MATCH);

  /* duplicated anchor addresses are invalid input */
  struct ud_addr_anchor dup[2] = {
    { 0x1100ULL, 0x2100ULL },
    { 0x1100ULL, 0x2200ULL }
  };
  TASK_CHECK(status_of(l, sizeof l, r, sizeof r, dup, 2) ==
             UD_MATCH_INVALID);
  return 0;
}

/* Extended low-byte views: R8B and SPL follow their 64-bit families. */
static int
alias_r8b_spl(void)
{
  /* mov r8b, al ; ret */
  const uint8_t l[] = { 0x41, 0x88, 0xc0, 0xc3 };
  /* mov r9b, cl ; ret */
  const uint8_t r[] = { 0x41, 0x88, 0xc9, 0xc3 };
  struct ud_match_result *res = match_bytes(l, sizeof l, r, sizeof r, NULL, 0);
  TASK_CHECK(res != NULL);
  TASK_CHECK(find_map(res, 8) == 9);  /* R8 -> R9 */
  TASK_CHECK(find_map(res, 0) == 1);  /* RAX -> RCX */
  ud_match_result_free(res);

  /* mov spl, cl (REX required); RSP stays fixed to itself */
  const uint8_t s[] = { 0x40, 0x88, 0xcc, 0xc3 };
  res = match_bytes(s, sizeof s, s, sizeof s, NULL, 0);
  TASK_CHECK(res != NULL);
  TASK_CHECK(find_map(res, 4) == 4);  /* RSP -> RSP */
  ud_match_result_free(res);
  return 0;
}

/* Immediate encoding width normalizes when the sign-extended value agrees,
 * and disjoint byte views can be scheduled independently. */
static int
sched_imm_width_and_bytes(void)
{
  /* add rax,5 (imm8) ; ret */
  const uint8_t narrow[] = { 0x48, 0x83, 0xc0, 0x05, 0xc3 };
  /* add rax,5 (imm32) ; ret */
  const uint8_t wide[] = {
    0x48, 0x81, 0xc0, 0x05, 0x00, 0x00, 0x00, 0xc3
  };
  struct ud_match_result *res =
    match_bytes(narrow, sizeof narrow, wide, sizeof wide, NULL, 0);
  TASK_CHECK(res != NULL);
  ud_match_result_free(res);

  /* add rax,6 (imm32) changes the constant and must not match */
  const uint8_t other[] = {
    0x48, 0x81, 0xc0, 0x06, 0x00, 0x00, 0x00, 0xc3
  };
  TASK_CHECK(status_of(narrow, sizeof narrow, other, sizeof other, NULL, 0) ==
             UD_MATCH_NO_MATCH);

  /* mov al,1 ; mov ah,2 ; ret  versus  mov ah,2 ; mov al,1 ; ret:
   * the two byte views never overlap, so reordering is allowed. */
  const uint8_t h1[] = { 0xb0, 0x01, 0xb4, 0x02, 0xc3 };
  const uint8_t h2[] = { 0xb4, 0x02, 0xb0, 0x01, 0xc3 };
  res = match_bytes(h1, sizeof h1, h2, sizeof h2, NULL, 0);
  TASK_CHECK(res != NULL);
  TASK_CHECK(res->insn_count == 3);
  ud_match_result_free(res);

  /* writing EAX (full low 32) conflicts with the AH view, so no reorder */
  const uint8_t e1[] = {
    0xb4, 0x02,
    0xb8, 0x01, 0x00, 0x00, 0x00,
    0xc3
  };
  const uint8_t e2[] = {
    0xb8, 0x01, 0x00, 0x00, 0x00,
    0xb4, 0x02,
    0xc3
  };
  TASK_CHECK(status_of(e1, sizeof e1, e2, sizeof e2, NULL, 0) ==
             UD_MATCH_NO_MATCH);
  return 0;
}

/* An absolute control target that leaves the uint64 address space is corrupt
 * input, not a normal mismatch. */
static int
topo_overflow(void)
{
  /* jmp rel32 with a large positive displacement */
  const uint8_t jmp[] = { 0xe9, 0xff, 0xff, 0xff, 0x7f };
  const uint8_t good[] = { 0xc3 };
  struct ud_function_bytes L = {
    jmp, sizeof jmp, 0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFFFFFFFF0ULL
  };
  struct ud_function_bytes R = { good, sizeof good, 0x1000ULL, 0x1000ULL };
  struct ud_match_result *res = NULL;
  TASK_CHECK(ud_match_functions64(&L, &R, NULL, 0, &res) ==
             UD_MATCH_INVALID);
  ud_match_result_free(res);
  return 0;
}

const struct task_case task_cases[] = {
  { "topology", "simple_ret",        topo_simple_ret },
  { "topology", "backedge_merge",    topo_backedge_merge },
  { "topology", "inverted_branch",   topo_inverted_branch },
  { "topology", "bad_connection",    topo_negative_bad_connection },
  { "topology", "call_reference",    topo_call_reference },
  { "topology", "transparent_cycle", topo_transparent_cycle },
  { "topology", "invalid_inputs",    topo_invalid_inputs },
  { "topology", "overflow",          topo_overflow },

  { "aliases", "ah_al",        alias_ah_al },
  { "aliases", "eax_widths",   alias_eax_widths },
  { "aliases", "bijection",    alias_bijection },
  { "aliases", "fixed_sp_bp",  alias_fixed_sp_bp },
  { "aliases", "r8b_spl",      alias_r8b_spl },

  { "schedule", "delayed_constraint", sched_delayed_constraint },
  { "schedule", "inc_adc_flags",      sched_inc_adc_flags },
  { "schedule", "memory_order",       sched_memory_order },
  { "schedule", "anchors",            sched_anchors },
  { "schedule", "imm_width_bytes",    sched_imm_width_and_bytes }
};

const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
