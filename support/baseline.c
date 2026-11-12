#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gif_lib.h"
struct bytes{unsigned char *p;size_t n,pos;};
static int wr(GifFileType*g,const GifByteType*p,int n){struct bytes*b=g->UserData;void*q=realloc(b->p,b->n+n);if(!q)return 0;b->p=q;memcpy(b->p+b->n,p,n);b->n+=n;return n;}
static int rd(GifFileType*g,GifByteType*p,int n){struct bytes*b=g->UserData;if((size_t)n>b->n-b->pos)return 0;memcpy(p,b->p+b->pos,n);b->pos+=n;return n;}
int main(void){
 struct bytes b={0};int err;GifColorType colors[4]={{0,0,0},{255,0,0},{0,255,0},{0,0,255}};
 ColorMapObject*m=GifMakeMapObject(4,colors);GifFileType*g=EGifOpen(&b,wr,&err);if(!m||!g)return 2;
 EGifSetGifVersion(g,true);if(EGifPutScreenDesc(g,16,16,2,0,m)==GIF_ERROR)return 3;
 GifByteType pixels[256],ext[4];
 for(int f=0;f<2;f++){
  GraphicsControlBlock ctl={DISPOSE_DO_NOT,false,7+f,0};EGifGCBToExtension(&ctl,ext);
  if(EGifPutExtension(g,GRAPHICS_EXT_FUNC_CODE,4,ext)==GIF_ERROR||EGifPutImageDesc(g,0,0,16,16,false,NULL)==GIF_ERROR)return 4;
  for(int i=0;i<256;i++)pixels[i]=1+(i+f)%3;
  if(EGifPutLine(g,pixels,256)==GIF_ERROR)return 5;
 }
 if(EGifCloseFile(g,&err)==GIF_ERROR)return 6;GifFreeMapObject(m);
 g=DGifOpen(&b,rd,&err);if(!g||DGifSlurp(g)==GIF_ERROR||g->ImageCount!=2)return 7;
 for(int f=0;f<2;f++){GraphicsControlBlock c;if(DGifSavedExtensionToGCB(g,f,&c)==GIF_ERROR||c.DelayTime!=7+f)return 8;
  for(int i=0;i<256;i++)if(g->SavedImages[f].RasterBits[i]!=1+(i+f)%3)return 9;
 }
 printf("GIF_PLAN_BASE frames=2 pixels=512 bytes=%zu\n",b.n);DGifCloseFile(g,&err);free(b.p);return 0;
}
