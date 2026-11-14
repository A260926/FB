#define MINIMP4_IMPLEMENTATION
#include "../minimp4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"baseline failed line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef struct { unsigned char *p; size_t n; } Input;
static int read_at(int64_t off,void *dst,size_t n,void *opaque) {
    Input *b=(Input*)opaque;
    if(off<0 || (uint64_t)off>b->n || n>b->n-(size_t)off) return 1;
    memcpy(dst,b->p+(size_t)off,n); return 0;
}
static unsigned be32(const unsigned char *p) { return (unsigned)p[0]<<24|(unsigned)p[1]<<16|(unsigned)p[2]<<8|p[3]; }
int main(void) {
    Input b={0}; MP4D_demux_t m={0}; unsigned i,n,ts,dur,total=0,found=0; int sps_n,pps_n; const void *sps,*pps;
    FILE *f=fopen("vectors/out_ref.mp4","rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END)); b.n=(size_t)ftell(f);rewind(f);
    b.p=(unsigned char*)malloc(b.n);CHECK(b.p);CHECK(fread(b.p,1,b.n,f)==b.n);fclose(f);
    CHECK(MP4D_open(&m,read_at,&b,(int64_t)b.n)); CHECK(m.track_count==1);CHECK(m.track[0].sample_count>1);
    sps=MP4D_read_sps(&m,0,0,&sps_n);pps=MP4D_read_pps(&m,0,0,&pps_n);CHECK(sps&&pps&&sps_n>0&&pps_n>0);
    for(i=0;i<m.track[0].sample_count;i++) {
        MP4D_file_offset_t off=MP4D_frame_offset(&m,0,i,&n,&ts,&dur);
        CHECK(n>0 && (uint64_t)off<=b.n && n<=b.n-(size_t)off);CHECK(dur>0);total+=n;
    }
    for(i=4;i+12<b.n;i++) if(!memcmp(b.p+i,"stsd",4)) { CHECK(be32(b.p+i+8)==1);found++; }
    CHECK(found==1);
    printf("MINIMP4_BASELINE single_description=1 samples=%u payload_bytes=%u dimensions=%ux%u\n",m.track[0].sample_count,total,m.track[0].SampleDescription.video.width,m.track[0].SampleDescription.video.height);
    MP4D_close(&m);free(b.p);return 0;
}
