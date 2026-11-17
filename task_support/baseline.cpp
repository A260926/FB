#include "vp9/ratectrl_rtc.h"
#include "vpx/vp8cx.h"
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vpx_encoder.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>
#define REQUIRE(x) do {if(!(x)) throw std::runtime_error(#x);} while(0)

static void run(int temporal_layers) {
  const int w=96,h=64,count=16;
  libvpx::VP9RateControlRtcConfig r;
  r.width=w;r.height=h;r.target_bandwidth=80;r.framerate=30;
  r.min_quantizer=2;r.max_quantizer=56;r.max_intra_bitrate_pct=1000;
  r.aq_mode=0;r.frame_drop_thresh=0;r.ts_number_layers=temporal_layers;
  r.layer_target_bitrate[0]=temporal_layers==2?48:80;
  r.ts_rate_decimator[0]=temporal_layers==2?2:1;
  r.layer_target_bitrate[1]=80;r.ts_rate_decimator[1]=1;
  for(int i=0;i<temporal_layers;++i) {r.min_quantizers[i]=2;r.max_quantizers[i]=56;}
  auto a=libvpx::VP9RateControlRTC::Create(r),b=libvpx::VP9RateControlRTC::Create(r),large=libvpx::VP9RateControlRTC::Create(r);
  REQUIRE(a&&b&&large);
  vpx_codec_enc_cfg_t cfg={};REQUIRE(vpx_codec_enc_config_default(vpx_codec_vp9_cx(),&cfg,0)==VPX_CODEC_OK);
  cfg.g_w=w;cfg.g_h=h;cfg.g_threads=1;cfg.g_timebase.num=1;cfg.g_timebase.den=30;cfg.g_lag_in_frames=0;
  cfg.rc_end_usage=VPX_CBR;cfg.rc_target_bitrate=80;cfg.rc_min_quantizer=2;cfg.rc_max_quantizer=56;
  cfg.rc_buf_initial_sz=600;cfg.rc_buf_optimal_sz=600;cfg.rc_buf_sz=1000;
  cfg.rc_dropframe_thresh=0;cfg.kf_mode=VPX_KF_DISABLED;
  vpx_codec_ctx_t enc={},dec={};REQUIRE(vpx_codec_enc_init(&enc,vpx_codec_vp9_cx(),&cfg,0)==VPX_CODEC_OK);
  REQUIRE(vpx_codec_control(&enc,VP8E_SET_CPUUSED,7)==VPX_CODEC_OK);
  REQUIRE(vpx_codec_control(&enc,VP9E_SET_AQ_MODE,0)==VPX_CODEC_OK);
  REQUIRE(vpx_codec_control(&enc,VP8E_SET_ENABLEAUTOALTREF,0)==VPX_CODEC_OK);
  REQUIRE(vpx_codec_control(&enc,VP8E_SET_MAX_INTRA_BITRATE_PCT,1000)==VPX_CODEC_OK);
  REQUIRE(vpx_codec_control(&enc,VP9E_SET_RTC_EXTERNAL_RATECTRL,1)==VPX_CODEC_OK);
  vpx_codec_dec_cfg_t dc={};dc.threads=1;REQUIRE(vpx_codec_dec_init(&dec,vpx_codec_vp9_dx(),&dc,0)==VPX_CODEC_OK);
  vpx_image_t* image=vpx_img_alloc(NULL,VPX_IMG_FMT_I420,w,h,1);REQUIRE(image);
  unsigned packets=0,decoded=0,divergent=0;size_t bytes=0;int min_q=256,max_q=-1;uint64_t hash=1469598103934665603ULL;
  for(int f=0;f<count;++f) {
    libvpx::VP9FrameParamsQpRTC p={};p.spatial_layer_id=0;p.temporal_layer_id=temporal_layers==2?f%2:0;
    p.frame_type=f%8==0?libvpx::RcFrameType::kKeyFrame:libvpx::RcFrameType::kInterFrame;
    REQUIRE(a->ComputeQP(p)==libvpx::FrameDropDecision::kOk);
    REQUIRE(b->ComputeQP(p)==libvpx::FrameDropDecision::kOk);
    REQUIRE(large->ComputeQP(p)==libvpx::FrameDropDecision::kOk);
    int q=a->GetQP();REQUIRE(q>=0&&q<=255);REQUIRE(q==b->GetQP());
    REQUIRE(a->GetLoopfilterLevel()==b->GetLoopfilterLevel());
    divergent+=q!=large->GetQP();min_q=std::min(min_q,q);max_q=std::max(max_q,q);
    for(int c=0;c<3;++c) {int cw=w>>(c!=0),ch=h>>(c!=0);
      for(int y=0;y<ch;++y) for(int x=0;x<cw;++x)
        image->planes[c][y*image->stride[c]+x]=(unsigned char)(((x*19+y*31+f*17+c*59)^(x*y+f*7))&255);
    }
    vpx_enc_frame_flags_t flags=f%8==0?VPX_EFLAG_FORCE_KF:VP8_EFLAG_NO_UPD_GF|VP8_EFLAG_NO_UPD_ARF;
    REQUIRE(vpx_codec_encode(&enc,image,f,1,flags,VPX_DL_REALTIME)==VPX_CODEC_OK);
    vpx_codec_iter_t it=NULL;const vpx_codec_cx_pkt_t* pkt;size_t frame_bytes=0;unsigned frame_packets=0;
    while((pkt=vpx_codec_get_cx_data(&enc,&it))) if(pkt->kind==VPX_CODEC_CX_FRAME_PKT) {
      ++packets;++frame_packets;frame_bytes+=pkt->data.frame.sz;
      REQUIRE(vpx_codec_decode(&dec,(const uint8_t*)pkt->data.frame.buf,pkt->data.frame.sz,NULL,0)==VPX_CODEC_OK);
      vpx_codec_iter_t di=NULL;vpx_image_t* out;
      while((out=vpx_codec_get_frame(&dec,&di))) {REQUIRE(out->d_w==w&&out->d_h==h);++decoded;
        for(int c=0;c<3;++c) for(int y=0;y<(h>>(c!=0));++y) for(int x=0;x<(w>>(c!=0));++x)
          hash=(hash^out->planes[c][y*out->stride[c]+x])*1099511628211ULL;
      }
    }
    REQUIRE(frame_packets==1&&frame_bytes>0);bytes+=frame_bytes;
    a->PostEncodeUpdate(frame_bytes,p);b->PostEncodeUpdate(frame_bytes,p);large->PostEncodeUpdate(frame_bytes*4,p);
  }
  REQUIRE(packets==count&&decoded==count&&bytes>0&&min_q<max_q&&divergent>0);
  std::printf("VP9_RTC layers=%d frames=%d packets=%u decoded=%u bytes=%zu qp=%d..%d feedback_sensitive=%u pixel_digest=%llu\n",
    temporal_layers,count,packets,decoded,bytes,min_q,max_q,divergent,(unsigned long long)hash);
  vpx_img_free(image);REQUIRE(vpx_codec_destroy(&enc)==VPX_CODEC_OK);REQUIRE(vpx_codec_destroy(&dec)==VPX_CODEC_OK);
}
int main() {try {run(1);run(2);std::puts("VP9_RTC_BASELINE_COMPLETE");return 0;}
  catch(const std::exception& e) {std::fprintf(stderr,"baseline failure: %s\n",e.what());return 1;}}
