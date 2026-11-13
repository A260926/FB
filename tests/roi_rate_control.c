/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Standalone tests for the optional ROI-aware rate control (roi-rc) mode.
 * The test has its own main() and is deliberately excluded from the
 * greatest-based kvazaar_tests binary (see tests/CMakeLists.txt). It drives
 * the real encoder library through the public API plus a few internal
 * observation points; no replacement encoder is used.
 *
 * Usage: roi_rate_control <allocation|feedback|mapping|compatibility>
 ****************************************************************************/

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvazaar.h"
#include "kvazaar_internal.h"
#include "encoderstate.h"
#include "rate_control.h"

#define CHECK(cond) do { \
  if (!(cond)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    exit(1); \
  } \
} while (0)

#define MAX_CTUS 256
#define TEST_W 192
#define TEST_H 136
#define TEST_FRAMES 10

static const kvz_api *api;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void option(kvz_config *c, const char *name, const char *value)
{
  if (!api->config_parse(c, name, value)) {
    fprintf(stderr, "config_parse failed for %s=%s\n", name, value);
    exit(1);
  }
}

static kvz_config *base_config(int width, int height)
{
  kvz_config *c = api->config_alloc();
  CHECK(c && api->config_init(c));
  c->width = width;
  c->height = height;
  option(c, "preset", "ultrafast");
  option(c, "gop", "0");
  option(c, "threads", "1");
  option(c, "owf", "0");
  option(c, "wpp", "0");
  option(c, "bitrate", "200000");
  option(c, "rc-algorithm", "lambda");
  return c;
}

static void fill_picture(kvz_picture *pic, int frame_num, int moving)
{
  for (int y = 0; y < pic->height; y++) {
    for (int x = 0; x < pic->width; x++) {
      uint8_t v;
      if (moving) {
        v = (uint8_t)(16 + ((x * 13 + y * 7 + frame_num * 23 + (x * y) % 59) % 220));
      } else {
        v = (uint8_t)(16 + ((x * 5 + y * 11) % 200));
      }
      pic->y[y * pic->stride + x] = v;
    }
  }
  for (int y = 0; y < pic->height / 2; y++) {
    for (int x = 0; x < pic->width / 2; x++) {
      pic->u[y * pic->stride / 2 + x] = (uint8_t)(96 + (frame_num % 16));
      pic->v[y * pic->stride / 2 + x] = 144;
    }
  }
}

static void set_roi_map(kvz_picture *pic, int roi_w, int roi_h,
                        const int8_t *values)
{
  if (pic->roi.roi_array) free(pic->roi.roi_array);
  pic->roi.width = roi_w;
  pic->roi.height = roi_h;
  pic->roi.roi_array = malloc((size_t)(roi_w * roi_h));
  CHECK(pic->roi.roi_array);
  memcpy(pic->roi.roi_array, values, (size_t)(roi_w * roi_h));
}

/* Constant-map provider used by the interleave test. */
static int g_static_delta = 0;
static int8_t g_static_roi[9];
static void static_roi_provider(kvz_picture *pic, int frame_num)
{
  (void)frame_num;
  for (int i = 0; i < 9; i++) g_static_roi[i] = (int8_t)g_static_delta;
  set_roi_map(pic, 3, 3, g_static_roi);
}

/* Active pixel count of a CTU in a width x height picture. */
static double ctu_pixels(int width, int height, int cols, int rows, int idx)
{
  int cx = idx % cols, cy = idx / cols;
  int pw = (cx == cols - 1) ? width - 64 * cx : 64;
  int ph = (cy == rows - 1) ? height - 64 * cy : 64;
  return (double)(pw * ph);
}

typedef struct {
  uint8_t *data;
  size_t size;
} byte_buffer_t;

static void buffer_append(byte_buffer_t *buf, const kvz_data_chunk *chunks)
{
  for (const kvz_data_chunk *t = chunks; t; t = t->next) {
    buf->data = realloc(buf->data, buf->size + t->len);
    CHECK(buf->data);
    memcpy(buf->data + buf->size, t->data, t->len);
    buf->size += t->len;
  }
}

/**
 * Per-frame snapshot of the internal rate control state, taken from the
 * encoder state that produced the output picture.
 */
typedef struct {
  int frame_num;
  int num_ctus;
  double pic_target;
  double roi_target[MAX_CTUS];
  double lambda[MAX_CTUS];
  double rc_alpha[MAX_CTUS];
  double rc_beta[MAX_CTUS];
  uint32_t bits[MAX_CTUS];
  int qp[MAX_CTUS];
  uint64_t rec_checksum;
} frame_obs_t;

static encoder_state_t *find_output_state(kvz_encoder *enc,
                                          const kvz_picture *rec)
{
  /* The returned picture is a reference to the output state's reconstructed
   * frame, so match the underlying luma buffer. Works for GOP reordering
   * because output order, not input frame number, selects the state. */
  for (unsigned s = 0; s < enc->num_encoder_states; ++s) {
    encoder_state_t *st = &enc->states[s];
    if (st->tile && st->tile->frame && st->tile->frame->rec &&
        st->tile->frame->rec->y == rec->y) {
      return st;
    }
  }
  return NULL;
}

static uint64_t luma_checksum(const kvz_picture *pic)
{
  uint64_t sum = 0;
  for (int y = 0; y < pic->height; y++) {
    for (int x = 0; x < pic->width; x++) {
      sum = sum * 131 + pic->y[y * pic->stride + x];
    }
  }
  return sum;
}

static void snapshot(encoder_state_t *st, frame_obs_t *obs, int frame_num,
                     const kvz_picture *rec)
{
  const int num = st->encoder_control->in.width_in_lcu *
                  st->encoder_control->in.height_in_lcu;
  CHECK(num <= MAX_CTUS);
  memset(obs, 0, sizeof(*obs));
  obs->frame_num = frame_num;
  obs->num_ctus = num;
  obs->pic_target = st->frame->cur_pic_target_bits;
  for (int i = 0; i < num; ++i) {
    obs->roi_target[i] = st->frame->lcu_stats[i].roi_target_bits;
    obs->lambda[i]     = st->frame->lcu_stats[i].lambda;
    obs->rc_alpha[i]   = st->frame->lcu_stats[i].rc_alpha;
    obs->rc_beta[i]    = st->frame->lcu_stats[i].rc_beta;
    obs->bits[i]       = st->frame->lcu_stats[i].bits;
    obs->qp[i]         = st->frame->lcu_stats[i].qp;
  }
  obs->rec_checksum = rec ? luma_checksum(rec) : 0;
}

/**
 * Encode TEST_FRAMES frames. When roi_provider is non-NULL it is called for
 * every input frame to install a (possibly changing) ROI map; otherwise the
 * frames carry no ROI. Returns observations for every output frame.
 */
typedef void (*roi_provider_fn)(kvz_picture *pic, int frame_num);

static frame_obs_t *run_encoder(kvz_config *cfg, int width, int height,
                                int *out_count, byte_buffer_t *stream,
                                roi_provider_fn roi_provider,
                                int moving, int total_frames)
{
  kvz_encoder *enc = api->encoder_open(cfg);
  CHECK(enc);

  frame_obs_t *obs = calloc((size_t)total_frames + 4, sizeof(frame_obs_t));
  CHECK(obs);
  int count = 0;

  /* Feed every input frame, then flush with NULL pictures until every
   * coded frame has been emitted (GOP reordering delays the output). */
  for (int n = 0; n < total_frames; n++) {
    kvz_picture *in = api->picture_alloc(width, height);
    CHECK(in);
    in->pts = n;
    fill_picture(in, n, moving);
    if (roi_provider) roi_provider(in, n);

    kvz_picture *rec = NULL, *src = NULL;
    kvz_data_chunk *chunks = NULL;
    uint32_t len = 0;
    CHECK(api->encoder_encode(enc, in, &chunks, &len, &rec, &src, NULL));
    if (stream) buffer_append(stream, chunks);
    if (rec) {
      encoder_state_t *st = find_output_state(enc, rec);
      CHECK(st);
      snapshot(st, &obs[count], st->frame->num, rec);
      count++;
    }
    api->chunk_free(chunks);
    api->picture_free(rec);
    api->picture_free(src);
    api->picture_free(in);
  }

  for (int guard = 0; guard < total_frames + 8 && count < total_frames; guard++) {
    kvz_picture *rec = NULL, *src = NULL;
    kvz_data_chunk *chunks = NULL;
    uint32_t len = 0;
    CHECK(api->encoder_encode(enc, NULL, &chunks, &len, &rec, &src, NULL));
    if (stream) buffer_append(stream, chunks);
    if (rec) {
      encoder_state_t *st = find_output_state(enc, rec);
      CHECK(st);
      snapshot(st, &obs[count], st->frame->num, rec);
      count++;
    }
    api->chunk_free(chunks);
    api->picture_free(rec);
    api->picture_free(src);
  }

  CHECK(count == total_frames);
  if (out_count) *out_count = count;
  api->encoder_close(enc);
  return obs;
}

/* ------------------------------------------------------------------ */
/* Independent reference solver for the allocation group               */
/* ------------------------------------------------------------------ */

static int ref_solve(int n, const double *p, const double *b_in,
                     const int8_t *d, double B, double *t_out)
{
  double P = 0;
  int any_roi = 0;
  for (int i = 0; i < n; i++) {
    P += p[i];
    if (d[i] != 0) any_roi = 1;
  }
  if (!any_roi || !(P > 0.0) || !(B > 0.0)) return 0;

  double b[MAX_CTUS], w[MAX_CTUS], lo[MAX_CTUS], hi[MAX_CTUS];
  double bsum = 0;
  for (int i = 0; i < n; i++) {
    b[i] = b_in[i] > 1e-12 * p[i] / P ? b_in[i] : 1e-12 * p[i] / P;
    bsum += b[i];
  }
  for (int i = 0; i < n; i++) {
    b[i] /= bsum;
    w[i]  = b[i] * pow(2.0, -(double)d[i] / 6.0);
    lo[i] = B * p[i] / (4.0 * P);
    hi[i] = 4.0 * B * p[i] / P;
  }

  double lo_c = 0.0, hi_c = 1.0;
  for (int guard = 0; guard < 5000; guard++) {
    double f = 0;
    for (int i = 0; i < n; i++) f += fmin(hi[i], fmax(lo[i], hi_c * w[i]));
    if (f >= B) break;
    hi_c *= 2.0;
  }
  for (int it = 0; it < 300; it++) {
    double mid = 0.5 * (lo_c + hi_c);
    double f = 0;
    for (int i = 0; i < n; i++) f += fmin(hi[i], fmax(lo[i], mid * w[i]));
    if (f < B) lo_c = mid; else hi_c = mid;
  }
  double c = 0.5 * (lo_c + hi_c);
  for (int i = 0; i < n; i++) {
    t_out[i] = fmin(hi[i], fmax(lo[i], c * w[i]));
  }
  return 1;
}

static void geometry(int width, int height, int *cols, int *rows,
                     double pixels[MAX_CTUS], int *n_out)
{
  int c = (width + 63) / 64;
  int r = (height + 63) / 64;
  *cols = c;
  *rows = r;
  int n = c * r;
  CHECK(n <= MAX_CTUS);
  double P_check = 0;
  for (int y = 0; y < r; y++) {
    int ph = (y == r - 1) ? height - 64 * y : 64;
    for (int x = 0; x < c; x++) {
      int pw = (x == c - 1) ? width - 64 * x : 64;
      pixels[x + y * c] = (double)(pw * ph);
      P_check += pw * ph;
    }
  }
  CHECK((int)P_check == width * height);
  *n_out = n;
}

/* ------------------------------------------------------------------ */
/* allocation group                                                    */
/* ------------------------------------------------------------------ */

static void test_allocation(void)
{
  const double B = 12345.0;

  /* Both edge geometries from the specification. */
  const int dims[][2] = { {192, 136}, {264, 130} };
  for (size_t g = 0; g < sizeof(dims) / sizeof(dims[0]); g++) {
    int cols, rows, n;
    double pixels[MAX_CTUS];
    geometry(dims[g][0], dims[g][1], &cols, &rows, pixels, &n);

    double base[MAX_CTUS];
    int8_t delta[MAX_CTUS];
    for (int i = 0; i < n; i++) {
      base[i]  = 1.0 / n + 0.05 * (i % 3);
      delta[i] = (int8_t)((i % 5) - 2); /* contains zeros and non-zeros */
    }
    base[0] = 0.0; /* an individual zero weight must stay solvable */

    double targets[MAX_CTUS], ref[MAX_CTUS], sentinel[MAX_CTUS];
    for (int i = 0; i < n; i++) sentinel[i] = -7.0;
    memcpy(targets, sentinel, sizeof(targets));

    CHECK(kvz_roi_rc_solve_targets(n, pixels, base, delta, B, targets) == 1);

    double P = 0, total = 0;
    int lo_hits = 0, hi_hits = 0;
    for (int i = 0; i < n; i++) P += pixels[i];
    for (int i = 0; i < n; i++) {
      double lo = B * pixels[i] / (4.0 * P);
      double hi = 4.0 * B * pixels[i] / P;
      CHECK(isfinite(targets[i]));
      CHECK(targets[i] >= lo * (1.0 - 1e-12) - 1e-12);
      CHECK(targets[i] <= hi * (1.0 + 1e-12) + 1e-12);
      if (fabs(targets[i] - lo) <= 1e-10 * B + 1e-12) lo_hits++;
      if (fabs(targets[i] - hi) <= 1e-10 * B + 1e-12) hi_hits++;
      total += targets[i];
    }
    /* Budget conservation in double precision, relative tolerance 1e-8. */
    CHECK(fabs(total - B) <= 1e-8 * B);

    /* Cross-check against an independently written solver. Both converge to
     * the same saturated structure and targets within the 1e-8 relative
     * budget tolerance the implementation is required to meet. */
    CHECK(ref_solve(n, pixels, base, delta, B, ref) == 1);
    for (int i = 0; i < n; i++) {
      CHECK(fabs(targets[i] - ref[i]) <= 2e-8 * B + 1e-12);
    }
    (void)cols; (void)rows; (void)lo_hits; (void)hi_hits;
  }

  /* Double-sided saturation: extreme deltas with group sizes chosen so the
   * target B is reached while both limits are held simultaneously. The two
   * boosted CTUs (d=-51) sit on the upper limit, each B*4/10, and the eight
   * deprioritized CTUs (d=+51) sit on the lower limit, each B/40; together
   * 2*0.4B + 8*0.025B = B. */
  {
    int n = 10;
    double pixels[10];
    double base[10];
    int8_t delta[10];
    for (int i = 0; i < n; i++) {
      pixels[i] = 4096.0;
      base[i] = 1.0 / n;
      delta[i] = (i < 2) ? -51 : 51;
    }
    double t[10];
    CHECK(kvz_roi_rc_solve_targets(n, pixels, base, delta, B, t) == 1);
    double P = 10.0 * 4096.0;
    int lo_seen = 0, hi_seen = 0;
    for (int i = 0; i < n; i++) {
      double lo = B * pixels[i] / (4.0 * P);
      double hi = 4.0 * B * pixels[i] / P;
      if (fabs(t[i] - lo) <= 2e-8 * B + 1e-12) lo_seen = 1;
      if (fabs(t[i] - hi) <= 2e-8 * B + 1e-12) hi_seen = 1;
    }
    CHECK(lo_seen && hi_seen);
    double total = 0;
    for (int i = 0; i < n; i++) total += t[i];
    CHECK(fabs(total - B) <= 1e-8 * B);
  }

  /* Several zero weights must not make the normalization unsolvable. */
  {
    int n = 6;
    double pixels[6] = { 100, 100, 100, 100, 100, 100 };
    double base[6]   = { 0, 0, 0.5, 0.5, 0, 0 };
    int8_t delta[6]  = { 3, -3, 0, 6, -6, 1 };
    double t[6];
    CHECK(kvz_roi_rc_solve_targets(n, pixels, base, delta, B, t) == 1);
    double total = 0;
    for (int i = 0; i < n; i++) {
      CHECK(isfinite(t[i]) && t[i] > 0.0);
      total += t[i];
    }
    CHECK(fabs(total - B) <= 1e-8 * B);
  }

  /* All-zero ROI map: no budget, the normal rate control path is kept. */
  {
    int n = 4;
    double pixels[4] = { 4096, 4096, 4096, 4096 };
    double base[4]   = { 0.25, 0.25, 0.25, 0.25 };
    int8_t delta[4]  = { 0, 0, 0, 0 };
    double t[4] = { 1, 2, 3, 4 };
    CHECK(kvz_roi_rc_solve_targets(n, pixels, base, delta, B, t) == 0);
    CHECK(t[0] == 1.0 && t[3] == 4.0); /* untouched */
  }

  /* Bad B values: never produce NaN, never write targets. */
  {
    int n = 2;
    double pixels[2] = { 4096, 4096 };
    double base[2]   = { 0.5, 0.5 };
    int8_t delta[2]  = { -3, 3 };
    double bad_B[] = { 0.0, -1.0, NAN, INFINITY };
    for (size_t k = 0; k < sizeof(bad_B) / sizeof(bad_B[0]); k++) {
      double t[2] = { 9.0, 9.0 };
      CHECK(kvz_roi_rc_solve_targets(n, pixels, base, delta, bad_B[k], t) == 0);
      CHECK(t[0] == 9.0 && t[1] == 9.0);
      CHECK(!isnan(t[0]) && !isnan(t[1]));
    }
  }
}

/* ------------------------------------------------------------------ */
/* feedback group: real encoding, model training and reconstruction    */
/* ------------------------------------------------------------------ */

static int8_t fb_roi[9];

static void fb_provider(kvz_picture *pic, int frame_num)
{
  /* Left column boosted, right column deprioritized; map changes lightly
   * per frame so the budget is rebuilt every frame. */
  for (int y = 0; y < 3; y++) {
    for (int x = 0; x < 3; x++) {
      fb_roi[x + y * 3] = x == 0 ? (int8_t)(-6 - (frame_num % 3))
                                 : (int8_t)(4 + (frame_num % 2));
    }
  }
  set_roi_map(pic, 3, 3, fb_roi);
}

static void test_feedback(void)
{
  int count_on, count_off;

  /* Use a real hierarchical GOP so at least two GOPs are coded and the
   * per-CTU alpha/beta model is exercised and retrained. */
  const int frames = 18;
  kvz_config *cfg_on = base_config(TEST_W, TEST_H);
  option(cfg_on, "gop", "8");
  option(cfg_on, "roi-rc", "true");
  byte_buffer_t stream_on = { 0 };
  frame_obs_t *on = run_encoder(cfg_on, TEST_W, TEST_H, &count_on, &stream_on,
                                fb_provider, 1, frames);
  api->config_destroy(cfg_on);

  kvz_config *cfg_off = base_config(TEST_W, TEST_H);
  option(cfg_off, "gop", "8");
  byte_buffer_t stream_off = { 0 };
  frame_obs_t *off = run_encoder(cfg_off, TEST_W, TEST_H, &count_off,
                                 &stream_off, NULL, 1, frames);
  api->config_destroy(cfg_off);

  CHECK(count_on == frames && count_off == frames);
  CHECK(stream_on.size > 0);

  const int cols = (TEST_W + 63) / 64;
  const int rows = (TEST_H + 63) / 64;

  int saw_directional = 0;
  for (int f = 0; f < frames; f++) {
    frame_obs_t *o = &on[f];
    CHECK(o->num_ctus == cols * rows);

    /* One complete, finite, conserved budget per frame. */
    double planned = 0;
    double spread = 0;
    for (int i = 0; i < o->num_ctus; i++) {
      CHECK(isfinite(o->roi_target[i]) && o->roi_target[i] > 0.0);
      planned += o->roi_target[i];
      spread += fabs(o->roi_target[i] - o->roi_target[0]);
    }
    CHECK(fabs(planned - o->pic_target) <= 1e-8 * o->pic_target);
    /* The ROI map makes the spatial budget non-uniform. */
    CHECK(spread > 1e-9);

    /* Real bits come from CABAC, not from the plan. */
    uint32_t real_total = 0;
    for (int i = 0; i < o->num_ctus; i++) real_total += o->bits[i];
    CHECK(real_total > 0);
    CHECK((double)real_total != planned);

    /* Final lambdas/QPs are legal. */
    for (int i = 0; i < o->num_ctus; i++) {
      CHECK(isfinite(o->lambda[i]));
      CHECK(o->lambda[i] >= 0.1 - 1e-12 && o->lambda[i] <= 10000.0 + 1e-9);
      CHECK(o->qp[i] >= 0 && o->qp[i] <= 51);
    }

    /* On the first output frame (the IDR) the classic spatial weights are
     * uniform, so the ROI factor alone decides: the boosted left column
     * receives more target bits per pixel and a lower QP than the
     * deprioritized right column. On later frames the learned complexity
     * weights legitimately dominate. */
    if (f == 0) {
      double bpp_left = 0, bpp_right = 0;
      int qp_left = 0, qp_right = 0;
      for (int y = 0; y < rows; y++) {
        int left = 0 + y * cols, right = (cols - 1) + y * cols;
        bpp_left  += o->roi_target[left]  / ctu_pixels(TEST_W, TEST_H, cols, rows, left);
        bpp_right += o->roi_target[right] / ctu_pixels(TEST_W, TEST_H, cols, rows, right);
        qp_left  += o->qp[left];
        qp_right += o->qp[right];
      }
      CHECK(bpp_left > bpp_right);
      CHECK(qp_left < qp_right);
      saw_directional = 1;
    }
  }
  CHECK(saw_directional);

  /* The plan changes the actual quantization, hence the reconstruction. */
  CHECK(on[0].rec_checksum != off[0].rec_checksum);
  CHECK(on[frames - 1].rec_checksum != off[frames - 1].rec_checksum);

  /* Real per-CTU results train the model: later frames carry updated
   * alpha/beta instead of the frame-zero seed values. */
  for (int i = 0; i < on[0].num_ctus; i++) {
    CHECK(on[1].rc_alpha[i] > 0.0 && on[1].rc_beta[i] < 0.0);
    int moved = (fabs(on[1].rc_alpha[i] - on[0].rc_alpha[i]) > 1e-12) ||
                (fabs(on[1].rc_beta[i]  - on[0].rc_beta[i])  > 1e-12);
    CHECK(moved);
  }

  /* A static-region sequence still trains the model over multiple GOPs. */
  kvz_config *cfg_static = base_config(TEST_W, TEST_H);
  option(cfg_static, "gop", "8");
  option(cfg_static, "roi-rc", "true");
  int sc;
  frame_obs_t *st = run_encoder(cfg_static, TEST_W, TEST_H, &sc, NULL,
                                fb_provider, 0, frames);
  CHECK(sc == frames);
  int trained = 0;
  for (int i = 0; i < st[0].num_ctus; i++) {
    if (fabs(st[sc - 1].rc_alpha[i] - st[0].rc_alpha[i]) > 1e-12) trained++;
  }
  CHECK(trained > 0);

  free(stream_on.data);
  free(stream_off.data);
  free(on);
  free(off);
  free(st);
  api->config_destroy(cfg_static);
}

/* ------------------------------------------------------------------ */
/* mapping group: non-uniform map, tiles, WPP, OWF, per-frame changes   */
/* ------------------------------------------------------------------ */

static int8_t map_a[15], map_b[15];

static void mapping_provider(kvz_picture *pic, int frame_num)
{
  /* Non-uniform 5x3 ROI map over a 3x3 CTU grid exercises the floor
   * mapping with unequal ROI and CTU dimensions. */
  int8_t *map = (frame_num % 2 == 0) ? map_a : map_b;
  set_roi_map(pic, 5, 3, map);
}

static void build_maps(void)
{
  for (int i = 0; i < 15; i++) {
    map_a[i] = (int8_t)((i % 7) - 3);
    map_b[i] = (int8_t)(-map_a[i]); /* visibly different next-frame map */
  }
  map_a[0] = -6;
  map_b[0] = 6;
}

static frame_obs_t *run_parallel_config(const char *threads, const char *owf,
                                        const char *wpp, const char *tiles,
                                        int *count_out)
{
  kvz_config *c = base_config(TEST_W, TEST_H);
  option(c, "roi-rc", "true");
  option(c, "threads", threads);
  option(c, "owf", owf);
  option(c, "wpp", wpp);
  if (tiles) option(c, "tiles", tiles);
  frame_obs_t *obs = run_encoder(c, TEST_W, TEST_H, count_out, NULL,
                                 mapping_provider, 1, 8);
  api->config_destroy(c);
  return obs;
}

static void test_mapping(void)
{
  build_maps();

  int n0, n1, n2, n3;
  frame_obs_t *single = run_parallel_config("1", "0", "0", NULL, &n0);
  frame_obs_t *tiles  = run_parallel_config("2", "0", "0", "2x2", &n1);
  frame_obs_t *wpp    = run_parallel_config("4", "0", "1", NULL, &n2);
  frame_obs_t *owf    = run_parallel_config("4", "2", "1", NULL, &n3);

  CHECK(n0 == 8 && n1 == 8 && n2 == 8 && n3 == 8);

  frame_obs_t *sets[4] = { single, tiles, wpp, owf };
  for (int k = 0; k < 4; k++) {
    frame_obs_t *s = sets[k];
    for (int f = 0; f < 8; f++) {
      /* Exactly one complete budget per frame: every CTU has a finite
       * target and the targets sum to the frame target. */
      double total = 0;
      int populated = 0;
      for (int i = 0; i < s[f].num_ctus; i++) {
        if (isfinite(s[f].roi_target[i]) && s[f].roi_target[i] > 0.0) {
          populated++;
        }
        total += s[f].roi_target[i];
      }
      CHECK(populated == s[f].num_ctus);
      CHECK(fabs(total - s[f].pic_target) <= 1e-8 * s[f].pic_target);
    }

    /* Per-frame map changes rebuild the budget instead of reusing the
     * previous frame's plan. */
    for (int f = 1; f < 8; f++) {
      double diff = 0;
      for (int i = 0; i < s[f].num_ctus; i++) {
        diff += fabs(s[f].roi_target[i] - s[f - 1].roi_target[i]);
      }
      CHECK(diff > 1e-9);
    }
  }

  /* Global localization: with uniform first-frame weights, every parallel
   * configuration must place the same per-CTU budget at the same global
   * CTU index. Byte-identical streams are intentionally not required. */
  for (int k = 1; k < 4; k++) {
    CHECK(sets[k][0].num_ctus == sets[0][0].num_ctus);
    for (int i = 0; i < sets[0][0].num_ctus; i++) {
      CHECK(fabs(sets[k][0].roi_target[i] - sets[0][0].roi_target[i])
            <= 1e-10 * sets[0][0].pic_target + 1e-12);
    }
  }

  /* Floor mapping sanity on the 5x3 map: CTU column 0 maps to ROI column 0,
   * the last CTU column maps to ROI column 4, and their ROI deltas differ
   * for frame 0, which must show up as different budgets per pixel. */
  {
    frame_obs_t *s = single;
    CHECK(map_a[0] != map_a[4]);
    double bpp_first = 0, bpp_last = 0;
    for (int y = 0; y < 3; y++) {
      bpp_first += s[0].roi_target[0 + y * 3];
      bpp_last  += s[0].roi_target[2 + y * 3];
    }
    CHECK(bpp_first != bpp_last);
  }

  free(single);
  free(tiles);
  free(wpp);
  free(owf);
}

/* ------------------------------------------------------------------ */
/* compatibility group                                                 */
/* ------------------------------------------------------------------ */

static void zero_roi_provider(kvz_picture *pic, int frame_num)
{
  (void)frame_num;
  int8_t zeros[9] = { 0 };
  set_roi_map(pic, 3, 3, zeros);
}

static int8_t classic_roi[9];
static void classic_roi_provider(kvz_picture *pic, int frame_num)
{
  (void)frame_num;
  for (int i = 0; i < 9; i++) classic_roi[i] = (i % 3 == 0) ? -6 : 3;
  set_roi_map(pic, 3, 3, classic_roi);
}

static byte_buffer_t encode_to_buffer(kvz_config *cfg,
                                      roi_provider_fn provider, int frames)
{
  byte_buffer_t buf = { 0 };
  frame_obs_t *obs = run_encoder(cfg, TEST_W, TEST_H, NULL, &buf, provider,
                                 1, frames);
  free(obs);
  return buf;
}

static void test_compatibility(void)
{
  /* Parse support for --roi-rc / --no-roi-rc and the default value. */
  {
    kvz_config *c = base_config(TEST_W, TEST_H);
    CHECK(c->roi_rc == 0);
    option(c, "roi-rc", "true");
    CHECK(c->roi_rc == 1);
    option(c, "no-roi-rc", "true");
    CHECK(c->roi_rc == 0);
    api->config_destroy(c);
  }

  /* Explicitly enabled but invalid combinations must fail encoder_open. */
  {
    kvz_config *c = base_config(TEST_W, TEST_H);
    option(c, "roi-rc", "true");
    option(c, "bitrate", "0");
    option(c, "rc-algorithm", "no-rc");
    CHECK(api->encoder_open(c) == NULL);
    api->config_destroy(c);
  }
  {
    kvz_config *c = base_config(TEST_W, TEST_H);
    option(c, "roi-rc", "true");
    option(c, "rc-algorithm", "oba");
    CHECK(api->encoder_open(c) == NULL);
    api->config_destroy(c);
  }
  {
    kvz_config *c = base_config(TEST_W, TEST_H);
    option(c, "roi-rc", "true");
    option(c, "bitrate", "0");
    option(c, "rc-algorithm", "lambda");
    CHECK(api->encoder_open(c) == NULL);
    api->config_destroy(c);
  }

  /* OBA keeps its own path: ROI with OBA still opens and encodes. */
  {
    kvz_config *c = base_config(TEST_W, TEST_H);
    option(c, "rc-algorithm", "oba");
    byte_buffer_t buf = encode_to_buffer(c, classic_roi_provider, 4);
    CHECK(buf.size > 0);
    free(buf.data);
    api->config_destroy(c);
  }

  /* Fixed-QP (no bitrate) classic ROI usage still works unchanged. */
  {
    kvz_config *c = api->config_alloc();
    CHECK(c && api->config_init(c));
    c->width = TEST_W; c->height = TEST_H;
    option(c, "preset", "ultrafast");
    option(c, "gop", "0");
    option(c, "threads", "1");
    option(c, "owf", "0");
    option(c, "wpp", "0");
    byte_buffer_t buf = encode_to_buffer(c, classic_roi_provider, 4);
    CHECK(buf.size > 0);
    free(buf.data);
    api->config_destroy(c);
  }

  /* Determinism with the new switch off: classic ROI and no-ROI streams
   * are reproducible bit for bit. */
  {
    kvz_config *c1 = base_config(TEST_W, TEST_H);
    kvz_config *c2 = base_config(TEST_W, TEST_H);
    byte_buffer_t a = encode_to_buffer(c1, classic_roi_provider, 6);
    byte_buffer_t b = encode_to_buffer(c2, classic_roi_provider, 6);
    CHECK(a.size == b.size && memcmp(a.data, b.data, a.size) == 0);
    free(a.data); free(b.data);
    api->config_destroy(c1); api->config_destroy(c2);
  }

  /* All-zero ROI maps, VAQ off: identical CTU targets/QPs/lambdas and an
   * identical bitstream to the new switch disabled with no ROI at all. */
  {
    kvz_config *cz = base_config(TEST_W, TEST_H);
    option(cz, "roi-rc", "true");
    option(cz, "vaq", "0");
    int nz;
    frame_obs_t *z = run_encoder(cz, TEST_W, TEST_H, &nz, NULL,
                                 zero_roi_provider, 1, 6);

    kvz_config *cn = base_config(TEST_W, TEST_H);
    option(cn, "vaq", "0");
    int nn;
    frame_obs_t *n = run_encoder(cn, TEST_W, TEST_H, &nn, NULL, NULL, 1, 6);
    CHECK(nz == nn);
    for (int f = 0; f < nz; f++) {
      for (int i = 0; i < z[f].num_ctus; i++) {
        CHECK(z[f].roi_target[i] == 0.0);
        CHECK(z[f].qp[i] == n[f].qp[i]);
        CHECK(z[f].lambda[i] == n[f].lambda[i]);
        CHECK(z[f].bits[i] == n[f].bits[i]);
      }
    }
    byte_buffer_t bz = encode_to_buffer(cz, zero_roi_provider, 6);
    byte_buffer_t bn = encode_to_buffer(cn, NULL, 6);
    CHECK(bz.size == bn.size && memcmp(bz.data, bn.data, bz.size) == 0);
    free(bz.data); free(bn.data);
    free(z); free(n);
    api->config_destroy(cz); api->config_destroy(cn);
  }

  /* VAQ applies on top of the ROI budget. The post-VAQ lambda/QP is what the
   * CTU codes with and what trains the next frame's model. */
  {
    kvz_config *cv = base_config(TEST_W, TEST_H);
    option(cv, "gop", "8");
    option(cv, "roi-rc", "true");
    option(cv, "vaq", "5");
    int vc;
    byte_buffer_t vstream = { 0 };
    frame_obs_t *v = run_encoder(cv, TEST_W, TEST_H, &vc, &vstream,
                                 fb_provider, 1, 10);
    CHECK(vc == 10 && vstream.size > 0);
    for (int f = 0; f < vc; f++) {
      double planned = 0;
      for (int i = 0; i < v[f].num_ctus; i++) {
        CHECK(isfinite(v[f].lambda[i]) && v[f].lambda[i] > 0.0);
        planned += v[f].roi_target[i];
      }
      CHECK(fabs(planned - v[f].pic_target) <= 1e-8 * v[f].pic_target);
    }
    /* The model still trains through the adjusted values. */
    int moved = 0;
    for (int i = 0; i < v[0].num_ctus; i++) {
      if (fabs(v[vc - 1].rc_alpha[i] - v[0].rc_alpha[i]) > 1e-12) moved++;
    }
    CHECK(moved > 0);
    free(vstream.data);
    free(v);
    api->config_destroy(cv);
  }

  /* Two encoder instances interleaved must not share state. */
  {
    kvz_config *ca = base_config(TEST_W, TEST_H);
    option(ca, "roi-rc", "true");
    kvz_config *cb = base_config(TEST_W, TEST_H);
    option(cb, "roi-rc", "true");

    /* Standalone reference runs with two different constant maps. */
    g_static_delta = -6;
    byte_buffer_t ref_a = encode_to_buffer(ca, static_roi_provider, 6);
    g_static_delta = 6;
    byte_buffer_t ref_b = encode_to_buffer(cb, static_roi_provider, 6);

    /* Interleaved run. */
    kvz_encoder *ea = api->encoder_open(ca);
    kvz_encoder *eb = api->encoder_open(cb);
    CHECK(ea && eb);
    byte_buffer_t ia = { 0 }, ib = { 0 };
    int got_a = 0, got_b = 0;
    for (int n = 0; n < 6; n++) {
      kvz_picture *pa = api->picture_alloc(TEST_W, TEST_H);
      kvz_picture *pb = api->picture_alloc(TEST_W, TEST_H);
      fill_picture(pa, n, 1);
      fill_picture(pb, n, 1);
      g_static_delta = -6;
      static_roi_provider(pa, n);
      g_static_delta = 6;
      static_roi_provider(pb, n);

      kvz_data_chunk *ca_chunks = NULL; uint32_t la = 0;
      kvz_picture *ra = NULL, *sa = NULL;
      CHECK(api->encoder_encode(ea, pa, &ca_chunks, &la, &ra, &sa, NULL));
      buffer_append(&ia, ca_chunks);
      api->chunk_free(ca_chunks); api->picture_free(ra); api->picture_free(sa);
      api->picture_free(pa);
      if (la) got_a++;

      kvz_data_chunk *cb_chunks = NULL; uint32_t lb = 0;
      kvz_picture *rb = NULL, *sb = NULL;
      CHECK(api->encoder_encode(eb, pb, &cb_chunks, &lb, &rb, &sb, NULL));
      buffer_append(&ib, cb_chunks);
      api->chunk_free(cb_chunks); api->picture_free(rb); api->picture_free(sb);
      api->picture_free(pb);
      if (lb) got_b++;
    }
    /* Flush both encoders. */
    for (int flush = 0; flush < 4; flush++) {
      kvz_data_chunk *ch = NULL; uint32_t l = 0;
      kvz_picture *r = NULL, *s = NULL;
      CHECK(api->encoder_encode(ea, NULL, &ch, &l, &r, &s, NULL));
      buffer_append(&ia, ch);
      api->chunk_free(ch); api->picture_free(r); api->picture_free(s);
      ch = NULL; l = 0; r = NULL; s = NULL;
      CHECK(api->encoder_encode(eb, NULL, &ch, &l, &r, &s, NULL));
      buffer_append(&ib, ch);
      api->chunk_free(ch); api->picture_free(r); api->picture_free(s);
    }
    CHECK(got_a == 6 && got_b == 6);
    CHECK(ia.size == ref_a.size && memcmp(ia.data, ref_a.data, ia.size) == 0);
    CHECK(ib.size == ref_b.size && memcmp(ib.data, ref_b.data, ib.size) == 0);

    free(ia.data); free(ib.data); free(ref_a.data); free(ref_b.data);
    api->encoder_close(ea);
    api->encoder_close(eb);
    api->config_destroy(ca);
    api->config_destroy(cb);
  }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
  if (argc != 2) {
    fprintf(stderr,
            "usage: %s <allocation|feedback|mapping|compatibility>\n",
            argv[0]);
    return 2;
  }

  api = kvz_api_get(8);
  CHECK(api);

  const char *group = argv[1];
  if (!strcmp(group, "allocation")) {
    test_allocation();
  } else if (!strcmp(group, "feedback")) {
    test_feedback();
  } else if (!strcmp(group, "mapping")) {
    test_mapping();
  } else if (!strcmp(group, "compatibility")) {
    test_compatibility();
  } else {
    fprintf(stderr, "unknown group: %s\n", group);
    return 2;
  }

  printf("ROI_RC_CASE %s PASS\n", group);
  return 0;
}
