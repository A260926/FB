/*
 * Regression tests for local (intra-bytecode) calls in the uBPF interpreter.
 *
 * Four contracts are exercised by the fixed driver:
 *   contract_call_graph   - function segmentation, control-flow containment,
 *                           recursion rejection and the 8-frame depth limit;
 *   contract_frames       - calling convention, register and stack isolation,
 *                           shared callees and forward/backward calls;
 *   contract_stack_bounds - per-frame stack bounds checking;
 *   contract_helpers_jit  - helper/local-call separation, unwind propagation,
 *                           JIT rejection and unload/reload reuse.
 *
 * Each entry point returns the number of assertions that actually ran to
 * completion; a failed assertion aborts with a nonzero exit status.  Test
 * programs are small hand-written instruction arrays, and only registers or
 * stack bytes the program explicitly wrote are inspected.  No test depends on
 * uninitialised reads, external services or an infinite loop.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ubpf.h"
#include "ebpf.h"

/* ---------------- instruction constructors ---------------- */

static struct ebpf_inst
local_call(int32_t imm)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_CALL, .src = 1, .imm = imm };
    return inst;
}

static struct ebpf_inst
helper_call(unsigned idx)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_CALL, .src = 0, .imm = (int32_t)idx };
    return inst;
}

static struct ebpf_inst
mov64(int dst, int32_t imm)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_MOV64_IMM, .dst = dst, .imm = imm };
    return inst;
}

static struct ebpf_inst
mov64_reg(int dst, int src)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_MOV64_REG, .dst = dst, .src = src };
    return inst;
}

static struct ebpf_inst
add64_imm(int dst, int32_t imm)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_ADD64_IMM, .dst = dst, .imm = imm };
    return inst;
}

static struct ebpf_inst
add64_reg(int dst, int src)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_ADD64_REG, .dst = dst, .src = src };
    return inst;
}

static struct ebpf_inst
sub64_reg(int dst, int src)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_SUB64_REG, .dst = dst, .src = src };
    return inst;
}

static struct ebpf_inst
ldxdw(int dst, int src, int16_t offset)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_LDXDW, .dst = dst, .src = src, .offset = offset };
    return inst;
}

static struct ebpf_inst
stxdw(int dst, int src, int16_t offset)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_STXDW, .dst = dst, .src = src, .offset = offset };
    return inst;
}

static struct ebpf_inst
stdw_imm(int dst, int16_t offset, int32_t imm)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_STDW, .dst = dst, .offset = offset, .imm = imm };
    return inst;
}

static struct ebpf_inst
exit_inst(void)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_EXIT };
    return inst;
}

static struct ebpf_inst
ja(int16_t offset)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_JA, .offset = offset };
    return inst;
}

static struct ebpf_inst
jeq_imm(int dst, int32_t imm, int16_t offset)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_JEQ_IMM, .dst = dst, .offset = offset, .imm = imm };
    return inst;
}

/* ---------------- load / execution helpers ---------------- */

static struct ubpf_vm *
load_ok(const struct ebpf_inst *prog, size_t count)
{
    struct ubpf_vm *vm = ubpf_create();
    assert(vm != NULL);
    char *errmsg = NULL;
    int rv = ubpf_load(vm, prog, (uint32_t)(count * sizeof(prog[0])), &errmsg);
    if (rv != 0) {
        fprintf(stderr, "expected load to succeed: %s\n", errmsg ? errmsg : "(no message)");
        exit(1);
    }
    assert(rv == 0 && errmsg == NULL);
    return vm;
}

/* Expect rejection; the returned message is owned by the caller. */
static char *
load_reject(const struct ebpf_inst *prog, size_t count)
{
    struct ubpf_vm *vm = ubpf_create();
    assert(vm != NULL);
    char *errmsg = NULL;
    int rv = ubpf_load(vm, prog, (uint32_t)(count * sizeof(prog[0])), &errmsg);
    if (rv >= 0) {
        fprintf(stderr, "expected load to be rejected\n");
        exit(1);
    }
    assert(rv < 0 && errmsg != NULL);
    ubpf_destroy(vm);
    return errmsg;
}

static uint64_t
exec_ok(struct ubpf_vm *vm, void *mem, size_t mem_len)
{
    uint64_t out = 0;
    int rv = ubpf_exec(vm, mem, mem_len, &out);
    if (rv != 0) {
        fprintf(stderr, "expected execution to succeed, got %d\n", rv);
        exit(1);
    }
    assert(rv == 0);
    return out;
}

/* Expect failure and assert the caller's result slot was not written. */
static void
exec_fail(struct ubpf_vm *vm, void *mem, size_t mem_len)
{
    uint64_t sentinel = UINT64_C(0xfeedfacecafebeef);
    uint64_t value = sentinel;
    int rv = ubpf_exec(vm, mem, mem_len, &value);
    assert(rv < 0);
    assert(value == sentinel);
}

/* ---------------- helpers used by the tests ---------------- */

static unsigned helper_calls;

static uint64_t
helper_seven(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    helper_calls++;
    return a + 7;
}

static unsigned unwind_calls;

/* Returns its first argument: r1 == 0 unwinds, nonzero continues. */
static uint64_t
helper_unwind(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    (void)b; (void)c; (void)d; (void)e;
    unwind_calls++;
    return a;
}

/* ====================================================================== */
/* Contract 1: call graph, function segmentation and the depth limit.     */
/* ====================================================================== */

int
contract_call_graph(void)
{
    int n = 0;

    /* A shared callee reached by a forward call and by a backward call.
     *
     *   entry [0,5):
     *     0: call g        (5, forward)
     *     1: mov r6, r0    (g() = 6)
     *     2: call h        (8, forward)
     *     3: add r0, r6    (h() = 7, total 13)
     *     4: exit
     *   g [5,8): 5: mov r0,5; 6: add r0,1; 7: exit
     *   h [8,11): 8: call g (5, backward, imm -4); 9: add r0,1; 10: exit
     */
    {
        struct ebpf_inst prog[] = {
            local_call(4),
            mov64_reg(6, 0),
            local_call(5),
            add64_reg(0, 6),
            exit_inst(),
            mov64(0, 5),
            add64_imm(0, 1),
            exit_inst(),
            local_call(-4),
            add64_imm(0, 1),
            exit_inst(),
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        assert(exec_ok(vm, NULL, 0) == 13);
        n++;
        ubpf_destroy(vm);
    }

    /* Longest accepted chain: entry + 7 callees = 8 frames. */
    {
        struct ebpf_inst prog[16];
        int i;
        for (i = 0; i < 7; i++) {
            prog[2 * i] = local_call(1);       /* pc 2i -> 2i+2 */
            prog[2 * i + 1] = exit_inst();
        }
        prog[14] = mov64(0, 42);
        prog[15] = exit_inst();
        struct ubpf_vm *vm = load_ok(prog, 16);
        assert(exec_ok(vm, NULL, 0) == 42);
        n++;
        ubpf_destroy(vm);
    }

    /* One frame past the limit (entry + 8 callees) is rejected. */
    {
        struct ebpf_inst prog[18];
        int i;
        for (i = 0; i < 8; i++) {
            prog[2 * i] = local_call(1);
            prog[2 * i + 1] = exit_inst();
        }
        prog[16] = mov64(0, 42);
        prog[17] = exit_inst();
        char *err = load_reject(prog, 18);
        assert(strstr(err, "too deep") != NULL);
        n++;
        free(err);
    }

    /* Wide but shallow: eight separate callees, each called once by the
     * entry.  Depth must not be confused with the number of functions. */
    {
        struct ebpf_inst prog[25];
        int i;
        for (i = 0; i < 8; i++) {
            int callee = 9 + 2 * i;
            prog[i] = local_call((int32_t)(callee - (i + 1)));
        }
        prog[8] = exit_inst();
        for (i = 0; i < 8; i++) {
            prog[9 + 2 * i] = mov64(0, i);
            prog[10 + 2 * i] = exit_inst();
        }
        struct ubpf_vm *vm = load_ok(prog, 25);
        assert(exec_ok(vm, NULL, 0) == 7);
        n++;
        ubpf_destroy(vm);
    }

    /* Direct recursion: f calls itself. */
    {
        struct ebpf_inst prog[] = {
            local_call(1),
            exit_inst(),
            local_call(-1),
            exit_inst(),
        };
        char *err = load_reject(prog, sizeof(prog)/sizeof(prog[0]));
        assert(strstr(err, "recursive") != NULL);
        n++;
        free(err);
    }

    /* Indirect recursion: entry -> a -> b -> a. */
    {
        struct ebpf_inst prog[] = {
            local_call(3),      /* 0 -> 4: a */
            exit_inst(),
            local_call(1),      /* 2 -> 4: b calls a */
            exit_inst(),
            local_call(-3),     /* 4 -> 2: a calls b */
            exit_inst(),
        };
        char *err = load_reject(prog, sizeof(prog)/sizeof(prog[0]));
        assert(strstr(err, "recursive") != NULL);
        n++;
        free(err);
    }

    /* A jump across a function boundary is rejected. */
    {
        struct ebpf_inst prog[] = {
            local_call(2),      /* 0 -> 3: f */
            mov64(0, 0),        /* 1 */
            ja(1),              /* 2 -> 4, inside f [3,5) */
            exit_inst(),        /* 3: f */
            exit_inst(),
        };
        char *err = load_reject(prog, sizeof(prog)/sizeof(prog[0]));
        assert(strstr(err, "cross-function") != NULL);
        n++;
        free(err);
    }

    /* The same check covers unreachable code: pc 2 is never executed but
     * still jumps into the segment of f. */
    {
        struct ebpf_inst prog[] = {
            local_call(2),      /* 0 -> 3: g */
            exit_inst(),        /* 1 */
            ja(3),              /* 2 -> 6, inside g [3,7) */
            exit_inst(),        /* 3: g */
            exit_inst(),        /* 4 */
            exit_inst(),        /* 5 */
            exit_inst(),        /* 6 */
        };
        char *err = load_reject(prog, sizeof(prog)/sizeof(prog[0]));
        assert(strstr(err, "cross-function") != NULL);
        n++;
        free(err);
    }

    /* Fallthrough into a function body is rejected. */
    {
        struct ebpf_inst prog[] = {
            local_call(2),      /* 0 -> 3: f */
            mov64(0, 0),        /* 1 */
            mov64(0, 0),        /* 2 falls through to 3 */
            mov64(0, 1),        /* 3: f */
            exit_inst(),
        };
        char *err = load_reject(prog, sizeof(prog)/sizeof(prog[0]));
        assert(strstr(err, "fallthrough") != NULL);
        n++;
        free(err);
    }

    /* A conditional jump whose branch target leaves the function is
     * rejected. */
    {
        struct ebpf_inst prog[] = {
            mov64(0, 0),        /* 0 */
            jeq_imm(0, 0, 6),   /* 1 -> 8, inside f [5,9) */
            mov64(0, 0),        /* 2 */
            local_call(1),      /* 3 -> 5 */
            exit_inst(),        /* 4 */
            mov64(0, 0),        /* 5: f */
            mov64(0, 1),        /* 6 */
            exit_inst(),        /* 7 */
            exit_inst(),        /* 8 */
        };
        char *err = load_reject(prog, sizeof(prog)/sizeof(prog[0]));
        assert(strstr(err, "cross-function") != NULL);
        n++;
        free(err);
    }

    /* A call target in the data slot of an LDDW is rejected. */
    {
        struct ebpf_inst prog[] = {
            local_call(1),      /* 0 -> 2, the data slot */
            { .opcode = EBPF_OP_LDDW, .dst = 0, .imm = 0 },  /* 1 */
            { .opcode = 0, .imm = 0 },                        /* 2 */
            exit_inst(),
        };
        char *err = load_reject(prog, sizeof(prog)/sizeof(prog[0]));
        assert(strstr(err, "middle of lddw") != NULL);
        n++;
        free(err);
    }

    /* Target past the end of the code. */
    {
        struct ebpf_inst prog[] = {
            local_call(100),
            exit_inst(),
        };
        char *err = load_reject(prog, sizeof(prog)/sizeof(prog[0]));
        assert(strstr(err, "out of bounds") != NULL);
        n++;
        free(err);
    }

    /* Target arithmetic that would overflow must not wrap into range. */
    {
        struct ebpf_inst prog[] = {
            local_call(INT32_MAX),
            exit_inst(),
        };
        char *err = load_reject(prog, sizeof(prog)/sizeof(prog[0]));
        assert(strstr(err, "out of bounds") != NULL);
        n++;
        free(err);
    }

    /* Local CALL with nonzero dst / offset, and any other src value. */
    {
        struct ebpf_inst prog[] = {
            { .opcode = EBPF_OP_CALL, .src = 1, .dst = 1, .imm = 1 },
            exit_inst(),
            mov64(0, 0),
            exit_inst(),
        };
        char *err = load_reject(prog, sizeof(prog)/sizeof(prog[0]));
        assert(strstr(err, "nonzero dst") != NULL);
        n++;
        free(err);
    }
    {
        struct ebpf_inst prog[] = {
            { .opcode = EBPF_OP_CALL, .src = 1, .offset = 1, .imm = 1 },
            exit_inst(),
            mov64(0, 0),
            exit_inst(),
        };
        char *err = load_reject(prog, sizeof(prog)/sizeof(prog[0]));
        assert(strstr(err, "nonzero offset") != NULL);
        n++;
        free(err);
    }
    {
        struct ebpf_inst prog[] = {
            { .opcode = EBPF_OP_CALL, .src = 2, .imm = 0 },
            exit_inst(),
        };
        char *err = load_reject(prog, sizeof(prog)/sizeof(prog[0]));
        assert(strstr(err, "invalid call src") != NULL);
        n++;
        free(err);
    }

    /* EXIT has no fallthrough successor, so an EXIT directly before a
     * function entry is fine. */
    {
        struct ebpf_inst prog[] = {
            local_call(1),      /* 0 -> 2 */
            exit_inst(),        /* 1 */
            mov64(0, 9),        /* 2: f */
            exit_inst(),
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        assert(exec_ok(vm, NULL, 0) == 9);
        n++;
        ubpf_destroy(vm);
    }

    /* LDDW in the caller occupies two slots; a target after it is a real
     * instruction and segmentation still works. */
    {
        struct ebpf_inst prog[] = {
            local_call(3),      /* 0 -> 4, after the two LDDW slots */
            { .opcode = EBPF_OP_LDDW, .dst = 6, .imm = 0x00000011 }, /* 1 */
            { .opcode = 0, .imm = 0x22334455 },                       /* 2 */
            exit_inst(),        /* 3 */
            mov64(0, 7),        /* 4: f */
            exit_inst(),
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        assert(exec_ok(vm, NULL, 0) == 7);
        n++;
        ubpf_destroy(vm);
    }

    /* A loop internal to a function is allowed. */
    {
        struct ebpf_inst prog[] = {
            local_call(1),          /* 0 -> 2: f */
            exit_inst(),            /* 1 */
            mov64(0, 0),            /* 2: f */
            add64_imm(0, 1),        /* 3 */
            { .opcode = EBPF_OP_JNE_IMM, .dst = 0, .imm = 3, .offset = -2 }, /* 4 -> 3 while r0 != 3 */
            exit_inst(),            /* 5 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        assert(exec_ok(vm, NULL, 0) == 3);
        n++;
        ubpf_destroy(vm);
    }

    return n;
}

/* ====================================================================== */
/* Contract 2: calling convention and frame isolation.                    */
/* ====================================================================== */

int
contract_frames(void)
{
    int n = 0;

    /* r1..r5 are arguments; r6..r9 are inherited.  The callee sums all nine
     * and returns 11+22+33+44+55+66+77+88+99 = 495. */
    {
        struct ebpf_inst prog[] = {
            mov64(1, 11), mov64(2, 22), mov64(3, 33), mov64(4, 44),
            mov64(5, 55), mov64(6, 66), mov64(7, 77), mov64(8, 88),
            mov64(9, 99),
            local_call(1),      /* 9 -> 11 */
            exit_inst(),        /* 10 */
            mov64_reg(0, 1),    /* 11: f */
            add64_reg(0, 2), add64_reg(0, 3), add64_reg(0, 4),
            add64_reg(0, 5), add64_reg(0, 6), add64_reg(0, 7),
            add64_reg(0, 8), add64_reg(0, 9),
            exit_inst(),
        };
        assert(sizeof(prog)/sizeof(prog[0]) == 21);
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        assert(exec_ok(vm, NULL, 0) == 495);
        n++;
        ubpf_destroy(vm);
    }

    /* A callee overwrites r6..r9; the caller must still see its own values.
     * Result 111 + 10000 + 222 + 333 + 444 = 11110. */
    {
        struct ebpf_inst prog[] = {
            mov64(6, 111), mov64(7, 222), mov64(8, 333), mov64(9, 444),
            local_call(6),          /* 4 -> 11: f */
            add64_reg(6, 0),        /* 5 */
            add64_reg(6, 7),        /* 6 */
            add64_reg(6, 8),        /* 7 */
            add64_reg(6, 9),        /* 8 */
            mov64_reg(0, 6),        /* 9 */
            exit_inst(),            /* 10 */
            mov64(6, 1000),         /* 11: f */
            mov64(7, 2000),         /* 12 */
            mov64(8, 3000),         /* 13 */
            mov64(9, 4000),         /* 14 */
            mov64(0, 10000),        /* 15 */
            exit_inst(),            /* 16 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        assert(exec_ok(vm, NULL, 0) == 11110);
        n++;
        ubpf_destroy(vm);
    }

    /* Frames have independent stacks: the callee writes its own r10-relative
     * slots and the caller's written slots are unchanged.  Result
     * 0x3333 + 0x1111 + 0x2222 = 0x6666. */
    {
        struct ebpf_inst prog[] = {
            stdw_imm(10, -8, 0x1111),       /* 0 */
            stdw_imm(10, -16, 0x2222),      /* 1 */
            local_call(7),                  /* 2 -> 10: f */
            mov64_reg(7, 0),                /* 3: r7 = 0x3333 */
            ldxdw(6, 10, -8),               /* 4: caller slot 0x1111 */
            ldxdw(1, 10, -16),              /* 5: caller slot 0x2222 */
            mov64_reg(0, 7),                /* 6 */
            add64_reg(0, 6),                /* 7 */
            add64_reg(0, 1),                /* 8 */
            exit_inst(),                    /* 9 */
            stdw_imm(10, -8, 0x3333),       /* 10: f */
            stdw_imm(10, -16, 0x4444),      /* 11 */
            ldxdw(0, 10, -8),               /* 12 */
            exit_inst(),                    /* 13 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        assert(exec_ok(vm, NULL, 0) == 0x6666);
        n++;
        ubpf_destroy(vm);
    }

    /* Each frame's r10 points at its own stack end, one frame apart.  The
     * callee's r10 is UBPF_STACK_SIZE bytes above the caller's. */
    {
        struct ebpf_inst prog[] = {
            mov64_reg(6, 10),       /* 0 */
            local_call(3),          /* 1 -> 5: f */
            sub64_reg(0, 6),        /* 2: callee r10 - caller r10 */
            exit_inst(),            /* 3 */
            exit_inst(),            /* 4: unreachable */
            mov64_reg(0, 10),       /* 5: f */
            exit_inst(),            /* 6 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        size_t frame_size = (UBPF_STACK_SIZE + 7) / 8 * 8;
        assert(exec_ok(vm, NULL, 0) == frame_size);
        n++;
        ubpf_destroy(vm);
    }

    /* Nested calls: c returns 5, b adds 4, a adds 2.  Entry returns 11. */
    {
        struct ebpf_inst prog[] = {
            local_call(3),          /* 0 -> 4: a */
            mov64_reg(6, 0),        /* 1 */
            mov64_reg(0, 6),        /* 2 */
            exit_inst(),            /* 3 */
            mov64(0, 1),            /* 4: a */
            local_call(2),          /* 5 -> 8: b */
            add64_imm(0, 2),        /* 6 */
            exit_inst(),            /* 7 */
            mov64(0, 3),            /* 8: b */
            local_call(2),          /* 9 -> 12: c */
            add64_imm(0, 4),        /* 10 */
            exit_inst(),            /* 11 */
            mov64(0, 5),            /* 12: c */
            exit_inst(),            /* 13 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        assert(exec_ok(vm, NULL, 0) == 11);
        n++;
        ubpf_destroy(vm);
    }

    /* Nested r6 restoration: entry 100 + (a's 200 + b's 300) = 600. */
    {
        struct ebpf_inst prog[] = {
            mov64(6, 100),          /* 0 */
            local_call(4),          /* 1 -> 6: a */
            add64_reg(6, 0),         /* 2 */
            mov64_reg(0, 6),        /* 3 */
            exit_inst(),            /* 4 */
            exit_inst(),            /* 5: unreachable, still in entry */
            mov64(6, 200),          /* 6: a */
            local_call(3),          /* 7 -> 11: b */
            add64_reg(6, 0),        /* 8 */
            mov64_reg(0, 6),        /* 9 */
            exit_inst(),            /* 10 */
            mov64(6, 300),          /* 11: b */
            mov64_reg(0, 6),        /* 12 */
            exit_inst(),            /* 13 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        /* Run twice: a second ubpf_exec must not inherit frame state. */
        assert(exec_ok(vm, NULL, 0) == 600);
        assert(exec_ok(vm, NULL, 0) == 600);
        n++;
        ubpf_destroy(vm);
    }

    /* A shared callee invoked twice gets a fresh frame each time and only
     * observes its own stack.  Result 1 + 1 = 2. */
    {
        struct ebpf_inst prog[] = {
            local_call(4),          /* 0 -> 5: f */
            mov64_reg(6, 0),        /* 1 */
            local_call(2),          /* 2 -> 5: f */
            add64_reg(0, 6),        /* 3 */
            exit_inst(),            /* 4 */
            stdw_imm(10, -8, 1),    /* 5: f */
            ldxdw(0, 10, -8),       /* 6 */
            exit_inst(),            /* 7 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        assert(exec_ok(vm, NULL, 0) == 2);
        n++;
        ubpf_destroy(vm);
    }

    /* A failure inside a nested frame must not leave a stale return address
     * or frame depth: after disabling bounds checking the same VM runs the
     * program again from a clean state.  The callee stores and reloads a
     * value at an address that crosses the boundary between the two active
     * frames -- rejected with bounds checking, accepted without it, and the
     * value is the one it explicitly wrote. */
    {
        struct ebpf_inst prog[] = {
            local_call(1),              /* 0 -> 2: f */
            exit_inst(),                /* 1 */
            stdw_imm(10, -516, 0x1234), /* 2: f, crosses frame 0/frame 1 */
            ldxdw(0, 10, -516),         /* 3 */
            exit_inst(),                /* 4 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        exec_fail(vm, NULL, 0);                 /* bounds on: rejected */
        ubpf_toggle_bounds_check(vm, false);
        assert(exec_ok(vm, NULL, 0) == 0x1234); /* clean re-execution */
        n++;
        ubpf_destroy(vm);
    }

    return n;
}

/* ====================================================================== */
/* Contract 3: bounds checking with per-frame stacks.                     */
/* ====================================================================== */

int
contract_stack_bounds(void)
{
    int n = 0;

    /* A callee may read an ancestor's stack through an argument pointer. */
    {
        struct ebpf_inst prog[] = {
            stdw_imm(10, -8, 0x1234),   /* 0 */
            mov64_reg(1, 10),           /* 1 */
            add64_imm(1, -8),           /* 2 */
            local_call(1),              /* 3 -> 5: f */
            exit_inst(),                /* 4 */
            ldxdw(0, 1, 0),             /* 5: f, ancestor stack */
            exit_inst(),                /* 6 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        assert(exec_ok(vm, NULL, 0) == 0x1234);
        n++;
        ubpf_destroy(vm);
    }

    /* A callee may also use its own stack and the original host memory. */
    {
        uint64_t mem[3] = { 0xAAAA0000ULL, 0xBBBB0000ULL, 0xCCCC0000ULL };
        struct ebpf_inst prog[] = {
            mov64_reg(6, 1),            /* 0: save host pointer */
            local_call(2),              /* 1 -> 4: f */
            exit_inst(),                /* 2 */
            exit_inst(),                /* 3: unreachable */
            mov64_reg(1, 6),            /* 4: f */
            stdw_imm(10, -8, 0xBB),     /* 5: f's own stack */
            ldxdw(0, 10, -8),           /* 6 */
            ldxdw(2, 1, 0),             /* 7: host memory */
            add64_reg(0, 2),            /* 8 */
            exit_inst(),                /* 9 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        assert(exec_ok(vm, mem, sizeof(mem)) == 0xBB + 0xAAAA0000ULL);
        n++;
        ubpf_destroy(vm);
    }

    /* A callee can write host memory, and the deterministic content is
     * visible in the caller's buffer after execution.  Entry passes a
     * pointer to mem[1]; the callee stores a computed value there. */
    {
        uint64_t mem[3] = { 0x11111111ULL, 0x22222222ULL, 0x33333333ULL };
        /* Exact layout, entry [0,6), f [6,10). */
        struct ebpf_inst prog3[] = {
            mov64_reg(6, 1),
            mov64(1, 0x55AA),
            mov64(2, 0x99),
            local_call(2),
            mov64(0, 0x1234),
            exit_inst(),
            stxdw(6, 1, 8),
            stxdw(6, 2, 16),
            mov64(0, 7),
            exit_inst(),
        };
        struct ubpf_vm *vm = load_ok(prog3, sizeof(prog3)/sizeof(prog3[0]));
        assert(exec_ok(vm, mem, sizeof(mem)) == 0x1234);
        assert(mem[0] == 0x11111111ULL);
        assert(mem[1] == 0x55AA);
        assert(mem[2] == 0x99);
        n++;
        ubpf_destroy(vm);
    }

    /* An access whose interval straddles the boundary between two active
     * frames is not contained in any single region and must fail, even
     * though the stack pool as a whole covers it. */
    {
        struct ebpf_inst prog[] = {
            mov64_reg(1, 10),           /* 0 */
            add64_imm(1, -4),           /* 1: caller's r10 - 4 */
            local_call(1),              /* 2 -> 4: f */
            exit_inst(),                /* 3 */
            ldxdw(0, 1, 0),             /* 4: f, spans frame 0/frame 1 */
            exit_inst(),                /* 5 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        exec_fail(vm, NULL, 0);
        n++;
        ubpf_destroy(vm);
    }

    /* A returned frame stops being an active region: a callee returns the
     * address of its own stack slot and the caller immediately reads it.  The
     * callee explicitly writes the slot before returning, so the value is
     * defined once bounds checking allows the access. */
    {
        struct ebpf_inst prog[] = {
            local_call(3),              /* 0 -> 4: f */
            mov64_reg(1, 0),            /* 1: r1 = returned stack address */
            ldxdw(0, 1, 0),             /* 2: reads a returned frame */
            exit_inst(),                /* 3 */
            stdw_imm(10, -8, 0x7777),   /* 4: f writes its own slot */
            mov64_reg(0, 10),           /* 5 */
            add64_imm(0, -8),           /* 6 */
            exit_inst(),                /* 7 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        exec_fail(vm, NULL, 0);                 /* bounds on: rejected */
        ubpf_toggle_bounds_check(vm, false);
        assert(exec_ok(vm, NULL, 0) == 0x7777); /* prior state not reused */
        n++;
        ubpf_destroy(vm);
    }

    /* Accesses exactly at the lower edge of a callee frame are allowed;
     * one byte below the edge is not. */
    {
        struct ebpf_inst prog[] = {
            local_call(1),              /* 0 -> 2: f */
            exit_inst(),                /* 1 */
            stdw_imm(10, -512, 5),      /* 2: f, lowest valid slot */
            stdw_imm(10, -8, 7),        /* 3 */
            ldxdw(0, 10, -512),         /* 4 */
            ldxdw(1, 10, -8),           /* 5 */
            add64_reg(0, 1),            /* 6 */
            exit_inst(),                /* 7 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        assert(exec_ok(vm, NULL, 0) == 12);
        n++;
        ubpf_destroy(vm);
    }
    /* A callee-computed address that falls below the whole stack pool is
     * rejected.  With two active frames the pool starts at frame 0's
     * address; r10 - 1025 is one byte before it. */
    {
        struct ebpf_inst prog[] = {
            local_call(1),              /* 0 -> 2: f */
            exit_inst(),                /* 1 */
            mov64_reg(1, 10),           /* 2: f */
            add64_imm(1, -(UBPF_STACK_SIZE + 1)),
            ldxdw(0, 1, 0),             /* 4 */
            exit_inst(),                /* 5 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        exec_fail(vm, NULL, 0);
        n++;
        ubpf_destroy(vm);
    }

    /* An access at r10 itself is one byte past the frame and must fail. */
    {
        struct ebpf_inst prog[] = {
            local_call(1),              /* 0 -> 2: f */
            exit_inst(),                /* 1 */
            ldxdw(0, 10, 0),            /* 2: f, at the frame end */
            exit_inst(),                /* 3 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        exec_fail(vm, NULL, 0);
        n++;
        ubpf_destroy(vm);
    }

    /* Host memory: the final eight bytes are accessible, one past the end is
     * not, and neither is one word before the start. */
    {
        uint64_t mem[3] = { 0x11111111ULL, 0x22222222ULL, 0x33333333ULL };
        struct ebpf_inst prog[] = {
            mov64_reg(6, 1),            /* 0 */
            local_call(2),              /* 1 -> 4: f */
            exit_inst(),                /* 2 */
            exit_inst(),                /* 3 */
            mov64_reg(1, 6),            /* 4: f */
            ldxdw(0, 1, 16),            /* 5: last 8 bytes */
            exit_inst(),                /* 6 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        assert(exec_ok(vm, mem, sizeof(mem)) == 0x33333333ULL);
        n++;
        ubpf_destroy(vm);
    }
    {
        uint64_t mem[3] = { 0 };
        struct ebpf_inst prog[] = {
            mov64_reg(6, 1),            /* 0 */
            local_call(2),              /* 1 -> 4: f */
            exit_inst(),                /* 2 */
            exit_inst(),                /* 3 */
            mov64_reg(1, 6),            /* 4: f */
            ldxdw(0, 1, 24),            /* 5: one word past the end */
            exit_inst(),                /* 6 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        exec_fail(vm, mem, sizeof(mem));
        n++;
        ubpf_destroy(vm);
    }
    {
        uint64_t mem[3] = { 0 };
        struct ebpf_inst prog[] = {
            mov64_reg(6, 1),            /* 0 */
            local_call(2),              /* 1 -> 4: f */
            exit_inst(),                /* 2 */
            exit_inst(),                /* 3 */
            mov64_reg(1, 6),            /* 4: f */
            ldxdw(0, 1, -8),            /* 5: one word before the start */
            exit_inst(),                /* 6 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        exec_fail(vm, mem, sizeof(mem));
        n++;
        ubpf_destroy(vm);
    }

    /* Address arithmetic that wraps at the top of the address space must not
     * be treated as a small in-range interval. */
    {
        struct ebpf_inst prog[] = {
            local_call(1),              /* 0 -> 2: f */
            exit_inst(),                /* 1 */
            mov64(1, -1),               /* 2: f, r1 = all ones */
            ldxdw(0, 1, 0),             /* 3: base + 8 wraps */
            exit_inst(),                /* 4 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        exec_fail(vm, NULL, 0);
        n++;
        ubpf_destroy(vm);
    }

    return n;
}

/* ====================================================================== */
/* Contract 4: helper separation, unwind, JIT rejection and reuse.        */
/* ====================================================================== */

int
contract_helpers_jit(void)
{
    int n = 0;

    /* A local call whose imm happens to equal a registered helper index must
     * dispatch to the local function, never the helper.  f returns 7; the
     * helper at index 3 would return 999. */
    {
        struct ebpf_inst prog[] = {
            local_call(3),          /* 0 -> 4 */
            exit_inst(),            /* 1 */
            exit_inst(),            /* 2 */
            exit_inst(),            /* 3 */
            mov64(0, 7),            /* 4: f */
            exit_inst(),            /* 5 */
        };
        struct ubpf_vm *vm = ubpf_create();
        assert(vm != NULL);
        helper_calls = 0;
        assert(ubpf_register(vm, 3, "colliding_helper", helper_seven) == 0);
        char *errmsg = NULL;
        assert(ubpf_load(vm, prog, sizeof(prog), &errmsg) == 0);
        assert(exec_ok(vm, NULL, 0) == 7);
        assert(helper_calls == 0);
        n++;
        ubpf_destroy(vm);
        free(errmsg);
    }

    /* src == 0 helper calls still work inside a program that also contains a
     * local call.  f returns 7, then the helper returns r1 + 7 = 12. */
    {
        struct ebpf_inst prog[] = {
            mov64(1, 5),            /* 0 */
            local_call(2),          /* 1 -> 4: f */
            helper_call(0),         /* 2: helper index 0 */
            exit_inst(),            /* 3 */
            mov64(0, 7),            /* 4: f */
            exit_inst(),            /* 5 */
        };
        struct ubpf_vm *vm = ubpf_create();
        assert(vm != NULL);
        helper_calls = 0;
        assert(ubpf_register(vm, 0, "seven", helper_seven) == 0);
        char *errmsg = NULL;
        assert(ubpf_load(vm, prog, sizeof(prog), &errmsg) == 0);
        assert(exec_ok(vm, NULL, 0) == 12);
        assert(helper_calls == 1);
        n++;
        ubpf_destroy(vm);
        free(errmsg);
    }

    /* An unwind helper returning 0 in a nested function ends the whole
     * interpretation instead of popping just one frame.  f's remaining
     * instruction never runs. */
    {
        struct ebpf_inst prog[] = {
            local_call(2),          /* 0 -> 3: f */
            mov64(0, 555),          /* 1 */
            exit_inst(),            /* 2 */
            mov64(1, 0),            /* 3: f */
            helper_call(2),         /* 4: unwind helper returns 0 */
            mov64(0, 999),          /* 5: must not run */
            exit_inst(),            /* 6 */
        };
        struct ubpf_vm *vm = ubpf_create();
        assert(vm != NULL);
        unwind_calls = 0;
        assert(ubpf_register(vm, 2, "unwind", helper_unwind) == 0);
        assert(ubpf_set_unwind_function_index(vm, 2) == 0);
        char *errmsg = NULL;
        assert(ubpf_load(vm, prog, sizeof(prog), &errmsg) == 0);
        assert(exec_ok(vm, NULL, 0) == 0);
        assert(unwind_calls == 1);
        n++;
        ubpf_destroy(vm);
        free(errmsg);
    }

    /* The same helper returning nonzero continues normally; f returns 999. */
    {
        struct ebpf_inst prog[] = {
            local_call(2),          /* 0 -> 3: f */
            exit_inst(),            /* 1 */
            exit_inst(),            /* 2 */
            mov64(1, 1),            /* 3: f */
            helper_call(2),         /* 4 */
            mov64(0, 999),          /* 5 */
            exit_inst(),            /* 6 */
        };
        struct ubpf_vm *vm = ubpf_create();
        assert(vm != NULL);
        unwind_calls = 0;
        assert(ubpf_register(vm, 2, "unwind", helper_unwind) == 0);
        assert(ubpf_set_unwind_function_index(vm, 2) == 0);
        char *errmsg = NULL;
        assert(ubpf_load(vm, prog, sizeof(prog), &errmsg) == 0);
        assert(exec_ok(vm, NULL, 0) == 999);
        assert(unwind_calls == 1);
        n++;
        ubpf_destroy(vm);
        free(errmsg);
    }

    /* ubpf_translate and ubpf_compile reject any local call, report its
     * position, and do not cache a compiled version.  Interpretation is still
     * possible after the rejection. */
    {
        struct ebpf_inst prog[] = {
            mov64(0, 0),            /* 0 */
            mov64(0, 0),            /* 1 */
            local_call(1),          /* 2 -> 4: f */
            exit_inst(),            /* 3 */
            mov64(0, 7),            /* 4: f */
            exit_inst(),            /* 5 */
        };
        struct ubpf_vm *vm = load_ok(prog, sizeof(prog)/sizeof(prog[0]));
        uint8_t buffer[4096];
        size_t size = sizeof(buffer);
        char *errmsg = NULL;
        assert(ubpf_translate(vm, buffer, &size, &errmsg) < 0);
        assert(errmsg != NULL && strstr(errmsg, "local call") != NULL);
        assert(strstr(errmsg, "PC 2") != NULL);
        free(errmsg);
        errmsg = NULL;

        assert(ubpf_compile(vm, &errmsg) == NULL);
        assert(errmsg != NULL && strstr(errmsg, "local call") != NULL);
        assert(strstr(errmsg, "PC 2") != NULL);
        free(errmsg);
        errmsg = NULL;

        /* No cached local-call version: a second attempt still fails. */
        assert(ubpf_compile(vm, &errmsg) == NULL);
        assert(errmsg != NULL && strstr(errmsg, "local call") != NULL);
        free(errmsg);

        /* Interpretation still works after the translation was refused. */
        assert(exec_ok(vm, NULL, 0) == 7);

        /* Unloading and loading a plain program makes translation work
         * again. */
        ubpf_unload_code(vm);
        struct ebpf_inst plain[] = { mov64(0, 42), exit_inst() };
        errmsg = NULL;
        assert(ubpf_load(vm, plain, sizeof(plain), &errmsg) == 0);
        size = sizeof(buffer);
        assert(ubpf_translate(vm, buffer, &size, &errmsg) == 0);
        assert(size > 0);
        assert(exec_ok(vm, NULL, 0) == 42);

        /* Reloading a different local-call program also finds the correct
         * function boundaries and frame depth from scratch. */
        ubpf_unload_code(vm);
        struct ebpf_inst local2[] = {
            local_call(1),          /* 0 -> 2: f */
            exit_inst(),            /* 1 */
            mov64(0, 5),            /* 2: f */
            exit_inst(),            /* 3 */
        };
        errmsg = NULL;
        assert(ubpf_load(vm, local2, sizeof(local2), &errmsg) == 0);
        assert(exec_ok(vm, NULL, 0) == 5);
        n++;
        ubpf_destroy(vm);
    }

    /* Registered helper indices keep working for src == 0 helper calls. */
    {
        struct ebpf_inst prog[] = {
            mov64(1, 0),
            helper_call(0),
            exit_inst(),
        };
        struct ubpf_vm *vm = ubpf_create();
        assert(vm != NULL);
        helper_calls = 0;
        assert(ubpf_register(vm, 0, "seven", helper_seven) == 0);
        char *errmsg = NULL;
        assert(ubpf_load(vm, prog, sizeof(prog), &errmsg) == 0);
        assert(exec_ok(vm, NULL, 0) == 7);
        assert(helper_calls == 1);
        n++;
        ubpf_destroy(vm);
        free(errmsg);
    }

    return n;
}
