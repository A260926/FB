/* L3mdct */

#include "l3mdct.h"
#include "l3subband.h"
#include "l3window.h"
#include "layer3.h"
#include "types.h"

/* This is table B.9: coefficients for aliasing reduction */
#define MDCT_CA(coef) (int32_t)(coef / sqrt(1.0 + (coef * coef)) * 0x7fffffff)
#define MDCT_CS(coef) (int32_t)(1.0 / sqrt(1.0 + (coef * coef)) * 0x7fffffff)

#define MDCT_CA0 MDCT_CA(-0.6)
#define MDCT_CA1 MDCT_CA(-0.535)
#define MDCT_CA2 MDCT_CA(-0.33)
#define MDCT_CA3 MDCT_CA(-0.185)
#define MDCT_CA4 MDCT_CA(-0.095)
#define MDCT_CA5 MDCT_CA(-0.041)
#define MDCT_CA6 MDCT_CA(-0.0142)
#define MDCT_CA7 MDCT_CA(-0.0037)

#define MDCT_CS0 MDCT_CS(-0.6)
#define MDCT_CS1 MDCT_CS(-0.535)
#define MDCT_CS2 MDCT_CS(-0.33)
#define MDCT_CS3 MDCT_CS(-0.185)
#define MDCT_CS4 MDCT_CS(-0.095)
#define MDCT_CS5 MDCT_CS(-0.041)
#define MDCT_CS6 MDCT_CS(-0.0142)
#define MDCT_CS7 MDCT_CS(-0.0037)

/* ISO 11172-3 long-to-short transition windows, 36 points. */
static double transition_window(int block_type, int n) {
  if (block_type == SHINE_BLOCK_START) {
    if (n < 18)
      return 1.0;
    if (n < 24)
      return sin(PI12 * (n - 18 + 0.5));
    if (n < 30)
      return 0.0;
    return sin(PI12 * (n - 30 + 0.5));
  }
  /* SHINE_BLOCK_STOP */
  if (n < 6)
    return sin(PI12 * (n + 0.5));
  if (n < 12)
    return 0.0;
  if (n < 18)
    return sin(PI12 * (n - 12 + 0.5));
  return 1.0;
}

/*
 * shine_mdct_initialise:
 * -------------------
 */
void shine_mdct_initialise(shine_global_config *config) {
  int m, k, n;

  /* prepare the mdct coefficients */
  for (m = 18; m--;)
    for (k = 36; k--;)
      /* combine window and mdct coefficients into a single table */
      /* scale and convert to fixed point before storing */
      config->mdct.cos_l[m][k] =
          (int32_t)(sin(PI36 * (k + 0.5)) *
                    cos((PI / 72) * (2 * k + 19) * (2 * m + 1)) * 0x7fffffff);

  /* long/short transition windows: same 36-point MDCT phase as the normal
   * long block, with the ISO start/stop window folded into the table so the
   * fixed-point accumulation path is unchanged. */
  for (m = 18; m--;)
    for (k = 36; k--;) {
      config->mdct.cos_start[m][k] =
          (int32_t)(transition_window(SHINE_BLOCK_START, k) *
                    sin(PI36 * (k + 0.5)) *
                    cos((PI / 72) * (2 * k + 19) * (2 * m + 1)) * 0x7fffffff);
      config->mdct.cos_stop[m][k] =
          (int32_t)(transition_window(SHINE_BLOCK_STOP, k) *
                    sin(PI36 * (k + 0.5)) *
                    cos((PI / 72) * (2 * k + 19) * (2 * m + 1)) * 0x7fffffff);
    }

  /* pure short blocks: three 12-point windows, sin(pi*(n+0.5)/12), with the
   * 12-point MDCT phase cos(pi/24*(2n+7)*(2m+1)). */
  for (m = 6; m--;)
    for (n = 12; n--;)
      config->mdct.cos_s[m][n] =
          (int32_t)(sin(PI12 * (n + 0.5)) *
                    cos((PI / 24) * (2 * n + 7) * (2 * m + 1)) * 0x7fffffff);
}

/* Fixed-point 36-point long MDCT shared by normal/start/stop blocks. The
 * coefficient table already contains the window, so the arithmetic is
 * identical to the original long-block path. */
static void long_mdct(const int32_t mdct_in[36], int32_t out[18],
                      const int32_t coeff[18][36]) {
  int k, j;

  for (k = 18; k--;) {
    int32_t vm;
    uint32_t vm_lo __attribute__((unused));

    mul0(vm, vm_lo, mdct_in[35], coeff[k][35]);
    for (j = 35; j; j -= 7) {
      muladd(vm, vm_lo, mdct_in[j - 1], coeff[k][j - 1]);
      muladd(vm, vm_lo, mdct_in[j - 2], coeff[k][j - 2]);
      muladd(vm, vm_lo, mdct_in[j - 3], coeff[k][j - 3]);
      muladd(vm, vm_lo, mdct_in[j - 4], coeff[k][j - 4]);
      muladd(vm, vm_lo, mdct_in[j - 5], coeff[k][j - 5]);
      muladd(vm, vm_lo, mdct_in[j - 6], coeff[k][j - 6]);
      muladd(vm, vm_lo, mdct_in[j - 7], coeff[k][j - 7]);
    }
    mulz(vm, vm_lo);
    out[k] = vm;
  }
}

/* Three offset-6 12-point short windows over the 36 input points, each
 * yielding six frequency coefficients. Results are written in natural
 * (window-major) order: out[6*window + m]. */
static void short_mdct(const int32_t mdct_in[36], int32_t out[18],
                       const mdct_t *mdct) {
  int window, m, n;

  for (window = 0; window < SHINE_SHORT_WINDOWS; window++) {
    const int offset = 6 * (window + 1);
    for (m = 0; m < 6; m++) {
      int32_t vm = 0;
      for (n = 0; n < 12; n++)
        vm += mul(mdct_in[offset + n], mdct->cos_s[m][n]);
      out[6 * window + m] = vm;
    }
  }
}

/* Anti-aliasing reduction butterfly between adjacent subbands, applied to
 * long blocks (normal/start/stop) only. */
static void alias_reduction(int32_t mdct_enc[32][18], int band) {
  cmuls(mdct_enc[band][0], mdct_enc[band - 1][17 - 0], mdct_enc[band][0],
        mdct_enc[band - 1][17 - 0], MDCT_CS0, MDCT_CA0);
  cmuls(mdct_enc[band][1], mdct_enc[band - 1][17 - 1], mdct_enc[band][1],
        mdct_enc[band - 1][17 - 1], MDCT_CS1, MDCT_CA1);
  cmuls(mdct_enc[band][2], mdct_enc[band - 1][17 - 2], mdct_enc[band][2],
        mdct_enc[band - 1][17 - 2], MDCT_CS2, MDCT_CA2);
  cmuls(mdct_enc[band][3], mdct_enc[band - 1][17 - 3], mdct_enc[band][3],
        mdct_enc[band - 1][17 - 3], MDCT_CS3, MDCT_CA3);
  cmuls(mdct_enc[band][4], mdct_enc[band - 1][17 - 4], mdct_enc[band][4],
        mdct_enc[band - 1][17 - 4], MDCT_CS4, MDCT_CA4);
  cmuls(mdct_enc[band][5], mdct_enc[band - 1][17 - 5], mdct_enc[band][5],
        mdct_enc[band - 1][17 - 5], MDCT_CS5, MDCT_CA5);
  cmuls(mdct_enc[band][6], mdct_enc[band - 1][17 - 6], mdct_enc[band][6],
        mdct_enc[band - 1][17 - 6], MDCT_CS6, MDCT_CA6);
  cmuls(mdct_enc[band][7], mdct_enc[band - 1][17 - 7], mdct_enc[band][7],
        mdct_enc[band - 1][17 - 7], MDCT_CS7, MDCT_CA7);
}

/*
 * shine_mdct_sub:
 * ------------
 */
void shine_mdct_sub(shine_global_config *config, int stride) {
  /* note. we wish to access the array 'config->mdct_freq[2][2][576]' as
   * [2][2][32][18]. (32*18=576),
   */
  int32_t(*mdct_enc)[18];

  int ch, gr, band, j, k;
  int32_t mdct_in[36];

#ifdef SHINE_TASK_OBSERVATIONS
  config->obs_valid = 1;
#endif

  for (ch = config->wave.channels; ch--;) {
    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++) {
      int block_type = config->block_mode ? config->frame_blocks[gr][ch]
                                          : SHINE_BLOCK_NORMAL;

      /* set up pointer to the part of config->mdct_freq we're using */
      mdct_enc = (int32_t(*)[18])config->mdct_freq[ch][gr];

      /* polyphase filtering */
      for (k = 0; k < 18; k += 2) {
        shine_window_filter_subband(&config->buffer[ch],
                                    &config->l3_sb_sample[ch][gr + 1][k][0], ch,
                                    config, stride);
        shine_window_filter_subband(&config->buffer[ch],
                                    &config->l3_sb_sample[ch][gr + 1][k + 1][0],
                                    ch, config, stride);
        /* Compensate for inversion in the analysis filter
         * (every odd index of band AND k)
         */
        for (band = 1; band < 32; band += 2)
          config->l3_sb_sample[ch][gr + 1][k + 1][band] *= -1;
      }

#ifdef SHINE_TASK_OBSERVATIONS
      config->obs_block_type[ch][gr] = block_type;
      config->obs_aliasing_applied[ch][gr] =
          (block_type != SHINE_BLOCK_SHORT);
#endif

      /* Perform imdct of 18 previous subband samples + 18 current subband
       * samples */
      for (band = 0; band < 32; band++) {
        for (k = 18; k--;) {
          mdct_in[k] = config->l3_sb_sample[ch][gr][k][band];
          mdct_in[k + 18] = config->l3_sb_sample[ch][gr + 1][k][band];
        }

#ifdef SHINE_TASK_OBSERVATIONS
        memcpy(config->obs_mdct_in[ch][gr][band], mdct_in,
               sizeof(mdct_in));
#endif

        if (block_type == SHINE_BLOCK_SHORT) {
          int32_t short_out[18];
          int window, m;

          /* Three real 12-point short windows; the long spectrum is not
           * split, the coefficients come from their own transform. */
          short_mdct(mdct_in, short_out, &config->mdct);

          /* Natural short layout: 192 positions per window, frequency =
           * 6*subband + coefficient. */
          for (window = 0; window < SHINE_SHORT_WINDOWS; window++)
            for (m = 0; m < 6; m++)
              config->mdct_freq[ch][gr][192 * window + 6 * band + m] =
                  short_out[6 * window + m];

#ifdef SHINE_TASK_OBSERVATIONS
          memcpy(config->obs_mdct_pre_alias[ch][gr][band], short_out,
                 sizeof(short_out));
#endif
          /* Pure short blocks skip cross-subband aliasing reduction. */
          continue;
        }

        /* Calculation of the MDCT
         * In the case of long blocks ( block_type 0,1,3 ) there are
         * 36 coefficients in the time domain and 18 in the frequency
         * domain.
         */
        if (block_type == SHINE_BLOCK_START)
          long_mdct(mdct_in, mdct_enc[band], config->mdct.cos_start);
        else if (block_type == SHINE_BLOCK_STOP)
          long_mdct(mdct_in, mdct_enc[band], config->mdct.cos_stop);
        else {
          for (k = 18; k--;) {
            int32_t vm;
#ifdef __BORLANDC__
            uint32_t vm_lo;
#else
            uint32_t vm_lo __attribute__((unused));
#endif

            mul0(vm, vm_lo, mdct_in[35], config->mdct.cos_l[k][35]);
            for (j = 35; j; j -= 7) {
              muladd(vm, vm_lo, mdct_in[j - 1], config->mdct.cos_l[k][j - 1]);
              muladd(vm, vm_lo, mdct_in[j - 2], config->mdct.cos_l[k][j - 2]);
              muladd(vm, vm_lo, mdct_in[j - 3], config->mdct.cos_l[k][j - 3]);
              muladd(vm, vm_lo, mdct_in[j - 4], config->mdct.cos_l[k][j - 4]);
              muladd(vm, vm_lo, mdct_in[j - 5], config->mdct.cos_l[k][j - 5]);
              muladd(vm, vm_lo, mdct_in[j - 6], config->mdct.cos_l[k][j - 6]);
              muladd(vm, vm_lo, mdct_in[j - 7], config->mdct.cos_l[k][j - 7]);
            }
            mulz(vm, vm_lo);
            mdct_enc[band][k] = vm;
          }
        }

#ifdef SHINE_TASK_OBSERVATIONS
        memcpy(config->obs_mdct_pre_alias[ch][gr][band], mdct_enc[band],
               18 * sizeof(int32_t));
#endif

        /* Perform aliasing reduction butterfly */
        if (band != 0)
          alias_reduction(mdct_enc, band);
      }
    }

    /* Save latest granule's subband samples to be used in the next mdct call */
    memcpy(config->l3_sb_sample[ch][0],
           config->l3_sb_sample[ch][config->mpeg.granules_per_frame],
           sizeof(config->l3_sb_sample[0][0]));
  }
}
