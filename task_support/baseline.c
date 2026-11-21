#include <TGL/gl.h>
#include "zbuffer.h"
#include <stdio.h>
#include <stdlib.h>
static void need(int ok){if(!ok){fputs("baseline assertion failed\n",stderr);exit(1);}}
static void tri(float z){glBegin(GL_TRIANGLES);glVertex3f(-.9f,-.9f,z);glVertex3f(.9f,-.9f,z);glVertex3f(0,.9f,z);glEnd();}
static int count(ZBuffer*b){int n=0;for(int i=0;i<b->xsize*b->ysize;i++)if(b->pbuf[i]&0xffffff)n++;return n;}
int main(void){
 ZBuffer*b=ZB_open(64,64,ZB_MODE_RGBA,NULL);need(b!=NULL);glInit(b);glViewport(0,0,64,64);
 glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();
 glEnable(GL_DEPTH_TEST);glDisable(GL_LIGHTING);glDisable(GL_TEXTURE_2D);glDisable(GL_CULL_FACE);glShadeModel(GL_FLAT);
 glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
 glColor3f(1,0,0);tri(.2f);int center=32*64+32;need(GET_RED(b->pbuf[center])>240);
 glColor3f(0,0,1);tri(-.2f);need(GET_BLUE(b->pbuf[center])>240&&GET_RED(b->pbuf[center])<4);
 glColor3f(0,1,0);tri(.6f);need(GET_BLUE(b->pbuf[center])>240);need(b->zbuf[center]>0);
 printf("CPU_RASTER center=blue covered=%d depth=%u\n",count(b),(unsigned)b->zbuf[center]);
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);GLuint list=glGenLists(1);need(glIsList(list));
 glNewList(list,GL_COMPILE);glColor3f(1,.5f,0);glBegin(GL_TRIANGLES);glVertex3f(-2,-.6f,0);glVertex3f(.7f,-.6f,0);glVertex3f(.7f,.7f,0);glEnd();glEndList();
 need(count(b)==0);glCallList(list);int pixels=count(b);need(pixels>100&&pixels<4096);
 printf("CPU_LIST replay=ok clipped_pixels=%d\n",pixels);glDeleteList(list);glClose();ZB_close(b);return 0;
}
