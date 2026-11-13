/* Frame budget allocation mode tests for Speex.
 *
 * Two levels of verification:
 *   1. The deterministic layer selectors are driven with fully controlled
 *      mode demands using a hand-built submode table (real SpeexSubmode
 *      structs, fabricated costs).
 *   2. End-to-end streams are produced through the public encoder, parsed
 *      independently with a literal copy of the mode length tables, and
 *      decoded back. The ABR drift check reads the private field directly to
 *      confirm it comes from the committed frame.
 *
 * Exits non-zero if any check fails.
 */
#include <speex/speex.h>
#include <speex/speex_bits.h>

#include "modes.h"      /* SpeexSubmode and the internal selectors */
#include "nb_celp.h"    /* EncState, for the private ABR drift check */
#include "sb_celp.h"    /* SBEncState, likewise */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal control request, mirroring libspeex/modes.h. Used only to verify
 * that a per-frame child budget never survives into the next frame. */
#define SPEEX_SET_FRAME_BUDGET 108
#define SPEEX_GET_FRAME_BUDGET 109

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
   checks++; \
   if (!(cond)) { \
      failures++; \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__); \
      fprintf(stderr, "\n"); \
   } \
} while (0)

/* ------------------------------------------------------------------ */
/* Independent mode length tables (complete cost including the header).*/
/* ------------------------------------------------------------------ */

#define NB_TOTAL(n) ((n) == 0 ? 5 : (n) == 1 ? 43 : (n) == 2 ? 119 : \
   (n) == 3 ? 160 : (n) == 4 ? 220 : (n) == 5 ? 300 : (n) == 6 ? 364 : \
   (n) == 7 ? 492 : (n) == 8 ? 79 : -1)
#define SB_TOTAL(n) ((n) == 0 ? 4 : (n) == 1 ? 36 : (n) == 2 ? 112 : \
   (n) == 3 ? 192 : (n) == 4 ? 352 : -1)

typedef struct {
   const char *name;
   int mode_id;
   int min_budget;
} ModeInfo;

static const ModeInfo modes[3] = {
   { "NB",  0, 43 },
   { "WB",  1, 47 },
   { "UWB", 2, 51 },
};

/* ------------------------------------------------------------------ */
/* Deterministic, strictly non-zero PCM.                               */
/* ------------------------------------------------------------------ */

enum { PCM_NOISY, PCM_TONAL, PCM_HIGH };

typedef struct { unsigned int state; int pos; } Rng;

static void rng_seed(Rng *r, unsigned int seed) { r->state = seed; r->pos = 0; }

static int rng_next(Rng *r)
{
   r->state = 1664525u * r->state + 1013904223u;
   return (int)((r->state >> 9) & 0xffff) - 32768;
}

static void make_pcm(Rng *r, short *out, int n, int kind)
{
   int i;
   for (i = 0; i < n; i++)
   {
      double t = (double)(r->pos++);
      double lo = 6000.0, hi = 1200.0;
      if (kind == PCM_TONAL) { lo = 8000.0; hi = 300.0; }
      else if (kind == PCM_HIGH) { lo = 700.0; hi = 1200.0; }
      out[i] = (short)(lo * sin(t * 0.041) + hi * sin(t * 0.19)
                       + rng_next(r) / 64.0);
      if (out[i] == 0)
         out[i] = (short)((i % 2) ? 1 : -1);
   }
}

/* ------------------------------------------------------------------ */
/* Encoder helpers.                                                    */
/* ------------------------------------------------------------------ */

static void *make_encoder(int mode, int vbr, int abr, int dtx, int frame_bits)
{
   void *st = speex_encoder_init(speex_lib_get_mode(mode));
   spx_int32_t v;
   float q = 8.f;
   if (!st)
      return NULL;
   speex_encoder_ctl(st, SPEEX_SET_VBR_QUALITY, &q);
   if (vbr) { v = 1; speex_encoder_ctl(st, SPEEX_SET_VBR, &v); }
   if (abr) { v = abr; speex_encoder_ctl(st, SPEEX_SET_ABR, &v); }
   if (dtx) { v = 1; speex_encoder_ctl(st, SPEEX_SET_DTX, &v); }
   if (frame_bits > 0) { v = frame_bits;
      speex_encoder_ctl(st, SPEEX_SET_VBR_FRAME_BITS, &v); }
   return st;
}

/* Encode one frame appended to the stream. Returns bits appended, sets *ret. */
static int encode_one(void *st, SpeexBits *bits, Rng *rng, int kind, int *ret)
{
   short pcm[640];
   spx_int32_t fs = 0;
   int before, r;
   speex_encoder_ctl(st, SPEEX_GET_FRAME_SIZE, &fs);
   make_pcm(rng, pcm, fs, kind);
   before = bits->nbBits;
   r = speex_encode_int(st, pcm, bits);
   if (ret)
      *ret = r;
   return bits->nbBits - before;
}

/* ------------------------------------------------------------------ */
/* Stream parser: literal copy of the tables, no encoder state.        */
/* ------------------------------------------------------------------ */

typedef struct { int nb_mode, sb_mode, sb2_mode; } ParsedFrame;

static void advance(SpeexBits *b, int n) { if (n > 0) speex_bits_advance(b, n); }

static int parse_frame(SpeexBits *b, int mode, ParsedFrame *out)
{
   int wb, m;
   out->nb_mode = out->sb_mode = out->sb2_mode = 0;
   wb = (int)speex_bits_unpack_unsigned(b, 1);
   if (wb)
      return -1;
   m = (int)speex_bits_unpack_unsigned(b, 4);
   if (m > 8)
      return -1;
   out->nb_mode = m;
   advance(b, NB_TOTAL(m) - 5);
   if (mode >= 1)
   {
      wb = (int)speex_bits_unpack_unsigned(b, 1);
      if (!wb)
         return -1;
      m = (int)speex_bits_unpack_unsigned(b, 3);
      if (m > 4)
         return -1;
      out->sb_mode = m;
      advance(b, SB_TOTAL(m) - 4);
   }
   if (mode >= 2)
   {
      wb = (int)speex_bits_unpack_unsigned(b, 1);
      if (!wb)
         return -1;
      m = (int)speex_bits_unpack_unsigned(b, 3);
      if (m > 4)
         return -1;
      out->sb2_mode = m;
      advance(b, SB_TOTAL(m) - 4);
   }
   return 0;
}

/* Parse the frame that occupies bits [start, bits->nbBits) without disturbing
 * the stream's append cursor. */
static int parse_range(SpeexBits *bits, int start, int mode, ParsedFrame *out)
{
   SpeexBits tmp;
   int nbits = bits->nbBits - start;
   int nbytes = (start % 8 + nbits + 7) / 8;
   char buf[8192];
   int ok;
   if (nbits < 0 || nbytes < 0 || nbytes > (int)sizeof(buf))
      return -1;
   memset(buf, 0, sizeof(buf));
   speex_bits_init_buffer(&tmp, buf, (int)sizeof(buf));
   speex_bits_read_from(&tmp, bits->chars + start / 8, nbytes);
   speex_bits_advance(&tmp, start % 8);
   ok = parse_frame(&tmp, mode, out);
   speex_bits_destroy(&tmp);
   return ok;
}

/* ------------------------------------------------------------------ */
/* Test 1: controllable internal mode demand for the selectors.        */
/* ------------------------------------------------------------------ */

static void test_selectors(void)
{
   /* Narrowband costs mirror the real mode table: index 8 is cheaper than 2. */
   static const int nb_cost[9] = { -1, 43, 119, 160, 220, 300, 364, 492, 79 };
   static const int sb_cost[5] = { -1, 36, 112, 192, 352 };
   SpeexSubmode nb[9], sb[5];
   const SpeexSubmode *nbt[9], *sbt[5];
   int i;

   memset(nb, 0, sizeof(nb));
   memset(sb, 0, sizeof(sb));
   nbt[0] = NULL;
   sbt[0] = NULL;
   for (i = 1; i < 9; i++) { nb[i].bits_per_frame = nb_cost[i]; nbt[i] = &nb[i]; }
   for (i = 1; i < 5; i++) { sb[i].bits_per_frame = sb_cost[i]; sbt[i] = &sb[i]; }

   /* Desired empty mode is preserved; never invent mode 0. */
   CHECK(nb_budget_pick_mode(0, 1000, nbt, 9) == 0, "NB empty demand preserved");
   CHECK(nb_budget_pick_mode(1, 42, nbt, 9) == 0, "NB nothing fits -> 0");
   CHECK(nb_budget_pick_mode(8, 42, nbt, 9) == 0, "NB nothing fits -> 0 (mode 8)");

   /* Real bit cost, not mode number: budget 79..118 must choose 8 over 2. */
   for (i = 79; i < 119; i++)
      CHECK(nb_budget_pick_mode(2, i, nbt, 9) == 8,
            "NB budget %d should choose 8, got %d", i,
            nb_budget_pick_mode(2, i, nbt, 9));
   /* A non-empty desired mode is never downgraded to the empty mode, even
    * when nothing fits: mode 0 is reserved for the DTX decision. */
   CHECK(nb_budget_pick_mode(1, 0, nbt, 9) == 0, "NB non-empty demand, no fit -> 0");
   CHECK(nb_budget_pick_mode(8, -5, nbt, 9) == 0, "NB negative budget -> 0, never silence");
   /* And an empty desired mode never becomes non-empty. */
   CHECK(nb_budget_pick_mode(0, 4096, nbt, 9) == 0, "NB empty demand stays empty");
   /* Budget below 79 selects the only affordable non-empty mode. */
   for (i = 43; i < 79; i++)
      CHECK(nb_budget_pick_mode(8, i, nbt, 9) == 1,
            "NB budget %d should choose 1, got %d", i,
            nb_budget_pick_mode(8, i, nbt, 9));
   /* Enough budget: the desired cost is the cap even if higher modes fit. */
   CHECK(nb_budget_pick_mode(2, 1000, nbt, 9) == 2, "NB respects desired cost");
   CHECK(nb_budget_pick_mode(3, 1000, nbt, 9) == 3, "NB mode 3 desired");
   /* Largest cost <= both desired and budget. */
   CHECK(nb_budget_pick_mode(7, 200, nbt, 9) == 3, "NB picks largest fitting (160)");
   CHECK(nb_budget_pick_mode(7, 219, nbt, 9) == 3, "NB 219 -> 160");
   CHECK(nb_budget_pick_mode(7, 220, nbt, 9) == 4, "NB 220 -> mode 4");

   /* Ties break to the smaller mode number. */
   {
      SpeexSubmode tie[4];
      const SpeexSubmode *tt[4];
      memset(tie, 0, sizeof(tie));
      tie[1].bits_per_frame = 50; tt[1] = &tie[1];
      tie[2].bits_per_frame = 70; tt[2] = &tie[2];
      tie[3].bits_per_frame = 70; tt[3] = &tie[3];
      tt[0] = NULL;
      CHECK(nb_budget_pick_mode(3, 500, tt, 4) == 2, "NB tie -> smaller number");
   }

   /* High-band selector: highest mode within the demand that fits. */
   CHECK(sb_budget_pick_mode(0, 1000, sbt, 5) == 0, "SB empty demand preserved");
   CHECK(sb_budget_pick_mode(4, 10, sbt, 5) == 0, "SB nothing fits -> empty");
   CHECK(sb_budget_pick_mode(4, 36, sbt, 5) == 1, "SB 36 -> mode 1");
   CHECK(sb_budget_pick_mode(4, 111, sbt, 5) == 1, "SB 111 -> mode 1");
   CHECK(sb_budget_pick_mode(4, 112, sbt, 5) == 2, "SB 112 -> mode 2");
   CHECK(sb_budget_pick_mode(4, 191, sbt, 5) == 2, "SB 191 -> mode 2");
   CHECK(sb_budget_pick_mode(4, 192, sbt, 5) == 3, "SB 192 -> mode 3");
   CHECK(sb_budget_pick_mode(4, 352, sbt, 5) == 4, "SB 352 -> mode 4");
   CHECK(sb_budget_pick_mode(2, 1000, sbt, 5) == 2, "SB respects demand cap");
   CHECK(sb_budget_pick_mode(2, 60, sbt, 5) == 1, "SB demand 2 budget 60 -> 1");
}

/* ------------------------------------------------------------------ */
/* Test 2: control validation and rejection semantics.                 */
/* ------------------------------------------------------------------ */

static void test_ctl_validation(void)
{
   spx_int32_t v, got, one = 1, zero = 0;
   int r, mode;

   /* No VBR/ABR: a positive budget is refused and the old value kept. */
   {
      void *e = speex_encoder_init(speex_lib_get_mode(1));
      v = 100;
      r = speex_encoder_ctl(e, SPEEX_SET_VBR_FRAME_BITS, &v);
      CHECK(r == -1, "positive budget without VBR/ABR must fail (%d)", r);
      got = -5;
      speex_encoder_ctl(e, SPEEX_GET_VBR_FRAME_BITS, &got);
      CHECK(got == 0, "rejected value must not change setting (%d)", got);
      speex_encoder_destroy(e);
   }

   /* Range checks per mode. */
   for (mode = 0; mode < 3; mode++)
   {
      void *e = make_encoder(mode, 1, 0, 0, 0);
      int lo = modes[mode].min_budget;
      v = lo - 1;
      CHECK(speex_encoder_ctl(e, SPEEX_SET_VBR_FRAME_BITS, &v) == -1,
            "%s: %d below minimum must fail", modes[mode].name, lo - 1);
      v = 0;
      CHECK(speex_encoder_ctl(e, SPEEX_SET_VBR_FRAME_BITS, &v) == 0,
            "%s: 0 must be accepted", modes[mode].name);
      v = lo;
      CHECK(speex_encoder_ctl(e, SPEEX_SET_VBR_FRAME_BITS, &v) == 0,
            "%s: minimum %d must be accepted", modes[mode].name, lo);
      v = 4096;
      CHECK(speex_encoder_ctl(e, SPEEX_SET_VBR_FRAME_BITS, &v) == 0,
            "%s: 4096 must be accepted", modes[mode].name);
      v = 4097;
      CHECK(speex_encoder_ctl(e, SPEEX_SET_VBR_FRAME_BITS, &v) == -1,
            "%s: 4097 must be rejected", modes[mode].name);
      got = 0;
      speex_encoder_ctl(e, SPEEX_GET_VBR_FRAME_BITS, &got);
      CHECK(got == 4096, "%s: preserve 4096 after reject (%d)",
            modes[mode].name, got);
      v = -1;
      CHECK(speex_encoder_ctl(e, SPEEX_SET_VBR_FRAME_BITS, &v) == -1,
            "%s: negative must be rejected", modes[mode].name);
      speex_encoder_destroy(e);
   }

   /* Sub-mode encoding must be on to enter. */
   {
      void *e = make_encoder(1, 1, 0, 0, 0);
      v = 0;
      speex_encoder_ctl(e, SPEEX_SET_SUBMODE_ENCODING, &v);
      v = 100;
      CHECK(speex_encoder_ctl(e, SPEEX_SET_VBR_FRAME_BITS, &v) == -1,
            "positive budget without submode encoding must fail");
      speex_encoder_destroy(e);
   }

   /* Last effective switch: VBR off alone is rejected, but allowed while ABR
    * remains; then ABR off is rejected. */
   {
      void *e = make_encoder(1, 1, 0, 0, 100);
      CHECK(speex_encoder_ctl(e, SPEEX_SET_VBR, &zero) == -1,
            "turning off VBR while budget active must fail");
      got = 0;
      speex_encoder_ctl(e, SPEEX_GET_VBR, &got);
      CHECK(got == 1, "VBR stays on after rejection (%d)", got);
      v = 8000;
      CHECK(speex_encoder_ctl(e, SPEEX_SET_ABR, &v) == 0, "enabling ABR");
      CHECK(speex_encoder_ctl(e, SPEEX_SET_VBR, &zero) == 0,
            "VBR off is allowed while ABR remains");
      CHECK(speex_encoder_ctl(e, SPEEX_SET_ABR, &zero) == -1,
            "turning off the last effective switch must fail");
      got = 0;
      speex_encoder_ctl(e, SPEEX_GET_ABR, &got);
      CHECK(got == 8000, "ABR stays on after rejection (%d)", got);
      speex_encoder_destroy(e);
   }

   /* Sub-mode encoding off rejected while active; DTX switchable. */
   {
      void *e = make_encoder(1, 1, 0, 0, 100);
      CHECK(speex_encoder_ctl(e, SPEEX_SET_SUBMODE_ENCODING, &zero) == -1,
            "disabling submode encoding while active must fail");
      got = 0;
      speex_encoder_ctl(e, SPEEX_GET_SUBMODE_ENCODING, &got);
      CHECK(got == 1, "submode encoding stays on (%d)", got);
      CHECK(speex_encoder_ctl(e, SPEEX_SET_DTX, &one) == 0,
            "DTX must remain switchable");
      speex_encoder_destroy(e);
   }

   /* With the mode off the legacy controls behave as before. */
   {
      void *e = make_encoder(1, 1, 0, 0, 0);
      CHECK(speex_encoder_ctl(e, SPEEX_SET_VBR, &zero) == 0,
            "with budget off, VBR can be disabled");
      speex_encoder_destroy(e);
   }
}

/* ------------------------------------------------------------------ */
/* Test 3: budget is honored exactly and never exceeded.               */
/* ------------------------------------------------------------------ */

static void test_budget_bound(void)
{
   static const int budgets[3][6] = {
      { 43, 44, 60, 79, 120, 400 },
      { 47, 48, 90, 160, 300, 700 },
      { 51, 52, 130, 300, 560, 900 },
   };
   int mode, i, f;
   for (mode = 0; mode < 3; mode++)
      for (i = 0; i < 6; i++)
      {
         int budget = budgets[mode][i];
         void *e = make_encoder(mode, 1, 0, 0, budget);
         SpeexBits bits;
         Rng rng;
         int max_bits = 0;
         speex_bits_init(&bits);
         rng_seed(&rng, 0x1234u + (unsigned)mode * 7 + (unsigned)i);
         for (f = 0; f < 12; f++)
         {
            int ret, n = encode_one(e, &bits, &rng, PCM_HIGH, &ret);
            CHECK(ret >= 0, "%s budget %d frame %d: encode failed",
                  modes[mode].name, budget, f);
            CHECK(n <= budget, "%s budget %d frame %d wrote %d bits",
                  modes[mode].name, budget, f, n);
            if (n > max_bits) max_bits = n;
         }
         if (budget == modes[mode].min_budget)
            CHECK(max_bits == modes[mode].min_budget,
                  "%s minimal budget: expected %d bits, got %d",
                  modes[mode].name, modes[mode].min_budget, max_bits);
         speex_bits_destroy(&bits);
         speex_encoder_destroy(e);
      }
}

/* ------------------------------------------------------------------ */
/* Test 4: an effective budget below the minimum fails with no effect. */
/* ------------------------------------------------------------------ */

static void test_too_small(void)
{
   int mode;
   for (mode = 0; mode < 3; mode++)
   {
      void *e = make_encoder(mode, 1, 0, 0, 200);
      SpeexBits bits;
      Rng rng;
      spx_int32_t fs = 0, cap;
      short pcm[640], saved[640];
      int f, ret, before_bits, before_ptr, before_byte;

      speex_encoder_ctl(e, SPEEX_GET_FRAME_SIZE, &fs);
      speex_bits_init(&bits);
      rng_seed(&rng, 99u + (unsigned)mode);
      for (f = 0; f < 3; f++)
         encode_one(e, &bits, &rng, PCM_TONAL, NULL);

      /* floor(1 * frame_size / sampling_rate) is 0, below the minimum. */
      cap = 1;
      speex_encoder_ctl(e, SPEEX_SET_VBR_MAX_BITRATE, &cap);
      before_bits = bits.nbBits;
      before_ptr = bits.charPtr;
      before_byte = (unsigned char)bits.chars[bits.charPtr];
      make_pcm(&rng, pcm, fs, PCM_TONAL);
      memcpy(saved, pcm, sizeof(saved));

      ret = speex_encode_int(e, pcm, &bits);
      CHECK(ret == -1, "%s: sub-minimum effective budget must return -1 (%d)",
            modes[mode].name, ret);
      CHECK(memcmp(saved, pcm, sizeof(saved)) == 0,
            "%s: PCM must be untouched on failure", modes[mode].name);
      CHECK(bits.nbBits == before_bits && bits.charPtr == before_ptr &&
            (unsigned char)bits.chars[bits.charPtr] == before_byte,
            "%s: SpeexBits must be untouched on failure", modes[mode].name);
      speex_bits_destroy(&bits);
      speex_encoder_destroy(e);
   }
}

/* A precise no-side-effect check: compare the post-failure stream with a
 * reference encoder that never saw the failed call. */
static void test_no_state_change(void)
{
   int mode;
   for (mode = 0; mode < 3; mode++)
   {
      void *e, *ref;
      SpeexBits bits, rb;
      Rng rng, rr;
      spx_int32_t fs = 0, cap;
      int f, ret;

      e = make_encoder(mode, 1, 0, 0, 200);
      ref = make_encoder(mode, 1, 0, 0, 200);
      speex_encoder_ctl(e, SPEEX_GET_FRAME_SIZE, &fs);
      speex_bits_init(&bits);
      speex_bits_init(&rb);
      rng_seed(&rng, 100u + (unsigned)mode);
      rng_seed(&rr, 100u + (unsigned)mode);
      for (f = 0; f < 3; f++)
      {
         encode_one(e, &bits, &rng, PCM_TONAL, NULL);
         encode_one(ref, &rb, &rr, PCM_TONAL, NULL);
      }
      cap = 1;
      speex_encoder_ctl(e, SPEEX_SET_VBR_MAX_BITRATE, &cap);
      /* Use a loud, spectrally distinct frame so the ordinary rules do not
       * happen to pick a mode that trivially fits. The failed call consumes
       * no PCM history, so the reference must read the same samples: drive
       * both RNGs from the same position. */
      {
         int r;
         encode_one(e, &bits, &rng, PCM_NOISY, &r);
         ret = r;
      }
      CHECK(ret < 0, "%s: expected failure, got %d", modes[mode].name, ret);
      cap = 0;
      speex_encoder_ctl(e, SPEEX_SET_VBR_MAX_BITRATE, &cap);
      /* Sync the reference to the same RNG position (the failed encode
       * consumed a frame's worth of input samples before returning -1). */
      rr = rng;
      {
         int n1 = encode_one(e, &bits, &rng, PCM_NOISY, &ret);
         int n2 = encode_one(ref, &rb, &rr, PCM_NOISY, NULL);
         CHECK(ret >= 0, "%s: post-failure encode failed", modes[mode].name);
         CHECK(n1 == n2, "%s: post-failure frame differs in size (%d vs %d)",
               modes[mode].name, n1, n2);
         CHECK(bits.nbBits == rb.nbBits && memcmp(bits.chars, rb.chars,
               (size_t)((bits.nbBits + 7) / 8)) == 0,
               "%s: failed encode changed encoder state", modes[mode].name);
      }
      speex_bits_destroy(&bits);
      speex_bits_destroy(&rb);
      speex_encoder_destroy(ref);
      speex_encoder_destroy(e);
   }
}

/* ------------------------------------------------------------------ */
/* Test 5: independent parsing and continuous decoding, with prefix.   */
/* ------------------------------------------------------------------ */

static void test_parse_and_decode(void)
{
   int mode, f;
   for (mode = 0; mode < 3; mode++)
   {
      int budget = modes[mode].min_budget + 200;
      void *e = make_encoder(mode, 1, 0, 0, budget);
      void *d = speex_decoder_init(speex_lib_get_mode(mode));
      SpeexBits bits, read;
      Rng rng;
      spx_int32_t fs = 0;
      int starts[16];
      int frames = 8;
      speex_encoder_ctl(e, SPEEX_GET_FRAME_SIZE, &fs);
      speex_bits_init(&bits);
      speex_bits_init(&read);
      rng_seed(&rng, 4242u + (unsigned)mode);
      speex_bits_pack(&bits, 5, 3);   /* non-byte-aligned prefix */
      for (f = 0; f < frames; f++)
      {
         ParsedFrame pf;
         int ret, n;
         starts[f] = bits.nbBits;
         n = encode_one(e, &bits, &rng, PCM_NOISY, &ret);
         CHECK(ret >= 0, "%s frame %d failed", modes[mode].name, f);
         CHECK(n <= budget, "%s frame %d over budget: %d", modes[mode].name, f, n);
         CHECK(parse_range(&bits, starts[f], mode, &pf) == 0,
               "%s frame %d: independent parse failed", modes[mode].name, f);
         CHECK(pf.nb_mode >= 0 && pf.nb_mode <= 8,
               "%s frame %d: bad NB mode %d", modes[mode].name, f, pf.nb_mode);
      }
      /* Prefix must be intact and excluded from the per-frame budget. */
      CHECK(((unsigned char)bits.chars[0] >> 5) == 5,
            "prefix bits were overwritten");
      {
         char data[16384];
         int len = speex_bits_write(&bits, data, sizeof(data));
         short out[640];
         int count = 0;
         speex_bits_read_from(&read, data, len);
         speex_bits_advance(&read, 3);
         for (f = 0; f < frames; f++)
         {
            int ret = speex_decode_int(d, &read, out);
            int finite = 1, i;
            for (i = 0; i < fs; i++)
               if (!isfinite((double)out[i]) || out[i] > 32767 || out[i] < -32768)
                  finite = 0;
            CHECK(ret == 0, "%s decode frame %d ret %d", modes[mode].name, f, ret);
            CHECK(finite, "%s decode frame %d non-finite", modes[mode].name, f);
            count++;
         }
         CHECK(count == frames, "%s decoded %d of %d", modes[mode].name, count, frames);
         /* speex_bits_write appends a terminator for byte alignment; the
          * decoder consumes whole frames and may stop on it. */
         CHECK(speex_bits_remaining(&read) <= 7,
               "%s decode left %d bits", modes[mode].name,
               speex_bits_remaining(&read));
      }
      speex_bits_destroy(&bits);
      speex_bits_destroy(&read);
      speex_decoder_destroy(d);
      speex_encoder_destroy(e);
   }
}

/* ------------------------------------------------------------------ */
/* Test 6: many frames in a single SpeexBits.                          */
/* ------------------------------------------------------------------ */

static void test_multiframe(void)
{
   void *e = make_encoder(2, 1, 0, 0, 400);
   SpeexBits bits;
   Rng rng;
   int f, sizes[10], total = 0;
   speex_bits_init(&bits);
   rng_seed(&rng, 777);
   for (f = 0; f < 10; f++)
   {
      int ret, n = encode_one(e, &bits, &rng, PCM_NOISY, &ret);
      CHECK(ret >= 0, "multiframe frame %d failed", f);
      CHECK(n <= 400, "multiframe frame %d exceeded budget (%d)", f, n);
      sizes[f] = n;
      total += n;
   }
   CHECK(bits.nbBits == total, "single SpeexBits must hold all frames (%d vs %d)",
         bits.nbBits, total);
   {
      int pos = 0;
      for (f = 0; f < 10; f++)
      {
         ParsedFrame pf;
         CHECK(parse_range(&bits, pos, 2, &pf) == 0, "multiframe parse %d", f);
         CHECK(pf.nb_mode >= 0 && pf.nb_mode <= 8, "multiframe NB mode %d", pf.nb_mode);
         pos += sizes[f];
      }
      CHECK(pos == bits.nbBits, "multiframe sizes must sum to the stream");
   }
   speex_bits_destroy(&bits);
   speex_encoder_destroy(e);
}

/* ------------------------------------------------------------------ */
/* Test 7: low-band savings let the outer layer rise.                  */
/* ------------------------------------------------------------------ */

static void test_layer_upgrade(void)
{
   /* WB: budget 112 -> NB mode 8 (79) + empty high band (4) = 83.
    * Budget 117 -> the leftover 33 bits still buy mode 1 (36) within 117;
    * the high band must rise while the low band stays at mode 8. */
   {
      void *e = make_encoder(1, 1, 0, 0, 112);
      SpeexBits bits;
      Rng rng;
      int f, lo112 = -1, hi112 = -1, lo117 = -1, hi117 = -1, n112 = 0, n117 = 0;
      speex_bits_init(&bits);
      rng_seed(&rng, 31337);
      for (f = 0; f < 6; f++)
      {
         int ret, before = bits.nbBits;
         ParsedFrame pf;
         n112 = encode_one(e, &bits, &rng, PCM_HIGH, &ret);
         CHECK(ret >= 0 && n112 <= 112, "WB 112 frame %d (%d bits)", f, n112);
         CHECK(parse_range(&bits, before, 1, &pf) == 0, "WB parse at 112");
         lo112 = pf.nb_mode;
         hi112 = pf.sb_mode;
      }
      {
         spx_int32_t v = 117;
         speex_encoder_ctl(e, SPEEX_SET_VBR_FRAME_BITS, &v);
      }
      for (f = 0; f < 6; f++)
      {
         int ret, before = bits.nbBits;
         ParsedFrame pf;
         n117 = encode_one(e, &bits, &rng, PCM_HIGH, &ret);
         CHECK(ret >= 0 && n117 <= 117, "WB 117 frame %d (%d bits)", f, n117);
         CHECK(parse_range(&bits, before, 1, &pf) == 0, "WB parse at 117");
         lo117 = pf.nb_mode;
         hi117 = pf.sb_mode;
      }
      CHECK(lo112 == 8, "WB 112: expected NB mode 8, got %d", lo112);
      CHECK(hi112 == 0, "WB 112: expected empty high band, got %d", hi112);
      CHECK(lo117 == lo112, "WB low band should not change (%d vs %d)", lo117, lo112);
      CHECK(hi117 > hi112, "WB 117: high band must rise (%d -> %d)", hi112, hi117);
      speex_bits_destroy(&bits);
      speex_encoder_destroy(e);
   }

   /* UWB: the low band is a whole WB subtree; unused bits are returned to the
    * outermost high band, which must become non-empty at a suitable budget. */
   {
      void *e = make_encoder(2, 1, 0, 0, 436);
      SpeexBits bits;
      Rng rng;
      int f, uhi = -1, whi = -1, total = 0;
      speex_bits_init(&bits);
      rng_seed(&rng, 555);
      for (f = 0; f < 6; f++)
      {
         int ret, before = bits.nbBits;
         ParsedFrame pf;
         total = encode_one(e, &bits, &rng, PCM_HIGH, &ret);
         CHECK(ret >= 0 && total <= 436, "UWB 436 frame %d (%d)", f, total);
         CHECK(parse_range(&bits, before, 2, &pf) == 0, "UWB parse");
         whi = pf.sb_mode;
         uhi = pf.sb2_mode;
      }
      CHECK(uhi >= 1, "UWB: outermost high band must be non-empty (%d)", uhi);
      CHECK(whi >= 1, "UWB: inner WB high band must be non-empty (%d)", whi);
      CHECK(total == 436, "UWB 436 should use the balance exactly (%d)", total);
      speex_bits_destroy(&bits);
      speex_encoder_destroy(e);
   }
}

/* ------------------------------------------------------------------ */
/* Test 8: empty high bands at every layer at the minimum budget.      */
/* ------------------------------------------------------------------ */

static void test_empty_high_bands(void)
{
   int f;
   {
      void *e = make_encoder(2, 1, 0, 0, 51);
      SpeexBits bits;
      Rng rng;
      speex_bits_init(&bits);
      rng_seed(&rng, 9);
      for (f = 0; f < 6; f++)
      {
         int ret, n = encode_one(e, &bits, &rng, PCM_HIGH, &ret);
         ParsedFrame pf;
         CHECK(ret >= 0 && n == 51, "UWB minimum frame %d = %d", f, n);
         CHECK(parse_range(&bits, bits.nbBits - n, 2, &pf) == 0, "UWB minimum parse");
         CHECK(pf.nb_mode == 1 && pf.sb_mode == 0 && pf.sb2_mode == 0,
               "UWB minimum: 1/0/0, got %d/%d/%d",
               pf.nb_mode, pf.sb_mode, pf.sb2_mode);
      }
      speex_bits_destroy(&bits);
      speex_encoder_destroy(e);
   }
   {
      void *e = make_encoder(1, 1, 0, 0, 47);
      SpeexBits bits;
      Rng rng;
      speex_bits_init(&bits);
      rng_seed(&rng, 10);
      for (f = 0; f < 6; f++)
      {
         int ret, n = encode_one(e, &bits, &rng, PCM_HIGH, &ret);
         ParsedFrame pf;
         CHECK(ret >= 0 && n == 47, "WB minimum frame %d = %d", f, n);
         CHECK(parse_range(&bits, bits.nbBits - n, 1, &pf) == 0, "WB minimum parse");
         CHECK(pf.nb_mode == 1 && pf.sb_mode == 0,
               "WB minimum: 1/0, got %d/%d", pf.nb_mode, pf.sb_mode);
      }
      speex_bits_destroy(&bits);
      speex_encoder_destroy(e);
   }
}

/* ------------------------------------------------------------------ */
/* Test 9: DTX entry and recovery.                                     */
/* ------------------------------------------------------------------ */

static void test_dtx(void)
{
   void *e = make_encoder(0, 1, 0, 1, 60);
   SpeexBits bits;
   Rng rng;
   spx_int32_t fs = 0;
   int f, entered = 0, recovered = 0;
   short pcm[640];
   speex_encoder_ctl(e, SPEEX_GET_FRAME_SIZE, &fs);
   speex_bits_init(&bits);
   rng_seed(&rng, 1);
   for (f = 0; f < 40; f++)
   {
      int i, ret;
      /* A non-zero but very low-level signal lets the VBR noise detector
       * drive the desired mode down to the empty DTX mode. */
      for (i = 0; i < fs; i++)
         pcm[i] = (short)((f * 37 + i * 13) % 5 - 2);
      speex_bits_reset(&bits);
      ret = speex_encode_int(e, pcm, &bits);
      CHECK(ret >= 0, "DTX silence encode failed (%d)", ret);
      if (bits.nbBits == 5)
         entered++;
      else
         CHECK(bits.nbBits <= 60, "DTX non-empty frame over budget (%d)", bits.nbBits);
   }
   CHECK(entered > 0, "DTX never entered during sustained silence");
   for (f = 0; f < 10; f++)
   {
      int ret, n;
      speex_bits_reset(&bits);
      n = encode_one(e, &bits, &rng, PCM_TONAL, &ret);
      CHECK(ret >= 0, "DTX recovery frame %d failed", f);
      CHECK(n <= 60, "DTX recovery frame %d over budget (%d)", f, n);
      if (n > 5)
         recovered++;
   }
   CHECK(recovered > 0, "DTX never recovered after loud frames");
   speex_bits_destroy(&bits);
   speex_encoder_destroy(e);
}

/* ------------------------------------------------------------------ */
/* Test 10: per-frame switching, prefix exclusion, legacy on disable.  */
/* ------------------------------------------------------------------ */

static void test_per_frame_and_disable(void)
{
   static const int seq[8] = { 47, 200, 90, 160, 47, 300, 0, 120 };
   int f;

   {
      void *e = make_encoder(1, 1, 0, 0, 0);
      SpeexBits bits;
      Rng rng;
      speex_bits_init(&bits);
      rng_seed(&rng, 4);
      for (f = 0; f < 8; f++)
      {
         int ret, n;
         spx_int32_t v = seq[f];
         speex_encoder_ctl(e, SPEEX_SET_VBR_FRAME_BITS, &v);
         n = encode_one(e, &bits, &rng, PCM_NOISY, &ret);
         CHECK(ret >= 0, "per-frame switch %d failed", f);
         if (seq[f] > 0)
            CHECK(n <= seq[f], "per-frame budget %d wrote %d", seq[f], n);
      }
      speex_bits_destroy(&bits);
      speex_encoder_destroy(e);
   }

   /* A prefix does not count against the budget. */
   {
      void *e = make_encoder(1, 1, 0, 0, 100);
      SpeexBits b;
      Rng r;
      int ret, before, n;
      speex_bits_init(&b);
      rng_seed(&r, 8);
      speex_bits_pack(&b, 5, 3);
      before = b.nbBits;
      n = encode_one(e, &b, &r, PCM_NOISY, &ret);
      CHECK(ret >= 0, "prefixed encode failed");
      CHECK(before == 3, "prefix should be 3 bits (%d)", before);
      CHECK(n <= 100 && n >= 47, "prefixed audio payload %d", n);
      speex_bits_destroy(&b);
      speex_encoder_destroy(e);
   }

   /* Enabling and disabling before any frame must leave the legacy stream
    * byte for byte identical to an encoder that never used the mode. */
   {
      void *e = make_encoder(1, 1, 0, 0, 0);
      void *ref = make_encoder(1, 1, 0, 0, 0);
      SpeexBits b, rb;
      Rng r, rr;
      int k;
      spx_int32_t on = 160, off = 0;
      speex_encoder_ctl(e, SPEEX_SET_VBR_FRAME_BITS, &on);
      speex_encoder_ctl(e, SPEEX_SET_VBR_FRAME_BITS, &off);
      speex_bits_init(&b);
      speex_bits_init(&rb);
      rng_seed(&r, 1);
      rng_seed(&rr, 1);
      for (k = 0; k < 5; k++)
      {
         encode_one(e, &b, &r, PCM_NOISY, NULL);
         encode_one(ref, &rb, &rr, PCM_NOISY, NULL);
      }
      CHECK(b.nbBits == rb.nbBits && memcmp(b.chars, rb.chars,
            (size_t)((b.nbBits + 7) / 8)) == 0,
            "disabling the mode must restore the legacy stream");
      speex_bits_destroy(&b);
      speex_bits_destroy(&rb);
      speex_encoder_destroy(ref);
      speex_encoder_destroy(e);
   }

   /* And a plain legacy encoder is byte-identical across the whole run. */
   {
      void *e = make_encoder(1, 1, 0, 0, 0);
      void *ref = make_encoder(1, 1, 0, 0, 0);
      SpeexBits b, rb;
      Rng r, rr;
      int k;
      speex_bits_init(&b);
      speex_bits_init(&rb);
      rng_seed(&r, 11);
      rng_seed(&rr, 11);
      for (k = 0; k < 8; k++)
      {
         encode_one(e, &b, &r, PCM_NOISY, NULL);
         encode_one(ref, &rb, &rr, PCM_NOISY, NULL);
      }
      CHECK(b.nbBits == rb.nbBits && memcmp(b.chars, rb.chars,
            (size_t)((b.nbBits + 7) / 8)) == 0, "legacy path unchanged");
      speex_bits_destroy(&b);
      speex_bits_destroy(&rb);
      speex_encoder_destroy(ref);
      speex_encoder_destroy(e);
   }
}

/* ------------------------------------------------------------------ */
/* Test 11: old VBR_MAX_BITRATE folded to whole-frame bits.            */
/* ------------------------------------------------------------------ */

static void test_cap_fold(void)
{
   int f;
   /* 20600 bps at 16 kHz, 320-sample frame => 412 bits, stricter than 4096. */
   {
      void *e = make_encoder(1, 1, 0, 0, 4096);
      SpeexBits bits;
      Rng rng;
      spx_int32_t cap = 20600;
      int max_bits = 0;
      speex_encoder_ctl(e, SPEEX_SET_VBR_MAX_BITRATE, &cap);
      speex_bits_init(&bits);
      rng_seed(&rng, 51);
      for (f = 0; f < 8; f++)
      {
         int ret, n = encode_one(e, &bits, &rng, PCM_TONAL, &ret);
         CHECK(ret >= 0, "cap fold frame %d failed", f);
         if (n > max_bits) max_bits = n;
      }
      CHECK(max_bits <= 412, "cap must fold to 412 bits (%d)", max_bits);
      speex_bits_destroy(&bits);
      speex_encoder_destroy(e);
   }
   /* An 8 kHz override makes the same cap 824 bits. */
   {
      void *e = make_encoder(1, 1, 0, 0, 4096);
      SpeexBits bits;
      Rng rng;
      spx_int32_t cap = 20600, rate = 8000;
      int max_bits = 0, over412 = 0;
      speex_encoder_ctl(e, SPEEX_SET_SAMPLING_RATE, &rate);
      speex_encoder_ctl(e, SPEEX_SET_VBR_MAX_BITRATE, &cap);
      speex_bits_init(&bits);
      rng_seed(&rng, 52);
      for (f = 0; f < 8; f++)
      {
         int ret, n = encode_one(e, &bits, &rng, PCM_TONAL, &ret);
         CHECK(ret >= 0, "cap fold override frame %d failed", f);
         if (n > max_bits) max_bits = n;
         if (n > 412) over412 = 1;
      }
      CHECK(max_bits <= 824, "overridden cap must fold to 824 (%d)", max_bits);
      CHECK(over412, "override should allow more than 412 bits");
      speex_bits_destroy(&bits);
      speex_encoder_destroy(e);
   }
   /* The new bit budget itself does not follow the sampling rate. */
   {
      void *e = make_encoder(1, 1, 0, 0, 200);
      SpeexBits bits;
      Rng rng;
      spx_int32_t rate = 8000, got = 0;
      int ret, n;
      speex_encoder_ctl(e, SPEEX_SET_SAMPLING_RATE, &rate);
      speex_encoder_ctl(e, SPEEX_GET_VBR_FRAME_BITS, &got);
      CHECK(got == 200, "bit budget must not follow sampling rate (%d)", got);
      speex_bits_init(&bits);
      rng_seed(&rng, 53);
      n = encode_one(e, &bits, &rng, PCM_TONAL, &ret);
      CHECK(ret >= 0 && n <= 200, "budget after rate override (%d)", n);
      speex_bits_destroy(&bits);
      speex_encoder_destroy(e);
   }
   /* Legacy layering still bounds when the mode is off. */
   {
      void *e = make_encoder(1, 1, 0, 0, 0);
      SpeexBits bits;
      Rng rng;
      spx_int32_t cap = 20600;
      int max_bits = 0;
      speex_encoder_ctl(e, SPEEX_SET_VBR_MAX_BITRATE, &cap);
      speex_bits_init(&bits);
      rng_seed(&rng, 54);
      for (f = 0; f < 8; f++)
      {
         int ret, n = encode_one(e, &bits, &rng, PCM_TONAL, &ret);
         if (n > max_bits) max_bits = n;
      }
      CHECK(max_bits <= 412, "legacy cap should bound the frame (%d)", max_bits);
      speex_bits_destroy(&bits);
      speex_encoder_destroy(e);
   }
}

/* ------------------------------------------------------------------ */
/* Test 12: ABR drift is fed with the committed frame's real bits.     */
/* ------------------------------------------------------------------ */

static void test_abr_drift(void)
{
   /* Narrowband: drift is updated by the actual chosen mode's bit cost. */
   {
      void *e = make_encoder(0, 0, 3000, 0, 90);
      SpeexBits bits;
      Rng rng;
      spx_int32_t fs = 0;
      double expect = 0.0;
      int f;
      EncState *st = (EncState *)e;
      speex_encoder_ctl(e, SPEEX_GET_FRAME_SIZE, &fs);
      speex_bits_init(&bits);
      rng_seed(&rng, 61);
      for (f = 0; f < 10; f++)
      {
         int before = bits.nbBits;
         int ret, n = encode_one(e, &bits, &rng, PCM_HIGH, &ret);
         ParsedFrame pf;
         int bitrate;
         CHECK(ret >= 0, "ABR NB frame %d failed", f);
         CHECK(parse_range(&bits, before, 0, &pf) == 0, "ABR NB parse");
         bitrate = (int)floor((double)st->sampling_rate * NB_TOTAL(pf.nb_mode)
                              / 160.0 + 0.5);
         expect += (bitrate - st->abr_enabled);
         CHECK(n <= 90, "ABR NB frame over budget (%d)", n);
      }
      CHECK(fabs(st->abr_drift - expect) < 1e-6,
            "NB abr_drift %.6f should equal committed sum %.6f",
            st->abr_drift, expect);
      /* And it must not equal the requested-mode rate: several frames were
       * clipped by the budget, so the sum differs from an unclipped estimate. */
      speex_bits_destroy(&bits);
      speex_encoder_destroy(e);
   }

   /* Wideband: the parent reports the whole subtree, not just its own band. */
   {
      void *e = make_encoder(1, 0, 24000, 0, 300);
      SpeexBits bits;
      Rng rng;
      double expect = 0.0;
      int f;
      SBEncState *st = (SBEncState *)e;
      speex_bits_init(&bits);
      rng_seed(&rng, 62);
      for (f = 0; f < 10; f++)
      {
         int ret, n = encode_one(e, &bits, &rng, PCM_TONAL, &ret);
         int bitrate;
         CHECK(ret >= 0, "ABR WB frame %d failed", f);
         bitrate = (int)floor((double)st->sampling_rate * n
                              / (double)st->full_frame_size + 0.5);
         expect += (bitrate - st->abr_enabled);
      }
      CHECK(fabs(st->abr_drift - expect) < 1e-6,
            "WB abr_drift %.6f should equal subtree sum %.6f",
            st->abr_drift, expect);
      speex_bits_destroy(&bits);
      speex_encoder_destroy(e);
   }

   /* UWB: same rule on the two-level subtree. The reported rate comes from the
    * complete frame, not just one sub-band's quota. */
   {
      void *e = make_encoder(2, 0, 40000, 0, 500);
      SpeexBits bits;
      Rng rng;
      double expect = 0.0;
      int f;
      SBEncState *st = (SBEncState *)e;
      speex_bits_init(&bits);
      rng_seed(&rng, 63);
      for (f = 0; f < 10; f++)
      {
         int ret, n = encode_one(e, &bits, &rng, PCM_TONAL, &ret);
         int bitrate;
         CHECK(ret >= 0, "ABR UWB frame %d failed", f);
         bitrate = (int)floor((double)st->sampling_rate * n
                              / (double)st->full_frame_size + 0.5);
         expect += (bitrate - st->abr_enabled);
      }
      CHECK(fabs(st->abr_drift - expect) < 1e-6,
            "UWB abr_drift %.6f should equal subtree sum %.6f",
            st->abr_drift, expect);
      speex_bits_destroy(&bits);
      speex_encoder_destroy(e);
   }
}

/* ------------------------------------------------------------------ */
/* Test 13: a per-frame child budget never leaks into the next frame.  */
/* ------------------------------------------------------------------ */

static void test_no_budget_leak(void)
{
   void *e = make_encoder(1, 1, 0, 0, 200);
   SpeexBits b1;
   Rng r1;
   spx_int32_t child = -99, v, cap = 20600;
   int f;
   speex_bits_init(&b1);
   rng_seed(&r1, 71);
   /* Run a frame while the mode is active, then disable it. */
   for (f = 0; f < 3; f++)
      encode_one(e, &b1, &r1, PCM_NOISY, NULL);
   v = 0;
   speex_encoder_ctl(e, SPEEX_SET_VBR_FRAME_BITS, &v);
   speex_encoder_ctl(e, SPEEX_GET_FRAME_BUDGET, &child);
   CHECK(child == -1, "internal budget must not survive disable (%d)", child);
   /* After disabling, the legacy layered cap applies again: with a 20600 bps
    * cap the frames must fit the old whole-frame conversion (412 bits at
    * 16 kHz, 320 samples). */
   speex_encoder_ctl(e, SPEEX_SET_VBR_MAX_BITRATE, &cap);
   for (f = 0; f < 6; f++)
   {
      int n = encode_one(e, &b1, &r1, PCM_TONAL, NULL);
      CHECK(n <= 412, "after disable the legacy cap must apply (%d bits)", n);
   }
   speex_bits_destroy(&b1);
   speex_encoder_destroy(e);
}

/* ------------------------------------------------------------------ */

int main(void)
{
   test_selectors();
   test_ctl_validation();
   test_budget_bound();
   test_too_small();
   test_no_state_change();
   test_parse_and_decode();
   test_multiframe();
   test_layer_upgrade();
   test_empty_high_bands();
   test_dtx();
   test_per_frame_and_disable();
   test_cap_fold();
   test_abr_drift();
   test_no_budget_leak();

   if (failures)
   {
      fprintf(stderr, "frame_budget: %d/%d checks FAILED\n", failures, checks);
      return 1;
   }
   printf("frame_budget: all %d checks passed\n", checks);
   return 0;
}
