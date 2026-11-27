/*
 * masked_quantization.c
 *
 * Verification for the caller supplied masking thresholds consumed by
 * shine_encode_buffer_masked() and
 * shine_encode_buffer_interleaved_masked().
 *
 * The test parses real encoded frames (header, side information,
 * scalefactors and the Huffman coded integer spectrum), reconstructs every
 * scalefactor band with the standard dequantization formula and recomputes
 * the normalized band noise D / (ratio * E) from the raw MDCT spectrum.
 *
 * Because shine_putbits keeps up to three trailing bytes in a 32-bit cache,
 * the buffer handed back by one encode call need not equal one logical
 * frame; the test therefore accumulates the returned bytes into the
 * contiguous MPEG stream and walks back-to-back frames, matching each one to
 * a read-only snapshot of the encoder state taken right after that frame.
 *
 * The SHINE_TEST_HOOKS build only exposes read-only copies of the raw
 * spectrum and of the legacy all-zero-scalefactor candidate; no production
 * cost function is used as an expected value.
 */

#include "huffman.h"
#include "layer3.h"
#include "tables.h"
#include "types.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SF_BANDS 22
#define MAX_SNAPS 64

static int failures;
static const char *first_deviation;

static void fail(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (!first_deviation) {
    static char store[512];
    strncpy(store, buf, sizeof(store) - 1);
    store[sizeof(store) - 1] = 0;
    first_deviation = store;
  }
  printf("  FAIL: %s\n", buf);
  failures++;
}

static unsigned get(const unsigned char *p, unsigned *bit, unsigned n) {
  unsigned v = 0;
  while (n--) {
    v = (v << 1) | ((p[*bit >> 3] >> (7 - (*bit & 7))) & 1);
    (*bit)++;
  }
  return v;
}

/* ---- Read-only snapshot of the encoder state for one encoded frame ---- */

typedef struct {
  gr_info tt;
  int sf[22];
  int l3_enc[GRANULE_SIZE];
} gran_snapshot_t;

typedef struct {
  int channels;
  int samplerate_index;
  int scfsi[2][4];
  double ratios[2 * 2 * SF_BANDS];
  gran_snapshot_t g[2][2]; /* [granule][channel] */
  int32_t mdct_freq[2][2][GRANULE_SIZE];
  struct {
    int present;
    int quantizerStepSize;
    int l3_enc[GRANULE_SIZE];
  } legacy[2][2];
} frame_snapshot_t;

static void snapshot_capture(shine_t enc, const double *ratios,
                             frame_snapshot_t *s) {
  shine_global_config *c = (shine_global_config *)enc;
  int ch, gr, b;

  memset(s, 0, sizeof(*s));
  s->channels = c->wave.channels;
  s->samplerate_index = c->mpeg.samplerate_index;
  if (ratios)
    memcpy(s->ratios, ratios,
           sizeof(double) * 2 * c->wave.channels * SF_BANDS);
  for (ch = 0; ch < c->wave.channels; ch++) {
    for (b = 0; b < 4; b++)
      s->scfsi[ch][b] = c->side_info.scfsi[ch][b];
    for (gr = 0; gr < 2; gr++) {
      s->g[gr][ch].tt = c->side_info.gr[gr].ch[ch].tt;
      memcpy(s->g[gr][ch].l3_enc, c->l3_enc[ch][gr],
             sizeof(s->g[gr][ch].l3_enc));
      for (b = 0; b < 22; b++)
        s->g[gr][ch].sf[b] = c->scalefactor.l[gr][ch][b];
      memcpy(s->mdct_freq[ch][gr], c->mdct_freq[ch][gr],
             sizeof(s->mdct_freq[ch][gr]));
      s->legacy[ch][gr].present = c->test_legacy[ch][gr].present;
      s->legacy[ch][gr].quantizerStepSize =
          c->test_legacy[ch][gr].quantizerStepSize;
      memcpy(s->legacy[ch][gr].l3_enc, c->test_legacy[ch][gr].l3_enc,
             sizeof(s->legacy[ch][gr].l3_enc));
    }
  }
}

/* ---- Contiguous stream accumulation ----------------------------------- */

typedef struct {
  unsigned char data[1 << 16];
  int len;  /* bytes written */
  int pos;  /* bytes already exposed as frames */
} stream_buf_t;

static void stream_append(stream_buf_t *sb, const unsigned char *out, int n) {
  if (n <= 0)
    return;
  if (sb->len + n > (int)sizeof(sb->data)) {
    fail("test stream buffer overflow");
    return;
  }
  memcpy(sb->data + sb->len, out, n);
  sb->len += n;
}

static int expected_frame_bytes(int samplerate, int bitr, int padding) {
  return (144 * bitr * 1000) / samplerate + padding;
}

/* Expose one complete back-to-back frame at the current read position. The
 * pointer stays valid until the next append; the caller parses before that. */
static int stream_next_frame(stream_buf_t *sb, const unsigned char **frame,
                             int *flen) {
  int br_idx, sr_idx, padding;

  if (sb->len - sb->pos < 4)
    return 0;
  if (sb->data[sb->pos] != 0xff ||
      (sb->data[sb->pos + 1] & 0xfe) != 0xfa) {
    fail("stream lost frame synchronization");
    return 0;
  }
  br_idx = (sb->data[sb->pos + 2] >> 4) & 0x0f;
  sr_idx = (sb->data[sb->pos + 2] >> 2) & 0x03;
  padding = (sb->data[sb->pos + 2] >> 1) & 1;
  *flen = expected_frame_bytes(samplerates[sr_idx],
                               bitrates[br_idx][MPEG_I], padding);
  if (sb->len - sb->pos < *flen)
    return 0;
  *frame = sb->data + sb->pos;
  sb->pos += *flen;
  return 1;
}

/* ---- Bitstream parser -------------------------------------------------- */

typedef struct {
  unsigned p23, big_values, global_gain, scalefac_compress;
  unsigned table_select[3], region0_count, region1_count;
  unsigned preflag, scalefac_scale, count1table;
  int sf[22];
  int ix[GRANULE_SIZE];
  int part2_bits, huff_bits, stuffing_bits;
  unsigned main_start;
} parsed_gran_t;

typedef struct {
  int channels, samplerate, bitr, padding;
  int scfsi[2][4];
  parsed_gran_t g[2][2]; /* [granule][channel] */
} parsed_frame_t;

static int huffman_lookup(const struct huffcodetab *h,
                          const unsigned char *p, unsigned *bit,
                          unsigned maxlen) {
  unsigned code = 0, len = 0;
  int n = (int)(h->xlen * h->ylen);

  while (len < maxlen) {
    code = (code << 1) | get(p, bit, 1);
    len++;
    for (int i = 0; i < n; i++)
      if (h->hlen[i] == len && h->table[i] == code)
        return i;
  }
  return -1;
}

static int decode_pair(const struct huffcodetab *h, const unsigned char *p,
                       unsigned *bit, int *ox, int *oy) {
  unsigned maxlen = 0;
  int x, y;
  int n = (int)(h->xlen * h->ylen);
  for (int i = 0; i < n; i++)
    if (h->hlen[i] > maxlen)
      maxlen = h->hlen[i];

  int idx = huffman_lookup(h, p, bit, maxlen);
  if (idx < 0)
    return -1;
  x = idx / h->ylen;
  y = idx % h->ylen;

  if (h->linbits) {
    int vx = x, vy = y;
    if (x == 15)
      vx = 15 + (int)get(p, bit, h->linbits);
    if (vx && get(p, bit, 1))
      vx = -vx;
    if (y == 15)
      vy = 15 + (int)get(p, bit, h->linbits);
    if (vy && get(p, bit, 1))
      vy = -vy;
    *ox = vx;
    *oy = vy;
  } else {
    if (x && get(p, bit, 1))
      x = -x;
    if (y && get(p, bit, 1))
      y = -y;
    *ox = x;
    *oy = y;
  }
  return 0;
}

static void parse_frame(const frame_snapshot_t *s, const unsigned char *data,
                        int flen, parsed_frame_t *pf) {
  unsigned bit = 0;
  int gr, ch, b;

  if (get(data, &bit, 11) != 0x7ff)
    fail("frame sync missing");
  get(data, &bit, 2); /* version */
  get(data, &bit, 2); /* layer */
  get(data, &bit, 1); /* protection */
  pf->bitr = bitrates[get(data, &bit, 4)][MPEG_I];
  pf->samplerate = samplerates[get(data, &bit, 2)];
  pf->padding = get(data, &bit, 1);
  get(data, &bit, 1); /* private */
  pf->channels = (get(data, &bit, 2) == MONO) ? 1 : 2;
  get(data, &bit, 6); /* mode ext, copyright, original, emphasis */

  if (pf->channels == 2)
    get(data, &bit, 9 + 3);
  else
    get(data, &bit, 9 + 5);

  for (ch = 0; ch < pf->channels; ch++)
    for (b = 0; b < 4; b++)
      pf->scfsi[ch][b] = get(data, &bit, 1);

  for (gr = 0; gr < 2; gr++)
    for (ch = 0; ch < pf->channels; ch++) {
      parsed_gran_t *pg = &pf->g[gr][ch];
      memset(pg, 0, sizeof(*pg));
      pg->p23 = get(data, &bit, 12);
      pg->big_values = get(data, &bit, 9);
      pg->global_gain = get(data, &bit, 8);
      pg->scalefac_compress = get(data, &bit, 4);
      /* windows_switching_flag: shine only emits long blocks, always 0. */
      if (get(data, &bit, 1) != 0)
        fail("unexpected windows_switching_flag (gr%d ch%d)", gr, ch);
      for (int k = 0; k < 3; k++)
        pg->table_select[k] = get(data, &bit, 5);
      pg->region0_count = get(data, &bit, 4);
      pg->region1_count = get(data, &bit, 3);
      pg->preflag = get(data, &bit, 1);
      pg->scalefac_scale = get(data, &bit, 1);
      pg->count1table = get(data, &bit, 1);
    }

  for (gr = 0; gr < 2; gr++)
    for (ch = 0; ch < pf->channels; ch++) {
      parsed_gran_t *pg = &pf->g[gr][ch];
      int slen1 = shine_slen1_tab[pg->scalefac_compress];
      int slen2 = shine_slen2_tab[pg->scalefac_compress];
      pg->main_start = bit;

      for (b = 0; b < 21; b++) {
        int group = b < 6 ? 0 : b < 11 ? 1 : b < 16 ? 2 : 3;
        int width = group < 2 ? slen1 : slen2;
        if (gr == 1 && pf->scfsi[ch][group])
          pg->sf[b] = pf->g[0][ch].sf[b];
        else
          pg->sf[b] = width ? (int)get(data, &bit, width) : 0;
      }
      pg->sf[21] = 0; /* last long band is implicitly zero */
      pg->part2_bits = (int)(bit - pg->main_start);

      unsigned huff_start = bit;
      const int *band =
          &shine_scale_fact_band_index[s->samplerate_index][0];
      unsigned r1 = band[pg->region0_count + 1];
      unsigned r2 = band[pg->region0_count + pg->region1_count + 2];
      unsigned bigend = pg->big_values * 2;

      for (unsigned i = 0; i < bigend; i += 2) {
        unsigned region = (i >= r1) + (i >= r2);
        unsigned t = pg->table_select[region];
        if (t == 0) {
          pg->ix[i] = pg->ix[i + 1] = 0;
          continue;
        }
        if (decode_pair(&shine_huffman_table[t], data, &bit, &pg->ix[i],
                        &pg->ix[i + 1]) < 0) {
          fail("big-values Huffman decode failed (gr%d ch%d)", gr, ch);
          return;
        }
      }

      /* count1 region length is not transmitted; the read-only snapshot of
       * the encoder's partition supplies the quadruple count, while every
       * symbol and sign bit is decoded from the real bitstream. */
      const struct huffcodetab *c1 =
          &shine_huffman_table[32 + pg->count1table];
      unsigned c1max = 0;
      for (int i = 0; i < (int)(c1->xlen * c1->ylen); i++)
        if (c1->hlen[i] > c1max)
          c1max = c1->hlen[i];
      unsigned count1 = s->g[gr][ch].tt.count1;
      unsigned pos = bigend;
      for (unsigned qd = 0; qd < count1; qd++) {
        int q = huffman_lookup(c1, data, &bit, c1max);
        if (q < 0) {
          fail("count1 Huffman decode failed (gr%d ch%d)", gr, ch);
          return;
        }
        if (pos + 4 > GRANULE_SIZE) {
          fail("count1 region overruns 576 samples (gr%d ch%d)", gr, ch);
          return;
        }
        int vals[4] = {q & 1, (q >> 1) & 1, (q >> 2) & 1, (q >> 3) & 1};
        for (int k = 0; k < 4; k++)
          if (vals[k]) {
            if (get(data, &bit, 1))
              vals[k] = -vals[k];
          }
        for (int k = 0; k < 4; k++)
          pg->ix[pos + k] = vals[k];
        pos += 4;
      }
      pg->huff_bits = (int)(bit - huff_start);

      while ((int)(bit - pg->main_start) < (int)pg->p23) {
        if (!get(data, &bit, 1)) {
          fail("non-one stuffing bit (gr%d ch%d)", gr, ch);
          break;
        }
        pg->stuffing_bits++;
      }
      if ((int)(bit - pg->main_start) != (int)pg->p23)
        fail("granule did not end exactly on part2_3 boundary (gr%d ch%d)",
             gr, ch);
    }

  if ((int)bit != flen * 8)
    fail("parsed %u bits but frame has %d bytes", bit, flen);
}

/* ---- Reconstruction and normalized noise ------------------------------ */

static double band_noise(const frame_snapshot_t *s, int ch, int gr, int sfb,
                         const int *ix, int step, const int *sf,
                         double ratio) {
  const int *band = &shine_scale_fact_band_index[s->samplerate_index][0];
  int start = band[sfb], end = band[sfb + 1];
  int scalefac = sfb < 21 ? sf[sfb] : 0;
  double gain = pow(2.0, (double)step / 4.0 - (double)scalefac / 2.0);
  double E = 0.0, D = 0.0;

  for (int i = start; i < end; i++) {
    double xr = (double)s->mdct_freq[ch][gr][i] / 2147483648.0;
    double rec =
        ix[i] ? (ix[i] < 0 ? -1.0 : 1.0) *
                    pow((double)abs(ix[i]), 4.0 / 3.0) * gain
              : 0.0;
    E += xr * xr;
    D += (xr - rec) * (xr - rec);
  }
  if (E == 0.0)
    return D == 0.0 ? 0.0 : INFINITY;
  return D / (ratio * E);
}

static double granule_worst(const frame_snapshot_t *s, int ch, int gr,
                            const int *ix, int step, const int *sf) {
  double worst = 0.0;
  for (int b = 0; b < SF_BANDS; b++) {
    double ratio = s->ratios[((gr * s->channels + ch) * SF_BANDS) + b];
    double n = band_noise(s, ch, gr, b, ix, step, sf, ratio);
    if (n > worst)
      worst = n;
  }
  return worst;
}

/* ---- Frame verification against its snapshot -------------------------- */

static void verify_frame(const frame_snapshot_t *s, const unsigned char *data,
                         int flen, parsed_frame_t *pf) {
  parse_frame(s, data, flen, pf);

  if (pf->channels != s->channels)
    fail("parsed channel count %d != %d", pf->channels, s->channels);
  if (flen != expected_frame_bytes(pf->samplerate, pf->bitr, pf->padding))
    fail("frame length %d does not match rate/sample formula (padding %d)",
         flen, pf->padding);

  for (int gr = 0; gr < 2; gr++)
    for (int ch = 0; ch < s->channels; ch++) {
      parsed_gran_t *pg = &pf->g[gr][ch];
      const gr_info *gi = &s->g[gr][ch].tt;

      if (pg->p23 != gi->part2_3_length)
        fail("part2_3_length parse %u != side info %u (gr%d ch%d)", pg->p23,
             gi->part2_3_length, gr, ch);
      if (pg->p23 > 4095)
        fail("part2_3_length %u exceeds 4095", pg->p23);
      if (pg->preflag != 0 || pg->scalefac_scale != 0)
        fail("preflag/scalefac_scale not zero (gr%d ch%d)", gr, ch);
      if (pg->global_gain != gi->global_gain ||
          gi->global_gain != (unsigned)gi->quantizerStepSize + 210)
        fail("global gain inconsistent (gr%d ch%d)", gr, ch);
      if (pg->part2_bits != (int)gi->part2_length)
        fail("part2 consumed %d != part2_length %u (gr%d ch%d)",
             pg->part2_bits, gi->part2_length, gr, ch);
      if (pg->part2_bits + pg->huff_bits + pg->stuffing_bits !=
          (int)pg->p23)
        fail("part2+huffman+stuffing %d != part2_3 %u (gr%d ch%d)",
             pg->part2_bits + pg->huff_bits + pg->stuffing_bits, pg->p23, gr,
             ch);

      for (int b = 0; b < SF_BANDS; b++) {
        if (pg->sf[b] < 0 || pg->sf[b] > 7)
          fail("scalefactor %d out of 0..7 (band %d)", pg->sf[b], b);
        if (pg->sf[b] != s->g[gr][ch].sf[b])
          fail("parsed scalefactor mismatch band %d (gr%d ch%d)", b, gr, ch);
      }
      if (pg->sf[21] != 0)
        fail("band 21 scalefactor must be implicit zero");

      /* scalefac_compress: minimum part2 cost among indices that can
       * represent every transmitted value, ties on the smallest index. */
      {
        int m1 = 0, m2 = 0, need1, need2, best = -1, best_cost = 0;
        for (int b = 0; b <= 10; b++)
          if (pg->sf[b] > m1)
            m1 = pg->sf[b];
        for (int b = 11; b <= 20; b++)
          if (pg->sf[b] > m2)
            m2 = pg->sf[b];
        need1 = 0;
        while ((1 << need1) <= m1)
          need1++;
        need2 = 0;
        while ((1 << need2) <= m2)
          need2++;
        for (int sci = 0; sci < 16; sci++) {
          int s1 = shine_slen1_tab[sci], s2 = shine_slen2_tab[sci];
          if (s1 < need1 || s2 < need2)
            continue;
          int cost = 11 * s1 + 10 * s2;
          if (best < 0 || cost < best_cost) {
            best = sci;
            best_cost = cost;
          }
        }
        if (best != (int)pg->scalefac_compress)
          fail("scalefac_compress %u not minimum-cost (want %d, gr%d ch%d)",
               pg->scalefac_compress, best, gr, ch);
      }

      /* SCFSI: reuse only on exact group equality; verify all four groups. */
      for (int g = 0; g < 4; g++) {
        static const int gs[4] = {0, 6, 11, 16};
        static const int ge[4] = {6, 11, 16, 21};
        int equal = 1;
        for (int b = gs[g]; b < ge[g]; b++)
          if (s->g[0][ch].sf[b] != s->g[1][ch].sf[b])
            equal = 0;
        if (gr == 1 && pf->scfsi[ch][g] != equal)
          fail("scfsi group %d flag %d but equality=%d (ch%d)", g,
               pf->scfsi[ch][g], equal, ch);
        if (pf->scfsi[ch][g] != s->scfsi[ch][g])
          fail("parsed scfsi differs from snapshot (ch%d group%d)", ch, g);
      }

      /* Decoded integer spectrum must match production exactly. */
      for (int i = 0; i < GRANULE_SIZE; i++)
        if (pg->ix[i] != s->g[gr][ch].l3_enc[i]) {
          fail("huffman spectrum mismatch at ix[%d]: parsed %d != %d (gr%d "
               "ch%d)",
               i, pg->ix[i], s->g[gr][ch].l3_enc[i], gr, ch);
          break;
        }

      /* Standard-formula reconstruction; worst normalized noise must not
       * exceed the legacy candidate under the same entering state. */
      double worst = granule_worst(s, ch, gr, pg->ix,
                                   gi->quantizerStepSize, pg->sf);
      if (s->legacy[ch][gr].present) {
        int zerosf[SF_BANDS] = {0};
        double oldworst = granule_worst(
            s, ch, gr, s->legacy[ch][gr].l3_enc,
            s->legacy[ch][gr].quantizerStepSize, zerosf);
        if (worst > oldworst + 1e-9)
          fail("worst noise %g exceeds legacy %g (gr%d ch%d)", worst,
               oldworst, gr, ch);
      }
    }
}

/* ---- Harness tying encode calls to drained logical frames ------------- */

typedef struct {
  stream_buf_t sb;
  frame_snapshot_t *snaps;
  int nsnaps;
  int verified;
  int strict_improvements;
  int nonzero_granules;
  int scfsi_seen[2][4][2];
  int pad_seen[2];
  int first_improvement_band;
  double first_oldworst, first_newworst;
} harness_t;

static void harness_init(harness_t *h) {
  memset(h, 0, sizeof(*h));
  h->snaps = calloc(MAX_SNAPS, sizeof(frame_snapshot_t));
  if (!h->snaps)
    fail("could not allocate snapshot storage");
}

static void harness_done(harness_t *h) {
  free(h->snaps);
  h->snaps = NULL;
}

static void harness_submit(harness_t *h, shine_t enc,
                           const double *ratios) {
  if (h->nsnaps >= MAX_SNAPS) {
    fail("too many frames for harness");
    return;
  }
  snapshot_capture(enc, ratios, &h->snaps[h->nsnaps]);
  h->nsnaps++;
}

static void harness_drain(harness_t *h) {
  const unsigned char *frame;
  int flen;

  while (stream_next_frame(&h->sb, &frame, &flen)) {
    if (h->verified >= h->nsnaps) {
      fail("drained a frame without a matching snapshot");
      return;
    }
    frame_snapshot_t *s = &h->snaps[h->verified];
    parsed_frame_t pf;
    parse_frame(s, frame, flen, &pf);
    verify_frame(s, frame, flen, &pf);
    h->pad_seen[pf.padding]++;

    for (int ch = 0; ch < s->channels; ch++) {
      for (int g = 0; g < 4; g++)
        h->scfsi_seen[ch][g][pf.scfsi[ch][g] ? 1 : 0]++;
      for (int gr = 0; gr < 2; gr++) {
        int any_sf = 0;
        for (int b = 0; b < 21; b++)
          if (s->g[gr][ch].sf[b])
            any_sf = 1;
        if (any_sf)
          h->nonzero_granules++;
        if (s->legacy[ch][gr].present) {
          int zerosf[SF_BANDS] = {0};
          double wn = granule_worst(s, ch, gr, s->g[gr][ch].l3_enc,
                                    s->g[gr][ch].tt.quantizerStepSize,
                                    s->g[gr][ch].sf);
          double wo = granule_worst(s, ch, gr, s->legacy[ch][gr].l3_enc,
                                    s->legacy[ch][gr].quantizerStepSize,
                                    zerosf);
          if (wn < wo - 1e-9) {
            h->strict_improvements++;
            if (h->strict_improvements == 1) {
              int first_band = -1;
              for (int b = 0; b < SF_BANDS; b++) {
                double ratio =
                    s->ratios[((gr * s->channels + ch) * SF_BANDS) + b];
                double nn = band_noise(
                    s, ch, gr, b, s->g[gr][ch].l3_enc,
                    s->g[gr][ch].tt.quantizerStepSize, s->g[gr][ch].sf,
                    ratio);
                double no = band_noise(
                    s, ch, gr, b, s->legacy[ch][gr].l3_enc,
                    s->legacy[ch][gr].quantizerStepSize, zerosf, ratio);
                if (fabs(nn - no) > 1e-9) {
                  first_band = b;
                  break;
                }
              }
              h->first_improvement_band = first_band;
              h->first_oldworst = wo;
              h->first_newworst = wn;
            }
          }
        }
      }
    }
    h->verified++;
  }
}

static void harness_finish(harness_t *h, shine_t enc) {
  const unsigned char *frame;
  int flen;

  int n;
  unsigned char *tail = shine_flush(enc, &n);
  stream_append(&h->sb, tail, n);
  harness_drain(h);
  if (h->verified < h->nsnaps - 1)
    fail("verified %d of %d logical frames", h->verified, h->nsnaps);
  /* A full frame still buffered means a snapshot went unverified; a shorter
   * remainder is just the sub-word tail shine keeps in its bit cache. */
  if (stream_next_frame(&h->sb, &frame, &flen))
    fail("a complete frame remains stranded (snaps=%d verified=%d)",
         h->nsnaps, h->verified);
}

/* ---- Input generation -------------------------------------------------- */

static void make_pcm(int16_t *buf, int channels, int frame, int silent) {
  for (int ch = 0; ch < channels; ch++)
    for (int i = 0; i < GRANULE_SIZE * 2; i++) {
      int t = frame * 1152 + i;
      int16_t v;
      if (silent)
        v = 0;
      else
        v = (int16_t)(14000.0 * sin(t * 0.31 + ch * 1.3) +
                      7000.0 * sin(t * 0.017 + ch * 0.7) +
                      3000.0 * sin(t * 0.113 + frame));
      buf[i * channels + ch] = v;
    }
}

static void make_ratios(double *ratios, int channels, int frame) {
  static const double profile[22] = {
      0.010, 0.012, 0.015, 0.020, 0.028, 0.040, 0.060, 0.090,
      0.130, 0.180, 0.240, 0.310, 0.390, 0.470, 0.550, 0.630,
      0.700, 0.770, 0.830, 0.890, 0.950, 1.000};
  for (int gr = 0; gr < 2; gr++)
    for (int ch = 0; ch < channels; ch++)
      for (int b = 0; b < SF_BANDS; b++) {
        double r = profile[b];
        /* Switch thresholds between frames and make granule 1 differ in
         * SCFSI group 1 (bands 6..10) so reuse cannot be blanket. */
        if (frame % 2)
          r = 1.01 - r;
        if (gr == 1 && b >= 6 && b <= 10)
          r = r * 0.5 + 0.05;
        if (r > 1.0)
          r = 1.0;
        if (r <= 0.0)
          r = 1e-6;
        ratios[((gr * channels + ch) * SF_BANDS) + b] = r;
      }
}

static shine_t open_enc(int samplerate, int bitr, int channels, int mode) {
  shine_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels = channels;
  cfg.wave.samplerate = samplerate;
  cfg.mpeg.mode = mode;
  cfg.mpeg.bitr = bitr;
  return shine_initialise(&cfg);
}

int main(void) {
  /* ---- Case 1: planar stereo, 48 kHz, non-uniform thresholds ---------- */
  {
    shine_t enc = open_enc(48000, 128, 2, STEREO);
    if (!enc)
      fail("could not initialize stereo encoder");
    harness_t h;
    harness_init(&h);
    int16_t inter[2304], left[1152], right[1152];
    int16_t *planes[2] = {left, right};
    double ratios[88];

    for (int f = 0; f < 8; f++) {
      make_pcm(inter, 2, f, f == 4);
      for (int i = 0; i < 1152; i++) {
        left[i] = inter[2 * i];
        right[i] = inter[2 * i + 1];
      }
      make_ratios(ratios, 2, f);
      int n = -99;
      unsigned char *out =
          shine_encode_buffer_masked(enc, planes, ratios, 88, &n);
      if (!out || n <= 0) {
        fail("masked encode failed on frame %d (n=%d)", f, n);
        break;
      }
      stream_append(&h.sb, out, n);
      harness_submit(&h, enc, ratios);
      harness_drain(&h);
    }
    harness_finish(&h, enc);
    harness_done(&h);
    shine_close(enc);

    if (!h.nonzero_granules)
      fail("no non-zero scalefactors were ever produced");
    if (!h.strict_improvements)
      fail("no strict worst-noise improvement observed");
    if (h.strict_improvements)
      printf("improvement: legacy_worst=%.6g masked_worst=%.6g delta=%.6g "
             "first_band=%d\n",
             h.first_oldworst, h.first_newworst,
             h.first_oldworst - h.first_newworst, h.first_improvement_band);
    printf("case1 stereo48: strict improvements=%d granules with sf>0=%d\n",
           h.strict_improvements, h.nonzero_granules);
    for (int ch = 0; ch < 2; ch++)
      for (int g = 0; g < 4; g++) {
        printf("  scfsi ch%d group%d (bands %d..%d): reused=%d explicit=%d\n",
               ch, g, g == 0 ? 0 : g == 1 ? 6 : g == 2 ? 11 : 16,
               g == 0 ? 5 : g == 1 ? 10 : g == 2 ? 15 : 20,
               h.scfsi_seen[ch][g][1], h.scfsi_seen[ch][g][0]);
        /* Every one of the four groups must be seen both reused and written
         * explicitly, and reuse must coincide with exact group equality
         * (that equality is checked inside verify_frame). */
        if (!h.scfsi_seen[ch][g][0] || !h.scfsi_seen[ch][g][1])
          fail("scfsi group %d not exercised in both states (ch%d)", g, ch);
      }
  }

  /* ---- Case 2: interleaved entry is byte-identical to planar ---------- */
  {
    shine_t ep = open_enc(48000, 128, 2, STEREO);
    shine_t ei = open_enc(48000, 128, 2, STEREO);
    harness_t hp, hi;
    harness_init(&hp);
    harness_init(&hi);
    int16_t inter[2304], left[1152], right[1152];
    int16_t *planes[2] = {left, right};
    double ratios[88];
    for (int f = 0; f < 4; f++) {
      make_pcm(inter, 2, f, 0);
      for (int i = 0; i < 1152; i++) {
        left[i] = inter[2 * i];
        right[i] = inter[2 * i + 1];
      }
      make_ratios(ratios, 2, f);
      int np = 0, ni = 0;
      unsigned char *op =
          shine_encode_buffer_masked(ep, planes, ratios, 88, &np);
      unsigned char *oi = shine_encode_buffer_interleaved_masked(
          ei, inter, ratios, 88, &ni);
      if (np != ni || memcmp(op, oi, np))
        fail("planar/interleaved masked outputs differ on frame %d", f);
      stream_append(&hp.sb, op, np);
      stream_append(&hi.sb, oi, ni);
      harness_submit(&hp, ep, ratios);
      harness_submit(&hi, ei, ratios);
    }
    harness_finish(&hp, ep);
    harness_finish(&hi, ei);
    harness_done(&hp);
    harness_done(&hi);
    shine_close(ep);
    shine_close(ei);
    if (hp.sb.len != hi.sb.len ||
        memcmp(hp.sb.data, hi.sb.data, hp.sb.len))
      fail("accumulated planar/interleaved streams differ");
    printf("case2 interleaved: byte-identical to planar\n");
  }

  /* ---- Case 3: mono 32 kHz -------------------------------------------- */
  {
    shine_t enc = open_enc(32000, 64, 1, MONO);
    if (!enc)
      fail("could not initialize mono encoder");
    harness_t h;
    harness_init(&h);
    int16_t mono[1152];
    int16_t *mplanes[1] = {mono};
    double ratios[44];
    for (int f = 0; f < 4; f++) {
      make_pcm(mono, 1, f, 0);
      make_ratios(ratios, 1, f);
      int n;
      unsigned char *out =
          shine_encode_buffer_masked(enc, mplanes, ratios, 44, &n);
      if (!out) {
        fail("mono masked encode failed frame %d", f);
        break;
      }
      stream_append(&h.sb, out, n);
      harness_submit(&h, enc, ratios);
      harness_drain(&h);
    }
    harness_finish(&h, enc);
    harness_done(&h);
    shine_close(enc);
    printf("case3 mono32: parsed and reconstructed\n");
  }

  /* ---- Case 4: silence frame gives exact zero reconstruction ---------- */
  {
    shine_t enc = open_enc(48000, 128, 2, STEREO);
    frame_snapshot_t s;
    int16_t zero[2304];
    int16_t *zplanes[2];
    double ratios[88];
    memset(zero, 0, sizeof(zero));
    zplanes[0] = zero;
    zplanes[1] = zero + 1152;
    make_ratios(ratios, 2, 0);
    int n;
    unsigned char *out =
        shine_encode_buffer_masked(enc, zplanes, ratios, 88, &n);
    if (!out)
      fail("silence encode failed");
    else {
      snapshot_capture(enc, ratios, &s);
      parsed_frame_t pf;
      parse_frame(&s, out, n, &pf);
      verify_frame(&s, out, n, &pf);
      for (int ch = 0; ch < 2; ch++)
        for (int gr = 0; gr < 2; gr++)
          for (int i = 0; i < GRANULE_SIZE; i++)
            if (s.g[gr][ch].l3_enc[i] != 0 || s.mdct_freq[ch][gr][i] != 0) {
              fail("silence frame not exactly zero (ch%d gr%d i%d)", ch, gr,
                   i);
              goto silence_done;
            }
    }
  silence_done:
    shine_close(enc);
    printf("case4 silence: exact zero spectrum and quantization\n");
  }

  /* ---- Case 5: low capacity fallback stays legal and <= legacy -------- */
  {
    shine_t enc = open_enc(48000, 32, 2, STEREO);
    harness_t h;
    harness_init(&h);
    int16_t inter[2304], left[1152], right[1152];
    int16_t *planes[2] = {left, right};
    double ratios[88];
    for (int f = 0; f < 4; f++) {
      make_pcm(inter, 2, f + 10, 0);
      for (int i = 0; i < 1152; i++) {
        left[i] = inter[2 * i];
        right[i] = inter[2 * i + 1];
      }
      make_ratios(ratios, 2, f);
      int n;
      unsigned char *out =
          shine_encode_buffer_masked(enc, planes, ratios, 88, &n);
      if (!out) {
        fail("low capacity encode failed frame %d", f);
        break;
      }
      stream_append(&h.sb, out, n);
      harness_submit(&h, enc, ratios);
      harness_drain(&h);
    }
    harness_finish(&h, enc);
    harness_done(&h);
    shine_close(enc);
    printf("case5 low-capacity 32kbps: legal frames, worst noise <= legacy\n");
  }

  /* ---- Case 6: 44.1 kHz exercises both padding values ----------------- */
  /* Shine's slot-lag padding stays at one value for long runs (the first
   * unpadded slot arrives around frame 27), so this case spans 30 frames. */
  {
    shine_t enc = open_enc(44100, 128, 2, STEREO);
    harness_t h;
    harness_init(&h);
    int16_t inter[2304], left[1152], right[1152];
    int16_t *planes[2] = {left, right};
    double ratios[88];
    for (int f = 0; f < 30; f++) {
      make_pcm(inter, 2, f, 0);
      for (int i = 0; i < 1152; i++) {
        left[i] = inter[2 * i];
        right[i] = inter[2 * i + 1];
      }
      make_ratios(ratios, 2, f);
      int n;
      unsigned char *out =
          shine_encode_buffer_masked(enc, planes, ratios, 88, &n);
      if (!out) {
        fail("44.1k encode failed frame %d", f);
        break;
      }
      stream_append(&h.sb, out, n);
      harness_submit(&h, enc, ratios);
      harness_drain(&h);
    }
    harness_finish(&h, enc);
    harness_done(&h);
    shine_close(enc);
    if (!h.pad_seen[0] || !h.pad_seen[1])
      fail("did not observe both padding values (got %d/%d)", h.pad_seen[0],
           h.pad_seen[1]);
    printf("case6 44.1kHz: unpadded=%d padded=%d frames\n", h.pad_seen[0],
           h.pad_seen[1]);
  }

  /* ---- Case 7: invalid requests do not advance analysis history ------- */
  {
    shine_t good = open_enc(48000, 128, 2, STEREO);
    shine_t ref = open_enc(48000, 128, 2, STEREO);
    int16_t inter[2304];
    int16_t left[1152], right[1152];
    int16_t *planes[2] = {left, right};
    double ratios[88];
    make_pcm(inter, 2, 0, 0);
    for (int i = 0; i < 1152; i++) {
      left[i] = inter[2 * i];
      right[i] = inter[2 * i + 1];
    }
    make_ratios(ratios, 2, 0);

    int n = 0;
    unsigned char *r;
    r = shine_encode_buffer_masked(good, planes, ratios, 87, &n);
    if (r || n != -1)
      fail("bad ratio_count accepted");
    r = shine_encode_buffer_masked(good, planes, NULL, 4, &n);
    if (r || n != -1)
      fail("NULL ratios with nonzero count accepted");
    ratios[10] = NAN;
    r = shine_encode_buffer_masked(good, planes, ratios, 88, &n);
    if (r || n != -1)
      fail("NaN ratio accepted");
    ratios[10] = INFINITY;
    r = shine_encode_buffer_masked(good, planes, ratios, 88, &n);
    if (r || n != -1)
      fail("infinite ratio accepted");
    ratios[10] = 0.0;
    r = shine_encode_buffer_masked(good, planes, ratios, 88, &n);
    if (r || n != -1)
      fail("zero ratio accepted");
    ratios[10] = 1.0001;
    r = shine_encode_buffer_masked(good, planes, ratios, 88, &n);
    if (r || n != -1)
      fail("ratio > 1 accepted");
    make_ratios(ratios, 2, 0);

    shine_t js = open_enc(48000, 128, 2, JOINT_STEREO);
    r = shine_encode_buffer_masked(js, planes, ratios, 88, &n);
    if (r || n != -1)
      fail("joint stereo masked request accepted");
    shine_close(js);

    int ng, nr;
    unsigned char *og =
        shine_encode_buffer_masked(good, planes, ratios, 88, &ng);
    unsigned char *oref =
        shine_encode_buffer_masked(ref, planes, ratios, 88, &nr);
    if (!og || !oref || ng != nr || memcmp(og, oref, ng))
      fail("retry after invalid request does not match fresh encoder");

    int nl, nl2;
    unsigned char *ol = shine_encode_buffer(good, planes, &nl);
    unsigned char *ol2 = shine_encode_buffer(ref, planes, &nl2);
    if (!ol || !ol2 || nl != nl2 || memcmp(ol, ol2, nl))
      fail("legacy encode after invalid request diverged");
    shine_close(good);
    shine_close(ref);
    printf("case7 errors: all rejected with NULL/-1, history intact\n");
  }

  /* ---- Case 8: NULL/0 masked call is byte-identical to legacy --------- */
  {
    shine_t em = open_enc(48000, 128, 2, STEREO);
    shine_t el = open_enc(48000, 128, 2, STEREO);
    harness_t hm, hl;
    harness_init(&hm);
    harness_init(&hl);
    int16_t inter[2304], left[1152], right[1152];
    int16_t *planes[2] = {left, right};
    for (int f = 0; f < 4; f++) {
      make_pcm(inter, 2, f, 0);
      for (int i = 0; i < 1152; i++) {
        left[i] = inter[2 * i];
        right[i] = inter[2 * i + 1];
      }
      int nm, nl;
      unsigned char *om =
          shine_encode_buffer_masked(em, planes, NULL, 0, &nm);
      unsigned char *ol = shine_encode_buffer(el, planes, &nl);
      if (nm != nl || memcmp(om, ol, nm))
        fail("NULL/0 masked call differs from legacy on frame %d", f);
      stream_append(&hm.sb, om, nm);
      stream_append(&hl.sb, ol, nl);
    }
    int fm, fl;
    stream_append(&hm.sb, shine_flush(em, &fm), fm);
    stream_append(&hl.sb, shine_flush(el, &fl), fl);
    harness_done(&hm);
    harness_done(&hl);
    shine_close(em);
    shine_close(el);
    if (hm.sb.len != hl.sb.len ||
        memcmp(hm.sb.data, hl.sb.data, hm.sb.len))
      fail("NULL/0 accumulated stream differs from legacy");
    printf("case8 NULL/0: byte-identical to legacy entry points\n");
  }

  /* ---- Case 9: threshold switching between frames stays legal --------- */
  {
    shine_t enc = open_enc(48000, 96, 1, MONO);
    harness_t h;
    harness_init(&h);
    int16_t mono[1152];
    double ratios[44];
    for (int f = 0; f < 6; f++) {
      make_pcm(mono, 1, f, f == 2);
      make_ratios(ratios, 1, f);
      int n;
      unsigned char *out = shine_encode_buffer_interleaved_masked(
          enc, mono, ratios, 44, &n);
      if (!out) {
        fail("threshold switch encode failed frame %d", f);
        break;
      }
      stream_append(&h.sb, out, n);
      harness_submit(&h, enc, ratios);
      harness_drain(&h);
    }
    harness_finish(&h, enc);
    harness_done(&h);
    shine_close(enc);
    printf("case9 threshold switching: all frames legal\n");
  }

  if (first_deviation)
    printf("FIRST DEVIATION: %s\n", first_deviation);
  if (failures) {
    printf("MASKED QUANTIZATION TESTS FAILED: %d\n", failures);
    return 1;
  }
  printf("MASKED QUANTIZATION TESTS PASSED\n");
  return 0;
}
