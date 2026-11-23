/********************************************************************
 *                                                                  *
 * Tests for the non-seekable Ogg/Vorbis rewind window.             *
 *                                                                  *
 * Three groups are registered through task_support/check.h:       *
 *   window    - storage, eviction, budget and range semantics      *
 *   replay    - sample exact reconstruction through every read API *
 *   lifecycle - enable rules, errors, chaining, holes, halfrate    *
 *                                                                  *
 * All material is generated in C with the bundled libvorbis        *
 * encoder and libogg framing; no external files are used.          *
 *                                                                  *
 ********************************************************************/

#include "../task_support/check.h"

#include <vorbis/vorbisenc.h>
#include <vorbis/vorbisfile.h>
#include <ogg/ogg.h>
#include "rewind_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

/* ---------------------------------------------------------------- */
/* in-memory Ogg stream with counted, deliberately short reads      */
/* ---------------------------------------------------------------- */

typedef struct {
  unsigned char *data;
  size_t         size;
  size_t         cap;
  size_t         pos;
  long           reads;
  size_t         short_read;
  size_t         mask_begin;   /* bytes hidden to simulate a hole   */
  size_t         mask_end;
} rstream;

static int rb_append(rstream *s,const void *p,size_t n){
  if(s->size+n>s->cap){
    size_t cap=(s->size+n)*2+4096;
    unsigned char *q=realloc(s->data,cap);
    if(!q)return -1;
    s->data=q;s->cap=cap;
  }
  memcpy(s->data+s->size,p,n);
  s->size+=n;
  return 0;
}

static int rb_page(rstream *s,ogg_page *pg){
  return rb_append(s,pg->header,pg->header_len) ||
         rb_append(s,pg->body,pg->body_len);
}

static size_t rs_read(void *p,size_t size,size_t nmemb,void *u){
  rstream *s=u;
  size_t bytes=size*nmemb;
  errno=0;
  s->reads++;
  if(!size)return 0;
  /* simulate a lost Ogg page by transparently jumping over its bytes;
     the framing layer then observes a page-sequence gap (OV_HOLE)
     while data keeps flowing, exactly as a pipe would deliver */
  if(s->mask_end>s->mask_begin &&
     s->pos>=s->mask_begin && s->pos<s->mask_end)
    s->pos=s->mask_end;
  if(bytes>s->size-s->pos)bytes=s->size-s->pos;
  if(s->short_read && bytes>s->short_read)bytes=s->short_read;
  bytes-=bytes%size;
  memcpy(p,s->data+s->pos,bytes);
  s->pos+=bytes;
  return bytes/size;
}

static int rs_seek(void *u,ogg_int64_t off,int whence){
  rstream *s=u;
  ogg_int64_t p=off+(whence==SEEK_CUR?(ogg_int64_t)s->pos:
                     whence==SEEK_END?(ogg_int64_t)s->size:0);
  if(p<0 || (size_t)p>s->size)return -1;
  s->pos=(size_t)p;
  return 0;
}
static long rs_tell(void *u){return (long)((rstream *)u)->pos;}

static int rs_open(rstream *s,OggVorbis_File *vf){
  ov_callbacks cb;
  memset(&cb,0,sizeof(cb));
  cb.read_func=rs_read;
  s->pos=0;
  s->reads=0;
  s->short_read=61; /* force many small, counted reads, as a pipe would */
  return ov_open_callbacks(s,vf,NULL,0,cb);
}

static int rs_open_seekable(rstream *s,OggVorbis_File *vf){
  ov_callbacks cb;
  memset(&cb,0,sizeof(cb));
  cb.read_func=rs_read;
  cb.seek_func=rs_seek;
  cb.tell_func=rs_tell;
  s->pos=0;
  s->reads=0;
  s->short_read=61;
  return ov_open_callbacks(s,vf,NULL,0,cb);
}

/* ---------------------------------------------------------------- */
/* C encoder: nonzero, transient-rich, both Vorbis window lengths   */
/* ---------------------------------------------------------------- */

static void audio_sample(long i,int ch,float *out,unsigned seed){
  /* quiet steady component (long blocks) plus isolated sharp
     attacks (short/transient blocks); never silent */
  float phase=(float)((seed*2654435761u)&0xffff);
  float v=0.06f*sinf(i*0.021f+phase*0.001f)+
          0.03f*sinf(i*0.007f+1.7f);
  if((i+seed)%2999<8)v+=0.9f;
  if((i+seed)%1777<3)v-=0.7f;
  for(int j=0;j<ch;j++)out[j]=v*(0.85f+0.15f*j)+1e-4f*(j+1);
}

static int encode_link(rstream *s,int ch,int rate,long frames,
                       float quality,unsigned serial_seed,
                       int *small,int *large){
  vorbis_info vi;vorbis_comment vc;vorbis_dsp_state vd;vorbis_block vb;
  ogg_stream_state os;ogg_packet h[3],op;ogg_page pg;
  long pos=0;
  int wsmall=0,wlarge=0;

  vorbis_info_init(&vi);
  if(vorbis_encode_init_vbr(&vi,ch,rate,quality))return -1;
  vorbis_comment_init(&vc);
  if(vorbis_analysis_init(&vd,&vi))return -1;
  vorbis_block_init(&vd,&vb);
  ogg_stream_init(&os,(int)(0x5eeda000u+serial_seed));

  vorbis_analysis_headerout(&vd,&vc,&h[0],&h[1],&h[2]);
  for(int i=0;i<3;i++)ogg_stream_packetin(&os,&h[i]);
  while(ogg_stream_flush(&os,&pg))if(rb_page(s,&pg))return -1;

  while(pos<frames){
    long n=frames-pos;
    float **pcm;
    if(n>2048)n=2048;
    pcm=vorbis_analysis_buffer(&vd,(int)n);
    for(long i=0;i<n;i++){
      float f[2];
      audio_sample(pos+i,ch,f,serial_seed);
      for(int j=0;j<ch;j++)pcm[j][i]=f[j];
    }
    vorbis_analysis_wrote(&vd,(int)n);
    pos+=n;
    while(vorbis_analysis_blockout(&vd,&vb)==1){
      vorbis_analysis(&vb,NULL);
      if(vb.W)wlarge++;else wsmall++;
      vorbis_bitrate_addblock(&vb);
      while(vorbis_bitrate_flushpacket(&vd,&op)){
        ogg_stream_packetin(&os,&op);
        while(ogg_stream_pageout(&os,&pg))if(rb_page(s,&pg))return -1;
      }
    }
  }
  vorbis_analysis_wrote(&vd,0);
  while(vorbis_analysis_blockout(&vd,&vb)==1){
    vorbis_analysis(&vb,NULL);
    if(vb.W)wlarge++;else wsmall++;
    vorbis_bitrate_addblock(&vb);
    while(vorbis_bitrate_flushpacket(&vd,&op)){
      ogg_stream_packetin(&os,&op);
      while(ogg_stream_pageout(&os,&pg))if(rb_page(s,&pg))return -1;
    }
  }
  while(ogg_stream_flush(&os,&pg))if(rb_page(s,&pg))return -1;

  if(small)*small=wsmall;
  if(large)*large=wlarge;
  ogg_stream_clear(&os);
  vorbis_block_clear(&vb);
  vorbis_dsp_clear(&vd);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  return 0;
}

/* ---------------------------------------------------------------- */
/* reference PCM storage, one blob per logical link                 */
/* ---------------------------------------------------------------- */

typedef struct {
  float *pcm;      /* interleaved */
  long   frames;
  long   cap;
  int    ch;
} pcm_blob;

static void blob_init(pcm_blob *b,int ch){memset(b,0,sizeof(*b));b->ch=ch;}
static void blob_free(pcm_blob *b){free(b->pcm);memset(b,0,sizeof(*b));}
static int blob_put(pcm_blob *b,float **pcm,long n){
  if(b->frames+n>b->cap){
    long cap=(b->frames+n)*2;
    float *q=realloc(b->pcm,cap*b->ch*sizeof(float));
    if(!q)return -1;
    b->pcm=q;b->cap=cap;
  }
  for(long i=0;i<n;i++)
    for(int j=0;j<b->ch;j++)
      b->pcm[(b->frames+i)*b->ch+j]=pcm[j][i];
  b->frames+=n;
  return 0;
}

static void scale_filter(float **pcm,long ch,long n,void *param);

/* skip exactly n PCM frames on a filtered integer entry point,
   capping each request so the decoder never overshoots the target and
   counting every sample actually handed to the filter */
static int skip_int_frames_filtered(OggVorbis_File *vf,long n,int ch,
                                    long *filtered){
  char buf[4096];
  int section;
  long done=0;
  while(done<n){
    long frames=n-done;
    long bytes=frames*ch*2;
    if(bytes>(long)sizeof(buf))bytes=sizeof(buf);
    long k=ov_read_filter(vf,buf,(int)bytes,0,2,1,&section,
                          scale_filter,filtered);
    if(k<=0)return -1;
    done+=k/(2*ch);
  }
  return done==n?0:-1;
}

/* decode an entire stream sequentially, keeping one blob per link.
   returns the number of links seen or a negative error. */
static long decode_reference(rstream *s,pcm_blob *blobs,int maxblobs,
                             int readsize){
  OggVorbis_File vf;
  float **pcm;
  int section;
  long k;
  long links=0;
  if(rs_open(s,&vf))return -1;
  while(1){
    k=ov_read_float(&vf,&pcm,readsize,&section);
    if(k==0)break;
    if(k<0){
      if(k==OV_HOLE)continue;
      ov_clear(&vf);
      return k;
    }
    if(section>=maxblobs){ov_clear(&vf);return -2;}
    while(links<=section){
      blob_init(&blobs[links],(int)ov_info(&vf,section)->channels);
      links++;
    }
    if(blob_put(&blobs[section],pcm,k)){ov_clear(&vf);return -3;}
  }
  ov_clear(&vf);
  return links;
}

static void free_blobs(pcm_blob *blobs,long n){
  for(long i=0;i<n;i++)blob_free(&blobs[i]);
}

/* compare a rewind suffix against a reference blob, starting
   blob-offset 'off'; reads use the given size. */
static int check_float_suffix(OggVorbis_File *vf,const pcm_blob *b,
                               ogg_int64_t off,int readsize){
  float **pcm;
  int section;
  long got=0;
  long want=b->frames-(long)off;
  while(got<want){
    int req=want-got;
    if(req>readsize)req=readsize;
    long k=ov_read_float(vf,&pcm,req,&section);
    if(k<=0)return -1;
    for(long i=0;i<k;i++)
      for(int j=0;j<b->ch;j++){
        float a=pcm[j][i];
        float c=b->pcm[((long)off+got+i)*b->ch+j];
        if(fabsf(a-c)>2e-6f)return -2;
      }
    got+=k;
  }
  return 0;
}

static void scale_filter(float **pcm,long ch,long n,void *param){
  long *count=param;
  *count+=n;
  for(long j=0;j<ch;j++)
    for(long i=0;i<n;i++)
      pcm[j][i]*=2.0f;
}

/* ================================================================= */
/* window group                                                      */
/* ================================================================= */

static int case_enable_validation(void){
  rstream s;OggVorbis_File vf,pf;
  ogg_int64_t lo,hi;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,2,48000,4096,0.3f,1,NULL,NULL)==0);

  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(!ov_seekable(&vf));
  TASK_CHECK(ov_rewind_enable(&vf,65535)==OV_EINVAL);
  TASK_CHECK(ov_rewind_enable(&vf,8388609)==OV_EINVAL);
  TASK_CHECK(ov_rewind_enable(&vf,0)==OV_EINVAL);
  TASK_CHECK(ov_rewind_enable(&vf,65536)==0);
  TASK_CHECK(ov_rewind_enable(&vf,65536)==OV_EINVAL); /* one shot */
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==OV_FALSE);
  TASK_CHECK(lo==0 && hi==0);
  TASK_CHECK(ov_pcm_rewind(&vf,0)==OV_EINVAL);         /* nothing yet */
  ov_clear(&vf);

  /* seekable sources reject the window */
  s.pos=0;
  TASK_CHECK(rs_open_seekable(&s,&pf)==0);
  TASK_CHECK(ov_seekable(&pf));
  TASK_CHECK(ov_rewind_enable(&pf,65536)==OV_EINVAL);
  TASK_CHECK(ov_rewind_range(&pf,&lo,&hi)==OV_ENOSEEK);
  ov_clear(&pf);

  /* enabling after PCM delivery started is refused */
  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_rewind_enable(&vf,65536)==0);
  {
    float **pcm;int section;
    TASK_CHECK(ov_read_float(&vf,&pcm,8,&section)>0);
  }
  TASK_CHECK(ov_rewind_enable(&vf,65536)==OV_EINVAL);
  ov_clear(&vf);
  free(s.data);
  return 0;
}

static int case_eviction_and_budget(void){
  rstream s,refstream;OggVorbis_File vf;
  float **pcm;int section;long k;
  ogg_int64_t lo,hi;
  long budget=65536;
  pcm_blob ref[1];
  int small_blocks=0,large_blocks=0;
  memset(&s,0,sizeof(s));
  memset(&refstream,0,sizeof(refstream));
  TASK_CHECK(encode_link(&s,1,44100,420000,0.25f,7,
                         &small_blocks,&large_blocks)==0);
  /* the generated material must exercise both Vorbis window sizes */
  TASK_CHECK(small_blocks>0 && large_blocks>0);
  TASK_CHECK(rb_append(&refstream,s.data,s.size)==0);
  refstream.size=s.size;
  TASK_CHECK(decode_reference(&refstream,ref,1,113)==1);
  TASK_CHECK(ref[0].frames==420000);

  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_rewind_enable(&vf,budget)==0);
  rewind_runtime *rt=vf.rewind_state;

  while((k=ov_read_float(&vf,&pcm,37,&section))>0){
    long payload=rewind_cache_payload_bytes(rt->cache);
    int cnt=rewind_cache_count(rt->cache);
    long sum=0;
    for(int i=0;i<cnt;i++)
      sum+=rewind_cache_get(rt->cache,i)->bytes;
    TASK_CHECK(payload==sum);
    /* a lone over-budget packet is allowed; otherwise the budget
       holds, and it always holds once more than one packet is kept */
    TASK_CHECK(payload<=budget || cnt==1);
  }
  TASK_CHECK(k==0);
  TASK_CHECK(rewind_cache_evictions(rt->cache)>0);
  TASK_CHECK(rewind_cache_payload_bytes(rt->cache)<=budget);

  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
  TASK_CHECK(lo>0);                 /* old history was retired */
  TASK_CHECK(hi==420000);           /* end == delivered frame count */

  /* the sliding window still reconstructs its suffix exactly, using
     only the packets that survived eviction */
  TASK_CHECK(ov_pcm_rewind(&vf,lo+64)==0);
  TASK_CHECK(ov_pcm_tell(&vf)==lo+64);
  TASK_CHECK(check_float_suffix(&vf,&ref[0],lo+64,7)==0);
  TASK_CHECK(ov_pcm_tell(&vf)==420000);

  ov_clear(&vf);
  free_blobs(ref,1);
  free(s.data);free(refstream.data);
  return 0;
}

static int case_oversized_packet_unit(void){
  rewind_cache *c=rewind_cache_create(REWIND_MIN_BYTES);
  ogg_packet big,small;
  ogg_int64_t lo,hi;
  TASK_CHECK(c!=NULL);
  memset(&big,0,sizeof(big));
  big.bytes=REWIND_MIN_BYTES+5000;
  big.packet=malloc((size_t)big.bytes);
  TASK_CHECK(big.packet!=NULL);
  memset(big.packet,0x5a,(size_t)big.bytes);
  big.granulepos=-1;

  /* the oversized packet is retained but nothing is recoverable yet */
  TASK_CHECK(rewind_cache_push(c,&big,0,0)==0);
  TASK_CHECK(rewind_cache_count(c)==1);
  TASK_CHECK(rewind_cache_range(c,&lo,&hi)==OV_FALSE);

  memset(&small,0,sizeof(small));
  small.bytes=128;
  small.packet=malloc(128);
  TASK_CHECK(small.packet!=NULL);
  memset(small.packet,0x33,128);
  TASK_CHECK(rewind_cache_push(c,&small,0,128)==0);
  /* the big packet could not share the budget and was retired */
  TASK_CHECK(rewind_cache_count(c)==1);
  TASK_CHECK(rewind_cache_range(c,&lo,&hi)==OV_FALSE);

  TASK_CHECK(rewind_cache_push(c,&small,128,256)==0);
  TASK_CHECK(rewind_cache_range(c,&lo,&hi)==0);
  TASK_CHECK(lo==128 && hi==256);
  TASK_CHECK(rewind_cache_locate(c,127)<0);
  TASK_CHECK(rewind_cache_locate(c,128)==1);
  TASK_CHECK(rewind_cache_locate(c,255)==1);
  TASK_CHECK(rewind_cache_locate(c,256)<0);

  free(big.packet);free(small.packet);
  rewind_cache_destroy(c);
  return 0;
}

static int case_range_query_inert(void){
  rstream s;OggVorbis_File vf;
  float **pcm;int section;
  ogg_int64_t lo,hi;
  long reads;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,2,32000,8192,0.3f,3,NULL,NULL)==0);
  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_rewind_enable(&vf,1<<20)==0);
  TASK_CHECK(ov_read_float(&vf,&pcm,256,&section)>0);

  reads=s.reads;
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
  TASK_CHECK(s.reads==reads);                 /* no input pulled */
  TASK_CHECK(ov_pcm_tell(&vf)>0);
  {
    ogg_int64_t tell=ov_pcm_tell(&vf);
    TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
    TASK_CHECK(ov_pcm_tell(&vf)==tell);       /* position untouched */
    TASK_CHECK(lo>=0 && hi>=tell-1);
  }
  ov_clear(&vf);
  free(s.data);
  return 0;
}

/* ================================================================= */
/* replay group                                                      */
/* ================================================================= */

static int case_float_suffix_readsizes(void){
  static const int sizes[]={1,7,113};
  rstream s;OggVorbis_File vf;
  pcm_blob ref[4];
  long links;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,2,48000,20000,0.3f,11,NULL,NULL)==0);
  links=decode_reference(&s,ref,4,113);
  TASK_CHECK(links==1);
  TASK_CHECK(ref[0].frames==20000);

  for(unsigned z=0;z<sizeof(sizes)/sizeof(sizes[0]);z++){
    int rsize=sizes[z];
    ogg_int64_t lo,hi;
    TASK_CHECK(rs_open(&s,&vf)==0);
    TASK_CHECK(ov_rewind_enable(&vf,1<<18)==0);
    {
      float **pcm;int section;
      while(ov_read_float(&vf,&pcm,53,&section)>0){}
    }
    TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
    ogg_int64_t targets[]={lo,lo+1,128,4097,(lo+hi)/2,hi-2,hi-1};
    for(unsigned t=0;t<sizeof(targets)/sizeof(targets[0]);t++){
      ogg_int64_t tg=targets[t];
      if(tg<lo||tg>=hi)continue;
      TASK_CHECK(ov_pcm_rewind(&vf,tg)==0);
      TASK_CHECK(ov_pcm_tell(&vf)==tg);
      TASK_CHECK(check_float_suffix(&vf,&ref[0],tg,rsize)==0);
      TASK_CHECK(ov_pcm_tell(&vf)==20000);
    }
    ov_clear(&vf);
  }
  free_blobs(ref,links);
  free(s.data);
  return 0;
}

/* compare one integer packing mode byte-for-byte against a fresh
   sequential decode advanced to the same link position */
static int int_mode_compare(int bigendian,int word,int sgned,
                             int ch,ogg_int64_t target,long frames,
                             rstream *data){
  rstream a,b;
  OggVorbis_File test,ref;
  memset(&a,0,sizeof(a));
  memset(&b,0,sizeof(b));
  TASK_CHECK(rb_append(&a,data->data,data->size)==0);
  TASK_CHECK(rb_append(&b,data->data,data->size)==0);
  a.size=b.size=data->size;
  TASK_CHECK(rs_open(&a,&test)==0);
  TASK_CHECK(ov_rewind_enable(&test,1<<18)==0);
  TASK_CHECK(rs_open(&b,&ref)==0);
  {
    float **pcm;int section;
    while(ov_read_float(&test,&pcm,64,&section)>0){}
  }
  {
    char buf[4096];int section;long done=0;
    while(done<target){
      long want=target-done;
      long bytes=want*ch*word;
      if(bytes>(long)sizeof(buf))bytes=sizeof(buf);
      long k=ov_read(&ref,buf,(int)bytes,bigendian,word,sgned,&section);
      TASK_CHECK(k>0);
      done+=k/(word*ch);
    }
  }
  TASK_CHECK(ov_pcm_rewind(&test,target)==0);
  for(;;){
    char buf1[4096],buf2[4096];
    int s1,s2;
    long x=ov_read_filter(&test,buf1,sizeof(buf1),bigendian,word,
                          sgned,&s1,NULL,NULL);
    long y=ov_read(&ref,buf2,sizeof(buf2),bigendian,word,sgned,&s2);
    TASK_CHECK(x==y);
    if(x==0)break;
    TASK_CHECK(memcmp(buf1,buf2,(size_t)x)==0);
  }
  TASK_CHECK(ov_pcm_tell(&test)==frames);
  ov_clear(&test);ov_clear(&ref);
  free(a.data);free(b.data);
  return 0;
}

static int case_integer_byteexact(void){
  rstream data;
  ogg_int64_t lo,hi;
  OggVorbis_File probe;
  memset(&data,0,sizeof(data));
  TASK_CHECK(encode_link(&data,2,44100,12000,0.35f,5,NULL,NULL)==0);
  TASK_CHECK(rs_open(&data,&probe)==0);
  TASK_CHECK(ov_rewind_enable(&probe,1<<18)==0);
  {
    float **pcm;int section;
    while(ov_read_float(&probe,&pcm,64,&section)>0){}
  }
  TASK_CHECK(ov_rewind_range(&probe,&lo,&hi)==0);
  ov_clear(&probe);
  ogg_int64_t target=lo+500;

  /* 16-bit little-endian signed, 16-bit big-endian signed, 8-bit
     unsigned and 8-bit signed must all be byte identical to a
     sequential decode */
  TASK_CHECK(int_mode_compare(0,2,1,2,target,12000,&data)==0);
  TASK_CHECK(int_mode_compare(1,2,1,2,target,12000,&data)==0);
  TASK_CHECK(int_mode_compare(0,1,0,2,target,12000,&data)==0);
  TASK_CHECK(int_mode_compare(0,1,1,2,target,12000,&data)==0);
  free(data.data);
  return 0;
}

static int case_partial_pcm_and_catchup(void){
  rstream s;OggVorbis_File vf;
  pcm_blob ref[2];
  long links;
  ogg_int64_t lo,hi;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,1,48000,30000,0.3f,13,NULL,NULL)==0);
  links=decode_reference(&s,ref,2,113);
  TASK_CHECK(links==1);

  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_rewind_enable(&vf,1<<18)==0);
  /* leave undelivered PCM inside the library between every step */
  {
    float **pcm;int section;
    TASK_CHECK(ov_read_float(&vf,&pcm,3,&section)==3);
    TASK_CHECK(ov_read_float(&vf,&pcm,1,&section)==1);
  }
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
  ogg_int64_t target=lo+10;
  long reads_at_rewind;
  TASK_CHECK(ov_pcm_rewind(&vf,target)==0);
  reads_at_rewind=s.reads;
  TASK_CHECK(ov_pcm_tell(&vf)==target);
  rewind_runtime *rt=vf.rewind_state;
  ogg_int64_t watermark=rt->watermark;
  TASK_CHECK(watermark>target);

  /* up to the unpacked high water mark the replay is served entirely
     from cached packets; the input callback is never called */
  {
    float **pcm;int section;
    long got=0,want=(long)(watermark-target);
    while(got<want){
      int req=want-got;if(req>7)req=7;
      long k=ov_read_float(&vf,&pcm,req,&section);
      TASK_CHECK(k>0);
      for(long i=0;i<k;i++)
        TASK_CHECK(fabsf(pcm[0][i]-
          ref[0].pcm[(target+got+i)*ref[0].ch])<2e-6f);
      got+=(int)k;
    }
    TASK_CHECK(got==want);
    TASK_CHECK(s.reads==reads_at_rewind);
  }

  /* after catching the high water mark, forward decoding resumes and
     the rest of the stream matches the reference */
  TASK_CHECK(check_float_suffix(&vf,&ref[0],watermark,113)==0);
  TASK_CHECK(ov_pcm_tell(&vf)==30000);
  TASK_CHECK(s.reads>reads_at_rewind);

  ov_clear(&vf);
  free_blobs(ref,links);
  free(s.data);
  return 0;
}

static int case_eof_and_repeated_target(void){
  rstream s;OggVorbis_File vf;
  pcm_blob ref[2];
  long links;
  ogg_int64_t lo,hi;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,2,32000,9000,0.3f,17,NULL,NULL)==0);
  links=decode_reference(&s,ref,2,113);
  TASK_CHECK(links==1);

  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_rewind_enable(&vf,1<<19)==0);
  {
    float **pcm;int section;long k;
    while((k=ov_read_float(&vf,&pcm,113,&section))>0){}
    TASK_CHECK(k==0); /* reached EOF */
  }
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
  TASK_CHECK(hi==9000);                   /* end == delivered count */
  for(int rep=0;rep<3;rep++){
    ogg_int64_t tg=hi-257;
    TASK_CHECK(ov_pcm_rewind(&vf,tg)==0);
    TASK_CHECK(ov_pcm_tell(&vf)==tg);
    TASK_CHECK(check_float_suffix(&vf,&ref[0],tg,113)==0);
    TASK_CHECK(ov_pcm_tell(&vf)==9000);
  }
  ov_clear(&vf);
  free_blobs(ref,links);
  free(s.data);
  return 0;
}

/* re-rewinding while a previous replay is still in flight (and again
   after catching up) must never drop, duplicate or shift a frame */
static int case_nested_rewind(void){
  rstream s;OggVorbis_File vf;
  pcm_blob ref[2];
  long links;
  ogg_int64_t lo,hi;
  float **pcm;int section;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,2,48000,40000,0.3f,19,NULL,NULL)==0);
  links=decode_reference(&s,ref,2,113);
  TASK_CHECK(links==1);

  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_rewind_enable(&vf,1<<19)==0);
  {
    long k;
    while((k=ov_read_float(&vf,&pcm,113,&section))>0){}
    TASK_CHECK(k==0);
  }
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);

  /* first rewind, consume only part of the replay (stay cached) */
  ogg_int64_t t1=lo+3000;
  TASK_CHECK(ov_pcm_rewind(&vf,t1)==0);
  {
    long got=0;
    while(got<500){
      int req=500-(int)got;if(req>37)req=37;
      long k=ov_read_float(&vf,&pcm,req,&section);
      TASK_CHECK(k>0);
      for(long i=0;i<k;i++)
        for(int j=0;j<2;j++)
          TASK_CHECK(fabsf(pcm[j][i]-
            ref[0].pcm[(t1+got+i)*2+j])<2e-6f);
      got+=(int)k;
    }
    TASK_CHECK(got==500);
    TASK_CHECK(ov_pcm_tell(&vf)==t1+500);
  }
  /* re-rewind to a different target from inside the replay */
  ogg_int64_t t2=lo+1200;
  TASK_CHECK(ov_pcm_rewind(&vf,t2)==0);
  TASK_CHECK(ov_pcm_tell(&vf)==t2);
  TASK_CHECK(check_float_suffix(&vf,&ref[0],t2,7)==0);

  /* again, from a point after catching the old high water mark */
  ogg_int64_t t3=lo+8000;
  TASK_CHECK(ov_pcm_rewind(&vf,t3)==0);
  TASK_CHECK(check_float_suffix(&vf,&ref[0],t3,113)==0);
  TASK_CHECK(ov_pcm_tell(&vf)==40000);

  /* a final rewind and a full suffix must still line up exactly */
  TASK_CHECK(ov_pcm_rewind(&vf,lo)==0);
  TASK_CHECK(check_float_suffix(&vf,&ref[0],lo,53)==0);
  TASK_CHECK(ov_pcm_tell(&vf)==40000);

  ov_clear(&vf);
  free_blobs(ref,links);
  free(s.data);
  return 0;
}

static int case_filter_only_delivered(void){
  rstream a,b;
  OggVorbis_File test,ref;
  ogg_int64_t lo,hi;
  long filtered_test=0,filtered_ref=0;
  memset(&a,0,sizeof(a));
  memset(&b,0,sizeof(b));
  TASK_CHECK(encode_link(&a,2,48000,10000,0.3f,23,NULL,NULL)==0);
  TASK_CHECK(rb_append(&b,a.data,a.size)==0);
  b.size=a.size;

  TASK_CHECK(rs_open(&a,&test)==0);
  TASK_CHECK(ov_rewind_enable(&test,1<<18)==0);
  TASK_CHECK(rs_open(&b,&ref)==0);
  {
    float **pcm;int section;
    while(ov_read_float(&test,&pcm,71,&section)>0){}
  }
  TASK_CHECK(ov_rewind_range(&test,&lo,&hi)==0);
  ogg_int64_t target=lo+777;
  TASK_CHECK(skip_int_frames_filtered(&ref,(long)target,2,
                                      &filtered_ref)==0);
  long ref_baseline=filtered_ref;
  TASK_CHECK(ov_pcm_rewind(&test,target)==0);
  for(;;){
    char buf1[4096],buf2[4096];
    int s1,s2;
    long x=ov_read_filter(&test,buf1,sizeof(buf1),0,2,1,&s1,
                          scale_filter,&filtered_test);
    long y=ov_read_filter(&ref,buf2,sizeof(buf2),0,2,1,&s2,
                          scale_filter,&filtered_ref);
    TASK_CHECK(x==y);
    if(x==0)break;
    TASK_CHECK(memcmp(buf1,buf2,(size_t)x)==0);
  }
  /* the filter saw exactly the delivered suffix samples, never the
     preroll; both decoders applied it to the same frame range */
  TASK_CHECK(filtered_test>0);
  TASK_CHECK(filtered_test==filtered_ref-ref_baseline);
  TASK_CHECK(filtered_test==10000-target);

  ov_clear(&test);ov_clear(&ref);
  free(a.data);free(b.data);
  return 0;
}

static int case_chained_switch(void){
  rstream s;OggVorbis_File vf;
  pcm_blob ref[4];
  long links;
  ogg_int64_t lo,hi;
  int ws,wl;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,1,44100,7000,0.3f,31,NULL,NULL)==0);
  TASK_CHECK(encode_link(&s,2,32000,5000,0.3f,47,&ws,&wl)==0);
  TASK_CHECK(ws>0 && wl>0);
  links=decode_reference(&s,ref,4,113);
  TASK_CHECK(links==2);
  TASK_CHECK(ref[0].ch==1 && ref[1].ch==2);

  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_rewind_enable(&vf,1<<19)==0);
  {
    float **pcm;int section=0;
    long k;
    /* play most of link 0, rewind inside it, then cross the link */
    while(section==0){
      k=ov_read_float(&vf,&pcm,97,&section);
      TASK_CHECK(k>0);
      if(ov_pcm_tell(&vf)>5000)break;
    }
    TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
    TASK_CHECK(ov_pcm_rewind(&vf,lo+50)==0);
    TASK_CHECK(check_float_suffix(&vf,&ref[0],lo+50,113)==0);

    /* drive through the boundary; the old window must be discarded.
       A page-sequence OV_HOLE right at an independently encoded chain
       boundary is ordinary vorbisfile behavior (it occurs with the
       window disabled too), so it is tolerated here. */
    int saw_new_link=0;
    while(1){
      k=ov_read_float(&vf,&pcm,113,&section);
      if(k==OV_HOLE)continue;
      if(k<=0)break;
      if(section==1){saw_new_link=1;break;}
    }
    TASK_CHECK(saw_new_link);
    /* the old link-0 window is gone and the new segment accumulates
       from frame 0: any reported range is strictly inside link 1, and
       old link-0 coordinates are no longer addressable */
    {
      int rr=ov_rewind_range(&vf,&lo,&hi);
      if(rr==0){
        TASK_CHECK(lo>=0 && hi<=5000);
      }else{
        TASK_CHECK(rr==OV_FALSE && lo==0 && hi==0);
      }
      TASK_CHECK(ov_pcm_rewind(&vf,6000)==OV_EINVAL);
    }

    /* accumulate the second link, whose coordinates restart at 0 */
    while(1){
      k=ov_read_float(&vf,&pcm,113,&section);
      if(k==OV_HOLE)continue;
      if(k<=0)break;
      TASK_CHECK(section==1);
    }
    TASK_CHECK(k==0);
    TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
    TASK_CHECK(lo>=0 && hi==5000);        /* end == frame count */
    TASK_CHECK(ov_pcm_rewind(&vf,lo+200)==0);
    TASK_CHECK(ov_pcm_tell(&vf)==lo+200);
    TASK_CHECK(check_float_suffix(&vf,&ref[1],lo+200,7)==0);
  }
  ov_clear(&vf);
  free_blobs(ref,links);
  free(s.data);
  return 0;
}

static int case_hole_clears_window(void){
  rstream s;OggVorbis_File vf;
  float **pcm;int section;long k;
  ogg_int64_t lo,hi;
  pcm_blob after;
  ogg_int64_t after_start=-1;
  int holes=0;
  size_t page_starts[256];
  int npages=0;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,2,48000,160000,0.3f,59,NULL,NULL)==0);

  /* locate interior Ogg page boundaries by walking the framing */
  {
    size_t at=0;
    while(at+27<=s.size && npages<256){
      const unsigned char *h=s.data+at;
      long hlen,blen,seg;
      if(memcmp(h,"OggS",4))break;
      hlen=(long)h[26]+27;
      if(at+(size_t)hlen>s.size)break;
      blen=0;
      for(seg=0;seg<h[26];seg++)blen+=h[27+seg];
      page_starts[npages++]=at;
      at+=(size_t)(hlen+blen);
    }
  }
  TASK_CHECK(npages>8);
  s.mask_begin=page_starts[4];
  s.mask_end=page_starts[5];

  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_rewind_enable(&vf,1<<19)==0);
  rewind_runtime *hrt=vf.rewind_state;
  blob_init(&after,2);
  while(1){
    k=ov_read_float(&vf,&pcm,79,&section);
    if(k==0)break;
    if(k==OV_HOLE){
      holes++;
      TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==OV_FALSE);
      /* link-local coordinate of the first samples after recovery */
      after_start=hrt->link_pos;
      continue;
    }
    TASK_CHECK(k>0);
    if(after_start>=0)TASK_CHECK(blob_put(&after,pcm,k)==0);
  }
  TASK_CHECK(holes>0);
  TASK_CHECK(after_start>=0);
  TASK_CHECK(after.frames>256);

  /* recovery rebuilt a fresh window; rewinds inside it reproduce the
     exact post-hole samples captured during forward playback */
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
  TASK_CHECK(lo>=after_start);
  {
    ogg_int64_t tg=lo+33;
    long off=(long)(tg-after_start);
    TASK_CHECK(off>=0 && off<after.frames);
    TASK_CHECK(ov_pcm_rewind(&vf,tg)==0);
    TASK_CHECK(check_float_suffix(&vf,&after,off,113)==0);
  }
  ov_clear(&vf);
  blob_free(&after);
  free(s.data);
  return 0;
}

static int case_synthesis_accounting(void){
  rstream s;OggVorbis_File vf;
  float **pcm;int section;
  ogg_int64_t lo,hi;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,2,48000,480000,0.25f,67,NULL,NULL)==0);
  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_rewind_enable(&vf,65536)==0);
  while(ov_read_float(&vf,&pcm,113,&section)>0){}

  rewind_runtime *rt=vf.rewind_state;
  long total_before=rt->synth_total;
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
  ogg_int64_t tg=lo+1000;
  TASK_CHECK(ov_pcm_rewind(&vf,tg)==0);
  long preroll_synths=rt->synth_total-total_before;
  /* localizing only rebuilds the target packet and its overlap
     predecessor, never the whole link */
  TASK_CHECK(preroll_synths>=1 && preroll_synths<=4);
  TASK_CHECK(rt->synth_replay>=preroll_synths);

  /* listen back to the high water mark, then confirm the retained
     payload stayed bounded across the whole run */
  {
    long k;
    while((k=ov_read_float(&vf,&pcm,113,&section))>0){}
    TASK_CHECK(k==0);
  }
  TASK_CHECK(rewind_cache_payload_bytes(rt->cache)<=
             rewind_cache_budget(rt->cache));
  TASK_CHECK(rewind_cache_evictions(rt->cache)>0);
  printf("ACCOUNTING payload=%ld entries=%d evictions=%ld preroll_synths=%ld\n",
         rewind_cache_payload_bytes(rt->cache),
         rewind_cache_count(rt->cache),
         rewind_cache_evictions(rt->cache),preroll_synths);
  ov_clear(&vf);
  free(s.data);
  return 0;
}

/* ================================================================= */
/* lifecycle group                                                   */
/* ================================================================= */

static int case_disabled_enoseek(void){
  rstream s;OggVorbis_File vf;
  ogg_int64_t lo,hi;
  float **pcm;int section;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,2,48000,4096,0.3f,71,NULL,NULL)==0);
  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==OV_ENOSEEK);
  TASK_CHECK(ov_pcm_rewind(&vf,0)==OV_ENOSEEK);
  TASK_CHECK(ov_pcm_seek(&vf,0)==OV_ENOSEEK);
  /* ordinary sequential playback is unaffected */
  long n=0;
  while(1){
    long k=ov_read_float(&vf,&pcm,61,&section);
    if(k==0)break;
    TASK_CHECK(k>0);
    n+=k;
  }
  TASK_CHECK(n==4096);
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==OV_ENOSEEK);
  ov_clear(&vf);
  free(s.data);
  return 0;
}

static int case_halfrate_rules(void){
  rstream s;OggVorbis_File vf;
  ogg_int64_t lo,hi;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,2,48000,6000,0.3f,73,NULL,NULL)==0);

  /* halfrate already on: enable refused */
  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_halfrate(&vf,1)==0);
  TASK_CHECK(ov_halfrate_p(&vf)==1);
  TASK_CHECK(ov_rewind_enable(&vf,65536)==OV_EINVAL);
  ov_clear(&vf);

  /* once armed, halfrate switches are refused and state survives */
  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_rewind_enable(&vf,65536)==0);
  TASK_CHECK(ov_halfrate(&vf,1)==OV_EINVAL);
  TASK_CHECK(ov_halfrate_p(&vf)==0);
  {
    float **pcm;int section;
    TASK_CHECK(ov_read_float(&vf,&pcm,128,&section)>0);
  }
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
  TASK_CHECK(ov_halfrate(&vf,0)==OV_EINVAL); /* already unset, still
                                                refused while armed */
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
  ov_clear(&vf);
  free(s.data);
  return 0;
}

static int case_bad_targets_unchanged(void){
  rstream s;OggVorbis_File vf;
  ogg_int64_t lo,hi;
  float **pcm;int section;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,1,48000,12000,0.3f,79,NULL,NULL)==0);
  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_rewind_enable(&vf,1<<19)==0);
  while(ov_read_float(&vf,&pcm,113,&section)>0){}
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);

  TASK_CHECK(ov_pcm_rewind(&vf,-1)==OV_EINVAL);
  TASK_CHECK(ov_pcm_rewind(&vf,lo-1)==OV_EINVAL);
  TASK_CHECK(ov_pcm_rewind(&vf,hi+1)==OV_EINVAL);
  TASK_CHECK(ov_pcm_rewind(&vf,1LL<<40)==OV_EINVAL);
  TASK_CHECK(ov_pcm_tell(&vf)==12000); /* unmoved */

  TASK_CHECK(ov_pcm_rewind(&vf,lo)==0);
  TASK_CHECK(ov_pcm_tell(&vf)==lo);
  TASK_CHECK(ov_pcm_rewind(&vf,hi+1)==OV_EINVAL);
  TASK_CHECK(ov_pcm_tell(&vf)==lo);    /* failed rewind keeps spot */
  ov_clear(&vf);
  free(s.data);
  return 0;
}

static int case_open_clear_cycles(void){
  rstream s;OggVorbis_File vf;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,2,48000,5000,0.3f,83,NULL,NULL)==0);
  for(int cycle=0;cycle<8;cycle++){
    float **pcm;int section;long n=0;
    TASK_CHECK(rs_open(&s,&vf)==0);
    TASK_CHECK(ov_rewind_enable(&vf,65536)==0);
    while(1){
      long k=ov_read_float(&vf,&pcm,47,&section);
      if(k==0)break;
      TASK_CHECK(k>0);
      n+=k;
    }
    TASK_CHECK(n==5000);
    ogg_int64_t lo,hi;
    TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
    TASK_CHECK(ov_pcm_rewind(&vf,lo)==0);
    ov_clear(&vf);
  }
  free(s.data);
  return 0;
}

static int case_link_reset_semantics(void){
  rstream s;OggVorbis_File vf;
  pcm_blob ref[4];
  long links;
  ogg_int64_t lo,hi;
  int section=-1;
  float **pcm;long k;
  memset(&s,0,sizeof(s));
  TASK_CHECK(encode_link(&s,2,48000,6000,0.3f,89,NULL,NULL)==0);
  TASK_CHECK(encode_link(&s,1,22050,4000,0.3f,97,NULL,NULL)==0);
  links=decode_reference(&s,ref,4,113);
  TASK_CHECK(links==2);

  TASK_CHECK(rs_open(&s,&vf)==0);
  TASK_CHECK(ov_rewind_enable(&vf,1<<19)==0);
  while(section<0){
    k=ov_read_float(&vf,&pcm,113,&section);
    TASK_CHECK(k>0);
  }
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
  ogg_int64_t old_hi=hi;
  TASK_CHECK(old_hi>0);

  /* cross into the second link (tolerate the boundary OV_HOLE that
     independently encoded chained links produce even without the
     rewind window) */
  while(1){
    k=ov_read_float(&vf,&pcm,113,&section);
    if(k==OV_HOLE)continue;
    if(k<=0)break;
    if(section==1)break;
  }
  TASK_CHECK(section==1);
  TASK_CHECK(ov_info(&vf,-1)->channels==1);
  TASK_CHECK(ov_info(&vf,-1)->rate==22050);
  /* the link-0 window is gone; link 1 re-accumulates from frame 0
     (a range may already exist if a few new packets were decoded) */
  {
    int rr=ov_rewind_range(&vf,&lo,&hi);
    if(rr==0)TASK_CHECK(lo>=0 && hi<=4000);
    else TASK_CHECK(rr==OV_FALSE && lo==0 && hi==0);
    TASK_CHECK(ov_pcm_rewind(&vf,5000)==OV_EINVAL);
  }

  while(1){
    k=ov_read_float(&vf,&pcm,113,&section);
    if(k==OV_HOLE)continue;
    if(k<=0)break;
    TASK_CHECK(section==1);
  }
  TASK_CHECK(k==0);
  TASK_CHECK(ov_rewind_range(&vf,&lo,&hi)==0);
  TASK_CHECK(hi==4000);                 /* new segment counts from 0 */
  TASK_CHECK(ov_pcm_rewind(&vf,lo+10)==0);
  TASK_CHECK(ov_pcm_tell(&vf)==lo+10);
  TASK_CHECK(check_float_suffix(&vf,&ref[1],lo+10,113)==0);

  ov_clear(&vf);
  free_blobs(ref,links);
  free(s.data);
  return 0;
}

/* ---------------------------------------------------------------- */

const struct task_case task_cases[]={
  {"window","enable_validation",case_enable_validation},
  {"window","eviction_and_budget",case_eviction_and_budget},
  {"window","oversized_packet_unit",case_oversized_packet_unit},
  {"window","range_query_inert",case_range_query_inert},

  {"replay","float_suffix_readsizes",case_float_suffix_readsizes},
  {"replay","integer_byteexact",case_integer_byteexact},
  {"replay","partial_pcm_and_catchup",case_partial_pcm_and_catchup},
  {"replay","eof_and_repeated_target",case_eof_and_repeated_target},
  {"replay","nested_rewind",case_nested_rewind},
  {"replay","filter_only_delivered",case_filter_only_delivered},
  {"replay","chained_switch",case_chained_switch},
  {"replay","hole_clears_window",case_hole_clears_window},
  {"replay","synthesis_accounting",case_synthesis_accounting},

  {"lifecycle","disabled_enoseek",case_disabled_enoseek},
  {"lifecycle","halfrate_rules",case_halfrate_rules},
  {"lifecycle","bad_targets_unchanged",case_bad_targets_unchanged},
  {"lifecycle","open_clear_cycles",case_open_clear_cycles},
  {"lifecycle","link_reset_semantics",case_link_reset_semantics},
};
const size_t task_case_count=sizeof(task_cases)/sizeof(task_cases[0]);
