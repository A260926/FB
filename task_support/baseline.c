#include <GL/gl.h>
#include <zbuffer.h>
#include <stdio.h>
#include <stdlib.h>

static void triangle(float z) {
    glBegin(GL_TRIANGLES);
    glTexCoord2f(.5f,.5f); glVertex3f(-.8f,-.8f,z);
    glTexCoord2f(.5f,.5f); glVertex3f(1.3f,-.8f,z);
    glTexCoord2f(.5f,.5f); glVertex3f(0,.8f,z);
    glEnd();
}
int main(void) {
    ZBuffer *zb=ZB_open(64,64,ZB_MODE_RGBA,NULL);
    GLuint texture;
    unsigned char *pixels;
    PIXEL center;
    unsigned covered=0;
    int i,d=TGL_FEATURE_TEXTURE_DIM;
    if(!zb) return 1;
    glInit(zb);
    glViewport(0,0,64,64);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glDisable(GL_TEXTURE_2D);
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glColor3f(1,0,0); triangle(-.4f);
    glColor3f(0,1,0); triangle(.5f);
    center=zb->pbuf[32*64+32];
    if(GET_RED(center)<200 || GET_GREEN(center)>10) return 2;
    for(i=0;i<64*64;++i) if(zb->zbuf[i]) ++covered;
    if(covered<500) return 3;
    pixels=malloc((size_t)d*d*3);
    if(!pixels) return 4;
    for(i=0;i<d*d;++i) { pixels[3*i]=32; pixels[3*i+1]=64; pixels[3*i+2]=192; }
    glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,3,d,d,0,GL_RGB,GL_UNSIGNED_BYTE,pixels);
    free(pixels);
    glEnable(GL_TEXTURE_2D); glColor3f(1,1,1); triangle(-.7f);
    center=zb->pbuf[32*64+32];
    if(GET_BLUE(center)<150 || GET_GREEN(center)<40 || GET_RED(center)>60) return 5;
    if(glGetError()!=GL_NO_ERROR) return 6;
    printf("PRODUCTION_TINYGL clipped=1 depth_occlusion=1 texture=1 covered=%u center=%u,%u,%u\n",
           covered,GET_RED(center),GET_GREEN(center),GET_BLUE(center));
    glDeleteTextures(1,&texture); glClose(); ZB_close(zb);
    return 0;
}
