#include "codec/api/wels/codec_api.h"
#include <cstdio>
#include <cstring>
#include <vector>
int main(){
  int fail=0;
  for(int q=22;q<=44;q+=11){
    ISVCEncoder* e=0; WelsCreateSVCEncoder(&e);
    int lv=WELS_LOG_ERROR; e->SetOption(ENCODER_OPTION_TRACE_LEVEL,&lv);
    SEncParamExt p; e->GetDefaultParams(&p);
    p.iUsageType=CAMERA_VIDEO_REAL_TIME;p.iPicWidth=128;p.iPicHeight=128;
    p.iTargetBitrate=400000;p.iRCMode=RC_BITRATE_MODE;p.fMaxFrameRate=30;
    p.iSpatialLayerNum=1;p.iTemporalLayerNum=1;p.iMultipleThreadIdc=1;
    p.bEnableFrameSkip=false;p.iPaddingFlag=0;p.bEnableLongTermReference=false;
    p.bEnableDenoise=false;p.bEnableBackgroundDetection=false;p.bEnableSceneChangeDetect=false;
    p.eSpsPpsIdStrategy=CONSTANT_ID;
    p.sSpatialLayers[0].iVideoWidth=128;p.sSpatialLayers[0].iVideoHeight=128;
    p.sSpatialLayers[0].fFrameRate=30;p.sSpatialLayers[0].iSpatialBitrate=400000;
    e->InitializeExt(&p);
    SFrameBudget b{0x3fffffff,q,q}; e->SetOption(ENCODER_OPTION_FRAME_BUDGET,&b);
    int W=128,H=128; std::vector<unsigned char> im(W*H*3/2);
    for(int y=0;y<H;y++)for(int x=0;x<W;x++) im[y*W+x]=(unsigned char)((x*3+y*7+((x+y)%17)*5)&255);
    for(size_t k=W*H;k<im.size();k++) im[k]=(unsigned char)(k*11&255);
    SSourcePicture s; memset(&s,0,sizeof(s));
    s.iColorFormat=videoFormatI420;s.iPicWidth=W;s.iPicHeight=H;
    s.iStride[0]=W;s.iStride[1]=s.iStride[2]=W/2;
    s.pData[0]=im.data();s.pData[1]=im.data()+W*H;s.pData[2]=im.data()+W*H*5/4;
    SFrameBSInfo o; memset(&o,0,sizeof(o));
    int r=e->EncodeFrame(&s,&o);
    SEncoderStatistics st; memset(&st,0,sizeof(st)); e->GetOption(ENCODER_OPTION_GET_STATISTICS,&st);
    printf("q=%d encR=%d size=%d statsAvgQP=%u idrSent=%u inCnt=%u\n",q,r,o.iFrameSizeInBytes,
           st.uiAverageFrameQP,st.uiIDRSentNum,st.uiInputFrameCount);
    if(r||(int)st.uiAverageFrameQP!=q){printf("  QP STAT MISMATCH\n");fail++;}
    e->Uninitialize(); WelsDestroySVCEncoder(e);
  }
  printf(fail?"QPSTAT FAIL\n":"QPSTAT PASS\n"); return fail?1:0;
}
