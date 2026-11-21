#include "fj_mesh.h"
#include "fj_bvh_accelerator.h"
#include "fj_object_instance.h"
#include "fj_object_group.h"
#include "fj_accelerator.h"
#include "fj_shader.h"
#include "fj_renderer.h"
#include "fj_framebuffer.h"
#include "fj_camera.h"
#include <atomic>
#include <cmath>
#include <cstdio>
using namespace fj;
class Flat : public Shader {
public:
  mutable std::atomic<int> calls;
  Flat():calls(0){}
private:
  void evaluate(const TraceContext &c,const SurfaceInput&,SurfaceOutput *out) const override {
    if(c.ray_context==CXT_CAMERA_RAY) ++calls;
    out->Cs=Color(.25,.5,.75);out->Os=1;
  }
};
int main(){
  Mesh m;m.SetPointCount(4);m.SetFaceCount(2);m.AddPointPosition();m.AddFaceIndices();
  m.SetPointPosition(0,Vector(-10,-10,-3));m.SetPointPosition(1,Vector(10,-10,-3));
  m.SetPointPosition(2,Vector(10,10,-3));m.SetPointPosition(3,Vector(-10,10,-3));
  m.SetFaceIndices(0,Index3(0,1,2));m.SetFaceIndices(1,Index3(0,2,3));m.ComputeNormals();m.ComputeBounds();
  BVHAccelerator meshAcc;meshAcc.SetPrimitiveSet(&m);if(meshAcc.Build()!=0)return 1;
  Flat shader;ObjectInstance object;object.SetSurface(&meshAcc);object.SetShader(&shader,0);object.ComputeBounds();
  ObjectGroup group;group.AddObject(&object);group.ComputeBounds();
  if(const_cast<Accelerator*>(group.GetSurfaceAccelerator())->Build()!=0)return 2;
  TraceContext c=SlCameraContext(&group);Vector o(0,0,0),d(0,0,-1);Color4 color;double t=0;
  int hit=SlTrace(&c,&o,&d,.01,100,&color,&t);
  if(hit!=1 || std::abs(t-3)>1e-9 || std::abs(color.g-.5)>1e-6)return 3;
  std::printf("SURFACE_BASELINE hit=%d distance=%.0f rgb=%.2f,%.2f,%.2f\n",hit,t,color.r,color.g,color.b);
  Camera cam;FrameBuffer fb;Renderer ren;ren.SetCamera(&cam);ren.SetFrameBuffers(&fb);ren.SetTargetObjects(&group);
  ren.SetResolution(8,8);ren.SetTileSize(8,8);ren.SetSampleJitter(0);ren.SetSampleTimeRange(0,0);ren.SetFilterWidth(1,1);
  ren.SetFrameReportCallback(NULL,NULL,NULL,NULL);ren.SetTileReportCallback(NULL,NULL,NULL,NULL);
  ren.SetPixelSamples(2,2);ren.SetMaxSubdivision(3);
  for(int mode=0;mode<2;++mode){
    shader.calls=0;ren.SetSamplerType(mode);
    if(ren.RenderScene()!=0)return 4;
    Color4 p=fb.GetColor(4,4);
    if(std::abs(p.r-.25)>1e-6||std::abs(p.g-.5)>1e-6||std::abs(p.b-.75)>1e-6||std::abs(p.a-1)>1e-6||shader.calls<=0)return 5;
    std::printf("RENDER_BASELINE mode=%s rays=%d center=%.2f,%.2f,%.2f,%.2f\n",mode?"adaptive":"fixed",shader.calls.load(),p.r,p.g,p.b,p.a);
  }
  std::puts("BASELINE_COMPLETE");return 0;
}
