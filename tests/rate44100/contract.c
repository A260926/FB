/* Contract tests for the 48 kHz -> 44.1 kHz float decoder entry.
   Registered through task_cases/task_case_count and driven by
   task_support/driver.c; this file intentionally defines no main(). */

#include "task_support/check.h"

#include "opus.h"
#include "opus_multistream.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define R441_TAPS 64
#define R441_PHASES 147
#define R441_PI 3.14159265358979323846

/* ---- Causal scalar reference: exactly the FIR definition from the spec. */
static double ref_h[R441_PHASES][R441_TAPS];
static double ref_sinc(double z)
{
   return z == 0.0 ? 1.0 : sin(R441_PI*z)/(R441_PI*z);
}
static void ref_init(void)
{
   double c = 0.45*147.0/160.0;
   int ph, k;
   for (ph=0; ph<R441_PHASES; ph++)
   {
      double f = (double)ph/R441_PHASES;
      double a[R441_TAPS], sum = 0.0;
      for (k=0; k<R441_TAPS; k++)
      {
         double w = 0.42 - 0.5*cos(2.0*R441_PI*k/63.0)
                        + 0.08*cos(4.0*R441_PI*k/63.0);
         a[k] = 2.0*c*ref_sinc(2.0*c*((double)k + f - 31.5))*w;
         sum += a[k];
      }
      for (k=0; k<R441_TAPS; k++)
         ref_h[ph][k] = a[k]/sum;
   }
}
/* Reference output sample m of channel c, from full interleaved 48 kHz PCM. */
static float ref_sample(const float *x48, long n48, int ch, long m, int c)
{
   static int ref_ready = 0;
   double t = (double)m*160.0/147.0;
   long n = (long)floor(t);
   int ph = (int)((13*(m % R441_PHASES)) % R441_PHASES);
   double acc = 0.0;
   int k;
   if (!ref_ready) { ref_init(); ref_ready = 1; }
   for (k=0; k<R441_TAPS; k++)
   {
      long idx = n - k;
      if (idx >= 0 && idx < n48)
         acc += ref_h[ph][k]*x48[idx*ch+c];
   }
   return (float)acc;
}

/* Asymmetric, non-zero, channel-dependent source signal. */
static float test_signal(long n, int c)
{
   return (float)(0.31*sin(n*0.029 + c*1.3)
                + 0.17*sin(n*0.071 + 0.7*c + 1.1)
                + 0.06*sin(n*0.213 + 0.4)
                + 0.03*sin(n*0.013 + 2.1*c));
}

static opus_int64 clock_outputs(opus_int64 src)
{
   return src <= 0 ? 0 : (src*147)/160;
}

/* ============================ clock group ================================ */

/* Mixed 2.5/5/20 ms packets behind a long 2.5 ms prefix; scalar accuracy,
   prediction, cumulative clock, queries. */
static int clock_mixed(void)
{
   int err;
   OpusEncoder *en = opus_encoder_create(48000, 2,
         OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
   OpusDecoder *d44 = opus_decoder_create(44100, 2, &err);
   OpusDecoder *d48 = opus_decoder_create(48000, 2, &err);
   static const int sizes[] = {
      120,120,120,120,120,120,120,120,120,120,120,120,
      120,120,120,120,120,120,120,120,120,120,120,120,
      240,120,480,240,960,120,240,480,120,120
   };
   enum { NP = (int)(sizeof(sizes)/sizeof(sizes[0])) };
   long cap48 = 0, p;
   float *x48, *y44;
   float in[960*2];
   unsigned char pkt[4000];
   long n48 = 0, n44 = 0, pos = 0;
   double maxerr = 0.0, energy = 0.0;
   opus_int32 rate = 0, dur = 0;

   TASK_CHECK(en && d44 && d48 && err == OPUS_OK);
   for (p=0; p<NP; p++) cap48 += sizes[p];
   x48 = (float*)malloc((size_t)cap48*2*sizeof(float));
   y44 = (float*)malloc((size_t)clock_outputs(cap48)*2*sizeof(float)+16);
   TASK_CHECK(x48 && y44);

   TASK_CHECK(opus_decoder_ctl(d44, OPUS_GET_SAMPLE_RATE(&rate)) == OPUS_OK);
   TASK_CHECK(rate == 44100);

   for (p=0; p<NP; p++)
   {
      int fs = sizes[p], i, nb, pred, q2, r44, r48;
      for (i=0; i<fs*2; i++)
         in[i] = test_signal(pos + i/2, i & 1);
      nb = opus_encode_float(en, in, fs, pkt, sizeof(pkt));
      TASK_CHECK(nb > 0);

      pred = opus_decoder_get_nb_samples(d44, pkt, nb);
      q2   = opus_decoder_get_nb_samples(d44, pkt, nb); /* must not advance */
      r48  = opus_decode_float(d48, pkt, nb, x48+n48*2, 5760, 0);
      r44  = opus_decode_float(d44, pkt, nb, y44+n44*2, 5760, 0);
      TASK_CHECK(r48 == fs);
      TASK_CHECK(pred > 0 && pred == q2 && r44 == pred);
      TASK_CHECK((long)r44 == clock_outputs(n48+fs) - clock_outputs(n48));
      opus_decoder_ctl(d44, OPUS_GET_LAST_PACKET_DURATION(&dur));
      TASK_CHECK(dur == r44);

      for (i=0; i<r44*2; i++)
      {
         TASK_CHECK(isfinite(y44[n44*2+i]));
         energy += (double)y44[n44*2+i]*y44[n44*2+i];
      }
      n48 += r48;
      n44 += r44;
      pos += fs;
   }
   TASK_CHECK(n44 == clock_outputs(n48));
   TASK_CHECK(energy > 1.0);
   for (p=0; p<n44; p++)
   {
      int c;
      for (c=0; c<2; c++)
      {
         double e = fabs((double)ref_sample(x48, n48, 2, p, c)
                         - y44[p*2+c]);
         if (e > maxerr) maxerr = e;
      }
   }
   TASK_CHECK(maxerr <= 3e-6);

   free(x48);
   free(y44);
   opus_encoder_destroy(en);
   opus_decoder_destroy(d44);
   opus_decoder_destroy(d48);
   return 0;
}

/* Continuous 2.5 ms packets: exact per-packet floor remainder, repeated
   queries do not advance, and an undersized buffer fails without committing. */
static int clock_small_packets(void)
{
   int err;
   OpusEncoder *en = opus_encoder_create(48000, 2,
         OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
   OpusDecoder *d44 = opus_decoder_create(44100, 2, &err);
   OpusDecoder *ref = opus_decoder_create(44100, 2, &err);
   enum { NP = 10 };
   unsigned char pkts[NP][2000];
   int plen[NP];
   float in[120*2];
   float out[512*2], good[512*2], refout[512*2];
   long pos = 0;
   int p, dsize;
   char *snap;

   TASK_CHECK(en && d44 && ref);
   for (p=0; p<NP; p++)
   {
      int i;
      for (i=0; i<240; i++)
         in[i] = test_signal(pos + i/2, i & 1);
      plen[p] = opus_encode_float(en, in, 120, pkts[p], sizeof(pkts[p]));
      TASK_CHECK(plen[p] > 0);
      pos += 120;
   }

   for (p=0; p<NP; p++)
   {
      long src_before = (long)p*120;
      int expected = (int)(clock_outputs(src_before+120)
                           - clock_outputs(src_before));
      int q1 = opus_decoder_get_nb_samples(d44, pkts[p], plen[p]);
      int q2 = opus_decoder_get_nb_samples(d44, pkts[p], plen[p]);
      int got;
      TASK_CHECK(q1 == expected && q2 == expected);
      got = opus_decode_float(d44, pkts[p], plen[p], out, 512, 0);
      TASK_CHECK(got == expected);
   }

   /* Capacity one frame short on a fresh decoder: no codec/FIR/clock commit. */
   {
      OpusDecoder *clean = opus_decoder_create(44100, 2, &err);
      int need = opus_decoder_get_nb_samples(ref, pkts[0], plen[0]);
      int rc, g2;
      TASK_CHECK(clean != NULL && need > 0);
      dsize = opus_decoder_get_size(2);
      snap = (char*)malloc((size_t)dsize);
      TASK_CHECK(snap != NULL);
      memcpy(snap, ref, (size_t)dsize);
      rc = opus_decode_float(ref, pkts[0], plen[0], refout, need-1, 0);
      TASK_CHECK(rc == OPUS_BUFFER_TOO_SMALL);
      TASK_CHECK(memcmp(snap, ref, (size_t)dsize) == 0);
      /* Retry with enough room; matches a separate undisturbed decoder. */
      rc = opus_decode_float(ref, pkts[0], plen[0], refout, 512, 0);
      TASK_CHECK(rc == need);
      g2 = opus_decode_float(clean, pkts[0], plen[0], good, 512, 0);
      TASK_CHECK(g2 == need);
      TASK_CHECK(memcmp(good, refout, (size_t)need*2*sizeof(float)) == 0);
      free(snap);
      opus_decoder_destroy(clean);
   }

   opus_encoder_destroy(en);
   opus_decoder_destroy(d44);
   opus_decoder_destroy(ref);
   return 0;
}

/* get_size + memcpy cloning and RESET_STATE behaviour. */
static int clock_clone_reset(void)
{
   int err;
   OpusEncoder *en = opus_encoder_create(48000, 1,
         OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
   OpusDecoder *d = opus_decoder_create(44100, 1, &err);
   OpusDecoder *fresh = opus_decoder_create(44100, 1, &err);
   enum { NP = 12 };
   unsigned char pkts[NP][2000];
   int plen[NP];
   float in[240], a[2048], b[2048], fa[2048], fb[2048];
   long pos = 0;
   int p, dsize, ra, rb, dsize2;
   OpusDecoder *clone;
   opus_int32 gain = 7, got_gain = 0;
   opus_uint32 range_a = 0, range_b = 0;

   TASK_CHECK(en && d && fresh);
   for (p=0; p<NP; p++)
   {
      int i;
      for (i=0; i<240; i++) in[i] = test_signal(pos+i, 0);
      plen[p] = opus_encode_float(en, in, 240, pkts[p], sizeof(pkts[p]));
      TASK_CHECK(plen[p] > 0);
      pos += 240;
      TASK_CHECK(opus_decode_float(d, pkts[p], plen[p], a, 2048, 0) > 0);
   }

   /* A decoder block obtained from get_size and memcpy'd keeps playing. */
   dsize = opus_decoder_get_size(1);
   clone = (OpusDecoder*)malloc((size_t)dsize);
   TASK_CHECK(clone != NULL);
   memcpy(clone, d, (size_t)dsize);
   ra = opus_decode_float(clone, pkts[0], plen[0], a, 2048, 0);
   rb = opus_decode_float(d,     pkts[0], plen[0], b, 2048, 0);
   TASK_CHECK(ra == rb && ra > 0);
   TASK_CHECK(memcmp(a, b, (size_t)ra*sizeof(float)) == 0);
   free(clone);

   /* Gain is codec state and must keep applying in 44.1 kHz mode. */
   TASK_CHECK(opus_decoder_ctl(d, OPUS_SET_GAIN(gain)) == OPUS_OK);
   TASK_CHECK(opus_decoder_ctl(d, OPUS_GET_GAIN(&got_gain)) == OPUS_OK);
   TASK_CHECK(got_gain == gain);
   ra = opus_decode_float(d, pkts[1], plen[1], a, 2048, 0);
   TASK_CHECK(ra > 0);
   opus_decoder_ctl(d, OPUS_GET_FINAL_RANGE(&range_a));
   TASK_CHECK(range_a != 0);

   /* RESET_STATE clears codec and FIR/clock back to a fresh instance.  Gain
      deliberately survives a reset and keeps applying, so mirror it on the
      fresh reference before comparing. */
   TASK_CHECK(opus_decoder_ctl(d, OPUS_RESET_STATE) == OPUS_OK);
   TASK_CHECK(opus_decoder_ctl(d, OPUS_GET_GAIN(&got_gain)) == OPUS_OK);
   TASK_CHECK(got_gain == gain);
   TASK_CHECK(opus_decoder_ctl(fresh, OPUS_SET_GAIN(gain)) == OPUS_OK);
   ra = opus_decode_float(d,     pkts[0], plen[0], fa, 2048, 0);
   rb = opus_decode_float(fresh, pkts[0], plen[0], fb, 2048, 0);
   TASK_CHECK(ra == rb && ra > 0);
   TASK_CHECK(memcmp(fa, fb, (size_t)ra*sizeof(float)) == 0);
   opus_decoder_ctl(d,     OPUS_GET_FINAL_RANGE(&range_a));
   opus_decoder_ctl(fresh, OPUS_GET_FINAL_RANGE(&range_b));
   TASK_CHECK(range_a == range_b);
   dsize2 = opus_decoder_get_size(1);
   TASK_CHECK(dsize2 == dsize);

   opus_encoder_destroy(en);
   opus_decoder_destroy(d);
   opus_decoder_destroy(fresh);
   return 0;
}

/* ========================== recovery group =============================== */

/* PLC request validation, counts and scalar-correct concealment. */
static int recovery_plc(void)
{
   int err;
   OpusEncoder *en = opus_encoder_create(48000, 1,
         OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
   OpusDecoder *d44 = opus_decoder_create(44100, 1, &err);
   OpusDecoder *d48 = opus_decoder_create(48000, 1, &err);
   OpusDecoder *fresh = opus_decoder_create(44100, 1, &err);
   enum { NP = 10 };
   unsigned char pkts[NP][2000];
   int plen[NP];
   float in[480];
   /* Real frames plus 10 ms..120 ms PLC: 10*480 + 480*78 source frames. */
   float *x48 = (float*)malloc((size_t)(480*(10+78)+8)*sizeof(float));
   float *y44 = (float*)malloc((size_t)(441*(10+78)+8)*sizeof(float));
   long pos = 0, n48 = 0, n44 = 0;
   int p, k, rc, dsize;
   char *snap;
   double maxerr;
   opus_int32 dur = 0;

   TASK_CHECK(en && d44 && d48 && fresh && x48 && y44);
   for (p=0; p<NP; p++)
   {
      int i, r44;
      for (i=0; i<480; i++) in[i] = test_signal(pos+i, 0);
      plen[p] = opus_encode_float(en, in, 480, pkts[p], sizeof(pkts[p]));
      TASK_CHECK(plen[p] > 0);
      pos += 480;
      TASK_CHECK(opus_decode_float(d48, pkts[p], plen[p],
                                   x48+n48, 5760, 0) == 480);
      n48 += 480;
      r44 = opus_decode_float(d44, pkts[p], plen[p], y44+n44, 5760, 0);
      TASK_CHECK(r44 > 0);
      n44 = clock_outputs(n48);
   }

   /* A fresh decoder only has zeros to conceal with. */
   {
      float z[441];
      rc = opus_decode_float(fresh, NULL, 0, z, 441, 0);
      TASK_CHECK(rc == 441);
      for (p=0; p<441; p++) TASK_CHECK(fabs(z[p]) < 1e-12);
   }

   /* Valid PLC sizes: 441*k for k=1..12; build one continuous 48k reference. */
   for (k=1; k<=12; k++)
   {
      int req = 441*k;
      float *buf = (float*)malloc((size_t)req*sizeof(float));
      float *b48 = (float*)malloc((size_t)(480*k)*sizeof(float));
      int r44, r48;
      TASK_CHECK(buf && b48);
      r44 = opus_decode_float(d44, NULL, 0, buf, req, 0);
      r48 = opus_decode_float(d48, NULL, 0, b48, 480*k, 0);
      TASK_CHECK(r44 == req && r48 == 480*k);
      memcpy(x48+n48, b48, (size_t)r48*sizeof(float));
      memcpy(y44+n44, buf, (size_t)r44*sizeof(float));
      n48 += r48;
      n44 += r44;
      opus_decoder_ctl(d44, OPUS_GET_LAST_PACKET_DURATION(&dur));
      TASK_CHECK(dur == req);
      free(buf);
      free(b48);
   }
   TASK_CHECK(n44 == clock_outputs(n48));

   /* Every concealed output still matches the scalar 48k+FIR reference. */
   maxerr = 0.0;
   for (p=0; p<n44; p++)
   {
      double e = fabs((double)ref_sample(x48, n48, 1, p, 0) - y44[p]);
      if (e > maxerr) maxerr = e;
   }
   TASK_CHECK(maxerr <= 3e-6);

   /* Invalid PLC sizes are rejected without touching the state. */
   dsize = opus_decoder_get_size(1);
   snap = (char*)malloc((size_t)dsize);
   TASK_CHECK(snap != NULL);
   {
      static const int bad[] = {0, 440, 442, 441*13, 441*12+1};
      size_t i;
      for (i=0; i<sizeof(bad)/sizeof(bad[0]); i++)
      {
         float guard[16];
         memcpy(snap, d44, (size_t)dsize);
         rc = opus_decode_float(d44, NULL, 0, guard, bad[i], 0);
         TASK_CHECK(rc == OPUS_BAD_ARG);
         TASK_CHECK(memcmp(snap, d44, (size_t)dsize) == 0);
      }
   }
   free(snap);
   free(x48);
   free(y44);

   opus_encoder_destroy(en);
   opus_decoder_destroy(d44);
   opus_decoder_destroy(d48);
   opus_decoder_destroy(fresh);
   return 0;
}

/* In-band FEC: real redundancy is recovered, then normal decoding continues
   on the shared history without advancing an extra packet's worth of time. */
static int recovery_fec(void)
{
   int err;
   OpusEncoder *en = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
   OpusDecoder *d44 = opus_decoder_create(44100, 1, &err);
   OpusDecoder *d48 = opus_decoder_create(48000, 1, &err);
   OpusDecoder *dplc = opus_decoder_create(44100, 1, &err);
   enum { NP = 24 };
   unsigned char pkts[NP][2000];
   int plen[NP];
   float in[480];
   float *y44 = (float*)malloc((size_t)(NP*441+8)*sizeof(float));
   float *yplc = (float*)malloc((size_t)(NP*441+8)*sizeof(float));
   float *x48 = (float*)malloc((size_t)(NP*480+8)*sizeof(float));
   long n44 = 0, nplc = 0, n48 = 0, pos = 0;
   int p, target = -1;
   double fec_energy = 0.0, plc_energy = 0.0, dist = 0.0, maxerr = 0.0;

   TASK_CHECK(en && d44 && d48 && dplc && y44 && yplc && x48);
   opus_encoder_ctl(en, OPUS_SET_INBAND_FEC(1));
   opus_encoder_ctl(en, OPUS_SET_PACKET_LOSS_PERC(50));
   opus_encoder_ctl(en, OPUS_SET_VBR(1));
   opus_encoder_ctl(en, OPUS_SET_BITRATE(24000));
   opus_encoder_ctl(en, 11002, 1000); /* OPUS_SET_FORCE_MODE(MODE_SILK_ONLY) */

   for (p=0; p<NP; p++)
   {
      int i;
      for (i=0; i<480; i++) in[i] = test_signal(pos+i, 0);
      pos += 480;
      plen[p] = opus_encode_float(en, in, 480, pkts[p], sizeof(pkts[p]));
      TASK_CHECK(plen[p] > 0);
   }
   for (p=1; p<NP; p++)
   {
      if (opus_packet_has_lbrr(pkts[p], plen[p]) > 0) { target = p; break; }
   }
   TASK_CHECK(target > 0); /* redundancy really exists in the stream */

   for (p=0; p<NP; p++)
   {
      float b44[2048], bp[2048], b48[2048];
      int r44 = 0, rp = 0, r48 = 0;
      if (p == target-1)
      {
         /* Lost packet: PLC stream conceals; FEC stream waits for packet p. */
         rp = opus_decode_float(dplc, NULL, 0, bp, 441, 0);
         TASK_CHECK(rp == 441);
         memcpy(yplc+nplc, bp, 441*sizeof(float));
         nplc += 441;
      } else if (p == target) {
         int fec, fec48, i;
         /* FEC recovers the lost frame; then the current packet decodes. */
         fec   = opus_decode_float(d44, pkts[p], plen[p], b44, 441, 1);
         fec48 = opus_decode_float(d48, pkts[p], plen[p], b48, 480, 1);
         TASK_CHECK(fec == 441 && fec48 == 480);
         memcpy(y44+n44, b44, fec*sizeof(float));
         memcpy(x48+n48, b48, fec48*sizeof(float));
         n44 += fec; n48 += fec48;

         r44 = opus_decode_float(d44, pkts[p], plen[p], b44, 441, 0);
         r48 = opus_decode_float(d48, pkts[p], plen[p], b48, 480, 0);
         rp  = opus_decode_float(dplc, pkts[p], plen[p], bp, 441, 0);
         TASK_CHECK(r44 == 441 && r48 == 480 && rp == 441);
         /* Actual redundancy content, not just an encoder option. */
         for (i=0; i<441; i++)
         {
            double a = y44[(n44-441)+i] - yplc[(target-1)*441+i];
            dist += a*a;
            fec_energy += (double)y44[(n44-441)+i]*y44[(n44-441)+i];
            plc_energy += (double)yplc[(target-1)*441+i]*yplc[(target-1)*441+i];
         }
      } else {
         r44 = opus_decode_float(d44, pkts[p], plen[p], b44, 441, 0);
         r48 = opus_decode_float(d48, pkts[p], plen[p], b48, 480, 0);
         rp  = opus_decode_float(dplc, pkts[p], plen[p], bp, 441, 0);
         TASK_CHECK(r44 == 441 && r48 == 480 && rp == 441);
      }
      if (p != target-1)
      {
         memcpy(y44+n44, b44, r44*sizeof(float));
         memcpy(x48+n48, b48, r48*sizeof(float));
         memcpy(yplc+nplc, bp, rp*sizeof(float));
         n44 += r44; n48 += r48; nplc += rp;
      }
   }
   TASK_CHECK(n44 == nplc);
   TASK_CHECK(n44 == clock_outputs((long)NP*480));
   TASK_CHECK(n48 == (long)NP*480);
   /* The recovered slot carries real redundancy: non-zero and distinct from
      pure PLC concealment (verified on actual output, not encoder options). */
   TASK_CHECK(fec_energy > 1e-6);
   TASK_CHECK(dist > 1e-7);
   TASK_CHECK(fabs(fec_energy - plc_energy) > 1e-6);

   for (p=0; p<n44; p++)
   {
      double e = fabs((double)ref_sample(x48, n48, 1, p, 0) - y44[p]);
      if (e > maxerr) maxerr = e;
   }
   TASK_CHECK(maxerr <= 3e-6);

   free(y44); free(yplc); free(x48);
   opus_encoder_destroy(en);
   opus_decoder_destroy(d44);
   opus_decoder_destroy(d48);
   opus_decoder_destroy(dplc);
   return 0;
}

/* A corrupted packet must be rejected and leave codec/FIR/clock untouched. */
static int recovery_bad_packet(void)
{
   int err;
   OpusEncoder *en = opus_encoder_create(48000, 2,
         OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
   OpusDecoder *d = opus_decoder_create(44100, 2, &err);
   enum { NP = 8 };
   unsigned char pkts[NP][2000], bad[2000];
   int plen[NP];
   float in[240], out[2048];
   long pos = 0;
   int p, dsize, rc;
   char *snap;

   TASK_CHECK(en && d);
   for (p=0; p<NP; p++)
   {
      int i;
      for (i=0; i<240*2; i++) in[i] = test_signal(pos+i/2, i&1);
      pos += 240;
      plen[p] = opus_encode_float(en, in, 240, pkts[p], sizeof(pkts[p]));
      TASK_CHECK(plen[p] > 0);
      if (p < NP-1)
         TASK_CHECK(opus_decode_float(d, pkts[p], plen[p], out, 2048, 0) > 0);
   }

   dsize = opus_decoder_get_size(2);
   snap = (char*)malloc((size_t)dsize);
   TASK_CHECK(snap != NULL);
   /* Deterministically malformed packet: code 3 advertises 48 frames but the
      payload is empty, so parsing must reject it before any decode. */
   bad[0] = (unsigned char)((pkts[NP-1][0] & 0xFC) | 0x03);
   bad[1] = 47;
   memcpy(snap, d, (size_t)dsize);
   rc = opus_decode_float(d, bad, 2, out, 2048, 0);
   TASK_CHECK(rc < 0);
   TASK_CHECK(memcmp(snap, d, (size_t)dsize) == 0);

   /* The untouched decoder still decodes the good packet normally. */
   {
      int pred = opus_decoder_get_nb_samples(d, pkts[NP-1], plen[NP-1]);
      rc = opus_decode_float(d, pkts[NP-1], plen[NP-1], out, 2048, 0);
      TASK_CHECK(rc > 0);
      TASK_CHECK(rc == pred);
   }

   /* Negative length with a non-NULL packet is BAD_ARG and leaves no trace. */
   memcpy(snap, d, (size_t)dsize);
   TASK_CHECK(opus_decode_float(d, bad, -1, out, 2048, 0) == OPUS_BAD_ARG);
   TASK_CHECK(memcmp(snap, d, (size_t)dsize) == 0);

   /* Integer entries are unimplemented and must not change the state. */
   {
      opus_int16 s16[2048];
      opus_int32 s24[2048];
      TASK_CHECK(opus_decode(d, pkts[NP-1], plen[NP-1], s16, 2048, 0)
                  == OPUS_UNIMPLEMENTED);
      TASK_CHECK(opus_decode24(d, pkts[NP-1], plen[NP-1], s24, 2048, 0)
                  == OPUS_UNIMPLEMENTED);
      TASK_CHECK(memcmp(snap, d, (size_t)dsize) == 0);
   }
   free(snap);

   opus_encoder_destroy(en);
   opus_decoder_destroy(d);
   return 0;
}

/* ========================== multistream group ============================ */

#define MS_CH 6
static const unsigned char ms_map[MS_CH] = {2, 1, 255, 0, 3, 2};

/* Coupled + two mono streams, repeated mapping and a 255 silent channel,
   checked against the independently remapped 48 kHz reference. */
static int multistream_mapping(void)
{
   int err;
   OpusMSEncoder *me = opus_multistream_encoder_create(48000, MS_CH, 3, 1,
         ms_map, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
   OpusMSDecoder *d44 = opus_multistream_decoder_create(44100, MS_CH, 3, 1,
         ms_map, &err);
   OpusMSDecoder *d48 = opus_multistream_decoder_create(48000, MS_CH, 3, 1,
         ms_map, &err);
   static const int sizes[] = {120, 120, 240, 120, 480, 120, 240, 120, 120};
   enum { NP = 9 };
   long cap = 0, p;
   float *x48, *y44;
   float in[960*MS_CH];
   unsigned char pkt[8000];
   long n48 = 0, n44 = 0, pos = 0;
   double maxerr = 0.0, sentinel = 0.0, energy = 0.0;
   opus_int32 rate = 0;

   TASK_CHECK(me && d44 && d48 && err == OPUS_OK);
   TASK_CHECK(opus_multistream_decoder_ctl(d44,
         OPUS_GET_SAMPLE_RATE(&rate)) == OPUS_OK && rate == 44100);
   for (p=0; p<NP; p++) cap += sizes[p];
   x48 = (float*)malloc((size_t)cap*MS_CH*sizeof(float));
   y44 = (float*)malloc((size_t)clock_outputs(cap)*MS_CH*sizeof(float)+16);
   TASK_CHECK(x48 && y44);

   for (p=0; p<NP; p++)
   {
      int fs = sizes[p], i, nb, r48, r44;
      long want;
      for (i=0; i<fs*MS_CH; i++)
         in[i] = test_signal(pos + i/MS_CH, i % MS_CH);
      nb = opus_multistream_encode_float(me, in, fs, pkt, sizeof(pkt));
      TASK_CHECK(nb > 0);
      r48 = opus_multistream_decode_float(d48, pkt, nb,
            x48+n48*MS_CH, 5760, 0);
      r44 = opus_multistream_decode_float(d44, pkt, nb,
            y44+n44*MS_CH, 5760, 0);
      TASK_CHECK(r48 == fs);
      want = clock_outputs(n48+fs) - clock_outputs(n48);
      TASK_CHECK(r44 == (int)want);
      n48 += r48;
      n44 += r44;
      pos += fs;
   }
   TASK_CHECK(n44 == clock_outputs(n48));

   for (p=0; p<MS_CH; p++)
   {
      long m;
      if (ms_map[p] == 255)
      {
         for (m=0; m<n44; m++)
         {
            double v = fabs(y44[m*MS_CH+p]);
            if (v > sentinel) sentinel = v;
         }
      } else {
         for (m=0; m<n44; m++)
         {
            float ref = ref_sample(x48, n48, MS_CH, m, (int)p);
            double e = fabs((double)ref - y44[m*MS_CH+p]);
            if (e > maxerr) maxerr = e;
            energy += (double)y44[m*MS_CH+p]*y44[m*MS_CH+p];
         }
      }
   }
   TASK_CHECK(maxerr <= 3e-6);
   TASK_CHECK(sentinel < 1e-12);
   TASK_CHECK(energy > 1.0);

   free(x48); free(y44);
   opus_multistream_encoder_destroy(me);
   opus_multistream_decoder_destroy(d44);
   opus_multistream_decoder_destroy(d48);
   return 0;
}

/* Small-packet remainder, capacity failure isolation, unimplemented integer
   entries and initial-PLC silence for multistream. */
static int multistream_guard(void)
{
   int err;
   OpusMSEncoder *me = opus_multistream_encoder_create(48000, MS_CH, 3, 1,
         ms_map, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
   OpusMSDecoder *d = opus_multistream_decoder_create(44100, MS_CH, 3, 1,
         ms_map, &err);
   OpusMSDecoder *probe = opus_multistream_decoder_create(44100, MS_CH, 3, 1,
         ms_map, &err);
   enum { NP = 6 };
   unsigned char pkts[NP][4000];
   int plen[NP];
   float in[120*MS_CH], tmp[5760*MS_CH];
   long pos = 0;
   int p, need, rc, dsize;
   char *snap;
   opus_int16 sbuf[5760*MS_CH];
   opus_int32 i24[5760*MS_CH];
   float plc[441*MS_CH];
   double pmax = 0.0;

   TASK_CHECK(me && d && probe);
   for (p=0; p<NP; p++)
   {
      int i;
      for (i=0; i<120*MS_CH; i++)
         in[i] = test_signal(pos+i/MS_CH, i % MS_CH);
      pos += 120;
      plen[p] = opus_multistream_encode_float(me, in, 120, pkts[p],
            sizeof(pkts[p]));
      TASK_CHECK(plen[p] > 0);
   }

   /* Run the probe to learn the clocked count for the first small packet. */
   need = opus_multistream_decode_float(probe, pkts[0], plen[0], tmp, 5760, 0);
   TASK_CHECK(need > 0 && need < 120);
   dsize = opus_multistream_decoder_get_size(3, 1);
   snap = (char*)malloc((size_t)dsize);
   TASK_CHECK(snap != NULL);
   memcpy(snap, d, (size_t)dsize);
   rc = opus_multistream_decode_float(d, pkts[0], plen[0], tmp, need-1, 0);
   TASK_CHECK(rc == OPUS_BUFFER_TOO_SMALL);
   TASK_CHECK(memcmp(snap, d, (size_t)dsize) == 0);
   rc = opus_multistream_decode_float(d, pkts[0], plen[0], tmp, 5760, 0);
   TASK_CHECK(rc == need);
   free(snap);

   /* Integer entries are unimplemented and must not change the state. */
   dsize = opus_multistream_decoder_get_size(3, 1);
   snap = (char*)malloc((size_t)dsize);
   TASK_CHECK(snap != NULL);
   memcpy(snap, d, (size_t)dsize);
   TASK_CHECK(opus_multistream_decode(d, pkts[1], plen[1], sbuf, 5760, 0)
               == OPUS_UNIMPLEMENTED);
   TASK_CHECK(opus_multistream_decode24(d, pkts[1], plen[1], i24, 5760, 0)
               == OPUS_UNIMPLEMENTED);
   TASK_CHECK(memcmp(snap, d, (size_t)dsize) == 0);
   free(snap);

   /* PLC on a brand-new decoder is silence at the 44.1 kHz frame count. */
   {
      OpusMSDecoder *fresher = opus_multistream_decoder_create(44100, MS_CH,
            3, 1, ms_map, &err);
      TASK_CHECK(fresher != NULL);
      rc = opus_multistream_decode_float(fresher, NULL, 0, plc, 441, 0);
      TASK_CHECK(rc == 441);
      for (p=0; p<441*MS_CH; p++)
      {
         double v = fabs(plc[p]);
         if (v > pmax) pmax = v;
      }
      TASK_CHECK(pmax < 1e-12);
      opus_multistream_decoder_destroy(fresher);
   }

   opus_multistream_encoder_destroy(me);
   opus_multistream_decoder_destroy(d);
   opus_multistream_decoder_destroy(probe);
   return 0;
}

/* ============================ registration ============================== */

const struct task_case task_cases[] = {
   {"clock", "mixed_packets_scalar_reference", clock_mixed},
   {"clock", "small_packet_remainder_and_capacity", clock_small_packets},
   {"clock", "clone_and_reset", clock_clone_reset},
   {"recovery", "plc_sizes_and_reference", recovery_plc},
   {"recovery", "inband_fec_redundancy", recovery_fec},
   {"recovery", "corrupt_packet_no_commit", recovery_bad_packet},
   {"multistream", "coupled_mono_repeat_sentinel", multistream_mapping},
   {"multistream", "capacity_integer_plc_guards", multistream_guard},
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
