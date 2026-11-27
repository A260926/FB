/* L3mdct */

#include "l3mdct.h"
#include "l3subband.h"
#include "tables.h"
#include "types.h"

#ifdef SHINE_TEST_HOOKS
#include "test_hooks.h"

int32_t shine_obs_mdct_in[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES][32]
                         [36];
int shine_obs_mdct_in_valid[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES];
int32_t shine_obs_xr_pre_aa[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES]
                           [SHINE_OBS_GRANULE_SIZE];
int32_t shine_obs_xr_post_aa[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES]
                            [SHINE_OBS_GRANULE_SIZE];
uint8_t shine_obs_block_type[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES];
uint8_t shine_obs_mixed[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES];
#endif

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

/*
 * shine_mdct_initialise:
 * -------------------
 */
void shine_mdct_initialise(shine_global_config *config) {
  int m, k;

  /* prepare the mdct coefficients */
  for (m = 18; m--;)
    for (k = 36; k--;) {
      double base = cos((PI / 72) * (2 * k + 19) * (2 * m + 1));
      double wl = sin(PI36 * (k + 0.5)); /* normal long window */
      double wstart, wstop;

      /* ISO long/start/stop windows (block length 36, short length 12):
       *   start: long half [0,18), flat [18,24), short half [24,30), 0 [30,36)
       *   stop:  0 [0,6), short half [6,12), flat [12,18), long half [18,36)
       */
      if (k < 18)
        wstart = wl;
      else if (k < 24)
        wstart = 1.0;
      else if (k < 30)
        wstart = sin(PI12 * (k - 24 + 0.5));
      else
        wstart = 0.0;

      if (k < 6)
        wstop = 0.0;
      else if (k < 12)
        wstop = sin(PI12 * (k - 6 + 0.5));
      else if (k < 18)
        wstop = 1.0;
      else
        wstop = wl;

      /* combine window and mdct coefficients into a single table */
      /* scale and convert to fixed point before storing */
      config->mdct.cos_l[m][k] = (int32_t)(wl * base * 0x7fffffff);
      config->mdct.cos_start[m][k] = (int32_t)(wstart * base * 0x7fffffff);
      config->mdct.cos_stop[m][k] = (int32_t)(wstop * base * 0x7fffffff);
    }
}

/*
 * Long-block MDCT: 36 time samples -> 18 frequency lines.  Used for normal,
 * start and stop blocks.
 */
static void mdct_long_line(const int32_t *mdct_in, const int32_t *cos_l,
                           int32_t *out, int nout) {
  int k, j;

  for (k = nout; k--;) {
    int32_t vm;
#ifdef __BORLANDC__
    uint32_t vm_lo;
#else
    uint32_t vm_lo __attribute__((unused));
#endif

    mul0(vm, vm_lo, mdct_in[35], cos_l[k * 36 + 35]);
    for (j = 35; j; j -= 7) {
      muladd(vm, vm_lo, mdct_in[j - 1], cos_l[k * 36 + j - 1]);
      muladd(vm, vm_lo, mdct_in[j - 2], cos_l[k * 36 + j - 2]);
      muladd(vm, vm_lo, mdct_in[j - 3], cos_l[k * 36 + j - 3]);
      muladd(vm, vm_lo, mdct_in[j - 4], cos_l[k * 36 + j - 4]);
      muladd(vm, vm_lo, mdct_in[j - 5], cos_l[k * 36 + j - 5]);
      muladd(vm, vm_lo, mdct_in[j - 6], cos_l[k * 36 + j - 6]);
      muladd(vm, vm_lo, mdct_in[j - 7], cos_l[k * 36 + j - 7]);
    }
    mulz(vm, vm_lo);
    out[k] = vm;
  }
}

/*
 * Short-block MDCT: three 12-point windows at offsets 6, 12 and 18 within the
 * 36-sample granule window.  Each produces 6 lines (18 per band).  The
 * window is sin(pi*(n+0.5)/12) and the MDCT kernel matches the long one with
 * N=12 (12 time samples -> 6 lines): with Shine's pi/(2N)*(2n+1+N/2)*(2k+1)
 * convention this is cos(pi/24 * (2n+7) * (2k+1)).
 */
static void mdct_short_band(const int32_t *time36, int32_t *out6x3) {
  static int32_t short_cos[6][12];
  static int initialised = 0;
  int w, n, k;

  if (!initialised) {
    for (k = 0; k < 6; k++)
      for (n = 0; n < 12; n++)
        short_cos[k][n] = (int32_t)(sin(PI12 * (n + 0.5)) *
                                    cos((PI / 24.0) * (2 * n + 7) * (2 * k + 1)) *
                                    0x7fffffff);
    initialised = 1;
  }

  /* For each of the three short windows, take the 12 lines starting at
   * offset 6, 12 and 18 (the windows overlap by 6 lines; together with the
   * continuous 18-line history this gives the standard 50% overlap-add). */
  for (w = 0; w < 3; w++) {
    const int32_t *in = time36 + 6 * (w + 1);
    for (k = 0; k < 6; k++) {
      int32_t vm;
#ifdef __BORLANDC__
      uint32_t vm_lo;
#else
      uint32_t vm_lo __attribute__((unused));
#endif
      mul0(vm, vm_lo, in[11], short_cos[k][11]);
      for (n = 11; n--;)
        muladd(vm, vm_lo, in[n], short_cos[k][n]);
      mulz(vm, vm_lo);
      /* Store window-major: out[w*6 + k]. */
      out6x3[w * 6 + k] = vm;
    }
  }
}

/* Aliasing reduction between adjacent long bands (8 butterflies). */
static void alias_reduce(int32_t *hi, int32_t *lo) {
  cmuls(hi[0], lo[17 - 0], hi[0], lo[17 - 0], MDCT_CS0, MDCT_CA0);
  cmuls(hi[1], lo[17 - 1], hi[1], lo[17 - 1], MDCT_CS1, MDCT_CA1);
  cmuls(hi[2], lo[17 - 2], hi[2], lo[17 - 2], MDCT_CS2, MDCT_CA2);
  cmuls(hi[3], lo[17 - 3], hi[3], lo[17 - 3], MDCT_CS3, MDCT_CA3);
  cmuls(hi[4], lo[17 - 4], hi[4], lo[17 - 4], MDCT_CS4, MDCT_CA4);
  cmuls(hi[5], lo[17 - 5], hi[5], lo[17 - 5], MDCT_CS5, MDCT_CA5);
  cmuls(hi[6], lo[17 - 6], hi[6], lo[17 - 6], MDCT_CS6, MDCT_CA6);
  cmuls(hi[7], lo[17 - 7], hi[7], lo[17 - 7], MDCT_CS7, MDCT_CA7);
}

/*
 * Reorder a short block's natural per-subband spectrum into the bitstream
 * coordinate system: scalefactor band, window, then frequency.
 *
 * xr_nat[band][w*6 + line] holds the six lines of window w (w = 0..2) for
 * that subband.  There are 32 subbands x 6 lines = 192 short-spectrum lines,
 * so global short line L maps to subband L/6 and window-local line L%6.  A
 * short scalefactor band covers lines [sbands[sfb], sbands[sfb+1]).  Each
 * band is emitted with its three windows contiguous, each window's lines in
 * increasing frequency.
 */
static void mdct_short_reorder(const int32_t xr_nat[SBLIMIT][18],
                               const int *sbands, int32_t *out) {
  int sfb, w, f;
  int outpos = 0;

  /* 13 short scalefactor bands cover all 192 short-spectrum lines. */
  for (sfb = 0; sfb < 13; sfb++) {
    int f0 = sbands[sfb];
    int f1 = sbands[sfb + 1];
    for (w = 0; w < 3; w++) {
      for (f = f0; f < f1; f++) {
        int band = f / 6;
        int line = f % 6;
        out[outpos++] = xr_nat[band][w * 6 + line];
      }
    }
  }
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

  int ch, gr, band, k;
  int32_t mdct_in[36];

  for (ch = config->wave.channels; ch--;) {
    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++) {
      int block_type = 0;

      if (config->window_switching_active && ch < MAX_CHANNELS &&
          gr < MAX_GRANULES)
        block_type = config->block_type[ch][gr];

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

      /* Perform the transform on 18 previous + 18 current subband samples. */
      {
        /* Natural per-subband transform output, before any reordering:
         * [band][18].  Long lines are already in natural order; short bands
         * hold window-major groups of 6 lines (w*6 + line). */
        int32_t xr_nat[SBLIMIT][18];
        int32_t short_scatter[GRANULE_SIZE];
        int is_short = (block_type == 2);
        int sr_idx = config->mpeg.samplerate_index;
        const int *sbands =
            (sr_idx >= 0 && sr_idx < 3) ? shine_short_band_index[sr_idx] : 0;

        memset(short_scatter, 0, sizeof(short_scatter));

        /* Pass 1: transform every band. */
        for (band = 0; band < 32; band++) {
          for (k = 18; k--;) {
            mdct_in[k] = config->l3_sb_sample[ch][gr][k][band];
            mdct_in[k + 18] = config->l3_sb_sample[ch][gr + 1][k][band];
          }

#ifdef SHINE_TEST_HOOKS
          if (ch < SHINE_OBS_MAX_CHANNELS && gr < SHINE_OBS_MAX_GRANULES) {
            memcpy(shine_obs_mdct_in[ch][gr][band], mdct_in, sizeof(mdct_in));
            shine_obs_mdct_in_valid[ch][gr] = 1;
          }
#endif

          if (block_type == 2) {
            /* Pure short block: three 12-point transforms per band. */
            mdct_short_band(mdct_in, xr_nat[band]);
          } else {
            /* Long block (normal/start/stop): pick the matching window. */
            const int32_t *win = &config->mdct.cos_l[0][0];
            if (block_type == 1)
              win = &config->mdct.cos_start[0][0];
            else if (block_type == 3)
              win = &config->mdct.cos_stop[0][0];

            mdct_long_line(mdct_in, win, xr_nat[band], 18);
          }
        }

#ifdef SHINE_TEST_HOOKS
        if (ch < SHINE_OBS_MAX_CHANNELS && gr < SHINE_OBS_MAX_GRANULES) {
          for (band = 0; band < 32; band++)
            memcpy(&shine_obs_xr_pre_aa[ch][gr][band * 18], xr_nat[band],
                   18 * sizeof(int32_t));
        }
#endif

        /* Pass 2: aliasing reduction, long blocks only.  Short blocks must
         * not be alias-reduced across subbands; normal/start/stop are. */
        if (block_type != 2) {
          for (band = 1; band < 32; band++)
            alias_reduce(xr_nat[band], xr_nat[band - 1]);
        }

#ifdef SHINE_TEST_HOOKS
        if (ch < SHINE_OBS_MAX_CHANNELS && gr < SHINE_OBS_MAX_GRANULES) {
          for (band = 0; band < 32; band++)
            memcpy(&shine_obs_xr_post_aa[ch][gr][band * 18], xr_nat[band],
                   18 * sizeof(int32_t));
        }
#endif

        if (is_short && sbands) {
          /* Reorder into the bitstream's short-block coordinate system:
           * scalefactor band, then window, then frequency. */
          mdct_short_reorder((const int32_t(*)[18])xr_nat, sbands,
                             short_scatter);
          for (k = 0; k < GRANULE_SIZE; k++)
            ((int32_t *)mdct_enc)[k] = short_scatter[k];
        } else {
          for (band = 0; band < 32; band++)
            for (k = 0; k < 18; k++)
              mdct_enc[band][k] = xr_nat[band][k];
        }

#ifdef SHINE_TEST_HOOKS
        if (ch < SHINE_OBS_MAX_CHANNELS && gr < SHINE_OBS_MAX_GRANULES) {
          shine_obs_block_type[ch][gr] = (uint8_t)block_type;
          shine_obs_mixed[ch][gr] = 0;
        }
#endif
      }
    }

    /* Save latest granule's subband samples to be used in the next mdct call */
    memcpy(config->l3_sb_sample[ch][0],
           config->l3_sb_sample[ch][config->mpeg.granules_per_frame],
           sizeof(config->l3_sb_sample[0][0]));
  }
}
