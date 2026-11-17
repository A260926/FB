#include "mytinygl.h"
#include <stdio.h>
#include <stdlib.h>
static void emit_vertex(float x,float y,const float *em){glMaterialfv(GL_FRONT_AND_BACK,GL_EMISSION,em);glVertex3f(x,y,0);}
static void draw(int back,GLenum mode,unsigned char p[4]){
 const float r[4]={1,0,0,1},g[4]={0,1,0,1},b[4]={0,0,1,1},zero[4]={0,0,0,1};
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glEnable(GL_LIGHTING);glLightModeli(GL_LIGHT_MODEL_TWO_SIDE,GL_TRUE);glLightModelfv(GL_LIGHT_MODEL_AMBIENT,zero);glShadeModel(mode);
 glBegin(GL_TRIANGLES);emit_vertex(-.8f,-.8f,r);if(back){emit_vertex(0,.8f,g);emit_vertex(.8f,-.8f,b);}else{emit_vertex(.8f,-.8f,g);emit_vertex(0,.8f,b);}glEnd();
 glReadPixels(32,24,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);
}
int main(void){GLState *c=gl_create_context(64,64);if(!c)return 2;gl_make_current(c);glViewport(0,0,64,64);unsigned char f[4],b[4],p[4];draw(0,GL_SMOOTH,f);draw(1,GL_SMOOTH,b);draw(0,GL_PHONG,p);printf("GOURAUD_FRONT %u %u %u\nGOURAUD_BACK %u %u %u\nPHONG_FRONT %u %u %u\n",f[0],f[1],f[2],b[0],b[1],b[2],p[0],p[1],p[2]);if(!(f[0]>20&&f[1]>20&&f[2]>20))return 3;puts("BASELINE_OK production lighting rendered");gl_make_current(NULL);gl_destroy_context(c);return 0;}
