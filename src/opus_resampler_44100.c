/* Copyright (c) 2026 Xiph.Org Foundation
   Redistribution and use in source and binary forms are governed by the
   same BSD-style license as the rest of Opus; see COPYING for details. */

/* Causal 48 kHz -> 44.1 kHz resampling engine.  See opus_resampler_44100.h
   for the clocking and storage contract.  The 147 fractional FIR phases are
   derived once, lazily, from the windowed sinc reference definition:

     c     = 0.45*147/160
     w[k]  = 0.42 - 0.5*cos(2*pi*k/63) + 0.08*cos(4*pi*k/63)
     a[k]  = 2*c*sinc(2*c*(k+f-31.5))*w[k]
     h[k]  = a[k]/sum(a)
     sinc(0) = 1
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "opus_resampler_44100.h"

#include <math.h>
#include <string.h>

#define R44100_PI 3.14159265358979323846

/* 147 phases x 64 taps, precomputed on first use.  Bounded static storage. */
static float r44100_coeffs[OPUS_R44100_PHASES][OPUS_R44100_TAPS];
static int r44100_coeffs_ready;

static double r44100_sinc(double z)
{
   if (z == 0.0)
      return 1.0;
   {
      double piz = R44100_PI*z;
      return sin(piz)/piz;
   }
}

static void r44100_compute_coeffs(void)
{
   /* Cutoff of the reference sinc expressed relative to the input rate. */
   const double c = 0.45*147.0/160.0;
   int phase;
   for (phase=0; phase<OPUS_R44100_PHASES; phase++)
   {
      double f = (double)phase/(double)OPUS_R44100_PHASES;
      double a[OPUS_R44100_TAPS];
      double sum = 0.0;
      int k;
      for (k=0; k<OPUS_R44100_TAPS; k++)
      {
         double w = 0.42
                    - 0.5*cos(2.0*R44100_PI*k/63.0)
                    + 0.08*cos(4.0*R44100_PI*k/63.0);
         double z = 2.0*c*((double)k + f - 31.5);
         a[k] = 2.0*c*r44100_sinc(z)*w;
         sum += a[k];
      }
      for (k=0; k<OPUS_R44100_TAPS; k++)
         r44100_coeffs[phase][k] = (float)(a[k]/sum);
   }
   r44100_coeffs_ready = 1;
}

int opus_r44100_init(OpusResampler44100 *st, int channels)
{
   if (st == NULL || (channels != 1 && channels != 2))
      return OPUS_BAD_ARG;
   if (!r44100_coeffs_ready)
      r44100_compute_coeffs();
   memset(st, 0, sizeof(*st));
   st->channels = channels;
   st->initialized = 1;
   st->hist_head = OPUS_R44100_HISTORY-1;
   return OPUS_OK;
}

int opus_r44100_reset(OpusResampler44100 *st)
{
   int channels;
   if (st == NULL || !st->initialized)
      return OPUS_BAD_ARG;
   channels = st->channels;
   memset(st, 0, sizeof(*st));
   st->channels = channels;
   st->initialized = 1;
   st->hist_head = OPUS_R44100_HISTORY-1;
   return OPUS_OK;
}

opus_int64 opus_r44100_outputs_for(opus_int64 src_total)
{
   if (src_total <= 0)
      return 0;
   return (src_total*147)/160;
}

int opus_r44100_pending(const OpusResampler44100 *st, int src_frame_count)
{
   opus_int64 target;
   if (st == NULL || !st->initialized || src_frame_count < 0)
      return OPUS_BAD_ARG;
   target = opus_r44100_outputs_for(st->src_count + src_frame_count);
   return (int)(target - st->out_count);
}

int opus_r44100_source_needed(const OpusResampler44100 *st, int out_frame_count)
{
   opus_int64 target;
   opus_int64 needed;
   if (st == NULL || !st->initialized || out_frame_count < 0)
      return OPUS_BAD_ARG;
   target = st->out_count + out_frame_count;
   /* Smallest S with floor(S*147/160) >= target is ceil(target*160/147). */
   needed = (target*160 + 146)/147;
   if (needed < st->src_count)
      needed = st->src_count;
   return (int)(needed - st->src_count);
}

/* Computes one output frame (global index out_index) from the 64 most recent
   source frames, the newest of which carries global index n == floor(t).
   Writes one contiguous sample per channel into dst (room for 2). */
static void r44100_emit(const OpusResampler44100 *st, opus_int64 out_index,
      long n, float *dst)
{
   /* t = m*160/147 = m + 13m/147, so frac(t) = (13m mod 147)/147 and the
      precomputed phase index is (13*m) mod 147, never just m mod 147. */
   int phase = (int)((13*(out_index % OPUS_R44100_PHASES))
                     % OPUS_R44100_PHASES);
   const float *h = r44100_coeffs[phase];
   int c, k;
   (void)n;
   for (c=0; c<st->channels; c++)
   {
      double acc = 0.0;
      for (k=0; k<OPUS_R44100_TAPS; k++)
      {
         /* Source frame n-k sits this many slots behind the newest one. */
         int slot = st->hist_head - k;
         if (slot < 0)
            slot += OPUS_R44100_HISTORY;
         /* Frames with negative global coordinates are zero; before enough
            history exists the corresponding slots were zero-initialized. */
         if (n - k >= 0)
            acc += (double)h[k]
                   * (double)st->history[slot*st->channels+c];
      }
      dst[c] = (float)acc;
   }
}

int opus_r44100_process(OpusResampler44100 *st, const float *in, int in_stride,
      int src_frame_count, float *out, int out_stride, int out_capacity)
{
   int want;
   int produced = 0;
   int i, c;
   int head;
   opus_int64 base_src;
   opus_int64 deliver_target;

   if (st == NULL || !st->initialized || in == NULL || out == NULL
         || in_stride < st->channels || out_stride < st->channels
         || src_frame_count < 0 || out_capacity < 0)
      return OPUS_BAD_ARG;

   want = opus_r44100_pending(st, src_frame_count);
   if (want < 0)
      return OPUS_BAD_ARG;
   /* Capacity failure must leave every piece of state untouched. */
   if (want > out_capacity)
      return OPUS_BUFFER_TOO_SMALL;

   if (!r44100_coeffs_ready)
      r44100_compute_coeffs();

   base_src = st->src_count;
   head = st->hist_head;
   /* Global index one past the last output this call may deliver. */
   deliver_target = st->out_count + want;

   /* A single output computed during a previous block but withheld by the
      clock (at most one can exist) becomes deliverable in this block. */
   if (st->hold_valid && st->hold_index < deliver_target)
   {
      for (c=0; c<st->channels; c++)
         out[produced*out_stride+c] = st->hold[c];
      produced++;
      st->hold_valid = 0;
   }

   for (i=0; i<src_frame_count; i++)
   {
      opus_int64 g = base_src + i;
      /* Push this source frame into the bounded circular history. */
      head = (head+1 == OPUS_R44100_HISTORY) ? 0 : head+1;
      for (c=0; c<st->channels; c++)
         st->history[head*st->channels+c] = in[i*in_stride+c];

      /* Emit every output whose reference time now lies in the history.
         By construction the next pending output has n == g exactly, so all
         64 taps are still present. */
      for (;;)
      {
         opus_int64 m = st->out_count + produced
                        + (st->hold_valid ? 1 : 0);
         double t = (double)m*160.0/147.0;
         long n = (long)floor(t);
         float frame[2];
         if (n > g)
            break;
         st->hist_head = head;
         r44100_emit(st, m, n, frame);
         if (m < deliver_target)
         {
            for (c=0; c<st->channels; c++)
               out[produced*out_stride+c] = frame[c];
            produced++;
         } else {
            /* Clock withholds this one; keep it for the next process call.
               At most one such frame can ever exist. */
            for (c=0; c<st->channels; c++)
               st->hold[c] = frame[c];
            st->hold_index = m;
            st->hold_valid = 1;
            break;
         }
      }
   }

   st->hist_head = head;
   st->src_count = base_src + src_frame_count;
   st->out_count += produced;
   st->last_out = produced;
   return produced;
}
