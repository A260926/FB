#include "tinyshader.h"
#include "spirv.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
static int compile(const char *src,TsShaderStage stage,int inspect,const char *path) {
 TsCompilerOptions *o=tsCompilerOptionsCreate();
 tsCompilerOptionsSetStage(o,stage);tsCompilerOptionsSetEntryPoint(o,"main",4);
 tsCompilerOptionsSetSource(o,src,strlen(src),path,strlen(path));
 TsCompilerOutput *r=tsCompile(o);const char *e=tsCompilerOutputGetErrors(r);
 if(e){fprintf(stderr,"%s\n",e);tsCompilerOutputDestroy(r);tsCompilerOptionsDestroy(o);return 0;}
 size_t bytes=0;const unsigned char *raw=tsCompilerOutputGetSpirv(r,&bytes);
 assert(raw && bytes>=20 && bytes%4==0);const uint32_t *w=(const uint32_t*)raw;
 size_t implicit=0,explicit=0,sampled=0;
 for(size_t at=5;at<bytes/4;) {
  unsigned n=w[at]>>16,op=w[at]&0xffff;assert(n && at+n<=bytes/4);
  if(op==SpvOpImageSampleImplicitLod){assert(n==5);++implicit;}
  if(op==SpvOpImageSampleExplicitLod){assert(n==7 && w[at+5]==SpvImageOperandsLodMask);++explicit;}
  if(op==SpvOpSampledImage)++sampled;
  at+=n;
 }
 if(inspect){assert(implicit==1 && explicit==1 && sampled==2);puts("ORDINARY_SAMPLING implicit=1 explicit=1 combined=2");}
 tsCompilerOutputDestroy(r);tsCompilerOptionsDestroy(o);return 1;
}
static char *read_source(const char *path) {
 FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);assert(n>=0);rewind(f);
 char *s=malloc((size_t)n+1);assert(s && fread(s,1,(size_t)n,f)==(size_t)n);s[n]=0;fclose(f);return s;
}
int main(void) {
 const char *normal="Texture2D<float4> tex; SamplerState samp; float4 main(float2 uv:TEXCOORD0):SV_Target {return tex.Sample(samp,uv)+tex.SampleLevel(samp,uv,1.0);}";
 assert(compile(normal,TS_SHADER_STAGE_FRAGMENT,1,"checks/ordinary.hlsl"));
 const char *paths[]={"tests/valid/test.frag.hlsl","tests/valid/test.vert.hlsl"};
 for(int i=0;i<2;i++){char *s=read_source(paths[i]);assert(compile(s,i?TS_SHADER_STAGE_VERTEX:TS_SHADER_STAGE_FRAGMENT,0,paths[i]));free(s);printf("UPSTREAM_SAMPLE_OK %s\n",paths[i]);}
 puts("SAMPLING_BASELINE_OK");return 0;
}
