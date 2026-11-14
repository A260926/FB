/* Copyright (c) 2007-2010 Xiph.Org Foundation
   Written by Jean-Marc Valin */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Read-only observation point for the CELT decoder pitch post-filter.
 *
 * This file is only included by celt_decoder.c when CELT_POSTFILTER_TRACE is
 * defined (the test build).  It contains no state of its own and adds no
 * allocation, thread state, or exported symbol to a normal library build:
 * the test program defines celt_postfilter_trace_frame() and the decoder
 * merely hands it read-only pointers.
 *
 * Two calls are made per decoded CELT frame:
 *   phase == CELT_POSTFILTER_TRACE_BEFORE  -> samples/history/guard contain
 *       the synthesis output (and the preceding decoded history) before any
 *       post-filter tap has been applied;
 *   phase == CELT_POSTFILTER_TRACE_AFTER   -> samples contain the in-place
 *       post-filter result.
 *
 * The pitch/gain/tapset fields are the values the entropy decoder produced
 * for this frame (the "current generation").  The two older generations are
 * deliberately NOT exported: the reference has to reconstruct the parameter
 * timeline itself instead of trusting whatever the decoder happens to pass
 * to comb_filter().  The decoder's call arguments therefore never leak into
 * the reference.
 */

#ifndef CELT_POSTFILTER_TRACE_H
#define CELT_POSTFILTER_TRACE_H

#define CELT_POSTFILTER_TRACE_BEFORE 0
#define CELT_POSTFILTER_TRACE_AFTER  1

typedef struct {
   int phase;
   int frame_size;          /* N: number of samples post-filtered per channel */
   int short_mdct_size;     /* mode->shortMdctSize (first-segment length) */
   int lm;                  /* 0 for 2.5 ms frames, 1..3 for longer frames */
   int overlap;             /* mode->overlap */
   int channels;            /* CC */
   int arch;
   int pitch;               /* entropy-decoded post-filter period, may be 0 */
   float gain;              /* entropy-decoded post-filter gain, may be 0 */
   int tapset;              /* entropy-decoded tapset, may be 0 */

   /* Per-channel read-only views into the decoder's synthesis buffer.
   * samples[c][0 .. frame_size-1] is the current frame.  history[c] holds
   * history_len samples immediately preceding it (the delay taps).  guard[c]
   * holds guard_len samples immediately following the current frame; the
   * post-filter must not touch them. */
   int history_len;
   int guard_len;
   const float *samples[2];
   const float *history[2];
   const float *guard[2];

   /* The mode's MDCT window (overlap samples), so the test can evaluate the
   * square-window cross-fade from the same format data the library uses. */
   const float *window;
} celt_postfilter_frame;

void celt_postfilter_trace_frame(const celt_postfilter_frame *frame);

#endif /* CELT_POSTFILTER_TRACE_H */
