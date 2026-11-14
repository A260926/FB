/*
 * Copyright (c) 2025 Bitdefender
 * SPDX-License-Identifier: Apache-2.0
 */
//
// Real-mode physical graph example.
//
// Reads the immutable hand-authored fixture from task_support/rom_fixture.h, builds the graph, and prints the
// logical nodes (with per-byte provenance), the routines, the matched edges and the termination records. A copy of
// one node is then read after the graph is released, demonstrating that the graph owned everything it reported.
//
#include "bddisasm.h"
#include "bdx86_realmode.h"

#include "../../task_support/rom_fixture.h"

#include <stdio.h>
#include <string.h>

static const char *
ReasonName(RM_TERMINATION_REASON Reason)
{
    switch (Reason)
    {
    case RM_TERM_HLT:                   return "hlt";
    case RM_TERM_OPEN_RETURN:           return "open-return";
    case RM_TERM_UNRESOLVED_RETURN:     return "unresolved-return";
    case RM_TERM_INDIRECT:              return "indirect";
    case RM_TERM_CONTROL32:             return "control32";
    case RM_TERM_INTERRUPT:             return "interrupt";
    case RM_TERM_SP_WRITE:              return "sp-write";
    case RM_TERM_STACK_ACCESS:          return "stack-access";
    case RM_TERM_MEMORY_WRITE:          return "memory-write";
    case RM_TERM_RET_IMMEDIATE:         return "ret-immediate";
    case RM_TERM_UNSUPPORTED_TRANSFER:  return "unsupported-transfer";
    case RM_TERM_TRUNCATED:             return "truncated";
    case RM_TERM_DECODE_ERROR:          return "decode-error";
    default:                            return "unknown";
    }
}

static const char *
KindName(RM_EDGE_KIND Kind)
{
    switch (Kind)
    {
    case RM_EDGE_SEQUENTIAL:            return "seq";
    case RM_EDGE_JUMP_NEAR:             return "jmp";
    case RM_EDGE_JUMP_FAR:              return "jmpf";
    case RM_EDGE_BRANCH_TAKEN:          return "br-taken";
    case RM_EDGE_BRANCH_FALLTHROUGH:    return "br-fall";
    case RM_EDGE_CALL_NEAR:             return "call";
    case RM_EDGE_CALL_FAR:              return "callf";
    case RM_EDGE_RETURN_NEAR:           return "ret";
    case RM_EDGE_RETURN_FAR:            return "retf";
    default:                            return "unknown";
    }
}

static void
PrintIndex(const char *Label, uint32_t Index)
{
    if (RM_INVALID_INDEX == Index)
    {
        printf("%s=-", Label);
    }
    else
    {
        printf("%s=%u", Label, Index);
    }
}

static void
PrintBytes(const RM_NODE *Node)
{
    size_t k;

    printf("bytes=");
    for (k = 0; k < Node->SourceCount; ++k)
    {
        printf("%02x", Node->Bytes[k]);
    }

    printf(" sources=");
    for (k = 0; k < Node->SourceCount; ++k)
    {
        printf("%sf%u+%u@%05x", (0 == k) ? "" : ",", Node->Sources[k].FragmentId,
               Node->Sources[k].FragmentOffset, Node->Sources[k].PhysicalAddress);
    }
}

static void
DumpGraph(const RM_GRAPH *Graph)
{
    size_t i;

    printf("graph a20=%d routines=%zu nodes=%zu edges=%zu terminations=%zu\n",
           RmA20Enabled(Graph), RmRoutineCount(Graph), RmNodeCount(Graph), RmEdgeCount(Graph),
           RmTerminationCount(Graph));

    for (i = 0; i < RmRoutineCount(Graph); ++i)
    {
        const RM_ROUTINE *routine = RmRoutineAt(Graph, i);

        printf("routine[%zu] %04x:%04x entry-node=%u top-level=%u\n",
               i, routine->Cs, routine->Ip, routine->EntryNode, routine->IsTopLevelEntry);
    }

    for (i = 0; i < RmNodeCount(Graph); ++i)
    {
        const RM_NODE *node = RmNodeAt(Graph, i);
        size_t k;

        printf("node[%zu] %04x:%04x decoded=%u length=%u ", i, node->Cs, node->Ip, node->Decoded,
               node->Length);
        PrintBytes(node);

        printf(" owned-by=");
        for (k = 0; k < RmNodeRoutineCount(Graph, (uint32_t)i); ++k)
        {
            uint32_t owner = RmNodeRoutineAt(Graph, (uint32_t)i, k);
            const RM_ROUTINE *routine = RmRoutineAt(Graph, owner);

            if (NULL != routine)
            {
                printf("%s%04x:%04x", (0 == k) ? "" : ",", routine->Cs, routine->Ip);
            }
        }

        printf(" | %s\n", node->Text);
    }

    for (i = 0; i < RmEdgeCount(Graph); ++i)
    {
        const RM_EDGE *edge = RmEdgeAt(Graph, i);
        size_t k;

        printf("edge[%zu] %s %u -> %u (%04x:%04x) ", i, KindName(edge->Kind), edge->FromNode, edge->ToNode,
               edge->ToCs, edge->ToIp);
        PrintIndex("call-site", edge->CallSiteNode);
        printf(" ");
        PrintIndex("return-site", edge->ReturnSiteNode);
        printf(" owned-by=");
        for (k = 0; k < RmEdgeRoutineCount(Graph, i); ++k)
        {
            printf("%s%u", (0 == k) ? "" : ",", RmEdgeRoutineAt(Graph, i, k));
        }
        printf("\n");
    }

    for (i = 0; i < RmTerminationCount(Graph); ++i)
    {
        const RM_TERMINATION_RECORD *term = RmTerminationAt(Graph, i);

        printf("termination[%zu] %s at %u (%04x:%04x) ", i, ReasonName(term->Reason), term->Node,
               term->Cs, term->Ip);
        PrintIndex("call-site", term->CallSiteNode);
        printf("\n");
    }
}

int
main(void)
{
    RM_FRAGMENT fragments[2];
    RM_ENTRY entries[2];
    RM_GRAPH *graph;
    RM_NODE detached;
    int haveDetached = 0;

    // Immutable input: the fixture arrays are only read while building the graph.
    fragments[0].Id = 1;
    fragments[0].PhysicalBase = TASK_ROM_MAIN_PHYSICAL;
    fragments[0].Bytes = task_rom_main;
    fragments[0].Size = sizeof(task_rom_main);

    fragments[1].Id = 2;
    fragments[1].PhysicalBase = TASK_ROM_FAR_PHYSICAL;
    fragments[1].Bytes = task_rom_far;
    fragments[1].Size = sizeof(task_rom_far);

    // Two logical entries which map into the same physical fragment.
    entries[0].Cs = TASK_ROM_ENTRY_CS;
    entries[0].Ip = TASK_ROM_ENTRY_IP;
    entries[1].Cs = TASK_ROM_ENTRY_CS;
    entries[1].Ip = TASK_ROM_SECOND_IP;

    graph = RmBuildGraph(fragments, 2, 1, entries, 2);
    if (NULL == graph)
    {
        fprintf(stderr, "RmBuildGraph failed\n");
        return 1;
    }

    DumpGraph(graph);

    // Copy one node, then release the graph and the input. The copy remains fully readable: the graph owned its bytes
    // and provenance, never the caller's fragments.
    if (RmNodeCount(graph) > 0)
    {
        detached = *RmNodeAt(graph, 0);
        haveDetached = 1;
    }

    RmFreeGraph(graph);
    printf("graph released\n");

    if (haveDetached)
    {
        printf("detached %04x:%04x decoded=%u length=%u ", detached.Cs, detached.Ip, detached.Decoded,
               detached.Length);
        PrintBytes(&detached);
        printf("\n");
    }

    return 0;
}
