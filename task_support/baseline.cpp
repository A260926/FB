#include "headers/Octree.h"
#include <cstdio>
int main() {
    Options::nologs=true; Options::TRANSPARENCY=true;
    Options::SMOOTH_SIBLINGS=false; Options::VOXEL_SHELL_TYPE=0;
    Options::AVOID_SCAN_GLITCHES=false; Options::ACCELERATED=false;
    Options::OCTREE_SIZE=16; Options::MIN_VOXELS_PROXIMITY=0;
    Octree tree; tree.initRoot(16,4,24);
    Voxel voxels[]={{{0,0,0},0xff000080},{{0,0,3},0x00ff00ff}};
    tree.addVoxels(voxels,2);
    Mat3f basis={{1,0,0},{0,1,0},{0,0,1}};tree.initBasis(&basis);
    Vec3<float> ray={0,0,-5}; tree.setRay(&ray);
    std::vector<DynamicVoxel> hits;
    if(!tree.rayTrace(&hits)||hits.size()!=2) return 2;
    if(hits[0].voxel->color!=voxels[0].color||hits[1].voxel->color!=voxels[1].color) return 3;
    if(hits[0].depth!=5||hits[1].depth!=8) return 4;
    std::printf("BASELINE_CPU hits=%zu depths=%.1f,%.1f colors=%08x,%08x\n",hits.size(),hits[0].depth,hits[1].depth,hits[0].voxel->color,hits[1].voxel->color);
    return 0;
}
