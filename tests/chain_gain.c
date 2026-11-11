/*Regression tests for playback gain handling in chained Ogg Opus streams.

  This file generates its own short chained Ogg Opus fixture in memory (three
  logical links with different serial numbers, channel counts, pre-skip,
  end-trim, header gains and R128 tags), and checks the decoded, gain-adjusted
  output of libopusfile against an independent reference:

    * the encoded Opus packets are kept separately and decoded by a second,
      freshly created libopus decoder with zero gain;
    * the audible interval of each link is computed from its own pre-skip and
      its EOS granule position;
    * the gain selected by each op_set_gain_offset() mode is applied by hand to
      those zero-gain samples.

  No audio device is used, and the production decoding/gain code is not linked
  into the reference path. libopus encodes the deterministic low-amplitude
  signals, libogg does the Ogg encapsulation.*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <ogg/ogg.h>
#include <opus.h>
#include "opusfile.h"

/*All Opus frames in the fixture are 20 ms at 48 kHz.*/
#define CHAIN_FRAME_SIZE 960
#define CHAIN_NLINKS 3
#define CHAIN_MAX_PACKETS 8
#define CHAIN_MAX_CHANNELS 2

/*Tight float tolerance used for straight-through decodes.
  Both outputs come from the same libopus build decoding the same packets, so
  the only difference between them is where the gain scale is applied:
  libopusfile sets OPUS_SET_GAIN on its decoder, while the reference multiplies
  in float afterwards. Each path is a single float32 multiply, whose rounding
  error is at most 0.5 ULP (~6e-8 relative); 2e-5 relative is several hundred
  ULP of slack for compiler/fma differences, and 2e-6 absolute covers samples
  near zero where a relative bound is meaningless.*/
#define CHAIN_TOL_REL 2.0e-5f
#define CHAIN_TOL_ABS 2.0e-6f

/*Looser tolerance after a seek. libopusfile decodes pre-roll after resetting
  the decoder state, and the documented API contract is that post-seek samples
  need not be bit-identical to a straight decode; the discrepancy is bounded by
  ordinary lossy-codec convergence, far below the dB-sized gain steps the
  mode-retention checks use.*/
#define CHAIN_SEEK_TOL_REL 3.0e-2f
#define CHAIN_SEEK_TOL_ABS 3.0e-4f

/*16-bit outputs with dither disabled are a plain round-to-nearest of
  32768*float, so allow one quantization unit (1 LSB) of slack.*/
#define CHAIN_SHORT_TOL 1

#define CHAIN_CLAMP(_lo,_x,_hi) ((_x)<(_lo)?(_lo):((_x)>(_hi)?(_hi):(_x)))

/*Per-link fixture specification.*/
typedef struct ChainLinkSpec{
  ogg_uint32_t serialno;
  int          channels;
  unsigned     pre_skip;
  int          n_packets;
  int          end_trim;
  int          header_gain_q8;
  int          has_album;
  int          album_gain_q8;
  int          track_valid;
  int          track_gain_q8;
  float        frequency;
  float        phase_base;
} ChainLinkSpec;

/*A single encoded Opus packet (the bytes are the same ones muxed into the
  Ogg stream, retained so the independent reference can decode them).*/
typedef struct ChainPacket{
  unsigned char *data;
  opus_int32     bytes;
} ChainPacket;

/*Growable byte buffer used to collect the Ogg pages.*/
typedef struct ChainBuf{
  unsigned char *data;
  size_t         len;
  size_t         cap;
} ChainBuf;

/*Record of one op_read_float()/op_read() call.*/
#define CHAIN_MAX_CALLS 512
typedef struct ChainCallRecord{
  int          li;
  int          n;
  ogg_int64_t  pcm_tell;
} ChainCallRecord;

static const ChainLinkSpec chain_specs[CHAIN_NLINKS]={
  /*link 0: mono, valid album and track tags.*/
  {0x4F670011u,1,312,6,408,-512,1, 768,1,-256,660.0f,0.000f},
  /*link 1: stereo, no album tag at all, but a valid track tag.*/
  {0x4F670012u,2,520,5,744,1024,0,   0,1, 384,880.0f,0.250f},
  /*link 2: mono again, a valid album tag and an illegal (non-numeric)
    track tag, so missing and invalid tags are exercised on different links.*/
  {0x4F670013u,1,216,7,264, 256,1,-512,0,   0,440.0f,0.500f}
};

static ChainPacket chain_packets[CHAIN_NLINKS][CHAIN_MAX_PACKETS];
/*Zero-gain reference decode of every packet, CHAIN_FRAME_SIZE*ch floats.*/
static float *chain_ref_pcm[CHAIN_NLINKS][CHAIN_MAX_PACKETS];
static ChainBuf chain_file;
static int chain_failures;

#define CHAIN_CHECK(cond,...) \
  do{ \
    if(!(cond)){ \
      fprintf(stderr,"FAIL: " __VA_ARGS__); \
      fputc('\n',stderr); \
      chain_failures++; \
    } \
  } \
  while(0)

static void chain_buf_put(ChainBuf *_buf,const void *_data,size_t _n){
  if(_buf->len+_n>_buf->cap){
    size_t cap;
    cap=_buf->cap==0?4096:_buf->cap;
    while(cap<_buf->len+_n)cap*=2;
    _buf->data=(unsigned char *)realloc(_buf->data,cap);
    if(_buf->data==NULL){
      fprintf(stderr,"FAIL: out of memory\n");
      exit(2);
    }
    _buf->cap=cap;
  }
  memcpy(_buf->data+_buf->len,_data,_n);
  _buf->len+=_n;
}

/*Flush every ready page of an ogg_stream_state into the memory buffer.*/
static void chain_flush_pages(ogg_stream_state *_os,ChainBuf *_out){
  ogg_page pg;
  while(ogg_stream_flush(_os,&pg)){
    chain_buf_put(_out,pg.header,pg.header_len);
    chain_buf_put(_out,pg.body,pg.body_len);
  }
}

static void chain_put_packet(ogg_stream_state *_os,ChainBuf *_out,
 unsigned char *_data,long _n,int _bos,int _eos,ogg_int64_t _gp,
 ogg_int64_t _no){
  ogg_packet op;
  op.packet=_data;
  op.bytes=_n;
  op.b_o_s=_bos;
  op.e_o_s=_eos;
  op.granulepos=_gp;
  op.packetno=_no;
  if(ogg_stream_packetin(_os,&op)!=0){
    fprintf(stderr,"FAIL: ogg_stream_packetin\n");
    exit(2);
  }
  chain_flush_pages(_os,_out);
}

/*Build an OpusTags packet (Vorbis-comment style, without the framing bit).
  The comments are passed as ready-made "TAG=value" strings.*/
static int chain_build_tags(unsigned char *_out,
 const char *const *_comments,int _ncomments){
  static const char vendor[]="chain_gain-regression";
  unsigned char *p;
  int ci;
  memcpy(_out,"OpusTags",8);
  p=_out+8;
  *(uint32_t *)p=(uint32_t)sizeof(vendor)-1;
  p+=4;
  memcpy(p,vendor,sizeof(vendor)-1);
  p+=sizeof(vendor)-1;
  *(uint32_t *)p=(uint32_t)_ncomments;
  p+=4;
  for(ci=0;ci<_ncomments;ci++){
    long len;
    len=(long)strlen(_comments[ci]);
    *(uint32_t *)p=(uint32_t)len;
    p+=4;
    memcpy(p,_comments[ci],len);
    p+=len;
  }
  return (int)(p-_out);
}

/*Deterministic low-amplitude source signal; each link/channel is distinct.*/
static float chain_source_sample(const ChainLinkSpec *_s,int _ch,
 long _global_index){
  double t;
  double phase;
  t=((double)_global_index+_s->phase_base*48000.0)/48000.0;
  phase=2.0*3.14159265358979323846*(double)_s->frequency*t;
  if(_s->channels==2&&_ch==1){
    /*Right channel uses a cosine so L and R cannot be swapped unnoticed.*/
    return (float)(0.02*cos(phase));
  }
  return (float)(0.02*sin(phase));
}

/*Encode one link and append its pages to the chain file.*/
static void chain_build_link(const ChainLinkSpec *_s,int _li){
  ogg_stream_state os;
  OpusEncoder *enc;
  unsigned char head[19];
  unsigned char tags[256];
  const char *comments[4];
  int ncomments;
  int tags_len;
  int err;
  int pi;
  char tag_buf[2][64];
  if(ogg_stream_init(&os,(int)_s->serialno)!=0){
    fprintf(stderr,"FAIL: ogg_stream_init\n");
    exit(2);
  }
  /*OpusHead, channel mapping family 0 (mono/stereo single stream).*/
  memcpy(head,"OpusHead",8);
  head[8]=1;
  head[9]=(unsigned char)_s->channels;
  head[10]=(unsigned char)(_s->pre_skip&0xFF);
  head[11]=(unsigned char)(_s->pre_skip>>8);
  head[12]=0x80;head[13]=0xBB;head[14]=0;head[15]=0; /*48000 Hz*/
  head[16]=(unsigned char)(_s->header_gain_q8&0xFF);
  head[17]=(unsigned char)((_s->header_gain_q8>>8)&0xFF);
  head[18]=0;
  chain_put_packet(&os,&chain_file,head,sizeof(head),1,0,0,0);

  ncomments=0;
  if(_s->has_album){
    snprintf(tag_buf[0],sizeof(tag_buf[0]),"R128_ALBUM_GAIN=%d",
     _s->album_gain_q8);
    comments[ncomments++]=tag_buf[0];
  }
  if(_s->track_valid){
    snprintf(tag_buf[1],sizeof(tag_buf[1]),"R128_TRACK_GAIN=%d",
     _s->track_gain_q8);
    comments[ncomments++]=tag_buf[1];
  }
  else{
    /*Link 1 carries a syntactically present but non-numeric track tag.*/
    comments[ncomments++]="R128_TRACK_GAIN=not-a-number";
  }
  tags_len=chain_build_tags(tags,comments,ncomments);
  chain_put_packet(&os,&chain_file,tags,tags_len,0,0,0,1);

  enc=opus_encoder_create(48000,_s->channels,OPUS_APPLICATION_AUDIO,&err);
  if(enc==NULL||err!=OPUS_OK){
    fprintf(stderr,"FAIL: opus_encoder_create: %d\n",err);
    exit(2);
  }
  if(opus_encoder_ctl(enc,OPUS_SET_BITRATE(64000))!=OPUS_OK){
    fprintf(stderr,"FAIL: OPUS_SET_BITRATE\n");
    exit(2);
  }
  for(pi=0;pi<_s->n_packets;pi++){
    float pcm[CHAIN_FRAME_SIZE*CHAIN_MAX_CHANNELS];
    static unsigned char packet[4000];
    opus_int32 bytes;
    ogg_int64_t gp;
    int i;
    int ci;
    for(i=0;i<CHAIN_FRAME_SIZE;i++){
      for(ci=0;ci<_s->channels;ci++){
        pcm[i*_s->channels+ci]=chain_source_sample(_s,ci,
         (long)pi*CHAIN_FRAME_SIZE+i);
      }
    }
    if(_s->channels==2)bytes=opus_encode_float(enc,pcm,CHAIN_FRAME_SIZE,
     packet,sizeof(packet));
    else bytes=opus_encode_float(enc,pcm,CHAIN_FRAME_SIZE,packet,
     sizeof(packet));
    if(bytes<1){
      fprintf(stderr,"FAIL: opus_encode_float: %d\n",(int)bytes);
      exit(2);
    }
    chain_packets[_li][pi].data=(unsigned char *)malloc((size_t)bytes);
    if(chain_packets[_li][pi].data==NULL){
      fprintf(stderr,"FAIL: out of memory\n");
      exit(2);
    }
    memcpy(chain_packets[_li][pi].data,packet,(size_t)bytes);
    chain_packets[_li][pi].bytes=bytes;
    /*Granule positions count PCM samples from zero, pre-skip included.
      The EOS packet pulls the final granule back by end_trim samples.*/
    gp=(ogg_int64_t)_s->pre_skip+
     (ogg_int64_t)(pi+1)*CHAIN_FRAME_SIZE;
    if(pi==_s->n_packets-1)gp-=_s->end_trim;
    chain_put_packet(&os,&chain_file,packet,bytes,0,
     pi==_s->n_packets-1,gp,pi+2);
  }
  opus_encoder_destroy(enc);
  ogg_stream_clear(&os);
}

/*Independent reference: decode every retained packet at zero gain with a
  brand new decoder, so libopusfile's output is never the only reference.*/
static void chain_build_reference(const ChainLinkSpec *_s,int _li){
  OpusDecoder *dec;
  int err;
  int pi;
  dec=opus_decoder_create(48000,_s->channels,&err);
  if(dec==NULL||err!=OPUS_OK){
    fprintf(stderr,"FAIL: opus_decoder_create: %d\n",err);
    exit(2);
  }
  for(pi=0;pi<_s->n_packets;pi++){
    float *pcm;
    int n;
    pcm=(float *)malloc(sizeof(float)*CHAIN_FRAME_SIZE*_s->channels);
    if(pcm==NULL){
      fprintf(stderr,"FAIL: out of memory\n");
      exit(2);
    }
    n=opus_decode_float(dec,chain_packets[_li][pi].data,
     chain_packets[_li][pi].bytes,pcm,CHAIN_FRAME_SIZE,0);
    if(n!=CHAIN_FRAME_SIZE){
      fprintf(stderr,"FAIL: reference decode link %d packet %d: %d\n",
       _li,pi,n);
      exit(2);
    }
    chain_ref_pcm[_li][pi]=pcm;
  }
  opus_decoder_destroy(dec);
}

static long chain_audible_frames(const ChainLinkSpec *_s){
  return (long)_s->n_packets*CHAIN_FRAME_SIZE-_s->pre_skip-_s->end_trim;
}

/*Mirror the gain rule implemented by op_update_gain() in src/opusfile.c,
  including the [-32768,32767] Q8 clamp and the "missing/invalid tag adds
  nothing" semantics.*/
static int chain_effective_gain_q8(const ChainLinkSpec *_s,int _gain_type,
 opus_int32 _offset){
  opus_int32 g;
  g=_offset;
  switch(_gain_type){
    case OP_HEADER_GAIN:
      g+=_s->header_gain_q8;
      break;
    case OP_ALBUM_GAIN:
      g+=_s->header_gain_q8;
      if(_s->has_album)g+=_s->album_gain_q8;
      break;
    case OP_TRACK_GAIN:
      g+=_s->header_gain_q8;
      if(_s->track_valid)g+=_s->track_gain_q8;
      break;
    case OP_ABSOLUTE_GAIN:
      break;
    default:
      fprintf(stderr,"FAIL: unknown gain type %d\n",_gain_type);
      exit(2);
  }
  if(g<-32768)g=-32768;
  if(g>32767)g=32767;
  return (int)g;
}

static float chain_gain_scale(int _gain_q8){
  return powf(10.0f,(float)_gain_q8/(20.0f*256.0f));
}

/*Reference sample (with the selected gain applied) for audible frame _a of
  link _li, channel _ch.*/
static float chain_expected_sample(int _li,long _a,int _ch,int _gain_type,
 opus_int32 _offset){
  const ChainLinkSpec *s;
  long global_index;
  int pi;
  int within;
  float zero_gain;
  s=&chain_specs[_li];
  global_index=(long)s->pre_skip+_a;
  pi=(int)(global_index/CHAIN_FRAME_SIZE);
  within=(int)(global_index%CHAIN_FRAME_SIZE);
  zero_gain=chain_ref_pcm[_li][pi][within*s->channels+_ch];
  return zero_gain*
   chain_gain_scale(chain_effective_gain_q8(s,_gain_type,_offset));
}

/*Per-link captured output.*/
typedef struct ChainCapture{
  float          *pcm[CHAIN_NLINKS];
  long            frames[CHAIN_NLINKS];
  long            cap[CHAIN_NLINKS];
  ChainCallRecord calls[CHAIN_MAX_CALLS];
  int             ncalls;
  long            total_frames;
} ChainCapture;

static void chain_capture_init(ChainCapture *_c){
  memset(_c,0,sizeof(*_c));
}

static void chain_capture_free(ChainCapture *_c){
  int li;
  for(li=0;li<CHAIN_NLINKS;li++)free(_c->pcm[li]);
}

static void chain_capture_append(ChainCapture *_c,int _li,const float *_src,
 int _n,int _nchannels,ogg_int64_t _tell){
  long old;
  if(_c->frames[_li]+_n>_c->cap[_li]){
    long cap;
    cap=_c->cap[_li]==0?2048:_c->cap[_li];
    while(cap<_c->frames[_li]+_n)cap*=2;
    _c->pcm[_li]=(float *)realloc(_c->pcm[_li],
     sizeof(float)*cap*_nchannels);
    if(_c->pcm[_li]==NULL){
      fprintf(stderr,"FAIL: out of memory\n");
      exit(2);
    }
    _c->cap[_li]=cap;
  }
  old=_c->frames[_li];
  memcpy(_c->pcm[_li]+old*_nchannels,_src,
   sizeof(float)*_n*_nchannels);
  _c->frames[_li]+=_n;
  _c->total_frames+=_n;
  CHAIN_CHECK(_c->ncalls<CHAIN_MAX_CALLS,
   "call record table overflow (more than %d reads)",CHAIN_MAX_CALLS);
  if(_c->ncalls<CHAIN_MAX_CALLS){
    _c->calls[_c->ncalls].li=_li;
    _c->calls[_c->ncalls].n=_n;
    _c->calls[_c->ncalls].pcm_tell=_tell;
    _c->ncalls++;
  }
}

/*Drain an open OggOpusFile through deliberately small buffers so that reads
  land in the middle of Opus packets and cross link boundaries.*/
static int chain_drain_float(OggOpusFile *_of,ChainCapture *_c,int _buf_size){
  float *buf;
  int n;
  int li;
  int prev_li;
  int saw_boundary;
  buf=(float *)malloc(sizeof(float)*_buf_size);
  if(buf==NULL){
    fprintf(stderr,"FAIL: out of memory\n");
    exit(2);
  }
  prev_li=-1;
  saw_boundary=0;
  while((n=op_read_float(_of,buf,_buf_size,&li))>0){
    const OpusHead *head;
    int nchannels;
    head=op_head(_of,li);
    if(head==NULL){
      fprintf(stderr,"FAIL: op_head returned NULL\n");
      free(buf);
      return -1;
    }
    nchannels=head->channel_count;
    CHAIN_CHECK(nchannels==chain_specs[li].channels,
     "link %d channel count: got %d want %d",li,nchannels,
     chain_specs[li].channels);
    if(n*nchannels>_buf_size){
      fprintf(stderr,"FAIL: decoder overran read buffer\n");
      free(buf);
      return -1;
    }
    if(prev_li!=-1&&li!=prev_li)saw_boundary=1;
    prev_li=li;
    chain_capture_append(_c,li,buf,n,nchannels,op_pcm_tell(_of));
  }
  free(buf);
  if(n<0){
    fprintf(stderr,"FAIL: op_read_float returned %d\n",n);
    return n;
  }
  return saw_boundary;
}

static int chain_sample_close(float _got,float _expected,float _tol_rel,
 float _tol_abs){
  float tol;
  if(!isfinite(_got))return 0;
  tol=_tol_abs+_tol_rel*fabsf(_expected);
  return fabsf(_got-_expected)<=tol;
}

/*Compare a full capture against the independent reference, sample by sample,
  for every audible PCM value.*/
static void chain_check_pcm(const ChainCapture *_c,int _gain_type,
 opus_int32 _offset,float _tol_rel,float _tol_abs,const char *_label){
  int li;
  for(li=0;li<CHAIN_NLINKS;li++){
    const ChainLinkSpec *s;
    long a;
    int ch;
    long bad;
    s=&chain_specs[li];
    if(_c->frames[li]!=chain_audible_frames(s)){
      CHAIN_CHECK(0,"%s: link %d frame count: got %ld want %ld",_label,li,
       _c->frames[li],chain_audible_frames(s));
      return;
    }
    bad=0;
    for(a=0;a<_c->frames[li];a++){
      for(ch=0;ch<s->channels;ch++){
        float got;
        float want;
        got=_c->pcm[li][a*s->channels+ch];
        want=chain_expected_sample(li,a,ch,_gain_type,_offset);
        if(!chain_sample_close(got,want,_tol_rel,_tol_abs)){
          if(bad<3){
            fprintf(stderr,
             "FAIL: %s: link %d frame %ld ch %d: got %.8g want %.8g\n",
             _label,li,a,ch,got,want);
          }
          bad++;
        }
      }
    }
    CHAIN_CHECK(bad==0,"%s: link %d has %ld PCM mismatches",_label,li,bad);
  }
}

/*Read-only, unseekable memory stream: only the read callback exists, and each
  read is clipped to a small odd number of bytes so Ogg pages and Opus packets
  are split across many callbacks.*/
typedef struct ChainReadOnlyStream{
  const unsigned char *data;
  size_t               size;
  size_t               pos;
  size_t               chunk;
  long                 n_reads;
  long                 n_short_reads;
} ChainReadOnlyStream;

static int chain_read_only_cb(void *_stream,unsigned char *_ptr,int _nbytes){
  ChainReadOnlyStream *s;
  size_t want;
  if(_nbytes<=0)return 0;
  s=(ChainReadOnlyStream *)_stream;
  want=(size_t)_nbytes;
  if(want>s->chunk)want=s->chunk;
  if(s->pos>=s->size)return 0;
  if(want>s->size-s->pos)want=s->size-s->pos;
  memcpy(_ptr,s->data+s->pos,want);
  s->pos+=want;
  s->n_reads++;
  if(want<(size_t)_nbytes)s->n_short_reads++;
  return (int)want;
}

static const int chain_gain_types[4]={
  OP_HEADER_GAIN,OP_TRACK_GAIN,OP_ALBUM_GAIN,OP_ABSOLUTE_GAIN
};
static const char *const chain_gain_names[4]={
  "HEADER","TRACK","ALBUM","ABSOLUTE"
};
static const opus_int32 chain_offsets[2]={512,-512};

/*Seekable path: op_open_memory, all four gain modes and both offset signs.*/
static void chain_test_seekable_modes(void){
  int gi;
  int oi;
  for(gi=0;gi<4;gi++){
    for(oi=0;oi<2;oi++){
      OggOpusFile *of;
      ChainCapture cap;
      int err;
      int saw_boundary;
      char label[64];
      of=op_open_memory(chain_file.data,chain_file.len,&err);
      if(of==NULL||err!=0){
        CHAIN_CHECK(0,"op_open_memory failed: %d",err);
        return;
      }
      CHAIN_CHECK(op_link_count(of)==CHAIN_NLINKS,
       "op_link_count: got %d want %d",op_link_count(of),CHAIN_NLINKS);
      err=op_set_gain_offset(of,chain_gain_types[gi],chain_offsets[oi]);
      CHAIN_CHECK(err==0,"op_set_gain_offset(%s) returned %d",
       chain_gain_names[gi],err);
      chain_capture_init(&cap);
      /*193 floats: 193 mono frames or 96 stereo frames per call.*/
      saw_boundary=chain_drain_float(of,&cap,193);
      snprintf(label,sizeof(label),"seekable/%s/offset%ld",
       chain_gain_names[gi],(long)chain_offsets[oi]);
      CHAIN_CHECK(saw_boundary,"%s: never observed a link boundary",label);
      CHAIN_CHECK(cap.ncalls>=8,"%s: expected many small reads, got %d calls",
       label,cap.ncalls);
      chain_check_pcm(&cap,chain_gain_types[gi],chain_offsets[oi],
       CHAIN_TOL_REL,CHAIN_TOL_ABS,label);
      CHAIN_CHECK(cap.total_frames==
       chain_audible_frames(&chain_specs[0])+
       chain_audible_frames(&chain_specs[1])+
       chain_audible_frames(&chain_specs[2]),
       "%s: total frames %ld",label,cap.total_frames);
      /*op_pcm_tell must track the cumulative audible sample position.*/
      CHAIN_CHECK(op_pcm_tell(of)==cap.total_frames,
       "%s: final op_pcm_tell %ld != %ld",label,(long)op_pcm_tell(of),
       cap.total_frames);
      {
        long cumulative;
        int ci;
        cumulative=0;
        for(ci=0;ci<cap.ncalls;ci++){
          cumulative+=cap.calls[ci].n;
          CHAIN_CHECK(cap.calls[ci].pcm_tell==cumulative,
           "%s: pcm_tell %ld after call %d, expected %ld",label,
           (long)cap.calls[ci].pcm_tell,ci,cumulative);
        }
      }
      op_free(of);
      chain_capture_free(&cap);
    }
  }
}

/*Tag-level checks: missing album and the invalid track tag in link 1 must
  parse as "no gain", never as a value left over from the previous link.*/
static void chain_test_tag_parsing(void){
  OggOpusFile *of;
  int err;
  int g;
  of=op_open_memory(chain_file.data,chain_file.len,&err);
  if(of==NULL||err!=0){
    CHAIN_CHECK(0,"op_open_memory for tags failed: %d",err);
    return;
  }
  CHAIN_CHECK(opus_tags_get_album_gain(op_tags(of,1),&g)==OP_FALSE,
   "link 1 unexpectedly exposed an album gain");
  CHAIN_CHECK(opus_tags_get_track_gain(op_tags(of,1),&g)==0&&g==384,
   "link 1 track gain: got %d want 384",g);
  CHAIN_CHECK(opus_tags_get_album_gain(op_tags(of,0),&g)==0&&g==768,
   "link 0 album gain: got %d want 768",g);
  CHAIN_CHECK(opus_tags_get_track_gain(op_tags(of,0),&g)==0&&g==-256,
   "link 0 track gain: got %d want -256",g);
  CHAIN_CHECK(opus_tags_get_album_gain(op_tags(of,2),&g)==0&&g==-512,
   "link 2 album gain: got %d want -512",g);
  CHAIN_CHECK(opus_tags_get_track_gain(op_tags(of,2),&g)==OP_FALSE,
   "invalid link 2 track tag parsed as a number");
  op_free(of);
}

/*Unseekable path: read-only callbacks, tiny byte-sized reads.*/
static void chain_test_unseekable_modes(void){
  int gi;
  int oi;
  for(gi=0;gi<4;gi++){
    for(oi=0;oi<2;oi++){
      static const OpusFileCallbacks cbs={
        chain_read_only_cb,NULL,NULL,NULL
      };
      ChainReadOnlyStream stream;
      OggOpusFile *of;
      ChainCapture cap;
      int err;
      int saw_boundary;
      int li_seen[CHAIN_NLINKS];
      int ci;
      char label[64];
      memset(li_seen,0,sizeof(li_seen));
      stream.data=chain_file.data;
      stream.size=chain_file.len;
      stream.pos=0;
      stream.chunk=137;
      stream.n_reads=0;
      stream.n_short_reads=0;
      of=op_open_callbacks(&stream,&cbs,NULL,0,&err);
      if(of==NULL||err!=0){
        CHAIN_CHECK(0,"op_open_callbacks failed: %d",err);
        return;
      }
      CHAIN_CHECK(op_link_count(of)==1,
       "unseekable op_link_count: got %d want 1",op_link_count(of));
      err=op_set_gain_offset(of,chain_gain_types[gi],chain_offsets[oi]);
      CHAIN_CHECK(err==0,"unseekable op_set_gain_offset returned %d",err);
      chain_capture_init(&cap);
      saw_boundary=chain_drain_float(of,&cap,193);
      snprintf(label,sizeof(label),"unseekable/%s/offset%ld",
       chain_gain_names[gi],(long)chain_offsets[oi]);
      CHAIN_CHECK(saw_boundary,"%s: never observed a link boundary",label);
      CHAIN_CHECK(stream.n_reads>=20,
       "%s: expected many 137-byte reads, got %ld",label,stream.n_reads);
      CHAIN_CHECK(stream.n_short_reads>0,
       "%s: read callback was never clipped to the chunk size",label);
      for(ci=0;ci<cap.ncalls;ci++){
        if(cap.calls[ci].li>=0&&cap.calls[ci].li<CHAIN_NLINKS){
          li_seen[cap.calls[ci].li]=1;
        }
      }
      CHAIN_CHECK(li_seen[0]&&li_seen[1]&&li_seen[2],
       "%s: link indices did not cover all three links",label);
      chain_check_pcm(&cap,chain_gain_types[gi],chain_offsets[oi],
       CHAIN_TOL_REL,CHAIN_TOL_ABS,label);
      /*For an unseekable stream op_pcm_tell resets at each new link, so it
        must be compared against the per-link position, not a file total.*/
      {
        long per_link_frames[CHAIN_NLINKS];
        long last_tell;
        int last_li;
        memset(per_link_frames,0,sizeof(per_link_frames));
        last_li=-1;
        last_tell=-1;
        for(ci=0;ci<cap.ncalls;ci++){
          int li;
          li=cap.calls[ci].li;
          if(last_li!=-1&&li!=last_li){
            CHAIN_CHECK(cap.calls[ci].pcm_tell<
             (ogg_int64_t)CHAIN_FRAME_SIZE,
             "%s: pcm_tell did not reset at link boundary: %ld",label,
             (long)cap.calls[ci].pcm_tell);
          }
          per_link_frames[li]+=cap.calls[ci].n;
          CHAIN_CHECK(cap.calls[ci].pcm_tell==per_link_frames[li],
           "%s: per-link pcm_tell %ld != %ld",label,
           (long)cap.calls[ci].pcm_tell,per_link_frames[li]);
          last_li=li;
          last_tell=cap.calls[ci].pcm_tell;
        }
        CHAIN_CHECK(last_tell==chain_audible_frames(&chain_specs[2]),
         "%s: final per-link tell %ld",label,last_tell);
      }
      op_free(of);
      chain_capture_free(&cap);
    }
  }
}

/*Seek into the middle of link 2 and confirm the selected gain mode survives
  the seek, on both links afterwards.*/
static void chain_test_seek_gain(void){
  const opus_int32 offset=0;
  const int mode=OP_TRACK_GAIN;
  OggOpusFile *of;
  ChainCapture cap;
  int err;
  long target;
  long link1_start;
  int ci;
  int first_li;
  of=op_open_memory(chain_file.data,chain_file.len,&err);
  if(of==NULL||err!=0){
    CHAIN_CHECK(0,"op_open_memory for seek failed: %d",err);
    return;
  }
  err=op_set_gain_offset(of,mode,offset);
  CHAIN_CHECK(err==0,"op_set_gain_offset before seek returned %d",err);
  link1_start=chain_audible_frames(&chain_specs[0]);
  /*Land two full packets into link 1's audible region.*/
  target=link1_start+2*CHAIN_FRAME_SIZE;
  err=op_pcm_seek(of,target);
  CHAIN_CHECK(err==0,"op_pcm_seek(%ld) returned %d",target,err);
  chain_capture_init(&cap);
  if(chain_drain_float(of,&cap,193)<0){
    op_free(of);
    chain_capture_free(&cap);
    return;
  }
  CHAIN_CHECK(cap.ncalls>0,"seek produced no audio");
  first_li=cap.calls[0].li;
  CHAIN_CHECK(first_li==1,"seek landed in link %d, want 1",first_li);
  CHAIN_CHECK(cap.frames[0]==0,"seek returned link 0 audio");
  CHAIN_CHECK(cap.frames[1]==
   chain_audible_frames(&chain_specs[1])-2*CHAIN_FRAME_SIZE,
   "post-seek link 1 frames: got %ld want %ld",cap.frames[1],
   chain_audible_frames(&chain_specs[1])-2*CHAIN_FRAME_SIZE);
  CHAIN_CHECK(cap.frames[2]==chain_audible_frames(&chain_specs[2]),
   "post-seek link 2 frames: got %ld want %ld",cap.frames[2],
   chain_audible_frames(&chain_specs[2]));
  /*Loose tolerance: pre-roll decodes after a decoder reset.*/
  {
    long a;
    long bad_track;
    long bad_absolute;
    int ch;
    bad_track=0;
    bad_absolute=0;
    for(a=0;a<cap.frames[1];a++){
      long ref_a;
      ref_a=a+2*CHAIN_FRAME_SIZE;
      for(ch=0;ch<chain_specs[1].channels;ch++){
        float got;
        float want_track;
        float want_abs;
        got=cap.pcm[1][a*chain_specs[1].channels+ch];
        want_track=chain_expected_sample(1,ref_a,ch,mode,offset);
        want_abs=chain_expected_sample(1,ref_a,ch,OP_ABSOLUTE_GAIN,offset);
        if(!chain_sample_close(got,want_track,CHAIN_SEEK_TOL_REL,
         CHAIN_SEEK_TOL_ABS))bad_track++;
        if(chain_sample_close(got,want_abs,CHAIN_SEEK_TOL_REL,
         CHAIN_SEEK_TOL_ABS))bad_absolute++;
      }
    }
    CHAIN_CHECK(bad_track==0,
     "post-seek link 1 no longer matches TRACK gain: %ld mismatches",
     bad_track);
    /*An absolute-gain reference must NOT fit in general: the track mode is
      still set. Only samples near a zero crossing can fall inside the absolute
      tolerance by accident, so require such accidental matches to stay rare.*/
    CHAIN_CHECK(4*bad_absolute<
     cap.frames[1]*chain_specs[1].channels,
     "post-seek gain mode was indistinguishable from ABSOLUTE_GAIN (%ld/%ld "
     "accidental matches)",bad_absolute,
     cap.frames[1]*chain_specs[1].channels);
  }
  for(ci=0;ci<cap.ncalls;ci++){
    CHAIN_CHECK(cap.calls[ci].li>=1,
     "post-seek stream revisited link 0");
  }
  chain_capture_free(&cap);

  /*Seek back to the start of the file.*/
  err=op_pcm_seek(of,0);
  CHAIN_CHECK(err==0,"op_pcm_seek(0) returned %d",err);
  chain_capture_init(&cap);
  if(chain_drain_float(of,&cap,193)<0){
    op_free(of);
    chain_capture_free(&cap);
    return;
  }
  CHAIN_CHECK(cap.calls[0].li==0,
   "seek-to-zero landed in link %d",cap.calls[0].li);
  chain_check_pcm(&cap,mode,offset,CHAIN_SEEK_TOL_REL,CHAIN_SEEK_TOL_ABS,
   "seek-to-zero/TRACK");
  op_free(of);
  chain_capture_free(&cap);
}

/*Gain offset changes are packet-granular: already decoded, buffered samples
  keep the old gain; only the next packet actually decoded uses the new one.*/
static void chain_test_packet_boundary_switch(void){
  OggOpusFile *of;
  ChainCapture cap;
  int err;
  const opus_int32 offset_a=0;
  const opus_int32 offset_b=1536;
  long first_read;
  long packet0_audible;
  long a;
  int ch;
  long bad;
  of=op_open_memory(chain_file.data,chain_file.len,&err);
  if(of==NULL||err!=0){
    CHAIN_CHECK(0,"op_open_memory for gain switch failed: %d",err);
    return;
  }
  err=op_set_gain_offset(of,OP_HEADER_GAIN,offset_a);
  CHAIN_CHECK(err==0,"initial op_set_gain_offset returned %d",err);
  chain_capture_init(&cap);
  /*64 mono frames: the first packet is decoded into the internal scratch
    buffer and partially returned, leaving a decoded remainder behind.*/
  first_read=0;
  {
    float buf[64];
    int li;
    int n;
    n=op_read_float(of,buf,64,&li);
    CHAIN_CHECK(n==64&&li==0,"first small read: n=%d li=%d",n,li);
    if(n>0)chain_capture_append(&cap,li,buf,n,1,op_pcm_tell(of));
    first_read=n;
  }
  err=op_set_gain_offset(of,OP_HEADER_GAIN,offset_b);
  CHAIN_CHECK(err==0,"second op_set_gain_offset returned %d",err);
  if(chain_drain_float(of,&cap,64)<0){
    op_free(of);
    chain_capture_free(&cap);
    return;
  }
  /*Link 0, packet 0 contributes frames [0, 960-pre_skip); everything before
    that audible index must still use gain A, everything after it gain B.*/
  packet0_audible=CHAIN_FRAME_SIZE-chain_specs[0].pre_skip;
  CHAIN_CHECK(first_read<packet0_audible,
   "first read consumed the whole first packet (%ld >= %ld)",
   first_read,packet0_audible);
  CHAIN_CHECK(cap.frames[0]==chain_audible_frames(&chain_specs[0]),
   "gain-switch link 0 frames: got %ld want %ld",cap.frames[0],
   chain_audible_frames(&chain_specs[0]));
  bad=0;
  for(a=0;a<cap.frames[0];a++){
    opus_int32 off;
    off=a<packet0_audible?offset_a:offset_b;
    for(ch=0;ch<chain_specs[0].channels;ch++){
      float got;
      float want;
      got=cap.pcm[0][a*chain_specs[0].channels+ch];
      want=chain_expected_sample(0,a,ch,OP_HEADER_GAIN,off);
      if(!chain_sample_close(got,want,CHAIN_TOL_REL,CHAIN_TOL_ABS)){
        if(bad<3){
          fprintf(stderr,
           "FAIL: gain switch link 0 frame %ld: got %.8g want %.8g (%s)\n",
           a,got,want,off==offset_a?"old gain":"new gain");
        }
        bad++;
      }
    }
  }
  CHAIN_CHECK(bad==0,"gain switch has %ld mismatched link 0 samples",bad);
  /*The remaining links are entirely decoded after the switch.*/
  {
    int li;
    for(li=1;li<CHAIN_NLINKS;li++){
      long li_bad;
      li_bad=0;
      for(a=0;a<cap.frames[li];a++){
        for(ch=0;ch<chain_specs[li].channels;ch++){
          float got;
          float want;
          got=cap.pcm[li][a*chain_specs[li].channels+ch];
          want=chain_expected_sample(li,a,ch,OP_HEADER_GAIN,offset_b);
          if(!chain_sample_close(got,want,CHAIN_TOL_REL,CHAIN_TOL_ABS)){
            li_bad++;
          }
        }
      }
      CHAIN_CHECK(li_bad==0,
       "gain switch link %d has %ld mismatches",li,li_bad);
    }
  }
  op_free(of);
  chain_capture_free(&cap);
}

/*An invalid gain type is rejected with OP_EINVAL, and neither stops playback
  nor changes the gain mode that was actually in use.*/
static void chain_test_invalid_gain_type(void){
  OggOpusFile *of;
  ChainCapture cap;
  int err;
  of=op_open_memory(chain_file.data,chain_file.len,&err);
  if(of==NULL||err!=0){
    CHAIN_CHECK(0,"op_open_memory for invalid type failed: %d",err);
    return;
  }
  err=op_set_gain_offset(of,OP_TRACK_GAIN,256);
  CHAIN_CHECK(err==0,"valid op_set_gain_offset returned %d",err);
  err=op_set_gain_offset(of,4242,256);
  CHAIN_CHECK(err==OP_EINVAL,"bogus gain type returned %d, want OP_EINVAL",
   err);
  chain_capture_init(&cap);
  if(chain_drain_float(of,&cap,193)<0){
    op_free(of);
    chain_capture_free(&cap);
    return;
  }
  chain_check_pcm(&cap,OP_TRACK_GAIN,256,CHAIN_TOL_REL,CHAIN_TOL_ABS,
   "invalid-gain-type");
  op_free(of);
  chain_capture_free(&cap);
}

/*Clamp boundaries: the final Q8 gain is clamped to [-32768,32767], and
  boundary-exact values behave identically to overshooting ones. The signals
  stay low-amplitude and are compared as floats, so no clipped waveform is
  used to infer anything.*/
static void chain_test_clamp(void){
  OggOpusFile *of1;
  OggOpusFile *of2;
  ChainCapture c1;
  ChainCapture c2;
  int err;
  int li;
  long a;
  int ch;
  /*Absolute mode: +40000 clamps to +32767, -40000 clamps to -32768.*/
  static const opus_int32 over[2]={40000,-40000};
  static const opus_int32 edge[2]={32767,-32768};
  int k;
  for(k=0;k<2;k++){
    of1=op_open_memory(chain_file.data,chain_file.len,&err);
    of2=op_open_memory(chain_file.data,chain_file.len,&err);
    if(of1==NULL||of2==NULL){
      CHAIN_CHECK(0,"op_open_memory in clamp test failed: %d",err);
      return;
    }
    CHAIN_CHECK(op_set_gain_offset(of1,OP_ABSOLUTE_GAIN,over[k])==0,
     "clamp: set overshoot failed");
    CHAIN_CHECK(op_set_gain_offset(of2,OP_ABSOLUTE_GAIN,edge[k])==0,
     "clamp: set edge failed");
    chain_capture_init(&c1);
    chain_capture_init(&c2);
    if(chain_drain_float(of1,&c1,193)<0||chain_drain_float(of2,&c2,193)<0){
      op_free(of1);op_free(of2);
      chain_capture_free(&c1);chain_capture_free(&c2);
      return;
    }
    for(li=0;li<CHAIN_NLINKS;li++){
      for(a=0;a<c1.frames[li];a++){
        for(ch=0;ch<chain_specs[li].channels;ch++){
          float x;
          float y;
          x=c1.pcm[li][a*chain_specs[li].channels+ch];
          y=c2.pcm[li][a*chain_specs[li].channels+ch];
          CHAIN_CHECK(isfinite(x)&&isfinite(y),
           "clamp produced non-finite output");
          CHAIN_CHECK(x==y,
           "clamp %ld: overshoot and edge gains differ at link %d frame %ld",
           (long)over[k],li,a);
        }
      }
    }
    op_free(of1);op_free(of2);
    chain_capture_free(&c1);chain_capture_free(&c2);
  }
  /*Header mode cross-checks: offset extremes plus the link-0 header gain also
    land on the clamped edges, so the output must equal ABSOLUTE edge output.*/
  {
    static const struct{opus_int32 header_off;opus_int32 abs_edge;}cases[2]={
      {98303,32767},{-98302,-32768}
    };
    for(k=0;k<2;k++){
      of1=op_open_memory(chain_file.data,chain_file.len,&err);
      of2=op_open_memory(chain_file.data,chain_file.len,&err);
      if(of1==NULL||of2==NULL){
        CHAIN_CHECK(0,"op_open_memory in header clamp test failed: %d",err);
        return;
      }
      op_set_gain_offset(of1,OP_HEADER_GAIN,cases[k].header_off);
      op_set_gain_offset(of2,OP_ABSOLUTE_GAIN,cases[k].abs_edge);
      chain_capture_init(&c1);
      chain_capture_init(&c2);
      if(chain_drain_float(of1,&c1,193)<0||
       chain_drain_float(of2,&c2,193)<0){
        op_free(of1);op_free(of2);
        chain_capture_free(&c1);chain_capture_free(&c2);
        return;
      }
      for(a=0;a<c1.frames[0];a++){
        CHAIN_CHECK(c1.pcm[0][a]==c2.pcm[0][a],
         "header clamp edge %ld mismatch at link 0 frame %ld",
         (long)cases[k].abs_edge,a);
      }
      op_free(of1);op_free(of2);
      chain_capture_free(&c1);chain_capture_free(&c2);
    }
  }
}

/*16-bit path with dither disabled: low-amplitude, non-clipping signals must
  match the rounded 32768-scaled float reference within one LSB.*/
static void chain_test_short_path(void){
  static const struct{int mode;opus_int32 off;}cases[2]={
    {OP_ABSOLUTE_GAIN,0},{OP_HEADER_GAIN,0}
  };
  int k;
  for(k=0;k<2;k++){
    OggOpusFile *of;
    long short_frames[CHAIN_NLINKS];
    int err;
    int li;
    long a;
    int ch;
    opus_int16 buf[193];
    of=op_open_memory(chain_file.data,chain_file.len,&err);
    if(of==NULL||err!=0){
      CHAIN_CHECK(0,"op_open_memory for short test failed: %d",err);
      return;
    }
    op_set_dither_enabled(of,0);
    err=op_set_gain_offset(of,cases[k].mode,cases[k].off);
    CHAIN_CHECK(err==0,"short path op_set_gain_offset returned %d",err);
    memset(short_frames,0,sizeof(short_frames));
    for(;;){
      int n;
      int li_out;
      n=op_read(of,buf,(int)(sizeof(buf)/sizeof(buf[0])),&li_out);
      if(n==0)break;
      if(n<0){
        fprintf(stderr,"FAIL: op_read returned %d\n",n);
        chain_failures++;
        break;
      }
      {
        const OpusHead *head;
        int nchannels;
        head=op_head(of,li_out);
        nchannels=head->channel_count;
        for(a=0;a<n;a++){
          for(ch=0;ch<nchannels;ch++){
            float fref;
            long expected;
            opus_int16 got;
            fref=chain_expected_sample(li_out,short_frames[li_out]+a,ch,
             cases[k].mode,cases[k].off);
            expected=lrintf((double)CHAIN_CLAMP(-32768.0f,32768.0f*fref,
             32767.0f));
            got=buf[a*nchannels+ch];
            /*Signals stay low-amplitude, so valid output must never touch
              either 16-bit rail.*/
            CHAIN_CHECK(abs(got)<32767,
             "short path clipped at link %d frame %ld",li_out,
             short_frames[li_out]+a);
            CHAIN_CHECK(abs((int)got-(int)expected)<=CHAIN_SHORT_TOL,
             "short link %d frame %ld ch %d: got %d want %ld",
             li_out,short_frames[li_out]+a,ch,got,expected);
          }
        }
        short_frames[li_out]+=n;
      }
    }
    for(li=0;li<CHAIN_NLINKS;li++){
      CHAIN_CHECK(short_frames[li]==chain_audible_frames(&chain_specs[li]),
       "short path link %d frames: got %ld want %ld",li,short_frames[li],
       chain_audible_frames(&chain_specs[li]));
    }
    op_free(of);
  }
}

int main(void){
  int li;
  for(li=0;li<CHAIN_NLINKS;li++){
    chain_build_link(&chain_specs[li],li);
    chain_build_reference(&chain_specs[li],li);
  }
  if(chain_file.len==0){
    fprintf(stderr,"FAIL: fixture generation produced no bytes\n");
    return 2;
  }
  chain_test_tag_parsing();
  chain_test_seekable_modes();
  chain_test_unseekable_modes();
  chain_test_seek_gain();
  chain_test_packet_boundary_switch();
  chain_test_invalid_gain_type();
  chain_test_clamp();
  chain_test_short_path();
  if(chain_failures!=0){
    fprintf(stderr,"FAIL: chain gain regression: %d failure(s)\n",
     chain_failures);
    return 1;
  }
  printf("CHAIN_GAIN_OK: %zu-byte chained fixture, %ld+%ld+%ld audible frames\n",
   chain_file.len,chain_audible_frames(&chain_specs[0]),
   chain_audible_frames(&chain_specs[1]),
   chain_audible_frames(&chain_specs[2]));
  return 0;
}
