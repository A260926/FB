#include "RenderHelp.h"
#include <cstdio>
#include <cmath>
struct Probe:RenderHelp{using RenderHelp::RenderHelp;uint32_t pixel(int x,int y){return _frame_buffer->GetPixel(x,y);}float depth(int x,int y){return _depth_buffer[y][x];}};
int main(){
 Probe r(32,32); int vs=0,ps=0;float w=1;Vec4f color={1,0,0,.5f};
 r.SetVertexShader([&](int i,ShaderContext&o){++vs;const Vec2f p[3]={{-.8f,-.8f},{.8f,-.8f},{0,.8f}};o.varying_vec4f[0]=color;return Vec4f{p[i].x*w,p[i].y*w,.5f*w,w};});
 r.SetPixelShader([&](ShaderContext&i){++ps;return i.varying_vec4f[0];});
 if(!r.DrawPrimitive())return 1;
 int covered=ps;if(covered<100||vs!=3||r.pixel(16,16)!=vector_to_color(color))return 2;
 color={0,0,1,.25f};if(!r.DrawPrimitive()||vs!=6||ps!=2*covered||r.pixel(16,16)!=vector_to_color(color))return 3;
 w=2;color={0,1,0,1};if(!r.DrawPrimitive()||vs!=9||ps!=2*covered||fabs(r.depth(16,16)-1)>1e-6)return 4;
 r.SaveFile(".task-build/ordinary.bmp");Bitmap saved(".task-build/ordinary.bmp");if(saved.GetW()!=32||saved.GetH()!=32||(saved.GetPixel(16,16)&0xffffff)!=(r.pixel(16,16)&0xffffff))return 5;
 printf("BASELINE DrawPrimitive covered=%d VS=%d PS=%d depth=%.1f blue_last=1 BMP=32x32\n",covered,vs,ps,r.depth(16,16));return 0;
}
