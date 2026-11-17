#ifndef MASKED_H
#define MASKED_H

#include "types.h"

/* Number of long scalefactor bands in an MPEG-1 Layer III granule. */
#define MASKED_SFB 22

/* Threshold layout per frame: granule, channel, 22 long bands. */
#define MASKED_RATIO_COUNT(ch) (2 * (ch)*MASKED_SFB)

/* Validate a masked request against the encoder configuration.
 * Returns 0 when the request may proceed, -1 otherwise. Must run before
 * any analysis history is advanced. */
int shine_masked_validate(shine_global_config *config, const double *ratios,
                          int ratio_count);

/* Iteration loop driven by caller supplied noise thresholds. Only valid
 * for MPEG-1, 32/44.1/48 kHz, mono or plain stereo, long blocks. */
void shine_masked_iteration_loop(shine_global_config *config,
                                 const double *ratios);

/* Test only observability hooks, compiled in solely for the test build. */
#ifdef SHINE_TEST_HOOKS
typedef struct {
  int masked;                        /* last frame used the masked path */
  int32_t xr[2][2][GRANULE_SIZE];   /* mdct spectrum of last frame */
  int base_ix[2][2][GRANULE_SIZE];  /* zero scalefactor candidate */
  int chosen_ix[2][2][GRANULE_SIZE]; /* committed integer spectrum */
  int base_global_gain[2][2];
  unsigned base_part2_3[2][2];
  int chosen_sf[2][2][MASKED_SFB]; /* committed long scalefactors */
  unsigned chosen_count1[2][2];    /* count1 run length (not in side info) */
  double base_worst[2][2];   /* worst normalized noise of the legacy candidate */
  double chosen_worst[2][2]; /* worst normalized noise of the emitted frame */
} shine_test_hook_t;

const shine_test_hook_t *shine_test_hooks(void);

/* Re-snapshot the committed spectra after the bitstream formatter has
 * applied their signs. */
void shine_test_hook_post_format(shine_global_config *config);
#endif

#endif
