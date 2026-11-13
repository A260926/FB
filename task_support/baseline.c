#include "nanovg.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static int fills,strokes,tris,updates,textures;
static int create(void*p){(void)p;return 1;}
static int tex(void*p,int t,int w,int h,int f,const unsigned char*d){(void)p;(void)t;(void)f;(void)d;assert(w>0&&h>0);return ++textures;}
static int del(void*p,int i){(void)p;(void)i;return 1;}
static int update(void*p,int i,int x,int y,int w,int h,const unsigned char*d){(void)p;(void)i;(void)x;(void)y;assert(w>0&&h>0&&d);++updates;return 1;}
static int size(void*p,int i,int*w,int*h){(void)p;(void)i;*w=*h=512;return 1;}
static void viewport(void*p,float w,float h,float r){(void)p;assert(w==64&&h==32&&r==1);}
static void noop(void*p){(void)p;}
static void fill(void*p,NVGpaint*a,NVGcompositeOperationState o,NVGscissor*s,float f,const float*b,const NVGpath*v,int n){(void)p;(void)a;(void)o;(void)s;(void)f;(void)b;assert(n==2&&v[0].nfill>=4&&v[1].nfill>=4);++fills;}
static void stroke(void*p,NVGpaint*a,NVGcompositeOperationState o,NVGscissor*s,float f,float w,const NVGpath*v,int n){(void)p;(void)a;(void)o;(void)s;(void)f;assert(w==3&&n==1&&v[0].nstroke>4);++strokes;}
static void triangle(void*p,NVGpaint*a,NVGcompositeOperationState o,NVGscissor*s,const NVGvertex*v,int n,float f){(void)p;(void)o;(void)s;(void)f;assert(a->image>0&&v&&n==12);tris+=n;}
int main(void){
 NVGparams p;memset(&p,0,sizeof(p));p.renderCreate=create;p.renderCreateTexture=tex;p.renderDeleteTexture=del;p.renderUpdateTexture=update;p.renderGetTextureSize=size;p.renderViewport=viewport;p.renderCancel=noop;p.renderFlush=noop;p.renderFill=fill;p.renderStroke=stroke;p.renderTriangles=triangle;p.renderDelete=noop;
 NVGcontext*c=nvgCreateInternal(&p);assert(c);nvgBeginFrame(c,64,32,1);
 nvgBeginPath(c);nvgRect(c,1,1,20,20);nvgRect(c,4,4,5,5);nvgPathWinding(c,NVG_HOLE);nvgFill(c);
 nvgBeginPath(c);nvgMoveTo(c,25,2);nvgLineTo(c,35,20);nvgLineTo(c,45,3);nvgStrokeWidth(c,3);nvgStroke(c);
 int font=nvgCreateFont(c,"baseline","example/Roboto-Regular.ttf");assert(font>=0);nvgFontFaceId(c,font);nvgFontSize(c,12);nvgText(c,2,29,"AV",NULL);nvgEndFrame(c);
 assert(fills==1&&strokes==1&&tris==12&&updates>0);printf("NANOVG_CALLBACKS_OK fill=%d stroke=%d text_vertices=%d updates=%d\n",fills,strokes,tris,updates);nvgDeleteInternal(c);return 0;
}
