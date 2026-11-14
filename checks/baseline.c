#include "GL/gl.h"
#include "zbuffer.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
 const int d=TGL_FEATURE_TEXTURE_DIM;
 ZBuffer *zb=ZB_open(d,d,ZB_MODE_RGBA,NULL);
 if(!zb)return 1;
 glInit(zb);glViewport(0,0,d,d);
 glMatrixMode(GL_PROJECTION);glLoadIdentity();
 glMatrixMode(GL_MODELVIEW);glLoadIdentity();
 glDisable(GL_LIGHTING);glDisable(GL_TEXTURE_2D);glEnable(GL_DEPTH_TEST);
 glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
 glColor3f(1,0,0);glBegin(GL_TRIANGLES);
 glVertex3f(-0.8f,-0.8f,0);glVertex3f(0.8f,-0.8f,0);glVertex3f(0,0.8f,0);glEnd();
 size_t center=(size_t)(d/2)*d+d/2;
 unsigned color=zb->pbuf[center]&0xffffffu;unsigned depth=zb->zbuf[center];
 if(((color>>16)<250u || (color&0xffffu)!=0u)||depth==0){fprintf(stderr,"render color=%06x depth=%u\n",color,depth);return 2;}
 GLuint tex;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
 glReadBuffer(GL_FRONT);glCopyTexImage2D(GL_TEXTURE_2D,0,GL_RGB,0,d,d,d,0);
 GLint w=0,h=0;PIXEL* image=glGetTexturePixmap(tex,0,&w,&h);
 if(!image||w!=d||h!=d||(image[center]&0xffffffu)!=color)return 3;
 glClearColor(0,1,0,0);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
 if((image[center]&0xffffffu)!=color)return 4;
 printf("BASELINE_RGB_SNAPSHOT_OK color=%06x z=%u size=%d\n",color,depth,d);
 glDeleteTextures(1,&tex);glClose();ZB_close(zb);return 0;
}
