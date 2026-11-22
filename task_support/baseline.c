#include <theora/theoraenc.h>
#include <theora/theoradec.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(void){
 th_info info,parsed; th_comment comment; th_setup_info *setup=NULL;
 th_enc_ctx *enc; th_dec_ctx *dec; th_ycbcr_buffer input,output;
 ogg_packet packet; unsigned char y[64*48],u[32*24],v[32*24];
 unsigned long sums[6]; int i,f,n=0,headers=0,pp=0,key=0,inter=0;
 th_info_init(&info);th_info_init(&parsed);th_comment_init(&comment);
 info.frame_width=info.pic_width=64;info.frame_height=info.pic_height=48;
 info.fps_numerator=25;info.fps_denominator=1;
 info.aspect_numerator=info.aspect_denominator=1;
 info.pixel_fmt=TH_PF_420;info.quality=42;info.keyframe_granule_shift=6;
 enc=th_encode_alloc(&info);assert(enc);
 while(th_encode_flushheader(enc,&comment,&packet)>0){
  assert(th_decode_headerin(&parsed,&comment,&setup,&packet)>0);headers++;
 }
 assert(headers==3);dec=th_decode_alloc(&parsed,setup);assert(dec);
 th_setup_free(setup);setup=NULL;
 assert(th_decode_ctl(dec,TH_DECCTL_SET_PPLEVEL,&pp,sizeof(pp))==0);
 input[0]=(th_img_plane){64,48,64,y};
 input[1]=(th_img_plane){32,24,32,u};input[2]=(th_img_plane){32,24,32,v};
 for(f=0;f<6;f++){
  for(i=0;i<64*48;i++)y[i]=(unsigned char)(40+(i%64)/2+(i/64)/3+f*2);
  for(i=0;i<32*24;i++){u[i]=(unsigned char)(100+i%13+f);v[i]=(unsigned char)(160-i%11-f);}
  assert(th_encode_ycbcr_in(enc,input)==0);
  while(th_encode_packetout(enc,f==5,&packet)>0){
   ogg_int64_t gp=-1;int k=th_packet_iskeyframe(&packet);assert(k>=0);
   key+=k;inter+=!k;assert(n<6);
   assert(th_decode_packetin(dec,&packet,&gp)==0);
   assert(th_decode_ycbcr_out(dec,output)==0);
   assert(output[0].width==64&&output[0].height==48);
   sums[n]=0;for(i=0;i<48;i++)for(int x=0;x<64;x++)sums[n]+=output[0].data[i*output[0].stride+x];
   printf("THEORA_FRAME index=%d key=%d bytes=%ld granule=%lld sum=%lu\n",n,k,packet.bytes,(long long)gp,sums[n]);n++;
  }
 }
 assert(n==6&&sums[0]!=sums[5]&&key>=1&&inter>=2);
 th_decode_free(dec);th_encode_free(enc);th_info_clear(&info);th_info_clear(&parsed);th_comment_clear(&comment);
 printf("THEORA_BASELINE_OK frames=%d keyframes=%d interframes=%d setup_freed=1\n",n,key,inter);
 return 0;
}
