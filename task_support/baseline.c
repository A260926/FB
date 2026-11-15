#define main c_ray_original_main
#include "../c-ray-fast/c-ray-fast.c"
#undef main
#include <assert.h>
int main(void) {
 struct object obs[2]={0}; struct light lamp={0};
 struct object *leaf[1]; struct octnode root={0};
 struct ray r={0}; struct spoint hit; struct color color;
 obs[0].type=OBJ_SPHERE; obs[0].o.sph.rad=1;
 obs[0].mat=def_mtl; obs[0].mat.kd.r=.5f; obs[0].mat.kd.g=.25f; obs[0].mat.kd.b=.125f; obs[0].mat.spow=0;
 obs[1].type=OBJ_PLANE; obs[1].o.pln.pos.z=3; obs[1].o.pln.norm.z=-1; obs[1].mat=def_mtl;
 objects=obs;num_obj=2;infobj=&obs[1];num_inf=1;
 leaf[0]=&obs[0];root.obj=leaf;root.num_obj=1;
 root.aabb.vmin=(struct vec3){-1,-1,-1};root.aabb.vmax=(struct vec3){1,1,1};octroot=&root;
 assert(build_octree(octroot,0)==0);
 lamp.pos=(struct vec3){0,0,-3};lamp.col=(struct color){1,1,1,1};lights=&lamp;num_lt=1;
 r.orig=(struct vec3){0,0,-3};r.dir=(struct vec3){0,0,6};
 assert(ray_scene(&r,&hit)&&hit.obj==&obs[0]&&fabs(hit.dist-1.0/3.0)<1e-12);
 assert(ray_scene(&r,0));
 r.dir.z=1;assert(!ray_scene(&r,0));
 r.dir.z=6;trace(&r,0,&color);
 assert(fabs(color.r-.505f)<1e-6&&fabs(color.g-.2525f)<1e-6&&fabs(color.b-.12625f)<1e-6);
 printf("BASELINE sphere_t=%.12f finite_shadow=clear rgb=%.6f,%.6f,%.6f\n",hit.dist,color.r,color.g,color.b);
 puts("BASELINE_COMPLETE octree/nearest/finite-shadow/trace/shade");return 0;
}
