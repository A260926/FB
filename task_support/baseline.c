#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "micromod.h"
#include "fixture.h"
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr,"baseline failed line %d: %s\n",__LINE__,#c); return 1; } } while(0)
static uint32_t hash_pcm(const short *p, int frames) {
    uint32_t h=2166136261u; int i;
    for(i=0;i<frames*2;++i) { h=(h^(unsigned char)p[i])*16777619u; h=(h^(unsigned char)((unsigned short)p[i]>>8))*16777619u; }
    return h;
}
int main(int argc, char **argv) {
    signed char mod[FIXTURE_BYTES], original[FIXTURE_BYTES];
    short full[4096*2], pieces[4096*2], skip[256*2], changed[4096*2];
    long duration; int off=0,n,i; FILE *f;
    fixture_mod(mod,0); memcpy(original,mod,sizeof(mod));
    REQUIRE(micromod_calculate_mod_file_len(mod)==FIXTURE_BYTES);
    REQUIRE(micromod_initialise(mod,8287)==0);
    duration=micromod_calculate_song_duration(); REQUIRE(duration==10560);
    memset(full,0,sizeof(full)); micromod_get_audio(full,4096);
    REQUIRE(hash_pcm(full,4096)==0x460b9e85u); REQUIRE(full[0]>0); for(i=0;i<4096;++i) REQUIRE(full[i*2+1]==0);
    REQUIRE(micromod_initialise(mod,8287)==0); memset(pieces,0,sizeof(pieces));
    while(off<4096) { n=1+(off*17+23)%257; if(n>4096-off)n=4096-off; micromod_get_audio(pieces+off*2,n);off+=n; }
    REQUIRE(!memcmp(full,pieces,sizeof(full)));
    REQUIRE(micromod_initialise(mod,8287)==0); micromod_get_audio(NULL,377);
    memset(skip,0,sizeof(skip)); micromod_get_audio(skip,256);
    REQUIRE(!memcmp(skip,full+377*2,sizeof(skip))); REQUIRE(!memcmp(mod,original,sizeof(mod)));
    printf("LEGACY_PCM_OK frames=4096 duration=%ld hash=%08x\n",duration,hash_pcm(full,4096));
    fixture_mod(mod,1); REQUIRE(micromod_initialise(mod,8287)==0);
    memset(changed,0,sizeof(changed)); micromod_get_audio(changed,4096);
    REQUIRE(changed[164*2]>0 && changed[165*2]<0);
    printf("RELOAD_STARTPOINT frame164=%d frame165=%d\n",changed[328],changed[330]);
    fixture_mod(mod,2); REQUIRE(micromod_initialise(mod,8287)==0);
    memset(changed,0,sizeof(changed)); micromod_get_audio(changed,4096);
    REQUIRE(!memcmp(full,changed,sizeof(full)));
    puts("EFX_STARTPOINT same_as_plain=1 source_unchanged=1");
    if(argc==2) { fixture_mod(mod,0); f=fopen(argv[1],"wb"); REQUIRE(f); REQUIRE(fwrite(mod,1,sizeof(mod),f)==sizeof(mod)); REQUIRE(fclose(f)==0); }
    puts("MICROMOD_BASELINE_COMPLETE"); return 0;
}
