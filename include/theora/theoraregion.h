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

  function: incremental region decoding interface
  last mod: $Id$

 ********************************************************************/
#if !defined(_O_THEORA_THEORAREGION_H_)
# define _O_THEORA_THEORAREGION_H_

# include <stddef.h>
# include <theora/codec.h>
# include <theora/theoradec.h>

# if defined(__cplusplus)
extern "C" {
# endif

/*Opaque region decoding context.
  A region decoding context parses complete frames exactly like the normal
   decoder, but it does not reconstruct whole frames up front.
  Instead it materializes the 8x8 blocks that are actually needed to serve
   th_region_out() requests, following the true block-level dependencies of
   motion compensation, picture edge extension and the in-loop deblocking
   filter, and caches them for the rest of the current GOP.*/
typedef struct th_region_ctx th_region_ctx;

/*Allocates a region decoding context.
  Only ordinary 8-bit 4:2:0 content where the displayable region covers the
   whole coded frame (pic_x==pic_y==0, pic_width==frame_width and
   pic_height==frame_height) is supported; any other configuration is
   rejected. Out-of-loop post-processing is never applied, while the
   bitstream's own in-loop filter is preserved.
  _info:       The stream information, as filled in by th_decode_headerin().
  _setup:      The setup information, as filled in by th_decode_headerin().
               It is copied internally and may be released with
                th_setup_free() as soon as this call returns.
  _max_frames: The maximum number of frames held in the current GOP.
               Must be at least 1.
  Return: A new context, or NULL on failure (including an unsupported
   configuration).*/
extern th_region_ctx *th_region_alloc(const th_info *_info,
 const th_setup_info *_setup,size_t _max_frames);

/*Submits one data packet to the context.
  The packet is only borrowed for the duration of this call.
  Every successfully submitted packet occupies one frame number in the
   current GOP, including empty (duplicate frame) packets.
  The first packet of a context must be a key frame.
  A successfully submitted key frame atomically replaces the whole previous
   GOP and restarts frame numbering at 0.
  A non-key packet is rejected without disturbing the existing GOP when the
   current GOP already holds _max_frames frames.
  Return: 0 on success, TH_EINVAL on parameter or state errors, TH_EIMPL
   when the GOP capacity would be exceeded (or on resource exhaustion).*/
extern int th_region_packetin(th_region_ctx *_ctx,const ogg_packet *_op);

/*Materializes a rectangular region of one frame of the current GOP.
  _x and _y are measured from the upper-left corner of the displayed
   picture, must be non-negative and even, and the rectangle must lie
   entirely inside the picture; _width and _height must be positive and
   even. Invalid requests are rejected without disturbing any cached state.
  On success _out is filled with pointers to three planes owned by the
   context: the requested rectangle and its two half-size chroma planes,
   each with a positive stride. The lent memory remains valid until the
   next th_region_out() call, the next successfully committed key frame, or
   th_region_free().
  The returned pixels are byte-for-byte identical to the corresponding crop
   of what th_decode_packetin()/th_decode_ycbcr_out() would produce for the
   same frame with post-processing disabled.
  Return: 0 on success, TH_EINVAL on parameter or state errors, TH_EIMPL on
   resource exhaustion.*/
extern int th_region_out(th_region_ctx *_ctx,size_t _frame,
 int _x,int _y,int _width,int _height,th_ycbcr_buffer _out);

/*Reports statistics of the current GOP.
  _blocks:      The number of 8x8 plane blocks actually reconstructed or
                 copied so far in the current GOP.
  _pixel_bytes: The number of bytes currently allocated for persistent
                 pixel storage (unfiltered and filtered block caches;
                 coefficients and the current output buffer are not
                 included).
  Either pointer may be NULL to skip the corresponding value.
  Return: 0 on success, TH_EINVAL on parameter errors.*/
extern int th_region_stats(th_region_ctx *_ctx,
 size_t *_blocks,size_t *_pixel_bytes);

/*Releases a region decoding context. NULL is accepted.*/
extern void th_region_free(th_region_ctx *_ctx);

# if defined(__cplusplus)
}
# endif

#endif
