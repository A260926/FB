#include "fixture.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
struct bits { unsigned char a[128]; unsigned n; };
static void put(struct bits *b,unsigned v,unsigned n) { unsigned i;for(i=0;i<n;i++,b->n++)if((v>>i)&1)b->a[b->n/8]|=(unsigned char)(1u<<(b->n%8)); }
static void le32(FILE *f,uint32_t v){unsigned i;for(i=0;i<4;i++)fputc((int)((v>>(8*i))&255),f);}
int smk_fixture_write(const char *path) {
 struct bits b={{0},0};FILE *f;unsigned i,j,n;unsigned a[4]={75,0,30,0},c[4]={16,0,224,0},sz[4];
 unsigned char pal[8]={2,0x86,63,0,0,0xff,0xf7,0};unsigned pa=0,pc=0;
 for(i=0;i<3;i++)put(&b,0,2);
 put(&b,1,1);
 put(&b,1,1);put(&b,0,1);put(&b,3,8);put(&b,0,1);
 put(&b,1,1);put(&b,0,1);put(&b,7,8);put(&b,0,1);
 for(i=0;i<3;i++)put(&b,65535,16);
 put(&b,0,2);n=(b.n+7)/8;
 for(i=0;i<4;i++){sz[i]=8+4+(a[i]?a[i]+4:0)+(c[i]?c[i]+4:0);sz[i]=(sz[i]+3)&~3u;}
 f=fopen(path,"wb");if(!f)return 1;
 fwrite("SMK4",1,4,f);le32(f,4);le32(f,4);le32(f,3);le32(f,(uint32_t)(int32_t)-3333);le32(f,1);
 for(i=0;i<7;i++)le32(f,i==0?128:i==2?256:0);
 le32(f,n);for(i=0;i<4;i++)le32(f,i==3?16:0);
 for(i=0;i<7;i++)le32(f,i==0?0x400003e8u:i==2?0x700003e8u:0);
 le32(f,0);for(i=0;i<4;i++)le32(f,sz[i]|1u);
 for(i=0;i<4;i++)fputc(1|(a[i]?2:0)|(c[i]?8:0),f);
 fwrite(b.a,1,n,f);
 for(i=0;i<4;i++){
  unsigned used=8;pal[2]=(i==0?63:0);pal[3]=(i==1?63:0);pal[4]=(i>=2?63:0);fwrite(pal,1,8,f);
  if(a[i]){le32(f,a[i]+4);for(j=0;j<a[i];j++,pa++)fputc(1+pa%120,f);used+=a[i]+4;}
  if(c[i]){le32(f,c[i]+4);for(j=0;j<c[i];j++,pc++)fputc((pc^0x55)&255,f);used+=c[i]+4;}
  for(j=used;j<sz[i];j++)fputc(0,f);
 }
 if(ferror(f)){fclose(f);return 1;}return fclose(f)!=0;
}
