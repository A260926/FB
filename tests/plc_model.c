/*
 * Independent reference model for the narrowband Speex packet-loss
 * concealment path, float build only.
 *
 * The model never calls the production PLC helpers (random generator, RMS,
 * bandwidth expansion, IIR synthesis, high-pass).  It re-implements them from
 * their published arithmetic and advances on its own sample clock after a
 * single snapshot of DecState taken just before the first lost frame.
 *
 * Only the public codec entry points are used to produce and decode speech.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <speex/speex.h>
#include "nb_celp.h"
#include "task_support/check.h"

#define M_FRAME     160
#define M_SUBFRAME   40
#define M_ORDER      10
#define M_PITCH_MIN  17
#define M_PITCH_MAX 144
#define M_HIST      M_FRAME          /* one full frame of excitation history */
#define M_MAX_LOSS   12
#define M_MAX_NORMAL 20

/* Float build constants copied from the reference tables (read-only data). */
static const float k_attenuation[M_ORDER] = {
   1.f, 0.961f, 0.852f, 0.698f, 0.527f,
   0.368f, 0.237f, 0.141f, 0.077f, 0.039f
};
/* highpass(): HIGHPASS_NARROWBAND | HIGHPASS_OUTPUT == filtID 1 */
static const float k_hp_num[3] = {0.96446f, -1.92879f, 0.96446f};
static const float k_hp_den[3] = {1.00000f, -1.92683f, 0.93071f};

/* ------------------------------------------------------------------ */
/* Independent primitives                                              */
/* ------------------------------------------------------------------ */

static float model_rand(float std, uint32_t *seed)
{
   const uint32_t jflone = 0x3f800000u;
   const uint32_t jflmsk = 0x007fffffu;
   union { uint32_t i; float f; } ran;

   *seed = 1664525u * *seed + 1013904223u;
   ran.i = jflone | (jflmsk & *seed);
   ran.f -= 1.5;
   /* The production expression uses a double literal; keep the multiply in
      double and round once on return, like the implicit conversion there. */
   return (float)(3.4642 * (double)std * (double)ran.f);
}

static float model_rms(const float *x, int len)
{
   int i;
   float sum = 0.f;
   for (i = 0; i < len; ++i)
      sum += x[i] * x[i];
   return (float)sqrt(.1 + (sum / len));
}

static float model_median3(float a, float b, float c)
{
   return (a) < (b) ? ((b) < (c) ? (b) : ((a) < (c) ? (c) : (a)))
                    : ((c) < (b) ? (b) : ((c) < (a) ? (c) : (a)));
}

/* bw_lpc(gamma, lpc, lpc, order), in place, exactly as the float build. */
static void model_bw_lpc(float gamma, float *lpc, int order)
{
   int i;
   float tmp = gamma;
   for (i = 0; i < order; ++i) {
      lpc[i] = tmp * lpc[i];
      tmp = tmp * gamma;
   }
}

/* iir_mem16(): y[i] = x[i] + mem[0], then shift the denominator memory. */
static void model_iir_mem(const float *x, const float *den, float *y,
                          int n, int order, float *mem)
{
   int i, j;
   for (i = 0; i < n; ++i) {
      float yi = x[i] + mem[0];
      float nyi = -yi;
      for (j = 0; j < order - 1; ++j)
         mem[j] = mem[j + 1] + den[j] * nyi;
      mem[order - 1] = den[order - 1] * nyi;
      y[i] = yi;
   }
}

/* highpass() with the narrowband output coefficients (filtID 1). */
static void model_highpass(float *x, int n, float *mem)
{
   int i;
   for (i = 0; i < n; ++i) {
      float xi = x[i];
      float vout = k_hp_num[0] * xi + mem[0];
      x[i] = vout;
      mem[0] = (mem[1] + k_hp_num[1] * xi) + (-k_hp_den[1]) * vout;
      mem[1] = k_hp_num[2] * xi + (-k_hp_den[2]) * vout;
   }
}

/* ------------------------------------------------------------------ */
/* Model state                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
   /* Excitation on the model's own absolute sample axis.  Frame -1 (last
      normally decoded frame) lives at [M_HIST-M_FRAME, M_HIST); frame t
      starts at M_HIST + t*M_FRAME. */
   float exc[M_HIST + M_MAX_LOSS * M_FRAME + 8];
   float lpc[M_ORDER];
   float mem_sp[M_ORDER];
   float mem_hp[2];
   float gain_ring[3];
   int   ring_idx;
   int   last_pitch;
   float last_pitch_gain;
   uint32_t seed;
   int   count_lost;
   float innov_gain;
} ModelState;

/* ------------------------------------------------------------------ */
/* Mismatch reporting                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
   const char *group;
   const char *case_name;
   const char *segment;
   int reported;
   int exc_ok;
   int lpc_ok;
   int mem_ok;
   int pcm_ok;
   int state_ok;
   int ring_ok;
   int old_energy_ok;
   int tail_ok;
   int recovery_ok;
} Cmp;

static void report_first(Cmp *c, int loss, const char *field, int index,
                         double expected, double actual, double tol)
{
   if (c->reported)
      return;
   c->reported = 1;
   fprintf(stderr,
           "CONTENT_MISMATCH group=%s case=%s segment=%s loss=%d field=%s",
           c->group, c->case_name, c->segment, loss, field);
   if (index >= 0)
      fprintf(stderr, "[%d]", index);
   fprintf(stderr, " expected=%.9g actual=%.9g absdiff=%.9g tol=%.9g\n",
           expected, actual, fabs(expected - actual), tol);
}

static int near_exc(double expected, double actual)
{
   return fabs(expected - actual) <= 1e-4 + 2e-5 * fabs(expected);
}

static int near_pcm(double expected, double actual)
{
   return fabs(expected - actual) <= 0.03 + 3e-4 * fabs(expected);
}

static int cmp_exc_array(Cmp *c, int loss, const char *field,
                         const float *expected, const float *actual, int n)
{
   int i, ok = 1;
   for (i = 0; i < n; ++i) {
      if (!near_exc(expected[i], actual[i])) {
         report_first(c, loss, field, i, expected[i], actual[i],
                      1e-4 + 2e-5 * fabs((double)expected[i]));
         ok = 0;
      }
   }
   return ok;
}

static int cmp_pcm_array(Cmp *c, int loss, const char *field,
                         const float *expected, const float *actual, int n)
{
   int i, ok = 1;
   for (i = 0; i < n; ++i) {
      if (!near_pcm(expected[i], actual[i])) {
         report_first(c, loss, field, i, expected[i], actual[i],
                      0.03 + 3e-4 * fabs((double)expected[i]));
         ok = 0;
      }
   }
   return ok;
}

static int cmp_exc_scalar(Cmp *c, int loss, const char *field,
                          float expected, float actual)
{
   if (near_exc(expected, actual))
      return 1;
   report_first(c, loss, field, -1, expected, actual,
                1e-4 + 2e-5 * fabs((double)expected));
   return 0;
}

static int cmp_pcm_scalar(Cmp *c, int loss, const char *field,
                          double expected, double actual)
{
   if (near_pcm(expected, actual))
      return 1;
   report_first(c, loss, field, -1, expected, actual,
                0.03 + 3e-4 * fabs(expected));
   return 0;
}

static int cmp_exact(Cmp *c, int loss, const char *field,
                     long expected, long actual)
{
   if (expected == actual)
      return 1;
   report_first(c, loss, field, -1, (double)expected, (double)actual, 0.0);
   return 0;
}

/* ------------------------------------------------------------------ */
/* One independent PLC frame                                           */
/* ------------------------------------------------------------------ */

static void model_lost_frame(ModelState *m, int loss_index, float *out)
{
   int i;
   int frame_start = loss_index * M_FRAME;
   int count = m->count_lost;
   float fact;
   float gain_med;
   float pitch_gain;
   float noise_gain;
   int pitch_val;
   float *exc_frame = &m->exc[M_HIST + frame_start];
   const float *prev_frame = &m->exc[M_HIST + frame_start - M_FRAME];
   const float *synth_in;

   fact = (count < M_ORDER) ? k_attenuation[count] : 0.f;

   gain_med = model_median3(m->gain_ring[0], m->gain_ring[1], m->gain_ring[2]);
   if (gain_med < m->last_pitch_gain)
      m->last_pitch_gain = gain_med;

   pitch_gain = m->last_pitch_gain;
   if (pitch_gain > .85)
      pitch_gain = .85;
   pitch_gain = fact * pitch_gain + 1e-15f;

   /* Old excitation energy is measured before this frame is generated. */
   m->innov_gain = model_rms(prev_frame, M_FRAME);
   noise_gain = m->innov_gain * (fact * (1.f - pitch_gain * pitch_gain));

   pitch_val = m->last_pitch + (int)model_rand(1.f + count, &m->seed);
   if (pitch_val > M_PITCH_MAX)
      pitch_val = M_PITCH_MAX;
   if (pitch_val < M_PITCH_MIN)
      pitch_val = M_PITCH_MIN;

   /* Samples written earlier in this same frame are legitimate pitch
      history for later samples, so generation is strictly forward.  Because
      pitch_val < M_FRAME, the tail of the frame always reads freshly
      predicted excitation from the head of the same frame; this is not a
      repeat of the previous frame's recording. */
   for (i = 0; i < M_FRAME; ++i) {
      float old = exc_frame[i - pitch_val];
      exc_frame[i] = pitch_gain * (old + 1e-15f) +
                     model_rand(noise_gain, &m->seed);
   }

   model_bw_lpc(0.98f, m->lpc, M_ORDER);

   /* Synthesis starts one subframe before the current excitation. */
   synth_in = &m->exc[M_HIST + frame_start - M_SUBFRAME];
   model_iir_mem(synth_in, m->lpc, out, M_FRAME, M_ORDER, m->mem_sp);
   model_highpass(out, M_FRAME, m->mem_hp);

   m->count_lost++;
   m->gain_ring[m->ring_idx++] = pitch_gain;
   if (m->ring_idx > 2)
      m->ring_idx = 0;
}

/* ------------------------------------------------------------------ */
/* Scenario driver                                                     */
/* ------------------------------------------------------------------ */

static void make_input(int history, int frame, spx_int16_t *in)
{
   int i;
   for (i = 0; i < M_FRAME; ++i) {
      int t = frame * M_FRAME + i;
      double v;
      if (history == 0) {
         v = 520.0 * sin(0.037 * t) + 310.0 * sin(0.011 * t + 0.7) +
             120.0 * sin(0.173 * t);
      } else {
         v = 430.0 * sin(0.021 * t + 1.1) + 280.0 * sin(0.093 * t) +
             90.0 * cos(0.211 * t + 0.4);
      }
      in[i] = (spx_int16_t)floor(v + .5);
   }
}

static int encode_packet(void *enc, SpeexBits *wb, int history, int frame,
                         char *packet, int packet_size)
{
   spx_int16_t in[M_FRAME];
   int n;

   make_input(history, frame, in);
   speex_bits_reset(wb);
   if (speex_encode_int(enc, in, wb) < 0)
      return -1;
   n = speex_bits_write(wb, packet, packet_size);
   return n > 0 ? n : -1;
}

static int decode_packet(void *dec, SpeexBits *rb, const char *packet, int n,
                         float *out)
{
   speex_bits_read_from(rb, (char *)packet, n);
   return speex_decode(dec, rb, out);
}

static void snapshot_model(ModelState *m, const DecState *st)
{
   int i;
   memset(m, 0, sizeof(*m));
   /* Frame -1 is the last normally decoded excitation frame. */
   for (i = 0; i < M_FRAME; ++i)
      m->exc[M_HIST - M_FRAME + i] = st->exc[i];
   for (i = 0; i < M_ORDER; ++i) {
      m->lpc[i] = st->interp_qlpc[i];
      m->mem_sp[i] = st->mem_sp[i];
   }
   for (i = 0; i < 3; ++i)
      m->gain_ring[i] = st->pitch_gain_buf[i];
   m->mem_hp[0] = st->mem_hp[0];
   m->mem_hp[1] = st->mem_hp[1];
   m->ring_idx = st->pitch_gain_buf_idx;
   m->last_pitch = st->last_pitch;
   m->last_pitch_gain = st->last_pitch_gain;
   m->seed = st->seed;
   m->count_lost = st->count_lost;
}

static void compare_frame(Cmp *c, int loss_index, ModelState *m,
                          const float *model_pcm, const DecState *st,
                          const float *actual_pcm)
{
   const float *model_exc = &m->exc[M_HIST + loss_index * M_FRAME];
   int i;

   if (!cmp_exc_array(c, loss_index, "excitation", model_exc, st->exc, M_FRAME))
      c->exc_ok = 0;
   if (!cmp_exc_array(c, loss_index, "interp_qlpc", m->lpc, st->interp_qlpc,
                      M_ORDER))
      c->lpc_ok = 0;
   if (!cmp_pcm_array(c, loss_index, "mem_sp", m->mem_sp, st->mem_sp, M_ORDER))
      c->mem_ok = 0;
   if (!cmp_pcm_array(c, loss_index, "mem_hp", m->mem_hp, st->mem_hp, 2))
      c->mem_ok = 0;
   if (!cmp_pcm_array(c, loss_index, "pcm", model_pcm, actual_pcm, M_FRAME))
      c->pcm_ok = 0;

   if (!cmp_exc_scalar(c, loss_index, "last_pitch_gain",
                       m->last_pitch_gain, st->last_pitch_gain))
      c->ring_ok = 0;
   for (i = 0; i < 3; ++i) {
      if (!cmp_exc_scalar(c, loss_index, "pitch_gain_buf",
                          m->gain_ring[i], st->pitch_gain_buf[i]))
         c->ring_ok = 0;
   }
   if (!cmp_exact(c, loss_index, "pitch_gain_buf_idx",
                  m->ring_idx, st->pitch_gain_buf_idx))
      c->ring_ok = 0;
   if (!cmp_exact(c, loss_index, "count_lost", m->count_lost, st->count_lost))
      c->state_ok = 0;
   if (!cmp_exact(c, loss_index, "seed", (long)m->seed, (long)st->seed))
      c->state_ok = 0;
   if (!cmp_exact(c, loss_index, "last_pitch", m->last_pitch, st->last_pitch))
      c->state_ok = 0;
}

/* The attenuation table has M_ORDER entries.  "Exhausted" means count_lost
   already past the table: the injected excitation is zero except for the
   random noise term, yet the recursive synthesis and high-pass filters still
   ring out.  Check that the first frame with zero attenuation still carries
   audible tail energy, and that its filter memory is not dead. */
static void check_tail(Cmp *c, int tail_loss, const float *pcm_tail,
                       const DecState *st)
{
   int i;
   double energy = 0.0;
   double memory = 0.0;

   for (i = 0; i < M_FRAME; ++i)
      energy += (double)pcm_tail[i] * pcm_tail[i];
   for (i = 0; i < M_ORDER; ++i)
      memory += (double)st->mem_sp[i] * st->mem_sp[i];
   memory += (double)st->mem_hp[0] * st->mem_hp[0] +
             (double)st->mem_hp[1] * st->mem_hp[1];

   if (!(energy > 1e-3)) {
      report_first(c, tail_loss, "synthesis_tail_energy", -1, 1e-3, energy, 0.0);
      c->tail_ok = 0;
   }
   /* The filter memories are legitimately tiny this late, so only require
      them to be present (not force-zeroed), which distinguishes a ringing
      filter from a hard reset to silence. */
   if (!(memory > 0.0)) {
      report_first(c, tail_loss, "synthesis_tail_memory", -1, 1.0, memory, 0.0);
      c->tail_ok = 0;
   }
}

static int run_scenario(const char *group, const char *case_name,
                        int history, int normal_frames, int losses,
                        int check_tail_flag)
{
   void *enc = NULL, *dec = NULL, *replay = NULL;
   SpeexBits wb, rb;
   char packets[M_MAX_NORMAL + 2][512];
   int plen[M_MAX_NORMAL + 2];
   DecState *st;
   ModelState model;
   Cmp cmp;
   float actual_pcm[M_FRAME], model_pcm[M_FRAME], replay_pcm[M_FRAME];
   float replay_mem_sp[M_ORDER], replay_mem_hp[2];
   int f, i, n;
   int quality = 5, zero = 0;
   int init_ok = 1;

   printf("PLC_CASE_BEGIN group=%s name=%s\n", group, case_name);
   fflush(stdout);

   memset(&cmp, 0, sizeof(cmp));
   cmp.group = group;
   cmp.case_name = case_name;
   cmp.segment = history == 0 ? "A_single_loss" : "B_twelve_loss";
   cmp.exc_ok = cmp.lpc_ok = cmp.mem_ok = cmp.pcm_ok = 1;
   cmp.state_ok = cmp.ring_ok = cmp.old_energy_ok = cmp.tail_ok = 1;
   cmp.recovery_ok = 1;
   memset(packets, 0, sizeof(packets));
   memset(plen, 0, sizeof(plen));

   enc = speex_encoder_init(&speex_nb_mode);
   dec = speex_decoder_init(&speex_nb_mode);
   if (!enc || !dec)
      init_ok = 0;
   if (init_ok) {
      speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &quality);
      speex_encoder_ctl(enc, SPEEX_SET_VBR, &zero);
      speex_encoder_ctl(enc, SPEEX_SET_DTX, &zero);
   }
   speex_bits_init(&wb);
   speex_bits_init(&rb);

   /* Normal speech history, carried through the public encode/decode API. */
   for (f = 0; init_ok && f < normal_frames; ++f) {
      n = encode_packet(enc, &wb, history, f, packets[f], (int)sizeof(packets[f]));
      if (n < 0) {
         init_ok = 0;
         break;
      }
      plen[f] = n;
      if (decode_packet(dec, &rb, packets[f], n, actual_pcm) != 0) {
         init_ok = 0;
         break;
      }
      for (i = 0; i < M_FRAME; ++i) {
         if (!isfinite(actual_pcm[i])) {
            init_ok = 0;
            break;
         }
      }
   }

   st = (DecState *)dec;
   if (init_ok) {
      snapshot_model(&model, st);
      /* The concealed first frame needs usable old excitation energy. */
      cmp.old_energy_ok = model_rms(&model.exc[M_HIST - M_FRAME], M_FRAME) > 1.f;
      cmp.state_ok = (st->count_lost == 0) && (st->seed == model.seed);
   }

   /* Lost frames, requested through the public NULL-bits decoder path. */
   for (f = 0; init_ok && f < losses; ++f) {
      /* At entry to the lost path the decoder measures the RMS of the frame
         it decoded last, before shifting its buffers.  On the model axis that
         is the previous frame; measure it with the model's own RMS formula
         and compare against the production buffer read through the public
         state.  Nothing is refreshed from the production value. */
      const float *model_prev = &model.exc[M_HIST + (f - 1) * M_FRAME];
      float observed_old = model_rms(st->exc, M_FRAME);
      float expected_old = model_rms(model_prev, M_FRAME);

      if (speex_decode(dec, NULL, actual_pcm) != 0) {
         init_ok = 0;
         break;
      }
      if (!cmp_exc_scalar(&cmp, f, "old_excitation_rms",
                          expected_old, observed_old))
         cmp.old_energy_ok = 0;
      model_lost_frame(&model, f, model_pcm);
      compare_frame(&cmp, f, &model, model_pcm, st, actual_pcm);
      /* First frame whose attenuation factor is already zero: the tail must
         still be alive there even though the injected excitation has died. */
      if (check_tail_flag && f == M_ORDER)
         check_tail(&cmp, f, actual_pcm, st);
   }

   /* Every segment ends with two normal packets, compared against a decoder
      that independently replayed the same prefix and loss schedule. */
   if (init_ok) {
      replay = speex_decoder_init(&speex_nb_mode);
      if (!replay) {
         init_ok = 0;
      } else {
         for (f = 0; f < normal_frames; ++f) {
            if (decode_packet(replay, &rb, packets[f], plen[f], replay_pcm) != 0) {
               init_ok = 0;
               break;
            }
         }
         for (f = 0; init_ok && f < losses; ++f) {
            if (speex_decode(replay, NULL, replay_pcm) != 0) {
               init_ok = 0;
               break;
            }
         }
      }
      for (f = 0; init_ok && f < 2; ++f) {
         DecState *rst = (DecState *)replay;
         n = encode_packet(enc, &wb, history, normal_frames + f, packets[M_MAX_NORMAL + f],
                           (int)sizeof(packets[M_MAX_NORMAL + f]));
         if (n < 0) {
            init_ok = 0;
            break;
         }
         plen[M_MAX_NORMAL + f] = n;
         if (decode_packet(dec, &rb, packets[M_MAX_NORMAL + f], n, actual_pcm) != 0) {
            init_ok = 0;
            break;
         }
         if (decode_packet(replay, &rb, packets[M_MAX_NORMAL + f], n, replay_pcm) != 0) {
            init_ok = 0;
            break;
         }
         if (!cmp_pcm_array(&cmp, normal_frames + f, "recovery_pcm",
                            replay_pcm, actual_pcm, M_FRAME))
            cmp.recovery_ok = 0;
         for (i = 0; i < M_ORDER; ++i)
            replay_mem_sp[i] = rst->mem_sp[i];
         replay_mem_hp[0] = rst->mem_hp[0];
         replay_mem_hp[1] = rst->mem_hp[1];
         if (!cmp_pcm_scalar(&cmp, normal_frames + f, "recovery_mem_sp",
                             model_rms(replay_mem_sp, M_ORDER),
                             model_rms(st->mem_sp, M_ORDER)))
            cmp.recovery_ok = 0;
         if (!cmp_pcm_scalar(&cmp, normal_frames + f, "recovery_mem_hp",
                             fmaxf(fabsf(replay_mem_hp[0]), fabsf(replay_mem_hp[1])),
                             fmaxf(fabsf(st->mem_hp[0]), fabsf(st->mem_hp[1]))))
            cmp.recovery_ok = 0;
         if (!cmp_exact(&cmp, normal_frames + f, "recovery_count_lost",
                        (long)rst->count_lost, (long)st->count_lost))
            cmp.recovery_ok = 0;
         if (!cmp_exact(&cmp, normal_frames + f, "recovery_seed",
                        (long)rst->seed, (long)st->seed))
            cmp.recovery_ok = 0;
      }
   }

   TASK_CHECK(init_ok);
   TASK_CHECK(cmp.old_energy_ok);
   TASK_CHECK(cmp.exc_ok);
   TASK_CHECK(cmp.lpc_ok);
   TASK_CHECK(cmp.mem_ok);
   TASK_CHECK(cmp.pcm_ok);
   TASK_CHECK(cmp.state_ok);
   TASK_CHECK(cmp.ring_ok);
   if (check_tail_flag)
      TASK_CHECK(cmp.tail_ok);
   TASK_CHECK(cmp.recovery_ok);

   if (replay)
      speex_decoder_destroy(replay);
   speex_bits_destroy(&rb);
   speex_bits_destroy(&wb);
   if (dec)
      speex_decoder_destroy(dec);
   if (enc)
      speex_encoder_destroy(enc);
   return 0;
}

/* Excitation group: one lost packet from history A. */
int plc_case_excitation_single(void)
{
   return run_scenario("Excitation", "single_loss_excitation_A",
                       0, 9, 1, 0);
}

/* Excitation group: twelve consecutive lost packets from history B. */
int plc_case_excitation_twelve(void)
{
   return run_scenario("Excitation", "twelve_loss_excitation_B",
                       1, 13, 12, 0);
}

/* Synthesis group: LPC expansion, filter memories and PCM, history A. */
int plc_case_synthesis_single(void)
{
   return run_scenario("Synthesis", "single_loss_synthesis_A",
                       0, 9, 1, 0);
}

/* Synthesis group: the same through and beyond attenuation exhaustion.
   The tail frame (loss index 10, attenuation factor zero) is checked for
   surviving energy, not expected to be silent. */
int plc_case_synthesis_twelve(void)
{
   return run_scenario("Synthesis", "twelve_loss_synthesis_B",
                       1, 13, 12, 1);
}

/* Recovery group: two normal packets after a single loss, history A. */
int plc_case_recovery_single(void)
{
   return run_scenario("Recovery", "single_loss_recovery_A",
                       0, 9, 1, 0);
}

/* Recovery group: two normal packets after twelve losses, history B. */
int plc_case_recovery_twelve(void)
{
   return run_scenario("Recovery", "twelve_loss_recovery_B",
                       1, 13, 12, 1);
}
