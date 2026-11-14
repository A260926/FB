/*
 * Copyright (c) 2025 Bitdefender
 * SPDX-License-Identifier: Apache-2.0
 */
#include "../inc/bddisasm.h"
#include "../inc/bdx86_realmode.h"

#include <stdlib.h>
#include <string.h>

//
// Static real-mode physical graph builder. See inc/bdx86_realmode.h for the contract.
//
// Instructions are decoded exclusively with NdDecodeEx in 16-bit code/data mode. Nothing is executed, no emulator is
// involved, and only the C standard library is used.
//
// The graph is built as follows:
//  * Logical nodes are keyed by (CS, IP), never by physical address, so aliasing segment:offset pairs stay distinct.
//  * Every fetched byte is resolved independently through the physical address translation, so an instruction may
//    span fragment boundaries, and the A20 wrap, without losing per-byte provenance.
//  * Routines are rooted at logical entries: the caller-supplied entries plus every supported direct CALL target.
//  * A routine's return summary is the set of RET/RETF sites reachable at call-depth zero. A CALL only passes its
//    continuation when the callee has a matching return summary. Summaries are computed as a least fixpoint, which
//    makes recursion stable: a recursive routine with a base return propagates to its callers, one without a base
//    return never fabricates a return.
//

#define RM_MAX_FETCH            RM_MAX_INSTRUCTION_LENGTH
#define RM_HASH_MIN_CAP         16u
#define RM_INVALID_U32          RM_INVALID_INDEX

//
// Instruction classification, computed once per logical node.
//
enum
{
    RM_CLS_CONTINUE = 0,    // Ordinary instruction: sequential successor.
    RM_CLS_HLT,             // Normal terminal instruction.
    RM_CLS_RETN,            // Supported 16-bit near return without immediate.
    RM_CLS_RETF,            // Supported 16-bit far return without immediate.
    RM_CLS_CALL_NEAR,       // Supported direct 16-bit near call.
    RM_CLS_CALL_FAR,        // Supported direct 16-bit far call.
    RM_CLS_JMP_NEAR,        // Direct 16-bit near jump (no new call frame).
    RM_CLS_JMP_FAR,         // Direct 16-bit far jump (no new call frame).
    RM_CLS_COND,            // Conditional branch: both successors kept.
    RM_CLS_INDIRECT,        // Indirect control transfer.
    RM_CLS_CONTROL32,       // 32-bit control transfer.
    RM_CLS_INTERRUPT,       // INT/INT1/INT3/INTO/IRET/UIRET.
    RM_CLS_UNSUPPORTED,     // Other control transfers (SYSENTER, XBEGIN, ...).
    RM_CLS_STACK,           // Non-call/ret stack access.
    RM_CLS_SP_WRITE,        // Non-call/ret write of SP.
    RM_CLS_MEM_WRITE,       // Non-call/ret memory write.
    RM_CLS_RET_IMM,         // RET/RETF with an immediate stack adjustment.
    RM_CLS_TRUNCATED,       // Not enough bytes (gap or IP=FFFF boundary).
    RM_CLS_DECODE_ERROR,    // NdDecodeEx rejected the fetched bytes.
};

typedef struct _RM_NODE_REC
{
    uint32_t        Key;            // (Cs << 16) | Ip.
    uint16_t        Cs;
    uint16_t        Ip;
    uint8_t         Decoded;
    uint8_t         Length;
    uint8_t         SourceCount;
    uint8_t         Class;
    uint8_t         ReturnKind;     // 0 none, 1 near, 2 far.
    uint16_t        TargetCs;       // Direct call/jump target.
    uint16_t        TargetIp;
    uint16_t        NextIp;         // (Ip + Length) & 0xFFFF.
    uint8_t         Bytes[RM_MAX_FETCH];
    RM_BYTE_SOURCE  Sources[RM_MAX_FETCH];
    char            Text[RM_TEXT_SIZE];
} RM_NODE_REC;

typedef struct _RM_ROUTINE_REC
{
    uint32_t        Key;
    uint16_t        Cs;
    uint16_t        Ip;
    uint32_t        EntryNode;      // Internal node index.
    uint8_t         TopLevel;
    uint32_t        *RetNodes;      // Committed depth-zero return sites, ascending node index.
    uint8_t         *RetKinds;      // Parallel array: 1 near, 2 far.
    size_t          RetCount;
    size_t          RetCap;
    uint32_t        *Callers;       // Routines which contain a call into this routine.
    size_t          CallerCount;
    size_t          CallerCap;
    uint8_t         Queued;         // Summary worklist membership (transient).
} RM_ROUTINE_REC;

typedef struct _RM_NODE_SLOT
{
    uint32_t        Key;
    uint32_t        Index;
    uint8_t         Used;
} RM_NODE_SLOT;

typedef struct _RM_ROUTINE_SLOT
{
    uint32_t        Key;
    uint32_t        Index;
    uint8_t         Used;
} RM_ROUTINE_SLOT;

//
// An accumulated edge occurrence. Duplicate edges produced by shared code are merged during finalization and their
// owning routines are unioned.
//
typedef struct _RM_EDGE_OCC
{
    uint32_t        From;
    uint32_t        To;
    uint32_t        CallSite;
    uint32_t        ReturnSite;
    uint32_t        Owner;
    uint8_t         Kind;
} RM_EDGE_OCC;

typedef struct _RM_TERM_OCC
{
    uint32_t        Node;
    uint32_t        CallSite;
    uint8_t         Reason;
} RM_TERM_OCC;

typedef struct _RM_PAIR_OCC
{
    uint32_t        Subject;
    uint32_t        Owner;
} RM_PAIR_OCC;

typedef struct _RM_BUILDER
{
    int                 Oom;
    int                 A20;
    RM_FRAGMENT         *Fragments;     // Sorted by physical base.
    size_t              FragmentCount;

    RM_NODE_REC         *Nodes;
    size_t              NodeCount;
    size_t              NodeCap;
    RM_NODE_SLOT        *NodeSlots;
    size_t              NodeSlotCap;

    RM_ROUTINE_REC      *Routines;
    size_t              RoutineCount;
    size_t              RoutineCap;
    RM_ROUTINE_SLOT     *RoutineSlots;
    size_t              RoutineSlotCap;

    // Walk scratch.
    uint32_t            *Stack;
    size_t              StackCap;
    size_t              StackLen;
    uint32_t            *Mark;
    size_t              MarkCap;
    uint32_t            Epoch;

    // Summary-fixpoint worklist.
    uint32_t            *Queue;
    size_t              QueueCap;
    size_t              QueueHead;
    size_t              QueueTail;

    // Emission accumulation.
    RM_EDGE_OCC         *Edges;
    size_t              EdgeCount;
    size_t              EdgeCap;
    RM_TERM_OCC         *Terms;
    size_t              TermCount;
    size_t              TermCap;
    RM_PAIR_OCC         *Calls;
    size_t              CallCount;
    size_t              CallCap;
    RM_PAIR_OCC         *NodeOwners;
    size_t              OwnerCount;
    size_t              OwnerCap;
} RM_BUILDER;

struct _RM_GRAPH
{
    int                         A20;
    RM_NODE                     *Nodes;
    size_t                      NodeCount;
    RM_ROUTINE                  *Routines;
    size_t                      RoutineCount;
    RM_EDGE                     *Edges;
    size_t                      EdgeCount;
    RM_TERMINATION_RECORD       *Terms;
    size_t                      TermCount;
    uint32_t                    *NodeOwnerFlat;
    size_t                      *NodeOwnerCount;
    uint32_t                    **NodeOwners;
    uint32_t                    *EdgeOwnerFlat;
    size_t                      *EdgeOwnerCount;
    uint32_t                    **EdgeOwners;
};

/* ------------------------------------------------------------------------------------------------------------------ */
/* Small allocation helpers.                                                                                          */
/* ------------------------------------------------------------------------------------------------------------------ */

static void *
RmAlloc(RM_BUILDER *B, size_t Size)
{
    void *p;

    if (B->Oom)
    {
        return NULL;
    }

    p = malloc(Size ? Size : 1);
    if (NULL == p)
    {
        B->Oom = 1;
    }

    return p;
}

static void *
RmRealloc(RM_BUILDER *B, void *Ptr, size_t Size)
{
    void *p;

    if (B->Oom)
    {
        return NULL;
    }

    p = realloc(Ptr, Size ? Size : 1);
    if (NULL == p)
    {
        B->Oom = 1;
        return NULL;
    }

    return p;
}

//
// Ensures *Array can hold Need elements of ElemSize. Returns non-zero on success.
//
static int
RmGrow(RM_BUILDER *B, void **Array, size_t *Cap, size_t Need, size_t ElemSize)
{
    size_t cap;
    void *p;

    if (B->Oom)
    {
        return 0;
    }

    if (Need <= *Cap)
    {
        return 1;
    }

    cap = *Cap ? *Cap : 8;
    while (cap < Need)
    {
        cap *= 2;
    }

    p = RmRealloc(B, *Array, cap * ElemSize);
    if (NULL == p)
    {
        return 0;
    }

    *Array = p;
    *Cap = cap;

    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Hashing.                                                                                                           */
/* ------------------------------------------------------------------------------------------------------------------ */

static uint32_t
RmHash32(uint32_t Value)
{
    Value ^= Value >> 16;
    Value *= UINT32_C(0x7feb352d);
    Value ^= Value >> 15;
    Value *= UINT32_C(0x846ca68b);
    Value ^= Value >> 16;
    return Value;
}

static int
RmNodeSlotsResize(RM_BUILDER *B, size_t NewCap)
{
    RM_NODE_SLOT *slots;
    size_t i;

    slots = RmAlloc(B, NewCap * sizeof(*slots));
    if (NULL == slots)
    {
        return 0;
    }
    memset(slots, 0, NewCap * sizeof(*slots));

    for (i = 0; i < B->NodeSlotCap; ++i)
    {
        size_t pos;

        if (!B->NodeSlots[i].Used)
        {
            continue;
        }

        pos = RmHash32(B->NodeSlots[i].Key) & (NewCap - 1);
        while (slots[pos].Used)
        {
            pos = (pos + 1) & (NewCap - 1);
        }
        slots[pos] = B->NodeSlots[i];
    }

    free(B->NodeSlots);
    B->NodeSlots = slots;
    B->NodeSlotCap = NewCap;

    return 1;
}

static int
RmNodeSlotsFind(const RM_BUILDER *B, uint32_t Key, uint32_t *Index)
{
    size_t pos;

    if (0 == B->NodeSlotCap)
    {
        return 0;
    }

    pos = RmHash32(Key) & (B->NodeSlotCap - 1);
    while (B->NodeSlots[pos].Used)
    {
        if (B->NodeSlots[pos].Key == Key)
        {
            *Index = B->NodeSlots[pos].Index;
            return 1;
        }
        pos = (pos + 1) & (B->NodeSlotCap - 1);
    }

    return 0;
}

static int
RmNodeSlotsInsert(RM_BUILDER *B, uint32_t Key, uint32_t Index)
{
    size_t pos;

    if (0 == B->NodeSlotCap || (B->NodeCount + 1) * 10 >= B->NodeSlotCap * 7)
    {
        size_t newCap = B->NodeSlotCap ? B->NodeSlotCap * 2 : RM_HASH_MIN_CAP;

        if (!RmNodeSlotsResize(B, newCap))
        {
            return 0;
        }
    }

    pos = RmHash32(Key) & (B->NodeSlotCap - 1);
    while (B->NodeSlots[pos].Used)
    {
        pos = (pos + 1) & (B->NodeSlotCap - 1);
    }

    B->NodeSlots[pos].Key = Key;
    B->NodeSlots[pos].Index = Index;
    B->NodeSlots[pos].Used = 1;

    return 1;
}

static int
RmRoutineSlotsResize(RM_BUILDER *B, size_t NewCap)
{
    RM_ROUTINE_SLOT *slots;
    size_t i;

    slots = RmAlloc(B, NewCap * sizeof(*slots));
    if (NULL == slots)
    {
        return 0;
    }
    memset(slots, 0, NewCap * sizeof(*slots));

    for (i = 0; i < B->RoutineSlotCap; ++i)
    {
        size_t pos;

        if (!B->RoutineSlots[i].Used)
        {
            continue;
        }

        pos = RmHash32(B->RoutineSlots[i].Key) & (NewCap - 1);
        while (slots[pos].Used)
        {
            pos = (pos + 1) & (NewCap - 1);
        }
        slots[pos] = B->RoutineSlots[i];
    }

    free(B->RoutineSlots);
    B->RoutineSlots = slots;
    B->RoutineSlotCap = NewCap;

    return 1;
}

static int
RmRoutineSlotsFind(const RM_BUILDER *B, uint32_t Key, uint32_t *Index)
{
    size_t pos;

    if (0 == B->RoutineSlotCap)
    {
        return 0;
    }

    pos = RmHash32(Key) & (B->RoutineSlotCap - 1);
    while (B->RoutineSlots[pos].Used)
    {
        if (B->RoutineSlots[pos].Key == Key)
        {
            *Index = B->RoutineSlots[pos].Index;
            return 1;
        }
        pos = (pos + 1) & (B->RoutineSlotCap - 1);
    }

    return 0;
}

static int
RmRoutineSlotsInsert(RM_BUILDER *B, uint32_t Key, uint32_t Index)
{
    size_t pos;

    if (0 == B->RoutineSlotCap || (B->RoutineCount + 1) * 10 >= B->RoutineSlotCap * 7)
    {
        size_t newCap = B->RoutineSlotCap ? B->RoutineSlotCap * 2 : RM_HASH_MIN_CAP;

        if (!RmRoutineSlotsResize(B, newCap))
        {
            return 0;
        }
    }

    pos = RmHash32(Key) & (B->RoutineSlotCap - 1);
    while (B->RoutineSlots[pos].Used)
    {
        pos = (pos + 1) & (B->RoutineSlotCap - 1);
    }

    B->RoutineSlots[pos].Key = Key;
    B->RoutineSlots[pos].Index = Index;
    B->RoutineSlots[pos].Used = 1;

    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Input validation and physical address translation.                                                                 */
/* ------------------------------------------------------------------------------------------------------------------ */

typedef struct _RM_FRAGMENT_SORT
{
    uint32_t        Key;
    size_t          Order;
} RM_FRAGMENT_SORT;

static int
RmCompareFragmentSort(const void *A, const void *B)
{
    const RM_FRAGMENT_SORT *a = (const RM_FRAGMENT_SORT *)A;
    const RM_FRAGMENT_SORT *b = (const RM_FRAGMENT_SORT *)B;

    if (a->Key < b->Key)
    {
        return -1;
    }
    if (a->Key > b->Key)
    {
        return 1;
    }
    if (a->Order < b->Order)
    {
        return -1;
    }
    if (a->Order > b->Order)
    {
        return 1;
    }
    return 0;
}

//
// Validates the fragments and produces a base-sorted copy. Returns NULL on invalid input or allocation failure.
//
static RM_FRAGMENT *
RmPrepareFragments(const RM_FRAGMENT *Fragments, size_t FragmentCount)
{
    RM_FRAGMENT *sorted;
    RM_FRAGMENT_SORT *order;
    size_t i;

    if (0 == FragmentCount || NULL == Fragments)
    {
        return NULL;
    }

    sorted = malloc(FragmentCount * sizeof(*sorted));
    order = malloc(FragmentCount * sizeof(*order));
    if (NULL == sorted || NULL == order)
    {
        free(sorted);
        free(order);
        return NULL;
    }

    for (i = 0; i < FragmentCount; ++i)
    {
        if (NULL == Fragments[i].Bytes || 0 == Fragments[i].Size)
        {
            free(sorted);
            free(order);
            return NULL;
        }
        sorted[i] = Fragments[i];
        order[i].Key = Fragments[i].PhysicalBase;
        order[i].Order = i;
    }

    // Reject overlaps.
    qsort(order, FragmentCount, sizeof(*order), RmCompareFragmentSort);
    for (i = 1; i < FragmentCount; ++i)
    {
        const RM_FRAGMENT *prev = &Fragments[order[i - 1].Order];
        const RM_FRAGMENT *cur = &Fragments[order[i].Order];

        if ((uint64_t)prev->PhysicalBase + (uint64_t)prev->Size > (uint64_t)cur->PhysicalBase)
        {
            free(sorted);
            free(order);
            return NULL;
        }
    }

    // Reject duplicate IDs.
    for (i = 0; i < FragmentCount; ++i)
    {
        order[i].Key = Fragments[i].Id;
        order[i].Order = i;
    }
    qsort(order, FragmentCount, sizeof(*order), RmCompareFragmentSort);
    for (i = 1; i < FragmentCount; ++i)
    {
        if (Fragments[order[i - 1].Order].Id == Fragments[order[i].Order].Id)
        {
            free(sorted);
            free(order);
            return NULL;
        }
    }

    // Produce the base-sorted copy.
    for (i = 0; i < FragmentCount; ++i)
    {
        order[i].Key = Fragments[i].PhysicalBase;
        order[i].Order = i;
    }
    qsort(order, FragmentCount, sizeof(*order), RmCompareFragmentSort);
    for (i = 0; i < FragmentCount; ++i)
    {
        sorted[i] = Fragments[order[i].Order];
    }

    free(order);

    return sorted;
}

static uint32_t
RmPhysicalAddress(const RM_BUILDER *B, uint16_t Cs, uint32_t Ip)
{
    uint32_t phys = ((uint32_t)Cs << 4) + Ip;

    if (!B->A20)
    {
        phys &= UINT32_C(0x000FFFFF);
    }

    return phys;
}

//
// Finds the fragment containing Phys. Returns 1 and fills in the source and byte on success.
//
static int
RmResolveByte(const RM_BUILDER *B, uint32_t Phys, RM_BYTE_SOURCE *Source, uint8_t *Byte)
{
    size_t lo = 0;
    size_t hi = B->FragmentCount;

    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        uint64_t start = (uint64_t)B->Fragments[mid].PhysicalBase;
        uint64_t end = start + (uint64_t)B->Fragments[mid].Size;

        if ((uint64_t)Phys < start)
        {
            hi = mid;
        }
        else if ((uint64_t)Phys >= end)
        {
            lo = mid + 1;
        }
        else
        {
            uint64_t off = (uint64_t)Phys - start;

            Source->FragmentId = B->Fragments[mid].Id;
            Source->FragmentOffset = (uint32_t)off;
            Source->PhysicalAddress = Phys;
            *Byte = B->Fragments[mid].Bytes[off];
            return 1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Instruction classification.                                                                                        */
/* ------------------------------------------------------------------------------------------------------------------ */

static uint16_t
RmRelativeTarget(uint16_t Ip, uint8_t Length, const INSTRUX *Ix)
{
    int64_t rel;

    switch (Ix->RelOffsLength)
    {
    case 1:
        rel = (int8_t)(uint8_t)Ix->RelativeOffset;
        break;
    case 2:
        rel = (int16_t)(uint16_t)Ix->RelativeOffset;
        break;
    case 4:
        rel = (int32_t)Ix->RelativeOffset;
        break;
    default:
        rel = 0;
        break;
    }

    // 16-bit IP arithmetic: the target wraps inside the segment.
    return (uint16_t)(((int64_t)Ip + (int64_t)Length + rel) & 0xFFFF);
}

static int
RmIsInterruptInstruction(ND_INS_CLASS Instruction)
{
    switch (Instruction)
    {
    case ND_INS_INT:
    case ND_INS_INT1:
    case ND_INS_INT3:
    case ND_INS_INTO:
    case ND_INS_IRET:
    case ND_INS_UIRET:
        return 1;
    default:
        return 0;
    }
}

static int
RmIsConditionalInstruction(ND_INS_CLASS Instruction)
{
    switch (Instruction)
    {
    case ND_INS_Jcc:
    case ND_INS_JrCXZ:
    case ND_INS_LOOP:
    case ND_INS_LOOPNZ:
    case ND_INS_LOOPZ:
        return 1;
    default:
        return 0;
    }
}

static int
RmOperandsTouchStack(const INSTRUX *Ix)
{
    uint32_t i;

    for (i = 0; i < Ix->OperandsCount; ++i)
    {
        const ND_OPERAND *op = &Ix->Operands[i];

        if (ND_OP_MEM == op->Type && op->Info.Memory.IsStack)
        {
            return 1;
        }
    }

    return 0;
}

static int
RmOperandsWriteSp(const INSTRUX *Ix)
{
    uint32_t i;

    for (i = 0; i < Ix->OperandsCount; ++i)
    {
        const ND_OPERAND *op = &Ix->Operands[i];

        if (ND_OP_REG == op->Type && ND_REG_GPR == op->Info.Register.Type &&
            NDR_RSP == op->Info.Register.Reg && (op->Access.Access & ND_ACCESS_ANY_WRITE))
        {
            return 1;
        }
    }

    return 0;
}

static int
RmOperandsWriteMemory(const INSTRUX *Ix)
{
    uint32_t i;

    for (i = 0; i < Ix->OperandsCount; ++i)
    {
        const ND_OPERAND *op = &Ix->Operands[i];

        if (ND_OP_MEM == op->Type && !op->Info.Memory.IsAG && (op->Access.Access & ND_ACCESS_ANY_WRITE))
        {
            return 1;
        }
    }

    return 0;
}

static void
RmClassify(RM_NODE_REC *Node, const INSTRUX *Ix, int HaveMap, const ND_ACCESS_MAP *Map)
{
    ND_INS_CLASS ins = Ix->Instruction;

    Node->ReturnKind = 0;
    Node->NextIp = (uint16_t)(((uint32_t)Node->Ip + (uint32_t)Ix->Length) & 0xFFFF);

    if (ND_INS_HLT == ins)
    {
        Node->Class = RM_CLS_HLT;
        return;
    }

    if (ND_INS_RETN == ins || ND_INS_RETF == ins)
    {
        Node->ReturnKind = (ND_INS_RETN == ins) ? 1 : 2;

        if (Ix->HasImm1)
        {
            Node->Class = RM_CLS_RET_IMM;
        }
        else if (ND_OPSZ_16 != Ix->EfOpMode)
        {
            // A 32-bit RET pops a 32-bit EIP/pointer: out of scope.
            Node->Class = RM_CLS_CONTROL32;
        }
        else
        {
            Node->Class = (ND_INS_RETN == ins) ? RM_CLS_RETN : RM_CLS_RETF;
        }
        return;
    }

    if (ND_INS_CALLNR == ins || ND_INS_CALLNI == ins || ND_INS_CALLFD == ins || ND_INS_CALLFI == ins)
    {
        if (Ix->BranchInfo.IsIndirect)
        {
            Node->Class = RM_CLS_INDIRECT;
            return;
        }
        if (ND_INS_CALLNR == ins)
        {
            if (4 == Ix->RelOffsLength)
            {
                // rel32 near call: a 32-bit control transfer.
                Node->Class = RM_CLS_CONTROL32;
                return;
            }
            Node->Class = RM_CLS_CALL_NEAR;
            Node->TargetCs = Node->Cs;
            Node->TargetIp = RmRelativeTarget(Node->Ip, Ix->Length, Ix);
        }
        else
        {
            if (4 != Ix->AddrLength || Ix->Address.Ip > 0xFFFF)
            {
                // A 32-bit offset far pointer: a 32-bit control transfer.
                Node->Class = RM_CLS_CONTROL32;
                return;
            }
            Node->Class = RM_CLS_CALL_FAR;
            Node->TargetCs = Ix->Address.Cs;
            Node->TargetIp = (uint16_t)Ix->Address.Ip;
        }
        return;
    }

    if (ND_INS_JMPNR == ins || ND_INS_JMPNI == ins || ND_INS_JMPFD == ins || ND_INS_JMPFI == ins)
    {
        if (Ix->BranchInfo.IsIndirect)
        {
            Node->Class = RM_CLS_INDIRECT;
            return;
        }
        if (ND_INS_JMPNR == ins)
        {
            if (4 == Ix->RelOffsLength)
            {
                // rel32 near jump: a 32-bit control transfer.
                Node->Class = RM_CLS_CONTROL32;
                return;
            }
            Node->Class = RM_CLS_JMP_NEAR;
            Node->TargetCs = Node->Cs;
            Node->TargetIp = RmRelativeTarget(Node->Ip, Ix->Length, Ix);
        }
        else
        {
            if (4 != Ix->AddrLength || Ix->Address.Ip > 0xFFFF)
            {
                // A 32-bit offset far pointer: a 32-bit control transfer.
                Node->Class = RM_CLS_CONTROL32;
                return;
            }
            Node->Class = RM_CLS_JMP_FAR;
            Node->TargetCs = Ix->Address.Cs;
            Node->TargetIp = (uint16_t)Ix->Address.Ip;
        }
        return;
    }

    if (RmIsConditionalInstruction(ins))
    {
        if (4 == Ix->RelOffsLength)
        {
            // rel32 conditional branch: a 32-bit control transfer.
            Node->Class = RM_CLS_CONTROL32;
            return;
        }
        Node->Class = RM_CLS_COND;
        Node->TargetCs = Node->Cs;
        Node->TargetIp = RmRelativeTarget(Node->Ip, Ix->Length, Ix);
        return;
    }

    if (RmIsInterruptInstruction(ins))
    {
        Node->Class = RM_CLS_INTERRUPT;
        return;
    }

    if (Ix->BranchInfo.IsBranch)
    {
        // Any other control transfer (SYSENTER/SYSCALL/SYSRET/RSM/XBEGIN/...) is outside the supported subset.
        Node->Class = RM_CLS_UNSUPPORTED;
        return;
    }

    // Ordinary instruction. The supported subset never lets an instruction below write SP, touch the stack or write
    // memory: those instructions are opaque path terminators, checked in that order.
    if ((HaveMap && 0 != Map->StackAccess) || RmOperandsTouchStack(Ix))
    {
        Node->Class = RM_CLS_STACK;
        return;
    }

    if ((HaveMap && (Map->GprAccess[NDR_RSP] & ND_ACCESS_ANY_WRITE)) || RmOperandsWriteSp(Ix))
    {
        Node->Class = RM_CLS_SP_WRITE;
        return;
    }

    if ((HaveMap && (Map->MemAccess & ND_ACCESS_ANY_WRITE)) || RmOperandsWriteMemory(Ix))
    {
        Node->Class = RM_CLS_MEM_WRITE;
        return;
    }

    Node->Class = RM_CLS_CONTINUE;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Node and routine creation.                                                                                         */
/* ------------------------------------------------------------------------------------------------------------------ */

static int
RmEnsureMarkCapacity(RM_BUILDER *B)
{
    size_t oldCap = B->MarkCap;

    if (B->MarkCap >= B->NodeCount)
    {
        return 1;
    }

    if (!RmGrow(B, (void **)&B->Mark, &B->MarkCap, B->NodeCount, sizeof(*B->Mark)))
    {
        return 0;
    }

    memset(B->Mark + oldCap, 0, (B->MarkCap - oldCap) * sizeof(*B->Mark));

    return 1;
}

static void
RmDecodeNode(RM_BUILDER *B, RM_NODE_REC *Node)
{
    uint8_t bytes[RM_MAX_FETCH];
    RM_BYTE_SOURCE sources[RM_MAX_FETCH];
    uint32_t count = 0;
    uint32_t i;
    INSTRUX ix;
    NDSTATUS status;

    for (i = 0; i < RM_MAX_FETCH; ++i)
    {
        uint32_t logicalIp = (uint32_t)Node->Ip + i;

        if (logicalIp > 0xFFFFu)
        {
            // A single instruction may not fetch past IP=FFFF.
            break;
        }

        if (!RmResolveByte(B, RmPhysicalAddress(B, Node->Cs, logicalIp), &sources[count], &bytes[count]))
        {
            // Byte not backed by any fragment: no zero padding.
            break;
        }

        ++count;
    }

    if (0 == count)
    {
        Node->Decoded = 0;
        Node->Length = 0;
        Node->SourceCount = 0;
        Node->Class = RM_CLS_TRUNCATED;
        Node->Text[0] = '\0';
        return;
    }

    memset(&ix, 0, sizeof(ix));
    status = NdDecodeEx(&ix, bytes, count, ND_CODE_16, ND_DATA_16);

    if (ND_SUCCESS(status) && ix.Length > 0 && ix.Length <= count)
    {
        ND_ACCESS_MAP map;
        int haveMap;

        Node->Decoded = 1;
        Node->Length = ix.Length;
        Node->SourceCount = (uint8_t)ix.Length;

        haveMap = 0;
        memset(&map, 0, sizeof(map));
        if (ND_SUCCESS(NdGetFullAccessMap(&ix, &map)))
        {
            haveMap = 1;
        }

        RmClassify(Node, &ix, haveMap, &map);

#if !defined(BDDISASM_NO_MNEMONIC) && !defined(BDDISASM_NO_FORMAT)
        Node->Text[0] = '\0';
        if (!ND_SUCCESS(NdToText(&ix, Node->Ip, RM_TEXT_SIZE, Node->Text)))
        {
            Node->Text[0] = '\0';
        }
#else
        Node->Text[0] = '\0';
#endif
    }
    else
    {
        Node->Decoded = 0;
        Node->Length = 0;
        Node->SourceCount = (uint8_t)count;
        Node->Class = (ND_STATUS_BUFFER_TOO_SMALL == status) ? RM_CLS_TRUNCATED : RM_CLS_DECODE_ERROR;
        Node->Text[0] = '\0';
    }

    if (Node->SourceCount > 0)
    {
        memcpy(Node->Bytes, bytes, Node->SourceCount);
        memcpy(Node->Sources, sources, (size_t)Node->SourceCount * sizeof(sources[0]));
    }
}

static uint32_t
RmEnsureNode(RM_BUILDER *B, uint16_t Cs, uint16_t Ip)
{
    uint32_t key = ((uint32_t)Cs << 16) | Ip;
    uint32_t index;

    if (RmNodeSlotsFind(B, key, &index))
    {
        return index;
    }

    if (!RmGrow(B, (void **)&B->Nodes, &B->NodeCap, B->NodeCount + 1, sizeof(*B->Nodes)))
    {
        return RM_INVALID_U32;
    }

    index = (uint32_t)B->NodeCount;
    memset(&B->Nodes[index], 0, sizeof(B->Nodes[index]));
    B->Nodes[index].Key = key;
    B->Nodes[index].Cs = Cs;
    B->Nodes[index].Ip = Ip;
    ++B->NodeCount;

    if (!RmNodeSlotsInsert(B, key, index))
    {
        return RM_INVALID_U32;
    }

    if (!RmEnsureMarkCapacity(B))
    {
        return RM_INVALID_U32;
    }

    RmDecodeNode(B, &B->Nodes[index]);

    return index;
}

static int
RmQueuePush(RM_BUILDER *B, uint32_t RoutineIndex);

static uint32_t
RmEnsureRoutine(RM_BUILDER *B, uint16_t Cs, uint16_t Ip)
{
    uint32_t key = ((uint32_t)Cs << 16) | Ip;
    uint32_t index;
    uint32_t entry;

    if (RmRoutineSlotsFind(B, key, &index))
    {
        return index;
    }

    entry = RmEnsureNode(B, Cs, Ip);
    if (RM_INVALID_U32 == entry)
    {
        return RM_INVALID_U32;
    }

    if (!RmGrow(B, (void **)&B->Routines, &B->RoutineCap, B->RoutineCount + 1, sizeof(*B->Routines)))
    {
        return RM_INVALID_U32;
    }

    index = (uint32_t)B->RoutineCount;
    memset(&B->Routines[index], 0, sizeof(B->Routines[index]));
    B->Routines[index].Key = key;
    B->Routines[index].Cs = Cs;
    B->Routines[index].Ip = Ip;
    B->Routines[index].EntryNode = entry;
    ++B->RoutineCount;

    if (!RmRoutineSlotsInsert(B, key, index))
    {
        return RM_INVALID_U32;
    }

    // A brand new routine starts with an empty summary, so it must be (re)walked by the summary fixpoint.
    if (!RmQueuePush(B, index))
    {
        return RM_INVALID_U32;
    }

    return index;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Accumulation helpers.                                                                                              */
/* ------------------------------------------------------------------------------------------------------------------ */

static void
RmAddEdge(RM_BUILDER *B, uint32_t From, uint32_t To, RM_EDGE_KIND Kind, uint32_t CallSite, uint32_t ReturnSite,
          uint32_t Owner)
{
    RM_EDGE_OCC *occ;

    if (B->Oom || !RmGrow(B, (void **)&B->Edges, &B->EdgeCap, B->EdgeCount + 1, sizeof(*B->Edges)))
    {
        return;
    }

    occ = &B->Edges[B->EdgeCount++];
    occ->From = From;
    occ->To = To;
    occ->CallSite = CallSite;
    occ->ReturnSite = ReturnSite;
    occ->Owner = Owner;
    occ->Kind = (uint8_t)Kind;
}

static void
RmAddTerm(RM_BUILDER *B, RM_TERMINATION_REASON Reason, uint32_t Node, uint32_t CallSite)
{
    RM_TERM_OCC *occ;

    if (B->Oom || !RmGrow(B, (void **)&B->Terms, &B->TermCap, B->TermCount + 1, sizeof(*B->Terms)))
    {
        return;
    }

    occ = &B->Terms[B->TermCount++];
    occ->Node = Node;
    occ->CallSite = CallSite;
    occ->Reason = (uint8_t)Reason;
}

static void
RmAddPairPair(RM_PAIR_OCC **Array, size_t *Count, size_t *Cap, uint32_t Subject, uint32_t Owner, RM_BUILDER *B)
{
    RM_PAIR_OCC *occ;

    if (B->Oom || !RmGrow(B, (void **)Array, Cap, *Count + 1, sizeof(**Array)))
    {
        return;
    }

    occ = &(*Array)[(*Count)++];
    occ->Subject = Subject;
    occ->Owner = Owner;
}

static void
RmAddNodeOwner(RM_BUILDER *B, uint32_t Node, uint32_t Routine)
{
    RmAddPairPair(&B->NodeOwners, &B->OwnerCount, &B->OwnerCap, Node, Routine, B);
}

static void
RmAddCallOcc(RM_BUILDER *B, uint32_t CallSite, uint32_t Callee)
{
    RmAddPairPair(&B->Calls, &B->CallCount, &B->CallCap, CallSite, Callee, B);
}

static int
RmPushNode(RM_BUILDER *B, uint32_t Node)
{
    if (!RmGrow(B, (void **)&B->Stack, &B->StackCap, B->StackLen + 1, sizeof(*B->Stack)))
    {
        return 0;
    }

    B->Stack[B->StackLen++] = Node;

    return 1;
}

//
// Appends an index to the summary worklist, unless it is already queued.
//
static int
RmQueuePush(RM_BUILDER *B, uint32_t RoutineIndex)
{
    if (RoutineIndex >= B->RoutineCount || B->Routines[RoutineIndex].Queued)
    {
        return 1;
    }

    if (!RmGrow(B, (void **)&B->Queue, &B->QueueCap, B->QueueTail + 1, sizeof(*B->Queue)))
    {
        return 0;
    }

    B->Queue[B->QueueTail++] = RoutineIndex;
    B->Routines[RoutineIndex].Queued = 1;

    return 1;
}

//
// Remembers that Caller contains a call instruction targeting Callee, so that a change in Callee's return summary
// re-queues Caller.
//
static void
RmAddCaller(RM_BUILDER *B, uint32_t Callee, uint32_t Caller)
{
    RM_ROUTINE_REC *rec = &B->Routines[Callee];
    size_t i;

    if (Callee == Caller)
    {
        return;
    }

    for (i = 0; i < rec->CallerCount; ++i)
    {
        if (rec->Callers[i] == Caller)
        {
            return;
        }
    }

    if (!RmGrow(B, (void **)&rec->Callers, &rec->CallerCap, rec->CallerCount + 1, sizeof(*rec->Callers)))
    {
        return;
    }

    rec->Callers[rec->CallerCount++] = Caller;
}

//
// The depth-zero return sites found by the current walk, in ascending node order. A routine walk visits each node at
// most once, so the entries are unique.
//
typedef struct _RM_RETSET
{
    uint32_t        *Nodes;
    uint8_t         *Kinds;
    size_t          Count;
    size_t          Cap;
} RM_RETSET;

static void
RmRetSetFree(RM_RETSET *Set)
{
    free(Set->Nodes);
    free(Set->Kinds);
    Set->Nodes = NULL;
    Set->Kinds = NULL;
    Set->Count = 0;
    Set->Cap = 0;
}

static int
RmGrowRetSet(RM_BUILDER *B, RM_RETSET *Set, size_t Need)
{
    uint32_t *nodes;
    uint8_t *kinds;
    size_t cap;

    if (Need <= Set->Cap)
    {
        return 1;
    }

    cap = Set->Cap ? Set->Cap * 2 : 8;
    while (cap < Need)
    {
        cap *= 2;
    }

    nodes = RmRealloc(B, Set->Nodes, cap * sizeof(*nodes));
    if (NULL == nodes)
    {
        return 0;
    }
    Set->Nodes = nodes;

    kinds = RmRealloc(B, Set->Kinds, cap * sizeof(*kinds));
    if (NULL == kinds)
    {
        return 0;
    }
    Set->Kinds = kinds;
    Set->Cap = cap;

    return 1;
}

static int
RmRetSetAppend(RM_BUILDER *B, RM_RETSET *Set, uint32_t Node, uint8_t Kind)
{
    if (!RmGrowRetSet(B, Set, Set->Count + 1))
    {
        return 0;
    }

    Set->Nodes[Set->Count] = Node;
    Set->Kinds[Set->Count] = Kind;
    ++Set->Count;

    return 1;
}

static void
RmRetSetSort(RM_RETSET *Set)
{
    size_t i;

    for (i = 1; i < Set->Count; ++i)
    {
        uint32_t node = Set->Nodes[i];
        uint8_t kind = Set->Kinds[i];
        size_t j = i;

        while (j > 0 && Set->Nodes[j - 1] > node)
        {
            Set->Nodes[j] = Set->Nodes[j - 1];
            Set->Kinds[j] = Set->Kinds[j - 1];
            --j;
        }

        Set->Nodes[j] = node;
        Set->Kinds[j] = kind;
    }
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Routine walk.                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------------ */

static uint32_t
RmNextEpoch(RM_BUILDER *B)
{
    if (0 == B->Epoch || UINT32_MAX == B->Epoch)
    {
        if (B->MarkCap > 0)
        {
            memset(B->Mark, 0, B->MarkCap * sizeof(*B->Mark));
        }
        B->Epoch = 1;
    }
    else
    {
        ++B->Epoch;
    }

    return B->Epoch;
}

static RM_TERMINATION_REASON
RmClassTermination(uint8_t Class)
{
    switch (Class)
    {
    case RM_CLS_HLT:
        return RM_TERM_HLT;
    case RM_CLS_INDIRECT:
        return RM_TERM_INDIRECT;
    case RM_CLS_CONTROL32:
        return RM_TERM_CONTROL32;
    case RM_CLS_INTERRUPT:
        return RM_TERM_INTERRUPT;
    case RM_CLS_UNSUPPORTED:
        return RM_TERM_UNSUPPORTED_TRANSFER;
    case RM_CLS_STACK:
        return RM_TERM_STACK_ACCESS;
    case RM_CLS_SP_WRITE:
        return RM_TERM_SP_WRITE;
    case RM_CLS_MEM_WRITE:
        return RM_TERM_MEMORY_WRITE;
    case RM_CLS_RET_IMM:
        return RM_TERM_RET_IMMEDIATE;
    case RM_CLS_TRUNCATED:
        return RM_TERM_TRUNCATED;
    case RM_CLS_DECODE_ERROR:
    default:
        return RM_TERM_DECODE_ERROR;
    }
}

//
// Walks one routine with the committed callee return summaries.
//
// Emit = 0: reports the set of depth-zero RET/RETF sites and, when it differs from the committed summary, commits it.
// Emit = 1: emits edges, per-instruction terminations and node ownership; nothing is committed.
//
// Returns 1 when the committed summary changed (Emit = 0 only).
//
static int
RmWalkRoutine(RM_BUILDER *B, uint32_t RoutineIndex, int Emit)
{
    uint32_t epoch;
    uint32_t entry;
    RM_RETSET set;
    int changed = 0;

    memset(&set, 0, sizeof(set));

    if (RoutineIndex >= B->RoutineCount)
    {
        return 0;
    }

    if (!RmEnsureMarkCapacity(B))
    {
        return 0;
    }

    entry = B->Routines[RoutineIndex].EntryNode;
    B->StackLen = 0;

    epoch = RmNextEpoch(B);

    if (!RmPushNode(B, entry))
    {
        return 0;
    }

    while (B->StackLen > 0)
    {
        uint32_t nodeIndex = B->Stack[--B->StackLen];
        uint8_t cls;
        uint8_t retKind;
        uint16_t cs;
        uint16_t nextIp;
        uint16_t targetCs;
        uint16_t targetIp;

        if (nodeIndex >= B->NodeCount)
        {
            continue;
        }

        if (B->Mark[nodeIndex] == epoch)
        {
            continue;
        }
        B->Mark[nodeIndex] = epoch;

        if (Emit)
        {
            RmAddNodeOwner(B, nodeIndex, RoutineIndex);
        }

        // Copy everything needed before any helper can reallocate the node array.
        cls = B->Nodes[nodeIndex].Class;
        retKind = B->Nodes[nodeIndex].ReturnKind;
        cs = B->Nodes[nodeIndex].Cs;
        nextIp = B->Nodes[nodeIndex].NextIp;
        targetCs = B->Nodes[nodeIndex].TargetCs;
        targetIp = B->Nodes[nodeIndex].TargetIp;

        switch (cls)
        {
        case RM_CLS_CONTINUE:
        {
            uint32_t next = RmEnsureNode(B, cs, nextIp);

            if (RM_INVALID_U32 == next)
            {
                goto done;
            }
            if (Emit)
            {
                RmAddEdge(B, nodeIndex, next, RM_EDGE_SEQUENTIAL, RM_INVALID_U32, RM_INVALID_U32, RoutineIndex);
            }
            RmPushNode(B, next);
            break;
        }

        case RM_CLS_HLT:
            if (Emit)
            {
                RmAddTerm(B, RM_TERM_HLT, nodeIndex, RM_INVALID_U32);
            }
            break;

        case RM_CLS_JMP_NEAR:
        case RM_CLS_JMP_FAR:
        {
            uint32_t target = RmEnsureNode(B, targetCs, targetIp);

            if (RM_INVALID_U32 == target)
            {
                goto done;
            }
            if (Emit)
            {
                RmAddEdge(B, nodeIndex, target,
                          (RM_CLS_JMP_NEAR == cls) ? RM_EDGE_JUMP_NEAR : RM_EDGE_JUMP_FAR,
                          RM_INVALID_U32, RM_INVALID_U32, RoutineIndex);
            }
            // An ordinary jump does not create a new call frame: the target stays inside this routine.
            RmPushNode(B, target);
            break;
        }

        case RM_CLS_COND:
        {
            uint32_t target = RmEnsureNode(B, targetCs, targetIp);
            uint32_t fall = RmEnsureNode(B, cs, nextIp);

            if (RM_INVALID_U32 == target || RM_INVALID_U32 == fall)
            {
                goto done;
            }
            if (Emit)
            {
                // Both arms are kept; no flag evaluation is performed.
                RmAddEdge(B, nodeIndex, target, RM_EDGE_BRANCH_TAKEN, RM_INVALID_U32, RM_INVALID_U32,
                          RoutineIndex);
                RmAddEdge(B, nodeIndex, fall, RM_EDGE_BRANCH_FALLTHROUGH, RM_INVALID_U32, RM_INVALID_U32,
                          RoutineIndex);
            }
            RmPushNode(B, target);
            RmPushNode(B, fall);
            break;
        }

        case RM_CLS_CALL_NEAR:
        case RM_CLS_CALL_FAR:
        {
            const RM_ROUTINE_REC *calleeRec;
            uint32_t callee;
            size_t i;

            callee = RmEnsureRoutine(B, targetCs, targetIp);
            if (RM_INVALID_U32 == callee || callee >= B->RoutineCount)
            {
                goto done;
            }

            calleeRec = &B->Routines[callee];

            if (!Emit)
            {
                // Record the reverse dependency so a change in the callee re-queues this routine.
                RmAddCaller(B, callee, RoutineIndex);
            }
            else
            {
                RmAddEdge(B, nodeIndex, calleeRec->EntryNode,
                          (RM_CLS_CALL_NEAR == cls) ? RM_EDGE_CALL_NEAR : RM_EDGE_CALL_FAR,
                          RM_INVALID_U32, RM_INVALID_U32, RoutineIndex);
                RmAddCallOcc(B, nodeIndex, callee);
            }

            // The continuation is reachable only when the callee has a matching return summary. Each matching return
            // site produces its own return edge, annotated with this exact call site.
            for (i = 0; i < calleeRec->RetCount; ++i)
            {
                uint32_t retSite = calleeRec->RetNodes[i];
                uint8_t retSiteKind = calleeRec->RetKinds[i];
                uint16_t contCs;
                uint32_t cont;

                if (retSite >= B->NodeCount)
                {
                    continue;
                }

                if (RM_CLS_CALL_NEAR == cls)
                {
                    if (1 != retSiteKind)
                    {
                        continue;
                    }
                    // A near RET restores the saved IP but keeps the CS of the return site.
                    contCs = B->Nodes[retSite].Cs;
                }
                else
                {
                    if (2 != retSiteKind)
                    {
                        continue;
                    }
                    // A far RETF restores both the saved CS and IP.
                    contCs = cs;
                }

                cont = RmEnsureNode(B, contCs, nextIp);
                if (RM_INVALID_U32 == cont)
                {
                    goto done;
                }

                if (Emit)
                {
                    RmAddEdge(B, retSite, cont,
                              (RM_CLS_CALL_NEAR == cls) ? RM_EDGE_RETURN_NEAR : RM_EDGE_RETURN_FAR,
                              nodeIndex, retSite, RoutineIndex);
                }

                RmPushNode(B, cont);
            }

            // No matching return summary: the call edge into the callee exists, but the continuation is not reached
            // and no return is fabricated.
            break;
        }

        case RM_CLS_RETN:
        case RM_CLS_RETF:
            if (Emit && B->Routines[RoutineIndex].TopLevel)
            {
                // A top-level RET has no call frame: it is an open return exit.
                RmAddTerm(B, RM_TERM_OPEN_RETURN, nodeIndex, RM_INVALID_U32);
            }
            if (!RmRetSetAppend(B, &set, nodeIndex, retKind))
            {
                goto done;
            }
            break;

        case RM_CLS_INDIRECT:
        case RM_CLS_CONTROL32:
        case RM_CLS_INTERRUPT:
        case RM_CLS_UNSUPPORTED:
        case RM_CLS_STACK:
        case RM_CLS_SP_WRITE:
        case RM_CLS_MEM_WRITE:
        case RM_CLS_RET_IMM:
        case RM_CLS_TRUNCATED:
        case RM_CLS_DECODE_ERROR:
            if (Emit)
            {
                RmAddTerm(B, RmClassTermination(cls), nodeIndex, RM_INVALID_U32);
            }
            break;

        default:
            break;
        }
    }

done:
    if (B->Oom)
    {
        RmRetSetFree(&set);
        return changed;
    }

    if (!Emit)
    {
        RM_ROUTINE_REC *routine = &B->Routines[RoutineIndex];

        RmRetSetSort(&set);

        if (set.Count != routine->RetCount ||
            (set.Count > 0 &&
             (0 != memcmp(routine->RetNodes, set.Nodes, set.Count * sizeof(*set.Nodes)) ||
              0 != memcmp(routine->RetKinds, set.Kinds, set.Count * sizeof(*set.Kinds)))))
        {
            if (0 == set.Count)
            {
                free(routine->RetNodes);
                free(routine->RetKinds);
                routine->RetNodes = NULL;
                routine->RetKinds = NULL;
                routine->RetCap = 0;
                routine->RetCount = 0;
            }
            else
            {
                uint32_t *nodes = RmRealloc(B, routine->RetNodes, set.Count * sizeof(*nodes));
                uint8_t *kinds;

                if (NULL == nodes)
                {
                    RmRetSetFree(&set);
                    return changed;
                }
                routine->RetNodes = nodes;

                kinds = RmRealloc(B, routine->RetKinds, set.Count * sizeof(*kinds));
                if (NULL == kinds)
                {
                    RmRetSetFree(&set);
                    return changed;
                }
                routine->RetKinds = kinds;

                memcpy(routine->RetNodes, set.Nodes, set.Count * sizeof(*set.Nodes));
                memcpy(routine->RetKinds, set.Kinds, set.Count * sizeof(*set.Kinds));
                routine->RetCap = set.Count;
                routine->RetCount = set.Count;
            }

            changed = 1;
        }
    }

    RmRetSetFree(&set);

    return changed;
}

//
// Least-fixpoint over the routine return summaries, driven by a worklist. A summary only ever grows (the set of
// depth-zero return sites is monotone in the callees' summaries), so processing a routine at most once per summary
// change reaches the least fixpoint. Routines are re-queued only when a callee's committed summary changed, and
// newly discovered routines are queued at creation.
//
static void
RmComputeSummaries(RM_BUILDER *B)
{
    size_t i;

    B->QueueHead = 0;
    B->QueueTail = 0;

    // Clear any membership flag left over from routine creation, then seed every top-level entry. Every other
    // routine is queued when RmEnsureRoutine creates it during a walk.
    for (i = 0; i < B->RoutineCount; ++i)
    {
        B->Routines[i].Queued = 0;
    }

    for (i = 0; i < B->RoutineCount; ++i)
    {
        if (B->Routines[i].TopLevel)
        {
            (void)RmQueuePush(B, (uint32_t)i);
        }
    }

    for (;;)
    {
        size_t head;
        size_t tail;

        if (B->Oom || B->QueueHead >= B->QueueTail)
        {
            return;
        }

        // Take the current queue segment. Walking may append more entries, which are handled by later iterations.
        head = B->QueueHead;
        tail = B->QueueTail;
        B->QueueHead = tail;

        for (i = head; i < tail; ++i)
        {
            uint32_t routine = B->Queue[i];
            size_t caller;

            if (routine >= B->RoutineCount)
            {
                continue;
            }

            B->Routines[routine].Queued = 0;

            if (!RmWalkRoutine(B, routine, 0))
            {
                continue;
            }

            if (B->Oom)
            {
                return;
            }

            // The summary changed: re-queue the routine itself (a self-recursive callee must be re-walked with its own
            // enlarged summary) and every routine which contains a call into this one.
            if (!RmQueuePush(B, routine))
            {
                return;
            }

            for (caller = 0; caller < B->Routines[routine].CallerCount; ++caller)
            {
                if (!RmQueuePush(B, B->Routines[routine].Callers[caller]))
                {
                    return;
                }
            }
        }
    }
}

//
// Records unmatched return exits. A return site of the callee whose kind does not match a given call site is
// reported as an unresolved exit annotated with that call site; non-paired stack tricks are never simulated.
//
static void
RmRecordUnresolvedReturns(RM_BUILDER *B)
{
    size_t i;

    for (i = 0; i < B->CallCount; ++i)
    {
        uint32_t callSite = B->Calls[i].Subject;
        uint32_t callee = B->Calls[i].Owner;
        uint8_t wantKind;
        size_t r;

        if (callee >= B->RoutineCount || callSite >= B->NodeCount)
        {
            continue;
        }

        switch (B->Nodes[callSite].Class)
        {
        case RM_CLS_CALL_NEAR:
            wantKind = 1;
            break;
        case RM_CLS_CALL_FAR:
            wantKind = 2;
            break;
        default:
            continue;
        }

        for (r = 0; r < B->Routines[callee].RetCount; ++r)
        {
            if (B->Routines[callee].RetKinds[r] != wantKind)
            {
                RmAddTerm(B, RM_TERM_UNRESOLVED_RETURN, B->Routines[callee].RetNodes[r], callSite);
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Finalization: canonical sorting, de-duplication and graph construction.                                             */
/* ------------------------------------------------------------------------------------------------------------------ */

typedef struct _RM_ORDER
{
    uint16_t        Cs;
    uint16_t        Ip;
    uint32_t        Orig;
} RM_ORDER;

static int
RmCompareOrder(const void *A, const void *B)
{
    const RM_ORDER *a = (const RM_ORDER *)A;
    const RM_ORDER *b = (const RM_ORDER *)B;

    if (a->Cs != b->Cs)
    {
        return (a->Cs < b->Cs) ? -1 : 1;
    }
    if (a->Ip != b->Ip)
    {
        return (a->Ip < b->Ip) ? -1 : 1;
    }
    return 0;
}

typedef struct _RM_EDGE_SORT
{
    uint32_t        From;
    uint32_t        Kind;
    uint32_t        To;
    uint32_t        Call;
    uint32_t        Ret;
    uint32_t        Owner;
} RM_EDGE_SORT;

static int
RmCompareEdgeSort(const void *A, const void *B)
{
    const RM_EDGE_SORT *a = (const RM_EDGE_SORT *)A;
    const RM_EDGE_SORT *b = (const RM_EDGE_SORT *)B;

    if (a->From != b->From)
    {
        return (a->From < b->From) ? -1 : 1;
    }
    if (a->Kind != b->Kind)
    {
        return (a->Kind < b->Kind) ? -1 : 1;
    }
    if (a->To != b->To)
    {
        return (a->To < b->To) ? -1 : 1;
    }
    if (a->Call != b->Call)
    {
        return (a->Call < b->Call) ? -1 : 1;
    }
    if (a->Ret != b->Ret)
    {
        return (a->Ret < b->Ret) ? -1 : 1;
    }
    if (a->Owner != b->Owner)
    {
        return (a->Owner < b->Owner) ? -1 : 1;
    }
    return 0;
}

static int
RmEdgeGroupKeyEqual(const RM_EDGE_SORT *a, const RM_EDGE_SORT *b)
{
    return a->From == b->From && a->Kind == b->Kind && a->To == b->To && a->Call == b->Call && a->Ret == b->Ret;
}

typedef struct _RM_TERM_SORT
{
    uint32_t        Node;
    uint32_t        Reason;
    uint32_t        Call;
} RM_TERM_SORT;

static int
RmCompareTermSort(const void *A, const void *B)
{
    const RM_TERM_SORT *a = (const RM_TERM_SORT *)A;
    const RM_TERM_SORT *b = (const RM_TERM_SORT *)B;

    if (a->Node != b->Node)
    {
        return (a->Node < b->Node) ? -1 : 1;
    }
    if (a->Reason != b->Reason)
    {
        return (a->Reason < b->Reason) ? -1 : 1;
    }
    if (a->Call != b->Call)
    {
        return (a->Call < b->Call) ? -1 : 1;
    }
    return 0;
}

static int
RmComparePairSort(const void *A, const void *B)
{
    const RM_PAIR_OCC *a = (const RM_PAIR_OCC *)A;
    const RM_PAIR_OCC *b = (const RM_PAIR_OCC *)B;

    if (a->Subject != b->Subject)
    {
        return (a->Subject < b->Subject) ? -1 : 1;
    }
    if (a->Owner != b->Owner)
    {
        return (a->Owner < b->Owner) ? -1 : 1;
    }
    return 0;
}

//
// Builds sorted, de-duplicated per-subject owner lists from (Subject, Owner) occurrences. On success, OutCount is
// indexed by subject and holds the number of distinct owners, OutFlat holds the compact owner values and OutPtrs
// points at the first owner inside OutFlat for each subject.
//
static int
RmBuildOwnerLists(RM_PAIR_OCC *Occ, size_t Count, size_t SubjectCount, size_t **OutCount, uint32_t **OutFlat,
                  uint32_t ***OutPtrs)
{
    size_t *counts;
    uint32_t *flat;
    uint32_t **ptrs;
    size_t *fill;
    size_t i;
    size_t unique = 0;
    size_t offset;

    *OutCount = NULL;
    *OutFlat = NULL;
    *OutPtrs = NULL;

    counts = calloc(SubjectCount ? SubjectCount : 1, sizeof(*counts));
    if (NULL == counts)
    {
        return 0;
    }

    qsort(Occ, Count, sizeof(*Occ), RmComparePairSort);

    for (i = 0; i < Count; ++i)
    {
        if (Occ[i].Subject >= SubjectCount)
        {
            continue;
        }
        if (i > 0 && Occ[i].Subject == Occ[i - 1].Subject && Occ[i].Owner == Occ[i - 1].Owner)
        {
            continue;
        }
        ++counts[Occ[i].Subject];
        ++unique;
    }

    flat = malloc((unique ? unique : 1) * sizeof(*flat));
    ptrs = calloc(SubjectCount ? SubjectCount : 1, sizeof(*ptrs));
    fill = calloc(SubjectCount ? SubjectCount : 1, sizeof(*fill));
    if (NULL == flat || NULL == ptrs || NULL == fill)
    {
        free(counts);
        free(flat);
        free(ptrs);
        free(fill);
        return 0;
    }

    offset = 0;
    for (i = 0; i < SubjectCount; ++i)
    {
        ptrs[i] = flat + offset;
        offset += counts[i];
    }

    for (i = 0; i < Count; ++i)
    {
        if (Occ[i].Subject >= SubjectCount)
        {
            continue;
        }
        if (i > 0 && Occ[i].Subject == Occ[i - 1].Subject && Occ[i].Owner == Occ[i - 1].Owner)
        {
            continue;
        }
        ptrs[Occ[i].Subject][fill[Occ[i].Subject]] = Occ[i].Owner;
        ++fill[Occ[i].Subject];
    }

    free(fill);

    *OutCount = counts;
    *OutFlat = flat;
    *OutPtrs = ptrs;

    return 1;
}

static int
RmCompareU32Asc(const void *A, const void *B)
{
    uint32_t a = *(const uint32_t *)A;
    uint32_t b = *(const uint32_t *)B;

    return (a < b) ? -1 : (a > b) ? 1 : 0;
}

static void
RmFreeBuilder(RM_BUILDER *B)
{
    size_t i;

    for (i = 0; i < B->RoutineCount; ++i)
    {
        free(B->Routines[i].RetNodes);
        free(B->Routines[i].RetKinds);
        free(B->Routines[i].Callers);
    }

    free(B->Fragments);
    free(B->Nodes);
    free(B->NodeSlots);
    free(B->Routines);
    free(B->RoutineSlots);
    free(B->Stack);
    free(B->Mark);
    free(B->Queue);
    free(B->Edges);
    free(B->Terms);
    free(B->Calls);
    free(B->NodeOwners);
    memset(B, 0, sizeof(*B));
}

static void
RmFreeGraphInternal(RM_GRAPH *Graph)
{
    if (NULL == Graph)
    {
        return;
    }

    free(Graph->Nodes);
    free(Graph->Routines);
    free(Graph->Edges);
    free(Graph->Terms);
    free(Graph->NodeOwnerFlat);
    free(Graph->NodeOwnerCount);
    free(Graph->NodeOwners);
    free(Graph->EdgeOwnerFlat);
    free(Graph->EdgeOwnerCount);
    free(Graph->EdgeOwners);
    free(Graph);
}

static RM_GRAPH *
RmFinalize(RM_BUILDER *B)
{
    RM_GRAPH *graph;
    RM_ORDER *nodeOrder = NULL;
    RM_ORDER *routineOrder = NULL;
    uint32_t *nodeRank = NULL;
    uint32_t *routineRank = NULL;
    RM_EDGE_SORT *edgeSort = NULL;
    RM_TERM_SORT *termSort = NULL;
    RM_PAIR_OCC *nodeOwnerOcc = NULL;
    size_t i;

    graph = calloc(1, sizeof(*graph));
    if (NULL == graph)
    {
        return NULL;
    }

    nodeOrder = malloc((B->NodeCount ? B->NodeCount : 1) * sizeof(*nodeOrder));
    routineOrder = malloc((B->RoutineCount ? B->RoutineCount : 1) * sizeof(*routineOrder));
    nodeRank = malloc((B->NodeCount ? B->NodeCount : 1) * sizeof(*nodeRank));
    routineRank = malloc((B->RoutineCount ? B->RoutineCount : 1) * sizeof(*routineRank));
    if (NULL == nodeOrder || NULL == routineOrder || NULL == nodeRank || NULL == routineRank)
    {
        goto fail;
    }

    for (i = 0; i < B->NodeCount; ++i)
    {
        nodeOrder[i].Cs = B->Nodes[i].Cs;
        nodeOrder[i].Ip = B->Nodes[i].Ip;
        nodeOrder[i].Orig = (uint32_t)i;
    }
    qsort(nodeOrder, B->NodeCount, sizeof(*nodeOrder), RmCompareOrder);
    for (i = 0; i < B->NodeCount; ++i)
    {
        nodeRank[nodeOrder[i].Orig] = (uint32_t)i;
    }

    for (i = 0; i < B->RoutineCount; ++i)
    {
        routineOrder[i].Cs = B->Routines[i].Cs;
        routineOrder[i].Ip = B->Routines[i].Ip;
        routineOrder[i].Orig = (uint32_t)i;
    }
    qsort(routineOrder, B->RoutineCount, sizeof(*routineOrder), RmCompareOrder);
    for (i = 0; i < B->RoutineCount; ++i)
    {
        routineRank[routineOrder[i].Orig] = (uint32_t)i;
    }

    // Nodes, in canonical (CS, IP) order.
    if (B->NodeCount > 0)
    {
        graph->Nodes = calloc(B->NodeCount, sizeof(*graph->Nodes));
        if (NULL == graph->Nodes)
        {
            goto fail;
        }
    }
    for (i = 0; i < B->NodeCount; ++i)
    {
        const RM_NODE_REC *src = &B->Nodes[nodeOrder[i].Orig];
        RM_NODE *dst = &graph->Nodes[i];

        dst->Cs = src->Cs;
        dst->Ip = src->Ip;
        dst->Decoded = src->Decoded;
        dst->Length = src->Length;
        dst->SourceCount = src->SourceCount;
        memcpy(dst->Bytes, src->Bytes, sizeof(dst->Bytes));
        memcpy(dst->Sources, src->Sources, sizeof(dst->Sources));
        memcpy(dst->Text, src->Text, sizeof(dst->Text));
        dst->Text[sizeof(dst->Text) - 1] = '\0';
    }
    graph->NodeCount = B->NodeCount;

    // Routines, in canonical (CS, IP) order.
    if (B->RoutineCount > 0)
    {
        graph->Routines = calloc(B->RoutineCount, sizeof(*graph->Routines));
        if (NULL == graph->Routines)
        {
            goto fail;
        }
    }
    for (i = 0; i < B->RoutineCount; ++i)
    {
        const RM_ROUTINE_REC *src = &B->Routines[routineOrder[i].Orig];
        RM_ROUTINE *dst = &graph->Routines[i];

        dst->Cs = src->Cs;
        dst->Ip = src->Ip;
        dst->EntryNode = nodeRank[src->EntryNode];
        dst->IsTopLevelEntry = src->TopLevel ? 1 : 0;
    }
    graph->RoutineCount = B->RoutineCount;

    // Edges: sort by canonical node ranks, de-duplicate by transfer identity, union owners.
    edgeSort = malloc((B->EdgeCount ? B->EdgeCount : 1) * sizeof(*edgeSort));
    if (NULL == edgeSort)
    {
        goto fail;
    }
    for (i = 0; i < B->EdgeCount; ++i)
    {
        const RM_EDGE_OCC *occ = &B->Edges[i];

        edgeSort[i].From = nodeRank[occ->From];
        edgeSort[i].Kind = occ->Kind;
        edgeSort[i].To = nodeRank[occ->To];
        edgeSort[i].Call = (RM_INVALID_U32 == occ->CallSite) ? UINT32_MAX : nodeRank[occ->CallSite];
        edgeSort[i].Ret = (RM_INVALID_U32 == occ->ReturnSite) ? UINT32_MAX : nodeRank[occ->ReturnSite];
        edgeSort[i].Owner = routineRank[occ->Owner];
    }
    qsort(edgeSort, B->EdgeCount, sizeof(*edgeSort), RmCompareEdgeSort);

    {
        size_t groups = 0;
        size_t ownerTotal = 0;
        size_t start = 0;

        for (i = 1; i <= B->EdgeCount; ++i)
        {
            if (i == B->EdgeCount || !RmEdgeGroupKeyEqual(&edgeSort[i], &edgeSort[start]))
            {
                size_t j;
                uint32_t prev = UINT32_MAX;

                ++groups;
                for (j = start; j < i; ++j)
                {
                    if (edgeSort[j].Owner != prev)
                    {
                        ++ownerTotal;
                        prev = edgeSort[j].Owner;
                    }
                }
                start = i;
            }
        }

        if (groups > 0)
        {
            graph->Edges = calloc(groups, sizeof(*graph->Edges));
            graph->EdgeOwnerCount = calloc(groups, sizeof(*graph->EdgeOwnerCount));
            graph->EdgeOwners = calloc(groups, sizeof(*graph->EdgeOwners));
            if (ownerTotal > 0)
            {
                graph->EdgeOwnerFlat = malloc(ownerTotal * sizeof(*graph->EdgeOwnerFlat));
            }
            if (NULL == graph->Edges || NULL == graph->EdgeOwnerCount || NULL == graph->EdgeOwners ||
                (ownerTotal > 0 && NULL == graph->EdgeOwnerFlat))
            {
                goto fail;
            }
        }

        {
            size_t outIndex = 0;
            size_t ownerOffset = 0;
            size_t groupStart = 0;

            for (i = 1; i <= B->EdgeCount; ++i)
            {
                if (i == B->EdgeCount || !RmEdgeGroupKeyEqual(&edgeSort[i], &edgeSort[groupStart]))
                {
                    RM_EDGE *dst = &graph->Edges[outIndex];
                    size_t j;
                    uint32_t prev = UINT32_MAX;

                    dst->Kind = (RM_EDGE_KIND)edgeSort[groupStart].Kind;
                    dst->FromNode = edgeSort[groupStart].From;
                    dst->ToNode = edgeSort[groupStart].To;
                    dst->ToCs = graph->Nodes[dst->ToNode].Cs;
                    dst->ToIp = graph->Nodes[dst->ToNode].Ip;
                    dst->CallSiteNode = (UINT32_MAX == edgeSort[groupStart].Call) ? RM_INVALID_INDEX
                                                                                 : edgeSort[groupStart].Call;
                    dst->ReturnSiteNode = (UINT32_MAX == edgeSort[groupStart].Ret) ? RM_INVALID_INDEX
                                                                                 : edgeSort[groupStart].Ret;

                    graph->EdgeOwners[outIndex] = graph->EdgeOwnerFlat + ownerOffset;
                    for (j = groupStart; j < i; ++j)
                    {
                        if (edgeSort[j].Owner != prev)
                        {
                            graph->EdgeOwnerFlat[ownerOffset++] = edgeSort[j].Owner;
                            ++graph->EdgeOwnerCount[outIndex];
                            prev = edgeSort[j].Owner;
                        }
                    }

                    ++outIndex;
                    groupStart = i;
                }
            }

            graph->EdgeCount = outIndex;
        }
    }

    // Terminations.
    termSort = malloc((B->TermCount ? B->TermCount : 1) * sizeof(*termSort));
    if (NULL == termSort)
    {
        goto fail;
    }
    for (i = 0; i < B->TermCount; ++i)
    {
        termSort[i].Node = nodeRank[B->Terms[i].Node];
        termSort[i].Reason = B->Terms[i].Reason;
        termSort[i].Call = (RM_INVALID_U32 == B->Terms[i].CallSite) ? UINT32_MAX
                                                                    : nodeRank[B->Terms[i].CallSite];
    }
    qsort(termSort, B->TermCount, sizeof(*termSort), RmCompareTermSort);

    {
        size_t unique = 0;

        if (B->TermCount > 0)
        {
            graph->Terms = calloc(B->TermCount, sizeof(*graph->Terms));
            if (NULL == graph->Terms)
            {
                goto fail;
            }
        }

        for (i = 0; i < B->TermCount; ++i)
        {
            if (i > 0 && 0 == memcmp(&termSort[i], &termSort[i - 1], sizeof(*termSort)))
            {
                continue;
            }

            graph->Terms[unique].Reason = (RM_TERMINATION_REASON)termSort[i].Reason;
            graph->Terms[unique].Node = termSort[i].Node;
            graph->Terms[unique].Cs = graph->Nodes[termSort[i].Node].Cs;
            graph->Terms[unique].Ip = graph->Nodes[termSort[i].Node].Ip;
            graph->Terms[unique].CallSiteNode = (UINT32_MAX == termSort[i].Call) ? RM_INVALID_INDEX
                                                                                : termSort[i].Call;
            ++unique;
        }

        graph->TermCount = unique;
    }

    // Node ownership.
    nodeOwnerOcc = malloc((B->OwnerCount ? B->OwnerCount : 1) * sizeof(*nodeOwnerOcc));
    if (NULL == nodeOwnerOcc)
    {
        goto fail;
    }
    for (i = 0; i < B->OwnerCount; ++i)
    {
        nodeOwnerOcc[i].Subject = nodeRank[B->NodeOwners[i].Subject];
        nodeOwnerOcc[i].Owner = routineRank[B->NodeOwners[i].Owner];
    }
    if (!RmBuildOwnerLists(nodeOwnerOcc, B->OwnerCount, graph->NodeCount, &graph->NodeOwnerCount,
                           &graph->NodeOwnerFlat, &graph->NodeOwners))
    {
        goto fail;
    }

    graph->A20 = B->A20;

    free(nodeOrder);
    free(routineOrder);
    free(nodeRank);
    free(routineRank);
    free(edgeSort);
    free(termSort);
    free(nodeOwnerOcc);

    return graph;

fail:
    free(nodeOrder);
    free(routineOrder);
    free(nodeRank);
    free(routineRank);
    free(edgeSort);
    free(termSort);
    free(nodeOwnerOcc);
    RmFreeGraphInternal(graph);
    return NULL;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* Public API.                                                                                                        */
/* ------------------------------------------------------------------------------------------------------------------ */

RM_GRAPH *
RmBuildGraph(
    const RM_FRAGMENT *Fragments,
    size_t FragmentCount,
    int A20Enabled,
    const RM_ENTRY *Entries,
    size_t EntryCount
)
{
    RM_BUILDER B;
    RM_GRAPH *graph;
    uint32_t *entryKeys = NULL;
    size_t i;

    if (NULL == Fragments || 0 == FragmentCount || (NULL == Entries && EntryCount > 0))
    {
        return NULL;
    }

    memset(&B, 0, sizeof(B));
    B.A20 = A20Enabled ? 1 : 0;

    B.Fragments = RmPrepareFragments(Fragments, FragmentCount);
    if (NULL == B.Fragments)
    {
        return NULL;
    }
    B.FragmentCount = FragmentCount;

    // Register every caller-supplied entry as a top-level routine before computing anything else. Entries are
    // sorted so that duplicates are removed and creation order is canonical.
    if (EntryCount > 0)
    {
        entryKeys = malloc(EntryCount * sizeof(*entryKeys));
        if (NULL == entryKeys)
        {
            RmFreeBuilder(&B);
            return NULL;
        }

        for (i = 0; i < EntryCount; ++i)
        {
            entryKeys[i] = ((uint32_t)Entries[i].Cs << 16) | Entries[i].Ip;
        }
        qsort(entryKeys, EntryCount, sizeof(*entryKeys), RmCompareU32Asc);

        for (i = 0; i < EntryCount; ++i)
        {
            uint16_t cs = (uint16_t)(entryKeys[i] >> 16);
            uint16_t ip = (uint16_t)(entryKeys[i] & 0xFFFF);
            uint32_t routine;

            if (i > 0 && entryKeys[i] == entryKeys[i - 1])
            {
                continue;
            }

            routine = RmEnsureRoutine(&B, cs, ip);
            if (RM_INVALID_U32 == routine)
            {
                free(entryKeys);
                RmFreeBuilder(&B);
                return NULL;
            }
            B.Routines[routine].TopLevel = 1;
        }

        free(entryKeys);
    }

    // Least fixpoint over routine return summaries.
    RmComputeSummaries(&B);
    if (B.Oom)
    {
        RmFreeBuilder(&B);
        return NULL;
    }

    // Emission walk: produce edges, ownership and per-instruction terminations.
    for (i = 0; i < B.RoutineCount; ++i)
    {
        RmWalkRoutine(&B, (uint32_t)i, 1);
        if (B.Oom)
        {
            RmFreeBuilder(&B);
            return NULL;
        }
    }

    RmRecordUnresolvedReturns(&B);
    if (B.Oom)
    {
        RmFreeBuilder(&B);
        return NULL;
    }

    graph = RmFinalize(&B);
    RmFreeBuilder(&B);

    return graph;
}

void
RmFreeGraph(
    RM_GRAPH *Graph
)
{
    RmFreeGraphInternal(Graph);
}

int
RmA20Enabled(
    const RM_GRAPH *Graph
)
{
    return (NULL != Graph) ? Graph->A20 : 0;
}

size_t
RmRoutineCount(
    const RM_GRAPH *Graph
)
{
    return (NULL != Graph) ? Graph->RoutineCount : 0;
}

const RM_ROUTINE *
RmRoutineAt(
    const RM_GRAPH *Graph,
    size_t Index
)
{
    if (NULL == Graph || Index >= Graph->RoutineCount)
    {
        return NULL;
    }

    return &Graph->Routines[Index];
}

int
RmFindRoutine(
    const RM_GRAPH *Graph,
    uint16_t Cs,
    uint16_t Ip,
    uint32_t *RoutineIndex
)
{
    size_t lo;
    size_t hi;

    if (NULL == Graph)
    {
        return 0;
    }

    lo = 0;
    hi = Graph->RoutineCount;

    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        const RM_ROUTINE *r = &Graph->Routines[mid];

        if (Cs < r->Cs || (Cs == r->Cs && Ip < r->Ip))
        {
            hi = mid;
        }
        else if (Cs > r->Cs || (Cs == r->Cs && Ip > r->Ip))
        {
            lo = mid + 1;
        }
        else
        {
            if (NULL != RoutineIndex)
            {
                *RoutineIndex = (uint32_t)mid;
            }
            return 1;
        }
    }

    return 0;
}

size_t
RmNodeCount(
    const RM_GRAPH *Graph
)
{
    return (NULL != Graph) ? Graph->NodeCount : 0;
}

const RM_NODE *
RmNodeAt(
    const RM_GRAPH *Graph,
    size_t Index
)
{
    if (NULL == Graph || Index >= Graph->NodeCount)
    {
        return NULL;
    }

    return &Graph->Nodes[Index];
}

int
RmFindNode(
    const RM_GRAPH *Graph,
    uint16_t Cs,
    uint16_t Ip,
    uint32_t *NodeIndex
)
{
    size_t lo;
    size_t hi;

    if (NULL == Graph)
    {
        return 0;
    }

    lo = 0;
    hi = Graph->NodeCount;

    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        const RM_NODE *n = &Graph->Nodes[mid];

        if (Cs < n->Cs || (Cs == n->Cs && Ip < n->Ip))
        {
            hi = mid;
        }
        else if (Cs > n->Cs || (Cs == n->Cs && Ip > n->Ip))
        {
            lo = mid + 1;
        }
        else
        {
            if (NULL != NodeIndex)
            {
                *NodeIndex = (uint32_t)mid;
            }
            return 1;
        }
    }

    return 0;
}

size_t
RmNodeRoutineCount(
    const RM_GRAPH *Graph,
    uint32_t NodeIndex
)
{
    if (NULL == Graph || NodeIndex >= Graph->NodeCount)
    {
        return 0;
    }

    return Graph->NodeOwnerCount[NodeIndex];
}

uint32_t
RmNodeRoutineAt(
    const RM_GRAPH *Graph,
    uint32_t NodeIndex,
    size_t OwnerIndex
)
{
    if (NULL == Graph || NodeIndex >= Graph->NodeCount || OwnerIndex >= Graph->NodeOwnerCount[NodeIndex])
    {
        return RM_INVALID_INDEX;
    }

    return Graph->NodeOwners[NodeIndex][OwnerIndex];
}

size_t
RmEdgeCount(
    const RM_GRAPH *Graph
)
{
    return (NULL != Graph) ? Graph->EdgeCount : 0;
}

const RM_EDGE *
RmEdgeAt(
    const RM_GRAPH *Graph,
    size_t Index
)
{
    if (NULL == Graph || Index >= Graph->EdgeCount)
    {
        return NULL;
    }

    return &Graph->Edges[Index];
}

size_t
RmEdgeRoutineCount(
    const RM_GRAPH *Graph,
    size_t EdgeIndex
)
{
    if (NULL == Graph || EdgeIndex >= Graph->EdgeCount)
    {
        return 0;
    }

    return Graph->EdgeOwnerCount[EdgeIndex];
}

uint32_t
RmEdgeRoutineAt(
    const RM_GRAPH *Graph,
    size_t EdgeIndex,
    size_t OwnerIndex
)
{
    if (NULL == Graph || EdgeIndex >= Graph->EdgeCount || OwnerIndex >= Graph->EdgeOwnerCount[EdgeIndex])
    {
        return RM_INVALID_INDEX;
    }

    return Graph->EdgeOwners[EdgeIndex][OwnerIndex];
}

size_t
RmTerminationCount(
    const RM_GRAPH *Graph
)
{
    return (NULL != Graph) ? Graph->TermCount : 0;
}

const RM_TERMINATION_RECORD *
RmTerminationAt(
    const RM_GRAPH *Graph,
    size_t Index
)
{
    if (NULL == Graph || Index >= Graph->TermCount)
    {
        return NULL;
    }

    return &Graph->Terms[Index];
}
