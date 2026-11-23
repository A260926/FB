/********************************************************************
 *                                                                  *
 * rewind_stream.c - device-free demonstration of the non-seekable  *
 * Ogg/Vorbis rewind window.                                        *
 *                                                                  *
 * The program synthesizes audio in memory, encodes it to a complete *
 * Ogg/Vorbis byte stream, then plays that stream back through a     *
 * strictly read-only callback (no seek/tell).  After forward        *
 * playback it rewinds inside the retained window and proves that    *
 * the rebuilt suffix is sample-identical to the corresponding       *
 * portion of a second, purely sequential decode.  On success it     *
 * prints VORBIS_REWIND_DEMO_OK.                                     *
 *                                                                  *
 ********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>
#include <vorbis/vorbisfile.h>

#define DEMO_CHANNELS 2
#define DEMO_RATE     48000
#define DEMO_FRAMES   24000
#define DEMO_BUDGET   (1L<<18)

/* in-memory, read-only data source with counted short reads */
typedef struct {
  const unsigned char *data;
  size_t               size;
  size_t               pos;
  long                 reads;
} demo_src;

static size_t demo_read(void *ptr,size_t size,size_t nmemb,void *arg){
  demo_src *s=arg;
  size_t bytes=size*nmemb;
  s->reads++;
  if(bytes>s->size-s->pos)bytes=s->size-s->pos;
  bytes-=bytes%size;
  memcpy(ptr,s->data+s->pos,bytes);
  s->pos+=bytes;
  return bytes/size;
}

/* a deterministic, nonzero signal mixing steady tones with isolated
   transients, so the encoder uses both Vorbis window lengths */
static float demo_sample(long frame,int channel,unsigned seed){
  float v=0.08f*sinf(frame*0.021f+channel*0.6f)+
          0.04f*sinf(frame*0.0073f+1.3f);
  if((frame+seed)%2999<8)v+=0.85f;
  if((frame+seed)%1777<3)v-=0.6f;
  return v*(0.9f+0.1f*channel)+1e-4f;
}

static int demo_append(unsigned char **buf,size_t *len,size_t *cap,
                       const void *p,size_t n){
  if(*len+n>*cap){
    size_t c=(*len+n)*2+4096;
    unsigned char *q=realloc(*buf,c);
    if(!q)return -1;
    *buf=q;*cap=c;
  }
  memcpy(*buf+*len,p,n);
  *len+=n;
  return 0;
}

static int demo_page(unsigned char **buf,size_t *len,size_t *cap,
                     ogg_page *og){
  return demo_append(buf,len,cap,og->header,og->header_len) ||
         demo_append(buf,len,cap,og->body,og->body_len);
}

/* encode the generated signal into a self-contained Ogg stream */
static int demo_encode(unsigned char **out,size_t *out_len){
  vorbis_info vi;vorbis_comment vc;vorbis_dsp_state vd;vorbis_block vb;
  ogg_stream_state os;ogg_packet hdr[3],op;ogg_page og;
  unsigned char *buf=NULL;size_t len=0,cap=0;
  long pos=0;

  vorbis_info_init(&vi);
  if(vorbis_encode_init_vbr(&vi,DEMO_CHANNELS,DEMO_RATE,0.3f))goto fail;
  vorbis_comment_init(&vc);
  if(vorbis_analysis_init(&vd,&vi))goto fail;
  vorbis_block_init(&vd,&vb);
  ogg_stream_init(&os,0x72657769);

  vorbis_analysis_headerout(&vd,&vc,&hdr[0],&hdr[1],&hdr[2]);
  for(int i=0;i<3;i++)ogg_stream_packetin(&os,&hdr[i]);
  while(ogg_stream_flush(&os,&og))
    if(demo_page(&buf,&len,&cap,&og))goto fail;

  while(pos<DEMO_FRAMES){
    long n=DEMO_FRAMES-pos;
    if(n>2048)n=2048;
    float **pcm=vorbis_analysis_buffer(&vd,(int)n);
    for(long i=0;i<n;i++)
      for(int j=0;j<DEMO_CHANNELS;j++)
        pcm[j][i]=demo_sample(pos+i,j,11);
    vorbis_analysis_wrote(&vd,(int)n);
    pos+=n;
    while(vorbis_analysis_blockout(&vd,&vb)==1){
      vorbis_analysis(&vb,NULL);
      vorbis_bitrate_addblock(&vb);
      while(vorbis_bitrate_flushpacket(&vd,&op)){
        ogg_stream_packetin(&os,&op);
        while(ogg_stream_pageout(&os,&og))
          if(demo_page(&buf,&len,&cap,&og))goto fail;
      }
    }
  }
  vorbis_analysis_wrote(&vd,0);
  while(vorbis_analysis_blockout(&vd,&vb)==1){
    vorbis_analysis(&vb,NULL);
    vorbis_bitrate_addblock(&vb);
    while(vorbis_bitrate_flushpacket(&vd,&op)){
      ogg_stream_packetin(&os,&op);
      while(ogg_stream_pageout(&os,&og))
        if(demo_page(&buf,&len,&cap,&og))goto fail;
    }
  }
  while(ogg_stream_flush(&os,&og))
    if(demo_page(&buf,&len,&cap,&og))goto fail;

  ogg_stream_clear(&os);
  vorbis_block_clear(&vb);
  vorbis_dsp_clear(&vd);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  *out=buf;*out_len=len;
  return 0;
fail:
  free(buf);
  ogg_stream_clear(&os);
  vorbis_block_clear(&vb);
  vorbis_dsp_clear(&vd);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  return -1;
}

/* fully sequential decode, kept as the ground-truth suffix */
static int demo_decode_ref(const unsigned char *data,size_t len,
                           float *flat){
  demo_src s={data,len,0,0};
  ov_callbacks cb;OggVorbis_File vf;
  memset(&cb,0,sizeof(cb));
  cb.read_func=demo_read;
  if(ov_open_callbacks(&s,&vf,NULL,0,cb))return -1;
  float **pcm;int section;long got=0;
  while(1){
    long k=ov_read_float(&vf,&pcm,113,&section);
    if(k==0)break;
    if(k<0){ov_clear(&vf);return -1;}
    for(long i=0;i<k;i++)
      for(int j=0;j<DEMO_CHANNELS;j++)
        flat[(got+i)*DEMO_CHANNELS+j]=pcm[j][i];
    got+=k;
  }
  ov_clear(&vf);
  return got==DEMO_FRAMES?0:-1;
}

int main(void){
  unsigned char *stream=NULL;size_t stream_len=0;
  float *reference=malloc((size_t)DEMO_FRAMES*DEMO_CHANNELS*
                          sizeof(*reference));
  if(!reference)return 1;

  if(demo_encode(&stream,&stream_len)){
    fprintf(stderr,"encode failed\n");return 1;
  }
  if(demo_decode_ref(stream,stream_len,reference)){
    fprintf(stderr,"reference decode failed\n");return 1;
  }

  demo_src s={stream,stream_len,0,0};
  ov_callbacks cb;OggVorbis_File vf;
  memset(&cb,0,sizeof(cb));
  cb.read_func=demo_read;                 /* read-only: no seek/tell */
  if(ov_open_callbacks(&s,&vf,NULL,0,cb)){
    fprintf(stderr,"open failed\n");return 1;
  }
  if(ov_seekable(&vf)){
    fprintf(stderr,"source unexpectedly seekable\n");return 1;
  }
  if(ov_rewind_enable(&vf,DEMO_BUDGET)){
    fprintf(stderr,"rewind enable failed\n");return 1;
  }

  /* forward playback, deliberately using small reads */
  float **pcm;int section;long played=0;
  while(1){
    long k=ov_read_float(&vf,&pcm,37,&section);
    if(k==0)break;
    if(k<0){fprintf(stderr,"decode error %ld\n",k);return 1;}
    played+=k;
  }
  if(played!=DEMO_FRAMES){fprintf(stderr,"short forward decode\n");return 1;}

  ogg_int64_t begin,end;
  if(ov_rewind_range(&vf,&begin,&end)){
    fprintf(stderr,"no rewind range available\n");return 1;
  }
  ogg_int64_t target=begin+1234;
  long reads_before=s.reads;
  if(ov_pcm_rewind(&vf,target)){
    fprintf(stderr,"rewind failed\n");return 1;
  }
  if(ov_pcm_tell(&vf)!=target){
    fprintf(stderr,"rewind position wrong\n");return 1;
  }

  /* the replay must not call the input callback at all */
  long got=0;
  while(got<DEMO_FRAMES-target){
    long k=ov_read_float(&vf,&pcm,7,&section);
    if(k<=0)break;
    for(long i=0;i<k;i++)
      for(int j=0;j<DEMO_CHANNELS;j++){
        float a=pcm[j][i];
        float b=reference[(target+got+i)*DEMO_CHANNELS+j];
        if(fabsf(a-b)>2e-6f){
          fprintf(stderr,"suffix mismatch at %ld\n",got+i);return 1;
        }
      }
    got+=k;
  }
  if(got!=DEMO_FRAMES-target){fprintf(stderr,"short replay\n");return 1;}
  if(s.reads!=reads_before){
    fprintf(stderr,"replay touched the input source\n");return 1;
  }

  ov_clear(&vf);
  free(stream);
  free(reference);
  printf("rewind_demo frames=%ld range=[%lld,%lld) rewound_to=%lld "
         "replay_frames=%ld reads_frozen=%ld\n",
         (long)DEMO_FRAMES,(long long)begin,(long long)end,
         (long long)target,got,reads_before);
  puts("VORBIS_REWIND_DEMO_OK");
  return 0;
}
