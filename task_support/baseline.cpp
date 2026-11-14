#include <FastBVH.h>
#include <cstdio>
#include <cmath>
#include <vector>
using namespace FastBVH;
struct Sphere { Vector3<double> c; double r; unsigned id; };
struct Bounds { BBox<double> operator()(const Sphere& s) const { Vector3<double> d{s.r,s.r,s.r};return {s.c-d,s.c+d}; } };
struct Hit {
 unsigned* calls;
 Intersection<double,Sphere> operator()(const Sphere& s,const Ray<double>& ray) const {
  ++*calls;
  auto v=ray.o-s.c;double a=dot(ray.d,ray.d),b=dot(v,ray.d),c=dot(v,v)-s.r*s.r;
  double d=b*b-a*c;if(d<0)return {};
  double t=(-b-std::sqrt(d))/a;if(t<0)t=(-b+std::sqrt(d))/a;if(t<0)return {};
  return {t,&s,normalize(ray.o+ray.d*t-s.c)};
 }
};
int main(){
 std::vector<Sphere> p;for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)p.push_back({{double(3*x),double(3*y),0},0.5,y*8+x});
 DefaultBuilder<double> build;auto tree=build(p,Bounds{});unsigned calls=0,hits=0,misses=0;
 Traverser<double,Sphere,Hit> closest(tree,Hit{&calls});
 Traverser<double,Sphere,Hit,OnlyTestOcclusion> shadow(tree,Hit{&calls});
 for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
  Ray<double> ray({double(3*x)-0.03,double(3*y)-0.02,-3},{0.01,0.006666666666666667,1});
  auto h=closest.traverse(ray);if(!h||h.object->id!=y*8+x||!(h.t>2.49&&h.t<2.51)||!shadow.traverse(ray))return 2;++hits;
  auto m=closest.traverse(Ray<double>({double(3*x)+1.0,double(3*y)+1.0,-3},{0.001,0.002,1}));if(m)return 3;++misses;
 }
 std::printf("BASELINE hits=%u misses=%u nodes=%zu node_bytes=%zu primitive_calls=%u\n",hits,misses,tree.getNodes().size(),tree.getNodes().size()*sizeof(Node<double>),calls);
 return hits==64&&misses==64?0:4;
}
