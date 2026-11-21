#include <cstdint>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include "sr.hpp"
static unsigned shaded;
static sr::VertexShaderOutput vs(const sr::Matrix<float,4>& m,const sr::Vertex& v) {
    sr::VertexShaderOutput o{}; o.position=m*v.position; o.color=v.color;
    o.texCoords[0]=v.texCoords[0]; o.texCoords[1]=v.texCoords[1]; o.normal=v.normal; return o;
}
static sr::Color fs(const sr::VertexShaderOutput& v,const std::array<const sr::Sampler*,2>&,const std::array<const sr::Texture*,2>&) { ++shaded;return v.color; }
int main() {
 sr::Texture color(sr::PixelFormat::rgba8,8,8),depth(sr::PixelFormat::float32,8,8);
 auto* z=reinterpret_cast<float*>(depth.getData().data());std::fill(z,z+64,1.0f);
 std::vector<sr::Vertex> vertices{
  sr::Vertex{sr::Vector<float,4>{-.75f,-.75f,.25f,1.f},sr::Color{1.f,0.f,0.f,1.f},sr::Vector<float,2>{0.f,0.f},sr::Vector<float,3>{0.f,0.f,1.f}},
  sr::Vertex{sr::Vector<float,4>{.5f,-.75f,.25f,1.f},sr::Color{1.f,0.f,0.f,1.f},sr::Vector<float,2>{1.f,0.f},sr::Vector<float,3>{0.f,0.f,1.f}},
  sr::Vertex{sr::Vector<float,4>{-.75f,.5f,.25f,1.f},sr::Color{1.f,0.f,0.f,1.f},sr::Vector<float,2>{0.f,1.f},sr::Vector<float,3>{0.f,0.f,1.f}}};
 std::vector<std::size_t> ids{0,1,2};std::array<const sr::Sampler*,2> samplers{};std::array<const sr::Texture*,2> textures{};
 sr::DepthState ds;ds.read=true;ds.write=true;sr::BlendState bs;
 auto draw=[&]{sr::drawTriangles(color,depth,vs,fs,samplers,textures,{0.f,0.f,8.f,8.f},{0.f,0.f,1.f,1.f},bs,ds,ids,vertices,sr::Matrix<float,4>::identity());};
 draw();unsigned near=shaded;if(!near)return 1;
 for(auto& v:vertices){v.position.v[2]=.75f;v.color=sr::Color{0.f,0.f,1.f,1.f};}
 shaded=0;draw();
 const auto& bytes=color.getData();const std::size_t k=2*8+2;
 if(shaded||std::abs(z[k]-.25f)>1e-6f||bytes[k*4]!=255||bytes[k*4+2]!=0)return 2;
 std::printf("DRAW_BASELINE_OK near_shaded=%u far_shaded=%u depth=%.2f red=%u\n",near,shaded,z[k],unsigned(bytes[k*4]));
 return 0;
}
