#include "opus.h"
#include "opus_projection.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
    int e,s,c;
    short pcm[960*4],out[960*4];
    unsigned char packet[4096],matrix[4*4*2];
    OpusProjectionEncoder *enc=opus_projection_ambisonics_encoder_create(48000,4,3,&s,&c,OPUS_APPLICATION_AUDIO,&e);
    assert(enc && e==OPUS_OK);
    assert(opus_projection_encoder_ctl(enc,OPUS_PROJECTION_GET_DEMIXING_MATRIX(matrix,sizeof matrix))==OPUS_OK);
    OpusProjectionDecoder *dec=opus_projection_decoder_create(48000,4,s,c,matrix,sizeof matrix,&e);
    assert(dec && e==OPUS_OK);
    for(int i=0;i<960;i++) for(int j=0;j<4;j++) pcm[4*i+j]=(short)(4000*sin((j+1)*0.03*i));
    int n=opus_projection_encode(enc,pcm,960,packet,sizeof packet);
    assert(n>0);
    assert(opus_projection_decode(dec,packet,n,out,960,0)==960);
    double energy=0; for(int i=0;i<960*4;i++) energy+=(double)out[i]*out[i];
    assert(energy>10000);
    opus_projection_decoder_destroy(dec);
    opus_projection_encoder_destroy(enc);
    puts("BASELINE_OPUS_PROJECTION_PCM_PASS");
    return 0;
}
