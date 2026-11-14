#include <stdint.h>
#include <stddef.h>
#include "src/ogt_voxel_meshify.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
struct Tracker {int live,calls;};
static void* take(size_t n,void* u){Tracker* t=(Tracker*)u;t->calls++;void* p=std::malloc(n);if(p)t->live++;return p;}
static void drop(void* p,void* u){if(p){((Tracker*)u)->live--;std::free(p);}}
static double area(const ogt_mesh* m){
 double out=0;
 for(unsigned i=0;i<m->index_count;i+=3){
  const ogt_mesh_vertex&a=m->vertices[m->indices[i]],&b=m->vertices[m->indices[i+1]],&c=m->vertices[m->indices[i+2]];
  double ux=b.pos.x-a.pos.x,uy=b.pos.y-a.pos.y,uz=b.pos.z-a.pos.z;
  double vx=c.pos.x-a.pos.x,vy=c.pos.y-a.pos.y,vz=c.pos.z-a.pos.z;
  double nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;
  assert(nx*a.normal.x+ny*a.normal.y+nz*a.normal.z>0);
  assert(a.palette_index==1&&b.palette_index==1&&c.palette_index==1);
  out+=std::sqrt(nx*nx+ny*ny+nz*nz)*.5;
 }return out;
}
static unsigned junctions(const ogt_mesh* m){
 unsigned count=0;
 for(unsigned i=0;i<m->index_count;i+=3)for(unsigned e=0;e<3;e++){
  const ogt_mesh_vec3&a=m->vertices[m->indices[i+e]].pos,&b=m->vertices[m->indices[i+(e+1)%3]].pos;
  double ux=b.x-a.x,uy=b.y-a.y,uz=b.z-a.z,len=ux*ux+uy*uy+uz*uz;
  for(unsigned j=0;j<m->vertex_count;j++){
   const ogt_mesh_vec3&p=m->vertices[j].pos;double vx=p.x-a.x,vy=p.y-a.y,vz=p.z-a.z,dot=vx*ux+vy*uy+vz*uz;
   if(dot<=0||dot>=len)continue;
   if(uy*vz-uz*vy==0&&uz*vx-ux*vz==0&&ux*vy-uy*vx==0){count++;break;}
  }
 }return count;
}
int main(){
 Tracker t={0,0};ogt_voxel_meshify_context ctx={take,drop,&t};ogt_mesh_rgba palette[256]={};palette[1]={61,109,173,255};
 uint8_t cube[1]={1};ogt_mesh* m=ogt_mesh_from_paletted_voxels_greedy(&ctx,cube,1,1,1,palette);assert(m&&m->index_count==36&&area(m)==6);ogt_mesh_destroy(&ctx,m);assert(t.live==0);
 uint8_t shape[4]={1,1,1,0};ogt_mesh* simple=ogt_mesh_from_paletted_voxels_simple(&ctx,shape,2,2,1,palette);
 ogt_mesh* greedy=ogt_mesh_from_paletted_voxels_greedy(&ctx,shape,2,2,1,palette);
 assert(simple&&greedy);assert(area(simple)==14&&area(greedy)==14);assert(greedy->index_count<simple->index_count);
 unsigned n=junctions(greedy);assert(n>0);
 std::printf("BASELINE voxel-greedy PASS area=14 t_junction_edges=%u triangles=%u\n",n,greedy->index_count/3);
 ogt_mesh_destroy(&ctx,simple);ogt_mesh_destroy(&ctx,greedy);assert(t.live==0);std::puts("BASELINE allocator PASS live=0");
}
