#include "audio_fixture.h"
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"baseline failed line %d\n",__LINE__);return 1;} }while(0)
int main(void) {
    struct audio_sink sink={0}; struct audio_decode decoded={0};
    FLAC__StreamEncoder *e;FLAC__StreamDecoder *d;int32_t samples[2*113];unsigned at=0,i,c;
    sink.data=calloc(1,AUDIO_CAP);REQUIRE(sink.data);e=audio_encoder(&sink);REQUIRE(e);
    while(at<AUDIO_FRAMES) {
        unsigned n=AUDIO_FRAMES-at;if(n>113)n=113;
        for(i=0;i<n;i++)for(c=0;c<2;c++)samples[2*i+c]=audio_value(at+i,c);
        REQUIRE(FLAC__stream_encoder_process_interleaved(e,samples,n));at+=n;
    }
    REQUIRE(FLAC__stream_encoder_finish(e));FLAC__stream_encoder_delete(e);
    decoded.sink=&sink;d=FLAC__stream_decoder_new();REQUIRE(d);
    REQUIRE(FLAC__stream_decoder_set_md5_checking(d,1));
    REQUIRE(FLAC__stream_decoder_init_stream(d,audio_read,NULL,NULL,NULL,NULL,audio_decoded,audio_metadata,audio_error,&decoded)==FLAC__STREAM_DECODER_INIT_STATUS_OK);
    REQUIRE(FLAC__stream_decoder_process_until_end_of_stream(d));REQUIRE(FLAC__stream_decoder_finish(d));
    REQUIRE(decoded.frames==AUDIO_FRAMES && !decoded.bad && !decoded.errors && decoded.md5_nonzero);
    printf("FLAC_BASELINE frames=%u channels=2 bytes=%zu pcm_exact=1 md5_checked=1\n",decoded.frames,sink.size);
    FLAC__stream_decoder_delete(d);free(sink.data);return 0;
}
