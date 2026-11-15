#include <FastBVH.h>
#include <iostream>
#include <cmath>
#include <vector>
using V=FastBVH::Vector3<double>;
struct Sphere{V center; double radius; unsigned id;};
struct Box{FastBVH::BBox<double> operator()(const Sphere& s)const{return {s.center-V{s.radius,s.radius,s.radius},s.center+V{s.radius,s.radius,s.radius}};}};
struct HitSphere{
  double limit; unsigned* calls;
  FastBVH::Intersection<double,Sphere> operator()(const Sphere& s,const FastBVH::Ray<double>& ray)const{
    ++*calls;FastBVH::Intersection<double,Sphere> out;
    V oc=ray.o-s.center;double a=ray.d.x*ray.d.x+ray.d.y*ray.d.y+ray.d.z*ray.d.z;
    double h=oc.x*ray.d.x+oc.y*ray.d.y+oc.z*ray.d.z;
    double c=oc.x*oc.x+oc.y*oc.y+oc.z*oc.z-s.radius*s.radius,disc=h*h-a*c;
    if(disc<0)return out;double t=(-h-std::sqrt(disc))/a;
    if(t<0)t=(-h+std::sqrt(disc))/a;
    if(t>=0&&t<=limit){out.t=t;out.object=&s;out.normal=(ray.o+ray.d*t-s.center)*(1/s.radius);}return out;
  }
};
int main(){
  std::vector<Sphere> spheres;for(unsigned i=0;i<64;++i)spheres.push_back({V{double(i)*3,0,5},1,i});
  FastBVH::DefaultBuilder<double> build;auto bvh=build(spheres,Box{});if(bvh.countLeafs()<2)return 1;
  unsigned calls=0;
  FastBVH::Traverser<double,Sphere,HitSphere> trace(bvh,HitSphere{100,&calls});
  FastBVH::Traverser<double,Sphere,HitSphere,FastBVH::OnlyTestOcclusion> shadow(bvh,HitSphere{1.5,&calls});
  for(unsigned i=0;i<64;++i){FastBVH::Ray<double> ray(V{double(i)*3,0,0},V{0,0,2});auto h=trace.traverse(ray);if(!h||h.object->id!=i||std::abs(h.t-2)>1e-10||h.normal.z!=-1)return 2;if(shadow.traverse(ray))return 3;}
  std::cout<<"BASELINE_OK primary=64 finite_shadow=64 original_ids=preserved leaves="<<bvh.countLeafs()<<" primitive_calls="<<calls<<"\n";
}
