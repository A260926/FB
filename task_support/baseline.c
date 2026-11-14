#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define N 8229u
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"BASELINE_FAIL line=%d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef struct {
    FILE *f;
    FLAC__int32 pcm[N*2];
    unsigned frames, fixed, decoded, last, errors;
    FLAC__StreamMetadata_StreamInfo info;
} State;
static FLAC__StreamEncoderWriteStatus enc_write(const FLAC__StreamEncoder *e, const FLAC__byte *p, size_t n, uint32_t samples, uint32_t frame, void *v) {
    State *s=v; (void)e;
    if (samples) {
        CHECK(frame==s->frames);
        CHECK(n>4 && p[0]==255 && (p[1]&0xfe)==0xf8);
        s->fixed += !(p[1]&1); s->frames++; s->last=samples;
    }
    return fwrite(p,1,n,s->f)==n ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
}
static FLAC__StreamEncoderSeekStatus enc_seek(const FLAC__StreamEncoder *e, FLAC__uint64 off, void *v) {
    (void)e; return fseek(((State*)v)->f,(long)off,SEEK_SET)==0 ? FLAC__STREAM_ENCODER_SEEK_STATUS_OK : FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
}
static FLAC__StreamEncoderTellStatus enc_tell(const FLAC__StreamEncoder *e, FLAC__uint64 *off, void *v) {
    (void)e; long p=ftell(((State*)v)->f); if(p<0) return FLAC__STREAM_ENCODER_TELL_STATUS_ERROR; *off=(FLAC__uint64)p; return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}
static FLAC__StreamDecoderReadStatus dec_read(const FLAC__StreamDecoder *d, FLAC__byte *p, size_t *n, void *v) {
    (void)d; *n=fread(p,1,*n,((State*)v)->f); return *n ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}
static FLAC__StreamDecoderWriteStatus dec_write(const FLAC__StreamDecoder *d, const FLAC__Frame *frame, const FLAC__int32 * const p[], void *v) {
    State *s=v; unsigned i,c; (void)d;
    CHECK(frame->header.channels==2 && frame->header.bits_per_sample==16);
    CHECK(frame->header.number_type==FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER);
    CHECK(frame->header.number.sample_number==s->decoded);
    CHECK(s->decoded+frame->header.blocksize<=N);
    for(i=0;i<frame->header.blocksize;i++) for(c=0;c<2;c++)
        CHECK(p[c][i]==s->pcm[(s->decoded+i)*2+c]);
    s->decoded+=frame->header.blocksize;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void dec_meta(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *v) {
    (void)d; if(m->type==FLAC__METADATA_TYPE_STREAMINFO) ((State*)v)->info=m->data.stream_info;
}
static void dec_error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus err, void *v) {
    (void)d; (void)err; ((State*)v)->errors++;
}
int main(void) {
    State *s=calloc(1,sizeof(*s)); CHECK(s);
    uint32_t rng=7; unsigned i, pos=0; const unsigned chunks[]={1,7,509,2048,3,31};
    for(i=0;i<N;i++) {
        rng^=rng<<13; rng^=rng>>17; rng^=rng<<5;
        int x=i<2048 ? 0 : i<4096 ? (int)(i%127)-63 : (int)(rng&65535)-32768;
        s->pcm[2*i]=x; s->pcm[2*i+1]=i<4096 ? x : -x/2;
    }
    s->f=tmpfile(); CHECK(s->f);
    FLAC__StreamEncoder *e=FLAC__stream_encoder_new(); CHECK(e);
    CHECK(FLAC__stream_encoder_set_channels(e,2));
    CHECK(FLAC__stream_encoder_set_bits_per_sample(e,16));
    CHECK(FLAC__stream_encoder_set_sample_rate(e,48000));
    CHECK(FLAC__stream_encoder_set_compression_level(e,5));
    CHECK(FLAC__stream_encoder_set_blocksize(e,1024));
    CHECK(FLAC__stream_encoder_set_verify(e,true));
    CHECK(FLAC__stream_encoder_init_stream(e,enc_write,enc_seek,enc_tell,NULL,s)==FLAC__STREAM_ENCODER_INIT_STATUS_OK);
    for(i=0;pos<N;i++) {
        unsigned n=chunks[i%6]; if(n>N-pos)n=N-pos;
        CHECK(FLAC__stream_encoder_process_interleaved(e,s->pcm+2*pos,n)); pos+=n;
    }
    CHECK(FLAC__stream_encoder_finish(e)); FLAC__stream_encoder_delete(e);
    CHECK(s->frames==9 && s->fixed==9 && s->last==37);
    rewind(s->f);
    FLAC__StreamDecoder *d=FLAC__stream_decoder_new(); CHECK(d);
    CHECK(FLAC__stream_decoder_set_md5_checking(d,true));
    CHECK(FLAC__stream_decoder_init_stream(d,dec_read,NULL,NULL,NULL,NULL,dec_write,dec_meta,dec_error,s)==FLAC__STREAM_DECODER_INIT_STATUS_OK);
    CHECK(FLAC__stream_decoder_process_until_end_of_stream(d));
    CHECK(FLAC__stream_decoder_finish(d)); FLAC__stream_decoder_delete(d);
    CHECK(s->decoded==N && !s->errors && s->info.total_samples==N);
    CHECK(s->info.min_blocksize==1024 && s->info.max_blocksize==1024);
    printf("BASELINE_FIXED frames=%u fixed_headers=%u final_samples=%u\n",s->frames,s->fixed,s->last);
    printf("BASELINE_PCM samples=%u channels=2 verify=1 md5=ok\n",s->decoded);
    fclose(s->f); free(s);
    return 0;
}
