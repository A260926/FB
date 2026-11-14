#include <spvm/context.h>
#include <spvm/state.h>
#include <spvm/ext/GLSL450.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>

int main(void){
 FILE *f=fopen("example/shader.spv","rb");assert(f);
 fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);assert(n>20 && n%4==0);
 spvm_word *w=malloc((size_t)n);assert(fread(w,1,(size_t)n,f)==(size_t)n);fclose(f);
 spvm_context_t c=spvm_context_initialize();
 spvm_program_t p=spvm_program_create(c,w,(size_t)n/4);assert(p);
 spvm_state_t s=spvm_state_create(p);
 spvm_ext_opcode_func *ext=spvm_build_glsl450_ext();spvm_state_set_extension(s,"GLSL.std.450",ext);
 spvm_result_t u=spvm_state_get_result(s,"uBlock");assert(u);
 spvm_member_t info=spvm_state_get_object_member(s,u,"info");
 spvm_member_t time=spvm_state_get_object_member(s,u,"time");
 float iv[2]={.2f,.8f},tv=.5f;spvm_member_set_value_f(info->members,info->member_count,iv);
 time->value.f=tv;
 spvm_state_prepare(s,spvm_state_get_result_location(s,"main"));spvm_state_call_function(s);
 spvm_result_t color=spvm_state_get_result(s,"outColor");assert(color && color->member_count==4);
 assert(fabsf(color->members[0].value.f-.2f)<1e-6f && fabsf(color->members[1].value.f-.8f)<1e-6f);
 printf("COLOR %.9f %.9f %.9f %.9f\n",color->members[0].value.f,color->members[1].value.f,color->members[2].value.f,color->members[3].value.f);
 assert(fabsf(color->members[2].value.f-fabsf(sinf(.5f)))<1e-6f && color->members[3].value.f==1);
 puts("OLD_SHADER_COLOR_OK");spvm_state_delete(s);spvm_program_delete(p);free(ext);free(w);
 spvm_context_deinitialize(c);
 return 0;
}
