#include "nanort.h"
#include <cmath>
#include <iostream>
int main() {
  double v[]={-.5,-.5,1, -.25,-.5,1, -.25,.5,1, -.5,.5,1};
  const unsigned f[]={0,1,2, 0,2,3};
  nanort::TriangleMesh<double> mesh(v,f,3*sizeof(double));
  nanort::TriangleSAHPred<double> pred(v,f,3*sizeof(double));
  nanort::BVHAccel<double> bvh;
  if(!bvh.Build(2,mesh,pred)) return 2;
  nanort::Ray<double> r; r.org[0]=r.org[1]=r.org[2]=0;
  r.dir[0]=r.dir[1]=0; r.dir[2]=2; r.min_t=0; r.max_t=1;
  nanort::TriangleIntersector<double> inter(v,f,3*sizeof(double));
  nanort::TriangleIntersection<double> h;
  const bool center=bvh.Traverse(r,inter,&h);
  r.dir[0]=-.75;
  const bool strip=bvh.Traverse(r,inter,&h);
  if(center || !strip || std::fabs(h.t-.5)>1e-12) return 3;
  std::cout<<"triangles=2 center_hit="<<center<<" strip_hit="<<strip
           <<" strip_t="<<h.t<<"\n";
  // Moving positions does not change the existing BVH's stored bounds.
  for (unsigned i=0; i<4; ++i) v[3*i] += 2.5;
  r.dir[0]=4.25;
  const bool stale=bvh.Traverse(r,inter,&h);
  nanort::BVHAccel<double> rebuilt;
  if(!rebuilt.Build(2,mesh,pred)) return 4;
  const bool moved=rebuilt.Traverse(r,inter,&h);
  if(stale || !moved || std::fabs(h.t-.5)>1e-12) return 5;
  std::cout<<"animation stale_hit="<<stale<<" rebuilt_hit="<<moved
           <<" rebuilt_t="<<h.t<<"\n";
}
