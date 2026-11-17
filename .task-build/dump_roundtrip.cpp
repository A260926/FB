#include "glsl_optimizer.h"
#include "nir_optimize.h"
#include "compiler/nir/nir.h"
#include "glsl_parser_extras.h"
#include <cstdio>
#include <cstring>

#include "../src/glsl/glsl_optimizer.cpp"

static int roundtrip(glslopt_ctx* ctx, const char* tag, const char* src) {
   glslopt_shader* s = glslopt_optimize(ctx, kGlslOptShaderFragment, src, 0);
   if (!glslopt_get_status(s)) { std::printf("[%s] FAIL: %s\n", tag, glslopt_get_log(s)); return 1; }
   const char* out = glslopt_get_output(s);
   std::printf("==== %s first:\n%s", tag, out);
   glslopt_shader* s2 = glslopt_optimize(ctx, kGlslOptShaderFragment, out, 0);
   if (!glslopt_get_status(s2)) { std::printf("[%s] RECOMPILE FAIL: %s\n", tag, glslopt_get_log(s2)); return 1; }
   const char* out2 = glslopt_get_output(s2);
   std::printf("---- %s recompiled:\n%s", tag, out2);
   // second compilation must keep the precise keyword
   int ok = std::strstr(out2, "precise") != nullptr;
   std::printf("[%s] recompiled-keeps-precise=%d\n\n", tag, ok);
   glslopt_shader_delete(s);
   glslopt_shader_delete(s2);
   return ok ? 0 : 1;
 }

int main() {
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   int rc = 0;
   rc |= roundtrip(ctx, "basic",
      "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=(a*b)+c; o=r; }\n");
   rc |= roundtrip(ctx, "loop",
      "#version 450 core\nuniform float a; uniform int n; layout(location=0) out float o;\n"
      "void main(){ float s=0.0; for(int i=0;i<n;i++){ s=s+a; } precise float r=s; o=r; }\n");

   // interleave: precise shader, then normal shader in the same context:
   // the normal output must contain no precise keyword and no exact flags.
   const char* normal =
      "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ o=(a*b)+(a*c); }\n";
   glslopt_shader* n1 = glslopt_optimize(ctx, kGlslOptShaderFragment, normal, 0);
   const char* no = glslopt_get_output(n1);
   std::printf("==== interleaved normal:\n%s", no);
   bool clean = std::strstr(no, "precise") == nullptr;
   std::printf("[interleave] normal-has-no-precise=%d\n", clean);
   rc |= !clean;
   glslopt_shader_delete(n1);

   glslopt_cleanup(ctx);
   return rc;
}
