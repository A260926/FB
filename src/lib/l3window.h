#ifndef L3WINDOW_H
#define L3WINDOW_H

#include "types.h"

/* True when the encoder configuration is one the block switching entries
 * accept: MPEG-1, mono/normal stereo at one of the three MPEG-1 rates. */
int shine_blocks_config_supported(const shine_global_config *config);

/* Validate a whole frame's block vector against the configuration and the
 * per-channel transition state. Returns 1 when legal, 0 otherwise. Must run
 * before any PCM analysis so a rejection leaves all encoder state intact. */
int shine_blocks_validate(const shine_global_config *config,
                          const uint8_t *block_types, int block_type_count);

/* Remember the accepted plan for the upcoming frame and clear per-frame side
 * information that only the block switching path maintains. */
void shine_blocks_begin_frame(shine_global_config *config,
                              const uint8_t *block_types);

/* Advance the per-channel transition state once a frame completed. */
void shine_blocks_commit_frame(shine_global_config *config);

/* Fourteen short scalefactor band endpoints (13 bands, last endpoint 192)
 * indexed by MPEG-1 samplerate index 0 (44.1k), 1 (48k) or 2 (32k). */
const int *shine_short_band_offsets(int samplerate_index);

/* Permute a short-block spectrum between natural (band, window, frequency)
 * coordinates and the MPEG bitstream (sfb, window, frequency) ordering. */
void shine_short_reorder(const int natural[GRANULE_SIZE],
                         int stream[GRANULE_SIZE], int samplerate_index);
void shine_short_unreorder(const int stream[GRANULE_SIZE],
                           int natural[GRANULE_SIZE], int samplerate_index);

#endif
