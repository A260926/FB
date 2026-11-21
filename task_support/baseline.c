#include "src/mytinygl.h"
#include <stdio.h>
#include <math.h>
int main(void){
 GLState *c=gl_create_context(32,32); if(!c)return 1; gl_make_current(c);
 glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
 glEnable(GL_DEPTH_TEST); glColor3f(1,0,0); glBegin(GL_TRIANGLES);
 glVertex3f(-.8f,-.8f,0);glVertex3f(.8f,-.8f,0);glVertex3f(0,.8f,0);glEnd();
 size_t n=0;for(int i=0;i<1024;i++)if(c->framebuffer.depth[i]<.9f)n++;
 if(n<100||fabsf(c->framebuffer.depth[16*32+16]-.5f)>1e-5f)return 2;
 glColor3f(0,1,0);glBegin(GL_LINES);glVertex3f(-2,0,-.5f);glVertex3f(2,0,-.5f);glEnd();
 size_t near=0;for(int i=0;i<1024;i++)if(c->framebuffer.depth[i]<.4f)near++;
 if(near<20||glGetError()!=GL_NO_ERROR)return 3;
 printf("BASELINE render_triangle covered=%zu clipped_line_near=%zu center_depth=%.2f\n",n,near,c->framebuffer.depth[16*32+16]);
 gl_make_current(NULL);gl_destroy_context(c);return 0;
}
