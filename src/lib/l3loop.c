/* l3loop.c */

#include "l3loop.h"
#include "bitstream.h"
#include "huffman.h"
#include "l3bitstream.h"
#include "layer3.h"
#include "reservoir.h"
#include "tables.h"
#include "types.h"

#ifdef SHINE_TEST_HOOKS
#include "test_hooks.h"

int shine_obs_l3_enc[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES]
                    [SHINE_OBS_GRANULE_SIZE];
int shine_obs_l3_enc_nat[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES]
                        [SHINE_OBS_GRANULE_SIZE];
int8_t shine_obs_sign[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES]
                      [SHINE_OBS_GRANULE_SIZE];
struct shine_obs_side
    shine_obs_side[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES];
#endif

#define e 2.71828182845
#define CBLIMIT 21
#define SFB_LMAX 22
#define en_tot_krit 10
#define en_dif_krit 100
#define en_scfsi_band_krit 10
#define xm_scfsi_band_krit 10

static void calc_scfsi(shine_psy_xmin_t *l3_xmin, int ch, int gr,
                       shine_global_config *config);
static int part2_length(int gr, int ch, shine_global_config *config);
static int bin_search_StepSize(int desired_rate, int ix[GRANULE_SIZE],
                               gr_info *cod_info, shine_global_config *config);
static int count_bit(int ix[GRANULE_SIZE], unsigned int start, unsigned int end,
                     unsigned int table);
static int bigv_bitcount(int ix[GRANULE_SIZE], gr_info *gi);
static int new_choose_table(int ix[GRANULE_SIZE], unsigned int begin,
                            unsigned int end);
static void bigv_tab_select(int ix[GRANULE_SIZE], gr_info *cod_info);
static void subdivide(gr_info *cod_info, shine_global_config *config);
static int count1_bitcount(int ix[GRANULE_SIZE], gr_info *cod_info);
static void calc_runlen(int ix[GRANULE_SIZE], gr_info *cod_info);
static void calc_xmin(shine_psy_ratio_t *ratio, gr_info *cod_info,
                      shine_psy_xmin_t *l3_xmin, int gr, int ch);
static int quantize(int ix[GRANULE_SIZE], int stepsize,
                    shine_global_config *config);

/*
 * Window-switching band layout helpers.
 *
 * For MPEG-1 the samplerate index is 0 (44.1), 1 (48) or 2 (32) kHz and maps
 * directly onto shine_scale_fact_band_index / shine_short_band_index.
 */
int shine_l3_is_short(gr_info *gi) {
  return gi->window_switching && gi->block_type == 2 && !gi->mixed_block_flag;
}

/*
 * shine_inner_loop:
 * ----------
 * The code selects the best quantizerStepSize for a particular set
 * of scalefacs.
 */
int shine_inner_loop(int ix[GRANULE_SIZE], int max_bits, gr_info *cod_info,
                     int gr, int ch, shine_global_config *config) {
  int bits, c1bits, bvbits;

  if (max_bits < 0)
    cod_info->quantizerStepSize--;
  do {
    while (quantize(ix, ++cod_info->quantizerStepSize, config) > 8192)
      ; /* within table range? */

    calc_runlen(ix, cod_info);                     /* rzero,count1,big_values*/
    bits = c1bits = count1_bitcount(ix, cod_info); /* count1_table selection*/
    subdivide(cod_info, config);                   /* bigvalues sfb division */
    bigv_tab_select(ix, cod_info);                 /* codebook selection*/
    bits += bvbits = bigv_bitcount(ix, cod_info);  /* bit count */
  } while (bits > max_bits);
  return bits;
}

/*
 * shine_outer_loop:
 * -----------
 *  Function: The outer iteration loop controls the masking conditions
 *  of all scalefactorbands. It computes the best scalefac and
 *  global gain. This module calls the inner iteration loop.
 */

int shine_outer_loop(
    int max_bits,
    shine_psy_xmin_t *l3_xmin, /* the allowed distortion of the scalefactor */
    int ix[GRANULE_SIZE],      /* vector of quantized values ix(0..575) */
    int gr, int ch, shine_global_config *config) {
  int bits, huff_bits;
  shine_side_info_t *side_info = &config->side_info;
  gr_info *cod_info = &side_info->gr[gr].ch[ch].tt;

  cod_info->quantizerStepSize =
      bin_search_StepSize(max_bits, ix, cod_info, config);

  cod_info->part2_length = part2_length(gr, ch, config);
  huff_bits = max_bits - cod_info->part2_length;

  bits = shine_inner_loop(ix, huff_bits, cod_info, gr, ch, config);
  cod_info->part2_3_length = cod_info->part2_length + bits;

  return cod_info->part2_3_length;
}

/*
 * shine_iteration_loop:
 * ------------------
 */
void shine_iteration_loop(shine_global_config *config) {
  shine_psy_xmin_t l3_xmin;
  gr_info *cod_info;
  int max_bits;
  int ch, gr, i;
  int *ix;

  for (ch = config->wave.channels; ch--;) {
    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++) {
      /* setup pointers */
      ix = config->l3_enc[ch][gr];
      config->l3loop.xr = config->mdct_freq[ch][gr];

      /* Precalculate the square, abs,  and maximum,
       * for use later on.
       */
      for (i = GRANULE_SIZE, config->l3loop.xrmax = 0; i--;) {
        config->l3loop.xrsq[i] =
            mulsr(config->l3loop.xr[i], config->l3loop.xr[i]);
        config->l3loop.xrabs[i] = labs(config->l3loop.xr[i]);
        if (config->l3loop.xrabs[i] > config->l3loop.xrmax)
          config->l3loop.xrmax = config->l3loop.xrabs[i];
      }

      cod_info = (gr_info *)&(config->side_info.gr[gr].ch[ch]);
      cod_info->sfb_lmax = SFB_LMAX - 1; /* gr_deco */

      /* Window-switching: record the requested type and adjust the band
       * layout.  start/stop and normal are handled as long blocks; pure short
       * blocks use the 13 short bands in band/window/frequency order. */
      cod_info->window_switching = 0;
      cod_info->block_type = 0;
      cod_info->mixed_block_flag = 0;
      cod_info->subblock_gain[0] = 0;
      cod_info->subblock_gain[1] = 0;
      cod_info->subblock_gain[2] = 0;
      if (config->window_switching_active && gr < MAX_GRANULES &&
          ch < MAX_CHANNELS) {
        int bt = config->block_type[ch][gr];
        if (bt < 0 || bt > 3)
          bt = 0;
        cod_info->block_type = (unsigned)bt;
        cod_info->window_switching = (bt != 0);
      }

      calc_xmin(&config->ratio, cod_info, &l3_xmin, gr, ch);

      if (config->mpeg.version == MPEG_I && !cod_info->window_switching)
        calc_scfsi(&l3_xmin, ch, gr, config);
      else
        for (i = 4; i--;)
          config->side_info.scfsi[ch][i] = 0;

      /* calculation of number of available bit( per granule ) */
      max_bits = shine_max_reservoir_bits(&config->pe[ch][gr], config);

      /* reset of iteration variables */
      memset(config->scalefactor.l[gr][ch], 0,
             sizeof(config->scalefactor.l[gr][ch]));
      memset(config->scalefactor.s[gr][ch], 0,
             sizeof(config->scalefactor.s[gr][ch]));

      for (i = 4; i--;)
        cod_info->slen[i] = 0;

      cod_info->part2_3_length = 0;
      cod_info->big_values = 0;
      cod_info->count1 = 0;
      cod_info->scalefac_compress = 0;
      cod_info->table_select[0] = 0;
      cod_info->table_select[1] = 0;
      cod_info->table_select[2] = 0;
      cod_info->region0_count = 0;
      cod_info->region1_count = 0;
      cod_info->part2_length = 0;
      cod_info->preflag = 0;
      cod_info->scalefac_scale = 0;
      cod_info->count1table_select = 0;

      /* all spectral values zero ? */
      if (config->l3loop.xrmax)
        cod_info->part2_3_length =
            shine_outer_loop(max_bits, &l3_xmin, ix, gr, ch, config);

      shine_ResvAdjust(cod_info, config);
      cod_info->global_gain = cod_info->quantizerStepSize + 210;

#ifdef SHINE_TEST_HOOKS
      if (ch < SHINE_OBS_MAX_CHANNELS && gr < SHINE_OBS_MAX_GRANULES) {
        int t;
        for (t = 0; t < GRANULE_SIZE; t++) {
          /* ix[] from quantize() is unsigned; the transmitted sign comes from
           * the MDCT coefficient's sign, exactly as shine_format_bitstream
           * folds it in (negative xr with positive ix -> negative value). */
          int mag = ix[t];
          int neg = (config->mdct_freq[ch][gr][t] < 0) && (mag > 0);
          shine_obs_sign[ch][gr][t] = (int8_t)(neg ? -1 : (mag > 0 ? 1 : 0));
          shine_obs_l3_enc[ch][gr][t] = mag;
        }
        /* Inverse-reorder short blocks back to natural (subband, window,
         * frequency) coordinates: bitstream position band/window/freq ->
         * natural index. */
        if (cod_info->window_switching && cod_info->block_type == 2 &&
            !cod_info->mixed_block_flag) {
          const int *sb = shine_short_band_index[config->mpeg.samplerate_index];
          int sfb, w, f, inpos = 0;
          for (t = 0; t < GRANULE_SIZE; t++)
            shine_obs_l3_enc_nat[ch][gr][t] = 0;
          for (sfb = 0; sfb < 13; sfb++)
            for (w = 0; w < 3; w++)
              for (f = sb[sfb]; f < sb[sfb + 1]; f++) {
                int band = f / 6, line = f % 6;
                shine_obs_l3_enc_nat[ch][gr][band * 18 + w * 6 + line] =
                    shine_obs_l3_enc[ch][gr][inpos++];
              }
        } else {
          for (t = 0; t < GRANULE_SIZE; t++)
            shine_obs_l3_enc_nat[ch][gr][t] = shine_obs_l3_enc[ch][gr][t];
        }
        {
          struct shine_obs_side *os = &shine_obs_side[ch][gr];
          os->part2_3_length = (int)cod_info->part2_3_length;
          os->part2_length = (int)cod_info->part2_length;
          os->big_values = (int)cod_info->big_values;
          os->count1 = (int)cod_info->count1;
          os->global_gain = (int)cod_info->global_gain;
          os->scalefac_compress = (int)cod_info->scalefac_compress;
          os->window_switching = (int)cod_info->window_switching;
          os->block_type = (int)cod_info->block_type;
          os->mixed_block_flag = (int)cod_info->mixed_block_flag;
          for (t = 0; t < 3; t++) {
            os->table_select[t] = (int)cod_info->table_select[t];
            os->subblock_gain[t] = (int)cod_info->subblock_gain[t];
          }
          os->region0_count = (int)cod_info->region0_count;
          os->region1_count = (int)cod_info->region1_count;
          os->scalefac_scale = (int)cod_info->scalefac_scale;
          os->count1table_select = (int)cod_info->count1table_select;
          os->preflag = (int)cod_info->preflag;
        }
      }
#endif

    } /* for gr */
  }   /* for ch */

  shine_ResvFrameEnd(config);

#ifdef SHINE_TEST_HOOKS
  /* Stuffing may have been added to part2_3_length by the reservoir code. */
  if (config->wave.channels <= SHINE_OBS_MAX_CHANNELS) {
    int c2, g2;
    for (c2 = 0; c2 < config->wave.channels; c2++)
      for (g2 = 0; g2 < config->mpeg.granules_per_frame; g2++) {
        gr_info *ci = &config->side_info.gr[g2].ch[c2].tt;
        shine_obs_side[c2][g2].part2_3_length = (int)ci->part2_3_length;
      }
  }
#endif
}

/*
 * calc_scfsi:
 * -----------
 * calculation of the scalefactor select information ( scfsi ).
 */
void calc_scfsi(shine_psy_xmin_t *l3_xmin, int ch, int gr,
                shine_global_config *config) {
  shine_side_info_t *l3_side = &config->side_info;
  /* This is the scfsi_band table from 2.4.2.7 of the IS */
  static const int scfsi_band_long[5] = {0, 6, 11, 16, 21};

  int scfsi_band;

  int sfb, start, end, i;
  int condition = 0;
  int temp;

  const int *scalefac_band_long =
      &shine_scale_fact_band_index[config->mpeg.samplerate_index][0];

  /* note. it goes quite a bit faster if you uncomment the next bit and exit
     early from scfsi, but you then loose the advantage of common scale factors.

  for(scfsi_band=0;scfsi_band<4;scfsi_band++)
    l3_side->scfsi[ch][scfsi_band] = 0;
  return;

  */

  config->l3loop.xrmaxl[gr] = config->l3loop.xrmax;

  /* the total energy of the granule */
  for (temp = 0, i = GRANULE_SIZE; i--;)
    temp += config->l3loop.xrsq[i] >>
            10; /* a bit of scaling to avoid overflow, (not very good) */
  if (temp)
    config->l3loop.en_tot[gr] =
        log((double)temp * 4.768371584e-7) / LN2; /* 1024 / 0x7fffffff */
  else
    config->l3loop.en_tot[gr] = 0;

  /* the energy of each scalefactor band, en */
  /* the allowed distortion of each scalefactor band, xm */

  for (sfb = 21; sfb--;) {
    start = scalefac_band_long[sfb];
    end = scalefac_band_long[sfb + 1];

    for (temp = 0, i = start; i < end; i++)
      temp += config->l3loop.xrsq[i] >> 10;
    if (temp)
      config->l3loop.en[gr][sfb] =
          log((double)temp * 4.768371584e-7) / LN2; /* 1024 / 0x7fffffff */
    else
      config->l3loop.en[gr][sfb] = 0;

    if (l3_xmin->l[gr][ch][sfb])
      config->l3loop.xm[gr][sfb] = log(l3_xmin->l[gr][ch][sfb]) / LN2;
    else
      config->l3loop.xm[gr][sfb] = 0;
  }

  if (gr == 1) {
    int gr2, tp;

    for (gr2 = 2; gr2--;) {
      /* The spectral values are not all zero */
      if (config->l3loop.xrmaxl[gr2])
        condition++;

      condition++;
    }
    if (abs(config->l3loop.en_tot[0] - config->l3loop.en_tot[1]) < en_tot_krit)
      condition++;
    for (tp = 0, sfb = 21; sfb--;)
      tp += abs(config->l3loop.en[0][sfb] - config->l3loop.en[1][sfb]);
    if (tp < en_dif_krit)
      condition++;

    if (condition == 6) {
      for (scfsi_band = 0; scfsi_band < 4; scfsi_band++) {
        int sum0 = 0, sum1 = 0;
        l3_side->scfsi[ch][scfsi_band] = 0;
        start = scfsi_band_long[scfsi_band];
        end = scfsi_band_long[scfsi_band + 1];
        for (sfb = start; sfb < end; sfb++) {
          sum0 += abs(config->l3loop.en[0][sfb] - config->l3loop.en[1][sfb]);
          sum1 += abs(config->l3loop.xm[0][sfb] - config->l3loop.xm[1][sfb]);
        }

        if (sum0 < en_scfsi_band_krit && sum1 < xm_scfsi_band_krit) {
          l3_side->scfsi[ch][scfsi_band] = 1;
        } else
          l3_side->scfsi[ch][scfsi_band] = 0;
      } /* for scfsi_band */
    }   /* if condition == 6 */
    else
      for (scfsi_band = 0; scfsi_band < 4; scfsi_band++)
        l3_side->scfsi[ch][scfsi_band] = 0;
  } /* if gr == 1 */
}

/*
 * part2_length:
 * -------------
 * calculates the number of bits needed to encode the scalefacs in the
 * main data block.
 */
int part2_length(int gr, int ch, shine_global_config *config) {
  int slen1, slen2, bits;
  gr_info *gi = &config->side_info.gr[gr].ch[ch].tt;

  bits = 0;

  {
    slen1 = shine_slen1_tab[gi->scalefac_compress];
    slen2 = shine_slen2_tab[gi->scalefac_compress];

    if (gi->window_switching) {
      /* Window-switched granules never use scfsi.  Short blocks carry 13
       * bands x 3 windows (sfb 0..5 use slen1, 6..12 use slen2); start/stop
       * carry the 21 long scalefactors (sfb 0..10 slen1, 11..20 slen2). */
      if (shine_l3_is_short(gi))
        bits += 3 * (6 * slen1 + 7 * slen2);
      else
        bits += 11 * slen1 + 10 * slen2;
      return bits;
    }

    if (!gr || !(config->side_info.scfsi[ch][0]))
      bits += (6 * slen1);

    if (!gr || !(config->side_info.scfsi[ch][1]))
      bits += (5 * slen1);

    if (!gr || !(config->side_info.scfsi[ch][2]))
      bits += (5 * slen2);

    if (!gr || !(config->side_info.scfsi[ch][3]))
      bits += (5 * slen2);
  }
  return bits;
}

/*
 * calc_xmin:
 * ----------
 * Calculate the allowed distortion for each scalefactor band,
 * as determined by the psychoacoustic model.
 * xmin(sb) = ratio(sb) * en(sb) / bw(sb)
 */
void calc_xmin(shine_psy_ratio_t *ratio, gr_info *cod_info,
               shine_psy_xmin_t *l3_xmin, int gr, int ch) {
  int sfb;

  for (sfb = cod_info->sfb_lmax; sfb--;) {
    /*  note. xmin will always be zero with no psychoacoustic model

        start = scalefac_band_long[ sfb ];
        end   = scalefac_band_long[ sfb+1 ];
        bw = end - start;

        for ( en = 0, l = start; l < end; l++ )
          en += config->l3loop.xrsq[l];

        l3_xmin->l[gr][ch][sfb] = ratio->l[gr][ch][sfb] * en / bw;
    */
    l3_xmin->l[gr][ch][sfb] = 0;
  }
}

/*
 * shine_loop_initialise:
 * -------------------
 * Calculates the look up tables used by the iteration loop.
 */
void shine_loop_initialise(shine_global_config *config) {
  int i;

  /* quantize: stepsize conversion, fourth root of 2 table.
   * The table is inverted (negative power) from the equation given
   * in the spec because it is quicker to do x*y than x/y.
   * The 0.5 is for rounding.
   */
  for (i = 128; i--;) {
    config->l3loop.steptab[i] = pow(2.0, (double)(127 - i) / 4);
    if ((config->l3loop.steptab[i] * 2) >
        0x7fffffff) /* MAXINT = 2**31 = 2**(124/4) */
      config->l3loop.steptabi[i] = 0x7fffffff;
    else
      /* The table is multiplied by 2 to give an extra bit of accuracy.
       * In quantize, the long multiply does not shift it's result left one
       * bit to compensate.
       */
      config->l3loop.steptabi[i] =
          (int32_t)((config->l3loop.steptab[i] * 2) + 0.5);
  }

  /* quantize: vector conversion, three quarter power table.
   * The 0.5 is for rounding, the .0946 comes from the spec.
   */
  for (i = 10000; i--;)
    config->l3loop.int2idx[i] =
        (int)(sqrt(sqrt((double)i) * (double)i) - 0.0946 + 0.5);
}

/*
 * quantize:
 * ---------
 * Function: Quantization of the vector xr ( -> ix).
 * Returns maximum value of ix.
 */
int quantize(int ix[GRANULE_SIZE], int stepsize, shine_global_config *config) {
  int i, max, ln;
  int32_t scalei;
  double scale, dbl;

  scalei = config->l3loop.steptabi[stepsize + 127]; /* 2**(-stepsize/4) */

  /* a quick check to see if ixmax will be less than 8192 */
  /* this speeds up the early calls to bin_search_StepSize */
  if ((mulr(config->l3loop.xrmax, scalei)) > 165140) /* 8192**(4/3) */
    max = 16384; /* no point in continuing, stepsize not big enough */
  else
    for (i = 0, max = 0; i < GRANULE_SIZE; i++) {
      /* This calculation is very sensitive. The multiply must round it's
       * result or bad things happen to the quality.
       */
      ln = mulr(labs(config->l3loop.xr[i]), scalei);

      if (ln < 10000)                       /* ln < 10000 catches most values */
        ix[i] = config->l3loop.int2idx[ln]; /* quick look up method */
      else {
        /* outside table range so have to do it using floats */
        scale = config->l3loop.steptab[stepsize + 127]; /* 2**(-stepsize/4) */
        dbl = ((double)config->l3loop.xrabs[i]) * scale *
              4.656612875e-10;              /* 0x7fffffff */
        ix[i] = (int)sqrt(sqrt(dbl) * dbl); /* dbl**(3/4) */
      }

      /* calculate ixmax while we're here */
      /* note. ix cannot be negative */
      if (max < ix[i])
        max = ix[i];
    }

  return max;
}

/*
 * ix_max:
 * -------
 * Function: Calculate the maximum of ix from 0 to 575
 */
static inline int ix_max(int ix[GRANULE_SIZE], unsigned int begin,
                         unsigned int end) {
  register int i;
  register int max = 0;

  for (i = begin; i < end; i++)
    if (max < ix[i])
      max = ix[i];
  return max;
}

/*
 * calc_runlen:
 * ------------
 * Function: Calculation of rzero, count1, big_values
 * (Partitions ix into big values, quadruples and zeros).
 */
void calc_runlen(int ix[GRANULE_SIZE], gr_info *cod_info) {
  int i;
  int rzero = 0;

  for (i = GRANULE_SIZE; i > 1; i -= 2)
    if (!ix[i - 1] && !ix[i - 2])
      rzero++;
    else
      break;

  cod_info->count1 = 0;
  for (; i > 3; i -= 4)
    if (ix[i - 1] <= 1 && ix[i - 2] <= 1 && ix[i - 3] <= 1 && ix[i - 4] <= 1)
      cod_info->count1++;
    else
      break;

  cod_info->big_values = i >> 1;
}

/*
 * count1_bitcount:
 * ----------------
 * Determines the number of bits to encode the quadruples.
 */
int count1_bitcount(int ix[GRANULE_SIZE], gr_info *cod_info) {
  int p, i, k;
  int v, w, x, y, signbits;
  int sum0 = 0, sum1 = 0;

  for (i = cod_info->big_values << 1, k = 0; k < cod_info->count1;
       i += 4, k++) {
    v = ix[i];
    w = ix[i + 1];
    x = ix[i + 2];
    y = ix[i + 3];

    p = v + (w << 1) + (x << 2) + (y << 3);

    signbits = 0;
    if (v != 0)
      signbits++;
    if (w != 0)
      signbits++;
    if (x != 0)
      signbits++;
    if (y != 0)
      signbits++;

    sum0 += signbits;
    sum1 += signbits;

    sum0 += shine_huffman_table[32].hlen[p];
    sum1 += shine_huffman_table[33].hlen[p];
  }

  if (sum0 < sum1) {
    cod_info->count1table_select = 0;
    return sum0;
  } else {
    cod_info->count1table_select = 1;
    return sum1;
  }
}

/*
 * subdivide:
 * ----------
 * presumable subdivides the bigvalue region which will use separate Huffman
 * tables.
 */
void subdivide(gr_info *cod_info, shine_global_config *config) {
  static const struct {
    unsigned region0_count;
    unsigned region1_count;
  } subdv_table[23] = {
      {0, 0}, /* 0 bands */
      {0, 0}, /* 1 bands */
      {0, 0}, /* 2 bands */
      {0, 0}, /* 3 bands */
      {0, 0}, /* 4 bands */
      {0, 1}, /* 5 bands */
      {1, 1}, /* 6 bands */
      {1, 1}, /* 7 bands */
      {1, 2}, /* 8 bands */
      {2, 2}, /* 9 bands */
      {2, 3}, /* 10 bands */
      {2, 3}, /* 11 bands */
      {3, 4}, /* 12 bands */
      {3, 4}, /* 13 bands */
      {3, 4}, /* 14 bands */
      {4, 5}, /* 15 bands */
      {4, 5}, /* 16 bands */
      {4, 6}, /* 17 bands */
      {5, 6}, /* 18 bands */
      {5, 6}, /* 19 bands */
      {5, 7}, /* 20 bands */
      {6, 7}, /* 21 bands */
      {6, 7}, /* 22 bands */
  };

  if (!cod_info->big_values) { /* no big_values region */
    cod_info->region0_count = 0;
    cod_info->region1_count = 0;
  } else if (cod_info->window_switching) {
    /* Window-switched granule (start, short or stop): the partition is
     * implicit.  Region 0 covers the first 36 lines, region 1 the rest up to
     * the big-values end.  Only two table_select fields are transmitted, so
     * table_select[2] is unused.  Pure short blocks additionally default to
     * region0_count=8 / region1_count=12; the 36-line region boundary is the
     * same for all switched types. */
    int bigvalues_region = 2 * cod_info->big_values;

    cod_info->region0_count = shine_l3_is_short(cod_info) ? 8 : 7;
    cod_info->region1_count = 20 - cod_info->region0_count;
    cod_info->address1 = 36;
    if (cod_info->address1 > bigvalues_region)
      cod_info->address1 = bigvalues_region;
    cod_info->address2 = bigvalues_region;
    cod_info->address3 = bigvalues_region;
  } else {
    const int *scalefac_band_long =
        &shine_scale_fact_band_index[config->mpeg.samplerate_index][0];
    int bigvalues_region, scfb_anz, thiscount;

    bigvalues_region = 2 * cod_info->big_values;

    /* Calculate scfb_anz */
    scfb_anz = 0;
    while (scalefac_band_long[scfb_anz] < bigvalues_region)
      scfb_anz++;

    for (thiscount = subdv_table[scfb_anz].region0_count; thiscount;
         thiscount--) {
      if (scalefac_band_long[thiscount + 1] <= bigvalues_region)
        break;
    }
    cod_info->region0_count = thiscount;
    cod_info->address1 = scalefac_band_long[thiscount + 1];

    scalefac_band_long += cod_info->region0_count + 1;

    for (thiscount = subdv_table[scfb_anz].region1_count; thiscount;
         thiscount--) {
      if (scalefac_band_long[thiscount + 1] <= bigvalues_region)
        break;
    }
    cod_info->region1_count = thiscount;
    cod_info->address2 = scalefac_band_long[thiscount + 1];

    cod_info->address3 = bigvalues_region;
  }
}

/*
 * bigv_tab_select:
 * ----------------
 * Function: Select huffman code tables for bigvalues regions
 */
void bigv_tab_select(int ix[GRANULE_SIZE], gr_info *cod_info) {
  cod_info->table_select[0] = 0;
  cod_info->table_select[1] = 0;
  cod_info->table_select[2] = 0;

  {
    if (cod_info->address1 > 0)
      cod_info->table_select[0] = new_choose_table(ix, 0, cod_info->address1);

    if (cod_info->address2 > cod_info->address1)
      cod_info->table_select[1] =
          new_choose_table(ix, cod_info->address1, cod_info->address2);

    /* Window-switched granules only transmit two tables; the first two
     * regions always cover the whole big-values area (see subdivide). */
    if (!cod_info->window_switching &&
        cod_info->big_values << 1 > cod_info->address2)
      cod_info->table_select[2] =
          new_choose_table(ix, cod_info->address2, cod_info->big_values << 1);
  }
}

/*
 * new_choose_table:
 * -----------------
 * Choose the Huffman table that will encode ix[begin..end] with
 * the fewest bits.
 * Note: This code contains knowledge about the sizes and characteristics
 * of the Huffman tables as defined in the IS (Table B.7), and will not work
 * with any arbitrary tables.
 */
int new_choose_table(int ix[GRANULE_SIZE], unsigned int begin,
                     unsigned int end) {
  int i, max;
  int choice[2];
  int sum[2];

  max = ix_max(ix, begin, end);
  if (!max)
    return 0;

  choice[0] = 0;
  choice[1] = 0;

  if (max < 15) {
    /* try tables with no linbits */
    for (i = 14; i--;)
      if (shine_huffman_table[i].xlen > max) {
        choice[0] = i;
        break;
      }

    sum[0] = count_bit(ix, begin, end, choice[0]);

    switch (choice[0]) {
    case 2:
      sum[1] = count_bit(ix, begin, end, 3);
      if (sum[1] <= sum[0])
        choice[0] = 3;
      break;

    case 5:
      sum[1] = count_bit(ix, begin, end, 6);
      if (sum[1] <= sum[0])
        choice[0] = 6;
      break;

    case 7:
      sum[1] = count_bit(ix, begin, end, 8);
      if (sum[1] <= sum[0]) {
        choice[0] = 8;
        sum[0] = sum[1];
      }
      sum[1] = count_bit(ix, begin, end, 9);
      if (sum[1] <= sum[0])
        choice[0] = 9;
      break;

    case 10:
      sum[1] = count_bit(ix, begin, end, 11);
      if (sum[1] <= sum[0]) {
        choice[0] = 11;
        sum[0] = sum[1];
      }
      sum[1] = count_bit(ix, begin, end, 12);
      if (sum[1] <= sum[0])
        choice[0] = 12;
      break;

    case 13:
      sum[1] = count_bit(ix, begin, end, 15);
      if (sum[1] <= sum[0])
        choice[0] = 15;
      break;
    }
  } else {
    /* try tables with linbits */
    max -= 15;

    for (i = 15; i < 24; i++)
      if (shine_huffman_table[i].linmax >= max) {
        choice[0] = i;
        break;
      }

    for (i = 24; i < 32; i++)
      if (shine_huffman_table[i].linmax >= max) {
        choice[1] = i;
        break;
      }

    sum[0] = count_bit(ix, begin, end, choice[0]);
    sum[1] = count_bit(ix, begin, end, choice[1]);
    if (sum[1] < sum[0])
      choice[0] = choice[1];
  }
  return choice[0];
}

/*
 * bigv_bitcount:
 * --------------
 * Function: Count the number of bits necessary to code the bigvalues region.
 */
int bigv_bitcount(int ix[GRANULE_SIZE], gr_info *gi) {
  int bits = 0;
  unsigned int table;

  if ((table = gi->table_select[0])) /* region0 */
    bits += count_bit(ix, 0, gi->address1, table);
  if ((table = gi->table_select[1])) /* region1 */
    bits += count_bit(ix, gi->address1, gi->address2, table);
  if ((table = gi->table_select[2])) /* region2 */
    bits += count_bit(ix, gi->address2, gi->address3, table);
  return bits;
}

/*
 * count_bit:
 * ----------
 * Function: Count the number of bits necessary to code the subregion.
 */
int count_bit(int ix[GRANULE_SIZE], unsigned int start, unsigned int end,
              unsigned int table) {
  unsigned linbits, ylen;
  register int i, sum;
  register int x, y;
  const struct huffcodetab *h;

  if (!table)
    return 0;

  h = &(shine_huffman_table[table]);
  sum = 0;

  ylen = h->ylen;
  linbits = h->linbits;

  if (table > 15) { /* ESC-table is used */
    for (i = start; i < end; i += 2) {
      x = ix[i];
      y = ix[i + 1];
      if (x > 14) {
        x = 15;
        sum += linbits;
      }
      if (y > 14) {
        y = 15;
        sum += linbits;
      }

      sum += h->hlen[(x * ylen) + y];
      if (x)
        sum++;
      if (y)
        sum++;
    }
  } else { /* No ESC-words */
    for (i = start; i < end; i += 2) {
      x = ix[i];
      y = ix[i + 1];

      sum += h->hlen[(x * ylen) + y];

      if (x != 0)
        sum++;
      if (y != 0)
        sum++;
    }
  }
  return sum;
}

/*
 * bin_search_StepSize:
 * --------------------
 * Succesive approximation approach to obtaining a initial quantizer
 * step size.
 * The following optional code written by Seymour Shlien
 * will speed up the shine_outer_loop code which is called
 * by iteration_loop. When BIN_SEARCH is defined, the
 * shine_outer_loop function precedes the call to the function shine_inner_loop
 * with a call to bin_search gain defined below, which
 * returns a good starting quantizerStepSize.
 */
int bin_search_StepSize(int desired_rate, int ix[GRANULE_SIZE],
                        gr_info *cod_info, shine_global_config *config) {
  int bit, next, count;

  next = -120;
  count = 120;

  do {
    int half = count / 2;

    if (quantize(ix, next + half, config) > 8192)
      bit = 100000; /* fail */
    else {
      calc_runlen(ix, cod_info);           /* rzero,count1,big_values */
      bit = count1_bitcount(ix, cod_info); /* count1_table selection */
      subdivide(cod_info, config);         /* bigvalues sfb division */
      bigv_tab_select(ix, cod_info);       /* codebook selection */
      bit += bigv_bitcount(ix, cod_info);  /* bit count */
    }

    if (bit < desired_rate)
      count = half;
    else {
      next += half;
      count -= half;
    }
  } while (count > 1);

  return next;
}
