/*
 * Structured regression for the 64-bit REP string instructions (MOVS, STOS,
 * LODS, CMPS, SCAS) against bdshemu.
 *
 * The test is built around two independent implementations fed by the same
 * "operation plan":
 *
 *   1. A tiny element-wise interpreter written entirely in this file.  It
 *      derives every expected memory access, register transition and
 *      arithmetic flag from the plan itself; it never looks at the decoded
 *      INSTRUX operand fields to figure out what should happen.
 *
 *   2. The production emulator (NdDecodeEx is used to validate/walk the
 *      generated code, ShemuEmulate runs it), driven by machine code that the
 *      very same plan is encoded into.
 *
 * The two traces (per-access address/width/direction/bytes, final registers,
 * flags, RCX and stop reason) are then compared event by event.
 *
 * Only 64-bit long mode, flat segments and 1/2/4/8 byte element widths are
 * covered; see regress_tests/README.md for the exact scope.
 */
#include "bdshemu.h"
#include "support/check.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Fixed virtual memory layout.  The three ranges never overlap.       */
/* ------------------------------------------------------------------ */
#define CODE_BASE       0x0000000000200000ULL
#define CODE_CAPACITY   0x2000ULL
#define STACK_BASE      0x0000000000100000ULL
#define STACK_SIZE      0x4000ULL
#define DATA_BASE       0x0000000000400000ULL
#define DATA_ELEMS      64u                 /* 64 eight-byte slots per region  */
#define REGION_A_OFF    0u
#define REGION_B_OFF    (DATA_ELEMS * 8u)
#define REGION_C_OFF    (DATA_ELEMS * 16u)
#define DATA_BYTES      (DATA_ELEMS * 24u)

#define MAX_PLAN_OPS    6u
#define MAX_EVENTS      256u
#define MAX_CODE_BYTES  256u
#define INSN_BUDGET     4096ULL
#define MAX_COUNT       8u
#define ACC_SENTINEL    0x5A5AULL           /* high bytes of the planned RAX load */

/* Arithmetic flags the independent model tracks.  AF is architecturally
 * defined for the compares but the production emulator never assigns it, so
 * it is excluded from the comparison (see README). */
#define ARITH_MASK      (NDR_RFLAG_CF | NDR_RFLAG_PF | NDR_RFLAG_ZF | \
                         NDR_RFLAG_SF | NDR_RFLAG_OF)
#define RFLAG_ONE       (1ULL << 1)

/* Seed every arithmetic flag as set so that flag-preserving instructions
 * (MOVS/STOS/LODS and RCX == 0 compares) are checked against real bits. */
#define INIT_RFLAGS     (RFLAG_ONE | NDR_RFLAG_CF | NDR_RFLAG_PF |          \
                         NDR_RFLAG_ZF | NDR_RFLAG_SF | NDR_RFLAG_OF)

enum
{
    OP_MOVS = 0,
    OP_STOS,
    OP_LODS,
    OP_CMPS,
    OP_SCAS,
    OP_COUNT
};

enum
{
    PFX_NONE = 0,
    PFX_REP,                                /* F3, valid only on MOVS/STOS/LODS */
    PFX_REPE,                               /* F3, valid only on CMPS/SCAS      */
    PFX_REPNE,                              /* F2, valid only on CMPS/SCAS      */
    PFX_COUNT
};

/* A single planned string operation.  Everything the reference interpreter
 * is allowed to know comes from these fields (plus the plan's data image). */
typedef struct _PLAN_OP
{
    uint8_t     Ins;                        /* OP_*                          */
    uint8_t     Prefix;                     /* PFX_*                         */
    uint8_t     Width;                      /* 1, 2, 4 or 8                  */
    uint8_t     Df;                         /* direction flag for this op    */
    uint8_t     SrcRegion;                  /* 0 = A, 1 = B, 2 = C           */
    uint8_t     DstRegion;
    uint8_t     SrcOffset;                  /* slot offset inside region     */
    uint8_t     DstOffset;
    uint8_t     SrcBias;                    /* 0..7 byte bias inside slot    */
    uint8_t     DstBias;
    uint8_t     Count;                      /* initial RCX, 0..MAX_COUNT     */
    uint8_t     Acc;                        /* planned AL value              */
    uint8_t     KeepAcc;                    /* skip the RAX reload (chains)  */
    uint8_t     InjectFail;                 /* callback rejects one read     */
    uint8_t     FailEvent;                  /* 1-based per-op event index    */
} PLAN_OP;

typedef struct _PLAN
{
    PLAN_OP     Ops[MAX_PLAN_OPS];
    unsigned    OpCount;
    uint8_t     Code[MAX_CODE_BYTES];
    unsigned    CodeLen;                    /* bytes of the string plan      */
    unsigned    ShellSize;                  /* bytes presented as shellcode  */
    unsigned    OpStart[MAX_PLAN_OPS];      /* code offsets of each op       */
    unsigned    OpEnd[MAX_PLAN_OPS];
    unsigned    EndKind;                    /* 0 = region end, 1 = UD2       */
    uint8_t     InitData[DATA_BYTES];       /* independent initial contents  */
    uint64_t    InitFlags;
    uint32_t    PlanId;
} PLAN;

/* One recorded data access, for both the reference and the emulator. */
typedef struct _MEM_EVENT
{
    uint64_t    Address;
    uint64_t    Value;                      /* bytes seen, little endian      */
    uint32_t    Width;
    uint8_t     Store;                      /* 0 = read, 1 = write            */
    uint8_t     PlanOp;                     /* owning plan operation          */
    uint8_t     Elem;                       /* element iteration within op    */
} MEM_EVENT;

/* Full outcome of running one plan through one implementation. */
typedef struct _RUN_RESULT
{
    MEM_EVENT   Events[MAX_EVENTS];
    unsigned    EventCount;
    uint64_t    Rax, Rcx, Rsi, Rdi, Flags, Rip;
    unsigned    Status;
    int         Rejected;
    int         RejectedPlanOp;
} RUN_RESULT;

/* ------------------------------------------------------------------ */
/* Deterministic small PRNG (xorshift32), fixed seed only.             */
/* ------------------------------------------------------------------ */
static uint32_t rng_state;

static uint32_t
rng_next(void)
{
    uint32_t x = rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static uint32_t
rng_below(uint32_t n)
{
    return n ? (rng_next() % n) : 0;
}

/* ------------------------------------------------------------------ */
/* Independent flag computation for a width-sized subtraction a - b.   */
/* Mirrors ShemuX86SetFlags(FM_SUB).  AF is computed as architecture    */
/* defines it, but the library never writes AF on string compares, so   */
/* callers compare only ARITH_MASK (without AF).                        */
/* ------------------------------------------------------------------ */
static uint64_t
sub_flags(uint64_t a, uint64_t b, unsigned width, uint64_t old_flags)
{
    static const uint8_t pf_arr[16] = {
        0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4
    };
    uint64_t mask = width == 8 ? UINT64_MAX : ((1ULL << (width * 8u)) - 1ULL);
    uint64_t res = (a - b) & mask;
    uint64_t flags = old_flags;
    uint64_t sign_bit = 1ULL << (width * 8u - 1u);
    int sa = (a & sign_bit) != 0;
    int sb = (b & sign_bit) != 0;
    int sr = (res & sign_bit) != 0;

    if ((pf_arr[res & 0xF] + pf_arr[(res >> 4) & 0xF]) % 2 == 0)
    {
        flags |= NDR_RFLAG_PF;
    }
    else
    {
        flags &= ~NDR_RFLAG_PF;
    }

    flags = res == 0 ? (flags | NDR_RFLAG_ZF) : (flags & ~NDR_RFLAG_ZF);
    flags = sr ? (flags | NDR_RFLAG_SF) : (flags & ~NDR_RFLAG_SF);
    flags = a < b ? (flags | NDR_RFLAG_CF) : (flags & ~NDR_RFLAG_CF);

    if ((sa && !sb && !sr) || (!sa && sb && sr))
    {
        flags |= NDR_RFLAG_OF;
    }
    else
    {
        flags &= ~NDR_RFLAG_OF;
    }

    /* architecturally defined AF; not produced by the current library */
    if (((a ^ b ^ res) & 0x10) != 0)
    {
        flags |= NDR_RFLAG_AF;
    }
    else
    {
        flags &= ~NDR_RFLAG_AF;
    }

    return flags;
}

static uint64_t
load_le(const uint8_t *p, unsigned width)
{
    uint64_t v = 0;

    for (unsigned i = 0; i < width; i++)
    {
        v |= (uint64_t)p[i] << (i * 8u);
    }
    return v;
}

static void
store_le(uint8_t *p, unsigned width, uint64_t v)
{
    for (unsigned i = 0; i < width; i++)
    {
        p[i] = (uint8_t)(v >> (i * 8u));
    }
}

static uint64_t
trim_width(uint64_t v, unsigned width)
{
    if (width == 8)
    {
        return v;
    }
    return v & ((1ULL << (width * 8u)) - 1ULL);
}

static uint64_t
region_base(unsigned region)
{
    static const unsigned off[3] = { REGION_A_OFF, REGION_B_OFF, REGION_C_OFF };

    return DATA_BASE + off[region];
}

/* ------------------------------------------------------------------ */
/* The independent element-wise interpreter.                           */
/*                                                                      */
/* mem is the reference's OWN mutable byte image.  When reject is set,  */
/* the modeled callback refuses the FailEvent-th read of the owning op, */
/* and interpretation stops there exactly like the emulator does.       */
/* ------------------------------------------------------------------ */
static void
ref_run(const PLAN *plan, uint8_t *mem, RUN_RESULT *r)
{
    uint64_t rax = 0, rcx = 0, rsi = 0, rdi = 0;
    uint64_t flags = plan->InitFlags;
    unsigned rejected_op = UINT32_MAX;

    memset(r, 0, sizeof(*r));
    r->Status = SHEMU_SUCCESS;
    r->RejectedPlanOp = -1;

    for (unsigned oi = 0; oi < plan->OpCount; oi++)
    {
        const PLAN_OP *op = &plan->Ops[oi];
        unsigned w = op->Width;
        int64_t step = op->Df ? -(int64_t)w : (int64_t)w;
        uint64_t count = op->Count;
        uint64_t src_addr, dst_addr;
        unsigned op_events = 0;
        int aborted = 0;

        flags = op->Df ? (flags | NDR_RFLAG_DF) : (flags & ~NDR_RFLAG_DF);

        if (!op->KeepAcc)
        {
            rax = (ACC_SENTINEL << 8) | op->Acc;
        }

        rcx = count;
        src_addr = region_base(op->SrcRegion) + (uint64_t)op->SrcOffset * 8u +
                   op->SrcBias;
        dst_addr = region_base(op->DstRegion) + (uint64_t)op->DstOffset * 8u +
                   op->DstBias;
        rsi = src_addr;
        rdi = dst_addr;

        /* RCX == 0 on a repeated op: no data access, everything frozen. */
        if (op->Prefix != PFX_NONE && count == 0)
        {
            continue;
        }

        unsigned iters = op->Prefix == PFX_NONE ? 1u : (unsigned)count;

        for (unsigned it = 0; it < iters; it++)
        {
            uint64_t a, b, val;
            MEM_EVENT ev;

            memset(&ev, 0, sizeof(ev));
            ev.PlanOp = (uint8_t)oi;
            ev.Elem = (uint8_t)it;
            ev.Width = w;

            switch (op->Ins)
            {
            case OP_MOVS:
                /* explicit read-before-write serial order, element by
                 * element; no memcpy/memmove style shortcut. */
                ev.Address = rsi;
                ev.Store = 0;
                op_events++;
                if (op->InjectFail && op_events == op->FailEvent)
                {
                    r->Status = SHEMU_ABORT_GLA_OUTSIDE;
                    r->Rejected = 1;
                    r->RejectedPlanOp = (int)oi;
                    rejected_op = oi;
                    aborted = 1;
                    break;
                }
                ev.Value = load_le(mem + (rsi - DATA_BASE), w);
                if (r->EventCount < MAX_EVENTS)
                {
                    r->Events[r->EventCount++] = ev;
                }
                val = ev.Value;

                memset(&ev, 0, sizeof(ev));
                ev.PlanOp = (uint8_t)oi;
                ev.Elem = (uint8_t)it;
                ev.Width = w;
                ev.Address = rdi;
                ev.Store = 1;
                ev.Value = val;
                op_events++;
                store_le(mem + (rdi - DATA_BASE), w, val);
                if (r->EventCount < MAX_EVENTS)
                {
                    r->Events[r->EventCount++] = ev;
                }

                rsi = (uint64_t)((int64_t)rsi + step);
                rdi = (uint64_t)((int64_t)rdi + step);
                break;

            case OP_STOS:
                val = trim_width(rax, w);
                ev.Address = rdi;
                ev.Store = 1;
                ev.Value = val;
                op_events++;
                store_le(mem + (rdi - DATA_BASE), w, val);
                if (r->EventCount < MAX_EVENTS)
                {
                    r->Events[r->EventCount++] = ev;
                }
                rdi = (uint64_t)((int64_t)rdi + step);
                break;

            case OP_LODS:
                ev.Address = rsi;
                ev.Store = 0;
                op_events++;
                if (op->InjectFail && op_events == op->FailEvent)
                {
                    r->Status = SHEMU_ABORT_GLA_OUTSIDE;
                    r->Rejected = 1;
                    r->RejectedPlanOp = (int)oi;
                    rejected_op = oi;
                    aborted = 1;
                    break;
                }
                ev.Value = load_le(mem + (rsi - DATA_BASE), w);
                if (r->EventCount < MAX_EVENTS)
                {
                    r->Events[r->EventCount++] = ev;
                }
                /* sub-register accumulator write, library semantics */
                if (w == 4)
                {
                    rax = ev.Value;
                }
                else
                {
                    uint64_t mm = w == 8 ? UINT64_MAX :
                                  ((1ULL << (w * 8u)) - 1ULL);
                    rax = (rax & ~mm) | ev.Value;
                }
                rsi = (uint64_t)((int64_t)rsi + step);
                break;

            case OP_CMPS:
                ev.Address = rsi;
                ev.Store = 0;
                op_events++;
                if (op->InjectFail && op_events == op->FailEvent)
                {
                    r->Status = SHEMU_ABORT_GLA_OUTSIDE;
                    r->Rejected = 1;
                    r->RejectedPlanOp = (int)oi;
                    rejected_op = oi;
                    aborted = 1;
                    break;
                }
                a = load_le(mem + (rsi - DATA_BASE), w);
                ev.Value = a;
                if (r->EventCount < MAX_EVENTS)
                {
                    r->Events[r->EventCount++] = ev;
                }

                memset(&ev, 0, sizeof(ev));
                ev.PlanOp = (uint8_t)oi;
                ev.Elem = (uint8_t)it;
                ev.Width = w;
                ev.Address = rdi;
                ev.Store = 0;
                op_events++;
                if (op->InjectFail && op_events == op->FailEvent)
                {
                    /* first read already advanced RSI in the library; the
                     * reference records the same partial state. */
                    rsi = (uint64_t)((int64_t)rsi + step);
                    r->Status = SHEMU_ABORT_GLA_OUTSIDE;
                    r->Rejected = 1;
                    r->RejectedPlanOp = (int)oi;
                    rejected_op = oi;
                    aborted = 1;
                    break;
                }
                b = load_le(mem + (rdi - DATA_BASE), w);
                ev.Value = b;
                if (r->EventCount < MAX_EVENTS)
                {
                    r->Events[r->EventCount++] = ev;
                }

                flags = sub_flags(a, b, w, flags);
                rsi = (uint64_t)((int64_t)rsi + step);
                rdi = (uint64_t)((int64_t)rdi + step);
                break;

            case OP_SCAS:
                a = trim_width(rax, w);
                ev.Address = rdi;
                ev.Store = 0;
                op_events++;
                if (op->InjectFail && op_events == op->FailEvent)
                {
                    r->Status = SHEMU_ABORT_GLA_OUTSIDE;
                    r->Rejected = 1;
                    r->RejectedPlanOp = (int)oi;
                    rejected_op = oi;
                    aborted = 1;
                    break;
                }
                b = load_le(mem + (rdi - DATA_BASE), w);
                ev.Value = b;
                if (r->EventCount < MAX_EVENTS)
                {
                    r->Events[r->EventCount++] = ev;
                }

                flags = sub_flags(a, b, w, flags);
                rdi = (uint64_t)((int64_t)rdi + step);
                break;

            default:
                break;
            }

            if (aborted)
            {
                goto done;
            }

            if (op->Prefix != PFX_NONE)
            {
                rcx--;                         /* the element is consumed */

                if (op->Prefix == PFX_REPE && (flags & NDR_RFLAG_ZF) == 0)
                {
                    break;
                }
                if (op->Prefix == PFX_REPNE && (flags & NDR_RFLAG_ZF) != 0)
                {
                    break;
                }
            }
        }
    }

done:
    r->Rax = rax;
    r->Rcx = rcx;
    r->Rsi = rsi;
    r->Rdi = rdi;
    r->Flags = flags;
    if (r->Rejected)
    {
        /* RIP is advanced before the instruction emulates, even on failure. */
        r->Rip = CODE_BASE + plan->OpEnd[rejected_op];
    }
    else if (plan->EndKind == 1)
    {
        r->Rip = CODE_BASE + plan->CodeLen + 2;   /* past UD2 */
        r->Status = SHEMU_ABORT_CANT_EMULATE;
    }
    else
    {
        r->Rip = CODE_BASE + plan->CodeLen;       /* one past the code region */
        r->Status = SHEMU_ABORT_RIP_OUTSIDE;
    }
}

/* ------------------------------------------------------------------ */
/* Machine code generation from a plan (long mode only).               */
/* ------------------------------------------------------------------ */
static unsigned
emit_mov_imm64(uint8_t *p, unsigned reg, uint64_t imm)
{
    p[0] = 0x48 | ((reg & 8) ? 0x01 : 0x00);      /* REX.W (+B) */
    p[1] = (uint8_t)(0xB8 + (reg & 7));
    for (unsigned i = 0; i < 8; i++)
    {
        p[2 + i] = (uint8_t)(imm >> (i * 8u));
    }
    return 10;
}

static unsigned
emit_mov_ecx(uint8_t *p, uint32_t imm)
{
    p[0] = 0xB9;
    p[1] = (uint8_t)imm;
    p[2] = (uint8_t)(imm >> 8);
    p[3] = (uint8_t)(imm >> 16);
    p[4] = (uint8_t)(imm >> 24);
    return 5;
}

static unsigned
emit_string_op(uint8_t *p, const PLAN_OP *op)
{
    unsigned n = 0;
    /* byte opcode vs. word/dword/qword opcode for each string instruction */
    static const uint8_t opcode_byte[OP_COUNT] = {
        0xA4,     /* MOVSB */
        0xAA,     /* STOSB */
        0xAC,     /* LODSB */
        0xA6,     /* CMPSB */
        0xAE      /* SCASB */
    };
    static const uint8_t opcode_wide[OP_COUNT] = {
        0xA5,     /* MOVSW/D/Q */
        0xAB,     /* STOSW/D/Q */
        0xAD,     /* LODSW/D/Q */
        0xA7,     /* CMPSW/D/Q */
        0xAF      /* SCASW/D/Q */
    };

    if (op->Prefix == PFX_REP || op->Prefix == PFX_REPE)
    {
        p[n++] = 0xF3;
    }
    else if (op->Prefix == PFX_REPNE)
    {
        p[n++] = 0xF2;
    }

    if (op->Width == 2)
    {
        p[n++] = 0x66;
    }
    else if (op->Width == 8)
    {
        p[n++] = 0x48;
    }

    p[n++] = op->Width == 1 ? opcode_byte[op->Ins] : opcode_wide[op->Ins];
    return n;
}

static void
plan_encode(PLAN *plan)
{
    unsigned n = 0;

    for (unsigned oi = 0; oi < plan->OpCount; oi++)
    {
        const PLAN_OP *op = &plan->Ops[oi];

        plan->OpStart[oi] = n;
        n += emit_mov_imm64(plan->Code + n, 6,
                            region_base(op->SrcRegion) +
                            (uint64_t)op->SrcOffset * 8u + op->SrcBias);
        n += emit_mov_imm64(plan->Code + n, 7,
                            region_base(op->DstRegion) +
                            (uint64_t)op->DstOffset * 8u + op->DstBias);
        n += emit_mov_ecx(plan->Code + n, op->Count);
        if (!op->KeepAcc)
        {
            n += emit_mov_imm64(plan->Code + n, 0,
                                (0x5A5AULL << 8) | op->Acc);
        }
        plan->Code[n++] = op->Df ? 0xFD : 0xFC;  /* STD / CLD */
        n += emit_string_op(plan->Code + n, op);
        plan->OpEnd[oi] = n;
    }

    plan->CodeLen = n;
    plan->ShellSize = n + (plan->EndKind == 1 ? 2u : 0u);
    if (plan->EndKind == 1)
    {
        plan->Code[n] = 0x0F;                    /* UD2 */
        plan->Code[n + 1] = 0x0B;
    }
}

/* ------------------------------------------------------------------ */
/* Emulator-side memory: one callback-backed byte array.               */
/* ------------------------------------------------------------------ */
typedef struct _EXT_MEM
{
    const PLAN  *Plan;
    uint8_t     Bytes[DATA_BYTES];
    MEM_EVENT   Events[MAX_EVENTS];
    unsigned    EventCount;
    unsigned    OpEventCount;
    int         CurrentOp;
    int         Rejected;
    int         RejectOp;
} EXT_MEM;

static int
owner_for_rip(const PLAN *plan, uint64_t rip)
{
    unsigned off = (unsigned)(rip - CODE_BASE);

    for (unsigned oi = 0; oi < plan->OpCount; oi++)
    {
        if (off <= plan->OpEnd[oi])
        {
            return (int)oi;
        }
    }
    return (int)plan->OpCount - 1;
}

static ND_BOOL
access_callback(void *ctx, uint64_t gla, ND_SIZET size, uint8_t *buffer,
                ND_BOOL store)
{
    SHEMU_CONTEXT *sc = (SHEMU_CONTEXT *)ctx;
    EXT_MEM *ext = (EXT_MEM *)sc->AuxData;
    const PLAN *plan = ext->Plan;
    MEM_EVENT ev;
    int owner;

    if (gla < DATA_BASE || gla + size > DATA_BASE + DATA_BYTES)
    {
        return ND_FALSE;                     /* outside the declared range */
    }

    owner = owner_for_rip(plan, sc->Arch.X86.Registers.RegRip);
    if (owner != ext->CurrentOp)
    {
        ext->CurrentOp = owner;
        ext->OpEventCount = 0;
    }

    uint64_t off = gla - DATA_BASE;

    memset(&ev, 0, sizeof(ev));
    ev.Address = gla;
    ev.Width = (uint32_t)size;
    ev.Store = store ? 1 : 0;
    ev.PlanOp = (uint8_t)owner;

    if (!store)
    {
        ext->OpEventCount++;
        const PLAN_OP *pop = &plan->Ops[owner];
        if (pop->InjectFail && ext->OpEventCount == pop->FailEvent)
        {
            ext->Rejected = 1;
            ext->RejectOp = owner;
            return ND_FALSE;                 /* GLA_OUTSIDE, nothing committed */
        }
        ev.Value = load_le(ext->Bytes + off, (unsigned)size);
        memcpy(buffer, ext->Bytes + off, size);
    }
    else
    {
        ext->OpEventCount++;
        ev.Value = load_le(buffer, (unsigned)size);
        /* the write is immediately visible to later reads */
        memcpy(ext->Bytes + off, buffer, size);
    }

    /* element index within the owning op */
    {
        const PLAN_OP *pop = &plan->Ops[owner];
        unsigned per_elem = (pop->Ins == OP_MOVS || pop->Ins == OP_CMPS) ? 2u : 1u;
        ev.Elem = (uint8_t)((ext->OpEventCount - 1) / per_elem);
    }

    if (ext->EventCount < MAX_EVENTS)
    {
        ext->Events[ext->EventCount] = ev;
    }
    ext->EventCount++;
    return ND_TRUE;
}

static void
emu_run(const PLAN *plan, const uint8_t *init_mem, uint8_t *out_mem,
        RUN_RESULT *r)
{
    uint8_t shellcode[MAX_CODE_BYTES];
    uint8_t stack[STACK_SIZE];
    uint8_t intbuf[CODE_CAPACITY + STACK_SIZE];
    SHEMU_CONTEXT ctx;
    EXT_MEM ext;

    memset(&ctx, 0, sizeof(ctx));
    memset(&ext, 0, sizeof(ext));
    memset(r, 0, sizeof(*r));
    r->RejectedPlanOp = -1;
    ext.Plan = plan;
    ext.CurrentOp = -1;
    memcpy(ext.Bytes, init_mem, DATA_BYTES);
    memcpy(shellcode, plan->Code, plan->ShellSize);

    ctx.ArchType = SHEMU_ARCH_TYPE_X86;
    ctx.Arch.X86.Mode = ND_CODE_64;
    ctx.Shellcode = shellcode;
    ctx.ShellcodeSize = plan->ShellSize;
    ctx.ShellcodeBase = CODE_BASE;
    ctx.Stack = stack;
    ctx.StackSize = sizeof(stack);
    ctx.StackBase = STACK_BASE;
    ctx.Intbuf = intbuf;
    ctx.IntbufSize = sizeof(intbuf);
    ctx.MaxInstructionsCount = INSN_BUDGET;   /* generous: never the stop cause */
    ctx.MemThreshold = 0xFFFFFFFFu;
    ctx.AccessMemory = access_callback;
    ctx.AuxData = &ext;
    ctx.Arch.X86.Registers.RegRip = CODE_BASE;
    ctx.Arch.X86.Registers.RegRsp = STACK_BASE + sizeof(stack) / 2;
    ctx.Arch.X86.Registers.RegFlags = plan->InitFlags;

    r->Status = ShemuEmulate(&ctx);

    r->EventCount = ext.EventCount > MAX_EVENTS ? MAX_EVENTS : ext.EventCount;
    memcpy(r->Events, ext.Events, r->EventCount * sizeof(MEM_EVENT));
    r->Rax = ctx.Arch.X86.Registers.RegRax;
    r->Rcx = ctx.Arch.X86.Registers.RegRcx;
    r->Rsi = ctx.Arch.X86.Registers.RegRsi;
    r->Rdi = ctx.Arch.X86.Registers.RegRdi;
    r->Flags = ctx.Arch.X86.Registers.RegFlags;
    r->Rip = ctx.Arch.X86.Registers.RegRip;
    r->Rejected = ext.Rejected;
    if (ext.Rejected)
    {
        r->RejectedPlanOp = ext.RejectOp;
    }
    if (out_mem)
    {
        memcpy(out_mem, ext.Bytes, DATA_BYTES);
    }
}

/* ------------------------------------------------------------------ */
/* NdDecodeEx walk of the generated code: every byte sequence must     */
/* decode, and the walk must end exactly at the string-plan boundary.  */
/* The reference model does not consume any of this information.       */
/* ------------------------------------------------------------------ */
static int
code_decodes_cleanly(const PLAN *plan)
{
    unsigned off = 0;

    while (off < plan->CodeLen)
    {
        INSTRUX ix;
        NDSTATUS st = NdDecodeEx(&ix, plan->Code + off,
                                 plan->ShellSize - off, ND_CODE_64, ND_DATA_64);
        if (!ND_SUCCESS(st) || ix.Length == 0)
        {
            return 0;
        }
        off += ix.Length;
    }

    return off == plan->CodeLen;
}

/* ------------------------------------------------------------------ */
/* Plan generation: deterministic enumeration with a fixed seed.       */
/* ------------------------------------------------------------------ */
static int
prefix_legal(unsigned ins, unsigned prefix)
{
    if (prefix == PFX_NONE)
    {
        return 1;
    }
    if (ins == OP_MOVS || ins == OP_STOS || ins == OP_LODS)
    {
        return prefix == PFX_REP;
    }
    /* CMPS / SCAS */
    return prefix == PFX_REPE || prefix == PFX_REPNE;
}

/* Scenario descriptions.  Each maps to one or more generated plans. */
enum
{
    SCN_MOVS_ALIAS = 0,      /* two MOVS over the same region, forward/backward */
    SCN_STOS_CMPS,           /* STOS accumulator feeds a CMPS                  */
    SCN_LODS_SCAS,           /* LODS accumulator feeds a SCAS                  */
    SCN_CMPS_STOP,           /* first/last/all-equal REPE/REPNE coverage       */
    SCN_MIXED,               /* five different string ops, shared regions      */
    SCN_ZEROCOUNT,           /* RCX == 0 freeze checks                         */
    SCN_CB_REJECT,           /* callback rejects one read                      */
    SCN_COUNT
};

static void
gen_plan(PLAN *plan, uint32_t id, unsigned scenario)
{
    memset(plan, 0, sizeof(*plan));
    plan->PlanId = id;
    plan->InitFlags = INIT_RFLAGS;                        /* reserved bit 1 only */
    rng_state = 0x12345678u ^ (id * 2654435761u);
    plan->EndKind = (id & 1u);                  /* alternate the two end kinds */

    /* Independent initial data image (never derived from decoded fields). */
    for (unsigned i = 0; i < DATA_BYTES; i++)
    {
        plan->InitData[i] = (uint8_t)(rng_next() & 0xFFu);
    }

    PLAN_OP *o;

    switch (scenario)
    {
    case SCN_MOVS_ALIAS:
    {
        plan->OpCount = 3;
        unsigned df = id & 1u;
        /* MOVS with identical source/destination region and slot. */
        o = &plan->Ops[0];
        o->Ins = OP_MOVS; o->Prefix = PFX_REP; o->Width = (uint8_t)(1u << (id % 4));
        o->Df = (uint8_t)df; o->SrcRegion = 0; o->DstRegion = 0;
        o->SrcOffset = 4; o->DstOffset = 4; o->SrcBias = 0; o->DstBias = 0;
        o->Count = 4;
        /* MOVS whose source and destination differ by exactly one element. */
        o = &plan->Ops[1];
        o->Ins = OP_MOVS; o->Prefix = PFX_REP; o->Width = plan->Ops[0].Width;
        o->Df = (uint8_t)(df ^ 1u); o->SrcRegion = 1; o->DstRegion = 1;
        o->SrcOffset = 8; o->DstOffset = 9; o->SrcBias = 0; o->DstBias = 0;
        o->Count = 3;
        /* a non-repeated MOVS touching data the first two already changed. */
        o = &plan->Ops[2];
        o->Ins = OP_MOVS; o->Prefix = PFX_NONE; o->Width = plan->Ops[0].Width;
        o->Df = 0; o->SrcRegion = 1; o->DstRegion = 2;
        o->SrcOffset = 9; o->DstOffset = 2; o->Count = 1;
        break;
    }

    case SCN_STOS_CMPS:
    {
        plan->OpCount = 3;
        unsigned w = (uint8_t)(1u << (id % 4));
        /* STOS writes a known pattern into region B. */
        o = &plan->Ops[0];
        o->Ins = OP_STOS; o->Prefix = PFX_REP; o->Width = w;
        o->Df = 0; o->DstRegion = 1; o->DstOffset = 6; o->DstBias = 0;
        o->Count = 5; o->Acc = (uint8_t)(0x40u + (id % 8));
        /* CMPS compares region A against the freshly stored region B. */
        o = &plan->Ops[1];
        o->Ins = OP_CMPS; o->Prefix = PFX_REPE; o->Width = w;
        o->Df = 0; o->SrcRegion = 0; o->DstRegion = 1;
        o->SrcOffset = 6; o->DstOffset = 6; o->Count = 5;
        /* a second CMPS with REPNE on the same data. */
        o = &plan->Ops[2];
        o->Ins = OP_CMPS; o->Prefix = PFX_REPNE; o->Width = w;
        o->Df = (uint8_t)(id & 1u); o->SrcRegion = 0; o->DstRegion = 1;
        o->SrcOffset = 6; o->DstOffset = 6; o->Count = 5;
        break;
    }

    case SCN_LODS_SCAS:
    {
        plan->OpCount = 3;
        unsigned w = (uint8_t)(1u << (id % 4));
        /* LODS loads the accumulator from region A. */
        o = &plan->Ops[0];
        o->Ins = OP_LODS; o->Prefix = PFX_REP; o->Width = w;
        o->Df = 0; o->SrcRegion = 0; o->SrcOffset = 10; o->SrcBias = 0;
        o->Count = 2; o->KeepAcc = 1;
        /* SCAS searches region B using that accumulator. */
        o = &plan->Ops[1];
        o->Ins = OP_SCAS; o->Prefix = PFX_REPNE; o->Width = w;
        o->Df = 0; o->DstRegion = 1; o->DstOffset = 10; o->DstBias = 0;
        o->Count = 4; o->KeepAcc = 1;
        /* REPE SCAS over the same area. */
        o = &plan->Ops[2];
        o->Ins = OP_SCAS; o->Prefix = PFX_REPE; o->Width = w;
        o->Df = (uint8_t)(id & 1u); o->DstRegion = 1; o->DstOffset = 10;
        o->Count = 4; o->KeepAcc = 1;
        break;
    }

    case SCN_CMPS_STOP:
    {
        plan->OpCount = 4;
        unsigned w = (uint8_t)(1u << (id % 4));
        /* Craft data so the mismatch position is chosen by the id. */
        unsigned mismatch = id % 5u;             /* 0 = first, 4 = last */
        for (unsigned k = 0; k < 5; k++)
        {
            uint64_t va = 0x1122334455667700ULL + k;
            uint64_t vb = (k == mismatch) ? (va ^ 0xFFu) : va;
            unsigned slot = 16 + k;
            for (unsigned b = 0; b < 8; b++)
            {
                plan->InitData[REGION_B_OFF + slot * 8u + b] =
                    (uint8_t)(va >> (b * 8u));
                plan->InitData[REGION_A_OFF + slot * 8u + b] =
                    (uint8_t)(vb >> (b * 8u));
            }
        }
        /* REPE CMPS: stops on the first mismatch. */
        o = &plan->Ops[0];
        o->Ins = OP_CMPS; o->Prefix = PFX_REPE; o->Width = w;
        o->SrcRegion = 1; o->DstRegion = 0;
        o->SrcOffset = 16; o->DstOffset = 16; o->Count = 5;
        /* REPE CMPS on data that is all equal: runs to the last element. */
        o = &plan->Ops[1];
        o->Ins = OP_CMPS; o->Prefix = PFX_REPE; o->Width = w;
        o->SrcRegion = 1; o->DstRegion = 1;
        o->SrcOffset = 24; o->DstOffset = 24; o->Count = 4;
        /* REPNE CMPS where the equal element appears last. */
        o = &plan->Ops[2];
        o->Ins = OP_CMPS; o->Prefix = PFX_REPNE; o->Width = w;
        o->Df = (uint8_t)(id & 1u);
        o->SrcRegion = 1; o->DstRegion = 0;
        o->SrcOffset = 16; o->DstOffset = 16; o->Count = 5;
        /* non-repeated CMPS: flags come only from this one element, leaving
         * prior ZF untouched otherwise. */
        o = &plan->Ops[3];
        o->Ins = OP_CMPS; o->Prefix = PFX_NONE; o->Width = w;
        o->SrcRegion = 1; o->DstRegion = 0;
        o->SrcOffset = 16; o->DstOffset = 16; o->Count = 1;
        break;
    }

    case SCN_MIXED:
    {
        plan->OpCount = 5;
        for (unsigned k = 0; k < plan->OpCount; k++)
        {
            o = &plan->Ops[k];
            o->Ins = (uint8_t)((OP_MOVS + k + (int)(id % 2)) % OP_COUNT);
            o->Width = (uint8_t)(1u << rng_below(4));
            o->Df = (uint8_t)rng_below(2);
            o->Prefix = (uint8_t)rng_below(PFX_COUNT);
            if (!prefix_legal(o->Ins, o->Prefix))
            {
                o->Prefix = PFX_NONE;
            }
            o->SrcRegion = (uint8_t)rng_below(3);
            o->DstRegion = (uint8_t)rng_below(3);
            o->SrcOffset = (uint8_t)rng_below(12);
            o->DstOffset = (uint8_t)rng_below(12);
            o->Count = (uint8_t)rng_below(MAX_COUNT + 1);
            o->Acc = (uint8_t)rng_below(256);
            o->KeepAcc = (uint8_t)(k != 0 ? rng_below(2) : 0);
        }
        break;
    }

    case SCN_ZEROCOUNT:
    {
        plan->OpCount = 4;
        o = &plan->Ops[0];
        o->Ins = OP_MOVS; o->Prefix = PFX_REP; o->Width = 4;
        o->SrcRegion = 0; o->DstRegion = 1; o->SrcOffset = 3; o->DstOffset = 3;
        o->Count = 0;
        o = &plan->Ops[1];
        o->Ins = OP_STOS; o->Prefix = PFX_REP; o->Width = 2;
        o->DstRegion = 2; o->DstOffset = 5; o->Count = 0; o->Acc = 0x77;
        o = &plan->Ops[2];
        o->Ins = OP_SCAS; o->Prefix = PFX_REPE; o->Width = 8;
        o->DstRegion = 1; o->DstOffset = 7; o->Count = 0; o->KeepAcc = 1;
        o = &plan->Ops[3];
        o->Ins = OP_CMPS; o->Prefix = PFX_REPNE; o->Width = 1;
        o->SrcRegion = 0; o->DstRegion = 2;
        o->SrcOffset = 9; o->DstOffset = 9; o->Count = 0;
        break;
    }

    case SCN_CB_REJECT:
    {
        plan->OpCount = 2;
        unsigned w = (uint8_t)(1u << (id % 4));
        /* a normal op commits an access prefix first. */
        o = &plan->Ops[0];
        o->Ins = OP_STOS; o->Prefix = PFX_REP; o->Width = w;
        o->DstRegion = 1; o->DstOffset = 4; o->Count = 3; o->Acc = 0x33;
        /* the second op has its Nth read rejected by the callback. */
        o = &plan->Ops[1];
        o->Ins = (id & 1u) ? OP_CMPS : OP_SCAS;
        o->Prefix = PFX_REPNE; o->Width = w;
        o->SrcRegion = 0; o->DstRegion = 1;
        o->SrcOffset = 4; o->DstOffset = 4; o->Count = 4; o->KeepAcc = 1;
        o->InjectFail = 1;
        o->FailEvent = (uint8_t)(1u + (id % 3u));
        break;
    }

    default:
        break;
    }

    /* Keep every generated element access inside the declared data range, in
     * both directions, so the bounded callback never spuriously rejects a plan
     * that is meant to run to completion. */
    for (unsigned k = 0; k < plan->OpCount; k++)
    {
        PLAN_OP *p = &plan->Ops[k];
        if (p->Prefix == PFX_NONE || p->Count == 0)
        {
            continue;
        }

        unsigned regions[2];
        unsigned offsets[2];
        unsigned nr = 0;

        if (p->Ins == OP_MOVS || p->Ins == OP_LODS || p->Ins == OP_CMPS)
        {
            regions[nr] = p->SrcRegion;
            offsets[nr] = p->SrcOffset * 8u + p->SrcBias;
            nr++;
        }
        if (p->Ins == OP_MOVS || p->Ins == OP_STOS ||
            p->Ins == OP_CMPS || p->Ins == OP_SCAS)
        {
            regions[nr] = p->DstRegion;
            offsets[nr] = p->DstOffset * 8u + p->DstBias;
            nr++;
        }

        unsigned max_count = p->Count;
        for (unsigned j = 0; j < nr; j++)
        {
            unsigned start = offsets[j];
            unsigned cap;

            if (p->Df)
            {
                /* backward: start - (n-1)*width >= 0 */
                cap = start / p->Width + 1u;
            }
            else
            {
                /* forward: start + n*width <= region size */
                cap = (DATA_ELEMS * 8u - start) / p->Width;
            }
            if (cap < max_count)
            {
                max_count = cap;
            }
        }
        p->Count = (uint8_t)max_count;
    }

    plan_encode(plan);
}

/* ------------------------------------------------------------------ */
/* Trace comparison and dual-context replay.                           */
/* ------------------------------------------------------------------ */
typedef struct _CHECK_LOG
{
    char    FirstDiff[256];
    int     DiffOp;
    int     DiffEvent;
    int     Failed;
    int     PlansRun;
} CHECK_LOG;

static CHECK_LOG g_log;

/* One final, greppable summary line: number of distinct plans executed, the
 * first differing event and the owning operation number. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor))
#endif
static void
print_test_summary(void)
{
    printf("SUMMARY plans_executed=%d first_diff='%s' op=%d event=%d\n",
           g_log.PlansRun,
           g_log.FirstDiff[0] != '\0' ? g_log.FirstDiff : "none",
           g_log.DiffOp, g_log.DiffEvent);
}

static int
flags_agree(uint64_t a, uint64_t b)
{
    return (a & ARITH_MASK) == (b & ARITH_MASK) &&
           ((a & NDR_RFLAG_DF) == (b & NDR_RFLAG_DF));
}

static int
results_agree(const PLAN *plan, const RUN_RESULT *ref, const RUN_RESULT *emu,
              const uint8_t *ref_mem, const uint8_t *emu_mem)
{
    g_log.DiffOp = -1;
    g_log.DiffEvent = -1;

    if (ref->Status != emu->Status)
    {
        snprintf(g_log.FirstDiff, sizeof(g_log.FirstDiff),
                 "status ref=%u emu=%u", ref->Status, emu->Status);
        return 0;
    }

    unsigned n = ref->EventCount < emu->EventCount ? ref->EventCount
                                                   : emu->EventCount;
    for (unsigned i = 0; i < n; i++)
    {
        const MEM_EVENT *a = &ref->Events[i];
        const MEM_EVENT *b = &emu->Events[i];

        if (a->Address != b->Address || a->Width != b->Width ||
            a->Store != b->Store || a->Value != b->Value ||
            a->PlanOp != b->PlanOp || a->Elem != b->Elem)
        {
            g_log.DiffOp = a->PlanOp;
            g_log.DiffEvent = (int)i;
            snprintf(g_log.FirstDiff, sizeof(g_log.FirstDiff),
                     "event %u op%u ref(addr=0x%llx,w=%u,%s,val=0x%llx) "
                     "emu(addr=0x%llx,w=%u,%s,val=0x%llx)",
                     i, a->PlanOp,
                     (unsigned long long)a->Address, a->Width,
                     a->Store ? "W" : "R", (unsigned long long)a->Value,
                     (unsigned long long)b->Address, b->Width,
                     b->Store ? "W" : "R", (unsigned long long)b->Value);
            return 0;
        }
    }
    if (ref->EventCount != emu->EventCount)
    {
        snprintf(g_log.FirstDiff, sizeof(g_log.FirstDiff),
                 "event count ref=%u emu=%u", ref->EventCount, emu->EventCount);
        return 0;
    }

    if (ref->Rax != emu->Rax || ref->Rcx != emu->Rcx ||
        ref->Rsi != emu->Rsi || ref->Rdi != emu->Rdi ||
        ref->Rip != emu->Rip || !flags_agree(ref->Flags, emu->Flags))
    {
        snprintf(g_log.FirstDiff, sizeof(g_log.FirstDiff),
                 "regs ref(rax=%llu rcx=%llu rsi=%llu rdi=%llx fl=%llx) "
                 "emu(rax=%llu rcx=%llu rsi=%llu rdi=%llx fl=%llx)",
                 (unsigned long long)ref->Rax, (unsigned long long)ref->Rcx,
                 (unsigned long long)ref->Rsi, (unsigned long long)ref->Rdi,
                 (unsigned long long)ref->Flags,
                 (unsigned long long)emu->Rax, (unsigned long long)emu->Rcx,
                 (unsigned long long)emu->Rsi, (unsigned long long)emu->Rdi,
                 (unsigned long long)emu->Flags);
        return 0;
    }

    if (memcmp(ref_mem, emu_mem, DATA_BYTES) != 0)
    {
        unsigned at = 0;
        while (at < DATA_BYTES && ref_mem[at] == emu_mem[at])
        {
            at++;
        }
        snprintf(g_log.FirstDiff, sizeof(g_log.FirstDiff),
                 "final memory differs at byte %u", at);
        return 0;
    }

    if (ref->Rejected != emu->Rejected ||
        ref->RejectedPlanOp != emu->RejectedPlanOp)
    {
        snprintf(g_log.FirstDiff, sizeof(g_log.FirstDiff),
                 "rejection ref=%d/%d emu=%d/%d",
                 ref->Rejected, ref->RejectedPlanOp,
                 emu->Rejected, emu->RejectedPlanOp);
        return 0;
    }

    (void)plan;
    return 1;
}

/* Runs one plan twice on independently allocated images and contexts. */
static int
run_and_compare(const PLAN *plan)
{
    uint8_t *ref_mem = malloc(DATA_BYTES);
    uint8_t *emu_mem = malloc(DATA_BYTES);
    RUN_RESULT *ref = calloc(1, sizeof(RUN_RESULT));
    RUN_RESULT *emu = calloc(1, sizeof(RUN_RESULT));
    RUN_RESULT *ref2 = calloc(1, sizeof(RUN_RESULT));
    RUN_RESULT *emu2 = calloc(1, sizeof(RUN_RESULT));
    uint8_t *ref_mem2 = malloc(DATA_BYTES);
    uint8_t *emu_mem2 = malloc(DATA_BYTES);
    int ok = 0;

    if (!ref_mem || !emu_mem || !ref || !emu || !ref2 || !emu2 ||
        !ref_mem2 || !emu_mem2)
    {
        goto out;
    }

    memcpy(ref_mem, plan->InitData, DATA_BYTES);
    memcpy(emu_mem, plan->InitData, DATA_BYTES);

    if (!code_decodes_cleanly(plan))
    {
        snprintf(g_log.FirstDiff, sizeof(g_log.FirstDiff),
                 "plan %u code did not decode cleanly", plan->PlanId);
        goto out;
    }

    ref_run(plan, ref_mem, ref);
    emu_run(plan, emu_mem, emu_mem, emu);

    if (!results_agree(plan, ref, emu, ref_mem, emu_mem))
    {
        goto out;
    }

    /* Replay on brand new contexts and separate buffers. */
    memcpy(ref_mem2, plan->InitData, DATA_BYTES);
    memcpy(emu_mem2, plan->InitData, DATA_BYTES);
    ref_run(plan, ref_mem2, ref2);
    emu_run(plan, emu_mem2, emu_mem2, emu2);

    if (!results_agree(plan, ref2, emu2, ref_mem2, emu_mem2))
    {
        goto out;
    }
    if (memcmp(ref->Events, ref2->Events,
               ref->EventCount * sizeof(MEM_EVENT)) != 0 ||
        ref->EventCount != ref2->EventCount)
    {
        snprintf(g_log.FirstDiff, sizeof(g_log.FirstDiff),
                 "reference replay was not deterministic");
        goto out;
    }

    g_log.PlansRun++;
    ok = 1;

out:
    free(ref_mem);
    free(emu_mem);
    free(ref);
    free(emu);
    free(ref2);
    free(emu2);
    free(ref_mem2);
    free(emu_mem2);
    return ok;
}

static int
run_scenario_group(unsigned scenario, unsigned count)
{
    int all_ok = 1;

    for (unsigned k = 0; k < count; k++)
    {
        PLAN plan;
        uint32_t id = scenario * 1000u + k + 1u;

        gen_plan(&plan, id, scenario);
        if (!run_and_compare(&plan))
        {
            g_log.Failed = 1;
            fprintf(stderr,
                    "PLAN_FAIL id=%u scenario=%u first_diff=%s op=%d event=%d\n",
                    id, scenario, g_log.FirstDiff, g_log.DiffOp, g_log.DiffEvent);
            all_ok = 0;
            break;
        }
    }
    printf("SCENARIO %u requested=%u distinct_plans_total=%d\n",
           scenario, count, g_log.PlansRun);
    return all_ok;
}

/* Runs one freshly built plan on independent stack-backed images. */
static void
run_pair(const PLAN *plan, uint8_t *ref_mem, uint8_t *emu_mem,
         RUN_RESULT *ref, RUN_RESULT *emu)
{
    memcpy(ref_mem, plan->InitData, DATA_BYTES);
    memcpy(emu_mem, plan->InitData, DATA_BYTES);
    ref_run(plan, ref_mem, ref);
    emu_run(plan, emu_mem, emu_mem, emu);
}

/* ------------------------------------------------------------------ */
/* Unit cases.  The three rep_*_witness cases contain ONLY tagged      */
/* assertions, so a mutation always surfaces as the matching FAIL tag.  */
/* ------------------------------------------------------------------ */
static void
case_normal_alias_and_mixed(void)
{
    /* the code, virtual stack and external data ranges must never overlap. */
    int disjoint = STACK_BASE + STACK_SIZE <= CODE_BASE &&
                   CODE_BASE + CODE_CAPACITY <= DATA_BASE;
    UNIT_CHECK(disjoint);
    UNIT_CHECK(run_scenario_group(SCN_MOVS_ALIAS, 24));
    UNIT_CHECK(run_scenario_group(SCN_MIXED, 24));
}

static void
case_normal_stos_lods_chains(void)
{
    UNIT_CHECK(run_scenario_group(SCN_STOS_CMPS, 24));
    UNIT_CHECK(run_scenario_group(SCN_LODS_SCAS, 24));
}

static void
case_boundary_termination_group(void)
{
    UNIT_CHECK(run_scenario_group(SCN_CMPS_STOP, 24));
}

static void
case_boundary_zero_count(void)
{
    UNIT_CHECK(run_scenario_group(SCN_ZEROCOUNT, 8));

    PLAN plan;
    gen_plan(&plan, 6001, SCN_ZEROCOUNT);
    uint8_t m1[DATA_BYTES], m2[DATA_BYTES];
    RUN_RESULT r1, r2;
    run_pair(&plan, m1, m2, &r1, &r2);

    /* every op had RCX == 0: zero data accesses, frozen state and memory */
    const PLAN_OP *last = &plan.Ops[plan.OpCount - 1];
    uint64_t entry_rdi = region_base(last->DstRegion) +
                         (uint64_t)last->DstOffset * 8u + last->DstBias;
    uint64_t entry_rsi = region_base(last->SrcRegion) +
                         (uint64_t)last->SrcOffset * 8u + last->SrcBias;
    uint64_t entry_flags = INIT_RFLAGS;
    if (last->Df)
    {
        entry_flags |= NDR_RFLAG_DF;          /* STD already executed */
    }
    else
    {
        entry_flags &= ~NDR_RFLAG_DF;
    }
    int zero_witness = r1.EventCount == 0 && r2.EventCount == 0 &&
                       r1.Rcx == 0 && r2.Rcx == 0 &&
                       /* pointers and flags frozen at string-op entry */
                       r2.Rdi == entry_rdi && r2.Rsi == entry_rsi &&
                       r1.Rdi == entry_rdi && r1.Rsi == entry_rsi &&
                       r2.Flags == entry_flags && r1.Flags == entry_flags &&
                       memcmp(m1, plan.InitData, DATA_BYTES) == 0 &&
                       memcmp(m2, plan.InitData, DATA_BYTES) == 0;
    UNIT_CHECK(zero_witness);
}

static void
case_integration_callback_reject(void)
{
    UNIT_CHECK(run_scenario_group(SCN_CB_REJECT, 24));

    PLAN plan;
    gen_plan(&plan, 7001, SCN_CB_REJECT);
    uint8_t m1[DATA_BYTES], m2[DATA_BYTES];
    RUN_RESULT r1, r2;
    run_pair(&plan, m1, m2, &r1, &r2);

    int reject_ok = r1.Status == SHEMU_ABORT_GLA_OUTSIDE &&
                    r2.Status == SHEMU_ABORT_GLA_OUTSIDE &&
                    r1.Rejected && r2.Rejected &&
                    r1.RejectedPlanOp == 1 && r2.RejectedPlanOp == 1 &&
                    /* the committed prefix from the first op is retained */
                    r1.EventCount >= 3 && r2.EventCount >= 3 &&
                    r1.EventCount == r2.EventCount;
    UNIT_CHECK(reject_ok);
}

/* Dedicated REP MOVS plan with an odd element count. */
static void
build_counter_plan(PLAN *plan)
{
    memset(plan, 0, sizeof(*plan));
    plan->PlanId = 9001;
    plan->InitFlags = INIT_RFLAGS;
    plan->EndKind = 0;
    for (unsigned i = 0; i < DATA_BYTES; i++)
    {
        plan->InitData[i] = (uint8_t)(0x31u + (i % 97u));
    }
    plan->OpCount = 1;
    PLAN_OP *o = &plan->Ops[0];
    o->Ins = OP_MOVS; o->Prefix = PFX_REP; o->Width = 2;
    o->Df = 0; o->SrcRegion = 0; o->DstRegion = 1;
    o->SrcOffset = 2; o->DstOffset = 2; o->Count = 5;       /* odd */
    plan_encode(plan);
}

static void
case_rep_counter_witness(void)
{
    PLAN plan;
    build_counter_plan(&plan);
    uint8_t m1[DATA_BYTES], m2[DATA_BYTES];
    RUN_RESULT r1, r2;
    run_pair(&plan, m1, m2, &r1, &r2);

    /* exactly five single decrements must leave RCX at zero; a double
     * decrement (counter mutation) cannot reach zero from an odd count. */
    int counter_ok = r1.Rcx == 0 && r2.Rcx == 0 &&
                     r1.EventCount == r2.EventCount &&
                     r1.EventCount == 10u;            /* 5 reads + 5 writes */
    UNIT_CHECK_TAG("counter", counter_ok);
}

static void
case_rep_direction_witness(void)
{
    PLAN plan;
    build_counter_plan(&plan);
    /* force the backward direction for the same plan */
    plan.Ops[0].Df = 1;
    plan_encode(&plan);

    uint8_t m1[DATA_BYTES], m2[DATA_BYTES];
    RUN_RESULT r1, r2;
    run_pair(&plan, m1, m2, &r1, &r2);

    const PLAN_OP *o = &plan.Ops[0];
    uint64_t dst_start = region_base(o->DstRegion) +
                         (uint64_t)o->DstOffset * 8u + o->DstBias;
    uint64_t src_start = region_base(o->SrcRegion) +
                         (uint64_t)o->SrcOffset * 8u + o->SrcBias;
    int64_t expect = -(int64_t)o->Count * (int64_t)o->Width;

    /* DF=1 must step both pointers downward, event by event. */
    int ptrs_ok = (int64_t)(r2.Rdi - dst_start) == expect &&
                  (int64_t)(r2.Rsi - src_start) == expect &&
                  (int64_t)(r1.Rdi - dst_start) == expect &&
                  (int64_t)(r1.Rsi - src_start) == expect;

    /* Consecutive reads, and consecutive writes, must each descend by exactly
     * one element width (the read-before-write ordering is kept separately). */
    int seq_ok = 1;
    int have_r = 0, have_w = 0;
    uint64_t prev_r = 0, prev_w = 0;
    for (unsigned i = 0; i < r2.EventCount; i++)
    {
        const MEM_EVENT *e = &r2.Events[i];
        if (e->PlanOp != 0)
        {
            continue;
        }
        if (!e->Store)
        {
            if (have_r && prev_r - e->Address != o->Width)
            {
                seq_ok = 0;
            }
            prev_r = e->Address;
            have_r = 1;
        }
        else
        {
            if (have_w && prev_w - e->Address != o->Width)
            {
                seq_ok = 0;
            }
            prev_w = e->Address;
            have_w = 1;
        }
    }
    UNIT_CHECK_TAG("direction", o->Df == 1 && ptrs_ok && seq_ok &&
                                r1.EventCount == r2.EventCount);
}

/* REPE CMPS whose first mismatch sits at a chosen element index. */
static void
build_repe_plan(PLAN *plan, unsigned mismatch, unsigned count)
{
    memset(plan, 0, sizeof(*plan));
    plan->PlanId = 9100u + mismatch;
    plan->InitFlags = INIT_RFLAGS;
    plan->EndKind = 1;
    plan->OpCount = 1;
    PLAN_OP *o = &plan->Ops[0];
    o->Ins = OP_CMPS; o->Prefix = PFX_REPE; o->Width = 1;
    o->Df = 0; o->SrcRegion = 0; o->DstRegion = 1;
    o->SrcOffset = 8; o->DstOffset = 8; o->Count = (uint8_t)count;
    for (unsigned k = 0; k < count; k++)
    {
        plan->InitData[REGION_A_OFF + o->SrcOffset * 8u + k] = 0x41;
        plan->InitData[REGION_B_OFF + o->DstOffset * 8u + k] =
            (k == mismatch) ? 0x42 : 0x41;
    }
    plan_encode(plan);
}

static void
case_rep_termination_witness(void)
{
    /* first-element stop, last-element stop, and an all-equal run. */
    PLAN first, last, equal;
    build_repe_plan(&first, 0, 5);
    build_repe_plan(&last, 4, 5);
    build_repe_plan(&equal, 99, 5);                 /* mismatch never hit */

    uint8_t fm[DATA_BYTES], lm[DATA_BYTES], em[DATA_BYTES];
    uint8_t fm2[DATA_BYTES], lm2[DATA_BYTES], em2[DATA_BYTES];
    RUN_RESULT fr, lr, er, fr2, lr2, er2;
    run_pair(&first, fm, fm2, &fr, &fr2);
    run_pair(&last, lm, lm2, &lr, &lr2);
    run_pair(&equal, em, em2, &er, &er2);

    /* first mismatch consumes one element: RCX 5 -> 4 and ZF clear. */
    int first_ok = fr.Rcx == 4 && fr2.Rcx == 4 &&
                   (fr.Flags & NDR_RFLAG_ZF) == 0 &&
                   (fr2.Flags & NDR_RFLAG_ZF) == 0 &&
                   fr.EventCount == fr2.EventCount;
    /* last-element mismatch consumes all five, RCX reaches zero. */
    int last_ok = lr.Rcx == 0 && lr2.Rcx == 0 &&
                  (lr.Flags & NDR_RFLAG_ZF) == 0 &&
                  lr.EventCount == lr2.EventCount;
    /* all equal: every element compared and ZF stays set. */
    int equal_ok = er.Rcx == 0 && er2.Rcx == 0 &&
                   (er.Flags & NDR_RFLAG_ZF) != 0 &&
                   (er2.Flags & NDR_RFLAG_ZF) != 0 &&
                   er.EventCount == 10u && er2.EventCount == 10u;
    /* the terminating comparison really consumed an element each time. */
    int consumed_ok = fr.EventCount == 2u && lr.EventCount == 10u;

    UNIT_CHECK_TAG("termination",
                   first_ok && last_ok && equal_ok && consumed_ok);
}

/* Concrete arithmetic-flag vectors: highest-bit borrow (CF), parity (PF),
 * signed overflow (SF/OF) and zero (ZF), across every element width, plus
 * preservation of ZF across a following non-flag-writing MOVS. */
static void
build_flag_plan(PLAN *plan)
{
    struct { uint8_t width; uint64_t a; uint64_t b; } vec[4] = {
        { 1, 0x0000000000000000ULL, 0x0000000000000080ULL },  /* CF,SF,OF */
        { 2, 0x0000000000000000ULL, 0x0000000000000001ULL },  /* CF,SF,PF */
        { 4, 0x0000000080000000ULL, 0x0000000000000001ULL },  /* CF,OF,PF */
        { 8, 0x1122334455667788ULL, 0x1122334455667788ULL },  /* ZF,PF */
    };

    memset(plan, 0, sizeof(*plan));
    plan->PlanId = 9200;
    plan->InitFlags = INIT_RFLAGS;
    plan->EndKind = 0;
    plan->OpCount = 5;

    for (unsigned k = 0; k < 4; k++)
    {
        PLAN_OP *o = &plan->Ops[k];
        o->Ins = OP_CMPS; o->Prefix = PFX_NONE; o->Width = vec[k].width;
        o->Df = 0; o->SrcRegion = 0; o->DstRegion = 1;
        o->SrcOffset = 8; o->DstOffset = 8; o->Count = 1;
        store_le(plan->InitData + REGION_A_OFF + o->SrcOffset * 8u,
                 o->Width, vec[k].a);
        store_le(plan->InitData + REGION_B_OFF + o->DstOffset * 8u,
                 o->Width, vec[k].b);
    }

    /* non-repeated MOVS must leave the ZF set by the last (equal) compare. */
    PLAN_OP *m = &plan->Ops[4];
    m->Ins = OP_MOVS; m->Prefix = PFX_NONE; m->Width = 1;
    m->Df = 0; m->SrcRegion = 0; m->DstRegion = 2;
    m->SrcOffset = 8; m->DstOffset = 8; m->Count = 1;

    plan_encode(plan);
}

static void
case_boundary_arith_flags(void)
{
    PLAN plan;
    build_flag_plan(&plan);
    uint8_t m1[DATA_BYTES], m2[DATA_BYTES];
    RUN_RESULT r1, r2;
    run_pair(&plan, m1, m2, &r1, &r2);

    /* library and independent model agree on the compared flags. */
    int agree = flags_agree(r1.Flags, r2.Flags) &&
                r1.Status == r2.Status && r1.Status == SHEMU_ABORT_RIP_OUTSIDE;
    UNIT_CHECK(agree);

    /* concrete expected bits after the final MOVS (ZF must have survived). */
    uint64_t f = r2.Flags;
    int concrete = (f & NDR_RFLAG_ZF) != 0 &&            /* last compare equal */
                   (f & NDR_RFLAG_CF) == 0 &&
                   (f & NDR_RFLAG_SF) == 0 &&
                   (f & NDR_RFLAG_OF) == 0 &&
                   (f & NDR_RFLAG_PF) != 0;
    UNIT_CHECK(concrete);

    /* re-derive each width's expected compare flags from the independent
     * model and confirm the emulator produced identical bits. */
    struct { uint8_t width; uint64_t a; uint64_t b; } vec[4] = {
        { 1, 0x0000000000000000ULL, 0x0000000000000080ULL },  /* CF,SF,OF */
        { 2, 0x0000000000000000ULL, 0x0000000000000001ULL },  /* CF,SF,PF */
        { 4, 0x0000000080000000ULL, 0x0000000000000001ULL },  /* CF,OF,PF */
        { 8, 0x1122334455667788ULL, 0x1122334455667788ULL },  /* ZF,PF */
    };
    static const uint64_t expect_cf[4] = { 1, 1, 0, 0 };
    static const uint64_t expect_of[4] = { 1, 0, 1, 0 };
    int per_width = 1;
    for (unsigned k = 0; k < 4; k++)
    {
        PLAN one;
        memset(&one, 0, sizeof(one));
        one.PlanId = 9300u + k;
        one.InitFlags = INIT_RFLAGS;
        one.EndKind = 0;
        one.OpCount = 1;
        PLAN_OP *o = &one.Ops[0];
        o->Ins = OP_CMPS; o->Prefix = PFX_NONE;
        o->Width = vec[k].width;
        o->SrcRegion = 0; o->DstRegion = 1;
        o->SrcOffset = 8; o->DstOffset = 8; o->Count = 1;
        /* fresh, non-overlapping-by-construction vector data */
        store_le(one.InitData + REGION_A_OFF + o->SrcOffset * 8u,
                 o->Width, vec[k].a);
        store_le(one.InitData + REGION_B_OFF + o->DstOffset * 8u,
                 o->Width, vec[k].b);
        plan_encode(&one);

        uint8_t a[DATA_BYTES], b[DATA_BYTES];
        RUN_RESULT ra, rb;
        run_pair(&one, a, b, &ra, &rb);
        if (!flags_agree(ra.Flags, rb.Flags) ||
            ((rb.Flags & NDR_RFLAG_CF) != 0) != expect_cf[k] ||
            ((rb.Flags & NDR_RFLAG_OF) != 0) != expect_of[k])
        {
            per_width = 0;
        }
    }
    UNIT_CHECK(per_width);
}

/* The two required end mechanisms: an explicit code-region end and an
 * unsupported instruction, neither of which may masquerade as budget end. */
static void
case_boundary_stop_reasons(void)
{
    PLAN end_region, end_ud2;
    build_counter_plan(&end_region);                 /* EndKind = 0 */
    build_counter_plan(&end_ud2);
    end_ud2.EndKind = 1;
    plan_encode(&end_ud2);

    uint8_t rm[DATA_BYTES], um[DATA_BYTES];
    uint8_t rm2[DATA_BYTES], um2[DATA_BYTES];
    RUN_RESULT rr, ur, rr2, ur2;
    run_pair(&end_region, rm, rm2, &rr, &rr2);
    run_pair(&end_ud2, um, um2, &ur, &ur2);

    int reasons = rr.Status == SHEMU_ABORT_RIP_OUTSIDE &&
                  rr2.Status == SHEMU_ABORT_RIP_OUTSIDE &&
                  ur.Status == SHEMU_ABORT_CANT_EMULATE &&
                  ur2.Status == SHEMU_ABORT_CANT_EMULATE &&
                  /* budget exhaustion returns SUCCESS; neither hit it */
                  rr.Status != SHEMU_SUCCESS && ur.Status != SHEMU_SUCCESS;
    UNIT_CHECK(reasons);
}

/* On a rejected read the library keeps the partial register updates it has
 * already made; there is no whole-instruction rollback.  This pins down that
 * convention rather than assuming transactional semantics. */
static void
case_boundary_partial_update(void)
{
    PLAN plan;
    memset(&plan, 0, sizeof(plan));
    plan.PlanId = 9400;
    plan.InitFlags = INIT_RFLAGS;
    plan.EndKind = 0;
    plan.OpCount = 1;
    PLAN_OP *o = &plan.Ops[0];
    o->Ins = OP_CMPS; o->Prefix = PFX_REPNE; o->Width = 4;
    o->Df = 0; o->SrcRegion = 0; o->DstRegion = 1;
    o->SrcOffset = 6; o->DstOffset = 6; o->Count = 3;
    o->InjectFail = 1; o->FailEvent = 2;          /* reject the 2nd read */
    plan_encode(&plan);

    uint8_t m1[DATA_BYTES], m2[DATA_BYTES];
    RUN_RESULT r1, r2;
    run_pair(&plan, m1, m2, &r1, &r2);

    uint64_t src = region_base(0) + 6u * 8u;
    uint64_t dst = region_base(1) + 6u * 8u;

    int aborted = r1.Status == SHEMU_ABORT_GLA_OUTSIDE &&
                  r2.Status == SHEMU_ABORT_GLA_OUTSIDE;
    /* only the source read was committed; RSI advanced once, RDI did not */
    int partial = r1.EventCount == 1 && r2.EventCount == 1 &&
                  r2.Rsi == src + 4 && r2.Rdi == dst &&
                  r1.Rsi == src + 4 && r1.Rdi == dst &&
                  /* RCX is not decremented until both operands are read */
                  r1.Rcx == 3 && r2.Rcx == 3 &&
                  r2.Rip == CODE_BASE + plan.OpEnd[0];
    UNIT_CHECK(aborted && partial);
}

const unit_case unit_cases[] = {
    { "normal_alias_and_mixed",      "normal",      case_normal_alias_and_mixed },
    { "normal_stos_lods_chains",     "normal",      case_normal_stos_lods_chains },
    { "boundary_termination",        "boundary",    case_boundary_termination_group },
    { "boundary_zero_count",         "boundary",    case_boundary_zero_count },
    { "boundary_arith_flags",        "boundary",    case_boundary_arith_flags },
    { "boundary_stop_reasons",       "boundary",    case_boundary_stop_reasons },
    { "boundary_partial_update",     "boundary",    case_boundary_partial_update },
    { "rep_counter_witness",         "integration", case_rep_counter_witness },
    { "rep_direction_witness",       "integration", case_rep_direction_witness },
    { "rep_termination_witness",     "integration", case_rep_termination_witness },
    { "integration_callback_reject", "integration", case_integration_callback_reject },
};

const size_t unit_case_count = sizeof(unit_cases) / sizeof(unit_cases[0]);

