#define PORTABLEGL_IMPLEMENTATION
#include "portablegl.h"
#include <stdio.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"BASELINE_FAIL line=%d\n",__LINE__); return 1; } } while(0)
static unsigned vs_calls,fs_calls,instances_seen;
static int bad_instance;
static void vs(float* out,vec4* attr,Shader_Builtins* b,void* u){
 (void)u; ++vs_calls; b->gl_Position=attr[0]; out[0]=attr[1].x;
 if(b->gl_InstanceID>=0 && b->gl_InstanceID<2)instances_seen|=1u<<b->gl_InstanceID;
 else bad_instance=1;
}
static void fs(float* in,Shader_Builtins* b,void* u){
 (void)u; ++fs_calls; SET_V4(b->gl_FragColor,in[0],0,0,1);
}
static void clear_frame(void){glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);}
int main(void){
 glContext ctx;pix_t* pixels=NULL;REQUIRE(init_glContext(&ctx,&pixels,16,16));
 float pos[]={-.75f,-.75f,0,1, .75f,-.75f,0,1, -.75f,.75f,0,1, .75f,.75f,0,1};
 float tags[]={.1f,.3f,.6f,.9f};GLuint bufs[3];glGenBuffers(3,bufs);
 glBindBuffer(GL_ARRAY_BUFFER,bufs[0]);glBufferData(GL_ARRAY_BUFFER,sizeof(pos),pos,GL_STATIC_DRAW);
 glEnableVertexAttribArray(0);glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,0,0);
 glBindBuffer(GL_ARRAY_BUFFER,bufs[1]);glBufferData(GL_ARRAY_BUFFER,sizeof(tags),tags,GL_STATIC_DRAW);
 glEnableVertexAttribArray(1);glVertexAttribPointer(1,1,GL_FLOAT,GL_FALSE,0,0);
 GLenum interp[]={PGL_FLAT};GLuint program=pglCreateProgram(vs,fs,1,interp,GL_FALSE);glUseProgram(program);
 clear_frame();glDrawArrays(GL_TRIANGLE_STRIP,0,4);
 REQUIRE(vs_calls==4 && fs_calls>0);pix_t snapshot[256];memcpy(snapshot,pixels,sizeof(snapshot));
 unsigned strip_fragments=fs_calls;unsigned red_pixels=0;for(int i=0;i<256;++i)red_pixels+=PIXEL_TO_COLOR(pixels[i]).r>0;
 REQUIRE(red_pixels>0 && red_pixels<256);
 GLushort idx[]={0,1,2,2,1,3};glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,bufs[2]);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(idx),idx,GL_STATIC_DRAW);
 clear_frame();vs_calls=fs_calls=0;glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_SHORT,0);
 REQUIRE(vs_calls==6 && fs_calls==strip_fragments && !memcmp(snapshot,pixels,sizeof(snapshot)));
 printf("BASELINE_ASSEMBLY_EQUIVALENT strip_vs=4 indexed_vs=6 fragments=%u colored=%u\n",strip_fragments,red_pixels);
 clear_frame();vs_calls=fs_calls=0;glEnable(GL_CULL_FACE);glCullFace(GL_FRONT_AND_BACK);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
 REQUIRE(vs_calls==4 && fs_calls==0);puts("BASELINE_CULL_AFTER_VERTEX vs=4 fs=0");glDisable(GL_CULL_FACE);
 clear_frame();vs_calls=fs_calls=instances_seen=0;glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP,0,4,2,7);
 REQUIRE(vs_calls==8 && instances_seen==3 && !bad_instance && fs_calls==2*strip_fragments);
 puts("BASELINE_INSTANCED_VERTEX_OK instances=2 vs=8");
 REQUIRE(glGetError()==GL_NO_ERROR);free_glContext(&ctx);return 0;
}
