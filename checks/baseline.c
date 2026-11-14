#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "twolame.h"
static int encode(int split, unsigned char *out) {
    twolame_options *o=twolame_init(); short pcm[1152*2*4]; int total=0;
    assert(o); twolame_set_verbosity(o,0); twolame_set_num_channels(o,2);
    twolame_set_in_samplerate(o,44100); twolame_set_out_samplerate(o,44100);
    twolame_set_mode(o,TWOLAME_JOINT_STEREO); twolame_set_VBR(o,1);
    assert(twolame_init_params(o)==0);
    /* Existing fallback is a capability witness, not the future contract. */
    assert(twolame_get_mode(o)==TWOLAME_STEREO);
    for(int i=0;i<1152*4;i++) { pcm[2*i]=(short)((i*137)%20000-10000); pcm[2*i+1]=(short)((i*251)%18000-9000); }
    for(int pos=0;pos<4608;) {
        int n=split ? 137 : 4608; if(n>4608-pos)n=4608-pos;
        int z=twolame_encode_buffer_interleaved(o,pcm+pos*2,n,out+total,32768-total);
        assert(z>=0); total+=z;pos+=n;
    }
    int z=twolame_encode_flush(o,out+total,32768-total);assert(z>=0);total+=z;
    twolame_close(&o); return total;
}
int main(void) {
    unsigned char a[32768],b[32768];int n=encode(0,a),m=encode(1,b);
    assert(n>0&&n==m&&!memcmp(a,b,n));int pos=0,frames=0;
    const int br[]={0,32,48,56,64,80,96,112,128,160,192,224,256,320,384};
    while(pos<n){assert(a[pos]==255&&(a[pos+1]&0xfe)==0xfc);int k=a[pos+2]>>4;
      assert(k>0&&k<15&&(a[pos+3]>>6)==0);pos+=144000*br[k]/44100;frames++;}
    assert(pos==n&&frames==4); printf("TWOLAME_BASELINE_OK frames=%d bytes=%d fragmented=equal fallback=stereo\n",frames,n);
}
