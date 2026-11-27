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

  /* Window switching starts from the normal state on every channel. */
  {
    int i, g;
    for (i = 0; i < MAX_CHANNELS; i++) {
      config->last_block_type[i] = -1;
      for (g = 0; g < MAX_GRANULES; g++)
        config->block_type[i][g] = 0;
    }
    config->window_switching_active = 0;
  }

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
  config->window_switching_active = 0;
  config->buffer[0] = data[0];
  if (config->wave.channels == 2)
    config->buffer[1] = data[1];

  return shine_encode_buffer_internal(config, written, 1);
}

unsigned char *shine_encode_buffer_interleaved(shine_global_config *config,
                                               int16_t *data, int *written) {
  config->window_switching_active = 0;
  config->buffer[0] = data;
  if (config->wave.channels == 2)
    config->buffer[1] = data + 1;

  return shine_encode_buffer_internal(config, written, config->wave.channels);
}

/*
 * Validate a block-type request and stash the requested types in the config.
 * Returns 0 on success, -1 if the request must be rejected.  The check is
 * purely about the request and the configuration; it never touches PCM.
 */
static int shine_blocks_prepare(shine_global_config *config,
                                const uint8_t *block_types,
                                int block_type_count) {
  int n, ch, gr;

  if (config == NULL)
    return -1;

  /* Only MPEG-1 mono or plain stereo. */
  if (config->mpeg.version != MPEG_I)
    return -1;
  if (config->wave.channels != 1 && config->wave.channels != 2)
    return -1;
  if (config->mpeg.mode != MONO && config->mpeg.mode != STEREO)
    return -1;
  if (config->mpeg.granules_per_frame != 2)
    return -1;
  if (config->mpeg.samplerate_index < 0 || config->mpeg.samplerate_index > 2)
    return -1;

  n = 2 * config->wave.channels;
  if (block_types == NULL || block_type_count != n)
    return -1;

  /* Validate every transition before touching any PCM.  Each channel keeps
   * independent state; the very first call starts from normal. */
  for (ch = 0; ch < config->wave.channels; ch++) {
    int prev = config->last_block_type[ch];
    if (prev < 0)
      prev = 0;
    for (gr = 0; gr < 2; gr++) {
      int idx = gr * config->wave.channels + ch;
      int bt = block_types[idx];
      int ok = 0;
      if (bt < 0 || bt > 3)
        return -1;
      switch (prev) {
      case 0: ok = (bt == 0 || bt == 1); break; /* normal -> normal/start */
      case 1: ok = (bt == 2); break;            /* start  -> short */
      case 2: ok = (bt == 2 || bt == 3); break; /* short  -> short/stop */
      case 3: ok = (bt == 0 || bt == 1); break; /* stop   -> normal/start */
      default: return -1;
      }
      if (!ok)
        return -1;
      prev = bt;
    }
  }

  /* Everything is valid; commit the request. */
  for (ch = 0; ch < config->wave.channels; ch++) {
    int idx = 0;
    for (gr = 0; gr < 2; gr++) {
      idx = gr * config->wave.channels + ch;
      config->block_type[ch][gr] = block_types[idx];
    }
    config->last_block_type[ch] = block_types[config->wave.channels + ch];
  }
  config->window_switching_active = 1;
  return 0;
}

static unsigned char *shine_encode_blocks_common(shine_global_config *config,
                                                 const uint8_t *block_types,
                                                 int block_type_count,
                                                 int *written, int stride,
                                                 int interleaved,
                                                 int16_t **planar,
                                                 int16_t *raw) {
  /* `written` must be a valid output pointer; there is no way to report the
   * frame size otherwise, so a NULL request is rejected outright. */
  if (written == NULL)
    return NULL;

  if (shine_blocks_prepare(config, block_types, block_type_count) != 0) {
    *written = -1;
    return NULL;
  }

  if (interleaved) {
    config->buffer[0] = raw;
    if (config->wave.channels == 2)
      config->buffer[1] = raw + 1;
  } else {
    config->buffer[0] = planar[0];
    if (config->wave.channels == 2)
      config->buffer[1] = planar[1];
  }

  return shine_encode_buffer_internal(config, written, stride);
}

unsigned char *shine_encode_buffer_blocks(shine_global_config *config,
                                          int16_t **data,
                                          const uint8_t *block_types,
                                          int block_type_count, int *written) {
  return shine_encode_blocks_common(config, block_types, block_type_count,
                                    written, 1, 0, data, NULL);
}

unsigned char *shine_encode_buffer_interleaved_blocks(
    shine_global_config *config, int16_t *data, const uint8_t *block_types,
    int block_type_count, int *written) {
  int stride;
  if (config == NULL) {
    if (written) *written = -1;
    return NULL;
  }
  stride = config->wave.channels;
  return shine_encode_blocks_common(config, block_types, block_type_count,
                                    written, stride, 1, NULL, data);
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
