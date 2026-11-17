/* masked_quantization.c
 *
 * Independent verification of the caller supplied threshold path.
 *
 * The test parses the emitted MP3 frames itself (header, side info,
 * scalefactors including SCFSI, Huffman integer spectrum), reconstructs
 * every spectral line with the specification dequantization formula and
 * recomputes per-band energy, distortion and normalized noise against the
 * read-only recorded MDCT spectrum. Production cost/noise code is never
 * used as an expectation; only table constants are shared.
 */

#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "huffman.h"
#include "layer3.h"
#include "masked.h"
#include "tables.h"

#ifndef SHINE_TEST_HOOKS
#error "This test must be built with -DSHINE_TEST_HOOKS"
#endif

#define SFB 22
#define GRAN 576
#define NAT (1.0 / 2147483648.0)
#define INFNOISE 1e300

static int failures;
static int checks;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                  \
    }                                                                          \
  } while (0)

/* ------------------------------------------------------------------ */
/* Bit reader                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
  const unsigned char *data;
  unsigned bit;
  unsigned size;
} bitreader_t;

static unsigned br_get(bitreader_t *br, unsigned n) {
  unsigned v = 0;
  while (n--) {
    if (br->bit >= br->size * 8) {
      failures++;
      printf("FAIL: bit read past end of frame (%s:%d)\n", __FILE__,
             __LINE__);
      return 0;
    }
    v = (v << 1) | ((br->data[br->bit >> 3] >> (7 - (br->bit & 7))) & 1);
    br->bit++;
  }
  return v;
}

static int suffix_all_ones(const unsigned char *data, unsigned from,
                           unsigned to) {
  for (unsigned b = from; b < to; b++)
    if (((data[b >> 3] >> (7 - (b & 7))) & 1) != 1)
      return 0;
  return 1;
}

/* ------------------------------------------------------------------ */
/* Parsed frame                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
  int nch;
  int samplerate;
  int samplerate_index;
  int bitrate;
  int padding;
  int mode;
  unsigned part2_3[2][2];
  unsigned big_values[2][2];
  unsigned global_gain[2][2];
  unsigned scalefac_compress[2][2];
  unsigned table_select[2][2][3];
  unsigned region0[2][2];
  unsigned region1[2][2];
  unsigned preflag[2][2];
  unsigned scalefac_scale[2][2];
  unsigned count1table[2][2];
  unsigned scfsi[2][4];
  int sf[2][2][SFB];
  int ix[2][2][GRAN];
  int part2[2][2];
  int huff_bits[2][2];
  int stuffing[2][2];
} parsed_frame_t;

static int huff_decode_pair(bitreader_t *br, unsigned table, int *x, int *y) {
  const struct huffcodetab *h = &shine_huffman_table[table];
  unsigned entries = h->xlen * h->ylen;
  unsigned idx;

  for (idx = 0; idx < entries; idx++) {
    unsigned len = h->hlen[idx];
    unsigned code;
    if (len == 0)
      continue;
    if (br->bit + len > br->size * 8)
      continue;
    code = 0;
    for (unsigned b = 0; b < len; b++)
      code = (code << 1) |
             ((br->data[(br->bit + b) >> 3] >>
               (7 - ((br->bit + b) & 7))) &
              1);
    if (code == h->table[idx]) {
      br->bit += len;
      *x = idx / h->ylen;
      *y = idx % h->ylen;
      if (table > 15) {
        if (*x == 15)
          *x += br_get(br, h->linbits);
        if (*x != 0 && br_get(br, 1))
          *x = -*x;
        if (*y == 15)
          *y += br_get(br, h->linbits);
        if (*y != 0 && br_get(br, 1))
          *y = -*y;
      } else {
        if (*x != 0 && br_get(br, 1))
          *x = -*x;
        if (*y != 0 && br_get(br, 1))
          *y = -*y;
      }
      return 0;
    }
  }
  return -1;
}

static void parse_frame(const unsigned char *data, unsigned size,
                        const unsigned count1_hint[2][2],
                        parsed_frame_t *pf) {
  bitreader_t br = {data, 0, size};
  int gr, ch, g, sfb;

  memset(pf, 0, sizeof(*pf));

  CHECK(br_get(&br, 11) == 0x7ff, "frame sync");
  CHECK(br_get(&br, 2) == MPEG_I, "mpeg version 1");
  CHECK(br_get(&br, 2) == LAYER_III, "layer iii");
  CHECK(br_get(&br, 1) == 1, "no crc");
  unsigned bitrate_index = br_get(&br, 4);
  unsigned sr_field = br_get(&br, 2);
  pf->padding = br_get(&br, 1);
  br_get(&br, 1); /* private */
  pf->mode = br_get(&br, 2);
  br_get(&br, 2); /* mode extension */
  br_get(&br, 1); /* copyright */
  br_get(&br, 1); /* original */
  br_get(&br, 2); /* emphasis */

  pf->nch = (pf->mode == MONO) ? 1 : 2;
  pf->samplerate_index = sr_field; /* MPEG-1 indices 0..2 */
  pf->samplerate = samplerates[sr_field];
  pf->bitrate = bitrates[bitrate_index][MPEG_I];

  br_get(&br, 9); /* reserved */
  br_get(&br, pf->nch == 2 ? 3 : 5); /* private bits */

  for (ch = 0; ch < pf->nch; ch++)
    for (g = 0; g < 4; g++)
      pf->scfsi[ch][g] = br_get(&br, 1);

  for (gr = 0; gr < 2; gr++)
    for (ch = 0; ch < pf->nch; ch++) {
      pf->part2_3[gr][ch] = br_get(&br, 12);
      pf->big_values[gr][ch] = br_get(&br, 9);
      pf->global_gain[gr][ch] = br_get(&br, 8);
      pf->scalefac_compress[gr][ch] = br_get(&br, 4);
      br_get(&br, 1); /* unused */
      for (unsigned r = 0; r < 3; r++)
        pf->table_select[gr][ch][r] = br_get(&br, 5);
      pf->region0[gr][ch] = br_get(&br, 4);
      pf->region1[gr][ch] = br_get(&br, 3);
      pf->preflag[gr][ch] = br_get(&br, 1);
      pf->scalefac_scale[gr][ch] = br_get(&br, 1);
      pf->count1table[gr][ch] = br_get(&br, 1);

      CHECK(pf->part2_3[gr][ch] <= 4095, "part2_3_length <= 4095");
    }

  /* Side information is one contiguous block; main data starts after it. */
  unsigned main_start = br.bit;
  bitreader_t mb = {data, main_start, size};
  const int *band = shine_scale_fact_band_index[pf->samplerate_index];

  /* Main data: granule outer, channel inner. */
  for (gr = 0; gr < 2; gr++)
    for (ch = 0; ch < pf->nch; ch++) {
      unsigned sc = pf->scalefac_compress[gr][ch];
      unsigned slen1 = shine_slen1_tab[sc];
      unsigned slen2 = shine_slen2_tab[sc];
      unsigned part2_start = mb.bit;

      for (g = 0; g < 4; g++) {
        unsigned slen = g < 2 ? slen1 : slen2;
        int start = g == 0 ? 0 : g == 1 ? 6 : g == 2 ? 11 : 16;
        int len = g == 0 ? 6 : 5;
        if (gr == 1 && pf->scfsi[ch][g]) {
          for (sfb = start; sfb < start + len; sfb++)
            pf->sf[gr][ch][sfb] = pf->sf[0][ch][sfb];
        } else {
          for (sfb = start; sfb < start + len; sfb++)
            pf->sf[gr][ch][sfb] = br_get(&mb, slen);
        }
      }
      pf->sf[gr][ch][21] = 0; /* implicit */
      pf->part2[gr][ch] = (int)(mb.bit - part2_start);

      /* Independent part2 accounting from slen tables and SCFSI. */
      int expect_part2 = 0;
      for (g = 0; g < 4; g++) {
        if (gr == 1 && pf->scfsi[ch][g])
          continue;
        unsigned slen = g < 2 ? slen1 : slen2;
        expect_part2 += (g == 0 ? 6 : 5) * slen;
      }
      CHECK(pf->part2[gr][ch] == expect_part2,
            "transmitted part2 matches slen/scfsi accounting");

      /* scalefac_compress must be the minimum-cost feasible table index. */
      int best_bits = INT_MAX, best_sc = -1;
      for (unsigned cand = 0; cand < 16; cand++) {
        unsigned s1 = shine_slen1_tab[cand];
        unsigned s2 = shine_slen2_tab[cand];
        int ok = 1, bits = 0;
        for (g = 0; g < 4; g++) {
          if (gr == 1 && pf->scfsi[ch][g])
            continue;
          unsigned slen = g < 2 ? s1 : s2;
          int start = g == 0 ? 0 : g == 1 ? 6 : g == 2 ? 11 : 16;
          int len = g == 0 ? 6 : 5;
          for (sfb = start; sfb < start + len; sfb++)
            if (pf->sf[gr][ch][sfb] > (int)((1u << slen) - 1))
              ok = 0;
          bits += len * slen;
        }
        if (ok && bits < best_bits) {
          best_bits = bits;
          best_sc = (int)cand;
        }
      }
      CHECK(best_sc == (int)sc, "scalefac_compress min cost, lowest index");

      unsigned huff_start = mb.bit;
      int *ix = pf->ix[gr][ch];
      unsigned bigvalues = pf->big_values[gr][ch] * 2;
      unsigned r1 = band[pf->region0[gr][ch] + 1];
      unsigned r2 = band[pf->region0[gr][ch] + pf->region1[gr][ch] + 2];
      unsigned huff_budget =
          pf->part2_3[gr][ch] - (unsigned)pf->part2[gr][ch];

      CHECK(r2 <= GRAN, "huffman region inside granule");

      for (unsigned i = 0; i < bigvalues; i += 2) {
        unsigned table = pf->table_select[gr][ch][(i >= r1) + (i >= r2)];
        if (table == 0) {
          ix[i] = 0;
          ix[i + 1] = 0;
        } else {
          int x, y;
          CHECK(huff_decode_pair(&mb, table, &x, &y) == 0,
                "big values huffman decode");
          ix[i] = x;
          ix[i + 1] = y;
        }
      }

      /* count1 quadruples: the run length is not in side info, so the
       * read-only test hook supplies the encoder value; the words and
       * sign bits themselves are decoded from the real bitstream. */
      unsigned c1_target = count1_hint[gr][ch];
      CHECK(bigvalues + c1_target * 4 <= GRAN, "count1 inside granule");
      unsigned c1 = 0;
      while (c1 < c1_target) {
        unsigned pos = bigvalues + c1 * 4;
        const struct huffcodetab *h =
            &shine_huffman_table[32 + pf->count1table[gr][ch]];
        unsigned p = 0, matched = 0;
        for (unsigned idx = 0; idx < 16; idx++) {
          unsigned len = h->hlen[idx];
          if (len == 0 || mb.bit + len > size * 8)
            continue;
          unsigned code = 0;
          for (unsigned b = 0; b < len; b++)
            code = (code << 1) |
                   ((data[(mb.bit + b) >> 3] >>
                     (7 - ((mb.bit + b) & 7))) &
                    1);
          if (code == h->table[idx]) {
            mb.bit += len;
            p = idx;
            matched = 1;
            break;
          }
        }
        CHECK(matched, "count1 huffman word decoded from stream");
        if (!matched)
          break;
        int vals[4] = {(int)(p & 1), (int)((p >> 1) & 1),
                       (int)((p >> 2) & 1), (int)((p >> 3) & 1)};
        for (int k = 0; k < 4; k++)
          if (vals[k]) {
            if (br_get(&mb, 1))
              vals[k] = -vals[k];
          }
        for (int k = 0; k < 4; k++)
          ix[pos + k] = vals[k];
        c1++;
      }
      for (unsigned i = bigvalues + c1 * 4; i < GRAN; i++)
        ix[i] = 0;

      pf->huff_bits[gr][ch] = (int)(mb.bit - huff_start);
      int remain = (int)huff_budget - pf->huff_bits[gr][ch];
      CHECK(remain >= 0, "huffman region within part2_3 budget");
      pf->stuffing[gr][ch] = remain;
      if (remain)
        CHECK(suffix_all_ones(data, mb.bit, mb.bit + remain),
              "trailing bits inside part2_3 are one-bit stuffing");
      mb.bit += remain;
    }
}

/* ------------------------------------------------------------------ */
/* Independent reconstruction and noise                                */
/* ------------------------------------------------------------------ */

static double band_noise(const int32_t *xr, const int *ix, int global_gain,
                         const int *sf, int samplerate_index, int sfb,
                         double ratio) {
  const int *band = shine_scale_fact_band_index[samplerate_index];
  double common = pow(2.0, ((double)global_gain - 210.0) / 4.0);
  double energy = 0.0, distortion = 0.0;

  for (int line = band[sfb]; line < band[sfb + 1]; line++) {
    double step = common * pow(2.0, -0.5 * sf[sfb]);
    int aq = ix[line] < 0 ? -ix[line] : ix[line];
    double y = 0.0;
    if (aq) {
      y = step * pow((double)aq, 4.0 / 3.0);
      if (ix[line] < 0)
        y = -y;
    }
    double x = (double)xr[line] * NAT;
    double d = x - y;
    energy += x * x;
    distortion += d * d;
  }

  if (energy == 0.0)
    return distortion == 0.0 ? 0.0 : INFNOISE;
  return distortion / (ratio * energy);
}

static double worst_noise(const int32_t *xr, const int *ix, int global_gain,
                          const int *sf, int samplerate_index,
                          const double *ratios) {
  double worst = 0.0;
  for (int sfb = 0; sfb < SFB; sfb++) {
    double n = band_noise(xr, ix, global_gain, sf, samplerate_index, sfb,
                          ratios[sfb]);
    if (n > worst)
      worst = n;
  }
  return worst;
}

/* ------------------------------------------------------------------ */
/* Encoder helpers                                                     */
/* ------------------------------------------------------------------ */

static shine_t make_encoder(int channels, int samplerate, int bitrate,
                            int mode) {
  shine_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels = channels;
  cfg.wave.samplerate = samplerate;
  cfg.mpeg.mode = mode;
  cfg.mpeg.bitr = bitrate;
  return shine_initialise(&cfg);
}

static void gen_pcm(int16_t *left, int16_t *right, int16_t *inter, int n,
                    int frame, unsigned seed) {
  unsigned state = seed + (unsigned)frame * 2654435761u;
  for (int i = 0; i < n; i++) {
    state = state * 1103515245u + 12345u;
    int noise = (int)((state >> 16) & 0x7fff) - 16384;
    int t = frame * n + i;
    int l = (int)(12000.0 * sin(2.0 * M_PI * (t % 97) / 97.0)) +
            (int)(5000.0 * sin(2.0 * M_PI * (t % 311) / 311.0)) +
            noise / 3;
    int r = l / 2 + (int)(4000.0 * sin(2.0 * M_PI * (t % 53) / 53.0)) +
            noise / 6;
    if (l > 32767) l = 32767;
    if (l < -32768) l = -32768;
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    left[i] = (int16_t)l;
    right[i] = (int16_t)r;
    if (inter) {
      inter[2 * i] = (int16_t)l;
      inter[2 * i + 1] = (int16_t)r;
    }
  }
}

static void uniform_ratios(double *r, int nch, double v) {
  for (int i = 0; i < 2 * nch * SFB; i++)
    r[i] = v;
}

/* Non-uniform thresholds: strong protection on the low bands, looser
 * thresholds up top; granule 1 uses a slightly different shape. */
static void shaped_ratios(double *r, int nch, int frame) {
  for (int gr = 0; gr < 2; gr++)
    for (int ch = 0; ch < nch; ch++)
      for (int sfb = 0; sfb < SFB; sfb++) {
        double v;
        if (sfb < 6)
          v = 0.01;
        else if (sfb < 11)
          v = 0.03;
        else if (sfb < 16)
          v = 0.15;
        else
          v = 0.6;
        if (gr == 1 && sfb >= 11 && sfb < 16)
          v = frame & 1 ? 0.4 : 0.05; /* force group 2 to differ sometimes */
        r[(gr * nch + ch) * SFB + sfb] = v;
      }
}

/* Thresholds shaped so granule 0 and granule 1 match exactly per group
 * for selected groups, while an adjacent group differs. */
static void scfsi_ratios(double *r, int nch, int differ_group) {
  for (int gr = 0; gr < 2; gr++)
    for (int ch = 0; ch < nch; ch++)
      for (int sfb = 0; sfb < SFB; sfb++) {
        int group = sfb < 6 ? 0 : sfb < 11 ? 1 : sfb < 16 ? 2 : 3;
        double v = group == 0 ? 0.01 : group == 1 ? 0.04
                   : group == 2   ? 0.12
                                  : 0.5;
        if (group == differ_group && gr == 1)
          v *= 0.25;
        r[(gr * nch + ch) * SFB + sfb] = v;
      }
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static int first_deviation_reported;
static void report_deviations(const char *tag, int frame,
                              const parsed_frame_t *pf,
                              const shine_test_hook_t *hooks,
                              const double *ratios) {
  for (int gr = 0; gr < 2; gr++)
    for (int ch = 0; ch < pf->nch; ch++) {
      const double *rr = ratios + (gr * pf->nch + ch) * SFB;
      for (int sfb = 0; sfb < SFB; sfb++) {
        double n = band_noise(hooks->xr[gr][ch], pf->ix[gr][ch],
                              pf->global_gain[gr][ch], pf->sf[gr][ch],
                              pf->samplerate_index, sfb, rr[sfb]);
        if (n > 1.0 && !first_deviation_reported) {
          first_deviation_reported = 1;
          printf("  first threshold deviation at %s frame=%d gr=%d ch=%d "
                 "sfb=%d noise=%.6g ratio=%.4g\n",
                 tag, frame, gr, ch, sfb, n, rr[sfb]);
        }
      }
    }
}

static int scfsi_seen_one[4], scfsi_seen_zero[4];
static int nonzero_sf_seen;
static int strict_improve_seen;
static int band21_nonzero_seen;

/* Shine returns a word-aligned buffer per call, but the bitstream cache is
 * shared between calls: a frame's sub-word tail appears at the start of the
 * next buffer. Concatenating every returned buffer therefore reconstructs
 * the exact stream, and frames are sliced out at their nominal lengths. */
static void verify_frame(const char *tag, int frame, const unsigned char *out,
                         int written, const parsed_frame_t *pf,
                         const shine_test_hook_t *hooks,
                         const double *ratios);

#define BATCH_MAX 32
typedef struct {
  unsigned char stream[BATCH_MAX * 600];
  unsigned stream_len;
  struct {
    double ratios[2 * 2 * SFB];
    shine_test_hook_t hooks;
  } fr[BATCH_MAX];
  int count;
} batch_t;

static unsigned nominal_frame_length(const unsigned char *hdr) {
  unsigned bitrate_index = (hdr[2] >> 4) & 0xf;
  unsigned sr_field = (hdr[2] >> 2) & 0x3;
  unsigned padding = (hdr[2] >> 1) & 0x1;
  return (unsigned)(144.0 * bitrates[bitrate_index][MPEG_I] * 1000.0 /
                    samplerates[sr_field]) +
         padding;
}

static void batch_append(batch_t *b, const unsigned char *out, int written) {
  CHECK(written > 0 && written % 4 == 0, "per-call buffer is word aligned");
  CHECK(b->stream_len + (unsigned)written <= sizeof(b->stream),
        "stream buffer capacity");
  memcpy(b->stream + b->stream_len, out, written);
  b->stream_len += written;
}

static void batch_add(batch_t *b, const unsigned char *out, int written,
                      const double *ratios, int nch) {
  CHECK(b->count < BATCH_MAX, "batch capacity");
  memcpy(b->fr[b->count].ratios, ratios,
         (size_t)(2 * nch * SFB) * sizeof(double));
  b->fr[b->count].hooks = *shine_test_hooks();
  b->count++;
  batch_append(b, out, written);
}

static void batch_verify(const batch_t *b, const char *tag,
                         int paddings[2]) {
  unsigned off = 0, nominal_total = 0;
  for (int f = 0; f < b->count; f++) {
    unsigned len = nominal_frame_length(b->stream + off);
    nominal_total += len;
    paddings[(b->stream[off + 2] >> 1) & 1]++;
    CHECK(off + len <= b->stream_len, "frame slice inside assembled stream");
    parsed_frame_t pf;
    parse_frame(b->stream + off, len, b->fr[f].hooks.chosen_count1, &pf);
    verify_frame(tag, f, b->stream + off, len, &pf, &b->fr[f].hooks,
                 b->fr[f].ratios);
    off += len;
  }
  /* The last call's buffer belongs to an unrecorded carrier frame whose
   * only role is flushing the previous frame's sub-word tail. */
  CHECK(off <= b->stream_len,
        "every recorded frame is fully present in the stream");
  CHECK(b->stream_len - off < 500,
        "only one carrier frame follows the recorded frames");
  CHECK(off == nominal_total, "padding slots account for frame sizing");
}

static void verify_frame(const char *tag, int frame, const unsigned char *out,
                         int written, const parsed_frame_t *pf,
                         const shine_test_hook_t *hooks,
                         const double *ratios) {
  (void)out;
  (void)frame;
  (void)tag;
  (void)written;

  for (int gr = 0; gr < 2; gr++)
    for (int ch = 0; ch < pf->nch; ch++) {
      const double *rr = ratios + (gr * pf->nch + ch) * SFB;

      CHECK(pf->preflag[gr][ch] == 0, "preflag forced to zero");
      CHECK(pf->scalefac_scale[gr][ch] == 0, "scalefac_scale forced to zero");

      for (int sfb = 0; sfb < SFB - 1; sfb++) {
        CHECK(pf->sf[gr][ch][sfb] >= 0 && pf->sf[gr][ch][sfb] <= 7,
              "long scalefactor in 0..7");
        if (pf->sf[gr][ch][sfb] != 0)
          nonzero_sf_seen = 1;
      }
      CHECK(pf->sf[gr][ch][21] == 0, "last band scalefactor implicit zero");

      /* SCFSI rule: reuse only on exact group equality. */
      if (gr == 1)
        for (int g = 0; g < 4; g++) {
          int start = g == 0 ? 0 : g == 1 ? 6 : g == 2 ? 11 : 16;
          int len = g == 0 ? 6 : 5;
          int equal = 1;
          for (int sfb = start; sfb < start + len; sfb++)
            if (pf->sf[0][ch][sfb] != pf->sf[1][ch][sfb])
              equal = 0;
          /* Rule: SCFSI reuse is permitted only on exact equality;
           * choosing not to reuse equal factors is always legal. */
          if (pf->scfsi[ch][g])
            CHECK(equal, "scfsi reuse requires exact scalefactor equality");
          if (pf->scfsi[ch][g])
            scfsi_seen_one[g] = 1;
          else
            scfsi_seen_zero[g] = 1;
          (void)hooks;
        }

      double masked_worst =
          worst_noise(hooks->xr[gr][ch], pf->ix[gr][ch],
                      pf->global_gain[gr][ch], pf->sf[gr][ch],
                      pf->samplerate_index, rr);
      /* The baseline candidate has all-zero scalefactors. */
      int zero_sf[SFB];
      memset(zero_sf, 0, sizeof(zero_sf));
      double base_worst = worst_noise(hooks->xr[gr][ch],
                                      hooks->base_ix[gr][ch],
                                      hooks->base_global_gain[gr][ch],
                                      zero_sf, pf->samplerate_index, rr);

      CHECK(masked_worst <= base_worst * (1.0 + 1e-9) + 1e-12,
            "masked worst noise never above legacy candidate");
      if (masked_worst < base_worst * (1.0 - 1e-6)) {
        strict_improve_seen = 1;
        printf("  improvement %s frame=%d gr=%d ch=%d: %.6g -> %.6g "
               "(x%.4f)\n",
               tag, frame, gr, ch, base_worst, masked_worst,
               base_worst / masked_worst);
      }

      /* The decoded integer spectrum must equal what the encoder
       * committed, on every line -- including the implicit-zero last
       * band, which still participates in quantization. */
      for (int line = 0; line < GRAN; line++)
        if (pf->ix[gr][ch][line] != hooks->chosen_ix[gr][ch][line]) {
          CHECK(0, "decoded spectrum matches committed spectrum");
          break;
        }
      const int *band = shine_scale_fact_band_index[pf->samplerate_index];
      int xr_nonzero = 0, committed_nonzero = 0;
      for (int line = band[21]; line < band[22]; line++) {
        if (hooks->xr[gr][ch][line] != 0)
          xr_nonzero = 1;
        if (hooks->chosen_ix[gr][ch][line] != 0) {
          committed_nonzero = 1;
          band21_nonzero_seen = 1;
        }
      }
      /* When capacity allows, the implicit-zero last band still carries
       * real quantized lines; under severe shortage it may round to zero
       * like any other band, but its lines must then be exactly zero and
       * still participate in the per-band noise accounting above. */
      if (xr_nonzero && !committed_nonzero)
        for (int line = band[21]; line < band[22]; line++)
          CHECK(pf->ix[gr][ch][line] == 0,
                "rounded-away band 21 lines decode as zero");
    }

  report_deviations(tag, frame, pf, hooks, ratios);
}

/* ------------------------------------------------------------------ */
/* Scenarios                                                           */
/* ------------------------------------------------------------------ */

static void scenario_byte_identical(void) {
  printf("== byte-identical NULL/0 path ==\n");
  static const struct {
    int channels;
    int samplerate;
    int bitrate;
    int mode;
  } cfgs[] = {{2, 48000, 128, STEREO},
              {2, 44100, 96, STEREO},
              {1, 32000, 64, MONO},
              {1, 48000, 32, MONO}};

  for (unsigned c = 0; c < sizeof(cfgs) / sizeof(cfgs[0]); c++) {
    int nch = cfgs[c].channels;
    int n = shine_samples_per_pass(
        make_encoder(nch, cfgs[c].samplerate, cfgs[c].bitrate, cfgs[c].mode));
    shine_t a = make_encoder(nch, cfgs[c].samplerate, cfgs[c].bitrate,
                             cfgs[c].mode);
    shine_t b = make_encoder(nch, cfgs[c].samplerate, cfgs[c].bitrate,
                             cfgs[c].mode);
    int16_t left[1152], right[1152], inter[2304];
    int16_t *planes[2] = {left, right};

    for (int f = 0; f < 4; f++) {
      gen_pcm(left, right, inter, n, f, 17 + c);
      int na, nb, nc, nd;
      unsigned char *oa, *ob, *oc, *od;

      oa = shine_encode_buffer(a, planes, &na);
      ob = shine_encode_buffer_masked(b, planes, NULL, 0, &nb);
      CHECK(na == nb && memcmp(oa, ob, na) == 0,
            "planar NULL ratios give byte identical legacy output");

      oc = shine_encode_buffer_interleaved(a, inter, &nc);
      od = shine_encode_buffer_interleaved_masked(b, inter, NULL, 0, &nd);
      CHECK(nc == nd && memcmp(oc, od, nc) == 0,
            "interleaved NULL ratios give byte identical legacy output");
      (void)nch;
    }
    shine_close(a);
    shine_close(b);
  }
}

static void scenario_entries_identical(void) {
  printf("== planar and interleaved masked entries ==\n");
  int n = 1152;
  shine_t a = make_encoder(2, 48000, 128, STEREO);
  shine_t b = make_encoder(2, 48000, 128, STEREO);
  int16_t left[1152], right[1152], inter[2304];
  int16_t *planes[2] = {left, right};
  double ratios[88];

  for (int f = 0; f < 4; f++) {
    gen_pcm(left, right, inter, n, f, 29);
    shaped_ratios(ratios, 2, f);
    int na, nb;
    unsigned char *oa = shine_encode_buffer_masked(a, planes, ratios, 88, &na);
    unsigned char *ob = shine_encode_buffer_interleaved_masked(
        b, inter, ratios, 88, &nb);
    CHECK(oa && ob && na == nb && memcmp(oa, ob, na) == 0,
          "both masked entries emit identical frames");
  }
  shine_close(a);
  shine_close(b);
}

/* Encode one extra silent frame purely to flush the bitstream cache's
 * sub-word tail; its bytes are appended but never parsed as a frame. */
static void batch_flush_carrier(batch_t *b, shine_t enc, int nch) {
  int16_t silence[SHINE_MAX_SAMPLES * 2];
  memset(silence, 0, sizeof(silence));
  double r[2 * 2 * SFB];
  uniform_ratios(r, nch, 1.0);
  int written;
  unsigned char *out = shine_encode_buffer_interleaved_masked(
      enc, silence, r, 2 * nch * SFB, &written);
  CHECK(out && written > 0, "carrier frame encoded");
  batch_append(b, out, written);
}

static void scenario_masked_stereo(const char *tag, int samplerate,
                                   int bitrate, int frames, unsigned seed,
                                   int low_capacity) {
  printf("== %s (sr=%d br=%d) ==\n", tag, samplerate, bitrate);
  shine_t enc = make_encoder(2, samplerate, bitrate, STEREO);
  int n = shine_samples_per_pass(enc);
  int16_t left[1152], right[1152], inter[2304];
  int16_t *planes[2] = {left, right};
  double ratios[88];
  batch_t batch;
  memset(&batch, 0, sizeof(batch));

  for (int f = 0; f < frames; f++) {
    gen_pcm(left, right, inter, n, f, seed);
    if (low_capacity) {
      /* Dense full scale broadband stress. */
      for (int i = 0; i < n; i++) {
        left[i] = (int16_t)((f * 57 + i * 131) % 60000 - 30000);
        right[i] = (int16_t)((f * 91 + i * 71) % 60000 - 30000);
      }
      uniform_ratios(ratios, 2, 0.02);
    } else if (f % 2 == 0)
      shaped_ratios(ratios, 2, f);
    else
      uniform_ratios(ratios, 2, 1.0); /* threshold switching between frames */

    int written;
    unsigned char *out =
        shine_encode_buffer_masked(enc, planes, ratios, 88, &written);
    CHECK(out && written > 0, "masked frame encoded");
    batch_add(&batch, out, written, ratios, 2);
  }

  batch_flush_carrier(&batch, enc, 2);
  int paddings[2] = {0, 0};
  batch_verify(&batch, tag, paddings);

  if (samplerate == 44100)
    CHECK(paddings[0] && paddings[1],
          "44.1 kHz stream observes both padding states");

  shine_close(enc);
}

static void scenario_mono(void) {
  printf("== mono masked path ==\n");
  shine_t enc = make_encoder(1, 32000, 64, MONO);
  int n = shine_samples_per_pass(enc);
  int16_t left[1152], inter[1152];
  double ratios[44];
  batch_t batch;
  memset(&batch, 0, sizeof(batch));

  for (int f = 0; f < 4; f++) {
    gen_pcm(left, NULL, NULL, n, f, 43);
    for (int i = 0; i < n; i++)
      inter[i] = left[i];
    shaped_ratios(ratios, 1, f);
    int written;
    unsigned char *out =
        shine_encode_buffer_interleaved_masked(enc, inter, ratios, 44,
                                                &written);
    CHECK(out && written > 0, "mono masked frame encoded");
    batch_add(&batch, out, written, ratios, 1);
  }
  batch_flush_carrier(&batch, enc, 1);
  int paddings[2] = {0, 0};
  batch_verify(&batch, "mono", paddings);
  shine_close(enc);
}

static void scenario_silence(void) {
  printf("== silence ==\n");
  shine_t enc = make_encoder(2, 48000, 128, STEREO);
  int16_t left[1152], right[1152];
  int16_t *planes[2] = {left, right};
  double ratios[88];
  memset(left, 0, sizeof(left));
  memset(right, 0, sizeof(right));
  shaped_ratios(ratios, 2, 0);

  int written;
  unsigned char *out =
      shine_encode_buffer_masked(enc, planes, ratios, 88, &written);
  CHECK(out && written > 0, "silent frame encoded");
  batch_t batch;
  memset(&batch, 0, sizeof(batch));
  batch_add(&batch, out, written, ratios, 2);
  batch_flush_carrier(&batch, enc, 2);
  int paddings[2] = {0, 0};
  batch_verify(&batch, "silence", paddings);
  parsed_frame_t pf;
  parse_frame(batch.stream,
              nominal_frame_length(batch.stream),
              batch.fr[0].hooks.chosen_count1, &pf);
  for (int gr = 0; gr < 2; gr++)
    for (int ch = 0; ch < 2; ch++) {
      for (int sfb = 0; sfb < SFB; sfb++)
        CHECK(pf.sf[gr][ch][sfb] == 0, "silence needs no scalefactors");
      double w = worst_noise(batch.fr[0].hooks.xr[gr][ch],
                             pf.ix[gr][ch], pf.global_gain[gr][ch],
                             pf.sf[gr][ch], pf.samplerate_index,
                             ratios + (gr * 2 + ch) * SFB);
      CHECK(w == 0.0, "silent granule reconstructs exactly to zero");
    }
  shine_close(enc);
}

static void scenario_scfsi_groups(void) {
  printf("== SCFSI group coverage ==\n");
  shine_t enc = make_encoder(2, 48000, 128, STEREO);
  int n = shine_samples_per_pass(enc);
  int16_t left[1152], right[1152];
  int16_t *planes[2] = {left, right};
  double ratios[88];
  batch_t batch;
  memset(&batch, 0, sizeof(batch));

  for (int differ = 0; differ < 4; differ++)
    for (int f = 0; f < 3; f++) {
      gen_pcm(left, right, NULL, n, f + differ * 7, 101 + differ);
      scfsi_ratios(ratios, 2, differ);
      int written;
      unsigned char *out =
          shine_encode_buffer_masked(enc, planes, ratios, 88, &written);
      batch_add(&batch, out, written, ratios, 2);
    }
  /* A run with identical granules should be able to reuse every group. */
  for (int f = 0; f < 3; f++) {
    gen_pcm(left, right, NULL, n, f, 202);
    scfsi_ratios(ratios, 2, -1);
    int written;
    unsigned char *out =
        shine_encode_buffer_masked(enc, planes, ratios, 88, &written);
    batch_add(&batch, out, written, ratios, 2);
  }
  batch_flush_carrier(&batch, enc, 2);
  int paddings[2] = {0, 0};
  batch_verify(&batch, "scfsi", paddings);
  shine_close(enc);

  for (int g = 0; g < 4; g++) {
    CHECK(scfsi_seen_one[g], "each SCFSI group observed reusing factors");
    CHECK(scfsi_seen_zero[g], "each SCFSI group observed writing factors");
  }
}

static void scenario_errors_and_retry(void) {
  printf("== invalid requests and retry ==\n");
  shine_t good = make_encoder(2, 48000, 128, STEREO);
  shine_t ref = make_encoder(2, 48000, 128, STEREO);
  int n = 1152;
  int16_t left[1152], right[1152];
  int16_t *planes[2] = {left, right};
  double ratios[88];
  uniform_ratios(ratios, 2, 0.5);

  int written;
  CHECK(shine_encode_buffer_masked(good, planes, NULL, 88, &written) == NULL &&
            written == -1,
        "NULL with non-zero count rejected");
  CHECK(shine_encode_buffer_masked(good, planes, ratios, 0, &written) == NULL &&
            written == -1,
        "non-NULL with zero count rejected");
  CHECK(shine_encode_buffer_masked(good, planes, ratios, 44, &written) == NULL &&
            written == -1,
        "stereo count mismatch rejected");
  double saved = ratios[5];
  ratios[5] = 0.0;
  CHECK(shine_encode_buffer_masked(good, planes, ratios, 88, &written) == NULL,
        "zero ratio rejected");
  ratios[5] = 1.5;
  CHECK(shine_encode_buffer_masked(good, planes, ratios, 88, &written) == NULL,
        "ratio above 1 rejected");
  ratios[5] = -0.2;
  CHECK(shine_encode_buffer_masked(good, planes, ratios, 88, &written) == NULL,
        "negative ratio rejected");
  ratios[5] = NAN;
  CHECK(shine_encode_buffer_masked(good, planes, ratios, 88, &written) == NULL,
        "NaN ratio rejected");
  ratios[5] = INFINITY;
  CHECK(shine_encode_buffer_masked(good, planes, ratios, 88, &written) == NULL,
        "infinite ratio rejected");
  ratios[5] = saved;

  shine_t mpeg2 = make_encoder(2, 22050, 64, STEREO);
  CHECK(shine_encode_buffer_masked(mpeg2, planes, ratios, 88, &written) ==
            NULL && written == -1,
        "MPEG-II rejected");
  shine_close(mpeg2);

  shine_t joint = make_encoder(2, 48000, 128, JOINT_STEREO);
  CHECK(shine_encode_buffer_masked(joint, planes, ratios, 88, &written) ==
            NULL && written == -1,
        "joint stereo rejected");
  shine_close(joint);

  shine_t mono = make_encoder(1, 48000, 64, MONO);
  CHECK(shine_encode_buffer_masked(mono, planes, ratios, 88, &written) ==
            NULL && written == -1,
        "mono encoder rejects stereo sized array");
  shine_close(mono);

  /* History must not have advanced: the encoder after the failures emits
   * exactly what an untouched encoder produces. */
  for (int f = 0; f < 4; f++) {
    gen_pcm(left, right, NULL, n, f, 71);
    int wa, wb;
    unsigned char *oa =
        shine_encode_buffer_masked(good, planes, ratios, 88, &wa);
    unsigned char *ob =
        shine_encode_buffer_masked(ref, planes, ratios, 88, &wb);
    CHECK(oa && ob && wa == wb && memcmp(oa, ob, wa) == 0,
          "failed requests leave analysis history untouched");
  }
  shine_close(good);
  shine_close(ref);
}

int main(void) {
  scenario_byte_identical();
  scenario_entries_identical();
  scenario_masked_stereo("stereo-48k", 48000, 128, 6, 11, 0);
  scenario_masked_stereo("stereo-44.1k", 44100, 128, 26, 23, 0);
  scenario_masked_stereo("low-capacity-32k", 48000, 32, 5, 37, 1);
  scenario_mono();
  scenario_silence();
  scenario_scfsi_groups();
  scenario_errors_and_retry();

  CHECK(nonzero_sf_seen, "real non-zero scalefactors were produced");
  CHECK(band21_nonzero_seen,
        "implicit-zero last band carries quantized spectrum");
  CHECK(strict_improve_seen,
        "strict worst-noise improvement over zero-factor candidate");

  printf("\nchecks=%d failures=%d\n", checks, failures);
  if (!first_deviation_reported)
    printf("no threshold deviations (all achieved bands within ratios)\n");
  if (failures == 0) {
    printf("MASKED_QUANTIZATION OK\n");
    return 0;
  }
  return 1;
}
