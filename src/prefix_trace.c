/*
prefix_trace.c

diStorm3 - Powerful disassembler for X86/AMD64
http://ragestorm.net/distorm/
distorm at gmail dot com
Copyright (C) 2003-2021 Gil Dabah
This library is licensed under the BSD license. See the file COPYING.
*/


/*
 * This module builds the prefix side-band records from the live decisions of
 * the production decoder: the scanner in prefix.c creates one record per
 * scanned prefix, the opcode selection (instructions.c) marks mandatory
 * selectors and the VEX pp field, and the operand processing marks the roles
 * that actually took effect. Nothing here re-decodes the stream or derives a
 * reason from the final mnemonic / unusedPrefixesMask.
 *
 * When tracing is disabled (the plain decompose entry) every entry point is
 * an early no-op, so the original decoder behavior and ABI are untouched.
 */

#include "prefix.h"
#include "instructions.h"


/* Map the internal prefix indexer onto the public trace group. */
static _PrefixTraceGroup prefix_trace_group_of(_PrefixIndexer pi)
{
	switch (pi)
	{
		case PFXIDX_OP_SIZE: return PT_GROUP_OPERAND_SIZE;
		case PFXIDX_ADRS: return PT_GROUP_ADDRESS_SIZE;
		case PFXIDX_SEG: return PT_GROUP_SEGMENT;
		case PFXIDX_LOREP: return PT_GROUP_LOCK_REPEAT;
		case PFXIDX_REX: return PT_GROUP_REX;
		default: return PT_GROUP_NONE;
	}
}

/* Return the most recent record recorded for a group, or NULL. */
static _PrefixTrace* prefix_trace_last(_PrefixState* ps, _PrefixTraceGroup group)
{
	int i;
	for (i = (int)ps->trace.count - 1; i >= 0; i--) {
		if (ps->trace.prefixes[i].group == group) return &ps->trace.prefixes[i];
	}
	return NULL;
}

/*
 * The scanner saw another legacy/REX prefix byte.
 * A former record of the same group was just superseded, so annotate it with
 * the override reason and the offset of the byte that won.
 */
void prefix_trace_scan_legacy(_PrefixState* ps, unsigned int index, unsigned char byte, _PrefixIndexer pi)
{
	_PrefixTrace* rec;
	_PrefixTraceGroup group;

	if (!ps->traceEnabled) return;

	group = prefix_trace_group_of(pi);
	rec = prefix_trace_last(ps, group);
	if (rec != NULL) {
		rec->reason = PT_REASON_GROUP_OVERRIDE;
		rec->overriddenByOffset = (uint8_t)index;
	}

	if (ps->trace.count >= sizeof(ps->trace.prefixes) / sizeof(ps->trace.prefixes[0])) return;

	rec = &ps->trace.prefixes[ps->trace.count++];
	memset(rec, 0, sizeof(_PrefixTrace));
	rec->offset = (uint8_t)index;
	rec->length = 1;
	rec->byteCount = 1;
	rec->bytes[0] = byte;
	rec->group = (uint8_t)group;
	rec->reason = PT_REASON_NONE;
	rec->overriddenByOffset = 0xff;
	rec->vexImpliedPp = 0xff;
}

/* The scanner accepted a whole 2 or 3 byte VEX prefix as one record. */
void prefix_trace_scan_vex(_PrefixState* ps, unsigned int index, const unsigned char* vexStart, unsigned int len)
{
	_PrefixTrace* rec;
	unsigned int i;

	if (!ps->traceEnabled) return;
	if (ps->trace.count >= sizeof(ps->trace.prefixes) / sizeof(ps->trace.prefixes[0])) return;

	rec = &ps->trace.prefixes[ps->trace.count++];
	memset(rec, 0, sizeof(_PrefixTrace));
	rec->offset = (uint8_t)index;
	rec->length = (uint8_t)len;
	rec->byteCount = (uint8_t)len;
	for (i = 0; i < len; i++) rec->bytes[i] = vexStart[i];
	rec->group = PT_GROUP_VEX;
	rec->reason = PT_REASON_NONE;
	rec->overriddenByOffset = 0xff;
	rec->vexImpliedPp = 0xff; /* Filled in by inst_vex_lookup once pp is known. */
}

/* Opcode selection succeeded for the VEX prefix; record the implied pp. */
void prefix_trace_vex_selected(_PrefixState* ps, unsigned int pp)
{
	_PrefixTrace* rec;

	if (!ps->traceEnabled) return;
	rec = prefix_trace_last(ps, PT_GROUP_VEX);
	if (rec == NULL) return;

	rec->vexImpliedPp = (uint8_t)pp;
	rec->roles |= PT_ROLE_VEX;
	/* pp is the virtual mandatory selector implied by the VEX encoding. */
	if (pp != 0) rec->roles |= PT_ROLE_MANDATORY_SELECTOR;
}

/* The REX byte is not immediately before the opcode, so the mode ignores it. */
void prefix_trace_rex_invalid_position(_PrefixState* ps)
{
	_PrefixTrace* rec;

	if (!ps->traceEnabled) return;
	rec = prefix_trace_last(ps, PT_GROUP_REX);
	if ((rec != NULL) && (rec->reason == PT_REASON_NONE)) rec->reason = PT_REASON_REX_POSITION;
}

/* In 64 bits the CS/DS/ES/SS segment override is ignored by the decoding mode. */
void prefix_trace_segment_mode_ignored(_PrefixState* ps)
{
	_PrefixTrace* rec;

	if (!ps->traceEnabled) return;
	rec = prefix_trace_last(ps, PT_GROUP_SEGMENT);
	if ((rec != NULL) && (rec->reason == PT_REASON_NONE)) rec->reason = PT_REASON_MODE_IGNORED;
}

/* A valid, adjacent REX.W forced 64 bit operand size over a normal 0x66. */
void prefix_trace_opsize_rexw(_PrefixState* ps)
{
	_PrefixTrace* opSize;
	_PrefixTrace* rex;

	if (!ps->traceEnabled) return;
	opSize = prefix_trace_last(ps, PT_GROUP_OPERAND_SIZE);
	rex = prefix_trace_last(ps, PT_GROUP_REX);
	if (opSize == NULL) return;

	opSize->reason = PT_REASON_REX_W_OVER_66;
	opSize->overriddenByOffset = (rex != NULL) ? rex->offset : 0xff;
}

/* WAIT absorbs its prefixes: legal bytes, but the decoded instruction ignores them. */
void prefix_trace_ignore_all(_PrefixState* ps)
{
	unsigned int i;

	if (!ps->traceEnabled) return;
	for (i = 0; i < ps->trace.count; i++) {
		if (ps->trace.prefixes[i].reason == PT_REASON_NONE) {
			ps->trace.prefixes[i].reason = PT_REASON_INSTRUCTION_UNUSED;
		}
	}
}

/* LEA never consumes a segment override. */
void prefix_trace_lea_ignore_segment(_PrefixState* ps)
{
	_PrefixTrace* rec;

	if (!ps->traceEnabled) return;
	rec = prefix_trace_last(ps, PT_GROUP_SEGMENT);
	if ((rec != NULL) && (rec->reason == PT_REASON_NONE)) rec->reason = PT_REASON_INSTRUCTION_UNUSED;
}

/* A 0x66/0xf3/0xf2 byte was consumed as a mandatory opcode selector. */
void prefix_trace_mandatory(_PrefixState* ps, _iflags selector)
{
	_PrefixTrace* rec;

	if (!ps->traceEnabled) return;

	if (selector == INST_PRE_OP_SIZE) {
		rec = prefix_trace_last(ps, PT_GROUP_OPERAND_SIZE);
	}
	else {
		/* LOCK/REP group: pick the record carrying the actual selected byte. */
		unsigned char wanted = (selector == INST_PRE_REPNZ) ? PREFIX_REPNZ : PREFIX_REP;
		int i;
		rec = NULL;
		for (i = (int)ps->trace.count - 1; i >= 0; i--) {
			if ((ps->trace.prefixes[i].group == PT_GROUP_LOCK_REPEAT) &&
				(ps->trace.prefixes[i].bytes[0] == wanted)) {
				rec = &ps->trace.prefixes[i];
				break;
			}
		}
	}

	/*
	 * Opcode selection is the later, more specific decision than the scan-time
	 * group bookkeeping. A byte that a later LOCK/REP appeared to supersede can
	 * still be the mandatory selector that actually chose the instruction
	 * (e.g. F2 F0 0F 10 selects MOVSD through F2). In that case the byte was
	 * not really dropped, so clear the stale override annotation.
	 */
	if (rec != NULL) {
		if (rec->reason == PT_REASON_GROUP_OVERRIDE) {
			rec->reason = PT_REASON_NONE;
			rec->overriddenByOffset = 0xff;
		}
		rec->roles |= PT_ROLE_MANDATORY_SELECTOR;
	}
}

/* A segment override really selected the segment of a memory operand. */
void prefix_trace_segment_used(_PrefixState* ps)
{
	_PrefixTrace* rec;

	if (!ps->traceEnabled) return;
	rec = prefix_trace_last(ps, PT_GROUP_SEGMENT);
	if ((rec != NULL) && (rec->reason == PT_REASON_NONE)) rec->roles |= PT_ROLE_SEGMENT;
}

/*
 * Once the instruction is fully decoded, translate the production
 * usedPrefixes bookkeeping (accumulated during operand processing) into
 * roles on the surviving record of each group. Records that already carry a
 * concrete drop reason are never resurrected, and records without any role or
 * reason are prefixes the instruction simply did not use.
 */
void prefix_trace_finalize(_PrefixState* ps)
{
	_PrefixTrace* rec;

	if (!ps->traceEnabled) return;

	if ((ps->usedPrefixes & INST_PRE_OP_SIZE) &&
		((rec = prefix_trace_last(ps, PT_GROUP_OPERAND_SIZE)) != NULL) &&
		(rec->reason == PT_REASON_NONE)) {
		rec->roles |= PT_ROLE_OPERAND_SIZE;
	}

	if ((ps->usedPrefixes & INST_PRE_ADDR_SIZE) &&
		((rec = prefix_trace_last(ps, PT_GROUP_ADDRESS_SIZE)) != NULL) &&
		(rec->reason == PT_REASON_NONE)) {
		rec->roles |= PT_ROLE_ADDRESS_SIZE;
	}

	if ((ps->usedPrefixes & INST_PRE_SEGOVRD_MASK) &&
		((rec = prefix_trace_last(ps, PT_GROUP_SEGMENT)) != NULL) &&
		(rec->reason == PT_REASON_NONE)) {
		rec->roles |= PT_ROLE_SEGMENT;
	}

	if ((ps->usedPrefixes & INST_PRE_LOKREP_MASK) &&
		((rec = prefix_trace_last(ps, PT_GROUP_LOCK_REPEAT)) != NULL) &&
		(rec->reason == PT_REASON_NONE)) {
		rec->roles |= PT_ROLE_LOCK_REPEAT;
	}

	if ((ps->usedPrefixes & INST_PRE_REX) &&
		((rec = prefix_trace_last(ps, PT_GROUP_REX)) != NULL) &&
		(rec->reason == PT_REASON_NONE)) {
		rec->roles |= PT_ROLE_REX;
	}

	{
		unsigned int i;
		for (i = 0; i < ps->trace.count; i++) {
			rec = &ps->trace.prefixes[i];
			if ((rec->reason == PT_REASON_NONE) && (rec->roles == PT_ROLE_NONE)) {
				rec->reason = PT_REASON_INSTRUCTION_UNUSED;
			}
		}
	}
}
