/*Region decoder test cases.
  Registered groups: Pixels, Dependencies, Lifetime.
  All test material is generated with this library's own encoder; reference
   pixels come from the normal full-frame decoder with post-processing
   disabled.*/

#include <theora/theoraregion.h>
#include <theora/theoraenc.h>
#include <theora/theoradec.h>
#include <string.h>
#include <stdlib.h>
#include "../task_support/check.h"



/*---- Test stream generation ----*/

typedef void (*tgen)(int f,unsigned char *y,unsigned char *u,
 unsigned char *v,int w,int h);

typedef struct{
  th_info        info;   /*parsed stream information*/
  th_setup_info *setup;
  ogg_packet    *pkts;
  int            npkts;
  int            cap;
  int            w;
  int            h;
}tstream;

typedef struct{
  th_ycbcr_buffer buf;
  unsigned char  *data;
}tframe;

static unsigned char tex_at(int x,int y){
  return (unsigned char)(x*2+y*3+((x*y)&15)+((x^y)&32?40:0));
}

/*Rich motion: the whole picture shifts in varying (including negative)
   directions, plus a moving bright box and drifting chroma.*/
static void gen_motion(int f,unsigned char *y,unsigned char *u,
 unsigned char *v,int w,int h){
  static const int DX[6]={0,-3,2,-1,4,-2};
  static const int DY[6]={0,2,-1,-2,1,3};
  int i;
  int j;
  int sx;
  int sy;
  for(j=0;j<h;j++)for(i=0;i<w;i++){
    sx=i+DX[f%6];
    sx=sx<0?0:sx>=w?w-1:sx;
    sy=j+DY[f%6];
    sy=sy<0?0:sy>=h?h-1:sy;
    y[j*w+i]=tex_at(sx,sy);
  }
  for(j=8+f*3;j<26+f*3;j++)for(i=10+f*2;i<36+f*2;i++){
    if(j>=0&&j<h&&i>=0&&i<w)y[j*w+i]=(unsigned char)(200+((i+j)&31));
  }
  for(j=0;j<h/2;j++)for(i=0;i<w/2;i++){
    u[j*(w/2)+i]=(unsigned char)(100+((i*5+j*3+f*4)&31));
    v[j*(w/2)+i]=(unsigned char)(140-((i*3+j*7+f*2)&31));
  }
}

/*Low motion: static textured background, one small moving box.*/
static void gen_lowmotion(int f,unsigned char *y,unsigned char *u,
 unsigned char *v,int w,int h){
  int i;
  int j;
  for(j=0;j<h;j++)for(i=0;i<w;i++)y[j*w+i]=tex_at(i,j);
  for(j=60+f*2;j<76+f*2;j++)for(i=60+f*2;i<76+f*2;i++){
    if(j>=0&&j<h&&i>=0&&i<w)y[j*w+i]=(unsigned char)(180+((i*2+j)&31));
  }
  for(j=0;j<h/2;j++)for(i=0;i<w/2;i++){
    u[j*(w/2)+i]=(unsigned char)(110+((i+j)&15));
    v[j*(w/2)+i]=(unsigned char)(150-((i*2+j)&15));
  }
}

static int tstream_append(tstream *ts,const ogg_packet *op){
  ogg_packet *np;
  if(ts->npkts>=ts->cap){
    int ncap;
    ncap=ts->cap?ts->cap*2:16;
    np=(ogg_packet *)realloc(ts->pkts,(size_t)ncap*sizeof(*np));
    if(np==NULL)return -1;
    ts->pkts=np;
    ts->cap=ncap;
  }
  np=ts->pkts+ts->npkts;
  *np=*op;
  np->packet=(unsigned char *)malloc((size_t)(op->bytes>0?op->bytes:1));
  if(np->packet==NULL)return -1;
  if(op->bytes>0)memcpy(np->packet,op->packet,(size_t)op->bytes);
  ts->npkts++;
  return 0;
}

static int tstream_encodeq(tstream *ts,int w,int h,int nframes,
 int kf_force,int quality,tgen gen){
  th_info       info;
  th_comment    comment;
  th_enc_ctx   *enc;
  th_ycbcr_buffer img;
  ogg_packet    op;
  unsigned char *y;
  unsigned char *u;
  unsigned char *v;
  int           f;
  int           ret;
  memset(ts,0,sizeof(*ts));
  ts->w=w;
  ts->h=h;
  y=(unsigned char *)malloc((size_t)w*h);
  u=(unsigned char *)malloc((size_t)w*h/4);
  v=(unsigned char *)malloc((size_t)w*h/4);
  enc=NULL;
  ret=-1;
  th_info_init(&info);
  th_comment_init(&comment);
  info.frame_width=info.pic_width=w;
  info.frame_height=info.pic_height=h;
  info.fps_numerator=25;
  info.fps_denominator=1;
  info.aspect_numerator=info.aspect_denominator=1;
  info.pixel_fmt=TH_PF_420;
  info.quality=quality;
  info.keyframe_granule_shift=6;
  if(y==NULL||u==NULL||v==NULL)goto end;
  enc=th_encode_alloc(&info);
  if(enc==NULL)goto end;
  if(kf_force>0&&th_encode_ctl(enc,TH_ENCCTL_SET_KEYFRAME_FREQUENCY_FORCE,
   &kf_force,sizeof(kf_force))!=0)goto end;
  while(th_encode_flushheader(enc,&comment,&op)>0){
    if(th_decode_headerin(&ts->info,&comment,&ts->setup,&op)<=0)goto end;
  }
  img[0]=(th_img_plane){w,h,w,y};
  img[1]=(th_img_plane){w/2,h/2,w/2,u};
  img[2]=(th_img_plane){w/2,h/2,w/2,v};
  for(f=0;f<nframes;f++){
    (*gen)(f,y,u,v,w,h);
    if(th_encode_ycbcr_in(enc,img)!=0)goto end;
    while(th_encode_packetout(enc,f==nframes-1,&op)>0){
      if(tstream_append(ts,&op)<0)goto end;
    }
  }
  if(ts->npkts!=nframes)goto end;
  ret=0;
 end:
  th_encode_free(enc);
  th_info_clear(&info);
  th_comment_clear(&comment);
  free(y);
  free(u);
  free(v);
  if(ret<0){
    int i;
    for(i=0;i<ts->npkts;i++)free(ts->pkts[i].packet);
    free(ts->pkts);
    th_setup_free(ts->setup);
    th_info_clear(&ts->info);
    memset(ts,0,sizeof(*ts));
  }
  return ret;
}

static int tstream_encode(tstream *ts,int w,int h,int nframes,int kf_force,
 tgen gen){
  return tstream_encodeq(ts,w,h,nframes,kf_force,42,gen);
}

static void tstream_free(tstream *ts){
  int i;
  for(i=0;i<ts->npkts;i++)free(ts->pkts[i].packet);
  free(ts->pkts);
  th_setup_free(ts->setup);
  th_info_clear(&ts->info);
  memset(ts,0,sizeof(*ts));
}

/*Decodes the whole stream (plus ndup trailing duplicate packets) with the
   normal decoder, post-processing disabled, and returns owned copies of
   all displayed frames.*/
static tframe *ref_decode(tstream *ts,int ndup){
  th_dec_ctx   *dec;
  tframe       *frames;
  ogg_int64_t   gp;
  int           pp;
  int           f;
  int           n;
  int           p;
  n=ts->npkts+ndup;
  frames=(tframe *)calloc((size_t)n,sizeof(*frames));
  if(frames==NULL)return NULL;
  dec=th_decode_alloc(&ts->info,ts->setup);
  if(dec==NULL){
    free(frames);
    return NULL;
  }
  pp=0;
  if(th_decode_ctl(dec,TH_DECCTL_SET_PPLEVEL,&pp,sizeof(pp))!=0){
    th_decode_free(dec);
    free(frames);
    return NULL;
  }
  for(f=0;f<n;f++){
    ogg_packet  op;
    th_ycbcr_buffer yc;
    size_t      ys;
    size_t      cs;
    if(f<ts->npkts)op=ts->pkts[f];
    else memset(&op,0,sizeof(op));
    if(th_decode_packetin(dec,&op,&gp)<0||th_decode_ycbcr_out(dec,yc)!=0){
      th_decode_free(dec);
      free(frames);
      return NULL;
    }
    ys=(size_t)yc[0].width*yc[0].height;
    cs=(size_t)yc[1].width*yc[1].height;
    frames[f].data=(unsigned char *)malloc(ys+2*cs);
    if(frames[f].data==NULL){
      th_decode_free(dec);
      free(frames);
      return NULL;
    }
    for(p=0;p<3;p++){
      int row;
      frames[f].buf[p].width=yc[p].width;
      frames[f].buf[p].height=yc[p].height;
      frames[f].buf[p].stride=yc[p].width;
      frames[f].buf[p].data=frames[f].data+(p?ys+(p-1)*cs:0);
      for(row=0;row<yc[p].height;row++){
        memcpy(frames[f].buf[p].data+(ptrdiff_t)row*yc[p].width,
         yc[p].data+(ptrdiff_t)row*yc[p].stride,(size_t)yc[p].width);
      }
    }
  }
  th_decode_free(dec);
  return frames;
}

static void ref_free(tframe *frames,int n){
  int f;
  if(frames!=NULL){
    for(f=0;f<n;f++)free(frames[f].data);
    free(frames);
  }
}

/*Checks one region of one frame against the full-decode reference.*/
static int check_region(th_region_ctx *r,size_t f,int x,int y,int w,int h,
 tframe *refs){
  th_ycbcr_buffer out;
  int             p;
  int             rr;
  TASK_CHECK(th_region_out(r,f,x,y,w,h,out)==0);
  for(p=0;p<3;p++){
    int ew;
    int eh;
    int ox;
    int oy;
    ew=p?w/2:w;
    eh=p?h/2:h;
    ox=p?x/2:x;
    oy=p?y/2:y;
    TASK_CHECK(out[p].width==ew&&out[p].height==eh&&out[p].stride>0&&
     out[p].data!=NULL);
    for(rr=0;rr<eh;rr++){
      TASK_CHECK(!memcmp(out[p].data+(ptrdiff_t)rr*out[p].stride,
       refs[f].buf[p].data+(ptrdiff_t)(rr+oy)*refs[f].buf[p].stride+ox,
       (size_t)ew));
    }
  }
  return 0;
}

static th_region_ctx *region_of(tstream *ts,size_t max_frames){
  return th_region_alloc(&ts->info,ts->setup,max_frames);
}

static int feed_packets(th_region_ctx *r,tstream *ts,int n){
  int i;
  for(i=0;i<n;i++)TASK_CHECK(th_region_packetin(r,ts->pkts+i)==0);
  return 0;
}



/*---- Pixels ----*/

/*Textured key frame and real inter frames: regions anywhere in the
   picture must match the full decoder byte for byte.*/
static int case_pixels_regions(void){
  static const int REGS[][4]={
    {0,0,16,16},{16,16,16,16},{0,0,96,64},{8,24,32,16},{2,2,30,28},
    {48,0,48,64},{0,32,64,32},{80,48,16,16},{14,6,2,2},{94,62,2,2},
    {0,0,2,64},{94,0,2,64},{0,62,96,2},{0,0,96,2},{32,16,24,40}
  };
  tstream ts;
  tframe *refs;
  th_region_ctx *r;
  size_t ri;
  int    f;
  int    ret;
  ret=1;
  refs=NULL;
  r=NULL;
  TASK_CHECK(tstream_encode(&ts,96,64,4,0,gen_motion)==0);
  refs=ref_decode(&ts,0);
  TASK_CHECK(refs!=NULL);
  r=region_of(&ts,4);
  TASK_CHECK(r!=NULL);
  TASK_CHECK(feed_packets(r,&ts,4)==0);
  for(f=0;f<4;f++){
    for(ri=0;ri<sizeof(REGS)/sizeof(REGS[0]);ri++){
      TASK_CHECK(check_region(r,(size_t)f,REGS[ri][0],REGS[ri][1],
       REGS[ri][2],REGS[ri][3],refs)==0);
    }
  }
  ret=0;
  th_region_free(r);
  ref_free(refs,4);
  tstream_free(&ts);
  return ret;
}

/*The same pixels must come out no matter in which order frames and
   regions are requested.*/
static int case_pixels_order(void){
  tstream ts;
  tframe *refs;
  th_region_ctx *r;
  int    ret;
  ret=1;
  refs=NULL;
  r=NULL;
  TASK_CHECK(tstream_encode(&ts,96,64,4,0,gen_motion)==0);
  refs=ref_decode(&ts,0);
  TASK_CHECK(refs!=NULL);
  r=region_of(&ts,4);
  TASK_CHECK(r!=NULL);
  TASK_CHECK(feed_packets(r,&ts,4)==0);
  /*Backwards frame order, scattered regions.*/
  TASK_CHECK(check_region(r,3,16,16,32,32,refs)==0);
  TASK_CHECK(check_region(r,1,48,0,48,64,refs)==0);
  TASK_CHECK(check_region(r,2,0,32,64,32,refs)==0);
  TASK_CHECK(check_region(r,0,16,16,16,16,refs)==0);
  TASK_CHECK(check_region(r,3,16,16,32,32,refs)==0);
  TASK_CHECK(check_region(r,0,0,0,96,64,refs)==0);
  TASK_CHECK(check_region(r,2,0,32,64,32,refs)==0);
  ret=0;
  th_region_free(r);
  ref_free(refs,4);
  tstream_free(&ts);
  return ret;
}

/*Duplicate (empty) packets occupy frame numbers and must reproduce the
   previous frame exactly.*/
static int case_pixels_dupframes(void){
  tstream ts;
  tframe *refs;
  th_region_ctx *r;
  ogg_packet dup;
  int    ret;
  ret=1;
  refs=NULL;
  r=NULL;
  TASK_CHECK(tstream_encode(&ts,96,64,3,0,gen_motion)==0);
  refs=ref_decode(&ts,2);
  TASK_CHECK(refs!=NULL);
  r=region_of(&ts,5);
  TASK_CHECK(r!=NULL);
  TASK_CHECK(feed_packets(r,&ts,3)==0);
  memset(&dup,0,sizeof(dup));
  TASK_CHECK(th_region_packetin(r,&dup)==0);
  TASK_CHECK(th_region_packetin(r,&dup)==0);
  /*Frames 3 and 4 are duplicates of frame 2.*/
  TASK_CHECK(check_region(r,3,0,0,96,64,refs)==0);
  TASK_CHECK(check_region(r,4,16,16,32,32,refs)==0);
  TASK_CHECK(check_region(r,4,0,0,96,64,refs)==0);
  ret=0;
  th_region_free(r);
  ref_free(refs,5);
  tstream_free(&ts);
  return ret;
}

/*A later key frame atomically replaces the GOP; decoding continues on the
   new GOP with frame numbers restarting at 0.*/
static int case_pixels_keyframe_replace(void){
  tstream ts;
  tframe *refs;
  th_region_ctx *r;
  int    ret;
  ret=1;
  refs=NULL;
  r=NULL;
  /*keyframe_frequency_force=2: frames 0, 2 and 4 are key frames.*/
  TASK_CHECK(tstream_encode(&ts,64,64,5,2,gen_motion)==0);
  TASK_CHECK(ts.npkts==5);
  TASK_CHECK(th_packet_iskeyframe(ts.pkts+0)==1);
  TASK_CHECK(th_packet_iskeyframe(ts.pkts+2)==1);
  TASK_CHECK(th_packet_iskeyframe(ts.pkts+4)==1);
  refs=ref_decode(&ts,0);
  TASK_CHECK(refs!=NULL);
  r=region_of(&ts,3);
  TASK_CHECK(r!=NULL);
  /*First GOP: frames 0..1.*/
  TASK_CHECK(th_region_packetin(r,ts.pkts+0)==0);
  TASK_CHECK(th_region_packetin(r,ts.pkts+1)==0);
  TASK_CHECK(check_region(r,0,0,0,64,64,refs)==0);
  TASK_CHECK(check_region(r,1,8,8,32,32,refs)==0);
  /*Second key frame replaces the GOP and restarts numbering.*/
  TASK_CHECK(th_region_packetin(r,ts.pkts+2)==0);
  {
    th_ycbcr_buffer tmp;
    TASK_CHECK(th_region_out(r,2,0,0,16,16,tmp)==TH_EINVAL);
  }
  TASK_CHECK(check_region(r,0,0,0,64,64,refs+2)==0);
  TASK_CHECK(th_region_packetin(r,ts.pkts+3)==0);
  TASK_CHECK(check_region(r,1,16,0,32,48,refs+2)==0);
  /*And a third GOP.*/
  TASK_CHECK(th_region_packetin(r,ts.pkts+4)==0);
  TASK_CHECK(check_region(r,0,32,32,32,32,refs+4)==0);
  ret=0;
  th_region_free(r);
  ref_free(refs,5);
  tstream_free(&ts);
  return ret;
}

/*The vanilla decoder and encoder stay compatible with the material the
   region decoder consumes, and a full-frame region equals a full-frame
   decode of every frame.*/
static int case_pixels_full_frames(void){
  tstream ts;
  tframe *refs;
  th_region_ctx *r;
  int    f;
  int    ret;
  ret=1;
  refs=NULL;
  r=NULL;
  TASK_CHECK(tstream_encode(&ts,96,64,6,0,gen_motion)==0);
  refs=ref_decode(&ts,0);
  TASK_CHECK(refs!=NULL);
  r=region_of(&ts,6);
  TASK_CHECK(r!=NULL);
  TASK_CHECK(feed_packets(r,&ts,6)==0);
  for(f=0;f<6;f++)TASK_CHECK(check_region(r,(size_t)f,0,0,96,64,refs)==0);
  ret=0;
  th_region_free(r);
  ref_free(refs,6);
  tstream_free(&ts);
  return ret;
}



/*Different quality levels exercise both ends of the in-loop filter
   strength, including streams where it is disabled entirely (qi>=47 with
   the default quantizer tables).*/
static int case_pixels_quality_extremes(void){
  static const int QUAL[]={0,10,63};
  tstream ts;
  tframe *refs;
  th_region_ctx *r;
  int    q;
  int    f;
  int    ret;
  ret=1;
  for(q=0;q<3;q++){
    refs=NULL;
    r=NULL;
    TASK_CHECK(tstream_encodeq(&ts,96,64,3,0,QUAL[q],gen_motion)==0);
    refs=ref_decode(&ts,0);
    TASK_CHECK(refs!=NULL);
    r=region_of(&ts,3);
    TASK_CHECK(r!=NULL);
    TASK_CHECK(feed_packets(r,&ts,3)==0);
    for(f=0;f<3;f++){
      TASK_CHECK(check_region(r,(size_t)f,0,0,96,64,refs)==0);
      TASK_CHECK(check_region(r,(size_t)f,14,10,34,26,refs)==0);
    }
    th_region_free(r);
    ref_free(refs,3);
    tstream_free(&ts);
  }
  ret=0;
  return ret;
}



/*---- Dependencies ----*/

/*Re-reading the same region must not materialize anything new; expanding
   or overlapping requests only fill in the missing blocks.*/
static int case_deps_reread_expand(void){
  tstream ts;
  th_region_ctx *r;
  th_ycbcr_buffer out;
  size_t b0;
  size_t b1;
  size_t b2;
  size_t b3;
  size_t p0;
  size_t p1;
  size_t p2;
  size_t p3;
  int    ret;
  ret=1;
  r=NULL;
  TASK_CHECK(tstream_encode(&ts,96,64,3,0,gen_motion)==0);
  r=region_of(&ts,3);
  TASK_CHECK(r!=NULL);
  TASK_CHECK(feed_packets(r,&ts,3)==0);
  TASK_CHECK(th_region_out(r,2,32,16,16,16,out)==0);
  TASK_CHECK(th_region_stats(r,&b0,&p0)==0);
  TASK_CHECK(b0>0&&p0>0);
  /*Identical request: nothing new.*/
  TASK_CHECK(th_region_out(r,2,32,16,16,16,out)==0);
  TASK_CHECK(th_region_stats(r,&b1,&p1)==0);
  TASK_CHECK(b1==b0&&p1==p0);
  /*Same frame, same place, other planes already cached: still nothing.*/
  TASK_CHECK(th_region_out(r,2,32,16,16,16,out)==0);
  TASK_CHECK(th_region_stats(r,&b2,&p2)==0);
  TASK_CHECK(b2==b0&&p2==p0);
  /*Overlapping expansion: only the missing blocks are added.*/
  TASK_CHECK(th_region_out(r,2,32,16,32,32,out)==0);
  TASK_CHECK(th_region_stats(r,&b3,&p3)==0);
  TASK_CHECK(b3>b0&&b3-b0<=(size_t)(3*(4*4+2*(2*2)))*2);
  TASK_CHECK(p3>=p0);
  /*Expanding to the whole frame fills in everything that is missing: the
     far corners cannot be part of a small central region's dependency
     closure, so this must add blocks.*/
  TASK_CHECK(th_region_out(r,2,0,0,96,64,out)==0);
  TASK_CHECK(th_region_stats(r,&b1,&p1)==0);
  TASK_CHECK(b1>b3&&p1>=p3);
  /*And it is stable afterwards.*/
  TASK_CHECK(th_region_out(r,2,0,0,96,64,out)==0);
  TASK_CHECK(th_region_stats(r,&b2,&p2)==0);
  TASK_CHECK(b2==b1&&p2==p1);
  ret=0;
  th_region_free(r);
  tstream_free(&ts);
  return ret;
}

/*The required proof: a 256x256 low-motion three-frame video; the center
   16x16 region of the last frame costs less than half of the full-segment
   full-frame amounts, both in reconstructed blocks and in persistent pixel
   storage, while staying pixel-exact. Full coverage of all frames must
   then cross-check to exactly the full-frame amounts.*/
static int case_deps_half_budget(void){
  enum{W=256,H=256,NF=3};
  tstream ts;
  tframe *refs;
  th_region_ctx *r;
  th_ycbcr_buffer out;
  size_t blocks;
  size_t pbytes;
  size_t full_blocks;
  size_t full_bytes;
  int    f;
  int    tx;
  int    ty;
  int    ret;
  ret=1;
  refs=NULL;
  r=NULL;
  full_blocks=(size_t)NF*((W/8)*(H/8)+2*(W/16)*(H/16));
  full_bytes=(size_t)NF*((size_t)W*H+2*(size_t)(W/2)*(H/2));
  TASK_CHECK(tstream_encode(&ts,W,H,NF,0,gen_lowmotion)==0);
  refs=ref_decode(&ts,0);
  TASK_CHECK(refs!=NULL);
  r=region_of(&ts,NF);
  TASK_CHECK(r!=NULL);
  TASK_CHECK(feed_packets(r,&ts,NF)==0);
  /*Center 16x16 region of the last frame.*/
  TASK_CHECK(check_region(r,NF-1,120,120,16,16,refs)==0);
  TASK_CHECK(th_region_stats(r,&blocks,&pbytes)==0);
  TASK_CHECK(blocks>0&&blocks<full_blocks/2);
  TASK_CHECK(pbytes>0&&pbytes<full_bytes/2);
  /*Cross-check the accounting: covering every frame completely with 16x16
     tiles must materialize exactly the full-frame amounts.*/
  for(f=0;f<NF;f++)for(ty=0;ty<H;ty+=16)for(tx=0;tx<W;tx+=16){
    TASK_CHECK(th_region_out(r,(size_t)f,tx,ty,16,16,out)==0);
  }
  TASK_CHECK(th_region_stats(r,&blocks,&pbytes)==0);
  TASK_CHECK(blocks==full_blocks);
  TASK_CHECK(pbytes==full_bytes);
  ret=0;
  th_region_free(r);
  ref_free(refs,NF);
  tstream_free(&ts);
  return ret;
}

/*Statistics are independent of the order in which regions are requested.*/
static int case_deps_order_independence(void){
  tstream ts;
  th_region_ctx *ra;
  th_region_ctx *rb;
  th_ycbcr_buffer out;
  size_t ba;
  size_t bb;
  size_t pa;
  size_t pb;
  int    ret;
  ret=1;
  ra=rb=NULL;
  TASK_CHECK(tstream_encode(&ts,96,64,4,0,gen_motion)==0);
  ra=region_of(&ts,4);
  rb=region_of(&ts,4);
  TASK_CHECK(ra!=NULL&&rb!=NULL);
  TASK_CHECK(feed_packets(ra,&ts,4)==0);
  TASK_CHECK(feed_packets(rb,&ts,4)==0);
  /*Forward on ra, scattered on rb.*/
  TASK_CHECK(th_region_out(ra,0,16,16,16,16,out)==0);
  TASK_CHECK(th_region_out(ra,1,16,16,16,16,out)==0);
  TASK_CHECK(th_region_out(ra,2,16,16,16,16,out)==0);
  TASK_CHECK(th_region_out(ra,3,16,16,16,16,out)==0);
  TASK_CHECK(th_region_out(rb,3,16,16,16,16,out)==0);
  TASK_CHECK(th_region_out(rb,0,16,16,16,16,out)==0);
  TASK_CHECK(th_region_out(rb,2,16,16,16,16,out)==0);
  TASK_CHECK(th_region_out(rb,1,16,16,16,16,out)==0);
  TASK_CHECK(th_region_stats(ra,&ba,&pa)==0);
  TASK_CHECK(th_region_stats(rb,&bb,&pb)==0);
  TASK_CHECK(ba==bb&&pa==pb);
  ret=0;
  th_region_free(ra);
  th_region_free(rb);
  tstream_free(&ts);
  return ret;
}

/*Non-key packets beyond the GOP capacity are rejected while existing
   regions stay readable; key frames are always accepted and reset the
   statistics.*/
static int case_deps_capacity(void){
  tstream ts;
  th_region_ctx *r;
  th_ycbcr_buffer out;
  size_t b0;
  size_t b1;
  size_t p0;
  size_t p1;
  int    ret;
  ret=1;
  r=NULL;
  /*keyframe_frequency_force=3: frames 0 and 3 are key frames.*/
  TASK_CHECK(tstream_encode(&ts,64,64,5,3,gen_motion)==0);
  TASK_CHECK(th_packet_iskeyframe(ts.pkts+0)==1);
  TASK_CHECK(th_packet_iskeyframe(ts.pkts+1)==0);
  TASK_CHECK(th_packet_iskeyframe(ts.pkts+2)==0);
  TASK_CHECK(th_packet_iskeyframe(ts.pkts+3)==1);
  r=region_of(&ts,2);
  TASK_CHECK(r!=NULL);
  TASK_CHECK(th_region_packetin(r,ts.pkts+0)==0);
  TASK_CHECK(th_region_packetin(r,ts.pkts+1)==0);
  /*The GOP is full: the next non-key packet is rejected with TH_EIMPL and
     the existing state is untouched.*/
  TASK_CHECK(th_region_packetin(r,ts.pkts+2)==TH_EIMPL);
  TASK_CHECK(th_region_out(r,0,0,0,32,32,out)==0);
  TASK_CHECK(th_region_out(r,1,16,16,32,32,out)==0);
  TASK_CHECK(th_region_stats(r,&b0,&p0)==0);
  TASK_CHECK(b0>0);
  TASK_CHECK(th_region_packetin(r,ts.pkts+2)==TH_EIMPL);
  TASK_CHECK(th_region_stats(r,&b1,&p1)==0);
  TASK_CHECK(b1==b0&&p1==p0);
  /*A key frame is always accepted, replaces the GOP and resets stats.*/
  TASK_CHECK(th_region_packetin(r,ts.pkts+3)==0);
  TASK_CHECK(th_region_stats(r,&b1,&p1)==0);
  TASK_CHECK(b1==0&&p1==0);
  TASK_CHECK(th_region_out(r,0,0,0,64,64,out)==0);
  TASK_CHECK(th_region_stats(r,&b1,&p1)==0);
  TASK_CHECK(b1>0&&b1<b0);
  ret=0;
  th_region_free(r);
  tstream_free(&ts);
  return ret;
}



/*---- Lifetime ----*/

/*Unsupported configurations and bad arguments are rejected at allocation
   time.*/
static int case_life_alloc_reject(void){
  tstream ts;
  th_region_ctx *r;
  th_info  info;
  int      ret;
  ret=1;
  TASK_CHECK(tstream_encode(&ts,64,64,2,0,gen_motion)==0);
  TASK_CHECK(th_region_alloc(NULL,ts.setup,2)==NULL);
  TASK_CHECK(th_region_alloc(&ts.info,NULL,2)==NULL);
  TASK_CHECK(th_region_alloc(&ts.info,ts.setup,0)==NULL);
  info=ts.info;
  info.pixel_fmt=TH_PF_422;
  TASK_CHECK(th_region_alloc(&info,ts.setup,2)==NULL);
  info=ts.info;
  info.pixel_fmt=TH_PF_444;
  TASK_CHECK(th_region_alloc(&info,ts.setup,2)==NULL);
  info=ts.info;
  info.pic_width=info.frame_width-2;
  TASK_CHECK(th_region_alloc(&info,ts.setup,2)==NULL);
  info=ts.info;
  info.pic_x=2;
  TASK_CHECK(th_region_alloc(&info,ts.setup,2)==NULL);
  info=ts.info;
  info.pic_height=info.frame_height-16;
  TASK_CHECK(th_region_alloc(&info,ts.setup,2)==NULL);
  r=th_region_alloc(&ts.info,ts.setup,1);
  TASK_CHECK(r!=NULL);
  th_region_free(r);
  ret=0;
  tstream_free(&ts);
  return ret;
}

/*Parameter and state errors on packets and requests.*/
static int case_life_bad_calls(void){
  tstream ts;
  th_region_ctx *r;
  th_ycbcr_buffer out;
  ogg_packet hdr;
  size_t b0;
  size_t p0;
  size_t b1;
  size_t p1;
  int    ret;
  ret=1;
  r=NULL;
  TASK_CHECK(tstream_encode(&ts,64,64,3,0,gen_motion)==0);
  r=region_of(&ts,3);
  TASK_CHECK(r!=NULL);
  TASK_CHECK(th_region_packetin(NULL,ts.pkts+0)==TH_EINVAL);
  TASK_CHECK(th_region_packetin(r,NULL)==TH_EINVAL);
  /*A header packet is not a data packet.*/
  hdr=ts.pkts[0];
  hdr.packet=(unsigned char *)"\x80""theora";
  hdr.bytes=7;
  TASK_CHECK(th_region_packetin(r,&hdr)==TH_EINVAL);
  /*The first packet must be a key frame.*/
  TASK_CHECK(th_packet_iskeyframe(ts.pkts+1)==0);
  TASK_CHECK(th_region_packetin(r,ts.pkts+1)==TH_EINVAL);
  /*An empty packet is not a key frame either.*/
  memset(&hdr,0,sizeof(hdr));
  TASK_CHECK(th_region_packetin(r,&hdr)==TH_EINVAL);
  TASK_CHECK(th_region_packetin(r,ts.pkts+0)==0);
  TASK_CHECK(th_region_packetin(r,ts.pkts+1)==0);
  /*Bad out requests do not disturb the caches.*/
  TASK_CHECK(th_region_out(r,0,0,0,32,32,out)==0);
  TASK_CHECK(th_region_stats(r,&b0,&p0)==0);
  TASK_CHECK(th_region_out(r,5,0,0,16,16,out)==TH_EINVAL);
  TASK_CHECK(th_region_out(r,0,-2,0,16,16,out)==TH_EINVAL);
  TASK_CHECK(th_region_out(r,0,0,0,0,16,out)==TH_EINVAL);
  TASK_CHECK(th_region_out(r,0,0,0,16,0,out)==TH_EINVAL);
  TASK_CHECK(th_region_out(r,0,1,0,16,16,out)==TH_EINVAL);
  TASK_CHECK(th_region_out(r,0,0,3,16,16,out)==TH_EINVAL);
  TASK_CHECK(th_region_out(r,0,0,0,15,16,out)==TH_EINVAL);
  TASK_CHECK(th_region_out(r,0,0,0,16,17,out)==TH_EINVAL);
  TASK_CHECK(th_region_out(r,0,48,0,32,32,out)==TH_EINVAL);
  TASK_CHECK(th_region_out(r,0,0,48,16,32,out)==TH_EINVAL);
  TASK_CHECK(th_region_out(r,0,62,62,4,4,out)==TH_EINVAL);
  TASK_CHECK(th_region_stats(r,&b1,&p1)==0);
  TASK_CHECK(b1==b0&&p1==p0);
  TASK_CHECK(th_region_stats(NULL,&b1,&p1)==TH_EINVAL);
  ret=0;
  th_region_free(r);
  tstream_free(&ts);
  return ret;
}

/*The setup information may be released right after allocation, and input
   packets are only borrowed for the packetin call.*/
static int case_life_setup_and_packets(void){
  tstream ts;
  tframe *refs;
  th_region_ctx *r;
  th_setup_info *setup;
  th_info  parsed;
  th_comment comment;
  ogg_packet op;
  int    ret;
  ret=1;
  refs=NULL;
  r=NULL;
  setup=NULL;
  TASK_CHECK(tstream_encode(&ts,64,64,2,0,gen_motion)==0);
  /*Rebuild the headers to get an independent setup handle.*/
  th_info_init(&parsed);
  th_comment_init(&comment);
  {
    th_enc_ctx *enc;
    th_info     info;
    th_comment  ec;
    th_info_init(&info);
    th_comment_init(&ec);
    info=ts.info;
    enc=th_encode_alloc(&info);
    TASK_CHECK(enc!=NULL);
    while(th_encode_flushheader(enc,&ec,&op)>0){
      TASK_CHECK(th_decode_headerin(&parsed,&comment,&setup,&op)>0);
    }
    th_encode_free(enc);
    th_comment_clear(&ec);
  }
  r=th_region_alloc(&parsed,setup,2);
  TASK_CHECK(r!=NULL);
  /*Release the setup information immediately.*/
  th_setup_free(setup);
  setup=NULL;
  refs=ref_decode(&ts,0);
  TASK_CHECK(refs!=NULL);
  {
    /*Packets are only borrowed: clobber them after packetin.*/
    ogg_packet p0;
    ogg_packet p1;
    p0=ts.pkts[0];
    p1=ts.pkts[1];
    TASK_CHECK(th_region_packetin(r,&p0)==0);
    memset(p0.packet,0xA5,(size_t)p0.bytes);
    TASK_CHECK(th_region_packetin(r,&p1)==0);
    memset(p1.packet,0x5A,(size_t)p1.bytes);
  }
  TASK_CHECK(check_region(r,0,0,0,64,64,refs)==0);
  TASK_CHECK(check_region(r,1,16,16,32,32,refs)==0);
  ret=0;
  th_region_free(r);
  ref_free(refs,2);
  th_info_clear(&parsed);
  th_comment_clear(&comment);
  tstream_free(&ts);
  return ret;
}

/*The lent output buffer stays valid until the next out call, the next
   committed key frame, or free.*/
static int case_life_out_buffer(void){
  tstream ts;
  tframe *refs;
  th_region_ctx *r;
  th_ycbcr_buffer out1;
  th_ycbcr_buffer out2;
  unsigned char saved[3][16*16];
  int    p;
  int    ret;
  ret=1;
  refs=NULL;
  r=NULL;
  /*keyframe_frequency_force=4: only frames 0 and 4 are key frames.*/
  TASK_CHECK(tstream_encode(&ts,64,64,5,4,gen_motion)==0);
  TASK_CHECK(th_packet_iskeyframe(ts.pkts+2)==0);
  TASK_CHECK(th_packet_iskeyframe(ts.pkts+4)==1);
  refs=ref_decode(&ts,0);
  TASK_CHECK(refs!=NULL);
  r=region_of(&ts,5);
  TASK_CHECK(r!=NULL);
  TASK_CHECK(feed_packets(r,&ts,2)==0);
  TASK_CHECK(th_region_out(r,1,16,16,16,16,out1)==0);
  /*The lent memory must stay valid while no other call invalidates it.*/
  for(p=0;p<3;p++){
    memcpy(saved[p],out1[p].data,(size_t)(out1[p].width*out1[p].height));
  }
  TASK_CHECK(th_region_packetin(r,ts.pkts+2)==0);
  TASK_CHECK(th_region_packetin(r,ts.pkts+3)==0);
  for(p=0;p<3;p++){
    TASK_CHECK(!memcmp(saved[p],out1[p].data,
     (size_t)(out1[p].width*out1[p].height)));
  }
  /*A later out call may reuse the buffer; the new content is correct.*/
  TASK_CHECK(th_region_out(r,3,0,0,32,32,out2)==0);
  TASK_CHECK(check_region(r,3,0,0,32,32,refs)==0);
  /*A committed key frame may invalidate it; decoding continues.*/
  TASK_CHECK(th_region_packetin(r,ts.pkts+4)==0);
  TASK_CHECK(check_region(r,0,0,0,64,64,refs+4)==0);
  ret=0;
  th_region_free(r);
  ref_free(refs,5);
  tstream_free(&ts);
  return ret;
}

/*Repeated allocation and release cycles, including th_region_free(NULL),
   and the normal decode/encode paths keep working alongside.*/
static int case_life_cycles(void){
  tstream ts;
  tframe *refs;
  th_region_ctx *r;
  th_dec_ctx *dec;
  th_ycbcr_buffer yc;
  ogg_int64_t gp;
  int    i;
  int    f;
  int    pp;
  int    ret;
  ret=1;
  refs=NULL;
  TASK_CHECK(tstream_encode(&ts,64,64,3,0,gen_motion)==0);
  th_region_free(NULL);
  for(i=0;i<4;i++){
    r=region_of(&ts,3);
    TASK_CHECK(r!=NULL);
    TASK_CHECK(feed_packets(r,&ts,3)==0);
    th_region_free(r);
  }
  /*The vanilla decoder still decodes the same packets fine.*/
  refs=ref_decode(&ts,0);
  TASK_CHECK(refs!=NULL);
  dec=th_decode_alloc(&ts.info,ts.setup);
  TASK_CHECK(dec!=NULL);
  pp=0;
  TASK_CHECK(th_decode_ctl(dec,TH_DECCTL_SET_PPLEVEL,&pp,sizeof(pp))==0);
  for(f=0;f<ts.npkts;f++){
    int row;
    TASK_CHECK(th_decode_packetin(dec,ts.pkts+f,&gp)>=0);
    TASK_CHECK(th_decode_ycbcr_out(dec,yc)==0);
    for(row=0;row<yc[0].height;row++){
      TASK_CHECK(!memcmp(yc[0].data+(ptrdiff_t)row*yc[0].stride,
       refs[f].buf[0].data+(ptrdiff_t)row*refs[f].buf[0].stride,
       (size_t)yc[0].width));
    }
  }
  th_decode_free(dec);
  ret=0;
  ref_free(refs,3);
  tstream_free(&ts);
  return ret;
}



const struct task_case task_cases[]={
  {"Pixels","regions",&case_pixels_regions},
  {"Pixels","request_order",&case_pixels_order},
  {"Pixels","dup_frames",&case_pixels_dupframes},
  {"Pixels","keyframe_replace",&case_pixels_keyframe_replace},
  {"Pixels","full_frames",&case_pixels_full_frames},
  {"Pixels","quality_extremes",&case_pixels_quality_extremes},
  {"Dependencies","reread_expand",&case_deps_reread_expand},
  {"Dependencies","half_budget",&case_deps_half_budget},
  {"Dependencies","order_independence",&case_deps_order_independence},
  {"Dependencies","capacity",&case_deps_capacity},
  {"Lifetime","alloc_reject",&case_life_alloc_reject},
  {"Lifetime","bad_calls",&case_life_bad_calls},
  {"Lifetime","setup_and_packets",&case_life_setup_and_packets},
  {"Lifetime","out_buffer",&case_life_out_buffer},
  {"Lifetime","cycles",&case_life_cycles}
};
const size_t task_case_count=sizeof(task_cases)/sizeof(task_cases[0]);
