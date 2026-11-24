#include "codec/api/wels/codec_api.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <map>

static ISVCEncoder* makeEnc(int W,int H,int cabac,int slices){
  ISVCEncoder* e=nullptr; WelsCreateSVCEncoder(&e);
  int lv=WELS_LOG_ERROR; e->SetOption(ENCODER_OPTION_TRACE_LEVEL,&lv);
  SEncParamExt p; e->GetDefaultParams(&p);
  p.iUsageType=CAMERA_VIDEO_REAL_TIME; p.iPicWidth=W; p.iPicHeight=H;
  p.iTargetBitrate=400000; p.iRCMode=RC_BITRATE_MODE; p.fMaxFrameRate=30;
  p.iSpatialLayerNum=1; p.iTemporalLayerNum=1; p.iMultipleThreadIdc=1;
  p.bEnableFrameSkip=false; p.iPaddingFlag=0; p.bEnableLongTermReference=false;
  p.bEnableDenoise=false; p.bEnableBackgroundDetection=false; p.bEnableSceneChangeDetect=false;
  p.bEnableAdaptiveQuant=false; p.eSpsPpsIdStrategy=CONSTANT_ID;
  p.iEntropyCodingModeFlag=cabac; p.uiIntraPeriod=0;
  p.sSpatialLayers[0].iVideoWidth=W; p.sSpatialLayers[0].iVideoHeight=H;
  p.sSpatialLayers[0].fFrameRate=30; p.sSpatialLayers[0].iSpatialBitrate=400000;
  p.sSpatialLayers[0].uiProfileIdc=cabac?PRO_HIGH:PRO_BASELINE;
  if(slices<=1) p.sSpatialLayers[0].sSliceArgument.uiSliceMode=SM_SINGLE_SLICE;
  else { p.sSpatialLayers[0].sSliceArgument.uiSliceMode=SM_FIXEDSLCNUM_SLICE;
         p.sSpatialLayers[0].sSliceArgument.uiSliceNum=slices; }
  if(e->InitializeExt(&p)) return nullptr;
  return e;
}
static void tex(std::vector<unsigned char>& im,int W,int H){
  im.resize(W*H*3/2);
  for(int y=0;y<H;y++)for(int x=0;x<W;x++) im[y*W+x]=(unsigned char)((x*3+y*7+((x+y)%17)*5)&255);
  for(size_t k=(size_t)W*H;k<im.size();k++) im[k]=(unsigned char)((k*11+(k/W)*7)&255);
}
static int enc(ISVCEncoder* e,const std::vector<unsigned char>& im,int W,int H,SFrameBSInfo& o){
  SSourcePicture s; memset(&s,0,sizeof(s));
  s.iColorFormat=videoFormatI420; s.iPicWidth=W; s.iPicHeight=H; s.uiTimeStamp=0;
  s.iStride[0]=W; s.iStride[1]=s.iStride[2]=W/2;
  s.pData[0]=const_cast<unsigned char*>(im.data());
  s.pData[1]=const_cast<unsigned char*>(im.data()+W*H);
  s.pData[2]=const_cast<unsigned char*>(im.data()+W*H*5/4);
  memset(&o,0,sizeof(o));
  return e->EncodeFrame(&s,&o);
}
static std::vector<unsigned char> au(SFrameBSInfo& o){
  std::vector<unsigned char> a;
  for(int l=0;l<o.iLayerNum;l++){int len=0;
    for(int j=0;j<o.sLayerInfo[l].iNalCount;j++) len+=o.sLayerInfo[l].pNalLengthInByte[j];
    a.insert(a.end(),o.sLayerInfo[l].pBsBuf,o.sLayerInfo[l].pBsBuf+len);}
  return a;
}
static void bud(ISVCEncoder* e,int mx,int f,int l){SFrameBudget b; b.iMaxBytes=mx;b.iFirstQp=f;b.iLastQp=l; e->SetOption(ENCODER_OPTION_FRAME_BUDGET,&b);}
static void kill(ISVCEncoder* e){ e->Uninitialize(); WelsDestroySVCEncoder(e); }

int main(){
  const int W=128,H=128;
  std::vector<unsigned char> im; tex(im,W,H);
  int fail=0,checks=0;
  auto check=[&](bool cond,const char* msg){++checks; if(!cond){++fail;printf("FAIL %s\n",msg);}};

  for(int cabac=0;cabac<2;cabac++) for(int slices : {1,3}){
    std::map<int,std::vector<unsigned char> > ora; std::map<int,int> sz;
    for(int q=18;q<=46;q++){
      ISVCEncoder* o=makeEnc(W,H,cabac,slices); bud(o,0x3fffffff,q,q);
      SFrameBSInfo info; int r=enc(o,im,W,H,info);
      if(r){printf("oracle fail q=%d r=%d\n",q,r);return 2;}
      ora[q]=au(info); sz[q]=info.iFrameSizeInBytes; kill(o);
    }
    printf("cfg cabac=%d sl=%d q22=%d q30=%d q38=%d q44=%d\n",cabac,slices,sz[22],sz[30],sz[38],sz[44]);

    for(int q : {22,30,38,44}){
      { // exact fit, fresh encoder
        ISVCEncoder* a=makeEnc(W,H,cabac,slices); bud(a,sz[q],q,q);
        SFrameBSInfo i; int r=enc(a,im,W,H,i);
        check(r==0,"exact-fit ret0");
        check(i.iFrameSizeInBytes==sz[q],"exact-fit size");
        check(au(i)==ora[q],"exact-fit AU");
        kill(a);
      }
      { // one byte less, single q -> exceeded, zeroed
        ISVCEncoder* a=makeEnc(W,H,cabac,slices); bud(a,sz[q]-1,q,q);
        SFrameBSInfo i; int r=enc(a,im,W,H,i);
        check(r==cmFrameBudgetExceeded,"oneless exceeded");
        check(i.iFrameSizeInBytes==0 && i.iLayerNum==0,"oneless zero");
        int tn=0; for(int l=0;l<i.iLayerNum;l++) tn+=i.sLayerInfo[l].iNalCount;
        check(tn==0,"oneless nal0");
        // retry same image with fit budget on SAME encoder after failure
        bud(a,sz[q],q,q);
        SFrameBSInfo j; int rj=enc(a,im,W,H,j);
        check(rj==0,"retry ret0");
        check(au(j)==ora[q],"retry AU equals fresh oracle");
        kill(a);
      }
    }
    for(int capq : {26,34,42}){
      int cap=sz[capq]; int expect=-1;
      for(int q=18;q<=46;q++) if(sz[q]<=cap){expect=q;break;}
      ISVCEncoder* a=makeEnc(W,H,cabac,slices); bud(a,cap,18,46);
      SFrameBSInfo i; int r=enc(a,im,W,H,i);
      check(r==0,"range ret0"); check(expect>=0,"expect");
      if(expect>=0){ check(i.iFrameSizeInBytes==sz[expect],"range size");
        check(au(i)==ora[expect],"range AU"); check(i.iFrameSizeInBytes<=cap,"range cap"); }
      kill(a);
    }
    { ISVCEncoder* a=makeEnc(W,H,cabac,slices); bud(a,1,35,51);
      SFrameBSInfo i; int rr=enc(a,im,W,H,i);
      check(rr==cmFrameBudgetExceeded,"tiny exceeded");
      check(i.iLayerNum==0 && i.iFrameSizeInBytes==0,"tiny zeroed");
      kill(a); }
  }
  printf("QP_PROBE checks=%d failures=%d %s\n",checks,fail,fail?"FAIL":"PASS");
  return fail?1:0;
}
