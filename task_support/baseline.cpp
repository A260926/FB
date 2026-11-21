#include "object/ObjectList.h"
#include "object/Triangle.h"
#include "light/PointLight.h"
#include "light/LightList.h"
#include "Camera.h"
#include <fstream>
#include <iostream>
#include <cmath>
#include <cstdio>
#define REQUIRE(x) do {if(!(x)){std::cerr<<"baseline failure line "<<__LINE__<<"\n";return 1;}} while(0)
int main(){
  Material mat;mat.kd=1;mat.ks=0;mat.ka=0;mat.diffusecolor=Radiance(.2,.4,.6);
  auto tri=std::make_shared<Triangle>(Point3(-20,-20,-3),Point3(20,-20,-3),Point3(0,20,-3),&mat);
  ObjectList world;world.add(tri);world.build();
  Ray r(Point3(0,0,0),Vector3(0,0,-2));IntersectionRecord a,b;
  REQUIRE(world.intersect(r,Interval(.001,100),a));REQUIRE(std::abs(a.t-1.5)<1e-10);
  world.use_bvh=UseBVH::DISABLE;REQUIRE(world.intersect(r,Interval(.001,100),b));REQUIRE(a.t==b.t);
  world.use_bvh=UseBVH::ENABLE;REQUIRE(!world.intersect(r,Interval(.001,1),b));
  std::cout<<"BVH_BASELINE t="<<a.t<<"\n";
  auto light=std::make_shared<PointLight>(Point3(0,0,0),Radiance(9,9,9));
  auto c=light->phong_shading(a.p,a.normal,Vector3(0,0,1),a,world,Radiance(0,0,0));
  REQUIRE(std::abs(c.x()-.2)<1e-10&&std::abs(c.y()-.4)<1e-10&&std::abs(c.z()-.6)<1e-10);
  std::cout<<"PHONG_BASELINE rgb="<<c.x()<<","<<c.y()<<","<<c.z()<<"\n";
  LightList lights;lights.add(light);Camera cam;cam.image_width=1;cam.image_height=1;
  cam.lookfrom=Point3(0,0,0);cam.lookat=Point3(0,0,-3);cam.vfov=60;cam.ambient_light=Radiance(0,0,0);
  cam.render_mode=RenderMode::PHONG;cam.tone_mapper=ToneMapper::LINEAR;cam.exposure=0;cam.nbounces=2;
  cam.filename=".task-build/phong.ppm";std::remove(cam.filename.c_str());cam.render(world,lights);
  std::ifstream file(cam.filename);std::string magic;int w,h,maxv;file>>magic>>w>>h>>maxv;
  REQUIRE(magic=="P3"&&w==1&&h==1&&maxv==255);int v,n=0,sum=0;while(file>>v){REQUIRE(v>=0&&v<=255);sum+=v;++n;}
  REQUIRE(n==3&&sum>0);std::cout<<"CAMERA_BASELINE pixels=1 values="<<n<<"\nBASELINE_COMPLETE\n";
}
