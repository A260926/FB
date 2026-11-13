/* Fixture, MIT: actual unmodified CPU clip, varying, depth and discard. */
#include "graphics.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
typedef struct{vec4_t p;} A;typedef struct{float value;} V;typedef struct{vec4_t color;int discard,calls;} U;
static vec4_t vs(void*a,void*v,void*u){((V*)v)->value=1;return ((A*)a)->p;}
static vec4_t fs(void*v,void*u,int*d,int back){U*p=u;++p->calls;*d=p->discard;return p->color;}
static void draw(framebuffer_t*f,program_t*p,float z,vec4_t color,int discard){float xy[3][2]={{-1.2f,-.8f},{1.2f,-.8f},{0,.9f}};for(int i=0;i<3;++i)((A*)program_get_attribs(p,i))->p=vec4_new(xy[i][0],xy[i][1],z,1);U*u=program_get_uniforms(p);u->color=color;u->discard=discard;graphics_draw_triangle(f,p);}
int main(){framebuffer_t*f=framebuffer_create(32,32);program_t*p=program_create(vs,fs,sizeof(A),sizeof(V),sizeof(U),1,0);int k=16*32+16;draw(f,p,.4f,vec4_new(0,0,1,1),0);assert(fabsf(f->depth_buffer[k]-.7f)<1e-5);draw(f,p,-.2f,vec4_new(0,1,0,1),1);assert(f->color_buffer[4*k+2]==255&&fabsf(f->depth_buffer[k]-.7f)<1e-5);puts("RENDERER_DISCARD_PRESERVES_BLUE_DEPTH_0.7");draw(f,p,-.4f,vec4_new(1,0,0,1),0);assert(f->color_buffer[4*k]==255&&fabsf(f->depth_buffer[k]-.3f)<1e-5);U*u=program_get_uniforms(p);int before=u->calls;draw(f,p,.8f,vec4_new(0,1,0,1),0);assert(u->calls==before);printf("RENDERER_RED_DEPTH=%.1f EARLY_REJECT_OK\n",f->depth_buffer[k]);program_release(p);framebuffer_release(f);}
