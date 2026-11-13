#include "foxen-flac.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
int main(void) {
 const char *paths[]={"test/data/short.flac","test/data/noise.flac","test/data/wasted_bits.flac"};
 for(int k=0;k<3;k++){
  FILE *f=fopen(paths[k],"rb"); assert(f); fseek(f,0,SEEK_END); long len=ftell(f); rewind(f);
  unsigned char *data=malloc(len); assert(fread(data,1,len,f)==(size_t)len); fclose(f);
  fx_flac_t *d=FX_FLAC_ALLOC_DEFAULT(); assert(d); size_t pos=0,count=0; uint64_t hash=1469598103934665603ULL;
  for(unsigned calls=0;calls<2000000;calls++){
   uint32_t n=(uint32_t)((size_t)len-pos),m=37; int32_t out[37];
   if(n>11)n=11;
   fx_flac_state_t state=fx_flac_process(d,data+pos,&n,out,&m); assert(state!=FLAC_ERR); pos+=n;
   for(uint32_t i=0;i<m;i++){hash^=(uint32_t)out[i];hash*=1099511628211ULL;} count+=m;
   if(pos==(size_t)len && !m && !n)break;
  }
  const size_t counts[]={4,44100,4410}; const uint64_t hashes[]={16409813063600517427ULL,7979690356161754163ULL,3199067534925364891ULL}; assert(count==counts[k] && hash==hashes[k]); printf("FOXEN_BASE %s samples=%zu hash=%llu\n",paths[k],count,(unsigned long long)hash);
  free(d);free(data);
 }
 puts("FOXEN_BASE_OK");return 0;
}
