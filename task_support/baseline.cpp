#include <cstring>
#include <cstdint>
#include <cmath>
#include <iostream>
#include "sr/sr.hpp"
static unsigned calls=0;
static sr::VertexShaderOutput vs(const sr::Matrix<float,4>& m,const sr::Vertex& v) {
    sr::VertexShaderOutput o{}; o.position=m*v.position; o.color=v.color;
    o.texCoords[0]=v.texCoords[0]; o.texCoords[1]=v.texCoords[1]; o.normal=v.normal; return o;
}
static sr::Color fs(const sr::VertexShaderOutput& i,const std::array<const sr::Sampler*,2>& s,const std::array<const sr::Texture*,2>& t) {
    ++calls; auto c=t[0]->sample(s[0],i.texCoords[0]);
    return sr::Color(c.r*i.color.r,c.g*i.color.g,c.b*i.color.b,c.a*i.color.a);
}
int main() {
    sr::Texture color(sr::PixelFormat::rgba8,16,16),depth(sr::PixelFormat::float32,16,16),tex(sr::PixelFormat::rgba8,2,2);
    tex.setData({255,64,32,255,255,64,32,255,255,64,32,255,255,64,32,255});
    sr::clear(color,sr::Color(0.f,0.f,0.f,1.f)); sr::clear(depth,1.f);
    sr::Matrix<float,4> m; m.setIdentity(); sr::Sampler sampler;
    std::vector<sr::Vertex> vertices;
    for(auto p: {sr::Vector<float,4>{-.8f,-.8f,.3f,1.f},sr::Vector<float,4>{.8f,-.8f,.3f,1.f},sr::Vector<float,4>{-.8f,.8f,.3f,1.f}}) {
        sr::Vertex v{}; v.position=p; v.color=sr::Color(.5f,.25f,1.f,1.f); v.texCoords[0]=sr::Vector<float,2>{.5f,.5f}; vertices.push_back(v);
    }
    sr::DepthState ds; ds.read=true; ds.write=true;
    sr::drawTriangles(color,depth,vs,fs,{&sampler,nullptr},{&tex,nullptr},sr::Rect<float>{16.f,16.f},sr::Rect<float>{1.f,1.f},{},ds,{0,1,2},vertices,m);
    auto p=color.getPixel(4,4,0); float z; std::memcpy(&z,depth.getData().data()+(4*16+4)*sizeof(float),sizeof z);
    if(!calls || std::abs(p.r-127.f/255)>1.f/255 || std::abs(p.g-16.f/255)>1.f/255 || std::abs(p.b-32.f/255)>1.f/255 || std::abs(z-.3f)>1e-5) return 1;
    std::cout<<"BASELINE_RENDER_OK fragments="<<calls<<" rgb="<<p.r<<","<<p.g<<","<<p.b<<" depth="<<z<<"\n";
}
