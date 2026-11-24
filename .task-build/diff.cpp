#include "codec/api/wels/codec_api.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

struct Ctx { ISVCEncoder* e; };

static ISVCEncoder* makeEnc(int W,int H,int cabac,int tl,int slices){
  ISVCEncoder* e=nullptr; WelsCreateSVCEncoder(&e);
  int lv=WELS_LOG_ERROR; e->SetOption(ENCODER_OPTION_TRACE_LEVEL,&lv);
  SEncParamExt p; e->GetDefaultParams(&p);
  p.iUsageType=CAMERA_VIDEO_REAL_TIME; p.iPicWidth=W; p.iPicHeight=H;
  p.iTargetBitrate=400000; p.iRCMode=RC_BITRATE_MODE; p.fMaxFrameRate=30;
  p.iSpatialLayerNum=1; p.iTemporalLayerNum=tl; p.iMultipleThreadIdc=1;
  p.bEnableFrameSkip=false; p.iPaddingFlag=0; p.bEnableLongTermReference=false;
  p.bEnableDenoise=false; p.bEnableBackgroundDetection=false; p.bEnableSceneChangeDetect=false;
  p.bEnableAdaptiveQuant=false; p.eSpsPpsIdStrategy=CONSTANT_ID;
  p.iEntropyCodingModeFlag=cabac; p.uiIntraPeriod=0;
  p.sSpatialLayers[0].iVideoWidth=W; p.sSpatialLayers[0].iVideoHeight=H;
  p.sSpatialLayers[0].fFrameRate=30; p.sSpatialLayers[0].iSpatialBitrate=400000;
  p.sSpatialLayers[0].uiProfileIdc=cabac?PRO_HIGH:PRO_BASELINE;
  if(slices<=1){ p.sSpatialLayers[0].sSliceArgument.uiSliceMode=SM_SINGLE_SLICE; }
  else { p.sSpatialLayers[0].sSliceArgument.uiSliceMode=SM_FIXEDSLCNUM_SLICE;
         p.sSpatialLayers[0].sSliceArgument.uiSliceNum=slices; }
  int r=e->InitializeExt(&p);
  if(r){ printf("init fail %d\n",r); return nullptr; }
  return e;
}

static void texture(std::vector<unsigned char>& img,int W,int H,int n){
  img.resize(W*H*3/2);
  for(int y=0;y<H;y++)for(int x=0;x<W;x++){
    size_t k=y*W+x;
    img[k]=(unsigned char)((x*3 + y*7 + n*13 + ((x+y)%17)*n)&255);
  }
  for(size_t k=(size_t)W*H;k<img.size();k++)
    img[k]=(unsigned char)((k*11+n*19+(k/W)*(n+5))&255);
}

static int encodeAU(ISVCEncoder* e,const std::vector<unsigned char>& img,int W,int H,long long ts,
                    std::vector<unsigned char>& au, EVideoFrameType* ft=nullptr){
  SSourcePicture s; memset(&s,0,sizeof(s));
  s.iColorFormat=videoFormatI420; s.iPicWidth=W; s.iPicHeight=H; s.uiTimeStamp=ts;
  s.iStride[0]=W; s.iStride[1]=s.iStride[2]=W/2;
  s.pData[0]=const_cast<unsigned char*>(img.data());
  s.pData[1]=const_cast<unsigned char*>(img.data()+W*H);
  s.pData[2]=const_cast<unsigned char*>(img.data()+W*H*5/4);
  SFrameBSInfo o; memset(&o,0,sizeof(o));
  int r=e->EncodeFrame(&s,&o);
  au.clear();
  if(r==0){
    for(int l=0;l<o.iLayerNum;l++){int len=0;
      for(int j=0;j<o.sLayerInfo[l].iNalCount;j++) len+=o.sLayerInfo[l].pNalLengthInByte[j];
      au.insert(au.end(),o.sLayerInfo[l].pBsBuf,o.sLayerInfo[l].pBsBuf+len);}
    if(ft)*ft=o.eFrameType;
  }
  return r;
}

static void setBudget(ISVCEncoder* e,int mx,int fq,int lq){
  SFrameBudget b; b.iMaxBytes=mx; b.iFirstQp=fq; b.iLastQp=lq;
  int r=e->SetOption(ENCODER_OPTION_FRAME_BUDGET,&b);
  if(r) printf("setbudget rejected %d {%d,%d,%d}\n",r,mx,fq,lq);
}

int main(int argc,char**argv){
  int W=argc>1?atoi(argv[1]):128;
  int H=argc>2?atoi(argv[2]):128;
  int cabac=argc>3?atoi(argv[3]):1;
  int tl=argc>4?atoi(argv[4]):3;
  int slices=argc>5?atoi(argv[5]):4;
  const int N=24, Q=30;

  ISVCEncoder* A=makeEnc(W,H,cabac,tl,slices);
  ISVCEncoder* B=makeEnc(W,H,cabac,tl,slices);
  if(!A||!B) return 2;
  setBudget(B,0x3fffffff,Q,Q); // control always encodes at fixed Q via same path

  int fails=0, committed=0, mism=0;
  for(int n=0;n<N;n++){
    std::vector<unsigned char> img; texture(img,W,H,n);
    long long ts=n*33;

    // Scripted disturbance on A at selected frames: impossible budget full fail,
    // sometimes force IDR pending first.
    bool disturb = (n%3==1);
    bool forceIdr = (n==7);
    if(forceIdr){ A->ForceIntraFrame(true); B->ForceIntraFrame(true); }
    if(disturb){
      setBudget(A,1,Q,Q+4);
      std::vector<unsigned char> junk;
      int r=encodeAU(A,img,W,H,ts,junk);
      if(r!=cmFrameBudgetExceeded){ printf("n=%d expected budgetexceed got %d\n",n,r); return 3; }
      if(!junk.empty()){ printf("n=%d failure produced bytes\n",n); return 4; }
      ++fails;
      if(n%2==1){
        // retry SAME image with a fit budget
        setBudget(A,0x3fffffff,Q,Q);
        std::vector<unsigned char> aa,bb;
        int ra=encodeAU(A,img,W,H,ts,aa);
        int rb=encodeAU(B,img,W,H,ts,bb);
        if(ra||rb){printf("encode err ra=%d rb=%d n=%d\n",ra,rb,n);return 5;}
        committed++;
        if(aa!=bb){ mism++; printf("MISMATCH same-retry n=%d sizes A=%zu B=%zu\n",n,aa.size(),bb.size()); }
        else printf("ok same-retry n=%d bytes=%zu\n",n,aa.size());
        continue;
      } else {
        // give a DIFFERENT next image
        int m=n+1;
        std::vector<unsigned char> img2; texture(img2,W,H,m);
        long long ts2=m*33;
        setBudget(A,0x3fffffff,Q,Q);
        std::vector<unsigned char> aa,bb;
        int ra=encodeAU(A,img2,W,H,ts2,aa);
        int rb=encodeAU(B,img2,W,H,ts2,bb);
        if(ra||rb){printf("encode err2 ra=%d rb=%d\n",ra,rb);return 5;}
        committed++;
        if(aa!=bb){ mism++; printf("MISMATCH change-img n=%d sizes A=%zu B=%zu\n",n,aa.size(),bb.size()); }
        else printf("ok change-img after fail n=%d bytes=%zu\n",n,aa.size());
        n++; // consumed m
        continue;
      }
    }
    // ordinary matched frame
    setBudget(A,0x3fffffff,Q,Q);
    std::vector<unsigned char> aa,bb;
    int ra=encodeAU(A,img,W,H,ts,aa);
    int rb=encodeAU(B,img,W,H,ts,bb);
    if(ra||rb){printf("encode err ra=%d rb=%d n=%d\n",ra,rb,n);return 5;}
    committed++;
    if(aa!=bb){ mism++; printf("MISMATCH normal n=%d sizes A=%zu B=%zu\n",n,aa.size(),bb.size()); }
  }
  printf("DONE %dx%d cabac=%d tl=%d sl=%d fails=%d committed=%d mismatches=%d %s\n",
         W,H,cabac,tl,slices,fails,committed,mism,mism?"FAIL":"PASS");
  return mism?1:0;
}
