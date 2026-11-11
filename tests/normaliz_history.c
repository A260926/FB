/*
 * normaliz_history.c -- temporal history regression tests for normaliz0r.
 *
 * The plugin is driven exclusively through its public f0r_* ABI.  Expected
 * pixels come from tests/normaliz_ref.c (an independent extrema-list model)
 * combined with a local, separately written copy of the original color
 * mapping formula.  No production instance field is ever read directly.
 */
#include "frei0r.h"
#include "normaliz_ref.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WIDTH 2
#define HEIGHT 1
#define NPIX (WIDTH * HEIGHT)
#define BYTES (NPIX * 4)

static int failures;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      failures++;                                                        \
    }                                                                    \
  } while (0)

#define TEST_CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define TEST_ROUND(x) ((int)((x) + 0.5f))
#define TEST_MIN(x, y) ((x) < (y) ? (x) : (y))
#define TEST_MAX(x, y) ((x) > (y) ? (x) : (y))

typedef struct
{
  float black[3];   // Output black point per channel, [0,255].
  float white[3];   // Output white point per channel, [0,255].
  int window;       // Smoothing window N, [1,128].
  float independence;
  float strength;
} nz_params_t;

/* Local reimplementation of the plugin's original color mapping, fed by the
 * independent reference's smoothed extrema.  Expression order and float
 * widths mirror the production code so results are byte comparable. */
static void
expected_output(const uint32_t* inframe, uint32_t* outframe,
                const nz_params_t* p,
                const uint8_t cur_min[3], const uint8_t cur_max[3],
                const double smoothed_min[3], const double smoothed_max[3])
{
  float mn[3], mx[3];
  float min_out[3], max_out[3];
  uint8_t lut[3][256];
  int c;

  for (c = 0; c < 3; c++)
  {
    mn[c] = (float)smoothed_min[c];
    mx[c] = (float)smoothed_max[c];
  }

  float rgb_min = mn[0];
  float rgb_max = mx[0];
  rgb_min = TEST_MIN(rgb_min, mn[1]);
  rgb_max = TEST_MAX(rgb_max, mx[1]);
  rgb_min = TEST_MIN(rgb_min, mn[2]);
  rgb_max = TEST_MAX(rgb_max, mx[2]);

  for (c = 0; c < 3; c++)
  {
    mn[c] = mn[c] * p->independence + rgb_min * (1.0f - p->independence);
    mx[c] = mx[c] * p->independence + rgb_max * (1.0f - p->independence);

    min_out[c] = p->black[c] * p->strength
                 + (float)cur_min[c] * (1.0f - p->strength);
    max_out[c] = p->white[c] * p->strength
                 + (float)cur_max[c] * (1.0f - p->strength);

    if (mn[c] == mx[c])
    {
      for (int in_val = cur_min[c]; in_val <= cur_max[c]; in_val++)
        lut[c][in_val] = (uint8_t)min_out[c];
    }
    else
    {
      float scale = (max_out[c] - min_out[c]) / (mx[c] - mn[c]);
      for (int in_val = cur_min[c]; in_val <= cur_max[c]; in_val++)
      {
        int out_val = TEST_ROUND((in_val - mn[c]) * scale + min_out[c]);
        lut[c][in_val] = (uint8_t)TEST_CLAMP(out_val, 0, 255);
      }
    }
  }

  const uint8_t* in = (const uint8_t*)inframe;
  uint8_t* out = (uint8_t*)outframe;
  for (int i = 0; i < NPIX; i++)
  {
    for (c = 0; c < 3; c++)
      out[4 * i + c] = lut[c][in[4 * i + c]];
    out[4 * i + 3] = in[4 * i + 3];  // alpha copied as-is
  }
}

static void
default_params(nz_params_t* p)
{
  p->black[0] = p->black[1] = p->black[2] = 0.0f;
  p->white[0] = p->white[1] = p->white[2] = 255.0f;
  p->window = 1;
  p->independence = 1.0f;
  p->strength = 1.0f;
}

static void
apply_params(f0r_instance_t s, nz_ref_t* ref, const nz_params_t* p)
{
  f0r_param_color_t black = { p->black[0] / 255.0f,
                              p->black[1] / 255.0f,
                              p->black[2] / 255.0f };
  f0r_param_color_t white = { p->white[0] / 255.0f,
                              p->white[1] / 255.0f,
                              p->white[2] / 255.0f };
  double smoothing = (double)(p->window - 1) / (NZ_REF_MAX_HISTORY - 1);
  double independence = p->independence;
  double strength = p->strength;
  f0r_set_param_value(s, &black, 0);
  f0r_set_param_value(s, &white, 1);
  f0r_set_param_value(s, &smoothing, 2);
  f0r_set_param_value(s, &independence, 3);
  f0r_set_param_value(s, &strength, 4);
  nz_ref_set_window(ref, p->window);
}

static void
set_window(f0r_instance_t s, nz_ref_t* ref, nz_params_t* p, int window)
{
  p->window = window;
  double smoothing = (double)(window - 1) / (NZ_REF_MAX_HISTORY - 1);
  f0r_set_param_value(s, &smoothing, 2);
  nz_ref_set_window(ref, window);
}

/* Two gray pixels: (x,x,x,alpha0) and (x+50,x+50,x+50,alpha1). */
static void
gray_frame(uint32_t* frame, int x, uint8_t alpha0, uint8_t alpha1)
{
  uint8_t* b = (uint8_t*)frame;
  b[0] = b[1] = b[2] = (uint8_t)x;
  b[3] = alpha0;
  b[4] = b[5] = b[6] = (uint8_t)(x + 50);
  b[7] = alpha1;
}

static void
color_frame(uint32_t* frame, const uint8_t px[2][4])
{
  memcpy(frame, px, BYTES);
}

/* Drive one update through plugin and reference, comparing every byte. */
static void
check_update(f0r_instance_t s, nz_ref_t* ref, const nz_params_t* p,
             double time, const uint32_t* input, int in_place)
{
  uint32_t expect_in[NPIX];
  uint32_t expect_out[NPIX];
  uint32_t work[NPIX];
  uint8_t cur_min[3], cur_max[3];
  double smoothed_min[3], smoothed_max[3];

  memcpy(expect_in, input, BYTES);
  int standalone = nz_ref_observe(ref, time, expect_in, NPIX,
                                  cur_min, cur_max);
  nz_ref_smoothed(ref, standalone, cur_min, cur_max,
                  smoothed_min, smoothed_max);
  expected_output(expect_in, expect_out, p, cur_min, cur_max,
                  smoothed_min, smoothed_max);

  memcpy(work, input, BYTES);
  if (in_place)
  {
    f0r_update(s, time, work, work);  // in-place input/output overlap
  }
  else
  {
    uint32_t plugin_out[NPIX];
    f0r_update(s, time, work, plugin_out);
    memcpy(work, plugin_out, BYTES);
  }

  if (memcmp(work, expect_out, BYTES) != 0)
  {
    printf("FAIL %s:%d: mismatch at time %g (in_place=%d)\n",
           __FILE__, __LINE__, time, in_place);
    failures++;
  }
}

/* The exact scenario from the pre-fix baseline witness (frames span x..x+100
 * there), with the corrected final pixel assertions: shrinking the window to
 * one frame must give 0/255.  Earlier frames keep their correct averages. */
static void
test_known_defect_scenario(void)
{
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  nz_ref_t ref;
  nz_params_t p;
  uint32_t in[NPIX], out[NPIX];
  CHECK(s != 0);
  nz_ref_init(&ref, 3);
  default_params(&p);
  p.window = 3;
  apply_params(s, &ref, &p);

  /* Baseline-style frames: gray pixels x and x+100 with fixed alphas. */
#define BASELINE_IMAGE(frame, x)                                         \
  do {                                                                   \
    uint8_t* b_ = (uint8_t*)(frame);                                     \
    b_[0] = b_[1] = b_[2] = (uint8_t)(x);                               \
    b_[3] = 37;                                                          \
    b_[4] = b_[5] = b_[6] = (uint8_t)((x) + 100);                       \
    b_[7] = 38;                                                          \
  } while (0)

  BASELINE_IMAGE(in, 10);
  f0r_update(s, 0.0, in, out);
  CHECK(((uint8_t*)out)[0] == 0);
  BASELINE_IMAGE(in, 30);
  f0r_update(s, 1.0, in, out);
  // avg extrema (10+30)/2=20 and (110+130)/2=120: (30-20)/100*255 -> 26
  CHECK(((uint8_t*)out)[0] == 26);
  BASELINE_IMAGE(in, 50);
  f0r_update(s, 2.0, in, out);
  // avg extrema 30 and 130: (50-30)/100*255 -> 51
  CHECK(((uint8_t*)out)[0] == 51);

  set_window(s, &ref, &p, 1);
  BASELINE_IMAGE(in, 70);
  f0r_update(s, 3.0, in, out);
  uint8_t* b = (uint8_t*)out;
  CHECK(b[0] == 0);    // darkest input maps to the black point
  CHECK(b[4] == 255);  // brightest input maps to the white point
  CHECK(b[3] == 37 && b[7] == 38);  // alpha untouched

#undef BASELINE_IMAGE
  f0r_destruct(s);
}

/* Fixed window over a longer stream, then shrink and grow again: growing the
 * window must reuse the still-retained history. */
static void
test_window_resize(int in_place)
{
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  nz_ref_t ref;
  nz_params_t p;
  uint32_t in[NPIX];
  int t;
  CHECK(s != 0);
  nz_ref_init(&ref, 4);
  default_params(&p);
  p.window = 4;
  apply_params(s, &ref, &p);

  for (t = 0; t < 5; t++)
  {
    gray_frame(in, 10 * t, 37, 200);
    check_update(s, &ref, &p, (double)t, in, in_place);
  }

  set_window(s, &ref, &p, 2);  // shrink: immediate effect, history retained
  for (; t < 7; t++)
  {
    gray_frame(in, 10 * t, 37, 200);
    check_update(s, &ref, &p, (double)t, in, in_place);
  }

  set_window(s, &ref, &p, 6);  // grow: reuses entries still retained
  for (; t < 10; t++)
  {
    gray_frame(in, 10 * t, 37, 200);
    check_update(s, &ref, &p, (double)t, in, in_place);
  }

  f0r_destruct(s);
}

/* More than 128 distinct video times: the ring overwrites oldest entries but
 * never grows beyond 128, and every output still matches the reference. */
static void
test_over_128_updates(int in_place)
{
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  nz_ref_t ref;
  nz_params_t p;
  uint32_t in[NPIX];
  CHECK(s != 0);
  nz_ref_init(&ref, NZ_REF_MAX_HISTORY);
  default_params(&p);
  p.window = NZ_REF_MAX_HISTORY;
  apply_params(s, &ref, &p);

  for (int t = 0; t < 160; t++)
  {
    gray_frame(in, (t * 37) % 200, (uint8_t)(t % 256), 38);
    check_update(s, &ref, &p, (double)t, in, in_place);
  }
  f0r_destruct(s);
}

/* Uneven time intervals: every distinct time is one history entry. */
static void
test_uneven_intervals(int in_place)
{
  static const double times[] = { 0.0, 0.3, 0.8, 2.5, 3.0, 3.05 };
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  nz_ref_t ref;
  nz_params_t p;
  uint32_t in[NPIX];
  CHECK(s != 0);
  nz_ref_init(&ref, 3);
  default_params(&p);
  p.window = 3;
  apply_params(s, &ref, &p);

  for (int i = 0; i < (int)(sizeof(times) / sizeof(times[0])); i++)
  {
    gray_frame(in, 20 * i, 37, 38);
    check_update(s, &ref, &p, times[i], in, in_place);
  }
  f0r_destruct(s);
}

/* Same-time redraw replaces the current entry: weighted once, no extra slot;
 * afterwards time advances normally. */
static void
test_same_time_redraw(int in_place)
{
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  nz_ref_t ref;
  nz_params_t p;
  uint32_t in[NPIX];
  CHECK(s != 0);
  nz_ref_init(&ref, 2);
  default_params(&p);
  p.window = 2;
  apply_params(s, &ref, &p);

  gray_frame(in, 0, 37, 38);    // range 0..50
  check_update(s, &ref, &p, 0.0, in, in_place);
  gray_frame(in, 60, 37, 38);   // range 60..110
  check_update(s, &ref, &p, 1.0, in, in_place);
  gray_frame(in, 200, 37, 38);  // redraw of t=1: range 200..250
  check_update(s, &ref, &p, 1.0, in, in_place);

  // Independently verify that the redraw replaced rather than accumulated:
  // with window 2 the retained entries are the t=0 entry and the replaced t=1
  // entry, so smoothed extrema are (0+200)/2 and (50+250)/2.  A buggy double
  // weighting would divide the three raw values by three instead.
  {
    nz_ref_t probe;
    uint8_t cm[3], cM[3];
    double smoothed_min[3], smoothed_max[3];
    nz_ref_init(&probe, 2);
    gray_frame(in, 0, 37, 38);
    (void)nz_ref_observe(&probe, 0.0, in, NPIX, cm, cM);
    gray_frame(in, 60, 37, 38);
    (void)nz_ref_observe(&probe, 1.0, in, NPIX, cm, cM);
    gray_frame(in, 200, 37, 38);
    (void)nz_ref_observe(&probe, 1.0, in, NPIX, cm, cM);
    nz_ref_smoothed(&probe, 0, cm, cM, smoothed_min, smoothed_max);
    CHECK(smoothed_min[0] == 100.0 && smoothed_max[0] == 150.0);
  }

  gray_frame(in, 90, 37, 38);   // advance to t=2
  check_update(s, &ref, &p, 2.0, in, in_place);
  f0r_destruct(s);
}

/* A time smaller than the previous one starts a new playback segment: all
 * earlier frames are discarded. */
static void
test_backwards_time(int in_place)
{
  static const double times[] = { 0.0, 1.0, 2.0, 0.5, 1.0, 1.5 };
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  nz_ref_t ref;
  nz_params_t p;
  uint32_t in[NPIX];
  CHECK(s != 0);
  nz_ref_init(&ref, 4);
  default_params(&p);
  p.window = 4;
  apply_params(s, &ref, &p);

  for (int i = 0; i < (int)(sizeof(times) / sizeof(times[0])); i++)
  {
    gray_frame(in, 15 * i, 37, 38);
    check_update(s, &ref, &p, times[i], in, in_place);
  }
  f0r_destruct(s);
}

/* Non-finite times are standalone single frames and clear the history; the
 * next finite time starts afresh. */
static void
test_non_finite_time(int in_place)
{
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  nz_ref_t ref;
  nz_params_t p;
  uint32_t in[NPIX];
  CHECK(s != 0);
  nz_ref_init(&ref, 3);
  default_params(&p);
  p.window = 3;
  apply_params(s, &ref, &p);

  gray_frame(in, 10, 37, 38);
  check_update(s, &ref, &p, 0.0, in, in_place);
  gray_frame(in, 40, 37, 38);
  check_update(s, &ref, &p, 1.0, in, in_place);

  gray_frame(in, 120, 37, 38);
  check_update(s, &ref, &p, NAN, in, in_place);
  gray_frame(in, 160, 37, 38);
  check_update(s, &ref, &p, INFINITY, in, in_place);

  gray_frame(in, 30, 37, 38);  // finite again: history was empty
  check_update(s, &ref, &p, 5.0, in, in_place);
  f0r_destruct(s);
}

/* Distinct per-channel values with non-default color parameters exercise the
 * linked/independent mix, black/white points, Strength, quantization, clamp
 * and alpha passthrough. */
static void
test_color_formula(int in_place)
{
  static const uint8_t frames[][2][4] = {
    { {  10,  20,  30,  7}, { 40,  60,  80,  9} },
    { { 200, 100,  50, 11}, {220, 140,  90, 13} },
    { {  0,  255,  128, 15}, { 17,  34,  51, 17} },
    { { 90,  180,  70, 19}, {110, 200,  90, 21} },
  };
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  nz_ref_t ref;
  nz_params_t p;
  uint32_t in[NPIX];
  CHECK(s != 0);
  nz_ref_init(&ref, 3);
  default_params(&p);
  p.window = 3;
  p.black[0] = 25.5f;  p.black[1] = 51.0f;  p.black[2] = 76.5f;
  p.white[0] = 229.5f; p.white[1] = 204.0f; p.white[2] = 178.5f;
  p.independence = 0.5f;
  p.strength = 0.7f;
  apply_params(s, &ref, &p);

  for (int i = 0; i < (int)(sizeof(frames) / sizeof(frames[0])); i++)
  {
    color_frame(in, frames[i]);
    check_update(s, &ref, &p, (double)i, in, in_place);
  }
  f0r_destruct(s);
}

/* Two instances with different windows and interleaved, in-place and
 * separate-buffer calls must keep independent histories. */
static void
test_interleaved_instances(void)
{
  f0r_instance_t a = f0r_construct(WIDTH, HEIGHT);
  f0r_instance_t b = f0r_construct(WIDTH, HEIGHT);
  nz_ref_t ra, rb;
  nz_params_t pa, pb;
  uint32_t in_a[NPIX], in_b[NPIX];
  CHECK(a != 0 && b != 0);
  nz_ref_init(&ra, 2);
  nz_ref_init(&rb, 3);
  default_params(&pa);
  default_params(&pb);
  pa.window = 2;
  pb.window = 3;
  apply_params(a, &ra, &pa);
  apply_params(b, &rb, &pb);

  for (int t = 0; t < 12; t++)
  {
    gray_frame(in_a, 10 * t, 37, 38);
    gray_frame(in_b, 200 - 12 * t, 200, 201);
    check_update(a, &ra, &pa, (double)t, in_a, 1);       // in-place
    check_update(b, &rb, &pb, (double)(t * 0.5), in_b, 0); // separate
  }
  f0r_destruct(a);
  f0r_destruct(b);
}

/* Destruction and reconstruction must yield a fresh, empty history. */
static void
test_construct_destruct_reuse(void)
{
  nz_params_t p;
  uint32_t in[NPIX], out[NPIX], expect[NPIX];
  uint8_t cm[3], cM[3];
  double smin[3], smax[3];

  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  nz_ref_t ref;
  CHECK(s != 0);
  nz_ref_init(&ref, 4);
  default_params(&p);
  p.window = 4;
  apply_params(s, &ref, &p);
  for (int t = 0; t < 6; t++)
  {
    gray_frame(in, 30 * t, 37, 38);
    check_update(s, &ref, &p, (double)t, in, 0);
  }
  f0r_destruct(s);

  s = f0r_construct(WIDTH, HEIGHT);
  CHECK(s != 0);
  nz_ref_init(&ref, 4);
  apply_params(s, &ref, &p);
  gray_frame(in, 70, 37, 38);  // fresh instance: single-frame normalization
  f0r_update(s, 0.0, in, out);
  int standalone = nz_ref_observe(&ref, 0.0, in, NPIX, cm, cM);
  nz_ref_smoothed(&ref, standalone, cm, cM, smin, smax);
  expected_output(in, expect, &p, cm, cM, smin, smax);
  CHECK(memcmp(out, expect, BYTES) == 0);
  CHECK(((uint8_t*)out)[0] == 0 && ((uint8_t*)out)[4] == 255);
  f0r_destruct(s);
}

/* Smoothing parameter get/set mapping must remain [0,1] <-> [1,128]. */
static void
test_param_mapping(void)
{
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  double v;
  CHECK(s != 0);
  v = 2.1 / 127.0;
  f0r_set_param_value(s, &v, 2);
  f0r_get_param_value(s, &v, 2);
  CHECK(v == 2.0 / 127.0);  // stored window is 3
  v = 1.0;
  f0r_set_param_value(s, &v, 2);
  f0r_get_param_value(s, &v, 2);
  CHECK(v == 1.0);         // window 128
  f0r_destruct(s);
}

int
main(void)
{
  CHECK(f0r_init());

  test_known_defect_scenario();
  test_window_resize(0);
  test_window_resize(1);
  test_over_128_updates(0);
  test_over_128_updates(1);
  test_uneven_intervals(0);
  test_uneven_intervals(1);
  test_same_time_redraw(0);
  test_same_time_redraw(1);
  test_backwards_time(0);
  test_backwards_time(1);
  test_non_finite_time(0);
  test_non_finite_time(1);
  test_color_formula(0);
  test_color_formula(1);
  test_interleaved_instances();
  test_construct_destruct_reuse();
  test_param_mapping();

  f0r_deinit();
  if (failures == 0)
  {
    puts("normaliz0r temporal history: ok");
    return 0;
  }
  printf("normaliz0r temporal history: %d failure(s)\n", failures);
  return 1;
}
