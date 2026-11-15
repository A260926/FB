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

  function: end-to-end loop filter regression test on a real stream.

  Generates a short video in memory, encodes it with this library, then
  decodes it through the public API (th_decode_packetin,
  th_decode_ycbcr_out, TH_DECCTL_SET_STRIPE_CB).  A test-only
  observation point compiled into the test build of lib/decode.c
  (OC_LOOPFILTER_OBSERVER) hands the reconstructed, not-yet-filtered
  pixels and the coded-fragment map of every frame to this test, which
  then predicts the filtered output with the independent reference model
  in loopfilter_ref.h and compares it against the actual decoder output
  of every single frame (not just the last one).

  Named cases printed on stdout:
    stream_frames_match_reference  per-frame filtered output equals the
                                   independent reference prediction
    stripe_delivery_stable         rows handed to the stripe callback
                                   are final and each visible row is
                                   delivered exactly once

 ********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <theora/theoraenc.h>
#include <theora/theoradec.h>

#include "state.h"
#include "loopfilter_ref.h"

#define FRAME_W 96
#define FRAME_H 80
#define NFRAMES 4
#define QUALITY 16

static int g_case_failed;

static void case_fail(const char *_case,const char *_what,int _a,int _b,
 int _c,int _d){
  printf("THEORA_LOOPFILTER_CASE %s FAIL %s %d %d %d %d\n",_case,_what,
   _a,_b,_c,_d);
  g_case_failed=1;
}

/* ------------------------------------------------------------------ */
/* Observer: collects pre-filter pixels and the coded map              */

static struct{
  ogg_int64_t frame_id;
  int flimit;
  int nh[3],nv[3];
  ptrdiff_t froff[3];
  unsigned char *snap[3];  /*pre-filter snapshot, internal row order*/
  unsigned char *coded;    /*coded flags of all fragments, by plane*/
  int next_row[3];
  int calls;
  long ncoded,nuncoded,ninter;
  int ready;
}OBS;

static void obs_begin_frame(const oc_theora_state *_st){
  ptrdiff_t total;
  ptrdiff_t base;
  int pli;
  OBS.frame_id=_st->curframe_num;
  OBS.flimit=_st->loop_filter_limits[_st->qis[0]];
  total=0;
  for(pli=0;pli<3;pli++){
    OBS.nh[pli]=_st->fplanes[pli].nhfrags;
    OBS.nv[pli]=_st->fplanes[pli].nvfrags;
    OBS.froff[pli]=_st->fplanes[pli].froffset;
    total+=_st->fplanes[pli].nfrags;
  }
  free(OBS.coded);
  OBS.coded=(unsigned char *)malloc((size_t)total);
  OBS.ncoded=OBS.nuncoded=OBS.ninter=0;
  base=0;
  for(pli=0;pli<3;pli++){
    ptrdiff_t n=_st->fplanes[pli].nfrags;
    ptrdiff_t i;
    for(i=0;i<n;i++){
      int c=_st->frags[OBS.froff[pli]+i].coded?1:0;
      OBS.coded[base+i]=(unsigned char)c;
      if(c){
        OBS.ncoded++;
        if(_st->frags[OBS.froff[pli]+i].refi==OC_FRAME_PREV||
         _st->frags[OBS.froff[pli]+i].refi==OC_FRAME_GOLD)OBS.ninter++;
      }
      else OBS.nuncoded++;
    }
    base+=n;
    free(OBS.snap[pli]);
    OBS.snap[pli]=(unsigned char *)
     malloc((size_t)(OBS.nh[pli]*8)*(OBS.nv[pli]*8));
    OBS.next_row[pli]=0;
  }
  OBS.calls=0;
  OBS.ready=1;
}

/*Called by the test build of lib/decode.c right before the loop filter
   runs on a stripe of one plane.  Rows up to _recon_end*8 are fully
   reconstructed and have not been touched by any loop filter yet, so
   they are snapshotted here; the filter call that follows may only
   modify rows that have already been snapshotted.*/
void oc_loopfilter_observe(const oc_theora_state *_state,int _pli,
 int _fragy0,int _fragy_end,int _recon_end){
  int r;
  int limit;
  (void)_fragy0;
  (void)_fragy_end;
  if(!OBS.ready||_state->curframe_num!=OBS.frame_id)
    obs_begin_frame(_state);
  limit=OBS.nv[_pli]*8;
  if(_recon_end*8<limit)limit=_recon_end*8;
  for(r=OBS.next_row[_pli];r<limit;r++){
    const unsigned char *src;
    src=_state->ref_frame_data[OC_FRAME_SELF]
     +_state->frag_buf_offs[OBS.froff[_pli]+(ptrdiff_t)(r>>3)*OBS.nh[_pli]]
     +(r&7)*(ptrdiff_t)_state->ref_ystride[_pli];
    memcpy(OBS.snap[_pli]+(size_t)r*(OBS.nh[_pli]*8),src,
     (size_t)(OBS.nh[_pli]*8));
  }
  if(limit>OBS.next_row[_pli])OBS.next_row[_pli]=limit;
  OBS.calls++;
}

/* ------------------------------------------------------------------ */
/* Stripe callback capture                                             */

static struct{
  unsigned char *rows[3];  /*copy of the delivered rows, user order*/
  int w[3],h[3];
  unsigned char *delivered;/*delivery count per luma pixel row*/
  int luma_rows;
  int calls;
  int bad;
}SC;

static void on_stripe(void *_ctx,th_ycbcr_buffer _buf,int _yfrag0,
 int _yfrag_end){
  int pli;
  int r;
  (void)_ctx;
  SC.calls++;
  if(_yfrag0<0||_yfrag0>=_yfrag_end||_yfrag_end*8>SC.luma_rows){
    SC.bad=1;
    return;
  }
  for(pli=0;pli<3;pli++){
    int vdec=pli>0?1:0;
    int r0=_yfrag0*8>>vdec;
    int r1=_yfrag_end*8>>vdec;
    if(_buf[pli].width!=SC.w[pli]||_buf[pli].height!=SC.h[pli])SC.bad=1;
    for(r=r0;r<r1;r++){
      memcpy(SC.rows[pli]+(size_t)r*SC.w[pli],
       _buf[pli].data+(ptrdiff_t)r*_buf[pli].stride,(size_t)SC.w[pli]);
    }
  }
  for(r=_yfrag0*8;r<_yfrag_end*8;r++)SC.delivered[r]++;
}

/* ------------------------------------------------------------------ */
/* Video generation                                                    */

static void gen_frame(th_ycbcr_buffer _yuv,int _f){
  int x,y;
  int sx=8+4*_f;
  int sy=8+2*_f;
  for(y=0;y<FRAME_H;y++){
    for(x=0;x<FRAME_W;x++){
      int v=(x*2+y)&0xFF;
      if(((x>>4)+(y>>4))&1)v=255-v;
      if(x>=sx&&x<sx+16&&y>=sy&&y<sy+16)v=240-30*_f;
      _yuv[0].data[y*_yuv[0].stride+x]=(unsigned char)v;
    }
  }
  for(y=0;y<FRAME_H/2;y++){
    for(x=0;x<FRAME_W/2;x++){
      int cb=128+((x^y)&15)*2;
      int cr=128-((x+y)&15)*2;
      if(x*2>=sx&&x*2<sx+16&&y*2>=sy&&y*2<sy+16){
        cb=64+20*_f;
        cr=200-20*_f;
      }
      _yuv[1].data[y*_yuv[1].stride+x]=(unsigned char)cb;
      _yuv[2].data[y*_yuv[2].stride+x]=(unsigned char)cr;
    }
  }
}

/* ------------------------------------------------------------------ */

typedef struct{
  unsigned char *data;
  long bytes;
  int keyframe;
}pkt;

int main(void){
  th_info info;
  th_comment tc;
  th_enc_ctx *enc;
  th_dec_ctx *dec;
  th_setup_info *setup;
  th_ycbcr_buffer yuv;
  th_ycbcr_buffer out;
  unsigned char *framebuf;
  pkt headers[8];
  pkt frames[NFRAMES];
  int nheaders=0;
  int nframes=0;
  ogg_packet op;
  ogg_int64_t granpos;
  th_stripe_callback scb;
  int pplevel=0;
  int ret;
  int f,pli;
  long total_ops=0;
  int keyframes=0;
  int inter_with_uncoded=0;
  long uncoded_seen=0;
  long inter_seen=0;
  int stripe_errors=0;

  memset(headers,0,sizeof(headers));
  memset(frames,0,sizeof(frames));
  memset(&OBS,0,sizeof(OBS));
  OBS.frame_id=-1;
  memset(&SC,0,sizeof(SC));

  /*---------------- Encode ----------------*/
  th_info_init(&info);
  info.frame_width=FRAME_W;
  info.frame_height=FRAME_H;
  info.pic_width=FRAME_W;
  info.pic_height=FRAME_H;
  info.pic_x=0;
  info.pic_y=0;
  info.fps_numerator=25;
  info.fps_denominator=1;
  info.aspect_numerator=1;
  info.aspect_denominator=1;
  info.colorspace=TH_CS_UNSPECIFIED;
  info.pixel_fmt=TH_PF_420;
  info.target_bitrate=0;
  info.quality=QUALITY;
  info.keyframe_granule_shift=6;
  enc=th_encode_alloc(&info);
  if(enc==NULL){
    case_fail("stream_frames_match_reference","encode_alloc",0,0,0,0);
    return 1;
  }
  th_comment_init(&tc);
  while(th_encode_flushheader(enc,&tc,&op)>0){
    if(nheaders>=8){
      case_fail("stream_frames_match_reference","too_many_headers",0,0,0,0);
      return 1;
    }
    headers[nheaders].data=(unsigned char *)malloc((size_t)op.bytes);
    memcpy(headers[nheaders].data,op.packet,(size_t)op.bytes);
    headers[nheaders].bytes=op.bytes;
    nheaders++;
  }
  framebuf=(unsigned char *)malloc(FRAME_W*FRAME_H+2*(FRAME_W/2)*(FRAME_H/2));
  yuv[0].width=FRAME_W;
  yuv[0].height=FRAME_H;
  yuv[0].stride=FRAME_W;
  yuv[0].data=framebuf;
  yuv[1].width=FRAME_W/2;
  yuv[1].height=FRAME_H/2;
  yuv[1].stride=FRAME_W/2;
  yuv[1].data=framebuf+FRAME_W*FRAME_H;
  yuv[2].width=FRAME_W/2;
  yuv[2].height=FRAME_H/2;
  yuv[2].stride=FRAME_W/2;
  yuv[2].data=yuv[1].data+(FRAME_W/2)*(FRAME_H/2);
  for(f=0;f<NFRAMES;f++){
    int got=0;
    gen_frame(yuv,f);
    if(th_encode_ycbcr_in(enc,yuv)<0){
      case_fail("stream_frames_match_reference","encode_ycbcr_in",f,0,0,0);
      return 1;
    }
    while(th_encode_packetout(enc,f==NFRAMES-1,&op)>0){
      if(nframes>=NFRAMES){
        case_fail("stream_frames_match_reference","too_many_frames",f,0,0,0);
        return 1;
      }
      frames[nframes].data=(unsigned char *)malloc((size_t)op.bytes);
      memcpy(frames[nframes].data,op.packet,(size_t)op.bytes);
      frames[nframes].bytes=op.bytes;
      frames[nframes].keyframe=th_packet_iskeyframe(&op);
      nframes++;
      got++;
    }
    if(got!=1){
      case_fail("stream_frames_match_reference","packets_per_frame",f,got,
       0,0);
      return 1;
    }
  }
  th_encode_free(enc);
  th_comment_clear(&tc);
  th_info_clear(&info);
  if(nframes!=NFRAMES){
    case_fail("stream_frames_match_reference","frame_count",nframes,0,0,0);
    return 1;
  }

  /*---------------- Decode ----------------*/
  th_info_init(&info);
  th_comment_init(&tc);
  setup=NULL;
  {
    int hi;
    ogg_packet hop;
    memset(&hop,0,sizeof(hop));
    for(hi=0;hi<nheaders;hi++){
      hop.packet=headers[hi].data;
      hop.bytes=headers[hi].bytes;
      hop.b_o_s=hi==0;
      ret=th_decode_headerin(&info,&tc,&setup,&hop);
      if(ret<0){
        case_fail("stream_frames_match_reference","bad_header",hi,ret,0,0);
        return 1;
      }
    }
  }
  dec=th_decode_alloc(&info,setup);
  if(dec==NULL){
    case_fail("stream_frames_match_reference","decode_alloc",0,0,0,0);
    return 1;
  }
  /*No out-of-loop post-processing: the output must be exactly the
     loop-filtered reference frame.*/
  if(th_decode_ctl(dec,TH_DECCTL_SET_PPLEVEL,&pplevel,sizeof(pplevel))!=0){
    case_fail("stream_frames_match_reference","set_pplevel",0,0,0,0);
    return 1;
  }
  SC.luma_rows=FRAME_H;
  SC.w[0]=FRAME_W;
  SC.h[0]=FRAME_H;
  SC.w[1]=SC.w[2]=FRAME_W/2;
  SC.h[1]=SC.h[2]=FRAME_H/2;
  for(pli=0;pli<3;pli++)
    SC.rows[pli]=(unsigned char *)malloc((size_t)SC.w[pli]*SC.h[pli]);
  SC.delivered=(unsigned char *)calloc(FRAME_H,1);
  scb.ctx=NULL;
  scb.stripe_decoded=on_stripe;
  if(th_decode_ctl(dec,TH_DECCTL_SET_STRIPE_CB,&scb,sizeof(scb))!=0){
    case_fail("stream_frames_match_reference","set_stripe_cb",0,0,0,0);
    return 1;
  }

  for(f=0;f<nframes;f++){
    long frame_ops=0;
    memset(&op,0,sizeof(op));
    op.packet=frames[f].data;
    op.bytes=frames[f].bytes;
    SC.calls=0;
    memset(SC.delivered,0,FRAME_H);
    ret=th_decode_packetin(dec,&op,&granpos);
    if(ret!=0){
      case_fail("stream_frames_match_reference","packetin",f,ret,0,0);
      return 1;
    }
    /*The observer must have seen the whole frame exactly once.*/
    if(OBS.calls<=0){
      case_fail("stream_frames_match_reference","no_observation",f,0,0,0);
      return 1;
    }
    for(pli=0;pli<3;pli++){
      if(OBS.next_row[pli]!=OBS.nv[pli]*8){
        case_fail("stream_frames_match_reference","observer_coverage",f,pli,
         OBS.next_row[pli],OBS.nv[pli]*8);
        return 1;
      }
    }
    if(OBS.flimit<=0){
      case_fail("stream_frames_match_reference","zero_flimit",f,0,0,0);
      return 1;
    }
    /*Predict the filtered frame with the independent reference.*/
    {
      ptrdiff_t base=0;
      for(pli=0;pli<3;pli++){
        int nops=lf_ref_filter_plane(OBS.snap[pli],OBS.nh[pli]*8,
         OBS.coded+base,OBS.nh[pli],OBS.nv[pli],OBS.flimit);
        if(nops<0){
          case_fail("stream_frames_match_reference","oom",f,pli,0,0);
          return 1;
        }
        frame_ops+=nops;
        base+=OBS.nh[pli]*OBS.nv[pli];
      }
    }
    if(frame_ops<=0){
      case_fail("stream_frames_match_reference","no_filter_ops",f,0,0,0);
      return 1;
    }
    total_ops+=frame_ops;
    /*Compare against the actual decoder output of this frame.*/
    if(th_decode_ycbcr_out(dec,out)!=0){
      case_fail("stream_frames_match_reference","ycbcr_out",f,0,0,0);
      return 1;
    }
    for(pli=0;pli<3;pli++){
      int h=OBS.nv[pli]*8;
      int w=OBS.nh[pli]*8;
      int yu,x;
      if(out[pli].width!=w||out[pli].height!=h){
        case_fail("stream_frames_match_reference","out_dims",f,pli,
         out[pli].width,w);
        return 1;
      }
      for(yu=0;yu<h;yu++){
        const unsigned char *got=out[pli].data+(ptrdiff_t)yu*out[pli].stride;
        const unsigned char *want=OBS.snap[pli]+(size_t)(h-1-yu)*w;
        for(x=0;x<w;x++){
          if(got[x]!=want[x]){
            printf("THEORA_LOOPFILTER_CASE stream_frames_match_reference "
             "FAIL frame=%d plane=%d pixel=(%d,%d) internal_row=%d "
             "got=%u want=%u\n",f,pli,x,yu,h-1-yu,got[x],want[x]);
            return 1;
          }
        }
      }
    }
    /*Stripe delivery checks for this frame.*/
    if(SC.calls<=0||SC.bad){
      case_fail("stripe_delivery_stable","callback",f,SC.calls,SC.bad,0);
      return 1;
    }
    {
      int r;
      for(r=0;r<FRAME_H;r++){
        if(SC.delivered[r]!=1){
          case_fail("stripe_delivery_stable","row_delivery_count",f,r,
           SC.delivered[r],0);
          return 1;
        }
      }
    }
    for(pli=0;pli<3;pli++){
      int yu;
      for(yu=0;yu<SC.h[pli];yu++){
        const unsigned char *got=out[pli].data+(ptrdiff_t)yu*out[pli].stride;
        const unsigned char *early=SC.rows[pli]+(size_t)yu*SC.w[pli];
        int x;
        for(x=0;x<SC.w[pli];x++){
          if(got[x]!=early[x]){
            printf("THEORA_LOOPFILTER_CASE stripe_delivery_stable FAIL "
             "frame=%d plane=%d pixel=(%d,%d) early=%u final=%u\n",
             f,pli,x,yu,early[x],got[x]);
            return 1;
          }
        }
      }
    }
    /*Per-frame stream property bookkeeping.*/
    if(frames[f].keyframe==1)keyframes++;
    else if(f>0&&OBS.nuncoded>0)inter_with_uncoded++;
    uncoded_seen+=OBS.nuncoded;
    inter_seen+=OBS.ninter;
  }

  /*Stream-level requirements: at least one real keyframe and at least
     two successor frames that actually use uncoded blocks.*/
  if(keyframes<1||frames[0].keyframe!=1){
    case_fail("stream_frames_match_reference","no_keyframe",keyframes,0,0,0);
    return 1;
  }
  if(inter_with_uncoded<2){
    case_fail("stream_frames_match_reference","uncoded_blocks",
     inter_with_uncoded,(int)(uncoded_seen>0),0,0);
    return 1;
  }
  if(total_ops<=0){
    case_fail("stream_frames_match_reference","total_ops",0,0,0,0);
    return 1;
  }

  printf("THEORA_LOOPFILTER_CASE stream_frames_match_reference PASS "
   "frames=%d flimit=%d filter_ops=%ld uncoded_frags=%ld inter_ref_frags=%ld"
   "\n",nframes,OBS.flimit,total_ops,uncoded_seen,inter_seen);
  printf("THEORA_LOOPFILTER_CASE stripe_delivery_stable PASS "
   "frames=%d luma_rows=%d\n",nframes,FRAME_H);

  /*---------------- Cleanup ----------------*/
  th_decode_free(dec);
  th_setup_free(setup);
  th_comment_clear(&tc);
  th_info_clear(&info);
  for(f=0;f<nframes;f++)free(frames[f].data);
  for(f=0;f<nheaders;f++)free(headers[f].data);
  free(framebuf);
  for(pli=0;pli<3;pli++){
    free(SC.rows[pli]);
    free(OBS.snap[pli]);
  }
  free(SC.delivered);
  free(OBS.coded);
  return 0;
}
