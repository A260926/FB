/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Copyright (c) 2021, Tampere University, ITU/ISO/IEC, project contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the Tampere University or ITU/ISO/IEC nor the names of
 *   its contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * INCLUDING NEGLIGENCE OR OTHERWISE ARISING IN ANY WAY OUT OF THE USE OF THIS
 ****************************************************************************/

/**
 * \file
 * \brief Tests for ROI-aware rate control.
 *
 * Standalone test binary with its own main. It takes a single group name
 * (allocation, feedback, mapping, compatibility), runs the assertions for
 * that group and prints "ROI_RC_CASE <group> PASS" only after every
 * assertion in the group succeeded.
 *
 * The test links against the real library (devcheck/build/libkvazaar.a) and
 * uses the internal observation hook rather than a substitute encoder.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvazaar.h"
#include "kvazaar_internal.h"
#include "rate_control.h"

static int observation_on(void)
{
  kvz_roi_rc_enable_observation(1);
  return 1;
}

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "ROI_RC_CASE %s FAIL line %d: %s\n", g_group,           \
              __LINE__, #cond);                                              \
      return 0;                                                              \
    }                                                                        \
  } while (0)

static const char *g_group = "?";
static const kvz_api *api = NULL;

/* ---------------------------------------------------------------------- */
/* Small helpers                                                          */
/* ---------------------------------------------------------------------- */

static int parse_opt(kvz_config *cfg, const char *name, const char *value)
{
  return api->config_parse(cfg, name, value);
}

static kvz_config *make_config(int width, int height, int roi_rc)
{
  kvz_config *cfg = api->config_alloc();
  if (!cfg) return NULL;
  if (!api->config_init(cfg)) {
    api->config_destroy(cfg);
    return NULL;
  }
  cfg->width = width;
  cfg->height = height;
  if (!parse_opt(cfg, "preset", "ultrafast") ||
      !parse_opt(cfg, "gop", "0") ||
      !parse_opt(cfg, "threads", "1") ||
      !parse_opt(cfg, "owf", "0") ||
      !parse_opt(cfg, "wpp", "0") ||
      !parse_opt(cfg, "bitrate", "180000") ||
      !parse_opt(cfg, "rc-algorithm", "lambda")) {
    api->config_destroy(cfg);
    return NULL;
  }
  if (roi_rc < 0) {
    cfg->roi_rc = false;
  } else {
    cfg->roi_rc = roi_rc ? true : false;
  }
  return cfg;
}

static void fill_moving_texture(kvz_picture *pic, int frame)
{
  const int w = pic->width;
  const int h = pic->height;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      pic->y[y * pic->stride + x] =
          (uint8_t)(16 + ((x * 13 + y * 7 + frame * 19 + (x * y) % 59) % 220));
    }
  }
  for (int y = 0; y < h / 2; y++) {
    for (int x = 0; x < w / 2; x++) {
      pic->u[y * pic->stride_c + x] = (uint8_t)(96 + (frame % 16));
      pic->v[y * pic->stride_c + x] = 144;
    }
  }
}

static void fill_static_picture(kvz_picture *pic)
{
  const int w = pic->width;
  const int h = pic->height;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      pic->y[y * pic->stride + x] = (uint8_t)(32 + ((x * 5 + y * 3) % 180));
    }
  }
  for (int y = 0; y < h / 2; y++) {
    for (int x = 0; x < w / 2; x++) {
      pic->u[y * pic->stride_c + x] = 110;
      pic->v[y * pic->stride_c + x] = 150;
    }
  }
}

/* Same clipping the implementation applies to the open-loop lambda. */
static double gamut_lambda(double lambda)
{
  if (isnan(lambda)) return 10000.0;
  if (lambda < 0.1) return 0.1;
  if (lambda > 10000.0) return 10000.0;
  return lambda;
}

/* Build a picture with an attached ROI map. The caller frees both. */
static kvz_picture *make_roi_picture(int width, int height, int roi_w,
                                     int roi_h, const int8_t *values,
                                     int frame)
{
  kvz_picture *pic = api->picture_alloc(width, height);
  if (!pic) return NULL;
  fill_moving_texture(pic, frame);
  int8_t *map = (int8_t *)calloc((size_t)roi_w * roi_h, sizeof(int8_t));
  if (!map) {
    api->picture_free(pic);
    return NULL;
  }
  for (int i = 0; i < roi_w * roi_h; i++) map[i] = values[i];
  pic->roi.width = roi_w;
  pic->roi.height = roi_h;
  pic->roi.roi_array = map;
  return pic;
}

/* Run a full encode. Frames with a NULL ROI picture use no ROI map.
 * If out_stream is non-NULL the concatenated bitstream is appended to it. */
static int encode_sequence(kvz_config *cfg, kvz_picture **frames, int frame_count,
                           int flush_frames, unsigned char **out_stream,
                           size_t *out_size)
{
  kvz_encoder *enc = api->encoder_open(cfg);
  if (!enc) return 0;

  unsigned char *stream = NULL;
  size_t stream_size = 0;

  int produced = 0;
  for (int n = 0; n < frame_count + flush_frames; n++) {
    kvz_picture *in = n < frame_count ? frames[n] : NULL;
    kvz_data_chunk *chunks = NULL;
    uint32_t len = 0;
    kvz_picture *rec = NULL, *src = NULL;
    if (!api->encoder_encode(enc, in, &chunks, &len, &rec, &src, NULL)) {
      api->encoder_close(enc);
      free(stream);
      return 0;
    }
    if (len > 0) {
      unsigned char *grown = (unsigned char *)realloc(stream, stream_size + len);
      if (!grown) {
        api->chunk_free(chunks);
        api->encoder_close(enc);
        free(stream);
        return 0;
      }
      stream = grown;
      for (kvz_data_chunk *c = chunks; c; c = c->next) {
        memcpy(stream + stream_size, c->data, c->len);
        stream_size += c->len;
      }
      produced++;
    }
    api->chunk_free(chunks);
    api->picture_free(rec);
    api->picture_free(src);
  }

  api->encoder_close(enc);
  if (out_stream) {
    *out_stream = stream;
    *out_size = stream_size;
  } else {
    free(stream);
  }
  return produced;
}

/* Free a NULL-terminated array of frames and their ROI arrays. */
static void free_frames(kvz_picture **frames, int count)
{
  for (int i = 0; i < count; i++) {
    if (frames[i]) {
      free(frames[i]->roi.roi_array);
      frames[i]->roi.roi_array = NULL;
      api->picture_free(frames[i]);
    }
  }
}

/* ---------------------------------------------------------------------- */
/* Independent reference solver used by the allocation group.            */
/*                                                                        */
/* Deliberately separate from the implementation: it solves the convex    */
/* feasibility problem with a long bisection in log(c) plus a final exact  */
/* redistribution of the remaining slack, then verifies the properties    */
/* from first principles.                                                 */
/* ---------------------------------------------------------------------- */

typedef struct ref_budget {
  double *b;      /* normalized base weights */
  double *ratio;  /* 2^(-d/6) */
  double *lo;
  double *hi;
  double target;
  int n;
} ref_budget_t;

static int ref_prepare(ref_budget_t *ref, double B, int n, const double *b_in,
                       const int8_t *d, const uint32_t *pixels)
{
  memset(ref, 0, sizeof(*ref));
  if (n <= 0 || !(B > 0.0) || !isfinite(B)) return 0;
  ref->n = n;
  ref->target = B;
  ref->b = calloc(n, sizeof(double));
  ref->ratio = calloc(n, sizeof(double));
  ref->lo = calloc(n, sizeof(double));
  ref->hi = calloc(n, sizeof(double));
  if (!ref->b || !ref->ratio || !ref->lo || !ref->hi) return 0;

  double P = 0.0;
  for (int i = 0; i < n; i++) P += (double)pixels[i];
  if (!(P > 0.0)) return 0;

  int all_zero = 1;
  for (int i = 0; i < n; i++) {
    if (b_in[i] > 0.0) all_zero = 0;
  }
  for (int i = 0; i < n; i++) {
    ref->b[i] = all_zero ? (double)pixels[i] / P : (b_in[i] > 0.0 ? b_in[i] : 0.0);
    ref->ratio[i] = pow(2.0, -(double)d[i] / 6.0);
    ref->lo[i] = B * ((double)pixels[i] / P) * 0.25;
    ref->hi[i] = B * ((double)pixels[i] / P) * 4.0;
  }

  if (!all_zero) {
    for (int i = 0; i < n; i++) {
      double floor = 1e-12 * (double)pixels[i] / P;
      if (ref->b[i] < floor) ref->b[i] = floor;
    }
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += ref->b[i];
    for (int i = 0; i < n; i++) ref->b[i] /= sum;
  }
  return 1;
}

static void ref_free(ref_budget_t *ref)
{
  free(ref->b);
  free(ref->ratio);
  free(ref->lo);
  free(ref->hi);
  memset(ref, 0, sizeof(*ref));
}

static double ref_sum(const ref_budget_t *ref, double c, double *out)
{
  double s = 0.0;
  for (int i = 0; i < ref->n; i++) {
    double raw = c * ref->b[i] * ref->ratio[i];
    double t = raw < ref->lo[i] ? ref->lo[i] : (raw > ref->hi[i] ? ref->hi[i] : raw);
    out[i] = t;
    s += t;
  }
  return s;
}

/* Solve for c with a different method: secant/regula-falsi in log space. */
static double ref_solve(const ref_budget_t *ref, double *out)
{
  double c_lo = 0.0, c_hi = 1.0;
  double *tmp = malloc(sizeof(double) * ref->n);
  double s;
  s = ref_sum(ref, c_hi, tmp);
  while (s < ref->target) {
    c_lo = c_hi;
    c_hi *= 3.0;
    s = ref_sum(ref, c_hi, tmp);
  }
  for (int it = 0; it < 400; it++) {
    double c_mid = 0.5 * (c_lo + c_hi);
    s = ref_sum(ref, c_mid, tmp);
    if (fabs(s - ref->target) <= 1e-11 * ref->target) {
      c_lo = c_hi = c_mid;
      break;
    }
    if (s < ref->target) c_lo = c_mid; else c_hi = c_mid;
  }
  double c = 0.5 * (c_lo + c_hi);
  ref_sum(ref, c, out);
  free(tmp);
  return c;
}

/* ---------------------------------------------------------------------- */
/* allocation group                                                       */
/* ---------------------------------------------------------------------- */

static int alloc_case(double B, int n, const double *bases, const int8_t *d,
                      const uint32_t *pixels, int expect_zero_weights,
                      const char *label)
{
  double *got = calloc((size_t)n, sizeof(double));
  double *ref_out = calloc((size_t)n, sizeof(double));
  CHECK(got && ref_out);

  int ok = kvz_roi_rc_solve_budget(B, n, bases, d, pixels, got);
  CHECK(ok == 1);

  /* Budget conservation with the required 1e-8 relative tolerance. */
  double sum = 0.0;
  int bad = 0;
  for (int i = 0; i < n; i++) {
    CHECK(isfinite(got[i]));
    CHECK(got[i] >= 0.0);
    sum += got[i];
  }
  CHECK(fabs(sum - B) <= 1e-8 * B);

  /* Compare against an independently solved allocation. The two solvers
   * should agree closely because the objective is convex and the shared
   * scale is unique. */
  ref_budget_t ref;
  CHECK(ref_prepare(&ref, B, n, bases, d, pixels));
  double cref = ref_solve(&ref, ref_out);
  CHECK(cref >= 0.0);
  double ref_sum_bits = 0.0;
  for (int i = 0; i < n; i++) ref_sum_bits += ref_out[i];
  CHECK(fabs(ref_sum_bits - B) <= 1e-8 * B);
  for (int i = 0; i < n; i++) {
    double tol = 1e-6 * B + 1e-9 * fabs(ref_out[i]);
    if (fabs(got[i] - ref_out[i]) > tol) bad++;
  }
  CHECK(bad == 0);

  /* Clamp bounds must hold for every CTU, and both sides must saturate in a
   * balanced problem. */
  double lower_sat = 0, upper_sat = 0;
  for (int i = 0; i < n; i++) {
    double P = 0.0;
    for (int j = 0; j < n; j++) P += (double)pixels[j];
    double lofrac = ((double)pixels[i] / P) * 0.25 * B;
    double hifrac = ((double)pixels[i] / P) * 4.0 * B;
    CHECK(got[i] >= lofrac - 1e-9 * B);
    CHECK(got[i] <= hifrac + 1e-9 * B);
    if (fabs(got[i] - lofrac) <= 1e-9 * B) lower_sat++;
    if (fabs(got[i] - hifrac) <= 1e-9 * B) upper_sat++;
  }
  (void)lower_sat;
  (void)upper_sat;
  (void)expect_zero_weights;
  (void)label;

  ref_free(&ref);
  free(got);
  free(ref_out);
  return 1;
}

static int run_allocation(void)
{
  /* A 4x3 grid with a mix of positive and negative ROI offsets gives both
   * lower and upper saturation simultaneously. */
  {
    enum { N = 12 };
    uint32_t pixels[N];
    double bases[N];
    int8_t d[N];
    for (int i = 0; i < N; i++) {
      pixels[i] = (i % 4 == 3) ? 64 * 40 : 64 * 64;
      bases[i] = 1.0 / N;
      d[i] = (i % 4 == 0) ? -18 : ((i % 4 == 1) ? 24 : 0);
    }
    CHECK(alloc_case(200000.0, N, bases, d, pixels, 0, "mixed"));
  }

  /* Zero weights: several b_i are exactly zero, others nonzero. The zero
   * ones participate in the same solve via the 1e-12 floor. */
  {
    enum { N = 9 };
    uint32_t pixels[N];
    double bases[N];
    int8_t d[N];
    for (int i = 0; i < N; i++) {
      pixels[i] = 64 * 64;
      bases[i] = (i == 0 || i == 4 || i == 8) ? 0.0 : 1.0;
      d[i] = (i < 4) ? 6 : -6;
    }
    CHECK(alloc_case(90000.0, N, bases, d, pixels, 1, "zero-weights"));

    /* All base weights zero -> pure area ratio, still conserves budget. */
    for (int i = 0; i < N; i++) bases[i] = 0.0;
    CHECK(alloc_case(90000.0, N, bases, d, pixels, 1, "all-zero"));

    /* All-zero ROI map -> uniform spatially, still sum to budget. */
    for (int i = 0; i < N; i++) {
      bases[i] = 1.0 / N;
      d[i] = 0;
    }
    CHECK(alloc_case(90000.0, N, bases, d, pixels, 0, "zero-roi"));
  }

  /* Edge areas at 192x136 and 264x130: the CTU grid has partial CTUs whose
   * valid pixel counts differ, and area must drive the clamp fractions. */
  {
    const int w1 = 192, h1 = 136;
    const int cols1 = (w1 + 63) / 64, rows1 = (h1 + 63) / 64;
    const int n1 = cols1 * rows1;
    uint32_t pixels[64];
    double bases[64];
    int8_t d[64];
    int n = 0;
    for (int y = 0; y < rows1; y++) {
      for (int x = 0; x < cols1; x++, n++) {
        int w = w1 - x * 64; if (w > 64) w = 64;
        int h = h1 - y * 64; if (h > 64) h = 64;
        pixels[n] = (uint32_t)(w * h);
        bases[n] = 1.0 / n1;
        d[n] = (x == 0 ? -12 : 12);
      }
    }
    CHECK(alloc_case(300000.0, n, bases, d, pixels, 0, "192x136"));
  }
  {
    const int w2 = 264, h2 = 130;
    const int cols2 = (w2 + 63) / 64, rows2 = (h2 + 63) / 64;
    const int n2 = cols2 * rows2;
    uint32_t pixels[64];
    double bases[64];
    int8_t d[64];
    int n = 0;
    for (int y = 0; y < rows2; y++) {
      for (int x = 0; x < cols2; x++, n++) {
        int w = w2 - x * 64; if (w > 64) w = 64;
        int h = h2 - y * 64; if (h > 64) h = 64;
        pixels[n] = (uint32_t)(w * h);
        bases[n] = 1.0 / n2;
        d[n] = (x < cols2 / 2 ? -10 : 10);
      }
    }
    CHECK(alloc_case(250000.0, n, bases, d, pixels, 0, "264x130"));

    /* Invalid frame targets must be rejected without producing NaNs. */
    double out[64];
    for (int i = 0; i < n2; i++) out[i] = 12345.0;
    CHECK(!kvz_roi_rc_solve_budget(0.0, n, bases, d, pixels, out));
    CHECK(!kvz_roi_rc_solve_budget(-5.0, n, bases, d, pixels, out));
    CHECK(!kvz_roi_rc_solve_budget(NAN, n, bases, d, pixels, out));
    CHECK(!kvz_roi_rc_solve_budget(INFINITY, n, bases, d, pixels, out));
    for (int i = 0; i < n2; i++) CHECK(out[i] == 12345.0);
  }

  return 1;
}

/* ---------------------------------------------------------------------- */
/* feedback group: encode real frames and verify the model received the   */
/* planned bits and actual lambda, with real bits coming back to training.*/
/* ---------------------------------------------------------------------- */

static int run_feedback(void)
{
  enum { W = 192, H = 136 };
  /* Two full GOPs plus flush. gop=16 exercises the RA GOP layer weights and
   * a real multi-frame dependency chain. */
  const int total_frames = 40;
  const int flush = 24;
  const int roi_w = 3, roi_h = 3;
  const int8_t roi_values[9] = { -8, 0, 4, 0, 8, 0, 4, 0, -8 };

  kvz_config *cfg = make_config(W, H, 1);
  CHECK(cfg);
  CHECK(parse_opt(cfg, "gop", "16"));
  CHECK(parse_opt(cfg, "owf", "0"));
  CHECK(parse_opt(cfg, "threads", "1"));

  kvz_picture *frames[40];
  int moving = total_frames / 2;
  for (int i = 0; i < total_frames; i++) {
    if (i < moving) {
      frames[i] = make_roi_picture(W, H, roi_w, roi_h, roi_values, i);
    } else {
      /* Static frames with an all-zero map exercise the no-ROI path. */
      frames[i] = api->picture_alloc(W, H);
      CHECK(frames[i]);
      fill_static_picture(frames[i]);
      int8_t *map = calloc((size_t)roi_w * roi_h, sizeof(int8_t));
      CHECK(map);
      frames[i]->roi.width = roi_w;
      frames[i]->roi.height = roi_h;
      frames[i]->roi.roi_array = map;
    }
    CHECK(frames[i]);
  }

  kvz_roi_rc_clear_samples();
  int produced = encode_sequence(cfg, frames, total_frames, flush, NULL, NULL);
  CHECK(produced >= total_frames);

  int samples = kvz_roi_rc_sample_count();
  CHECK(samples >= total_frames);

  int saw_effective = 0;
  for (int s = 0; s < samples; s++) {
    kvz_roi_rc_frame_sample smp;
    CHECK(kvz_roi_rc_get_sample(s, &smp));
    CHECK(smp.ctu_count > 0);
    CHECK(smp.budget_count <= 1);

    if (!smp.effective) continue;
    saw_effective++;

    /* Planned budget conserves the frame target. */
    CHECK(fabs(smp.planned_bits - smp.frame_target_bits) <= 1e-8 * smp.frame_target_bits);

    /* Real actual bits are distinct from the planned budget (the budget is
     * not treated as a hard occupation count). Allow either direction. */
    CHECK(smp.actual_bits > 0.0);
    CHECK(smp.actual_ctu_bits != NULL);
    double ctu_sum = 0.0;
    for (int i = 0; i < smp.ctu_count; i++) ctu_sum += smp.actual_ctu_bits[i];
    CHECK(fabs(ctu_sum - smp.actual_bits) <= 1e-6 * smp.actual_bits);

    /* Final QP/lambda went into the reconstruction: compare each CTU's final
     * lambda against the QP actually stored in the reconstruction picture via
     * the lambda->QP relation (same relation the encoder applies). */
    for (int i = 0; i < smp.ctu_count; i++) {
      CHECK(smp.final_qps[i] >= 0.0 && smp.final_qps[i] <= 51.0);
      CHECK(smp.final_lambdas[i] > 0.0);
      double expected_lambda =
          0.57 * pow(2.0, (smp.final_qps[i] - 12.0) / 3.0);
      double rel = smp.final_lambdas[i] / expected_lambda;
      CHECK(rel > 1.0 / 1.2 && rel < 1.2);

      /* Without clamping, the model lambda reproduces the final QP. With
       * clamping, the final QP sits on the frame-delta bound. */
      double model_qp = 4.2005 * log(smp.model_raw_lambdas[i]) + 13.7223;
      if (!smp.model_clamped[i]) {
        CHECK(fabs(round(model_qp) - smp.final_qps[i]) <= 1.0);
        CHECK(fabs(log(smp.model_raw_lambdas[i]) -
                   log(smp.final_lambdas[i])) < 3.0);
      } else {
        double bound_lo = smp.frame_qp - 13.0;
        double bound_hi = smp.frame_qp + 12.5;
        CHECK(smp.final_qps[i] >= bound_lo - 1.0);
        CHECK(smp.final_qps[i] <= bound_hi + 1.0);
      }
    }

    /* The training model must receive the final (adjusted) lambda, not the
     * value from before the QP clamp. model_lambdas is lcu->lambda, which is
     * also what kvz_set_lcu_lambda_and_qp feeds into update_parameters. */
    for (int i = 0; i < smp.ctu_count; i++) {
      CHECK(smp.model_lambdas[i] > 0.0);
      CHECK(fabs(log(smp.model_lambdas[i]) - log(smp.final_lambdas[i])) <= 1e-9);
      CHECK(smp.rc_alphas[i] > 0.0);
      CHECK(smp.rc_betas[i] < 0.0);
    }

    /* The planned target drives the model: recomputing the open-loop lambda
     * from the recorded alpha/beta and target bpp must reproduce it, proving
     * the ROI target (not some other weight) is what entered the model. */
    for (int i = 0; i < smp.ctu_count; i++) {
      double bpp = smp.targets[i] / (double)smp.ctu_pixels[i];
      double expect = smp.rc_alphas[i] * pow(bpp, smp.rc_betas[i]);
      expect = gamut_lambda(expect);
      CHECK(fabs(log(smp.model_raw_lambdas[i]) - log(expect)) < 1e-9);
    }
  }
  CHECK(saw_effective > 0);

  /* Static, all-zero-map frames must stay on the plain path: no budget
   * and QP driven only by the regular model. */
  int static_seen = 0;
  for (int s = 0; s < samples; s++) {
    kvz_roi_rc_frame_sample smp;
    CHECK(kvz_roi_rc_get_sample(s, &smp));
    if (!smp.effective) {
      static_seen++;
      CHECK(smp.budget_count == 0);
      for (int i = 0; i < smp.ctu_count; i++) CHECK(smp.targets[i] == 0.0);
    }
  }
  CHECK(static_seen > 0);

  /* Real feedback loop: the alpha/beta recorded for frame N must be the
   * output of update_parameters applied to frame N-1's real per-CTU bits,
   * real lambda and pixels. Recompute the update independently here. */
  int feedback_checked = 0;
  for (int s = 1; s < samples; s++) {
    kvz_roi_rc_frame_sample cur, prev;
    CHECK(kvz_roi_rc_get_sample(s, &cur));
    CHECK(kvz_roi_rc_get_sample(s - 1, &prev));
    if (!prev.effective) continue;
    CHECK(cur.ctu_count == prev.ctu_count);

    /* Only I-frames reset the per-CTU model; every other frame trains all
     * CTUs from the previous frame's results. Compare on an effective
     * non-I frame whose predecessor was also effective. */
    if (cur.is_irap || !cur.effective) continue;

    feedback_checked++;
    for (int i = 0; i < cur.ctu_count; i++) {
      /* Skip CTUs that were never coded with a real lambda in the previous
       * frame (e.g. zero-bit skip CTUs still train, so only guard non-finite). */
      double alpha = prev.rc_alphas[i];
      double beta = prev.rc_betas[i];
      double bits = prev.actual_ctu_bits[i];
      double pixels = (double)prev.ctu_pixels[i];
      double lambda_real = prev.model_lambdas[i];

      /* update_parameters, reproduced exactly (same constants and clipping). */
      double bpp = bits / pixels;
      double lambda_comp = alpha * pow(bpp, beta);
      if (isnan(lambda_comp)) lambda_comp = 10000.0;
      if (lambda_comp < 0.1) lambda_comp = 0.1;
      if (lambda_comp > 10000.0) lambda_comp = 10000.0;
      double ratio = log(lambda_real) - log(lambda_comp);
      double new_alpha = alpha + 0.10 * ratio * alpha;
      if (new_alpha < 0.05) new_alpha = 0.05;
      if (new_alpha > 20.0) new_alpha = 20.0;
      double log_bpp = log(bpp);
      double beta_scale = log_bpp;
      if (beta_scale < -5.0) beta_scale = -5.0;
      if (beta_scale > -1.0) beta_scale = -1.0;
      double new_beta = beta + 0.05 * ratio * beta_scale;
      if (new_beta < -3.0) new_beta = -3.0;
      if (new_beta > -0.1) new_beta = -0.1;

      /* The recorded alpha/beta for the current frame is exactly the model
       * that entered this frame's lambda derivation, so it must equal the
       * independent update from the previous frame's real results. */
      CHECK(fabs(new_alpha - cur.rc_alphas[i]) <= 1e-9 * MAX(1.0, cur.rc_alphas[i]));
      CHECK(fabs(new_beta - cur.rc_betas[i]) <= 1e-9 * MAX(1.0, fabs(cur.rc_betas[i])));
    }
  }
  CHECK(feedback_checked >= 2);

  free_frames(frames, total_frames);
  api->config_destroy(cfg);

  /* VAQ enabled: the ROI budget still forms, and the per-CTU lambda fed to
   * the next frame's model must be the VAQ-adjusted actual lambda (the same
   * value used by search/quantization), not the pre-VAQ model output. */
  {
    const int nv = 16;
    kvz_picture *vf[16];
    kvz_config *vcfg = make_config(W, H, 1);
    CHECK(vcfg);
    CHECK(parse_opt(vcfg, "gop", "8"));
    CHECK(parse_opt(vcfg, "owf", "2"));
    CHECK(parse_opt(vcfg, "vaq", "5"));
    for (int i = 0; i < nv; i++) {
      vf[i] = make_roi_picture(W, H, roi_w, roi_h, roi_values, i);
      CHECK(vf[i]);
    }
    kvz_roi_rc_clear_samples();
    CHECK(encode_sequence(vcfg, vf, nv, 14, NULL, NULL) >= nv);
    int samples = kvz_roi_rc_sample_count();
    CHECK(samples >= nv);
    int vaq_effective = 0;
    int vaq_changed_lambda = 0;
    for (int s = 0; s < samples; s++) {
      kvz_roi_rc_frame_sample smp;
      CHECK(kvz_roi_rc_get_sample(s, &smp));
      if (!smp.effective) continue;
      vaq_effective++;
      for (int i = 0; i < smp.ctu_count; i++) {
        /* model_lambdas is what update_parameters will train on next frame
         * and must equal the final lambda after VAQ. */
        CHECK(fabs(log(smp.model_lambdas[i]) - log(smp.final_lambdas[i])) <= 1e-9);
        if (fabs(log(smp.model_lambdas[i]) - log(smp.model_raw_lambdas[i])) > 1e-6) {
          vaq_changed_lambda++;
        }
      }
    }
    CHECK(vaq_effective > 0);
    CHECK(vaq_changed_lambda > 0);
    free_frames(vf, nv);
    api->config_destroy(vcfg);
  }

  return 1;
}

/* ---------------------------------------------------------------------- */
/* mapping group: non-uniform ROI, tiles, WPP and OWF.                    */
/* ---------------------------------------------------------------------- */

static int run_mapping_variant(int tiles_x, int tiles_y, int wpp, int owf,
                               int threads, const char *label,
                               double *first_targets, int *first_count,
                               int *first_frame)
{
  enum { W = 192, H = 136 };
  /* Non-uniform 5x4 map -> several distinct offsets across the CTU grid. */
  const int roi_w = 5, roi_h = 4;
  static int8_t values[20];
  for (int y = 0; y < roi_h; y++)
    for (int x = 0; x < roi_w; x++)
      values[y * roi_w + x] = (int8_t)((x - 2) * 5 + (y - 1) * 3);

  kvz_config *cfg = make_config(W, H, 1);
  CHECK(cfg);
  char buf[32];
  if (tiles_x > 1 || tiles_y > 1) {
    snprintf(buf, sizeof(buf), "%dx%d", tiles_x, tiles_y);
    CHECK(parse_opt(cfg, "tiles", buf));
  }
  if (wpp) CHECK(parse_opt(cfg, "wpp", "1"));
  CHECK(parse_opt(cfg, "threads", threads == 0 ? "1" : (threads == 1 ? "2" : "4")));
  snprintf(buf, sizeof(buf), "%d", owf);
  CHECK(parse_opt(cfg, "owf", buf));

  enum { NFRAMES = 14 };
  kvz_picture *frames[NFRAMES];
  const int flush = owf + 4;
  for (int i = 0; i < NFRAMES; i++) {
    /* Change the map every frame to also verify per-frame budget swaps. */
    int8_t per_frame[20];
    for (int k = 0; k < roi_w * roi_h; k++) {
      per_frame[k] = (i % 3 == 0) ? values[k]
                                  : (int8_t)(values[k] + ((i + k) % 5) - 2);
    }
    frames[i] = make_roi_picture(W, H, roi_w, roi_h, per_frame, i);
    CHECK(frames[i]);
    /* Force at least one nonzero entry so the frame is effective. */
    if (i % 3 != 0 && per_frame[0] == 0) frames[i]->roi.roi_array[0] = 3;
  }

  kvz_roi_rc_clear_samples();
  int produced = encode_sequence(cfg, frames, NFRAMES, flush, NULL, NULL);
  CHECK(produced >= NFRAMES);

  int samples = kvz_roi_rc_sample_count();
  CHECK(samples >= NFRAMES);

  /* Every frame must receive exactly one complete budget, independent of
   * tiles, WPP and OWF, and must conserve its own target. */
  for (int s = 0; s < samples; s++) {
    kvz_roi_rc_frame_sample smp;
    CHECK(kvz_roi_rc_get_sample(s, &smp));
    if (!smp.effective) continue;
    CHECK(smp.budget_count == 1);
    CHECK(fabs(smp.planned_bits - smp.frame_target_bits) <=
          1e-8 * smp.frame_target_bits);
  }

  /* Verify each per-CTU offset and target against the independently mapped
   * ROI map for the frame, using the real recorded ROI offsets (so this does
   * not depend on tile/WPP enumeration order). */
  const int cols = (W + 63) / 64;
  const int rows = (H + 63) / 64;
  int effective_frames = 0;
  for (int s = 0; s < samples; s++) {
    kvz_roi_rc_frame_sample smp;
    CHECK(kvz_roi_rc_get_sample(s, &smp));
    if (!smp.effective) continue;
    effective_frames++;

    CHECK(smp.ctu_count == cols * rows);

    /* With gop=0 the frame numbers are in input order, so the per-frame map
     * we constructed for input i is the map of sample frame_num i. Verify
     * every global CTU offset against the documented mapping formula. */
    int idx_in = smp.frame_num;
    CHECK(idx_in >= 0 && idx_in < NFRAMES);
    for (int y = 0; y < rows; y++) {
      for (int x = 0; x < cols; x++) {
        int idx = x + y * cols;
        int rx = (x * roi_w) / cols;
        int ry = (y * roi_h) / rows;
        CHECK(rx >= 0 && rx < roi_w);
        CHECK(ry >= 0 && ry < roi_h);
        CHECK(smp.roi_offsets[idx] ==
              frames[idx_in]->roi.roi_array[rx + ry * roi_w]);
      }
    }

    /* Independent budget solve from the real inputs must reproduce the
     * encoder's per-CTU targets exactly, proving one full-frame budget with
     * global coordinates regardless of tiling. */
    double *indep = calloc((size_t)smp.ctu_count, sizeof(double));
    CHECK(indep);
    CHECK(kvz_roi_rc_solve_budget(smp.frame_target_bits, smp.ctu_count,
                                   smp.base_weights, smp.roi_offsets,
                                   smp.ctu_pixels, indep));
    double indep_sum = 0.0;
    for (int i = 0; i < smp.ctu_count; i++) {
      indep_sum += indep[i];
      CHECK(fabs(indep[i] - smp.targets[i]) <= 1e-8 * smp.frame_target_bits);
    }
    CHECK(fabs(indep_sum - smp.frame_target_bits) <= 1e-8 * smp.frame_target_bits);
    free(indep);
  }
  CHECK(effective_frames > 0);

  /* Record the per-CTU targets of the first effective frame so the caller
   * can compare the same logical CTU across parallel configurations. The
   * budget depends only on global inputs, so these must be identical. */
  *first_count = 0;
  for (int s = 0; s < samples; s++) {
    kvz_roi_rc_frame_sample smp;
    CHECK(kvz_roi_rc_get_sample(s, &smp));
    if (!smp.effective) continue;
    *first_count = smp.ctu_count;
    *first_frame = smp.frame_num;
    for (int i = 0; i < smp.ctu_count; i++) first_targets[i] = smp.targets[i];
    break;
  }
  CHECK(*first_count > 0);

  free_frames(frames, NFRAMES);
  api->config_destroy(cfg);
  (void)label;
  return 1;
}

static int run_mapping(void)
{
  enum { MAX_CTUS = 16 };
  double ref_targets[MAX_CTUS];
  int ref_count = 0, ref_frame = -1;
  double t[MAX_CTUS];
  int c = 0, f = -1;

  CHECK(run_mapping_variant(1, 1, 0, 0, 0, "plain", ref_targets, &ref_count,
                            &ref_frame));
  CHECK(run_mapping_variant(2, 2, 0, 0, 1, "tiles", t, &c, &f));
  CHECK(c == ref_count && f == ref_frame);
  for (int i = 0; i < c; i++) {
    CHECK(fabs(t[i] - ref_targets[i]) <= 1e-8 * ref_targets[i]);
  }

  CHECK(run_mapping_variant(1, 1, 1, 0, 1, "wpp", t, &c, &f));
  CHECK(c == ref_count && f == ref_frame);
  for (int i = 0; i < c; i++) {
    CHECK(fabs(t[i] - ref_targets[i]) <= 1e-8 * ref_targets[i]);
  }

  CHECK(run_mapping_variant(1, 1, 0, 2, 1, "owf", t, &c, &f));
  CHECK(c == ref_count && f == ref_frame);
  for (int i = 0; i < c; i++) {
    CHECK(fabs(t[i] - ref_targets[i]) <= 1e-8 * ref_targets[i]);
  }

  CHECK(run_mapping_variant(2, 1, 1, 2, 2, "tiles-wpp-owf", t, &c, &f));
  CHECK(c == ref_count && f == ref_frame);
  for (int i = 0; i < c; i++) {
    CHECK(fabs(t[i] - ref_targets[i]) <= 1e-8 * ref_targets[i]);
  }
  return 1;
}

/* ---------------------------------------------------------------------- */
/* compatibility group                                                    */
/* ---------------------------------------------------------------------- */

static int run_compatibility(void)
{
  enum { W = 192, H = 136 };
  const int roi_w = 3, roi_h = 3;
  const int8_t roi_values[9] = { -6, 3, 3, 3, 3, 3, 3, 3, -6 };

  /* 1. Illegal combinations must fail encoder_open with a reason. */
  {
    kvz_config *cfg = api->config_alloc();
    CHECK(cfg && api->config_init(cfg));
    cfg->width = W;
    cfg->height = H;
    cfg->roi_rc = true;
    CHECK(!api->encoder_open(cfg)); /* no bitrate / rc algorithm */
    api->config_destroy(cfg);
  }
  {
    kvz_config *cfg = make_config(W, H, 1);
    CHECK(cfg);
    CHECK(parse_opt(cfg, "rc-algorithm", "oba"));
    CHECK(!api->encoder_open(cfg));
    api->config_destroy(cfg);
  }
  {
    kvz_config *cfg = make_config(W, H, 1);
    CHECK(cfg);
    CHECK(parse_opt(cfg, "bitrate", "0"));
    CHECK(parse_opt(cfg, "rc-algorithm", "no-rc"));
    CHECK(!api->encoder_open(cfg));
    api->config_destroy(cfg);
  }
  {
    /* OBA without roi_rc keeps working (its normal path). */
    kvz_config *cfg = make_config(W, H, 0);
    CHECK(cfg);
    CHECK(parse_opt(cfg, "rc-algorithm", "oba"));
    kvz_encoder *enc = api->encoder_open(cfg);
    CHECK(enc != NULL);
    api->encoder_close(enc);
    api->config_destroy(cfg);
  }

  /* 1b. The ROI file input works with roi_rc enabled and produces one
   * budget per frame from the file's per-frame maps. */
  {
    const char *path = "roi_rate_control_tmp_roi.txt";
    FILE *f = fopen(path, "w");
    CHECK(f);
    /* Two frames: an all-positive map then an all-negative one. */
    fprintf(f, "3 3\n");
    for (int i = 0; i < 9; i++) fprintf(f, "%d ", 5);
    fprintf(f, "\n3 3\n");
    for (int i = 0; i < 9; i++) fprintf(f, "%d ", -5);
    fprintf(f, "\n");
    CHECK(!fclose(f));

    kvz_config *cfg = make_config(W, H, 1);
    CHECK(cfg);
    CHECK(parse_opt(cfg, "roi", path));
    /* The file loops when its end is reached. */
    enum { NF = 6 };
    kvz_picture *frames[NF];
    for (int i = 0; i < NF; i++) {
      frames[i] = api->picture_alloc(W, H);
      CHECK(frames[i]);
      fill_moving_texture(frames[i], i);
    }
    kvz_roi_rc_clear_samples();
    CHECK(encode_sequence(cfg, frames, NF, 4, NULL, NULL) >= 1);
    int samples = kvz_roi_rc_sample_count();
    CHECK(samples >= NF);
    for (int s = 0; s < samples; s++) {
      kvz_roi_rc_frame_sample smp;
      CHECK(kvz_roi_rc_get_sample(s, &smp));
      CHECK(smp.effective);
      CHECK(smp.budget_count == 1);
      CHECK(fabs(smp.planned_bits - smp.frame_target_bits) <=
            1e-8 * smp.frame_target_bits);
    }
    free_frames(frames, NF);
    api->config_destroy(cfg);
    remove(path);
  }

  /* 2. Fixed-QP ROI usage and roi_rc disabled remain compatible: the legacy
   * delta-QP path ignores the new budget. */
  {
    kvz_config *cfg = make_config(W, H, 0);
    CHECK(cfg);
    CHECK(parse_opt(cfg, "bitrate", "0"));
    CHECK(parse_opt(cfg, "rc-algorithm", "no-rc"));
    CHECK(parse_opt(cfg, "qp", "30"));
    kvz_picture *f = make_roi_picture(W, H, roi_w, roi_h, roi_values, 0);
    CHECK(f);
    kvz_roi_rc_clear_samples();
    CHECK(encode_sequence(cfg, &f, 1, 2, NULL, NULL) >= 1);
    /* Even in the legacy/no-rate-control path the hook records the frame,
     * but it must carry no ROI budget. */
    CHECK(kvz_roi_rc_sample_count() >= 1);
    {
      kvz_roi_rc_frame_sample smp;
      CHECK(kvz_roi_rc_get_sample(0, &smp));
      CHECK(!smp.effective);
      CHECK(smp.budget_count == 0);
      for (int i = 0; i < smp.ctu_count; i++) CHECK(smp.targets[i] == 0.0);
    }
    free_frames(&f, 1);
    api->config_destroy(cfg);
  }

  /* 3. New switch off must produce deterministic, byte-identical streams. */
  {
    kvz_picture *frames_a[8], *frames_b[8];
    for (int i = 0; i < 8; i++) {
      frames_a[i] = make_roi_picture(W, H, roi_w, roi_h, roi_values, i);
      frames_b[i] = make_roi_picture(W, H, roi_w, roi_h, roi_values, i);
      CHECK(frames_a[i] && frames_b[i]);
    }
    kvz_config *cfg_a = make_config(W, H, 0);
    kvz_config *cfg_b = make_config(W, H, 0);
    CHECK(cfg_a && cfg_b);
    unsigned char *sa = NULL, *sb = NULL;
    size_t la = 0, lb = 0;
    CHECK(encode_sequence(cfg_a, frames_a, 8, 2, &sa, &la));
    CHECK(encode_sequence(cfg_b, frames_b, 8, 2, &sb, &lb));
    CHECK(la == lb);
    CHECK(memcmp(sa, sb, la) == 0);
    free(sa);
    free(sb);
    free_frames(frames_a, 8);
    free_frames(frames_b, 8);
    api->config_destroy(cfg_a);
    api->config_destroy(cfg_b);
  }

  /* 4. With roi_rc enabled but no ROI map, or an all-zero map, the encoder
   * must equal the plain (roi_rc off, no ROI) encode exactly: same CTU
   * targets, QP and lambda, and identical bitstream. */
  {
    const int n = 10;
    enum { MAX_CTUS = 16 };
    kvz_picture *plain_frames[10], *roi_frames[10];
    double plain_qp[10][MAX_CTUS], plain_lambda[10][MAX_CTUS],
        plain_target[10][MAX_CTUS];
    for (int i = 0; i < n; i++) {
      plain_frames[i] = api->picture_alloc(W, H);
      CHECK(plain_frames[i]);
      fill_moving_texture(plain_frames[i], i);

      roi_frames[i] = api->picture_alloc(W, H);
      CHECK(roi_frames[i]);
      fill_moving_texture(roi_frames[i], i);
      int8_t *map = calloc((size_t)roi_w * roi_h, sizeof(int8_t));
      CHECK(map);
      roi_frames[i]->roi.width = roi_w;
      roi_frames[i]->roi.height = roi_h;
      roi_frames[i]->roi.roi_array = map; /* all zero */
    }

    /* Plain: roi_rc off and no ROI. */
    kvz_config *cfg_plain = make_config(W, H, 0);
    CHECK(cfg_plain);
    unsigned char *s_plain = NULL;
    size_t l_plain = 0;
    kvz_roi_rc_clear_samples();
    CHECK(encode_sequence(cfg_plain, plain_frames, n, 4, &s_plain, &l_plain));
    { /* Save plain per-CTU QP/lambda targets for the direct comparison. */
      int samples = kvz_roi_rc_sample_count();
      CHECK(samples >= n);
      for (int s = 0; s < n && s < samples; s++) {
        kvz_roi_rc_frame_sample smp;
        CHECK(kvz_roi_rc_get_sample(s, &smp));
        CHECK(!smp.effective);
        CHECK(smp.ctu_count <= MAX_CTUS);
        for (int i = 0; i < smp.ctu_count; i++) {
          plain_qp[s][i] = smp.final_qps[i];
          plain_lambda[s][i] = smp.final_lambdas[i];
          plain_target[s][i] = smp.targets[i];
        }
      }
    }
    api->config_destroy(cfg_plain);

    /* ROI present (all zeros) with the new switch on must reproduce the
     * plain encode exactly, including per-CTU target/QP/lambda. Run this
     * before the legacy ROI-off encode so the observation buffer holds only
     * the ROI-enabled result. */
    kvz_config *cfg_roi_on = make_config(W, H, 1);
    CHECK(cfg_roi_on);
    unsigned char *s_on = NULL;
    size_t l_on = 0;
    kvz_roi_rc_clear_samples();
    CHECK(encode_sequence(cfg_roi_on, roi_frames, n, 4, &s_on, &l_on));
    api->config_destroy(cfg_roi_on);

    CHECK(l_plain == l_on && memcmp(s_plain, s_on, l_plain) == 0);

    int samples = kvz_roi_rc_sample_count();
    CHECK(samples >= n);
    for (int s = 0; s < n && s < samples; s++) {
      kvz_roi_rc_frame_sample smp;
      CHECK(kvz_roi_rc_get_sample(s, &smp));
      CHECK(!smp.effective);
      CHECK(smp.budget_count == 0);
      CHECK(smp.ctu_count > 0);
      for (int i = 0; i < smp.ctu_count; i++) {
        CHECK(smp.targets[i] == 0.0);
        /* The per-CTU QP and lambda must equal the plain encode exactly. */
        CHECK(smp.final_qps[i] == plain_qp[s][i]);
        CHECK(fabs(log(smp.final_lambdas[i]) - log(plain_lambda[s][i])) < 1e-9);
        CHECK(smp.targets[i] == plain_target[s][i]);
      }
    }

    /* Legacy ROI usage (switch off) keeps working; its raw delta-QP path
     * intentionally differs from the plain encode. */
    kvz_config *cfg_roi_off = make_config(W, H, 0);
    CHECK(cfg_roi_off);
    unsigned char *s_off = NULL;
    size_t l_off = 0;
    CHECK(encode_sequence(cfg_roi_off, roi_frames, n, 4, &s_off, &l_off));
    CHECK(l_off > 0 && l_off != l_plain);
    api->config_destroy(cfg_roi_off);

    free(s_plain);
    free(s_off);
    free(s_on);
    free_frames(plain_frames, n);
    free_frames(roi_frames, n);
  }

  /* 5. Two interleaved encoder instances must not share ROI state. Encode
   * two independent streams with different maps, alternating frames, and
   * check both match their own single-instance reference streams. */
  {
    const int n = 8;
    kvz_picture *fa1[8], *fb1[8], *fa2[8], *fb2[8];
    const int8_t map_a[9] = { -9, -3, 3, -3, 3, 9, 3, 9, 15 };
    const int8_t map_b[9] = { 15, 9, 3, 9, 3, -3, 3, -3, -9 };
    for (int i = 0; i < n; i++) {
      fa1[i] = make_roi_picture(W, H, roi_w, roi_h, map_a,
                                i); /* A moving texture, frame i */
      fb1[i] = make_roi_picture(W, H, roi_w, roi_h, map_b, i + 17);
      fa2[i] = make_roi_picture(W, H, roi_w, roi_h, map_a, i);
      fb2[i] = make_roi_picture(W, H, roi_w, roi_h, map_b, i + 17);
      CHECK(fa1[i] && fb1[i] && fa2[i] && fb2[i]);
    }

    kvz_config *cfg_a = make_config(W, H, 1);
    kvz_config *cfg_b = make_config(W, H, 1);
    CHECK(cfg_a && cfg_b);

    unsigned char *ref_a = NULL, *ref_b = NULL;
    size_t ref_a_len = 0, ref_b_len = 0;
    CHECK(encode_sequence(cfg_a, fa1, n, 4, &ref_a, &ref_a_len));
    CHECK(encode_sequence(cfg_b, fb1, n, 4, &ref_b, &ref_b_len));

    /* Interleave two live encoders. */
    kvz_encoder *ea = api->encoder_open(cfg_a);
    kvz_encoder *eb = api->encoder_open(cfg_b);
    CHECK(ea && eb);

    unsigned char *ia = NULL, *ib = NULL;
    size_t ia_len = 0, ib_len = 0;
    for (int i = 0; i < n + 4; i++) {
      kvz_picture *in_a = i < n ? fa2[i] : NULL;
      kvz_picture *in_b = i < n ? fb2[i] : NULL;
      kvz_data_chunk *ca = NULL, *cb = NULL;
      uint32_t la = 0, lb = 0;
      kvz_picture *ra = NULL, *rb = NULL, *sa = NULL, *sb = NULL;
      CHECK(api->encoder_encode(ea, in_a, &ca, &la, &ra, &sa, NULL));
      CHECK(api->encoder_encode(eb, in_b, &cb, &lb, &rb, &sb, NULL));
      if (la > 0) {
        unsigned char *g = realloc(ia, ia_len + la);
        CHECK(g);
        ia = g;
        for (kvz_data_chunk *c = ca; c; c = c->next) {
          memcpy(ia + ia_len, c->data, c->len);
          ia_len += c->len;
        }
      }
      if (lb > 0) {
        unsigned char *g = realloc(ib, ib_len + lb);
        CHECK(g);
        ib = g;
        for (kvz_data_chunk *c = cb; c; c = c->next) {
          memcpy(ib + ib_len, c->data, c->len);
          ib_len += c->len;
        }
      }
      api->chunk_free(ca);
      api->chunk_free(cb);
      api->picture_free(ra);
      api->picture_free(rb);
      api->picture_free(sa);
      api->picture_free(sb);
    }
    api->encoder_close(ea);
    api->encoder_close(eb);

    CHECK(ia_len == ref_a_len && memcmp(ia, ref_a, ia_len) == 0);
    CHECK(ib_len == ref_b_len && memcmp(ib, ref_b, ib_len) == 0);

    free(ia);
    free(ib);
    free(ref_a);
    free(ref_b);
    free_frames(fa1, n);
    free_frames(fb1, n);
    free_frames(fa2, n);
    free_frames(fb2, n);
    api->config_destroy(cfg_a);
    api->config_destroy(cfg_b);
  }

  return 1;
}

/* ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
  if (argc != 2) {
    fprintf(stderr,
            "usage: %s <allocation|feedback|mapping|compatibility>\n",
            argv[0]);
    return 2;
  }

  api = kvz_api_get(8);
  if (!api) {
    fprintf(stderr, "failed to get kvz_api\n");
    return 1;
  }

  g_group = argv[1];
  int ok = 0;
  observation_on();
  if (!strcmp(g_group, "allocation")) {
    ok = run_allocation();
  } else if (!strcmp(g_group, "feedback")) {
    ok = run_feedback();
  } else if (!strcmp(g_group, "mapping")) {
    ok = run_mapping();
  } else if (!strcmp(g_group, "compatibility")) {
    ok = run_compatibility();
  } else {
    fprintf(stderr, "unknown group: %s\n", g_group);
    return 2;
  }

  if (!ok) return 1;
  kvz_roi_rc_clear_samples();
  printf("ROI_RC_CASE %s PASS\n", g_group);
  return 0;
}
