#include "codec/api/wels/codec_api.h"
#include <cstdio>
#include <cstring>
#include <vector>
static ISVCEncoder* mk(int rcmode,int scene){
  ISVCEncoder* e=0; WelsCreateSVCEncoder(&e);
  int lv=WELS_LOG_ERROR; e->SetOption(ENCODER_OPTION_TRACE_LEVEL,&lv);
  SEncParamExt p; e->GetDefaultParams(&p);
  p.iUsageType=CAMERA_VIDEO_REAL_TIME; p.iPicWidth=64; p.iPicHeight=64;
  p.iTargetBitrate=200000; p.iRCMode=(RC_MODES)rcmode; p.fMaxFrameRate=30;
  p.iSpatialLayerNum=1; p.iTemporalLayerNum=1; p.iMultipleThreadIdc=1;
  p.bEnableFrameSkip=false; p.iPaddingFlag=0; p.bEnableLongTermReference=false;
  p.bEnableDenoise=false; p.bEnableBackgroundDetection=false;
  p.bEnableSceneChangeDetect=scene?true:false; p.eSpsPpsIdStrategy=CONSTANT_ID;
  p.sSpatialLayers[0].iVideoWidth=64; p.sSpatialLayers[0].iVideoHeight=64;
  p.sSpatialLayers[0].fFrameRate=30; p.sSpatialLayers[0].iSpatialBitrate=200000;
  e->InitializeExt(&p); return e;
}
static int enc(ISVCEncoder* e){
  int W=64,H=64; std::vector<unsigned char> im(W*H*3/2);
  for(size_t k=0;k<im.size();k++) im[k]=(unsigned char)(k*7&255);
  SSourcePicture s; memset(&s,0,sizeof(s));
  s.iColorFormat=videoFormatI420; s.iPicWidth=W; s.iPicHeight=H;
  s.iStride[0]=W; s.iStride[1]=s.iStride[2]=W/2;
  s.pData[0]=im.data(); s.pData[1]=im.data()+W*H; s.pData[2]=im.data()+W*H*5/4;
  SFrameBSInfo o; memset(&o,0,sizeof(o));
  return e->EncodeFrame(&s,&o);
}
int main(){
  int fail=0;
  // RC quality mode -> enable must reject
  ISVCEncoder* a=mk(RC_QUALITY_MODE,0);
  SFrameBudget b{1,40,50}; int r=a->SetOption(ENCODER_OPTION_FRAME_BUDGET,&b);
  printf("rcquality setbudget r=%d (want nonzero)\n",r); if(r==0)fail++;
  int er=enc(a); printf("encode after rejected enable r=%d (want 0 normal)\n",er); if(er!=0)fail++;
  SFrameBudget rb; memset(&rb,0x55,sizeof(rb)); a->GetOption(ENCODER_OPTION_FRAME_BUDGET,&rb);
  printf("stored maxbytes=%d (want 0)\n",rb.iMaxBytes); if(rb.iMaxBytes!=0)fail++;
  a->Uninitialize(); WelsDestroySVCEncoder(a);
  // scene detect on (otherwise bitrate) -> reject
  ISVCEncoder* c=mk(RC_BITRATE_MODE,1);
  int r2=c->SetOption(ENCODER_OPTION_FRAME_BUDGET,&b);
  printf("scene-on setbudget r=%d (want nonzero)\n",r2); if(r2==0)fail++;
  c->Uninitialize(); WelsDestroySVCEncoder(c);
  // unsupported dimension 48? 48 is 16-multiple (3*16). use 72 (not multiple)
  ISVCEncoder* d=0; WelsCreateSVCEncoder(&d);
  { int lv=WELS_LOG_ERROR; d->SetOption(ENCODER_OPTION_TRACE_LEVEL,&lv);
    SEncParamExt p; d->GetDefaultParams(&p);
    p.iUsageType=CAMERA_VIDEO_REAL_TIME; p.iPicWidth=72; p.iPicHeight=64;
    p.iRCMode=RC_BITRATE_MODE; p.iMultipleThreadIdc=1; p.iTemporalLayerNum=1;
    p.bEnableFrameSkip=false;p.iPaddingFlag=0;p.bEnableLongTermReference=false;
    p.bEnableDenoise=false;p.bEnableBackgroundDetection=false;p.bEnableSceneChangeDetect=false;
    p.eSpsPpsIdStrategy=CONSTANT_ID;
    p.sSpatialLayers[0].iVideoWidth=72;p.sSpatialLayers[0].iVideoHeight=64;p.sSpatialLayers[0].fFrameRate=30;
    int ir=d->InitializeExt(&p);
    printf("width72 init r=%d\n",ir);
    SEncParamExt g; d->GetOption(ENCODER_OPTION_SVC_ENCODE_PARAM_EXT,&g);
    printf("final iPicWidth=%d layerW=%d\n",g.iPicWidth,g.sSpatialLayers[0].iVideoWidth);
  }
  int r3=d->SetOption(ENCODER_OPTION_FRAME_BUDGET,&b);
  printf("width72 setbudget r=%d (want nonzero)\n",r3); if(r3==0)fail++;
  d->Uninitialize(); WelsDestroySVCEncoder(d);
  printf(fail?"CONFIG_PROBE FAIL\n":"CONFIG_PROBE PASS\n");
  return fail?1:0;
}
