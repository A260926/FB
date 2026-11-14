#include "tinyshader.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
static int compile(const char *src) {
 TsCompilerOptions *o=tsCompilerOptionsCreate();
 tsCompilerOptionsSetStage(o,TS_SHADER_STAGE_FRAGMENT);
 tsCompilerOptionsSetEntryPoint(o,"main",4);
 tsCompilerOptionsSetSource(o,src,strlen(src),NULL,0);
 TsCompilerOutput *r=tsCompile(o); const char *e=tsCompilerOutputGetErrors(r);
 size_t n=0; const unsigned char *s=tsCompilerOutputGetSpirv(r,&n);
 int ok=!e && s && n>=20; if(ok) printf("SPIRV_BYTES=%zu\n",n);
 else if(e) printf("COMPILE_REJECTED %s\n",e);
 tsCompilerOutputDestroy(r);tsCompilerOptionsDestroy(o);return ok;
}
int main(void){
 assert(compile("float4 main(float x:TEXCOORD0):SV_Target {int a=0;for(int i=0;i<3;i++){if(i==1)continue;a=a+i;}return float4(float(a),x,0.0,1.0);}"));

 puts("BASELINE_OK");return 0;
}
