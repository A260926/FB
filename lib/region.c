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

  function: incremental region decoder.
  last mod: $Id$

 ********************************************************************/

#include <stdlib.h>
#include <string.h>
#include "theora/theoraregion.h"
#include "decint.h"

/*This file implements the region decoding context declared in
   theora/theoraregion.h.

  Packets are parsed exactly like the normal decoder does (modes, motion
   vectors, DC and residual tokens are fully unpacked, and the dequantized
   coefficients of every coded fragment are stored), but no whole-frame
   pixel reconstruction is ever performed and no full-frame reference
   images are held.

  Pixels are materialized lazily, one 8x8 plane block at a time, following
   the true block-level dependencies of the codec:

   - An uncoded block is a copy of the co-located block of the previous
      (already loop-filtered) frame.
   - An intra block only needs its own stored coefficients.
   - An inter block needs the (loop-filtered) reference pixels covered by
      its motion-compensated 8x8 (or 9x9, with half-pixel refinement)
      window, with picture edge extension by clamping, plus its own
      coefficients.
   - The in-loop deblocking filter of Theora is a sequential process: each
      block edge is filtered at most once, in a fixed global order, and the
      filter of one edge reads pixels written by a few neighboring,
      earlier-filtered edges. To obtain the filtered value of a block we
      compute the exact predecessor closure of its edges (see
      region_expand() for the derivation), make sure the unfiltered pixels
      of every block the closure touches are available, and replay the
      closure's edges in the exact global order on a scratch tile.

  Materialized blocks are kept in per-frame, per-plane sparse caches, so
   persistent pixel storage grows with the blocks (and necessary
   neighborhoods) that were actually requested, never with the frame size.*/


/*Motion vector offset tables, identical to the ones used by
   oc_state_get_mv_offsets() in state.c.*/
static const signed char OC_REGION_MVMAP[2][64]={
  {
          -15,-15,-14,-14,-13,-13,-12,-12,-11,-11,-10,-10, -9, -9, -8,
     -8, -7, -7, -6, -6, -5, -5, -4, -4, -3, -3, -2, -2, -1, -1,  0,
      0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,  6,  7,  7,
      8,  8,  9,  9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15
  },
  {
           -7, -7, -7, -7, -6, -6, -6, -6, -5, -5, -5, -5, -4, -4, -4,
     -4, -3, -3, -3, -3, -2, -2, -2, -2, -1, -1, -1, -1,  0,  0,  0,
      0,  0,  0,  0,  1,  1,  1,  1,  2,  2,  2,  2,  3,  3,  3,  3,
      4,  4,  4,  4,  5,  5,  5,  5,  6,  6,  6,  6,  7,  7,  7,  7
  }
};
static const signed char OC_REGION_MVMAP2[2][64]={
  {
        -1, 0,-1,  0,-1, 0,-1,  0,-1, 0,-1,  0,-1, 0,-1,
     0,-1, 0,-1,  0,-1, 0,-1,  0,-1, 0,-1,  0,-1, 0,-1,
     0, 1, 0, 1,  0, 1, 0, 1,  0, 1, 0, 1,  0, 1, 0, 1,
     0, 1, 0, 1,  0, 1, 0, 1,  0, 1, 0, 1,  0, 1, 0, 1
  },
  {
        -1,-1,-1,  0,-1,-1,-1,  0,-1,-1,-1,  0,-1,-1,-1,
     0,-1,-1,-1,  0,-1,-1,-1,  0,-1,-1,-1,  0,-1,-1,-1,
     0, 1, 1, 1,  0, 1, 1, 1,  0, 1, 1, 1,  0, 1, 1, 1,
     0, 1, 1, 1,  0, 1, 1, 1,  0, 1, 1, 1,  0, 1, 1, 1
  }
};



/*A sparse cache of materialized 8x8 blocks of one plane of one frame.
  Keys are the plane-local fragment indices; values are 64 pixels in
   decoder (bottom-up) row order.*/
typedef struct{
  ptrdiff_t     *keys;   /*fragment index + 1; 0 marks an empty slot*/
  unsigned char *pixels; /*64 bytes per slot*/
  size_t         cap;    /*number of slots; a power of two*/
  size_t         count;
}oc_block_cache;

/*One parsed frame of the current GOP.*/
typedef struct{
  int            frame_type;
  int            nqis;
  unsigned char  qis[3];
  /*Whether the in-loop filter is enabled for this frame, and its bounding
     values.*/
  int            loop_filter;
  signed char    bv[256];
  /*Full-frame compressed information (explicitly allowed to be kept).*/
  oc_fragment   *frags;
  oc_mv         *frag_mvs;
  /*Dequantized coefficients of the coded fragments, 64 per fragment, plus
     64 spare entries at the end (see oc_dec_parse_finish()).*/
  ogg_int16_t   *coeffs;
  signed char   *last_zzi;
  /*The sparse pixel caches, one per plane.*/
  oc_block_cache cache[3];
  /*Bit sets of already-filtered horizontal/vertical block edges, indexed
     by global fragment index.*/
  unsigned char *hedge;
  unsigned char *vedge;
}oc_region_frame;

/*One edge of the in-loop filter, used by the closure computation.*/
typedef struct{
  ogg_int64_t key; /*position in the global filter order*/
  int         u,v; /*block coordinates of the edge (see region_add_edge())*/
  int         isv; /*0: horizontal edge, 1: vertical edge*/
}oc_region_edge;

/*A growable list of filter edges. Each closure computation owns its own
   list, because materializing the blocks a closure touches recurses into
   the closures of other frames.*/
typedef struct{
  oc_region_edge *edges;
  size_t          count;
  size_t          cap;
}oc_region_edge_list;

struct th_region_ctx{
  th_info          info;
  /*The parse-only decoder; holds no reference frame pixel buffers.*/
  oc_dec_ctx      *dec;
  size_t           max_frames;
  oc_region_frame **frames;
  size_t           nframes;
  /*Plane geometry, cached from the decoder state.*/
  int              nhfrags[3];
  int              nvfrags[3];
  ptrdiff_t        froffset[3];
  ptrdiff_t        nfrags;
  size_t           visit_bytes;
  /*Statistics of the current GOP.*/
  size_t           stat_blocks;
  size_t           pixel_bytes;
  /*The buffer lent to the caller by th_region_out().*/
  unsigned char   *out_buf;
  size_t           out_cap;
  /*Scratch workspaces (transient; not persistent pixel storage).
    The tile is only used after the recursive block materialization of a
     closure has finished, so sharing it across nested closures is safe.
    The visit bit sets are only used while a closure is being computed, and
     closures never nest.*/
  unsigned char   *tile;
  size_t           tile_cap;
  unsigned char   *visit_h;
  unsigned char   *visit_v;
};

typedef struct th_region_ctx oc_region_ctx;



static int  region_ensure_unfiltered(oc_region_ctx *r,size_t f,int pli,
   int u,int v);
static int  region_ensure_final(oc_region_ctx *r,size_t f,int pli,
   int u,int v);



/*---- Small bit set helpers ----*/
static int region_bit_get(const unsigned char *b,ptrdiff_t i){
  return b[i>>3]>>(i&7)&1;
}
static void region_bit_set(unsigned char *b,ptrdiff_t i){
  b[i>>3]|=(unsigned char)(1<<(i&7));
}



/*---- Sparse block cache ----*/
static void region_cache_clear(oc_block_cache *c){
  free(c->keys);
  free(c->pixels);
  memset(c,0,sizeof(*c));
}

static unsigned char *region_cache_find(oc_block_cache *c,ptrdiff_t k){
  size_t i;
  if(c->cap==0)return NULL;
  i=(size_t)k&(c->cap-1);
  while(c->keys[i]!=0){
    if(c->keys[i]==k+1)return c->pixels+(i<<6);
    i=(i+1)&(c->cap-1);
  }
  return NULL;
}

static unsigned char *region_cache_insert(oc_region_ctx *r,
 oc_block_cache *c,ptrdiff_t k){
  size_t i;
  if(c->count+1>c->cap-(c->cap>>2)){
    /*Grow and rehash, keeping the load factor below 3/4.*/
    size_t         ncap;
    ptrdiff_t     *nk;
    unsigned char *np;
    ncap=c->cap!=0?c->cap<<1:16;
    nk=(ptrdiff_t *)malloc(ncap*sizeof(*nk));
    np=(unsigned char *)malloc(ncap<<6);
    if(nk!=NULL&&np!=NULL){
      memset(nk,0,ncap*sizeof(*nk));
      for(i=0;i<c->cap;i++)if(c->keys[i]!=0){
        ptrdiff_t kk;
        size_t    j;
        kk=c->keys[i]-1;
        j=(size_t)kk&(ncap-1);
        while(nk[j]!=0)j=(j+1)&(ncap-1);
        nk[j]=kk+1;
        memcpy(np+(j<<6),c->pixels+(i<<6),64);
      }
      free(c->keys);
      free(c->pixels);
      c->keys=nk;
      c->pixels=np;
      c->cap=ncap;
    }
    else{
      free(nk);
      free(np);
      if(c->count>=c->cap)return NULL;
    }
  }
  i=(size_t)k&(c->cap-1);
  while(c->keys[i]!=0){
    if(c->keys[i]==k+1)return c->pixels+(i<<6);
    i=(i+1)&(c->cap-1);
  }
  c->keys[i]=k+1;
  c->count++;
  r->pixel_bytes+=64;
  return c->pixels+(i<<6);
}



/*---- Frame management ----*/
static void region_frame_free(oc_region_frame *fr){
  int pli;
  free(fr->frags);
  free(fr->frag_mvs);
  free(fr->coeffs);
  free(fr->last_zzi);
  for(pli=0;pli<3;pli++)region_cache_clear(fr->cache+pli);
  free(fr->hedge);
  free(fr->vedge);
  memset(fr,0,sizeof(*fr));
}

static int region_frame_alloc(oc_region_ctx *r,oc_region_frame *fr){
  memset(fr,0,sizeof(*fr));
  fr->frags=(oc_fragment *)malloc((size_t)r->nfrags*sizeof(fr->frags[0]));
  fr->frag_mvs=(oc_mv *)malloc((size_t)r->nfrags*sizeof(fr->frag_mvs[0]));
  fr->coeffs=(ogg_int16_t *)malloc(((size_t)r->nfrags*64+64)*
   sizeof(fr->coeffs[0]));
  fr->last_zzi=(signed char *)malloc((size_t)r->nfrags*
   sizeof(fr->last_zzi[0]));
  fr->hedge=(unsigned char *)calloc(1,r->visit_bytes);
  fr->vedge=(unsigned char *)calloc(1,r->visit_bytes);
  if(fr->frags==NULL||fr->frag_mvs==NULL||fr->coeffs==NULL||
   fr->last_zzi==NULL||fr->hedge==NULL||fr->vedge==NULL){
    region_frame_free(fr);
    return -1;
  }
  return 0;
}

/*Parses one packet into a freshly allocated frame structure.*/
static int region_frame_parse(oc_region_ctx *r,oc_region_frame *fr,
 const ogg_packet *op){
  oc_theora_state *st;
  int              ret;
  int              flimit;
  ret=oc_dec_parse_packet(r->dec,op);
  if(ret<0)return TH_EINVAL;
  st=&r->dec->state;
  fr->frame_type=st->frame_type;
  fr->nqis=st->nqis;
  fr->qis[0]=st->qis[0];
  fr->qis[1]=st->qis[1];
  fr->qis[2]=st->qis[2];
  if(st->ntotal_coded_fragis>0){
    oc_dec_parse_finish(r->dec,fr->coeffs,fr->last_zzi);
  }
  memcpy(fr->frags,st->frags,(size_t)r->nfrags*sizeof(fr->frags[0]));
  memcpy(fr->frag_mvs,st->frag_mvs,(size_t)r->nfrags*sizeof(fr->frag_mvs[0]));
  flimit=st->loop_filter_limits[fr->qis[0]];
  fr->loop_filter=flimit!=0;
  if(flimit!=0)oc_loop_filter_init_c(fr->bv,flimit);
  return 0;
}

/*Drops the whole current GOP and resets the statistics.*/
static void region_gop_clear(oc_region_ctx *r){
  size_t f;
  for(f=0;f<r->nframes;f++){
    region_frame_free(r->frames[f]);
    free(r->frames[f]);
    r->frames[f]=NULL;
  }
  r->nframes=0;
  r->stat_blocks=0;
  r->pixel_bytes=0;
}



/*---- Pixel reconstruction ----*/

/*Computes the 64 residuals of a coded fragment (IDCT, or the DC-only
   shortcut), exactly like oc_state_frag_recon_c() does before filling in
   the target buffer.*/
static void region_residue(oc_region_ctx *r,oc_region_frame *fr,int pli,
 ptrdiff_t fragi,ogg_int16_t *res){
  const ogg_int16_t *co;
  ogg_uint16_t       dc_quant;
  int                last_zzi;
  int                qti;
  int                i;
  co=fr->coeffs+fragi*64;
  last_zzi=fr->last_zzi[fragi];
  qti=fr->frags[fragi].mb_mode!=OC_MODE_INTRA;
  dc_quant=r->dec->state.dequant_tables[fr->qis[0]][pli][qti][0];
  if(last_zzi<2){
    ogg_int16_t p;
    /*We round this dequant product (and not any of the others) because
       there's no iDCT rounding.*/
    p=(ogg_int16_t)(co[0]*(ogg_int32_t)dc_quant+15>>5);
    for(i=0;i<64;i++)res[i]=p;
  }
  else{
    OC_ALIGN16(ogg_int16_t tmp[64]);
    memcpy(tmp,co,sizeof(tmp));
    tmp[0]=(ogg_int16_t)(tmp[0]*(int)dc_quant);
    oc_idct8x8_c(res,tmp,last_zzi);
  }
}

/*Fetches an 8x8 window from the (loop-filtered) reference frame f at
   pixel position (X,Y), extending the picture edges by clamping, exactly
   like the padded reference buffers of the normal decoder do.
  All blocks the window covers must have been materialized already.*/
static void region_gather(oc_region_ctx *r,size_t f,int pli,int X,int Y,
 unsigned char *dst){
  oc_region_frame *fr;
  int              nh;
  int              W;
  int              H;
  int              i;
  int              j;
  fr=r->frames[f];
  nh=r->nhfrags[pli];
  W=nh<<3;
  H=r->nvfrags[pli]<<3;
  for(i=0;i<8;i++){
    int yy;
    yy=Y+i;
    yy=yy<0?0:yy>=H?H-1:yy;
    for(j=0;j<8;j++){
      int               xx;
      const unsigned char *blk;
      xx=X+j;
      xx=xx<0?0:xx>=W?W-1:xx;
      blk=region_cache_find(fr->cache+pli,(ptrdiff_t)(yy>>3)*nh+(xx>>3));
      dst[i*8+j]=blk[(yy&7)*8+(xx&7)];
    }
  }
}

/*Materializes the unfiltered pixels of one block, reconstructing or
   copying them as appropriate, and counts the work in the statistics.*/
static int region_ensure_unfiltered(oc_region_ctx *r,size_t f,int pli,
 int u,int v){
  oc_region_frame *fr;
  ptrdiff_t        key;
  ptrdiff_t        fragi;
  oc_fragment     *frag;
  unsigned char    dst[64];
  unsigned char   *slot;
  int              nh;
  int              ret;
  fr=r->frames[f];
  nh=r->nhfrags[pli];
  key=(ptrdiff_t)v*nh+u;
  if(region_cache_find(fr->cache+pli,key)!=NULL)return 0;
  fragi=r->froffset[pli]+key;
  frag=fr->frags+fragi;
  if(!frag->coded){
    /*Uncoded block: copy the co-located block of the loop-filtered
       previous frame.*/
    const unsigned char *src;
    if(f==0)return TH_EFAULT;
    ret=region_ensure_final(r,f-1,pli,u,v);
    if(ret!=0)return ret;
    src=region_cache_find(r->frames[f-1]->cache+pli,key);
    if(src==NULL)return TH_EFAULT;
    memcpy(dst,src,64);
  }
  else if(frag->refi==OC_FRAME_SELF){
    /*Intra block: only its own coefficients are needed.*/
    OC_ALIGN16(ogg_int16_t res[64]);
    region_residue(r,fr,pli,fragi,res);
    oc_frag_recon_intra_c(dst,8,res);
  }
  else{
    /*Inter block: motion-compensated prediction plus residual.*/
    size_t           rf;
    oc_mv            mv;
    int              dx;
    int              dy;
    int              qpx;
    int              mx;
    int              mx2;
    int              my;
    int              my2;
    int              two;
    int              sx0;
    int              sy0;
    int              sx1;
    int              sy1;
    int              W;
    int              H;
    int              bx;
    int              by;
    unsigned char    s0[64];
    unsigned char    s1[64];
    OC_ALIGN16(ogg_int16_t res[64]);
    rf=frag->refi==OC_FRAME_GOLD?0:f-1;
    mv=fr->frag_mvs[fragi];
    dx=OC_MV_X(mv);
    dy=OC_MV_Y(mv);
    qpx=pli!=0;
    mx=OC_REGION_MVMAP[qpx][dx+31];
    mx2=OC_REGION_MVMAP2[qpx][dx+31];
    my=OC_REGION_MVMAP[qpx][dy+31];
    my2=OC_REGION_MVMAP2[qpx][dy+31];
    two=mx2||my2;
    /*The union of the (at most) two 8x8 source windows.*/
    sx0=u*8+OC_MINI(mx,mx+mx2);
    sx1=u*8+OC_MAXI(mx,mx+mx2)+7;
    sy0=v*8+OC_MINI(my,my+my2);
    sy1=v*8+OC_MAXI(my,my+my2)+7;
    W=nh<<3;
    H=r->nvfrags[pli]<<3;
    sx0=OC_MAXI(sx0,0);
    sy0=OC_MAXI(sy0,0);
    sx1=OC_MINI(sx1,W-1);
    sy1=OC_MINI(sy1,H-1);
    /*Materialize the reference blocks the window covers (edge extension
       reads the clamped edge blocks).*/
    for(by=sy0>>3;by<=(sy1>>3);by++){
      for(bx=sx0>>3;bx<=(sx1>>3);bx++){
        ret=region_ensure_final(r,rf,pli,bx,by);
        if(ret!=0)return ret;
      }
    }
    region_gather(r,rf,pli,u*8+mx,v*8+my,s0);
    if(two)region_gather(r,rf,pli,u*8+mx+mx2,v*8+my+my2,s1);
    region_residue(r,fr,pli,fragi,res);
    if(two)oc_frag_recon_inter2_c(dst,s0,s1,8,res);
    else oc_frag_recon_inter_c(dst,s0,8,res);
  }
  slot=region_cache_insert(r,fr->cache+pli,key);
  if(slot==NULL)return TH_EIMPL;
  memcpy(slot,dst,64);
  r->stat_blocks++;
  return 0;
}



/*---- In-loop deblocking filter ----*/

/*These two are exact copies of the (static) loop_filter_h()/loop_filter_v()
   of state.c; _bv points to the middle of the bounding values array.*/
static void region_loop_filter_h(unsigned char *_pix,int _ystride,
 const signed char *_bv){
  int y;
  _pix-=2;
  for(y=0;y<8;y++){
    int f;
    f=_pix[0]-_pix[3]+3*(_pix[2]-_pix[1]);
    f=*(_bv+(f+4>>3));
    _pix[1]=OC_CLAMP255(_pix[1]+f);
    _pix[2]=OC_CLAMP255(_pix[2]-f);
    _pix+=_ystride;
  }
}

static void region_loop_filter_v(unsigned char *_pix,int _ystride,
 const signed char *_bv){
  int x;
  _pix-=_ystride*2;
  for(x=0;x<8;x++){
    int f;
    f=_pix[x]-_pix[_ystride*3+x]+3*(_pix[_ystride*2+x]-_pix[_ystride+x]);
    f=*(_bv+(f+4>>3));
    _pix[_ystride+x]=OC_CLAMP255(_pix[_ystride+x]+f);
    _pix[_ystride*2+x]=OC_CLAMP255(_pix[_ystride*2+x]-f);
  }
}

/*Whether the fragment at (u,v) of the given plane is coded.
  Out-of-grid positions count as uncoded.*/
static int region_coded(oc_region_frame *fr,ptrdiff_t froff,int nh,int nv,
 int u,int v){
  if(u<0||v<0||u>=nh||v>=nv)return 0;
  return fr->frags[froff+(ptrdiff_t)v*nh+u].coded;
}

/*Adds one edge to the closure if it exists, has not been filtered yet, and
   is not already part of the closure.
  A horizontal edge (u,v) sits on the column boundary to the left of block
   (u,v); a vertical edge (u,v) sits on the row boundary above block (u,v).
  Return: 0 on success, -1 on allocation failure.*/
static int region_add_edge(oc_region_ctx *r,oc_region_frame *fr,int pli,
 oc_region_edge_list *list,int u,int v,int isv){
  ptrdiff_t        froff;
  ptrdiff_t        fragi;
  unsigned char   *visit;
  oc_region_edge  *ne;
  int              nh;
  int              nv;
  int              ou;
  int              ov;
  int              slot;
  if(!fr->loop_filter)return 0;
  nh=r->nhfrags[pli];
  nv=r->nvfrags[pli];
  froff=r->froffset[pli];
  if(!isv){
    if(u<1||u>=nh||v<0||v>=nv)return 0;
    if(!region_coded(fr,froff,nh,nv,u,v)&&
     !region_coded(fr,froff,nh,nv,u-1,v))return 0;
  }
  else{
    if(u<0||u>=nh||v<1||v>=nv)return 0;
    if(!region_coded(fr,froff,nh,nv,u,v)&&
     !region_coded(fr,froff,nh,nv,u,v-1))return 0;
  }
  fragi=froff+(ptrdiff_t)v*nh+u;
  visit=isv?r->visit_v:r->visit_h;
  if(region_bit_get(visit,fragi))return 0;
  if(region_bit_get(isv?fr->vedge:fr->hedge,fragi))return 0;
  region_bit_set(visit,fragi);
  if(list->count>=list->cap){
    size_t ncap;
    ncap=list->cap!=0?list->cap<<1:64;
    ne=(oc_region_edge *)realloc(list->edges,ncap*sizeof(*ne));
    if(ne==NULL)return -1;
    list->edges=ne;
    list->cap=ncap;
  }
  /*Compute the position of this edge in the global filter order: the
     filter processes fragments in raster order, filtering the left edge,
     the top edge, the right edge (if the right neighbor is uncoded) and
     the bottom edge (if the bottom neighbor is uncoded) of each coded
     fragment, in that order.*/
  if(!isv){
    if(region_coded(fr,froff,nh,nv,u,v)){ou=u;ov=v;slot=0;}
    else{ou=u-1;ov=v;slot=2;}
  }
  else{
    if(region_coded(fr,froff,nh,nv,u,v)){ou=u;ov=v;slot=1;}
    else{ou=u;ov=v-1;slot=3;}
  }
  ne=list->edges+list->count++;
  ne->key=(((ogg_int64_t)ov*nh+ou)<<2)|slot;
  ne->u=u;
  ne->v=v;
  ne->isv=isv;
  return 0;
}

/*Adds the predecessor edges of one edge to the closure.
  An edge's filter reads pixels up to two positions across the edge, which
   are written by (at most) the two edges of the opposite orientation on
   each side; the exact conditions below are derived from the global
   filter order (see region_add_edge()): a predecessor is an edge that
   writes pixels this edge reads _and_ runs earlier in the global order.*/
static int region_expand(oc_region_ctx *r,oc_region_frame *fr,int pli,
 oc_region_edge_list *list,int u,int v,int isv){
  ptrdiff_t froff;
  int       nh;
  int       nv;
  int       ret;
  nh=r->nhfrags[pli];
  nv=r->nvfrags[pli];
  froff=r->froffset[pli];
  if(!isv){
    /*Predecessors of the horizontal edge (u,v).*/
    ret=region_add_edge(r,fr,pli,list,u-1,v,1);
    if(ret<0)return ret;
    if(!region_coded(fr,froff,nh,nv,u,v)&&
     region_coded(fr,froff,nh,nv,u,v-1)){
      ret=region_add_edge(r,fr,pli,list,u,v,1);
      if(ret<0)return ret;
    }
    if(region_coded(fr,froff,nh,nv,u,v)&&
     region_coded(fr,froff,nh,nv,u-1,v)&&
     !region_coded(fr,froff,nh,nv,u-1,v+1)){
      ret=region_add_edge(r,fr,pli,list,u-1,v+1,1);
      if(ret<0)return ret;
    }
  }
  else{
    /*Predecessors of the vertical edge (u,v).*/
    ret=region_add_edge(r,fr,pli,list,u,v-1,0);
    if(ret<0)return ret;
    if(region_coded(fr,froff,nh,nv,u,v)){
      ret=region_add_edge(r,fr,pli,list,u,v,0);
      if(ret<0)return ret;
    }
    if(region_coded(fr,froff,nh,nv,u,v)||
     !region_coded(fr,froff,nh,nv,u+1,v-1)){
      ret=region_add_edge(r,fr,pli,list,u+1,v-1,0);
      if(ret<0)return ret;
    }
  }
  return 0;
}

/*Computes the predecessor closure of the not-yet-filtered edges of block
   (tu,tv); the collected edges are owned by the caller.
  Return: 0 on success, -1 on allocation failure.*/
static int region_closure(oc_region_ctx *r,oc_region_frame *fr,int pli,
 int tu,int tv,oc_region_edge_list *list){
  size_t i;
  int    ret;
  memset(r->visit_h,0,r->visit_bytes);
  memset(r->visit_v,0,r->visit_bytes);
  list->count=0;
  ret=region_add_edge(r,fr,pli,list,tu,tv,0);
  if(ret<0)return ret;
  ret=region_add_edge(r,fr,pli,list,tu+1,tv,0);
  if(ret<0)return ret;
  ret=region_add_edge(r,fr,pli,list,tu,tv,1);
  if(ret<0)return ret;
  ret=region_add_edge(r,fr,pli,list,tu,tv+1,1);
  if(ret<0)return ret;
  for(i=0;i<list->count;i++){
    ret=region_expand(r,fr,pli,list,
     list->edges[i].u,list->edges[i].v,list->edges[i].isv);
    if(ret<0)return ret;
  }
  return 0;
}

static int region_edge_cmp(const void *a,const void *b){
  ogg_int64_t ka;
  ogg_int64_t kb;
  ka=((const oc_region_edge *)a)->key;
  kb=((const oc_region_edge *)b)->key;
  return ka<kb?-1:ka>kb?1:0;
}

/*Materializes the unfiltered pixels of every block the closure touches,
   replays the closure's edges in the exact global order on a scratch tile,
   and writes the results back into the block cache.*/
static int region_apply(oc_region_ctx *r,size_t f,int pli,
 oc_region_edge_list *list){
  oc_region_frame *fr;
  unsigned char   *visit_b;
  size_t           n;
  size_t           i;
  ptrdiff_t        froff;
  int              nh;
  int              nv;
  int              u0;
  int              u1;
  int              v0;
  int              v1;
  int              tw;
  int              th;
  int              u;
  int              v;
  int              row;
  int              ret;
  fr=r->frames[f];
  nh=r->nhfrags[pli];
  nv=r->nvfrags[pli];
  froff=r->froffset[pli];
  n=list->count;
  visit_b=(unsigned char *)calloc(1,r->visit_bytes);
  if(visit_b==NULL)return TH_EIMPL;
  /*Collect the touched blocks and make sure their current pixels are
     available. This recurses into the closures of other frames, which is
     why the edge list and this block set are owned by this call.*/
  u0=nh;
  u1=-1;
  v0=nv;
  v1=-1;
  for(i=0;i<n;i++){
    oc_region_edge *e;
    int             bu[2];
    int             bv[2];
    int             k;
    e=list->edges+i;
    if(!e->isv){
      bu[0]=e->u-1;
      bu[1]=e->u;
      bv[0]=bv[1]=e->v;
    }
    else{
      bu[0]=bu[1]=e->u;
      bv[0]=e->v-1;
      bv[1]=e->v;
    }
    for(k=0;k<2;k++){
      ptrdiff_t fragi;
      u=bu[k];
      v=bv[k];
      if(u<u0)u0=u;
      if(u>u1)u1=u;
      if(v<v0)v0=v;
      if(v>v1)v1=v;
      fragi=froff+(ptrdiff_t)v*nh+u;
      if(!region_bit_get(visit_b,fragi)){
        region_bit_set(visit_b,fragi);
        ret=region_ensure_unfiltered(r,f,pli,u,v);
        if(ret!=0){
          free(visit_b);
          return ret;
        }
      }
    }
  }
  /*Assemble the scratch tile.*/
  tw=u1-u0+1<<3;
  th=v1-v0+1<<3;
  if((size_t)tw*th>r->tile_cap){
    unsigned char *nt;
    nt=(unsigned char *)realloc(r->tile,(size_t)tw*th);
    if(nt==NULL){
      free(visit_b);
      return TH_EIMPL;
    }
    r->tile=nt;
    r->tile_cap=(size_t)tw*th;
  }
  memset(r->tile,0,(size_t)tw*th);
  for(v=v0;v<=v1;v++)for(u=u0;u<=u1;u++){
    ptrdiff_t fragi;
    fragi=froff+(ptrdiff_t)v*nh+u;
    if(region_bit_get(visit_b,fragi)){
      const unsigned char *src;
      src=region_cache_find(fr->cache+pli,(ptrdiff_t)v*nh+u);
      for(row=0;row<8;row++){
        memcpy(r->tile+((v-v0)*8+row)*tw+(u-u0)*8,src+row*8,8);
      }
    }
  }
  /*Replay the edges in the exact global filter order.*/
  qsort(list->edges,n,sizeof(list->edges[0]),region_edge_cmp);
  for(i=0;i<n;i++){
    oc_region_edge *e;
    unsigned char  *pix;
    ptrdiff_t       fragi;
    e=list->edges+i;
    pix=r->tile+((e->v-v0)*8)*tw+(e->u-u0)*8;
    fragi=froff+(ptrdiff_t)e->v*nh+e->u;
    if(!e->isv){
      region_loop_filter_h(pix,tw,fr->bv+127);
      region_bit_set(fr->hedge,fragi);
    }
    else{
      region_loop_filter_v(pix,tw,fr->bv+127);
      region_bit_set(fr->vedge,fragi);
    }
  }
  /*Write the touched blocks back into the cache.*/
  for(v=v0;v<=v1;v++)for(u=u0;u<=u1;u++){
    ptrdiff_t fragi;
    fragi=froff+(ptrdiff_t)v*nh+u;
    if(region_bit_get(visit_b,fragi)){
      unsigned char *dst;
      dst=region_cache_find(fr->cache+pli,(ptrdiff_t)v*nh+u);
      for(row=0;row<8;row++){
        memcpy(dst+row*8,r->tile+((v-v0)*8+row)*tw+(u-u0)*8,8);
      }
    }
  }
  free(visit_b);
  return 0;
}

/*Whether every edge of the block that exists has already been filtered.*/
static int region_block_final(oc_region_ctx *r,oc_region_frame *fr,int pli,
 int u,int v){
  ptrdiff_t froff;
  int       nh;
  int       nv;
  if(!fr->loop_filter)return 1;
  nh=r->nhfrags[pli];
  nv=r->nvfrags[pli];
  froff=r->froffset[pli];
  if(u>=1&&(region_coded(fr,froff,nh,nv,u,v)||
   region_coded(fr,froff,nh,nv,u-1,v))&&
   !region_bit_get(fr->hedge,froff+(ptrdiff_t)v*nh+u))return 0;
  if(u+1<nh&&(region_coded(fr,froff,nh,nv,u+1,v)||
   region_coded(fr,froff,nh,nv,u,v))&&
   !region_bit_get(fr->hedge,froff+(ptrdiff_t)v*nh+u+1))return 0;
  if(v>=1&&(region_coded(fr,froff,nh,nv,u,v)||
   region_coded(fr,froff,nh,nv,u,v-1))&&
   !region_bit_get(fr->vedge,froff+(ptrdiff_t)v*nh+u))return 0;
  if(v+1<nv&&(region_coded(fr,froff,nh,nv,u,v+1)||
   region_coded(fr,froff,nh,nv,u,v))&&
   !region_bit_get(fr->vedge,froff+(ptrdiff_t)(v+1)*nh+u))return 0;
  return 1;
}

/*Materializes the final (loop-filtered) pixels of one block.*/
static int region_ensure_final(oc_region_ctx *r,size_t f,int pli,
 int u,int v){
  oc_region_frame *fr;
  int              ret;
  fr=r->frames[f];
  if(!region_block_final(r,fr,pli,u,v)){
    oc_region_edge_list list;
    list.edges=NULL;
    list.count=list.cap=0;
    ret=region_closure(r,fr,pli,u,v,&list);
    if(ret<0){
      free(list.edges);
      return TH_EIMPL;
    }
    if(list.count>0){
      ret=region_apply(r,f,pli,&list);
      if(ret!=0){
        free(list.edges);
        return ret;
      }
    }
    free(list.edges);
  }
  return region_ensure_unfiltered(r,f,pli,u,v);
}



/*---- Public API ----*/

th_region_ctx *th_region_alloc(const th_info *_info,
 const th_setup_info *_setup,size_t _max_frames){
  oc_region_ctx *r;
  int            pli;
  if(_info==NULL||_setup==NULL||_max_frames<1)return NULL;
  /*Only ordinary 8-bit 4:2:0 content with the displayable region covering
     the whole coded frame is supported.*/
  if(_info->pixel_fmt!=TH_PF_420)return NULL;
  if(_info->pic_x!=0||_info->pic_y!=0||
   _info->pic_width!=_info->frame_width||
   _info->pic_height!=_info->frame_height)return NULL;
  r=(oc_region_ctx *)calloc(1,sizeof(*r));
  if(r==NULL)return NULL;
  r->dec=oc_dec_parse_alloc(_info,_setup);
  if(r->dec==NULL){
    free(r);
    return NULL;
  }
  r->info=*_info;
  r->max_frames=_max_frames;
  for(pli=0;pli<3;pli++){
    r->nhfrags[pli]=r->dec->state.fplanes[pli].nhfrags;
    r->nvfrags[pli]=r->dec->state.fplanes[pli].nvfrags;
    r->froffset[pli]=r->dec->state.fplanes[pli].froffset;
  }
  r->nfrags=r->dec->state.nfrags;
  r->visit_bytes=(size_t)(r->nfrags+7>>3);
  r->frames=(oc_region_frame **)calloc(_max_frames,sizeof(*r->frames));
  r->visit_h=(unsigned char *)calloc(1,r->visit_bytes);
  r->visit_v=(unsigned char *)calloc(1,r->visit_bytes);
  if(r->frames==NULL||r->visit_h==NULL||r->visit_v==NULL){
    th_region_free(r);
    return NULL;
  }
  return r;
}

void th_region_free(th_region_ctx *_ctx){
  if(_ctx!=NULL){
    region_gop_clear(_ctx);
    free(_ctx->frames);
    th_decode_free(_ctx->dec);
    free(_ctx->out_buf);
    free(_ctx->tile);
    free(_ctx->visit_h);
    free(_ctx->visit_v);
    free(_ctx);
  }
}

int th_region_packetin(th_region_ctx *_ctx,const ogg_packet *_op){
  oc_region_frame *fr;
  int              key;
  int              ret;
  if(_ctx==NULL||_op==NULL)return TH_EINVAL;
  key=th_packet_iskeyframe((ogg_packet *)_op);
  if(key<0)return TH_EINVAL;
  if(!key){
    /*The first packet must be a key frame.*/
    if(_ctx->nframes==0)return TH_EINVAL;
    /*Reject non-key packets once the GOP capacity is reached, leaving the
       existing state untouched.*/
    if(_ctx->nframes>=_ctx->max_frames)return TH_EIMPL;
  }
  fr=(oc_region_frame *)malloc(sizeof(*fr));
  if(fr==NULL)return TH_EIMPL;
  if(region_frame_alloc(_ctx,fr)<0){
    free(fr);
    return TH_EIMPL;
  }
  ret=region_frame_parse(_ctx,fr,_op);
  if(ret<0){
    region_frame_free(fr);
    free(fr);
    return ret;
  }
  if(key){
    /*A successfully received key frame atomically replaces the old GOP and
       restarts frame numbering at 0.*/
    region_gop_clear(_ctx);
    _ctx->frames[0]=fr;
    _ctx->nframes=1;
    /*The lent output buffer is invalidated by a committed key frame.*/
    free(_ctx->out_buf);
    _ctx->out_buf=NULL;
    _ctx->out_cap=0;
  }
  else{
    _ctx->frames[_ctx->nframes++]=fr;
  }
  return 0;
}

int th_region_out(th_region_ctx *_ctx,size_t _frame,
 int _x,int _y,int _width,int _height,th_ycbcr_buffer _out){
  oc_region_frame *fr;
  size_t           ys;
  size_t           cs;
  int              bx0;
  int              bx1;
  int              by0;
  int              by1;
  int              bx;
  int              by;
  int              pli;
  int              ret;
  if(_ctx==NULL||_out==NULL)return TH_EINVAL;
  if(_frame>=_ctx->nframes)return TH_EINVAL;
  if(_x<0||_y<0||_width<=0||_height<=0||
   (_x&1)||(_y&1)||(_width&1)||(_height&1))return TH_EINVAL;
  if(_x+_width>_ctx->info.pic_width||
   _y+_height>_ctx->info.pic_height)return TH_EINVAL;
  /*Materialize every block the rectangle covers, in all three planes.
     Display block row b corresponds to internal (bitstream) block row
     nvfrags-1-b, and display row y to local row 7-(y&7).*/
  for(pli=0;pli<3;pli++){
    int nvc;
    int sx;
    int sy;
    nvc=_ctx->nvfrags[pli];
    sx=pli==0?_x:_x>>1;
    sy=pli==0?_y:_y>>1;
    bx0=sx>>3;
    by0=sy>>3;
    bx1=sx+(pli==0?_width:_width>>1)-1>>3;
    by1=sy+(pli==0?_height:_height>>1)-1>>3;
    for(by=by0;by<=by1;by++)for(bx=bx0;bx<=bx1;bx++){
      ret=region_ensure_final(_ctx,_frame,pli,bx,nvc-1-by);
      if(ret!=0)return ret;
    }
  }
  /*Set up the lent output buffer.*/
  ys=(size_t)_width*_height;
  cs=(size_t)(_width>>1)*(_height>>1);
  if(ys+2*cs>_ctx->out_cap){
    unsigned char *nb;
    nb=(unsigned char *)malloc(ys+2*cs);
    if(nb==NULL)return TH_EIMPL;
    free(_ctx->out_buf);
    _ctx->out_buf=nb;
    _ctx->out_cap=ys+2*cs;
  }
  _out[0].width=_width;
  _out[0].height=_height;
  _out[0].stride=_width;
  _out[0].data=_ctx->out_buf;
  _out[1].width=_width>>1;
  _out[1].height=_height>>1;
  _out[1].stride=_width>>1;
  _out[1].data=_ctx->out_buf+ys;
  _out[2].width=_width>>1;
  _out[2].height=_height>>1;
  _out[2].stride=_width>>1;
  _out[2].data=_ctx->out_buf+ys+cs;
  /*Copy the pixels out of the block caches.*/
  fr=_ctx->frames[_frame];
  for(pli=0;pli<3;pli++){
    int              nh;
    int              nvc;
    int              cw;
    int              ch;
    int              cx0;
    int              cy0;
    int              xx;
    int              yy;
    nh=_ctx->nhfrags[pli];
    nvc=_ctx->nvfrags[pli];
    cw=pli==0?_width:_width>>1;
    ch=pli==0?_height:_height>>1;
    cx0=pli==0?_x:_x>>1;
    cy0=pli==0?_y:_y>>1;
    for(yy=0;yy<ch;yy++){
      int                Y;
      int                iby;
      int                lr;
      unsigned char     *orow;
      Y=cy0+yy;
      iby=nvc-1-(Y>>3);
      lr=7-(Y&7);
      orow=_out[pli].data+(ptrdiff_t)yy*_out[pli].stride;
      xx=0;
      while(xx<cw){
        int                X;
        int                ibx;
        int                lc;
        int                run;
        const unsigned char *blk;
        X=cx0+xx;
        ibx=X>>3;
        lc=X&7;
        run=OC_MINI(8-lc,cw-xx);
        blk=region_cache_find(fr->cache+pli,(ptrdiff_t)iby*nh+ibx);
        memcpy(orow+xx,blk+lr*8+lc,(size_t)run);
        xx+=run;
      }
    }
  }
  return 0;
}

int th_region_stats(th_region_ctx *_ctx,
 size_t *_blocks,size_t *_pixel_bytes){
  if(_ctx==NULL)return TH_EINVAL;
  if(_blocks!=NULL)*_blocks=_ctx->stat_blocks;
  if(_pixel_bytes!=NULL)*_pixel_bytes=_ctx->pixel_bytes;
  return 0;
}
