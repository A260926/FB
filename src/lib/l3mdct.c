/* L3mdct */

#include "l3mdct.h"
#include "l3subband.h"
#include "tables.h"
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

#ifdef SHINE_TEST_HOOKS
int32_t shine_test_mdct_in[MAX_CHANNELS][MAX_GRANULES][32][36];
int32_t shine_test_mdct_pre[MAX_CHANNELS][MAX_GRANULES][32][18];
int32_t shine_test_mdct_final[MAX_CHANNELS][MAX_GRANULES][GRANULE_SIZE];
int shine_test_mdct_type[MAX_CHANNELS][MAX_GRANULES];
#endif

/* ISO 11172-3 long-block window shapes. block_type 0 is the normal long
 * window, 1 the start window and 3 the stop window. The short block
 * (type 2) uses three 12-point windows and is handled separately. */
static double mdct_window_long(int block_type, int k) {
  if (block_type == 1) { /* start */
    if (k < 18)
      return sin(PI36 * (k + 0.5));
    if (k < 24)
      return 1.0;
    if (k < 30)
      return sin(PI12 * (k - 18 + 0.5));
    return 0.0;
  }
  if (block_type == 3) { /* stop */
    if (k < 6)
      return 0.0;
    if (k < 12)
      return sin(PI12 * (k - 6 + 0.5));
    if (k < 18)
      return 1.0;
    return sin(PI36 * (k + 0.5));
  }
  return sin(PI36 * (k + 0.5)); /* normal long */
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

  /* start/stop transition windows, same long 36-point MDCT kernel */
  for (m = 18; m--;)
    for (k = 36; k--;) {
      config->mdct.cos_l_start[m][k] =
          (int32_t)(mdct_window_long(1, k) *
                    cos((PI / 72) * (2 * k + 19) * (2 * m + 1)) * 0x7fffffff);
      config->mdct.cos_l_stop[m][k] =
          (int32_t)(mdct_window_long(3, k) *
                    cos((PI / 72) * (2 * k + 19) * (2 * m + 1)) * 0x7fffffff);
    }

  /* 12-point short windows: three sub-windows of 12 samples producing six
   * coefficients each, at offsets 6, 12 and 18 of the granule. Window
   * sin(pi*(n+0.5)/12), 6->6 MDCT phase cos(pi/24*(2n+7)*(2k+1)). */
  for (k = 6; k--;)
    for (n = 12; n--;)
      config->mdct.cos_s[k][n] =
          (int32_t)(sin(PI12 * (n + 0.5)) *
                    cos((PI / 24.0) * (2 * n + 7) * (2 * k + 1)) *
                    0x7fffffff);
}

/* 36-point MDCT using a pre-folded window/kernel table. */
static void mdct_long(shine_global_config *config, const int32_t *in,
                      int32_t out[18], const int32_t (*cos_tab)[36]) {
  int j, k;
  (void)config;

  for (k = 18; k--;) {
    int32_t vm;
#ifdef __BORLANDC__
    uint32_t vm_lo;
#else
    uint32_t vm_lo __attribute__((unused));
#endif

    mul0(vm, vm_lo, in[35], cos_tab[k][35]);
    for (j = 35; j; j -= 7) {
      muladd(vm, vm_lo, in[j - 1], cos_tab[k][j - 1]);
      muladd(vm, vm_lo, in[j - 2], cos_tab[k][j - 2]);
      muladd(vm, vm_lo, in[j - 3], cos_tab[k][j - 3]);
      muladd(vm, vm_lo, in[j - 4], cos_tab[k][j - 4]);
      muladd(vm, vm_lo, in[j - 5], cos_tab[k][j - 5]);
      muladd(vm, vm_lo, in[j - 6], cos_tab[k][j - 6]);
      muladd(vm, vm_lo, in[j - 7], cos_tab[k][j - 7]);
    }
    mulz(vm, vm_lo);
    out[k] = vm;
  }
}

/* Three 12-point short MDCTs positioned at offsets 6, 12 and 18 of the
 * 36-sample granule. Each produces six coefficients. */
static void mdct_short(shine_global_config *config, const int32_t *in,
                       int32_t out[18]) {
  int w, k, n;

  for (w = 0; w < 3; w++) {
    const int32_t *frame = in + 6 + 6 * w;
    for (k = 0; k < 6; k++) {
      int32_t vm;
#ifdef __BORLANDC__
      uint32_t vm_lo;
#else
      uint32_t vm_lo __attribute__((unused));
#endif
      mul0(vm, vm_lo, frame[11], config->mdct.cos_s[k][11]);
      for (n = 11; n; n--)
        muladd(vm, vm_lo, frame[n - 1], config->mdct.cos_s[k][n - 1]);
      mulz(vm, vm_lo);
      out[w * 6 + k] = vm;
    }
  }
}

void shine_mdct_set_block_type(shine_global_config *config, int gr, int ch,
                               int block_type) {
  config->window.block_type[ch][gr] = block_type;
}

void shine_mdct_clear_block_types(shine_global_config *config) {  int ch, gr;
  for (ch = 0; ch < MAX_CHANNELS; ch++) {
    config->window.previous_block_type[ch] = 0;
    for (gr = 0; gr < MAX_GRANULES; gr++)
      config->window.block_type[ch][gr] = 0;
  }
  config->window.any_short = 0;
}

/*
 * shine_mdct_sub:
 * ------------
 */
void shine_mdct_sub(shine_global_config *config, int stride) {
  /* note. we wish to access the array 'config->mdct_freq[2][2][576]' as
   * [2][2][32][18]. (32*18=576),
   */
  int32_t raw[32][18];
  int32_t mdct_in[36];

  int ch, gr, band, k;

  for (ch = config->wave.channels; ch--;) {
    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++) {
      int block_type = config->window.block_type[ch][gr];
      int32_t *flat = &config->mdct_freq[ch][gr][0];

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

      /* Perform imdct of 18 previous subband samples + 18 current subband
       * samples. The result is gathered in `raw` so that short blocks can be
       * reordered as a whole; long blocks simply copy it afterwards. */
      for (band = 0; band < 32; band++) {
        for (k = 18; k--;) {
          mdct_in[k] = config->l3_sb_sample[ch][gr][k][band];
          mdct_in[k + 18] = config->l3_sb_sample[ch][gr + 1][k][band];
        }

#ifdef SHINE_TEST_HOOKS
        for (k = 0; k < 36; k++)
          shine_test_mdct_in[ch][gr][band][k] = mdct_in[k];
#endif

        if (block_type == 2) {
          mdct_short(config, mdct_in, raw[band]);
        } else {
          const int32_t(*tab)[36] = config->mdct.cos_l;
          if (block_type == 1)
            tab = config->mdct.cos_l_start;
          else if (block_type == 3)
            tab = config->mdct.cos_l_stop;
          mdct_long(config, mdct_in, raw[band], tab);
        }

#ifdef SHINE_TEST_HOOKS
        for (k = 0; k < 18; k++)
          shine_test_mdct_pre[ch][gr][band][k] = raw[band][k];
#endif
      }

#ifdef SHINE_TEST_HOOKS
      shine_test_mdct_type[ch][gr] = block_type;
#endif

      /* Perform aliasing reduction butterfly. Short blocks never alias. */
      if (block_type != 2) {
        for (band = 1; band < 32; band++) {
          cmuls(raw[band][0], raw[band - 1][17 - 0], raw[band][0],
                raw[band - 1][17 - 0], MDCT_CS0, MDCT_CA0);
          cmuls(raw[band][1], raw[band - 1][17 - 1], raw[band][1],
                raw[band - 1][17 - 1], MDCT_CS1, MDCT_CA1);
          cmuls(raw[band][2], raw[band - 1][17 - 2], raw[band][2],
                raw[band - 1][17 - 2], MDCT_CS2, MDCT_CA2);
          cmuls(raw[band][3], raw[band - 1][17 - 3], raw[band][3],
                raw[band - 1][17 - 3], MDCT_CS3, MDCT_CA3);
          cmuls(raw[band][4], raw[band - 1][17 - 4], raw[band][4],
                raw[band - 1][17 - 4], MDCT_CS4, MDCT_CA4);
          cmuls(raw[band][5], raw[band - 1][17 - 5], raw[band][5],
                raw[band - 1][17 - 5], MDCT_CS5, MDCT_CA5);
          cmuls(raw[band][6], raw[band - 1][17 - 6], raw[band][6],
                raw[band - 1][17 - 6], MDCT_CS6, MDCT_CA6);
          cmuls(raw[band][7], raw[band - 1][17 - 7], raw[band][7],
                raw[band - 1][17 - 7], MDCT_CS7, MDCT_CA7);
        }
      }

      if (block_type == 2) {
        /* Reorder short-block coefficients into band / window / frequency
         * order. Frequency position g = subband*6 + k; short scalefactor
         * band b occupies [3*band[b], 3*band[b+1]) in this layout. */
        const int *sb_band =
            &shine_scale_fact_band_index_short[config->mpeg.samplerate_index][0];
        int pos = 0;
        int b, w, f, g, s, kf;
        for (b = 0; b < 13; b++) {
          int width = sb_band[b + 1] - sb_band[b];
          for (w = 0; w < 3; w++)
            for (f = 0; f < width; f++) {
              g = sb_band[b] + f;
              s = g / 6;
              kf = g % 6;
              flat[pos++] = raw[s][w * 6 + kf];
            }
        }
      } else {
        for (band = 0; band < 32; band++)
          for (k = 0; k < 18; k++)
            flat[band * 18 + k] = raw[band][k];
      }

#ifdef SHINE_TEST_HOOKS
      memcpy(shine_test_mdct_final[ch][gr], flat,
             GRANULE_SIZE * sizeof(int32_t));
#endif
    }

    /* Save latest granule's subband samples to be used in the next mdct call */
    memcpy(config->l3_sb_sample[ch][0],
           config->l3_sb_sample[ch][config->mpeg.granules_per_frame],
           sizeof(config->l3_sb_sample[0][0]));
  }
}
