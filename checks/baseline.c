#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "renderer/core/api.h"
#include "renderer/shaders/blinn_shader.h"
#include "renderer/shaders/pbr_shader.h"
#include "renderer/shaders/cache_helper.h"
#include "renderer/tests/test_helper.h"
#define REQUIRE(e) do { if (!(e)) {fprintf(stderr,"BASELINE_FAIL line=%d %s\n",__LINE__,#e); exit(9);} }while(0)
static perframe_t frame(void) {
    perframe_t p; memset(&p,0,sizeof p);
    p.camera_view_matrix = mat4_identity();
    p.camera_proj_matrix = mat4_perspective(1.57079632679f,1,1,9);
    p.light_view_matrix = p.light_proj_matrix = mat4_identity();
    p.light_dir = vec3_new(0,0,-1); p.camera_pos = vec3_new(0,0,0);
    p.punctual_intensity = 1; p.layer_view = -1; return p;
}
static void one(int kind) {
    model_t *model; model_t **models = NULL; scene_t *s;
    framebuffer_t *f=framebuffer_create(32,32); perframe_t p=frame();
    mat4_t t=mat4_translate(0,0,-2); unsigned lit=0; int i;
    if (kind==0 || kind==3) {
        blinn_material_t m; memset(&m,0,sizeof m); m.basecolor=vec4_new(1,0,0,kind==3?0:1);
        m.shininess=8; m.double_sided=1; m.alpha_cutoff=0.5f;
        model=blinn_create_model("checks/triangle.obj",t,NULL,-1,&m);
    } else if (kind==1) {
        pbrm_material_t m; memset(&m,0,sizeof m); m.basecolor_factor=vec4_new(1,0,0,1);
        m.roughness_factor=.7f; m.double_sided=1;
        model=pbrm_create_model("checks/triangle.obj",t,NULL,-1,&m,NULL);
    } else {
        pbrs_material_t m; memset(&m,0,sizeof m); m.diffuse_factor=vec4_new(1,0,0,1);
        m.glossiness_factor=.3f; m.double_sided=1;
        model=pbrs_create_model("checks/triangle.obj",t,NULL,-1,&m,NULL);
    }
    darray_push(models,model); s=scene_create(vec3_new(0,0,0),NULL,models,0,1,0,0);
    test_draw_scene(s,f,&p);
    for(i=0;i<32*32;i++) if(f->depth_buffer[i]<1)lit++;
    if(kind==3) {REQUIRE(lit==0); printf("OLD_ALPHA_CUTOFF depth-untouched=1024\n");}
    else {
        REQUIRE(lit>50); REQUIRE(fabs(f->depth_buffer[16*32+16]-.5625f)<1e-6);
        REQUIRE(f->color_buffer[(16*32+16)*4]>0);
        printf("OLD_SCENE_DRAW kind=%d covered=%u center-depth=%.6f rgb=%u,%u,%u\n",kind,lit,f->depth_buffer[16*32+16],f->color_buffer[(16*32+16)*4],f->color_buffer[(16*32+16)*4+1],f->color_buffer[(16*32+16)*4+2]);
    }
    framebuffer_release(f); scene_release(s);
}
int main(void) {int k; for(k=0;k<4;k++)one(k); cache_cleanup(); puts("RENDERER_BASELINE_COMPLETE materials=3 cutoff=1 upstream=test_draw_scene"); return 0;}
