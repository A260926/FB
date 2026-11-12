#define QOA_IMPLEMENTATION
#define QOA_NO_STDIO
#include "qoa.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    enum { N = QOA_FRAME_LEN + 21, C = 2 };
    short *in = malloc(N*C*sizeof(*in));
    assert(in);
    for (int i=0;i<N;i++) { in[2*i]=(i*173)%18001-9000; in[2*i+1]=(i%29<14?6000:-5000); }
    qoa_desc enc={0}, dec={0}, framed={0};
    enc.channels=C; enc.samplerate=32000; enc.samples=N;
    unsigned bytes=0; unsigned char *data=qoa_encode(in,&enc,&bytes);
    assert(data && bytes>0);
    short *pcm=qoa_decode(data,bytes,&dec); assert(pcm && dec.samples==N && dec.channels==C);
    assert(qoa_decode_header(data,bytes,&framed)==8);
    short *separate=calloc(N*C,sizeof(short)); assert(separate);
    unsigned pos=8, frames=0, got=0, len=0;
    while (got<N) { unsigned n=qoa_decode_frame(data+pos,bytes-pos,&framed,separate+got*C,&len); assert(n && len); pos+=n; got+=len; frames++; }
    assert(pos==bytes && frames==2 && got==N);
    assert(!memcmp(pcm,separate,N*C*sizeof(short)));
    long long energy=0; for(int i=0;i<N*C;i++) energy+=(long long)pcm[i]*pcm[i]; assert(energy>0);
    printf("QOA_BASELINE_OK frames=%u samples=%u channels=%u bytes=%u\n",frames,got,dec.channels,bytes);
    free(in); free(data); free(pcm); free(separate); return 0;
}
