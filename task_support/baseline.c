#include <stdio.h>
#include <string.h>
#include "renderer/core/graphics.h"
typedef struct { vec4_t clip; float u; } Attr;
typedef struct { float u; } Vary;
typedef struct { vec4_t color; int masked; } Uniform;
static unsigned vertex_calls,fragment_calls,discard_calls;
static vec4_t vertex(void *a,void *v,void *u) {
 (void)u;++vertex_calls;((Vary*)v)->u=((Attr*)a)->u;return ((Attr*)a)->clip;
}
static vec4_t fragment(void *v,void *u,int *discard,int backface) {
 Uniform *s=u;(void)backface;++fragment_calls;
 if(s->masked&&((Vary*)v)->u>0.5f){*discard=1;++discard_calls;}
 return s->color;
}
static void draw(framebuffer_t *f,program_t *p,float z,int masked,vec4_t color){
 const float x[3]={-1.5f,1.5f,0},y[3]={-1, -1,1.5f},w[3]={1,2,0.75f};
 const float uv[3]={0,1,0.5f};int i;
 Uniform *u=program_get_uniforms(p);u->color=color;u->masked=masked;
 for(i=0;i<3;i++) {Attr *a=program_get_attribs(p,i);a->clip=vec4_new(x[i]*w[i],y[i]*w[i],z*w[i],w[i]);a->u=uv[i];}
 graphics_draw_triangle(f,p);
}
int main(void){
 framebuffer_t *f=framebuffer_create(16,12);
 program_t *p=program_create(vertex,fragment,sizeof(Attr),sizeof(Vary),sizeof(Uniform),1,0);
 unsigned red=0,blue=0;int i;
 draw(f,p,0.4f,0,vec4_new(0,0,1,1));draw(f,p,-0.4f,1,vec4_new(1,0,0,1));
 for(i=0;i<16*12;i++){
  if(f->color_buffer[i*4]==255){if(f->depth_buffer[i]<0.2999f||f->depth_buffer[i]>0.3001f)return 2;++red;}
  if(f->color_buffer[i*4+2]==255){if(f->depth_buffer[i]<0.6999f||f->depth_buffer[i]>0.7001f)return 3;++blue;}
 }
 printf("BASELINE clipped_masked red=%u blue=%u vertex_calls=%u fragment_calls=%u discards=%u\n",red,blue,vertex_calls,fragment_calls,discard_calls);
 program_release(p);framebuffer_release(f);
 return red>10&&blue>10&&vertex_calls==6&&discard_calls>0?0:4;
}
