/*
 * target_cases.c - numerical regression for Vorbis I mapping0 channel
 * coupling.
 *
 * The expected values in this file are never captured from the decoder
 * under test.  They are computed from an independent model of the
 * Vorbis I mapping0 rules:
 *
 *   1. the forward coupling definition (magnitude taken from the
 *      channel with the larger absolute value, sign carried by the
 *      angle), from which the inverse relation is re-derived as an
 *      affine sign-quadrant map;
 *   2. a closed-form inverse MDCT written directly from cosine basis
 *      functions (production mdct_backward is never used for expected
 *      values);
 *   3. a closed-form Vorbis window and an independently written
 *      overlap-add assembly matching the public synthesis buffering
 *      geometry.
 *
 * Production decoding runs through the real mapping0 backend (and, for
 * the packet group, the whole public vorbis_synthesis* chain).  Test
 * coefficients reach the production inverse coupling through legal
 * bitstreams; the only test-only seam is the VORBIS_COUPLING_TRACE
 * read-only observation point in mapping0.c.
 */

#include "check.h"

#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#include <ogg/ogg.h>

#include "misc.h"
#include "codebook.h"
#include "codec_internal.h"
#include "backends.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Some model helpers are only referenced from the TRACE-only snapshot
 * checks; keep them warning-free in ordinary (non-trace) builds. */
#if defined(__GNUC__) || defined(__clang__)
#define TC_UNUSED __attribute__((unused))
#else
#define TC_UNUSED
#endif

/* ------------------------------------------------------------------ */
/* small utilities                                                     */
/* ------------------------------------------------------------------ */

static void *xmalloc(size_t n){
  void *p=malloc(n ? n : 1);
  if(!p){fprintf(stderr,"ASSERT out of memory\n");exit(42);}
  return p;
}
static void *xcalloc(size_t n,size_t s){
  void *p=calloc(n ? n : 1,s);
  if(!p){fprintf(stderr,"ASSERT out of memory\n");exit(42);}
  return p;
}

static TC_UNUSED int fsame(float a,float b,double tol){
  double d=(double)a-(double)b;
  if(d<0)d=-d;
  return d<=tol;
}
static int dsame(double a,double b,double tol){
  double d=a-b;
  if(d<0)d=-d;
  return d<=tol;
}

/* ------------------------------------------------------------------ */
/* independent mapping0 coupling model                                 */
/* ------------------------------------------------------------------ */

/*
 * Forward definition (Vorbis I, section 8.4.2; same convention the
 * encoder implements in psy.c).  Given two coupled channel samples
 * (x on the magnitude channel, y on the angle channel), pick the
 * larger absolute value as the magnitude m and let the angle a carry
 * the sign relationship:
 *
 *   |x| >= |y| :  m = sx|x|,            a = sx|x| - sy|y|
 *   |x| <  |y| :  m = sy|y|,            a = sy|y| - sx|x|
 *
 * where sx,sy are the signs of x,y.  Solving these two piecewise
 * linear equations for (x,y) gives four affine quadrant rules.  The
 * inverse below applies those rules to the transmitted (m,a); zero
 * inputs fall out of the same rules and need no special case, which
 * the +0/-0 test cases exercise explicitly.
 */
static void model_inverse_pair(float m,float a,float *x,float *y){
  if(m>0.f){
    if(a>0.f){
      *x=m;          /* quadrant ++ */
      *y=m-a;
    }else{
      *y=m;          /* quadrant +- */
      *x=m+a;
    }
  }else{
    if(a>0.f){
      *x=m;          /* quadrant -+ */
      *y=m+a;
    }else{
      *y=m;          /* quadrant -- */
      *x=m-a;
    }
  }
}

/*
 * Forward model used by the bitstream builder to decide which
 * magnitude/angle values to transmit.  The channel with the larger
 * absolute sample supplies the magnitude (ties go to the angle
 * channel); the angle is the signed difference selected by the sign
 * of that dominant sample:
 *
 *   |x| >  |y| : m = x,  a = (x > 0) ? x - y : y - x
 *   |x| <= |y| : m = y,  a = (y > 0) ? x - y : y - x
 *
 * These are exactly the four rules whose inverse is model_inverse_pair;
 * the function is written from the mapping0 forward definition, not
 * copied from the production inverse loop.
 */
static void model_forward_pair(float x,float y,float *m,float *a){
  float ax=x<0.f?-x:x;
  float ay=y<0.f?-y:y;
  if(ax>ay){
    *m=x;
    *a=(x>0.f)?(x-y):(y-x);
  }else{
    *m=y;
    *a=(y>0.f)?(x-y):(y-x);
  }
}

/*
 * Multi-step inverse for a coupling chain.  Each step is an affine
 * map from two current channel vectors to the pair that existed before
 * that coupling step.  The decoder must undo the steps in the reverse
 * of their declared order; applying them in declaration order is a
 * different map whenever a channel is shared between steps.
 *
 * Channels are passed as an array of independent vectors so the model
 * makes no assumption about the stride between channel storage.
 */
static TC_UNUSED void model_inverse_chain(int ch,int n,int steps,
                                const int *magch,const int *angch,
                                float *const *in,
                                float *const *out){
  float *work[8];
  int c,i,s;
  for(c=0;c<ch;c++){
    work[c]=xmalloc(sizeof(float)*n);
    memcpy(work[c],in[c],sizeof(float)*n);
  }
  for(s=steps-1;s>=0;s--){
    int cm=magch[s],ca=angch[s];
    for(i=0;i<n;i++){
      float x,y;
      model_inverse_pair(work[cm][i],work[ca][i],&x,&y);
      work[cm][i]=x;
      work[ca][i]=y;
    }
  }
  for(c=0;c<ch;c++){
    memcpy(out[c],work[c],sizeof(float)*n);
    free(work[c]);
  }
}

/* Forward multi-step coupling, in declared order. */
static void model_forward_chain(int ch,int n,int steps,
                                const int *magch,const int *angch,
                                float *const *in,
                                float *const *out){
  float *work[8];
  int c,i,s;
  for(c=0;c<ch;c++){
    work[c]=xmalloc(sizeof(float)*n);
    memcpy(work[c],in[c],sizeof(float)*n);
  }
  for(s=0;s<steps;s++){
    int cm=magch[s],ca=angch[s];
    for(i=0;i<n;i++){
      float m,a;
      model_forward_pair(work[cm][i],work[ca][i],&m,&a);
      work[cm][i]=m;
      work[ca][i]=a;
    }
  }
  for(c=0;c<ch;c++){
    memcpy(out[c],work[c],sizeof(float)*n);
    free(work[c]);
  }
}

/* ------------------------------------------------------------------ */
/* closed-form floor, window and IMDCT used for expected PCM           */
/* ------------------------------------------------------------------ */

/*
 * floor1 quantizes a 0..-140 dB envelope to 256 steps.  The production
 * table is exp((ln10/20) * (y-255) * 140/256); derive the gain
 * directly instead of including a production table.
 */
static double model_floor_gain(int y){
  return exp(0.11512925464970229*(double)(y-255)*140.0/256.0);
}

/* Vorbis power-of-two window for a block of N samples, half length n. */
static double model_window(int N,int i){
  double t=M_PI*(i+0.5)/(double)N;
  double s=sin(t);
  return sin(0.5*M_PI*s*s);
}

/*
 * Direct inverse MDCT.  Production mdct_backward implements, per bin k,
 * the basis cos((pi/(2N)) (2k+1) (2n + N/2 + 1)); the closed form is
 * evaluated here explicitly with no production transform call.
 */
static void model_imdct(int N,const float *spectrum,double *time){
  int n=N/2;
  int i,k;
  for(i=0;i<N;i++){
    double acc=0.0;
    for(k=0;k<n;k++){
      double arg=(M_PI/(2.0*(double)N))*
                 (double)(2*k+1)*(double)(2*i+N/2+1);
      acc+=(double)spectrum[k]*cos(arg);
    }
    time[i]=acc;
  }
}

/* ================================================================== */
/* C bitstream builder                                                 */
/* ================================================================== */
/*
 * Every packet below is assembled with raw oggpack_write calls following
 * the Vorbis I header/back-end wire formats.  Library usage is limited to
 * generic bit-format helpers (ov_ilog, the 32 bit float packer); no
 * production synthesis result feeds the expected values.
 */

struct cb_spec {
  int dim;
  int entries;
  signed char lengths[64];   /* unordered codeword lengths, 0 = unused */
  int maptype;               /* 0 or 1 */
  float q_min;
  float q_delta;
  int q_quant;
  int sequencep;
};

#define TC_MAX_CH    6
#define TC_MAX_STEP  4
#define TC_MAX_SUB   2
#define TC_MAX_N     128   /* longest spectral half (N1=256) */
#define TC_MAX_PK    32    /* audio packets per synthetic stream */

struct stream_cfg {
  int channels;
  long rate;
  int blockshort;          /* N0 */
  int blocklong;           /* N1 */
  int submaps;
  int mux[TC_MAX_CH];
  int floor_submap[TC_MAX_SUB];
  int res_submap[TC_MAX_SUB];
  int steps;
  int mag[TC_MAX_STEP];
  int ang[TC_MAX_STEP];
  int floor_y[TC_MAX_CH];  /* constant floor1 dB index per channel */
  int value_entries;       /* entries of the integer residue book */
  int value_min;           /* integer book minimum */
  int frac_entries;        /* entries of fractional book (submap 1) */
  float frac_min;
  float frac_delta;
};

/* All test books are complete, equal-length binary trees, so the LSb
 * first codeword of an entry is the entry index written with its bits
 * reversed.  Restricting ourselves to full trees keeps the builder
 * small while still exercising arbitrary integer and fractional
 * quantized values. */
static int book_bits(int entries){
  int bits=0,x=entries;
  while(x>1){bits++;x=(x+1)>>1;}
  return bits;
}
static unsigned long book_word(int entries,int entry){
  int bits=book_bits(entries),b;
  unsigned long word=0;
  for(b=0;b<bits;b++)
    word|=((unsigned long)entry>>b & 1UL)<<(bits-1-b);
  return word;
}

static void write_codebook(oggpack_buffer *opb,const struct cb_spec *cb){
  int i;
  oggpack_write(opb,0x564342,24);
  oggpack_write(opb,cb->dim,16);
  oggpack_write(opb,cb->entries,24);
  oggpack_write(opb,0,1);            /* unordered */
  oggpack_write(opb,0,1);            /* every entry used */
  for(i=0;i<cb->entries;i++)
    oggpack_write(opb,cb->lengths[i]-1,5);
  oggpack_write(opb,cb->maptype,4);
  if(cb->maptype==1){
    oggpack_write(opb,_float32_pack(cb->q_min),32);
    oggpack_write(opb,_float32_pack(cb->q_delta),32);
    oggpack_write(opb,cb->q_quant-1,4);
    oggpack_write(opb,cb->sequencep,1);
    /* dim 1 lattice: quantlist entries are 0..entries-1 */
    for(i=0;i<cb->entries;i++)
      oggpack_write(opb,i,cb->q_quant);
  }
}

/* floor type 1, zero partitions, two implicit posts at the same dB
 * value so the rendered curve is a constant gain. */
static void write_floor1_cfg(oggpack_buffer *opb,int rangebits){
  oggpack_write(opb,0,5);            /* partitions = 1 */
  oggpack_write(opb,0,2);            /* mult = 1 */
  oggpack_write(opb,rangebits,4);
}

/* residue type 1: one partition, one stage, grouping 1, dim-1 books.
 * The cascade wire format is a 3 bit low field followed by a 1 bit
 * extension flag (and, when set, a 5 bit high field); stage 0 only
 * needs the low field value 1 and a zero extension flag. */
static void write_res1_cfg(oggpack_buffer *opb,int end,
                           int phrasebook,int valuebook){
  oggpack_write(opb,0,24);           /* begin */
  oggpack_write(opb,end,24);         /* end */
  oggpack_write(opb,0,24);           /* grouping = 1 */
  oggpack_write(opb,0,6);            /* partitions = 1 */
  oggpack_write(opb,phrasebook,8);
  oggpack_write(opb,1,3);            /* cascade low: stage 0 present */
  oggpack_write(opb,0,1);            /* no high cascade extension */
  oggpack_write(opb,valuebook,8);
}

static void write_map0(oggpack_buffer *opb,const struct stream_cfg *cfg){
  int i,chbits=ov_ilog(cfg->channels-1);
  if(cfg->submaps>1){
    oggpack_write(opb,1,1);
    oggpack_write(opb,cfg->submaps-1,4);
  }else{
    oggpack_write(opb,0,1);
  }
  if(cfg->steps>0){
    oggpack_write(opb,1,1);
    oggpack_write(opb,cfg->steps-1,8);
    for(i=0;i<cfg->steps;i++){
      oggpack_write(opb,cfg->mag[i],chbits);
      oggpack_write(opb,cfg->ang[i],chbits);
    }
  }else{
    oggpack_write(opb,0,1);
  }
  oggpack_write(opb,0,2);            /* reserved */
  if(cfg->submaps>1)
    for(i=0;i<cfg->channels;i++)
      oggpack_write(opb,cfg->mux[i],4);
  for(i=0;i<cfg->submaps;i++){
    oggpack_write(opb,0,8);          /* time backend unused */
    oggpack_write(opb,cfg->floor_submap[i],8);
    oggpack_write(opb,cfg->res_submap[i],8);
  }
}

/* ================================================================== */
/* stream configuration and full header / audio packet construction    */
/* ================================================================== */

struct packet_spec {
  int W;                   /* 0 short, 1 long */
  int lW,nW;
  const int *floor_on;     /* 1 per channel: floor presence flag */
  const float *spectrum[TC_MAX_CH];
  int n;
  int frac_submap;         /* submap index whose values are fractional,
                              or -1 when all integer */
};

static void write_id_header(oggpack_buffer *opb,const struct stream_cfg *c){
  oggpack_write(opb,1,8);
  oggpack_write(opb,'v',8);oggpack_write(opb,'o',8);
  oggpack_write(opb,'r',8);oggpack_write(opb,'b',8);
  oggpack_write(opb,'i',8);oggpack_write(opb,'s',8);
  oggpack_write(opb,0,32);
  oggpack_write(opb,c->channels,8);
  oggpack_write(opb,c->rate,32);
  oggpack_write(opb,0,32);             /* upper bitrate */
  oggpack_write(opb,0,32);             /* nominal */
  oggpack_write(opb,0,32);             /* lower */
  oggpack_write(opb,ov_ilog(c->blockshort-1),4);
  oggpack_write(opb,ov_ilog(c->blocklong-1),4);
  oggpack_write(opb,1,1);
}

static void write_comment_header(oggpack_buffer *opb){
  const char vendor[]="coupling-regression";
  oggpack_write(opb,3,8);
  oggpack_write(opb,'v',8);oggpack_write(opb,'o',8);
  oggpack_write(opb,'r',8);oggpack_write(opb,'b',8);
  oggpack_write(opb,'i',8);oggpack_write(opb,'s',8);
  oggpack_write(opb,(long)sizeof(vendor)-1,32);
  for(unsigned i=0;i<sizeof(vendor)-1;i++)
    oggpack_write(opb,vendor[i],8);
  oggpack_write(opb,0,32);             /* no user comments */
  oggpack_write(opb,1,1);
}

static void write_setup_header(oggpack_buffer *opb,
                               const struct stream_cfg *c,
                               const struct cb_spec *phrase,
                               const struct cb_spec *intbook,
                               const struct cb_spec *fracbook){
  int books = fracbook ? 3 : 2;
  oggpack_write(opb,5,8);
  oggpack_write(opb,'v',8);oggpack_write(opb,'o',8);
  oggpack_write(opb,'r',8);oggpack_write(opb,'b',8);
  oggpack_write(opb,'i',8);oggpack_write(opb,'s',8);

  oggpack_write(opb,books-1,8);
  write_codebook(opb,phrase);          /* book 0 */
  write_codebook(opb,intbook);         /* book 1 */
  if(fracbook)write_codebook(opb,fracbook); /* book 2 */

  oggpack_write(opb,0,6);              /* one time backend */
  oggpack_write(opb,0,16);            /* unused */

  oggpack_write(opb,0,6);              /* one floor backend (type 1) */
  oggpack_write(opb,1,16);
  write_floor1_cfg(opb,4);            /* rangebits = 4 */

  oggpack_write(opb,c->submaps-1,6);  /* residue backends */
  for(int s=0;s<c->submaps;s++){
    int vb = (fracbook && s==c->submaps-1) ? 2 : 1;
    oggpack_write(opb,1,16);          /* residue type 1 */
    /* long-block half length; short blocks are clamped by the decoder */
    write_res1_cfg(opb,c->blocklong/2,0,vb);
  }

  oggpack_write(opb,0,6);             /* one mapping (type 0) */
  oggpack_write(opb,0,16);
  write_map0(opb,c);

  oggpack_write(opb,1,6);             /* two modes */
  oggpack_write(opb,0,1);             /* mode 0: short block */
  oggpack_write(opb,0,16);
  oggpack_write(opb,0,16);
  oggpack_write(opb,0,8);
  oggpack_write(opb,1,1);             /* mode 1: long block */
  oggpack_write(opb,0,16);
  oggpack_write(opb,0,16);
  oggpack_write(opb,0,8);

  oggpack_write(opb,1,1);            /* framing flag */
}

static void finish_packet(oggpack_buffer *opb,ogg_packet *p,
                          long packetno,int eos){
  p->packet=xmalloc(oggpack_bytes(opb));
  memcpy(p->packet,opb->buffer,oggpack_bytes(opb));
  p->bytes=oggpack_bytes(opb);
  p->b_o_s=0;
  p->e_o_s=eos;
  p->granulepos=-1;
  p->packetno=packetno;
}

/* write one residue type 1 submap: partition words then values */
static void write_res1_payload(oggpack_buffer *opb,
                               const struct stream_cfg *c,
                               int submap,int n,
                               const int *participate,
                               const float *const spectrum[TC_MAX_CH],
                               int frac){
  int used[TC_MAX_CH],nu=0,i,k,ch;
  for(ch=0;ch<c->channels;ch++)
    if(c->mux[ch]==submap && participate[ch])
      used[nu++]=ch;
  if(!nu)return;
  for(i=0;i<n;i++){
    for(k=0;k<nu;k++)oggpack_write(opb,0,1); /* partition class 0 */
    for(k=0;k<nu;k++){
      float v=spectrum[used[k]][i];
      long idx;
      if(frac){
        idx=(long)lroundf((v-c->frac_min)/c->frac_delta);
        if(idx<0)idx=0;
        if(idx>=c->frac_entries)idx=c->frac_entries-1;
      }else{
        idx=(long)v-c->value_min;
        if(idx<0)idx=0;
        if(idx>=c->value_entries)idx=c->value_entries-1;
      }
      unsigned long word;int len;
      int entries=frac?c->frac_entries:c->value_entries;
      len=book_bits(entries);
      word=book_word(entries,(int)idx);
      oggpack_write(opb,word,len);
    }
  }
}

/*
 * Residue participation flags follow mapping0: a channel takes part if
 * its own floor is present, or coupling dirties it because a coupled
 * partner has a floor.  This is distinct from the raw floor presence.
 */
static void residue_participation(const struct stream_cfg *c,
                                  const int *floor_on,int *participate){
  int ch,i;
  for(ch=0;ch<c->channels;ch++)participate[ch]=floor_on[ch];
  for(i=0;i<c->steps;i++)
    if(participate[c->mag[i]] || participate[c->ang[i]])
      participate[c->mag[i]]=participate[c->ang[i]]=1;
}

static ogg_packet *build_audio_packet(const struct stream_cfg *c,
                                      const struct packet_spec *spec,
                                      long packetno){
  oggpack_buffer opb;
  int participate[TC_MAX_CH];
  int ch,s;
  oggpack_writeinit(&opb);

  oggpack_write(&opb,0,1);           /* audio packet */
  oggpack_write(&opb,spec->W,1);     /* modebit: two modes => 1 bit */
  if(spec->W)oggpack_write(&opb,spec->lW,1),oggpack_write(&opb,spec->nW,1);

  for(ch=0;ch<c->channels;ch++){
    if(spec->floor_on[ch]){
      oggpack_write(&opb,1,1);
      oggpack_write(&opb,c->floor_y[ch],8);
      oggpack_write(&opb,c->floor_y[ch],8);
    }else{
      oggpack_write(&opb,0,1);
    }
  }

  residue_participation(c,spec->floor_on,participate);
  for(s=0;s<c->submaps;s++)
    write_res1_payload(&opb,c,s,spec->n,participate,spec->spectrum,
                       s==spec->frac_submap);

  ogg_packet *p=xmalloc(sizeof(*p));
  finish_packet(&opb,p,packetno,0);
  oggpack_writeclear(&opb);
  return p;
}

/* ================================================================== */
/* VORBIS_COUPLING_TRACE observation storage                          */
/* ================================================================== */
/*
 * mapping0.c calls vorbis_coupling_trace() at the three read-only probe
 * points when built with -DVORBIS_COUPLING_TRACE.  The hook only copies
 * values out of the block; it never writes through vb->pcm and never
 * touches the bitpacker.
 */
#ifdef VORBIS_COUPLING_TRACE
#define TC_TRACE_N 128
struct trace_slot {
  int channels;
  int n;
  float after_residue[TC_MAX_CH][TC_TRACE_N];
  float after_coupling[TC_MAX_CH][TC_TRACE_N];
  float after_floor[TC_MAX_CH][TC_TRACE_N];
  int have[4];
};
static struct trace_slot trace_slots[TC_MAX_PK];
static int trace_current;

void vorbis_coupling_trace(int phase,const vorbis_block *vb){
  struct trace_slot *s=&trace_slots[trace_current];
  int n=vb->pcmend/2,c;
  if(phase<1||phase>3)return;
  s->channels=vb->vd->vi->channels;
  s->n=n;
  for(c=0;c<s->channels;c++){
    float *dst=(phase==1)?s->after_residue[c]:
               (phase==2)?s->after_coupling[c]:s->after_floor[c];
    const float *src=vb->pcm[c];
    for(int i=0;i<n && i<TC_TRACE_N;i++)dst[i]=src[i];
  }
  s->have[phase]=1;
}
static void trace_reset(int slot){
  memset(&trace_slots[slot],0,sizeof(trace_slots[slot]));
}
static void trace_select(int slot){trace_current=slot;}
#endif

/* ================================================================== */
/* independent PCM reference: IMDCT + window + overlap-add            */
/* ================================================================== */

struct ref_block {
  int W;                 /* block flag 0 short, 1 long */
  int n;                 /* spectral half length */
  const float **spectrum;/* per channel floor-scaled spectra */
};

struct ref_engine {
  int channels;
  int n0,n1;             /* half block sizes */
  int lW;
  int centerW;           /* 0 or n1 */
  float *pcm;            /* channels * 2*n1 */
  int returned;
  int current;
};

static struct ref_engine *ref_new(int channels,int n0,int n1){
  struct ref_engine *e=xcalloc(1,sizeof(*e));
  e->channels=channels;e->n0=n0;e->n1=n1;
  e->centerW=n1;e->returned=0;e->current=0;
  e->pcm=xcalloc((size_t)channels*2*n1,sizeof(float));
  return e;
}
static void ref_free(struct ref_engine *e){free(e->pcm);free(e);}

static void ref_blockin(struct ref_engine *e,const struct ref_block *b){
  int n1=e->n1,n0=e->n0,n=b->n,W=b->W,lW=e->lW;
  int prevCenter=e->centerW?0:n1;
  int thisCenter=e->centerW?n1:0;
  int ch;
  for(ch=0;ch<e->channels;ch++){
    double t[256];
    float *pcm=e->pcm+(size_t)ch*2*n1;
    model_imdct(2*n,b->spectrum[ch],t);
    if(!lW){
      if(!W){ /* small/small */
        for(int i=0;i<n0;i++)
          pcm[prevCenter+i]=(float)(pcm[prevCenter+i]*model_window(2*n0,n0-1-i)
                                    +t[i]*model_window(2*n0,i));
      }else{  /* small/large */
        for(int i=0;i<n0;i++)
          pcm[prevCenter+i]=(float)(pcm[prevCenter+i]*model_window(2*n0,n0-1-i)
                                    +t[i+n1/2-n0/2]*model_window(2*n0,i));
        for(int i=n0;i<n1/2+n0/2;i++)
          pcm[prevCenter+i]=(float)t[i+n1/2-n0/2];
      }
    }else{
      if(W){ /* large/large */
        for(int i=0;i<n1;i++)
          pcm[prevCenter+i]=(float)(pcm[prevCenter+i]*model_window(2*n1,n1-1-i)
                                    +t[i]*model_window(2*n1,i));
      }else{ /* large/small */
        int off=n1/2-n0/2;
        for(int i=0;i<n0;i++)
          pcm[prevCenter+off+i]=(float)(pcm[prevCenter+off+i]*
                                        model_window(2*n0,n0-1-i)
                                        +t[i]*model_window(2*n0,i));
      }
    }
    for(int i=0;i<n;i++)
      pcm[thisCenter+i]=(float)t[n+i];
  }
  e->centerW=e->centerW?0:n1;
  if(e->current==0 && e->returned==0){
    e->returned=thisCenter;
    e->current=thisCenter;
  }else{
    e->returned=prevCenter;
    e->current=prevCenter+((lW?2*n1:2*n0)/4+(W?2*n1:2*n0)/4);
  }
  e->lW=W;
}
static int ref_pending(const struct ref_engine *e){
  return e->current-e->returned;
}
static float ref_sample(const struct ref_engine *e,int ch,int idx){
  return e->pcm[(size_t)ch*2*e->n1+e->returned+idx];
}
static void ref_read(struct ref_engine *e,int n){e->returned+=n;}

/* ================================================================== */
/* production decode driver and comparison helpers                     */
/* ================================================================== */

struct audio_stream {
  struct stream_cfg cfg;
  ogg_packet hdr[3];
  ogg_packet *audio[TC_MAX_PK];
  int npackets;
  int slot[TC_MAX_PK];
  int W[TC_MAX_PK];
  int floor_on[TC_MAX_PK][TC_MAX_CH];
  const float *expected_spec[TC_MAX_PK][TC_MAX_CH]; /* decoupled, pre-floor */
  int spec_n[TC_MAX_PK];
  float *owned[TC_MAX_PK];
  float *owned_trans[TC_MAX_PK];
};

static void make_headers(struct audio_stream *s,int frac){
  oggpack_buffer opb;
  struct cb_spec phrase,intbook,fracbook;
  memset(&phrase,0,sizeof(phrase));
  phrase.dim=1;phrase.entries=1;phrase.lengths[0]=1;phrase.maptype=0;
  memset(&intbook,0,sizeof(intbook));
  intbook.dim=1;intbook.entries=s->cfg.value_entries;intbook.maptype=1;
  intbook.q_min=(float)s->cfg.value_min;intbook.q_delta=1.f;
  intbook.q_quant=book_bits(s->cfg.value_entries);
  for(int e=0;e<intbook.entries;e++)intbook.lengths[e]=(signed char)intbook.q_quant;
  memset(&fracbook,0,sizeof(fracbook));
  if(frac){
    fracbook.dim=1;fracbook.entries=s->cfg.frac_entries;fracbook.maptype=1;
    fracbook.q_min=s->cfg.frac_min;fracbook.q_delta=s->cfg.frac_delta;
    fracbook.q_quant=book_bits(fracbook.entries);
    for(int e=0;e<fracbook.entries;e++)
      fracbook.lengths[e]=(signed char)fracbook.q_quant;
  }
  oggpack_writeinit(&opb);
  write_id_header(&opb,&s->cfg);
  s->hdr[0].packet=xmalloc(oggpack_bytes(&opb));
  memcpy(s->hdr[0].packet,opb.buffer,oggpack_bytes(&opb));
  s->hdr[0].bytes=oggpack_bytes(&opb);s->hdr[0].b_o_s=1;
  s->hdr[0].packetno=0;
  oggpack_reset(&opb);
  write_comment_header(&opb);
  s->hdr[1].packet=xmalloc(oggpack_bytes(&opb));
  memcpy(s->hdr[1].packet,opb.buffer,oggpack_bytes(&opb));
  s->hdr[1].bytes=oggpack_bytes(&opb);s->hdr[1].packetno=1;
  oggpack_reset(&opb);
  write_setup_header(&opb,&s->cfg,&phrase,&intbook,frac?&fracbook:NULL);
  s->hdr[2].packet=xmalloc(oggpack_bytes(&opb));
  memcpy(s->hdr[2].packet,opb.buffer,oggpack_bytes(&opb));
  s->hdr[2].bytes=oggpack_bytes(&opb);s->hdr[2].packetno=2;
  oggpack_writeclear(&opb);
}

struct decode_out {
  int channels;
  int samples;
  int cap;
  float *pcm[TC_MAX_CH];
};

static void out_push(struct decode_out *o,int ch,float v){
  if(o->samples==o->cap){
    o->cap=o->cap?o->cap*2:256;
    for(int c=0;c<o->channels;c++)
      o->pcm[c]=realloc(o->pcm[c],sizeof(float)*o->cap);
  }
  o->pcm[ch][o->samples]=v;
}

static void decode_out_free(struct decode_out *o){
  for(int c=0;c<TC_MAX_CH;c++){free(o->pcm[c]);o->pcm[c]=NULL;}
  o->samples=o->cap=o->channels=0;
}

static int run_production_decode(struct audio_stream *s,struct decode_out *out,
                                 int slot_base){
  vorbis_info vi;vorbis_comment vc;vorbis_dsp_state vd;vorbis_block vb;
  vorbis_info_init(&vi);vorbis_comment_init(&vc);
  for(int i=0;i<3;i++)
    if(vorbis_synthesis_headerin(&vi,&vc,&s->hdr[i])!=0)return -1;
  if(vorbis_synthesis_init(&vd,&vi)!=0)return -2;
  if(vorbis_block_init(&vd,&vb)!=0)return -3;
  memset(out,0,sizeof(*out));
  out->channels=s->cfg.channels;
  for(int p=0;p<s->npackets;p++){
    s->slot[p]=slot_base+p;
#ifdef VORBIS_COUPLING_TRACE
    trace_reset(s->slot[p]);
    trace_select(s->slot[p]);
#endif
    if(vorbis_synthesis(&vb,s->audio[p])!=0)return -4;
    if(vorbis_synthesis_blockin(&vd,&vb)!=0)return -5;
    float **pcm;int n;
    while((n=vorbis_synthesis_pcmout(&vd,&pcm))>0){
      for(int t=0;t<n;t++)
        for(int c=0;c<s->cfg.channels;c++)
          out_push(out,c,pcm[c][t]);
      out->samples+=n;
      vorbis_synthesis_read(&vd,n);
    }
  }
  vorbis_block_clear(&vb);vorbis_dsp_clear(&vd);
  vorbis_comment_clear(&vc);vorbis_info_clear(&vi);
  return 0;
}

/* expected PCM from the independent model engine, drained after each
 * block exactly like the production pcmout/read loop */
static int run_reference(const struct audio_stream *s,struct decode_out *out){
  int n0=s->cfg.blockshort/2,n1=s->cfg.blocklong/2;
  struct ref_engine *e=ref_new(s->cfg.channels,n0,n1);
  memset(out,0,sizeof(*out));
  out->channels=s->cfg.channels;
  for(int p=0;p<s->npackets;p++){
    int n=s->spec_n[p];
    const float *spec[TC_MAX_CH];
    float scaled[TC_MAX_CH][TC_MAX_N];
    for(int c=0;c<s->cfg.channels;c++){
      for(int i=0;i<n;i++){
        if(s->floor_on[p][c])
          scaled[c][i]=(float)((double)s->expected_spec[p][c][i]*
                               model_floor_gain(s->cfg.floor_y[c]));
        else
          scaled[c][i]=0.f;
      }
      spec[c]=scaled[c];
    }
    struct ref_block rb;
    rb.W=s->W[p];rb.n=n;rb.spectrum=spec;
    ref_blockin(e,&rb);
    int avail=ref_pending(e);
    for(int t=0;t<avail;t++)
      for(int c=0;c<s->cfg.channels;c++)
        out_push(out,c,ref_sample(e,c,t));
    out->samples+=avail;
    ref_read(e,avail);
  }
  ref_free(e);
  return out->samples;
}

/* ================================================================== */
/* group construction helpers                                          */
/* ================================================================== */

static void free_stream(struct audio_stream *s){
  for(int i=0;i<3;i++)free(s->hdr[i].packet);
  for(int i=0;i<s->npackets;i++){
    free(s->audio[i]->packet);free(s->audio[i]);
    free(s->owned[i]);
    free(s->owned_trans[i]);
  }
}

static void add_packet(struct audio_stream *s,int W,int frac_submap,
                       const float *src[TC_MAX_CH],const int *floor_on,int n){
  int p=s->npackets,ch=s->cfg.channels;
  /* per-packet, per-stream owned storage so streams never alias each
     other's declared source or transmitted spectra */
  float (*source)[TC_MAX_N]=xmalloc(sizeof(float)*TC_MAX_CH*TC_MAX_N);
  float (*transmitted)[TC_MAX_N]=xmalloc(sizeof(float)*TC_MAX_CH*TC_MAX_N);
  float *srcv[TC_MAX_CH],*transv[TC_MAX_CH];
  for(int c=0;c<ch;c++){
    memcpy(source[c],src[c],sizeof(float)*n);
    s->expected_spec[p][c]=source[c];
    srcv[c]=source[c];
    transv[c]=transmitted[c];
  }
  model_forward_chain(ch,n,s->cfg.steps,s->cfg.mag,s->cfg.ang,
                      srcv,transv);
  for(int c=0;c<ch;c++)s->floor_on[p][c]=floor_on[c];
  s->W[p]=W;s->spec_n[p]=n;
  struct packet_spec ps;
  memset(&ps,0,sizeof(ps));
  ps.W=W;ps.lW=0;ps.nW=0;ps.floor_on=s->floor_on[p];ps.n=n;
  ps.frac_submap=frac_submap;
  for(int c=0;c<ch;c++)ps.spectrum[c]=transmitted[c];
  s->audio[p]=build_audio_packet(&s->cfg,&ps,3+p);
  /* transmitted is fully encoded into the packet; keep both buffers
     alive until the stream is freed (the reference reads source) */
  s->owned[p]=(float *)source;
  s->owned_trans[p]=(float *)transmitted;
  s->npackets++;
}

static void compare_pcm(const struct decode_out *got,
                        const struct decode_out *want,
                        const int *silent_channels){
  task_check(got->samples==want->samples,"pcm sample count exact");
  task_check(got->channels==want->channels,"pcm channel count exact");
  for(int c=0;c<got->channels;c++)
    for(int t=0;t<got->samples;t++){
      double tol=2e-5*(1.0+fabs((double)want->pcm[c][t]));
      if(silent_channels && silent_channels[c]){
        /* The mapping0-level silence guarantee (the floorless channel's
         * post-floor spectrum is exactly zero) is asserted separately on
         * the TRACE snapshot.  At the PCM layer require agreement with
         * the all-zero reference within the ordinary numeric tolerance
         * rather than an exact-equality that depends on the platform
         * allocator leaving reused overlap buffers zeroed. */
        task_check(isfinite(got->pcm[c][t]),"silent sample finite");
        task_check(dsame((double)got->pcm[c][t],
                         (double)want->pcm[c][t],tol),
                   "silent channel matches zero reference");
      }else{
        task_check(isfinite(got->pcm[c][t]),"decoded sample finite");
        task_check(dsame((double)got->pcm[c][t],
                         (double)want->pcm[c][t],tol),
                   "pcm within model tolerance");
      }
    }
}

/* ================================================================== */
/* independent packet bit-length model                                 */
/* ================================================================== */

static long expected_audio_bits(const struct audio_stream *s,int p){
  const struct stream_cfg *c=&s->cfg;
  int n=s->spec_n[p],participate[TC_MAX_CH];
  long bits=1+1; /* packet type + one mode bit (two modes) */
  int sub,ch;
  residue_participation(c,s->floor_on[p],participate);
  if(s->W[p])bits+=2;
  /* floor1 with zero partitions: 1 presence bit plus two 8 bit post
   * values when present */
  for(ch=0;ch<c->channels;ch++)
    bits+=s->floor_on[p][ch]?17:1;
  for(sub=0;sub<c->submaps;sub++){
    int nu=0,ent;
    for(ch=0;ch<c->channels;ch++)
      if(c->mux[ch]==sub && participate[ch])nu++;
    ent=(sub==c->submaps-1 && c->submaps>1)?c->frac_entries:c->value_entries;
    /* one phrase bit and one value codeword per participating channel
       per spectral bin; one partition, grouping 1 */
    bits+=(long)n*nu*(1+book_bits(ent));
  }
  return bits;
}

/* independent cumulative output sample count for a packet sequence.
 * Matches the public synthesis buffering: the first block exposes no
 * samples by itself, and every later block drains
 * blocksizes[lW]/4 + blocksizes[W]/4. */
static int expected_samples(const struct audio_stream *s,int upto){
  int n0=s->cfg.blockshort/2,n1=s->cfg.blocklong/2;
  int total=0,prevW=0;
  for(int p=0;p<=upto;p++){
    int curN=s->W[p]?2*n1:2*n0;
    int prevN=prevW?2*n1:2*n0;
    if(p>0)total+=prevN/4+curN/4;
    prevW=s->W[p];
  }
  return total;
}

/* ================================================================== */
/* group 1: four sign quadrants, zeros, ties and fractional values     */
/* ================================================================== */

static void base_stereo_cfg(struct stream_cfg *c){
  memset(c,0,sizeof(*c));
  c->channels=2;c->rate=8000;
  c->blockshort=128;c->blocklong=256;
  c->submaps=1;c->mux[0]=c->mux[1]=0;
  c->floor_submap[0]=0;c->res_submap[0]=0;
  c->steps=1;c->mag[0]=0;c->ang[0]=1;
  c->floor_y[0]=255;c->floor_y[1]=255;
  c->value_entries=16;c->value_min=-8;
  c->frac_entries=16;c->frac_min=-2.f;c->frac_delta=0.25f;
}

static int quadrant_group(void){
  struct audio_stream s;memset(&s,0,sizeof(s));
  base_stereo_cfg(&s.cfg);
  make_headers(&s,0);
  int n=64,on[2]={1,1};
  /* one source pair per bin; each lands in a different inverse branch */
  static float x[64],y[64];
  static const float *src[2];
  src[0]=x;src[1]=y;
  for(int i=0;i<n;i++){x[i]=0.f;y[i]=0.f;}
  /* (m>0,a>0) */
  x[0]=3.f;y[0]=1.f;
  /* (m>0,a<=0) */
  x[1]=1.f;y[1]=3.f;
  /* (m<=0,a>0) */
  x[2]=-3.f;y[2]=1.f;
  /* (m<=0,a<=0) */
  x[3]=-1.f;y[3]=-3.f;
  /* equal magnitude ties, including a zero-valued angle */
  x[4]=2.f;y[4]=2.f;
  x[5]=-2.f;y[5]=-2.f;
  x[6]=2.f;y[6]=-2.f;
  /* signed zero handling */
  x[7]=3.f;y[7]=0.f;
  x[8]=0.f;y[8]=3.f;
  x[9]=-3.f;y[9]=-0.f;
  x[10]=0.f;y[10]=-3.f;
  x[11]=0.f;y[11]=0.f;
  /* asymmetric small integers in every branch */
  x[12]=-3.f;y[12]=2.f;
  x[13]=2.f;y[13]=-3.f;
  x[14]=1.f;y[14]=-4.f;
  add_packet(&s,0,-1,src,on,n);

  /* fractional stream: exact binary fractions (multiples of 1/4) */
  struct audio_stream sf;memset(&sf,0,sizeof(sf));
  base_stereo_cfg(&sf.cfg);
  make_headers(&sf,1);
  static float fx[64],fy[64];
  static const float *fsrc[2];
  fsrc[0]=fx;fsrc[1]=fy;
  for(int i=0;i<n;i++){fx[i]=0.f;fy[i]=0.f;}
  fx[0]=0.75f;fy[0]=0.25f;
  fx[1]=-0.5f;fy[1]=-0.75f;
  fx[2]=0.5f;fy[2]=-0.25f;
  fx[3]=-0.25f;fy[3]=0.75f;
  fx[4]=-0.25f;fy[4]=-0.25f;   /* tie, both negative */
  add_packet(&sf,0,0,fsrc,on,n);

  struct decode_out got,want;
  memset(&got,0,sizeof(got));memset(&want,0,sizeof(want));
  task_check(run_production_decode(&s,&got,0)==0,"integer stream decodes");
  task_check(run_reference(&s,&want)==got.samples,"ref integer sample count");
  decode_out_free(&got);decode_out_free(&want);
  task_check(run_production_decode(&sf,&got,2)==0,"fractional stream decodes");
  task_check(run_reference(&sf,&want)==got.samples,"ref fractional sample count");

#ifdef VORBIS_COUPLING_TRACE
  for(int q=0;q<2;q++){
    struct audio_stream *ss=q?&sf:&s;
    int slot=ss->slot[0];
    task_check(trace_slots[slot].have[1]&&trace_slots[slot].have[2]
               &&trace_slots[slot].have[3],"all three trace phases fired");
    float tx[64],ty[64];
    (void)tx;(void)ty;
    for(int i=0;i<n;i++){
      float m,a,ex,ey;
      model_forward_pair(ss->expected_spec[0][0][i],
                         ss->expected_spec[0][1][i],&m,&a);
      /* after residue: exactly the transmitted pair */
      task_check(fsame(trace_slots[slot].after_residue[0][i],m,0.0),
                 "residue magnitude exact");
      task_check(fsame(trace_slots[slot].after_residue[1][i],a,0.0),
                 "residue angle exact");
      /* after coupling: exactly the independent source pair */
      model_inverse_pair(m,a,&ex,&ey);
      task_check(fsame(trace_slots[slot].after_coupling[0][i],ex,0.0),
                 "decoupled magnitude channel exact");
      task_check(fsame(trace_slots[slot].after_coupling[1][i],ey,0.0),
                 "decoupled angle channel exact");
      task_check(ex==ss->expected_spec[0][0][i]
                 &&ey==ss->expected_spec[0][1][i],
                 "model round trip matches declared source");
      /* after floor: constant unit gain (y=255), allow one float ulp */
      task_check(fsame(trace_slots[slot].after_floor[0][i],ex,1e-6),
                 "floored spectrum magnitude channel");
      task_check(fsame(trace_slots[slot].after_floor[1][i],ey,1e-6),
                 "floored spectrum angle channel");
    }
  }
#endif
  /* exact wire length against the independent field-width count */
  for(int p=0;p<s.npackets;p++)
    task_check(s.audio[p]->bytes==(expected_audio_bits(&s,p)+7)/8,
               "integer packet length exact");
  for(int p=0;p<sf.npackets;p++)
    task_check(sf.audio[p]->bytes==(expected_audio_bits(&sf,p)+7)/8,
               "fractional packet length exact");
  task_check(got.samples==expected_samples(&sf,sf.npackets-1),
             "output sample count matches block geometry");
  decode_out_free(&got);decode_out_free(&want);
  free_stream(&s);free_stream(&sf);
  return 1;
}

/* ================================================================== */
/* group 2: multi-step coupling chains and submapping isolation        */
/* ================================================================== */

static int chain_group(void){
  struct audio_stream s;memset(&s,0,sizeof(s));
  struct stream_cfg *c=&s.cfg;
  c->channels=3;c->rate=8000;
  c->blockshort=128;c->blocklong=256;
  /* two submaps: channel 0 in submap 0, channels 1,2 in submap 1 */
  c->submaps=2;c->mux[0]=0;c->mux[1]=1;c->mux[2]=1;
  c->floor_submap[0]=0;c->floor_submap[1]=0;
  c->res_submap[0]=0;c->res_submap[1]=1;
  /* two steps sharing channel 1: step0 (0,1), step1 (1,2) */
  c->steps=2;c->mag[0]=0;c->ang[0]=1;c->mag[1]=1;c->ang[1]=2;
  c->floor_y[0]=255;c->floor_y[1]=255;c->floor_y[2]=255;
  c->value_entries=16;c->value_min=-8;
  c->frac_entries=16;c->frac_min=-2.f;c->frac_delta=0.25f;
  /* both submaps use the integer residue book so the exact chain
     coefficients round-trip; the submap boundary itself still proves
     coefficients cannot cross between channel groups */
  make_headers(&s,0);

  int n=64,on[3]={1,1,1};
  static float src0[64],src1[64],src2[64];
  static const float *src[3];
  src[0]=src0;src[1]=src1;src[2]=src2;
  for(int i=0;i<n;i++){src0[i]=0.f;src1[i]=0.f;src2[i]=0.f;}
  /* asymmetric small integers so every chain step has work to do */
  src0[0]=3;src1[0]=1;src2[0]=-2;
  src0[1]=-3;src1[1]=2;src2[1]=1;
  src0[2]=1;src1[2]=-2;src2[2]=3;
  src0[3]=-1;src1[3]=-3;src2[3]=-2;
  src0[4]=2;src1[4]=2;src2[4]=-2;   /* tie then second step */
  src0[5]=0;src1[5]=3;src2[5]=1;
  src0[6]=-2;src1[6]=0;src2[6]=-3;
  for(int i=7;i<n;i++){
    src0[i]=(i%5)-2;src1[i]=(i%3)-1;src2[i]=1-(i%4);
  }
  add_packet(&s,0,-1,src,on,n);

  struct decode_out got,want;
  memset(&got,0,sizeof(got));memset(&want,0,sizeof(want));
  task_check(run_production_decode(&s,&got,4)==0,"chain stream decodes");
  task_check(run_reference(&s,&want)==got.samples,
             "chain reference sample count matches");
  compare_pcm(&got,&want,NULL);

#ifdef VORBIS_COUPLING_TRACE
  int slot=s.slot[0];
  task_check(trace_slots[slot].have[1]&&trace_slots[slot].have[2]
             &&trace_slots[slot].have[3],"chain trace phases fired");
  /* independently re-run the inverse chain and match every bin at the
     post-coupling observation point */
  static float transmitted[3][64];
  static float decoupled[3][64];
  float *srcv[3]={src0,src1,src2};
  float *transv[3]={transmitted[0],transmitted[1],transmitted[2]};
  float *decv[3]={decoupled[0],decoupled[1],decoupled[2]};
  model_forward_chain(3,n,2,c->mag,c->ang,srcv,transv);
  model_inverse_chain(3,n,2,c->mag,c->ang,transv,decv);
  for(int ch=0;ch<3;ch++)
    for(int i=0;i<n;i++){
      task_check(fsame(trace_slots[slot].after_residue[ch][i],
                       transmitted[ch][i],0.0),
                 "chain residue vector matches forward model");
      task_check(fsame(trace_slots[slot].after_coupling[ch][i],
                       decoupled[ch][i],0.0),
                 "chain decoupled vector matches reverse-order model");
      task_check(decoupled[ch][i]==src[ch][i],
                 "chain model recovers the declared source");
    }
  /* forward order (the mutation's behavior) gives a different result */
  {
    float work[3][64];
    for(int ch=0;ch<3;ch++)memcpy(work[ch],transmitted[ch],sizeof(float)*n);
    for(int st=0;st<2;st++)
      for(int i=0;i<n;i++){
        float x,y;
        model_inverse_pair(work[c->mag[st]][i],work[c->ang[st]][i],&x,&y);
        work[c->mag[st]][i]=x;work[c->ang[st]][i]=y;
      }
    int differs=0;
    for(int ch=0;ch<3 && !differs;ch++)
      for(int i=0;i<n;i++)
        if(work[ch][i]!=decoupled[ch][i]){differs=1;break;}
    task_check(differs,"declaration-order undo is a different map");
  }
#endif
  task_check(s.audio[0]->bytes==(expected_audio_bits(&s,0)+7)/8,
             "chain packet length exact");
  decode_out_free(&got);decode_out_free(&want);
  free_stream(&s);
  return 1;
}

/* ================================================================== */
/* group 3: full public chain packets, block sizes and stream history  */
/* ================================================================== */

static void stereo_simple_cfg(struct stream_cfg *c,int submaps){
  memset(c,0,sizeof(*c));
  c->channels=2;c->rate=8000;
  c->blockshort=128;c->blocklong=256;
  c->submaps=submaps;
  for(int i=0;i<2;i++)c->mux[i]=i%submaps;
  c->floor_submap[0]=0;c->floor_submap[1]=0;
  c->res_submap[0]=0;c->res_submap[1]=1%submaps;
  c->steps=1;c->mag[0]=0;c->ang[0]=1;
  c->floor_y[0]=255;c->floor_y[1]=255;
  c->value_entries=16;c->value_min=-8;
  c->frac_entries=16;c->frac_min=-2.f;c->frac_delta=0.25f;
}

static void fill_pair(float *x,float *y,int n,int seed,int amp){
  for(int i=0;i<n;i++){
    int vx=((i*7+seed)%(2*amp+1))-amp;
    int vy=((i*5+seed*3)%(2*amp+1))-amp;
    x[i]=(float)vx;y[i]=(float)vy;
  }
}

/*
 * Compatibility control: build a real six-channel stream with the
 * production encoder and decode it through the same public
 * headerin -> synthesis -> blockin -> pcmout chain.  The production
 * encoder chooses its own coupling chain; we only assert that the
 * decoder under test accepts the stream, emits a finite, non-silent
 * multichannel signal and reports the declared channel count.  No PCM
 * value is frozen here.
 */
static void real_encoder_six_channel_compat(void){
  const int ch=6,frames=2048;
  vorbis_info enc,dec;vorbis_comment ec,dc;
  vorbis_dsp_state venc,vdec;vorbis_block bec,bdec;
  vorbis_info_init(&enc);vorbis_info_init(&dec);
  vorbis_comment_init(&ec);vorbis_comment_init(&dc);
  task_check(vorbis_encode_init_vbr(&enc,ch,48000,0.4f)==0,
             "real six-channel encoder initializes");
  task_check(vorbis_analysis_init(&venc,&enc)==0,"analysis init");
  task_check(vorbis_block_init(&venc,&bec)==0,"analysis block init");
  ogg_packet h0,h1,h2;
  task_check(vorbis_analysis_headerout(&venc,&ec,&h0,&h1,&h2)==0,
             "real encoder emits headers");
  task_check(vorbis_synthesis_headerin(&dec,&dc,&h0)==0
             &&vorbis_synthesis_headerin(&dec,&dc,&h1)==0
             &&vorbis_synthesis_headerin(&dec,&dc,&h2)==0,
             "public headerin accepts real six-channel headers");
  /* confirm the real stream actually declares a coupling chain */
  codec_setup_info *ci=enc.codec_setup;
  int total_steps=0;
  for(int m=0;m<ci->maps;m++){
    struct vorbis_info_mapping0 *mi=
      (struct vorbis_info_mapping0 *)ci->map_param[m];
    total_steps+=mi->coupling_steps;
  }
  task_check(total_steps>0,"real six-channel stream uses coupling");
  task_check(vorbis_synthesis_init(&vdec,&dec)==0,"synthesis init");
  task_check(vorbis_block_init(&vdec,&bdec)==0,"synthesis block init");

  float **ab=vorbis_analysis_buffer(&venc,frames);
  for(int t=0;t<frames;t++)
    for(int c=0;c<ch;c++)
      ab[c][t]=0.2f*sinf(0.05f*t+0.7f*c)*(c+1)/6.0f;
  task_check(vorbis_analysis_wrote(&venc,frames)==0,"analysis wrote");
  task_check(vorbis_analysis_wrote(&venc,0)==0,"analysis closed");

  int decoded_samples=0,finite_all=1,non_silent=0,eos=0;
  while(vorbis_analysis_blockout(&venc,&bec)==1){
    task_check(vorbis_analysis(&bec,NULL)==0,"real block analysis");
    task_check(vorbis_bitrate_addblock(&bec)==0,"bitrate addblock");
    ogg_packet op;
    while(vorbis_bitrate_flushpacket(&venc,&op)){
      if(op.e_o_s)eos=1;
      task_check(vorbis_synthesis(&bdec,&op)==0,"real packet synthesis");
      task_check(vorbis_synthesis_blockin(&vdec,&bdec)==0,
                 "real packet blockin");
      float **pcm;int n;
      while((n=vorbis_synthesis_pcmout(&vdec,&pcm))>0){
        for(int t=0;t<n;t++)
          for(int c=0;c<ch;c++){
            if(!isfinite(pcm[c][t]))finite_all=0;
            if(pcm[c][t]!=0.f)non_silent=1;
          }
        decoded_samples+=n;
        vorbis_synthesis_read(&vdec,n);
      }
    }
  }
  task_check(eos,"real encoder stream terminated");
  task_check(finite_all,"real stream decodes to finite samples");
  task_check(non_silent,"real stream decodes to non-silent audio");
  task_check(decoded_samples>0,"real stream produced output samples");
  task_check(vdec.vi->channels==ch,"real stream keeps six channels");

  vorbis_block_clear(&bec);vorbis_dsp_clear(&venc);
  vorbis_block_clear(&bdec);vorbis_dsp_clear(&vdec);
  vorbis_comment_clear(&ec);vorbis_info_clear(&enc);
  vorbis_comment_clear(&dc);vorbis_info_clear(&dec);
}

static int packet_group(void){
  /* stream A: all-zero packet first, then short/long alternation.
   * Channel 1 has no floor in every packet: coupling still pulls it
   * into residue participation, but the floor stage must silence it,
   * so its PCM has to be exactly zero for the whole stream. */
  struct audio_stream a;memset(&a,0,sizeof(a));
  stereo_simple_cfg(&a.cfg,1);
  make_headers(&a,0);
  int n0=a.cfg.blockshort/2,n1=a.cfg.blocklong/2;
  static float ax[128],ay[128];
  static const float *asrc[2];asrc[0]=ax;asrc[1]=ay;
  int allon[2]={1,1};
  int silentA[2]={1,0}; /* channel 1 floor absent in every packet */

  /* packet 0: all zeros; floors present on ch0 only */
  for(int i=0;i<n0;i++){ax[i]=0.f;ay[i]=0.f;}
  add_packet(&a,0,-1,asrc,silentA,n0);

  /* packet 1: short block with content */
  fill_pair(ax,ay,n0,1,3);
  add_packet(&a,0,-1,asrc,silentA,n0);

  /* packet 2: long block; ch1 still floorless but coupling pulls it
     into residue participation */
  fill_pair(ax,ay,n1,2,3);
  add_packet(&a,1,-1,asrc,silentA,n1);

  /* packet 3: short block again, ch1 remains floorless */
  fill_pair(ax,ay,n0,3,2);
  add_packet(&a,0,-1,asrc,silentA,n0);

  struct decode_out got,want;
  memset(&got,0,sizeof(got));memset(&want,0,sizeof(want));
  task_check(run_production_decode(&a,&got,8)==0,
             "packet stream A decodes on public chain");
  task_check(run_reference(&a,&want)==got.samples,
             "packet stream A reference count");
  int silent_channels[2]={0,1};
  compare_pcm(&got,&want,silent_channels);
#ifdef VORBIS_COUPLING_TRACE
  /* Deterministic mapping0 silence check: even though coupling pulls the
   * floorless channel 1 into residue participation, its post-floor
   * spectrum must be exactly zero in every packet. */
  for(int p=0;p<a.npackets;p++){
    int slot=a.slot[p],nn=a.spec_n[p];
    task_check(trace_slots[slot].have[1]&&trace_slots[slot].have[2]
               &&trace_slots[slot].have[3],"packet trace phases fired");
    int residue_participates=0,floor_exact_zero=1;
    for(int i=0;i<nn;i++){
      if(trace_slots[slot].after_residue[1][i]!=0.f)
        residue_participates=1;
      if(trace_slots[slot].after_floor[1][i]!=0.f)
        floor_exact_zero=0;
    }
    /* the content-bearing packets prove residue participation despite
       the absent floor; the all-zero packet naturally has no energy */
    if(p>0)task_check(residue_participates,
               "floorless channel still participates in residue");
    task_check(floor_exact_zero,
               "floorless channel post-floor spectrum exactly zero");
  }
#endif
  /* packet 0 and packet 2 exact length checks against field model */
  task_check(a.audio[0]->bytes==(expected_audio_bits(&a,0)+7)/8,
             "all-zero packet length exact");
  task_check(a.audio[2]->bytes==(expected_audio_bits(&a,2)+7)/8,
             "floorless-channel long packet length exact");
  /* cumulative sample counts for the alternating sizes */
  for(int p=0;p<a.npackets;p++)
    task_check(expected_samples(&a,p)>=0,"block geometry computable");

  /* independent second stream interleaved between A's decodes, using a
     different slot range so residual traces cannot bleed across */
  struct audio_stream b;memset(&b,0,sizeof(b));
  stereo_simple_cfg(&b.cfg,1);
  make_headers(&b,0);
  static float bx[128],by[128];
  static const float *bsrc[2];bsrc[0]=bx;bsrc[1]=by;
  fill_pair(bx,by,n0,9,3);
  add_packet(&b,0,-1,bsrc,allon,n0);
  fill_pair(bx,by,n0,4,2);
  add_packet(&b,0,-1,bsrc,allon,n0);
  struct decode_out gotb,wantb;
  memset(&gotb,0,sizeof(gotb));memset(&wantb,0,sizeof(wantb));
  task_check(run_production_decode(&b,&gotb,16)==0,
             "interleaved stream B decodes");
  task_check(run_reference(&b,&wantb)==gotb.samples,
             "interleaved reference count");
  compare_pcm(&gotb,&wantb,NULL);

  /* distinct content proves no cross-instance spectral carry-over */
  int different=0;
  for(int t=0;t<got.samples && t<gotb.samples;t++)
    if(got.pcm[0][t]!=gotb.pcm[0][t]){different=1;break;}
  task_check(different,"interleaved instances stay independent");

  real_encoder_six_channel_compat();

  decode_out_free(&got);decode_out_free(&want);
  decode_out_free(&gotb);decode_out_free(&wantb);
  free_stream(&a);free_stream(&b);
  return 1;
}

int task_cases(const char *group){
  if(!strcmp(group,"quadrants"))return quadrant_group();
  if(!strcmp(group,"chain"))return chain_group();
  if(!strcmp(group,"packets"))return packet_group();
  return 0;
}
