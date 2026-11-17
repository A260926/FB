/* masked.c
 *
 * Caller supplied noise thresholds driving real long-block scalefactor
 * allocation.  The legacy entry points keep their zero scalefactor,
 * capacity-only behaviour; this file is used solely by the *_masked
 * entry points (and, when built with SHINE_TEST_HOOKS, records read-only
 * observability state for the test suite).
 */

#include "masked.h"

#include <float.h>
#include <limits.h>
#include <math.h>

#include "huffman.h"
#include "l3loop.h"
#include "layer3.h"
#include "reservoir.h"
#include "tables.h"

#define MASKED_MAX_ITER 64
#define MASKED_INF 1e300

/* Loop helpers implemented in l3loop.c. */
int quantize(int ix[GRANULE_SIZE], int stepsize, shine_global_config *config);
void calc_runlen(int ix[GRANULE_SIZE], gr_info *cod_info);
int count1_bitcount(int ix[GRANULE_SIZE], gr_info *cod_info);
void subdivide(gr_info *cod_info, shine_global_config *config);
void bigv_tab_select(int ix[GRANULE_SIZE], gr_info *cod_info);
int bigv_bitcount(int ix[GRANULE_SIZE], gr_info *gi);

#ifdef SHINE_TEST_HOOKS
static shine_test_hook_t g_test_hooks;
const shine_test_hook_t *shine_test_hooks(void) { return &g_test_hooks; }

void shine_test_hook_post_format(shine_global_config *config) {
  int gr, ch;
  for (ch = 0; ch < config->wave.channels; ch++)
    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++)
      memcpy(g_test_hooks.chosen_ix[gr][ch], config->l3_enc[ch][gr],
             sizeof(g_test_hooks.chosen_ix[gr][ch]));
}
#endif

int shine_masked_validate(shine_global_config *config, const double *ratios,
                          int ratio_count) {
  int expected, i;

  if (ratios == NULL)
    return ratio_count == 0 ? 0 : -1;

  if (ratio_count <= 0)
    return -1;

  if (config->mpeg.version != MPEG_I)
    return -1;

  if (config->wave.samplerate != 32000 &&
      config->wave.samplerate != 44100 &&
      config->wave.samplerate != 48000)
    return -1;

  if (config->wave.channels != PCM_MONO &&
      config->wave.channels != PCM_STEREO)
    return -1;

  if (config->mpeg.mode != MONO && config->mpeg.mode != STEREO)
    return -1;

  expected = MASKED_RATIO_COUNT(config->wave.channels);
  if (ratio_count != expected)
    return -1;

  for (i = 0; i < ratio_count; i++) {
    double r = ratios[i];
    if (!isfinite(r) || r <= 0.0 || r > 1.0)
      return -1;
  }

  return 0;
}

/* ----------------------------------------------------------------------- */
/* Scalefactor compression / SCFSI                                          */
/* ----------------------------------------------------------------------- */

static const int scfsi_group_start[4] = {0, 6, 11, 16};
static const int scfsi_group_len[4] = {6, 5, 5, 5};

/* Choose scalefac_compress for one granule: smallest part2 cost among the
 * MPEG-1 slen table entries able to represent every scalefactor that is
 * actually transmitted; ties resolve to the lowest table index. */
static int choose_compress(int gr, const int *sf, const int *sf_gr0,
                           int *scfsi_out, int *part2_out) {
  int sc, group, best = 0, best_bits = INT_MAX;

  for (group = 0; group < 4; group++) {
    if (gr == 0 || sf_gr0 == NULL)
      scfsi_out[group] = 0;
    else {
      int sfb, equal = 1;
      for (sfb = scfsi_group_start[group];
           sfb < scfsi_group_start[group] + scfsi_group_len[group]; sfb++)
        if (sf[sfb] != sf_gr0[sfb]) {
          equal = 0;
          break;
        }
      scfsi_out[group] = equal;
    }
  }

  for (sc = 0; sc < 16; sc++) {
    int slen1 = shine_slen1_tab[sc];
    int slen2 = shine_slen2_tab[sc];
    int bits = 0, ok = 1;

    for (group = 0; group < 4; group++) {
      int slen = group < 2 ? slen1 : slen2;
      int sfb, maxv = 0, limit = (1 << slen) - 1;

      if (gr != 0 && scfsi_out[group])
        continue; /* group is copied from granule 0 */

      for (sfb = scfsi_group_start[group];
           sfb < scfsi_group_start[group] + scfsi_group_len[group]; sfb++)
        if (sf[sfb] > maxv)
          maxv = sf[sfb];

      if (maxv > limit) {
        ok = 0;
        break;
      }
      bits += scfsi_group_len[group] * slen;
    }

    if (ok && bits < best_bits) {
      best_bits = bits;
      best = sc;
    }
  }

  *part2_out = best_bits;
  return best;
}

/* ----------------------------------------------------------------------- */
/* Quantization with real per-band scalefactors                             */
/* ----------------------------------------------------------------------- */

static int band_of_line(int line, int samplerate_index) {
  const int *band = shine_scale_fact_band_index[samplerate_index];
  int sfb;

  for (sfb = 0; sfb < MASKED_SFB - 1; sfb++)
    if (line < band[sfb + 1])
      return sfb;
  return MASKED_SFB - 1;
}

/* Quantize every line with the effective step size of its band.
 * quantizerStepSize is raised until every value fits the Huffman range. */
static void quantize_with_scalefacs(int ix[GRANULE_SIZE], int *qss_inout,
                                    const int *sf,
                                    shine_global_config *config) {
  int tmp[8][GRANULE_SIZE];
  int have[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  int qss = *qss_inout;
  int sfb, line, maxv;

  do {
    maxv = 0;
    for (sfb = 0; sfb < MASKED_SFB; sfb++) {
      int f = sf[sfb]; /* band 21 always carries an implicit zero */
      int eff = qss - 2 * f;
      int band_max;

      if (eff < -127)
        eff = -127;
      if (eff > 0)
        eff = 0;

      if (!have[f]) {
        band_max = quantize(tmp[f], eff, config);
        have[f] = 1;
        if (band_max > maxv)
          maxv = band_max;
      }
    }
    /* At qss == 0 the effective step is the largest one the tables
     * represent, so the spectrum always fits the Huffman value range. */
    if (maxv > 8192 && qss < 0) {
      qss++;
      memset(have, 0, sizeof(have));
    }
  } while (maxv > 8192 && qss < 0);

  for (line = 0; line < GRANULE_SIZE; line++) {
    sfb = band_of_line(line, config->mpeg.samplerate_index);
    ix[line] = tmp[sf[sfb]][line];
  }

  *qss_inout = qss;
}

/* Run table selection and count the Huffman bits of an already quantized
 * spectrum, raising the global step size until the budget is met.
 * Returns 0 on success, -1 if no legal step size exists. The achieved
 * bit count is written through huff_bits_out. The step size is bounded
 * at 0: steptab only indexes stepsize+127 in [0,127] and global gain is
 * an 8-bit field. */
static int fit_huffman(int ix[GRANULE_SIZE], int *qss_inout, const int *sf,
                       int huff_budget, gr_info *gi,
                       shine_global_config *config, int *huff_bits_out) {
  int bits;

  if (huff_budget < 0)
    return -1;

  for (;;) {
    quantize_with_scalefacs(ix, qss_inout, sf, config);
    calc_runlen(ix, gi);
    bits = count1_bitcount(ix, gi);
    subdivide(gi, config);
    bigv_tab_select(ix, gi);
    bits += bigv_bitcount(ix, gi);
    if (bits <= huff_budget) {
      *huff_bits_out = bits;
      return 0;
    }
    if (*qss_inout >= 0)
      return -1;
    (*qss_inout)++;
  }
}

/* ----------------------------------------------------------------------- */
/* Independent reconstruction and normalized band noise                     */
/* ----------------------------------------------------------------------- */

static void reconstruct(const int *ix, int global_gain, const int *sf,
                        int samplerate_index, double *y) {
  double common = pow(2.0, ((double)global_gain - 210.0) / 4.0);
  int line;

  for (line = 0; line < GRANULE_SIZE; line++) {
    int sfb = band_of_line(line, samplerate_index);
    double step = common * pow(2.0, -0.5 * sf[sfb]);
    int aq = ix[line] < 0 ? -ix[line] : ix[line];

    if (aq == 0)
      y[line] = 0.0;
    else {
      double mag = step * pow((double)aq, 4.0 / 3.0);
      y[line] = ix[line] < 0 ? -mag : mag;
    }
  }
}

static double worst_band_noise(shine_global_config *config, int gr, int ch,
                               const int *ix, int global_gain,
                               const int *sf, const double *ratios) {
  const int *band = shine_scale_fact_band_index[config->mpeg.samplerate_index];
  const double nat = 1.0 / 2147483648.0; /* mdct_freq / 2^31 */
  const int32_t *xr = config->mdct_freq[ch][gr];
  const double *frame_ratios =
      ratios + (gr * config->wave.channels + ch) * MASKED_SFB;
  double y[GRANULE_SIZE];
  double worst = 0.0;
  int sfb;

  reconstruct(ix, global_gain, sf, config->mpeg.samplerate_index, y);

  for (sfb = 0; sfb < MASKED_SFB; sfb++) {
    double energy = 0.0, distortion = 0.0, noise;
    int line;

    for (line = band[sfb]; line < band[sfb + 1]; line++) {
      double x = (double)xr[line] * nat;
      double d = x - y[line];
      energy += x * x;
      distortion += d * d;
    }

    if (energy == 0.0)
      noise = (distortion == 0.0) ? 0.0 : MASKED_INF;
    else
      noise = distortion / (frame_ratios[sfb] * energy);

    if (noise > worst)
      worst = noise;
  }

  return worst;
}

/* ----------------------------------------------------------------------- */
/* Per granule search                                                       */
/* ----------------------------------------------------------------------- */

typedef struct {
  int sf[MASKED_SFB];
  int qss;
  int ix[GRANULE_SIZE];
  int scfsi[4];
  gr_info gi;
  int part2;
  int part2_3;
  double worst;
} masked_candidate_t;

static void reset_gr_info(gr_info *gi) {
  memset(gi, 0, sizeof(*gi));
  gi->sfb_lmax = MASKED_SFB - 1;
  gi->preflag = 0;
  gi->scalefac_scale = 0;
}

static void evaluate(shine_global_config *config, int gr, int ch,
                     const double *ratios, int max_bits,
                     masked_candidate_t *cand, int *legal) {
  gr_info *gi = &config->side_info.gr[gr].ch[ch].tt;
  const int *sf_gr0 = (gr == 0)
                          ? NULL
                          : config->scalefactor.l[0][ch];
  int scfsi[4], huff_bits;

  cand->sf[MASKED_SFB - 1] = 0;
  gi->preflag = 0;
  gi->scalefac_scale = 0;

  gi->scalefac_compress =
      choose_compress(gr, cand->sf, sf_gr0, scfsi, &cand->part2);
  cand->scfsi[0] = scfsi[0];
  cand->scfsi[1] = scfsi[1];
  cand->scfsi[2] = scfsi[2];
  cand->scfsi[3] = scfsi[3];
  config->side_info.scfsi[ch][0] = scfsi[0];
  config->side_info.scfsi[ch][1] = scfsi[1];
  config->side_info.scfsi[ch][2] = scfsi[2];
  config->side_info.scfsi[ch][3] = scfsi[3];

  *legal = 0;
  if (cand->part2 > max_bits)
    return;

  if (fit_huffman(cand->ix, &cand->qss, cand->sf,
                  max_bits - cand->part2, gi, config, &huff_bits) < 0)
    return;
  cand->part2_3 = cand->part2 + huff_bits;
  if (cand->part2_3 > 4095 || cand->part2_3 > max_bits)
    return;

  gi->part2_length = cand->part2;
  gi->part2_3_length = cand->part2_3;
  cand->gi = *gi;
  cand->worst =
      worst_band_noise(config, gr, ch, cand->ix, cand->qss + 210,
                       cand->sf, ratios);
  *legal = 1;
}

static int pick_worst_band(shine_global_config *config, int gr, int ch,
                           const int *ix, int global_gain, const int *sf,
                           const double *ratios, const int *blocked) {
  const int *band = shine_scale_fact_band_index[config->mpeg.samplerate_index];
  const double nat = 1.0 / 2147483648.0;
  const int32_t *xr = config->mdct_freq[ch][gr];
  const double *frame_ratios =
      ratios + (gr * config->wave.channels + ch) * MASKED_SFB;
  double y[GRANULE_SIZE];
  double worst = -1.0;
  int worst_sfb = -1, sfb;

  reconstruct(ix, global_gain, sf, config->mpeg.samplerate_index, y);

  for (sfb = 0; sfb < MASKED_SFB - 1; sfb++) {
    double energy = 0.0, distortion = 0.0, noise;
    int line;

    if (blocked[sfb])
      continue; /* scalefactor range or bit budget exhausted */

    for (line = band[sfb]; line < band[sfb + 1]; line++) {
      double x = (double)xr[line] * nat;
      double d = x - y[line];
      energy += x * x;
      distortion += d * d;
    }

    if (energy == 0.0)
      noise = (distortion == 0.0) ? 0.0 : MASKED_INF;
    else
      noise = distortion / (frame_ratios[sfb] * energy);

    if (noise > worst) {
      worst = noise;
      worst_sfb = sfb;
    }
  }

  return worst_sfb;
}

static void search_granule(shine_global_config *config, int gr, int ch,
                           const double *ratios, int max_bits) {
  gr_info *gi = &config->side_info.gr[gr].ch[ch].tt;
  masked_candidate_t current, best;
  int legal, best_legal = 0, iter;
  int blocked[MASKED_SFB];
  int32_t xrmax = 0;
  int line;

  reset_gr_info(gi);
  memset(blocked, 0, sizeof(blocked));
  memset(config->scalefactor.l[gr][ch], 0,
         sizeof(config->scalefactor.l[gr][ch]));
  memset(config->scalefactor.s[gr][ch], 0,
         sizeof(config->scalefactor.s[gr][ch]));

  for (line = 0; line < GRANULE_SIZE; line++)
    if (config->l3loop.xrabs[line] > xrmax)
      xrmax = config->l3loop.xrabs[line];

  if (!xrmax) {
    /* Silent granule: exact zero reconstruction, nothing to allocate. */
    memset(config->l3_enc[ch][gr], 0, sizeof(config->l3_enc[ch][gr]));
    gi->quantizerStepSize = 0;
    gi->global_gain = 210;
#ifdef SHINE_TEST_HOOKS
    g_test_hooks.base_worst[gr][ch] = 0.0;
    g_test_hooks.chosen_worst[gr][ch] = 0.0;
    g_test_hooks.base_global_gain[gr][ch] = 210;
    g_test_hooks.base_part2_3[gr][ch] = 0;
#endif
    return;
  }

  memset(&current, 0, sizeof(current));
  /* Shine step sizes live at the fine end of the range; global gain is
   * qss + 210, so -120 matches the legacy binary search start. */
  current.qss = -120;
  evaluate(config, gr, ch, ratios, max_bits, &current, &legal);
  if (!legal) {
    /* Cannot fit at the current step size; raise the global step until
     * a legal stream exists. */
    do {
      current.qss++;
      evaluate(config, gr, ch, ratios, max_bits, &current, &legal);
    } while (!legal && current.qss < 0);
  }

  if (!legal) {
    /* Extreme capacity shortage: even the coarsest step size does not
     * meet the budget. Emit the coarsest representable stream anyway so
     * the frame stays decodable; reservoir accounting absorbs the debt. */
    gr_info *gi = &config->side_info.gr[gr].ch[ch].tt;
    int huff_bits;

    memset(current.sf, 0, sizeof(current.sf));
    current.qss = 0;
    gi->preflag = 0;
    gi->scalefac_scale = 0;
    gi->scalefac_compress =
        choose_compress(gr, current.sf,
                        gr == 0 ? NULL : config->scalefactor.l[0][ch],
                        current.scfsi, &current.part2);
    quantize_with_scalefacs(current.ix, &current.qss, current.sf, config);
    calc_runlen(current.ix, gi);
    huff_bits = count1_bitcount(current.ix, gi);
    subdivide(gi, config);
    bigv_tab_select(current.ix, gi);
    huff_bits += bigv_bitcount(current.ix, gi);
    current.part2_3 = current.part2 + huff_bits;
    if (current.part2_3 > 4095)
      current.part2_3 = 4095;
    gi->part2_length = current.part2;
    gi->part2_3_length = current.part2_3;
    current.gi = *gi;
    current.worst =
        worst_band_noise(config, gr, ch, current.ix, current.qss + 210,
                         current.sf, ratios);
    legal = 1;
  }

  best = current;
  best_legal = legal;

#ifdef SHINE_TEST_HOOKS
  g_test_hooks.base_worst[gr][ch] = current.worst;
  g_test_hooks.base_global_gain[gr][ch] = current.qss + 210;
  g_test_hooks.base_part2_3[gr][ch] = current.part2_3;
  memcpy(g_test_hooks.base_ix[gr][ch], current.ix, sizeof(current.ix));
#endif

  for (iter = 0; iter < MASKED_MAX_ITER; iter++) {
    int sfb;

    if (current.worst <= 1.0)
      break; /* every band satisfies its threshold */

    sfb = pick_worst_band(config, gr, ch, current.ix, current.qss + 210,
                          current.sf, ratios, blocked);
    if (sfb < 0)
      break;
    if (current.sf[sfb] >= 7) {
      blocked[sfb] = 1;
      continue;
    }

    current.sf[sfb]++;
    evaluate(config, gr, ch, ratios, max_bits, &current, &legal);
    if (!legal) {
      /* The extra scalefactor bits do not fit: this band can no longer
       * be raised, revert to the last legal candidate. */
      current.sf[sfb]--;
      blocked[sfb] = 1;
      evaluate(config, gr, ch, ratios, max_bits, &current, &legal);
      if (!legal)
        break;
      continue;
    }

    /* Strict improvement only; on equality the older candidate stays. */
    if (current.worst < best.worst) {
      best = current;
      best_legal = 1;
    }
  }

  if (!best_legal)
    best = current;

  /* Commit the winning candidate. */
  *gi = best.gi;
  gi->quantizerStepSize = best.qss;
  gi->global_gain = best.qss + 210;
  gi->preflag = 0;
  gi->scalefac_scale = 0;
  memcpy(config->l3_enc[ch][gr], best.ix, sizeof(best.ix));
  for (line = 0; line < MASKED_SFB; line++) {
    config->scalefactor.l[gr][ch][line] = best.sf[line];
#ifdef SHINE_TEST_HOOKS
    g_test_hooks.chosen_sf[gr][ch][line] = best.sf[line];
#endif
  }
  config->side_info.scfsi[ch][0] = best.scfsi[0];
  config->side_info.scfsi[ch][1] = best.scfsi[1];
  config->side_info.scfsi[ch][2] = best.scfsi[2];
  config->side_info.scfsi[ch][3] = best.scfsi[3];

#ifdef SHINE_TEST_HOOKS
  g_test_hooks.chosen_worst[gr][ch] = best.worst;
  g_test_hooks.chosen_count1[gr][ch] = best.gi.count1;
#endif
}

/* ----------------------------------------------------------------------- */
/* Frame driver                                                             */
/* ----------------------------------------------------------------------- */

void shine_masked_iteration_loop(shine_global_config *config,
                                 const double *ratios) {
  int ch, gr, line, max_bits;

#ifdef SHINE_TEST_HOOKS
  memset(&g_test_hooks, 0, sizeof(g_test_hooks));
  g_test_hooks.masked = 1;
  for (ch = 0; ch < config->wave.channels; ch++)
    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++)
      memcpy(g_test_hooks.xr[gr][ch], config->mdct_freq[ch][gr],
             sizeof(g_test_hooks.xr[gr][ch]));
#endif

  for (ch = config->wave.channels; ch--;) {
    memset(config->side_info.scfsi[ch], 0,
           sizeof(config->side_info.scfsi[ch]));
    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++) {
      config->l3loop.xr = config->mdct_freq[ch][gr];

      for (line = GRANULE_SIZE, config->l3loop.xrmax = 0; line--;) {
        config->l3loop.xrsq[line] =
            mulsr(config->l3loop.xr[line], config->l3loop.xr[line]);
        config->l3loop.xrabs[line] = labs(config->l3loop.xr[line]);
        if (config->l3loop.xrabs[line] > config->l3loop.xrmax)
          config->l3loop.xrmax = config->l3loop.xrabs[line];
      }

      max_bits = shine_max_reservoir_bits(&config->pe[ch][gr], config);
      search_granule(config, gr, ch, ratios, max_bits);
      shine_ResvAdjust(&config->side_info.gr[gr].ch[ch].tt, config);
    }
  }

  shine_ResvFrameEnd(config);
}
