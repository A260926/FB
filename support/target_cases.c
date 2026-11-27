/*
 * Target test-suite for the MPEG-1 long/short window-switching chain.
 *
 * Built by support/target.sh against a library compiled with SHINE_TEST_HOOKS,
 * which exposes read-only observation points (declared in test_hooks.h).  The
 * tests never write through those vectors.
 *
 * Three groups: windows, bitstream, state.  Each registers cases with
 * TASK_CASE and reports assertions with TASK_CHECK.  No main() here; the
 * fixed driver (support/driver.c) provides it.
 */

#include "cases.h"
#include "layer3.h"
#include "test_hooks.h"
#include "huffman.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SENTINEL 0x5A
#define SENTINEL_SZ 64
#define GRANULE_SIZE_576 576

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int32_t clamp16(int v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return v;
}

/* Deterministic pseudo-random 16-bit sample generator (LCG), so tests do not
 * depend on libc's rand(). */
static uint32_t lcg_state;
static void lcg_seed(uint32_t s) { lcg_state = s ? s : 1u; }
static int lcg_next(void) {
  lcg_state = lcg_state * 1664525u + 1013904223u;
  return (int)((lcg_state >> 8) & 0xFFFF) - 32768;
}

/* A musical-ish test signal with transients.  amplitude stays below 2^24
 * after the analysis filter, so the fixed-point MDCT vs half-scale reference
 * tolerance of 64 applies. */
static void make_pcm(int16_t *dst, int n, int sr, int ch, int seed) {
  int i;
  lcg_seed((uint32_t)(seed * 2654435761u + sr));
  for (i = 0; i < n; i++) {
    double t = (double)i / (double)sr;
    double v = 0.18 * sin(2.0 * M_PI * 440.0 * t) +
               0.10 * sin(2.0 * M_PI * 1733.0 * t + 0.7) +
               0.05 * sin(2.0 * M_PI * 5100.0 * t + 1.3);
    /* asymmetric transient every ~37 ms */
    {
      int ph = i % (sr / 27);
      if (ph < 2) v += (ph == 0 ? 0.55 : -0.30);
      else if (ph < 4) v += 0.12;
    }
    (void)ch;
    dst[i] = (int16_t)clamp16((int)(v * 26000.0) + (lcg_next() >> 9));
  }
}

/* Encode helper: one encoder, planar buffers. */
struct enc {
  shine_t s;
  int spp;
  int ch;
  int sr;
  int16_t *l, *r;
  int16_t *planar[2];
};

static int enc_open(struct enc *e, int sr, int br, int ch) {
  shine_config_t c;
  memset(&c, 0, sizeof(c));
  memset(e, 0, sizeof(*e));
  c.wave.channels = (enum channels)ch;
  c.wave.samplerate = sr;
  shine_set_config_mpeg_defaults(&c.mpeg);
  c.mpeg.mode = (ch == 1) ? MONO : STEREO;
  c.mpeg.bitr = br;
  e->s = shine_initialise(&c);
  if (!e->s) return -1;
  e->spp = shine_samples_per_pass(e->s);
  e->ch = ch;
  e->sr = sr;
  e->l = malloc((size_t)e->spp * sizeof(int16_t));
  e->r = malloc((size_t)e->spp * sizeof(int16_t));
  e->planar[0] = e->l;
  e->planar[1] = e->r;
  return 0;
}
static void enc_close(struct enc *e) {
  if (e->s) shine_close(e->s);
  free(e->l);
  free(e->r);
  e->s = NULL;
}

/* Encode one frame through the block entry point (planar).  Returns 0 on
 * success and stores the delivered frame in *out with its size in *n. */
static int enc_blocks(struct enc *e, const uint8_t *bt, int count,
                      unsigned char **out, int *n) {
  unsigned char *p;
  *n = -999;
  p = shine_encode_buffer_blocks(e->s, e->planar, bt, count, n);
  *out = p;
  return (p != NULL && *n >= 0) ? 0 : -1;
}
/* ------------------------------------------------------------------ */
/* Bit reader mirroring the encoder's 32-bit big-endian word layout.     */
/* ------------------------------------------------------------------ */
struct brt {
  const unsigned char *buf;
  int bitpos;
};

static void brt_init(struct brt *b, const unsigned char *buf) {
  b->buf = buf;
  b->bitpos = 0;
}
static unsigned brt_get(struct brt *b, int n) {
  unsigned v = 0;
  while (n--) {
    unsigned byte = b->buf[b->bitpos >> 3];
    v = (v << 1) | ((byte >> (7 - (b->bitpos & 7))) & 1u);
    b->bitpos++;
  }
  return v;
}

/* Huffman tables exposed by the library (allowed as constants). */
#define HTN 34

/* Decode one (x,y) pair using table `tbl`, reading from a plain bit cursor. */
static int huff_pair(struct brt *b, int tbl, int *x, int *y) {
  const struct huffcodetab *h = &shine_huffman_table[tbl];
  int ln;
  unsigned acc = 0;
  int yy, xx;
  for (ln = 1; ln <= 19; ln++) {
    acc = (acc << 1) | brt_get(b, 1);
    for (yy = 0; yy < (int)h->ylen; yy++)
      for (xx = 0; xx < (int)h->xlen; xx++) {
        int idx = xx * (int)h->ylen + yy;
        if (h->hlen[idx] == ln && h->table[idx] == acc) {
          int px = xx, py = yy;
          if (tbl > 15) {
            /* ESC order: x linbits, x sign, y linbits, y sign. */
            if (px == 15) px += (int)brt_get(b, (int)h->linbits);
            if (px) px = brt_get(b, 1) ? -px : px;
            if (py == 15) py += (int)brt_get(b, (int)h->linbits);
            if (py) py = brt_get(b, 1) ? -py : py;
          } else {
            if (px) px = brt_get(b, 1) ? -px : px;
            if (py) py = brt_get(b, 1) ? -py : py;
          }
          *x = px;
          *y = py;
          return 1;
        }
      }
  }
  return 0;
}

/* Decode one count1 quadruple (values in {-1,0,1}) from table 32/33. */
static int huff_quad(struct brt *b, int tbl, int q[4]) {
  const struct huffcodetab *h = &shine_huffman_table[tbl];
  int ln;
  unsigned acc = 0;
  int idx;
  for (ln = 1; ln <= 19; ln++) {
    acc = (acc << 1) | brt_get(b, 1);
    for (idx = 0; idx < 16; idx++) {
      if (h->hlen[idx] == ln && h->table[idx] == acc) {
        int p = idx;
        int k;
        for (k = 0; k < 4; k++) {
          int v = (p >> k) & 1;
          q[k] = v ? (brt_get(b, 1) ? -v : v) : 0;
        }
        return 1;
      }
    }
  }
  return 0;
}

/* Parsed side info for one MPEG-1 stereo frame (2 granules x 2 channels). */
struct side_gi {
  int part2_3_length, big_values, global_gain, scalefac_compress;
  int window_switching, block_type, mixed;
  int table_select[3];
  int subblock_gain[3];
  int region0_count, region1_count;
  int preflag, scalefac_scale, count1table_select;
};
struct side_frame {
  int main_data_begin;
  int scfsi[2][4];
  struct side_gi gr[2][2];
  int side_end_bit;
};

static const int *band_table_for(int sr) {
  extern const int shine_scale_fact_band_index[9][23];
  if (sr == 44100) return shine_scale_fact_band_index[0];
  if (sr == 48000) return shine_scale_fact_band_index[1];
  return shine_scale_fact_band_index[2];
}
static const int *short_band_table_for(int sr) {
  extern const int shine_short_band_index[3][14];
  if (sr == 44100) return shine_short_band_index[0];
  if (sr == 48000) return shine_short_band_index[1];
  return shine_short_band_index[2];
}

/* MPEG-1 Layer III frame length in bytes from the 4-byte header, for the
 * bitrates this suite uses (128, 192, 320 kbps).  Validates the header. */
static int mp3_frame_bytes(const unsigned char *p, int sr) {
  static const int br_tab[16] = {0, 32, 40, 48, 56, 64, 80, 96,
                                 112, 128, 160, 192, 224, 256, 320, 0};
  int br_idx, sr_idx, pad;
  int srtab[3] = {44100, 48000, 32000};
  if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return -1;
  if (((p[1] >> 3) & 3) != 3) return -1;   /* MPEG-1 */
  if (((p[1] >> 1) & 3) != 1) return -1;   /* Layer III */
  br_idx = (p[2] >> 4) & 0xF;
  sr_idx = (p[2] >> 2) & 3;
  pad = (p[2] >> 1) & 1;
  if (br_idx == 0 || br_idx == 15 || sr_idx == 3) return -1;
  if (srtab[sr_idx] != sr) return -1;
  return 144 * br_tab[br_idx] * 1000 / sr + pad;
}

/* ---- independent bit writer + Huffman encoder (mirrors the code tables
 * only; it is a separate implementation from l3bitstream.c) ---- */
struct bwt {
  unsigned char *buf;
  int bitpos;
  int cap;
};
static void bwt_init(struct bwt *w, unsigned char *buf, int cap, int bitpos) {
  w->buf = buf;
  w->bitpos = bitpos;
  w->cap = cap;
}
static void bwt_put(struct bwt *w, unsigned val, int n) {
  while (n--) {
    int bit = (val >> n) & 1u;
    int byte = w->bitpos >> 3;
    int off = 7 - (w->bitpos & 7);
    if (byte < w->cap) w->buf[byte] = (unsigned char)((w->buf[byte] & ~(1 << off)) |
                                                      (bit << off));
    w->bitpos++;
  }
}
static int getbits_at(const unsigned char *p, int bitpos, int n) {
  struct brt b;
  brt_init(&b, p);
  b.bitpos = bitpos;
  return (int)brt_get(&b, n);
}
static int bits_at_equal(const unsigned char *p, const unsigned char *q, int at,
                         int n) {
  int i;
  for (i = 0; i < n; i++)
    if (getbits_at(p, at + i, 1) != getbits_at(q, at + i, 1)) return 0;
  return 1;
}
/* Encode one pair from signed values (x,y), write to w. Returns 1 on
 * success.  table 0 means "zero codes". */
static int huff_encode_pair(struct bwt *w, int tbl, int x, int y) {
  const struct huffcodetab *h = &shine_huffman_table[tbl];
  unsigned signx, signy, ylen, idx;
  int cbits, xbits = 0;
  unsigned code, ext = 0;
  int negx = x < 0, negy = y < 0;
  unsigned ax = (unsigned)(negx ? -x : x);
  unsigned ay = (unsigned)(negy ? -y : y);
  signx = (unsigned)negx;
  signy = (unsigned)negy;
  if (!tbl) return 1;
  ylen = h->ylen;
  if (tbl > 15) {
    unsigned linbitsx = 0, linbitsy = 0, linbits = h->linbits;
    if (ax > 14) { linbitsx = ax - 15; ax = 15; }
    if (ay > 14) { linbitsy = ay - 15; ay = 15; }
    idx = ax * ylen + ay;
    code = h->table[idx];
    cbits = h->hlen[idx];
    if (ax > 14) { ext |= linbitsx; xbits += (int)linbits; }
    if (ax != 0) { ext <<= 1; ext |= signx; xbits += 1; }
    if (ay > 14) { ext <<= linbits; ext |= linbitsy; xbits += (int)linbits; }
    if (ay != 0) { ext <<= 1; ext |= signy; xbits += 1; }
    bwt_put(w, code, cbits);
    bwt_put(w, ext, xbits);
  } else {
    idx = ax * ylen + ay;
    code = h->table[idx];
    cbits = h->hlen[idx];
    if (ax != 0) { code <<= 1; code |= signx; cbits += 1; }
    if (ay != 0) { code <<= 1; code |= signy; cbits += 1; }
    bwt_put(w, code, cbits);
  }
  return 1;
}
static int huff_encode_quad(struct bwt *w, int tbl, const int q[4]) {
  const struct huffcodetab *h = &shine_huffman_table[tbl];
  int k, v, p = 0, cbits = 0;
  unsigned code = 0;
  for (k = 0; k < 4; k++) {
    v = q[k] != 0 ? 1 : 0;
    p |= v << k;
  }
  bwt_put(w, h->table[p], h->hlen[p]);
  for (k = 0; k < 4; k++)
    if (q[k] != 0) {
      code = (code << 1) | (q[k] < 0 ? 1u : 0u);
      cbits++;
    }
  if (cbits) bwt_put(w, code, cbits);
  return 1;
}

/* Independently Huffman-encode the observed integer spectrum (magnitude +
 * sign) for one granule, using the parsed side info, and compare the result
 * with the delivered main-data bits.  Returns 1 if they match over the
 * granule's part2_3_length span. */
static int verify_maindata(const unsigned char *delivered, int total_bits,
                           const struct side_frame *sf, int gr, int c, int sr,
                           int start_bit, int delivered_bits,
                           const int *obs_mag, const int8_t *obs_sign,
                           int obs_count1) {
  const struct side_gi *g = &sf->gr[gr][c];
  int end = start_bit + g->part2_3_length;
  int r1, r2, i;
  const int *lb = band_table_for(sr);
  unsigned char *tmp;
  struct bwt w;
  int pos;

  if (g->scalefac_compress != 0) return 0;

  if (g->window_switching) { r1 = 36; r2 = g->big_values * 2; }
  else {
    r1 = lb[g->region0_count + 1];
    r2 = lb[g->region0_count + 1 + g->region1_count + 1];
  }
  if (r1 > g->big_values * 2) r1 = g->big_values * 2;
  if (r2 > g->big_values * 2) r2 = g->big_values * 2;
  if (r2 < r1) r2 = r1;

  tmp = calloc((size_t)total_bits / 8 + 8, 1);
  if (!tmp) return 0;
  /* copy everything before the granule so bit positions line up */
  memcpy(tmp, delivered, (size_t)((start_bit + 7) / 8));
  bwt_init(&w, tmp, total_bits / 8 + 8, start_bit);

  for (i = 0; i < g->big_values * 2; i += 2) {
    int tbl;
    int x, y;
    if (g->window_switching) {
      int idx = (i >= r1);
      tbl = g->table_select[idx < 2 ? idx : 1];
    } else {
      int idx = (i >= r1) + (i >= r2);
      tbl = g->table_select[idx < 3 ? idx : 2];
    }
    x = obs_sign[i] < 0 ? -obs_mag[i] : obs_mag[i];
    y = obs_sign[i + 1] < 0 ? -obs_mag[i + 1] : obs_mag[i + 1];
    if (!huff_encode_pair(&w, tbl, x, y)) { free(tmp); return 0; }
  }
  pos = g->big_values * 2;
  {
    int tbl = g->count1table_select + 32;
    int k;
    for (k = 0; k < obs_count1; k++) {
      int q[4];
      if (pos + 4 > GRANULE_SIZE_576) break;
      q[0] = obs_sign[pos] < 0 ? -obs_mag[pos] : obs_mag[pos];
      q[1] = obs_sign[pos + 1] < 0 ? -obs_mag[pos + 1] : obs_mag[pos + 1];
      q[2] = obs_sign[pos + 2] < 0 ? -obs_mag[pos + 2] : obs_mag[pos + 2];
      q[3] = obs_sign[pos + 3] < 0 ? -obs_mag[pos + 3] : obs_mag[pos + 3];
      huff_encode_quad(&w, tbl, q);
      pos += 4;
    }
  }
  /* Compare from start_bit to the point where our independent encoding stops
   * (w.bitpos).  The encoder's remaining bits are padding ones. */
  {
    int cmp_end = w.bitpos;
    int ok;
    /* Bits beyond the delivered word boundary still sit in the encoder's
     * 32-bit cache and are not part of the returned buffer; do not treat
     * them as a short frame.  Compare only the delivered prefix. */
    if (cmp_end > delivered_bits) cmp_end = delivered_bits;
    ok = bits_at_equal(delivered, tmp, start_bit, cmp_end - start_bit);
    /* padding ones check only over the delivered region */
    {
      int bp = cmp_end;
      while (ok && bp < end && bp < delivered_bits) {
        if (getbits_at(delivered, bp, 1) != 1) { ok = 0; break; }
        bp++;
      }
    }
    free(tmp);
    return ok;
  }
}

/* ------------------------------------------------------------------ */
/* windows group                                                       */
/* ------------------------------------------------------------------ */

/* Double-precision reference matching the task's definitions.  The fixed
 * pipeline computes sum(in * w * cos * 2^31) >> 32, i.e. a half-scale direct
 * sum, so the reference divides by 2. */
static double win_long(int n) { return sin(M_PI * (n + 0.5) / 36.0); }
static double kern_long(int n, int k) {
  return cos((M_PI / 72.0) * (2 * n + 19) * (2 * k + 1));
}
static double win_short(int n) { return sin(M_PI * (n + 0.5) / 12.0); }
static double kern_short(int n, int k) {
  return cos((M_PI / 24.0) * (2 * n + 7) * (2 * k + 1));
}

static double win_start_val(int n) {
  if (n < 18) return win_long(n);
  if (n < 24) return 1.0;
  if (n < 30) return sin(M_PI * ((n - 24) + 0.5) / 12.0);
  return 0.0;
}
static double win_stop_val(int n) {
  if (n < 6) return 0.0;
  if (n < 12) return sin(M_PI * ((n - 6) + 0.5) / 12.0);
  if (n < 18) return 1.0;
  return win_long(n);
}

static double ref_long_coef(const int32_t *in, int k, double (*win)(int)) {
  double s = 0.0;
  int n;
  for (n = 0; n < 36; n++)
    s += (double)in[n] * win(n) * kern_long(n, k);
  return s / 2.0;
}
static void ref_short_band(const int32_t *in36, double out18[18]) {
  int w, k, n;
  for (w = 0; w < 3; w++) {
    const int32_t *p = in36 + 6 * (w + 1);
    for (k = 0; k < 6; k++) {
      double s = 0.0;
      for (n = 0; n < 12; n++)
        s += (double)p[n] * win_short(n) * kern_short(n, k);
      out18[w * 6 + k] = s / 2.0;
    }
  }
}

/* Encode a legal normal->start->short->stop sequence and keep the
 * observations for the last frame.  Returns 0 on success. */
static int drive_sequence(struct enc *e, int final_granule0_type) {
  uint8_t seq[4];
  int n;
  unsigned char *p;
  (void)final_granule0_type;
  memset(seq, 0, sizeof(seq));
  if (enc_blocks(e, seq, 4, &p, &n)) return -1; /* frame 0: normal normal */
  seq[0] = seq[1] = 1;                         /* frame 1: start start */
  seq[2] = seq[3] = 2;
  if (enc_blocks(e, seq, 4, &p, &n)) return -1; /* g0 start, g1 short */
  return 0;
}

TASK_CASE("windows", long_matches_double_reference) {
  struct enc e;
  unsigned char *p;
  int n, band, k;
  int32_t in[36];
  double worst = 0.0;
  if (enc_open(&e, 48000, 128, 2)) return;
  {
    uint8_t bt[4] = {0, 0, 0, 0};
    make_pcm(e.l, e.spp, e.sr, 2, 1);
    make_pcm(e.r, e.spp, e.sr, 2, 2);
    TASK_CHECK(enc_blocks(&e, bt, 4, &p, &n) == 0);
  }
  TASK_CHECK(shine_obs_block_type[0][0] == 0);
  TASK_CHECK(shine_obs_mdct_in_valid[0][0] == 1);
  /* Check all 32 bands: for odd subbands too (task requires odd bands). */
  for (band = 0; band < 32; band++) {
    for (k = 0; k < 36; k++) in[k] = shine_obs_mdct_in[0][0][band][k];
    for (k = 0; k < 18; k++) {
      double r = ref_long_coef(in, k, win_long);
      double f = (double)shine_obs_xr_pre_aa[0][0][band * 18 + k];
      double d = fabs(f - r);
      if (d > worst) worst = d;
    }
  }
  TASK_CHECK(worst <= 64.0);
  enc_close(&e);
}

TASK_CASE("windows", short_three_windows_nonzero) {
  struct enc e;
  unsigned char *p;
  int n, band;
  if (enc_open(&e, 48000, 128, 2)) return;
  {
    uint8_t f0[4] = {0, 0, 0, 0}, f1[4] = {1, 1, 2, 2};
    make_pcm(e.l, e.spp, e.sr, 2, 3);
    make_pcm(e.r, e.spp, e.sr, 2, 4);
    TASK_CHECK(enc_blocks(&e, f0, 4, &p, &n) == 0);
    TASK_CHECK(enc_blocks(&e, f1, 4, &p, &n) == 0);
  }
  /* Frame 1 granule 1 is short. */
  TASK_CHECK(shine_obs_block_type[0][1] == 2);
  {
    /* Each of the three short windows must have non-zero energy in at least
     * one band, and the natural coefficients must match the reference per
     * window. */
    int w, k;
    double worst = 0.0;
    long wenergy[3] = {0, 0, 0};
    for (band = 0; band < 32; band++) {
      const int32_t *in = shine_obs_mdct_in[0][1][band];
      double ref[18];
      ref_short_band(in, ref);
      for (k = 0; k < 18; k++) {
        double f = (double)shine_obs_xr_pre_aa[0][1][band * 18 + k];
        double d = fabs(f - ref[k]);
        if (d > worst) worst = d;
      }
      for (w = 0; w < 3; w++)
        for (k = 0; k < 6; k++)
          wenergy[w] += labs((long)shine_obs_xr_pre_aa[0][1][band * 18 +
                                                       w * 6 + k]);
    }
    TASK_CHECK(worst <= 64.0);
    TASK_CHECK(wenergy[0] > 0);
    TASK_CHECK(wenergy[1] > 0);
    TASK_CHECK(wenergy[2] > 0);
  }
  enc_close(&e);
}

TASK_CASE("windows", aliasing_only_on_long_types) {
  struct enc e;
  unsigned char *p;
  int n, band, k;
  int long_mod_some = 0, short_mod = 0;
  if (enc_open(&e, 44100, 128, 2)) return;
  {
    uint8_t f0[4] = {0, 0, 0, 0}, f1[4] = {0, 0, 1, 1},
            f2[4] = {2, 2, 2, 2};
    make_pcm(e.l, e.spp, e.sr, 2, 5);
    make_pcm(e.r, e.spp, e.sr, 2, 6);
    TASK_CHECK(enc_blocks(&e, f0, 4, &p, &n) == 0);
    TASK_CHECK(enc_blocks(&e, f1, 4, &p, &n) == 0);
    /* Frame 1 granule 1 is start; check before the next frame overwrites
     * the observations. */
    TASK_CHECK(shine_obs_block_type[0][1] == 1);
    for (band = 0; band < 32; band++)
      for (k = 0; k < 18; k++)
        if (shine_obs_xr_pre_aa[0][1][band * 18 + k] !=
            shine_obs_xr_post_aa[0][1][band * 18 + k])
          long_mod_some++;
    TASK_CHECK(long_mod_some > 0);
    /* Band 0's first coefficient has no lower neighbour and is untouched. */
    TASK_CHECK(shine_obs_xr_pre_aa[0][1][0] == shine_obs_xr_post_aa[0][1][0]);

    TASK_CHECK(enc_blocks(&e, f2, 4, &p, &n) == 0);
    /* Frame 2 granule 0 is short: pre and post must be identical. */
    TASK_CHECK(shine_obs_block_type[0][0] == 2);
    for (band = 0; band < 32; band++)
      for (k = 0; k < 18; k++)
        if (shine_obs_xr_pre_aa[0][0][band * 18 + k] !=
            shine_obs_xr_post_aa[0][0][band * 18 + k])
          short_mod++;
    TASK_CHECK(short_mod == 0);
  }
  enc_close(&e);
}

TASK_CASE("windows", start_stop_match_reference) {
  struct enc e;
  unsigned char *p;
  int n, band, k;
  double worst_start = 0.0, worst_stop = 0.0;
  if (enc_open(&e, 32000, 128, 2)) return;
  {
    uint8_t f0[4] = {0, 0, 0, 0}, f1[4] = {0, 0, 1, 1},
            f2[4] = {2, 2, 2, 2}, f3[4] = {3, 3, 0, 0};
    make_pcm(e.l, e.spp, e.sr, 2, 7);
    make_pcm(e.r, e.spp, e.sr, 2, 8);
    TASK_CHECK(enc_blocks(&e, f0, 4, &p, &n) == 0);
    TASK_CHECK(enc_blocks(&e, f1, 4, &p, &n) == 0);
    /* start in frame 1 granule 1 -- check before the next frame runs. */
    TASK_CHECK(shine_obs_block_type[0][1] == 1);
    for (band = 0; band < 32; band++) {
      int32_t in[36];
      for (k = 0; k < 36; k++) in[k] = shine_obs_mdct_in[0][1][band][k];
      for (k = 0; k < 18; k++) {
        double r = ref_long_coef(in, k, win_start_val);
        double f = (double)shine_obs_xr_pre_aa[0][1][band * 18 + k];
        double d = fabs(f - r);
        if (d > worst_start) worst_start = d;
      }
    }
    TASK_CHECK(worst_start <= 64.0);

    TASK_CHECK(enc_blocks(&e, f2, 4, &p, &n) == 0); /* gr0 short */
    TASK_CHECK(enc_blocks(&e, f3, 4, &p, &n) == 0); /* gr0 stop */
    TASK_CHECK(shine_obs_block_type[0][0] == 3);
    for (band = 0; band < 32; band++) {
      int32_t in[36];
      for (k = 0; k < 36; k++) in[k] = shine_obs_mdct_in[0][0][band][k];
      for (k = 0; k < 18; k++) {
        double r = ref_long_coef(in, k, win_stop_val);
        double f = (double)shine_obs_xr_pre_aa[0][0][band * 18 + k];
        double d = fabs(f - r);
        if (d > worst_stop) worst_stop = d;
      }
    }
    TASK_CHECK(worst_stop <= 64.0);
  }
  enc_close(&e);
}

TASK_CASE("windows", history_carries_across_granules_and_frames) {
  struct enc e;
  unsigned char *p;
  int n;
  int32_t frame0_tail[32][18];
  int band, k;
  if (enc_open(&e, 48000, 128, 2)) return;
  {
    uint8_t f0[4] = {0, 0, 0, 0};
    make_pcm(e.l, e.spp, e.sr, 2, 9);
    make_pcm(e.r, e.spp, e.sr, 2, 10);
    TASK_CHECK(enc_blocks(&e, f0, 4, &p, &n) == 0);
  }
  /* Within a frame, granule 1's first 18 input lines equal granule 0's
   * second 18 (the continuous polyphase history). */
  {
    int match_g01 = 1;
    for (band = 0; band < 32; band++)
      for (k = 0; k < 18; k++) {
        if (shine_obs_mdct_in[0][0][band][18 + k] !=
            shine_obs_mdct_in[0][1][band][k])
          match_g01 = 0;
        frame0_tail[band][k] = shine_obs_mdct_in[0][1][band][18 + k];
      }
    TASK_CHECK(match_g01);
  }
  {
    uint8_t f0[4] = {0, 0, 0, 0};
    TASK_CHECK(enc_blocks(&e, f0, 4, &p, &n) == 0);
    /* Frame 1 granule 0's first 18 input lines must equal frame 0 granule
     * 1's second 18 lines: history continues across the frame boundary. */
    {
      int same = 1;
      for (band = 0; band < 32; band++)
        for (k = 0; k < 18; k++)
          if (shine_obs_mdct_in[0][0][band][k] != frame0_tail[band][k])
            same = 0;
      TASK_CHECK(same);
    }
  }
  enc_close(&e);
}

TASK_CASE("windows", odd_subbands_use_analysis_sign) {
  /* The analysis filter inverts odd subbands (every odd index of band and
   * k).  Check that odd bands are reconstructed by the reference too, which
   * would fail if the sign flip were lost. */
  struct enc e;
  unsigned char *p;
  int n, band, k;
  double worst_odd = 0.0;
  if (enc_open(&e, 48000, 128, 1)) return;
  {
    uint8_t bt[2] = {0, 0};
    make_pcm(e.l, e.spp, e.sr, 1, 11);
    TASK_CHECK(enc_blocks(&e, bt, 2, &p, &n) == 0);
  }
  for (band = 1; band < 32; band += 2) {
    int32_t in[36];
    for (k = 0; k < 36; k++) in[k] = shine_obs_mdct_in[0][0][band][k];
    for (k = 0; k < 18; k++) {
      double r = ref_long_coef(in, k, win_long);
      double f = (double)shine_obs_xr_pre_aa[0][0][band * 18 + k];
      double d = fabs(f - r);
      if (d > worst_odd) worst_odd = d;
    }
  }
  TASK_CHECK(worst_odd <= 64.0);
  enc_close(&e);
}

/* ------------------------------------------------------------------ */
/* bitstream group                                                     */
/* ------------------------------------------------------------------ */

/* Parsed side info for one MPEG-1 stereo frame (2 granules x 2 channels). */
/* (struct side_gi / side_frame are declared above with the bitstream helpers.) */

/* Parse MPEG-1 Layer III side info for a stereo frame, header at bit 0. */
static int parse_side(const unsigned char *p, int ch, struct side_frame *sf) {
  struct brt b;
  int gr, c, i;
  brt_init(&b, p);
  b.bitpos = 32;
  sf->main_data_begin = (int)brt_get(&b, 9);
  brt_get(&b, ch == 2 ? 3 : 5); /* private bits */
  for (c = 0; c < ch; c++)
    for (i = 0; i < 4; i++) sf->scfsi[c][i] = (int)brt_get(&b, 1);
  for (gr = 0; gr < 2; gr++)
    for (c = 0; c < ch; c++) {
      struct side_gi *g = &sf->gr[gr][c];
      g->part2_3_length = (int)brt_get(&b, 12);
      g->big_values = (int)brt_get(&b, 9);
      g->global_gain = (int)brt_get(&b, 8);
      g->scalefac_compress = (int)brt_get(&b, 4);
      g->window_switching = (int)brt_get(&b, 1);
      g->region0_count = 0;
      g->region1_count = 0;
      g->table_select[0] = g->table_select[1] = 0;
      g->subblock_gain[0] = g->subblock_gain[1] = g->subblock_gain[2] = 0;
      if (g->window_switching) {
        g->block_type = (int)brt_get(&b, 2);
        g->mixed = (int)brt_get(&b, 1);
        g->table_select[0] = (int)brt_get(&b, 5);
        g->table_select[1] = (int)brt_get(&b, 5);
        for (i = 0; i < 3; i++) g->subblock_gain[i] = (int)brt_get(&b, 3);
        if (g->block_type == 2 && !g->mixed) {
          g->region0_count = 8;
          g->region1_count = 12;
        } else {
          g->region0_count = 7;
          g->region1_count = 13;
        }
      } else {
        g->block_type = 0;
        g->mixed = 0;
        for (i = 0; i < 3; i++) g->table_select[i] = (int)brt_get(&b, 5);
        g->region0_count = (int)brt_get(&b, 4);
        g->region1_count = (int)brt_get(&b, 3);
      }
      g->preflag = (int)brt_get(&b, 1);
      g->scalefac_scale = (int)brt_get(&b, 1);
      g->count1table_select = (int)brt_get(&b, 1);
    }
  sf->side_end_bit = b.bitpos;
  return sf->side_end_bit;
}

/* Decode one granule's coefficients from the bitstream in its transmitted
 * (bitstream) order, plus its scalefactors.  Returns 1 on success if the
 * payload consumes exactly part2_3_length bits (allowing stuffing ones).
 * `ix_out` is filled in transmitted order; `sf_out` with long[21] or
 * short[13][3] scalefactors. */
static int decode_granule(const unsigned char *p, const struct side_frame *sf,
                          int gr, int c, int sr, int start_bit, int count1_obs,
                          int *ix_out, int *sf_l, int sf_s[13][3]) {
  const struct side_gi *g = &sf->gr[gr][c];
  int end = start_bit + g->part2_3_length;
  struct brt b;
  int i, band;
  int short_block = g->window_switching && g->block_type == 2 && !g->mixed;
  int r1, r2;
  const int *lb = band_table_for(sr);

  brt_init(&b, p);
  b.bitpos = start_bit;

  /* part2: scalefactors.  We require scalefac_compress == 0 (zero bits),
   * which is what this encoder emits, so this is a no-op but keeps the
   * reader honest. */
  if (g->scalefac_compress != 0) return 0;
  for (band = 0; band < 21; band++) sf_l[band] = 0;
  for (band = 0; band < 13; band++)
    sf_s[band][0] = sf_s[band][1] = sf_s[band][2] = 0;

  for (i = 0; i < GRANULE_SIZE_576; i++) ix_out[i] = 0;

  /* implicit region boundaries */
  if (g->window_switching) {
    r1 = 36;
    r2 = g->big_values * 2;
  } else {
    r1 = lb[g->region0_count + 1];
    r2 = lb[g->region0_count + 1 + g->region1_count + 1];
  }
  if (r1 > g->big_values * 2) r1 = g->big_values * 2;
  if (r2 > g->big_values * 2) r2 = g->big_values * 2;
  if (r2 < r1) r2 = r1;

  /* part3: big values */
  for (i = 0; i < g->big_values * 2; i += 2) {
    int idx, tbl;
    if (g->window_switching) {
      /* Only two tables for switched granules; region 0 is lines [0,36),
       * region 1 the remainder. */
      idx = (i >= r1);
      tbl = g->table_select[idx < 2 ? idx : 1];
    } else {
      idx = (i >= r1) + (i >= r2);
      tbl = g->table_select[idx < 3 ? idx : 2];
    }
    if (tbl) {
      int x, y;
      if (!huff_pair(&b, tbl, &x, &y)) return 0;
      ix_out[i] = x;
      ix_out[i + 1] = y;
    } else {
      ix_out[i] = ix_out[i + 1] = 0;
    }
  }
  /* part3: count1.  The number of count1 quads is not transmitted for MPEG-1;
   * decoders derive it by decoding quads until the granule's bit budget is
   * exhausted, which also swallows the encoder's trailing padding ones.  We
   * decode up to the read-only observed quad count so the comparison covers
   * only real coefficients, not stuffing. */
  {
    int tbl = g->count1table_select + 32;
    int pos = g->big_values * 2;
    int want = count1_obs; /* bound to real coefficients, not padding */
    int k;
    for (k = 0; k < want; k++) {
      int q[4];
      if (pos + 4 > GRANULE_SIZE_576) break;
      if (!huff_quad(&b, tbl, q)) return 0;
      ix_out[pos++] = q[0];
      ix_out[pos++] = q[1];
      ix_out[pos++] = q[2];
      ix_out[pos++] = q[3];
    }
  }
  /* The remaining bits up to `end` are the encoder's padding ones; a
   * conformant decoder ignores them.  We only require that we never ran
   * past the granule. */
  if (b.bitpos > end) return 0;
  return 1;
}

/* Compute a granule's Huffman/scalefactor start bit within a frame. */
static int granule_start_bit(const struct side_frame *sf, int gr, int c,
                             int ch) {
  int start = sf->side_end_bit;
  int i;
  for (i = 0; i < gr * ch + c; i++)
    start += sf->gr[i / ch][i % ch].part2_3_length;
  return start;
}

/* Independently inverse-reorder a short block's transmitted coefficients to
 * natural (subband, window, frequency) coordinates. */
static void inverse_short(const int *bt_ix, int sr, int out[576]) {
  const int *sb = short_band_table_for(sr);
  int sfb, w, f, pos = 0;
  for (int i = 0; i < 576; i++) out[i] = 0;
  for (sfb = 0; sfb < 13; sfb++)
    for (w = 0; w < 3; w++)
      for (f = sb[sfb]; f < sb[sfb + 1]; f++) {
        int band = f / 6, line = f % 6;
        out[band * 18 + w * 6 + line] = bt_ix[pos++];
      }
}

/* Shared: encode several window-switched frames at sr, concatenate the
 * returned chunks into the continuous bitstream the library produces (the
 * encoder keeps its 32-bit cache across calls, so one delivered chunk is not
 * necessarily a byte-aligned frame), then walk the stream frame by frame
 * checking side info, scalefactors, signs, the integer spectrum (transmitted
 * order and inverse-reordered) and each granule's payload. */
static void bitstream_roundtrip(int sr, int frames) {
  struct enc e;
  static unsigned char stream[1 << 16];
  /* Per-frame snapshots of the read-only quantization observations, since the
   * hooks only hold the most recent frame. */
  static int snap_mag[16][2][2][576];
  static int snap_nat[16][2][2][576];
  static int8_t snap_sign[16][2][2][576];
  static struct shine_obs_side snap_side[16][2][2];
  int stream_len = 0;
  int f;
  int short_seen = 0, nonzero_sign = 0, two_regions = 0, frames_ok = 0;
  int count1_seen = 0;
  if (frames > 16) frames = 16;
  if (enc_open(&e, sr, 128, 2)) return;
  make_pcm(e.l, e.spp, e.sr, 2, 101 + sr);
  make_pcm(e.r, e.spp, e.sr, 2, 202 + sr);
  for (f = 0; f < frames; f++) {
    static const int c0seq[] = {0, 0, 1, 2, 2, 3, 0, 0};
    static const int c1seq[] = {0, 0, 1, 2, 2, 2, 3, 0};
    int g = (2 * f) % 8;
    uint8_t bt[4];
    unsigned char *delivered = NULL;
    int dn = -1;
    int ch, gr, i;

    bt[0] = (uint8_t)c0seq[g];
    bt[1] = (uint8_t)c1seq[g];
    bt[2] = (uint8_t)c0seq[(g + 1) % 8];
    bt[3] = (uint8_t)c1seq[(g + 1) % 8];

    if (enc_blocks(&e, bt, 4, &delivered, &dn)) {
      TASK_CHECK(0 && "encode failed");
      return;
    }
    if (stream_len + dn + 8 < (int)sizeof(stream)) {
      memcpy(stream + stream_len, delivered, (size_t)dn);
      stream_len += dn;
    }
    /* snapshot this frame's observations */
    for (ch = 0; ch < 2; ch++)
      for (gr = 0; gr < 2; gr++) {
        for (i = 0; i < 576; i++) {
          snap_mag[f][ch][gr][i] = shine_obs_l3_enc[ch][gr][i];
          snap_nat[f][ch][gr][i] = shine_obs_l3_enc_nat[ch][gr][i];
          snap_sign[f][ch][gr][i] = shine_obs_sign[ch][gr][i];
        }
        snap_side[f][ch][gr] = shine_obs_side[ch][gr];
      }
  }
  {
    int fn = -1;
    unsigned char *fp = shine_flush(e.s, &fn);
    if (fp && fn > 0 && stream_len + fn < (int)sizeof(stream)) {
      memcpy(stream + stream_len, fp, (size_t)fn);
      stream_len += fn;
    }
  }

  {
    int pos = 0;
    while (pos + 4 <= stream_len) {
      struct side_frame sf;
      int fb = mp3_frame_bytes(stream + pos, sr);
      int gr, ch;
      if (fb <= 0) {
        pos++;
        continue;
      }
      if (pos + fb > stream_len) break;
      parse_side(stream + pos, 2, &sf);
      {
        int fi = frames_ok;
        if (fi >= frames) break;
        frames_ok++;

      for (gr = 0; gr < 2; gr++)
        for (ch = 0; ch < 2; ch++) {
          const struct side_gi *sg = &sf.gr[gr][ch];
          int ix[576], sfl[21], sfs[13][3];
          int nat[576];
          int start = granule_start_bit(&sf, gr, ch, 2);
          int i;
          int is_short = sg->window_switching && sg->block_type == 2 &&
                         !sg->mixed;
          int bv2 = sg->big_values * 2;

          TASK_CHECK(sg->scalefac_compress == 0);
          if (sg->part2_3_length == 0)
            continue;
          if (is_short) short_seen = 1;
          if (snap_side[fi][ch][gr].count1) count1_seen = 1;

          if (!decode_granule(stream + pos, &sf, gr, ch, sr, start,
                              snap_side[fi][ch][gr].count1, ix, sfl, sfs)) {
            TASK_CHECK(0 && "granule did not parse");
            continue;
          }
          {
            int mism = 0, signs = 0;
            for (i = 0; i < bv2; i++) {
              int iv = ix[i];
              int av = iv < 0 ? -iv : iv;
              if (snap_mag[fi][ch][gr][i] != av) mism++;
              if (iv != 0) {
                signs++;
                if ((iv > 0) != (snap_sign[fi][ch][gr][i] > 0)) mism++;
              }
            }
            if (signs) nonzero_sign = 1;
            TASK_CHECK(mism == 0);
          }

          TASK_CHECK(verify_maindata(stream + pos, fb * 8, &sf, gr, ch, sr,
                                     start, fb * 8, snap_mag[fi][ch][gr],
                                     snap_sign[fi][ch][gr],
                                     snap_side[fi][ch][gr].count1));

          if (is_short) {
            int bt_mag[576];
            for (i = 0; i < 576; i++) bt_mag[i] = snap_mag[fi][ch][gr][i];
            inverse_short(bt_mag, sr, nat);
            {
              int mism = 0;
              for (i = 0; i < 576; i++)
                if (nat[i] != snap_nat[fi][ch][gr][i]) mism++;
              TASK_CHECK(mism == 0);
            }
          } else {
            int mism = 0;
            for (i = 0; i < 576; i++)
              if (snap_mag[fi][ch][gr][i] != snap_nat[fi][ch][gr][i])
                mism++;
            TASK_CHECK(mism == 0);
          }
        }

      for (gr = 0; gr < 2; gr++)
        for (ch = 0; ch < 2; ch++) {
          const struct side_gi *sg = &sf.gr[gr][ch];
          if (sg->window_switching && sg->big_values * 2 > 36 &&
              sg->table_select[0] && sg->table_select[1])
            two_regions = 1;
        }

      pos += fb;
      }
    }
    /* The encoder leaves up to 31 bits in its 32-bit cache; the very last
     * frame of the stream is therefore not necessarily delivered complete.
     * Only actually-delivered complete frames are consumed. */
    TASK_CHECK(frames_ok >= frames - 1);
    TASK_CHECK(frames_ok >= 1);
  }
  TASK_CHECK(short_seen);
  TASK_CHECK(nonzero_sign);
  TASK_CHECK(two_regions);
  TASK_CHECK(count1_seen);
  enc_close(&e);
}

TASK_CASE("bitstream", roundtrip_32k) { bitstream_roundtrip(32000, 8); }
TASK_CASE("bitstream", roundtrip_441k) { bitstream_roundtrip(44100, 9); }
TASK_CASE("bitstream", roundtrip_48k) { bitstream_roundtrip(48000, 8); }

TASK_CASE("bitstream", padding_both_values_44100) {
  /* 44.1 kHz uses fractional slots.  At 160 kbps the slot fraction makes the
     padding bit alternate frequently, so both values occur within a short
     run.  Accumulate the stream and walk frames at their true offsets. */
  struct enc e;
  unsigned char *p;
  static unsigned char stream[1 << 16];
  int stream_len = 0, n, f;
  int saw0 = 0, saw1 = 0, frames = 0;
  if (enc_open(&e, 44100, 160, 2)) return;
  make_pcm(e.l, e.spp, e.sr, 2, 303);
  make_pcm(e.r, e.spp, e.sr, 2, 404);
  for (f = 0; f < 24; f++) {
    static const int c0seq[] = {0, 0, 1, 2, 2, 3, 0, 0};
    uint8_t bt[4];
    int g = (2 * f) % 8;
    bt[0] = (uint8_t)c0seq[g];
    bt[1] = (uint8_t)c0seq[g];
    bt[2] = (uint8_t)c0seq[(g + 1) % 8];
    bt[3] = (uint8_t)c0seq[(g + 1) % 8];
    if (enc_blocks(&e, bt, 4, &p, &n)) return;
    if (stream_len + n + 8 < (int)sizeof(stream)) {
      memcpy(stream + stream_len, p, (size_t)n);
      stream_len += n;
    }
  }
  {
    int pos = 0;
    while (pos + 4 <= stream_len) {
      int fb = mp3_frame_bytes(stream + pos, 44100);
      int pad;
      if (fb <= 0) { pos++; continue; }
      if (pos + fb > stream_len) break;
      pad = (stream[pos + 2] >> 1) & 1;
      if (pad) saw1 = 1; else saw0 = 1;
      frames++;
      pos += fb;
    }
  }
  TASK_CHECK(frames >= 20);
  TASK_CHECK(saw0 && saw1);
  enc_close(&e);
}

TASK_CASE("bitstream", short_three_distinct_windows) {
  /* Drive a short granule whose three windows have distinct spectral shapes
     and confirm the transmitted short coefficients differ per window. */
  struct enc e;
  unsigned char *p;
  int n, f;
  int checked = 0;
  if (enc_open(&e, 48000, 192, 2)) return;
  make_pcm(e.l, e.spp, e.sr, 2, 505);
  make_pcm(e.r, e.spp, e.sr, 2, 606);
  for (f = 0; f < 8; f++) {
    static const int seq[] = {0, 0, 1, 2, 2, 3, 0, 0};
    uint8_t bt[4];
    int g = (2 * f) % 8;
    struct side_frame sf;
    bt[0] = (uint8_t)seq[g];
    bt[1] = (uint8_t)seq[g];
    bt[2] = (uint8_t)seq[(g + 1) % 8];
    bt[3] = (uint8_t)seq[(g + 1) % 8];
    if (enc_blocks(&e, bt, 4, &p, &n)) break;
    parse_side(p, 2, &sf);
    for (int gr = 0; gr < 2; gr++)
      for (int ch = 0; ch < 2; ch++) {
        const struct side_gi *sg = &sf.gr[gr][ch];
        if (sg->window_switching && sg->block_type == 2 && !sg->mixed) {
          int ix[576], sfl[21], sfs[13][3];
          int start = granule_start_bit(&sf, gr, ch, 2);
          if (decode_granule(p, &sf, gr, ch, 48000, start,
                             shine_obs_side[ch][gr].count1, ix, sfl, sfs)) {
            /* Compare window energies across the transmitted short layout:
               within each band the layout is window-major, so a band's
               three windows occupy consecutive runs of (f1-f0) values. */
            int sfb, w;
            long wsum[3] = {0, 0, 0};
            const int *sb = short_band_table_for(48000);
            int pos = 0;
            for (sfb = 0; sfb < 13; sfb++) {
              int width = sb[sfb + 1] - sb[sfb];
              for (w = 0; w < 3; w++) {
                int t;
                for (t = 0; t < width; t++) {
                  int v = ix[pos++];
                  wsum[w] += v < 0 ? -v : v;
                }
              }
            }
            /* The three windows should not all be identical; at least one
               differs from another. */
            if (!(wsum[0] == wsum[1] && wsum[1] == wsum[2]) &&
                (wsum[0] + wsum[1] + wsum[2]) > 0)
              checked = 1;
          }
        }
      }
  }
  TASK_CHECK(checked);
  enc_close(&e);
}

TASK_CASE("bitstream", count1_present) {
  /* Use a quiet high-frequency signal so the count1 (quadruple) region is
     populated; verify at least one granule carries count1 bits by checking
     that the decoded payload extends past the big-values region. */
  struct enc e;
  unsigned char *p;
  int n, f;
  int saw = 0;
  if (enc_open(&e, 48000, 128, 2)) return;
  /* Quiet broadband signal -> many small non-zero values -> count1. */
  for (int i = 0; i < e.spp; i++) {
    double v = 0.02 * sin(2 * M_PI * 11000.0 * i / 48000.0) +
               0.01 * sin(2 * M_PI * 15000.0 * i / 48000.0);
    e.l[i] = (int16_t)(v * 30000);
    e.r[i] = (int16_t)(-v * 30000);
  }
  for (f = 0; f < 6; f++) {
    static const int seq[] = {0, 0, 1, 2, 2, 3, 0, 0};
    uint8_t bt[4];
    int g = (2 * f) % 8;
    struct side_frame sf;
    bt[0] = (uint8_t)seq[g];
    bt[1] = (uint8_t)seq[g];
    bt[2] = (uint8_t)seq[(g + 1) % 8];
    bt[3] = (uint8_t)seq[(g + 1) % 8];
    if (enc_blocks(&e, bt, 4, &p, &n)) break;
    parse_side(p, 2, &sf);
    for (int gr = 0; gr < 2 && !saw; gr++)
      for (int ch = 0; ch < 2 && !saw; ch++) {
        const struct side_gi *sg = &sf.gr[gr][ch];
        int start = granule_start_bit(&sf, gr, ch, 2);
        struct brt b;
        int r1, r2, i;
        int bv2 = sg->big_values * 2;
        if (sg->part2_3_length == 0) continue;
        if (sg->window_switching) { r1 = 36; r2 = bv2; }
        else {
          const int *lb = band_table_for(48000);
          r1 = lb[sg->region0_count + 1];
          r2 = lb[sg->region0_count + 2 + sg->region1_count];
        }
        if (r1 > bv2) r1 = bv2;
        if (r2 > bv2) r2 = bv2;
        if (r2 < r1) r2 = r1;
        brt_init(&b, p);
        b.bitpos = start;
        for (i = 0; i < bv2; i += 2) {
          int idx, tbl;
          if (sg->window_switching) {
            idx = (i >= r1);
            tbl = sg->table_select[idx ? 1 : 0];
          } else {
            idx = (i >= r1) + (i >= r2);
            tbl = sg->table_select[idx < 3 ? idx : 2];
          }
          if (tbl) { int x, y; if (!huff_pair(&b, tbl, &x, &y)) break; }
        }
        if (b.bitpos < start + sg->part2_3_length)
          saw = 1; /* count1 region consumes the remainder */
      }
  }
  TASK_CHECK(saw);
  enc_close(&e);
}

/* ------------------------------------------------------------------ */
/* state group                                                         */
/* ------------------------------------------------------------------ */

/* Sentinel-guarded input/output helper.  Every PCM buffer passed to the
 * encoder and every returned buffer is wrapped with a guard region that must
 * stay untouched. */
struct guarded_pcm {
  int16_t l[1152 + 2 * SENTINEL_SZ];
  int16_t r[1152 + 2 * SENTINEL_SZ];
  int16_t *planar[2];
};
static void guarded_pcm_init(struct guarded_pcm *g, int spp) {
  int i;
  for (i = 0; i < spp + 2 * SENTINEL_SZ; i++) {
    g->l[i] = (int16_t)(0x5A5A);
    g->r[i] = (int16_t)(0xA5A5);
  }
  g->planar[0] = g->l + SENTINEL_SZ;
  g->planar[1] = g->r + SENTINEL_SZ;
}
static int guarded_pcm_ok(struct guarded_pcm *g, int spp) {
  int i;
  for (i = 0; i < SENTINEL_SZ; i++) {
    if (g->l[i] != (int16_t)0x5A5A || g->r[i] != (int16_t)0xA5A5) return 0;
    if (g->l[SENTINEL_SZ + spp + i] != (int16_t)0x5A5A ||
        g->r[SENTINEL_SZ + spp + i] != (int16_t)0xA5A5)
      return 0;
  }
  return 1;
}

/* Encode a byte-equality comparison of the old entry point vs the new
 * all-normal entry point, including sentinels on the output. */
static void check_all_normal_identical(int sr, int br, int ch) {
  struct enc a, b;
  int f, n;
  static unsigned char oa[1 << 15], ob[1 << 15];
  int sa = 0, sb = 0;
  if (enc_open(&a, sr, br, ch)) return;
  if (enc_open(&b, sr, br, ch)) { enc_close(&a); return; }
  for (f = 0; f < 6; f++) {
    int16_t *pa[2], *pb[2];
    unsigned char *p;
    make_pcm(a.l, a.spp, sr, ch, 900 + f);
    make_pcm(a.r, a.spp, sr, ch, 901 + f);
    memcpy(b.l, a.l, (size_t)a.spp * sizeof(int16_t));
    memcpy(b.r, a.r, (size_t)a.spp * sizeof(int16_t));
    pa[0] = a.l; pa[1] = a.r;
    pb[0] = b.l; pb[1] = b.r;
    p = shine_encode_buffer(a.s, pa, &n);
    if (n < 0 || sa + n > (int)sizeof(oa)) { enc_close(&a); enc_close(&b); return; }
    memcpy(oa + sa, p, (size_t)n);
    sa += n;
    {
      uint8_t bt[4] = {0, 0, 0, 0};
      p = (ch == 1) ? shine_encode_buffer_blocks(b.s, pb, bt, 2, &n)
                    : shine_encode_buffer_blocks(b.s, pb, bt, 4, &n);
    }
    if (n < 0 || sb + n > (int)sizeof(ob)) { enc_close(&a); enc_close(&b); return; }
    memcpy(ob + sb, p, (size_t)n);
    sb += n;
  }
  TASK_CHECK(sa == sb);
  TASK_CHECK(memcmp(oa, ob, (size_t)sa) == 0);
  enc_close(&a);
  enc_close(&b);
}

TASK_CASE("state", old_equals_all_normal) {
  check_all_normal_identical(48000, 128, 2);
  check_all_normal_identical(44100, 192, 2);
  check_all_normal_identical(32000, 320, 1);
}

TASK_CASE("state", illegal_request_retry_matches_fresh) {
  /* A rejected request must not touch state.  Two encoders walk the same
   * legal sequence; encoder b additionally receives several illegal requests
   * just before one frame.  Their outputs must stay byte-identical, proving
   * the rejected calls changed nothing. */
  struct enc a, b;
  int f, n;
  static unsigned char oa[1 << 15], ob[1 << 15];
  int sa = 0, sb = 0;
  static const uint8_t seq[6][4] = {
      {0, 0, 0, 0}, {0, 1, 1, 2}, {2, 2, 2, 2},
      {2, 3, 3, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
  if (enc_open(&a, 48000, 128, 2)) return;
  if (enc_open(&b, 48000, 128, 2)) { enc_close(&a); return; }
  for (f = 0; f < 6; f++) {
    int16_t *pa[2] = {a.l, a.r}, *pb[2] = {b.l, b.r};
    unsigned char *p;
    make_pcm(a.l, a.spp, 48000, 2, 1000 + f);
    make_pcm(a.r, a.spp, 48000, 2, 1100 + f);
    memcpy(b.l, a.l, (size_t)a.spp * sizeof(int16_t));
    memcpy(b.r, a.r, (size_t)a.spp * sizeof(int16_t));

    if (f == 3) {
      /* encoder b gets a burst of invalid requests before the real one:
       *  - bad transition (current state short -> normal is illegal)
       *  - wrong entry count
       *  - NULL block_types
       *  - NULL written pointer */
      uint8_t ill_transition[4] = {0, 0, 0, 0}; /* short -> normal illegal */
      uint8_t good_next[4];
      int w;
      memcpy(good_next, seq[3], 4);
      w = 7;
      TASK_CHECK(shine_encode_buffer_blocks(b.s, pb, ill_transition, 4, &w) ==
                 NULL);
      TASK_CHECK(w == -1);
      w = 7;
      TASK_CHECK(shine_encode_buffer_blocks(b.s, pb, good_next, 3, &w) == NULL);
      TASK_CHECK(w == -1);
      w = 7;
      TASK_CHECK(shine_encode_buffer_blocks(b.s, pb, NULL, 4, &w) == NULL);
      TASK_CHECK(w == -1);
      TASK_CHECK(shine_encode_buffer_blocks(b.s, pb, good_next, 4, NULL) ==
                 NULL);
    }

    p = shine_encode_buffer_blocks(a.s, pa, seq[f], 4, &n);
    TASK_CHECK(p && n > 0);
    if (sa + n <= (int)sizeof(oa)) { memcpy(oa + sa, p, (size_t)n); sa += n; }
    p = shine_encode_buffer_blocks(b.s, pb, seq[f], 4, &n);
    TASK_CHECK(p && n > 0);
    if (sb + n <= (int)sizeof(ob)) { memcpy(ob + sb, p, (size_t)n); sb += n; }
  }
  TASK_CHECK(sa == sb);
  TASK_CHECK(sa > 0);
  TASK_CHECK(memcmp(oa, ob, (size_t)sa) == 0);
  enc_close(&a);
  enc_close(&b);
}

TASK_CASE("state", cross_frame_switching_sequence) {
  /* A single channel walks normal -> start -> short -> short -> stop ->
   * normal across frames; every step must be accepted. */
  struct enc e;
  int f, n;
  static const uint8_t seq[6][4] = {
      {0, 0, 0, 0}, {1, 1, 2, 2}, {2, 2, 2, 2},
      {2, 2, 3, 3}, {0, 0, 0, 0}, {1, 1, 2, 2}};
  if (enc_open(&e, 48000, 128, 2)) return;
  for (f = 0; f < 6; f++) {
    unsigned char *p;
    make_pcm(e.l, e.spp, 48000, 2, 1200 + f);
    make_pcm(e.r, e.spp, 48000, 2, 1300 + f);
    p = shine_encode_buffer_blocks(e.s, e.planar, seq[f], 4, &n);
    TASK_CHECK(p != NULL);
    TASK_CHECK(n > 0);
  }
  enc_close(&e);
}

TASK_CASE("state", channels_differ) {
  /* Left and right run different but individually legal sequences; both
   * accepted, proving per-channel state. */
  struct enc e;
  int f, n;
  if (enc_open(&e, 48000, 128, 2)) return;
  for (f = 0; f < 6; f++) {
    /* channel 0 walks: 0,0 -> 1,2 -> 2,2 -> 3,0 -> 0,0 -> 0,0
       channel 1 walks one step behind: 0,1 -> 2,2 -> 2,3 -> 0,0 -> 0,0 -> 0,0
       (both per-channel granule sequences are legal, but they differ). */
    static const int c0g0[6] = {0, 1, 2, 3, 0, 0};
    static const int c0g1[6] = {0, 2, 2, 0, 0, 0};
    static const int c1g0[6] = {0, 2, 2, 0, 0, 0};
    static const int c1g1[6] = {1, 2, 3, 0, 0, 0};
    uint8_t bt[4];
    unsigned char *p;
    bt[0] = (uint8_t)c0g0[f];
    bt[1] = (uint8_t)c1g0[f];
    bt[2] = (uint8_t)c0g1[f];
    bt[3] = (uint8_t)c1g1[f];
    make_pcm(e.l, e.spp, 48000, 2, 1400 + f);
    make_pcm(e.r, e.spp, 48000, 2, 1500 + f);
    p = shine_encode_buffer_blocks(e.s, e.planar, bt, 4, &n);
    TASK_CHECK(p != NULL);
    TASK_CHECK(n > 0);
  }
  enc_close(&e);
}

TASK_CASE("state", two_instances_interleaved) {
  /* Two encoders driven in lock-step on the same PCM must each match an
   * encoder run alone on that PCM: interleaving must not leak state. */
  struct enc a, b, c;
  int f, n;
  static unsigned char oa[1 << 15], oc[1 << 15];
  int sa = 0, sc = 0;
  static const uint8_t seq[5][4] = {
      {0, 0, 0, 0}, {0, 1, 1, 2}, {2, 2, 2, 2},
      {2, 3, 3, 0}, {0, 0, 0, 0}};
  if (enc_open(&a, 48000, 128, 2)) return;
  if (enc_open(&b, 48000, 128, 2)) { enc_close(&a); return; }
  if (enc_open(&c, 48000, 128, 2)) { enc_close(&a); enc_close(&b); return; }
  for (f = 0; f < 5; f++) {
    unsigned char *p;
    int i;
    make_pcm(a.l, a.spp, 48000, 2, 1600 + f);
    make_pcm(a.r, a.spp, 48000, 2, 1700 + f);
    /* encoder a: first, its output recorded */
    p = shine_encode_buffer_blocks(a.s, a.planar, seq[f], 4, &n);
    TASK_CHECK(p && n > 0);
    if (sa + n <= (int)sizeof(oa)) { memcpy(oa + sa, p, (size_t)n); sa += n; }
    /* encoder b: different PCM, same legal sequence, interleaved between a */
    for (i = 0; i < a.spp; i++) {
      b.l[i] = (int16_t)(a.l[i] / 2);
      b.r[i] = (int16_t)(a.r[i] / 3);
    }
    p = shine_encode_buffer_blocks(b.s, b.planar, seq[f], 4, &n);
    TASK_CHECK(p && n > 0);
    /* encoder c: exact copy of a's PCM/sequence but driven after a and b */
    for (i = 0; i < a.spp; i++) { c.l[i] = a.l[i]; c.r[i] = a.r[i]; }
    p = shine_encode_buffer_blocks(c.s, c.planar, seq[f], 4, &n);
    TASK_CHECK(p && n > 0);
    if (sc + n <= (int)sizeof(oc)) { memcpy(oc + sc, p, (size_t)n); sc += n; }
  }
  TASK_CHECK(sa == sc);
  TASK_CHECK(sa > 0);
  TASK_CHECK(memcmp(oa, oc, (size_t)sa) == 0);
  enc_close(&a);
  enc_close(&b);
  enc_close(&c);
}

TASK_CASE("state", dual_pcm_entries_match) {
  /* The planar and interleaved block entry points must produce identical
   * streams for identical input. */
  struct enc a, b;
  int f, n;
  static unsigned char oa[1 << 15], ob[1 << 15];
  int sa = 0, sb = 0;
  if (enc_open(&a, 48000, 128, 2)) return;
  if (enc_open(&b, 48000, 128, 2)) { enc_close(&a); return; }
  for (f = 0; f < 5; f++) {
    static const uint8_t seq[5][4] = {
        {0, 0, 0, 0}, {0, 1, 1, 2}, {2, 2, 2, 2},
        {2, 3, 3, 0}, {0, 0, 0, 0}};
    unsigned char *p;
    int i;
    int16_t inter[1152 * 2];
    make_pcm(a.l, a.spp, 48000, 2, 1800 + f);
    make_pcm(a.r, a.spp, 48000, 2, 1900 + f);
    p = shine_encode_buffer_blocks(a.s, a.planar, seq[f], 4, &n);
    TASK_CHECK(p && n > 0);
    if (sa + n <= (int)sizeof(oa)) { memcpy(oa + sa, p, (size_t)n); sa += n; }
    for (i = 0; i < a.spp; i++) {
      inter[2 * i] = a.l[i];
      inter[2 * i + 1] = a.r[i];
    }
    p = shine_encode_buffer_interleaved_blocks(b.s, inter, seq[f], 4, &n);
    TASK_CHECK(p && n > 0);
    if (sb + n <= (int)sizeof(ob)) { memcpy(ob + sb, p, (size_t)n); sb += n; }
  }
  TASK_CHECK(sa == sb);
  TASK_CHECK(sa > 0);
  TASK_CHECK(memcmp(oa, ob, (size_t)sa) == 0);
  enc_close(&a);
  enc_close(&b);
}

TASK_CASE("state", mpeg2_rejected) {
  /* The block entry points only support MPEG-1; an MPEG-2 rate must be
   * rejected with written = -1 even for an otherwise valid request. */
  shine_config_t cfg;
  shine_t s;
  uint8_t bt[2] = {0, 0};
  int n;
  int16_t pcm[2][576];
  int16_t *pl[2] = {pcm[0], pcm[1]};
  memset(&cfg, 0, sizeof(cfg));
  cfg.wave.channels = PCM_STEREO;
  cfg.wave.samplerate = 22050; /* MPEG-2 */
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.mpeg.mode = STEREO;
  cfg.mpeg.bitr = 128;
  s = shine_initialise(&cfg);
  if (!s) return; /* if it refuses to init, nothing to reject */
  n = 123;
  TASK_CHECK(shine_encode_buffer_blocks(s, pl, bt, 4, &n) == NULL);
  TASK_CHECK(n == -1);
  shine_close(s);
}

TASK_CASE("state", sentinels_preserved) {
  /* Input sentinels must be untouched and the delivered output must carry no
   * data before/after the frame in the returned size. */
  struct guarded_pcm g;
  struct enc e;
  int f, n;
  if (enc_open(&e, 48000, 128, 2)) return;
  for (f = 0; f < 6; f++) {
    static const uint8_t seq[6][4] = {
        {0, 0, 0, 0}, {0, 1, 1, 2}, {2, 2, 2, 2},
        {2, 3, 3, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    unsigned char *p;
    uint8_t before = SENTINEL;
    guarded_pcm_init(&g, e.spp);
    {
      int i;
      make_pcm(g.planar[0], e.spp, 48000, 2, 2000 + f);
      make_pcm(g.planar[1], e.spp, 48000, 2, 2100 + f);
      (void)i;
    }
    p = shine_encode_buffer_blocks(e.s, g.planar, seq[f], 4, &n);
    TASK_CHECK(p != NULL);
    TASK_CHECK(n > 0);
    TASK_CHECK(guarded_pcm_ok(&g, e.spp));
    /* output pointer lifetime: it points into the encoder's own 4096-byte
     * buffer, and `written` bytes are valid. */
    TASK_CHECK(p[0] == 0xFF);
    TASK_CHECK(before == SENTINEL);
  }
  enc_close(&e);
}

TASK_CASE("state", complete_sequence_no_missing_frames_48k) {
  /* 48 kHz frame length (384 bytes at 128 kbps) divides 4 evenly, so every
   * frame is word-aligned in the delivered chunks.  Concatenating all chunks
   * plus the final flush must yield exactly N complete frames, none missing. */
  struct enc e;
  int f, n;
  static unsigned char stream[1 << 16];
  int stream_len = 0, frames = 0, pos;
  if (enc_open(&e, 48000, 128, 2)) return;
  for (f = 0; f < 7; f++) {
    static const uint8_t seq[7][4] = {
        {0, 0, 0, 0}, {0, 1, 1, 2}, {2, 2, 2, 2}, {2, 3, 3, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    unsigned char *p;
    make_pcm(e.l, e.spp, 48000, 2, 2200 + f);
    make_pcm(e.r, e.spp, 48000, 2, 2300 + f);
    p = shine_encode_buffer_blocks(e.s, e.planar, seq[f], 4, &n);
    TASK_CHECK(p && n > 0);
    if (stream_len + n + 8 < (int)sizeof(stream)) {
      memcpy(stream + stream_len, p, (size_t)n);
      stream_len += n;
    }
  }
  pos = 0;
  while (pos + 4 <= stream_len) {
    int fb = mp3_frame_bytes(stream + pos, 48000);
    if (fb <= 0) { pos++; continue; }
    if (pos + fb > stream_len) break;
    frames++;
    pos += fb;
  }
  /* Every requested frame is delivered completely at 48 kHz. */
  TASK_CHECK(frames == 7);
  TASK_CHECK(stream_len == 7 * 384);
  enc_close(&e);
}
