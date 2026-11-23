/* Regression tests for SPEEX_RESET_STATE.
 *
 * A "seasoned" Speex handle first processes enough frames (speech, silence,
 * DTX and lost packets) to accumulate non-zero session history, then receives
 * SPEEX_RESET_STATE.  Every frame produced afterwards is compared with the
 * output of an independent, freshly created handle that was configured with
 * the same controls in the same order.  Encoder comparisons cover the whole
 * packet (number of bits, bit cursor and every byte), decoder comparisons are
 * sample-by-sample PCM comparisons.
 *
 * There is no main() here: task_support/driver.c drives task_cases[].
 */

#include "speex/speex.h"
#include "speex/speex_callbacks.h"
#include "../task_support/check.h"
#include <math.h>
#include <string.h>

#define HIST_FRAMES 14
#define DTX_HIST_FRAMES 30
#define POST_FRAMES 10
#define MAX_FRAME 640
#define PACKET_CAP 256

static const int mode_ids[3] = {SPEEX_MODEID_NB, SPEEX_MODEID_WB, SPEEX_MODEID_UWB};

struct enc_cfg {
   int quality;
   int complexity;
   int vbr;
   float vbr_quality;
   int vad;
   int dtx;
   int highpass;
   int sampling_rate;
   int plc_tuning;
   int submode_enc;
};

struct dec_cfg {
   int enh;
   int highpass;
   int sampling_rate;
   int submode_enc;
};

struct packet {
   char bytes[PACKET_CAP];
   int len;
};

/* Deterministic, non-zero test signal.  kind 0 is silence, kind 1 is a
   mixed waveform, kind 2 adds low-frequency periodic content. */
static void fill_frame(spx_int16_t *p, int n, int frame, int kind)
{
   int i;
   for (i = 0; i < n; ++i) {
      int k = frame * n + i;
      if (kind == 0)
         p[i] = 0;
      else
         p[i] = (spx_int16_t)(((k * 13) % 201 - 100) * 35
                              + ((k * 7) % 57 - 28) * 18
                              + (kind == 2 ? (i % 33) * 7 : 0));
   }
}

static void *new_encoder(int mode_id)
{
   return speex_encoder_init(speex_lib_get_mode(mode_id));
}

static void *new_decoder(int mode_id)
{
   return speex_decoder_init(speex_lib_get_mode(mode_id));
}

static int frame_size_of(void *st, int encoder)
{
   int n = 0;
   if (encoder)
      speex_encoder_ctl(st, SPEEX_GET_FRAME_SIZE, &n);
   else
      speex_decoder_ctl(st, SPEEX_GET_FRAME_SIZE, &n);
   return n;
}

/* The same sequence of control requests is replayed on the fresh handle. */
static void apply_enc_cfg(void *st, const struct enc_cfg *c)
{
   int i;
   float f;
   i = c->quality;          speex_encoder_ctl(st, SPEEX_SET_QUALITY, &i);
   i = c->complexity;       speex_encoder_ctl(st, SPEEX_SET_COMPLEXITY, &i);
   i = c->highpass;         speex_encoder_ctl(st, SPEEX_SET_HIGHPASS, &i);
   i = c->sampling_rate;    speex_encoder_ctl(st, SPEEX_SET_SAMPLING_RATE, &i);
   i = c->submode_enc;      speex_encoder_ctl(st, SPEEX_SET_SUBMODE_ENCODING, &i);
   i = c->plc_tuning;       speex_encoder_ctl(st, SPEEX_SET_PLC_TUNING, &i);
   i = c->vbr;              speex_encoder_ctl(st, SPEEX_SET_VBR, &i);
   f = c->vbr_quality;      speex_encoder_ctl(st, SPEEX_SET_VBR_QUALITY, &f);
   i = c->vad;              speex_encoder_ctl(st, SPEEX_SET_VAD, &i);
   i = c->dtx;              speex_encoder_ctl(st, SPEEX_SET_DTX, &i);
}

static void apply_dec_cfg(void *st, const struct dec_cfg *c)
{
   int i;
   i = c->enh;              speex_decoder_ctl(st, SPEEX_SET_ENH, &i);
   i = c->highpass;         speex_decoder_ctl(st, SPEEX_SET_HIGHPASS, &i);
   i = c->sampling_rate;    speex_decoder_ctl(st, SPEEX_SET_SAMPLING_RATE, &i);
   i = c->submode_enc;      speex_decoder_ctl(st, SPEEX_SET_SUBMODE_ENCODING, &i);
}

static int check_enc_cfg_kept(void *old_st, void *fresh_st)
{
   int io, ifr;
   float fo, ff;

   /* The instantaneous sub-mode is VBR/VAD driven and explicitly not user
      configuration, so it is not compared here; identical post-reset packets
      validate the effective mode selection instead. */
   speex_encoder_ctl(old_st, SPEEX_GET_COMPLEXITY, &io);
   speex_encoder_ctl(fresh_st, SPEEX_GET_COMPLEXITY, &ifr);
   TASK_CHECK(io == ifr);
   speex_encoder_ctl(old_st, SPEEX_GET_HIGHPASS, &io);
   speex_encoder_ctl(fresh_st, SPEEX_GET_HIGHPASS, &ifr);
   TASK_CHECK(io == ifr);
   speex_encoder_ctl(old_st, SPEEX_GET_SAMPLING_RATE, &io);
   speex_encoder_ctl(fresh_st, SPEEX_GET_SAMPLING_RATE, &ifr);
   TASK_CHECK(io == ifr);
   speex_encoder_ctl(old_st, SPEEX_GET_SUBMODE_ENCODING, &io);
   speex_encoder_ctl(fresh_st, SPEEX_GET_SUBMODE_ENCODING, &ifr);
   TASK_CHECK(io == ifr);
   speex_encoder_ctl(old_st, SPEEX_GET_PLC_TUNING, &io);
   speex_encoder_ctl(fresh_st, SPEEX_GET_PLC_TUNING, &ifr);
   TASK_CHECK(io == ifr);
   speex_encoder_ctl(old_st, SPEEX_GET_VBR, &io);
   speex_encoder_ctl(fresh_st, SPEEX_GET_VBR, &ifr);
   TASK_CHECK(io == ifr);
   speex_encoder_ctl(old_st, SPEEX_GET_VAD, &io);
   speex_encoder_ctl(fresh_st, SPEEX_GET_VAD, &ifr);
   TASK_CHECK(io == ifr);
   speex_encoder_ctl(old_st, SPEEX_GET_DTX, &io);
   speex_encoder_ctl(fresh_st, SPEEX_GET_DTX, &ifr);
   TASK_CHECK(io == ifr);
   speex_encoder_ctl(old_st, SPEEX_GET_VBR_QUALITY, &fo);
   speex_encoder_ctl(fresh_st, SPEEX_GET_VBR_QUALITY, &ff);
   TASK_CHECK(fo == ff);
   return 0;
}

static int check_dec_cfg_kept(void *old_st, void *fresh_st)
{
   int io, ifr;
   speex_decoder_ctl(old_st, SPEEX_GET_ENH, &io);
   speex_decoder_ctl(fresh_st, SPEEX_GET_ENH, &ifr);
   TASK_CHECK(io == ifr);
   speex_decoder_ctl(old_st, SPEEX_GET_HIGHPASS, &io);
   speex_decoder_ctl(fresh_st, SPEEX_GET_HIGHPASS, &ifr);
   TASK_CHECK(io == ifr);
   speex_decoder_ctl(old_st, SPEEX_GET_SAMPLING_RATE, &io);
   speex_decoder_ctl(fresh_st, SPEEX_GET_SAMPLING_RATE, &ifr);
   TASK_CHECK(io == ifr);
   speex_decoder_ctl(old_st, SPEEX_GET_SUBMODE_ENCODING, &io);
   speex_decoder_ctl(fresh_st, SPEEX_GET_SUBMODE_ENCODING, &ifr);
   TASK_CHECK(io == ifr);
   return 0;
}

/* Encode one frame on both handles and compare the complete packet. */
static int compare_encoded(void *old_st, void *fresh_st, SpeexBits *bo, SpeexBits *bf,
                           const spx_int16_t *in, int n, int use_float)
{
   spx_int16_t in_a[MAX_FRAME], in_b[MAX_FRAME];
   float fa[MAX_FRAME], fb[MAX_FRAME];
   char pa[PACKET_CAP], pb[PACKET_CAP];
   int ra, rb, na, nb, i;

   memcpy(in_a, in, (size_t)n * sizeof(spx_int16_t));
   memcpy(in_b, in, (size_t)n * sizeof(spx_int16_t));
   speex_bits_reset(bo);
   speex_bits_reset(bf);
   if (use_float) {
      for (i = 0; i < n; ++i) {
         fa[i] = (float)in[i];
         fb[i] = (float)in[i];
      }
      ra = speex_encode(old_st, fa, bo);
      rb = speex_encode(fresh_st, fb, bf);
   } else {
      ra = speex_encode_int(old_st, in_a, bo);
      rb = speex_encode_int(fresh_st, in_b, bf);
   }
   TASK_CHECK(ra == rb);
   TASK_CHECK(bo->nbBits == bf->nbBits);
   TASK_CHECK(bo->nbBits > 0);
   /* Bit cursor must sit exactly at the end of the packed data. */
   TASK_CHECK(bo->charPtr * 8 + bo->bitPtr == bo->nbBits);
   TASK_CHECK(bf->charPtr * 8 + bf->bitPtr == bf->nbBits);
   TASK_CHECK(bo->overflow == 0 && bf->overflow == 0);
   na = speex_bits_write(bo, pa, sizeof(pa));
   nb = speex_bits_write(bf, pb, sizeof(pb));
   TASK_CHECK(na == nb);
   TASK_CHECK(memcmp(pa, pb, (size_t)na) == 0);
   speex_bits_rewind(bo);
   speex_bits_rewind(bf);
   TASK_CHECK(bo->charPtr == 0 && bo->bitPtr == 0);
   TASK_CHECK(bf->charPtr == 0 && bf->bitPtr == 0);
   return 0;
}

/* Decode one frame (normal packet or NULL-bits loss) on both handles and
   compare every output sample. */
static int compare_decoded(void *old_st, void *fresh_st, const struct packet *pkt,
                           SpeexBits *bo, SpeexBits *bf, int n, int lost, int use_float)
{
   spx_int16_t out_a[MAX_FRAME], out_b[MAX_FRAME];
   float fa[MAX_FRAME], fb[MAX_FRAME];
   int ra, rb, i;

   if (lost) {
      if (use_float) {
         ra = speex_decode(old_st, NULL, fa);
         rb = speex_decode(fresh_st, NULL, fb);
      } else {
         ra = speex_decode_int(old_st, NULL, out_a);
         rb = speex_decode_int(fresh_st, NULL, out_b);
      }
   } else {
      speex_bits_read_from(bo, pkt->bytes, pkt->len);
      speex_bits_read_from(bf, pkt->bytes, pkt->len);
      if (use_float) {
         ra = speex_decode(old_st, bo, fa);
         rb = speex_decode(fresh_st, bf, fb);
      } else {
         ra = speex_decode_int(old_st, bo, out_a);
         rb = speex_decode_int(fresh_st, bf, out_b);
      }
      TASK_CHECK(speex_bits_remaining(bo) == speex_bits_remaining(bf));
   }
   TASK_CHECK(ra == rb);
   if (ra != 0)
      return 0;
   if (use_float) {
      for (i = 0; i < n; ++i)
         TASK_CHECK(fabsf(fa[i] - fb[i]) <= 1e-5f + 1e-6f * fabsf(fb[i]));
   } else {
      for (i = 0; i < n; ++i)
         TASK_CHECK(out_a[i] == out_b[i]);
   }
   return 0;
}

/* Generate a shared packet stream with an independent encoder. */
static void make_packets(int mode_id, struct packet *pkts, int count)
{
   void *enc = new_encoder(mode_id);
   SpeexBits bits;
   spx_int16_t frame[MAX_FRAME];
   int n = frame_size_of(enc, 1), k, quality = 5, complexity = 3;
   speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &quality);
   speex_encoder_ctl(enc, SPEEX_SET_COMPLEXITY, &complexity);
   speex_bits_init(&bits);
   for (k = 0; k < count; ++k) {
      fill_frame(frame, n, k, (k % 5 == 0) ? 0 : 1);
      speex_bits_reset(&bits);
      speex_encode_int(enc, frame, &bits);
      pkts[k].len = speex_bits_write(&bits, pkts[k].bytes, PACKET_CAP);
   }
   speex_bits_destroy(&bits);
   speex_encoder_destroy(enc);
}

static int is_lost(int frame_index)
{
   /* Includes losses on the first frames after a reset. */
   return frame_index == 0 || frame_index == 1 || frame_index == 4 || frame_index == 7;
}

static int post_kind(int frame_index)
{
   if (frame_index < 4)
      return 0;
   return (frame_index & 1) ? 2 : 1;
}

/* ----------------------------- encoder group ----------------------------- */

static int run_enc_cbr(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct enc_cfg cfg = {5, 3, 0, 5.0f, 0, 0, 0, mode_ids[m] == 0 ? 8000 :
                            (mode_ids[m] == 1 ? 16000 : 32000), 37, 1};
      void *old_st = new_encoder(mode_ids[m]);
      SpeexBits bo, bf;
      spx_int16_t frame[MAX_FRAME];
      int n = frame_size_of(old_st, 1), k;
      apply_enc_cfg(old_st, &cfg);
      speex_bits_init(&bo);
      speex_bits_init(&bf);
      for (k = 0; k < HIST_FRAMES; ++k) {
         fill_frame(frame, n, k, k % 4 == 0 ? 0 : 1);
         speex_bits_reset(&bo);
         speex_encode_int(old_st, frame, &bo);
      }
      TASK_CHECK(speex_encoder_ctl(old_st, SPEEX_RESET_STATE, NULL) == 0);
      {
         void *fresh_st = new_encoder(mode_ids[m]);
         apply_enc_cfg(fresh_st, &cfg);
         TASK_CHECK(check_enc_cfg_kept(old_st, fresh_st) == 0);
         for (k = 0; k < POST_FRAMES; ++k) {
            fill_frame(frame, n, k + 100, post_kind(k));
            TASK_CHECK(compare_encoded(old_st, fresh_st, &bo, &bf, frame, n, 0) == 0);
            TASK_CHECK(compare_encoded(old_st, fresh_st, &bo, &bf, frame, n, 1) == 0);
         }
         speex_encoder_destroy(fresh_st);
      }
      speex_bits_destroy(&bo);
      speex_bits_destroy(&bf);
      speex_encoder_destroy(old_st);
   }
   return 0;
}

static int run_enc_vbr(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct enc_cfg cfg = {6, 4, 1, 6.25f, 1, 0, 1,
                            mode_ids[m] == 0 ? 8000 :
                            (mode_ids[m] == 1 ? 16000 : 32000), 12, 1};
      void *old_st = new_encoder(mode_ids[m]);
      SpeexBits bo, bf;
      spx_int16_t frame[MAX_FRAME];
      int n = frame_size_of(old_st, 1), k;
      apply_enc_cfg(old_st, &cfg);
      speex_bits_init(&bo);
      speex_bits_init(&bf);
      for (k = 0; k < HIST_FRAMES; ++k) {
         fill_frame(frame, n, k, k % 3 == 0 ? 2 : 1);
         speex_bits_reset(&bo);
         speex_encode_int(old_st, frame, &bo);
      }
      TASK_CHECK(speex_encoder_ctl(old_st, SPEEX_RESET_STATE, NULL) == 0);
      {
         void *fresh_st = new_encoder(mode_ids[m]);
         apply_enc_cfg(fresh_st, &cfg);
         TASK_CHECK(check_enc_cfg_kept(old_st, fresh_st) == 0);
         for (k = 0; k < POST_FRAMES; ++k) {
            fill_frame(frame, n, k + 200, post_kind(k));
            TASK_CHECK(compare_encoded(old_st, fresh_st, &bo, &bf, frame, n, 0) == 0);
            TASK_CHECK(compare_encoded(old_st, fresh_st, &bo, &bf, frame, n, 1) == 0);
         }
         speex_encoder_destroy(fresh_st);
      }
      speex_bits_destroy(&bo);
      speex_bits_destroy(&bf);
      speex_encoder_destroy(old_st);
   }
   return 0;
}

static int run_enc_dtx(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct enc_cfg cfg = {4, 3, 1, 4.0f, 1, 1, 1,
                            mode_ids[m] == 0 ? 8000 :
                            (mode_ids[m] == 1 ? 16000 : 32000), 2, 1};
      void *old_st = new_encoder(mode_ids[m]);
      SpeexBits bo, bf;
      spx_int16_t frame[MAX_FRAME];
      int n = frame_size_of(old_st, 1), k, saw_dtx = 0, ret;
      apply_enc_cfg(old_st, &cfg);
      speex_bits_init(&bo);
      speex_bits_init(&bf);
      /* Speech followed by a long silence: the handle must really enter DTX. */
      for (k = 0; k < DTX_HIST_FRAMES; ++k) {
         fill_frame(frame, n, k, k < 6 ? 2 : 0);
         speex_bits_reset(&bo);
         ret = speex_encode_int(old_st, frame, &bo);
         if (ret == 0)
            saw_dtx = 1;
      }
      TASK_CHECK(saw_dtx);
      TASK_CHECK(speex_encoder_ctl(old_st, SPEEX_RESET_STATE, NULL) == 0);
      {
         void *fresh_st = new_encoder(mode_ids[m]);
         apply_enc_cfg(fresh_st, &cfg);
         TASK_CHECK(check_enc_cfg_kept(old_st, fresh_st) == 0);
         /* Silence first (DTX entry) then active speech (DTX recovery). */
         for (k = 0; k < POST_FRAMES; ++k) {
            fill_frame(frame, n, k + 300, post_kind(k));
            TASK_CHECK(compare_encoded(old_st, fresh_st, &bo, &bf, frame, n, 0) == 0);
         }
         speex_encoder_destroy(fresh_st);
      }
      speex_bits_destroy(&bo);
      speex_bits_destroy(&bf);
      speex_encoder_destroy(old_st);
   }
   return 0;
}

static int run_enc_control_change(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct enc_cfg first_cfg = {3, 1, 0, 3.0f, 0, 0, 1,
                                  mode_ids[m] == 0 ? 8000 :
                                  (mode_ids[m] == 1 ? 16000 : 32000), 10, 1};
      struct enc_cfg final_cfg = {7, 4, 1, 7.5f, 1, 1, 0,
                                  mode_ids[m] == 0 ? 8000 :
                                  (mode_ids[m] == 1 ? 16000 : 32000), 55, 0};
      void *old_st = new_encoder(mode_ids[m]);
      SpeexBits bo, bf;
      spx_int16_t frame[MAX_FRAME];
      int n = frame_size_of(old_st, 1), k;
      apply_enc_cfg(old_st, &first_cfg);
      speex_bits_init(&bo);
      speex_bits_init(&bf);
      for (k = 0; k < HIST_FRAMES / 2; ++k) {
         fill_frame(frame, n, k, 1);
         speex_bits_reset(&bo);
         speex_encode_int(old_st, frame, &bo);
      }
      /* Controls change mid-session. */
      apply_enc_cfg(old_st, &final_cfg);
      for (k = HIST_FRAMES / 2; k < HIST_FRAMES; ++k) {
         fill_frame(frame, n, k, k % 3 ? 2 : 0);
         speex_bits_reset(&bo);
         speex_encode_int(old_st, frame, &bo);
      }
      TASK_CHECK(speex_encoder_ctl(old_st, SPEEX_RESET_STATE, NULL) == 0);
      {
         void *fresh_st = new_encoder(mode_ids[m]);
         /* Same requests, same order on the reference handle. */
         apply_enc_cfg(fresh_st, &first_cfg);
         apply_enc_cfg(fresh_st, &final_cfg);
         TASK_CHECK(check_enc_cfg_kept(old_st, fresh_st) == 0);
         for (k = 0; k < POST_FRAMES; ++k) {
            fill_frame(frame, n, k + 400, post_kind(k));
            TASK_CHECK(compare_encoded(old_st, fresh_st, &bo, &bf, frame, n, 0) == 0);
         }
         speex_encoder_destroy(fresh_st);
      }
      speex_bits_destroy(&bo);
      speex_bits_destroy(&bf);
      speex_encoder_destroy(old_st);
   }
   return 0;
}

static int run_enc_fresh_and_continuous(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct enc_cfg cfg = {5, 2, 1, 5.5f, 1, 0, 0,
                            mode_ids[m] == 0 ? 8000 :
                            (mode_ids[m] == 1 ? 16000 : 32000), 5, 1};
      void *a = new_encoder(mode_ids[m]);
      void *b = new_encoder(mode_ids[m]);
      SpeexBits ba, bb;
      spx_int16_t frame[MAX_FRAME];
      int n = frame_size_of(a, 1), k;
      speex_bits_init(&ba);
      speex_bits_init(&bb);
      /* Reset immediately (twice) on a brand new handle. */
      TASK_CHECK(speex_encoder_ctl(a, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(speex_encoder_ctl(a, SPEEX_RESET_STATE, NULL) == 0);
      apply_enc_cfg(a, &cfg);
      apply_enc_cfg(b, &cfg);
      /* Reset after configuration but before any frame: output must be
         unchanged relative to the untouched, identically configured handle. */
      TASK_CHECK(speex_encoder_ctl(a, SPEEX_RESET_STATE, NULL) == 0);
      for (k = 0; k < POST_FRAMES; ++k) {
         fill_frame(frame, n, k + 500, post_kind(k));
         /* No reset on the continuous path: packets must stay identical. */
         TASK_CHECK(compare_encoded(a, b, &ba, &bb, frame, n, 0) == 0);
      }
      speex_bits_destroy(&ba);
      speex_bits_destroy(&bb);
      speex_encoder_destroy(a);
      speex_encoder_destroy(b);
   }
   return 0;
}

static int run_enc_interleaved_double_reset(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct enc_cfg cfg_a = {4, 2, 0, 4.0f, 0, 0, 1,
                              mode_ids[m] == 0 ? 8000 :
                              (mode_ids[m] == 1 ? 16000 : 32000), 8, 0};
      struct enc_cfg cfg_b = {8, 5, 1, 8.25f, 1, 1, 0,
                              mode_ids[m] == 0 ? 8000 :
                              (mode_ids[m] == 1 ? 16000 : 32000), 20, 1};
      void *old_a = new_encoder(mode_ids[m]), *old_b = new_encoder(mode_ids[m]);
      SpeexBits ba, bb, bfa, bfb;
      spx_int16_t frame[MAX_FRAME];
      int n = frame_size_of(old_a, 1), k;
      void *fresh_a, *fresh_b;
      apply_enc_cfg(old_a, &cfg_a);
      apply_enc_cfg(old_b, &cfg_b);
      speex_bits_init(&ba);
      speex_bits_init(&bb);
      speex_bits_init(&bfa);
      speex_bits_init(&bfb);
      for (k = 0; k < HIST_FRAMES; ++k) {
         fill_frame(frame, n, k, 1);
         speex_bits_reset(&ba);
         speex_encode_int(old_a, frame, &ba);
         fill_frame(frame, n, k, 2);
         speex_bits_reset(&bb);
         speex_encode_int(old_b, frame, &bb);
      }
      /* First reset, a little more history, then a second reset. */
      TASK_CHECK(speex_encoder_ctl(old_a, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(speex_encoder_ctl(old_b, SPEEX_RESET_STATE, NULL) == 0);
      for (k = 0; k < 4; ++k) {
         fill_frame(frame, n, k, 2);
         speex_bits_reset(&ba);
         speex_encode_int(old_a, frame, &ba);
         fill_frame(frame, n, k, 0);
         speex_bits_reset(&bb);
         speex_encode_int(old_b, frame, &bb);
      }
      TASK_CHECK(speex_encoder_ctl(old_a, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(speex_encoder_ctl(old_b, SPEEX_RESET_STATE, NULL) == 0);
      fresh_a = new_encoder(mode_ids[m]);
      fresh_b = new_encoder(mode_ids[m]);
      apply_enc_cfg(fresh_a, &cfg_a);
      apply_enc_cfg(fresh_b, &cfg_b);
      TASK_CHECK(check_enc_cfg_kept(old_a, fresh_a) == 0);
      TASK_CHECK(check_enc_cfg_kept(old_b, fresh_b) == 0);
      for (k = 0; k < POST_FRAMES; ++k) {
         fill_frame(frame, n, k + 600, 1);
         TASK_CHECK(compare_encoded(old_a, fresh_a, &ba, &bfa, frame, n, 0) == 0);
         fill_frame(frame, n, k + 600, 2);
         TASK_CHECK(compare_encoded(old_b, fresh_b, &bb, &bfb, frame, n, 0) == 0);
      }
      speex_encoder_destroy(fresh_a);
      speex_encoder_destroy(fresh_b);
      speex_bits_destroy(&ba);
      speex_bits_destroy(&bb);
      speex_bits_destroy(&bfa);
      speex_bits_destroy(&bfb);
      speex_encoder_destroy(old_a);
      speex_encoder_destroy(old_b);
   }
   return 0;
}

/* ----------------------------- decoder group ----------------------------- */

static int run_dec_packets(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct packet pkts[HIST_FRAMES + POST_FRAMES];
      struct dec_cfg cfg = {1, 1,
                            mode_ids[m] == 0 ? 8000 :
                            (mode_ids[m] == 1 ? 16000 : 32000), 1};
      void *old_st = new_decoder(mode_ids[m]);
      SpeexBits bo, bf;
      int n = frame_size_of(old_st, 0), k;
      make_packets(mode_ids[m], pkts, HIST_FRAMES + POST_FRAMES);
      apply_dec_cfg(old_st, &cfg);
      speex_bits_init(&bo);
      speex_bits_init(&bf);
      for (k = 0; k < HIST_FRAMES; ++k) {
         speex_bits_read_from(&bo, pkts[k].bytes, pkts[k].len);
         TASK_CHECK(speex_decode_int(old_st, &bo, (spx_int16_t[640]){0}) == 0);
      }
      TASK_CHECK(speex_decoder_ctl(old_st, SPEEX_RESET_STATE, NULL) == 0);
      {
         void *fresh_st = new_decoder(mode_ids[m]);
         apply_dec_cfg(fresh_st, &cfg);
         TASK_CHECK(check_dec_cfg_kept(old_st, fresh_st) == 0);
         for (k = 0; k < POST_FRAMES; ++k) {
            TASK_CHECK(compare_decoded(old_st, fresh_st, &pkts[HIST_FRAMES + k],
                                       &bo, &bf, n, 0, 0) == 0);
            TASK_CHECK(compare_decoded(old_st, fresh_st, &pkts[HIST_FRAMES + k],
                                       &bo, &bf, n, 0, 1) == 0);
         }
         speex_decoder_destroy(fresh_st);
      }
      speex_bits_destroy(&bo);
      speex_bits_destroy(&bf);
      speex_decoder_destroy(old_st);
   }
   return 0;
}

static int run_dec_lost(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct packet pkts[HIST_FRAMES + POST_FRAMES];
      struct dec_cfg cfg = {1, 1,
                            mode_ids[m] == 0 ? 8000 :
                            (mode_ids[m] == 1 ? 16000 : 32000), 1};
      void *old_st = new_decoder(mode_ids[m]);
      SpeexBits bo, bf;
      spx_int16_t out[MAX_FRAME];
      int n = frame_size_of(old_st, 0), k, saw_loss = 0;
      make_packets(mode_ids[m], pkts, HIST_FRAMES + POST_FRAMES);
      apply_dec_cfg(old_st, &cfg);
      speex_bits_init(&bo);
      speex_bits_init(&bf);
      /* History includes consecutive NULL-bits lost frames. */
      for (k = 0; k < HIST_FRAMES; ++k) {
         if (is_lost(k)) {
            TASK_CHECK(speex_decode_int(old_st, NULL, out) == 0);
            saw_loss = 1;
         } else {
            speex_bits_read_from(&bo, pkts[k].bytes, pkts[k].len);
            TASK_CHECK(speex_decode_int(old_st, &bo, out) == 0);
         }
      }
      TASK_CHECK(saw_loss);
      TASK_CHECK(speex_decoder_ctl(old_st, SPEEX_RESET_STATE, NULL) == 0);
      {
         void *fresh_st = new_decoder(mode_ids[m]);
         apply_dec_cfg(fresh_st, &cfg);
         TASK_CHECK(check_dec_cfg_kept(old_st, fresh_st) == 0);
         for (k = 0; k < POST_FRAMES; ++k) {
            int lost = is_lost(k);
            TASK_CHECK(compare_decoded(old_st, fresh_st, &pkts[HIST_FRAMES + k],
                                       &bo, &bf, n, lost, 0) == 0);
            TASK_CHECK(compare_decoded(old_st, fresh_st, &pkts[HIST_FRAMES + k],
                                       &bo, &bf, n, lost, 1) == 0);
         }
         speex_decoder_destroy(fresh_st);
      }
      speex_bits_destroy(&bo);
      speex_bits_destroy(&bf);
      speex_decoder_destroy(old_st);
   }
   return 0;
}

static int run_dec_enh_highpass(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct packet pkts[HIST_FRAMES + POST_FRAMES];
      /* Non-default enhancement and high-pass settings. */
      struct dec_cfg cfg = {0, 0,
                            mode_ids[m] == 0 ? 8000 :
                            (mode_ids[m] == 1 ? 16000 : 32000), 1};
      void *old_st = new_decoder(mode_ids[m]);
      SpeexBits bo, bf;
      spx_int16_t out[MAX_FRAME];
      int n = frame_size_of(old_st, 0), k;
      make_packets(mode_ids[m], pkts, HIST_FRAMES + POST_FRAMES);
      apply_dec_cfg(old_st, &cfg);
      speex_bits_init(&bo);
      speex_bits_init(&bf);
      for (k = 0; k < HIST_FRAMES; ++k) {
         if (is_lost(k))
            TASK_CHECK(speex_decode_int(old_st, NULL, out) == 0);
         else {
            speex_bits_read_from(&bo, pkts[k].bytes, pkts[k].len);
            TASK_CHECK(speex_decode_int(old_st, &bo, out) == 0);
         }
      }
      TASK_CHECK(speex_decoder_ctl(old_st, SPEEX_RESET_STATE, NULL) == 0);
      {
         void *fresh_st = new_decoder(mode_ids[m]);
         apply_dec_cfg(fresh_st, &cfg);
         TASK_CHECK(check_dec_cfg_kept(old_st, fresh_st) == 0);
         for (k = 0; k < POST_FRAMES; ++k) {
            TASK_CHECK(compare_decoded(old_st, fresh_st, &pkts[HIST_FRAMES + k],
                                       &bo, &bf, n, is_lost(k), 0) == 0);
         }
         speex_decoder_destroy(fresh_st);
      }
      speex_bits_destroy(&bo);
      speex_bits_destroy(&bf);
      speex_decoder_destroy(old_st);
   }
   return 0;
}

/* Custom user in-band handler: counts invocations. */
static int user_handler_calls[2];
static int counting_user_handler(SpeexBits *bits, void *state, void *data)
{
   int *counter = (int *)data;
   int req_size = speex_bits_unpack_unsigned(bits, 4);
   speex_bits_advance(bits, 5 + 8 * req_size);
   (void)state;
   ++(*counter);
   return 0;
}

/* Build a packet that only carries in-band requests and ends with a
   terminator (both decoders therefore return -1, without touching PCM
   state).  which=0 selects an enhancer request, which=1 a user request. */
static void build_request_packet(struct packet *pkt, int which, int enh_value)
{
   SpeexBits bits;
   speex_bits_init(&bits);
   speex_bits_pack(&bits, 0, 1);                /* narrowband block marker */
   if (which == 0) {
      speex_bits_pack(&bits, 14, 4);            /* Speex in-band request */
      speex_bits_pack(&bits, SPEEX_INBAND_ENH_REQUEST, 4);
      speex_bits_pack(&bits, enh_value, 1);
   } else {
      speex_bits_pack(&bits, 13, 4);            /* user in-band request */
      speex_bits_pack(&bits, 0, 4);             /* req_size = 0 */
      speex_bits_pack(&bits, 0, 5);             /* 5 padding bits the handler skips */
   }
   speex_bits_pack(&bits, 0, 1);                /* next block marker */
   speex_bits_pack(&bits, 15, 4);               /* terminator */
   pkt->len = speex_bits_write(&bits, pkt->bytes, PACKET_CAP);
   speex_bits_destroy(&bits);
}

static int run_dec_inband(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct packet pkts[HIST_FRAMES + POST_FRAMES];
      struct packet req_enh, req_user;
      struct dec_cfg cfg = {1, 1,
                            mode_ids[m] == 0 ? 8000 :
                            (mode_ids[m] == 1 ? 16000 : 32000), 1};
      void *old_st = new_decoder(mode_ids[m]);
      SpeexBits bo, bf;
      SpeexCallback enh_cb, user_cb;
      spx_int16_t out[MAX_FRAME];
      int n = frame_size_of(old_st, 0), k, ret_o, ret_f, enh;
      int old_calls = 0, fresh_calls = 0;
      make_packets(mode_ids[m], pkts, HIST_FRAMES + POST_FRAMES);
      build_request_packet(&req_enh, 0, 0);
      build_request_packet(&req_user, 1, 0);
      apply_dec_cfg(old_st, &cfg);
      enh_cb.callback_id = SPEEX_INBAND_ENH_REQUEST;
      enh_cb.func = speex_std_enh_request_handler;
      enh_cb.data = old_st;
      enh_cb.reserved1 = 0;
      enh_cb.reserved2 = 0;
      user_cb.callback_id = 0;
      user_cb.func = counting_user_handler;
      user_cb.data = &user_handler_calls[0];
      user_cb.reserved1 = 0;
      user_cb.reserved2 = 0;
      TASK_CHECK(speex_decoder_ctl(old_st, SPEEX_SET_HANDLER, &enh_cb) == 0);
      TASK_CHECK(speex_decoder_ctl(old_st, SPEEX_SET_USER_HANDLER, &user_cb) == 0);
      speex_bits_init(&bo);
      speex_bits_init(&bf);
      /* Exercise both handlers before the reset. */
      speex_bits_read_from(&bo, req_enh.bytes, req_enh.len);
      (void)speex_decode_int(old_st, &bo, out);
      speex_bits_read_from(&bo, req_user.bytes, req_user.len);
      (void)speex_decode_int(old_st, &bo, out);
      for (k = 0; k < HIST_FRAMES; ++k) {
         speex_bits_read_from(&bo, pkts[k].bytes, pkts[k].len);
         TASK_CHECK(speex_decode_int(old_st, &bo, out) == 0);
      }
      TASK_CHECK(user_handler_calls[0] > 0);
      TASK_CHECK(speex_decoder_ctl(old_st, SPEEX_RESET_STATE, NULL) == 0);
      {
         void *fresh_st = new_decoder(mode_ids[m]);
         /* The stream-driven enhancer change is part of the effective
            configuration, so the reference starts with the same value. */
         apply_dec_cfg(fresh_st, &cfg);
         enh = 0;
         speex_decoder_ctl(fresh_st, SPEEX_SET_ENH, &enh);
         speex_decoder_ctl(old_st, SPEEX_GET_ENH, &enh);
         TASK_CHECK(enh == 0);
         enh_cb.data = fresh_st;
         user_cb.data = &user_handler_calls[1];
         TASK_CHECK(speex_decoder_ctl(fresh_st, SPEEX_SET_HANDLER, &enh_cb) == 0);
         TASK_CHECK(speex_decoder_ctl(fresh_st, SPEEX_SET_USER_HANDLER, &user_cb) == 0);
         TASK_CHECK(check_dec_cfg_kept(old_st, fresh_st) == 0);

         old_calls = user_handler_calls[0];
         fresh_calls = user_handler_calls[1];
         speex_bits_read_from(&bo, req_user.bytes, req_user.len);
         speex_bits_read_from(&bf, req_user.bytes, req_user.len);
         ret_o = speex_decode_int(old_st, &bo, out);
         ret_f = speex_decode_int(fresh_st, &bf, out);
         TASK_CHECK(ret_o == ret_f && ret_o == -1);
         TASK_CHECK(user_handler_calls[0] - old_calls == 1);
         TASK_CHECK(user_handler_calls[1] - fresh_calls == 1);

         speex_bits_read_from(&bo, req_enh.bytes, req_enh.len);
         speex_bits_read_from(&bf, req_enh.bytes, req_enh.len);
         ret_o = speex_decode_int(old_st, &bo, out);
         ret_f = speex_decode_int(fresh_st, &bf, out);
         TASK_CHECK(ret_o == ret_f && ret_o == -1);
         speex_decoder_ctl(old_st, SPEEX_GET_ENH, &enh);
         TASK_CHECK(enh == 0);
         speex_decoder_ctl(fresh_st, SPEEX_GET_ENH, &enh);
         TASK_CHECK(enh == 0);

         for (k = 0; k < POST_FRAMES; ++k) {
            TASK_CHECK(compare_decoded(old_st, fresh_st, &pkts[HIST_FRAMES + k],
                                       &bo, &bf, n, is_lost(k), 0) == 0);
         }
         speex_decoder_destroy(fresh_st);
      }
      speex_bits_destroy(&bo);
      speex_bits_destroy(&bf);
      speex_decoder_destroy(old_st);
   }
   return 0;
}

static int run_dec_fresh_and_continuous(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct packet pkts[HIST_FRAMES + POST_FRAMES];
      struct dec_cfg cfg = {1, 0,
                            mode_ids[m] == 0 ? 8000 :
                            (mode_ids[m] == 1 ? 16000 : 32000), 1};
      void *a = new_decoder(mode_ids[m]);
      void *b = new_decoder(mode_ids[m]);
      SpeexBits ba, bb;
      int n = frame_size_of(a, 0), k;
      make_packets(mode_ids[m], pkts, HIST_FRAMES + POST_FRAMES);
      /* Immediate, repeated reset on an untouched handle. */
      TASK_CHECK(speex_decoder_ctl(a, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(speex_decoder_ctl(a, SPEEX_RESET_STATE, NULL) == 0);
      apply_dec_cfg(a, &cfg);
      apply_dec_cfg(b, &cfg);
      /* Reset after configuration but before any frame: PCM must be
         unchanged relative to the untouched, identically configured handle. */
      TASK_CHECK(speex_decoder_ctl(a, SPEEX_RESET_STATE, NULL) == 0);
      speex_bits_init(&ba);
      speex_bits_init(&bb);
      for (k = 0; k < POST_FRAMES; ++k) {
         /* Continuous path without any reset: PCM stays identical. */
         TASK_CHECK(compare_decoded(a, b, &pkts[k], &ba, &bb, n, 0, 0) == 0);
      }
      speex_bits_destroy(&ba);
      speex_bits_destroy(&bb);
      speex_decoder_destroy(a);
      speex_decoder_destroy(b);
   }
   return 0;
}

static int run_dec_interleaved_double_reset(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct packet pkts[HIST_FRAMES + POST_FRAMES];
      struct dec_cfg cfg_a = {1, 1,
                              mode_ids[m] == 0 ? 8000 :
                              (mode_ids[m] == 1 ? 16000 : 32000), 1};
      struct dec_cfg cfg_b = {0, 0,
                              mode_ids[m] == 0 ? 8000 :
                              (mode_ids[m] == 1 ? 16000 : 32000), 1};
      void *old_a = new_decoder(mode_ids[m]), *old_b = new_decoder(mode_ids[m]);
      SpeexBits ba, bb, bfa, bfb;
      spx_int16_t out[MAX_FRAME];
      int n = frame_size_of(old_a, 0), k;
      void *fresh_a, *fresh_b;
      make_packets(mode_ids[m], pkts, HIST_FRAMES + POST_FRAMES);
      apply_dec_cfg(old_a, &cfg_a);
      apply_dec_cfg(old_b, &cfg_b);
      speex_bits_init(&ba);
      speex_bits_init(&bb);
      speex_bits_init(&bfa);
      speex_bits_init(&bfb);
      for (k = 0; k < HIST_FRAMES; ++k) {
         if (is_lost(k)) {
            TASK_CHECK(speex_decode_int(old_a, NULL, out) == 0);
            TASK_CHECK(speex_decode_int(old_b, NULL, out) == 0);
         } else {
            speex_bits_read_from(&ba, pkts[k].bytes, pkts[k].len);
            TASK_CHECK(speex_decode_int(old_a, &ba, out) == 0);
            speex_bits_read_from(&bb, pkts[k].bytes, pkts[k].len);
            TASK_CHECK(speex_decode_int(old_b, &bb, out) == 0);
         }
      }
      TASK_CHECK(speex_decoder_ctl(old_a, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(speex_decoder_ctl(old_b, SPEEX_RESET_STATE, NULL) == 0);
      for (k = 0; k < 4; ++k) {
         speex_bits_read_from(&ba, pkts[k].bytes, pkts[k].len);
         TASK_CHECK(speex_decode_int(old_a, &ba, out) == 0);
         TASK_CHECK(speex_decode_int(old_b, NULL, out) == 0);
      }
      TASK_CHECK(speex_decoder_ctl(old_a, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(speex_decoder_ctl(old_b, SPEEX_RESET_STATE, NULL) == 0);
      fresh_a = new_decoder(mode_ids[m]);
      fresh_b = new_decoder(mode_ids[m]);
      apply_dec_cfg(fresh_a, &cfg_a);
      apply_dec_cfg(fresh_b, &cfg_b);
      TASK_CHECK(check_dec_cfg_kept(old_a, fresh_a) == 0);
      TASK_CHECK(check_dec_cfg_kept(old_b, fresh_b) == 0);
      for (k = 0; k < POST_FRAMES; ++k) {
         TASK_CHECK(compare_decoded(old_a, fresh_a, &pkts[HIST_FRAMES + k],
                                    &ba, &bfa, n, is_lost(k), 0) == 0);
         TASK_CHECK(compare_decoded(old_b, fresh_b, &pkts[HIST_FRAMES + k],
                                    &bb, &bfb, n, 0, 0) == 0);
      }
      speex_decoder_destroy(fresh_a);
      speex_decoder_destroy(fresh_b);
      speex_bits_destroy(&ba);
      speex_bits_destroy(&bb);
      speex_bits_destroy(&bfa);
      speex_bits_destroy(&bfb);
      speex_decoder_destroy(old_a);
      speex_decoder_destroy(old_b);
   }
   return 0;
}

/* ---------------------------- recursive group ---------------------------- */

static int run_rec_encoder(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      /* WB/UWB stack a sub-band encoder on top of a recursive low band;
         the reset must clear every level including the QMF memories. */
      struct enc_cfg cfg = {6, 3, 1, 6.0f, 1, 1, 0,
                            mode_ids[m] == 0 ? 8000 :
                            (mode_ids[m] == 1 ? 16000 : 32000), 25, 1};
      void *old_st = new_encoder(mode_ids[m]);
      SpeexBits bo, bf;
      spx_int16_t frame[MAX_FRAME];
      int n = frame_size_of(old_st, 1), k, saw_dtx = 0, ret;
      apply_enc_cfg(old_st, &cfg);
      speex_bits_init(&bo);
      speex_bits_init(&bf);
      for (k = 0; k < DTX_HIST_FRAMES; ++k) {
         fill_frame(frame, n, k, k < 6 ? 2 : 0);
         speex_bits_reset(&bo);
         ret = speex_encode_int(old_st, frame, &bo);
         if (ret == 0)
            saw_dtx = 1;
      }
      TASK_CHECK(saw_dtx);
      TASK_CHECK(speex_encoder_ctl(old_st, SPEEX_RESET_STATE, NULL) == 0);
      {
         void *fresh_st = new_encoder(mode_ids[m]);
         apply_enc_cfg(fresh_st, &cfg);
         TASK_CHECK(check_enc_cfg_kept(old_st, fresh_st) == 0);
         for (k = 0; k < POST_FRAMES; ++k) {
            fill_frame(frame, n, k + 700, post_kind(k));
            TASK_CHECK(compare_encoded(old_st, fresh_st, &bo, &bf, frame, n, 0) == 0);
            TASK_CHECK(compare_encoded(old_st, fresh_st, &bo, &bf, frame, n, 1) == 0);
         }
         speex_encoder_destroy(fresh_st);
      }
      speex_bits_destroy(&bo);
      speex_bits_destroy(&bf);
      speex_encoder_destroy(old_st);
   }
   return 0;
}

static int run_rec_decoder(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct packet pkts[HIST_FRAMES + POST_FRAMES];
      struct dec_cfg cfg = {0, 0,
                            mode_ids[m] == 0 ? 8000 :
                            (mode_ids[m] == 1 ? 16000 : 32000), 1};
      void *old_st = new_decoder(mode_ids[m]);
      SpeexBits bo, bf;
      spx_int16_t out[MAX_FRAME];
      int n = frame_size_of(old_st, 0), k, saw_loss = 0;
      make_packets(mode_ids[m], pkts, HIST_FRAMES + POST_FRAMES);
      apply_dec_cfg(old_st, &cfg);
      speex_bits_init(&bo);
      speex_bits_init(&bf);
      /* NULL bits drive PLC through every recursive band. */
      for (k = 0; k < HIST_FRAMES; ++k) {
         if (is_lost(k)) {
            TASK_CHECK(speex_decode_int(old_st, NULL, out) == 0);
            saw_loss = 1;
         } else {
            speex_bits_read_from(&bo, pkts[k].bytes, pkts[k].len);
            TASK_CHECK(speex_decode_int(old_st, &bo, out) == 0);
         }
      }
      TASK_CHECK(saw_loss);
      TASK_CHECK(speex_decoder_ctl(old_st, SPEEX_RESET_STATE, NULL) == 0);
      {
         void *fresh_st = new_decoder(mode_ids[m]);
         apply_dec_cfg(fresh_st, &cfg);
         TASK_CHECK(check_dec_cfg_kept(old_st, fresh_st) == 0);
         for (k = 0; k < POST_FRAMES; ++k) {
            TASK_CHECK(compare_decoded(old_st, fresh_st, &pkts[HIST_FRAMES + k],
                                       &bo, &bf, n, is_lost(k), 0) == 0);
            TASK_CHECK(compare_decoded(old_st, fresh_st, &pkts[HIST_FRAMES + k],
                                       &bo, &bf, n, is_lost(k), 1) == 0);
         }
         speex_decoder_destroy(fresh_st);
      }
      speex_bits_destroy(&bo);
      speex_bits_destroy(&bf);
      speex_decoder_destroy(old_st);
   }
   return 0;
}

static int run_rec_double_reset_interleave(void)
{
   int m;
   for (m = 0; m < 3; ++m) {
      struct packet pkts[HIST_FRAMES + POST_FRAMES];
      struct enc_cfg cfg_e = {7, 4, 1, 7.0f, 1, 0, 1,
                              mode_ids[m] == 0 ? 8000 :
                              (mode_ids[m] == 1 ? 16000 : 32000), 30, 1};
      struct dec_cfg cfg_d = {1, 1,
                              mode_ids[m] == 0 ? 8000 :
                              (mode_ids[m] == 1 ? 16000 : 32000), 1};
      void *old_enc = new_encoder(mode_ids[m]);
      void *old_dec = new_decoder(mode_ids[m]);
      SpeexBits be, bd, bfe, bfd;
      spx_int16_t frame[MAX_FRAME], out[MAX_FRAME];
      int n = frame_size_of(old_enc, 1), k;
      void *fresh_enc, *fresh_dec;
      make_packets(mode_ids[m], pkts, HIST_FRAMES + POST_FRAMES);
      apply_enc_cfg(old_enc, &cfg_e);
      apply_dec_cfg(old_dec, &cfg_d);
      speex_bits_init(&be);
      speex_bits_init(&bd);
      speex_bits_init(&bfe);
      speex_bits_init(&bfd);
      for (k = 0; k < HIST_FRAMES; ++k) {
         fill_frame(frame, n, k, k % 4 == 0 ? 0 : 2);
         speex_bits_reset(&be);
         speex_encode_int(old_enc, frame, &be);
         if (is_lost(k))
            TASK_CHECK(speex_decode_int(old_dec, NULL, out) == 0);
         else {
            speex_bits_read_from(&bd, pkts[k].bytes, pkts[k].len);
            TASK_CHECK(speex_decode_int(old_dec, &bd, out) == 0);
         }
      }
      TASK_CHECK(speex_encoder_ctl(old_enc, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(speex_decoder_ctl(old_dec, SPEEX_RESET_STATE, NULL) == 0);
      for (k = 0; k < 3; ++k) {
         fill_frame(frame, n, k, 1);
         speex_bits_reset(&be);
         speex_encode_int(old_enc, frame, &be);
         speex_bits_read_from(&bd, pkts[k].bytes, pkts[k].len);
         TASK_CHECK(speex_decode_int(old_dec, &bd, out) == 0);
      }
      /* Second reset on both handles in the same session. */
      TASK_CHECK(speex_encoder_ctl(old_enc, SPEEX_RESET_STATE, NULL) == 0);
      TASK_CHECK(speex_decoder_ctl(old_dec, SPEEX_RESET_STATE, NULL) == 0);
      fresh_enc = new_encoder(mode_ids[m]);
      fresh_dec = new_decoder(mode_ids[m]);
      apply_enc_cfg(fresh_enc, &cfg_e);
      apply_dec_cfg(fresh_dec, &cfg_d);
      TASK_CHECK(check_enc_cfg_kept(old_enc, fresh_enc) == 0);
      TASK_CHECK(check_dec_cfg_kept(old_dec, fresh_dec) == 0);
      for (k = 0; k < POST_FRAMES; ++k) {
         fill_frame(frame, n, k + 800, post_kind(k));
         TASK_CHECK(compare_encoded(old_enc, fresh_enc, &be, &bfe, frame, n, 0) == 0);
         TASK_CHECK(compare_decoded(old_dec, fresh_dec, &pkts[HIST_FRAMES + k],
                                    &bd, &bfd, n, is_lost(k), 0) == 0);
      }
      speex_encoder_destroy(fresh_enc);
      speex_decoder_destroy(fresh_dec);
      speex_bits_destroy(&be);
      speex_bits_destroy(&bd);
      speex_bits_destroy(&bfe);
      speex_bits_destroy(&bfd);
      speex_encoder_destroy(old_enc);
      speex_decoder_destroy(old_dec);
   }
   return 0;
}

const struct task_case task_cases[] = {
   {"encoder", "cbr_all_bandwidths", run_enc_cbr},
   {"encoder", "vbr_all_bandwidths", run_enc_vbr},
   {"encoder", "dtx_silence_recovery", run_enc_dtx},
   {"encoder", "controls_updated_then_kept", run_enc_control_change},
   {"encoder", "fresh_reset_and_continuous_path", run_enc_fresh_and_continuous},
   {"encoder", "interleaved_double_reset", run_enc_interleaved_double_reset},
   {"decoder", "normal_packets_all_bandwidths", run_dec_packets},
   {"decoder", "null_bits_loss_all_bandwidths", run_dec_lost},
   {"decoder", "nondefault_enh_highpass", run_dec_enh_highpass},
   {"decoder", "inband_message_callbacks", run_dec_inband},
   {"decoder", "fresh_reset_and_continuous_path", run_dec_fresh_and_continuous},
   {"decoder", "interleaved_double_reset", run_dec_interleaved_double_reset},
   {"recursive", "recursive_encoder_reset", run_rec_encoder},
   {"recursive", "recursive_decoder_reset_loss", run_rec_decoder},
   {"recursive", "double_reset_interleaved_bands", run_rec_double_reset_interleave},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
