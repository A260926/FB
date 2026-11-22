#include "codec/api/wels/codec_api.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define REQUIRE(x) do { if(!(x)) { std::fprintf(stderr,"baseline failure line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static int run(int cabac) {
  ISVCEncoder* enc=nullptr; ISVCDecoder* dec=nullptr;
  REQUIRE(WelsCreateSVCEncoder(&enc)==0 && enc);
  REQUIRE(WelsCreateDecoder(&dec)==0 && dec);
  int trace=WELS_LOG_ERROR; enc->SetOption(ENCODER_OPTION_TRACE_LEVEL,&trace); dec->SetOption(DECODER_OPTION_TRACE_LEVEL,&trace);
  SEncParamExt p; REQUIRE(enc->GetDefaultParams(&p)==0);
  p.iUsageType=CAMERA_VIDEO_REAL_TIME; p.iPicWidth=64; p.iPicHeight=48;
  p.iTargetBitrate=100000; p.iRCMode=RC_BITRATE_MODE; p.fMaxFrameRate=30;
  p.iSpatialLayerNum=1; p.iTemporalLayerNum=3; p.iMultipleThreadIdc=1;
  p.bEnableFrameSkip=false; p.iPaddingFlag=0; p.bEnableLongTermReference=false;
  p.bEnableDenoise=false; p.bEnableBackgroundDetection=false; p.bEnableSceneChangeDetect=false; p.bEnableAdaptiveQuant=false;
  p.eSpsPpsIdStrategy=CONSTANT_ID; p.iEntropyCodingModeFlag=cabac; p.uiIntraPeriod=0;
  p.sSpatialLayers[0].iVideoWidth=64; p.sSpatialLayers[0].iVideoHeight=48;
  p.sSpatialLayers[0].fFrameRate=30; p.sSpatialLayers[0].iSpatialBitrate=100000;
  p.sSpatialLayers[0].uiProfileIdc=cabac?PRO_HIGH:PRO_BASELINE;
  p.sSpatialLayers[0].sSliceArgument.uiSliceMode=SM_FIXEDSLCNUM_SLICE;
  p.sSpatialLayers[0].sSliceArgument.uiSliceNum=2;
  REQUIRE(enc->InitializeExt(&p)==0);
  SDecodingParam dp; std::memset(&dp,0,sizeof(dp)); dp.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC; dp.eEcActiveIdc=ERROR_CON_DISABLE;
  REQUIRE(dec->Initialize(&dp)==0);
  std::vector<unsigned char> img(64*48*3/2); unsigned long long bytes=0,hash=14695981039346656037ull; int decoded=0;
  for(int n=0;n<16;n++) {
    for(size_t k=0;k<img.size();k++) img[k]=(unsigned char)((k*13+n*17+(k/64)*(n+3))&255);
    SSourcePicture src; std::memset(&src,0,sizeof(src)); src.iColorFormat=videoFormatI420; src.iPicWidth=64; src.iPicHeight=48; src.uiTimeStamp=n*33;
    src.iStride[0]=64; src.iStride[1]=src.iStride[2]=32;
    src.pData[0]=img.data(); src.pData[1]=img.data()+64*48; src.pData[2]=img.data()+64*48*5/4;
    SFrameBSInfo out; std::memset(&out,0,sizeof(out)); REQUIRE(enc->EncodeFrame(&src,&out)==0); REQUIRE(out.iFrameSizeInBytes>0);
    std::vector<unsigned char> au;
    for(int l=0;l<out.iLayerNum;l++) { int len=0; for(int j=0;j<out.sLayerInfo[l].iNalCount;j++) len+=out.sLayerInfo[l].pNalLengthInByte[j];
      au.insert(au.end(),out.sLayerInfo[l].pBsBuf,out.sLayerInfo[l].pBsBuf+len); }
    REQUIRE(au.size()==(size_t)out.iFrameSizeInBytes); bytes+=au.size();
    unsigned char* planes[3]={nullptr,nullptr,nullptr}; SBufferInfo bi; std::memset(&bi,0,sizeof(bi));
    REQUIRE(dec->DecodeFrameNoDelay(au.data(),(int)au.size(),planes,&bi)==dsErrorFree);
    if(bi.iBufferStatus==0) REQUIRE(dec->FlushFrame(planes,&bi)==dsErrorFree);
    REQUIRE(bi.iBufferStatus==1); decoded++;
    REQUIRE(bi.UsrData.sSystemBuffer.iWidth==64 && bi.UsrData.sSystemBuffer.iHeight==48);
    for(int c=0;c<3;c++) for(int y=0;y<(c?24:48);y++) for(int x=0;x<(c?32:64);x++) {hash^=planes[c][y*bi.UsrData.sSystemBuffer.iStride[c?1:0]+x];hash*=1099511628211ull;}
  }
  REQUIRE(decoded==16 && bytes>0); dec->Uninitialize(); WelsDestroyDecoder(dec); enc->Uninitialize(); WelsDestroySVCEncoder(enc);
  std::printf("NORMAL_SEQUENCE entropy=%s frames=16 decoded=%d annexb=%llu pixel_fnv=%016llx\n",cabac?"CABAC":"CAVLC",decoded,bytes,hash); return 0;
}
int main(){if(run(0)||run(1)) return 1; std::puts("BASELINE_COMPLETE production=EncodeFrame+DecodeFrameNoDelay");return 0;}
