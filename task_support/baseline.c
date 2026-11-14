#define VOXELIZER_IMPLEMENTATION
#include "voxelizer.h"
#include <assert.h>
#include <stdio.h>
static vx_mesh_t* sample(int reverse) {
 vx_mesh_t *m=vx_mesh_alloc(6,6);
 for(int i=0;i<6;i++){m->vertices[i].x=(i%3==1?0.35f:-0.35f);m->vertices[i].y=(i%3==2?0.35f:-0.35f);m->vertices[i].z=0.1f;m->colors[i].r=i<3?1:0;m->colors[i].b=i<3?0:1;m->indices[i]=reverse?(i+3)%6:i;}
 return m;
}
int main(void){
 vx_mesh_t *a=sample(0),*b=sample(1);vx_point_cloud_t *p=vx_voxelize_pc(a,1,1,1,0),*q=vx_voxelize_pc(b,1,1,1,0);
 assert(p && q && p->nvertices==q->nvertices && p->nvertices>0);double delta=0;
 for(size_t i=0;i<p->nvertices;i++){assert(isfinite(p->colors[i].r));delta+=fabs(p->colors[i].r-q->colors[i].r);}
 vx_mesh_t* cubes=vx_voxelize(a,1,1,1,0);assert(cubes->nvertices==8*p->nvertices && cubes->nindices==36*p->nvertices);
 printf("VOXEL_PC_MESH_OK voxels=%zu cubes=%zu\nORDER_COLOR_DELTA=%g\n",p->nvertices,cubes->nvertices,delta);
 vx_mesh_free(cubes);vx_point_cloud_free(p);vx_point_cloud_free(q);vx_mesh_free(a);vx_mesh_free(b);
}
