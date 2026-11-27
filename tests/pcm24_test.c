/* SoX Resampler Library
 * Tests for packed, signed 24-bit PCM datatypes (SOXR_INT24_*).
 *
 * Everything in this file is self contained: the packed 24-bit pack/unpack
 * and the round/clip reference conversions are written independently here
 * and do not call any of the library's internal conversion helpers.
 */

#include <soxr.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PI
#define PI 3.14159265358979323846
#endif

#define PCM24_MAX  8388607            /*  2^23 - 1 */
#define PCM24_MIN (-8388608)          /* -2^23     */
#define PCM24_FULL_SCALE 8388608.0   /*  2^23     */

static int g_cases = 0;

#define CASE() do { ++g_cases; } while (0)

/* The scalar and SIMD variants of an engine share the same base name, the
 * latter carrying an 's' suffix (e.g. cr32 / cr32s). */
static int engine_is(char const * name, char const * base)
{
  size_t n = strlen(base);
  return strcmp(name, base) == 0 ||
      (strncmp(name, base, n) == 0 && strcmp(name + n, "s") == 0);
}

#define FAIL(...) do { \
  fprintf(stderr, "FAIL at %s:%d: ", __FILE__, __LINE__); \
  fprintf(stderr, __VA_ARGS__); \
  fputc('\n', stderr); \
  exit(1); \
} while (0)

#define CHECK(cond, ...) do { if (!(cond)) FAIL(__VA_ARGS__); } while (0)



/* ----------------------- Independent 24-bit helpers ---------------------- */
/* These deliberately mirror nothing from the library sources. */

static int t_is_split(soxr_datatype_t t)
{
  return t == SOXR_FLOAT32_S || t == SOXR_FLOAT64_S ||
      t == SOXR_INT32_S || t == SOXR_INT16_S ||
      t == SOXR_INT24_LE_S || t == SOXR_INT24_BE_S;
}

static int t_is_be(soxr_datatype_t t)
{
  return t == SOXR_INT24_BE_I || t == SOXR_INT24_BE_S;
}

static void t_pack24(unsigned char * p, int32_t x, int be)
{
  uint32_t u = (uint32_t)x;
  if (be) {
    p[0] = (unsigned char)(u >> 16);
    p[1] = (unsigned char)(u >> 8);
    p[2] = (unsigned char)u;
  } else {
    p[0] = (unsigned char)u;
    p[1] = (unsigned char)(u >> 8);
    p[2] = (unsigned char)(u >> 16);
  }
}

static int32_t t_unpack24(unsigned char const * p, int be)
{
  uint32_t u = be?
      ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2] :
      (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
  if (u & 0x00800000u)
    u |= 0xff000000u;
  return (int32_t)u;
}

/* Round half away from zero, then saturate to the signed 24-bit range.
 * Returns the clipped flag separately so tests can count independently. */
static int32_t t_round24(double d, int * clipped)
{
  double r;
  *clipped = 0;
  if (d != d) {                       /* NaN */
    *clipped = 1;
    return 0;
  }
  r = d >= 0.? floor(d + 0.5) : ceil(d - 0.5);
  if (r > PCM24_MAX) { *clipped = 1; return PCM24_MAX; }
  if (r < PCM24_MIN) { *clipped = 1; return PCM24_MIN; }
  return (int32_t)r;
}

static int16_t t_round16(double d, int * clipped)
{
  double r;
  *clipped = 0;
  if (d != d) { *clipped = 1; return 0; }
  r = d >= 0.? floor(d + 0.5) : ceil(d - 0.5);
  if (r > 32767.) { *clipped = 1; return 32767; }
  if (r < -32768.) { *clipped = 1; return -32768; }
  return (int16_t)r;
}

/* Map a normalised sound sample [-1, 1] into a 24-bit LSB count. */
static int32_t t_norm_to_pcm24(double s)
{
  int clip;
  return t_round24(s * PCM24_FULL_SCALE, &clip);
}



/* ------------------------- Packed buffer management ----------------------- */

#define GUARD 16
#define HEAD_PATTERN 0x5A
#define TAIL_PATTERN 0xA5

typedef struct {
  unsigned ch;
  int split;
  size_t frames;
  unsigned char * block;
  size_t block_size;
  unsigned char * base[8];          /* Valid region start per channel (split)
                                       or base[0] for interleaved. */
} pcm_buf;

static void * xmalloc(size_t n)
{
  void * p = malloc(n ? n : 1);
  if (!p) FAIL("out of memory (%zu bytes)", n);
  return p;
}

static pcm_buf * pcm_alloc(unsigned ch, size_t frames, int split)
{
  size_t per = frames * 3 + 2 * GUARD;              /* Split: per channel. */
  size_t whole = (size_t)ch * frames * 3 + 2 * GUARD; /* Interleaved. */
  unsigned c;
  pcm_buf * b = xmalloc(sizeof(*b));
  b->ch = ch;
  b->split = split;
  b->frames = frames;
  b->block_size = split? per * ch : whole;
  b->block = xmalloc(b->block_size);
  memset(b->block, 0, b->block_size);
  if (split) {
    for (c = 0; c < ch; ++c) {
      unsigned char * s = b->block + c * per;
      memset(s, HEAD_PATTERN, GUARD);
      memset(s + GUARD + frames * 3, TAIL_PATTERN, GUARD);
      b->base[c] = s + GUARD;
    }
  } else {
    memset(b->block, HEAD_PATTERN, GUARD);
    memset(b->block + GUARD + (size_t)ch * frames * 3, TAIL_PATTERN, GUARD);
    b->base[0] = b->block + GUARD;
  }
  return b;
}

static void pcm_free(pcm_buf * b)
{
  if (b) { free(b->block); free(b); }
}

static unsigned char * pcm_ptr(pcm_buf * b, unsigned c, size_t frame)
{
  if (b->split)
    return b->base[c] + 3 * frame;
  return b->base[0] + 3 * ((size_t)b->ch * frame + c);
}

static void pcm_put(pcm_buf * b, unsigned c, size_t frame, int32_t v, int be)
{
  t_pack24(pcm_ptr(b, c, frame), v, be);
}

static int32_t pcm_get(pcm_buf * b, unsigned c, size_t frame, int be)
{
  return t_unpack24(pcm_ptr(b, c, frame), be);
}

static void pcm_check_guards(pcm_buf * b)
{
  unsigned c, n = b->split? b->ch : 1;
  for (c = 0; c < n; ++c) {
    size_t seg = b->split?
        b->frames * 3 + 2 * GUARD :
        (size_t)b->ch * b->frames * 3 + 2 * GUARD;
    unsigned char const * s = b->block + c * seg;
    size_t k;
    for (k = 0; k < GUARD; ++k) {
      CHECK(s[k] == HEAD_PATTERN, "head guard overwritten (ch %u k %zu)", c, k);
      CHECK(s[seg - GUARD + k] == TAIL_PATTERN,
          "tail guard overwritten (ch %u k %zu)", c, k);
    }
  }
}

/* Fill every valid byte with a distinctive pattern before a run, so that
 * "no output" regions can also be detected by the caller. */
static void pcm_fill_valid(pcm_buf * b, unsigned char pat)
{
  unsigned c;
  if (b->split)
    for (c = 0; c < b->ch; ++c)
      memset(b->base[c], pat, b->frames * 3);
  else
    memset(b->base[0], pat, b->frames * (size_t)b->ch * 3);
}



/* --------------------------- Float reference path ------------------------- */

/* Independent reference resampling in double precision (FLOAT64_I/O,
 * cr64 engine), returning a freshly malloc'd interleaved double buffer. */
static double * ref_resample64(double const * in, size_t ilen, unsigned ch,
    double irate, double orate, soxr_quality_spec_t q, double scale,
    size_t * olen_out)
{
  size_t cap = (size_t)((double)ilen * orate / irate + 4096);
  double * out = xmalloc(cap * ch * sizeof(*out));
  size_t idone = 0, odone = 0;
  soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
  soxr_error_t e;
  io.scale = scale;
  e = soxr_oneshot(irate, orate, ch, in, ilen, &idone, out, cap, &odone,
      &io, &q, 0);
  CHECK(!e, "reference soxr_oneshot failed: %s", soxr_strerror(e));
  CHECK(idone == ilen, "reference consumed %zu of %zu", idone, ilen);
  *olen_out = odone;
  return out;
}

/* Single precision (cr32/vr32) FLOAT32 reference. */
static float * ref_resample32(float const * in, size_t ilen, unsigned ch,
    double irate, double orate, soxr_quality_spec_t q, double scale,
    size_t * olen_out)
{
  size_t cap = (size_t)((double)ilen * orate / irate + 4096);
  float * out = xmalloc(cap * ch * sizeof(*out));
  size_t idone = 0, odone = 0;
  soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
  soxr_error_t e;
  io.scale = scale;
  e = soxr_oneshot(irate, orate, ch, in, ilen, &idone, out, cap, &odone,
      &io, &q, 0);
  CHECK(!e, "float reference soxr_oneshot failed: %s", soxr_strerror(e));
  CHECK(idone == ilen, "float reference consumed %zu of %zu", idone, ilen);
  *olen_out = odone;
  return out;
}



/* ----------------- 1. Enum values, sizes and validation ------------------- */

static void test_enums_and_validation(void)
{
  soxr_io_spec_t io;
  soxr_error_t e;
  soxr_t p;

  CASE();
  /* Old enumerators keep their historic numeric values. */
  CHECK(SOXR_FLOAT32_I == 0, "FLOAT32_I value changed");
  CHECK(SOXR_FLOAT64_I == 1, "FLOAT64_I value changed");
  CHECK(SOXR_INT32_I == 2, "INT32_I value changed");
  CHECK(SOXR_INT16_I == 3, "INT16_I value changed");
  CHECK(SOXR_SPLIT == 4, "SPLIT value changed");
  CHECK(SOXR_FLOAT32_S == 4 && SOXR_FLOAT64_S == 5 &&
      SOXR_INT32_S == 6 && SOXR_INT16_S == 7, "split enum values changed");
  CHECK(SOXR_INT24_LE_I == 8 && SOXR_INT24_BE_I == 9 &&
      SOXR_INT24_LE_S == 10 && SOXR_INT24_BE_S == 11, "new enum values");

  CASE();
  CHECK(soxr_datatype_size(SOXR_FLOAT32_I) == 4, "size f32");
  CHECK(soxr_datatype_size(SOXR_FLOAT64_S) == 8, "size f64");
  CHECK(soxr_datatype_size(SOXR_INT32_I) == 4, "size i32");
  CHECK(soxr_datatype_size(SOXR_INT16_S) == 2, "size i16");
  CHECK(soxr_datatype_size(SOXR_INT24_LE_I) == 3, "size i24 LE I");
  CHECK(soxr_datatype_size(SOXR_INT24_BE_I) == 3, "size i24 BE I");
  CHECK(soxr_datatype_size(SOXR_INT24_LE_S) == 3, "size i24 LE S");
  CHECK(soxr_datatype_size(SOXR_INT24_BE_S) == 3, "size i24 BE S");
  CHECK(soxr_datatype_size((soxr_datatype_t)12) == 0, "invalid size 12");
  CHECK(soxr_datatype_size((soxr_datatype_t)99) == 0, "invalid size 99");
  CHECK(soxr_datatype_size((soxr_datatype_t)-1) == 0, "invalid size -1");

  CASE();
  io = soxr_io_spec(SOXR_INT24_LE_S, SOXR_INT24_BE_I);
  CHECK(!io.e, "valid pcm24 io_spec rejected");
  io = soxr_io_spec(SOXR_INT16_S, SOXR_INT16_S);
  CHECK(!io.e, "legacy split io_spec rejected");
  io = soxr_io_spec((soxr_datatype_t)12, SOXR_INT24_LE_I);
  CHECK(io.e != 0, "invalid itype accepted");
  io = soxr_io_spec(SOXR_INT24_BE_S, (soxr_datatype_t)99);
  CHECK(io.e != 0, "invalid otype accepted");

  CASE();
  io = soxr_io_spec((soxr_datatype_t)12, SOXR_INT24_LE_I);
  p = soxr_create(48000, 48000, 1, &e, &io, 0, 0);
  CHECK(p == 0 && e != 0, "soxr_create accepted invalid itype");
  io = soxr_io_spec(SOXR_INT24_LE_I, SOXR_INT24_LE_I);
  p = soxr_create(48000, 48000, 1, &e, &io, 0, 0);
  CHECK(p != 0 && !e, "soxr_create rejected valid pcm24 spec");
  soxr_delete(p);
}



/* ------------------- 2. Independent helper self tests --------------------- */

static void test_pack_round_reference(void)
{
  static const struct { int32_t v; unsigned char le[3], be[3]; } tab[] = {
    {      0, {0x00, 0x00, 0x00}, {0x00, 0x00, 0x00}},
    {      1, {0x01, 0x00, 0x00}, {0x00, 0x00, 0x01}},
    {     -1, {0xff, 0xff, 0xff}, {0xff, 0xff, 0xff}},
    {    256, {0x00, 0x01, 0x00}, {0x00, 0x01, 0x00}},
    {1193046, {0x56, 0x34, 0x12}, {0x12, 0x34, 0x56}}, /* 0x123456 */
    {8388607, {0xff, 0xff, 0x7f}, {0x7f, 0xff, 0xff}},
    {-8388608,{0x00, 0x00, 0x80}, {0x80, 0x00, 0x00}},
    {-8388607,{0x01, 0x00, 0x80}, {0x80, 0x00, 0x01}},
  };
  unsigned i;
  unsigned char buf[3];

  CASE();
  for (i = 0; i < sizeof(tab) / sizeof(tab[0]); ++i) {
    t_pack24(buf, tab[i].v, 0);
    CHECK(memcmp(buf, tab[i].le, 3) == 0, "LE pack of %d", (int)tab[i].v);
    CHECK(t_unpack24(buf, 0) == tab[i].v, "LE unpack of %d", (int)tab[i].v);
    t_pack24(buf, tab[i].v, 1);
    CHECK(memcmp(buf, tab[i].be, 3) == 0, "BE pack of %d", (int)tab[i].v);
    CHECK(t_unpack24(buf, 1) == tab[i].v, "BE unpack of %d", (int)tab[i].v);
  }
  /* Endianness must actually matter. */
  t_pack24(buf, 1193046, 0);
  CHECK(buf[0] == 0x56 && buf[2] == 0x12, "LE byte order");
  t_pack24(buf, 1193046, 1);
  CHECK(buf[0] == 0x12 && buf[2] == 0x56, "BE byte order");

  CASE();
  {
    int clip = 0;
    /* Half integers go away from zero. */
    CHECK(t_round24(0.0, &clip) == 0 && !clip, "round 0");
    CHECK(t_round24(0.4999, &clip) == 0 && !clip, "round 0.4999");
    CHECK(t_round24(0.5, &clip) == 1 && !clip, "round +0.5");
    CHECK(t_round24(-0.5, &clip) == -1 && !clip, "round -0.5");
    CHECK(t_round24(1.5, &clip) == 2 && !clip, "round 1.5");
    CHECK(t_round24(-1.5, &clip) == -2 && !clip, "round -1.5");
    CHECK(t_round24(8388606.5, &clip) == 8388607 && !clip, "round max-0.5");
    CHECK(t_round24(8388607.5, &clip) == 8388607 && clip, "clip positive");
    CHECK(t_round24(1e12, &clip) == 8388607 && clip, "clip huge positive");
    CHECK(t_round24(-8388608.5, &clip) == -8388608 && clip, "clip negative");
    CHECK(t_round24(-1e12, &clip) == -8388608 && clip, "clip huge negative");
    CHECK(t_round24((double)PCM24_MAX, &clip) == PCM24_MAX && !clip, "max");
    CHECK(t_round24((double)PCM24_MIN, &clip) == PCM24_MIN && !clip, "min");
  }
}



/* ---------------- 3. Unit ratio: every valid bit survives ----------------- */

static soxr_quality_spec_t q_quick(void)
{
  return soxr_quality_spec(SOXR_QQ, 0);       /* Short cubic kernel. */
}

/* Constant runs of each special value; the resampler has unity DC gain so a
 * settled run must come back bit-for-bit identical at unit ratio. */
static void test_unit_ratio_all_bits(void)
{
  static int32_t const vals[] = {
    PCM24_MAX, PCM24_MIN, -1, 0, 1, 171, 255, 256, -255, -256,
    1193046, -1193047, 8323073, -8323072, 65535, 65536, 0x7F0001, -0x7F0001
  };
  enum { LEAD = 256, RUN = 256, TAIL = 256, SKIP = 64, CHECKN = 128 };
  size_t const ilen = LEAD + sizeof(vals) / sizeof(vals[0]) * RUN + TAIL;
  soxr_datatype_t const types[4] = {
    SOXR_INT24_LE_I, SOXR_INT24_BE_I, SOXR_INT24_LE_S, SOXR_INT24_BE_S};
  unsigned ch = 2, a, b, vi, c;
  size_t i;
  int32_t * src;
  soxr_quality_spec_t q = q_quick();

  src = xmalloc(ilen * ch * sizeof(*src));
  for (i = 0; i < ilen; ++i)
    for (c = 0; c < ch; ++c)
      src[i * ch + c] =
          (i >= LEAD && i < ilen - TAIL)?
          vals[(i - LEAD) / RUN % (sizeof(vals) / sizeof(vals[0]))] : 0;

  for (a = 0; a < 4; ++a) for (b = 0; b < 4; ++b) {
    pcm_buf * in = pcm_alloc(ch, ilen, t_is_split(types[a]));
    pcm_buf * out = pcm_alloc(ch, ilen + 64, t_is_split(types[b]));
    size_t idone = 0, odone = 0, f;
    soxr_io_spec_t io = soxr_io_spec(types[a], types[b]);
    soxr_error_t e;
    CASE();
    CHECK(!io.e, "pcm24 io spec");
    for (i = 0; i < ilen; ++i)
      for (c = 0; c < ch; ++c)
        pcm_put(in, c, i, src[i * ch + c], t_is_be(types[a]));
    {
      soxr_t p = soxr_create(48000, 48000, ch, &e, &io, &q, 0);
      CHECK(p && !e, "create: %s", soxr_strerror(e));
      CHECK(engine_is(soxr_engine(p), "cr32"), "QQ must use cr32/cr32s");
      soxr_delete(p);
    }
    e = soxr_oneshot(48000, 48000, ch,
        t_is_split(types[a])? (void *)in->base : (void *)in->base[0],
        ilen, &idone,
        t_is_split(types[b])? (void *)out->base : (void *)out->base[0],
        out->frames, &odone, &io, &q, 0);
    CHECK(!e, "oneshot: %s", soxr_strerror(e));
    CHECK(idone == ilen, "idone %zu vs %zu", idone, ilen);
    CHECK(odone >= ilen - 16, "too little output %zu", odone);
    for (vi = 0; vi < sizeof(vals) / sizeof(vals[0]); ++vi) {
      size_t start = LEAD + vi * RUN + SKIP;
      for (f = start; f < start + CHECKN && f < odone; ++f)
        for (c = 0; c < ch; ++c) {
          int32_t got = pcm_get(out, c, f, t_is_be(types[b]));
          CHECK(got == vals[vi],
              "bit-exact run vi=%u f=%zu ch=%u: got %d want %d (types %d/%d)",
              vi, f, c, (int)got, (int)vals[vi], (int)types[a], (int)types[b]);
        }
    }
    pcm_check_guards(in);
    pcm_check_guards(out);
    pcm_free(in);
    pcm_free(out);
  }
  free(src);
}



/* --------- 4. Legacy type cross-conversions at unity (DC) gain ------------ */

static void test_legacy_cross_types(void)
{
  enum { N = 600, SETTLE = 100 };
  static int32_t const pcm_vals[] = {
    0, 256000, -256000, 1193046, -1193047, 65536, -65536, 8323073, -8323072};
  size_t i;
  soxr_quality_spec_t q = q_quick();
  soxr_io_spec_t io;
  soxr_error_t e;
  size_t idone, odone;

  /* PCM24 -> FLOAT32 / FLOAT64 */
  for (unsigned ft = 0; ft < 2; ++ft) {
    soxr_datatype_t otype = ft? SOXR_FLOAT64_I : SOXR_FLOAT32_I;
    pcm_buf * in = pcm_alloc(1, N, 0);
    void * out = xmalloc((size_t)N * (ft? 8 : 4));
    CASE();
    for (i = 0; i < N; ++i)
      pcm_put(in, 0, i, pcm_vals[i % 9], 0);
    io = soxr_io_spec(SOXR_INT24_LE_I, otype);
    e = soxr_oneshot(48000, 48000, 1, in->base[0], N, &idone, out, N, &odone,
        &io, &q, 0);
    CHECK(!e && idone == N && odone >= N - 16, "f-out oneshot");
    for (i = SETTLE; i < N - 16; ++i) {
      double want = (double)pcm_vals[i % 9] / PCM24_FULL_SCALE;
      double got = ft? ((double *)out)[i] : (double)((float *)out)[i];
      CHECK(fabs(got - want) <= 2.0 / PCM24_FULL_SCALE,
          "pcm24->float i=%zu got %.10g want %.10g", i, got, want);
    }
    pcm_check_guards(in);
    pcm_free(in);
    free(out);
  }

  /* FLOAT64 -> PCM24: exact since both the integer and /2^23 are exact. */
  {
    double * fin = xmalloc(N * sizeof(double));
    pcm_buf * out = pcm_alloc(1, N + 16, 0);
    CASE();
    for (i = 0; i < N; ++i)
      fin[i] = (double)pcm_vals[i % 9] / PCM24_FULL_SCALE;
    io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_INT24_BE_I);
    e = soxr_oneshot(48000, 48000, 1, fin, N, &idone, out->base[0],
        out->frames, &odone, &io, &q, 0);
    CHECK(!e && idone == N, "f64-in oneshot");
    for (i = SETTLE; i < N - 16; ++i)
      CHECK(pcm_get(out, 0, i, 1) == pcm_vals[i % 9],
          "float64->pcm24 i=%zu got %d want %d", i,
          (int)pcm_get(out, 0, i, 1), (int)pcm_vals[i % 9]);
    pcm_check_guards(out);
    pcm_free(out);
    free(fin);
  }

  /* INT32 -> PCM24 (use values divisible by 256). */
  {
    int32_t * i32 = xmalloc(N * sizeof(*i32));
    pcm_buf * out = pcm_alloc(1, N + 16, 0);
    CASE();
    for (i = 0; i < N; ++i)
      i32[i] = pcm_vals[i % 9] * 256;
    io = soxr_io_spec(SOXR_INT32_I, SOXR_INT24_LE_I);
    e = soxr_oneshot(48000, 48000, 1, i32, N, &idone, out->base[0],
        out->frames, &odone, &io, &q, 0);
    CHECK(!e && idone == N, "i32-in oneshot");
    for (i = SETTLE; i < N - 16; ++i)
      CHECK(pcm_get(out, 0, i, 0) == pcm_vals[i % 9],
          "int32->pcm24 i=%zu got %d want %d", i,
          (int)pcm_get(out, 0, i, 0), (int)pcm_vals[i % 9]);
    pcm_check_guards(out);
    pcm_free(out);
    free(i32);
  }

  /* PCM24 -> INT32 and PCM24 -> INT16 (dither disabled for determinism). */
  {
    int32_t * o32 = xmalloc(N * sizeof(*o32));
    int16_t * o16 = xmalloc(N * sizeof(*o16));
    pcm_buf * in = pcm_alloc(1, N, 0);
    CASE();
    for (i = 0; i < N; ++i)
      pcm_put(in, 0, i, pcm_vals[i % 9], 0);
    io = soxr_io_spec(SOXR_INT24_LE_I, SOXR_INT32_I);
    e = soxr_oneshot(48000, 48000, 1, in->base[0], N, &idone, o32, N, &odone,
        &io, &q, 0);
    CHECK(!e && idone == N, "pcm24->i32 oneshot");
    io = soxr_io_spec(SOXR_INT24_LE_I, SOXR_INT16_I);
    io.flags = SOXR_NO_DITHER;
    e = soxr_oneshot(48000, 48000, 1, in->base[0], N, &idone, o16, N, &odone,
        &io, &q, 0);
    CHECK(!e && idone == N, "pcm24->i16 oneshot");
    for (i = SETTLE; i < N - 16; ++i) {
      int32_t v = pcm_vals[i % 9];
      int clip;
      int16_t want16;
      CHECK(o32[i] == v * 256, "pcm24->int32 i=%zu got %d want %d",
          i, (int)o32[i], (int)(v * 256));
      want16 = t_round16((double)v / 256., &clip);
      CHECK(o16[i] == want16, "pcm24->int16 i=%zu got %d want %d",
          i, (int)o16[i], (int)want16);
    }
    pcm_check_guards(in);
    pcm_free(in);
    free(o32);
    free(o16);
  }
}



/* --- 4b. Mixed datatypes with independent input/output split layouts ----- */

static void make_pcm_source(int32_t * src, size_t n, unsigned ch,
    double irate, unsigned seed);

static void test_mixed_split_layouts(void)
{
  enum { ILEN = 1500 };
  double const irate = 48000., orate = 44100.;
  unsigned ch = 6;
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_20_BITQ, 0);
  int32_t * src = xmalloc(ILEN * ch * sizeof(*src));
  float * f_ref;
  size_t ref_olen, i, c;
  pcm_buf * in24s = pcm_alloc(ch, ILEN, 1);            /* packed24 split */
  pcm_buf * out24s = pcm_alloc(ch, ILEN + 1024, 1);    /* packed24 split */
  pcm_buf * out24i = pcm_alloc(ch, ILEN + 1024, 0);    /* packed24 inter. */
  float ** fout_split = xmalloc(ch * sizeof(*fout_split));

  make_pcm_source(src, ILEN, ch, irate, 7);
  {
    float * fin = xmalloc(ILEN * ch * sizeof(*fin));
    for (i = 0; i < ILEN * ch; ++i)
      fin[i] = (float)((double)src[i] / PCM24_FULL_SCALE);
    f_ref = ref_resample32(fin, ILEN, ch, irate, orate, q, 1., &ref_olen);
    free(fin);
  }
  for (i = 0; i < ILEN; ++i)
    for (c = 0; c < ch; ++c)
      pcm_put(in24s, c, i, src[i * ch + c], 0);

  /* (a) packed24 split in -> FLOAT32 split out. */
  CASE();
  for (c = 0; c < ch; ++c)
    fout_split[c] = xmalloc((ref_olen + 16) * sizeof(float));
  {
    soxr_io_spec_t io = soxr_io_spec(SOXR_INT24_LE_S, SOXR_FLOAT32_S);
    size_t idone = 0, odone = 0;
    soxr_error_t e = soxr_oneshot(irate, orate, ch, in24s->base, ILEN,
        &idone, fout_split, ref_olen + 16, &odone, &io, &q, 0);
    CHECK(!e && idone == ILEN && odone == ref_olen, "24s->f32s counts");
    for (i = 0; i < odone; ++i)
      for (c = 0; c < ch; ++c) {
        int clip;
        int32_t want = t_round24((double)f_ref[i * ch + c] *
            PCM24_FULL_SCALE, &clip);
        int32_t got = t_round24((double)fout_split[c][i] *
            PCM24_FULL_SCALE, &clip);
        CHECK(labs((long)got - (long)want) <= 2, "24s->f32s f=%zu ch=%u",
            i, (unsigned)c);
      }
  }

  /* (b) packed24 split in -> packed24 split out, and -> interleaved out:
   * the input and output split layouts are chosen independently. */
  {
    soxr_io_spec_t ios = soxr_io_spec(SOXR_INT24_LE_S, SOXR_INT24_BE_S);
    soxr_io_spec_t ioi = soxr_io_spec(SOXR_INT24_LE_S, SOXR_INT24_BE_I);
    size_t idone = 0, odone = 0;
    soxr_error_t e = soxr_oneshot(irate, orate, ch, in24s->base, ILEN,
        &idone, out24s->base, out24s->frames, &odone, &ios, &q, 0);
    CHECK(!e && idone == ILEN && odone == ref_olen, "24s->24s counts");
    idone = odone = 0;
    e = soxr_oneshot(irate, orate, ch, in24s->base, ILEN, &idone,
        out24i->base[0], out24i->frames, &odone, &ioi, &q, 0);
    CHECK(!e && idone == ILEN && odone == ref_olen, "24s->24i counts");
    for (i = 0; i < ref_olen; ++i)
      for (c = 0; c < ch; ++c) {
        int clip;
        int32_t want = t_round24((double)f_ref[i * ch + c] *
            PCM24_FULL_SCALE, &clip);
        CHECK(labs((long)pcm_get(out24s, c, i, 1) - (long)want) <= 2,
            "24s->24s f=%zu ch=%u", i, (unsigned)c);
        CHECK(labs((long)pcm_get(out24i, c, i, 1) - (long)want) <= 2,
            "24s->24i f=%zu ch=%u", i, (unsigned)c);
      }
  }

  pcm_check_guards(in24s);
  pcm_check_guards(out24s);
  pcm_check_guards(out24i);
  for (c = 0; c < ch; ++c)
    free(fout_split[c]);
  free(fout_split);
  pcm_free(in24s);
  pcm_free(out24s);
  pcm_free(out24i);
  free(f_ref);
  free(src);
}



/* ------ 5. Non-unit ratios: double engine exactly matches an independent --- */
/* ------------------ rounding of a float64 resample path ------------------ */

static void make_pcm_source(int32_t * src, size_t n, unsigned ch,
    double irate, unsigned seed)
{
  static double const freqs[6] = {371, 523, 741, 987, 1231, 1543};
  static double const amps[6] = {.85, .7, .61, .53, .44, .33};
  static double const phase[6] = {0, .7, 1.3, 2.1, 2.9, 3.7};
  size_t i;
  unsigned c;
  for (i = 0; i < n; ++i)
    for (c = 0; c < ch; ++c) {
      double s = amps[c % 6] * sin(2 * PI * freqs[c % 6] * i / irate +
          phase[c % 6] + seed * .01);
      src[i * ch + c] = t_norm_to_pcm24(s);
    }
}

static void test_double_engine_exact(void)
{
  enum { ILEN = 3000 };
  double const irate = 32000., orate = 48000.;
  unsigned ch = 2;
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_28_BITQ, 0);
  soxr_datatype_t const types[4] = {
    SOXR_INT24_LE_I, SOXR_INT24_BE_I, SOXR_INT24_LE_S, SOXR_INT24_BE_S};
  int32_t * src = xmalloc(ILEN * ch * sizeof(*src));
  double * ref_in = xmalloc(ILEN * ch * sizeof(*ref_in));
  size_t ref_olen, i, c;
  double * ref_out;
  unsigned a4, b4;

  CASE();
  make_pcm_source(src, ILEN, ch, irate, 1);
  for (i = 0; i < ILEN * ch; ++i)
    ref_in[i] = (double)src[i] / PCM24_FULL_SCALE;
  ref_out = ref_resample64(ref_in, ILEN, ch, irate, orate, q, 1., &ref_olen);

  for (a4 = 0; a4 < 4; ++a4) for (b4 = 0; b4 < 4; ++b4) {
    pcm_buf * in = pcm_alloc(ch, ILEN, t_is_split(types[a4]));
    pcm_buf * out = pcm_alloc(ch, ref_olen + 64, t_is_split(types[b4]));
    size_t idone = 0, odone = 0;
    soxr_io_spec_t io = soxr_io_spec(types[a4], types[b4]);
    soxr_error_t e;
    soxr_t p;
    CASE();
    for (i = 0; i < ILEN; ++i)
      for (c = 0; c < ch; ++c)
        pcm_put(in, c, i, src[i * ch + c], t_is_be(types[a4]));
    p = soxr_create(irate, orate, ch, &e, &io, &q, 0);
    CHECK(p && !e, "create: %s", soxr_strerror(e));
    CHECK(engine_is(soxr_engine(p), "cr64"), "28-bit quality must use cr64/cr64s");
    soxr_delete(p);
    e = soxr_oneshot(irate, orate, ch,
        t_is_split(types[a4])? (void *)in->base : (void *)in->base[0],
        ILEN, &idone,
        t_is_split(types[b4])? (void *)out->base : (void *)out->base[0],
        out->frames, &odone, &io, &q, 0);
    CHECK(!e, "oneshot: %s", soxr_strerror(e));
    CHECK(idone == ILEN, "idone %zu", idone);
    CHECK(odone == ref_olen, "odone %zu vs ref %zu", odone, ref_olen);
    for (i = 0; i < odone; ++i)
      for (c = 0; c < ch; ++c) {
        int clip;
        int32_t want = t_round24(ref_out[i * ch + c] * PCM24_FULL_SCALE, &clip);
        int32_t got = pcm_get(out, c, i, t_is_be(types[b4]));
        CHECK(got == want,
            "exact mismatch types %d/%d f=%zu ch=%u: got %d want %d",
            (int)types[a4], (int)types[b4], i, (unsigned)c,
            (int)got, (int)want);
      }
    pcm_check_guards(in);
    pcm_check_guards(out);
    pcm_free(in);
    pcm_free(out);
  }
  free(ref_out);
  free(ref_in);
  free(src);
}



/* ---- 6. Single precision path vs a same-parameter FLOAT32 resample ------- */
/* The independent reference runs the *same* cr32 engine with FLOAT32 I/O, so
 * the only differences under test are the packed unpack/normalise, the *2^23
 * scale-back and the half-away-from-zero round/saturate: every frame, edge
 * transients included, must match the independently rounded float output to
 * within 2 LSB (in practice it is exact). */

static void test_single_precision_tolerance(void)
{
  enum { ILEN = 4000 };
  double const irate = 48000., orate = 44100.;
  unsigned ch = 2;
  soxr_quality_spec_t qs = soxr_quality_spec(SOXR_20_BITQ, 0);
  int32_t * src = xmalloc(ILEN * ch * sizeof(*src));
  float * ref_in = xmalloc(ILEN * ch * sizeof(*ref_in));
  float * ref_out;
  size_t ref_olen, i, c, exact = 0, compared = 0;
  pcm_buf * in = pcm_alloc(ch, ILEN, 0);
  pcm_buf * out = pcm_alloc(ch, ILEN + 1024, 0);
  soxr_io_spec_t io = soxr_io_spec(SOXR_INT24_LE_I, SOXR_INT24_BE_I);
  soxr_error_t e;
  soxr_t p;
  size_t idone = 0, odone = 0;
  long maxdiff = 0;

  CASE();
  make_pcm_source(src, ILEN, ch, irate, 2);
  for (i = 0; i < ILEN * ch; ++i)
    ref_in[i] = (float)((double)src[i] / PCM24_FULL_SCALE);
  ref_out = ref_resample32(ref_in, ILEN, ch, irate, orate, qs, 1., &ref_olen);

  for (i = 0; i < ILEN; ++i)
    for (c = 0; c < ch; ++c)
      pcm_put(in, c, i, src[i * ch + c], 0);
  p = soxr_create(irate, orate, ch, &e, &io, &qs, 0);
  CHECK(p && !e, "create: %s", soxr_strerror(e));
  CHECK(engine_is(soxr_engine(p), "cr32"), "20-bit quality must use cr32/cr32s");
  soxr_delete(p);
  e = soxr_oneshot(irate, orate, ch, in->base[0], ILEN, &idone,
      out->base[0], out->frames, &odone, &io, &qs, 0);
  CHECK(!e && idone == ILEN && odone == ref_olen, "cr32 oneshot counts");
  for (i = 0; i < odone; ++i)
    for (c = 0; c < ch; ++c) {
      int clip;
      int32_t want = t_round24((double)ref_out[i * ch + c] * PCM24_FULL_SCALE,
          &clip);
      int32_t got = pcm_get(out, c, i, 1);
      long d = labs((long)got - (long)want);
      if (d > maxdiff) maxdiff = d;
      if (d == 0) ++exact;
      ++compared;
      CHECK(d <= 2, "cr32 vs same-engine float drift %ld LSB at f=%zu ch=%u",
          d, i, (unsigned)c);
    }
  CHECK(exact * 2 >= compared, "too few exact samples: %zu/%zu",
      exact, compared);
  pcm_check_guards(in);
  pcm_check_guards(out);
  pcm_free(in);
  pcm_free(out);
  free(ref_out);
  free(ref_in);
  free(src);
}



/* 7. Streaming: tiny chunks, tiny output capacity, all layouts, sentinels --- */

typedef struct {
  soxr_datatype_t itype, otype;
  unsigned ch;
  double irate, orate;
  soxr_quality_spec_t q;
} stream_cfg;

static void run_streaming(stream_cfg cfg, int32_t const * src, size_t ilen,
    float const * ref_out, size_t ref_olen)
{
  static size_t const chunk_tab[3] = {1, 7, 113};
  static size_t const out_cap = 5;
  pcm_buf * in = pcm_alloc(cfg.ch, ilen + 4, t_is_split(cfg.itype));
  pcm_buf * out = pcm_alloc(cfg.ch, ref_olen + 64, t_is_split(cfg.otype));
  size_t i, c, in_pos = 0, chunk_i = 0, total_in = 0, total_out = 0;
  soxr_io_spec_t io = soxr_io_spec(cfg.itype, cfg.otype);
  soxr_error_t e;
  soxr_t p;

  CHECK(!io.e, "stream io spec");
  for (i = 0; i < ilen; ++i)
    for (c = 0; c < cfg.ch; ++c)
      pcm_put(in, c, i, src[i * cfg.ch + c], t_is_be(cfg.itype));
  pcm_fill_valid(out, 0x11);

  p = soxr_create(cfg.irate, cfg.orate, cfg.ch, &e, &io, &cfg.q, 0);
  CHECK(p && !e, "stream create: %s", soxr_strerror(e));

  /* Feed the input in 1/7/113-frame chunks, always requesting at most a
   * small output capacity; advance the 3-byte buffers by returned frames. */
  while (in_pos < ilen) {
    size_t want = chunk_tab[chunk_i++ % 3];
    size_t ilen1 = want < ilen - in_pos? want : ilen - in_pos;
    size_t idone = 0, odone = 0;
    void * in_arg, * out_arg;
    unsigned char * ipp[8], * opp[8];   /* Distinct, both live past the call. */
    if (t_is_split(cfg.itype)) {
      for (c = 0; c < cfg.ch; ++c)
        ipp[c] = pcm_ptr(in, (unsigned)c, in_pos);
      in_arg = ipp;
    } else
      in_arg = pcm_ptr(in, 0, in_pos);
    if (t_is_split(cfg.otype)) {
      for (c = 0; c < cfg.ch; ++c)
        opp[c] = pcm_ptr(out, (unsigned)c, total_out);
      out_arg = opp;
    } else
      out_arg = pcm_ptr(out, 0, total_out);
    e = soxr_process(p, in_arg, ilen1, &idone, out_arg, out_cap, &odone);
    CHECK(!e, "stream process: %s", soxr_strerror(e));
    CHECK(idone <= ilen1, "over-consumed input");
    CHECK(odone <= out_cap, "over-produced output");
    in_pos += idone;
    total_in += idone;
    total_out += odone;
    pcm_check_guards(in);
    pcm_check_guards(out);
  }
  CHECK(total_in == ilen, "stream consumed %zu of %zu", total_in, ilen);

  /* Flush with NULL input until nothing more comes out. */
  for (;;) {
    size_t idone = 0, odone = 0;
    void * out_arg;
    if (t_is_split(cfg.otype)) {
      unsigned char * pp[8];
      for (c = 0; c < cfg.ch; ++c)
        pp[c] = pcm_ptr(out, (unsigned)c, total_out);
      out_arg = pp;
    } else
      out_arg = pcm_ptr(out, 0, total_out);
    e = soxr_process(p, 0, 0, &idone, out_arg, out_cap, &odone);
    CHECK(!e, "flush process: %s", soxr_strerror(e));
    total_out += odone;
    if (!odone) break;
    pcm_check_guards(out);
  }
  CHECK(total_out == ref_olen, "stream produced %zu vs ref %zu",
      total_out, ref_olen);

  /* Same-engine FLOAT32 reference, independently rounded: the packed path
   * must agree to within 2 LSB over every frame including the edges. */
  for (i = 0; i < total_out; ++i)
    for (c = 0; c < cfg.ch; ++c) {
      int clip;
      int32_t want = t_round24((double)ref_out[i * cfg.ch + c] *
          PCM24_FULL_SCALE, &clip);
      int32_t got = pcm_get(out, (unsigned)c, i, t_is_be(cfg.otype));
      long d = labs((long)got - (long)want);
      CHECK(d <= 2, "stream audio drift %ld LSB f=%zu ch=%u (types %d/%d)",
          d, i, (unsigned)c, (int)cfg.itype, (int)cfg.otype);
    }
  pcm_check_guards(in);
  pcm_check_guards(out);
  soxr_delete(p);
  pcm_free(in);
  pcm_free(out);
}

static void test_streaming_all_layouts(void)
{
  enum { ILEN = 2500 };
  double const irate = 44100., orate = 48000.;
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_20_BITQ, 0);
  static unsigned const ch_tab[3] = {1, 2, 6};
  static soxr_datatype_t const types[4] = {
    SOXR_INT24_LE_I, SOXR_INT24_BE_I, SOXR_INT24_LE_S, SOXR_INT24_BE_S};
  static int const crosses[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
  unsigned k, x;

  for (k = 0; k < 3; ++k) {
    unsigned ch = ch_tab[k];
    int32_t * src = xmalloc(ILEN * ch * sizeof(*src));
    float * ref_in = xmalloc(ILEN * ch * sizeof(*ref_in));
    size_t ref_olen, i;
    float * ref_out;
    make_pcm_source(src, ILEN, ch, irate, 3);
    for (i = 0; i < ILEN * ch; ++i)
      ref_in[i] = (float)((double)src[i] / PCM24_FULL_SCALE);
    ref_out = ref_resample32(ref_in, ILEN, ch, irate, orate, q, 1., &ref_olen);
    for (x = 0; x < 4; ++x) {
      stream_cfg cfg;
      CASE();
      cfg.itype = types[crosses[x][0]];
      cfg.otype = types[crosses[x][1]];
      cfg.ch = ch;
      cfg.irate = irate;
      cfg.orate = orate;
      cfg.q = q;
      run_streaming(cfg, src, ILEN, ref_out, ref_olen);
    }
    free(ref_out);
    free(ref_in);
    free(src);
  }
}



/* ---------------- 8. Input callback, flush and clear/reuse ---------------- */

typedef struct {
  unsigned ch;
  int split;
  size_t total, pos;
  pcm_buf * buf;
} cb_state;

static size_t input_callback(void * state0, soxr_in_t * data, size_t requested)
{
  cb_state * st = (cb_state *)state0;
  size_t remain = st->total - st->pos;
  size_t n = requested < remain? requested : remain;
  unsigned c;
  if (!n) {
    /* End of input: data left non-NULL, length zero. */
    if (st->split)
      *data = st->buf->base;
    else
      *data = st->buf->base[0];
    return 0;
  }
  if (st->split) {
    static __thread unsigned char * pp[8];
    for (c = 0; c < st->ch; ++c)
      pp[c] = pcm_ptr(st->buf, c, st->pos);
    *data = pp;
  } else
    *data = pcm_ptr(st->buf, 0, st->pos);
  st->pos += n;
  return n;
}

static void test_callback_flush_clear(void)
{
  enum { ILEN = 2000, OCAP = 7 };
  double const irate = 48000., orate = 32000.;
  unsigned ch = 6;
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_20_BITQ, 0);
  int32_t * src = xmalloc(ILEN * ch * sizeof(*src));
  float * ref_in = xmalloc(ILEN * ch * sizeof(*ref_in));
  float * ref_out;
  size_t ref_olen, i, c, total = 0;
  pcm_buf * in = pcm_alloc(ch, ILEN, 0);                 /* interleaved LE */
  pcm_buf * out1 = pcm_alloc(ch, ILEN + 1024, 1);       /* split BE */
  pcm_buf * out2 = pcm_alloc(ch, ILEN + 1024, 1);
  soxr_io_spec_t io = soxr_io_spec(SOXR_INT24_LE_I, SOXR_INT24_BE_S);
  soxr_error_t e;
  soxr_t p;
  cb_state st;

  CASE();
  make_pcm_source(src, ILEN, ch, irate, 4);
  for (i = 0; i < ILEN * ch; ++i)
    ref_in[i] = (float)((double)src[i] / PCM24_FULL_SCALE);
  ref_out = ref_resample32(ref_in, ILEN, ch, irate, orate, q, 1., &ref_olen);
  for (i = 0; i < ILEN; ++i)
    for (c = 0; c < ch; ++c)
      pcm_put(in, c, i, src[i * ch + c], 0);

  p = soxr_create(irate, orate, ch, &e, &io, &q, 0);
  CHECK(p && !e, "cb create: %s", soxr_strerror(e));
  st.ch = ch; st.split = 0; st.total = ILEN; st.pos = 0; st.buf = in;
  e = soxr_set_input_fn(p, (soxr_input_fn_t)input_callback, &st, 13);
  CHECK(!e, "set_input_fn");

  for (;;) {
    unsigned char * pp[8];
    size_t odone, f;
    for (c = 0; c < ch; ++c)
      pp[c] = pcm_ptr(out1, (unsigned)c, total);
    odone = soxr_output(p, pp, OCAP);
    total += odone;
    if (!odone) break;
    for (f = total - odone; f < total; ++f)
      pcm_check_guards(out1);
  }
  CHECK(!soxr_error(p), "callback run error: %s", soxr_strerror(soxr_error(p)));
  CHECK(total == ref_olen, "callback produced %zu vs ref %zu", total, ref_olen);
  for (i = 0; i < total; ++i)
    for (c = 0; c < ch; ++c) {
      int clip;
      int32_t want = t_round24((double)ref_out[i * ch + c] *
          PCM24_FULL_SCALE, &clip);
      int32_t got = pcm_get(out1, (unsigned)c, i, 1);
      CHECK(labs((long)got - (long)want) <= 2, "callback audio f=%zu ch=%u",
          i, (unsigned)c);
    }
  pcm_check_guards(in);
  pcm_check_guards(out1);

  /* Reuse after clear: same input must give byte-identical packed output. */
  CASE();
  e = soxr_clear(p);
  CHECK(!e, "clear");
  st.pos = 0;
  total = 0;
  for (;;) {
    unsigned char * pp[8];
    size_t odone;
    for (c = 0; c < ch; ++c)
      pp[c] = pcm_ptr(out2, (unsigned)c, total);
    odone = soxr_output(p, pp, OCAP);
    total += odone;
    if (!odone) break;
  }
  CHECK(total == ref_olen, "rerun produced %zu vs ref %zu", total, ref_olen);
  for (c = 0; c < ch; ++c)
    CHECK(memcmp(out2->base[c], out1->base[c], total * 3) == 0,
        "output differs after clear (ch %zu)", (size_t)c);
  pcm_check_guards(out2);
  soxr_delete(p);

  pcm_free(in);
  pcm_free(out1);
  pcm_free(out2);
  free(ref_out);
  free(ref_in);
  free(src);
}



/* ------ 9. Scale is audio gain (not a byte shift) plus per-sample clips --- */

static void test_scale_and_clips(void)
{
  enum { ILEN = 1600, SETTLE = 120 };
  double const rate = 48000.;
  unsigned ch = 6;
  static double const amps[6] = {.99, .99, .2, .2, .62, .62};
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_28_BITQ, 0);
  int32_t * src = xmalloc(ILEN * ch * sizeof(*src));
  double * d_in = xmalloc(ILEN * ch * sizeof(*d_in));
  double * d_out;
  size_t d_olen, i, c;
  pcm_buf * in = pcm_alloc(ch, ILEN, 0);
  pcm_buf * out = pcm_alloc(ch, ILEN + 64, 1);
  soxr_io_spec_t io = soxr_io_spec(SOXR_INT24_LE_I, SOXR_INT24_BE_S);
  size_t idone = 0, odone = 0, expect_clips = 0;
  soxr_error_t e;

  CASE();
  io.scale = 2.0;                         /* Audio gain, applied in engine. */
  for (i = 0; i < ILEN; ++i)
    for (c = 0; c < ch; ++c) {
      double s = amps[c] * sin(2 * PI * (313 + 37 * c) * i / rate + c);
      src[i * ch + c] = t_norm_to_pcm24(s);
      d_in[i * ch + c] = (double)src[i * ch + c] / PCM24_FULL_SCALE;
      pcm_put(in, c, i, src[i * ch + c], 0);
    }
  /* Independent reference: float64 path with the same gain, same engine. */
  d_out = ref_resample64(d_in, ILEN, ch, rate, rate, q, 2.0, &d_olen);

  e = soxr_oneshot(rate, rate, ch, in->base[0], ILEN, &idone, out->base,
      out->frames, &odone, &io, &q, 0);
  CHECK(!e && idone == ILEN && odone == d_olen, "clip oneshot counts");
  for (i = 0; i < odone; ++i)
    for (c = 0; c < ch; ++c) {
      int clip = 0;
      int32_t want = t_round24(d_out[i * ch + c] * PCM24_FULL_SCALE, &clip);
      int32_t got = pcm_get(out, (unsigned)c, i, 1);
      expect_clips += (size_t)clip;
      CHECK(got == want, "amplified mismatch f=%zu ch=%u got %d want %d",
          i, (unsigned)c, (int)got, (int)want);
      if (clip)
        CHECK(want == PCM24_MAX || want == PCM24_MIN, "clip not saturated");
    }
  CHECK(expect_clips > 0, "test signal produced no clips");
  /* Per-sample, not per-frame: with six channels some frames clip on only
   * some channels, so the count must exceed the number of clipping frames. */
  {
    size_t clip_frames = 0;
    for (i = 0; i < odone; ++i) {
      int any = 0;
      for (c = 0; c < ch; ++c) {
        int clip;
        (void)t_round24(d_out[i * ch + c] * PCM24_FULL_SCALE, &clip);
        if (clip) any = 1;
      }
      clip_frames += (size_t)any;
    }
    CHECK(expect_clips > clip_frames,
        "clips=%zu look per-frame rather than per-sample (frames=%zu)",
        expect_clips, clip_frames);
  }
  {
    soxr_t p = soxr_create(rate, rate, ch, &e, &io, &q, 0);
    CHECK(p && !e, "clip create");
    CHECK(*soxr_num_clips(p) == 0, "fresh clip counter non-zero");
    soxr_delete(p);
  }
  /* The oneshot above reported its clips through its internal counter;
   * verify a streamed run reports the same per-sample total. */
  {
    soxr_t p;
    size_t pos = 0, tot = 0;
    io = soxr_io_spec(SOXR_INT24_LE_I, SOXR_INT24_BE_S);
    io.scale = 2.0;
    p = soxr_create(rate, rate, ch, &e, &io, &q, 0);
    CHECK(p && !e, "stream clip create");
    while (pos < ILEN) {
      size_t n = ILEN - pos;
      unsigned char * pp[8];
      size_t id1 = 0, od1 = 0;
      if (n > 37) n = 37;
      for (c = 0; c < ch; ++c)
        pp[c] = pcm_ptr(out, (unsigned)c, tot);
      e = soxr_process(p, pcm_ptr(in, 0, pos), n, &id1, pp, 11, &od1);
      CHECK(!e, "clip stream process");
      pos += id1;
      tot += od1;
    }
    for (;;) {
      unsigned char * pp[8];
      size_t od1 = 0;
      for (c = 0; c < ch; ++c)
        pp[c] = pcm_ptr(out, (unsigned)c, tot);
      e = soxr_process(p, 0, 0, 0, pp, 11, &od1);
      CHECK(!e, "clip flush");
      tot += od1;
      if (!od1) break;
    }
    CHECK(tot == odone, "clip stream frame count %zu vs %zu", tot, odone);
    CHECK(*soxr_num_clips(p) == expect_clips,
        "num_clips=%zu want %zu", (size_t)*soxr_num_clips(p), expect_clips);
    soxr_delete(p);
  }

  /* Gain semantics on un-clipped constants: 1000 * 0.25 -> 250, *2 -> 2000;
   * a byte-shift implementation could never produce these numbers. */
  {
    static int32_t const kv = 1000;
    size_t n = 400, k;
    pcm_buf * ci = pcm_alloc(1, n, 0), * co = pcm_alloc(1, n + 16, 0);
    double gains[2] = {0.25, 2.0};
    int32_t want[2] = {250, 2000};
    int g;
    for (k = 0; k < n; ++k) pcm_put(ci, 0, k, kv, 0);
    for (g = 0; g < 2; ++g) {
      soxr_io_spec_t g_io = soxr_io_spec(SOXR_INT24_LE_I, SOXR_INT24_LE_I);
      size_t id1 = 0, od1 = 0;
      g_io.scale = gains[g];
      e = soxr_oneshot(rate, rate, 1, ci->base[0], n, &id1, co->base[0],
          co->frames, &od1, &g_io, &q, 0);
      CHECK(!e && id1 == n, "gain oneshot");
      for (k = SETTLE; k + SETTLE < od1; ++k)
        CHECK(pcm_get(co, 0, k, 0) == want[g],
            "gain %.2f: got %d want %d", gains[g],
            (int)pcm_get(co, 0, k, 0), (int)want[g]);
    }
    pcm_free(ci);
    pcm_free(co);
  }
  pcm_check_guards(in);
  pcm_check_guards(out);
  pcm_free(in);
  pcm_free(out);
  free(d_out);
  free(d_in);
  free(src);
}



/* ----------- 10. Packed 24 output is never dithered; int16 is ------------- */

static void test_dither_policy(void)
{
  enum { ILEN = 4096 };
  double const rate = 48000.;
  int32_t * src = xmalloc(ILEN * sizeof(*src));
  pcm_buf * a = pcm_alloc(1, ILEN + 256, 0);
  pcm_buf * b = pcm_alloc(1, ILEN + 256, 0);
  int16_t * s1 = xmalloc((ILEN + 256) * sizeof(*s1));
  int16_t * s2 = xmalloc((ILEN + 256) * sizeof(*s2));
  size_t i, oa = 0, ob = 0, id = 0;
  unsigned differs = 0;
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_20_BITQ, 0);
  soxr_io_spec_t io24 = soxr_io_spec(SOXR_INT24_LE_I, SOXR_INT24_LE_I);
  soxr_io_spec_t io16 = soxr_io_spec(SOXR_INT24_LE_I, SOXR_INT16_I);
  soxr_error_t e;

  CASE();
  /* Noise-like signal so dithering (if it were applied) would show. */
  for (i = 0; i < ILEN; ++i) {
    double s = 0.9 * sin(2 * PI * 1013 * i / rate) *
        sin(2 * PI * (7 + (i % 23)) * i / rate);
    src[i] = t_norm_to_pcm24(s);
    pcm_put(a, 0, i, src[i], 0);
    pcm_put(b, 0, i, src[i], 0);
  }
  e = soxr_oneshot(rate, rate * 1.05, 1, a->base[0], ILEN, &id,
      a->base[0], a->frames, &oa, &io24, &q, 0);
  CHECK(!e, "dither24 a");
  e = soxr_oneshot(rate, rate * 1.05, 1, b->base[0], ILEN, &id,
      b->base[0], b->frames, &ob, &io24, &q, 0);
  CHECK(!e, "dither24 b");
  CHECK(oa == ob, "pcm24 run lengths differ");
  CHECK(memcmp(a->base[0], b->base[0], oa * 3) == 0,
      "packed 24-bit output was dithered (runs differ)");

  /* Default int16 path remains dithered: two resamplers that are alive at the
   * same time have distinct seeds (the seed is the time XOR the object
   * address), so their dithered outputs must differ somewhere. */
  {
    soxr_t p1 = soxr_create(rate, rate * 1.05, 1, &e, &io16, &q, 0);
    soxr_t p2 = soxr_create(rate, rate * 1.05, 1, &e, &io16, &q, 0);
    size_t id1 = 0, od1 = 0, id2 = 0, od2 = 0, od1f = 0, od2f = 0;
    CHECK(p1 && p2 && !e, "dither create");
    e = soxr_process(p1, a->base[0], ILEN, &id1, s1, ILEN + 256, &od1);
    CHECK(!e && id1 == ILEN, "dither16 p1 process");
    e = soxr_process(p2, b->base[0], ILEN, &id2, s2, ILEN + 256, &od2);
    CHECK(!e && id2 == ILEN, "dither16 p2 process");
    /* Drain the filter tails. */
    e = soxr_process(p1, 0, 0, 0, s1 + od1, 256, &od1f);
    CHECK(!e, "dither16 p1 flush");
    e = soxr_process(p2, 0, 0, 0, s2 + od2, 256, &od2f);
    CHECK(!e, "dither16 p2 flush");
    oa = od1 + od1f;
    ob = od2 + od2f;
    CHECK(oa == ob, "int16 run lengths differ");
    for (i = 0; i < oa; ++i)
      differs += s1[i] != s2[i];
    CHECK(differs > 0, "int16 dither seems disabled (identical outputs)");
    soxr_delete(p1);
    soxr_delete(p2);
  }

  pcm_check_guards(a);
  pcm_check_guards(b);
  pcm_free(a);
  pcm_free(b);
  free(s1);
  free(s2);
  free(src);
}



/* ------------------- 11. Variable-rate (SOXR_VR) path --------------------- */

static void test_variable_rate(void)
{
  enum { ILEN = 2000 };
  double const irate = 48000., orate = 44100.;
  unsigned ch = 2;
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_LQ, SOXR_VR);
  int32_t * src = xmalloc(ILEN * ch * sizeof(*src));
  float * f_in = xmalloc(ILEN * ch * sizeof(*f_in));
  float * f_ref;
  size_t ref_olen, i, c, idone = 0, odone = 0;
  pcm_buf * in = pcm_alloc(ch, ILEN, 1);
  pcm_buf * out = pcm_alloc(ch, ILEN + 1024, 0);
  soxr_io_spec_t io = soxr_io_spec(SOXR_INT24_LE_S, SOXR_INT24_BE_I);
  soxr_error_t e;
  soxr_t p;

  CASE();
  make_pcm_source(src, ILEN, ch, irate, 5);
  for (i = 0; i < ILEN * ch; ++i)
    f_in[i] = (float)((double)src[i] / PCM24_FULL_SCALE);
  f_ref = ref_resample32(f_in, ILEN, ch, irate, orate, q, 1., &ref_olen);

  for (i = 0; i < ILEN; ++i)
    for (c = 0; c < ch; ++c)
      pcm_put(in, c, i, src[i * ch + c], 0);
  p = soxr_create(irate, orate, ch, &e, &io, &q, 0);
  CHECK(p && !e, "vr create: %s", soxr_strerror(e));
  CHECK(strcmp(soxr_engine(p), "vr32") == 0, "VR must use vr32 engine");
  e = soxr_oneshot(irate, orate, ch, in->base, ILEN, &idone,
      out->base[0], out->frames, &odone, &io, &q, 0);
  CHECK(!e && idone == ILEN, "vr oneshot");
  CHECK(odone == ref_olen, "vr odone %zu vs ref %zu", odone, ref_olen);
  for (i = 0; i < odone; ++i)
    for (c = 0; c < ch; ++c) {
      int clip;
      int32_t want = t_round24((double)f_ref[i * ch + c] * PCM24_FULL_SCALE,
          &clip);
      int32_t got = pcm_get(out, c, i, 1);
      CHECK(labs((long)got - (long)want) <= 2, "vr audio drift f=%zu ch=%u",
          i, (unsigned)c);
    }
  pcm_check_guards(in);
  pcm_check_guards(out);
  soxr_delete(p);

  /* Mid-stream ratio change is the VR-specific feature; exercise it. */
  CASE();
  p = soxr_create(irate, orate, ch, &e, &io, &q, 0);
  CHECK(p && !e, "vr recreate");
  e = soxr_set_io_ratio(p, irate / (orate * 1.2), 64);
  CHECK(!e, "set_io_ratio: %s", soxr_strerror(e));
  idone = 0; odone = 0;
  e = soxr_process(p, in->base, 256, &idone, out->base[0], 512, &odone);
  CHECK(!e, "vr process after ratio change");
  CHECK(odone > 0, "vr produced no output after ratio change");
  e = soxr_set_io_ratio(p, irate / (orate * 0.8), 64);
  CHECK(!e, "second set_io_ratio");
  soxr_delete(p);
  pcm_free(in);
  pcm_free(out);
  free(f_ref);
  free(f_in);
  free(src);
}



/* ---------------------- 12. Legacy format control group ------------------- */

static void test_legacy_control(void)
{
  enum { N = 4410, M = 6000 };
  static float fin[N], fout[M];
  double f64in[N], f64out[M];
  int32_t i32in[N], i32out[M];
  int16_t i16in[N], i16out[M];
  size_t i, id = 0, od = 0;
  double energy = 0;
  soxr_io_spec_t io;
  soxr_error_t e;
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_LQ, 0);

  CASE();
  for (i = 0; i < N; ++i) {
    float s = (float)(.25 * sin(2 * PI * 1000 * i / 44100.));
    fin[i] = s;
    f64in[i] = s;
    i32in[i] = (int32_t)(s * 2147483648.);
    i16in[i] = (int16_t)(s * 32768.);
  }
  io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
  e = soxr_oneshot(44100, 48000, 1, fin, N, &id, fout, M, &od, &io, &q, 0);
  CHECK(!e && id == N && od == 4800, "legacy f32");
  for (i = 0; i < od; ++i) energy += (double)fout[i] * fout[i];
  CHECK(energy > 100., "legacy f32 energy %.3f", energy);

  CASE();
  io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
  id = od = 0;
  e = soxr_oneshot(44100, 48000, 1, f64in, N, &id, f64out, M, &od, &io, &q, 0);
  CHECK(!e && id == N && od == 4800, "legacy f64");

  CASE();
  io = soxr_io_spec(SOXR_INT32_I, SOXR_INT32_I);
  id = od = 0;
  e = soxr_oneshot(44100, 48000, 1, i32in, N, &id, i32out, M, &od, &io, &q, 0);
  CHECK(!e && id == N && od == 4800, "legacy i32");

  CASE();
  io = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
  id = od = 0;
  e = soxr_oneshot(44100, 48000, 1, i16in, N, &id, i16out, M, &od, &io, &q, 0);
  CHECK(!e && id == N && od == 4800, "legacy i16");
}



/* ---- 13. No address-alignment requirement (deliberately odd pointers) ---- */

static void test_unaligned_buffers(void)
{
  enum { ILEN = 1200 };
  double const irate = 48000., orate = 32000.;
  unsigned ch = 2;
  soxr_quality_spec_t q = soxr_quality_spec(SOXR_20_BITQ, 0);
  static size_t const offsets[3] = {1, 2, 3};
  static soxr_datatype_t const types[4] = {
    SOXR_INT24_LE_I, SOXR_INT24_BE_I, SOXR_INT24_LE_S, SOXR_INT24_BE_S};
  int32_t * src = xmalloc(ILEN * ch * sizeof(*src));
  float * f_in = xmalloc(ILEN * ch * sizeof(*f_in));
  size_t ref_olen, i, c, o, t;
  float * ref_out;

  make_pcm_source(src, ILEN, ch, irate, 6);
  for (i = 0; i < ILEN * ch; ++i)
    f_in[i] = (float)((double)src[i] / PCM24_FULL_SCALE);
  ref_out = ref_resample32(f_in, ILEN, ch, irate, orate, q, 1., &ref_olen);

  for (t = 0; t < 4; ++t) for (o = 0; o < 3; ++o) {
    int split = t_is_split(types[t]);
    int be = t_is_be(types[t]);
    size_t off = offsets[o];
    /* Oversized block so the deliberately odd start address stays in range
     * for every frame, including both per-channel split segments. */
    unsigned char * raw_in = xmalloc((size_t)ch * ILEN * 3 + 16);
    unsigned char * raw_out = xmalloc((size_t)ch * (ref_olen + 8) * 3 + 16);
    unsigned char * in_base = raw_in + off;
    unsigned char * out_base = raw_out + off;
    size_t idone = 0, odone = 0;
    soxr_io_spec_t io = soxr_io_spec(types[t], types[t]);
    soxr_error_t e;
    void * in_arg, * out_arg;
    unsigned char * ip[8], * op[8];   /* Keep both live through the call. */
    CASE();

    /* Force a non-4-byte-aligned base (off is 1..3); assert the premise. */
    CHECK(((uintptr_t)in_base & 3u) == (off & 3u) && off != 0,
        "alignment premise");

    if (split) {
      for (c = 0; c < ch; ++c) {
        ip[c] = in_base + c * ILEN * 3;
        op[c] = out_base + c * (ref_olen + 8) * 3;
      }
      for (i = 0; i < ILEN; ++i)
        for (c = 0; c < ch; ++c)
          t_pack24(ip[c] + 3 * i, src[i * ch + c], be);
      in_arg = ip;
      out_arg = op;
    } else {
      for (i = 0; i < ILEN; ++i)
        for (c = 0; c < ch; ++c)
          t_pack24(in_base + 3 * (i * ch + c), src[i * ch + c], be);
      in_arg = in_base;
      out_arg = out_base;
    }

    e = soxr_oneshot(irate, orate, ch, in_arg, ILEN, &idone, out_arg,
        ref_olen + 8, &odone, &io, &q, 0);
    CHECK(!e && idone == ILEN && odone == ref_olen, "unaligned oneshot");
    for (i = 0; i < odone; ++i)
      for (c = 0; c < ch; ++c) {
        int clip;
        int32_t want = t_round24((double)ref_out[i * ch + c] *
            PCM24_FULL_SCALE, &clip);
        unsigned char const * p = split?
            ((unsigned char * const *)out_arg)[c] + 3 * i :
            (unsigned char *)out_arg + 3 * (i * ch + c);
        int32_t got = t_unpack24(p, be);
        CHECK(labs((long)got - (long)want) <= 2,
            "unaligned audio type=%d off=%zu f=%zu ch=%u",
            (int)types[t], off, i, (unsigned)c);
      }
    free(raw_in);
    free(raw_out);
  }
  free(ref_out);
  free(f_in);
  free(src);
}



int main(void)
{
  test_enums_and_validation();
  test_pack_round_reference();
  test_unit_ratio_all_bits();
  test_legacy_cross_types();
  test_mixed_split_layouts();
  test_double_engine_exact();
  test_single_precision_tolerance();
  test_streaming_all_layouts();
  test_callback_flush_clear();
  test_scale_and_clips();
  test_dither_policy();
  test_variable_rate();
  test_legacy_control();
  test_unaligned_buffers();
  printf("SOXR_PCM24_OK cases=%d\n", g_cases);
  return 0;
}
