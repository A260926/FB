/* diStorm 3.5.3 */

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

/*
 * Prefix tracing sideband for the decompose interface.
 * -----------------------------------------------------
 *
 * distorm_decompose only tells you (via _DInst.unusedPrefixesMask) WHICH prefix
 * bytes of an instruction had no effect. It cannot tell you WHY: a byte may be
 * shadowed by a later byte of the same group (66 66 ...), a REX may sit too far
 * from the opcode (48 66 0f 10 c0), a REX.W may take precedence over a 0x66
 * (66 48 98), a segment override may be void in 64 bits (2e ...), or the
 * instruction may simply not consume the prefix at all. Likewise, a byte that
 * IS consumed may serve very different purposes: f3 selects MOVSS in
 * f3 0f 10 c0 (mandatory opcode selector) but repeats the string in f3 a4.
 *
 * distorm_decompose_prefix_trace works exactly like distorm_decompose (same
 * _CodeInfo contract, same features, same _DecodeResult, same advance of
 * nextOffset, same per-instruction output) and additionally fills a caller
 * supplied array of _PrefixTraceInfo, one entry per emitted instruction, in
 * the same order. The records are collected from the decisions the decoder
 * itself makes while scanning prefixes, selecting the opcode and extracting
 * operands - not from post-hoc guesses.
 *
 * Result structure
 * ----------------
 * Each _PrefixTraceInfo holds up to PFX_TRACE_MAX_RECORDS records, in the
 * original byte order of the instruction. Every _PrefixTrace describes one
 * prefix byte sequence:
 *
 *   offset       - byte offset of the prefix inside its instruction.
 *   length       - 1 for legacy prefixes and REX, 2 or 3 for a VEX prefix.
 *   byte         - the first prefix byte as read from the input stream.
 *   group        - the encoding family the byte belongs to (_PrefixTraceGroup),
 *                  regardless of how the instruction ended up using it.
 *   roles        - bitmask of the effects the byte actually had (PFX_ROLE_*).
 *                  A byte may carry several roles, e.g. a VEX prefix whose pp
 *                  field also implies a mandatory opcode selector.
 *   reason       - when roles == PFX_ROLE_NONE, tells why the byte had no
 *                  effect (_PrefixTraceReason); PFX_REASON_NONE otherwise.
 *   supersededBy - for PFX_REASON_SUPERSEDED and PFX_REASON_REX_W_OP_SIZE,
 *                  the offset of the byte that overrode this one,
 *                  PFX_TRACE_NO_OFFSET otherwise.
 *   selector     - for a VEX record, the mandatory prefix byte implied by its
 *                  pp field (0x66/0xf3/0xf2), or 0 when pp == 0. This is a
 *                  virtual selector only; no record is invented for a byte
 *                  that does not exist in the input.
 *
 * Entries of instructions that failed to decode (the FLAG_NOT_DECODABLE "DB"
 * output of the classic interface) have count == 0, and entries are always
 * rewritten per instruction, so records never leak into the next instruction.
 * When the output array is too small, the function stops exactly where
 * distorm_decompose would stop (DECRES_MEMORYERR, same nextOffset) and only
 * the first usedInstructionsCount sideband entries are valid.
 *
 * Usage example
 * -------------
 *	_CodeInfo ci;
 *	_DInst insts[16];
 *	_PrefixTraceInfo traces[16];
 *	unsigned int used = 0, i, j;
 *	static const uint8_t code[] = { 0x66, 0x48, 0x98 }; // CDQE with a dead 0x66
 *
 *	memset(&ci, 0, sizeof(ci));
 *	ci.code = code;
 *	ci.codeLen = sizeof(code);
 *	ci.dt = Decode64Bits;
 *
 *	if (distorm_decompose_prefix_trace(&ci, insts, traces, 16, &used) == DECRES_SUCCESS) {
 *		for (i = 0; i < used; i++) {
 *			for (j = 0; j < traces[i].count; j++) {
 *				const _PrefixTrace* t = &traces[i].items[j];
 *				printf("+%u byte %02x group %u roles 0x%x reason %u by %u\n",
 *					t->offset, t->byte, t->group, t->roles, t->reason, t->supersededBy);
 *			}
 *		}
 *	}
 *	// prints: +0 byte 66 group 1 roles 0x0 reason 3 by 1   (0x66 overridden by REX.W)
 *	//         +1 byte 48 group 5 roles 0x40 reason 0 by 255 (REX promoted to 64 bits)
 */

#ifdef __cplusplus
 extern "C" {
#endif

/* The encoding family a prefix byte belongs to, by its byte value. */
typedef enum {
	PFX_GROUP_NONE = 0,
	PFX_GROUP_OPERAND_SIZE = 1,	/* 0x66 */
	PFX_GROUP_ADDRESS_SIZE = 2,	/* 0x67 */
	PFX_GROUP_SEGMENT = 3,		/* 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65 */
	PFX_GROUP_LOCK_REPEAT = 4,	/* 0xf0, 0xf2, 0xf3 */
	PFX_GROUP_REX = 5,		/* 0x40 - 0x4f, 64 bits decoding mode only */
	PFX_GROUP_VEX = 6		/* 0xc4 / 0xc5 multi-byte prefix */
} _PrefixTraceGroup;

/* The effects a prefix byte actually had on the decoded instruction (bitmask). */
#define PFX_ROLE_NONE		0
#define PFX_ROLE_OPERAND_SIZE	(1 << 0)	/* Switched the effective operand size. */
#define PFX_ROLE_ADDRESS_SIZE	(1 << 1)	/* Switched the effective address size. */
#define PFX_ROLE_SEGMENT	(1 << 2)	/* Segment override took effect (incl. branch hints). */
#define PFX_ROLE_LOCK		(1 << 3)	/* LOCK was accepted by the instruction. */
#define PFX_ROLE_REPEAT		(1 << 4)	/* REP/REPNZ was accepted (string ops, PAUSE, ...). */
#define PFX_ROLE_MANDATORY	(1 << 5)	/* Consumed as a mandatory opcode selector (incl. VEX.pp). */
#define PFX_ROLE_REX		(1 << 6)	/* REX affected the decoding (W/R/X/B or 8 bit registers). */
#define PFX_ROLE_VEX		(1 << 7)	/* VEX selected the instruction encoding. */

/* Why a prefix byte had no effect at all (PFX_ROLE_NONE in roles). */
typedef enum {
	PFX_REASON_NONE = 0,	/* The byte was used, see roles. */
	PFX_REASON_SUPERSEDED = 1,	/* A later byte of the same group won, see supersededBy. */
	PFX_REASON_REX_POSITION = 2,	/* REX did not immediately precede the opcode/VEX byte. */
	PFX_REASON_REX_W_OP_SIZE = 3,	/* A REX.W took precedence over this 0x66, see supersededBy. */
	PFX_REASON_MODE_IGNORED = 4,	/* The decoding mode ignores it (E.G CS/SS/DS/ES in 64 bits). */
	PFX_REASON_NOT_USED = 5	/* The instruction simply does not consume it. */
} _PrefixTraceReason;

/* supersededBy value when no overriding byte is involved. */
#define PFX_TRACE_NO_OFFSET ((uint8_t)0xff)

/*
 * An instruction may carry at most 15 prefix bytes (the scan stops at the
 * instruction size limit of 15 bytes), so 15 records always suffice.
 */
#define PFX_TRACE_MAX_RECORDS (15)

typedef struct {
	uint8_t offset;		/* Offset of the prefix inside the instruction, in bytes. */
	uint8_t length;		/* 1 for legacy prefixes and REX, 2 or 3 for VEX. */
	uint8_t byte;		/* The prefix byte as read from the input stream. */
	uint8_t group;		/* _PrefixTraceGroup. */
	uint8_t reason;		/* _PrefixTraceReason, PFX_REASON_NONE when the byte was used. */
	uint8_t supersededBy;	/* Offset of the byte that overrode this one, or PFX_TRACE_NO_OFFSET. */
	uint8_t selector;	/* VEX only: mandatory byte implied by pp (0x66/0xf3/0xf2), else 0. */
	uint8_t reserved;	/* Alignment padding, kept 0. */
	uint16_t roles;		/* PFX_ROLE_* bitmask of the effects this byte had. */
} _PrefixTrace;

/* Sideband entry: all prefix records of one decoded instruction, in byte order. */
typedef struct {
	unsigned int count;
	_PrefixTrace items[PFX_TRACE_MAX_RECORDS];
} _PrefixTraceInfo;

/* Define the following interface function only for outer projects. */
#if !(defined(DISTORM_STATIC) || defined(DISTORM_DYNAMIC))

/*
 * distorm_decompose_prefix_trace
 *
 * Same contract as distorm_decompose, plus a sideband array of prefix traces.
 * Input:
 *         ci - See distorm_decompose; all fields (incl. features) behave the same.
 *         result - Array of _DInst, as with distorm_decompose.
 *         traces - Caller allocated array of _PrefixTraceInfo with room for
 *                  maxInstructions entries; entry i describes result[i].
 *         maxInstructions - Capacity of BOTH result and traces.
 *         usedInstructionsCount - Number of instructions actually decoded.
 * Return: the same _DecodeResult distorm_decompose would return for this input,
 *         and ci->nextOffset advances exactly as it does there.
 */
#ifdef SUPPORT_64BIT_OFFSET

	_DecodeResult distorm_decompose_prefix_trace64(_CodeInfo* ci, _DInst result[], _PrefixTraceInfo traces[], unsigned int maxInstructions, unsigned int* usedInstructionsCount);
	#define distorm_decompose_prefix_trace distorm_decompose_prefix_trace64

#else /*SUPPORT_64BIT_OFFSET*/

	_DecodeResult distorm_decompose_prefix_trace32(_CodeInfo* ci, _DInst result[], _PrefixTraceInfo traces[], unsigned int maxInstructions, unsigned int* usedInstructionsCount);
	#define distorm_decompose_prefix_trace distorm_decompose_prefix_trace32

#endif

#endif /* DISTORM_STATIC */

#ifdef __cplusplus
} /* End Of Extern */
#endif

#endif /* PREFIX_TRACE_H */
