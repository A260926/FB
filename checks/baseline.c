/* Fixture, MIT. Runs existing render and feedback without changing the library. */
#include <GL/gl.h>
#include <zbuffer.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
int main(int argc,char **argv){ZBuffer*z=ZB_open(32,32,ZB_MODE_RGBA,0);assert(z);glInit(z);glViewport(0,0,32,32);glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glColor3f(1,0,0);glBegin(GL_TRIANGLES);glVertex3f(-.8,-.8,0);glVertex3f(.8,-.8,0);glVertex3f(0,.8,0);glEnd();int n=0;for(int i=0;i<32*32;++i)n+=((z->pbuf[i]&0xffffff)!=0);assert(n>100);printf("TINYGL_RENDER pixels=%d\n",n);
float b[64];for(int i=0;i<64;++i)b[i]=-777;glFeedbackBuffer(64,GL_3D,b);glRenderMode(GL_FEEDBACK);glPassThrough(7.25f);glBegin(GL_POINTS);glVertex3f(.125,.25,0);glEnd();int count=glRenderMode(GL_RENDER);printf("FEEDBACK count=%d token0=%.9g payload=%.2f token2=%.9g pointX=%.2f\n",count,b[0],b[1],b[2],b[3]);assert(b[63]==-777);int bad=(count!=6||b[0]!=(float)GL_PASS_THROUGH_TOKEN||b[1]!=7.25f||b[2]!=(float)GL_POINT_TOKEN||b[3]==-777);glClose();ZB_close(z);if(argc>1&&bad){puts("FEEDBACK_CONTRACT_FAILURE numeric_tokens_and_point_payload");return 33;}puts("TINYGL_BASELINE_OK");return 0;}
