/*
prefix_trace.h

diStorm3 - Powerful disassembler for X86/AMD64
http://ragestorm.net/distorm/
distorm at gmail dot com
Copyright (C) 2003-2021 Gil Dabah
This library is licensed under the BSD license. See the file COPYING.
*/


#ifndef PREFIX_TRACE_H
#define PREFIX_TRACE_H

#include "distorm.h"

#ifdef __cplusplus
 extern "C" {
#endif

/*
 * Prefix tracing API.
 *
 * distorm_decompose_prefix_trace() mirrors distorm_decompose(): it takes the
 * same _CodeInfo, the same caller-allocated instruction array and capacity,
 * and returns the same _DecodeResult while advancing _CodeInfo.nextOffset
 * exactly like the plain decompose entry. In addition it fills a parallel
 * array of _PrefixTrace structures, one entry per instruction, so result[i]
 * and trace[i] always describe the same instruction (including instructions
 * filtered out by features, which never occupy a slot).
 *
 * Every prefix byte the production scanner saw is recorded in its original
 * stream order, together with the role it ended up playing in the actual
 * decode decision and, when it had no effect, the reason for that. A VEX
 * prefix is stored as a single 2 or 3 byte record; no bytes that are not
 * physically present in the input are ever invented.
 */

/* Logical group a prefix byte belongs to, per the x86/AMD64 prefix types. */
typedef enum {
	PT_GROUP_NONE = 0,
	/* 0x66 operand-size override (may be consumed as a mandatory selector). */
	PT_GROUP_OPERAND_SIZE,
	/* 0x67 address-size override. */
	PT_GROUP_ADDRESS_SIZE,
	/* 0x26/0x2e/0x36/0x3e/0x64/0x65 segment overrides. */
	PT_GROUP_SEGMENT,
	/* 0xf0 LOCK and 0xf2/0xf3 REP/REPNE (may be consumed as mandatory selectors). */
	PT_GROUP_LOCK_REPEAT,
	/* 0x40-0x4f REX, 64 bit decoding mode only. */
	PT_GROUP_REX,
	/* 0xc4/0xc5 VEX, stored as one 2 or 3 byte record. */
	PT_GROUP_VEX
} _PrefixTraceGroup;

/*
 * The final usage of a prefix record. A single record may carry several
 * roles at once, so these are bit flags ORed together in _PrefixTrace.roles.
 */
#define PT_ROLE_NONE               0
/* 0x66 switched the effective operand size (normal operand-size prefix). */
#define PT_ROLE_OPERAND_SIZE       (1u << 0)
/* 0x67 switched the effective address size. */
#define PT_ROLE_ADDRESS_SIZE       (1u << 1)
/* Segment override actually selected the segment of a memory operand. */
#define PT_ROLE_SEGMENT            (1u << 2)
/* LOCK/REP/REPNE kept its lock/repeat meaning for the instruction. */
#define PT_ROLE_LOCK_REPEAT        (1u << 3)
/*
 * The byte acted as a mandatory opcode selector (SSE/SSE2 mandatory 66/f2/f3,
 * or the virtual pp selector implied by a VEX prefix).
 */
#define PT_ROLE_MANDATORY_SELECTOR (1u << 4)
/* REX extension bits (B/X/R/W, including a W-only promotion to 64 bits). */
#define PT_ROLE_REX                (1u << 5)
/* VEX prefix: extension fields, implied map/mandatory selector and vvvv. */
#define PT_ROLE_VEX                (1u << 6)

/*
 * Why a prefix record ended up with no effect. PT_REASON_NONE means the
 * record was used by the instruction (its roles describe how). Otherwise the
 * reason explains the exact production decision that dropped it.
 */
typedef enum {
	PT_REASON_NONE = 0,
	/* A later byte of the same prefix group overrode this one. */
	PT_REASON_GROUP_OVERRIDE,
	/* REX was not the last byte before the opcode, so the architecture ignores it. */
	PT_REASON_REX_POSITION,
	/* A valid REX.W forced 64 bit operand size and neutralized a normal 0x66. */
	PT_REASON_REX_W_OVER_66,
	/* The decoding mode ignores this prefix (e.g. CS/DS/ES/SS overrides in 64 bits). */
	PT_REASON_MODE_IGNORED,
	/* The prefix was legal here, but the decoded instruction simply does not use it. */
	PT_REASON_INSTRUCTION_UNUSED
} _PrefixTraceReason;

/*
 * One prefix record per scanned prefix, in original instruction order.
 * A whole VEX prefix occupies one record with length 2 or 3.
 */
typedef struct {
	/* Byte offset of the prefix relative to the start of its instruction. */
	uint8_t offset;
	/* Length in bytes: 1 for every legacy/REX prefix, 2 or 3 for VEX. */
	uint8_t length;
	/* Number of valid entries in bytes[]. */
	uint8_t byteCount;
	/* Original prefix bytes; bytes[0..byteCount-1] are valid, rest is zero. */
	uint8_t bytes[3];
	/* Prefix group, see _PrefixTraceGroup. */
	uint8_t group;
	/* Bit mask of PT_ROLE_* values; PT_ROLE_NONE when the prefix was unused. */
	uint16_t roles;
	/* Why the prefix was unused, PT_REASON_NONE when it was used. */
	uint8_t reason;
	/*
	 * When reason is PT_REASON_GROUP_OVERRIDE or PT_REASON_REX_W_OVER_66,
	 * offset of the later prefix byte that superseded this one (relative to
	 * the instruction). 0xff otherwise.
	 */
	uint8_t overriddenByOffset;
	/*
	 * For VEX records only: the pp field (0..3) of the VEX prefix, which is
	 * the implied virtual mandatory selector (0 none, 1 = 0x66, 2 = 0xf3,
	 * 3 = 0xf2). 0xff for every non-VEX record. No virtual bytes are emitted
	 * as separate records.
	 */
	uint8_t vexImpliedPp;
} _PrefixTrace;

/*
 * Per-instruction side band: prefixes[0..count-1] hold the traced prefixes
 * in original stream order. count is 0 for instructions without prefixes
 * (including failure bytes emitted as not-decodable entries).
 */
typedef struct {
	/* Number of valid _PrefixTrace entries in prefixes[]. */
	uint8_t count;
	_PrefixTrace prefixes[15];
} _PrefixTraceInfo;

/*
 * distorm_decompose_prefix_trace
 *
 * Input:
 *         ci - Same _CodeInfo as for distorm_decompose. On return nextOffset
 *              is advanced identically to the plain decompose entry.
 *         result - Caller-allocated array of _DInst, exactly as for
 *              distorm_decompose.
 *         trace - Caller-allocated side-band array with the SAME capacity as
 *              result; trace[i] aligns one-to-one with result[i].
 *         maxInstructions - Capacity shared by result and trace.
 *         usedInstructionsCount - Number of entries written, identical to the
 *              plain decompose entry for the very same input/features.
 *
 * Return: The same _DecodeResult distorm_decompose would return
 *         (DECRES_SUCCESS / DECRES_MEMORYERR / DECRES_INPUTERR).
 *
 * Notes:
 *   - Every feature flag (DF_*) is honored; filtered instructions produce no
 *     result slot and therefore no trace slot either.
 *   - On capacity exhaustion the code stream is advanced exactly as the old
 *     entry does and DECRES_MEMORYERR is returned.
 *   - A not-decodable byte is emitted by the same rule as the old entry; its
 *     trace slot is empty (count == 0), so a later valid instruction can
 *     never inherit a previous instruction's records.
 */
#if !(defined(DISTORM_STATIC) || defined(DISTORM_DYNAMIC))

#ifdef SUPPORT_64BIT_OFFSET
	_DecodeResult distorm_decompose_prefix_trace64(_CodeInfo* ci, _DInst result[], _PrefixTraceInfo trace[], unsigned int maxInstructions, unsigned int* usedInstructionsCount);
	#define distorm_decompose_prefix_trace distorm_decompose_prefix_trace64
#else
	_DecodeResult distorm_decompose_prefix_trace32(_CodeInfo* ci, _DInst result[], _PrefixTraceInfo trace[], unsigned int maxInstructionsCount, unsigned int* usedInstructionsCount);
	#define distorm_decompose_prefix_trace distorm_decompose_prefix_trace32
#endif

#endif /* DISTORM_STATIC */

#ifdef __cplusplus
} /* End Of Extern */
#endif

#endif /* PREFIX_TRACE_H */
