#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t u32(const unsigned char *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
int main(int argc, char **argv) {
    FILE *f; long len; unsigned char *b; size_t n,p,i; int data=0,nonzero=0;
    if(argc!=2 || !(f=fopen(argv[1],"rb"))) return 1;
    if(fseek(f,0,SEEK_END) || (len=ftell(f))<44 || fseek(f,0,SEEK_SET)) return 2;
    n=(size_t)len; b=malloc(n); if(!b || fread(b,1,n,f)!=n || fclose(f)) return 3;
    if(memcmp(b,"RIFF",4) || memcmp(b+8,"WAVE",4) || u32(b+4)+8!=n) return 4;
    for(p=12;p+8<=n;) {
        size_t z=u32(b+p+4); if(z>n-p-8) return 5;
        if(!memcmp(b+p,"data",4)) { if(data++ || !z) return 6; for(i=0;i<z;++i) if(b[p+8+i]) nonzero=1; }
        p+=8+z+(z&1);
    }
    free(b); if(p!=n || data!=1 || !nonzero) return 7;
    puts("EXAMPLE_WAVE_OK nonzero_pcm=1"); return 0;
}
