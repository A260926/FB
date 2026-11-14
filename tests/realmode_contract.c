/*
 * Copyright (c) 2025 Bitdefender
 * SPDX-License-Identifier: Apache-2.0
 */
//
// Real-mode physical graph contract tests.
//
// These tests assert concrete graph properties against hand-authored inputs: aliasing/wrapping, per-byte fragment
// provenance, near/far call matching, the CS used after a far tail jump, shared routines, and recursion with and
// without a base return exit. They are not a differential comparison of two runs.
//
#include "bddisasm.h"
#include "bdx86_realmode.h"

#include "../task_support/check.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------------------------ */
/* Helpers.                                                                                                           */
/* ------------------------------------------------------------------------------------------------------------------ */

static uint32_t
NodeId(const RM_GRAPH *Graph, uint16_t Cs, uint16_t Ip)
{
    uint32_t index = RM_INVALID_INDEX;

    if (!RmFindNode(Graph, Cs, Ip, &index))
    {
        return RM_INVALID_INDEX;
    }

    return index;
}

static const RM_EDGE *
FindEdge(const RM_GRAPH *Graph, RM_EDGE_KIND Kind, uint32_t From, uint32_t To)
{
    size_t i;

    if (RM_INVALID_INDEX == From || RM_INVALID_INDEX == To)
    {
        return NULL;
    }

    for (i = 0; i < RmEdgeCount(Graph); ++i)
    {
        const RM_EDGE *edge = RmEdgeAt(Graph, i);

        if (Kind == edge->Kind && From == edge->FromNode && To == edge->ToNode)
        {
            return edge;
        }
    }

    return NULL;
}

static size_t
CountEdgesOfKind(const RM_GRAPH *Graph, RM_EDGE_KIND Kind)
{
    size_t i;
    size_t count = 0;

    for (i = 0; i < RmEdgeCount(Graph); ++i)
    {
        if (Kind == RmEdgeAt(Graph, i)->Kind)
        {
            ++count;
        }
    }

    return count;
}

static int
HasTermination(const RM_GRAPH *Graph, uint32_t Node, RM_TERMINATION_REASON Reason, uint32_t *CallSite)
{
    size_t i;

    for (i = 0; i < RmTerminationCount(Graph); ++i)
    {
        const RM_TERMINATION_RECORD *term = RmTerminationAt(Graph, i);

        if (Node == term->Node && Reason == term->Reason)
        {
            if (NULL != CallSite)
            {
                *CallSite = term->CallSiteNode;
            }
            return 1;
        }
    }

    return 0;
}

static size_t
CountTerminations(const RM_GRAPH *Graph, uint32_t Node, RM_TERMINATION_REASON Reason)
{
    size_t i;
    size_t count = 0;

    for (i = 0; i < RmTerminationCount(Graph); ++i)
    {
        const RM_TERMINATION_RECORD *term = RmTerminationAt(Graph, i);

        if (Node == term->Node && Reason == term->Reason)
        {
            ++count;
        }
    }

    return count;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Group: aliases                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------------ */

//
// 0001:fffe and 1000:000e resolve to the same physical byte (0x1000e). After the two-byte EB 00 their successors
// differ: 0001:0000 -> physical 0x10, 1000:0010 -> physical 0x10010. The two logical nodes must not be merged.
//
static int
case_alias_wrap(void)
{
    static const unsigned char low[] = { 0x90, 0xf4 };
    static const unsigned char shared[] = { 0xeb, 0x00 };
    static const unsigned char high[] = { 0xf4 };
    RM_FRAGMENT frags[3];
    RM_ENTRY entries[2];
    RM_GRAPH *graph;
    uint32_t a, b, aTarget, bTarget;
    const RM_NODE *na, *nb;
    const RM_EDGE *ea, *eb;
    size_t k;

    frags[0].Id = 10;
    frags[0].PhysicalBase = 0x000010;
    frags[0].Bytes = low;
    frags[0].Size = sizeof(low);

    frags[1].Id = 11;
    frags[1].PhysicalBase = 0x01000e;
    frags[1].Bytes = shared;
    frags[1].Size = sizeof(shared);

    frags[2].Id = 12;
    frags[2].PhysicalBase = 0x010010;
    frags[2].Bytes = high;
    frags[2].Size = sizeof(high);

    entries[0].Cs = 0x0001;
    entries[0].Ip = 0xfffe;
    entries[1].Cs = 0x1000;
    entries[1].Ip = 0x000e;

    graph = RmBuildGraph(frags, 3, 1, entries, 2);
    TASK_CHECK(NULL != graph);

    a = NodeId(graph, 0x0001, 0xfffe);
    b = NodeId(graph, 0x1000, 0x000e);
    TASK_CHECK(RM_INVALID_INDEX != a);
    TASK_CHECK(RM_INVALID_INDEX != b);
    // Aliasing physical bytes must still yield two logical nodes.
    TASK_CHECK(a != b);

    na = RmNodeAt(graph, a);
    nb = RmNodeAt(graph, b);
    TASK_CHECK(NULL != na && NULL != nb);
    TASK_CHECK(na->Decoded && nb->Decoded);
    TASK_CHECK(2 == na->Length && 2 == nb->Length);
    TASK_CHECK(2 == na->SourceCount && 2 == nb->SourceCount);

    // Both logical nodes read the same two physical bytes, with the same fragment provenance.
    for (k = 0; k < 2; ++k)
    {
        TASK_CHECK(11 == na->Sources[k].FragmentId);
        TASK_CHECK(11 == nb->Sources[k].FragmentId);
        TASK_CHECK(k == na->Sources[k].FragmentOffset);
        TASK_CHECK(k == nb->Sources[k].FragmentOffset);
        TASK_CHECK(0x01000e + k == na->Sources[k].PhysicalAddress);
        TASK_CHECK(0x01000e + k == nb->Sources[k].PhysicalAddress);
        TASK_CHECK(na->Bytes[k] == nb->Bytes[k]);
    }

    // The 16-bit IP successors wrap in their own segment; the physical successors differ.
    aTarget = NodeId(graph, 0x0001, 0x0000);
    bTarget = NodeId(graph, 0x1000, 0x0010);
    TASK_CHECK(RM_INVALID_INDEX != aTarget);
    TASK_CHECK(RM_INVALID_INDEX != bTarget);
    TASK_CHECK(aTarget != bTarget);
    TASK_CHECK(0x000010 == RmNodeAt(graph, aTarget)->Sources[0].PhysicalAddress);
    TASK_CHECK(0x010010 == RmNodeAt(graph, bTarget)->Sources[0].PhysicalAddress);

    ea = FindEdge(graph, RM_EDGE_JUMP_NEAR, a, aTarget);
    eb = FindEdge(graph, RM_EDGE_JUMP_NEAR, b, bTarget);
    TASK_CHECK(NULL != ea && NULL != eb);
    TASK_CHECK(ea->ToIp != eb->ToIp);

    RmFreeGraph(graph);
    return 0;
}

//
// A single instruction split across two adjacent physical fragments must collect each byte from its own fragment,
// with the right intra-fragment offset, and still decode as one instruction.
//
static int
case_fragment_sources(void)
{
    static const unsigned char first[] = { 0xe8, 0x02 };
    static const unsigned char second[] = { 0x00 };
    static const unsigned char ret[] = { 0xc3 };
    RM_FRAGMENT frags[3];
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t call, retNode, continuation;
    const RM_NODE *node;

    frags[0].Id = 1;
    frags[0].PhysicalBase = 0x01000;
    frags[0].Bytes = first;
    frags[0].Size = sizeof(first);

    frags[1].Id = 2;
    frags[1].PhysicalBase = 0x01002;
    frags[1].Bytes = second;
    frags[1].Size = sizeof(second);

    frags[2].Id = 3;
    frags[2].PhysicalBase = 0x01005;
    frags[2].Bytes = ret;
    frags[2].Size = sizeof(ret);

    entry.Cs = 0x0100;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(frags, 3, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    call = NodeId(graph, 0x0100, 0x0000);
    TASK_CHECK(RM_INVALID_INDEX != call);
    node = RmNodeAt(graph, call);
    TASK_CHECK(node->Decoded);
    TASK_CHECK(3 == node->Length);
    TASK_CHECK(3 == node->SourceCount);

    TASK_CHECK(1 == node->Sources[0].FragmentId && 0 == node->Sources[0].FragmentOffset);
    TASK_CHECK(1 == node->Sources[1].FragmentId && 1 == node->Sources[1].FragmentOffset);
    TASK_CHECK(2 == node->Sources[2].FragmentId && 0 == node->Sources[2].FragmentOffset);
    TASK_CHECK(0x01000 == node->Sources[0].PhysicalAddress);
    TASK_CHECK(0x01001 == node->Sources[1].PhysicalAddress);
    TASK_CHECK(0x01002 == node->Sources[2].PhysicalAddress);
    TASK_CHECK(0xe8 == node->Bytes[0] && 0x02 == node->Bytes[1] && 0x00 == node->Bytes[2]);

    retNode = NodeId(graph, 0x0100, 0x0005);
    continuation = NodeId(graph, 0x0100, 0x0003);
    TASK_CHECK(RM_INVALID_INDEX != retNode);
    TASK_CHECK(RM_INVALID_INDEX != continuation);
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_CALL_NEAR, call, retNode));
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_RETURN_NEAR, retNode, continuation));

    RmFreeGraph(graph);
    return 0;
}

//
// A fetch which crosses the A20 boundary is resolved byte by byte. With A20 disabled, the physical address of each
// byte is masked; with A20 enabled, bit 20 is preserved.
//
static int
case_a20_boundary(void)
{
    static const unsigned char head[] = { 0xe8 };
    static const unsigned char highTail[] = { 0x02, 0x00 };
    static const unsigned char highRet[] = { 0xc3 };
    static const unsigned char lowTail[] = { 0x02, 0x00 };
    static const unsigned char lowRet[] = { 0xc3 };
    RM_FRAGMENT frags[3];
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t call;
    const RM_NODE *node;

    entry.Cs = 0xffff;
    entry.Ip = 0x000f; // physical 0xfffff

    // A20 enabled: the crossing bytes live at 0x100000/0x100001, the target at 0x100004.
    frags[0].Id = 1;
    frags[0].PhysicalBase = 0x00fffff;
    frags[0].Bytes = head;
    frags[0].Size = sizeof(head);

    frags[1].Id = 2;
    frags[1].PhysicalBase = 0x100000;
    frags[1].Bytes = highTail;
    frags[1].Size = sizeof(highTail);

    frags[2].Id = 3;
    frags[2].PhysicalBase = 0x100004;
    frags[2].Bytes = highRet;
    frags[2].Size = sizeof(highRet);

    graph = RmBuildGraph(frags, 3, 1, &entry, 1);
    TASK_CHECK(NULL != graph);
    TASK_CHECK(RmA20Enabled(graph));
    call = NodeId(graph, 0xffff, 0x000f);
    TASK_CHECK(RM_INVALID_INDEX != call);
    node = RmNodeAt(graph, call);
    TASK_CHECK(node->Decoded && 3 == node->Length);
    TASK_CHECK(0x00fffff == node->Sources[0].PhysicalAddress);
    TASK_CHECK(0x100000 == node->Sources[1].PhysicalAddress);
    TASK_CHECK(0x100001 == node->Sources[2].PhysicalAddress);
    TASK_CHECK(1 == node->Sources[0].FragmentId);
    TASK_CHECK(2 == node->Sources[1].FragmentId);
    TASK_CHECK(2 == node->Sources[2].FragmentId);
    TASK_CHECK(RM_INVALID_INDEX != NodeId(graph, 0xffff, 0x0014));
    RmFreeGraph(graph);

    // A20 disabled: the same logical fetch wraps below 1 MiB, so the tail bytes come from physical 0x0/0x1 and the
    // call target is at physical 0x4.
    frags[0].Id = 1;
    frags[0].PhysicalBase = 0x00fffff;
    frags[0].Bytes = head;
    frags[0].Size = sizeof(head);

    frags[1].Id = 2;
    frags[1].PhysicalBase = 0x000000;
    frags[1].Bytes = lowTail;
    frags[1].Size = sizeof(lowTail);

    frags[2].Id = 3;
    frags[2].PhysicalBase = 0x000004;
    frags[2].Bytes = lowRet;
    frags[2].Size = sizeof(lowRet);

    graph = RmBuildGraph(frags, 3, 0, &entry, 1);
    TASK_CHECK(NULL != graph);
    TASK_CHECK(!RmA20Enabled(graph));
    call = NodeId(graph, 0xffff, 0x000f);
    TASK_CHECK(RM_INVALID_INDEX != call);
    node = RmNodeAt(graph, call);
    TASK_CHECK(node->Decoded && 3 == node->Length);
    TASK_CHECK(0x00fffff == node->Sources[0].PhysicalAddress);
    TASK_CHECK(0x000000 == node->Sources[1].PhysicalAddress);
    TASK_CHECK(0x000001 == node->Sources[2].PhysicalAddress);
    TASK_CHECK(RM_INVALID_INDEX != NodeId(graph, 0xffff, 0x0014));
    RmFreeGraph(graph);

    return 0;
}

//
// An instruction at IP=ffff which needs more than one byte must stop at the IP boundary. There is no zero padding.
//
static int
case_ip_ffff_truncation(void)
{
    static const unsigned char byte[] = { 0xe8 };
    RM_FRAGMENT frag;
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t node;
    const RM_NODE *n;

    frag.Id = 1;
    frag.PhysicalBase = 0x010fff;
    frag.Bytes = byte;
    frag.Size = sizeof(byte);

    entry.Cs = 0x0100;
    entry.Ip = 0xffff;

    graph = RmBuildGraph(&frag, 1, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    node = NodeId(graph, 0x0100, 0xffff);
    TASK_CHECK(RM_INVALID_INDEX != node);
    n = RmNodeAt(graph, node);
    TASK_CHECK(!n->Decoded);
    TASK_CHECK(0 == n->Length);
    // Only the byte which fits before IP wraps was fetched; nothing was padded.
    TASK_CHECK(1 == n->SourceCount);
    TASK_CHECK(0xe8 == n->Bytes[0]);
    TASK_CHECK(HasTermination(graph, node, RM_TERM_TRUNCATED, NULL));
    // The only edge in the graph would have to leave this node; none may exist.
    TASK_CHECK(0 == RmEdgeCount(graph));

    RmFreeGraph(graph);
    return 0;
}

//
// Invalid input is rejected, and valid but empty input yields a usable empty graph. No path may crash or leak.
//
static int
case_invalid_and_empty_input(void)
{
    static const unsigned char one[] = { 0x90 };
    static const unsigned char two[] = { 0x90, 0x90 };
    RM_FRAGMENT frag;
    RM_FRAGMENT bad;
    RM_ENTRY entry;
    RM_GRAPH *graph;

    frag.Id = 1;
    frag.PhysicalBase = 0;
    frag.Bytes = one;
    frag.Size = 1;
    entry.Cs = 0;
    entry.Ip = 0;

    // No entries at all is valid: an empty graph with read-only accessors that stay in bounds.
    graph = RmBuildGraph(&frag, 1, 1, NULL, 0);
    TASK_CHECK(NULL != graph);
    TASK_CHECK(0 == RmRoutineCount(graph));
    TASK_CHECK(0 == RmNodeCount(graph));
    TASK_CHECK(0 == RmEdgeCount(graph));
    TASK_CHECK(0 == RmTerminationCount(graph));
    TASK_CHECK(NULL == RmNodeAt(graph, 0));
    TASK_CHECK(NULL == RmEdgeAt(graph, 0));
    TASK_CHECK(NULL == RmTerminationAt(graph, 0));
    TASK_CHECK(NULL == RmRoutineAt(graph, 0));
    TASK_CHECK(0 == RmNodeRoutineCount(graph, 0));
    TASK_CHECK(0 == RmEdgeRoutineCount(graph, 0));
    TASK_CHECK(!RmFindNode(graph, 0, 0, NULL));
    TASK_CHECK(!RmFindRoutine(graph, 0, 0, NULL));
    RmFreeGraph(graph);
    RmFreeGraph(NULL);

    // Malformed input.
    TASK_CHECK(NULL == RmBuildGraph(NULL, 1, 1, &entry, 1));
    TASK_CHECK(NULL == RmBuildGraph(&frag, 0, 1, &entry, 1));
    TASK_CHECK(NULL == RmBuildGraph(&frag, 1, 1, NULL, 1));

    bad = frag;
    bad.Size = 0;
    TASK_CHECK(NULL == RmBuildGraph(&bad, 1, 1, &entry, 1));

    bad = frag;
    bad.Bytes = NULL;
    TASK_CHECK(NULL == RmBuildGraph(&bad, 1, 1, &entry, 1));

    {
        RM_FRAGMENT overlap[2];

        overlap[0].Id = 1;
        overlap[0].PhysicalBase = 0x10;
        overlap[0].Bytes = one;
        overlap[0].Size = 1;

        overlap[1].Id = 2;
        overlap[1].PhysicalBase = 0x10;
        overlap[1].Bytes = two;
        overlap[1].Size = 2;

        TASK_CHECK(NULL == RmBuildGraph(overlap, 2, 1, &entry, 1));

        // Duplicate IDs, even without physical overlap, are rejected.
        overlap[1].Id = 1;
        overlap[1].PhysicalBase = 0x20;
        overlap[1].Size = 1;
        TASK_CHECK(NULL == RmBuildGraph(overlap, 2, 1, &entry, 1));

        // Adjacent fragments are accepted and both contribute bytes.
        overlap[1].Id = 2;
        overlap[1].PhysicalBase = 0x11;
        overlap[1].Size = 1;
        entry.Cs = 0x0000;
        entry.Ip = 0x0010;
        graph = RmBuildGraph(overlap, 2, 1, &entry, 1);
        TASK_CHECK(NULL != graph);
        RmFreeGraph(graph);
    }

    return 0;
}

//
// The graph must own the reported bytes and provenance. Releasing (or mutating) the input after the build must not
// affect the graph.
//
static int
case_input_release(void)
{
    static const unsigned char program[] = { 0x90, 0xf4 };
    RM_FRAGMENT frag;
    RM_ENTRY entry;
    RM_GRAPH *graph;
    unsigned char *owned;
    uint32_t node;
    const RM_NODE *n;

    owned = malloc(sizeof(program));
    TASK_CHECK(NULL != owned);
    memcpy(owned, program, sizeof(program));

    frag.Id = 77;
    frag.PhysicalBase = 0x02000;
    frag.Bytes = owned;
    frag.Size = sizeof(program);

    entry.Cs = 0x0200;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(&frag, 1, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    // Release the input: the graph must have copied everything it reports.
    memset(owned, 0xcc, sizeof(program));
    free(owned);

    node = NodeId(graph, 0x0200, 0x0000);
    TASK_CHECK(RM_INVALID_INDEX != node);
    n = RmNodeAt(graph, node);
    TASK_CHECK(n->Decoded && 1 == n->Length);
    TASK_CHECK(0x90 == n->Bytes[0]);
    TASK_CHECK(77 == n->Sources[0].FragmentId);
    TASK_CHECK(0 == n->Sources[0].FragmentOffset);
    TASK_CHECK(0x02000 == n->Sources[0].PhysicalAddress);

    RmFreeGraph(graph);
    return 0;
}

//
// Reordering the fragments and the entries must leave content and ordering unchanged.
//
static int
case_input_order_invariant(void)
{
    static const unsigned char a[] = { 0x90, 0xf4 };
    static const unsigned char b[] = { 0xe8, 0x02, 0x00 };
    static const unsigned char c[] = { 0xc3 };
    RM_FRAGMENT fwd[3];
    RM_FRAGMENT rev[3];
    RM_ENTRY entriesFwd[2];
    RM_ENTRY entriesRev[2];
    RM_GRAPH *g1;
    RM_GRAPH *g2;
    size_t i;

    fwd[0].Id = 1;
    fwd[0].PhysicalBase = 0x03000;
    fwd[0].Bytes = a;
    fwd[0].Size = sizeof(a);

    fwd[1].Id = 2;
    fwd[1].PhysicalBase = 0x03002;
    fwd[1].Bytes = b;
    fwd[1].Size = sizeof(b);

    fwd[2].Id = 3;
    fwd[2].PhysicalBase = 0x03007;
    fwd[2].Bytes = c;
    fwd[2].Size = sizeof(c);

    rev[0] = fwd[2];
    rev[1] = fwd[0];
    rev[2] = fwd[1];

    entriesFwd[0].Cs = 0x0300;
    entriesFwd[0].Ip = 0x0002;
    entriesFwd[1].Cs = 0x0300;
    entriesFwd[1].Ip = 0x0000;

    entriesRev[0] = entriesFwd[1];
    entriesRev[1] = entriesFwd[0];

    g1 = RmBuildGraph(fwd, 3, 1, entriesFwd, 2);
    TASK_CHECK(NULL != g1);
    g2 = RmBuildGraph(rev, 3, 1, entriesRev, 2);
    TASK_CHECK(NULL != g2);

    TASK_CHECK(RmRoutineCount(g1) == RmRoutineCount(g2));
    TASK_CHECK(RmNodeCount(g1) == RmNodeCount(g2));
    TASK_CHECK(RmEdgeCount(g1) == RmEdgeCount(g2));
    TASK_CHECK(RmTerminationCount(g1) == RmTerminationCount(g2));

    for (i = 0; i < RmNodeCount(g1); ++i)
    {
        TASK_CHECK(0 == memcmp(RmNodeAt(g1, i), RmNodeAt(g2, i), sizeof(RM_NODE)));
    }
    for (i = 0; i < RmRoutineCount(g1); ++i)
    {
        const RM_ROUTINE *r1 = RmRoutineAt(g1, i);
        const RM_ROUTINE *r2 = RmRoutineAt(g2, i);

        TASK_CHECK(r1->Cs == r2->Cs && r1->Ip == r2->Ip);
        TASK_CHECK(r1->EntryNode == r2->EntryNode);
        TASK_CHECK(r1->IsTopLevelEntry == r2->IsTopLevelEntry);
    }
    for (i = 0; i < RmEdgeCount(g1); ++i)
    {
        const RM_EDGE *e1 = RmEdgeAt(g1, i);
        const RM_EDGE *e2 = RmEdgeAt(g2, i);

        TASK_CHECK(0 == memcmp(e1, e2, sizeof(RM_EDGE)));
        TASK_CHECK(RmEdgeRoutineCount(g1, i) == RmEdgeRoutineCount(g2, i));
    }
    for (i = 0; i < RmTerminationCount(g1); ++i)
    {
        TASK_CHECK(0 == memcmp(RmTerminationAt(g1, i), RmTerminationAt(g2, i), sizeof(RM_TERMINATION_RECORD)));
    }

    RmFreeGraph(g1);
    RmFreeGraph(g2);
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Group: returns                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------------ */

//
// A direct near CALL into a routine with a matching near RET reaches its continuation, and the return edge names the
// exact call site and return site.
//
static int
case_near_call_return(void)
{
    static const unsigned char code[] = { 0xe8, 0x02, 0x00, 0x90, 0xf4, 0xc3 };
    RM_FRAGMENT frag;
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t call, retNode, continuation, hlt;
    const RM_EDGE *ret;
    uint32_t r;

    frag.Id = 1;
    frag.PhysicalBase = 0x03000;
    frag.Bytes = code;
    frag.Size = sizeof(code);

    entry.Cs = 0x0300;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(&frag, 1, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    call = NodeId(graph, 0x0300, 0x0000);
    retNode = NodeId(graph, 0x0300, 0x0005);
    continuation = NodeId(graph, 0x0300, 0x0003);
    hlt = NodeId(graph, 0x0300, 0x0004);
    TASK_CHECK(RM_INVALID_INDEX != call);
    TASK_CHECK(RM_INVALID_INDEX != retNode);
    TASK_CHECK(RM_INVALID_INDEX != continuation);
    TASK_CHECK(RM_INVALID_INDEX != hlt);

    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_CALL_NEAR, call, retNode));
    ret = FindEdge(graph, RM_EDGE_RETURN_NEAR, retNode, continuation);
    TASK_CHECK(NULL != ret);
    TASK_CHECK(call == ret->CallSiteNode);
    TASK_CHECK(retNode == ret->ReturnSiteNode);
    TASK_CHECK(0x0300 == ret->ToCs && 0x0003 == ret->ToIp);

    // The continuation is genuinely reachable: it flows into HLT.
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_SEQUENTIAL, continuation, hlt));
    TASK_CHECK(HasTermination(graph, hlt, RM_TERM_HLT, NULL));

    // The callee is its own routine, entered through the call, not a top-level entry.
    TASK_CHECK(RmFindRoutine(graph, 0x0300, 0x0005, &r));
    TASK_CHECK(!RmRoutineAt(graph, r)->IsTopLevelEntry);
    TASK_CHECK(1 == CountEdgesOfKind(graph, RM_EDGE_CALL_NEAR));
    TASK_CHECK(1 == CountEdgesOfKind(graph, RM_EDGE_RETURN_NEAR));

    RmFreeGraph(graph);
    return 0;
}

//
// A CALL is not automatically transparent: without a matching return summary the continuation stays unreachable.
//
static int
case_call_without_return(void)
{
    static const unsigned char code[] = { 0xe8, 0x02, 0x00, 0x90, 0xf4, 0xf4 };
    RM_FRAGMENT frag;
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t call, callee;

    frag.Id = 1;
    frag.PhysicalBase = 0x03100;
    frag.Bytes = code;
    frag.Size = sizeof(code);

    entry.Cs = 0x0310;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(&frag, 1, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    call = NodeId(graph, 0x0310, 0x0000);
    callee = NodeId(graph, 0x0310, 0x0005);
    TASK_CHECK(RM_INVALID_INDEX != call);
    TASK_CHECK(RM_INVALID_INDEX != callee);
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_CALL_NEAR, call, callee));
    TASK_CHECK(0 == CountEdgesOfKind(graph, RM_EDGE_RETURN_NEAR));
    TASK_CHECK(0 == CountEdgesOfKind(graph, RM_EDGE_RETURN_FAR));
    // The continuation node must not exist at all.
    TASK_CHECK(RM_INVALID_INDEX == NodeId(graph, 0x0310, 0x0003));
    TASK_CHECK(HasTermination(graph, callee, RM_TERM_HLT, NULL));

    RmFreeGraph(graph);
    return 0;
}

//
// A near CALL into a routine whose only exit is a far RETF cannot be paired. The exit is recorded as unresolved,
// annotated with the mismatching call site, and no continuation is produced.
//
static int
case_return_kind_mismatch(void)
{
    static const unsigned char code[] = { 0xe8, 0x02, 0x00, 0x90, 0xf4, 0xcb };
    RM_FRAGMENT frag;
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t call, retNode, callSite;

    frag.Id = 1;
    frag.PhysicalBase = 0x03200;
    frag.Bytes = code;
    frag.Size = sizeof(code);

    entry.Cs = 0x0320;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(&frag, 1, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    call = NodeId(graph, 0x0320, 0x0000);
    retNode = NodeId(graph, 0x0320, 0x0005);
    TASK_CHECK(RM_INVALID_INDEX != call);
    TASK_CHECK(RM_INVALID_INDEX != retNode);
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_CALL_NEAR, call, retNode));
    TASK_CHECK(0 == CountEdgesOfKind(graph, RM_EDGE_RETURN_FAR));
    TASK_CHECK(RM_INVALID_INDEX == NodeId(graph, 0x0320, 0x0003));

    callSite = RM_INVALID_INDEX;
    TASK_CHECK(HasTermination(graph, retNode, RM_TERM_UNRESOLVED_RETURN, &callSite));
    TASK_CHECK(call == callSite);

    RmFreeGraph(graph);
    return 0;
}

//
// A direct far CALL followed by a matching RETF returns to the call site's CS and the saved IP.
//
static int
case_far_call_return(void)
{
    static const unsigned char mainCode[] = { 0x9a, 0x10, 0x00, 0x00, 0x20, 0x90, 0xf4 };
    static const unsigned char farCode[] = { 0xcb };
    RM_FRAGMENT frags[2];
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t call, retNode, continuation;
    const RM_EDGE *ret;

    frags[0].Id = 1;
    frags[0].PhysicalBase = 0x04000;
    frags[0].Bytes = mainCode;
    frags[0].Size = sizeof(mainCode);

    frags[1].Id = 2;
    frags[1].PhysicalBase = 0x20010;
    frags[1].Bytes = farCode;
    frags[1].Size = sizeof(farCode);

    entry.Cs = 0x0400;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(frags, 2, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    call = NodeId(graph, 0x0400, 0x0000);
    retNode = NodeId(graph, 0x2000, 0x0010);
    continuation = NodeId(graph, 0x0400, 0x0005);
    TASK_CHECK(RM_INVALID_INDEX != call);
    TASK_CHECK(RM_INVALID_INDEX != retNode);
    TASK_CHECK(RM_INVALID_INDEX != continuation);

    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_CALL_FAR, call, retNode));
    ret = FindEdge(graph, RM_EDGE_RETURN_FAR, retNode, continuation);
    TASK_CHECK(NULL != ret);
    TASK_CHECK(call == ret->CallSiteNode);
    TASK_CHECK(retNode == ret->ReturnSiteNode);
    // RETF restores the call site's CS.
    TASK_CHECK(0x0400 == ret->ToCs && 0x0005 == ret->ToIp);

    RmFreeGraph(graph);
    return 0;
}

//
// A near CALL whose callee reaches a RETN after a far tail jump: the near RET keeps the return site's CS, so the
// continuation does not land back in the caller's segment.
//
static int
case_far_tail_jump_cs(void)
{
    static const unsigned char mainCode[] = {
        0xe8, 0x05, 0x00,     // 0000: CALL 0008
        0x90,                 // 0003: NOP (must stay unreachable)
        0xf4,                 // 0004: HLT
        0x90, 0x90, 0x90,     // 0005..0007
        0xea, 0x00, 0x30, 0x00, 0x20 // 0008: JMPF 2000:3000
    };
    static const unsigned char farCode[] = { 0xc3 };
    static const unsigned char landing[] = { 0xf4 };
    RM_FRAGMENT frags[3];
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t call, jumpNode, retNode;
    uint32_t continuation, wrongContinuation;
    const RM_EDGE *ret;
    uint32_t r;

    frags[0].Id = 1;
    frags[0].PhysicalBase = 0x04000;
    frags[0].Bytes = mainCode;
    frags[0].Size = sizeof(mainCode);

    frags[1].Id = 2;
    frags[1].PhysicalBase = 0x23000;
    frags[1].Bytes = farCode;
    frags[1].Size = sizeof(farCode);

    frags[2].Id = 3;
    frags[2].PhysicalBase = 0x20003;
    frags[2].Bytes = landing;
    frags[2].Size = sizeof(landing);

    entry.Cs = 0x0400;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(frags, 3, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    call = NodeId(graph, 0x0400, 0x0000);
    jumpNode = NodeId(graph, 0x0400, 0x0008);
    retNode = NodeId(graph, 0x2000, 0x3000);
    continuation = NodeId(graph, 0x2000, 0x0003);
    wrongContinuation = NodeId(graph, 0x0400, 0x0003);
    TASK_CHECK(RM_INVALID_INDEX != call);
    TASK_CHECK(RM_INVALID_INDEX != jumpNode);
    TASK_CHECK(RM_INVALID_INDEX != retNode);
    TASK_CHECK(RM_INVALID_INDEX != continuation);
    // The far tail jump keeps the same routine: no new call frame, no new routine.
    TASK_CHECK(RmFindRoutine(graph, 0x0400, 0x0008, &r));
    TASK_CHECK(!RmRoutineAt(graph, r)->IsTopLevelEntry);
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_JUMP_FAR, jumpNode, retNode));
    // The tail-jump target belongs to the callee routine only: a far jump does not start a new routine.
    TASK_CHECK(1 == RmNodeRoutineCount(graph, retNode));

    ret = FindEdge(graph, RM_EDGE_RETURN_NEAR, retNode, continuation);
    TASK_CHECK(NULL != ret);
    TASK_CHECK(call == ret->CallSiteNode);
    TASK_CHECK(retNode == ret->ReturnSiteNode);
    // Near RET restores the saved IP but keeps the return site's CS (0x2000), not the caller's (0x0400).
    TASK_CHECK(0x2000 == ret->ToCs && 0x0003 == ret->ToIp);
    // The caller-segment continuation must not exist.
    TASK_CHECK(RM_INVALID_INDEX == wrongContinuation);
    TASK_CHECK(HasTermination(graph, continuation, RM_TERM_HLT, NULL));

    RmFreeGraph(graph);
    return 0;
}

//
// A top-level RET/RETF has no call frame and is reported as an open return exit.
//
static int
case_open_return(void)
{
    static const unsigned char code[] = { 0x90, 0xc3 };
    RM_FRAGMENT frag;
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t retNode;
    uint32_t r;

    frag.Id = 1;
    frag.PhysicalBase = 0x04200;
    frag.Bytes = code;
    frag.Size = sizeof(code);

    entry.Cs = 0x0420;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(&frag, 1, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    retNode = NodeId(graph, 0x0420, 0x0001);
    TASK_CHECK(RM_INVALID_INDEX != retNode);
    TASK_CHECK(RmFindRoutine(graph, 0x0420, 0x0000, &r));
    TASK_CHECK(RmRoutineAt(graph, r)->IsTopLevelEntry);
    TASK_CHECK(HasTermination(graph, retNode, RM_TERM_OPEN_RETURN, NULL));

    RmFreeGraph(graph);
    return 0;
}

//
// Two sibling call sites into the same shared routine must not merge: each return edge names its own call site and
// its own continuation.
//
static int
case_shared_routine_siblings(void)
{
    static const unsigned char code[] = {
        0xe8, 0x05, 0x00,   // 0000: CALL 0008
        0xe8, 0x02, 0x00,   // 0003: CALL 0008
        0xf4,               // 0006: HLT
        0x90,               // 0007: padding
        0xc3                // 0008: RETN
    };
    RM_FRAGMENT frag;
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t call1, call2, retNode, cont1, cont2;
    uint32_t routine1, routine2;
    const RM_EDGE *e1;
    const RM_EDGE *e2;
    size_t i;
    size_t returns = 0;
    size_t calls = 0;

    frag.Id = 1;
    frag.PhysicalBase = 0x04300;
    frag.Bytes = code;
    frag.Size = sizeof(code);

    entry.Cs = 0x0430;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(&frag, 1, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    call1 = NodeId(graph, 0x0430, 0x0000);
    call2 = NodeId(graph, 0x0430, 0x0003);
    retNode = NodeId(graph, 0x0430, 0x0008);
    cont1 = NodeId(graph, 0x0430, 0x0003);
    cont2 = NodeId(graph, 0x0430, 0x0006);
    TASK_CHECK(RM_INVALID_INDEX != call1);
    TASK_CHECK(RM_INVALID_INDEX != call2);
    TASK_CHECK(RM_INVALID_INDEX != retNode);
    TASK_CHECK(RM_INVALID_INDEX != cont1);
    TASK_CHECK(RM_INVALID_INDEX != cont2);

    // Both calls resolve to the very same shared routine record.
    TASK_CHECK(RmFindRoutine(graph, 0x0430, 0x0008, &routine1));
    TASK_CHECK(RmFindRoutine(graph, 0x0430, 0x0008, &routine2));
    TASK_CHECK(routine1 == routine2);
    TASK_CHECK(!RmRoutineAt(graph, routine1)->IsTopLevelEntry);

    e1 = FindEdge(graph, RM_EDGE_RETURN_NEAR, retNode, cont1);
    e2 = FindEdge(graph, RM_EDGE_RETURN_NEAR, retNode, cont2);
    TASK_CHECK(NULL != e1 && NULL != e2);
    TASK_CHECK(call1 == e1->CallSiteNode);
    TASK_CHECK(call2 == e2->CallSiteNode);

    // Sibling calls must never be stitched into one path: call1 must not use the continuation of call2.
    for (i = 0; i < RmEdgeCount(graph); ++i)
    {
        const RM_EDGE *edge = RmEdgeAt(graph, i);

        if (RM_EDGE_RETURN_NEAR == edge->Kind)
        {
            ++returns;
            TASK_CHECK(!(call1 == edge->CallSiteNode && cont2 == edge->ToNode));
            TASK_CHECK(!(call2 == edge->CallSiteNode && cont1 == edge->ToNode));
        }
        if (RM_EDGE_CALL_NEAR == edge->Kind)
        {
            ++calls;
            TASK_CHECK(retNode == edge->ToNode);
        }
    }
    TASK_CHECK(2 == returns);
    TASK_CHECK(2 == calls);
    TASK_CHECK(HasTermination(graph, cont2, RM_TERM_HLT, NULL));

    RmFreeGraph(graph);
    return 0;
}

//
// Bytes shared by two logical routines are one node with two owners; shared terminations are reported once.
//
static int
case_shared_code_owners(void)
{
    static const unsigned char code[] = { 0xeb, 0x04, 0xeb, 0x02, 0x90, 0x90, 0xf4 };
    RM_FRAGMENT frag;
    RM_ENTRY entries[2];
    RM_GRAPH *graph;
    uint32_t sharedNode;
    size_t owners;

    frag.Id = 1;
    frag.PhysicalBase = 0x04400;
    frag.Bytes = code;
    frag.Size = sizeof(code);

    // Routine A jumps from 0000 to 0006; routine B jumps from 0002 to the same 0006.
    entries[0].Cs = 0x0440;
    entries[0].Ip = 0x0000;
    entries[1].Cs = 0x0440;
    entries[1].Ip = 0x0002;

    graph = RmBuildGraph(&frag, 1, 1, entries, 2);
    TASK_CHECK(NULL != graph);

    sharedNode = NodeId(graph, 0x0440, 0x0006);
    TASK_CHECK(RM_INVALID_INDEX != sharedNode);
    TASK_CHECK(RmNodeAt(graph, sharedNode)->Decoded);
    owners = RmNodeRoutineCount(graph, sharedNode);
    TASK_CHECK(2 == owners);
    TASK_CHECK(RmNodeRoutineAt(graph, sharedNode, 0) != RmNodeRoutineAt(graph, sharedNode, 1));
    TASK_CHECK(2 == RmRoutineCount(graph));
    TASK_CHECK(1 == CountTerminations(graph, sharedNode, RM_TERM_HLT));

    RmFreeGraph(graph);
    return 0;
}

//
// Every out-of-scope instruction becomes an opaque termination record at its own position; other entries survive.
//
static int
case_opaque_terminations(void)
{
    static const unsigned char code[] = {
        0xff, 0x16, 0x00, 0x00,                     // 0000: CALL word ptr [0]      (indirect)
        0x66, 0xe8, 0x02, 0x00, 0x00, 0x00,         // 0004: CALL rel32             (32-bit transfer)
        0xcd, 0x21,                                 // 000a: INT 0x21               (interrupt)
        0x89, 0xc4,                                 // 000c: MOV SP, AX             (SP write)
        0x50,                                       // 000e: PUSH AX                (stack access)
        0x88, 0x07,                                 // 000f: MOV [BX], AL           (memory write)
        0xc2, 0x04, 0x00,                           // 0011: RET 4                  (RET immediate)
        0x0f, 0x34                                  // 0014: SYSENTER               (unsupported transfer)
    };
    static const struct
    {
        uint16_t Ip;
        RM_TERMINATION_REASON Reason;
    } expected[] = {
        { 0x0000, RM_TERM_INDIRECT },
        { 0x0004, RM_TERM_CONTROL32 },
        { 0x000a, RM_TERM_INTERRUPT },
        { 0x000c, RM_TERM_SP_WRITE },
        { 0x000e, RM_TERM_STACK_ACCESS },
        { 0x000f, RM_TERM_MEMORY_WRITE },
        { 0x0011, RM_TERM_RET_IMMEDIATE },
        { 0x0014, RM_TERM_UNSUPPORTED_TRANSFER },
    };
    RM_FRAGMENT frag;
    RM_ENTRY entries[8];
    RM_GRAPH *graph;
    size_t i;

    frag.Id = 1;
    frag.PhysicalBase = 0x04500;
    frag.Bytes = code;
    frag.Size = sizeof(code);

    for (i = 0; i < 8; ++i)
    {
        entries[i].Cs = 0x0450;
        entries[i].Ip = expected[i].Ip;
    }

    graph = RmBuildGraph(&frag, 1, 1, entries, 8);
    TASK_CHECK(NULL != graph);
    TASK_CHECK(8 == RmRoutineCount(graph));

    for (i = 0; i < 8; ++i)
    {
        uint32_t node = NodeId(graph, 0x0450, expected[i].Ip);
        const RM_TERMINATION_RECORD *term = NULL;
        size_t k;

        TASK_CHECK(RM_INVALID_INDEX != node);
        TASK_CHECK(RmNodeAt(graph, node)->Decoded);
        TASK_CHECK(HasTermination(graph, node, expected[i].Reason, NULL));

        // The record carries the instruction position.
        for (k = 0; k < RmTerminationCount(graph); ++k)
        {
            const RM_TERMINATION_RECORD *candidate = RmTerminationAt(graph, k);

            if (node == candidate->Node && expected[i].Reason == candidate->Reason)
            {
                term = candidate;
            }
        }
        TASK_CHECK(NULL != term);
        TASK_CHECK(0x0450 == term->Cs);
        TASK_CHECK(expected[i].Ip == term->Ip);
    }

    RmFreeGraph(graph);
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Group: recursion                                                                                                   */
/* ------------------------------------------------------------------------------------------------------------------ */

//
// A recursive routine with a base return exit: the fixpoint summary contains the base RET, so the self-call
// continuation is reachable.
//
static int
case_recursive_with_exit(void)
{
    static const unsigned char code[] = {
        0x74, 0x06,             // 0000: JZ 0008
        0xe8, 0xfb, 0xff,       // 0002: CALL 0000
        0xf4,                   // 0005: HLT
        0x90, 0x90,             // 0006..0007
        0xc3                    // 0008: RETN
    };
    RM_FRAGMENT frag;
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t call, retNode, continuation, branch;
    const RM_EDGE *ret;

    frag.Id = 1;
    frag.PhysicalBase = 0x08000;
    frag.Bytes = code;
    frag.Size = sizeof(code);

    entry.Cs = 0x0800;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(&frag, 1, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    call = NodeId(graph, 0x0800, 0x0002);
    branch = NodeId(graph, 0x0800, 0x0000);
    retNode = NodeId(graph, 0x0800, 0x0008);
    continuation = NodeId(graph, 0x0800, 0x0005);
    TASK_CHECK(RM_INVALID_INDEX != call);
    TASK_CHECK(RM_INVALID_INDEX != branch);
    TASK_CHECK(RM_INVALID_INDEX != retNode);
    TASK_CHECK(RM_INVALID_INDEX != continuation);

    // Both branch arms are kept, without evaluating flags.
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_BRANCH_TAKEN, branch, retNode));
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_BRANCH_FALLTHROUGH, branch, call));

    // The self-call target is the same routine entry.
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_CALL_NEAR, call, branch));
    ret = FindEdge(graph, RM_EDGE_RETURN_NEAR, retNode, continuation);
    TASK_CHECK(NULL != ret);
    TASK_CHECK(call == ret->CallSiteNode);
    TASK_CHECK(retNode == ret->ReturnSiteNode);
    TASK_CHECK(HasTermination(graph, continuation, RM_TERM_HLT, NULL));

    RmFreeGraph(graph);
    return 0;
}

//
// Recursion without a base return exit must not fabricate a return through an arbitrary depth bound: the summary
// stays empty and the continuation is unreachable.
//
static int
case_recursive_without_exit(void)
{
    static const unsigned char code[] = {
        0xe8, 0xfd, 0xff,   // 0000: CALL 0000
        0xf4                // 0003: HLT (continuation, unreachable)
    };
    RM_FRAGMENT frag;
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t call;

    frag.Id = 1;
    frag.PhysicalBase = 0x08100;
    frag.Bytes = code;
    frag.Size = sizeof(code);

    entry.Cs = 0x0810;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(&frag, 1, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    call = NodeId(graph, 0x0810, 0x0000);
    TASK_CHECK(RM_INVALID_INDEX != call);
    TASK_CHECK(1 == CountEdgesOfKind(graph, RM_EDGE_CALL_NEAR));
    TASK_CHECK(0 == CountEdgesOfKind(graph, RM_EDGE_RETURN_NEAR));
    TASK_CHECK(0 == CountEdgesOfKind(graph, RM_EDGE_RETURN_FAR));
    TASK_CHECK(RM_INVALID_INDEX == NodeId(graph, 0x0810, 0x0003));

    RmFreeGraph(graph);
    return 0;
}

//
// Mutual recursion whose base exit lives in the other routine: the return summary only reaches a fixpoint after the
// second pass, and both continuations then become reachable.
//
static int
case_mutual_recursion_with_exit(void)
{
    static const unsigned char code[] = {
        0x74, 0x07,             // 0000: JZ 0009
        0xe8, 0x09, 0x00,       // 0002: CALL 000e
        0xf4,                   // 0005: HLT (A continuation)
        0x90, 0x90, 0x90,       // 0006..0008
        0xc3,                   // 0009: RETN (A base exit)
        0x90, 0x90, 0x90, 0x90, // 000a..000d
        0xe8, 0xef, 0xff,       // 000e: CALL 0000
        0x90,                   // 0011: NOP (B continuation)
        0xc3                    // 0012: RETN (B exit)
    };
    RM_FRAGMENT frag;
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t callA, callB, retA, retB, contA, contB;
    const RM_EDGE *eA, *eB;

    frag.Id = 1;
    frag.PhysicalBase = 0x09000;
    frag.Bytes = code;
    frag.Size = sizeof(code);

    entry.Cs = 0x0900;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(&frag, 1, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    callA = NodeId(graph, 0x0900, 0x0002);
    callB = NodeId(graph, 0x0900, 0x000e);
    retA = NodeId(graph, 0x0900, 0x0009);
    retB = NodeId(graph, 0x0900, 0x0012);
    contA = NodeId(graph, 0x0900, 0x0005);
    contB = NodeId(graph, 0x0900, 0x0011);
    TASK_CHECK(RM_INVALID_INDEX != callA);
    TASK_CHECK(RM_INVALID_INDEX != callB);
    TASK_CHECK(RM_INVALID_INDEX != retA);
    TASK_CHECK(RM_INVALID_INDEX != retB);
    TASK_CHECK(RM_INVALID_INDEX != contA);
    TASK_CHECK(RM_INVALID_INDEX != contB);

    // B's return summary feeds A's call, and A's base return feeds B's call.
    eA = FindEdge(graph, RM_EDGE_RETURN_NEAR, retB, contA);
    eB = FindEdge(graph, RM_EDGE_RETURN_NEAR, retA, contB);
    TASK_CHECK(NULL != eA);
    TASK_CHECK(NULL != eB);
    TASK_CHECK(callA == eA->CallSiteNode && retB == eA->ReturnSiteNode);
    TASK_CHECK(callB == eB->CallSiteNode && retA == eB->ReturnSiteNode);
    TASK_CHECK(HasTermination(graph, contA, RM_TERM_HLT, NULL));

    RmFreeGraph(graph);
    return 0;
}

//
// A called routine whose self-call continuation reveals an additional return site. B calls A, and A's recursive
// self-call continuation flows through a jump to a second RET. Re-walking A after its own summary grows is what
// discovers that second return; without it, B's call to A would only match one return site. This exercises the
// self-requeue of the summary worklist, not merely the caller dependency.
//
// Layout (fragment base 0x10000, CS=0x1000):
//   0000 X: JMP 0010; 0002..000f padding; 0010 B: CALL A (0020); 0013 NOP; 0014 HLT
//   0020 A: JZ 002b (base RET); 0022 CALL 0020 (self); 0025 JMP 0029; 0028 HLT; 0029 RET; 002b RET
//
static int
case_recursive_self_call_grows_summary(void)
{
    static const unsigned char code[] = {
        0xeb, 0x0e,             // 0000: JMP 0010
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, // 0002..000f padding
        0xe8, 0x0d, 0x00,       // 0010: CALL 0020
        0x90,                   // 0013: NOP (B continuation)
        0xf4,                   // 0014: HLT
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, // 0015..001f padding
        0x74, 0x09,             // 0020: JZ 002b
        0xe8, 0xfb, 0xff,       // 0022: CALL 0020 (self)
        0xe9, 0x01, 0x00,       // 0025: JMP 0029
        0xf4,                   // 0028: HLT
        0xc3,                   // 0029: RETN (second exit; reached only after a matched self-call)
        0x90,                   // 002a: NOP
        0xc3                    // 002b: RETN (base exit)
    };
    RM_FRAGMENT frag;
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t bCall, aEntry, baseRet, secondRet, selfCall, bCont, aCont;

    frag.Id = 1;
    frag.PhysicalBase = 0x10000;
    frag.Bytes = code;
    frag.Size = sizeof(code);

    entry.Cs = 0x1000;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(&frag, 1, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    bCall = NodeId(graph, 0x1000, 0x0010);
    bCont = NodeId(graph, 0x1000, 0x0013);
    aEntry = NodeId(graph, 0x1000, 0x0020);
    selfCall = NodeId(graph, 0x1000, 0x0022);
    aCont = NodeId(graph, 0x1000, 0x0025);
    baseRet = NodeId(graph, 0x1000, 0x002b);
    secondRet = NodeId(graph, 0x1000, 0x0029);
    TASK_CHECK(RM_INVALID_INDEX != bCall);
    TASK_CHECK(RM_INVALID_INDEX != bCont);
    TASK_CHECK(RM_INVALID_INDEX != aEntry);
    TASK_CHECK(RM_INVALID_INDEX != selfCall);
    TASK_CHECK(RM_INVALID_INDEX != aCont);
    TASK_CHECK(RM_INVALID_INDEX != baseRet);
    TASK_CHECK(RM_INVALID_INDEX != secondRet);

    // A's final summary contains both return sites, so B's CALL A matches both of them.
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_RETURN_NEAR, baseRet, bCont));
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_RETURN_NEAR, secondRet, bCont));
    // The self-call matches both as well; its continuation is the JMP at 0015, which reaches the second RET.
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_RETURN_NEAR, baseRet, aCont));
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_RETURN_NEAR, secondRet, aCont));
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_JUMP_NEAR, aCont, secondRet));
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_CALL_NEAR, bCall, aEntry));
    TASK_CHECK(NULL != FindEdge(graph, RM_EDGE_CALL_NEAR, selfCall, aEntry));

    RmFreeGraph(graph);
    return 0;
}

//
// Mutual recursion with no base exit: neither routine has a return summary, so neither continuation may appear.
//
static int
case_mutual_recursion_without_exit(void)
{
    static const unsigned char code[] = {
        0xe8, 0x0b, 0x00,   // 0000: CALL 000e
        0xf4,               // 0003: HLT (A continuation)
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, // 0004..000d
        0xe8, 0xef, 0xff,   // 000e: CALL 0000
        0xf4                // 0011: HLT (B continuation)
    };
    RM_FRAGMENT frag;
    RM_ENTRY entry;
    RM_GRAPH *graph;
    uint32_t callA, callB;

    frag.Id = 1;
    frag.PhysicalBase = 0x0a000;
    frag.Bytes = code;
    frag.Size = sizeof(code);

    entry.Cs = 0x0a00;
    entry.Ip = 0x0000;

    graph = RmBuildGraph(&frag, 1, 1, &entry, 1);
    TASK_CHECK(NULL != graph);

    callA = NodeId(graph, 0x0a00, 0x0000);
    callB = NodeId(graph, 0x0a00, 0x000e);
    TASK_CHECK(RM_INVALID_INDEX != callA);
    TASK_CHECK(RM_INVALID_INDEX != callB);
    TASK_CHECK(2 == CountEdgesOfKind(graph, RM_EDGE_CALL_NEAR));
    TASK_CHECK(0 == CountEdgesOfKind(graph, RM_EDGE_RETURN_NEAR));
    TASK_CHECK(0 == CountEdgesOfKind(graph, RM_EDGE_RETURN_FAR));
    // Neither continuation exists.
    TASK_CHECK(RM_INVALID_INDEX == NodeId(graph, 0x0a00, 0x0003));
    TASK_CHECK(RM_INVALID_INDEX == NodeId(graph, 0x0a00, 0x0011));

    RmFreeGraph(graph);
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Case registry.                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "aliases",   "wrap_aliases",          case_alias_wrap },
    { "aliases",   "fragment_sources",      case_fragment_sources },
    { "aliases",   "a20_boundary",          case_a20_boundary },
    { "aliases",   "ip_ffff_truncation",    case_ip_ffff_truncation },
    { "aliases",   "invalid_and_empty_input", case_invalid_and_empty_input },
    { "aliases",   "input_release",         case_input_release },
    { "aliases",   "input_order_invariant", case_input_order_invariant },

    { "returns",   "near_call_return",      case_near_call_return },
    { "returns",   "call_without_return",   case_call_without_return },
    { "returns",   "return_kind_mismatch",  case_return_kind_mismatch },
    { "returns",   "far_call_return",       case_far_call_return },
    { "returns",   "far_tail_jump_cs",      case_far_tail_jump_cs },
    { "returns",   "open_return",           case_open_return },
    { "returns",   "shared_routine",        case_shared_routine_siblings },
    { "returns",   "shared_code_owners",    case_shared_code_owners },
    { "returns",   "opaque_terminations",   case_opaque_terminations },

    { "recursion", "recursive_with_exit",          case_recursive_with_exit },
    { "recursion", "recursive_without_exit",       case_recursive_without_exit },
    { "recursion", "recursive_self_call_grows_summary", case_recursive_self_call_grows_summary },
    { "recursion", "mutual_recursion_with_exit",   case_mutual_recursion_with_exit },
    { "recursion", "mutual_recursion_without_exit", case_mutual_recursion_without_exit },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
