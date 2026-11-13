#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
int main(void){
 vorbis_info e,i;vorbis_comment ec,ic;vorbis_dsp_state enc,dec;vorbis_block eb,db;ogg_packet h[3],p;
 vorbis_info_init(&e);vorbis_info_init(&i);vorbis_comment_init(&ec);vorbis_comment_init(&ic);
 assert(vorbis_encode_init_vbr(&e,6,48000,.4f)==0);assert(vorbis_analysis_init(&enc,&e)==0);assert(vorbis_block_init(&enc,&eb)==0);
 assert(vorbis_analysis_headerout(&enc,&ec,&h[0],&h[1],&h[2])==0);for(int j=0;j<3;j++)assert(vorbis_synthesis_headerin(&i,&ic,&h[j])==0);
 assert(vorbis_synthesis_init(&dec,&i)==0);assert(vorbis_block_init(&dec,&db)==0);
 float **v=vorbis_analysis_buffer(&enc,4096);for(int c=0;c<6;c++)for(int t=0;t<4096;t++)v[c][t]=.2f*sin((.03+.007*c)*t)*(c%2?-1:1);
 vorbis_analysis_wrote(&enc,4096);vorbis_analysis_wrote(&enc,0);int packets=0,frames=0;double energy=0,weighted=0;
 while(vorbis_analysis_blockout(&enc,&eb)==1){assert(vorbis_analysis(&eb,NULL)==0);assert(vorbis_bitrate_addblock(&eb)==0);
  while(vorbis_bitrate_flushpacket(&enc,&p)){packets++;assert(vorbis_synthesis(&db,&p)==0);assert(vorbis_synthesis_blockin(&dec,&db)==0);float **pcm;int n;
   while((n=vorbis_synthesis_pcmout(&dec,&pcm))>0){for(int c=0;c<6;c++)for(int t=0;t<n;t++){assert(isfinite(pcm[c][t]));energy+=pcm[c][t]*pcm[c][t];weighted+=pcm[c][t]*(c+1)*((frames+t)%31-15);}frames+=n;vorbis_synthesis_read(&dec,n);}
  }
 }
 assert(packets>1 && frames==4096 && energy>1);printf("VORBIS_BASE packets=%d frames=%d energy=%.9f weighted=%.9f\n",packets,frames,energy,weighted);
 
 vorbis_block_clear(&db);vorbis_dsp_clear(&dec);vorbis_comment_clear(&ic);vorbis_info_clear(&i);vorbis_block_clear(&eb);vorbis_dsp_clear(&enc);vorbis_comment_clear(&ec);vorbis_info_clear(&e);puts("VORBIS_BASE_OK");return 0;
}
