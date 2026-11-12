#include "ubpf.h"
#include "ebpf.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned helper_calls;
static uint64_t helper(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; ++helper_calls; return 7; }
int main(void)
{
    /* All executed legacy paths return defined values and access no memory. */
    struct ebpf_inst program[] = {
        {.opcode=EBPF_OP_CALL, .src=1, .imm=1},
        {.opcode=EBPF_OP_EXIT},
        {.opcode=EBPF_OP_MOV64_IMM, .dst=0, .imm=42},
        {.opcode=EBPF_OP_EXIT}
    };
    struct ubpf_vm *vm=ubpf_create(); char *error=NULL; uint64_t value=0;
    assert(vm && ubpf_register(vm,1,"legacy_helper",helper)==0);
    assert(ubpf_load(vm,program,sizeof(program),&error)==0);
    assert(ubpf_exec(vm,NULL,0,&value)==0);
    /* This is an observation of the starting implementation, not a local-call implementation. */
    assert(value==7 && helper_calls==1);
    printf("UBPF_START_WITNESS src=1 result=%llu helper_calls=%u local_body=not_run\n",
           (unsigned long long)value,helper_calls);
    ubpf_destroy(vm); free(error); error=NULL;
    vm=ubpf_create(); assert(vm);
    assert(ubpf_load(vm,program,sizeof(program),&error)<0);
    assert(error && strstr(error,"nonexistent function 1 at PC 0"));
    puts("UBPF_START_NO_HELPER local_call_load=rejected");
    ubpf_destroy(vm); free(error); return 0;
}
