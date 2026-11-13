/* Offline starting-point observations and existing-path controls. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/nanosvg.h"
#include "../src/nanosvgrast.h"
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"baseline:%d: %s\n",__LINE__,#x);exit(1);} } while(0)
static NSVGimage* parse(const char* text) {
 size_t n=strlen(text); char* p=(char*)malloc(n+1); NSVGimage* img;
 CHECK(p);memcpy(p,text,n+1);img=nsvgParse(p,"px",96);free(p);CHECK(img);return img;
}
static unsigned char pixels[64*64*4];
static void render(NSVGrasterizer* r,NSVGimage* im) {nsvgRasterize(r,im,0,0,1,pixels,64,64,64*4);}
static unsigned char* pixel(int x,int y){return pixels+(y*64+x)*4;}
static void legacy(NSVGrasterizer* r){
 NSVGimage* im=parse("<svg width='64' height='64'><rect x='4' y='4' width='12' height='12' fill='red'/><path d='M24 8L48 8' stroke='blue' stroke-width='4'/><path d='M4 24H24V44H4Z M8 28V40H20V28Z' fill='green' fill-rule='evenodd'/></svg>");
 render(r,im);CHECK(pixel(8,8)[0]==255 && pixel(8,8)[3]==255);CHECK(pixel(30,8)[2]==255 && pixel(30,8)[3]==255);CHECK(pixel(12,32)[3]==0 && pixel(5,32)[3]==255);
 CHECK(im->shapes && im->shapes->paths);{NSVGpath* cp=nsvgDuplicatePath(im->shapes->paths);CHECK(cp && cp->npts==im->shapes->paths->npts);free(cp->pts);free(cp);}nsvgDelete(im);
 puts("LEGACY_PRIMITIVES_OK");
 im=parse("<svg width='64' height='64'><defs><linearGradient id='g' x1='0%' x2='100%'><stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient></defs><rect x='0' y='0' width='64' height='16' fill='url(#g)'/></svg>");
 render(r,im);CHECK(pixel(4,8)[0]>200 && pixel(4,8)[2]<40);CHECK(pixel(60,8)[2]>200 && pixel(60,8)[0]<40);nsvgDelete(im);puts("LEGACY_GRADIENT_OK");
 im=nsvgParseFromFile("example/nano.svg","px",96);CHECK(im && im->shapes && im->width>0 && im->height>0);
 {size_t n=(size_t)im->width*(size_t)im->height*4; unsigned char* p=malloc(n);size_t i,count=0;CHECK(p && n<16000000);nsvgRasterize(r,im,0,0,1,p,(int)im->width,(int)im->height,(int)im->width*4);for(i=3;i<n;i+=4)count+=p[i]!=0;CHECK(count>100);printf("LEGACY_FILE_OK nonzero=%zu\n",count);free(p);}nsvgDelete(im);
}
int main(int argc,char** argv){
 NSVGrasterizer* r=nsvgCreateRasterizer();CHECK(r);legacy(r);
 if(argc==1){
 NSVGimage* im=parse("<svg width='64' height='64'><defs><marker id='arrow' markerWidth='10' markerHeight='10' refX='0' refY='0' markerUnits='userSpaceOnUse' orient='auto' overflow='visible'><path d='M0 -5L10 0L0 5Z' fill='red'/></marker></defs><path d='M8 16L32 16' fill='none' stroke='blue' stroke-width='2' marker-end='url(#arrow)'/></svg>");
 int count=0;NSVGshape* sh;render(r,im);for(sh=im->shapes;sh;sh=sh->next)count++;
 CHECK(pixel(20,16)[2]==255 && pixel(20,16)[3]==255);CHECK(pixel(36,16)[3]==0);CHECK(count==1);
 printf("MARKER_STARTPOINT body_alpha=%u marker_alpha=%u shapes=%d\n",pixel(20,16)[3],pixel(36,16)[3],count);nsvgDelete(im);
 im=parse("<svg width='64' height='64'><path d='M10 20A10 10 0 1 1 30 20' fill='none' stroke='black'/></svg>");CHECK(im->shapes && im->shapes->paths && im->shapes->paths->npts>4);printf("ARC_NORMALIZATION cubic_segments=%d source_arc_segments=1\n",(im->shapes->paths->npts-1)/3);nsvgDelete(im);
 }else CHECK(argc==2 && strcmp(argv[1],"--legacy")==0);
 nsvgDeleteRasterizer(r);puts("BASELINE_COMPLETE");return 0;
}
