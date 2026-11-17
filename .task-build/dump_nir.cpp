#include "glsl_optimizer.h"
#include "nir_optimize.h"
#include "compiler/nir/nir.h"
#include "glsl_parser_extras.h"
#include <cstdio>
#include <cstring>

// pull in the internals the inspect bridge uses
#include "../src/glsl/glsl_optimizer.cpp"

static glslopt_shader* compile_raw(glslopt_ctx* ctx, glslopt_shader_type type, const char* src) {
   glslopt_shader* result = new glslopt_shader;
   gl_shader* shader = result->shader;
   shader->Stage = type == kGlslOptShaderVertex ? MESA_SHADER_VERTEX :
                   type == kGlslOptShaderFragment ? MESA_SHADER_FRAGMENT : MESA_SHADER_COMPUTE;
   shader->Source = ralloc_strdup(shader, src);
   parse_info parsed;
   _mesa_glsl_compile_shader_glslopt(&ctx->mesa_ctx, shader, false,
                                     capture_parse_state, &parsed);
   if (shader->CompileStatus == COMPILE_FAILURE || !shader->nir) {
      std::puts(shader->InfoLog);
      return nullptr;
   }
   return result;
}

static void dump(const char* tag, const char* src, glslopt_shader_type type, int stage) {
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   glslopt_shader* s = compile_raw(ctx, type, src);
   if (!s) return;
   nir_shader* nir = s->shader->nir;
   if (stage >= 1) glslopt_lower_nir(nir);
   std::printf("==== %s (stage=%d)\n", tag, stage);
   nir_print_shader(nir, stdout);
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
}

int main() {
   const char* precise =
      "#version 450 core\n"
      "uniform vec4 a,b,c; layout(location=0) out vec4 color;\n"
      "void main(){vec4 t=a*b; precise vec4 r=t+c; color=r;}\n";
   dump("precise raw", precise, kGlslOptShaderFragment, 0);
   dump("precise lowered", precise, kGlslOptShaderFragment, 1);
   return 0;
}
