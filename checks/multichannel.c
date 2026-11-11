/* Multi-channel AEC and residual echo preprocessing checks.
 *
 * Covers:
 *  - asynchronous capture/playback vs explicit speex_echo_cancellation()
 *    references for 1x2, 2x1 and 2x2 channel configurations;
 *  - playback buffer underrun (pass-through, no adaptation, sentinels),
 *    auto-fill on underrun, frame drop when full and in-place rec/out;
 *  - per-microphone residual echo selection in the preprocessor, including
 *    binding/channel validation, channel swapping, query ordering and
 *    isolation between microphone channels.
 */
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { F = 64, FILTER = 256, NFRAMES = 300, PPFRAMES = 250 };
#define MAXC 2
#define MAXK 2

static unsigned rng_state;
static unsigned rng_next(void)
{
   rng_state = rng_state*1664525u + 1013904223u;
   return rng_state;
}
static short rng_signed(int amp)
{
   return (short)((int)(rng_next()%(unsigned)(2*amp+1)) - amp);
}
static short sat16(long v)
{
   if (v > 32767)
      return 32767;
   if (v < -32768)
      return -32768;
   return (short)v;
}

/* Distinct deterministic echo path for every (mic, speaker) pair. */
static float echo_coef(int c, int k, int j)
{
   int pair = c*MAXK + k;
   static const float base[3] = {0.28f, 0.16f, 0.08f};
   return base[j] + 0.04f*pair + 0.01f*j;
}

/* Generates interleaved playback frames, planar speaker references and
 * interleaved microphone frames with a different echo path per pair.
 * The echo is built from playback frames t-2..t-4 so it is aligned with the
 * two-frame playback delay used by speex_echo_capture(). */
static void gen_signals(int C, int K, int frames, short *play,
                        short ref[][NFRAMES*F], short *mic,
                        int silent_mic1, int quiet_ref)
{
   unsigned seed[MAXK] = {11u, 97u};
   int t, i, c, k, j;
   for (t=0;t<frames;t++)
   {
      for (i=0;i<F;i++)
      {
         for (k=0;k<K;k++)
         {
            short v;
            seed[k] = seed[k]*1664525u + 1013904223u;
            v = quiet_ref ? 0 : (short)((int)(seed[k]%5001u) - 2500);
            ref[k][t*F+i] = v;
            play[(t*F+i)*K+k] = v;
         }
      }
   }
   rng_state = 7;
   for (t=0;t<frames;t++)
   {
      for (i=0;i<F;i++)
      {
         for (c=0;c<C;c++)
         {
            long v = (silent_mic1 && c==1) ? 0 : rng_signed(60);
            if (!(silent_mic1 && c==1))
            {
               for (k=0;k<K;k++)
               {
                  for (j=0;j<3;j++)
                  {
                     long idx = (long)(t-2-j)*F + i;
                     if (idx >= 0)
                        v += (long)(echo_coef(c,k,j)*ref[k][idx]);
                  }
               }
            }
            mic[(t*F+i)*C+c] = sat16(v);
         }
      }
   }
}

static const short *delayed_ref(int t, short hist[2][F*MAXK], const short *zero)
{
   return t < 2 ? zero : hist[t&1];
}

/* Asynchronous capture/playback must match the explicit two-frame-delayed
 * reference path, sample for sample, for every channel configuration. */
static void test_async_equivalence(int C, int K)
{
   int t;
   short *play = (short*)malloc(NFRAMES*F*K*sizeof(short));
   short *mic = (short*)malloc(NFRAMES*F*C*sizeof(short));
   short ref[MAXK][NFRAMES*F];
   short hist[2][F*MAXK];
   short zero[F*MAXK];
   short oa[F*MAXC], od[F*MAXC];
   double e_in = 0., e_out = 0.;
   SpeexEchoState *a, *d;

   memset(ref, 0, sizeof(ref));
   memset(hist, 0, sizeof(hist));
   memset(zero, 0, sizeof(zero));
   gen_signals(C, K, NFRAMES, play, ref, mic, 0, 0);

   a = speex_echo_state_init_mc(F, FILTER, C, K);
   d = speex_echo_state_init_mc(F, FILTER, C, K);
   for (t=0;t<NFRAMES;t++)
   {
      const short *m = mic + t*C*F;
      const short *r = delayed_ref(t, hist, zero);
      speex_echo_capture(a, m, oa);
      speex_echo_cancellation(d, m, r, od);
      assert(memcmp(oa, od, C*F*sizeof(short)) == 0);
      speex_echo_playback(a, play + t*K*F);
      memcpy(hist[t&1], play + t*K*F, K*F*sizeof(short));
      if (t >= NFRAMES-15)
      {
         int i;
         for (i=0;i<C*F;i++)
         {
            e_in += (double)m[i]*m[i];
            e_out += (double)oa[i]*oa[i];
         }
      }
   }
   /* The adaptive filter must actually cancel the simulated echo. */
   assert(e_in > 0. && e_out < 0.25*e_in);
   speex_echo_state_destroy(a);
   speex_echo_state_destroy(d);
   free(play);
   free(mic);
   printf("AEC async/direct equivalence %dx%d OK\n", C, K);
}

/* Underrun: all microphone samples pass through, adaptation is untouched and
 * samples beyond the frame keep their previous content. */
static void test_underrun_copy(void)
{
   static const int cfgs[][2] = {{1,1},{2,1},{1,2},{2,2}};
   int q;
   for (q=0;q<4;q++)
   {
      int C = cfgs[q][0], K = cfgs[q][1];
      int i;
      short in[F*MAXC], out[4*F];
      SpeexEchoState *st = speex_echo_state_init_mc(F, FILTER, C, K);
      for (i=0;i<C*F;i++)
         in[i] = (short)(20+i);
      speex_echo_capture(st, in, out);
      speex_echo_capture(st, in, out);
      for (i=0;i<4*F;i++)
         out[i] = -12345;
      speex_echo_capture(st, in, out);
      for (i=0;i<C*F;i++)
         assert(out[i] == in[i]);
      for (i=C*F;i<4*F;i++)
         assert(out[i] == -12345);
      speex_echo_state_destroy(st);
   }
   printf("AEC underrun pass-through OK\n");
}

/* An underrun capture must not move the adaptation state: two states driven
 * identically except for the extra empty capture keep producing identical
 * output on every later frame. */
static void test_underrun_neutral(void)
{
   int t, i;
   short *play = (short*)malloc(80*F*2*sizeof(short));
   short *mic = (short*)malloc(80*F*2*sizeof(short));
   short ref[MAXK][NFRAMES*F];
   short in[2*F], oa[2*F], ob[2*F];
   SpeexEchoState *a = speex_echo_state_init_mc(F, FILTER, 2, 2);
   SpeexEchoState *b = speex_echo_state_init_mc(F, FILTER, 2, 2);

   memset(ref, 0, sizeof(ref));
   for (i=0;i<2*F;i++)
      in[i] = (short)(30+i);
   speex_echo_capture(a, in, oa);
   speex_echo_capture(a, in, oa);
   speex_echo_capture(a, in, oa); /* underrun, must be a no-op for adaptation */
   speex_echo_capture(b, in, ob);
   speex_echo_capture(b, in, ob);

   gen_signals(2, 2, 80, play, ref, mic, 0, 0);
   for (t=0;t<80;t++)
   {
      const short *m = mic + t*2*F;
      speex_echo_capture(a, m, oa);
      speex_echo_capture(b, m, ob);
      assert(memcmp(oa, ob, sizeof(oa)) == 0);
      speex_echo_playback(a, play + t*2*F);
      speex_echo_playback(b, play + t*2*F);
   }
   speex_echo_state_destroy(a);
   speex_echo_state_destroy(b);
   free(play);
   free(mic);
   printf("AEC underrun adaptation neutrality OK\n");
}

/* Queue model: with no capture in between, the first playback frame is
 * duplicated by the auto-fill, later frames fill up to capacity and any
 * further frame is dropped; exhaustion ends in raw pass-through. */
static void test_playback_queue(void)
{
   int i;
   short m[2*F], zero[2*F], p0[2*F], p1[2*F], p2[2*F], p3[2*F];
   short oa[2*F], od[2*F];
   SpeexEchoState *a = speex_echo_state_init_mc(F, FILTER, 2, 2);
   SpeexEchoState *d = speex_echo_state_init_mc(F, FILTER, 2, 2);

   for (i=0;i<2*F;i++)
   {
      m[i] = (short)(i*13 - 400);
      zero[i] = 0;
      p0[i] = (short)(500 + (i%7)*11);
      p1[i] = (short)(-700 - (i%5)*13);
      p2[i] = (short)(12000 + i);
      p3[i] = (short)(-12000 - i);
   }

   /* First two captures consume the zero references. */
   speex_echo_capture(a, m, oa);
   speex_echo_cancellation(d, m, zero, od);
   assert(memcmp(oa, od, sizeof(oa)) == 0);
   speex_echo_capture(a, m, oa);
   speex_echo_cancellation(d, m, zero, od);
   assert(memcmp(oa, od, sizeof(oa)) == 0);

   /* p0 is auto-filled once, p1 fills the last slot, p2 and p3 are dropped. */
   speex_echo_playback(a, p0);
   speex_echo_playback(a, p1);
   speex_echo_playback(a, p2);
   speex_echo_playback(a, p3);

   speex_echo_capture(a, m, oa);
   speex_echo_cancellation(d, m, p0, od);
   assert(memcmp(oa, od, sizeof(oa)) == 0);
   speex_echo_capture(a, m, oa);
   speex_echo_cancellation(d, m, p0, od);
   assert(memcmp(oa, od, sizeof(oa)) == 0);
   speex_echo_capture(a, m, oa);
   speex_echo_cancellation(d, m, p1, od);
   assert(memcmp(oa, od, sizeof(oa)) == 0);

   /* Buffer exhausted: pass-through again, adaptation frozen. */
   speex_echo_capture(a, m, oa);
   for (i=0;i<2*F;i++)
      assert(oa[i] == m[i]);

   speex_echo_state_destroy(a);
   speex_echo_state_destroy(d);
   printf("AEC playback queue auto-fill/drop OK\n");
}

/* In-place rec/out buffers give the same result as separate buffers. */
static void test_inplace(void)
{
   int t;
   short *play = (short*)malloc(60*F*2*sizeof(short));
   short *mic = (short*)malloc(60*F*2*sizeof(short));
   short ref[MAXK][NFRAMES*F];
   short ba[2*F], bb[2*F], ref_out[2*F];
   SpeexEchoState *a = speex_echo_state_init_mc(F, FILTER, 2, 2);
   SpeexEchoState *b = speex_echo_state_init_mc(F, FILTER, 2, 2);

   memset(ref, 0, sizeof(ref));
   gen_signals(2, 2, 60, play, ref, mic, 0, 0);
   for (t=0;t<60;t++)
   {
      const short *m = mic + t*2*F;
      memcpy(ba, m, sizeof(ba));
      memcpy(bb, m, sizeof(bb));
      speex_echo_capture(a, ba, ba);
      speex_echo_capture(b, bb, ref_out);
      assert(memcmp(ba, ref_out, sizeof(ba)) == 0);
      speex_echo_playback(a, play + t*2*F);
      speex_echo_playback(b, play + t*2*F);
   }
   speex_echo_state_destroy(a);
   speex_echo_state_destroy(b);

   /* In-place underrun leaves the input frame untouched. */
   a = speex_echo_state_init_mc(F, FILTER, 2, 2);
   for (t=0;t<2*F;t++)
      ba[t] = (short)(77 + t);
   speex_echo_capture(a, ba, ref_out);
   speex_echo_capture(a, ba, ref_out);
   speex_echo_capture(a, ba, ba);
   for (t=0;t<2*F;t++)
      assert(ba[t] == (short)(77 + t));
   speex_echo_state_destroy(a);

   free(play);
   free(mic);
   printf("AEC in-place buffers OK\n");
}

/* SET/GET_ECHO_CHANNEL validation and echo state binding compatibility. */
static void test_preprocess_controls(void)
{
   int v;
   void *ptr;
   SpeexPreprocessState *p = speex_preprocess_state_init(F, 8000);
   SpeexEchoState *aec2 = speex_echo_state_init_mc(F, FILTER, 2, 2);
   SpeexEchoState *aec1 = speex_echo_state_init(F, FILTER);

   v = 99;
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_GET_ECHO_CHANNEL, &v) == 0);
   assert(v == 0);

   v = -1;
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_CHANNEL, &v) == -1);
   v = 99;
   speex_preprocess_ctl(p, SPEEX_PREPROCESS_GET_ECHO_CHANNEL, &v);
   assert(v == 0);

   /* Unbound: any non-negative index is accepted. */
   v = 1;
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_CHANNEL, &v) == 0);
   v = 5;
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_CHANNEL, &v) == 0);

   /* Binding an incompatible state fails and leaves the binding empty. */
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_STATE, aec2) == -1);
   ptr = (void*)0x1;
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_GET_ECHO_STATE, &ptr) == 0);
   assert(ptr == NULL);

   v = 1;
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_CHANNEL, &v) == 0);
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_STATE, aec2) == 0);
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_GET_ECHO_STATE, &ptr) == 0);
   assert(ptr == aec2);

   /* Bound: out-of-range and negative indices fail and keep the old choice. */
   v = 2;
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_CHANNEL, &v) == -1);
   speex_preprocess_ctl(p, SPEEX_PREPROCESS_GET_ECHO_CHANNEL, &v);
   assert(v == 1);
   v = -3;
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_CHANNEL, &v) == -1);
   speex_preprocess_ctl(p, SPEEX_PREPROCESS_GET_ECHO_CHANNEL, &v);
   assert(v == 1);
   v = 0;
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_CHANNEL, &v) == 0);
   speex_preprocess_ctl(p, SPEEX_PREPROCESS_GET_ECHO_CHANNEL, &v);
   assert(v == 0);
   /* Channel 0 exists in a mono state too: this replacement is compatible. */
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_STATE, aec1) == 0);
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_GET_ECHO_STATE, &ptr) == 0);
   assert(ptr == aec1);

   /* Selecting the second microphone while bound to the mono state is rejected. */
   v = 1;
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_CHANNEL, &v) == -1);
   speex_preprocess_ctl(p, SPEEX_PREPROCESS_GET_ECHO_CHANNEL, &v);
   assert(v == 0);

   /* With channel 1 selected on the stereo state, rebinding to the mono
    * state is incompatible and the old stereo binding must survive. */
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_STATE, aec2) == 0);
   v = 1;
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_CHANNEL, &v) == 0);
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_STATE, aec1) == -1);
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_GET_ECHO_STATE, &ptr) == 0);
   assert(ptr == aec2);

   /* Unbinding stays supported. */
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_STATE, NULL) == 0);
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_GET_ECHO_STATE, &ptr) == 0);
   assert(ptr == NULL);
   speex_preprocess_state_destroy(p);

   /* Mono state and default channel remain a valid combination. */
   p = speex_preprocess_state_init(F, 8000);
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_STATE, aec1) == 0);
   v = 1;
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_CHANNEL, &v) == -1);
   speex_preprocess_ctl(p, SPEEX_PREPROCESS_GET_ECHO_CHANNEL, &v);
   assert(v == 0);
   speex_preprocess_state_destroy(p);

   speex_echo_state_destroy(aec2);
   speex_echo_state_destroy(aec1);
   printf("Preprocess channel/binding controls OK\n");
}

typedef struct {
   SpeexEchoState *aec;
   SpeexPreprocessState *pp[2];
   SpeexPreprocessState *extra;
   int sel[2];
   int reverse_order;
   int second_enabled;
   short rec[NFRAMES][2*F];
} World;

static SpeexPreprocessState *make_pp(SpeexEchoState *aec, int channel)
{
   SpeexPreprocessState *p = speex_preprocess_state_init(F, 8000);
   int v = channel;
   if (aec)
   {
      assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_CHANNEL, &v) == 0);
      assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_STATE, aec) == 0);
   }
   return p;
}

static World make_world(const short *play, const short *mic, int frames,
                        int sel0, int sel1, int reverse_order,
                        int second_enabled, int extra_channel)
{
   int t;
   World w;
   memset(&w, 0, sizeof(w));
   w.sel[0] = sel0;
   w.sel[1] = sel1;
   w.reverse_order = reverse_order;
   w.second_enabled = second_enabled;
   w.aec = speex_echo_state_init_mc(F, FILTER, 2, 2);
   w.pp[0] = make_pp(w.aec, sel0);
   w.pp[1] = second_enabled ? make_pp(w.aec, sel1) : NULL;
   w.extra = extra_channel >= 0 ? make_pp(w.aec, extra_channel) : NULL;

   for (t=0;t<frames;t++)
   {
      const short *m = mic + t*2*F;
      short out[2*F], q[2][F];
      short work[2][F];
      int order[2];
      int n = second_enabled ? 2 : 1;
      int z, i;
      speex_echo_capture(w.aec, m, out);
      speex_echo_playback(w.aec, play + t*2*F);
      for (i=0;i<F;i++)
      {
         q[0][i] = out[2*i];
         q[1][i] = out[2*i+1];
      }
      if (w.extra)
      {
         memcpy(work[0], q[extra_channel], F*sizeof(short));
         speex_preprocess_run(w.extra, work[0]);
      }
      order[0] = (reverse_order && n == 2) ? 1 : 0;
      order[1] = reverse_order ? 0 : 1;
      for (z=0;z<n;z++)
      {
         int id = order[z];
         /* The selected channel is also the microphone frame processed. */
         int mic_chan = w.sel[id];
         memcpy(work[id], q[mic_chan], F*sizeof(short));
         speex_preprocess_run(w.pp[id], work[id]);
      }
      for (i=0;i<F;i++)
      {
         w.rec[t][2*i] = work[0][i];
         w.rec[t][2*i+1] = second_enabled ? work[1][i] : 0;
      }
   }
   return w;
}

static void destroy_world(World *w)
{
   speex_preprocess_state_destroy(w->pp[0]);
   if (w->pp[1])
      speex_preprocess_state_destroy(w->pp[1]);
   if (w->extra)
      speex_preprocess_state_destroy(w->extra);
   speex_echo_state_destroy(w->aec);
}

static void test_preprocess_worlds(void)
{
   int t, i;
   short *play = (short*)malloc(PPFRAMES*F*2*sizeof(short));
   short *mic = (short*)malloc(PPFRAMES*F*2*sizeof(short));
   short ref[MAXK][NFRAMES*F];
   World wa, wb, wc, we, wf;

   memset(ref, 0, sizeof(ref));
   gen_signals(2, 2, PPFRAMES, play, ref, mic, 0, 0);

   /* A: pp0=mic0/chan0, pp1=mic1/chan1.
      B: selections and microphones swapped.
      C: same as A but preprocessors run in reverse order.
      E: only the channel-0 preprocessor (fewer residual queries on the AEC).
      F: an extra channel-0 preprocessor queries the same channel first. */
   wa = make_world(play, mic, PPFRAMES, 0, 1, 0, 1, -1);
   wb = make_world(play, mic, PPFRAMES, 1, 0, 0, 1, -1);
   wc = make_world(play, mic, PPFRAMES, 0, 1, 1, 1, -1);
   we = make_world(play, mic, PPFRAMES, 0, 0, 0, 0, -1);
   wf = make_world(play, mic, PPFRAMES, 0, 0, 0, 0, 0);

   for (t=0;t<PPFRAMES;t++)
   {
      for (i=0;i<F;i++)
      {
         /* Swapping mics and selections swaps the outputs. */
         assert(wa.rec[t][2*i] == wb.rec[t][2*i+1]);
         assert(wa.rec[t][2*i+1] == wb.rec[t][2*i]);
         /* Run order must not change either channel. */
         assert(wc.rec[t][2*i] == wa.rec[t][2*i]);
         assert(wc.rec[t][2*i+1] == wa.rec[t][2*i+1]);
         /* Extra residual queries leave the next PCM untouched. */
         assert(we.rec[t][2*i] == wa.rec[t][2*i]);
         assert(wf.rec[t][2*i] == wa.rec[t][2*i]);
      }
   }
   destroy_world(&wa);
   destroy_world(&wb);
   destroy_world(&wc);
   destroy_world(&we);
   destroy_world(&wf);
   free(play);
   free(mic);
   printf("Preprocess channel swap/order/query isolation OK\n");
}

/* Internal AEC query (non-static symbol in libspeexdsp). Under the
 * floating-point build used by checks/run.sh spx_word32_t is float. */
#ifdef FLOATING_POINT
extern void speex_echo_get_residual(SpeexEchoState *st, int channel,
                                    float *residual_echo, int len);
#endif

/* Exciting only microphone 0 must not feed microphone 1's residual spectrum.
 * Microphone 1 stays silent while microphone 0 carries strong echo, so the
 * per-channel residual spectrum queried for channel 1 must stay (nearly)
 * zero while channel 0's carries real energy. A second run that adds a
 * channel-0 query before every channel-1 query must not change channel 1. */
static void test_silent_channel_isolation(void)
{
   int t, i;
   short *play = (short*)malloc(PPFRAMES*F*2*sizeof(short));
   short *mic = (short*)malloc(PPFRAMES*F*2*sizeof(short));
   short ref[MAXK][NFRAMES*F];
   SpeexEchoState *a = speex_echo_state_init_mc(F, FILTER, 2, 2);
   SpeexEchoState *b = speex_echo_state_init_mc(F, FILTER, 2, 2);
   SpeexPreprocessState *pa1 = make_pp(a, 1);
   SpeexPreprocessState *pb1 = make_pp(b, 1);
   SpeexPreprocessState *pb0 = make_pp(b, 0);
#ifdef FLOATING_POINT
   float rs0[F+1], rs1[F+1];
#endif

   memset(ref, 0, sizeof(ref));
   gen_signals(2, 2, PPFRAMES, play, ref, mic, 1 /* mic 1 silent */, 0);

   for (t=0;t<PPFRAMES;t++)
   {
      short oa[2*F], ob[2*F], fa[F], fb[F], g0[F];
      speex_echo_capture(a, mic + t*2*F, oa);
      speex_echo_playback(a, play + t*2*F);
      speex_echo_capture(b, mic + t*2*F, ob);
      speex_echo_playback(b, play + t*2*F);
      assert(memcmp(oa, ob, sizeof(oa)) == 0);

#ifdef FLOATING_POINT
      if (t == PPFRAMES-1)
      {
         double e0 = 0., e1 = 0.;
         speex_echo_get_residual(a, 0, rs0, F);
         speex_echo_get_residual(a, 1, rs1, F);
         for (i=0;i<=F;i++)
         {
            e0 += (double)rs0[i]*rs0[i];
            e1 += (double)rs1[i]*rs1[i];
         }
         /* Channel 0 is excited, channel 1 silent: no spectrum borrowing. */
         assert(e0 > 0.);
         assert(e1 < 1e-3*e0);
      }
#endif

      for (i=0;i<F;i++)
      {
         fa[i] = oa[2*i+1];
         fb[i] = ob[2*i+1];
         g0[i] = ob[2*i];
      }
      speex_preprocess_run(pa1, fa);
      /* Run B queries channel 0 first: this must not perturb channel 1. */
      speex_preprocess_run(pb0, g0);
      speex_preprocess_run(pb1, fb);
      assert(memcmp(fa, fb, sizeof(fa)) == 0);
   }

   speex_preprocess_state_destroy(pa1);
   speex_preprocess_state_destroy(pb1);
   speex_preprocess_state_destroy(pb0);
   speex_echo_state_destroy(a);
   speex_echo_state_destroy(b);
   free(play);
   free(mic);
   printf("Preprocess silent-channel residual isolation OK\n");
}

/* Smoke test for the mono preprocessor path and unbinding. */
static void test_mono_preprocess(void)
{
   int t, i;
   short *play = (short*)malloc(60*F*sizeof(short));
   short *mic = (short*)malloc(60*F*sizeof(short));
   short ref[1][NFRAMES*F];
   SpeexEchoState *aec = speex_echo_state_init(F, FILTER);
   SpeexPreprocessState *p = speex_preprocess_state_init(F, 8000);

   memset(ref, 0, sizeof(ref));
   gen_signals(1, 1, 60, play, ref, mic, 0, 0);
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_STATE, aec) == 0);
   for (t=0;t<60;t++)
   {
      short out[F], frame[F];
      speex_echo_capture(aec, mic + t*F, out);
      speex_echo_playback(aec, play + t*F);
      memcpy(frame, out, sizeof(frame));
      speex_preprocess_run(p, frame);
      for (i=0;i<F;i++)
         assert(frame[i] >= -32768 && frame[i] <= 32767);
   }
   assert(speex_preprocess_ctl(p, SPEEX_PREPROCESS_SET_ECHO_STATE, NULL) == 0);
   speex_preprocess_run(p, mic);
   speex_preprocess_state_destroy(p);
   speex_echo_state_destroy(aec);
   free(play);
   free(mic);
   printf("Mono preprocess compatibility OK\n");
}

int main(void)
{
   test_async_equivalence(1, 2);
   test_async_equivalence(2, 1);
   test_async_equivalence(2, 2);
   test_underrun_copy();
   test_underrun_neutral();
   test_playback_queue();
   test_inplace();
   test_preprocess_controls();
   test_preprocess_worlds();
   test_silent_channel_isolation();
   test_mono_preprocess();
   puts("AEC_MULTICHANNEL_OK");
   return 0;
}
