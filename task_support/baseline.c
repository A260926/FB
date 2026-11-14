#include "caption.h"
#include "scc.h"
#include "vtt.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void){
 const char *text="Scenarist_SCC V1.0\n\n00:00:00:00\t9420 94ae 9470 c8e9 942f 942f\n00:00:01:00\t8080 8080 8080 8080 8080 8080 8080 8080\n";
 scc_t *s=0;size_t n=scc_to_608(&s,text);assert(n>0&&s&&s->cc_size==6);
 caption_frame_t f;caption_frame_init(&f);int ready=0;
 for(unsigned i=0;i<s->cc_size;i++)if(caption_frame_decode(&f,s->cc_data[i],s->timestamp)==LIBCAPTION_READY)ready++;
 char out[CAPTION_FRAME_TEXT_BYTES];caption_frame_to_text(&f,out);
 assert(strcmp(out,"Hi")==0&&ready==1);
 printf("SCC_BASELINE_OK words=%u ready=%d text=%s\n",s->cc_size,ready,out);
 vtt_t *v=vtt_new();vtt_block_t *cue=vtt_cue_from_caption_frame(&f,v);assert(cue&&cue->duration==0);
 printf("CURRENT_LINE_TIME=%.6f EXPECTED_EOC_FRAME=4 EXPECTED_SECONDS=%.9f OPEN_DURATION=%.1f\n",cue->timestamp,4*1001.0/30000,cue->duration);
 vtt_free(v);scc_free(s);return 0;
}
