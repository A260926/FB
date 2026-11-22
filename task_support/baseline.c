#include "fixture.h"
int main(void){
 struct fixture x={0};fixture_encode(&x);FLAC__StreamDecoder *d=fixture_decoder(&x);
 assert(FLAC__stream_decoder_process_until_end_of_stream(d));
 assert(x.frames==FIXTURE_FRAMES&&x.errors==0&&x.metadata>0&&x.md5_nonzero);
 assert(FLAC__stream_decoder_finish(d));FLAC__stream_decoder_delete(d);
 printf("FLAC_BASELINE_OK frames=%zu channels=2 bytes=%zu md5=verified\n",x.frames,x.len);free(x.data);return 0;
}
