/****************************************************************************
*
* Segment protection fault cases for libx86emu.
*
* These cases build minimal, valid protected-mode segment caches by hand
* and run small, benign instruction streams. A custom memio callback logs
* every data memory transaction so a rejected access can be proven not to
* have reached the data memory at all.
*
* Groups:
*   ranges       - limit edge bytes/words, 32-bit end wrap, expand-down
*                  bounds for both D/B settings
*   permissions  - read-only RMW, code segment read/write rights, null
*                  DS/ES, SS vs. GP vector identity, BP default SS vs.
*                  explicit DS override
*   restart      - faults on prefixed instructions keep saved_cs/saved_eip
*                  at the first prefix byte and stop before the next insn
*
****************************************************************************/

#include "include/x86emu_int.h"
#include <stdio.h>
#include <string.h>

#define CS_BASE		0x10000u
#define DS_BASE		0x20000u
#define SS_BASE		0x30000u

/* cached descriptor access rights used below */
#define ACC_DATA32_RW	0x493u	/* 32-bit expand-up writable data       */
#define ACC_DATA32_RO	0x491u	/* 32-bit expand-up read-only data      */
#define ACC_DATA32_ED	0x497u	/* 32-bit expand-down writable data     */
#define ACC_DATA16_ED	0x097u	/* 16-bit expand-down writable data     */
#define ACC_CODE32_RX	0x49bu	/* 32-bit readable code                 */
#define ACC_CODE32_XO	0x499u	/* 32-bit execute-only code             */

struct memlog_entry {
  unsigned type;
  u32 addr;
};

static struct memlog_entry memlog[512];
static unsigned memlog_n;

static unsigned fault_count;
static unsigned fault_nr;
static unsigned fault_errcode;
static u32 fault_saved_eip;
static u16 fault_saved_cs;

static unsigned failures;
static unsigned group_failures;


static unsigned tracked_memio(x86emu_t *emu, u32 addr, u32 *val, unsigned type)
{
  if(memlog_n < sizeof memlog / sizeof memlog[0]) {
    memlog[memlog_n].type = type;
    memlog[memlog_n].addr = addr;
    memlog_n++;
  }

  return vm_memio(emu, addr, val, type);
}


static int fault_handler(x86emu_t *emu, u8 nr, unsigned type)
{
  (void) type;

  fault_count++;
  fault_nr = nr;
  fault_errcode = emu->x86.intr_errcode;
  fault_saved_eip = emu->x86.saved_eip;
  fault_saved_cs = emu->x86.saved_cs;

  x86emu_stop(emu);

  return 1;
}


static void set_seg(x86emu_t *emu, int idx, u16 sel, u32 base, u32 limit, u16 acc)
{
  emu->x86.seg[idx].sel = sel;
  emu->x86.seg[idx].base = base;
  emu->x86.seg[idx].limit = limit;
  emu->x86.seg[idx].acc = acc;
}


static void reset_logs(void)
{
  memlog_n = 0;
  fault_count = 0;
  fault_nr = 0;
  fault_errcode = 0xffff;
  fault_saved_eip = 0xdeadbeef;
  fault_saved_cs = 0;
}


static x86emu_t *new_vm(u16 ds_acc, u32 ds_limit, u32 ss_limit, u16 ds_sel, u16 ss_sel)
{
  x86emu_t *emu = x86emu_new(X86EMU_PERM_R | X86EMU_PERM_W | X86EMU_PERM_X, 0);

  if(!emu) return NULL;

  emu->x86.R_CR0 |= 1;	/* protected mode */

  set_seg(emu, R_CS_INDEX, 8, CS_BASE, 0xffff, ACC_CODE32_RX);
  set_seg(emu, R_DS_INDEX, ds_sel, DS_BASE, ds_limit, ds_acc);
  set_seg(emu, R_ES_INDEX, ds_sel, DS_BASE, ds_limit, ds_acc);
  set_seg(emu, R_SS_INDEX, ss_sel, SS_BASE, ss_limit, ACC_DATA32_RW);

  emu->x86.R_EIP = 0;
  emu->x86.R_EBP = 0;
  emu->x86.R_EAX = 0;

  x86emu_set_memio_handler(emu, tracked_memio);
  x86emu_set_intr_handler(emu, fault_handler);
  emu->max_instr = 64;

  return emu;
}


static void load_code(x86emu_t *emu, const unsigned char *code, unsigned len)
{
  unsigned i;

  for(i = 0; i < len; i++) x86emu_write_byte(emu, CS_BASE + i, code[i]);
}


static void run_code(x86emu_t *emu, const unsigned char *code, unsigned len)
{
  reset_logs();
  load_code(emu, code, len);
  emu->x86.R_EIP = 0;
  x86emu_run(emu, X86EMU_RUN_MAX_INSTR);
}


/* returns 1 when a data transaction of access (X86EMU_MEMIO_R/W) was seen */
static int saw_access(u32 lin_addr, unsigned access)
{
  unsigned i;

  for(i = 0; i < memlog_n; i++) {
    if(memlog[i].addr == lin_addr && (memlog[i].type & 0x300) == access) return 1;
  }

  return 0;
}


static void check(int cond, const char *what)
{
  if(!cond) {
    printf("FAIL: %s (fault_nr=%u count=%u)\n", what, fault_nr, fault_count);
    failures++;
    group_failures++;
  }
}


/* mov byte  [disp32], imm8  : C6 05 <disp32> <ib> */
static unsigned emit_mov_byte_imm(unsigned char *p, u32 disp, u8 imm)
{
  p[0] = 0xc6; p[1] = 0x05;
  p[2] = disp; p[3] = disp >> 8; p[4] = disp >> 16; p[5] = disp >> 24;
  p[6] = imm;
  return 7;
}


/* 66 C7 05 <disp32> <imm16> : mov word [disp32], imm16 */
static unsigned emit_mov_word_imm(unsigned char *p, u32 disp, u16 imm)
{
  p[0] = 0x66; p[1] = 0xc7; p[2] = 0x05;
  p[3] = disp; p[4] = disp >> 8; p[5] = disp >> 16; p[6] = disp >> 24;
  p[7] = imm; p[8] = imm >> 8;
  return 9;
}


/* C7 05 <disp32> <imm32> : mov dword [disp32], imm32 */
static unsigned emit_mov_long_imm(unsigned char *p, u32 disp, u32 imm)
{
  p[0] = 0xc7; p[1] = 0x05;
  p[2] = disp; p[3] = disp >> 8; p[4] = disp >> 16; p[5] = disp >> 24;
  p[6] = imm; p[7] = imm >> 8; p[8] = imm >> 16; p[9] = imm >> 24;
  return 10;
}


static void test_ranges(void)
{
  x86emu_t *emu;
  unsigned char code[24];
  unsigned n;
  u32 lin;

  /* --- expand-up data segment, limit 0x100 --- */
  emu = new_vm(ACC_DATA32_RW, 0x100, 0x100, 16, 24);

  /* last legal byte: write offset 0x100 must succeed */
  n = emit_mov_byte_imm(code, 0x100, 0x7b);
  code[n++] = 0xf4;
  x86emu_write_dword(emu, DS_BASE + 0x100, 0xa5a5a5a5);
  run_code(emu, code, n);
  lin = DS_BASE + 0x100;
  check(fault_count == 0, "edge byte write must not fault");
  check(saw_access(lin, X86EMU_MEMIO_W), "edge byte write must reach memory");
  check((x86emu_read_dword(emu, lin) & 0xff) == 0x7b, "edge byte value stored");

  /* one byte past the limit: fault, no write, target untouched */
  n = emit_mov_byte_imm(code, 0x101, 0x7b);
  code[n++] = 0xf4;
  x86emu_write_dword(emu, DS_BASE + 0x101, 0xa5a5a5a5);
  run_code(emu, code, n);
  lin = DS_BASE + 0x101;
  check(fault_count == 1 && fault_nr == 13, "byte past limit raises #GP");
  check(!saw_access(lin, X86EMU_MEMIO_W), "byte past limit must not write");
  check(x86emu_read_dword(emu, lin) == 0xa5a5a5a5, "byte past limit keeps memory");

  /* last legal word: offsets 0xff..0x100 */
  n = emit_mov_word_imm(code, 0xff, 0x1234);
  code[n++] = 0xf4;
  run_code(emu, code, n);
  lin = DS_BASE + 0xff;
  check(fault_count == 0, "edge word write must not fault");
  check(saw_access(lin, X86EMU_MEMIO_W), "edge word write must reach memory");
  check(x86emu_read_word(emu, lin) == 0x1234, "edge word value stored");

  /* word straddling the limit: 0x100..0x101 must be rejected as a whole */
  n = emit_mov_word_imm(code, 0x100, 0x1234);
  code[n++] = 0xf4;
  x86emu_write_dword(emu, DS_BASE + 0x100, 0xa5a5a5a5);
  run_code(emu, code, n);
  lin = DS_BASE + 0x100;
  check(fault_count == 1 && fault_nr == 13, "word over limit raises #GP");
  check(!saw_access(lin, X86EMU_MEMIO_W), "word over limit must not write");
  check(x86emu_read_dword(emu, lin) == 0xa5a5a5a5, "word over limit keeps memory");

  /* dword starting at 0xfffffffe: 64-bit end math must catch the wrap */
  n = emit_mov_long_imm(code, 0xfffffffe, 0x11223344);
  code[n++] = 0xf4;
  x86emu_write_dword(emu, DS_BASE + 0x1ffe, 0xa5a5a5a5);
  run_code(emu, code, n);
  check(fault_count == 1 && fault_nr == 13, "wrapping dword raises #GP");
  check(!saw_access(DS_BASE + 0xfffffffe, X86EMU_MEMIO_W),
        "wrapping dword must not reach wrapped memory");
  check(x86emu_read_dword(emu, DS_BASE + 0x1ffe) == 0xa5a5a5a5,
        "wrapping dword leaves unrelated memory alone");

  x86emu_done(emu);

  /* --- 32-bit expand-down segment, limit 0x100, upper bound 0xffffffff --- */
  emu = new_vm(ACC_DATA32_ED, 0x100, 0x100, 16, 24);

  /* offset == limit is not part of an expand-down segment */
  n = emit_mov_byte_imm(code, 0x100, 0x11);
  code[n++] = 0xf4;
  run_code(emu, code, n);
  check(fault_count == 1 && fault_nr == 13, "expand-down offset at limit #GP");

  /* offset strictly above the limit is legal */
  n = emit_mov_byte_imm(code, 0x101, 0x22);
  code[n++] = 0xf4;
  run_code(emu, code, n);
  lin = DS_BASE + 0x101;
  check(fault_count == 0, "expand-down byte above limit is legal");
  check(saw_access(lin, X86EMU_MEMIO_W), "expand-down legal byte reaches memory");
  check(x86emu_read_byte(emu, lin) == 0x22, "expand-down legal byte value");

  /* dword 0xfffffffd..0x100000000 crosses the D/B = 1 upper bound */
  n = emit_mov_long_imm(code, 0xfffffffd, 0x33445566);
  code[n++] = 0xf4;
  run_code(emu, code, n);
  check(fault_count == 1 && fault_nr == 13, "expand-down32 dword over 0xffffffff #GP");
  check(!saw_access(DS_BASE + 0xfffffffd, X86EMU_MEMIO_W),
        "expand-down32 over-bound dword must not write");

  x86emu_done(emu);

  /* --- 16-bit expand-down segment, limit 0x10, upper bound 0xffff --- */
  emu = new_vm(ACC_DATA16_ED, 0x10, 0x100, 16, 24);

  /* word at 0x11 ends at 0x12: inside the 16-bit bound regardless of
     the 32-bit address encoding used to reach it */
  n = emit_mov_word_imm(code, 0x11, 0x4455);
  code[n++] = 0xf4;
  run_code(emu, code, n);
  lin = DS_BASE + 0x11;
  check(fault_count == 0, "expand-down16 word inside 0xffff is legal");
  check(saw_access(lin, X86EMU_MEMIO_W), "expand-down16 legal word reaches memory");

  /* byte at the limit itself is outside */
  n = emit_mov_byte_imm(code, 0x10, 0x66);
  code[n++] = 0xf4;
  run_code(emu, code, n);
  check(fault_count == 1 && fault_nr == 13, "expand-down16 byte at limit #GP");

  /* word at 0xffff ends at 0x10000: the segment bound is 0xffff, not the
     address size, so this must fault */
  n = emit_mov_word_imm(code, 0xffff, 0x7788);
  code[n++] = 0xf4;
  run_code(emu, code, n);
  check(fault_count == 1 && fault_nr == 13, "expand-down16 word over 0xffff #GP");
  check(!saw_access(DS_BASE + 0xffff, X86EMU_MEMIO_W),
        "expand-down16 over-bound word must not write");

  x86emu_done(emu);
}


static void test_permissions(void)
{
  x86emu_t *emu;
  unsigned char code[24];
  unsigned n;
  u32 flags_before, lin;

  /* --- read-only data segment --- */
  emu = new_vm(ACC_DATA32_RO, 0xffff, 0x100, 16, 24);

  x86emu_write_dword(emu, DS_BASE + 0x40, 0x99887766);

  /* reads are allowed */
  code[0] = 0xa1;	/* mov eax, [disp32] */
  code[1] = 0x40; code[2] = 0; code[3] = 0; code[4] = 0;
  code[5] = 0xf4;
  run_code(emu, code, 6);
  check(fault_count == 0, "read-only segment read is legal");
  check(emu->x86.R_EAX == 0x99887766, "read-only segment returns value");

  /* plain write is #GP, no data write, no zero transaction */
  code[0] = 0xa3;	/* mov [disp32], eax */
  code[1] = 0x40; code[2] = 0; code[3] = 0; code[4] = 0;
  code[5] = 0xf4;
  run_code(emu, code, 6);
  lin = DS_BASE + 0x40;
  check(fault_count == 1 && fault_nr == 13, "read-only write raises #GP");
  check(!saw_access(lin, X86EMU_MEMIO_W), "read-only write must not reach memory");
  check(x86emu_read_dword(emu, lin) == 0x99887766, "read-only write keeps memory");

  /* RMW must validate writability before reading: no R, no W, flags kept */
  flags_before = emu->x86.R_FLG = 0x312 | 2;
  n = 0;
  code[n++] = 0x80; code[n++] = 0x05;	/* add byte [disp32], imm8 */
  code[n++] = 0x40; code[n++] = 0; code[n++] = 0; code[n++] = 0;
  code[n++] = 0x05;
  code[n++] = 0xf4;
  run_code(emu, code, n);
  check(fault_count == 1 && fault_nr == 13, "read-only RMW raises #GP");
  check(!saw_access(lin, X86EMU_MEMIO_W) && !saw_access(lin, X86EMU_MEMIO_R),
        "read-only RMW performs no data transaction");
  check(x86emu_read_dword(emu, lin) == 0x99887766, "read-only RMW keeps memory");
  check(emu->x86.R_FLG == flags_before, "read-only RMW keeps arithmetic flags");

  x86emu_done(emu);

  /* --- readable code segment used as data --- */
  emu = new_vm(ACC_DATA32_RW, 0xffff, 0x100, 16, 24);
  x86emu_write_dword(emu, CS_BASE + 0x80, 0x0badf00d);

  code[0] = 0x2e;	/* cs: */
  code[1] = 0xa1;	/* mov eax, [disp32] */
  code[2] = 0x80; code[3] = 0; code[4] = 0; code[5] = 0;
  code[6] = 0xf4;
  run_code(emu, code, 7);
  check(fault_count == 0, "readable code segment data read is legal");
  check(emu->x86.R_EAX == 0x0badf00d, "readable code segment returns value");

  /* code segments can never be written as data */
  code[0] = 0x2e;	/* cs: */
  code[1] = 0xa3;	/* mov [disp32], eax */
  code[2] = 0x80; code[3] = 0; code[4] = 0; code[5] = 0;
  code[6] = 0xf4;
  run_code(emu, code, 7);
  check(fault_count == 1 && fault_nr == 13, "code segment write raises #GP");
  check(!saw_access(CS_BASE + 0x80, X86EMU_MEMIO_W),
        "code segment write must not reach memory");
  x86emu_done(emu);

  /* --- execute-only code segment: data read refused --- */
  emu = new_vm(ACC_DATA32_RW, 0xffff, 0x100, 16, 24);
  set_seg(emu, R_CS_INDEX, 8, CS_BASE, 0xffff, ACC_CODE32_XO);
  code[0] = 0x2e;	/* cs: */
  code[1] = 0xa1;
  code[2] = 0x80; code[3] = 0; code[4] = 0; code[5] = 0;
  code[6] = 0xf4;
  run_code(emu, code, 7);
  check(fault_count == 1 && fault_nr == 13, "execute-only code read raises #GP");
  check(!saw_access(CS_BASE + 0x80, X86EMU_MEMIO_R),
        "execute-only code read must not reach memory");
  check(emu->x86.R_EAX == 0, "execute-only code read does not load zero into eax");
  x86emu_done(emu);

  /* --- null DS and null ES --- */
  emu = new_vm(ACC_DATA32_RW, 0xffff, 0x100, 0, 24);
  code[0] = 0xa1;	/* mov eax, [disp32] via null DS */
  code[1] = 0x10; code[2] = 0; code[3] = 0; code[4] = 0;
  code[5] = 0xf4;
  run_code(emu, code, 6);
  check(fault_count == 1 && fault_nr == 13 && fault_errcode == 0,
        "null DS reference raises #GP(0)");
  check(!saw_access(DS_BASE + 0x10, X86EMU_MEMIO_R), "null DS read must not run");

  code[0] = 0x26;	/* es: */
  code[1] = 0xa1;
  code[2] = 0x10; code[3] = 0; code[4] = 0; code[5] = 0;
  code[6] = 0xf4;
  run_code(emu, code, 7);
  check(fault_count == 1 && fault_nr == 13 && fault_errcode == 0,
        "null ES reference raises #GP(0)");

  /* null FS and null GS via their override prefixes */
  emu->x86.seg[R_FS_INDEX].sel = 0;
  emu->x86.seg[R_GS_INDEX].sel = 0;

  code[0] = 0x64;	/* fs: */
  code[1] = 0xa1;
  code[2] = 0x10; code[3] = 0; code[4] = 0; code[5] = 0;
  code[6] = 0xf4;
  run_code(emu, code, 7);
  check(fault_count == 1 && fault_nr == 13 && fault_errcode == 0,
        "null FS reference raises #GP(0)");

  code[0] = 0x65;	/* gs: */
  code[1] = 0xa1;
  code[2] = 0x10; code[3] = 0; code[4] = 0; code[5] = 0;
  code[6] = 0xf4;
  run_code(emu, code, 7);
  check(fault_count == 1 && fault_nr == 13 && fault_errcode == 0,
        "null GS reference raises #GP(0)");
  x86emu_done(emu);

  /* --- SS vs DS identity: same selector value, different fault vector --- */
  emu = new_vm(ACC_DATA32_RW, 0x40, 0x80, 0x20, 0x20);
  emu->x86.R_EBP = 0x90;	/* beyond SS limit 0x80 */

  /* [ebp] defaults to SS: out of SS range raises #SS(0) */
  code[0] = 0x89; code[1] = 0x6d; code[2] = 0x00;	/* mov [ebp], ebp */
  code[3] = 0xf4;
  run_code(emu, code, 4);
  check(fault_count == 1 && fault_nr == 12 && fault_errcode == 0,
        "SS out of range raises #SS(0)");
  check(!saw_access(SS_BASE + 0x90, X86EMU_MEMIO_W), "SS fault must not write");

  /* same encoding, explicit DS override: DS limit 0x40 still rejects,
     but now as #GP because the DS register identity is used */
  emu->x86.R_EBP = 0x90;
  code[0] = 0x3e;	/* ds: */
  code[1] = 0x89; code[2] = 0x6d; code[3] = 0x00;
  code[4] = 0xf4;
  run_code(emu, code, 5);
  check(fault_count == 1 && fault_nr == 13, "DS override out of range raises #GP");
  check(!saw_access(DS_BASE + 0x90, X86EMU_MEMIO_W), "DS fault must not write");

  /* widen DS and repeat the override: BP addressing under DS now succeeds */
  set_seg(emu, R_DS_INDEX, 0x20, DS_BASE, 0x1000, ACC_DATA32_RW);
  emu->x86.R_EBP = 0x90;
  run_code(emu, code, 5);
  lin = DS_BASE + 0x90;
  check(fault_count == 0, "explicit DS override on [ebp] is legal");
  check(saw_access(lin, X86EMU_MEMIO_W), "DS override write reaches DS memory");
  check(x86emu_read_dword(emu, lin) == emu->x86.R_EBP,
        "DS override stores through DS base");

  x86emu_done(emu);
}


static void test_restart(void)
{
  x86emu_t *emu;
  unsigned char code[24];
  unsigned n;

  emu = new_vm(ACC_DATA32_RW, 0x100, 0x100, 16, 24);

  /*
   * inc eax                 (40)           -> eax 5 -> 6, completes
   * ds: mov byte [0x101],.. (3e c6 05 ..)  -> #GP, prefix is part of insn
   * mov eax, 0xdeadbeef    (b8 ..)         -> must never execute
   * hlt                     (f4)
   */
  emu->x86.R_EAX = 5;
  n = 0;
  code[n++] = 0x40;
  code[n++] = 0x3e;
  n += emit_mov_byte_imm(code + n, 0x101, 0x7b);
  code[n++] = 0xb8;
  code[n++] = 0xef; code[n++] = 0xbe; code[n++] = 0xad; code[n++] = 0xde;
  code[n++] = 0xf4;
  run_code(emu, code, n);

  check(fault_count == 1 && fault_nr == 13, "prefixed faulting insn raises #GP");
  check(fault_saved_cs == 8, "saved_cs points at the running code segment");
  check(fault_saved_eip == 1, "saved_eip includes the segment prefix byte");
  check(emu->x86.R_EAX == 6, "completed earlier insn survives, later insn skipped");
  check(x86emu_read_byte(emu, DS_BASE + 0x101) != 0x7b,
        "faulting prefixed write does not modify memory");
  check(!saw_access(DS_BASE + 0x101, X86EMU_MEMIO_W),
        "prefixed faulting write reaches no data memory");

  /* 66-prefixed word write over the limit: saved_eip points at 0x66 */
  n = emit_mov_word_imm(code, 0x100, 0x1234);
  code[n++] = 0xf4;
  run_code(emu, code, n);
  check(fault_count == 1 && fault_nr == 13, "66-prefixed word fault raises #GP");
  check(fault_saved_eip == 0, "saved_eip starts at the 66 prefix");
  check(fault_errcode == 0, "data limit #GP carries error code 0");
  check(!saw_access(DS_BASE + 0x100, X86EMU_MEMIO_W),
        "66-prefixed faulting word reaches no data memory");

  /* SS fault on a prefixed stack-style access keeps the same restart point */
  emu->x86.R_EBP = 0x90;
  set_seg(emu, R_SS_INDEX, 24, SS_BASE, 0x80, ACC_DATA32_RW);
  code[0] = 0x36;	/* ss: */
  code[1] = 0x89; code[2] = 0x6d; code[3] = 0x00;	/* mov [ebp], ebp */
  code[4] = 0xf4;
  run_code(emu, code, 5);
  check(fault_count == 1 && fault_nr == 12, "SS prefixed access raises #SS");
  check(fault_saved_eip == 0 && fault_saved_cs == 8,
        "SS fault restarts at the prefixed instruction");

  x86emu_done(emu);
}


int main(void)
{
  failures = 0;

  group_failures = 0;
  test_ranges();
  if(!group_failures) puts("PASS ranges");

  group_failures = 0;
  test_permissions();
  if(!group_failures) puts("PASS permissions");

  group_failures = 0;
  test_restart();
  if(!group_failures) puts("PASS restart");

  if(failures) {
    printf("X86EMU_SEGMENT_FAULT_FAILURES=%u\n", failures);
    return 2;
  }

  puts("X86EMU_SEGMENT_FAULT_OK");
  return 0;
}
