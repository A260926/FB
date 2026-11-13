#ifndef CAPSTONE_CFG_H
#define CAPSTONE_CFG_H

/* Read-only control-flow recovery for offline x86-64 firmware analysis.
 *
 * The recovery engine only consumes the byte buffers handed to it through
 * cs_cfg_region structures. It never executes machine code, never dereferences
 * host pointers derived from target addresses and never reads the address
 * space of the host process. All returned memory is independently owned by
 * the report and is released with cs_cfg_report_free().
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "capstone.h"

/// Maximum number of concrete values tracked per general purpose register.
/// Abstract values whose union grows beyond this size are promoted to
/// CS_CFG_VALUE_UNKNOWN.
#define CS_CFG_MAX_VALUES 8

/// Number of tracked general purpose registers (RAX..R15, RSP, RBP included).
#define CS_CFG_GP_REG_COUNT 16

/// A virtual address region mapped from an offline image.
typedef struct cs_cfg_region {
	uint64_t address;	///< first virtual address of the region
	const uint8_t *data;	///< bytes backing the region (must stay valid for
				///< the duration of the call only; the report
				///< does not borrow this memory)
	uint64_t size;		///< region size in bytes
	bool executable;	///< region may contain code
	bool read_only;		///< region may be read as constant data
} cs_cfg_region;

/// Kinds of values abstractly tracked for a register.
typedef enum cs_cfg_value_kind {
	CS_CFG_VALUE_UNKNOWN = 0, ///< unknown / top: more than CS_CFG_MAX_VALUES
	CS_CFG_VALUE_SET,	  ///< finite set of concrete uint64 values
} cs_cfg_value_kind;

/// Abstract value of a general purpose register.
typedef struct cs_cfg_register_value {
	x86_reg reg;			///< identity of the 64-bit register
	cs_cfg_value_kind kind;	///< CS_CFG_VALUE_UNKNOWN or CS_CFG_VALUE_SET
	uint8_t count;			///< number of entries in values[] (0..8)
	uint64_t values[CS_CFG_MAX_VALUES]; ///< concrete values, sorted, unique
} cs_cfg_register_value;

/// Kinds of basic block terminators.
typedef enum cs_cfg_terminator {
	CS_CFG_TERM_FALLTHROUGH = 0, ///< block runs into the next instruction
	CS_CFG_TERM_JMP,	     ///< unconditional direct/indirect jump
	CS_CFG_TERM_JCC,	     ///< conditional jump, both sides kept
	CS_CFG_TERM_CALL,	     ///< call; only the return successor is traced
	CS_CFG_TERM_RET,	     ///< return: a normal end of a path
	CS_CFG_TERM_TRAP,	     ///< instruction could not be decoded
} cs_cfg_terminator;

/// A recovered basic block.
typedef struct cs_cfg_block {
	uint64_t address;	///< address of the first instruction
	uint64_t end;		///< address immediately after the last instruction
	cs_cfg_terminator terminator; ///< how the block ends
	uint32_t instruction_count;  ///< number of decoded instructions
	uint32_t overlap_count;	    ///< number of overlapping decode starts
				     ///< inside this block's byte range
} cs_cfg_block;

/// Kinds of control flow edges.
typedef enum cs_cfg_edge_kind {
	CS_CFG_EDGE_FALLTHROUGH = 0, ///< straight-line successor
	CS_CFG_EDGE_TRUE_BRANCH,	    ///< taken side of a conditional jump
	CS_CFG_EDGE_FALSE_BRANCH,	    ///< not-taken side of a conditional jump
	CS_CFG_EDGE_JUMP,		    ///< unconditional jump successor
	CS_CFG_EDGE_CALL,		    ///< recorded target of a direct call
	CS_CFG_EDGE_CALL_RETURN,	    ///< synthetic return successor of a call
	CS_CFG_EDGE_INDIRECT,		    ///< one resolved candidate of an
					    ///< indirect jump
} cs_cfg_edge_kind;

/// A directed edge between two basic blocks.
typedef struct cs_cfg_edge {
	uint64_t source;	 ///< source block start address
	uint64_t target;	 ///< target block start address
	cs_cfg_edge_kind kind; ///< edge classification
} cs_cfg_edge;

/// A resolved candidate target for an indirect jump.
typedef struct cs_cfg_indirect_target {
	uint64_t site;   ///< address of the indirect jump instruction
	uint64_t target; ///< one concrete candidate address
} cs_cfg_indirect_target;

/// A byte range decoded from more than one instruction start. Both decode
/// chains are retained; nothing is discarded.
typedef struct cs_cfg_overlap {
	uint64_t start;	 ///< first overlapping byte
	uint64_t end;	 ///< one past the last overlapping byte
	uint64_t first;	 ///< start address of the first chain's instruction
	uint64_t second; ///< start address of the later, conflicting chain
} cs_cfg_overlap;

/// Kinds of diagnostics.
typedef enum cs_cfg_diagnostic_kind {
	CS_CFG_DIAG_OUTSIDE_REGION = 0, ///< target address is not mapped
	CS_CFG_DIAG_TRUNCATED,		///< instruction runs past region end
	CS_CFG_DIAG_ILLEGAL,		///< bytes do not decode to an instruction
	CS_CFG_DIAG_UNRESOLVED_INDIRECT, ///< indirect jump with unknown target
} cs_cfg_diagnostic_kind;

/// A recovery diagnostic carrying the offending address.
typedef struct cs_cfg_diagnostic {
	uint64_t address;		  ///< address the diagnostic refers to
	cs_cfg_diagnostic_kind kind; ///< diagnostic classification
} cs_cfg_diagnostic;

/// The independently owned result of a recovery run.
typedef struct cs_cfg_report {
	uint64_t *entry_points;	    ///< deduplicated, sorted entry points
	size_t entry_count;

	cs_cfg_block *blocks;	    ///< blocks sorted by start address
	size_t block_count;

	cs_cfg_edge *edges;	    ///< edges sorted by (source, kind, target)
	size_t edge_count;

	cs_cfg_indirect_target *indirect_targets; ///< sorted by (site, target)
	size_t indirect_target_count;

	cs_cfg_overlap *overlaps;   ///< sorted by (start, first, second)
	size_t overlap_count;

	cs_cfg_diagnostic *diagnostics; ///< sorted by (address, kind)
	size_t diagnostic_count;

	cs_cfg_register_value *registers; ///< final abstract state at each block
					 ///< end; block_count *
					 ///< CS_CFG_GP_REG_COUNT entries, in
					 ///< the same order as blocks[]
	size_t register_count;		///< == block_count * CS_CFG_GP_REG_COUNT

	bool incomplete;		    ///< a diagnostic was produced
} cs_cfg_report;

/**
 Recover a read-only control flow graph for x86-64 firmware fragments.

 @param regions      virtual address regions; ranges must not wrap and must
                     not overlap.
 @param region_count number of entries in @regions.
 @param entries      entry point addresses; each must lie inside an executable
                     region. An empty array yields an empty graph.
 @param entry_count  number of entries in @entries.
 @param report       on success receives a heap allocated report owned by the
                     caller; release it with cs_cfg_report_free(). On failure
                     it is set to NULL.

 @return CS_ERR_OK on success, CS_ERR_MEM on allocation failure or another
         CS_ERR_* value for invalid arguments.
*/
CAPSTONE_EXPORT
cs_err CAPSTONE_API cs_cfg_recover_x86_64(const cs_cfg_region *regions,
		size_t region_count,
		const uint64_t *entries, size_t entry_count,
		cs_cfg_report **report);

/// Release a report returned by cs_cfg_recover_x86_64(). NULL is accepted.
CAPSTONE_EXPORT
void CAPSTONE_API cs_cfg_report_free(cs_cfg_report *report);

#ifdef __cplusplus
}
#endif

#endif
