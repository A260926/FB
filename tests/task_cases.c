/*
task_cases.c

Task test cases for the prefix trace sideband interface
(distorm_decompose_prefix_trace, see include/prefix_trace.h).

Four groups are registered:
  roles       - the effective usage of prefix bytes (incl. mandatory selectors,
                REX, VEX and its pp-implied selector).
  suppression - why prefix bytes lost their effect (same-group override, REX
                position, REX.W precedence, mode, plain non-use).
  modes       - 16/32/64 bits specific behavior (C4/C5 as LES/LDS, 0x40-0x4f
                not being REX outside 64 bits, segment overrides in 64 bits).
  recovery    - undecodable bytes, capacity limits and feature flags behave
                exactly like the classic interface, with aligned sidebands.

Every case also cross-checks the new entry point against distorm_decompose:
same _DecodeResult, same instructions and the same advance (nextOffset).
*/

#include "task_support/check.h"

#include <string.h>

#include "distorm.h"
#include "prefix_trace.h"
#include "mnemonics.h"

#define MAXI 8

typedef struct {
	_DecodeResult res;
	unsigned int used;
	_OffsetType next;
	_DInst insts[MAXI];
	_PrefixTraceInfo traces[MAXI];
} decode_run;

/*
 * Decode the same input with the classic and the tracing interface and
 * require identical results, identical instructions and identical advance.
 */
static void run_both(decode_run* rn, const uint8_t* code, int len, _DecodeType dt, unsigned int features, unsigned int maxi)
{
	_CodeInfo ci;
	_DInst oldInsts[MAXI];
	unsigned int oldUsed = 0;
	_OffsetType oldNext;
	_DecodeResult oldRes;

	memset(&ci, 0, sizeof(ci));
	ci.code = code;
	ci.codeLen = len;
	ci.dt = dt;
	ci.features = features;
	oldRes = distorm_decompose(&ci, oldInsts, maxi, &oldUsed);
	oldNext = ci.nextOffset;

	memset(rn, 0, sizeof(*rn));
	memset(&ci, 0, sizeof(ci));
	ci.code = code;
	ci.codeLen = len;
	ci.dt = dt;
	ci.features = features;
	rn->res = distorm_decompose_prefix_trace(&ci, rn->insts, rn->traces, maxi, &rn->used);
	rn->next = ci.nextOffset;

	TASK_CHECK(rn->res == oldRes);
	TASK_CHECK(rn->used == oldUsed);
	TASK_CHECK(rn->next == oldNext);
	TASK_CHECK(memcmp(rn->insts, oldInsts, oldUsed * sizeof(_DInst)) == 0);
}

/* Assert a full prefix trace record (offset/length/byte/group/roles/reason/supersededBy). */
static void check_rec(const _PrefixTraceInfo* t, unsigned int idx,
                      unsigned int offset, unsigned int length, unsigned int byte, unsigned int group,
                      unsigned int roles, unsigned int reason, unsigned int supersededBy)
{
	const _PrefixTrace* r;
	TASK_CHECK(idx < t->count);
	r = &t->items[idx];
	TASK_CHECK(r->offset == offset);
	TASK_CHECK(r->length == length);
	TASK_CHECK(r->byte == byte);
	TASK_CHECK(r->group == group);
	TASK_CHECK(r->roles == roles);
	TASK_CHECK(r->reason == reason);
	TASK_CHECK(r->supersededBy == supersededBy);
}

/* Same as check_rec, plus the implied mandatory selector byte (VEX records). */
static void check_rec_sel(const _PrefixTraceInfo* t, unsigned int idx,
                          unsigned int offset, unsigned int length, unsigned int byte, unsigned int group,
                          unsigned int roles, unsigned int reason, unsigned int supersededBy, unsigned int selector)
{
	check_rec(t, idx, offset, length, byte, group, roles, reason, supersededBy);
	TASK_CHECK(t->items[idx].selector == selector);
}

/* ------------------------------------------------------------------ roles */

static void case_roles(void)
{
	decode_run rn;

	/* f3 selects MOVSS: a mandatory opcode selector, not a string repeat. */
	{
		static const uint8_t code[] = { 0xf3, 0x0f, 0x10, 0xc0 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.res == DECRES_SUCCESS && rn.used == 1);
		TASK_CHECK(rn.insts[0].opcode == I_MOVSS);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0xf3, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_MANDATORY, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* 66 selects MOVUPD: mandatory as well, group stays operand-size. */
	{
		static const uint8_t code[] = { 0x66, 0x0f, 0x10, 0xc0 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_MOVUPD);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x66, PFX_GROUP_OPERAND_SIZE, PFX_ROLE_MANDATORY, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* f3 90 is PAUSE: the f3 acts as a repeat-class prefix. */
	{
		static const uint8_t code[] = { 0xf3, 0x90 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_PAUSE);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0xf3, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_REPEAT, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* 66 switches the operand size of MOV to 16 bits. */
	{
		static const uint8_t code[] = { 0x66, 0x89, 0xc0 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_MOV);
		TASK_CHECK(FLAG_GET_OPSIZE(rn.insts[0].flags) == Decode16Bits);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x66, PFX_GROUP_OPERAND_SIZE, PFX_ROLE_OPERAND_SIZE, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* 67 switches the address size. */
	{
		static const uint8_t code[] = { 0x67, 0x8b, 0x00 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(FLAG_GET_ADDRSIZE(rn.insts[0].flags) == Decode16Bits);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x67, PFX_GROUP_ADDRESS_SIZE, PFX_ROLE_ADDRESS_SIZE, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* FS override takes effect on a memory operand. */
	{
		static const uint8_t code[] = { 0x64, 0x8b, 0x00 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].segment == R_FS);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x64, PFX_GROUP_SEGMENT, PFX_ROLE_SEGMENT, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* LOCK is accepted by INC [mem]. */
	{
		static const uint8_t code[] = { 0xf0, 0xff, 0x00 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK((rn.insts[0].flags & FLAG_LOCK) != 0);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0xf0, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_LOCK, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* REP is accepted by MOVSB. */
	{
		static const uint8_t code[] = { 0xf3, 0xa4 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_MOVS);
		TASK_CHECK((rn.insts[0].flags & FLAG_REP) != 0);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0xf3, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_REPEAT, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* REX.W promotes MOV to 64 bits. */
	{
		static const uint8_t code[] = { 0x48, 0x89, 0xc0 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(FLAG_GET_OPSIZE(rn.insts[0].flags) == Decode64Bits);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x48, PFX_GROUP_REX, PFX_ROLE_REX, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* 2-bytes VEX, pp == 0: VEX role only, no implied selector. */
	{
		static const uint8_t code[] = { 0xc5, 0xf8, 0x77 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_VZEROUPPER);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec_sel(&rn.traces[0], 0, 0, 2, 0xc5, PFX_GROUP_VEX, PFX_ROLE_VEX, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET, 0);
	}

	/* 2-bytes VEX with pp == 1: also implies a virtual 0x66 mandatory selector. */
	{
		static const uint8_t code[] = { 0xc5, 0xf9, 0x6f, 0xc1 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_VMOVDQA);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec_sel(&rn.traces[0], 0, 0, 2, 0xc5, PFX_GROUP_VEX, PFX_ROLE_VEX | PFX_ROLE_MANDATORY, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET, 0x66);
	}

	/* 3-bytes VEX, pp == 0: a single three bytes record. */
	{
		static const uint8_t code[] = { 0xc4, 0xc1, 0x78, 0x77 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_VZEROUPPER);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec_sel(&rn.traces[0], 0, 0, 3, 0xc4, PFX_GROUP_VEX, PFX_ROLE_VEX, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET, 0);
	}

	/* LZCNT uses both prefixes at once: 66 for operand size, f3 as selector. */
	{
		static const uint8_t code[] = { 0x66, 0xf3, 0x0f, 0xbd, 0xc0 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_LZCNT);
		TASK_CHECK(FLAG_GET_OPSIZE(rn.insts[0].flags) == Decode16Bits);
		TASK_CHECK(rn.traces[0].count == 2);
		check_rec(&rn.traces[0], 0, 0, 1, 0x66, PFX_GROUP_OPERAND_SIZE, PFX_ROLE_OPERAND_SIZE, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
		check_rec(&rn.traces[0], 1, 1, 1, 0xf3, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_MANDATORY, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* CS override before Jcc is consumed as a branch hint. */
	{
		static const uint8_t code[] = { 0x2e, 0x74, 0x00 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_JZ);
		TASK_CHECK((rn.insts[0].flags & FLAG_HINT_NOT_TAKEN) != 0);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x2e, PFX_GROUP_SEGMENT, PFX_ROLE_SEGMENT, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}
}

/* ------------------------------------------------------------ suppression */

static void case_suppression(void)
{
	decode_run rn;

	/* 66 66 90: the first 66 is overridden by the second, the second is of no use to NOP. */
	{
		static const uint8_t code[] = { 0x66, 0x66, 0x90 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_NOP);
		TASK_CHECK(rn.insts[0].unusedPrefixesMask == 0x3);
		TASK_CHECK(rn.traces[0].count == 2);
		check_rec(&rn.traces[0], 0, 0, 1, 0x66, PFX_GROUP_OPERAND_SIZE, PFX_ROLE_NONE, PFX_REASON_SUPERSEDED, 1);
		check_rec(&rn.traces[0], 1, 1, 1, 0x66, PFX_GROUP_OPERAND_SIZE, PFX_ROLE_NONE, PFX_REASON_NOT_USED, PFX_TRACE_NO_OFFSET);
	}

	/* 48 66 0f 10 c0: same mask bit 0 as above, different story - the REX sits
	 * too far from the opcode and the 66 is MOVUPD's mandatory selector. */
	{
		static const uint8_t code[] = { 0x48, 0x66, 0x0f, 0x10, 0xc0 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_MOVUPD);
		TASK_CHECK(rn.insts[0].unusedPrefixesMask == 0x1);
		TASK_CHECK(rn.traces[0].count == 2);
		check_rec(&rn.traces[0], 0, 0, 1, 0x48, PFX_GROUP_REX, PFX_ROLE_NONE, PFX_REASON_REX_POSITION, PFX_TRACE_NO_OFFSET);
		check_rec(&rn.traces[0], 1, 1, 1, 0x66, PFX_GROUP_OPERAND_SIZE, PFX_ROLE_MANDATORY, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* f3 f3 a4: repeated REP, the first one is overridden. */
	{
		static const uint8_t code[] = { 0xf3, 0xf3, 0xa4 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK((rn.insts[0].flags & FLAG_REP) != 0);
		TASK_CHECK(rn.traces[0].count == 2);
		check_rec(&rn.traces[0], 0, 0, 1, 0xf3, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_NONE, PFX_REASON_SUPERSEDED, 1);
		check_rec(&rn.traces[0], 1, 1, 1, 0xf3, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_REPEAT, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* ES overridden by a later FS, which then takes effect. */
	{
		static const uint8_t code[] = { 0x26, 0x64, 0x8b, 0x00 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].segment == R_FS);
		TASK_CHECK(rn.traces[0].count == 2);
		check_rec(&rn.traces[0], 0, 0, 1, 0x26, PFX_GROUP_SEGMENT, PFX_ROLE_NONE, PFX_REASON_SUPERSEDED, 1);
		check_rec(&rn.traces[0], 1, 1, 1, 0x64, PFX_GROUP_SEGMENT, PFX_ROLE_SEGMENT, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* 66 48 98 (CDQE): REX.W takes precedence over the operand-size prefix. */
	{
		static const uint8_t code[] = { 0x66, 0x48, 0x98 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_CDQE);
		TASK_CHECK(rn.insts[0].unusedPrefixesMask == 0x1);
		TASK_CHECK(rn.traces[0].count == 2);
		check_rec(&rn.traces[0], 0, 0, 1, 0x66, PFX_GROUP_OPERAND_SIZE, PFX_ROLE_NONE, PFX_REASON_REX_W_OP_SIZE, 1);
		check_rec(&rn.traces[0], 1, 1, 1, 0x48, PFX_GROUP_REX, PFX_ROLE_REX, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* 41 42 90: the second REX overrides the first, and NOP needs neither. */
	{
		static const uint8_t code[] = { 0x41, 0x42, 0x90 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].unusedPrefixesMask == 0x3);
		TASK_CHECK(rn.traces[0].count == 2);
		check_rec(&rn.traces[0], 0, 0, 1, 0x41, PFX_GROUP_REX, PFX_ROLE_NONE, PFX_REASON_SUPERSEDED, 1);
		check_rec(&rn.traces[0], 1, 1, 1, 0x42, PFX_GROUP_REX, PFX_ROLE_NONE, PFX_REASON_NOT_USED, PFX_TRACE_NO_OFFSET);
	}

	/* 41 67 90: REX invalidated by its position (67 precedes the opcode). */
	{
		static const uint8_t code[] = { 0x41, 0x67, 0x90 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].unusedPrefixesMask == 0x3);
		TASK_CHECK(rn.traces[0].count == 2);
		check_rec(&rn.traces[0], 0, 0, 1, 0x41, PFX_GROUP_REX, PFX_ROLE_NONE, PFX_REASON_REX_POSITION, PFX_TRACE_NO_OFFSET);
		check_rec(&rn.traces[0], 1, 1, 1, 0x67, PFX_GROUP_ADDRESS_SIZE, PFX_ROLE_NONE, PFX_REASON_NOT_USED, PFX_TRACE_NO_OFFSET);
	}

	/* 41 c5 f8 77: REX dropped before VEX, VEX itself is fully effective. */
	{
		static const uint8_t code[] = { 0x41, 0xc5, 0xf8, 0x77 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_VZEROUPPER);
		TASK_CHECK(rn.traces[0].count == 2);
		check_rec(&rn.traces[0], 0, 0, 1, 0x41, PFX_GROUP_REX, PFX_ROLE_NONE, PFX_REASON_REX_POSITION, PFX_TRACE_NO_OFFSET);
		check_rec_sel(&rn.traces[0], 1, 1, 2, 0xc5, PFX_GROUP_VEX, PFX_ROLE_VEX, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET, 0);
	}

	/* f3 f0 ae: the decoder keeps the REP of the first byte alive although a
	 * LOCK byte follows it, so the scan-time override is revoked by the use. */
	{
		static const uint8_t code[] = { 0xf3, 0xf0, 0xae };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_SCAS);
		TASK_CHECK((rn.insts[0].flags & FLAG_REP) != 0);
		TASK_CHECK(rn.traces[0].count == 2);
		check_rec(&rn.traces[0], 0, 0, 1, 0xf3, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_REPEAT, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
		check_rec(&rn.traces[0], 1, 1, 1, 0xf0, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_NONE, PFX_REASON_NOT_USED, PFX_TRACE_NO_OFFSET);
	}

	/* 66 a4: MOVSB has no use for an operand-size prefix. */
	{
		static const uint8_t code[] = { 0x66, 0xa4 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].unusedPrefixesMask == 0x1);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x66, PFX_GROUP_OPERAND_SIZE, PFX_ROLE_NONE, PFX_REASON_NOT_USED, PFX_TRACE_NO_OFFSET);
	}

	/* 3e 8b 00: DS is the default segment here, the override is pointless. */
	{
		static const uint8_t code[] = { 0x3e, 0x8b, 0x00 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].unusedPrefixesMask == 0x1);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x3e, PFX_GROUP_SEGMENT, PFX_ROLE_NONE, PFX_REASON_NOT_USED, PFX_TRACE_NO_OFFSET);
	}

	/* f3 0f 28 c0: no f3-variant of MOVAPS exists, the decoder falls back to
	 * the non-prefixed instruction - the byte changed nothing. (The classic
	 * mask still calls it "used", the trace tells what really happened.) */
	{
		static const uint8_t code[] = { 0xf3, 0x0f, 0x28, 0xc0 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_MOVAPS);
		TASK_CHECK(rn.insts[0].unusedPrefixesMask == 0x0);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0xf3, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_NONE, PFX_REASON_NOT_USED, PFX_TRACE_NO_OFFSET);
	}
}

/* ----------------------------------------------------------------- modes */

static void case_modes(void)
{
	decode_run rn;

	/* 64 bits: CS override is void. */
	{
		static const uint8_t code[] = { 0x2e, 0x8b, 0x00 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].unusedPrefixesMask == 0x1);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x2e, PFX_GROUP_SEGMENT, PFX_ROLE_NONE, PFX_REASON_MODE_IGNORED, PFX_TRACE_NO_OFFSET);
	}

	/* 64 bits: FS override still works (even though the classic mask cannot
	 * express that and reports it as unused). */
	{
		static const uint8_t code[] = { 0x64, 0x8b, 0x00 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].segment == R_FS);
		TASK_CHECK(rn.insts[0].unusedPrefixesMask == 0x1);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x64, PFX_GROUP_SEGMENT, PFX_ROLE_SEGMENT, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* 16 bits: 66 switches MOV to 32 bits operands. */
	{
		static const uint8_t code[] = { 0x66, 0x89, 0xc0 };
		run_both(&rn, code, sizeof(code), Decode16Bits, DF_NONE, MAXI);
		TASK_CHECK(FLAG_GET_OPSIZE(rn.insts[0].flags) == Decode32Bits);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x66, PFX_GROUP_OPERAND_SIZE, PFX_ROLE_OPERAND_SIZE, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* 16 bits: 67 switches to 32 bits addressing. */
	{
		static const uint8_t code[] = { 0x67, 0x8b, 0x00 };
		run_both(&rn, code, sizeof(code), Decode16Bits, DF_NONE, MAXI);
		TASK_CHECK(FLAG_GET_ADDRSIZE(rn.insts[0].flags) == Decode32Bits);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x67, PFX_GROUP_ADDRESS_SIZE, PFX_ROLE_ADDRESS_SIZE, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* 32 bits: c5 with a low second byte is LDS, not a VEX prefix. */
	{
		static const uint8_t code[] = { 0xc5, 0x20 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_LDS);
		TASK_CHECK(rn.insts[0].size == 2);
		TASK_CHECK(rn.traces[0].count == 0);
	}

	/* 32 bits: c4 with a low second byte is LES, not a VEX prefix. */
	{
		static const uint8_t code[] = { 0xc4, 0x20 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_LES);
		TASK_CHECK(rn.insts[0].size == 2);
		TASK_CHECK(rn.traces[0].count == 0);
	}

	/* 32 bits: a genuine VEX prefix is still reported as such. */
	{
		static const uint8_t code[] = { 0xc5, 0xf8, 0x77 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.insts[0].opcode == I_VZEROUPPER);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec_sel(&rn.traces[0], 0, 0, 2, 0xc5, PFX_GROUP_VEX, PFX_ROLE_VEX, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET, 0);
	}

	/* 32 bits: 0x40 is INC EAX, not a REX prefix - no records at all. */
	{
		static const uint8_t code[] = { 0x40, 0x90 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.used == 2);
		TASK_CHECK(rn.insts[0].opcode == I_INC && rn.insts[0].size == 1);
		TASK_CHECK(rn.insts[1].opcode == I_NOP);
		TASK_CHECK(rn.traces[0].count == 0);
		TASK_CHECK(rn.traces[1].count == 0);
	}

	/* 16 bits: 0x40 is INC AX, not a REX prefix either. */
	{
		static const uint8_t code[] = { 0x40 };
		run_both(&rn, code, sizeof(code), Decode16Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.used == 1);
		TASK_CHECK(rn.insts[0].opcode == I_INC);
		TASK_CHECK(rn.traces[0].count == 0);
	}

	/* 32 bits: 0x48 is DEC EAX; the same bytes are MOV RAX, RAX in 64 bits. */
	{
		static const uint8_t code[] = { 0x48, 0x89, 0xc0 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.used == 2);
		TASK_CHECK(rn.insts[0].opcode == I_DEC && rn.insts[0].size == 1);
		TASK_CHECK(rn.insts[1].opcode == I_MOV);
		TASK_CHECK(rn.traces[0].count == 0);
		TASK_CHECK(rn.traces[1].count == 0);
	}
}

/* -------------------------------------------------------------- recovery */

static void case_recovery(void)
{
	decode_run rn;

	/* Undecodable bytes in the stream: they come out as DB with an empty
	 * sideband, and the following instructions start with a clean trace. */
	{
		static const uint8_t code[] = { 0x62, 0xf3, 0x0f, 0x10, 0xc0, 0xd6, 0x66, 0x66, 0x90 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.res == DECRES_SUCCESS && rn.used == 4);
		TASK_CHECK(rn.next == sizeof(code));

		TASK_CHECK(rn.insts[0].flags == FLAG_NOT_DECODABLE && rn.insts[0].imm.byte == 0x62);
		TASK_CHECK(rn.traces[0].count == 0);

		TASK_CHECK(rn.insts[1].opcode == I_MOVSS);
		TASK_CHECK(rn.traces[1].count == 1);
		check_rec(&rn.traces[1], 0, 0, 1, 0xf3, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_MANDATORY, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);

		TASK_CHECK(rn.insts[2].flags == FLAG_NOT_DECODABLE && rn.insts[2].imm.byte == 0xd6);
		TASK_CHECK(rn.traces[2].count == 0);

		TASK_CHECK(rn.insts[3].opcode == I_NOP);
		TASK_CHECK(rn.traces[3].count == 2);
		check_rec(&rn.traces[3], 0, 0, 1, 0x66, PFX_GROUP_OPERAND_SIZE, PFX_ROLE_NONE, PFX_REASON_SUPERSEDED, 1);
		check_rec(&rn.traces[3], 1, 1, 1, 0x66, PFX_GROUP_OPERAND_SIZE, PFX_ROLE_NONE, PFX_REASON_NOT_USED, PFX_TRACE_NO_OFFSET);
	}

	/* f2 f3 ...: both REP-classes together are illegal, the first decode fails
	 * *after* scanning two prefix bytes - the DB entry must not keep them, and
	 * the recovered MOVSS must see only its own f3 at offset 0. */
	{
		static const uint8_t code[] = { 0xf2, 0xf3, 0x0f, 0x10, 0xc0 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.used == 2);
		TASK_CHECK(rn.insts[0].flags == FLAG_NOT_DECODABLE && rn.insts[0].imm.byte == 0xf2);
		TASK_CHECK(rn.traces[0].count == 0);
		TASK_CHECK(rn.insts[1].opcode == I_MOVSS);
		TASK_CHECK(rn.traces[1].count == 1);
		check_rec(&rn.traces[1], 0, 0, 1, 0xf3, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_MANDATORY, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* 66 9b ...: prefixes of a WAIT-inclusive instruction are dropped. */
	{
		static const uint8_t code[] = { 0x66, 0x9b, 0xd9, 0x7d, 0xfc };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, MAXI);
		TASK_CHECK(rn.used == 1);
		TASK_CHECK(rn.insts[0].opcode == I_FSTCW);
		TASK_CHECK(rn.insts[0].unusedPrefixesMask == 0x1);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x66, PFX_GROUP_OPERAND_SIZE, PFX_ROLE_NONE, PFX_REASON_NOT_USED, PFX_TRACE_NO_OFFSET);
	}

	/* DF_RETURN_FC_ONLY: filtered-out instructions consume no sideband slot,
	 * the surviving results and traces stay aligned one to one. */
	{
		static const uint8_t code[] = { 0x2e, 0x74, 0x00, 0x90, 0x3e, 0x75, 0x00 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_RETURN_FC_ONLY, MAXI);
		TASK_CHECK(rn.used == 2);
		TASK_CHECK(rn.next == sizeof(code));
		TASK_CHECK(rn.insts[0].opcode == I_JZ);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0x2e, PFX_GROUP_SEGMENT, PFX_ROLE_SEGMENT, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
		TASK_CHECK(rn.insts[1].opcode == I_JNZ);
		TASK_CHECK(rn.traces[1].count == 1);
		check_rec(&rn.traces[1], 0, 0, 1, 0x3e, PFX_GROUP_SEGMENT, PFX_ROLE_SEGMENT, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}

	/* DF_STOP_ON_UNDECODEABLE: stops right at the bad byte, same as classic. */
	{
		static const uint8_t code[] = { 0x90, 0x62, 0x90 };
		run_both(&rn, code, sizeof(code), Decode64Bits, DF_STOP_ON_UNDECODEABLE, MAXI);
		TASK_CHECK(rn.used == 2);
		TASK_CHECK(rn.insts[0].opcode == I_NOP);
		TASK_CHECK(rn.traces[0].count == 0);
		TASK_CHECK(rn.insts[1].flags == FLAG_NOT_DECODABLE && rn.insts[1].imm.byte == 0x62);
		TASK_CHECK(rn.traces[1].count == 0);
	}

	/* DF_SINGLE_BYTE_STEP: resyncs one byte at a time; the following
	 * instructions must not inherit the records of the prefixed one. */
	{
		static const uint8_t code[] = { 0xf3, 0x0f, 0x10, 0xc0 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_SINGLE_BYTE_STEP, MAXI);
		TASK_CHECK(rn.used == 4);
		TASK_CHECK(rn.next == sizeof(code));
		TASK_CHECK(rn.insts[0].opcode == I_MOVSS);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0xf3, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_MANDATORY, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
		TASK_CHECK(rn.insts[1].opcode == I_MOVUPS);
		TASK_CHECK(rn.traces[1].count == 0);
		TASK_CHECK(rn.traces[2].count == 0);
		TASK_CHECK(rn.insts[3].flags == FLAG_NOT_DECODABLE);
		TASK_CHECK(rn.traces[3].count == 0);
	}

	/* Capacity too small: DECRES_MEMORYERR, and the advance matches the
	 * classic entry point exactly (checked inside run_both). */
	{
		static const uint8_t code[] = { 0xf3, 0x0f, 0x10, 0xc0, 0x90 };
		run_both(&rn, code, sizeof(code), Decode32Bits, DF_NONE, 1);
		TASK_CHECK(rn.res == DECRES_MEMORYERR);
		TASK_CHECK(rn.used == 1);
		TASK_CHECK(rn.next == 4);
		TASK_CHECK(rn.insts[0].opcode == I_MOVSS);
		TASK_CHECK(rn.traces[0].count == 1);
		check_rec(&rn.traces[0], 0, 0, 1, 0xf3, PFX_GROUP_LOCK_REPEAT, PFX_ROLE_MANDATORY, PFX_REASON_NONE, PFX_TRACE_NO_OFFSET);
	}
}

/* ------------------------------------------------------- registration */

static void run_roles(void) { case_roles(); }
static void run_suppression(void) { case_suppression(); }
static void run_modes(void) { case_modes(); }
static void run_recovery(void) { case_recovery(); }

const struct task_case task_cases[] = {
	{ "roles", run_roles },
	{ "suppression", run_suppression },
	{ "modes", run_modes },
	{ "recovery", run_recovery },
};

const unsigned int task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
