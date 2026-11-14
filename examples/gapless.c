/* examples/gapless.c
 *
 * Minimal gapless MP3 streaming example.
 *
 * It synthesizes a short stereo tone of unknown length and feeds it to the
 * gapless encoder in arbitrary small fragments.  The output is written to
 * the file given on the command line (default: gapless.mp3); the patch
 * callback seeks back into the file so the Info/LAME counts can be filled
 * in at finish time.
 *
 * Build against the in-tree library, for example:
 *   cc -std=c99 -O2 -I. -Isrc/lib examples/gapless.c (lib sources) -lm
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gapless.h"

struct file_writer {
  FILE *fp;
};

static int write_cb(void *user, const unsigned char *data, size_t len) {
  struct file_writer *w = (struct file_writer *)user;
  return fwrite(data, 1, len, w->fp) == len ? 0 : -1;
}

static int patch_cb(void *user, uint64_t offset, const unsigned char *data,
                    size_t len) {
  struct file_writer *w = (struct file_writer *)user;
  if (fseeko(w->fp, (off_t)offset, SEEK_SET) != 0)
    return -1;
  if (fwrite(data, 1, len, w->fp) != len)
    return -1;
  /* Leave the stream positioned at the end so subsequent writes append. */
  if (fseeko(w->fp, 0, SEEK_END) != 0)
    return -1;
  return 0;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "gapless.mp3";
  const int rate = 48000;
  const size_t total_frames = 100000; /* ~2.08 s of audio */
  const size_t fragment = 37;         /* deliberately not frame aligned */

  shine_config_t config;
  shine_gapless_t *enc;
  struct file_writer writer;
  int16_t chunk[37 * 2];
  size_t produced = 0;
  int rc = 1;

  writer.fp = fopen(path, "wb");
  if (writer.fp == NULL) {
    fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
    return 1;
  }

  memset(&config, 0, sizeof(config));
  shine_set_config_mpeg_defaults(&config.mpeg);
  config.wave.samplerate = rate;
  config.wave.channels = PCM_STEREO;
  config.mpeg.bitr = 128;
  config.mpeg.mode = STEREO;

  enc = shine_gapless_open(&config, &writer, write_cb, patch_cb);
  if (enc == NULL) {
    fprintf(stderr, "shine_gapless_open: unsupported configuration\n");
    goto out;
  }

  /* Feed the recording in arbitrary-sized interleaved fragments. */
  while (produced < total_frames) {
    size_t take = total_frames - produced;
    size_t i;
    if (take > fragment)
      take = fragment;

    for (i = 0; i < take; i++) {
      double t = (double)(produced + i) / rate;
      int16_t left = (int16_t)(16000.0 * sin(2.0 * 3.14159265 * 440.0 * t));
      int16_t right =
          (int16_t)(16000.0 * sin(2.0 * 3.14159265 * 660.0 * t));
      chunk[i * 2] = left;
      chunk[i * 2 + 1] = right;
    }

    if (shine_gapless_write(enc, chunk, take) != SHINE_GAPLESS_OK) {
      fprintf(stderr, "shine_gapless_write failed\n");
      goto out_enc;
    }
    produced += take;
  }

  if (shine_gapless_finish(enc) != SHINE_GAPLESS_OK) {
    fprintf(stderr, "shine_gapless_finish failed\n");
    goto out_enc;
  }

  printf("wrote %zu sample frames (%zu bytes including the Info tag) to %s\n",
         produced, (size_t)ftello(writer.fp), path);
  rc = 0;

out_enc:
  shine_gapless_close(enc);
out:
  if (fclose(writer.fp) != 0)
    rc = 1;
  return rc;
}
