/* Original task support, CC0-1.0. Checks only the pre-existing I_PCM path. */
#include "h264bsd_decoder.h"
#include "h264bsd_util.h"
#include "fixture.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static void check(int ok,const char *s) { if(!ok) {fprintf(stderr,"BASE_FAIL %s\n",s);exit(10);} }
static unsigned drain(storage_t *dec,unsigned *count) {
    u32 id,idr,errors;u8 *p;unsigned got=0;
    while((p=h264bsdNextOutputPicture(dec,&id,&idr,&errors))) {
        check(!errors,"concealed picture");check(*count<2,"unexpected extra frame");
        size_t k=0;
        for(unsigned plane=0;plane<3;plane++) {
            unsigned side=plane?16:32;
            for(unsigned y=0;y<side;y++) for(unsigned x=0;x<side;x++)
                check(p[k++]==task_pixel(*count,plane,x,y),"I_PCM content");
        }
        ++*count;++got;
    }
    return got;
}
int main(int argc,char **argv) {
    (void)argv;int observe=argc>1;uint8_t input[32768];storage_t dec;
    size_t n=task_fixture(input,sizeof(input),observe),offset=0;unsigned pictures=0,steps=0,errors=0;
    check(h264bsdInit(&dec,HANTRO_FALSE)==HANTRO_OK,"init");
    while(offset<n) {
        u32 used=0,result=h264bsdDecode(&dec,input+offset,(u32)(n-offset),0,&used);
        check(++steps<100,"no progress");check(used<=n-offset,"consumption");offset+=used;
        if(result==H264BSD_ERROR || result==H264BSD_PARAM_SET_ERROR || result==H264BSD_MEMALLOC_ERROR) {
            errors++; if(!observe) check(0,"decode error");break;
        }
        if(result==H264BSD_HDRS_RDY) check(h264bsdPicWidth(&dec)==2 && h264bsdPicHeight(&dec)==2,"dimensions");
        drain(&dec,&pictures);
    }
    if(observe) {
        printf("B_SLICE_OBSERVED decode_errors=%u offset=%zu bytes=%zu future_total_frames=3\n",errors,offset,n);
    } else {
        h264bsdFlushBuffer(&dec);drain(&dec,&pictures);check(pictures==2,"frame count");
        printf("H264BSD_BASE_OK frames=%u pixels_checked=3072 concealed=0\n",pictures);
    }
    h264bsdShutdown(&dec);return 0;
}
