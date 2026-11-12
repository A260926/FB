#include "allheaders.h"
#include <stdio.h>
#include <assert.h>
int main(void){PIX *s=pixCreate(9,5,8),*m=pixCreate(9,5,8);assert(s&&m);
 pixSetAllArbitrary(m,200);pixSetPixel(s,4,2,100);assert(!pixSeedfillGray(s,m,4));
 for(int y=0;y<5;y++)for(int x=0;x<9;x++){l_uint32 v;pixGetPixel(s,x,y,&v);assert(v==100);}
 PIX *t=pixCopy(NULL,s);assert(!pixSeedfillGraySimple(t,m,8));int equal=0;pixEqual(s,t,&equal);assert(equal);
 puts("LEPT_GRAY_BASELINE_OK pixels=45 value=100");pixDestroy(&s);pixDestroy(&t);pixDestroy(&m);return 0;}
