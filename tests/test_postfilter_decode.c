/* Decoder-level content regression for the CELT pitch post-filter.
 *
 * The signal is produced locally with the public encoder and decoded with the
 * public decoder through the trace build of the library (OPUS_POSTFILTER_TRACE).
 * Only read-only observation points are added in that build: the entropy
 * decoded current-frame parameters, the pre-filter synthesis samples, and the
 * post-filter samples.  A normal library build has no extra allocation, no
 * extra thread state and no additional exported ABI.
 *
 * The reference is not the decoder's own comb_filter() arguments.  This test
 * maintains its own two generations of post-filter parameters and its own
 * per-channel sample history, replays the same timeline (first segment for
 * every frame, second segment for long frames), and evaluates the independent
 * five-tap recurrence from postfilter_ref.h.  It then compares the observed
 * post-filter samples with that reference element by element.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "opus.h"
#include "celt/postfilter_trace.h"
#include "postfilter_ref.h"

#define MAX_FRAMES 96
#define MAX_FRAME_SAMPLES 960
#define MAX_CHANNELS 2
#define PF_GUARD_MAX 256

/* ---- Deterministic signal ------------------------------------------------ */

static unsigned long g_rng;
static double noise(void)
{
   g_rng = g_rng * 1664525UL + 1013904223UL;
   return (double)((g_rng >> 8) & 0xffffffUL) / 16777216.0 - 0.5;
}

/* Two channels with different periodic content, a slowly moving pitch (so
 * the entropy-coded pitch/gain/tapset actually change over the run), and a
 * small deterministic dither so the decoder sees realistic low-level detail.
 * Each channel uses a different partial mix and a different phase. */
static void fill_frame(float *pcm, int N, long t, double f0_l, double f0_r)
{
   int i;
   for (i = 0; i < N; i++) {
      double tt = (double)(t + i) / 48000.0;
      double mod = 1.0 + 0.015 * sin(2.0 * M_PI * 1.7 * tt);
      pcm[2 * i] = (float)(0.120 * sin(2.0 * M_PI * f0_l * mod * tt)
                         + 0.035 * sin(4.0 * M_PI * f0_l * tt + 0.20)
                         + 0.012 * sin(6.0 * M_PI * f0_l * tt + 1.10)
                         + 1.0e-4 * noise());
      pcm[2 * i + 1] = (float)(0.110 * sin(2.0 * M_PI * f0_r * mod * tt + 0.80)
                             + 0.030 * sin(3.0 * M_PI * f0_r * tt + 0.35)
                             + 0.015 * sin(8.0 * M_PI * f0_r * tt + 2.00)
                             + 1.0e-4 * noise());
   }
}

/* ---- Observation storage ------------------------------------------------- */

typedef struct {
   int packet;            /* packet index within this run */
   int N;                 /* frame length */
   int short_mdct;        /* first-segment length */
   int lm;
   int overlap;
   int channels;
   int pitch;             /* entropy-decoded current generation */
   float gain;
   int tapset;
   float pre[MAX_CHANNELS][MAX_FRAME_SAMPLES];
   float post[MAX_CHANNELS][MAX_FRAME_SAMPLES];
   float history[MAX_CHANNELS][PF_MAX_PERIOD];
   float history_after[MAX_CHANNELS][PF_MAX_PERIOD];
   float guard[MAX_CHANNELS][PF_GUARD_MAX];
   float guard_after[MAX_CHANNELS][PF_GUARD_MAX];
   int guard_len;
   int history_len;
   float window[PF_OVERLAP_MAX];
   float last_ref[MAX_CHANNELS][MAX_FRAME_SAMPLES];
} observed_frame;

static long g_packet_index;   /* index of the packet currently being decoded */
static observed_frame g_frames[MAX_FRAMES];
static int g_frame_count;
static int g_pending;              /* BEFORE seen, waiting for AFTER */
static observed_frame g_pending_frame;

void celt_postfilter_trace_frame(const celt_postfilter_frame *f)
{
   int c;
   if (f->phase == CELT_POSTFILTER_TRACE_BEFORE) {
      if (g_pending) {
         fprintf(stderr, "POSTFILTER_DEC_FAIL nested BEFORE observation\n");
         exit(2);
      }
      if (f->channels < 1 || f->channels > 2) {
         fprintf(stderr, "POSTFILTER_DEC_FAIL unexpected channel count %d\n", f->channels);
         exit(2);
      }
      if (f->frame_size > MAX_FRAME_SAMPLES || f->history_len > PF_MAX_PERIOD ||
            f->guard_len > PF_GUARD_MAX) {
         fprintf(stderr, "POSTFILTER_DEC_FAIL observation too large\n");
         exit(2);
      }
      memset(&g_pending_frame, 0, sizeof(g_pending_frame));
      g_pending_frame.packet = (int)g_packet_index;
      g_pending_frame.N = f->frame_size;
      g_pending_frame.short_mdct = f->short_mdct_size;
      g_pending_frame.lm = f->lm;
      g_pending_frame.overlap = f->overlap;
      g_pending_frame.channels = f->channels;
      g_pending_frame.pitch = f->pitch;
      g_pending_frame.gain = f->gain;
      g_pending_frame.tapset = f->tapset;
      g_pending_frame.guard_len = f->guard_len;
      g_pending_frame.history_len = f->history_len;
      memcpy(g_pending_frame.window, f->window, (size_t)f->overlap * sizeof(float));
      for (c = 0; c < f->channels; c++) {
         memcpy(g_pending_frame.pre[c], f->samples[c], (size_t)f->frame_size * sizeof(float));
         memcpy(g_pending_frame.history[c], f->history[c], (size_t)f->history_len * sizeof(float));
         memcpy(g_pending_frame.guard[c], f->guard[c], (size_t)f->guard_len * sizeof(float));
      }
      g_pending = 1;
   } else {
      if (!g_pending) {
         fprintf(stderr, "POSTFILTER_DEC_FAIL AFTER without BEFORE\n");
         exit(2);
      }
      if (g_frame_count >= MAX_FRAMES) {
         fprintf(stderr, "POSTFILTER_DEC_FAIL too many frames\n");
         exit(2);
      }
      if (f->channels != g_pending_frame.channels ||
            f->frame_size != g_pending_frame.N ||
            f->history_len != g_pending_frame.history_len ||
            f->guard_len != g_pending_frame.guard_len) {
         fprintf(stderr, "POSTFILTER_DEC_FAIL BEFORE/AFTER shape mismatch\n");
         exit(2);
      }
      for (c = 0; c < f->channels; c++) {
         memcpy(g_pending_frame.post[c], f->samples[c], (size_t)f->frame_size * sizeof(float));
         memcpy(g_pending_frame.history_after[c], f->history[c], (size_t)f->history_len * sizeof(float));
         memcpy(g_pending_frame.guard_after[c], f->guard[c], (size_t)f->guard_len * sizeof(float));
      }
      g_frames[g_frame_count++] = g_pending_frame;
      g_pending = 0;
   }
}

/* ---- Per-length bookkeeping --------------------------------------------- */

typedef struct {
   const char *name;
   int ms;
   int N;
   int opus_frame_duration;
   long compared;
   long pf_frames;
   long frames;
   int first_diff_packet;
   int first_diff_channel;
   long first_diff_sample;
   double first_diff_expected;
   double first_diff_observed;
   double worst_err, worst_limit;
} length_stats;

static length_stats g_len[3];

/* ---- Reference replay ---------------------------------------------------- */

static void replay_length(length_stats *st)
{
   pf_gen gen_old, gen_prev;
   float hist[MAX_CHANNELS][PF_MAX_PERIOD];
   int hist_ready = 0;
   int i, c, f;

   /* Two generations are owned here, starting from the decoder's reset
      state (zero gain, zero period). */
   gen_old.period = 0;  gen_old.gain = 0.0f;  gen_old.tapset = 0;
   gen_prev = gen_old;

   for (f = 0; f < g_frame_count; f++) {
      observed_frame *ob = &g_frames[f];
      pf_gen cur;

      cur.period = ob->pitch;
      cur.gain = ob->gain;
      cur.tapset = ob->tapset;

      if (ob->lm == 0 && ob->short_mdct != ob->N) {
         fprintf(stderr, "POSTFILTER_DEC_FAIL short frame first segment does not span the frame (%d of %d)\n",
               ob->short_mdct, ob->N);
         exit(1);
      }
      if (ob->lm != 0 && ob->short_mdct >= ob->N) {
         fprintf(stderr, "POSTFILTER_DEC_FAIL long frame has no second segment\n");
         exit(1);
      }

      /* The first decoded frame follows a reset.  The observation point
         exposes the decoder's history buffer as it stands before the taps
         are applied; seed the reference history from it so the comparison is
         about the filter, not about the decoder's startup samples. */
      if (!hist_ready) {
         for (c = 0; c < ob->channels; c++)
            memcpy(hist[c], ob->history[c], sizeof(hist[c]));
         hist_ready = 1;
      }

      st->frames++;
      if (cur.gain != 0.0f) st->pf_frames++;

      for (c = 0; c < ob->channels; c++) {
         pf_hist h;
         int fade1, fade2;
         int second = ob->lm != 0;
         float ref[MAX_FRAME_SAMPLES];

         memcpy(ref, ob->pre[c], (size_t)ob->N * sizeof(float));
         h.data = hist[c];
         h.len = PF_MAX_PERIOD;

         /* First segment: OLD generation cross-fades into the PREVIOUS
            frame's generation over the window square, then runs constantly
            to the end of the segment. */
         fade1 = pf_fade_len(&gen_old, &gen_prev, ob->overlap, ob->short_mdct);
         pf_segment(&h, ob->pre[c], ref, 1, 0, ob->short_mdct,
               &gen_old, &gen_prev, ob->window, fade1);

         /* Second segment, only for long frames: the previous generation
            cross-fades into the CURRENT (freshly decoded) one, which then
            runs constantly to the end of the frame. */
         if (second) {
            fade2 = pf_fade_len(&gen_prev, &cur, ob->overlap,
                  ob->N - ob->short_mdct);
            pf_segment(&h, ob->pre[c], ref, 1, ob->short_mdct,
                  ob->N - ob->short_mdct, &gen_prev, &cur, ob->window, fade2);
         }

         /* Compare this channel's post-filter samples at the stated
            tolerance; also require the observations to be finite. */
         for (i = 0; i < ob->N; i++) {
            float expected = ref[i];
            float observed = ob->post[c][i];
            st->compared++;
            if (!isfinite(observed)) {
               fprintf(stderr, "POSTFILTER_DEC_FAIL non-finite observed sample packet=%d channel=%d index=%d\n",
                     ob->packet, c, i);
               exit(1);
            }
            {
               double err = fabs((double)observed - (double)expected);
               double limit = 4e-5 * (1.0 + fabs((double)expected));
               if (err > st->worst_err) { st->worst_err = err; st->worst_limit = limit; }
               if (err > limit && st->first_diff_packet < 0) {
                  st->first_diff_packet = ob->packet;
                  st->first_diff_channel = c;
                  st->first_diff_sample = i;
                  st->first_diff_expected = expected;
                  st->first_diff_observed = observed;
               }
            }
         }
         memcpy(ob->last_ref[c], ref, (size_t)ob->N * sizeof(float));

         /* Protected regions: the post-filter must not write the history
            prefix before the frame nor the samples after it. */
         for (i = 0; i < ob->history_len; i++) {
            if (ob->history_after[c][i] != ob->history[c][i]) {
               fprintf(stderr, "POSTFILTER_DEC_FAIL history modified packet=%d channel=%d offset=%d\n",
                     ob->packet, c, i);
               exit(1);
            }
         }
         for (i = 0; i < ob->guard_len; i++) {
            if (ob->guard_after[c][i] != ob->guard[c][i]) {
               fprintf(stderr, "POSTFILTER_DEC_FAIL guard modified packet=%d channel=%d offset=%d\n",
                     ob->packet, c, i);
               exit(1);
            }
         }
      }

      /* Advance the timeline exactly the way the decoder does.  The previous
         generation becomes the old one and the current becomes the previous;
         for long frames the second segment already promoted the current
         generation into both slots. */
      {
         pf_gen new_prev = cur;
         pf_gen new_old = gen_prev;
         gen_old = (ob->lm != 0) ? new_prev : new_old;
         gen_prev = new_prev;
      }

      /* The next frame's delay taps read this frame's post-filtered output.
         The reference advances on its OWN output, so a single divergent
         sample is reported where it first occurs instead of being folded
         back into the reference and compounding.  The observed history is
         used only to seed the first frame, where it is the decoder's reset
         buffer. */
      for (c = 0; c < ob->channels; c++) {
         memmove(hist[c], hist[c] + ob->N,
               (size_t)(PF_MAX_PERIOD - ob->N) * sizeof(float));
         memcpy(hist[c] + (PF_MAX_PERIOD - ob->N), ob->last_ref[c],
               (size_t)ob->N * sizeof(float));
      }
   }
}

/* ---- Encoder/decoder driver --------------------------------------------- */

static int run_length(length_stats *st, int bitrate)
{
   OpusEncoder *enc;
   OpusDecoder *dec;
   unsigned char packet[1500];
   float pcm[2 * MAX_FRAME_SAMPLES];
   float out[2 * MAX_FRAME_SAMPLES];
   int err;
   int p;
   long t = 0;
   int N = st->N;

   g_frame_count = 0;
   g_pending = 0;
   g_packet_index = 0;
   st->compared = 0;
   st->pf_frames = 0;
   st->frames = 0;
   st->first_diff_packet = -1;
   st->first_diff_channel = -1;
   st->first_diff_sample = -1;
   st->worst_err = 0.0;
   st->worst_limit = 0.0;

   enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
   if (!enc || err != OPUS_OK) return 1;
   dec = opus_decoder_create(48000, 2, &err);
   if (!dec || err != OPUS_OK) return 1;

   if (opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate)) != OPUS_OK) return 1;
   if (opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10)) != OPUS_OK) return 1;
   if (opus_encoder_ctl(enc, OPUS_SET_EXPERT_FRAME_DURATION(st->opus_frame_duration)) != OPUS_OK) return 1;
   if (opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND)) != OPUS_OK) return 1;

   g_rng = 0x12345678UL;
   for (p = 0; p < MAX_FRAMES; p++) {
      double pitch_l, pitch_r;
      int n;
      /* Move the pitch in steps so the post-filter period, gain and tapset
         actually change between frames.  Larger steps than the overlap
         length make the two-generation handoff visible on every length. */
      switch ((p / 2) % 6) {
      case 0: pitch_l = 171.0; pitch_r = 196.0; break;
      case 1: pitch_l = 223.0; pitch_r = 158.0; break;
      case 2: pitch_l = 242.0; pitch_r = 208.0; break;
      case 3: pitch_l = 165.0; pitch_r = 181.0; break;
      case 4: pitch_l = 260.0; pitch_r = 277.0; break;
      default: pitch_l = 205.0; pitch_r = 220.0; break;
      }
      fill_frame(pcm, N, t, pitch_l, pitch_r);

      n = opus_encode_float(enc, pcm, N, packet, sizeof(packet));
      if (n <= 0) {
         fprintf(stderr, "POSTFILTER_DEC_FAIL encode returned %d\n", n);
         return 1;
      }
      g_packet_index = p;
      n = opus_decode_float(dec, packet, n, out, N, 0);
      if (n != N) {
         fprintf(stderr, "POSTFILTER_DEC_FAIL decode returned %d\n", n);
         return 1;
      }
      t += N;
   }

   opus_encoder_destroy(enc);
   opus_decoder_destroy(dec);

   if (g_frame_count == 0) {
      fprintf(stderr, "POSTFILTER_DEC_FAIL %s: no decoded frames observed\n", st->name);
      return 1;
   }
   replay_length(st);
   if (st->pf_frames == 0) {
      fprintf(stderr, "POSTFILTER_DEC_FAIL %s: generated signal never triggered a nonzero post-filter\n", st->name);
      return 1;
   }

   printf("POSTFILTER_DEC_LENGTH %s N=%d worst_err=%.3g worst_limit=%.3g compared=%ld pf_frames=%ld frames=%ld first_diff_packet=%d first_diff_channel=%d first_diff_sample=%ld\n",
         st->name, st->N, st->worst_err, st->worst_limit, st->compared, st->pf_frames, st->frames,
         st->first_diff_packet, st->first_diff_channel, st->first_diff_sample);
   if (st->first_diff_packet >= 0) {
      printf("POSTFILTER_DEC_FAIL %s first difference at packet=%d channel=%d sample=%ld expected=%.9g observed=%.9g\n",
            st->name, st->first_diff_packet, st->first_diff_channel,
            st->first_diff_sample, st->first_diff_expected, st->first_diff_observed);
      return 1;
   }
   if (st->compared == 0) {
      fprintf(stderr, "POSTFILTER_DEC_FAIL %s: zero samples compared\n", st->name);
      return 1;
   }
   return 0;
}

int main(void)
{
   int fails = 0;

   g_len[0].name = "2.5ms"; g_len[0].ms = 2;  g_len[0].N = 120;
   g_len[0].opus_frame_duration = OPUS_FRAMESIZE_2_5_MS;
   g_len[1].name = "5ms";   g_len[1].ms = 5;  g_len[1].N = 240;
   g_len[1].opus_frame_duration = OPUS_FRAMESIZE_5_MS;
   g_len[2].name = "20ms";  g_len[2].ms = 20; g_len[2].N = 960;
   g_len[2].opus_frame_duration = OPUS_FRAMESIZE_20_MS;

   printf("POSTFILTER_DEC_START\n");
   fails += run_length(&g_len[0], 128000);
   fails += run_length(&g_len[1], 128000);
   fails += run_length(&g_len[2], 128000);

   if (fails) {
      printf("POSTFILTER_DEC_FAIL failures=%d\n", fails);
      return 1;
   }
   printf("POSTFILTER_DEC_PASS\n");
   return 0;
}
