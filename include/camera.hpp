#pragma once
#include "math.hpp"
namespace render {
struct Projection {V2 pixel; double depth; bool valid;};
class Camera {
 V3 eye_,right_,up_,forward_;
 double scale_,near_;
public:
 Camera(V3 eye,V3 target,V3 up,double fov_y,double near_distance=0.1);
 Projection project(V3 p,int width,int height,V2 jitter={})const;
 V3 unproject(V2 pixel,double depth,int width,int height,V2 jitter={})const;
 V3 view(V3 p)const;
 V3 eye()const{return eye_;}
};
}
