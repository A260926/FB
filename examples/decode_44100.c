/* decode_44100.c -- small end-to-end demo of the 44.1 kHz float decode entry.
   It encodes a continuous stream of short (2.5 ms) 48 kHz packets with the
   ordinary Opus encoder, then decodes them both through a normal 48 kHz
   decoder and through a 44100 Hz decoder.  The 44.1 kHz PCM is checked
   sample-by-sample against an independent 48 kHz decoder passed through the
   specified causal 64-tap 48->44.1 reference filter, the cumulative frame
   count is verified against floor(S*147/160), and the packet-count queries
   are checked.  No audio device, download or extra library is used. */

#include "opus.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_TAPS 64
#define DEMO_PHASES 147
#define DEMO_PI 3.14159265358979323846
#define DEMO_PACKETS 40
#define DEMO_FRAME 120           /* 2.5 ms at 48 kHz */
#define DEMO_CHANNELS 2

static double demo_h[DEMO_PHASES][DEMO_TAPS];
static double demo_sinc(double z)
{
   return z == 0.0 ? 1.0 : sin(DEMO_PI*z)/(DEMO_PI*z);
}
static void demo_reference_init(void)
{
   double c = 0.45*147.0/160.0;
   int ph, k;
   for (ph=0; ph<DEMO_PHASES; ph++)
   {
      double f = (double)ph/DEMO_PHASES, a[DEMO_TAPS], sum = 0.0;
      for (k=0; k<DEMO_TAPS; k++)
      {
         double w = 0.42 - 0.5*cos(2.0*DEMO_PI*k/63.0)
                        + 0.08*cos(4.0*DEMO_PI*k/63.0);
         a[k] = 2.0*c*demo_sinc(2.0*c*((double)k + f - 31.5))*w;
         sum += a[k];
      }
      for (k=0; k<DEMO_TAPS; k++) demo_h[ph][k] = a[k]/sum;
   }
}
static float demo_reference(const float *x48, long n48, long m, int ch)
{
   double t = (double)m*160.0/147.0;
   long n = (long)floor(t);
   int ph = (int)((13*(m % DEMO_PHASES)) % DEMO_PHASES);
   double acc = 0.0;
   int k;
   for (k=0; k<DEMO_TAPS; k++)
   {
      long idx = n - k;
      if (idx >= 0 && idx < n48)
         acc += demo_h[ph][k]*x48[idx*DEMO_CHANNELS+ch];
   }
   return (float)acc;
}
static float demo_signal(long n, int ch)
{
   return (float)(0.30*sin(n*0.031 + ch*1.3)
                + 0.16*sin(n*0.073 + 0.7*ch + 1.0)
                + 0.05*sin(n*0.19 + 0.4));
}

int main(void)
{
   int err = 0;
   OpusEncoder *enc;
   OpusDecoder *dec44, *dec48;
   unsigned char packet[2000];
   float input[DEMO_FRAME*DEMO_CHANNELS];
   float *pcm48, *pcm44;
   long src_total = 0, out_total = 0;
   long cap48 = (long)DEMO_PACKETS*DEMO_FRAME;
   double energy = 0.0, maxerr = 0.0;
   int p;

   demo_reference_init();
   pcm48 = (float*)malloc((size_t)cap48*DEMO_CHANNELS*sizeof(float));
   pcm44 = (float*)malloc((size_t)(cap48*147/160+8)*DEMO_CHANNELS
                          *sizeof(float));
   if (!pcm48 || !pcm44) return 2;

   enc = opus_encoder_create(48000, DEMO_CHANNELS,
         OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
   dec44 = opus_decoder_create(44100, DEMO_CHANNELS, &err);
   dec48 = opus_decoder_create(48000, DEMO_CHANNELS, &err);
   if (!enc || !dec44 || !dec48 || err != OPUS_OK) return 3;

   {
      opus_int32 rate = 0;
      opus_decoder_ctl(dec44, OPUS_GET_SAMPLE_RATE(&rate));
      if (rate != 44100) return 4;
   }

   for (p=0; p<DEMO_PACKETS; p++)
   {
      int i, bytes, n48, n44, predicted;
      opus_int32 last_dur = 0;
      for (i=0; i<DEMO_FRAME*DEMO_CHANNELS; i++)
         input[i] = demo_signal(src_total + i/DEMO_CHANNELS, i & 1);

      bytes = opus_encode_float(enc, input, DEMO_FRAME, packet, sizeof(packet));
      if (bytes <= 0) return 5;

      /* The stateful query predicts the next ordinary decode exactly. */
      predicted = opus_decoder_get_nb_samples(dec44, packet, bytes);
      if (predicted <= 0) return 6;

      n48 = opus_decode_float(dec48, packet, bytes,
            pcm48+src_total*DEMO_CHANNELS, 5760, 0);
      n44 = opus_decode_float(dec44, packet, bytes,
            pcm44+out_total*DEMO_CHANNELS, 5760, 0);
      if (n48 != DEMO_FRAME || n44 != predicted) return 7;

      opus_decoder_ctl(dec44, OPUS_GET_LAST_PACKET_DURATION(&last_dur));
      if (last_dur != n44) return 8;

      for (i=0; i<n44*DEMO_CHANNELS; i++)
      {
         if (!isfinite(pcm44[out_total*DEMO_CHANNELS+i])) return 9;
         energy += (double)pcm44[out_total*DEMO_CHANNELS+i]
                   * pcm44[out_total*DEMO_CHANNELS+i];
      }
      src_total += n48;
      out_total += n44;
   }

   /* Cumulative clock: floor(S*147/160), never rounded per packet. */
   if (out_total != src_total*147/160) return 10;
   if (energy <= 1.0) return 11;

   /* Independent 48 kHz decoder + scalar reference must match the new entry. */
   for (p=0; p<out_total; p++)
   {
      int c;
      for (c=0; c<DEMO_CHANNELS; c++)
      {
         double e = fabs((double)demo_reference(pcm48, src_total, p, c)
                         - pcm44[p*DEMO_CHANNELS+c]);
         if (e > maxerr) maxerr = e;
      }
   }
   if (maxerr > 3e-6) return 12;

   printf("Encoded %d continuous 2.5 ms packets (%ld 48 kHz source frames).\n",
         DEMO_PACKETS, src_total);
   printf("44.1 kHz decoder returned %ld frames (floor(%ld*147/160)=%ld).\n",
         out_total, src_total, src_total*147/160);
   printf("Max abs difference vs independent 48 kHz + scalar FIR: %.3e.\n",
         maxerr);
   printf("Capacity: size each output buffer for the next call's predicted "
         "count (a 2.5 ms packet yields 110 or 111 frames); the largest legal "
         "request is 120 ms = 5292 frames, otherwise OPUS_BUFFER_TOO_SMALL is "
         "returned without consuming any state.\n");
   printf("Latency: purely causal, no look-ahead or tail frames beyond the "
         "codec; the 64-tap FIR spans 64/48000 s (~1.33 ms) of source history, "
         "and clock/FIR state is carried across packet boundaries.\n");

   opus_encoder_destroy(enc);
   opus_decoder_destroy(dec44);
   opus_decoder_destroy(dec48);
   free(pcm48);
   free(pcm44);
   puts("OPUS_44100_DEMO_OK");
   return 0;
}
