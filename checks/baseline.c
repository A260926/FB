#include "g722_encoder.h"
#include "g722_decoder.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    short pcm[2048],whole[2048],chunk[2048]; unsigned char data[2048];
    int rates[3]={48000,56000,64000},cases=0;
    for(int r=0;r<3;r++)for(int narrow=0;narrow<2;narrow++) {
        int opt=narrow?G722_SAMPLE_RATE_8000:0;
        for(int i=0;i<2048;i++)pcm[i]=(short)(8000*sin(i*0.071)+1300*cos(i*0.137));
        G722_ENC_CTX *enc=g722_encoder_new(rates[r],opt);
        G722_DEC_CTX *a=g722_decoder_new(rates[r],opt),*b=g722_decoder_new(rates[r],opt); assert(enc&&a&&b);
        int bytes=g722_encode(enc,pcm,2048,data);assert(bytes==(narrow?2048:1024));
        int n=g722_decode(a,data,bytes,whole);assert(n==2048);int total=0;
        for(int pos=0;pos<bytes;){int len=bytes-pos<37?bytes-pos:37;total+=g722_decode(b,data+pos,len,chunk+total);pos+=len;}
        assert(total==n&&!memcmp(whole,chunk,n*sizeof(short)));
        long long energy=0;for(int i=0;i<n;i++)energy+=(long long)whole[i]*whole[i];assert(energy>0);
        g722_encoder_destroy(enc);g722_decoder_destroy(a);g722_decoder_destroy(b);cases++;
    }
    printf("G722_BASELINE_OK cases=%d rates=3 layouts=2 chunk_equal=1\n",cases);
}
