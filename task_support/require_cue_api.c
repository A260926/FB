#include "out123.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"required cue API probe failed: %s\n",#x);return 1;}}while(0)
static uint32_t le32(const unsigned char*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int main(int argc,char**argv){
 int16_t pcm[4]={100,-200,300,-400};out123_handle*a;unsigned char file[4096];size_t n,p;int data=0,cue=0;FILE*f;
 REQUIRE(argc==2);a=out123_new();REQUIRE(a!=NULL);
 REQUIRE(out123_open(a,"wav",argv[1])==0);REQUIRE(out123_start(a,48000,2,MPG123_ENC_SIGNED_16)==0);
 REQUIRE(out123_play(a,pcm,sizeof pcm)==sizeof pcm);
 REQUIRE(out123_wav_cue(a,7,"first")==0);
 REQUIRE(out123_play(a,pcm,sizeof pcm)==sizeof pcm);
 REQUIRE(out123_wav_cue(a,8,"last")==0);
 out123_stop(a);REQUIRE(out123_errcode(a)==0);out123_del(a);
 f=fopen(argv[1],"rb");REQUIRE(f!=NULL);n=fread(file,1,sizeof file,f);REQUIRE(!ferror(f));REQUIRE(fclose(f)==0);
 REQUIRE(n>=44&&!memcmp(file,"RIFF",4)&&!memcmp(file+8,"WAVE",4));REQUIRE(le32(file+4)+8==n);
 for(p=12;p+8<=n;){size_t len=le32(file+p+4);REQUIRE(len<=n-p-8);
  if(!memcmp(file+p,"data",4)){REQUIRE(!data&&len==2*sizeof pcm);REQUIRE(!memcmp(file+p+8,pcm,sizeof pcm));data=1;}
  if(!memcmp(file+p,"cue ",4)){const unsigned char*c=file+p+12;REQUIRE(!cue&&len==52&&le32(file+p+8)==2);
   REQUIRE(le32(c)==7&&le32(c+4)==2&&!memcmp(c+8,"data",4)&&le32(c+12)==0&&le32(c+16)==0&&le32(c+20)==2);
   c+=24;REQUIRE(le32(c)==8&&le32(c+4)==4&&!memcmp(c+8,"data",4)&&le32(c+12)==0&&le32(c+16)==0&&le32(c+20)==4);cue=1;}
  p+=8+len+(len&1);
 }
 REQUIRE(p==n&&data&&cue);puts("REQUIRED_CUE_API_CALL_OK");return 0;
}
