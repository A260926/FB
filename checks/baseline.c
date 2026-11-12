#include "schrift.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
int main(void){
 SFT_Font *font=sft_loadfile("resources/FiraGO-Regular.ttf");assert(font);
 SFT s={font,24,24,0.25,0.5,SFT_DOWNWARD_Y};SFT_Glyph g;SFT_GMetrics m;
 assert(sft_lookup(&s,'A',&g)==0&&g);assert(sft_gmetrics(&s,g,&m)==0);
 unsigned char *pixels=calloc((size_t)m.minWidth,m.minHeight);assert(pixels);
 assert(sft_render(&s,g,(SFT_Image){pixels,m.minWidth,m.minHeight})==0);
 int nonzero=0;for(int i=0;i<m.minWidth*m.minHeight;i++)nonzero+=pixels[i]!=0;
 assert(m.minWidth==15&&m.minHeight==19&&nonzero==129);
 printf("SCHRIFT_TTF_BASELINE_OK size=%dx%d nonzero=%d\n",m.minWidth,m.minHeight,nonzero);
 free(pixels);sft_freefont(font);return 0;
}
