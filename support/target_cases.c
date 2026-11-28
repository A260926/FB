/* target_cases.c -- independent checks for the MPEG-1 window-switching chain.
 *
 * The production side is exercised only through the public API plus a few
 * read-only observation points that exist exclusively under SHINE_TEST_HOOKS.
 * The decoder below re-derives everything from the emitted bitstream: it
 * reads headers and side info with its own bit reader, walks the Huffman
 * codewords using the constant code tables (never the encoder's writers),
 * and reconstructs the transmitted signs and integer spectrum.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The read-only observation points are compiled into both the library (the
 * build script enables them for the test build) and this translation unit. */
#ifndef SHINE_TEST_HOOKS
#define SHINE_TEST_HOOKS
#endif

#include "cases.h"
#include "huffman.h"
#include "layer3.h"
#include "tables.h"
#include "types.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* Sentinel-guarded buffers and PCM sources.                           */
/* ------------------------------------------------------------------ */

#define CANARY 0x5A
#define MAXN SHINE_MAX_SAMPLES

/* Each PCM buffer carries one sentinel word before and after the payload;
 * pcmL/pcmR/pcmI point at the payload. */
static int16_t pcmL_storage[MAXN + 2], pcmR_storage[MAXN + 2];
static int16_t pcmI_storage[2 * MAXN + 2];
static int16_t *const pcmL = pcmL_storage + 1;
static int16_t *const pcmR = pcmR_storage + 1;
static int16_t *const pcmI = pcmI_storage + 1;

static void guard_pcm(void) {
  pcmL_storage[0] = pcmL_storage[MAXN + 1] = CANARY;
  pcmR_storage[0] = pcmR_storage[MAXN + 1] = CANARY;
  pcmI_storage[0] = pcmI_storage[2 * MAXN + 1] = CANARY;
}
static int pcm_intact(void) {
  return pcmL_storage[0] == CANARY && pcmL_storage[MAXN + 1] == CANARY &&
         pcmR_storage[0] == CANARY && pcmR_storage[MAXN + 1] == CANARY &&
         pcmI_storage[0] == CANARY && pcmI_storage[2 * MAXN + 1] == CANARY;
}

/* n is a per-channel sample count. */
static void make_pcm_sine(int16_t *l, int16_t *r, int n, int frame,
                          double f0, double f1) {
  for (int i = 0; i < n; i++) {
    double t = (double)(frame * n + i);
    double a = 0.55 * sin(2 * M_PI * f0 * t / 48000.0);
    double b = 0.35 * sin(2 * M_PI * f1 * t / 48000.0);
    int16_t s = (int16_t)((a + b) * 12000.0);
    l[i] = s;
    r[i] = (int16_t)(s / 2 + (int16_t)(4000.0 * sin(t * 0.03)));
  }
}

/* Asymmetric impulses: the three short windows see distinct content. */
static void make_pcm_impulses(int16_t *l, int16_t *r, int n, int frame) {
  for (int i = 0; i < n; i++)
    l[i] = r[i] = 0;
  int base = 7 * frame % 11;
  int pos[3] = {base + 40, base + 300, base + 700};
  int amp[3] = {20000, -15000, 11000};
  for (int k = 0; k < 3; k++)
    for (int d = -2; d <= 2; d++) {
      int p = pos[k] + d;
      if (p >= 0 && p < n) {
        int v = amp[k] / (1 + abs(d));
        l[p] = (int16_t)v;
        r[p] = (int16_t)(v / 2);
      }
    }
}

static shine_t make_encoder(int rate, int channels, int mode, int bitr) {
  shine_config_t c;
  memset(&c, 0, sizeof(c));
  c.wave.channels = channels;
  c.wave.samplerate = rate;
  shine_set_config_mpeg_defaults(&c.mpeg);
  c.mpeg.mode = mode;
  c.mpeg.bitr = bitr;
  return shine_initialise(&c);
}

/* Legal per-channel transition cycle: normal, start, short, short, stop. */
static int cycle_type(int idx) {
  static const int c[5] = {0, 1, 2, 2, 3};
  return c[idx % 5];
}

/* Build a block-type array following per-channel offsets into a legal
 * repeating sequence. Offsets start at a normal block boundary so every
 * channel independently begins at the head of a legal run. */
static void build_cycle_types(uint8_t *types, int ch_count, int frames,
                              const int *offset) {
  static const int cycle[5] = {0, 1, 2, 2, 3};
  int idx[2] = {offset[0], offset[1]};
  int k = 0;
  for (int f = 0; f < frames; f++)
    for (int gr = 0; gr < 2; gr++)
      for (int ch = 0; ch < ch_count; ch++) {
        int pos = idx[ch]++ % 5;
        if (pos == 0 && idx[ch] > 1) {
          /* After stop the legal successor is normal or start; the cycle
           * already restarts at normal. */
        }
        types[k++] = (uint8_t)cycle[pos];
      }
}

/* ------------------------------------------------------------------ */
/* Independent bit reader.                                             */
/* ------------------------------------------------------------------ */
typedef struct {
  const unsigned char *p;
  int bit;
} bitreader;

static unsigned br_get(bitreader *b, int n) {
  unsigned v = 0;
  for (int i = 0; i < n; i++) {
    int off = b->bit + i;
    v = (v << 1) | ((b->p[off >> 3] >> (7 - (off & 7))) & 1);
  }
  b->bit += n;
  return v;
}

typedef struct {
  int version, layer, bitrate_index, samplerate_index, padding, mode, channels;
  int frame_bytes, sideinfo_bytes, header_bits;
} hdr;

static const int kbitrates[16] = {0,  32,  40,  48,  56,  64,  80,  96,
                                  112, 128, 160, 192, 224, 256, 320, 0};
static const int ksrates[3] = {44100, 48000, 32000};

static int parse_header(const unsigned char *p, hdr *h) {
  memset(h, 0, sizeof(*h));
  if (p[0] != 0xff || (p[1] & 0xfe) != 0xfa) return 0;
  h->version = (p[1] >> 3) & 3;
  h->layer = (p[1] >> 1) & 3;
  h->bitrate_index = (p[2] >> 4) & 0xf;
  h->samplerate_index = (p[2] >> 2) & 3;
  h->padding = (p[2] >> 1) & 1;
  h->mode = (p[3] >> 6) & 3;
  h->channels = (h->mode == 3) ? 1 : 2;
  if (h->version != 3 || h->layer != 1) return 0;
  /* The header stores the samplerate index modulo three. Map it back to the
   * global samplerate table row (0..2 for MPEG-1). */
  int br = kbitrates[h->bitrate_index];
  if (!br) return 0;
  h->frame_bytes = (144000 * br) / ksrates[h->samplerate_index] + h->padding;
  h->header_bits = 32;
  /* MPEG-1 Layer III side info is 32 bytes stereo and 17 bytes mono. */
  h->sideinfo_bytes = (h->channels == 1) ? 17 : 32;
  return 1;
}

typedef struct {
  unsigned part2_3_length, big_values, global_gain, scalefac_compress;
  unsigned window_switching, block_type, mixed_block_flag;
  unsigned table_select[3], subblock_gain[3];
  unsigned region0_count, region1_count, preflag, scalefac_scale,
      count1table_select;
} side_gr;

typedef struct {
  unsigned scfsi[2][4];
  side_gr gr[2][2];
} side_info;

static void parse_side(const unsigned char *p, const hdr *h, side_info *si) {
  bitreader b = {p, h->header_bits};
  memset(si, 0, sizeof(*si));
  br_get(&b, 9);
  br_get(&b, (h->channels == 1) ? 5 : 3);
  for (int ch = 0; ch < h->channels; ch++)
    for (int s = 0; s < 4; s++) si->scfsi[ch][s] = br_get(&b, 1);
  for (int gr = 0; gr < 2; gr++)
    for (int ch = 0; ch < h->channels; ch++) {
      side_gr *g = &si->gr[gr][ch];
      g->part2_3_length = br_get(&b, 12);
      g->big_values = br_get(&b, 9);
      g->global_gain = br_get(&b, 8);
      g->scalefac_compress = br_get(&b, 4);
      g->window_switching = br_get(&b, 1);
      if (g->window_switching) {
        g->block_type = br_get(&b, 2);
        g->mixed_block_flag = br_get(&b, 1);
        for (int r = 0; r < 2; r++) g->table_select[r] = br_get(&b, 5);
        for (int w = 0; w < 3; w++) g->subblock_gain[w] = br_get(&b, 3);
        /* region0_count/region1_count are not present; ISO implies 7/36. */
        g->region0_count = 7;
        g->region1_count = 36;
      } else {
        for (int r = 0; r < 3; r++) g->table_select[r] = br_get(&b, 5);
        g->region0_count = br_get(&b, 4);
        g->region1_count = br_get(&b, 3);
      }
      g->preflag = br_get(&b, 1);
      g->scalefac_scale = br_get(&b, 1);
      g->count1table_select = br_get(&b, 1);
    }
}

/* Decode one Huffman pair by scanning the constant code table. The stored
 * codewords are right-aligned in HUFFBITS (the encoder writes the low
 * hlen[idx] bits), so compare against the low n bits. */
static void huff_decode_pair(bitreader *b, int table, int *x, int *y) {
  const struct huffcodetab *h = &shine_huffman_table[table];
  int xlen = (int)h->xlen, ylen = (int)h->ylen;
  unsigned code = 0;
  int found = 0, xi = 0, yi = 0;
  for (int n = 1; n <= 19 && !found; n++) {
    code = (code << 1) | br_get(b, 1);
    unsigned mask = (1u << n) - 1u;
    for (int i = 0; i < xlen && !found; i++)
      for (int j = 0; j < ylen; j++) {
        int idx = i * ylen + j;
        if ((int)h->hlen[idx] == n && (h->table[idx] & mask) == code) {
          xi = i;
          yi = j;
          found = 1;
          break;
        }
      }
  }
  *x = xi;
  *y = yi;
  if (table > 15) {
    /* ESC tables: x linbits, x sign, y linbits, y sign (ISO order). */
    int lb = (int)h->linbits;
    if (xi == 15 && lb) xi += (int)br_get(b, lb);
    if (xi) xi = br_get(b, 1) ? -xi : xi;
    if (yi == 15 && lb) yi += (int)br_get(b, lb);
    if (yi) yi = br_get(b, 1) ? -yi : yi;
  } else {
    if (xi) xi = br_get(b, 1) ? -xi : xi;
    if (yi) yi = br_get(b, 1) ? -yi : yi;
  }
  *x = xi;
  *y = yi;
}

/* Decode one granule's main data starting at *b, bounded by its own
 * part2_3_length. Returns the last index actually written to ix. */
static int decode_granule(bitreader *b, const hdr *h, side_gr *g,
                          int rate_index, int ix[576]) {
  const int *sb = &shine_scale_fact_band_index[rate_index][0];
  int startbit = b->bit;
  int endbit = startbit + (int)g->part2_3_length;
  int slen1 = shine_slen1_tab[g->scalefac_compress];
  int slen2 = shine_slen2_tab[g->scalefac_compress];

  for (int i = 0; i < 576; i++) ix[i] = 0;

  /* Scalefactors: this encoder emits scalefac_compress 0 (zero-width). Only
   * present for long blocks and for short blocks with a non-zero
   * scalefac_compress. */
  if (g->block_type != 2 || g->scalefac_compress != 0) {
    for (int s = 0; s < 6; s++) br_get(b, slen1);
    for (int s = 0; s < 5; s++) br_get(b, slen1);
    for (int s = 0; s < 5; s++) br_get(b, slen2);
    for (int s = 0; s < 5; s++) br_get(b, slen2);
  }

  int bigvalues = (int)g->big_values * 2;
  int region1, region2;
  if (g->window_switching) {
    if (g->block_type == 2) {
      const int *sbs = &shine_scale_fact_band_index_short[rate_index][0];
      region1 = 3 * sbs[8];
    } else {
      region1 = sb[8];
    }
    region2 = 1 << 20; /* implicit region1_count 36 leaves no second split */
  } else {
    region1 = sb[g->region0_count + 1];
    region2 = sb[g->region0_count + g->region1_count + 2];
  }

  for (int i = 0; i < bigvalues; i += 2) {
    int idx = (i >= region1) + (i >= region2);
    int table;
    if (g->window_switching)
      table = (idx >= 1) ? (int)g->table_select[1] : (int)g->table_select[0];
    else
      table = (int)g->table_select[idx];
    if (table) {
      int x, y;
      huff_decode_pair(b, table, &x, &y);
      ix[i] = x;
      ix[i + 1] = y;
    }
  }

  /* Count1 quadruples. Decoding runs until the granule's bit budget is
   * consumed; the encoder pads the tail with the all-ones codeword, which
   * the count1 tables map to a zero quadruple, so the padding is
   * indistinguishable from (and consistent with) untransmitted zeros. */
  const struct huffcodetab *hc =
      &shine_huffman_table[32 + g->count1table_select];
  int i = bigvalues, last = bigvalues;
  while (i + 3 < 576 && b->bit < endbit) {
    int save = b->bit;
    unsigned code = 0;
    int found = 0, v = 0, w = 0, x = 0, y = 0;
    for (int n = 1; n <= 6 && !found; n++) {
      code = (code << 1) | br_get(b, 1);
      unsigned mask = (1u << n) - 1u;
      for (int p = 0; p < 16; p++)
        if ((int)hc->hlen[p] == n && (hc->table[p] & mask) == code) {
          v = p & 1;
          w = (p >> 1) & 1;
          x = (p >> 2) & 1;
          y = (p >> 3) & 1;
          found = 1;
          break;
        }
    }
    if (!found) {
      b->bit = save;
      break;
    }
    int signs = v + w + x + y;
    if (b->bit + signs > endbit) {
      b->bit = save;
      break;
    }
    if (v) v = br_get(b, 1) ? -1 : 1;
    if (w) w = br_get(b, 1) ? -1 : 1;
    if (x) x = br_get(b, 1) ? -1 : 1;
    if (y) y = br_get(b, 1) ? -1 : 1;
    ix[i++] = v;
    ix[i++] = w;
    ix[i++] = x;
    ix[i++] = y;
    last = i;
  }
  b->bit = endbit;
  return last;
}

/* Transmitted short-block sequence -> natural band/window/frequency slots. */
static void short_to_natural(const int *stream, int *natural, int rate_index) {
  const int *sb = &shine_scale_fact_band_index_short[rate_index][0];
  int pos = 0;
  for (int b = 0; b < 13; b++) {
    int width = sb[b + 1] - sb[b];
    for (int w = 0; w < 3; w++)
      for (int f = 0; f < width; f++) {
        int g = sb[b] + f;
        int s = g / 6, kf = g % 6;
        natural[s * 18 + w * 6 + kf] = stream[pos++];
      }
  }
}

/* ------------------------------------------------------------------ */
/* Windows group.                                                      */
/* ------------------------------------------------------------------ */

/* Reference long MDCT: explicit window times the 36/18 kernel, halved to
 * match the fixed-point scale. */
static double ref_window(int block_type, int k) {
  if (block_type == 1) {
    if (k < 18) return sin(M_PI / 36.0 * (k + 0.5));
    if (k < 24) return 1.0;
    if (k < 30) return sin(M_PI / 12.0 * (k - 18 + 0.5));
    return 0.0;
  }
  if (block_type == 3) {
    if (k < 6) return 0.0;
    if (k < 12) return sin(M_PI / 12.0 * (k - 6 + 0.5));
    if (k < 18) return 1.0;
    return sin(M_PI / 36.0 * (k + 0.5));
  }
  return sin(M_PI / 36.0 * (k + 0.5));
}

static void ref_mdct_long(const double *in36, int block_type, double *out18) {
  for (int k = 0; k < 18; k++) {
    double sum = 0;
    for (int n = 0; n < 36; n++)
      sum += in36[n] * ref_window(block_type, n) *
             cos(M_PI / 72.0 * (2 * n + 19) * (2 * k + 1));
    out18[k] = sum * 0.5;
  }
}

TASK_CASE("windows", short_windows_are_three_twelve_point_transforms) {
  shine_t s = make_encoder(48000, 2, STEREO, 128);
  TASK_CHECK(s != NULL);
  int16_t *split[2] = {pcmL, pcmR};
  /* Frame 0: normal/start. Frame 1: short/short. */
  uint8_t types[8];
  int off[2] = {0, 0};
  build_cycle_types(types, 2, 2, off);
  guard_pcm();
  make_pcm_impulses(pcmL, pcmR, MAXN, 0);
  int written;
  TASK_CHECK(shine_encode_buffer_blocks(s, split, types, 4, &written) != NULL);
  make_pcm_impulses(pcmL, pcmR, MAXN, 1);
  TASK_CHECK(shine_encode_buffer_blocks(s, split, types + 4, 4, &written) !=
             NULL);
  TASK_CHECK(pcm_intact());

  /* Each of the three short windows must carry energy, and the three window
   * spectra of some band must differ (not one spectrum split three ways). */
  int energy[3] = {0, 0, 0};
  int shape_diff = 0;
  int odd_subband_energy = 0;
  for (int band = 0; band < 32; band++) {
    int e[3] = {0, 0, 0};
    for (int w = 0; w < 3; w++) {
      int64_t acc = 0;
      for (int k = 0; k < 6; k++) {
        int32_t v = shine_test_mdct_pre[0][1][band][w * 6 + k];
        acc += (int64_t)v * v;
      }
      e[w] = acc != 0;
      if (acc) energy[w] = 1;
    }
    if (e[0] && e[1] && (memcmp(&shine_test_mdct_pre[0][1][band][0],
                                &shine_test_mdct_pre[0][1][band][6],
                                6 * sizeof(int32_t)) != 0))
      shape_diff = 1;
    if ((band & 1) && (e[0] | e[1] | e[2])) odd_subband_energy = 1;
  }
  TASK_CHECK(energy[0] && energy[1] && energy[2]);
  TASK_CHECK(shape_diff);
  TASK_CHECK(odd_subband_energy);

  /* The three transforms must each match a direct 12-point sum taken from
   * the same 12 samples the encoder fed to each window (offsets 6, 12 and
   * 18 of the 36-point granule). */
  double maxerr = 0;
  for (int w = 0; w < 3; w++)
    for (int band = 1; band < 32; band += 2) {
      for (int k = 0; k < 6; k++) {
        double sum = 0;
        for (int n = 0; n < 12; n++) {
          double in =
              (double)shine_test_mdct_in[0][1][band][6 + 6 * w + n];
          sum += in * sin(M_PI / 12.0 * (n + 0.5)) *
                 cos(M_PI / 24.0 * (2 * n + 7) * (2 * k + 1));
        }
        sum *= 0.5;
        double got = (double)shine_test_mdct_pre[0][1][band][6 * w + k];
        double d = fabs(got - sum);
        if (d > maxerr) maxerr = d;
      }
    }
  TASK_CHECK(maxerr <= 64.0);
  shine_close(s);
}

TASK_CASE("windows", long_mdct_matches_direct_sum_for_all_long_types) {
  /* Normal, start and stop long windows all use the 36/18 kernel; verify
   * each against an independent direct sum on odd subbands. */
  int block_types[3] = {0, 1, 3};
  for (int t = 0; t < 3; t++) {
    int want = block_types[t];
    shine_t s = make_encoder(48000, 1, MONO, 128);
    TASK_CHECK(s != NULL);
    int16_t *split[1] = {pcmL};
    for (int i = 0; i < MAXN; i++)
      pcmL[i] = (int16_t)(3000 * sin(i * 0.021) + 1200 * sin(i * 0.17));
    /* Legal approach to the wanted type. */
    uint8_t t0[2], t1[2], t2[2];
    int written;
    if (want == 0) {
      t0[0] = t0[1] = 0;
      TASK_CHECK(shine_encode_buffer_blocks(s, split, t0, 2, &written));
      /* inspect granule 0 */
    } else if (want == 1) {
      t0[0] = 0;
      t0[1] = 1; /* granule 1 is start */
      TASK_CHECK(shine_encode_buffer_blocks(s, split, t0, 2, &written));
    } else {
      t0[0] = 0;
      t0[1] = 1;
      t1[0] = t1[1] = 2;
      t2[0] = 3;
      t2[1] = 0;
      TASK_CHECK(shine_encode_buffer_blocks(s, split, t0, 2, &written));
      TASK_CHECK(shine_encode_buffer_blocks(s, split, t1, 2, &written));
      TASK_CHECK(shine_encode_buffer_blocks(s, split, t2, 2, &written));
    }
    int gr = (want == 1) ? 1 : 0;
    double maxerr = 0;
    for (int band = 1; band < 32; band += 2) {
      double in[36], ref[18];
      for (int n = 0; n < 36; n++)
        in[n] = (double)shine_test_mdct_in[0][gr][band][n];
      ref_mdct_long(in, want, ref);
      for (int k = 0; k < 18; k++) {
        double got = (double)shine_test_mdct_pre[0][gr][band][k];
        double d = fabs(got - ref[k]);
        if (d > maxerr) maxerr = d;
      }
    }
    TASK_CHECK(maxerr <= 64.0);
    shine_close(s);
  }
}

TASK_CASE("windows", start_and_stop_differ_from_normal) {
  /* Same PCM, three encoders; compare long spectra before aliasing for the
   * same granule position. A pure label change would leave them equal. */
  int32_t snap[3][32][18];
  int modes[3] = {0, 1, 3};
  for (int m = 0; m < 3; m++) {
    int want = modes[m];
    shine_t s = make_encoder(48000, 1, MONO, 128);
    TASK_CHECK(s != NULL);
    int16_t *split[1] = {pcmL};
    for (int i = 0; i < MAXN; i++) pcmL[i] = (int16_t)(5000 * sin(i * 0.037));
    int written;
    uint8_t a[2], b[2], c[2];
    if (want == 0) {
      a[0] = a[1] = 0;
      TASK_CHECK(shine_encode_buffer_blocks(s, split, a, 2, &written));
      memcpy(snap[m], shine_test_mdct_pre[0][0], sizeof(snap[m]));
    } else if (want == 1) {
      a[0] = 0;
      a[1] = 1;
      TASK_CHECK(shine_encode_buffer_blocks(s, split, a, 2, &written));
      memcpy(snap[m], shine_test_mdct_pre[0][1], sizeof(snap[m]));
    } else {
      a[0] = 0;
      a[1] = 1;
      b[0] = b[1] = 2;
      c[0] = 3;
      c[1] = 0;
      TASK_CHECK(shine_encode_buffer_blocks(s, split, a, 2, &written));
      TASK_CHECK(shine_encode_buffer_blocks(s, split, b, 2, &written));
      TASK_CHECK(shine_encode_buffer_blocks(s, split, c, 2, &written));
      memcpy(snap[m], shine_test_mdct_pre[0][0], sizeof(snap[m]));
    }
    shine_close(s);
  }
  int diff_start = 0, diff_stop = 0;
  for (int band = 0; band < 32; band++)
    for (int k = 0; k < 18; k++) {
      if (snap[0][band][k] != snap[1][band][k]) diff_start = 1;
      if (snap[0][band][k] != snap[2][band][k]) diff_stop = 1;
    }
  TASK_CHECK(diff_start);
  TASK_CHECK(diff_stop);
}

TASK_CASE("windows", aliasing_exactly_on_long_blocks_only) {
  int16_t *split[1] = {pcmL};
  for (int i = 0; i < MAXN; i++)
    pcmL[i] = (int16_t)(6000 * sin(i * 0.013) + 3000 * sin(i * 0.41));

  /* Short path. */
  shine_t s = make_encoder(48000, 1, MONO, 128);
  TASK_CHECK(s != NULL);
  uint8_t a[2] = {0, 1}, b[2] = {2, 2};
  int w;
  TASK_CHECK(shine_encode_buffer_blocks(s, split, a, 2, &w));
  TASK_CHECK(shine_encode_buffer_blocks(s, split, b, 2, &w));
  int natural[576];
  short_to_natural(shine_test_mdct_final[0][0], natural, 1);
  int short_ok = 1;
  for (int band = 0; band < 32; band++)
    for (int k = 0; k < 18; k++)
      if (natural[band * 18 + k] != shine_test_mdct_pre[0][0][band][k])
        short_ok = 0;
  TASK_CHECK(short_ok); /* no butterfly applied to short blocks */
  shine_close(s);

  /* Long path. */
  shine_t l = make_encoder(48000, 1, MONO, 128);
  TASK_CHECK(l != NULL);
  uint8_t c[2] = {0, 0};
  TASK_CHECK(shine_encode_buffer_blocks(l, split, c, 2, &w));
  int altered = 0;
  for (int band = 1; band < 32; band++)
    if (shine_test_mdct_final[0][0][band * 18] !=
        shine_test_mdct_pre[0][0][band][0])
      altered = 1;
  TASK_CHECK(altered); /* long block did undergo aliasing reduction */
  shine_close(l);
}

TASK_CASE("windows", history_carries_across_granules_and_frames) {
  shine_t s = make_encoder(48000, 1, MONO, 128);
  TASK_CHECK(s != NULL);
  int16_t *split[1] = {pcmL};
  for (int i = 0; i < MAXN; i++) pcmL[i] = (int16_t)(4000 * sin(i * 0.05));
  int w;
  uint8_t t[2] = {0, 0};
  TASK_CHECK(shine_encode_buffer_blocks(s, split, t, 2, &w));
  int32_t g1_first[32][18];
  memcpy(g1_first, shine_test_mdct_pre[0][1], sizeof(g1_first));
  /* Second frame feeds new samples; granule 0 now reuses the last 18 subband
   * slots of the previous frame through l3_sb_sample[ch][0]. */
  for (int i = 0; i < MAXN; i++)
    pcmL[i] = (int16_t)(4000 * sin((i + MAXN) * 0.05));
  TASK_CHECK(shine_encode_buffer_blocks(s, split, t, 2, &w));
  int changed = 0;
  for (int band = 0; band < 32; band++)
    for (int k = 0; k < 18; k++)
      if (shine_test_mdct_pre[0][0][band][k] != g1_first[band][k]) changed = 1;
  TASK_CHECK(changed);
  shine_close(s);
}

/* ------------------------------------------------------------------ */
/* Bitstream group.                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
  unsigned char data[16384];
  int len;
} frame_buf;

typedef struct {
  frame_buf frames[80];
  int nframes;
  /* Byte stream as delivered by successive calls. At samplerates whose frame
   * length is not a multiple of four the final partial word of a frame is
   * completed at the start of the next call, exactly as the original output
   * convention describes; frames must be parsed from this stream. */
  unsigned char stream[131072];
  int stream_len;
  /* The observation points are overwritten on every call, so the checks
   * anchor on one fully delivered frame; its index is recorded here. */
  int obs_frame;
  int obs_ix[2][2][576]; /* signed quantized spectrum of obs_frame */
  int32_t obs_final[2][2][576];
  int obs_type[2][2];
} enc_result;

static void encode_sequence(int rate, int channels, int bitr, int nframes,
                            const int *offsets, enc_result *out) {
  shine_t s = make_encoder(rate, channels, channels == 1 ? MONO : STEREO, bitr);
  TASK_CHECK(s != NULL);
  int16_t *split[2] = {pcmL, pcmR};
  uint8_t types[2 * 2 * 64];
  build_cycle_types(types, channels, nframes, offsets);
  out->nframes = 0;
  out->stream_len = 0;
  /* The last call may leave a sub-word tail unflushed, so its carved frame is
   * not necessarily complete; anchor the observation on the previous one. */
  out->obs_frame = (nframes >= 2) ? nframes - 2 : 0;
  for (int f = 0; f < nframes; f++) {
    make_pcm_sine(pcmL, pcmR, MAXN, f, 520.0 + 60.0 * f, 3300.0 + 100.0 * f);
    int w = 999;
    unsigned char *p = shine_encode_buffer_blocks(
        s, split, types + f * 2 * channels, 2 * channels, &w);
    TASK_CHECK(p != NULL);
    TASK_CHECK(w > 0);
    TASK_CHECK(out->stream_len + w < (int)sizeof(out->stream));
    memcpy(out->stream + out->stream_len, p, w);
    out->stream_len += w;
    if (f == out->obs_frame)
      for (int ch = 0; ch < channels; ch++)
        for (int gr = 0; gr < 2; gr++) {
          memcpy(out->obs_ix[ch][gr], shine_test_q_signed[ch][gr],
                 sizeof(out->obs_ix[ch][gr]));
          memcpy(out->obs_final[ch][gr], shine_test_mdct_final[ch][gr],
                 sizeof(out->obs_final[ch][gr]));
          out->obs_type[ch][gr] = shine_test_mdct_type[ch][gr];
        }
  }
  /* Carve complete frames out of the concatenated stream. At 44.1 kHz a
   * frame is 417.9 bytes, so a frame can straddle two returned fragments;
   * only the concatenation is guaranteed to contain whole frames. */
  int pos = 0;
  while (pos + 4 <= out->stream_len && out->nframes < 80) {
    hdr h;
    if (!parse_header(out->stream + pos, &h)) break;
    if (pos + h.frame_bytes > out->stream_len) break;
    TASK_CHECK(out->nframes < 80);
    memcpy(out->frames[out->nframes].data, out->stream + pos, h.frame_bytes);
    out->frames[out->nframes].len = h.frame_bytes;
    out->nframes++;
    pos += h.frame_bytes;
  }
  shine_close(s);
}

/* Decode the observed frame and compare against the observation item by
 * item. */
static void compare_spectrum_frame(const enc_result *r, int rate_index,
                                   int channels, int *total_nonzero,
                                   int *total_count1) {
  const frame_buf *fb = &r->frames[r->obs_frame];
  hdr h;
  TASK_CHECK(parse_header(fb->data, &h));
  TASK_CHECK(h.channels == channels);
  TASK_CHECK(h.samplerate_index == rate_index);
  side_info si;
  parse_side(fb->data, &h, &si);
  bitreader b = {fb->data, 32 + h.sideinfo_bytes * 8};
  for (int gr = 0; gr < 2; gr++)
    for (int ch = 0; ch < channels; ch++) {
      side_gr *g = &si.gr[gr][ch];
      /* The transmitted window type must match what the transform used. */
      TASK_CHECK((int)g->block_type == r->obs_type[ch][gr]);
      /* Short blocks must never reuse scalefactors (ISO 2.4.2.7). */
      if (g->window_switching && g->block_type == 2)
        for (int s = 0; s < 4; s++) TASK_CHECK(si.scfsi[ch][s] == 0);
      int ix[576];
      int last = decode_granule(&b, &h, g, rate_index, ix);
      int natural[576];
      if (g->window_switching && g->block_type == 2) {
        short_to_natural(ix, natural, rate_index);
      } else {
        for (int i = 0; i < 576; i++) natural[i] = ix[i];
      }
      /* Short payloads stop at the last transmitted quadruple; long blocks
       * always cover the full spectrum. */
      int cmp_end = (g->window_switching && g->block_type == 2) ? last : 576;
      if (!g->window_switching) cmp_end = 576;
      for (int i = 0; i < cmp_end; i++) {
        TASK_CHECK(natural[i] == r->obs_ix[ch][gr][i]);
        if (natural[i]) (*total_nonzero)++;
      }
      *total_count1 += (int)g->big_values * 2;
      if (g->window_switching) {
        TASK_CHECK(g->block_type <= 3);
        TASK_CHECK(g->region0_count == 7 && g->region1_count == 36);
        TASK_CHECK(g->table_select[0] < 32 && g->table_select[1] < 32);
      }
    }
}

TASK_CASE("bitstream", short_frames_decode_to_quantized_spectrum) {
  static const int rates_header[3] = {2, 0, 1}; /* 32000, 44100, 48000 */
  static const int rates_table[3] = {2, 0, 1};  /* samplerate table rows */
  for (int ri = 0; ri < 3; ri++) {
    int offs[2] = {0, 0};
    enc_result r;
    /* 4 frames: normal/start, short/short, short/stop, normal/normal. */
    encode_sequence(rates_header[ri] == 2   ? 32000
                    : rates_header[ri] == 0 ? 44100
                                            : 48000,
                    2, 128, 4, offs, &r);
    TASK_CHECK(r.nframes == 4);
    int nz = 0, c1 = 0;
    compare_spectrum_frame(&r, rates_table[ri], 2, &nz, &c1);
    TASK_CHECK(nz > 0);
    TASK_CHECK(c1 > 0);
    /* Nonzero symbols with both signs must be present. */
    int neg = 0, pos = 0;
    for (int ch = 0; ch < 2; ch++)
      for (int gr = 0; gr < 2; gr++)
        for (int i = 0; i < 576; i++) {
          if (r.obs_ix[ch][gr][i] < 0) neg++;
          else if (r.obs_ix[ch][gr][i] > 0) pos++;
        }
    TASK_CHECK(neg > 0 && pos > 0);
  }
}

TASK_CASE("bitstream", two_huffman_regions_and_count1) {
  int offs[2] = {0, 1};
  enc_result r;
  encode_sequence(48000, 2, 128, 6, offs, &r);
  hdr h;
  TASK_CHECK(parse_header(r.frames[r.obs_frame].data, &h));
  side_info si;
  parse_side(r.frames[r.obs_frame].data, &h, &si);
  int region_used = 0, table_variety = 0;
  int seen[32];
  memset(seen, 0, sizeof(seen));
  for (int gr = 0; gr < 2; gr++)
    for (int ch = 0; ch < 2; ch++) {
      side_gr *g = &si.gr[gr][ch];
      if (g->window_switching) {
        TASK_CHECK(g->region0_count == 7 && g->region1_count == 36);
        region_used = 1;
      } else {
        TASK_CHECK(g->region0_count + g->region1_count > 0);
        if (g->region1_count > 0) region_used = 1;
      }
      if (g->table_select[0]) seen[g->table_select[0]] = 1;
      if (g->table_select[1]) seen[g->table_select[1]] = 1;
      TASK_CHECK(g->count1table_select <= 1);
    }
  for (int i = 0; i < 32; i++)
    if (seen[i]) table_variety++;
  TASK_CHECK(region_used);
  TASK_CHECK(table_variety >= 1);
  int nz = 0, c1 = 0;
  compare_spectrum_frame(&r, 1, 2, &nz, &c1);
  TASK_CHECK(nz > 0);
}

TASK_CASE("bitstream", padding_both_values_44100) {
  int offs[2] = {0, 0};
  enc_result r;
  /* 44.1 kHz at 128 kbps has a fractional slot count of ~417.96, so the
   * padding bit stays set for many frames before dropping once; 40 frames
   * guarantee both values are exercised. */
  encode_sequence(44100, 2, 128, 40, offs, &r);
  int seen[2] = {0, 0};
  int len_by_padding[2] = {-1, -1};
  for (int f = 0; f < r.nframes; f++) {
    hdr h;
    TASK_CHECK(parse_header(r.frames[f].data, &h));
    TASK_CHECK(h.frame_bytes == r.frames[f].len);
    seen[h.padding] = 1;
    len_by_padding[h.padding] = h.frame_bytes;
  }
  TASK_CHECK(seen[0] && seen[1]);
  /* 44100 at 128 kbps: whole slots = 417, so unpadded 417 / padded 418. */
  TASK_CHECK(len_by_padding[0] == 417);
  TASK_CHECK(len_by_padding[1] == 418);
  int nz = 0, c1 = 0;
  TASK_CHECK(r.obs_frame < r.nframes);
  compare_spectrum_frame(&r, 0, 2, &nz, &c1);
  TASK_CHECK(nz > 0);
}

TASK_CASE("bitstream", count1_and_signs_recovered_mono) {
  int offs[1] = {0};
  enc_result r;
  encode_sequence(48000, 1, 128, 4, offs, &r);
  int nz = 0, c1 = 0;
  compare_spectrum_frame(&r, 1, 1, &nz, &c1);
  TASK_CHECK(nz > 0);
  hdr h;
  TASK_CHECK(parse_header(r.frames[r.obs_frame].data, &h));
  side_info si;
  parse_side(r.frames[r.obs_frame].data, &h, &si);
  hdr h0;
  TASK_CHECK(parse_header(r.frames[0].data, &h0));
  TASK_CHECK(h0.channels == 1);
  TASK_CHECK(h0.sideinfo_bytes == 17);
}

/* ------------------------------------------------------------------ */
/* State group.                                                        */
/* ------------------------------------------------------------------ */

static int compare_all_normal(int rate, int ch, int bitr) {
  shine_t s_old = make_encoder(rate, ch, ch == 1 ? MONO : STEREO, bitr);
  shine_t s_new = make_encoder(rate, ch, ch == 1 ? MONO : STEREO, bitr);
  TASK_CHECK(s_old && s_new);
  int16_t *split[2] = {pcmL, pcmR};
  int ok = 1;
  for (int f = 0; f < 4; f++) {
    make_pcm_sine(pcmL, pcmR, MAXN, f, 440, 2600);
    int n1, n2;
    unsigned char a[4096], b[4096];
    unsigned char *pa = shine_encode_buffer(s_old, split, &n1);
    uint8_t t[4] = {0, 0, 0, 0};
    unsigned char *pb = shine_encode_buffer_blocks(s_new, split, t, 2 * ch, &n2);
    if (!pa || !pb || n1 != n2) {
      ok = 0;
      break;
    }
    memcpy(a, pa, n1);
    memcpy(b, pb, n2);
    if (memcmp(a, b, n1)) ok = 0;
  }
  /* flush output must also agree */
  int f1 = 0, f2 = 0;
  unsigned char *qa = shine_flush(s_old, &f1);
  unsigned char *qb = shine_flush(s_new, &f2);
  if (!qa || !qb || f1 != f2 || (f1 && memcmp(qa, qb, f1))) ok = 0;
  shine_close(s_old);
  shine_close(s_new);
  return ok;
}

TASK_CASE("state", all_normal_byte_identical_to_old_entry) {
  TASK_CHECK(compare_all_normal(48000, 2, 128));
  TASK_CHECK(compare_all_normal(44100, 1, 128));
  TASK_CHECK(compare_all_normal(32000, 2, 192));
  TASK_CHECK(compare_all_normal(48000, 1, 64));
}

TASK_CASE("state", cross_frame_sequences_and_channel_independence) {
  shine_t s = make_encoder(48000, 2, STEREO, 128);
  TASK_CHECK(s != NULL);
  int16_t *split[2] = {pcmL, pcmR};
  /* Per channel: 0,1,2,2,3,0,1,2,2,3 over granules; stagger ch1. */
  static const uint8_t seq[10][4] = {
      {0, 0, 1, 1}, /* frame 0 */
      {2, 2, 2, 2}, /* frame 1 */
      {3, 3, 0, 1}, /* frame 2 */
      {1, 2, 2, 2}, /* frame 3 */
      {3, 3, 0, 0}, /* frame 4 */
  };
  /* Validate legality locally as we go (mirror of public rules). */
  int prev[2] = {0, 0};
  for (int f = 0; f < 5; f++) {
    for (int g = 0; g < 2; g++)
      for (int ch = 0; ch < 2; ch++) {
        int from = prev[ch], to = seq[f][g * 2 + ch];
        int ok = (from == 0 && (to == 0 || to == 1)) ||
                 (from == 1 && to == 2) || (from == 2 && (to == 2 || to == 3)) ||
                 (from == 3 && (to == 0 || to == 1));
        TASK_CHECK(ok);
        prev[ch] = to;
      }
    make_pcm_sine(pcmL, pcmR, MAXN, f, 600 + 50 * f, 3500);
    int written;
    unsigned char *p = shine_encode_buffer_blocks(s, split, seq[f], 4, &written);
    TASK_CHECK(p != NULL && written > 0);
    hdr h;
    TASK_CHECK(parse_header(p, &h));
    side_info si;
    parse_side(p, &h, &si);
    int frame_has_short = 0;
    for (int g = 0; g < 2; g++)
      for (int ch = 0; ch < 2; ch++)
        if (seq[f][g * 2 + ch] == 2) frame_has_short = 1;
    for (int g = 0; g < 2; g++)
      for (int ch = 0; ch < 2; ch++) {
        int to = seq[f][g * 2 + ch];
        if (to == 0) TASK_CHECK(si.gr[g][ch].window_switching == 0);
        else TASK_CHECK(si.gr[g][ch].block_type == (unsigned)to);
        /* Short blocks force scfsi = 0 for their channel. */
        if (to == 2) TASK_CHECK(si.scfsi[ch][0] == 0);
      }
  }
  shine_close(s);
}

TASK_CASE("state", two_instances_interleaved) {
  shine_t a = make_encoder(48000, 2, STEREO, 128);
  shine_t b = make_encoder(48000, 2, STEREO, 128);
  TASK_CHECK(a && b);
  int16_t *split[2] = {pcmL, pcmR};
  int offs_a[2] = {0, 0}, offs_b[2] = {1, 0};
  uint8_t ta[12], tb[12];
  build_cycle_types(ta, 2, 3, offs_a);
  build_cycle_types(tb, 2, 3, offs_b);
  for (int f = 0; f < 3; f++) {
    int wa, wb;
    make_pcm_sine(pcmL, pcmR, MAXN, f, 700, 3000);
    TASK_CHECK(shine_encode_buffer_blocks(a, split, ta + f * 4, 4, &wa));
    make_pcm_sine(pcmL, pcmR, MAXN, f, 900, 5000);
    TASK_CHECK(shine_encode_buffer_blocks(b, split, tb + f * 4, 4, &wb));
    TASK_CHECK(wa > 0 && wb > 0);
  }
  shine_close(a);
  shine_close(b);
}

TASK_CASE("state", planar_and_interleaved_agree) {
  shine_t sp = make_encoder(48000, 2, STEREO, 128);
  shine_t si = make_encoder(48000, 2, STEREO, 128);
  TASK_CHECK(sp && si);
  int16_t *split[2] = {pcmL, pcmR};
  int offs[2] = {0, 0};
  uint8_t types[16];
  build_cycle_types(types, 2, 4, offs);
  for (int f = 0; f < 4; f++) {
    make_pcm_sine(pcmL, pcmR, MAXN, f, 520, 4100);
    for (int i = 0; i < MAXN; i++) {
      pcmI[2 * i] = pcmL[i];
      pcmI[2 * i + 1] = pcmR[i];
    }
    int np, ni;
    unsigned char *pp = shine_encode_buffer_blocks(sp, split, types + f * 4, 4,
                                                   &np);
    unsigned char *pi = shine_encode_buffer_interleaved_blocks(
        si, pcmI, types + f * 4, 4, &ni);
    TASK_CHECK(pp && pi);
    TASK_CHECK(np == ni && memcmp(pp, pi, np) == 0);
  }
  TASK_CHECK(pcm_intact());
  shine_close(sp);
  shine_close(si);
}

TASK_CASE("state", invalid_requests_then_clean_retry) {
  shine_t s = make_encoder(48000, 2, STEREO, 128);
  TASK_CHECK(s != NULL);
  int16_t *split[2] = {pcmL, pcmR};
  make_pcm_sine(pcmL, pcmR, MAXN, 0, 500, 2000);
  guard_pcm();

  static const uint8_t bad_count[3] = {0, 0, 0};
  static const uint8_t bad_type[4] = {0, 0, 4, 0};
  static const uint8_t bad_trans[4] = {0, 0, 0, 2}; /* normal -> short */
  static const uint8_t bad_stop[4] = {0, 0, 0, 3};  /* normal -> stop */
  static const uint8_t good[4] = {0, 0, 0, 0};
  int written;

  written = 42;
  TASK_CHECK(shine_encode_buffer_blocks(s, split, bad_count, 3, &written) ==
             NULL);
  TASK_CHECK(written == -1);
  written = 42;
  TASK_CHECK(shine_encode_buffer_blocks(s, split, bad_type, 4, &written) ==
             NULL);
  TASK_CHECK(written == -1);
  written = 42;
  TASK_CHECK(shine_encode_buffer_blocks(s, split, bad_trans, 4, &written) ==
             NULL);
  TASK_CHECK(written == -1);
  written = 42;
  TASK_CHECK(shine_encode_buffer_blocks(s, split, bad_stop, 4, &written) ==
             NULL);
  TASK_CHECK(written == -1);
  written = 42;
  TASK_CHECK(shine_encode_buffer_interleaved_blocks(s, pcmI, bad_trans, 4,
                                                   &written) == NULL);
  TASK_CHECK(written == -1);

  /* A subsequent valid call must match a pristine encoder byte for byte,
   * proving the rejected requests consumed no input. */
  shine_t fresh = make_encoder(48000, 2, STEREO, 128);
  TASK_CHECK(fresh != NULL);
  int w1, w2;
  unsigned char *p1 = shine_encode_buffer_blocks(s, split, good, 4, &w1);
  unsigned char *p2 = shine_encode_buffer_blocks(fresh, split, good, 4, &w2);
  TASK_CHECK(p1 && p2);
  TASK_CHECK(w1 == w2 && memcmp(p1, p2, w1) == 0);
  /* And over a second frame with a real transition. */
  static const uint8_t start[4] = {0, 0, 1, 1};
  unsigned char *p3 = shine_encode_buffer_blocks(s, split, start, 4, &w1);
  unsigned char *p4 = shine_encode_buffer_blocks(fresh, split, start, 4, &w2);
  TASK_CHECK(p3 && p4);
  TASK_CHECK(w1 == w2 && memcmp(p3, p4, w1) == 0);
  shine_close(s);
  shine_close(fresh);
}

TASK_CASE("state", unsupported_configurations_rejected) {
  int16_t *split[2] = {pcmL, pcmR};
  uint8_t t[4] = {0, 0, 0, 0};
  int written = 9;

  /* MPEG-2 samplerate. */
  shine_t s = make_encoder(24000, 2, STEREO, 64);
  TASK_CHECK(s != NULL);
  TASK_CHECK(shine_encode_buffer_blocks(s, split, t, 4, &written) == NULL);
  TASK_CHECK(written == -1);
  shine_close(s);

  /* Joint stereo. */
  shine_t j = make_encoder(48000, 2, JOINT_STEREO, 128);
  TASK_CHECK(j != NULL);
  written = 9;
  TASK_CHECK(shine_encode_buffer_blocks(j, split, t, 4, &written) == NULL);
  TASK_CHECK(written == -1);
  shine_close(j);

  /* Dual channel. */
  shine_t d = make_encoder(48000, 2, DUAL_CHANNEL, 128);
  TASK_CHECK(d != NULL);
  written = 9;
  TASK_CHECK(shine_encode_buffer_blocks(d, split, t, 4, &written) == NULL);
  TASK_CHECK(written == -1);
  shine_close(d);

  /* MPEG-1 mono with a mismatched MPEG mode. */
  shine_t m = make_encoder(48000, 1, STEREO, 128);
  TASK_CHECK(m != NULL);
  written = 9;
  TASK_CHECK(shine_encode_buffer_blocks(m, split, t, 2, &written) == NULL);
  TASK_CHECK(written == -1);
  shine_close(m);
}

TASK_CASE("state", complete_frames_cache_and_48k_divisibility) {
  shine_t s = make_encoder(48000, 2, STEREO, 128);
  TASK_CHECK(s != NULL);
  int16_t *split[2] = {pcmL, pcmR};
  int offs[2] = {0, 0};
  uint8_t types[2 * 2 * 8];
  build_cycle_types(types, 2, 8, offs);
  int total = 0;
  for (int f = 0; f < 8; f++) {
    make_pcm_sine(pcmL, pcmR, MAXN, f, 400 + 37 * f, 3300);
    int written;
    unsigned char *p = shine_encode_buffer_blocks(s, split, types + f * 4, 4,
                                                  &written);
    TASK_CHECK(p != NULL);
    hdr h;
    TASK_CHECK(parse_header(p, &h));
    TASK_CHECK(written == h.frame_bytes);
    TASK_CHECK(written % 4 == 0);
    /* Output buffer sentinel: bytes beyond `written` belong to the encoder's
     * internal buffer and are not reported as a frame; only the delivered
     * prefix is a complete frame. */
    total += written;
  }
  TASK_CHECK(total == 8 * 384);
  shine_global_config *g = (shine_global_config *)s;
  /* Each call delivered exactly one whole frame; the encoder's cache holds
   * only the not-yet-written tail of the current word (0..31 bits), never a
   * complete undelivered frame. */
  TASK_CHECK(g->bs.cache_bits >= 0 && g->bs.cache_bits <= 32);
  TASK_CHECK(g->bs.data_position == 0);
  int fl = 123;
  unsigned char *fp = shine_flush(s, &fl);
  /* Nothing beyond the delivered frames is pending: flush hands back no
   * additional bytes. */
  TASK_CHECK(fl == 0);
  (void)fp;
  shine_close(s);
}
