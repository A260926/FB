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

  function: independent reference model of the Theora loop filter, used
            only by the regression tests in tests/loopfilter_reference.c
            and tests/loopfilter_stream.c.

  This is a from-scratch re-implementation of the filtering convention
  described in the Theora specification (section 7.10) and the pipeline
  behaviour of lib/decode.c.  It deliberately shares no code with the
  production implementation in lib/state.c: it builds an explicit,
  ordered list of boundary operations from the coded-fragment map and
  then interprets that list with its own arithmetic for the filter
  response (signed rounding, limit folding and output saturation).  The
  production bounding-value table, boundary traversal and pixel filter
  helpers are not called from here.

 ********************************************************************/
#if !defined(_loopfilter_ref_H)
#define _loopfilter_ref_H

#include <stdlib.h>
#include <string.h>

/*A single boundary filter operation.
  The boundary lies between the fragment at (fx,fy) and its left
   (orient==0) or upper (orient==1) neighbour, i.e. (fx,fy) is the
   fragment immediately to the right of or below the boundary.*/
typedef struct{
  int orient;
  int fx;
  int fy;
}lf_ref_op;

/*Saturate to an unsigned byte.*/
static int lf_ref_clamp255(int _v){
  return _v<0?0:_v>255?255:_v;
}

/*Compute floor((F+4)/8) with exact arithmetic-shift (round towards
   negative infinity) semantics, without relying on the behaviour of the
   shift operator on negative operands.*/
static int lf_ref_round(int _F){
  int v=_F+4;
  return v>=0?v>>3:-((7-v)>>3);
}

/*The full loop filter response for filter value _F and limit _L:
   the rounded value is used unchanged while it is within +/-_L, folded
   back towards zero while within +/-2*_L, and forced to zero beyond
   that.*/
static int lf_ref_response(int _F,int _L){
  int R=lf_ref_round(_F);
  if(R>=2*_L||R<=-2*_L)return 0;
  if(R>_L)return 2*_L-R;
  if(R<-_L)return -2*_L-R;
  return R;
}

/*Build the ordered list of boundary operations for one plane from the
   coded-fragment map.
  Fragments are visited in raster order; for each coded fragment its
   left edge, top edge, right edge (only when the right neighbour exists
   and is not coded) and bottom edge (only when the fragment below exists
   and is not coded) are emitted, in that order.  A boundary shared by
   two coded fragments is therefore emitted exactly once, a boundary
   with a coded fragment on only one side is still emitted, and
   boundaries along the outer edge of the fragment grid are never
   emitted.
  _coded: nh*nv map of coded flags in raster order.
  _ops:   storage for at most 4*nh*nv operations.
  Returns the number of operations emitted.*/
static int lf_ref_build_ops(const unsigned char *_coded,int _nh,int _nv,
 lf_ref_op *_ops){
  int n=0;
  int fx,fy;
  for(fy=0;fy<_nv;fy++){
    for(fx=0;fx<_nh;fx++){
      if(!_coded[fy*_nh+fx])continue;
      if(fx>0){
        _ops[n].orient=0;_ops[n].fx=fx;_ops[n].fy=fy;n++;
      }
      if(fy>0){
        _ops[n].orient=1;_ops[n].fx=fx;_ops[n].fy=fy;n++;
      }
      if(fx+1<_nh&&!_coded[fy*_nh+fx+1]){
        _ops[n].orient=0;_ops[n].fx=fx+1;_ops[n].fy=fy;n++;
      }
      if(fy+1<_nv&&!_coded[(fy+1)*_nh+fx]){
        _ops[n].orient=1;_ops[n].fx=fx;_ops[n].fy=fy+1;n++;
      }
    }
  }
  return n;
}

/*Filter the four pixels straddling a vertical boundary in one row:
   p[-2],p[-1] on the left and p[0],p[1] on the right.*/
static void lf_ref_filter4(unsigned char *_p,int _step,int _L){
  int F=_p[-2*_step]-_p[_step]+3*(_p[0]-_p[-_step]);
  int R=lf_ref_response(F,_L);
  _p[-_step]=(unsigned char)lf_ref_clamp255(_p[-_step]+R);
  _p[0]=(unsigned char)lf_ref_clamp255(_p[0]-R);
}

/*Execute a list of boundary operations on an image.
  _img points at pixel (0,0) of the fragment grid; _stride is the signed
   distance between consecutive rows (negative for bottom-up buffers).*/
static void lf_ref_exec_ops(unsigned char *_img,int _stride,
 const lf_ref_op *_ops,int _nops,int _L){
  int i;
  for(i=0;i<_nops;i++){
    if(_ops[i].orient==0){
      int r;
      unsigned char *p=_img+(_ops[i].fy*8)*(ptrdiff_t)_stride+_ops[i].fx*8;
      for(r=0;r<8;r++){
        lf_ref_filter4(p,1,_L);
        p+=_stride;
      }
    }
    else{
      int c;
      unsigned char *p=_img+(_ops[i].fy*8)*(ptrdiff_t)_stride+_ops[i].fx*8;
      for(c=0;c<8;c++,p++)lf_ref_filter4(p,_stride,_L);
    }
  }
}

/*Filter one complete plane; returns the number of boundary operations
   that were applied.*/
static int lf_ref_filter_plane(unsigned char *_img,int _stride,
 const unsigned char *_coded,int _nh,int _nv,int _L){
  lf_ref_op *ops;
  int nops;
  ops=(lf_ref_op *)malloc(sizeof(*ops)*4*(size_t)_nh*_nv+sizeof(*ops));
  if(ops==NULL)return -1;
  nops=lf_ref_build_ops(_coded,_nh,_nv,ops);
  lf_ref_exec_ops(_img,_stride,ops,nops,_L);
  free(ops);
  return nops;
}

#endif
