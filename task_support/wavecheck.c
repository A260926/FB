#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
static uint32_t le(const unsigned char *b){return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);}
int main(int argc,char **argv){
 if(argc!=2)return 2;
 FILE *f=fopen(argv[1],"rb");if(!f)return 3;
 if(fseek(f,0,SEEK_END))return 4;long n=ftell(f);if(n<52||n>16000000)return 5;
 rewind(f);unsigned char *b=malloc((size_t)n);if(!b)return 6;
 if(fread(b,1,(size_t)n,f)!=(size_t)n)return 7;fclose(f);
 if(memcmp(b,"RIFF",4)||memcmp(b+8,"WAVE",4)||le(b+4)!=(uint32_t)n-8)return 8;
 size_t data=0,bytes=0;int fmt=0;
 for(size_t p=12;p+8<=(size_t)n;){
  uint32_t k=le(b+p+4);if(k>(size_t)n-p-8)return 9;
  if(!memcmp(b+p,"fmt ",4)){if(k<16||b[p+8]!=3||b[p+9]||b[p+10]!=2||b[p+11]||le(b+p+12)!=48000||b[p+20]!=8||b[p+22]!=32)return 10;fmt=1;}
  if(!memcmp(b+p,"data",4)){data=p+8;bytes=k;}
  p+=8+(size_t)k+(k&1);
 }
 if(!fmt||bytes<16||bytes%8)return 11;
 int nz=0,different=0;
 for(size_t p=data;p<data+bytes;p+=8){
  uint32_t u=le(b+p),v=le(b+p+4);float l,r;memcpy(&l,&u,4);memcpy(&r,&v,4);
  if(!isfinite(l)||!isfinite(r))return 12;
  if(l!=0||r!=0)nz=1;if(l!=r)different=1;
 }
 free(b);if(!nz||!different)return 13;
 printf("WAVE_CONTENT_OK frames=%zu\n",bytes/8);return 0;
}
