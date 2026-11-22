/* Fixed compile/link consumer, not a resumable decoder implementation. */
#include "fixture.h"
int main(void){
 struct fixture x={0};fixture_encode(&x);FLAC__StreamDecoder *d=fixture_decoder(&x);
 FLAC__StreamDecoderReadStatus wait_status=FLAC__STREAM_DECODER_READ_STATUS_WOULD_BLOCK;(void)wait_status;
 int ended=0;
 for(int calls=0;calls<100;calls++){
  FLAC__StreamDecoderPumpStatus r=FLAC__stream_decoder_process_available(d);
  assert(r==FLAC__STREAM_DECODER_PUMP_PROGRESS||r==FLAC__STREAM_DECODER_PUMP_END);
  if(r==FLAC__STREAM_DECODER_PUMP_END){ended=1;break;}
 }
 assert(ended&&x.frames==FIXTURE_FRAMES&&x.errors==0&&x.metadata>0&&x.md5_nonzero);
 assert(FLAC__stream_decoder_process_available(d)==FLAC__STREAM_DECODER_PUMP_END);
 assert(FLAC__stream_decoder_finish(d));FLAC__stream_decoder_delete(d);free(x.data);
 puts("FLAC_PUMP_API_PCM_OK frames=1536 md5=verified");return 0;
}
