#include "xvc_enc_lib/xvcenc.h"
#include "xvc_dec_lib/xvcdec.h"
#include "xvc_dec_lib/decoder.h"
#include <cstdio>
#include <vector>
#include <cstdint>

struct N { std::vector<uint8_t> b; };

static int geo(int W,int H,int bd,int chroma){
  auto e=xvc_encoder_api_get(); auto d=xvc_decoder_api_get();
  auto p=e->parameters_create(); e->parameters_set_default(p);
  p->width=W;p->height=H;p->input_bitdepth=bd;p->internal_bitdepth=bd;
  p->chroma_format=(xvc_enc_chroma_format)chroma;
  p->speed_mode=2;p->qp=30;p->sub_gop_length=1;p->max_keypic_distance=64;
  p->simd_mask=0;p->threads=0;p->num_ref_pics=0;
  xvc_encoder_set_row_restart(p,1);
  if(e->parameters_check(p)!=XVC_ENC_OK){printf("pcheck fail %dx%d\n",W,H);return 1;}
  auto enc=e->encoder_create(p); auto rec=e->picture_create(enc);
  int cw=chroma==3?W:(W+1)/2, ch=chroma==3?H:(H+1)/2, bps=bd==8?1:2;
  size_t fsz=bps*(W*H+2*cw*ch);
  std::vector<uint8_t> pic(fsz);
  std::vector<N> nals;
  for(int f=0;f<2;f++){
    if(bd==8) for(size_t k=0;k<fsz;k++) pic[k]=(uint8_t)((k*7+f*31+(k/W)*13)&255);
    else { uint16_t*q=(uint16_t*)pic.data(); for(size_t k=0;k<fsz/2;k++) q[k]=(uint16_t)(((k*7+f*31)*8)&1023); }
    xvc_enc_nal_unit*n=0;int cnt=0;
    if(e->encoder_encode(enc,pic.data(),&n,&cnt,rec)!=XVC_ENC_OK){printf("encfail\n");return 9;}
    for(int i=0;i<cnt;i++) nals.push_back({std::vector<uint8_t>(n[i].bytes,n[i].bytes+n[i].size)});
  }
  for(int i=0;i<16;i++){xvc_enc_nal_unit*n=0;int cnt=0;auto rc=e->encoder_flush(enc,&n,&cnt,rec);
    for(int j=0;j<cnt;j++) nals.push_back({std::vector<uint8_t>(n[j].bytes,n[j].bytes+n[j].size)});
    if(rc==XVC_ENC_NO_MORE_OUTPUT)break;}
  e->picture_destroy(rec);e->encoder_destroy(enc);e->parameters_destroy(p);

  std::vector<std::vector<uint8_t>> out[2];
  int fail=0;
  for(int order=0;order<2;order++){
    auto dp=d->parameters_create();d->parameters_set_default(dp);
    dp->output_bitdepth=bd;dp->threads=0;dp->simd_mask=0;
    auto base=d->decoder_create(dp);
    static_cast<xvc::Decoder*>(base)->SetRowDecodeOrderForTest(order);
    auto o=d->picture_create(base);
    for(auto& nl:nals){
      if(d->decoder_decode_nal(base,nl.b.data(),nl.b.size(),0)!=XVC_DEC_OK){
        printf("dec fail %dx%d o=%d type=%d\n",W,H,order,(nl.b[0]>>1)&31);fail=1;}
      while(d->decoder_get_picture(base,o)==XVC_DEC_OK) out[order].push_back(std::vector<uint8_t>(o->bytes,o->bytes+o->size));
    }
    d->decoder_flush(base);
    while(d->decoder_get_picture(base,o)==XVC_DEC_OK) out[order].push_back(std::vector<uint8_t>(o->bytes,o->bytes+o->size));
    int bad=0;d->decoder_check_conformance(base,&bad);
    if(bad){printf("bad conform %dx%d o=%d\n",W,H,order);fail=1;}
    d->picture_destroy(o);d->decoder_destroy(base);d->parameters_destroy(dp);
  }
  bool eq=out[0]==out[1];
  int rows=(H+63)/64, cols=(W+63)/64;
  printf("%dx%d rows=%d cols=%d n=%zu/%zu eq=%d\n",W,H,rows,cols,out[0].size(),out[1].size(),eq);
  return (eq&&out[0].size()==2&&!fail)?0:4;
}
int main(){
  int r=0;
  r|=geo(32,32,8,1);
  r|=geo(64,64,8,1);
  r|=geo(64,128,8,1);
  r|=geo(128,64,8,1);
  r|=geo(192,128,8,1);
  r|=geo(130,72,8,1);
  r|=geo(176,144,10,1);
  r|=geo(96,160,8,3);
  printf(r?"GEO_FAIL\n":"GEO_OK\n");
  return r;
}
