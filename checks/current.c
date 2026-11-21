#include "render.h"
#include "object.h"
#include "material.h"
#include "accel.h"
#include "argv.h"
#include <stdio.h>
#include <stdlib.h>

#include <float.h>
#include <math.h>
float cast_ray(const struct Ray*,const v3,v3,uint32_t,struct Object*);
void get_closest_intersection(const struct Ray*,struct Object**,v3,float*);
#define CHECK(x) do {if(!(x)){fprintf(stderr,"CHECK line %d\n",__LINE__);return 1;}}while(0)
int main(void){
 char *args[]={"offscreen","-l","none"}; myargc=3; myargv=args; argv_init(); render_init();
 struct Material m[2]; v3 zero={0,0,0},le={1,2,3};
 material_init(&m[0],0,zero,zero,zero,zero,le,1,1,texture_uniform_new(zero));
 material_init(&m[1],1,zero,zero,zero,zero,zero,1,1,texture_uniform_new(zero));
 num_objects=2;num_emittant_objects=1;objects_init();
 objects[0]=sphere_new((v3){0,0,0},1);objects[1]=sphere_new((v3){4,1,0},.5f);
 for(int i=0;i<2;i++){object_init(objects[i],&m[i],1e-5f,1,OBJECT_SPHERE);objects[i]->object_data->postinit(objects[i]);}
 emittant_objects[0]=objects[0];accel_init();
 struct Ray ray={{0,0,-1},{0,0,3}};struct Object *hit=NULL;v3 n;float t=FLT_MAX;
 get_closest_intersection(&ray,&hit,n,&t);CHECK(hit==objects[0]&&fabsf(t-2)<1e-5f&&fabsf(n[2]-1)<1e-5f);
 printf("BVH_CPU distance=%.1f normal_z=%.1f\n",t,n[2]);
 v3 rgb={0,0,0},weight={1,1,1};float depth=cast_ray(&ray,weight,rgb,1,NULL);
 CHECK(fabsf(depth-2)<1e-5f&&fabsf(rgb[0]-1)<1e-5f&&fabsf(rgb[1]-2)<1e-5f&&fabsf(rgb[2]-3)<1e-5f);
 printf("CAST_RAY_CPU rgb=%.1f,%.1f,%.1f depth=%.1f\n",rgb[0],rgb[1],rgb[2],depth);
 accel_deinit();objects_deinit();free(m[0].texture);free(m[1].texture);argv_deinit();puts("CURRENT_RENDER_OK");return 0;
}
