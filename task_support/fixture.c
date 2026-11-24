/* Ordinary coded nonzero MP3 fixture, not a granule decoder implementation.
   Huffman table 1 symbol (1,0) = 01 followed by sign. */
#include "task_support/fixture.h"
#include "stream.h"
#include "frame.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static void put(unsigned char *buf, unsigned *bit, unsigned n, unsigned value) {
  while (n--) {
    unsigned char mask=(unsigned char)(1u<<(7-(*bit&7u)));
    if ((value>>n)&1u) buf[*bit/8]|=mask;
    else buf[*bit/8]&=(unsigned char)~mask;
    ++*bit;
  }
}
size_t task_fixture(unsigned char *out, int lsf, int bad_second) {
  static const unsigned back[TASK_FRAMES]={0,80,35,95,50,110,65,90};
  unsigned char slots[TASK_FRAMES*381];
  unsigned frame_len=lsf?208:417, prefix=lsf?21:36, slot=frame_len-prefix;
  unsigned f,gr,ch,i,ngr=lsf?1:2;
  size_t length=TASK_FRAMES*frame_len;
  memset(out,0,length+MAD_BUFFER_GUARD);
  for(i=0;i<sizeof slots;++i) slots[i]=(unsigned char)(17u+53u*i);
  for(f=0;f<TASK_FRAMES;++f) {
    unsigned char *p=out+f*frame_len;unsigned bit=0, audio=0;
    p[0]=0xff;p[1]=lsf?0xf3:0xfb;p[2]=lsf?0x80:0x90;p[3]=0;
    put(p+4,&bit,lsf?8:9,back[f]);put(p+4,&bit,lsf?2:3,0);
    if(!lsf) put(p+4,&bit,8,0x84); /* left reuses group0; right group1 */
    for(gr=0;gr<ngr;++gr) for(ch=0;ch<2;++ch) {
      unsigned select=1; /* every granule uses a legal Huffman table */
      unsigned sfcount=lsf?0:(gr==0?21:(ch==0?15:16));
      put(p+4,&bit,12,sfcount+3-((bad_second && f==0 && gr==1 && ch==0)?1:0));put(p+4,&bit,9,1);
      put(p+4,&bit,8,211+5*ch+(f%3));put(p+4,&bit,lsf?9:4,lsf?0:5);
      put(p+4,&bit,1,0);put(p+4,&bit,5,select);
      put(p+4,&bit,5,0);put(p+4,&bit,5,0);
      put(p+4,&bit,4,0);put(p+4,&bit,3,0);put(p+4,&bit,lsf?2:3,0);
      if(!lsf) {
        unsigned sf;
        for(sf=0;sf<21;++sf) {
          if(gr==1 && ((ch==0 && sf<6)||(ch==1 && sf>=6 && sf<11))) continue;
          put(slots+f*slot-back[f],&audio,1,(sf+ch+f)&1u);
        }
      }
      put(slots+f*slot-back[f],&audio,2,1);
      put(slots+f*slot-back[f],&audio,1,(f+gr+2*ch+(f>>1))&1u);
    }
    if(bit!=(prefix-4)*8) abort();
  }
  for(f=0;f<TASK_FRAMES;++f) memcpy(out+f*frame_len+prefix,slots+f*slot,slot);
  return length;
}
int task_reference(const unsigned char *bytes,size_t length,int options,
                   mad_fixed_t out[2][TASK_MAX_SAMPLES],unsigned *samples,uint64_t *hash) {
  struct mad_stream stream;struct mad_frame frame;struct mad_synth synth;
  unsigned f,ch,i;int status=0;
  *samples=0;*hash=UINT64_C(1469598103934665603);
  mad_stream_init(&stream);mad_frame_init(&frame);mad_synth_init(&synth);
  mad_stream_options(&stream,options);
  mad_stream_buffer(&stream,bytes,(unsigned long)(length+MAD_BUFFER_GUARD));
  for(f=0;f<TASK_FRAMES;++f) {
    if(mad_frame_decode(&frame,&stream)) {
      fprintf(stderr,"reference frame=%u error=%04x\n",f,stream.error);status=1;break;
    }
    mad_synth_frame(&synth,&frame);
    for(i=0;i<synth.pcm.length;++i) for(ch=0;ch<2;++ch) {
      mad_fixed_t value=synth.pcm.samples[ch][i];out[ch][*samples+i]=value;
      *hash^=(uint32_t)value;*hash*=UINT64_C(1099511628211);
    }
    *samples+=synth.pcm.length;
  }
  mad_frame_finish(&frame);mad_stream_finish(&stream);return status;
}
