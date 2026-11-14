#include "speex/speex_jitter.h"
#include <stdio.h>
#include <string.h>
int main(void) {
 JitterBuffer *j=jitter_buffer_init(10); int i;
 if (!j) return 1;
 for(i=0;i<80;i++) {
  char input[4]={(char)i,37,81,19}, output[4]={0};
  JitterBufferPacket in={input,4,(spx_uint32_t)(1000+i*10),10,(spx_uint16_t)i,(spx_uint32_t)(99+i)};
  JitterBufferPacket out={0}; spx_int32_t offset=-999;
  jitter_buffer_put(j,&in); input[0]=0;
  out.data=output;out.len=4;
  if(jitter_buffer_get(j,&out,10,&offset)!=JITTER_BUFFER_OK || output[0]!=(char)i || output[1]!=37 || out.timestamp!=1000u+(unsigned)i*10 || out.span!=10 || offset!=0 || out.sequence!=i || out.user_data!=99u+(unsigned)i) return 2;
  jitter_buffer_tick(j);
 }
 jitter_buffer_destroy(j); puts("JITTER_INORDER_COPY_OK frames=80");return 0;
}
