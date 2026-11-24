/*
 * ltr_example.c - dual-slot long-term reference example for Kvazaar.
 *
 * The program generates a small 64x64 sequence with a static texture and
 * moving frames, keeps the first IDR reconstruction in LTR slot 0, and
 * later predicts an exact return to the static texture from that slot.
 *
 * Usage: ltr_example <output.h265>
 */

#include "kvazaar.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 64
#define HEIGHT 64
#define FRAME_COUNT 48

static void fill_picture(kvz_picture *pic, int frame)
{
  /* Frames 0..7 and 16..23 are the same static scene. The middle frames
   * are moving/high-energy pictures. */
  int static_scene = frame < 8 || frame >= 16;

  for (int y = 0; y < HEIGHT; ++y) {
    for (int x = 0; x < WIDTH; ++x) {
      uint8_t value;
      if (static_scene) {
        int block = ((x >> 3) ^ (y >> 3)) & 1;
        value = block ? 196 : 52;
        if (((x + 2 * y) % 19) < 2) value = value > 128 ? 24 : 220;
      } else {
        int xx = (x + 4 * frame) & 63;
        int yy = (y + 3 * frame) & 63;
        value = (uint8_t)(((xx * 7 + yy * 13 + frame * 29) ^ (xx + frame)) & 255);
      }
      pic->y[y * pic->stride + x] = value;
    }
  }

  for (int y = 0; y < pic->height_c; ++y) {
    for (int x = 0; x < pic->width_c; ++x) {
      pic->u[y * pic->stride_c + x] = static_scene ? 112 : (uint8_t)(120 + frame);
      pic->v[y * pic->stride_c + x] = static_scene ? 152 : (uint8_t)(100 + 2 * frame);
    }
  }
}

static int write_chunks(FILE *out, kvz_data_chunk *chunk)
{
  for (kvz_data_chunk *p = chunk; p != NULL; p = p->next) {
    if (fwrite(p->data, 1, p->len, out) != p->len) return 0;
  }
  return 1;
}

int main(int argc, char **argv)
{
  if (argc != 2) {
    fprintf(stderr, "usage: %s <output.h265>\n", argv[0]);
    return 2;
  }

  const kvz_api *api = kvz_api_get(8);
  if (!api) {
    fprintf(stderr, "8-bit Kvazaar API is unavailable\n");
    return 1;
  }

  kvz_config *cfg = api->config_alloc();
  if (!cfg || !api->config_init(cfg)) {
    fprintf(stderr, "config allocation/initialization failed\n");
    return 1;
  }

  cfg->enable_logging_output = 0;
  if (!api->config_parse(cfg, "preset", "ultrafast") ||
      !api->config_parse(cfg, "ltr-slots", "2")) {
    fprintf(stderr, "failed to enable ltr-slots=2\n");
    return 1;
  }

  cfg->width = WIDTH;
  cfg->height = HEIGHT;
  cfg->gop_len = 0;
  cfg->intra_period = 0;
  cfg->ref_frames = 2;
  cfg->bipred = 0;
  cfg->tmvp_enable = 0;
  cfg->owf = 0;
  cfg->threads = 2;
  cfg->wpp = 1;
  cfg->cpuid = 0;

  kvz_encoder *enc = api->encoder_open(cfg);
  if (!enc) {
    fprintf(stderr, "encoder_open rejected the LTR configuration\n");
    api->config_destroy(cfg);
    return 1;
  }

  FILE *out = fopen(argv[1], "wb");
  if (!out) {
    perror(argv[1]);
    api->encoder_close(enc);
    api->config_destroy(cfg);
    return 1;
  }

  int frames = 0;
  uint64_t bytes = 0;

  for (int n = 0; n < FRAME_COUNT; ++n) {
    kvz_picture *pic = api->picture_alloc(WIDTH, HEIGHT);
    if (!pic) {
      fprintf(stderr, "picture_alloc failed\n");
      goto fail;
    }

    pic->pts = n;
    fill_picture(pic, n);
    pic->ltr_mark = (n == 4) ? 1 : -1;
    /* After the moving middle section, predict the returning static scene
     * directly from the IDR reconstruction stored in slot 0. */
    pic->ltr_use = -1; if (0) {} 
    pic->ltr_use = (n == 17) ? 1 : -1;

    kvz_data_chunk *data = NULL;
    uint32_t len = 0;
    kvz_picture *rec = NULL, *src = NULL;
    kvz_frame_info info;
    int ok = api->encoder_encode(enc, pic, &data, &len, &rec, &src, &info);
    api->picture_free(pic);

    if (!ok || (len && !write_chunks(out, data))) {
      fprintf(stderr, "encoding/writing frame %d failed\n", n);
      api->chunk_free(data);
      api->picture_free(rec);
      api->picture_free(src);
      goto fail;
    }

    bytes += len;
    if (rec) ++frames;
    api->chunk_free(data);
    api->picture_free(rec);
    api->picture_free(src);
  }

  /* NULL is the drain marker. Keep calling until all OWF frames are out. */
  for (int i = 0; i < 16 && frames < FRAME_COUNT; ++i) {
    kvz_data_chunk *data = NULL;
    uint32_t len = 0;
    kvz_picture *rec = NULL, *src = NULL;
    kvz_frame_info info;
    int ok = api->encoder_encode(enc, NULL, &data, &len, &rec, &src, &info);
    if (!ok || (len && !write_chunks(out, data))) {
      fprintf(stderr, "flush failed\n");
      api->chunk_free(data);
      api->picture_free(rec);
      api->picture_free(src);
      goto fail;
    }
    bytes += len;
    if (rec) ++frames;
    api->chunk_free(data);
    api->picture_free(rec);
    api->picture_free(src);
  }

  if (fclose(out) != 0) {
    perror("fclose");
    out = NULL;
    goto fail_closed;
  }

  api->encoder_close(enc);
  api->config_destroy(cfg);

  printf("ltr_example output=%s frames=%d bytes=%llu\n",
         argv[1], frames, (unsigned long long)bytes);
  return frames == FRAME_COUNT ? 0 : 1;

fail:
  if (out) fclose(out);
fail_closed:
  api->encoder_close(enc);
  api->config_destroy(cfg);
  return 1;
}
