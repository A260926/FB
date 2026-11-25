/*
 * libmad - MPEG audio decoder library
 * Copyright (C) 2000-2004 Underbit Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * $Id: layer3.h,v 1.10 2004/01/23 09:41:32 rob Exp $
 */

# ifndef LIBMAD_LAYER3_H
# define LIBMAD_LAYER3_H

# include "stream.h"
# include "frame.h"

/*
 * Layer III side information.  This is exposed to the bounded granule
 * session, which must retain the complete side information of a physical
 * frame across granule boundaries (grammar across the two granules, SCFSI
 * reuse, and the shared main-data bit position).
 */
struct sideinfo {
  unsigned int main_data_begin;
  unsigned int private_bits;

  unsigned char scfsi[2];

  struct granule {
    struct channel {
      /* from side info */
      unsigned short part2_3_length;
      unsigned short big_values;
      unsigned short global_gain;
      unsigned short scalefac_compress;

      unsigned char flags;
      unsigned char block_type;
      unsigned char table_select[3];
      unsigned char subblock_gain[3];
      unsigned char region0_count;
      unsigned char region1_count;

      /* from main_data */
      unsigned char scalefac[39];	/* scalefac_l and/or scalefac_s */
    } ch[2];
  } gr[2];
};

/*
 * Per-physical-frame Layer III preparation state.  This is the shared,
 * resumable portion of frame decoding: everything up to the point where the
 * first granule's scalefactors/Huffman data are read, plus the saved state
 * needed to finish the reservoir once the last granule has been consumed.
 *
 * The main-data bit pointer and bits_left below are live across granules and
 * must not be recomputed or snapshotted between granule calls.
 */
struct mad_iii_frame {
  struct sideinfo si;

  unsigned int nch;
  unsigned int ngr;
  unsigned int sfreqi;

  unsigned int data_bitlen;
  unsigned int priv_bitlen;

  unsigned int md_len;			/* this frame's usable main-data bytes */
  unsigned int frame_space;
  unsigned int frame_used;
  unsigned int frame_free;
  unsigned int next_md_begin;

  struct mad_bitptr ptr;		/* main-data bit position */
  int bits_left;			/* main-data bits remaining */

  int result;				/* 0, or -1 with stream->error set */
};

int mad_layer_III(struct mad_stream *, struct mad_frame *);

/*
 * Prepare one physical frame: header sanity, CRC, side information, and
 * composition of this frame's main data.  Returns -1 on a hard error (the
 * caller must stop immediately), otherwise 0 with f->result recording any
 * soft error already stored in stream->error.  No spectral or synthesis
 * work is performed.
 */
int mad_iii_prepare(struct mad_stream *, struct mad_frame *,
		    struct mad_iii_frame *);

/* Decode exactly one granule's scalefactors/Huffman/stereo/IMDCT/overlap. */
enum mad_error mad_iii_decode_granule(struct mad_bitptr *,
				      struct mad_frame *,
				      struct sideinfo *,
				      unsigned int sfreqi,
				      unsigned int nch, unsigned int gr,
				      int *bits_left);

/*
 * Finalize the reservoir: preload main data for the following frame(s).
 * next_frame points one byte past the end of this physical frame; only the
 * frame_free / next_md_begin bytes preceding it are read.
 */
void mad_iii_preload(struct mad_stream *, struct mad_iii_frame const *,
		     unsigned char const *next_frame);

# endif
