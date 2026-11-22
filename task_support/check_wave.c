#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t u32(const unsigned char *p){return(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned u16(const unsigned char*p){return(unsigned)p[0]|(unsigned)p[1]<<8;}
int main(int n,char**v){
    unsigned char*b;FILE*f;long size;size_t p,z,i;int fmt=0,data=0,nonzero=0;
    if(n!=2||!(f=fopen(v[1],"rb")))return 1;
    if(fseek(f,0,SEEK_END)||(size=ftell(f))<44||fseek(f,0,SEEK_SET))return 2;
    b=malloc((size_t)size);if(!b||fread(b,1,(size_t)size,f)!=(size_t)size||fclose(f))return 3;
    if(memcmp(b,"RIFF",4)||memcmp(b+8,"WAVE",4)||u32(b+4)+8!=(size_t)size)return 4;
    for(p=12;p+8<=(size_t)size;){
        z=u32(b+p+4);if(z>(size_t)size-p-8)return 5;
        if(!memcmp(b+p,"fmt ",4)){if(fmt++||z<16||u16(b+p+8)!=1||u16(b+p+10)!=1||u32(b+p+12)!=8000||u16(b+p+20)!=2||u16(b+p+22)!=16)return 6;}
        if(!memcmp(b+p,"data",4)){if(data++||z!=5120)return 7;for(i=0;i<z;++i)if(b[p+8+i])nonzero=1;}
        p+=8+z+(z&1);
    }
    free(b);if(p!=(size_t)size||fmt!=1||data!=1||!nonzero)return 8;
    puts("ROUNDTRIP_WAVE_OK rate=8000 channels=1 frames=2560 nonzero_pcm=1");return 0;
}
