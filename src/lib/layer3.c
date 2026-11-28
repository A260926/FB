/* layer3.c */

#include "layer3.h"
#include "bitstream.h"
#include "l3bitstream.h"
#include "l3loop.h"
#include "l3mdct.h"
#include "l3subband.h"
#include "tables.h"
#include "types.h"

static int granules_per_frame[4] = {
    1,  /* MPEG 2.5 */
    -1, /* Reserved */
    1,  /* MPEG II */
    2   /* MPEG I */
};

/* Set default values for important vars */
void shine_set_config_mpeg_defaults(shine_mpeg_t *mpeg) {
  mpeg->bitr = 128;
  mpeg->emph = NONE;
  mpeg->copyright = 0;
  mpeg->original = 1;
}

int shine_mpeg_version(int samplerate_index) {
  /* Pick mpeg version according to samplerate index. */
  if (samplerate_index < 3)
    /* First 3 samplerates are for MPEG-I */
    return MPEG_I;
  else if (samplerate_index < 6)
    /* Then it's MPEG-II */
    return MPEG_II;
  else
    /* Finally, MPEG-2.5 */
    return MPEG_25;
}

int shine_find_samplerate_index(int freq) {
  int i;

  for (i = 0; i < 9; i++)
    if (freq == samplerates[i])
      return i;

  return -1; /* error - not a valid samplerate for encoder */
}

int shine_find_bitrate_index(int bitr, int mpeg_version) {
  int i;

  for (i = 0; i < 16; i++)
    if (bitr == bitrates[i][mpeg_version])
      return i;

  return -1; /* error - not a valid samplerate for encoder */
}

int shine_check_config(int freq, int bitr) {
  int samplerate_index, bitrate_index, mpeg_version;

  samplerate_index = shine_find_samplerate_index(freq);
  if (samplerate_index < 0)
    return -1;

  mpeg_version = shine_mpeg_version(samplerate_index);

  bitrate_index = shine_find_bitrate_index(bitr, mpeg_version);
  if (bitrate_index < 0)
    return -1;

  return mpeg_version;
}

int shine_samples_per_pass(shine_t s) {
  return s->mpeg.granules_per_frame * GRANULE_SIZE;
}

/* Compute default encoding values. */
shine_global_config *shine_initialise(shine_config_t *pub_config) {
  double avg_slots_per_frame;
  shine_global_config *config;

  if (shine_check_config(pub_config->wave.samplerate, pub_config->mpeg.bitr) <
      0)
    return NULL;

  config = calloc(1, sizeof(shine_global_config));
  if (config == NULL)
    return config;

  shine_subband_initialise(config);
  shine_mdct_initialise(config);
  shine_loop_initialise(config);
  shine_mdct_clear_block_types(config);

  /* Copy public config. */
  config->wave.channels = pub_config->wave.channels;
  config->wave.samplerate = pub_config->wave.samplerate;
  config->mpeg.mode = pub_config->mpeg.mode;
  config->mpeg.bitr = pub_config->mpeg.bitr;
  config->mpeg.emph = pub_config->mpeg.emph;
  config->mpeg.copyright = pub_config->mpeg.copyright;
  config->mpeg.original = pub_config->mpeg.original;

  /* Set default values. */
  config->ResvMax = 0;
  config->ResvSize = 0;
  config->mpeg.layer = LAYER_III;
  config->mpeg.crc = 0;
  config->mpeg.ext = 0;
  config->mpeg.mode_ext = 0;
  config->mpeg.bits_per_slot = 8;

  config->mpeg.samplerate_index =
      shine_find_samplerate_index(config->wave.samplerate);
  config->mpeg.version = shine_mpeg_version(config->mpeg.samplerate_index);
  config->mpeg.bitrate_index =
      shine_find_bitrate_index(config->mpeg.bitr, config->mpeg.version);
  config->mpeg.granules_per_frame = granules_per_frame[config->mpeg.version];

  /* Figure average number of 'slots' per frame. */
  avg_slots_per_frame =
      ((double)config->mpeg.granules_per_frame * GRANULE_SIZE /
       ((double)config->wave.samplerate)) *
      (1000 * (double)config->mpeg.bitr / (double)config->mpeg.bits_per_slot);

  config->mpeg.whole_slots_per_frame = (int)avg_slots_per_frame;

  config->mpeg.frac_slots_per_frame =
      avg_slots_per_frame - (double)config->mpeg.whole_slots_per_frame;
  config->mpeg.slot_lag = -config->mpeg.frac_slots_per_frame;

  if (config->mpeg.frac_slots_per_frame == 0)
    config->mpeg.padding = 0;

  shine_open_bit_stream(&config->bs, BUFFER_SIZE);

  memset((char *)&config->side_info, 0, sizeof(shine_side_info_t));

  /* determine the mean bitrate for main data */
  if (config->mpeg.granules_per_frame == 2) /* MPEG 1 */
    config->sideinfo_len = 8 * ((config->wave.channels == 1) ? 4 + 17 : 4 + 32);
  else /* MPEG 2 */
    config->sideinfo_len = 8 * ((config->wave.channels == 1) ? 4 + 9 : 4 + 17);

  return config;
}

static unsigned char *shine_encode_buffer_internal(shine_global_config *config,
                                                   int *written, int stride) {
  if (config->mpeg.frac_slots_per_frame) {
    config->mpeg.padding =
        (config->mpeg.slot_lag <= (config->mpeg.frac_slots_per_frame - 1.0));
    config->mpeg.slot_lag +=
        (config->mpeg.padding - config->mpeg.frac_slots_per_frame);
  }

  config->mpeg.bits_per_frame =
      8 * (config->mpeg.whole_slots_per_frame + config->mpeg.padding);
  config->mean_bits = (config->mpeg.bits_per_frame - config->sideinfo_len) /
                      config->mpeg.granules_per_frame;

  /* apply mdct to the polyphase output */
  shine_mdct_sub(config, stride);

  /* bit and noise allocation */
  shine_iteration_loop(config);

  /* write the frame to the bitstream */
  shine_format_bitstream(config);

  /* Return data. */
  *written = config->bs.data_position;
  config->bs.data_position = 0;

  return config->bs.data;
}

unsigned char *shine_encode_buffer(shine_global_config *config, int16_t **data,
                                   int *written) {
  shine_mdct_clear_block_types(config);

  config->buffer[0] = data[0];
  if (config->wave.channels == 2)
    config->buffer[1] = data[1];

  return shine_encode_buffer_internal(config, written, 1);
}

unsigned char *shine_encode_buffer_interleaved(shine_global_config *config,
                                               int16_t *data, int *written) {
  shine_mdct_clear_block_types(config);

  config->buffer[0] = data;
  if (config->wave.channels == 2)
    config->buffer[1] = data + 1;

  return shine_encode_buffer_internal(config, written, config->wave.channels);
}

/* Validate one channel's requested transition. Returns 0 when legal. */
static int window_transition_ok(int from, int to) {
  switch (from) {
  case 0: /* normal */
    return to == 0 || to == 1;
  case 1: /* start */
    return to == 2;
  case 2: /* short */
    return to == 2 || to == 3;
  case 3: /* stop */
    return to == 0 || to == 1;
  }
  return 0;
}

/* Check that the new entry points can handle this encoder at all. */
static int blocks_config_supported(shine_global_config *config) {
  if (config->mpeg.version != MPEG_I)
    return 0;
  if (config->wave.samplerate != 32000 && config->wave.samplerate != 44100 &&
      config->wave.samplerate != 48000)
    return 0;
  if (config->wave.channels != 1 && config->wave.channels != 2)
    return 0;
  if (config->mpeg.mode != MONO && config->mpeg.mode != STEREO)
    return 0;
  /* Mono streams must use the mono mode and vice versa. */
  if ((config->wave.channels == 1) != (config->mpeg.mode == MONO))
    return 0;
  return 1;
}

/* Validate and stage the requested window sequence. On failure no encoder
 * state is modified. */
static int blocks_prepare(shine_global_config *config, const uint8_t *types,
                          int count) {
  int ch, gr, i;
  int next[MAX_CHANNELS];
  int any_short = 0;

  if (!types || !blocks_config_supported(config))
    return 0;
  if (count != 2 * config->wave.channels)
    return 0;

  for (ch = 0; ch < config->wave.channels; ch++)
    next[ch] = config->window.previous_block_type[ch];

  i = 0;
  for (gr = 0; gr < 2; gr++)
    for (ch = 0; ch < config->wave.channels; ch++, i++) {
      int t = types[i];
      if (t < 0 || t > 3)
        return 0;
      if (!window_transition_ok(next[ch], t))
        return 0;
      next[ch] = t;
      if (t == 2)
        any_short = 1;
    }

  i = 0;
  config->window.any_short = any_short;
  for (gr = 0; gr < 2; gr++)
    for (ch = 0; ch < config->wave.channels; ch++, i++) {
      shine_mdct_set_block_type(config, gr, ch, types[i]);
      config->window.previous_block_type[ch] = types[i];
    }
  return 1;
}

unsigned char *shine_encode_buffer_blocks(shine_global_config *config,
                                          int16_t **data,
                                          const uint8_t *block_types,
                                          int block_type_count, int *written) {
  if (!blocks_prepare(config, block_types, block_type_count)) {
    if (written)
      *written = -1;
    return NULL;
  }

  config->buffer[0] = data[0];
  if (config->wave.channels == 2)
    config->buffer[1] = data[1];

  return shine_encode_buffer_internal(config, written, 1);
}

unsigned char *shine_encode_buffer_interleaved_blocks(
    shine_global_config *config, int16_t *data, const uint8_t *block_types,
    int block_type_count, int *written) {
  if (!blocks_prepare(config, block_types, block_type_count)) {
    if (written)
      *written = -1;
    return NULL;
  }

  config->buffer[0] = data;
  if (config->wave.channels == 2)
    config->buffer[1] = data + 1;

  return shine_encode_buffer_internal(config, written, config->wave.channels);
}

unsigned char *shine_flush(shine_global_config *config, int *written) {
  *written = config->bs.data_position;
  config->bs.data_position = 0;

  return config->bs.data;
}

void shine_close(shine_global_config *config) {
  shine_close_bit_stream(&config->bs);
  free(config);
}
