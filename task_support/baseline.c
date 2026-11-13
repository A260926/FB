#include "wavpack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FRAMES 1025
#define CHANNELS 6
static int32_t original[FRAMES*CHANNELS],work[FRAMES*CHANNELS],out[13*CHANNELS];
static int blocks;
static int write_block(void *id,void *data,int32_t count){blocks++;return fwrite(data,1,count,(FILE*)id)==(size_t)count;}
int main(void){
 for(int f=0;f<FRAMES;f++)for(int c=0;c<CHANNELS;c++)original[f*CHANNELS+c]=((f*7919+c*137)%1200000)-600000+(c&1?f*11:-f*7);
 memcpy(work,original,sizeof(work));FILE *file=fopen(".task-build/baseline.wv","wb");if(!file)return 1;
 WavpackContext *w=WavpackOpenFileOutput(write_block,file,NULL);if(!w)return 2;
 WavpackConfig cfg;memset(&cfg,0,sizeof(cfg));cfg.bits_per_sample=24;cfg.bytes_per_sample=3;cfg.num_channels=CHANNELS;cfg.channel_mask=0x3f;cfg.sample_rate=48000;cfg.block_samples=128;cfg.flags=CONFIG_HIGH_FLAG;
 if(!WavpackSetConfiguration64(w,&cfg,FRAMES,NULL)||!WavpackPackInit(w)||!WavpackPackSamples(w,work,FRAMES)||!WavpackFlushSamples(w))return 3;
 WavpackCloseFile(w);if(fclose(file))return 4;
 char error[80];WavpackContext *d=WavpackOpenFileInput(".task-build/baseline.wv",error,0,0);if(!d){fprintf(stderr,"%s\n",error);return 5;}
 unsigned total=0,n;while((n=WavpackUnpackSamples(d,out,13))>0){
  if(total+n>FRAMES)return 6;
  for(unsigned j=0;j<n*CHANNELS;j++)if(out[j]!=original[total*CHANNELS+j])return 7;
  total+=n;
 }
 if(total!=FRAMES||WavpackGetNumErrors(d)||WavpackGetNumChannels(d)!=CHANNELS)return 8;
 if(!WavpackSeekSample64(d,501)||WavpackUnpackSamples(d,out,7)!=7)return 9;
 for(int i=0;i<7*CHANNELS;i++)if(out[i]!=original[501*CHANNELS+i])return 10;
 printf("WAVPACK_BASELINE frames=%u channels=6 depth=24 physical_blocks=%d errors=%d seek=501\n",total,blocks,WavpackGetNumErrors(d));WavpackCloseFile(d);return 0;
}
