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
 *
 * $Id: g722_decoder.h,v 1.1 2012/08/07 11:33:45 sobomax Exp $
 */


/*! \file */

#pragma once

#include "g722.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _G722_DEC_CTX_DEFINED
typedef void G722_DEC_CTX;
#define _G722_DEC_CTX_DEFINED
#endif

G722_DEC_CTX *g722_decoder_new(int rate, int options);
int g722_decoder_destroy(G722_DEC_CTX *s);
int g722_decode(G722_DEC_CTX *s, const uint8_t g722_data[], int len, int16_t amp[]);
/*! Fill in a missing interval after band reconstruction and before the
    receive QMF.  \a missing_pairs counts 8 kHz subband instants: a normal
    (16 kHz output) decoder produces twice that many PCM samples; with
    G722_SAMPLE_RATE_8000 it produces exactly that many.  Returns the number
    of samples written, 0 for an empty call, or -1 for a negative/oversized
    count or an unsupported (packed) context. */
int g722_fillin(G722_DEC_CTX *s, int16_t out[], int missing_pairs);

#ifdef __cplusplus
}
#endif
