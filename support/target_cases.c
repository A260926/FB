/* target_cases.c
 *
 * In-tree tests for the explicit MPEG-1 long/short window encoding path.
 * Registered through cases.h; this file intentionally has no main().
 *
 * Three groups:
 *   windows   - independent double direct-sum MDCT references against the
 *               read-only pre-aliasing observation points, plus checks that
 *               aliasing reduction is applied to the correct block types.
 *   bitstream - continuous-stream frame/side-info parse with a private bit
 *               reader and Huffman walker, recovering scale factors, signs
 *               and the integer spectrum, inverse-reordering short blocks
 *               and comparing against quantized observation snapshots.
 *   state     - cross-frame window state machine, interleaved instances,
 *               dual PCM entries, error-then-retry semantics, sentinels and
 *               complete-frame delivery.
 */

#include "cases.h"
#include "layer3.h"
#include "l3obs.h"
#include "l3window.h"
#include "huffman.h"
#include "tables.h"
#include "types.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SENTINEL 0xA5
#define GUARD 16
#define REF_TOL 64
#define MAX_SNAP_FRAMES 40

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void fail_line(const char *what, int line) {
  fprintf(stderr, "helper failure: %s (line %d)\n", what, line);
  exit(47);
}

static void *guard_alloc(size_t bytes) {
  unsigned char *base = malloc(bytes + 2 * GUARD);
  if (!base)
    exit(48);
  memset(base, SENTINEL, GUARD);
  memset(base + GUARD + bytes, SENTINEL, GUARD);
  return base + GUARD;
}

static int guard_ok(const void *p, size_t bytes) {
  const unsigned char *base = (const unsigned char *)p - GUARD;
  size_t i;
  for (i = 0; i < GUARD; i++)
    if (base[i] != SENTINEL || base[GUARD + bytes + i] != SENTINEL)
      return 0;
  return 1;
}

static void guard_free(void *p) { free((unsigned char *)p - GUARD); }

static void fill_pcm(int16_t *pcm, int count, unsigned seed, int amp) {
  int i;
  for (i = 0; i < count; i++) {
    seed = seed * 1103515245u + 12345u;
    pcm[i] = (int16_t)((int)((seed >> 16) % (2 * amp + 1)) - amp);
  }
}

static shine_t make_encoder(int channels, int rate, int mode) {
  shine_config_t c;
  memset(&c, 0, sizeof(c));
  c.wave.channels = channels;
  c.wave.samplerate = rate;
  shine_set_config_mpeg_defaults(&c.mpeg);
  c.mpeg.mode = mode;
  return shine_initialise(&c);
}

/* MSB-first bit reader. */
typedef struct {
  const unsigned char *p;
  int bit;
} bitreader_t;

static void br_init(bitreader_t *br, const unsigned char *p, int byte_offset) {
  br->p = p;
  br->bit = byte_offset * 8;
}

static unsigned br_get(bitreader_t *br, int n) {
  unsigned v = 0;
  int i;
  for (i = 0; i < n; i++) {
    unsigned bit = (br->p[br->bit / 8] >> (7 - (br->bit % 8))) & 1u;
    v = (v << 1) | bit;
    br->bit++;
  }
  return v;
}

static unsigned br_peek(bitreader_t *br, int n) {
  int save = br->bit;
  unsigned v = br_get(br, n);
  br->bit = save;
  return v;
}

/* Locate the real MPEG-1 Layer III sync word inside a delivered block. The
 * 32-bit cache can leave a few leading bytes from the previous frame, and
 * audio data can contain the raw 0xFFFA pattern by chance, so every
 * candidate is validated against the expected header fields. */
static int locate_sync(const unsigned char *block, int len) {
  int i;
  for (i = 0; i + 3 < len; i++) {
    if (block[i] != 0xFF || (block[i + 1] & 0xFE) != 0xFA)
      continue;
    unsigned version = (block[i + 1] >> 3) & 3;
    unsigned layer = (block[i + 1] >> 1) & 3;
    unsigned protection = block[i + 1] & 1;
    unsigned bitrate_idx = block[i + 2] >> 4;
    unsigned samplerate_idx = (block[i + 2] >> 2) & 3;
    if (version == 3 /* MPEG-I */ && layer == 1 /* Layer III */ &&
        protection == 1 /* no CRC */ && bitrate_idx > 0 &&
        bitrate_idx < 15 && samplerate_idx < 3)
      return i;
  }
  return -1;
}

/* ------------------------------------------------------------------ */
/* Independent Huffman decoder (uses only the published code tables)  */
/* ------------------------------------------------------------------ */

static const struct huffcodetab *huff_table(int t) {
  return &shine_huffman_table[t];
}

static void huff_read_pair(bitreader_t *br, int table, int *out_x,
                           int *out_y) {
  const struct huffcodetab *h = huff_table(table);
  int nbits, x, y;

  for (nbits = 1; nbits <= 32; nbits++) {
    unsigned code = br_peek(br, nbits);
    for (x = 0; x < (int)h->xlen; x++)
      for (y = 0; y < (int)h->ylen; y++) {
        int idx = x * h->ylen + y;
        if ((int)h->hlen[idx] == nbits && h->table[idx] == code) {
          br->bit += nbits;
          if (table > 15) {
            int vx = x, vy = y;
            if (vx > 14)
              vx = 15 + (int)br_get(br, h->linbits);
            if (vx != 0 && br_get(br, 1))
              vx = -vx;
            if (vy > 14)
              vy = 15 + (int)br_get(br, h->linbits);
            if (vy != 0 && br_get(br, 1))
              vy = -vy;
            *out_x = vx;
            *out_y = vy;
          } else {
            int vx = x, vy = y;
            if (vx != 0 && br_get(br, 1))
              vx = -vx;
            if (vy != 0 && br_get(br, 1))
              vy = -vy;
            *out_x = vx;
            *out_y = vy;
          }
          return;
        }
      }
  }
  fail_line("no huffman prefix matched", __LINE__);
}

static void huff_read_count1(bitreader_t *br, int count1table, int out[4]) {
  const struct huffcodetab *h = huff_table(count1table);
  int nbits, p;

  for (nbits = 1; nbits <= 32; nbits++) {
    unsigned code = br_peek(br, nbits);
    for (p = 0; p < 16; p++)
      if ((int)h->hlen[p] == nbits && h->table[p] == code) {
        int vals[4], k;
        br->bit += nbits;
        vals[0] = p & 1;
        vals[1] = (p >> 1) & 1;
        vals[2] = (p >> 2) & 1;
        vals[3] = (p >> 3) & 1;
        for (k = 0; k < 4; k++)
          if (vals[k] != 0 && br_get(br, 1))
            vals[k] = -vals[k];
        memcpy(out, vals, sizeof(vals));
        return;
      }
  }
  fail_line("no count1 prefix matched", __LINE__);
}

typedef struct {
  unsigned part2_3_length, big_values, count1, global_gain;
  unsigned scalefac_compress;
  int switched;
  unsigned block_type, mixed;
  unsigned table_select[3];
  unsigned region0_count, region1_count;
  unsigned subblock_gain[3];
  unsigned count1table_select;
} parsed_gi_t;

static void parse_record(bitreader_t *br, parsed_gi_t *g) {
  int r;
  g->part2_3_length = br_get(br, 12);
  g->big_values = br_get(br, 9);
  g->global_gain = br_get(br, 8);
  g->scalefac_compress = br_get(br, 4);
  g->switched = br_get(br, 1);
  g->mixed = 0;
  g->block_type = 0;
  g->table_select[0] = g->table_select[1] = g->table_select[2] = 0;
  g->region0_count = g->region1_count = 0;
  g->subblock_gain[0] = g->subblock_gain[1] = g->subblock_gain[2] = 0;

  if (g->switched) {
    g->block_type = br_get(br, 2);
    g->mixed = br_get(br, 1);
    g->table_select[1] = br_get(br, 5);
    g->table_select[2] = br_get(br, 5);
    g->subblock_gain[0] = br_get(br, 3);
    g->subblock_gain[1] = br_get(br, 3);
    g->subblock_gain[2] = br_get(br, 3);
  } else {
    for (r = 0; r < 3; r++)
      g->table_select[r] = br_get(br, 5);
    g->region0_count = br_get(br, 4);
    g->region1_count = br_get(br, 3);
  }
  br_get(br, 1); /* preflag */
  br_get(br, 1); /* scalefac_scale */
  g->count1table_select = br_get(br, 1);
}

static void parse_all_records(bitreader_t *br, int channels,
                              parsed_gi_t recs[2][MAX_CHANNELS]) {
  int gr, ch;
  for (gr = 0; gr < 2; gr++)
    for (ch = 0; ch < channels; ch++)
      parse_record(br, &recs[gr][ch]);
}

static void decode_spectrum(bitreader_t *br, const parsed_gi_t *g,
                            int samplerate_index, int stream[GRANULE_SIZE]) {
  const int *long_bands =
      &shine_scale_fact_band_index[samplerate_index][0];
  int region1_start, region2_start;
  int i, x, y;
  int quad[4];

  memset(stream, 0, sizeof(int) * GRANULE_SIZE);

  if (g->switched && g->block_type == SHINE_BLOCK_SHORT && !g->mixed) {
    region1_start = 0;
    region2_start = 3 * shine_short_band_offsets(samplerate_index)[8];
  } else if (g->switched) {
    region1_start = long_bands[8];
    region2_start = long_bands[12];
  } else {
    region1_start = long_bands[g->region0_count + 1];
    region2_start = long_bands[g->region0_count + 1 + g->region1_count + 1];
  }

  for (i = 0; i < (int)(g->big_values * 2); i += 2) {
    int table;
    if (g->switched &&
        g->block_type != SHINE_BLOCK_SHORT) {
      /* switched long block (start/stop): three implicit regions. Region 0
       * has no transmitted table (table_select[0] stays 0, values are
       * zero), region 1 uses table_select[1], region 2 uses [2]. */
      if (i < region1_start)
        table = 0;
      else if (i < region2_start)
        table = g->table_select[1];
      else
        table = g->table_select[2];
    } else if (g->switched) {
      table = (i >= region2_start) ? g->table_select[2] : g->table_select[1];
    } else {
      int idx = (i >= region1_start) + (i >= region2_start);
      table = g->table_select[idx];
    }
    if (table == 0) {
      stream[i] = 0;
      stream[i + 1] = 0;
      continue;
    }
    huff_read_pair(br, table, &x, &y);
    stream[i] = x;
    stream[i + 1] = y;
  }

  for (i = (int)(g->big_values * 2);
       i < (int)(g->big_values * 2 + g->count1 * 4); i += 4) {
    huff_read_count1(br, 32 + g->count1table_select, quad);
    stream[i] = quad[0];
    stream[i + 1] = quad[1];
    stream[i + 2] = quad[2];
    stream[i + 3] = quad[3];
  }
}

/* Independent natural<->stream permutation:
 * natural[192*window + freq] = stream[3*freq + window]. */
static void independent_unreorder(const int stream[GRANULE_SIZE],
                                  int natural[GRANULE_SIZE]) {
  int freq, window;
  for (freq = 0; freq < 192; freq++)
    for (window = 0; window < SHINE_SHORT_WINDOWS; window++)
      natural[192 * window + freq] = stream[3 * freq + window];
}

/* ================================================================== */
/* windows group                                                       */
/* ================================================================== */

static double ref_long_coef(const int32_t in[36], int m, int block_type) {
  double acc = 0.0;
  int k;
  for (k = 0; k < 36; k++) {
    double shape;
    if (block_type == SHINE_BLOCK_START) {
      if (k < 18)
        shape = 1.0;
      else if (k < 24)
        shape = sin(M_PI / 12.0 * (k - 18 + 0.5));
      else if (k < 30)
        shape = 0.0;
      else
        shape = sin(M_PI / 12.0 * (k - 30 + 0.5));
    } else if (block_type == SHINE_BLOCK_STOP) {
      if (k < 6)
        shape = sin(M_PI / 12.0 * (k + 0.5));
      else if (k < 12)
        shape = 0.0;
      else if (k < 18)
        shape = sin(M_PI / 12.0 * (k - 12 + 0.5));
      else
        shape = 1.0;
    } else {
      shape = 1.0;
    }
    acc += (double)in[k] * shape * sin(M_PI / 36.0 * (k + 0.5)) *
           cos((M_PI / 72.0) * (2 * k + 19) * (2 * m + 1));
  }
  return acc;
}

static double ref_short_coef(const int32_t in[36], int window, int m) {
  const int offset = 6 * (window + 1);
  double acc = 0.0;
  int n;
  for (n = 0; n < 12; n++) {
    double win = sin(M_PI / 12.0 * (n + 0.5));
    acc += (double)in[offset + n] * win *
           cos((M_PI / 24.0) * (2 * n + 7) * (2 * m + 1));
  }
  return acc;
}

static int inputs_bounded(const shine_global_config *o, int gr, int band) {
  int k;
  for (k = 0; k < 36; k++)
    if (labs(o->obs_mdct_in[0][gr][band][k]) > (1 << 24))
      return 0;
  return 1;
}

static void fill_asymmetric(int16_t *pcm, int count) {
  int i;
  for (i = 0; i < count; i++)
    pcm[i] = (int16_t)((i % 97) * 12 - 580);
  pcm[37] = 4000;
  pcm[41] = -3500;
  pcm[600] = 4000;
  pcm[1100] = -4000;
}

TASK_CASE("windows", short_three_windows_odd_bands) {
  shine_t s = make_encoder(PCM_MONO, 48000, MONO);
  int16_t *pcm = guard_alloc(1152 * sizeof(int16_t));
  int n, band, gr, w, m;
  const shine_global_config *o;
  uint8_t blocks[2] = {SHINE_BLOCK_START, SHINE_BLOCK_SHORT};
  const int odd_bands[3] = {5, 13, 27};

  fill_asymmetric(pcm, 1152);
  TASK_CHECK(shine_encode_buffer_blocks(s, (int16_t *[]){pcm}, blocks, 2, &n) != NULL);
  TASK_CHECK(guard_ok(pcm, 1152 * sizeof(int16_t)));
  o = shine_test_observations(s);
  TASK_CHECK(o->obs_valid);
  TASK_CHECK(o->obs_block_type[0][1] == SHINE_BLOCK_SHORT);
  TASK_CHECK(o->obs_aliasing_applied[0][1] == 0);

  gr = 1;
  for (band = 0; band < 32; band++) {
    TASK_CHECK(inputs_bounded(o, gr, band));
    for (w = 0; w < 3; w++)
      for (m = 0; m < 6; m++) {
        double ref = 0.5 * ref_short_coef(o->obs_mdct_in[0][gr][band], w, m);
        int32_t got = o->obs_mdct_pre_alias[0][gr][band][6 * w + m];
        TASK_CHECK(labs((long)got - (long)llround(ref)) <= REF_TOL);
      }
  }
  /* each of the three short windows nonzero over at least one odd band */
  for (w = 0; w < 3; w++) {
    int nz = 0, bi;
    for (bi = 0; bi < 3; bi++)
      for (m = 0; m < 6; m++)
        nz += (o->obs_mdct_pre_alias[0][gr][odd_bands[bi]][6 * w + m] != 0);
    TASK_CHECK(nz > 0);
  }
  for (int bi = 0; bi < 3; bi++) {
    band = odd_bands[bi];
    for (w = 0; w < 3; w++)
      for (m = 0; m < 6; m++) {
        double ref = 0.5 * ref_short_coef(o->obs_mdct_in[0][gr][band], w, m);
        int32_t got = o->obs_mdct_pre_alias[0][gr][band][6 * w + m];
        TASK_CHECK(labs((long)got - (long)llround(ref)) <= REF_TOL);
      }
  }
  guard_free(pcm);
  shine_close(s);
}

TASK_CASE("windows", start_stop_transition_windows) {
  shine_t s = make_encoder(PCM_MONO, 48000, MONO);
  int16_t *pcm = guard_alloc(1152 * sizeof(int16_t));
  int n, band, m, gr;
  const shine_global_config *o;
  uint8_t f1[2] = {SHINE_BLOCK_START, SHINE_BLOCK_SHORT};
  uint8_t f2[2] = {SHINE_BLOCK_STOP, SHINE_BLOCK_NORMAL};

  fill_pcm(pcm, 1152, 777, 3000);
  TASK_CHECK(shine_encode_buffer_blocks(s, (int16_t *[]){pcm}, f1, 2, &n));
  fill_pcm(pcm, 1152, 999, 3000);
  TASK_CHECK(shine_encode_buffer_blocks(s, (int16_t *[]){pcm}, f2, 2, &n));
  o = shine_test_observations(s);
  TASK_CHECK(o->obs_block_type[0][0] == SHINE_BLOCK_STOP);
  TASK_CHECK(o->obs_block_type[0][1] == SHINE_BLOCK_NORMAL);
  TASK_CHECK(o->obs_aliasing_applied[0][0] == 1);
  TASK_CHECK(o->obs_aliasing_applied[0][1] == 1);

  gr = 0;
  for (band = 0; band < 32; band++) {
    TASK_CHECK(inputs_bounded(o, gr, band));
    for (m = 0; m < 18; m++) {
      double ref = 0.5 * ref_long_coef(o->obs_mdct_in[0][gr][band], m,
                                       SHINE_BLOCK_STOP);
      int32_t got = o->obs_mdct_pre_alias[0][gr][band][m];
      TASK_CHECK(labs((long)got - (long)llround(ref)) <= REF_TOL);
    }
  }
  gr = 1;
  for (band = 0; band < 32; band++)
    for (m = 0; m < 18; m++) {
      double ref = 0.5 * ref_long_coef(o->obs_mdct_in[0][gr][band], m,
                                       SHINE_BLOCK_NORMAL);
      int32_t got = o->obs_mdct_pre_alias[0][gr][band][m];
      TASK_CHECK(labs((long)got - (long)llround(ref)) <= REF_TOL);
    }
  guard_free(pcm);
  shine_close(s);
}

static void ref_butterfly(int32_t pre[32][18], double out[32][18]) {
  static const double c[8] = {-0.6,    -0.535, -0.33,  -0.185,
                              -0.095, -0.041, -0.0142, -0.0037};
  int band, i;
  for (band = 0; band < 32; band++)
    for (i = 0; i < 18; i++)
      out[band][i] = pre[band][i];
  for (band = 1; band < 32; band++)
    for (i = 0; i < 8; i++) {
      double cs = 1.0 / sqrt(1.0 + c[i] * c[i]);
      double ca = c[i] / sqrt(1.0 + c[i] * c[i]);
      double lo = out[band][i], hi = out[band - 1][17 - i];
      out[band][i] = lo * cs - hi * ca;
      out[band - 1][17 - i] = lo * ca + hi * cs;
    }
}

TASK_CASE("windows", aliasing_applied_by_type) {
  shine_t s = make_encoder(PCM_MONO, 48000, MONO);
  int16_t *pcm = guard_alloc(1152 * sizeof(int16_t));
  int n, band, m, gr;
  const shine_global_config *o;
  double ref[32][18];
  uint8_t f1[2] = {SHINE_BLOCK_START, SHINE_BLOCK_SHORT};

  fill_pcm(pcm, 1152, 4242, 4000);
  TASK_CHECK(shine_encode_buffer_blocks(s, (int16_t *[]){pcm}, f1, 2, &n));
  o = shine_test_observations(s);

  gr = 1;
  for (band = 0; band < 32; band++)
    for (m = 0; m < 6; m++)
      for (int w = 0; w < 3; w++)
        TASK_CHECK(o->mdct_freq[0][gr][192 * w + 6 * band + m] ==
                   o->obs_mdct_pre_alias[0][gr][band][6 * w + m]);

  gr = 0;
  {
    int32_t pre[32][18];
    for (band = 0; band < 32; band++)
      for (m = 0; m < 18; m++)
        pre[band][m] = o->obs_mdct_pre_alias[0][gr][band][m];
    ref_butterfly(pre, ref);
  }
  for (band = 0; band < 32; band++)
    for (m = 0; m < 18; m++) {
      int32_t got = o->mdct_freq[0][gr][18 * band + m];
      TASK_CHECK(labs((long)got - (long)llround(ref[band][m])) <= 2);
    }
  guard_free(pcm);
  shine_close(s);
}

TASK_CASE("windows", history_across_granules_and_frames) {
  shine_t s = make_encoder(PCM_MONO, 48000, MONO);
  int16_t *pcm = guard_alloc(1152 * sizeof(int16_t));
  int n, f, band, k;
  const shine_global_config *o;
  int32_t prev_in[32][36];
  int have_prev = 0;
  uint8_t normal[2] = {SHINE_BLOCK_NORMAL, SHINE_BLOCK_NORMAL};

  for (f = 0; f < 3; f++) {
    fill_pcm(pcm, 1152, 100 + f, 2500);
    TASK_CHECK(shine_encode_buffer_blocks(s, (int16_t *[]){pcm}, normal, 2, &n));
    o = shine_test_observations(s);
    for (band = 0; band < 32; band++) {
      for (k = 0; k < 18; k++)
        TASK_CHECK(o->obs_mdct_in[0][0][band][k + 18] ==
                   o->obs_mdct_in[0][1][band][k]);
      if (have_prev)
        for (k = 0; k < 18; k++)
          TASK_CHECK(o->obs_mdct_in[0][0][band][k] == prev_in[band][k + 18]);
      memcpy(prev_in[band], o->obs_mdct_in[0][1][band],
             36 * sizeof(int32_t));
    }
    have_prev = 1;
  }
  guard_free(pcm);
  shine_close(s);
}

/* ================================================================== */
/* bitstream group                                                     */
/* ================================================================== */

typedef struct {
  int stream[GRANULE_SIZE];
  int natural[GRANULE_SIZE];
  unsigned big_values;
  unsigned count1;
} snap_gc_t;

typedef struct {
  snap_gc_t gc[2][MAX_CHANNELS];
} snap_frame_t;

static void snapshot_frame(const shine_global_config *o, int channels,
                           snap_frame_t *snap) {
  int gr, ch, i;
  for (gr = 0; gr < 2; gr++)
    for (ch = 0; ch < channels; ch++) {
      for (i = 0; i < GRANULE_SIZE; i++) {
        snap->gc[gr][ch].stream[i] = o->l3_enc[ch][gr][i];
        snap->gc[gr][ch].natural[i] = o->l3_enc_natural[ch][gr][i];
      }
      snap->gc[gr][ch].big_values =
          o->side_info.gr[gr].ch[ch].tt.big_values;
      snap->gc[gr][ch].count1 = o->side_info.gr[gr].ch[ch].tt.count1;
    }
}

/*
 * Independently decode one fully-delivered short-block frame inside the
 * continuous byte stream. A frame is fully delivered only when a later sync
 * word exists: shine's 32-bit bit cache emits each frame's final few bits as
 * leading bytes of the next delivered block, so the frame's logical extent is
 * exactly [sync_byte, next_sync_byte). Only published code tables and the
 * read-only quantized snapshot are consulted.
 */
static void check_stream_frame(const unsigned char *stream, int sync_byte,
                               int next_sync_byte, int rate_index,
                               int channels, const snap_frame_t *snap,
                               int padding_seen[2], int coverage[3]) {
  bitreader_t br;
  parsed_gi_t recs[2][MAX_CHANNELS];
  int main_start_bit, frame_end_bit, gr, ch, rec_index;

  TASK_CHECK(next_sync_byte > sync_byte);
  br_init(&br, stream, sync_byte);
  frame_end_bit = next_sync_byte * 8;

  TASK_CHECK(br_get(&br, 11) == 0x7FF);
  TASK_CHECK(br_get(&br, 2) == MPEG_I);
  TASK_CHECK(br_get(&br, 2) == LAYER_III);
  TASK_CHECK(br_get(&br, 1) == 1);
  TASK_CHECK(br_get(&br, 4) > 0);
  TASK_CHECK(br_get(&br, 2) == (unsigned)rate_index);
  padding_seen[br_get(&br, 1)] = 1;
  br_get(&br, 1);
  br_get(&br, 2); /* mode */
  br_get(&br, 2); /* mode_ext */
  br_get(&br, 1); /* copyright */
  br_get(&br, 1); /* original */
  br_get(&br, 2); /* emphasis */

  TASK_CHECK(br_get(&br, 9) == 0); /* main_data_begin */
  br_get(&br, channels == 2 ? 3 : 5);
  if (channels == 2)
    br_get(&br, 8);
  else
    br_get(&br, 4);

  parse_all_records(&br, channels, recs);
  main_start_bit = br.bit;

  rec_index = 0;
  for (gr = 0; gr < 2; gr++)
    for (ch = 0; ch < channels; ch++, rec_index++) {
      parsed_gi_t *g = &recs[gr][ch];
      int stream_spec[GRANULE_SIZE], natural_spec[GRANULE_SIZE];
      int region_start, huff_end, i;
      int negatives = 0, region_tables = 0;

      TASK_CHECK(g->switched);
      TASK_CHECK(g->block_type == SHINE_BLOCK_SHORT);
      TASK_CHECK(g->mixed == 0);
      TASK_CHECK(g->scalefac_compress == 0);
      TASK_CHECK(shine_slen1_tab[0] == 0 && shine_slen2_tab[0] == 0);
      TASK_CHECK(g->subblock_gain[0] == 0 && g->subblock_gain[1] == 0 &&
                 g->subblock_gain[2] == 0);

      /* count1 is not transmitted in side info; the boundary is part of the
       * quantized geometry. Read the encoder's own boundary from the
       * read-only snapshot and verify the region invariants: every count1
       * symbol is in {-1,0,1} and every rzero symbol is zero. */
      g->count1 = snap->gc[gr][ch].count1;
      TASK_CHECK(g->big_values == snap->gc[gr][ch].big_values);
      {
        const int *sx = snap->gc[gr][ch].stream;
        int bv2 = (int)g->big_values * 2;
        int c1end = bv2 + (int)g->count1 * 4;
        int q;
        TASK_CHECK(c1end <= GRANULE_SIZE);
        for (q = bv2; q < c1end; q++)
          TASK_CHECK(sx[q] >= -1 && sx[q] <= 1);
        for (q = c1end; q + 1 < GRANULE_SIZE; q += 2)
          TASK_CHECK(sx[q] == 0 && sx[q + 1] == 0);
      }
      TASK_CHECK((int)(g->big_values * 2 + g->count1 * 4) <= GRANULE_SIZE);
      if (g->count1)
        coverage[0] = 1;
      if (g->table_select[1])
        region_tables++;
      if (g->table_select[2])
        region_tables++;
      if (region_tables == 2)
        coverage[1] = 1;

      region_start = main_start_bit;
      for (i = 0; i < rec_index; i++) {
        int pg = i / channels, pc = i % channels;
        region_start += recs[pg][pc].part2_3_length;
      }
      br.bit = region_start;
      decode_spectrum(&br, g, rate_index, stream_spec);
      huff_end = region_start + (int)g->part2_3_length;
      TASK_CHECK(br.bit <= huff_end);
      while (br.bit < huff_end)
        TASK_CHECK(br_get(&br, 1) == 1);

      for (i = 0; i < GRANULE_SIZE; i++) {
        TASK_CHECK(stream_spec[i] == snap->gc[gr][ch].stream[i]);
        if (stream_spec[i] < 0)
          negatives++;
      }
      TASK_CHECK(negatives > 0);
      coverage[2] = 1;

      independent_unreorder(stream_spec, natural_spec);
      for (i = 0; i < GRANULE_SIZE; i++)
        TASK_CHECK(natural_spec[i] == snap->gc[gr][ch].natural[i]);

      {
        long energy[3] = {0, 0, 0};
        int ww, ff;
        for (ww = 0; ww < 3; ww++)
          for (ff = 0; ff < 192; ff++) {
            long v = natural_spec[192 * ww + ff];
            energy[ww] += v * v;
          }
        TASK_CHECK(energy[0] != energy[1] || energy[1] != energy[2]);
      }
    }

  {
    int expected_end = main_start_bit;
    for (gr = 0; gr < 2; gr++)
      for (ch = 0; ch < channels; ch++)
        expected_end += recs[gr][ch].part2_3_length;
    TASK_CHECK(expected_end == frame_end_bit);
  }
}

static void fill_transient(int16_t *pcm, int count, unsigned seed) {
  int i;
  for (i = 0; i < count; i++) {
    seed = seed * 1103515245u + 12345u;
    pcm[i] = (int16_t)((int)((seed >> 16) % 1200) - 600);
  }
  pcm[120] = 22000;
  pcm[130] = -20000;
  pcm[560] = 18000;
  pcm[580] = -16000;
  pcm[1020] = 20000;
  pcm[1040] = -18000;
}

static void roundtrip_sequence(int channels, int rate, int rate_index,
                                int mode, int frames, int pcm_per_chan,
                                int padding_seen[2], int coverage[3]) {
  shine_t s = make_encoder(channels, rate, mode);
  int total = channels * pcm_per_chan;
  int16_t *pcm = guard_alloc(total * sizeof(int16_t));
  unsigned char *stream = guard_alloc(32768);
  snap_frame_t snaps[MAX_SNAP_FRAMES];
  int sync_pos[MAX_SNAP_FRAMES];
  size_t sl = 0;
  int nshort = 0, f, n;
  const shine_global_config *o;

  TASK_CHECK(frames + 1 < MAX_SNAP_FRAMES);

  fill_transient(pcm, total, 99 + rate_index);
  if (channels == 2) {
    uint8_t enter[4] = {SHINE_BLOCK_NORMAL, SHINE_BLOCK_NORMAL,
                        SHINE_BLOCK_START, SHINE_BLOCK_START};
    TASK_CHECK(
        shine_encode_buffer_interleaved_blocks(s, pcm, enter, 4, &n));
  } else {
    uint8_t enter[2] = {SHINE_BLOCK_START, SHINE_BLOCK_SHORT};
    TASK_CHECK(shine_encode_buffer_blocks(s, (int16_t *[]){pcm}, enter, 2, &n));
  }

  for (f = 0; f < frames; f++) {
    unsigned char *block;
    int local_sync;
    fill_transient(pcm, total, 1000 + f * 37 + rate_index);
    if (channels == 2) {
      uint8_t shorts[4] = {SHINE_BLOCK_SHORT, SHINE_BLOCK_SHORT,
                           SHINE_BLOCK_SHORT, SHINE_BLOCK_SHORT};
      block =
          shine_encode_buffer_interleaved_blocks(s, pcm, shorts, 4, &n);
    } else {
      uint8_t shorts[2] = {SHINE_BLOCK_SHORT, SHINE_BLOCK_SHORT};
      block = shine_encode_buffer_blocks(s, (int16_t *[]){pcm}, shorts, 2, &n);
    }
    TASK_CHECK(block != NULL && n > 0);
    TASK_CHECK(guard_ok(pcm, total * sizeof(int16_t)));
    local_sync = locate_sync(block, n);
    TASK_CHECK(local_sync >= 0);
    sync_pos[nshort] = (int)sl + local_sync;
    memcpy(stream + sl, block, n);
    sl += (size_t)n;
    o = shine_test_observations(s);
    snapshot_frame(o, channels, &snaps[nshort]);
    nshort++;
  }
  TASK_CHECK(guard_ok(stream, 32768));

  /* only frames followed by another sync word are fully delivered. */
  for (f = 0; f + 1 < nshort; f++)
    check_stream_frame(stream, sync_pos[f], sync_pos[f + 1], rate_index,
                       channels, &snaps[f], padding_seen, coverage);

  guard_free(stream);
  guard_free(pcm);
  shine_close(s);
}

TASK_CASE("bitstream", short_block_roundtrip_all_rates) {
  static const int rates[3] = {44100, 48000, 32000};
  static const int rate_index[3] = {0, 1, 2};
  /* 44.1 kHz padding only turns off after ~23 frames (slot drift), so that
   * sequence runs long enough to observe both padding values. */
  static const int frames[3] = {30, 8, 8};
  int r;

  for (r = 0; r < 3; r++) {
    int padding_seen[2] = {0, 0};
    int coverage[3] = {0, 0, 0};
    roundtrip_sequence(PCM_STEREO, rates[r], rate_index[r], STEREO,
                       frames[r], 1152, padding_seen, coverage);
    TASK_CHECK(coverage[0] && coverage[1] && coverage[2]);
    if (rates[r] == 44100)
      /* 44.1 kHz has a fractional slot rate: both padding values occur. */
      TASK_CHECK(padding_seen[0] && padding_seen[1]);
    else
      TASK_CHECK(padding_seen[0] || padding_seen[1]);
  }
}

TASK_CASE("bitstream", mono_short_block_roundtrip) {
  int padding_seen[2] = {0, 0};
  int coverage[3] = {0, 0, 0};

  roundtrip_sequence(PCM_MONO, 48000, 1, MONO, 5, 1152, padding_seen,
                     coverage);
  TASK_CHECK(coverage[0] && coverage[1] && coverage[2]);
}

/* Decode a fully delivered frame that contains start/stop transition
 * granules. Transition granules keep the 576-line long MDCT (so stream and
 * natural coordinates coincide) but use switched side info with the implicit
 * long-band Huffman regions at bands 8 and 12. The same frame may also carry
 * pure-short granules, which use the short-band regions; decode_spectrum()
 * picks the correct geometry from the parsed block type. */
static void check_transition_frame(const unsigned char *stream, int sync_byte,
                                   int next_sync_byte, int rate_index,
                                   int channels, const snap_frame_t *snap,
                                   int *saw_start, int *saw_stop) {
  bitreader_t br;
  parsed_gi_t recs[2][MAX_CHANNELS];
  int main_start_bit, frame_end_bit, gr, ch, rec_index;

  TASK_CHECK(next_sync_byte > sync_byte);
  br_init(&br, stream, sync_byte);
  frame_end_bit = next_sync_byte * 8;
  TASK_CHECK(br_get(&br, 11) == 0x7FF);
  TASK_CHECK(br_get(&br, 2) == MPEG_I);
  TASK_CHECK(br_get(&br, 2) == LAYER_III);
  TASK_CHECK(br_get(&br, 1) == 1);
  TASK_CHECK(br_get(&br, 4) > 0);
  TASK_CHECK(br_get(&br, 2) == (unsigned)rate_index);
  br_get(&br, 1);
  br_get(&br, 1);
  br_get(&br, 2);
  br_get(&br, 2);
  br_get(&br, 1);
  br_get(&br, 1);
  br_get(&br, 2);
  TASK_CHECK(br_get(&br, 9) == 0);
  br_get(&br, channels == 2 ? 3 : 5);
  if (channels == 2)
    br_get(&br, 8);
  else
    br_get(&br, 4);

  parse_all_records(&br, channels, recs);
  main_start_bit = br.bit;

  rec_index = 0;
  for (gr = 0; gr < 2; gr++)
    for (ch = 0; ch < channels; ch++, rec_index++) {
      parsed_gi_t *g = &recs[gr][ch];
      int stream_spec[GRANULE_SIZE], natural_spec[GRANULE_SIZE];
      int region_start, huff_end, i;

      TASK_CHECK(g->mixed == 0);
      TASK_CHECK(g->subblock_gain[0] == 0 && g->subblock_gain[1] == 0 &&
                 g->subblock_gain[2] == 0);
      if (g->switched && g->block_type == SHINE_BLOCK_START)
        *saw_start = 1;
      else if (g->switched && g->block_type == SHINE_BLOCK_STOP)
        *saw_stop = 1;

      g->count1 = snap->gc[gr][ch].count1;
      TASK_CHECK(g->big_values == snap->gc[gr][ch].big_values);

      /* switched records only populate table_select[1] and [2]. */
      if (g->switched)
        TASK_CHECK(g->table_select[0] == 0);

      region_start = main_start_bit;
      for (i = 0; i < rec_index; i++) {
        int pg = i / channels, pc = i % channels;
        region_start += recs[pg][pc].part2_3_length;
      }
      br.bit = region_start;
      decode_spectrum(&br, g, rate_index, stream_spec);
      huff_end = region_start + (int)g->part2_3_length;
      TASK_CHECK(br.bit <= huff_end);

      if (g->block_type == SHINE_BLOCK_SHORT) {
        independent_unreorder(stream_spec, natural_spec);
        for (i = 0; i < GRANULE_SIZE; i++) {
          TASK_CHECK(stream_spec[i] == snap->gc[gr][ch].stream[i]);
          TASK_CHECK(natural_spec[i] == snap->gc[gr][ch].natural[i]);
        }
      } else if (g->switched) {
        /* transition long blocks: no permutation, stream == natural. The
         * switched side info has no table_select[0], so the implicit region
         * 0 (long bands 0..7) is not coded and decodes to zero; the coded
         * regions start at long band 8. */
        int r1 = shine_scale_fact_band_index[rate_index][8];
        for (i = 0; i < r1; i++)
          TASK_CHECK(stream_spec[i] == 0);
        for (i = r1; i < GRANULE_SIZE; i++)
          TASK_CHECK(stream_spec[i] == snap->gc[gr][ch].stream[i]);
      } else {
        /* ordinary long block: all 576 lines are coded, no permutation. */
        for (i = 0; i < GRANULE_SIZE; i++)
          TASK_CHECK(stream_spec[i] == snap->gc[gr][ch].stream[i]);
      }

      /* symbols already match, so any remaining part2_3 bits are the
       * all-ones Huffman stuffing. */
      while (br.bit < huff_end)
        TASK_CHECK(br_get(&br, 1) == 1);
    }

  {
    int expected_end = main_start_bit;
    for (gr = 0; gr < 2; gr++)
      for (ch = 0; ch < channels; ch++)
        expected_end += recs[gr][ch].part2_3_length;
    TASK_CHECK(expected_end == frame_end_bit);
  }
}

TASK_CASE("bitstream", transition_blocks_roundtrip) {
  shine_t s = make_encoder(PCM_MONO, 48000, MONO);
  int16_t *pcm = guard_alloc(1152 * sizeof(int16_t));
  unsigned char *stream = guard_alloc(16384);
  snap_frame_t snaps[MAX_SNAP_FRAMES];
  int sync_pos[MAX_SNAP_FRAMES];
  size_t sl = 0;
  int nshort = 0, f, n, saw_start = 0, saw_stop = 0;
  const shine_global_config *o;
  /* mono per-frame plans (two granules). The enter frame leaves the channel
   * in short; the following plans walk the legal short->stop->normal->
   * start->short cycle so both transition types appear. */
  uint8_t p_enter[2] = {SHINE_BLOCK_START, SHINE_BLOCK_SHORT};
  uint8_t p_stop[2] = {SHINE_BLOCK_SHORT, SHINE_BLOCK_STOP};
  uint8_t p_start[2] = {SHINE_BLOCK_NORMAL, SHINE_BLOCK_START};

  fill_transient(pcm, 1152, 321);
  TASK_CHECK(
      shine_encode_buffer_blocks(s, (int16_t *[]){pcm}, p_enter, 2, &n));
  for (f = 0; f < 5; f++) {
    unsigned char *block;
    int local_sync;
    const uint8_t *plan;
    /* states: after enter=short. f0 stop->stop, f1 start->start, then
     * alternate stop/start frames, each walking start->short->stop->normal
     * legally so both transition types recur. */
    if (f % 2 == 0)
      plan = p_stop;
    else
      plan = p_start;
    fill_transient(pcm, 1152, 700 + f);
    block = shine_encode_buffer_blocks(s, (int16_t *[]){pcm}, plan, 2, &n);
    TASK_CHECK(block && n > 0);
    local_sync = locate_sync(block, n);
    TASK_CHECK(local_sync >= 0);
    sync_pos[nshort] = (int)sl + local_sync;
    memcpy(stream + sl, block, n);
    sl += (size_t)n;
    o = shine_test_observations(s);
    snapshot_frame(o, 1, &snaps[nshort]);
    nshort++;
  }
  TASK_CHECK(guard_ok(stream, 16384));
  for (f = 0; f + 1 < nshort; f++)
    check_transition_frame(stream, sync_pos[f], sync_pos[f + 1], 1, 1,
                           &snaps[f], &saw_start, &saw_stop);
  TASK_CHECK(saw_start && saw_stop);
  guard_free(stream);
  guard_free(pcm);
  shine_close(s);
}

/* ================================================================== */
/* state group                                                         */
/* ================================================================== */

typedef struct {
  unsigned char *buf;
  size_t cap, len;
} sink_t;

static sink_t sink_make(size_t cap) {
  sink_t sk;
  sk.cap = cap;
  sk.len = 0;
  sk.buf = guard_alloc(cap);
  return sk;
}

static void sink_push(sink_t *sk, const unsigned char *p, int n) {
  if (sk->len + (size_t)n > sk->cap)
    fail_line("sink overflow", __LINE__);
  memcpy(sk->buf + sk->len, p, n);
  sk->len += (size_t)n;
  TASK_CHECK(guard_ok(sk->buf, sk->cap));
}

TASK_CASE("state", cross_frame_sequences_per_channel) {
  shine_t s = make_encoder(PCM_STEREO, 48000, STEREO);
  int16_t *pcm = guard_alloc(2304 * sizeof(int16_t));
  const shine_global_config *o;
  int n, frame;
  static const uint8_t plans[][4] = {
      {SHINE_BLOCK_START, SHINE_BLOCK_NORMAL, SHINE_BLOCK_SHORT,
       SHINE_BLOCK_NORMAL},
      {SHINE_BLOCK_SHORT, SHINE_BLOCK_START, SHINE_BLOCK_SHORT,
       SHINE_BLOCK_SHORT},
      {SHINE_BLOCK_STOP, SHINE_BLOCK_STOP, SHINE_BLOCK_NORMAL,
       SHINE_BLOCK_NORMAL},
      {SHINE_BLOCK_NORMAL, SHINE_BLOCK_NORMAL, SHINE_BLOCK_NORMAL,
       SHINE_BLOCK_NORMAL},
  };

  for (frame = 0; frame < 4; frame++) {
    fill_pcm(pcm, 2304, 31 + frame, 5000);
    TASK_CHECK(shine_encode_buffer_interleaved_blocks(s, pcm, plans[frame], 4, &n));
    TASK_CHECK(guard_ok(pcm, 2304 * sizeof(int16_t)));
    o = shine_test_observations(s);
    TASK_CHECK(o->obs_block_type[0][0] == plans[frame][0]);
    TASK_CHECK(o->obs_block_type[0][1] == plans[frame][2]);
    TASK_CHECK(o->obs_block_type[1][0] == plans[frame][1]);
    TASK_CHECK(o->obs_block_type[1][1] == plans[frame][3]);
  }
  guard_free(pcm);
  shine_close(s);
}

TASK_CASE("state", two_instances_interleaved) {
  shine_t a = make_encoder(PCM_STEREO, 48000, STEREO);
  shine_t b = make_encoder(PCM_STEREO, 48000, STEREO);
  int16_t *pcm = guard_alloc(2304 * sizeof(int16_t));
  const shine_global_config *oa, *ob;
  int n;
  uint8_t enter[4] = {SHINE_BLOCK_START, SHINE_BLOCK_NORMAL,
                      SHINE_BLOCK_SHORT, SHINE_BLOCK_NORMAL};
  uint8_t shorts[4] = {SHINE_BLOCK_SHORT, SHINE_BLOCK_NORMAL,
                       SHINE_BLOCK_SHORT, SHINE_BLOCK_NORMAL};
  uint8_t bad[4] = {SHINE_BLOCK_NORMAL, SHINE_BLOCK_NORMAL,
                    SHINE_BLOCK_NORMAL, SHINE_BLOCK_NORMAL};

  fill_pcm(pcm, 2304, 5, 4000);
  TASK_CHECK(shine_encode_buffer_interleaved_blocks(a, pcm, enter, 4, &n));
  oa = shine_test_observations(a);
  TASK_CHECK(oa->block_state[0] == SHINE_BLOCK_SHORT);

  {
    int wb = -9;
    TASK_CHECK(shine_encode_buffer_interleaved_blocks(a, pcm, bad, 4, &wb) ==
               NULL);
    TASK_CHECK(wb == -1);
  }
  ob = shine_test_observations(b);
  TASK_CHECK(ob->block_state[0] == SHINE_BLOCK_NORMAL &&
             ob->block_state[1] == SHINE_BLOCK_NORMAL);

  TASK_CHECK(shine_encode_buffer_interleaved_blocks(b, pcm, enter, 4, &n));
  ob = shine_test_observations(b);
  TASK_CHECK(ob->block_state[0] == SHINE_BLOCK_SHORT);
  TASK_CHECK(shine_encode_buffer_interleaved_blocks(a, pcm, shorts, 4, &n));
  oa = shine_test_observations(a);
  TASK_CHECK(oa->block_state[0] == SHINE_BLOCK_SHORT);

  guard_free(pcm);
  shine_close(a);
  shine_close(b);
}

TASK_CASE("state", planar_and_interleaved_entries_equivalent) {
  shine_t sp = make_encoder(PCM_STEREO, 48000, STEREO);
  shine_t si = make_encoder(PCM_STEREO, 48000, STEREO);
  int16_t *inter = guard_alloc(2304 * sizeof(int16_t));
  int16_t *left = guard_alloc(1152 * sizeof(int16_t));
  int16_t *right = guard_alloc(1152 * sizeof(int16_t));
  int16_t *split[2] = {left, right};
  int np, ni, f;
  uint8_t plans[2][4] = {
      {SHINE_BLOCK_START, SHINE_BLOCK_START, SHINE_BLOCK_SHORT,
       SHINE_BLOCK_SHORT},
      {SHINE_BLOCK_SHORT, SHINE_BLOCK_SHORT, SHINE_BLOCK_STOP,
       SHINE_BLOCK_STOP}};

  for (f = 0; f < 2; f++) {
    unsigned char *pp, *pi;
    fill_pcm(inter, 2304, 71 + f, 6000);
    {
      int i;
      for (i = 0; i < 1152; i++) {
        left[i] = inter[2 * i];
        right[i] = inter[2 * i + 1];
      }
    }
    pp = shine_encode_buffer_blocks(sp, split, plans[f], 4, &np);
    pi = shine_encode_buffer_interleaved_blocks(si, inter, plans[f], 4, &ni);
    TASK_CHECK(pp && pi && np == ni);
    TASK_CHECK(memcmp(pp, pi, np) == 0);
    TASK_CHECK(guard_ok(inter, 2304 * sizeof(int16_t)));
    TASK_CHECK(guard_ok(left, 1152 * sizeof(int16_t)));
    TASK_CHECK(guard_ok(right, 1152 * sizeof(int16_t)));
  }
  guard_free(inter);
  guard_free(left);
  guard_free(right);
  shine_close(sp);
  shine_close(si);
}

TASK_CASE("state", illegal_requests_then_clean_retry) {
  shine_t s = make_encoder(PCM_STEREO, 48000, STEREO);
  shine_t ref = make_encoder(PCM_STEREO, 48000, STEREO);
  int16_t *pcm = guard_alloc(2304 * sizeof(int16_t));
  int n, wb;
  unsigned char *good, *refp;
  uint8_t enter[4] = {SHINE_BLOCK_START, SHINE_BLOCK_NORMAL,
                      SHINE_BLOCK_SHORT, SHINE_BLOCK_NORMAL};
  uint8_t bad_count[3] = {0, 0, 0};
  uint8_t bad_value[4] = {0, 0, 9, 0};
  uint8_t bad_trans[4] = {SHINE_BLOCK_NORMAL, SHINE_BLOCK_NORMAL,
                          SHINE_BLOCK_NORMAL, SHINE_BLOCK_NORMAL};

  fill_pcm(pcm, 2304, 83, 4000);
  good = shine_encode_buffer_interleaved_blocks(s, pcm, enter, 4, &n);
  refp = shine_encode_buffer_interleaved_blocks(ref, pcm, enter, 4, &n);
  TASK_CHECK(good && refp);

  wb = 3;
  TASK_CHECK(shine_encode_buffer_interleaved_blocks(s, pcm, bad_count, 3, &wb) == NULL &&
             wb == -1);
  wb = 3;
  TASK_CHECK(shine_encode_buffer_interleaved_blocks(s, pcm, bad_value, 4, &wb) == NULL &&
             wb == -1);
  wb = 3;
  TASK_CHECK(shine_encode_buffer_interleaved_blocks(s, pcm, bad_trans, 4, &wb) == NULL &&
             wb == -1);
  wb = 3;
  TASK_CHECK(shine_encode_buffer_interleaved_blocks(s, pcm, NULL, 4, &wb) ==
             NULL &&
             wb == -1);

  {
    uint8_t cont[4] = {SHINE_BLOCK_SHORT, SHINE_BLOCK_NORMAL,
                       SHINE_BLOCK_SHORT, SHINE_BLOCK_NORMAL};
    int n2;
    good = shine_encode_buffer_interleaved_blocks(s, pcm, cont, 4, &n);
    refp = shine_encode_buffer_interleaved_blocks(ref, pcm, cont, 4, &n2);
    TASK_CHECK(good && refp && n == n2);
    TASK_CHECK(memcmp(good, refp, n) == 0);
  }
  guard_free(pcm);
  shine_close(s);
  shine_close(ref);
}

TASK_CASE("state", unsupported_configurations_rejected) {
  static const struct {
    int channels, rate, mode;
  } cfgs[] = {
      {PCM_STEREO, 22050, STEREO},
      {PCM_STEREO, 48000, JOINT_STEREO},
      {PCM_STEREO, 48000, DUAL_CHANNEL},
      {PCM_MONO, 32000, STEREO},
  };
  int c;
  for (c = 0; c < (int)(sizeof(cfgs) / sizeof(cfgs[0])); c++) {
    shine_t s = make_encoder(cfgs[c].channels, cfgs[c].rate, cfgs[c].mode);
    if (s) {
      int16_t *pcm = guard_alloc(2304 * sizeof(int16_t));
      uint8_t blocks[4] = {0, 0, 0, 0};
      int wb = 5;
      fill_pcm(pcm, 2304, 11, 1000);
      TASK_CHECK(shine_encode_buffer_interleaved_blocks(s, pcm, blocks, 4, &wb) == NULL &&
                 wb == -1);
      guard_free(pcm);
      shine_close(s);
    }
  }
  {
    static const int ok_rates[3] = {32000, 44100, 48000};
    int r;
    for (r = 0; r < 3; r++) {
      shine_t s = make_encoder(PCM_MONO, ok_rates[r], MONO);
      int16_t *pcm = guard_alloc(1152 * sizeof(int16_t));
      uint8_t blocks[2] = {SHINE_BLOCK_NORMAL, SHINE_BLOCK_NORMAL};
      int n;
      fill_pcm(pcm, 1152, 12, 1000);
      TASK_CHECK(shine_encode_buffer_blocks(s, (int16_t *[]){pcm}, blocks, 2, &n) != NULL);
      guard_free(pcm);
      shine_close(s);
    }
  }
}

TASK_CASE("state", all_normal_matches_legacy_entry_byte_for_byte) {
  shine_t legacy = make_encoder(PCM_STEREO, 48000, STEREO);
  shine_t blocks = make_encoder(PCM_STEREO, 48000, STEREO);
  int16_t *pcm = guard_alloc(2304 * sizeof(int16_t));
  sink_t sl = sink_make(4096), sb = sink_make(4096);
  uint8_t all_normal[4] = {SHINE_BLOCK_NORMAL, SHINE_BLOCK_NORMAL,
                           SHINE_BLOCK_NORMAL, SHINE_BLOCK_NORMAL};
  int f, nl, nb;

  for (f = 0; f < 6; f++) {
    unsigned char *pl, *pb;
    fill_pcm(pcm, 2304, 200 + f, 9000);
    pl = shine_encode_buffer_interleaved(legacy, pcm, &nl);
    pb = shine_encode_buffer_interleaved_blocks(blocks, pcm, all_normal, 4, &nb);
    TASK_CHECK(pl && pb && nl == nb);
    TASK_CHECK(memcmp(pl, pb, nl) == 0);
    sink_push(&sl, pl, nl);
    sink_push(&sb, pb, nb);
  }
  TASK_CHECK(sl.len == sb.len && memcmp(sl.buf, sb.buf, sl.len) == 0);
  guard_free(sl.buf);
  guard_free(sb.buf);
  guard_free(pcm);
  shine_close(legacy);
  shine_close(blocks);
}

TASK_CASE("state", complete_frame_delivery_no_dropped_or_fake_frames) {
  shine_t s = make_encoder(PCM_STEREO, 48000, STEREO);
  int16_t *pcm = guard_alloc(2304 * sizeof(int16_t));
  sink_t sk = sink_make(8192);
  uint8_t plans[3][4] = {
      {SHINE_BLOCK_START, SHINE_BLOCK_NORMAL, SHINE_BLOCK_SHORT,
       SHINE_BLOCK_NORMAL},
      {SHINE_BLOCK_SHORT, SHINE_BLOCK_NORMAL, SHINE_BLOCK_STOP,
       SHINE_BLOCK_NORMAL},
      {SHINE_BLOCK_NORMAL, SHINE_BLOCK_NORMAL, SHINE_BLOCK_NORMAL,
       SHINE_BLOCK_NORMAL}};
  const int frames = 9;
  int f, n, complete = 0;

  for (f = 0; f < frames; f++) {
    unsigned char *block;
    int sync, frame_bytes;
    fill_pcm(pcm, 2304, 900 + f, 7000);
    block = shine_encode_buffer_interleaved_blocks(s, pcm, plans[f % 3],
                                                   4, &n);
    TASK_CHECK(block && n > 0);
    sink_push(&sk, block, n);
    sync = locate_sync(block, n);
    TASK_CHECK(sync >= 0);
    frame_bytes = n - sync;
    TASK_CHECK(frame_bytes == 384 && frame_bytes % 4 == 0);
    complete++;
  }

  {
    int fn;
    unsigned char *tail = shine_flush(s, &fn);
    TASK_CHECK(tail != NULL);
    TASK_CHECK(fn >= 0 && fn < 4);
    if (fn)
      TASK_CHECK(locate_sync(tail, fn) < 0);
  }

  {
    int pos = 0, found = 0;
    while (pos + 4 <= (int)sk.len) {
      int sync = locate_sync(sk.buf + pos, sk.len - pos);
      if (sync < 0)
        break;
      pos += sync;
      if (pos + 384 <= (int)sk.len) {
        TASK_CHECK(sk.buf[pos] == 0xFF &&
                   (sk.buf[pos + 1] & 0xFE) == 0xFA);
        found++;
        pos += 384;
      } else
        break;
    }
    TASK_CHECK(found == frames);
    TASK_CHECK(complete == frames);
  }
  guard_free(sk.buf);
  guard_free(pcm);
  shine_close(s);
}
