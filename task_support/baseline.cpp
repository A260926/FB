#include "RenderHelp.h"
#include <cassert>
#include <cmath>
#include <iostream>
struct Inspect : RenderHelp { using RenderHelp::RenderHelp; unsigned pixel(int x,int y)const{return _frame_buffer->GetPixel(x,y);} float depth(int x,int y)const{return _depth_buffer[y][x];} };
int main(){
 Inspect r(16,16); int vs=0,ps=0; const Vec4f p[3]={{-.8f,-.8f,.5f,1},{.8f,-.8f,.5f,1},{0,.8f,.5f,1}};
 r.SetVertexShader([&](int i,ShaderContext& c){++vs;c.varying_vec4f[0]=Vec4f(2,.25f,.5f,1);return p[i];});
 r.SetPixelShader([&](ShaderContext& c){++ps;return c.varying_vec4f[0];});
 assert(r.DrawPrimitive()); assert(vs==3&&ps>0); const auto a=r.pixel(8,8);assert((a&0x00ffffff)==(vector_to_color(Vec4f(2,.25f,.5f,1))&0x00ffffff));assert(std::abs(r.depth(8,8)-1)<1e-6f);
 const int before=ps;
 r.SetVertexShader([&](int i,ShaderContext&){Vec4f q=p[i];q*=2.0f;return q;});
 assert(r.DrawPrimitive());assert(ps==before);assert(r.pixel(8,8)==a);
 r.Clear();assert(r.depth(8,8)==0);
 std::cout<<"BASELINE real_triangle vs=3 fragments="<<before<<" hdr_clamped=1 depth_occlusion=1 clear=1\n";
}
