#include "codec/api/wels/codec_api.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

static std::vector<std::vector<unsigned char> > run(int seedExtra) {
  ISVCEncoder* enc=nullptr; WelsCreateSVCEncoder(&enc);
  int tl=WELS_LOG_ERROR; enc->SetOption(ENCODER_OPTION_TRACE_LEVEL,&tl);
  SEncParamExt p; enc->GetDefaultParams(&p);
  int W=64,H=64;
  p.iUsageType=CAMERA_VIDEO_REAL_TIME; p.iPicWidth=W; p.iPicHeight=H;
  p.iTargetBitrate=200000; p.iRCMode=RC_BITRATE_MODE; p.fMaxFrameRate=30;
  p.iSpatialLayerNum=1; p.iTemporalLayerNum=3; p.iMultipleThreadIdc=1;
  p.bEnableFrameSkip=false; p.iPaddingFlag=0; p.bEnableLongTermReference=false;
  p.bEnableDenoise=false; p.bEnableBackgroundDetection=false; p.bEnableSceneChangeDetect=false;
  p.bEnableAdaptiveQuant=false; p.eSpsPpsIdStrategy=CONSTANT_ID; p.iEntropyCodingModeFlag=1; p.uiIntraPeriod=0;
  p.sSpatialLayers[0].iVideoWidth=W; p.sSpatialLayers[0].iVideoHeight=H;
  p.sSpatialLayers[0].fFrameRate=30; p.sSpatialLayers[0].iSpatialBitrate=200000;
  p.sSpatialLayers[0].uiProfileIdc=PRO_HIGH;
  p.sSpatialLayers[0].sSliceArgument.uiSliceMode=SM_FIXEDSLCNUM_SLICE;
  p.sSpatialLayers[0].sSliceArgument.uiSliceNum=2;
  enc->InitializeExt(&p);
  std::vector<unsigned char> img(W*H*3/2);
  std::vector<std::vector<unsigned char> > aus;
  for(int n=0;n<16;n++){
    for(size_t k=0;k<img.size();k++) img[k]=(unsigned char)((k*13+n*17+(k/W)*(n+3)+seedExtra)&255);
    SSourcePicture s; memset(&s,0,sizeof(s));
    s.iColorFormat=videoFormatI420; s.iPicWidth=W; s.iPicHeight=H; s.uiTimeStamp=n*33;
    s.iStride[0]=W; s.iStride[1]=s.iStride[2]=W/2;
    s.pData[0]=img.data(); s.pData[1]=img.data()+W*H; s.pData[2]=img.data()+W*H*5/4;
    SFrameBSInfo o; memset(&o,0,sizeof(o));
    int r=enc->EncodeFrame(&s,&o);
    if(r){ printf("encode r=%d frame %d\n",r,n); break; }
    std::vector<unsigned char> au;
    for(int l=0;l<o.iLayerNum;l++){int len=0; for(int j=0;j<o.sLayerInfo[l].iNalCount;j++) len+=o.sLayerInfo[l].pNalLengthInByte[j];
      au.insert(au.end(),o.sLayerInfo[l].pBsBuf,o.sLayerInfo[l].pBsBuf+len);}
    aus.push_back(au);
  }
  enc->Uninitialize(); WelsDestroySVCEncoder(enc);
  return aus;
}

int main(){
  auto a=run(0), b=run(0);
  printf("frames %zu %zu\n",a.size(),b.size());
  int identical=1;
  for(size_t i=0;i<a.size()&&i<b.size();i++){
    bool eq=a[i]==b[i];
    if(!eq){ identical=0; printf("frame %zu DIFF sizes %zu vs %zu\n",i,a[i].size(),b[i].size()); }
    else printf("frame %zu identical bytes=%zu\n",i,a[i].size());
  }
  printf(identical?"DETERMINISTIC_YES\n":"DETERMINISTIC_NO\n");
  return identical?0:1;
}
