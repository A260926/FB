#include "camera.hpp"
#include <stdexcept>
namespace render {
Camera::Camera(V3 eye,V3 target,V3 up,double fov_y,double near_distance):eye_(eye),near_(near_distance){
 if(!finite(eye)||!finite(target)||!finite(up)||!(fov_y>0&&fov_y<3.13)||!(near_>0))
  throw std::invalid_argument("invalid camera");
 forward_=normalized(target-eye);
 right_=normalized(cross(forward_,up));
 up_=cross(right_,forward_);
 if(length(forward_)<.5||length(right_)<.5)throw std::invalid_argument("degenerate basis");
 scale_=1/std::tan(fov_y*.5);
}
V3 Camera::view(V3 p)const{
 V3 d=p-eye_;
 return {dot(d,right_),dot(d,up_),dot(d,forward_)};
}
Projection Camera::project(V3 p,int width,int height,V2 jitter)const{
 if(width<=0||height<=0)throw std::invalid_argument("invalid viewport");
 V3 q=view(p);
 if(!finite(q)||q.z<near_)return {{},q.z,false};
 double aspect=double(width)/height;
 double ndc_x=q.x*scale_/(q.z*aspect);
 double ndc_y=q.y*scale_/q.z;
 return {{(ndc_x+1)*width*.5+jitter.x,(1-ndc_y)*height*.5+jitter.y},q.z,true};
}
V3 Camera::unproject(V2 pixel,double depth,int width,int height,V2 jitter)const{
 if(width<=0||height<=0||!std::isfinite(depth)||depth<near_)
  throw std::invalid_argument("invalid unprojection");
 double ndc_x=(pixel.x-jitter.x)*2/width-1;
 double ndc_y=1-(pixel.y-jitter.y)*2/height;
 double x=ndc_x*depth*(double(width)/height)/scale_;
 double y=ndc_y*depth/scale_;
 return eye_+right_*x+up_*y+forward_*depth;
}
}
