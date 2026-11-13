#include "glsl_optimizer.h"
#include <cstdio>
#include <cstring>
int main(){auto*c=glslopt_initialize(kGlslTargetOpenGL); const char*s="#version 330 core\nuniform bool enabled; uniform float weights[3]; out vec4 color; void main(){color=enabled?vec4(weights[1]):vec4(0.0);}";auto*r=glslopt_optimize(c,kGlslOptShaderFragment,s,0);if(!r||!glslopt_get_status(r)){if(r)puts(glslopt_get_log(r));return 1;} puts(glslopt_get_output(r));printf("UNIFORMS=%d\n",glslopt_shader_get_uniform_count(r));if(glslopt_shader_get_uniform_count(r)!=2)return 2;glslopt_shader_delete(r);glslopt_cleanup(c);puts("BASELINE_UNSPECIALIZED_OK");}
