#include <cmath>
#include <cstdlib>
#include <iostream>
#include "Rasterizer.h"
using namespace swr;
struct Shader:PixelShaderBase<Shader>{
 static const int PVarCount=1;
 static const bool InterpolateZ=true;
 static const bool InterpolateW=true;
 static unsigned count; static bool bad,sample;
 static void drawPixel(const PixelData&p){
  ++count;
  if(p.x<0||p.x>=12||p.y<0||p.y>=12||std::fabs(p.z-.25f)>1e-5f) bad=true;
  if(p.x==3&&p.y==3){sample=true;if(std::fabs(p.pvar[0]-.3125f)>1e-5f)bad=true;}
 }
};
unsigned Shader::count=0;bool Shader::bad=false;bool Shader::sample=false;
int main(){
 Rasterizer r;r.setScissorRect(0,0,12,12);r.setPixelShader<Shader>();
 RasterizerVertex v[3]={};v[0].x=1;v[0].y=1;v[1].x=9;v[1].y=1;v[2].x=1;v[2].y=9;
 for(auto& a:v){a.w=1;a.z=.25f;}v[1].pvar[0]=1;
 const RasterMode modes[]={RasterMode::Span,RasterMode::Block,RasterMode::Adaptive};
 const char*names[]={"span","block","adaptive"};
 for(int i=0;i<3;i++){
  Shader::count=0;Shader::bad=false;Shader::sample=false;r.setRasterMode(modes[i]);
  r.drawTriangle(v[0],v[1],v[2]);
  if(!Shader::count||Shader::bad||!Shader::sample)return 2;
  std::cout<<names[i]<<" fragments="<<Shader::count<<" depth=0.25 uv=0.3125\n";
 }
}
