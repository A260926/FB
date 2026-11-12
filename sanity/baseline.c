#include <wavpack.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int out(void*id,void*b,int32_t n){return fwrite(b,1,n,id)==(size_t)n;}
int main(void){
 FILE*f=fopen("sample.wv","wb"),*fc=fopen("sample.wvc","wb");assert(f&&fc);
 WavpackContext*w=WavpackOpenFileOutput(out,f,fc);assert(w);
 WavpackConfig c={0};c.num_channels=3;c.channel_mask=7;c.sample_rate=48000;
 c.bits_per_sample=16;c.bytes_per_sample=2;c.block_samples=16;c.bitrate=2.5;
 c.flags=CONFIG_HYBRID_FLAG|CONFIG_CREATE_WVC;
 assert(WavpackSetConfiguration64(w,&c,33,NULL));assert(WavpackPackInit(w));
 int32_t pcm[99],original[99];
 for(int i=0;i<99;i++)pcm[i]=((i*1103515245u+12345u)>>16)%60001-30000;
 memcpy(original,pcm,sizeof(pcm));assert(WavpackPackSamples(w,pcm,33));assert(WavpackFlushSamples(w));
 WavpackCloseFile(w);fclose(f);fclose(fc);char error[80];int different=0;
 for(int corrected=0;corrected<2;corrected++){
  WavpackContext*r=WavpackOpenFileInput("sample.wv",error,corrected?OPEN_WVC:0,0);assert(r);
  int32_t decoded[99];assert(WavpackUnpackSamples(r,decoded,33)==33);
  if(corrected)assert(!memcmp(decoded,original,sizeof(decoded)));
  else for(int i=0;i<99;i++)different+=decoded[i]!=original[i];
  assert(WavpackGetNumErrors(r)==0);assert(WavpackGetSampleIndex64(r)==33);WavpackCloseFile(r);
 }
 assert(different>0);printf("BASELINE hybrid_frames=33 channels=3 corrected_exact=1 lossy_diff=%d\n",different);return 0;
}
