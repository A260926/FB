#include <stdio.h>
#include <math.h>
#define MINI3D_HEADLESS
#include "../mini3d.c"
static vertex_t vert(float x,float y,float z) {
 vertex_t v; memset(&v,0,sizeof(v));v.pos=(point_t){x,y,z,1};v.color=(color_t){1,0,0}; return v;
}
int main(void) {
 device_t d;device_init(&d,32,32,NULL);d.background=0;device_clear(&d,0);
 matrix_set_identity(&d.transform.transform);
 vertex_t a=vert(-.75f,-.75f,.5f),b=vert(.75f,-.75f,.5f),c=vert(0,.75f,.5f);
 d.render_state=RENDER_STATE_COLOR;device_draw_primitive(&d,&a,&b,&c);
 unsigned center=d.framebuffer[16][16];float depth=d.zbuffer[16][16];int covered=0;
 for(int y=0;y<32;y++)for(int x=0;x<32;x++)covered+=d.framebuffer[y][x]!=0;
 if(center!=0xff0000||fabsf(depth-1)>1e-6f||covered<200||covered>330)return 1;
 printf("COLOR center=%06x depth=%.3f covered=%d\n",center,depth,covered);
 IUINT32 tex[4]={0x224466,0x224466,0x224466,0x224466};device_set_texture(&d,tex,8,2,2);
 d.render_state=RENDER_STATE_COLOR|RENDER_STATE_TEXTURE;device_draw_primitive(&d,&a,&b,&c);
 if(d.framebuffer[16][16]!=0x224466)return 2;
 printf("TEXTURE priority=%06x\n",d.framebuffer[16][16]);
 device_destroy(&d);puts("BASELINE_COMPLETE");return 0;
}
