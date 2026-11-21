#include "ray_tracer.hpp"
#include <cmath>
#include <iostream>
int main() {
  Material mat; mat.color={80,120,160};mat.specular=0;mat.reflective=0;mat.transparency=0;
  Sphere sphere({0,0,0},1,mat);
  Ray axis({0,0,-4},{0,0,0});
  auto sh=sphere.Intersects(axis);
  Triangle tri({-1,-1,2},{1,-1,2},{0,1,2},mat);
  auto th=tri.Intersects(axis);
  if (!sh.is_hit || std::abs(sh.t-3)>1e-6 || !th.is_hit || std::abs(th.t-6)>1e-6 || std::abs(sphere.NormalAt(sh.where).z+1)>1e-6) return 2;
  Camera camera(8,54,54,{0,0,-4});
  if(camera.width()!=8 || camera.height()!=8)return 3;
  Lights lights;lights.AddAmbient(1);
  RayTracer tracer(camera,lights);tracer.SetBackground(2,3,4);tracer.AddObject<Sphere>(Vec3f{0,0,0},1.0f,mat);tracer.Trace(0);
  auto corners=camera.CornersWorld();int matched=0,foreground=0;
  for(unsigned row=0;row<8;++row)for(unsigned col=0;col<8;++col){
    auto target=corners[0]+(corners[1]-corners[0])*(float(col)/7)+(corners[2]-corners[0])*(float(row)/7);
    // Independent double quadratic for a unit sphere, rather than calling its intersector.
    double dx=target.x,dy=target.y,dz=target.z+4;
    double a=dx*dx+dy*dy+dz*dz,b=-8*dz,c=15;
    double disc=b*b-4*a*c;
    bool hit=disc>0 && (-b-std::sqrt(disc))/(2*a)>0;
    auto expected=hit?Vec3u8{80,120,160}:Vec3u8{2,3,4};auto pixel=tracer.image().at(row,col);
    if(pixel.x!=expected.x||pixel.y!=expected.y||pixel.z!=expected.z)return 4;
    ++matched;if(hit)++foreground;
  }
  if(foreground==0||foreground==64)return 5;
  std::cout<<"legacy_geometry=2 render=8x8 pixel_matches="<<matched<<" foreground="<<foreground<<"\n";
  return 0;
}
