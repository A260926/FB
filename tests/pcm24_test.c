/* SoX Resampler Library      Copyright (c) 2007-24 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

/* Tests for packed, signed 24-bit PCM datatypes:
 *   SOXR_INT24_LE_I / SOXR_INT24_BE_I (interleaved frames)
 *   SOXR_INT24_LE_S / SOXR_INT24_BE_S (array of per-channel pointers)
 *
 * All pack/unpack and rounding expectations below are written independently
 * of the library's own conversion helpers. */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soxr.h"

#define PI 3.14159265358979323846
#define PCM24_FULL_SCALE 8388608.0          /* Normalised full scale: 2^23. */
#define PCM24_MAX 8388607L
#define PCM24_MIN (-8388607L - 1)

static int cases = 0;
static int failures = 0;

#define CHECK(cond, ...) do { \
  if (!(cond)) { \
    ++failures; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); \
  } \
} while (0)

#define TEST(name) do { ++cases; fprintf(stderr, "--- case %d: %s\n", cases, name); } while (0)



/* ----------------------- Independent 24-bit references ----------------------- */

static void t_pack24(unsigned char * p, int32_t v, int big_endian)
{
  uint32_t u = (uint32_t)v;
  if (big_endian) {
    p[0] = (unsigned char)(u >> 16);
    p[1] = (unsigned char)(u >> 8);
    p[2] = (unsigned char)u;
  } else {
    p[0] = (unsigned char)u;
    p[1] = (unsigned char)(u >> 8);
    p[2] = (unsigned char)(u >> 16);
  }
}

static int32_t t_unpack24(unsigned char const * p, int big_endian)
{
  uint32_t u = big_endian?
      ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2]
    : (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
  if (u & 0x800000u)
    u |= 0xff000000u;                    /* Sign-extend from 24 bits. */
  return (int32_t)u;
}

/* Round half away from zero to 24-bit LSB units, then saturate. */
static int32_t t_round24(double x)
{
  double a, r;
  if (x != x)
    return (int32_t)PCM24_MIN;           /* NaN. */
  a = fabs(x);
  r = floor(a + 0.5);
  /* +8388608 cannot be represented, but -8388608 can. */
  if (r > PCM24_FULL_SCALE || (r == PCM24_FULL_SCALE && x > 0))
    return x > 0? (int32_t)PCM24_MAX : (int32_t)PCM24_MIN;
  return (int32_t)(x < 0? -r : r);
}

/* Whether rounding x would exceed the representable 24-bit range. */
static int t_clip24(double x)
{
  double a, r;
  if (x != x)
    return 1;
  a = fabs(x);
  r = floor(a + 0.5);
  return (x > 0 && r >= PCM24_FULL_SCALE) ||
         (x < 0 && r > PCM24_FULL_SCALE);
}

static int t_big_endian(soxr_datatype_t t)
{
  return t == SOXR_INT24_BE_I || t == SOXR_INT24_BE_S;
}

static int t_is_split(soxr_datatype_t t)
{
  return (t & SOXR_SPLIT) != 0;
}



/* ----------------------------- Guarded buffers ------------------------------- */

#define GUARD 17
static unsigned char const guard_pattern[GUARD] =
    {0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,
     0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C};
static unsigned char const payload_mark = 0xA5;

static unsigned char * guarded_alloc(size_t bytes)
{
  unsigned char * b = (unsigned char *)malloc(GUARD + bytes + GUARD);
  if (!b) { fprintf(stderr, "out of memory\n"); exit(3); }
  memcpy(b, guard_pattern, GUARD);
  memset(b + GUARD, payload_mark, bytes);
  memcpy(b + GUARD + bytes, guard_pattern, GUARD);
  return b;
}

static void check_guards(unsigned char const * b, size_t bytes, char const * what)
{
  CHECK(memcmp(b, guard_pattern, GUARD) == 0, "pre-guard overwritten (%s)", what);
  CHECK(memcmp(b + GUARD + bytes, guard_pattern, GUARD) == 0,
      "post-guard overwritten (%s)", what);
}

static void check_payload_tail(unsigned char const * b, size_t used,
    size_t bytes, char const * what)
{
  size_t k;
  for (k = used; k < bytes; ++k)
    if (b[GUARD + k] != payload_mark) {
      CHECK(0, "payload overrun at +%zu (%s)", k, what);
      break;
    }
}



/* ------------------------------ Signal builders ------------------------------ */

static int32_t sine24(unsigned frame, unsigned channel)
{
  /* Distinct frequency and phase per channel; comfortably below full scale. */
  double f = 120.0 * (channel + 1);
  double phase = 0.37 * channel;
  double v = 0.35 * PCM24_FULL_SCALE *
      sin(2.0 * PI * f * frame / 44100.0 + phase);
  long r = (long)floor(v + 0.5);
  if (r > PCM24_MAX) r = PCM24_MAX;
  if (r < PCM24_MIN) r = PCM24_MIN;
  return (int32_t)r;
}

static size_t packed_bytes(size_t frames, unsigned ch)
{
  return 3 * (size_t)ch * frames;
}

/* Build an interleaved or split packed-24 signal in freshly guarded storage.
 * For split layout, *view holds the per-channel payload pointers. */
static void build_packed(soxr_datatype_t type, unsigned ch, size_t n,
    unsigned char * blocks[6], unsigned char *** view)
{
  int be = t_big_endian(type), split = t_is_split(type);
  size_t f, c;
  if (split) {
    unsigned char ** ptrs = (unsigned char **)malloc((size_t)ch * sizeof(*ptrs));
    if (!ptrs) { fprintf(stderr, "out of memory\n"); exit(3); }
    for (c = 0; c < ch; ++c) {
      unsigned char * b = guarded_alloc(3 * n);
      for (f = 0; f < n; ++f)
        t_pack24(b + GUARD + 3 * f, sine24(f, (unsigned)c), be);
      ptrs[c] = b + GUARD;
      blocks[c] = b;
    }
    *view = ptrs;
  } else {
    unsigned char * b = guarded_alloc(packed_bytes(n, ch));
    for (f = 0; f < n; ++f)
      for (c = 0; c < ch; ++c)
        t_pack24(b + GUARD + 3 * ((size_t)ch * f + c),
            sine24(f, (unsigned)c), be);
    blocks[0] = b;
    *view = 0;
  }
}

static void free_packed(soxr_datatype_t type, unsigned ch,
    unsigned char * blocks[6], unsigned char ** view)
{
  unsigned c;
  if (t_is_split(type)) {
    for (c = 0; c < ch; ++c) free(blocks[c]);
    free(view);
  } else {
    free(blocks[0]);
  }
}

/* Output storage: guarded and marker-filled, but containing no signal, so
 * payload-tail sentinels can detect any overrun. */
static void build_packed_out(soxr_datatype_t type, unsigned ch, size_t n,
    unsigned char * blocks[6], unsigned char *** view)
{
  unsigned c;
  if (t_is_split(type)) {
    unsigned char ** ptrs = (unsigned char **)malloc((size_t)ch * sizeof(*ptrs));
    if (!ptrs) { fprintf(stderr, "out of memory\n"); exit(3); }
    for (c = 0; c < ch; ++c) {
      blocks[c] = guarded_alloc(3 * n);
      ptrs[c] = blocks[c] + GUARD;
    }
    *view = ptrs;
  } else {
    blocks[0] = guarded_alloc(packed_bytes(n, ch));
    *view = 0;
  }
}

static int32_t read_packed(soxr_datatype_t type, unsigned char const * base,
    size_t frame, unsigned channel, unsigned ch)
{
  int be = t_big_endian(type);
  if (t_is_split(type)) {
    unsigned char const * const * ptrs = (unsigned char const * const *)base;
    return t_unpack24(ptrs[channel] + 3 * frame, be);
  }
  return t_unpack24(base + 3 * ((size_t)ch * frame + channel), be);
}



/* ------------------ Enum values, sizes, and type validation ------------------ */

static void test_enums_and_validation(void)
{
  soxr_datatype_t bad[] = {
    (soxr_datatype_t)10, (soxr_datatype_t)11, (soxr_datatype_t)14,
    (soxr_datatype_t)15, (soxr_datatype_t)16, (soxr_datatype_t)255};
  size_t k;

  TEST("enum values, datatype sizes, validation");

  /* Old enum numeric values must not have moved. */
  CHECK(SOXR_FLOAT32_I == 0 && SOXR_FLOAT64_I == 1 &&
      SOXR_INT32_I == 2 && SOXR_INT16_I == 3, "old interleaved enum values");
  CHECK(SOXR_SPLIT == 4 && SOXR_FLOAT32_S == 4 && SOXR_FLOAT64_S == 5 &&
      SOXR_INT32_S == 6 && SOXR_INT16_S == 7, "old split enum values");
  CHECK(SOXR_FLOAT32 == 0 && SOXR_FLOAT64 == 1 && SOXR_INT32 == 2 &&
      SOXR_INT16 == 3, "internal base enum values");

  CHECK(SOXR_INT24_LE_I == 8 && SOXR_INT24_BE_I == 9 &&
      SOXR_INT24_LE_S == 12 && SOXR_INT24_BE_S == 13, "new enum values");
  CHECK((SOXR_INT24_LE_S & SOXR_SPLIT) && (SOXR_INT24_BE_S & SOXR_SPLIT),
      "new split types carry SOXR_SPLIT");

  CHECK(soxr_datatype_size(SOXR_INT24_LE_I) == 3 &&
      soxr_datatype_size(SOXR_INT24_BE_I) == 3 &&
      soxr_datatype_size(SOXR_INT24_LE_S) == 3 &&
      soxr_datatype_size(SOXR_INT24_BE_S) == 3, "pcm24 size is 3");
  CHECK(soxr_datatype_size(SOXR_FLOAT32_I) == 4 &&
      soxr_datatype_size(SOXR_FLOAT64_I) == 8 &&
      soxr_datatype_size(SOXR_INT32_I) == 4 &&
      soxr_datatype_size(SOXR_INT16_I) == 2, "legacy sizes unchanged");

  {
    soxr_datatype_t const good[] = {
      SOXR_INT24_LE_I, SOXR_INT24_BE_I, SOXR_INT24_LE_S, SOXR_INT24_BE_S};
    for (k = 0; k < sizeof(good) / sizeof(good[0]); ++k) {
      soxr_io_spec_t io = soxr_io_spec(good[k], good[k]);
      CHECK(io.e == 0, "valid pcm24 spec %d rejected", (int)good[k]);
    }
  }

  for (k = 0; k < sizeof(bad) / sizeof(bad[0]); ++k) {
    soxr_io_spec_t io = soxr_io_spec(bad[k], SOXR_INT24_LE_I);
    CHECK(io.e != 0, "bad itype %d accepted", (int)bad[k]);
    io = soxr_io_spec(SOXR_INT24_BE_S, bad[k]);
    CHECK(io.e != 0, "bad otype %d accepted", (int)bad[k]);

    {
      soxr_error_t error = 0;
      io = soxr_io_spec(bad[k], SOXR_FLOAT32_I);
      {
        soxr_t r = soxr_create(44100, 48000, 1, &error, &io, 0, 0);
        CHECK(r == 0 && error != 0, "soxr_create accepted bad type %d",
            (int)bad[k]);
        soxr_delete(r);
      }
    }
  }
}



/* ----------------------- Pack/unpack round-trip sanity ----------------------- */

static void test_pack_unpack(void)
{
  static int32_t const vals[] = {
    PCM24_MIN, -8388607, -256, -255, -2, -1, 0, 1, 2, 255, 256,
    0x010203, -0x010204, 0x123456, -0x123457, 0x7ABCDE, 8388606, PCM24_MAX};
  int be;
  size_t k;

  TEST("independent pack/unpack of edge values");
  for (be = 0; be <= 1; ++be) {
    for (k = 0; k < sizeof(vals) / sizeof(vals[0]); ++k) {
      unsigned char b[3];
      t_pack24(b, vals[k], be);
      CHECK(t_unpack24(b, be) == vals[k],
          "round-trip v=%ld be=%d", (long)vals[k], be);
      if (be)
        CHECK(b[0] == (unsigned char)((uint32_t)vals[k] >> 16), "BE byte 0");
      else
        CHECK(b[0] == (unsigned char)vals[k], "LE byte 0");
    }
  }
}



/* --- Unit-proportion conversion: all 24 bits survive, rounding and clips ---- */

static soxr_t make_pcm24(soxr_datatype_t itype, soxr_datatype_t otype,
    unsigned ch, double scale, unsigned long qflags, unsigned recipe,
    soxr_error_t * error)
{
  soxr_io_spec_t io = soxr_io_spec(itype, otype);
  soxr_quality_spec_t q = soxr_quality_spec(recipe, qflags);
  io.scale = scale;
  return soxr_create(48000, 48000, ch, error, &io, &q, 0);
}

static void test_unit_ratio_bits_and_rounding(void)
{
  /* Values exercising both rails, zero, +/-1 and non-zero low octets. */
  static int32_t const vals[] = {
    PCM24_MIN, -8388607, -0x123457, -0x010204, -257, -256, -255, -3, -2, -1,
    0, 1, 2, 3, 255, 256, 257, 0x010203, 0x123456, 0x7ABCDE, 8388606, PCM24_MAX};
  size_t n = sizeof(vals) / sizeof(vals[0]), k;
  size_t out_bytes = packed_bytes(n + 4, 1);
  unsigned char * in_buf = guarded_alloc(packed_bytes(n, 1));
  unsigned char * out = guarded_alloc(out_bytes);
  size_t odone = 0, idone = 0, clips = 0;
  soxr_error_t error;

  TEST("unit-ratio double engine preserves every valid bit (no 16-bit shrink)");
  for (k = 0; k < n; ++k)
    t_pack24(in_buf + GUARD + 3 * k, vals[k], 0);
  {
    soxr_t r = make_pcm24(SOXR_INT24_LE_I, SOXR_INT24_LE_I, 1, 1.0,
        SOXR_DOUBLE_PRECISION, SOXR_HQ, &error);
    CHECK(!error && r, "bit-preserve create: %s", soxr_strerror(error));
    error = soxr_process(r, in_buf + GUARD, n, &idone,
        out + GUARD, out_bytes / 3, &odone);
    CHECK(!error, "bit-preserve process: %s", soxr_strerror(error));
    CHECK(idone == n, "unit-ratio consumed %zu of %zu", idone, n);
    CHECK(odone == n, "odone %zu != %zu", odone, n);
    clips = *soxr_num_clips(r);
    CHECK(clips == 0, "unexpected clips %zu", clips);
    for (k = 0; k < odone; ++k) {
      int32_t got = t_unpack24(out + GUARD + 3 * k, 0);
      CHECK(got == vals[k], "bit-preserve v=%ld got=%ld",
          (long)vals[k], (long)got);
    }
    check_guards(in_buf, packed_bytes(n, 1), "bit-preserve input");
    check_guards(out, out_bytes, "bit-preserve output");
    check_payload_tail(out, 3 * odone, out_bytes, "bit-preserve output tail");
    soxr_delete(r);
  }
  free(in_buf);
  free(out);

  /* Half-integer results round away from zero; rails saturate and clip.
   * A non-unit scale adds a gain stage with filter latency, so each value is
   * fed as a constant stream and the resampler is flushed; every output frame
   * must then equal the independently rounded gain product. */
  {
    struct { int32_t x; double g; } tbl[] = {
      {2, 1.25}, {-2, 1.25}, {1, 0.5}, {-1, 0.5}, {0, 7.0},
      {3, 1.5}, {-3, 1.5}, {257, 1.25}, {-257, 1.25},
      {PCM24_MAX, 1.5}, {PCM24_MIN, 1.5}};
    size_t m = sizeof(tbl) / sizeof(tbl[0]), j;
    size_t const REPS = 40;
    TEST("half-away-from-zero rounding and per-sample saturation");
    for (j = 0; j < m; ++j) {
      unsigned char * ib = guarded_alloc(3 * REPS);
      unsigned char * ob = guarded_alloc(3 * (REPS + 32));
      size_t id = 0, od = 0, total = 0, k, expect_clip;
      int32_t expect = t_round24(tbl[j].g * (double)tbl[j].x);
      soxr_t r;
      for (k = 0; k < REPS; ++k)
        t_pack24(ib + GUARD + 3 * k, tbl[j].x, 0);
      r = make_pcm24(SOXR_INT24_LE_I, SOXR_INT24_BE_I, 1, tbl[j].g,
          SOXR_DOUBLE_PRECISION, SOXR_HQ, &error);
      CHECK(!error && r, "rounding create");
      error = soxr_process(r, ib + GUARD, REPS, &id,
          ob + GUARD, REPS + 32, &od);
      CHECK(!error && id == REPS, "rounding process");
      total = od;
      for (;;) {                     /* Flush the gain stage's tail. */
        size_t fd = 0;
        error = soxr_process(r, 0, 0, 0,
            ob + GUARD + 3 * total, REPS + 32 - total, &fd);
        CHECK(!error, "rounding flush");
        total += fd;
        if (fd == 0)
          break;
      }
      CHECK(total >= REPS, "rounding produced %zu frames", total);
      /* The gain filter ramps at start-up and flush; check the settled middle. */
      for (k = total / 4; k < (3 * total) / 4; ++k) {
        int32_t got = t_unpack24(ob + GUARD + 3 * k, 1);
        CHECK(got == expect, "round x=%ld g=%g k=%zu: got=%ld want=%ld",
            (long)tbl[j].x, tbl[j].g, k, (long)got, (long)expect);
      }
      expect_clip = t_clip24(tbl[j].g * (double)tbl[j].x);
      if (expect_clip)
        CHECK(*soxr_num_clips(r) >= total / 2,
            "clip count x=%ld got=%zu", (long)tbl[j].x, *soxr_num_clips(r));
      else
        CHECK(*soxr_num_clips(r) == 0, "unexpected clip x=%ld",
            (long)tbl[j].x);
      check_guards(ib, 3 * REPS, "rounding input");
      check_guards(ob, 3 * (REPS + 32), "rounding output");
      soxr_delete(r);
      free(ib);
      free(ob);
    }
  }

  /* Out-of-range values in two of six channels: clips are counted per
   * individual sample, not once per whole multi-channel frame.  Float input
   * beyond full scale with scale == 1 keeps the signal on the passthrough
   * path.  A constant stream is fed and the resampler flushed. */
  {
    static size_t const N = 64;
    float * in6 = (float *)malloc(6 * N * sizeof(float));
    unsigned char * out_blocks[6];
    void * out_ptrs[6];
    int32_t expect[6];
    unsigned c;
    size_t f, id = 0, od = 0, total = 0, expect_clips = 0;
    soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT24_BE_S);
    soxr_quality_spec_t q =
        soxr_quality_spec(SOXR_HQ, SOXR_DOUBLE_PRECISION);
    soxr_error_t e2;
    soxr_t r;
    static float const level[6] = {2.0f, 0.0f, 0.25f, -0.25f, 0.001f, -2.0f};
    TEST("clips counted per individual sample, not per frame");
    for (f = 0; f < N; ++f)
      for (c = 0; c < 6; ++c)
        in6[6 * f + c] = level[c];
    for (c = 0; c < 6; ++c) {
      out_blocks[c] = guarded_alloc(3 * (N + 32));
      expect[c] = t_round24((double)level[c] * PCM24_FULL_SCALE);
      if (t_clip24((double)level[c] * PCM24_FULL_SCALE))
        ++expect_clips;
    }
    r = soxr_create(48000, 48000, 6, &e2, &io, &q, 0);
    CHECK(!e2 && r, "per-sample clip create");
    for (c = 0; c < 6; ++c)
      out_ptrs[c] = out_blocks[c] + GUARD;
    e2 = soxr_process(r, in6, N, &id, out_ptrs, N, &od);
    CHECK(!e2 && id == N, "per-sample clip process");
    total = od;
    for (;;) {                     /* Drain the resampler's two-frame tail. */
      size_t fd = 0;
      for (c = 0; c < 6; ++c)
        out_ptrs[c] = out_blocks[c] + GUARD + 3 * total;
      e2 = soxr_process(r, 0, 0, 0, out_ptrs, N + 32 - total, &fd);
      CHECK(!e2, "per-sample clip flush");
      total += fd;
      if (fd == 0)
        break;
    }
    CHECK(total == N, "per-sample clip produced %zu/%zu", total, N);
    CHECK(*soxr_num_clips(r) == expect_clips * N,
        "expected %zu clips, got %zu", expect_clips * N,
        *soxr_num_clips(r));
    for (c = 0; c < 6; ++c) {
      size_t k;
      check_guards(out_blocks[c], 3 * (N + 32), "per-sample clip output");
      for (k = N / 4; k < (3 * N) / 4; ++k) {
        int32_t got = t_unpack24(out_blocks[c] + GUARD + 3 * k, 1);
        CHECK(got == expect[c], "ch%u k=%zu got=%ld want=%ld", c, k,
            (long)got, (long)expect[c]);
      }
      free(out_blocks[c]);
    }
    soxr_delete(r);
    free(in6);
  }
}



/* --- Independent float resampling reference for non-unit ratios -------------- */

struct engine_def {
  unsigned long flags;
  unsigned recipe;
  int is_double;
  char const * name;
};

static struct engine_def const engines[3] = {
  {0, SOXR_HQ, 0, "cr32 single precision"},
  {SOXR_DOUBLE_PRECISION, SOXR_HQ, 1, "cr64 double precision"},
  {SOXR_VR, SOXR_HQ, 0, "vr32 variable rate"}};

static size_t float64_reference(double irate, double orate, unsigned ch,
    size_t n, double const * fin, size_t out_cap, double * fout,
    unsigned long qflags, unsigned recipe)
{
  soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
  soxr_quality_spec_t q = soxr_quality_spec(recipe, qflags);
  size_t id = 0, od = 0;
  soxr_error_t error = soxr_oneshot(irate, orate, ch,
      (void const *)fin, n, &id, fout, out_cap, &od, &io, &q, 0);
  CHECK(!error, "reference oneshot: %s", soxr_strerror(error));
  CHECK(id == n, "reference consumed %zu/%zu", id, n);
  return od;
}

/* Full cross-product of the four pcm24 types against each other over 1/2/6
 * channels and the three engine paths, checked against the float reference. */
static void test_cross_format_matrix(void)
{
  static soxr_datatype_t const pcm24_types[4] = {
    SOXR_INT24_LE_I, SOXR_INT24_BE_I, SOXR_INT24_LE_S, SOXR_INT24_BE_S};
  static unsigned const channels[3] = {1, 2, 6};
  static size_t const N = 1500;
  static double const irate = 44100, orate = 48000;
  size_t out_cap = (size_t)((double)N * orate / irate) + 64;
  double * fin = (double *)malloc((size_t)6 * N * sizeof(double));
  double * fout = (double *)malloc((size_t)6 * out_cap * sizeof(double));
  size_t f, c, a, b, ci, e;
  unsigned char * in_blocks[6]; unsigned char ** in_view = 0;
  unsigned char * out_blocks[6]; unsigned char ** out_view = 0;

  TEST("cross-format matrix vs independent float resampling reference");

  for (ci = 0; ci < 3; ++ci) {
    unsigned ch = channels[ci];
    for (e = 0; e < 3; ++e) {
      size_t ref_odone, k;
      int32_t * expect24;

      for (f = 0; f < N; ++f)
        for (c = 0; c < ch; ++c)
          fin[(size_t)ch * f + c] = sine24(f, (unsigned)c) / PCM24_FULL_SCALE;

      /* Same engine and parameters, but float64 I/O; the 24-bit rounding is
       * done independently in this test, not by production code. */
      ref_odone = float64_reference(irate, orate, ch, N, fin, out_cap, fout,
          engines[e].flags, engines[e].recipe);
      expect24 = (int32_t *)malloc((size_t)ch * ref_odone * sizeof(int32_t));
      for (k = 0; k < (size_t)ch * ref_odone; ++k)
        expect24[k] = t_round24(fout[k] * PCM24_FULL_SCALE);

      for (a = 0; a < 4; ++a) {
        for (b = 0; b < 4; ++b) {
          soxr_datatype_t itype = pcm24_types[a];
          soxr_datatype_t otype = pcm24_types[b];
          soxr_io_spec_t io = soxr_io_spec(itype, otype);
          soxr_quality_spec_t q =
              soxr_quality_spec(engines[e].recipe, engines[e].flags);
          size_t id = 0, od = 0, kk;
          soxr_error_t error;
          unsigned char const * obase;

          build_packed(itype, ch, N, in_blocks, &in_view);
          build_packed_out(otype, ch, out_cap, out_blocks, &out_view);

          error = soxr_oneshot(irate, orate, ch,
              t_is_split(itype)? (void *)in_view :
                                 (void *)(in_blocks[0] + GUARD),
              N, &id,
              t_is_split(otype)? (void *)out_view :
                                 (void *)(out_blocks[0] + GUARD),
              out_cap, &od, &io, &q, 0);
          CHECK(!error, "oneshot %d->%d ch=%u eng=%s: %s",
              (int)itype, (int)otype, ch, engines[e].name,
              soxr_strerror(error));
          CHECK(id == N, "matrix idone %zu/%zu", id, N);
          CHECK(od == ref_odone, "length %zu vs ref %zu (%s)",
              od, ref_odone, engines[e].name);

          obase = t_is_split(otype)?
              (unsigned char const *)out_view : out_blocks[0] + GUARD;
          for (kk = 0; kk < (size_t)ch * (od < ref_odone? od : ref_odone); ++kk) {
            size_t frame = kk / ch, channel = kk % ch;
            int32_t got = read_packed(otype, obase, frame,
                (unsigned)channel, ch);
            long diff = (long)got - (long)expect24[kk];
            long tol = engines[e].is_double? 0 : 2;
            if (diff < 0) diff = -diff;
            CHECK(diff <= tol,
                "pcm24 %d->%d ch=%u eng=%s f=%zu c=%zu got=%ld want=%ld d=%ld",
                (int)itype, (int)otype, ch, engines[e].name,
                frame, channel, (long)got, (long)expect24[kk], diff);
          }
          if (!t_is_split(otype)) {
            check_guards(out_blocks[0], packed_bytes(out_cap, ch),
                "matrix interleaved output");
            check_payload_tail(out_blocks[0], packed_bytes(od, ch),
                packed_bytes(out_cap, ch), "matrix output tail");
          } else {
            for (c = 0; c < ch; ++c) {
              check_guards(out_blocks[c], 3 * out_cap, "matrix split output");
              check_payload_tail(out_blocks[c], 3 * od, 3 * out_cap,
                  "matrix split output tail");
            }
          }
          free_packed(itype, ch, in_blocks, in_view);
          free_packed(otype, ch, out_blocks, out_view);
        }
      }
      free(expect24);
    }
  }
  free(fin);
  free(fout);
}

/* Packed-24 against each legacy family in both directions. */
static void test_legacy_cross_types(void)
{
  static size_t const N = 800;
  static double const irate = 48000, orate = 44100;
  size_t out_cap = (size_t)((double)N * orate / irate) + 64;
  soxr_datatype_t legacy[4] = {
      SOXR_FLOAT32_I, SOXR_FLOAT64_I, SOXR_INT32_I, SOXR_INT16_I};
  size_t li, k, f, c;

  TEST("pcm24 mixed with float32/float64/int32/int16");
  for (li = 0; li < 4; ++li) {
    soxr_datatype_t ltype = legacy[li];
    size_t ls = soxr_datatype_size(ltype);
    void * lin = malloc((size_t)2 * N * ls);
    void * lout = malloc((size_t)2 * out_cap * ls);
    unsigned char * p_out = guarded_alloc(packed_bytes(out_cap, 2));
    unsigned char * p_in = guarded_alloc(packed_bytes(N, 2));
    size_t id = 0, od = 0;
    soxr_io_spec_t io;
    soxr_error_t error;

    for (f = 0; f < N; ++f)
      for (c = 0; c < 2; ++c) {
        int32_t v = sine24(f, (unsigned)c);
        size_t idx = 2 * f + c;
        switch (ltype) {
          case SOXR_FLOAT32_I:
            ((float *)lin)[idx] = (float)(v / PCM24_FULL_SCALE); break;
          case SOXR_FLOAT64_I:
            ((double *)lin)[idx] = v / PCM24_FULL_SCALE; break;
          case SOXR_INT32_I:
            ((int32_t *)lin)[idx] = v; break;
          default:
            ((int16_t *)lin)[idx] = (int16_t)(v >> 8); break;
        }
      }
    for (f = 0; f < N; ++f)
      for (c = 0; c < 2; ++c)
        t_pack24(p_in + GUARD + 3 * (2 * f + c), sine24(f, (unsigned)c), 1);

    io = soxr_io_spec(ltype, SOXR_INT24_LE_I);
    error = soxr_oneshot(irate, orate, 2, lin, N, &id,
        p_out + GUARD, out_cap, &od, &io, 0, 0);
    CHECK(!error && id == N && od > 0, "legacy %d -> pcm24", (int)ltype);
    for (k = 0; k < (size_t)2 * od; ++k) {
      int32_t v = t_unpack24(p_out + GUARD + 3 * k, 0);
      CHECK(v >= PCM24_MIN && v <= PCM24_MAX, "legacy->pcm24 range");
    }
    check_guards(p_out, packed_bytes(out_cap, 2), "legacy->pcm24 guards");
    check_payload_tail(p_out, packed_bytes(od, 2),
        packed_bytes(out_cap, 2), "legacy->pcm24 tail");

    id = od = 0;
    io = soxr_io_spec(SOXR_INT24_BE_I, ltype);
    error = soxr_oneshot(irate, orate, 2, p_in + GUARD, N, &id,
        lout, out_cap, &od, &io, 0, 0);
    CHECK(!error && id == N && od > 0, "pcm24 -> legacy %d", (int)ltype);
    for (k = 0; k < od; ++k) {
      size_t idx = 2 * k;
      switch (ltype) {
        case SOXR_FLOAT32_I:
          CHECK(fabsf(((float *)lout)[idx]) <= 1.0f, "f32 range"); break;
        case SOXR_FLOAT64_I:
          CHECK(fabs(((double *)lout)[idx]) <= 1.0, "f64 range"); break;
        case SOXR_INT32_I:
          CHECK(INT32_MIN <= ((int32_t *)lout)[idx], "s32 range"); break;
        default:
          CHECK(INT16_MIN <= ((int16_t *)lout)[idx] &&
              ((int16_t *)lout)[idx] <= INT16_MAX, "s16 range"); break;
      }
    }
    check_guards(p_in, packed_bytes(N, 2), "pcm24->legacy guards");
    free(lin);
    free(lout);
    free(p_out);
    free(p_in);
  }
}



/* ---- Chunked streaming: 1/7/113-frame feeds, small output cap, sentinels ---- */

struct stream_case {
  soxr_datatype_t itype, otype;
  unsigned ch;
};

static void test_chunked_streaming(struct stream_case const * sc)
{
  static size_t const N = 1400;
  static size_t const chunk_seq[3] = {1, 7, 113};
  static size_t const OUT_CAP = 5;
  static double const irate = 44100, orate = 48000;
  size_t out_cap = (size_t)((double)N * orate / irate) + 256;
  unsigned char * in_blocks[6]; unsigned char ** in_view = 0;
  unsigned char * out_blocks[6]; unsigned char ** out_view = 0;
  unsigned char * ref_blocks[6]; unsigned char ** ref_view = 0;
  size_t total_in = 0, total_out = 0, produced = 0, cycle = 0, f, c;
  soxr_io_spec_t io = soxr_io_spec(sc->itype, sc->otype);
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_HQ, SOXR_DOUBLE_PRECISION);
  soxr_error_t error;
  soxr_t r;
  int split_i = t_is_split(sc->itype), split_o = t_is_split(sc->otype);

  TEST("chunked streaming with guarded buffers");
  build_packed(sc->itype, sc->ch, N, in_blocks, &in_view);
  build_packed_out(sc->otype, sc->ch, out_cap, out_blocks, &out_view);
  build_packed(sc->otype, sc->ch, out_cap, ref_blocks, &ref_view);

  {
    size_t id = 0, od = 0;
    error = soxr_oneshot(irate, orate, sc->ch,
        split_i? (void *)in_view : (void *)(in_blocks[0] + GUARD), N, &id,
        split_o? (void *)ref_view : (void *)(ref_blocks[0] + GUARD),
        out_cap, &od, &io, &q, 0);
    CHECK(!error && id == N, "stream reference oneshot");
    produced = od;
  }

  r = soxr_create(irate, orate, sc->ch, &error, &io, &q, 0);
  CHECK(!error && r, "stream create");

  for (;;) {
    size_t ilen, idone = 0, odone = 0, want;
    void * in_ptrs[6], * out_ptrs[6];
    void * in_arg;
    void * out_arg;

    if (total_in < N) {
      want = chunk_seq[cycle++ % 3];
      ilen = want < N - total_in? want : N - total_in;
    } else {
      ilen = 0;
    }

    if (split_i) {
      for (c = 0; c < sc->ch; ++c)
        in_ptrs[c] = in_view[c] + 3 * total_in;
      in_arg = ilen? in_ptrs : 0;
    } else {
      in_arg = ilen?
          in_blocks[0] + GUARD + packed_bytes(total_in, sc->ch) : 0;
    }

    if (split_o) {
      for (c = 0; c < sc->ch; ++c)
        out_ptrs[c] = out_view[c] + 3 * total_out;
      out_arg = out_ptrs;
    } else {
      out_arg = out_blocks[0] + GUARD + packed_bytes(total_out, sc->ch);
    }

    error = soxr_process(r, in_arg, ilen, &idone, out_arg, OUT_CAP, &odone);
    CHECK(!error, "stream process: %s", soxr_strerror(error));
    CHECK(idone <= ilen, "idone exceeds ilen");
    CHECK(odone <= OUT_CAP, "odone exceeds cap");

    total_in += idone;
    total_out += odone;

    if (split_i)
      for (c = 0; c < sc->ch; ++c)
        check_guards(in_blocks[c], 3 * N, "stream split input");
    else
      check_guards(in_blocks[0], packed_bytes(N, sc->ch), "stream input");
    if (split_o) {
      for (c = 0; c < sc->ch; ++c) {
        check_guards(out_blocks[c], 3 * out_cap, "stream split output");
        check_payload_tail(out_blocks[c], 3 * total_out, 3 * out_cap,
            "stream split output tail");
      }
    } else {
      check_guards(out_blocks[0], packed_bytes(out_cap, sc->ch),
          "stream output");
      check_payload_tail(out_blocks[0], packed_bytes(total_out, sc->ch),
          packed_bytes(out_cap, sc->ch), "stream output tail");
    }

    if (total_in == N && ilen == 0 && odone == 0)
      break;
    if (total_out > out_cap) { CHECK(0, "stream overflow"); break; }
  }
  CHECK(total_in == N, "stream consumed %zu/%zu", total_in, N);
  CHECK(total_out == produced, "stream produced %zu vs oneshot %zu",
      total_out, produced);

  for (f = 0; f < produced; ++f)
    for (c = 0; c < sc->ch; ++c) {
      unsigned char const * ob = split_o?
          (unsigned char const *)out_view : out_blocks[0] + GUARD;
      unsigned char const * rb = split_o?
          (unsigned char const *)ref_view : ref_blocks[0] + GUARD;
      int32_t got = read_packed(sc->otype, ob, f, (unsigned)c, sc->ch);
      int32_t ref = read_packed(sc->otype, rb, f, (unsigned)c, sc->ch);
      CHECK(got == ref, "stream mismatch f=%zu c=%zu %ld!=%ld",
          f, c, (long)got, (long)ref);
    }
  soxr_delete(r);
  free_packed(sc->itype, sc->ch, in_blocks, in_view);
  free_packed(sc->otype, sc->ch, out_blocks, out_view);
  free_packed(sc->otype, sc->ch, ref_blocks, ref_view);
}



/* ------------------------- Input callback / soxr_output ---------------------- */

struct cb_state {
  unsigned char const * base;
  unsigned char ** views;
  void * ptrs[6];
  size_t n, pos;
  unsigned ch;
  int split;
};

static size_t input_callback(void * state0, soxr_in_t * data, size_t requested)
{
  struct cb_state * s = (struct cb_state *)state0;
  size_t avail = s->n - s->pos;
  size_t give = requested < avail? requested : avail;
  unsigned c;
  if (give == 0) {
    /* End of input: data_len 0 with a valid pointer (NULL means failure). */
    *data = s->split? (soxr_in_t)s->ptrs : (soxr_in_t)s->base;
    return 0;
  }
  if (s->split) {
    for (c = 0; c < s->ch; ++c)
      s->ptrs[c] = (void *)(s->views[c] + 3 * s->pos);
    *data = s->ptrs;
  } else {
    *data = (void *)(s->base + 3 * (size_t)s->ch * s->pos);
  }
  s->pos += give;
  return give;
}

static void test_input_callback(int split, int big_endian, unsigned ch)
{
  static size_t const N = 900;
  static double const irate = 48000, orate = 44100;
  size_t out_cap = (size_t)((double)N * orate / irate) + 256;
  soxr_datatype_t itype = split?
      (big_endian? SOXR_INT24_BE_S : SOXR_INT24_LE_S)
    : (big_endian? SOXR_INT24_BE_I : SOXR_INT24_LE_I);
  soxr_datatype_t otype = split? SOXR_INT24_LE_S : SOXR_INT24_LE_I;
  unsigned char * in_blocks[6]; unsigned char ** in_view = 0;
  unsigned char * out_blocks[6]; unsigned char ** out_view = 0;
  unsigned char * ref_blocks[6]; unsigned char ** ref_view = 0;
  size_t total = 0, produced = 0, f, c;
  struct cb_state state;
  soxr_io_spec_t io = soxr_io_spec(itype, otype);
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_HQ, SOXR_DOUBLE_PRECISION);
  soxr_error_t error;
  soxr_t r;

  TEST("input callback through soxr_output");
  build_packed(itype, ch, N, in_blocks, &in_view);
  build_packed_out(otype, ch, out_cap, out_blocks, &out_view);
  build_packed(otype, ch, out_cap, ref_blocks, &ref_view);

  {
    size_t id = 0, od = 0;
    error = soxr_oneshot(irate, orate, ch,
        split? (void *)in_view : (void *)(in_blocks[0] + GUARD), N, &id,
        split? (void *)ref_view : (void *)(ref_blocks[0] + GUARD),
        out_cap, &od, &io, &q, 0);
    CHECK(!error && id == N, "callback reference");
    produced = od;
  }

  memset(&state, 0, sizeof(state));
  state.base = in_blocks[0] + GUARD;
  state.views = in_view;
  state.n = N; state.pos = 0; state.ch = ch; state.split = split;

  r = soxr_create(irate, orate, ch, &error, &io, &q, 0);
  CHECK(!error && r, "callback create");
  error = soxr_set_input_fn(r, input_callback, &state, 37);
  CHECK(!error, "set_input_fn");

  for (;;) {
    size_t want = 11, got;
    void * out_ptrs[6];
    void * out_arg;
    if (split) {
      for (c = 0; c < ch; ++c)
        out_ptrs[c] = out_view[c] + 3 * total;
      out_arg = out_ptrs;
    } else {
      out_arg = out_blocks[0] + GUARD + packed_bytes(total, ch);
    }
    got = soxr_output(r, out_arg, want);
    total += got;
    if (split)
      for (c = 0; c < ch; ++c) {
        check_guards(out_blocks[c], 3 * out_cap, "callback split output");
        check_payload_tail(out_blocks[c], 3 * total, 3 * out_cap,
            "callback split tail");
      }
    else {
      check_guards(out_blocks[0], packed_bytes(out_cap, ch),
          "callback output");
      check_payload_tail(out_blocks[0], packed_bytes(total, ch),
          packed_bytes(out_cap, ch), "callback tail");
    }
    if (got == 0)
      break;
    if (total > out_cap) { CHECK(0, "callback overflow"); break; }
  }
  CHECK(total == produced, "callback produced %zu vs ref %zu", total, produced);

  for (f = 0; f < total; ++f)
    for (c = 0; c < ch; ++c) {
      unsigned char const * ob = split?
          (unsigned char const *)out_view : out_blocks[0] + GUARD;
      unsigned char const * rb = split?
          (unsigned char const *)ref_view : ref_blocks[0] + GUARD;
      int32_t got = read_packed(otype, ob, f, (unsigned)c, ch);
      int32_t ref = read_packed(otype, rb, f, (unsigned)c, ch);
      CHECK(got == ref, "callback mismatch f=%zu c=%zu", f, c);
    }
  soxr_delete(r);
  free_packed(itype, ch, in_blocks, in_view);
  free_packed(otype, ch, out_blocks, out_view);
  free_packed(otype, ch, ref_blocks, ref_view);
}



/* ----------------------------- flush + clear reuse --------------------------- */

static void test_clear_reuse(void)
{
  static size_t const N = 600;
  static double const irate = 44100, orate = 32000;
  size_t out_cap = (size_t)((double)N * orate / irate) + 256;
  unsigned ch = 2, c, run;
  unsigned char * inb = guarded_alloc(packed_bytes(N, ch));
  unsigned char * ob1[2], * ob2[2];
  size_t totals[2] = {0, 0};
  size_t f;
  soxr_io_spec_t io = soxr_io_spec(SOXR_INT24_BE_I, SOXR_INT24_LE_S);
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_HQ, SOXR_DOUBLE_PRECISION);
  soxr_error_t error;
  soxr_t r;

  TEST("flush, soxr_clear, then reuse with the same configuration");
  for (f = 0; f < N; ++f)
    for (c = 0; c < ch; ++c)
      t_pack24(inb + GUARD + 3 * (ch * f + c), sine24(f, c), 1);
  for (c = 0; c < ch; ++c) {
    ob1[c] = guarded_alloc(3 * out_cap);
    ob2[c] = guarded_alloc(3 * out_cap);
  }

  r = soxr_create(irate, orate, ch, &error, &io, &q, 0);
  CHECK(!error && r, "clear-reuse create");

  for (run = 0; run < 2; ++run) {
    unsigned char ** dst = run == 0? ob1 : ob2;
    size_t pos = 0, total = 0;

    if (run == 1) {
      error = soxr_clear(r);
      CHECK(!error, "soxr_clear");
    }
    for (;;) {
      size_t ilen = pos < N? (N - pos < 13? N - pos : 13) : 0;
      size_t id = 0, od = 0;
      void * in = ilen? inb + GUARD + packed_bytes(pos, ch) : 0;
      void * out_ptrs[2];
      for (c = 0; c < ch; ++c)
        out_ptrs[c] = dst[c] + GUARD + 3 * total;
      error = soxr_process(r, in, ilen, &id, out_ptrs, 7, &od);
      CHECK(!error, "clear-reuse process");
      pos += id;
      total += od;
      if (ilen == 0 && od == 0)
        break;
    }
    totals[run] = total;
    for (c = 0; c < ch; ++c) {
      check_guards(dst[c], 3 * out_cap, "clear-reuse output");
      check_payload_tail(dst[c], 3 * total, 3 * out_cap, "clear-reuse tail");
    }
  }

  CHECK(totals[0] == totals[1] && totals[0] > 0, "run lengths differ");
  for (c = 0; c < ch; ++c)
    for (f = 0; f < totals[0]; ++f) {
      int32_t a = t_unpack24(ob1[c] + GUARD + 3 * f, 0);
      int32_t b = t_unpack24(ob2[c] + GUARD + 3 * f, 0);
      CHECK(a == b, "post-clear mismatch c=%u f=%zu", c, f);
    }

  soxr_delete(r);
  for (c = 0; c < ch; ++c) { free(ob1[c]); free(ob2[c]); }
  free(inb);
}



/* ------------------- Amplified stream: exact per-sample clips ---------------- */

static void test_amplified_clips(void)
{
  static size_t const N = 1200;
  static double const irate = 44100, orate = 48000;
  unsigned ch = 6, c;
  size_t out_cap = (size_t)((double)N * orate / irate) + 64;
  double gain = 4.0;
  unsigned char * inb = guarded_alloc(packed_bytes(N, ch));
  unsigned char * outb = guarded_alloc(packed_bytes(out_cap, ch));
  double * fin = (double *)malloc((size_t)ch * N * sizeof(double));
  double * fout = (double *)malloc((size_t)ch * out_cap * sizeof(double));
  size_t f, ref_odone = 0, expect_clips = 0, k;
  size_t id = 0, od = 0;
  soxr_io_spec_t io = soxr_io_spec(SOXR_INT24_LE_I, SOXR_INT24_BE_I);
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_HQ, SOXR_DOUBLE_PRECISION);
  soxr_error_t error;

  TEST("amplification clips counted per sample against float reference");
  io.scale = gain;
  for (f = 0; f < N; ++f)
    for (c = 0; c < ch; ++c) {
      int32_t v = sine24(f, c);
      t_pack24(inb + GUARD + 3 * ((size_t)ch * f + c), v, 0);
      fin[(size_t)ch * f + c] = v / PCM24_FULL_SCALE;
    }

  /* Independent reference: float64 resample with the same engine/parameters,
   * apply the gain, then round to 24 bits using this test's own rounding. */
  {
    soxr_io_spec_t fio = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
    soxr_quality_spec_t fq =
        soxr_quality_spec(SOXR_HQ, SOXR_DOUBLE_PRECISION);
    size_t fid = 0, fod = 0;
    error = soxr_oneshot(irate, orate, ch, fin, N, &fid, fout, out_cap, &fod,
        &fio, &fq, 0);
    CHECK(!error && fid == N, "clip reference resample");
    ref_odone = fod;
    for (k = 0; k < (size_t)ch * ref_odone; ++k) {
      double scaled = gain * fout[k] * PCM24_FULL_SCALE;
      if (t_clip24(scaled))
        ++expect_clips;
    }
  }
  CHECK(expect_clips > 0, "reference signal actually clips");

  {
    soxr_t r = soxr_create(irate, orate, ch, &error, &io, &q, 0);
    CHECK(!error && r, "clip create");
    error = soxr_process(r, inb + GUARD, N, &id, outb + GUARD, out_cap, &od);
    CHECK(!error && id == N, "clip process");
    /* Drain the resampler's tail by signalling end-of-input. */
    for (;;) {
      size_t fd = 0;
      void * tail = outb + GUARD + packed_bytes(od, ch);
      error = soxr_process(r, 0, 0, 0, tail, out_cap - od, &fd);
      CHECK(!error, "clip flush");
      od += fd;
      if (fd == 0)
        break;
    }
    CHECK(od == ref_odone, "clip length %zu vs %zu", od, ref_odone);
    CHECK(*soxr_num_clips(r) == expect_clips,
        "clips %zu != reference %zu", *soxr_num_clips(r), expect_clips);

    for (k = 0; k < (size_t)ch * od; ++k) {
      int32_t got = t_unpack24(outb + GUARD + 3 * k, 1);
      int32_t want = t_round24(gain * fout[k] * PCM24_FULL_SCALE);
      CHECK(got == want, "clip-path sample %zu: %ld != %ld",
          k, (long)got, (long)want);
    }
    check_guards(inb, packed_bytes(N, ch), "clip input");
    check_guards(outb, packed_bytes(out_cap, ch), "clip output");
    check_payload_tail(outb, packed_bytes(od, ch),
        packed_bytes(out_cap, ch), "clip output tail");
    soxr_delete(r);
  }
  free(inb);
  free(outb);
  free(fin);
  free(fout);
}



/* ----------------------- Legacy formats control group ------------------------ */

static void test_legacy_control(void)
{
  static size_t const N = 1000;
  static double const irate = 44100, orate = 48000;
  size_t out_cap = (size_t)((double)N * orate / irate) + 64;
  float * fin = (float *)malloc(2 * N * sizeof(float));
  void * outs[4];
  struct { soxr_datatype_t t; size_t s; } const legs[4] = {
    {SOXR_FLOAT32_I, 4}, {SOXR_FLOAT64_I, 8},
    {SOXR_INT32_I, 4}, {SOXR_INT16_I, 2}};
  size_t li, f, c;

  TEST("legacy float32/float64/int32/int16 control group");
  for (f = 0; f < N; ++f)
    for (c = 0; c < 2; ++c)
      fin[2 * f + c] = (float)(0.25 * sin(2 * PI * 220.0 * (f + c) / irate));

  for (li = 0; li < 4; ++li) {
    soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, legs[li].t);
    size_t id = 0, od = 0;
    soxr_error_t error;
    outs[li] = malloc(2 * out_cap * legs[li].s);
    error = soxr_oneshot(irate, orate, 2, fin, N, &id, outs[li], out_cap, &od,
        &io, 0, 0);
    CHECK(!error && id == N && od > 0, "legacy oneshot %d", (int)legs[li].t);
    if (legs[li].t == SOXR_INT16_I)
      for (f = 0; f < od; ++f) {
        int16_t v = ((int16_t *)outs[li])[2 * f];
        CHECK(v >= INT16_MIN && v <= INT16_MAX, "int16 range");
      }
  }
  for (li = 0; li < 4; ++li) free(outs[li]);
  free(fin);
}



int main(void)
{
  const char * only = getenv("PCM24_ONLY");
  #define WANT(name) (!only || strcmp(only, name) == 0)
  if (WANT("enum")) test_enums_and_validation();
  if (WANT("pack")) test_pack_unpack();
  if (WANT("unit")) test_unit_ratio_bits_and_rounding();
  if (WANT("matrix")) test_cross_format_matrix();
  if (WANT("legacy")) test_legacy_cross_types();

  {
    struct stream_case const sc[4] = {
      {SOXR_INT24_LE_I, SOXR_INT24_LE_I, 1},
      {SOXR_INT24_BE_I, SOXR_INT24_LE_S, 2},
      {SOXR_INT24_LE_S, SOXR_INT24_BE_I, 6},
      {SOXR_INT24_BE_S, SOXR_INT24_BE_S, 2}};
    size_t i;
    for (i = 0; i < sizeof(sc) / sizeof(sc[0]); ++i) {
      char tag[16];
      snprintf(tag, sizeof(tag), "stream%zu", i);
      if (!only || strcmp(only, "stream") == 0 || strcmp(only, tag) == 0)
        test_chunked_streaming(&sc[i]);
    }
  }

  if (WANT("cb")) {
    test_input_callback(0, 0, 2);
    test_input_callback(1, 1, 6);
  }
  if (WANT("clear")) test_clear_reuse();
  if (WANT("clips")) test_amplified_clips();
  if (WANT("legacyctl")) test_legacy_control();

  if (failures) {
    fprintf(stderr, "PCM24 TESTS FAILED: %d failure(s) across %d cases\n",
        failures, cases);
    return 1;
  }
  printf("SOXR_PCM24_OK cases=%d\n", cases);
  return 0;
}
