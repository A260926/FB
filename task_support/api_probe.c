/* Fixed compile/link consumer: exercises declarations, symbols and real pixels.
   This is scaffolding, not a region decoder implementation. */
#include <theora/theoraregion.h>
#include <theora/theoraenc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 th_info info,parsed;th_comment comment;th_setup_info *setup=NULL;
 th_enc_ctx *enc;th_dec_ctx *full;th_region_ctx *region;th_ycbcr_buffer in,out,ref;
 ogg_packet op;unsigned char y[64*48],u[32*24],v[32*24];size_t blocks=0,bytes=0;
 th_info_init(&info);th_info_init(&parsed);th_comment_init(&comment);
 info.frame_width=info.pic_width=64;info.frame_height=info.pic_height=48;
 info.fps_numerator=25;info.fps_denominator=1;info.pixel_fmt=TH_PF_420;info.quality=42;
 info.aspect_numerator=info.aspect_denominator=1;info.keyframe_granule_shift=6;
 enc=th_encode_alloc(&info);assert(enc);
 while(th_encode_flushheader(enc,&comment,&op)>0)assert(th_decode_headerin(&parsed,&comment,&setup,&op)>0);
 full=th_decode_alloc(&parsed,setup);region=th_region_alloc(&parsed,setup,3);assert(full&&region);
 th_setup_free(setup);int pp=0;assert(!th_decode_ctl(full,TH_DECCTL_SET_PPLEVEL,&pp,sizeof(pp)));
 for(int i=0;i<64*48;i++)y[i]=(unsigned char)(40+i%47);
 memset(u,110,sizeof(u));memset(v,149,sizeof(v));
 in[0]=(th_img_plane){64,48,64,y};in[1]=(th_img_plane){32,24,32,u};in[2]=(th_img_plane){32,24,32,v};
 assert(!th_encode_ycbcr_in(enc,in));assert(th_encode_packetout(enc,1,&op)>0);
 assert(!th_region_packetin(region,&op));assert(!th_decode_packetin(full,&op,NULL));
 assert(!th_decode_ycbcr_out(full,ref));assert(!th_region_out(region,0,16,16,16,16,out));
 for(int p=0;p<3;p++){
  int s=p?8:16,offset=p?8:16;
  assert(out[p].width==s&&out[p].height==s&&out[p].stride>=s&&out[p].data);
  for(int r=0;r<s;r++)assert(!memcmp(out[p].data+r*out[p].stride,ref[p].data+(r+offset)*ref[p].stride+offset,s));
 }
 assert(!th_region_stats(region,&blocks,&bytes));assert(blocks>0&&bytes>0);
 th_region_free(region);th_decode_free(full);th_encode_free(enc);
 th_info_clear(&info);th_info_clear(&parsed);th_comment_clear(&comment);
 puts("REGION_API_PIXELS_OK");return 0;
}
