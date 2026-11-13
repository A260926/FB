#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wavpack.h"

typedef struct { unsigned char *p; size_t n, pos, visible; } Bytes;
static void ck(int yes, const char *why) { if (!yes) { fprintf(stderr,"BASELINE_FAIL %s\n",why); exit(1); } }
static int append(void *id, void *data, int32_t n) {
    Bytes *b=id;
    unsigned char *p=realloc(b->p,b->n+(size_t)n);
    if (!p) return 0;
    b->p=p; memcpy(b->p+b->n,data,(size_t)n); b->n+=(size_t)n; return 1;
}
static int32_t rd(void *id, void *out, int32_t want) {
    Bytes *b=id; size_t n=(size_t)want;
    if (n>b->visible-b->pos) n=b->visible-b->pos;
    memcpy(out,b->p+b->pos,n); b->pos+=n; return (int32_t)n;
}
static int64_t pos(void *id) { return (int64_t)((Bytes*)id)->pos; }
static int seek_abs(void *id,int64_t p) { (void)id;(void)p;return -1; }
static int seek_rel(void *id,int64_t p,int m) { (void)id;(void)p;(void)m;return -1; }
static int push(void *id,int c) { Bytes *b=id; if(!b->pos)return EOF; --b->pos; return c; }
static int64_t length(void *id) { (void)id;return 0; }
static int no(void *id) { (void)id;return 0; }
static WavpackStreamReader64 reader={rd,NULL,pos,seek_abs,seek_rel,push,length,no,NULL,NULL};
static size_t group_end(Bytes *b, size_t start) {
    size_t p=start;
    while(p<b->n) {
        WavpackHeader h;
        ck(b->n-p>=sizeof h,"header bounds"); memcpy(&h,b->p+p,sizeof h);
        WavpackLittleEndianToNative(&h,WavpackHeaderFormat);
        ck(!memcmp(h.ckID,"wvpk",4),"physical header");
        ck(h.ckSize>=24 && (size_t)h.ckSize+8<=b->n-p,"block bounds");
        p+=(size_t)h.ckSize+8;
        if(h.block_samples && (h.flags&FINAL_BLOCK)) return p;
    }
    ck(0,"complete group"); return 0;
}
static void run(int hybrid) {
    enum {N=257,C=6}; int32_t input[N*C],out[N*C];
    Bytes wv={0},wvc={0}; WavpackConfig conf={0}; char err[128]={0};
    for(int i=0;i<N;i++) for(int c=0;c<C;c++) input[i*C+c]=((i*7919+c*1543+i*i*(c+1))%60001)-30000;
    conf.bits_per_sample=16;conf.bytes_per_sample=2;conf.num_channels=C;
    conf.channel_mask=0x3f;conf.sample_rate=48000;conf.block_samples=64;
    if(hybrid) {conf.flags=CONFIG_HYBRID_FLAG|CONFIG_CREATE_WVC;conf.bitrate=2.5;}
    WavpackContext *enc=WavpackOpenFileOutput(append,&wv,hybrid?&wvc:NULL);
    ck(enc!=NULL,"encoder open"); ck(WavpackSetConfiguration64(enc,&conf,N,NULL),"configure");
    ck(WavpackPackInit(enc),"pack init"); ck(WavpackPackSamples(enc,input,N),"pack PCM");
    ck(WavpackFlushSamples(enc),"flush");WavpackCloseFile(enc);
    size_t first=group_end(&wv,0); ck(first<wv.n,"multiple block groups");
    size_t n_groups=0;
    for(size_t p=0;p<wv.n;) {p=group_end(&wv,p);n_groups++;}
    wv.visible=wv.n; wvc.visible=wvc.n;
    WavpackContext *dec=WavpackOpenFileInputEx64(&reader,&wv,hybrid?&wvc:NULL,err,0,0);
    ck(dec!=NULL,"reader open"); ck(WavpackGetNumChannels(dec)==C,"channel count");
    ck(WavpackUnpackSamples(dec,out,N)==N,"full frame count");
    ck(!memcmp(input,out,sizeof input),"exact full PCM"); ck(!WavpackGetNumErrors(dec),"full CRC");
    WavpackCloseFile(dec);
    if(hybrid) {
        wv.pos=0;dec=WavpackOpenFileInputEx64(&reader,&wv,NULL,err,0,0);
        ck(dec!=NULL,"lossy open");ck(WavpackUnpackSamples(dec,out,N)==N,"lossy frame count");
        ck(memcmp(input,out,sizeof input)!=0,"correction actually needed");WavpackCloseFile(dec);
    }
    // Current raw API consumes one complete multichannel frame, not a resumable byte feed.
    size_t corr_first=hybrid?group_end(&wvc,0):0;
    dec=WavpackOpenRawDecoder(wv.p,(int32_t)first,wvc.p,(int32_t)corr_first,0,err,0,0);
    ck(dec!=NULL,"complete raw group opens");
    uint32_t frames=WavpackGetNumSamplesInFrame(dec);ck(frames==64,"first group samples");
    ck(WavpackUnpackSamples(dec,out,frames)==frames,"raw frame count");
    ck(!memcmp(input,out,frames*C*sizeof(int32_t)),"raw exact PCM");WavpackCloseFile(dec);
    wv.pos=0;wv.visible=19;wvc.pos=0;
    dec=WavpackOpenFileInputEx64(&reader,&wv,hybrid?&wvc:NULL,err,0,0);
    ck(dec==NULL,"short header cannot suspend open");
    printf("WV_SHORT_READ_REJECTED mode=%s bytes=19 error=%s\n",hybrid?"hybrid":"lossless",err);
    printf("WV_ROUNDTRIP_OK mode=%s frames=%d channels=%d groups=%zu raw_frames=%u\n",hybrid?"hybrid":"lossless",N,C,n_groups,frames);
    free(wv.p);free(wvc.p);
}
int main(void) {run(0);run(1);puts("WAVPACK_BASELINE_OK existing-full-reader-and-raw");return 0;}
