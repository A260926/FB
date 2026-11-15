/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggTheora SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE Theora SOURCE CODE IS COPYRIGHT (C) 2002-2009,2025           *
 * by the Xiph.Org Foundation and contributors                      *
 * https://www.xiph.org/                                            *
 *                                                                  *
 ********************************************************************

  function: regression tests for the Theora loop filter boundary order.

  This driver compares the production loop filter (both the plain C
  implementation oc_state_loop_filter_frag_rows_c and whatever
  accelerated implementation this host selects) against the independent
  reference model in loopfilter_ref.h, on synthetic fragment planes.
  It never compares two decoder instances running the same production
  filter against each other: every check is anchored to the independent
  reference, so a systematic error in the production filter itself is
  still caught.

  Named cases printed on stdout (one line each):
    ref_vs_c_impl           independent reference vs C implementation
    ref_vs_native_impl      independent reference vs host-selected impl
    targeted_response_edges crafted 4-pixel windows: signed rounding,
                            limit folding, 0/255 saturation, zero limit
    batched_vs_whole_plane  row-batched filtering (the decode.c stripe
                            pattern) must equal whole-plane filtering,
                            including untouched padding

 ********************************************************************/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "state.h"
#include "loopfilter_ref.h"

#define SENTINEL 0xA5
#define MPAD 16

/* ------------------------------------------------------------------ */
/* Mock decoder state                                                  */

typedef struct{
  int nh,nv;             /*fragment grid dimensions*/
  int W,H;               /*storage dimensions including padding*/
  int stride;            /*signed row stride*/
  ptrdiff_t pbase;       /*offset of this plane's storage in the buffer*/
  ptrdiff_t porig;       /*offset of grid pixel (0,0) in the buffer*/
}mplane;

typedef struct{
  oc_theora_state st;
  oc_fragment *frags;
  ptrdiff_t *offs;
  unsigned char *pristine;  /*buffer as constructed (sentinel+pixels)*/
  unsigned char *run;       /*scratch buffer the filters mutate*/
  size_t bufsz;
  mplane pl[3];
  int nplanes;
  unsigned char *coded[3];  /*per-plane coded maps (raster order)*/
}mock;

static unsigned lcg_next(unsigned *_s){
  *_s=*_s*1664525u+1013904223u;
  return *_s>>8;
}

static int align16(int _v){
  return _v+15&~15;
}

/*Pixel generators: gen_id selects the pattern for grid pixel (x,r).*/
static unsigned char gen_pixel(int _gen,int _pli,int _x,int _r,unsigned *_rng){
  static const unsigned char extremes[8]={0,1,2,3,252,253,254,255};
  switch(_gen){
    case 0:return (unsigned char)(lcg_next(_rng)&0xFF);
    case 1:return extremes[lcg_next(_rng)%8];
    case 2:return (unsigned char)(40+36*(((_x>>3)+(_r>>3))%5));
    case 3:return (unsigned char)(_x*3+_r*5+_pli*40);
    default:return (unsigned char)((((_x>>3)+(_r>>3))&1?250:5)
     +(int)(lcg_next(_rng)%3));
  }
}

/*Coded-map generators: map_id selects the pattern for fragment (fx,fy).*/
static int gen_coded(int _map,int _fx,int _fy,int _nh,int _nv,unsigned *_rng){
  switch(_map){
    case 0:return 1;
    case 1:return 0;
    case 2:return (_fx+_fy)&1;
    case 3:return _fx==_nh/2&&_fy==_nv/2;
    case 4:return _fx<_nh/2;
    case 5:return (_fy&1)==0;
    case 6:return (_fx&1)==0;
    case 7:return (lcg_next(_rng)&1);
    case 8:return (lcg_next(_rng)&3)==0;
    case 9:return _fx==0||_fy==0||_fx==_nh-1||_fy==_nv-1;
    default:return _fx==_fy;
  }
}

static void mock_free(mock *_m){
  int pli;
  if(_m==NULL)return;
  free(_m->frags);
  free(_m->offs);
  free(_m->pristine);
  free(_m->run);
  for(pli=0;pli<3;pli++)free(_m->coded[pli]);
  memset(_m,0,sizeof(*_m));
}

/*Build a mock decoder state with the given per-plane fragment grids.
  _neg_stride: if non-zero, use bottom-up (negative stride) planes like
   the real reference frame buffers.
  _map_id/_gen_id select the coded map and pixel content per plane;
   _seed varies the random content.
  _flimit is stored in the state for implementations that read it.*/
static int mock_build(mock *_m,const int *_nh,const int *_nv,int _nplanes,
 int _neg_stride,int _map_id,int _gen_id,unsigned _seed,int _flimit){
  ptrdiff_t total;
  ptrdiff_t base;
  unsigned rng;
  int pli;
  memset(_m,0,sizeof(*_m));
  _m->nplanes=_nplanes;
  total=0;
  base=0;
  rng=_seed;
  for(pli=0;pli<_nplanes;pli++){
    mplane *p=_m->pl+pli;
    p->nh=_nh[pli];
    p->nv=_nv[pli];
    p->W=align16(_nh[pli]*8+2*MPAD);
    p->H=_nv[pli]*8+2*MPAD;
    p->stride=_neg_stride?-p->W:p->W;
    p->pbase=base;
    base+=(ptrdiff_t)p->W*p->H;
    total+=(ptrdiff_t)_nh[pli]*_nv[pli];
  }
  _m->bufsz=(size_t)base;
  _m->pristine=(unsigned char *)malloc(_m->bufsz);
  _m->run=(unsigned char *)malloc(_m->bufsz);
  _m->frags=(oc_fragment *)calloc((size_t)total,sizeof(*_m->frags));
  _m->offs=(ptrdiff_t *)malloc((size_t)total*sizeof(*_m->offs));
  if(_m->pristine==NULL||_m->run==NULL||_m->frags==NULL||_m->offs==NULL){
    mock_free(_m);
    return -1;
  }
  memset(_m->pristine,SENTINEL,_m->bufsz);
  base=0;
  for(pli=0;pli<_nplanes;pli++){
    mplane *p=_m->pl+pli;
    ptrdiff_t froff=base;
    int fx,fy;
    if(_neg_stride)p->porig=p->pbase+(ptrdiff_t)(p->H-1-MPAD)*p->W+MPAD;
    else p->porig=p->pbase+(ptrdiff_t)MPAD*p->W+MPAD;
    _m->coded[pli]=(unsigned char *)malloc((size_t)(p->nh*p->nv));
    if(_m->coded[pli]==NULL){
      mock_free(_m);
      return -1;
    }
    for(fy=0;fy<p->nv;fy++){
      int r;
      for(fx=0;fx<p->nh;fx++){
        int c=gen_coded(_map_id,fx,fy,p->nh,p->nv,&rng);
        _m->coded[pli][fy*p->nh+fx]=(unsigned char)c;
        _m->frags[froff+(ptrdiff_t)fy*p->nh+fx].coded=(unsigned)c;
        _m->offs[froff+(ptrdiff_t)fy*p->nh+fx]=
         p->porig+(ptrdiff_t)fy*8*p->stride+fx*8;
      }
      for(r=0;r<8;r++){
        int y=fy*8+r;
        int x;
        for(x=0;x<p->nh*8;x++){
          _m->pristine[p->porig+(ptrdiff_t)y*p->stride+x]=
           gen_pixel(_gen_id,pli,x,y,&rng);
        }
      }
    }
    _m->st.fplanes[pli].nhfrags=p->nh;
    _m->st.fplanes[pli].nvfrags=p->nv;
    _m->st.fplanes[pli].froffset=froff;
    _m->st.fplanes[pli].nfrags=(ptrdiff_t)p->nh*p->nv;
    _m->st.ref_ystride[pli]=p->stride;
    base=froff+(ptrdiff_t)p->nh*p->nv;
  }
  _m->st.frags=_m->frags;
  _m->st.frag_buf_offs=_m->offs;
  _m->st.ref_frame_data[OC_FRAME_SELF]=_m->run;
  _m->st.qis[0]=0;
  _m->st.loop_filter_limits[0]=(unsigned char)_flimit;
  oc_state_accel_init(&_m->st);
  return 0;
}

/*Reset the scratch buffer to the pristine state.*/
static void mock_reset(mock *_m){
  memcpy(_m->run,_m->pristine,_m->bufsz);
  _m->st.ref_frame_data[OC_FRAME_SELF]=_m->run;
}

/* ------------------------------------------------------------------ */
/* Deviation reporting                                                 */

static int ceil_div_p(ptrdiff_t _a,ptrdiff_t _b){
  return _a>0?(int)((_a+_b-1)/_b):(int)(-((-_a)/_b));
}

/*Describe a buffer offset as plane/grid pixel/padding for reports.*/
static void localize(const mock *_m,ptrdiff_t _off,char *_buf,size_t _sz){
  int pli;
  for(pli=0;pli<_m->nplanes;pli++){
    const mplane *p=_m->pl+pli;
    ptrdiff_t lo=p->pbase;
    ptrdiff_t hi=p->pbase+(ptrdiff_t)p->W*p->H;
    if(_off>=lo&&_off<hi){
      ptrdiff_t d=_off-p->porig;
      int r,x;
      if(p->stride>0){
        r=(int)(d/p->stride);
        x=(int)(d%p->stride);
      }
      else{
        r=ceil_div_p(-d,-p->stride);
        x=(int)(d+(ptrdiff_t)r*(-p->stride));
      }
      if(r>=0&&r<p->nv*8&&x>=0&&x<p->nh*8){
        snprintf(_buf,_sz,"plane=%d pixel=(%d,%d) frag=(%d,%d)",pli,x,r,
         x>>3,r>>3);
      }
      else snprintf(_buf,_sz,"plane=%d PADDING(off=%td)",pli,_off-p->pbase);
      return;
    }
  }
  snprintf(_buf,_sz,"offset=%td(outside planes)",_off);
}

static int g_case_failed;

static void case_fail(const char *_case,const char *_fmt,...){
  va_list ap;
  printf("THEORA_LOOPFILTER_CASE %s FAIL ",_case);
  va_start(ap,_fmt);
  vprintf(_fmt,ap);
  va_end(ap);
  printf("\n");
  g_case_failed=1;
}

/*Compare two full buffers; on the first mismatch describe it.
  Returns 0 when equal, -1 otherwise.*/
static int cmp_buffers(const mock *_m,const unsigned char *_got,
 const unsigned char *_want,char *_rep,size_t _repsz){
  size_t i;
  for(i=0;i<_m->bufsz;i++){
    if(_got[i]!=_want[i]){
      char where[128];
      localize(_m,(ptrdiff_t)i,where,sizeof(where));
      snprintf(_rep,_repsz,"%s got=%u want=%u",where,_got[i],_want[i]);
      return -1;
    }
  }
  return 0;
}

/*Verify every padding byte of a buffer still holds the sentinel.*/
static int check_padding(const mock *_m,const unsigned char *_bufp,
 char *_rep,size_t _repsz){
  size_t i;
  for(i=0;i<_m->bufsz;i++){
    int pli;
    int isgrid=0;
    for(pli=0;pli<_m->nplanes&&!isgrid;pli++){
      const mplane *p=_m->pl+pli;
      ptrdiff_t d=(ptrdiff_t)i-p->porig;
      int r,x;
      if((ptrdiff_t)i<p->pbase||(ptrdiff_t)i>=p->pbase+(ptrdiff_t)p->W*p->H)
        continue;
      if(p->stride>0){
        if(d<0)continue;
        r=(int)(d/p->stride);
        x=(int)(d%p->stride);
      }
      else{
        r=ceil_div_p(-d,-p->stride);
        x=(int)(d+(ptrdiff_t)r*(-p->stride));
      }
      isgrid=r>=0&&r<p->nv*8&&x>=0&&x<p->nh*8;
    }
    if(!isgrid&&_bufp[i]!=SENTINEL){
      char where[128];
      localize(_m,(ptrdiff_t)i,where,sizeof(where));
      snprintf(_rep,_repsz,"padding overwritten: %s got=%u want=%u",where,
       _bufp[i],SENTINEL);
      return -1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Filter runners                                                      */

typedef struct{
  OC_ALIGN16(signed char bv[256]);
}bv_block;

static void run_c(mock *_m,int _pli,int _y0,int _yend,bv_block *_bvb){
  oc_state_loop_filter_frag_rows_c(&_m->st,_bvb->bv,OC_FRAME_SELF,_pli,
   _y0,_yend);
}

static void run_native(mock *_m,int _pli,int _y0,int _yend,bv_block *_bvb){
  oc_state_loop_filter_frag_rows(&_m->st,_bvb->bv,OC_FRAME_SELF,_pli,
   _y0,_yend);
}

/*Replicates the stripe/batch call pattern of oc_dec_pipeline_...() in
  lib/decode.c: each batch filters one fragment-row group delayed by one
  group, so the row ranges partition [0,nv) exactly.*/
static void run_batched(mock *_m,int _pli,int _mcu,int _use_native,
 bv_block *_bvb){
  int nv=_m->pl[_pli].nv;
  int notstart=0;
  int notdone=1;
  int stripe;
  for(stripe=0;notdone;stripe+=_mcu){
    int y0=stripe;
    int yend=nv<stripe+_mcu?nv:stripe+_mcu;
    int sd,ed;
    notdone=stripe+_mcu<nv;
    sd=notstart;
    ed=notdone;
    if(_use_native)run_native(_m,_pli,y0-sd,yend-ed,_bvb);
    else run_c(_m,_pli,y0-sd,yend-ed,_bvb);
    notstart=1;
  }
}

static void run_reference(mock *_m){
  int pli;
  for(pli=0;pli<_m->nplanes;pli++){
    mplane *p=_m->pl+pli;
    lf_ref_filter_plane(_m->run+p->porig,p->stride,_m->coded[pli],p->nh,
     p->nv,_m->st.loop_filter_limits[0]);
  }
}

/* ------------------------------------------------------------------ */
/* Shared combination driver                                           */

typedef int (*combo_fn)(mock *_m,const char *_tag);

/*Iterate formats x coded maps x pixel generators x limits x stride
   signs, calling _fn for each combination; stops at the first
   failure.*/
static int for_each_combo(const int *_limits,int _nlimits,combo_fn _fn){
  static const int fmt_nh[5][3]={{6,3,3},{6,3,3},{6,6,6},{4,2,2},{10,10,10}};
  static const int fmt_nv[5][3]={{4,2,2},{4,4,4},{4,4,4},{6,3,3},{3,3,3}};
  static const int fmt_np[5]={3,3,3,3,3};
  static const char *const fmt_name[5]={"420","422","444","420t","444w"};
  char tag[128];
  int fmt,map,gen,li,neg;
  for(fmt=0;fmt<5;fmt++){
    for(map=0;map<11;map++){
      for(gen=0;gen<5;gen++){
        for(li=0;li<_nlimits;li++){
          for(neg=0;neg<2;neg++){
            mock m;
            int ret;
            if(mock_build(&m,fmt_nh[fmt],fmt_nv[fmt],fmt_np[fmt],neg,map,
             gen,0x1234u+fmt*97+map*13+gen*7,_limits[li])<0)return -1;
            snprintf(tag,sizeof(tag),"fmt=%s map=%d gen=%d L=%d stride=%s",
             fmt_name[fmt],map,gen,_limits[li],neg?"neg":"pos");
            ret=_fn(&m,tag);
            mock_free(&m);
            if(ret<0)return -1;
          }
        }
      }
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Case 1+2: reference vs production implementations on synthetic planes*/

static int combo_ref_vs_impl(mock *_m,const char *_tag,int _use_native,
 const char *_case){
  bv_block bvb;
  char rep[256];
  int pli;
  if(_use_native)oc_loop_filter_init(&_m->st,bvb.bv,_m->st.loop_filter_limits[0]);
  else oc_loop_filter_init_c(bvb.bv,_m->st.loop_filter_limits[0]);
  /*Production run.*/
  mock_reset(_m);
  for(pli=0;pli<_m->nplanes;pli++){
    if(_use_native)run_native(_m,pli,0,_m->pl[pli].nv,&bvb);
    else run_c(_m,pli,0,_m->pl[pli].nv,&bvb);
  }
  {
    /*Save production result, then compute the reference prediction.*/
    unsigned char *prod=(unsigned char *)malloc(_m->bufsz);
    if(prod==NULL)return -1;
    memcpy(prod,_m->run,_m->bufsz);
    mock_reset(_m);
    run_reference(_m);
    if(cmp_buffers(_m,prod,_m->run,rep,sizeof(rep))<0){
      case_fail(_case,"%s impl=%s %s",_tag,_use_native?"native":"c",rep);
      free(prod);
      return -1;
    }
    if(check_padding(_m,prod,rep,sizeof(rep))<0){
      case_fail(_case,"%s impl=%s %s",_tag,_use_native?"native":"c",rep);
      free(prod);
      return -1;
    }
    free(prod);
  }
  return 0;
}

static int combo_c(mock *_m,const char *_tag){
  return combo_ref_vs_impl(_m,_tag,0,"ref_vs_c_impl");
}
static int combo_native(mock *_m,const char *_tag){
  return combo_ref_vs_impl(_m,_tag,1,"ref_vs_native_impl");
}

/* ------------------------------------------------------------------ */
/* Case 3: crafted 4-pixel windows                                     */

typedef struct{
  unsigned char p[4];
}quad;

/*Run one batch of up to 8 quads through one boundary orientation.
  _orient: 0 vertical boundary (horizontal window), 1 horizontal.
  _trigger_low: when set, only the lower-indexed fragment of the pair is
   coded, so the boundary is filtered as its right/bottom edge;
   otherwise both fragments are coded and the boundary is filtered as
   the left/top edge of the higher-indexed fragment.*/
static int window_case(const quad *_q,int _nq,int _L,int _orient,
 int _trigger_low,int _use_native,const char **_err){
  static char err[256];
  int nh=_orient==0?2:1;
  int nv=_orient==0?1:2;
  mock m;
  bv_block bvb;
  unsigned char expect[8][4];
  int coded0[2]={1,1};
  int i,r,c;
  int ret=-1;
  *_err=NULL;
  if(_trigger_low)coded0[1]=0;
  if(mock_build(&m,&nh,&nv,1,0,0,0,777,_L)<0)return -1;
  /*Overwrite the plane with a constant and place the quads astride the
     single boundary.*/
  {
    mplane *p=m.pl;
    for(r=0;r<nv*8;r++){
      for(c=0;c<nh*8;c++)m.pristine[p->porig+(ptrdiff_t)r*p->stride+c]=100;
    }
    m.coded[0][0]=(unsigned char)coded0[0];
    m.coded[0][1]=(unsigned char)coded0[1];
    m.frags[0].coded=(unsigned)coded0[0];
    m.frags[1].coded=(unsigned)coded0[1];
    for(i=0;i<_nq;i++){
      int k;
      for(k=0;k<4;k++)expect[i][k]=_q[i].p[k];
      {
        int F=_q[i].p[0]-_q[i].p[3]+3*(_q[i].p[2]-_q[i].p[1]);
        int R=lf_ref_response(F,_L);
        expect[i][1]=(unsigned char)lf_ref_clamp255(_q[i].p[1]+R);
        expect[i][2]=(unsigned char)lf_ref_clamp255(_q[i].p[2]-R);
      }
      if(_orient==0){
        for(k=0;k<4;k++)
          m.pristine[p->porig+(ptrdiff_t)i*p->stride+6+k]=_q[i].p[k];
      }
      else{
        for(k=0;k<4;k++)
          m.pristine[p->porig+(ptrdiff_t)(6+k)*p->stride+i]=_q[i].p[k];
      }
    }
  }
  if(_use_native)oc_loop_filter_init(&m.st,bvb.bv,_L);
  else oc_loop_filter_init_c(bvb.bv,_L);
  mock_reset(&m);
  if(_use_native)run_native(&m,0,0,nv,&bvb);
  else run_c(&m,0,0,nv,&bvb);
  /*Check the window pixels and everything around them.*/
  {
    mplane *p=m.pl;
    for(r=0;r<nv*8&&ret!=0;r++){
      for(c=0;c<nh*8;c++){
        unsigned char got=m.run[p->porig+(ptrdiff_t)r*p->stride+c];
        unsigned char want=100;
        if(_orient==0){
          if(c>=6&&c<10&&r<_nq)want=expect[r][c-6];
        }
        else{
          if(r>=6&&r<10&&c<_nq)want=expect[c][r-6];
        }
        if(got!=want){
          snprintf(err,sizeof(err),
           "orient=%s trigger=%s impl=%s L=%d pixel=(%d,%d) got=%u want=%u",
           _orient==0?"V":"H",_trigger_low?"low-edge":"both-coded",
           _use_native?"native":"c",_L,c,r,got,want);
          *_err=err;
          ret=0;
          goto out;
        }
      }
    }
    {
      char rep[256];
      if(check_padding(&m,m.run,rep,sizeof(rep))<0){
        snprintf(err,sizeof(err),"orient=%s impl=%s L=%d %s",
         _orient==0?"V":"H",_use_native?"native":"c",_L,rep);
        *_err=err;
        ret=0;
        goto out;
      }
    }
    ret=1;
  }
out:
  mock_free(&m);
  return ret>0?0:-1;
}

static int case_targeted_response_edges(void){
  /*Quads covering: the signed rounding boundary for negative filter
     values, the limit fold points, values beyond twice the limit, and
     saturation at both ends of the byte range.*/
  static const quad special[]={
    {{100,1,0,110}},      /*F=-13: rounds to -2, saturates low*/
    {{100,254,255,90}},   /*F=+13: rounds to +2, saturates high*/
    {{0,0,0,0}},          /*flat black*/
    {{255,255,255,255}},  /*flat white*/
    {{255,0,0,255}},      /*F=-765: huge negative*/
    {{0,255,255,0}},      /*F=+765: huge positive*/
    {{128,128,128,133}},  /*F=-5: negative rounding boundary*/
    {{128,128,128,141}}   /*F=-13: negative rounding boundary*/
  };
  static const int limits[]={0,1,2,3,5,10,15,64};
  const char *err;
  int li,orient,trig,impl;
  int runs=0;
  for(li=0;li<8;li++){
    int L=limits[li];
    for(orient=0;orient<2;orient++){
      for(trig=0;trig<2;trig++){
        for(impl=0;impl<2;impl++){
          /*Sweep F from -64 to 64 in batches of 8 rows.*/
          int F;
          for(F=-64;F<=64;F+=8){
            quad q[8];
            int k;
            for(k=0;k<8;k++){
              int f=F+k;
              q[k].p[0]=128;
              q[k].p[1]=128;
              q[k].p[2]=128;
              q[k].p[3]=(unsigned char)(128-f);
            }
            if(window_case(q,8,L,orient,trig,impl,&err)<0){
              case_fail("targeted_response_edges","F0=%d %s",F,err);
              return -1;
            }
            runs++;
          }
          /*Special crafted quads.*/
          if(window_case(special,8,L,orient,trig,impl,&err)<0){
            case_fail("targeted_response_edges","special %s",err);
            return -1;
          }
          runs++;
        }
      }
    }
  }
  printf("THEORA_LOOPFILTER_CASE targeted_response_edges PASS runs=%d\n",
   runs);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Case 4: batched rows vs whole plane                                 */

static int case_batched_vs_whole(void){
  static const int lay_nh[4][3]={{6,0,0},{6,0,0},{3,0,0},{6,3,3}};
  static const int lay_nv[4][3]={{8,0,0},{5,0,0},{4,0,0},{4,2,2}};
  static const int lay_np[4]={1,1,1,3};
  static const int limits[]={0,3,10,64};
  static const int mcus[]={1,2,3,4,8};
  static const int maps[]={0,2,5,7};
  bv_block bvc,bvn;
  char rep[256];
  int lay,map,gen,li,neg,mi,pli;
  int runs=0;
  for(lay=0;lay<4;lay++){
    for(map=0;map<4;map++){
      for(gen=0;gen<3;gen++){
        for(li=0;li<4;li++){
          for(neg=0;neg<2;neg++){
            mock m;
            unsigned char *whole_c,*whole_n,*ref;
            if(mock_build(&m,lay_nh[lay],lay_nv[lay],lay_np[lay],neg,
             maps[map],gen,0xBEEFu+lay*31+map*17+gen*5,limits[li])<0)
              return -1;
            whole_c=(unsigned char *)malloc(m.bufsz);
            whole_n=(unsigned char *)malloc(m.bufsz);
            ref=(unsigned char *)malloc(m.bufsz);
            if(whole_c==NULL||whole_n==NULL||ref==NULL){
              free(whole_c);free(whole_n);free(ref);
              mock_free(&m);
              return -1;
            }
            oc_loop_filter_init_c(bvc.bv,limits[li]);
            oc_loop_filter_init(&m.st,bvn.bv,limits[li]);
            /*Whole-plane runs.*/
            mock_reset(&m);
            for(pli=0;pli<m.nplanes;pli++)
              run_c(&m,pli,0,m.pl[pli].nv,&bvc);
            memcpy(whole_c,m.run,m.bufsz);
            mock_reset(&m);
            for(pli=0;pli<m.nplanes;pli++)
              run_native(&m,pli,0,m.pl[pli].nv,&bvn);
            memcpy(whole_n,m.run,m.bufsz);
            /*Reference prediction.*/
            mock_reset(&m);
            run_reference(&m);
            memcpy(ref,m.run,m.bufsz);
            /*Batched runs for every MCU size and both impls.*/
            for(mi=0;mi<5;mi++){
              int impl;
              for(impl=0;impl<2;impl++){
                mock_reset(&m);
                for(pli=0;pli<m.nplanes;pli++){
                  int mcu=mcus[mi];
                  if(m.nplanes==3&&pli>0)mcu=mcus[mi]>1?mcus[mi]/2:1;
                  run_batched(&m,pli,mcu,impl,impl?&bvn:&bvc);
                }
                if(cmp_buffers(&m,m.run,ref,rep,sizeof(rep))<0){
                  case_fail("batched_vs_whole_plane",
                   "lay=%d map=%d gen=%d L=%d neg=%d mcu=%d impl=%s %s",
                   lay,map,gen,limits[li],neg,mcus[mi],
                   impl?"native":"c",rep);
                  free(whole_c);free(whole_n);free(ref);
                  mock_free(&m);
                  return -1;
                }
                if(check_padding(&m,m.run,rep,sizeof(rep))<0){
                  case_fail("batched_vs_whole_plane",
                   "lay=%d map=%d gen=%d L=%d neg=%d mcu=%d impl=%s %s",
                   lay,map,gen,limits[li],neg,mcus[mi],
                   impl?"native":"c",rep);
                  free(whole_c);free(whole_n);free(ref);
                  mock_free(&m);
                  return -1;
                }
                runs++;
              }
            }
            /*Whole-plane runs must match the reference, too.*/
            if(cmp_buffers(&m,whole_c,ref,rep,sizeof(rep))<0){
              case_fail("batched_vs_whole_plane",
               "whole-c lay=%d map=%d gen=%d L=%d neg=%d %s",
               lay,map,gen,limits[li],neg,rep);
              free(whole_c);free(whole_n);free(ref);
              mock_free(&m);
              return -1;
            }
            if(cmp_buffers(&m,whole_n,ref,rep,sizeof(rep))<0){
              case_fail("batched_vs_whole_plane",
               "whole-native lay=%d map=%d gen=%d L=%d neg=%d %s",
               lay,map,gen,limits[li],neg,rep);
              free(whole_c);free(whole_n);free(ref);
              mock_free(&m);
              return -1;
            }
            free(whole_c);free(whole_n);free(ref);
            mock_free(&m);
          }
        }
      }
    }
  }
  printf("THEORA_LOOPFILTER_CASE batched_vs_whole_plane PASS runs=%d\n",
   runs);
  return 0;
}

/* ------------------------------------------------------------------ */

static const char *backend_name(int *_is_c){
#if defined(OC_X86_64_ASM)
  *_is_c=0;
  return "x86-64-mmxext-static";
#elif defined(OC_X86_ASM)
  *_is_c=0;
  return "x86-vtable";
#elif defined(OC_ARM_ASM)
  *_is_c=0;
  return "arm-vtable";
#elif defined(OC_C64X_ASM)
  *_is_c=0;
  return "c64x";
#else
  *_is_c=1;
  return "generic-c";
#endif
}

int main(void){
  static const int limits[]={0,1,2,3,5,10,15,64};
  int is_c=0;
  const char *backend=backend_name(&is_c);
  int resolved_c=is_c;
#if defined(OC_STATE_USE_VTABLE)
  {
    oc_theora_state st;
    memset(&st,0,sizeof(st));
    oc_state_accel_init(&st);
    if(st.opt_vtable.state_loop_filter_frag_rows==
     oc_state_loop_filter_frag_rows_c)resolved_c=1;
  }
#endif
  printf("THEORA_LOOPFILTER_INFO backend=%s native_resolves_to=%s\n",
   backend,resolved_c?"c":"simd");
  /*Case 1: independent reference vs the C implementation.*/
  if(for_each_combo(limits,8,combo_c)<0)return 1;
  printf("THEORA_LOOPFILTER_CASE ref_vs_c_impl PASS\n");
  /*Case 2: independent reference vs the host-selected implementation.
    When no SIMD backend is compiled in for this target the host path is
    the C path; the check still runs (it is never skipped).*/
  if(for_each_combo(limits,8,combo_native)<0)return 1;
  printf("THEORA_LOOPFILTER_CASE ref_vs_native_impl PASS backend=%s%s\n",
   backend,resolved_c?"":"(simd)");
  /*Case 3: crafted windows at the rounding/limit/saturation edges.*/
  if(case_targeted_response_edges()<0)return 1;
  /*Case 4: row-batched filtering equals whole-plane filtering.*/
  if(case_batched_vs_whole()<0)return 1;
  return 0;
}
