// Fixture, MIT; exercises the unmodified upstream clipping and raster path.
#include "Renderer.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace swr;
struct V { float x,y,z,w; };
struct VS : VertexShaderBase<VS> {
 static const int AttribCount=1;
 static void processVertex(VertexShaderInput in,VertexShaderOutput* o){ auto v=(const V*)in[0];o->x=v->x;o->y=v->y;o->z=v->z;o->w=v->w; }
};
struct PS:PixelShaderBase<PS>{
 static const bool InterpolateZ=true;
 static float red,green,sr,sg;static int calls;
 static void drawPixel(const PixelData&p){if(p.x==16&&p.y==16){red=.5f*sr+.5f*red;green=.5f*sg+.5f*green;++calls;assert(std::isfinite(p.z));}}
};
float PS::red,PS::green,PS::sr,PS::sg;int PS::calls;
int main(){Rasterizer r;VertexProcessor v(&r);r.setScissorRect(0,0,32,32);r.setPixelShader<PS>();v.setViewport(0,0,32,32);v.setCullMode(CullMode::None);v.setVertexShader<VS>();int ids[]={0,1,2};V a[]={{-1.2f,-.8f,-.2f,1},{1.2f,-.8f,-.2f,1},{0,.9f,-.2f,1}};V b[]={{-1.2f,-.8f,.3f,1},{1.2f,-.8f,.3f,1},{0,.9f,.3f,1}};
 for(int order=0;order<2;++order){PS::red=PS::green=0;PS::calls=0;for(int j=0;j<2;++j){bool isRed=(j==order);PS::sr=isRed;PS::sg=!isRed;v.setVertexAttribPointer(0,sizeof(V),isRed?a:b);v.drawElements(DrawMode::Triangle,3,ids);}assert(PS::calls==2);printf("ORDER_%d rgb=%.2f,%.2f calls=%d\n",order,PS::red,PS::green,PS::calls);assert(std::abs(PS::red-(order?.5f:.25f))<1e-5f);}puts("SWR_PIPELINE_BASELINE_OK");}
