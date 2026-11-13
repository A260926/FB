#include <spvm/context.h>
#include <spvm/state.h>
#include <stdio.h>
static spvm_word words[]={0x07230203,0x00010000,0,24,0,
 (2u<<16)|SpvOpCapability,SpvCapabilityShader,
 (3u<<16)|SpvOpMemoryModel,SpvAddressingModelLogical,SpvMemoryModelGLSL450,
 (5u<<16)|SpvOpEntryPoint,SpvExecutionModelGLCompute,10,0x6e69616d,0,
 (6u<<16)|SpvOpExecutionMode,10,SpvExecutionModeLocalSize,2,1,1,
 (2u<<16)|SpvOpTypeVoid,1,
 (3u<<16)|SpvOpTypeFunction,2,1,
 (4u<<16)|SpvOpTypeInt,3,32,0,
 (4u<<16)|SpvOpTypePointer,4,SpvStorageClassPrivate,3,
 (4u<<16)|SpvOpConstant,3,5,7,
 (4u<<16)|SpvOpConstant,3,6,9,
 (4u<<16)|SpvOpVariable,4,7,SpvStorageClassPrivate,
 (5u<<16)|SpvOpFunction,1,10,0,2,
 (2u<<16)|SpvOpLabel,11,
 (5u<<16)|SpvOpIAdd,3,12,5,6,
 (3u<<16)|SpvOpStore,7,12,
 (1u<<16)|SpvOpReturn,
 (1u<<16)|SpvOpFunctionEnd};
int main(){spvm_context_t c=spvm_context_initialize();spvm_program_t p=spvm_program_create(c,words,sizeof(words)/sizeof(*words));spvm_state_t a=spvm_state_create(p),b=spvm_state_create(p);spvm_state_prepare(a,10);spvm_state_call_function(a);printf("single=%u independent=%u local_size=%u,%u,%u callbacks=%d,%d\n",a->results[7].members[0].value.u,b->results[7].members[0].value.u,p->local_size_x,p->local_size_y,p->local_size_z,p->allocate_workgroup_memory!=NULL,a->control_barrier!=NULL);int ok=a->results[7].members[0].value.u==16&&b->results[7].members[0].value.u==0;spvm_state_delete(a);spvm_state_delete(b);spvm_program_delete(p);spvm_context_deinitialize(c);if(!ok)return 1;puts("BASELINE_COMPUTE_SINGLE_OK");return 0;}
