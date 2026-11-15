#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "accel.h"
#include "argv.h"
#include "calc.h"
#include "material.h"
#include "object.h"
#include "render.h"

extern bool is_light_blocked(const struct Ray *, float, v3, const struct Object *);
extern float cast_ray(const struct Ray *, const v3, v3, uint32_t, struct Object *);
static struct ObjectVTable emitter_vtable;
static int failures;
static void near_value(const char *name, double got, double expected) {
    if (!isfinite(got) || fabs(got-expected)>2e-6) {
        fprintf(stderr,"BASELINE_CONTENT_FAIL %s got=%.12g expected=%.12g\n",name,got,expected);
        failures++;
    }
}
static void fixed_point(const struct Object *o, const v3 p, v3 out) {
    (void)o; (void)p;
    assign3(out,(v3){0,0,5});
}
static struct Object *new_sphere(const v3 p,float radius,struct Material *m) {
    struct Object *o=sphere_new(p,radius);
    object_init(o,m,0.0001f,1,OBJECT_SPHERE);
    o->object_data->postinit(o);
    return o;
}
int main(void) {
    struct Material receiver,filter,emitter;
    const v3 zero={0,0,0};
    struct Texture *t=texture_uniform_new((v3){.5f,.25f,1});
    struct Texture *black=texture_uniform_new(zero);
    material_init(&receiver,1,zero,zero,zero,zero,zero,2,1,t);
    material_init(&filter,2,zero,zero,zero,(v3){.5f,.25f,.75f},zero,2,1,black);
    material_init(&emitter,3,zero,zero,zero,zero,(v3){8,4,2},2,1,black);
    num_objects=3;num_emittant_objects=1;num_unbound_objects=0;
    objects_init();
    objects[0]=new_sphere((v3){0,0,0},1,&receiver);
    objects[1]=new_sphere((v3){0,0,3},.25f,&filter);
    objects[2]=new_sphere((v3){0,0,5},.1f,&emitter);
    emitter_vtable=*objects[2]->object_data;
    emitter_vtable.get_light_point=fixed_point;
    objects[2]->object_data=&emitter_vtable;
    emittant_objects[0]=objects[2];
    accel_init();
    struct Ray shadow={{0,0,1},{0,0,1}};
    v3 intensity={1,1,1};
    if(is_light_blocked(&shadow,4,intensity,objects[2]))failures++;
    near_value("shadow.red",intensity[0],.5);
    near_value("shadow.green",intensity[1],.25);
    near_value("shadow.blue",intensity[2],.75);
    struct Ray view={{0,0,-1},{0,0,2}};
    v3 color={0,0,0};
    srand(1675);int expected_random=rand();srand(1675);
    float depth=cast_ray(&view,(v3){1,1,1},color,1,NULL);
    near_value("view.depth",depth,1);
    near_value("cast.red",color[0],.5/17);
    near_value("cast.green",color[1],.0625/17);
    near_value("cast.blue",color[2],.375/17);
    printf("BASELINE_TRANSPORT rgb=%.9g,%.9g,%.9g depth=%.9g\n",(double)color[0],(double)color[1],(double)color[2],(double)depth);
    if(rand()!=expected_random) {fprintf(stderr,"BASELINE_CONTENT_FAIL random stream\n");failures++;}
    filter.transparent=false;
    assign3(color,zero);
    cast_ray(&view,(v3){1,1,1},color,1,NULL);
    for(size_t i=0;i<3;i++)near_value("opaque",color[i],0);
    filter.transparent=true;
    accel_deinit();objects_deinit();free(t);free(black);
    if(failures)return 1;
    puts("BASELINE_COMPLETE checks=11");
    return 0;
}
