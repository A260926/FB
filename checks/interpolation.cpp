#include "Renderer.h"
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool b, const char* s) { if (!b) throw std::runtime_error(s); }
struct Value { float x,y,z,w,q; };
const Value vertices[]={{-.75f,.75f,0,1,0}, {1.5f,1.5f,0,2,1}, {-3,-3,0,4,.5f}};
struct VS : swr::VertexShaderBase<VS> {
    static const int AttribCount=1, PVarCount=1;
    static void processVertex(swr::VertexShaderInput in, swr::VertexShaderOutput* o) {
        const auto& v=*static_cast<const Value*>(in[0]);
        o->x=v.x; o->y=v.y; o->z=v.z; o->w=v.w; o->pvar[0]=v.q;
    }
};
struct PS : swr::PixelShaderBase<PS> {
    static const int InterpolateZ=true, InterpolateW=true, PVarCount=1;
    static std::array<float,1024> q;
    static int calls;
    static void drawPixel(const swr::PixelData& p) {
        require(p.x>=0&&p.x<32&&p.y>=0&&p.y<32,"viewport bounds");
        require(std::isfinite(p.pvar[0])&&std::isfinite(p.w),"finite varying");
        require(std::abs(p.z-.5f)<1e-5f,"depth mapping");
        q[p.y*32+p.x]=p.pvar[0]; ++calls;
    }
};
std::array<float,1024> PS::q;
int PS::calls;
}
int main() {
 try {
    for (auto mode : {swr::RasterMode::Span,swr::RasterMode::Block,swr::RasterMode::Adaptive}) {
        PS::q.fill(-1); PS::calls=0;
        swr::Rasterizer r; r.setRasterMode(mode); r.setScissorRect(0,0,32,32); r.setPixelShader<PS>();
        swr::VertexProcessor v(&r); v.setViewport(4,4,24,24); v.setCullMode(swr::CullMode::None); v.setVertexShader<VS>();
        v.setVertexAttribPointer(0,sizeof(Value),vertices); int indices[]={0,1,2};
        v.drawElements(swr::DrawMode::Triangle,3,indices);
        // Viewport produces (7,7),(25,7),(7,25). Independent barycentrics at (10.5,10.5).
        double b=3.5/18, a=1-2*b;
        double expected=(b*.5+b*.5*.25)/(a+b*.5+b*.25);
        require(PS::calls>100,"triangle not rendered");
        require(std::abs(PS::q[10*32+10]-expected)<2e-6,"perspective reference mismatch");
        std::cout<<"BASE mode="<<int(mode)<<" calls="<<PS::calls<<" q="<<PS::q[330]<<" expected="<<expected<<'\n';
    }
    std::cout<<"BASELINE_RENDER_OK modes=3\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
