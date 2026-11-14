/*
 *  TwoLAME: an optimized MPEG Audio Layer Two encoder
 *
 *  Copyright (C) 2001-2004 Michael Cheng
 *  Copyright (C) 2004-2018 The TwoLAME Project
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifndef TWOLAME_ENCODE_H
#define TWOLAME_ENCODE_H

#include "bitbuffer.h"

int twolame_encode_init(twolame_options * glopts);

void twolame_scalefactor_calc(FLOAT sb_sample[][3][SCALE_BLOCK][SBLIMIT],
                              unsigned int scalar[][3][SBLIMIT], int nch, int sblimit);

void twolame_combine_lr(FLOAT sb_sample[2][3][SCALE_BLOCK][SBLIMIT],
                        FLOAT joint_sample[3][SCALE_BLOCK][SBLIMIT], int sblimit);

void twolame_find_sf_max(twolame_options * glopts,
                         unsigned int sf_index[2][3][SBLIMIT], FLOAT sf_max[2][SBLIMIT]);

void twolame_sf_transmission_pattern(twolame_options * glopts,
                                     unsigned int sf_index[2][3][SBLIMIT],
                                     unsigned int sf_selectinfo[2][SBLIMIT]);

void twolame_write_header(twolame_options * glopts, bit_stream * bs);

void twolame_write_bit_alloc(twolame_options * glopts, unsigned int bit_alloc[2][SBLIMIT], bit_stream * bs);

void twolame_write_scalefactors(twolame_options * glopts,
                                unsigned int bit_alloc[2][SBLIMIT],
                                unsigned int sf_selectinfo[2][SBLIMIT],
                                unsigned int scalar[2][3][SBLIMIT], bit_stream * bs);

void twolame_subband_quantization(twolame_options * glopts,
                                  unsigned int sf_index[2][3][SBLIMIT],
                                  FLOAT sb_samples[2][3][SCALE_BLOCK][SBLIMIT],
                                  unsigned int j_scale[3][SBLIMIT],
                                  FLOAT j_samps[3][SCALE_BLOCK][SBLIMIT],
                                  unsigned int bit_alloc[2][SBLIMIT],
                                  unsigned int sbband[2][3][SCALE_BLOCK][SBLIMIT]);

void twolame_write_samples(twolame_options * glopts,
                           unsigned int sbband[2][3][SCALE_BLOCK][SBLIMIT],
                           unsigned int bit_alloc[2][SBLIMIT], bit_stream * bs);


/*******************************************************
   Bit Allocation Stuff
******************************************************/

int twolame_bits_for_nonoise(twolame_options * glopts, FLOAT SMR[2][SBLIMIT],
                             unsigned int scfsi[2][SBLIMIT], FLOAT min_mnr,
                             unsigned int bit_alloc[2][SBLIMIT]);

int twolame_init_bit_allocation(twolame_options * glopts);

void twolame_main_bit_allocation(twolame_options * glopts, FLOAT SMR[2][SBLIMIT],
                                 unsigned int scfsi[2][SBLIMIT],
                                 unsigned int bit_alloc[2][SBLIMIT], int *adb);

int twolame_vbr_bit_allocation(twolame_options * glopts, FLOAT SMR[2][SBLIMIT],
                               unsigned int scfsi[2][SBLIMIT],
                               unsigned int bit_alloc[2][SBLIMIT], int *adb);

int twolame_a_bit_allocation(twolame_options * glopts, FLOAT SMR[2][SBLIMIT],
                             unsigned int scfsi[2][SBLIMIT], unsigned int bit_alloc[2][SBLIMIT], int *adb);


/*******************************************************
   Rate-Distortion Optimization
******************************************************/

/* Exact intra-frame joint selection of bit allocation levels and
   SCFSI scalefactor sharing under a fixed bit budget. */
int twolame_rd_bit_allocation(twolame_options * glopts, int *adb);

/* Write the sample codewords chosen by the RD solver. Mirrors the
   framing of twolame_write_samples() using the solver's own codes. */
void twolame_rd_write_samples(twolame_options * glopts, bit_stream * bs);

/* Internal accessors for the quantization tables defined in encode.c */
int twolame_rd_nbal_for(twolame_options * glopts, int sb);
int twolame_rd_max_alloc_for(twolame_options * glopts, int sb);
int twolame_rd_step_index_for(twolame_options * glopts, int sb, int ba);
int twolame_rd_steps(int step_index_value);
int twolame_rd_group(int step_index_value);
int twolame_rd_bits(int step_index_value);
FLOAT twolame_rd_scalefactor(int scale_index);
int twolame_rd_fixed_overhead(twolame_options * glopts);

/* Read-only observation points, only built for the target tests.
   They do not exist in the normal ABI. */
#ifdef TWOLAME_RD_TESTING
sb_sample_t *twolame_rd_test_get_samples(twolame_options * glopts);
subband_t *twolame_rd_test_get_subband(twolame_options * glopts);
unsigned int (*twolame_rd_test_get_scalar(twolame_options * glopts))[3][SBLIMIT];
unsigned int (*twolame_rd_test_get_bit_alloc(twolame_options * glopts))[SBLIMIT];
unsigned int (*twolame_rd_test_get_scfsi(twolame_options * glopts))[SBLIMIT];
#endif

#endif


// vim:ts=4:sw=4:nowrap:
