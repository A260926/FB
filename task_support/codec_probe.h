#ifndef TASK_CODEC_PROBE_H
#define TASK_CODEC_PROBE_H
#include "codec_api.h"
#include <cstdio>
#include <cstring>
#include <vector>
inline bool probe_params(ISVCEncoder* e, SEncParamExt& p) {
  std::memset(&p,0,sizeof(p));
  if(e->GetDefaultParams(&p)) return false;
  p.iUsageType=CAMERA_VIDEO_REAL_TIME; p.iPicWidth=64; p.iPicHeight=64;
  p.fMaxFrameRate=25; p.iRCMode=RC_OFF_MODE; p.iTemporalLayerNum=1;
  p.iSpatialLayerNum=1; p.iMultipleThreadIdc=1; p.bEnableFrameSkip=false;
  p.iNumRefFrame=1; p.uiIntraPeriod=16; p.bEnableDenoise=false;
  SSpatialLayerConfig& l=p.sSpatialLayers[0];
  l.iVideoWidth=64;l.iVideoHeight=64;l.fFrameRate=25;
  l.uiProfileIdc=PRO_HIGH; l.iDLayerQp=24;
  l.sSliceArgument.uiSliceMode=SM_SINGLE_SLICE;
  return true;
}
inline int probe_codec(SEncParamExt& p, int frames) {
  ISVCEncoder* e=0; ISVCDecoder* d=0;
  if(WelsCreateSVCEncoder(&e) || !e || WelsCreateDecoder(&d) || !d) return 11;
  int quiet=WELS_LOG_QUIET; e->SetOption(ENCODER_OPTION_TRACE_LEVEL,&quiet);
  d->SetOption(DECODER_OPTION_TRACE_LEVEL,&quiet);
  if(e->InitializeExt(&p)) return 12;
  SDecodingParam dp;std::memset(&dp,0,sizeof(dp));
  dp.uiTargetDqLayer=255;dp.eEcActiveIdc=ERROR_CON_DISABLE;
  dp.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;
  if(d->Initialize(&dp)) return 13;
  std::vector<unsigned char> pixels(64*64*3/2);
  SSourcePicture pic;std::memset(&pic,0,sizeof(pic));
  pic.iColorFormat=videoFormatI420;pic.iPicWidth=64;pic.iPicHeight=64;
  pic.iStride[0]=64;pic.iStride[1]=pic.iStride[2]=32;
  pic.pData[0]=pixels.data();pic.pData[1]=pixels.data()+4096;pic.pData[2]=pixels.data()+5120;
  unsigned long long hash=1469598103934665603ULL,bytes=0;int got=0;
  for(int f=0;f<frames;++f) {
    for(size_t i=0;i<pixels.size();++i)pixels[i]=(unsigned char)((i*13+(i/64)*7+f*19)^((i+f*17)>>3));
    pic.uiTimeStamp=f*40;
    SFrameBSInfo info;std::memset(&info,0,sizeof(info));
    if(e->EncodeFrame(&pic,&info) || info.eFrameType==videoFrameTypeSkip)return 14;
    std::vector<unsigned char> bs;
    for(int l=0;l<info.iLayerNum;++l){
      SLayerBSInfo& layer=info.sLayerInfo[l];int n=0;
      for(int k=0;k<layer.iNalCount;++k)n+=layer.pNalLengthInByte[k];
      bs.insert(bs.end(),layer.pBsBuf,layer.pBsBuf+n);
    }
    if(bs.empty())return 15;
    bytes+=bs.size();for(size_t i=0;i<bs.size();++i)hash=(hash^bs[i])*1099511628211ULL;
    unsigned char* out[3]={0,0,0};SBufferInfo bi;std::memset(&bi,0,sizeof(bi));
    DECODING_STATE state=d->DecodeFrame2(bs.data(),(int)bs.size(),out,&bi);
    if(!bi.iBufferStatus)state=(DECODING_STATE)(state|d->DecodeFrame2(0,0,out,&bi));
    if(state!=dsErrorFree)return 16;
    if(!bi.iBufferStatus && d->FlushFrame(out,&bi)!=dsErrorFree)return 20;
    if(bi.iBufferStatus!=1 || bi.UsrData.sSystemBuffer.iWidth!=64 || bi.UsrData.sSystemBuffer.iHeight!=64){std::fprintf(stderr,"decode f=%d status=%d width=%d height=%d bytes=%zu frameType=%d\n",f,bi.iBufferStatus,bi.UsrData.sSystemBuffer.iWidth,bi.UsrData.sSystemBuffer.iHeight,bs.size(),info.eFrameType);return 17;}
    for(int c=0;c<3;++c){int w=c?32:64,h=c?32:64,s=bi.UsrData.sSystemBuffer.iStride[c?1:0];
      if(!out[c])return 18;
      for(int y=0;y<h;++y)for(int x=0;x<w;++x)hash=(hash^out[c][y*s+x])*1099511628211ULL;
    }
    ++got;
  }
  e->Uninitialize();d->Uninitialize();WelsDestroySVCEncoder(e);WelsDestroyDecoder(d);
  if(got!=frames || bytes==0)return 19;
  std::printf("codec frames=%d decoded=%d bytes=%llu hash=%016llx\n",frames,got,bytes,hash);
  return 0;
}
#endif
