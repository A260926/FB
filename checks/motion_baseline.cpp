#include "meshoptimizer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static void require(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "BASELINE_FAILURE: %s\n", message); std::exit(1); }
}

static float normalZ(const float* p)
{
    const float ax = p[3]-p[0], ay = p[4]-p[1];
    const float bx = p[6]-p[0], by = p[7]-p[1];
    return ax*by-ay*bx;
}

int main()
{
    const unsigned int vertices[] = {0,1,2};
    const unsigned char triangle[] = {0,1,2};
    const float begin[] = {0,0,0, 1,0,0, 0,3,0};
    const float end[]   = {0,0,0,-3,0,0, 0,-1,0};
    float middle[9];
    for (int i=0;i<9;++i) middle[i]=(begin[i]+end[i])*0.5f;
    const meshopt_Bounds b0=meshopt_computeMeshletBounds(vertices,triangle,1,begin,3,12);
    const meshopt_Bounds b1=meshopt_computeMeshletBounds(vertices,triangle,1,end,3,12);
    const meshopt_Bounds bm=meshopt_computeMeshletBounds(vertices,triangle,1,middle,3,12);
    require(normalZ(begin)==3 && normalZ(end)==3 && normalZ(middle)==-1,"independent winding witness");
    require(b0.cone_axis[2]>0.999f && b1.cone_axis[2]>0.999f && bm.cone_axis[2]<-0.999f,"static cones follow actual winding");
    require(b0.cone_cutoff<0.001f && b1.cone_cutoff<0.001f && bm.cone_cutoff<0.001f,"single face cones remain useful");
    std::printf("BASELINE_STATIC_BOUNDS_OK cutoff=%.6f\n",b0.cone_cutoff);
    const bool endpointReject0=b0.cone_axis[2]>=b0.cone_cutoff;
    const bool endpointReject1=b1.cone_axis[2]>=b1.cone_cutoff;
    const bool middleReject=bm.cone_axis[2]>=bm.cone_cutoff;
    require(endpointReject0 && endpointReject1 && !middleReject,"endpoint culling cannot certify the interval");
    std::printf("BASELINE_MOTION_WITNESS normal_z=%.0f,%.0f,%.0f endpoint_reject=%d,%d midpoint_reject=%d\n",
                normalZ(begin),normalZ(end),normalZ(middle),int(endpointReject0),int(endpointReject1),int(middleReject));

    // The two static faces are geometrically coincident, with opposite winding.
    const unsigned int opposite[] = {0,1,2,0,2,1};
    meshopt_Meshlet meshlets[2] = {};
    unsigned int mv[6] = {};
    unsigned char mt[6] = {};
    size_t count=meshopt_buildMeshlets(meshlets,mv,mt,opposite,6,begin,3,12,64,64,1.f);
    require(count==1 && meshlets[0].triangle_count==2,"soft cone scoring still packs opposite faces");
    meshopt_Bounds mixed=meshopt_computeMeshletBounds(mv,mt,2,begin,3,12);
    require(mixed.cone_cutoff==1.f,"opposite winding disables the cone");
    std::printf("BASELINE_SOFT_WEIGHT meshlets=%zu triangles=%u cutoff=%.0f\n",count,meshlets[0].triangle_count,mixed.cone_cutoff);

    const float translated[] = {10,0,0,11,0,0,10,3,0};
    meshopt_Bounds bt=meshopt_computeMeshletBounds(vertices,triangle,1,translated,3,12);
    require(std::fabs(bt.cone_axis[2]-b0.cone_axis[2])<1e-6f,"translation keeps the normal cone");
    float dx=translated[0]-b0.center[0],dy=translated[1]-b0.center[1],dz=translated[2]-b0.center[2];
    require(dx*dx+dy*dy+dz*dz>b0.radius*b0.radius,"one pose sphere cannot cover the motion");
    std::printf("BASELINE_TRANSLATION normal_preserved=1 start_sphere_misses_end=1\n");
    return 0;
}
