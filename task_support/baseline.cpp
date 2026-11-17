#include "ray_tracer.hpp"
#include <cstdio>
#include <stdexcept>
static void require(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
int main(){
  Material white; white.color={100,100,100}; white.specular=0;
  Quad receiver({-2,-2,0},{2,-2,0},{2,2,0},{-2,2,0},white);
  Camera camera(24,20,20,{0,0,-4});
  Lights point; point.AddPoint(1,0,0,6);
  std::vector<std::unique_ptr<Object>> objects;
  auto clear=point.ColorAt(objects,receiver,{0,0,0},camera,{0,0,1});
  Material glass;glass.transparency=1;glass.refractive_index=1.5f;
  auto lens=std::make_unique<Sphere>(Vec3f{0,0,3},1.f,glass);
  auto hit=lens->Intersects(Ray({0,0,0},{0,0,6}));
  require(hit.is_hit && std::abs(hit.t-2)<1e-6f,"sphere production intersection");
  objects.push_back(std::move(lens));
  auto through_glass=point.ColorAt(objects,receiver,{0,0,0},camera,{0,0,1});
  objects[0]->material.transparency=0;
  auto through_opaque=point.ColorAt(objects,receiver,{0,0,0},camera,{0,0,1});
  require(clear.x==100,"clear point light");
  require(through_opaque.x>0 && through_opaque.x<clear.x,"existing shadow heuristic");
  require(through_glass==through_opaque,"old shadow ignores refractive material");
  std::printf("BASELINE_SHADOW clear=%u glass=%u opaque=%u t=%.6f\n",unsigned(clear.x),unsigned(through_glass.x),unsigned(through_opaque.x),hit.t);
  Lights scene_lights;scene_lights.AddAmbient(.2f);scene_lights.AddPoint(.8f,-3,3,-1);
  RayTracer tracer(camera,scene_lights);tracer.SetBackground(2,3,7);
  Material red;red.color={210,60,35};red.specular=0;
  tracer.AddObject<Sphere>(Vec3f{0,0,4},1.6f,red);
  tracer.AddObject<Sphere>(Vec3f{-.8f,.3f,1},.45f,glass);
  tracer.Trace(4);auto before=tracer.image().data;
  unsigned changed=0;for(const auto& px:before)if(!(px==Vec3u8{2,3,7}))++changed;
  require(changed>0,"actual Trace writes foreground");
  tracer.Trace(4);require(before==tracer.image().data,"repeat image equality");
  Ppm::Write(tracer.image(),".task-build/baseline.ppm");
  std::printf("BASELINE_RENDER size=%ux%u foreground=%u repeat=identical\n",tracer.image().width,tracer.image().height,changed);
  std::puts("BASELINE_OK production_sphere_lights_trace");
}
