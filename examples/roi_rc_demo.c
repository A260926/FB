/*
 * ROI-aware rate control example for Kvazaar.
 *
 * Generates synthetic I420 material and a per-frame changing ROI map,
 * encodes it with the real library using --roi-rc semantics (the ROI map is
 * attached to each input picture, exactly as the public API allows), and
 * writes the HEVC bitstream and the reconstructed frames to disk.
 *
 * Build (from the repository root, after building the library):
 *
 *   cc -std=c99 -Isrc -Ibuild examples/roi_rc_demo.c \
 *      build/libkvazaar.a -lm -pthread -o roi_rc_demo
 *
 * Run:
 *
 *   ./roi_rc_demo out.hevc out.yuv
 *
 * The example does not need any external downloads.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvazaar.h"

#define WIDTH 192
#define HEIGHT 136
#define FRAME_COUNT 24
#define BITRATE 180000

static const kvz_api *api;

static void die(const char *msg)
{
  fprintf(stderr, "roi_rc_demo: %s\n", msg);
  exit(1);
}

static int parse(kvz_config *cfg, const char *name, const char *value)
{
  return api->config_parse(cfg, name, value);
}

/* A moving high-detail block plus text-like edges gives the ROI something to
 * protect. Coordinates move so the ROI map changes every frame. */
static void generate_luma(kvz_picture *pic, int frame)
{
  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      /* Background: smooth gradient. */
      int value = 40 + (x * 2) / 3 + (y * 2) / 3;
      /* Moving "face" region: a bright ellipse with fine texture. */
      int fx = WIDTH / 2 + (frame * 3) % 40 - 20;
      int fy = HEIGHT / 2 + (frame * 5) % 24 - 12;
      int dx = x - fx;
      int dy = y - fy;
      if (dx * dx * 6 + dy * dy * 14 < 900) {
        value = 170 + ((x * 7 + y * 11 + frame * 13) & 63);
      }
      /* "Text" stripe: strong vertical/horizontal edges. */
      if (y > HEIGHT / 4 && y < HEIGHT / 4 + 18 && ((x / 8) % 2 == 0)) {
        value = 220;
      } else if (y > HEIGHT / 4 && y < HEIGHT / 4 + 18 &&
                 x % 8 < 2) {
        value = 30;
      }
      pic->y[y * pic->stride + x] = (uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value));
    }
  }
}

static void generate_chroma(kvz_picture *pic, int frame)
{
  for (int y = 0; y < HEIGHT / 2; y++) {
    for (int x = 0; x < WIDTH / 2; x++) {
      pic->u[y * pic->stride_c + x] = (uint8_t)(120 + ((x + frame) % 16));
      pic->v[y * pic->stride_c + x] = (uint8_t)(140 - ((y + frame) % 16));
    }
  }
}

/* The ROI map follows the moving face (negative delta = more bits) and the
 * text stripe (negative delta). Everything else gets a positive delta. */
static void generate_roi(kvz_picture *pic, int frame)
{
  const int roi_w = 6;
  const int roi_h = 5;
  int8_t *map = (int8_t *)calloc((size_t)roi_w * roi_h, sizeof(int8_t));
  if (!map) die("out of memory");
  for (int ry = 0; ry < roi_h; ry++) {
    for (int rx = 0; rx < roi_w; rx++) {
      map[rx + ry * roi_w] = 6; /* background: fewer bits */
    }
  }
  /* Face block moves with the generated ellipse. */
  int face_rx = 2 + (frame * 3) / 40 % 2;
  int face_ry = 2;
  map[face_rx + face_ry * roi_w] = -10;
  map[face_rx + 1 + face_ry * roi_w] = -6;
  /* Text stripe. */
  map[0 + 1 * roi_w] = -8;
  map[1 + 1 * roi_w] = -8;
  map[2 + 1 * roi_w] = -8;
  pic->roi.width = roi_w;
  pic->roi.height = roi_h;
  pic->roi.roi_array = map;
}

int main(int argc, char **argv)
{
  const char *hevc_path = argc > 1 ? argv[1] : "roi_rc_demo.hevc";
  const char *recon_path = argc > 2 ? argv[2] : "roi_rc_demo.yuv";

  api = kvz_api_get(8);
  if (!api) die("failed to get the kvazaar API");

  kvz_config *cfg = api->config_alloc();
  if (!cfg || !api->config_init(cfg)) die("failed to create config");

  cfg->width = WIDTH;
  cfg->height = HEIGHT;
  cfg->roi_rc = 1;
  if (!parse(cfg, "preset", "ultrafast") ||
      !parse(cfg, "gop", "8") ||
      !parse(cfg, "threads", "1") ||
      !parse(cfg, "owf", "2") ||
      !parse(cfg, "bitrate", "180000") ||
      !parse(cfg, "rc-algorithm", "lambda")) {
    die("failed to set encoder options");
  }

  kvz_encoder *enc = api->encoder_open(cfg);
  if (!enc) {
    die("encoder_open failed (roi-rc needs a positive bitrate and "
        "rc-algorithm=lambda)");
  }

  FILE *hevc = fopen(hevc_path, "wb");
  FILE *recon = fopen(recon_path, "wb");
  if (!hevc || !recon) die("failed to open output files");

  unsigned frames_written = 0;
  unsigned total_bytes = 0;

  for (int n = 0; n < FRAME_COUNT + 16; n++) {
    kvz_picture *in = NULL;
    if (n < FRAME_COUNT) {
      in = api->picture_alloc(WIDTH, HEIGHT);
      if (!in) die("failed to allocate input picture");
      in->pts = n;
      generate_luma(in, n);
      generate_chroma(in, n);
      generate_roi(in, n);
    }

    kvz_data_chunk *chunks = NULL;
    uint32_t len = 0;
    kvz_picture *rec = NULL, *src = NULL;
    if (!api->encoder_encode(enc, in, &chunks, &len, &rec, &src, NULL)) {
      die("encoder_encode failed");
    }

    for (kvz_data_chunk *c = chunks; c; c = c->next) {
      if (fwrite(c->data, 1, c->len, hevc) != c->len) die("write failed");
      total_bytes += c->len;
    }
    if (len > 0 && rec) {
      /* Write the reconstruction as planar I420. */
      for (int y = 0; y < HEIGHT; y++) {
        if (fwrite(rec->y + y * rec->stride, 1, WIDTH, recon) != WIDTH) {
          die("write failed");
        }
      }
      for (int y = 0; y < HEIGHT / 2; y++) {
        if (fwrite(rec->u + y * rec->stride_c, 1, WIDTH / 2, recon) !=
            (size_t)(WIDTH / 2)) {
          die("write failed");
        }
      }
      for (int y = 0; y < HEIGHT / 2; y++) {
        if (fwrite(rec->v + y * rec->stride_c, 1, WIDTH / 2, recon) !=
            (size_t)(WIDTH / 2)) {
          die("write failed");
        }
      }
      frames_written++;
    }

    api->chunk_free(chunks);
    api->picture_free(rec);
    api->picture_free(src);
    if (in) {
      /* The encoder copies the picture (and shares the ROI array through the
       * image reference); the caller still owns this allocation. */
      api->picture_free(in);
    }
  }

  fclose(hevc);
  fclose(recon);
  api->encoder_close(enc);
  api->config_destroy(cfg);

  printf("ROI_RC_DEMO frames=%u bytes=%u hevc=%s recon=%s\n",
         frames_written, total_bytes, hevc_path, recon_path);
  return 0;
}
