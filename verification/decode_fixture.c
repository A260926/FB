#include "dav1d/dav1d.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    assert(argc==2);
    FILE *f=fopen(argv[1],"rb");assert(f);
    unsigned char head[44];assert(fread(head,1,44,f)==44);
    unsigned n=head[32]|head[33]<<8|head[34]<<16|head[35]<<24;
    Dav1dData data={0};unsigned char *buf=dav1d_data_create(&data,n);assert(buf);
    assert(fread(buf,1,n,f)==n);fclose(f);
    Dav1dSettings s;dav1d_default_settings(&s);s.n_threads=1;s.apply_grain=0;
    Dav1dContext *c=NULL;assert(!dav1d_open(&c,&s));
    assert(!dav1d_send_data(c,&data));
    Dav1dPicture p={0},out={0};assert(!dav1d_get_picture(c,&p));
    assert(p.p.w==96 && p.p.h==80);
    Dav1dFrameHeader h=*p.frame_hdr;
    h.film_grain.present=1;h.film_grain.data.seed=12345;
    h.film_grain.data.num_y_points=2;
    h.film_grain.data.y_points[0][0]=0;h.film_grain.data.y_points[0][1]=80;
    h.film_grain.data.y_points[1][0]=255;h.film_grain.data.y_points[1][1]=80;
    h.film_grain.data.scaling_shift=8;h.film_grain.data.ar_coeff_shift=6;
    h.film_grain.data.overlap_flag=1;
    p.frame_hdr=&h;assert(!dav1d_apply_grain(c,&out,&p));
    assert(out.data[0] && out.data[0]!=p.data[0]);
    printf("BASELINE_AV1_FIXTURE_%d_PASS\n",p.p.bpc);
    dav1d_picture_unref(&out);dav1d_picture_unref(&p);dav1d_close(&c);
    return 0;
}
