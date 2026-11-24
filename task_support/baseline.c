#include "smacker.h"
#include "fixture.h"
#include <stdio.h>
#include <string.h>
#define NEED(x) do { if(!(x)){fprintf(stderr,"baseline check failed line %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main(int argc,char **argv) {
 smk s;unsigned i,j;unsigned long frame,count;double usf;unsigned long totals[2]={0,0};
 if(argc!=2)return 2;NEED(smk_fixture_write(argv[1])==0);
 s=smk_open_file(argv[1],SMK_MODE_DISK);NEED(s);NEED(smk_enable_all(s,0x85)==0);
 NEED(smk_info_all(s,&frame,&count,0,&usf)==0);NEED(count==3&&usf==33330.0);
 for(i=0;i<3;i++){
  NEED((i==0?smk_first(s):smk_next(s))>=0);
  for(j=0;j<16;j++)NEED(smk_get_video(s)[j]==7);
  NEED(smk_get_palette(s)[7*3+i]==255);
  totals[0]+=smk_get_audio_size(s,0);totals[1]+=smk_get_audio_size(s,2);
 }
 NEED(totals[0]==105&&totals[1]==240);smk_close(s);
 printf("BASELINE_SMACKER_PASS frames=3 usf=33330 pixels=48 pcm0=105 pcm2=240\n");return 0;
}
