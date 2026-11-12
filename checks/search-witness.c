#define QOA_IMPLEMENTATION
#define QOA_NO_STDIO
#include "qoa.h"
#include "search_witness.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint64_t distortion(const short *pcm) {
    uint64_t total=0;
    for(int i=0;i<40;i++) { int64_t d=(int)witness_pcm[i]-pcm[i]; total+=(uint64_t)(d*d); }
    return total;
}
int main(void) {
    qoa_desc enc={0},dec={0},fixed={0};
    enc.channels=1;enc.samplerate=32000;enc.samples=40;
    unsigned size=0,len=0;
    unsigned char *data=qoa_encode(witness_pcm,&enc,&size);assert(data&&size==48);
    short *pcm=qoa_decode(data,size,&dec);assert(pcm&&dec.samples==40&&dec.channels==1);
    uint64_t greedy=distortion(pcm);
    short alt[40];fixed.channels=1;fixed.samplerate=32000;
    assert(qoa_decode_frame(witness_frame,sizeof(witness_frame),&fixed,alt,&len)==40&&len==40);
    uint64_t feasible=distortion(alt);
    assert(greedy==360667757 && feasible==327723244 && feasible<greedy);
    printf("QOA_SEARCH_BASELINE_OK samples=40 greedy_sse=%llu feasible_sse=%llu greedy_scales=%u,%u feasible_scales=%u,%u\n",
        (unsigned long long)greedy,(unsigned long long)feasible,data[32]>>4,data[40]>>4,witness_frame[24]>>4,witness_frame[32]>>4);
    free(data);free(pcm);return 0;
}
