/* Fixed API binding, not an implementation of checkpoint persistence. */
#include <FLAC/stream_encoder_checkpoint.h>
#include "audio_fixture.h"
#define NEED(x) do { if(!(x)) return __LINE__; }while(0)
int main(void) {
    struct audio_sink sink={0};FLAC__StreamEncoder *a,*b;FLAC__byte *state;size_t n;FLAC__uint64 pos;
    int32_t pcm[34]={0};sink.data=calloc(1,AUDIO_CAP);NEED(sink.data);
    a=audio_encoder(&sink);NEED(a);n=FLAC__stream_encoder_checkpoint_size(a);NEED(n>0);
    state=malloc(n);NEED(state);NEED(FLAC__stream_encoder_checkpoint_write(a,state,n));
    pos=sink.pos;b=FLAC__stream_encoder_checkpoint_resume(state,n,pos,audio_write,audio_seek,audio_tell,NULL,&sink);NEED(b && b!=a);
    NEED(FLAC__stream_encoder_process_interleaved(b,pcm,17));NEED(FLAC__stream_encoder_finish(b));NEED(sink.size>pos);
    FLAC__stream_encoder_delete(b);sink.disabled=1;FLAC__stream_encoder_delete(a);free(state);free(sink.data);
    puts("CHECKPOINT_API_BOUND samples=17");return 0;
}
