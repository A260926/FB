#include <spvm/context.h>
#include <spvm/state.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "constants_module.h"
int main(void) {
    spvm_word words[512]; size_t n=constants_module(words);
    spvm_context_t c=spvm_context_initialize();
    spvm_program_t p=spvm_program_create(c,words,n); assert(p);
    spvm_state_t s=spvm_state_create(p); assert(s);
    spvm_result_t v=&s->results[22];
    assert(v->member_count==3 && v->members[0].member_count==3);
    assert(v->members[1].member_count==2);
    assert(v->members[0].members[1].value.u==11);
    uint32_t bits; memcpy(&bits,&v->members[2].value.f,4); assert(bits==0x80000000u);
    spvm_state_prepare(s,40); spvm_state_call_function(s);
    assert(s->results[43].members[0].value.u==11);
    assert(s->results[25].members[0].value.u==18);
    printf("ORDINARY_AGGREGATE output=%u array=%d helper=%u negative_zero=%08x\n",
      s->results[25].members[0].value.u,v->members[0].member_count,
      s->results[43].members[0].value.u,(unsigned)bits);
    spvm_state_delete(s); spvm_program_delete(p); spvm_context_deinitialize(c);
    puts("ORDINARY_AGGREGATE_OK"); return 0;
}
