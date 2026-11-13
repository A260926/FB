#include <TGL/gl.h>
#include "zbuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void triangle(void) {
 glBegin(GL_TRIANGLES); glColor3f(1,1,1);
 glVertex3f(-.75f,-.75f,0);glVertex3f(.75f,-.75f,0);glVertex3f(0,.75f,0);glEnd();
}
int main(void) {
 ZBuffer *z=ZB_open(64,64,ZB_MODE_RGBA,NULL); if(!z)return 1;
 glInit(z);glViewport(0,0,64,64);glClearColor(0,0,0,0);
 glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
 glEnable(GL_DEPTH_TEST);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);triangle();
 size_t n=64*64*sizeof(PIXEL),dz=64*64*sizeof(*z->zbuf);void *image=malloc(n),*depth=malloc(dz);
 if(!image||!depth)return 2;memcpy(image,z->pbuf,n);memcpy(depth,z->zbuf,dz);
 unsigned count=0,left=0,right=0,written=0;
 for(int i=0;i<4096;i++){if(z->pbuf[i]){count++;if(i%64<32)left++;else right++;}if(z->zbuf[i])written++;}
 if(count<100||!left||!right||!written)return 3;
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);GLuint list=glGenLists(1);
 glNewList(list,GL_COMPILE);triangle();glEndList();glCallList(list);
 if(memcmp(image,z->pbuf,n)||memcmp(depth,z->zbuf,dz))return 4;
 printf("BASELINE_OK pixels=%u left=%u right=%u depth=%u list_equal=1\n",count,left,right,written);
 glDeleteLists(list,1);free(image);free(depth);glClose();ZB_close(z);return 0;
}
