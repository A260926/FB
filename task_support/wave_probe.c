#include "out123.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"probe:%d: %s\n",__LINE__,#x); return 1; } } while (0)
static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static int one(const char *path, int buffered, int floating) {
    int16_t pcm[34]; float fp[34]; unsigned char expected[136], file[1024];
    const size_t bytes=floating?sizeof(fp):sizeof(pcm);
    size_t i,n,p,data=0,fact=0; FILE *f; out123_handle *a=out123_new();
    CHECK(a);
    for(i=0;i<34;++i) { pcm[i]=(int16_t)(i*713-11000); fp[i]=(float)((int)i-17)/32.f; }
    for(i=0;i<34;++i) {
        if(floating) { uint32_t u; memcpy(&u,fp+i,4); expected[4*i]=(unsigned char)u; expected[4*i+1]=(unsigned char)(u>>8); expected[4*i+2]=(unsigned char)(u>>16); expected[4*i+3]=(unsigned char)(u>>24); }
        else { uint16_t u=(uint16_t)pcm[i]; expected[2*i]=(unsigned char)u; expected[2*i+1]=(unsigned char)(u>>8); }
    }
    CHECK(!buffered || !out123_set_buffer(a,65536));
    CHECK(!out123_open(a,"wav",path));
    CHECK(!out123_start(a,48000,2,floating?MPG123_ENC_FLOAT_32:MPG123_ENC_SIGNED_16));
    CHECK(out123_play(a,floating?(void*)fp:(void*)pcm,bytes)==bytes);
    out123_stop(a); CHECK(!out123_errcode(a)); out123_del(a);
    f=fopen(path,"rb"); CHECK(f); n=fread(file,1,sizeof(file),f); CHECK(!ferror(f)); CHECK(!fclose(f));
    CHECK(n>=44 && !memcmp(file,"RIFF",4) && !memcmp(file+8,"WAVE",4)); CHECK(le32(file+4)+8==n);
    for(p=12;p+8<=n;) {
        size_t len=le32(file+p+4); CHECK(len<=n-p-8);
        if(!memcmp(file+p,"data",4)) { CHECK(!data && len==bytes); CHECK(!memcmp(file+p+8,expected,bytes)); data=1; }
        if(!memcmp(file+p,"fact",4)) { CHECK(len>=4 && le32(file+p+8)==17); fact=1; }
        p+=8+len+(len&1);
    }
    CHECK(data && p==n && (!floating || fact));
    printf("WAVE_PROBE buffered=%d encoding=%s frames=17 pcm=exact riff=valid\n",buffered,floating?"float32":"s16");
    return 0;
}
int main(void) {
    int b,f;
    for(b=0;b<2;++b) for(f=0;f<2;++f) {
        char path[100]; snprintf(path,sizeof(path),".task-build/probe-%d-%d.wav",b,f); remove(path);
        if(one(path,b,f)) return 1;
    }
    puts("WAVE_BASELINE_OK cases=4"); return 0;
}
