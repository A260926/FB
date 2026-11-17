#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "src/libttaR.h"
static int invoke(const char *s){ int r=system(s); if(r) fprintf(stderr,"command failed: %s (%d)\n",s,r);return r; }
static int equal_files(const char*a,const char*b){FILE*x=fopen(a,"rb"),*y=fopen(b,"rb");int c,d;if(!x||!y)return 0;do{c=fgetc(x);d=fgetc(y);if(c!=d){fclose(x);fclose(y);return 0;}}while(c!=EOF);fclose(x);fclose(y);return 1;}
int main(void){
 const size_t frames=25000;size_t i;FILE*f=fopen(".task-build/input.raw","wb");if(!f)return 2;
 for(i=0;i<frames*2;++i){uint16_t v=(uint16_t)((i*37u+(i/17u)*503u)^((i%2)?0x7312u:0x291du));fputc(v&255,f);fputc(v>>8,f);}if(fclose(f))return 3;
 remove(".task-build/audio.tta");remove(".task-build/st.raw");remove(".task-build/mt.raw");
 if(invoke(".task-build/ttaR encode -S -q --rawpcm=i16le,8000,2 .task-build/input.raw -o .task-build/audio.tta"))return 4;
 if(invoke(".task-build/ttaR decode -S -q -f raw .task-build/audio.tta -o .task-build/st.raw"))return 5;
 if(invoke(".task-build/ttaR decode -M -t 2 -q -f raw .task-build/audio.tta -o .task-build/mt.raw"))return 6;
 if(!equal_files(".task-build/input.raw",".task-build/st.raw")||!equal_files(".task-build/input.raw",".task-build/mt.raw"))return 7;
 printf("BASELINE_OK PCM_frames=%zu channels=2 frame_length=%zu ST=MT=source\n",frames,libttaR_nsamples_perframe_tta1(8000));return 0;
}
