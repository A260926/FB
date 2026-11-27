/* frame_budget.c - tests for the per-frame VBR/ABR bit budget
 *
 * The budget (SPEEX_SET_VBR_FRAME_BITS) is the number of audio bits one encode
 * call actually appends: the narrowband frame plus every higher layer's mode
 * header.  These tests cover
 *   - the low-frequency-first deterministic allocator (driven through internal
 *     mode-demand hooks so the expected choice is known exactly),
 *   - real, non-zero PCM encoded through the public entry point and parsed with
 *     an independent table of per-mode frame lengths,
 *   - continuous decoding of the resulting NB/WB/UWB streams,
 *   - the legacies that must not change when the budget is 0.
 *
 * A failing check prints a message and exits non-zero.
 */
#include <speex/speex.h>
#include <speex/speex_bits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal encoder requests used to drive and observe the allocator.  They are
 * numeric copies of libspeex/modes.h entries; the test intentionally uses the
 * public control surface for everything that is part of the feature. */
#define IT_SET_FRAME_BITS_LIMIT 107
#define IT_GET_FRAME_BITS_USED  108
#define IT_GET_MIN_FRAME_BITS   109
#define IT_GET_ABR_DRIFT        110
#define IT_SET_TEST_LOW_DEMAND  111
#define IT_SET_TEST_HIGH_DEMAND 112

static int failures = 0;

#define CHECK(cond) do {                                                    \
   if (!(cond)) {                                                           \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                           \
   }                                                                        \
} while (0)

/* ---- independent per-mode length tables ---------------------------------
 * NB entry = 1-bit flag + 4-bit submode header + payload.  SB entry = 1-bit
 * wideband flag + 3-bit submode header + payload.  These were read off the
 * published sub-mode tables, not out of the encoder. */
static const int NB_TOTAL[9] = {5, 43, 119, 160, 220, 300, 364, 492, 79};
static const int SB_TOTAL[5] = {4, 36, 112, 192, 352};

typedef struct {
   int nb_mode;
   int wb_mode;      /* -1 for a narrowband stream */
   int uwb_mode;     /* -1 unless the stream has an ultra-wideband layer */
   int total_bits;   /* audio bits actually present in the frame */
} FrameInfo;

static int bit_get(const unsigned char *d, int *pos)
{
   int b = (d[*pos >> 3] >> (7 - (*pos & 7))) & 1;
   (*pos)++;
   return b;
}

static int bit_read(const unsigned char *d, int *pos, int n)
{
   int v = 0, i;
   for (i = 0; i < n; i++)
      v = (v << 1) | bit_get(d, pos);
   return v;
}

/* Parse a complete frame written by the encoder.  level: 0=NB, 1=WB, 2=UWB. */
static void parse_frame(const unsigned char *d, int level, FrameInfo *fi)
{
   int pos = 0;
   fi->wb_mode = -1;
   fi->uwb_mode = -1;
   CHECK(bit_read(d, &pos, 1) == 0);          /* narrowband flag */
   fi->nb_mode = bit_read(d, &pos, 4);
   CHECK(fi->nb_mode >= 0 && fi->nb_mode <= 8);
   pos += NB_TOTAL[fi->nb_mode] - 5;
   if (level >= 1) {
      CHECK(bit_read(d, &pos, 1) == 1);       /* wideband block present */
      fi->wb_mode = bit_read(d, &pos, 3);
      CHECK(fi->wb_mode >= 0 && fi->wb_mode <= 4);
      pos += SB_TOTAL[fi->wb_mode] - 4;
   }
   if (level >= 2) {
      CHECK(bit_read(d, &pos, 1) == 1);       /* ultra-wideband block */
      fi->uwb_mode = bit_read(d, &pos, 3);
      CHECK(fi->uwb_mode >= 0 && fi->uwb_mode <= 4);
      pos += SB_TOTAL[fi->uwb_mode] - 4;
   }
   fi->total_bits = pos;
}

/* ---- helpers ------------------------------------------------------------ */

typedef struct {
   void *enc;
   void *dec;
   int frame_size;
   int level;
} Codec;

static void codec_open(Codec *c, int mode, int vbr, float vbr_quality, int submode_on)
{
   const SpeexMode *m = speex_lib_get_mode(mode);
   c->level = mode;
   c->enc = speex_encoder_init(m);
   c->dec = speex_decoder_init(m);
   CHECK(c->enc != NULL && c->dec != NULL);
   speex_encoder_ctl(c->enc, SPEEX_GET_FRAME_SIZE, &c->frame_size);
   if (vbr) {
      spx_int32_t one = 1;
      speex_encoder_ctl(c->enc, SPEEX_SET_VBR, &one);
      speex_encoder_ctl(c->enc, SPEEX_SET_VBR_QUALITY, &vbr_quality);
   }
   if (submode_on) {
      spx_int32_t one = 1;
      speex_encoder_ctl(c->enc, SPEEX_SET_SUBMODE_ENCODING, &one);
   }
}

static void codec_close(Codec *c)
{
   speex_encoder_destroy(c->enc);
   speex_decoder_destroy(c->dec);
}

static void set_ctl(Codec *c, int req, spx_int32_t v)
{
   CHECK(speex_encoder_ctl(c->enc, req, &v) == 0);
}

static int try_ctl(Codec *c, int req, spx_int32_t v)
{
   return speex_encoder_ctl(c->enc, req, &v);
}

static spx_int32_t get_ctl(Codec *c, int req)
{
   spx_int32_t v = -12345;
   speex_encoder_ctl(c->enc, req, &v);
   return v;
}

/* Fill a frame with deterministic, non-zero PCM. */
static void fill_pcm(short *in, int n, int seed, int loud)
{
   uint32_t s = (uint32_t)seed * 2654435761u + 1u;
   int i;
   for (i = 0; i < n; i++) {
      s = 1664525u * s + 1013904223u;
      in[i] = (short)((int)(s >> 17) - 16384);
      if (!loud)
         in[i] = (short)(in[i] / 64);
   }
}

/* Encode one frame into a fresh SpeexBits, optionally after a bit prefix.
 * Returns the encoder's status, the audio bits used (encoder's own count) and
 * fills the written bytes. */
static int encode_one(Codec *c, const short *pcm, int prefix_bits, int prefix_val,
                      unsigned char *bytes, int *nbytes, int *used)
{
   SpeexBits b;
   int i, ret;
   speex_bits_init(&b);
   for (i = 0; i < prefix_bits; i++)
      speex_bits_pack(&b, (prefix_val >> (prefix_bits - 1 - i)) & 1, 1);
   ret = speex_encode_int(c->enc, (short *)pcm, &b);
   if (bytes)
      *nbytes = speex_bits_write(&b, (char *)bytes, 4096);
   if (used) {
      spx_int32_t u = -1;
      speex_encoder_ctl(c->enc, IT_GET_FRAME_BITS_USED, &u);
      *used = u;
   }
   speex_bits_destroy(&b);
   return ret;
}

/* ---- tests -------------------------------------------------------------- */

/* NB mode 8 costs 79 bits and mode 2 costs 119: mode number order must not be
 * confused with bit-size order when fitting a budget. */
static void test_nb_mode8_vs_2(void)
{
   Codec c;
   short pcm[160];
   unsigned char bytes[4096];
   int nbytes, used, ret;
   FrameInfo fi;

   codec_open(&c, 0, 1, 8.f, 1);
   fill_pcm(pcm, c.frame_size, 11, 1);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 100);
   /* Demand mode 2 (119 bits); 100 cannot hold it, the largest fitting mode
    * is 8 (79 bits), not 2 and not a smaller-numbered mode. */
   set_ctl(&c, IT_SET_TEST_LOW_DEMAND, 2);
   ret = encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
   CHECK(ret == 1);
   parse_frame(bytes, 0, &fi);
   CHECK(fi.nb_mode == 8);
   CHECK(fi.total_bits == 79);
   CHECK(used == 79);
   /* Demand 8 with room for 2: only modes no larger than the demand qualify,
    * so mode 8 again (2 is larger than the demand). */
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 119);
   set_ctl(&c, IT_SET_TEST_LOW_DEMAND, 8);
   encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
   parse_frame(bytes, 0, &fi);
   CHECK(fi.nb_mode == 8);
   CHECK(fi.total_bits == 79);
   /* Demand 2 with enough room picks mode 2 exactly, as long as the higher
    * priority demand of mode 8 is cleared. */
   set_ctl(&c, IT_SET_TEST_LOW_DEMAND, 2);
   encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
   parse_frame(bytes, 0, &fi);
   CHECK(fi.nb_mode == 2);
   CHECK(fi.total_bits == 119);
   codec_close(&c);
}

/* Exact budget fill plus prefix independence for NB. */
static void test_nb_budget_and_prefix(void)
{
   Codec c, d;
   short pcm[160];
   unsigned char a[4096], b[4096];
   int na, nb, ua, ub, i;
   FrameInfo fi;

   codec_open(&c, 0, 1, 8.f, 1);
   codec_open(&d, 0, 1, 8.f, 1);
   fill_pcm(pcm, c.frame_size, 21, 1);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 160);
   set_ctl(&d, SPEEX_SET_VBR_FRAME_BITS, 160);
   encode_one(&c, pcm, 0, 0, a, &na, &ua);
   parse_frame(a, 0, &fi);
   CHECK(fi.nb_mode == 3);          /* exactly 160 bits */
   CHECK(fi.total_bits == 160);
   CHECK(ua == 160);

   /* A non-byte-aligned prefix must not change the audio allocation.  Compare
    * freshly-created encoders so the frame history is identical; the prefix
    * shifts the byte packing but not the audio bits. */
   encode_one(&d, pcm, 5, 0x15, b, &nb, &ub);
   CHECK(ub == ua);
   {
      int pa = 0, pb = 5;
      for (i = 0; i < ua; i++)
         CHECK(bit_get(a, &pa) == bit_get(b, &pb));
   }
   codec_close(&c);
   codec_close(&d);
}

/* WB: saving bits in the low layer lets the outer layer take a higher mode. */
static void test_wb_low_saving_raises_high(void)
{
   Codec c;
   short pcm[320];
   unsigned char bytes[4096];
   int nbytes, used;
   FrameInfo fi;

   codec_open(&c, 1, 1, 8.f, 1);
   fill_pcm(pcm, c.frame_size, 31, 1);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 200);
   set_ctl(&c, IT_SET_TEST_HIGH_DEMAND, 4);   /* ask for the biggest high band */

   /* Low demand mode 2 (119 bits) -> only 81 bits remain -> high mode 1. */
   set_ctl(&c, IT_SET_TEST_LOW_DEMAND, 2);
   encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
   parse_frame(bytes, 1, &fi);
   CHECK(fi.nb_mode == 2);
   CHECK(fi.wb_mode == 1);
   CHECK(fi.total_bits == NB_TOTAL[2] + SB_TOTAL[1]);
   CHECK(fi.total_bits <= 200);
   CHECK(used == fi.total_bits);

   /* Low demand mode 8 (79 bits) -> 121 bits remain -> high mode 2. */
   set_ctl(&c, IT_SET_TEST_LOW_DEMAND, 8);
   encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
   parse_frame(bytes, 1, &fi);
   CHECK(fi.nb_mode == 8);
   CHECK(fi.wb_mode == 2);
   CHECK(fi.total_bits == NB_TOTAL[8] + SB_TOTAL[2]);
   CHECK(fi.total_bits <= 200);
   codec_close(&c);
}

/* UWB low layer is a whole WB subtree: the NB bits alone do not describe it. */
static void test_uwb_recursion(void)
{
   Codec c;
   short pcm[640];
   unsigned char bytes[4096];
   int nbytes, used;
   FrameInfo fi;

   codec_open(&c, 2, 1, 8.f, 1);
   fill_pcm(pcm, c.frame_size, 41, 1);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 200);
   encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
   parse_frame(bytes, 2, &fi);
   /* WB child: NB 160 + WB mode 1 (36) = 196, then UWB header leaves 4 bits,
    * so the UWB high band is empty.  Subtracting only the NB bits would leave
    * 40 bits and would have fit UWB mode 1. */
   CHECK(fi.nb_mode == 3);
   CHECK(fi.wb_mode == 1);
   CHECK(fi.uwb_mode == 0);
   CHECK(fi.total_bits == 200);
   CHECK(used == 200);
   CHECK(NB_TOTAL[fi.nb_mode] == 160);   /* low is far more than the NB block */
   codec_close(&c);
}

/* Each of the three layers can end up with an empty high band. */
static void test_empty_high_layers(void)
{
   Codec c;
   short pcm[640];
   unsigned char bytes[4096];
   int nbytes;
   FrameInfo fi;

   /* NB: a budget that only holds mode 1. */
   codec_open(&c, 0, 1, 8.f, 1);
   fill_pcm(pcm, c.frame_size, 51, 1);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 43);
   encode_one(&c, pcm, 0, 0, bytes, &nbytes, NULL);
   parse_frame(bytes, 0, &fi);
   CHECK(fi.nb_mode == 1 && fi.total_bits == 43);
   codec_close(&c);

   /* WB: low=8 (79) and only 11 bits left -> WB mode 0 (4-bit header). */
   codec_open(&c, 1, 1, 8.f, 1);
   fill_pcm(pcm, c.frame_size, 52, 1);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 90);
   set_ctl(&c, IT_SET_TEST_LOW_DEMAND, 8);
   set_ctl(&c, IT_SET_TEST_HIGH_DEMAND, 4);
   encode_one(&c, pcm, 0, 0, bytes, &nbytes, NULL);
   parse_frame(bytes, 1, &fi);
   CHECK(fi.nb_mode == 8);
   CHECK(fi.wb_mode == 0);
   CHECK(fi.total_bits == 83);
   codec_close(&c);

   /* UWB: both high bands empty (WB and UWB sub-modes 0). */
   codec_open(&c, 2, 1, 8.f, 1);
   fill_pcm(pcm, c.frame_size, 53, 1);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 51);
   encode_one(&c, pcm, 0, 0, bytes, &nbytes, NULL);
   parse_frame(bytes, 2, &fi);
   CHECK(fi.nb_mode == 1);
   CHECK(fi.wb_mode == 0);
   CHECK(fi.uwb_mode == 0);
   CHECK(fi.total_bits == 51);
   codec_close(&c);
}

/* DTX must be able to enter (silent, time-driven) and recover, both with and
 * without a budget, and the two must agree frame for frame. */
static void test_dtx(void)
{
   Codec c, ref;
   short pcm[160];
   unsigned char bytes[4096];
   int nbytes, used, f, entered = 0, recovered = 0;
   spx_int32_t one = 1;

   codec_open(&c, 0, 1, 8.f, 1);
   codec_open(&ref, 0, 1, 8.f, 1);
   speex_encoder_ctl(c.enc, SPEEX_SET_DTX, &one);
   speex_encoder_ctl(ref.enc, SPEEX_SET_DTX, &one);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 60);
   for (f = 0; f < 60; f++) {
      int ret, rret, rused;
      if (f < 6 || (f >= 34 && f < 40))
         fill_pcm(pcm, c.frame_size, 300 + f, 1);   /* voiced */
      else
         memset(pcm, 0, sizeof(pcm));               /* silence */
      ret = encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
      rret = encode_one(&ref, pcm, 0, 0, bytes, &nbytes, &rused);
      CHECK(ret == rret);                            /* DTX decision identical */
      if (ret == 0) {
         entered = 1;
         CHECK(used == 5);                           /* header only, null mode */
      } else if (entered && f >= 34) {
         recovered = 1;
         CHECK(used <= 60);                  /* within the budget when voiced */
      }
   }
   CHECK(entered);
   CHECK(recovered);
   codec_close(&c);
   codec_close(&ref);

   /* The budget-setting path must accept DTX on and off freely. */
   codec_open(&c, 0, 1, 8.f, 1);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 60);
   set_ctl(&c, SPEEX_SET_DTX, 1);
   set_ctl(&c, SPEEX_SET_DTX, 0);
   set_ctl(&c, SPEEX_SET_DTX, 1);
   codec_close(&c);
}

/* Budget may change between frames, and a single budget may alternate. */
static void test_per_frame_switch(void)
{
   Codec c;
   short pcm[160];
   unsigned char bytes[4096];
   int nbytes, used;
   int i;
   static const int budgets[6] = {43, 119, 79, 220, 160, 300};
   static const int expect[6]  = {1, 2, 8, 4, 3, 5};
   FrameInfo fi;

   codec_open(&c, 0, 1, 8.f, 1);
   fill_pcm(pcm, c.frame_size, 71, 1);
   for (i = 0; i < 6; i++) {
      set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, budgets[i]);
      encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
      parse_frame(bytes, 0, &fi);
      CHECK(fi.nb_mode == expect[i]);
      CHECK(fi.total_bits == NB_TOTAL[expect[i]]);
      CHECK(used == budgets[i]);
   }
   codec_close(&c);
}

/* Several frames share one SpeexBits; decoding must yield exactly that many
 * frames of finite PCM, with the terminator appended only when writing. */
static void test_multi_frame_and_decode(void)
{
   Codec c;
   SpeexBits b;
   short pcm[640], out[640];
   unsigned char bytes[16384];
   int f, i, nbytes, ok = 1, frames = 8;
   int prefix = 3, prefix_val = 5;

   codec_open(&c, 2, 1, 8.f, 1);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 180);
   speex_bits_init(&b);
   for (i = 0; i < prefix; i++)
      speex_bits_pack(&b, (prefix_val >> (prefix - 1 - i)) & 1, 1);
   for (f = 0; f < frames; f++) {
      fill_pcm(pcm, c.frame_size, 100 + f, f % 3 != 0);
      CHECK(speex_encode_int(c.enc, pcm, &b) >= 0);
   }
   nbytes = speex_bits_write(&b, (char *)bytes, sizeof(bytes));
   CHECK(nbytes > 0);
   /* Prefix intact. */
   for (i = 0; i < prefix; i++)
      CHECK(((bytes[i >> 3] >> (7 - (i & 7))) & 1) == ((prefix_val >> (prefix - 1 - i)) & 1));

   {
      SpeexBits rd;
      speex_bits_init(&rd);
      speex_bits_read_from(&rd, (char *)bytes, nbytes);
      speex_bits_advance(&rd, prefix);
      for (f = 0; f < frames; f++) {
         int ret = speex_decode_int(c.dec, &rd, out);
         if (ret != 0) { ok = 0; break; }
         for (i = 0; i < c.frame_size; i++)
            if (!isfinite((double)out[i])) { ok = 0; break; }
      }
      CHECK(ok);
      /* No spurious extra frame before the terminator: the next decode must
       * report end-of-stream (-1). */
      CHECK(speex_decode_int(c.dec, &rd, out) == -1);
      speex_bits_destroy(&rd);
   }
   speex_bits_destroy(&b);
   codec_close(&c);
}

/* With the budget off, the old controls and their output are untouched. */
static void test_legacy_equivalence(void)
{
   static const int caps[5] = {0, 20600, 27800, 32000, 42200};
   int mode, ci, f;
   for (mode = 0; mode < 3; mode++) {
      for (ci = 0; ci < 5; ci++) {
         Codec a, b;
         short pcm[640];
         unsigned char ba[4096], bb[4096];
         codec_open(&a, mode, 1, 8.f, 1);
         codec_open(&b, mode, 1, 8.f, 1);
         if (caps[ci]) {
            set_ctl(&a, SPEEX_SET_VBR_MAX_BITRATE, caps[ci]);
            set_ctl(&b, SPEEX_SET_VBR_MAX_BITRATE, caps[ci]);
         }
         /* b enables and disables the new mode before encoding anything: it
          * must still produce the identical legacy stream. */
         set_ctl(&b, SPEEX_SET_VBR_FRAME_BITS, 500);
         CHECK(get_ctl(&b, SPEEX_GET_VBR_FRAME_BITS) == 500);
         set_ctl(&b, SPEEX_SET_VBR_FRAME_BITS, 0);
         CHECK(get_ctl(&b, SPEEX_GET_VBR_FRAME_BITS) == 0);
         for (f = 0; f < 6; f++) {
            int na, nb;
            fill_pcm(pcm, a.frame_size, 700 + f, f % 2);
            encode_one(&a, pcm, f % 4, 0xA, ba, &na, NULL);
            encode_one(&b, pcm, f % 4, 0xA, bb, &nb, NULL);
            CHECK(na == nb);
            if (na == nb)
               CHECK(memcmp(ba, bb, na) == 0);
         }
         codec_close(&a);
         codec_close(&b);
      }
   }
}

/* Turning the budget off after use must restore the legacy layered cap and not
 * leave the previous frame's sub-encoder allowance behind. */
static void test_off_restores_cap(void)
{
   Codec ref, c;
   short pcm[320];
   unsigned char br[4096], bc[4096];
   int nr, nc, f;

   codec_open(&ref, 1, 1, 8.f, 1);
   codec_open(&c, 1, 1, 8.f, 1);
   set_ctl(&ref, SPEEX_SET_VBR_MAX_BITRATE, 20600);
   set_ctl(&c, SPEEX_SET_VBR_MAX_BITRATE, 20600);
   for (f = 0; f < 4; f++) {
      fill_pcm(pcm, 320, 800 + f, 1);
      encode_one(&ref, pcm, 0, 0, br, &nr, NULL);
      encode_one(&c, pcm, 0, 0, bc, &nc, NULL);
      CHECK(nr == nc && memcmp(br, bc, nr) == 0);
   }
   /* Both encoders process the same next frame: ref via the legacy layered
    * cap, c while the new budget is still active. */
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 400);
   fill_pcm(pcm, 320, 900, 1);
   encode_one(&ref, pcm, 0, 0, br, &nr, NULL);
   encode_one(&c, pcm, 0, 0, bc, &nc, NULL);
   /* ... then both are reset identically and the budget switched off. */
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 0);
   {
      spx_int32_t z = 0;
      speex_encoder_ctl(ref.enc, SPEEX_RESET_STATE, &z);
      speex_encoder_ctl(c.enc, SPEEX_RESET_STATE, &z);
   }
   fill_pcm(pcm, 320, 901, 1);
   encode_one(&ref, pcm, 0, 0, br, &nr, NULL);
   encode_one(&c, pcm, 0, 0, bc, &nc, NULL);
   CHECK(nr == nc);
   CHECK(memcmp(br, bc, nr) == 0);
   codec_close(&ref);
   codec_close(&c);
}

/* Invalid controls: bad ranges and unmet preconditions are rejected and the
 * stored value is preserved. */
static void test_invalid_controls(void)
{
   int mode;
   static const int lo[3] = {43, 47, 51};
   for (mode = 0; mode < 3; mode++) {
      Codec c;
      spx_int32_t v;
      codec_open(&c, mode, 0, 8.f, 1);   /* VBR off */
      CHECK(get_ctl(&c, SPEEX_GET_VBR_FRAME_BITS) == 0);
      CHECK(try_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, lo[mode]) == -1);
      CHECK(get_ctl(&c, SPEEX_GET_VBR_FRAME_BITS) == 0);

      set_ctl(&c, SPEEX_SET_VBR, 1);
      CHECK(try_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, lo[mode] - 1) == -1);
      CHECK(try_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 4097) == -1);
      CHECK(get_ctl(&c, SPEEX_GET_VBR_FRAME_BITS) == 0);
      CHECK(try_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, lo[mode]) == 0);
      CHECK(get_ctl(&c, SPEEX_GET_VBR_FRAME_BITS) == lo[mode]);

      /* Cannot turn off submode encoding while active. */
      CHECK(try_ctl(&c, SPEEX_SET_SUBMODE_ENCODING, 0) == 0);  /* silently kept */
      CHECK(get_ctl(&c, SPEEX_GET_SUBMODE_ENCODING) == 1);
      /* Cannot turn off the last VBR/ABR switch while active. */
      CHECK(try_ctl(&c, SPEEX_SET_VBR, 0) == 0);
      CHECK(get_ctl(&c, SPEEX_GET_VBR) == 1);

      /* Once the budget is cleared those controls work again. */
      CHECK(try_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 0) == 0);
      CHECK(try_ctl(&c, SPEEX_SET_VBR, 0) == 0);
      CHECK(get_ctl(&c, SPEEX_GET_VBR) == 0);
      CHECK(try_ctl(&c, SPEEX_SET_SUBMODE_ENCODING, 0) == 0);
      CHECK(get_ctl(&c, SPEEX_GET_SUBMODE_ENCODING) == 0);

      /* ABR alone satisfies the precondition too.  Submode encoding must be
       * back on first. */
      ((void)v);
      CHECK(try_ctl(&c, SPEEX_SET_SUBMODE_ENCODING, 1) == 0);
      {
         spx_int32_t abr = 6000;
         speex_encoder_ctl(c.enc, SPEEX_SET_ABR, &abr);
      }
      CHECK(try_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, lo[mode]) == 0);
      /* While active, neither clearing ABR (which also clears the VBR flag)
       * nor clearing the VBR flag may disable the last analysis mode. */
      CHECK(try_ctl(&c, SPEEX_SET_ABR, 0) == 0);
      CHECK(get_ctl(&c, SPEEX_GET_VBR) == 1);
      CHECK(try_ctl(&c, SPEEX_SET_VBR, 0) == 0);
      CHECK(get_ctl(&c, SPEEX_GET_VBR) == 1);
      CHECK(get_ctl(&c, SPEEX_GET_VBR_FRAME_BITS) == lo[mode]);
      codec_close(&c);
   }
}

/* An effective budget below the smallest frame fails before the frame is
 * touched: PCM, the encoder state and the bit-stream stay intact. */
static void test_effective_too_small(void)
{
   Codec c, fresh;
   short pcm[320], snapshot[320];
   unsigned char bc[4096], bf[4096];
   int nc, nf, used, f;
   SpeexBits b;

   codec_open(&c, 1, 1, 8.f, 1);
   codec_open(&fresh, 1, 1, 8.f, 1);
   /* Both are configured identically: budget 60 is valid, but the legacy cap
    * folds to 40 bits, below the narrowband minimum of 43. */
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 60);
   set_ctl(&c, SPEEX_SET_VBR_MAX_BITRATE, 2000);
   set_ctl(&fresh, SPEEX_SET_VBR_FRAME_BITS, 60);
   set_ctl(&fresh, SPEEX_SET_VBR_MAX_BITRATE, 2000);
   fill_pcm(pcm, 320, 950, 1);
   memcpy(snapshot, pcm, sizeof(pcm));

   speex_bits_init(&b);
   CHECK(speex_encode_int(c.enc, pcm, &b) == -1);
   CHECK(b.nbBits == 0);
   CHECK(memcmp(pcm, snapshot, sizeof(pcm)) == 0);
   speex_bits_destroy(&b);

   /* The rejected call advanced no encoder state: with the same knobs removed
    * identically from both, the streams match a fresh, never-failed encoder. */
   set_ctl(&c, SPEEX_SET_VBR_MAX_BITRATE, 0);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 0);
   set_ctl(&fresh, SPEEX_SET_VBR_MAX_BITRATE, 0);
   set_ctl(&fresh, SPEEX_SET_VBR_FRAME_BITS, 0);
   for (f = 0; f < 3; f++) {
      fill_pcm(pcm, 320, 960 + f, 1);
      encode_one(&c, pcm, 0, 0, bc, &nc, &used);
      encode_one(&fresh, pcm, 0, 0, bf, &nf, &used);
      CHECK(nc == nf && memcmp(bc, bf, nc) == 0);
   }
   codec_close(&c);
   codec_close(&fresh);
}

/* VBR_MAX_BITRATE folds into whole-frame bits; a sampling-rate override uses
 * the override for the cap but never changes the explicit bit budget. */
static void test_vbr_max_and_sampling(void)
{
   Codec c;
   short pcm[320];
   unsigned char bytes[4096];
   int nbytes, used, u16, u8;
   FrameInfo fi;

   codec_open(&c, 1, 1, 8.f, 1);
   fill_pcm(pcm, 320, 970, 1);

   /* cap 20600 at 16 kHz -> floor(20600*320/16000)=412 bits whole frame. */
   set_ctl(&c, SPEEX_SET_VBR_MAX_BITRATE, 20600);
   encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
   parse_frame(bytes, 1, &fi);
   CHECK(fi.total_bits <= 412);
   u16 = fi.total_bits;

   /* Budget 100 is smaller than the folded cap, so the budget wins. */
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 100);
   encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
   parse_frame(bytes, 1, &fi);
   CHECK(fi.total_bits <= 100);
   CHECK(used == fi.total_bits);
   u16 = fi.total_bits;

   /* Sampling-rate override leaves the explicit bit budget alone. */
   set_ctl(&c, SPEEX_SET_SAMPLING_RATE, 8000);
   encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
   parse_frame(bytes, 1, &fi);
   u8 = fi.total_bits;
   CHECK(u8 == u16);

   /* The legacy cap alone follows the overridden clock: 20600 at 8 kHz. */
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 0);
   encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
   parse_frame(bytes, 1, &fi);
   CHECK(fi.total_bits <= (int)floor(20600.0 * 320 / 8000));
   codec_close(&c);
}

/* ABR drift must be fed from the committed frame's bits, converted at the
 * layer's clock, and advance exactly once per frame. */
static void test_abr_drift(void)
{
   Codec c;
   short pcm[640];
   unsigned char bytes[4096];
   int nbytes, used, f;
   spx_int32_t abr = 6000;   /* deliberately away from any mode's rate */
   float drift[3];
   FrameInfo fi;

   /* NB.  The drift advances by (committed_rate - abr) each frame, where the
    * committed rate is derived from the mode actually written, not from the
    * mode the analysis asked for or the legacy fixed quota. */
   codec_open(&c, 0, 0, 8.f, 1);
   speex_encoder_ctl(c.enc, SPEEX_SET_ABR, &abr);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 160);
   {
      float prev_drift = 0, prev_count = 0;
      for (f = 0; f < 6; f++) {
         double expect;
         fill_pcm(pcm, c.frame_size, 980 + f, 1);
         encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
         parse_frame(bytes, 0, &fi);
         speex_encoder_ctl(c.enc, IT_GET_ABR_DRIFT, drift);
         CHECK(drift[2] == prev_count + 1.f);
         expect = floor(8000.0 * NB_TOTAL[fi.nb_mode] / 160.0);
         CHECK(fabs((drift[0] - prev_drift) - (expect - abr)) < 1e-3);
         prev_drift = drift[0];
         prev_count = drift[2];
      }
   }
   codec_close(&c);

   /* WB: the per-frame drift uses the whole committed sub-tree at the WB clock,
    * never the legacy fixed low/high quota. */
   codec_open(&c, 1, 0, 8.f, 1);
   speex_encoder_ctl(c.enc, SPEEX_SET_ABR, &abr);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 220);
   {
      float prev_drift = 0;
      for (f = 0; f < 4; f++) {
         double expect;
         fill_pcm(pcm, c.frame_size, 990 + f, 1);
         encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
         parse_frame(bytes, 1, &fi);
         speex_encoder_ctl(c.enc, IT_GET_ABR_DRIFT, drift);
         expect = floor(16000.0 * fi.total_bits / 320.0);
         CHECK(fabs((drift[0] - prev_drift) - (expect - abr)) < 1e-3);
         prev_drift = drift[0];
      }
      CHECK(drift[2] == 4.f);
   }
   codec_close(&c);

   /* UWB: the parent aggregates the whole three-layer sub-tree at 32 kHz. */
   codec_open(&c, 2, 0, 8.f, 1);
   speex_encoder_ctl(c.enc, SPEEX_SET_ABR, &abr);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 460);
   {
      float prev_drift = 0;
      for (f = 0; f < 3; f++) {
         double expect;
         fill_pcm(pcm, c.frame_size, 1070 + f, 1);
         encode_one(&c, pcm, 0, 0, bytes, &nbytes, &used);
         parse_frame(bytes, 2, &fi);
         speex_encoder_ctl(c.enc, IT_GET_ABR_DRIFT, drift);
         expect = floor(32000.0 * fi.total_bits / 640.0);
         CHECK(fabs((drift[0] - prev_drift) - (expect - abr)) < 1e-3);
         prev_drift = drift[0];
      }
      CHECK(drift[2] == 3.f);
   }
   codec_close(&c);
}

/* Decode a stream of real frames continuously and verify frame count and PCM. */
static void test_continuous_decode_all_modes(void)
{
   int mode;
   for (mode = 0; mode < 3; mode++) {
      Codec c;
      short pcm[640], out[640];
      unsigned char bytes[8192];
      int f, i, nbytes, ok = 1, frames = 12;
      SpeexBits b, rd;

      codec_open(&c, mode, 1, 8.f, 1);
      set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, mode == 0 ? 220 : mode == 1 ? 300 : 420);
      speex_bits_init(&b);
      for (f = 0; f < frames; f++) {
         fill_pcm(pcm, c.frame_size, 1100 + f, 1);
         CHECK(speex_encode_int(c.enc, pcm, &b) >= 0);
      }
      nbytes = speex_bits_write(&b, (char *)bytes, sizeof(bytes));
      speex_bits_init(&rd);
      speex_bits_read_from(&rd, (char *)bytes, nbytes);
      for (f = 0; f < frames; f++) {
         int ret = speex_decode_int(c.dec, &rd, out);
         if (ret != 0) { ok = 0; break; }
         for (i = 0; i < c.frame_size; i++)
            if (!isfinite((double)out[i])) { ok = 0; break; }
      }
      CHECK(ok);
      CHECK(speex_decode_int(c.dec, &rd, out) == -1);
      speex_bits_destroy(&rd);
      speex_bits_destroy(&b);
      codec_close(&c);
   }
}

/* A budget-only frame must still fit when it is appended after an arbitrary
 * prefix, and the prefix bits are never overwritten. */
static void test_prefix_not_overwritten(void)
{
   Codec c;
   short pcm[160];
   SpeexBits b;
   unsigned char bytes[4096];
   int nbytes, i, prefix = 11;
   unsigned prefixval = 0x2A5;   /* 11 bits */

   codec_open(&c, 0, 1, 8.f, 1);
   set_ctl(&c, SPEEX_SET_VBR_FRAME_BITS, 119);
   fill_pcm(pcm, c.frame_size, 1200, 1);
   speex_bits_init(&b);
   speex_bits_pack(&b, 0x2A5, 11);
   speex_encode_int(c.enc, pcm, &b);
   nbytes = speex_bits_write(&b, (char *)bytes, sizeof(bytes));
   CHECK(nbytes > 0);
   for (i = 0; i < prefix; i++)
      CHECK(((bytes[i >> 3] >> (7 - (i & 7))) & 1) == ((prefixval >> (prefix - 1 - i)) & 1));
   /* The narrowband mode header follows immediately at bit 11. */
   {
      int pos = prefix;
      CHECK(bit_read(bytes, &pos, 1) == 0);
      CHECK(bit_read(bytes, &pos, 4) == 2);
   }
   speex_bits_destroy(&b);
   codec_close(&c);
}

int main(void)
{
   test_nb_mode8_vs_2();
   test_nb_budget_and_prefix();
   test_wb_low_saving_raises_high();
   test_uwb_recursion();
   test_empty_high_layers();
   test_dtx();
   test_per_frame_switch();
   test_multi_frame_and_decode();
   test_legacy_equivalence();
   test_off_restores_cap();
   test_invalid_controls();
   test_effective_too_small();
   test_vbr_max_and_sampling();
   test_abr_drift();
   test_continuous_decode_all_modes();
   test_prefix_not_overwritten();

   if (failures) {
      fprintf(stderr, "%d check(s) failed\n", failures);
      return 1;
   }
   printf("frame_budget: all checks passed\n");
   return 0;
}
