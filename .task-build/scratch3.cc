#include "xvc_enc_lib/xvcenc.h"
#include "xvc_dec_lib/xvcdec.h"
#include <cstdio>
#include <vector>
#include <cstdint>

static int one(int checksum_mode,int restricted){
  auto e=xvc_encoder_api_get(); auto d=xvc_decoder_api_get();
  auto p=e->parameters_create(); e->parameters_set_default(p);
  p->width=160;p->height=96;p->input_bitdepth=8;p->internal_bitdepth=8;
  p->speed_mode=2;p->qp=32;p->sub_gop_length=1;p->max_keypic_distance=64;
  p->simd_mask=0;p->threads=0;p->num_ref_pics=0;
  p->checksum_mode=checksum_mode; p->restricted_mode=restricted;
  xvc_encoder_set_row_restart(p,1);
  if(e->parameters_check(p)!=XVC_ENC_OK){printf("pcheck fail cm=%d r=%d\n",checksum_mode,restricted);return 1;}
  auto enc=e->encoder_create(p); auto rec=e->picture_create(enc);
  auto dp=d->parameters_create(); d->parameters_set_default(dp);
  dp->output_bitdepth=8;dp->threads=0;dp->simd_mask=0;
  auto dec=d->decoder_create(dp); auto out=d->picture_create(dec);
  int frames=0;
  std::vector<uint8_t> pic(160*96*3/2);
  for(int f=0;f<2;f++){
    for(size_t k=0;k<pic.size();k++) pic[k]=(uint8_t)((k*13+(k/160)*17+f*23+(k%19)*(f+1))&255);
    xvc_enc_nal_unit*n=0;int cnt=0;
    if(e->encoder_encode(enc,pic.data(),&n,&cnt,rec)!=XVC_ENC_OK){printf("enc fail\n");return 2;}
    for(int i=0;i<cnt;i++){
      int rc=d->decoder_decode_nal(dec,n[i].bytes,n[i].size,0);
      if(rc!=XVC_DEC_OK){printf("decode rc=%d cm=%d r=%d type=%d\n",rc,checksum_mode,restricted,(n[i].bytes[0]>>1)&31);return 3;}
      while(d->decoder_get_picture(dec,out)==XVC_DEC_OK) frames++;
    }
  }
  for(int i=0;i<32;i++){xvc_enc_nal_unit*n=0;int cnt=0;auto rc=e->encoder_flush(enc,&n,&cnt,rec);
    for(int j=0;j<cnt;j++){ if(d->decoder_decode_nal(dec,n[j].bytes,n[j].size,0)!=XVC_DEC_OK){printf("flush decode fail\n");return 4;}
      while(d->decoder_get_picture(dec,out)==XVC_DEC_OK) frames++; }
    if(rc==XVC_ENC_NO_MORE_OUTPUT)break;}
  d->decoder_flush(dec);
  while(d->decoder_get_picture(dec,out)==XVC_DEC_OK) frames++;
  int bad=0; d->decoder_check_conformance(dec,&bad);
  printf("cm=%d restricted=%d frames=%d bad=%d\n",checksum_mode,restricted,frames,bad);
  d->picture_destroy(out);d->decoder_destroy(dec);d->parameters_destroy(dp);
  e->picture_destroy(rec);e->encoder_destroy(enc);e->parameters_destroy(p);
  return (frames==2&&bad==0)?0:5;
}
int main(){
  int r=0;
  r|=one(1,0); // max robust MD5 per component
  r|=one(0,1); // restricted -> CRC fallback + static cabac
  r|=one(1,1);
  printf(r?"SCRATCH3_FAIL\n":"SCRATCH3_OK\n");
  return r;
}
