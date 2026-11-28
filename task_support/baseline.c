#include "mytinygl.h"
#include <stdio.h>
#include <math.h>
int main(void){
 GLState *ctx=gl_create_context(32,32);if(!ctx)return 1;gl_make_current(ctx);
 glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
 glEnable(GL_DEPTH_TEST);glEnable(GL_LIGHTING);glEnable(GL_NORMALIZE);glShadeModel(GL_PHONG);
 GLfloat emission[]={.5f,.05f,0,1};glMaterialfv(GL_FRONT_AND_BACK,GL_EMISSION,emission);
 glBegin(GL_TRIANGLES);glNormal3f(0,0,1);
 glTexCoord2f(0,0);glVertex3f(-.8f,-.8f,0);
 glTexCoord2f(1,0);glVertex3f(.8f,-.8f,0);
 glTexCoord2f(.5f,1);glVertex3f(0,.8f,0);glEnd();
 unsigned covered=0;for(int i=0;i<32*32;++i){if(!isfinite(ctx->framebuffer.depth[i]))return 2;covered+=ctx->framebuffer.depth[i]<1;}
 color_t c=color_from_rgba32(ctx->framebuffer.color[16*32+16]);
 if(covered<100||c.r<c.g+.2f||glGetError()!=GL_NO_ERROR)return 3;
 printf("BASELINE_PHONG covered=%u center=%.6f,%.6f,%.6f depth=%.6f\n",covered,c.r,c.g,c.b,ctx->framebuffer.depth[16*32+16]);
 gl_make_current(NULL);gl_destroy_context(ctx);return 0;
}
