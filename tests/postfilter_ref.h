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

/* Independent reference for the CELT pitch post-filter.
 *
 * Derived from the five-tap delay-difference relation in the Opus
 * specification, not from the loops in celt/celt.c.  It never calls
 * comb_filter(), comb_filter_const_c(), their fast paths, and it does not
 * copy any of the library's unrolled loops.
 *
 * The arithmetic is deliberately performed in the floating-point signal
 * domain (float storage, float products), because the post-filter recurses
 * on its own output: a delayed tap whose feedback can approach unity must
 * read the same rounded values the decoder wrote back.  A double-precision
 * evaluation of the same formula diverges from the decoder's float
 * recursion for large gains, and such a divergence is a property of the
 * number format, not of the algorithm.
 *
 * Two independent properties are modelled and must not be conflated:
 *
 *  (a) In-place recursion.  The decoder post-filter runs in place, so a
 *      delayed tap of output n may point at a sample this call already
 *      wrote.  The reference evaluates one output sample at a time; for
 *      non-negative source indices it reads the output array when
 *      `in_place` and the pristine input otherwise.  The out-of-place entry
 *      (encoder pre-filter, PLC fold) therefore gets its own evaluation.
 *      Comparing the two entries with each other is not a correctness
 *      check.
 *
 *  (b) Parameter timeline.  The decoder keeps two generations of
 *      post-filter parameters.  The first segment of a frame cross-fades the
 *      OLD generation into the PREVIOUS frame's generation using the square
 *      of the MDCT window, then runs the previous generation constantly.
 *      For long frames (LM != 0) a second segment follows, cross-fading the
 *      previous generation into the CURRENT decoded generation and then
 *      running it constantly.  For short frames (LM == 0) the first segment
 *      covers the whole frame.  The test owns this timeline; comb_filter()'s
 *      call arguments must never be used as the expected sequence.
 */

#ifndef POSTFILTER_REF_H
#define POSTFILTER_REF_H

#include <math.h>

#define PF_MAX_PERIOD 1024
#define PF_MIN_PERIOD 15
#define PF_OVERLAP_MAX 120

/* Tap sets from the Opus bitstream description.  These are the format-data
 * gains also compiled into the float build; the specification lists the same
 * five decimals. */
static const float pf_taps[3][3] = {
   {0.3066406250f, 0.2170410156f, 0.1296386719f},
   {0.4638671875f, 0.2680664062f, 0.0f},
   {0.7998046875f, 0.1000976562f, 0.0f}
};

/* One generation of post-filter parameters. */
typedef struct {
   int period;
   float gain;
   int tapset;
} pf_gen;

/* History store: `data` points at the sample immediately before index 0 and
 * `data[PF_MAX_PERIOD + j]` is frame sample j for negative j. */
typedef struct {
   float *data;
   int len;              /* number of samples available before index 0 */
} pf_hist;

/* Read frame sample j.  Negative j comes from the history; non-negative j
 * comes from `y` for the in-place entry after it has been written, or from
 * the pristine `x` for the out-of-place entry. */
static float pf_read(const pf_hist *hist, const float *x, const float *y,
      int in_place, long j)
{
   if (j < 0) return hist->data[hist->len + j];
   if (in_place) return y[j];
   return x[j];
}

static int pf_gen_equal(const pf_gen *a, const pf_gen *b)
{
   return a->gain == b->gain && a->period == b->period &&
         a->tapset == b->tapset;
}

/* One five-tap delay-difference contribution with weight `w`. */
static float pf_tap_sum(const pf_hist *hist, const float *x, const float *y,
      int in_place, long n, const pf_gen *g, float w)
{
   const float *t = pf_taps[g->tapset];
   long T = g->period;
   float a0 = w * (g->gain * t[0]);
   float a1 = w * (g->gain * t[1]);
   float a2 = w * (g->gain * t[2]);
   float acc = a0 * pf_read(hist, x, y, in_place, n - T);
   acc += a1 * (pf_read(hist, x, y, in_place, n - T + 1) + pf_read(hist, x, y, in_place, n - T - 1));
   acc += a2 * (pf_read(hist, x, y, in_place, n - T + 2) + pf_read(hist, x, y, in_place, n - T - 2));
   return acc;
}

/* Evaluate one output sample: base x(n) plus the old and new generations
 * with weights wa and wb.  A generation with zero gain contributes nothing
 * (production skips it), and when both gains are zero the output is a copy
 * of x(n). */
static float pf_sample(const pf_hist *hist, const float *x, float *y,
      int in_place, long n, const pf_gen *ga, float wa, const pf_gen *gb, float wb)
{
   float acc = pf_read(hist, x, y, in_place, n);
   if (ga->gain != 0.0f && wa != 0.0f)
      acc += pf_tap_sum(hist, x, y, in_place, n, ga, wa);
   if (gb->gain != 0.0f && wb != 0.0f)
      acc += pf_tap_sum(hist, x, y, in_place, n, gb, wb);
   return acc;
}

/* Apply one filter segment (the equivalent of one comb_filter() call with a
 * single generation handoff).  `fade` is the number of samples over which the
 * window square blends `ga` into `gb`; the remainder uses `gb` constantly
 * (or is a copy when gb->gain == 0).  `y` must already hold the pristine
 * input for the frame so the base term and in-place reads are well defined. */
static void pf_segment(const pf_hist *hist, const float *x, float *y,
      int in_place, long start, long len,
      const pf_gen *ga, const pf_gen *gb, const float *window, int fade)
{
   long i;
   for (i = 0; i < len; i++) {
      long n = start + i;
      float wa, wb;
      if (i < fade) {
         float f = window[i] * window[i];
         wa = 1.0f - f;
         wb = f;
      } else {
         wa = 0.0f;
         wb = 1.0f;
      }
      y[n] = pf_sample(hist, x, y, in_place, n, ga, wa, gb, wb);
   }
}

/* Fade length production uses for a segment: none when the generations are
 * identical, and the overlap length otherwise (clamped to the segment). */
static int pf_fade_len(const pf_gen *a, const pf_gen *b, int overlap, int len)
{
   if (a->gain == 0.0f && b->gain == 0.0f) return 0;
   if (pf_gen_equal(a, b)) return 0;
   if (overlap > len) return len;
   return overlap;
}

#endif /* POSTFILTER_REF_H */
