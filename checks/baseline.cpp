#include "glsl_optimizer.h"
#include <cassert>
#include <cstdio>
#include <cstring>
int main(){
 const char* vs="#version 330\nin vec4 p;out vec2 uv;out float fog;out vec3 unusedColor;void main(){gl_Position=p;uv=p.xy;fog=p.z;unusedColor=p.xyz;}";
 const char* fs="#version 330\nin vec2 uv;in float fog;out vec4 color;void main(){color=vec4(uv,fog,1.0);}";
 auto c=glslopt_initialize(kGlslTargetOpenGL);
 auto v=glslopt_optimize(c,kGlslOptShaderVertex,vs,0); auto f=glslopt_optimize(c,kGlslOptShaderFragment,fs,0);
 if(!glslopt_get_status(v))std::puts(glslopt_get_log(v));if(!glslopt_get_status(f))std::puts(glslopt_get_log(f));
 assert(glslopt_get_status(v)&&glslopt_get_status(f));
 assert(glslopt_shader_get_input_count(f)==2);
 assert(std::strstr(glslopt_get_output(v),"unusedColor"));
 std::puts(glslopt_get_output(v));std::puts(glslopt_get_output(f));
 glslopt_shader_delete(v);glslopt_shader_delete(f);glslopt_cleanup(c);
 std::puts("SEPARATE_STAGE inputs=2 unused_vertex_output=retained");std::puts("BASELINE_OK");
}
