#include "headers/Octree.h"
#include "headers/Camera.h"
#include <cassert>
#include <cmath>
#include <cstdio>
static bool hit(Octree& t, int x, unsigned *color=nullptr) {
 Mat3f b={{1,0,0},{0,1,0},{0,0,1}}; Vec3<float> r={(float)x,0,-6};
 t.initBasis(&b);t.setRay(&r);std::vector<DynamicVoxel> out;
 bool yes=t.rayTrace(&out);if(yes && color)*color=out[0].voxel->color;return yes;
}
int main(){
 Options::nologs=true;Options::ACCELERATED=false;Options::TRANSPARENCY=false;
 Options::SMOOTH_SIBLINGS=false;Options::VOXEL_SHELL_TYPE=0;Options::AVOID_SCAN_GLITCHES=false;
 Octree t;t.initRoot(32,5,24);Voxel v[]={{{0,0,0},0xF03020FF},{{8,0,0},0x30F020FF}};
 t.addVoxels(v,2);unsigned c=0;assert(hit(t,0,&c)&&c==0xF03020FF);assert(hit(t,8));assert(!hit(t,-8));
 Camera a;a.rotate({0,1,0},.2f);a.rotate({0,1,0},.2f);Mat3f expected;a.getBasis(&expected);
 assert(std::isfinite(expected.k.x)&&std::fabs(expected.k.x)>.1f);
 Camera interleaved;interleaved.rotate({0,1,0},.2f);Camera other;other.rotate({1,0,0},0);
 interleaved.rotate({0,1,0},.2f);Mat3f actual;interleaved.getBasis(&actual);
 std::printf("camera_isolated_kx=%.7f camera_interleaved_kx=%.7f\n",expected.k.x,actual.k.x);
 Octree small;small.initRoot(8,3,24);Voxel extra={ {9,0,0},0x123456FF};t.addVoxels(&extra,1);
 std::printf("legal_large_tree_insert_visible=%d\n",hit(t,9)?1:0);
 std::puts("BASELINE_OK production_hits=2 miss=1 camera=finite");
}
