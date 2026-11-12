#pragma once
#include "camera.hpp"
#include <vector>
#include <cstdint>
#include <string>
namespace render {
struct Image {
 int width=0,height=0;std::vector<V3> color;
 Image()=default;Image(int w,int h,V3 clear={});
 V3& at(int x,int y);V3 at(int x,int y)const;
 V3 bilinear(V2 pixel)const;
 void ppm(const std::string& path)const;
};
struct Frame {
 Image hdr;
 std::vector<double> depth,previous_depth;
 std::vector<V2> motion;
 std::vector<uint32_t> object;
 V2 jitter{};double exposure=1;
 uint64_t sequence=0;bool camera_cut=false;
 Frame(int w,int h);
};
struct Triangle {
 V3 a,b,c;
 V3 offset{},previous_offset{};
 V3 albedo{1,1,1};uint32_t object=1;
};
Frame draw(const std::vector<Triangle>& scene,const Camera& current,const Camera& previous,
 int width,int height,V2 jitter,double exposure,uint64_t sequence);
}
