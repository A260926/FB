#include "object/ObjectList.h"
#include "object/Triangle.h"
#include "object/Sphere.h"
#include "Camera.h"
#include "ImageWriter.h"
#include <fstream>
#include <iostream>
#include <cmath>
#define REQUIRE(x) do{if(!(x)){std::cerr<<"baseline failure "<<__LINE__<<"\n";return 1;}}while(0)
int main(){
 Material mat;
 auto tri=std::make_shared<Triangle>(Point3(-2,-2,-3),Point3(2,-2,-3),Point3(0,2,-3),&mat);
 auto sph=std::make_shared<Sphere>(Point3(5,0,-6),1,&mat);
 ObjectList world;world.add(tri);world.add(sph);world.build();
 Ray ray(Point3(0,0,0),Vector3(0,0,-2));IntersectionRecord a,b;
 REQUIRE(world.intersect(ray,Interval(.001,100),a));REQUIRE(std::fabs(a.t-1.5)<1e-10);REQUIRE(a.mat_ptr==&mat);
 world.use_bvh=UseBVH::DISABLE;REQUIRE(world.intersect(ray,Interval(.001,100),b));REQUIRE(std::fabs(b.t-a.t)<1e-10);
 world.use_bvh=UseBVH::ENABLE;REQUIRE(!world.intersect(ray,Interval(.001,1),b));
 Camera camera;camera.image_width=4;camera.image_height=4;camera.lookfrom=Point3(0,0,0);camera.lookat=Point3(0,0,-3);camera.vfov=60;
 camera.render_mode=RenderMode::BINARY;camera.filename=".task-build/base.ppm";
 LightList lights;camera.render(world,lights);
 std::ifstream f(".task-build/base.ppm");std::string magic;int w,h,maxv;f>>magic>>w>>h>>maxv;
 REQUIRE(magic=="P3"&&w==4&&h==4&&maxv==255);int v,n=0,red=0;while(f>>v){if(n%3==0&&v>0)++red;++n;}REQUIRE(n==48&&red>0);
 std::cout<<"\nBASELINE_PATCH_SOURCE_OK hit_t=1.5 bvh=linear pixels=16 ppm_values=48\n";
}
