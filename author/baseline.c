#define PORTABLEGL_IMPLEMENTATION
#include "portablegl.h"
#include <assert.h>
#include <stdio.h>
static void vs(float* o, vec4* a, Shader_Builtins* b, void* u) {
 (void)o;(void)u;b->gl_Position=a[0];
}
static void fs(float* i, Shader_Builtins* b, void* u) {
 (void)i;b->discard=*(int*)u;b->gl_FragColor=make_v4(1,0,0,1);
}
int main(void) {
 glContext ctx;pix_t* pixels=NULL;int discard=0;
 assert(init_glContext(&ctx,&pixels,16,16));
 float p[]={-.75f,-.75f,0, .75f,-.75f,0, 0,.75f,0};
 GLuint buf;glGenBuffers(1,&buf);glBindBuffer(GL_ARRAY_BUFFER,buf);
 glBufferData(GL_ARRAY_BUFFER,sizeof p,p,GL_STATIC_DRAW);
 glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,0,0);glEnableVertexAttribArray(0);
 glUseProgram(pglCreateProgram(vs,fs,0,NULL,GL_TRUE));pglSetUniform(&discard);
 glClearColor(0,0,0,1);glClearDepth(1);glEnable(GL_DEPTH_TEST);
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
 pix_t clear=pixels[8*16+8];discard=1;glDrawArrays(GL_TRIANGLES,0,3);
 assert(pixels[8*16+8]==clear);
 discard=0;glDrawArrays(GL_TRIANGLES,0,3);assert(pixels[8*16+8]!=clear);
 int covered=0;for(int i=0;i<256;i++)covered+=pixels[i]!=clear;
 assert(covered>20 && covered<200);printf("PGL_BASELINE_OK covered=%d discard-preserves-depth\n",covered);
 assert(glGetError()==GL_NO_ERROR);free_glContext(&ctx);return 0;
}
