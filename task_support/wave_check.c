/* Independent container check of the newly produced example artifact. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
static uint32_t u32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned u16(const unsigned char *p){return p[0]|(unsigned)p[1]<<8;}
int main(int argc,char **argv){
 if(argc!=2)return 1;FILE *f=fopen(argv[1],"rb");if(!f)return 2;
 if(fseek(f,0,SEEK_END))return 3;long size=ftell(f);rewind(f);if(size<48||size>16000000)return 4;
 unsigned char *b=malloc(size);if(!b||fread(b,1,size,f)!=(size_t)size)return 5;fclose(f);
 if(memcmp(b,"RIFF",4)||memcmp(b+8,"WAVE",4)||(uint64_t)u32(b+4)+8!=(uint64_t)size)return 6;
 size_t at=12;int format=0,data=0,nonzero=0;
 while(at+8<=(size_t)size){uint32_t n=u32(b+at+4);size_t end=at+8+(size_t)n;if(end>(size_t)size)return 7;
  if(!memcmp(b+at,"fmt ",4)){if(n<16||u16(b+at+8)!=1||u16(b+at+10)!=2||!u32(b+at+12)||u16(b+at+20)!=4||u16(b+at+22)!=16)return 8;format=1;}
  if(!memcmp(b+at,"data",4)){if(n<4||n%4)return 9;data=1;for(size_t i=at+8;i<end;i++)if(b[i])nonzero=1;}
  at=end+(n&1);
 }
 free(b);if(at!=(size_t)size||!format||!data||!nonzero)return 10;puts("CONFORM_EXAMPLE_WAV_OK");return 0;
}
