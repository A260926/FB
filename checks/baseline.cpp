// Fixture, MIT. No baking implementation.
#include "nanort.h"
#include <cassert>
#include <cstdio>
#include <cmath>
int main(){float v[]={-2,-2,1,2,-2,1,0,2,1,-2,-2,3,2,-2,3,0,2,3};unsigned f[]={0,1,2,3,4,5};nanort::TriangleMesh<float> mesh(v,f,12);nanort::TriangleSAHPred<float> pred(v,f,12);nanort::BVHAccel<float> b;assert(b.Build(2,mesh,pred));nanort::TriangleIntersector<float> inter(v,f,12);nanort::Ray<float> ray;ray.org[0]=ray.org[1]=ray.org[2]=0;ray.dir[0]=ray.dir[1]=0;ray.dir[2]=1;ray.min_t=0;ray.max_t=2;nanort::TriangleIntersection<float> hit;assert(b.Traverse(ray,inter,&hit));assert(hit.prim_id==0&&std::fabs(hit.t-1)<1e-6);printf("NANORT_NEAR id=%u t=%.1f uv=%.2f,%.2f\n",hit.prim_id,hit.t,hit.u,hit.v);ray.max_t=.5f;assert(!b.Traverse(ray,inter,&hit));puts("NANORT_SEGMENT_MISS_OK");nanort::BVHTraceOptions opts;opts.skip_prim_id=0;ray.max_t=4;assert(b.Traverse(ray,inter,&hit,opts));assert(hit.prim_id==1&&std::fabs(hit.t-3)<1e-6);puts("NANORT_SKIP_TO_FAR_OK");}
