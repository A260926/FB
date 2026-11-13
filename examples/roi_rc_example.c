/*****************************************************************************
 * ROI-aware rate control example for Kvazaar.
 *
 * Generates a short I420 sequence in memory together with a per-frame
 * changing ROI map, encodes it with the real libkvazaar using --roi-rc
 * semantics (a positive bitrate and the lambda rate controller) and writes:
 *
 *   <prefix>.hevc  - the HEVC bitstream
 *   <prefix>_rec.yuv - the reconstructed I420 frames
 *
 * The ROI map is attached through picture.roi, one of the two supported
 * input mechanisms (the other is the --roi text/binary file). The ROI
 * values redistribute the frame bit budget across CTUs; they are never
 * added directly to QP. The planned per-CTU budget is a target, not a hard
 * cap: the real bits written are taken from the CABAC bitstream and feed
 * the next frame's alpha/beta model training.
 *
 * Build (offline, no extra dependencies):
 *   cc -std=c99 -I<kvazaar-src>/src roi_rc_example.c -L<build-dir> \
 *      -lkvazaar -lm -lpthread -o roi_rc_example
 *
 * Run:
 *   ./roi_rc_example [output-prefix]
 ****************************************************************************/

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvazaar.h"

#define WIDTH       192
#define HEIGHT      136
#define FRAME_COUNT 12
#define ROI_COLS    3
#define ROI_ROWS    3

#define CHECK(x) do { \
  if (!(x)) { \
    fprintf(stderr, "failed at line %d: %s\n", __LINE__, #x); \
    exit(1); \
  } \
} while (0)

static const kvz_api *api;

static void option(kvz_config *c, const char *name, const char *value)
{
  if (!api->config_parse(c, name, value)) {
    fprintf(stderr, "config_parse failed for %s=%s\n", name, value);
    exit(1);
  }
}

/* Synthetic moving I420 content: a drifting gradient plus a bright band. */
static void fill_i420(kvz_picture *pic, int frame_num)
{
  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      int band = (y >= HEIGHT / 3 && y < 2 * HEIGHT / 3) ? 24 : 0;
      pic->y[y * pic->stride + x] =
        (uint8_t)(16 + ((x * 13 + y * 7 + frame_num * 29 + band) % 220));
    }
  }
  for (int y = 0; y < HEIGHT / 2; y++) {
    for (int x = 0; x < WIDTH / 2; x++) {
      pic->u[y * pic->stride_c + x] = (uint8_t)(96 + (frame_num % 16));
      pic->v[y * pic->stride_c + x] = 144;
    }
  }
}

/*
 * Per-frame ROI map. The left region is marked important (negative delta ->
 * more bits), the right region deprioritized (positive delta -> fewer
 * bits), and the map changes every frame so the budget is rebuilt each
 * time. Values are delta-QP units even though roi-rc maps them to the
 * spatial weight 2^(-delta/6) instead of adding them to QP.
 */
static void install_roi(kvz_picture *pic, int frame_num)
{
  int8_t *map = malloc((size_t)(ROI_COLS * ROI_ROWS));
  CHECK(map);
  for (int ry = 0; ry < ROI_ROWS; ry++) {
    for (int rx = 0; rx < ROI_COLS; rx++) {
      int delta;
      if (rx == 0) {
        delta = -(4 + (frame_num % 4));       /* important region */
      } else if (rx == ROI_COLS - 1) {
        delta = 4 + ((frame_num + 1) % 3);    /* background region */
      } else {
        delta = (frame_num % 2) ? -1 : 1;     /* shifting middle band */
      }
      map[rx + ry * ROI_COLS] = (int8_t)delta;
    }
  }
  pic->roi.width = ROI_COLS;
  pic->roi.height = ROI_ROWS;
  pic->roi.roi_array = map; /* freed by api->picture_free */
}

static void write_all(FILE *out, const kvz_data_chunk *chunks)
{
  for (const kvz_data_chunk *t = chunks; t; t = t->next) {
    CHECK(fwrite(t->data, 1, t->len, out) == t->len);
  }
}

static void write_i420(FILE *out, const kvz_picture *pic)
{
  for (int y = 0; y < HEIGHT; y++) {
    CHECK(fwrite(&pic->y[y * pic->stride], 1, WIDTH, out) == (size_t)WIDTH);
  }
  for (int y = 0; y < HEIGHT / 2; y++) {
    CHECK(fwrite(&pic->u[y * pic->stride_c], 1, WIDTH / 2, out) == (size_t)(WIDTH / 2));
  }
  for (int y = 0; y < HEIGHT / 2; y++) {
    CHECK(fwrite(&pic->v[y * pic->stride_c], 1, WIDTH / 2, out) == (size_t)(WIDTH / 2));
  }
}

int main(int argc, char **argv)
{
  const char *prefix = (argc > 1) ? argv[1] : "roi_rc";

  char hevc_path[256], rec_path[256];
  snprintf(hevc_path, sizeof(hevc_path), "%s.hevc", prefix);
  snprintf(rec_path, sizeof(rec_path), "%s_rec.yuv", prefix);

  FILE *hevc_file = fopen(hevc_path, "wb");
  FILE *rec_file = fopen(rec_path, "wb");
  CHECK(hevc_file && rec_file);

  api = kvz_api_get(8);
  CHECK(api);

  kvz_config *cfg = api->config_alloc();
  CHECK(cfg && api->config_init(cfg));
  cfg->width = WIDTH;
  cfg->height = HEIGHT;
  option(cfg, "preset", "ultrafast");
  option(cfg, "gop", "0");
  option(cfg, "threads", "1");
  option(cfg, "owf", "0");
  option(cfg, "wpp", "0");
  option(cfg, "bitrate", "200000");
  option(cfg, "rc-algorithm", "lambda");
  option(cfg, "roi-rc", "true"); /* enable ROI-aware rate control */

  kvz_encoder *enc = api->encoder_open(cfg);
  CHECK(enc);

  int frames_written = 0;
  uint64_t total_bytes = 0;

  for (int n = 0; n < FRAME_COUNT; n++) {
    kvz_picture *in = api->picture_alloc(WIDTH, HEIGHT);
    CHECK(in);
    in->pts = n;
    fill_i420(in, n);
    install_roi(in, n);

    kvz_picture *rec = NULL, *src = NULL;
    kvz_data_chunk *chunks = NULL;
    uint32_t len = 0;
    CHECK(api->encoder_encode(enc, in, &chunks, &len, &rec, &src, NULL));
    write_all(hevc_file, chunks);
    total_bytes += len;
    if (rec) {
      write_i420(rec_file, rec);
      frames_written++;
    }
    api->chunk_free(chunks);
    api->picture_free(rec);
    api->picture_free(src);
    api->picture_free(in);
  }

  /* Flush delayed output frames. */
  for (int guard = 0; guard < FRAME_COUNT + 4 && frames_written < FRAME_COUNT;
       guard++) {
    kvz_picture *rec = NULL, *src = NULL;
    kvz_data_chunk *chunks = NULL;
    uint32_t len = 0;
    CHECK(api->encoder_encode(enc, NULL, &chunks, &len, &rec, &src, NULL));
    write_all(hevc_file, chunks);
    total_bytes += len;
    if (rec) {
      write_i420(rec_file, rec);
      frames_written++;
    }
    api->chunk_free(chunks);
    api->picture_free(rec);
    api->picture_free(src);
  }
  CHECK(frames_written == FRAME_COUNT);

  api->encoder_close(enc);
  api->config_destroy(cfg);
  CHECK(!fclose(hevc_file));
  CHECK(!fclose(rec_file));

  printf("encoded %d I420 frames (%dx%d)\n", frames_written, WIDTH, HEIGHT);
  printf("  bitstream:      %s (%llu bytes)\n",
         hevc_path, (unsigned long long)total_bytes);
  printf("  reconstruction: %s\n", rec_path);
  printf("The per-CTU ROI budget is a target, not a cap; the real CABAC\n"
         "bit count, not the plan, trains the next frame's rate model.\n");
  return 0;
}
