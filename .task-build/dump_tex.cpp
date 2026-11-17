#include "glsl_optimizer.h"
#include "nir_optimize.h"
#include "compiler/nir/nir.h"
#include <cstdio>
#include "../src/glsl/glsl_optimizer.cpp"
int main(){
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* src =
      "#version 450 core\nuniform sampler2D t; uniform vec2 uv; uniform vec4 k; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=texture(t,uv)+k; o=r; }\n";
   glslopt_shader* s = glslopt_optimize(ctx, kGlslOptShaderFragment, src, 0);
   nir_foreach_block(block, nir_shader_get_entrypoint(s->shader->nir)) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_alu) {
            nir_alu_instr* a = nir_instr_as_alu(instr);
            std::printf("ALU %s fpctrl=%u\n", nir_op_infos[a->op].name, a->fp_math_ctrl);
         } else if (instr->type == nir_instr_type_tex) {
            std::printf("TEX (no fp_math_ctrl field)\n");
         } else if (instr->type == nir_instr_type_intrinsic) {
            nir_intrinsic_instr* i = nir_instr_as_intrinsic(instr);
            if (i->intrinsic == nir_intrinsic_glslopt_precise)
               std::printf("MARKER base=%u\n", nir_intrinsic_base(i));
         }
      }
   }
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
}
