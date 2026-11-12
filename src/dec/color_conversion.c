// Copyright 2026 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the COPYING file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS. All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
// -----------------------------------------------------------------------------
//
// Explicit Y'CbCr matrix / range selection for lossy VP8 -> RGB conversion.
//
// With Kr/Kb denoting the matrix luma weights (Kg = 1 - Kr - Kb), the
// conversion is:
//   full range   : Y = y,          Cb = u - 128,           Cr = v - 128
//   limited range: Y = 255*(y-16)/219, Cb = 255*(u-128)/224, Cr = 255*(v-128)/224
//   R = Y + 2*(1-Kr)*Cr
//   B = Y + 2*(1-Kb)*Cb
//   G = Y - (2*(1-Kr)*Kr/Kg)*Cr - (2*(1-Kb)*Kb/Kg)*Cb
// Each channel is rounded to the nearest integer (halves up) and clipped to
// [0,255] only at the very end.
//
// The fixed-point implementation below scales Y and the chroma samples to a
// 16-bit fraction and rounds once at the end. It stays within +/- 1 of the
// double-precision formula above for every possible 8-bit (y,u,v) triple,
// which is inside the +/- 2 contract.

#include "src/dec/color_conversion.h"

#include "src/dsp/dsp.h"
#include "src/webp/decode.h"
#include "src/webp/types.h"

//------------------------------------------------------------------------------
// Matrices

typedef struct {
  int y_offset;  // 16 for limited range, 0 for full range
  int y_scale;   // Q16 scale applied to (y - y_offset)
  int c_scale;   // Q16 scale applied to (u/v - 128)
  int cr_r;      // Q16 coefficient of Cr in R
  int cb_b;      // Q16 coefficient of Cb in B
  int cr_g;      // Q16 coefficient of Cr subtracted from Y for G
  int cb_g;      // Q16 coefficient of Cb subtracted from Y for G
} WebPColorMatrix;

// 255 * 65536 / 219 and 255 * 65536 / 224, rounded.
#define COLOR_Y_SCALE_LIMITED 76309
#define COLOR_C_SCALE_LIMITED 74606
#define COLOR_SCALE_FULL 65536

// Q16 coefficients: 2*(1-Kr), 2*(1-Kb), 2*(1-Kr)*Kr/Kg, 2*(1-Kb)*Kb/Kg.
// BT.601: (Kr,Kb) = (0.299,0.114), Kg = 0.587
#define COLOR_601_CR_R 91881
#define COLOR_601_CB_B 116130
#define COLOR_601_CR_G 46802
#define COLOR_601_CB_G 22553
// BT.709: (Kr,Kb) = (0.2126,0.0722), Kg = 0.7152
#define COLOR_709_CR_R 103206
#define COLOR_709_CB_B 121609
#define COLOR_709_CR_G 30679
#define COLOR_709_CB_G 12276
// BT.2020: (Kr,Kb) = (0.2627,0.0593), Kg = 0.6780
#define COLOR_2020_CR_R 96639
#define COLOR_2020_CB_B 123299
#define COLOR_2020_CR_G 37444
#define COLOR_2020_CB_G 10784

#define LIMITED_MATRIX(CR_R, CB_B, CR_G, CB_G) \
  {16, COLOR_Y_SCALE_LIMITED, COLOR_C_SCALE_LIMITED, CR_R, CB_B, CR_G, CB_G}
#define FULL_MATRIX(CR_R, CB_B, CR_G, CB_G) \
  {0, COLOR_SCALE_FULL, COLOR_SCALE_FULL, CR_R, CB_B, CR_G, CB_G}

static const WebPColorMatrix k709Limited =
    LIMITED_MATRIX(COLOR_709_CR_R, COLOR_709_CB_B, COLOR_709_CR_G,
                   COLOR_709_CB_G);
static const WebPColorMatrix k2020Limited =
    LIMITED_MATRIX(COLOR_2020_CR_R, COLOR_2020_CB_B, COLOR_2020_CR_G,
                   COLOR_2020_CB_G);
static const WebPColorMatrix k601Full =
    FULL_MATRIX(COLOR_601_CR_R, COLOR_601_CB_B, COLOR_601_CR_G,
                COLOR_601_CB_G);
static const WebPColorMatrix k709Full =
    FULL_MATRIX(COLOR_709_CR_R, COLOR_709_CB_B, COLOR_709_CR_G,
                COLOR_709_CB_G);
static const WebPColorMatrix k2020Full =
    FULL_MATRIX(COLOR_2020_CR_R, COLOR_2020_CB_B, COLOR_2020_CR_G,
                COLOR_2020_CB_G);

//------------------------------------------------------------------------------
// Pixel conversion

// Rounds a Q16 value to the nearest integer, halves up, and clips to [0,255].
// Negative values clip to 0, so the "half up" tie rule is unambiguous here.
static WEBP_INLINE uint8_t ColorRoundClip(int64_t v) {
  if (v <= 0) return 0;  // no clipping needed below; keeps "half up" defined
  v = (v + 32768) >> 16;  // Q16 -> Q0, nearest integer, halves up.
  return (v > 255) ? 255 : (uint8_t)v;
}

static WEBP_INLINE void ColorConvert(const WebPColorMatrix* const m, int y,
                                     int u, int v, int* const r,
                                     int* const g, int* const b) {
  const int64_t yy = (int64_t)(y - m->y_offset) * m->y_scale;
  const int64_t cb = (int64_t)(u - 128) * m->c_scale;
  const int64_t cr = (int64_t)(v - 128) * m->c_scale;
  const int64_t rr = (int64_t)m->cr_r * cr >> 16;
  const int64_t bb = (int64_t)m->cb_b * cb >> 16;
  const int64_t gg = ((int64_t)m->cr_g * cr >> 16) +
                     ((int64_t)m->cb_g * cb >> 16);
  *r = ColorRoundClip(yy + rr);
  *g = ColorRoundClip(yy - gg);
  *b = ColorRoundClip(yy + bb);
}

static WEBP_INLINE void StoreRgba(uint8_t* const dst, int r, int g, int b) {
  dst[0] = (uint8_t)r;
  dst[1] = (uint8_t)g;
  dst[2] = (uint8_t)b;
  dst[3] = 0xff;
}

static WEBP_INLINE void StoreBgra(uint8_t* const dst, int r, int g, int b) {
  dst[0] = (uint8_t)b;
  dst[1] = (uint8_t)g;
  dst[2] = (uint8_t)r;
  dst[3] = 0xff;
}

#define COLOR_PIXEL_FUNC(NAME, MATRIX, STORE)                             \
  static void NAME(uint8_t y, uint8_t u, uint8_t v, uint8_t* const dst) { \
    int r, g, b;                                                          \
    ColorConvert(&MATRIX, y, u, v, &r, &g, &b);                           \
    STORE(dst, r, g, b);                                                  \
  }

COLOR_PIXEL_FUNC(ColorPixelRgba709L, k709Limited, StoreRgba)
COLOR_PIXEL_FUNC(ColorPixelRgba2020L, k2020Limited, StoreRgba)
COLOR_PIXEL_FUNC(ColorPixelRgba601F, k601Full, StoreRgba)
COLOR_PIXEL_FUNC(ColorPixelRgba709F, k709Full, StoreRgba)
COLOR_PIXEL_FUNC(ColorPixelRgba2020F, k2020Full, StoreRgba)
COLOR_PIXEL_FUNC(ColorPixelBgra709L, k709Limited, StoreBgra)
COLOR_PIXEL_FUNC(ColorPixelBgra2020L, k2020Limited, StoreBgra)
COLOR_PIXEL_FUNC(ColorPixelBgra601F, k601Full, StoreBgra)
COLOR_PIXEL_FUNC(ColorPixelBgra709F, k709Full, StoreBgra)
COLOR_PIXEL_FUNC(ColorPixelBgra2020F, k2020Full, StoreBgra)

#undef COLOR_PIXEL_FUNC

static const WebPColorPixelFunc kRgbaPixels[6] = {
    NULL,           ColorPixelRgba709L, ColorPixelRgba2020L,
    ColorPixelRgba601F, ColorPixelRgba709F, ColorPixelRgba2020F};
static const WebPColorPixelFunc kBgraPixels[6] = {
    NULL,           ColorPixelBgra709L, ColorPixelBgra2020L,
    ColorPixelBgra601F, ColorPixelBgra709F, ColorPixelBgra2020F};

//------------------------------------------------------------------------------
// Point-sampled rows (no fancy upsampling)

#define COLOR_ROW_FUNC(NAME, PIXEL)                                       \
  static void NAME(const uint8_t* WEBP_RESTRICT y,                        \
                   const uint8_t* WEBP_RESTRICT u,                        \
                   const uint8_t* WEBP_RESTRICT v,                        \
                   uint8_t* WEBP_RESTRICT dst, int len) {                 \
    int i;                                                                \
    for (i = 0; i < len; i += 2) {                                        \
      const int c = i >> 1;                                               \
      PIXEL(y[i], u[c], v[c], dst + 4 * i);                               \
      if (i + 1 < len) PIXEL(y[i + 1], u[c], v[c], dst + 4 * (i + 1));    \
    }                                                                     \
  }

COLOR_ROW_FUNC(ColorRowRgba709L, ColorPixelRgba709L)
COLOR_ROW_FUNC(ColorRowRgba2020L, ColorPixelRgba2020L)
COLOR_ROW_FUNC(ColorRowRgba601F, ColorPixelRgba601F)
COLOR_ROW_FUNC(ColorRowRgba709F, ColorPixelRgba709F)
COLOR_ROW_FUNC(ColorRowRgba2020F, ColorPixelRgba2020F)
COLOR_ROW_FUNC(ColorRowBgra709L, ColorPixelBgra709L)
COLOR_ROW_FUNC(ColorRowBgra2020L, ColorPixelBgra2020L)
COLOR_ROW_FUNC(ColorRowBgra601F, ColorPixelBgra601F)
COLOR_ROW_FUNC(ColorRowBgra709F, ColorPixelBgra709F)
COLOR_ROW_FUNC(ColorRowBgra2020F, ColorPixelBgra2020F)

#undef COLOR_ROW_FUNC

static const WebPSamplerRowFunc kRgbaRows[6] = {
    NULL,         ColorRowRgba709L, ColorRowRgba2020L,
    ColorRowRgba601F, ColorRowRgba709F, ColorRowRgba2020F};
static const WebPSamplerRowFunc kBgraRows[6] = {
    NULL,         ColorRowBgra709L, ColorRowBgra2020L,
    ColorRowBgra601F, ColorRowBgra709F, ColorRowBgra2020F};

//------------------------------------------------------------------------------
// YUV444 -> RGB (used by the scaling path)

#define COLOR_YUV444_FUNC(NAME, PIXEL)                                     \
  static void NAME(const uint8_t* WEBP_RESTRICT y,                         \
                   const uint8_t* WEBP_RESTRICT u,                         \
                   const uint8_t* WEBP_RESTRICT v,                         \
                   uint8_t* WEBP_RESTRICT dst, int len) {                  \
    int i;                                                                 \
    for (i = 0; i < len; ++i) PIXEL(y[i], u[i], v[i], dst + 4 * i);        \
  }

COLOR_YUV444_FUNC(ColorYuv444Rgba709L, ColorPixelRgba709L)
COLOR_YUV444_FUNC(ColorYuv444Rgba2020L, ColorPixelRgba2020L)
COLOR_YUV444_FUNC(ColorYuv444Rgba601F, ColorPixelRgba601F)
COLOR_YUV444_FUNC(ColorYuv444Rgba709F, ColorPixelRgba709F)
COLOR_YUV444_FUNC(ColorYuv444Rgba2020F, ColorPixelRgba2020F)
COLOR_YUV444_FUNC(ColorYuv444Bgra709L, ColorPixelBgra709L)
COLOR_YUV444_FUNC(ColorYuv444Bgra2020L, ColorPixelBgra2020L)
COLOR_YUV444_FUNC(ColorYuv444Bgra601F, ColorPixelBgra601F)
COLOR_YUV444_FUNC(ColorYuv444Bgra709F, ColorPixelBgra709F)
COLOR_YUV444_FUNC(ColorYuv444Bgra2020F, ColorPixelBgra2020F)

#undef COLOR_YUV444_FUNC

static const WebPYUV444Converter kRgbaYuv444[6] = {
    NULL,             ColorYuv444Rgba709L, ColorYuv444Rgba2020L,
    ColorYuv444Rgba601F, ColorYuv444Rgba709F, ColorYuv444Rgba2020F};
static const WebPYUV444Converter kBgraYuv444[6] = {
    NULL,             ColorYuv444Bgra709L, ColorYuv444Bgra2020L,
    ColorYuv444Bgra601F, ColorYuv444Bgra709F, ColorYuv444Bgra2020F};

//------------------------------------------------------------------------------
// Fancy upsampling (same interpolation as the built-in upsampler)

#ifdef FANCY_UPSAMPLING
static void ColorUpsampleLinePair(
    const uint8_t* WEBP_RESTRICT top_y,
    const uint8_t* WEBP_RESTRICT bottom_y,
    const uint8_t* WEBP_RESTRICT top_u, const uint8_t* WEBP_RESTRICT top_v,
    const uint8_t* WEBP_RESTRICT cur_u, const uint8_t* WEBP_RESTRICT cur_v,
    uint8_t* WEBP_RESTRICT top_dst, uint8_t* WEBP_RESTRICT bottom_dst, int len,
    WebPColorPixelFunc convert) {
  const int last_pixel_pair = (len - 1) >> 1;
  // U and V are packed as 16 bits each, exactly like in upsampling.c, so the
  // chroma interpolation math (phase, edge extension, rounding) is identical.
#define LOAD_UV(u, v) ((uint32_t)(u) | ((uint32_t)(v) << 16))
  uint32_t tl_uv = LOAD_UV(top_u[0], top_v[0]);
  uint32_t l_uv = LOAD_UV(cur_u[0], cur_v[0]);
  int x;
  {
    const uint32_t uv0 = (3 * tl_uv + l_uv + 0x00020002u) >> 2;
    convert(top_y[0], uv0 & 0xff, uv0 >> 16, top_dst);
  }
  if (bottom_y != NULL) {
    const uint32_t uv0 = (3 * l_uv + tl_uv + 0x00020002u) >> 2;
    convert(bottom_y[0], uv0 & 0xff, uv0 >> 16, bottom_dst);
  }
  for (x = 1; x <= last_pixel_pair; ++x) {
    const uint32_t t_uv = LOAD_UV(top_u[x], top_v[x]);
    const uint32_t uv = LOAD_UV(cur_u[x], cur_v[x]);
    const uint32_t avg = tl_uv + t_uv + l_uv + uv + 0x00080008u;
    const uint32_t diag_12 = (avg + 2 * (t_uv + l_uv)) >> 3;
    const uint32_t diag_03 = (avg + 2 * (tl_uv + uv)) >> 3;
    {
      const uint32_t uv0 = (diag_12 + tl_uv) >> 1;
      const uint32_t uv1 = (diag_03 + t_uv) >> 1;
      convert(top_y[2 * x - 1], uv0 & 0xff, uv0 >> 16,
              top_dst + (2 * x - 1) * 4);
      convert(top_y[2 * x - 0], uv1 & 0xff, uv1 >> 16,
              top_dst + (2 * x - 0) * 4);
    }
    if (bottom_y != NULL) {
      const uint32_t uv0 = (diag_03 + l_uv) >> 1;
      const uint32_t uv1 = (diag_12 + uv) >> 1;
      convert(bottom_y[2 * x - 1], uv0 & 0xff, uv0 >> 16,
              bottom_dst + (2 * x - 1) * 4);
      convert(bottom_y[2 * x + 0], uv1 & 0xff, uv1 >> 16,
              bottom_dst + (2 * x + 0) * 4);
    }
    tl_uv = t_uv;
    l_uv = uv;
  }
  if (!(len & 1)) {
    {
      const uint32_t uv0 = (3 * tl_uv + l_uv + 0x00020002u) >> 2;
      convert(top_y[len - 1], uv0 & 0xff, uv0 >> 16, top_dst + (len - 1) * 4);
    }
    if (bottom_y != NULL) {
      const uint32_t uv0 = (3 * l_uv + tl_uv + 0x00020002u) >> 2;
      convert(bottom_y[len - 1], uv0 & 0xff, uv0 >> 16,
              bottom_dst + (len - 1) * 4);
    }
  }
#undef LOAD_UV
}
#endif  // FANCY_UPSAMPLING

//------------------------------------------------------------------------------
// Public dispatch

int WebPIsSupportedColorConversionOutput(WEBP_CSP_MODE mode) {
  return mode == MODE_RGBA || mode == MODE_BGRA || mode == MODE_rgbA ||
         mode == MODE_bgrA;
}

static int IsRgbaOrder(WEBP_CSP_MODE mode) {
  return mode == MODE_RGBA || mode == MODE_rgbA;
}

static int IsBgraOrder(WEBP_CSP_MODE mode) {
  return mode == MODE_BGRA || mode == MODE_bgrA;
}

VP8StatusCode WebPValidateColorConversionOptions(
    const WebPDecoderOptions* const options, int is_lossless,
    WEBP_CSP_MODE output_mode) {
  const int conv = (options == NULL) ? 0 : options->yuv_conversion;
  if (conv < 0 || conv > 5) {
    return VP8_STATUS_INVALID_PARAM;  // rejected for any input and output.
  }
  if (conv == 0 || is_lossless) {
    return VP8_STATUS_OK;  // legacy behavior, or lossless VP8L (ignored).
  }
  if (!WebPIsRGBMode(output_mode)) {
    return VP8_STATUS_OK;  // YUV/YUVA output is untouched.
  }
  return WebPIsSupportedColorConversionOutput(output_mode)
             ? VP8_STATUS_OK
             : VP8_STATUS_INVALID_PARAM;
}

WebPSamplerRowFunc WebPGetColorSamplerRow(WEBP_CSP_MODE mode, int conv) {
  if (conv < 1 || conv > 5) return NULL;
  if (IsRgbaOrder(mode)) return kRgbaRows[conv];
  if (IsBgraOrder(mode)) return kBgraRows[conv];
  return NULL;
}

WebPYUV444Converter WebPGetColorYuv444Converter(WEBP_CSP_MODE mode, int conv) {
  if (conv < 1 || conv > 5) return NULL;
  if (IsRgbaOrder(mode)) return kRgbaYuv444[conv];
  if (IsBgraOrder(mode)) return kBgraYuv444[conv];
  return NULL;
}

WebPColorPixelFunc WebPGetColorPixelFunc(WEBP_CSP_MODE mode, int conv) {
  if (conv < 1 || conv > 5) return NULL;
  if (IsRgbaOrder(mode)) return kRgbaPixels[conv];
  if (IsBgraOrder(mode)) return kBgraPixels[conv];
  return NULL;
}

WebPColorLinePairFunc WebPGetColorLinePair(WEBP_CSP_MODE mode) {
  if (!WebPIsSupportedColorConversionOutput(mode)) return NULL;
#ifdef FANCY_UPSAMPLING
  (void)mode;
  return ColorUpsampleLinePair;
#else
  (void)mode;
  return NULL;
#endif
}
