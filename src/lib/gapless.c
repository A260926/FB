/* gapless.c
 *
 * Gapless streaming layer built on top of the fixed-point Shine encoding
 * core.  See gapless.h for the public contract.
 *
 * The core encoder consumes fixed 1152-sample blocks and never flushes its
 * analysis filter bank / MDCT overlap history.  This layer:
 *
 *   - accumulates arbitrary PCM fragments into one bounded input block;
 *   - zero-pads the trailing partial block at finish time and feeds one
 *     extra silent block so the filter history is really drained;
 *   - emits the few bytes still held in the bitstream word cache;
 *   - prefixes every file with an Info/LAME tag frame describing the exact
 *     encoder delay and end padding, patching the final counts back once
 *     the length of the recording is known.
 */

#include "gapless.h"

#include <stdlib.h>
#include <string.h>

#include "bitstream.h"
#include "layer3.h"
#include "types.h"

/* Number of samples the polyphase analysis filter bank looks into the
 * future: 16 groups of 32 input samples. */
#define GAPLESS_ANALYSIS_DELAY 512

/* Shine's total encoder delay, measured in input samples: the 512-sample
 * filter bank delay plus the 16-sample half-window delay of the MDCT.  The
 * decoder adds its own delay on top of this; that delay belongs to the
 * decoder and must never be reported here. */
#define GAPLESS_ENCODER_DELAY (GAPLESS_ANALYSIS_DELAY + 16) /* 528 */

/* One extra silent block is enough to push every buffered sample through
 * the analysis filter bank (delay 512) and the MDCT (half window 16). */
#define GAPLESS_DRAIN_BLOCKS 1

struct shine_gapless {
  shine_global_config *enc;
  shine_gapless_write_cb on_write;
  shine_gapless_patch_cb on_patch;
  void *user;

  int channels;
  int samples_per_pass;

  int16_t *block;        /* one samples_per_pass * channels block */
  size_t fill;           /* valid samples (per channel) in block */
  uint64_t total_samples;
  uint64_t total_bytes;
  uint32_t frame_count;

  int tag_frame_size;
  uint32_t frames_field_offset;
  uint32_t bytes_field_offset;
  uint32_t trim_field_offset;

  int status;
  int finished;
};

static int gapless_emit(shine_gapless_t *g, const unsigned char *data,
                        size_t len) {
  if (len == 0)
    return SHINE_GAPLESS_OK;

  if (g->on_write(g->user, data, len) != 0) {
    g->status = SHINE_GAPLESS_ERR_CALLBACK;
    return g->status;
  }

  g->total_bytes += len;
  return SHINE_GAPLESS_OK;
}

static int gapless_patch(shine_gapless_t *g, uint64_t offset,
                         const unsigned char *data, size_t len) {
  if (g->on_patch(g->user, offset, data, len) != 0) {
    g->status = SHINE_GAPLESS_ERR_CALLBACK;
    return g->status;
  }
  return SHINE_GAPLESS_OK;
}

static void put_be32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v >> 24);
  p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);
  p[3] = (unsigned char)v;
}

/* Build the leading Info/LAME tag frame.  It is a syntactically valid,
 * silent MP3 frame (zero side information) so generic parsers skip it, and
 * it is produced without touching the encoder, which keeps the encoding
 * filter history and the CBR padding phase untouched. */
static int gapless_build_tag(shine_gapless_t *g, unsigned char *tag) {
  shine_global_config *enc = g->enc;
  int sideinfo_bytes = (g->channels == 2) ? 32 : 17;
  int tag_offset = 4 + sideinfo_bytes;
  int lame_offset;

  memset(tag, 0, (size_t)g->tag_frame_size);

  /* Frame header, identical framing bits to the audio frames (padding bit
   * cleared: this frame does not participate in the padding phase). */
  tag[0] = 0xFF;
  tag[1] = (unsigned char)(0xF0 | (enc->mpeg.version << 3) |
                           (enc->mpeg.layer << 1) | 1);
  tag[2] = (unsigned char)((enc->mpeg.bitrate_index << 4) |
                           ((enc->mpeg.samplerate_index % 3) << 2));
  tag[3] = (unsigned char)((enc->mpeg.mode << 6) |
                           (enc->mpeg.mode_ext << 4) |
                           (enc->mpeg.copyright << 3) |
                           (enc->mpeg.original << 2) | enc->mpeg.emph);

  /* Side information is all zero: main_data_begin 0, no scale factors and
   * no main data, i.e. a silent frame decoders accept and skip. */

  /* Xing/Info header. */
  memcpy(tag + tag_offset, "Info", 4);
  put_be32(tag + tag_offset + 4, 3); /* FRAMES_FLAG | BYTES_FLAG */
  g->frames_field_offset = (uint32_t)(tag_offset + 8);
  g->bytes_field_offset = (uint32_t)(tag_offset + 12);
  put_be32(tag + g->frames_field_offset, 0); /* patched at finish */
  put_be32(tag + g->bytes_field_offset, 0);  /* patched at finish */

  /* 36-byte LAME extension. */
  lame_offset = tag_offset + 16;
  memcpy(tag + lame_offset, "LAME3.100", 9);
  /* All remaining extension fields stay zero: no replay gain, no lowpass
   * claim, no preset, and crucially no CRC is declared.  Only the 12-bit
   * encoder delay / end padding triplet (extension bytes 21..23) gets
   * filled in at finish. */
  g->trim_field_offset = (uint32_t)(lame_offset + 21);

  if (lame_offset + 36 > g->tag_frame_size)
    return SHINE_GAPLESS_ERR_PARAM;

  return SHINE_GAPLESS_OK;
}

shine_gapless_t *shine_gapless_open(const shine_config_t *config, void *user,
                                    shine_gapless_write_cb on_write,
                                    shine_gapless_patch_cb on_patch) {
  shine_gapless_t *g;
  shine_config_t local_config;
  unsigned char *tag = NULL;
  int channels;

  if (config == NULL || on_write == NULL || on_patch == NULL)
    return NULL;

  /* Only MPEG-I, 44.1/48 kHz, 128 kbps mono/stereo is supported, and the
   * wave and MPEG channel modes have to agree. */
  if (config->mpeg.bitr != 128 ||
      (config->wave.samplerate != 44100 &&
       config->wave.samplerate != 48000))
    return NULL;

  if (config->wave.channels == PCM_MONO && config->mpeg.mode == MONO)
    channels = 1;
  else if (config->wave.channels == PCM_STEREO && config->mpeg.mode == STEREO)
    channels = 2;
  else
    return NULL;

  g = calloc(1, sizeof(*g));
  if (g == NULL)
    return NULL;

  g->on_write = on_write;
  g->on_patch = on_patch;
  g->user = user;
  g->channels = channels;
  g->status = SHINE_GAPLESS_OK;

  /* The core copies the configuration internally; keep a mutable copy so
   * its (non-const) signature is honoured without touching the caller's
   * object. */
  local_config = *config;
  g->enc = shine_initialise(&local_config);
  if (g->enc == NULL)
    goto fail;

  if (g->enc->mpeg.version != MPEG_I)
    goto fail;

  g->samples_per_pass = shine_samples_per_pass(g->enc);
  if (g->samples_per_pass != SHINE_MAX_SAMPLES)
    goto fail;

  g->block = calloc((size_t)g->samples_per_pass * (size_t)channels,
                    sizeof(int16_t));
  if (g->block == NULL)
    goto fail;

  /* Unpadded frame slot for MPEG-I: floor(144 * bitrate * 1000 / rate). */
  g->tag_frame_size = (144000 * config->mpeg.bitr) / config->wave.samplerate;
  tag = malloc((size_t)g->tag_frame_size);
  if (tag == NULL)
    goto fail;

  if (gapless_build_tag(g, tag) != SHINE_GAPLESS_OK)
    goto fail;

  if (gapless_emit(g, tag, (size_t)g->tag_frame_size) != SHINE_GAPLESS_OK)
    goto fail;

  free(tag);
  return g;

fail:
  free(tag);
  if (g->enc != NULL)
    shine_close(g->enc);
  free(g->block);
  free(g);
  return NULL;
}

static int gapless_encode_block(shine_gapless_t *g) {
  int written = 0;
  unsigned char *data;

  data = shine_encode_buffer_interleaved(g->enc, g->block, &written);
  g->frame_count++;

  return gapless_emit(g, data, (size_t)written);
}

int shine_gapless_write(shine_gapless_t *g, const int16_t *pcm,
                        size_t frames) {
  size_t consumed = 0;

  if (g == NULL)
    return SHINE_GAPLESS_ERR_PARAM;
  if (g->status != SHINE_GAPLESS_OK)
    return g->status;
  if (g->finished)
    return SHINE_GAPLESS_ERR_STATE;
  if (frames != 0 && pcm == NULL)
    return SHINE_GAPLESS_ERR_PARAM;

  while (consumed < frames) {
    size_t room = (size_t)g->samples_per_pass - g->fill;
    size_t take = frames - consumed;
    int rc;

    if (take > room)
      take = room;

    memcpy(g->block + g->fill * g->channels, pcm + consumed * g->channels,
           take * (size_t)g->channels * sizeof(int16_t));
    g->fill += take;
    consumed += take;

    if (g->fill == (size_t)g->samples_per_pass) {
      rc = gapless_encode_block(g);
      if (rc != SHINE_GAPLESS_OK)
        return rc;
      g->fill = 0;
    }
  }

  g->total_samples += frames;
  return SHINE_GAPLESS_OK;
}

/* Emit the bytes still held in the bitstream word cache.  The core stores
 * the cache in big-endian byte order (see shine_putbits), so the valid
 * leading bytes are taken directly without any byte swap. */
static int gapless_flush_cache(shine_gapless_t *g) {
  bitstream_t *bs = &g->enc->bs;
  int valid_bytes = (32 - bs->cache_bits) / 8;
  unsigned char tail[4];
  int i;

  for (i = 0; i < valid_bytes; i++)
    tail[i] = (unsigned char)(bs->cache >> (24 - 8 * i));

  return gapless_emit(g, tail, (size_t)valid_bytes);
}

int shine_gapless_finish(shine_gapless_t *g) {
  unsigned char counts[8];
  unsigned char trim[3];
  uint64_t padded_samples;
  uint32_t end_padding;
  int rc, i;

  if (g == NULL)
    return SHINE_GAPLESS_ERR_PARAM;
  if (g->finished)
    return SHINE_GAPLESS_OK;
  if (g->status != SHINE_GAPLESS_OK)
    return g->status;
  if (g->total_samples == 0) {
    g->status = SHINE_GAPLESS_ERR_EMPTY;
    return g->status;
  }

  /* Flush the trailing partial PCM block with zero padding. */
  if (g->fill != 0) {
    for (i = (int)g->fill; i < g->samples_per_pass; i++)
      memset(g->block + i * g->channels, 0,
             (size_t)g->channels * sizeof(int16_t));
    rc = gapless_encode_block(g);
    if (rc != SHINE_GAPLESS_OK)
      return rc;
    g->fill = 0;
  }

  /* Drain the analysis filter bank and MDCT overlap history. */
  for (i = 0; i < GAPLESS_DRAIN_BLOCKS; i++) {
    memset(g->block, 0,
           (size_t)g->samples_per_pass * (size_t)g->channels *
               sizeof(int16_t));
    rc = gapless_encode_block(g);
    if (rc != SHINE_GAPLESS_OK)
      return rc;
  }

  /* Hand over any bytes not yet flushed out of the bitstream cache. */
  {
    int written = 0;
    unsigned char *data = shine_flush(g->enc, &written);
    rc = gapless_emit(g, data, (size_t)written);
    if (rc != SHINE_GAPLESS_OK)
      return rc;
  }
  rc = gapless_flush_cache(g);
  if (rc != SHINE_GAPLESS_OK)
    return rc;

  /* Number of samples (per channel) effectively presented to the encoder
   * once the tail padding and drain block are included. */
  padded_samples = (uint64_t)g->frame_count *
                   (uint64_t)g->samples_per_pass;
  end_padding =
      (uint32_t)(padded_samples - g->total_samples -
                 GAPLESS_ENCODER_DELAY);

  /* Patch the tag's frame and byte counts. */
  put_be32(counts, g->frame_count);
  put_be32(counts + 4, (uint32_t)g->total_bytes);
  rc = gapless_patch(g, g->frames_field_offset, counts, sizeof(counts));
  if (rc != SHINE_GAPLESS_OK)
    return rc;

  /* Patch the packed 12-bit encoder delay and 12-bit end padding. */
  trim[0] = (unsigned char)(GAPLESS_ENCODER_DELAY >> 4);
  trim[1] = (unsigned char)(((GAPLESS_ENCODER_DELAY & 0x0F) << 4) |
                            (end_padding >> 8));
  trim[2] = (unsigned char)(end_padding & 0xFF);
  rc = gapless_patch(g, g->trim_field_offset, trim, sizeof(trim));
  if (rc != SHINE_GAPLESS_OK)
    return rc;

  g->finished = 1;
  return SHINE_GAPLESS_OK;
}

void shine_gapless_close(shine_gapless_t *g) {
  if (g == NULL)
    return;

  if (g->enc != NULL)
    shine_close(g->enc);
  free(g->block);
  free(g);
}
