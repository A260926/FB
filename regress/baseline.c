#include <assert.h>
#include <stdio.h>
#include <string.h>
#define NANOSVG_IMPLEMENTATION
#include "../src/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../src/nanosvgrast.h"
static NSVGimage* image(const char* focus) {
 char s[1024];
 snprintf(s,sizeof(s),"<svg width='64' height='40'><defs><radialGradient id='g' gradientUnits='userSpaceOnUse' cx='32' cy='20' r='16' %s><stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></radialGradient></defs><rect width='64' height='40' fill='url(#g)'/></svg>",focus);
 return nsvgParse(s,"px",96);
}
int main(void) {
 NSVGimage *a=image(""), *b=image("fx='24' fy='20'"), *c=image("fx='32' fy='20'");
 NSVGrasterizer *r=nsvgCreateRasterizer();
 unsigned char x[64*40*4],y[64*40*4],z[64*40*4];
 assert(a&&b&&c&&r&&a->shapes&&b->shapes&&c->shapes);
 assert(a->shapes->fill.type==NSVG_PAINT_RADIAL_GRADIENT);
 nsvgRasterize(r,a,0,0,1,x,64,40,64*4);
 nsvgRasterize(r,b,0,0,1,y,64,40,64*4);
 nsvgRasterize(r,c,0,0,1,z,64,40,64*4);
 assert(x[(20*64+32)*4]>240 && x[(20*64+32)*4+3]==255);
 /* undeclared focus coincides with the center */
 assert(memcmp(x,z,sizeof x)==0);
 /* eccentric focus shifts the gradient */
 assert(memcmp(x,y,sizeof x)!=0);
 assert(y[(20*64+24)*4]>240 && y[(20*64+24)*4+3]==255);
 printf("RADIAL_BASELINE_OK center-red=%u focus-images-equal=0\n",x[(20*64+32)*4]);
 nsvgDelete(a);nsvgDelete(b);nsvgDelete(c);nsvgDeleteRasterizer(r);
 return 0;
}
