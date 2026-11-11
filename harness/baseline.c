#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "material.h"
#include "object.h"
#include "accel.h"
#include "image.h"
#include "camera.h"
struct Image image;
struct Camera camera;
bool is_light_blocked(const struct Ray *,float,v3,const struct Object *);
float cast_ray(const struct Ray *,const v3,v3,uint32_t,struct Object *);
int main(void){
 struct Material m[4]={0};v3 zero={0},one={1,1,1};
 for(int i=0;i<4;i++)material_init(&m[i],i,zero,zero,zero,zero,zero,8,1,texture_uniform_new(one));
 m[0].transparent=true;m[0].kt[0]=.5;m[0].kt[1]=.25;m[0].kt[2]=1;
 m[1].transparent=true;m[1].kt[0]=.4;m[1].kt[1]=.8;m[1].kt[2]=.5;
 m[2].ke[0]=m[2].ke[1]=m[2].ke[2]=1;m[2].emittant=true;
 struct Object *a=sphere_new((v3){0,0,2},.25),*b=plane_new((v3){0,0,3},(v3){0,0,1});
 struct Object *lamp=sphere_new((v3){0,0,6},.25),*floor=plane_new(zero,(v3){0,0,1});
 struct Object *all[]={a,b,lamp,floor},*unbound[]={b,floor},*lights[]={lamp};
 objects=all;num_objects=4;unbound_objects=unbound;num_unbound_objects=2;emittant_objects=lights;num_emittant_objects=1;
 for(int i=0;i<4;i++){object_init(all[i],&m[i],1e-5,1,(i==0||i==2)?OBJECT_SPHERE:OBJECT_PLANE);all[i]->object_data->postinit(all[i]);}
 accel_init();struct Ray ray={{0,0,1},{0,0,0}};v3 intensity={1,1,1};
 assert(!is_light_blocked(&ray,7,intensity,lamp));
 assert(fabsf(intensity[0]-.2f)<1e-6&&fabsf(intensity[1]-.2f)<1e-6&&fabsf(intensity[2]-.5f)<1e-6);
 printf("SHADOW_RGB %.6f %.6f %.6f\n",intensity[0],intensity[1],intensity[2]);
 struct Ray view={{0,0,-1},{0,0,1}};v3 color={0};srand(4);cast_ray(&view,one,color,0,NULL);
 for(int i=0;i<3;i++)assert(isfinite(color[i])&&color[i]>0);
 printf("CAST_RAY_RGB %.9f %.9f %.9f\n",color[0],color[1],color[2]);
 accel_deinit();for(int i=0;i<4;i++){all[i]->object_data->delete(all[i]);free(m[i].texture);}
 puts("SHADOW_BASELINE_OK");return 0;
}
