#include "sr/sr.hpp"
#include <cstdio>
#include <stdexcept>
static size_t calls=0;
static sr::VertexShaderOutput vs(const sr::Matrix<float,4>&,const sr::Vertex& v){return {v.position,v.color,{v.texCoords[0],v.texCoords[1]},v.normal};}
static sr::Color fs(const sr::VertexShaderOutput&,const std::array<const sr::Sampler*,2>& a,const std::array<const sr::Texture*,2>& t){++calls;return t[0]->sample(a[0],sr::Vector<float,2>{0.5F,0.5F});}
int main(){
 sr::Texture input(sr::PixelFormat::rgba8,4,2,true),plain(sr::PixelFormat::rgba8,4,2,false);
 std::vector<std::uint8_t> base(4*2*4);for(size_t i=0;i<base.size();i+=4){base[i]=64;base[i+1]=128;base[i+2]=192;base[i+3]=255;}input.setData(base);
 sr::Sampler sampler;auto c=input.sample(&sampler,sr::Vector<float,2>{0.5F,0.5F});
 if(std::fabs(c.r-64.F/255.F)>1e-5F)return 2;
 std::printf("BASE_SAMPLE %.6f %.6f %.6f\nPLAIN_LEVELS %zu\n",c.r,c.g,c.b,plain.getLevelCount());
 try{input.setData(std::vector<std::uint8_t>(8,255),1);std::puts("MIP_UPLOAD accepted");}catch(const std::exception& e){std::printf("MIP_UPLOAD rejected: %s\n",e.what());}
 sr::Texture color(sr::PixelFormat::rgba8,8,8,false),depth(sr::PixelFormat::float32,8,8,false);sr::clear(color,sr::Color{0.F,0.F,0.F,1.F});sr::clear(depth,1000.F);
 std::vector<sr::Vertex> vertices(3);vertices[0].position=sr::Vector<float,4>{-.8F,-.8F,.2F,1.F};vertices[1].position=sr::Vector<float,4>{.8F,-.8F,.2F,1.F};vertices[2].position=sr::Vector<float,4>{0.F,.8F,.2F,1.F};
 for(auto& v:vertices){v.color={1.F,1.F,1.F,1.F};v.texCoords[0]=sr::Vector<float,2>{.5F,.5F};v.texCoords[1]=sr::Vector<float,2>{0.F,0.F};v.normal=sr::Vector<float,3>{0.F,0.F,1.F};}
 sr::DepthState ds;ds.read=true;ds.write=true;sr::drawTriangles(color,depth,vs,fs,{&sampler,nullptr},{&input,nullptr},sr::Rect<float>{0.F,0.F,8.F,8.F},sr::Rect<float>{0.F,0.F,1.F,1.F},sr::BlendState{},ds,{0,1,2},vertices,sr::Matrix<float,4>{});
 size_t colored=0;for(size_t i=0;i<color.getData().size();i+=4)if(color.getData()[i])++colored;
 if(!calls||!colored)return 3;std::printf("DRAW callbacks=%zu colored=%zu\n",calls,colored);std::puts("TEXTURED_DRAW_BASELINE_OK");return 0;
}
