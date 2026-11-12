/* Local starting-point witness and legacy controls. No clip implementation. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "nanosvg.h"
#include "nanosvgrast.h"
#define NEED(x) do { if (!(x)) { fprintf(stderr,"baseline:%d: %s\n",__LINE__,#x); exit(1); } } while(0)
static NSVGimage* parse(const char* s) {
    char* p = (char*)malloc(strlen(s)+1); NSVGimage* im;
    NEED(p); strcpy(p,s); im=nsvgParse(p,"px",96); free(p); NEED(im); return im;
}
static void draw(NSVGrasterizer* r, NSVGimage* im, unsigned char* p) {
    memset(p,0xa5,16*80); nsvgRasterize(r,im,0,0,1,p,16,16,80);
    for (int y=0;y<16;y++) for(int x=64;x<80;x++) NEED(p[y*80+x]==0xa5);
}
static unsigned alpha(unsigned char* p,int x,int y) { return p[y*80+x*4+3]; }
static void legacy(NSVGrasterizer* r) {
    unsigned char p[16*80];
    NSVGimage* im=parse("<svg width='16' height='16'><rect x='2' y='2' width='12' height='12' fill='red'/></svg>");
    draw(r,im,p); NEED(alpha(p,5,5)==255); NEED(p[5*80+5*4]==255); NEED(alpha(p,0,0)==0); nsvgDelete(im);
    im=parse("<svg width='16' height='16'><defs><linearGradient id='g'><stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient></defs><rect x='2' y='2' width='12' height='12' fill='url(#g)' stroke='green' stroke-width='2'/></svg>");
    draw(r,im,p); NEED(alpha(p,5,5)==255); NEED(p[5*80+3*4]>p[5*80+12*4]); NEED(p[5*80+3*4+2]<p[5*80+12*4+2]); NEED(p[5*80+1*4+1]>0); nsvgDelete(im);
    const char* files[]={"example/nano.svg","example/23.svg","example/drawing.svg"};
    for(size_t i=0;i<sizeof(files)/sizeof(files[0]);i++) {
        im=nsvgParseFromFile(files[i],"px",96); NEED(im && im->width>0 && im->height>0);
        size_t shapes=0; for(NSVGshape* s=im->shapes;s;s=s->next) shapes++;
        NEED(shapes); unsigned char out[128*128*4];
        float scale=120.0f/(im->width>im->height?im->width:im->height);
        nsvgRasterize(r,im,4,4,scale,out,128,128,128*4);
        uint32_t hash=2166136261u; size_t painted=0;
        for(size_t k=0;k<sizeof(out);k++) hash=(hash^out[k])*16777619u;
        for(size_t k=3;k<sizeof(out);k+=4) if(out[k]) painted++;
        NEED(painted); printf("LEGACY_ASSET=%s shapes=%zu painted=%zu hash=%08x\n",files[i],shapes,painted,hash);
        nsvgDelete(im);
    }
    puts("LEGACY_CONTROLS_OK");
}
int main(int argc,char** argv) {
    NSVGrasterizer* r=nsvgCreateRasterizer(); NEED(r); legacy(r);
    if(argc==2 && strcmp(argv[1],"--legacy")==0) { nsvgDeleteRasterizer(r); return 0; }
    NEED(argc==1);
    NSVGimage* im=parse("<svg width='16' height='16'><defs><clipPath id='c'><rect x='2' y='2' width='5' height='12'/></clipPath></defs><rect x='2' y='2' width='12' height='12' fill='red' clip-path='url(#c)'/></svg>");
    unsigned char p[16*80]; draw(r,im,p);
    size_t shapes=0; for(NSVGshape* s=im->shapes;s;s=s->next) shapes++;
    NEED(shapes==1); NEED(alpha(p,4,6)==255);
    printf("CLIP_OUTSIDE_ALPHA=%u EXPECTED_AFTER_CHANGE=0\n",alpha(p,11,6));
    NEED(alpha(p,11,6)==255); puts("CLIP_IGNORED_WITNESS_OK");
    nsvgDelete(im); nsvgDeleteRasterizer(r); return 0;
}
