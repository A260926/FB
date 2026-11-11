#include "schrift.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *render(SFT *s,SFT_Glyph g,SFT_GMetrics *m){assert(sft_gmetrics(s,g,m)==0);assert(m->minWidth>0&&m->minHeight>0);unsigned char *p=calloc(m->minWidth*m->minHeight,1);assert(p);assert(sft_render(s,g,(SFT_Image){p,m->minWidth,m->minHeight})==0);return p;}
int main(void){const char *path="resources/FiraGO-Regular.ttf";SFT_Font *f=sft_loadfile(path);assert(f);SFT s={f,24,24,.25,.5,SFT_DOWNWARD_Y};SFT_Glyph g;assert(!sft_lookup(&s,'A',&g));SFT_GMetrics m;unsigned char *p=render(&s,g,&m);int n=0;for(int i=0;i<m.minWidth*m.minHeight;i++)n+=p[i]!=0;assert(n>0);FILE *fp=fopen(path,"rb");assert(fp);fseek(fp,0,SEEK_END);long len=ftell(fp);rewind(fp);void *bytes=malloc(len);assert(fread(bytes,1,len,fp)==(size_t)len);fclose(fp);SFT_Font *fm=sft_loadmem(bytes,len);assert(fm);s.font=fm;SFT_GMetrics mm;unsigned char *q=render(&s,g,&mm);assert(m.minWidth==mm.minWidth&&m.minHeight==mm.minHeight&&!memcmp(p,q,m.minWidth*m.minHeight));printf("PASS A file/mem %dx%d ink=%d\n",m.minWidth,m.minHeight,n);free(p);free(q);sft_freefont(fm);free(bytes);sft_freefont(f);return 0;}
