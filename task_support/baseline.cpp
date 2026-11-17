#include "fj_mesh.h"
#include "fj_bvh_accelerator.h"
#include "fj_grid_accelerator.h"
#include "fj_object_instance.h"
#include "fj_object_group.h"
#include "fj_intersection.h"
#include "fj_point_light.h"
#include "fj_shader.h"
#include <cmath>
#include <cstdio>
using namespace fj;
class Opacity : public Shader {
  void evaluate(const TraceContext&, const SurfaceInput&, SurfaceOutput* out) const override {
    out->Cs=Color(1,1,1); out->Os=0.5f;
  }
};
static int run(Accelerator& accelerator, const char* backend) {
  Mesh mesh;mesh.SetPointCount(6);mesh.SetFaceCount(2);
  mesh.AddPointPosition();mesh.AddFaceIndices();
  for(int layer=0;layer<2;++layer) {
    int b=layer*3;double z=layer+1;
    mesh.SetPointPosition(b,Vector(-4,-4,z));
    mesh.SetPointPosition(b+1,Vector(4,-4,z));
    mesh.SetPointPosition(b+2,Vector(0,4,z));
    mesh.SetFaceIndices(layer,Index3(b,b+1,b+2));
  }
  mesh.ComputeNormals();mesh.ComputeBounds();
  accelerator.SetPrimitiveSet(&mesh);
  if(accelerator.Build()!=0)return 1;
  Opacity opacity;ObjectInstance object;object.SetSurface(&accelerator);
  object.SetShader(&opacity,0);object.ComputeBounds();
  ObjectGroup group;group.AddObject(&object);group.ComputeBounds();
  auto* top=const_cast<Accelerator*>(group.GetSurfaceAccelerator());
  if(top->Build()!=0)return 2;
  TraceContext shadow=SlCameraContext(&group);shadow.ray_context=CXT_SHADOW_RAY;
  Vector origin(0,0,3),direction(0,0,-1);Color4 color;double hit=0;
  int found=SlTrace(&shadow,&origin,&direction,0.0001,3,&color,&hit);
  if(!found||std::abs(color.a-.5)>1e-6||std::abs(hit-1)>1e-6)return 3;
  ObjectInstance receiver;receiver.SetShadowTarget(&group);
  SurfaceInput in;in.shaded_object=&receiver;in.P=origin;in.N=direction;
  PointLight light;light.SetColor(1,1,1);light.SetIntensity(1);light.Preprocess();
  LightSample sample;light.GetSamples(&sample,1);
  TraceContext camera=SlCameraContext(&group);LightOutput output;
  int lit=SlIlluminance(&camera,&sample,&origin,&direction,1.5707963267948966,&in,&output);
  if(!lit||std::abs(output.Cl.r-.5)>1e-6)return 4;
  std::printf("SHADOW_BASELINE backend=%s alpha=%.6f light=%.6f surfaces=2\n",backend,color.a,output.Cl.r);
  return 0;
}
int main(){BVHAccelerator bvh;GridAccelerator grid;int a=run(bvh,"bvh");if(a)return a;return run(grid,"grid");}
