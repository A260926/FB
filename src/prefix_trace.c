/*
prefix_trace.c

diStorm3 - Powerful disassembler for X86/AMD64
http://ragestorm.net/distorm/
distorm at gmail dot com
Copyright (C) 2003-2021 Gil Dabah
This library is licensed under the BSD license. See the file COPYING.
*/


#include "../include/prefix_trace.h"

#include "config.h"
#include "decoder.h"
#include "prefix.h"
#include "instructions.h"
#include "x86defs.h"


/*
 * This module implements the sideband prefix trace of
 * distorm_decompose_prefix_trace (see include/prefix_trace.h for the docs).
 *
 * The hooks below are called from the decoder's own decision points:
 * prefixes_decode (scan), inst_lookup_prefixed (opcode selection),
 * decode_inst (operand-size resolution) and prefixes_use_segment/operands
 * (operand handling). Nothing here re-decodes the input or guesses from the
 * resulting mnemonic; the records merely reflect what the decoder did.
 */

/* Count of trailing zeros of a 16 bits prefix-position mask. */
static unsigned int prefix_trace_ctz(uint16_t mask)
{
	unsigned int n = 0;
	while ((mask & 1) == 0) {
		mask >>= 1;
		n++;
	}
	return n;
}

/* Find a record by the offset of its byte inside the instruction. */
static _PrefixTrace* prefix_trace_by_offset(_PrefixTraceInfo* t, unsigned int offset)
{
	unsigned int i;
	for (i = 0; i < t->count; i++) {
		if (t->items[i].offset == offset) return &t->items[i];
	}
	return NULL;
}

/* Find the last record of a prefix byte with the given value. */
static _PrefixTrace* prefix_trace_last_byte(_PrefixTraceInfo* t, uint8_t byte)
{
	unsigned int i = t->count;
	while (i > 0) {
		i--;
		if (t->items[i].byte == byte) return &t->items[i];
	}
	return NULL;
}

/* Find the last record of the given group. */
static _PrefixTrace* prefix_trace_last_group(_PrefixTraceInfo* t, unsigned int group)
{
	unsigned int i = t->count;
	while (i > 0) {
		i--;
		if (t->items[i].group == group) return &t->items[i];
	}
	return NULL;
}

/*
 * Give a record one more effective role.
 * A scan-time SUPERSEDED marking is tentative for the lock/repeat group
 * (the decoder keeps the lock/rep bits of every byte around, so an earlier
 * byte may still be the one that takes effect); a real role cancels it.
 * Records that already carry a final reason (a dropped REX, a mode-ignored
 * override, a 0x66 suppressed by REX.W) stay unused - later bookkeeping may
 * still mention their type, but the byte itself was already thrown away.
 */
static void prefix_trace_add_role(_PrefixTrace* r, unsigned int role)
{
	if ((r->reason != PFX_REASON_NONE) && (r->reason != PFX_REASON_SUPERSEDED)) return;
	r->roles |= (uint16_t)role;
	if (r->reason == PFX_REASON_SUPERSEDED) {
		r->reason = PFX_REASON_NONE;
		r->supersededBy = PFX_TRACE_NO_OFFSET;
	}
}

/* Give a role to the last prefix of the given type (by the decoder's own bookkeeping). */
static void prefix_trace_role_by_index(_PrefixState* ps, _PrefixIndexer pi, unsigned int role)
{
	_PrefixTrace* r;
	if (ps->pfxIndexer[pi] == 0) return;
	r = prefix_trace_by_offset(ps->trace, prefix_trace_ctz(ps->pfxIndexer[pi]));
	if (r != NULL) prefix_trace_add_role(r, role);
}

void prefix_trace_append(_PrefixState* ps, unsigned int offset, unsigned int length, uint8_t byte, unsigned int group)
{
	_PrefixTrace* r;
	_PrefixTraceInfo* t = ps->trace;
	if (t == NULL) return;
	if (t->count >= PFX_TRACE_MAX_RECORDS) return;
	r = &t->items[t->count++];
	r->offset = (uint8_t)offset;
	r->length = (uint8_t)length;
	r->byte = byte;
	r->group = (uint8_t)group;
	r->reason = PFX_REASON_NONE;
	r->supersededBy = PFX_TRACE_NO_OFFSET;
	r->selector = 0;
	r->reserved = 0;
	r->roles = PFX_ROLE_NONE;
}

void prefix_trace_superseded(_PrefixState* ps, unsigned int victimOffset, unsigned int byOffset)
{
	_PrefixTrace* r;
	if (ps->trace == NULL) return;
	r = prefix_trace_by_offset(ps->trace, victimOffset);
	/* The victim might already carry a final reason from an earlier decision. */
	if ((r != NULL) && (r->reason == PFX_REASON_NONE)) {
		r->reason = PFX_REASON_SUPERSEDED;
		r->supersededBy = (uint8_t)byOffset;
	}
}

void prefix_trace_rex_position(_PrefixState* ps)
{
	_PrefixTrace* r;
	if (ps->trace == NULL) return;
	if (ps->pfxIndexer[PFXIDX_REX] == 0) return;
	r = prefix_trace_by_offset(ps->trace, prefix_trace_ctz(ps->pfxIndexer[PFXIDX_REX]));
	if ((r != NULL) && (r->reason == PFX_REASON_NONE)) r->reason = PFX_REASON_REX_POSITION;
}

void prefix_trace_mode_ignored(_PrefixState* ps, _PrefixIndexer pi)
{
	_PrefixTrace* r;
	if (ps->trace == NULL) return;
	if (ps->pfxIndexer[pi] == 0) return;
	r = prefix_trace_by_offset(ps->trace, prefix_trace_ctz(ps->pfxIndexer[pi]));
	if ((r != NULL) && (r->reason == PFX_REASON_NONE)) r->reason = PFX_REASON_MODE_IGNORED;
}

void prefix_trace_rexw_opsize(_PrefixState* ps)
{
	_PrefixTrace* r;
	if (ps->trace == NULL) return;
	if (ps->pfxIndexer[PFXIDX_OP_SIZE] == 0) return;
	r = prefix_trace_by_offset(ps->trace, prefix_trace_ctz(ps->pfxIndexer[PFXIDX_OP_SIZE]));
	if ((r != NULL) && (r->reason == PFX_REASON_NONE)) {
		r->reason = PFX_REASON_REX_W_OP_SIZE;
		/* Point at the REX byte that won over this operand-size prefix. */
		if (ps->pfxIndexer[PFXIDX_REX] != 0)
			r->supersededBy = (uint8_t)prefix_trace_ctz(ps->pfxIndexer[PFXIDX_REX]);
	}
}

void prefix_trace_mandatory(_PrefixState* ps, uint8_t byte)
{
	_PrefixTrace* r;
	if (ps->trace == NULL) return;
	r = prefix_trace_last_byte(ps->trace, byte);
	if (r != NULL) prefix_trace_add_role(r, PFX_ROLE_MANDATORY);
}

void prefix_trace_segment_used(_PrefixState* ps)
{
	if (ps->trace == NULL) return;
	prefix_trace_role_by_index(ps, PFXIDX_SEG, PFX_ROLE_SEGMENT);
}

void prefix_trace_finalize(_PrefixState* ps)
{
	_PrefixTraceInfo* t = ps->trace;
	_PrefixTrace* r;
	unsigned int i;
	_iflags used;

	if (t == NULL) return;
	used = ps->usedPrefixes;

	/*
	 * Map the decoder's own used-prefixes bookkeeping onto the records.
	 * The lock/repeat types share one slot in the decoder's state, so they
	 * are matched by their byte value instead; the other types are located
	 * through the positions the scanner book-kept.
	 */
	if (used & INST_PRE_OP_SIZE) prefix_trace_role_by_index(ps, PFXIDX_OP_SIZE, PFX_ROLE_OPERAND_SIZE);
	if (used & INST_PRE_ADDR_SIZE) prefix_trace_role_by_index(ps, PFXIDX_ADRS, PFX_ROLE_ADDRESS_SIZE);
	if (used & INST_PRE_SEGOVRD_MASK) prefix_trace_role_by_index(ps, PFXIDX_SEG, PFX_ROLE_SEGMENT);
	if (used & INST_PRE_LOCK) {
		r = prefix_trace_last_byte(t, PREFIX_LOCK);
		if (r != NULL) prefix_trace_add_role(r, PFX_ROLE_LOCK);
	}
	if (used & INST_PRE_REPNZ) {
		r = prefix_trace_last_byte(t, PREFIX_REPNZ);
		if (r != NULL) prefix_trace_add_role(r, PFX_ROLE_REPEAT);
	}
	if (used & INST_PRE_REP) {
		r = prefix_trace_last_byte(t, PREFIX_REP);
		if (r != NULL) prefix_trace_add_role(r, PFX_ROLE_REPEAT);
	}
	if (used & INST_PRE_REX) prefix_trace_role_by_index(ps, PFXIDX_REX, PFX_ROLE_REX);

	/* A VEX prefix that got this far selected the instruction encoding. */
	if (ps->decodedPrefixes & INST_PRE_VEX) {
		r = prefix_trace_last_group(t, PFX_GROUP_VEX);
		if (r != NULL) {
			/* The pp field implies a mandatory selector, see inst_vex_lookup. */
			static const uint8_t ppToSelector[4] = { 0, PREFIX_OP_SIZE, PREFIX_REP, PREFIX_REPNZ };
			prefix_trace_add_role(r, PFX_ROLE_VEX);
			if (ps->vexPP) {
				prefix_trace_add_role(r, PFX_ROLE_MANDATORY);
				r->selector = ppToSelector[ps->vexPP & 3];
			}
		}
	}

	/* Whatever is left without a role was not consumed by the instruction. */
	for (i = 0; i < t->count; i++) {
		r = &t->items[i];
		if ((r->roles == PFX_ROLE_NONE) && (r->reason == PFX_REASON_NONE))
			r->reason = PFX_REASON_NOT_USED;
	}
}

/* C DLL EXPORTS */
#ifdef SUPPORT_64BIT_OFFSET
	_DLLEXPORT_ _DecodeResult distorm_decompose_prefix_trace64(_CodeInfo* ci, _DInst result[], _PrefixTraceInfo traces[], unsigned int maxInstructions, unsigned int* usedInstructionsCount)
#else
	_DLLEXPORT_ _DecodeResult distorm_decompose_prefix_trace32(_CodeInfo* ci, _DInst result[], _PrefixTraceInfo traces[], unsigned int maxInstructions, unsigned int* usedInstructionsCount)
#endif
{
	if (usedInstructionsCount == NULL) {
		return DECRES_SUCCESS;
	}

	/* Same input validation as the classic decompose interface, plus the sideband array. */
	if ((ci == NULL) ||
		(ci->codeLen < 0) ||
		((unsigned)ci->dt > (unsigned)Decode64Bits) ||
		(ci->code == NULL) ||
		(result == NULL) ||
		(traces == NULL) ||
		(maxInstructions == 0) ||
		((ci->features & (DF_MAXIMUM_ADDR16 | DF_MAXIMUM_ADDR32)) == (DF_MAXIMUM_ADDR16 | DF_MAXIMUM_ADDR32)))
	{
		return DECRES_INPUTERR;
	}

	return decode_internal_ex(ci, FALSE, result, maxInstructions, usedInstructionsCount, traces);
}
