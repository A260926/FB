#include "RenderHelp.h"
#include <cstdio>
#include <cmath>
struct Probe : RenderHelp {
 Probe():RenderHelp(32,32){}
 uint32_t pixel(int x,int y)const{return _frame_buffer->GetPixel(x,y);}
 float depth(int x,int y)const{return _depth_buffer[y][x];}
};
int main(){
 Probe r;r.SetBGColor(0xff000000);r.Clear();r.SetRenderState(false,true);
 Vec4f p[3]={{-.75f,-.75f,.5f,1},{.75f,-.75f,.5f,1},{0,.75f,.5f,1}};
 int vs=0,ps=0;Vec3f rgb={1,0,0};
 r.SetVertexShader([&](int i,ShaderContext& out){++vs;out.varying_vec3f[0]=rgb;return p[i];});
 r.SetPixelShader([&](ShaderContext& in){++ps;return in.varying_vec3f[0].xyz1();});
 if(!r.DrawPrimitive())return 1;
 auto color=r.pixel(16,16);int covered=ps;
 if(((color>>16)&255)<253||(color&65535)!=0||std::fabs(r.depth(16,16)-1)>1e-6||covered<200||covered>330)return 2;
 std::printf("HARD color=%08x depth=%.3f covered=%d\n",color,r.depth(16,16),covered);
 for(auto& v:p)v*=2.0f;rgb={0,1,0};ps=0;
 if(!r.DrawPrimitive()||r.pixel(16,16)!=color||ps!=0||vs!=6)return 3;
 std::printf("OCCLUSION behind_callbacks=%d vertex_callbacks=%d\n",ps,vs);
 std::puts("BASELINE_COMPLETE");return 0;
}
