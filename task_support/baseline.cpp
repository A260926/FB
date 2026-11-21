#include "../voxelizer.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

using Key=std::array<int,3>;
static void need(bool v,const char* why){if(!v){std::fprintf(stderr,"BASELINE_FAIL %s\n",why);std::exit(2);}}
static Key key(vx_vertex_t v){return {{int(std::lround(v.x)),int(std::lround(v.y)),int(std::lround(v.z))}};}
static std::map<Key,vx_color_t> cloud(vx_point_cloud_t* p){
    std::map<Key,vx_color_t> out;
    for(size_t i=0;i<p->nvertices;++i){need(out.emplace(key(p->vertices[i]),p->colors[i]).second,"unique cell");}
    return out;
}
int main(){
    vx_mesh_t* m=vx_mesh_alloc(6,6);need(m!=nullptr,"mesh allocation");
    for(int tri=0;tri<2;++tri){
        const float xy[3][2]={{.05f,.05f},{.20f,.05f},{.05f,.20f}};
        for(int j=0;j<3;++j){int i=3*tri+j;m->indices[i]=i;
            m->vertices[i].x=xy[j][0];m->vertices[i].y=xy[j][1];m->vertices[i].z=.1f;
            m->colors[i].r=tri?0:1;m->colors[i].g=0;m->colors[i].b=tri?1:0;
        }
    }
    vx_point_cloud_t* p=vx_voxelize_pc(m,1,1,1,0);need(p&&p->nvertices==1,"analytic one-cell overlap");
    auto before=cloud(p);need(before.count(Key{{0,0,0}})==1,"cell origin");
    need(before.begin()->second.r>.999f&&before.begin()->second.b<1e-6f,"first triangle owns color");
    vx_mesh_t* cubes=vx_voxelize(m,1,1,1,0);
    need(cubes&&cubes->nvertices==8&&cubes->nindices==36&&cubes->nnormals==6,"real cube export");
    for(size_t i=0;i<cubes->nindices;++i)need(cubes->indices[i]<cubes->nvertices,"cube indices");
    for(size_t i=0;i<cubes->nvertices;++i){need(cubes->colors[i].r>.999f,"cube color");
        need(std::fabs(std::fabs(cubes->vertices[i].x)-.5f)<1e-6f,"cube geometry");}
    vx_mesh_free(cubes);vx_point_cloud_free(p);
    for(int j=0;j<3;++j)m->vertices[j].x+=4;
    p=vx_voxelize_pc(m,1,1,1,0);need(p&&p->nvertices==2,"disjoint cells after moving winner");
    auto after=cloud(p);need(after.size()==2&&after.count(Key{{0,0,0}})&&after.count(Key{{4,0,0}}),"moved occupancy");
    need(after.at(Key{{0,0,0}}).b>.999f&&after.at(Key{{4,0,0}}).r>.999f,"remaining triangle supplies color");
    vx_point_cloud_free(p);
    vx_color_t* colors=m->colors;m->colors=nullptr;
    p=vx_voxelize_pc(m,1,1,1,0);need(p&&p->nvertices==2&&!p->colors,"uncolored cloud");
    vx_point_cloud_free(p);cubes=vx_voxelize(m,1,1,1,0);
    need(cubes&&cubes->nvertices==16&&cubes->nindices==72&&!cubes->colors,"uncolored cubes");
    vx_mesh_free(cubes);m->colors=colors;vx_mesh_free(m);
    std::puts("STATIC_OVERLAP cells=1 winner=red");
    std::puts("STATIC_MOVE cells=2 restored=blue moved=red");
    std::puts("VOXELIZER_BASELINE_OK pointcloud=cubes colored=uncolored");
}
