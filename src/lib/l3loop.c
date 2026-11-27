/* l3loop.c */

#include "l3loop.h"
#include "bitstream.h"
#include "huffman.h"
#include "l3bitstream.h"
#include "layer3.h"
#include "reservoir.h"
#include "tables.h"
#include "types.h"

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
static void shine_legacy_iteration_loop(shine_global_config *config);

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
  if (config->mask_ratios)
    shine_masked_iteration_loop(config);
  else
    shine_legacy_iteration_loop(config);
}

static void shine_legacy_iteration_loop(shine_global_config *config) {
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

      calc_xmin(&config->ratio, cod_info, &l3_xmin, gr, ch);

      if (config->mpeg.version == MPEG_I)
        calc_scfsi(&l3_xmin, ch, gr, config);

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

    } /* for gr */
  }   /* for ch */

  shine_ResvFrameEnd(config);
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

    if (cod_info->big_values << 1 > cod_info->address2)
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

/* ===================================================================
 * Masked outer loop
 *
 * The legacy loop above only adjusts the global gain: every long-window
 * scalefactor is transmitted as zero. This extension lets caller supplied
 * masking ratios drive real per-band scalefactors (preflag = 0,
 * scalefac_scale = 0, bands 0..20 in 0..7, band 21 implicitly zero), while
 * the global gain keeps each granule inside the same CBR capacity the
 * legacy encoder would have used.
 * =================================================================== */

#define MASK_SFB 22
#define MASK_MAX_SF 7
#define MASK_OUTER_LIMIT 200

typedef struct {
  int sf[21];                  /* transmitted scalefactors, band 21 is 0 */
  int step;                    /* quantizerStepSize (global gain - 210) */
  int scalefac_compress;
  int slen1, slen2;
  int part2;                   /* scalefactor bits */
  int huff;                    /* Huffman bits */
  int part2_3;
  int feasible;
  double noise[MASK_SFB];      /* normalized noise D / (ratio * E) */
  double worst;
  gr_info gi;
  int ix[GRANULE_SIZE];
} mask_cand_t;

/* Quantize one spectral magnitude with an effective per-band step size.
 * The band scalefactor f turns the exponent step/4 - f/2 into the global
 * exponent (step - 2*f)/4, so the band quantizes with step - 2*f. */
static int mask_quant_sample(int32_t xrabs, int bstep,
                             shine_global_config *config) {
  int32_t scalei;
  int ln;

  if (bstep < -127)
    bstep = -127;
  if (bstep > 0)
    bstep = 0;

  scalei = config->l3loop.steptabi[bstep + 127]; /* 2**(-bstep/4) */
  ln = mulr(xrabs, scalei);

  if (ln < 10000)
    return config->l3loop.int2idx[ln];

  {
    double scale = config->l3loop.steptab[bstep + 127];
    double dbl = ((double)xrabs) * scale * 4.656612875e-10;
    return (int)sqrt(sqrt(dbl) * dbl);
  }
}

static void mask_quantize_bands(mask_cand_t *c,
                                shine_global_config *config) {
  const int *band =
      &shine_scale_fact_band_index[config->mpeg.samplerate_index][0];
  int b, i;

  for (b = 0; b < MASK_SFB; b++) {
    int bstep = c->step - 2 * (b < 21 ? c->sf[b] : 0);
    for (i = band[b]; i < band[b + 1]; i++)
      c->ix[i] = mask_quant_sample(config->l3loop.xrabs[i], bstep, config);
  }
}

/* Smallest slen width that can represent value v. */
static int mask_width_needed(int v) {
  int bits = 0;
  while ((1 << bits) <= v)
    bits++;
  return bits;
}

/* Pick the MPEG-I scalefac_compress index with the smallest granule-0
 * part2 cost that can still represent every transmitted value; ties go to
 * the smaller index. */
static int mask_pick_compress(const int *sf, int *slen1_out, int *slen2_out) {
  int b, m1 = 0, m2 = 0, need1, need2, best = 0, best_cost = 0;
  int found = 0;

  for (b = 0; b <= 10; b++)
    if (sf[b] > m1)
      m1 = sf[b];
  for (b = 11; b <= 20; b++)
    if (sf[b] > m2)
      m2 = sf[b];

  need1 = mask_width_needed(m1);
  need2 = mask_width_needed(m2);

  for (b = 0; b < 16; b++) {
    int s1 = shine_slen1_tab[b], s2 = shine_slen2_tab[b];
    int cost;
    if (s1 < need1 || s2 < need2)
      continue;
    cost = 11 * s1 + 10 * s2; /* all 21 bands present, as in granule 0 */
    if (!found || cost < best_cost) {
      found = 1;
      best = b;
      best_cost = cost;
      *slen1_out = s1;
      *slen2_out = s2;
    }
  }
  return best;
}

static int mask_part2_bits(int slen1, int slen2, const int scfsi[4], int gr) {
  int bits = 11 * slen1 + 10 * slen2;

  if (gr) {
    if (scfsi[0])
      bits -= 6 * slen1;
    if (scfsi[1])
      bits -= 5 * slen1;
    if (scfsi[2])
      bits -= 5 * slen2;
    if (scfsi[3])
      bits -= 5 * slen2;
  }
  return bits;
}

/* Reconstruct one band and return its normalized noise D / (ratio * E),
 * with both E and D in the natural mdct_freq / 2^31 spectral units. */
static double mask_band_noise(int sfb, const int *ix, int step, const int *sf,
                              double ratio, shine_global_config *config) {
  const int *band =
      &shine_scale_fact_band_index[config->mpeg.samplerate_index][0];
  int start = band[sfb], end = band[sfb + 1];
  int scalefac = sfb < 21 ? sf[sfb] : 0;
  double gain = pow(2.0, (double)step / 4.0 - (double)scalefac / 2.0);
  double E = 0.0, D = 0.0, xr, rec;
  int i;

  for (i = start; i < end; i++) {
    int32_t v = config->l3loop.xr[i];
    xr = (double)v / 2147483648.0;
    rec = (ix[i] ? (ix[i] < 0 ? -1.0 : 1.0) *
                       pow((double)abs(ix[i]), 4.0 / 3.0) * gain
                 : 0.0);
    E += xr * xr;
    D += (xr - rec) * (xr - rec);
  }

  if (E == 0.0)
    return D == 0.0 ? 0.0 : INFINITY;
  return D / (ratio * E);
}

static void mask_compute_noise(mask_cand_t *c, const double *ratios,
                               shine_global_config *config) {
  int b;

  c->worst = 0.0;
  for (b = 0; b < MASK_SFB; b++) {
    c->noise[b] = mask_band_noise(b, c->ix, c->step, c->sf, ratios[b], config);
    if (c->noise[b] > c->worst)
      c->worst = c->noise[b];
  }
}

/* Fully evaluate a candidate: choose the scalefactor compression, quantize
 * every band, run the Huffman region selection against the remaining budget
 * (raising the global step like the legacy inner loop) and score the band
 * noise. scfsi is assumed all-zero while searching; granule 1 reuses are
 * resolved once its final scalefactors exist. */
static void mask_evaluate(mask_cand_t *c, int gr, int max_bits,
                          const int scfsi[4], const double *ratios,
                          shine_global_config *config) {
  int budget;

  c->scalefac_compress =
      mask_pick_compress(c->sf, &c->slen1, &c->slen2);
  c->part2 = mask_part2_bits(c->slen1, c->slen2, scfsi, gr);
  budget = max_bits - c->part2;
  c->feasible = 0;

  if (budget < 0)
    return;

  for (;;) {
    int maxix;

    mask_quantize_bands(c, config);
    maxix = ix_max(c->ix, 0, GRANULE_SIZE);

    memset(&c->gi, 0, sizeof(c->gi));
    calc_runlen(c->ix, &c->gi);
    c->huff = count1_bitcount(c->ix, &c->gi);
    subdivide(&c->gi, config);
    bigv_tab_select(c->ix, &c->gi);
    c->huff += bigv_bitcount(c->ix, &c->gi);

    if (maxix <= 8192 && c->huff <= budget)
      break;

    if (c->step >= 0)
      return; /* cannot coarsen any further: infeasible */
    c->step++;
  }

  if (c->part2 + c->huff > 4095)
    return;

  c->part2_3 = c->part2 + c->huff;
  c->feasible = 1;
  mask_compute_noise(c, ratios, config);
}

/* Run the legacy all-zero scalefactor candidate through the untouched
 * production outer loop so the fallback is exactly the old behaviour. */
static void mask_legacy_candidate(mask_cand_t *c, int gr, int ch, int max_bits,
                                  const double *ratios,
                                  shine_global_config *config) {
  shine_psy_xmin_t l3_xmin;
  gr_info *gi = &config->side_info.gr[gr].ch[ch].tt;
  int b;

  memset(c, 0, sizeof(*c));
  memset(c->sf, 0, sizeof(c->sf));

  if (!config->l3loop.xrmax) {
    c->step = 0;
    c->feasible = 1;
    mask_compute_noise(c, ratios, config);
    return;
  }

  memset(&l3_xmin, 0, sizeof(l3_xmin));
  memset(config->scalefactor.l[gr][ch], 0,
         sizeof(config->scalefactor.l[gr][ch]));
  memset(gi, 0, sizeof(*gi));
  gi->sfb_lmax = SFB_LMAX - 1;

  c->part2_3 = shine_outer_loop(max_bits, &l3_xmin, config->l3_enc[ch][gr], gr,
                                ch, config);
  c->step = gi->quantizerStepSize;
  c->gi = *gi;
  memcpy(c->ix, config->l3_enc[ch][gr], sizeof(c->ix));
  c->huff = c->part2_3; /* legacy part2 is zero */
  c->part2 = 0;
  c->slen1 = c->slen2 = 0;
  c->feasible = 1;
  mask_compute_noise(c, ratios, config);

  for (b = 0; b < 21; b++)
    config->scalefactor.l[gr][ch][b] = 0;
}

void shine_masked_iteration_loop(shine_global_config *config) {
  int channels = config->wave.channels;
  int ch, gr;

  /* Start from a clean slate: SCFSI is recomputed per channel below and no
   * value from a previous frame may leak into a freshly encoded one. */
  memset(config->side_info.scfsi, 0, sizeof(config->side_info.scfsi));

  for (ch = 0; ch < channels; ch++) {
    int scfsi[MAX_CHANNELS][4];
    mask_cand_t best[MAX_GRANULES];
    memset(scfsi, 0, sizeof(scfsi));

    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++) {
      const double *ratios =
          config->mask_ratios + ((gr * channels + ch) * MASK_SFB);
      int max_bits;
      mask_cand_t cur;
      int i, b;

      config->l3loop.xr = config->mdct_freq[ch][gr];
      for (i = GRANULE_SIZE, config->l3loop.xrmax = 0; i--;) {
        config->l3loop.xrsq[i] =
            mulsr(config->l3loop.xr[i], config->l3loop.xr[i]);
        config->l3loop.xrabs[i] = labs(config->l3loop.xr[i]);
        if (config->l3loop.xrabs[i] > config->l3loop.xrmax)
          config->l3loop.xrmax = config->l3loop.xrabs[i];
      }

      max_bits = shine_max_reservoir_bits(&config->pe[ch][gr], config);

      /* Seed with the legacy candidate; it is the guaranteed fallback. */
      mask_legacy_candidate(&best[gr], gr, ch, max_bits, ratios, config);

#ifdef SHINE_TEST_HOOKS
      memcpy(config->test_mdct_freq[ch][gr], config->mdct_freq[ch][gr],
             sizeof(config->test_mdct_freq[ch][gr]));
      config->test_legacy[ch][gr].present = config->l3loop.xrmax != 0;
      config->test_legacy[ch][gr].quantizerStepSize = best[gr].step;
      config->test_legacy[ch][gr].part2_3_length = best[gr].part2_3;
      memcpy(config->test_legacy[ch][gr].l3_enc, best[gr].ix,
             sizeof(config->test_legacy[ch][gr].l3_enc));
#endif

      if (config->l3loop.xrmax) {
        int no_scfsi[4] = {0, 0, 0, 0};

        memset(&cur, 0, sizeof(cur));
        cur.step = best[gr].step;

        for (i = 0; i < MASK_OUTER_LIMIT; i++) {
          int target = -1;

          mask_evaluate(&cur, gr, max_bits, no_scfsi, ratios, config);
          if (!cur.feasible) {
            /* The global step already saturated or the scalefactors alone
             * overflow the budget; raising more factors cannot help. */
            break;
          }
          if (cur.worst < best[gr].worst)
            best[gr] = cur; /* strictly better; ties keep the earlier one */

          for (b = 0; b < MASK_SFB - 1; b++) {
            if (cur.sf[b] >= MASK_MAX_SF)
              continue;
            if (cur.noise[b] > 1.0 &&
                (target < 0 || cur.noise[b] > cur.noise[target]))
              target = b;
          }
          if (target < 0)
            break;
          cur.sf[target]++;
        }
      }

      /* Granule 1 may reuse granule 0 scalefactor groups, but only when
       * every transmitted value of the group matches exactly. */
      if (gr == 1) {
        static const int group_start[4] = {0, 6, 11, 16};
        static const int group_end[4] = {6, 11, 16, 21};
        int g;

        for (g = 0; g < 4; g++) {
          scfsi[ch][g] = 1;
          for (b = group_start[g]; b < group_end[g]; b++)
            if (best[1].sf[b] != best[0].sf[b]) {
              scfsi[ch][g] = 0;
              break;
            }
        }

        /* Re-select compression against the bytes actually transmitted. */
        best[1].scalefac_compress = mask_pick_compress(
            best[1].sf, &best[1].slen1, &best[1].slen2);
        best[1].part2 =
            mask_part2_bits(best[1].slen1, best[1].slen2, scfsi[ch], 1);
        best[1].part2_3 = best[1].part2 + best[1].huff;
      }

      /* Commit the winner to the encoder state. */
      {
        gr_info *gi = &config->side_info.gr[gr].ch[ch].tt;
        *gi = best[gr].gi;
        gi->sfb_lmax = SFB_LMAX - 1;
        gi->quantizerStepSize = best[gr].step;
        gi->global_gain = best[gr].step + 210;
        gi->scalefac_compress = best[gr].scalefac_compress;
        gi->slen[0] = gi->slen[1] = best[gr].slen1;
        gi->slen[2] = gi->slen[3] = best[gr].slen2;
        gi->part2_length = best[gr].part2;
        gi->part2_3_length = best[gr].part2_3;
        gi->preflag = 0;
        gi->scalefac_scale = 0;

        for (b = 0; b < 21; b++)
          config->scalefactor.l[gr][ch][b] = best[gr].sf[b];
        config->scalefactor.l[gr][ch][21] = 0;
        memset(config->scalefactor.s[gr][ch], 0,
               sizeof(config->scalefactor.s[gr][ch]));
        memcpy(config->l3_enc[ch][gr], best[gr].ix,
               sizeof(config->l3_enc[ch][gr]));

        for (b = 0; b < 4; b++)
          config->side_info.scfsi[ch][b] = gr == 0 ? 0 : scfsi[ch][b];

        shine_ResvAdjust(gi, config);
      }
    }
  }

  shine_ResvFrameEnd(config);
}
