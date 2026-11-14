#include "glsl_optimizer.h"
#include <cstdio>
#include <cstring>
int main(){
 const char* normal="#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 color; void main(){vec4 t=a*b; color=t+c;}\n";
 const char* precise="#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 color; void main(){vec4 t=a*b; precise vec4 r=t+c; color=r;}\n";
 glslopt_ctx* ctx=glslopt_initialize(kGlslTargetOpenGL);if(!ctx)return 1;
 int ok=1;
 for(int i=0;i<2;++i){auto* s=glslopt_optimize(ctx,kGlslOptShaderFragment,i?precise:normal,0);
 bool good=glslopt_get_status(s);const char* out=glslopt_get_output(s);
 std::printf("%s status=%d precise_keyword=%d\n",i?"precise":"normal",good,out&&std::strstr(out,"precise")!=nullptr);
 if(good&&out){std::puts(out);}else{std::puts(glslopt_get_log(s));}
 if(!good||!out||!*out)ok=0;
 glslopt_shader_delete(s);}
 glslopt_cleanup(ctx);return ok?0:2;
}
