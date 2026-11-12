#include "temporal.hpp"
#include <stdexcept>
#include <cmath>
#include <algorithm>
namespace render {
namespace {
bool finite_v2(V2 v){return std::isfinite(v.x)&&std::isfinite(v.y);}
struct YCoCg {double y,co,cg;};
YCoCg to_ycocg(V3 c){
 return {(c.x+2*c.y+c.z)/4,(c.x-c.z)/2,(-c.x+2*c.y-c.z)/4};
}
V3 to_rgb(YCoCg c){
 return {c.y+c.co-c.cg,c.y+c.cg,c.y-c.co-c.cg};
}
}
void TemporalAccumulator::reset(){
 history_color_=Image();
 history_depth_.clear();
 history_object_.clear();
 has_history_=false;
}
Image TemporalAccumulator::resolve(const Frame& frame,const TemporalSettings& settings){
 const int w=frame.hdr.width,h=frame.hdr.height;
 const size_t n=size_t(w)*h;
 if(!(settings.history_weight>=0&&settings.history_weight<=1)||
    !(settings.depth_relative_threshold>=0&&std::isfinite(settings.depth_relative_threshold)))
  throw std::invalid_argument("invalid temporal settings");
 if(!(std::isfinite(frame.exposure)&&frame.exposure>0))
  throw std::invalid_argument("invalid exposure");
 if(frame.depth.size()!=n||frame.previous_depth.size()!=n||
    frame.motion.size()!=n||frame.object.size()!=n)
  throw std::invalid_argument("frame arrays size mismatch");

 Image out(w,h);
 const bool reuse=has_history_&&history_color_.width==w&&history_color_.height==h&&
                  !frame.camera_cut&&frame.sequence==history_sequence_+1;
 const double exposure_scale=reuse?frame.exposure/history_exposure_:1.0;

 for(int y=0;y<h;y++)for(int x=0;x<w;x++){
  const size_t i=size_t(y)*w+x;
  const V3 current=frame.hdr.color[i];
  V3 result=current;

  if(reuse&&frame.object[i]!=0){
   // motion = prev-unjittered minus cur-unjittered; the history texel was rendered
   // under history jitter, so the query adds (prev jitter - cur jitter), not the
   // reverse. The motion vector itself must not be treated as jittered velocity.
   const V2 query{double(x)+.5+frame.motion[i].x+(history_jitter_.x-frame.jitter.x),
                  double(y)+.5+frame.motion[i].y+(history_jitter_.y-frame.jitter.y)};
   const double pd=frame.previous_depth[i];
   bool ok=query.x>=0&&query.x<w&&query.y>=0&&query.y<h&&
           finite_v2(frame.motion[i])&&pd>0&&std::isfinite(pd);
   V3 hist{};
   if(ok){
    // Four texels of the unclamped bilinear footprint; each is validated on its own.
    const double gx=query.x-.5,gy=query.y-.5;
    const int x0=int(std::floor(gx)),y0=int(std::floor(gy));
    const int x1=x0+1,y1=y0+1;
    const double fx=gx-x0,fy=gy-y0;
    struct Tap {int tx,ty;double weight;};
    const Tap taps[4]={{x0,y0,(1-fx)*(1-fy)},{x1,y0,fx*(1-fy)},
                       {x0,y1,(1-fx)*fy},{x1,y1,fx*fy}};
    double wsum=0;V3 acc{};
    for(const Tap& t:taps){
     if(t.tx<0||t.ty<0||t.tx>=w||t.ty>=h)continue;
     const size_t ti=size_t(t.ty)*w+t.tx;
     if(history_object_[ti]==0||history_object_[ti]!=frame.object[i])continue;
     const double hd=history_depth_[ti];
     if(!(hd>0&&std::isfinite(hd)))continue;
     if(std::abs(hd-pd)>std::max(1e-4,settings.depth_relative_threshold*std::max(std::abs(hd),std::abs(pd))))
      continue;
     acc=acc+history_color_.at(t.tx,t.ty)*t.weight;
     wsum+=t.weight;
    }
    if(wsum>0){
     hist=acc/wsum;
     // History was stored pre-exposure at the previous exposure; renormalize first.
     hist=hist*exposure_scale;

     double ymin=1e300,comin=1e300,cgmin=1e300;
     double ymax=-1e300,comax=-1e300,cgmax=-1e300;
     for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
      const int nx=x+dx,ny=y+dy;
      if(nx<0||ny<0||nx>=w||ny>=h)continue;
      if(frame.object[size_t(ny)*w+nx]!=frame.object[i])continue;
      const YCoCg c=to_ycocg(frame.hdr.color[size_t(ny)*w+nx]);
      ymin=std::min(ymin,c.y);ymax=std::max(ymax,c.y);
      comin=std::min(comin,c.co);comax=std::max(comax,c.co);
      cgmin=std::min(cgmin,c.cg);cgmax=std::max(cgmax,c.cg);
     }
     YCoCg hc=to_ycocg(hist);
     hc.y=std::clamp(hc.y,ymin,ymax);
     hc.co=std::clamp(hc.co,comin,comax);
     hc.cg=std::clamp(hc.cg,cgmin,cgmax);
     hist=to_rgb(hc);
     result=current*(1-settings.history_weight)+hist*settings.history_weight;
    }
   }
  }

  out.color[i]=result;
 }

 // Publish an immutable snapshot only after the whole output has been built.
 history_color_=Image(w,h);
 history_color_.color=out.color;
 history_depth_=frame.depth;
 history_object_=frame.object;
 history_jitter_=frame.jitter;
 history_exposure_=frame.exposure;
 history_sequence_=frame.sequence;
 has_history_=true;
 return out;
}
}
