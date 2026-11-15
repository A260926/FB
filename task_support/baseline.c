#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <v.h>
#include <c-ray/c-ray.h>
#include <c-ray/node.h>
#include <datatypes/scene.h>
#include <nodes/colornode.h>
#include <nodes/shaders/background.h>
#include <renderer/pathtrace.h>
#include <renderer/samplers/sampler.h>
#include <accelerators/bvh.h>
static char image_path[]=".task-build/gray.ppm";
static struct cr_color_node desc(unsigned opt) {
    struct cr_color_node n={.type=cr_cn_image}; n.arg.image.full_path=image_path; n.arg.image.options=opt; return n;
}
static float value(const struct colorNode *n) {
    struct hitRecord h={0}; h.uv=(struct coord){.25f,.25f}; return n->eval(n,NULL,&h).red;
}
static int normal(void) {
    struct world *w=scene_new(); struct cr_color_node d=desc(NO_BILINEAR);
    const struct colorNode *n=build_color_node((struct cr_scene*)w,&d);
    v_threadpool_wait(w->bg_worker);
    float raw=value(n);
    w->background=newBackground(&w->storage,n,NULL,NULL,false);
    w->topLevel=build_top_level_bvh(w->instances);
    sampler *s=sampler_new();sampler_init(s,Halton,0,1,0);
    struct lightRay ray={.direction={0,0,-1},.type=rt_camera};
    struct color c=path_trace(ray,w,0,s);
    int ok=fabsf(raw-128.f/255.f)<1e-6f && fabsf(c.red-raw)<1e-6f;
    sampler_destroy(s); scene_destroy(w);
    if(!ok)return 1;
    printf("NORMAL_IMAGE_PATH_OK raw=%.9f traced=%.9f\n",raw,c.red);return 0;
}
static int order(unsigned first,float *raw,float *converted) {
    struct world *w=scene_new();
    struct cr_color_node a=desc(NO_BILINEAR|first),b=desc(NO_BILINEAR|(first^SRGB_TRANSFORM));
    const struct colorNode *na=build_color_node((struct cr_scene*)w,&a),*nb=build_color_node((struct cr_scene*)w,&b);
    v_threadpool_wait(w->bg_worker);
    *raw=value(first?nb:na);*converted=value(first?na:nb);
    scene_destroy(w);return 0;
}
int main(int argc,char **argv) {
    FILE *f=fopen(image_path,"wb");if(!f)return 2;
    const unsigned char px[]={128,128,128,128,128,128,128,128,128,128,128,128};
    fputs("P6\n2 2\n255\n",f);if(fwrite(px,1,sizeof px,f)!=sizeof px)return 3;fclose(f);
    if(normal())return 4;
    if(argc>1 && !strcmp(argv[1],"reproduce")) {
        float r1,l1,r2,l2;order(0,&r1,&l1);order(SRGB_TRANSFORM,&r2,&l2);
        printf("ORDER_COUPLING_REPRO raw_first=%.9f linear_after=%.9f linear_first=%.9f raw_after=%.9f\n",r1,l1,l2,r2);
        if(fabsf(r1-l1)>1e-7f || fabsf(r2-l2)>1e-7f || fabsf(r1-r2)<.2f)return 5;
    }
    return 0;
}
