#include "xvc_enc_lib/xvcenc.h"
#include "xvc_dec_lib/xvcdec.h"
#include "xvc_dec_lib/decoder.h"
#include <cstdio>
#include <vector>
#include <cstdint>
#include <cstring>

static const xvc_encoder_api *e;
static const xvc_decoder_api *d;

struct Nal { std::vector<uint8_t> b; };

static int run_cfg(int W, int H, int bd, int chroma, int frames, int subgop,
                   int order, std::vector<std::vector<uint8_t> > *outs,
                   std::vector<xvc::PictureDecoder::RowDiag> *diag_out = 0,
                   int *bad_conform = 0) {
  xvc_encoder_parameters *p = e->parameters_create();
  e->parameters_set_default(p);
  p->width=W; p->height=H; p->input_bitdepth=bd; p->internal_bitdepth=bd;
  p->chroma_format=(xvc_enc_chroma_format)chroma;
  p->speed_mode=2; p->qp=32; p->sub_gop_length=subgop;
  p->max_keypic_distance=64; p->simd_mask=0; p->threads=0;
  p->num_ref_pics = subgop>1 ? -1 : 0;
  xvc_encoder_set_row_restart(p,1);
  if (e->parameters_check(p)!=XVC_ENC_OK) { printf("pcheck fail\n"); return 1; }
  xvc_encoder *enc=e->encoder_create(p);
  xvc_enc_pic_buffer *rec=e->picture_create(enc);
  xvc_decoder_parameters *dp=d->parameters_create();
  d->parameters_set_default(dp);
  dp->output_bitdepth=bd; dp->threads=0; dp->simd_mask=0;
  xvc_decoder *dbase=d->decoder_create(dp);
  xvc::Decoder *dec=static_cast<xvc::Decoder*>(dbase);
  dec->SetRowDecodeOrderForTest(order);
  xvc_decoded_picture *out=d->picture_create(dbase);

  auto feed=[&](xvc_enc_nal_unit *n,int cnt){
    for(int i=0;i<cnt;i++){
      if(d->decoder_decode_nal(dbase,n[i].bytes,n[i].size,n[i].user_data)!=XVC_DEC_OK){
        printf("decode_nal not ok order=%d\n",order);
      }
      xvc_dec_return_code rc;
      while((rc=d->decoder_get_picture(dbase,out))==XVC_DEC_OK){
        outs->push_back(std::vector<uint8_t>(out->bytes,out->bytes+out->size));
      }
      if(rc!=XVC_DEC_NO_DECODED_PIC) printf("drain rc=%d\n",(int)rc);
    }
  };

  int cw = chroma==3 ? W : (W+1)/2;
  int ch = chroma==3 ? H : (H+1)/2;
  int bps = bd==8?1:2;
  size_t fsz = (size_t)bps*(W*H + 2*cw*ch);
  std::vector<uint8_t> pic(fsz);
  for(int f=0;f<frames;f++){
    if(bd==8){
      for(size_t k=0;k<pic.size();k++) pic[k]=(uint8_t)((k*13+(k/W)*17+f*23+(k%19)*f)&255);
    } else {
      uint16_t *q=reinterpret_cast<uint16_t*>(pic.data());
      size_t n=fsz/2;
      for(size_t k=0;k<n;k++) q[k]=(uint16_t)(((k*13+(k/W)*17+f*23+(k%19)*f)*4)&1023);
    }
    xvc_enc_nal_unit *n=0; int cnt=0;
    if(e->encoder_encode(enc,pic.data(),&n,&cnt,rec)!=XVC_ENC_OK){printf("enc fail\n");return 1;}
    feed(n,cnt);
  }
  for(int i=0;i<32;i++){
    xvc_enc_nal_unit *n=0;int cnt=0;
    auto rc=e->encoder_flush(enc,&n,&cnt,rec);
    feed(n,cnt);
    if(rc==XVC_ENC_NO_MORE_OUTPUT) break;
  }
  d->decoder_flush(dbase);
  xvc_dec_return_code rc;
  while((rc=d->decoder_get_picture(dbase,out))==XVC_DEC_OK){
    outs->push_back(std::vector<uint8_t>(out->bytes,out->bytes+out->size));
  }
  if(rc!=XVC_DEC_NO_DECODED_PIC) printf("flush drain rc=%d\n",(int)rc);
  int bad=0; d->decoder_check_conformance(dbase,&bad);
  if(bad_conform)*bad_conform=bad;
  if(diag_out)*diag_out=dec->GetRowDiagnosticsForTest();
  d->picture_destroy(out); e->picture_destroy(rec);
  e->encoder_destroy(enc); d->decoder_destroy(dbase);
  e->parameters_destroy(p); d->parameters_destroy(dp);
  return 0;
}

int main(){
  e=xvc_encoder_api_get(); d=xvc_decoder_api_get();
  struct C { int W,H,bd,ch,f,g; const char*name; };
  C cs[]={
    {160,96,8,1,2,1,"trunc420"},
    {160,96,10,1,2,1,"10bit420"},
    {160,96,8,3,2,1,"444"},
    {128,128,8,1,9,4,"Bdelay"},
  };
  for(auto &c:cs){
    std::vector<std::vector<uint8_t> > a,b; int bad0=0,bad1=0;
    std::vector<xvc::PictureDecoder::RowDiag> diag;
    run_cfg(c.W,c.H,c.bd,c.ch,c.f,c.g,0,&a,&diag,&bad0);
    run_cfg(c.W,c.H,c.bd,c.ch,c.f,c.g,1,&b,0,&bad1);
    bool eq=a==b;
    printf("%s frames fwd=%zu rev=%zu eq=%d bad=%d/%d diag=%zu",c.name,a.size(),b.size(),eq,bad0,bad1,diag.size());
    if(!diag.empty()){
      printf(" fp=%08x/%08x qp=%d/%d pqp=%d/%d cons=%zu/%zu sec=%d",
        diag[0].context_fingerprint,diag[1].context_fingerprint,
        diag[0].pic_qp,diag[1].pic_qp,
        diag[0].predicted_qp_first,diag[1].predicted_qp_first,
        diag[0].consumed_bytes,diag[1].consumed_bytes,(int)diag[0].secondary_tree);
    }
    printf("\n");
    if(!eq || a.size()!=(size_t)c.f || bad0 || bad1) { printf("MISMATCH/FAIL %s\n",c.name); return 2; }
  }
  printf("SCRATCH_OK\n");
  return 0;
}
