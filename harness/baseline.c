#include <stdio.h>
#include <math.h>
#include <assert.h>
#include "opusfile.h"
int main(int argc,char **argv){
 assert(argc==2);char path[4096];snprintf(path,sizeof path,"%s/zero.opus",argv[1]);int err;
 OggOpusFile *of=op_open_file(path,&err);assert(of&&err==0);
 assert(op_link_count(of)==1&&op_head(of,0)->channel_count==1);
 float pcm[257];int n,total=0;double energy=0;
 while((n=op_read_float(of,pcm,257,NULL))>0){
  total+=n;for(int i=0;i<n;i++){assert(isfinite(pcm[i]));energy+=pcm[i]*pcm[i];}
 }
 assert(n==0&&total==5448&&op_pcm_tell(of)==total&&energy>0.1);
 printf("seed Opus decode: frames=%d finite nonzero PCM ok\n",total);
 op_free(of);return 0;
}
