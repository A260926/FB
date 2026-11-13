/*
task_cases.c

Prefix tracing task test-suite.

This file intentionally has no main(): task_support/main.c drives the four
registered groups. Every expectation below (group, roles, reason, byte
offset/length) is written independently from the production decoder, and each
case also compares the prefix-trace entry against the plain decompose entry
(instruction stream, per-instruction structures and stream advancement).
*/

#include "task_support/check.h"
#include "distorm.h"
#include "prefix_trace.h"

#include <string.h>

#define CAP 16

/* Run both entries on the same input and require identical observable output. */
static void compare_entries(const unsigned char* code, int len, _DecodeType dt,
                            unsigned features, unsigned cap,
                            _DInst outRef[], _DInst outTrace[],
                            _PrefixTraceInfo trace[],
                            unsigned* usedRef, unsigned* usedTrace,
                            _DecodeResult* resRef, _DecodeResult* resTrace,
                            _OffsetType* nextRef, _OffsetType* nextTrace)
{
	_CodeInfo ciRef, ciTrace;

	memset(&ciRef, 0, sizeof(ciRef));
	memset(&ciTrace, 0, sizeof(ciTrace));
	ciRef.code = ciTrace.code = code;
	ciRef.codeLen = ciTrace.codeLen = len;
	ciRef.dt = ciTrace.dt = dt;
	ciRef.features = ciTrace.features = features;
	ciRef.codeOffset = ciTrace.codeOffset = 0x2000;

	memset(outRef, 0xCC, sizeof(_DInst) * cap);
	memset(outTrace, 0x55, sizeof(_DInst) * cap);
	memset(trace, 0xAA, sizeof(_PrefixTraceInfo) * cap);

	*resRef = distorm_decompose(&ciRef, outRef, cap, usedRef);
	*resTrace = distorm_decompose_prefix_trace(&ciTrace, outTrace, trace, cap, usedTrace);
	*nextRef = ciRef.nextOffset;
	*nextTrace = ciTrace.nextOffset;

	/* Same result code, same number of emitted instructions. */
	TASK_CHECK(*resRef == *resTrace);
	TASK_CHECK(*usedRef == *usedTrace);
	TASK_CHECK(*nextRef == *nextTrace);

	/* The filtered instruction streams must be byte-for-byte identical. */
	{
		unsigned i;
		for (i = 0; i < *usedRef; i++) {
			TASK_CHECK(memcmp(&outRef[i], &outTrace[i], sizeof(_DInst)) == 0);
		}
	}
}

static const _PrefixTrace* find_record(const _PrefixTraceInfo* ti, unsigned offset)
{
	unsigned i;
	for (i = 0; i < ti->count; i++) {
		if (ti->prefixes[i].offset == offset) return &ti->prefixes[i];
	}
	return NULL;
}

static void check_basic_record(const _PrefixTrace* p, unsigned offset, unsigned length,
                               unsigned char firstByte, unsigned char group)
{
	TASK_CHECK(p != NULL);
	if (p == NULL) return;
	TASK_CHECK(p->offset == offset);
	TASK_CHECK(p->length == length);
	TASK_CHECK(p->byteCount == length);
	TASK_CHECK(p->bytes[0] == firstByte);
	TASK_CHECK(p->group == group);
}

/*
 * Group 1: roles.
 * Every way a prefix can genuinely participate in decoding must be reported
 * with the right role flag(s) and byte position.
 */
static void run_roles(void)
{
	_DInst ref[CAP], got[CAP];
	_PrefixTraceInfo tr[CAP];
	unsigned uRef, uTr, i;
	_DecodeResult rRef, rTr;
	_OffsetType nRef, nTr;
	const _PrefixTrace* p;

	/* Duplicate 0x66, second one really switches operand size to 16 bits. */
	{
		const unsigned char b[] = {0x66, 0x66, 0xff, 0xc0}; /* INC AX */
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(rTr == DECRES_SUCCESS && uTr == 1);
		TASK_CHECK(tr[0].count == 2);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x66, PT_GROUP_OPERAND_SIZE);
		TASK_CHECK(p->roles == PT_ROLE_NONE);
		TASK_CHECK(p->reason == PT_REASON_GROUP_OVERRIDE);
		TASK_CHECK(p->overriddenByOffset == 1);
		p = find_record(&tr[0], 1);
		check_basic_record(p, 1, 1, 0x66, PT_GROUP_OPERAND_SIZE);
		TASK_CHECK(p->roles == PT_ROLE_OPERAND_SIZE);
		TASK_CHECK(p->reason == PT_REASON_NONE);
		TASK_CHECK(p->vexImpliedPp == 0xff);
	}

	/* F3 selects the SSE instruction, it is not a string-repeat prefix. */
	{
		const unsigned char b[] = {0xf3, 0x0f, 0x10, 0xc0}; /* MOVSS */
		compare_entries(b, (int)sizeof(b), Decode32Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0xf3, PT_GROUP_LOCK_REPEAT);
		TASK_CHECK(p->roles == PT_ROLE_MANDATORY_SELECTOR);
		TASK_CHECK((p->roles & PT_ROLE_LOCK_REPEAT) == 0);
		TASK_CHECK(p->reason == PT_REASON_NONE);
	}

	/* LZCNT: 0x66 is a real operand-size prefix and F3 is the mandatory selector. */
	{
		const unsigned char b[] = {0x66, 0xf3, 0x0f, 0xbd, 0xc0}; /* LZCNT AX, AX */
		compare_entries(b, (int)sizeof(b), Decode32Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1 && got[0].unusedPrefixesMask == 0);
		TASK_CHECK(tr[0].count == 2);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x66, PT_GROUP_OPERAND_SIZE);
		TASK_CHECK(p->roles == PT_ROLE_OPERAND_SIZE);
		p = find_record(&tr[0], 1);
		check_basic_record(p, 1, 1, 0xf3, PT_GROUP_LOCK_REPEAT);
		TASK_CHECK(p->roles == PT_ROLE_MANDATORY_SELECTOR);
		TASK_CHECK(p->reason == PT_REASON_NONE);
	}

	/* 2 byte VEX, pp=1: VEX role plus virtual mandatory selector. */
	{
		const unsigned char b[] = {0xc5, 0xf9, 0x10, 0xc0}; /* VMOVUPD */
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		TASK_CHECK(tr[0].count == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 2, 0xc5, PT_GROUP_VEX);
		TASK_CHECK(p->bytes[1] == 0xf9);
		TASK_CHECK((p->roles & PT_ROLE_VEX) != 0);
		TASK_CHECK((p->roles & PT_ROLE_MANDATORY_SELECTOR) != 0);
		TASK_CHECK(p->vexImpliedPp == 1);
		TASK_CHECK(p->reason == PT_REASON_NONE);
	}

	/* 3 byte VEX, pp=0: only the VEX role, no invented virtual bytes. */
	{
		const unsigned char b[] = {0xc4, 0xe1, 0x78, 0x10, 0xc0}; /* VMOVUPS */
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		TASK_CHECK(tr[0].count == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 3, 0xc4, PT_GROUP_VEX);
		TASK_CHECK(p->bytes[1] == 0xe1 && p->bytes[2] == 0x78);
		TASK_CHECK(p->roles == PT_ROLE_VEX);
		TASK_CHECK(p->vexImpliedPp == 0);
	}

	/* REP keeps its repeat meaning on a string instruction. */
	{
		const unsigned char b[] = {0xf3, 0xa4}; /* REP MOVSB */
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0xf3, PT_GROUP_LOCK_REPEAT);
		TASK_CHECK(p->roles == PT_ROLE_LOCK_REPEAT);
	}

	/* LOCK role. */
	{
		const unsigned char b[] = {0xf0, 0x00, 0x00}; /* LOCK ADD [RAX], AL */
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0xf0, PT_GROUP_LOCK_REPEAT);
		TASK_CHECK((p->roles & PT_ROLE_LOCK_REPEAT) != 0);
	}

	/* Address-size prefix really changes the effective address size. */
	{
		const unsigned char b[] = {0x67, 0x8b, 0x00}; /* 32 bit mode: 16 bit addressing */
		compare_entries(b, (int)sizeof(b), Decode32Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x67, PT_GROUP_ADDRESS_SIZE);
		TASK_CHECK(p->roles == PT_ROLE_ADDRESS_SIZE);
	}

	/* Segment override really selects the segment, plus a working REX. */
	{
		const unsigned char b[] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x64, PT_GROUP_SEGMENT);
		TASK_CHECK(p->roles == PT_ROLE_SEGMENT);
		p = find_record(&tr[0], 1);
		check_basic_record(p, 1, 1, 0x48, PT_GROUP_REX);
		TASK_CHECK(p->roles == PT_ROLE_REX);
	}

	/* Every emitted trace slot is aligned with its instruction slot. */
	for (i = 0; i < uTr; i++) {
		unsigned j;
		for (j = 0; j < tr[i].count; j++) {
			TASK_CHECK(tr[i].prefixes[j].offset < got[i].size);
		}
	}
}

/*
 * Group 2: suppression.
 * Each distinct reason for a prefix having no effect must be distinguishable
 * and carry the offset of the suppressing byte when one exists.
 */
static void run_suppression(void)
{
	_DInst ref[CAP], got[CAP];
	_PrefixTraceInfo tr[CAP];
	unsigned uRef, uTr;
	_DecodeResult rRef, rTr;
	_OffsetType nRef, nTr;
	const _PrefixTrace* p;

	/* REX too far from the opcode: position failure, the 0x66 is mandatory. */
	{
		const unsigned char b[] = {0x48, 0x66, 0x0f, 0x10, 0xc0};
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1 && got[0].unusedPrefixesMask == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x48, PT_GROUP_REX);
		TASK_CHECK(p->reason == PT_REASON_REX_POSITION);
		TASK_CHECK(p->roles == PT_ROLE_NONE);
		p = find_record(&tr[0], 1);
		check_basic_record(p, 1, 1, 0x66, PT_GROUP_OPERAND_SIZE);
		TASK_CHECK(p->roles == PT_ROLE_MANDATORY_SELECTOR);
	}

	/* Adjacent REX.W forces 64 bits and neutralizes an ordinary 0x66. */
	{
		const unsigned char b[] = {0x66, 0x48, 0x98}; /* CDQE */
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x66, PT_GROUP_OPERAND_SIZE);
		TASK_CHECK(p->reason == PT_REASON_REX_W_OVER_66);
		TASK_CHECK(p->overriddenByOffset == 1);
		p = find_record(&tr[0], 1);
		check_basic_record(p, 1, 1, 0x48, PT_GROUP_REX);
		TASK_CHECK(p->roles == PT_ROLE_REX);
		TASK_CHECK(p->reason == PT_REASON_NONE);
	}

	/* CS override is ignored by long mode. */
	{
		const unsigned char b[] = {0x2e, 0x90};
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x2e, PT_GROUP_SEGMENT);
		TASK_CHECK(p->reason == PT_REASON_MODE_IGNORED);
	}

	/* LEA never consumes a segment override, even in 32 bit mode. */
	{
		const unsigned char b[] = {0x2e, 0x8d, 0x00};
		compare_entries(b, (int)sizeof(b), Decode32Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x2e, PT_GROUP_SEGMENT);
		TASK_CHECK(p->reason == PT_REASON_INSTRUCTION_UNUSED);
	}

	/* A REX without any extended register/width effect on NOP is simply unused. */
	{
		const unsigned char b[] = {0x40, 0x90};
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x40, PT_GROUP_REX);
		TASK_CHECK(p->reason == PT_REASON_INSTRUCTION_UNUSED);
	}

	/* SS prefix matching the default segment does not override anything. */
	{
		const unsigned char b[] = {0x36, 0x8b, 0x04, 0x24}; /* MOV EAX, [ESP] */
		compare_entries(b, (int)sizeof(b), Decode32Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x36, PT_GROUP_SEGMENT);
		TASK_CHECK(p->reason == PT_REASON_INSTRUCTION_UNUSED);
	}

	/* Repeated segment overrides: first is group-overridden, last mode-ignored. */
	{
		const unsigned char b[] = {0x2e, 0x2e, 0x90};
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1 && tr[0].count == 2);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x2e, PT_GROUP_SEGMENT);
		TASK_CHECK(p->reason == PT_REASON_GROUP_OVERRIDE);
		TASK_CHECK(p->overriddenByOffset == 1);
		p = find_record(&tr[0], 1);
		check_basic_record(p, 1, 1, 0x2e, PT_GROUP_SEGMENT);
		TASK_CHECK(p->reason == PT_REASON_MODE_IGNORED);
	}

	/* REPNZ then REP on MOVSB: the earlier repeat byte loses within its group. */
	{
		const unsigned char b[] = {0xf2, 0xf3, 0xa4};
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0xf2, PT_GROUP_LOCK_REPEAT);
		TASK_CHECK(p->reason == PT_REASON_GROUP_OVERRIDE);
		TASK_CHECK(p->overriddenByOffset == 1);
		p = find_record(&tr[0], 1);
		check_basic_record(p, 1, 1, 0xf3, PT_GROUP_LOCK_REPEAT);
		TASK_CHECK((p->roles & PT_ROLE_LOCK_REPEAT) != 0);
		TASK_CHECK(p->reason == PT_REASON_NONE);
	}

	/* Two NOP operand-size prefixes: override then instruction-unused. */
	{
		const unsigned char b[] = {0x66, 0x66, 0x90};
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1 && got[0].unusedPrefixesMask == 0x3);
		p = find_record(&tr[0], 0);
		TASK_CHECK(p->reason == PT_REASON_GROUP_OVERRIDE && p->overriddenByOffset == 1);
		p = find_record(&tr[0], 1);
		TASK_CHECK(p->reason == PT_REASON_INSTRUCTION_UNUSED);
	}

	/*
	 * F2 then LOCK before an escaped opcode: the scan-time group bookkeeping
	 * would say F2 lost to the later LOCK, but opcode selection really consumes
	 * F2 as the mandatory selector (MOVSD). The later, more specific decision
	 * wins, so F2 is reported as a selector and only LOCK is unused.
	 */
	{
		const unsigned char b[] = {0xf2, 0xf0, 0x0f, 0x10, 0xc0};
		compare_entries(b, (int)sizeof(b), Decode32Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0xf2, PT_GROUP_LOCK_REPEAT);
		TASK_CHECK(p->roles == PT_ROLE_MANDATORY_SELECTOR);
		TASK_CHECK(p->reason == PT_REASON_NONE);
		p = find_record(&tr[0], 1);
		check_basic_record(p, 1, 1, 0xf0, PT_GROUP_LOCK_REPEAT);
		TASK_CHECK(p->roles == PT_ROLE_NONE);
		TASK_CHECK(p->reason == PT_REASON_INSTRUCTION_UNUSED);
	}

	/*
	 * F3 before EMMS has no mandatory form, so the byte truly has no effect.
	 * The production mask cannot show this (the failed selector lookup already
	 * cleared its bookkeeping), yet the trace must still explain it accurately.
	 */
	{
		const unsigned char b[] = {0xf3, 0x0f, 0x77};
		compare_entries(b, (int)sizeof(b), Decode32Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1 && got[0].unusedPrefixesMask == 0);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0xf3, PT_GROUP_LOCK_REPEAT);
		TASK_CHECK(p->roles == PT_ROLE_NONE);
		TASK_CHECK(p->reason == PT_REASON_INSTRUCTION_UNUSED);
	}
}

/*
 * Group 3: modes.
 * 16/32/64 bit decoding differences, including C4/C5 and 0x40-0x4f not being
 * VEX/REX outside long mode.
 */
static void run_modes(void)
{
	_DInst ref[CAP], got[CAP];
	_PrefixTraceInfo tr[CAP];
	unsigned uRef, uTr;
	_DecodeResult rRef, rTr;
	_OffsetType nRef, nTr;
	const _PrefixTrace* p;

	/* 16 bit mode: 0x66 promotes the operand size, so it is genuinely used. */
	{
		const unsigned char b[] = {0x66, 0x89, 0xc0}; /* MOV EAX, EAX */
		compare_entries(b, (int)sizeof(b), Decode16Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x66, PT_GROUP_OPERAND_SIZE);
		TASK_CHECK(p->roles == PT_ROLE_OPERAND_SIZE);
	}

	/* 16 bit mode address-size override. */
	{
		const unsigned char b[] = {0x67, 0xa4};
		compare_entries(b, (int)sizeof(b), Decode16Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x67, PT_GROUP_ADDRESS_SIZE);
		TASK_CHECK(p->roles == PT_ROLE_ADDRESS_SIZE);
	}

	/* 32 bit mode: C5 with Mod != 11 is plain LDS, hence no VEX record. */
	{
		const unsigned char b[] = {0xc5, 0x00}; /* LDS EAX, [EAX] */
		compare_entries(b, (int)sizeof(b), Decode32Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		TASK_CHECK(got[0].flags != FLAG_NOT_DECODABLE);
		TASK_CHECK(got[0].size == 2);
		TASK_CHECK(tr[0].count == 0);
	}

	/* 32 bit mode: 0x40 is INC EAX, not a REX prefix. */
	{
		const unsigned char b[] = {0x40, 0x90};
		compare_entries(b, (int)sizeof(b), Decode32Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 2);
		TASK_CHECK(tr[0].count == 0 && tr[1].count == 0);
	}

	/* Same bytes in 64 bit mode are a REX-prefixed NOP with one REX record. */
	{
		const unsigned char b[] = {0x40, 0x90};
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		TASK_CHECK(tr[0].count == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x40, PT_GROUP_REX);
	}

	/* 32 bit mode still accepts a well-formed 2 byte VEX (second byte >= 0xc0). */
	{
		const unsigned char b[] = {0xc5, 0xf8, 0x10, 0xc0};
		compare_entries(b, (int)sizeof(b), Decode32Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 2, 0xc5, PT_GROUP_VEX);
		TASK_CHECK(p->vexImpliedPp == 0);
	}

	/* ES override works in 32 bit mode, but is mode-ignored in long mode. */
	{
		const unsigned char b[] = {0x26, 0x8b, 0x00};
		compare_entries(b, (int)sizeof(b), Decode32Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x26, PT_GROUP_SEGMENT);
		TASK_CHECK(p->roles == PT_ROLE_SEGMENT);

		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x26, PT_GROUP_SEGMENT);
		TASK_CHECK(p->reason == PT_REASON_MODE_IGNORED);
	}

	/* FS stays effective in long mode, but on a NOP it has no memory operand. */
	{
		const unsigned char b[] = {0x64, 0x90};
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		p = find_record(&tr[0], 0);
		check_basic_record(p, 0, 1, 0x64, PT_GROUP_SEGMENT);
		TASK_CHECK(p->reason == PT_REASON_INSTRUCTION_UNUSED);
	}
}

/*
 * Group 4: recovery.
 * Undecodable bytes, capacity limits, feature filtering and input validation:
 * the side-band must stay aligned and never leak across instructions.
 */
static void run_recovery(void)
{
	_DInst ref[CAP], got[CAP];
	_PrefixTraceInfo tr[CAP];
	unsigned uRef, uTr;
	_DecodeResult rRef, rTr;
	_OffsetType nRef, nTr;
	const _PrefixTrace* p;
	_CodeInfo ci;

	/* Truncated VEX: failure byte gets an empty trace slot. */
	{
		const unsigned char b[] = {0xc4};
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 1);
		TASK_CHECK(got[0].flags == FLAG_NOT_DECODABLE);
		TASK_CHECK(tr[0].count == 0);
	}

	/* Invalid VEX3 candidate then a valid NOP: both slots stay empty. */
	{
		const unsigned char b[] = {0xc4, 0x90};
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == 2);
		TASK_CHECK(got[0].flags == FLAG_NOT_DECODABLE);
		TASK_CHECK(tr[0].count == 0);
		TASK_CHECK(tr[1].count == 0);
	}

	/*
	 * A failed prefixed instruction followed by a valid prefixed one: the
	 * surviving instruction's records are relative to itself and not inherited.
	 */
	{
		const unsigned char b[] = {0xc4, 0x66, 0x90};
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, CAP, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uTr == uRef && uTr == 2);
		/* First byte is an undecodable VEX with an empty trace slot. */
		TASK_CHECK(got[0].flags == FLAG_NOT_DECODABLE);
		TASK_CHECK(tr[0].count == 0);
		/* The surviving NOP owns a fresh, self-relative 0x66 record. */
		TASK_CHECK(got[1].size == 2);
		TASK_CHECK(tr[1].count == 1);
		p = find_record(&tr[1], 0);
		check_basic_record(p, 0, 1, 0x66, PT_GROUP_OPERAND_SIZE);
		TASK_CHECK(p->reason == PT_REASON_INSTRUCTION_UNUSED);
	}

	/* Capacity exhaustion: same advancement and MEMORYERR as the old entry. */
	{
		const unsigned char b[] = {0x90, 0x90, 0x90};
		compare_entries(b, (int)sizeof(b), Decode64Bits, 0, 1, ref, got, tr,
		                &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(rRef == DECRES_MEMORYERR && rTr == DECRES_MEMORYERR);
		TASK_CHECK(uRef == 1 && uTr == 1);
		TASK_CHECK(nRef == nTr);
	}

	/* Flow-control-only filtering keeps result and trace arrays aligned. */
	{
		const unsigned char b[] = {0x90, 0xeb, 0x00};
		compare_entries(b, (int)sizeof(b), Decode64Bits, DF_RETURN_FC_ONLY, CAP,
		                ref, got, tr, &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(uRef == 1 && uTr == 1);
		TASK_CHECK(META_GET_FC(got[0].meta) != FC_NONE);
		TASK_CHECK(tr[0].count == 0);
	}

	/* Stop-on-undecodable keeps the old behavior, with an empty trace slot. */
	{
		const unsigned char b[] = {0xc4, 0x90};
		compare_entries(b, (int)sizeof(b), Decode64Bits, DF_STOP_ON_UNDECODEABLE,
		                CAP, ref, got, tr, &uRef, &uTr, &rRef, &rTr, &nRef, &nTr);
		TASK_CHECK(rRef == DECRES_SUCCESS && rTr == DECRES_SUCCESS);
		TASK_CHECK(uRef == 1 && uTr == 1);
		TASK_CHECK(got[0].flags == FLAG_NOT_DECODABLE);
		TASK_CHECK(tr[0].count == 0);
	}

	/* Input validation mirrors the plain entry and additionally rejects NULL trace. */
	{
		_DInst out[2];
		_PrefixTraceInfo t[2];
		unsigned used = 0xdead;
		const unsigned char b[] = {0x90};
		TASK_CHECK(distorm_decompose_prefix_trace(NULL, out, t, 2, &used) == DECRES_INPUTERR);

		memset(&ci, 0, sizeof(ci));
		ci.code = b; ci.codeLen = 1; ci.dt = Decode64Bits;
		TASK_CHECK(distorm_decompose_prefix_trace(&ci, NULL, t, 2, &used) == DECRES_INPUTERR);
		TASK_CHECK(distorm_decompose_prefix_trace(&ci, out, NULL, 2, &used) == DECRES_INPUTERR);
		TASK_CHECK(distorm_decompose_prefix_trace(&ci, out, t, 0, &used) == DECRES_INPUTERR);
	}
}

const struct task_case task_cases[] = {
	{"roles", run_roles},
	{"suppression", run_suppression},
	{"modes", run_modes},
	{"recovery", run_recovery}
};

const unsigned task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
