#define PORTABLEGL_IMPLEMENTATION
#include <portablegl.h>
#include <stdio.h>
static int vs_calls,fs_calls;
static void vs(float* out,vec4* a,Shader_Builtins* b,void* u){(void)u;++vs_calls;b->gl_Position=a[0];out[0]=a[0].x+(float)b->gl_InstanceID;}
static void fs(float* in,Shader_Builtins* b,void* u){(void)in;(void)u;++fs_calls;b->gl_FragColor=make_v4(1,0,0,1);}
int main(void){
 glContext ctx;pix_t* pixels=NULL;GLuint vbo,ebo;GLenum interp=PGL_SMOOTH;
 float verts[]={-.75f,-.75f,0,1,.75f,-.75f,0,1,0,.75f,0,1};GLushort idx[]={0,1,2};
 if(!init_glContext(&ctx,&pixels,16,16))return 1;
 glUseProgram(pglCreateProgram(vs,fs,1,&interp,GL_FALSE));
 glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,sizeof verts,verts,GL_STATIC_DRAW);
 glEnableVertexAttribArray(0);glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,0,0);
 glGenBuffers(1,&ebo);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof idx,idx,GL_STATIC_DRAW);
 glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
 glDrawElementsInstanced(GL_TRIANGLES,3,GL_UNSIGNED_SHORT,0,2);
 int different=0;for(int i=0;i<256;++i)if(pixels[i]!=pixels[0])++different;
 printf("production indexed instances: vs=%d fs=%d colored=%d error=%u\n",vs_calls,fs_calls,different,(unsigned)glGetError());
 int ok=vs_calls==6&&fs_calls>0&&different>0;free_glContext(&ctx);return ok?0:2;
}
