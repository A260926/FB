/* Regression tests for SPEEX_RESET_STATE.
 *
 * Every case first drives a "used" encoder/decoder through real audio,
 * silence and (for decoders) lost packets so that non-zero session history
 * accumulates, resets it with SPEEX_RESET_STATE, and then compares several
 * further frames against an independent freshly created handle configured
 * with the same controls in the same order.
 *
 * Comparisons are content based: encoded packets are compared bit for bit
 * (including the SpeexBits cursor position) and decoded audio sample by
 * sample. Return codes alone are never enough.
 *
 * This file has no main(): task_support/driver.c provides one and drives the
 * registered task_cases table.
 */

#include "task_support/check.h"
#include "speex/speex.h"
#include "speex/speex_callbacks.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAME   640
#define MAX_PACKET  512
#define WARMUP      14
#define COMPARE     10
#define WARMUP_LOSS 3

static const int all_modes[3] = {SPEEX_MODEID_NB, SPEEX_MODEID_WB, SPEEX_MODEID_UWB};

/* Deterministic test signal, identical for every handle of a mode. */
static void make_frame(spx_int16_t *p, int n, int frame, int seed)
{
   int i;
   for (i = 0; i < n; ++i) {
      int k = frame*n + i;
      int v = ((k*(13+seed))%201-100)*35 + ((k*(3+seed))%57-28)*18;
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      p[i] = (spx_int16_t)v;
   }
}

static void make_float_frame(float *p, int n, int frame, int seed)
{
   int i;
   for (i = 0; i < n; ++i) {
      int k = frame*n + i;
      p[i] = (float)(((k*(13+seed))%201-100)*35 + ((k*(3+seed))%57-28)*18);
   }
}

static void silence_short(spx_int16_t *p, int n)
{
   memset(p, 0, (size_t)n*sizeof(*p));
}

static void silence_float(float *p, int n)
{
   int i;
   for (i = 0; i < n; ++i) p[i] = 0.0f;
}

/* The encoder may overwrite its input (the high-pass filter runs in place
 * and the wideband QMF analysis writes both bands back into the buffer), so
 * two handles must never share one input frame. */
static void fill_pair_short(spx_int16_t *a, spx_int16_t *b, int n, int frame, int seed)
{
   make_frame(a, n, frame, seed);
   memcpy(b, a, (size_t)n*sizeof(*a));
}

static void silence_pair_short(spx_int16_t *a, spx_int16_t *b, int n)
{
   silence_short(a, n);
   silence_short(b, n);
}

static void fill_pair_float(float *a, float *b, int n, int frame, int seed)
{
   make_float_frame(a, n, frame, seed);
   memcpy(b, a, (size_t)n*sizeof(*a));
}

static void silence_pair_float(float *a, float *b, int n)
{
   silence_float(a, n);
   silence_float(b, n);
}

struct enc_cfg {
   int use_set_mode;   /* explicit submode instead of a quality */
   int quality;
   int explicit_mode;
   int complexity;
   int sampling_rate;
   int highpass;
   int vbr;
   float vbr_quality;
   int vad;
   int dtx;
   int plc_tuning;
   int submode_encoding;
};

static void default_enc_cfg(struct enc_cfg *c)
{
   memset(c, 0, sizeof(*c));
   c->quality = 7;
   c->explicit_mode = 3;
   c->complexity = 3;
   c->sampling_rate = 0;      /* 0 means "leave the mode default" */
   c->highpass = 0;           /* non-default */
   c->vbr = 0;
   c->vbr_quality = 5.5f;
   c->vad = 1;
   c->dtx = 0;
   c->plc_tuning = 7;         /* non-default */
   c->submode_encoding = 1;
}

static int configure_encoder(void *st, const struct enc_cfg *c)
{
   int q;
   if (c->use_set_mode) {
      q = c->explicit_mode;
      TASK_CHECK(speex_encoder_ctl(st, SPEEX_SET_MODE, &q) == 0);
   } else {
      q = c->quality;
      TASK_CHECK(speex_encoder_ctl(st, SPEEX_SET_QUALITY, &q) == 0);
   }
   q = c->complexity;
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_SET_COMPLEXITY, &q) == 0);
   if (c->sampling_rate) {
      q = c->sampling_rate;
      TASK_CHECK(speex_encoder_ctl(st, SPEEX_SET_SAMPLING_RATE, &q) == 0);
   }
   q = c->highpass;
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_SET_HIGHPASS, &q) == 0);
   q = c->vbr;
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_SET_VBR, &q) == 0);
   /* SET_VBR_QUALITY has a SET_QUALITY side effect, so only apply it while
    * VBR is actually in use; in CBR it cannot change the chosen mode. */
   if (c->vbr) {
      float vq = c->vbr_quality;
      TASK_CHECK(speex_encoder_ctl(st, SPEEX_SET_VBR_QUALITY, &vq) == 0);
   }
   q = c->vad;
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_SET_VAD, &q) == 0);
   q = c->dtx;
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_SET_DTX, &q) == 0);
   q = c->plc_tuning;
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_SET_PLC_TUNING, &q) == 0);
   q = c->submode_encoding;
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_SET_SUBMODE_ENCODING, &q) == 0);
   return 0;
}

/* GET every caller-controlled item back and make sure a reset kept it. */
static int encoder_config_preserved(void *st, const struct enc_cfg *c)
{
   int q;
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_GET_COMPLEXITY, &q) == 0);
   TASK_CHECK(q == c->complexity);
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_GET_HIGHPASS, &q) == 0);
   TASK_CHECK(q == c->highpass);
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_GET_VBR, &q) == 0);
   TASK_CHECK(q == c->vbr);
   if (c->vbr) {
      float vq;
      TASK_CHECK(speex_encoder_ctl(st, SPEEX_GET_VBR_QUALITY, &vq) == 0);
      TASK_CHECK(fabsf(vq - c->vbr_quality) < 1e-6f);
   }
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_GET_VAD, &q) == 0);
   TASK_CHECK(q == c->vad);
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_GET_DTX, &q) == 0);
   TASK_CHECK(q == c->dtx);
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_GET_PLC_TUNING, &q) == 0);
   TASK_CHECK(q == c->plc_tuning);
   TASK_CHECK(speex_encoder_ctl(st, SPEEX_GET_SUBMODE_ENCODING, &q) == 0);
   TASK_CHECK(q == c->submode_encoding);
   if (c->sampling_rate) {
      TASK_CHECK(speex_encoder_ctl(st, SPEEX_GET_SAMPLING_RATE, &q) == 0);
      TASK_CHECK(q == c->sampling_rate);
   }
   return 0;
}

struct dec_cfg {
   int enh;
   int highpass;
   int submode_encoding;
};

static void default_dec_cfg(struct dec_cfg *c)
{
   memset(c, 0, sizeof(*c));
   c->enh = 0;             /* non-default */
   c->highpass = 0;        /* non-default */
   c->submode_encoding = 1;
}

static int configure_decoder(void *st, const struct dec_cfg *c)
{
   int q;
   q = c->enh;
   TASK_CHECK(speex_decoder_ctl(st, SPEEX_SET_ENH, &q) == 0);
   q = c->highpass;
   TASK_CHECK(speex_decoder_ctl(st, SPEEX_SET_HIGHPASS, &q) == 0);
   q = c->submode_encoding;
   TASK_CHECK(speex_decoder_ctl(st, SPEEX_SET_SUBMODE_ENCODING, &q) == 0);
   return 0;
}

static int decoder_config_preserved(void *st, const struct dec_cfg *c)
{
   int q;
   TASK_CHECK(speex_decoder_ctl(st, SPEEX_GET_ENH, &q) == 0);
   TASK_CHECK(q == c->enh);
   TASK_CHECK(speex_decoder_ctl(st, SPEEX_GET_HIGHPASS, &q) == 0);
   TASK_CHECK(q == c->highpass);
   TASK_CHECK(speex_decoder_ctl(st, SPEEX_GET_SUBMODE_ENCODING, &q) == 0);
   TASK_CHECK(q == c->submode_encoding);
   return 0;
}

static int packets_equal(const SpeexBits *a, const SpeexBits *b,
                         const char *pa, const char *pb, int na, int nb)
{
   TASK_CHECK(na == nb);
   TASK_CHECK(a->nbBits == b->nbBits);
   TASK_CHECK(a->charPtr == b->charPtr);
   TASK_CHECK(a->bitPtr == b->bitPtr);
   TASK_CHECK(a->overflow == b->overflow && b->overflow == 0);
   TASK_CHECK(memcmp(pa, pb, (size_t)na) == 0);
   return 0;
}

static int short_equal(const spx_int16_t *a, const spx_int16_t *b, int n)
{
   int i;
   for (i = 0; i < n; ++i)
      TASK_CHECK(a[i] == b[i]);
   return 0;
}

static int float_close(const float *got, const float *ref, int n)
{
   int i;
   for (i = 0; i < n; ++i) {
      float tol = 1e-5f + 1e-6f*fabsf(ref[i]);
      TASK_CHECK(fabsf(got[i] - ref[i]) <= tol);
   }
   return 0;
}

/* Custom in-band and user handlers: retention is proven by the counters
 * moving after a reset, which can only happen if func/data survived. */
struct handler_ctx { int inband_calls; int user_calls; };

static int counting_inband_handler(SpeexBits *bits, void *state, void *data)
{
   struct handler_ctx *ctx = (struct handler_ctx*)data;
   (void)bits; (void)state;
   ctx->inband_calls++;
   speex_bits_advance(bits, 1);  /* id 1 is a 1-bit request */
   return 0;
}

static int counting_user_handler(SpeexBits *bits, void *state, void *data)
{
   unsigned req_size;
   struct handler_ctx *ctx = (struct handler_ctx*)data;
   (void)state;
   ctx->user_calls++;
   req_size = speex_bits_unpack_unsigned(bits, 4);
   speex_bits_advance(bits, 5 + 8*(int)req_size);
   return 0;
}

static void register_handlers(void *st, struct handler_ctx *ctx, SpeexCallback *cb)
{
   cb[0].callback_id = SPEEX_INBAND_RESERVED1;
   cb[0].func = counting_inband_handler;
   cb[0].data = ctx;
   speex_decoder_ctl(st, SPEEX_SET_HANDLER, &cb[0]);
   cb[1].callback_id = 0;
   cb[1].func = counting_user_handler;
   cb[1].data = ctx;
   speex_decoder_ctl(st, SPEEX_SET_USER_HANDLER, &cb[1]);
}

/* Packet carrying one Speex in-band request (id 1, 1 bit) and one user
 * request (req_size 0), terminated like any Speex stream. */
static int build_request_packet(SpeexBits *bits, char *buf, int *len)
{
   speex_bits_reset(bits);
   speex_bits_pack(bits, 0, 1);                       /* NB block marker */
   speex_bits_pack(bits, 14, 4);                      /* Speex in-band */
   speex_bits_pack(bits, SPEEX_INBAND_RESERVED1, 4);  /* handler id */
   speex_bits_pack(bits, 1, 1);                       /* 1-bit payload */
   speex_bits_pack(bits, 0, 1);                       /* NB block marker */
   speex_bits_pack(bits, 13, 4);                      /* user request */
   speex_bits_pack(bits, 0, 4);                       /* req_size */
   speex_bits_pack(bits, 0, 1);                       /* NB block marker */
   speex_bits_pack(bits, 15, 4);                      /* terminator */
   *len = speex_bits_write(bits, buf, MAX_PACKET);
   TASK_CHECK(*len > 0);
   return 0;
}

/* Warm up an encoder with audio followed by silence (and, outside DTX, an
 * audio recovery) and return once real history has accumulated. */
static int warmup_encoder(void *st, int frame_size, int dtx,
                          SpeexBits *bits, char *pkt, int *saw_nonzero)
{
   spx_int16_t in[MAX_FRAME];
   int k, n;
   *saw_nonzero = 0;
   for (k = 0; k < WARMUP; ++k) {
      if (dtx) {
         if (k < WARMUP/2) make_frame(in, frame_size, k, 7);
         else silence_short(in, frame_size);
      } else {
         if (k >= 4 && k < 9) silence_short(in, frame_size);
         else make_frame(in, frame_size, k, 7);
      }
      speex_bits_reset(bits);
      TASK_CHECK(speex_encode_int(st, in, bits) >= 0);
      n = speex_bits_write(bits, pkt, MAX_PACKET);
      TASK_CHECK(n > 0);
      if ((int)(unsigned char)pkt[0] != 0) *saw_nonzero = 1;
   }
   TASK_CHECK(*saw_nonzero);
   return 0;
}

/* ------------------------------------------------------------------ */
/* Encoder cases                                                      */
/* ------------------------------------------------------------------ */

static int encoder_reset_matches_fresh(int mode_id, const struct enc_cfg *cfg)
{
   const SpeexMode *mode = speex_lib_get_mode(mode_id);
   void *used, *fresh;
   SpeexBits bu, bf;
   spx_int16_t in_u[MAX_FRAME], in_f[MAX_FRAME];
   char pu[MAX_PACKET], pf[MAX_PACKET];
   int frame_size, saw, k, nu, nf, nonzero_after = 0;
   TASK_CHECK(mode != NULL);
   used = speex_encoder_init(mode);
   TASK_CHECK(used != NULL);
   TASK_CHECK(configure_encoder(used, cfg) == 0);
   speex_bits_init(&bu);
   speex_bits_init(&bf);
   speex_encoder_ctl(used, SPEEX_GET_FRAME_SIZE, &frame_size);
   TASK_CHECK(frame_size > 0 && frame_size <= MAX_FRAME);
   TASK_CHECK(warmup_encoder(used, frame_size, cfg->dtx, &bu, pu, &saw) == 0);

   TASK_CHECK(speex_encoder_ctl(used, SPEEX_RESET_STATE, NULL) == 0);
   TASK_CHECK(encoder_config_preserved(used, cfg) == 0);

   fresh = speex_encoder_init(mode);
   TASK_CHECK(fresh != NULL);
   TASK_CHECK(configure_encoder(fresh, cfg) == 0);

   for (k = 0; k < COMPARE; ++k) {
      if (k == COMPARE/2) silence_pair_short(in_u, in_f, frame_size);
      else fill_pair_short(in_u, in_f, frame_size, k, 2);
      TASK_CHECK(memcmp(in_u, in_f, (size_t)frame_size*sizeof(*in_u)) == 0);
      speex_bits_reset(&bu);
      speex_bits_reset(&bf);
      TASK_CHECK(speex_encode_int(used, in_u, &bu) >= 0);
      TASK_CHECK(speex_encode_int(fresh, in_f, &bf) >= 0);
      nu = speex_bits_write(&bu, pu, MAX_PACKET);
      nf = speex_bits_write(&bf, pf, MAX_PACKET);
      TASK_CHECK(packets_equal(&bu, &bf, pu, pf, nu, nf) == 0);
      if ((int)(unsigned char)pu[0] != 0) nonzero_after = 1;
   }
   TASK_CHECK(nonzero_after);

   speex_bits_destroy(&bu);
   speex_bits_destroy(&bf);
   speex_encoder_destroy(used);
   speex_encoder_destroy(fresh);
   return 0;
}

static int enc_cbr(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      struct enc_cfg cfg;
      default_enc_cfg(&cfg);
      cfg.vbr = 0; cfg.dtx = 0;
      TASK_CHECK(encoder_reset_matches_fresh(all_modes[i], &cfg) == 0);
   }
   return 0;
}

static int enc_vbr(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      struct enc_cfg cfg;
      default_enc_cfg(&cfg);
      cfg.vbr = 1; cfg.dtx = 0; cfg.vad = 1;
      TASK_CHECK(encoder_reset_matches_fresh(all_modes[i], &cfg) == 0);
   }
   return 0;
}

static int enc_vbr_dtx_recovery(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      struct enc_cfg cfg;
      default_enc_cfg(&cfg);
      cfg.vbr = 1; cfg.vad = 1; cfg.dtx = 1;
      TASK_CHECK(encoder_reset_matches_fresh(all_modes[i], &cfg) == 0);
   }
   return 0;
}

static int enc_explicit_mode(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      struct enc_cfg cfg;
      default_enc_cfg(&cfg);
      cfg.use_set_mode = 1;
      TASK_CHECK(encoder_reset_matches_fresh(all_modes[i], &cfg) == 0);
   }
   return 0;
}

/* Controls changed on a live handle must survive the reset, and the output
 * must match a fresh handle given the resulting configuration. */
static int enc_control_update(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      const SpeexMode *mode = speex_lib_get_mode(all_modes[i]);
      struct enc_cfg cfg;
      void *used, *fresh;
      SpeexBits bu, bf;
      spx_int16_t in_u[MAX_FRAME], in_f[MAX_FRAME];
      char pu[MAX_PACKET], pf[MAX_PACKET];
      int frame_size, saw, k, nu, nf, q;
      default_enc_cfg(&cfg);
      used = speex_encoder_init(mode);
      TASK_CHECK(used != NULL);
      TASK_CHECK(configure_encoder(used, &cfg) == 0);
      speex_bits_init(&bu);
      speex_bits_init(&bf);
      speex_encoder_ctl(used, SPEEX_GET_FRAME_SIZE, &frame_size);
      TASK_CHECK(frame_size > 0 && frame_size <= MAX_FRAME);
      TASK_CHECK(warmup_encoder(used, frame_size, 0, &bu, pu, &saw) == 0);

      /* Mid-session control updates. */
      cfg.quality = 4;
      cfg.complexity = 1;
      cfg.highpass = 1;
      cfg.plc_tuning = 20;
      q = cfg.quality;
      TASK_CHECK(speex_encoder_ctl(used, SPEEX_SET_QUALITY, &q) == 0);
      q = cfg.complexity;
      TASK_CHECK(speex_encoder_ctl(used, SPEEX_SET_COMPLEXITY, &q) == 0);
      q = cfg.highpass;
      TASK_CHECK(speex_encoder_ctl(used, SPEEX_SET_HIGHPASS, &q) == 0);
      q = cfg.plc_tuning;
      TASK_CHECK(speex_encoder_ctl(used, SPEEX_SET_PLC_TUNING, &q) == 0);

      TASK_CHECK(speex_encoder_ctl(used, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(encoder_config_preserved(used, &cfg) == 0);

      fresh = speex_encoder_init(mode);
      TASK_CHECK(fresh != NULL);
      TASK_CHECK(configure_encoder(fresh, &cfg) == 0);
      for (k = 0; k < COMPARE; ++k) {
         fill_pair_short(in_u, in_f, frame_size, k, 4);
         speex_bits_reset(&bu);
         speex_bits_reset(&bf);
         TASK_CHECK(speex_encode_int(used, in_u, &bu) >= 0);
         TASK_CHECK(speex_encode_int(fresh, in_f, &bf) >= 0);
         nu = speex_bits_write(&bu, pu, MAX_PACKET);
         nf = speex_bits_write(&bf, pf, MAX_PACKET);
         TASK_CHECK(packets_equal(&bu, &bf, pu, pf, nu, nf) == 0);
      }
      speex_bits_destroy(&bu);
      speex_bits_destroy(&bf);
      speex_encoder_destroy(used);
      speex_encoder_destroy(fresh);
   }
   return 0;
}

/* Two handles interleaved through one stream of calls, and a reset applied
 * twice in a row, must each be equivalent to a brand new handle. */
static int enc_interleaved_double_reset(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      const SpeexMode *mode = speex_lib_get_mode(all_modes[i]);
      struct enc_cfg cfg;
      void *a, *b, *ra, *rb;
      SpeexBits ba, bb, bra, brb;
      spx_int16_t in_a[MAX_FRAME], in_ra[MAX_FRAME];
      spx_int16_t in_b[MAX_FRAME], in_rb[MAX_FRAME];
      char pa[MAX_PACKET], pb[MAX_PACKET], pra[MAX_PACKET], prb[MAX_PACKET];
      int frame_size, k, na, nb, nra, nrb;
      default_enc_cfg(&cfg);
      cfg.vbr = 1; cfg.vad = 1;
      a = speex_encoder_init(mode);
      b = speex_encoder_init(mode);
      TASK_CHECK(a != NULL && b != NULL);
      TASK_CHECK(configure_encoder(a, &cfg) == 0);
      TASK_CHECK(configure_encoder(b, &cfg) == 0);
      speex_bits_init(&ba); speex_bits_init(&bb);
      speex_bits_init(&bra); speex_bits_init(&brb);
      speex_encoder_ctl(a, SPEEX_GET_FRAME_SIZE, &frame_size);
      for (k = 0; k < WARMUP; ++k) {
         make_frame(in_a, frame_size, k, 7);
         speex_bits_reset(&ba);
         TASK_CHECK(speex_encode_int(a, in_a, &ba) >= 0);
         (void)speex_bits_write(&ba, pa, MAX_PACKET);
         make_frame(in_b, frame_size, k, 11);
         speex_bits_reset(&bb);
         TASK_CHECK(speex_encode_int(b, in_b, &bb) >= 0);
         (void)speex_bits_write(&bb, pb, MAX_PACKET);
      }
      /* Interleaved resets, one of them repeated. */
      TASK_CHECK(speex_encoder_ctl(a, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(speex_encoder_ctl(b, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(speex_encoder_ctl(b, SPEEX_RESET_STATE, NULL) == 0);

      ra = speex_encoder_init(mode);
      rb = speex_encoder_init(mode);
      TASK_CHECK(ra != NULL && rb != NULL);
      TASK_CHECK(configure_encoder(ra, &cfg) == 0);
      TASK_CHECK(configure_encoder(rb, &cfg) == 0);
      for (k = 0; k < COMPARE; ++k) {
         fill_pair_short(in_a, in_ra, frame_size, k, 2);
         speex_bits_reset(&ba); speex_bits_reset(&bra);
         TASK_CHECK(speex_encode_int(a, in_a, &ba) >= 0);
         TASK_CHECK(speex_encode_int(ra, in_ra, &bra) >= 0);
         na = speex_bits_write(&ba, pa, MAX_PACKET);
         nra = speex_bits_write(&bra, pra, MAX_PACKET);
         TASK_CHECK(packets_equal(&ba, &bra, pa, pra, na, nra) == 0);

         fill_pair_short(in_b, in_rb, frame_size, k, 5);
         speex_bits_reset(&bb); speex_bits_reset(&brb);
         TASK_CHECK(speex_encode_int(b, in_b, &bb) >= 0);
         TASK_CHECK(speex_encode_int(rb, in_rb, &brb) >= 0);
         nb = speex_bits_write(&bb, pb, MAX_PACKET);
         nrb = speex_bits_write(&brb, prb, MAX_PACKET);
         TASK_CHECK(packets_equal(&bb, &brb, pb, prb, nb, nrb) == 0);
      }
      speex_bits_destroy(&ba); speex_bits_destroy(&bb);
      speex_bits_destroy(&bra); speex_bits_destroy(&brb);
      speex_encoder_destroy(a); speex_encoder_destroy(b);
      speex_encoder_destroy(ra); speex_encoder_destroy(rb);
   }
   return 0;
}

/* Resetting immediately after init must not change anything. */
static int enc_reset_fresh_handle(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      const SpeexMode *mode = speex_lib_get_mode(all_modes[i]);
      struct enc_cfg cfg;
      void *used, *fresh;
      SpeexBits bu, bf;
      spx_int16_t in_u[MAX_FRAME], in_f[MAX_FRAME];
      char pu[MAX_PACKET], pf[MAX_PACKET];
      int frame_size, k, nu, nf;
      default_enc_cfg(&cfg);
      used = speex_encoder_init(mode);
      fresh = speex_encoder_init(mode);
      TASK_CHECK(used != NULL && fresh != NULL);
      TASK_CHECK(configure_encoder(used, &cfg) == 0);
      TASK_CHECK(configure_encoder(fresh, &cfg) == 0);
      TASK_CHECK(speex_encoder_ctl(used, SPEEX_RESET_STATE, NULL) == 0);
      speex_bits_init(&bu);
      speex_bits_init(&bf);
      speex_encoder_ctl(used, SPEEX_GET_FRAME_SIZE, &frame_size);
      for (k = 0; k < COMPARE; ++k) {
         fill_pair_short(in_u, in_f, frame_size, k, 2);
         speex_bits_reset(&bu);
         speex_bits_reset(&bf);
         TASK_CHECK(speex_encode_int(used, in_u, &bu) >= 0);
         TASK_CHECK(speex_encode_int(fresh, in_f, &bf) >= 0);
         nu = speex_bits_write(&bu, pu, MAX_PACKET);
         nf = speex_bits_write(&bf, pf, MAX_PACKET);
         TASK_CHECK(packets_equal(&bu, &bf, pu, pf, nu, nf) == 0);
      }
      speex_bits_destroy(&bu);
      speex_bits_destroy(&bf);
      speex_encoder_destroy(used);
      speex_encoder_destroy(fresh);
   }
   return 0;
}

/* Float API: packets stay byte exact (and cursors identical); the float
 * outputs are compared with the mandated tolerance in a decoder case below. */
static int enc_float_entry(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      const SpeexMode *mode = speex_lib_get_mode(all_modes[i]);
      struct enc_cfg cfg;
      void *used, *fresh;
      SpeexBits bu, bf;
      float in_u[MAX_FRAME], in_f[MAX_FRAME];
      char pu[MAX_PACKET], pf[MAX_PACKET];
      int frame_size, saw, k, nu, nf;
      default_enc_cfg(&cfg);
      cfg.vbr = 1;
      used = speex_encoder_init(mode);
      TASK_CHECK(used != NULL);
      TASK_CHECK(configure_encoder(used, &cfg) == 0);
      speex_bits_init(&bu);
      speex_bits_init(&bf);
      speex_encoder_ctl(used, SPEEX_GET_FRAME_SIZE, &frame_size);
      saw = 0;
      for (k = 0; k < WARMUP; ++k) {
         if (k < WARMUP/2) make_float_frame(in_u, frame_size, k, 7);
         else silence_float(in_u, frame_size);
         speex_bits_reset(&bu);
         TASK_CHECK(speex_encode(used, in_u, &bu) >= 0);
         nu = speex_bits_write(&bu, pu, MAX_PACKET);
         if ((int)(unsigned char)pu[0] != 0) saw = 1;
         TASK_CHECK(nu > 0);
      }
      TASK_CHECK(saw);
      TASK_CHECK(speex_encoder_ctl(used, SPEEX_RESET_STATE, NULL) == 0);
      fresh = speex_encoder_init(mode);
      TASK_CHECK(fresh != NULL);
      TASK_CHECK(configure_encoder(fresh, &cfg) == 0);
      for (k = 0; k < COMPARE; ++k) {
         if (k == COMPARE/2) silence_pair_float(in_u, in_f, frame_size);
         else fill_pair_float(in_u, in_f, frame_size, k, 2);
         speex_bits_reset(&bu);
         speex_bits_reset(&bf);
         TASK_CHECK(speex_encode(used, in_u, &bu) >= 0);
         TASK_CHECK(speex_encode(fresh, in_f, &bf) >= 0);
         nu = speex_bits_write(&bu, pu, MAX_PACKET);
         nf = speex_bits_write(&bf, pf, MAX_PACKET);
         TASK_CHECK(packets_equal(&bu, &bf, pu, pf, nu, nf) == 0);
      }
      speex_bits_destroy(&bu);
      speex_bits_destroy(&bf);
      speex_encoder_destroy(used);
      speex_encoder_destroy(fresh);
   }
   return 0;
}

/* A handle that is never reset must keep producing its normal stream: run
 * two fresh handles in lockstep and require identical packets throughout. */
static int enc_no_reset_unchanged(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      const SpeexMode *mode = speex_lib_get_mode(all_modes[i]);
      struct enc_cfg cfg;
      void *a, *b;
      SpeexBits ba, bb;
      spx_int16_t in_a[MAX_FRAME], in_b[MAX_FRAME];
      char pa[MAX_PACKET], pb[MAX_PACKET];
      int frame_size, k, na, nb;
      default_enc_cfg(&cfg);
      a = speex_encoder_init(mode);
      b = speex_encoder_init(mode);
      TASK_CHECK(a != NULL && b != NULL);
      TASK_CHECK(configure_encoder(a, &cfg) == 0);
      TASK_CHECK(configure_encoder(b, &cfg) == 0);
      speex_bits_init(&ba);
      speex_bits_init(&bb);
      speex_encoder_ctl(a, SPEEX_GET_FRAME_SIZE, &frame_size);
      for (k = 0; k < WARMUP + COMPARE; ++k) {
         fill_pair_short(in_a, in_b, frame_size, k, 3);
         speex_bits_reset(&ba);
         speex_bits_reset(&bb);
         TASK_CHECK(speex_encode_int(a, in_a, &ba) >= 0);
         TASK_CHECK(speex_encode_int(b, in_b, &bb) >= 0);
         na = speex_bits_write(&ba, pa, MAX_PACKET);
         nb = speex_bits_write(&bb, pb, MAX_PACKET);
         TASK_CHECK(packets_equal(&ba, &bb, pa, pb, na, nb) == 0);
      }
      speex_bits_destroy(&ba);
      speex_bits_destroy(&bb);
      speex_encoder_destroy(a);
      speex_encoder_destroy(b);
   }
   return 0;
}

/* ------------------------------------------------------------------ */
/* Decoder cases                                                      */
/* ------------------------------------------------------------------ */

/* Decode one packet (or a lost frame when pkt_len <= 0) on two decoders. */
static int decode_pair(void *used, void *fresh, int frame_size,
                       const char *pkt, int pkt_len,
                       spx_int16_t *ou, spx_int16_t *of)
{
   SpeexBits bu, bf;
   speex_bits_init(&bu);
   speex_bits_init(&bf);
   if (pkt_len <= 0) {
      TASK_CHECK(speex_decode_int(used, NULL, ou) == 0);
      TASK_CHECK(speex_decode_int(fresh, NULL, of) == 0);
   } else {
      speex_bits_read_from(&bu, pkt, pkt_len);
      speex_bits_read_from(&bf, pkt, pkt_len);
      TASK_CHECK(speex_decode_int(used, &bu, ou) == 0);
      TASK_CHECK(speex_decode_int(fresh, &bf, of) == 0);
      TASK_CHECK(bu.overflow == 0 && bf.overflow == 0);
   }
   TASK_CHECK(short_equal(ou, of, frame_size) == 0);
   speex_bits_destroy(&bu);
   speex_bits_destroy(&bf);
   return 0;
}

static int decoder_reset_matches_fresh(int mode_id, const struct enc_cfg *ecfg,
                                       const struct dec_cfg *dcfg, int losses)
{
   const SpeexMode *mode = speex_lib_get_mode(mode_id);
   void *enc, *used, *fresh;
   SpeexBits be;
   spx_int16_t in[MAX_FRAME], ou[MAX_FRAME], of[MAX_FRAME];
   char pkt[MAX_PACKET];
   int frame_size, k, n, nonzero_pcm = 0, loss;
   enc = speex_encoder_init(mode);
   used = speex_decoder_init(mode);
   TASK_CHECK(enc != NULL && used != NULL);
   TASK_CHECK(configure_encoder(enc, ecfg) == 0);
   TASK_CHECK(configure_decoder(used, dcfg) == 0);
   speex_bits_init(&be);
   speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame_size);

   /* Warm up with real audio, silence and lost frames. */
   for (k = 0; k < WARMUP; ++k) {
      if (k < WARMUP/2) make_frame(in, frame_size, k, 7);
      else silence_short(in, frame_size);
      speex_bits_reset(&be);
      TASK_CHECK(speex_encode_int(enc, in, &be) >= 0);
      n = speex_bits_write(&be, pkt, MAX_PACKET);
      {
         SpeexBits bd;
         speex_bits_init(&bd);
         speex_bits_read_from(&bd, pkt, n);
         TASK_CHECK(speex_decode_int(used, &bd, ou) == 0);
         speex_bits_destroy(&bd);
      }
      if (ou[0] || ou[frame_size/2]) nonzero_pcm = 1;
   }
   TASK_CHECK(nonzero_pcm);
   for (loss = 0; loss < WARMUP_LOSS; ++loss)
      TASK_CHECK(speex_decode_int(used, NULL, ou) == 0);

   TASK_CHECK(speex_decoder_ctl(used, SPEEX_RESET_STATE, NULL) == 0);
   TASK_CHECK(decoder_config_preserved(used, dcfg) == 0);

   fresh = speex_decoder_init(mode);
   TASK_CHECK(fresh != NULL);
   TASK_CHECK(configure_decoder(fresh, dcfg) == 0);

   for (k = 0; k < COMPARE; ++k) {
      make_frame(in, frame_size, k, 2);
      speex_bits_reset(&be);
      TASK_CHECK(speex_encode_int(enc, in, &be) >= 0);
      n = speex_bits_write(&be, pkt, MAX_PACKET);
      if (losses && (k % 3 == 1)) {
         TASK_CHECK(decode_pair(used, fresh, frame_size, NULL, 0, ou, of) == 0);
      } else {
         TASK_CHECK(decode_pair(used, fresh, frame_size, pkt, n, ou, of) == 0);
      }
   }
   speex_bits_destroy(&be);
   speex_encoder_destroy(enc);
   speex_decoder_destroy(used);
   speex_decoder_destroy(fresh);
   return 0;
}

static int dec_normal_packets(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      struct enc_cfg ecfg;
      struct dec_cfg dcfg;
      default_enc_cfg(&ecfg);
      default_dec_cfg(&dcfg);
      TASK_CHECK(decoder_reset_matches_fresh(all_modes[i], &ecfg, &dcfg, 0) == 0);
   }
   return 0;
}

static int dec_lost_packets(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      struct enc_cfg ecfg;
      struct dec_cfg dcfg;
      default_enc_cfg(&ecfg);
      default_dec_cfg(&dcfg);
      TASK_CHECK(decoder_reset_matches_fresh(all_modes[i], &ecfg, &dcfg, 1) == 0);
   }
   return 0;
}

static int dec_enh_highpass_options(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      struct enc_cfg ecfg;
      struct dec_cfg dcfg;
      default_enc_cfg(&ecfg);
      default_dec_cfg(&dcfg);
      dcfg.enh = 1;
      dcfg.highpass = 1;
      TASK_CHECK(decoder_reset_matches_fresh(all_modes[i], &ecfg, &dcfg, 1) == 0);
   }
   return 0;
}

/* In-band and user handlers must survive a reset, together with their data
 * pointers; subsequent audio must still decode exactly like a fresh handle. */
static int dec_inband_handlers(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      const SpeexMode *mode = speex_lib_get_mode(all_modes[i]);
      struct enc_cfg ecfg;
      struct dec_cfg dcfg;
      struct handler_ctx used_ctx, fresh_ctx;
      SpeexCallback used_cb[2], fresh_cb[2];
      void *enc, *used, *fresh;
      SpeexBits be, brq;
      spx_int16_t in[MAX_FRAME], ou[MAX_FRAME], of[MAX_FRAME];
      char pkt[MAX_PACKET], reqpkt[MAX_PACKET];
      int frame_size, k, n, reqlen;
      default_enc_cfg(&ecfg);
      default_dec_cfg(&dcfg);
      memset(&used_ctx, 0, sizeof(used_ctx));
      memset(&fresh_ctx, 0, sizeof(fresh_ctx));
      enc = speex_encoder_init(mode);
      used = speex_decoder_init(mode);
      TASK_CHECK(enc != NULL && used != NULL);
      TASK_CHECK(configure_encoder(enc, &ecfg) == 0);
      TASK_CHECK(configure_decoder(used, &dcfg) == 0);
      register_handlers(used, &used_ctx, used_cb);
      speex_bits_init(&be);
      speex_bits_init(&brq);
      speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame_size);
      for (k = 0; k < WARMUP; ++k) {
         make_frame(in, frame_size, k, 7);
         speex_bits_reset(&be);
         TASK_CHECK(speex_encode_int(enc, in, &be) >= 0);
         n = speex_bits_write(&be, pkt, MAX_PACKET);
         {
            SpeexBits bd;
            speex_bits_init(&bd);
            speex_bits_read_from(&bd, pkt, n);
            TASK_CHECK(speex_decode_int(used, &bd, ou) == 0);
            speex_bits_destroy(&bd);
         }
      }
      TASK_CHECK(build_request_packet(&brq, reqpkt, &reqlen) == 0);
      {
         SpeexBits bd;
         speex_bits_init(&bd);
         speex_bits_read_from(&bd, reqpkt, reqlen);
         (void)speex_decode_int(used, &bd, ou);   /* ends on terminator */
         speex_bits_destroy(&bd);
      }
      TASK_CHECK(used_ctx.inband_calls == 1);
      TASK_CHECK(used_ctx.user_calls == 1);

      TASK_CHECK(speex_decoder_ctl(used, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(decoder_config_preserved(used, &dcfg) == 0);

      fresh = speex_decoder_init(mode);
      TASK_CHECK(fresh != NULL);
      TASK_CHECK(configure_decoder(fresh, &dcfg) == 0);
      register_handlers(fresh, &fresh_ctx, fresh_cb);

      /* Handlers retained: the same request packet fires both again. */
      {
         SpeexBits du, df;
         speex_bits_init(&du);
         speex_bits_init(&df);
         speex_bits_read_from(&du, reqpkt, reqlen);
         speex_bits_read_from(&df, reqpkt, reqlen);
         (void)speex_decode_int(used, &du, ou);
         (void)speex_decode_int(fresh, &df, of);
         speex_bits_destroy(&du);
         speex_bits_destroy(&df);
      }
      TASK_CHECK(used_ctx.inband_calls == 2);
      TASK_CHECK(used_ctx.user_calls == 2);
      TASK_CHECK(fresh_ctx.inband_calls == 1);
      TASK_CHECK(fresh_ctx.user_calls == 1);

      for (k = 0; k < COMPARE; ++k) {
         make_frame(in, frame_size, k, 2);
         speex_bits_reset(&be);
         TASK_CHECK(speex_encode_int(enc, in, &be) >= 0);
         n = speex_bits_write(&be, pkt, MAX_PACKET);
         TASK_CHECK(decode_pair(used, fresh, frame_size, pkt, n, ou, of) == 0);
      }
      speex_bits_destroy(&be);
      speex_bits_destroy(&brq);
      speex_encoder_destroy(enc);
      speex_decoder_destroy(used);
      speex_decoder_destroy(fresh);
   }
   return 0;
}

/* Interleaved decoders, a repeated reset and a reset of a handle that was
 * only just initialised. */
static int dec_interleaved_double_fresh(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      const SpeexMode *mode = speex_lib_get_mode(all_modes[i]);
      struct enc_cfg ecfg;
      struct dec_cfg dcfg;
      void *enc, *a, *b, *ra, *rb, *rfresh;
      SpeexBits be;
      spx_int16_t in[MAX_FRAME], oa[MAX_FRAME], ob[MAX_FRAME],
                  ora[MAX_FRAME], orb[MAX_FRAME], orf[MAX_FRAME];
      char pkt[MAX_PACKET];
      int frame_size, k, n;
      default_enc_cfg(&ecfg);
      default_dec_cfg(&dcfg);
      enc = speex_encoder_init(mode);
      a = speex_decoder_init(mode);
      b = speex_decoder_init(mode);
      TASK_CHECK(enc && a && b);
      TASK_CHECK(configure_encoder(enc, &ecfg) == 0);
      TASK_CHECK(configure_decoder(a, &dcfg) == 0);
      TASK_CHECK(configure_decoder(b, &dcfg) == 0);
      speex_bits_init(&be);
      speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame_size);
      for (k = 0; k < WARMUP; ++k) {
         make_frame(in, frame_size, k, 7);
         speex_bits_reset(&be);
         TASK_CHECK(speex_encode_int(enc, in, &be) >= 0);
         n = speex_bits_write(&be, pkt, MAX_PACKET);
         {
            SpeexBits bd;
            speex_bits_init(&bd);
            speex_bits_read_from(&bd, pkt, n);
            TASK_CHECK(speex_decode_int(a, &bd, oa) == 0);
            speex_bits_destroy(&bd);
         }
         TASK_CHECK(speex_decode_int(b, NULL, ob) == 0);  /* losses on b */
      }
      TASK_CHECK(speex_decoder_ctl(a, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(speex_decoder_ctl(b, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(speex_decoder_ctl(b, SPEEX_RESET_STATE, NULL) == 0);

      rfresh = speex_decoder_init(mode);
      TASK_CHECK(rfresh != NULL);
      TASK_CHECK(speex_decoder_ctl(rfresh, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(configure_decoder(rfresh, &dcfg) == 0);

      ra = speex_decoder_init(mode);
      rb = speex_decoder_init(mode);
      TASK_CHECK(ra != NULL && rb != NULL);
      TASK_CHECK(configure_decoder(ra, &dcfg) == 0);
      TASK_CHECK(configure_decoder(rb, &dcfg) == 0);
      for (k = 0; k < COMPARE; ++k) {
         make_frame(in, frame_size, k, 2);
         speex_bits_reset(&be);
         TASK_CHECK(speex_encode_int(enc, in, &be) >= 0);
         n = speex_bits_write(&be, pkt, MAX_PACKET);
         TASK_CHECK(decode_pair(a, ra, frame_size, pkt, n, oa, ora) == 0);
         if (k % 2 == 0)
            TASK_CHECK(decode_pair(b, rb, frame_size, pkt, n, ob, orb) == 0);
         else
            TASK_CHECK(decode_pair(b, rb, frame_size, NULL, 0, ob, orb) == 0);
         /* rfresh was reset immediately after init; it decodes each packet
            once, so compare it with the once-decoding fresh handle ra. */
         {
            SpeexBits bf2;
            speex_bits_init(&bf2);
            speex_bits_read_from(&bf2, pkt, n);
            TASK_CHECK(speex_decode_int(rfresh, &bf2, orf) == 0);
            TASK_CHECK(bf2.overflow == 0);
            speex_bits_destroy(&bf2);
            TASK_CHECK(short_equal(orf, ora, frame_size) == 0);
         }
      }
      speex_bits_destroy(&be);
      speex_encoder_destroy(enc);
      speex_decoder_destroy(a);
      speex_decoder_destroy(b);
      speex_decoder_destroy(ra);
      speex_decoder_destroy(rb);
      speex_decoder_destroy(rfresh);
   }
   return 0;
}

/* Float decode API with the mandated tolerance and a bit-cursor check. */
static int dec_float_entry(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      const SpeexMode *mode = speex_lib_get_mode(all_modes[i]);
      struct enc_cfg ecfg;
      struct dec_cfg dcfg;
      void *enc, *used, *fresh;
      SpeexBits be, bu, bf;
      float fin[MAX_FRAME], ou[MAX_FRAME], of[MAX_FRAME];
      char pkt[MAX_PACKET];
      int frame_size, k, n, nonzero = 0;
      default_enc_cfg(&ecfg);
      default_dec_cfg(&dcfg);
      enc = speex_encoder_init(mode);
      used = speex_decoder_init(mode);
      TASK_CHECK(enc && used);
      TASK_CHECK(configure_encoder(enc, &ecfg) == 0);
      TASK_CHECK(configure_decoder(used, &dcfg) == 0);
      speex_bits_init(&be);
      speex_bits_init(&bu);
      speex_bits_init(&bf);
      speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame_size);
      for (k = 0; k < WARMUP; ++k) {
         make_float_frame(fin, frame_size, k, 7);
         speex_bits_reset(&be);
         TASK_CHECK(speex_encode(enc, fin, &be) >= 0);
         n = speex_bits_write(&be, pkt, MAX_PACKET);
         speex_bits_read_from(&bu, pkt, n);
         TASK_CHECK(speex_decode(used, &bu, ou) == 0);
         if (ou[0] != 0.0f) nonzero = 1;
      }
      TASK_CHECK(nonzero);
      TASK_CHECK(speex_decoder_ctl(used, SPEEX_RESET_STATE, NULL) == 0);
      fresh = speex_decoder_init(mode);
      TASK_CHECK(fresh != NULL);
      TASK_CHECK(configure_decoder(fresh, &dcfg) == 0);
      for (k = 0; k < COMPARE; ++k) {
         make_float_frame(fin, frame_size, k, 2);
         speex_bits_reset(&be);
         TASK_CHECK(speex_encode(enc, fin, &be) >= 0);
         n = speex_bits_write(&be, pkt, MAX_PACKET);
         speex_bits_read_from(&bu, pkt, n);
         speex_bits_read_from(&bf, pkt, n);
         TASK_CHECK(speex_decode(used, &bu, ou) == 0);
         TASK_CHECK(speex_decode(fresh, &bf, of) == 0);
         TASK_CHECK(bu.overflow == 0 && bf.overflow == 0);
         TASK_CHECK(bu.nbBits == bf.nbBits && bu.charPtr == bf.charPtr &&
                    bu.bitPtr == bf.bitPtr);
         TASK_CHECK(float_close(ou, of, frame_size) == 0);
      }
      speex_bits_destroy(&be);
      speex_bits_destroy(&bu);
      speex_bits_destroy(&bf);
      speex_encoder_destroy(enc);
      speex_decoder_destroy(used);
      speex_decoder_destroy(fresh);
   }
   return 0;
}

/* ------------------------------------------------------------------ */
/* Recursive (wideband / ultra-wideband nesting) cases                */
/* ------------------------------------------------------------------ */

static int recursive_enc_reset(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      struct enc_cfg cbr_cfg, vbr_cfg;
      default_enc_cfg(&cbr_cfg);
      default_enc_cfg(&vbr_cfg);
      vbr_cfg.vbr = 1; vbr_cfg.vad = 1; vbr_cfg.dtx = 1;
      TASK_CHECK(encoder_reset_matches_fresh(all_modes[i], &cbr_cfg) == 0);
      TASK_CHECK(encoder_reset_matches_fresh(all_modes[i], &vbr_cfg) == 0);
   }
   return 0;
}

static int recursive_dec_reset(void)
{
   int i;
   for (i = 0; i < 3; ++i) {
      struct enc_cfg ecfg;
      struct dec_cfg dcfg;
      default_enc_cfg(&ecfg);
      default_dec_cfg(&dcfg);
      ecfg.vbr = 1; ecfg.vad = 1;
      TASK_CHECK(decoder_reset_matches_fresh(all_modes[i], &ecfg, &dcfg, 1) == 0);
   }
   return 0;
}

/* Every QMF level plus the embedded low-band state must restart cleanly:
 * warm each WB/UWB handle with audio, silence and losses, reset it twice,
 * and compare against a fresh hierarchy. */
static int recursive_nested_double_reset(void)
{
   int i;
   for (i = 1; i < 3; ++i) {
      const SpeexMode *mode = speex_lib_get_mode(all_modes[i]);
      struct enc_cfg ecfg;
      struct dec_cfg dcfg;
      void *enc, *used, *fresh;
      SpeexBits be;
      spx_int16_t in[MAX_FRAME], ou[MAX_FRAME], of[MAX_FRAME];
      char pkt[MAX_PACKET];
      int frame_size, k, n, loss;
      default_enc_cfg(&ecfg);
      default_dec_cfg(&dcfg);
      enc = speex_encoder_init(mode);
      used = speex_decoder_init(mode);
      TASK_CHECK(enc && used);
      TASK_CHECK(configure_encoder(enc, &ecfg) == 0);
      TASK_CHECK(configure_decoder(used, &dcfg) == 0);
      speex_bits_init(&be);
      speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame_size);
      for (k = 0; k < WARMUP; ++k) {
         if (k < WARMUP/2) make_frame(in, frame_size, k, 7);
         else silence_short(in, frame_size);
         speex_bits_reset(&be);
         TASK_CHECK(speex_encode_int(enc, in, &be) >= 0);
         n = speex_bits_write(&be, pkt, MAX_PACKET);
         if (k % 4 == 3) {
            TASK_CHECK(speex_decode_int(used, NULL, ou) == 0);
         } else {
            SpeexBits bd;
            speex_bits_init(&bd);
            speex_bits_read_from(&bd, pkt, n);
            TASK_CHECK(speex_decode_int(used, &bd, ou) == 0);
            speex_bits_destroy(&bd);
         }
      }
      for (loss = 0; loss < WARMUP_LOSS; ++loss)
         TASK_CHECK(speex_decode_int(used, NULL, ou) == 0);
      TASK_CHECK(speex_decoder_ctl(used, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(speex_decoder_ctl(used, SPEEX_RESET_STATE, NULL) == 0);

      fresh = speex_decoder_init(mode);
      TASK_CHECK(fresh != NULL);
      TASK_CHECK(configure_decoder(fresh, &dcfg) == 0);
      for (k = 0; k < COMPARE; ++k) {
         make_frame(in, frame_size, k, 2);
         speex_bits_reset(&be);
         TASK_CHECK(speex_encode_int(enc, in, &be) >= 0);
         n = speex_bits_write(&be, pkt, MAX_PACKET);
         TASK_CHECK(decode_pair(used, fresh, frame_size, pkt, n, ou, of) == 0);
      }
      speex_bits_destroy(&be);
      speex_encoder_destroy(enc);
      speex_decoder_destroy(used);
      speex_decoder_destroy(fresh);
   }
   return 0;
}

static int recursive_float_roundtrip(void)
{
   int i;
   for (i = 1; i < 3; ++i) {
      const SpeexMode *mode = speex_lib_get_mode(all_modes[i]);
      struct enc_cfg ecfg;
      struct dec_cfg dcfg;
      void *enc, *used, *fresh;
      SpeexBits be, bu, bf;
      float fin[MAX_FRAME], ou[MAX_FRAME], of[MAX_FRAME];
      char pkt[MAX_PACKET];
      int frame_size, k, n;
      default_enc_cfg(&ecfg);
      default_dec_cfg(&dcfg);
      ecfg.vbr = 1;
      enc = speex_encoder_init(mode);
      used = speex_decoder_init(mode);
      TASK_CHECK(enc && used);
      TASK_CHECK(configure_encoder(enc, &ecfg) == 0);
      TASK_CHECK(configure_decoder(used, &dcfg) == 0);
      speex_bits_init(&be);
      speex_bits_init(&bu);
      speex_bits_init(&bf);
      speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame_size);
      for (k = 0; k < WARMUP; ++k) {
         make_float_frame(fin, frame_size, k, 7);
         speex_bits_reset(&be);
         TASK_CHECK(speex_encode(enc, fin, &be) >= 0);
         n = speex_bits_write(&be, pkt, MAX_PACKET);
         speex_bits_read_from(&bu, pkt, n);
         TASK_CHECK(speex_decode(used, &bu, ou) == 0);
      }
      TASK_CHECK(speex_decoder_ctl(used, SPEEX_RESET_STATE, NULL) == 0);
      fresh = speex_decoder_init(mode);
      TASK_CHECK(fresh != NULL);
      TASK_CHECK(configure_decoder(fresh, &dcfg) == 0);
      for (k = 0; k < COMPARE; ++k) {
         make_float_frame(fin, frame_size, k, 2);
         speex_bits_reset(&be);
         TASK_CHECK(speex_encode(enc, fin, &be) >= 0);
         n = speex_bits_write(&be, pkt, MAX_PACKET);
         speex_bits_read_from(&bu, pkt, n);
         speex_bits_read_from(&bf, pkt, n);
         TASK_CHECK(speex_decode(used, &bu, ou) == 0);
         TASK_CHECK(speex_decode(fresh, &bf, of) == 0);
         TASK_CHECK(bu.nbBits == bf.nbBits && bu.charPtr == bf.charPtr &&
                    bu.bitPtr == bf.bitPtr && bu.overflow == bf.overflow);
         TASK_CHECK(float_close(ou, of, frame_size) == 0);
      }
      speex_bits_destroy(&be);
      speex_bits_destroy(&bu);
      speex_bits_destroy(&bf);
      speex_encoder_destroy(enc);
      speex_decoder_destroy(used);
      speex_decoder_destroy(fresh);
   }
   return 0;
}

const struct task_case task_cases[] = {
   {"encoder", "cbr_reset_matches_fresh",            enc_cbr},
   {"encoder", "vbr_reset_matches_fresh",            enc_vbr},
   {"encoder", "vbr_dtx_silence_recovery",           enc_vbr_dtx_recovery},
   {"encoder", "explicit_mode_reset",                enc_explicit_mode},
   {"encoder", "mid_session_control_update",         enc_control_update},
   {"encoder", "interleaved_and_double_reset",       enc_interleaved_double_reset},
   {"encoder", "reset_immediately_after_init",       enc_reset_fresh_handle},
   {"encoder", "float_entry_packets_and_cursor",     enc_float_entry},
   {"encoder", "unreset_continuous_stream_unchanged",enc_no_reset_unchanged},
   {"decoder", "normal_packets_after_reset",         dec_normal_packets},
   {"decoder", "lost_packets_after_reset",           dec_lost_packets},
   {"decoder", "enhancement_highpass_options",       dec_enh_highpass_options},
   {"decoder", "inband_and_user_handlers_kept",      dec_inband_handlers},
   {"decoder", "interleaved_double_and_fresh_reset", dec_interleaved_double_fresh},
   {"decoder", "float_entry_tolerance_and_cursor",   dec_float_entry},
   {"recursive", "enc_reset_all_bandwidths",         recursive_enc_reset},
   {"recursive", "dec_reset_all_bandwidths",         recursive_dec_reset},
   {"recursive", "nested_qmf_double_reset",          recursive_nested_double_reset},
   {"recursive", "float_recursive_roundtrip",        recursive_float_roundtrip},
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
