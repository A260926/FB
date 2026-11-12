/*
 * Example: local (intra-bytecode) function calls in the uBPF interpreter.
 *
 * A CALL instruction with src == 1 calls a function inside the same bytecode
 * instead of a registered C helper.  The target is
 * (call_pc + 1 + imm) in 8-byte instruction slots; LDDW occupies two slots.
 *
 * The program below has three functions in one instruction array:
 *
 *   entry  (pc 0):  add_two(30, 12), which returns 30 + (double(12)) + 30
 *                   = 66
 *   add_two(pc 5):  r6 = r1 + r2, then r0 = double(r2) + r6
 *   double (pc 11): r0 = r1 * 2
 *
 * Build with:
 *   cc -I vm/inc example.c vm/ubpf_vm.c vm/ubpf_jit_x86_64.c \
 *      vm/ubpf_local_calls.c -lm -o example
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ubpf.h"
#include "ebpf.h"

static struct ebpf_inst
local_call(int32_t imm)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_CALL, .src = 1, .imm = imm };
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
alu64(uint8_t opcode, int dst, int src)
{
    struct ebpf_inst inst = { .opcode = opcode, .dst = dst, .src = src };
    return inst;
}

static struct ebpf_inst
exit_inst(void)
{
    struct ebpf_inst inst = { .opcode = EBPF_OP_EXIT };
    return inst;
}

int
main(void)
{
    struct ebpf_inst program[] = {
        /* entry */
        mov64(1, 30),               /*  0 */
        mov64(2, 12),               /*  1 */
        local_call(2),              /*  2 -> 5: add_two */
        exit_inst(),                /*  3 */
        exit_inst(),                /*  4: padding, never runs */

        /* add_two: r0 = r1 + r2; returns r0 + double(r2) */
        mov64_reg(6, 1),            /*  5 */
        alu64(EBPF_OP_ADD64_REG, 6, 2),   /*  6: r6 = 42 */
        mov64_reg(1, 2),            /*  7 */
        local_call(2),              /*  8 -> 11: double */
        alu64(EBPF_OP_ADD64_REG, 0, 6),   /*  9 */
        exit_inst(),                /* 10 */

        /* double: r0 = r1 * 2 */
        mov64_reg(0, 1),            /* 11 */
        { .opcode = EBPF_OP_LSH64_IMM, .dst = 0, .imm = 1 },  /* 12 */
        exit_inst(),                /* 13 */
    };

    struct ubpf_vm *vm = ubpf_create();
    if (vm == NULL) {
        fprintf(stderr, "failed to create VM\n");
        return 1;
    }

    char *errmsg = NULL;
    if (ubpf_load(vm, program, sizeof(program), &errmsg) != 0) {
        fprintf(stderr, "failed to load: %s\n", errmsg);
        free(errmsg);
        ubpf_destroy(vm);
        return 1;
    }

    uint64_t result = 0;
    if (ubpf_exec(vm, NULL, 0, &result) != 0) {
        fprintf(stderr, "execution failed\n");
        ubpf_destroy(vm);
        return 1;
    }

    printf("result = %llu\n", (unsigned long long)result);

    /* JIT translation deliberately refuses programs with local calls. */
    uint8_t buffer[4096];
    size_t size = sizeof(buffer);
    free(errmsg);
    errmsg = NULL;
    if (ubpf_translate(vm, buffer, &size, &errmsg) != 0) {
        printf("translate refused: %s\n", errmsg);
    }
    free(errmsg);

    ubpf_destroy(vm);
    return 0;
}
