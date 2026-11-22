#include "aacenc_lib.h"
#include <cstdio>
#include <cstring>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"required ADTS seal consumer failed: %s\n",#x); return 1; } } while (0)
/* Exercise the public API on a real initialized legal encoder, with no pending AU. */
int main(void) {
 AACENC_ERROR (*seal)(HANDLE_AACENCODER,UCHAR*,UINT,AACENC_AdtsSealInfo*)=&aacEncSealAdts;
 REQUIRE(AACENC_OUTPUT_BUFFER_TOO_SMALL==0x0061);
 HANDLE_AACENCODER e=nullptr;REQUIRE(aacEncOpen(&e,0,2)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_AOT,2)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_SAMPLERATE,48000)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_CHANNELMODE,MODE_2)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_BITRATE,128000)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_TRANSMUX,TT_MP4_ADTS)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_TPSUBFRAMES,4)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_PROTECTION,0)==AACENC_OK);
 REQUIRE(aacEncEncode(e,nullptr,nullptr,nullptr,nullptr)==AACENC_OK);
 AACENC_AdtsSealInfo info;std::memset(&info,0xa5,sizeof(info));
 REQUIRE(seal(e,nullptr,0,&info)==AACENC_OK);
 REQUIRE(info.numAUs==0 && info.numBytes==0 && info.removedBits==0 && info.creditedBits==0);
 REQUIRE(info.bitResBefore==info.bitResAfter);
 REQUIRE(aacEncClose(&e)==AACENC_OK);
 std::puts("REQUIRED_ADTS_SEAL_CONSUMER_OK empty_query=1");return 0;
}
