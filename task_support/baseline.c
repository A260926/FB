#include "gl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { SIDE=16, ATTR=7 };
struct capture { float value[SIDE*SIDE]; unsigned calls; };
static void vertex(float *out,float *in,void *u) {
    (void)u; memcpy(out,in,ATTR*sizeof(float));
}
static void fragment(uint32_t *out,float *in,void *u) {
    struct capture *c=u;
    int x=(int)floorf(in[0]), y=(int)floorf(in[1]);
    if(x<0||x>=SIDE||y<0||y>=SIDE) { fprintf(stderr,"outside framebuffer\n"); exit(6); }
    c->value[y*SIDE+x]=in[4]; ++c->calls; *out=0x224466;
}
static void clear(uint32_t *colors,float *depths,struct capture *cap) {
    memset(cap,0,sizeof(*cap));
    for(int i=0;i<SIDE*SIDE;i++) { colors[i]=0; depths[i]=INFINITY; }
}
int main(void) {
    float vertices[3][ATTR]={
        {-.75f,.75f,0,1,0,0,0},
        {-1.5f,-1.5f,0,2,1,0,0},
        {3,3,0,4,.5f,0,0}
    };
    int indices[3]={0,1,2};
    uint32_t colors[SIDE*SIDE]; float depths[SIDE*SIDE]; struct capture cap;
    struct gl_framebuffer f={colors,depths,SIDE,SIDE};
    struct gl_pipeline p={.fbuf=&f,.uniform=&cap,.vs=vertex,.fs=fragment,
        .pts_in=&vertices[0][0],.n_pts=3,.n_attr_in=ATTR,.n_attr_out=ATTR,
        .winding=GL_WINDING_ORDER_CCW};
    clear(colors,depths,&cap);
    gl_render(&p,indices,3,GL_TRIANGLE_LIST);
    double a=7.0/12,b=2.5/12,c=2.5/12;
    double smooth=(b/2+c*.5/4)/(a+b/2+c/4), affine=b+c*.5;
    float direct=cap.value[4*SIDE+4]; unsigned direct_calls=cap.calls;
    if(!cap.calls||fabs(direct-smooth)>1e-5||fabs(direct-affine)<.01||colors[4*SIDE+4]!=0x224466) return 2;
    clear(colors,depths,&cap);
    gl_bind_framebuffer(SIDE,SIDE,colors,depths); gl_bind_vertices(&vertices[0][0],3,ATTR);
    gl_bind_uniform(&cap); gl_bind_custom_vs(vertex,ATTR); gl_bind_custom_fs(fragment);
    gl_renderl(indices,3,GL_TRIANGLE_LIST);
    if(cap.calls!=direct_calls||fabs(cap.value[4*SIDE+4]-direct)>1e-6) return 3;
    printf("SMOOTH direct=%.8f bound=%.8f affine=%.8f fragments=%u\n",direct,cap.value[4*SIDE+4],affine,cap.calls);
    vertices[0][0]=-1.5f;
    clear(colors,depths,&cap); gl_render(&p,indices,3,GL_TRIANGLE_LIST);
    if(!cap.calls) return 4;
    printf("CLIPPED fragments=%u\n",cap.calls);
    puts("BASELINE_OK production_gl_render_and_gl_renderl");
    return 0;
}
