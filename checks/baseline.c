#include <stdio.h>
#define MINI3D_HEADLESS 1
#include "../mini3d.c"
static vertex_t v(float x,float y,float z,float r,float g,float b){vertex_t a;memset(&a,0,sizeof(a));a.pos=(point_t){x,y,z,1};a.color=(color_t){r,g,b};return a;}
static size_t count(device_t*d){size_t n=0;for(int y=0;y<d->height;y++)for(int x=0;x<d->width;x++)if(d->framebuffer[y][x])n++;return n;}
int main(void){device_t d;device_init(&d,64,64,NULL);d.background=0;d.render_state=RENDER_STATE_COLOR;device_clear(&d,0);
 vertex_t a=v(-1,-1,3,1,0,0),b=v(1,-1,3,1,0,0),c=v(0,1,3,1,0,0);device_draw_primitive(&d,&a,&b,&c);
 a=v(-2,-2,5,0,0,1);b=v(2,-2,5,0,0,1);c=v(0,2,5,0,0,1);device_draw_primitive(&d,&a,&b,&c);
 size_t n=count(&d);unsigned center=d.framebuffer[32][32];float depth=d.zbuffer[32][32];int ok=n>100&&(center&0xff0000)>0xc00000&&(center&255)<4&&depth>.32f&&depth<.34f;
 device_clear(&d,0);a=v(-.2f,-.2f,.5f,1,0,0);b=v(1,-1,3,1,0,0);c=v(0,1,3,1,0,0);device_draw_primitive(&d,&a,&b,&c);
 printf("BASELINE normal=%zu center=%06x rhw=%.6f crossclip=%zu\n",n,center,depth,count(&d));
 /* crossclip is an observation only, never a required old defect in target mode. */
 device_destroy(&d);return ok?0:3;}
