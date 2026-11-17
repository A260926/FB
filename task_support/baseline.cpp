#include "codec_api.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <cstdint>
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)

struct Bits {
  std::vector<unsigned char> b; size_t pos;
  Bits(const unsigned char* p, size_t n):pos(0) {
    unsigned zeros=0;
    for(size_t i=0;i<n;++i) {
      if(zeros==2 && p[i]==3) { zeros=0; continue; }
      b.push_back(p[i]); zeros=p[i]==0?zeros+1:0;
    }
  }
  unsigned bit() { REQUIRE(pos<b.size()*8); unsigned v=(b[pos/8]>>(7-pos%8))&1; ++pos; return v; }
  unsigned ue() { unsigned z=0; while(!bit()) { REQUIRE(++z<30); } unsigned v=1; while(z--) v=v*2+bit(); return v-1; }
  bool more() const { size_t q=pos; if(q>=b.size()*8) return false;
    if(!((b[q/8]>>(7-q%8))&1)) return true;
    for(++q;q<b.size()*8;++q) if((b[q/8]>>(7-q%8))&1) return true;
    return false;
  }
};
static int pps_transform(const unsigned char* p, size_t n) {
  Bits r(p,n); r.ue();r.ue();r.bit();r.bit(); REQUIRE(r.ue()==0);
  r.ue();r.ue();r.bit();r.bit();r.bit();r.ue();r.ue();r.ue();r.bit();r.bit();r.bit();
  return r.more()?int(r.bit()):0;
}
static std::vector<unsigned char> snapshot(unsigned char** p,const SBufferInfo& info) {
  const SSysMEMBuffer& s=info.UsrData.sSystemBuffer; REQUIRE(info.iBufferStatus==1);
  std::vector<unsigned char> out;
  for(int c=0;c<3;++c) { int w=s.iWidth>>(c!=0),h=s.iHeight>>(c!=0),stride=s.iStride[c!=0];
    for(int y=0;y<h;++y) out.insert(out.end(),p[c]+y*stride,p[c]+y*stride+w);
  } return out;
}
static uint64_t digest(const std::vector<unsigned char>& p) {
  uint64_t h=1469598103934665603ULL;for(size_t i=0;i<p.size();++i) h=(h^p[i])*1099511628211ULL; return h;
}
static void run(int cabac,int slices) {
  ISVCEncoder* e=0;ISVCDecoder *a=0,*b=0;
  REQUIRE(WelsCreateSVCEncoder(&e)==0); REQUIRE(WelsCreateDecoder(&a)==0); REQUIRE(WelsCreateDecoder(&b)==0);
  int trace=WELS_LOG_QUIET;e->SetOption(ENCODER_OPTION_TRACE_LEVEL,&trace);
  SEncParamExt c; REQUIRE(e->GetDefaultParams(&c)==0);
  const int w=96,h=64;
  c.iUsageType=CAMERA_VIDEO_REAL_TIME;c.iPicWidth=w;c.iPicHeight=h;c.fMaxFrameRate=30;
  c.iRCMode=RC_OFF_MODE;c.iSpatialLayerNum=1;c.iTemporalLayerNum=1;c.iMultipleThreadIdc=1;
  c.iEntropyCodingModeFlag=cabac;c.bEnableFrameSkip=false;c.bEnableSceneChangeDetect=false;
  c.bEnableBackgroundDetection=false;c.bEnableAdaptiveQuant=false;c.uiIntraPeriod=0;
  c.sSpatialLayers[0].iVideoWidth=w;c.sSpatialLayers[0].iVideoHeight=h;c.sSpatialLayers[0].fFrameRate=30;
  c.sSpatialLayers[0].uiProfileIdc=cabac?PRO_HIGH:PRO_BASELINE;c.sSpatialLayers[0].iDLayerQp=25;
  c.sSpatialLayers[0].sSliceArgument.uiSliceMode=SM_FIXEDSLCNUM_SLICE;
  c.sSpatialLayers[0].sSliceArgument.uiSliceNum=slices;
  REQUIRE(e->InitializeExt(&c)==0);
  SDecodingParam d={}; d.eEcActiveIdc=ERROR_CON_DISABLE; d.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;
  REQUIRE(a->Initialize(&d)==0);REQUIRE(b->Initialize(&d)==0);
  std::vector<unsigned char> raw(w*h*3/2);uint64_t rolling=0;unsigned frames=0,pps=0,bytes=0,intra=0,inter=0;
  std::vector<std::vector<unsigned char> > pictures_a,pictures_b;
  for(int f=0;f<6;++f) {
    for(size_t i=0;i<raw.size();++i) raw[i]=(unsigned char)(((i*13+(i/w)*31+(i%w>20+f*3?73:0))^(f*11))&255);
    SSourcePicture pic={};pic.iColorFormat=videoFormatI420;pic.iPicWidth=w;pic.iPicHeight=h;
    pic.iStride[0]=w;pic.iStride[1]=pic.iStride[2]=w/2;
    pic.pData[0]=raw.data();pic.pData[1]=raw.data()+w*h;pic.pData[2]=raw.data()+w*h*5/4;pic.uiTimeStamp=f*33;
    SFrameBSInfo out={}; REQUIRE(e->EncodeFrame(&pic,&out)==0); REQUIRE(out.eFrameType!=videoFrameTypeSkip);
    intra+=out.eFrameType==videoFrameTypeIDR||out.eFrameType==videoFrameTypeI;inter+=out.eFrameType==videoFrameTypeP;
    std::vector<unsigned char> bs;
    for(int l=0;l<out.iLayerNum;++l) { size_t offset=0;const SLayerBSInfo& layer=out.sLayerInfo[l];
      for(int n=0;n<layer.iNalCount;++n) { int len=layer.pNalLengthInByte[n];const unsigned char* p=layer.pBsBuf+offset;
        REQUIRE(len>4);size_t start=p[2]==1?3:4;
        if((p[start]&31)==8) { REQUIRE(pps_transform(p+start+1,len-start-1)==0);++pps; }
        bs.insert(bs.end(),p,p+len);offset+=len;
      }
    }
    REQUIRE(!bs.empty());bytes+=bs.size();unsigned char *pa[3]={},*pb[3]={};SBufferInfo ia={},ib={};
    ia.uiInBsTimeStamp=ib.uiInBsTimeStamp=pic.uiTimeStamp;
    REQUIRE(a->DecodeFrame2(bs.data(),bs.size(),pa,&ia)==dsErrorFree);
    REQUIRE(b->DecodeFrame2(bs.data(),bs.size(),pb,&ib)==dsErrorFree);
    if(ia.iBufferStatus) pictures_a.push_back(snapshot(pa,ia));
    if(ib.iBufferStatus) pictures_b.push_back(snapshot(pb,ib));
    ia=SBufferInfo();ib=SBufferInfo();
    REQUIRE(a->DecodeFrame2(NULL,0,pa,&ia)==dsErrorFree);
    REQUIRE(b->DecodeFrame2(NULL,0,pb,&ib)==dsErrorFree);
    if(ia.iBufferStatus) pictures_a.push_back(snapshot(pa,ia));
    if(ib.iBufferStatus) pictures_b.push_back(snapshot(pb,ib));
    ++frames;
  }
  ISVCDecoder* decoders[]={a,b};
  std::vector<std::vector<unsigned char> >* pictures[]={&pictures_a,&pictures_b};
  for(int k=0;k<2;++k) {
    int eos=1;REQUIRE(decoders[k]->SetOption(DECODER_OPTION_END_OF_STREAM,&eos)==0);
    for(int drain=0;drain<10;++drain) {int remaining=0;
      REQUIRE(decoders[k]->GetOption(DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER,&remaining)==0);
      if(!remaining) break;
      unsigned char* plane[3]={};SBufferInfo info={};
      REQUIRE(decoders[k]->FlushFrame(plane,&info)==dsErrorFree);
      REQUIRE(info.iBufferStatus==1);pictures[k]->push_back(snapshot(plane,info));
    }
  }
  REQUIRE(pictures_a==pictures_b);REQUIRE(pictures_a.size()==frames);
  for(size_t f=0;f<pictures_a.size();++f) {REQUIRE(pictures_a[f].size()==raw.size());rolling^=digest(pictures_a[f])+f;}
  REQUIRE(pps>0&&intra>0&&inter>0&&frames==6&&rolling!=0);
  std::printf("OPENH264 mode=%s slices=%d frames=%u I=%u P=%u bytes=%u digest=%llu legacy_pps_transform8x8=0\n",
    cabac?"CABAC-High":"CAVLC-Baseline",slices,frames,intra,inter,bytes,(unsigned long long)rolling);
  e->Uninitialize();a->Uninitialize();b->Uninitialize();WelsDestroySVCEncoder(e);WelsDestroyDecoder(a);WelsDestroyDecoder(b);
}
int main() {try {run(0,1);run(1,4);std::puts("OPENH264_BASELINE_COMPLETE");return 0;}
  catch(const std::exception& e) {std::fprintf(stderr,"baseline failure: %s\n",e.what());return 1;}}
