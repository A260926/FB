#include "api.h"
#include "media/grid.h"
#include "samplers/random.h"
#include "accelerators/bvh.h"
#include "scene.h"
#include "interaction.h"
#include "memory.h"
#include <cmath>
#include <iostream>
using namespace pbrt;
int main(){
 Options options; options.nThreads=1; pbrtInit(options);
 Float density[8]={1,1,1,1,1,1,1,1};
 GridDensityMedium medium(Spectrum(0.25f),Spectrum(0.75f),0,2,2,2,Transform(),density);
 std::vector<std::shared_ptr<Primitive>> primitives;
 auto bvh=std::make_shared<BVHAccel>(primitives);
 Scene scene(bvh,{}); RandomSampler sampler(1,73); MemoryArena arena; sampler.StartPixel(Point2i(0,0));
 double sum=0; int events=0; const int count=8192;
 for(int i=0;i<count;i++){
  Ray ray(Point3f(.5f,.5f,.25f),Vector3f(0,0,1),.5f,.37f,&medium);
  Spectrum tr; SurfaceInteraction si;
  if(scene.IntersectTr(ray,sampler,&si,&tr)) return 2;
  sum+=tr[0];
  MediumInteraction mi;
  medium.Sample(ray,sampler,arena,&mi);
  if(mi.IsValid()){
   if(mi.GetMedium()!=&medium || mi.p.z<.25f || mi.p.z>.75f || mi.time!=.37f) return 3;
   ++events;
  }
  arena.Reset();
 }
 double mean=sum/count, expected=std::exp(-.5), eventRate=double(events)/count;
 if(std::abs(mean-expected)>.035 || std::abs(eventRate-(1-expected))>.035) return 4;
 std::cout<<"GRID_WORLD_BASELINE samples="<<count<<" tr="<<mean<<" expected="<<expected<<" events="<<events<<"\n";
 pbrtCleanup(); return 0;
}
