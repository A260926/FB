#ifndef CODEC_WITNESS_H
#define CODEC_WITNESS_H
#include "xvc_enc_lib/xvcenc.h"
#include "xvc_dec_lib/xvcdec.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <fstream>
#include <iostream>
static int codec_witness(const char *prefix){
 const xvc_encoder_api*e=xvc_encoder_api_get();const xvc_decoder_api*d=xvc_decoder_api_get();
 xvc_encoder_parameters*p=e->parameters_create();if(!p||e->parameters_set_default(p)!=XVC_ENC_OK)return 2;
 p->width=128;p->height=128;p->input_bitdepth=8;p->internal_bitdepth=8;p->speed_mode=2;p->qp=32;p->sub_gop_length=4;p->max_keypic_distance=64;p->simd_mask=0;p->threads=0;
#ifdef REQUIRE_ROWS
 if(xvc_encoder_set_row_restart(p,1)!=XVC_ENC_OK)return 30;
 if(xvc_encoder_set_row_restart(p,2)!=XVC_ENC_INVALID_ARGUMENT)return 31;
 if(xvc_encoder_set_row_restart(nullptr,1)!=XVC_ENC_INVALID_ARGUMENT)return 32;
#endif
 if(e->parameters_check(p)!=XVC_ENC_OK)return 3;xvc_encoder*enc=e->encoder_create(p);if(!enc)return 4;
 xvc_decoder_parameters*dp=d->parameters_create();if(!dp||d->parameters_set_default(dp)!=XVC_DEC_OK)return 5;
 dp->output_bitdepth=8;dp->threads=0;dp->simd_mask=0;xvc_decoder*dec=d->decoder_create(dp);if(!dec)return 6;
 xvc_enc_pic_buffer*rec=e->picture_create(enc);xvc_decoded_picture*out=d->picture_create(dec);
 if(!rec||!out)return 7;
 std::vector<std::vector<uint8_t>>recs,decs;unsigned total_nals=0,segments=0;bool version_ok=false;
 std::ofstream bs(std::string(prefix)+".xvc",std::ios::binary),raw(std::string(prefix)+".yuv",std::ios::binary);if(!bs||!raw)return 8;
 auto drain=[&]()->int{
  xvc_dec_return_code rc;
  while((rc=d->decoder_get_picture(dec,out))==XVC_DEC_OK){
   if(out->stats.width!=128||out->stats.height!=128||out->stats.bitdepth!=8||!out->stats.conforming)return 10;
   std::vector<uint8_t>b;
   for(int c=0;c<3;c++){int w=c?64:128,h=c?64:128;for(int y=0;y<h;y++){const uint8_t*s=reinterpret_cast<const uint8_t*>(out->planes[c])+y*out->stride[c];b.insert(b.end(),s,s+w);}}
   raw.write(reinterpret_cast<const char*>(b.data()),b.size());decs.push_back(b);
  }
  return rc==XVC_DEC_NO_DECODED_PIC?0:11;
 };
 auto take=[&](xvc_enc_nal_unit*n,int count)->int{
  if(rec->pic&&rec->size)recs.emplace_back(rec->pic,rec->pic+rec->size);
  for(int i=0;i<count;i++){
   if(!n[i].bytes||!n[i].size||n[i].size>0xffffffffu)return 12;
   if(((n[i].bytes[0]>>1)&31)==16){
    segments++;if(n[i].size<8)return 13;
#ifdef REQUIRE_ROWS
    if(n[i].bytes[4]!=0||n[i].bytes[5]!=3||n[i].bytes[6]||n[i].bytes[7])return 40;
#else
    if(n[i].bytes[4]!=0||n[i].bytes[5]!=2||n[i].bytes[6]||n[i].bytes[7])return 41;
#endif
    version_ok=true;
   }
   uint32_t sz=static_cast<uint32_t>(n[i].size);char len[4]={static_cast<char>(sz>>24),static_cast<char>(sz>>16),static_cast<char>(sz>>8),static_cast<char>(sz)};
   bs.write(len,4);bs.write(reinterpret_cast<const char*>(n[i].bytes),n[i].size);total_nals++;
   if(d->decoder_decode_nal(dec,n[i].bytes,n[i].size,n[i].user_data)!=XVC_DEC_OK)return 14;
   int rc=drain();if(rc)return rc;
  }return 0;
 };
 std::vector<uint8_t>picture(128*128*3/2);
 for(int f=0;f<5;f++){
  for(size_t k=0;k<picture.size();k++)picture[k]=static_cast<uint8_t>((k*13+(k/128)*17+f*23+(k%19)*f)&255);
  xvc_enc_nal_unit*n=nullptr;int count=0;
  if(e->encoder_encode(enc,picture.data(),&n,&count,rec)!=XVC_ENC_OK)return 15;
  int rc=take(n,count);if(rc)return rc;
 }
 bool done=false;for(int i=0;i<32;i++){
  xvc_enc_nal_unit*n=nullptr;int count=0;auto rc=e->encoder_flush(enc,&n,&count,rec);
  if(rc!=XVC_ENC_OK&&rc!=XVC_ENC_NO_MORE_OUTPUT)return 16;
  int err=take(n,count);if(err)return err;
  if(rc==XVC_ENC_NO_MORE_OUTPUT){done=true;break;}
 }if(!done)return 17;
 if(d->decoder_flush(dec)!=XVC_DEC_OK)return 18;int err=drain();if(err)return err;
 if(recs.size()!=5||decs.size()!=5||recs!=decs||!segments||!version_ok)return 19;
 int bad=0;if(d->decoder_check_conformance(dec,&bad)!=XVC_DEC_OK||bad)return 20;
 bs.close();raw.close();if(!bs||!raw)return 21;
 e->picture_destroy(rec);d->picture_destroy(out);e->encoder_destroy(enc);d->decoder_destroy(dec);e->parameters_destroy(p);d->parameters_destroy(dp);
#ifdef REQUIRE_ROWS
 std::cout<<"API_NORMAL row_restart=1 frames=5 version=3.0 reconstruction=equal\n";
#else
 std::cout<<"BASELINE frames=5 version=2.0 reconstruction=equal nals="<<total_nals<<"\n";
#endif
 return 0;
}
#endif
