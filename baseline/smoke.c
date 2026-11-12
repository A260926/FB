#include "aubio.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
    const unsigned N=1024,H=128,rate=16000;
    fvec_t *in=new_fvec(H),*out=new_fvec(1);
    aubio_pitch_t *p=new_aubio_pitch("yin",N,H,rate);
    assert(p && in && out);
    assert(aubio_pitch_set_unit(p,"Hz")==0);
    for(unsigned k=0;k<24;k++) {
        for(unsigned j=0;j<H;j++) in->data[j]=0.4f*sinf(2*3.14159265358979323846*200*(k*H+j)/rate);
        aubio_pitch_do(p,in,out);
        if(k>=8) assert(fabs(out->data[0]-200)<2);
    }
    assert(aubio_pitch_get_confidence(p)>0.95);
    puts("BASELINE_YIN_200HZ_OK");
    del_aubio_pitch(p);del_fvec(in);del_fvec(out);aubio_cleanup();
    return 0;
}
