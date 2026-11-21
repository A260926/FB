#include "tiny_bvh.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <set>
using namespace tinybvh;
static bvhvec3 reference_cross(const bvhvec3&a,const bvhvec3&b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static float reference_dot(const bvhvec3&a,const bvhvec3&b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static bool near(float a,float b){return std::fabs(a-b)<1e-5f;}
int main(){
 bvhvec4 vertices[]={{-1,-1,2,0},{1,-1,2,0},{0,1,2,0},{-1,-1,4,0},{1,-1,4,0},{0,1,4,0}};
 BVH plain;plain.settings.useSIMDifavailable=false;plain.settings.enableThreading=false;plain.settings.binnedBVH=true;plain.Build(vertices,2);
 Ray ray({0,0,0},{0,0,2});plain.Intersect(ray);
 if(!near(ray.hit.t,1)||ray.hit.prim!=0||!near(ray.hit.u,.25f)||!near(ray.hit.v,.5f))return 2;
 Ray cap({0,0,0},{0,0,2},1);if(plain.IsOccluded(cap))return 3;
 Ray shadow({0,0,0},{0,0,2},1.1f);if(!plain.IsOccluded(shadow))return 4;
 for(auto&v:vertices)v.x+=3;plain.Refit();
 Ray shifted({3,0,0},{0,0,2});plain.Intersect(shifted);if(!near(shifted.hit.t,1))return 5;
 Ray old({0,0,0},{0,0,2});plain.Intersect(old);if(old.hit.t<BVH_FAR)return 6;
 std::puts("BASELINE_PLAIN_REFIT_OK t=1 barycentric=0.25,0.5 finite_occlusion=1");
 std::vector<bvhvec4> soup;uint32_t rng=17;
 auto random=[&](){rng=1664525u*rng+1013904223u;return float(rng>>8)/16777216.0f;};
 for(int n=0;n<512;++n){float x=random()*10,y=random()*10,z=random()*2;float a=random()*6.2831853f;float dx=std::cos(a)*5,dy=std::sin(a)*5;float w=.01f+random()*.1f;
  soup.push_back({x-dx,y-dy,z,0});soup.push_back({x+dx,y+dy,z+.01f,0});soup.push_back({x+dx-dy*w,y+dy+dx*w,z+.02f,0});}
 BVH hq;hq.settings.useSIMDifavailable=false;hq.settings.enableThreading=false;hq.settings.binnedBVH=true;hq.BuildHQ(soup.data(),512);
 uint32_t refs=0;std::set<uint32_t> ids;
 for(uint32_t n=0;n<hq.usedNodes;++n){if(n==1)continue;const auto&node=hq.bvhNode[n];if(node.isLeaf())for(uint32_t j=0;j<node.triCount;++j){++refs;ids.insert(hq.primIdx[node.leftFirst+j]);}}
 if(ids.size()!=512||hq.isRefittable())return 7;
 Ray actual({5,5,-1},{0,0,2});hq.Intersect(actual);
 // Independently intersect all original triangles with Moller-Trumbore.
 float best=BVH_FAR;
 for(size_t i=0;i<soup.size();i+=3){bvhvec3 a(soup[i]),b(soup[i+1]),c(soup[i+2]);auto e1=b-a,e2=c-a,p=reference_cross(actual.D,e2);float det=reference_dot(e1,p);if(std::fabs(det)<1e-10f)continue;auto tv=actual.O-a;float u=reference_dot(tv,p)/det;if(u<0||u>1)continue;auto qq=reference_cross(tv,e1);float v=reference_dot(actual.D,qq)/det;if(v<0||u+v>1)continue;float t=reference_dot(e2,qq)/det;if(t>=0&&t<best)best=t;}
 if((best==BVH_FAR)!=(actual.hit.t==BVH_FAR)|| (best<BVH_FAR&&!near(best,actual.hit.t)))return 8;
 std::printf("BASELINE_HQ_OK prims=512 leaf_refs=%u duplicate_refs=%u refittable=0 oracle=1\n",refs,refs-512);
}
