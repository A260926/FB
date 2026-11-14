/*
 * Copyright (c) 2025 Bitdefender
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BDX86_REALMODE_H
#define BDX86_REALMODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//
// Real-mode physical graph builder.
//
// This module statically walks a set of immutable, non-overlapping physical ROM fragments, starting from a set of
// 16-bit CS:IP entry points. Instructions are always decoded with NdDecodeEx in ND_CODE_16 / ND_DATA_16 mode; the
// input is never executed and bdshemu is never used.
//
// The result is a read-only graph which owns copies of every fetched instruction byte and of the physical source
// of every byte, so it remains valid after the caller releases the input fragments.
//

#define RM_MAX_INSTRUCTION_LENGTH   15
#define RM_TEXT_SIZE                128
#define RM_INVALID_INDEX            UINT32_C(0xFFFFFFFF)

//
// One immutable physical fragment. Fragments must not overlap, must have non-zero size and stable, unique IDs.
// PhysicalBase is the physical address of Bytes[0].
//
typedef struct _RM_FRAGMENT
{
    uint32_t        Id;             // Stable caller-provided identifier. Copied in every byte source.
    uint32_t        PhysicalBase;   // Physical address of the first byte.
    const uint8_t   *Bytes;         // Read-only fragment bytes. Copied by the builder.
    size_t          Size;           // Number of bytes.
} RM_FRAGMENT;

//
// One logical entry point. Cs:Ip is a 16-bit real-mode segment:offset pair.
//
typedef struct _RM_ENTRY
{
    uint16_t        Cs;
    uint16_t        Ip;
} RM_ENTRY;

//
// Where a fetched instruction byte came from. PhysicalAddress is the A20-effective physical address, i.e. after
// bit 20 has been masked off when A20 is disabled.
//
typedef struct _RM_BYTE_SOURCE
{
    uint32_t        FragmentId;
    uint32_t        FragmentOffset;
    uint32_t        PhysicalAddress;
} RM_BYTE_SOURCE;

//
// A logical instruction-boundary node. Nodes are identified by (Cs, Ip), not by physical address: two different
// CS:IP pairs which alias the same physical byte are distinct nodes, because their 16-bit successors may differ.
//
typedef struct _RM_NODE
{
    uint16_t        Cs;             // Logical instruction pointer segment.
    uint16_t        Ip;             // Logical instruction pointer offset.
    uint8_t         Decoded;        // 1 if NdDecodeEx accepted the fetched bytes, 0 otherwise.
    uint8_t         Length;         // Decoded instruction length. 0 when Decoded is 0.
    uint8_t         SourceCount;    // Number of fetched bytes copied into Bytes/Sources (0..15).
    uint8_t         Bytes[RM_MAX_INSTRUCTION_LENGTH];
    RM_BYTE_SOURCE  Sources[RM_MAX_INSTRUCTION_LENGTH];
    char            Text[RM_TEXT_SIZE]; // Best-effort textual disassembly, NUL terminated ("" if unavailable).
} RM_NODE;

//
// A logical routine. A routine is rooted at a logical entry: either one of the caller-provided entries, or the
// direct target of a supported near/far CALL. Intra-routine edges are sequential flow, direct jumps and both arms
// of conditional branches. CALL targets are separate routines, and RET/RETF ends the routine (apart from the
// matched return edges which leave it).
//
typedef struct _RM_ROUTINE
{
    uint16_t        Cs;             // Routine logical entry.
    uint16_t        Ip;
    uint32_t        EntryNode;      // Index of the entry node in the node table.
    uint8_t         IsTopLevelEntry;// 1 if this entry was supplied by the caller (no call frame).
} RM_ROUTINE;

//
// Edge kinds. Conditional branches always produce both a taken and a fall-through edge; flags are never
// evaluated. RETURN_* edges are the matched call-result edges: CallSiteNode identifies the exact CALL that was
// matched and ReturnSiteNode identifies the RET/RETF which produced the edge.
//
typedef enum _RM_EDGE_KIND
{
    RM_EDGE_SEQUENTIAL = 1,         // Ordinary sequential successor.
    RM_EDGE_JUMP_NEAR = 2,          // Direct near jump; target wraps in 16-bit IP.
    RM_EDGE_JUMP_FAR = 3,           // Direct immediate far jump; target updates CS:IP.
    RM_EDGE_BRANCH_TAKEN = 4,       // Conditional branch taken target.
    RM_EDGE_BRANCH_FALLTHROUGH = 5, // Conditional branch not-taken successor.
    RM_EDGE_CALL_NEAR = 6,          // Direct near CALL into a target routine.
    RM_EDGE_CALL_FAR = 7,           // Direct immediate far CALL into a target routine.
    RM_EDGE_RETURN_NEAR = 8,        // Matched near return: restores saved IP, uses the return site's CS.
    RM_EDGE_RETURN_FAR = 9,         // Matched far return: restores saved CS and IP.
} RM_EDGE_KIND;

//
// A transfer edge. FromNode/ToNode are node indices. ToCs/ToIp mirror the destination node's logical address.
// CallSiteNode and ReturnSiteNode are RM_INVALID_INDEX for non-return edges.
//
typedef struct _RM_EDGE
{
    RM_EDGE_KIND    Kind;
    uint32_t        FromNode;
    uint32_t        ToNode;
    uint16_t        ToCs;
    uint16_t        ToIp;
    uint32_t        CallSiteNode;   // Return edges: the matched CALL instruction node.
    uint32_t        ReturnSiteNode; // Return edges: the RET/RETF instruction node.
} RM_EDGE;

//
// Why a path stopped. Opaque terminations are recorded at the instruction they occur on; the node bytes and
// sources are still available.
//
typedef enum _RM_TERMINATION_REASON
{
    RM_TERM_HLT = 1,                // HLT: a normal terminal instruction.
    RM_TERM_OPEN_RETURN = 2,        // Top-level RET/RETF: an open return exit (no call frame to match).
    RM_TERM_UNRESOLVED_RETURN = 3,  // RET/RETF whose kind has no matching caller at the annotated call site.
    RM_TERM_INDIRECT = 4,           // Indirect control transfer.
    RM_TERM_CONTROL32 = 5,          // 32-bit control transfer.
    RM_TERM_INTERRUPT = 6,          // INT/INT1/INT3/INTO/IRET/UIRET.
    RM_TERM_SP_WRITE = 7,           // Non-CALL/RET instruction which writes SP/ESP.
    RM_TERM_STACK_ACCESS = 8,       // Non-CALL/RET instruction which accesses the stack.
    RM_TERM_MEMORY_WRITE = 9,       // Non-CALL/RET instruction which writes memory.
    RM_TERM_RET_IMMEDIATE = 10,     // RET/RETF with an immediate stack adjustment (not supported).
    RM_TERM_UNSUPPORTED_TRANSFER = 11, // Control transfer outside the supported subset (SYSENTER, XBEGIN, ...).
    RM_TERM_TRUNCATED = 12,         // Not enough bytes: missing fragment byte or the fetch would pass IP=FFFF.
    RM_TERM_DECODE_ERROR = 13,      // NdDecodeEx rejected the fetched bytes.
} RM_TERMINATION_REASON;

//
// A termination record. Node is the instruction node at which the path stopped. CallSiteNode is set for
// RM_TERM_UNRESOLVED_RETURN (the mismatching call site) and RM_INVALID_INDEX otherwise.
//
typedef struct _RM_TERMINATION_RECORD
{
    RM_TERMINATION_REASON   Reason;
    uint32_t                Node;
    uint16_t                Cs;
    uint16_t                Ip;
    uint32_t                CallSiteNode;
} RM_TERMINATION_RECORD;

//
// Opaque graph handle.
//
typedef struct _RM_GRAPH RM_GRAPH;

//
// Builds a graph from immutable input. The input arrays and fragment bytes are only read during this call; the
// graph copies every byte and source it reports. Returns NULL on invalid input (overlapping/empty fragments,
// duplicate fragment IDs, invalid pointers) or allocation failure.
//
// Fragment and entry order does not influence the result: fragments are sorted internally by physical base and
// entries by CS:IP, and all output tables have a canonical order.
//
RM_GRAPH *
RmBuildGraph(
    const RM_FRAGMENT *Fragments,
    size_t FragmentCount,
    int A20Enabled,
    const RM_ENTRY *Entries,
    size_t EntryCount
);

//
// Releases a graph. Passing NULL is allowed.
//
void
RmFreeGraph(
    RM_GRAPH *Graph
);

//
// Basic graph properties.
//
int
RmA20Enabled(
    const RM_GRAPH *Graph
);

size_t
RmRoutineCount(
    const RM_GRAPH *Graph
);

const RM_ROUTINE *
RmRoutineAt(
    const RM_GRAPH *Graph,
    size_t Index
);

int
RmFindRoutine(
    const RM_GRAPH *Graph,
    uint16_t Cs,
    uint16_t Ip,
    uint32_t *RoutineIndex
);

size_t
RmNodeCount(
    const RM_GRAPH *Graph
);

const RM_NODE *
RmNodeAt(
    const RM_GRAPH *Graph,
    size_t Index
);

int
RmFindNode(
    const RM_GRAPH *Graph,
    uint16_t Cs,
    uint16_t Ip,
    uint32_t *NodeIndex
);

//
// Routines which reach a node through intra-routine flow. A byte/code sequence shared between routines has one
// node with several owners.
//
size_t
RmNodeRoutineCount(
    const RM_GRAPH *Graph,
    uint32_t NodeIndex
);

uint32_t
RmNodeRoutineAt(
    const RM_GRAPH *Graph,
    uint32_t NodeIndex,
    size_t OwnerIndex
);

size_t
RmEdgeCount(
    const RM_GRAPH *Graph
);

const RM_EDGE *
RmEdgeAt(
    const RM_GRAPH *Graph,
    size_t Index
);

//
// Routines whose analysis produced an edge. A shared call/jump edge can have more than one owner.
//
size_t
RmEdgeRoutineCount(
    const RM_GRAPH *Graph,
    size_t EdgeIndex
);

uint32_t
RmEdgeRoutineAt(
    const RM_GRAPH *Graph,
    size_t EdgeIndex,
    size_t OwnerIndex
);

size_t
RmTerminationCount(
    const RM_GRAPH *Graph
);

const RM_TERMINATION_RECORD *
RmTerminationAt(
    const RM_GRAPH *Graph,
    size_t Index
);

#ifdef __cplusplus
}
#endif

#endif // BDX86_REALMODE_H
