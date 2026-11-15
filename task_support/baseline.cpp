#include <cmath>
#include <cstdint>
#include <limits>
#include <algorithm>
using std::isnan;
#include "src/rasterizer.cpp"
int main(){
  Mat4 id; for(int i=0;i<4;++i)id(i,i)=1;
  std::vector<uint32_t> color(W*H,0);std::vector<double> depth(W*H,std::numeric_limits<double>::infinity());
  Rasterize(id,id,id,Vec({-.8f,-.8f,.5f,1}),Vec({.8f,-.8f,.5f,1}),Vec({0,.8f,.5f,1}),Vec({0,0,1,0}),Vec({0,0,1,0}),Vec({0,0,1,0}),color.data(),depth.data());
  size_t hit=0; for(auto d:depth)if(std::isfinite(d)){if(std::abs(d-.5)>1e-6)return 2;++hit;}
  if(hit<10000||color[W*H-(H/2*W+W/2)-1]==0)return 3;
  auto front=color; Rasterize(id,id,id,Vec({-.8f,-.8f,.8f,1}),Vec({.8f,-.8f,.8f,1}),Vec({0,.8f,.8f,1}),Vec({0,0,1,0}),Vec({0,0,1,0}),Vec({0,0,1,0}),color.data(),depth.data());
  if(color!=front)return 4;
  std::cout<<"BASELINE_RENDER_OK covered="<<hit<<" depth_occlusion=preserved\n";
}
