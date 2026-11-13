/* l3window.c
 *
 * Explicit MPEG-1 Layer III window switching: block type state machine,
 * short-window scalefactor band tables and the natural <-> bitstream
 * spectrum permutations. */

#include "l3window.h"
#include "layer3.h"
#include "tables.h"

/* MPEG-1 short window scalefactor band endpoints (Table B.8), one entry per
 * MPEG-1 samplerate index: 44.1 kHz, 48 kHz, 32 kHz. The final endpoint is
 * 192, the short-block frequency range (3 windows * 6 coefficients). */
static const int short_band_table[3][SHINE_SHORT_BANDS + 1] = {
    /* 44100 */
    {0, 4, 8, 12, 16, 22, 30, 40, 52, 66, 84, 106, 136, 192},
    /* 48000 */
    {0, 4, 8, 12, 16, 22, 28, 38, 50, 64, 80, 100, 126, 192},
    /* 32000 */
    {0, 4, 8, 12, 16, 22, 30, 42, 58, 78, 104, 138, 180, 192},
};

/* Legal successors for a given block type. Every channel starts its
 * sequence in NORMAL (block_type 0). */
static const int legal_next[4][4] = {
    /* normal -> normal, start */
    {1, 1, 0, 0},
    /* start -> short */
    {0, 0, 1, 0},
    /* short -> short, stop */
    {0, 0, 1, 1},
    /* stop -> normal, start */
    {1, 1, 0, 0},
};

int shine_blocks_config_supported(const shine_global_config *config) {
  if (config->mpeg.version != MPEG_I)
    return 0;
  if (config->mpeg.granules_per_frame != 2)
    return 0;
  if (config->wave.channels == 1)
    return config->mpeg.mode == MONO;
  if (config->wave.channels == 2)
    return config->mpeg.mode == STEREO;
  return 0;
}

int shine_blocks_validate(const shine_global_config *config,
                          const uint8_t *block_types, int block_type_count) {
  int gr, ch;

  if (block_types == NULL)
    return 0;

  if (!shine_blocks_config_supported(config))
    return 0;

  if (block_type_count !=
      config->mpeg.granules_per_frame * config->wave.channels)
    return 0;

  /* The whole frame is checked before any PCM analysis, including every
   * transition against the state carried over from previous frames. */
  for (ch = 0; ch < config->wave.channels; ch++) {
    int prev = config->block_state[ch];
    for (gr = 0; gr < config->mpeg.granules_per_frame; gr++) {
      int cur = block_types[gr * config->wave.channels + ch];
      if (cur > 3 || !legal_next[prev][cur])
        return 0;
      prev = cur;
    }
  }

  return 1;
}

void shine_blocks_begin_frame(shine_global_config *config,
                              const uint8_t *block_types) {
  int gr, ch;

  for (gr = 0; gr < config->mpeg.granules_per_frame; gr++)
    for (ch = 0; ch < config->wave.channels; ch++)
      config->frame_blocks[gr][ch] =
          block_types[gr * config->wave.channels + ch];

  /* Window switching never uses scalefactor select information. */
  memset(config->side_info.scfsi, 0, sizeof(config->side_info.scfsi));
}

void shine_blocks_commit_frame(shine_global_config *config) {
  int ch;

  for (ch = 0; ch < config->wave.channels; ch++)
    config->block_state[ch] =
        config->frame_blocks[config->mpeg.granules_per_frame - 1][ch];
}

const int *shine_short_band_offsets(int samplerate_index) {
  return short_band_table[samplerate_index % 3];
}

/*
 * A short block produces 192 frequency positions per short window. In the
 * natural MDCT coordinates the three windows are stored window-major:
 *   natural index = 192 * window + frequency
 * with frequency = 6 * subband + coefficient. The MPEG bitstream order
 * groups by short scalefactor band, then window, then frequency:
 *   stream index  = 3 * frequency + window
 * i.e. positions (3p, 3p+1, 3p+2) hold windows 0, 1, 2 of frequency p, and
 * the short scalefactor bands simply span consecutive stream positions via
 * the samplerate-specific band offsets. Both permutations are exact inverses
 * and cover all 576 positions. */
void shine_short_reorder(const int natural[GRANULE_SIZE],
                         int stream[GRANULE_SIZE], int samplerate_index) {
  int window, freq;

  (void)samplerate_index;
  for (freq = 0; freq < 192; freq++)
    for (window = 0; window < SHINE_SHORT_WINDOWS; window++)
      stream[3 * freq + window] = natural[192 * window + freq];
}

void shine_short_unreorder(const int stream[GRANULE_SIZE],
                           int natural[GRANULE_SIZE], int samplerate_index) {
  int window, freq;

  (void)samplerate_index;
  for (freq = 0; freq < 192; freq++)
    for (window = 0; window < SHINE_SHORT_WINDOWS; window++)
      natural[192 * window + freq] = stream[3 * freq + window];
}
