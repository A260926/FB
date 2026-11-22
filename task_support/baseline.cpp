#include <audiofile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>
static void need(bool b) { if(!b){fprintf(stderr,"baseline failed\n");exit(1);} }
static int value(int i,int c) { return ((i*131+c*997)%40001)-20000; }
static AFfilehandle writer(const char *p,int compression) {
 AFfilesetup s=afNewFileSetup();afInitFileFormat(s,AF_FILE_CAF);
 afInitChannels(s,AF_DEFAULT_TRACK,2);afInitSampleFormat(s,AF_DEFAULT_TRACK,AF_SAMPFMT_TWOSCOMP,16);
 afInitCompression(s,AF_DEFAULT_TRACK,compression);AFfilehandle f=afOpenFile(p,"w",s);afFreeFileSetup(s);need(f);return f;
}
static long frames(const char *p) { AFfilehandle f=afOpenFile(p,"r",0);need(f);long n=afGetFrameCount(f,AF_DEFAULT_TRACK);need(afCloseFile(f)==0);return n; }
int main(int argc,char **argv){
 const bool probe=argc>1&&!strcmp(argv[1],"probe");
 const char *p=".task-build/baseline.caf";unlink(p);
 std::vector<int16_t> pcm(18002);for(int i=0;i<9001;i++)for(int c=0;c<2;c++)pcm[2*i+c]=value(i,c);
 AFfilehandle f=writer(p,AF_COMPRESSION_ALAC);
 if(probe){
  need(afWriteFrames(f,AF_DEFAULT_TRACK,pcm.data(),6000)==6000);need(afSyncFile(f)==0);long a=frames(p);
  need(afSyncFile(f)==0);long b=frames(p);
  printf("ALAC_SYNC_OBSERVATION first=%ld repeated=%ld expected=6000\n",a,b);
  need(a==6000&&b!=6000);need(afCloseFile(f)==0);puts("OLD_SHORT_TAIL_DIRECTORY_REPRODUCED");
 }else{
  need(afWriteFrames(f,AF_DEFAULT_TRACK,pcm.data(),9001)==9001);need(afCloseFile(f)==0);
  f=afOpenFile(p,"r",0);need(f);need(afGetFrameCount(f,AF_DEFAULT_TRACK)==9001);
  std::vector<int16_t> out(18002);need(afReadFrames(f,AF_DEFAULT_TRACK,out.data(),9001)==9001);need(out==pcm);
  need(afSeekFrame(f,AF_DEFAULT_TRACK,4097)==4097);int16_t pair[2];need(afReadFrames(f,AF_DEFAULT_TRACK,pair,1)==1);need(pair[0]==pcm[8194]&&pair[1]==pcm[8195]);need(afCloseFile(f)==0);
  unlink(p);f=writer(p,AF_COMPRESSION_NONE);need(afWriteFrames(f,AF_DEFAULT_TRACK,pcm.data(),9001)==9001);need(afCloseFile(f)==0);need(frames(p)==9001);
  puts("NORMAL_ALAC_PCM_ROUNDTRIP frames=9001 seek=4097 PASS");puts("NORMAL_CAF_PCM frames=9001 PASS");
 }
 unlink(p);return 0;
}
