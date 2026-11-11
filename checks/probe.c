#include "tigr.h"
#include <assert.h>
#include <stdio.h>
int main(void){Tigr *a=tigrBitmap(2,4),*b=tigrBitmap(2,4);for(int y=0;y<4;y++)for(int x=0;x<2;x++)a->pix[y*2+x]=tigrRGB((y+1)*10,0,0);tigrBlit(b,a,0,1,0,0,2,3);assert(b->pix[2].r==10&&b->pix[4].r==20&&b->pix[6].r==30);puts("PASS distinct source rows=0,10,20,30");tigrBlit(a,a,0,1,0,0,2,3);printf("OBSERVE self source rows=%u,%u,%u,%u; snapshot target=10,10,20,30\n",a->pix[0].r,a->pix[2].r,a->pix[4].r,a->pix[6].r);tigrFree(a);tigrFree(b);return 0;}
