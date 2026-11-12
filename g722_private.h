/*
 * g722.h - The ITU G.722 codec.
 *
 * Written by Steve Underwood <steveu@coppice.org>
 *
 * Copyright (C) 2005 Steve Underwood
 *
 *  Despite my general liking of the GPL, I place my own contributions 
 *  to this code in the public domain for the benefit of all mankind -
 *  even the slimy ones who might try to proprietize my work and use it
 *  to my detriment.
 *
 * Based on a single channel G.722 codec which is:
 *
 *****    Copyright (c) CMU    1993      *****
 * Computer Science, Speech Group
 * Chengxiang Lu and Alex Hauptmann
 */


/*! \file */

#pragma once

/*! \page g722_page G.722 encoding and decoding
\section g722_page_sec_1 What does it do?
The G.722 module is a bit exact implementation of the ITU G.722 specification for all three
specified bit rates - 64000bps, 56000bps and 48000bps. It passes the ITU tests.

To allow fast and flexible interworking with narrow band telephony, the encoder and decoder
support an option for the linear audio to be an 8k samples/second stream. In this mode the
codec is considerably faster, and still fully compatible with wideband terminals using G.722.

\section g722_page_sec_2 How does it work?
???.
*/

typedef struct g722_encode_state G722_ENC_CTX;
#define _G722_ENC_CTX_DEFINED
typedef struct g722_decode_state G722_DEC_CTX;
#define _G722_DEC_CTX_DEFINED

struct g722_band
{
    int s;
    int sp;
    int sz;
    int r[3];
    int a[3];
    int ap[3];
    int p[3];
    int d[7];
    int b[7];
    int bp[7];
    int sg[7];
    int nb;
    int det;
};

struct g722_encode_state
{
    /*! TRUE if the operating in the special ITU test mode, with the band split filters
             disabled. */
    int itu_test_mode;
    /*! TRUE if the G.722 data is packed */
    int packed;
    /*! TRUE if encode from 8k samples/second */
    int eight_k;
    /*! 6 for 48000kbps, 7 for 56000kbps, or 8 for 64000kbps. */
    int bits_per_sample;

    /*! Signal history for the QMF */
    int x[24];

    struct g722_band band[2];

    unsigned int in_buffer;
    int in_bits;
    unsigned int out_buffer;
    int out_bits;
};

struct g722_decode_state
{
    /*! TRUE if the operating in the special ITU test mode, with the band split filters
             disabled. */
    int itu_test_mode;
    /*! TRUE if the G.722 data is packed */
    int packed;
    /*! TRUE if decode to 8k samples/second */
    int eight_k;
    /*! 6 for 48000kbps, 7 for 56000kbps, or 8 for 64000kbps. */
    int bits_per_sample;

    /*! Signal history for the QMF */
    int x[24];

    struct g722_band band[2];

    unsigned int in_buffer;
    int in_bits;
    unsigned int out_buffer;
    int out_bits;

    /*! Packet loss concealment (fill-in) state.  Everything below is private
        to the decoder instance; no allocation is performed while filling in. */
    /*! Recent limited low-band reconstructions, oldest first in chronological
        order.  Only the last 320 values are retained. */
    int plc_hist[320];
    /*! Next write position in the history ring. */
    int plc_hist_pos;
    /*! Number of valid entries in the history ring (0..320). */
    int plc_hist_len;
    /*! One complete pitch period copied from the end of the real history. */
    int plc_period[120];
    /*! Selected pitch period in subband samples; 0 means zero fill. */
    int plc_pitch;
    /*! Last real high-band reconstruction before the current gap. */
    int plc_high_seed;
    /*! Last real high-band reconstruction seen at any time. */
    int plc_last_rhigh;
    /*! Number of subband instants produced for the current gap. */
    int plc_k;
    /*! TRUE while a missing interval is being filled and no real code arrived. */
    int plc_gap;
    /*! TRUE during the 40-subband-sample crossfade back to real data. */
    int plc_transition;
    /*! Crossfade position 0..39 since the first real subband instant. */
    int plc_recover;
};
