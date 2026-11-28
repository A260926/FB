/* SPDX-License-Identifier: MIT */
#include "xvid.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define N 80
static unsigned char pcm[64*64*3/2], out[1024*1024];
static long run(int pass) {
 xvid_enc_create_t c; xvid_enc_plugin_t p; xvid_plugin_2pass1_t p1; xvid_plugin_2pass2_t p2;
 memset(&c,0,sizeof c); memset(&p,0,sizeof p); memset(&p1,0,sizeof p1); memset(&p2,0,sizeof p2);
 c.version=XVID_VERSION;c.width=64;c.height=64;c.fincr=1;c.fbase=25;c.max_key_interval=20;
 c.num_threads=1;c.num_plugins=1;c.plugins=&p;
 p1.version=XVID_VERSION;p1.filename="devcheck/build/cfr.stats";
 p2.version=XVID_VERSION;p2.filename=p1.filename;p2.bitrate=40000;
 if(pass==1){p.func=xvid_plugin_2pass1;p.param=&p1;}else{p.func=xvid_plugin_2pass2;p.param=&p2;}
 for(int k=0;k<3;k++){c.min_quant[k]=1;c.max_quant[k]=31;}
 int r=xvid_encore(NULL,XVID_ENC_CREATE,&c,NULL);if(r<0){fprintf(stderr,"create=%d pass=%d\n",r,pass);exit(2);}
 long bytes=0;uint32_t rng=0x12345678;
 for(int i=0;i<N;i++){
  for(size_t j=0;j<sizeof pcm;j++){rng=1664525*rng+1013904223;pcm[j]=(unsigned char)((rng>>27)*4+i*3+j%64);}
  xvid_enc_frame_t f; xvid_enc_stats_t s;memset(&f,0,sizeof f);memset(&s,0,sizeof s);
  f.version=s.version=XVID_VERSION;f.input.csp=XVID_CSP_I420;f.input.plane[0]=pcm;f.input.stride[0]=64;
  f.bitstream=out;f.length=sizeof out;f.type=i%20==0?XVID_TYPE_IVOP:XVID_TYPE_AUTO;
  f.par=XVID_PAR_11_VGA;f.vop_flags=XVID_VOP_HALFPEL;f.motion=XVID_ME_ADVANCEDDIAMOND16;
  r=xvid_encore(c.handle,XVID_ENC_ENCODE,&f,&s);if(r<=0){fprintf(stderr,"encode=%d frame=%d\n",r,i);exit(3);}bytes+=r;
 }
 xvid_encore(c.handle,XVID_ENC_DESTROY,NULL,NULL);return bytes;
}
int main(void){xvid_gbl_init_t g;memset(&g,0,sizeof g);g.version=XVID_VERSION;g.cpu_flags=XVID_CPU_FORCE;
 if(xvid_global(NULL,XVID_GBL_INIT,&g,NULL)<0)return 1;
 long b1=run(1),b2=run(2);FILE*f=fopen("devcheck/build/cfr.stats","r");char line[1024];int n=0;
 if(!f)return 4;while(fgets(line,sizeof line,f))if(strchr("ipbs",line[0]))n++;fclose(f);
 if(n!=N||b1<=0||b2<=0)return 5;
 printf("XVID_BASELINE frames=%d stats=%d pass1_bytes=%ld pass2_bytes=%ld\n",N,n,b1,b2);return 0;
}
