#include "fj_volume.h"
#include "fj_mesh.h"
#include "fj_bvh_accelerator.h"
#include "fj_object_instance.h"
#include "fj_object_group.h"
#include "fj_volume_accelerator.h"
#include "fj_accelerator.h"
#include "fj_shader.h"
#include <cmath>
#include <cstdio>
using namespace fj;
class ConstantVolume : public Shader {
  void evaluate(const TraceContext&,const SurfaceInput&,SurfaceOutput* out) const override {
    out->Cs=Color(.2f,.4f,.8f);out->Os=1;
  }
};
int main() {
  Volume volume;volume.Resize(8,8,8);volume.SetBounds(Box(Vector(0,0,0),Vector(1,1,1)));
  for(int z=0;z<8;++z)for(int y=0;y<8;++y)for(int x=0;x<8;++x)volume.SetValue(x,y,z,.25f);
  VolumeSample s;
  if(!volume.GetSample(Vector(.5,.5,.5),&s)||std::abs(s.density-.25f)>1e-6) return 1;
  std::printf("SAMPLE_BASELINE center=%.6f\n",s.density);
  ConstantVolume shader;ObjectInstance object;
  if(object.SetVolume(&volume)!=0)return 2;
  object.SetShader(&shader,0);object.ComputeBounds();
  Mesh mesh;mesh.SetPointCount(3);mesh.SetFaceCount(1);mesh.AddPointPosition();mesh.AddFaceIndices();
  mesh.SetPointPosition(0,Vector(-2,-2,3));mesh.SetPointPosition(1,Vector(2,-2,3));mesh.SetPointPosition(2,Vector(0,2,3));
  mesh.SetFaceIndices(0,Index3(0,1,2));mesh.ComputeNormals();mesh.ComputeBounds();
  BVHAccelerator surface;surface.SetPrimitiveSet(&mesh);if(surface.Build()!=0)return 9;
  ObjectInstance farSurface;farSurface.SetSurface(&surface);farSurface.SetShader(&shader,0);farSurface.ComputeBounds();
  ObjectGroup group;group.AddObject(&object);group.AddObject(&farSurface);group.ComputeBounds();
  if(const_cast<Accelerator*>(group.GetSurfaceAccelerator())->Build()!=0)return 3;
  if(VolumeAccBuild(const_cast<VolumeAccelerator*>(group.GetVolumeAccelerator()))!=0)return 4;
  TraceContext c=SlCameraContext(&group);c.raymarch_step=.1;c.raymarch_shadow_step=.1;c.opacity_threshold=1;
  Vector o(.5,.5,-1),d(0,0,1);Color4 color;double hit=0;
  if(!SlTrace(&c,&o,&d,1.2,1.8,&color,&hit))return 5;
  if(!(color.a>0&&color.a<1))return 6;
  if(std::abs(color.r-.2*color.a)>1e-6||std::abs(color.g-.4*color.a)>1e-6||std::abs(color.b-.8*color.a)>1e-6)return 7;
  std::printf("VOLUME_BASELINE alpha=%.6f rgb=%.6f,%.6f,%.6f\n",color.a,color.r,color.g,color.b);
  TraceContext shadow=c;shadow.ray_context=CXT_SHADOW_RAY;Color4 sh;
  if(!SlTrace(&shadow,&o,&d,1.2,1.8,&sh,&hit)||std::abs(sh.a-color.a)>1e-6)return 8;
  std::printf("SHADOW_BASELINE alpha=%.6f\nBASELINE_COMPLETE\n",sh.a);
}
