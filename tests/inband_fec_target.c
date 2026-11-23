/********************************************************************
 *                                                                  *
 * inband_fec_target.c: target tests for libopusfile's single-      *
 * packet in-band FEC repair.                                       *
 *                                                                  *
 * The cases are registered through task_support/check.h (the       *
 * driver in task_support/driver.c supplies main()) and split into  *
 * three groups:                                                    *
 *  - granules:       Ogg hole/timestamp qualification decisions,   *
 *  - decode_history: libopus event-sequence reference and the      *
 *                    FEC/PLC/normal-decode distinctions,            *
 *  - consumer:       the four read entry points, fragmented reads, *
 *                    gain handling, and the unsupported ranges.    *
 *                                                                  *
 * Every damaged stream is muxed from independently retained Opus  *
 * packets, and every audio reference comes from an ordinary,       *
 * persistent libopus decoder driven with an explicit event         *
 * sequence, so nothing here relies on libopusfile internals.       *
 *                                                                  *
 ********************************************************************/

#include "task_support/check.h"

#include <opusfile.h>
#include <opus.h>
#include <ogg/ogg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TEST_NPACKETS   (40)
#define TEST_FRAME      (960)
#define TEST_DROP       (20)
#define TEST_ENDTRIM    (137)
#define TEST_MAXPACKET  (4000)
#define TEST_MAXCHAN    (2)

/*A growable byte buffer used to hold a complete muxed Ogg stream.*/
struct mbuf{
  unsigned char *d;
  size_t         n;
  size_t         cap;
};

static void mb_init(struct mbuf *_b){
  _b->d=NULL;
  _b->n=_b->cap=0;
}

static void mb_clear(struct mbuf *_b){
  free(_b->d);
  _b->d=NULL;
  _b->n=_b->cap=0;
}

static int mb_put(struct mbuf *_b,const void *_p,size_t _n){
  if(_b->n+_n>_b->cap){
    size_t cap;
    unsigned char *d;
    cap=_b->cap?2*_b->cap:4096;
    while(cap<_b->n+_n)cap*=2;
    d=(unsigned char *)realloc(_b->d,cap);
    if(d==NULL)return -1;
    _b->d=d;
    _b->cap=cap;
  }
  memcpy(_b->d+_b->n,_p,_n);
  _b->n+=_n;
  return 0;
}

static void put_le32(unsigned char *_p,opus_uint32 _v){
  int i;
  for(i=0;i<4;i++)_p[i]=(unsigned char)(_v>>(8*i));
}

static int is_dropped(const int *_drop,int _ndrop,int _index){
  int i;
  for(i=0;i<_ndrop;i++)if(_drop[i]==_index)return 1;
  return 0;
}

/*Parameters for one muxed logical link.*/
struct mux_link{
  long                 serial;
  int                  channel_count;
  int                  mapping_family;
  int                  stream_count;
  int                  coupled_count;
  const unsigned char *mapping;
  int                  npackets;
  const int           *lengths;
  const int           *durations;
  const unsigned char *packets;
  int                  preskip;
  /*Optional per-packet granule positions; NULL means cumulative duration.*/
  const ogg_int64_t   *granule_override;
  const int           *drop;
  int                  ndrop;
  int                  endtrim;
};

static void put_page(struct mbuf *_b,ogg_page *_pg){
  (void)mb_put(_b,_pg->header,(size_t)_pg->header_len);
  (void)mb_put(_b,_pg->body,(size_t)_pg->body_len);
}

static void flush_stream(ogg_stream_state *_os,struct mbuf *_b,
 const int *_drop,int _ndrop,int _packet_index){
  ogg_page pg;
  while(ogg_stream_flush(_os,&pg)){
    if(!is_dropped(_drop,_ndrop,_packet_index))put_page(_b,&pg);
  }
}

/*Build an OpusHead packet for the requested mapping.*/
static int make_head(unsigned char *_h,const struct mux_link *_l){
  int n;
  memset(_h,0,32);
  memcpy(_h,"OpusHead",8);
  _h[8]=1;
  _h[9]=(unsigned char)_l->channel_count;
  _h[10]=(unsigned char)(_l->preskip&255);
  _h[11]=(unsigned char)(_l->preskip>>8);
  put_le32(_h+12,48000);
  _h[18]=(unsigned char)_l->mapping_family;
  if(_l->mapping_family==0)return 19;
  _h[19]=(unsigned char)_l->stream_count;
  _h[20]=(unsigned char)_l->coupled_count;
  for(n=0;n<_l->channel_count;n++)_h[21+n]=_l->mapping[n];
  return 21+_l->channel_count;
}

/*A captured Ogg page plus the index of the audio packet it completes.*/
struct raw_page{
  unsigned char header[282];
  int           header_len;
  unsigned char body[2048];
  int           body_len;
  int           packet_index;
};

/*Capture every page produced by one link (one flushed page per packet).*/
static int capture_link_pages(const struct mux_link *_l,
 struct raw_page *_pages,int _page_cap,int *_page_count){
  ogg_stream_state os;
  ogg_packet       op;
  unsigned char    head[32];
  unsigned char    tags[16];
  int              head_len;
  int              k;
  int              npages;
  ogg_int64_t      gp;
  npages=0;
  if(ogg_stream_init(&os,_l->serial)!=0)return -1;
  head_len=make_head(head,_l);
  memset(&op,0,sizeof(op));
  op.packet=head;
  op.bytes=head_len;
  op.b_o_s=1;
  op.packetno=0;
  if(ogg_stream_packetin(&os,&op)!=0)goto fail;
  for(;;){
    ogg_page pg;
    if(ogg_stream_flush(&os,&pg)==0)break;
    if(npages>=_page_cap)goto fail;
    memcpy(_pages[npages].header,pg.header,pg.header_len);
    _pages[npages].header_len=pg.header_len;
    memcpy(_pages[npages].body,pg.body,pg.body_len);
    _pages[npages].body_len=pg.body_len;
    _pages[npages].packet_index=-1;
    npages++;
  }
  memset(tags,0,sizeof(tags));
  memcpy(tags,"OpusTags",8);
  op.packet=tags;
  op.bytes=(long)sizeof(tags);
  op.b_o_s=0;
  op.packetno=1;
  if(ogg_stream_packetin(&os,&op)!=0)goto fail;
  for(;;){
    ogg_page pg;
    if(ogg_stream_flush(&os,&pg)==0)break;
    if(npages>=_page_cap)goto fail;
    memcpy(_pages[npages].header,pg.header,pg.header_len);
    _pages[npages].header_len=pg.header_len;
    memcpy(_pages[npages].body,pg.body,pg.body_len);
    _pages[npages].body_len=pg.body_len;
    _pages[npages].packet_index=-1;
    npages++;
  }
  gp=0;
  for(k=0;k<_l->npackets;k++){
    ogg_page pg;
    memset(&op,0,sizeof(op));
    op.packet=(unsigned char *)(_l->packets+(size_t)k*TEST_MAXPACKET);
    op.bytes=_l->lengths[k];
    op.packetno=k+2;
    gp+=_l->durations[k];
    op.granulepos=gp;
    op.e_o_s=k==_l->npackets-1;
    if(op.e_o_s)op.granulepos-=_l->endtrim;
    if(ogg_stream_packetin(&os,&op)!=0)goto fail;
    if(ogg_stream_flush(&os,&pg)==0)goto fail;
    if(npages>=_page_cap)goto fail;
    memcpy(_pages[npages].header,pg.header,pg.header_len);
    _pages[npages].header_len=pg.header_len;
    memcpy(_pages[npages].body,pg.body,pg.body_len);
    _pages[npages].body_len=pg.body_len;
    _pages[npages].packet_index=k;
    npages++;
  }
  ogg_stream_clear(&os);
  *_page_count=npages;
  return 0;
fail:
  ogg_stream_clear(&os);
  return -1;
}

/*Mux a link in which the completion page of packet _drop is removed and a
   synthetic continued-packet-only page takes its sequence number.
  libogg therefore reports a real page-sequence hole on a page that carries no
   completed packet (its orphan body gets discarded), and the real successor
   page (renumbered to stay contiguous) is the first one with a completed
   packet.
  Every later page is renumbered and re-checksummed.*/
static int mux_link_synth_continued_hole(struct mbuf *_b,
 const struct mux_link *_l,int _drop){
  static struct raw_page pages[256];
  unsigned char synth_header[28];
  unsigned char synth_body[255];
  int npages;
  int pi;
  int drop_page;
  long serial;
  if(capture_link_pages(_l,pages,
   (int)(sizeof(pages)/sizeof(pages[0])),&npages)!=0)return -1;
  drop_page=-1;
  for(pi=0;pi<npages;pi++){
    if(pages[pi].packet_index==_drop){
      drop_page=pi;
      break;
    }
  }
  if(drop_page<0)return -1;
  {
    ogg_page dpg;
    dpg.header=pages[drop_page].header;
    dpg.header_len=pages[drop_page].header_len;
    dpg.body=pages[drop_page].body;
    dpg.body_len=pages[drop_page].body_len;
    serial=ogg_page_serialno(&dpg);
    /*Synthetic orphan page: continued flag, no granule, one unterminated
       255-byte segment, so no packet ever completes on it.
      It takes the sequence number right after the dropped page, making the
       dropped page itself the missing one; every real later page shifts up by
       one to stay contiguous.*/
    memset(synth_header,0,sizeof(synth_header));
    memcpy(synth_header,"OggS",4);
    synth_header[4]=0;
    synth_header[5]=1;
    memset(synth_header+6,0xFF,8);
    put_le32(synth_header+14,(opus_uint32)serial);
    put_le32(synth_header+18,(opus_uint32)(ogg_page_pageno(&dpg)+1));
  }
  synth_header[26]=1;
  synth_header[27]=255;
  memset(synth_body,0xAB,sizeof(synth_body));
  for(pi=0;pi<npages;pi++){
    ogg_page pg;
    if(pi==drop_page)continue;
    if(pi<drop_page){
      if(mb_put(_b,pages[pi].header,(size_t)pages[pi].header_len)<0||
       mb_put(_b,pages[pi].body,(size_t)pages[pi].body_len)<0)return -1;
      continue;
    }
    if(pi==drop_page+1){
      pg.header=synth_header;
      pg.header_len=(int)sizeof(synth_header);
      pg.body=synth_body;
      pg.body_len=(int)sizeof(synth_body);
      ogg_page_checksum_set(&pg);
      if(mb_put(_b,pg.header,(size_t)pg.header_len)<0||
       mb_put(_b,pg.body,(size_t)pg.body_len)<0)return -1;
    }
    /*Renumber every page after the dropped one to keep the page sequence
       contiguous, then fix the CRC.*/
    put_le32(pages[pi].header+18,
     (unsigned)(ogg_page_pageno(&(ogg_page){
      .header=pages[pi].header,
      .header_len=pages[pi].header_len,
      .body=pages[pi].body,
      .body_len=pages[pi].body_len})+1));
    pg.header=pages[pi].header;
    pg.header_len=pages[pi].header_len;
    pg.body=pages[pi].body;
    pg.body_len=pages[pi].body_len;
    ogg_page_checksum_set(&pg);
    if(mb_put(_b,pg.header,(size_t)pg.header_len)<0||
     mb_put(_b,pg.body,(size_t)pg.body_len)<0)return -1;
  }
  return 0;
}

/*Mux one link (head + tags + audio pages, one page per audio packet) into
   _b.
  Dropped packet indices simply never have their page written, which is
   exactly the page-sequence hole libopusfile has to recognize.*/
static int mux_link(struct mbuf *_b,const struct mux_link *_l){
  ogg_stream_state os;
  ogg_packet       op;
  unsigned char    head[32];
  unsigned char    tags[16];
  int              head_len;
  int              k;
  ogg_int64_t      gp;
  if(ogg_stream_init(&os,_l->serial)!=0)return -1;
  head_len=make_head(head,_l);
  memset(&op,0,sizeof(op));
  op.packet=head;
  op.bytes=head_len;
  op.b_o_s=1;
  op.packetno=0;
  if(ogg_stream_packetin(&os,&op)!=0)goto fail;
  flush_stream(&os,_b,NULL,0,-1);
  memset(tags,0,sizeof(tags));
  memcpy(tags,"OpusTags",8);
  op.packet=tags;
  op.bytes=(long)sizeof(tags);
  op.b_o_s=0;
  op.packetno=1;
  if(ogg_stream_packetin(&os,&op)!=0)goto fail;
  flush_stream(&os,_b,NULL,0,-1);
  gp=0;
  for(k=0;k<_l->npackets;k++){
    memset(&op,0,sizeof(op));
    op.packet=(unsigned char *)(_l->packets+(size_t)k*TEST_MAXPACKET);
    op.bytes=_l->lengths[k];
    op.packetno=k+2;
    if(_l->granule_override!=NULL)op.granulepos=_l->granule_override[k];
    else{
      gp+=_l->durations[k];
      op.granulepos=gp;
    }
    op.e_o_s=k==_l->npackets-1;
    if(op.e_o_s&&_l->granule_override==NULL)op.granulepos-=_l->endtrim;
    if(ogg_stream_packetin(&os,&op)!=0)goto fail;
    flush_stream(&os,_b,_l->drop,_l->ndrop,k);
  }
  ogg_stream_clear(&os);
  return 0;
fail:
  ogg_stream_clear(&os);
  return -1;
}

/*A non-seekable memory stream plus its libopusfile callbacks.*/
struct mem_stream{
  const unsigned char *data;
  size_t               size;
  size_t               pos;
};

static int mem_read(void *_stream,unsigned char *_ptr,int _n){
  struct mem_stream *ms;
  ms=(struct mem_stream *)_stream;
  if(_n>(int)(ms->size-ms->pos))_n=(int)(ms->size-ms->pos);
  if(_n<=0)return 0;
  memcpy(_ptr,ms->data+ms->pos,(size_t)_n);
  ms->pos+=(size_t)_n;
  return _n;
}

/*Encode TEST_NPACKETS independent 20 ms packets of a distinctive tone mix.*/
struct encoded{
  int           lengths[TEST_NPACKETS];
  unsigned char packets[TEST_NPACKETS][TEST_MAXPACKET];
  int           durations[TEST_NPACKETS];
  int           preskip;
};

static int encode_packets(struct encoded *_e,int _channels,int _use_fec,
 unsigned _seed){
  OpusEncoder *enc;
  int          err;
  int          k;
  int          i;
  int          c;
  float        in[TEST_FRAME*TEST_MAXCHAN];
  enc=opus_encoder_create(48000,_channels,OPUS_APPLICATION_VOIP,&err);
  if(enc==NULL)return -1;
  if(opus_encoder_ctl(enc,OPUS_SET_BITRATE(36000))!=0)goto fail;
  if(opus_encoder_ctl(enc,OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND))
   !=0)goto fail;
  if(opus_encoder_ctl(enc,OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE))!=0)goto fail;
  if(opus_encoder_ctl(enc,OPUS_SET_INBAND_FEC(_use_fec?1:0))!=0)goto fail;
  if(_use_fec&&
   opus_encoder_ctl(enc,OPUS_SET_PACKET_LOSS_PERC(30))!=0)goto fail;
  if(opus_encoder_ctl(enc,OPUS_GET_LOOKAHEAD(&_e->preskip))!=0)goto fail;
  for(k=0;k<TEST_NPACKETS;k++){
    for(i=0;i<TEST_FRAME;i++){
      double t;
      t=(k*TEST_FRAME+i)/48000.0;
      for(c=0;c<_channels;c++){
        in[i*_channels+c]=(float)(
         0.22*sin((117.0+31.0*c+_seed)*2.0*M_PI*t)*
          (0.75+0.2*sin(2.9*2.0*M_PI*t))+
         0.07*sin((421.0+37.0*c+_seed)*2.0*M_PI*t));
      }
    }
    _e->lengths[k]=opus_encode_float(enc,in,TEST_FRAME,
     _e->packets[k],TEST_MAXPACKET);
    if(_e->lengths[k]<=0)goto fail;
    _e->durations[k]=TEST_FRAME;
  }
  opus_encoder_destroy(enc);
  return 0;
fail:
  opus_encoder_destroy(enc);
  return -1;
}

/*Drain an OggOpusFile with op_read_float() using the given per-call buffer
   size (in samples per channel).
  Returns the number of frames delivered, counts OP_HOLE reports, and copies
   every sample into a freshly allocated interleaved buffer.*/
static int drain_float(OggOpusFile *_of,int _channels,int _chunk,
 float **_out,long *_frames,int *_holes){
  size_t cap;
  long   frames;
  int    holes;
  float *out;
  int    li;
  cap=4096;
  out=(float *)malloc(cap*sizeof(float));
  if(out==NULL)return -1;
  frames=0;
  holes=0;
  for(;;){
    float chunk[120*48*TEST_MAXCHAN];
    int   n;
    int   want;
    want=_chunk;
    if(want>(int)(sizeof(chunk)/sizeof(*chunk)/_channels)){
      want=(int)(sizeof(chunk)/sizeof(*chunk)/_channels);
    }
    n=op_read_float(_of,chunk,want*_channels,&li);
    if(n==OP_HOLE){
      holes++;
      continue;
    }
    if(n<0){
      free(out);
      return n;
    }
    if(n==0)break;
    if((size_t)(frames+n)*_channels>cap){
      while((size_t)(frames+n)*_channels>cap)cap*=2;
      out=(float *)realloc(out,cap*sizeof(float));
      if(out==NULL)return -1;
    }
    memcpy(out+frames*_channels,chunk,
     sizeof(float)*_channels*(size_t)n);
    frames+=n;
  }
  *_out=out;
  *_frames=frames;
  if(_holes!=NULL)*_holes=holes;
  return 0;
}

/*Drive an ordinary persistent libopus decoder with the exact event sequence
   libopusfile uses for a single repaired hole:
     normal decodes up to _drop-1,
     decode_fec=1 on the successor packet (FEC, or natural PLC fallback),
     decode_fec=0 on that same successor packet,
     normal decodes for the rest.
  Each output window is stored separately in _base (one window every
   _stride floats), so the caller can compare individual events.*/
static int reference_event_sequence(const struct encoded *_e,int _channels,
 int _drop,int _fec_call,float *_base,int _stride,int *_nwindows){
  OpusDecoder *dec;
  int          err;
  int          k;
  int          w;
  dec=opus_decoder_create(48000,_channels,&err);
  if(dec==NULL)return -1;
  w=0;
  for(k=0;k<_drop;k++){
    if(opus_decode_float(dec,_e->packets[k],_e->lengths[k],
     _base+(size_t)w*_stride,TEST_FRAME,0)!=TEST_FRAME)goto fail;
    w++;
  }
  if(opus_decode_float(dec,_e->packets[_drop+1],_e->lengths[_drop+1],
   _base+(size_t)w*_stride,TEST_FRAME,_fec_call)!=TEST_FRAME)goto fail;
  w++;
  if(opus_decode_float(dec,_e->packets[_drop+1],_e->lengths[_drop+1],
   _base+(size_t)w*_stride,TEST_FRAME,0)!=TEST_FRAME)goto fail;
  w++;
  for(k=_drop+2;k<TEST_NPACKETS;k++){
    if(opus_decode_float(dec,_e->packets[k],_e->lengths[k],
     _base+(size_t)w*_stride,TEST_FRAME,0)!=TEST_FRAME)goto fail;
    w++;
  }
  opus_decoder_destroy(dec);
  *_nwindows=w;
  return 0;
fail:
  opus_decoder_destroy(dec);
  return -1;
}

/*Flatten reference windows into delivered PCM by applying pre-skip at the
   front and end trimming at the back, exactly like the Ogg stream.*/
static int flatten_reference(int _channels,int _preskip,int _endtrim,
 const float *_base,int _stride,int _nwindows,float **_out,long *_frames){
  long total;
  long out_frames;
  long i;
  float *out;
  total=(long)_nwindows*TEST_FRAME;
  out_frames=total-_preskip-_endtrim;
  if(out_frames<=0)return -1;
  out=(float *)malloc((size_t)out_frames*_channels*sizeof(float));
  if(out==NULL)return -1;
  for(i=0;i<out_frames*_channels;i++){
    long flat;
    int  wi;
    int  fi;
    flat=_preskip*_channels+i;
    wi=(int)(flat/(TEST_FRAME*_channels));
    fi=(int)(flat%(TEST_FRAME*_channels));
    out[i]=_base[(size_t)wi*_stride+fi];
  }
  *_out=out;
  *_frames=out_frames;
  return 0;
}

static int open_unseekable(const struct mbuf *_b,
 struct mem_stream *_ms,OpusFileCallbacks *_cb,OggOpusFile **_of){
  _ms->data=_b->d;
  _ms->size=_b->n;
  _ms->pos=0;
  memset(_cb,0,sizeof(*_cb));
  _cb->read=mem_read;
  *_of=op_open_callbacks(_ms,_cb,NULL,0,&(int){0});
  return *_of!=NULL;
}

static int count_diff_samples(const float *_a,const float *_b,long _n,
 float _eps){
  long i;
  int  count;
  count=0;
  for(i=0;i<_n;i++)if(fabsf(_a[i]-_b[i])>_eps)count++;
  return count;
}

static int count_nonzero(const float *_a,long _n){
  long i;
  int  count;
  count=0;
  for(i=0;i<_n;i++)if(_a[i]!=0.0f)count++;
  return count;
}

static long expected_frames(const struct encoded *_e){
  return (long)TEST_NPACKETS*TEST_FRAME-_e->preskip-TEST_ENDTRIM;
}

/*Build the standard single-hole stereo fixture (one page dropped at
   TEST_DROP).*/
static int build_hole_fixture(struct mbuf *_b,const struct encoded *_e,
 const int *_drop,int _ndrop,const ogg_int64_t *_gp_override){
  static const unsigned char map0[2]={0,1};
  struct mux_link l;
  memset(&l,0,sizeof(l));
  l.serial=246841;
  l.channel_count=2;
  l.mapping_family=0;
  l.stream_count=1;
  l.coupled_count=1;
  l.mapping=map0;
  l.npackets=TEST_NPACKETS;
  l.lengths=_e->lengths;
  l.durations=_e->durations;
  l.packets=(const unsigned char *)_e->packets;
  l.preskip=_e->preskip;
  l.granule_override=_gp_override;
  l.drop=_drop;
  l.ndrop=_ndrop;
  l.endtrim=TEST_ENDTRIM;
  mb_init(_b);
  return mux_link(_b,&l);
}

/********************************************************************
 * granules group: hole qualification and timestamp decisions       *
 ********************************************************************/

static int case_single_hole_repairs(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *pcm;
  long   frames;
  int    holes;
  long   expected;
  long   total;
  long   prev_tell;
  int    li;
  TASK_CHECK(encode_packets(&e,2,1,0)==0);
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,NULL)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(!op_seekable(of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,TEST_FRAME,&pcm,&frames,&holes)==0);
  expected=expected_frames(&e);
  TASK_CHECK(holes==0);
  TASK_CHECK(frames==expected);
  /*op_pcm_tell must track exactly what was delivered, even when the reader
     pulls a single frame at a time across the repaired window.*/
  op_free(of);
  free(pcm);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  total=0;
  prev_tell=-1;
  for(;;){
    float one[2];
    int   n;
    /*Exactly one stereo frame per call, straddling the repaired window.*/
    n=op_read_float(of,one,2,&li);
    TASK_CHECK(n>=0);
    if(n==0)break;
    total+=n;
    TASK_CHECK(op_pcm_tell(of)==total);
    TASK_CHECK(prev_tell<0||op_pcm_tell(of)>prev_tell);
    prev_tell=op_pcm_tell(of);
  }
  TASK_CHECK(total==expected);
  TASK_CHECK(op_pcm_tell(of)==expected);
  op_free(of);
  mb_clear(&b);
  return 0;
}

static int case_multi_packet_hole(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[2]={TEST_DROP,TEST_DROP+1};
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *pcm;
  long   frames;
  int    holes;
  TASK_CHECK(encode_packets(&e,2,1,1)==0);
  TASK_CHECK(build_hole_fixture(&b,&e,drop,2,NULL)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,512,&pcm,&frames,&holes)==0);
  /*Two lost packets can never be proven to be one 20 ms window: keep the
     legacy OP_HOLE behavior and deliver less audio.*/
  TASK_CHECK(holes>=1);
  TASK_CHECK(frames<expected_frames(&e));
  op_free(of);
  free(pcm);
  mb_clear(&b);
  return 0;
}

static int case_eos_right_page(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_NPACKETS-2};
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *pcm;
  long   frames;
  int    holes;
  TASK_CHECK(encode_packets(&e,2,1,2)==0);
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,NULL)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,512,&pcm,&frames,&holes)==0);
  /*The first page after the hole is the EOS page: no repair is attempted.*/
  TASK_CHECK(holes>=1);
  TASK_CHECK(frames<expected_frames(&e));
  op_free(of);
  free(pcm);
  mb_clear(&b);
  return 0;
}

static int case_missing_timestamp(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  ogg_int64_t gp[TEST_NPACKETS];
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *pcm;
  long   frames;
  int    holes;
  int    k;
  TASK_CHECK(encode_packets(&e,2,1,3)==0);
  for(k=0;k<TEST_NPACKETS;k++)gp[k]=(ogg_int64_t)(k+1)*TEST_FRAME;
  /*The explicit override bypasses the muxer's automatic end trim.*/
  gp[TEST_NPACKETS-1]-=TEST_ENDTRIM;
  /*No determinable timestamp on the first page after the hole.*/
  gp[TEST_DROP+1]=-1;
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,gp)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,512,&pcm,&frames,&holes)==0);
  TASK_CHECK(holes>=1);
  TASK_CHECK(frames<expected_frames(&e));
  op_free(of);
  free(pcm);
  mb_clear(&b);
  return 0;
}

static int case_non_20ms_page(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  static const unsigned char map0[2]={0,1};
  struct mux_link l;
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  OpusEncoder *enc;
  float         in[2880*2];
  float        *pcm;
  long          frames;
  int           holes;
  int           err;
  int           i;
  int           c;
  TASK_CHECK(encode_packets(&e,2,1,4)==0);
  /*Replace the packet right after the hole with a 60 ms packet.*/
  enc=opus_encoder_create(48000,2,OPUS_APPLICATION_VOIP,&err);
  TASK_CHECK(enc!=NULL);
  TASK_CHECK(opus_encoder_ctl(enc,OPUS_SET_INBAND_FEC(1))==0);
  TASK_CHECK(opus_encoder_ctl(enc,OPUS_SET_PACKET_LOSS_PERC(30))==0);
  for(i=0;i<2880;i++)for(c=0;c<2;c++){
    double t;
    t=i/48000.0;
    in[i*2+c]=(float)(0.2*sin((150.0+30.0*c)*2.0*M_PI*t));
  }
  e.lengths[TEST_DROP+1]=opus_encode_float(enc,in,2880,
   e.packets[TEST_DROP+1],TEST_MAXPACKET);
  opus_encoder_destroy(enc);
  TASK_CHECK(e.lengths[TEST_DROP+1]>0);
  e.durations[TEST_DROP+1]=2880;
  memset(&l,0,sizeof(l));
  l.serial=246841;
  l.channel_count=2;
  l.mapping_family=0;
  l.stream_count=1;
  l.coupled_count=1;
  l.mapping=map0;
  l.npackets=TEST_NPACKETS;
  l.lengths=e.lengths;
  l.durations=e.durations;
  l.packets=(const unsigned char *)e.packets;
  l.preskip=e.preskip;
  l.drop=drop;
  l.ndrop=1;
  l.endtrim=TEST_ENDTRIM;
  mb_init(&b);
  TASK_CHECK(mux_link(&b,&l)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,512,&pcm,&frames,&holes)==0);
  /*A page whose completed packets are not all 20 ms never qualifies.*/
  TASK_CHECK(holes>=1);
  op_free(of);
  free(pcm);
  mb_clear(&b);
  return 0;
}

static int case_timestamp_jump_without_hole(void){
  struct encoded e;
  struct mbuf    b_clean;
  struct mbuf    b_jump;
  ogg_int64_t gp[TEST_NPACKETS];
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *pcm_clean;
  float *pcm_jump;
  long   frames_clean;
  long   frames_jump;
  int    holes;
  int    k;
  TASK_CHECK(encode_packets(&e,2,1,5)==0);
  TASK_CHECK(build_hole_fixture(&b_clean,&e,NULL,0,NULL)==0);
  for(k=0;k<TEST_NPACKETS;k++)gp[k]=(ogg_int64_t)(k+1)*TEST_FRAME;
  /*The explicit override bypasses the muxer's automatic end trim.*/
  gp[TEST_NPACKETS-1]-=TEST_ENDTRIM;
  /*Inflate a timestamp without dropping any page: there is no sequence hole,
     so nothing may be repaired and the PCM must stay identical.*/
  gp[TEST_DROP]+=TEST_FRAME;
  TASK_CHECK(build_hole_fixture(&b_jump,&e,NULL,0,gp)==0);
  TASK_CHECK(open_unseekable(&b_clean,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,512,&pcm_clean,&frames_clean,&holes)==0);
  TASK_CHECK(holes==0);
  op_free(of);
  TASK_CHECK(open_unseekable(&b_jump,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,512,&pcm_jump,&frames_jump,&holes)==0);
  TASK_CHECK(holes==0);
  TASK_CHECK(frames_jump==frames_clean);
  TASK_CHECK(memcmp(pcm_clean,pcm_jump,
   (size_t)frames_clean*2*sizeof(float))==0);
  op_free(of);
  free(pcm_clean);
  free(pcm_jump);
  mb_clear(&b_clean);
  mb_clear(&b_jump);
  return 0;
}

static int case_gap_size_mismatch(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  ogg_int64_t gp[TEST_NPACKETS];
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *pcm;
  long   frames;
  int    holes;
  int    k;
  TASK_CHECK(encode_packets(&e,2,1,6)==0);
  for(k=0;k<TEST_NPACKETS;k++)gp[k]=(ogg_int64_t)(k+1)*TEST_FRAME;
  /*The explicit override bypasses the muxer's automatic end trim.*/
  gp[TEST_NPACKETS-1]-=TEST_ENDTRIM;
  /*Make the proven gap two 20 ms windows instead of one; the single-packet
     repair must refuse and leave the hole to the legacy path.*/
  for(k=TEST_DROP+1;k<TEST_NPACKETS;k++)gp[k]+=TEST_FRAME;
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,gp)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,512,&pcm,&frames,&holes)==0);
  TASK_CHECK(holes>=1);
  TASK_CHECK(frames<expected_frames(&e));
  op_free(of);
  free(pcm);
  mb_clear(&b);
  return 0;
}

static int case_hole_on_continued_only_page(void){
  struct encoded e;
  static const unsigned char map0[2]={0,1};
  struct mux_link l;
  struct mbuf    b;
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_packet      oop;
  ogg_page        opg;
  float *pcm;
  long   frames;
  int    holes;
  int    saw_continued_only;
  int    saw_stream_hole;
  int    got_first_page;
  TASK_CHECK(encode_packets(&e,2,1,22)==0);
  memset(&l,0,sizeof(l));
  l.serial=246841;
  l.channel_count=2;
  l.mapping_family=0;
  l.stream_count=1;
  l.coupled_count=1;
  l.mapping=map0;
  l.npackets=TEST_NPACKETS;
  l.lengths=e.lengths;
  l.durations=e.durations;
  l.packets=(const unsigned char *)e.packets;
  l.preskip=e.preskip;
  l.endtrim=TEST_ENDTRIM;
  mb_init(&b);
  TASK_CHECK(mux_link_synth_continued_hole(&b,&l,TEST_DROP)==0);
  /*Independently verify the framing: a continued-only page must exist, and
     libogg's stream layer must report a real packet hole.*/
  saw_continued_only=0;
  saw_stream_hole=0;
  got_first_page=0;
  TASK_CHECK(ogg_sync_init(&oy)==0);
  {
    char *ob;
    ob=ogg_sync_buffer(&oy,(long)b.n);
    TASK_CHECK(ob!=NULL);
    memcpy(ob,b.d,b.n);
    TASK_CHECK(ogg_sync_wrote(&oy,(long)b.n)==0);
  }
  for(;;){
    int pr;
    pr=ogg_sync_pageout(&oy,&opg);
    if(pr<=0)break;
    if(ogg_page_continued(&opg)&&ogg_page_packets(&opg)==0){
      saw_continued_only=1;
    }
    if(!got_first_page){
      TASK_CHECK(ogg_stream_init(&os,ogg_page_serialno(&opg))==0);
      got_first_page=1;
    }
    if((ogg_uint32_t)ogg_page_serialno(&opg)==
     (ogg_uint32_t)l.serial){
      ogg_stream_pagein(&os,&opg);
      if(ogg_stream_packetout(&os,&oop)<0)saw_stream_hole=1;
    }
  }
  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  TASK_CHECK(saw_continued_only);
  TASK_CHECK(saw_stream_hole);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,512,&pcm,&frames,&holes)==0);
  /*Even though the hole marker arrives on a page without completed packets,
     the first completing page still proves one lost 20 ms packet, so the
     repair succeeds and no OP_HOLE is ever surfaced.*/
  TASK_CHECK(holes==0);
  TASK_CHECK(frames==expected_frames(&e));
  op_free(of);
  free(pcm);
  mb_clear(&b);
  return 0;
}

/********************************************************************
 * decode_history group: reference event sequences                  *
 ********************************************************************/

static int case_fec_distinct_from_plc_and_normal(void){
  struct encoded e;
  OpusDecoder *fec_dec;
  OpusDecoder *plc_dec;
  OpusDecoder *normal_dec;
  float fec_win[TEST_FRAME*2];
  float plc_win[TEST_FRAME*2];
  float normal_win[TEST_FRAME*2];
  float tmp[TEST_FRAME*2];
  int   err;
  int   k;
  TASK_CHECK(encode_packets(&e,2,1,7)==0);
  fec_dec=opus_decoder_create(48000,2,&err);
  plc_dec=opus_decoder_create(48000,2,&err);
  normal_dec=opus_decoder_create(48000,2,&err);
  TASK_CHECK(fec_dec!=NULL&&plc_dec!=NULL&&normal_dec!=NULL);
  /*All three decoders share the same initial state and packet history up to
     the loss.*/
  for(k=0;k<TEST_DROP;k++){
    TASK_CHECK(opus_decode_float(fec_dec,e.packets[k],e.lengths[k],
     tmp,TEST_FRAME,0)==TEST_FRAME);
    TASK_CHECK(opus_decode_float(plc_dec,e.packets[k],e.lengths[k],
     tmp,TEST_FRAME,0)==TEST_FRAME);
    TASK_CHECK(opus_decode_float(normal_dec,e.packets[k],e.lengths[k],
     tmp,TEST_FRAME,0)==TEST_FRAME);
  }
  /*The actual FEC event...*/
  TASK_CHECK(opus_decode_float(fec_dec,e.packets[TEST_DROP+1],
   e.lengths[TEST_DROP+1],fec_win,TEST_FRAME,1)==TEST_FRAME);
  /*...plain PLC from the same state...*/
  TASK_CHECK(opus_decode_float(plc_dec,NULL,0,plc_win,TEST_FRAME,0)
   ==TEST_FRAME);
  /*...and an ordinary decode of the successor packet.*/
  TASK_CHECK(opus_decode_float(normal_dec,e.packets[TEST_DROP+1],
   e.lengths[TEST_DROP+1],normal_win,TEST_FRAME,0)==TEST_FRAME);
  TASK_CHECK(count_nonzero(fec_win,TEST_FRAME*2)>0);
  TASK_CHECK(count_diff_samples(fec_win,plc_win,TEST_FRAME*2,1e-7f)>0);
  TASK_CHECK(count_diff_samples(fec_win,normal_win,TEST_FRAME*2,1e-7f)>0);
  opus_decoder_destroy(fec_dec);
  opus_decoder_destroy(plc_dec);
  opus_decoder_destroy(normal_dec);
  return 0;
}

static int case_repaired_matches_event_sequence(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  static float windows[(TEST_NPACKETS+1)*TEST_FRAME*2];
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *got;
  float *ref;
  long   got_frames;
  long   ref_frames;
  int    holes;
  int    nwindows;
  TASK_CHECK(encode_packets(&e,2,1,8)==0);
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,NULL)==0);
  TASK_CHECK(reference_event_sequence(&e,2,TEST_DROP,1,windows,
   TEST_FRAME*2,&nwindows)==0);
  /*The FEC window replaces the one lost packet, so the repaired stream has
     the same window count as the original.*/
  TASK_CHECK(nwindows==TEST_NPACKETS);
  TASK_CHECK(flatten_reference(2,e.preskip,TEST_ENDTRIM,windows,
   TEST_FRAME*2,nwindows,&ref,&ref_frames)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,512,&got,&got_frames,&holes)==0);
  TASK_CHECK(holes==0);
  TASK_CHECK(got_frames==ref_frames);
  TASK_CHECK(memcmp(got,ref,(size_t)ref_frames*2*sizeof(float))==0);
  op_free(of);
  free(got);
  free(ref);
  mb_clear(&b);
  return 0;
}

static int case_successor_consumed_once(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  static float windows[(TEST_NPACKETS+1)*TEST_FRAME*2];
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *got;
  float *ref;
  long   got_frames;
  long   ref_frames;
  int    holes;
  int    nwindows;
  long   off;
  TASK_CHECK(encode_packets(&e,2,1,9)==0);
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,NULL)==0);
  TASK_CHECK(reference_event_sequence(&e,2,TEST_DROP,1,windows,
   TEST_FRAME*2,&nwindows)==0);
  TASK_CHECK(flatten_reference(2,e.preskip,TEST_ENDTRIM,windows,
   TEST_FRAME*2,nwindows,&ref,&ref_frames)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  /*One frame per call: neither the FEC pass nor the successor packet may be
     replayed no matter how small the reads are.*/
  TASK_CHECK(drain_float(of,2,1,&got,&got_frames,&holes)==0);
  TASK_CHECK(got_frames==ref_frames);
  TASK_CHECK(memcmp(got,ref,(size_t)ref_frames*2*sizeof(float))==0);
  /*The frame right after the repaired window is the ordinary successor
     decode, not another copy of the repair.*/
  off=(long)(TEST_DROP*TEST_FRAME-e.preskip+TEST_FRAME)*2;
  TASK_CHECK(off>0&&off+TEST_FRAME*2<=got_frames*2);
  TASK_CHECK(count_diff_samples(got+off,
   windows+(TEST_DROP+1)*TEST_FRAME*2,TEST_FRAME*2,0.0f)==0);
  op_free(of);
  free(got);
  free(ref);
  mb_clear(&b);
  return 0;
}

static int case_fallback_is_plc_not_silence(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  static float fec_windows[(TEST_NPACKETS+1)*TEST_FRAME*2];
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  OpusDecoder *plc_dec;
  float        plc_window[TEST_FRAME*2];
  float *got;
  float *ref;
  long   got_frames;
  long   ref_frames;
  int    holes;
  int    nfec;
  int    err;
  int    k;
  TASK_CHECK(encode_packets(&e,2,0,10)==0);
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,NULL)==0);
  /*With no redundancy encoded, decode_fec=1 naturally degrades to PLC.
    The event sequence on a persistent decoder still produces a complete
     stream, which the library output must match sample for sample.*/
  TASK_CHECK(reference_event_sequence(&e,2,TEST_DROP,1,fec_windows,
   TEST_FRAME*2,&nfec)==0);
  TASK_CHECK(nfec==TEST_NPACKETS);
  TASK_CHECK(count_nonzero(fec_windows+TEST_DROP*TEST_FRAME*2,
   TEST_FRAME*2)>0);
  TASK_CHECK(flatten_reference(2,e.preskip,TEST_ENDTRIM,fec_windows,
   TEST_FRAME*2,nfec,&ref,&ref_frames)==0);
  /*An independent decoder with identical history, asked for explicit PLC for
     the lost window, must produce exactly the same concealment samples.
    This proves the library did not invent silence or anything else.*/
  plc_dec=opus_decoder_create(48000,2,&err);
  TASK_CHECK(plc_dec!=NULL);
  for(k=0;k<TEST_DROP;k++){
    TASK_CHECK(opus_decode_float(plc_dec,e.packets[k],e.lengths[k],
     plc_window,TEST_FRAME,0)==TEST_FRAME);
  }
  TASK_CHECK(opus_decode_float(plc_dec,NULL,0,plc_window,TEST_FRAME,0)
   ==TEST_FRAME);
  TASK_CHECK(count_nonzero(plc_window,TEST_FRAME*2)>0);
  TASK_CHECK(memcmp(plc_window,fec_windows+TEST_DROP*TEST_FRAME*2,
   sizeof(plc_window))==0);
  opus_decoder_destroy(plc_dec);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,512,&got,&got_frames,&holes)==0);
  TASK_CHECK(holes==0);
  TASK_CHECK(got_frames==ref_frames);
  TASK_CHECK(memcmp(got,ref,(size_t)ref_frames*2*sizeof(float))==0);
  op_free(of);
  free(got);
  free(ref);
  mb_clear(&b);
  return 0;
}

static int case_gain_applied_to_repair(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  static float windows[(TEST_NPACKETS+1)*TEST_FRAME*2];
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  OpusDecoder *ref_dec;
  float *got;
  long   got_frames;
  int    holes;
  int    err;
  int    k;
  int    w;
  TASK_CHECK(encode_packets(&e,2,1,11)==0);
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,NULL)==0);
  /*Reference decoder with the same absolute gain applied to every event,
     including the reconstructed window.*/
  ref_dec=opus_decoder_create(48000,2,&err);
  TASK_CHECK(ref_dec!=NULL);
  TASK_CHECK(opus_decoder_ctl(ref_dec,OPUS_SET_GAIN(384))==0);
  w=0;
  for(k=0;k<TEST_DROP;k++){
    TASK_CHECK(opus_decode_float(ref_dec,e.packets[k],e.lengths[k],
     windows+w*TEST_FRAME*2,TEST_FRAME,0)==TEST_FRAME);
    w++;
  }
  TASK_CHECK(opus_decode_float(ref_dec,e.packets[TEST_DROP+1],
   e.lengths[TEST_DROP+1],windows+w*TEST_FRAME*2,TEST_FRAME,1)
   ==TEST_FRAME);
  w++;
  TASK_CHECK(opus_decode_float(ref_dec,e.packets[TEST_DROP+1],
   e.lengths[TEST_DROP+1],windows+w*TEST_FRAME*2,TEST_FRAME,0)
   ==TEST_FRAME);
  w++;
  for(k=TEST_DROP+2;k<TEST_NPACKETS;k++){
    TASK_CHECK(opus_decode_float(ref_dec,e.packets[k],e.lengths[k],
     windows+w*TEST_FRAME*2,TEST_FRAME,0)==TEST_FRAME);
    w++;
  }
  opus_decoder_destroy(ref_dec);
  TASK_CHECK(w==TEST_NPACKETS);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(op_set_gain_offset(of,OP_ABSOLUTE_GAIN,384)==0);
  TASK_CHECK(drain_float(of,2,512,&got,&got_frames,&holes)==0);
  TASK_CHECK(holes==0);
  TASK_CHECK(got_frames==expected_frames(&e));
  {
    float *ref;
    long   ref_frames;
    TASK_CHECK(flatten_reference(2,e.preskip,TEST_ENDTRIM,windows,
     TEST_FRAME*2,w,&ref,&ref_frames)==0);
    TASK_CHECK(got_frames==ref_frames);
    TASK_CHECK(memcmp(got,ref,(size_t)ref_frames*2*sizeof(float))==0);
    free(ref);
  }
  op_free(of);
  free(got);
  mb_clear(&b);
  return 0;
}

static int case_chain_boundary(void){
  struct encoded e1;
  struct encoded e2;
  struct mbuf    b;
  static const int drop1[1]={TEST_NPACKETS-2};
  static const int drop2[1]={5};
  static const unsigned char map0[2]={0,1};
  struct mux_link l[2];
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  long   frames;
  int    holes;
  int    li;
  int    saw_link1;
  int    saw_link2;
  TASK_CHECK(encode_packets(&e1,2,1,12)==0);
  TASK_CHECK(encode_packets(&e2,2,1,13)==0);
  memset(l,0,sizeof(l));
  /*Link 1 loses its penultimate page, so the first page after that hole is
     the link's EOS page and cannot be repaired.*/
  l[0].serial=111;
  l[0].channel_count=2;
  l[0].mapping_family=0;
  l[0].stream_count=1;
  l[0].coupled_count=1;
  l[0].mapping=map0;
  l[0].npackets=TEST_NPACKETS;
  l[0].lengths=e1.lengths;
  l[0].durations=e1.durations;
  l[0].packets=(const unsigned char *)e1.packets;
  l[0].preskip=e1.preskip;
  l[0].drop=drop1;
  l[0].ndrop=1;
  l[0].endtrim=TEST_ENDTRIM;
  /*Link 2 has its own ordinary single-packet hole, which must repair using
     only link 2 state.*/
  l[1].serial=222;
  l[1].channel_count=2;
  l[1].mapping_family=0;
  l[1].stream_count=1;
  l[1].coupled_count=1;
  l[1].mapping=map0;
  l[1].npackets=TEST_NPACKETS;
  l[1].lengths=e2.lengths;
  l[1].durations=e2.durations;
  l[1].packets=(const unsigned char *)e2.packets;
  l[1].preskip=e2.preskip;
  l[1].drop=drop2;
  l[1].ndrop=1;
  l[1].endtrim=TEST_ENDTRIM;
  mb_init(&b);
  TASK_CHECK(mux_link(&b,&l[0])==0);
  TASK_CHECK(mux_link(&b,&l[1])==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  saw_link1=saw_link2=0;
  frames=0;
  holes=0;
  {
    long within;
    int  prev_li;
    within=0;
    prev_li=0;
    for(;;){
      float chunk[512*2];
      int   n;
      n=op_read_float(of,chunk,512*2,&li);
      if(n==OP_HOLE){
        holes++;
        continue;
      }
      TASK_CHECK(n>=0);
      if(n==0)break;
      if(li!=prev_li){
        /*op_pcm_tell is relative to the current link, whose offset restarts
           at zero on unseekable streams.*/
        within=0;
        prev_li=li;
      }
      if(li==0)saw_link1=1;
      if(li==1){
        /*Link 2's repaired hole means it delivers its full frame count, and a
           pending repair from link 1 could never have survived the switch.*/
        saw_link2=1;
        TASK_CHECK(holes>=1);
      }
      frames+=n;
      within+=n;
      TASK_CHECK(op_pcm_tell(of)==within);
    }
    /*Link 2 repaired its own hole, so it delivers the full intact length.*/
    TASK_CHECK(within==expected_frames(&e2));
  }
  TASK_CHECK(saw_link1&&saw_link2);
  TASK_CHECK(op_current_link(of)==1);
  TASK_CHECK(frames>expected_frames(&e2));
  op_free(of);
  mb_clear(&b);
  return 0;
}

/********************************************************************
 * consumer group: read entries, chunking, gain, unsupported range  *
 ********************************************************************/

static int case_api_matrix(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *fref;
  long   fref_frames;
  int    holes;
  long   expected;
  TASK_CHECK(encode_packets(&e,2,1,14)==0);
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,NULL)==0);
  expected=expected_frames(&e);
  /*Float native entry.*/
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,512,&fref,&fref_frames,&holes)==0);
  TASK_CHECK(fref_frames==expected&&holes==0);
  op_free(of);
  /*Float stereo entry: identical for an actual stereo stream.*/
  {
    float *st;
    long   st_frames;
    TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
    TASK_CHECK(op_set_inband_fec(of,1)==0);
    st=NULL;
    st_frames=0;
    holes=0;
    for(;;){
      float chunk[512*2];
      int   n;
      n=op_read_float_stereo(of,chunk,512*2);
      if(n==OP_HOLE){holes++;continue;}
      TASK_CHECK(n>=0);
      if(n==0)break;
      st=(float *)realloc(st,
       sizeof(float)*2*(size_t)(st_frames+n));
      TASK_CHECK(st!=NULL);
      memcpy(st+st_frames*2,chunk,sizeof(float)*2*(size_t)n);
      st_frames+=n;
    }
    TASK_CHECK(holes==0&&st_frames==expected);
    TASK_CHECK(memcmp(st,fref,(size_t)expected*2*sizeof(float))==0);
    free(st);
    op_free(of);
  }
  /*Short native and short stereo entries, with dithering disabled.*/
  {
    opus_int16 *s_native;
    opus_int16 *s_stereo;
    long        n_native;
    long        n_stereo;
    int         i;
    TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
    TASK_CHECK(op_set_inband_fec(of,1)==0);
    op_set_dither_enabled(of,0);
    s_native=malloc(sizeof(opus_int16)*2*(size_t)expected);
    n_native=0;
    for(;;){
      opus_int16 chunk[512*2];
      int        n;
      int        li;
      n=op_read(of,chunk,512*2,&li);
      if(n==OP_HOLE)continue;
      TASK_CHECK(n>=0);
      if(n==0)break;
      memcpy(s_native+n_native*2,chunk,sizeof(opus_int16)*2*(size_t)n);
      n_native+=n;
    }
    op_free(of);
    TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
    TASK_CHECK(op_set_inband_fec(of,1)==0);
    op_set_dither_enabled(of,0);
    s_stereo=malloc(sizeof(opus_int16)*2*(size_t)expected);
    n_stereo=0;
    for(;;){
      opus_int16 chunk[512*2];
      int        n;
      n=op_read_stereo(of,chunk,512*2);
      if(n==OP_HOLE)continue;
      TASK_CHECK(n>=0);
      if(n==0)break;
      memcpy(s_stereo+n_stereo*2,chunk,sizeof(opus_int16)*2*(size_t)n);
      n_stereo+=n;
    }
    op_free(of);
    TASK_CHECK(n_native==expected&&n_stereo==expected);
    TASK_CHECK(memcmp(s_native,s_stereo,
     sizeof(opus_int16)*2*(size_t)expected)==0);
    for(i=0;i<expected*2;i++){
      int want;
      int got;
      want=(int)lrintf(fref[i]*32768.0f);
      got=s_native[i];
      TASK_CHECK(abs(got-want)<=1);
    }
    free(s_native);
    free(s_stereo);
  }
  free(fref);
  mb_clear(&b);
  return 0;
}

static int case_fragmented_reads(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *big;
  float *frag;
  long   big_frames;
  long   frag_frames;
  int    holes;
  int    chunks[4];
  int    ci;
  long   total;
  int    li;
  TASK_CHECK(encode_packets(&e,2,1,15)==0);
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,NULL)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,2048,&big,&big_frames,&holes)==0);
  TASK_CHECK(holes==0);
  op_free(of);
  /*Cycle through one-frame, small and large reads, checking op_pcm_tell after
     every single call.*/
  chunks[0]=1;
  chunks[1]=2;
  chunks[2]=7;
  chunks[3]=1931;
  ci=0;
  total=0;
  frag=malloc(sizeof(float)*2*(size_t)big_frames);
  TASK_CHECK(frag!=NULL);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  for(;;){
    float chunk[2048*2];
    int   n;
    n=op_read_float(of,chunk,chunks[ci%4]*2,&li);
    ci++;
    TASK_CHECK(n>=0);
    if(n==0)break;
    memcpy(frag+total*2,chunk,sizeof(float)*2*(size_t)n);
    total+=n;
    TASK_CHECK(op_pcm_tell(of)==total);
  }
  frag_frames=total;
  TASK_CHECK(frag_frames==big_frames);
  TASK_CHECK(memcmp(frag,big,(size_t)big_frames*2*sizeof(float))==0);
  op_free(of);
  free(big);
  free(frag);
  mb_clear(&b);
  return 0;
}

static int case_surplus_independent_of_chunking(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *a;
  float *c;
  long   fa;
  long   fc;
  int    holes;
  TASK_CHECK(encode_packets(&e,2,1,16)==0);
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,NULL)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,4096,&a,&fa,&holes)==0);
  TASK_CHECK(holes==0);
  op_free(of);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,1,&c,&fc,&holes)==0);
  TASK_CHECK(holes==0);
  TASK_CHECK(fa==fc);
  TASK_CHECK(memcmp(a,c,(size_t)fa*2*sizeof(float))==0);
  op_free(of);
  free(a);
  free(c);
  mb_clear(&b);
  return 0;
}

static int case_toggle_and_validation(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *pcm;
  long   frames;
  int    holes;
  TASK_CHECK(encode_packets(&e,2,1,17)==0);
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,NULL)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  /*Only 0 and 1 are accepted.*/
  TASK_CHECK(op_set_inband_fec(of,0)==0);
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(op_set_inband_fec(of,2)==OP_EINVAL);
  TASK_CHECK(op_set_inband_fec(of,-1)==OP_EINVAL);
  /*A failed call must not change the stored configuration.*/
  TASK_CHECK(drain_float(of,2,512,&pcm,&frames,&holes)==0);
  TASK_CHECK(holes==0&&frames==expected_frames(&e));
  op_free(of);
  free(pcm);
  /*After reading has started, further changes are rejected.*/
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  {
    float tmp[4];
    int   li;
    TASK_CHECK(op_read_float(of,tmp,4,&li)>0);
  }
  TASK_CHECK(op_set_inband_fec(of,0)==OP_EINVAL);
  TASK_CHECK(op_set_inband_fec(of,1)==OP_EINVAL);
  op_free(of);
  /*Seekable inputs never implement this mode.*/
  {
    int err;
    of=op_open_memory(b.d,b.n,&err);
    TASK_CHECK(of!=NULL&&op_seekable(of));
    TASK_CHECK(op_set_inband_fec(of,1)==OP_EIMPL);
    op_free(of);
  }
  /*Disabled mode keeps the legacy hole behavior on the same fixture.*/
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,0)==0);
  TASK_CHECK(drain_float(of,2,512,&pcm,&frames,&holes)==0);
  TASK_CHECK(holes>=1);
  TASK_CHECK(frames<expected_frames(&e));
  op_free(of);
  free(pcm);
  mb_clear(&b);
  return 0;
}

/*A decode callback that simply defers to the library default.*/
static int passthrough_decode_cb(void *_ctx,OpusMSDecoder *_decoder,
 void *_pcm,const ogg_packet *_op,int _nsamples,int _nchannels,int _format,
 int _li){
  (void)_ctx;
  (void)_decoder;
  (void)_pcm;
  (void)_op;
  (void)_nsamples;
  (void)_nchannels;
  (void)_format;
  (void)_li;
  return OP_DEC_USE_DEFAULT;
}

static int case_custom_decode_callback(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *pcm;
  long   frames;
  int    holes;
  TASK_CHECK(encode_packets(&e,2,1,18)==0);
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,NULL)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  op_set_decode_callback(of,passthrough_decode_cb,NULL);
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,512,&pcm,&frames,&holes)==0);
  /*The custom callback always uses the legacy page-continuation path.*/
  TASK_CHECK(holes>=1);
  TASK_CHECK(frames<expected_frames(&e));
  op_free(of);
  free(pcm);
  mb_clear(&b);
  return 0;
}

static int case_unsupported_mapping_family1(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  static const unsigned char map1[2]={0,1};
  struct mux_link l;
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *pcm;
  long   frames;
  int    holes;
  TASK_CHECK(encode_packets(&e,2,1,19)==0);
  memset(&l,0,sizeof(l));
  l.serial=246841;
  l.channel_count=2;
  l.mapping_family=1;
  l.stream_count=1;
  l.coupled_count=1;
  l.mapping=map1;
  l.npackets=TEST_NPACKETS;
  l.lengths=e.lengths;
  l.durations=e.durations;
  l.packets=(const unsigned char *)e.packets;
  l.preskip=e.preskip;
  l.drop=drop;
  l.ndrop=1;
  l.endtrim=TEST_ENDTRIM;
  mb_init(&b);
  TASK_CHECK(mux_link(&b,&l)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,2,512,&pcm,&frames,&holes)==0);
  /*Anything other than mapping family 0 mono/stereo stays on the hole path.*/
  TASK_CHECK(holes>=1);
  TASK_CHECK(frames<expected_frames(&e));
  op_free(of);
  free(pcm);
  mb_clear(&b);
  return 0;
}

static int case_mono_repair(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  static float windows[(TEST_NPACKETS+1)*TEST_FRAME];
  struct mux_link l;
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *got;
  float *ref;
  long   got_frames;
  long   ref_frames;
  int    holes;
  int    nwindows;
  TASK_CHECK(encode_packets(&e,1,1,20)==0);
  memset(&l,0,sizeof(l));
  l.serial=98765;
  l.channel_count=1;
  l.mapping_family=0;
  l.stream_count=1;
  l.coupled_count=0;
  l.mapping=NULL;
  l.npackets=TEST_NPACKETS;
  l.lengths=e.lengths;
  l.durations=e.durations;
  l.packets=(const unsigned char *)e.packets;
  l.preskip=e.preskip;
  l.drop=drop;
  l.ndrop=1;
  l.endtrim=TEST_ENDTRIM;
  mb_init(&b);
  TASK_CHECK(mux_link(&b,&l)==0);
  TASK_CHECK(reference_event_sequence(&e,1,TEST_DROP,1,windows,
   TEST_FRAME,&nwindows)==0);
  TASK_CHECK(nwindows==TEST_NPACKETS);
  TASK_CHECK(flatten_reference(1,e.preskip,TEST_ENDTRIM,windows,
   TEST_FRAME,nwindows,&ref,&ref_frames)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  TASK_CHECK(drain_float(of,1,512,&got,&got_frames,&holes)==0);
  TASK_CHECK(holes==0);
  TASK_CHECK(got_frames==ref_frames);
  TASK_CHECK(memcmp(got,ref,(size_t)ref_frames*sizeof(float))==0);
  op_free(of);
  free(got);
  /*The float stereo entry must serve the same repair, duplicated into both
     channels.*/
  {
    float *st;
    long   st_frames;
    long   i;
    TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
    TASK_CHECK(op_set_inband_fec(of,1)==0);
    st=malloc(sizeof(float)*2*(size_t)ref_frames);
    TASK_CHECK(st!=NULL);
    st_frames=0;
    for(;;){
      float chunk[512*2];
      int   n;
      n=op_read_float_stereo(of,chunk,512*2);
      TASK_CHECK(n>=0);
      if(n==0)break;
      memcpy(st+st_frames*2,chunk,sizeof(float)*2*(size_t)n);
      st_frames+=n;
    }
    TASK_CHECK(st_frames==ref_frames);
    for(i=0;i<ref_frames;i++){
      TASK_CHECK(st[2*i]==ref[i]&&st[2*i+1]==ref[i]);
    }
    free(st);
    op_free(of);
  }
  free(ref);
  mb_clear(&b);
  return 0;
}

static int case_small_output_drains(void){
  struct encoded e;
  struct mbuf    b;
  static const int drop[1]={TEST_DROP};
  static float windows[(TEST_NPACKETS+1)*TEST_FRAME*2];
  struct mem_stream ms;
  OpusFileCallbacks cb;
  OggOpusFile *of;
  float *got;
  float *ref;
  long   got_frames;
  long   ref_frames;
  int    holes;
  int    nwindows;
  TASK_CHECK(encode_packets(&e,2,1,21)==0);
  TASK_CHECK(build_hole_fixture(&b,&e,drop,1,NULL)==0);
  TASK_CHECK(reference_event_sequence(&e,2,TEST_DROP,1,windows,
   TEST_FRAME*2,&nwindows)==0);
  TASK_CHECK(flatten_reference(2,e.preskip,TEST_ENDTRIM,windows,
   TEST_FRAME*2,nwindows,&ref,&ref_frames)==0);
  TASK_CHECK(open_unseekable(&b,&ms,&cb,&of));
  TASK_CHECK(op_set_inband_fec(of,1)==0);
  /*Buffers that cannot even hold a full 20 ms frame, repeatedly, across the
     repair boundary.*/
  TASK_CHECK(drain_float(of,2,1,&got,&got_frames,&holes)==0);
  TASK_CHECK(holes==0);
  TASK_CHECK(got_frames==ref_frames);
  TASK_CHECK(memcmp(got,ref,(size_t)ref_frames*2*sizeof(float))==0);
  op_free(of);
  free(got);
  free(ref);
  mb_clear(&b);
  return 0;
}

const struct task_case task_cases[]={
  {"granules","single_hole_repairs",case_single_hole_repairs},
  {"granules","multi_packet_hole",case_multi_packet_hole},
  {"granules","eos_right_page",case_eos_right_page},
  {"granules","missing_timestamp",case_missing_timestamp},
  {"granules","non_20ms_page",case_non_20ms_page},
  {"granules","timestamp_jump_without_hole",case_timestamp_jump_without_hole},
  {"granules","gap_size_mismatch",case_gap_size_mismatch},
  {"granules","hole_on_continued_only_page",
   case_hole_on_continued_only_page},
  {"decode_history","fec_distinct_from_plc_and_normal",
   case_fec_distinct_from_plc_and_normal},
  {"decode_history","repaired_matches_event_sequence",
   case_repaired_matches_event_sequence},
  {"decode_history","successor_consumed_once",case_successor_consumed_once},
  {"decode_history","fallback_is_plc_not_silence",
   case_fallback_is_plc_not_silence},
  {"decode_history","gain_applied_to_repair",case_gain_applied_to_repair},
  {"decode_history","chain_boundary",case_chain_boundary},
  {"consumer","api_matrix",case_api_matrix},
  {"consumer","fragmented_reads",case_fragmented_reads},
  {"consumer","surplus_independent_of_chunking",
   case_surplus_independent_of_chunking},
  {"consumer","toggle_and_validation",case_toggle_and_validation},
  {"consumer","custom_decode_callback",case_custom_decode_callback},
  {"consumer","unsupported_mapping_family1",case_unsupported_mapping_family1},
  {"consumer","mono_repair",case_mono_repair},
  {"consumer","small_output_drains",case_small_output_drains}
};

const size_t task_case_count=sizeof(task_cases)/sizeof(task_cases[0]);
