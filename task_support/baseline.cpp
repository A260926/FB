#include "ray_tracer.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
static void require(bool ok, const char* why) { if(!ok) throw std::runtime_error(why); }
int main() {
  Camera camera(5,80,80);
  Lights lights; lights.AddAmbient(1);
  Material mat; mat.color={200,100,40}; mat.specular=0; mat.reflective=0; mat.transparency=0;
  Sphere ball({0,0,12},2,mat);
  Ray ray({0,0,0},{0,0,12});
  auto h=ball.Intersects(ray);
  require(h.is_hit && std::abs(h.t-10)<1e-6,"production sphere nearest");
  std::vector<std::unique_ptr<Object>> objects;
  objects.push_back(std::make_unique<Sphere>(Vec3f{0,0,12},2,mat));
  auto color=lights.ColorAt(objects,*objects[0],h.where,camera,ball.NormalAt(h.where));
  require(color.x==200 && color.y==100 && color.z==40,"production ambient lighting");
  RayTracer tracer(camera,lights); tracer.AddObject<Sphere>(Vec3f{0,0,12},2,mat);
  tracer.SetBackground(3,5,7); tracer.Trace(1);
  const auto& image=tracer.image();
  require(image.width==8 && image.height==8,"production camera size");
  unsigned foreground=0, background=0;
  for(unsigned y=0;y<image.height;y++)for(unsigned x=0;x<image.width;x++){
    auto p=image.at(y,x);
    if(p.x==200 && p.y==100 && p.z==40) foreground++;
    if(p.x==3 && p.y==5 && p.z==7) background++;
  }
  require(foreground>0 && background>0,"production trace pixels");
  Ppm::Write(image,".task-build/baseline.ppm");
  Image loaded;Ppm::Read(loaded,".task-build/baseline.ppm");
  require(loaded.data.size()==64,"production ppm readback");
  std::cout<<"BASELINE nearest="<<h.t<<" ambient=200,100,40 image=8x8 foreground="<<foreground<<" background="<<background<<" PPM_OK\n";
}
