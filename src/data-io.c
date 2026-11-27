/* SoX Resampler Library      Copyright (c) 2007-16 robs@users.sourceforge.net
 * Licence for this file: LGPL v2.1                  See LICENCE for details. */

#include <limits.h>
#include <math.h>
#include <string.h>

#include "data-io.h"
#include "internal.h"



/* Packed, signed 24-bit PCM: each sample is stored in exactly 3 bytes, in
 * the requested byte order, with no alignment requirement.  The valid
 * sample range is [-8388608, 8388607] and nominal full scale is 8388608. */

static int soxr_is_pcm24(soxr_datatype_t data_type)
{
  return data_type == SOXR_INT24_LE_I || data_type == SOXR_INT24_BE_I ||
      data_type == SOXR_INT24_LE_S || data_type == SOXR_INT24_BE_S;
}

static int32_t soxr_unpack_pcm24(unsigned char const * p, int big_endian)
{
  uint32_t u = big_endian?
      ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2] :
      (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
  if (u & 0x00800000u)
    u |= 0xff000000u;                 /* Sign-extend. */
  return (int32_t)u;
}

static void soxr_pack_pcm24(unsigned char * p, int32_t x, int big_endian)
{
  uint32_t u = (uint32_t)x;           /* Caller guarantees 24-bit range. */
  if (big_endian)
    p[0] = (unsigned char)(u >> 16),
    p[1] = (unsigned char)(u >> 8),
    p[2] = (unsigned char)u;
  else
    p[0] = (unsigned char)u,
    p[1] = (unsigned char)(u >> 8),
    p[2] = (unsigned char)(u >> 16);
}

/* Round to the nearest 24-bit LSB, halves away from zero, then saturate to
 * [-8388608, 8388607]; sets `clip' non-zero when the rounded value was out
 * of range so that clipping can be counted per individual sample.  Input is
 * already in 24-bit LSB units, i.e. the engine's normalised output scaled by
 * the 2^23 full scale. */
static int32_t soxr_round_clip_pcm24(double d, int * clip)
{
  double r;
  if (d != d) {                       /* NaN: deterministic, counted clip. */
    *clip = 1;
    return 0;
  }
  r = d >= 0.? floor(d + .5) : -floor(.5 - d);
  if (r > 8388607.) {
    *clip = 1;
    return 8388607;
  }
  if (r < -8388608.) {
    *clip = 1;
    return -8388608;
  }
  *clip = 0;
  return (int32_t)r;
}

#define INTERLEAVE_TO_PCM24() do { \
  unsigned char * dest = *dest0; \
  int const big_endian1 = (data_type == SOXR_INT24_BE_I || \
      data_type == SOXR_INT24_BE_S); \
  unsigned i; \
  size_t j; \
  /* Engine output is normalised [-1, 1]; convert to 24-bit LSB units \
   * before rounding, in double so the multiplication is exact enough \
   * that every valid bit survives even on the single-precision path. */ \
  double const to_lsb = 8388608.; \
  clips = 0; \
  if (ch == 1) for (j = 0; j < n; ++j) { \
    int clip = 0; \
    int32_t v = soxr_round_clip_pcm24((double)src[0][j] * to_lsb, &clip); \
    clips += (size_t)clip; \
    soxr_pack_pcm24(dest + 3 * j, v, big_endian1); \
  } \
  else for (j = 0; j < n; ++j) for (i = 0; i < ch; ++i) { \
    int clip = 0; \
    int32_t v = soxr_round_clip_pcm24((double)src[i][j] * to_lsb, &clip); \
    clips += (size_t)clip; \
    soxr_pack_pcm24(dest + 3 * ((size_t)ch * j + i), v, big_endian1); \
  } \
  *dest0 = dest + 3 * (size_t)ch * n; \
  return clips; \
} while (0)

#define DEINTERLEAVE_FROM_PCM24(T) do { \
  unsigned char const * src = *src0; \
  int const big_endian = (data_type == SOXR_INT24_BE_I || \
      data_type == SOXR_INT24_BE_S); \
  unsigned i; \
  size_t j; \
  /* Normalise packed integers to [-1, 1] internal units: full scale, the \
   * magnitude of -2^23, is exactly 1.0. */ \
  T const norm = (T)(1. / 8388608.); \
  if (ch > 1) for (j = 0; j < n; ++j) \
    for (i = 0; i < ch; ++i) \
      dest[i][j] = (T)soxr_unpack_pcm24( \
          src + 3 * ((size_t)ch * j + i), big_endian) * norm; \
  else for (j = 0; j < n; ++j) \
    dest[0][j] = (T)soxr_unpack_pcm24(src + 3 * j, big_endian) * norm; \
  *src0 = src + 3 * (size_t)ch * n; \
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
  if (soxr_is_pcm24(data_type)) {DEINTERLEAVE_FROM_PCM24(double); return;}
  switch (data_type & 3) {
    case SOXR_FLOAT32: DEINTERLEAVE_FROM(float, 0); break;
    case SOXR_FLOAT64: DEINTERLEAVE_FROM(double, 1); break;
    case SOXR_INT32:   DEINTERLEAVE_FROM(int32_t, 0); break;
    case SOXR_INT16:   DEINTERLEAVE_FROM(int16_t, 0); break;
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
  if (soxr_is_pcm24(data_type)) {DEINTERLEAVE_FROM_PCM24(float); return;}
  switch (data_type & 3) {
    case SOXR_FLOAT32: DEINTERLEAVE_FROM(float, 1); break;
    case SOXR_FLOAT64: DEINTERLEAVE_FROM(double, 0); break;
    case SOXR_INT32:   DEINTERLEAVE_FROM(int32_t, 0); break;
    case SOXR_INT16:   DEINTERLEAVE_FROM(int16_t, 0); break;
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

#if WITH_CR64 || WITH_CR64S
size_t /* clips */ _soxr_interleave(soxr_datatype_t data_type, void * * dest0,
  double const * const * src, size_t n, unsigned ch, unsigned long * seed)
{
  if (soxr_is_pcm24(data_type)) {
    size_t clips;
    INTERLEAVE_TO_PCM24();
  }
  switch (data_type & 3) {
    case SOXR_FLOAT32: INTERLEAVE_TO(float, 0);
    case SOXR_FLOAT64: INTERLEAVE_TO(double, 1);

    case SOXR_INT32: if (ch == 1)
        return lsx_rint32_clip(dest0, src[0], n);
      return lsx_rint32_clip_2(dest0, src, ch, n);

    case SOXR_INT16: if (seed) {
      if (ch == 1)
        return lsx_rint16_clip_dither(dest0, src[0], n, seed);
      return lsx_rint16_clip_2_dither(dest0, src, ch, n, seed);
    }
    if (ch == 1)
        return lsx_rint16_clip(dest0, src[0], n);
      return lsx_rint16_clip_2(dest0, src, ch, n);
    default: break;
  }
  return 0;
}
#endif

#if WITH_CR32 || WITH_CR32S || WITH_VR32
size_t /* clips */ _soxr_interleave_f(soxr_datatype_t data_type, void * * dest0,
  float const * const * src, size_t n, unsigned ch, unsigned long * seed)
{
  if (soxr_is_pcm24(data_type)) {
    size_t clips;
    INTERLEAVE_TO_PCM24();
  }
  switch (data_type & 3) {
    case SOXR_FLOAT32: INTERLEAVE_TO(float, 1);
    case SOXR_FLOAT64: INTERLEAVE_TO(double, 0);

    case SOXR_INT32: if (ch == 1)
        return lsx_rint32_clip_f(dest0, src[0], n);
      return lsx_rint32_clip_2_f(dest0, src, ch, n);

    case SOXR_INT16: if (seed) {
      if (ch == 1)
        return lsx_rint16_clip_dither_f(dest0, src[0], n, seed);
      return lsx_rint16_clip_2_dither_f(dest0, src, ch, n, seed);
    }
    if (ch == 1)
        return lsx_rint16_clip_f(dest0, src[0], n);
      return lsx_rint16_clip_2_f(dest0, src, ch, n);
    default: break;
  }
  return 0;
}
#endif
