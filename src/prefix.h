/*
prefix.h

diStorm3 - Powerful disassembler for X86/AMD64
http://ragestorm.net/distorm/
distorm at gmail dot com
Copyright (C) 2003-2021 Gil Dabah
This library is licensed under the BSD license. See the file COPYING.
*/


#ifndef PREFIX_H
#define PREFIX_H

#include "config.h"
#include "decoder.h"
#include "../include/prefix_trace.h"


/* Specifies the type of the extension prefix, such as: REX, 2 bytes VEX, 3 bytes VEX. */
typedef enum {PET_NONE = 0, PET_REX, PET_VEX2BYTES, PET_VEX3BYTES} _PrefixExtType;

/* Specifies an index into a table of prefixes by their type. */
typedef enum {PFXIDX_NONE = -1, PFXIDX_REX, PFXIDX_LOREP, PFXIDX_SEG, PFXIDX_OP_SIZE, PFXIDX_ADRS, PFXIDX_MAX} _PrefixIndexer;

/*
* This holds the prefixes state for the current instruction we decode.
* decodedPrefixes includes all specific prefixes that the instruction got.
* start is a pointer to the first prefix to take into account.
* last is a pointer to the last byte we scanned.
* Other pointers are used to keep track of prefixes positions and help us know if they appeared already and where.
*/
typedef struct {
	_iflags decodedPrefixes, usedPrefixes;
	/* Number of prefixes scanned for current instruction, including VEX! */
	unsigned int count;
	uint16_t unusedPrefixesMask;
	/* Holds the offset to the prefix byte by its type. */
	uint16_t pfxIndexer[PFXIDX_MAX];
	_PrefixExtType prefixExtType;
	/* Indicates whether the operand size prefix (0x66) was used as a mandatory prefix. */
	int isOpSizeMandatory;
	/* If VEX prefix is used, store the VEX.vvvv field. */
	unsigned int vexV;
	/* If VEX prefix is used, store the VEX.pp field (the implied mandatory prefix). */
	unsigned int vexPP;
	/* The fields B/X/R/W/L of REX and VEX are stored together in this byte. */
	unsigned int vrex;
	const uint8_t* vexPos;
	/*
	 * Sideband prefix trace of the current instruction, NULL when tracing is off.
	 * Filled by the prefix_trace_* hooks below while scanning/decoding.
	 */
	_PrefixTraceInfo* trace;
} _PrefixState;

/*
* Intel supports 6 types of prefixes, whereas AMD supports 5 types (lock is seperated from rep/nz).
* REX is the fifth prefix type, this time I'm based on AMD64.
* VEX is the 6th, though it can't be repeated.
*/
#define MAX_PREFIXES (5)

extern int PrefixTables[256 * 2];

_INLINE_ int prefixes_is_valid(unsigned char ch, _DecodeType dt)
{
	/* The predicate selects (branchlessly) second half table for 64 bits otherwise selects first half. */
	return PrefixTables[ch + ((dt >> 1) << 8)];
}

/* Ignore a specific prefix type. */
_INLINE_ void prefixes_ignore(_PrefixState* ps, _PrefixIndexer pi)
{
	/*
	 * If that type of prefix appeared already, set the bit of that *former* prefix.
	 * Anyway, set the new index of that prefix type to the current index, so next time we know its position.
	 */
	ps->unusedPrefixesMask |= ps->pfxIndexer[pi];
}

void prefixes_ignore_all(_PrefixState* ps);
uint16_t prefixes_set_unused_mask(_PrefixState* ps);
void prefixes_decode(_CodeInfo* ci, _PrefixState* ps);
void prefixes_use_segment(_iflags defaultSeg, _PrefixState* ps, _DecodeType dt, _DInst* di);

/*
 * Prefix trace hooks, called from the decoder at the exact spots where it
 * decides the fate of a prefix byte. All of them are no-ops when tracing is
 * off (ps->trace == NULL). Implemented in prefix_trace.c.
 */
/* Append a record for a prefix that was just scanned. */
void prefix_trace_append(_PrefixState* ps, unsigned int offset, unsigned int length, uint8_t byte, unsigned int group);
/* The prefix at victimOffset got overridden by a later byte of the same group at byOffset. */
void prefix_trace_superseded(_PrefixState* ps, unsigned int victimOffset, unsigned int byOffset);
/* The last REX prefix doesn't immediately precede the opcode/VEX byte. */
void prefix_trace_rex_position(_PrefixState* ps);
/* The last prefix of type pi is void in the current decoding mode. */
void prefix_trace_mode_ignored(_PrefixState* ps, _PrefixIndexer pi);
/* A REX.W takes precedence over the last 0x66 operand-size prefix. */
void prefix_trace_rexw_opsize(_PrefixState* ps);
/* The last prefix byte with the given value was consumed as a mandatory opcode selector. */
void prefix_trace_mandatory(_PrefixState* ps, uint8_t byte);
/* The last segment override prefix took effect (64 bits FS/GS override). */
void prefix_trace_segment_used(_PrefixState* ps);
/*
 * Assign the final roles/reasons of all records of the current instruction,
 * from the decoder's own used-prefixes bookkeeping. Called once per
 * successfully decoded instruction.
 */
void prefix_trace_finalize(_PrefixState* ps);

#endif /* PREFIX_H */
