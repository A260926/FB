#include "Renderer.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace swr;
static int calls=0;
struct PS:PixelShaderBase<PS>{
 static const int PVarCount=1;static const bool InterpolateW=true;
 static void drawPixel(const PixelData&p){assert(std::isfinite(p.pvar[0])); ++calls;}
};
struct VS:VertexShaderBase<VS>{
 static const int AttribCount=1,PVarCount=1;
 static void processVertex(VertexShaderInput i,VertexShaderOutput*o){const float*p=(const float*)i[0];o->x=p[0];o->y=p[1];o->z=p[2];o->w=p[3];o->pvar[0]=p[4];}
};
struct Spy:IRasterizer{
 mutable int tris=0;Rasterizer*r;
 void drawPointList(const RasterizerVertex*v,const int*i,size_t n)const override{r->drawPointList(v,i,n);}
 void drawLineList(const RasterizerVertex*v,const int*i,size_t n)const override{r->drawLineList(v,i,n);}
 void drawTriangleList(const RasterizerVertex*v,const int*i,size_t n)const override{for(size_t k=0;k<n;k+=3)if(i[k]>=0)++tris;r->drawTriangleList(v,i,n);}
};
int main(){
 Rasterizer r;r.setScissorRect(0,0,16,16);r.setPixelShader<PS>();r.setRasterMode(RasterMode::Span);
 Spy s;s.r=&r;VertexProcessor vp(&s);vp.setViewport(0,0,16,16);vp.setCullMode(CullMode::None);vp.setVertexShader<VS>();
 float a[3][5]={{-2,0,0,1,1},{.8f,-.8f,0,1,2},{.8f,.8f,0,1,3}};int ids[]={0,1,2};vp.setVertexAttribPointer(0,sizeof(a[0]),a);vp.drawElements(DrawMode::Triangle,3,ids);
 assert(s.tris==2 && calls>0);std::printf("CLIPPED fans=%d scalar_callbacks=%d\n",s.tris,calls);std::puts("BASELINE_OK");
}
