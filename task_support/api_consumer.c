#include "mytinygl.h"
#include "irregular_shadow.h"
int main(void){
 GLState *ctx=gl_create_context(16,16);if(!ctx)return 50;gl_make_current(ctx);
 glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
 glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDepthMask(GL_TRUE);
 glShadeModel(GL_PHONG);glEnable(GL_LIGHTING);glEnable(GL_LIGHT0);glEnable(GL_NORMALIZE);
 GLfloat emission[]={.5f,0,0,1}, radiance[]={.1f,.1f,.1f,1}, position[]={0,0,1,0};
 glMaterialfv(GL_FRONT_AND_BACK,GL_EMISSION,emission);glLightfv(GL_LIGHT0,GL_DIFFUSE,radiance);glLightfv(GL_LIGHT0,GL_POSITION,position);
 const mtgl_shadow_vertex v[]={{{-.8f,-.8f,0},{0,0,1},{0,0}},{{.8f,-.8f,0},{0,0,1},{1,0}},{{0,.8f,0},{0,0,1},{.5f,1}}};
 const GLfloat light[]={1,0,0,0,0,1,0,0,0,0,-1,0,0,0,0,1};mtgl_shadow_stats stats={0};
 int ok=mtgl_render_irregular_shadow(ctx,v,1,light,GL_LIGHT0,0,&stats);
 color_t c=color_from_rgba32(ctx->framebuffer.color[8*16+8]);
 if(!ok||stats.camera_samples==0||stats.hierarchy_nodes==0||stats.shadowed_samples!=0||c.r<c.g+.2f||glGetError()!=GL_NO_ERROR)return 51;
 gl_make_current(NULL);gl_destroy_context(ctx);return 0;
}
