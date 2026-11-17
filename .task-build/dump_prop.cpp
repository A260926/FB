#include "glsl_optimizer.h"
#include "nir_optimize.h"
#include "compiler/nir/nir.h"
#include "glsl_parser_extras.h"
#include <cstdio>
#include "../src/glsl/glsl_optimizer.cpp"

int main(){
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   glslopt_shader* result = new glslopt_shader;
   gl_shader* shader = result->shader;
   shader->Stage = MESA_SHADER_FRAGMENT;
   const char* src =
      "#version 450 core\nuniform vec4 a,b; layout(location=0) out vec4 o;\n"
      "void main(){ vec4 x=a*b; vec4 y=x.zywx; precise vec4 r=-y+a; o=r; }\n";
   shader->Source = ralloc_strdup(shader, src);
   parse_info parsed;
   _mesa_glsl_compile_shader_glslopt(&ctx->mesa_ctx, shader, false, capture_parse_state, &parsed);
   glslopt_lower_nir(shader->nir);
   std::puts("== after lower, before precise:");
   nir_print_shader(shader->nir, stdout);
   glslopt_propagate_precise(shader->nir);
   std::puts("== after precise propagation:");
   nir_print_shader(shader->nir, stdout);
   glslopt_shader_delete(result);
   glslopt_cleanup(ctx);
}
