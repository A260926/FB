#include "xvc_enc_lib/xvcenc.h"
#include "xvc_dec_lib/xvcdec.h"
#include "xvc_dec_lib/decoder.h"
#include <cstdio>
#include <vector>
#include <cstdint>

static const xvc_encoder_api *e=xvc_encoder_api_get();
static const xvc_decoder_api *d=xvc_decoder_api_get();

static std::vector<std::vector<uint8_t>> nals;
static std::vector<uint8_t> segnal;

static void encode_one(){
  xvc_encoder_parameters *p=e->parameters_create(); e->parameters_set_default(p);
  p->width=160;p->height=96;p->input_bitdepth=8;p->internal_bitdepth=8;
  p->speed_mode=2;p->qp=32;p->sub_gop_length=1;p->max_keypic_distance=64;
  p->simd_mask=0;p->threads=0;p->num_ref_pics=0;
  xvc_encoder_set_row_restart(p,1);
  xvc_encoder *enc=e->encoder_create(p);
  xvc_enc_pic_buffer *rec=e->picture_create(enc);
  std::vector<uint8_t> pic(160*96*3/2);
  for(size_t k=0;k<pic.size();k++) pic[k]=(uint8_t)((k*13+(k/160)*17+((k%19)*3))&255);
  xvc_enc_nal_unit *n=0;int cnt=0;
  e->encoder_encode(enc,pic.data(),&n,&cnt,rec);
  for(int i=0;i<cnt;i++){
    std::vector<uint8_t> b(n[i].bytes,n[i].bytes+n[i].size);
    int type=(n[i].bytes[0]>>1)&31;
    if(type==16) segnal=b; else nals.push_back(b);
  }
  e->picture_destroy(rec);e->encoder_destroy(enc);e->parameters_destroy(p);
}

struct Res { int drc; int pic_rc; int bad; size_t outsize; bool disc; };

static Res try_decode(const std::vector<uint8_t>& picnal){
  xvc_decoder_parameters *dp=d->parameters_create();d->parameters_set_default(dp);
  dp->output_bitdepth=8;dp->threads=0;dp->simd_mask=0;
  xvc_decoder *dec=d->decoder_create(dp);
  xvc_decoded_picture *out=d->picture_create(dec);
  int rc1=d->decoder_decode_nal(dec,segnal.data(),segnal.size(),0);
  int rc2=d->decoder_decode_nal(dec,picnal.data(),picnal.size(),0);
  d->decoder_flush(dec);
  int prc=d->decoder_get_picture(dec,out);
  size_t sz=out->size;
  int bad=0;d->decoder_check_conformance(dec,&bad);
  xvc::Decoder* xx=static_cast<xvc::Decoder*>(dec);
  bool disc=xx->HasDiscardPictureForTest();
  d->picture_destroy(out);d->decoder_destroy(dec);d->parameters_destroy(dp);
  return {rc2,prc,bad,sz,disc};
}

static size_t find_magic(const std::vector<uint8_t>& b){
  for(size_t i=0;i+8<=b.size();i++)
    if(b[i]=='X'&&b[i+1]=='R'&&b[i+2]=='S'&&b[i+3]=='T') return i;
  return (size_t)-1;
}

int main(){
  encode_one();
  printf("pic nals=%zu seg=%zu\n",nals.size(),segnal.size());
  std::vector<uint8_t> base=nals[0];
  size_t m=find_magic(base);
  printf("magic at %zu size=%zu rows=%u len0=%u len1=%u\n",m,base.size(),
    (base[m+4]<<8)|base[m+5],
    ((unsigned)base[m+8]<<24)|((unsigned)base[m+9]<<16)|((unsigned)base[m+10]<<8)|base[m+11],
    ((unsigned)base[m+12]<<24)|((unsigned)base[m+13]<<16)|((unsigned)base[m+14]<<8)|base[m+15]);
  uint32_t l0=((unsigned)base[m+8]<<24)|((unsigned)base[m+9]<<16)|((unsigned)base[m+10]<<8)|base[m+11];
  size_t rows_start=m+16;
  // baseline valid
  Res r=try_decode(base); printf("valid drc=%d prc=%d bad=%d sz=%zu disc=%d\n",r.drc,r.pic_rc,r.bad,r.outsize,r.disc);
  // missing dir magic
  { auto b=base; b[m]='Q'; Res x=try_decode(b); printf("\1 drc=%d prc=%d bad=%d disc=%d\n",x.drc,x.pic_rc,x.bad,x.disc); }
  // short dir
  { auto b=base; b.resize(m+5); Res x=try_decode(b); printf("\1 drc=%d prc=%d bad=%d disc=%d\n",x.drc,x.pic_rc,x.bad,x.disc); }
  // zero len
  { auto b=base; b[m+8]=b[m+9]=b[m+10]=b[m+11]=0; Res x=try_decode(b); printf("\1 drc=%d prc=%d bad=%d disc=%d\n",x.drc,x.pic_rc,x.bad,x.disc); }
  // oob len
  { auto b=base; b[m+8]=0xff;b[m+9]=0xff;b[m+10]=0xff;b[m+11]=0xff; Res x=try_decode(b); printf("\1 drc=%d prc=%d bad=%d disc=%d\n",x.drc,x.pic_rc,x.bad,x.disc); }
  // truncate whole nal
  { auto b=base; b.resize(rows_start+3); Res x=try_decode(b); printf("\1 drc=%d prc=%d bad=%d disc=%d\n",x.drc,x.pic_rc,x.bad,x.disc); }
  // corrupt last byte of row0 various masks
  for(int mask=1;mask<256;mask*=2){
    auto b=base; size_t pos=rows_start+l0-1; b[pos]^=(uint8_t)mask;
    Res x=try_decode(b);
    printf("lastbyte xor%02x drc=%d prc=%d bad=%d disc=%d\n",mask,x.drc,x.pic_rc,x.bad,x.disc);
  }
  // zero last 2 bytes row0
  { auto b=base; b[rows_start+l0-1]=0;b[rows_start+l0-2]=0; Res x=try_decode(b); printf("zero2last drc=%d prc=%d bad=%d disc=%d\n",x.drc,x.pic_rc,x.bad,x.disc); }
  // corrupt middle of row0
  { auto b=base; size_t pos=rows_start+l0/2; b[pos]^=0xff; Res x=try_decode(b); printf("midrow drc=%d prc=%d bad=%d disc=%d\n",x.drc,x.pic_rc,x.bad,x.disc); }
  return 0;
}
