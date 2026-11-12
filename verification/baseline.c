#include "config.h"
#include "src/filmgrain.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    Dav1dFilmGrainDSPContext dsp;
    Dav1dFilmGrainData d={0};
    _Alignas(64) entry lut[GRAIN_HEIGHT+1][GRAIN_WIDTH];
    _Alignas(64) unsigned char src[64*64],a[64*64],b[64*64],scaling[256];
    dav1d_film_grain_dsp_init_8bpc(&dsp);
    d.seed=12345; d.scaling_shift=8; d.ar_coeff_shift=6;
    d.overlap_flag=1; d.num_y_points=2;
    memset(src,120,sizeof src); memset(scaling,80,sizeof scaling);
    dsp.generate_grain_y(lut,&d);
    for (int row=0;row<2;row++) {
        dsp.fgy_32x32xn(a+row*32*64,src+row*32*64,64,&d,64,scaling,lut,32,row);
        dsp.fgy_32x32xn(b+row*32*64,src+row*32*64,64,&d,64,scaling,lut,32,row);
    }
    assert(!memcmp(a,b,sizeof a));
    assert(memcmp(a,src,sizeof a));
    int changed=0;for(int i=0;i<4096;i++) changed+=a[i]!=src[i];
    assert(changed>100);
    puts("BASELINE_AV1_FILMGRAIN_REPRODUCIBLE_PASS");
    return 0;
}
