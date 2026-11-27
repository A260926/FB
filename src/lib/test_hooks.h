#ifndef SHINE_TEST_HOOKS_H
#define SHINE_TEST_HOOKS_H

/*
 * Read-only observation points for the test-suite.
 *
 * This header is only meaningful when SHINE_TEST_HOOKS is defined (the test
 * build defines it); the storage itself lives in the production translation
 * units behind the same guard, so nothing is exported from a normal build.
 * Tests must not write to any of these arrays.
 */

#include <stdint.h>

#define SHINE_OBS_MAX_GRANULES 4
#define SHINE_OBS_MAX_CHANNELS 2
#define SHINE_OBS_GRANULE_SIZE 576

/* --- MDCT / window switching (l3mdct.c) --- */

/* Time-domain input fed to each band's transform: 18 previous + 18 current
 * subband samples (36 lines), per channel/granule/band. */
extern int32_t shine_obs_mdct_in[SHINE_OBS_MAX_CHANNELS]
                                [SHINE_OBS_MAX_GRANULES][32][36];
extern int shine_obs_mdct_in_valid[SHINE_OBS_MAX_CHANNELS]
                                   [SHINE_OBS_MAX_GRANULES];

/* Spectral coefficients before aliasing reduction, natural coefficient order:
 * long[band*18 + line]; short[band*18 + window*6 + line]. */
extern int32_t shine_obs_xr_pre_aa[SHINE_OBS_MAX_CHANNELS]
                                   [SHINE_OBS_MAX_GRANULES]
                                   [SHINE_OBS_GRANULE_SIZE];

/* Same layout, after aliasing reduction (== pre-aa for short blocks, which
 * must never be alias-reduced). */
extern int32_t shine_obs_xr_post_aa[SHINE_OBS_MAX_CHANNELS]
                                    [SHINE_OBS_MAX_GRANULES]
                                    [SHINE_OBS_GRANULE_SIZE];

/* Block type (0..3) and mixed flag actually used. */
extern uint8_t shine_obs_block_type[SHINE_OBS_MAX_CHANNELS]
                                   [SHINE_OBS_MAX_GRANULES];
extern uint8_t shine_obs_mixed[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES];

/* --- Quantization (l3loop.c) --- */

/* Quantized integers exactly as written to the Huffman stream, in the
 * encoder's working (bitstream) order.  For short blocks this is the
 * band/window/frequency order. */
extern int shine_obs_l3_enc[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES]
                           [SHINE_OBS_GRANULE_SIZE];
/* The same quantized magnitudes after inverse-reordering into natural
 * (per-subband, window-major) coordinates, so the bitstream test can compare
 * an independently inverse-reordered stream against it.  Long blocks are
 * identical to shine_obs_l3_enc. */
extern int shine_obs_l3_enc_nat[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES]
                               [SHINE_OBS_GRANULE_SIZE];
extern int8_t shine_obs_sign[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES]
                            [SHINE_OBS_GRANULE_SIZE];
/* Chopped side-info values needed to mirror the encoder's decisions. */
struct shine_obs_side {
  int part2_3_length, part2_length, big_values, count1, global_gain;
  int scalefac_compress, window_switching, block_type, mixed_block_flag;
  int table_select[3], subblock_gain[3];
  int region0_count, region1_count;
  int scalefac_scale, count1table_select, preflag;
};
extern struct shine_obs_side
    shine_obs_side[SHINE_OBS_MAX_CHANNELS][SHINE_OBS_MAX_GRANULES];

#endif
