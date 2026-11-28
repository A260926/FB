#include "tiny_bvh.h"
#include <cstdio>
#include <cmath>
using namespace tinybvh;
int main(){
 bvhvec4 v[]={bvhvec4(-1,-1,2,0),bvhvec4(1,-1,2,0),bvhvec4(0,1,2,0),bvhvec4(-1,-1,4,0),bvhvec4(1,-1,4,0),bvhvec4(0,1,4,0)};
 BVH b;b.settings.useSIMDifavailable=false;b.Build(v,2);
 Ray a(bvhvec3(0,0,0),bvhvec3(0,0,2),10);b.Intersect(a);
 Ray shortRay(bvhvec3(0,0,0),bvhvec3(0,0,2),0.5f),longRay(bvhvec3(0,0,0),bvhvec3(0,0,2),1.5f);
 bool s=b.IsOccluded(shortRay),l=b.IsOccluded(longRay);
 if(std::abs(a.hit.t-1)>1e-6||a.hit.prim!=0||s||!l)return 2;
 std::printf("BASELINE closest_t=%.3f primitive=%u short=%d long=%d\n",a.hit.t,a.hit.prim,int(s),int(l));
 return 0;
}
