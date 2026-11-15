#include "softgpu/renderer.hpp"
#include <cstdio>
#include <vector>
#include <cmath>
using namespace softgpu;
struct Vertex { Vec4 p; float u; };
struct VS { VsOut<1> operator()(const Vertex& v) const {VsOut<1> o;o.pos=v.p;o.attr[0]=v.u;return o;} };
struct FS { bool hole; bool operator()(const Fragment<1>& f,Vec4& c) const {
  if(hole && ((f.x+f.y)%5==0))return false;
  c=Vec4(f.attr[0],0.25f,0.8f,1);return true;
}};
static void draw(Renderer& r){
  r.clear(Vec4(0,0,0,1));DrawState st;st.cull=CullMode::None;
  const uint32_t idx[]={0,1,2};
  const Vertex near[]={{{-1.3f,-.7f,-.4f,1},0},{{.8f,-.7f,-.4f,1},1},{{0,1.5f,-.8f,2},.5f}};
  const Vertex far[]={{{-.9f,-.8f,.5f,1},0},{{.9f,-.8f,.5f,1},1},{{0,.9f,.5f,1},.5f}};
  r.draw<1>(near,3,idx,3,VS{},FS{true},st);
  r.draw<1>(far,3,idx,3,VS{},FS{false},st);r.finish();
}
int main(){
 Renderer a(48,40,Config{Mode::Immediate,1,16}), b(48,40,Config{Mode::Tiled,3,16});draw(a);draw(b);
 if(a.fb().color.rgba!=b.fb().color.rgba||a.fb().depth!=b.fb().depth)return 1;
 const auto& c=a.stats().raster;
 if(c.fragments_shaded==0||c.fragments_discarded==0||c.early_z_rejected==0||a.stats().triangles_clipped==0)return 2;
 for(float z:a.fb().depth)if(!std::isfinite(z)||z<0||z>1)return 3;
 std::printf("BASELINE clipped=%llu shaded=%llu rejected=%llu discarded=%llu modes_equal=1\n",
 (unsigned long long)a.stats().triangles_clipped,(unsigned long long)c.fragments_shaded,
 (unsigned long long)c.early_z_rejected,(unsigned long long)c.fragments_discarded);
 return 0;
}
