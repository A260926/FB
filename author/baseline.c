#include "GL/gl.h"
#include "zbuffer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void triangle(void) {
 glBegin(GL_TRIANGLES);glColor3f(1,0,0);
 glVertex3f(-.75f,-.75f,0);glVertex3f(.75f,-.75f,0);glVertex3f(0,.75f,0);glEnd();
}
static int countz(ZBuffer* z) {
 int n=0;for(int i=0;i<z->xsize*z->ysize;i++)n+=z->zbuf[i]!=0;return n;
}
int main(void) {
 ZBuffer* z=ZB_open(32,32,ZB_MODE_RGBA,NULL);assert(z);glInit(z);
 glViewport(0,0,32,32);glMatrixMode(GL_PROJECTION);glLoadIdentity();
 glMatrixMode(GL_MODELVIEW);glLoadIdentity();glClearColor(0,0,0,0);
 glDepthMask(GL_TRUE);glEnable(GL_DEPTH_TEST);
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);triangle();int normal=countz(z);assert(normal>50);
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glDisable(GL_DEPTH_TEST);triangle();
 int disabled=countz(z);assert(disabled==normal);
 glEnable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glClear(GL_DEPTH_BUFFER_BIT);
 int masked_clear=countz(z);assert(masked_clear==0);
 glDepthMask(GL_TRUE);glNewList(1,GL_COMPILE);glDepthMask(GL_FALSE);glEndList();triangle();
 int compile_write=countz(z);assert(compile_write==0);
 printf("TINYGL_BASELINE_OK normal=%d disabled-wrote=%d masked-clear=%d compile-write=%d\n",normal,disabled,masked_clear,compile_write);
 glClose();ZB_close(z);return 0;
}
