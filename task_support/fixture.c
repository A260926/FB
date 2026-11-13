/* Original task support, CC0-1.0. Handwritten Annex-B, 32x32 YUV420. */
#include "fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint8_t b[8192]; size_t bits; } bits_t;
static void bit(bits_t *w, unsigned v) {
    if (w->bits >= sizeof(w->b)*8) abort();
    if (v&1) w->b[w->bits/8] |= (uint8_t)(1u << (7-w->bits%8));
    w->bits++;
}
static void fixed(bits_t *w, unsigned v, unsigned n) {
    while(n) bit(w, v >> --n);
}
static void ue(bits_t *w, unsigned v) {
    unsigned n=0, q=v+1; while(q>>++n) {}
    for(unsigned i=1;i<n;i++) bit(w,0);
    fixed(w,v+1,n);
}
static void se(bits_t *w, int v) { ue(w, v<=0 ? (unsigned)(-2*v) : (unsigned)(2*v-1)); }
static void align0(bits_t *w) { while(w->bits%8) bit(w,0); }
static void trailing(bits_t *w) { bit(w,1);align0(w); }
static void nal(uint8_t *dst,size_t cap,size_t *n,unsigned h,bits_t *w) {
    unsigned zeros=0;
    if(*n+(w->bits/8)*2+5>cap) abort();
    dst[(*n)++]=0;dst[(*n)++]=0;dst[(*n)++]=0;dst[(*n)++]=1;dst[(*n)++]=(uint8_t)h;
    for(size_t i=0;i<w->bits/8;i++) {
        unsigned c=w->b[i];
        if(zeros>=2 && c<=3) { dst[(*n)++]=3;zeros=0; }
        dst[(*n)++]=(uint8_t)c;
        zeros=c==0?zeros+1:0;
    }
    memset(w,0,sizeof(*w));
}
unsigned task_pixel(unsigned ref,unsigned plane,unsigned x,unsigned y) {
    return (17u+ref*71u+plane*37u+x*3u+y*5u)%220u+16u;
}
static void pcm(bits_t *w, unsigned ref, unsigned mb) {
    ue(w,25);align0(w); /* I_PCM in I slice */
    for(unsigned p=0;p<3;p++) {
        unsigned side=p?8:16;
        for(unsigned y=0;y<side;y++) for(unsigned x=0;x<side;x++)
            fixed(w,task_pixel(ref,p,(mb%2)*side+x,(mb/2)*side+y),8);
    }
}
size_t task_fixture(uint8_t *dst,size_t capacity,int include_b) {
    size_t n=0;bits_t w={{0},0};
    fixed(&w,include_b?77:66,8);fixed(&w,0,8);fixed(&w,10,8);ue(&w,0);
    ue(&w,0); /* 4 frame_num bits */
    ue(&w,0);ue(&w,0); /* POC type0, 4 POC bits */
    ue(&w,2);bit(&w,0);ue(&w,1);ue(&w,1); /* 2 refs; 2x2 MB */
    bit(&w,1);bit(&w,1);bit(&w,0);bit(&w,0);trailing(&w);nal(dst,capacity,&n,0x67,&w);
    ue(&w,0);ue(&w,0);bit(&w,0);bit(&w,0);ue(&w,0);
    ue(&w,0);ue(&w,0);bit(&w,0);fixed(&w,0,2);
    se(&w,0);se(&w,0);se(&w,0);bit(&w,1);bit(&w,0);bit(&w,0);
    trailing(&w);nal(dst,capacity,&n,0x68,&w);
    for(unsigned ref=0;ref<2;ref++) {
        ue(&w,0);ue(&w,2);ue(&w,0);fixed(&w,ref,4);
        if(!ref) ue(&w,0); /* idr_pic_id */
        fixed(&w,ref*4,4);
        if(!ref) {bit(&w,0);bit(&w,0);} else bit(&w,0);
        se(&w,0);ue(&w,1); /* no deblock */
        for(unsigned mb=0;mb<4;mb++) pcm(&w,ref,mb);
        trailing(&w);nal(dst,capacity,&n,ref?0x61:0x65,&w);
    }
    if(include_b) {
        ue(&w,0);ue(&w,1);ue(&w,0);fixed(&w,2,4);fixed(&w,2,4);
        bit(&w,1); /* direct_spatial flag (no direct MBs) */
        bit(&w,0);bit(&w,0);bit(&w,0); /* default counts, no reordering */
        se(&w,0);ue(&w,1);
        for(unsigned mb=0;mb<4;mb++) {
            ue(&w,0);ue(&w,3); /* no skip, B_Bi_16x16 */
            se(&w,0);se(&w,0);se(&w,0);se(&w,0);ue(&w,0); /* MVDs, no residual */
        }
        trailing(&w);nal(dst,capacity,&n,0x01,&w);
    }
    return n;
}
#ifdef TASK_WRITE_FIXTURES
int main(int argc,char **argv) {
    uint8_t data[32768];
    if(argc!=3) return 2;
    for(int b=0;b<2;b++) {
        size_t n=task_fixture(data,sizeof(data),b);FILE *f=fopen(argv[b+1],"wb");
        if(!f || fwrite(data,1,n,f)!=n || fclose(f)) return 3;
        printf("FIXTURE b=%d bytes=%zu\n",b,n);
    }
    return 0;
}
#endif
