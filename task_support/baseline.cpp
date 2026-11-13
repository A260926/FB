// Local inputs for the existing nearest-hit path; no ordered traversal implementation.
#define TINYBVH_IMPLEMENTATION
#include "tiny_bvh.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace tinybvh;
static void check(bool b, const char* s) { if (!b) { std::fprintf(stderr,"SEED_FAIL %s\n",s); std::exit(1); } }
static bool near(double a,double b) { return std::fabs(a-b)<1e-5; }
int main() {
    bvhvec4 triangles[] = {
        {-1,-1,2,0},{1,-1,2,0},{0,1,2,0},
        {-1,-1,2,0},{1,-1,2,0},{0,1,2,0},
        {-1,-1,4,0},{1,-1,4,0},{0,1,4,0}
    };
    BVH blas; blas.Build(triangles,3);
    Ray r({0,0,0},{0,0,2}); blas.Intersect(r);
    check(near(r.hit.t,1),"nearest distance");
    check(r.hit.prim<2 && near(r.hit.u,.25) && near(r.hit.v,.5),"original identity and barycentrics");
    Ray cap({0,0,0},{0,0,2},1); check(!blas.IsOccluded(cap),"half-open cap");
    Ray shadow({0,0,0},{0,0,2},1.01f); check(blas.IsOccluded(shadow),"shadow");
    std::puts("SEED_NEAREST t=1 equal_depth_primitives=2 returned=1");
    BLASInstance inst[2];
    inst[0].transform[0]=2; inst[0].transform[5]=3; inst[0].transform[10]=4; inst[0].transform[11]=6; inst[0].mask=1;
    inst[1].transform[11]=20; inst[1].mask=2;
    BVH* blasses[]={&blas}; BVH tlas; tlas.Build(inst,2,blasses,1);
    Ray a({0,0,0},{0,0,2},100,1); tlas.Intersect(a);
    check(near(a.hit.t,7) && a.hit.inst==0,"shared instance affine parameter");
    Ray b({0,0,0},{0,0,2},100,2); tlas.Intersect(b);
    check(near(b.hit.t,11) && b.hit.inst==1,"mask and second instance");
    check(near(b.O.z,0) && near(b.D.z,2),"ray restored");
    uint32_t idx[]={6,7,8,0,1,2,3,4,5}; BVH indexed; indexed.Build(triangles,idx,3);
    Ray c({0,0,0},{0,0,2}); indexed.Intersect(c); check(near(c.hit.t,1) && c.hit.prim>0,"indexed primitive identity");
    bvhdbl3 td[]={{-1,-1,2},{1,-1,2},{0,1,2}}; BVH_Double bd; bd.Build(td,1);
    RayEx d({0,0,0},{0,0,2}); bd.Intersect(d); check(near(d.hit.t,1),"double path");
    std::puts("SEED_TLAS affine_t=7 mask_t=11 indexed=ok double=ok");
    std::puts("TINYBVH_BASELINE_OK");
}
