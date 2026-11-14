/* SoX Resampler Library      Copyright (c) 2007-16 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

#include <limits.h>
#include <math.h>
#include <string.h>

#include "data-io.h"
#include "internal.h"



/* Packed, signed 24-bit PCM: each sample is exactly 3 bytes, with no
 * alignment requirement on the containing buffer.  Samples enter and leave
 * the resampler in 24-bit LSB units; the engine's scale multiplier handles
 * the full-scale ratio, as for the 16- and 32-bit integer types. */

static int32_t unpack_pcm24(unsigned char const * p, int big_endian)
{
  uint32_t u = big_endian?
      ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]
    : (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
  if (u & 0x800000u)                         /* Sign-extend from 24 bits. */
    u |= 0xff000000u;
  return (int32_t)u;
}

static void pack_pcm24(unsigned char * p, int32_t v, int big_endian)
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

#define DEINTERLEAVE_PCM24(BIG) do { \
  unsigned char const * s = *src0; \
  unsigned i; \
  size_t j; \
  if (ch > 1) for (j = 0; j < n; ++j) \
    for (i = 0; i < ch; ++i) \
      dest[i][j] = (DEINTERLEAVE_TO)unpack_pcm24(s, BIG), s += 3; \
  else for (j = 0; j < n; ++j) \
    dest[0][j] = (DEINTERLEAVE_TO)unpack_pcm24(s, BIG), s += 3; \
  *src0 = s; \
} while (0)



#define DEINTERLEAVE_FROM(T,flag) do { \
  unsigned i; \
  size_t j; \
  T const * src = *src0; \
  if (ch > 1) for (j = 0; j < n; ++j) \
    for (i = 0; i < ch; ++i) dest[i][j] = (DEINTERLEAVE_TO)*src++; \
  else if (flag) memcpy(dest[0], src, n * sizeof(T)), src = &src[n]; \
  else for (j = 0; j < n; dest[0][j++] = (DEINTERLEAVE_TO)*src++); \
  *src0 = src; \
} while (0)



#if WITH_CR64 || WITH_CR64S
void _soxr_deinterleave(double * * dest, /* Round/clipping not needed here */
    soxr_datatype_t data_type, void const * * src0, size_t n, unsigned ch)
{
#define DEINTERLEAVE_TO double
  switch (data_type) {
    case SOXR_FLOAT32_I: case SOXR_FLOAT32_S:
      DEINTERLEAVE_FROM(float, 0); break;
    case SOXR_FLOAT64_I: case SOXR_FLOAT64_S:
      DEINTERLEAVE_FROM(double, 1); break;
    case SOXR_INT32_I: case SOXR_INT32_S:
      DEINTERLEAVE_FROM(int32_t, 0); break;
    case SOXR_INT16_I: case SOXR_INT16_S:
      DEINTERLEAVE_FROM(int16_t, 0); break;
    case SOXR_INT24_LE_I: case SOXR_INT24_LE_S:
      DEINTERLEAVE_PCM24(0); break;
    case SOXR_INT24_BE_I: case SOXR_INT24_BE_S:
      DEINTERLEAVE_PCM24(1); break;
    default: break;
  }
}
#endif



#if WITH_CR32 || WITH_CR32S || WITH_VR32
void _soxr_deinterleave_f(float * * dest, /* Round/clipping not needed here */
    soxr_datatype_t data_type, void const * * src0, size_t n, unsigned ch)
{
#undef DEINTERLEAVE_TO
#define DEINTERLEAVE_TO float
  switch (data_type) {
    case SOXR_FLOAT32_I: case SOXR_FLOAT32_S:
      DEINTERLEAVE_FROM(float, 1); break;
    case SOXR_FLOAT64_I: case SOXR_FLOAT64_S:
      DEINTERLEAVE_FROM(double, 0); break;
    case SOXR_INT32_I: case SOXR_INT32_S:
      DEINTERLEAVE_FROM(int32_t, 0); break;
    case SOXR_INT16_I: case SOXR_INT16_S:
      DEINTERLEAVE_FROM(int16_t, 0); break;
    case SOXR_INT24_LE_I: case SOXR_INT24_LE_S:
      DEINTERLEAVE_PCM24(0); break;
    case SOXR_INT24_BE_I: case SOXR_INT24_BE_S:
      DEINTERLEAVE_PCM24(1); break;
    default: break;
  }
}
#endif



#include "rint.h"



#if defined FE_INVALID && defined FPU_RINT32 && defined __STDC_VERSION__
  #if __STDC_VERSION__ >= 199901L
    #pragma STDC FENV_ACCESS ON
  #endif
#endif

#if WITH_CR64 || WITH_CR64S
#define FLOATX double

#define LSX_RINT_CLIP_2 lsx_rint32_clip_2
#define LSX_RINT_CLIP lsx_rint32_clip
#define RINT_CLIP rint32_clip
#define RINT rint32D
#if defined FPU_RINT32
  #define FPU_RINT
#endif
#define RINT_T int32_t
#define RINT_MAX 2147483647L
#include "rint-clip.h"

#define LSX_RINT_CLIP_2 lsx_rint16_clip_2
#define LSX_RINT_CLIP lsx_rint16_clip
#define RINT_CLIP rint16_clip
#define RINT rint16D
#if defined FPU_RINT16
  #define FPU_RINT
#endif
#define RINT_T int16_t
#define RINT_MAX 32767
#include "rint-clip.h"

#define LSX_RINT_CLIP_2 lsx_rint16_clip_2_dither
#define LSX_RINT_CLIP lsx_rint16_clip_dither
#define RINT_CLIP rint16_clip_dither
#define RINT rint16D
#if defined FPU_RINT16
  #define FPU_RINT
#endif
#define RINT_T int16_t
#define RINT_MAX 32767
#define DITHER
#include "rint-clip.h"

#undef FLOATX
#endif



#if WITH_CR32 || WITH_CR32S || WITH_VR32
#define FLOATX float

#define LSX_RINT_CLIP_2 lsx_rint32_clip_2_f
#define LSX_RINT_CLIP lsx_rint32_clip_f
#define RINT_CLIP rint32_clip_f
#define RINT rint32F
#if defined FPU_RINT32
  #define FPU_RINT
#endif
#define RINT_T int32_t
#define RINT_MAX 2147483647L
#include "rint-clip.h"

#define LSX_RINT_CLIP_2 lsx_rint16_clip_2_f
#define LSX_RINT_CLIP lsx_rint16_clip_f
#define RINT_CLIP rint16_clip_f
#define RINT rint16F
#if defined FPU_RINT16
  #define FPU_RINT
#endif
#define RINT_T int16_t
#define RINT_MAX 32767
#include "rint-clip.h"

#define LSX_RINT_CLIP_2 lsx_rint16_clip_2_dither_f
#define LSX_RINT_CLIP lsx_rint16_clip_dither_f
#define RINT_CLIP rint16_clip_dither_f
#define RINT rint16D
#if defined FPU_RINT16
  #define FPU_RINT
#endif
#define RINT_T int16_t
#define RINT_MAX 32767
#define DITHER
#include "rint-clip.h"

#undef FLOATX
#endif

#if defined FE_INVALID && defined FPU_RINT32 && defined __STDC_VERSION__
  #if __STDC_VERSION__ >= 199901L
    #pragma STDC FENV_ACCESS OFF
  #endif
#endif



#define INTERLEAVE_TO(T,flag) do { \
  unsigned i; \
  size_t j; \
  T * dest = *dest0; \
  if (ch > 1) \
  for (j = 0; j < n; ++j) for (i = 0; i < ch; ++i) *dest++ = (T)src[i][j]; \
  else if (flag) memcpy(dest, src[0], n * sizeof(T)), dest = &dest[n]; \
  else for (j = 0; j < n; *dest++ = (T)src[0][j++]); \
  *dest0 = dest; \
  return 0; \
} while (0)



/* Packed 24-bit output: round half away from zero to 24-bit LSB units,
 * then saturate to [-8388608, 8388607].  No dithering is applied; every
 * saturated sample is counted individually. */

#define RINT24_MAX 8388607L
#define RINT24_MIN (-8388607L - 1)

#define DEFINE_RINT24(SUFFIX, FLOAT_TYPE) \
static int32_t rint24_clip##SUFFIX(FLOAT_TYPE x0, size_t * clips) \
{ \
  double x = (double)x0, a, r; \
  if (x != x) {                     /* NaN: saturate to the negative rail. */ \
    ++*clips; \
    return (int32_t)RINT24_MIN; \
  } \
  a = fabs(x); \
  r = floor(a + .5);               /* Half away from zero. */ \
  /* +8388608 cannot be represented, but -8388608 can: */ \
  if (r > 8388608. || (r == 8388608. && x > 0)) { \
    ++*clips; \
    return x > 0? (int32_t)RINT24_MAX : (int32_t)RINT24_MIN; \
  } \
  return (int32_t)(x < 0? -r : r); \
} \
static size_t lsx_rint24_clip##SUFFIX(void * * dest0, FLOAT_TYPE const * src, \
    size_t n, int big_endian) \
{ \
  size_t i, clips = 0; \
  unsigned char * dest = *dest0; \
  for (i = 0; i < n; ++i) \
    pack_pcm24(dest + 3 * i, rint24_clip##SUFFIX(src[i], &clips), big_endian); \
  *dest0 = dest + 3 * n; \
  return clips; \
} \
static size_t lsx_rint24_clip_2##SUFFIX(void * * dest0, \
    FLOAT_TYPE const * const * srcs, unsigned ch, size_t n, int big_endian) \
{ \
  unsigned j; \
  size_t i, clips = 0; \
  unsigned char * dest = *dest0; \
  for (i = 0; i < n; ++i) \
    for (j = 0; j < ch; ++j) \
      pack_pcm24(dest + 3 * ((size_t)ch * i + j), \
          rint24_clip##SUFFIX(srcs[j][i], &clips), big_endian); \
  *dest0 = dest + 3 * (size_t)ch * n; \
  return clips; \
}

#if WITH_CR64 || WITH_CR64S
DEFINE_RINT24(_d, double)

size_t /* clips */ _soxr_interleave(soxr_datatype_t data_type, void * * dest0,
  double const * const * src, size_t n, unsigned ch, unsigned long * seed)
{
  (void)seed; /* Packed 24-bit output is never dithered. */
  switch (data_type) {
    case SOXR_FLOAT32_I: case SOXR_FLOAT32_S: INTERLEAVE_TO(float, 0);
    case SOXR_FLOAT64_I: case SOXR_FLOAT64_S: INTERLEAVE_TO(double, 1);

    case SOXR_INT32_I: case SOXR_INT32_S: if (ch == 1)
        return lsx_rint32_clip(dest0, src[0], n);
      return lsx_rint32_clip_2(dest0, src, ch, n);

    case SOXR_INT16_I: case SOXR_INT16_S: if (seed) {
      if (ch == 1)
        return lsx_rint16_clip_dither(dest0, src[0], n, seed);
      return lsx_rint16_clip_2_dither(dest0, src, ch, n, seed);
    }
    if (ch == 1)
        return lsx_rint16_clip(dest0, src[0], n);
      return lsx_rint16_clip_2(dest0, src, ch, n);

    case SOXR_INT24_LE_I: case SOXR_INT24_LE_S:
      return ch == 1? lsx_rint24_clip_d(dest0, src[0], n, 0)
                    : lsx_rint24_clip_2_d(dest0, src, ch, n, 0);
    case SOXR_INT24_BE_I: case SOXR_INT24_BE_S:
      return ch == 1? lsx_rint24_clip_d(dest0, src[0], n, 1)
                    : lsx_rint24_clip_2_d(dest0, src, ch, n, 1);
    default: break;
  }
  return 0;
}
#endif

#if WITH_CR32 || WITH_CR32S || WITH_VR32
DEFINE_RINT24(_f, float)

size_t /* clips */ _soxr_interleave_f(soxr_datatype_t data_type, void * * dest0,
  float const * const * src, size_t n, unsigned ch, unsigned long * seed)
{
  (void)seed; /* Packed 24-bit output is never dithered. */
  switch (data_type) {
    case SOXR_FLOAT32_I: case SOXR_FLOAT32_S: INTERLEAVE_TO(float, 1);
    case SOXR_FLOAT64_I: case SOXR_FLOAT64_S: INTERLEAVE_TO(double, 0);

    case SOXR_INT32_I: case SOXR_INT32_S: if (ch == 1)
        return lsx_rint32_clip_f(dest0, src[0], n);
      return lsx_rint32_clip_2_f(dest0, src, ch, n);

    case SOXR_INT16_I: case SOXR_INT16_S: if (seed) {
      if (ch == 1)
        return lsx_rint16_clip_dither_f(dest0, src[0], n, seed);
      return lsx_rint16_clip_2_dither_f(dest0, src, ch, n, seed);
    }
    if (ch == 1)
        return lsx_rint16_clip_f(dest0, src[0], n);
      return lsx_rint16_clip_2_f(dest0, src, ch, n);

    case SOXR_INT24_LE_I: case SOXR_INT24_LE_S:
      return ch == 1? lsx_rint24_clip_f(dest0, src[0], n, 0)
                    : lsx_rint24_clip_2_f(dest0, src, ch, n, 0);
    case SOXR_INT24_BE_I: case SOXR_INT24_BE_S:
      return ch == 1? lsx_rint24_clip_f(dest0, src[0], n, 1)
                    : lsx_rint24_clip_2_f(dest0, src, ch, n, 1);
    default: break;
  }
  return 0;
}
#endif

#undef RINT24_MAX
#undef RINT24_MIN
